/* The Switch's side of platform.h where a desktop's does not fit
 * (android.c is Android's): GLES rather than desktop OpenGL, no file picker,
 * and a word on memory for a game started from the album. The SDL backend
 * runs over SDL2 there (src/pc/compat/switch/SDL3/sdl3_shim.h). */
#ifdef __SWITCH__
#include "platform.h"
#include "paths.h"
#include "pc/memory_map.h"
#include <switch.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* libnx's hook once its services are up, before main and before any
 * thread of the port's: the game's address ranges are kept from libnx. */
static void reserve_tokens(void);

void userAppInit(void)
{
    reserve_tokens();
}

/* The translated runtime tells the host's addresses below 4 GiB from the
 * game's by range (pc/memory_map.h): nothing it reaches may lie where the
 * game's tokens do. */
static const struct { u64 start, end; } tokens[] = {
    {0, MEMORIES_HOST_LOW_START}, {MEMORIES_HOST_LOW_END, 0x100000000ull}};

/* Kept from libnx's choices of address (thread stacks, which Horizon puts
 * in 0x8000000..0x80000000, and other mappings), before any is made. */
static void reserve_tokens(void)
{
    unsigned t;
    virtmemLock();
    for (t = 0; t < sizeof(tokens) / sizeof(*tokens); t++)
        virtmemAddReservation((void *)(uintptr_t)tokens[t].start, (size_t)(tokens[t].end - tokens[t].start));
    virtmemUnlock();
}

/* The .nro's own image: crt0's entry starts it (switch.ld's __start__ is
 * absolute, not moved with the image), __end__ ends it. */
extern char _start[], __end__[];

/* The stack the loader started main on, once Switch_Main has left it. */
static u64 left_stack;

/* Pages only libnx or another module reads, which the game never reaches:
 * thread-local pages, the code of a module other than the .nro (the
 * homebrew loader's), and the stack main was started on once left. */
static int ignored(const MemoryInfo *info)
{
    if (left_stack && left_stack >= info->addr && left_stack < info->addr + info->size) return 1;
    switch (info->type) {
    case MemType_Unmapped:
    case MemType_Reserved:
    case MemType_ThreadLocal:
        return 1;
    case MemType_CodeStatic:
    case MemType_CodeMutable:
    case MemType_ModuleCodeStatic:
    case MemType_ModuleCodeMutable:
        return info->addr >= (u64)(uintptr_t)__end__ || info->addr + info->size <= (u64)(uintptr_t)_start;
    default:
        return 0;
    }
}

/* What the kernel or the loader mapped before reserve_tokens could keep it
 * away: the .nro, the main thread's stack, the heap. 1 with `why` on a
 * clash, which a new start (a new layout) resolves. */
static int check_layout(char *why, size_t size)
{
    u64 address = 0;
    while (address < 0x100000000ull) {
        MemoryInfo info;
        u32 page;
        unsigned t;
        if (R_FAILED(svcQueryMemory(&info, &page, address)) || !info.size) break;
        if (!ignored(&info))
            for (t = 0; t < sizeof(tokens) / sizeof(*tokens); t++)
                if (info.addr < tokens[t].end && info.addr + info.size > tokens[t].start) {
                    fprintf(stderr, "memories-pc: memory 0x%lx..0x%lx (type 0x%x) is in the game's range\n",
                            (unsigned long)info.addr, (unsigned long)(info.addr + info.size), (unsigned)info.type);
                    snprintf(why, size, "This start of the console placed the game's memory where Forbidden "
                                        "Memories' own addresses go.\n\nClose it and start it again: the layout "
                                        "changes on every start.");
                    return 1;
                }
        if (info.addr + info.size <= address) break;
        address = info.addr + info.size;
    }
    return 0;
}

/* run(argc, argv) with the stack pointer at `top`, then back. */
int Switch_CallOnStack(int (*run)(int, char **), int argc, char **argv, void *top);
__asm__(".text\n"
        ".p2align 2\n"
        ".type Switch_CallOnStack, %function\n"
        "Switch_CallOnStack:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov sp, x3\n"
        "    mov x9, x0\n"
        "    mov x0, x1\n"
        "    mov x1, x2\n"
        "    blr x9\n"
        "    mov sp, x29\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        ".size Switch_CallOnStack, . - Switch_CallOnStack\n");

int Switch_ThreadStack(void **end, size_t *size);

static int in_tokens(u64 address)
{
    unsigned t;
    for (t = 0; t < sizeof(tokens) / sizeof(*tokens); t++)
        if (address >= tokens[t].start && address < tokens[t].end) return 1;
    return 0;
}

/* main's body. The loader starts main on a stack the kernel placed before
 * reserve_tokens ran, and it may lie where the game's tokens do: then the
 * body runs on a stack of the same size mapped as libnx maps a thread's,
 * where the reservations keep it in the host's window. Its own mapping, so
 * Switch_ThreadStack still finds just the stack. */
int Switch_Main(int (*run)(int, char **), int argc, char **argv)
{
    void *end, *memory, *stack;
    size_t size;
    if (!in_tokens((u64)(uintptr_t)__builtin_frame_address(0))) return run(argc, argv);
    if (Switch_ThreadStack(&end, &size)) size = 0;
    if (size < 0x100000) size = 0x100000;
    size = (size + 0xfff) & ~(size_t)0xfff;
    memory = aligned_alloc(0x1000, size);
    if (!memory) return run(argc, argv); /* check_layout says why it stops */
    virtmemLock();
    stack = virtmemFindStack(size, 0x4000);
    if (!stack || R_FAILED(svcMapMemory(stack, memory, size))) stack = NULL;
    virtmemUnlock();
    if (!stack) return run(argc, argv);
    left_stack = (u64)(uintptr_t)__builtin_frame_address(0);
    return Switch_CallOnStack(run, argc, argv, (char *)stack + size);
}

/* The folders the player fills, made on the first start so that the .nro
 * alone in sdmc:/switch is a complete install, with a word on the disc. */
static void make_folders(void)
{
    FILE *note;
    Paths_MakeDirs(PATHS_SWITCH_DIR "/game");
    Paths_MakeDirs(PATHS_SWITCH_DIR "/mods");
    if (access(PATHS_SWITCH_DIR "/game/README.txt", F_OK) == 0) return;
    note = fopen(PATHS_SWITCH_DIR "/game/README.txt", "w");
    if (!note) return;
    fputs("Put your raw image of Yu-Gi-Oh! Forbidden Memories (USA, SLUS-01411) here:\n"
          "the .bin file of a .bin/.cue pair. Any file name ending in .bin will do.\n"
          "Mods go in ../mods, one folder per mod.\n",
          note);
    fclose(note);
}

/* Before anything else in main: the player's folders (make_folders), the
 * console's output (stderr: every message the port has before it has a
 * window) to nxlink's terminal when started with `nxlink -s`, otherwise to
 * log.txt in PATHS_SWITCH_DIR; then check_layout. */
int Switch_EarlyInit(char *why, size_t size)
{
    make_folders();
    if (__nxlink_host.s_addr && R_SUCCEEDED(socketInitializeDefault()) && nxlinkStdio() >= 0) {
        /* stdout and stderr go to the terminal */
    } else {
        freopen(PATHS_SWITCH_DIR "/log.txt", "w", stdout);
        dup2(fileno(stdout), fileno(stderr));
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    return check_layout(why, size);
}

/* Mesa's GLES through SDL's renderer, as on Android: sdl.c shows the
 * picture with it, and present_pass.c's desktop GL is not linked
 * (gl_desktop_none.c stands in). */
int Platform_HasDesktopGL(void) { return 0; }

/* No file picker: the disc is found in game/ beside the .nro
 * (PATHS_SWITCH_DIR), and GameFiles_Disc's message says so when it is not
 * there. 0: nothing chosen. */
int Platform_SelectDisc(char *path, size_t size, char *why, size_t why_size)
{
    (void)path;
    (void)size;
    (void)why;
    (void)why_size;
    return 0;
}

/* The calling thread's stack, for the translated runtime
 * (translated_image_backend.c): libnx has no pthread_getattr_np, so the
 * memory mapping the caller's frame is in. */
int Switch_ThreadStack(void **end, size_t *size)
{
    MemoryInfo info;
    u32 page;
    if (R_FAILED(svcQueryMemory(&info, &page, (u64)(uintptr_t)__builtin_frame_address(0)))) return -1;
    *end = (void *)(uintptr_t)(info.addr + info.size);
    *size = (size_t)info.size;
    return 0;
}

/* A homebrew started from the album runs as an applet, with a few hundred
 * MiB; started in place of a game (holding R as it starts) it has the
 * game's memory. */
int Platform_GuestMemoryHelp(char *why, size_t size)
{
    const AppletType type = appletGetAppletType();
    if (type == AppletType_Application || type == AppletType_SystemApplication) return 0;
    snprintf(why, size,
             "Not enough memory: the game was started from the album, which leaves homebrew only a few "
             "hundred MiB.\n\nStart it in place of a game instead: hold R while you start any game, then "
             "open the Homebrew Menu and choose Forbidden Memories.");
    return 1;
}
#endif
