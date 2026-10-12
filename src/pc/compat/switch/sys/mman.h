/* The virtual-memory calls the port makes, for the Switch, whose newlib has
 * none. Only the Switch build has src/pc/compat/switch on its include path
 * (tools/pc/switch_deps.py). The translated build asks for anonymous memory
 * anywhere: pc/compat/mman.h turns the game's fixed requests into registered
 * guest tokens over these. Horizon gives homebrew no executable mappings of
 * its own, so PROT_EXEC is refused (no code mods), and a file or a fixed
 * address cannot be mapped. src/pc/platform/switch_posix.c. */
#ifndef MEMORIES_PC_COMPAT_SWITCH_SYS_MMAN_H
#define MEMORIES_PC_COMPAT_SWITCH_SYS_MMAN_H
#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000
#define MAP_FAILED ((void *)-1)

void *mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *address, size_t length);
int mprotect(void *address, size_t length, int prot);
#endif
