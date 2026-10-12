/* What every window backend shares: the interrupt-style clock and the
 * scripted test input. The 1 kHz SIGALRM stands in for the console's
 * interrupts and must fire on the main thread, between instructions of the
 * game (busy-waits poll what the handlers update); backends block signals
 * on every thread they create. Windows interrupts the main thread from a
 * timer thread instead (win32.c). */
#define _GNU_SOURCE
#include "pc/compat/fs.h"
#include "platform.h"
#include "pc/guest/state.h"
#include "pc/debug/log.h"
#include "pc/debug/crash.h"
#include "pc/debug/monitor.h"
#include "pc/debug/crash_test.h"
#include "pc/sdk/display.h"
#include "pc/debug/profile.h"
#include "pc/compat/signal.h"
#include "pc/audio/spu.h"
#include "controls_runtime.h"
#include "settings.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>
#ifdef _WIN32
#include "win32.h"
#elif !defined(__SWITCH__) /* no signals there: the clock is cooperative */
#include <sys/syscall.h>
#include "signal_context.h"
#endif

static volatile unsigned vblank_count;
static void (*vblank_handler)(void);
static void (*tick_handler)(uint64_t, uint64_t);
static volatile int rate = 100;
static uint64_t real_prev, virtual_now, next_vblank;
static unsigned virtual_remainder; /* hundredths of a microsecond a rate other than 100% left over */
static volatile unsigned vblank_period = 16683;
static volatile int step_pending;
static float present_refresh;
static int present_cap;           /* frames per second; 0 display refresh; -1 every frame */
static uint64_t present_next_us;  /* the present pacer's next slot */
static int vsync_on;              /* the backend's presents wait for the display (Platform_SetVSync) */
static uint64_t last_vsync_real;
/* When the real-time clock's last VBlank was due, and when it came (Platform_NotifyPresent). */
static uint64_t vblank_due_at, vblank_came_at;
static unsigned watchdog_seconds = 5;
static volatile int watchdog_reported;
/* The virtual clock. MEMORIES_DETERMINISTIC=1 asks for it, headless or in a
 * window, for as long as the game runs; headless, uncapped and dumping a
 * frame (MEMORIES_HEADLESS + MEMORIES_DUMP_FRAME + MEMORIES_SPEED=-1, what
 * the smoke tests have always set) gets it too. The run must come out the
 * same on every host. Virtual time then moves only in
 * Platform_WaitVBlank, 1 ms at a time, so the disc, the root counter and the
 * VBlank see the same time between the same two game frames whatever the
 * host's speed or its timer. Driven by the host timer, a frame that took
 * longer to compute got more ticks, so more sectors, and a load finished a
 * frame or ten earlier on one host than another (the random seed, which the
 * name entry screen draws on every frame, then parted company). A VSync
 * that only reads the count steps it too (Platform_PollTime). In a window
 * the frames are then shown at the game speed (pace(), which only sleeps),
 * and pause and frame step work; headless runs flat out. */
static int deterministic_dump, deterministic_asked, deterministic_paced;
static volatile uint64_t deterministic_last_wait; /* real time the game last waited for a VBlank */
static uint64_t pace_next;                         /* real time the next paced VBlank is due */
static int controlled;                             /* a control client decides (Platform_ControlAttach) */

static int virtual_clock(void)
{
    return deterministic_asked || (deterministic_dump && rate == -1);
}
#define DETERMINISTIC_STEP 1000
#define DETERMINISTIC_SPIN 1000000 /* real us outside a wait before the timer steps a spinning game */

static uint64_t now_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

static void deliver_vblank(void)
{
    vblank_count++;
    if (vblank_handler) vblank_handler();
}

/* The cooperative clock, the default: no interrupt. The clock's time is taken, and
 * the ticks and VBlanks it owes are run, where the game calls in to wait or
 * to read the time (VSync, Platform_WaitVBlank, Platform_PollTime), so the
 * game's interrupt code never runs in the middle of anything. Every loop the
 * game polls the clock in passes through one of those (notes/pc-build.md,
 * "Cooperative clock"). MEMORIES_CLOCK=interrupt brings back the timer that
 * interrupts the game, as the console's VBlank would; the sampling profiler
 * (MEMORIES_PROFILE) samples from that timer, so it chooses it too. */
static int cooperative;
/* VBlanks the clock may still deliver, -1 for no limit (Platform_LimitVBlanks). */
static volatile int vblank_budget = -1;

static void advance(uint64_t real_now, uintptr_t eip)
{
    uint64_t elapsed = real_prev ? real_now - real_prev : 0;
    real_prev = real_now;
    /* A longer gap is a stall (a breakpoint, a window being dragged), not
     * time the game should catch up. Serviced cooperatively, a heavy frame
     * can legitimately run past 100 ms between two services. */
    if (elapsed > (cooperative ? 500000u : 100000u)) elapsed = 0;
    if (virtual_clock()) {
        /* Time passes in Platform_WaitVBlank. Only a game that has spun for
         * a second without waiting for a VBlank gets steps from the timer,
         * so that it cannot hang; no loop the game runs does that today, and
         * no frame takes that long to compute. Such a step is reported, with
         * where the game was: a loop that polls the clock without a wait. */
        if (deterministic_last_wait && real_now - deterministic_last_wait > DETERMINISTIC_SPIN) {
            static uintptr_t reported[16];
            static unsigned reported_count, steps;
            unsigned i;
            virtual_now += DETERMINISTIC_STEP;
            if (tick_handler) tick_handler(virtual_now, virtual_now);
            steps++;
            for (i = 0; i < reported_count && reported[i] != eip; i++) {}
            if (i == reported_count && reported_count < 16) {
                reported[reported_count++] = eip;
                /* The timer's handler: Log_Signal, never stdio or LOG (log.c). */
                Log_Signal(LOG_FRAMES, "clock: the game spun a second without a wait, at 0x%lx (%ld steps so far)",
                           (long)eip, (long)steps, 0, 0, 0, 0);
            }
        }
        return;
    }
    /* The fraction is carried: a loop that polls the clock sees a
     * microsecond or two per call, and at 50% each such microsecond came
     * out as nothing, so a game waiting on the clock that way (the boot,
     * the movies) stood still for seconds at a time. */
    if (rate > 0) {
        uint64_t scaled = elapsed * (uint64_t)rate + virtual_remainder;
        virtual_now += scaled / 100;
        virtual_remainder = (unsigned)(scaled % 100);
    }
    if (rate == -1) virtual_now += elapsed;
    if (tick_handler) tick_handler(virtual_now, real_now);
    if (!next_vblank) next_vblank = virtual_now;
    while (virtual_now >= next_vblank && vblank_budget != 0) {
        vblank_due_at = next_vblank;
        next_vblank += vblank_period;
        if (virtual_now > next_vblank + 4 * vblank_period) next_vblank = virtual_now;
        vblank_came_at = virtual_now;
        deliver_vblank();
        if (vblank_budget > 0) vblank_budget--;
    }
    if (step_pending && vblank_budget != 0) {
        step_pending = 0;
        vblank_due_at = vblank_came_at = virtual_now;
        deliver_vblank();
        if (vblank_budget > 0) vblank_budget--;
    }
}

/* The cooperative clock's service point: the time now, and whatever it owes. */
static int servicing; /* inside advance(): the handlers it runs are the game's interrupt code */
static void service(void)
{
    sigset_t set, previous;
    if (!cooperative) return;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, &previous);
    servicing++;
    advance(now_us(), (uintptr_t)__builtin_return_address(0));
    servicing--;
    sigprocmask(SIG_SETMASK, &previous, NULL);
}

static void on_tick(uintptr_t eip, void *context)
{
    uint64_t real_now = now_us();
    while (CrashTest_TickHang) {
    }
    Profile_Sample(eip);
    {
        /* MEMORIES_TRACE=frames: where the game is when it has run 30 ms
         * past its last VSync, once per such stretch. */
        static uint64_t reported_stretch;
        if (last_vsync_real && real_now - last_vsync_real > 30000 && reported_stretch != last_vsync_real &&
            Log_Wanted(LOG_FRAMES)) {
            reported_stretch = last_vsync_real;
            Log_Signal(LOG_FRAMES, "long stretch without a VSync: %ld us so far, at 0x%lx",
                       (long)(real_now - last_vsync_real), (long)eip, 0, 0, 0, 0);
        }
    }
#ifndef _WIN32 /* Windows watches from the clock thread (Win32_SetStallReporter) */
    if (watchdog_seconds && rate != 0 && !watchdog_reported &&
        real_now - last_vsync_real >= (uint64_t)watchdog_seconds * 1000000u) {
        watchdog_reported = 1;
        Crash_ReportHang(context);
    }
#else
    (void)context;
#endif
    advance(real_now, eip);
}

#if !defined(_WIN32) && !defined(__SWITCH__)
static void on_alarm(int number, siginfo_t *info, void *context)
{
    ucontext_t *user = context;
    (void)number;
    (void)info;
    on_tick((uintptr_t)SIGNAL_CONTEXT_PC(user), context);
}
#endif

/* The 1 kHz signal is aimed at the main thread itself (SIGEV_THREAD_ID), not
 * the process: a process-directed signal lands on any thread that does not
 * block it, and graphics drivers start threads of their own after the
 * backend has finished creating its own with the signal masked. With the
 * game's interrupt code running on a driver thread the main thread stalls
 * on that driver's locks. The process-wide timer is only a fallback. */
int Platform_StartTimers(void (*tick)(uint64_t, uint64_t), void (*vblank)(void))
{
#if !defined(_WIN32) && !defined(__SWITCH__)
    struct sigaction action;
 #ifndef __APPLE__
    struct sigevent event;
    struct itimerspec spec;
    timer_t timer;
 #endif
#endif
    tick_handler = tick;
    vblank_handler = vblank;
    /* The virtual clock (above). The frame-dump combination is kept as it
     * was; MEMORIES_DETERMINISTIC=1 alone needs neither a dump nor speed -1. */
    deterministic_dump = (getenv("MEMORIES_HEADLESS") != NULL || getenv("MEMORIES_DETERMINISTIC") != NULL) &&
                         getenv("MEMORIES_DUMP_FRAME") != NULL;
    {
        const char *asked = getenv("MEMORIES_DETERMINISTIC");
        deterministic_asked = asked && *asked && strcmp(asked, "0") != 0;
        deterministic_paced = deterministic_asked && getenv("MEMORIES_HEADLESS") == NULL;
    }
    last_vsync_real = now_us();
    {
        const char *watchdog = getenv("MEMORIES_WATCHDOG");
        if (watchdog && *watchdog) watchdog_seconds = (unsigned)strtoul(watchdog, NULL, 10);
    }
    Profile_Init();
    {
        const char *clock = getenv("MEMORIES_CLOCK"), *profile = getenv("MEMORIES_PROFILE");
        if (clock && *clock) cooperative = strcmp(clock, "interrupt") != 0;
        else cooperative = !(profile && *profile);
#if defined(_WIN32) && defined(__x86_64__)
        if (!cooperative) {
            fprintf(stderr, "memories-pc: the 64-bit build has only the cooperative clock yet\n");
            cooperative = 1;
        }
#elif defined(__SWITCH__)
        cooperative = 1; /* Horizon delivers no SIGALRM */
#endif
        if (!cooperative) {
            fprintf(stderr, "memories-pc: interrupt clock%s\n",
                    clock && *clock ? " (MEMORIES_CLOCK=interrupt)" : ", which MEMORIES_PROFILE samples from");
        }
    }
#ifdef _WIN32
    Win32_SetStallReporter(Crash_ReportHang, watchdog_seconds);
    if (cooperative) return Win32_StartWatch(); /* the clock thread only watches for a stall */
    return Win32_StartInterrupt(on_tick);
#elif defined(__SWITCH__)
    return 0;
#else
    if (cooperative) return 0; /* the crash monitor process watches for a stall */
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_alarm;
    action.sa_flags = SA_RESTART | SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL)) {
        return -1;
    }
 #ifndef __APPLE__
    memset(&event, 0, sizeof(event));
    event.sigev_notify = SIGEV_THREAD_ID;
    event.sigev_signo = SIGALRM;
    event._sigev_un._tid = (pid_t)syscall(SYS_gettid);
    spec.it_interval.tv_sec = spec.it_value.tv_sec = 0;
    spec.it_interval.tv_nsec = spec.it_value.tv_nsec = 1000000;
    if (timer_create(CLOCK_MONOTONIC, &event, &timer) == 0 && timer_settime(timer, 0, &spec, NULL) == 0) {
        return 0;
    }
 #endif
    {
        struct itimerval fallback;
        fallback.it_interval.tv_sec = fallback.it_value.tv_sec = 0;
        fallback.it_interval.tv_usec = fallback.it_value.tv_usec = 1000;
        return setitimer(ITIMER_REAL, &fallback, NULL) ? -1 : 0;
    }
#endif
}

unsigned Platform_VBlankCount(void)
{
    return vblank_count;
}

void Platform_SetClockRate(int percent)
{
    if (percent < -1) percent = -1;
    if (percent > 400) percent = 400;
    if (percent > 0 && percent < 25) percent = 25;
    rate = percent;
    Monitor_Shared()->paused = percent == 0; /* no VSync is expected: not a freeze */
}

int Platform_ClockRate(void) { return rate; }

#if !defined(__ANDROID__) && !defined(__SWITCH__) /* android.c, switch.c: GLES only */
int Platform_HasDesktopGL(void) { return 1; }
int Platform_GuestMemoryHelp(char *why, size_t size)
{
    (void)why;
    (void)size;
    return 0;
}
#endif
void Platform_StepFrame(void) { step_pending = 1; }

void Platform_ControlAttach(int attached)
{
    static unsigned watchdog_before;
    if (!attached == !controlled) return;
    controlled = attached;
    if (attached) {
        watchdog_before = watchdog_seconds;
        watchdog_seconds = 0;
        if (!cooperative) {
            fprintf(stderr, "memories-pc: control: the interrupt clock (MEMORIES_CLOCK=interrupt) keeps running "
                            "while the client holds the game; lockstep needs the default clock\n");
        }
    } else {
        watchdog_seconds = watchdog_before;
    }
#ifdef _WIN32
    Win32_SetStallReporter(Crash_ReportHang, watchdog_seconds);
#endif
    Monitor_Modal(attached); /* the crash monitor's freeze check */
}

int Platform_VirtualClock(void)
{
    return virtual_clock();
}

void Platform_ControlIdle(void)
{
    /* Waiting on purpose, not frozen: every watch sees a heartbeat. */
    __atomic_add_fetch(&Monitor_Shared()->heartbeat, 1, __ATOMIC_RELEASE);
    last_vsync_real = now_us();
    /* A present meanwhile (the wait's still) services the clock: the virtual
     * one must not take the wait for a spin and step. */
    if (deterministic_last_wait) deterministic_last_wait = last_vsync_real;
#ifdef _WIN32
    Win32_Heartbeat();
#endif
}

void Platform_ControlHold(int held)
{
    sigset_t set, previous;
    uint64_t now;
    if (held) return;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, &previous);
    now = now_us();
    real_prev = now;                /* the real-time clock does not catch up the wait */
    last_vsync_real = now;
    if (deterministic_last_wait) deterministic_last_wait = now; /* nor does the virtual one take it for a spin */
    pace_next = 0;
    sigprocmask(SIG_SETMASK, &previous, NULL);
}

float Platform_GameHz(void)
{
    return rate > 0 ? 1000000.0f / (float)vblank_period * (float)rate / 100.0f : 0.0f;
}

void Platform_SetPresentCap(int fps)
{
    present_cap = fps < -1 ? -1 : fps;
    present_next_us = 0;
}

int Platform_PresentCap(void) { return present_cap; }

unsigned Platform_PresentPeriodUs(void)
{
    if (present_cap == -1) return 0;
    if (present_cap > 0) return (unsigned)(1000000.0f / (float)present_cap + 0.5f);
    return present_refresh > 0.0f ? (unsigned)(1000000.0f / present_refresh + 0.5f) : 0u;
}

/* One present per period, on a fixed grid so a game frame that arrives a
 * little before its slot (frames come from a 1 kHz clock, and the game rate
 * may sit within a few microseconds of the cap) still takes it: a frame is
 * due from half a period before its slot. A stall resets the grid. */
static int vsync_paces_clock(void);

int Platform_PresentDue(void)
{
    unsigned period = Platform_PresentPeriodUs();
    uint64_t now = now_us();
    if (!period) return 1;
    /* The swap paces the game to the display (Platform_NotifyPresent), a
     * frame each refresh: a cap at the refresh or above would only drop the
     * frames that run a 64th of a refresh early while a driver's queue fills,
     * so its swaps never come to wait and the game runs that much fast. */
    if (vsync_paces_clock() && period <= (unsigned)(1010000.0f / present_refresh)) {
        present_next_us = 0;
        return 1;
    }
    if (!present_next_us || now >= present_next_us + period) present_next_us = now;
    if (now + period / 2 < present_next_us) return 0;
    present_next_us += period;
    return 1;
}

int Platform_VSyncPacesGame(void)
{
    if (rate == -1) return 0;
    if (present_refresh <= 0.0f) return rate <= 100; /* every display refreshes at 59.94 Hz or faster */
    return Platform_GameHz() <= present_refresh + 0.5f;
}

void Platform_VSyncHeartbeat(void)
{
    sigset_t set, previous;
    MonitorShared *monitor = Monitor_Shared();
    service();
    monitor->frame = Memories_PresentedFrames();
    monitor->vblank = vblank_count;
    monitor->running = 1;
    __atomic_add_fetch(&monitor->heartbeat, 1, __ATOMIC_RELEASE);
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, &previous);
    last_vsync_real = now_us();
    watchdog_reported = 0;
#ifdef _WIN32
    Win32_Heartbeat();
#endif
    sigprocmask(SIG_SETMASK, &previous, NULL);
#ifdef _WIN32
    /* A VSync(-1) polling loop (the movie waiting for sectors) spends most
     * of its time reading the clock, outside the executable, where the clock
     * thread only leaves the tick pending. Take it here, as the waits do. */
    Win32_ServiceInterrupt();
#endif
    CrashTest_Frame();
}

void Platform_SetVBlankPeriod(unsigned us)
{
    sigset_t set, previous;
    if (!us) return;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, &previous);
    vblank_period = us;
    sigprocmask(SIG_SETMASK, &previous, NULL);
}

void Platform_SetPresentRefresh(float hz)
{
    present_refresh = hz;
    present_next_us = 0;
}

float Platform_PresentRefresh(void) { return present_refresh; }

/* A vsynced present at 100% on a display within a hertz of 60 paces the
 * game. A swap that held the game until the display took the frame (or
 * until the driver's queue had room for it) has just returned at the
 * display's refresh, so the game's VBlank is due now, and the next one a
 * refresh on: the game then has the whole refresh to compute and draw its
 * next frame before the next swap holds it again. The VBlank used to be put
 * a period less 1.5 ms after every swap, which fits only a frame computed and
 * drawn in 1.5 ms: since the present comes before the wait (in VSync(0)), the
 * time past that went onto every frame, and 6 ms of game and present
 * (widescreen, the CRT pass and a model mod on an older PC) made 45 fps of a
 * 60 Hz display. A VBlank comes no sooner than a 64th of a refresh before a
 * refresh since the last one was due: a swap that does not block (a driver
 * queueing ahead, a hidden window) runs the game at most that much faster
 * than the display, which is what fills such a queue until its swaps do
 * block. A VBlank that came at this present's own service is the one the
 * swap held for, and the next goes a refresh (less that 64th) from it.
 * Presents that are not the game's (the menu repainted during a frame) come
 * before that, and leave the VBlank where it was. */
/* Never under the virtual clock: the display's phase is not the game's. */
static int vsync_paces_clock(void)
{
    return vsync_on && rate == 100 && present_refresh >= 59.0f && present_refresh <= 61.0f && !virtual_clock();
}

void Platform_SetVSync(int on)
{
    vsync_on = on;
    present_next_us = 0;
}

void Platform_NotifyPresent(uint64_t real_now_us, int vsynced)
{
    (void)real_now_us;
    service(); /* the cooperative clock's time, as of after the present */
    if (vsynced && vsync_paces_clock()) {
        sigset_t set, previous;
        unsigned period = (unsigned)(1000000.0f / present_refresh + 0.5f);
        uint64_t due;
        sigemptyset(&set);
        sigaddset(&set, SIGALRM);
        sigprocmask(SIG_BLOCK, &set, &previous);
        vblank_period = period;
        due = (vblank_came_at == virtual_now ? virtual_now : vblank_due_at) + period - period / 64;
        next_vblank = due > virtual_now ? due : virtual_now;
        sigprocmask(SIG_SETMASK, &previous, NULL);
        service(); /* a VBlank due now ends the VSync(0) this present came from */
    }
}

void Platform_StopTimers(void)
{
#ifdef _WIN32
    Win32_StopInterrupt();
#else
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, NULL);
#endif
}

/* The virtual clock in a window (MEMORIES_DETERMINISTIC=1 without
 * MEMORIES_HEADLESS), before a VBlank: paused, nothing moves until the pause
 * ends or a frame step lets one VBlank through; running, the VBlank waits
 * for its real time at the game speed. It only sleeps and pumps the
 * window's events, so the game sees the same as uncapped. */
static void pace(void)
{
    struct timespec nap = {0, 500000};
    uint64_t now;
    if (!deterministic_paced || controlled) return;
    while (rate == 0 && !step_pending && !Platform_ShouldQuit()) {
        deterministic_last_wait = now_us(); /* waiting, not spinning (advance) */
        Platform_PumpEvents();
#ifdef _WIN32
        Win32_Heartbeat(); /* paused, not hung */
#endif
        nanosleep(&nap, NULL);
    }
    step_pending = 0;
    if (rate <= 0) {
        pace_next = 0;
        return;
    }
    now = now_us();
    if (!pace_next || now > pace_next + 100000u) pace_next = now; /* a stall: start the grid again */
    while (now < pace_next) {
        struct timespec rest = {0, (long)(pace_next - now) * 1000};
        nanosleep(&rest, NULL);
        now = now_us();
    }
    pace_next += (uint64_t)vblank_period * 100u / (unsigned)rate;
}

void Platform_PollTime(void)
{
    sigset_t set, previous;
    if (!virtual_clock()) {
        service();
        return;
    }
    if (next_vblank && virtual_now + DETERMINISTIC_STEP >= next_vblank) pace();
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, &previous);
    if (!next_vblank) next_vblank = virtual_now + vblank_period;
    virtual_now += DETERMINISTIC_STEP;
    if (tick_handler) tick_handler(virtual_now, virtual_now);
    if (virtual_now >= next_vblank) {
        next_vblank += vblank_period;
        deliver_vblank();
    }
    deterministic_last_wait = now_us();
    sigprocmask(SIG_SETMASK, &previous, NULL);
}

/* DrawSync's service point (libgpu.c). The console takes its VBlank
 * interrupts while the game computes and draws a frame, so when a frame runs
 * past a VBlank, Graphics_SyncFrame's read of the game's VBlank counter
 * (D_8009B0C8) already counts it: that frame gets D_8009B0D8 = 2, then
 * VSync(0) returns at the next VBlank with the counter at 0. The cooperative
 * clock took those VBlanks only at VSync(0)'s entry, after Graphics_SyncFrame
 * had read the counter and set it to -1, so a slow frame left the counter at
 * 1 or more when Input_UpdatePads ran. Input_UpdatePads takes that as a
 * VBlank that came in after VSync and publishes the frame's newly pressed and
 * repeat bits once more on the next frame: one tap of Down moved the title
 * menu's cursor two places (every screen, every input device). Graphics_SyncFrame
 * calls DrawSync(0) just before its read, after the frame's drawing is
 * rasterized, so the VBlanks the frame overran are delivered here, where the
 * console would have delivered them. */
void Platform_ServiceClock(void)
{
    if (servicing) return; /* DrawSync from the game's own interrupt code */
    service();
}

/* VSync(0) on the console returns at the first VBlank after its entry, with
 * the game's VBlank callback run once; later ones interrupt the next frame,
 * whose Graphics_SyncFrame counts them. A present or a host hiccup that runs
 * past two VBlank periods inside VSync(0) had the clock deliver them all
 * there, in one go, after Graphics_SyncFrame had set the game's counter to
 * -1: Input_UpdatePads then found it at 1 or more, took the frame as late
 * and published its presses twice (see Platform_ServiceClock). VSync(0)
 * limits the clock to one VBlank until it returns; those still owed come at
 * the next service point, normally the next frame's DrawSync. */
void Platform_LimitVBlanks(int count)
{
    vblank_budget = count;
}

void Platform_WaitVBlank(unsigned count_at_entry)
{
    struct timespec nap = {0, 500000};
    /* Spent, and still no VBlank since the caller's count: a wait nested in
     * VSync(0) after its VBlank, which the limit must not hold forever. */
    if (vblank_budget == 0 && vblank_count == count_at_entry) vblank_budget = -1;
    while (vblank_count == count_at_entry && !Platform_ShouldQuit()) {
        if (virtual_clock()) {
            sigset_t set, previous;
            pace();
            if (Platform_ShouldQuit()) break;
            sigemptyset(&set);
            sigaddset(&set, SIGALRM);
            sigprocmask(SIG_BLOCK, &set, &previous);
            if (!next_vblank) next_vblank = virtual_now + vblank_period;
            while (virtual_now < next_vblank) {
                virtual_now += DETERMINISTIC_STEP;
                if (tick_handler) tick_handler(virtual_now, virtual_now);
            }
            next_vblank += vblank_period;
            deliver_vblank();
            deterministic_last_wait = now_us();
            sigprocmask(SIG_SETMASK, &previous, NULL);
#ifdef _WIN32
            Win32_ServiceInterrupt();
#endif
            continue;
        }
        if (rate == -1) {
            sigset_t set, previous;
            uint64_t real_now;
            sigemptyset(&set);
            sigaddset(&set, SIGALRM);
            sigprocmask(SIG_BLOCK, &set, &previous);
            real_now = now_us();
            advance(real_now, 0);
            if (vblank_count == count_at_entry) {
                if (!next_vblank) next_vblank = virtual_now;
                virtual_now = next_vblank;
                advance(real_now, 0);
            }
            sigprocmask(SIG_SETMASK, &previous, NULL);
        } else {
            service();
            if (vblank_count != count_at_entry) break;
            if (rate == 0) Platform_PumpEvents();
#ifdef _WIN32
            if (rate == 0) Win32_Heartbeat(); /* paused, not hung */
            Win32_ServiceInterrupt();
            if (vblank_count != count_at_entry) break;
#endif
            nanosleep(&nap, NULL);
        }
    }
}

/* The VBlank count is what the game sees through VSync(-1). The clock's
 * phase goes in a chunk of its own ("platform-clock", so that states
 * without it keep loading "platform"): how far off the next VBlank is, in
 * the clock's microseconds. Under the virtual clock the ticks between two VBlanks are the
 * 1 ms steps up to it, 16 or 17 as the phase falls, and each tick reads disc
 * sectors and runs the sound driver; a load that kept the loading process's
 * phase gave a frame a tick more or less than the game that saved it had,
 * a file finished loading a frame apart and the campaign map parted company
 * from it some 350 frames on. */
void Platform_State(MemoriesState *state)
{
    const MemoriesStateField fields[] = {{(void *)&vblank_count, sizeof(vblank_count)}};
    int64_t phase = next_vblank ? (int64_t)(next_vblank - virtual_now) : 0;
    MemoriesStateField clock = {&phase, sizeof(phase)};
    Memories_StateChunk(state, "platform", fields, 1);
    if (Memories_StateChunk(state, "platform-clock", &clock, 1) && phase > 0 &&
        phase <= 4 * (int64_t)vblank_period) {
        next_vblank = virtual_now + (uint64_t)phase;
    }
}

/* MEMORIES_INPUT="600:0008,610:0000": hex pad bits applied from a frame on;
 * MEMORIES_INPUT2 the same for the second pad, which then counts as
 * connected. Returns the bits in force at `frame`. */
static uint16_t scripted_bits(int port, unsigned frame)
{
    static const char *script[2];
    static int loaded[2];
    static uint16_t bits[2];
    if (!loaded[port]) {
        loaded[port] = 1;
        script[port] = getenv(port ? "MEMORIES_INPUT2" : "MEMORIES_INPUT");
    }
    while (script[port] && *script[port]) {
        char *end;
        unsigned long at = strtoul(script[port], &end, 10);
        if (*end != ':' || at > frame) {
            break;
        }
        bits[port] = (uint16_t)strtoul(end + 1, &end, 16);
        LOG(LOG_INPUT, "script frame %u pad %d %04x", frame, port + 1, bits[port]);
        script[port] = *end == ',' ? end + 1 : end;
    }
    return bits[port];
}

uint16_t Platform_ScriptedBits(unsigned frame)
{
    return scripted_bits(0, frame);
}

uint16_t Platform_ScriptedBits2(unsigned frame)
{
    return scripted_bits(1, frame);
}

int Platform_ScriptedPad2(void)
{
    const char *script = getenv("MEMORIES_INPUT2");
    return script && *script;
}

/* MEMORIES_DUMP_AUDIO=path: no device; mix in real time into raw s16le
 * stereo 44.1 kHz so output can be inspected without speakers. With no path
 * this is the silent sink: the SPU still has to run, because the game polls
 * envelopes and the disc service waits for CD input room. */
static void (*silent_mixer)(int16_t *, size_t);

static void *run_silent(void *path)
{
    static int16_t buffer[256 * 2];
    FILE *file = path ? fopen(path, "wb") : NULL;
    struct timespec nap = {0, 256 * 1000000000ll / 44100};
    for (;;) {
        silent_mixer(buffer, 256);
        if (file) {
            fwrite(buffer, sizeof(buffer), 1, file);
            fflush(file);
        }
        nanosleep(&nap, NULL);
    }
    return NULL;
}

int Platform_StartSilentAudio(void (*mix)(int16_t *, size_t), const char *dump_path)
{
    pthread_t thread;
    sigset_t all, previous;
    int error;
    silent_mixer = mix;
    /* The timer signal is the game's interrupt and must stay on the main thread. */
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &previous);
    error = pthread_create(&thread, NULL, run_silent, (void *)dump_path);
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return error ? -1 : 0;
}
