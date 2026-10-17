/* Carrying a translated save state to another build
 * (src/pc/guest/translated_state_symbols.c): every case is carried right
 * or refused, never guessed. The cases are the 2026-10-17 review's
 * (tmp/pc/review-reloc-1/harness.c), each of which the version-1 tables
 * carried wrongly or let through. */
#include "pc/guest/translated_state_symbols.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* No game beside the test: Memories_NativeSymbolsText finds no table. */
int Memories_SymbolTablePath(char *out, size_t size);
int Memories_SymbolTablePath(char *out, size_t size)
{
    if (out && size) out[0] = 0;
    return -1;
}
#ifdef _WIN32
FILE *Memories_Fopen(const char *path, const char *mode);
FILE *Memories_Fopen(const char *path, const char *mode) { return fopen(path, mode); }
#endif

#define H1 "1111111111111111"
#define H2 "2222222222222222"
#define H0 "0000000000000000"

static int prepare(const char *old, unsigned long long old_size, const char *now, unsigned long long new_size)
{
    return Memories_NativeSymbolsPrepareTables(old, strlen(old), old_size, now, strlen(now), new_size);
}

static int map(unsigned long long offset, int stack, unsigned long long *mapped)
{
    uint64_t result = 0;
    int refused = Memories_NativeSymbolsMap(offset, stack, &result);
    *mapped = result;
    return refused;
}

int main(void)
{
    unsigned long long m;
    static const char old[] =
        "# memories-symbols 2 3000\n"
        "1000 0 X " H0 " 0 " H0 " section:.text\n"
        "1000 100 F " H1 " 0 " H0 " duel.c:update\n"
        "1100 100 F " H1 " 0 " H0 " put\n"
        "1200 40 F " H1 " 0 " H0 " b.c:put\n"
        "2000 0 S " H0 " 0 " H0 " section:.rodata\n"
        "2000 80 D " H1 " 80 " H1 " table\n"
        "2100 40 D " H1 " 0 " H0 " hexdigits\n"
        "2100 40 D " H1 " 0 " H0 " png_digit\n"
        "2140 40 D " H1 " 0 " H0 " stats\n";
    /* Everything moved by 0x200; update's code, table's bytes, the pool
     * after it and stats changed; a.c gained its own static put; the two
     * folded names were split. */
    static const char now[] =
        "# memories-symbols 2 3200\n"
        "1200 0 X " H0 " 0 " H0 " section:.text\n"
        "1200 100 F " H2 " 0 " H0 " duel.c:update\n"
        "1300 100 F " H1 " 0 " H0 " put\n"
        "1400 40 F " H1 " 0 " H0 " b.c:put\n"
        "1440 40 F " H1 " 0 " H0 " a.c:put\n"
        "2200 0 S " H0 " 0 " H0 " section:.rodata\n"
        "2200 80 D " H2 " 80 " H2 " table\n"
        "2300 40 D " H1 " 0 " H0 " hexdigits\n"
        "2340 40 D " H1 " 0 " H0 " png_digit\n"
        "2380 40 D " H2 " 0 " H0 " stats\n";
    assert(!prepare(old, 0x3000, now, 0x3200));
    /* A function's start: the function of that name, even changed. */
    assert(!map(0x1000, 0, &m) && m == 0x1200);
    /* 2: a return address into a function whose code changed: refused,
     * on the stack too; into an unchanged one: only on the stack. */
    assert(map(0x1010, 1, &m));
    assert(!map(0x1110, 1, &m) && m == 0x1310);
    assert(map(0x1110, 0, &m));
    /* 5: unit B's static goes to unit B's static, not a new one. */
    assert(!map(0x1200, 0, &m) && m == 0x1400);
    /* 1: inside an object whose bytes changed: refused, even at its start. */
    assert(map(0x2010, 0, &m) && map(0x2000, 0, &m));
    /* 3: the string pool after it: its bytes changed: refused. */
    assert(map(0x2090, 0, &m));
    /* 6: two names at one address that are now apart: refused. */
    assert(map(0x2100, 0, &m) && map(0x2108, 0, &m));
    /* An object that changed (stats): refused; padding in code: refused. */
    assert(map(0x2150, 0, &m));
    /* 4 / 8c: past the old image, or past every row: refused. */
    assert(map(0x3400, 0, &m) && map(0x2fff, 0, &m));
    /* The scan counts exactly what the map refuses (8a: one classifier). */
    {
        uint64_t words[4] = {0x7000001000ull, 0x7000001010ull, 0x7000002090ull, 0x7000003400ull};
        assert(Memories_NativeSymbolsScan((const uint8_t *)words, sizeof(words), 0x7000000000ull, 0, 1) == 2);
        assert(Memories_NativeSymbolsScan((const uint8_t *)words, sizeof(words), 0x7000000000ull, 1, 1) == 2);
    }
    /* An unchanged object, its inside and the unchanged pool after it. */
    {
        static const char a[] = "# memories-symbols 2 3000\n2000 80 D " H1 " 80 " H1 " table\n";
        static const char b[] = "# memories-symbols 2 3000\n2400 80 D " H1 " 80 " H1 " table\n";
        assert(!prepare(a, 0x3000, b, 0x3000));
        assert(!map(0x2010, 0, &m) && m == 0x2410);
        assert(!map(0x2090, 0, &m) && m == 0x2490);
        assert(map(0x2100, 0, &m));   /* past the pool */
    }
    /* A name twice in one table is used for nothing. */
    {
        static const char a[] = "# memories-symbols 2 3000\n1000 10 F " H1 " 0 " H0 " put\n2000 10 F " H1 " 0 " H0
                                " put\n";
        assert(!prepare(a, 0x3000, a, 0x3000));
        assert(map(0x1000, 0, &m));
    }
    /* 7: tables that are not version 2 whole are not read at all: version
     * 1, a field missing, a line too long to read whole, rows past the
     * image or out of order. */
    {
        char *line = malloc(2000);
        static const char *const broken[] = {
            "1000 10 put\n",
            "# memories-symbols 2 3000\n1000 10 F " H1 " 0 " H0 "\n",
            "# memories-symbols 2 3000\n2ff0 20 F " H1 " 0 " H0 " put\n",
            "# memories-symbols 2 3000\n2000 10 F " H1 " 0 " H0 " b\n1000 10 F " H1 " 0 " H0 " a\n",
            "# memories-symbols 2 3000\n1000 10 Q " H1 " 0 " H0 " put\n",
        };
        assert(line);
        for (size_t i = 0; i < sizeof(broken) / sizeof(*broken); i++)
            assert(prepare(broken[i], 0x3000, now, 0x3200) && !Memories_NativeSymbolsActive());
        strcpy(line, "# memories-symbols 2 3000\n1000 10 F " H1 " 0 " H0 " ");
        memset(line + strlen(line), 'x', 1500);
        line[strlen("# memories-symbols 2 3000\n1000 10 F " H1 " 0 " H0 " ") + 1500] = '\0';
        assert(prepare(line, 0x3000, now, 0x3200));
        free(line);
    }
    Memories_NativeSymbolsClear();
    puts("translated state symbols: carried only when certain");
    return 0;
}
