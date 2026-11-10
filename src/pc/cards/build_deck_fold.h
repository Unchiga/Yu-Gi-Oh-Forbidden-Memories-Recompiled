#ifndef MEMORIES_PC_BUILD_DECK_FOLD_H
#define MEMORIES_PC_BUILD_DECK_FOLD_H

/* The Build Deck's chest with a card and its variants folded into one row (SET_BUILD_DECK_FOLD_VARIANTS): the row
 * is the card's (its "head"), shows one of the entry's cards (the one picked, the head itself at first), and L3
 * and R3 step through the cards the player has. What the save keeps is by card as ever: the chest's counts, the
 * deck's cards. Nothing differs while no mod adds a variant. The functions take the Build Deck's workspace only as
 * a key: its picks are kept by it. */

/* Whether the chest is folded: the setting is on, and some mod added a variant. */
int BuildDeckFold_Active(void);

/* A new Build Deck screen: nothing is picked in `workspace`. */
void BuildDeckFold_Begin(const void *workspace);

/* The card a row is of: a variant's base, any other card itself. */
int BuildDeckFold_Head(int id);

/* The card the row of `head` shows: the picked one, `head` when none is. */
int BuildDeckFold_Shown(const void *workspace, int head);

/* Puts the card `id` on show in its row. */
void BuildDeckFold_Pick(const void *workspace, int id);

/* Whether the player has a copy of `id`, in the chest or in the deck. */
int BuildDeckFold_Present(const unsigned char *chest, const unsigned char *deck, int id);

/* The row's flag: `on` when the chest holds a copy of any card of the entry, `held` when only the deck does, 0
 * (not shown) when neither. */
int BuildDeckFold_RowFlag(const unsigned char *chest, const unsigned char *deck, int head, int on, int held);

/* The chest's copies of the whole entry. */
int BuildDeckFold_ChestTotal(const unsigned char *chest, int head);

/* Steps the row of `head` to the next (step 1) or previous (-1) card of the entry the player has, round again. 1
 * when another is on show now, 0 when there is no other. */
int BuildDeckFold_Step(const void *workspace, const unsigned char *chest, const unsigned char *deck, int head,
                       int step);

/* After a count changed: the card on show is one the player still has, else the first that is. */
void BuildDeckFold_Settle(const void *workspace, const unsigned char *chest, const unsigned char *deck, int head);

#endif
