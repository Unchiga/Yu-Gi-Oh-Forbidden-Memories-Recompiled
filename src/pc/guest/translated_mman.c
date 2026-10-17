#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#elif !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE /* MAP_ANONYMOUS (Android, Linux) */
#endif
#define MEMORIES_TRANSLATED_MMAN_IMPLEMENTATION
#include "../../types.h"
#include "pc/compat/mman.h"
#include "translated_runtime.h"
#include <errno.h>
#include <stdint.h>

void *Memories_TranslatedMmap(void *address, size_t length, int prot, int flags, int fd, off_t offset)
{
    void *host;
    if (!(flags & MAP_FIXED_NOREPLACE)) return mmap(address, length, prot, flags, fd, offset);
    if (!address || (uintptr_t)address > UINT32_MAX || !length || !(flags & MAP_ANONYMOUS) ||
        fd != -1 || offset || (prot & PROT_EXEC)) {
        errno = ENOTSUP;
        return MAP_FAILED;
    }
    host = mmap(NULL, length, prot, flags & ~MAP_FIXED_NOREPLACE, fd, offset);
    if (host == MAP_FAILED) return host;
    if (GuestRuntime_RegisterMapping(host, length, (u32)(uintptr_t)address)) {
        munmap(host, length);
        errno = EEXIST;
        return MAP_FAILED;
    }
    return address;
}
int Memories_TranslatedMunmap(void *address, size_t length)
{
    void *host;
    if ((uintptr_t)address > UINT32_MAX) return munmap(address, length);
    host = GuestRuntime_ResolveData(address, length);
    if (GuestRuntime_UnregisterData(host)) { errno = EINVAL; return -1; }
    return munmap(host, length);
}
int Memories_TranslatedMprotect(void *address, size_t length, int prot)
{
    if ((uintptr_t)address <= UINT32_MAX) address = GuestRuntime_ResolveData(address, length);
    return mprotect(address, length, prot);
}
