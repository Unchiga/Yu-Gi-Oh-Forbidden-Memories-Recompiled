/* More duelists than the disc has (duelists.h, notes/more-duelists.md).
 *
 * Each applied mod's "duelists" is read into the list here, once, in the order
 * the mods load. A new duelist is a copy of a retail one: it has the base's
 * portrait, deck pool, drop pools and AI, and its own name. Nothing about the
 * disc changes, so every reader that indexes a disc-shaped table goes through
 * Duelists_BaseId and gets a real record back.
 *
 * What the base gives is then edited through the tables a mod already has:
 * "drops" and "decks" name these as readily as the retail ones, so an added
 * duelist with its own deck is a "duelists" entry and a "decks" entry for it.
 */
#include "duelists.h"
#include "game/card_constants.h"
#include "pc/cards/cards.h"
#include "pc/cards/tables.h"
#include "game/campaign_flags.h"
#include "pc/mods/mods.h"
#include "pc/mods/json.h"
#include "pc/debug/log.h"
#include "pc/platform/paths.h"
#include "pc/compat/fs.h"
#include "pc/cards/art.h"
#include "pc/render/texture_pack.h"
#include "pc/text/glyphs.h"
#include "pc/guest/low_memory.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DUELIST_NAME_MAX 32
/* An entry's key -- its "id", or its file's name in "duelists/" -- is under
 * KEY_MAX, as a pool file's in "decks/" and "drops/" is (tables.c); its
 * identity is "<mod>:<key>", and a mod id is under 64 too. */
#define KEY_MAX 64
#define IDENTITY_MAX (64 + KEY_MAX)
/* A condition names a duelist or a card the way a manifest does anywhere
 * else: an id, a name or an identity, so it wants room for the longest of
 * those rather than for a duelist's name. */
#define REFERENCE_MAX 64

typedef struct {
    char name[DUELIST_NAME_MAX];
    char identity[IDENTITY_MAX];
    int base;
    signed char ai[DUELIST_AI_FIELDS];
    signed char sight;             /* 1 reads face-down cards, 0 does not, -1 the base's */
    unsigned char has_ai;          /* else its base's row */
    unsigned char *portrait;       /* PORTRAIT_RECORD bytes, or NULL for its base's */
    char *art;                     /* the PNG it was made from, for the scaled picture */
    /* "ranks": how a duel against it is scored, five threshold/change pairs
     * per rule over what its block holds. Given to the tables once the
     * duelist has an id (tables.h). */
    short ranks[TABLES_RANK_RULE_COUNT * TABLES_RANK_STEPS * 2];
    unsigned char rank_given[TABLES_RANK_RULE_COUNT];
    unsigned char *glyphs;         /* the name in the game's codes, or NULL */
    /* "unlock" (duelists.h). The duelist and the card are kept as they were
     * written and looked up when they are tested: either may belong to a mod
     * that loads after this one, and the card list is not built yet when a
     * manifest is read. */
    char beat[REFERENCE_MAX];      /* a duelist to have beaten, or "" */
    char card[REFERENCE_MAX];      /* a card to hold, or "" */
    int wins;                      /* wins wanted: against `beat`, else in all */
    int copies;                    /* copies of `card` wanted */
    int story;                     /* a campaign story flag, or -1 for none */
    unsigned char has_unlock;      /* else shown from the start */
    /* Read from the manifest, settled once every mod has been read. The AI
     * row a duelist is taken from may be another mod's, so like the unlock's
     * conditions it is kept as it was written. */
    char ai_copy[REFERENCE_MAX];   /* a duelist to take the row from, or "" */
    unsigned char ai_given[DUELIST_AI_FIELDS]; /* which numbers the entry set */
    int slot;                      /* the id asked for, or -1 for the next free */
    int replace;                   /* a stock duelist to take over, or -1 */
    const char *mod;               /* whose entry it is, for a note */
    int index;                     /* its place in that mod's list, for a note */
    unsigned char used;            /* this record stands for a duelist */
} Duelist;

/* The list is addressed by slot, not filled in order: an entry may ask for an
 * id of its own, which leaves the slots before it empty until something takes
 * them. An empty slot is no duelist -- Duelists_Valid says so, and the grid
 * draws nothing in its cell, as it does for a locked one. */
static Duelist *added;             /* by slot: ids DUELISTS_RETAIL_COUNT and up */
static int added_top, added_room;  /* one past the highest slot in use */

/* A stock duelist a mod took over: its name, face, way of playing and when it
 * appears, over the disc's. Its deck, its drops and everything else the disc
 * lays out stay its own, which is what "decks" and "drops" are for. */
static Duelist replaced[DUELISTS_RETAIL_COUNT];

/* Every entry every mod wrote, in the order they were read. Slots are handed
 * out from this once the last mod has been read, so that an entry asking for
 * an id is not beaten to it by one that would have been happy anywhere. */
static Duelist *pending;
static int pending_count, pending_room;

/* The list is made once, from the mods applied; Duelists_Clear undoes that. */
static int built;

/* The record standing for a duelist: a mod's, or none. */
static Duelist *entry_at(int duelist)
{
    if (duelist < 0) return NULL;
    if (duelist < DUELISTS_RETAIL_COUNT) return replaced[duelist].used ? &replaced[duelist] : NULL;
    duelist -= DUELISTS_RETAIL_COUNT;
    if (duelist >= added_top || !added) return NULL;
    return added[duelist].used ? &added[duelist] : NULL;
}

/* Letters and digits only, either case: the spelling a manifest uses for a
 * duelist need not match the table's punctuation. */
static int same_letters(const char *a, const char *b)
{
    while (*a && *b) {
        while (*a && !isalnum((unsigned char)*a)) a++;
        while (*b && !isalnum((unsigned char)*b)) b++;
        if (!*a || !*b) break;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    while (*a && !isalnum((unsigned char)*a)) a++;
    while (*b && !isalnum((unsigned char)*b)) b++;
    return !*a && !*b;
}

int Duelists_Count(void)
{
    return DUELISTS_RETAIL_COUNT + added_top;
}

int Duelists_Valid(int duelist)
{
    if (duelist < 0 || duelist >= Duelists_Count()) return 0;
    /* A slot nothing was placed in is no duelist, though it is inside the
     * list: it keeps the cells after it where their mods asked for them. */
    return duelist < DUELISTS_RETAIL_COUNT || entry_at(duelist) != NULL;
}

int Duelists_BaseId(int duelist)
{
    const Duelist *one;
    if (duelist < 0 || duelist >= Duelists_Count()) return 0;
    /* A stock duelist is its own base, replaced or not: what a mod takes over
     * is the face, the name and the way of playing, never where the disc is
     * read. */
    if (duelist < DUELISTS_RETAIL_COUNT) return duelist;
    one = entry_at(duelist);
    return one ? one->base : 0;
}

const char *Duelists_Name(int duelist)
{
    const Duelist *one = entry_at(duelist);
    if (one && one->name[0]) return one->name;
    if (duelist >= 0 && duelist < DUELISTS_RETAIL_COUNT) return Tables_DuelistNames[duelist];
    if (!one) return Tables_DuelistNames[0];
    return Tables_DuelistNames[one->base];
}

const char *Duelists_Identity(int duelist)
{
    const Duelist *one = duelist < DUELISTS_RETAIL_COUNT ? NULL : entry_at(duelist);
    return one ? one->identity : NULL;
}

int Duelists_Find(const char *identity)
{
    int i;
    if (!identity || !*identity) return -1;
    for (i = 0; i < added_top; i++) {
        if (added[i].used && !strcmp(added[i].identity, identity)) return DUELISTS_RETAIL_COUNT + i;
    }
    /* A stock duelist a mod took over answers to the identity that took it,
     * so a mod's own pool files name it the way its duelists/ file did. */
    for (i = 0; i < DUELISTS_RETAIL_COUNT; i++) {
        if (replaced[i].used && !strcmp(replaced[i].identity, identity)) return i;
    }
    return -1;
}

/* --- a duelist's own face and name --------------------------------------- */

/* The game's own table, one nine-byte row per duelist the disc has
 * (game/ai_opponent_data.h). Spelled out rather than included: that header
 * brings in the whole of ygo_types.h for a layout this states itself, and the
 * size of a row is asserted there. */
extern signed char gDuel_aOpponentData[][DUELIST_AI_FIELDS];

int Duelists_HidesFaceDown(int duelist, int script)
{
    const Duelist *one = entry_at(duelist);
    /* Said outright, or left to the script, which settled it from the base's
       id (AiScript_LoadOpponentID). */
    if (!one || one->sight < 0) return script;
    return !one->sight;
}

const signed char *Duelists_AiRow(int duelist)
{
    const Duelist *one = entry_at(duelist);
    if (one && one->has_ai) return one->ai;
    return gDuel_aOpponentData[Duelists_BaseId(duelist)];
}

const unsigned char *Duelists_Portrait(int duelist)
{
    const Duelist *one = entry_at(duelist);
    return one ? one->portrait : NULL;
}

/* The text id the disc names a duelist by: FreeDuel_PlaceCursor and the duel's
 * reward step both put `id - 31960` in D_8009B32E, which for duelist 1 reads
 * back as 0x8329. Only the disc's own thirty-nine are in it -- 0x8350 onwards
 * are the campaign's location names. */
#define DUELIST_NAME_TEXT_BASE 0x8328
/* An added duelist's, which nothing on the disc answers for: one id each from
 * 0xFF00 up, clear of the disc's strings, of the port's own block
 * (TEXT_OWN_FIRST to TEXT_OWN_LAST, text.h) and of the composed ids handed out
 * from 0xFFFF down (cards/drops.h, cards/passwords.h, page_box.h). The last
 * duelist an id can name, 127, lands at 0xFF57. */
#define DUELIST_NAME_TEXT_ADDED 0xFF00

int Duelists_NameTextId(int duelist)
{
    if (duelist < 0 || duelist >= DUELIST_ID_LIMIT) return 0;
    if (duelist < DUELISTS_RETAIL_COUNT) return DUELIST_NAME_TEXT_BASE + duelist;
    return DUELIST_NAME_TEXT_ADDED + (duelist - DUELISTS_RETAIL_COUNT);
}

const unsigned char *Duelists_Text(int id, const unsigned char *text)
{
    int duelist;

    /* A stock duelist a mod took over answers on the disc's own id, which is
     * what makes its new name the one the screen and the duel both show. */
    if (id > DUELIST_NAME_TEXT_BASE && id < DUELIST_NAME_TEXT_BASE + DUELISTS_RETAIL_COUNT)
        duelist = id - DUELIST_NAME_TEXT_BASE;
    else if (id >= DUELIST_NAME_TEXT_ADDED &&
             id < DUELIST_NAME_TEXT_ADDED + DUELIST_ID_LIMIT - DUELISTS_RETAIL_COUNT)
        duelist = id - DUELIST_NAME_TEXT_ADDED + DUELISTS_RETAIL_COUNT;
    else
        return text;                 /* not a name: the game's string stands */
    {
        const Duelist *one = entry_at(duelist);
        return one && one->glyphs ? one->glyphs : text;
    }
}

/* A name in the game's own glyph codes, ending 0xFF, as cards.c makes one for
 * a card, in low memory: the game keeps the text in a 4-byte pointer. NULL
 * when nothing could be made of it. */
static unsigned char *name_glyphs(const char *mod, const char *text)
{
    size_t length = strlen(text);
    unsigned char *glyphs = Memories_LowAlloc(length * 2 + 1);
    const char *at = text;
    int bad = 0;

    if (!glyphs) return NULL;
    length = 0;
    while (*at) {
        const char *letter = at;
        uint32_t character = Glyphs_NextCharacter(&at);
        int code;
        if (character == GLYPHS_NOT_UTF8) {
            if (!bad++) Mods_Note(mod, "a duelist name is not UTF-8; save the file as UTF-8");
            continue;
        }
        code = Glyphs_Code(character);
        if (code < 0) {
            Mods_Note(mod, "the game has no letter \"%.*s\"; left out of a duelist name",
                      (int)(at - letter), letter);
            continue;
        }
        if (code >= 0xF0) {
            glyphs[length++] = (unsigned char)(0xF0 + (code >> 8));
            glyphs[length++] = (unsigned char)code;
        } else {
            glyphs[length++] = (unsigned char)code;
        }
    }
    if (!length) { Memories_LowFree(glyphs); return NULL; }
    glyphs[length] = 0xFF;
    return glyphs;
}

/* --- what a save holds of a duelist ------------------------------------- */

/* The win/loss records of the duelists past the grid's forty, and whether the
 * grid shows them (src/pc/game/free_duel_storage.c: a game unit, so a save
 * state carries them). The first forty of each are the game's own. */
extern unsigned short gFreeDuel_aExtraRecords[DUELIST_TABLE_COUNT][2];
extern unsigned char gFreeDuel_abExtraAvailable[DUELIST_TABLE_COUNT];
/* Whose the records are: the duelist code of the save they came from. */
extern int gFreeDuel_nExtraOwner;
extern unsigned char gFreeDuel_abGridAvailable[];

/* The save block's records. save_data.h pins the offset with a static assert
 * (SaveDataState_duelist_records_offset_must_be_0x51C); it is spelled out
 * here because that header does not compile outside a game unit, which is why
 * cards.c addresses the save by offset too. */
#define SAVE_DUELIST_RECORDS 0x51C

unsigned short *Duelists_RecordSlot(void *state, int duelist)
{
    /* A slot for an id with nowhere to live: reads zero, and what is written
     * to it is forgotten, as Cards_ChestSlot does for a card. */
    static unsigned short nowhere[2];

    nowhere[0] = nowhere[1] = 0;
    if (!Duelists_Valid(duelist)) return nowhere;
    if (duelist < DUELISTS_RETAIL_COUNT) {
        if (!state) return nowhere;
        return (unsigned short *)((unsigned char *)state + SAVE_DUELIST_RECORDS +
                                  duelist * FREE_DUEL_GRID_RECORD_SIZE);
    }
    if (duelist >= DUELIST_TABLE_COUNT) return nowhere;
    return gFreeDuel_aExtraRecords[duelist];
}

/* Which page of forty the grid is showing (free_duel_storage.c: a game unit,
 * so a save state carries it). */
extern int gFreeDuel_nPage;

int Duelists_AtCell(int cell)
{
    return gFreeDuel_nPage * DUELISTS_RETAIL_COUNT + cell;
}

int Duelists_Available(int duelist)
{
    if (!Duelists_Valid(duelist)) return 0;
    if (duelist < DUELISTS_RETAIL_COUNT) return gFreeDuel_abGridAvailable[duelist] != 0;
    return duelist < DUELIST_TABLE_COUNT && gFreeDuel_abExtraAvailable[duelist] != 0;
}

void Duelists_SetAvailable(int duelist, int shown)
{
    if (!Duelists_Valid(duelist)) return;
    if (duelist < DUELISTS_RETAIL_COUNT) gFreeDuel_abGridAvailable[duelist] = (unsigned char)(shown != 0);
    else if (duelist < DUELIST_TABLE_COUNT) gFreeDuel_abExtraAvailable[duelist] = (unsigned char)(shown != 0);
}

/* --- when an added duelist appears (duelists.h) --------------------------- */

/* Defined with the manifest reading below, and used here: a condition names a
 * duelist the same way an entry names its base. */

/* Wins the save at `state` has against one duelist, or against all of them
 * when `duelist` is negative. Duelist 0 is Deck Build, never an opponent. */
static int wins_against(const void *state, int duelist)
{
    int total = 0, id;
    if (duelist >= 0) return Duelists_RecordSlot((void *)state, duelist)[0];
    for (id = 1; id < Duelists_Count(); id++) total += Duelists_RecordSlot((void *)state, id)[0];
    return total;
}

/* Copies of a card the save holds, trunk and deck, as cards/drops.c counts
 * them for a drop. A SaveDataState begins with the deck. */
static int copies_held(void *state, int card)
{
    const unsigned short *deck = (const unsigned short *)state;
    int count = *Cards_ChestSlot(state, card), i;
    for (i = 0; i < DECK_SIZE; i++) count += deck[i] == card;
    return count;
}

int Duelists_HasUnlock(int duelist)
{
    const Duelist *one = entry_at(duelist);
    return one && one->has_unlock;
}

int Duelists_Unlocked(const void *state, int duelist)
{
    const Duelist *one;

    if (!Duelists_Valid(duelist)) return 0;
    one = entry_at(duelist);
    /* No conditions means nothing to fail. A stock duelist is then left to the
     * campaign, whose story flag the screen tests for itself; one a mod gave
     * conditions to is answered here instead, and the screen leaves its flag
     * alone (Duelists_HasUnlock). */
    if (!one || !one->has_unlock) return 1;
    return Duelists_ConditionsMet(state, one->beat, one->wins, one->story, one->card, one->copies);
}

int Duelists_ConditionsMet(const void *state, const char *beat, int wins, int story, const char *card, int copies)
{
    if (!state) return 0;
    if (story >= 0 && Campaign_TestStoryFlag(story) == 0) return 0;
    if (beat && beat[0]) {
        /* A duelist that is not here this run -- its mod was turned off, or
         * the name is a misspelling -- leaves the condition unmet rather than
         * met, so a roster never opens up by accident. */
        const int against = Duelists_Named(beat);
        if (against < 0 || wins_against(state, against) < (wins > 0 ? wins : 1)) return 0;
    } else if (wins > 0 && wins_against(state, -1) < wins) {
        return 0;
    }
    if (card && card[0]) {
        const int id = Cards_Named(card);
        if (id <= 0 || copies_held((void *)state, id) < (copies > 0 ? copies : 1)) return 0;
    }
    return 1;
}

/* --- the sidecar ---------------------------------------------------------
 *
 * The added duelists' records have no room in the memory card's block, so
 * they are kept beside it in the user directory, by the duelist code and save
 * sequence the save carries -- the same arrangement cards.c keeps the added
 * cards' trunk in, and for the same reason.
 *
 * The file is sections, newest last, one per save written. A save made while
 * no duelist mod was applied has no section of its own and reads the newest
 * earlier one, which is what the player had when they last played with the
 * mod. Records are written by identity, never by id: an id is only what a
 * duelist happens to be this run, and the load order decides it.
 */
#define KEPT_SAVES 8                    /* sections kept per duelist code */
#define SAVE_DUELIST_CODE 0x334
#define SAVE_SEQUENCE 0x404

extern unsigned short gDuel_awPlayerDeck[];   /* the running save's SaveDataState */

static int state_word(const void *state, int offset)
{
    int value;
    memcpy(&value, (const unsigned char *)state + offset, sizeof(value));
    return value;
}

static int sidecar_path(char *out, size_t size, int code)
{
    char relative[64];
    snprintf(relative, sizeof relative, "duelists/%08X.txt", (unsigned)code);
    return Paths_User(out, size, relative);
}

static int section_header(const char *line, unsigned *sequence)
{
    return sscanf(line, "save %u", sequence) == 1;
}

/* The rest of a line, without its newline: an identity may hold spaces, so it
 * is written last and read whole rather than by a scanf field. */
static void copy_line(char *out, size_t size, const char *text)
{
    size_t n = 0;
    while (text[n] && text[n] != '\n' && text[n] != '\r' && n + 1 < size) n++;
    memcpy(out, text, n);
    while (n && out[n - 1] == ' ') n--;
    out[n] = '\0';
}

/* Which section a save of `sequence` reads: its own, else the newest no later
 * than it. 0 when there is none. */
static int choose_section(FILE *file, unsigned sequence, unsigned *chosen)
{
    char line[256];
    int have = 0;
    rewind(file);
    while (fgets(line, sizeof line, file)) {
        unsigned value;
        if (!section_header(line, &value) || value > sequence) continue;
        if (!have || value > *chosen) { *chosen = value; have = 1; }
    }
    rewind(file);
    return have;
}

static void clear_extra(void)
{
    memset(gFreeDuel_aExtraRecords, 0, sizeof gFreeDuel_aExtraRecords);
}

/* Is anything worth writing? A save with no added duelist leaves the file
 * alone, so ordinary play never makes one. */
static int any_record(void)
{
    int id;
    for (id = DUELISTS_RETAIL_COUNT; id < Duelists_Count() && id < DUELIST_TABLE_COUNT; id++) {
        if (gFreeDuel_aExtraRecords[id][0] || gFreeDuel_aExtraRecords[id][1]) return 1;
    }
    return 0;
}

void Duelists_SaveLoaded(const void *state)
{
    const int code = state_word(state, SAVE_DUELIST_CODE);
    const unsigned sequence = (unsigned)state_word(state, SAVE_SEQUENCE);
    char path[1024], line[256];
    unsigned chosen = 0;
    int inside = 0, read = 0;
    FILE *file;

    clear_extra();
    gFreeDuel_nExtraOwner = code;
    if (Duelists_Count() <= DUELISTS_RETAIL_COUNT) return;
    if (sidecar_path(path, sizeof path, code)) return;
    file = fopen(path, "r");
    if (!file) return;
    if (choose_section(file, sequence, &chosen)) {
        while (fgets(line, sizeof line, file)) {
            char identity[IDENTITY_MAX];
            unsigned value;
            int wins, losses, id, at = 0;
            if (section_header(line, &value)) { inside = value == chosen; continue; }
            if (!inside) continue;
            /* The identity is last on the line because it may hold spaces:
               duelists/Dark Simon.json is "<mod>:Dark Simon". */
            if (sscanf(line, "record %d %d %n", &wins, &losses, &at) < 2 || at <= 0) continue;
            copy_line(identity, sizeof identity, line + at);
            if (!*identity) continue;
            /* A record whose duelist is not here this run is dropped: the mod
             * that had it may come back, but its id would be another's. */
            id = Duelists_Find(identity);
            if (id < DUELISTS_RETAIL_COUNT || id >= DUELIST_TABLE_COUNT) continue;
            gFreeDuel_aExtraRecords[id][0] = (unsigned short)(wins < 0 ? 0 : wins);
            gFreeDuel_aExtraRecords[id][1] = (unsigned short)(losses < 0 ? 0 : losses);
            read++;
        }
    }
    fclose(file);
    if (read) LOG(LOG_MODS, "duelists: %d records of duelist %08X save %u (from save %u)",
                  read, (unsigned)code, sequence, chosen);
}

void Duelists_Frame(void)
{
    /* NEW GAME writes a new duelist code into the running save, and loads
       nothing, so this is where the change shows: the records belong to the
       save that code came from. The cards' trunk is cleared the same way
       (cards.c's Cards_Frame). */
    const int code = state_word(gDuel_awPlayerDeck, SAVE_DUELIST_CODE);

    if (code != gFreeDuel_nExtraOwner) {
        clear_extra();
        gFreeDuel_nExtraOwner = code;
    }
}

void Duelists_SaveWritten(const void *state, unsigned sequence)
{
    const int code = state_word(state, SAVE_DUELIST_CODE);
    char path[1024], temporary[1040], line[256], directory[1024];
    unsigned kept[KEPT_SAVES];
    int kept_count = 0, keep = 0, i, id;
    FILE *in, *out;
    char *slash;

    if (Duelists_Count() <= DUELISTS_RETAIL_COUNT) return;
    if (sidecar_path(path, sizeof path, code)) return;
    in = fopen(path, "r");
    if (!in && !any_record()) return;    /* nothing to say about this save */

    /* Every section but this save's own stays, the newest KEPT_SAVES of them;
     * this save's is written fresh below. */
    if (in) {
        while (fgets(line, sizeof line, in)) {
            unsigned value;
            if (!section_header(line, &value) || value == sequence) continue;
            if (kept_count < KEPT_SAVES - 1) kept[kept_count++] = value;
            else {
                int oldest = 0;
                for (i = 1; i < kept_count; i++) if (kept[i] < kept[oldest]) oldest = i;
                if (value > kept[oldest]) kept[oldest] = value;
            }
        }
        rewind(in);
    }

    snprintf(directory, sizeof directory, "%s", path);
    slash = strrchr(directory, '/');
    if (slash) { *slash = '\0'; Paths_MakeDirs(directory); }
    snprintf(temporary, sizeof temporary, "%s.tmp", path);
    Paths_WriteBegin();
    out = fopen(temporary, "w");
    if (!out) {
        char why[1200];
        Paths_WriteError(why, sizeof why, path);
        if (in) fclose(in);
        fprintf(stderr, "memories-pc: cannot write %s\n", why);
        return;
    }

    fprintf(out, "# The duelists mods added, as the saves of duelist %08X hold them.\n", (unsigned)code);
    while (in && fgets(line, sizeof line, in)) {
        unsigned value;
        if (section_header(line, &value)) {
            keep = 0;
            for (i = 0; i < kept_count; i++) keep |= kept[i] == value;
        }
        if (keep) fputs(line, out);
    }
    fprintf(out, "save %u\n", sequence);
    for (id = DUELISTS_RETAIL_COUNT; id < Duelists_Count() && id < DUELIST_TABLE_COUNT; id++) {
        const char *identity = Duelists_Identity(id);
        if (!identity || (!gFreeDuel_aExtraRecords[id][0] && !gFreeDuel_aExtraRecords[id][1])) continue;
        fprintf(out, "record %u %u %s\n", gFreeDuel_aExtraRecords[id][0],
                gFreeDuel_aExtraRecords[id][1], identity);
    }
    if (in) fclose(in);
    {
        int failed = ferror(out);
        if (fclose(out)) failed = 1;
        if (failed || rename(temporary, path)) {
            char why[1200];
            Paths_WriteError(why, sizeof why, path); /* before remove() changes the reason */
            remove(temporary);
            fprintf(stderr, "memories-pc: cannot replace %s\n", why);
        }
    }
}

/* A duelist a manifest names: an id, a name from the list, or the identity of
 * one this or an earlier mod added. The tables name a duelist the same way, so
 * this is the list's own answer and not a second copy of the rules
 * (duelists.h). */
int Duelists_Named(const char *text)
{
    int id;
    if (!text || !*text) return -1;
    if (strspn(text, "0123456789") == strlen(text)) return Duelists_Valid(id = atoi(text)) ? id : -1;
    if ((id = Duelists_Find(text)) >= 0) return id;
    for (id = 0; id < Duelists_Count(); id++) {
        /* An empty slot answers with the first name in the table, which would
         * make it match "Deck Build"; it is nobody, so it is skipped. */
        if (!Duelists_Valid(id)) continue;
        if (same_letters(text, Duelists_Name(id))) return id;
        /* A stock duelist a mod renamed still answers to the name the disc
         * gave it, so another mod's "decks" or "drops" keeps naming it. */
        if (id < DUELISTS_RETAIL_COUNT && same_letters(text, Tables_DuelistNames[id])) return id;
    }
    return -1;
}

/* Everything an entry owns. Every path that drops one -- a rejected entry, a
 * replacement taken over by a later mod, an entry with nowhere to go, and
 * Duelists_Clear -- comes through here, so a field added to the record cannot
 * be forgotten by one of them. */
static void release(Duelist *one)
{
    free(one->portrait);
    Memories_LowFree(one->glyphs);
    free(one->art);
    one->portrait = NULL;
    one->glyphs = NULL;
    one->art = NULL;
}

/* Whether an identity is spoken for, by a duelist already placed or by an
 * entry still waiting for a place. */
static int taken_identity(const char *identity)
{
    int i;
    if (Duelists_Find(identity) >= 0) return 1;
    for (i = 0; i < pending_count; i++) {
        if (!strcmp(pending[i].identity, identity)) return 1;
    }
    return 0;
}

/* One duelist: an object as "duelists" holds them, or as a file of its own
 * in "duelists/" holds one. `named` is the identity the file's own name
 * gives it, NULL when the entry names itself. `index` is for the notes. */
static void read_one_duelist(const char *mod, const char *mod_directory, const JsonValue *entry, int index,
                             const char *named)
{
    const char *copy = Json_String(Json_Member(entry, "copy"), NULL);
    const char *name = Json_String(Json_Member(entry, "name"), NULL);
    const char *key = named ? named : Json_String(Json_Member(entry, "id"), NULL);
    const JsonValue *over = Json_Member(entry, "replace");
    const JsonValue *where = Json_Member(entry, "slot");
    Duelist one;
    int base, target = -1;

    if (Json_TypeOf(entry) != JSON_OBJECT) {
        Mods_Note(mod, "duelists[%d]: an object, with a copy and a name", index);
        return;
    }
    /* Cut short, it would be another duelist's, or no pool file's. */
    if (key && strlen(key) >= KEY_MAX) {
        Mods_Note(mod, "duelists[%d]: id %.20s... is longer than %d letters", index, key, KEY_MAX - 1);
        return;
    }
    /* "replace" takes over one of the disc's own instead of adding a
     * duelist: it is the entry's base as well, since a replacement still
     * reads the disc where that duelist does. */
    if (over) {
        target = Duelists_Named(Json_String(over, NULL));
        if (target <= 0 || target >= DUELISTS_RETAIL_COUNT) {
            Mods_Note(mod, "duelists[%d]: replace names no stock duelist (1-%d)",
                      index, DUELISTS_RETAIL_COUNT - 1);
            return;
        }
        if (copy) Mods_Note(mod, "duelists[%d]: replace and copy together; copy is left out", index);
    }
    /* The base is what the disc answers for: without one there is no
     * portrait, no deck and no AI to fall back on. */
    base = over ? target : Duelists_Named(copy);
    if (base <= 0 || base >= DUELISTS_RETAIL_COUNT) {
        Mods_Note(mod, "duelists[%d]: copy names no retail duelist (1-%d)",
                  index, DUELISTS_RETAIL_COUNT - 1);
        return;
    }

    memset(&one, 0, sizeof one);
    one.base = base;
    one.story = -1;
    one.sight = -1;                /* the base's, until "ai" says otherwise */
    one.replace = target;
    one.slot = -1;
    one.mod = mod;
    one.index = index;
    one.used = 1;
    /* "slot" asks for an id of its own: which page the grid shows it on
     * and where. Meaningless for a replacement, which has the stock
     * duelist's place. */
    if (where && target >= 0) {
        Mods_Note(mod, "duelists[%d]: a replacement keeps its duelist's place; slot is left out", index);
    } else if (where) {
        const int asked = (int)Json_Number(where, -1);
        if (asked < DUELISTS_RETAIL_COUNT || asked >= DUELIST_TABLE_COUNT) {
            Mods_Note(mod, "duelists[%d]: slot is an id from %d to %d", index, DUELISTS_RETAIL_COUNT,
                      DUELIST_TABLE_COUNT - 1);
        } else {
            one.slot = asked;
        }
    }
    if (name) {
        snprintf(one.name, sizeof one.name, "%s", name);
        one.glyphs = name_glyphs(mod, name);
    }
    if (!one.glyphs && target < 0) {
        /* An added duelist without a name of its own: its base's, made here
         * rather than left to the string id, which for an added duelist is a
         * private one nothing on the disc answers; so too when its name had
         * no letter the font draws. A replacement needs none --
         * it keeps answering on the stock duelist's id, whose string is the
         * name it is taking over. */
        one.glyphs = name_glyphs(mod, Tables_DuelistNames[base]);
    }
    {   /* What the save must meet before the grid shows it (duelists.h).
         * Every member is a requirement and all of them must hold; an
         * entry without the member is shown from the start. Neither the
         * duelist nor the card is looked up here: the card list is not
         * built yet, and either may belong to a mod that loads later. */
        const JsonValue *unlock = Json_Member(entry, "unlock");
        if (unlock && Json_TypeOf(unlock) != JSON_OBJECT) {
            Mods_Note(mod, "duelists[%d]: unlock is an object of conditions", index);
        } else if (unlock) {
            const JsonValue *card = Json_Member(unlock, "card");
            const char *beat = Json_String(Json_Member(unlock, "beat"), NULL);
            const char *named = Json_String(card, NULL);
            if (beat) snprintf(one.beat, sizeof one.beat, "%s", beat);
            if (named) snprintf(one.card, sizeof one.card, "%s", named);
            else if (card) snprintf(one.card, sizeof one.card, "%ld", Json_Number(card, 0));
            one.wins = (int)Json_Number(Json_Member(unlock, "wins"), 0);
            one.copies = (int)Json_Number(Json_Member(unlock, "copies"), 0);
            one.story = (int)Json_Number(Json_Member(unlock, "story"), -1);
            if (one.wins < 0) one.wins = 0;
            if (one.copies < 0) one.copies = 0;
            one.has_unlock = (unsigned char)(one.beat[0] || one.card[0] || one.wins > 0 || one.story >= 0);
            if (!one.has_unlock)
                Mods_Note(mod, "duelists[%d]: unlock names no condition, so it is shown from the start", index);
        }
    }
    {   /* How a duel against it is scored, over what its block holds -- which
         * the disc makes the same for all forty. A rule is up to five
         * [threshold, change] pairs; one left out keeps the disc's. */
        const JsonValue *ranks = Json_Member(entry, "ranks");
        int r;
        if (ranks && Json_TypeOf(ranks) != JSON_OBJECT) {
            Mods_Note(mod, "duelists[%d]: ranks is an object of rules", index);
        } else for (r = 0; r < Json_Count(ranks); r++) {
            const JsonValue *steps = Json_At(ranks, r);
            const int rule = Tables_RankNamed(Json_Name(steps));
            const int count = Json_Count(steps);
            int k;
            if (rule < 0) {
                Mods_Note(mod, "duelists[%d]: ranks \"%s\": no such rule", index, Json_Name(steps));
                continue;
            }
            if (Json_TypeOf(steps) != JSON_ARRAY || count < 1) {
                Mods_Note(mod, "duelists[%d]: ranks \"%s\": up to %d [threshold, change] pairs",
                          index, Json_Name(steps), TABLES_RANK_STEPS);
                continue;
            }
            for (k = 0; k < TABLES_RANK_STEPS; k++) {
                /* Fewer than five is filled out with the last, so a rule that
                 * stops mattering above some value need not be spelled out. */
                const JsonValue *pair = Json_At(steps, k < count ? k : count - 1);
                short *at = one.ranks + (rule * TABLES_RANK_STEPS + k) * 2;
                if (Json_TypeOf(pair) != JSON_ARRAY || Json_Count(pair) < 2) {
                    Mods_Note(mod, "duelists[%d]: ranks \"%s\": pair %d is [threshold, change]",
                              index, Json_Name(steps), k + 1);
                    break;
                }
                at[0] = (short)Json_Number(Json_At(pair, 0), 0);
                at[1] = (short)Json_Number(Json_At(pair, 1), 0);
                if (k == TABLES_RANK_STEPS - 1) one.rank_given[rule] = 1;
            }
        }
    }
    {   /* Its own way of playing, over its base's: up to nine numbers, and
         * what is left out is the base's. An object may name a duelist to
         * take the whole row from instead -- kept as it was written, since
         * a mod that loads later may be the one that adds it. Which
         * numbers were set is kept beside them, so the row underneath can
         * be settled once every mod has been read. */
        const JsonValue *ai = Json_Member(entry, "ai");
        const char *from = Json_String(Json_Member(ai, "copy"), NULL);
        int field;
        if (from) snprintf(one.ai_copy, sizeof one.ai_copy, "%s", from);
        if (Json_TypeOf(ai) == JSON_ARRAY) {
            for (field = 0; field < DUELIST_AI_FIELDS && field < Json_Count(ai); field++) {
                one.ai[field] = (signed char)Json_Number(Json_At(ai, field), 0);
                one.ai_given[field] = 1;
            }
        } else if (Json_TypeOf(ai) == JSON_OBJECT) {
            const JsonValue *search = Json_Member(ai, "search");
            const JsonValue *values = Json_Member(ai, "values");
            if (search) {
                one.ai[0] = (signed char)Json_Number(search, 0);
                one.ai_given[0] = 1;
            }
            for (field = 0; field < DUELIST_AI_FIELDS && field < Json_Count(values); field++) {
                one.ai[field] = (signed char)Json_Number(Json_At(values, field), one.ai[field]);
                one.ai_given[field] = 1;
            }
        } else if (ai) {
            Mods_Note(mod, "duelists[%d]: ai is a list of numbers, or an object", index);
        }
        /* Whether its searches may read a face-down card, which the disc
         * settles in the script's own bytecode from the id (duelists.h). Only
         * the object form can say so -- Json_Member answers nothing for the
         * array, which is the numbers alone. */
        if (Json_Member(ai, "sight")) {
            one.sight = (signed char)(Json_Bool(Json_Member(ai, "sight"), 0) != 0);
        }
        one.has_ai = one.ai_copy[0] != 0;
        for (field = 0; field < DUELIST_AI_FIELDS; field++)
            if (one.ai_given[field]) one.has_ai = 1;
    }
    {   /* Its own face, over its base's: "portraits/<id>.png" beside the
         * manifest, or the path "portrait" names. A PNG of any size: the
         * middle of it is taken at the portrait's shape and reduced to
         * the 64 colours the console's slot holds, and the file itself is
         * kept for the scaled picture to draw at its own size
         * (duelists.h). One file, both pictures. */
        const char *art = Json_String(Json_Member(entry, "portrait"), NULL);
        char path[1024], why[256];
        int have = 0;
        if (art && !Paths_Contained(art)) {
            Mods_Note(mod, "duelists[%d]: portrait %s is not a path inside the mod", index, art);
        } else if (art) {
            have = snprintf(path, sizeof path, "%s/%s", mod_directory, art) < (int)sizeof path;
        } else if (key) {
            /* Quietly, since most entries have no picture of their own:
             * only a file that is there is a portrait. */
            if (snprintf(path, sizeof path, "%s/portraits/%s.png", mod_directory, key) < (int)sizeof path) {
                FILE *file = fopen(path, "rb");
                if (file) {
                    fclose(file);
                    have = 1;
                }
            }
        }
        if (have) {
            unsigned char *record = malloc(PORTRAIT_RECORD);
            if (!record) Mods_Note(mod, "duelists[%d]: out of memory for its portrait", index);
            else if (!CardArt_PortraitFromImage(path, record, why, sizeof why)) {
                Mods_Note(mod, "duelists[%d]: %s", index, why);
                free(record);
            } else {
                one.portrait = record;
                one.art = malloc(strlen(path) + 1);
                if (one.art) strcpy(one.art, path);
            }
        }
    }
    /* An entry without an id of its own is identified by its place, which
     * moves when the list is edited; saves are the reason to give one. */
    if (key) snprintf(one.identity, sizeof one.identity, "%s:%s", mod, key);
    else snprintf(one.identity, sizeof one.identity, "%s:#%d", mod, index);
    if (taken_identity(one.identity)) {
        Mods_Note(mod, "duelists[%d]: %s is already taken", index, one.identity);
        release(&one);
        return;
    }

    if (pending_count == pending_room) {
        const int room = pending_room ? pending_room * 2 : 8;
        Duelist *bigger = realloc(pending, (size_t)room * sizeof(*pending));
        if (!bigger) {
            Mods_Note(mod, "duelists[%d]: out of memory", index);
            release(&one);
            return;
        }
        pending = bigger;
        pending_room = room;
    }
    pending[pending_count++] = one;
}

/* --- a folder of duelists (duelists.h) ------------------------------------
 *
 * One duelist to a file, the file's own name being its id, so a roster is a
 * directory to drop a character into rather than a list to edit. The same
 * directories are read from the player's own folder, where a character need
 * not belong to a mod at all.
 *
 * Read in the order the names sort, never the order the filesystem hands
 * them back: what slot an entry without one gets depends on the order they
 * are read, and a roster must come out the same on every machine.
 */
#define ROSTER_NAMES 256

typedef struct {
    char name[KEY_MAX];
} RosterName;

static int by_name(const void *a, const void *b)
{
    return strcmp(((const RosterName *)a)->name, ((const RosterName *)b)->name);
}

/* The "<key>" of a "<key>.json", or 0 when the file is not one. The save's
 * own sidecars live beside these and end .txt, so they are passed over. */
static int roster_key(const char *file, char *key, size_t size)
{
    const size_t length = strlen(file);
    if (length < 6 || strcmp(file + length - 5, ".json") != 0) return 0;
    if (length - 5 >= size) return 0;
    memcpy(key, file, length - 5);
    key[length - 5] = '\0';
    return 1;
}

/* Every "<key>.json" in `directory`, sorted. Returns how many were found. */
static int roster_names(const char *directory, RosterName *names)
{
    DIR *folder = opendir(directory);
    struct dirent *item;
    int count = 0;
    if (!folder) return 0;
    while ((item = readdir(folder)) != NULL && count < ROSTER_NAMES) {
        if (roster_key(item->d_name, names[count].name, sizeof names[count].name)) count++;
    }
    closedir(folder);
    qsort(names, (size_t)count, sizeof *names, by_name);
    return count;
}

static void read_duelist_folder(const char *mod, const char *directory)
{
    RosterName names[ROSTER_NAMES];
    char path[1024];
    int count, i;

    if (snprintf(path, sizeof path, "%s/duelists", directory) >= (int)sizeof path) return;
    count = roster_names(path, names);
    for (i = 0; i < count; i++) {
        char file[1200], error[256];
        JsonDocument *document;
        if (snprintf(file, sizeof file, "%s/%s.json", path, names[i].name) >= (int)sizeof file) continue;
        document = Json_ParseFile(file, error, sizeof error);
        if (!document) {
            Mods_Note(mod, "duelists/%s.json: %s", names[i].name, error);
            continue;
        }
        /* The file's name is the duelist's, so an entry need not say. */
        read_one_duelist(mod, directory, Json_Root(document), i, names[i].name);
        Json_Free(document);
    }
}

static void read_duelists(const char *mod, const char *mod_directory, const JsonValue *list)
{
    const JsonValue *entry;
    JsonDocument *document = NULL;
    int index;

    if (!list) return;
    /* "duelists" may name a file of the mod's instead of being written out in
     * the manifest, as "drops" and "decks" may: a roster of any size belongs
     * beside the manifest rather than inside it. */
    if (Json_TypeOf(list) == JSON_STRING) {
        const char *relative = Json_String(list, NULL);
        char path[1024], error[256];
        if (!relative || !Paths_Contained(relative)) {
            Mods_Note(mod, "duelists: %s is not a path inside the mod",
                      relative ? relative : "(nothing)");
            return;
        }
        if (snprintf(path, sizeof path, "%s/%s", mod_directory, relative) >= (int)sizeof path) {
            Mods_Note(mod, "duelists: %s is too long a path", relative);
            return;
        }
        document = Json_ParseFile(path, error, sizeof error);
        if (!document) {
            Mods_Note(mod, "duelists: %s: %s", relative, error);
            return;
        }
        list = Json_Root(document);
    }
    if (Json_TypeOf(list) != JSON_ARRAY) {
        Mods_Note(mod, "duelists: a list of duelists to add");
        Json_Free(document);
        return;
    }
    for (index = 0, entry = Json_At(list, 0); entry; entry = Json_Next(entry), index++) {
        read_one_duelist(mod, mod_directory, entry, index, NULL);
    }
    /* Names and portraits are copied into the list, so the document goes. */
    Json_Free(document);
}

/* --- handing out places --------------------------------------------------
 *
 * Every entry is read before any of them is placed, so that one asking for an
 * id of its own is not beaten to it by one that would have been happy
 * anywhere. Two mods asking for the same slot are settled by load order: the
 * earlier keeps it and the later takes the next free one, since a slot is a
 * place on the grid and not an override -- moving the duelist already there
 * would rearrange a roster its own mod laid out. A replacement is an override
 * and goes the other way, as an edited deck or drop pool does: the last mod
 * to name a stock duelist is the one that has it.
 */

/* The added list is indexed from the first added id, so a slot of 40 is its
 * element 0. Grows to hold `slot` and counts it in. */
static int room_for_slot(int slot)
{
    const int at = slot - DUELISTS_RETAIL_COUNT;
    if (slot < DUELISTS_RETAIL_COUNT || slot >= DUELIST_TABLE_COUNT) return 0;
    if (at >= added_room) {
        const int room = at + 1 > 8 ? at + 1 : 8;
        Duelist *bigger = realloc(added, (size_t)room * sizeof(*added));
        if (!bigger) return 0;
        memset(bigger + added_room, 0, (size_t)(room - added_room) * sizeof(*added));
        added = bigger;
        added_room = room;
    }
    if (at >= added_top) added_top = at + 1;
    return 1;
}

/* Put an entry in a slot of the added list. 0 when there was no room. */
static int place_at(const Duelist *one, int slot)
{
    if (!room_for_slot(slot)) return 0;
    added[slot - DUELISTS_RETAIL_COUNT] = *one;
    added[slot - DUELISTS_RETAIL_COUNT].used = 1;
    return 1;
}

static int slot_free(int slot)
{
    const int at = slot - DUELISTS_RETAIL_COUNT;
    return slot >= DUELISTS_RETAIL_COUNT && slot < DUELIST_TABLE_COUNT &&
           (at >= added_top || !added || !added[at].used);
}

/* Settle the AI row: the numbers the entry set, over the row it named or its
 * base's. Every duelist is placed by now, so a row may be another mod's. */
static void settle_ai(Duelist *one)
{
    const signed char *row;
    int borrowed = -1, field;
    if (!one->has_ai) return;
    if (one->ai_copy[0]) {
        borrowed = Duelists_Named(one->ai_copy);
        if (borrowed < 0) Mods_Note(one->mod, "duelists[%d]: ai copy names no duelist", one->index);
    }
    row = borrowed >= 0 ? Duelists_AiRow(borrowed) : gDuel_aOpponentData[one->base];
    for (field = 0; field < DUELIST_AI_FIELDS; field++)
        if (!one->ai_given[field]) one->ai[field] = row[field];
    /* Byte 0 is how many cards from the hand on the AI looks through
       (Ai_GetHandSize): the disc's run from 5 to 20, and its fusion search
       keeps a flag per card for no more than that. A row it borrows is
       one of those, or another mod's already held to them. */
    if (one->ai_given[0] && (one->ai[0] < 5 || one->ai[0] > 20)) {
        Mods_Note(one->mod, "duelists[%d]: ai search is 5 to 20 (%d asked)", one->index, one->ai[0]);
        one->ai[0] = (signed char)(one->ai[0] < 5 ? 5 : 20);
    }
}

/* Its rank rules, once it has an id to give the tables (tables.h). */
static void register_ranks(const Duelist *one, int duelist)
{
    int rule;
    for (rule = 0; rule < TABLES_RANK_RULE_COUNT; rule++) {
        if (one->rank_given[rule])
            Tables_SetRank(duelist, rule, one->ranks + rule * TABLES_RANK_STEPS * 2);
    }
}

/* A duelist's picture, for the scaled picture to draw at its own size: the
 * record's bytes find the PNG as a texture pack's image is found
 * (TexturePack_AddMade), as an added card's picture does (cards.c). A PNG at
 * the console's size or under has no more to show. */
static void register_art(const Duelist *one)
{
    int x, y, cw, ch, width, height;
    if (!one->art || !one->portrait) return;
    if (!CardArt_Crop(one->art, PORTRAIT_SIDE, PORTRAIT_SIDE, &x, &y, &cw, &ch, &width, &height) ||
        (cw <= PORTRAIT_SIDE && ch <= PORTRAIT_SIDE))
        return;
    if (!TexturePack_AddMade(one->portrait, PORTRAIT_SIDE / 2, PORTRAIT_SIDE, 8, one->portrait + PORTRAIT_PIXELS, 64,
                             one->art, x, y, cw, ch))
        Mods_Note(one->mod, "duelists[%d]: its portrait is drawn at the console's size only", one->index);
}

static void place_pending(void)
{
    int i, slot, next = DUELISTS_RETAIL_COUNT;

    /* The stock duelists a mod took over. */
    for (i = 0; i < pending_count; i++) {
        Duelist *one = &pending[i];
        if (one->replace < 0) continue;
        if (replaced[one->replace].used) {
            Mods_Note(one->mod, "duelists[%d]: %s was already replaced; this one has it instead",
                      one->index, Tables_DuelistNames[one->replace]);
            release(&replaced[one->replace]);
        }
        replaced[one->replace] = *one;
        one->used = 0;   /* its portrait and name belong to the list now */
    }
    /* Then the entries that asked for a place of their own. */
    for (i = 0; i < pending_count; i++) {
        Duelist *one = &pending[i];
        if (!one->used || one->slot < 0) continue;
        if (!slot_free(one->slot)) {
            Mods_Note(one->mod, "duelists[%d]: slot %d is taken by %s; this one goes wherever there is room",
                      one->index, one->slot, Duelists_Name(one->slot));
            one->slot = -1;
            continue;
        }
        if (place_at(one, one->slot)) one->used = 0;
    }
    /* Then everybody else, into the lowest free slot, so the places left over
     * by an entry that asked for a high one are not wasted. */
    for (i = 0; i < pending_count; i++) {
        Duelist *one = &pending[i];
        if (!one->used) continue;
        for (slot = next; slot < DUELIST_TABLE_COUNT && !slot_free(slot); slot++) {
        }
        if (slot >= DUELIST_TABLE_COUNT) {
            Mods_Note(one->mod, "duelists[%d]: no room left; the grid holds %d duelists",
                      one->index, DUELIST_TABLE_COUNT);
            release(one);
            one->used = 0;
            continue;
        }
        next = slot + 1;
        if (place_at(one, slot)) one->used = 0;
    }
    /* Anything still standing found no room. */
    for (i = 0; i < pending_count; i++) {
        if (!pending[i].used) continue;
        release(&pending[i]);
    }
    free(pending);
    pending = NULL;
    pending_count = pending_room = 0;

    for (i = 0; i < DUELISTS_RETAIL_COUNT; i++)
        if (replaced[i].used) settle_ai(&replaced[i]);
    for (i = 0; i < added_top; i++)
        if (added[i].used) settle_ai(&added[i]);
    for (i = 0; i < DUELISTS_RETAIL_COUNT; i++)
        if (replaced[i].used) { register_art(&replaced[i]); register_ranks(&replaced[i], i); }
    for (i = 0; i < added_top; i++)
        if (added[i].used) {
            register_art(&added[i]);
            register_ranks(&added[i], DUELISTS_RETAIL_COUNT + i);
        }
}

void Duelists_Clear(void)
{
    int i;
    /* Built again after this: the list is the applied mods', so it is made
       afresh when they change, and by each of the tests' cases. */
    built = 0;
    for (i = 0; i < added_top; i++) release(&added[i]);
    free(added);
    added = NULL;
    added_top = added_room = 0;
    for (i = 0; i < DUELISTS_RETAIL_COUNT; i++) release(&replaced[i]);
    memset(replaced, 0, sizeof replaced);
}

void Duelists_Build(void)
{
    int i, count = 0, over = 0;

    if (built) return;
    built = 1;
    for (i = 0; i < Mods_LoadedCount(); i++) {
        const int mod = Mods_Loaded(i);
        if (!Mods_Active(mod)) continue;
        /* Written out in the manifest, or a file it names; then one file to a
         * duelist in a folder of its own. A mod may do either or both. */
        read_duelists(Mods_Id(mod), Mods_Directory(mod), Json_Member(Mods_Manifest(mod), "duelists"));
        read_duelist_folder(Mods_Id(mod), Mods_Directory(mod));
    }
    /* And the player's own folder, read last so a character dropped in there
     * is placed after the mods' and never moves one of theirs. It belongs to
     * no mod, so its duelists are identified by "user:". */
    if (Paths_UserDir()) read_duelist_folder("user", Paths_UserDir());
    place_pending();
    /* A code mod resolves an identity to the id it has this run through the
       host's duelist_id, as it does a card's (modapi.h). */
    Mods_SetDuelistResolver(Duelists_Find);
    Mods_SetOverlapDuelists(Duelists_Named);
    for (i = 0; i < added_top; i++) count += added[i].used;
    for (i = 0; i < DUELISTS_RETAIL_COUNT; i++) over += replaced[i].used;
    if (count || over)
        LOG(LOG_MODS, "duelists: %d added, %d replaced, %d in all", count, over, Duelists_Count());
}
