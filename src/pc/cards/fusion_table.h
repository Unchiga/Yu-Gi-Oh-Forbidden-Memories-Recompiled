#ifndef MEMORIES_PC_CARDS_FUSION_TABLE_H
#define MEMORIES_PC_CARDS_FUSION_TABLE_H
/* The fusions mods add: millions of rules in a big mod, held outside the game's
 * token arena (a host-heap unit, tools/pc/build_arm64.py). Each is eight bytes,
 * in a bucket per lower card, the card itself being where the row is. */

/* Whether there was room; `result` 0 is a forbidden fusion. Later rules have a
 * higher `order`. */
int FusionTable_Add(int a, int b, int result, unsigned order);
/* Sorts what was added. Find does it if it is needed; the load does it first,
 * so that the first fusion of a duel does not have to. */
int FusionTable_Seal(void);   /* 0 when there was no room for the buckets (Find, below) */
int FusionTable_Count(void);
/* Rows past this many fail as if memory ran out, to test that; negative: no limit. */
void FusionTable_LimitRows(long rows);
/* Takes rows out, by their place in the order added, before sealing: a mod that goes. */
void FusionTable_Remove(int first, int rows);
void FusionTable_Clear(void);
/* The latest rule for the pair, in either order: 1 with its result and order. Unsealed
 * for lack of memory it goes down the rows, and past a size where that is no answer
 * it finds nothing: the load takes the rows out first (tables.c). */
int FusionTable_Find(int a, int b, int *result, unsigned *order);

/* A card's own "fusions" (cards.c), in the order the list has them, the cards in
 * ascending order: with whom, and what it gives, as the entry says it. */
int FusionTable_AddOwn(int card, int with, int result);
void FusionTable_ClearOwn(void);
/* The first row of `card` for `with`: 1 with *result. */
int FusionTable_FindOwn(int card, int with, int *result);
#endif
