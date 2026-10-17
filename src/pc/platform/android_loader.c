/* Android: libmain.so, the library SDL's Java shell loads and whose SDL_main
 * it runs, is only this loader, and the game is libgame.so.
 *
 * The arm64 app's game is the translated build (MEMORIES_ANDROID_TRANSLATED,
 * set by build_game32.py): linked at 0, it holds no host address and is
 * opened wherever the system puts it, with nothing reserved; its save states
 * name it by its build ID (translated_state_backend.c). Its guest memory
 * needs no fixed host range either, so a phone whose Java runtime holds
 * 0x80000000 or 0xC0000000 starts it all the same. A library that will not
 * load ends in the system's box with the device line, as below.
 *
 * The fixed game (build_game32.py --android-memory fixed, and android-x86)
 * is linked at a fixed base (MEMORIES_ANDROID_GAME_BASE), and loaded here
 * into a range reserved at that very address, so it sits where it was
 * linked on every launch: the addresses a save state holds (return
 * addresses on the game stack, callbacks in guest RAM, the game's variables)
 * and the build's symbol tables (crash reports) mean the same thing from one
 * launch to the next, as they do for the desktop executables. The system
 * would otherwise load it at a different address each time.
 *
 * If the range cannot be had, the 64-bit game is not loaded at all (its
 * function addresses go into 4-byte guest slots: anywhere else it would run
 * broken), and the player is told so in a message box; the 32-bit one is
 * loaded where the system chooses and told so (MEMORIES_ANDROID_LOAD_BIAS):
 * it runs, but its save states cannot be carried to another launch
 * (notes/pc-build.md, "Android"). A failure ends the process: the app does
 * not just flash and close. */
#ifdef __ANDROID__
#include "pc/compat/fs.h" /* setenv, as every unit that names a file or a variable */
#ifndef MEMORIES_ANDROID_TRANSLATED
#include "pc/compat/mman.h" /* <sys/mman.h> and Memories_MapFixed */
#include <android/dlext.h>
#endif
#include <android/log.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/system_properties.h>
#include <sys/utsname.h>
#include <unistd.h>

#if !defined(MEMORIES_ANDROID_TRANSLATED) && !defined(MEMORIES_ANDROID_GAME_BASE)
#error "build_game32.py defines MEMORIES_ANDROID_GAME_BASE and MEMORIES_ANDROID_GAME_SPAN"
#endif

#define LOG_TAG "memories"

typedef int (*GameMain)(int argc, char **argv);

static void say(const char *text)
{
    __android_log_write(ANDROID_LOG_INFO, LOG_TAG, text);
}

#ifndef MEMORIES_ANDROID_TRANSLATED
/* What holds the part of [low, high) that the reservation could not have:
 * to the log, and the first few, as "start-end name", to `held` for the
 * message the player sees (a screenshot of it is the report). */
static void say_occupants(uintptr_t low, uintptr_t high, char *held, size_t size)
{
    char text[512], line[600];
    unsigned long start, end;
    size_t used = 0;
    int name = 0, shown = 0;
    FILE *maps = fopen("/proc/self/maps", "r");
    held[0] = '\0';
    if (!maps) return;
    while (fgets(text, sizeof(text), maps)) {
        name = 0;
        if (sscanf(text, "%lx-%lx %*s %*s %*s %*s %n", &start, &end, &name) < 2 || start >= high || end <= low)
            continue;
        text[strcspn(text, "\n")] = 0;
        snprintf(line, sizeof(line), "memories-pc: occupied by %s", text);
        say(line);
        if (shown++ < 2 && used + 1 < size) {
            int added = snprintf(held + used, size - used, "%lx-%lx %s\n", start, end,
                                 name > 0 && text[name] ? text + name : "(no name)");
            if (added > 0) used = used + (size_t)added < size ? used + (size_t)added : size - 1;
        }
    }
    fclose(maps);
}
#endif

/* "Xiaomi Redmi Note 8 Pro, Android 10 (API 29), Linux 4.14.141" */
static void device_line(char *out, size_t size)
{
    char maker[PROP_VALUE_MAX] = "", model[PROP_VALUE_MAX] = "", release[PROP_VALUE_MAX] = "", api[PROP_VALUE_MAX] = "";
    struct utsname name;
    __system_property_get("ro.product.manufacturer", maker);
    __system_property_get("ro.product.model", model);
    __system_property_get("ro.build.version.release", release);
    __system_property_get("ro.build.version.sdk", api);
    if (uname(&name)) snprintf(name.release, sizeof(name.release), "?");
    name.release[strcspn(name.release, "-")] = '\0'; /* the version, without the build's suffix */
    snprintf(out, size, "%s %s, Android %s (API %s), Linux %s", maker, model, release, api, name.release);
}

/* A failure the player sees: the log, then SDL's message box (libSDL3.so,
 * which SDL's Java shell loaded before this library), then the process
 * ends, so the next launch starts afresh. */
static void fail(const char *text)
{
    typedef int (*ShowBox)(unsigned flags, const char *title, const char *message, void *window);
    void *sdl = dlopen("libSDL3.so", RTLD_NOW | RTLD_NOLOAD);
    ShowBox show = sdl ? (ShowBox)dlsym(sdl, "SDL_ShowSimpleMessageBox") : NULL;
    say(text);
    if (show) show(0x10u /* SDL_MESSAGEBOX_ERROR */, "YFM Re-Decomp", text, NULL);
    _exit(1); /* not exit(): see end_process in android.c */
}

/* libgame.so beside this library (the app's native library folder). */
static int game_path(char *out, size_t size)
{
    Dl_info info;
    const char *slash;
    if (!dladdr((void *)game_path, &info) || !info.dli_fname || !(slash = strrchr(info.dli_fname, '/'))) return -1;
    return snprintf(out, size, "%.*s/libgame.so", (int)(slash - info.dli_fname), info.dli_fname) < (int)size ? 0 : -1;
}

#ifdef MEMORIES_ANDROID_TRANSLATED
int SDL_main(int argc, char **argv)
{
    char path[1024], line[1500], device[300];
    void *game;
    GameMain run;
    Dl_info info;
    if (game_path(path, sizeof(path))) snprintf(path, sizeof(path), "libgame.so");
    if (!(game = dlopen(path, RTLD_NOW))) {
        device_line(device, sizeof(device));
        snprintf(line, sizeof(line), "The game could not start: cannot load %s: %s\n\n%s", path, dlerror(), device);
        fail(line);
    }
    run = (GameMain)dlsym(game, "Memories_AndroidMain");
    if (!run) fail("The game could not start: libgame.so has no Memories_AndroidMain");
    if (dladdr((void *)run, &info)) {
        snprintf(line, sizeof(line), "memories-pc: libgame.so (translated) loaded at %p", info.dli_fbase);
        say(line);
    }
    /* As below: Memories_AndroidMain ends the process itself. */
    exit(run(argc, argv));
}
#else
int SDL_main(int argc, char **argv)
{
    char path[1024], line[1200];
    void *base = (void *)(uintptr_t)MEMORIES_ANDROID_GAME_BASE, *reserved, *game = NULL;
    GameMain run;
    long bias = 0;
    if (game_path(path, sizeof(path))) snprintf(path, sizeof(path), "libgame.so");
    /* MAP_FIXED_NOREPLACE: without it the address is only a hint, which
     * the kernel passes over when anything holds part of the range (the JIT
     * caches did, at the first base, on a phone); kernels before 4.17 ignore
     * the flag and take the hint, which Memories_MapFixed undoes. */
    reserved = Memories_MapFixed(base, MEMORIES_ANDROID_GAME_SPAN, PROT_NONE,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (reserved == base) {
        android_dlextinfo extinfo;
        memset(&extinfo, 0, sizeof(extinfo));
        extinfo.flags = ANDROID_DLEXT_RESERVED_ADDRESS;
        extinfo.reserved_addr = base;
        extinfo.reserved_size = MEMORIES_ANDROID_GAME_SPAN;
        game = android_dlopen_ext(path, RTLD_NOW, &extinfo);
        if (!game) {
            snprintf(line, sizeof(line), "The game could not start: loading %s at %p failed: %s", path, base,
                     dlerror());
#if UINTPTR_MAX > 0xffffffffu
            fail(line);
#else
            say(line);
            munmap(reserved, MEMORIES_ANDROID_GAME_SPAN);
#endif
        }
    } else {
        char held[400], device[300];
        snprintf(line, sizeof(line), "memories-pc: the game's address range at %p is taken (%s)", base,
                 strerror(errno));
        say(line);
        say_occupants((uintptr_t)base, (uintptr_t)base + MEMORIES_ANDROID_GAME_SPAN, held, sizeof(held));
        device_line(device, sizeof(device));
        snprintf(line, sizeof(line), "memories-pc: %s", device);
        say(line);
#if UINTPTR_MAX > 0xffffffffu
        /* Refused before anything of the game is loaded: elsewhere its
         * initialisers would run at an address it cannot work at. Short:
         * the system's box does not scroll in landscape, and its button
         * sits under the text. */
        snprintf(line, sizeof(line), "The game could not start: this phone already uses memory it needs (%p-%p). "
                 "Please send a screenshot of this to us on Discord or GitHub.\n\n%s\nHeld by %s", base,
                 (void *)((uintptr_t)base + MEMORIES_ANDROID_GAME_SPAN), device,
                 held[0] ? held : "nothing found there now");
        fail(line);
#endif
    }
    if (!game && !(game = dlopen(path, RTLD_NOW))) {
        snprintf(line, sizeof(line), "The game could not start: cannot load %s: %s", path, dlerror());
        fail(line);
    }
    run = (GameMain)dlsym(game, "Memories_AndroidMain");
    if (!run) fail("The game could not start: libgame.so has no Memories_AndroidMain");
    {
        /* Where it went, against where it was linked: 0 when the
         * reservation held. */
        Dl_info info;
        if (dladdr((void *)run, &info)) bias = (long)((uintptr_t)info.dli_fbase - (uintptr_t)base);
    }
#if UINTPTR_MAX > 0xffffffffu
    /* 64-bit: the game's function addresses go into 4-byte guest slots, so
     * anywhere but its link address (below 4 GB) it would run broken. */
    if (bias) {
        snprintf(line, sizeof(line), "The game could not start: libgame.so was loaded %ld bytes from its link "
                 "address %p; the 64-bit game needs it there", bias, base);
        fail(line);
    }
#endif
    snprintf(line, sizeof(line), "%ld", bias);
    setenv("MEMORIES_ANDROID_LOAD_BIAS", line, 1);
    /* Memories_AndroidMain ends the process itself (exit(main(...)), through
     * __wrap_exit in android.c), so the next launch starts afresh instead
     * of SDL's Java shell finding a finished main in a live process. This
     * exit is for one that returns anyway: it reaches end_process, the
     * first atexit handler Memories_AndroidMain registers, which ends the
     * process before the destructors of the system libraries loaded before
     * it run under the activity's live threads (those of a library loaded
     * later, such as one SDL opens during main, run first). */
    exit(run(argc, argv));
}
#endif
#endif
