#ifndef MEMORIES_PC_COMPAT_MMAN_H
#define MEMORIES_PC_COMPAT_MMAN_H
/* <sys/mman.h> for the patterns the port uses: anonymous read/write memory,
 * at a fixed address (MAP_FIXED_NOREPLACE) or anywhere, and turning some of
 * it into code (mprotect, for mods: src/pc/mods/object_loader.c). Windows
 * maps it with VirtualAlloc, which also refuses an address that is already
 * taken. */
#ifdef _WIN32
#include <errno.h>
#include <stddef.h>
/* Declared by hand: <windows.h> would bring RECT and other names that clash
 * with the game's own types. Same signatures as the SDK (LPVOID, SIZE_T, DWORD). */
#ifndef _WINDOWS_
#ifdef _WIN64
#define MEMORIES_SIZE_T unsigned long long /* SIZE_T is 64 bits there */
#else
#define MEMORIES_SIZE_T unsigned long
#endif
__declspec(dllimport) void *__stdcall VirtualAlloc(void *address, MEMORIES_SIZE_T size, unsigned long type,
                                                   unsigned long protect);
__declspec(dllimport) int __stdcall VirtualFree(void *address, MEMORIES_SIZE_T size, unsigned long type);
__declspec(dllimport) int __stdcall VirtualProtect(void *address, MEMORIES_SIZE_T size, unsigned long protect,
                                                   unsigned long *old_protect);
__declspec(dllimport) unsigned long __stdcall GetLastError(void);
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#endif

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FIXED 0x10   /* taken as NOREPLACE: VirtualAlloc never replaces */
#define MAP_FAILED ((void *)-1)

#ifdef _WIN64
/* src/pc/guest/image.c, under MEMORIES_X64_HIGH_HEAP: a fixed region takes
 * its range back from the reservation that keeps the heap above 4 GB. Weak,
 * for the tests that use this header without the game. */
void Memories_ReleaseLowPlaceholder(void *address, size_t length) __attribute__((weak));
#endif

static inline void *mmap(void *address, size_t length, int prot, int flags, int fd, long long offset)
{
    void *p;
    (void)prot;
    (void)flags;
    (void)fd;
    (void)offset;
#ifdef _WIN64
    if (address && Memories_ReleaseLowPlaceholder) Memories_ReleaseLowPlaceholder(address, length);
#endif
    p = VirtualAlloc(address, length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p) return p;
    /* errno as mmap sets it, for the callers' messages: an address already
     * reserved or mapped is ERROR_INVALID_ADDRESS (487), as EEXIST is for
     * MAP_FIXED_NOREPLACE; no memory or commit charge left, ENOMEM. */
    switch (GetLastError()) {
    case 487: errno = address ? EEXIST : ENOMEM; break;    /* ERROR_INVALID_ADDRESS */
    case 8: case 14: case 1455: errno = ENOMEM; break;     /* NOT_ENOUGH_MEMORY, OUTOFMEMORY, COMMITMENT_LIMIT */
    default: errno = EINVAL; break;
    }
    return MAP_FAILED;
}

static inline int munmap(void *address, size_t length)
{
    (void)length;
    return VirtualFree(address, 0, MEM_RELEASE) ? 0 : -1;
}

static inline int mprotect(void *address, size_t length, int prot)
{
    unsigned long old;
    unsigned long protect = (prot & PROT_EXEC) ? ((prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
                                               : ((prot & PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY);
    return VirtualProtect(address, (unsigned long)length, protect, &old) ? 0 : -1;
}
#elif defined(MEMORIES_TRANSLATED)
#include <sys/mman.h>
/* The translated game (macOS, Android arm64): a fixed request (the text
 * arena at 0x9C000000, low memory at 0x9E000000, the interpreter stack at
 * 0x9FF00000, mod arenas) takes host memory wherever the system gives it
 * and registers it with the translation runtime at the guest address asked
 * for (translated_mman.c), so no fixed host address is needed: a range the
 * system holds there (ART's spaces on a phone) changes nothing. They never
 * replace an existing virtual-memory mapping. Darwin has no
 * MAP_FIXED_NOREPLACE; Linux's (0x100000) is kept, so a unit that maps
 * through <sys/mman.h> itself means the kernel's flag by it. */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x40000000
#endif
void *Memories_TranslatedMmap(void *, size_t, int, int, int, off_t);
int Memories_TranslatedMunmap(void *, size_t);
int Memories_TranslatedMprotect(void *, size_t, int);
#ifndef MEMORIES_TRANSLATED_MMAN_IMPLEMENTATION
#define mmap Memories_TranslatedMmap
#define munmap Memories_TranslatedMunmap
#define mprotect Memories_TranslatedMprotect
#endif
#else
#include <sys/mman.h>
#endif

/* Memories_MapFixed: mmap for a MAP_FIXED_NOREPLACE request, which maps at
 * `address` exactly or not at all (MAP_FAILED, errno EEXIST when something
 * holds part of the range), on every kernel. Linux before 4.17 does not
 * know the flag and takes the address as a hint: a range that is free is
 * given as asked, one that is not goes wherever the kernel puts it (in an
 * arm64 app process top-down from below the stack, far above 4 GB), which
 * is undone here and told as the newer kernels tell it. The range is never
 * checked and then mapped with MAP_FIXED: another thread (an app's Java
 * runtime maps below 4 GB itself) could take it in between, and MAP_FIXED
 * would map over that. MEMORIES_TEST_OLD_KERNEL=1, in a test build, drops
 * the flag as such a kernel does (notes/pc-build.md, "Android arm64"). */
#if defined(_WIN32) || defined(MEMORIES_TRANSLATED)
#define Memories_MapFixed mmap /* both refuse a taken address themselves */
#else
#include <errno.h>
#include <stdlib.h>
static inline void *Memories_MapFixed(void *address, size_t length, int prot, int flags, int fd, off_t offset)
{
    void *got;
#ifdef MEMORIES_TEST_HOOKS
    const char *old = getenv("MEMORIES_TEST_OLD_KERNEL");
    if (old && old[0] && old[0] != '0') flags &= ~MAP_FIXED_NOREPLACE;
#endif
    got = mmap(address, length, prot, flags, fd, offset);
    if (got != MAP_FAILED && got != address) {
        munmap(got, length);
        errno = EEXIST;
        return MAP_FAILED;
    }
    return got;
}
#endif
#endif
