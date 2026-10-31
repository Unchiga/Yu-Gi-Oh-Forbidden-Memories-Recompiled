#define _GNU_SOURCE
#include "state.h"
#include "state_io.h"
#include "state_requests.h"
#include "state_subsystems.h"
#include "state_remap.h"
#include "pc/platform/settings.h"
#include "pc/platform/paths.h"
#include "pc/mods/mods.h"
#include "pc/mods/events.h"
#include "pc/memory.h"
#include "image.h"
#include "retail_image.h"
#include "pc/audio/spu.h"
#include "pc/audio/replace.h"
#include "pc/compat/gte.h"
#include "pc/rng.h"
#include "pc/render/soft_gpu.h"
#include "pc/render/texture_dump.h"
#include "pc/saves/deck_menu.h"
#include "pc/cards/pack_shop.h"
#include "pc/text/language.h"
#include "pc/text/text.h"
#include "pc/platform/menu.h"
#include "pc/platform/title_screen.h"
#include "pc/debug/crash.h"
#include "pc/debug/log.h"
#include "pc/tower/tower.h"
#include "pc/compat/signal.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pc/compat/mman.h"
#include <sys/stat.h>
#include <unistd.h>
#include "pc/compat/posix.h"
#ifdef _WIN32
#include "pc/platform/win32.h"
#elif !defined(__ANDROID__)
#ifdef __APPLE__
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#endif

/* The game stack is at the same address on every system (state.h). Linux
 * builds up to v0.2.0 had it at 0x70000000: their states hold that stack,
 * and cannot resume on this one. */
#define STACK_BASE MEMORIES_GAME_STACK_BASE
#define OLD_LINUX_STACK_BASE 0x70000000u
/* Which system's build made a state ("system" chunk): the game code's
 * layout, which the stack's return addresses point into, is the compiler's
 * for that system. States from before the chunk tell by the stack. */
#ifdef _WIN32
#define SYSTEM "Windows"
#elif defined(__ANDROID__)
#define SYSTEM "Android"
#else
#define SYSTEM "Linux"
#endif
#define STACK_SIZE MEMORIES_GAME_STACK_SIZE
#define STACK_TOP (STACK_BASE + STACK_SIZE)
#define SCRATCHPAD MEMORIES_GUEST_SCRATCHPAD
#define SCRATCHPAD_SIZE MEMORIES_GUEST_SCRATCHPAD_SIZE
/* 1: the header word was the game-source fingerprint; 2: the build id;
 * 3: the game compiled with the indirect-branch thunks (branch_thunks.c).
 * Every function's code changed shape with them, and a return address on a
 * saved stack cannot be carried into it: on Windows, where symbols have no
 * sizes, a function whose extent stayed the same passed for unchanged and
 * the state resumed a byte off (Graphics_SyncFrame's call to VSync). So
 * states from before 3 are refused. */
#define VERSION 3u

/* Provided by the link: the fixed-address sections of the game objects. */
extern char __start_game_text[], __stop_game_text[];
extern char __start_game_data[], __stop_game_data[];
extern char __start_game_bss[] __attribute__((weak)), __stop_game_bss[] __attribute__((weak));
void Memories_StateReturn(const MemoriesStateEntry *entry, int value) __attribute__((noreturn));

MemoriesStateEntry Memories_StateEntry;

typedef struct Region {
    const char *name;
    char *data, *data_end, *bss, *bss_end;
    char *startup; /* the data as linked, before the game ran */
} Region;

static Region *regions;
static unsigned region_count;
#if defined(_WIN32) || defined(__ANDROID__) || (defined(__aarch64__) && !defined(MEMORIES_TRANSLATED))
/* Windows has no ucontext, and neither has Android's C library (bionic).
 * A context there is the stack pointer of a suspended
 * Memories_ContextSwitch (state_i386.S, state_x86_64.S, state_aarch64.S),
 * which keeps the callee-saved registers on that stack. */
#define ASM_CONTEXT_SWITCH 1
void Memories_ContextSwitch(uintptr_t *from_sp, const uintptr_t *to_sp);
static uintptr_t service_context, game_context;
#ifdef _WIN32
/* On Windows the thread's stack bounds and exception-handler
 * chain live in its TEB and must follow the stack, as fibers do: exceptions
 * raised on a stack outside those bounds cannot be dispatched. Bounds are
 * the TEB's first three words (handler chain, stack base, stack limit) and
 * its DeallocationStack (0xE0C), the bottom of the stack for Windows' guard
 * page logic.
 *
 * The game stack's bottom is guarded (Win32_GuardStack): a guard page
 * GUARD_ROOM above it, and DeallocationStack just above that, so Windows
 * and Wine take a touch of it for a plain guard page exception (below
 * DeallocationStack is not the stack to them, so not stack growth) with
 * GUARD_ROOM of stack left to report the overflow in. Running off the end
 * of the stack instead leaves no room to deliver the exception, and the
 * process just ends. */
#define GUARD_ROOM 0x10000u
#if defined(__x86_64__)
/* The 64-bit build (state_x86_64.S): the same, through the 64-bit TEB at
 * %gs, whose stack base, limit and DeallocationStack are at 0x08, 0x10 and
 * 0x1478. x86-64 exceptions are dispatched from unwind tables, not a
 * handler chain, so word 0 is not switched. */
static uintptr_t process_bounds[4];
static const uintptr_t game_bounds[4] = {0, STACK_TOP, STACK_BASE, STACK_BASE + GUARD_ROOM + 0x1000u};

static void save_stack_bounds(uintptr_t *bounds)
{
    bounds[0] = 0;
    __asm__ volatile("movq %%gs:8, %0\n\tmovq %%gs:0x10, %1\n\tmovq %%gs:0x1478, %2"
                     : "=r"(bounds[1]), "=r"(bounds[2]), "=r"(bounds[3]));
}

static void set_stack_bounds(const uintptr_t *bounds)
{
    __asm__ volatile("movq %0, %%gs:8\n\tmovq %1, %%gs:0x10\n\tmovq %2, %%gs:0x1478"
                     :
                     : "r"(bounds[1]), "r"(bounds[2]), "r"(bounds[3])
                     : "memory");
}
#else
static uint32_t process_bounds[4];
static const uint32_t game_bounds[4] = {0xffffffffu, STACK_TOP, STACK_BASE, /* no handlers */
                                        STACK_BASE + GUARD_ROOM + 0x1000u};

static void save_stack_bounds(uint32_t *bounds)
{
    __asm__ volatile("movl %%fs:0, %0\n\tmovl %%fs:4, %1\n\tmovl %%fs:8, %2\n\tmovl %%fs:0xe0c, %3"
                     : "=r"(bounds[0]), "=r"(bounds[1]), "=r"(bounds[2]), "=r"(bounds[3]));
}

static void set_stack_bounds(const uint32_t *bounds)
{
    __asm__ volatile("movl %0, %%fs:0\n\tmovl %1, %%fs:4\n\tmovl %2, %%fs:8\n\tmovl %3, %%fs:0xe0c"
                     :
                     : "r"(bounds[0]), "r"(bounds[1]), "r"(bounds[2]), "r"(bounds[3])
                     : "memory");
}
#endif
#else
/* Elsewhere nothing but the stack pointer says which stack a thread is on. */
#define save_stack_bounds(bounds) ((void)0)
#define set_stack_bounds(bounds) ((void)0)
#endif

#if defined(__x86_64__) || (defined(__aarch64__) && !defined(MEMORIES_TRANSLATED))
/* setjmp_x86_64.S, setjmp_aarch64.S: where the registers for a game jmp_buf
 * are kept. The game's buffer (the Psy-Q int[12] at 0x800E9DC0) is too
 * small for them, so each buffer address gets a host slot of 240 bytes. The
 * game uses one. */
static struct {
    void *buffer;
    uint64_t words[30];
} jump_slots[16];

void *Memories_JumpSlot(void *buffer)
{
    unsigned i;
    for (i = 0; i < sizeof(jump_slots) / sizeof(jump_slots[0]) && jump_slots[i].buffer; i++) {
        if (jump_slots[i].buffer == buffer) return jump_slots[i].words;
    }
    if (i == sizeof(jump_slots) / sizeof(jump_slots[0])) {
        Crash_ReportFatal("setjmp", "more than 16 jmp_buf addresses");
        _exit(70);
    }
    jump_slots[i].buffer = buffer;
    return jump_slots[i].words;
}

void Memories_StateJumps(MemoriesState *state)
{
    MemoriesStateField jumps = {jump_slots, sizeof(jump_slots)};
    Memories_StateChunk(state, "jump-slots", &jumps, 1);
}
#endif

#ifdef ASM_CONTEXT_SWITCH
static void leave_game_stack(void)
{
    set_stack_bounds(process_bounds);
    Memories_ContextSwitch(&game_context, &service_context);
}
#endif

#if defined(__x86_64__)
/* What Memories_ContextSwitch (state_x86_64.S) pops to start `function` on
 * a stack whose top is `limit`: xmm6-xmm15 (20 words), eight registers,
 * then the return into `function`, 16-byte aligned so that `function`
 * starts as a called function does, with room above it for the shadow
 * space a Win64 callee may write. Returns the context. */
static uintptr_t switch_frame(uintptr_t limit, void (*function)(void))
{
    uintptr_t *slot = (uintptr_t *)((limit - 64) & ~(uintptr_t)15);
    uintptr_t *frame = slot - 8 - 20;
    memset(frame, 0, (size_t)((char *)(slot + 8) - (char *)frame));
    *slot = (uintptr_t)function;
    return (uintptr_t)frame;
}
#elif defined(__aarch64__) && !defined(MEMORIES_TRANSLATED)
/* A frame for Memories_ContextSwitch (state_aarch64.S) to resume `function`
 * from, below `limit`: what it pops (D8-D15, X19-X28, X29), then LR, which
 * it returns to. `function` never returns (it would start over). It starts
 * with the stack 16-byte aligned, as AAPCS64 requires, and a zero frame
 * pointer, where a stack walk ends. Returns the context. */
static uintptr_t switch_frame(uintptr_t limit, void (*function)(void))
{
    uintptr_t start = (limit - 64u) & ~(uintptr_t)15; /* the stack pointer `function` starts with */
    uintptr_t *frame = (uintptr_t *)(start - 176u);
    memset(frame, 0, 176u);
    frame[19] = (uintptr_t)function; /* LR, at 152 */
    return (uintptr_t)frame;
}
#else
/* A frame for Memories_ContextSwitch to resume `function` from, below
 * `limit`: what it pops (EDI ESI EBX EBP), then the return into `function`,
 * whose own return address is never used. `function` starts with the stack
 * as a call leaves it when the stack was 16-byte aligned at the call: the
 * i386 System V ABI (Android) keeps SSE values on the stack at that
 * alignment; Windows needs 4. Returns the context. */
static uint32_t switch_frame(uint32_t limit, void (*function)(void))
{
    uint32_t start = ((limit - 32u) & ~15u) - 4u; /* the stack pointer `function` starts with */
    uint32_t *frame = (uint32_t *)(uintptr_t)(start - 20u);
    frame[0] = frame[1] = frame[2] = frame[3] = 0;
    frame[4] = (uint32_t)(uintptr_t)function;
    frame[5] = 0;
    return (uint32_t)(uintptr_t)frame;
}
#endif

#if defined(__ANDROID__) && defined(__aarch64__)
void Memories_CallOnStack(void (*function)(void *), void *argument, uintptr_t top); /* state_aarch64.S */
#endif

/* The VSync a loaded state resumes in (apply), and what it returns. */
static MemoriesStateEntry resume_entry;
static int resume_value;

/* The first thing the game stack runs after a load: return from that VSync. */
static void resume_game(void)
{
    Memories_StateReturn(&resume_entry, resume_value);
}
#else
static ucontext_t service_context, game_context;
#endif
#if defined(__ANDROID__) && defined(__aarch64__)
/* MEMORIES_TEST_HOST_STACK=1: how deep the calls Memories_OnHostStack runs
 * on the thread's own stack go. At the first switch the span below the
 * switch point is painted (at most 512 KiB, and never within 64 KiB of the
 * stack's low end, where ART keeps protected pages), and
 * Memories_HostStackUsed finds the lowest word written since. */
#include <pthread.h>
#define HOST_STACK_PAINT 0xa5a5a5a5a5a5a5a5ull
static uintptr_t paint_low, paint_top;
static size_t host_stack_size;
static int paint_state; /* 0 not yet, 1 painted, -1 off */

static void paint_host_stack(uintptr_t top)
{
    const char *value = getenv("MEMORIES_TEST_HOST_STACK");
    pthread_attr_t attributes;
    void *address = NULL;
    size_t size = 0;
    uintptr_t low, at;
    paint_state = -1;
    if (!value || !*value || !strcmp(value, "0")) return;
    if (pthread_getattr_np(pthread_self(), &attributes)) return;
    if (pthread_attr_getstack(&attributes, &address, &size)) size = 0;
    pthread_attr_destroy(&attributes);
    if (!size || top <= (uintptr_t)address + 64u * 1024u) return;
    low = (uintptr_t)address + 64u * 1024u;
    if (top - low > 512u * 1024u) low = top - 512u * 1024u;
    low = (low + 7u) & ~(uintptr_t)7u;
    for (at = low; at + 8u <= top; at += 8u) *(volatile uint64_t *)at = HOST_STACK_PAINT;
    paint_low = low;
    paint_top = top;
    host_stack_size = size;
    paint_state = 1;
    fprintf(stderr, "memories-pc: host stack: %zu bytes painted below the switch point %p (thread stack %zu bytes)\n",
            (size_t)(top - low), (void *)top, size);
}

size_t Memories_HostStackUsed(size_t *painted, size_t *stack_size)
{
    uintptr_t at;
    if (paint_state != 1) return 0;
    for (at = paint_low; at < paint_top && *(const volatile uint64_t *)at == HOST_STACK_PAINT; at += 8u) {
    }
    *painted = paint_top - paint_low;
    *stack_size = host_stack_size;
    return paint_top - at;
}
#endif

void Memories_OnHostStack(void (*function)(void *), void *argument)
{
#if defined(__ANDROID__) && defined(__aarch64__)
    /* On the game stack, the process side waits in Memories_ContextSwitch
     * with its stack pointer in service_context: the thread's own stack
     * below it is free (256 bytes kept below the saved frame). */
    uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    if (here >= STACK_BASE && here < STACK_TOP && service_context) {
        if (!paint_state) paint_host_stack((service_context - 256u) & ~(uintptr_t)15);
        Memories_CallOnStack(function, argument, (service_context - 256u) & ~(uintptr_t)15);
        return;
    }
#endif
    function(argument);
}

#ifdef __ANDROID__
static unsigned jni_guard_count;

void Memories_JniGuard(const char *name)
{
    static const char *seen[48];
    static unsigned seen_count;
    uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    unsigned i;
    if (here < STACK_BASE || here >= STACK_TOP) return;
    jni_guard_count++;
    for (i = 0; i < seen_count; i++) {
        if (seen[i] == name) return;
    }
    if (seen_count < sizeof(seen) / sizeof(seen[0])) seen[seen_count++] = name;
    fprintf(stderr, "memories-pc: %s, which may call Java, ran on the game stack (%p): Android's runtime refuses "
            "that; its caller must go through Memories_OnHostStack\n", name, (void *)here);
}

unsigned Memories_JniGuardCount(void) { return jni_guard_count; }
#endif

static int (*game_entry)(void);
static int game_result;
static MemoriesStateRequests requests = MEMORIES_STATE_REQUESTS_INIT;
static volatile int last_loaded_slot;
static uint8_t *pending_image;
static uint32_t build_id; /* from the `buildid` file beside the executable */
static size_t pending_size;

int Memories_LastStateSlot(void) { return last_loaded_slot; }

void Memories_StateRequest(int what, int slot)
{
    Memories_StateQueue(&requests, what, slot);
}

void Memories_StateRemapRange(MemoriesState *state, uint32_t from, uint32_t to, uint32_t size)
{
    if (state->loading) Memories_StateRemapImage((uint8_t *)state->image, state->image_size, from, to, size);
}

/* Why a state was not loaded: on stderr, and in a notice over the picture
 * (Menu_ShowNotice), since F7 is otherwise silent where there is no
 * console. load() runs in the game's thread, which is the window's. */
/* Set while the control channel loads (Memories_StateLoadHere): the reason
 * goes back to it instead of into a notice. */
static char *refusal;
static size_t refusal_size;
static int tower_refused;   /* the last load was one a tower refused (Tower_Guard) */

static void refuse(const char *format, ...)
{
    static const char *const ok[] = {"OK"};
    char text[900];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    fprintf(stderr, "memories-pc: %s\n", text);
    if (refusal) snprintf(refusal, refusal_size, "%s", text);
    else Menu_ShowNotice("Save state not loaded", text, ok, 1, 0, NULL);
}

static int compatible_language(const MemoriesState *state, const char *path)
{
    char why[900];
    if (Memories_StateCompatibleLanguage(state, path, why, sizeof(why))) return 1;
    refuse("%s", why);
    return 0;
}

#if defined(__ANDROID__) && defined(__aarch64__)
static void state_subsystems(MemoriesState *state)
{
    /* Android's native AArch64 setjmp buffers are hosted outside game RAM. */
    extern void Memories_StateJumps(MemoriesState *state);
    Memories_StateJumps(state);
    Memories_StateSubsystems(state);
}
#else
#define state_subsystems Memories_StateSubsystems
#endif

static void tagged(char *out, size_t size, const char *kind, const char *name)
{
    snprintf(out, size, "%s:%s", kind, name);
}

static void hold_signals(int hold)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(hold ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL);
}

/* The whole state, to a file. Chunks are found by tag, in any order; the
 * stack, whose size follows the depth of the VSync call, goes last. */
static void serialize(MemoriesState *state)
{
    MemoriesStateEntry entry = Memories_StateEntry;
    char tag[32];
    uint32_t header[2] = {VERSION, build_id};
    unsigned i;
    hold_signals(1);
    Spu_Hold(1);
    Memories_StateWrite(state, "YFMSTATE", 8);
    Memories_StateWrite(state, header, 8);
    {
        MemoriesStateField fields[] = {{&entry, sizeof(entry)}};
        Memories_StateChunk(state, "entry", fields, 1);
    }
    {
        char system[16] = SYSTEM;
        MemoriesStateField fields[] = {{system, sizeof(system)}};
        Memories_StateChunk(state, "system", fields, 1);
    }
    {
        MemoriesStateField fields[] = {{(void *)(uintptr_t)MEMORIES_GUEST_RAM, MEMORIES_GUEST_RAM_SIZE},
                                       {(void *)(uintptr_t)SCRATCHPAD, SCRATCHPAD_SIZE}};
        Memories_StateChunk(state, "memory", fields, 2);
    }
    for (i = 0; i < region_count; i++) {
        Region *region = &regions[i];
        MemoriesStateField data[] = {{region->startup, (size_t)(region->data_end - region->data)},
                                     {region->data, (size_t)(region->data_end - region->data)}};
        MemoriesStateField bss[] = {{region->bss, (size_t)(region->bss_end - region->bss)}};
        tagged(tag, sizeof(tag), "data", region->name);
        Memories_StateChunk(state, tag, data, 2);
        tagged(tag, sizeof(tag), "bss", region->name);
        Memories_StateChunk(state, tag, bss, 1);
    }
    {
        MemoriesModEvent event = {MEMORIES_EVENT_SAVE, MEMORIES_BEFORE, 0, 0, 0, 0, 0};
        Mods_Dispatch(&event);
    }
    if (Memories_ModPacketsUsed()) {
        /* The state point is before GsDrawOt: the frame's ordering tables
         * still link into the mods' packet area, so it goes with them. */
        uint32_t physical = Memories_ModPacketPhysical;
        MemoriesStateField fields[] = {{&physical, sizeof(physical)},
                                       {Memories_ModPacketBase, Memories_ModPacketsUsed()}};
        Memories_StateChunk(state, "mod-packets", fields, 2);
    }
    state_subsystems(state);
    {
        MemoriesStateField fields[] = {{(void *)(uintptr_t)MEMORIES_STATE_ENTRY_SP(entry), STACK_TOP - MEMORIES_STATE_ENTRY_SP(entry)}};
        Memories_StateChunk(state, "stack", fields, 1);
    }
    Spu_Hold(0);
    hold_signals(0);
}

static int save(const char *path, int tell)
{
    MemoriesState state = {0, NULL, NULL, 0};
    char partial[1024];
    int failed, length;
    /* Every save comes here (the hotkeys, the menu, autosave, the control
     * channel): a tower whose mod turns save states off refuses it while
     * the player is in it (tower.h, Tower_Guard). */
    if (Tower_Guard(TOWER_GUARD_SAVE_STATE, tell, NULL, 0)) return -1;
    Paths_WriteBegin();
    length = snprintf(partial, sizeof(partial), "%s.partial", path);
    if (length < 0 || (size_t)length >= sizeof(partial)) {
        errno = ENAMETOOLONG;
        return Memories_StateSaveFailure(path, tell);
    }
    state.file = fopen(partial, "wb");
    if (!state.file) return Memories_StateSaveFailure(path, tell);
    serialize(&state);
    failed = ferror(state.file) != 0; /* a short write (a full disk) leaves fclose content */
    if (fclose(state.file) != 0) failed = 1;
    if (failed || rename(partial, path) != 0) {
        Memories_StateSaveFailure(path, tell); /* before remove() changes the reason */
        remove(partial);
        return -1;
    }
    fprintf(stderr, "memories-pc: state saved to %s\n", path);
    Paths_WriteDone(path);
    return 0;
}

/* Runs on the service (process) stack: the game stack is about to be replaced. */
static void apply(void)
{
    MemoriesState state = {1, NULL, pending_image, pending_size};
    static MemoriesStateEntry entry; /* not on a stack that a handler may share */
    const uint8_t *chunk;
    size_t size;
    char tag[32];
    unsigned i;
    refusal = NULL; /* accepted: Memories_StateLoadHere's buffer is on the stack being replaced */
    hold_signals(1);
    Spu_Hold(1);
    DeckMenu_ShopState(&state);
    PackShop_State(&state); /* its text, like the shop's menu, remapped first */
    chunk = Memories_StateFindChunk(&state, "memory", &size);
    memcpy((void *)(uintptr_t)MEMORIES_GUEST_RAM, chunk, MEMORIES_GUEST_RAM_SIZE);
    memcpy((void *)(uintptr_t)SCRATCHPAD, chunk + MEMORIES_GUEST_RAM_SIZE, SCRATCHPAD_SIZE);
    for (i = 0; i < region_count; i++) {
        Region *region = &regions[i];
        size_t length = (size_t)(region->data_end - region->data), word, saved;
        tagged(tag, sizeof(tag), "data", region->name);
        chunk = Memories_StateFindChunk(&state, tag, &size);
        /* A shorter chunk is a state from before the variables that now
         * end the section (the Windows build's small data, which came in
         * after the rest: build_game32.py): those keep their values. Only
         * that build's sections grew so; elsewhere the size must match. */
        saved = size / 2;
#ifdef _WIN32
        if (chunk && size % 2 == 0 && saved <= length) {
#else
        if (chunk && size % 2 == 0 && saved == length) {
#endif
            memcpy(region->data, chunk + saved, saved);
            /* Relocated words the game never changed follow this build. */
            for (word = 0; word + 4 <= saved; word += 4) {
                if (!memcmp(chunk + word, chunk + saved + word, 4) && memcmp(chunk + word, region->startup + word, 4)) {
                    memcpy(region->data + word, region->startup + word, 4);
                }
            }
        } else {
            fprintf(stderr, "memories-pc: state: variables of '%s' do not match this build\n", region->name);
        }
        tagged(tag, sizeof(tag), "bss", region->name);
        chunk = Memories_StateFindChunk(&state, tag, &size);
        if (chunk && size == (size_t)(region->bss_end - region->bss)) {
            memcpy(region->bss, chunk, size);
        } else if (chunk) {
            fprintf(stderr, "memories-pc: state: zeroed variables of '%s' do not match this build\n", region->name);
        }
    }
    Memories_ModPacketsFrame();
    chunk = Memories_StateFindChunk(&state, "mod-packets", &size);
    if (chunk && size > 4 && size - 4 <= MEMORIES_MOD_PACKETS_SIZE) {
        /* Mapped here too: a new process has no mod that asked for it yet. */
        uint32_t physical;
        memcpy(&physical, chunk, sizeof(physical));
        if (Memories_MapModPackets(physical)) {
            memcpy(Memories_ModPacketBase, chunk + 4, size - 4);
            Memories_SetModPacketsUsed((uint32_t)(size - 4));
        } else {
            fprintf(stderr, "memories-pc: state: no room for the mods' packet area\n");
        }
    }
    state_subsystems(&state);
    chunk = Memories_StateFindChunk(&state, "entry", &size);
    memcpy(&entry, chunk, sizeof(entry));
    chunk = Memories_StateFindChunk(&state, "stack", &size);
    memcpy((void *)(uintptr_t)MEMORIES_STATE_ENTRY_SP(entry), chunk, size);
    free(pending_image);
    pending_image = NULL;
    Spu_Hold(0);
    Mods_Reset(); /* another game: whatever the mods were holding is not it */
    AudioReplace_StateLoaded(); /* replacement sounds are not in the state: the song restarts */
    {
        MemoriesModEvent event = {MEMORIES_EVENT_LOAD, MEMORIES_AFTER, 0, 0, 0, 0, 0};
        Mods_Dispatch(&event);
    }
    fprintf(stderr, "memories-pc: state loaded\n");
    const int value = LibEtc_StateResumed(); /* the fields VSync(0) reports, as for the game that saved it */
    hold_signals(0);
#ifdef ASM_CONTEXT_SWITCH
    set_stack_bounds(game_bounds);
    {
        /* Into the game through a context switch, as its first run went, so
         * that the service context is taken again here. The one taken
         * before kept its registers on this stack, where apply has run since:
         * the next load would resume from those (EBP 0, a return into the
         * middle of Memories_StateRunGame). The switch lands in resume_game
         * on the game stack, below what the state restored there. */
        resume_entry = entry;
        resume_value = value;
        game_context = switch_frame(MEMORIES_STATE_ENTRY_SP(entry) - 64u, resume_game);
        Memories_ContextSwitch(&service_context, &game_context);
    }
#else
    Memories_StateReturn(&entry, value);
#endif
}

/* Relocation across game-source changes. A state holds addresses of game
 * code: return addresses on the stack, and callbacks stored in guest RAM, in
 * game variables and in a few native chunks. The build files every game
 * build's symbols under its fingerprint (tmp/pc/game32/symbols/), so a state
 * from another build can be carried over by name:
 *
 * - a word equal to the start of an old function becomes the new start,
 *   wherever function pointers live (aligned words of guest RAM and game
 *   data, and the callback-bearing part of the native chunks);
 * - on the stack any address inside an old function moves with it, which is
 *   only right if that function is unchanged, so a function on the stack
 *   whose size differs refuses the load;
 * - game variables must not have moved at all, or the load is refused.
 *
 * A value that merely looks like a function's first byte would be rewritten
 * too; with about a thousand functions that is improbable, and the count of
 * rewritten words is reported. */
typedef struct Symbol {
    uint32_t address, size;
    char name[72];
} Symbol;

/* The program directory (paths.h), where the build keeps buildid and
 * symbols/ beside the executable. */
static int executable_directory(char *out, size_t size)
{
    const char *directory = Paths_ProgramDir();
    size_t length = strlen(directory);
    if (length >= size) return -1;
    memcpy(out, directory, length + 1);
    return 0;
}

int Memories_SymbolTablePath(char *out, size_t size)
{
    char directory[512], path[640], text[32] = "";
    FILE *file;
    if (executable_directory(directory, sizeof(directory))) return -1;
    snprintf(path, sizeof(path), "%s/buildid", directory);
    file = fopen(path, "r");
    if (!file) return -1;
    if (fgets(text, sizeof(text), file)) build_id = (uint32_t)strtoul(text, NULL, 16);
    fclose(file);
    if (!build_id || snprintf(out, size, "%s/symbols/%08x.txt", directory, (unsigned)build_id) >= (int)size) {
        return -1;
    }
    return 0;
}

static Symbol *read_symbols(uint32_t fingerprint, size_t *count)
{
    char path[640], exe[512];
    Symbol *table = NULL;
    FILE *file;
    size_t used = 0, room = 0;
    *count = 0;
    if (executable_directory(exe, sizeof(exe))) return NULL;
    snprintf(path, sizeof(path), "%s/symbols/%08x.txt", exe, (unsigned)fingerprint);
    file = fopen(path, "r");
    if (!file) {
        return NULL;
    }
    for (;;) {
        Symbol symbol;
        unsigned address, size;
        if (fscanf(file, "%x %x %71s", &address, &size, symbol.name) != 3) {
            break;
        }
        symbol.address = address;
        symbol.size = size;
        if (used == room) {
            room = room ? room * 2 : 2048;
            table = realloc(table, room * sizeof(*table));
            if (!table) {
                fclose(file);
                return NULL;
            }
        }
        table[used++] = symbol;
    }
    fclose(file);
    *count = used;
    return table;
}

static const Symbol *by_name(const Symbol *table, size_t count, const char *name)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (!strcmp(table[i].name, name)) {
            return &table[i];
        }
    }
    return NULL;
}

/* Tables are in address order. */
static const Symbol *containing(const Symbol *table, size_t count, uint32_t address)
{
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = (low + high) / 2;
        if (table[middle].address + (table[middle].size ? table[middle].size : 1) <= address) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low < count && table[low].address <= address ? &table[low] : NULL;
}

/* The tables hold functions and the game objects' variables; the variables
 * are the ones linked between these addresses. */
static int is_text(uint32_t address) { return address < 0x03000000u || address >= 0x08000000u; }
static int is_game_text(uint32_t address) { return address >= 0x01000000u && address < 0x01400000u; }

static void read_build_id(void)
{
    char path[640];
    Memories_SymbolTablePath(path, sizeof(path));
}

/* Returns the number of words rewritten, or -1 when the state cannot move. */
static long relocate_words(uint8_t *bytes, size_t size, size_t step, int whole_functions, const Symbol *old,
                           size_t old_count, const Symbol *new, size_t new_count)
{
    long changed = 0;
    size_t at;
    for (at = 0; at + 4 <= size; at += step) {
        const Symbol *from, *to;
        uint32_t value;
        memcpy(&value, bytes + at, 4);
        if (!is_text(value) || !(from = containing(old, old_count, value)) || !is_text(from->address)) {
            continue;
        }
        if (value != from->address && !whole_functions) {
            continue;
        }
        to = by_name(new, new_count, from->name);
        if (!to || (value != from->address && to->size != from->size)) {
            /* A native frame below the game's entry is never returned to. */
            if (whole_functions && is_game_text(from->address)) {
                fprintf(stderr, "memories-pc: state: %s was running when the state was saved and has changed since\n",
                        from->name);
                return -1;
            }
            continue;
        }
        value = to->address + (value - from->address);
        memcpy(bytes + at, &value, 4);
        changed += to->address != from->address;
    }
    return changed;
}

static int relocate(uint8_t *image, size_t image_size, uint32_t saved_fingerprint)
{
    size_t old_count, new_count, at = 16, i;
    Symbol *old = read_symbols(saved_fingerprint, &old_count);
    Symbol *new = read_symbols(build_id, &new_count);
    long total = 0;
    int result = -1;
    if (!old || !new) {
        fprintf(stderr, "memories-pc: state: no symbol table for build %08x or %08x in symbols/; cannot carry the "
                        "state over\n", (unsigned)saved_fingerprint, (unsigned)build_id);
        goto done;
    }
    for (i = 0; i < old_count; i++) {
        const Symbol *now = is_text(old[i].address) ? NULL : by_name(new, new_count, old[i].name);
        if (now && now->address != old[i].address) {
            fprintf(stderr, "memories-pc: state: game variable %s moved; cannot carry the state over\n", old[i].name);
            goto done;
        }
    }
    while (at + 20 <= image_size) {
        const char *tag = (const char *)image + at;
        uint32_t length;
        long changed = 0;
        memcpy(&length, image + at + 16, 4);
        if (length > image_size - at - 20) {
            break;
        }
        if (!strcmp(tag, "stack")) {
            changed = relocate_words(image + at + 20, length, 4, 1, old, old_count, new, new_count);
        } else if (!strcmp(tag, "memory") || !strncmp(tag, "data:", 5) || !strncmp(tag, "bss:", 4)) {
            changed = relocate_words(image + at + 20, length, 4, 0, old, old_count, new, new_count);
        } else if (!strcmp(tag, "libetc") || !strcmp(tag, "libpress") || !strcmp(tag, "libds")) {
            /* Packed fields: callbacks sit at any offset, all within the first kilobyte. */
            changed = relocate_words(image + at + 20, length < 1024 ? length : 1024, 1, 0, old, old_count, new,
                                     new_count);
        }
        if (changed < 0) {
            goto done;
        }
        total += changed;
        at += 20 + length;
    }
    fprintf(stderr, "memories-pc: state from build %08x carried over to %08x: %ld code addresses moved\n",
            (unsigned)saved_fingerprint, (unsigned)build_id, total);
    result = 0;
done:
    free(old);
    free(new);
    return result;
}

static int load(const char *path)
{
    MemoriesState state = {1, NULL, NULL, 0};
    MemoriesStateEntry entry;
    FILE *file = fopen(path, "rb");
    const uint8_t *chunk;
    uint8_t *image;
    uint32_t header[2];
    size_t size;
    long length;
    char why[256];
    tower_refused = 0;
    /* Every load comes here too: a tower whose mod turns save states off
     * refuses it while the player is in it (Tower_Guard). */
    if (Tower_Guard(TOWER_GUARD_LOAD_STATE, 0, why, sizeof(why))) {   /* refuse() tells */
        if (file) fclose(file);
        tower_refused = 1;
        refuse("%s", why);
        return -1;
    }
    if (!file) {
        refuse("%s: %s", path, strerror(errno));
        return -1;
    }
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    rewind(file);
    image = malloc(length > 0 ? (size_t)length : 1);
    if (!image || length < 16 || fread(image, 1, (size_t)length, file) != (size_t)length ||
        memcmp(image, "YFMSTATE", 8)) {
        refuse("%s is not a save state", path);
        fclose(file);
        free(image);
        return -1;
    }
    fclose(file);
    memcpy(header, image + 8, 8);
    state.image = image;
    state.image_size = (size_t)length;
    chunk = Memories_StateFindChunk(&state, "entry", &size);
    if (header[0] < VERSION) {
        refuse("%s was made by an older version of the game; save states don't carry over across this update "
               "(memory card saves do)", path);
        free(image);
        return -1;
    }
    if (header[0] == VERSION && chunk && size != sizeof(entry) && (sizeof(void *) == 8 ? size == 20 : size >= 72)) {
        /* The registers at VSync: five 32-bit words from the 32-bit game,
         * nine or more 64-bit ones from the 64-bit game (another size here
         * is refused below, as an unsupported state). The rest of the
         * state is laid out for that build as well: its native stack frames,
         * its variables and its subsystems' fields. */
        int other = sizeof(void *) == 8 ? 32 : 64;
        refuse("%s was saved by the %d-bit game, and this one is %d-bit: a save state loads only in the kind of "
               "build that saved it", path, other, other == 32 ? 64 : 32);
        free(image);
        return -1;
    }
    if (header[0] != VERSION || !chunk || size != sizeof(entry)) {
        refuse("%s: unsupported state version", path);
        free(image);
        return -1;
    }
    memcpy(&entry, chunk, sizeof(entry));
    {
        /* The state holds the game stack, return addresses into the game code
         * as the other system's compiler laid it out: nothing here to resume. */
        char saved_by[16] = "";
        const uint8_t *system = Memories_StateFindChunk(&state, "system", &size);
        if (system && size == sizeof(saved_by)) {
            memcpy(saved_by, system, sizeof(saved_by) - 1);
        } else if (MEMORIES_STATE_ENTRY_SP(entry) >= OLD_LINUX_STACK_BASE &&
                   MEMORIES_STATE_ENTRY_SP(entry) < OLD_LINUX_STACK_BASE + STACK_SIZE) {
            refuse("%s was saved by an older Linux build, whose game stack was elsewhere; save states don't carry "
                   "over across this update (memory card saves do)", path);
            free(image);
            return -1;
        } else {
            strcpy(saved_by, "Windows"); /* before the chunk, only Windows had this stack */
        }
        if (strcmp(saved_by, SYSTEM)) {
            refuse("%s was saved by the %s build; a state loads only in a build for the system that saved it", path,
                   saved_by);
            free(image);
            return -1;
        }
    }
    chunk = Memories_StateFindChunk(&state, "stack", &size);
    if (!chunk || MEMORIES_STATE_ENTRY_SP(entry) < STACK_BASE || MEMORIES_STATE_ENTRY_SP(entry) >= STACK_TOP ||
        size != STACK_TOP - MEMORIES_STATE_ENTRY_SP(entry) ||
        !Memories_StateFindChunk(&state, "memory", &size) || size != MEMORIES_GUEST_RAM_SIZE + SCRATCHPAD_SIZE) {
        refuse("%s: damaged state", path);
        free(image);
        return -1;
    }
    if (header[1] != build_id && relocate(image, (size_t)length, header[1]) != 0) {
        refuse("%s was saved by another build and was not loaded", path);
        free(image);
        return -1;
    }
    if (!Memories_StateCompatibleMods(&state)) {
        refuse("save state uses different mods, card definitions or mod state layouts; restore its mod profile first");
        free(image); return -1;
    }
    if (!compatible_language(&state, path)) {
        free(image);
        return -1;
    }
    pending_image = image;
    pending_size = (size_t)length;
    /* Leave the game stack; the service context applies the state. */
#ifdef ASM_CONTEXT_SWITCH
    leave_game_stack();
#else
    swapcontext(&game_context, &service_context);
#endif
    return 0; /* not reached: the state resumes in its own VSync caller */
}

static int from_game_code(void)
{
    uint32_t caller;
    if (MEMORIES_STATE_ENTRY_SP(Memories_StateEntry) < STACK_BASE ||
        MEMORIES_STATE_ENTRY_SP(Memories_StateEntry) >= STACK_TOP) {
        return 0;
    }
    caller = MEMORIES_STATE_ENTRY_CALLER(Memories_StateEntry);
    return caller >= (uintptr_t)__start_game_text && caller < (uintptr_t)__stop_game_text;
}

int Memories_StateStartupDone(void) { return requests.startup_done; }

static void load_requested(const char *path, int slot)
{
    if (!load(path)) last_loaded_slot = slot;
    else if (!tower_refused) Crash_ReportSoft("state load failed", path);   /* a refusal is no failure */
}

void Memories_StatePoint(unsigned presented_frames)
{
    static const MemoriesStateActions actions = {save, load_requested};
    if (from_game_code()) Memories_StateProcess(&requests, presented_frames, &actions);
}

/* The control channel (src/pc/debug/control.c), from the end of VSync(0),
 * after the state point. */
int Memories_StateSaveHere(const char *path)
{
    if (!from_game_code()) return -2;
    return save(path, 0);
}

int Memories_StateLoadHere(const char *path, char *why, size_t why_size)
{
    int result;
    if (!from_game_code()) return -2;
    snprintf(why, why_size, "the state was not loaded (the reason is in the log)");
    refusal = why;
    refusal_size = why_size;
    result = load(path); /* returns only when it refused */
    refusal = NULL;
    if (!result) {
        snprintf(why, why_size, "the state was not loaded");
        result = -1;
    }
    return result;
}

uint32_t Memories_StateBuildId(void)
{
    if (!build_id) read_build_id();
    return build_id;
}

static void run_game(void)
{
    game_result = game_entry();
#ifdef ASM_CONTEXT_SWITCH
    leave_game_stack(); /* what uc_link does on Linux */
#endif
}

static int add_region(const char *name, char *data, char *data_end, char *bss, char *bss_end)
{
    Region *region = &regions[region_count++];
    size_t size = (size_t)(data_end - data);
    region->name = name;
    region->data = data;
    region->data_end = data_end;
    region->bss = bss;
    region->bss_end = bss_end;
    region->startup = malloc(size ? size : 1);
    if (!region->startup) {
        return -1;
    }
    memcpy(region->startup, data, size);
    return 0;
}

int Memories_StateRunGame(int (*entry)(void))
{
    void *stack = Memories_MapFixed((void *)(uintptr_t)STACK_BASE, STACK_SIZE, PROT_READ | PROT_WRITE,
                                    MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned i;
    if (stack != (void *)(uintptr_t)STACK_BASE) {
        /* Never another address: the game's return addresses and pointers
         * in a save state are on this one. Memories_MapFixed refuses a
         * range that holds a mapping (EEXIST) on every kernel. */
        int error = errno;
        fprintf(stderr, "memories-pc: the game stack needs 0x%08x-0x%08x, %s; the game cannot run without it\n",
                (unsigned)STACK_BASE, (unsigned)STACK_TOP,
                error == EEXIST ? "which something else in this process already holds" : strerror(error));
        return 1;
    }
    read_build_id();
    regions = calloc(Memories_ModuleCount + 1, sizeof(*regions));
    if (!regions || add_region("game", __start_game_data, __stop_game_data, __start_game_bss, __stop_game_bss)) {
        return 1;
    }
    for (i = 0; i < Memories_ModuleCount; i++) {
        const MemoriesModule *module = &Memories_Modules[i];
        if (add_region(module->name, module->data, module->data_end, module->bss, module->bss_end)) {
            return 1;
        }
    }
    game_entry = entry;
#ifdef ASM_CONTEXT_SWITCH
    {
        game_context = switch_frame(STACK_TOP, run_game);
#ifdef _WIN32
        Win32_GuardStack(STACK_BASE, GUARD_ROOM);
#endif
        save_stack_bounds(process_bounds);
        set_stack_bounds(game_bounds);
        /* Every load request re-enters here, on the process stack. */
        Memories_ContextSwitch(&service_context, &game_context);
    }
#else
    getcontext(&game_context);
    game_context.uc_stack.ss_sp = stack;
    game_context.uc_stack.ss_size = STACK_SIZE;
    game_context.uc_link = &service_context;
    makecontext(&game_context, run_game, 0);
    /* Every load request re-enters here, on the process stack. */
    swapcontext(&service_context, &game_context);
#endif
    /* With the asm switch (Windows, Android) apply returns once the game
     * leaves its stack again: for the next load, or at its end (run_game). */
    while (pending_image) {
        apply();
    }
    return game_result;
}
