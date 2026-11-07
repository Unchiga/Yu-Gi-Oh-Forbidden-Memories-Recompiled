#include "../../types.h"
#include "translated_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#ifdef MEMORIES_TEST_HOOKS
#include <pthread.h>

/* A thread that must stay out of the token arena (card_art_cache.c's): a block of its
 * would be in a save state, and a state loaded under it would free the block. Builds
 * with the test hooks stop there, so a unit that belongs in HOST_HEAP_UNITS
 * (tools/pc/build_arm64.py) shows itself on a device. */
static pthread_t forbidden;
static int forbidden_set;

void GuestRuntime_ForbidHeapOnThisThread(void)
{
    forbidden = pthread_self();
    __atomic_store_n(&forbidden_set, 1, __ATOMIC_RELEASE);
    fprintf(stderr, "translated runtime: this thread stays out of the guest heap\n");
}

static void check_thread(const char *what)
{
    if (!__atomic_load_n(&forbidden_set, __ATOMIC_ACQUIRE) || !pthread_equal(forbidden, pthread_self())) return;
    fprintf(stderr, "translated runtime: GuestRuntime_%s on a thread that must not reach the guest heap\n", what);
    abort();
}
#else
#define check_thread(what) ((void)0)
#endif

void *GuestRuntime_malloc(size_t length)
{
    void *p;
    check_thread("malloc");
    p = malloc(length ? length : 1);
    if (p && GuestRuntime_IsBound()) GuestRuntime_RegisterAllocation(p, length ? length : 1);
    return p;
}
void *GuestRuntime_calloc(size_t count, size_t length)
{
    size_t size;
    void *p;
    check_thread("calloc");
    if (count && length > SIZE_MAX / count) { errno = ENOMEM; return NULL; }
    size = count * length;
    p = calloc(1, size ? size : 1);
    if (p && GuestRuntime_IsBound()) GuestRuntime_RegisterAllocation(p, size ? size : 1);
    return p;
}
void *GuestRuntime_TryCalloc(size_t count, size_t length)
{
    size_t size;
    void *p;
    check_thread("TryCalloc");
    if (count && length > SIZE_MAX / count) { errno = ENOMEM; return NULL; }
    size = count * length;
    p = calloc(1, size ? size : 1);
    if (p && GuestRuntime_IsBound() && GuestRuntime_TryRegisterAllocation(p, size ? size : 1)) {
        fprintf(stderr, "memories-pc: no room in the token arena for %zu bytes; the caller goes without\n", size);
        free(p);
        errno = ENOMEM;
        return NULL;
    }
    return p;
}
void GuestRuntime_free(void *pointer)
{
    void *host;
    check_thread("free");
    if (!pointer) return;
    host = GuestRuntime_ResolveData(pointer, 1);
    GuestRuntime_UnregisterData(host);
    free(host);
}
void *GuestRuntime_realloc(void *pointer, size_t length)
{
    check_thread("realloc");
    if (!pointer) return GuestRuntime_malloc(length);
    if (!length) { GuestRuntime_free(pointer); return NULL; }
    return GuestRuntime_ReallocRegistered(GuestRuntime_ResolveData(pointer, 1), length);
}
