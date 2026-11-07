/* Where the token arena puts a native allocation (translated_runtime.c): the
 * search walks the regions in address order, each once, and lands where
 * looking at every region again after each step would have. The hosts here
 * are numbers, never read, the arena being only an address map, but for what
 * GuestRuntime_TryCalloc allocates itself. */
#include "types.h"
#include "pc/guest/translated_runtime.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#define BASE 0xc0000000u  /* below AUTOMATIC_START: used once the top is full */
#define START 0xd0000000u /* where a save state expects the globals */
#define END 0xe0000000u
#define STACK 0xe0000000u       /* the game's stacks: fixed tokens, registered late */
#define STACK_TOO 0xe1000000u
#define STACK_SIZE (8u * MB)
#define MB (1024u * 1024u)
#define MODEL_MAX 2048

void *GuestRuntime_TryCalloc(size_t count, size_t length);
void GuestRuntime_free(void *pointer);

static uintptr_t next_host = (uintptr_t)1 << 44;

static void *new_host(size_t length)
{
    void *host = (void *)next_host;
    next_host += (length + 0xFFFFu) & ~(size_t)0xFFFFu;
    return host;
}

static u32 token(void *host) { return GuestRuntime_EncodePointer(host); }

static void *place(size_t length)
{
    void *host = new_host(length);
    GuestRuntime_RegisterAutomatic(host, length);
    return host;
}

/* The search as it was written first: over every region, in any order,
 * from the start after each step. */
static struct { u32 guest; size_t length; void *host; } model[MODEL_MAX];
static unsigned model_count;
static u32 model_cursor = START;

static int model_search(size_t length, u32 *found)
{
    u32 candidate = model_cursor;
    int wrapped = 0, moved;
    do {
        moved = 0;
        if (length > END - candidate) {
            if (wrapped) return 0;
            candidate = BASE;
            wrapped = moved = 1;
            continue;
        }
        for (unsigned i = 0; i < model_count; ++i) {
            if (candidate < model[i].guest + model[i].length && model[i].guest < candidate + length) {
                size_t next = ((size_t)model[i].guest + model[i].length + 15u) & ~(size_t)15u;
                candidate = next >= END ? END : (u32)next;
                moved = 1;
                break;
            }
        }
    } while (moved);
    *found = candidate;
    return 1;
}

static void model_add(u32 guest, size_t length, void *host)
{
    assert(model_count < MODEL_MAX);
    model[model_count].guest = guest;
    model[model_count].length = length;
    model[model_count++].host = host;
}

static u32 state = 0x2545F491u;
static u32 next_random(void)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static size_t random_length(void)
{
    u32 kind = next_random() % 16;
    if (!kind) return 1u + next_random() % (8u * MB); /* one that has to step over many */
    return 1u + next_random() % (kind < 8 ? 64u : 4096u);
}

/* Allocations and frees, with a few regions in the arena at addresses no
 * allocation would pick: one too short to be aligned past them is the case
 * the walk has to skip. Every token as the first search gives it. */
static void same_as_the_first_search(void)
{
    for (unsigned round = 0; round < 20000; ++round) {
        u32 what = next_random() % 8;
        if (model_count && (what < 3 || model_count > MODEL_MAX - 8)) {
            unsigned index = next_random() % model_count;
            assert(GuestRuntime_UnregisterData(model[index].host) == 0);
            if (model[index].guest >= BASE && model[index].guest < model_cursor) model_cursor = model[index].guest;
            model[index] = model[--model_count];
        } else if (what == 3) {
            /* Somewhere free, odd, and shorter than the 16 rounding. */
            size_t length = 1 + next_random() % 7;
            u32 guest = (BASE + next_random() % (END - BASE - 64)) | 1u;
            void *host = new_host(length);
            if (GuestRuntime_RegisterData(host, length, guest) == 0) {
                unsigned overlaps = 0;
                for (unsigned i = 0; i < model_count; ++i)
                    overlaps += guest < model[i].guest + model[i].length && model[i].guest < guest + length;
                assert(!overlaps);
                model_add(guest, length, host);
            }
        } else {
            size_t length = random_length();
            u32 expected = 0;
            void *host;
            if (!model_search(length, &expected)) continue; /* a full arena stops the game */
            host = place(length);
            assert(token(host) == expected);
            model_add(expected, length, host);
            model_cursor = (u32)(((size_t)expected + length + 15u) & ~(size_t)15u);
        }
    }
}

static unsigned region_flags(void *host)
{
    for (unsigned i = 0; i < GuestRuntime_RegionCount(); ++i) {
        const GuestRuntimeRegion *region = GuestRuntime_Region(i);
        if (region->host == (uintptr_t)host) return region->flags;
    }
    return 0;
}

/* From the start to the end, then from the base up, then a span left below
 * the cursor. */
static void the_order_of_the_arena(void)
{
    enum { SLOTS = 14, LOW = 16 };
    void *slots[SLOTS], *low[LOW], *high, *back, *stack, *stack_too;
    for (int i = 0; i < SLOTS; ++i) slots[i] = place(16u * MB);
    assert(token(slots[0]) == START && token(slots[SLOTS - 1]) == START + (SLOTS - 1) * 16u * MB);
    assert(GuestRuntime_UnregisterData(slots[3]) == 0);
    /* Does not fit where slot 3 was: it goes on top, past that span. */
    high = place(17u * MB);
    assert(token(high) == START + SLOTS * 16u * MB);
    /* 15 MB are left on top, so this one wraps to the base. */
    low[0] = place(16u * MB);
    assert(token(low[0]) == BASE);
    for (int i = 1; i < LOW; ++i) {
        low[i] = place(16u * MB);
        assert(token(low[i]) == BASE + i * 16u * MB);
    }
    /* The base is full up to the start: slot 3's span is what is left. */
    back = place(16u * MB);
    assert(token(back) == START + 3u * 16u * MB);
    assert(GuestRuntime_AutomaticSpace() == 15u * MB);
    /* The arena nearly full, as with a big mod: the game's stacks come later
     * (Memories_NativeStateRunGame) and their tokens have to be free. */
    stack = new_host(STACK_SIZE);
    stack_too = new_host(STACK_SIZE);
    assert(GuestRuntime_RegisterData(stack, STACK_SIZE, STACK) == 0);
    assert(GuestRuntime_RegisterData(stack_too, STACK_SIZE, STACK_TOO) == 0);
    /* Nothing fits now, and this caller can do without. */
    {
        unsigned regions = GuestRuntime_RegionCount();
        void *refused = new_host(16u * MB);
        assert(GuestRuntime_TryRegisterAllocation(refused, 16u * MB) == -1);
        assert(GuestRuntime_RegionCount() == regions);
        errno = 0;
        assert(!GuestRuntime_TryCalloc(16, MB) && errno == ENOMEM);
        assert(GuestRuntime_RegionCount() == regions);
        /* What fits comes back as any allocation does, and is freed as one. */
        void *fits = GuestRuntime_TryCalloc(1, 4096);
        assert(fits && GuestRuntime_RegionCount() == regions + 1);
        assert(token(fits) == START + SLOTS * 16u * MB + 17u * MB);
        assert(region_flags(fits) & MEMORIES_REGION_HEAP);
        GuestRuntime_free(fits);
        assert(GuestRuntime_RegionCount() == regions);
    }
    assert(GuestRuntime_UnregisterData(stack) == 0);
    assert(GuestRuntime_UnregisterData(stack_too) == 0);
    assert(GuestRuntime_UnregisterData(back) == 0);
    assert(GuestRuntime_UnregisterData(high) == 0);
    for (int i = 0; i < SLOTS; ++i)
        if (i != 3) assert(GuestRuntime_UnregisterData(slots[i]) == 0);
    for (int i = 0; i < LOW; ++i) assert(GuestRuntime_UnregisterData(low[i]) == 0);
}

#ifndef _WIN32
/* Nothing fits, above the cursor or below it: the arena says so. */
static void exhausted(void)
{
    int output[2], status;
    char text[512];
    ssize_t length;
    pid_t child;
    assert(pipe(output) == 0);
    child = fork();
    assert(child >= 0);
    if (!child) {
        close(output[0]);
        assert(dup2(output[1], 2) == 2);
        close(output[1]);
        for (int i = 0; i < 32; ++i) place(16u * MB);
        place(16u);
        _exit(0);
    }
    close(output[1]);
    assert(waitpid(child, &status, 0) == child);
    length = read(output[0], text, sizeof(text) - 1);
    close(output[0]);
    assert(length > 0);
    text[length] = 0;
    assert(strstr(text, "guest token arena exhausted"));
}
#endif

/* The start-up of a big mod: tens of thousands of small allocations, then big
 * ones that have to step over all of them. Each step looked at every region
 * from the start, so this took minutes. The test's time limit is the check. */
static void a_big_allocation_past_many_regions(void)
{
    enum { MANY = 20000 };
    static void *hosts[MANY];
    for (int i = 0; i < MANY; ++i) hosts[i] = place(912);
    assert(GuestRuntime_RegionCount() == MANY);
    for (int round = 0; round < 3000; ++round) {
        void *big, *small;
        /* The cursor goes back down to a span too small for what comes next,
         * which then steps over nearly every region. */
        assert(GuestRuntime_UnregisterData(hosts[round]) == 0);
        big = place(4u * MB);
        assert(token(big) == START + (u32)(MANY + round) * 912u);
        assert(GuestRuntime_UnregisterData(big) == 0);
        /* The cursor is where it was, so this goes on top. */
        small = place(912);
        assert(token(small) == START + (u32)(MANY + round) * 912u);
    }
}

int main(void)
{
    MemoriesMemory *memory = calloc(1, sizeof(*memory));
    assert(memory && GuestRuntime_Bind(memory) == 0);
    the_order_of_the_arena();
    assert(GuestRuntime_Bind(memory) == 0);
#ifndef _WIN32
    exhausted();
#endif
    same_as_the_first_search();
    assert(GuestRuntime_Bind(memory) == 0);
    a_big_allocation_past_many_regions();
    GuestRuntime_Reset();
    free(memory);
    puts("Token arena: address-order search lands where the full search does");
    return 0;
}
