#ifndef MEMORIES_PC_MODS_OVERLAP_H
#define MEMORIES_PC_MODS_OVERLAP_H
/* What two or more mods both change, and which of them the game then uses
 * (notes/modding.md, "When mods overlap"). Warnings only: nothing here
 * changes what loads or who wins; it reads the manifests (and the files they
 * name: texture packs, text listings, pool files, the duelists folder) the
 * way the readers do (src/pc/cards/tables.c, stars.c, cards.c, mods.c,
 * title_config.c) and says where they meet.
 *
 * The mods come in load order, earliest first, which is what decides most
 * overlaps: the later mod wins. Some keys add up instead (drop and deck
 * pool edits, starter decks and pools, title text lines), which is said too,
 * as information rather than a warning. Each overlap is one line, built when
 * asked for (a mod of every fusion pair has 261,003 of them). One line may
 * name a single mod: starter pools that do not draw a deck's forty cards,
 * which the game leaves out however many mods wrote them (when two mods or
 * more are listed; one mod alone has no overlaps).
 *
 * The FM Editor makes the same check in Python
 * (tools/pc/fm_editor/overlaps.py); tests/pc/mod_overlaps holds the mods and
 * the lines both must find, in order. */
#include <stddef.h>
#include <stdint.h>

struct JsonValue;

enum {
    MODS_OVERLAP_DATA,
    MODS_OVERLAP_AUDIO,
    MODS_OVERLAP_TEXTURES,
    MODS_OVERLAP_CARDS,
    MODS_OVERLAP_FUSIONS,
    MODS_OVERLAP_EQUIPS,
    MODS_OVERLAP_RITUALS,
    MODS_OVERLAP_POOLS,
    MODS_OVERLAP_STARTER,
    MODS_OVERLAP_PASSWORDS,
    MODS_OVERLAP_PACKS,
    MODS_OVERLAP_STARS,
    MODS_OVERLAP_LIMITS,
    MODS_OVERLAP_TERRAIN,
    MODS_OVERLAP_TRAPS,
    MODS_OVERLAP_DUELISTS,
    MODS_OVERLAP_TEXT,
    MODS_OVERLAP_FONT,
    MODS_OVERLAP_TITLE,
    MODS_OVERLAP_UI,
    MODS_OVERLAP_HOOKS,
    MODS_OVERLAP_EVENTS,
    MODS_OVERLAP_KINDS
};
/* A real override (one mod's change is not used) is a warning; changes that
 * add up, agree, or follow an `after`/`requires` the winner declared on the
 * others are information. */
enum { MODS_OVERLAP_INFO, MODS_OVERLAP_WARNING };

typedef struct {
    const char *id, *name;
    const char *directory; /* where its files are named from; NULL: none read */
    const struct JsonValue *manifest;
} ModsOverlapMod;

/* What the engine cannot know from manifests alone. Every member may be
 * NULL: cards are then matched by number and by name letter for letter
 * (case, spaces and punctuation aside), duelists by name, every file and
 * image a setting may switch off counts as on, and no code hooks are known. */
typedef struct {
    void *context;
    /* A card a manifest names: `text`, or the number when `text` is NULL.
     * The card's id, or 0 or less for none (pc/cards Cards_Named). */
    int (*card)(const char *text, long number, void *context);
    /* A card's name for a line, 0 when there is none. */
    int (*card_name)(int id, char *out, size_t size, void *context);
    /* A card's base (the disc's card a copy is of, or itself), monster type
     * (0-23, cards.c type_names) and attribute (0-5), 0 when unknown: an
     * equip's rules by type, a copy of an attack trap. */
    int (*card_info)(int id, int *base, int *type, int *attribute, void *context);
    /* A mod's "fusions" that are not in its manifest: each entry in turn, gone
     * when `each` returns (Mods_EachFusion); and how many bytes they are, for
     * which list is the longest. NULL: the manifests hold them all. */
    long (*fusions)(int mod, int (*each)(const struct JsonValue *, long, void *), void *each_context, void *context);
    long (*fusion_bytes)(int mod, void *context);
    /* A mod's "cards" that are not in its manifest, for the report to keep and
     * free, or NULL for none (Mods_CardsDocument). NULL: the manifests have them. */
    struct JsonDocument *(*cards)(int mod, void *context);
    /* An opponent by name ("Heishin", "8"), -1 for none (Duelists_Named). */
    int (*duelist)(const char *text, void *context);
    /* Mod `mod`'s setting `key` (a "setting" of a text file or a pack's
     * image, which is left out while it is 0), or -1 when it declares none. */
    int (*setting)(int mod, const char *key, void *context);
    /* The code mods' function hooks and event subscriptions, `index` from 0:
     * 0 past the last, else 1 with the mod (its place in the list given),
     * what it hooks (any number naming the function or event) and its name. */
    int (*hook)(int index, int *mod, uint64_t *what, char *label, size_t size, void *context);
    int (*event)(int index, int *mod, uint64_t *what, char *label, size_t size, void *context);
} ModsOverlapSource;

typedef struct ModsOverlaps ModsOverlaps;

/* The overlaps of `count` mods in load order. NULL when memory ran out. */
ModsOverlaps *Mods_OverlapCompute(const ModsOverlapMod *mods, int count, const ModsOverlapSource *source);
void Mods_OverlapFree(ModsOverlaps *overlaps);
/* The overlaps, sorted by kind; and for one of them its kind, severity,
 * whether mod `mod` (an index of the list given) is one of its mods, and the
 * line: "Card 'Kuriboh' (Alpha, Beta): Beta's attack is used: later in load
 * order; the rest combines". */
int Mods_OverlapCount(const ModsOverlaps *overlaps);
int Mods_OverlapKind(const ModsOverlaps *overlaps, int index);
int Mods_OverlapSeverity(const ModsOverlaps *overlaps, int index);
int Mods_OverlapInvolves(const ModsOverlaps *overlaps, int index, int mod);
/* How many mods a line names: 1 for a line about one mod alone (starter
 * pools the game leaves out), else 2 or more. */
int Mods_OverlapModCount(const ModsOverlaps *overlaps, int index);
void Mods_OverlapText(const ModsOverlaps *overlaps, int index, char *out, size_t size);
/* What the line is about, alone ("Card 'Kuriboh'"), and the mods' ids in
 * load order, comma-separated, for tests and tools. */
void Mods_OverlapLabel(const ModsOverlaps *overlaps, int index, char *out, size_t size);
void Mods_OverlapMods(const ModsOverlaps *overlaps, int index, char *out, size_t size);
/* How it comes out, as one word the FM Editor's check uses too: "later",
 * "after", "agree", "add", "reset", "fixed", "keys", "bytes", "chain",
 * "events", "first", "aimed", "patched", "early", "sold", "written" (a
 * written starter deck wins over the pools) or "dropped" (the pools do not
 * draw the forty cards of a deck) (overlap.c, outcome_words). */
const char *Mods_OverlapOutcome(const ModsOverlaps *overlaps, int index);
/* "Cards", "Drops and decks"... */
const char *Mods_OverlapKindName(int kind);

#endif
