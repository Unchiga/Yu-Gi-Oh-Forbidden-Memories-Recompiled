/* What two or more mods both change (overlap.h, notes/modding.md "When mods
 * overlap").
 *
 * Every mod's manifest becomes claims: one for each thing it sets (a card, a
 * fusion pair, a pool, a limit, an image...), with a key naming the thing, a
 * mode (it sets it, adds to it, defines it, hooks it) and a hash of what it
 * sets. The claims are sorted by kind and key; a run of claims from two or
 * more mods is an overlap, and the load order decides how it comes out, the
 * way the reader of that key decides it. A kind fewer than two mods have is
 * not looked at, so one mod (or none) costs nothing.
 *
 * Some keys reach things they do not name: "all" (drops, decks, passwords),
 * "stats" (the attack and defense caps), a life-point start (both sides),
 * "replace" (guardian star matchups, terrain bonuses) and a new star's first
 * declaration (its matchups). Such a claim is "wide", and stands, besides its
 * own key, beside every key of its kind another mod names that it covers; one
 * that resets (a "replace") reaches only what earlier mods set.
 *
 * Every key is read as the type its reader takes, a list or an object, and
 * anything else as nothing, as the readers note it and leave it out. */
#include "pc/compat/fs.h" /* UTF-8 paths on Windows too (notes/pc-build.md) */
#include "overlap.h"
#include "json.h"
#include "pc/platform/paths.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SET, ADD, FIXED, BASE, CHAIN, EVENT, FIRST };   /* what a claim does */
enum { VIA_WIDE = 1, AIMED = 2, RESETS = 4, CARD_PW = 8 }; /* claim flags (CARD_PW: a card's password) */
enum {
    O_LATER, O_AFTER, O_AGREE, O_ADD, O_RESET, O_FIXED, O_KEYS, O_BYTES, O_CHAIN, O_EVENTS, O_FIRST, O_AIMED,
    O_PATCHED, O_EARLY, O_SOLD
};
static const char *const outcome_words[] = {"later", "after", "agree", "add",   "reset",   "fixed", "keys",
                                            "bytes", "chain", "events", "first", "aimed", "patched", "early",
                                            "sold"};

#define CARD_COUNT 722             /* the disc's cards: what "replace" takes */
#define TEXT_KEY (1ull << 40)      /* a card named by letters no card has: its letters' hash */
#define ALL_CARDS ((1ull << 41) - 1)
#define SUB(n) ((uint64_t)(n) << 56)
#define SUB_MASK SUB(0xFF)
#define RESET_SUB 0xFE
#define DEFAULT_EQUIP_BONUS (1ull << 62)
#define EQUIP_CARDS (1ull << 20)  /* an equip's key: base << 20 | card, for cards below this */
#define ALL_DUELISTS ((1ull << 47) - 1)
#define RAW_SECTORS (3ull << 62)   /* a data key: raw sectors, by the first of a run that meets (a file's: 2 << 62) */
#define ATTACK_TRAP_FIRST 681      /* House of Adhesive Tape .. Widespread Ruin: 681 to 686 */
#define ATTACK_TRAPS 6

typedef struct {
    /* mask: a wide claim, beside every key k with (k & mask) == match, or
     * (k & mask2) == match2 when mask2 is set */
    uint64_t key, mask, match, mask2, match2;
    long long lo, hi;          /* raw sectors: the first and the last a "data" entry reaches */
    uint32_t value;            /* what it sets, hashed: equal values agree */
    int mod, seq, label, via;  /* label, via: in the strings, or -1 (via: the wide key's own name) */
    unsigned char kind, mode, flags;
    const JsonValue *src;
} Claim;

typedef struct {
    int first, count, winner, other, outcome, best; /* best: the claim a password gives (O_SOLD) */
    unsigned char kind, severity;
} Group;

typedef struct {
    uint64_t hash;
    int id;
} Memo;

typedef struct {
    uint64_t name;
    int id;
} StarName;

/* A duelist a mod defines (its duelists/ folder and "duelists" list): its
 * pool files name it by its own id, which stands for whom it replaces. */
typedef struct {
    int mod;
    char id[64];
    uint64_t replaces; /* the duelist key, or 0 for a new one */
    char who[96];      /* whom it replaces, as the mod wrote it: its pools' label */
} Defined;

struct ModsOverlaps {
    ModsOverlapMod *mods;
    int mod_count;
    char (*names)[96];  /* the mods' names, as the game keeps them (fit_name) */
    char *name_list;    /* room for every name of a line, comma-separated */
    size_t name_list_size;
    ModsOverlapSource source;
    Claim *claims;
    int claim_count, claim_room;
    Group *groups;
    int group_count;
    char *strings;
    size_t string_length, string_room;
    JsonDocument **documents;
    int document_count, document_room;
    unsigned char *declared; /* [winner * mod_count + other]: the winner loads after it on purpose */
    Memo *memo;
    int memo_room, memo_count;
    StarName *star_names;
    int star_name_count, star_name_room;
    Defined *defined;
    int defined_count, defined_room;
    unsigned char star_declared[16];
    int seq, failed, packs_declared;
};

/* --- small tools ---------------------------------------------------------- */

static uint64_t fnv(uint64_t hash, const char *text)
{
    while (*text) hash = (hash ^ (unsigned char)*text++) * 1099511628211ull;
    return hash;
}
static uint64_t hash_text(const char *text) { return fnv(14695981039346656037ull, text ? text : ""); }
/* Letters and digits only, lowercased, as the readers compare names
 * (tables.c same_letters): 0 for a name of neither. */
static uint64_t hash_letters(const char *text)
{
    uint64_t hash = 14695981039346656037ull;
    int any = 0;
    for (; text && *text; text++)
        if (isalnum((unsigned char)*text)) {
            hash = (hash ^ (unsigned char)tolower((unsigned char)*text)) * 1099511628211ull;
            any = 1;
        }
    return any ? hash : 0;
}
static int same_letters(const char *a, const char *b) { return hash_letters(a) == hash_letters(b); }
/* A member's name; "" for an element of an array, which has none (a
 * manifest with a list where an object belongs walks its elements). */
static const char *name_of(const JsonValue *value)
{
    const char *name = Json_Name(value);
    return name ? name : "";
}
/* A list, or an object, where the readers take only that: anything else
 * (they note it) reads as nothing. */
static const JsonValue *list_of(const JsonValue *value) { return Json_TypeOf(value) == JSON_ARRAY ? value : NULL; }
static const JsonValue *object_of(const JsonValue *value) { return Json_TypeOf(value) == JSON_OBJECT ? value : NULL; }
static int digits_only(const char *text) { return text && *text && strspn(text, "0123456789") == strlen(text); }
static uint64_t mix(uint64_t a, uint64_t b)
{
    uint64_t hash = (a ^ 0x9E3779B97F4A7C15ull) * 0xBF58476D1CE4E5B9ull;
    return (hash ^ (hash >> 31) ^ b) * 0x94D049BB133111EBull;
}
/* What a value says, its members' names included but not its own (a trap's
 * threshold is the same whether the trap is named "Bear Trap" or "600"). */
static uint32_t hash_json(uint32_t hash, const JsonValue *value, int named)
{
    const JsonValue *child;
    char number[40];
    if (!value) return hash;
    hash = (hash ^ (unsigned)Json_TypeOf(value)) * 16777619u;
    if (named && Json_Name(value)) hash = (uint32_t)fnv(hash, Json_Name(value));
    if (Json_TypeOf(value) == JSON_STRING) hash = (uint32_t)fnv(hash, Json_String(value, ""));
    if (Json_TypeOf(value) == JSON_NUMBER || Json_TypeOf(value) == JSON_BOOL) {
        snprintf(number, sizeof(number), "%ld", Json_Number(value, Json_Bool(value, 0)));
        hash = (uint32_t)fnv(hash, number);
    }
    for (child = Json_At(value, 0); child; child = Json_Next(child)) hash = hash_json(hash * 31u, child, 1);
    return hash;
}
static uint32_t value_of(const JsonValue *value) { return hash_json(2166136261u, value, 0); }
/* The value of a number the reader takes when a key is left out, as
 * value_of would hash it written out. */
static uint32_t number_value(long number)
{
    char text[40];
    snprintf(text, sizeof(text), "%ld", number);
    return (uint32_t)fnv((2166136261u ^ (unsigned)JSON_NUMBER) * 16777619u, text);
}
/* A value nobody else's equals: two images of two packs never "agree". */
static uint32_t own_value(int mod) { return 0x80000000u | (uint32_t)mod; }

static void *grow(void *array, int *room, int count, size_t size)
{
    void *bigger;
    int next;
    if (count < *room) return array;
    next = *room ? *room * 2 : 64;
    bigger = realloc(array, (size_t)next * size);
    if (!bigger) return NULL;
    *room = next;
    return bigger;
}

static int add_string(ModsOverlaps *x, const char *text)
{
    size_t length = strlen(text) + 1;
    int at;
    if (x->string_length + length > x->string_room) {
        size_t room = x->string_room ? x->string_room * 2 : 4096;
        char *bigger;
        while (room < x->string_length + length) room *= 2;
        if (!(bigger = realloc(x->strings, room))) {
            x->failed = 1;
            return -1;
        }
        x->strings = bigger;
        x->string_room = room;
    }
    at = (int)x->string_length;
    memcpy(x->strings + at, text, length);
    x->string_length += length;
    return at;
}

static Claim *claim(ModsOverlaps *x, int kind, int mod, uint64_t key, int mode, uint32_t value, const JsonValue *src)
{
    Claim *c;
    Claim *claims = grow(x->claims, &x->claim_room, x->claim_count, sizeof(*x->claims));
    if (!claims) {
        x->failed = 1;
        return NULL;
    }
    x->claims = claims;
    c = &claims[x->claim_count++];
    memset(c, 0, sizeof(*c));
    c->key = key;
    c->value = value;
    c->mod = mod;
    c->seq = ++x->seq;
    c->label = c->via = -1;
    c->kind = (unsigned char)kind;
    c->mode = (unsigned char)mode;
    c->src = src;
    return c;
}
static void labelled(ModsOverlaps *x, Claim *c, const char *label)
{
    if (c) c->label = add_string(x, label);
}
/* A claim made wide: beside the keys (k & mask) == match (or the second
 * pair's), naming itself `via` in the line ("all", "stats"...). */
static void widened(ModsOverlaps *x, Claim *c, uint64_t mask, uint64_t match, uint64_t mask2, uint64_t match2,
                    const char *via)
{
    if (!c) return;
    c->mask = mask;
    c->match = match;
    c->mask2 = mask2;
    c->match2 = match2;
    if (via) c->via = add_string(x, via);
}

static const JsonValue *member(const ModsOverlaps *x, int mod, const char *key)
{
    return Json_Member(x->mods[mod].manifest, key);
}
/* How many mods have `key` in their manifest. */
static int having(const ModsOverlaps *x, const char *key)
{
    int n = 0;
    for (int i = 0; i < x->mod_count; i++) n += member(x, i, key) != NULL;
    return n;
}

/* A file of the mod's, parsed and kept for the claims that point into it. */
static const JsonValue *mod_file(ModsOverlaps *x, int mod, const char *relative)
{
    char path[1024], error[256];
    JsonDocument *document, **documents;
    if (!x->mods[mod].directory || !relative || !Paths_Contained(relative) ||
        snprintf(path, sizeof(path), "%s/%s", x->mods[mod].directory, relative) >= (int)sizeof(path))
        return NULL;
    if (!(document = Json_ParseFile(path, error, sizeof(error)))) return NULL;
    documents = grow(x->documents, &x->document_room, x->document_count, sizeof(*x->documents));
    if (!documents) {
        Json_Free(document);
        x->failed = 1;
        return NULL;
    }
    x->documents = documents;
    documents[x->document_count++] = document;
    return Json_Root(document);
}

/* The ".json" files of a folder of the mod's, sorted (strcmp) as the readers
 * sort them; their names without ".json", one after another, ended by "". */
static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static char *folder_names(const ModsOverlaps *x, int mod, const char *folder)
{
    char path[1024], *names = NULL, **list = NULL;
    size_t length = 0, total = 0;
    int count = 0, room = 0;
    DIR *dir;
    struct dirent *item;
    if (!x->mods[mod].directory ||
        snprintf(path, sizeof(path), "%s/%s", x->mods[mod].directory, folder) >= (int)sizeof(path) ||
        !(dir = opendir(path)))
        return NULL;
    while ((item = readdir(dir)) != NULL) {
        size_t n = strlen(item->d_name);
        char **bigger, *name;
        if (n < 6 || strcmp(item->d_name + n - 5, ".json")) continue;
        if (!(bigger = grow(list, &room, count, sizeof(*list)))) break;
        list = bigger;
        if (!(name = malloc(n - 4))) break;
        memcpy(name, item->d_name, n - 5);
        name[n - 5] = 0;
        list[count++] = name;
        total += n - 4;
    }
    closedir(dir);
    if (count) qsort(list, (size_t)count, sizeof(*list), by_name);
    if ((names = malloc(total + 1)) != NULL) {
        for (int i = 0; i < count; i++) {
            size_t n = strlen(list[i]) + 1;
            memcpy(names + length, list[i], n);
            length += n;
        }
        names[length] = 0;
    }
    for (int i = 0; i < count; i++) free(list[i]);
    free(list);
    return names;
}

/* --- cards and duelists by name ---------------------------------------- */

static int memo_find(ModsOverlaps *x, uint64_t hash, int *id)
{
    if (!x->memo_room) return 0;
    for (int i = (int)(hash % (uint64_t)x->memo_room);; i = (i + 1) % x->memo_room) {
        if (!x->memo[i].hash) return 0;
        if (x->memo[i].hash == hash) {
            *id = x->memo[i].id;
            return 1;
        }
    }
}
static void memo_keep(ModsOverlaps *x, uint64_t hash, int id)
{
    if ((x->memo_count + 1) * 2 > x->memo_room) {
        Memo *old = x->memo;
        int old_room = x->memo_room, room = old_room ? old_room * 2 : 1024;
        Memo *bigger = calloc((size_t)room, sizeof(*bigger));
        if (!bigger) return;
        x->memo = bigger;
        x->memo_room = room;
        x->memo_count = 0;
        for (int i = 0; i < old_room; i++)
            if (old[i].hash) memo_keep(x, old[i].hash, old[i].id);
        free(old);
    }
    for (int i = (int)(hash % (uint64_t)x->memo_room);; i = (i + 1) % x->memo_room)
        if (!x->memo[i].hash) {
            x->memo[i].hash = hash;
            x->memo[i].id = id;
            x->memo_count++;
            return;
        }
}

/* A card by what a manifest writes: its id when the game knows it, else
 * its letters' hash (two mods spelling it alike still meet). 0 for none. */
static uint64_t card_text(ModsOverlaps *x, const char *text)
{
    uint64_t hash = hash_letters(text);
    int id = 0;
    if (!hash) return 0;
    if (!memo_find(x, hash, &id)) {
        if (x->source.card) id = x->source.card(text, 0, x->source.context);
        else if (digits_only(text)) id = atoi(text);
        memo_keep(x, hash, id);
    }
    return id > 0 ? (uint64_t)id : TEXT_KEY | (hash >> 24);
}
static uint64_t card_key(ModsOverlaps *x, const JsonValue *value)
{
    if (Json_TypeOf(value) == JSON_NUMBER) {
        long number = Json_Number(value, 0);
        int id = x->source.card ? x->source.card(NULL, number, x->source.context) : (int)number;
        return id > 0 ? (uint64_t)id : number > 0 && number < (long)TEXT_KEY ? (uint64_t)number : 0;
    }
    return Json_TypeOf(value) == JSON_STRING ? card_text(x, Json_String(value, "")) : 0;
}
/* A card's base, type and attribute, when the game says (0 and -1 when it
 * does not). */
static void card_info(const ModsOverlaps *x, uint64_t key, int *base, int *type, int *attribute)
{
    *base = key < TEXT_KEY ? (int)key : 0;
    *type = *attribute = -1;
    if (key && key < TEXT_KEY && x->source.card_info &&
        !x->source.card_info((int)key, base, type, attribute, x->source.context)) {
        *base = (int)key;
        *type = *attribute = -1;
    }
}
/* A card for a line: its name when the game has one, else as written. */
static void card_words(const ModsOverlaps *x, const JsonValue *value, char *out, size_t size)
{
    long number = Json_TypeOf(value) == JSON_NUMBER ? Json_Number(value, 0) : 0;
    const char *text = Json_String(value, NULL);
    int id = 0;
    char name[96];
    if (x->source.card) id = x->source.card(text, number, x->source.context);
    else if (number > 0) id = (int)number;
    if (id > 0 && x->source.card_name && x->source.card_name(id, name, sizeof(name), x->source.context))
        snprintf(out, size, "'%s'", name);
    else if (text)
        snprintf(out, size, "'%s'", text);
    else
        snprintf(out, size, "#%ld", number);
}

static uint64_t duelist_key(ModsOverlaps *x, const char *name)
{
    int id;
    if (!name) name = "";
    if (same_letters(name, "all")) return ALL_DUELISTS;
    id = x->source.duelist ? x->source.duelist(name, x->source.context) : -1;
    if (id < 0 && digits_only(name)) id = atoi(name);
    return id >= 0 ? (uint64_t)(id & 0xFFFFFF) : (1ull << 46) | (hash_letters(name) >> 18);
}

/* Whether an entry naming a mod's setting ("setting", and "value" for one
 * choice of it) is used, as Mods_File and the rule tables decide (mods.c
 * entry_used): a text file, a fusion, equip or ritual entry. A setting the
 * mod does not declare, or a "value" that is not a number, leaves it used. */
static int switched_on(const ModsOverlaps *x, int mod, const JsonValue *entry)
{
    const char *key = Json_String(Json_Member(entry, "setting"), NULL);
    const JsonValue *only = Json_Member(entry, "value");
    int value;
    if (!key || !*key || !x->source.setting || (value = x->source.setting(mod, key, x->source.context)) < 0) return 1;
    if (only && Json_TypeOf(only) != JSON_NUMBER) return 1;
    return only ? value == (int)Json_Number(only, 0) : value != 0;
}

/* --- the kinds ------------------------------------------------------------ */

/* "data": a disc file by name, as the disc's lookup takes it (libds.c
 * search_file: leading backslashes and the ";1" aside, letter for letter),
 * or raw sectors, which meet when their runs of sectors do. Replacements
 * go first, then every patch over them, the later of each winning. */
static long patch_length(const JsonValue *patch)
{
    long digits = 0;
    for (const char *s = Json_String(Json_Member(patch, "bytes"), ""); *s; s++) digits += isxdigit((unsigned char)*s) != 0;
    return digits / 2;
}
static void read_data(ModsOverlaps *x, int mod)
{
    for (const JsonValue *entry = Json_At(list_of(member(x, mod, "data")), 0); entry; entry = Json_Next(entry)) {
        const char *file = Json_String(Json_Member(entry, "file"), NULL);
        long lba = Json_Number(Json_Member(entry, "lba"), -1);
        int replace = Json_Member(entry, "replace") != NULL;
        Claim *c;
        if (file && *file) {
            char name[256];
            size_t n = 0;
            while (*file == '\\') file++;
            while (file[n] && file[n] != ';' && n + 1 < sizeof(name)) {
                name[n] = file[n];
                n++;
            }
            name[n] = 0;
            claim(x, MODS_OVERLAP_DATA, mod, (2ull << 62) | (hash_text(name) >> 2), replace ? SET : ADD,
                  own_value(mod), entry);
        } else if (lba >= 0) {
            /* The sectors it reaches: a replacement's run, a patch's bytes. */
            /* In long long: a long is 32 bits in the game, and a manifest's
             * numbers may be as large as one holds. */
            long long sectors = Json_Number(Json_Member(entry, "sectors"), 1);
            long long lo = -1, hi = -1;
            if (replace) {
                lo = lba;
                hi = (long long)lba + (sectors > 0 ? sectors : 1) - 1;
            }
            for (const JsonValue *p = Json_At(list_of(Json_Member(entry, "patch")), 0); p; p = Json_Next(p)) {
                long long at = Json_Number(Json_Member(p, "at"), 0), length = patch_length(p);
                long long first, last;
                if (at < 0) continue;
                first = lba + at / 2048;
                last = lba + (at + (length ? length : 1) - 1) / 2048;
                if (lo < 0 || first < lo) lo = first;
                if (last > hi) hi = last;
            }
            if (lo < 0) lo = hi = lba;
            if ((c = claim(x, MODS_OVERLAP_DATA, mod, RAW_SECTORS, replace ? SET : ADD, own_value(mod), entry))) {
                c->lo = lo;
                c->hi = hi;
            }
        }
    }
}
/* Raw sectors: each run of entries whose sectors meet is one key, named
 * for the sectors it spans. */
static int by_sector(const void *left, const void *right)
{
    const Claim *a = *(const Claim *const *)left, *b = *(const Claim *const *)right;
    if (a->lo != b->lo) return a->lo < b->lo ? -1 : 1;
    return (a->seq > b->seq) - (a->seq < b->seq);
}
static void sector_runs(ModsOverlaps *x)
{
    Claim **raw = NULL;
    int count = 0, room = 0;
    for (int i = 0; i < x->claim_count; i++) {
        Claim **bigger;
        if (x->claims[i].kind != MODS_OVERLAP_DATA || x->claims[i].key != RAW_SECTORS) continue;
        if (!(bigger = grow(raw, &room, count, sizeof(*raw)))) {
            x->failed = 1;
            free(raw);
            return;
        }
        raw = bigger;
        raw[count++] = &x->claims[i];
    }
    if (count) qsort(raw, (size_t)count, sizeof(*raw), by_sector);
    for (int start = 0; start < count;) {
        int end = start + 1;
        long long hi = raw[start]->hi;
        char label[80];
        int at;
        while (end < count && raw[end]->lo <= hi) {
            if (raw[end]->hi > hi) hi = raw[end]->hi;
            end++;
        }
        if (raw[start]->lo == hi) snprintf(label, sizeof(label), "Sector %lld", hi);
        else snprintf(label, sizeof(label), "Sectors %lld-%lld", raw[start]->lo, hi);
        at = add_string(x, label);
        for (int i = start; i < end; i++) {
            raw[i]->key = RAW_SECTORS | (uint64_t)raw[start]->lo;
            raw[i]->label = at;
        }
        start = end;
    }
    free(raw);
}

/* "audio": an id of music, xa or sfx, as AudioReplace_ParseId reads it; the
 * mod applied later is heard. A file is the mod's own, whatever its name. */
static long audio_id(const char *text)
{
    char *end;
    long value;
    if (!text || !*text) return -1;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        if (!text[2]) return -1;
        value = strtol(text + 2, &end, 16);
    } else
        value = strtol(text, &end, 10);
    if (*end || value < 0 || value > 0xFFFF || text[0] == '-' || text[0] == '+' || text[0] == ' ') return -1;
    return value;
}
static void read_audio(ModsOverlaps *x, int mod)
{
    static const char *const kinds[] = {"music", "xa", "sfx"};
    for (int k = 0; k < 3; k++)
        for (const JsonValue *m = Json_At(object_of(Json_Member(object_of(member(x, mod, "audio")), kinds[k])), 0); m;
             m = Json_Next(m)) {
            long id = audio_id(name_of(m));
            if (id >= 0) claim(x, MODS_OVERLAP_AUDIO, mod, ((uint64_t)k << 32) | (uint64_t)id, SET, own_value(mod), m);
        }
}

/* "textures": the pack's manifest.json, an image per entry; the same image
 * is the same reading of the same words (texture_pack.c compare: archive,
 * offset, size, stride, depth and palette). An entry the loader leaves out
 * (no file or archive, measures out of range, its part switched off) is not
 * counted; a pack's "setting" is on while not 0, its "value" unread. */
static void read_textures(ModsOverlaps *x, int mod)
{
    const char *folder = Json_String(member(x, mod, "textures"), NULL);
    char relative[512], key[512];
    const JsonValue *list;
    if (!folder || snprintf(relative, sizeof(relative), "%s/manifest.json", folder) >= (int)sizeof(relative)) return;
    list = mod_file(x, mod, relative);
    for (const JsonValue *entry = Json_At(list_of(list), 0); entry; entry = Json_Next(entry)) {
        const char *archive = Json_String(Json_Member(entry, "archive"), NULL);
        const char *setting = Json_String(Json_Member(entry, "setting"), NULL);
        long offset = Json_Number(Json_Member(entry, "offset"), -1), words = Json_Number(Json_Member(entry, "words"), 0),
             rows = Json_Number(Json_Member(entry, "rows"), 0), bpp = Json_Number(Json_Member(entry, "bpp"), 0),
             stride = Json_Number(Json_Member(entry, "stride"), words),
             clut = Json_Number(Json_Member(entry, "clut_entries"), 0);
        if (!archive || !Json_String(Json_Member(entry, "file"), NULL) ||
            !Paths_Contained(Json_String(Json_Member(entry, "file"), "")) || offset < 0 || words < 1 || words > 1024 ||
            rows < 1 || rows > 512 || (bpp != 4 && bpp != 8 && bpp != 16) || stride < 1)
            continue;
        if (Json_Member(entry, "setting") && setting && *setting && x->source.setting &&
            x->source.setting(mod, setting, x->source.context) == 0)
            continue;
        /* Read row by row, it has no stride (texture_pack.c: 0 with
         * row_offsets, a list of one offset a row). */
        if (Json_Count(Json_Member(entry, "row_offsets")) == rows) stride = 0;
        snprintf(key, sizeof(key), "%s|%ld|%ld|%ld|%ld|%ld|%ld", archive, offset, words, rows, stride, bpp,
                 clut ? Json_Number(Json_Member(entry, "clut_offset"), 0) : 0);
        claim(x, MODS_OVERLAP_TEXTURES, mod, hash_text(key), SET, own_value(mod), entry);
    }
}

/* "cards": a "replace" changes one of the disc's cards in place (cards.c
 * add_entry: a name, or a number, of the disc's 722; nothing else), the
 * mods read in load order (mods.c Mods_VisitCards). Its stats, stars,
 * frame, model and effect go over the earlier entries'; its name, text,
 * password, art, plate ("title", which goes with the name), field art and
 * fusion groups are reset by every later replace, whether it gives them or
 * not; notes add up. An entry of notes alone only adds them. */
static const char *const card_reset_keys[] = {"name",      "description", "password",  "art",
                                              "thumbnail", "title",       "field_art", "fusion_groups"};
static int card_resets(const char *key)
{
    for (size_t i = 0; i < sizeof(card_reset_keys) / sizeof(card_reset_keys[0]); i++)
        if (!strcmp(key, card_reset_keys[i])) return 1;
    return 0;
}
static int notes_only(const JsonValue *entry)
{
    for (const JsonValue *m = Json_At(entry, 0); m; m = Json_Next(m))
        if (strcmp(name_of(m), "replace") && strcmp(name_of(m), "notes") && strcmp(name_of(m), "id")) return 0;
    return Json_Member(entry, "notes") != NULL;
}
static void read_cards(ModsOverlaps *x, int mod)
{
    for (const JsonValue *e = Json_At(list_of(member(x, mod, "cards")), 0); e; e = Json_Next(e)) {
        const JsonValue *replaced = Json_Member(e, "replace");
        const char *text = Json_String(replaced, NULL);
        uint64_t key;
        if (!replaced || Json_TypeOf(e) != JSON_OBJECT || notes_only(e)) continue;
        if (text && !isdigit((unsigned char)*text)) {
            if (strchr(text, ':')) continue; /* an added card's identity: not the disc's */
            key = card_text(x, text);
        } else {
            long number = Json_Number(replaced, 0);
            key = number >= 1 && number <= CARD_COUNT ? (uint64_t)number : 0;
        }
        if (key && (key >= TEXT_KEY || key <= CARD_COUNT)) claim(x, MODS_OVERLAP_CARDS, mod, key, SET, value_of(e), e);
    }
}

/* "fusions": a pair in either order, the later mod's rule winning;
 * "remove"s add up (a mod's own recipes still make the card). */
static void read_fusions(ModsOverlaps *x, int mod)
{
    for (const JsonValue *rule = Json_At(list_of(member(x, mod, "fusions")), 0); rule; rule = Json_Next(rule)) {
        const JsonValue *with = Json_Member(rule, "with"), *removed = Json_Member(rule, "remove");
        if (!switched_on(x, mod, rule)) continue;
        if (removed) {
            uint64_t key = card_key(x, removed);
            if (key) claim(x, MODS_OVERLAP_FUSIONS, mod, (1ull << 63) | key, ADD, 0, rule);
        } else if (Json_Count(with) == 2) {
            uint64_t a = card_key(x, Json_At(with, 0)), b = card_key(x, Json_Next(Json_At(with, 0)));
            const JsonValue *result = Json_Member(rule, "result");
            if (!a || !b || !result) continue;
            claim(x, MODS_OVERLAP_FUSIONS, mod, (mix(a < b ? a : b, a < b ? b : a) >> 1), SET,
                  (uint32_t)card_key(x, result) ^ (uint32_t)(card_key(x, result) >> 32), rule);
        }
    }
}

/* "equips": an entry per equip card. For a monster, the latest entry that
 * says anything about it decides (tables.c Tables_Equip, Tables_EquipBonus):
 * the decisions are worked out per monster below. "equip_bonus_default":
 * the latest mod's. */
static void read_equips(ModsOverlaps *x, int mod)
{
    const JsonValue *bonus = member(x, mod, "equip_bonus_default");
    for (const JsonValue *e = Json_At(list_of(member(x, mod, "equips")), 0); e; e = Json_Next(e)) {
        uint64_t key = card_key(x, Json_Member(e, "card"));
        int base, type, attribute;
        Claim *c;
        if (!key || Json_TypeOf(e) != JSON_OBJECT || !switched_on(x, mod, e)) continue;
        /* A rule for an equip card is one for its copies too (Tables_Equip
         * matches the card or its base): keyed by base and card, a card's
         * own rule stands beside its copies' rules in other mods. */
        if (key < EQUIP_CARDS) {
            card_info(x, key, &base, &type, &attribute);
            if (base <= 0 || (uint64_t)base >= EQUIP_CARDS) base = (int)key;
            key |= (uint64_t)base << 20;
        }
        c = claim(x, MODS_OVERLAP_EQUIPS, mod, key, SET, value_of(e), e);
        if (c && key < EQUIP_CARDS << 20 && (key >> 20) == (key & (EQUIP_CARDS - 1)))
            widened(x, c, ~(EQUIP_CARDS - 1), key & ~(EQUIP_CARDS - 1), 0, 0, NULL);
    }
    if (bonus) claim(x, MODS_OVERLAP_EQUIPS, mod, DEFAULT_EQUIP_BONUS, SET, value_of(bonus), bonus);
}

/* "rituals": the latest recipe of a ritual card is the one used. */
static void read_rituals(ModsOverlaps *x, int mod)
{
    for (const JsonValue *e = Json_At(list_of(member(x, mod, "rituals")), 0); e; e = Json_Next(e)) {
        uint64_t key = card_key(x, Json_Member(e, "card"));
        if (key && switched_on(x, mod, e))
            claim(x, MODS_OVERLAP_RITUALS, mod, key, SET,
                  hash_json(value_of(Json_Member(e, "tributes")), Json_Member(e, "result"), 0), e);
    }
}

/* The duelists the mods define: their duelists/ folders (a file a duelist,
 * named by its id) and their "duelists" lists (or the file it names), read
 * before the pools that may name them. A replacement takes over one of the
 * disc's 39; anything else is a duelist of its own (duelists.c
 * read_one_duelist). */
static void define_duelist(ModsOverlaps *x, int mod, const char *id, uint64_t replaces, const char *who)
{
    Defined *defined;
    if (!id || !*id) return; /* before grow(), which may move the array */
    defined = grow(x->defined, &x->defined_room, x->defined_count, sizeof(*x->defined));
    if (!defined) return;
    x->defined = defined;
    snprintf(defined[x->defined_count].id, sizeof(defined[0].id), "%s", id);
    snprintf(defined[x->defined_count].who, sizeof(defined[0].who), "%s", who ? who : id);
    defined[x->defined_count].mod = mod;
    defined[x->defined_count++].replaces = replaces;
}
/* One duelist entry: the later of two mods replacing one duelist has him; of
 * two asking for one Free Duel slot (40-127, not on a replacement, with a
 * "copy"), the earlier keeps it. */
static void duelist_entry(ModsOverlaps *x, int mod, const char *id, const JsonValue *entry)
{
    const JsonValue *over = Json_Member(entry, "replace"), *slot = Json_Member(entry, "slot");
    const char *copy = Json_String(Json_Member(entry, "copy"), NULL);
    char label[200];
    uint64_t target = 0;
    if (Json_TypeOf(entry) != JSON_OBJECT) return;
    if (over) {
        const char *who = Json_String(over, NULL);
        int number = digits_only(who) ? atoi(who) : -1;
        if (!who) return;
        target = duelist_key(x, who);
        /* One of the disc's 39 (Duelists_Named), when the game can say. */
        if (target == ALL_DUELISTS || number == 0 || number >= 40 || (target < (1ull << 46) && (target == 0 || target >= 40)) ||
            (x->source.duelist && target >= (1ull << 46)))
            return;
        snprintf(label, sizeof(label), "Duelist '%.80s' replaced", who);
        labelled(x, claim(x, MODS_OVERLAP_DUELISTS, mod, (1ull << 62) | target, SET, own_value(mod), entry), label);
    } else if (!copy || !*copy)
        return;
    define_duelist(x, mod, id, target, over ? Json_String(over, NULL) : NULL);
    if (slot && !over && Json_TypeOf(slot) == JSON_NUMBER && Json_Number(slot, -1) >= 40 &&
        Json_Number(slot, -1) < 128) {
        snprintf(label, sizeof(label), "Free Duel slot %ld", Json_Number(slot, 0));
        labelled(x,
                 claim(x, MODS_OVERLAP_DUELISTS, mod, (3ull << 62) | (uint64_t)Json_Number(slot, 0), FIRST,
                       own_value(mod), entry),
                 label);
    }
}
static void read_duelists(ModsOverlaps *x, int mod)
{
    const JsonValue *list = member(x, mod, "duelists");
    char *names = folder_names(x, mod, "duelists"), relative[160];
    for (const char *name = names; name && *name; name += strlen(name) + 1) {
        snprintf(relative, sizeof(relative), "duelists/%.100s.json", name);
        duelist_entry(x, mod, name, mod_file(x, mod, relative));
    }
    free(names);
    if (Json_TypeOf(list) == JSON_STRING) list = mod_file(x, mod, Json_String(list, NULL));
    for (const JsonValue *e = Json_At(list_of(list), 0); e; e = Json_Next(e))
        duelist_entry(x, mod, Json_String(Json_Member(e, "id"), NULL), e);
}

/* "drops" and "decks": an opponent's pools. Plain edits add up, each on the
 * pool as the mods before left it; "replace": true empties it first; a fixed
 * deck (exactly 40 cards) wins over every weighted edit (tables.c). "all"
 * reaches every opponent. The pools may be in a file, or a file per opponent
 * in the mod's drops/ and decks/ folders, named by an opponent or by a
 * duelist of the mod's own. */
static int pool_named(const char *name)
{
    static const char *const names[][2] = {{"pow", "sa-pow"}, {"bcd", "b-c-d"}, {"tec", "sa-tec"}};
    for (int i = 0; i < 3; i++)
        if (same_letters(name, names[i][0]) || same_letters(name, names[i][1])) return i + 1;
    return -1;
}
static const char *const pool_words[] = {"deck", "POW drops", "B/C/D drops", "TEC drops"};
/* An opponent the game has, by what duelist_key made of a name: NO_DUELIST
 * when the game can say there is none (the reader leaves its pools out). */
#define NO_DUELIST (~0ull)
static uint64_t known_duelist(const ModsOverlaps *x, uint64_t who)
{
    return x->source.duelist && who >= (1ull << 46) && who != ALL_DUELISTS ? NO_DUELIST : who;
}
static void pool_claim(ModsOverlaps *x, int mod, const char *duelist, uint64_t who, int pool, const JsonValue *entry)
{
    const JsonValue *fixed_value = pool ? NULL : Json_Member(entry, "fixed");
    int fixed = Json_Bool(fixed_value, 0), mode;
    char label[160];
    Claim *c;
    /* A pool is an object of cards (tables.c read_pool, read_fixed_deck). */
    if (Json_TypeOf(entry) != JSON_OBJECT) return;
    if (fixed_value && Json_TypeOf(fixed_value) != JSON_BOOL && Json_TypeOf(fixed_value) != JSON_NUMBER) return;
    if (fixed) { /* the forty cards, counted out; any other count is left out */
        long total = 0;
        for (const JsonValue *m = Json_At(entry, 0); m; m = Json_Next(m))
            if (strcmp(name_of(m), "fixed") && Json_TypeOf(m) == JSON_NUMBER) total += Json_Number(m, 0);
        if (total != 40) return;
    }
    mode = fixed ? FIXED : Json_Bool(Json_Member(entry, "replace"), 0) ? SET : ADD;
    if (!(c = claim(x, MODS_OVERLAP_POOLS, mod, (who << 8) | (uint64_t)pool, mode, value_of(entry), entry))) return;
    if (c->mode == SET) c->flags |= RESETS;
    if (who == ALL_DUELISTS) {
        widened(x, c, 0xFF, (uint64_t)pool, 0, 0, "all");
        snprintf(label, sizeof(label), "Every opponent's %s", pool_words[pool]);
    } else
        snprintf(label, sizeof(label), "%.80s's %s", duelist, pool_words[pool]);
    labelled(x, c, label);
}
static void pool_table(ModsOverlaps *x, int mod, const JsonValue *table, int decks)
{
    for (const JsonValue *entry = Json_At(object_of(table), 0); entry; entry = Json_Next(entry)) {
        uint64_t who = known_duelist(x, duelist_key(x, name_of(entry)));
        if (who == NO_DUELIST) continue;
        if (decks) {
            pool_claim(x, mod, name_of(entry), who, 0, entry);
            continue;
        }
        for (const JsonValue *pool = Json_At(object_of(entry), 0); pool; pool = Json_Next(pool)) {
            int which = pool_named(name_of(pool));
            if (which > 0) pool_claim(x, mod, name_of(entry), who, which, pool);
        }
    }
}
/* A pool file's opponent: the mod's own duelist of that id (or whom it
 * replaces, and then named as the mod named him), else an opponent by name
 * (tables.c read_pool_folder). */
static uint64_t pool_file_duelist(ModsOverlaps *x, int mod, const char *name, const char **label)
{
    *label = name;
    for (int i = 0; i < x->defined_count; i++)
        if (x->defined[i].mod == mod && !strcmp(x->defined[i].id, name)) {
            char identity[192];
            if (x->defined[i].replaces) {
                *label = x->defined[i].who;
                return x->defined[i].replaces;
            }
            snprintf(identity, sizeof(identity), "%s:%s", x->mods[mod].id, name);
            return (1ull << 45) | (hash_text(identity) >> 20);
        }
    return known_duelist(x, duelist_key(x, name));
}
static void read_pools(ModsOverlaps *x, int mod)
{
    for (int decks = 0; decks < 2; decks++) {
        const JsonValue *table = member(x, mod, decks ? "decks" : "drops");
        char *names, relative[160];
        if (Json_TypeOf(table) == JSON_STRING) table = mod_file(x, mod, Json_String(table, NULL));
        pool_table(x, mod, table, decks);
        names = folder_names(x, mod, decks ? "decks" : "drops");
        for (const char *name = names; name && *name; name += strlen(name) + 1) {
            const JsonValue *root;
            const char *label;
            uint64_t who = pool_file_duelist(x, mod, name, &label);
            if (who == NO_DUELIST) continue;
            snprintf(relative, sizeof(relative), "%s/%.100s.json", decks ? "decks" : "drops", name);
            if (!(root = mod_file(x, mod, relative))) continue;
            if (decks)
                pool_claim(x, mod, label, who, 0, root);
            else
                for (const JsonValue *pool = Json_At(object_of(root), 0); pool; pool = Json_Next(pool))
                    if (pool_named(name_of(pool)) > 0) pool_claim(x, mod, label, who, pool_named(name_of(pool)), pool);
        }
        free(names);
    }
}

/* "passwords": a card's password and its price; "all" every card, before
 * the cards named beside it. The latest mod that sets one wins; a price in
 * starchips and one in percent are not the same price. Added cards can be
 * sold by password too. */
static void read_passwords(ModsOverlaps *x, int mod)
{
    static const char *const fields[][2] = {{"password", NULL}, {"starchips", "starchips_percent"}};
    for (const JsonValue *m = Json_At(object_of(member(x, mod, "passwords")), 0); m; m = Json_Next(m)) {
        int all = same_letters(name_of(m), "all");
        uint64_t card = all ? ALL_CARDS : card_text(x, name_of(m));
        if (!card || Json_TypeOf(m) != JSON_OBJECT) continue;
        for (int f = 0; f < 2; f++) {
            const JsonValue *v = Json_Member(m, fields[f][0]);
            char label[160];
            Claim *c;
            if (!v && fields[f][1]) v = Json_Member(m, fields[f][1]);
            if (!v) continue;
            c = claim(x, MODS_OVERLAP_PASSWORDS, mod, (card << 4) | (uint64_t)(f + 1), SET,
                      hash_json(2166136261u, v, 1), m);
            if (!c) return;
            if (all) {
                widened(x, c, 0xF, (uint64_t)(f + 1), 0, 0, "all");
                snprintf(label, sizeof(label), "Every card's %s", f ? "price" : "password");
            } else {
                char card_name[120];
                snprintf(label, sizeof(label), "%s of '%.80s'", f ? "Price" : "Password", name_of(m));
                if (x->source.card_name && card < TEXT_KEY &&
                    x->source.card_name((int)card, card_name, sizeof(card_name), x->source.context))
                    snprintf(label, sizeof(label), "%s of '%.100s'", f ? "Price" : "Password", card_name);
            }
            labelled(x, c, label);
        }
    }
}

/* "guardian_stars": an ordered pair of stars, a star's name, icon and
 * palette, the way a summon chooses; the later mod's. "replace" sets every
 * pair to 0 first, and the first mod to declare a star 11-15 sets its pairs
 * to 0 (stars.c), over what earlier mods gave them. */
static const char *const star_retail[] = {"",        "Mars",    "Jupiter", "Saturn", "Uranus", "Pluto",
                                          "Neptune", "Mercury", "Sun",     "Moon",   "Venus"};
static void star_learn(ModsOverlaps *x, uint64_t name, int id)
{
    StarName *names;
    if (!name) return;
    names = grow(x->star_names, &x->star_name_room, x->star_name_count, sizeof(*x->star_names));
    if (!names) return;
    x->star_names = names;
    names[x->star_name_count].name = name;
    names[x->star_name_count++].id = id;
}
/* A mod's "stars", declared before its own matchups (stars.c: all of
 * them first): a star's name replaces the names it had, so a matchup knows
 * the names of the stars the mods read so far gave them, and no others. */
static void star_declare(ModsOverlaps *x, const JsonValue *section)
{
    for (const JsonValue *s = Json_At(list_of(Json_Member(section, "stars")), 0); s; s = Json_Next(s)) {
        const JsonValue *name = Json_Member(s, "name");
        int id = (int)Json_Number(Json_Member(s, "id"), -1);
        if (Json_TypeOf(s) != JSON_OBJECT || !Json_Member(s, "id") || id < 1 || id > 15 ||
            (Json_TypeOf(name) != JSON_STRING && Json_TypeOf(name) != JSON_OBJECT))
            continue;
        for (int i = 0; i < x->star_name_count; i++)
            if (x->star_names[i].id == id) x->star_names[i].id = 0;
        if (Json_TypeOf(name) == JSON_STRING) star_learn(x, hash_letters(Json_String(name, "")), id);
        for (const JsonValue *n = Json_TypeOf(name) == JSON_OBJECT ? Json_At(name, 0) : NULL; n; n = Json_Next(n))
            if (Json_TypeOf(n) == JSON_STRING) star_learn(x, hash_letters(Json_String(n, "")), id);
    }
}
static int star_of(const ModsOverlaps *x, const JsonValue *value)
{
    const char *text = Json_String(value, NULL);
    uint64_t name;
    if (Json_TypeOf(value) == JSON_NUMBER) return (int)Json_Number(value, -1);
    if (!text) return -1;
    if (digits_only(text)) return atoi(text);
    name = hash_letters(text);
    /* The disc's ten by their names first, then 1-15 by theirs now (Stars_Find). */
    for (int id = 1; id <= 10; id++)
        if (hash_letters(star_retail[id]) == name) return id;
    for (int id = 1; id <= 15; id++)
        for (int i = 0; i < x->star_name_count; i++)
            if (x->star_names[i].id == id && x->star_names[i].name == name) return id;
    return -1;
}
static void star_pair(ModsOverlaps *x, int mod, int a, int d, long bonus, const JsonValue *src)
{
    if (a >= 0 && a <= 15 && d >= 0 && d <= 15)
        claim(x, MODS_OVERLAP_STARS, mod, SUB(1) | ((uint64_t)a << 8) | (uint64_t)d, SET, (uint32_t)bonus, src);
}
static void read_stars(ModsOverlaps *x, int mod)
{
    const JsonValue *section = object_of(member(x, mod, "guardian_stars")), *v;
    long bonus = 500;
    int reset = Json_Bool(Json_Member(section, "replace"), 0);
    if (!section) return;
    star_declare(x, section);
    if (reset) {
        Claim *c = claim(x, MODS_OVERLAP_STARS, mod, SUB(RESET_SUB), SET, 0, Json_Member(section, "replace"));
        widened(x, c, SUB_MASK, SUB(1), 0, 0, "replace");
        if (c) c->flags |= RESETS;
    }
    if ((v = Json_Member(section, "default_bonus")) != NULL && Json_TypeOf(v) == JSON_NUMBER) {
        bonus = Json_Number(v, 500);
        /* What the disc's two cycles give, unless "replace" took them; the
         * section's own beats and matchups take it too. */
        for (int a = 1; a <= 10 && !reset; a++) {
            int d = a <= 6 ? a % 6 + 1 : (a - 7 + 1) % 4 + 7;
            star_pair(x, mod, a, d, bonus, v);
            star_pair(x, mod, d, a, -bonus, v);
        }
    }
    if ((v = Json_Member(section, "choice")) != NULL)
        claim(x, MODS_OVERLAP_STARS, mod, SUB(3) | 2, SET, (uint32_t)hash_letters(Json_String(v, "")), v);
    for (const JsonValue *s = Json_At(list_of(Json_Member(section, "stars")), 0); s; s = Json_Next(s)) {
        int id = (int)Json_Number(Json_Member(s, "id"), -1);
        if (Json_TypeOf(s) != JSON_OBJECT || !Json_Member(s, "id") || id < 1 || id > 15) continue;
        if (id > 10 && !x->star_declared[id]) {
            Claim *c = claim(x, MODS_OVERLAP_STARS, mod, SUB(0xFD) | (uint64_t)id, SET, 0, s);
            char label[80];
            widened(x, c, SUB_MASK | 0xFF00, SUB(1) | ((uint64_t)id << 8), SUB_MASK | 0xFF, SUB(1) | (uint64_t)id,
                    "stars");
            if (c) c->flags |= RESETS;
            snprintf(label, sizeof(label), "Star %d declared (every matchup of it at 0)", id);
            labelled(x, c, label);
        }
        x->star_declared[id] = 1;
    }
    for (const JsonValue *s = Json_At(list_of(Json_Member(section, "stars")), 0); s; s = Json_Next(s)) {
        static const char *const fields[] = {"name", "icon", "palette"};
        int id = (int)Json_Number(Json_Member(s, "id"), -1);
        if (Json_TypeOf(s) != JSON_OBJECT || !Json_Member(s, "id") || id < 1 || id > 15) continue;
        for (int f = 0; f < 3; f++)
            if ((v = Json_Member(s, fields[f])) != NULL)
                claim(x, MODS_OVERLAP_STARS, mod, SUB(2) | ((uint64_t)id << 8) | (uint64_t)(f + 1), SET,
                      f == 1 ? own_value(mod) : value_of(v), v);
        for (const JsonValue *t = Json_At(list_of(Json_Member(s, "beats")), 0); t; t = Json_Next(t)) {
            int other = star_of(x, t);
            star_pair(x, mod, id, other, bonus, t);
            star_pair(x, mod, other, id, -bonus, t);
        }
    }
    for (const JsonValue *m = Json_At(list_of(Json_Member(section, "matchups")), 0); m; m = Json_Next(m)) {
        int a = star_of(x, Json_Member(m, "attacker")), d = star_of(x, Json_Member(m, "defender"));
        long points = Json_Member(m, "bonus") ? Json_Number(Json_Member(m, "bonus"), bonus) : bonus;
        if (Json_TypeOf(m) != JSON_OBJECT) continue;
        star_pair(x, mod, a, d, points, m);
        if (Json_Bool(Json_Member(m, "mirror"), 0)) star_pair(x, mod, d, a, -points, m);
    }
}

/* "limits" (and "chest_overflow"): each key the latest mod's (tables.c
 * read_limits). "stats" sets both "attack" and "defense", and a life-point
 * start (a number, or "start") both "player" and "opponent"; a duelist's
 * own LP is a side or both, and two mods' different duelists add up.
 * "chest_overflow" sets the chest and the starchips both, 250 and 0 when it
 * leaves one out. */
static Claim *limit_claim(ModsOverlaps *x, int mod, const char *path, const char *key, uint32_t value,
                          const JsonValue *v)
{
    char label[200];
    Claim *c = claim(x, MODS_OVERLAP_LIMITS, mod, hash_text(key ? key : path), SET, value, v);
    snprintf(label, sizeof(label), "Limit %s", path);
    labelled(x, c, label);
    return c;
}
/* A key that stands for two: its own claim, and wide over the two. */
static void limit_both(ModsOverlaps *x, int mod, const char *path, const char *key, const char *one,
                       const char *two, const char *via, const JsonValue *v)
{
    Claim *c = limit_claim(x, mod, path, key, value_of(v), v);
    widened(x, c, ~0ull, hash_text(one), ~0ull, hash_text(two), via);
}
static void read_limits(ModsOverlaps *x, int mod)
{
    const JsonValue *limits = object_of(member(x, mod, "limits")), *overflow = object_of(member(x, mod, "chest_overflow"));
    for (const JsonValue *m = Json_At(limits, 0); m; m = Json_Next(m)) {
        const char *name = name_of(m);
        if (!strcmp(name, "stats"))
            limit_both(x, mod, "stats", "stats", "attack", "defense", "stats", m);
        else if (!strcmp(name, "life_points") && Json_TypeOf(m) != JSON_OBJECT)
            limit_both(x, mod, "life_points", "life_points.start", "life_points.player", "life_points.opponent",
                       "life_points", m);
        else if (!strcmp(name, "life_points")) {
            for (const JsonValue *k = Json_At(m, 0); k; k = Json_Next(k)) {
                char path[200];
                snprintf(path, sizeof(path), "life_points.%.60s", name_of(k));
                if (!strcmp(name_of(k), "start"))
                    limit_both(x, mod, path, path, "life_points.player", "life_points.opponent", "life_points.start", k);
                else if (!strcmp(name_of(k), "duelists")) {
                    for (const JsonValue *d = Json_At(object_of(k), 0); d; d = Json_Next(d)) {
                        /* tables.c read_life_points: "all" is its own (a named duelist wins
                         * over it whatever the order); an opponent the game lacks is left out. */
                        uint64_t who = known_duelist(x, duelist_key(x, name_of(d)));
                        char key[200], side[220], label[220];
                        if (who == NO_DUELIST) continue;
                        snprintf(key, sizeof(key), "life_points.duelists.#%llx", (unsigned long long)who);
                        snprintf(label, sizeof(label), "life_points.duelists.%.60s", name_of(d));
                        if (Json_TypeOf(d) == JSON_OBJECT) {
                            for (const JsonValue *s = Json_At(d, 0); s; s = Json_Next(s)) {
                                char path_side[240];
                                if (strcmp(name_of(s), "player") && strcmp(name_of(s), "opponent")) continue;
                                snprintf(side, sizeof(side), "%s.%s", key, name_of(s));
                                snprintf(path_side, sizeof(path_side), "%s.%s", label, name_of(s));
                                limit_claim(x, mod, path_side, side, value_of(s), s);
                            }
                        } else if (Json_TypeOf(d) == JSON_NUMBER) {
                            /* A number is the duelist's own LP: the opponent's side alone. */
                            snprintf(side, sizeof(side), "%s.opponent", key);
                            limit_claim(x, mod, label, side, value_of(d), d);
                        }
                    }
                } else if (Json_TypeOf(k) == JSON_OBJECT) {
                    for (const JsonValue *s = Json_At(k, 0); s; s = Json_Next(s)) {
                        char inner[280];
                        snprintf(inner, sizeof(inner), "%s.%.60s", path, name_of(s));
                        limit_claim(x, mod, inner, NULL, value_of(s), s);
                    }
                } else
                    limit_claim(x, mod, path, NULL, value_of(k), k);
            }
        } else if (Json_TypeOf(m) == JSON_OBJECT) {
            for (const JsonValue *k = Json_At(m, 0); k; k = Json_Next(k)) {
                char path[200];
                snprintf(path, sizeof(path), "%.60s.%.60s", name, name_of(k));
                limit_claim(x, mod, path, NULL, value_of(k), k);
            }
        } else
            limit_claim(x, mod, name, NULL, value_of(m), m);
    }
    if (overflow) {
        const JsonValue *limit = Json_Member(overflow, "limit"), *starchips = Json_Member(overflow, "starchips");
        long n = Json_Number(limit, 250), m = Json_Number(starchips, 0);
        /* tables.c read_chest_overflow leaves the whole entry out for a
         * limit past 1-255 or starchips past 0-999999. */
        if ((limit && Json_TypeOf(limit) != JSON_NUMBER) || n < 1 || n > 255 ||
            (starchips && Json_TypeOf(starchips) != JSON_NUMBER) || m < 0 || m > 999999L)
            return;
        limit_claim(x, mod, limit ? "chest_overflow.limit" : "chest_overflow.limit (250, left out)", "chest",
                    limit ? value_of(limit) : number_value(250), overflow);
        limit_claim(x, mod, starchips ? "chest_overflow.starchips" : "chest_overflow.starchips (0, left out)",
                    "chest_overflow.starchips", starchips ? value_of(starchips) : number_value(0), overflow);
    }
}

/* "terrain_bonus": a terrain and a monster type; "replace" clears every
 * pair the earlier mods set. */
static int terrain_named(const char *name)
{
    static const char *const names[][2] = {{"Forest", NULL},     {"Wasteland", NULL}, {"Mountain", NULL},
                                           {"Sogen", "Meadow"}, {"Umi", "Sea"},       {"Yami", "Dark"}};
    if (!name) return -1;
    for (int i = 0; i < 6; i++)
        if (same_letters(name, names[i][0]) || (names[i][1] && same_letters(name, names[i][1]))) return i + 1;
    if (digits_only(name)) return atoi(name);
    return -1;
}
static const char *const terrain_names[] = {"", "Forest", "Wasteland", "Mountain", "Sogen", "Umi", "Yami"};
static void read_terrain(ModsOverlaps *x, int mod)
{
    const JsonValue *table = object_of(member(x, mod, "terrain_bonus"));
    if (!table) return;
    if (Json_Bool(Json_Member(table, "replace"), 0)) {
        Claim *c = claim(x, MODS_OVERLAP_TERRAIN, mod, SUB(RESET_SUB), SET, 0, Json_Member(table, "replace"));
        widened(x, c, SUB_MASK, SUB(1), 0, 0, "replace");
        if (c) c->flags |= RESETS;
        labelled(x, c, "Every terrain bonus (\"replace\")");
    }
    for (const JsonValue *t = Json_At(table, 0); t; t = Json_Next(t)) {
        int terrain = terrain_named(name_of(t));
        if (terrain < 1 || terrain > 6) continue;
        for (const JsonValue *type = Json_At(object_of(t), 0); type; type = Json_Next(type)) {
            char label[160];
            snprintf(label, sizeof(label), "%s bonus of %.60s", terrain_names[terrain], name_of(type));
            labelled(x,
                     claim(x, MODS_OVERLAP_TERRAIN, mod,
                           SUB(1) | ((uint64_t)terrain << 48) | (hash_letters(name_of(type)) >> 16), SET,
                           value_of(type), type),
                     label);
        }
    }
}

/* "trap_thresholds": a trap's threshold, the latest mod's. Only the six
 * attack traps (or copies of them) have one. */
static void read_traps(ModsOverlaps *x, int mod)
{
    for (const JsonValue *t = Json_At(object_of(member(x, mod, "trap_thresholds")), 0); t; t = Json_Next(t)) {
        uint64_t key = card_text(x, name_of(t));
        int base, type, attribute;
        if (!key) continue;
        card_info(x, key, &base, &type, &attribute);
        if (key < TEXT_KEY && (base < ATTACK_TRAP_FIRST || base >= ATTACK_TRAP_FIRST + ATTACK_TRAPS)) continue;
        claim(x, MODS_OVERLAP_TRAPS, mod, key < TEXT_KEY ? (uint64_t)base : key, SET, value_of(t), t);
    }
}

/* "text": the listings' strings by id (translation.c: a later string with
 * the same id replaces an earlier one). An item may name several ids, "[8001
 * 8002]", each as strtoul reads it (listing.c); a "{:L...}" line starts an
 * item of no id. The value is the item's words (its blank and "#" lines
 * aside), so two mods with the same words agree. */
static void text_item(ModsOverlaps *x, int *open, int count, uint32_t value)
{
    for (int i = 0; i < count; i++) x->claims[open[i]].value = value;
}
static void text_file(ModsOverlaps *x, int mod, const char *path)
{
    FILE *file = fopen(path, "rb");
    char *text = NULL, *line, *next;
    long size;
    int open[64], count = 0;
    uint32_t value = 2166136261u;
    if (!file) return;
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) ||
        !(text = malloc((size_t)size + 1)) || fread(text, 1, (size_t)size, file) != (size_t)size) {
        free(text);
        fclose(file);
        return;
    }
    fclose(file);
    text[size] = 0;
    for (line = text; line; line = next) {
        size_t n;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        n = strlen(line);
        if (n && line[n - 1] == '\r') line[--n] = 0;
        if (line[0] == '[' || (line[0] == '{' && line[1] == ':')) {
            const char *close = line[0] == '[' ? strchr(line, ']') : NULL, *word = line + 1;
            text_item(x, open, count, value);
            count = 0;
            value = 2166136261u;
            while (close && word < close && count < 64) {
                char *after;
                unsigned long id = strtoul(word, &after, 16);
                Claim *c;
                if (after == word) break;
                word = after;
                while (word < close && *word == ' ') word++;
                if (id > 0xFFFF) continue;
                if (!(c = claim(x, MODS_OVERLAP_TEXT, mod, (uint64_t)id, SET, 0, NULL))) break;
                open[count++] = (int)(c - x->claims);
            }
        } else if (count && line[0] && line[0] != '#')
            value = (uint32_t)fnv(value * 31u, line);
    }
    text_item(x, open, count, value);
    free(text);
}
static void read_text(ModsOverlaps *x, int mod)
{
    const JsonValue *text = member(x, mod, "text");
    const JsonValue *one = Json_TypeOf(text) == JSON_ARRAY ? Json_At(text, 0) : text;
    char path[1024];
    for (; one; one = Json_TypeOf(text) == JSON_ARRAY ? Json_Next(one) : NULL) {
        const char *name = Json_String(one, Json_String(Json_Member(one, "file"), NULL));
        if (name && x->mods[mod].directory && Paths_Contained(name) && switched_on(x, mod, one) &&
            snprintf(path, sizeof(path), "%s/%s", x->mods[mod].directory, name) < (int)sizeof(path))
            text_file(x, mod, path);
    }
}

/* "title" and "menu": each key the latest mod's (title_config.c). Both say
 * "spacing" and "entries", which are one setting: an entry by its name or
 * its number 0-10. The title's "text" lines add up. A button belongs to the
 * mod that makes it; another changes it by naming it "<mod id>:<id>", and
 * only when it loads after that mod. */
static const char *const entry_names[] = {"new_game", "load",       "duel",    "trade",    "options", "campaign",
                                          "free_duel", "build_deck", "library", "password", "save"};
static int entry_index(const char *name)
{
    char *end;
    long number;
    for (int i = 0; i < 11; i++)
        if (!strcmp(name, entry_names[i])) return i;
    number = strtol(name, &end, 10);
    return *name && !*end && number >= 0 && number < 11 ? (int)number : -1;
}
static void title_tree(ModsOverlaps *x, int mod, const char *path, const char *key, const JsonValue *v, int mode,
                       int flags)
{
    char inner[256], inner_key[256];
    if (Json_TypeOf(v) != JSON_OBJECT) {
        Claim *c = claim(x, MODS_OVERLAP_TITLE, mod, hash_text(key), mode, value_of(v), v);
        if (c) c->flags |= (unsigned char)flags;
        labelled(x, c, path);
        return;
    }
    for (const JsonValue *m = Json_At(v, 0); m; m = Json_Next(m)) {
        snprintf(inner, sizeof(inner), "%s.%.60s", path, name_of(m));
        snprintf(inner_key, sizeof(inner_key), "%s.%.60s", key, name_of(m));
        title_tree(x, mod, inner, inner_key, m, mode, flags);
    }
}
static void title_entries(ModsOverlaps *x, int mod, const char *path, const JsonValue *entries)
{
    for (const JsonValue *e = Json_At(object_of(entries), 0); e; e = Json_Next(e)) {
        char inner[160], key[64];
        int i = entry_index(name_of(e));
        if (i < 0) continue;
        snprintf(inner, sizeof(inner), "%s.%.60s", path, name_of(e));
        snprintf(key, sizeof(key), "entries.%d", i);
        title_tree(x, mod, inner, key, e, SET, 0);
    }
}
static void read_title(ModsOverlaps *x, int mod)
{
    const JsonValue *title = object_of(member(x, mod, "title")), *menu = object_of(member(x, mod, "menu"));
    for (const JsonValue *m = Json_At(title, 0); m; m = Json_Next(m)) {
        char path[128];
        const char *name = name_of(m);
        snprintf(path, sizeof(path), "title.%.60s", name);
        if (!strcmp(name, "text")) {
            if (Json_TypeOf(m) == JSON_ARRAY)
                labelled(x, claim(x, MODS_OVERLAP_TITLE, mod, hash_text(path), ADD, 0, m), "title.text (lines)");
        } else if (!strcmp(name, "entries"))
            title_entries(x, mod, path, m);
        else
            title_tree(x, mod, path, !strcmp(name, "spacing") ? "spacing" : path, m, SET, 0);
    }
    for (const JsonValue *m = Json_At(menu, 0); m; m = Json_Next(m)) {
        char path[160];
        const char *name = name_of(m);
        if (!strcmp(name, "buttons")) {
            /* title_config.c read_buttons: a list of objects. */
            for (const JsonValue *b = Json_At(list_of(m), 0); b; b = Json_Next(b)) {
                const char *id = Json_String(Json_Member(b, "id"), "");
                size_t own = strlen(x->mods[mod].id);
                int theirs = strchr(id, ':') && !(strncmp(id, x->mods[mod].id, own) == 0 && id[own] == ':');
                if (Json_TypeOf(b) != JSON_OBJECT || entry_index(id) >= 0 || !*id) continue;
                if (strchr(id, ':'))
                    snprintf(path, sizeof(path), "menu.buttons.%.100s", id);
                else
                    snprintf(path, sizeof(path), "menu.buttons.%.60s:%.60s", x->mods[mod].id, id);
                for (const JsonValue *k = Json_At(b, 0); k; k = Json_Next(k)) {
                    char leaf[256];
                    if (!strcmp(name_of(k), "id")) continue;
                    snprintf(leaf, sizeof(leaf), "%s.%.60s", path, name_of(k));
                    title_tree(x, mod, leaf, leaf, k, theirs ? SET : BASE, theirs ? AIMED : 0);
                }
            }
        } else if (!strcmp(name, "order")) {
            if (Json_TypeOf(m) == JSON_ARRAY)
                title_tree(x, mod, "menu.order", "menu.order.first", m, SET, 0);
            else
                for (const JsonValue *k = Json_At(object_of(m), 0); k; k = Json_Next(k))
                    if (!strcmp(name_of(k), "first") || !strcmp(name_of(k), "second")) {
                        snprintf(path, sizeof(path), "menu.order.%s", name_of(k));
                        title_tree(x, mod, path, path, k, SET, 0);
                    }
        } else if (!strcmp(name, "entries")) {
            title_entries(x, mod, "menu.entries", m);
        } else {
            snprintf(path, sizeof(path), "menu.%.60s", name);
            title_tree(x, mod, path, !strcmp(name, "spacing") ? "spacing" : path, m, SET, 0);
        }
    }
}

/* "packs" and "pack_shop" (packs.c): a pack is its mod's own, "<mod
 * id>:<id>". The shop's rules are one mod's, the last in the load order's,
 * every rule it leaves out back at its default; its shops are kept by id,
 * a later mod's name and unlock going over an earlier's, the packs of both
 * in it. "packs" may name a file holding the list, or the list and the
 * rules. */
static void pack_rules(ModsOverlaps *x, int mod, const JsonValue *rules)
{
    uint32_t value = 2166136261u;
    if (!object_of(rules)) return;
    for (const JsonValue *m = Json_At(rules, 0); m; m = Json_Next(m))
        if (strcmp(name_of(m), "shops")) value = hash_json(value * 31u, m, 1);
    labelled(x, claim(x, MODS_OVERLAP_PACKS, mod, 1, SET, value, rules), "The pack shop's rules");
    for (const JsonValue *s = Json_At(list_of(Json_Member(rules, "shops")), 0); s; s = Json_Next(s)) {
        const char *id = Json_String(Json_Member(s, "id"), "");
        char label[120];
        if (Json_TypeOf(s) != JSON_OBJECT || !*id) continue;
        snprintf(label, sizeof(label), "Pack shop '%.60s'", id);
        labelled(x, claim(x, MODS_OVERLAP_PACKS, mod, (1ull << 62) | (hash_text(id) >> 2), SET, value_of(s), s), label);
    }
}
/* The digits typed on the Password screen, as a number: up to eight digits
 * as a string, or a number 0-99999999 (packs.c read_password); -1 for none. */
#define PASSWORD_KEY (3ull << 62)
static long password_number(const JsonValue *value)
{
    const char *text = Json_String(value, NULL);
    if (Json_TypeOf(value) == JSON_NUMBER) {
        long number = Json_Number(value, -1);
        return number >= 0 && number <= 99999999L ? number : -1;
    }
    if (!text || !*text || strlen(text) > 8 || strspn(text, "0123456789") != strlen(text)) return -1;
    return atol(text);
}
/* A pack's password: of two packs with one, the first in the packs' list is
 * sold (by "order", else the place it is declared in, across all mods;
 * packs.c Packs_Finish); a card with it too is what the digits sell
 * (pack_shop.c check_passwords). */
static void pack_passwords(ModsOverlaps *x, int mod, const JsonValue *list)
{
    for (const JsonValue *p = Json_At(list_of(list), 0); p; p = Json_Next(p)) {
        long password, declared;
        char label[40];
        Claim *c;
        if (Json_TypeOf(p) != JSON_OBJECT) continue;
        declared = x->packs_declared++;
        if ((password = password_number(Json_Member(p, "password"))) < 0) continue;
        snprintf(label, sizeof(label), "Password %08ld", password);
        if ((c = claim(x, MODS_OVERLAP_PACKS, mod, PASSWORD_KEY | (uint64_t)password, SET, 0, p))) {
            c->lo = Json_TypeOf(Json_Member(p, "order")) == JSON_NUMBER ? Json_Number(Json_Member(p, "order"), 0)
                                                                         : declared;
            c->hi = declared;
            labelled(x, c, label);
        }
    }
}
static void read_packs(ModsOverlaps *x, int mod)
{
    const JsonValue *packs = member(x, mod, "packs");
    if (Json_TypeOf(packs) == JSON_STRING) {
        const JsonValue *file = mod_file(x, mod, Json_String(packs, NULL));
        if (Json_TypeOf(file) == JSON_OBJECT) {
            pack_passwords(x, mod, Json_Member(file, "packs"));
            pack_rules(x, mod, Json_Member(file, "pack_shop"));
        } else
            pack_passwords(x, mod, file);
    } else
        pack_passwords(x, mod, packs);
    pack_rules(x, mod, member(x, mod, "pack_shop"));
}
/* The passwords a mod gives cards ("passwords", by card), beside
 * the packs' and each other's: of two cards with one, the screen gives the
 * lower card number (tables.c Tables_CheckPasswords). */
static void card_passwords(ModsOverlaps *x, int mod)
{
    for (const JsonValue *m = Json_At(object_of(member(x, mod, "passwords")), 0); m; m = Json_Next(m)) {
        const JsonValue *v = Json_Member(m, "password");
        uint64_t card;
        long password;
        Claim *c;
        char label[40];
        if (same_letters(name_of(m), "all") || !(card = card_text(x, name_of(m))))
            continue;
        password = Json_String(v, NULL) && same_letters(Json_String(v, ""), "card number") && card < TEXT_KEY
                       ? (long)card : password_number(v);
        if (password < 0) continue;
        snprintf(label, sizeof(label), "Password %08ld", password);
        if ((c = claim(x, MODS_OVERLAP_PACKS, mod, PASSWORD_KEY | (uint64_t)password, SET, 0, m))) {
            c->flags |= CARD_PW;
            c->lo = card < TEXT_KEY ? (long long)card : 0x7FFFFFFF;
            labelled(x, c, label);
        }
    }
}

/* Code: function hooks chain, the mod applied last called first; event
 * subscribers are all called, by priority. */
static void read_code(ModsOverlaps *x)
{
    char label[200], text[160];
    int mod;
    uint64_t what;
    for (int kind = 0; kind < 2; kind++) {
        int (*next)(int, int *, uint64_t *, char *, size_t, void *) = kind ? x->source.event : x->source.hook;
        for (int i = 0; next && next(i, &mod, &what, text, sizeof(text), x->source.context); i++) {
            if (mod < 0 || mod >= x->mod_count) continue;
            snprintf(label, sizeof(label), kind ? "%s event" : "Function %s", text);
            labelled(x, claim(x, kind ? MODS_OVERLAP_EVENTS : MODS_OVERLAP_HOOKS, mod, what, kind ? EVENT : CHAIN, 0, NULL),
                     label);
        }
    }
}

/* --- wide claims and grouping ------------------------------------------- */

static int by_key(const void *left, const void *right)
{
    const Claim *a = left, *b = right;
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    if (a->mod != b->mod) return a->mod < b->mod ? -1 : 1;
    return (a->seq > b->seq) - (a->seq < b->seq);
}

static int covers(const Claim *wide, uint64_t key)
{
    return (key & wide->mask) == wide->match || (wide->mask2 && (key & wide->mask2) == wide->match2);
}
/* A wide claim stands beside each key of its kind another mod names that
 * it covers, as a copy marked VIA_WIDE; one that resets, only beside the
 * keys of mods loading before it, but for a pool's: an "all" that empties
 * every pool also stands beside a later mod's own replace or fixed deck of
 * one, which empties that pool again (only a later plain edit of it adds
 * up). */
static void widen(ModsOverlaps *x)
{
    int count = x->claim_count, wide = 0;
    for (int i = 0; i < count; i++) wide += x->claims[i].mask != 0;
    if (!wide) return;
    qsort(x->claims, (size_t)count, sizeof(*x->claims), by_key);
    for (int w = 0; w < count; w++) {
        Claim spread = x->claims[w];
        int emitted = 0;
        uint64_t last = 0;
        if (!spread.mask) continue;
        for (int i = 0; i < count; i++) {
            Claim c = x->claims[i], *copy;
            if (c.kind != spread.kind || c.mask || c.mod == spread.mod || !covers(&spread, c.key) ||
                (spread.flags & RESETS && c.mod > spread.mod &&
                 !(spread.kind == MODS_OVERLAP_POOLS && c.mode != ADD)) ||
                (emitted && c.key == last))
                continue;
            if (!(copy = claim(x, spread.kind, spread.mod, c.key, spread.mode, spread.value, spread.src))) return;
            copy->seq = spread.seq;
            copy->label = spread.label;
            copy->via = spread.via;
            copy->flags = (unsigned char)(spread.flags | VIA_WIDE);
            emitted = 1;
            last = c.key;
        }
    }
}

static int declared(const ModsOverlaps *x, int winner, int other)
{
    return x->declared[winner * x->mod_count + other];
}

/* Two card entries of one card, earlier and later from another mod: the
 * keys the later sets differently (it wins them), and the reset keys the
 * earlier sets that the later leaves out (dropped); whether any key met. */
typedef struct {
    char used[200], dropped[200];
    int met, from[8], froms; /* from: the mods whose keys are dropped, in reading order */
} CardKeys;
static void add_word(char *list, size_t size, const char *word)
{
    size_t n = strlen(list);
    char padded[80];
    snprintf(padded, sizeof(padded), ", %s,", word);
    if (n) {
        char check[220];
        snprintf(check, sizeof(check), ", %s,", list);
        if (strstr(check, padded)) return;
    }
    if (n + strlen(word) + 3 < size) snprintf(list + n, size - n, "%s%s", n ? ", " : "", word);
}
static void card_keys(const ModsOverlaps *x, const Group *g, CardKeys *keys)
{
    const Claim *c = &x->claims[g->first];
    memset(keys, 0, sizeof(*keys));
    for (int i = 0; i < g->count; i++)
        for (int j = i + 1; j < g->count; j++) {
            if (c[j].mod == c[i].mod) continue;
            for (const JsonValue *k = Json_At(c[i].src, 0); k; k = Json_Next(k)) {
                const char *name = name_of(k);
                const JsonValue *other = Json_Member(c[j].src, name);
                if (!strcmp(name, "replace") || !strcmp(name, "notes") || !strcmp(name, "id")) continue;
                if (other) {
                    keys->met = 1;
                    if (value_of(other) != value_of(k)) add_word(keys->used, sizeof(keys->used), name);
                } else if (card_resets(name)) {
                    int seen = 0;
                    add_word(keys->dropped, sizeof(keys->dropped), name);
                    for (int f = 0; f < keys->froms; f++) seen |= keys->from[f] == c[i].mod;
                    if (!seen && keys->froms < 8) keys->from[keys->froms++] = c[i].mod;
                }
            }
        }
}

/* Two equip entries of one equip card: for each kind of monster either
 * names (a card, a type, any other; a type and an attribute for a bonus),
 * what each says; the later decides where it says anything. Whether they
 * differ anywhere, and whether they said the same somewhere. */
static const char *const type_names[] = {"Dragon", "Spellcaster", "Zombie", "Warrior", "Beast-Warrior", "Beast",
                                         "Winged Beast", "Fiend", "Fairy", "Insect", "Dinosaur", "Reptile",
                                         "Fish", "Sea Serpent", "Machine", "Thunder", "Aqua", "Pyro", "Rock",
                                         "Plant", "Magic", "Trap", "Ritual", "Equip"};
static const char *const attribute_names[] = {"Light", "Dark", "Earth", "Water", "Fire", "Wind"};
static int type_named(const char *text)
{
    for (int i = 0; i < 24; i++)
        if (same_letters(text, type_names[i])) return i;
    return -1;
}
static int attribute_named(const char *text)
{
    for (int i = 0; i < 6; i++)
        if (same_letters(text, attribute_names[i])) return i;
    return -1;
}
/* A target of "add" or "remove": a card (its key), or a type (-2 - type). */
static long long equip_target(ModsOverlaps *x, const JsonValue *t)
{
    const char *text = Json_String(t, NULL);
    int type = text ? type_named(text) : -1;
    uint64_t key;
    if (type >= 0 && (!x->source.card || x->source.card(text, 0, x->source.context) <= 0)) return -2 - type;
    key = card_key(x, t);
    return key ? (long long)key : -1;
}
/* What an entry says of a monster that is card `card` (0 none) of `type`:
 * 1 may equip, 0 may not, -1 nothing. The most specific rule decides. */
static int equip_allows(ModsOverlaps *x, const JsonValue *entry, long long card, int type)
{
    int best = -1, rank = 0;
    if (Json_Bool(Json_Member(entry, "replace"), 0)) {
        best = 0;
        rank = 1;
    }
    for (int pass = 0; pass < 2; pass++)
        for (const JsonValue *t = Json_At(list_of(Json_Member(entry, pass ? "remove" : "add")), 0); t; t = Json_Next(t)) {
            long long target = equip_target(x, t);
            int r = target >= 0 && target == card ? 3 : target <= -2 && type >= 0 && -2 - target == type ? 2 : 0;
            if (r && r >= rank) {
                best = !pass;
                rank = r;
            }
        }
    return best;
}
/* Its bonus for a monster of `type` and `attribute` (-1: neither named): the
 * first "bonus_if" that fits, then "bonus"; INT32_MIN for nothing. */
static long equip_bonus(const JsonValue *entry, int type, int attribute)
{
    for (const JsonValue *b = Json_At(object_of(Json_Member(entry, "bonus_if")), 0); b; b = Json_Next(b)) {
        int t = type_named(name_of(b)), a = t < 0 || t >= 20 ? attribute_named(name_of(b)) : -1;
        if (Json_TypeOf(b) != JSON_NUMBER) continue;
        if ((t >= 0 && t < 20 && t == type) || (a >= 0 && a == attribute)) return Json_Number(b, 0);
    }
    if (Json_TypeOf(Json_Member(entry, "bonus")) == JSON_NUMBER) return Json_Number(Json_Member(entry, "bonus"), 0);
    return -2147483647L - 1;
}
static void equips_meet(ModsOverlaps *x, const JsonValue *early, const JsonValue *late, int *differ, int *same)
{
    long long cards[64];
    int card_types[64], types[32], attributes[8], n_cards = 0, n_types = 0, n_attributes = 0;
    const JsonValue *entries[2] = {early, late};
    types[n_types++] = -1;
    attributes[n_attributes++] = -1;
    for (int e = 0; e < 2; e++) {
        for (int pass = 0; pass < 2; pass++)
            for (const JsonValue *t = Json_At(list_of(Json_Member(entries[e], pass ? "remove" : "add")), 0); t;
                 t = Json_Next(t)) {
                long long target = equip_target(x, t);
                if (target >= 0 && n_cards < 64) {
                    int base, type, attribute;
                    card_info(x, (uint64_t)target, &base, &type, &attribute);
                    card_types[n_cards] = type;
                    cards[n_cards++] = target;
                } else if (target <= -2 && n_types < 32)
                    types[n_types++] = (int)(-2 - target);
            }
        for (const JsonValue *b = Json_At(object_of(Json_Member(entries[e], "bonus_if")), 0); b; b = Json_Next(b)) {
            int t = type_named(name_of(b)), a = attribute_named(name_of(b));
            if (t >= 0 && t < 20 && n_types < 32) types[n_types++] = t;
            else if (a >= 0 && n_attributes < 8) attributes[n_attributes++] = a;
        }
    }
    *differ = *same = 0;
    for (int i = 0; i < n_cards + n_types; i++) {
        long long card = i < n_cards ? cards[i] : 0;
        int type = i < n_cards ? card_types[i] : types[i - n_cards];
        int a = equip_allows(x, early, card, type), b = equip_allows(x, late, card, type);
        if (a < 0 || b < 0) continue;
        if (a != b) *differ = 1;
        else *same = 1;
    }
    for (int t = 0; t < n_types; t++)
        for (int a = 0; a < n_attributes; a++) {
            long one = equip_bonus(early, types[t], attributes[a]), two = equip_bonus(late, types[t], attributes[a]);
            if (one == -2147483647L - 1 || two == -2147483647L - 1) continue;
            if (one != two) *differ = 1;
            else *same = 1;
        }
}

/* Two patches from different mods over the same bytes; raw sectors from
 * their first sector's byte. */
static int patches_meet(const Claim *a, const Claim *b)
{
    long long base_a = (a->key >> 62) == 3 ? Json_Number(Json_Member(a->src, "lba"), 0) * 2048LL : 0,
              base_b = (b->key >> 62) == 3 ? Json_Number(Json_Member(b->src, "lba"), 0) * 2048LL : 0;
    if ((a->key >> 62) != (b->key >> 62)) return 0;
    for (const JsonValue *p = Json_At(list_of(Json_Member(a->src, "patch")), 0); p; p = Json_Next(p))
        for (const JsonValue *q = Json_At(list_of(Json_Member(b->src, "patch")), 0); q; q = Json_Next(q)) {
            long long pa = base_a + Json_Number(Json_Member(p, "at"), 0), qa = base_b + Json_Number(Json_Member(q, "at"), 0);
            if (pa < qa + patch_length(q) && qa < pa + patch_length(p)) return 1;
        }
    return 0;
}
/* Raw replacements whose sectors meet. */
static int sectors_meet(const Claim *a, const Claim *b) { return a->lo <= b->hi && b->lo <= a->hi; }

static void decide_data(ModsOverlaps *x, Group *g)
{
    const Claim *c = &x->claims[g->first];
    int i, j;
    g->severity = MODS_OVERLAP_WARNING;
    for (i = g->count - 1; i >= 0; i--) /* two replacements: the later's is read */
        for (j = 0; j < i; j++)
            if (c[i].mode == SET && c[j].mode == SET && c[i].mod != c[j].mod && sectors_meet(&c[i], &c[j])) {
                g->winner = c[i].mod;
                g->other = c[j].mod;
                g->outcome = declared(x, g->winner, g->other) ? O_AFTER : O_LATER;
                if (g->outcome == O_AFTER) g->severity = MODS_OVERLAP_INFO;
                return;
            }
    for (i = g->count - 1; i >= 0; i--)
        for (j = 0; j < i; j++)
            if (c[i].mode == ADD && c[j].mode == ADD && c[i].mod != c[j].mod && patches_meet(&c[i], &c[j])) {
                g->winner = c[i].mod;
                g->other = c[j].mod;
                g->outcome = O_BYTES;
                return;
            }
    /* A patch goes over every replacement (mods.c Mods_DiscSector), however
     * the two load: another mod's patch lands in this one's file. */
    for (i = 0; i < g->count; i++)
        for (j = 0; j < g->count; j++)
            if (c[i].mode == ADD && c[j].mode == SET && c[i].mod != c[j].mod && sectors_meet(&c[i], &c[j])) {
                g->winner = c[i].mod;
                g->other = c[j].mod;
                g->outcome = O_PATCHED;
                return;
            }
    g->outcome = O_ADD;
    g->severity = MODS_OVERLAP_INFO;
}

static void decide(ModsOverlaps *x, Group *g)
{
    const Claim *c = &x->claims[g->first];
    int last = -1, fixed = -1, i, losers = 0, agree = 1, all_declared = 1;
    g->winner = g->other = -1;
    g->severity = MODS_OVERLAP_WARNING;
    switch (g->kind) {
    case MODS_OVERLAP_HOOKS:
        g->winner = c[g->count - 1].mod;
        g->outcome = O_CHAIN;
        return;
    case MODS_OVERLAP_EVENTS:
        g->outcome = O_EVENTS;
        g->severity = MODS_OVERLAP_INFO;
        return;
    case MODS_OVERLAP_DATA: decide_data(x, g); return;
    case MODS_OVERLAP_PACKS:
        if ((c[0].key >> 62) == 3) { /* one password: a card's first (the lowest), else the first pack */
            int best = -1;
            for (i = 0; i < g->count; i++) {
                int card = !!(c[i].flags & CARD_PW), best_card = best >= 0 && (c[best].flags & CARD_PW);
                if (best < 0 || (card && !best_card) ||
                    (card == best_card && (c[i].lo < c[best].lo || (c[i].lo == c[best].lo && c[i].hi < c[best].hi))))
                    best = i;
            }
            g->winner = c[best].mod;
            g->best = best;
            g->outcome = O_SOLD;
            return;
        }
        break;
    default: break;
    }
    for (i = 0; i < g->count; i++) {
        if (c[i].mode == FIXED) fixed = i;
        if (c[i].mode == SET || c[i].mode == FIXED) last = i;
    }
    if (g->kind == MODS_OVERLAP_DUELISTS && c[0].mode == FIRST) {
        g->winner = c[0].mod;
        g->outcome = O_FIRST;
        return;
    }
    if (fixed >= 0) {
        g->winner = c[fixed].mod;
        for (i = 0; i < g->count; i++)
            if (c[i].mod != g->winner && (c[i].mode != FIXED || c[i].value != c[fixed].value)) agree = 0;
        g->outcome = agree ? O_AGREE : O_FIXED;
        g->severity = agree ? MODS_OVERLAP_INFO : MODS_OVERLAP_WARNING;
        return;
    }
    /* A button named by a mod loading before the one that makes it: its
     * change is left out (title_config.c read_buttons). */
    for (i = 0; i < g->count; i++)
        if (c[i].mode == BASE)
            for (int j = 0; j < i; j++)
                if (c[j].flags & AIMED && c[j].mod < c[i].mod) {
                    g->winner = c[j].mod;
                    g->other = c[i].mod;
                    g->outcome = O_EARLY;
                    return;
                }
    if (last < 0) {
        g->outcome = O_ADD;
        g->severity = MODS_OVERLAP_INFO;
        return;
    }
    g->winner = c[last].mod;
    for (i = 0; i < last; i++) {
        if (c[i].mod == g->winner) continue;
        if (c[i].mode == BASE) {
            g->other = c[i].mod;
            continue;
        }
        losers++;
        g->other = c[i].mod;
        if (c[i].mode != SET || c[i].value != c[last].value) agree = 0;
        if (!declared(x, g->winner, c[i].mod)) all_declared = 0;
    }
    if (!losers) {
        /* The one change is the only one, on top of what the others add, or
         * of a button another mod made (its BASE). */
        for (i = 0; i < g->count; i++)
            if (c[i].mode == BASE && c[i].mod != g->winner) g->other = c[i].mod;
        g->outcome = g->other >= 0 ? O_AIMED : O_ADD;
        g->severity = MODS_OVERLAP_INFO;
        return;
    }
    if (g->kind == MODS_OVERLAP_CARDS) {
        CardKeys keys;
        card_keys(x, g, &keys);
        if (!*keys.used && !*keys.dropped) {
            g->outcome = keys.met ? O_AGREE : O_ADD;
            g->severity = MODS_OVERLAP_INFO;
            return;
        }
        g->outcome = O_KEYS;
    } else if (g->kind == MODS_OVERLAP_EQUIPS && c[last].key != DEFAULT_EQUIP_BONUS) {
        int differ = 0, same = 0;
        for (i = 0; i < g->count; i++)
            for (int j = i + 1; j < g->count; j++)
                if (c[i].mod != c[j].mod) {
                    int d, s;
                    equips_meet(x, c[i].src, c[j].src, &d, &s);
                    differ |= d;
                    same |= s;
                }
        if (!differ) {
            g->outcome = same ? O_AGREE : O_ADD;
            g->severity = MODS_OVERLAP_INFO;
            return;
        }
        g->outcome = Json_Bool(Json_Member(c[last].src, "replace"), 0) ? O_RESET : O_LATER;
    } else if (agree) {
        g->outcome = O_AGREE;
        g->severity = MODS_OVERLAP_INFO;
        return;
    } else
        g->outcome = c[last].flags & RESETS ? O_RESET : O_LATER;
    if (all_declared) {
        g->outcome = O_AFTER;
        g->severity = MODS_OVERLAP_INFO;
    }
}

/* Within a kind, the warnings first, then as the earliest mod's manifest
 * has them (its claims come first in a group), and between the keys an
 * "all" reaches, as the manifests name them. */
static const Claim *sorting;
static int named_seq(const Group *g)
{
    for (int i = 0; i < g->count; i++)
        if (!(sorting[g->first + i].flags & VIA_WIDE)) return sorting[g->first + i].seq;
    return sorting[g->first].seq;
}
static int by_severity(const void *left, const void *right)
{
    const Group *a = left, *b = right;
    int sa = sorting[a->first].seq, sb = sorting[b->first].seq;
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    if (a->severity != b->severity) return a->severity > b->severity ? -1 : 1;
    if (sa != sb) return sa < sb ? -1 : 1;
    sa = named_seq(a);
    sb = named_seq(b);
    if (sa != sb) return sa < sb ? -1 : 1;
    return (a->first > b->first) - (a->first < b->first);
}

static void group(ModsOverlaps *x)
{
    int start = 0, room = 0;
    if (!x->claim_count) return;
    qsort(x->claims, (size_t)x->claim_count, sizeof(*x->claims), by_key);
    x->groups = NULL;
    for (int i = 1; i <= x->claim_count; i++) {
        const Claim *a = &x->claims[start];
        int mods = 0;
        if (i < x->claim_count && x->claims[i].kind == a->kind && x->claims[i].key == a->key) continue;
        for (int j = start + 1; j < i; j++) mods += x->claims[j].mod != x->claims[j - 1].mod;
        if (mods) {
            Group *g, *groups = grow(x->groups, &room, x->group_count, sizeof(*groups));
            if (!groups) {
                x->failed = 1;
                return;
            }
            x->groups = groups;
            g = &groups[x->group_count++];
            g->first = start;
            g->count = i - start;
            g->kind = a->kind;
            decide(x, g);
        }
        start = i;
    }
    sorting = x->claims;
    if (x->group_count) qsort(x->groups, (size_t)x->group_count, sizeof(*x->groups), by_severity);
}

/* A mod's name as the game keeps one (mods.c copy_text, Menu_TextFit): 95
 * bytes at most, cut where a character starts, so every caller's lines
 * read as the Mods window's do. */
static void fit_name(char *out, const char *name)
{
    size_t length = strlen(name);
    if (length >= 96) {
        length = 95;
        while (length && ((unsigned char)name[length] & 0xC0) == 0x80) length--;
    }
    memcpy(out, name, length);
    out[length] = '\0';
}

ModsOverlaps *Mods_OverlapCompute(const ModsOverlapMod *mods, int count, const ModsOverlapSource *source)
{
    ModsOverlaps *x = calloc(1, sizeof(*x));
    if (!x) return NULL;
    if (source) x->source = *source;
    x->mod_count = count;
    x->mods = calloc((size_t)(count ? count : 1), sizeof(*x->mods));
    x->declared = calloc((size_t)(count ? count * count : 1), 1);
    x->names = calloc((size_t)(count ? count : 1), sizeof(*x->names));
    x->name_list_size = 1;
    for (int i = 0; i < count; i++) x->name_list_size += strlen(mods[i].name ? mods[i].name : "") + 2;
    x->name_list = malloc(x->name_list_size);
    if (!x->mods || !x->declared || !x->names || !x->name_list) {
        Mods_OverlapFree(x);
        return NULL;
    }
    memcpy(x->mods, mods, (size_t)count * sizeof(*mods));
    for (int i = 0; i < count; i++) {
        fit_name(x->names[i], mods[i].name ? mods[i].name : "");
        x->mods[i].name = x->names[i];
    }
    for (int w = 0; w < count; w++)
        for (int k = 0; k < 2; k++)
            for (const JsonValue *v = Json_At(list_of(member(x, w, k ? "requires" : "after")), 0); v; v = Json_Next(v)) {
                const char *id = Json_String(v, Json_String(Json_Member(v, "id"), ""));
                for (int o = 0; o < count; o++)
                    if (!strcmp(id, mods[o].id)) x->declared[w * count + o] = 1;
            }
    if (count < 2) return x;
    for (int mod = 0; mod < count; mod++) read_duelists(x, mod); /* before the pools that name them */
    for (int mod = 0; mod < count; mod++) {
        if (having(x, "data") >= 2) read_data(x, mod);
        if (having(x, "audio") >= 2) read_audio(x, mod);
        if (having(x, "textures") >= 2) read_textures(x, mod);
        if (having(x, "cards") >= 2) read_cards(x, mod);
        if (having(x, "fusions") >= 2) read_fusions(x, mod);
        if (having(x, "equips") + having(x, "equip_bonus_default") >= 2) read_equips(x, mod);
        if (having(x, "rituals") >= 2) read_rituals(x, mod);
        read_pools(x, mod);
        if (having(x, "starter") >= 2 && member(x, mod, "starter"))
            claim(x, MODS_OVERLAP_STARTER, mod, 0, ADD, 0, member(x, mod, "starter"));
        if (having(x, "passwords") >= 2) read_passwords(x, mod);
        /* A pack's password meets another mod's pack or card password too. */
        if (having(x, "packs") + having(x, "pack_shop") >= 2 ||
            (having(x, "packs") && having(x, "packs") + having(x, "passwords") >= 2))
            read_packs(x, mod);
        if (having(x, "packs") + having(x, "passwords") >= 2 && having(x, "passwords")) card_passwords(x, mod);
        if (having(x, "guardian_stars") >= 2) read_stars(x, mod);
        if (having(x, "limits") + having(x, "chest_overflow") >= 2) read_limits(x, mod);
        if (having(x, "terrain_bonus") >= 2) read_terrain(x, mod);
        if (having(x, "trap_thresholds") >= 2) read_traps(x, mod);
        if (having(x, "text") >= 2) read_text(x, mod);
        if (having(x, "font") >= 2 && member(x, mod, "font"))
            claim(x, MODS_OVERLAP_FONT, mod, 0, ADD, 0, member(x, mod, "font"));
        if (having(x, "title") + having(x, "menu") >= 2) read_title(x, mod);
    }
    read_code(x);
    sector_runs(x);
    widen(x);
    group(x);
    if (x->failed) {
        Mods_OverlapFree(x);
        return NULL;
    }
    return x;
}

void Mods_OverlapFree(ModsOverlaps *x)
{
    if (!x) return;
    for (int i = 0; i < x->document_count; i++) Json_Free(x->documents[i]);
    free(x->documents);
    free(x->claims);
    free(x->groups);
    free(x->strings);
    free(x->memo);
    free(x->star_names);
    free(x->defined);
    free(x->declared);
    free(x->names);
    free(x->name_list);
    free(x->mods);
    free(x);
}

/* --- lines ---------------------------------------------------------------- */

int Mods_OverlapCount(const ModsOverlaps *x) { return x ? x->group_count : 0; }
int Mods_OverlapKind(const ModsOverlaps *x, int index) { return x->groups[index].kind; }
int Mods_OverlapSeverity(const ModsOverlaps *x, int index) { return x->groups[index].severity; }
const char *Mods_OverlapOutcome(const ModsOverlaps *x, int index) { return outcome_words[x->groups[index].outcome]; }
int Mods_OverlapInvolves(const ModsOverlaps *x, int index, int mod)
{
    const Group *g = &x->groups[index];
    for (int i = 0; i < g->count; i++)
        if (x->claims[g->first + i].mod == mod) return 1;
    return 0;
}

const char *Mods_OverlapKindName(int kind)
{
    static const char *const names[MODS_OVERLAP_KINDS] = {
        "Disc data", "Sounds",   "Texture images", "Cards",        "Fusions",  "Equips",          "Rituals",
        "Drops and decks", "Starter decks", "Passwords", "Card packs", "Guardian Stars", "Limits", "Terrain bonuses",
        "Attack traps", "Duelists", "Text", "Fonts", "Title screen and menus", "Code hooks", "Game events"};
    return kind >= 0 && kind < MODS_OVERLAP_KINDS ? names[kind] : "";
}

static void star_words(const ModsOverlaps *x, int star, char *out, size_t size)
{
    if (star >= 1 && star <= 10) {
        snprintf(out, size, "%s", star_retail[star]);
        return;
    }
    for (int mod = 0; mod < x->mod_count; mod++)
        for (const JsonValue *s = Json_At(list_of(Json_Member(object_of(member(x, mod, "guardian_stars")), "stars")), 0);
             s; s = Json_Next(s)) {
            const JsonValue *name = Json_Member(s, "name");
            if (Json_Number(Json_Member(s, "id"), -1) != star) continue;
            if (Json_TypeOf(name) == JSON_OBJECT) name = Json_At(name, 0);
            if (Json_String(name, NULL)) {
                snprintf(out, size, "%s", Json_String(name, ""));
                return;
            }
        }
    snprintf(out, size, "Star %d", star);
}

void Mods_OverlapLabel(const ModsOverlaps *x, int index, char *out, size_t size)
{
    const Group *g = &x->groups[index];
    const Claim *c = &x->claims[g->first];
    char a[120], b[120];
    for (int i = 0; i < g->count; i++)
        if (!(x->claims[g->first + i].flags & VIA_WIDE)) {
            c = &x->claims[g->first + i];
            break;
        }
    if (c->label >= 0) {
        snprintf(out, size, "%s", x->strings + c->label);
        return;
    }
    switch (g->kind) {
    case MODS_OVERLAP_DATA:
        snprintf(out, size, "File %s", Json_String(Json_Member(c->src, "file"), "?"));
        return;
    case MODS_OVERLAP_AUDIO: {
        static const char *const kinds[] = {"Song", "XA clip", "Sound effect"};
        snprintf(out, size, "%s 0x%llX", kinds[(c->key >> 32) % 3], (unsigned long long)(c->key & 0xFFFFFFFFu));
        return;
    }
    case MODS_OVERLAP_TEXTURES:
        snprintf(out, size, "Image %s, %s at 0x%lX",
                 Json_String(Json_Member(c->src, "alias"), Json_String(Json_Member(c->src, "file"), "?")),
                 Json_String(Json_Member(c->src, "archive"), "?"), Json_Number(Json_Member(c->src, "offset"), 0));
        return;
    case MODS_OVERLAP_CARDS:
        card_words(x, Json_Member(c->src, "replace"), a, sizeof(a));
        snprintf(out, size, "Card %s", a);
        return;
    case MODS_OVERLAP_FUSIONS:
        if (Json_Member(c->src, "remove")) {
            card_words(x, Json_Member(c->src, "remove"), a, sizeof(a));
            snprintf(out, size, "Disc recipes for %s removed", a);
        } else {
            const JsonValue *with = Json_Member(c->src, "with");
            card_words(x, Json_At(with, 0), a, sizeof(a));
            card_words(x, Json_Next(Json_At(with, 0)), b, sizeof(b));
            snprintf(out, size, "Fusion %s + %s", a, b);
        }
        return;
    case MODS_OVERLAP_EQUIPS:
        if (c->key == DEFAULT_EQUIP_BONUS) {
            snprintf(out, size, "The default equip bonus");
            return;
        }
        card_words(x, Json_Member(c->src, "card"), a, sizeof(a));
        snprintf(out, size, "Equip %s", a);
        return;
    case MODS_OVERLAP_RITUALS:
        card_words(x, Json_Member(c->src, "card"), a, sizeof(a));
        snprintf(out, size, "Ritual %s", a);
        return;
    case MODS_OVERLAP_STARTER: snprintf(out, size, "Starter decks"); return;
    case MODS_OVERLAP_STARS: {
        int sub = (int)(c->key >> 56), one = (int)((c->key >> 8) & 0xFF), two = (int)(c->key & 0xFF);
        static const char *const fields[] = {"", "name", "icon", "palette"};
        if (sub == 1) {
            star_words(x, one, a, sizeof(a));
            star_words(x, two, b, sizeof(b));
            snprintf(out, size, "Matchup %s attacking %s", a, b);
        } else if (sub == 2) {
            star_words(x, one, a, sizeof(a));
            snprintf(out, size, "The %s of star %d, %s", fields[two & 3], one, a);
        } else if (sub == 3)
            snprintf(out, size, "How a summon chooses its star");
        else
            snprintf(out, size, "Every matchup (\"replace\")");
        return;
    }
    case MODS_OVERLAP_TRAPS:
        snprintf(out, size, "Attack trap '%s'", name_of(c->src));
        return;
    case MODS_OVERLAP_TEXT: snprintf(out, size, "Text [%04llX]", (unsigned long long)c->key); return;
    case MODS_OVERLAP_FONT: snprintf(out, size, "Fonts"); return;
    default: snprintf(out, size, "?"); return;
    }
}

void Mods_OverlapMods(const ModsOverlaps *x, int index, char *out, size_t size)
{
    const Group *g = &x->groups[index];
    size_t length = 0;
    if (size) *out = 0;
    for (int i = 0; i < g->count; i++) {
        int mod = x->claims[g->first + i].mod;
        if (i && x->claims[g->first + i - 1].mod == mod) continue;
        if (length < size)
            length += (size_t)snprintf(out + length, size - length, "%s%s", length ? ", " : "", x->mods[mod].id);
    }
}

void Mods_OverlapText(const ModsOverlaps *x, int index, char *out, size_t size)
{
    const Group *g = &x->groups[index];
    char label[256], *names = x->name_list, through[120] = "", word[90] = "replace";
    const char *winner = g->winner >= 0 ? x->mods[g->winner].name : "",
               *other = g->other >= 0 ? x->mods[g->other].name : "";
    size_t length = 0;
    int distinct = 0;
    Mods_OverlapLabel(x, index, label, sizeof(label));
    names[0] = 0;
    for (int i = 0; i < g->count; i++) {
        const Claim *c = &x->claims[g->first + i];
        if (c->mod == g->winner && c->flags & VIA_WIDE && c->via >= 0 && !*through) {
            snprintf(through, sizeof(through), ", through its \"%.80s\"", x->strings + c->via);
            snprintf(word, sizeof(word), "%.80s", x->strings + c->via);
        }
        if (i && x->claims[g->first + i - 1].mod == c->mod) continue;
        distinct++;
        if (length < x->name_list_size)
            length += (size_t)snprintf(names + length, x->name_list_size - length, "%s%s", length ? ", " : "",
                                       x->mods[c->mod].name);
    }
    switch (g->outcome) {
    case O_LATER:
        snprintf(out, size, "%s (%s): %s wins (later in load order%s)", label, names, winner, through);
        break;
    case O_AFTER:
        snprintf(out, size, "%s (%s): %s wins (it loads after %s on purpose: after/requires)", label, names,
                 winner, other);
        break;
    case O_AGREE:
        snprintf(out, size, "%s (%s): %s", label, names,
                 g->kind == MODS_OVERLAP_CARDS ? "where they set the same key they agree; the rest combines"
                                               : "the same in each, so no difference");
        break;
    case O_ADD:
        snprintf(out, size, "%s (%s): %s apply and add up%s", label, names, distinct > 2 ? "all" : "both",
                 g->kind == MODS_OVERLAP_DATA      ? " (replacements first, then patches of different bytes)"
                 : g->kind == MODS_OVERLAP_CARDS   ? " (they set different stats)"
                 : g->kind == MODS_OVERLAP_POOLS   ? " (each edits the pool as the mods before left it)"
                 : g->kind == MODS_OVERLAP_FONT    ? " (a letter comes from the first font that has it)"
                                                   : "");
        break;
    case O_RESET:
        snprintf(out, size, "%s (%s): %s's \"%s\" clears what the earlier mods set", label, names, winner, word);
        break;
    case O_FIXED:
        snprintf(out, size, "%s (%s): %s's fixed deck is dealt; the other edits of it are left out", label, names,
                 winner);
        break;
    case O_KEYS: {
        /* Named: which mod's keys are used and which mod's are dropped. */
        CardKeys keys;
        char used[400] = "", dropped[1500] = "", from[900] = "";
        card_keys(x, g, &keys);
        for (int f = 0; f < keys.froms; f++) {
            size_t n = strlen(from);
            snprintf(from + n, sizeof(from) - n, "%s%s", !f ? "" : f == keys.froms - 1 ? " and " : ", ",
                     x->mods[keys.from[f]].name);
        }
        if (*keys.used)
            snprintf(used, sizeof(used), "%s's %s %s used", winner, keys.used, strchr(keys.used, ',') ? "are" : "is");
        if (*keys.dropped)
            snprintf(dropped, sizeof(dropped), "%sthe %s %s gave %s dropped, as %s's replace starts %s from the disc's",
                     *used ? " and " : "", keys.dropped, from, strchr(keys.dropped, ',') ? "are" : "is", winner,
                     strchr(keys.dropped, ',') ? "them" : "it");
        snprintf(out, size, "%s (%s): %s%s (later in load order); the rest combines", label, names, used, dropped);
        break;
    }
    case O_BYTES:
        snprintf(out, size, "%s (%s): %s's bytes are read where they patch the same ones", label, names, winner);
        break;
    case O_PATCHED:
        snprintf(out, size, "%s (%s): %s's patch is written into %s's replacement, at the disc's offsets", label,
                 names, winner, other);
        break;
    case O_CHAIN:
        snprintf(out, size, "%s (%s): %s's hook runs first (applied last); the others run only if it calls its original",
                 label, names, winner);
        break;
    case O_EVENTS:
        snprintf(out, size,
                 "%s (%s): each is called, higher priority first; a before-hook that handles it stops the rest", label,
                 names);
        break;
    case O_FIRST:
        snprintf(out, size, "%s (%s): %s keeps it, earlier in load order; the others take the next free slot", label,
                 names, winner);
        break;
    case O_AIMED:
        snprintf(out, size, "%s (%s): %s changes %s's own on purpose", label, names, winner, other);
        break;
    case O_SOLD: {
        const Claim *best = &x->claims[g->first + g->best];
        if (best->flags & CARD_PW)
            snprintf(out, size, "%s (%s): the digits give %s's card '%s'%s", label, names, winner, name_of(best->src),
                     g->count > 1 ? ", not the others' card or pack" : "");
        else
            snprintf(out, size, "%s (%s): %s's pack '%s' is sold, the first in the list; the others' with it are not",
                     label, names, winner, Json_String(Json_Member(best->src, "id"), Json_String(Json_Member(best->src, "name"), "?")));
        break;
    }
    case O_EARLY:
        snprintf(out, size, "%s (%s): %s's change is left out: it loads before %s, whose button it names", label,
                 names, winner, other);
        break;
    }
}
