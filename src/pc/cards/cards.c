/* More cards than the disc has: the registry (cards.h, notes/more-cards.md).
 *
 * Cards_Build fills the card tables the game reads (src/pc/game/
 * card_storage.c) from the retail executable, then appends the cards every
 * applied mod's "cards" asks for. The rest of this file is what the game's
 * own code calls where a card past the disc's 722 needs something the disc
 * cannot give it: the retail card to borrow from, a trunk and seen marks
 * that fit nowhere in the save, a name, and a share of the drops. */
#define _POSIX_C_SOURCE 200809L
#include "cards.h"
#include "art.h"
#include "card_layout.h"
#include "card_layout_art.h"
#include "card_notes.h"
#include "monster_effects.h"
#include "tables.h"
#include "starter.h"
#include "stars.h"
#include "packs.h"
#include "pc/free_duel/duelists.h"
#include "pc/text/glyphs.h"
#include "pc/text/text.h"
#include "pc/mods/mods.h"
#include "pc/mods/json.h"
#include "pc/platform/paths.h"
#include "pc/platform/settings.h"
#include "pc/debug/log.h"
#include "pc/guest/low_memory.h"
#include "pc/render/texture_dump.h"
#include "pc/render/texture_pack.h"
#include "pc/rng.h"
#include "pc/compat/posix.h"
#include "game/card_constants.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Where the retail tables sit in the executable's image (notes/card-catalog.md).
 * A test that includes this file may put them elsewhere first. */
#ifndef RETAIL_STATS
#define RETAIL_STATS 0x801D4244u        /* s32 [722], by id - 1 */
#define RETAIL_SORT_KEYS 0x801D4D8Eu    /* s16 [722], by id - 1 */
#define RETAIL_LEVEL_ATTR 0x801D5332u   /* u8 [723], by id */
#define RETAIL_NAME_OFFSETS 0x801D5800u /* u16, by 0x8000 + id - 0x8000, from 0x801D0000 */
#define TEXT_BANK 0x801D0000u
#define GLYPH_TABLE 0x801D9000u         /* u32 per glyph code, the Shift-JIS code in the low half */
#endif

/* The save's layout (src/game/save_data.h). */
#define SAVE_CHEST 0x50
#define SAVE_DUELIST_CODE 0x334
#define SAVE_SEQUENCE 0x404
#define SAVE_RECENT 0x5BC               /* gDuel_awRecentCardDrops: 16 card ids, newest first */
#define SAVE_PAIR_BASE 0x801D1200u      /* two-player loads, 0x1000 apart */
#define SAVE_PAIR_STRIDE 0x1000u
#define SAVE_PAIR_COPY 0x680u          /* the copy a trade writes (SAVE_DATA_STATE_SIZE on) */

#define LIBRARY_SEEN_FLAG_BASE 0x120    /* CAMPAIGN_FLAG_LIBRARY_CARD_BASE */
#define KEPT_SAVES 8                    /* sections kept per duelist code */

extern unsigned short gDuel_awPlayerDeck[];   /* the running save's SaveDataState */
extern int gDuel_adwCardStats[];
extern short gCard_asNameSortKey[];
extern unsigned char gDuel_abCardLevelAttr[];
extern int Campaign_TestStoryFlag(int flag);
extern void Library_UpdateCardUsedFlag(int flag);

static char *identities[CARD_TABLE_ID_END];
static const JsonValue *definitions[CARD_TABLE_ID_END];
static unsigned short model_ids[CARD_TABLE_ID_END], effect_ids[CARD_TABLE_ID_END];
/* Threshold plus one: zero inherits the effect's global/retail threshold. */
static unsigned int trap_thresholds[CARD_TABLE_ID_END];
static unsigned char not_exodia[EXODIA_PIECE_COUNT];  /* a replaced piece without Exodia's rules */
/* The frame a card is drawn in when its entry says ("frame"), plus one: 0
 * left out, FRAME_TYPE "Type" (cards.h Cards_FrameColor). */
#define FRAME_TYPE 0xFF
static unsigned char frames[CARD_TABLE_ID_END];
/* "monster_effects" (monster_effects.h): an entry's list, shared by its cards. */
static const MonsterEffect *monster_effects[CARD_TABLE_ID_END];
static unsigned char monster_effect_counts[CARD_TABLE_ID_END];
int Cards_MonsterEffects(int id, const MonsterEffect **effects)
{
    *effects = Cards_Valid(id) ? monster_effects[id] : NULL;
    return *effects ? monster_effect_counts[id] : 0;
}
/* Explicit secondary fusion groups for modded/replaced cards. Zero means inherit the retail base. */
static unsigned int fusion_groups[CARD_TABLE_ID_END];
static unsigned char has_fusion_groups[CARD_TABLE_ID_END];
const char *Cards_Identity(int id) { return id > CARD_COUNT && Cards_Valid(id) && identities[id] ? identities[id] : ""; }
int Cards_FindIdentity(const char *identity)
{
    int id;
    if (!identity || !*identity) return 0;
    for (id = CARD_ID_END; id <= gCard_nCount; id++) if (identities[id] && !strcmp(identities[id], identity)) return id;
    return 0;
}
int Cards_ModelId(int id) { return Cards_Valid(id) && model_ids[id] ? model_ids[id] : Cards_BaseId(id); }
int Cards_EffectId(int id) { return Cards_Valid(id) && effect_ids[id] ? effect_ids[id] : Cards_BaseId(id); }
int Cards_TrapId(int id) { return Cards_Valid(id) && Cards_Type(id) == CARD_TYPE_TRAP ? Cards_EffectId(id) : 0; }
int Cards_TrapThreshold(int id, int fallback)
{
    int effect = Cards_TrapId(id);
    return (unsigned)(effect - DUEL_ATTACK_TRAP_FIRST_CARD_ID) < DUEL_ATTACK_TRAP_COUNT && trap_thresholds[id]
               ? (int)trap_thresholds[id] - 1 : fallback;
}
static int retail_monster(int id);
int Cards_HasModel(int id) { return Cards_Valid(id) && retail_monster(Cards_ModelId(id)); }
int Cards_FrameColor(int id)
{
    if (!Cards_Valid(id) || frames[id] == FRAME_TYPE) return -1;
    /* Left out, a monster with effects of its own is drawn orange, as an
     * effect monster is in the card game. */
    if (!frames[id]) {
        const MonsterEffect *effects;
        /* With the anime frame on (card_layout.h's CardLayout_FullBleed) a
         * ritual spell wears the magic frame's colour, as the card viewer's
         * frame does (hd_assets_pack.py draws no ritual frame of its own), so
         * its hand card and the viewer agree -- unless the layout ships its
         * own ritual frame, which keeps it. */
        if (Cards_Type(id) == CARD_TYPE_RITUAL && CardLayout_RitualWearsMagic()) return CARD_FRAME_MAGIC;
        return Cards_Type(id) < CARD_TYPE_MAGIC && Cards_MonsterEffects(id, &effects) ? CARD_FRAME_ORANGE : -1;
    }
    return frames[id] - 1;
}
int Cards_ExodiaPiece(int id)
{
    return (unsigned)(id - EXODIA_FIRST_CARD_ID) < EXODIA_PIECE_COUNT && !not_exodia[id - EXODIA_FIRST_CARD_ID];
}
static int card_reference(const JsonValue *value)
{
    return Cards_Reference(value);   /* -1 matches no card, and makes none */
}
int Cards_Fusion(int a, int b, int *result)
{
    int side;
    if (!Cards_Valid(a) || !Cards_Valid(b)) return 0;
    for (side = 0; side < 2; side++) {
        const JsonValue *fusions = Json_Member(definitions[side ? b : a], "fusions");
        int i;
        for (i = 0; i < Json_Count(fusions); i++) {
            const JsonValue *rule = Json_At(fusions, i);
            int with = card_reference(Json_Member(rule, "with"));
            if (with == (side ? a : b)) {
                int output = card_reference(Json_Member(rule, "result"));
                if (output && !Cards_Valid(output)) return 0;
                *result = output; return 1;
            }
        }
    }
    return 0;
}

/* Own names and card text, in glyph codes. The game is handed them as text
 * and keeps them in 4-byte pointers: low memory (pc/guest/low_memory.h). */
static unsigned char *names[CARD_TABLE_ID_END];
static unsigned char *descriptions[CARD_TABLE_ID_END];
/* The "notes" of every entry for the card, in load order, a line between
 * two (card_notes.h). Nothing the game draws or plays by. */
static char *card_notes[CARD_TABLE_ID_END];
/* An entry's "password": its eight digits, a nibble each, or
 * CARD_PASSWORD_NONE ("" or null); `own_password` says the entry gave one. */
static unsigned passwords[CARD_TABLE_ID_END];
static unsigned char own_password[CARD_TABLE_ID_END];
/* Own artwork: an art record (art.h) shared by an entry's cards, which of its
 * parts are the card's own, and the card's own title plate. */
#define ART_PICTURE 1
#define ART_THUMBNAIL 2
static unsigned char *art_records[CARD_TABLE_ID_END];
static unsigned char art_parts[CARD_TABLE_ID_END];
static unsigned char *plates[CARD_TABLE_ID_END];
/* Field-only artwork ("field_art"): a card's picture and CLUT (art.h's
 * CARD_ART_PIXELS/CARD_ART_CLUT layout) for its cutout on the duel field
 * alone (mods/3d-monsters/field_art.c). Never patched into a card's own
 * record, so nothing else the card's art shows (Library, hand, trade, the
 * detail panel) is touched by it. NULL: the cutout uses the card's own art. */
static unsigned char *field_art_records[CARD_TABLE_ID_END];
static const char *replaced[CARD_ID_END];          /* the mod that replaced a retail card */
static unsigned short *variants[2];                 /* per use: copies, grouped by base */
static unsigned short variant_start[2][CARD_ID_END + 1];

static void say(const char *format, ...)
{
    char message[512];
    va_list arguments;
    if (!Log_Wanted(LOG_MODS)) return;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    LOG(LOG_MODS, "cards: %s", message);
}

/* --- names ----------------------------------------------------------- */

/* The characters a name may have, as the Shift-JIS code the glyph table
 * lists them under; the retail names are written in the same full-width
 * forms. A space is glyph 0. */
static unsigned sjis_of(int c)
{
    static const char punctuation[] = "!\"#$%&'()*+,-./:<>?";
    static const unsigned short codes[] = {0x8149, 0x8168, 0x8194, 0x8190, 0x8193, 0x8195, 0x8166, 0x8169,
                                           0x816A, 0x8196, 0x817B, 0x8143, 0x817C, 0x8144, 0x815E, 0x8146,
                                           0x8183, 0x8184, 0x8148};
    const char *at;
    if (c >= 'A' && c <= 'Z') return 0x8260u + (unsigned)(c - 'A');
    if (c >= 'a' && c <= 'z') return 0x8281u + (unsigned)(c - 'a');
    if (c >= '0' && c <= '9') return 0x824Fu + (unsigned)(c - '0');
    at = c ? strchr(punctuation, c) : NULL;
    return at ? codes[at - punctuation] : 0;
}

static int glyph_of(int c)
{
    const unsigned *table = (const unsigned *)(uintptr_t)GLYPH_TABLE;
    unsigned sjis;
    int code;
    if (c == ' ') return 0;
    sjis = sjis_of(c);
    if (!sjis) return -1;
    for (code = 1; code < 0x600 && table[code]; code++) {
        if ((table[code] & 0xFFFFu) == sjis) return code;
    }
    return -1;
}

/* A retail name in ASCII, for matching "copy": "Kuriboh" and for the log. */
static void retail_name(int id, char *out, size_t size)
{
    const unsigned short *offsets = (const unsigned short *)(uintptr_t)RETAIL_NAME_OFFSETS;
    const unsigned *table = (const unsigned *)(uintptr_t)GLYPH_TABLE;
    const unsigned char *text = (const unsigned char *)(uintptr_t)(TEXT_BANK + offsets[id]);
    size_t n = 0;
    while (*text != 0xFF && n + 1 < size) {
        int code = *text++;
        int c = '?';
        if (code >= 0xF0) code = ((code - 0xF0) << 8) | *text++;
        if (code == 0) {
            c = ' ';
        } else {
            int probe;
            for (probe = 32; probe < 127; probe++) {
                if (sjis_of(probe) && sjis_of(probe) == (table[code] & 0xFFFFu)) { c = probe; break; }
            }
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';
}

/* A glyph code as the text holds it: one byte, or F1-F5 and a byte for the
 * port's own glyphs. Returns the bytes written. */
static size_t put_glyph(unsigned char *out, int code)
{
    if (code >= 0xF0) {
        out[0] = (unsigned char)(0xF0 + (code >> 8));
        out[1] = (unsigned char)code;
        return 2;
    }
    out[0] = (unsigned char)code;
    return 1;
}

/* "{n}" is the card's number within its entry, "{id}" its card id. */
static unsigned char *encode_name(const char *mod, const char *pattern, int n, int id)
{
    char text[128];
    unsigned char *glyphs;
    size_t length = 0;
    const char *p;
    for (p = pattern; *p && length + 8 < sizeof(text); p++) {
        if (!strncmp(p, "{n}", 3)) { length += (size_t)snprintf(text + length, sizeof(text) - length, "%d", n); p += 2; }
        else if (!strncmp(p, "{id}", 4)) { length += (size_t)snprintf(text + length, sizeof(text) - length, "%d", id); p += 3; }
        else text[length++] = *p;
    }
    text[length] = '\0';
    glyphs = Memories_LowAlloc(length * 2 + 1);
    if (!glyphs) return NULL;
    {   /* UTF-8: accented letters and the like are glyphs of the port's (glyphs.h). */
        const char *at = text;
        int bad = 0;
        length = 0;
        while (*at) {
            const char *letter = at;
            uint32_t character = Glyphs_NextCharacter(&at);
            int code;
            if (character == GLYPHS_NOT_UTF8) {
                if (!bad++) Mods_Note(mod, "card %d: its name is not UTF-8; save the file as UTF-8. Left out", id);
                continue;
            }
            code = Glyphs_Code(character);
            if (code < 0) {
                Mods_Note(mod, "card %d: the game has no letter \"%.*s\"; left out of its name", id,
                          (int)(at - letter), letter);
                continue;
            }
            length += put_glyph(glyphs + length, code);
        }
    }
    glyphs[length] = 0xFF;
    return glyphs;
}

/* Card text, wrapped as the retail texts are: lines of twenty letters at
 * most, broken at spaces (0xFE between them); "\n" breaks where it stands. */
#define TEXT_LINE_LETTERS 20
#define TEXT_LINES 8

/* A code in card text, spelled as the FM Editor and the text listing show
 * it: "{f8 0B NN}" an icon (two letters wide: the card view draws it 16
 * pixels across), "{f8 0A NN}" a colour (none),
 * "{g X}" a glyph by number. Returns the characters it takes, 0 when "at"
 * starts none (and is then read as letters); its bytes go to out. */
static size_t text_code(const char *at, unsigned char out[3], int *bytes, int *letters)
{
    unsigned kind, value;
    int used = 0;
    if (sscanf(at, "{f8 %2x %2x}%n", &kind, &value, &used) == 2 && used && (kind == 0x0A || kind == 0x0B)) {
        out[0] = 0xF8; out[1] = (unsigned char)kind; out[2] = (unsigned char)value;
        *bytes = 3; *letters = kind == 0x0B ? 2 : 0;
        return (size_t)used;
    }
    used = 0;
    if (sscanf(at, "{g %4x}%n", &value, &used) == 1 && used && value < GLYPHS_EXTENDED_LIMIT) {
        *bytes = (int)put_glyph(out, (int)value); *letters = 1;
        return (size_t)used;
    }
    return 0;
}
static unsigned char *encode_description(const char *mod, const char *text, int id)
{
    size_t length = strlen(text), n = 0;
    unsigned char *glyphs = Memories_LowAlloc(length * 2 + 2);
    const char *word = text;
    int column = 0, lines = 1, warned = 0;
    if (!glyphs) return NULL;
    while (*word) {
        const char *end = word;
        int letters;
        if (*word == '\n') {
            glyphs[n++] = 0xFE; lines++; column = 0; word++;
            continue;
        }
        if (*word == ' ') { word++; continue; }
        letters = 0;   /* characters, not bytes */
        while (*end && *end != ' ' && *end != '\n') {
            unsigned char code[3];
            int bytes, wide;
            size_t used = *end == '{' ? text_code(end, code, &bytes, &wide) : 0;
            if (used) { end += used; letters += wide; }
            else letters += ((unsigned char)*end++ & 0xC0) != 0x80;
        }
        if (column && column + 1 + letters > TEXT_LINE_LETTERS) {
            glyphs[n++] = 0xFE; lines++; column = 0;
        } else if (column) {
            glyphs[n++] = 0; column++;   /* glyph 0 is the space */
        }
        while (word < end) {
            const char *letter = word;
            uint32_t character;
            int code, bytes, wide;
            size_t used = *word == '{' ? text_code(word, glyphs + n, &bytes, &wide) : 0;
            if (used) {
                word += used; n += (size_t)bytes; column += wide;
                continue;
            }
            character = Glyphs_NextCharacter(&word);
            if (character == GLYPHS_NOT_UTF8) {
                if (!warned++) Mods_Note(mod, "card %d: its text is not UTF-8; save the file as UTF-8. Left out", id);
                continue;
            }
            code = Glyphs_Code(character);
            if (code < 0) {
                if (!warned++) Mods_Note(mod, "card %d: the game has no letter \"%.*s\"; left out of its text", id,
                                         (int)(word - letter), letter);
                continue;
            }
            n += put_glyph(glyphs + n, code);
            column++;
        }
    }
    glyphs[n] = 0xFF;
    if (lines > TEXT_LINES) Mods_Note(mod, "card %d: its text runs to %d lines; the card view shows %d", id, lines, TEXT_LINES);
    return glyphs;
}

/* The Library's heading, string F8: "<" then the seen count (F8 03: four
 * address bytes and a width byte, 0x80 for zero padding) then "/722>".
 * With more cards than the disc's, the "722" is their number and the count
 * as wide as it. The string is keyed by its id, not its address, so a
 * translation's heading is rewritten the same way: whatever else it says
 * stays, and a heading that jumps (whose operands only mean something where
 * they are) is left as it is. */
#define LIBRARY_HEADING_ID 0xF8
static unsigned char library_heading[64];
static const unsigned char *library_source;

static const unsigned char *heading(const unsigned char *text)
{
    const unsigned char *at;
    char digits[16];
    int width, i, n = 0;
    if (text == library_source) return library_heading;
    for (at = text; *at != 0xFF; at++) {
        if (*at >= 0xF9 && *at <= 0xFD) return text;
    }
    width = snprintf(digits, sizeof(digits), "%d", gCard_nCount);
    at = text;
    while (*at != 0xFF) {
        if ((size_t)n + 16 >= sizeof(library_heading)) return text;
        if (at[0] == 0xF8 && at[1] == 0x03 && !memchr(at + 2, 0xFF, 5)) {
            int wide = at[6] & 0x0F;
            memcpy(library_heading + n, at, 6);
            library_heading[n + 6] = (unsigned char)((at[6] & 0xF0) | (wide > width ? wide : width));
            n += 7;
            at += 7;
        } else if (at[0] == glyph_of('7') && at[1] == glyph_of('2') && at[2] == glyph_of('2')) {
            for (i = 0; i < width; i++) library_heading[n++] = (unsigned char)glyph_of(digits[i]);
            at += 3;
        } else if (at[0] >= 0xF0 && at[0] <= 0xF5) {   /* an added glyph's two bytes */
            library_heading[n++] = *at++;
            if (*at != 0xFF) library_heading[n++] = *at++;
        } else {
            library_heading[n++] = *at++;
        }
    }
    library_heading[n] = 0xFF;
    library_source = text;
    return library_heading;
}

const unsigned char *Cards_Text(int id, const unsigned char *text)
{
    if (id == LIBRARY_HEADING_ID && gCard_nCount > CARD_COUNT && text) return heading(text);
    return text;
}

/* --- building the tables --------------------------------------------- */

static const char *const type_names[] = {"Dragon", "Spellcaster", "Zombie", "Warrior", "Beast-Warrior", "Beast",
                                         "Winged Beast", "Fiend", "Fairy", "Insect", "Dinosaur", "Reptile",
                                         "Fish", "Sea Serpent", "Machine", "Thunder", "Aqua", "Pyro", "Rock",
                                         "Plant", "Magic", "Trap", "Ritual", "Equip"};
static const char *const attribute_names[] = {"Light", "Dark", "Earth", "Water", "Fire", "Wind"};
/* The frames, in the order of their palettes (CARD_FRAME_*); "type" is the
 * card's own type's again. */
static const char *const frame_names[] = {"Monster", "Magic", "Trap", "Ritual", "Purple", "Orange", "Type"};

static int same_words(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    }
    return !*a && !*b;
}

/* A number, or one of `names` spelled out. -1 when it is neither. */
static int choice(const JsonValue *value, const char *const *choices, int count)
{
    int i;
    const char *text;
    if (!value) return -1;
    if (Json_TypeOf(value) == JSON_NUMBER) return (int)Json_Number(value, -1);
    text = Json_String(value, NULL);
    if (!text) return -1;
    for (i = 0; i < count; i++) {
        if (same_words(text, choices[i])) return i;
    }
    return (int)Json_Number(value, -1);
}


/* Letters and digits only, lowercased: "Blue-Eyes White Dragon" finds the
 * disc's "Blue-eyes White Dragon". */
static int same_letters(const char *a, const char *b)
{
    for (;;) {
        while (*a && !isalnum((unsigned char)*a)) a++;
        while (*b && !isalnum((unsigned char)*b)) b++;
        if (!*a || !*b) return !*a && !*b;
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    }
}

int Cards_FusionGroupNamed(const char *text)
{
    static const struct { const char *name; int group; } groups[] = {
        {"AngelWinged", CARD_FUSION_GROUP_ANGEL_WINGED},
        {"Bugrothian", CARD_FUSION_GROUP_BUGROTHIAN},
        {"Egg", CARD_FUSION_GROUP_EGG},
        {"Elf", CARD_FUSION_GROUP_ELF},
        {"FeatherFromBear", CARD_FUSION_GROUP_FEATHER_FROM_BEAR},
        {"FeatherFromHarpie", CARD_FUSION_GROUP_FEATHER_FROM_HARPIE},
        {"FeatherFromMachine", CARD_FUSION_GROUP_FEATHER_FROM_MACHINE},
        {"Female", CARD_FUSION_GROUP_FEMALE},
        {"Jar", CARD_FUSION_GROUP_JAR},
        {"Koumorian", CARD_FUSION_GROUP_KOUMORIAN},
        {"MercuryMagicUser", CARD_FUSION_GROUP_MERCURY_MAGIC_USER},
        {"MercurySpellcaster", CARD_FUSION_GROUP_MERCURY_SPELLCASTER},
        {"Mirror", CARD_FUSION_GROUP_MIRROR},
        {"MusKingian", CARD_FUSION_GROUP_MUS_KINGIAN},
        {"MystElfian", CARD_FUSION_GROUP_MYST_ELFIAN},
        {"Rainbow", CARD_FUSION_GROUP_RAINBOW},
        {"Sheepian", CARD_FUSION_GROUP_SHEEPIAN},
        {"Thronian", CARD_FUSION_GROUP_THRONIAN},
        {"Turtle", CARD_FUSION_GROUP_TURTLE},
        {"UsableBeast", CARD_FUSION_GROUP_USABLE_BEAST},
    };
    size_t i;
    for (i = 0; i < sizeof(groups) / sizeof(groups[0]); i++)
        if (same_letters(text, groups[i].name)) return groups[i].group;
    return CARD_FUSION_GROUP_NONE;
}

/* The retail names, decoded once: a manifest may name hundreds of cards. */
static char (*retail_names)[48];

static int retail_by_name(const char *text)
{
    int id;
    if (!retail_names) {
        retail_names = calloc(CARD_ID_END, sizeof(*retail_names));
        if (!retail_names) return 0;
        for (id = 1; id <= CARD_COUNT; id++) retail_name(id, retail_names[id], sizeof(retail_names[id]));
    }
    for (id = 1; id <= CARD_COUNT; id++) {
        if (same_words(text, retail_names[id])) return id;
    }
    for (id = 1; id <= CARD_COUNT; id++) {
        if (same_letters(text, retail_names[id])) return id;
    }
    return 0;
}

int Cards_Reference(const JsonValue *value)
{
    const char *text;
    int id;
    if (!value || Json_TypeOf(value) == JSON_NULL) return 0;
    if (Json_TypeOf(value) == JSON_NUMBER) {
        id = (int)Json_Number(value, 0);
        return Cards_Valid(id) ? id : -1;
    }
    text = Json_String(value, NULL);
    return text ? Cards_Named(text) : -1;
}

/* A card a manifest names, for the Mods window's overlaps (mods.h). */
static int overlap_card(const char *text, long number)
{
    return text ? Cards_Named(text) : number > 0 && number <= 0xFFFF && Cards_Valid((int)number) ? (int)number : -1;
}
static int overlap_card_info(int id, int *base, int *type, int *attribute)
{
    if (!Cards_Valid(id)) return 0;
    *base = Cards_BaseId(id);
    *type = Cards_Type(id);
    *attribute = Cards_Attribute(id);
    return 1;
}

int Cards_Named(const char *text)
{
    int id;
    if (!text || !*text) return -1;
    if (strchr(text, ':')) return (id = Cards_FindIdentity(text)) ? id : -1;
    if (strspn(text, "0123456789") == strlen(text)) return Cards_Valid(id = atoi(text)) ? id : -1;
    return (id = retail_by_name(text)) ? id : -1;
}

int Cards_TypeNamed(const char *text)
{
    int type;
    for (type = 0; text && type < (int)(sizeof(type_names) / sizeof(type_names[0])); type++) {
        if (same_letters(text, type_names[type])) return type;
    }
    return -1;
}

int Cards_Type(int id)
{
    return Cards_Valid(id) ? (int)(((unsigned)gDuel_adwCardStats[id - 1] >> 26) & 0x1F) : -1;
}

int Cards_AttributeNamed(const char *text)
{
    int attribute;
    for (attribute = 0; text && attribute < (int)(sizeof(attribute_names) / sizeof(attribute_names[0])); attribute++) {
        if (same_letters(text, attribute_names[attribute])) return attribute;
    }
    return -1;
}

int Cards_Attribute(int id)
{
    return Cards_Valid(id) ? gDuel_abCardLevelAttr[id] >> 4 : -1;
}

int Cards_Level(int id)
{
    return Cards_Valid(id) ? gDuel_abCardLevelAttr[id] & 0x0F : -1;
}



int Cards_InFusionGroup(int id, int group)
{
    /* The retail cards' groups by id, looked up by name once. */
    static unsigned int by_id[CARD_ID_END];
    static int looked_up;
    int base;
    if (!Cards_Valid(id) || group <= CARD_FUSION_GROUP_NONE || group > CARD_FUSION_GROUP_USABLE_BEAST) return 0;
    if (has_fusion_groups[id]) return !!(fusion_groups[id] & (1u << group));
    /* Membership is the canonical "secondary card types by card" table from
     * Marcelo Silvarolla's programmatically validated Forbidden Memories
     * fusion guide.  Keep this explicit: several groups have conflict-driven
     * exceptions which are not safely reconstructed from names or appearance. */
    static const struct { const char *name; unsigned int groups; } cards[] = {
        {"30,000-Year White Turtle", (1u << CARD_FUSION_GROUP_BUGROTHIAN) | (1u << CARD_FUSION_GROUP_TURTLE)},
        {"7 Colored Fish", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Air Marmot of Nefariousness", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Akakieisu", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Akihiron", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Amazon of the Seas", (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Ameba", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Ancient Brain", (1u << CARD_FUSION_GROUP_ANGEL_WINGED) | (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Ancient Elf", (1u << CARD_FUSION_GROUP_ELF) | (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Ancient Jar", (1u << CARD_FUSION_GROUP_JAR)},
        {"Ancient Lizard Warrior", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Ancient One of the Deep Forest", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Ancient Sorcerer", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Angelwitch", (1u << CARD_FUSION_GROUP_ANGEL_WINGED) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_BEAR) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE) | (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Ansatsu", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Aqua Madoor", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Aqua Snake", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Arlownay", (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Arma Knight", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Armed Ninja", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Armored Lizard", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Armored Rat", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Armored Starfish", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Beaked Snake", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Bear Trap", (1u << CARD_FUSION_GROUP_FEATHER_FROM_HARPIE)},
        {"Beastking of the Swamps", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Beautiful Beast Trainer", (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Beautiful Headhuntress", (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Behegon", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Big Eye", (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Binding Chain", (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Bio Plant", (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Blackland Fire Dragon", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Blue-eyed Silver Zombie", (1u << CARD_FUSION_GROUP_SHEEPIAN)},
        {"Blue-winged Crown", (1u << CARD_FUSION_GROUP_FEATHER_FROM_BEAR) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE)},
        {"Bone Mouse", (1u << CARD_FUSION_GROUP_SHEEPIAN)},
        {"Boo Koo", (1u << CARD_FUSION_GROUP_MUS_KINGIAN) | (1u << CARD_FUSION_GROUP_MYST_ELFIAN)},
        {"Bottom Dweller", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Boulder Tortoise", (1u << CARD_FUSION_GROUP_BUGROTHIAN) | (1u << CARD_FUSION_GROUP_TURTLE)},
        {"Burglar", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Castle of Dark Illusions", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Catapult Turtle", (1u << CARD_FUSION_GROUP_BUGROTHIAN) | (1u << CARD_FUSION_GROUP_TURTLE)},
        {"Celtic Guardian", (1u << CARD_FUSION_GROUP_ELF)},
        {"Change Slime", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Claw Reacher", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Corroding Shark", (1u << CARD_FUSION_GROUP_SHEEPIAN)},
        {"Crab Turtle", (1u << CARD_FUSION_GROUP_TURTLE)},
        {"Crazy Fish", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Crimson Sunbird", (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE)},
        {"Curtain of the Dark Ones", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Dancing Elf", (1u << CARD_FUSION_GROUP_ANGEL_WINGED) | (1u << CARD_FUSION_GROUP_ELF) | (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Dark Artist", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Dark Elf", (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Dark Gray", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Dark King of the Abyss", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Dark Prisoner", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Dark Rabbit", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Dark Shade", (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Dark Titan of Terror", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Dark Witch", (1u << CARD_FUSION_GROUP_ANGEL_WINGED) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE) | (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Dokuroizo the Grim Reaper", (1u << CARD_FUSION_GROUP_SHEEPIAN)},
        {"Doma The Angel of Silence", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Doron", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Dorover", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Dragon Piper", (1u << CARD_FUSION_GROUP_JAR)},
        {"Drooling Lizard", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Dryad", (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Eatgaboon", (1u << CARD_FUSION_GROUP_FEATHER_FROM_HARPIE)},
        {"Eldeen", (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Embryonic Beast", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Emperor of the Land and Sea", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Enchanting Mermaid", (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Exodia the Forbidden", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Fairy of the Fountain", (1u << CARD_FUSION_GROUP_BUGROTHIAN) | (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Fairy's Gift", (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Faith Bird", (1u << CARD_FUSION_GROUP_FEATHER_FROM_BEAR) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE) | (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Feral Imp", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Fiend Kraken", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Fiend Refrection #1", (1u << CARD_FUSION_GROUP_MIRROR)},
        {"Fiend Refrection #2", (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE) | (1u << CARD_FUSION_GROUP_MIRROR)},
        {"Fiend's Hand", (1u << CARD_FUSION_GROUP_SHEEPIAN)},
        {"Fiend's Mirror", (1u << CARD_FUSION_GROUP_MIRROR)},
        {"Fire Kraken", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Flower Wolf", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Flying Penguin", (1u << CARD_FUSION_GROUP_BUGROTHIAN) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE)},
        {"Follow Wind", (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE)},
        {"Frenzied Panda", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Frog The Jam", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Fungi of the Musk", (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Fusionist", (1u << CARD_FUSION_GROUP_ANGEL_WINGED) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_BEAR) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE) | (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Garvas", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Gate Deeg", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Gatekeeper", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Gemini Elf", (1u << CARD_FUSION_GROUP_ELF) | (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Giant Red Seasnake", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Giant Turtle Who Feeds on Flames", (1u << CARD_FUSION_GROUP_BUGROTHIAN) | (1u << CARD_FUSION_GROUP_TURTLE)},
        {"Goddess of Whim", (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Goddess with the Third Eye", (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Gorgon Egg", (1u << CARD_FUSION_GROUP_EGG) | (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Grappler", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Graveyard and the Hand of Invitation", (1u << CARD_FUSION_GROUP_SHEEPIAN)},
        {"Great White", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Greenkappa", (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Griffore", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Gruesome Goo", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Guardian of the Labyrinth", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Guardian of the Sea", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Guardian of the Throne Room", (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Gyakutenno Megami", (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Hane-Hane", (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE)},
        {"Happy Lover", (1u << CARD_FUSION_GROUP_ANGEL_WINGED) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_BEAR) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE) | (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Harpie Lady", (1u << CARD_FUSION_GROUP_FEATHER_FROM_BEAR) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_HARPIE) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE) | (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Harpie Lady Sisters", (1u << CARD_FUSION_GROUP_FEATHER_FROM_BEAR) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_HARPIE) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE) | (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Hibikime", (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"High Tide Gyojin", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Hiro's Shadow Scout", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Hitodenchak", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Hitotsu-me Giant", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Horn Imp", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Hoshiningen", (1u << CARD_FUSION_GROUP_MUS_KINGIAN) | (1u << CARD_FUSION_GROUP_MYST_ELFIAN) | (1u << CARD_FUSION_GROUP_RAINBOW)},
        {"Hourglass of Courage", (1u << CARD_FUSION_GROUP_ANGEL_WINGED) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE) | (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Hourglass of Life", (1u << CARD_FUSION_GROUP_ANGEL_WINGED) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_BEAR) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE) | (1u << CARD_FUSION_GROUP_MUS_KINGIAN) | (1u << CARD_FUSION_GROUP_MYST_ELFIAN)},
        {"House of Adhesive Tape", (1u << CARD_FUSION_GROUP_FEATHER_FROM_HARPIE)},
        {"Hyo", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Hyosube", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Ice Water", (1u << CARD_FUSION_GROUP_BUGROTHIAN) | (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Ill Witch", (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Invader from Another Dimension", (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Invader of the Throne", (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Invisible Wire", (1u << CARD_FUSION_GROUP_FEATHER_FROM_HARPIE)},
        {"Jellyfish", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Job-change Mirror", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MIRROR) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Kageningen", (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Kamion Wizard", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Kanan the Swordmistress", (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Kanikabuto", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Kappa Avenger", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Key Mace", (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MUS_KINGIAN) | (1u << CARD_FUSION_GROUP_MYST_ELFIAN)},
        {"Key Mace #2", (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"King Fog", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Krokodilus", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Kuriboh", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"La Jinn the Mystical Genie", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"LaMoon", (1u << CARD_FUSION_GROUP_ANGEL_WINGED) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_BEAR)},
        {"Lady of Faith", (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Lava Battleguard", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Left Arm of the Forbidden One", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Left Leg of the Forbidden One", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Leo Wizard", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Leogun", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Lesser Dragon", (1u << CARD_FUSION_GROUP_KOUMORIAN)},
        {"Liquid Beast", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Lisark", (1u << CARD_FUSION_GROUP_FEATHER_FROM_BEAR) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE)},
        {"Little Chimera", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Lord of Zemia", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Lord of the Lamp", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Lucky Trinket", (1u << CARD_FUSION_GROUP_MUS_KINGIAN) | (1u << CARD_FUSION_GROUP_MYST_ELFIAN)},
        {"Lunar Queen Elzaim", (1u << CARD_FUSION_GROUP_ANGEL_WINGED) | (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MUS_KINGIAN) | (1u << CARD_FUSION_GROUP_MYST_ELFIAN)},
        {"Machine Conversion Factory", (1u << CARD_FUSION_GROUP_FEATHER_FROM_HARPIE)},
        {"Madjinn Gunn", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Magical Labyrinth", (1u << CARD_FUSION_GROUP_FEATHER_FROM_HARPIE)},
        {"Magician of Faith", (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MUS_KINGIAN) | (1u << CARD_FUSION_GROUP_MYST_ELFIAN)},
        {"Maha Vailo", (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Maiden of the Moonlight", (1u << CARD_FUSION_GROUP_ANGEL_WINGED) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE) | (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Mammoth Graveyard", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Marine Beast", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Masaki the Legendary Swordsman", (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Mask of Darkness", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Masked Clown", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Masked Sorcerer", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Master & Expert", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Mavelus", (1u << CARD_FUSION_GROUP_FEATHER_FROM_BEAR) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE)},
        {"Mech Bass", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Mech Mole Zombie", (1u << CARD_FUSION_GROUP_SHEEPIAN)},
        {"Mechaleon", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Meda Bat", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Megirus Light", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Meotoko", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Metal Fish", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Metal Guardian", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Midnight Fiend", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Milus Radiant", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Minar", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Monster Egg", (1u << CARD_FUSION_GROUP_EGG)},
        {"Monster Eye", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Monstrous Bird", (1u << CARD_FUSION_GROUP_FEATHER_FROM_BEAR) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE)},
        {"Monsturtle", (1u << CARD_FUSION_GROUP_BUGROTHIAN) | (1u << CARD_FUSION_GROUP_TURTLE)},
        {"Moon Envoy", (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Morphing Jar", (1u << CARD_FUSION_GROUP_JAR)},
        {"Muse-A", (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Mystery Hand", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Mystic Clown", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Mystic Horseman", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Mystic Lamp", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Mystical Capture Chain", (1u << CARD_FUSION_GROUP_MUS_KINGIAN) | (1u << CARD_FUSION_GROUP_MYST_ELFIAN)},
        {"Mystical Elf", (1u << CARD_FUSION_GROUP_ELF) | (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Mystical Sheep #1", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Mystical Sheep #2", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Needle Ball", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Nekogal #1", (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Nekogal #2", (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Nemuriko", (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Night Lizard", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Obese Marmot of Nefariousness", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Octoberser", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Ocubeam", (1u << CARD_FUSION_GROUP_ANGEL_WINGED)},
        {"Ogre of the Black Shadow", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"One Who Hunts Souls", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Ooguchi", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Pale Beast", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Peacock", (1u << CARD_FUSION_GROUP_FEATHER_FROM_BEAR) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE)},
        {"Pendulum Machine", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Penguin Knight", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Penguin Soldier", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Petit Angel", (1u << CARD_FUSION_GROUP_ANGEL_WINGED) | (1u << CARD_FUSION_GROUP_MUS_KINGIAN) | (1u << CARD_FUSION_GROUP_MYST_ELFIAN)},
        {"Phantom Dewan", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Phantom Ghost", (1u << CARD_FUSION_GROUP_SHEEPIAN)},
        {"Pot the Trick", (1u << CARD_FUSION_GROUP_JAR) | (1u << CARD_FUSION_GROUP_SHEEPIAN)},
        {"Prevent Rat", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Princess of Tsurugi", (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Protector of the Throne", (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Psychic Kappa", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Punished Eagle", (1u << CARD_FUSION_GROUP_FEATHER_FROM_BEAR) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE)},
        {"Queen Bird", (1u << CARD_FUSION_GROUP_FEATHER_FROM_BEAR) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE)},
        {"Queen of Autumn Leaves", (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Queen's Double", (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Rainbow Flower", (1u << CARD_FUSION_GROUP_RAINBOW)},
        {"Rainbow Marine Mermaid", (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_RAINBOW)},
        {"Reaper of the Cards", (1u << CARD_FUSION_GROUP_KOUMORIAN)},
        {"Rhaimundos of the Red Sword", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Right Arm of the Forbidden One", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Right Leg of the Forbidden One", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Rogue Doll", (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Rose Spectre of Dunn", (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Ryu-kishin", (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Saggi the Dark Clown", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Sangan", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Sea Kamen", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Sectarian of Secrets", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Serpent Marauder", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Shadow Specter", (1u << CARD_FUSION_GROUP_SHEEPIAN)},
        {"Shining Friendship", (1u << CARD_FUSION_GROUP_ANGEL_WINGED) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE) | (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Silver Fang", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Sinister Serpent", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Skelengel", (1u << CARD_FUSION_GROUP_ANGEL_WINGED) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE) | (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Skull Red Bird", (1u << CARD_FUSION_GROUP_FEATHER_FROM_BEAR) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE)},
        {"Skull Servant", (1u << CARD_FUSION_GROUP_SHEEPIAN)},
        {"Skull Stalker", (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Sky Dragon", (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE)},
        {"Sleeping Lion", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Solitude", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Sonic Maid", (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Spiked Snail", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Spirit of the Harp", (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Star Boy", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Stuffed Animal", (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Succubus Knight", (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Swamp Battleguard", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Synchar", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Tainted Wisdom", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Takuhee", (1u << CARD_FUSION_GROUP_FEATHER_FROM_BEAR) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE)},
        {"Tatsunootoshigo", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Temple of Skulls", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Tenderness", (1u << CARD_FUSION_GROUP_ANGEL_WINGED) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE) | (1u << CARD_FUSION_GROUP_MUS_KINGIAN) | (1u << CARD_FUSION_GROUP_MYST_ELFIAN)},
        {"Terra the Terrible", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"That Which Feeds on Life", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"The Drdek", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"The Furious Sea King", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"The Judgement Hand", (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"The Little Swordsman of Aile", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"The Melting Red Shadow", (1u << CARD_FUSION_GROUP_BUGROTHIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"The Shadow Who Controls the Dark", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"The Wandering Doomed", (1u << CARD_FUSION_GROUP_SHEEPIAN)},
        {"Three-legged Zombies", (1u << CARD_FUSION_GROUP_SHEEPIAN)},
        {"Toad Master", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Togex", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Toon Alligator", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Torike", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Trap Master", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Trial of Nightmares", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Turtle Bird", (1u << CARD_FUSION_GROUP_TURTLE)},
        {"Turtle Raccoon", (1u << CARD_FUSION_GROUP_BUGROTHIAN) | (1u << CARD_FUSION_GROUP_TURTLE)},
        {"Turtle Tiger", (1u << CARD_FUSION_GROUP_BUGROTHIAN) | (1u << CARD_FUSION_GROUP_TURTLE)},
        {"Turu-Purun", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Twin Long Rods #1", (1u << CARD_FUSION_GROUP_BUGROTHIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Twin Long Rods #2", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Two-mouth Darkruler", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Unknown Warrior of Fiend", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Versago the Destroyer", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Violent Rain", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Vishwar Randi", (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Warrior of Tradition", (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Water Element", (1u << CARD_FUSION_GROUP_BUGROTHIAN) | (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Water Girl", (1u << CARD_FUSION_GROUP_BUGROTHIAN) | (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Water Magician", (1u << CARD_FUSION_GROUP_BUGROTHIAN) | (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Water Omotics", (1u << CARD_FUSION_GROUP_BUGROTHIAN) | (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Waterdragon Fairy", (1u << CARD_FUSION_GROUP_BUGROTHIAN) | (1u << CARD_FUSION_GROUP_FEMALE)},
        {"Weather Control", (1u << CARD_FUSION_GROUP_MUS_KINGIAN) | (1u << CARD_FUSION_GROUP_MYST_ELFIAN)},
        {"Weather Report", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Wetha", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"White Magical Hat", (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Wicked Dragon with the Ersatz Head", (1u << CARD_FUSION_GROUP_KOUMORIAN) | (1u << CARD_FUSION_GROUP_SHEEPIAN)},
        {"Wicked Mirror", (1u << CARD_FUSION_GROUP_MIRROR) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Wing Eagle", (1u << CARD_FUSION_GROUP_FEATHER_FROM_BEAR) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE)},
        {"Wing Egg Elf", (1u << CARD_FUSION_GROUP_ANGEL_WINGED) | (1u << CARD_FUSION_GROUP_EGG) | (1u << CARD_FUSION_GROUP_ELF) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_BEAR) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE) | (1u << CARD_FUSION_GROUP_MYST_ELFIAN)},
        {"Winged Egg of New Life", (1u << CARD_FUSION_GROUP_ANGEL_WINGED) | (1u << CARD_FUSION_GROUP_EGG) | (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE) | (1u << CARD_FUSION_GROUP_MUS_KINGIAN)},
        {"Winged Trumpeter", (1u << CARD_FUSION_GROUP_FEATHER_FROM_MACHINE)},
        {"Witch of the Black Forest", (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Witch's Apprentice", (1u << CARD_FUSION_GROUP_ANGEL_WINGED) | (1u << CARD_FUSION_GROUP_FEMALE) | (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_MERCURY_SPELLCASTER)},
        {"Witty Phantom", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Wolf", (1u << CARD_FUSION_GROUP_USABLE_BEAST)},
        {"Wood Clown", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER)},
        {"Wretched Ghost of the Attic", (1u << CARD_FUSION_GROUP_MERCURY_MAGIC_USER) | (1u << CARD_FUSION_GROUP_SHEEPIAN) | (1u << CARD_FUSION_GROUP_THRONIAN)},
        {"Yado Karu", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Yormungarde", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Zarigun", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
        {"Zone Eater", (1u << CARD_FUSION_GROUP_BUGROTHIAN)},
    };
    if (!looked_up) {
        size_t i;
        looked_up = 1;
        for (i = 0; i < sizeof(cards) / sizeof(cards[0]); i++) {
            int named = Cards_Named(cards[i].name);
            if (named >= 1 && named <= CARD_COUNT) by_id[named] |= cards[i].groups;
            else fprintf(stderr, "memories-pc: fusion groups: no retail card \"%s\"\n", cards[i].name);
        }
    }
    base = Cards_BaseId(id);
    return base >= 1 && base <= CARD_COUNT && (by_id[base] & (1u << group)) != 0;
}

typedef struct {
    int use_count[2][CARD_ID_END];  /* copies taking a base's place, per use */
    unsigned char use[CARD_TABLE_ID_END];
} BuildContext;

static int clamp(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

/* The type a card has on the disc (-1 past it): what its effect is. */
int Cards_RetailType(int id)
{
    return id >= 1 && id <= CARD_COUNT ? (int)((((const unsigned *)(uintptr_t)RETAIL_STATS)[id - 1] >> 26) & 0x1F) : -1;
}

/* Monster, magic, trap, ritual or equip: what a card is played as. */
static int kind(int type) { return type < CARD_TYPE_MAGIC ? 0 : type; }
int Cards_KindChanged(int id)
{
    return Cards_Valid(id) && kind(Cards_Type(id)) != kind(Cards_RetailType(Cards_BaseId(id)));
}
int Cards_AiId(int id)
{
    int as = Cards_EffectId(id);
    return Cards_Valid(id) && kind(Cards_Type(id)) == kind(Cards_RetailType(as)) ? as : -1;
}

static int retail_monster(int id)
{
    return id >= 1 && id <= CARD_COUNT &&
           (int)((((const unsigned *)(uintptr_t)RETAIL_STATS)[id - 1] >> 26) & 0x1F) < CARD_TYPE_MAGIC;
}

/* A card that was no monster has no guardian stars either: made one (a
 * replace's or a copy's "type"), unless "stars" gives some, its model's, or
 * the Sun and the Moon. A monster that "stars" leaves with none, or that was
 * one before this entry (an earlier mod's no-star card), keeps none
 * (stars.h). */
static void default_stars(int id, unsigned *stats, int was_monster, int stars_given)
{
    if ((int)((*stats >> 26) & 0x1F) < CARD_TYPE_MAGIC && !was_monster && !stars_given && !(*stats & (0xFFu << 18))) {
        *stats |= retail_monster(Cards_ModelId(id))
                      ? ((const unsigned *)(uintptr_t)RETAIL_STATS)[Cards_ModelId(id) - 1] & (0xFFu << 18)
                      : (8u << 22) | (9u << 18);
    }
}

/* A replaced card's 3D model and effect: its own unless the entry names
 * others. Only a card that is a monster on the disc has a model; a magic,
 * trap, ritual or equip card made a monster has none unless "model" names a
 * monster whose model it takes, and fights without one. A monster made
 * anything else has no effect unless "effect" names a card whose effect it
 * takes. */
static void replace_model_effect(const char *mod, int index, const JsonValue *entry, int id, unsigned *stats,
                                 int was_monster, int stars_given)
{
    const JsonValue *model = Json_Member(entry, "model"), *effect = Json_Member(entry, "effect");
    int value;
    if (model) {
        value = Cards_Reference(model);
        if (retail_monster(value)) {
            model_ids[id] = (unsigned short)value;
        } else {
            Mods_Note(mod, "cards[%d]: \"model\" must name a monster of the disc", index);
        }
    }
    default_stars(id, stats, was_monster, stars_given);
    if (effect) {
        value = Cards_Reference(effect);
        if (value >= 1 && value <= CARD_COUNT) {
            effect_ids[id] = (unsigned short)value;
        } else {
            Mods_Note(mod, "cards[%d]: \"effect\" must name a card of the disc", index);
        }
    }
}

/* A card's picture from a PNG bigger than the console's is drawn from the
 * PNG itself above the console's resolution: the made bytes of the record
 * (art.c) find it as a texture pack's image is found (texture_pack.h,
 * TexturePack_AddMade). A picture at the console's size or under has no
 * more to show. */
static void add_full_picture(const char *path, const unsigned char *record, int thumbnail)
{
    int w = thumbnail ? CARD_THUMB_WIDTH : CARD_ART_WIDTH, h = thumbnail ? CARD_THUMB_HEIGHT : CARD_ART_HEIGHT;
    int x, y, cw, ch, width, height;
    if (!CardArt_Crop(path, w, h, &x, &y, &cw, &ch, &width, &height) || (cw <= w && ch <= h)) return;
    if (!TexturePack_AddMade(record + (thumbnail ? CARD_THUMB_PIXELS : CARD_ART_PIXELS), w / 2, h, 8,
                             record + (thumbnail ? CARD_THUMB_CLUT : CARD_ART_CLUT), thumbnail ? 64 : 256, path, x, y, cw,
                             ch))
        fprintf(stderr, "memories-pc: cards: %s is drawn at the console's size only\n", path);
}

/* "password": up to eight digits as a string ("08124921", leading zeros
 * kept) or a whole number, "" or null for none. 0 and a note if it is
 * neither. */
static int read_password(const char *mod, int index, const JsonValue *value, unsigned *out)
{
    char digits[16];
    const char *text;
    size_t length, i;
    *out = CARD_PASSWORD_NONE;
    if (Json_TypeOf(value) == JSON_NULL) return 1;
    if (Json_TypeOf(value) == JSON_NUMBER && Json_Number(value, -1) >= 0 && Json_Number(value, -1) <= 99999999) {
        snprintf(digits, sizeof(digits), "%ld", Json_Number(value, 0));
        text = digits;
    } else {
        text = Json_String(value, NULL);
    }
    length = text ? strlen(text) : 9;
    if (length > 8 || strspn(text, "0123456789") != length) {
        Mods_Note(mod, "cards[%d]: \"password\" must be up to 8 digits", index);
        return 0;
    }
    if (!length) return 1;
    *out = 0;
    for (i = 0; i < length; i++) *out = (*out << 4) | (unsigned)(text[i] - '0');
    return 1;
}

/* An entry's "notes" go after what earlier entries wrote for the card: a
 * later entry that replaces the card again does not take them away. */
static void add_notes(const char *mod, int index, int id, const JsonValue *value)
{
    const char *text = Json_String(value, NULL);
    size_t had, length;
    char *joined;
    if (!value) return;
    if (!text) {
        Mods_Note(mod, "cards[%d]: \"notes\" must be text", index);
        return;
    }
    if (!*text) return;
    had = card_notes[id] ? strlen(card_notes[id]) : 0;
    length = strlen(text);
    joined = realloc(card_notes[id], had + (had ? 1 : 0) + length + 1);
    if (!joined) return;
    if (had) joined[had++] = '\n';
    memcpy(joined + had, text, length + 1);
    card_notes[id] = joined;
}

/* A "replace" entry with nothing but "notes" (and an "id") only adds its
 * notes: the card stays as the disc or an earlier mod left it. */
static int notes_only(const JsonValue *entry)
{
    const JsonValue *member;
    for (member = Json_At(entry, 0); member; member = Json_Next(member)) {
        const char *key = Json_Name(member);
        if (strcmp(key, "replace") && strcmp(key, "notes") && strcmp(key, "id")) return 0;
    }
    return Json_Member(entry, "notes") != NULL;
}

static void add_entry(const char *mod, const char *directory, int index, const JsonValue *entry, BuildContext *context)
{
    const JsonValue *replace = Json_Member(entry, "replace");
    const JsonValue *copy = replace ? replace : Json_Member(entry, "copy");
    const JsonValue *stars = Json_Member(entry, "stars");
    const char *name = Json_String(Json_Member(entry, "name"), NULL);
    const char *setting = Json_String(Json_Member(entry, "count_setting"), NULL);
    const char *description = Json_String(Json_Member(entry, "description"), NULL);
    const JsonValue *password_value = Json_Member(entry, "password");
    unsigned char *record = NULL, *title = NULL, *named_plate = NULL, *field_art_record = NULL;
    int parts = 0;
    int base = 0, count, n, value, has_password;
    unsigned stats, password = CARD_PASSWORD_NONE, entry_fusion_groups = 0;
    int entry_has_fusion_groups = 0;
    int was_monster, stars_given = 0;
    MonsterEffect read_effects[MONSTER_EFFECTS_MAX];
    MonsterEffect *own_effects = NULL;
    int effect_count;
    unsigned int trap_threshold;
    unsigned char level_attr, frame;
    if (Json_TypeOf(entry) != JSON_OBJECT) {
        Mods_Note(mod, "cards[%d] is not an object", index);
        return;
    }
    if (Json_TypeOf(copy) == JSON_STRING && !isdigit((unsigned char)*Json_String(copy, "")) ) {
        base = retail_by_name(Json_String(copy, ""));
    } else {
        base = (int)Json_Number(copy, 0);
    }
    if (base < 1 || base > CARD_COUNT) {
        Mods_Note(mod, "cards[%d]: \"%s\" must name a card of the disc, 1 to %d", index, replace ? "replace" : "copy",
                  CARD_COUNT);
        return;
    }
    if (replace && notes_only(entry)) {
        add_notes(mod, index, base, Json_Member(entry, "notes"));
        return;
    }
    /* "replace" changes the retail card itself, in place: one card, no new
     * id, and nothing of it goes in the save. */
    count = replace ? 1 : (int)Json_Number(Json_Member(entry, "count"), 1);
    if (!replace && setting && *setting) count = Mods_Setting(mod, setting, count);
    if (count < 0) count = 0;
    if (replace && replaced[base]) {
        char text[128];
        retail_name(base, text, sizeof(text));
        Mods_Note(mod, "cards[%d]: %d %s is replaced by %s too; what this entry sets goes over it", index, base, text,
                  replaced[base]);
    }
    if (!replace && count > CARD_TABLE_COUNT - gCard_nCount) {
        Mods_Note(mod, "cards[%d]: only %d more cards fit (%d asked)", index, CARD_TABLE_COUNT - gCard_nCount, count);
        count = CARD_TABLE_COUNT - gCard_nCount;
    }
    /* Left out, a copy has its base's monster effects and a replace keeps
     * the earlier one's; [] or null takes them away. */
    effect_count = MonsterEffects_Read(mod, index, Json_Member(entry, "monster_effects"), read_effects);
    if (effect_count > 0) {
        own_effects = malloc((size_t)effect_count * sizeof(*own_effects));
        if (own_effects) memcpy(own_effects, read_effects, (size_t)effect_count * sizeof(*own_effects));
        else effect_count = 0;
    }
    /* What the entry leaves out is the base's. */
    stats = (unsigned)gDuel_adwCardStats[base - 1];
    trap_threshold = trap_thresholds[base];
    {
        const JsonValue *threshold = Json_Member(entry, "trap_threshold");
        if (threshold) {
            long points = Json_Number(threshold, -1);
            if (Json_TypeOf(threshold) == JSON_NULL) trap_threshold = 0;
            else if (Json_TypeOf(threshold) == JSON_NUMBER && points >= 0 && points <= 65535)
                trap_threshold = (unsigned int)points + 1;
            else Mods_Note(mod, "cards[%d]: \"trap_threshold\" must be a whole number, 0 to 65535, or null", index);
        }
    }
    was_monster = (int)((stats >> 26) & 0x1F) < CARD_TYPE_MAGIC;
    level_attr = gDuel_abCardLevelAttr[base];
    /* A card's own ATK and DEF are nine bits of tens in its stats word
     * (gDuel_adwCardStats): 0 to 5110 in steps of 10. A mod's "limits" raise
     * what a monster may reach with its bonuses, not this; more would mean a
     * wider card table, which every reader of the word would have to follow.
     * So a value past it or between tens is said, not quietly changed. */
    {
        static const char *const stat_keys[2] = {"attack", "defense"};
        int k;
        for (k = 0; k < 2; k++) {
            if ((value = (int)Json_Number(Json_Member(entry, stat_keys[k]), -1)) < 0) continue;
            if (value > 0x1FF * 10) {
                Mods_Note(mod, "cards[%d]: \"%s\" %d is past the %d a card's own stat can be (nine bits of tens); "
                          "%d used, and \"limits\" or a bonus takes it higher in a duel", index, stat_keys[k], value,
                          0x1FF * 10, 0x1FF * 10);
            } else if (value % 10) {
                Mods_Note(mod, "cards[%d]: \"%s\" %d is kept in tens; %d used", index, stat_keys[k], value,
                          value / 10 * 10);
            }
            stats = (stats & ~(0x1FFu << (9 * k))) | ((unsigned)clamp(value / 10, 0, 0x1FF) << (9 * k));
        }
    }
    if ((value = choice(Json_Member(entry, "type"), type_names, 24)) >= 0) {
        /* A copy may become a non-monster when it has a retail effect of
         * that type. The effect names a built-in behavior, even if the
         * source card was itself replaced. An equip needs none: what it
         * equips and adds are the mod's "equips" rules (tables.h). Any
         * card may become a monster, as a replaced one may. */
        int monster = ((stats >> 26) & 0x1F) < CARD_TYPE_MAGIC;
        int effect = Cards_Reference(Json_Member(entry, "effect"));
        if (effect <= 0 && Json_Number(Json_Member(entry, "effect"), 0) == 0)
            effect = Cards_EffectId(base);
        value = clamp(value, 0, CARD_TYPE_EQUIP);
        if (!replace && value >= CARD_TYPE_MAGIC && value != (int)((stats >> 26) & 0x1F) &&
            !(value >= CARD_TYPE_MAGIC && Cards_RetailType(effect) == value) && value != CARD_TYPE_EQUIP) {
            Mods_Note(mod, "cards[%d]: %s; \"type\" left out", index,
                      monster ? "a copy needs a matching retail effect to become a non-monster" :
                                "a non-monster copy needs a matching retail effect to change type");
        } else {
            stats = (stats & ~(0x1Fu << 26)) | ((unsigned)value << 26);
        }
    }
    /* Only a monster has ATK and DEF: no magic, trap, ritual or equip card of
     * the disc has either. A monster replaced as one of those would keep its
     * own, and the CPU, which ranks its hand by them whatever their type,
     * would take it for its best monster: it plays it face down every turn,
     * into the back row, over the last one it set. */
    if (((stats >> 26) & 0x1F) >= CARD_TYPE_MAGIC && (stats & 0x3FFFFu)) {
        if (Json_Number(Json_Member(entry, "attack"), 0) > 0 || Json_Number(Json_Member(entry, "defense"), 0) > 0)
            Mods_Note(mod, "cards[%d]: only a monster has ATK and DEF; \"attack\" and \"defense\" left out", index);
        stats &= ~0x3FFFFu;
    }
    if (stars && (Json_TypeOf(stars) != JSON_ARRAY || Json_Count(stars) != 2)) {
        Mods_Note(mod, "cards[%d]: \"stars\" is a list of two, [first, second] (none for no star); left out", index);
    } else if (stars) {
        /* A number, the disc's names, or a name a mod's "guardian_stars"
         * gives (stars.h): up to 15, what the card's 4-bit fields hold. */
        int first = Stars_Value(Json_At(stars, 0)), second = Stars_Value(Json_At(stars, 1));
        int kept_first, kept_second;
        stars_given = first >= 0 && second >= 0;
        if (first >= 0) stats = (stats & ~(0xFu << 22)) | ((unsigned)clamp(first, 0, STARS_MAX) << 22);
        if (second >= 0) stats = (stats & ~(0xFu << 18)) | ((unsigned)clamp(second, 0, STARS_MAX) << 18);
        if (first > STARS_MAX || second > STARS_MAX)
            Mods_Note(mod, "cards[%d]: a card holds a guardian star in 4 bits: 15 at most", index);
        if (first == -1 || second == -1)
            Mods_Note(mod, "cards[%d]: \"stars\": not a guardian star; left out", index);
        /* None is 0, null, "none" or "(none)". The duel reads the first star
           unless the second is chosen, and a card with no second never
           chooses it: so [none, X] is the one-star card [X, none], and
           [none, none] a monster with no star at all (stars.h). Of the
           pair as it now stands, with the base's for a star left out. */
        kept_first = (int)((stats >> 22) & 0xF);
        kept_second = (int)((stats >> 18) & 0xF);
        if (Stars_Normalize(&kept_first, &kept_second))
            stats = (stats & ~(0xFFu << 18)) | ((unsigned)kept_first << 22) | ((unsigned)kept_second << 18);
    }
    if ((value = (int)Json_Number(Json_Member(entry, "level"), -1)) >= 0) {
        level_attr = (unsigned char)((level_attr & 0xF0) | clamp(value, 0, 12));
    }
    if ((value = choice(Json_Member(entry, "attribute"), attribute_names, 6)) >= 0) {
        level_attr = (unsigned char)((level_attr & 0x0F) | (clamp(value, 0, 15) << 4));
    }
    /* Optional secondary fusion groups. A new card inherits its retail base when omitted.
     * Supplying fusion_groups gives the card its own membership instead. */
    {
        const JsonValue *groups = Json_Member(entry, "fusion_groups");
        if (groups) {
            const JsonValue *g;
            entry_has_fusion_groups = 1;
            for (g = Json_At(groups, 0); g; g = Json_Next(g)) {
                int group = Cards_FusionGroupNamed(Json_String(g, NULL));
                if (group > CARD_FUSION_GROUP_NONE && group <= CARD_FUSION_GROUP_USABLE_BEAST)
                    entry_fusion_groups |= 1u << group;
                else
                    Mods_Note(mod, "cards[%d]: unknown fusion_groups entry", index);
            }
        }
    }
    /* Left out, the frame is the base's (its type's unless an earlier entry
     * chose one); "type" goes back to the type's. */
    frame = frames[base];
    if (Json_Member(entry, "frame")) {
        value = choice(Json_Member(entry, "frame"), frame_names, CARD_FRAME_COUNT + 1);
        if (value < 0 || value > CARD_FRAME_COUNT) {
            Mods_Note(mod, "cards[%d]: \"frame\" is Monster, Magic, Trap, Ritual, Purple, Orange or Type; left out",
                      index);
        } else {
            frame = (unsigned char)(value == CARD_FRAME_COUNT ? FRAME_TYPE : value + 1);
        }
    }
    /* What View > Card passwords shows (passwords.h): a copy has none
     * unless it says, a replaced card keeps the disc's. */
    has_password = password_value && read_password(mod, index, password_value, &password);
    /* Artwork: PNGs relative to the mod's directory, shared by the entry's
     * cards; a card with a name of its own gets a title plate that says it. */
    {
        static const char *const keys[] = {"art", "thumbnail", "title"};
        char full[2][1200] = {"", ""}; /* the art's and the thumbnail's PNGs, as used */
        int k;
        for (k = 0; k < 3 && count; k++) {
            const char *file = Json_String(Json_Member(entry, keys[k]), NULL);
            char path[1200], why[1300];
            int ok;
            if (!file || !*file) continue;
            if (!Paths_Contained(file) || snprintf(path, sizeof(path), "%s/%s", directory, file) >= (int)sizeof(path)) {
                Mods_Note(mod, "cards[%d]: \"%s\": %s is outside the mod", index, keys[k], file);
                continue;
            }
            if (k == 2) {
                if (!title) title = calloc(1, CARD_TITLE_BYTES);
                ok = title && CardArt_TitleFromImage(path, title, why, sizeof(why));
            } else {
                if (!record) record = calloc(1, CARD_ART_RECORD);
                ok = record && (k == 0 ? CardArt_FromImage(path, record, why, sizeof(why))
                                       : CardArt_ThumbnailFromImage(path, record, why, sizeof(why)));
                if (ok) parts |= k == 0 ? ART_PICTURE | ART_THUMBNAIL : ART_THUMBNAIL;
                if (ok) snprintf(full[k], sizeof(full[k]), "%s", path);
            }
            if (!ok) Mods_Note(mod, "cards[%d]: \"%s\": %s", index, keys[k], why);
        }
        if (parts & ART_PICTURE) add_full_picture(full[0], record, 0);
        if (parts & ART_THUMBNAIL) add_full_picture(full[1][0] ? full[1] : full[0], record, 1);
    }
    /* Field-only artwork: its own PNG, never shared with "art" and never
     * patched into the card's own record, so only the field cutout ever
     * shows it. Its own transparency-preserving loader (art.h), not
     * CardArt_FromImage's: a background-removed PNG draws as a cutout of
     * its own shape, not a rectangle. */
    {
        const char *file = Json_String(Json_Member(entry, "field_art"), NULL);
        char path[1200], why[1300];
        if (file && *file && count) {
            if (!Paths_Contained(file) || snprintf(path, sizeof(path), "%s/%s", directory, file) >= (int)sizeof(path)) {
                Mods_Note(mod, "cards[%d]: \"field_art\": %s is outside the mod", index, file);
            } else {
                field_art_record = calloc(1, CARD_ART_RECORD);
                if (!field_art_record) {
                    Mods_Note(mod, "cards[%d]: \"field_art\": out of memory", index);
                } else if (!CardArt_FieldArtFromImage(path, field_art_record, why, sizeof(why))) {
                    Mods_Note(mod, "cards[%d]: \"field_art\": %s", index, why);
                    free(field_art_record);
                    field_art_record = NULL;
                }
            }
        }
    }
    for (n = 1; n <= count; n++) {
        char identity[192], fallback[32];
        const char *key = Json_String(Json_Member(entry, "id"), "");
        int id;
        if (replace) {
            id = base;
            /* Left out, "exodia" is false for the first entry to replace a
             * piece and what the earlier one said for a later one. */
            if ((unsigned)(id - EXODIA_FIRST_CARD_ID) < EXODIA_PIECE_COUNT &&
                (!replaced[id] || Json_Member(entry, "exodia"))) {
                not_exodia[id - EXODIA_FIRST_CARD_ID] = !Json_Bool(Json_Member(entry, "exodia"), 0);
            }
            replaced[id] = mod;
            replace_model_effect(mod, index, entry, id, &stats, was_monster, stars_given);
            goto own;
        }
        if (!*key) { snprintf(fallback, sizeof(fallback), "entry-%d", index); key = fallback; }
        if (strlen(key) > 80 || strspn(key, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != strlen(key)) {
            Mods_Note(mod, "cards[%d]: invalid stable id", index); break;
        }
        snprintf(identity, sizeof(identity), "%s:%s:%d", mod, key, n);
        if (Cards_FindIdentity(identity)) { Mods_Note(mod, "duplicate card identity %s", identity); break; }
        id = gCard_nCount + 1;
        identities[id] = strdup(identity);
        if (!identities[id]) { Mods_Note(mod, "out of memory for card identity"); break; }
        gCard_nCount = id;
        definitions[id] = entry;
        /* A replaced base lends the model and effect it was given. "model"
         * is a number or, as a replace's, a monster's name. */
        value = Cards_Reference(Json_Member(entry, "model"));
        if (value < 0 || (Json_Member(entry, "model") && Json_TypeOf(Json_Member(entry, "model")) == JSON_STRING &&
                          !retail_monster(value))) {
            if (n == 1) Mods_Note(mod, "cards[%d]: \"model\" must name a monster of the disc", index);
            value = 0;
        }
        model_ids[id] = (unsigned short)(value >= 1 && value <= CARD_COUNT ? value : Cards_ModelId(base));
        default_stars(id, &stats, was_monster, stars_given);
        /* A number or a card's name, as a replace's (Cards_Reference); 0,
         * as before names could be given, is the base's effect. */
        value = Json_Number(Json_Member(entry, "effect"), -1) == 0 ? 0 : Cards_Reference(Json_Member(entry, "effect"));
        if (n == 1 && (value < 0 || value > CARD_COUNT))
            Mods_Note(mod, "cards[%d]: \"effect\" must name a card of the disc", index);
        effect_ids[id] = (unsigned short)(value >= 1 && value <= CARD_COUNT ? value : Cards_EffectId(base));
        gCard_awBaseId[id] = (unsigned short)base;
        gCard_asNameSortKey[id - 1] = gCard_asNameSortKey[base - 1];
    own:
        if (effect_count >= 0) {
            monster_effects[id] = own_effects;
            monster_effect_counts[id] = (unsigned char)effect_count;
        } else if (!replace) {
            monster_effects[id] = monster_effects[base];
            monster_effect_counts[id] = monster_effect_counts[base];
        }
        trap_thresholds[id] = trap_threshold;
        fusion_groups[id] = entry_fusion_groups;
        has_fusion_groups[id] = (unsigned char)entry_has_fusion_groups;
        gDuel_adwCardStats[id - 1] = (int)stats;
        /* A monster with no star: star 0 is neutral from now on (stars.h). */
        if (((stats >> 26) & 0x1F) < CARD_TYPE_MAGIC && !(stats & (0xFu << 22))) Stars_NoteNoStar();
        gDuel_abCardLevelAttr[id] = level_attr;
        frames[id] = frame;
        names[id] = name && *name ? encode_name(mod, name, n, id) : NULL;
        descriptions[id] = description && *description ? encode_description(mod, description, id) : NULL;
        add_notes(mod, index, id, Json_Member(entry, "notes"));
        /* Reset as names and descriptions are, so a later mod's entry for
         * the same card without one does not keep an earlier mod's. */
        own_password[id] = (unsigned char)(has_password != 0);
        if (has_password) passwords[id] = password;
        art_records[id] = parts ? record : NULL;
        art_parts[id] = (unsigned char)parts;
        field_art_records[id] = field_art_record;
        if (title) {
            plates[id] = title;
        } else if (name && *name && (!named_plate || strstr(name, "{n}") || strstr(name, "{id}"))) {
            /* The name as it will read, "{n}" and all, on the card's plate:
             * one plate for the entry's cards unless the name numbers them. */
            char text[128];
            size_t length = 0;
            const char *p;
            named_plate = calloc(1, CARD_TITLE_BYTES);
            for (p = name; *p && length + 8 < sizeof(text); p++) {
                if (!strncmp(p, "{n}", 3)) { length += (size_t)snprintf(text + length, sizeof(text) - length, "%d", n); p += 2; }
                else if (!strncmp(p, "{id}", 4)) { length += (size_t)snprintf(text + length, sizeof(text) - length, "%d", id); p += 3; }
                else text[length++] = *p;
            }
            text[length] = '\0';
            /* Without a serif font the plate is left blank: better no name
             * on the card than its base's. */
            if (named_plate) CardArt_TitleFromName(text, named_plate);
            plates[id] = named_plate;
        } else if (name && *name) {
            plates[id] = named_plate;
        } else {
            /* Neither: the plate goes with the name, so a later mod's
             * replace that gives none shows the card's own name on the
             * picture too, not an earlier mod's plate. */
            plates[id] = NULL;
        }
        if (replace) continue;
        context->use[id] = (unsigned char)((Json_Bool(Json_Member(entry, "drops"), 1) ? 1 : 0) |
                                           (Json_Bool(Json_Member(entry, "opponents"), 0) ? 2 : 0));
        if (context->use[id] & 1) context->use_count[CARDS_USE_DROP][base]++;
        if (context->use[id] & 2) context->use_count[CARDS_USE_OPPONENT][base]++;
    }
    if (replace) {
        char text[128];
        retail_name(base, text, sizeof(text));
        say("%s: card %d %s replaced", mod, base, text);
    } else if (count) {
        char text[128];
        retail_name(base, text, sizeof(text));
        say("%s: cards %d-%d are copies of %d %s", mod, gCard_nCount - count + 1, gCard_nCount, base, text);
    }
}

static void add_mod(const char *mod, const char *directory, const struct JsonValue *cards, void *context)
{
    const struct JsonValue *entry;
    int i;
    for (i = 0, entry = Json_At(cards, 0); entry; i++, entry = Json_Next(entry)) add_entry(mod, directory, i, entry, context);
}

void Cards_Build(void)
{
    static int built;
    BuildContext *context;
    int id, use;
    if (built) return;
    built = 1;
    gCard_nCount = CARD_COUNT;
    memcpy(gDuel_adwCardStats, (const void *)(uintptr_t)RETAIL_STATS, CARD_COUNT * sizeof(int));
    memcpy(gCard_asNameSortKey, (const void *)(uintptr_t)RETAIL_SORT_KEYS, CARD_COUNT * sizeof(short));
    memcpy(gDuel_abCardLevelAttr, (const void *)(uintptr_t)RETAIL_LEVEL_ATTR, CARD_ID_END);
    for (id = 0; id <= CARD_COUNT; id++) gCard_awBaseId[id] = (unsigned short)id;
    context = calloc(1, sizeof(*context));
    if (!context) return;
    Mods_VisitCards(add_mod, context);
    Mods_SetCardResolver(Cards_FindIdentity);
    Mods_SetOverlapCards(overlap_card, Cards_NameUtf8, overlap_card_info);
    Mods_SetCardNotes(Cards_Notes, Cards_NoteTag);
    Mods_SetLimitSource(Tables_Limit);   /* read by Tables_Build below; asked later */
    {
        unsigned signature = 0;
        if (gCard_nCount > CARD_COUNT) {
            signature = 2166136261u;
            for (int card = CARD_ID_END; card <= gCard_nCount; card++) {
                const unsigned char *key = (const unsigned char *)identities[card];
                while (*key) signature = (signature ^ *key++) * 16777619u;
                signature = (signature ^ (unsigned)card) * 16777619u;
            }
        }
        Mods_SetCardSignature(signature);
    }
    /* The copies that take a base's place, grouped by base, for PickVariant. */
    for (use = 0; use < 2; use++) {
        int total = 0, base, fill[CARD_ID_END];
        for (base = 0; base <= CARD_COUNT; base++) {
            variant_start[use][base] = (unsigned short)total;
            fill[base] = total;
            total += context->use_count[use][base];
        }
        variant_start[use][CARD_ID_END] = (unsigned short)total;
        if (!total) continue;
        variants[use] = malloc((size_t)total * sizeof(unsigned short));
        if (!variants[use]) continue;   /* PickVariant then keeps every retail card */
        for (id = CARD_ID_END; id <= gCard_nCount; id++) {
            if (context->use[id] & (1 << use)) variants[use][fill[gCard_awBaseId[id]]++] = (unsigned short)id;
        }
    }
    free(context);
    if (gCard_nCount > CARD_COUNT) {
        fprintf(stderr, "memories-pc: %d cards (%d added by mods)\n", gCard_nCount, gCard_nCount - CARD_COUNT);
    }
    /* The mods' fusions, equips, rituals, drops and decks name cards, the
     * new ones included. */
    /* The duelists a mod adds, before the tables: "drops" and "decks" name
     * them, so they have to exist by the time those are read. */
    Duelists_Build();
    Tables_Build();
    /* What the stars a mod adds are worth noting, now the cards have them
     * and "limits" has set the stat cap (stars.h). */
    Stars_Check();
    /* And so do the starter decks a new game may be dealt (starter.h). */
    Starter_Build();
    /* And the card packs (packs.h), whose files and pictures a state's
     * mod signature covers. */
    Packs_Build();
    Mods_SetPackSignature(Packs_Signature());
}

/* --- what the game asks -------------------------------------------- */

int Cards_Valid(int id)
{
    return id >= CARD_ID_FIRST && id <= gCard_nCount;
}

int Cards_BaseId(int id)
{
    return Cards_Valid(id) ? gCard_awBaseId[id] : 0;
}

unsigned char *Cards_ChestSlot(void *state, int id)
{
    static unsigned char nowhere;
    uintptr_t at = (uintptr_t)state;
    if (id >= CARD_ID_FIRST && id <= CARD_COUNT) return (unsigned char *)state + SAVE_CHEST + id - CARD_ID_FIRST;
    nowhere = 0;
    if (!Cards_Valid(id)) return &nowhere;
    if (at == (uintptr_t)gDuel_awPlayerDeck) return &gCard_abExtraChest[id];
    if (at == SAVE_PAIR_BASE) return &gCard_abPairChest[0][id];
    if (at == SAVE_PAIR_BASE + SAVE_PAIR_STRIDE) return &gCard_abPairChest[1][id];
    if (at == SAVE_PAIR_BASE + SAVE_PAIR_COPY) return &gCard_abPairPending[0][id];
    if (at == SAVE_PAIR_BASE + SAVE_PAIR_STRIDE + SAVE_PAIR_COPY) return &gCard_abPairPending[1][id];
    return &nowhere;
}

int Cards_Seen(int id)
{
    if (id >= CARD_ID_FIRST && id <= CARD_COUNT) return Campaign_TestStoryFlag(LIBRARY_SEEN_FLAG_BASE + id);
    if (!Cards_Valid(id)) return 0;
    return (gCard_abExtraSeen[id >> 3] >> (id & 7)) & 1;
}

int Cards_LibraryPlaceholder(int id)
{
    return Settings_Get(SET_LIBRARY_ALL_CARDS) && Cards_Valid(id) && !Cards_Seen(id);
}

void Cards_MarkSeen(int id)
{
    if (id >= CARD_ID_FIRST && id <= CARD_COUNT) {
        Library_UpdateCardUsedFlag(LIBRARY_SEEN_FLAG_BASE + id);
    } else if (Cards_Valid(id)) {
        gCard_abExtraSeen[id >> 3] |= (unsigned char)(1u << (id & 7));
    }
}

const unsigned char *Cards_NameText(int id)
{
    return Cards_Valid(id) ? names[id] : NULL;
}

const unsigned char *Cards_NameCodes(int id)
{
    const unsigned char *name;
    if (!Cards_Valid(id)) return NULL;
    name = Cards_NameText(id);
    if (!name) {
        int base = Cards_BaseId(id);
        const unsigned short *offsets = (const unsigned short *)(uintptr_t)RETAIL_NAME_OFFSETS;
        name = Text_Resolve(0x8000 + base, (const unsigned char *)(uintptr_t)(TEXT_BANK + offsets[base]));
    }
    return name;
}

int Cards_NameUtf8(int id, char *out, size_t size)
{
    const unsigned char *name;
    size_t n = 0;
    if (!Cards_Valid(id) || !size) return 0;
    name = Cards_NameCodes(id);
    while (name && *name < 0xF6) {
        int code = *name++;
        uint32_t c;
        if (code >= 0xF0) code = ((code - 0xF0) << 8) | *name++;
        c = code ? Glyphs_Character(code) : ' ';
        if (!c) c = '?';
        if (c < 0x80 && n + 1 < size) out[n++] = (char)c;
        else if (c < 0x800 && n + 2 < size) {
            out[n++] = (char)(0xC0 | c >> 6);
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000 && n + 3 < size) {
            out[n++] = (char)(0xE0 | c >> 12);
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else if (c >= 0x10000 && n + 4 < size) {
            out[n++] = (char)(0xF0 | c >> 18);
            out[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            break;
        }
    }
    out[n] = '\0';
    return 1;
}

const unsigned char *Cards_DescriptionText(int id)
{
    return Cards_Valid(id) ? descriptions[id] : NULL;
}

const char *Cards_Notes(int id)
{
    return Cards_Valid(id) ? card_notes[id] : NULL;
}

int Cards_NoteTag(int id, const char *key, char *out, size_t size)
{
    return CardNotes_Tag(Cards_Notes(id), key, out, size);
}

int Cards_OwnPassword(int id, unsigned *password)
{
    if (!Cards_Valid(id) || !own_password[id]) return 0;
    *password = passwords[id];
    return 1;
}

/* The bytes written over the base's are no longer the disc's: a texture
 * pack must not find the base card's picture in them (texture_dump.h),
 * even in the words that happen to match it. */
static void patch(unsigned char *to, const unsigned char *from, size_t bytes)
{
    memcpy(to, from, bytes);
    TextureDump_Written(to, (unsigned)bytes);
}

/* Whose artwork a card shows for `part`: its own, else its base's (a retail
 * card a mod replaced), else none (0). */
static int art_of(int id, int part)
{
    if (!Cards_Valid(id)) return 0;
    if (art_parts[id] & part) return id;
    return art_parts[Cards_BaseId(id)] & part ? Cards_BaseId(id) : 0;
}

/* The plate of a retail card a translation renames (its name, string
 * 0x8000 + id, rewritten by a mod's "text"): the retail plate is the English
 * name drawn into the art, so it is set anew from the translated name, as a
 * mod's own name is (above), once per card. A translation that keeps the
 * name, or a system with no serif face, leaves the retail plate as it is:
 * the English name beats a blank one when the name reads right everywhere
 * else. HD text still sets the title from the name at its own size
 * (hd_text.h) over this plate. */
static unsigned char *text_plates[CARD_ID_END];
static unsigned char text_plate_tried[CARD_ID_END];

/* The next character of a name's glyph codes (0 at its end), advancing. */
static uint32_t name_character(const unsigned char **at)
{
    int code = *(*at)++;
    if (code >= 0xF6) return 0;
    if (code >= 0xF0) code = ((code - 0xF0) << 8) | *(*at)++;
    return code ? Glyphs_Character(code) : ' ';
}

/* Whether the name the text gives is the disc's, character for character
 * (a translation lists every name, the ones it keeps too). Compared as
 * characters, not as retail_name's ASCII, which spells a glyph outside
 * it as '?', nor as codes, which differ for a glyph the port added. */
static int retail_name_kept(int id)
{
    const unsigned short *offsets = (const unsigned short *)(uintptr_t)RETAIL_NAME_OFFSETS;
    const unsigned char *retail = (const unsigned char *)(uintptr_t)(TEXT_BANK + offsets[id]);
    const unsigned char *name = Cards_NameCodes(id);
    uint32_t a, b;
    if (!name) return 1;
    do {
        a = name_character(&name);
        b = name_character(&retail);
    } while (a == b && a);
    return a == b;
}

static const unsigned char *translated_plate(int id)
{
    char name[128];
    if (id < CARD_ID_FIRST || id > CARD_COUNT || names[id] || !Text_Overridden(0x8000 + id)) return NULL;
    if (!text_plate_tried[id]) {
        text_plate_tried[id] = 1;
        if (!retail_name_kept(id) && Cards_NameUtf8(id, name, sizeof(name)) &&
            (text_plates[id] = malloc(CARD_TITLE_BYTES)) != NULL && !CardArt_TitleFromName(name, text_plates[id])) {
            free(text_plates[id]);
            text_plates[id] = NULL;
        }
    }
    return text_plates[id];
}

/* The Password screen's card packs (pack_shop.c): the next record loaded
 * for card `override_id` takes a pack's picture and plate, or its plate. */
static int override_id, override_fired;
static const unsigned char *override_record, *override_plate;

void Cards_OverrideArt(int id, const unsigned char *record, const unsigned char *plate)
{
    override_fired = 0;
    override_id = id;
    override_record = record;
    override_plate = plate;
}

int Cards_ArtOverridden(void)
{
    int fired = override_fired;
    override_fired = 0;
    return fired;
}

void Cards_PatchArtRecord(int id, unsigned char *record)
{
    const unsigned char *translated;
    int from;
    if (!Cards_Valid(id)) return;
    if (override_id && id == override_id && override_record) {
        /* A pack's own picture, whole: the art, its palette and the plate. */
        override_id = 0;
        override_fired = 1;
        patch(record, override_record, CARD_THUMB_PIXELS);
        return;
    }
    if ((from = art_of(id, ART_PICTURE)) != 0) patch(record, art_records[from], CARD_TITLE_PIXELS);
    /* The plate is not reported: it sits in the middle of the sector that
     * also ends the base's palette, and a write inside a delivery drops all
     * of it (texture_dump.c, forget), so a copy with only a name of its own
     * would lose the base's pack picture. The words of a plate that match
     * the base's are the same inks, so its pack picture there is no harm. */
    if (plates[id] || plates[Cards_BaseId(id)]) {
        memcpy(record + CARD_TITLE_PIXELS, plates[id] ? plates[id] : plates[Cards_BaseId(id)], CARD_TITLE_BYTES);
    } else if (!names[id] && (translated = translated_plate(Cards_BaseId(id))) != NULL) {
        memcpy(record + CARD_TITLE_PIXELS, translated, CARD_TITLE_BYTES);
    }
    if ((from = art_of(id, ART_THUMBNAIL)) != 0) {
        patch(record + CARD_THUMB_PIXELS, art_records[from] + CARD_THUMB_PIXELS, CARD_THUMB_BLOCK);
    }
    if (override_id && id == override_id && override_plate) {
        /* A pack shown by its cover card's art: the pack's name on the plate. */
        override_id = 0;
        override_fired = 1;
        memcpy(record + CARD_TITLE_PIXELS, override_plate, CARD_TITLE_BYTES);
    }
}

void Cards_PatchThumbnail(int id, unsigned char *block)
{
    int from = art_of(id, ART_THUMBNAIL);
    if (from) patch(block, art_records[from] + CARD_THUMB_PIXELS, CARD_THUMB_BLOCK);
}

/* The card's own field_art record (picture at CARD_ART_PIXELS, CLUT at
 * CARD_ART_CLUT), or its base's, or NULL when neither has one: the cutout
 * then falls back to the card's own art, as before. */
const unsigned char *Cards_FieldArtRecord(int id)
{
    if (!Cards_Valid(id)) return NULL;
    if (field_art_records[id]) return field_art_records[id];
    return field_art_records[Cards_BaseId(id)];
}

int Cards_PickVariant(int id, int use)
{
    int first, count, pick;
    if (use < 0 || use > 1 || id < CARD_ID_FIRST || id > CARD_COUNT || !variants[use]) return id;
    first = variant_start[use][id];
    count = variant_start[use][id + 1] - first;
    if (!count) return id;
    pick = Memories_Rand() % (count + 1);
    return pick ? variants[use][first + pick - 1] : id;
}

/* --- beside the save -------------------------------------------------- */

/* cards/<duelist code>.txt in the user directory: a section per save,
 *
 *     save <sequence> <token>
 *     chest2 <identity> <count>
 *     seen2 <identity>
 *     deck2 <slot> <old-id> <base> <identity>
 *     recent2 <index> <old-id> <identity>
 *     end
 *
 * recent2 is the save's recent-drops list (SAVE_RECENT, Build Deck's "New!"
 * mark and its sort by new): one line for each entry past the disc's cards,
 * with "-" for one this run did not have when it saved. A load puts back
 * the card of that identity (none when the mod is missing) wherever the list
 * holds the old id. The id alone is the key: a section's recent2 lines are
 * one numbering (the run's that last wrote the list), so an old id names one
 * card wherever it stands in the list. A run with no card mod has none of
 * them and clears their entries on load. A save from
 * before recent2 has no such lines and its ids stay as they are: they are
 * right as long as the mods are, and zeroing them would wipe right marks.
 * A trade writes the save's first 0x400 bytes only, not this list, so a
 * trade's rewrite of the section carries its recent2 lines over unchanged.
 *
 * The token is the save slot's (save_slots.h), drawn afresh at each save, so
 * two slots holding the same duelist at the same sequence -- one game saved
 * twice, then played on from the older -- each find their own. A section
 * whose token a slot still holds is kept; of the rest, the newest
 * KEPT_SAVES. Sections from before tokens have none and are found by
 * sequence alone.
 *
 * Stable identities remap to this run's ids. Legacy numeric sections require
 * explicit migration with the original mods and order; preserve them until then. */

/* The tokens of the save being played, of the two saves a two-player
 * screen loaded, and of every slot (save_cards.c sets them). */
static unsigned play_token, pair_tokens[2], live_tokens[32];
static int live_count;

void Cards_SetSlotTokens(unsigned playing, const unsigned *live, int count)
{
    play_token = playing;
    live_count = count < 0 ? 0 : count > 32 ? 32 : count;
    if (live_count) memcpy(live_tokens, live, (size_t)live_count * sizeof(*live));
}

void Cards_SetPairTokens(unsigned first, unsigned second)
{
    pair_tokens[0] = first;
    pair_tokens[1] = second;
}

static int live(unsigned token)
{
    for (int i = 0; token && i < live_count; i++) if (live_tokens[i] == token) return 1;
    return 0;
}

/* A section's first line: its sequence and token (0 for none). */
static int section_header(const char *line, unsigned *sequence, unsigned *token)
{
    int got = sscanf(line, "save %u %x", sequence, token);
    if (got == 1) *token = 0;
    return got >= 1;
}

/* Which section a save of `sequence` and `token` reads: its own, else the
 * newest no later than it (what the player had when they last played with
 * the mod). 0 when there is none. */
static int choose_section(FILE *file, unsigned sequence, unsigned token, unsigned *chosen, unsigned *chosen_token)
{
    char line[512];
    int have = 0, exact = 0;
    rewind(file);
    while (fgets(line, sizeof(line), file)) {
        unsigned value, tag;
        if (!section_header(line, &value, &tag) || value > sequence || exact) continue;
        if (token && value == sequence && tag == token) {
            *chosen = value; *chosen_token = tag; have = exact = 1;
        } else if (!have || value > *chosen) {
            *chosen = value; *chosen_token = tag; have = 1;
        }
    }
    rewind(file);
    return have;
}

static int state_word(const void *state, int offset)
{
    int value;
    memcpy(&value, (const unsigned char *)state + offset, sizeof(value));
    return value;
}

static int sidecar_path(char *out, size_t size, int code)
{
    char relative[64];
    snprintf(relative, sizeof(relative), "cards/%08X.txt", (unsigned)code);
    return Paths_User(out, size, relative);
}

/* What a section says about the deck: slot, card id and its base. */
typedef struct {
    int count;
    unsigned short slot[DECK_SIZE], id[DECK_SIZE], base[DECK_SIZE], resolved[DECK_SIZE];
    unsigned char stable[DECK_SIZE];
} DeckNotes;

/* What a section says about the recent-drops list: old id and its card now. */
typedef struct {
    int count;
    unsigned short id[DUEL_RECENT_CARD_DROP_COUNT], resolved[DUEL_RECENT_CARD_DROP_COUNT];
} RecentNotes;

/* Read the section for `sequence` into `chest` (and `seen`, `deck` and
 * `recent`, if given). A save made while no card mod was applied has no
 * section of its own; it has what the newest earlier one held, which is what
 * the player had when they last played with the mod. Returns the sequence
 * read, or -1. */
static long read_section(int code, unsigned sequence, unsigned token, unsigned char *chest, unsigned char *seen,
                         DeckNotes *deck, RecentNotes *recent)
{
    char path[1024], line[512];
    FILE *file;
    unsigned chosen = 0, chosen_token = 0;
    int inside = 0, legacy_warning = 0, have;
    int migrate = getenv("MEMORIES_MIGRATE_CARD_IDS") && !strcmp(getenv("MEMORIES_MIGRATE_CARD_IDS"), "1");
    if (deck) deck->count = 0;
    if (recent) recent->count = 0;
    if (sidecar_path(path, sizeof(path), code)) return -1;
    file = fopen(path, "r");
    if (!file) return -1;
    have = choose_section(file, sequence, token, &chosen, &chosen_token);
    while (have && fgets(line, sizeof(line), file)) {
        unsigned value, tag;
        int id, count, slot, base;
        char identity[192];
        if (section_header(line, &value, &tag)) {
            inside = value == chosen && tag == chosen_token;
        } else if (!inside) {
            continue;
        } else if (sscanf(line, "chest2 %191s %d", identity, &count) == 2) {
            id = Cards_FindIdentity(identity);
            /* A byte a card: a mod's "limits" may keep up to 255 (tables.h). */
            if (id) chest[id] = (unsigned char)clamp(count, 0, TABLES_LIMIT_CHEST_MAX);
        } else if (sscanf(line, "seen2 %191s", identity) == 1) {
            id = Cards_FindIdentity(identity);
            if (seen && id) seen[id >> 3] |= (unsigned char)(1u << (id & 7));
        } else if (sscanf(line, "deck2 %d %d %d %191s", &slot, &id, &base, identity) == 4) {
            if (deck && deck->count < DECK_SIZE && slot >= 0 && slot < DECK_SIZE) {
                int at = deck->count++;
                deck->slot[at] = (unsigned short)slot; deck->id[at] = (unsigned short)id;
                deck->base[at] = (unsigned short)base; deck->stable[at] = 1;
                deck->resolved[at] = (unsigned short)Cards_FindIdentity(identity);
            }
        } else if (sscanf(line, "recent2 %d %d %191s", &slot, &id, identity) == 3) {
            if (recent && recent->count < DUEL_RECENT_CARD_DROP_COUNT && id > CARD_COUNT && id < CARD_TABLE_ID_END) {
                int at = recent->count++;
                recent->id[at] = (unsigned short)id;
                recent->resolved[at] = (unsigned short)(strcmp(identity, "-") ? Cards_FindIdentity(identity) : 0);
            }
        } else if (sscanf(line, "chest %d %d", &id, &count) == 2) {
            if (!migrate && !legacy_warning++) fprintf(stderr, "memories-pc: legacy card IDs have no identities; restore the original card mods and use MEMORIES_MIGRATE_CARD_IDS=1 to migrate\n");
            if (migrate && id > CARD_COUNT && Cards_Valid(id)) chest[id] = (unsigned char)clamp(count, 0, TABLES_LIMIT_CHEST_MAX);
        } else if (sscanf(line, "seen %d", &id) == 1) {
            if (migrate && seen && id > CARD_COUNT && Cards_Valid(id)) seen[id >> 3] |= (unsigned char)(1u << (id & 7));
        } else if (sscanf(line, "deck %d %d %d", &slot, &id, &base) == 3) {
            if (deck && deck->count < DECK_SIZE && slot >= 0 && slot < DECK_SIZE) {
                deck->slot[deck->count] = (unsigned short)slot;
                deck->id[deck->count] = (unsigned short)id;
                deck->stable[deck->count] = (unsigned char)!migrate;
                deck->resolved[deck->count] = 0;
                deck->base[deck->count++] = (unsigned short)base;
            }
        } else if (!strncmp(line, "end", 3)) {
            inside = 0;
        }
    }
    fclose(file);
    return have ? (long)chosen : -1;
}

/* A deck holding a card this run does not have (the mod that added it is
 * not applied, or adds fewer): each such slot gets the retail card it was a
 * copy of, or is emptied, so a duel never deals a card that is not there. */
static void repair_deck(unsigned short *cards, const DeckNotes *notes)
{
    int slot, i;
    for (slot = 0; slot < DECK_SIZE; slot++) {
        int id = cards[slot], base = 0;
        int remapped = 0;
        for (i = 0; notes && i < notes->count; i++) {
            if (notes->stable[i] && notes->slot[i] == slot && notes->id[i] == id) {
                cards[slot] = notes->resolved[i] ? notes->resolved[i] : notes->base[i];
                remapped = 1; break;
            }
        }
        if (remapped || id == 0 || Cards_Valid(id)) continue;
        for (i = 0; notes && i < notes->count; i++) {
            if (notes->slot[i] == slot && notes->id[i] == id) base = notes->base[i];
        }
        cards[slot] = (unsigned short)(base >= CARD_ID_FIRST && base <= CARD_COUNT ? base : 0);
        fprintf(stderr, "memories-pc: deck slot %d held card %d, which this run does not have; now %d\n",
                slot + 1, id, cards[slot]);
    }
}

/* The recent-drops list as this run numbers it (the header comment above):
 * an entry the section names by its old id gets that card back, or none;
 * the disc's cards, and every entry of a save from before recent2, stay. */
static void repair_recent(unsigned char *state, const RecentNotes *notes)
{
    int index, i;
    for (index = 0; notes && notes->count && index < DUEL_RECENT_CARD_DROP_COUNT; index++) {
        unsigned short id;
        memcpy(&id, state + SAVE_RECENT + index * 2, sizeof(id));
        if (id <= CARD_COUNT) continue;
        for (i = 0; i < notes->count; i++) {
            if (notes->id[i] == id) {
                memcpy(state + SAVE_RECENT + index * 2, &notes->resolved[i], sizeof(id));
                break;
            }
        }
    }
}

/* `recent` is the save's recent-drops list, or NULL to keep the section's
 * recent2 lines as they are (a trade does not write the list). */
static void write_section(int code, unsigned sequence, unsigned token, const unsigned char *chest,
                          const unsigned char *seen, const unsigned short *deck, const unsigned short *recent)
{
    char path[1024], temporary[1040], line[512];
    unsigned kept[KEPT_SAVES], kept_tokens[KEPT_SAVES];
    int kept_count = 0, i, id, keep = 0, any = 0, migrate = 0;
    FILE *in, *out;
    for (id = CARD_ID_END; id <= gCard_nCount; id++) {
        if (chest[id] || (seen && (seen[id >> 3] >> (id & 7)) & 1)) { any = 1; break; }
    }
    for (i = 0; deck && i < DECK_SIZE; i++) any |= deck[i] > CARD_COUNT;
    for (i = 0; recent && i < DUEL_RECENT_CARD_DROP_COUNT; i++) any |= recent[i] > CARD_COUNT;
    if (sidecar_path(path, sizeof(path), code)) return;
    in = fopen(path, "r");
    if (!in && !any) return;   /* nothing to say about a save without new cards */
    /* Ambiguous legacy (numeric) lines are only translated with explicit
     * migration; otherwise they are carried over unchanged while new progress
     * is still saved. Either way the original is kept once as .legacy. */
    if (in) {
        int legacy = 0;
        while (fgets(line, sizeof(line), in)) {
            if (!strncmp(line, "chest ", 6) || !strncmp(line, "seen ", 5) || !strncmp(line, "deck ", 5)) legacy = 1;
        }
        rewind(in);
        migrate = getenv("MEMORIES_MIGRATE_CARD_IDS") && !strcmp(getenv("MEMORIES_MIGRATE_CARD_IDS"), "1");
        if (legacy && !migrate)
            fprintf(stderr, "memories-pc: keeping legacy card sidecar lines in %s pending identity migration\n", path);
        if (legacy) {
            char backup[1040]; FILE *copy;
            snprintf(backup, sizeof(backup), "%s.legacy", path);
            copy = fopen(backup, "rb");
            if (copy) fclose(copy);
            else {
                copy = fopen(backup, "wb");
                if (!copy) { fclose(in); return; }
                while (fgets(line, sizeof(line), in)) fputs(line, copy);
                { int failed = ferror(copy); if (fclose(copy)) failed = 1;
                  if (failed) { remove(backup); fclose(in); return; } }
                rewind(in);
            }
        }
    }
    /* Every section a slot still holds stays, and the newest of the others;
     * this save's own is written again below. */
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            unsigned value, tag;
            if (!section_header(line, &value, &tag) || (value == sequence && tag == token) || live(tag)) continue;
            if (kept_count < KEPT_SAVES - 1) {
                kept[kept_count] = value;
                kept_tokens[kept_count++] = tag;
            } else {
                int oldest = 0;
                for (i = 1; i < kept_count; i++) if (kept[i] < kept[oldest]) oldest = i;
                if (value > kept[oldest]) { kept[oldest] = value; kept_tokens[oldest] = tag; }
            }
        }
        rewind(in);
    }
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    {
        char directory[1024];
        char *slash;
        snprintf(directory, sizeof(directory), "%s", path);
        slash = strrchr(directory, '/');
        if (slash) { *slash = '\0'; Paths_MakeDirs(directory); }
    }
    Paths_WriteBegin();
    out = fopen(temporary, "w");
    if (!out) {
        char why[1200];
        Paths_WriteError(why, sizeof(why), path);
        if (in) fclose(in);
        fprintf(stderr, "memories-pc: cannot write %s\n", why);
        return;
    }
    fprintf(out, "# The cards mods added, as the saves of duelist %08X hold them.\n", (unsigned)code);
    while (in && fgets(line, sizeof(line), in)) {
        unsigned value, tag;
        if (section_header(line, &value, &tag)) {
            keep = !(value == sequence && tag == token) && live(tag);
            for (i = 0; i < kept_count; i++) keep |= kept[i] == value && kept_tokens[i] == tag;
        }
        if (keep) {
            int old_id, old_count, old_slot, old_base;
            if (!migrate) fputs(line, out);
            else if (sscanf(line, "chest %d %d", &old_id, &old_count) == 2 && *Cards_Identity(old_id))
                fprintf(out, "chest2 %s %d\n", Cards_Identity(old_id), old_count);
            else if (sscanf(line, "seen %d", &old_id) == 1 && *Cards_Identity(old_id))
                fprintf(out, "seen2 %s\n", Cards_Identity(old_id));
            else if (sscanf(line, "deck %d %d %d", &old_slot, &old_id, &old_base) == 3 && *Cards_Identity(old_id))
                fprintf(out, "deck2 %d %d %d %s\n", old_slot, old_id, old_base, Cards_Identity(old_id));
            else if (strncmp(line, "chest ", 6) && strncmp(line, "seen ", 5) && strncmp(line, "deck ", 5)) fputs(line, out);
        }
    }
    if (token) fprintf(out, "save %u %08x\n", sequence, token);
    else fprintf(out, "save %u\n", sequence);
    /* Carry ownership of temporarily missing mods into the new section.
     * Identity-based records cannot collide with another mod's live IDs. */
    if (in) {
        unsigned newest = 0, newest_token = 0; int have, selected_section = 0;
        have = choose_section(in, sequence, token, &newest, &newest_token);
        while (have && fgets(line, sizeof(line), in)) {
            unsigned value, tag; char identity[192];
            int index, old_id;
            if (section_header(line, &value, &tag)) selected_section = value == newest && tag == newest_token;
            else if (!strncmp(line, "end", 3)) selected_section = 0;
            else if (selected_section && (sscanf(line, "chest2 %191s", identity) == 1 || sscanf(line, "seen2 %191s", identity) == 1) &&
                     !Cards_FindIdentity(identity)) fputs(line, out);
            else if (selected_section && !recent && sscanf(line, "recent2 %d %d %191s", &index, &old_id, identity) == 3)
                fputs(line, out);
        }
    }
    for (id = CARD_ID_END; id <= gCard_nCount; id++) {
        if (chest[id]) fprintf(out, "chest2 %s %d\n", Cards_Identity(id), chest[id]);
    }
    for (id = CARD_ID_END; seen && id <= gCard_nCount; id++) {
        if ((seen[id >> 3] >> (id & 7)) & 1) fprintf(out, "seen2 %s\n", Cards_Identity(id));
    }
    for (i = 0; deck && i < DECK_SIZE; i++) {
        if (deck[i] > CARD_COUNT && Cards_Valid(deck[i])) fprintf(out, "deck2 %d %d %d %s\n", i, deck[i], Cards_BaseId(deck[i]), Cards_Identity(deck[i]));
    }
    for (i = 0; recent && i < DUEL_RECENT_CARD_DROP_COUNT; i++) {
        if (recent[i] > CARD_COUNT)
            fprintf(out, "recent2 %d %d %s\n", i, recent[i], *Cards_Identity(recent[i]) ? Cards_Identity(recent[i]) : "-");
    }
    fprintf(out, "end\n");
    if (in) fclose(in);
    if (fclose(out) != 0 || rename(temporary, path) != 0) {
        char why[1200];
        fprintf(stderr, "memories-pc: cannot write %s\n", Paths_WriteError(why, sizeof(why), path));
        remove(temporary);
        return;
    }
    say("saved duelist %08X save %u", (unsigned)code, sequence);
}

static void clear_extra(void)
{
    memset(gCard_abExtraChest, 0, CARD_TABLE_ID_END);
    memset(gCard_abExtraSeen, 0, (CARD_TABLE_ID_END + 7) / 8);
}

void Cards_SaveLoaded(const void *state)
{
    int code = state_word(state, SAVE_DUELIST_CODE);
    unsigned sequence = (unsigned)state_word(state, SAVE_SEQUENCE);
    DeckNotes deck;
    RecentNotes recent;
    long read;
    clear_extra();
    gCard_nExtraOwner = code;
    /* Read even without a card mod: the deck may need its slots back. */
    read = read_section(code, sequence, play_token, gCard_abExtraChest, gCard_abExtraSeen, &deck, &recent);
    repair_deck((unsigned short *)state, &deck);
    repair_recent((unsigned char *)state, &recent);
    if (read >= 0) say("loaded duelist %08X save %u (from the section of save %ld)", (unsigned)code, sequence, read);
}

void Cards_SaveWritten(const void *state, unsigned sequence)
{
    int code = state_word(state, SAVE_DUELIST_CODE);
    if (gCard_nCount <= CARD_COUNT) return;   /* no card mod: the file is left as it is */
    write_section(code, sequence, play_token, gCard_abExtraChest, gCard_abExtraSeen, (const unsigned short *)state,
                  (const unsigned short *)((const unsigned char *)state + SAVE_RECENT));
}

void Cards_PairLoaded(void)
{
    int slot;
    for (slot = 0; slot < 2; slot++) {
        void *state = (void *)(uintptr_t)(SAVE_PAIR_BASE + slot * SAVE_PAIR_STRIDE);
        DeckNotes deck;
        memset(gCard_abPairChest[slot], 0, CARD_TABLE_ID_END);
        read_section(state_word(state, SAVE_DUELIST_CODE), (unsigned)state_word(state, SAVE_SEQUENCE),
                     pair_tokens[slot], gCard_abPairChest[slot], NULL, &deck, NULL);
        repair_deck((unsigned short *)state, &deck);
    }
}

void Cards_PairBackup(void)
{
    memcpy(gCard_abPairPending, gCard_abPairChest, sizeof(gCard_abPairChest));
}

void Cards_PairCommit(void)
{
    static unsigned char seen[(CARD_TABLE_ID_END + 7) / 8], chest[CARD_TABLE_ID_END];
    int slot;
    memcpy(gCard_abPairChest, gCard_abPairPending, sizeof(gCard_abPairChest));
    if (gCard_nCount <= CARD_COUNT) return;
    for (slot = 0; slot < 2; slot++) {
        const void *state = (const void *)(uintptr_t)(SAVE_PAIR_BASE + slot * SAVE_PAIR_STRIDE);
        int code = state_word(state, SAVE_DUELIST_CODE);
        unsigned sequence = (unsigned)state_word(state, SAVE_SEQUENCE);
        /* A trade writes the trunk, not the sequence number or what the
         * Library has seen: the same section, with the new trunk. */
        memset(seen, 0, sizeof(seen));
        read_section(code, sequence, pair_tokens[slot], chest, seen, NULL, NULL);
        write_section(code, sequence, pair_tokens[slot], gCard_abPairChest[slot], seen, (const unsigned short *)state,
                      NULL);
    }
}

void Cards_Frame(void)
{
    CardLayoutArt_Prewarm();
    const void *state = gDuel_awPlayerDeck;
    int code = state_word(state, SAVE_DUELIST_CODE);
    /* NEW GAME writes a new duelist code into the running save. */
    if (code != gCard_nExtraOwner) {
        clear_extra();
        gCard_nExtraOwner = code;
    }
}
