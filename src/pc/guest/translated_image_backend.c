#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#elif !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* pthread_getattr_np (Android) */
#endif
#include "pc/compat/fs.h"
#include "../../types.h"
#include "image.h"
#include "mips.h"
#include "translated_image.h"
#include "translated_runtime.h"
#include "function_map.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#if defined(MEMORIES_TEST_HOOKS) && defined(__linux__)
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#endif

static MemoriesMemory *guest_memory;
int Memories_ScratchpadRetailView = 1;
extern void GuestRuntime_RegisterAllGlobals(void);
void *(*Memories_GuestBranchResolver)(unsigned);

static void *resolve_function(u32 address)
{
    const MemoriesGuestFunction *entry = Memories_FindResidentFunction(address);
    if (entry) return (void *)(uintptr_t)entry->host;
    if (Memories_MipsInOverlay(address)) {
        Memories_MipsThunkTarget = address;
        return (void *)(uintptr_t)Memories_MipsThunk;
    }
    return NULL;
}
static void *resolve_branch(unsigned address)
{
    return GuestRuntime_ResolveFunction((void *)(uintptr_t)address);
}
/* Guest memory is ordinary heap memory here, at no fixed address: nothing
 * else in the process can hold it, and no kernel flag matters. What failed,
 * if anything, is said for the start-up report (Android's
 * Platform_GuestMemoryHelp, as for the fixed game's image.c). */
static char map_error[160];
const char *Memories_GuestMapError(void) { return map_error; }
const char *Memories_GuestMapOccupants(void) { return ""; }
int Memories_KernelRefusesTakenRange(void) { return -1; }
static int map_failed(const char *why)
{
    snprintf(map_error, sizeof(map_error), "%s", why);
    fprintf(stderr, "memories-pc: guest memory: %s\n", why);
    return -1;
}
#if defined(MEMORIES_TEST_HOOKS) && defined(__linux__)
/* MEMORIES_TEST_HOLD_GUEST=1 (or an address, or several separated by
 * commas): as image.c's for the fixed game, 2 MiB of host memory held at
 * each (0x80000000 for 1) and named as ART's large object space, which
 * holds guest RAM on some phones (notes/pc-build.md, "Android arm64"). The
 * translated game needs no host address there, so it starts all the same:
 * this shows it. "0x80000000,0x9C000000,0x9E000000,0x9FF00000" also holds
 * the text arena, low memory and the interpreter stack, which
 * compat/mman.h's fixed requests take elsewhere (translated_mman.c). An
 * entry START-END holds that whole range instead, as a phone's runtime
 * can: 0x762fd000-0x962fd000 is the large object space of #372 (a Retroid
 * Pocket Nova, Android 13), across guest RAM's 0x80000000.
 * <sys/mman.h>'s own mmap, which compat/mman.h does not reach here. A test
 * hook: not in a release. */
static void hold_guest(void)
{
    const char *value = getenv("MEMORIES_TEST_HOLD_GUEST");
    char *next;
    if (!value || !*value || !strcmp(value, "0")) return;
    for (;;) {
        unsigned long at = strtoul(value, &next, 0), size = 0x200000ul;
        void *wanted, *got;
        if (next == value) break;
        if (at <= 1) at = 0x80000000ul;
        if (*next == '-') {   /* START-END: the whole range */
            unsigned long end = strtoul(next + 1, &next, 0);
            if (end > at) size = ((end - (at & ~0xFFFul)) + 0xFFFul) & ~0xFFFul;
        }
        wanted = (void *)(uintptr_t)(at & ~0xFFFul);
        got = mmap(wanted, size, PROT_NONE, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (got != wanted && size > 0x200000ul) {
            /* Part of the range is taken already (an emulator's own runtime
             * may sit there): every free 2 MiB piece is held instead, so
             * none of it is the game's either way. */
            unsigned long piece, held = 0;
            if (got != MAP_FAILED) munmap(got, size);
            for (piece = 0; piece + 0x200000ul <= size; piece += 0x200000ul) {
                char *part = (char *)wanted + piece;
                void *one = mmap(part, 0x200000, PROT_NONE, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (one == (void *)part) {
                    prctl(0x53564d41, 0, (unsigned long)(uintptr_t)one, 0x200000ul,
                          (unsigned long)(uintptr_t)"dalvik-free list large object space (MEMORIES_TEST_HOLD_GUEST)");
                    held += 0x200000ul;
                } else if (one != MAP_FAILED) {
                    munmap(one, 0x200000);
                }
            }
            fprintf(stderr, "memories-pc: %p-%p: %lu of %lu MiB held (MEMORIES_TEST_HOLD_GUEST), the rest was "
                    "taken already; the translated game maps nothing there\n", wanted, (void *)((char *)wanted + size),
                    held >> 20, size >> 20);
        } else if (got != wanted) {
            if (got != MAP_FAILED) munmap(got, size);
            fprintf(stderr, "memories-pc: MEMORIES_TEST_HOLD_GUEST: %p could not be held\n", wanted);
        } else {
            /* PR_SET_VMA, PR_SET_VMA_ANON_NAME: Android kernels, Linux 5.17+. */
            prctl(0x53564d41, 0, (unsigned long)(uintptr_t)got, size,
                  (unsigned long)(uintptr_t)"dalvik-free list large object space (MEMORIES_TEST_HOLD_GUEST)");
            fprintf(stderr, "memories-pc: %p-%p is held (MEMORIES_TEST_HOLD_GUEST); the translated game maps "
                    "nothing there\n", got, (void *)((char *)got + size));
        }
        if (*next != ',') break;
        value = next + 1;
    }
}
#endif

int Memories_GuestMap(void)
{
    unsigned i;
    void *stack_end;
    size_t stack_size;
#if defined(MEMORIES_TEST_HOOKS) && defined(__linux__)
    hold_guest();
#endif
    guest_memory = calloc(1, sizeof(*guest_memory));
    if (!guest_memory) return map_failed("no memory for the game's RAM");
    if (GuestRuntime_Bind(guest_memory)) return map_failed("the translated runtime could not take the game's RAM");
#ifdef __APPLE__
    stack_end = pthread_get_stackaddr_np(pthread_self());
    stack_size = pthread_get_stacksize_np(pthread_self());
#else
    {
        pthread_attr_t attributes;
        void *low = NULL;
        stack_size = 0;
        if (!pthread_getattr_np(pthread_self(), &attributes)) {
            if (pthread_attr_getstack(&attributes, &low, &stack_size)) stack_size = 0;
            pthread_attr_destroy(&attributes);
        }
        stack_end = low ? (u8 *)low + stack_size : NULL;
    }
#endif
    if (!stack_end || !stack_size || GuestRuntime_RegisterData((u8 *)stack_end - stack_size,
                                                              stack_size, 0xe0000000u))
        return map_failed("the main thread's stack could not be found");
    for (i = 0; i < Memories_FunctionMapCount; ++i) {
        const MemoriesGuestFunction *entry = &Memories_FunctionMap[i];
        unsigned previous;
        for (previous = 0; previous < i; ++previous)
            if (Memories_FunctionMap[previous].guest == entry->guest &&
                Memories_FunctionMap[previous].host == entry->host) break;
        if (previous < i) continue;
        if (GuestRuntime_RegisterFunction(entry->guest, entry->host)) {
            fprintf(stderr, "translated runtime: cannot register guest function 0x%08x\n", entry->guest);
            return map_failed("the game's functions could not be registered");
        }
    }
    GuestRuntime_RegisterAllGlobals();
    GuestRuntime_SetFunctionResolver(resolve_function);
    Memories_GuestBranchResolver = resolve_branch;
    return 0;
}
int Memories_GuestLoadExeData(const unsigned char *data, size_t size, const char *name)
{
    u32 entry;
    if (Memories_LoadTranslatedExe(guest_memory, data, size, &entry)) {
        fprintf(stderr, "%s: invalid or truncated PS-X EXE\n", name);
        return -1;
    }
    printf("Native arm64: loaded PS-X EXE, guest entry 0x%08x\n", entry);
    return 0;
}
void Memories_Unimplemented(const char *name)
{
    fprintf(stderr, "Native arm64: unimplemented game routine %s; stopping\n", name);
    fflush(stderr);
    exit(70);
}
