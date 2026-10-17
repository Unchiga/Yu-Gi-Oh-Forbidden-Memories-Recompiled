/* A test harness for a phone over adb: run the Android build's libgame.so
 * as a plain process (adb shell), headless, with no app around it. Loads
 * libSDL3.so (which libgame.so needs) and libgame.so at the address the
 * build linked it at (the same reservation android_loader.c makes in the
 * app), then calls the port's own main: the caller sets the environment a
 * desktop run would have (MEMORIES_DISC, MEMORIES_HEADLESS, a replay's
 * MEMORIES_PLAY/MEMORIES_RECORD). tools/pc/android/device_run.py builds
 * it (GAME_BASE and GAME_SPAN from libgame.so's LOAD segments, 0xC0000000
 * and 64 MiB today) and drives it:
 *
 *   clang --target=aarch64-linux-android24 -fPIE -pie -O2 runner.c -ldl \
 *       -DGAME_BASE=0xc0000000 -DGAME_SPAN=0x04000000 -o runner */
#include <android/dlext.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
    const char *dir = getenv("MEMORIES_RUNNER_LIBS");
    char path[512];
    void *base = (void *)(uintptr_t)GAME_BASE, *reserved, *game;
    int (*game_main)(int, char **);
    android_dlextinfo extinfo;
    if (!dir) dir = ".";
    snprintf(path, sizeof(path), "%s/libSDL3.so", dir);
    if (!dlopen(path, RTLD_NOW | RTLD_GLOBAL)) {
        fprintf(stderr, "runner: %s\n", dlerror());
        return 1;
    }
    if (getenv("MEMORIES_RUNNER_HOLD_LOW")) {
        /* A stand-in for ART's large object space on a CC-collector phone
         * (notes/pc-build.md, "Android arm64"): 512 MiB at
         * 0x7C000000-0x9C000000, over guest RAM (0x80000000) and 0x90000000,
         * held before the game loads. The translated game runs under it;
         * the fixed one cannot map its guest memory. */
        void *held = mmap((void *)(uintptr_t)0x7C000000u, 0x20000000u, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        fprintf(stderr, "runner: held 0x7c000000-0x9c000000: %s\n",
                held == (void *)(uintptr_t)0x7C000000u ? "yes" : "no");
    }
    snprintf(path, sizeof(path), "%s/libgame.so", dir);
#if GAME_BASE == 0
    /* The translated game (build_arm64.py --target android-arm64-translated)
     * is linked at 0 and loads wherever the system puts it. */
    (void)reserved;
    (void)extinfo;
    game = dlopen(path, RTLD_NOW);
#else
    reserved = mmap(base, GAME_SPAN, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reserved != base) {
        fprintf(stderr, "runner: the game's range at %p is taken (got %p)\n", base, reserved);
        return 1;
    }
    memset(&extinfo, 0, sizeof(extinfo));
    extinfo.flags = ANDROID_DLEXT_RESERVED_ADDRESS;
    extinfo.reserved_addr = base;
    extinfo.reserved_size = GAME_SPAN;
    game = android_dlopen_ext(path, RTLD_NOW, &extinfo);
#endif
    if (!game) {
        fprintf(stderr, "runner: %s\n", dlerror());
        return 1;
    }
    game_main = (int (*)(int, char **))dlsym(game, "main");
    if (!game_main) {
        fprintf(stderr, "runner: no main in libgame.so\n");
        return 1;
    }
    {
        Dl_info info;
        if (dladdr((void *)game_main, &info))
            fprintf(stderr, "runner: libgame.so at %p (linked at %p)\n", info.dli_fbase, base);
    }
    {
        int status = game_main(argc, argv);
        fprintf(stderr, "runner: main returned %d\n", status);
        fflush(stderr);
        return status;
    }
}
