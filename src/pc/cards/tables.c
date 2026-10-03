/* The duel's rule tables as mods change them (tables.h,
 * notes/gameplay-tables.md).
 *
 * Each applied mod's "fusions", "equips", "rituals", "drops" and "decks" are
 * read into rules here, once, in the order the mods load. The game's table
 * readers (duel_card_checks.c, duel_check_ritual.c, duel_shuffle_deck.c,
 * duel_result_runtime.c) ask these first. A deck may also be fixed: forty
 * cards written down by their copies, dealt as they are. Past the tables,
 * "chest_overflow" turns a card the chest has no room for into
 * starchips (Duel_AwardCard), and "terrain_bonus" sets what a terrain gives
 * each monster type (Duel_GetTerrainBoost), and "trap_thresholds" the attack
 * each attack trap stops (Duel_SelectAttackTrap), and "passwords" the
 * Password screen's passwords and prices (Main_RunPasswordMenu), and
 * "limits" the numbers the game caps (ATK/DEF, LP, starchips...). A drop or
 * deck pool is worked out from what the game loaded when it is drawn from,
 * so a data mod's patch of the same pool comes first and the edits here go
 * on top of it. */
#include "tables.h"
#include "cards.h"
#include "pc/free_duel/duelists.h"
#include "pc/mods/mods.h"
#include "pc/mods/json.h"
#include "pc/debug/log.h"
#include "pc/platform/paths.h"
#include "pc/compat/fs.h"
#include "game/card_constants.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>

extern signed char gDuel_bOpponentID;

const char *const Tables_DuelistNames[TABLES_DUELIST_COUNT] = {
    "Unused", "Simon Muran", "Teana", "Jono", "Villager 1", "Villager 2", "Villager 3", "Seto", "Heishin",
    "Rex Raptor", "Weevil Underwood", "Mai Valentine", "Bandit Keith", "Shadi", "Yami Bakura", "Pegasus",
    "Isis", "Kaiba", "Mage Soldier", "Jono 2nd", "Teana 2nd", "Ocean Mage", "High Mage Secmeton",
    "Forest Mage", "High Mage Anubisius", "Mountain Mage", "High Mage Atenza", "Desert Mage",
    "High Mage Martis", "Meadow Mage", "High Mage Kepura", "Labyrinth Mage", "Seto 2nd", "Guardian Sebek",
    "Guardian Neku", "Heishin 2nd", "Seto 3rd", "DarkNite", "Nitemare", "Duel Master K"};

const char *Tables_DuelistShortName(int duelist)
{
    /* The names longer than 11 letters, shortened: a High Mage or a
     * Guardian keeps the title as H.M. or G. */
    static const struct {
        int duelist;
        const char *name;
    } shorter[] = {{10, "Weevil"},   {11, "Mai"},       {12, "Keith"},    {18, "Soldier"},  {22, "H.M. Secmeton"},
                   {24, "H.M. Anubisius"}, {25, "Mountain"},  {26, "H.M. Atenza"},   {28, "H.M. Martis"},   {30, "H.M. Kepura"},
                   {31, "Labyrinth"}, {33, "G. Sebek"},     {34, "G. Neku"},     {39, "Master K"}};
    static char latin[64], name[TABLES_SHORT_NAME_LIMIT + 1];
    const unsigned char *in;
    size_t n = 0;
    unsigned i;
    if (duelist < 1 || !Duelists_Valid(duelist)) return NULL;
    if (!Tables_DuelistRenamed(duelist)) {
        for (i = 0; i < sizeof(shorter) / sizeof(shorter[0]); i++) {
            if (shorter[i].duelist == duelist) return shorter[i].name;
        }
        return Duelists_Name(duelist);
    }
    /* A mod's name, which is UTF-8, in the Latin-1 the name box draws: a
     * letter past Latin-1 ends it, as any other letter the box lacks ends a
     * translated name (Tables_ShortenName). */
    for (in = (const unsigned char *)Duelists_Name(duelist); *in && n < sizeof(latin) - 1; in++) {
        if (*in < 0x80) {
            latin[n++] = (char)*in;
        } else if ((*in == 0xC2 || *in == 0xC3) && (in[1] & 0xC0) == 0x80) {
            latin[n++] = (char)(((*in & 0x1F) << 6) | (in[1] & 0x3F));
            in++;
        } else {
            break;
        }
    }
    latin[n] = '\0';
    return Tables_ShortenName(latin, name) ? name : NULL;
}

int Tables_DuelistRenamed(int duelist)
{
    return Duelists_Valid(duelist) &&
           (duelist >= TABLES_DUELIST_COUNT || strcmp(Duelists_Name(duelist), Tables_DuelistNames[duelist]));
}

/* A letter of Latin-1: A-Z, a-z, or an accented one (not × or ÷). */
static int name_letter(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c != 0xD7 && c != 0xF7);
}

static int name_capital(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7);
}

int Tables_ShortenName(const char *name, char *out)
{
    const unsigned char *in = (const unsigned char *)name;
    char whole[64];
    size_t n = 0, last = 0, i;
    int capitals = 1, words = 0;
    out[0] = '\0';
    while (*in && n < sizeof(whole) - 1 && (name_letter(*in) || *in == ' ' || *in == '.')) whole[n++] = (char)*in++;
    /* Up to an elided article ("Sekmeton l'Archimage", "Mago dell'Oceano"):
     * the words before it, not the article's letters. */
    if (*in == '\'') {
        while (n && whole[n - 1] != ' ') n--;
    }
    while (n && whole[n - 1] == ' ') n--;
    whole[n] = '\0';
    if (!n) return 0;
    if (n <= TABLES_SHORT_NAME_LIMIT) {
        memcpy(out, whole, n + 1);
        return 1;
    }
    /* Its words: where the last starts, and whether each starts with a
     * capital. */
    for (i = 0; i < n; i++) {
        if (whole[i] != ' ' && (i == 0 || whole[i - 1] == ' ')) {
            capitals &= name_capital((unsigned char)whole[i]);
            last = i;
            words++;
        }
    }
    /* Each first word's initial and full stop, a space, the last word. */
    if (words > 1 && capitals && 2 * (size_t)(words - 1) + 1 + (n - last) <= TABLES_SHORT_NAME_LIMIT) {
        size_t k = 0;
        for (i = 0; i < last; i++) {
            if (whole[i] != ' ' && (i == 0 || whole[i - 1] == ' ')) {
                out[k++] = whole[i];
                out[k++] = '.';
            }
        }
        out[k++] = ' ';
        memcpy(out + k, whole + last, n - last + 1);
        return 1;
    }
    if (words > 1 && n - last <= TABLES_SHORT_NAME_LIMIT) {
        memcpy(out, whole + last, n - last + 1);
        return 1;
    }
    /* One word too long: its first letters. */
    for (n = TABLES_SHORT_NAME_LIMIT; n && (whole[n - 1] == ' ' || whole[n - 1] == '.'); n--) {
    }
    memcpy(out, whole, n);
    out[n] = '\0';
    return n != 0;
}

int Tables_OpponentId(void)
{
    return gDuel_bOpponentID;
}

static const char *const pool_names[TABLES_POOL_COUNT] = {"deck", "pow", "bcd", "tec"};
static const char *const pool_long_names[TABLES_POOL_COUNT] = {"deck", "sa-pow", "b-c-d", "sa-tec"};

/* A deck is 40 cards, at most three of each, so its pool needs 14. */
#define POOL_TOTAL DUEL_DROP_WEIGHT_TOTAL
#define DECK_POOL_MIN_CARDS ((DECK_SIZE + DECK_CARD_COPY_LIMIT - 1) / DECK_CARD_COPY_LIMIT)

typedef struct {
    unsigned short low, high;   /* the two cards, low <= high */
    int result;                 /* 0: the fusion is forbidden */
    unsigned order;             /* later rules win */
} FusionRule;

enum { TARGET_ANY, TARGET_TYPE, TARGET_CARD };
typedef struct {
    unsigned short equip;
    unsigned char kind, allow;  /* TARGET_*, and whether it may equip */
    int target;                 /* a type or a card id */
    unsigned order;
} EquipRule;

typedef struct {
    unsigned short recipe[6];   /* ritual, three tributes, result, 0 */
    TablesRitualRequirement requirements[DUEL_RITUAL_TRIBUTE_COUNT];
    unsigned char removed, conditional;
} RitualRule;

typedef struct {
    const char *mod;
    /* `duelist` is an int: it indexes the extended list, which a mod can take
     * past what a byte holds. */
    int duelist;
    unsigned char pool, replace;
    unsigned char warned, waiting;   /* told it failed; told a fixed deck wins */
    int count;
    unsigned short *cards, *weights;
} PoolEdit;

static FusionRule *fusions;
static int fusion_count, fusion_room;
static unsigned char *removed_results;   /* by card id: no retail recipe makes it */
static int removed_room;
static EquipRule *equips;
static int equip_count, equip_room;
/* An equip's bonus: for any monster, or one of a type or an attribute. */
enum { BONUS_ANY, BONUS_TYPE, BONUS_ATTRIBUTE };
typedef struct {
    unsigned short equip;
    unsigned char kind, value;  /* BONUS_*, and the type or attribute */
    short bonus;
    unsigned order, rank;       /* the entry, and the place in its "bonus_if" */
} BonusRule;

static BonusRule *bonuses;
static int bonus_count, bonus_room;
static short equip_default;             /* "equip_bonus_default", when set */
static unsigned char equip_default_set;
static RitualRule *rituals;
static int ritual_count, ritual_room;
static PoolEdit *edits;
static int edit_count, edit_room;
typedef struct {
    const char *mod;
    int duelist;                       /* an added duelist's id outgrows a byte */
    unsigned short cards[DECK_SIZE];   /* in id order */
} FixedDeck;
static FixedDeck *fixed_decks;
static int fixed_count, fixed_room;
static long overflow_starchips;          /* per card the chest has no room for */
static int chest_limit;                  /* copies the chest keeps; 0: the disc's 250 */
#define TERRAINS 6                        /* DUEL_TERRAIN_COUNT: Forest to Yami, 1-6 */
static short terrain_bonus[TERRAINS][CARD_TYPE_MAGIC];
static unsigned char terrain_listed[TERRAINS][CARD_TYPE_MAGIC];
static long trap_threshold[DUEL_ATTACK_TRAP_COUNT];   /* House of Adhesive Tape to Widespread Ruin */
static unsigned char trap_listed[DUEL_ATTACK_TRAP_COUNT];
static const char *trap_from[DUEL_ATTACK_TRAP_COUNT];      /* the mod that set it */
/* Which pools have an edit, per duelist. It grows with the duelist list
 * (pc/free_duel/duelists.h), so a duelist a mod added is named by "drops" and
 * "decks" exactly as a retail one is. */
static unsigned char *edited;
static int edited_room;

static void mark_edited(int duelist, int pool)
{
    if (duelist < 0 || pool < 0 || pool >= TABLES_POOL_COUNT) return;
    if (duelist >= edited_room) {
        const int room = Duelists_Count() > duelist + 1 ? Duelists_Count() : duelist + 1;
        unsigned char *bigger = realloc(edited, (size_t)room * TABLES_POOL_COUNT);
        if (!bigger) return;
        memset(bigger + (size_t)edited_room * TABLES_POOL_COUNT, 0,
               (size_t)(room - edited_room) * TABLES_POOL_COUNT);
        edited = bigger;
        edited_room = room;
    }
    edited[(size_t)duelist * TABLES_POOL_COUNT + pool] = 1;
}

static int is_edited(int duelist, int pool)
{
    if (!edited || duelist < 0 || duelist >= edited_room) return 0;
    if (pool < 0 || pool >= TABLES_POOL_COUNT) return 0;
    return edited[(size_t)duelist * TABLES_POOL_COUNT + pool];
}
static unsigned order_counter;
static int fusions_sorted;

/* Grow an array by one; NULL when memory runs out. */
static void *grow(void *array, int *room, int count, size_t size)
{
    void **slot = (void **)array;
    if (count >= *room) {
        int wanted = *room ? *room * 2 : 16;
        void *bigger = realloc(*slot, (size_t)wanted * size);
        if (!bigger) return NULL;
        *slot = bigger;
        *room = wanted;
    }
    return (char *)*slot + (size_t)count * size;
}

static int same_letters(const char *a, const char *b)
{
    for (;;) {
        while (*a && !isalnum((unsigned char)*a)) a++;
        while (*b && !isalnum((unsigned char)*b)) b++;
        if (!*a || !*b) return !*a && !*b;
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    }
}

/* A card the manifest names, or 0 after saying why it names none. */
static int card(const char *mod, const char *where, const JsonValue *value)
{
    int id = Cards_Reference(value);
    if (id > 0) return id;
    if (Json_TypeOf(value) == JSON_STRING) Mods_Note(mod, "%s: no card \"%s\"", where, Json_String(value, ""));
    else if (Json_TypeOf(value) == JSON_NUMBER) Mods_Note(mod, "%s: no card %ld", where, Json_Number(value, 0));
    else Mods_Note(mod, "%s: a card is a name, an id or a stable identity", where);
    return 0;
}

/* A card or a monster type ("Dragon"), for an equip's targets. */
static int target(const char *mod, const char *where, const JsonValue *value, EquipRule *rule)
{
    int type = Json_TypeOf(value) == JSON_STRING ? Cards_TypeNamed(Json_String(value, "")) : -1;
    if (type >= 0 && Cards_Reference(value) <= 0) {
        rule->kind = TARGET_TYPE;
        rule->target = type;
        return 1;
    }
    rule->kind = TARGET_CARD;
    rule->target = card(mod, where, value);
    return rule->target != 0;
}

/* --- fusions --------------------------------------------------------- */

static void add_fusion(int a, int b, int result)
{
    FusionRule *rule = grow(&fusions, &fusion_room, fusion_count, sizeof(*fusions));
    if (!rule) return;
    rule->low = (unsigned short)(a < b ? a : b);
    rule->high = (unsigned short)(a < b ? b : a);
    rule->result = result;
    rule->order = ++order_counter;
    fusion_count++;
    fusions_sorted = 0;
}

static void read_fusions(const char *mod, const JsonValue *list)
{
    int i;
    const JsonValue *rule;
    char where[64];
    if (list && Json_TypeOf(list) != JSON_ARRAY) {
        Mods_Note(mod, "\"fusions\" is not an array");
        return;
    }
    /* One pass down the list: Json_Count and Json_At each walk it from the
     * start, which made a mod of every pair (261,003 rules, the editor's
     * bulk fusions) take minutes to read. */
    for (i = 0, rule = Json_At(list, 0); rule; i++, rule = Json_Next(rule)) {
        const JsonValue *with = Json_Member(rule, "with");
        const JsonValue *result = Json_Member(rule, "result");
        const JsonValue *removed = Json_Member(rule, "remove");
        int a, b, made = 0;
        snprintf(where, sizeof(where), "fusions[%d]", i);
        if (!Mods_EntryUsed(mod, rule, where)) continue;
        if (removed) {
            /* { "remove": card }: no recipe on the disc makes it any more. */
            int id = card(mod, where, removed);
            if (!id) continue;
            if (id >= removed_room) {
                int room = gCard_nCount + 1 > id + 1 ? gCard_nCount + 1 : id + 1;
                unsigned char *bigger = realloc(removed_results, (size_t)room);
                if (!bigger) continue;
                memset(bigger + removed_room, 0, (size_t)(room - removed_room));
                removed_results = bigger;
                removed_room = room;
            }
            removed_results[id] = 1;
            continue;
        }
        if (Json_Count(with) != 2) {
            Mods_Note(mod, "%s: \"with\" names two cards", where);
            continue;
        }
        a = card(mod, where, Json_At(with, 0));
        b = card(mod, where, Json_At(with, 1));
        if (!a || !b) continue;
        if (!result) {
            Mods_Note(mod, "%s: \"result\" is a card, or null to forbid the fusion", where);
            continue;
        }
        if (Json_TypeOf(result) != JSON_NULL && !(Json_TypeOf(result) == JSON_NUMBER && Json_Number(result, 0) == 0)) {
            made = card(mod, where, result);
            if (!made) continue;
        }
        add_fusion(a, b, made);
    }
}

static int by_pair(const void *left, const void *right)
{
    const FusionRule *a = left, *b = right;
    if (a->low != b->low) return a->low < b->low ? -1 : 1;
    if (a->high != b->high) return a->high < b->high ? -1 : 1;
    return a->order > b->order ? -1 : a->order < b->order;   /* the latest first */
}

static const FusionRule *find_fusion(int a, int b)
{
    int low = 0, high = fusion_count;
    FusionRule key;
    key.low = (unsigned short)(a < b ? a : b);
    key.high = (unsigned short)(a < b ? b : a);
    while (low < high) {   /* the first rule for the pair: the latest */
        int middle = (low + high) / 2;
        const FusionRule *rule = &fusions[middle];
        if (rule->low < key.low || (rule->low == key.low && rule->high < key.high)) low = middle + 1;
        else high = middle;
    }
    if (low < fusion_count && fusions[low].low == key.low && fusions[low].high == key.high) return &fusions[low];
    return NULL;
}

int Tables_Fusion(int a, int b, int *result)
{
    const FusionRule *rule;
    int base_a, base_b;
    if (!fusion_count || !Cards_Valid(a) || !Cards_Valid(b)) return 0;
    if (!fusions_sorted) {
        qsort(fusions, (size_t)fusion_count, sizeof(*fusions), by_pair);
        fusions_sorted = 1;
    }
    rule = find_fusion(a, b);
    base_a = Cards_BaseId(a);
    base_b = Cards_BaseId(b);
    /* A copy fuses as its base, as it does in the disc's table; a rule
     * naming a copy itself is surer, so one that names both cards as they
     * are comes first, then one that names one of them (a copy with its
     * partner's base; of two such, the later), then the bases' own. */
    if (!rule) {
        const FusionRule *one = base_b != b ? find_fusion(a, base_b) : NULL;
        const FusionRule *other = base_a != a ? find_fusion(base_a, b) : NULL;
        rule = one && (!other || one->order > other->order) ? one : other;
    }
    if (!rule && base_a != a && base_b != b) rule = find_fusion(base_a, base_b);
    if (!rule) return 0;
    *result = rule->result;
    return 1;
}

int Tables_FilterFusion(int result)
{
    return result > 0 && result < removed_room && removed_results[result] ? 0 : result;
}

/* --- equips ---------------------------------------------------------- */

static void add_equip(int equip, EquipRule rule, unsigned order)
{
    EquipRule *slot = grow(&equips, &equip_room, equip_count, sizeof(*equips));
    if (!slot) return;
    rule.equip = (unsigned short)equip;
    rule.order = order;
    *slot = rule;
    equip_count++;
}

static void add_bonus(int equip, int kind, int value, long bonus, unsigned order, unsigned rank)
{
    BonusRule *slot = grow(&bonuses, &bonus_room, bonus_count, sizeof(*bonuses));
    if (!slot) return;
    slot->equip = (unsigned short)equip;
    slot->kind = (unsigned char)kind;
    slot->value = (unsigned char)value;
    slot->bonus = (short)bonus;
    slot->order = order;
    slot->rank = rank;
    bonus_count++;
}

static int bonus_points(const char *mod, const char *where, const JsonValue *value, long *points)
{
    *points = Json_Number(value, 0);
    /* As far as a 16-bit stat goes, not the 9999 cap: a mod's "limits" may
     * raise that, and the duel clamps the sum to whatever it is. */
    if (Json_TypeOf(value) == JSON_NUMBER && *points >= -TABLES_LIMIT_STAT_MAX && *points <= TABLES_LIMIT_STAT_MAX)
        return 1;
    Mods_Note(mod, "%s: a bonus is a whole number of points, -%d to %d", where, TABLES_LIMIT_STAT_MAX,
              TABLES_LIMIT_STAT_MAX);
    return 0;
}

/* An equip entry's "bonus": points, in place of the disc's +500 (+1000 for
 * Megamorph), and "bonus_if": { type or attribute: points }, for monsters
 * of that type or attribute; the first that fits comes before "bonus". */
static void read_equip_bonus(const char *mod, int index, int equip, const JsonValue *entry, unsigned order)
{
    const JsonValue *plain = Json_Member(entry, "bonus"), *conditions = Json_Member(entry, "bonus_if");
    char where[128];
    long points;
    int j;
    if (plain) {
        snprintf(where, sizeof(where), "equips[%d].bonus", index);
        if (bonus_points(mod, where, plain, &points)) add_bonus(equip, BONUS_ANY, 0, points, order, 0);
    }
    if (!conditions) return;
    if (Json_TypeOf(conditions) != JSON_OBJECT) {
        Mods_Note(mod, "equips[%d].bonus_if: an object of monster types or attributes and their bonus", index);
        return;
    }
    for (j = 0; j < Json_Count(conditions); j++) {
        const JsonValue *member = Json_At(conditions, j);
        const char *name = Json_Name(member);
        int type = Cards_TypeNamed(name), attribute = Cards_AttributeNamed(name);
        snprintf(where, sizeof(where), "equips[%d].bonus_if \"%s\"", index, name);
        if ((type < 0 || type >= CARD_TYPE_MAGIC) && attribute < 0) {
            Mods_Note(mod, "%s: not a monster type or an attribute", where);
            continue;
        }
        if (!bonus_points(mod, where, member, &points)) continue;
        if (type >= 0 && type < CARD_TYPE_MAGIC) add_bonus(equip, BONUS_TYPE, type, points, order, (unsigned)j + 1);
        else add_bonus(equip, BONUS_ATTRIBUTE, attribute, points, order, (unsigned)j + 1);
    }
}

/* "equip_bonus_default": points, what an equip no entry gives a bonus for
 * adds, in place of the disc's +500 and Megamorph's +1000 alike. */
static void read_equip_default(const char *mod, const JsonValue *value)
{
    long points;
    if (!value || !bonus_points(mod, "equip_bonus_default", value, &points)) return;
    equip_default = (short)points;
    equip_default_set = 1;
}

static void read_equips(const char *mod, const JsonValue *list)
{
    int i, j, pass;
    char where[80];
    if (list && Json_TypeOf(list) != JSON_ARRAY) {
        Mods_Note(mod, "\"equips\" is not an array");
        return;
    }
    for (i = 0; i < Json_Count(list); i++) {
        const JsonValue *entry = Json_At(list, i);
        unsigned order = ++order_counter;
        int equip;
        snprintf(where, sizeof(where), "equips[%d]", i);
        if (!Mods_EntryUsed(mod, entry, where)) continue;
        equip = card(mod, where, Json_Member(entry, "card"));
        if (!equip) continue;
        if (Cards_Type(equip) != CARD_TYPE_EQUIP) {
            Mods_Note(mod, "%s: \"card\" is not an equip card", where);
            continue;
        }
        read_equip_bonus(mod, i, equip, entry, order);
        if (Json_Bool(Json_Member(entry, "replace"), 0)) {
            EquipRule none = {0};
            none.kind = TARGET_ANY;
            add_equip(equip, none, order);
        }
        for (pass = 0; pass < 2; pass++) {
            const JsonValue *targets = Json_Member(entry, pass ? "remove" : "add");
            for (j = 0; j < Json_Count(targets); j++) {
                EquipRule rule = {0};
                snprintf(where, sizeof(where), "equips[%d].%s[%d]", i, pass ? "remove" : "add", j);
                if (!target(mod, where, Json_At(targets, j), &rule)) continue;
                rule.allow = (unsigned char)!pass;
                add_equip(equip, rule, order);
            }
        }
    }
}

int Tables_Equip(int equip, int monster)
{
    const EquipRule *best = NULL;
    int i, base_equip, base_monster, type;
    if (!equip_count || !Cards_Valid(equip) || !Cards_Valid(monster)) return -1;
    base_equip = Cards_BaseId(equip);
    base_monster = Cards_BaseId(monster);
    type = Cards_Type(monster);
    /* The latest entry that says anything decides; within it a card is
     * surer than a type, and a type than "replace". */
    for (i = 0; i < equip_count; i++) {
        const EquipRule *rule = &equips[i];
        int match;
        if (rule->equip != equip && rule->equip != base_equip) continue;
        match = rule->kind == TARGET_ANY || (rule->kind == TARGET_TYPE && rule->target == type) ||
                (rule->kind == TARGET_CARD && (rule->target == monster || rule->target == base_monster));
        if (!match) continue;
        if (!best || rule->order > best->order || (rule->order == best->order && rule->kind >= best->kind)) best = rule;
    }
    return best ? best->allow : -1;
}

int Tables_EquipBonus(int equip, int monster, int retail)
{
    const BonusRule *best = NULL;
    int i, base_equip, type, attribute;
    if ((!bonus_count && !equip_default_set) || !Cards_Valid(equip) || !Cards_Valid(monster)) return retail;
    base_equip = Cards_BaseId(equip);
    type = Cards_Type(monster);
    attribute = Cards_Attribute(monster);
    /* The latest entry that says anything about this monster decides;
     * within it the first "bonus_if" that fits, then its "bonus". */
    for (i = 0; i < bonus_count; i++) {
        const BonusRule *rule = &bonuses[i];
        int fits;
        if (rule->equip != equip && rule->equip != base_equip) continue;
        fits = rule->kind == BONUS_ANY || (rule->kind == BONUS_TYPE && rule->value == type) ||
               (rule->kind == BONUS_ATTRIBUTE && rule->value == attribute);
        if (!fits) continue;
        if (!best || rule->order > best->order ||
            (rule->order == best->order && rule->rank && (!best->rank || rule->rank < best->rank)))
            best = rule;
    }
    if (!best && !equip_default_set) return retail;
    LOG(LOG_MODS, "tables: equip %d on %d: %+d (the disc's %+d)%s", equip, monster, best ? best->bonus : equip_default,
        retail, best ? "" : ", the mods' default");
    return best ? best->bonus : equip_default;
}

/* --- rituals --------------------------------------------------------- */

static int ritual_requirement(const char *mod, const char *where, const JsonValue *value,
                              TablesRitualRequirement *out)
{
    const JsonValue *v;
    int id, type;
    memset(out, 0, sizeof(*out));
    out->type = -1;
    out->max_attack = -1;
    out->max_defense = -1;
    out->min_level = -1;
    out->max_level = -1;
    if (Json_TypeOf(value) != JSON_OBJECT) {
        id = card(mod, where, value);
        if (!id) return 0;
        out->card = (unsigned short)id;
        return 1;
    }
    v = Json_Member(value, "card");
    if (v) {
        id = card(mod, where, v);
        if (!id) return 0;
        out->card = (unsigned short)id;
    }
    v = Json_Member(value, "type");
    if (v) {
        if (Json_TypeOf(v) != JSON_STRING || (type = Cards_TypeNamed(Json_String(v, ""))) < 0 ||
            type >= CARD_TYPE_MAGIC) {
            Mods_Note(mod, "%s: ritual tribute \"type\" is a monster type", where);
            return 0;
        }
        out->type = (signed char)type;
    }
    v = Json_Member(value, "fusion_group");
    if (v) {
        int group = Cards_FusionGroupNamed(Json_String(v, ""));
        if (group == CARD_FUSION_GROUP_NONE) {
            Mods_Note(mod, "%s: \"fusion_group\" is a known secondary fusion group", where); return 0;
        }
        out->fusion_group = (unsigned char)group;
    }
    v = Json_Member(value, "min_attack");
    if (v) {
        long n = Json_Number(v, -1);
        if (n < 0 || n > CARD_STAT_MAX) {
            Mods_Note(mod, "%s: \"min_attack\" is 0 to %d", where, CARD_STAT_MAX); return 0;
        }
        out->min_attack = (short)n;
    }
    v = Json_Member(value, "min_defense");
    if (v) {
        long n = Json_Number(v, -1);
        if (n < 0 || n > CARD_STAT_MAX) {
            Mods_Note(mod, "%s: \"min_defense\" is 0 to %d", where, CARD_STAT_MAX); return 0;
        }
        out->min_defense = (short)n;
    }
    v = Json_Member(value, "max_attack");
    if (v) {
        long n = Json_Number(v, -1);
        if (n < 0 || n > CARD_STAT_MAX) {
            Mods_Note(mod, "%s: \"max_attack\" is 0 to %d", where, CARD_STAT_MAX); return 0;
        }
        out->max_attack = (short)n;
    }
    v = Json_Member(value, "max_defense");
    if (v) {
        long n = Json_Number(v, -1);
        if (n < 0 || n > CARD_STAT_MAX) {
            Mods_Note(mod, "%s: \"max_defense\" is 0 to %d", where, CARD_STAT_MAX); return 0;
        }
        out->max_defense = (short)n;
    }
    if (out->max_attack >= 0 && out->min_attack > out->max_attack) {
        Mods_Note(mod, "%s: ritual tribute minimum ATK is above maximum ATK", where); return 0;
    }
    if (out->max_defense >= 0 && out->min_defense > out->max_defense) {
        Mods_Note(mod, "%s: ritual tribute minimum DEF is above maximum DEF", where); return 0;
    }
    v = Json_Member(value, "min_level");
    if (v) {
        long n = Json_Number(v, -1);
        if (n < 0 || n > 12) {
            Mods_Note(mod, "%s: \"min_level\" is 0 to 12", where); return 0;
        }
        out->min_level = (signed char)n;
    }
    v = Json_Member(value, "max_level");
    if (v) {
        long n = Json_Number(v, -1);
        if (n < 0 || n > 12) {
            Mods_Note(mod, "%s: \"max_level\" is 0 to 12", where); return 0;
        }
        out->max_level = (signed char)n;
    }
    if (out->min_level >= 0 && out->max_level >= 0 && out->min_level > out->max_level) {
        Mods_Note(mod, "%s: ritual tribute minimum level is above maximum level", where); return 0;
    }
    v = Json_Member(value, "defense_gt_attack");
    if (v) {
        if (Json_TypeOf(v) != JSON_BOOL) {
            Mods_Note(mod, "%s: \"defense_gt_attack\" is true or false", where);
            return 0;
        }
        out->defense_gt_attack = Json_Bool(v, 0) != 0;
    }
    /* Any key counts, "min_attack": 0 too: that tribute is any monster. */
    {
        static const char *const known[] = {"card", "type", "fusion_group", "min_attack", "min_defense", "max_attack",
                                            "max_defense", "min_level", "max_level", "defense_gt_attack"};
        const JsonValue *member;
        int keys = 0;
        size_t k;
        for (member = Json_At(value, 0); member; member = Json_Next(member)) {
            for (k = 0; k < sizeof(known) / sizeof(known[0]) && strcmp(Json_Name(member), known[k]); k++) {}
            if (k < sizeof(known) / sizeof(known[0])) keys++;
            else Mods_Note(mod, "%s: a ritual tribute has no \"%s\" key; left out", where, Json_Name(member));
        }
        if (!keys) {
            Mods_Note(mod, "%s: ritual tribute has no requirement", where);
            return 0;
        }
    }
    return 1;
}

static void read_rituals(const char *mod, const JsonValue *list)
{
    int i, j;
    char where[64];
    if (list && Json_TypeOf(list) != JSON_ARRAY) {
        Mods_Note(mod, "\"rituals\" is not an array");
        return;
    }
    for (i = 0; i < Json_Count(list); i++) {
        const JsonValue *entry = Json_At(list, i);
        const JsonValue *tributes = Json_Member(entry, "tributes");
        const JsonValue *result = Json_Member(entry, "result");
        RitualRule rule;
        RitualRule *slot;
        int ritual, ok = 1;
        memset(&rule, 0, sizeof(rule));
        snprintf(where, sizeof(where), "rituals[%d]", i);
        if (!Mods_EntryUsed(mod, entry, where)) continue;
        ritual = card(mod, where, Json_Member(entry, "card"));
        if (!ritual) continue;
        /* It must be a ritual played as one: its effect (its own, its
         * base's for a copy, or the card "effect" names) a disc ritual's. A
         * card only typed Ritual does nothing when played. */
        if (Cards_Type(ritual) != CARD_TYPE_RITUAL || Cards_RetailType(Cards_EffectId(ritual)) != CARD_TYPE_RITUAL) {
            Mods_Note(mod, "%s: \"card\" must be a ritual card whose effect is a ritual's (a copy of one, or "
                           "\"effect\" naming one)", where);
            continue;
        }
        rule.recipe[0] = (unsigned short)ritual;
        if (result && Json_TypeOf(result) == JSON_NULL) {
            rule.removed = 1;
        } else {
            if (Json_Count(tributes) != DUEL_RITUAL_TRIBUTE_COUNT) {
                Mods_Note(mod, "%s: \"tributes\" names three monsters or requirement objects", where);
                continue;
            }
            for (j = 0; j < DUEL_RITUAL_TRIBUTE_COUNT && ok; j++) {
                const JsonValue *tribute = Json_At(tributes, j);
                ok = ritual_requirement(mod, where, tribute, &rule.requirements[j]);
                if (Json_TypeOf(tribute) == JSON_OBJECT) rule.conditional = 1;
                if (rule.requirements[j].card) rule.recipe[1 + j] = rule.requirements[j].card;
            }
            rule.recipe[4] = (unsigned short)(ok ? card(mod, where, result) : 0);
            if (!ok || !rule.recipe[4]) continue;
            if (!rule.conditional) {
                for (j = 0; j < DUEL_RITUAL_TRIBUTE_COUNT; j++)
                    if (!rule.recipe[1 + j]) ok = 0;
                if (!ok) continue;
            }
        }
        slot = grow(&rituals, &ritual_room, ritual_count, sizeof(*rituals));
        if (!slot) continue;
        *slot = rule;
        ritual_count++;
    }
}

int Tables_HasRitual(int ritual)
{
    int i;
    for (i = 0; i < ritual_count; i++)
        if (rituals[i].recipe[0] == ritual) return 1;
    return 0;
}

int Tables_Ritual(int ritual, unsigned short recipe[6])
{
    int i;
    for (i = ritual_count - 1; i >= 0; i--) {
        if (rituals[i].recipe[0] != ritual) continue;
        if (rituals[i].removed) return 0;
        if (rituals[i].conditional) return -1;
        memcpy(recipe, rituals[i].recipe, sizeof(rituals[i].recipe));
        return 1;
    }
    return -1;
}

int Tables_RitualRequirements(int ritual, TablesRitualRequirement requirements[3], unsigned short *result)
{
    int i;
    for (i = ritual_count - 1; i >= 0; i--) {
        if (rituals[i].recipe[0] != ritual) continue;
        if (rituals[i].removed || !rituals[i].conditional) return 0;
        memcpy(requirements, rituals[i].requirements, sizeof(rituals[i].requirements));
        if (result) *result = rituals[i].recipe[4];
        return 1;
    }
    return 0;
}

/* --- drops and decks ------------------------------------------------- */

/* --- the rank score (tables.h) --------------------------------------------
 *
 * One row of five threshold/change pairs per rule per duelist, over what the
 * disc's block holds. Kept flat and grown with the duelist list, as `edited`
 * is, so a duelist a mod added is named exactly as a retail one.
 */
static const char *const rank_rule_names[TABLES_RANK_RULE_COUNT] = {
    "turns", "effective attacks", "defensive wins", "face-down plays", "pure magic",
    "traps triggered", "cards used", "remaining lp", "initiate fusion", "equip magic",
};

#define RANK_ROW_VALUES (TABLES_RANK_STEPS * 2)

static short *rank_rows;              /* [duelist][rule][step][threshold, change] */
static unsigned char *rank_given;     /* [duelist][rule]: this row was written */
static int rank_room;

static int rank_room_for(int duelist)
{
    if (duelist < 0) return 0;
    if (duelist >= rank_room) {
        const int room = Duelists_Count() > duelist + 1 ? Duelists_Count() : duelist + 1;
        short *rows = realloc(rank_rows, (size_t)room * TABLES_RANK_RULE_COUNT * RANK_ROW_VALUES * sizeof(*rows));
        unsigned char *given = realloc(rank_given, (size_t)room * TABLES_RANK_RULE_COUNT);
        if (rows) rank_rows = rows;
        if (given) rank_given = given;
        if (!rows || !given) return 0;
        memset(rank_rows + (size_t)rank_room * TABLES_RANK_RULE_COUNT * RANK_ROW_VALUES, 0,
               (size_t)(room - rank_room) * TABLES_RANK_RULE_COUNT * RANK_ROW_VALUES * sizeof(*rank_rows));
        memset(rank_given + (size_t)rank_room * TABLES_RANK_RULE_COUNT, 0,
               (size_t)(room - rank_room) * TABLES_RANK_RULE_COUNT);
        rank_room = room;
    }
    return 1;
}

int Tables_RankNamed(const char *text)
{
    int rule;
    if (!text) return -1;
    for (rule = 0; rule < TABLES_RANK_RULE_COUNT; rule++) {
        if (same_letters(text, rank_rule_names[rule])) return rule;
    }
    return -1;
}



void Tables_SetRank(int duelist, int rule, const short *row)
{
    short *at;
    if (duelist < 0 || rule < 0 || rule >= TABLES_RANK_RULE_COUNT || !row) return;
    if (!rank_room_for(duelist)) return;
    at = rank_rows + ((size_t)duelist * TABLES_RANK_RULE_COUNT + rule) * RANK_ROW_VALUES;
    memcpy(at, row, RANK_ROW_VALUES * sizeof(*at));
    at[(TABLES_RANK_STEPS - 1) * 2] = 0x7FFF;
    rank_given[(size_t)duelist * TABLES_RANK_RULE_COUNT + rule] = 1;
}

const short *Tables_RankFor(int duelist, int rule)
{
    if (!Duelists_Valid(duelist) || rule < 0 || rule >= TABLES_RANK_RULE_COUNT) return NULL;
    if (!rank_rows || duelist >= rank_room) return NULL;
    if (!rank_given[(size_t)duelist * TABLES_RANK_RULE_COUNT + rule]) return NULL;
    return rank_rows + ((size_t)duelist * TABLES_RANK_RULE_COUNT + rule) * RANK_ROW_VALUES;
}

const short *Tables_Rank(int rule)
{
    return Tables_RankFor(Tables_OpponentId(), rule);
}

static int pool_named(const char *text)
{
    int pool;
    for (pool = TABLES_POOL_POW; pool < TABLES_POOL_COUNT; pool++) {
        if (same_letters(text, pool_names[pool]) || same_letters(text, pool_long_names[pool])) return pool;
    }
    return -1;
}

/* One pool of one or every opponent: { card: weight, ..., "replace": true }. */
static void read_pool(const char *mod, const char *where, int duelist, int pool, const JsonValue *cards)
{
    PoolEdit edit;
    int i, n = 0, first, last;
    if (Json_TypeOf(cards) != JSON_OBJECT) {
        Mods_Note(mod, "%s: a pool is an object of cards and their weights", where);
        return;
    }
    memset(&edit, 0, sizeof(edit));
    edit.mod = mod;
    edit.pool = (unsigned char)pool;
    edit.replace = (unsigned char)Json_Bool(Json_Member(cards, "replace"), 0);
    edit.cards = calloc((size_t)Json_Count(cards) + 1, sizeof(unsigned short));
    edit.weights = calloc((size_t)Json_Count(cards) + 1, sizeof(unsigned short));
    if (!edit.cards || !edit.weights) {
        free(edit.cards);
        free(edit.weights);
        return;
    }
    for (i = 0; i < Json_Count(cards); i++) {
        const JsonValue *member = Json_At(cards, i);
        const char *name = Json_Name(member);
        long weight;
        int id;
        char at[160];
        if (!strcmp(name, "replace") || !strcmp(name, "fixed")) continue;   /* "fixed": false */
        snprintf(at, sizeof(at), "%s \"%s\"", where, name);
        id = Cards_Named(name);
        if (id <= 0) {
            Mods_Note(mod, "%s: no such card", at);
            continue;
        }
        weight = Json_Number(member, -1);
        if (Json_TypeOf(member) != JSON_NUMBER || weight < 0) {
            Mods_Note(mod, "%s: a weight is a whole number, 0 or more", at);
            continue;
        }
        edit.cards[n] = (unsigned short)id;
        edit.weights[n++] = (unsigned short)(weight > 0xFFFF ? 0xFFFF : weight);
    }
    edit.count = n;
    if (!n && !edit.replace) {
        free(edit.cards);
        free(edit.weights);
        return;
    }
    first = duelist < 0 ? 0 : duelist;
    last = duelist < 0 ? Duelists_Count() - 1 : duelist;
    for (i = first; i <= last; i++) {
        PoolEdit *slot = grow(&edits, &edit_room, edit_count, sizeof(*edits));
        if (!slot) break;
        *slot = edit;
        slot->duelist = i;
        edit_count++;
        mark_edited(i, pool);
    }
    /* The edit's lists are shared by the opponents it names, and live on. */
}

/* A fixed deck: { "fixed": true, card: copies, ... }, the copies adding up
 * to the 40 cards of a deck. The counts are the deck, so the three-copy limit
 * of a dealt deck does not apply: the author wrote down every copy, and a
 * limit could only refuse the list or change it behind their back. */
typedef struct {
    int id, copies;
} Copies;

static int by_id(const void *left, const void *right)
{
    return ((const Copies *)left)->id - ((const Copies *)right)->id;
}

static void read_fixed_deck(const char *mod, const char *where, int duelist, const JsonValue *cards)
{
    Copies *list;
    FixedDeck deck;
    int i, n = 0, dealt = 0, total = 0, first, last;
    if (Json_TypeOf(cards) != JSON_OBJECT) {
        Mods_Note(mod, "%s: a deck is an object of cards and their copies", where);
        return;
    }
    list = calloc((size_t)Json_Count(cards) + 1, sizeof(*list));
    if (!list) return;
    for (i = 0; i < Json_Count(cards); i++) {
        const JsonValue *member = Json_At(cards, i);
        const char *name = Json_Name(member);
        long copies;
        int id;
        char at[160];
        if (!strcmp(name, "fixed") || !strcmp(name, "replace")) continue;
        snprintf(at, sizeof(at), "%s \"%s\"", where, name);
        id = Cards_Named(name);
        if (id <= 0) {
            Mods_Note(mod, "%s: no such card", at);
            continue;
        }
        copies = Json_Number(member, -1);
        if (Json_TypeOf(member) != JSON_NUMBER || copies < 0 || copies > DECK_SIZE) {
            Mods_Note(mod, "%s: a fixed deck gives each card its copies, 0 to %d", at, DECK_SIZE);
            continue;
        }
        total += (int)copies;
        if (!copies) continue;
        list[n].id = id;
        list[n++].copies = (int)copies;
    }
    if (total != DECK_SIZE) {
        Mods_Note(mod, "%s: a fixed deck is %d cards, and this one has %d; left out", where, DECK_SIZE, total);
        free(list);
        return;
    }
    /* In id order, as the game's own pools are read; the duel shuffles it. */
    qsort(list, (size_t)n, sizeof(*list), by_id);
    memset(&deck, 0, sizeof(deck));
    deck.mod = mod;
    for (i = 0; i < n; i++) {
        int copy;
        for (copy = 0; copy < list[i].copies; copy++) deck.cards[dealt++] = (unsigned short)list[i].id;
    }
    free(list);
    /* "all" is every duelist this run has, the added ones included; a slot
     * nothing was placed in is no duelist and is passed over. */
    first = duelist < 0 ? 0 : duelist;
    last = duelist < 0 ? Duelists_Count() - 1 : duelist;
    for (i = first; i <= last; i++) {
        FixedDeck *slot;
        if (duelist < 0 && !Duelists_Valid(i)) continue;
        slot = grow(&fixed_decks, &fixed_room, fixed_count, sizeof(*fixed_decks));
        if (!slot) break;
        *slot = deck;
        slot->duelist = i;
        fixed_count++;
    }
}

int Tables_FixedDeck(int duelist, unsigned short cards[TABLES_DECK_SIZE])
{
    const FixedDeck *deck = NULL;
    int i;
    if (!Duelists_Valid(duelist)) return 0;
    for (i = fixed_count - 1; i >= 0 && !deck; i--) {   /* the latest */
        if (fixed_decks[i].duelist == duelist) deck = &fixed_decks[i];
    }
    if (!deck) return 0;
    for (i = 0; i < edit_count; i++) {
        /* A fixed deck is the whole deck: the weighted edits of it wait. */
        PoolEdit *edit = &edits[i];
        if (edit->duelist != duelist || edit->pool != TABLES_POOL_DECK || edit->waiting) continue;
        edit->waiting = 1;
        Mods_Note(edit->mod, "%s's deck: left as it is; %s fixes it", Duelists_Name(duelist), deck->mod);
    }
    memcpy(cards, deck->cards, sizeof(deck->cards));
    if (Log_Wanted(LOG_MODS)) {
        char text[DECK_SIZE * 12];
        int at = 0;
        for (i = 0; i < DECK_SIZE; i++) {
            int copies = 1;
            while (i + 1 < DECK_SIZE && deck->cards[i + 1] == deck->cards[i]) i++, copies++;
            at += snprintf(text + at, sizeof(text) - (size_t)at, " %dx%d", copies, deck->cards[i]);
        }
        LOG(LOG_MODS, "tables: %s's deck fixed by %s (copies x card):%s", Duelists_Name(duelist), deck->mod, text);
    }
    return 1;
}

/* A "decks" entry: a pool of weights, unless "fixed" says it is the whole deck.
 * Both ways of writing a roster come through here, so decks/<id>.json reads
 * exactly as the same object written into the manifest does. */
static void read_deck_entry(const char *mod, const char *where, int duelist, const JsonValue *entry)
{
    const JsonValue *fixed = Json_Member(entry, "fixed");
    if (fixed && Json_TypeOf(fixed) != JSON_BOOL && Json_TypeOf(fixed) != JSON_NUMBER) {
        /* "true" in quotes: neither deck is what the mod meant. */
        Mods_Note(mod, "%s: \"fixed\" is true or false, without quotes; left out", where);
        return;
    }
    if (Json_Bool(fixed, 0)) read_fixed_deck(mod, where, duelist, entry);
    else read_pool(mod, where, duelist, TABLES_POOL_DECK, entry);
}

/* "drops": { opponent: { pool: { card: weight } } }, and
 * "decks": { opponent: { card: weight } } or { "fixed": true, card: copies }. */
static void read_pool_table(const char *mod, const JsonValue *table, int decks)
{
    int i, j;
    char where[128];
    if (table && Json_TypeOf(table) != JSON_OBJECT) {
        Mods_Note(mod, "\"%s\" is an object of opponents", decks ? "decks" : "drops");
        return;
    }
    for (i = 0; i < Json_Count(table); i++) {
        const JsonValue *entry = Json_At(table, i);
        const char *name = Json_Name(entry);
        int duelist = same_letters(name, "all") ? -1 : Duelists_Named(name);
        if (duelist == -1 && !same_letters(name, "all")) {
            Mods_Note(mod, "%s: no opponent \"%s\"", decks ? "decks" : "drops", name);
            continue;
        }
        if (decks) {
            snprintf(where, sizeof(where), "decks \"%s\"", name);
            read_deck_entry(mod, where, duelist, entry);
            continue;
        }
        if (Json_TypeOf(entry) != JSON_OBJECT) {
            Mods_Note(mod, "drops \"%s\": an object of pools (pow, bcd, tec)", name);
            continue;
        }
        for (j = 0; j < Json_Count(entry); j++) {
            const JsonValue *pool = Json_At(entry, j);
            int which = pool_named(Json_Name(pool));
            snprintf(where, sizeof(where), "drops \"%s\" \"%s\"", name, Json_Name(pool));
            if (which < 0) {
                Mods_Note(mod, "%s: the pools are pow, bcd and tec", where);
                continue;
            }
            read_pool(mod, where, duelist, which, pool);
        }
    }
}

/* Scale the chosen weights so they add up to `target` exactly: each gets
 * its share rounded down, and what that leaves goes to the largest
 * remainders (the lower id first between equals). */
typedef struct {
    unsigned remainder;
    int id;
} Share;

/* "drops" and "decks" may be a file of the mod's instead of an object written
 * out in the manifest: a roster of any size puts its pools in files beside it
 * rather than in one manifest nobody can read. The file holds exactly what the
 * key would have held. */
static const char *pool_directory;

static void read_pools(const char *mod, const JsonValue *table, int decks)
{
    const char *key = decks ? "decks" : "drops";
    JsonDocument *document = NULL;

    if (Json_TypeOf(table) == JSON_STRING) {
        const char *relative = Json_String(table, NULL);
        char path[1024], error[256];
        if (!pool_directory) {
            Mods_Note(mod, "\"%s\": a file cannot be read from here", key);
            return;
        }
        if (!relative || !Paths_Contained(relative)) {
            Mods_Note(mod, "\"%s\": %s is not a path inside the mod", key,
                      relative ? relative : "(nothing)");
            return;
        }
        if (snprintf(path, sizeof path, "%s/%s", pool_directory, relative) >= (int)sizeof path) {
            Mods_Note(mod, "\"%s\": %s is too long a path", key, relative);
            return;
        }
        document = Json_ParseFile(path, error, sizeof error);
        if (!document) {
            Mods_Note(mod, "\"%s\": %s: %s", key, relative, error);
            return;
        }
        table = Json_Root(document);
    }
    read_pool_table(mod, table, decks);
    /* Everything read is copied into the edits, so the document goes now. */
    Json_Free(document);
}

/* --- a folder of pools ----------------------------------------------------
 *
 * One duelist to a file, the file's own name naming the duelist, beside the
 * folder of duelists (pc/free_duel/duelists.h): "decks/<id>.json" holds what
 * a "decks" entry for it holds, "drops/<id>.json" what a "drops" entry does.
 * A duelist with no file of either keeps what it copies, which is what having
 * no edit already meant.
 *
 * Sorted, so two pools read in the same order on every machine.
 */
#define POOL_NAMES 256

typedef struct {
    char name[64];
} PoolName;

static int pool_name_order(const void *a, const void *b)
{
    return strcmp(((const PoolName *)a)->name, ((const PoolName *)b)->name);
}

static void read_pool_folder(const char *mod, const char *directory, int decks)
{
    const char *key = decks ? "decks" : "drops";
    PoolName names[POOL_NAMES];
    char path[1024];
    int count = 0, i, j;
    DIR *folder;
    struct dirent *item;

    if (!directory || snprintf(path, sizeof path, "%s/%s", directory, key) >= (int)sizeof path) return;
    folder = opendir(path);
    if (!folder) return;
    while ((item = readdir(folder)) != NULL && count < POOL_NAMES) {
        const size_t length = strlen(item->d_name);
        if (length < 6 || strcmp(item->d_name + length - 5, ".json") || length - 5 >= sizeof names[0].name) continue;
        memcpy(names[count].name, item->d_name, length - 5);
        names[count].name[length - 5] = '\0';
        count++;
    }
    closedir(folder);
    qsort(names, (size_t)count, sizeof *names, pool_name_order);

    for (i = 0; i < count; i++) {
        char file[1200], error[256], where[192];
        JsonDocument *document;
        const JsonValue *root;
        char identity[192];
        int duelist;
        /* The file's name is the id the mod's own duelists/<id>.json gave it,
         * so the four folders are keyed alike; failing that, a name as any
         * other entry names an opponent, which is how a stock duelist or
         * another mod's is reached. */
        duelist = snprintf(identity, sizeof identity, "%s:%s", mod, names[i].name) < (int)sizeof identity
                      ? Duelists_Find(identity)
                      : -1;
        if (duelist < 0) duelist = same_letters(names[i].name, "all") ? -1 : Duelists_Named(names[i].name);
        if (duelist == -1 && !same_letters(names[i].name, "all")) {
            Mods_Note(mod, "%s/%s.json: no opponent of that name", key, names[i].name);
            continue;
        }
        if (snprintf(file, sizeof file, "%s/%s.json", path, names[i].name) >= (int)sizeof file) continue;
        document = Json_ParseFile(file, error, sizeof error);
        if (!document) {
            Mods_Note(mod, "%s/%s.json: %s", key, names[i].name, error);
            continue;
        }
        root = Json_Root(document);
        if (snprintf(where, sizeof where, "%s/%s.json", key, names[i].name) < 0) where[0] = '\0';
        if (decks) {
            read_deck_entry(mod, where, duelist, root);
        } else if (Json_TypeOf(root) != JSON_OBJECT) {
            Mods_Note(mod, "%s: an object of pools (pow, bcd, tec)", where);
        } else {
            for (j = 0; j < Json_Count(root); j++) {
                const JsonValue *pool = Json_At(root, j);
                const int which = pool_named(Json_Name(pool));
                char at[224];
                snprintf(at, sizeof at, "%s \"%s\"", where, Json_Name(pool));
                if (which < 0) Mods_Note(mod, "%s: the pools are pow, bcd and tec", at);
                else read_pool(mod, at, duelist, which, pool);
            }
        }
        Json_Free(document);
    }
}


static int by_remainder(const void *left, const void *right)
{
    const Share *a = left, *b = right;
    if (a->remainder != b->remainder) return a->remainder > b->remainder ? -1 : 1;
    return a->id - b->id;
}

static Share *shares;
static int share_room;

int Tables_Scale(unsigned *weights, const unsigned char *chosen, int count, unsigned target)
{
    unsigned long long sum = 0;
    unsigned given = 0;
    int id, n = 0;
    for (id = 1; id <= count; id++) if (chosen[id]) sum += weights[id];
    if (!sum) return target == 0;
    if (count + 1 > share_room) {
        Share *bigger = realloc(shares, (size_t)(count + 1) * sizeof(*shares));
        if (!bigger) return 0;
        shares = bigger;
        share_room = count + 1;
    }
    for (id = 1; id <= count; id++) {
        unsigned long long part;
        if (!chosen[id] || !weights[id]) continue;
        part = (unsigned long long)weights[id] * target;
        weights[id] = (unsigned)(part / sum);
        given += weights[id];
        shares[n].remainder = (unsigned)(part % sum);
        shares[n++].id = id;
    }
    qsort(shares, (size_t)n, sizeof(*shares), by_remainder);
    for (id = 0; given < target && id < n; id++, given++) weights[shares[id].id]++;
    return 1;
}

/* One mod's edit of a pool, over what it was; 0 (and the pool as it was)
 * when it leaves a pool the game cannot draw from. */
static int apply(const PoolEdit *edit, unsigned *weights, unsigned *before, unsigned char *listed, int count)
{
    unsigned long long given = 0, rest = 0;
    int i, id, cards = 0;
    memcpy(before, weights, (size_t)(count + 1) * sizeof(*weights));
    memset(listed, 0, (size_t)(count + 1));
    if (edit->replace) memset(weights, 0, (size_t)(count + 1) * sizeof(*weights));
    for (i = 0; i < edit->count; i++) {
        if (edit->cards[i] > count) continue;   /* not this run's (the tests) */
        weights[edit->cards[i]] = edit->weights[i];
        listed[edit->cards[i]] = 1;
    }
    for (id = 1; id <= count; id++) {
        if (listed[id]) given += weights[id];
        else rest += weights[id];
    }
    if (given >= POOL_TOTAL || !rest) {
        /* The listed cards are the pool, in proportion. */
        for (id = 1; id <= count; id++) if (!listed[id]) weights[id] = 0;
        if (!Tables_Scale(weights, listed, count, POOL_TOTAL) || !given) goto refuse;
    } else {
        /* The listed cards have their weights; the rest share what is left. */
        for (id = 1; id <= count; id++) listed[id] = !listed[id];
        if (!Tables_Scale(weights, listed, count, POOL_TOTAL - (unsigned)given)) goto refuse;
    }
    for (id = 1; id <= count; id++) cards += weights[id] != 0;
    if (edit->pool == TABLES_POOL_DECK && cards < DECK_POOL_MIN_CARDS) goto refuse;
    return 1;
refuse:
    memcpy(weights, before, (size_t)(count + 1) * sizeof(*weights));
    return 0;
}

/* The pools worked out last, one per kind: an opponent draws from the same
 * one forty times, and a duel's drop once. */
typedef struct {
    int duelist, count;
    unsigned checksum;
    unsigned short *weights;
} PoolCache;
static PoolCache caches[TABLES_POOL_COUNT];
static unsigned *work, *spare;
static unsigned char *marks;
static int work_room;

const unsigned short *Tables_PoolFor(int duelist, int pool, const unsigned short *retail)
{
    PoolCache *cache;
    unsigned checksum = 2166136261u;
    int count = gCard_nCount, id, i;
    if (!Duelists_Valid(duelist) || pool < 0 || pool >= TABLES_POOL_COUNT) return NULL;
    if (!is_edited(duelist, pool) || !retail) return NULL;
    for (id = 0; id < CARD_COUNT; id++) checksum = (checksum ^ retail[id]) * 16777619u;
    cache = &caches[pool];
    if (cache->weights && cache->duelist == duelist && cache->count == count && cache->checksum == checksum)
        return cache->weights;
    if (count + 1 > work_room) {
        unsigned *a = realloc(work, (size_t)(count + 1) * sizeof(*work));
        unsigned *b = a ? realloc(spare, (size_t)(count + 1) * sizeof(*spare)) : NULL;
        unsigned char *c = b ? realloc(marks, (size_t)(count + 1)) : NULL;
        if (a) work = a;
        if (b) spare = b;
        if (!c) return NULL;
        marks = c;
        work_room = count + 1;
    }
    if (cache->count != count || !cache->weights) {
        unsigned short *bigger = realloc(cache->weights, (size_t)(count + 1) * sizeof(*cache->weights));
        if (!bigger) return NULL;
        cache->weights = bigger;
    }
    memset(work, 0, (size_t)(count + 1) * sizeof(*work));
    for (id = 1; id <= CARD_COUNT && id <= count; id++) work[id] = retail[id - 1];
    for (i = 0; i < edit_count; i++) {
        PoolEdit *edit = &edits[i];
        if (edit->duelist != duelist || edit->pool != pool) continue;
        if (!apply(edit, work, spare, marks, count) && !edit->warned) {
            edit->warned = 1;
            if (pool) Mods_Note(edit->mod, "%s's %s drops: left as they were; no card would be left to win",
                                Duelists_Name(duelist), pool_names[pool]);
            else Mods_Note(edit->mod, "%s's deck: left as it was; a deck is dealt from at least %d cards",
                           Duelists_Name(duelist), DECK_POOL_MIN_CARDS);
        }
    }
    {
        unsigned total = 0;
        for (id = 1; id <= count; id++) total += work[id];
        /* A pool the disc (or a data mod) left short is the game's affair,
         * but an empty one would stall the draw: keep the game's own. */
        if (!total) return NULL;
    }
    for (id = 0; id <= count; id++) cache->weights[id] = (unsigned short)(work[id] > 0xFFFF ? 0xFFFF : work[id]);
    if (Log_Wanted(LOG_MODS)) {
        int cards = 0, heaviest = 0;
        for (id = 1; id <= count; id++) {
            cards += work[id] != 0;
            if (work[id] > work[heaviest]) heaviest = id;
        }
        LOG(LOG_MODS, "tables: %s's %s as the mods have it: %d cards, the likeliest %d (%u/%d)",
            Duelists_Name(duelist), pool_names[pool], cards, heaviest, work[heaviest], POOL_TOTAL);
    }
    cache->duelist = duelist;
    cache->count = count;
    cache->checksum = checksum;
    return cache->weights;
}

const unsigned short *Tables_Pool(int pool, const unsigned short *retail)
{
    return Tables_PoolFor(gDuel_bOpponentID, pool, retail);
}

/* --- terrains ------------------------------------------------------- */

/* The terrains by gDuel_bTerrain's value, 1-6, as the field cards name them,
 * and the English words for the last three. */
static const char *const terrain_names[TERRAINS][2] = {
    {"Forest", NULL}, {"Wasteland", NULL}, {"Mountain", NULL}, {"Sogen", "Meadow"}, {"Umi", "Sea"}, {"Yami", "Dark"}};

static int terrain_named(const char *text)
{
    int terrain;
    if (strspn(text, "0123456789") == strlen(text) && *text) {
        terrain = atoi(text);
        return terrain >= 1 && terrain <= TERRAINS ? terrain : -1;
    }
    for (terrain = 0; terrain < TERRAINS; terrain++) {
        if (same_letters(text, terrain_names[terrain][0]) ||
            (terrain_names[terrain][1] && same_letters(text, terrain_names[terrain][1])))
            return terrain + 1;
    }
    return -1;
}

/* "terrain_bonus": { terrain: { type: points }, "replace": true }. A listed
 * pair's bonus is its points, with a sign, in place of the disc's +-500;
 * "replace" gives every pair the mod does not list none at all. */
static void read_terrain_bonus(const char *mod, const JsonValue *table)
{
    int i, j;
    char where[128];
    if (!table) return;
    if (Json_TypeOf(table) != JSON_OBJECT) {
        Mods_Note(mod, "\"terrain_bonus\" is an object of terrains (Forest, Wasteland, Mountain, Sogen, Umi, Yami)");
        return;
    }
    if (Json_Bool(Json_Member(table, "replace"), 0)) {
        memset(terrain_bonus, 0, sizeof(terrain_bonus));
        memset(terrain_listed, 1, sizeof(terrain_listed));
    }
    for (i = 0; i < Json_Count(table); i++) {
        const JsonValue *entry = Json_At(table, i);
        const char *name = Json_Name(entry);
        int terrain;
        if (!strcmp(name, "replace")) continue;
        snprintf(where, sizeof(where), "terrain_bonus \"%s\"", name);
        terrain = terrain_named(name);
        if (terrain < 0) {
            Mods_Note(mod, "%s: the terrains are Forest, Wasteland, Mountain, Sogen, Umi and Yami", where);
            continue;
        }
        if (Json_TypeOf(entry) != JSON_OBJECT) {
            Mods_Note(mod, "%s: an object of monster types and their bonus", where);
            continue;
        }
        for (j = 0; j < Json_Count(entry); j++) {
            const JsonValue *member = Json_At(entry, j);
            int type = Cards_TypeNamed(Json_Name(member));
            long points = Json_Number(member, 0);
            if (type < 0 || type >= CARD_TYPE_MAGIC) {
                Mods_Note(mod, "%s \"%s\": not a monster type", where, Json_Name(member));
                continue;
            }
            /* A 16-bit stat's range; "limits" decides the cap the sum meets. */
            if (Json_TypeOf(member) != JSON_NUMBER || points < -TABLES_LIMIT_STAT_MAX ||
                points > TABLES_LIMIT_STAT_MAX) {
                Mods_Note(mod, "%s \"%s\": a bonus is a whole number of points, -%d to %d", where, Json_Name(member),
                          TABLES_LIMIT_STAT_MAX, TABLES_LIMIT_STAT_MAX);
                continue;
            }
            terrain_bonus[terrain - 1][type] = (short)points;
            terrain_listed[terrain - 1][type] = 1;
        }
    }
}

int Tables_TerrainBonus(int terrain, int type, int *bonus)
{
    if (terrain < 1 || terrain > TERRAINS || type < 0 || type >= CARD_TYPE_MAGIC) return 0;
    if (!terrain_listed[terrain - 1][type]) return 0;
    *bonus = terrain_bonus[terrain - 1][type];
    return 1;
}

/* --- attack traps --------------------------------------------------- */

#define TRAP_THRESHOLD_MAX 65535L

static const long retail_thresholds[DUEL_ATTACK_TRAP_COUNT] = {
    DUEL_HOUSE_OF_ADHESIVE_TAPE_ATTACK_THRESHOLD, DUEL_EATGABOON_ATTACK_THRESHOLD, DUEL_BEAR_TRAP_ATTACK_THRESHOLD,
    DUEL_INVISIBLE_WIRE_ATTACK_THRESHOLD, DUEL_ACID_TRAP_HOLE_ATTACK_THRESHOLD, DUEL_WIDESPREAD_RUIN_ATTACK_THRESHOLD};
static const char *const trap_names[DUEL_ATTACK_TRAP_COUNT] = {
    "House of Adhesive Tape", "Eatgaboon", "Bear Trap", "Invisible Wire", "Acid Trap Hole", "Widespread Ruin"};

/* "trap_thresholds": { trap: points }, the attack at or under which each of
 * the six attack traps springs, in place of the disc's 500 to 3000. */
static void read_trap_thresholds(const char *mod, const JsonValue *table)
{
    int i;
    char where[128];
    if (!table) return;
    if (Json_TypeOf(table) != JSON_OBJECT) {
        Mods_Note(mod, "\"trap_thresholds\" is an object of attack traps and the attack each stops");
        return;
    }
    for (i = 0; i < Json_Count(table); i++) {
        const JsonValue *member = Json_At(table, i);
        const char *name = Json_Name(member);
        long points = Json_Number(member, -1);
        int id = Cards_Named(name), trap;
        snprintf(where, sizeof(where), "trap_thresholds \"%s\"", name);
        trap = id > 0 ? Cards_BaseId(id) - DUEL_ATTACK_TRAP_FIRST_CARD_ID : -1;
        if (trap < 0 || trap >= DUEL_ATTACK_TRAP_COUNT) {
            Mods_Note(mod, "%s: not one of the attack traps (House of Adhesive Tape, Eatgaboon, Bear Trap, "
                      "Invisible Wire, Acid Trap Hole, Widespread Ruin)", where);
            continue;
        }
        if (Json_TypeOf(member) != JSON_NUMBER || points < 0 || points > TRAP_THRESHOLD_MAX) {
            Mods_Note(mod, "%s: a threshold is a whole number of points, 0 to %ld", where, TRAP_THRESHOLD_MAX);
            continue;
        }
        trap_threshold[trap] = points;
        trap_listed[trap] = 1;
        trap_from[trap] = mod;
    }
    for (i = 1; i < DUEL_ATTACK_TRAP_COUNT; i++) {
        /* The duel looks from the strongest trap down and stops at the first
         * set one the attack is over: out of order, a weaker trap behind it
         * is never reached. The values may come from other mods, which the
         * note names. */
        int before = Tables_TrapThreshold(i - 1, retail_thresholds[i - 1]);
        int here = Tables_TrapThreshold(i, retail_thresholds[i]);
        if (here < before) {
            Mods_Note(mod, "trap_thresholds: out of order: %s at %d (%s) is under %s at %d (%s), the trap before it; "
                      "a trap behind a lower threshold never springs", trap_names[i], here,
                      trap_from[i] ? trap_from[i] : "the disc", trap_names[i - 1], before,
                      trap_from[i - 1] ? trap_from[i - 1] : "the disc");
            break;
        }
    }
}

int Tables_TrapThreshold(int trap, int retail)
{
    if (trap < 0 || trap >= DUEL_ATTACK_TRAP_COUNT || !trap_listed[trap]) return retail;
    return (int)trap_threshold[trap];
}

/* --- the chest ------------------------------------------------------ */

#define STARCHIP_MAX 999999L   /* SAVE_DATA_STARCHIP_MAX */

/* "chest_overflow": {"limit": n, "starchips": m}: the chest keeps at most n
 * copies of a card (1-255, a byte a card; the disc's 250 when left out), and
 * a card won when it already holds n is worth m starchips instead (0 when
 * left out). The latest mod that says wins; "limits": {"chest": n} sets the
 * same n. */
static void read_chest_overflow(const char *mod, const JsonValue *value)
{
    const JsonValue *limit = Json_Member(value, "limit"), *starchips = Json_Member(value, "starchips");
    long n = Json_Number(limit, CARD_CHEST_QUANTITY_MAX), m = Json_Number(starchips, 0);
    if (!value) return;
    if (Json_TypeOf(value) != JSON_OBJECT) {
        Mods_Note(mod, "\"chest_overflow\" is an object: {\"limit\": copies, \"starchips\": per card past it}");
        return;
    }
    if ((limit && Json_TypeOf(limit) != JSON_NUMBER) || n < 1 || n > TABLES_LIMIT_CHEST_MAX) {
        Mods_Note(mod, "chest_overflow: \"limit\" is a whole number of copies, 1 to %d (the chest keeps a byte a card); "
                  "left out", TABLES_LIMIT_CHEST_MAX);
        return;
    }
    if ((starchips && Json_TypeOf(starchips) != JSON_NUMBER) || m < 0 || m > STARCHIP_MAX) {
        Mods_Note(mod, "chest_overflow: \"starchips\" is a whole number, 0 to %ld; left out", STARCHIP_MAX);
        return;
    }
    chest_limit = (int)n;
    overflow_starchips = m;
}

int Tables_ChestLimit(void)
{
    return chest_limit ? chest_limit : CARD_CHEST_QUANTITY_MAX;
}

int Tables_ChestRoom(void)
{
    return chest_limit > CARD_CHEST_QUANTITY_MAX ? chest_limit : CARD_CHEST_QUANTITY_MAX;
}

int Tables_ChestFull(unsigned quantity)
{
    return chest_limit && quantity >= (unsigned)chest_limit;
}

int Tables_ChestOverflow(unsigned quantity, unsigned *starchips)
{
    unsigned long long total, cap = Tables_StarchipCap();
    if (!overflow_starchips || quantity < (unsigned)Tables_ChestLimit()) return 0;
    total = (unsigned long long)*starchips + (unsigned long long)overflow_starchips;
    /* Never below the balance: one past the cap (a mod's "limits" that is
       off now) stays. */
    if (cap < *starchips) cap = *starchips;
    if (total > cap) total = cap;
    LOG(LOG_MODS, "tables: a card past the chest's %d: %ld starchips, %u -> %u", Tables_ChestLimit(),
        overflow_starchips, *starchips, (unsigned)total);
    *starchips = (unsigned)total;
    return (int)overflow_starchips;
}

/* --- limits ----------------------------------------------------------
 *
 * "limits": {
 *     "stats": n, "attack": n, "defense": n,
 *     "life_points": n or {"start": n, "player": n, "opponent": n, "max": n,
 *                          "duelists": {duelist: n or {"player": n, "opponent": n}}},
 *     "two_player": {"start": n, "max": n, "step": n},
 *     "starchips": n, "chest": n, "free_duel_record": n
 * }
 *
 * 0 in the variables below is "no mod said"; each key's latest mod wins, and a
 * duelist's entries from two mods are kept side by side, the later one first
 * found. */

static int limit_stat[2];                  /* ATK, DEF */
static int limit_life[2];                  /* the player's, the opponent's start */
static int limit_life_max;
static int limit_two_player[3];            /* TABLES_TWO_PLAYER_* */
static long limit_starchips = -1;          /* -1: no mod said (0 is a cap of none) */
static int limit_record, limit_two_player_record;

typedef struct {
    int duelist;
    int life[2];                           /* 0: not given */
} DuelistLife;
static DuelistLife *duelist_lives;
static int duelist_life_count, duelist_life_room;

/* A whole number from `low` to `high` at `key` of `where`: 1 with it in *out.
 * One past `high` but still a number is held at `high` with a note that says
 * what keeping more would take; anything else is noted and left out. */
static int limit_number(const char *mod, const char *where, const JsonValue *value, long low, long high,
                        const char *storage, long *out)
{
    long number;
    if (!value) return 0;
    if (Json_TypeOf(value) != JSON_NUMBER) {
        Mods_Note(mod, "limits: %s is a whole number, %ld to %ld, without quotes; left out", where, low, high);
        return 0;
    }
    number = Json_Number(value, 0);
    if (number < low) {
        Mods_Note(mod, "limits: %s is at least %ld; left out", where, low);
        return 0;
    }
    if (number > high) {
        Mods_Note(mod, "limits: %s is %ld, past the %ld %s; %ld used", where, number, high, storage, high);
        number = high;
    }
    *out = number;
    return 1;
}

static DuelistLife *duelist_life(int duelist)
{
    DuelistLife *slot;
    int i;
    for (i = duelist_life_count - 1; i >= 0; i--) {
        if (duelist_lives[i].duelist == duelist) return &duelist_lives[i];
    }
    slot = grow(&duelist_lives, &duelist_life_room, duelist_life_count, sizeof(*duelist_lives));
    if (!slot) return NULL;
    memset(slot, 0, sizeof(*slot));
    slot->duelist = duelist;
    duelist_life_count++;
    return slot;
}

#define LIFE_STORAGE "the game keeps (a 16-bit number; more would mean widening each side's duel record)"

static void read_life_points(const char *mod, const JsonValue *value)
{
    static const char *const sides[2] = {"player", "opponent"};
    const JsonValue *duelists;
    char where[160];
    long n;
    int side, i;
    if (!value) return;
    if (Json_TypeOf(value) == JSON_NUMBER) {
        if (limit_number(mod, "life_points", value, 1, TABLES_LIMIT_LIFE_POINTS_MAX, LIFE_STORAGE, &n))
            limit_life[0] = limit_life[1] = (int)n;
        return;
    }
    if (Json_TypeOf(value) != JSON_OBJECT) {
        Mods_Note(mod, "limits: \"life_points\" is a number (both sides' start) or an object: {\"start\": 8000, "
                  "\"player\": 8000, \"opponent\": 8000, \"max\": 8000, \"duelists\": {\"Heishin\": 12000}}");
        return;
    }
    for (i = 0; i < Json_Count(value); i++) {
        const char *name = Json_Name(Json_At(value, i));
        if (strcmp(name, "start") && strcmp(name, "player") && strcmp(name, "opponent") && strcmp(name, "max") &&
            strcmp(name, "duelists"))
            Mods_Note(mod, "limits: life_points has no \"%s\" (start, player, opponent, max, duelists)", name);
    }
    if (limit_number(mod, "life_points \"start\"", Json_Member(value, "start"), 1, TABLES_LIMIT_LIFE_POINTS_MAX,
                     LIFE_STORAGE, &n))
        limit_life[0] = limit_life[1] = (int)n;
    for (side = 0; side < 2; side++) {
        snprintf(where, sizeof(where), "life_points \"%s\"", sides[side]);
        if (limit_number(mod, where, Json_Member(value, sides[side]), 1, TABLES_LIMIT_LIFE_POINTS_MAX, LIFE_STORAGE,
                         &n))
            limit_life[side] = (int)n;
    }
    if (limit_number(mod, "life_points \"max\"", Json_Member(value, "max"), 1, TABLES_LIMIT_LIFE_POINTS_MAX,
                     LIFE_STORAGE, &n))
        limit_life_max = (int)n;
    duelists = Json_Member(value, "duelists");
    if (duelists && Json_TypeOf(duelists) != JSON_OBJECT) {
        Mods_Note(mod, "limits: life_points \"duelists\" is an object of duelists and their LP");
        return;
    }
    for (i = 0; i < Json_Count(duelists); i++) {
        const JsonValue *entry = Json_At(duelists, i);
        const char *name = Json_Name(entry);
        int all = same_letters(name, "all"), duelist = all ? -1 : Duelists_Named(name), life[2] = {0, 0};
        if (!all && duelist < 0) {
            Mods_Note(mod, "limits: life_points \"duelists\": no duelist \"%s\"", name);
            continue;
        }
        if (Json_TypeOf(entry) == JSON_NUMBER) {
            /* A number is the duelist's own LP: the opponent's side. */
            snprintf(where, sizeof(where), "life_points \"duelists\" \"%s\"", name);
            if (limit_number(mod, where, entry, 1, TABLES_LIMIT_LIFE_POINTS_MAX, LIFE_STORAGE, &n)) life[1] = (int)n;
        } else if (Json_TypeOf(entry) == JSON_OBJECT) {
            for (side = 0; side < 2; side++) {
                snprintf(where, sizeof(where), "life_points \"duelists\" \"%s\" \"%s\"", name, sides[side]);
                if (limit_number(mod, where, Json_Member(entry, sides[side]), 1, TABLES_LIMIT_LIFE_POINTS_MAX,
                                 LIFE_STORAGE, &n))
                    life[side] = (int)n;
            }
        } else {
            Mods_Note(mod, "limits: life_points \"duelists\" \"%s\": a number (its LP) or {\"player\": n, "
                      "\"opponent\": n}", name);
            continue;
        }
        if (life[0] || life[1]) {
            DuelistLife *slot = duelist_life(all ? -2 : duelist);
            if (!slot) continue;
            for (side = 0; side < 2; side++)
                if (life[side]) slot->life[side] = life[side];
        }
    }
}

static void read_limits(const char *mod, const JsonValue *limits)
{
    static const char *const two_player_keys[3] = {"start", "max", "step"};
    const JsonValue *two_player;
    char where[64];
    long n;
    int i;
    if (!limits) return;
    if (Json_TypeOf(limits) != JSON_OBJECT) {
        Mods_Note(mod, "\"limits\" is an object: {\"stats\": 30000, \"life_points\": 16000, ...}");
        return;
    }
    for (i = 0; i < Json_Count(limits); i++) {
        static const char *const keys[] = {"stats", "attack", "defense", "life_points", "two_player", "starchips",
                                            "chest", "free_duel_record", "two_player_record"};
        const char *name = Json_Name(Json_At(limits, i));
        size_t k;
        for (k = 0; k < sizeof(keys) / sizeof(keys[0]) && strcmp(name, keys[k]); k++) {}
        if (k == sizeof(keys) / sizeof(keys[0]))
            Mods_Note(mod, "limits: no limit \"%s\" (stats, attack, defense, life_points, two_player, starchips, chest, "
                      "free_duel_record, two_player_record)", name);
    }
#define STAT_STORAGE "the game keeps (a 16-bit number; more would mean widening every card record)"
    if (limit_number(mod, "\"stats\"", Json_Member(limits, "stats"), 0, TABLES_LIMIT_STAT_MAX, STAT_STORAGE, &n))
        limit_stat[0] = limit_stat[1] = (int)n + 1;
    if (limit_number(mod, "\"attack\"", Json_Member(limits, "attack"), 0, TABLES_LIMIT_STAT_MAX, STAT_STORAGE, &n))
        limit_stat[0] = (int)n + 1;
    if (limit_number(mod, "\"defense\"", Json_Member(limits, "defense"), 0, TABLES_LIMIT_STAT_MAX, STAT_STORAGE, &n))
        limit_stat[1] = (int)n + 1;
#undef STAT_STORAGE
    read_life_points(mod, Json_Member(limits, "life_points"));
    two_player = Json_Member(limits, "two_player");
    if (two_player && Json_TypeOf(two_player) != JSON_OBJECT) {
        Mods_Note(mod, "limits: \"two_player\" is an object: {\"start\": 8000, \"max\": 8000, \"step\": 500}");
    } else {
        for (i = 0; i < Json_Count(two_player); i++) {
            const char *name = Json_Name(Json_At(two_player, i));
            if (strcmp(name, "start") && strcmp(name, "max") && strcmp(name, "step"))
                Mods_Note(mod, "limits: two_player has no \"%s\" (start, max, step)", name);
        }
        for (i = 0; i < 3; i++) {
            snprintf(where, sizeof(where), "two_player \"%s\"", two_player_keys[i]);
            if (limit_number(mod, where, Json_Member(two_player, two_player_keys[i]), 1, TABLES_LIMIT_LIFE_POINTS_MAX,
                             LIFE_STORAGE, &n))
                limit_two_player[i] = (int)n;
        }
    }
    if (limit_number(mod, "\"starchips\"", Json_Member(limits, "starchips"), 0, TABLES_LIMIT_STARCHIPS_MAX,
                     "the game shows (eight digits)", &n))
        limit_starchips = n;
    if (limit_number(mod, "\"chest\"", Json_Member(limits, "chest"), 1, TABLES_LIMIT_CHEST_MAX,
                     "copies the chest keeps (a byte a card; more would change the memory card's save)", &n))
        chest_limit = (int)n;
    if (limit_number(mod, "\"free_duel_record\"", Json_Member(limits, "free_duel_record"), 1, TABLES_LIMIT_RECORD_MAX,
                     "the save keeps (a 16-bit number)", &n))
        limit_record = (int)n;
    if (limit_number(mod, "\"two_player_record\"", Json_Member(limits, "two_player_record"), 1,
                     TABLES_LIMIT_TWO_PLAYER_RECORD_MAX, "the save keeps (a 16-bit number)", &n))
        limit_two_player_record = (int)n;
}

int Tables_StatCap(int defense)
{
    int set = limit_stat[defense ? 1 : 0];
    return set ? set - 1 : CARD_STAT_MAX;
}

int Tables_StatCapEither(void)
{
    int attack = Tables_StatCap(0), defense = Tables_StatCap(1);
    return attack > defense ? attack : defense;
}

int Tables_StartingLifePoints(int side, int duelist, int retail)
{
    int i, which = side ? 1 : 0;
    /* The duelist's own entry, then "all" of them; a two-player duel has no
     * duelist. */
    for (i = duelist_life_count - 1; duelist >= 0 && i >= 0; i--) {
        if (duelist_lives[i].duelist == duelist && duelist_lives[i].life[which]) return duelist_lives[i].life[which];
    }
    for (i = duelist_life_count - 1; duelist >= 0 && i >= 0; i--) {
        if (duelist_lives[i].duelist == -2 && duelist_lives[i].life[which]) return duelist_lives[i].life[which];
    }
    return limit_life[which] ? limit_life[which] : retail;
}

int Tables_MaxLifePoints(int start)
{
    return limit_life_max ? limit_life_max : start;
}

int Tables_TwoPlayerLifePoints(int which, int retail)
{
    int value;
    if (which < 0 || which > TABLES_TWO_PLAYER_STEP) return retail;
    value = limit_two_player[which] ? limit_two_player[which] : retail;
    if (which != TABLES_TWO_PLAYER_MAX) {
        /* A start or a step past the choice's top is the top, the disc's
         * start too when a mod only lowers the top. */
        int top = Tables_TwoPlayerLifePoints(TABLES_TWO_PLAYER_MAX, DUEL_STARTING_LIFE_POINTS);
        if (value > top) value = top;
    }
    return value;
}

unsigned Tables_StarchipCap(void)
{
    return limit_starchips >= 0 ? (unsigned)limit_starchips : (unsigned)STARCHIP_MAX;
}

int Tables_FreeDuelRecordCap(void)
{
    return limit_record ? limit_record : FREE_DUEL_RECORD_MAX;
}

int Tables_TwoPlayerRecordCap(void)
{
    return limit_two_player_record ? limit_two_player_record : 9999;   /* func_800218F0 */
}

long Tables_Limit(const char *name)
{
    if (!name) return -1;
    if (same_letters(name, "attack")) return Tables_StatCap(0);
    if (same_letters(name, "defense")) return Tables_StatCap(1);
    if (same_letters(name, "life_points")) return Tables_StartingLifePoints(0, -1, DUEL_STARTING_LIFE_POINTS);
    if (same_letters(name, "life_points_max")) return limit_life_max;
    if (same_letters(name, "two_player_start"))
        return Tables_TwoPlayerLifePoints(TABLES_TWO_PLAYER_START, DUEL_STARTING_LIFE_POINTS);
    if (same_letters(name, "two_player_max"))
        return Tables_TwoPlayerLifePoints(TABLES_TWO_PLAYER_MAX, DUEL_STARTING_LIFE_POINTS);
    if (same_letters(name, "two_player_step"))
        return Tables_TwoPlayerLifePoints(TABLES_TWO_PLAYER_STEP, DUEL_LIFE_POINT_SELECTION_STEP);
    if (same_letters(name, "starchips")) return (long)Tables_StarchipCap();
    if (same_letters(name, "chest")) return Tables_ChestLimit();
    if (same_letters(name, "free_duel_record")) return Tables_FreeDuelRecordCap();
    if (same_letters(name, "two_player_record")) return Tables_TwoPlayerRecordCap();
    return -1;
}

static int limits_set(void)
{
    return limit_stat[0] || limit_stat[1] || limit_life[0] || limit_life[1] || limit_life_max ||
           limit_two_player[0] || limit_two_player[1] || limit_two_player[2] || limit_starchips >= 0 ||
           limit_record || limit_two_player_record || duelist_life_count;
}

/* --- the Password screen ------------------------------------------- */

#define SHOP_PASSWORD 1     /* shop_password[id] is the card's */
#define SHOP_PRICE 2        /* shop_price[id] is the card's */
#define SHOP_PERCENT 4      /* shop_price[id] is a percent of the loaded record's */

static unsigned shop_password[CARD_TABLE_ID_END], shop_price[CARD_TABLE_ID_END];
static unsigned char shop_set[CARD_TABLE_ID_END];
static const char *shop_from[CARD_TABLE_ID_END];  /* the mod that set the password */
static int shop_count;

/* Eight decimal digits as the game keeps them, a digit a nibble. */
static unsigned password_digits(unsigned long number)
{
    unsigned packed = 0;
    int shift;
    for (shift = 0; shift < 32; shift += 4, number /= 10) packed |= (unsigned)(number % 10) << shift;
    return packed;
}

/* A "password": up to eight digits as a string ("00000001") or a number
 * (1), "card number" for the card's own number (2, `out` left alone), ""
 * or null for none (the screen cannot give the card). 0 when it is none
 * of those. */
static int read_shop_password(const JsonValue *value, unsigned *out)
{
    const char *text = Json_String(value, NULL);
    long number = Json_Number(value, -1);
    size_t length;
    if (Json_TypeOf(value) == JSON_NULL || (text && !*text)) {
        *out = CARD_PASSWORD_NONE;
        return 1;
    }
    if (Json_TypeOf(value) == JSON_NUMBER) {
        if (number < 0 || number > 99999999L) return 0;
        *out = password_digits((unsigned long)number);
        return 1;
    }
    if (!text) return 0;
    if (same_letters(text, "card number")) return 2;
    length = strlen(text);
    if (length > 8 || strspn(text, "0123456789") != length) return 0;
    *out = password_digits(strtoul(text, NULL, 10));
    return 1;
}

/* One entry as read once, "all" included: what it sets (SHOP_*), and
 * whether its password is each card's own number. */
typedef struct {
    unsigned char set, own;
    unsigned password, price;
} ShopEntry;

/* {"password": ..., "starchips": n} or {"starchips_percent": n}; a key it
 * gets wrong is noted and left out like one it leaves out. */
static void read_shop_entry(const char *mod, const char *where, const JsonValue *entry, ShopEntry *out)
{
    const JsonValue *password = Json_Member(entry, "password");
    const JsonValue *starchips = Json_Member(entry, "starchips");
    const JsonValue *percent = Json_Member(entry, "starchips_percent");
    long n;
    memset(out, 0, sizeof(*out));
    if (password) {
        int read = read_shop_password(password, &out->password);
        if (read) {
            out->set |= SHOP_PASSWORD;
            out->own = read == 2;
        } else {
            Mods_Note(mod, "%s: \"password\" is up to 8 digits (\"00000001\"), \"card number\", or \"\" for none", where);
        }
    }
    if (starchips && percent) Mods_Note(mod, "%s: \"starchips\" and \"starchips_percent\" both; \"starchips\" used", where);
    if (starchips) {
        n = Json_Number(starchips, -1);
        if (Json_TypeOf(starchips) != JSON_NUMBER || n < 0 || n > STARCHIP_MAX) {
            Mods_Note(mod, "%s: \"starchips\" is a whole number, 0 to %ld", where, STARCHIP_MAX);
        } else {
            out->price = (unsigned)n;
            out->set |= SHOP_PRICE;
        }
    } else if (percent) {
        n = Json_Number(percent, -1);
        if (Json_TypeOf(percent) != JSON_NUMBER || n < 0 || n > 1000) {
            Mods_Note(mod, "%s: \"starchips_percent\" is a whole number, 0 to 1000", where);
        } else {
            out->price = (unsigned)n;
            out->set |= SHOP_PERCENT;
        }
    }
}

/* Card `id` takes what the entry sets; what it leaves out stays. */
static void apply_shop_entry(const char *mod, const ShopEntry *entry, int id)
{
    if (entry->set & SHOP_PASSWORD) {
        shop_password[id] = entry->own ? password_digits((unsigned long)id) : entry->password;
        shop_set[id] |= SHOP_PASSWORD;
        shop_from[id] = mod;
    }
    if (entry->set & (SHOP_PRICE | SHOP_PERCENT)) {
        shop_price[id] = entry->price;
        shop_set[id] = (unsigned char)((shop_set[id] & ~(SHOP_PRICE | SHOP_PERCENT)) | (entry->set & (SHOP_PRICE | SHOP_PERCENT)));
    }
}

/* "passwords": { card: {"password": ..., "starchips": n} }, with "all" for
 * every loaded card; "all" goes first, so a card named beside it
 * keeps what its own entry says. */
static void read_passwords(const char *mod, const JsonValue *table)
{
    int i, id, pass;
    char where[128];
    ShopEntry shop;
    if (!table) return;
    if (Json_TypeOf(table) != JSON_OBJECT) {
        Mods_Note(mod, "\"passwords\" is an object: {\"Blue-eyes White Dragon\": {\"password\": \"00000001\", "
                  "\"starchips\": 100}}");
        return;
    }
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < Json_Count(table); i++) {
            const JsonValue *entry = Json_At(table, i);
            const char *name = Json_Name(entry);
            int all = same_letters(name, "all");
            if (all != (pass == 0)) continue;
            snprintf(where, sizeof(where), "passwords \"%s\"", name);
            if (Json_TypeOf(entry) != JSON_OBJECT) {
                Mods_Note(mod, "%s: an entry is {\"password\": \"00000001\", \"starchips\": 100}", where);
                continue;
            }
            if (all) {
                read_shop_entry(mod, where, entry, &shop);
                if (!shop.set) continue;
                for (id = 1; id <= gCard_nCount; id++) apply_shop_entry(mod, &shop, id);
                shop_count += gCard_nCount;
                continue;
            }
            id = Cards_Named(name);
            if (id <= 0) {
                Mods_Note(mod, "%s: no card by that name or number", where);
                continue;
            }
            read_shop_entry(mod, where, entry, &shop);
            if (!shop.set) continue;
            apply_shop_entry(mod, &shop, id);
            shop_count++;
        }
    }
}

typedef struct { unsigned password; int id; } ShopPassword;
static int compare_shop_password(const void *left, const void *right)
{
    const ShopPassword *a = left, *b = right;
    if (a->password != b->password) return a->password < b->password ? -1 : 1;
    return a->id - b->id;
}

int Tables_CheckPasswords(const unsigned *passwords)
{
    /* One note a mod: the first clash, and how many more ("all" giving
     * every card one password would otherwise make 721); the rest go to
     * the trace. */
    enum { NOTED_MAX = 64 };
    const char *noted[NOTED_MAX];
    int first_a[NOTED_MAX], first_b[NOTED_MAX], more[NOTED_MAX];
    static ShopPassword sorted[CARD_TABLE_ID_END];
    int a, b, i, n = 0, first = 0, row, count = 0, clashes = 0;
    for (i = 1; i <= gCard_nCount; i++) {
        if (passwords[i] != CARD_PASSWORD_NONE) sorted[n++] = (ShopPassword){passwords[i], i};
    }
    qsort(sorted, (size_t)n, sizeof(*sorted), compare_shop_password);
    for (row = 1; row < n; row++) {
        if (sorted[row].password != sorted[first].password) { first = row; continue; }
        a = sorted[first].id;
        b = sorted[row].id;
        {
            const char *mod;
            /* The note goes beside the mod that set the card left out,
             * else beside the one that set the card that wins. */
            mod = shop_from[b] ? shop_from[b] : shop_from[a];
            LOG(LOG_MODS, "tables: cards %d and %d both have password %08X; the screen gives card %d", a, b,
                passwords[b], a);
            if (mod) {
                for (i = 0; i < count && noted[i] != mod; i++) {}
                if (i < count) {
                    more[i]++;
                } else if (count < NOTED_MAX) {
                    noted[count] = mod;
                    first_a[count] = a;
                    first_b[count] = b;
                    more[count++] = 0;
                }
            }
            clashes++;
        }
    }
    for (i = 0; i < count; i++) {
        if (more[i]) {
            Mods_Note(noted[i], "passwords: cards %d and %d both have password %08X, and %d more cards share one; "
                      "the Password screen gives the lower card", first_a[i], first_b[i], passwords[first_b[i]],
                      more[i]);
        } else {
            Mods_Note(noted[i], "passwords: cards %d and %d both have password %08X; the Password screen gives card %d",
                      first_a[i], first_b[i], passwords[first_b[i]], first_a[i]);
        }
    }
    return clashes;
}

int Tables_PasswordShop(int id, unsigned *price, unsigned *password)
{
    int changed = 0;
    if (!Cards_Valid(id) || !shop_set[id]) return 0;
    if (shop_set[id] & SHOP_PASSWORD) {
        changed |= *password != shop_password[id];
        *password = shop_password[id];
    }
    if (shop_set[id] & (SHOP_PRICE | SHOP_PERCENT)) {
        unsigned long long value = shop_price[id];
        if (shop_set[id] & SHOP_PERCENT) {
            /* Rounded, and never down to free by rounding: a card that
             * cost something still costs a starchip, unless the percent
             * is 0 (free, as "starchips": 0; shop.c skips the count). */
            value = ((unsigned long long)*price * shop_price[id] + 50) / 100;
            if (!value && *price && shop_price[id]) value = 1;
            if (value > STARCHIP_MAX) value = STARCHIP_MAX;
        }
        changed |= *price != (unsigned)value;
        *price = (unsigned)value;
    }
    return changed;
}

/* --- building -------------------------------------------------------- */

static void forget_pools(void)
{
    int pool;
    for (pool = 0; pool < TABLES_POOL_COUNT; pool++) caches[pool].count = -1;
}

void Tables_AddFrom(const char *mod, const char *directory, const JsonValue *manifest)
{
    forget_pools();
    pool_directory = directory;
    read_fusions(mod, Json_Member(manifest, "fusions"));
    read_equips(mod, Json_Member(manifest, "equips"));
    read_equip_default(mod, Json_Member(manifest, "equip_bonus_default"));
    read_rituals(mod, Json_Member(manifest, "rituals"));
    read_pools(mod, Json_Member(manifest, "drops"), 0);
    read_pools(mod, Json_Member(manifest, "decks"), 1);
    read_chest_overflow(mod, Json_Member(manifest, "chest_overflow"));
    read_terrain_bonus(mod, Json_Member(manifest, "terrain_bonus"));
    read_trap_thresholds(mod, Json_Member(manifest, "trap_thresholds"));
    read_passwords(mod, Json_Member(manifest, "passwords"));
    read_limits(mod, Json_Member(manifest, "limits"));
    /* And a file to a duelist, beside the folder of duelists. */
    read_pool_folder(mod, directory, 0);
    read_pool_folder(mod, directory, 1);
    pool_directory = NULL;
}

/* Without a directory: a manifest whose pools are written out in it, which is
 * what the tests hand over. */
void Tables_Add(const char *mod, const JsonValue *manifest)
{
    Tables_AddFrom(mod, NULL, manifest);
}

void Tables_Clear(void)
{
    int i;
    /* Edits of "all" share their lists: free each list once. */
    for (i = 0; i < edit_count; i++) {
        int shared = 0, j;
        for (j = 0; j < i && !shared; j++) shared = edits[j].cards == edits[i].cards;
        if (!shared) {
            free(edits[i].cards);
            free(edits[i].weights);
        }
    }
    fusion_count = equip_count = ritual_count = edit_count = fixed_count = bonus_count = 0;
    overflow_starchips = chest_limit = 0;
    memset(shop_set, 0, sizeof(shop_set));
    memset(shop_from, 0, sizeof(shop_from));
    shop_count = 0;
    equip_default = 0;
    equip_default_set = 0;
    memset(limit_stat, 0, sizeof(limit_stat));
    memset(limit_life, 0, sizeof(limit_life));
    memset(limit_two_player, 0, sizeof(limit_two_player));
    limit_life_max = limit_record = limit_two_player_record = duelist_life_count = 0;
    limit_starchips = -1;
    memset(terrain_bonus, 0, sizeof(terrain_bonus));
    memset(terrain_listed, 0, sizeof(terrain_listed));
    memset(trap_listed, 0, sizeof(trap_listed));
    memset(trap_from, 0, sizeof(trap_from));
    if (removed_results) memset(removed_results, 0, (size_t)removed_room);
    if (edited) memset(edited, 0, (size_t)edited_room * TABLES_POOL_COUNT);
    if (rank_given) memset(rank_given, 0, (size_t)rank_room * TABLES_RANK_RULE_COUNT);
    forget_pools();
}

void Tables_Build(void)
{
    static int built;
    int i;
    if (built) return;
    built = 1;
    for (i = 0; i < Mods_LoadedCount(); i++) {
        int mod = Mods_Loaded(i);
        if (Mods_Active(mod)) Tables_AddFrom(Mods_Id(mod), Mods_Directory(mod), Mods_Manifest(mod));
    }
    /* The player's own folders, read last so their edits sit over the mods'
     * -- the same order two mods are settled in (pc/free_duel/duelists.h). */
    if (Paths_UserDir()) {
        read_pool_folder("user", Paths_UserDir(), 0);
        read_pool_folder("user", Paths_UserDir(), 1);
    }
    if (fusion_count || equip_count || bonus_count || equip_default_set || ritual_count || edit_count || fixed_count || overflow_starchips || chest_limit)
        LOG(LOG_MODS, "tables: %d fusion rules, %d equip rules, %d equip bonuses, %d rituals, %d pool edits, "
            "%d fixed decks, a chest of %d with %ld starchips a card past it", fusion_count, equip_count,
            bonus_count, ritual_count, edit_count, fixed_count, Tables_ChestLimit(), overflow_starchips);
    if (shop_count) LOG(LOG_MODS, "tables: %d Password screen entries", shop_count);
    if (limits_set())
        LOG(LOG_MODS, "tables: limits: ATK %d, DEF %d, LP %d/%d (max %d, %d duelists), two-player %d-%d by %d, "
            "starchips %u, chest %d, Free Duel record %d", Tables_StatCap(0), Tables_StatCap(1),
            Tables_StartingLifePoints(0, -1, DUEL_STARTING_LIFE_POINTS),
            Tables_StartingLifePoints(1, -1, DUEL_STARTING_LIFE_POINTS), limit_life_max, duelist_life_count,
            Tables_TwoPlayerLifePoints(TABLES_TWO_PLAYER_START, DUEL_STARTING_LIFE_POINTS),
            Tables_TwoPlayerLifePoints(TABLES_TWO_PLAYER_MAX, DUEL_STARTING_LIFE_POINTS),
            Tables_TwoPlayerLifePoints(TABLES_TWO_PLAYER_STEP, DUEL_LIFE_POINT_SELECTION_STEP), Tables_StarchipCap(),
            Tables_ChestLimit(), Tables_FreeDuelRecordCap());
}
