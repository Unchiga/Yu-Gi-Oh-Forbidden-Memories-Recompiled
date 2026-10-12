/* What the port asks of POSIX that the Switch's newlib declares but does not
 * have: anonymous mappings from page-aligned heap blocks
 * (src/pc/compat/switch/sys/mman.h says what is left out and why), the
 * signal mask, the page size, a positioned read and (no) iconv. */
#ifdef __SWITCH__
#include <sys/mman.h>
#include "pc/compat/fs.h"
#include <errno.h>
#include <iconv.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>

#define PAGE 0x1000u

void *mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset)
{
    void *block;
    if ((flags & MAP_FIXED) || !(flags & MAP_ANONYMOUS) || fd != -1 || offset || !length) {
        errno = (flags & MAP_ANONYMOUS) ? EINVAL : ENODEV;
        return MAP_FAILED;
    }
    if (prot & PROT_EXEC) {
        errno = ENOTSUP;
        return MAP_FAILED;
    }
    (void)address; /* a hint, as on Linux */
    length = (length + PAGE - 1) & ~(size_t)(PAGE - 1);
    if (!(block = aligned_alloc(PAGE, length))) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return memset(block, 0, length); /* anonymous memory starts zeroed */
}

/* Callers unmap what they mapped, whole: a block is freed by its start. */
int munmap(void *address, size_t length)
{
    (void)length;
    if ((uintptr_t)address & (PAGE - 1)) {
        errno = EINVAL;
        return -1;
    }
    free(address);
    return 0;
}

/* Guard pages (PROT_NONE) are not enforced; turning memory into code is
 * refused. */
int mprotect(void *address, size_t length, int prot)
{
    (void)address;
    (void)length;
    if (prot & PROT_EXEC) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

/* No signal is ever delivered on Horizon (a fault is an exception:
 * src/pc/debug/crash.c), so there is nothing to install, and blocking one
 * changes nothing; the port's clock is cooperative there
 * (platform_common.c). */
int sigaction(int number, const struct sigaction *action, struct sigaction *previous)
{
    (void)number;
    (void)action;
    if (previous) memset(previous, 0, sizeof(*previous));
    errno = ENOSYS;
    return -1;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *previous)
{
    (void)how;
    (void)set;
    if (previous) sigemptyset(previous);
    return 0;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *previous)
{
    return sigprocmask(how, set, previous);
}

/* The disc reader's one positioned read (libds.c): the game reads the
 * image from one thread. */
ssize_t pread(int fd, void *buffer, size_t count, off_t offset)
{
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) return -1;
    return read(fd, buffer, count);
}

/* No converter at all: the kanji ROM (libapi_krom.c) does without one, as on
 * an Android without iconv. */
iconv_t iconv_open(const char *to, const char *from)
{
    (void)to;
    (void)from;
    errno = EINVAL;
    return (iconv_t)-1;
}

size_t iconv(iconv_t converter, char **in, size_t *in_left, char **out, size_t *out_left)
{
    (void)converter; (void)in; (void)in_left; (void)out; (void)out_left;
    errno = EBADF;
    return (size_t)-1;
}

int iconv_close(iconv_t converter)
{
    (void)converter;
    return 0;
}

/* The SD card's rename does not replace an existing file (EEXIST), and the
 * port saves by writing aside and renaming over: the old file goes first.
 * Not atomic, as it is on POSIX: a power cut between the two leaves the
 * new file under its temporary name. */
int __real_rename(const char *from, const char *to);
int __wrap_rename(const char *from, const char *to)
{
    struct stat info;
    if (!__real_rename(from, to)) return 0;
    if (errno != EEXIST || stat(to, &info) || S_ISDIR(info.st_mode) || remove(to)) return -1;
    return __real_rename(from, to);
}

long sysconf(int name)
{
    if (name == _SC_PAGESIZE) return PAGE;
    errno = EINVAL;
    return -1;
}
#endif
