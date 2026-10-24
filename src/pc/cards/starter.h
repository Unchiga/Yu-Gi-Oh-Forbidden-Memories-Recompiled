#ifndef MEMORIES_PC_STARTER_H
#define MEMORIES_PC_STARTER_H
/* The deck a new game starts with, as mods write it down
 * (notes/starter-deck.md).
 *
 * A mod's manifest may carry "starter": one deck, or a list of them, each
 * forty cards written down by their copies. When any applied mod offers one,
 * NameEntry_BuildStarterDeck deals a deck from this list in place of the
 * disc's seven weighted pools; the decks every applied mod offers add up, and
 * one of them is picked at random for each new game.
 *
 * Writing the deck down rather than weighting a pool is what lets a starting
 * deck hold a card the disc has not got: the disc's pools are 722 weights of
 * a fixed width, and its generator reads only the first 720 of them
 * (STARTER_DECK_WEIGHT_SCAN_COUNT), so no weight can name a card a mod added.
 * A written deck names cards by id, so a mod's own are dealt like any other.
 *
 * Nothing here runs when no mod offers a deck: the disc's pools are read as
 * they always were, and draw the random numbers they always drew. */
#include "game/card_constants.h"

#define STARTER_DECK_SIZE DECK_SIZE

/* How often one draw may be taken again because the card it found is already
 * held to the copy limit. The disc retries for ever, which a pool of three
 * cards or fewer would never leave; the last draw stands instead. The disc's
 * own pools are read with the same bound (name_entry_main.c). */
#define STARTER_POOL_RETRIES 64

/* The disc's table of pools ends with a null pointer; a table that does not
 * end is walked this many times at most. */
#define STARTER_DECK_MAX_POOLS 64

/* Read every applied mod's "starter"; once, from Cards_Build. */
void Starter_Build(void);

/* What a new game will make of the decks and pools read: in the log, and a
 * Mods_Note beside each mod whose pools are left out (a written deck that
 * weighs anything is dealt first; pools that do not draw a deck's forty
 * leave it to the disc's rows). Starter_Build calls it; so do the tests. */
void Starter_Check(void);

/* Starter_Build without a mod list: one manifest's decks, for the tests. */
struct JsonValue;
void Starter_Add(const char *mod, const struct JsonValue *manifest);
void Starter_Clear(void);

/* How many decks the applied mods offer between them; 0 when none do, and
 * the disc's pools stand. */
int Starter_Count(void);

/* The total of every offered deck's weight, for the roll Starter_Deck takes;
 * 0 when no deck is offered. A deck's "weight" is 1 unless it says. */
unsigned Starter_WeightTotal(void);

/* A roll for Starter_Deck from one of the game's random numbers (rand(), 0
 * to 0x7FFF), spread over the whole weight total however large it is. */
unsigned Starter_Roll(unsigned random);

/* The deck a roll of `roll` (0 to Starter_WeightTotal() - 1) picks: its forty
 * cards, in id order, in `cards`, and its name in *name when `name` is not
 * NULL. 1 when a deck was written there, 0 when no mod offers one (leaving
 * both alone). The caller draws the roll, so the random numbers a new game
 * spends stay where the game spends them. */
int Starter_Deck(unsigned roll, unsigned short cards[STARTER_DECK_SIZE], const char **name);

/* The deck at `index` (0 to Starter_Count() - 1), for the tests and the log:
 * as Starter_Deck writes it, picked by place rather than by weight. */
int Starter_DeckAt(int index, unsigned short cards[STARTER_DECK_SIZE], const char **name);

/* --- pools of the mod's own ------------------------------------------------
 *
 * A mod may weight its own pools instead of writing a deck down, with
 * "starter_pools": a list of {"draws": n, "cards": {card: weight, ...}} whose
 * draws add up to the forty a deck holds. They are the disc's seven rows made
 * a mod's to write, and they lift what the disc's cannot do: the disc keeps
 * 722 weights of a fixed width and reads only the first 720
 * (STARTER_DECK_WEIGHT_SCAN_COUNT), so no weight of its own can name a card a
 * mod added, while a pool here names cards as the rest of a manifest does and
 * may weight any of them.
 *
 * A written deck still wins: NameEntry_BuildStarterDeck asks for one first,
 * then for these, then reads the disc's rows. The pools of every applied mod
 * add up, in the order the mods load, as the decks do. */

/* How many pools the applied mods offer; 0 when none do. */
int Starter_PoolCount(void);

/* The cards the offered pools draw between them: STARTER_DECK_SIZE when they
 * would deal a deck, and whatever they add up to otherwise. */
int Starter_PoolDraws(void);

/* Whether the pools would deal a deck: some are offered and their draws add
 * up to STARTER_DECK_SIZE. 0 leaves the disc's rows to it. */
int Starter_HasPools(void);

/* Deal a deck from the pools, drawing from `next_random` (the game's rand(),
 * so the numbers a new game spends stay the game's): the forty cards in id
 * order in `cards`. 1 when one was dealt, 0 when the pools would not deal.
 *
 * A card already held DECK_CARD_COPY_LIMIT times is drawn again, as the
 * disc's generator retries one; unlike the disc's, the retry gives up after a
 * bounded number of tries so a pool of three cards or fewer cannot hang the
 * new game, and the last draw stands. */
int Starter_DealPools(unsigned (*next_random)(void), unsigned short cards[STARTER_DECK_SIZE]);

#endif
