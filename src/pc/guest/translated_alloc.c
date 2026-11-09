#include "../../types.h"
#include "translated_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

void *GuestRuntime_malloc(size_t length)
{
    void *p = malloc(length ? length : 1);
    if (p && GuestRuntime_IsBound()) GuestRuntime_RegisterAllocation(p, length ? length : 1);
    return p;
}
void *GuestRuntime_calloc(size_t count, size_t length)
{
    size_t size;
    void *p;
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
    if (!pointer) return;
    host = GuestRuntime_ResolveData(pointer, 1);
    GuestRuntime_UnregisterData(host);
    free(host);
}
void *GuestRuntime_realloc(void *pointer, size_t length)
{
    void *old, *p;
    volatile uintptr_t key;
    if (!pointer) return GuestRuntime_malloc(length);
    if (!length) { GuestRuntime_free(pointer); return NULL; }
    old = GuestRuntime_ResolveData(pointer, 1);
    key = (uintptr_t)old;
    p = realloc(old, length);
    if (!p) return NULL;
    /* Only the old address is looked up, never read. Through a volatile, so
     * GCC 13's -Wuse-after-free cannot trace the key back to the freed block. */
    GuestRuntime_UnregisterData((void *)key);
    if (GuestRuntime_IsBound()) GuestRuntime_RegisterAllocation(p, length);
    return p;
}
