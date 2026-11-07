#ifndef MEMORIES_PC_CARDS_CARD_ART_CACHE_H
#define MEMORIES_PC_CARDS_CARD_ART_CACHE_H
/* The art records (art.h) of the cards mods add: made from their PNGs when a card
 * is first seen, not while the game starts. A record takes about 30 ms to make
 * on a phone and is 12 KB, so a mod of thousands of cards must not make them all.
 *
 * What was made is kept on disk, under the user folder and never in the mod's or the
 * chosen mods folder, by the PNGs' paths, sizes and times and the build; a thread reads
 * ahead from the last card seen, so a list opened later is already there. In memory a
 * few MB at most, oldest out. All of it on the host heap (a host-heap unit,
 * tools/pc/build_arm64.py), so none of it in a save state. */

#define ART_CACHE_PICTURE 1
#define ART_CACHE_THUMBNAIL 2

/* An entry's PNGs, either of which may be NULL; one pair is made once however many
 * entries name it. A handle, or 0 for no room. `mod`, which outlives this, and
 * `index` name the entry in a note if a PNG proves bad when it is read. */
int ArtCache_Add(const char *mod, int index, const char *art, const char *thumbnail);
/* The record, whole, until the next call on this thread; NULL if none of it could be
 * made. *parts says which of it is real. From the game's thread only. */
const unsigned char *ArtCache_Record(int handle, int *parts);
/* 1 the first time it is asked for this handle: for what is done once per picture. */
int ArtCache_FirstSight(int handle);
void ArtCache_Paths(int handle, const char **art, const char **thumbnail);
/* With every entry added: the disk cache in `directory` (made if need be), which only
 * this may be told, and the thread. Without it records live in memory alone. */
void ArtCache_Start(const char *directory, unsigned build);
#endif
