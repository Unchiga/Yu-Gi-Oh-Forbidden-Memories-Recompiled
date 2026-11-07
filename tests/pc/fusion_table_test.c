/* The fusion table against a plain list of the same rules: the latest rule for a
 * pair, in either order of the cards, before and after sealing and with rules
 * added after it was sealed. */
#include "pc/cards/fusion_table.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define RULES 40000
#define CARDS 300
static struct { int low, high, result; unsigned order; } list[RULES];
static int used;
static unsigned order;

static unsigned state = 0x2545F491u;
static unsigned next_random(void)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static void add(int count)
{
    while (count--) {
        int a = 1 + (int)(next_random() % CARDS), b = 1 + (int)(next_random() % CARDS);
        int result = (int)(next_random() % 32767);
        assert(FusionTable_Add(a, b, result, ++order));
        list[used].low = a < b ? a : b;
        list[used].high = a < b ? b : a;
        list[used].result = result;
        list[used++].order = order;
    }
}

static void check(int rounds)
{
    while (rounds--) {
        int a = 1 + (int)(next_random() % (CARDS + 20)), b = 1 + (int)(next_random() % (CARDS + 20));
        int low = a < b ? a : b, high = a < b ? b : a, found = -1, result = -1;
        unsigned at = 0;
        for (int i = 0; i < used; i++)
            if (list[i].low == low && list[i].high == high && (found < 0 || list[i].order > list[found].order)) found = i;
        assert(FusionTable_Find(a, b, &result, &at) == (found >= 0));
        if (found >= 0) assert(result == list[found].result && at == list[found].order);
    }
}

int main(void)
{
    int result = 7;
    unsigned at = 9;
    assert(!FusionTable_Find(1, 2, &result, &at) && result == 7 && at == 9);
    add(20000);
    assert(FusionTable_Count() == 20000);
    check(2000); /* sealed by the first look */
    add(10000); /* and added to again */
    check(2000);
    add(10000);
    FusionTable_Seal();
    check(4000);
    assert(FusionTable_Count() == RULES);
    /* One pair over and over: the latest. */
    assert(FusionTable_Add(5, 9, 11, ++order) && FusionTable_Add(9, 5, 12, ++order) && FusionTable_Add(5, 9, 0, ++order));
    assert(FusionTable_Find(9, 5, &result, &at) && result == 0 && at == order);
    FusionTable_Clear();
    assert(!FusionTable_Count() && !FusionTable_Find(5, 9, &result, &at));
    assert(FusionTable_Add(2, 1, 3, 1) && FusionTable_Find(1, 2, &result, &at) && result == 3);
    /* A card's own list: in its order, the first for a partner. */
    assert(!FusionTable_FindOwn(7, 8, &result));
    assert(FusionTable_AddOwn(7, 8, 100) && FusionTable_AddOwn(7, 9, 0) && FusionTable_AddOwn(7, 8, 101));
    assert(FusionTable_AddOwn(9, 8, -1));
    assert(FusionTable_FindOwn(7, 8, &result) && result == 100);
    assert(FusionTable_FindOwn(7, 9, &result) && result == 0);
    assert(FusionTable_FindOwn(9, 8, &result) && result == -1);
    assert(!FusionTable_FindOwn(8, 7, &result) && !FusionTable_FindOwn(7, 10, &result) && !FusionTable_FindOwn(10, 8, &result));
    FusionTable_ClearOwn();
    assert(!FusionTable_FindOwn(7, 8, &result));
    /* Rows taken out before sealing: the rest are found, at their order. */
    FusionTable_Clear();
    assert(FusionTable_Add(1, 2, 3, 1) && FusionTable_Add(1, 4, 5, 2) && FusionTable_Add(2, 3, 6, 3));
    FusionTable_Remove(1, 1);
    assert(FusionTable_Count() == 2);
    assert(FusionTable_Find(1, 2, &result, &at) && result == 3);
    assert(FusionTable_Find(2, 3, &result, &at) && result == 6 && at == 3);
    assert(!FusionTable_Find(1, 4, &result, &at));
    FusionTable_Clear();
    /* The limit stands for memory running out. */
    FusionTable_LimitRows(2);
    assert(FusionTable_Add(1, 2, 3, 1) && FusionTable_Add(1, 3, 4, 2) && !FusionTable_Add(1, 4, 5, 3));
    FusionTable_LimitRows(-1);
    assert(FusionTable_Add(1, 4, 5, 3));
    puts("Fusion table: the latest rule of a plain list");
    return 0;
}
