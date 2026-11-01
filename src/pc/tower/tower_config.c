/* A mod's "tower" as written, and the progress a save has in it (tower.h,
 * notes/tower-mode.md).
 *
 * Nothing here touches the game: the manifest is read through the JSON
 * reader, names through the duelist and card lists, and the progress is
 * numbers in a struct and lines in a file, so the tests run all of it
 * without a disc. */
#include "tower.h"
#include "pc/free_duel/busts.h"
#include "pc/mods/json.h"
#include "pc/mods/mods.h"
#include "pc/cards/cards.h"
#include "pc/cards/tables.h"
#include "pc/free_duel/duelists.h"
#include "pc/platform/paths.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static TowerConfig config;
static int defined;
static const char *reading_directory;   /* the mod's folder while its tower is read */

/* Every line's text and every PNG made, freed with the tower. */
static void free_tower_lines(TowerConfig *tower)
{
    int i;
    for (i = 0; i < tower->line_count; i++) free(tower->lines[i].text);
    free(tower->lines);
    tower->lines = NULL;
    tower->line_count = tower->line_room = 0;
    for (i = 0; i < tower->art_count; i++) {
        free(tower->arts[i].texels);
        free(tower->arts[i].bank);
    }
    tower->art_count = 0;
}

static void free_lines(void) { free_tower_lines(&config); }

/* --- the campaign's scenes and characters ---------------------------------- */

/* Mode 0's pictures (ScriptImage_RequestTransfer, 33 sectors from WA sector
 * 0x21D5), named for what they show; 0x042-0x049 are empty on the disc. */
static const char *const scene_names[TOWER_SCENE_NAMES] = {
    "black", "palace gate", "palace chamber", "jungle temple door", "card shop", "colonnade hall",
    "moonlit colonnade", "collapsed ruins", "classroom", "tournament lounge", "card collection",
    "shop street", "temple ruins", "destroyed town", "ruined altar room", "ruined chamber",
    "sunlit ruined hall", "fiery valley gate", "seal shrine", "green pyramid", "green mausoleum",
    "obelisk avenue", "torch corridor", "cliff temple", "night temple stairs", "jungle pyramid",
    "green shrine", "dusk tower", "altar hall", "desert sphinx", "sphinx shrine", "meadow temple",
    "twin statue shrine", "throne room", "hall of fires", "eye chamber", "town at night",
    "night balcony", "golden doors", "golden door chamber", "statue gallery", "burning pyramid",
};

static int bcd(int index) { return (index / 10) << 4 | index % 10; }
static int unbcd(int id) { return ((id >> 4) & 0xF) * 10 + (id & 0xF); }

int Tower_SceneNamed(const char *name)
{
    int i;
    for (i = 0; i < TOWER_SCENE_NAMES; i++) {
        const char *a = name, *b = scene_names[i];
        for (; *a && *b; a++, b++) {
            int x = tolower((unsigned char)*a);
            if (x == '_' || x == '-') x = ' ';
            if (x != (unsigned char)*b) break;
        }
        if (!*a && !*b) return bcd(i);
    }
    return -1;
}

const char *Tower_SceneName(int id)
{
    if (id < 0 || id >= 0x100 || (id & 0xF) > 9 || unbcd(id) >= TOWER_SCENE_NAMES) return NULL;
    return scene_names[unbcd(id)];
}

int Tower_RetailScene(int id)
{
    static const int counts[3] = {TOWER_SCENE_NAMES, 42, 13};
    int mode = id >> 8;
    if (id < 0 || mode > 2 || (id & 0xF) > 9 || ((id >> 4) & 0xF) > 9) return 0;
    return unbcd(id & 0xFF) < counts[mode];
}

/* The character each retail duelist's own dialogue brings in before its
 * duel: the last F6 01-40 of the text that starts its duel (F8 0D
 * <duelist>) in the campaign's dialogue; Jono's two duels show none there,
 * and he is 0x02 where he speaks ({fx 02 00} "Can you believe that guy!?",
 * text 0x521); Heishin's second, Heishin's own 0x07. 0: the campaign never
 * shows one (the villagers, the five shrine mages). */
static const unsigned char duelist_characters[39] = {
    0,
    0x01, 0x03, 0x02, 0, 0, 0, 0x04, 0x07, 0x10, 0x11,          /* Simon Muran .. Weevil Underwood */
    0x12, 0x13, 0x0F, 0x14, 0x15, 0x2F, 0x0E, 0x18, 0x02, 0x03, /* Mai Valentine .. Teana 2nd */
    0, 0x1A, 0, 0x1C, 0, 0x1E, 0, 0x20, 0, 0x22,                /* Ocean Mage .. High Mage Kepura */
    0x2E, 0x04, 0x23, 0x24, 0x07, 0x04, 0x25, 0x26,             /* Labyrinth Mage .. Nitemare */
};

int Tower_DuelistCharacter(int duelist)
{
    if (duelist <= 0) return 0;
    if (duelist >= (int)sizeof(duelist_characters)) duelist = Duelists_BaseId(duelist);
    return duelist > 0 && duelist < (int)sizeof(duelist_characters) ? duelist_characters[duelist] : 0;
}

int Tower_EmptyCharacter(int id)
{
    return id == 0x00 || id == 0x05 || id == 0x0B || id == 0x27 || id == 0x28 || id == 0x29;
}

void Tower_Reset(void)
{
    free_lines();
    memset(&config, 0, sizeof(config));
    defined = 0;
}

const TowerConfig *Tower_Config(void) { return defined ? &config : NULL; }
const char *Tower_Mod(void) { return defined ? config.mod : NULL; }
const char *Tower_Label(void) { return defined && config.label[0] ? config.label : "TOWER"; }

/* --- reading ---------------------------------------------------------------- */

/* Keys a part of the tower knows; another is noted, as a misspelling would
 * otherwise be silently left out. */
static void check_keys(const char *mod, const char *where, const JsonValue *object, const char *const *known,
                       size_t count)
{
    const JsonValue *member;
    for (member = Json_At(object, 0); member; member = Json_Next(member)) {
        size_t k;
        for (k = 0; k < count && strcmp(Json_Name(member), known[k]); k++) {}
        if (k == count) Mods_Note(mod, "%s: unknown key \"%s\"", where, Json_Name(member));
    }
}

/* A reference (a duelist, a card) as written: a name, an identity, or an
 * id written as a number. "" when it is neither. */
static void reference(const JsonValue *value, char *out, size_t size)
{
    if (Json_TypeOf(value) == JSON_NUMBER) snprintf(out, size, "%ld", Json_Number(value, 0));
    else snprintf(out, size, "%s", Json_String(value, ""));
}

static int whole(const JsonValue *value, long low, long high, long *out)
{
    long number;
    if (Json_TypeOf(value) != JSON_NUMBER) return 0;
    number = Json_Number(value, low - 1);
    if (number < low || number > high) return 0;
    *out = number;
    return 1;
}

static int same_word(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    }
    return !*a && !*b;
}

/* A duelist named in the tower: its id, or 0 noted. */
static int duelist_named(const char *mod, const char *where, const JsonValue *value)
{
    char text[TOWER_REF];
    int id;
    reference(value, text, sizeof(text));
    id = text[0] ? Duelists_Named(text) : -1;
    if (id <= 0 || !Duelists_Valid(id)) {
        Mods_Note(mod, "%s: no duelist \"%s\"", where, text);
        return 0;
    }
    return id;
}

static int card_named(const char *mod, const char *where, const JsonValue *value)
{
    char text[TOWER_REF];
    int id;
    reference(value, text, sizeof(text));
    id = text[0] ? Cards_Named(text) : -1;
    if (id <= 0 || !Cards_Valid(id)) {
        Mods_Note(mod, "%s: no card \"%s\"", where, text);
        return 0;
    }
    return id;
}

static int read_flag(const char *mod, const char *where, const JsonValue *object, const char *key);

/* A PNG the tower names, relative to the mod's folder: its art's index,
 * -1 noted. The same file of the same kind is made once. */
static int art_named(const char *mod, const char *where, const char *file, int kind, int flip)
{
    char path[TOWER_ART_PATH];
    int i;
    if (!reading_directory) {
        Mods_Note(mod, "%s: %s: the mod has no folder to find it in", where, file);
        return -1;
    }
    if (!Paths_Contained(file)) {
        Mods_Note(mod, "%s: %s is not a path inside the mod", where, file);
        return -1;
    }
    if (snprintf(path, sizeof(path), "%s/%s", reading_directory, file) >= (int)sizeof(path)) {
        Mods_Note(mod, "%s: the path of %s is too long", where, file);
        return -1;
    }
    for (i = 0; i < config.art_count; i++) {
        if (config.arts[i].kind == kind && config.arts[i].flip == flip && !strcmp(config.arts[i].path, path)) return i;
    }
    if (config.art_count == TOWER_ARTS) {
        Mods_Note(mod, "%s: a tower has at most %d pictures of its own; %s is left out", where, TOWER_ARTS, file);
        return -1;
    }
    memset(&config.arts[config.art_count], 0, sizeof(config.arts[0]));
    config.arts[config.art_count].kind = kind;
    config.arts[config.art_count].flip = flip;
    snprintf(config.arts[config.art_count].path, sizeof(config.arts[0].path), "%s", path);
    return config.art_count++;
}

static int is_png(const char *text)
{
    size_t length = text ? strlen(text) : 0;
    return length > 4 && same_word(text + length - 4, ".png");
}

/* "scene": a name of mode 0's, the game's id in decimal digits (22, 139,
 * 205), "none" (black), or a PNG of the mod's. TOWER_SCENE_KEEP, noted,
 * when it is none of those. */
static int read_scene(const char *mod, const char *where, const JsonValue *value)
{
    const char *word = Json_String(value, NULL);
    int id;
    if (Json_TypeOf(value) == JSON_NUMBER) {
        long number = Json_Number(value, -1);
        id = number >= 0 && number <= 299 ? (int)((number / 100) << 8 | bcd((int)(number % 100))) : -1;
        if (id >= 0 && Tower_RetailScene(id)) return id;
        Mods_Note(mod, "%s: \"scene\" %ld is none of the disc's (0-41, 100-141, 200-212)", where, number);
        return TOWER_SCENE_KEEP;
    }
    if (word && is_png(word)) {
        int art = art_named(mod, where, word, TOWER_ART_SCENE, 0);
        if (art < 0) return TOWER_SCENE_KEEP;
        if (art >= TOWER_SCENE_ARTS) {
            Mods_Note(mod, "%s: a tower has at most %d scenes of its own; %s is left out", where,
                      TOWER_SCENE_ARTS, word);
            return TOWER_SCENE_KEEP;
        }
        return TOWER_SCENE_ART + art;
    }
    if (word && (id = Tower_SceneNamed(word)) >= 0) return id;
    if (word && same_word(word, "none")) return 0x000;
    Mods_Note(mod, "%s: \"scene\" is a scene's name (\"torch corridor\"), its number (22, 139) or a PNG", where);
    return TOWER_SCENE_KEEP;
}

/* A side's expression, its duelist known (`opponent`: as the floor's).
 *
 * By name: one its bust lacks is the bust's default, by that name, so the
 * bust is still what is shown. By number: 0 to 3 of a disc duelist whose
 * bust only adds expressions (no "replace_bust") are the campaign
 * character's, as for any duelist without a bust; a number past 3 can only
 * be a bust's, so it is that expression of the bust, by its name; one past
 * the bust's count is its default. A name, or a number past 3, for a
 * duelist without a bust is left out, and the side is the campaign's
 * character as it first comes in (expression 0), or nothing when the
 * duelist has none. Each is noted as what is shown. */
static void bust_expression(const char *mod, const char *where, TowerCast *cast, int duelist, int opponent)
{
    int bust = Duelists_Bust(duelist), character,
        own = duelist >= DUELISTS_RETAIL_COUNT || Duelists_ReplacesBust(duelist);
    const char *name = Duelists_Name(duelist) ? Duelists_Name(duelist) : "the duelist";
    char who[96], whose[96], asked[64];
    if (!cast->expression_name[0] && cast->expression <= 3 && (bust < 0 || !own)) return;
    snprintf(who, sizeof(who), opponent ? "the opponent, %s," : "%s", name);
    snprintf(whose, sizeof(whose), opponent ? "the opponent's (%s's) bust" : "%s's bust", name);
    if (bust >= 0) {
        const char *fallback = Busts_Name(bust, Busts_Default(bust));
        if (!fallback) return;
        if (cast->expression_name[0]) {
            if (Busts_Named(bust, cast->expression_name) >= 0) return;
            Mods_Note(mod, "%s: %s has no expression \"%s\"; its default, \"%s\", is shown instead", where, whose,
                      cast->expression_name, fallback);
            snprintf(cast->expression_name, sizeof(cast->expression_name), "%s", fallback);
            return;
        }
        if (cast->expression >= Busts_Count(bust)) {
            Mods_Note(mod, "%s: %s has %d expression%s (0-%d); for %d its default, \"%s\", is shown instead", where,
                      whose, Busts_Count(bust), Busts_Count(bust) == 1 ? "" : "s", Busts_Count(bust) - 1,
                      cast->expression, fallback);
            cast->expression = 0;
            if (!own) snprintf(cast->expression_name, sizeof(cast->expression_name), "%s", fallback);
        } else if (!own) {
            const char *named = Busts_Name(bust, Busts_Ordered(bust, cast->expression));
            snprintf(cast->expression_name, sizeof(cast->expression_name), "%s", named ? named : fallback);
            cast->expression = 0;
        }
        return;
    }
    if (cast->expression_name[0]) snprintf(asked, sizeof(asked), "\"expression\" \"%s\"", cast->expression_name);
    else snprintf(asked, sizeof(asked), "\"expression\" %d (past the campaign's 0 to 3)", cast->expression);
    character = Tower_DuelistCharacter(duelist);
    if (character)
        Mods_Note(mod, "%s: %s has no bust of its own (its duelists entry's \"bust\"); %s is left out and the "
                  "campaign's character %d is shown as it first comes in (expression 0)", where, who, asked,
                  character);
    else
        Mods_Note(mod, "%s: %s has no bust of its own (its duelists entry's \"bust\") and no character in the "
                  "campaign; %s is left out%s", where, who, asked, opponent ? " and the side stays empty" : "");
    cast->expression_name[0] = 0;
    cast->expression = 0;
}

/* One side's character: "none", a character of the campaign's by its
 * number (1-64), a duelist ("Seto"; "opponent", the floor's), a PNG of the
 * mod's, or an object of one of those ("character", "duelist", "image")
 * with "expression" (0-3; a duelist's bust's by name or 0-15) and, for a
 * PNG, "flip". */
static TowerCast read_cast(const char *mod, const char *where, const JsonValue *value)
{
    static const char *const known[] = {"character", "duelist", "image", "expression", "flip"};
    TowerCast cast;
    const JsonValue *what = value;
    const char *word;
    long number;
    int flip = 0;
    memset(&cast, 0, sizeof(cast));
    cast.kind = TOWER_CAST_NONE;
    if (Json_TypeOf(value) == JSON_OBJECT) {
        check_keys(mod, where, value, known, sizeof(known) / sizeof(known[0]));
        what = Json_Member(value, "character");
        if (!what) what = Json_Member(value, "duelist");
        if (!what) what = Json_Member(value, "image");
        if (Json_Member(value, "character") && (Json_Member(value, "duelist") || Json_Member(value, "image")))
            Mods_Note(mod, "%s: a side is a \"character\", or a \"duelist\" and its \"image\"", where);
        if (Json_Member(value, "expression")) {
            /* A campaign character's is 0 to 3; a duelist's may be its
             * bust's, by name or by number (0-15), sorted out once the
             * duelist is known (bust_expression). */
            const JsonValue *expression = Json_Member(value, "expression");
            const char *named = Json_String(expression, NULL);
            int by_duelist = Json_Member(value, "duelist") && !Json_Member(value, "character");
            if (named && !by_duelist)
                Mods_Note(mod, "%s: \"expression\" by name is a duelist's bust's (\"duelist\" and \"expression\"); "
                          "a campaign character's is 0 to 3", where);
            else if (named && !named[0])
                Mods_Note(mod, "%s: \"expression\" \"\" names no expression; left out", where);
            else if (named && strlen(named) >= TOWER_EXPRESSION_NAME)
                Mods_Note(mod, "%s: \"expression\" \"%.24s...\" is longer than a bust's expression's name can be "
                          "(%d bytes at most); left out", where, named, TOWER_EXPRESSION_NAME - 1);
            else if (named)
                snprintf(cast.expression_name, sizeof(cast.expression_name), "%s", named);
            else if (whole(expression, 0, by_duelist ? BUST_EXPRESSIONS - 1 : 3, &number))
                cast.expression = (int)number;
            else if (by_duelist)
                Mods_Note(mod, "%s: \"expression\" is the name of one of the duelist's bust's, or a number: 0 to 3 "
                          "(the campaign character's, or the bust's), 4 to %d (the bust's)", where,
                          BUST_EXPRESSIONS - 1);
            else
                Mods_Note(mod, "%s: \"expression\" is 0 to 3", where);
        }
        flip = read_flag(mod, where, value, "flip");
        if (Json_Member(value, "character") && Json_TypeOf(what) != JSON_NUMBER) {
            Mods_Note(mod, "%s: \"character\" is one of the campaign's, 1 to 64", where);
            return cast;
        }
        if (Json_Member(value, "image") && !Json_Member(value, "duelist") && !is_png(Json_String(what, NULL))) {
            Mods_Note(mod, "%s: \"image\" is a PNG of the mod's", where);
            return cast;
        }
    }
    if (Json_TypeOf(value) == JSON_OBJECT && Json_Member(value, "image") && what == Json_Member(value, "duelist")) {
        /* An image whose duelist is empty or "none" is the image alone. */
        const char *named = Json_String(what, NULL);
        if (Json_TypeOf(what) == JSON_NULL || (named && (same_word(named, "none") || !named[0])))
            what = Json_Member(value, "image");
    }
    word = Json_String(what, NULL);
    if (!what || Json_TypeOf(what) == JSON_NULL || (word && (same_word(word, "none") || !word[0]))) return cast;
    if (Json_TypeOf(value) == JSON_OBJECT && Json_Member(value, "image") && what == Json_Member(value, "duelist")) {
        /* {"image": ..., "duelist": ...}: the mod's picture of that duelist,
         * whose lines it then speaks. */
        const char *file = Json_String(Json_Member(value, "image"), NULL);
        int art = file && is_png(file) ? art_named(mod, where, file, TOWER_ART_CHARACTER, flip) : -1;
        if (art < 0) {
            if (!file || !is_png(file)) Mods_Note(mod, "%s: \"image\" is a PNG of the mod's", where);
            return cast;
        }
        cast.kind = TOWER_CAST_ART;
        cast.id = art;
        cast.duelist = word && same_word(word, "opponent") ? TOWER_SPEAKER_FLOOR : duelist_named(mod, where, what);
        if (cast.expression > 3) {
            Mods_Note(mod, "%s: \"expression\" %d is a bust's; with an \"image\" it is 0 to 3, and 0 is shown", where,
                      cast.expression);
            cast.expression = 0;
        }
        if (cast.expression_name[0]) {
            Mods_Note(mod, "%s: \"expression\" by name is the duelist's bust's; with an \"image\" the image is shown",
                      where);
            cast.expression_name[0] = 0;
        }
        return cast;
    }
    if (Json_TypeOf(what) == JSON_NUMBER && !(Json_TypeOf(value) == JSON_OBJECT && what == Json_Member(value, "duelist"))) {
        if (!whole(what, 1, TOWER_CHARACTERS, &number) || Tower_EmptyCharacter((int)number)) {
            Mods_Note(mod, "%s: character %ld is none of the campaign's (1-64; 5, 11, 39, 40 and 41 are empty)",
                      where, Json_Number(what, 0));
            return cast;
        }
        cast.kind = TOWER_CAST_RETAIL;
        cast.id = (int)number;
        return cast;
    }
    if (word && is_png(word)) {
        int art = art_named(mod, where, word, TOWER_ART_CHARACTER, flip);
        if (art >= 0) {
            cast.kind = TOWER_CAST_ART;
            cast.id = art;
        }
        return cast;
    }
    if (flip) Mods_Note(mod, "%s: \"flip\" turns a PNG of the mod's, not a character of the campaign's", where);
    if (word && same_word(word, "opponent")) {
        cast.kind = TOWER_CAST_OPPONENT;
        cast.duelist = TOWER_SPEAKER_FLOOR;
        return cast;
    }
    cast.duelist = duelist_named(mod, where, what);
    if (!cast.duelist) return cast;
    {   /* A duelist with a bust of its own (busts.h): its expressions, by
         * name or by the codes' number. A disc duelist whose bust only
         * adds expressions is still the campaign's character unless a
         * name of its bust's asks for one of them. */
        int bust;
        bust_expression(mod, where, &cast, cast.duelist, 0);
        bust = Duelists_Bust(cast.duelist);
        if (bust >= 0 && (cast.expression_name[0] || cast.duelist >= DUELISTS_RETAIL_COUNT ||
                          Duelists_ReplacesBust(cast.duelist))) {
            cast.kind = TOWER_CAST_DUELIST;
            return cast;
        }
    }
    cast.id = Tower_DuelistCharacter(cast.duelist);
    if (!cast.id) {
        const char *name = Duelists_Name(cast.duelist);
        Mods_Note(mod, "%s: %s has no character in the campaign; give the side a PNG", where,
                  name ? name : "the duelist");
        cast.duelist = 0;
        return cast;
    }
    cast.kind = TOWER_CAST_RETAIL;
    return cast;
}

/* "characters": {"left": ..., "right": ...}; a side it does not name stays
 * as it was. */
static void read_characters(const char *mod, const char *where, const JsonValue *value,
                            TowerCast cast[TOWER_SIDES])
{
    static const char *const known[] = {"left", "right"};
    static const char *const sides[TOWER_SIDES] = {"left", "right"};
    char at[200];
    int side;
    if (Json_TypeOf(value) != JSON_OBJECT) {
        Mods_Note(mod, "%s: \"characters\" is {\"left\": ..., \"right\": ...}", where);
        return;
    }
    check_keys(mod, where, value, known, sizeof(known) / sizeof(known[0]));
    for (side = 0; side < TOWER_SIDES; side++) {
        const JsonValue *one = Json_Member(value, sides[side]);
        if (!one) continue;
        snprintf(at, sizeof(at), "%s %s", where, sides[side]);
        cast[side] = read_cast(mod, at, one);
    }
}

/* "pan": {"x": ..., "y": ..., "frames": ..., "wait": ...}: the scene's
 * view moved there over that many frames (F7 0B, the campaign's
 * Script_UpdateViewportTween); with "wait" the words wait for it. */
static void read_pan(const char *mod, const char *where, const JsonValue *value, TowerLine *line)
{
    static const char *const known[] = {"x", "y", "frames", "wait"};
    long number;
    if (Json_TypeOf(value) != JSON_OBJECT) {
        Mods_Note(mod, "%s: \"pan\" is {\"x\": ..., \"y\": ..., \"frames\": ...}", where);
        return;
    }
    check_keys(mod, where, value, known, sizeof(known) / sizeof(known[0]));
    line->pan = 1;
    line->pan_x = line->pan_y = 0;
    line->pan_frames = 60;
    if (Json_Member(value, "x")) {
        if (whole(Json_Member(value, "x"), 0, 576, &number)) line->pan_x = (int)number;
        else Mods_Note(mod, "%s: pan \"x\" is 0 to 576", where);
    }
    if (Json_Member(value, "y")) {
        if (whole(Json_Member(value, "y"), 0, 96, &number)) line->pan_y = (int)number;
        else Mods_Note(mod, "%s: pan \"y\" is 0 to 96", where);
    }
    if (Json_Member(value, "frames")) {
        if (whole(Json_Member(value, "frames"), 1, 3600, &number)) line->pan_frames = (int)number;
        else Mods_Note(mod, "%s: pan \"frames\" is 1 to 3600", where);
    }
    line->pan_wait = read_flag(mod, where, value, "wait");
}

static int add_line(const TowerLine *staged, const char *text)
{
    TowerLine *line;
    if (config.line_count == config.line_room) {
        int room = config.line_room ? config.line_room * 2 : 32;
        TowerLine *grown = realloc(config.lines, (size_t)room * sizeof(*grown));
        if (!grown) return 0;
        config.lines = grown;
        config.line_room = room;
    }
    line = &config.lines[config.line_count];
    *line = *staged;
    line->text = malloc(strlen(text) + 1);
    if (!line->text) return 0;
    strcpy(line->text, text);
    config.line_count++;
    return 1;
}

/* A line that changes nothing but its words. */
static TowerLine plain_line(int speaker)
{
    TowerLine line;
    memset(&line, 0, sizeof(line));
    line.speaker = speaker;
    line.portrait = TOWER_PORTRAIT_AUTO;
    line.scene = TOWER_SCENE_KEEP;
    return line;
}

/* "speaker": "left", "right", "none", "opponent" or a duelist. */
static int read_speaker(const char *mod, const char *where, const JsonValue *who)
{
    const char *word = Json_String(who, NULL);
    if (Json_TypeOf(who) == JSON_NULL || (word && (same_word(word, "none") || !word[0]))) return TOWER_SPEAKER_NONE;
    if (word && same_word(word, "opponent")) return TOWER_SPEAKER_FLOOR;
    if (word && same_word(word, "left")) return TOWER_SPEAKER_LEFT;
    if (word && same_word(word, "right")) return TOWER_SPEAKER_RIGHT;
    return duelist_named(mod, where, who);
}

/* One line: its words, alone or with who says them and what the stage does
 * from it on. */
static void read_line(const char *mod, const char *where, const JsonValue *value, const TowerLine *defaults)
{
    static const char *const known[] = {"text", "speaker", "portrait", "scene", "characters", "pan"};
    const char *text = Json_String(value, NULL);
    TowerLine line = *defaults;
    if (!text && Json_TypeOf(value) == JSON_OBJECT) {
        const JsonValue *who = Json_Member(value, "speaker"), *face = Json_Member(value, "portrait"), *part;
        check_keys(mod, where, value, known, sizeof(known) / sizeof(known[0]));
        text = Json_String(Json_Member(value, "text"), NULL);
        if (who) line.speaker = read_speaker(mod, where, who);
        if (face) {
            long number;
            const char *word = Json_String(face, NULL);
            if (whole(face, 0, TOWER_CAMPAIGN_PORTRAITS - 1, &number)) line.portrait = (int)number;
            else if ((word && same_word(word, "speaker")) || (Json_TypeOf(face) == JSON_BOOL && Json_Bool(face, 0)))
                line.portrait = TOWER_PORTRAIT_SPEAKER;
            else if ((word && same_word(word, "none")) || Json_TypeOf(face) == JSON_NULL ||
                     Json_TypeOf(face) == JSON_BOOL)
                line.portrait = -1;
            else
                Mods_Note(mod, "%s: \"portrait\" is one of the campaign's portraits (0 to %d), \"speaker\" or \"none\"",
                          where, TOWER_CAMPAIGN_PORTRAITS - 1);
        }
        if ((part = Json_Member(value, "scene"))) line.scene = read_scene(mod, where, part);
        if ((part = Json_Member(value, "characters"))) read_characters(mod, where, part, line.cast);
        if ((part = Json_Member(value, "pan"))) read_pan(mod, where, part, &line);
    }
    if (!text) {
        Mods_Note(mod, "%s: a line is text, or {\"text\": ..., \"speaker\": ...}", where);
        return;
    }
    if (strlen(text) >= TOWER_LINE_TEXT) {
        char cut[TOWER_LINE_TEXT];
        Mods_Note(mod, "%s: a line is at most %d bytes; the rest is left out", where, TOWER_LINE_TEXT - 1);
        snprintf(cut, sizeof(cut), "%s", text);
        add_line(&line, cut);
        return;
    }
    if (!text[0]) return;
    add_line(&line, text);
}

/* A story: a line, a list of them, or {"scene": ..., "characters": ...,
 * "lines": [...]}, whose stage is its first line's. `speaker` says the
 * lines that name none. */
static TowerStory read_story(const char *mod, const char *where, const JsonValue *value, int speaker)
{
    static const char *const known[] = {"scene", "characters", "lines"};
    TowerStory story;
    TowerLine first = plain_line(speaker), rest = plain_line(speaker);
    const JsonValue *lines = value;
    size_t letters = 0;
    char at[160];
    int i;
    story.first = config.line_count;
    story.count = 0;
    if (!value) return story;
    if (Json_TypeOf(value) == JSON_OBJECT && Json_Member(value, "lines")) {
        const JsonValue *part;
        check_keys(mod, where, value, known, sizeof(known) / sizeof(known[0]));
        if ((part = Json_Member(value, "scene"))) first.scene = read_scene(mod, where, part);
        if ((part = Json_Member(value, "characters"))) read_characters(mod, where, part, first.cast);
        lines = Json_Member(value, "lines");
    }
    if (Json_TypeOf(lines) == JSON_ARRAY) {
        const JsonValue *item;
        int index = 0;
        for (item = Json_At(lines, 0); item; item = Json_Next(item), index++) {
            snprintf(at, sizeof(at), "%s[%d]", where, index);
            read_line(mod, at, item, config.line_count == story.first ? &first : &rest);
        }
    } else {
        read_line(mod, where, lines, &first);
    }
    story.count = config.line_count - story.first;
    /* One box tells a story (tower_screen.c): its words in the game's codes
     * fit TOWER_STORY_TEXT. */
    for (i = 0; i < story.count; i++) {
        letters += strlen(config.lines[story.first + i].text) + TOWER_LINE_CODES;
        if (letters > TOWER_STORY_TEXT) {
            Mods_Note(mod, "%s: a story is at most about %d letters; lines %d on are left out", where,
                      TOWER_STORY_TEXT, i);
            for (; story.first + i < config.line_count; config.line_count--) free(config.lines[config.line_count - 1].text);
            story.count = i;
            break;
        }
    }
    return story;
}

/* A floor's story naming "opponent" with an expression of its bust's by
 * name: the floor's opponent is known now, so the name is checked here. */
static void opponent_expressions(const char *mod, const char *where, TowerStory story, int duelist)
{
    int i, s;
    for (i = 0; i < story.count; i++) {
        for (s = 0; s < TOWER_SIDES; s++) {
            TowerCast *cast = &config.lines[story.first + i].cast[s];
            if (cast->kind == TOWER_CAST_OPPONENT) bust_expression(mod, where, cast, duelist, 1);
        }
    }
}

/* "deck": {card: copies} as a fixed deck is written ("decks", tables.h), or
 * a list of the forty. Exactly forty, else the floor deals the opponent's
 * own. */
static void read_deck(const char *mod, const char *where, const JsonValue *value, TowerFloor *floor)
{
    unsigned short cards[TOWER_DECK];
    int count = 0, broken = 0;
    char at[200];
    if (Json_TypeOf(value) == JSON_ARRAY) {
        const JsonValue *item;
        for (item = Json_At(value, 0); item; item = Json_Next(item)) {
            int id = card_named(mod, where, item);
            if (!id) { broken = 1; continue; }
            if (count < TOWER_DECK) cards[count] = (unsigned short)id;
            count++;
        }
    } else if (Json_TypeOf(value) == JSON_OBJECT) {
        const JsonValue *member;
        for (member = Json_At(value, 0); member; member = Json_Next(member)) {
            long copies;
            int id = Cards_Named(Json_Name(member)), i;
            snprintf(at, sizeof(at), "%s \"%s\"", where, Json_Name(member));
            if (id <= 0 || !Cards_Valid(id)) {
                Mods_Note(mod, "%s: no such card", at);
                broken = 1;
                continue;
            }
            if (!whole(member, 0, TOWER_DECK, &copies)) {
                Mods_Note(mod, "%s: a deck gives each card its copies, 0 to %d", at, TOWER_DECK);
                broken = 1;
                continue;
            }
            for (i = 0; i < copies; i++, count++) {
                if (count < TOWER_DECK) cards[count] = (unsigned short)id;
            }
        }
    } else {
        Mods_Note(mod, "%s: a deck is {card: copies} or a list of forty cards; the opponent's own is dealt", where);
        return;
    }
    if (broken) {
        Mods_Note(mod, "%s: the opponent's own deck is dealt instead", where);
        return;
    }
    if (count != TOWER_DECK) {
        Mods_Note(mod, "%s: a deck is %d cards, and this one has %d; the opponent's own is dealt", where, TOWER_DECK,
                  count);
        return;
    }
    memcpy(floor->deck, cards, sizeof(cards));
    floor->deck_given = 1;
}

/* "life_points": the same for both, or {"player": ..., "opponent": ...}. */
static void read_life_points(const char *mod, const char *where, const JsonValue *value, TowerFloor *floor)
{
    static const char *const sides[] = {"player", "opponent"};
    long number;
    int side;
    if (Json_TypeOf(value) == JSON_NUMBER) {
        if (!whole(value, 1, 65535, &number)) {
            Mods_Note(mod, "%s: \"life_points\" is 1 to 65535", where);
            return;
        }
        floor->life_points[0] = floor->life_points[1] = (int)number;
        return;
    }
    if (Json_TypeOf(value) != JSON_OBJECT) {
        Mods_Note(mod, "%s: \"life_points\" is a number, or {\"player\": ..., \"opponent\": ...}", where);
        return;
    }
    check_keys(mod, where, value, sides, 2);
    for (side = 0; side < 2; side++) {
        const JsonValue *part = Json_Member(value, sides[side]);
        if (!part) continue;
        if (whole(part, 1, 65535, &number)) floor->life_points[side] = (int)number;
        else Mods_Note(mod, "%s: \"life_points\" \"%s\" is 1 to 65535", where, sides[side]);
    }
}

/* One card, or a list of them. */
static void read_reward_cards(const char *mod, const char *where, const JsonValue *value, TowerFloor *floor)
{
    const JsonValue *item = Json_TypeOf(value) == JSON_ARRAY ? Json_At(value, 0) : value;
    for (; item; item = Json_TypeOf(value) == JSON_ARRAY ? Json_Next(item) : NULL) {
        int id = card_named(mod, where, item);
        if (!id) continue;
        if (floor->reward_card_count >= TOWER_REWARD_CARDS) {
            Mods_Note(mod, "%s: at most %d cards; the rest are left out", where, TOWER_REWARD_CARDS);
            return;
        }
        floor->reward_cards[floor->reward_card_count++] = id;
    }
}

static void read_reward(const char *mod, const char *where, const JsonValue *value, TowerFloor *floor)
{
    static const char *const known[] = {"card", "cards", "starchips", "unlock_duelist"};
    const JsonValue *part;
    char at[160];
    if (Json_TypeOf(value) != JSON_OBJECT) {
        Mods_Note(mod, "%s: \"reward\" is an object ({\"cards\": [...], \"starchips\": 100, \"unlock_duelist\": ...})",
                  where);
        return;
    }
    check_keys(mod, where, value, known, sizeof(known) / sizeof(known[0]));
    snprintf(at, sizeof(at), "%s reward", where);
    if ((part = Json_Member(value, "card"))) read_reward_cards(mod, at, part, floor);
    if ((part = Json_Member(value, "cards"))) read_reward_cards(mod, at, part, floor);
    if ((part = Json_Member(value, "starchips"))) {
        long number;
        if (whole(part, 0, 999999, &number)) floor->reward_starchips = (unsigned)number;
        else Mods_Note(mod, "%s: \"starchips\" is 0 to 999999", at);
    }
    if ((part = Json_Member(value, "unlock_duelist"))) {
        const JsonValue *item = Json_TypeOf(part) == JSON_ARRAY ? Json_At(part, 0) : part;
        for (; item; item = Json_TypeOf(part) == JSON_ARRAY ? Json_Next(item) : NULL) {
            int id = duelist_named(mod, at, item);
            if (!id) continue;
            if (floor->reward_duelist_count >= TOWER_REWARD_DUELISTS) {
                Mods_Note(mod, "%s: at most %d duelists to unlock", at, TOWER_REWARD_DUELISTS);
                break;
            }
            floor->reward_duelists[floor->reward_duelist_count++] = id;
        }
    }
}

static int read_flag(const char *mod, const char *where, const JsonValue *object, const char *key)
{
    const JsonValue *value = Json_Member(object, key);
    if (!value) return 0;
    if (Json_TypeOf(value) != JSON_BOOL) {
        Mods_Note(mod, "%s: \"%s\" is true or false, without quotes", where, key);
        return 0;
    }
    return Json_Bool(value, 0);
}

/* A flag of the rules of the run or a floor's unlock: written wrong, noted,
 * and its default kept (rather than read as false, which would turn a
 * protection on or hide a duelist the mod meant to show). */
static int read_option(const char *mod, const char *where, const JsonValue *object, const char *key, int fallback)
{
    const JsonValue *value = Json_Member(object, key);
    if (!value) return fallback;
    if (Json_TypeOf(value) != JSON_BOOL) {
        Mods_Note(mod, "%s: \"%s\" is true or false, without quotes; left as if not written", where, key);
        return fallback;
    }
    return Json_Bool(value, 0);
}

/* One floor; 0 when it cannot be fought (no opponent), which leaves it out. */
static int read_floor(const char *mod, int index, const JsonValue *value, TowerFloor *floor)
{
    static const char *const known[] = {"duelist", "field", "deck", "life_points", "no_fusions", "no_equips",
                                        "reward", "checkpoint", "unlock_opponent", "before", "after"};
    char where[64], at[96];
    const JsonValue *part;
    memset(floor, 0, sizeof(*floor));
    snprintf(where, sizeof(where), "tower floors[%d]", index);
    if (Json_TypeOf(value) != JSON_OBJECT) {
        Mods_Note(mod, "%s: a floor is an object ({\"duelist\": \"Seto\", ...}); left out", where);
        return 0;
    }
    check_keys(mod, where, value, known, sizeof(known) / sizeof(known[0]));
    if (!(part = Json_Member(value, "duelist"))) {
        Mods_Note(mod, "%s: a floor needs a \"duelist\"; left out", where);
        return 0;
    }
    floor->duelist = duelist_named(mod, where, part);
    if (!floor->duelist) {
        Mods_Note(mod, "%s: left out", where);
        return 0;
    }
    if ((part = Json_Member(value, "field"))) {
        char text[TOWER_REF];
        reference(part, text, sizeof(text));
        if (same_word(text, "none") || !strcmp(text, "0")) floor->terrain = 0;
        else if ((floor->terrain = Tables_TerrainNamed(text)) < 0) {
            Mods_Note(mod, "%s: \"field\" is none, Forest, Wasteland, Mountain, Meadow, Sea, Dark or a mod's field", where);
            floor->terrain = 0;
        }
    }
    if ((part = Json_Member(value, "deck"))) {
        snprintf(at, sizeof(at), "%s deck", where);
        read_deck(mod, at, part, floor);
    }
    if ((part = Json_Member(value, "life_points"))) read_life_points(mod, where, part, floor);
    if (read_flag(mod, where, value, "no_fusions")) floor->rules |= TOWER_RULE_NO_FUSIONS;
    if (read_flag(mod, where, value, "no_equips")) floor->rules |= TOWER_RULE_NO_EQUIPS;
    floor->checkpoint = read_flag(mod, where, value, "checkpoint");
    floor->unlock_opponent = read_option(mod, where, value, "unlock_opponent", -1);
    if ((part = Json_Member(value, "reward"))) read_reward(mod, where, part, floor);
    snprintf(at, sizeof(at), "%s before", where);
    floor->before = read_story(mod, at, Json_Member(value, "before"), TOWER_SPEAKER_FLOOR);
    snprintf(at, sizeof(at), "%s after", where);
    floor->after = read_story(mod, at, Json_Member(value, "after"), TOWER_SPEAKER_FLOOR);
    snprintf(at, sizeof(at), "%s before", where);
    opponent_expressions(mod, at, floor->before, floor->duelist);
    snprintf(at, sizeof(at), "%s after", where);
    opponent_expressions(mod, at, floor->after, floor->duelist);
    return 1;
}

static void read_unlock(const char *mod, const JsonValue *value)
{
    static const char *const known[] = {"after_ending", "beat", "wins", "story", "card", "copies"};
    TowerUnlock *unlock = &config.unlock;
    const char *word = Json_String(value, NULL);
    long number;
    memset(unlock, 0, sizeof(*unlock));
    unlock->story = -1;
    if (word) {
        if (same_word(word, "always")) return;
        if (same_word(word, "after_ending")) {
            unlock->kind = TOWER_UNLOCK_CONDITIONS;
            unlock->after_ending = 1;
            return;
        }
        Mods_Note(mod, "tower: \"unlock\" is \"always\", \"after_ending\" or an object of conditions; "
                       "the tower stays locked");
        unlock->kind = TOWER_UNLOCK_NEVER;
        return;
    }
    if (Json_TypeOf(value) != JSON_OBJECT) {
        Mods_Note(mod, "tower: \"unlock\" is \"always\", \"after_ending\" or an object of conditions; "
                       "the tower stays locked");
        unlock->kind = TOWER_UNLOCK_NEVER;
        return;
    }
    check_keys(mod, "tower unlock", value, known, sizeof(known) / sizeof(known[0]));
    unlock->kind = TOWER_UNLOCK_CONDITIONS;
    unlock->after_ending = read_flag(mod, "tower unlock", value, "after_ending");
    if (Json_Member(value, "after_ending") && Json_TypeOf(Json_Member(value, "after_ending")) != JSON_BOOL) {
        Mods_Note(mod, "tower unlock: the tower stays locked");   /* "yes" is no condition to open it on */
        unlock->kind = TOWER_UNLOCK_NEVER;
    }
    if (Json_Member(value, "beat")) reference(Json_Member(value, "beat"), unlock->beat, sizeof(unlock->beat));
    if (Json_Member(value, "card")) reference(Json_Member(value, "card"), unlock->card, sizeof(unlock->card));
    if (Json_Member(value, "wins")) {
        if (whole(Json_Member(value, "wins"), 0, 65535, &number)) unlock->wins = (int)number;
        else { Mods_Note(mod, "tower unlock: \"wins\" is 0 to 65535; the tower stays locked"); unlock->kind = TOWER_UNLOCK_NEVER; }
    }
    if (Json_Member(value, "copies")) {
        if (whole(Json_Member(value, "copies"), 0, 250, &number)) unlock->copies = (int)number;
        else { Mods_Note(mod, "tower unlock: \"copies\" is 0 to 250; the tower stays locked"); unlock->kind = TOWER_UNLOCK_NEVER; }
    }
    if (Json_Member(value, "story")) {
        if (whole(Json_Member(value, "story"), 0, 0x7FF, &number)) unlock->story = (int)number;
        else { Mods_Note(mod, "tower unlock: \"story\" is a flag, 0 to 2047; the tower stays locked"); unlock->kind = TOWER_UNLOCK_NEVER; }
    }
    if (unlock->beat[0] && Duelists_Named(unlock->beat) < 0)
        Mods_Note(mod, "tower unlock: no duelist \"%s\"; the tower stays locked until there is one", unlock->beat);
    if (unlock->card[0] && Cards_Named(unlock->card) <= 0)
        Mods_Note(mod, "tower unlock: no card \"%s\"; the tower stays locked until there is one", unlock->card);
    if (unlock->kind == TOWER_UNLOCK_CONDITIONS && !unlock->after_ending && !unlock->beat[0] && !unlock->card[0] &&
        !unlock->wins && unlock->story < 0)
        unlock->kind = TOWER_UNLOCK_ALWAYS;   /* {} asks for nothing */
}

void Tower_Read(const char *mod, const char *directory, const JsonValue *manifest)
{
    static const char *const known[] = {"name", "label", "music", "unlock", "on_lose", "on_leave", "drops",
                                        "save_states", "cheats", "scene", "dim", "intro", "ending", "lose",
                                        "floors"};
    static TowerConfig earlier;
    const JsonValue *tower = Json_Member(manifest, "tower"), *part, *floor;
    int index = 0, had_previous;
    if (!tower) return;
    if (Json_TypeOf(tower) != JSON_OBJECT) {
        Mods_Note(mod, "\"tower\" is an object (notes/tower-mode.md)");
        return;
    }
    part = Json_Member(tower, "floors");
    if (Json_TypeOf(part) != JSON_ARRAY || !Json_Count(part)) {
        Mods_Note(mod, "tower: \"floors\" is a list of at least one floor ({\"duelist\": \"Seto\"}); no tower");
        return;
    }
    /* An earlier mod's tower is kept aside until this one has a floor: a
     * tower with none leaves the earlier one in place. */
    had_previous = defined;
    if (had_previous) earlier = config;
    memset(&config, 0, sizeof(config));
    defined = 0;
    reading_directory = directory;
    check_keys(mod, "tower", tower, known, sizeof(known) / sizeof(known[0]));
    snprintf(config.mod, sizeof(config.mod), "%s", mod);
    snprintf(config.name, sizeof(config.name), "%s", Json_String(Json_Member(tower, "name"), mod));
    if (strlen(Json_String(Json_Member(tower, "name"), "")) >= sizeof(config.name))
        Mods_Note(mod, "tower: \"name\" is at most %d bytes", (int)sizeof(config.name) - 1);
    if ((part = Json_Member(tower, "label"))) {
        const char *label = Json_String(part, NULL);
        if (!label) Mods_Note(mod, "tower: \"label\" is text");
        else if (strlen(label) >= sizeof(config.label))
            Mods_Note(mod, "tower: \"label\" is longer than %d letters", (int)sizeof(config.label) - 1);
        snprintf(config.label, sizeof(config.label), "%s", label ? label : "");
    }
    config.music = -1;
    if ((part = Json_Member(tower, "music"))) {
        long song;
        if (whole(part, 0, 0xFFF, &song) || (Json_TypeOf(part) == JSON_STRING && (song = Json_Number(part, -1)) >= 0 &&
                                            song <= 0xFFF))
            config.music = (int)song;
        else
            Mods_Note(mod, "tower: \"music\" is a song id, 0x000 to 0xFFF");
    }
    config.unlock.story = -1;
    if ((part = Json_Member(tower, "unlock"))) read_unlock(mod, part);
    if ((part = Json_Member(tower, "on_lose"))) {
        const char *word = Json_String(part, "");
        if (same_word(word, "checkpoint")) config.on_lose = TOWER_ON_LOSE_CHECKPOINT;
        else if (same_word(word, "restart")) config.on_lose = TOWER_ON_LOSE_RESTART;
        else Mods_Note(mod, "tower: \"on_lose\" is \"checkpoint\" or \"restart\"; checkpoint it is");
    }
    if ((part = Json_Member(tower, "on_leave"))) {
        const char *word = Json_String(part, "");
        if (same_word(word, "resume")) config.on_leave = TOWER_ON_LEAVE_RESUME;
        else if (same_word(word, "checkpoint")) config.on_leave = TOWER_ON_LEAVE_CHECKPOINT;
        else if (same_word(word, "restart")) config.on_leave = TOWER_ON_LEAVE_RESTART;
        else Mods_Note(mod, "tower: \"on_leave\" is \"resume\", \"checkpoint\" or \"restart\"; resume it is");
    }
    config.drops = TOWER_DROPS_DEFAULT;
    if ((part = Json_Member(tower, "drops"))) {
        long number;
        const char *word = Json_String(part, NULL);
        if (word && same_word(word, "player")) config.drops = TOWER_DROPS_PLAYER;
        else if (whole(part, 1, TOWER_DROPS_MAX, &number)) config.drops = (int)number;
        else Mods_Note(mod, "tower: \"drops\" is 1 to %d cards a won duel, or \"player\" (Game > Card drops); "
                            "%d it is", TOWER_DROPS_MAX, TOWER_DROPS_DEFAULT);
    }
    config.save_states = read_option(mod, "tower", tower, "save_states", 1);
    config.cheats = read_option(mod, "tower", tower, "cheats", 1);
    config.scene = TOWER_SCENE_DEFAULT;
    if ((part = Json_Member(tower, "scene"))) {
        int scene = read_scene(mod, "tower", part);
        if (scene != TOWER_SCENE_KEEP) config.scene = scene;
    }
    config.dim = read_flag(mod, "tower", tower, "dim");
    config.intro = read_story(mod, "tower intro", Json_Member(tower, "intro"), TOWER_SPEAKER_NONE);
    config.ending = read_story(mod, "tower ending", Json_Member(tower, "ending"), TOWER_SPEAKER_NONE);
    config.lose = read_story(mod, "tower lose", Json_Member(tower, "lose"), TOWER_SPEAKER_NONE);
    for (floor = Json_At(Json_Member(tower, "floors"), 0); floor; floor = Json_Next(floor), index++) {
        if (config.floor_count >= TOWER_MAX_FLOORS) {
            Mods_Note(mod, "tower: at most %d floors; the rest are left out", TOWER_MAX_FLOORS);
            break;
        }
        if (read_floor(mod, index, floor, &config.floors[config.floor_count])) config.floor_count++;
    }
    reading_directory = NULL;
    if (!config.floor_count) {
        if (had_previous) Mods_Note(mod, "tower: no floor can be fought; %s's tower stays the one the menu offers",
                                    earlier.mod);
        else Mods_Note(mod, "tower: no floor can be fought; no tower");
        Tower_Reset();
        if (had_previous) {
            config = earlier;
            defined = 1;
        }
        return;
    }
    if (had_previous) {
        Mods_Note(earlier.mod, "tower: %s's tower, loaded after this mod, is the one the menu offers", mod);
        free_tower_lines(&earlier);
    }
    defined = 1;
}

void Tower_Build(void)
{
    int i;
    Tower_Reset();
    for (i = 0; i < Mods_LoadedCount(); i++) {
        int mod = Mods_Loaded(i);
        if (Mods_Active(mod)) Tower_Read(Mods_Id(mod), Mods_Directory(mod), Mods_Manifest(mod));
    }
    if (defined) Tower_MakeArt(&config);
}

/* --- progress ---------------------------------------------------------------- */

uint32_t Tower_Identity(const TowerConfig *tower)
{
    uint32_t hash = 2166136261u;
    const char *text;
    if (!tower) return 0;
    for (text = tower->mod; *text; text++) hash = (hash ^ (unsigned char)*text) * 16777619u;
    hash = (hash ^ '\n') * 16777619u;
    for (text = tower->name; *text; text++) hash = (hash ^ (unsigned char)*text) * 16777619u;
    return hash ? hash : 1;
}

int Tower_FloorCleared(const TowerProgress *progress, int floor)
{
    if (!progress || floor < 0 || floor >= TOWER_MAX_FLOORS) return 0;
    return (progress->cleared[floor >> 3] >> (floor & 7)) & 1;
}

void Tower_ClearProgress(TowerProgress *progress, uint32_t owner, uint32_t tower)
{
    memset(progress, 0, sizeof(*progress));
    progress->owner = owner;
    progress->tower = tower;
}

void Tower_Bank(const TowerConfig *tower, TowerProgress *progress)
{
    if (!tower || !progress) return;
    if (tower->on_leave == TOWER_ON_LEAVE_RESTART) {
        progress->floor = 0;
        progress->checkpoint = 0;
    } else if (tower->on_leave == TOWER_ON_LEAVE_CHECKPOINT && progress->floor > progress->checkpoint) {
        progress->floor = progress->checkpoint;
    }
}

TowerOutcome Tower_ApplyResult(const TowerConfig *tower, TowerProgress *progress, int floor, int won)
{
    TowerOutcome outcome;
    memset(&outcome, 0, sizeof(outcome));
    outcome.back_to = progress->floor;
    if (!tower || floor < 0 || floor >= tower->floor_count) return outcome;
    outcome.won = won;
    if (!won) {
        progress->floor = tower->on_lose == TOWER_ON_LOSE_RESTART ? 0 : progress->checkpoint;
        if (tower->on_lose == TOWER_ON_LOSE_RESTART) progress->checkpoint = 0;
        outcome.back_to = progress->floor;
        return outcome;
    }
    outcome.first_clear = !Tower_FloorCleared(progress, floor);
    progress->cleared[floor >> 3] |= (uint8_t)(1 << (floor & 7));
    if (floor + 1 >= tower->floor_count) {
        /* The top: told the ending, and the tower stands open again from
         * the first floor. */
        outcome.completed = 1;
        if (progress->completions < 0xFFFF) progress->completions++;
        progress->floor = 0;
        progress->checkpoint = 0;
        return outcome;
    }
    progress->floor = (uint16_t)(floor + 1);
    if (tower->floors[floor].checkpoint) {
        progress->checkpoint = (uint16_t)(floor + 1);
        outcome.checkpoint = 1;
    }
    outcome.back_to = progress->floor;
    return outcome;
}

/* --- the progress file ---------------------------------------------------------
 *
 *   tower <mod id>
 *   identity 1A2B3C4D
 *   owner 0012ABCD
 *   floor 1
 *   checkpoint 1
 *   completions 0
 *   flags 1
 *   cleared 0 3
 *   end
 *
 * one section per tower a save has played, the mod id first so a person can
 * read it. A section of a tower this run has not (its mod is off) is kept
 * as it was and written back. */

static void append(char **buffer, size_t *length, const char *line)
{
    size_t add = strlen(line);
    char *grown = realloc(*buffer, *length + add + 1);
    if (!grown) return;
    memcpy(grown + *length, line, add + 1);
    *buffer = grown;
    *length += add;
}

int Tower_ReadProgress(FILE *file, const char *mod, uint32_t identity, TowerProgress *progress, char **foreign)
{
    char line[512];
    int mine = 0, found = 0, inside = 0, stale = 0;
    size_t foreign_length = 0;
    *foreign = NULL;
    Tower_ClearProgress(progress, 0, identity);
    while (fgets(line, sizeof(line), file)) {
        char word[64], name[256];
        unsigned value;
        if (!inside) {
            if (sscanf(line, "tower %255s", name) == 1) {
                inside = 1;
                mine = !strcmp(name, mod);
                if (!mine) append(foreign, &foreign_length, line);
            }
            continue;
        }
        if (!mine && !stale) append(foreign, &foreign_length, line);
        if (sscanf(line, "%63s", word) != 1) continue;
        if (!strcmp(word, "end")) {
            inside = 0;
            mine = 0;
            stale = 0;
            continue;
        }
        if (!mine) continue;
        if (!strcmp(word, "identity") && sscanf(line, "identity %x", &value) == 1) {
            /* The same mod with another tower ("name" changed): the floors
             * may be others, so its progress begins again, and the rest of
             * the section is left out (written again as this tower's). A
             * section of this tower read before it stays read. */
            if (value != identity) {
                if (!found) Tower_ClearProgress(progress, 0, identity);
                mine = 0;
                stale = 1;
                continue;
            }
            found = 1;
        } else if (!strcmp(word, "owner") && sscanf(line, "owner %x", &value) == 1) {
            progress->owner = value;
        } else if (!strcmp(word, "floor") && sscanf(line, "floor %u", &value) == 1) {
            progress->floor = (uint16_t)(value < TOWER_MAX_FLOORS ? value : 0);
        } else if (!strcmp(word, "checkpoint") && sscanf(line, "checkpoint %u", &value) == 1) {
            progress->checkpoint = (uint16_t)(value < TOWER_MAX_FLOORS ? value : 0);
        } else if (!strcmp(word, "completions") && sscanf(line, "completions %u", &value) == 1) {
            progress->completions = (uint16_t)(value < 0xFFFF ? value : 0xFFFF);
        } else if (!strcmp(word, "flags") && sscanf(line, "flags %u", &value) == 1) {
            progress->flags = (uint16_t)value;
        } else if (!strcmp(word, "cleared")) {
            const char *at = line + strlen("cleared");
            while (*at) {
                char *end;
                long floor = strtol(at, &end, 10);
                if (end == at) break;
                if (floor >= 0 && floor < TOWER_MAX_FLOORS)
                    progress->cleared[floor >> 3] |= (uint8_t)(1 << (floor & 7));
                at = end;
            }
        }
    }
    if (!found) Tower_ClearProgress(progress, 0, identity);
    return found;
}

void Tower_WriteProgress(FILE *file, const char *mod, const TowerProgress *progress, const char *foreign)
{
    int floor;
    fprintf(file, "tower %s\n", mod);
    fprintf(file, "identity %08X\n", (unsigned)progress->tower);
    fprintf(file, "owner %08X\n", (unsigned)progress->owner);
    fprintf(file, "floor %u\n", (unsigned)progress->floor);
    fprintf(file, "checkpoint %u\n", (unsigned)progress->checkpoint);
    fprintf(file, "completions %u\n", (unsigned)progress->completions);
    fprintf(file, "flags %u\n", (unsigned)progress->flags);
    fprintf(file, "cleared");
    for (floor = 0; floor < TOWER_MAX_FLOORS; floor++) {
        if (Tower_FloorCleared(progress, floor)) fprintf(file, " %d", floor);
    }
    fprintf(file, "\nend\n");
    if (foreign && *foreign) fputs(foreign, file);
}
