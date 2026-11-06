/* Where the token arena puts a native allocation (translated_runtime.c): the
 * search walks the regions in address order, each once, and lands where
 * looking at every region again after each step would have. The hosts here
 * are numbers, never read: the arena is only an address map. */
#include "types.h"
#include "pc/guest/translated_runtime.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#define BASE 0xd0000000u
#define END 0xe0000000u
#define MB (1024u * 1024u)
#define MODEL_MAX 2048

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
static u32 model_cursor = BASE;

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

/* A span below the cursor is used once the top is too small. */
static void a_span_after_the_wrap(void)
{
    void *slots[14], *high, *back;
    for (int i = 0; i < 14; ++i) slots[i] = place(16u * MB);
    assert(token(slots[0]) == BASE && token(slots[13]) == BASE + 13u * 16u * MB);
    assert(GuestRuntime_UnregisterData(slots[3]) == 0);
    /* Does not fit where slot 3 was: it goes on top, past that span. */
    high = place(17u * MB);
    assert(token(high) == BASE + 14u * 16u * MB);
    /* 15 MB are left on top: this one comes back down. */
    back = place(16u * MB);
    assert(token(back) == BASE + 3u * 16u * MB);
    assert(GuestRuntime_UnregisterData(back) == 0);
    assert(GuestRuntime_UnregisterData(high) == 0);
    for (int i = 0; i < 14; ++i)
        if (i != 3) assert(GuestRuntime_UnregisterData(slots[i]) == 0);
}

/* An exact fit reaches the arena's end; a region across the end, and one
 * across its base, leave the top and the bottom to the search. */
static void the_ends_of_the_arena(void)
{
    void *slots[16], *back, *across, *before;
    for (int i = 0; i < 16; ++i) slots[i] = place(16u * MB);
    assert(token(slots[15]) == END - 16u * MB);
    assert(GuestRuntime_UnregisterData(slots[15]) == 0);
    assert(GuestRuntime_UnregisterData(slots[3]) == 0);
    across = new_host(0x20000);
    assert(GuestRuntime_RegisterData(across, 0x20000, END - 0x10000u) == 0);
    /* The top is 64 KiB short now: it comes back to slot 3's span. */
    back = place(16u * MB);
    assert(token(back) == BASE + 3u * 16u * MB);
    assert(GuestRuntime_UnregisterData(back) == 0);
    assert(GuestRuntime_UnregisterData(across) == 0);
    assert(GuestRuntime_UnregisterData(slots[0]) == 0);
    before = new_host(0x20000);
    assert(GuestRuntime_RegisterData(before, 0x20000, BASE - 0x10000u) == 0);
    back = place(1u * MB);
    assert(token(back) >= BASE + 0x10000u);
    assert(GuestRuntime_UnregisterData(back) == 0);
    assert(GuestRuntime_UnregisterData(before) == 0);
    for (int i = 1; i < 15; ++i)
        if (i != 3) assert(GuestRuntime_UnregisterData(slots[i]) == 0);
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
        for (int i = 0; i < 15; ++i) place(16u * MB);
        place(8u * MB);
        if (token(place(8u * MB)) != END - 8u * MB) _exit(1);
        place(16u);
        _exit(0);
    }
    close(output[1]);
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status));
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
        assert(token(big) == BASE + (u32)(MANY + round) * 912u);
        assert(GuestRuntime_UnregisterData(big) == 0);
        /* The cursor is where it was, so this goes on top. */
        small = place(912);
        assert(token(small) == BASE + (u32)(MANY + round) * 912u);
    }
}

int main(void)
{
    MemoriesMemory *memory = calloc(1, sizeof(*memory));
    assert(memory && GuestRuntime_Bind(memory) == 0);
    a_span_after_the_wrap();
    assert(GuestRuntime_Bind(memory) == 0);
    the_ends_of_the_arena();
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
