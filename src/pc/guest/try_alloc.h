#ifndef MEMORIES_PC_GUEST_TRY_ALLOC_H
#define MEMORIES_PC_GUEST_TRY_ALLOC_H
/* An allocation the caller can do without. On the translated game a full
 * token arena aborts, because plain calloc's callers are not all ready for
 * NULL, and NULL there is guest address 0: a write through an unchecked one
 * lands in console RAM without a word. When the arena is what is full,
 * Memories_TryCalloc returns NULL (errno ENOMEM) instead, so use it only where
 * NULL is handled; any other refusal to register still aborts. It is free()d
 * as usual. Elsewhere it is calloc, which the caller includes: this header goes in
 * the mods' SDK, whose headers keep the disc's old prototypes of the C library. */
#include <stddef.h>
#ifdef MEMORIES_TRANSLATED
void *GuestRuntime_TryCalloc(size_t count, size_t length);
#define Memories_TryCalloc GuestRuntime_TryCalloc
#else
#define Memories_TryCalloc calloc
#endif
#endif
