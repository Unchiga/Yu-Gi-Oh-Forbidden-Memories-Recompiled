#include "types.h"
#include "pc/guest/translated_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
static s32 sum(s32 a, s32 b) { return a + b; }
static void expect_abort(int which)
{
    static const char *operations[] = {
        "invalid guest data span", "invalid guest data span",
        "unknown guest function", "unregistered native pointer cannot fit guest storage",
        "invalid guest data span", "invalid guest data span"
    };
    int output[2], status;
    char diagnostic[1024];
    ssize_t length;
    pid_t child;
    assert(pipe(output) == 0);
    child = fork();
    assert(child >= 0);
    if (!child) {
        close(output[0]);
        assert(dup2(output[1], STDERR_FILENO) == STDERR_FILENO);
        close(output[1]);
        if (which == 0) GuestRuntime_ResolveData((void *)(uintptr_t)0xffffffffu, 1);
        if (which == 1) GuestRuntime_ResolveData((void *)(uintptr_t)0xd0000007u, 2);
        if (which == 2) GuestRuntime_ResolveFunction((void *)(uintptr_t)0x80010008u);
        if (which == 3) GuestRuntime_EncodePointer((void *)(uintptr_t)0xfeed00000000ull);
        if (which == 4) GuestRuntime_ResolveData((void *)(uintptr_t)0xd0010008u, 25);
        if (which == 5) GuestRuntime_ResolveData((void *)(uintptr_t)0xd0010008u, 1);
        _exit(0);
    }
    close(output[1]);
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    length = read(output[0], diagnostic, sizeof(diagnostic) - 1);
    close(output[0]);
    assert(length > 0);
    diagnostic[length] = 0;
    assert(strstr(diagnostic, operations[which]));
    assert(strstr(diagnostic, "regions=") && strstr(diagnostic, "live="));
    assert(strstr(diagnostic, "heap=") && strstr(diagnostic, "cursor="));
}
/* What the crash report asks for, from a handler of the abort. */
static int detail_fd;
static void write_detail(int number)
{
    (void)number;
    GuestRuntime_WriteFatalDetail(detail_fd);
    _exit(7);
}
static void expect_detail(void)
{
    int output[2], status;
    char text[1024];
    ssize_t length;
    pid_t child;
    assert(pipe(output) == 0);
    child = fork();
    assert(child >= 0);
    if (!child) {
        close(output[0]);
        detail_fd = output[1];
        GuestRuntime_WriteFatalDetail(detail_fd); /* no detail yet: nothing */
        signal(SIGABRT, write_detail);
        GuestRuntime_ResolveData((void *)(uintptr_t)0xffffffffu, 1);
        _exit(0);
    }
    close(output[1]);
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 7);
    length = read(output[0], text, sizeof(text) - 1);
    close(output[0]);
    assert(length > 0);
    text[length] = 0;
    assert(!strncmp(text, "translated runtime: invalid guest data span at 0xffffffff", 57));
    assert(strstr(text, "regions=") && text[length - 1] == '\n');
}
int main(void)
{
    MemoriesMemory *memory = calloc(1, sizeof(*memory));
    u8 external[8] = {0};
    struct { u8 bytes[34]; u8 guard[16]; } adjacent_storage = {0};
    u8 *adjacent = adjacent_storage.bytes;
    s32 (*function)(s32, s32);
    unsigned i;
    assert(memory && GuestRuntime_Bind(memory) == 0);
    assert(GuestRuntime_Bind(NULL) == -1);
    assert(GuestRuntime_ResolveData((void *)(uintptr_t)0x80000100u, 4) == memory->ram + 0x100);
    assert(GuestRuntime_ResolveData((void *)(uintptr_t)0xa0000100u, 4) == memory->ram + 0x100);
    assert(GuestRuntime_ResolveData(NULL, 1) == memory->ram); /* console kernel RAM */
    assert(GuestRuntime_EncodePointer(memory->ram + 0x123) == 0x80000123u);
    assert(GuestRuntime_EncodePointer(memory->scratchpad + 0x123) == 0x1f800123u);
    assert(GuestRuntime_EncodePointer(memory->scratchpad) == 0x1f800000u);
    assert(GuestRuntime_ResolveData((void *)(uintptr_t)GuestRuntime_EncodePointer(memory->scratchpad), 1) == memory->scratchpad);
    assert(GuestRuntime_EncodePointer(NULL) == 0);
    /* External tokens must never collide with the console aliases, even
     * when a registered span crosses into them from the preceding byte. */
    assert(GuestRuntime_RegisterData(external, 8, MEMORIES_GUEST_RAM_UNCACHED) == -1);
    assert(GuestRuntime_RegisterData(external, 8, MEMORIES_GUEST_RAM_UNCACHED - 4) == -1);
    assert(GuestRuntime_RegisterData(external, 8, MEMORIES_GUEST_RAM_UNCACHED + MEMORIES_RAM_SIZE - 4) == -1);
    assert(GuestRuntime_RegisterData(external, 8, MEMORIES_GUEST_SCRATCHPAD) == -1);
    assert(GuestRuntime_RegisterData(external, 8, MEMORIES_GUEST_SCRATCHPAD - 4) == -1);
    assert(GuestRuntime_RegisterData(memory->ram + 4, 8, 0xd0100000u) == -1);
    assert(GuestRuntime_RegisterData(memory->scratchpad + 4, 8, 0xd0100000u) == -1);
    {
        const u32 neighbors[] = {MEMORIES_GUEST_RAM_UNCACHED - 8,
                                MEMORIES_GUEST_RAM_UNCACHED + MEMORIES_RAM_SIZE,
                                MEMORIES_GUEST_SCRATCHPAD - 8,
                                MEMORIES_GUEST_SCRATCHPAD + MEMORIES_SCRATCHPAD_SIZE};
        for (i = 0; i < sizeof(neighbors) / sizeof(neighbors[0]); ++i) {
            assert(!GuestRuntime_RegisterData(external, 8, neighbors[i]));
            assert(GuestRuntime_EncodePointer(external + 7) == neighbors[i] + 7);
            assert(GuestRuntime_ResolveData((void *)(uintptr_t)(neighbors[i] + 7), 1) == external + 7);
            assert(!GuestRuntime_UnregisterData(external));
        }
    }
    assert(GuestRuntime_RegisterData(external, sizeof(external), 0xd0000000u) == 0);
    assert(GuestRuntime_RegisterData(external, 1, 0xd0000010u) == -1);
    assert(GuestRuntime_RegisterData(external + 1, 1, 0xd0000000u) == -1);
    assert(GuestRuntime_EncodePointer(external + 7) == 0xd0000007u);
    assert(!GuestRuntime_RegisterData(adjacent, 14, 0xd0001000u));
    assert(!GuestRuntime_RegisterData(adjacent + 14, 20, 0xd0002000u));
    assert(GuestRuntime_EncodePointer(adjacent + 14) == 0xd0002000u);
    assert(GuestRuntime_ResolveData((void *)(uintptr_t)GuestRuntime_EncodePointer(adjacent + 14), 20) == adjacent + 14);
    assert(GuestRuntime_EncodePointer(adjacent + 34) == 0xd0002014u);
    assert(GuestRuntime_EncodePointer(external + 8) == 0xd0000008u);
    assert(GuestRuntime_ResolveData((void *)(uintptr_t)0xd0000007u, 1) == external + 7);
    assert(GuestRuntime_ResolveData(external, sizeof(external)) == external);
    assert(GuestRuntime_ResolveData((void *)(intptr_t)(int32_t)0xd0000007u, 1) == external + 7);
    assert(GuestRuntime_ResolveData((void *)(intptr_t)(int32_t)0x80000100u, 4) == memory->ram + 0x100);
    assert(GuestRuntime_EncodePointer((void *)(intptr_t)(int32_t)0xd0000007u) == 0xd0000007u);
    assert(GuestRuntime_RegisterFunction(0x80010000u, (void (*)(void))sum) == 0);
    assert(GuestRuntime_RegisterFunction(0xa0010000u, (void (*)(void))sum) == -1);
    assert(GuestRuntime_RegisterFunction(0x80010001u, (void (*)(void))sum) == -1);
    assert(GuestRuntime_RegisterFunction(0x80010004u, (void (*)(void))sum) == 0);
    assert(GuestRuntime_EncodePointer((void *)(uintptr_t)sum) == 0x80010000u);
    function = (s32 (*)(s32, s32))GuestRuntime_ResolveFunction((void *)(uintptr_t)0x10000u);
    assert(function(10, -3) == 7);
    function = (s32 (*)(s32, s32))GuestRuntime_ResolveFunction((void *)(intptr_t)(int32_t)0x80010000u);
    assert(function(10, -3) == 7);
    for (i = 0; i < 4; ++i) expect_abort((int)i);
    expect_detail();
    {
        u32 *tags = (u32 *)(void *)(memory->ram + 0x10000);
        u32 *a = (u32 *)(void *)(memory->ram + 0x20000);
        u32 *b = (u32 *)(void *)(memory->ram + 0x20010);
        u32 first, last;
        unsigned nearest;
        tags[0] = 0xffffffu; tags[1] = 0x10000u;
        tags[2] = 0x20000u; tags[3] = 0x20010u;
        *a = 0x02010004u; *b = 0x03010008u;
        GuestRuntime_FlattenOt((void *)(uintptr_t)0x80010000u, 4, 0xffffffu,
                              &first, &last, &nearest);
        assert(first == 0x20010u && last == 0x80020000u && nearest == 2);
        assert(*b == 0x03020000u && *a == 0x02010004u);
        tags[2] = 0x10004u; tags[3] = 0x10008u;
        GuestRuntime_FlattenOt((void *)(uintptr_t)0x80010000u, 4, 0xffffffu,
                              &first, &last, &nearest);
        assert(first == 0xffffffu && !last && !nearest);
    }
    {
        u16 values[6] = {9, 0, 3, 0, 7, 5};
        GuestRuntime_RegisterAutomatic(values, sizeof(values));
        u32 token = GuestRuntime_EncodePointer(values);
        assert(GuestRuntime_Clear16((void *)(uintptr_t)(token + 2), 4) == 2);
        assert(values[0] == 9 && values[5] == 5);
        assert(!values[1] && !values[2] && !values[3] && !values[4]);
        assert(GuestRuntime_Clear16((void *)(uintptr_t)(token + 2), 4) == 0);
        assert(!GuestRuntime_UnregisterData(values));
    }
    {
        u8 first[32], second[32], replacement[16];
        /* Small regions sharing a page must still resolve their own span.
         * Removal swaps registry entries; reuse must point at new storage. */
        assert(!GuestRuntime_RegisterData(first, sizeof(first), 0xd0010000u));
        assert(!GuestRuntime_RegisterData(second, sizeof(second), 0xd0010300u));
        for (i = 0; i < 8; ++i) {
            assert(GuestRuntime_ResolveData((void *)(uintptr_t)0xd0010008u, 24) == first + 8);
            assert(GuestRuntime_ResolveData((void *)(uintptr_t)0xd0010308u, 24) == second + 8);
        }
        expect_abort(4); /* Cached page cannot widen a registered span. */
        assert(!GuestRuntime_UnregisterData(first));
        expect_abort(5); /* Cached page cannot retain a freed allocation. */
        assert(GuestRuntime_ResolveData((void *)(uintptr_t)0xd0010308u, 24) == second + 8);
        assert(!GuestRuntime_RegisterData(replacement, sizeof(replacement), 0xd0010000u));
        assert(GuestRuntime_ResolveData((void *)(uintptr_t)0xd0010008u, 8) == replacement + 8);
        assert(!GuestRuntime_UnregisterData(second));
        assert(!GuestRuntime_UnregisterData(replacement));
    }
    {
        u8 storage[64][16];
        /* Reverse/random guest ordering, gaps, multiple allocations per
         * page, and swap removal must retain all surviving identities. */
        for (i = 0; i < 64; ++i) {
            unsigned slot = (i * 37u) & 63u;
            assert(!GuestRuntime_RegisterData(storage[63 - i], 16, 0xd0100000u + slot * 32));
        }
        for (i = 0; i < 64; ++i) {
            unsigned slot = (i * 37u) & 63u;
            assert(GuestRuntime_ResolveData((void *)(uintptr_t)(0xd0100000u + slot * 32 + 3), 13) == storage[63 - i] + 3);
            assert(GuestRuntime_EncodePointer(storage[63 - i] + 3) == 0xd0100000u + slot * 32 + 3);
        }
        for (i = 0; i < 64; i += 2) assert(!GuestRuntime_UnregisterData(storage[63 - i]));
        for (i = 1; i < 64; i += 2) {
            unsigned slot = (i * 37u) & 63u;
            assert(GuestRuntime_ResolveData((void *)(uintptr_t)(0xd0100000u + slot * 32), 16) == storage[63 - i]);
            assert(GuestRuntime_EncodePointer(storage[63 - i]) == 0xd0100000u + slot * 32);
            assert(!GuestRuntime_UnregisterData(storage[63 - i]));
        }
    }
    {
        /* The structured build registers over 5,000 globals; keep tokens
         * valid across table growth and reuse a released allocation. */
        u8 *many = calloc(8192, 1);
        u32 released;
        assert(many);
        for (i = 0; i < 8192; ++i) GuestRuntime_RegisterAutomatic(many + i, 1);
        assert(GuestRuntime_ResolveData((void *)(uintptr_t)0xd0000007u, 1) == external + 7);
        for (i = 0; i < 8192; ++i)
            assert(GuestRuntime_ResolveData((void *)(uintptr_t)GuestRuntime_EncodePointer(many + i), 1) == many + i);
        released = GuestRuntime_EncodePointer(many + 4000);
        assert(!GuestRuntime_UnregisterData(many + 4000));
        GuestRuntime_RegisterAutomatic(many + 4000, 1);
        assert(GuestRuntime_EncodePointer(many + 4000) == released);
        GuestRuntime_Reset();
        free(many);
    }
    {
        /* A temporary decoded image skips a small hole. At the arena end,
         * a later small allocation comes back to the arena's base. */
        u8 *pool = malloc(0x10000000u);
        assert(pool && !GuestRuntime_Bind(memory));
        GuestRuntime_RegisterAutomatic(pool, 16);
        GuestRuntime_RegisterAutomatic(pool + 16, 0x10000000u - 48);
        assert(!GuestRuntime_UnregisterData(pool));
        GuestRuntime_RegisterAutomatic(pool + 0x10000000u - 32, 32);
        GuestRuntime_RegisterAutomatic(pool, 16);
        assert(GuestRuntime_EncodePointer(pool) == 0xc0000000u);
        GuestRuntime_Reset();
        free(pool);
    }
    GuestRuntime_Reset();
    free(memory);
    puts("Translated runtime: data/function round trips and fail-closed invalid addresses passed");
    return 0;
}
