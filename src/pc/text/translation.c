/* Translations (text.h, notes/translation.md). */
#define _DEFAULT_SOURCE /* MAP_ANONYMOUS, MAP_FIXED_NOREPLACE */
#include "pc/compat/fs.h" /* a mod's text under a folder named with accents */
#include "text.h"
#include "glyphs.h"
#include "hd_text.h"
#include "listing.h"
#include "language.h"
#include "pc/cards/cards.h"
#include "pc/mods/mods.h"
#include "pc/mods/json.h"
#include "pc/platform/paths.h"
#include "pc/platform/settings.h"
#include "pc/cards/tables.h"
#include "pc/cards/drops.h"
#include "pc/cards/passwords.h"
#include "pc/cards/stars.h"
#include "pc/cards/pack_shop.h"
#include "pc/free_duel/duelists.h"
#include "pc/free_duel/page_box.h"
#include "pc/saves/deck_menu.h"
#include "pc/debug/log.h"
#include "pc/debug/crash.h"
#include "pc/guest/low_memory.h"
#include "game/card_constants.h"
#include "game/duel_side_state.h"
#include "pc/compat/mman.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOTES_PER_FILE 20

static const uint32_t bases[TEXT_BANK_COUNT] = {0x801B0000u, 0x801C0000u, 0x801D0000u};

static const unsigned char **overrides;   /* by string id */
static TextUnit **units;
static int unit_count;
/* Game > Language's text (language.h): under the mods' strings, and not
 * among the units a mod's labels are looked for in, since its labels are
 * the PAL banks' offsets, not the US ones a mod means. */
static TextUnit *language_unit;

/* Where the compiled text lives. The game keeps pointers into it in its
 * own memory (a text box's cursor, the streams it jumps between), and a
 * save state keeps them as they were: on the heap they would name another
 * place at the next launch, which the heap lays out differently (the
 * folder names alone move it), and a state loaded mid-dialogue read
 * garbage. So the text is copied into one region at a fixed address, unit
 * after unit in the order they are compiled, which a launch with the same
 * language and mods repeats (a state with another language or other mods
 * is refused: state.c). The card shop's menu, compiled on first use, comes
 * after the others either way. 3D Monsters' arenas sit at 0x90000000 for
 * the same reason; the interpreter's stack at 0x9FF00000. If the region
 * cannot be had, the text stays on the heap, as before; on the 64-bit build,
 * whose heap may be above 4 GB where the game's 4-byte pointers cannot
 * reach, it goes to the low memory region instead (pc/guest/low_memory.h). */
#define ARENA_BASE 0x9C000000u
#define ARENA_SIZE 0x01000000u
#define ARENA_ALIGN 16u
static unsigned char *arena;
static size_t arena_used;
static int unpinned; /* a unit is outside the region: on the heap, or in low memory */
#ifdef MEMORIES_LOW_MEMORY
static int low_fallback; /* in low memory (low_memory.h), the region being full */
#define OFF_REGION (low_fallback ? "partly in the low memory region (the text region is full)" : "on the heap")
#else
#define OFF_REGION "on the heap"
#endif

static unsigned char *arena_take(size_t size)
{
    static int tried;
    unsigned char *at;
    if (!tried) {
        void *wanted = (void *)(uintptr_t)ARENA_BASE, *got;
        tried = 1;
        got = mmap(wanted, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (got != MAP_FAILED && got != wanted) munmap(got, ARENA_SIZE); /* taken as a hint */
        if (got == wanted) {
            arena = got;
        } else {
            fprintf(stderr, "memories-pc: no room for the text at 0x%08x; save states made in a dialogue with a "
                            "translation may not load\n", ARENA_BASE);
        }
    }
    /* One byte more than asked: Text_Retarget takes a unit's end as its
     * own, so the next unit must not start there. */
    if (!arena || size >= ARENA_SIZE - arena_used) return NULL;
    at = arena + arena_used;
    arena_used += (size + 1 + ARENA_ALIGN - 1) & ~(size_t)(ARENA_ALIGN - 1);
    return at;
}

/* A unit just compiled, into the region (above); its jumps within itself
 * follow it. Jumps into an earlier unit are into the region already, and
 * the retail text stays where it is. */
static void pin(TextUnit *unit)
{
    unsigned char *old = unit->data, *moved;
    int i;
    if (!unit->size) return;
    if (!(moved = arena_take(unit->size))) {
        unpinned = 1;
#ifdef MEMORIES_LOW_MEMORY
        /* One byte more, as arena_take gives. */
        low_fallback = 1;
        if (!(moved = Memories_LowAlloc(unit->size + 1))) {
            fprintf(stderr, "memories-pc: no room below 4 GB for %lu bytes of text\n", (unsigned long)unit->size);
            Crash_ReportFatal("text", "no room below 4 GB for the compiled text");
            exit(1);
        }
#else
        return;
#endif
    }
    memcpy(moved, old, unit->size);
    for (i = 0; i < unit->target_count; i++) {
        uintptr_t target = (uintptr_t)unit->targets[i];
        if (target >= (uintptr_t)old && target - (uintptr_t)old <= unit->size) {
            unit->targets[i] = moved + (target - (uintptr_t)old);
        }
    }
    free(old);
    unit->data = moved; /* not freed again: a unit, once added, stays */
}

/* The startup text's layout (Text_Layout), once Text_Build is done. A
 * state holds addresses inside it: the same language and mods give the
 * same layout, but a pack, a mod's text file or the port's own strings
 * changed since (none of them in the mods' signature) move them. */
static TextLayout layout;

static unsigned crc32_update(unsigned crc, const unsigned char *data, size_t length)
{
    size_t i;
    int bit;
    crc = ~crc;
    for (i = 0; i < length; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void measure_layout(void)
{
    size_t bytes = 0;
    int i;
    layout.crc = 0;
    for (i = -1; i < unit_count; i++) {
        TextUnit *unit = i < 0 ? language_unit : units[i];
        unsigned size;
        if (!unit || !unit->size) continue;
        size = (unsigned)unit->size;
        layout.crc = crc32_update(layout.crc, (const unsigned char *)&size, sizeof(size));
        layout.crc = crc32_update(layout.crc, unit->data, unit->size);
        bytes += unit->size;
    }
    layout.base = arena && arena_used && !unpinned ? ARENA_BASE : 0;
    layout.used = (unsigned)(layout.base ? arena_used : bytes);
    if (layout.used) {
        LOG(LOG_MODS, "text: %u bytes %s, CRC-32 %08x", layout.used,
            layout.base ? "in the region at 0x9C000000" : OFF_REGION, layout.crc);
    }
}

typedef struct {
    const char *mod, *file;
    int notes;
} Reporting;

static void report(void *context, int line, const char *message)
{
    Reporting *reporting = context;
    LOG(LOG_MODS, "text: %s: %s:%d: %s", reporting->mod, reporting->file, line, message);
    if (reporting->notes++ < NOTES_PER_FILE) Mods_Note(reporting->mod, "%s, line %d: %s", reporting->file, line, message);
    else if (reporting->notes == NOTES_PER_FILE + 1) Mods_Note(reporting->mod, "%s: more problems left out", reporting->file);
}

static char *read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    char *text;
    long size;
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return NULL;
    }
    text = malloc((size_t)size + 1);
    if (text && fread(text, 1, (size_t)size, file) != (size_t)size) {
        free(text);
        text = NULL;
    }
    fclose(file);
    if (text) {
        text[size] = '\0';
        *length = (size_t)size;
    }
    return text;
}

static void add_unit(int mod, const char *path, const char *name)
{
    Reporting reporting = {Mods_Id(mod), name, 0};
    size_t length = 0;
    char *text = read_file(path, &length);
    TextUnit *unit, **bigger;
    int i;
    if (!text) {
        Mods_Note(Mods_Id(mod), "\"text\": cannot read %s", name);
        return;
    }
    unit = TextListing_Compile(text, length, bases, units, unit_count, Glyphs_Code, report, &reporting);
    free(text);
    if (!unit) {
        /* The compiler has said why, when it knows (a UTF-16 file). */
        if (!reporting.notes) Mods_Note(Mods_Id(mod), "\"text\": %s could not be read", name);
        return;
    }
    if (unit->not_utf8_lines) {
        /* Last, so it is what the Mods window shows: the likeliest reason
         * the whole translation looks wrong. */
        LOG(LOG_MODS, "text: %s: %s: %d lines are not UTF-8, from line %d", Mods_Id(mod), name, unit->not_utf8_lines,
            unit->first_not_utf8_line);
        Mods_Note(Mods_Id(mod), "%s is not UTF-8 (%d lines, from line %d): save it as UTF-8", name,
                  unit->not_utf8_lines, unit->first_not_utf8_line);
    }
    bigger = realloc(units, (size_t)(unit_count + 1) * sizeof(*units));
    if (!overrides) overrides = calloc(0x10000, sizeof(*overrides));
    if (!bigger || !overrides) {
        TextListing_Free(unit);
        return;
    }
    units = bigger;
    units[unit_count++] = unit;
    pin(unit);
    /* A later file, or a later mod, has the last word on a string. */
    for (i = 0; i < unit->string_count; i++) overrides[unit->strings[i].id] = unit->data + unit->strings[i].offset;
    LOG(LOG_MODS, "text: %s: %s: %d strings, %lu bytes, %d jumps, at %p", Mods_Id(mod), name, unit->string_count,
        (unsigned long)unit->size, unit->target_count, (void *)unit->data);
}

static void report_language(void *context, int line, const char *message)
{
    (void)context;
    LOG(LOG_MODS, "text: the language's listing, line %d: %s", line, message);
}

static void add_language(void)
{
    size_t length = 0;
    char *listing = Language_Listing(&length);
    int i;
    if (!listing) return;
    language_unit = TextListing_Compile(listing, length, bases, NULL, 0, Glyphs_Code, report_language, NULL);
    free(listing);
    if (!overrides) overrides = calloc(0x10000, sizeof(*overrides));
    if (!language_unit || !overrides) {
        TextListing_Free(language_unit);
        language_unit = NULL;
        Language_Drop();
        return;
    }
    pin(language_unit);
    for (i = 0; i < language_unit->string_count; i++) {
        overrides[language_unit->strings[i].id] = language_unit->data + language_unit->strings[i].offset;
    }
    LOG(LOG_MODS, "text: %s: %d strings, %lu bytes, %d jumps, at %p", Language_Label(Language_Current()),
        language_unit->string_count, (unsigned long)language_unit->size, language_unit->target_count,
        (void *)language_unit->data);
}

void Text_Build(void)
{
    static int built;
    char path[1200];
    int i, index;
    if (built) return;
    built = 1;
    /* Fonts first: the text may need their letters. */
    for (i = 0; i < Mods_LoadedCount(); i++) {
        int mod = Mods_Loaded(i);
        const char *name;
        if (!Mods_Active(mod)) continue;
        for (index = 0; Mods_File(mod, "font", index, path, sizeof(path), &name); index++) {
            if (path[0]) Glyphs_AddFont(path);
        }
    }
    /* The official language first: a mod's string stands over its. */
    add_language();
    for (i = 0; i < Mods_LoadedCount(); i++) {
        int mod = Mods_Loaded(i);
        const char *name;
        if (!Mods_Active(mod)) continue;
        for (index = 0; Mods_File(mod, "text", index, path, sizeof(path), &name); index++) {
            if (path[0]) add_unit(mod, path, name);
        }
    }
    measure_layout();
}

TextLayout Text_Layout(void) { return layout; }

/* View > Opponent's name for COM (hd_text.h) names the sides after the
 * duel too: strings 0x3E (YOU, or 1P in a 2P duel) and 0x3F (COM, or 2P)
 * become the player's name and the opponent's short name, as the
 * life-point panel has them, and 0x3D, which picks the winner's of the four
 * ({f8 18}), becomes the winner's name (gDuel_bWinnerSide). The result
 * screens call them by their place in the dialogue bank (TEXT_*_AT,
 * Text_Retarget) rather than by id. Only against the computer (an opponent
 * id), and only when every letter has a glyph of one byte; else the game's
 * own. */
#define TEXT_YOU_FIRST 0x3D
#define TEXT_YOU 0x3E
#define TEXT_COM 0x3F
#define TEXT_YOU_FIRST_AT 0x0504
#define TEXT_YOU_AT 0x050E
#define TEXT_COM_AT 0x051C
/* The result screens' strings, and how far a letter of their font goes. */
#define TEXT_RESULTS_FIRST 0x40
#define TEXT_RESULTS_LAST 0x45
/* The bank's bytes the result strings lie in (YOU and COM, 0x3D-0x3F, then
 * 0x40-0x45 to 0x9A5): copied whole, as 0x41 and 0x42 jump into 0x40's
 * tail, where COM's column is set. */
#define TEXT_RESULTS_FROM 0x0500
#define TEXT_RESULTS_SIZE 0x0500
#define TEXT_RESULTS_LETTER 7
/* The cell every letter of that font takes ({f8 04 01}), whatever the
 * letter: the player's name moves by whole cells, so it ends exactly where
 * YOU did, over its column of numbers. */
#define TEXT_RESULTS_CELL 8

/* The opponent's name as the result screens' small font can show it in
 * COM's column: the panel's name when it is letters and spaces only and at
 * most TEXT_RESULTS_WIDE of them, else its longest word of letters alone
 * (G. Sebek: Sebek, Teana 2nd: Teana, Simon Muran: Simon); the font has no
 * full stop and other digits. NULL when even that is too long. */
#define TEXT_RESULTS_WIDE 9

/* A letter of Latin-1, which the opponent's name is in (Text_OpponentName):
 * A-Z, a-z or an accented one. */
static int latin_letter(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c != 0xD7 && c != 0xF7);
}

static const char *results_name(void)
{
    static char name[TEXT_RESULTS_WIDE + 1];
    const char *whole = Text_OpponentName(Tables_OpponentId()), *c, *best = NULL;
    int plain = 1, best_length = 0;
    if (!whole) return NULL;
    for (c = whole; *c; c++) plain &= latin_letter((unsigned char)*c) || *c == ' ';
    if (plain && strlen(whole) <= TEXT_RESULTS_WIDE) return whole;
    for (c = whole; *c;) {
        const char *start = c;
        int letters = 1;
        while (*c && *c != ' ') letters &= latin_letter((unsigned char)*c++);
        if (letters && c - start > best_length) best = start, best_length = (int)(c - start);
        while (*c == ' ') c++;
    }
    if (!best || best_length > TEXT_RESULTS_WIDE) return NULL;
    memcpy(name, best, (size_t)best_length);
    name[best_length] = '\0';
    return name;
}

static int side_letters(void)
{
    const char *name = results_name();
    return name ? (int)strlen(name) : 0;
}

/* The player's name (HdText_PlayerName, as YOU's box has it) when it is
 * letters, digits, spaces and ':' alone; else You. The result screens'
 * small font has no . ! ? $ & * % @, and draws - / + in the blue of the
 * game's own dots. */
static const char *results_player(void)
{
    const char *name = HdText_PlayerName(), *c;
    for (c = name; *c; c++) {
        if (!isalnum((unsigned char)*c) && *c != ' ' && *c != ':') return "You";
    }
    return name;
}

/* `name` in glyph codes, ended by 0xFF, in texts[which]; NULL when a letter
 * has none of one byte. */
static const unsigned char *side_text(const char *name, int which)
{
    static unsigned char texts[2][40];
    unsigned char *out = texts[which];
    int i, n = 0;
    if (!name) return NULL;
    for (i = 0; name[i] && n < (int)sizeof(texts[0]) - 1; i++) {
        /* An accented letter as its plain one: the small font has none.
         * A letter made whole (ß, æ, þ...) has no plain one (its base is
         * 0, the space): the screen keeps COM. */
        int code = Glyphs_Code((unsigned char)name[i]), base = Glyphs_Base(code);
        if (code >= GLYPHS_EXTENDED_FIRST && base == 0) return NULL;
        code = base;
        if (code < 0 || code >= 0xF0) return NULL;
        out[n++] = (unsigned char)code;
    }
    out[n] = 0xFF; /* the string's end */
    return out;
}

static const unsigned char *side_name(int id)
{
    if (id < TEXT_YOU_FIRST || id > TEXT_COM || !Settings_Get(SET_OPPONENT_NAME)) return NULL;
    if (!Text_OpponentName(Tables_OpponentId())) return NULL;
    if (id == TEXT_COM || (id == TEXT_YOU_FIRST && gDuel_bWinnerSide)) return side_text(results_name(), 1);
    return side_text(results_player(), 0);
}

/* How far side_name(id) runs past the three letters of YOU or COM, in
 * pixels of the result screens (less than 0 for a shorter one). */
static int side_shift(int id)
{
    const unsigned char *name = side_name(id);
    int n = 0;
    if (!name) return 0;
    while (name[n] != 0xFF) n++;
    return (n - 3) * TEXT_RESULTS_CELL;
}

/* The result screens set COM's column with {f8 02 NN}, a step right from
 * the end of YOU's, then call COM. For a longer name the copy of the
 * strings steps that much less, so the name ends where COM did; a name the
 * step cannot make room for stays COM (results_room). The player's name
 * ends where YOU did: the copy steps less (more for a shorter name) before
 * each call to YOU (results_you), and the winner's where YOU or COM did,
 * before WINNER on the line that calls it (results_won). A name whose step
 * cannot change stays You, and a computer's win COM. */
static unsigned char results[TEXT_RESULTS_SIZE];
static int results_room = 0x7FFF, results_you = 0, results_won = 0;

/* Moves what the copy draws before the call at `at` on its line `shift`
 * pixels left: the nearest {f8 02 NN} before it with only letters and
 * spaces between steps that much less; else, after four spaces, the last
 * three become such a step. 0 if neither can. */
static int step_before(unsigned char *copy, int at, int shift)
{
    int i, step;
    for (i = at - 3; i >= 0; i--) {
        if (copy[i] == 0xF8 && copy[i + 1] == 0x02) {
            step = copy[i + 2] - shift;
            if (step < 0 || step > 0xFF) return 0;
            copy[i + 2] = (unsigned char)step;
            return 1;
        }
        if (copy[i + 2] >= 0xF0) break; /* a control byte */
    }
    step = 3 * TEXT_RESULTS_CELL - shift;
    if (at < 4 || copy[at - 4] || copy[at - 3] || copy[at - 2] || copy[at - 1] || step < 0 || step > 0xFF) return 0;
    copy[at - 3] = 0xF8;
    copy[at - 2] = 0x02;
    copy[at - 1] = (unsigned char)step;
    return 1;
}

/* Where the string at `retail` starts in the copy, or NULL. */
static const unsigned char *results_copy(const unsigned char *retail)
{
    unsigned char *copy = results;
    uintptr_t offset = (uintptr_t)retail & 0xFFFF;
    int shift = (side_letters() - 3) * TEXT_RESULTS_LETTER, i, room = 0x7FFF;
    int you = side_shift(TEXT_YOU), won = side_shift(TEXT_YOU_FIRST);
    results_room = 0x7FFF;
    results_you = results_won = 0;
    if (!side_name(TEXT_COM) || (shift <= 0 && !you && !won) ||
        ((uintptr_t)retail & 0xFFFF0000u) != bases[TEXT_BANK_DIALOG] ||
        offset < TEXT_RESULTS_FROM || offset >= TEXT_RESULTS_FROM + TEXT_RESULTS_SIZE) {
        return NULL;
    }
    memcpy(copy, (const unsigned char *)(uintptr_t)(bases[TEXT_BANK_DIALOG] + TEXT_RESULTS_FROM), TEXT_RESULTS_SIZE);
    /* {f8 02 NN} with a call to COM in the bytes after it. */
    for (i = 0; i + 8 < TEXT_RESULTS_SIZE; i++) {
        int k, calls = 0;
        if (copy[i] != 0xF8 || copy[i + 1] != 0x02) continue;
        for (k = i + 3; k < i + 8; k++) {
            if ((copy[k] == (TEXT_COM_AT & 0xFF) && copy[k + 1] == TEXT_COM_AT >> 8) ||
                (copy[k] == TEXT_COM_AT >> 8 && copy[k + 1] == (TEXT_COM_AT & 0xFF))) {
                calls = 1;
            }
        }
        if (!calls) continue;
        if (copy[i + 2] < room) room = copy[i + 2];
        if (shift > 0) copy[i + 2] = (unsigned char)(copy[i + 2] > shift ? copy[i + 2] - shift : 0);
    }
    /* A call ({fc}) or jump ({fd}) to YOU, or to the winner's name. */
    results_you = results_won = 1;
    for (i = 0; i + 2 < TEXT_RESULTS_SIZE; i++) {
        unsigned target = copy[i + 1] | copy[i + 2] << 8;
        if (copy[i] != 0xFC && copy[i] != 0xFD) continue;
        if (target == TEXT_YOU_AT && you) results_you &= step_before(copy, i, you);
        if (target == TEXT_YOU_FIRST_AT && won) results_won &= step_before(copy, i, won);
    }
    results_room = room;
    return room >= shift ? copy + (offset - TEXT_RESULTS_FROM) : NULL;
}

int Text_Overridden(int id) { return overrides && id >= 0 && id <= 0xFFFF && overrides[id]; }

const unsigned char *Text_Own(int id) { return Text_Overridden(id) ? overrides[id] : NULL; }

/* A translation's string `id` in Latin-1, to `out` (`size` bytes): 0 when
 * it has none, -1 when it has a character past Latin-1, or a code other
 * than a letter or a space. */
static int latin_text(int id, char *out, size_t size)
{
    const unsigned char *text = Text_Own(id);
    size_t n = 0;
    if (!text) return 0;
    while (*text != 0xFF) {
        int code = *text++;
        uint32_t character;
        if (code >= 0xF6) return -1;
        if (code >= 0xF0) {
            if (*text == 0xFF) return -1;
            code = ((code - 0xF0) << 8) | *text++;
        }
        character = Glyphs_Character(code);
        if (!character || character > 0xFF) return -1;
        if (n + 1 < size) out[n++] = (char)character;
    }
    out[n] = '\0';
    return 1;
}

/* The names bank's names of the duelists: 0x8328 + id. */
#define TEXT_DUELIST_NAMES 0x8328

const char *Text_OpponentName(int duelist)
{
    static char name[TABLES_SHORT_NAME_LIMIT + 1];
    /* By duelist, the added ones too: Tables_DuelistShortName answers for
       any duelist this run has (duelists.h). */
    static int said[DUELIST_TABLE_COUNT];
    const char *english = Tables_DuelistShortName(duelist);
    char text[64];
    const unsigned char *c;
    int got;
    if (!english || duelist < 1 || duelist >= DUELIST_TABLE_COUNT) return NULL;
    if (Tables_DuelistRenamed(duelist)) return english;
    /* A translation's own: letters, spaces and full stops, cut to the limit.
     * Only for the disc's own thirty-nine -- TEXT_OWN_OPPONENT holds one id
     * each for those and no more (text.h), and a duelist a mod added is named
     * by the mod rather than by a translation. */
    got = duelist < TABLES_DUELIST_COUNT ? latin_text(TEXT_OWN_OPPONENT + duelist, text, sizeof(text)) : 0;
    for (c = (const unsigned char *)text; got > 0 && *c; c++) {
        if (!latin_letter(*c) && *c != ' ' && *c != '.') got = -1;
    }
    if (got > 0) {
        size_t n = strlen(text);
        if (n > TABLES_SHORT_NAME_LIMIT) {
            if (!(said[duelist] & 1)) {
                LOG(LOG_MODS, "text: string %04X has %d letters; the first %d are shown", TEXT_OWN_OPPONENT + duelist,
                    (int)n, TABLES_SHORT_NAME_LIMIT);
            }
            said[duelist] |= 1;
            n = TABLES_SHORT_NAME_LIMIT;
        }
        while (n && text[n - 1] == ' ') n--;
        if (n) {
            memcpy(name, text, n);
            name[n] = '\0';
            return name;
        }
    } else if (got < 0 && !(said[duelist] & 2)) {
        said[duelist] |= 2;
        LOG(LOG_MODS, "text: string %04X is not letters, spaces and full stops of Latin-1; not used",
            TEXT_OWN_OPPONENT + duelist);
    }
    /* The translation's full name, when it changed the English. */
    if (duelist < TABLES_DUELIST_COUNT &&
        latin_text(TEXT_DUELIST_NAMES + duelist, text, sizeof(text)) > 0 &&
        strcmp(text, Duelists_Name(duelist)) && Tables_ShortenName(text, name)) {
        return name;
    }
    return english;
}

static void report_own(void *context, int line, const char *message)
{
    (void)context;
    LOG(LOG_MODS, "text: the port's own listing, line %d: %s", line, message);
}

const unsigned char *Text_CompileOwn(const char *listing, int id, size_t *size)
{
    TextUnit *unit = TextListing_Compile(listing, strlen(listing), bases, units, unit_count, Glyphs_Code, report_own,
                                         NULL);
    TextUnit **bigger;
    int i;
    if (!unit) return NULL;
    bigger = realloc(units, (size_t)(unit_count + 1) * sizeof(*units));
    if (!bigger) {
        TextListing_Free(unit);
        return NULL;
    }
    units = bigger;
    units[unit_count++] = unit;
    pin(unit);
    LOG(LOG_MODS, "text: the port's own listing: %d strings, %lu bytes, at %p", unit->string_count,
        (unsigned long)unit->size, (void *)unit->data);
    for (i = 0; i < unit->string_count; i++) {
        if (unit->strings[i].id == id) {
            *size = unit->size - unit->strings[i].offset;
            return unit->data + unit->strings[i].offset;
        }
    }
    return NULL;
}

/* Strings the game writes while it runs: the player's name (0x125A), the
 * two names a two-player screen loads (0x122B, 0x1238) and the Password
 * screen's eight digits (string 0xFD, Password_RefreshDigitDisplay). A mod's
 * string for one is a copy of the placeholder (FM Editor imports from before
 * the listing knew 0x1245 write 0xFD as blanks): the game would show it, not
 * what it wrote. */
static int game_buffer(const unsigned char *retail)
{
    uintptr_t at = (uintptr_t)retail;
    return at == 0x801B122Bu || at == 0x801B1238u || at == 0x801B1245u || at == 0x801B125Au;
}

const unsigned char *Text_Resolve(int id, const unsigned char *retail)
{
    const unsigned char *own = overrides && id >= 0 && id <= 0xFFFF ? overrides[id] : NULL;
    if (own && game_buffer(retail)) {
        static unsigned char told[0x10000 / 8];
        if (!(told[id >> 3] & (1 << (id & 7)))) {
            told[id >> 3] |= (unsigned char)(1 << (id & 7));
            LOG(LOG_MODS, "text: [%04X] is a place the game writes (the player's name, the Password screen's "
                          "digits): the mod's string for it is left out", id);
        }
        own = NULL;
    }
    const unsigned char *card = NULL, *side = side_name(id), *drops = CardDrops_Text(id), *shop = DeckMenu_Text(id);
    const unsigned char *page = FreeDuelPage_Text(id), *packs = PackShop_Text(id);
    if (page) return page;   /* the Free Duel grid's page (free_duel/page_box.h) */
    if (packs) return packs; /* the card packs on the Password screen (cards/pack_shop.h) */
    if (drops) return drops; /* the results screen's added pages (drops.h) */
    if (shop) return shop;   /* the card shop's menu with DECK SLOTS (deck_menu.h) */
    if (CardPassword_Text(id)) return CardPassword_Text(id); /* View > Card passwords (passwords.h) */
    if (side) return side;
    /* A retail card a mod's "cards" replaced: its name and text, over a
     * translation's (cards.h). */
    if (id > 0x8000 && id <= 0x8000 + CARD_COUNT) card = Cards_NameText(id - 0x8000);
    if (id > 0xD100 && id <= 0xD100 + CARD_COUNT) card = Cards_DescriptionText(id - 0xD100);
    if (card) return card;
    /* A guardian star a mod names (stars.h): its "name" over a translation's;
     * a new star with none, a translation's, else "Star N" in place of the
     * Dragon the names bank has at 11-15's places. */
    if (id > STARS_NAME_TEXT && id <= STARS_NAME_TEXT + STARS_MAX && Stars_NameText(id - STARS_NAME_TEXT) &&
        (!own || Stars_Named(id - STARS_NAME_TEXT)))
        return Stars_NameText(id - STARS_NAME_TEXT);
    if (!own && id >= TEXT_RESULTS_FIRST && id <= TEXT_RESULTS_LAST) {
        const unsigned char *copy = results_copy(retail);
        if (copy) return copy;
    }
    return own ? own : retail;
}

int Text_CutsMenuGlyph(int id, int x, int width, int y, int line_height, int height, int lines_left)
{
    static unsigned char told[0x10000 / 8];
    if (x < width) return 0;
    /* The line wraps (TextBox_WrapLineIfNeeded) and every line still to
     * come but the last needs a row under it: while that fits the box,
     * the game goes on as the console does. */
    if (y + (lines_left + 1) * line_height <= height) return 0;
    if (id >= 0 && id <= 0xFFFF && !(told[id >> 3] & (1 << (id & 7)))) {
        told[id >> 3] |= (unsigned char)(1 << (id & 7));
        LOG(LOG_MODS, "text: [%04X] has a line wider than its box (%d pixels), which stops the game; "
                      "cut at the edge", id, width);
    }
    return 1;
}

unsigned char *Text_Retarget(unsigned char *cursor, unsigned target)
{
    int i;
    {   /* The card packs' question: its answer ends the text (pack_shop.h). */
        unsigned char *packs = PackShop_Retarget(cursor);
        if (packs) return packs;
    }
    int copied = cursor >= results && cursor < results + sizeof(results);
    if (copied || ((uintptr_t)cursor & 0xFFFF0000u) == bases[TEXT_BANK_DIALOG]) {
        /* The retail result screens calling YOU, COM or the winner (COM
         * only when its column made room for the name). */
        int id = target == TEXT_COM_AT ? TEXT_COM
               : target == TEXT_YOU_AT ? TEXT_YOU
               : target == TEXT_YOU_FIRST_AT ? TEXT_YOU_FIRST : -1;
        const unsigned char *side = side_name(id);
        /* The player's or the winner's name only where the copy moved it to
         * end where YOU did; else You, or for the computer's win the
         * game's own pick ({f8 18}: COM). */
        if (side && id != TEXT_COM && side_shift(id) && !(copied && (id == TEXT_YOU ? results_you : results_won))) {
            side = id == TEXT_YOU_FIRST && gDuel_bWinnerSide ? NULL : side_text("You", 0);
        }
        if (side && (target != TEXT_COM_AT || side_letters() <= 3 ||
                     results_room >= (side_letters() - 3) * TEXT_RESULTS_LETTER)) {
            return (unsigned char *)side;
        }
        /* A jump from the copy stays in it where the copy has the place,
         * else lands in the bank it was copied from. */
        if (copied) {
            unsigned place = target & 0xFFFF;
            if (place >= TEXT_RESULTS_FROM && place < TEXT_RESULTS_FROM + TEXT_RESULTS_SIZE) {
                return results + (place - TEXT_RESULTS_FROM);
            }
            return (unsigned char *)(uintptr_t)(bases[TEXT_BANK_DIALOG] | place);
        }
    }
    for (i = -1; i < unit_count; i++) {
        TextUnit *unit = i < 0 ? language_unit : units[i];
        if (unit && cursor >= unit->data && cursor <= unit->data + unit->size) {
            /* An operand past the unit's targets (a hand-edited byte) ends
             * the stream rather than jumping anywhere. */
            return target < (unsigned)unit->target_count ? unit->targets[target] : unit->data + unit->size - 1;
        }
    }
    return (unsigned char *)(((uintptr_t)cursor & 0xFFFF0000u) | (target & 0xFFFF));
}

/* --- the order of the card names --------------------------------------- */

extern short gCard_asNameSortKey[];
#define NAME_OFFSETS 0x801D5800u
#define NAME_BANK 0x801D0000u
#define SORT_LETTERS 40

typedef struct {
    int id;
    uint32_t key[SORT_LETTERS];
} Sorted;

static int by_name(const void *left, const void *right)
{
    const Sorted *a = left, *b = right;
    int i;
    for (i = 0; i < SORT_LETTERS; i++) {
        if (a->key[i] != b->key[i]) return a->key[i] < b->key[i] ? -1 : 1;
        if (!a->key[i]) break;
    }
    return a->id - b->id;
}

/* The next character of a name's glyph codes (0 at its end), advancing. */
static uint32_t name_character(const unsigned char **at)
{
    int code = *(*at)++;
    if (code >= 0xF6) return 0;
    if (code >= 0xF0) code = ((code - 0xF0) << 8) | *(*at)++;
    return Glyphs_Character(code);
}

/* Whether the text gives retail card `id` a name other than the disc's,
 * character for character: a translation that lists a name as it was (the
 * English cards over a PAL language, say) renames nothing. */
static int text_renames(int id)
{
    const unsigned char *name = overrides ? overrides[0x8000 + id] : NULL;
    const unsigned char *retail = (const unsigned char *)(uintptr_t)(
        NAME_BANK + ((const uint16_t *)(uintptr_t)NAME_OFFSETS)[id]);
    uint32_t a, b;
    if (!name) return 0;
    do {
        a = name_character(&name);
        b = name_character(&retail);
    } while (a == b && a);
    return a != b;
}

void Text_SortCards(void)
{
    Sorted *cards;
    int id, renamed = 0;
    /* Only when a name changed: the disc's order (gCard_asNameSortKey) is
     * not quite the one by_name gives (it passes over hyphens: M-warrior #1
     * after Mushroom Man), so the names as they were keep it. */
    for (id = 1; id <= CARD_COUNT; id++) renamed |= text_renames(id) || Cards_NameText(id);
    if (!renamed) return;
    cards = calloc((size_t)gCard_nCount, sizeof(*cards));
    if (!cards) return;
    for (id = 1; id <= gCard_nCount; id++) {
        int base = Cards_BaseId(id), n = 0;
        const unsigned char *name = Cards_NameText(id);
        if (!name) {
            const unsigned char *retail = (const unsigned char *)(uintptr_t)(
                NAME_BANK + ((const uint16_t *)(uintptr_t)NAME_OFFSETS)[base]);
            name = Text_Resolve(0x8000 + base, retail);
        }
        cards[id - 1].id = id;
        while (n < SORT_LETTERS - 1 && *name < 0xF6) {
            int code = *name++;
            if (code >= 0xF0) code = ((code - 0xF0) << 8) | *name++;
            cards[id - 1].key[n++] = Glyphs_SortCharacter(code);
        }
    }
    qsort(cards, (size_t)gCard_nCount, sizeof(*cards), by_name);
    for (id = 0; id < gCard_nCount; id++) gCard_asNameSortKey[cards[id].id - 1] = (short)(id + 1);
    free(cards);
    LOG(LOG_MODS, "text: cards sorted by their translated names");
}
