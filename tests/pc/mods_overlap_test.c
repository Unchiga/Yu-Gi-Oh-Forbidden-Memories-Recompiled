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

    /* A name as json.c reads it and the game keeps it: a \u escape is one
     * byte (0xC9 alone, not UTF-8), and six names of 95 bytes are all in
     * the line, each cut to 95 bytes where a letter starts. */
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
        assert(!strcmp(line, "Fonts (\xc9" "clair, 1000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000): "
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

    for (int i = 0; i < n; i++) free(lines[i]);
    for (int i = 0; i < count; i++) {
        free((void *)list[i].directory);
        Json_Free(documents[i]);
    }
    free(expected);
    Json_Free(setup);
    printf("mods_overlap: %d lines as expected\n", n);
    return 0;
}
