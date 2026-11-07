/* What two or more mods change in common (src/pc/mods/overlap.c), against
 * the mods in tests/pc/mod_overlaps: every overlap they make, one line each
 * as "kind|severity|outcome|mods|label", a tab and the line the Mods window
 * shows, in the engine's order, must be expected.txt there. The FM Editor's
 * check (tools/pc/fm_editor/overlaps.py) reads the same fixture and must
 * find the same lines in the same order. Then the cases a manifest cannot
 * show: code hooks and events, one mod, and many thousands of rules. */
#include "pc/mods/json.h"
#include "pc/mods/overlap.h"
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const JsonValue *fixture;
/* What tests/pc/mod_overlaps/duelists-64 makes (tools/pc/fm_editor/tests/test_overlaps.py too). */
#define DUELISTS_64 "Heishin's POW drops (B, C): C's \"replace\" clears what the earlier mods set"

static int same_letters(const char *a, const char *b)
{
    for (;;) {
        while (*a && !isalnum((unsigned char)*a)) a++;
        while (*b && !isalnum((unsigned char)*b)) b++;
        if (!*a || !*b) return !*a && !*b;
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    }
}
/* The fixture's cards: by number, or by name letter for letter. */
static int card(const char *text, long number, void *context)
{
    const JsonValue *cards = Json_Member(fixture, "cards");
    (void)context;
    for (const JsonValue *c = Json_At(cards, 0); c; c = Json_Next(c)) {
        long id = atol(Json_Name(c));
        if (text ? same_letters(text, Json_String(c, "")) || !strcmp(text, Json_Name(c)) : number == id) return (int)id;
    }
    return -1;
}
static int card_name(int id, char *out, size_t size, void *context)
{
    char key[16];
    (void)context;
    snprintf(key, sizeof(key), "%d", id);
    if (!Json_Member(Json_Member(fixture, "cards"), key)) return 0;
    snprintf(out, size, "%s", Json_String(Json_Member(Json_Member(fixture, "cards"), key), ""));
    return 1;
}
static int duelist(const char *text, void *context)
{
    (void)context;
    for (const JsonValue *d = Json_At(Json_Member(fixture, "duelists"), 0); d; d = Json_Next(d))
        if (same_letters(text, Json_Name(d)) || atol(text) == Json_Number(d, -1)) return (int)Json_Number(d, -1);
    return -1;
}

/* The fixture's "types": a card's monster type and attribute. */
static int card_info(int id, int *base, int *type, int *attribute, void *context)
{
    char key[16];
    const JsonValue *info;
    (void)context;
    snprintf(key, sizeof(key), "%d", id);
    *base = (int)Json_Number(Json_Member(Json_Member(fixture, "bases"), key), id);
    if (!(info = Json_Member(Json_Member(fixture, "types"), key))) return 0;
    *type = (int)Json_Number(Json_At(info, 0), -1);
    *attribute = (int)Json_Number(Json_At(info, 1), -1);
    return 1;
}

/* A mod's setting as a fresh install has it: its declared default. */
static ModsOverlapMod list[8];
static int setting(int mod, const char *key, void *context)
{
    (void)context;
    for (const JsonValue *s = Json_At(Json_Member(list[mod].manifest, "settings"), 0); s; s = Json_Next(s))
        if (!strcmp(Json_String(Json_Member(s, "key"), ""), key)) return (int)Json_Number(Json_Member(s, "default"), 0);
    return -1;
}

/* Two hooks on one function and two subscriptions to one event, from the
 * second and third mods. */
static int hook(int index, int *mod, uint64_t *what, char *label, size_t size, void *context)
{
    (void)context;
    if (index > 1) return 0;
    *mod = index + 1;
    *what = 0x1000;
    snprintf(label, size, "DuelScene_UpdateResultRewards");
    return 1;
}
static int event(int index, int *mod, uint64_t *what, char *label, size_t size, void *context)
{
    (void)context;
    if (index > 1) return 0;
    *mod = 2 - index;
    *what = 3;
    snprintf(label, size, "FUSION");
    return 1;
}

static char *read_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *text;
    assert(f);
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = calloc(1, (size_t)n + 1);
    assert(text && fread(text, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    return text;
}

/* Lists as Mods_EachFusion gives them: rules gone when `each` returns. */
static const JsonValue *stream_lists[4];
static int streamed;
static long stream_fusions(int mod, int (*each)(const JsonValue *, long, void *), void *each_context, void *context)
{
    long i = 0;
    (void)context;
    streamed++;
    for (const JsonValue *rule = Json_At(stream_lists[mod], 0); rule; rule = Json_Next(rule)) each(rule, i++, each_context);
    return i;
}
static long stream_bytes(int mod, void *context)
{
    (void)context;
    return (long)Json_Count(stream_lists[mod]);
}

/* The same mods give the same lines with their fusions in the manifests and
 * read from the files. */
static void streamed_fusions(void)
{
    static const char *const in_manifest[3] = {
        "{\"id\": \"a\", \"fusions\": [{\"with\": [1, 2], \"result\": 3}, {\"with\": [4, 5], \"result\": 6},"
        " {\"remove\": 7}, {\"with\": [8, 9], \"result\": 10}]}",
        "{\"id\": \"b\", \"fusions\": [{\"with\": [2, 1], \"result\": 11}, {\"remove\": 7}, {\"remove\": \"all\"}]}",
        "{\"id\": \"c\", \"fusions\": [{\"with\": [9, 8], \"result\": 10}, {\"remove\": \"all\"},"
        " {\"with\": [1, 2], \"result\": 12}]}"};
    JsonDocument *with[3], *empty[3];
    ModsOverlapMod held[3], read[3];
    ModsOverlapSource source = {0};
    ModsOverlaps *a, *b;
    char error[128], x[512], y[512];
    memset(held, 0, sizeof(held));
    memset(read, 0, sizeof(read));
    for (int i = 0; i < 3; i++) {
        with[i] = Json_Parse(in_manifest[i], error, sizeof(error));
        empty[i] = Json_Parse("{\"fusions\": []}", error, sizeof(error));
        assert(with[i] && empty[i]);
        stream_lists[i] = Json_Member(Json_Root(with[i]), "fusions");
        held[i].id = read[i].id = i == 0 ? "a" : i == 1 ? "b" : "c";
        held[i].name = read[i].name = held[i].id;
        held[i].manifest = Json_Root(with[i]);
        read[i].manifest = Json_Root(empty[i]);
    }
    a = Mods_OverlapCompute(held, 3, &source);
    source.fusions = stream_fusions;
    source.fusion_bytes = stream_bytes;
    b = Mods_OverlapCompute(read, 3, &source);
    assert(a && b && streamed);
    assert(Mods_OverlapCount(a) == Mods_OverlapCount(b) && Mods_OverlapCount(a) >= 3);
    for (int i = 0; i < Mods_OverlapCount(a); i++) {
        Mods_OverlapText(a, i, x, sizeof(x));
        Mods_OverlapText(b, i, y, sizeof(y));
        assert(!strcmp(x, y));
        assert(!strcmp(Mods_OverlapOutcome(a, i), Mods_OverlapOutcome(b, i)));
        assert(Mods_OverlapSeverity(a, i) == Mods_OverlapSeverity(b, i));
    }
    Mods_OverlapFree(a);
    Mods_OverlapFree(b);
    for (int i = 0; i < 3; i++) {
        Json_Free(with[i]);
        Json_Free(empty[i]);
    }
}

int main(void)
{
    char path[1024], error[256], line[8192], label[512], mods[512], text[4096];
    JsonDocument *documents[8], *setup;
    ModsOverlapSource source = {0};
    ModsOverlaps *found;
    char *lines[512], *expected, *at;
    int count = 0, n = 0, failed = 0;

    snprintf(path, sizeof(path), "%s/tests/pc/mod_overlaps/fixture.json", MEMORIES_SOURCE_DIR);
    setup = Json_ParseFile(path, error, sizeof(error));
    assert(setup);
    fixture = Json_Root(setup);
    for (const JsonValue *id = Json_At(Json_Member(fixture, "order"), 0); id; id = Json_Next(id), count++) {
        char *directory = malloc(1024);
        snprintf(directory, 1024, "%s/tests/pc/mod_overlaps/%s", MEMORIES_SOURCE_DIR, Json_String(id, ""));
        snprintf(path, sizeof(path), "%s/mod.json", directory);
        documents[count] = Json_ParseFile(path, error, sizeof(error));
        assert(documents[count]);
        list[count].id = Json_String(id, "");
        list[count].name = Json_String(Json_Member(Json_Root(documents[count]), "name"), "");
        list[count].directory = directory;
        list[count].manifest = Json_Root(documents[count]);
    }
    source.card = card;
    source.card_name = card_name;
    source.card_info = card_info;
    source.duelist = duelist;
    source.setting = setting;
    found = Mods_OverlapCompute(list, count, &source);
    assert(found);

    /* The fixture: the lines the FM Editor finds too. */
    for (int i = 0; i < Mods_OverlapCount(found); i++) {
        Mods_OverlapLabel(found, i, label, sizeof(label));
        Mods_OverlapMods(found, i, mods, sizeof(mods));
        Mods_OverlapText(found, i, text, sizeof(text));
        snprintf(line, sizeof(line), "%s|%s|%s|%s|%s\t%s", Mods_OverlapKindName(Mods_OverlapKind(found, i)),
                 Mods_OverlapSeverity(found, i) ? "warning" : "info", Mods_OverlapOutcome(found, i), mods, label, text);
        assert(n < 512);
        lines[n] = malloc(strlen(line) + 1);
        assert(lines[n]);
        strcpy(lines[n++], line);
        printf("  %s\n", text);
    }
    snprintf(path, sizeof(path), "%s/tests/pc/mod_overlaps/expected.txt", MEMORIES_SOURCE_DIR);
    expected = read_all(path);
    at = expected;
    for (int i = 0; i < n || *at; i++) {
        char *end = strchr(at, '\n');
        size_t length = end ? (size_t)(end - at) : strlen(at), advance = length + (end != NULL);
        if (length && at[length - 1] == '\r') /* a checkout with CRLF */
            length--;
        if (i >= n || strlen(lines[i]) != length || strncmp(lines[i], at, length)) {
            fprintf(stderr, "line %d: found   %s\n        expected %.*s\n", i + 1, i < n ? lines[i] : "(nothing)",
                    (int)length, at);
            failed = 1;
            break;
        }
        at += advance;
    }
    if (failed) {
        fprintf(stderr, "every line found:\n");
        for (int i = 0; i < n; i++) fprintf(stderr, "%s\n", lines[i]);
    }
    assert(!failed);
    Mods_OverlapFree(found);

    /* tests/pc/mod_overlaps/duelists-64: 64 duelists of one mod's folder, then
     * one of its "duelists" list without an "id" (the game loads it), then
     * another mod's duelist whose pool file a third mod's edit meets. The
     * id-less entry once grew the list without keeping it: the later
     * definition was lost, or past 64 written to freed memory. */
    {
        static const char *const ids[] = {"a", "b", "c"};
        JsonDocument *docs[3];
        ModsOverlapMod three[3];
        char *directories[3];
        for (int m = 0; m < 3; m++) {
            directories[m] = malloc(1024);
            snprintf(directories[m], 1024, "%s/tests/pc/mod_overlaps/duelists-64/%s", MEMORIES_SOURCE_DIR, ids[m]);
            snprintf(path, sizeof(path), "%s/mod.json", directories[m]);
            docs[m] = Json_ParseFile(path, error, sizeof(error));
            assert(docs[m]);
            three[m].id = ids[m];
            three[m].name = Json_String(Json_Member(Json_Root(docs[m]), "name"), "");
            three[m].directory = directories[m];
            three[m].manifest = Json_Root(docs[m]);
        }
        found = Mods_OverlapCompute(three, 3, &source);
        assert(found && Mods_OverlapCount(found) == 1);
        Mods_OverlapText(found, 0, line, sizeof(line));
        printf("duelists-64: %s\n", line);
        assert(!strcmp(line, DUELISTS_64));
        Mods_OverlapFree(found);
        for (int m = 0; m < 3; m++) {
            Json_Free(docs[m]);
            free(directories[m]);
        }
    }

    /* tests/pc/mod_overlaps/starter-pools: each "# a, b" a set of its mods in
     * load order, then the lines it makes (test_overlaps.py too). Pools of
     * several mods add up and are dealt from only at forty draws, one mod's
     * that do not draw forty make a line too, and a written deck wins. */
    {
        char *sets, *row, *next;
        JsonDocument *docs[8];
        ModsOverlapMod some[8];
        char *directories[8];
        int m = 0, have = 0, set_count = 0;
        snprintf(path, sizeof(path), "%s/tests/pc/mod_overlaps/starter-pools/expected.txt", MEMORIES_SOURCE_DIR);
        sets = read_all(path);
        found = NULL;
        for (row = sets; row && *row; row = next) {
            size_t length;
            next = strchr(row, '\n');
            if (next) *next++ = 0;
            length = strlen(row);
            if (length && row[length - 1] == '\r') row[--length] = 0;
            if (!strncmp(row, "# ", 2)) {
                /* The set before is done: every line it makes was read. */
                assert(!found || have == Mods_OverlapCount(found));
                Mods_OverlapFree(found);
                for (int i = 0; i < m; i++) {
                    Json_Free(docs[i]);
                    free(directories[i]);
                }
                m = have = 0;
                for (char *id = strtok(row + 2, ", "); id; id = strtok(NULL, ", ")) {
                    assert(m < 8);
                    directories[m] = malloc(1024);
                    snprintf(directories[m], 1024, "%s/tests/pc/mod_overlaps/starter-pools/%s", MEMORIES_SOURCE_DIR, id);
                    snprintf(path, sizeof(path), "%s/mod.json", directories[m]);
                    docs[m] = Json_ParseFile(path, error, sizeof(error));
                    assert(docs[m]);
                    some[m].id = Json_String(Json_Member(Json_Root(docs[m]), "id"), "");
                    some[m].name = Json_String(Json_Member(Json_Root(docs[m]), "name"), "");
                    some[m].directory = directories[m];
                    some[m].manifest = Json_Root(docs[m]);
                    m++;
                }
                found = Mods_OverlapCompute(some, m, &source);
                assert(found);
                set_count++;
                continue;
            }
            if (!length) continue;
            assert(found && have < Mods_OverlapCount(found));
            Mods_OverlapLabel(found, have, label, sizeof(label));
            Mods_OverlapMods(found, have, mods, sizeof(mods));
            Mods_OverlapText(found, have, text, sizeof(text));
            snprintf(line, sizeof(line), "%s|%s|%s|%s|%s\t%s", Mods_OverlapKindName(Mods_OverlapKind(found, have)),
                     Mods_OverlapSeverity(found, have) ? "warning" : "info", Mods_OverlapOutcome(found, have), mods,
                     label, text);
            if (strcmp(line, row)) {
                fprintf(stderr, "starter-pools: found   %s\n               expected %s\n", line, row);
                assert(0);
            }
            printf("starter-pools: %s\n", text);
            {   /* How many mods it names, as its ids do (one for a lone mod's pools). */
                int named = 1;
                for (const char *comma = mods; (comma = strchr(comma, ',')); comma++) named++;
                assert(Mods_OverlapModCount(found, have) == named);
            }
            have++;
        }
        assert(!found || have == Mods_OverlapCount(found));
        Mods_OverlapFree(found);
        for (int i = 0; i < m; i++) {
            Json_Free(docs[i]);
            free(directories[i]);
        }
        assert(set_count >= 8);
        free(sets);
    }

    /* Code: hooks chain, events are all called. */
    source.hook = hook;
    source.event = event;
    found = Mods_OverlapCompute(list, count, &source);
    assert(found);
    {
        int hooks = 0, events = 0;
        for (int i = 0; i < Mods_OverlapCount(found); i++) {
            Mods_OverlapText(found, i, line, sizeof(line));
            if (Mods_OverlapKind(found, i) == MODS_OVERLAP_HOOKS) {
                hooks++;
                assert(!strcmp(Mods_OverlapOutcome(found, i), "chain"));
                assert(Mods_OverlapSeverity(found, i) == MODS_OVERLAP_WARNING);
                assert(strstr(line, "Function DuelScene_UpdateResultRewards (Beta, Gamma): Gamma's hook runs first"));
                assert(!Mods_OverlapInvolves(found, i, 0) && Mods_OverlapInvolves(found, i, 2));
            }
            if (Mods_OverlapKind(found, i) == MODS_OVERLAP_EVENTS) {
                events++;
                assert(Mods_OverlapSeverity(found, i) == MODS_OVERLAP_INFO);
                assert(strstr(line, "FUSION event (Beta, Gamma): each is called, higher priority first; a before-hook"));
            }
        }
        assert(hooks == 1 && events == 1);
    }
    Mods_OverlapFree(found);

    /* Wrong types, as a hand-written manifest has them: a list where an
     * object belongs, a number where a list does. The readers note and
     * leave them out; so do the overlaps, without reading a name an
     * element of a list does not have. */
    {
        static const char *const wrong[] = {
            "{\"id\":\"w%d\",\"limits\":[1],\"title\":[{}],\"menu\":[[1]],\"terrain_bonus\":[1],\"decks\":[{}],"
            "\"drops\":[{\"pow\":{}}],\"passwords\":[1],\"trap_thresholds\":[2],\"audio\":{\"music\":[\"a\"]}}",
            "{\"id\":\"w%d\",\"cards\":5,\"fusions\":5,\"equips\":5,\"rituals\":5,\"data\":5,\"textures\":5,"
            "\"guardian_stars\":{\"stars\":5,\"matchups\":5},\"menu\":{\"buttons\":[[1],5,{\"id\":5}]},\"text\":5}",
            "{\"id\":\"w%d\",\"limits\":{\"life_points\":{\"duelists\":[1]}},\"guardian_stars\":{\"stars\":[{\"id\":11,"
            "\"beats\":5}]},\"drops\":{\"all\":[1]},\"decks\":{\"all\":5},\"terrain_bonus\":{\"Forest\":[1]}}"};
        for (int a = 0; a < 3; a++)
            for (int b = 0; b < 3; b++) {
                char text[1024];
                JsonDocument *docs[2];
                ModsOverlapMod pair[2];
                for (int m = 0; m < 2; m++) {
                    snprintf(text, sizeof(text), wrong[m ? b : a], m);
                    docs[m] = Json_Parse(text, error, sizeof(error));
                    assert(docs[m]);
                    pair[m].id = m ? "w1" : "w0";
                    pair[m].name = pair[m].id;
                    pair[m].directory = NULL;
                    pair[m].manifest = Json_Root(docs[m]);
                }
                found = Mods_OverlapCompute(pair, 2, &source);
                assert(found);
                for (int i = 0; i < Mods_OverlapCount(found); i++) Mods_OverlapText(found, i, line, sizeof(line));
                Mods_OverlapFree(found);
                Json_Free(docs[0]);
                Json_Free(docs[1]);
            }
    }

    /* The cards follow the load order (mods.c Mods_VisitCards); a later
     * replace that gives no plate drops the earlier's, as it does the name. */
    {
        JsonDocument *docs[2];
        ModsOverlapMod pair[2];
        for (int m = 0; m < 2; m++) {
            snprintf(line, sizeof(line), "{\"id\":\"m%d\",\"cards\":[{\"replace\":1,\"attack\":%d%s}]}", m,
                     100 * (m + 1), m ? "" : ",\"title\":\"A\"");
            docs[m] = Json_Parse(line, error, sizeof(error));
            assert(docs[m]);
            pair[m].id = pair[m].name = m ? "B" : "A";
            pair[m].directory = NULL;
            pair[m].manifest = Json_Root(docs[m]);
        }
        found = Mods_OverlapCompute(pair, 2, NULL);
        assert(found && Mods_OverlapCount(found) == 1);
        Mods_OverlapText(found, 0, line, sizeof(line));
        assert(!strcmp(line, "Card #1 (A, B): B's attack is used and the title A gave is dropped, as B's replace starts "
                             "it from the disc's (later in load order); the rest combines"));
        Mods_OverlapFree(found);
        Json_Free(docs[0]);
        Json_Free(docs[1]);
    }

    /* One mod's pack and another's card password, and no second pack: the
     * digits give the card (pack_shop.c check_passwords). */
    for (int added = 0; added < 2; added++) {
        const char *texts[2] = {"{\"passwords\":{\"Kuriboh\":{\"password\":\"87654321\"}}}",
                                             "{\"packs\":[{\"name\":\"B\",\"cards\":[1],\"password\":\"87654321\"}]}"};
        if (added) texts[0] = "{\"passwords\":{\"723\":{\"password\":\"87654321\"}}}";
        JsonDocument *docs[2];
        ModsOverlapMod pair[2];
        for (int m = 0; m < 2; m++) {
            docs[m] = Json_Parse(texts[m], error, sizeof(error));
            assert(docs[m]);
            pair[m].id = m ? "b" : "a";
            pair[m].name = m ? "B" : "A";
            pair[m].directory = NULL;
            pair[m].manifest = Json_Root(docs[m]);
        }
        found = Mods_OverlapCompute(pair, 2, NULL);
        assert(found && Mods_OverlapCount(found) == 1);
        Mods_OverlapText(found, 0, line, sizeof(line));
        assert(!strcmp(line, added
            ? "Password 87654321 (A, B): the digits give A's card '723', not the others' card or pack"
            : "Password 87654321 (A, B): the digits give A's card 'Kuriboh', not the others' card or pack"));
        Mods_OverlapFree(found);
        Json_Free(docs[0]);
        Json_Free(docs[1]);
    }

    /* A name as json.c reads it and the game keeps it: a \u escape is its
     * character as UTF-8 (C3 89), and six names of 95 bytes are all in the
     * line, each cut to 95 bytes where a letter starts. */
    {
        JsonDocument *docs[6];
        ModsOverlapMod six[6];
        char long_name[6][140];
        static const char *const ids[6] = {"m0", "m1", "m2", "m3", "m4", "m5"};
        for (int m = 0; m < 6; m++) {
            snprintf(line, sizeof(line), "{\"name\":\"%s\",\"font\":\"f.ttf\"}", m ? "x" : "\\u00c9clair");
            docs[m] = Json_Parse(line, error, sizeof(error));
            assert(docs[m]);
            /* 94 letters and a two-byte one across the 95th byte: cut before it. */
            snprintf(long_name[m], sizeof(long_name[m]), "%d%093d\xc3\xb1tail", m, 0);
            six[m].id = ids[m];
            six[m].name = m ? long_name[m] : Json_String(Json_Member(Json_Root(docs[m]), "name"), "");
            six[m].directory = NULL;
            six[m].manifest = Json_Root(docs[m]);
        }
        found = Mods_OverlapCompute(six, 2, NULL);
        assert(found && Mods_OverlapCount(found) == 1);
        Mods_OverlapText(found, 0, line, sizeof(line));
        assert(!strcmp(line, "Fonts (\xc3\x89" "clair, 1000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000): "
                             "both apply and add up (a letter comes from the first font that has it)"));
        Mods_OverlapFree(found);
        found = Mods_OverlapCompute(six + 1, 5, NULL);
        assert(found && Mods_OverlapCount(found) == 1);
        Mods_OverlapText(found, 0, line, sizeof(line));
        assert(strstr(line, ", 5000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000): all apply"));
        Mods_OverlapFree(found);
        for (int m = 0; m < 6; m++) Json_Free(docs[m]);
    }

    /* One mod, or none: nothing is read and nothing overlaps. */
    found = Mods_OverlapCompute(list, 1, &source);
    assert(found && Mods_OverlapCount(found) == 0);
    Mods_OverlapFree(found);
    found = Mods_OverlapCompute(list, 0, NULL);
    assert(found && Mods_OverlapCount(found) == 0);
    Mods_OverlapFree(found);

    /* Two mods of every pair of 400 cards' fusions (79,800 rules each, as the
     * FM Editor's bulk fusions writes) meet on every pair: worked out once,
     * in well under a second. */
    {
        size_t room = 8u << 20, length = 0;
        char *text = malloc(room);
        JsonDocument *big[2];
        ModsOverlapMod pair[2];
        clock_t start;
        double seconds;
        for (int m = 0; m < 2; m++) {
            length = (size_t)snprintf(text, room, "{\"id\": \"bulk%d\", \"fusions\": [", m);
            for (int a = 1; a <= 400; a++)
                for (int b = a + 1; b <= 400; b++)
                    length += (size_t)snprintf(text + length, room - length, "%s{\"with\": [%d, %d], \"result\": %d}",
                                               a == 1 && b == 2 ? "" : ",", a, b, m ? 700 : 1);
            snprintf(text + length, room - length, "]}");
            big[m] = Json_Parse(text, error, sizeof(error));
            assert(big[m]);
            pair[m].id = m ? "bulk1" : "bulk0";
            pair[m].name = pair[m].id;
            pair[m].directory = NULL;
            pair[m].manifest = Json_Root(big[m]);
        }
        start = clock();
        found = Mods_OverlapCompute(pair, 2, NULL);
        seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
        assert(found && Mods_OverlapCount(found) == 79800);
        Mods_OverlapText(found, 0, line, sizeof(line));
        printf("bulk: %d overlaps in %.3f s, the first: %s\n", Mods_OverlapCount(found), seconds, line);
        assert(seconds < 5.0);
        Mods_OverlapFree(found);
        Json_Free(big[0]);
        Json_Free(big[1]);
        free(text);
    }

    /* Where a ritual's tributes come from is part of its recipe: two mods
     * agree only when that does too, "field" written out or left out. */
    {
        static const char *const texts[3] = {
            "{\"id\":\"r1\",\"rituals\":[{\"card\":\"Black Luster Ritual\",\"tributes\":[\"Kuriboh\"],"
            "\"result\":\"Black Luster Soldier\"}]}",
            "{\"id\":\"r2\",\"rituals\":[{\"card\":\"Black Luster Ritual\",\"tributes_from\":\"field\","
            "\"tributes\":[\"Kuriboh\"],\"result\":\"Black Luster Soldier\"}]}",
            "{\"id\":\"r3\",\"rituals\":[{\"card\":\"Black Luster Ritual\",\"tributes_from\":\"hand\","
            "\"tributes\":[\"Kuriboh\"],\"result\":\"Black Luster Soldier\"}]}"};
        static const char *const expect[2] = {"agree", "later"};
        JsonDocument *docs[3];
        ModsOverlapMod pair[2];
        for (int m = 0; m < 3; m++) {
            docs[m] = Json_Parse(texts[m], error, sizeof(error));
            assert(docs[m]);
        }
        for (int other = 1; other <= 2; other++) {
            for (int m = 0; m < 2; m++) {
                const JsonValue *root = Json_Root(docs[m ? other : 0]);
                pair[m].id = Json_String(Json_Member(root, "id"), "");
                pair[m].name = pair[m].id;
                pair[m].directory = MEMORIES_SOURCE_DIR;
                pair[m].manifest = root;
            }
            found = Mods_OverlapCompute(pair, 2, &source);
            assert(found && Mods_OverlapCount(found) == 1);
            printf("rituals r1, r%d: %s\n", other + 1, Mods_OverlapOutcome(found, 0));
            assert(!strcmp(Mods_OverlapOutcome(found, 0), expect[other - 1]));
            Mods_OverlapFree(found);
        }
        for (int m = 0; m < 3; m++) Json_Free(docs[m]);
    }

    for (int i = 0; i < n; i++) free(lines[i]);
    for (int i = 0; i < count; i++) {
        free((void *)list[i].directory);
        Json_Free(documents[i]);
    }
    free(expected);
    Json_Free(setup);
    streamed_fusions();
    printf("mods_overlap: %d lines as expected\n", n);
    return 0;
}
