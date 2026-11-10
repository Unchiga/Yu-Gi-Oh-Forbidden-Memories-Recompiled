/* Game > Card drops (src/pc/cards/drops.c) over a made-up drop pool: every
 * card is one roll of the game's Duel_SelectCardDrop (which a code mod may
 * hook), in the community drop mod's order of draws, 1 + 7N for N cards;
 * and a mod's "limits": {"card_drops": n} locking the count over the
 * player's setting. tables.c (Tables_Scale, Tables_Pool, the limits) is
 * linked as it is. */
#include "../../src/pc/cards/drops.c"
#include "pc/mods/json.h"
#include "pc/debug/log.h"
#include "game/duel_rewards.h"
#include <stdarg.h>
#include <stdio.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);     \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

/* --- the game around drops.c ------------------------------------------- */

int gCard_nCount = CARD_COUNT;
signed char gDuel_bOpponentID = 1;
CardDropsState gCardDrops;
u16 gDuel_awPlayerDeck[DECK_SIZE];
DuelDropTable gDuel_awSaPowCardDrops[3];
static unsigned char chest[CARD_COUNT + 1];
static int card_drops = 1;
static unsigned seed = 1, draws;

int Settings_Get(SettingId id) { return id == SET_CARD_DROPS ? card_drops : 0; }
/* A tower duel's "drops" (tower.h): -1 not in one. */
static int tower_drops = -1;
int Tower_CardDrops(int wanted) { return tower_drops >= 0 ? tower_drops : wanted; }
int Memories_Rand(void)
{
    draws++;
    seed = seed * 1103515245u + 12345u;
    return (int)((seed >> 16) & 0x7FFF);
}
int Glyphs_Code(uint32_t character) { return (int)character; }
const unsigned char *Text_Own(int id) { (void)id; return NULL; } /* no translation: the port's English */
const unsigned char *Cards_NameCodes(int id) { (void)id; return NULL; }
unsigned char *Cards_ChestSlot(void *state, int id)
{
    static unsigned char nowhere;
    (void)state;
    return id >= 1 && id <= CARD_COUNT ? &chest[id] : &nowhere;
}
int Cards_Valid(int id) { return id >= 1 && id <= gCard_nCount; }
int Cards_BaseId(int id) { return Cards_Valid(id) ? id : 0; }
int Cards_EffectId(int id) { return Cards_BaseId(id); }
int Cards_RetailType(int id) { (void)id; return -1; }
int Cards_PickVariant(int id, int use) { (void)use; return id; }
/* The game's roll (duel_result_runtime.c) over the retail rows. */
s32 Duel_SelectCardDrop(s32 pool)
{
    s32 threshold = (Memories_Rand() & (DUEL_DROP_WEIGHT_TOTAL - 1)) + 1, sum = 0, i;
    for (i = 0; i < CARD_COUNT; i++) {
        sum += gDuel_awSaPowCardDrops[pool].weights[i];
        if (sum >= threshold) return i + 1;
    }
    return 0;
}
void Duel_AwardCard(s32 id) { chest[id]++; }

/* What tables.c asks of the cards and the mods: no mod edits a pool. */
int Cards_Type(int id) { (void)id; return 0; }
int Cards_IsMonsterType(int type) { return type >= 0 && (type < CARD_TYPE_MAGIC || type > CARD_TYPE_EQUIP); }
int Cards_TypeNamed(const char *text) { (void)text; return -1; }
int Cards_FusionGroupNamed(const char *text) { (void)text; return 0; }
int Cards_Attribute(int id) { (void)id; return 0; }
int Cards_AttributeNamed(const char *text) { (void)text; return -1; }
int Cards_Named(const char *text) { (void)text; return -1; }
int Cards_Reference(const JsonValue *value) { (void)value; return -1; }
int Mods_EntryUsed(const char *id, const JsonValue *entry, const char *where) { (void)id; (void)entry; (void)where; return 1; }
/* What the Mods window would say, every note in a row. */
long Mods_EachFusion(const char *id, int (*each)(const JsonValue *, long, void *), void *context, char *error,
                     size_t error_size)
{
    (void)id; (void)each; (void)context; (void)error; (void)error_size;
    return 0;
}
static char notes[2048];
void Mods_Note(const char *id, const char *format, ...)
{
    size_t used = strlen(notes);
    va_list arguments;
    used += (size_t)snprintf(notes + used, sizeof(notes) - used, "%s: ", id);
    if (used >= sizeof(notes)) return;
    va_start(arguments, format);
    vsnprintf(notes + used, sizeof(notes) - used, format, arguments);
    va_end(arguments);
    used = strlen(notes);
    snprintf(notes + used, sizeof(notes) - used, "\n");
}
int Log_Wanted(LogChannel channel) { (void)channel; return 0; }
void Log_Printf(LogChannel channel, const char *format, ...) { (void)channel; (void)format; }
int Mods_LoadedCount(void) { return 0; }
int Mods_Loaded(int index) { return index; }
int Mods_Active(int mod) { (void)mod; return 0; }
const char *Mods_Id(int mod) { (void)mod; return ""; }
const JsonValue *Mods_Manifest(int mod) { (void)mod; return NULL; }

/* --- the pool ------------------------------------------------------------ */

/* POW row: cards 5-10, 2048 in all. */
static const struct {
    int id;
    unsigned weight;
} pool[] = {{5, 700}, {6, 500}, {7, 400}, {8, 248}, {9, 100}, {10, 100}};
#define POOL_CARDS (int)(sizeof(pool) / sizeof(pool[0]))

static void reset(void)
{
    int i;
    memset(chest, 0, sizeof(chest));
    memset(gDuel_awPlayerDeck, 0, sizeof(gDuel_awPlayerDeck));
    memset(gDuel_awSaPowCardDrops, 0, sizeof(gDuel_awSaPowCardDrops));
    for (i = 0; i < POOL_CARDS; i++) gDuel_awSaPowCardDrops[0].weights[pool[i].id - 1] = (u16)pool[i].weight;
    CardDrops_Begin();
}

static int in_pool(int id)
{
    int i;
    for (i = 0; i < POOL_CARDS; i++) {
        if (pool[i].id == id) return 1;
    }
    return 0;
}

/* `wanted` cards from the same seed: the draws, the cards dealt, and SPOILS'
 * card from the pool. */
static void rolls(void)
{
    int wanted, spoils, i;
    for (wanted = 1; wanted <= 20; wanted += 19) {
        reset();
        card_drops = wanted;
        seed = 0x1234;
        draws = 0;
        spoils = CardDrops_Roll(0);
        CHECK(in_pool(spoils));
        CHECK(gCardDrops.count == wanted - 1);
        CHECK(draws == (wanted == 1 ? 1u : 1u + 7u * (unsigned)wanted));
        for (i = 0; i < gCardDrops.count; i++) CHECK(in_pool(gCardDrops.cards[i]));
    }
    card_drops = 1;
}

/* One card is the game's own roll, card for card. */
static void same_as_the_game(void)
{
    int run, cards[2][64];
    reset();
    seed = 77;
    for (run = 0; run < 64; run++) cards[0][run] = CardDrops_Roll(0);
    seed = 77;
    for (run = 0; run < 64; run++) cards[1][run] = Duel_SelectCardDrop(0);
    CHECK(!memcmp(cards[0], cards[1], sizeof(cards[0])));
}

/* --- a mod's lock ------------------------------------------------------- */

/* Mod `mod`'s manifest, applied after the ones before it. */
static void apply(const char *mod, const char *json)
{
    char error[128] = "";
    JsonDocument *document = Json_Parse(json, error, sizeof(error));
    if (!document) fprintf(stderr, "%s: %s\n", json, error);
    CHECK(document != NULL);
    Tables_Add(mod, Json_Root(document));
    Json_Free(document);
}

/* The cards a won duel deals with the player's setting at `player`, and
 * which: the added ones first, SPOILS' card last. */
static int dealt(int player, int cards[CARD_DROPS_MAX])
{
    int i;
    reset();
    card_drops = player;
    seed = 0x1234;
    draws = 0;
    cards[CARD_DROPS_MAX - 1] = CardDrops_Roll(0);
    CHECK(in_pool(cards[CARD_DROPS_MAX - 1]));
    for (i = 0; i < gCardDrops.count; i++) cards[i] = gCardDrops.cards[i];
    CHECK(draws == (gCardDrops.count ? 1u + 7u * (unsigned)(gCardDrops.count + 1) : 1u));
    return gCardDrops.count + 1;
}

#define FIVE "{\"limits\": {\"card_drops\": 5}}"

static void mod_lock(void)
{
    static const struct {
        const char *json, *note;
    } wrong[] = {
        {"{\"limits\": {\"card_drops\": 0}}", "bad: limits: \"card_drops\" is at least 1; left out\n"},
        {"{\"limits\": {\"card_drops\": -3}}", "bad: limits: \"card_drops\" is at least 1; left out\n"},
        {"{\"limits\": {\"card_drops\": 100}}", "bad: limits: \"card_drops\" is at most 99; left out\n"},
        {"{\"limits\": {\"card_drops\": \"5\"}}",
         "bad: limits: \"card_drops\" is a whole number, 1 to 99, without quotes; left out\n"},
        {"{\"limits\": {\"card_drops\": true}}",
         "bad: limits: \"card_drops\" is a whole number, 1 to 99, without quotes; left out\n"},
    };
    int locked[CARD_DROPS_MAX], own[CARD_DROPS_MAX], player;
    const char *by = "x";
    size_t i;
    Tables_Clear();
    notes[0] = '\0';
    /* No mod: the player's setting, and nothing locked. */
    CHECK(dealt(1, own) == 1 && dealt(99, own) == 99 && dealt(5, own) == 5);
    CHECK(Tables_CardDropsLock(&by) == 0 && by == NULL);
    CHECK(Tables_Limit("card_drops") == 0);

    /* A mod's 5 over the player's 1, 50 and 99: five cards each time, the
     * same five the player's own 5 deals from the seed. */
    apply("five", FIVE);
    CHECK(Tables_CardDropsLock(&by) == 5 && !strcmp(by, "five"));
    CHECK(Tables_Limit("card_drops") == 5);
    for (player = 1; player <= CARD_DROPS_MAX; player += 49) {
        CHECK(dealt(player, locked) == 5);
        Tables_Clear();
        CHECK(dealt(5, own) == 5);
        CHECK(!memcmp(locked, own, sizeof(int) * 4) && locked[CARD_DROPS_MAX - 1] == own[CARD_DROPS_MAX - 1]);
        apply("five", FIVE);
    }

    /* 99, the most, over the player's 1; and the mod removed: the player's
     * own count is back, as it was. */
    Tables_Clear();
    apply("all", "{\"limits\": {\"card_drops\": 99}}");
    CHECK(dealt(1, locked) == 99 && dealt(99, locked) == 99);
    Tables_Clear();
    CHECK(dealt(99, own) == 99 && dealt(1, own) == 1);
    CHECK(Tables_CardDropsLock(NULL) == 0);
    CHECK(!notes[0]);

    /* Two mods: the later one decides, and the earlier one says so; two
     * that agree say nothing. */
    apply("first", FIVE);
    apply("second", "{\"limits\": {\"card_drops\": 7}}");
    CHECK(Tables_CardDropsLock(&by) == 7 && !strcmp(by, "second"));
    CHECK(dealt(99, locked) == 7);
    CHECK(!strcmp(notes, "first: limits: \"card_drops\" 5 is not used: second, applied later, locks it at 7\n"));
    Tables_Clear();
    notes[0] = '\0';
    apply("first", FIVE);
    apply("second", FIVE);
    CHECK(Tables_CardDropsLock(&by) == 5 && !strcmp(by, "second") && !notes[0]);

    /* What is not 1 to 99 is noted and left out: the player decides, or
     * an earlier mod that said a good number. */
    for (i = 0; i < sizeof(wrong) / sizeof(wrong[0]); i++) {
        Tables_Clear();
        notes[0] = '\0';
        apply("bad", wrong[i].json);
        CHECK(!strcmp(notes, wrong[i].note));
        CHECK(Tables_CardDropsLock(&by) == 0 && by == NULL);
        CHECK(dealt(99, own) == 99);
        apply("good", "{\"limits\": {\"card_drops\": 3}}");
        apply("bad", wrong[i].json);
        CHECK(Tables_CardDropsLock(&by) == 3 && !strcmp(by, "good"));
    }
    Tables_Clear();
}

/* In a tower's duel its "drops" wins over Game > Card drops: 99 asked, one card. */
static void tower(void)
{
    reset();
    card_drops = 99;
    tower_drops = 1;
    seed = 0x1234;
    draws = 0;
    CHECK(in_pool(CardDrops_Roll(0)));
    CHECK(gCardDrops.count == 0 && draws == 1);
    tower_drops = 5;
    CardDrops_Roll(0);
    CHECK(gCardDrops.count == 4);
    tower_drops = -1;
    card_drops = 1;
}

int main(void)
{
    rolls();
    tower();
    same_as_the_game();
    mod_lock();
    puts("card drops: rolls and a mod's lock passed");
    return 0;
}
