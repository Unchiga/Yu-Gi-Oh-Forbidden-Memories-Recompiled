/* The deck a new game starts with, as mods write it down
 * (src/pc/cards/starter.c, notes/starter-deck.md): real manifests through the
 * real JSON reader, over a made-up card table where a card is named by its
 * id. What is checked without a screen: a deck's size, the copies, the id
 * order it is dealt in, the notes a deck that cannot be dealt raises, the
 * weights a roll picks by, and the decks of several mods adding up.
 *
 * A deck of forty from cards nobody has more than three of takes fourteen of
 * them, so the decks here are filled out by `filler`: every case is an
 * ordinary deck but for the one thing it is checking. */
#include "../../src/pc/cards/starter.c"
#include <stdarg.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);     \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

/* --- the port around starter.c ----------------------------------------- */

int gCard_nCount = CARD_COUNT;

/* A card is named by its id, so a deck reads as the ids it deals. Ids past
 * the disc's CARD_COUNT stand for the cards a mod adds. */
int Cards_Named(const char *text)
{
    char *end;
    long id;
    if (!text || !*text) return 0;
    id = strtol(text, &end, 10);
    return !*end && id >= CARD_ID_FIRST && id <= 2000 ? (int)id : 0;
}
int Cards_ExodiaPiece(int id)
{
    return id >= EXODIA_FIRST_CARD_ID && id < EXODIA_CARD_ID_END;
}

static int notes;
static char note[512], noted[64]; /* noted: the mod the last note is beside */
void Mods_Note(const char *id, const char *format, ...)
{
    va_list arguments;
    snprintf(noted, sizeof(noted), "%s", id);
    notes++;
    va_start(arguments, format);
    vsnprintf(note, sizeof(note), format, arguments);
    va_end(arguments);
}
int Log_Wanted(LogChannel channel) { (void)channel; return 0; }
void Log_Printf(LogChannel channel, const char *format, ...) { (void)channel; (void)format; }

/* Starter_Build's mod list: the tests call Starter_Add themselves. */
static const char *const loaded_ids[] = {"test", "first", "second"};
static const char *const loaded_names[] = {"Test Mod", "First Mod", "Second Mod"};
static int loaded; /* how many of them are loaded: the notes name a mod by its name */
int Mods_LoadedCount(void) { return loaded; }
int Mods_Loaded(int index) { return index; }
int Mods_Active(int mod) { (void)mod; return 0; }
const char *Mods_Id(int mod) { return loaded_ids[mod]; }
const char *Mods_Name(int mod) { return loaded_names[mod]; }
const JsonValue *Mods_Manifest(int mod) { (void)mod; return NULL; }

/* --- the harness -------------------------------------------------------- */

/* A manifest's decks. The document goes at once: a block of this unit's is in a save
 * state and the manifest is not, so a deck's or a pool's "name" must be a copy. */
static void add(const char *mod, const char *text)
{
    char error[256];
    JsonDocument *document = Json_Parse(text, error, sizeof(error));
    CHECK(document != NULL);
    Starter_Add(mod, Json_Root(document));
    Json_Free(document);
}

static void one(const char *text)
{
    Starter_Clear();
    notes = 0;
    note[0] = 0;
    add("test", text);
}

/* The rest of a deck of forty after `used` cards: distinct cards three at a
 * time, from ids that are neither Exodia's nor a mod's. Each member comes
 * with the comma before it, to follow whatever the case wrote. */
static const char *filler(int used)
{
    static char text[512];
    int at = 0, id = 100, left = STARTER_DECK_SIZE - used;
    text[0] = 0;
    while (left > 0) {
        int n = left < DECK_CARD_COPY_LIMIT ? left : DECK_CARD_COPY_LIMIT;
        at += snprintf(text + at, sizeof(text) - (size_t)at, ",\"%d\":%d", id++, n);
        left -= n;
    }
    return text;
}

/* The game's rand() for Starter_DealPools: the numbers a case wants, in
 * order, and the last one again once they run out. A pool spreads one over
 * its own weight total, so a case picks a card by naming the number. */
static unsigned rolls[256];
static int roll_count, roll_at;

static unsigned next_roll(void)
{
    if (roll_at >= roll_count) return roll_count ? rolls[roll_count - 1] : 0;
    return rolls[roll_at++];
}

static void rolling(int count, ...)
{
    va_list arguments;
    int i;
    va_start(arguments, count);
    for (i = 0; i < count && i < (int)(sizeof(rolls) / sizeof(*rolls)); i++) {
        rolls[i] = (unsigned)va_arg(arguments, unsigned);
    }
    va_end(arguments);
    roll_count = count;
    roll_at = 0;
}

int main(void)
{
    unsigned short cards[STARTER_DECK_SIZE];
    const char *name;
    char text[1024];
    int i;

    /* No "starter" at all: the disc's pools stand. */
    one("{\"id\":\"quiet\"}");
    CHECK(Starter_Count() == 0);
    CHECK(Starter_WeightTotal() == 0);
    CHECK(!Starter_Deck(0, cards, &name));
    CHECK(notes == 0);

    /* One deck of forty, dealt in id order whatever order it was written in. */
    snprintf(text, sizeof(text), "{\"starter\":{\"name\":\"Giants\",\"700\":3,\"3\":3,\"41\":3%s}}", filler(9));
    one(text);
    CHECK(Starter_Count() == 1);
    CHECK(Starter_WeightTotal() == 1);   /* a deck without a weight weighs 1 */
    CHECK(notes == 0);
    CHECK(Starter_Deck(0, cards, &name));
    CHECK(name && !strcmp(name, "Giants"));
    CHECK(cards[0] == 3 && cards[2] == 3);
    CHECK(cards[3] == 41 && cards[5] == 41);
    CHECK(cards[6] == 100);
    CHECK(cards[37] == 700 && cards[39] == 700);
    for (i = 1; i < STARTER_DECK_SIZE; i++) CHECK(cards[i - 1] <= cards[i]);   /* in id order */

    /* A card the disc has not got is dealt like any other: naming the cards
     * is the whole point of writing a deck down. */
    one("{\"starter\":{\"900\":3,\"901\":3,\"902\":3,\"903\":3,\"904\":3,\"905\":3,\"906\":3,"
        "\"907\":3,\"908\":3,\"909\":3,\"910\":3,\"911\":3,\"912\":3,\"913\":1}}");
    CHECK(Starter_Count() == 1 && notes == 0);
    CHECK(Starter_Deck(0, cards, NULL));
    CHECK(cards[0] == 900 && cards[STARTER_DECK_SIZE - 1] == 913);

    /* Not forty cards: the save holds forty, so neither size is a deck. */
    snprintf(text, sizeof(text), "{\"starter\":{\"3\":2%s}}", filler(3));
    one(text);
    CHECK(Starter_Count() == 0 && notes == 1);
    CHECK(strstr(note, "40 cards") && strstr(note, "39"));
    snprintf(text, sizeof(text), "{\"starter\":{\"3\":3%s,\"200\":1}}", filler(3));
    one(text);
    CHECK(Starter_Count() == 0 && notes == 1 && strstr(note, "41"));

    /* A card that is not one: noted, and the forty are short without it. */
    snprintf(text, sizeof(text), "{\"starter\":{\"Blue-eyes\":3%s}}", filler(3));
    one(text);
    CHECK(Starter_Count() == 0);
    CHECK(notes == 2);   /* no such card, then the size */

    /* Copies outside 0 to forty: noted, and that card is left out. */
    snprintf(text, sizeof(text), "{\"starter\":{\"3\":-1%s}}", filler(0));
    one(text);
    CHECK(Starter_Count() == 1 && notes == 1 && strstr(note, "copies"));

    /* A weight outside its own range, and one that is not a number. */
    snprintf(text, sizeof(text), "{\"starter\":{\"weight\":-1%s}}", filler(0));
    one(text);
    CHECK(Starter_Count() == 0 && notes == 1 && strstr(note, "weight"));
    snprintf(text, sizeof(text), "{\"starter\":{\"weight\":\"2\"%s}}", filler(0));
    one(text);
    CHECK(Starter_Count() == 0 && notes == 1 && strstr(note, "weight"));

    /* Zero copies are no card at all, and leave the forty to the others. */
    snprintf(text, sizeof(text), "{\"starter\":{\"3\":0%s}}", filler(0));
    one(text);
    CHECK(Starter_Count() == 1 && notes == 0);
    CHECK(Starter_Deck(0, cards, NULL) && cards[0] == 100);

    /* What Build Deck would not take back is said, and the deck is dealt as
     * written: the author wrote down every copy. */
    snprintf(text, sizeof(text), "{\"starter\":{\"5\":4%s}}", filler(4));
    one(text);
    CHECK(Starter_Count() == 1 && notes == 1);
    CHECK(strstr(note, "more than 3 copies"));
    CHECK(Starter_Deck(0, cards, NULL) && cards[0] == 5 && cards[3] == 5 && cards[4] == 100);
    snprintf(text, sizeof(text), "{\"starter\":{\"17\":2%s}}", filler(2));
    one(text);
    CHECK(Starter_Count() == 1 && notes == 1 && strstr(note, "Exodia"));
    /* A card named twice is one card, and its copies add up. */
    snprintf(text, sizeof(text), "{\"starter\":{\"5\":2,\"05\":2%s}}", filler(4));
    one(text);
    CHECK(Starter_Count() == 1 && notes == 1 && strstr(note, "1 card with more than 3 copies"));
    /* One piece each raises nothing. */
    snprintf(text, sizeof(text), "{\"starter\":{\"17\":1,\"18\":1,\"19\":1,\"20\":1,\"21\":1%s}}", filler(5));
    one(text);
    CHECK(Starter_Count() == 1 && notes == 0);

    /* A list of decks, picked by their weights: 3 and 1 of 4. */
    snprintf(text, sizeof(text), "{\"starter\":[{\"name\":\"a\",\"weight\":3,\"3\":3%s},"
                                 "{\"name\":\"b\",\"4\":3%s}]}", filler(3), filler(3));
    one(text);
    CHECK(Starter_Count() == 2);
    CHECK(Starter_WeightTotal() == 4);
    for (i = 0; i < 3; i++) {
        CHECK(Starter_Deck((unsigned)i, cards, &name) && !strcmp(name, "a"));
    }
    CHECK(Starter_Deck(3, cards, &name) && !strcmp(name, "b"));

    /* One of the game's random numbers (0 to 0x7FFF) reaches every deck,
     * however far past 32768 the weights add up. */
    snprintf(text, sizeof(text), "{\"starter\":[{\"name\":\"a\",\"weight\":32767,\"3\":3%s},"
                                 "{\"name\":\"b\",\"weight\":32767,\"4\":3%s}]}", filler(3), filler(3));
    one(text);
    CHECK(Starter_WeightTotal() == 65534);
    CHECK(Starter_Deck(Starter_Roll(0), cards, &name) && !strcmp(name, "a"));
    CHECK(Starter_Deck(Starter_Roll(0x3FFF), cards, &name) && !strcmp(name, "a"));
    CHECK(Starter_Deck(Starter_Roll(0x4000), cards, &name) && !strcmp(name, "b"));
    CHECK(Starter_Deck(Starter_Roll(0x7FFF), cards, &name) && !strcmp(name, "b"));
    snprintf(text, sizeof(text), "{\"starter\":{\"weight\":32768%s}}", filler(0));
    one(text);
    CHECK(Starter_Count() == 0 && notes == 1 && strstr(note, "weight"));

    /* A weight of nothing is a deck that is never picked. */
    snprintf(text, sizeof(text), "{\"starter\":[{\"name\":\"never\",\"weight\":0,\"3\":3%s},"
                                 "{\"name\":\"always\",\"4\":3%s}]}", filler(3), filler(3));
    one(text);
    CHECK(Starter_Count() == 2 && Starter_WeightTotal() == 1);
    CHECK(Starter_Deck(0, cards, &name) && !strcmp(name, "always"));

    /* Every deck weighing nothing leaves the disc's pools to it. */
    snprintf(text, sizeof(text), "{\"starter\":[{\"weight\":0,\"3\":3%s}]}", filler(3));
    one(text);
    CHECK(Starter_Count() == 1 && Starter_WeightTotal() == 0);
    CHECK(!Starter_Deck(0, cards, NULL));

    /* A roll past every weight still deals a deck rather than nothing. */
    snprintf(text, sizeof(text), "{\"starter\":{\"3\":3%s}}", filler(3));
    one(text);
    CHECK(Starter_Deck(99, cards, NULL) && cards[0] == 3);
    /* And a deck without a name has none. */
    CHECK(Starter_DeckAt(0, cards, &name) && name == NULL);

    /* The decks of several mods add up, in the order the mods load. */
    Starter_Clear();
    notes = 0;
    snprintf(text, sizeof(text), "{\"starter\":{\"name\":\"one\",\"3\":3%s}}", filler(3));
    add("first", text);
    snprintf(text, sizeof(text), "{\"starter\":[{\"name\":\"two\",\"4\":3%s},"
                                 "{\"name\":\"three\",\"5\":3%s}]}", filler(3), filler(3));
    add("second", text);
    CHECK(Starter_Count() == 3 && Starter_WeightTotal() == 3 && notes == 0);
    CHECK(Starter_DeckAt(0, cards, &name) && !strcmp(name, "one") && cards[0] == 3);
    CHECK(Starter_DeckAt(1, cards, &name) && !strcmp(name, "two") && cards[0] == 4);
    CHECK(Starter_DeckAt(2, cards, &name) && !strcmp(name, "three") && cards[0] == 5);
    CHECK(!Starter_DeckAt(3, cards, &name));
    CHECK(!Starter_DeckAt(-1, cards, &name));

    /* "starter" of the wrong shape is noted, not read. */
    one("{\"starter\":40}");
    CHECK(Starter_Count() == 0 && notes == 1 && strstr(note, "list of decks"));
    one("{\"starter\":[40]}");
    CHECK(Starter_Count() == 0 && notes == 1 && strstr(note, "object of cards"));

    /* --- "starter_pools": the disc's seven rows made a mod's to write --- */

    /* Nothing offered: the disc's own rows stand. */
    one("{\"id\":\"quiet\"}");
    CHECK(Starter_PoolCount() == 0 && Starter_PoolDraws() == 0 && !Starter_HasPools());
    rolling(1, 0u);
    CHECK(!Starter_DealPools(next_roll, cards));

    /* Pools whose draws add up to a deck deal one, in id order. A draw
     * spreads its number over the pool's own weight total: 0 reaches the
     * first card, 0x7FFF the last. */
    one("{\"starter_pools\":[{\"name\":\"weak\",\"draws\":20,"
        "\"cards\":{\"7\":1,\"9\":1}},"
        "{\"draws\":20,\"cards\":{\"3\":1}}]}");
    CHECK(notes == 0);
    CHECK(Starter_PoolCount() == 2 && Starter_PoolDraws() == STARTER_DECK_SIZE);
    CHECK(Starter_HasPools());
    rolling(2, 0u, 0x7FFFu);
    CHECK(Starter_DealPools(next_roll, cards));
    for (i = 1; i < STARTER_DECK_SIZE; i++) CHECK(cards[i - 1] <= cards[i]);
    CHECK(cards[0] == 3);                       /* the second pool's twenty */
    CHECK(cards[19] == 3 && cards[20] == 7);    /* then the first pool's */
    CHECK(cards[21] == 9);                      /* 0 took 7, the rest took 9 */

    /* Draws that do not add up to forty are no pools at all: the game reads
     * the disc's rows instead. */
    one("{\"starter_pools\":{\"draws\":39,\"cards\":{\"3\":1}}}");
    CHECK(notes == 0 && Starter_PoolCount() == 1 && Starter_PoolDraws() == 39);
    CHECK(!Starter_HasPools());
    rolling(1, 0u);
    CHECK(!Starter_DealPools(next_roll, cards));

    /* A pool of one card draws it past the copy limit: the retry is bounded,
     * so the deal ends rather than running for ever, and the last draw
     * stands. */
    one("{\"starter_pools\":{\"draws\":40,\"cards\":{\"5\":1}}}");
    CHECK(Starter_HasPools());
    rolling(1, 0u);
    CHECK(Starter_DealPools(next_roll, cards));
    for (i = 0; i < STARTER_DECK_SIZE; i++) CHECK(cards[i] == 5);

    /* A weight of 0 is a card the pool never draws. */
    one("{\"starter_pools\":{\"draws\":40,\"cards\":{\"11\":0,\"12\":1}}}");
    rolling(1, 0u);
    CHECK(Starter_DealPools(next_roll, cards));
    for (i = 0; i < STARTER_DECK_SIZE; i++) CHECK(cards[i] == 12);

    /* A pool that draws cards but weights none is left out, with a note: a
     * weight of 0 is a card the pool never draws, so nothing is left to
     * draw from. Without it the draws no longer add up and the disc's rows
     * stand. */
    one("{\"starter_pools\":[{\"draws\":40,\"cards\":{\"13\":0}}]}");
    CHECK(Starter_PoolCount() == 0 && !Starter_HasPools());
    CHECK(notes == 1 && strstr(note, "weights none"));
    rolling(1, 0u);
    CHECK(!Starter_DealPools(next_roll, cards));

    /* A pool that draws nothing needs no weights, and is kept. */
    one("{\"starter_pools\":[{\"draws\":0,\"cards\":{}},"
        "{\"draws\":40,\"cards\":{\"14\":1}}]}");
    CHECK(notes == 0 && Starter_PoolCount() == 2 && Starter_HasPools());
    rolling(1, 0u);
    CHECK(Starter_DealPools(next_roll, cards));
    CHECK(cards[0] == 14 && cards[39] == 14);

    /* Without the game's own numbers there is nothing to draw by. */
    one("{\"starter_pools\":{\"draws\":40,\"cards\":{\"3\":1}}}");
    CHECK(!Starter_DealPools(NULL, cards));

    /* The pools of several mods add up, in the order the mods load. */
    Starter_Clear();
    notes = 0;
    add("first", "{\"starter_pools\":{\"draws\":16,\"cards\":{\"3\":1}}}");
    add("second", "{\"starter_pools\":{\"draws\":24,\"cards\":{\"4\":1}}}");
    CHECK(notes == 0 && Starter_PoolCount() == 2);
    CHECK(Starter_PoolDraws() == STARTER_DECK_SIZE && Starter_HasPools());
    rolling(1, 0u);
    CHECK(Starter_DealPools(next_roll, cards));
    CHECK(cards[0] == 3 && cards[15] == 3 && cards[16] == 4 && cards[39] == 4);

    /* A written deck and pools together: Starter_Deck still answers, and
     * NameEntry_BuildStarterDeck asks it first. */
    snprintf(text, sizeof(text), "{\"starter\":{\"name\":\"Written\",\"3\":3%s},"
                                 "\"starter_pools\":{\"draws\":40,\"cards\":{\"9\":1}}}", filler(3));
    one(text);
    CHECK(notes == 0 && Starter_Count() == 1 && Starter_HasPools());
    CHECK(Starter_Deck(0, cards, &name) && !strcmp(name, "Written"));

    /* Starter_Check says beside each mod whose pools a new game leaves out:
     * one mod's that do not draw forty, several mods' that do not together,
     * and any beside a written deck that weighs anything. */
    Starter_Check();
    CHECK(notes == 1 && !strcmp(noted, "test") && strstr(note, "test's written starter deck is dealt first"));
    loaded = 3;
    notes = 0;
    Starter_Check();
    CHECK(notes == 1 && strstr(note, ": Test Mod's written starter deck is dealt first"));
    /* Every mod whose decks weigh anything is named, the deck of weight 0 not. */
    Starter_Clear();
    notes = 0;
    snprintf(text, sizeof(text), "{\"starter\":[{\"3\":3%s},{\"weight\":2,\"3\":3%s}]}", filler(3), filler(3));
    add("first", text);
    snprintf(text, sizeof(text), "{\"starter\":{\"weight\":0,\"3\":3%s},"
                                 "\"starter_pools\":{\"draws\":40,\"cards\":{\"9\":1}}}", filler(3));
    add("test", text);
    snprintf(text, sizeof(text), "{\"starter\":{\"3\":3%s}}", filler(3));
    add("second", text);
    CHECK(notes == 0 && Starter_Count() == 4);
    Starter_Check();
    CHECK(notes == 1 && !strcmp(noted, "test") &&
          strstr(note, ": First Mod's and Second Mod's written starter decks are dealt first"));
    loaded = 0;
    one("{\"starter_pools\":{\"draws\":24,\"cards\":{\"3\":1}}}");
    Starter_Check();
    CHECK(notes == 1 && strstr(note, "they draw 24 cards, not 40; the disc's starter decks are dealt"));
    Starter_Clear();
    notes = 0;
    add("first", "{\"starter_pools\":[{\"draws\":30,\"cards\":{\"3\":1}},{\"draws\":10,\"cards\":{\"3\":1}}]}");
    add("second", "{\"starter_pools\":{\"draws\":40,\"cards\":{\"4\":1}}}");
    Starter_Check();
    CHECK(notes == 2 && !strcmp(noted, "second") && strstr(note, "draw 80 cards together, not 40 (these 40)"));
    Starter_Clear();
    notes = 0;
    add("first", "{\"starter_pools\":{\"draws\":16,\"cards\":{\"3\":1}}}");
    add("second", "{\"starter_pools\":{\"draws\":24,\"cards\":{\"4\":1}}}");
    Starter_Check();
    CHECK(notes == 0);
    /* A written deck of weight 0 is never dealt: the pools stand. */
    snprintf(text, sizeof(text), "{\"starter\":{\"weight\":0,\"3\":3%s},"
                                 "\"starter_pools\":{\"draws\":40,\"cards\":{\"9\":1}}}", filler(3));
    one(text);
    Starter_Check();
    CHECK(notes == 0 && Starter_HasPools());

    /* What the reader refuses, and says so about. */
    one("{\"starter_pools\":40}");
    CHECK(Starter_PoolCount() == 0 && notes == 1 && strstr(note, "pool, or a list of pools"));
    one("{\"starter_pools\":[40]}");
    CHECK(Starter_PoolCount() == 0 && notes == 1 && strstr(note, "object of \"draws\" and \"cards\""));
    one("{\"starter_pools\":{\"cards\":{\"3\":1}}}");
    CHECK(Starter_PoolCount() == 0 && notes == 1 && strstr(note, "how many cards it draws"));
    one("{\"starter_pools\":{\"draws\":41,\"cards\":{\"3\":1}}}");
    CHECK(Starter_PoolCount() == 0 && notes == 1 && strstr(note, "how many cards it draws"));
    one("{\"starter_pools\":{\"draws\":40,\"cards\":3}}");
    CHECK(Starter_PoolCount() == 0 && notes == 1 && strstr(note, "cards and their weights"));
    /* A weight the game could not read is left out; the pool keeps the cards
     * it could. */
    one("{\"starter_pools\":{\"draws\":40,\"cards\":{\"3\":70000,\"4\":1}}}");
    CHECK(notes == 1 && strstr(note, "a weight is 0 to"));
    CHECK(Starter_PoolCount() == 1 && Starter_HasPools());
    rolling(1, 0u);
    CHECK(Starter_DealPools(next_roll, cards) && cards[0] == 4 && cards[39] == 4);
    one("{\"starter_pools\":{\"draws\":40,\"cards\":{\"3\":1},\"odds\":2}}");
    CHECK(notes == 1 && strstr(note, "no \"odds\" in a pool"));

    Starter_Clear();
    printf("starter deck: ok\n");
    return 0;
}
