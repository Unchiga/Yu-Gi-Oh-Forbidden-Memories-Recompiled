/* Guardian Stars as mods change them (stars.h, notes/modding.md).
 *
 * The matchup table is one signed adjustment per ordered pair of 4-bit star
 * ids. Nothing is decided until a mod decides it: Duel_CalcGuardianStarMatchup
 * asks Stars_Matchup first and otherwise runs the disc's arithmetic, so a run
 * without such a mod is the disc's to the bit. */
#include "stars.h"
#include "cards.h"
#include "tables.h"
#include "pc/mods/mods.h"
#include "pc/mods/json.h"
#include "pc/platform/paths.h"
#include "pc/compat/fs.h"
#include "pc/text/glyphs.h"
#include "pc/text/language.h"
#include "pc/guest/low_memory.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IDS (STARS_MAX + 1)   /* 0 (no star) to 15 */
#define RETAIL_BONUS 500      /* the disc's DUEL_GUARDIAN_STAR_BONUS */
#define NAMES_PER_STAR 16     /* languages a star's "name" may list */

extern int gDuel_adwCardStats[];

static const char *const retail_names[STARS_RETAIL + 1] = {
    "", "Mars", "Jupiter", "Saturn", "Uranus", "Pluto", "Neptune", "Mercury", "Sun", "Moon", "Venus"};

typedef struct {
    char language[16];   /* "" for a plain string, or "default" */
    char *text;          /* UTF-8 */
} StarName;

typedef struct {
    unsigned char declared;
    const char *mod;     /* the latest mod that declared it */
    StarName names[NAMES_PER_STAR];
    int name_count;
    char *icon;          /* a PNG's full path */
    unsigned char own_palette;   /* "palette": "own" */
    unsigned char *compiled;
    int compile_tried;
    char *fallback;      /* "Star N", for Stars_Name */
} Star;

static short bonus_of[IDS][IDS];
static unsigned char decided[IDS][IDS];
static Star stars[IDS];
static int choice_mode = STARS_CHOICE_ASK;
static int built;
static int any;          /* whether any mod has a "guardian_stars" */
static int no_star;      /* whether a mod made a monster with no star */
void (*Stars_IconsCleared)(void);

/* --- the disc's rule -------------------------------------------------- */

/* Duel_CalcGuardianStarMatchup's arithmetic, as the disc runs it for any
 * ids: 1-6 and 7-10 are two cycles; the next star in its cycle is +500 and
 * the previous -500. Ids outside 1-10 go through the same arithmetic, so
 * some of them meet +/-500 too (0 against Mars, 11 against Mercury). */
int Stars_RetailMatchup(int a0, int a1)
{
    int v1;
    a0 -= 7;
    if (a0 >= 0) {
        a1 -= 7;
        if (a1 < 0) return 0;
        v1 = 4;
    } else {
        v1 = 6;
        a1 -= 1;
        a0 += v1;
        if (a1 >= v1) return 0;
    }
    a0 += 1;
    if (a0 >= v1) a0 = 0;
    if (a0 == a1) return RETAIL_BONUS;
    a0 -= 2;
    if (a0 < 0) a0 += v1;
    if (a0 == a1) return -RETAIL_BONUS;
    return 0;
}

/* --- reading ----------------------------------------------------------- */

static int same_letters(const char *a, const char *b)
{
    for (;;) {
        while (*a && !isalnum((unsigned char)*a) && !((unsigned char)*a & 0x80)) a++;
        while (*b && !isalnum((unsigned char)*b) && !((unsigned char)*b & 0x80)) b++;
        if (!*a || !*b) return !*a && !*b;
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    }
}

static char *copy(const char *text)
{
    size_t length = strlen(text) + 1;
    char *out = malloc(length);
    if (out) memcpy(out, text, length);
    return out;
}

/* A star: its number (1-15), or a name the disc or a mod gives it. */
static int star_value(const JsonValue *value)
{
    if (!value) return -1;
    if (Json_TypeOf(value) == JSON_NUMBER) return (int)Json_Number(value, -1);
    if (Json_TypeOf(value) != JSON_STRING) return -1;
    return Stars_Find(Json_String(value, ""));
}

static void decide(int attacker, int defender, int bonus)
{
    bonus_of[attacker][defender] = (short)bonus;
    decided[attacker][defender] = 1;
}

/* A bonus as far as a 16-bit stat goes (the on-screen modifier is one); the
 * stat cap is a mod's "limits", checked in Stars_Check. */
static int read_bonus(const char *mod, const char *where, const JsonValue *value, int *bonus)
{
    long points = Json_Number(value, 0);
    if (Json_TypeOf(value) == JSON_NUMBER && points >= -TABLES_LIMIT_STAT_MAX && points <= TABLES_LIMIT_STAT_MAX) {
        *bonus = (int)points;
        return 1;
    }
    Mods_Note(mod, "guardian_stars: %s: a bonus is a whole number of points, -%d to %d", where,
              TABLES_LIMIT_STAT_MAX, TABLES_LIMIT_STAT_MAX);
    return 0;
}

static int valid_id(const char *mod, const char *where, int id)
{
    if (id >= 1 && id <= STARS_MAX) return 1;
    if (id > STARS_MAX) {
        Mods_Note(mod, "guardian_stars: %s: star %d: a card holds a star in 4 bits, so there are 15 at most", where, id);
    } else {
        Mods_Note(mod, "guardian_stars: %s: not a star (a number 1-15, or a star's name)", where);
    }
    return 0;
}

static void clear_names(Star *star)
{
    int i;
    for (i = 0; i < star->name_count; i++) free(star->names[i].text);
    star->name_count = 0;
    Memories_LowFree(star->compiled);
    star->compiled = NULL;
    star->compile_tried = 0;
}

static void read_name(const char *mod, int id, Star *star, const JsonValue *name)
{
    if (!name) return;
    if (Json_TypeOf(name) == JSON_STRING) {
        clear_names(star);
        star->names[0].language[0] = '\0';
        star->names[0].text = copy(Json_String(name, ""));
        star->name_count = star->names[0].text != NULL;
        return;
    }
    if (Json_TypeOf(name) == JSON_OBJECT) {
        const JsonValue *member;
        clear_names(star);
        for (member = Json_At(name, 0); member; member = Json_Next(member)) {
            StarName *entry;
            if (Json_TypeOf(member) != JSON_STRING) {
                Mods_Note(mod, "guardian_stars: star %d: \"name\" \"%s\" is not a string", id, Json_Name(member));
                continue;
            }
            if (star->name_count == NAMES_PER_STAR) {
                Mods_Note(mod, "guardian_stars: star %d: more than %d names; the rest left out", id, NAMES_PER_STAR);
                break;
            }
            entry = &star->names[star->name_count];
            snprintf(entry->language, sizeof(entry->language), "%s", Json_Name(member));
            entry->text = copy(Json_String(member, ""));
            if (entry->text) star->name_count++;
        }
        return;
    }
    Mods_Note(mod, "guardian_stars: star %d: \"name\" is a string, or an object of languages (\"en-us\", \"fr\", ...)",
              id);
}

static void read_icon(const char *mod, const char *directory, int id, Star *star, const JsonValue *icon)
{
    const char *file = Json_String(icon, NULL);
    char path[1200];
    FILE *probe;
    if (!icon) return;
    if (!file || !*file) {
        Mods_Note(mod, "guardian_stars: star %d: \"icon\" names a PNG in the mod", id);
        return;
    }
    if (!Paths_Contained(file) ||
        snprintf(path, sizeof(path), "%s/%s", directory ? directory : ".", file) >= (int)sizeof(path)) {
        Mods_Note(mod, "guardian_stars: star %d: \"icon\": %s is outside the mod", id, file);
        return;
    }
    if (!(probe = fopen(path, "rb"))) {
        Mods_Note(mod, "guardian_stars: star %d: \"icon\": %s is not there", id, file);
        return;
    }
    fclose(probe);
    free(star->icon);
    star->icon = copy(path);
}

static void read_stars(const char *mod, const char *directory, const JsonValue *list, int bonus)
{
    const JsonValue *entry;
    int index = 0;
    char where[64];
    if (!list) return;
    if (Json_TypeOf(list) != JSON_ARRAY) {
        Mods_Note(mod, "guardian_stars: \"stars\" is a list of {\"id\", \"name\", \"icon\"}");
        return;
    }
    /* Declared first, all of them, so that "beats" and "matchups" may name a
     * star declared further down the list. */
    for (entry = Json_At(list, 0); entry; entry = Json_Next(entry), index++) {
        int id = (int)Json_Number(Json_Member(entry, "id"), -1), j;
        Star *star;
        snprintf(where, sizeof(where), "stars[%d]", index);
        if (Json_TypeOf(entry) != JSON_OBJECT || !Json_Member(entry, "id")) {
            Mods_Note(mod, "guardian_stars: %s: an object with an \"id\" (1-15)", where);
            continue;
        }
        if (!valid_id(mod, where, id)) continue;
        star = &stars[id];
        /* A new star meets every other as neutral, unless an earlier mod
         * already declared it; the disc's arithmetic would give 11-15 the
         * odd +/-500 against Mercury. */
        if (id > STARS_RETAIL && !star->declared) {
            for (j = 0; j < IDS; j++) {
                decide(id, j, 0);
                decide(j, id, 0);
            }
        }
        star->declared = 1;
        star->mod = mod;
        read_name(mod, id, star, Json_Member(entry, "name"));
        read_icon(mod, directory, id, star, Json_Member(entry, "icon"));
        if (Json_Member(entry, "palette")) {
            const char *palette = Json_String(Json_Member(entry, "palette"), "");
            if (same_letters(palette, "own")) {
                star->own_palette = 1;
            } else if (same_letters(palette, "game")) {
                star->own_palette = 0;
            } else {
                Mods_Note(mod, "guardian_stars: star %d: \"palette\" is \"game\" (the stars' colors) or \"own\"", id);
            }
        }
    }
    index = 0;
    for (entry = Json_At(list, 0); entry; entry = Json_Next(entry), index++) {
        const JsonValue *beats = Json_Member(entry, "beats"), *target;
        int id = (int)Json_Number(Json_Member(entry, "id"), -1), k = 0;
        if (!beats || id < 1 || id > STARS_MAX || Json_TypeOf(entry) != JSON_OBJECT) continue;
        if (Json_TypeOf(beats) != JSON_ARRAY) {
            Mods_Note(mod, "guardian_stars: stars[%d]: \"beats\" is a list of stars", index);
            continue;
        }
        for (target = Json_At(beats, 0); target; target = Json_Next(target), k++) {
            int other = star_value(target);
            snprintf(where, sizeof(where), "stars[%d].beats[%d]", index, k);
            if (!valid_id(mod, where, other)) continue;
            decide(id, other, bonus);
            decide(other, id, -bonus);
        }
    }
}

static void read_matchups(const char *mod, const JsonValue *list, int magnitude)
{
    const JsonValue *entry;
    int index = 0;
    char where[64];
    if (!list) return;
    if (Json_TypeOf(list) != JSON_ARRAY) {
        Mods_Note(mod, "guardian_stars: \"matchups\" is a list of {\"attacker\", \"defender\", \"bonus\"}");
        return;
    }
    for (entry = Json_At(list, 0); entry; entry = Json_Next(entry), index++) {
        int attacker, defender, bonus = magnitude;
        const JsonValue *points;
        snprintf(where, sizeof(where), "matchups[%d]", index);
        if (Json_TypeOf(entry) != JSON_OBJECT) {
            Mods_Note(mod, "guardian_stars: %s: an object with \"attacker\", \"defender\" and \"bonus\"", where);
            continue;
        }
        attacker = star_value(Json_Member(entry, "attacker"));
        defender = star_value(Json_Member(entry, "defender"));
        if (!valid_id(mod, where, attacker) || !valid_id(mod, where, defender)) continue;
        /* Left out, the bonus is the default's: "attacker beats defender". */
        if ((points = Json_Member(entry, "bonus")) != NULL && !read_bonus(mod, where, points, &bonus)) continue;
        decide(attacker, defender, bonus);
        if (Json_Bool(Json_Member(entry, "mirror"), 0)) decide(defender, attacker, -bonus);
    }
}

static const char *const known_keys[] = {"stars", "matchups", "default_bonus", "replace", "choice"};
static const char *const choice_names[] = {"ask", "first", "best"};

static void read_choice(const char *mod, const JsonValue *value)
{
    const char *text = Json_String(value, NULL);
    int i;
    if (!value) return;
    for (i = 0; text && i < (int)(sizeof(choice_names) / sizeof(choice_names[0])); i++) {
        if (same_letters(text, choice_names[i])) {
            choice_mode = i;
            return;
        }
    }
    Mods_Note(mod, "guardian_stars: \"choice\" is \"ask\", \"first\" or \"best\"");
}

void Stars_AddFrom(const char *mod, const char *directory, const JsonValue *manifest)
{
    const JsonValue *section = Json_Member(manifest, "guardian_stars"), *member, *value;
    /* This section's own: a later mod's "beats" and bonus-less matchups do
     * not take an earlier mod's "default_bonus" (the FM Editor's table
     * reads each section alone too). */
    int default_bonus = RETAIL_BONUS;
    int a, d;
    if (!section) return;
    if (Json_TypeOf(section) != JSON_OBJECT) {
        Mods_Note(mod, "\"guardian_stars\" is an object: \"stars\", \"matchups\", \"default_bonus\", \"replace\", \"choice\"");
        return;
    }
    any = 1;
    for (member = Json_At(section, 0); member; member = Json_Next(member)) {
        size_t k;
        for (k = 0; k < sizeof(known_keys) / sizeof(known_keys[0]); k++) {
            if (!strcmp(Json_Name(member), known_keys[k])) break;
        }
        if (k == sizeof(known_keys) / sizeof(known_keys[0])) {
            Mods_Note(mod, "guardian_stars: unknown key '%s' (the keys are stars, matchups, default_bonus, replace, choice)",
                      Json_Name(member));
        }
    }
    /* "replace": every pair starts neutral, the disc's cycles gone. */
    if (Json_Bool(Json_Member(section, "replace"), 0)) {
        for (a = 0; a < IDS; a++) {
            for (d = 0; d < IDS; d++) decide(a, d, 0);
        }
    }
    /* "default_bonus": what the disc's cycles give in place of 500, and
     * what "beats" and a matchup with no "bonus" give. */
    if ((value = Json_Member(section, "default_bonus")) != NULL) {
        int bonus;
        if (read_bonus(mod, "default_bonus", value, &bonus)) {
            default_bonus = bonus;
            if (!Json_Bool(Json_Member(section, "replace"), 0)) {
                for (a = 1; a <= STARS_RETAIL; a++) {
                    for (d = 1; d <= STARS_RETAIL; d++) {
                        int retail = Stars_RetailMatchup(a, d);
                        if (retail) decide(a, d, retail > 0 ? bonus : -bonus);
                    }
                }
            }
        }
    }
    read_choice(mod, Json_Member(section, "choice"));
    read_stars(mod, directory, Json_Member(section, "stars"), default_bonus);
    read_matchups(mod, Json_Member(section, "matchups"), default_bonus);
}

void Stars_Add(const char *mod, const JsonValue *manifest)
{
    Stars_AddFrom(mod, NULL, manifest);
}

void Stars_Clear(void)
{
    int i;
    for (i = 0; i < IDS; i++) {
        clear_names(&stars[i]);
        free(stars[i].icon);
        free(stars[i].fallback);
    }
    if (Stars_IconsCleared) Stars_IconsCleared();
    memset(stars, 0, sizeof(stars));
    memset(bonus_of, 0, sizeof(bonus_of));
    memset(decided, 0, sizeof(decided));
    choice_mode = STARS_CHOICE_ASK;
    any = 0;
    no_star = 0;
    built = 1;   /* the tests add their own */
}

void Stars_Build(void)
{
    int i;
    if (built) return;
    built = 1;
    for (i = 0; i < Mods_LoadedCount(); i++) {
        int mod = Mods_Loaded(i);
        if (Mods_Active(mod)) Stars_AddFrom(Mods_Id(mod), Mods_Directory(mod), Mods_Manifest(mod));
    }
}

/* --- what the game asks ------------------------------------------------ */

int Stars_Matchup(int attacker, int defender, int *bonus)
{
    Stars_Build();
    if (attacker < 0 || attacker > STARS_MAX || defender < 0 || defender > STARS_MAX) return 0;
    /* A monster with no star gives no bonus and takes none: star 0 is
     * neutral both ways, which the disc's arithmetic is not (0 against Mars
     * is +500 there). Only once a mod has such a card, so that nothing else
     * reading star 0 changes. */
    if (no_star && (attacker == 0 || defender == 0)) {
        *bonus = 0;
        return 1;
    }
    if (!any || !decided[attacker][defender]) return 0;
    *bonus = bonus_of[attacker][defender];
    return 1;
}

int Stars_Bonus(int attacker, int defender)
{
    int bonus;
    return Stars_Matchup(attacker, defender, &bonus) ? bonus : Stars_RetailMatchup(attacker, defender);
}

int Stars_ChoiceMode(void)
{
    Stars_Build();
    return choice_mode;
}

int Stars_Single(int first, int second)
{
    return second == 0 || second == first;
}

int Stars_Value(const JsonValue *value)
{
    const char *text;
    int star;
    if (!value) return -1;
    if (Json_TypeOf(value) == JSON_NULL) return 0;
    if (Json_TypeOf(value) == JSON_NUMBER) {
        star = (int)Json_Number(value, -1);
        return star >= 0 ? star : -1;
    }
    if (Json_TypeOf(value) != JSON_STRING) return -1;
    text = Json_String(value, "");
    /* A star a mod names "None" is that star; otherwise "none" and the FM
     * Editor's "(none)" are no star. */
    if ((star = Stars_Find(text)) >= 0) return star;
    return same_letters(text, "none") ? 0 : -1;
}

int Stars_Normalize(int *first, int *second)
{
    if (*first != 0 || *second <= 0) return 0;
    *first = *second;
    *second = 0;
    return 1;
}

void Stars_NoteNoStar(void)
{
    no_star = 1;
}

int Stars_NoStarUsed(void)
{
    return no_star;
}

int Stars_SummonChoice(int first, int second, const int *enemy, int count)
{
    int scores[2] = {0, 0}, which, i;
    if (Stars_Single(first, second)) return 0;
    switch (Stars_ChoiceMode()) {
    case STARS_CHOICE_FIRST:
        return 0;
    case STARS_CHOICE_BEST:
        /* What it gains attacking each, less what each gains attacking it. */
        for (which = 0; which < 2; which++) {
            int star = which ? second : first;
            for (i = 0; i < count; i++) scores[which] += Stars_Bonus(star, enemy[i]) - Stars_Bonus(enemy[i], star);
        }
        return scores[1] > scores[0];
    default:
        return -1;
    }
}

int Stars_DisplayStep(int shown)
{
    int step = (shown + 31) / 32;
    return step > 16 ? step : 16;
}

int Stars_Count(void)
{
    int id, count = STARS_RETAIL;
    Stars_Build();
    for (id = STARS_RETAIL + 1; id <= STARS_MAX; id++) {
        if (stars[id].declared) count = id;
    }
    return count;
}

int Stars_Declared(int star)
{
    Stars_Build();
    return star >= 1 && star <= STARS_MAX && stars[star].declared;
}

int Stars_Find(const char *name)
{
    int id, i;
    char *end;
    long number;
    if (!name || !*name) return -1;
    number = strtol(name, &end, 10);
    if (end != name && !*end) return number >= 0 && number <= STARS_MAX ? (int)number : -1;
    for (id = 1; id <= STARS_RETAIL; id++) {
        if (same_letters(name, retail_names[id])) return id;
    }
    Stars_Build();
    for (id = 1; id <= STARS_MAX; id++) {
        for (i = 0; i < stars[id].name_count; i++) {
            if (same_letters(name, stars[id].names[i].text)) return id;
        }
    }
    return -1;
}

/* The name for the language the game runs in: that language's, else
 * "default", else "en-us" or "en", else the first. */
static const char *name_for_language(const Star *star)
{
    const char *code = Language_Code(Language_Current());
    static const char *const fallbacks[] = {"default", "en-us", "en"};
    size_t f;
    int i;
    if (!star->name_count) return NULL;
    for (i = 0; i < star->name_count; i++) {
        if (!star->names[i].language[0] || (code && !strcmp(star->names[i].language, code))) return star->names[i].text;
    }
    for (f = 0; f < sizeof(fallbacks) / sizeof(fallbacks[0]); f++) {
        for (i = 0; i < star->name_count; i++) {
            if (!strcmp(star->names[i].language, fallbacks[f])) return star->names[i].text;
        }
    }
    return star->names[0].text;
}

/* UTF-8 to the names bank's glyph codes, as a card's name is (cards.c), in
 * low memory: the game keeps the text in a 4-byte pointer. */
static unsigned char *compile(const char *mod, int id, const char *text)
{
    size_t length = 0;
    unsigned char *out = Memories_LowAlloc(strlen(text) * 2 + 1);
    int bad = 0;
    if (!out) return NULL;
    while (*text) {
        const char *letter = text;
        uint32_t character = Glyphs_NextCharacter(&text);
        int code;
        if (character == GLYPHS_NOT_UTF8) {
            if (!bad++) Mods_Note(mod, "guardian_stars: star %d: its name is not UTF-8; save the file as UTF-8", id);
            continue;
        }
        if ((code = Glyphs_Code(character)) < 0) {
            Mods_Note(mod, "guardian_stars: star %d: the game has no letter \"%.*s\"; left out of its name", id,
                      (int)(text - letter), letter);
            continue;
        }
        if (code >= 0xF0) {
            out[length++] = (unsigned char)(0xF0 + (code >> 8));
            out[length++] = (unsigned char)code;
        } else {
            out[length++] = (unsigned char)code;
        }
    }
    out[length] = 0xFF;
    return out;
}

const unsigned char *Stars_NameText(int star)
{
    Star *entry;
    const char *name;
    Stars_Build();
    if (star < 1 || star > STARS_MAX) return NULL;
    entry = &stars[star];
    if (!entry->compile_tried) {
        entry->compile_tried = 1;
        if ((name = name_for_language(entry)) != NULL) {
            entry->compiled = compile(entry->mod ? entry->mod : "guardian_stars", star, name);
        } else if (entry->declared && star > STARS_RETAIL) {
            /* A new star with no name: its number, not the disc's Dragon
             * that the names bank has at its place. */
            char fallback[16];
            snprintf(fallback, sizeof(fallback), "Star %d", star);
            entry->compiled = compile(entry->mod ? entry->mod : "guardian_stars", star, fallback);
        }
    }
    return entry->compiled;
}

int Stars_Named(int star)
{
    Stars_Build();
    return star >= 1 && star <= STARS_MAX && stars[star].name_count > 0;
}

const char *Stars_Name(int star)
{
    const char *name;
    Stars_Build();
    if (star < 1 || star > STARS_MAX) return "";
    if ((name = name_for_language(&stars[star])) != NULL) return name;
    if (star <= STARS_RETAIL) return retail_names[star];
    if (!stars[star].fallback && (stars[star].fallback = malloc(16)) != NULL) {
        snprintf(stars[star].fallback, 16, "Star %d", star);
    }
    return stars[star].fallback ? stars[star].fallback : "";
}

const char *Stars_Icon(int star)
{
    Stars_Build();
    return star >= 1 && star <= STARS_MAX ? stars[star].icon : NULL;
}

int Stars_IconOwnPalette(int star)
{
    Stars_Build();
    return star >= 1 && star <= STARS_MAX && stars[star].own_palette;
}

void Stars_Check(void)
{
    int used[IDS] = {0}, card, star, other, cap;
    const char *mod;
    Stars_Build();
    if (!any) return;
    for (card = 1; card <= gCard_nCount; card++) {
        unsigned stats = (unsigned)gDuel_adwCardStats[card - 1];
        if (((stats >> 26) & 0x1F) >= 20) continue;   /* magic, trap, ritual, equip */
        used[(stats >> 22) & 0xF]++;
        used[(stats >> 18) & 0xF]++;
    }
    for (star = 1; star <= STARS_MAX; star++) {
        int reachable = 0;
        mod = stars[star].mod ? stars[star].mod : "guardian_stars";
        if (star > STARS_RETAIL && used[star] && !stars[star].declared) {
            Mods_Note(mod, "guardian_stars: %d card%s star %d, which no mod declares", used[star],
                      used[star] == 1 ? " has" : "s have", star);
        }
        if (!stars[star].declared) continue;
        if (!used[star]) Mods_Note(mod, "guardian_stars: no card has star %d (%s)", star, Stars_Name(star));
        for (other = 1; other <= STARS_MAX; other++) {
            reachable |= Stars_Bonus(star, other) != 0 || Stars_Bonus(other, star) != 0;
        }
        if (!reachable) {
            Mods_Note(mod, "guardian_stars: star %d (%s) has no matchup: every battle with it is neutral", star,
                      Stars_Name(star));
        }
    }
    /* Past the stat cap (a mod's "limits"), every such battle is the cap's. */
    cap = Tables_StatCapEither();
    for (star = 1; star <= STARS_MAX; star++) {
        for (other = 1; other <= STARS_MAX; other++) {
            int bonus = decided[star][other] ? bonus_of[star][other] : 0;
            if (bonus > cap || -bonus > cap) {
                Mods_Note(stars[star].mod ? stars[star].mod : "guardian_stars",
                          "guardian_stars: star %d against %d: %d is past the ATK/DEF cap of %d", star, other, bonus,
                          cap);
                return;
            }
        }
    }
}
