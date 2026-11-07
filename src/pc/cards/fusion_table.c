/* fusion_table.h. Rules are appended as they are read, in two arrays, and sealed
 * into one: a bucket per lower card, each sorted by the higher card, the latest
 * rule first. */
#include "fusion_table.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned short high, result;
    unsigned order;
} Row;

static Row *rows;                /* in the order added, until sealed */
static unsigned short *lows;     /* the lower card of each row, until sealed */
static unsigned *starts;         /* where each lower card's rows begin */
static int top;                  /* the highest lower card in `starts` */
static int count, room, sealed = 1;

int FusionTable_Count(void) { return count; }

void FusionTable_Clear(void)
{
    free(rows);
    free(lows);
    free(starts);
    rows = NULL;
    lows = NULL;
    starts = NULL;
    count = room = top = 0;
    sealed = 1;
}

int FusionTable_Add(int a, int b, int result, unsigned order)
{
    if (count == room) {
        int wanted = room ? room + room / 2 : 1024;
        Row *bigger;
        unsigned short *more;
        if ((size_t)wanted > SIZE_MAX / sizeof(*rows) || wanted <= room) return 0;
        bigger = realloc(rows, (size_t)wanted * sizeof(*rows));
        if (!bigger) return 0;
        rows = bigger;
        more = realloc(lows, (size_t)wanted * sizeof(*lows));
        if (!more) return 0;
        lows = more;
        room = wanted;
    }
    if (sealed && count) {
        /* Added to after sealing: `lows` again, from the buckets. */
        for (int card = 0; card <= top; card++)
            for (unsigned at = starts[card]; at < starts[card + 1]; at++) lows[at] = (unsigned short)card;
    }
    sealed = 0;
    rows[count].high = (unsigned short)(a < b ? b : a);
    rows[count].result = (unsigned short)result;
    rows[count].order = order;
    lows[count++] = (unsigned short)(a < b ? a : b);
    return 1;
}

static int by_high(const void *left, const void *right)
{
    const Row *a = left, *b = right;
    if (a->high != b->high) return a->high < b->high ? -1 : 1;
    return a->order > b->order ? -1 : a->order < b->order; /* the latest first */
}

void FusionTable_Seal(void)
{
    Row *sorted;
    unsigned *place;
    unsigned *bucket_starts;
    int highest = 0;
    if (sealed) return;
    for (int i = 0; i < count; i++)
        if (lows[i] > highest) highest = lows[i];
    sorted = malloc((size_t)count * sizeof(*sorted));
    place = calloc((size_t)highest + 2, sizeof(*place));
    bucket_starts = malloc(((size_t)highest + 2) * sizeof(*bucket_starts));
    if (!sorted || !place || !bucket_starts) {
        free(sorted);
        free(place);
        free(bucket_starts);
        /* No bucket can be made; Find goes down the rows as they are. */
        return;
    }
    for (int i = 0; i < count; i++) place[lows[i] + 1]++;
    for (int card = 0; card <= highest; card++) place[card + 1] += place[card];
    free(starts);
    starts = bucket_starts;
    memcpy(starts, place, ((size_t)highest + 2) * sizeof(*starts));
    for (int i = 0; i < count; i++) sorted[place[lows[i]]++] = rows[i];
    free(place);
    for (int card = 0; card <= highest; card++)
        qsort(sorted + starts[card], starts[card + 1] - starts[card], sizeof(*sorted), by_high);
    free(rows);
    free(lows);
    rows = sorted;
    lows = NULL; /* Add makes it again if rules come after this; room says it must */
    room = count;
    top = highest;
    sealed = 1;
}

int FusionTable_Find(int a, int b, int *result, unsigned *order)
{
    int lower = a < b ? a : b, higher = a < b ? b : a;
    unsigned low, high;
    if (!sealed) FusionTable_Seal();
    if (!sealed) {
        const Row *best = NULL;
        for (int i = 0; i < count; i++)
            if (lows[i] == lower && rows[i].high == higher && (!best || rows[i].order > best->order)) best = &rows[i];
        if (!best) return 0;
        *result = best->result;
        *order = best->order;
        return 1;
    }
    if (!count || lower > top) return 0;
    low = starts[lower];
    high = starts[lower + 1];
    while (low < high) { /* the first row for the pair: the latest */
        unsigned middle = low + (high - low) / 2;
        if (rows[middle].high < higher) low = middle + 1;
        else high = middle;
    }
    if (low == starts[lower + 1] || rows[low].high != higher) return 0;
    *result = rows[low].result;
    *order = rows[low].order;
    return 1;
}
