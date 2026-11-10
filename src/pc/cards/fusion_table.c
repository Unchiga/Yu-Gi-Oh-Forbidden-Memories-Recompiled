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
static long rows_max = -1;
static int seal_failed;          /* until something changes, one try is enough */
#define SCAN_MAX 65536           /* rows Find goes down, a duel's fusions being asked for by the hundred */

int FusionTable_Count(void) { return count; }
void FusionTable_LimitRows(long rows) { rows_max = rows; }

void FusionTable_Remove(int first, int rows_out)
{
    if (sealed || first < 0 || rows_out <= 0 || first + rows_out > count) return;
    memmove(rows + first, rows + first + rows_out, (size_t)(count - first - rows_out) * sizeof(*rows));
    memmove(lows + first, lows + first + rows_out, (size_t)(count - first - rows_out) * sizeof(*lows));
    count -= rows_out;
    seal_failed = 0;
}

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
    seal_failed = 0;
}

int FusionTable_Add(int a, int b, int result, unsigned order)
{
    if (rows_max >= 0 && count >= rows_max) return 0;
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
    seal_failed = 0;
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

int FusionTable_Seal(void)
{
    Row *sorted;
    unsigned *place;
    unsigned *bucket_starts;
    int highest = 0;
    if (sealed) return 1;
    if (seal_failed) return 0;
    for (int i = 0; i < count; i++)
        if (lows[i] > highest) highest = lows[i];
    sorted = malloc((size_t)count * sizeof(*sorted));
    place = calloc((size_t)highest + 2, sizeof(*place));
    bucket_starts = malloc(((size_t)highest + 2) * sizeof(*bucket_starts));
    if (!sorted || !place || !bucket_starts) {
        free(sorted);
        free(place);
        free(bucket_starts);
        seal_failed = 1;
        return 0;
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
    return 1;
}

int FusionTable_Find(int a, int b, int *result, unsigned *order)
{
    int lower = a < b ? a : b, higher = a < b ? b : a;
    unsigned low, high;
    if (!sealed && !FusionTable_Seal()) {
        if (count > SCAN_MAX) return 0;
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

typedef struct {
    unsigned short card, with;
    int result;
} OwnRow;
static OwnRow *own_rows;
static int own_count, own_room;

void FusionTable_ClearOwn(void)
{
    free(own_rows);
    own_rows = NULL;
    own_count = own_room = 0;
}

int FusionTable_AddOwn(int card, int with, int result)
{
    if (own_count == own_room) {
        int wanted = own_room ? own_room * 2 : 64;
        OwnRow *bigger = realloc(own_rows, (size_t)wanted * sizeof(*bigger));
        if (!bigger) return 0;
        own_rows = bigger;
        own_room = wanted;
    }
    own_rows[own_count].card = (unsigned short)card;
    own_rows[own_count].with = (unsigned short)with;
    own_rows[own_count++].result = result;
    return 1;
}

int FusionTable_FindOwn(int card, int with, int *result)
{
    int low = 0, high = own_count;
    while (low < high) {
        int middle = low + (high - low) / 2;
        if (own_rows[middle].card < card) low = middle + 1;
        else high = middle;
    }
    for (; low < own_count && own_rows[low].card == card; low++)
        if (own_rows[low].with == with) {
            *result = own_rows[low].result;
            return 1;
        }
    return 0;
}
