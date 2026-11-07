#ifndef MEMORIES_PC_CARDS_NAME_INDEX_H
#define MEMORIES_PC_CARDS_NAME_INDEX_H
/* Which retail card a name means. A mod names cards in its fusions, decks and
 * drops, a million times in a big one, so going down the 722 names for each is
 * what a start-up spends its time on.
 *
 * A name matches exactly but for case (Names_SameWords), and failing that by
 * its letters and digits alone (Names_SameLetters: "Blue-Eyes White Dragon"
 * finds the disc's "Blue-eyes White Dragon"). Each way has a hash table of the
 * ids, added in order, so that of two ids that match, the lower one is found. */
#include <ctype.h>

#define NAME_INDEX_SLOTS 2048u /* more than twice the disc's 722 */

typedef struct NameIndex {
    unsigned short by_words[NAME_INDEX_SLOTS], by_letters[NAME_INDEX_SLOTS]; /* 0: empty */
} NameIndex;

static inline int Names_SameWords(const char *a, const char *b)
{
    if (!a || !b) return 0;
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    }
    return !*a && !*b;
}

/* Letters and digits only, lowercased. */
static inline int Names_SameLetters(const char *a, const char *b)
{
    for (;;) {
        while (*a && !isalnum((unsigned char)*a)) a++;
        while (*b && !isalnum((unsigned char)*b)) b++;
        if (!*a || !*b) return !*a && !*b;
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    }
}

/* Names the same way hash the same: FNV-1a over what the comparison sees. */
static inline unsigned NameIndex_Hash(const char *text, int letters)
{
    unsigned hash = 2166136261u;
    for (; *text; text++) {
        if (letters && !isalnum((unsigned char)*text)) continue;
        hash = (hash ^ (unsigned)tolower((unsigned char)*text)) * 16777619u;
    }
    return hash ^ hash >> 15;
}

static inline void NameIndex_Place(unsigned short *table, unsigned hash, int id)
{
    unsigned slot = hash & (NAME_INDEX_SLOTS - 1u);
    while (table[slot]) slot = (slot + 1u) & (NAME_INDEX_SLOTS - 1u);
    table[slot] = (unsigned short)id;
}

/* Call in order of id, at most NAME_INDEX_SLOTS - 1 of them, ids from 1. */
static inline void NameIndex_Add(NameIndex *index, int id, const char *name)
{
    NameIndex_Place(index->by_words, NameIndex_Hash(name, 0), id);
    NameIndex_Place(index->by_letters, NameIndex_Hash(name, 1), id);
}

/* The id, 0 for none. `names` is the table of what was added, indexed by id,
 * `stride` bytes apart. */
static inline int NameIndex_Find(const NameIndex *index, const char *text, const char *names, unsigned stride)
{
    unsigned slot;
    for (slot = NameIndex_Hash(text, 0) & (NAME_INDEX_SLOTS - 1u); index->by_words[slot];
         slot = (slot + 1u) & (NAME_INDEX_SLOTS - 1u)) {
        int id = index->by_words[slot];
        if (Names_SameWords(text, names + (unsigned)id * stride)) return id;
    }
    for (slot = NameIndex_Hash(text, 1) & (NAME_INDEX_SLOTS - 1u); index->by_letters[slot];
         slot = (slot + 1u) & (NAME_INDEX_SLOTS - 1u)) {
        int id = index->by_letters[slot];
        if (Names_SameLetters(text, names + (unsigned)id * stride)) return id;
    }
    return 0;
}
#endif
