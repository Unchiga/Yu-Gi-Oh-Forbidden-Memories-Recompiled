/* The token registry used from several threads at once, as Import does while
 * the game draws, or the picture reader while the game runs: allocating,
 * growing, freeing, and resolving and encoding pointers all the while. Before
 * the registry had a lock this aborted within milliseconds. */
#include "types.h"
#include "pc/guest/translated_runtime.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *GuestRuntime_calloc(size_t count, size_t length);
void *GuestRuntime_malloc(size_t length);
void *GuestRuntime_realloc(void *pointer, size_t length);
void GuestRuntime_free(void *pointer);

#define KEEP 256
static volatile int stop;

/* What a reader checks: a block that holds its own token. */
typedef struct Block { u32 token; u32 words[15]; } Block;

static Block *make_block(void)
{
    Block *block = GuestRuntime_calloc(1, sizeof(*block));
    assert(block);
    block->token = GuestRuntime_EncodePointer(block);
    return block;
}

/* A parser: small blocks, kept, and some given back. */
static void *allocator(void *argument)
{
    unsigned seed = (unsigned)(uintptr_t)argument;
    long rounds = 40000;
    void *kept[64] = {0};
    while (rounds-- && !stop) {
        unsigned slot = (seed = seed * 1103515245u + 12345u) >> 16 & 63u;
        if (kept[slot]) GuestRuntime_free((void *)(uintptr_t)GuestRuntime_EncodePointer(kept[slot]));
        kept[slot] = GuestRuntime_calloc(1, 16 + (seed >> 24));
        if (!(seed >> 8 & 7u)) make_block();
    }
    return NULL;
}

/* A list that grows, so that realloc's old block goes back to the heap while
 * the other threads allocate. */
static void *grower(void *unused)
{
    (void)unused;
    while (!stop) {
        unsigned char *list = GuestRuntime_malloc(16);
        for (size_t size = 32; size < 1u << 17 && !stop; size *= 2) {
            list = GuestRuntime_realloc((void *)(uintptr_t)GuestRuntime_EncodePointer(list), size);
            assert(list);
            memset(list, 1, size);
        }
        GuestRuntime_free((void *)(uintptr_t)GuestRuntime_EncodePointer(list));
    }
    return NULL;
}

/* A thread that only reads: the blocks that stay must always resolve to
 * themselves, and encode to their own tokens. */
static Block *live[KEEP];
static void *reader(void *unused)
{
    unsigned at = 0;
    (void)unused;
    while (!stop) {
        Block *block = live[at++ % KEEP];
        Block *resolved = GuestRuntime_ResolveData((void *)(uintptr_t)block->token, sizeof(*block));
        assert(resolved == block);
        assert(GuestRuntime_EncodePointer(block) == block->token);
        /* A different page each time, so that the search runs as well. */
        resolved = GuestRuntime_ResolveData((void *)(uintptr_t)(block->token + (at & 15u) * 4u), 4);
        assert((uintptr_t)resolved - (uintptr_t)block == (at & 15u) * 4u);
    }
    return NULL;
}

int main(void)
{
    MemoriesMemory *memory = calloc(1, sizeof(*memory));
    pthread_t allocators[3], readers[8], growing;
    assert(memory && GuestRuntime_Bind(memory) == 0);
    for (int i = 0; i < KEEP; ++i) live[i] = make_block();
    /* More threads than cores, so that a writer loses its core mid-change. */
    for (int i = 0; i < 8; ++i) pthread_create(&readers[i], NULL, reader, NULL);
    pthread_create(&growing, NULL, grower, NULL);
    for (int i = 0; i < 3; ++i) pthread_create(&allocators[i], NULL, allocator, (void *)(uintptr_t)(i * 7919 + 1));
    for (int i = 0; i < 3; ++i) pthread_join(allocators[i], NULL);
    stop = 1;
    pthread_join(growing, NULL);
    for (int i = 0; i < 8; ++i) pthread_join(readers[i], NULL);
    puts("Translated runtime: the registry from several threads");
    return 0;
}
