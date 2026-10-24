/* The deck a new game starts with, as mods write it down (starter.h,
 * notes/starter-deck.md).
 *
 * Each applied mod's "starter" is read into the decks here, once, in the
 * order the mods load, and they add up: a mod adds its decks to whatever the
 * mods before it offered rather than replacing them. NameEntry_BuildStarterDeck
 * asks for one before it reads the disc's pools, and takes the disc's path
 * when no mod offers any.
 *
 * A deck is forty cards written down by their copies, as an opponent's fixed
 * deck is (tables.c). The counts are the deck, so the three-copy limit of a
 * dealt deck does not apply here either: the author wrote down every copy.
 * What a player would not be able to rebuild in Build Deck afterwards is
 * said in the Mods window, and the deck is dealt as written. */
#include "starter.h"
#include "cards.h"
#include "pc/mods/mods.h"
#include "pc/mods/json.h"
#include "pc/debug/log.h"
#include "game/card_constants.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *mod;    /* the manifest's, which outlives these */
    const char *name;   /* its "name", or NULL for the mod's own */
    unsigned weight;
    unsigned short cards[STARTER_DECK_SIZE];
} StarterDeck;

static StarterDeck *decks;
static int deck_count, deck_room;

static void *grow(void *array, int *room, int count, size_t size)
{
    void **slot = (void **)array;
    if (count >= *room) {
        int wanted = *room ? *room * 2 : 8;
        void *bigger = realloc(*slot, (size_t)wanted * size);
        if (!bigger) return NULL;
        *slot = bigger;
        *room = wanted;
    }
    return (char *)*slot + (size_t)count * size;
}

typedef struct {
    int id, copies;
} Copies;

static int by_id(const void *left, const void *right)
{
    return ((const Copies *)left)->id - ((const Copies *)right)->id;
}

/* A deck's own keys, which are not cards. */
static int control_key(const char *name)
{
    return !strcmp(name, "name") || !strcmp(name, "weight");
}

/* The most a weight may be: the game's rand() tops out at 0x7FFF, and
 * Starter_Roll spreads one of its numbers over every deck's weight. */
#define STARTER_WEIGHT_LIMIT 32767

/* One deck: { "name": ..., "weight": ..., card: copies, ... }, the copies
 * adding up to the forty cards of a deck. */
static void read_deck(const char *mod, const char *where, const JsonValue *entry)
{
    Copies *list;
    StarterDeck deck, *slot;
    const JsonValue *weight_value;
    long weight;
    int i, n = 0, named, dealt = 0, total = 0, pieces = 0, over = 0;
    if (Json_TypeOf(entry) != JSON_OBJECT) {
        Mods_Note(mod, "%s: a starter deck is an object of cards and their copies", where);
        return;
    }
    weight_value = Json_Member(entry, "weight");
    weight = Json_Number(weight_value, 1);
    if (weight_value && (Json_TypeOf(weight_value) != JSON_NUMBER || weight < 0 || weight > STARTER_WEIGHT_LIMIT)) {
        Mods_Note(mod, "%s: a deck's \"weight\" is how often it is picked, 0 to %d", where, STARTER_WEIGHT_LIMIT);
        return;
    }
    list = calloc((size_t)Json_Count(entry) + 1, sizeof(*list));
    if (!list) return;
    for (i = 0; i < Json_Count(entry); i++) {
        const JsonValue *member = Json_At(entry, i);
        const char *name = Json_Name(member);
        long copies;
        int id;
        char at[160];
        if (control_key(name)) continue;
        snprintf(at, sizeof(at), "%s \"%s\"", where, name);
        id = Cards_Named(name);
        if (id <= 0) {
            Mods_Note(mod, "%s: no such card", at);
            continue;
        }
        copies = Json_Number(member, -1);
        if (Json_TypeOf(member) != JSON_NUMBER || copies < 0 || copies > STARTER_DECK_SIZE) {
            Mods_Note(mod, "%s: a starter deck gives each card its copies, 0 to %d", at, STARTER_DECK_SIZE);
            continue;
        }
        total += (int)copies;
        if (!copies) continue;
        list[n].id = id;
        list[n++].copies = (int)copies;
    }
    /* The save holds forty cards (SaveDataState's player_deck), so a deck of
     * any other size is not a deck this can deal. */
    if (total != STARTER_DECK_SIZE) {
        Mods_Note(mod, "%s: a starter deck is %d cards, and this one has %d; left out", where, STARTER_DECK_SIZE,
                  total);
        free(list);
        return;
    }
    /* In id order, as the disc's own pools are read; a card named twice (by
     * name and by number) is one card with both counts. */
    qsort(list, (size_t)n, sizeof(*list), by_id);
    for (i = 0, named = n, n = 0; i < named; i++) {
        if (n && list[n - 1].id == list[i].id) list[n - 1].copies += list[i].copies;
        else list[n++] = list[i];
    }
    /* What Build Deck would not let the player put back, once they take the
     * deck apart: its three-copy limit, and one Exodia piece each. */
    for (i = 0; i < n; i++) {
        if (list[i].copies > DECK_CARD_COPY_LIMIT) over++;
        if (Cards_ExodiaPiece(list[i].id) && list[i].copies > 1) pieces++;
    }
    if (over) {
        Mods_Note(mod, "%s: %d card%s with more than %d copies; dealt as written, and Build Deck will not take "
                       "them back", where, over, over == 1 ? "" : "s", DECK_CARD_COPY_LIMIT);
    }
    if (pieces) {
        Mods_Note(mod, "%s: %d Exodia piece%s with more than one copy; dealt as written, and Build Deck will not "
                       "take them back", where, pieces, pieces == 1 ? "" : "s");
    }
    memset(&deck, 0, sizeof(deck));
    deck.mod = mod;
    deck.name = Json_String(Json_Member(entry, "name"), NULL);
    deck.weight = (unsigned)weight;
    for (i = 0; i < n; i++) {
        int copy;
        for (copy = 0; copy < list[i].copies; copy++) deck.cards[dealt++] = (unsigned short)list[i].id;
    }
    free(list);
    slot = grow(&decks, &deck_room, deck_count, sizeof(*decks));
    if (!slot) return;
    *slot = deck;
    deck_count++;
}

/* "starter": one deck, or a list of them. */
static void read_starter(const char *mod, const JsonValue *value)
{
    char where[64];
    int i;
    if (!value) return;
    if (Json_TypeOf(value) == JSON_OBJECT) {
        read_deck(mod, "starter", value);
        return;
    }
    if (Json_TypeOf(value) != JSON_ARRAY) {
        Mods_Note(mod, "\"starter\" is a deck, or a list of decks");
        return;
    }
    for (i = 0; i < Json_Count(value); i++) {
        snprintf(where, sizeof(where), "starter %d", i + 1);
        read_deck(mod, where, Json_At(value, i));
    }
}

/* --- pools of the mod's own ------------------------------------------------
 *
 * "starter_pools" is the disc's seven rows made a mod's to write: each pool
 * draws its own number of cards from its own weights. The disc keeps its
 * weights in a table of a fixed width whose first 720 only are read, so a
 * card a mod adds can never be drawn from it; a pool here names cards the way
 * the rest of a manifest does, so a mod's own are weighted like any other. */

typedef struct {
    int id;
    unsigned weight;
} StarterWeight;

typedef struct {
    const char *mod;        /* the manifest's, which outlives these */
    const char *name;       /* its "name", for the log */
    int draws;
    StarterWeight *cards;
    int count;
    unsigned total;         /* its weights added up */
} StarterPool;

static StarterPool *pools;
static int pool_count, pool_room;

/* The most one weight may be, as a pool's total must stay countable. */
#define STARTER_POOL_WEIGHT_LIMIT 65535

static int pool_control_key(const char *name)
{
    return !strcmp(name, "name") || !strcmp(name, "draws") || !strcmp(name, "cards");
}

/* One pool: { "name": ..., "draws": n, "cards": { card: weight, ... } }. */
static void read_pool(const char *mod, const char *where, const JsonValue *entry)
{
    StarterPool pool, *slot;
    const JsonValue *draws_value, *cards;
    StarterWeight *list;
    long draws;
    int i, n = 0, room;

    if (Json_TypeOf(entry) != JSON_OBJECT) {
        Mods_Note(mod, "%s: a starter pool is an object of \"draws\" and \"cards\"", where);
        return;
    }
    draws_value = Json_Member(entry, "draws");
    draws = Json_Number(draws_value, -1);
    if (Json_TypeOf(draws_value) != JSON_NUMBER || draws < 0 || draws > STARTER_DECK_SIZE) {
        Mods_Note(mod, "%s: a pool's \"draws\" is how many cards it draws, 0 to %d", where, STARTER_DECK_SIZE);
        return;
    }
    cards = Json_Member(entry, "cards");
    if (Json_TypeOf(cards) != JSON_OBJECT) {
        Mods_Note(mod, "%s: a pool's \"cards\" is an object of cards and their weights", where);
        return;
    }
    for (i = 0; i < Json_Count(entry); i++) {
        const char *name = Json_Name(Json_At(entry, i));
        if (name && !pool_control_key(name)) {
            Mods_Note(mod, "%s: no \"%s\" in a pool (name, draws, cards)", where, name);
        }
    }
    room = Json_Count(cards);
    list = calloc((size_t)room + 1, sizeof(*list));
    if (!list) return;
    memset(&pool, 0, sizeof(pool));
    for (i = 0; i < room; i++) {
        const JsonValue *member = Json_At(cards, i);
        const char *name = Json_Name(member);
        long weight;
        int id, seen;
        char at[160];
        snprintf(at, sizeof(at), "%s \"%s\"", where, name);
        id = Cards_Named(name);
        if (id <= 0) {
            Mods_Note(mod, "%s: no such card", at);
            continue;
        }
        weight = Json_Number(member, -1);
        if (Json_TypeOf(member) != JSON_NUMBER || weight < 0 || weight > STARTER_POOL_WEIGHT_LIMIT) {
            Mods_Note(mod, "%s: a weight is 0 to %d", at, STARTER_POOL_WEIGHT_LIMIT);
            continue;
        }
        if (!weight) continue;
        /* A card named twice (by name and by number) is one card, its weights
         * added, as a written deck's copies are. */
        for (seen = 0; seen < n; seen++) {
            if (list[seen].id == id) break;
        }
        if (seen < n) {
            list[seen].weight += (unsigned)weight;
        } else {
            list[n].id = id;
            list[n++].weight = (unsigned)weight;
        }
        pool.total += (unsigned)weight;
    }
    if (draws && !n) {
        Mods_Note(mod, "%s: draws %ld card%s but weights none; left out", where, draws, draws == 1 ? "" : "s");
        free(list);
        return;
    }
    pool.mod = mod;
    pool.name = Json_String(Json_Member(entry, "name"), NULL);
    pool.draws = (int)draws;
    pool.cards = list;
    pool.count = n;
    slot = grow(&pools, &pool_room, pool_count, sizeof(*pools));
    if (!slot) {
        free(list);
        return;
    }
    *slot = pool;
    pool_count++;
}

/* "starter_pools": one pool, or a list of them. */
static void read_starter_pools(const char *mod, const JsonValue *value)
{
    char where[64];
    int i;
    if (!value) return;
    if (Json_TypeOf(value) == JSON_OBJECT) {
        read_pool(mod, "starter_pools", value);
        return;
    }
    if (Json_TypeOf(value) != JSON_ARRAY) {
        Mods_Note(mod, "\"starter_pools\" is a pool, or a list of pools");
        return;
    }
    for (i = 0; i < Json_Count(value); i++) {
        snprintf(where, sizeof(where), "starter_pools %d", i + 1);
        read_pool(mod, where, Json_At(value, i));
    }
}

int Starter_PoolCount(void)
{
    return pool_count;
}

int Starter_PoolDraws(void)
{
    int total = 0, i;
    for (i = 0; i < pool_count; i++) total += pools[i].draws;
    return total;
}

int Starter_HasPools(void)
{
    return pool_count && Starter_PoolDraws() == STARTER_DECK_SIZE;
}

/* The card a roll picks out of one pool: its weights walked until they reach
 * the threshold, as the disc's generator walks its row. */
static int pool_card(const StarterPool *pool, unsigned random)
{
    unsigned long long threshold;
    unsigned acc = 0;
    int i;
    if (!pool->count || !pool->total) return 0;
    /* The disc asks for (rand() & 0x7FF) + 1 against weights that add up to
     * 2048; a pool here may add up to anything, so the number is spread over
     * its own total instead. */
    threshold = (unsigned long long)(random & 0x7FFFu) * pool->total / 0x8000u + 1;
    for (i = 0; i < pool->count; i++) {
        acc += pool->cards[i].weight;
        if (acc >= threshold) return pool->cards[i].id;
    }
    return pool->cards[pool->count - 1].id;
}

static int by_card_id(const void *left, const void *right)
{
    return (int)((const unsigned short *)left)[0] - (int)((const unsigned short *)right)[0];
}

int Starter_DealPools(unsigned (*next_random)(void), unsigned short cards[STARTER_DECK_SIZE])
{
    int held[STARTER_DECK_SIZE];
    int dealt = 0, i, draw, capped = 0;
    if (!Starter_HasPools() || !next_random) return 0;
    memset(held, 0, sizeof(held));
    for (i = 0; i < pool_count; i++) {
        const StarterPool *pool = &pools[i];
        for (draw = 0; draw < pool->draws && dealt < STARTER_DECK_SIZE; draw++) {
            int id = 0, tries;
            for (tries = 0; tries <= STARTER_POOL_RETRIES; tries++) {
                int copies = 0, k;
                id = pool_card(pool, next_random());
                if (!id) break;
                for (k = 0; k < dealt; k++) {
                    if (cards[k] == (unsigned short)id) copies++;
                }
                if (copies < DECK_CARD_COPY_LIMIT) break;
                /* Held to the limit already: draw again, as the disc does. */
                if (tries == STARTER_POOL_RETRIES) capped = 1;
            }
            if (!id) continue;
            cards[dealt++] = (unsigned short)id;
        }
    }
    if (dealt != STARTER_DECK_SIZE) {
        LOG(LOG_MODS, "starter: the pools drew %d of %d cards; the disc's rows stand", dealt, STARTER_DECK_SIZE);
        return 0;
    }
    if (capped) {
        LOG(LOG_MODS, "starter: a pool kept drawing a card already held %d times; the last draw stands",
            DECK_CARD_COPY_LIMIT);
    }
    /* In id order, as a written deck's cards are. */
    qsort(cards, STARTER_DECK_SIZE, sizeof(*cards), by_card_id);
    if (Log_Wanted(LOG_MODS)) {
        char text[STARTER_DECK_SIZE * 12];
        int at = 0;
        for (i = 0; i < STARTER_DECK_SIZE; i++) {
            int copies = 1;
            while (i + 1 < STARTER_DECK_SIZE && cards[i + 1] == cards[i]) i++, copies++;
            at += snprintf(text + at, sizeof(text) - (size_t)at, " %dx%d", copies, cards[i]);
        }
        LOG(LOG_MODS, "starter: dealt from %d pool%s (copies x card):%s", pool_count, pool_count == 1 ? "" : "s",
            text);
    }
    return 1;
}

void Starter_Add(const char *mod, const JsonValue *manifest)
{
    read_starter(mod, Json_Member(manifest, "starter"));
    read_starter_pools(mod, Json_Member(manifest, "starter_pools"));
}

void Starter_Clear(void)
{
    int i;
    free(decks);
    decks = NULL;
    deck_count = deck_room = 0;
    for (i = 0; i < pool_count; i++) free(pools[i].cards);
    free(pools);
    pools = NULL;
    pool_count = pool_room = 0;
}

void Starter_Build(void)
{
    static int built;
    int i;
    if (built) return;
    built = 1;
    for (i = 0; i < Mods_LoadedCount(); i++) {
        int mod = Mods_Loaded(i);
        if (Mods_Active(mod)) Starter_Add(Mods_Id(mod), Mods_Manifest(mod));
    }
    Starter_Check();
}

/* A mod's name for a note, by its id; the id when it is not loaded. */
static const char *mod_name(const char *id)
{
    int i;
    for (i = 0; i < Mods_LoadedCount(); i++) {
        int mod = Mods_Loaded(i);
        if (!strcmp(Mods_Id(mod), id)) return Mods_Name(mod);
    }
    return id;
}

/* What a new game will make of the decks and pools, in the log, and beside
 * each mod whose pools are left out (Mods_Note: the Mods window shows it,
 * with one mod or many): a written deck that weighs anything is dealt
 * first, and pools whose draws are not a deck's forty leave it to the
 * disc's rows (NameEntry_DealModStarterDeck). */
void Starter_Check(void)
{
    unsigned weight = Starter_WeightTotal();
    int draws = Starter_PoolDraws(), i, j;
    if (deck_count) {
        LOG(LOG_MODS, "starter: %d deck%s offered, %u weight between them", deck_count, deck_count == 1 ? "" : "s",
            weight);
    }
    if (!pool_count) return;
    if (weight) {
        LOG(LOG_MODS, "starter: %d pool%s offered, drawing %d cards; left out, as a written deck is dealt first",
            pool_count, pool_count == 1 ? "" : "s", draws);
    } else if (draws != STARTER_DECK_SIZE) {
        LOG(LOG_MODS, "starter: %d pool%s offered, drawing %d cards, not the %d a deck holds; the disc's rows stand",
            pool_count, pool_count == 1 ? "" : "s", draws, STARTER_DECK_SIZE);
    } else {
        LOG(LOG_MODS, "starter: %d pool%s offered, drawing %d of %d cards", pool_count, pool_count == 1 ? "" : "s",
            draws, STARTER_DECK_SIZE);
        return;
    }
    /* Once for each mod with pools, at its first. */
    for (i = 0; i < pool_count; i++) {
        const char *mod = pools[i].mod;
        int own = 0, mods = 0;
        for (j = 0; j < i && strcmp(pools[j].mod, mod); j++) {
        }
        if (j < i) continue;
        for (j = 0; j < pool_count; j++) {
            if (!strcmp(pools[j].mod, mod)) own += pools[j].draws;
            if (!j || strcmp(pools[j].mod, pools[j - 1].mod)) mods++;
        }
        if (weight) {
            /* Every mod whose decks weigh anything, by name, as the Mods
             * window's overlap line names them: "A's", "A's and B's". */
            char who[600] = "";
            int writers = 0, written = 0, dealt = 0, k;
            for (j = 0; j < deck_count; j++) {
                if (!decks[j].weight) continue;
                dealt++;
                for (k = 0; k < j && (!decks[k].weight || strcmp(decks[k].mod, decks[j].mod)); k++) {
                }
                writers += k == j;
            }
            for (j = 0; j < deck_count; j++) {
                size_t at = strlen(who);
                if (!decks[j].weight) continue;
                for (k = 0; k < j && (!decks[k].weight || strcmp(decks[k].mod, decks[j].mod)); k++) {
                }
                if (k < j) continue;
                written++;
                snprintf(who + at, sizeof(who) - at, "%s%s's", written == 1 ? "" : written == writers ? " and " : ", ",
                         mod_name(decks[j].mod));
            }
            Mods_Note(mod, "starter_pools left out: %s written starter deck%s %s dealt first", who,
                      dealt > 1 ? "s" : "", dealt > 1 ? "are" : "is");
        } else if (mods == 1) {
            Mods_Note(mod, "starter_pools left out: they draw %d cards, not %d; the disc's starter decks are dealt",
                      own, STARTER_DECK_SIZE);
        } else {
            Mods_Note(mod, "starter_pools left out: every mod's pools draw %d cards together, not %d (these %d); the "
                           "disc's starter decks are dealt",
                      draws, STARTER_DECK_SIZE, own);
        }
    }
}

int Starter_Count(void)
{
    return deck_count;
}

unsigned Starter_WeightTotal(void)
{
    unsigned total = 0;
    int i;
    for (i = 0; i < deck_count; i++) total += decks[i].weight;
    return total;
}

unsigned Starter_Roll(unsigned random)
{
    unsigned long long total = Starter_WeightTotal();
    /* The game's rand() is 0 to 0x7FFF, which a plain `% total` would leave
     * short of any deck past its 32768th weight. */
    return total ? (unsigned)((random & 0x7FFFu) * total / 0x8000u) : 0;
}

static int write_deck(const StarterDeck *deck, unsigned short cards[STARTER_DECK_SIZE], const char **name)
{
    memcpy(cards, deck->cards, sizeof(deck->cards));
    if (name) *name = deck->name;
    if (Log_Wanted(LOG_MODS)) {
        char text[STARTER_DECK_SIZE * 12];
        int at = 0, i;
        for (i = 0; i < STARTER_DECK_SIZE; i++) {
            int copies = 1;
            while (i + 1 < STARTER_DECK_SIZE && deck->cards[i + 1] == deck->cards[i]) i++, copies++;
            at += snprintf(text + at, sizeof(text) - (size_t)at, " %dx%d", copies, deck->cards[i]);
        }
        LOG(LOG_MODS, "starter: \"%s\" from %s (copies x card):%s", deck->name ? deck->name : "a deck", deck->mod,
            text);
    }
    return 1;
}

int Starter_DeckAt(int index, unsigned short cards[STARTER_DECK_SIZE], const char **name)
{
    if (index < 0 || index >= deck_count) return 0;
    return write_deck(&decks[index], cards, name);
}

int Starter_Deck(unsigned roll, unsigned short cards[STARTER_DECK_SIZE], const char **name)
{
    unsigned acc = 0;
    int i;
    for (i = 0; i < deck_count; i++) {
        acc += decks[i].weight;
        if (roll < acc) return write_deck(&decks[i], cards, name);
    }
    /* Past every weight: the roll came from a total these decks no longer
     * add up to, or they all weigh nothing. The last one that weighs
     * anything is a deck; no deck at all leaves the disc's pools to it. */
    for (i = deck_count - 1; i >= 0; i--) {
        if (decks[i].weight) return write_deck(&decks[i], cards, name);
    }
    return 0;
}
