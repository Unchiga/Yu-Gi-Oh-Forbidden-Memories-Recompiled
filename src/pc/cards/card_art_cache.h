#ifndef MEMORIES_PC_CARDS_CARD_ART_CACHE_H
#define MEMORIES_PC_CARDS_CARD_ART_CACHE_H
/* The art records (art.h) of the cards mods add: made from their PNGs when a card
 * is first seen, not while the game starts, so a mod of thousands of cards makes only
 * those it shows.
 *
 * What was made is kept on disk, under the user folder and never in the mod's or the
 * chosen mods folder, in a file per record named by the PNGs' paths, sizes and times
 * and the build; a file that does not check out counts as not there. A thread fills
 * the folder, the decks of the next duel first, then from the last card seen, and it
 * alone writes there. In memory 12 MB at most, oldest out. All of it on the host heap
 * (host-heap units, tools/pc/build_arm64.py): the thread never touches the token arena
 * or anything a save state holds, so a state can be saved or loaded while it works.
 *
 * MEMORIES_ART_CACHE_MB sets how much the folder may hold (512 unless it is a whole
 * number up to 1048576; 0 keeps nothing). Past that, the thread stops filling, and a
 * card the game reads takes the oldest file's place. After a write that fails the thread
 * stops for the run; records are still made when the game reads them. */

#define ART_CACHE_PICTURE 1
#define ART_CACHE_THUMBNAIL 2

/* An entry's PNGs, either of which may be NULL; one pair is made once however many
 * entries name it. A handle, or 0 for no room. `mod`, which outlives this, and
 * `index` name the entry in a note if a PNG proves bad when it is read. Before
 * ArtCache_Start only. */
int ArtCache_Add(const char *mod, int index, const char *art, const char *thumbnail);
/* The record, whole, until the next call on this thread; NULL if none of it could be
 * made. *parts says which of it is real, and is -1 when that is not known yet: in an
 * interrupt (ArtCache_Start) a record not in memory is not looked for. From the game's
 * thread only. */
const unsigned char *ArtCache_Record(int handle, int *parts);
/* 1 the first time it is asked for this handle: for what is done once per picture.
 * Never 1 in an interrupt, which leaves that first time for later. */
int ArtCache_FirstSight(int handle);
void ArtCache_Paths(int handle, const char **art, const char **thumbnail);
/* These are read soon (a duel's decks): the thread makes them before anything else,
 * the last of them first, as the game reads from the first. Any earlier ones are
 * forgotten. From the game's thread. */
void ArtCache_Prefer(const int *handles, int count);
/* With every entry added: the disk cache in `directory` (made if need be), which only
 * this may be told, and the thread. With "" records live in memory alone.
 * `in_interrupt`, which may be NULL, says the caller is in the clock's interrupt
 * (MEMORIES_CLOCK=interrupt): nothing here then takes a lock, allocates or reads a file. */
void ArtCache_Start(const char *directory, unsigned build, int (*in_interrupt)(void));
#endif
