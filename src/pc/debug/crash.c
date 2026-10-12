#define _GNU_SOURCE
#include "pc/compat/fs.h"
#include "crash.h"
#include "log.h"
#include "monitor.h"
#include "symbols.h"
#include "pc/guest/state.h"
#include "pc/platform/platform.h"
#include "pc/sdk/display.h"
#include "pc/platform/paths.h"
#ifdef __APPLE__
#include "pc/platform/darwin_image.h"
#ifdef MEMORIES_TRANSLATED
#include "pc/guest/translated_state_backend.h"
#include "pc/guest/translated_runtime.h"
#endif
#endif
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef _WIN32
#include "pc/platform/win32.h"
#elif defined(__SWITCH__)
#include <switch.h>
#else
#include "pc/platform/signal_context.h"
#endif

#define GAME_STACK_LOW 0xB0000000u /* state.c, on every system */
#define GAME_STACK_HIGH 0xB0800000u

/* An address in the report, and the registers line. The 64-bit Windows
 * build's long is 32 bits: its addresses are printed as uintptr_t, whole. */
#if defined(_WIN32) && defined(__x86_64__)
#include <inttypes.h>
#define ADDRESS "%08" PRIxPTR
#define REGISTERS "registers: RIP=0x" ADDRESS " RSP=0x" ADDRESS " RBP=0x" ADDRESS "\n"
#elif defined(__aarch64__)
/* AArch64: the program counter, stack pointer, frame pointer (x29),
 * and the link register on a line of its own (fault_lr), whole. */
#define ADDRESS "%016lx"
#define REGISTERS "registers: PC=0x%016lx SP=0x%016lx FP(x29)=0x%016lx\n"
static uintptr_t fault_lr;
#else
#define ADDRESS "%08lx"
#define REGISTERS "registers: EIP=0x%08lx ESP=0x%08lx EBP=0x%08lx\n"
#endif

static unsigned char alternate_stack[64 * 1024];
static uintptr_t main_stack_low, main_stack_high;
static volatile sig_atomic_t reporting;
static int report_fd = -1;

static void output(const char *text, size_t length)
{
    if (length) {
        (void)!write(2, text, length);
        if (report_fd >= 0) (void)!write(report_fd, text, length);
    }
}

static void line(const char *format, uintptr_t a, uintptr_t b, uintptr_t c)
{
    char text[512];
    int length = snprintf(text, sizeof(text), format, a, b, c);
    if (length > 0) output(text, (size_t)(length < (int)sizeof(text) ? length : (int)sizeof(text) - 1));
}

static const char *region(uintptr_t address)
{
#ifdef _WIN32
    uintptr_t image_low, image_high;
    Win32_ImageRange(&image_low, &image_high);
#elif defined(__APPLE__)
    uintptr_t image_low, image_high;
    DarwinImage_TextRange(&image_low, &image_high);
#elif defined(__SWITCH__)
    extern char _start[], __rodata_start[]; /* crt0's entry starts the code (switch.ld's __start__ is absolute), read-only data follows */
    uintptr_t image_low = (uintptr_t)_start, image_high = (uintptr_t)__rodata_start;
#else
    extern char __executable_start[], etext[];
    uintptr_t image_low = (uintptr_t)__executable_start, image_high = (uintptr_t)etext;
#endif
    if ((address >= 0x80000000u && address < 0x80200000u) ||
        (address >= 0xa0000000u && address < 0xa0200000u) || address < 0x00200000u) return "guest RAM";
    if ((address >= 0x1f800000u && address < 0x1f801000u) || (address >= 0x9f800000u && address < 0x9f801000u))
        return "scratchpad";
    if (address >= 0x01000000u && address < 0x0a000000u) return "game section";
    if (address >= GAME_STACK_LOW && address < GAME_STACK_HIGH) return "game stack";
    if (address >= image_low && address < image_high) return "native text";
    return "native/unmapped";
}

static int valid_frame(uintptr_t address)
{
#ifdef MEMORIES_TRANSLATED
    uintptr_t low, high;
    if (Memories_NativeStateStackRange(&low, &high) && address >= low &&
        address <= high && high - address >= 2 * sizeof(uintptr_t)) return 1;
#endif
    return (address >= GAME_STACK_LOW && address + 2 * sizeof(uintptr_t) <= GAME_STACK_HIGH) ||
           (address >= main_stack_low && address + 2 * sizeof(uintptr_t) <= main_stack_high);
}

static void symbol_line(int index, uintptr_t address)
{
    uintptr_t offset = 0;
    const char *name = Symbols_Lookup(address, &offset);
    char text[256];
    int length;
#ifdef _WIN32
    char module[64];
    if (!name && Win32_ModuleName(address, module, sizeof(module), &offset)) name = module;
#endif
#if defined(_WIN32) && defined(__x86_64__)
    if (name) length = snprintf(text, sizeof(text), "  #%d 0x" ADDRESS " %s+0x%" PRIxPTR "\n", index, address, name,
                                offset);
    else length = snprintf(text, sizeof(text), "  #%d 0x" ADDRESS "\n", index, address);
#else
    if (name) length = snprintf(text, sizeof(text), "  #%d 0x%08lx %s+0x%lx\n",
                                index, (unsigned long)address, name, (unsigned long)offset);
    else length = snprintf(text, sizeof(text), "  #%d 0x%08lx\n", index, (unsigned long)address);
#endif
    if (length > 0) output(text, (size_t)length);
}

#if defined(_WIN32) && defined(__x86_64__)
/* x86-64 code keeps no frame chain to follow: the callers come from the
 * unwind tables (win32.c), symbolized as on 32-bit. */
#define WALK(pc, sp, fp) walk_unwound(pc, sp, fp)
static void walk_unwound(uintptr_t pc, uintptr_t sp, uintptr_t fp)
{
    uintptr_t callers[31];
    int count = Win32_UnwindCallers(pc, sp, fp, callers, 31), i;
    (void)valid_frame; /* the frame-pointer walk's */
    symbol_line(0, pc);
    for (i = 0; i < count; i++) symbol_line(i + 1, callers[i]);
}

/* Where the report function that calls this was called from, and its
 * callers. */
__attribute__((noinline)) static void walk_current(void)
{
    uintptr_t callers[32];
    int count = Win32_CurrentCallers(2, callers, 32), i;
    for (i = 0; i < count; i++) symbol_line(i, callers[i]);
}
#else
#define WALK(pc, sp, fp) walk(pc, fp)
static void walk(uintptr_t eip, uintptr_t ebp)
{
    int depth = 0;
#if defined(__aarch64__)
    int crossed = 0;
#endif
    symbol_line(depth++, eip);
    while (depth < 32 && valid_frame(ebp)) {
        const uintptr_t *frame = (const uintptr_t *)ebp;
        uintptr_t next = frame[0], return_address = frame[1];
        if (!return_address) break;
        symbol_line(depth++, return_address);
#if defined(__aarch64__)
        /* A chain only climbs, but once: Memories_CallOnStack (Android)
         * runs a call on the thread's own stack and leaves its frame record
         * on the game stack, below, so from a crash inside a present the
         * walk goes on there to the game's callers. */
        if (!valid_frame(next)) break;
        if (next <= ebp) {
            if (crossed || next < GAME_STACK_LOW || next >= GAME_STACK_HIGH ||
                (ebp >= GAME_STACK_LOW && ebp < GAME_STACK_HIGH))
                break;
            crossed = 1;
        }
#else
        if (next <= ebp || !valid_frame(next)) break;
#endif
        ebp = next;
    }
}
#endif

char Crash_ReportDir[512] = "tmp/pc";

void Crash_ChooseReportDir(void)
{
    struct stat info;
    if (!stat("tmp/pc", &info) && S_ISDIR(info.st_mode)) return;   /* running from a checkout */
    if (Paths_User(Crash_ReportDir, sizeof(Crash_ReportDir), "reports") || Paths_MakeDirs(Crash_ReportDir)) {
        snprintf(Crash_ReportDir, sizeof(Crash_ReportDir), ".");
    }
}

static void open_report(void)
{
    MonitorShared *shared = Monitor_Shared();
    char path[640];
    snprintf(path, sizeof(path), "%s/crash-%ld.txt", Crash_ReportDir, (long)getpid());
    report_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (report_fd >= 0) {
        memcpy(shared->report_path, path, sizeof(shared->report_path));
        shared->reported = 1;
    }
}

/* What the game knew: the system and its last log lines. With a monitor
 * those come in its section of the same file, with more of the log. */
static void context(void)
{
    const char *tail_lines[32];
    int count, i;
    if (Monitor_Active()) {
        static const char note[] = "(the system, the log and the console output follow in the monitor's section)\n";
        output(note, sizeof(note) - 1);
        return;
    }
    output(Monitor_Shared()->facts, strnlen(Monitor_Shared()->facts, sizeof(Monitor_Shared()->facts)));
    count = Log_Tail(32, tail_lines);
    if (count) output("log tail:\n", 10);
    for (i = 0; i < count; i++) {
        output(tail_lines[i], strnlen(tail_lines[i], MONITOR_LINE_SIZE));
        if (!strchr(tail_lines[i], '\n')) output("\n", 1);
    }
}

/* `what` names the kind of number: a signal, or a Windows exception code. */
static void report_fatal(const char *what, unsigned long number, uintptr_t fault, uintptr_t eip, uintptr_t esp,
                         uintptr_t ebp)
{
    char text[128];
    open_report();
    snprintf(text, sizeof(text), strcmp(what, "signal") ? "fatal %s 0x%08lx" : "fatal %s %lu", what, number);
    line("memories-pc: %s at 0x" ADDRESS " (%s)\n", (uintptr_t)text, fault, (uintptr_t)region(fault));
    line(REGISTERS, eip, esp, ebp);
#if defined(__aarch64__)
    line("           LR=0x%016lx\n", fault_lr, 0, 0);
#endif
    WALK(eip, esp, ebp);
#ifdef __APPLE__
    uintptr_t text_first, text_last;
    if (DarwinImage_TextRange(&text_first, &text_last))
        line("Mach-O text: 0x%lx..0x%lx (PC offset 0x%lx)\n", text_first, text_last,
             eip >= text_first && eip < text_last ? eip - text_first : 0);
#endif
#ifdef __APPLE__
    line("Mach-O UUID: %s\n", (uintptr_t)DarwinImage_UUID(), 0, 0);
#endif
    line("frame=%lu vblank=%lu clock=%ld%%\n", Memories_PresentedFrames(), Platform_VBlankCount(),
         (uintptr_t)(long)Platform_ClockRate());
    line("last loaded state slot=%ld\n", (uintptr_t)(long)Memories_LastStateSlot(), 0, 0);
#ifdef MEMORIES_TRANSLATED
    extern const char Memories_BuildIdentity[];
    line("build: %s\n", (uintptr_t)Memories_BuildIdentity, 0, 0);
    const char *detail = GuestRuntime_FatalDetail();
    if (*detail) {
        output("translated runtime: ", 20);
        output(detail, strnlen(detail, 512));
        output("\n", 1);
    }
#endif
    context();
    if (report_fd >= 0) close(report_fd);
    report_fd = -1;
}

#ifdef _WIN32
static void report_exception(unsigned long code, uintptr_t fault, uintptr_t eip, uintptr_t esp, uintptr_t ebp)
{
    if (reporting++) return;
    report_fatal("exception", code, fault, eip, esp, ebp);
}

/* abort() ends the process without an exception the handlers above would
 * see. */
static void on_abort(int number)
{
    (void)number;
    if (reporting++) _exit(3);
    Crash_ReportFatal("abort()", "the C runtime was asked to abort");
    _exit(3);
}

/* A bad argument to a C runtime function: the function fails and returns,
 * as with MinGW's own handler, which this replaces; the game has always
 * gone on past them. Noted, so a report shows them. */
static void on_invalid_parameter(const wchar_t *expression, const wchar_t *function, const wchar_t *file,
                                 unsigned int at, uintptr_t reserved)
{
    static volatile long noted;
    (void)expression;
    (void)function;
    (void)file;
    (void)at;
    (void)reserved;
    if (__atomic_fetch_add(&noted, 1, __ATOMIC_RELAXED) < 8) {
        fprintf(stderr, "memories-pc: a C runtime function was given an invalid parameter; it failed (called from %p)\n",
                __builtin_return_address(0));
    }
}

void Crash_Init(void)
{
    Crash_ChooseReportDir();
    Win32_StackRange(&main_stack_low, &main_stack_high);
    Win32_SetCrashReporter(report_exception);
    signal(SIGABRT, on_abort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(on_invalid_parameter);
}
#elif defined(__SWITCH__)
/* Horizon has no signals: libnx hands a fault to the process itself, on a
 * stack of its own (these names are libnx's), and the process ends when the
 * handler returns. */
_Alignas(16) unsigned char __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
u32 __nx_exception_ignoredebug = 1; /* report under a debugger as well */

void __libnx_exception_handler(ThreadExceptionDump *dump)
{
    if (reporting++) return;
    fault_lr = (uintptr_t)dump->lr.x;
    report_fatal("exception", dump->error_desc, (uintptr_t)dump->far.x, (uintptr_t)dump->pc.x,
                 (uintptr_t)dump->sp.x, (uintptr_t)dump->fp.x);
}

void Crash_Init(void)
{
    MemoryInfo info;
    u32 page;
    Crash_ChooseReportDir();
    if (R_SUCCEEDED(svcQueryMemory(&info, &page, (u64)(uintptr_t)__builtin_frame_address(0)))) {
        main_stack_low = (uintptr_t)info.addr;
        main_stack_high = main_stack_low + info.size;
    }
}
#else
void Crash_HandleSignal(int number, siginfo_t *info, void *context)
{
    ucontext_t *user = context;
    struct sigaction action;
    if (reporting++) _exit(128 + number);
#if defined(__aarch64__)
#ifdef __APPLE__
    fault_lr = (uintptr_t)user->uc_mcontext->__ss.__lr;
#else
    fault_lr = (uintptr_t)user->uc_mcontext.regs[30];
#endif
#endif
    report_fatal("signal", (unsigned long)number, info ? (uintptr_t)info->si_addr : 0,
                 (uintptr_t)SIGNAL_CONTEXT_PC(user), (uintptr_t)SIGNAL_CONTEXT_SP(user),
                 (uintptr_t)SIGNAL_CONTEXT_FP(user));
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(number, &action, NULL);
    raise(number);
}

static void installed_handler(int number, siginfo_t *info, void *context)
{
    Crash_HandleSignal(number, info, context);
}

void Crash_Init(void)
{
    static const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    stack_t stack;
    struct sigaction action;
    pthread_attr_t attributes;
    void *address;
    size_t size;
    unsigned i;
    Crash_ChooseReportDir();
#ifdef __APPLE__
    uintptr_t text_first, text_last;
    DarwinImage_TextRange(&text_first, &text_last); /* populate cache outside handlers */
#endif
    stack.ss_sp = alternate_stack;
    stack.ss_size = sizeof(alternate_stack);
    stack.ss_flags = 0;
    sigaltstack(&stack, NULL);
#ifdef __APPLE__
    main_stack_high = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
    main_stack_low = main_stack_high - pthread_get_stacksize_np(pthread_self());
#else
    if (!pthread_getattr_np(pthread_self(), &attributes)) {
        if (!pthread_attr_getstack(&attributes, &address, &size)) {
            main_stack_low = (uintptr_t)address;
            main_stack_high = main_stack_low + size;
        }
        pthread_attr_destroy(&attributes);
    }
#endif
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = installed_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) sigaction(signals[i], &action, NULL);
}
#endif

void Crash_ReportSoft(const char *kind, const char *detail)
{
    const char *tail_lines[32];
    int count, i;
    fprintf(stderr, "memories-pc: %s: %s\n", kind, detail ? detail : "");
    fprintf(stderr, "frame=%u vblank=%u clock=%d%%\n", Memories_PresentedFrames(), Platform_VBlankCount(),
            Platform_ClockRate());
#if defined(_WIN32) && defined(__x86_64__)
    walk_current();
#else
    walk((uintptr_t)__builtin_return_address(0), (uintptr_t)__builtin_frame_address(0));
#endif
    count = Log_Tail(32, tail_lines);
    for (i = 0; i < count; i++) fprintf(stderr, "  %s%s", tail_lines[i], strchr(tail_lines[i], '\n') ? "" : "\n");
}

void Crash_ReportFatal(const char *kind, const char *detail)
{
    uintptr_t ebp = (uintptr_t)__builtin_frame_address(0);
    fflush(stdout);
    open_report();
    output("memories-pc: fatal: ", 20);
    output(kind, strlen(kind));
    if (detail) {
        output(": ", 2);
        output(detail, strlen(detail));
    }
    output("\n", 1);
#if defined(_WIN32) && defined(__x86_64__)
    (void)ebp;
    walk_current();
#else
    walk((uintptr_t)__builtin_return_address(0), ebp);
#endif
    line("frame=%lu vblank=%lu clock=%ld%%\n", Memories_PresentedFrames(), Platform_VBlankCount(),
         (uintptr_t)(long)Platform_ClockRate());
    line("last loaded state slot=%ld\n", (uintptr_t)(long)Memories_LastStateSlot(), 0, 0);
    context();
    if (report_fd >= 0) close(report_fd);
    report_fd = -1;
}

void Crash_ReportHang(void *context_pointer)
{
    char path[640];
    uintptr_t eip, esp, ebp;
#ifdef _WIN32
    Win32_ContextRegisters(context_pointer, &eip, &esp, &ebp);
#elif defined(__SWITCH__)
    (void)context_pointer; /* no watcher interrupts the game there: Monitor_Active */
    eip = esp = ebp = 0;
    fault_lr = 0;
#else
    ucontext_t *user = context_pointer;
    eip = context_pointer ? (uintptr_t)SIGNAL_CONTEXT_PC(user) : 0;
    esp = context_pointer ? (uintptr_t)SIGNAL_CONTEXT_SP(user) : 0;
    ebp = context_pointer ? (uintptr_t)SIGNAL_CONTEXT_FP(user) : 0;
#if defined(__aarch64__)
#ifdef __APPLE__
    fault_lr = context_pointer ? (uintptr_t)user->uc_mcontext->__ss.__lr : 0;
#else
    fault_lr = context_pointer ? (uintptr_t)user->uc_mcontext.regs[30] : 0;
#endif
#endif
#endif
    report_fd = -1;
    snprintf(path, sizeof(path), "%s/hang-%ld.txt", Crash_ReportDir, (long)getpid());
    report_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666); /* the monitor may have begun it */
    {
        static const char message[] = "memories-pc: no VSync for 5 s\n";
        output(message, sizeof(message) - 1);
        line(REGISTERS, eip, esp, ebp);
#if defined(__aarch64__)
        line("           LR=0x%016lx\n", fault_lr, 0, 0);
#endif
        WALK(eip, esp, ebp);
#ifdef __APPLE__
    uintptr_t text_first, text_last;
    if (DarwinImage_TextRange(&text_first, &text_last))
        line("Mach-O text: 0x%lx..0x%lx (PC offset 0x%lx)\n", text_first, text_last,
             eip >= text_first && eip < text_last ? eip - text_first : 0);
#endif
        line("frame=%lu vblank=%lu clock=%ld%%\n", Memories_PresentedFrames(), Platform_VBlankCount(),
             (uintptr_t)(long)Platform_ClockRate());
        context();
    }
    if (report_fd >= 0) close(report_fd);
    report_fd = -1;
}
