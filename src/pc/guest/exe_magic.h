#ifndef MEMORIES_PC_GUEST_EXE_MAGIC_H
#define MEMORIES_PC_GUEST_EXE_MAGIC_H
#include <stdint.h>
#include <string.h>

/* Whether the data starts with the magic of a PlayStation executable.
 *
 * A community "free version" image ships its executable with the magic's
 * first byte wiped and nothing else changed (the header is the retail one,
 * byte for byte, from the entry point on). The rest of the magic is what
 * identifies the executable, so it is read from the second byte on: a first
 * byte of 'P' or of zero both mean a PlayStation executable. */
static inline int Memories_ExeMagicOk(const uint8_t *data)
{
    return data && !memcmp(data + 1, "S-X EXE", 7);
}
#endif
