/* The mods' rule tables (src/pc/cards/tables.c) against a small made-up
 * card list: fusions, equips, rituals, and drop and deck pools. */
#include "../../src/pc/cards/tables.c"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

int gCard_nCount = 800;
signed char gDuel_bOpponentID;
static int notes;
static const char *noted;   /* the mod of the latest note */

/* Cards 1-722 are the disc's, 723-800 copies of card (id - 722). Card 10
 * is "Kuriboh", 11 "Thunder Dragon", 12 "Blue-Eyes White Dragon", 20 an
 * equip "Legendary Sword", 21 a ritual "Black Luster Ritual"; dragons are
 * 12 and 13, everything else a warrior. */
static const char *const named[][2] = {{"Kuriboh", "10"}, {"Thunder Dragon", "11"}, {"Blue-eyes White Dragon", "12"},
                                       {"Legendary Sword", "20"}, {"Black Luster Ritual", "21"}};
int Cards_Valid(int id) { return id >= 1 && id <= gCard_nCount; }
int Cards_BaseId(int id) { return Cards_Valid(id) ? (id > CARD_COUNT ? id - CARD_COUNT : id) : 0; }
int Cards_EffectId(int id) { return Cards_BaseId(id); }
int Cards_Type(int id)
{
    id = Cards_BaseId(id);
    return id == 20 ? CARD_TYPE_EQUIP : id == 21 ? CARD_TYPE_RITUAL : id == 12 || id == 13 ? 0 : 3;
}
int Cards_RetailType(int id) { return id >= 1 && id <= CARD_COUNT ? Cards_Type(id) : -1; }
int Cards_TypeNamed(const char *text) { return same_letters(text, "Dragon") ? 0 : same_letters(text, "Warrior") ? 3 : -1; }
int Cards_FusionGroupNamed(const char *text)
{
    return same_letters(text, "Female") ? CARD_FUSION_GROUP_FEMALE
         : same_letters(text, "Bugrothian") ? CARD_FUSION_GROUP_BUGROTHIAN : CARD_FUSION_GROUP_NONE;
}
/* Card 12 is Light, 13 Dark, the rest Earth. */
int Cards_AttributeNamed(const char *text)
{
    return same_letters(text, "Light") ? 0 : same_letters(text, "Dark") ? 1 : same_letters(text, "Earth") ? 2 : -1;
}
int Cards_Attribute(int id)
{
    id = Cards_BaseId(id);
    return id == 12 ? 0 : id == 13 ? 1 : 2;
}
int Cards_Named(const char *text)
{
    size_t i;
    if (strspn(text, "0123456789") == strlen(text)) return Cards_Valid(atoi(text)) ? atoi(text) : -1;
    if (!strcmp(text, "test:copy:1")) return 723;
    for (i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        if (same_letters(text, named[i][0])) return atoi(named[i][1]);
    }
    return -1;
}
int Cards_Reference(const JsonValue *value)
{
    if (!value || Json_TypeOf(value) == JSON_NULL) return 0;
    if (Json_TypeOf(value) == JSON_NUMBER) return Cards_Valid((int)Json_Number(value, 0)) ? (int)Json_Number(value, 0) : -1;
    return Cards_Named(Json_String(value, ""));
}
void Mods_Note(const char *id, const char *format, ...)
{
    va_list arguments;
    noted = id;
    va_start(arguments, format);
    fprintf(stderr, "note %s: ", id);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
    notes++;
}
/* Mod "s" declares "on" (1) and "pick" (2); every other mod nothing. */
int Mods_EntryUsed(const char *id, const JsonValue *entry, const char *where)
{
    const char *setting = Json_String(Json_Member(entry, "setting"), NULL);
    const JsonValue *only = Json_Member(entry, "value");
    int value;
    (void)where;
    if (!setting || strcmp(id, "s")) return 1;
    value = !strcmp(setting, "on") ? 1 : !strcmp(setting, "pick") ? 2 : 0;
    return only ? value == Json_Number(only, -1) : value != 0;
}
int Log_Wanted(LogChannel channel) { (void)channel; return 1; }
void Log_Printf(LogChannel channel, const char *format, ...)
{
    va_list arguments;
    (void)channel;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
}
int Mods_LoadedCount(void) { return 0; }
int Mods_Loaded(int index) { return index; }
int Mods_Active(int mod) { return mod >= 0; }
const char *Mods_Id(int mod) { (void)mod; return ""; }
const JsonValue *Mods_Manifest(int mod) { (void)mod; return NULL; }
/* The duelist list is duelists_stubs.c: no duelist mod, the disc's forty,
 * which is what these cases are written against. */

static JsonDocument *documents[112];
static int document_count;
static void add(const char *mod, const char *text)
{
    char error[128];
    JsonDocument *document = Json_Parse(text, error, sizeof(error));
    if (!document) fprintf(stderr, "%s\n", error);
    assert(document);
    assert(document_count < (int)(sizeof(documents) / sizeof(documents[0])));
    documents[document_count++] = document;
    Tables_Add(mod, Json_Root(document));
}

static int fusion(int a, int b)
{
    int result = -1;
    return Tables_Fusion(a, b, &result) ? result : -1;
}

static unsigned total(const unsigned short *weights)
{
    unsigned sum = 0;
    int id;
    for (id = 1; id <= gCard_nCount; id++) sum += weights[id];
    return sum;
}

int main(void)
{
    unsigned short retail[CARD_COUNT] = {0};
    unsigned short own[6];
    const unsigned short *pool;
    int id;
    char name[TABLES_SHORT_NAME_LIMIT + 1];

    /* A translated full name made a name in place of COM. */
    assert(Tables_ShortenName("Sim\xE3o Muran", name) && !strcmp(name, "Sim\xE3o Muran"));
    assert(Tables_ShortenName("Jono 2\xBA Duelo", name) && !strcmp(name, "Jono"));
    assert(Tables_ShortenName("Weevil Underwood", name) && !strcmp(name, "W. Underwood"));
    assert(Tables_ShortenName("Sumo Mago Anubisius", name) && !strcmp(name, "S.M. Anubisius"));
    assert(Tables_ShortenName("Mago da Montanha", name) && !strcmp(name, "Montanha"));
    assert(Tables_ShortenName("Abcdefghijklmnopq", name) && !strcmp(name, "Abcdefghijklmn"));
    assert(Tables_ShortenName("A B C D E F G H Ijklmnopq", name) && !strcmp(name, "Ijklmnopq"));
    assert(!Tables_ShortenName("2P", name) && !name[0]);
    /* An elided article ends the name before it, not in it. */
    assert(Tables_ShortenName("Sekmeton l'Archimage", name) && !strcmp(name, "Sekmeton"));
    assert(Tables_ShortenName("Mago dell'Oceano", name) && !strcmp(name, "Mago"));
    assert(!Tables_ShortenName("l'Archimage", name) && !name[0]);

    /* Fusions: added, changed by a later mod, forbidden, and removed by
     * result; either order; a copy fuses as its base. */
    add("a", "{\"fusions\": ["
             "{\"with\": [\"Kuriboh\", \"Thunder Dragon\"], \"result\": \"Blue-Eyes White Dragon\"},"
             "{\"with\": [\"Kuriboh\", 30], \"result\": 31},"
             "{\"with\": [\"Kuriboh\", 40], \"result\": null},"
             "{\"remove\": 50},"
             "{\"with\": [\"Kuriboh\", \"nothing\"], \"result\": 1}]}");
    assert(notes == 1);
    add("b", "{\"fusions\": [{\"with\": [30, \"kuriboh\"], \"result\": 32}]}");
    assert(fusion(10, 11) == 12 && fusion(11, 10) == 12);
    assert(fusion(10, 30) == 32);          /* the later mod wins */
    assert(fusion(40, 10) == 0);           /* forbidden */
    assert(fusion(10, 12) == -1);          /* no rule: the disc decides */
    assert(fusion(CARD_COUNT + 10, 11) == 12);
    assert(Tables_FilterFusion(50) == 0 && Tables_FilterFusion(51) == 51);

    /* An entry the mod's settings leave out is not read: "setting" off,
     * or a "value" the setting is not. */
    add("s", "{\"fusions\": ["
             "{\"with\": [20, 21], \"result\": 70, \"setting\": \"on\"},"
             "{\"with\": [20, 22], \"result\": 71, \"setting\": \"off\"},"
             "{\"with\": [20, 23], \"result\": 72, \"setting\": \"pick\", \"value\": 2},"
             "{\"with\": [20, 24], \"result\": 73, \"setting\": \"pick\", \"value\": 1}]}");
    assert(fusion(20, 21) == 70 && fusion(20, 22) == -1);
    assert(fusion(20, 23) == 72 && fusion(20, 24) == -1);

    /* A rule naming a copy is surer than its base's: both cards as they
     * are, then a copy with its partner's base (the later of two such),
     * then the bases. 732 is a copy of 10, 733 of 11, 739 of 17, 740 of 18. */
    add("c", "{\"fusions\": ["
             "{\"with\": [732, 11], \"result\": 60},"
             "{\"with\": [10, 733], \"result\": 61},"
             "{\"with\": [17, 18], \"result\": 62},"
             "{\"with\": [732, 18], \"result\": 63},"
             "{\"with\": [17, 740], \"result\": 64}]}");
    assert(fusion(732, 11) == 60 && fusion(11, 732) == 60);   /* the copy's own rule, not 10 + 11's */
    assert(fusion(10, 733) == 61);
    assert(fusion(732, 733) == 61);                           /* 10 + 733 is later than 732 + 11 */
    add("d", "{\"fusions\": [{\"with\": [732, 11], \"result\": 65}]}");
    assert(fusion(733, 732) == 65);                           /* now 732 + 11 is */
    add("e", "{\"fusions\": [{\"with\": [732, 733], \"result\": 66}]}");
    assert(fusion(733, 732) == 66);                           /* both named as they are: surest */
    assert(fusion(739, 740) == 64);                           /* 17 + 740 */
    assert(fusion(739, 18) == 62);                            /* only the bases' */
    assert(fusion(732, 740) == 63);                           /* 732 + 18 */

    /* Many rules, as the editor's bulk fusions write them (every pair of
     * two sets of cards): read in one pass, each pair found in either
     * order, the later of two rules for a pair the one that counts. */
    {
        enum { FIRST = 100, LAST = 299 };
        size_t size = (size_t)(LAST - FIRST + 1) * (LAST - FIRST + 2) / 2 * 48 + 64, used;
        char *text = malloc(size);
        int x, y;
        assert(text);
        used = (size_t)sprintf(text, "{\"fusions\": [");
        for (x = FIRST; x <= LAST; x++)
            for (y = x; y <= LAST; y++)
                used += (size_t)sprintf(text + used, "{\"with\": [%d, %d], \"result\": %d},", y, x, 300 + (x + y) % 100);
        sprintf(text + used, "{\"with\": [%d, %d], \"result\": 1}]}", FIRST, LAST);
        add("bulk", text);
        free(text);
        assert(fusion(FIRST, FIRST) == 300 + (2 * FIRST) % 100);
        assert(fusion(150, 222) == 372 && fusion(222, 150) == 372);
        assert(fusion(LAST, LAST) == 300 + (2 * LAST) % 100);
        assert(fusion(FIRST, LAST) == 1);                     /* the later rule */
        assert(fusion(10, 11) == 12);                         /* the other mods' rules stay */
    }

    /* Equips: every dragon, less one; a replaced list; a copy of the equip. */
    add("a", "{\"equips\": [{\"card\": \"Legendary Sword\", \"add\": [\"Dragon\", 5], \"remove\": [13]},"
             "{\"card\": \"Kuriboh\", \"add\": [1]}]}");
    assert(notes == 2);                    /* Kuriboh is not an equip */
    assert(Tables_Equip(20, 12) == 1 && Tables_Equip(20, 13) == 0 && Tables_Equip(20, 5) == 1);
    assert(Tables_Equip(20, 6) == -1 && Tables_Equip(CARD_COUNT + 20, 12) == 1);
    add("b", "{\"equips\": [{\"card\": 20, \"replace\": true, \"add\": [6]}]}");
    assert(Tables_Equip(20, 6) == 1 && Tables_Equip(20, 12) == 0 && Tables_Equip(20, 7) == 0);

    /* Rituals: a new recipe, then removed by a later mod. */
    add("a", "{\"rituals\": [{\"card\": \"Black Luster Ritual\", \"tributes\": [1, 2, \"test:copy:1\"], \"result\": 12}]}");
    assert(Tables_Ritual(21, own) == 1 && own[0] == 21 && own[3] == 723 && own[4] == 12 && own[5] == 0);
    assert(Tables_Ritual(22, own) == -1);
    {
        TablesRitualRequirement req[3];
        unsigned short result = 0;
        add("conditions", "{\"rituals\": [{\"card\": 21, \"tributes\": ["
            "{\"min_defense\": 1000, \"max_defense\": 2999, \"defense_gt_attack\": true},"
            "{\"card\": 11},"
            "{\"type\": \"Dragon\", \"min_attack\": 500, \"max_attack\": 2500}], \"result\": 12}]}");
        assert(Tables_Ritual(21, own) == -1);
        assert(Tables_RitualRequirements(21, req, &result) == 1 && result == 12);
        assert(req[0].card == 0 && req[0].min_defense == 1000 && req[0].max_defense == 2999 && req[0].defense_gt_attack == 1);
        assert(req[1].card == 11 && req[1].type == -1);
        assert(req[2].type == 0 && req[2].min_attack == 500 && req[2].max_attack == 2500);
        add("condition-groups", "{\"rituals\": [{\"card\": 21, \"tributes\": ["
            "{\"fusion_group\": \"Female\", \"min_level\": 4, \"max_level\": 6},"
            "{\"fusion_group\": \"Bugrothian\"},"
            "{\"card\": \"test:copy:1\"}], \"result\": \"test:copy:1\"}]}");
        result = 0;
        assert(Tables_RitualRequirements(21, req, &result) == 1 && result == 723);
        assert(req[0].fusion_group == CARD_FUSION_GROUP_FEMALE && req[0].min_level == 4 && req[0].max_level == 6);
        assert(req[1].fusion_group == CARD_FUSION_GROUP_BUGROTHIAN);
        assert(req[2].card == 723); /* stable identity resolves to an added card, not a retail-only id */
        /* A minimum of 0 is still a requirement (any monster), any case of a
         * group's name is it, and a key the game does not know is noted. */
        notes = 0;
        add("condition-any", "{\"rituals\": [{\"card\": 21, \"tributes\": ["
            "{\"min_attack\": 0}, {\"fusion_group\": \"female\"}, {\"card\": 11, \"colour\": 1}],"
            " \"result\": 12}]}");
        assert(notes == 1);
        assert(Tables_RitualRequirements(21, req, &result) == 1 && result == 12);
        assert(req[0].min_attack == 0 && req[0].card == 0 && req[0].type == -1);
        assert(req[1].fusion_group == CARD_FUSION_GROUP_FEMALE && req[2].card == 11);
        notes = 0;
        add("condition-empty", "{\"rituals\": [{\"card\": 21, \"tributes\": [{}, 1, 2], \"result\": 12}]}");
        assert(notes == 1);
    }
    add("b", "{\"rituals\": [{\"card\": 21, \"result\": null}]}");
    assert(Tables_Ritual(21, own) == 0);
    /* A mod's own ritual card (a copy of one) takes a recipe of its own;
     * a copy of a monster does not, and the base keeps its rule. */
    assert(!Tables_HasRitual(CARD_COUNT + 21));
    add("c", "{\"rituals\": [{\"card\": 743, \"tributes\": [1, 2, 3], \"result\": 13}]}");
    assert(Tables_HasRitual(743) && Tables_Ritual(743, own) == 1 && own[0] == 743 && own[4] == 13);
    assert(Tables_Ritual(21, own) == 0);
    notes = 0;
    add("c", "{\"rituals\": [{\"card\": 723, \"tributes\": [1, 2, 3], \"result\": 13}]}");
    assert(notes == 1 && !Tables_HasRitual(723));
    {
        TablesRitualRequirement req[3];
        assert(Tables_RitualRequirements(21, req, 0) == 0);
    }

    /* Pools. The disc's: cards 101-116 at 128 each. */
    for (id = 101; id <= 116; id++) retail[id - 1] = 128;
    assert(!Tables_PoolFor(7, TABLES_POOL_POW, retail));
    add("a", "{\"drops\": {\"Seto\": {\"pow\": {\"Blue-Eyes White Dragon\": 1024, \"101\": 0}}}}");
    pool = Tables_PoolFor(7, TABLES_POOL_POW, retail);
    assert(pool && total(pool) == 2048 && pool[12] == 1024 && pool[101] == 0);
    for (id = 102; id <= 116; id++) assert(pool[id] == 68 || pool[id] == 69);   /* 1024 over fifteen */
    assert(!Tables_PoolFor(7, TABLES_POOL_BCD, retail) && !Tables_PoolFor(8, TABLES_POOL_POW, retail));
    /* A second mod's edit of the same pool goes on top of the first. */
    add("b", "{\"drops\": {\"all\": {\"sa-pow\": {\"test:copy:1\": 48}}}}");
    pool = Tables_PoolFor(7, TABLES_POOL_POW, retail);
    assert(total(pool) == 2048 && pool[723] == 48 && pool[101] == 0 && pool[12] > 990);
    pool = Tables_PoolFor(8, TABLES_POOL_POW, retail);
    assert(total(pool) == 2048 && pool[723] == 48 && pool[101] > 0);
    /* Replacing the pool: the listed cards, in proportion. */
    add("a", "{\"drops\": {\"Heishin 2nd\": {\"tec\": {\"replace\": true, \"10\": 1, \"11\": 3}}}}");
    pool = Tables_PoolFor(35, TABLES_POOL_TEC, retail);
    assert(pool[10] == 512 && pool[11] == 1536 && total(pool) == 2048);
    /* Too much given: in proportion as well. */
    add("a", "{\"drops\": {\"3\": {\"bcd\": {\"10\": 3000, \"11\": 3000}}}}");
    pool = Tables_PoolFor(3, TABLES_POOL_BCD, retail);
    assert(pool[10] == 1024 && pool[11] == 1024 && total(pool) == 2048);

    /* Decks: a deck of fewer than 14 cards cannot be dealt, and is left. */
    notes = 0;
    add("a", "{\"decks\": {\"Teana\": {\"replace\": true, \"10\": 5, \"11\": 5}}}");
    pool = Tables_PoolFor(2, TABLES_POOL_DECK, retail);
    assert(pool && notes == 1 && pool[101] == 128 && pool[10] == 0 && total(pool) == 2048);
    add("a", "{\"decks\": {\"Pegasus\": {\"Kuriboh\": 300}}, \"drops\": {\"nobody\": {}}}");
    assert(notes == 2);
    pool = Tables_PoolFor(15, TABLES_POOL_DECK, retail);
    assert(pool[10] == 300 && total(pool) == 2048);
    add("a", "{\"decks\": {\"Isis\": {\"fixed\": false, \"Kuriboh\": 300}}}");
    assert(notes == 2 && Tables_PoolFor(16, TABLES_POOL_DECK, retail)[10] == 300);
    /* The pool follows what the game loaded (a data mod may patch it). */
    retail[100] = 0;
    pool = Tables_PoolFor(15, TABLES_POOL_DECK, retail);
    assert(pool[101] == 0 && pool[10] == 300 && total(pool) == 2048);

    /* Fixed decks: copies by card, past three of one, in id order; a later
     * fixed deck wins; one that is not 40 cards is left out; a fixed deck
     * wins over the weighted edits of the same deck, which are told so. */
    {
        unsigned short deck[TABLES_DECK_SIZE];
        assert(!Tables_FixedDeck(15, deck) && !Tables_FixedDeck(-1, deck));
        notes = 0;
        add("f", "{\"decks\": {\"Pegasus\": {\"fixed\": true, \"Kuriboh\": 30, \"test:copy:1\": 4, \"12\": 6}}}");
        assert(notes == 0 && Tables_FixedDeck(15, deck));
        assert(notes == 1);                /* Pegasus's weighted edit above waits */
        for (id = 0; id < 30; id++) assert(deck[id] == 10);
        for (id = 30; id < 36; id++) assert(deck[id] == 12);
        for (id = 36; id < 40; id++) assert(deck[id] == 723);
        assert(Tables_FixedDeck(15, deck) && notes == 1);   /* told once */
        notes = 0;
        add("g", "{\"decks\": {\"Pegasus\": {\"fixed\": true, \"Kuriboh\": 39}, \"Shadi\": {\"fixed\": true, \"nothing\": 40},"
                 "\"Seto\": {\"fixed\": true, \"Kuriboh\": -1}, \"all\": {\"fixed\": true, \"11\": 40, \"12\": 0}}}");
        assert(notes == 5);                /* 39 cards; no card, and so 0; a count under 0, and so 0 */
        assert(Tables_FixedDeck(15, deck) && deck[0] == 11 && deck[39] == 11);   /* "all" came later */
        assert(Tables_FixedDeck(1, deck) && deck[0] == 11);
        add("h", "{\"decks\": {\"Pegasus\": {\"fixed\": true, \"Kuriboh\": 20, \"Thunder Dragon\": 20}}}");
        assert(Tables_FixedDeck(15, deck) && deck[0] == 10 && deck[19] == 10 && deck[20] == 11 && deck[39] == 11);
        assert(Tables_FixedDeck(14, deck) && deck[0] == 11);
        /* "fixed" in quotes is neither kind of deck, and is told so. */
        notes = 0;
        add("h2", "{\"decks\": {\"Teana\": {\"fixed\": \"true\", \"Kuriboh\": 40}}}");
        assert(notes == 1 && Tables_FixedDeck(2, deck) && deck[0] == 11);
        /* Told a fixed deck wins, a weighted edit is still told it fails. */
        add("h3", "{\"decks\": {\"Pegasus\": {\"replace\": true, \"10\": 5, \"11\": 5}}}");
        notes = 0;
        assert(Tables_FixedDeck(15, deck) && notes == 1);
        assert(Tables_PoolFor(15, TABLES_POOL_DECK, retail) && notes == 2);
    }

    /* The chest: without the rule it keeps 250 and a card past that is lost,
     * as on the disc; with it, it keeps "limit" and each card past that is
     * worth its starchips, up to 999999; the later mod wins. The chest is
     * full only under a mod's rule: without one, a save past 250 is the
     * disc's business. */
    {
        unsigned starchips = 100;
        assert(Tables_ChestLimit() == 250 && Tables_ChestOverflow(250, &starchips) == 0 && starchips == 100);
        assert(!Tables_ChestFull(250) && !Tables_ChestFull(255));
        notes = 0;
        add("i", "{\"chest_overflow\": {\"starchips\": 3}}");
        add("j", "{\"chest_overflow\": {\"limit\": 0, \"starchips\": 9}}");
        add("k", "{\"chest_overflow\": {\"limit\": 3, \"starchips\": \"many\"}}");
        add("k2", "{\"chest_overflow\": 3}");
        assert(notes == 3 && Tables_ChestLimit() == 250);
        assert(!Tables_ChestFull(249) && Tables_ChestFull(250));
        assert(Tables_ChestOverflow(249, &starchips) == 0 && starchips == 100);
        assert(Tables_ChestOverflow(250, &starchips) == 3 && starchips == 103);
        add("l", "{\"chest_overflow\": {\"limit\": 3, \"starchips\": 999999}}");
        assert(Tables_ChestLimit() == 3 && Tables_ChestFull(3) && !Tables_ChestFull(2));
        assert(Tables_ChestOverflow(2, &starchips) == 0 && starchips == 103);
        assert(Tables_ChestOverflow(3, &starchips) == 999999 && starchips == 999999);
        assert(Tables_ChestOverflow(40, &starchips) == 999999 && starchips == 999999);
        /* A balance past the cap is kept, not cut back to it. */
        starchips = 2000000;
        assert(Tables_ChestOverflow(40, &starchips) == 999999 && starchips == 2000000);
        starchips = 103;
        add("m", "{\"chest_overflow\": {\"limit\": 10}}");
        starchips = 5;
        assert(Tables_ChestLimit() == 10 && Tables_ChestOverflow(10, &starchips) == 0 && starchips == 5);
        add("n", "{\"chest_overflow\": {\"limit\": 250, \"starchips\": 1}}");
        /* Past the disc's 250, up to the byte a card the chest keeps. */
        notes = 0;
        add("n2", "{\"chest_overflow\": {\"limit\": 256}}");
        assert(notes == 1 && Tables_ChestLimit() == 250);
        add("n3", "{\"chest_overflow\": {\"limit\": 255}}");
        assert(notes == 1 && Tables_ChestLimit() == 255 && !Tables_ChestFull(254) && Tables_ChestFull(255));
        add("n4", "{\"chest_overflow\": {\"limit\": 250, \"starchips\": 1}}");
    }

    /* Terrains: a listed pair has its points; the rest are the disc's until
     * a mod replaces the table; names, aliases and numbers; refusals. */
    {
        int bonus = 12345;
        assert(!Tables_TerrainBonus(1, 0, &bonus) && bonus == 12345);
        notes = 0;
        add("o", "{\"terrain_bonus\": {\"Forest\": {\"Dragon\": -300}, \"sea\": {\"Warrior\": 700},"
                 "\"7\": {\"Dragon\": 1}, \"Umi\": {\"Magic\": 5, \"Dragon\": 40000, \"Warrior\": 650}, \"Yami\": 3}}");
        assert(notes == 4);                /* terrain 7, Magic, 40000, not an object */
        assert(Tables_TerrainBonus(1, 0, &bonus) && bonus == -300);
        assert(Tables_TerrainBonus(5, 3, &bonus) && bonus == 650);          /* "Umi" is "sea", later */
        assert(!Tables_TerrainBonus(1, 3, &bonus) && !Tables_TerrainBonus(5, 0, &bonus));
        assert(!Tables_TerrainBonus(0, 0, &bonus) && !Tables_TerrainBonus(1, CARD_TYPE_MAGIC, &bonus));
        add("p", "{\"terrain_bonus\": {\"Mountain\": {\"Dragon\": 250}, \"replace\": true}}");
        assert(Tables_TerrainBonus(3, 0, &bonus) && bonus == 250);
        assert(Tables_TerrainBonus(1, 0, &bonus) && bonus == 0);            /* replaced */
        assert(Tables_TerrainBonus(6, 19, &bonus) && bonus == 0);
        add("q", "{\"terrain_bonus\": {\"4\": {\"Warrior\": 100}, \"Meadow\": {\"Dragon\": -50}}}");
        assert(Tables_TerrainBonus(4, 3, &bonus) && bonus == 100 && Tables_TerrainBonus(4, 0, &bonus) && bonus == -50);
    }

    /* Equip bonuses: none is the disc's; "bonus" for any monster; the first
     * "bonus_if" that fits comes before it; a later entry wins; a copy of
     * the equip is the equip; refusals. 12 is a Light dragon, 13 a Dark
     * dragon, 5 an Earth warrior. */
    assert(Tables_EquipBonus(20, 12, 500) == 500);
    notes = 0;
    add("r", "{\"equips\": [{\"card\": \"Legendary Sword\", \"bonus\": 300,"
             "\"bonus_if\": {\"Light\": 900, \"Dragon\": 700, \"Magic\": 1, \"Earth\": 40000}},"
             "{\"card\": \"Kuriboh\", \"bonus\": 5}, {\"card\": 20, \"bonus\": \"lots\"}]}");
    assert(notes == 4);                    /* Magic, 40000, Kuriboh not an equip, "lots" */
    assert(Tables_EquipBonus(20, 12, 500) == 900);           /* Light first */
    assert(Tables_EquipBonus(20, 13, 500) == 700);           /* a Dark dragon */
    assert(Tables_EquipBonus(20, 5, 500) == 300);            /* neither */
    assert(Tables_EquipBonus(CARD_COUNT + 20, 13, 500) == 700);
    assert(Tables_EquipBonus(21, 13, 1000) == 1000);         /* another equip: the disc's */
    add("s", "{\"equips\": [{\"card\": 20, \"bonus_if\": {\"Warrior\": -200}}]}");
    assert(Tables_EquipBonus(20, 5, 500) == -200);           /* the later entry fits */
    assert(Tables_EquipBonus(20, 12, 500) == 900);           /* it says nothing of a dragon */
    add("t", "{\"equips\": [{\"card\": 20, \"bonus\": 0, \"add\": [\"Dragon\"]}]}");
    assert(Tables_EquipBonus(20, 12, 500) == 0 && Tables_Equip(20, 12) == 1);
    /* A default for the equips no entry gives a bonus: Megamorph's +1000 too. */
    notes = 0;
    add("t2", "{\"equip_bonus_default\": 40000}");
    assert(notes == 1 && Tables_EquipBonus(21, 13, 1000) == 1000);
    add("t3", "{\"equip_bonus_default\": 700}");
    assert(Tables_EquipBonus(21, 13, 1000) == 700 && Tables_EquipBonus(21, 5, 500) == 700);
    assert(Tables_EquipBonus(20, 12, 500) == 0 && Tables_EquipBonus(20, 5, 500) == 0);

    /* Attack traps: the disc's thresholds until a mod sets one, in points;
     * only the six attack traps; a warning when they fall out of order. */
    assert(Tables_TrapThreshold(0, 500) == 500 && Tables_TrapThreshold(5, 25500) == 25500);
    notes = 0;
    add("u", "{\"trap_thresholds\": {\"681\": 800, \"686\": 30000, \"Kuriboh\": 5, \"683\": -1, \"684\": 70000}}");
    assert(notes == 3);                    /* Kuriboh, -1, 70000 */
    assert(Tables_TrapThreshold(0, 500) == 800 && Tables_TrapThreshold(1, 1000) == 1000);
    assert(Tables_TrapThreshold(5, 25500) == 30000 && Tables_TrapThreshold(6, 7) == 7);
    notes = 0;
    add("v", "{\"trap_thresholds\": {\"682\": 700}}");
    assert(notes == 1 && Tables_TrapThreshold(1, 1000) == 700);   /* 700 after 800: out of order */
    add("w", "{\"trap_thresholds\": {\"682\": 900}}");
    assert(notes == 1 && Tables_TrapThreshold(1, 1000) == 900);

    /* The Password screen: a card by name or number, "all" first whatever
     * its place, passwords as digits, numbers or the card's own number, and
     * prices in starchips or a percent of the disc's. */
    {
        unsigned price = 999999, password = 0x89631139u;
        assert(!Tables_PasswordShop(12, &price, &password));
        notes = 0;
        add("x", "{\"passwords\": {\"Blue-eyes White Dragon\": {\"password\": \"00000001\", \"starchips\": 100},"
                 " \"all\": {\"password\": \"card number\", \"starchips_percent\": 10},"
                 " \"Kuriboh\": {\"password\": 12345678}, \"11\": {\"password\": \"\"},"
                 " \"723\": {\"password\": \"1\"}, \"Nobody\": {\"starchips\": 1},"
                 " \"20\": {\"password\": \"12a\", \"starchips\": 1000000}}}");
        assert(notes == 3);                    /* Nobody, "12a", 1000000 */
        assert(Tables_PasswordShop(12, &price, &password) && price == 100 && password == 0x00000001u);
        price = 70, password = 0x76184692u;    /* card 3 by "all": 10% of 70 */
        assert(Tables_PasswordShop(3, &price, &password) && price == 7 && password == 0x00000003u);
        price = 4, password = 0x1u;
        assert(Tables_PasswordShop(10, &price, &password) && price == 1 && password == 0x12345678u);
        price = 999999, password = 0x1u;
        assert(Tables_PasswordShop(722, &price, &password) && price == 100000 && password == 0x00000722u);
        price = 50, password = 0x1u;
        assert(Tables_PasswordShop(11, &price, &password) && price == 5 && password == CARD_PASSWORD_NONE);
        price = 50, password = 0x1u;           /* 20: its bad keys left out, "all" stands */
        assert(Tables_PasswordShop(20, &price, &password) && price == 5 && password == 0x00000020u);
        price = 0, password = 0x1u;            /* free stays free */
        assert(Tables_PasswordShop(5, &price, &password) && price == 0);
        price = 5, password = 0x1u;            /* 10% of 5 still costs one */
        assert(Tables_PasswordShop(5, &price, &password) && price == 1);
        price = 1, password = 0x1u;
        assert(!Tables_PasswordShop(0, &price, &password));
        password = 2;
        assert(Tables_PasswordShop(723, &price, &password) && password == 1 && price == 1);
        price = 999999; password = CARD_PASSWORD_NONE;
        assert(Tables_PasswordShop(800, &price, &password) && password == 0x800 && price == 100000);
        notes = 0;
        add("y", "{\"passwords\": {\"Kuriboh\": {\"starchips\": 3}}}");
        assert(notes == 0);
        price = 4, password = 0x1u;            /* a later mod: its price, the earlier password */
        assert(Tables_PasswordShop(10, &price, &password) && price == 3 && password == 0x12345678u);
        add("z", "{\"passwords\": [1]}");
        assert(notes == 1);
    }

    /* A price of 0 is a free card, by "starchips" or a percent of 0; a
     * bad "all" is noted once and counts no entries. */
    Tables_Clear();
    {
        unsigned price, password;
        notes = 0;
        add("free", "{\"passwords\": {\"Kuriboh\": {\"starchips\": 0}, \"11\": {\"starchips_percent\": 0},"
                    " \"12\": {\"starchips_percent\": 1}}}");
        assert(notes == 0 && shop_count == 3);
        price = 999999, password = 0x1u;
        assert(Tables_PasswordShop(10, &price, &password) && price == 0 && password == 0x1u);
        price = 999999, password = 0x1u;
        assert(Tables_PasswordShop(11, &price, &password) && price == 0);
        price = 40, password = 0x1u;           /* 1% of 40 rounds to 0: still one */
        assert(Tables_PasswordShop(12, &price, &password) && price == 1);
        add("bad", "{\"passwords\": {\"all\": {\"password\": \"x\"}}}");
        assert(notes == 1 && shop_count == 3);
        price = 70, password = 0x1u;
        assert(!Tables_PasswordShop(3, &price, &password) && price == 70 && password == 0x1u);
        add("both", "{\"passwords\": {\"all\": {\"starchips\": 5, \"starchips_percent\": 10}}}");
        assert(notes == 2 && shop_count == 3 + gCard_nCount);
        price = 70, password = 0x1u;
        assert(Tables_PasswordShop(3, &price, &password) && price == 5 && password == 0x1u);
    }

    /* Two cards with one password in the loaded table: each card the
     * screen cannot give is noted once, beside the mod that set a
     * password; a clash no mod's "passwords" made is only logged, and
     * cards with no password never clash. */
    Tables_Clear();
    {
        static unsigned passwords[CARD_TABLE_ID_END];
        for (id = CARD_COUNT + 1; id <= gCard_nCount; id++) passwords[id] = CARD_PASSWORD_NONE;
        for (id = 1; id <= CARD_COUNT; id++) passwords[id] = password_digits(10000000UL + (unsigned long)id);
        passwords[5] = passwords[6] = CARD_PASSWORD_NONE;
        notes = 0;
        assert(Tables_CheckPasswords(passwords) == 0 && notes == 0);
        add("retail", "{\"passwords\": {\"Thunder Dragon\": {\"password\": \"10000012\"}}}");
        passwords[11] = 0x10000012u;           /* card 12's: the screen gives 11 */
        assert(Tables_CheckPasswords(passwords) == 1 && notes == 1 && !strcmp(noted, "retail"));
        add("twin", "{\"passwords\": {\"20\": {\"password\": 10000012}}}");
        passwords[20] = 0x10000012u;           /* 11, 12 and 20: two left out */
        notes = 0;
        assert(Tables_CheckPasswords(passwords) == 2 && notes == 2 && !strcmp(noted, "twin"));
        passwords[30] = passwords[31];         /* a data patch's: no mod to note */
        notes = 0;
        assert(Tables_CheckPasswords(passwords) == 3 && notes == 2);

        /* "all" giving every card one password: one note, not 719. */
        Tables_Clear();
        add("flood", "{\"passwords\": {\"all\": {\"password\": \"12345678\"}}}");
        for (id = 1; id <= CARD_COUNT; id++) passwords[id] = 0x12345678u;
        passwords[5] = passwords[6] = CARD_PASSWORD_NONE;
        notes = 0;
        assert(Tables_CheckPasswords(passwords) == CARD_COUNT - 3 && notes == 1 && !strcmp(noted, "flood"));
    }

    /* Limits: the retail numbers until a mod says; the latest mod wins
     * each; past what the game keeps is held there with a note; a
     * duelist's own LP before its side's, before "start". */
    Tables_Clear();
    assert(Tables_StatCap(0) == 9999 && Tables_StatCap(1) == 9999 && Tables_StatCapEither() == 9999);
    assert(Tables_StartingLifePoints(0, 8, 8000) == 8000 && Tables_StartingLifePoints(1, -1, 4000) == 4000);
    assert(Tables_MaxLifePoints(8000) == 8000 && Tables_MaxLifePoints(3000) == 3000);
    assert(Tables_TwoPlayerLifePoints(TABLES_TWO_PLAYER_START, 8000) == 8000);
    assert(Tables_TwoPlayerLifePoints(TABLES_TWO_PLAYER_MAX, 8000) == 8000);
    assert(Tables_TwoPlayerLifePoints(TABLES_TWO_PLAYER_STEP, 500) == 500);
    assert(Tables_StarchipCap() == 999999 && Tables_FreeDuelRecordCap() == 999 && Tables_ChestLimit() == 250);
    assert(Tables_Limit("attack") == 9999 && Tables_Limit("life_points_max") == 0 && Tables_Limit("nothing") == -1);
    notes = 0;
    add("lim1", "{\"limits\": {\"stats\": 30000, \"defense\": 20000, \"life_points\": 16000,"
                " \"starchips\": 5000000, \"chest\": 99, \"free_duel_record\": 9999}}");
    assert(notes == 0);
    assert(Tables_StatCap(0) == 30000 && Tables_StatCap(1) == 20000 && Tables_StatCapEither() == 30000);
    assert(Tables_StartingLifePoints(0, 8, 8000) == 16000 && Tables_StartingLifePoints(1, 8, 8000) == 16000);
    assert(Tables_StartingLifePoints(0, -1, 8000) == 16000);   /* two-player duels too */
    assert(Tables_MaxLifePoints(16000) == 16000 && Tables_StarchipCap() == 5000000);
    assert(Tables_ChestLimit() == 99 && Tables_FreeDuelRecordCap() == 9999 && Tables_Limit("defense") == 20000);
    add("lim2", "{\"limits\": {\"attack\": 0, \"life_points\": {\"player\": 4000, \"max\": 32767,"
                " \"duelists\": {\"Heishin\": 12000, \"Seto\": {\"player\": 100, \"opponent\": 200},"
                " \"all\": {\"player\": 7000}}},"
                " \"two_player\": {\"start\": 9000, \"max\": 20000, \"step\": 1000}}}");
    assert(notes == 0);
    assert(Tables_StatCap(0) == 0 && Tables_StatCap(1) == 20000);
    assert(Tables_StartingLifePoints(1, 8, 8000) == 12000);  /* Heishin's own */
    assert(Tables_StartingLifePoints(0, 8, 8000) == 7000);   /* "all" of them, before the side's */
    assert(Tables_StartingLifePoints(0, 7, 8000) == 100 && Tables_StartingLifePoints(1, 7, 8000) == 200);
    assert(Tables_StartingLifePoints(1, 9, 8000) == 16000);  /* the opponent's side, from lim1's start */
    assert(Tables_StartingLifePoints(0, -1, 8000) == 4000);  /* a two-player duel has no duelist */
    assert(Tables_MaxLifePoints(4000) == 32767 && Tables_Limit("life_points_max") == 32767);
    assert(Tables_TwoPlayerLifePoints(TABLES_TWO_PLAYER_START, 8000) == 9000);
    assert(Tables_TwoPlayerLifePoints(TABLES_TWO_PLAYER_MAX, 8000) == 20000);
    assert(Tables_TwoPlayerLifePoints(TABLES_TWO_PLAYER_STEP, 500) == 1000);
    /* Past the storage: held at the most the game keeps, with a note each;
     * the wrong kind of value is left out with one. */
    notes = 0;
    add("lim3", "{\"limits\": {\"stats\": 40000, \"life_points\": {\"start\": 99999, \"max\": 0},"
                " \"two_player\": {\"start\": 30000, \"max\": 25000}, \"chest\": 300,"
                " \"starchips\": 123456789, \"free_duel_record\": \"lots\", \"lp\": 3}}");
    assert(notes == 7);   /* stats, start, max 0, chest, starchips, "lots", "lp" */
    assert(Tables_StatCap(0) == 32767 && Tables_StatCap(1) == 32767);
    assert(Tables_StartingLifePoints(1, 9, 8000) == 32767 && Tables_MaxLifePoints(8000) == 32767);
    assert(Tables_ChestLimit() == 255 && Tables_StarchipCap() == 99999999u && Tables_FreeDuelRecordCap() == 9999);
    assert(Tables_TwoPlayerLifePoints(TABLES_TWO_PLAYER_START, 8000) == 25000);   /* held at the top */
    /* A top under the disc's start or the step: both are held at it. */
    add("lim3b", "{\"limits\": {\"two_player\": {\"max\": 4000, \"step\": 10000}}}");
    assert(Tables_TwoPlayerLifePoints(TABLES_TWO_PLAYER_START, 8000) == 4000);
    assert(Tables_TwoPlayerLifePoints(TABLES_TWO_PLAYER_STEP, 500) == 4000);
    notes = 0;
    add("lim4", "{\"limits\": {\"life_points\": {\"duelists\": {\"Nobody\": 5, \"Heishin\": [1]}},"
                " \"two_player\": 3}}");
    assert(notes == 3);
    add("lim5", "{\"limits\": {\"starchips\": 0}}");
    assert(Tables_StarchipCap() == 0);
    add("lim6", "{\"limits\": 5}");
    assert(notes == 4);
    /* A key misspelt inside life_points or two_player is said too. */
    add("lim7", "{\"limits\": {\"life_points\": {\"strat\": 9000}, \"two_player\": {\"stpe\": 100}}}");
    assert(notes == 6 && Tables_StartingLifePoints(1, 9, 8000) == 32767);

    Tables_Clear();
    assert(Tables_StatCap(0) == 9999 && Tables_StartingLifePoints(1, 8, 8000) == 8000);
    assert(Tables_MaxLifePoints(8000) == 8000 && Tables_StarchipCap() == 999999 && Tables_ChestLimit() == 250);
    assert(Tables_TwoPlayerLifePoints(TABLES_TWO_PLAYER_MAX, 8000) == 8000 && Tables_FreeDuelRecordCap() == 999);

    Tables_Clear();
    {
        unsigned price = 70, password = 0x76184692u;
        assert(!Tables_PasswordShop(3, &price, &password) && price == 70 && password == 0x76184692u);
    }
    assert(Tables_TrapThreshold(0, 500) == 500);
    {
        int bonus;
        assert(!Tables_TerrainBonus(1, 0, &bonus) && !Tables_TerrainBonus(6, 19, &bonus));
        assert(Tables_EquipBonus(20, 12, 500) == 500);
    }
    {
        unsigned starchips = 7;
        assert(Tables_ChestLimit() == 250 && Tables_ChestOverflow(250, &starchips) == 0 && starchips == 7);
        assert(!Tables_ChestFull(255));
    }
    assert(fusion(10, 11) == -1 && Tables_Equip(20, 12) == -1 && Tables_Ritual(21, own) == -1);
    assert(!Tables_PoolFor(15, TABLES_POOL_DECK, retail));
    {
        unsigned short deck[TABLES_DECK_SIZE];
        assert(!Tables_FixedDeck(15, deck));
    }
    while (document_count) Json_Free(documents[--document_count]);
    puts("tables: ok");
    return 0;
}
