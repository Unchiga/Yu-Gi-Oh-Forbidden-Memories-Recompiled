/* The manifest reader: what a mod.json is allowed to say, and what happens
 * to one that is malformed. */
#include "pc/mods/json.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *stream_path = "json_stream_test.tmp";
static const char *const stream_key[] = {"fusions"};
static void write_stream(const char *text)
{
    FILE *file = fopen(stream_path, "wb");
    assert(file && fputs(text, file) >= 0 && !fclose(file));
}

/* Adds up what the elements hold, and how many there were. */
static long seen_sum, seen_stop = -1;
static int sum_element(const JsonValue *value, long index, void *context)
{
    long *count = context;
    seen_sum += Json_TypeOf(value) == JSON_OBJECT ? Json_Number(Json_Member(value, "n"), 0) : Json_Number(value, 0);
    (*count)++;
    return index != seen_stop;
}

/* A list left out of the document and read from the file, one element at a time. */
static void stream_tests(void)
{
    char error[128];
    JsonDeferred list, other;
    JsonDocument *document;
    long count = 0;
    /* What is around the list stays, strings that look like it are not it, and an
     * empty list stands in for it. */
    write_stream("\xEF\xBB\xBF{\"a\": \"fusions\", \"note\": \"\\\"fusions\\\": [\", \"inner\": {\"fusions\": [1]},\n"
                 "  \"fusions\" : [ {\"n\": 1, \"s\": \"]}\\\"[\"}, 2 , {\"n\": 3},\n 4,], \"b\": [5]}");
    document = Json_ParseFileDeferring(stream_path, stream_key, 1, &list, error, sizeof(error));
    assert(document && list.found);
    assert(!strcmp(Json_String(Json_Member(Json_Root(document), "a"), ""), "fusions"));
    assert(Json_Count(Json_Member(Json_Member(Json_Root(document), "inner"), "fusions")) == 1);
    assert(Json_TypeOf(Json_Member(Json_Root(document), "fusions")) == JSON_ARRAY);
    assert(Json_Count(Json_Member(Json_Root(document), "fusions")) == 0);
    assert(Json_Count(Json_Member(Json_Root(document), "b")) == 1);
    assert(Json_EachElement(stream_path, &list, sum_element, &count, error, sizeof(error)) == 4);
    assert(count == 4 && seen_sum == 10 && !error[0]);
    Json_Free(document);
    /* The hash is of the list alone. */
    write_stream("{\"z\": 1, \"fusions\" : [ {\"n\": 1, \"s\": \"]}\\\"[\"}, 2 , {\"n\": 3},\n 4,]}");
    document = Json_ParseFileDeferring(stream_path, stream_key, 1, &other, error, sizeof(error));
    assert(document && other.hash == list.hash);
    Json_Free(document);
    write_stream("{\"z\": 1, \"fusions\" : [ {\"n\": 1, \"s\": \"]}\\\"[\"}, 2 , {\"n\": 3},\n 5,]}");
    document = Json_ParseFileDeferring(stream_path, stream_key, 1, &other, error, sizeof(error));
    assert(document && other.hash != list.hash);
    Json_Free(document);
    /* `each` can stop it. */
    seen_sum = count = 0;
    seen_stop = 1;
    write_stream("{\"fusions\": [1, 2, 3]}");
    document = Json_ParseFileDeferring(stream_path, stream_key, 1, &list, error, sizeof(error));
    assert(document && Json_EachElement(stream_path, &list, sum_element, &count, error, sizeof(error)) == 2);
    assert(count == 2 && seen_sum == 3);
    seen_stop = -1;
    Json_Free(document);
    /* An entry that does not parse is that entry's mistake: the rest are read. */
    seen_sum = count = 0;
    write_stream("{\"fusions\": [{\"n\": 1}, {\"n\" 2}, {\"n\": 4}]}");
    document = Json_ParseFileDeferring(stream_path, stream_key, 1, &list, error, sizeof(error));
    assert(document);
    assert(Json_EachElement(stream_path, &list, sum_element, &count, error, sizeof(error)) == -1);
    assert(count == 2 && seen_sum == 5 && strstr(error, "entry 1:"));
    Json_Free(document);
    /* What the parser would not have taken in a list is a mistake, and the rest read. */
    {
        static const struct { const char *text; long sum; const char *why; } lists[] = {
            {"{\"fusions\": [{\"n\": 1} {\"n\": 2}]}", 3, "no comma"},
            {"{\"fusions\": [1,,2]}", 3, "a comma with no entry"},
            {"{\"fusions\": [,1]}", 1, "a comma with no entry"},
            {"{\"fusions\": [1 2,3]}", 3, "trailing text"},
            {"{\"fusions\": [\"a\" 4]}", 4, "no comma"},
            {"{\"fusions\": [{\"n\": 1}x, 2]}", 3, "no comma"},
        };
        for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
            write_stream(lists[i].text);
            document = Json_ParseFileDeferring(stream_path, stream_key, 1, &list, error, sizeof(error));
            assert(document);
            seen_sum = count = 0;
            assert(Json_EachElement(stream_path, &list, sum_element, &count, error, sizeof(error)) == -1);
            assert(seen_sum == lists[i].sum && strstr(error, lists[i].why));
            Json_Free(document);
        }
        /* A trailing comma stays allowed; form feed and vertical tab are spaces. */
        write_stream("{\"fusions\": [1\f,\v2,]}");
        document = Json_ParseFileDeferring(stream_path, stream_key, 1, &list, error, sizeof(error));
        seen_sum = count = 0;
        assert(document && Json_EachElement(stream_path, &list, sum_element, &count, error, sizeof(error)) == 2);
        assert(seen_sum == 3 && !error[0]);
        Json_Free(document);
        write_stream("{\"fusions\": [\f]}");
        document = Json_ParseFileDeferring(stream_path, stream_key, 1, &list, error, sizeof(error));
        assert(document && list.found && !list.filled);
        assert(!Json_EachElement(stream_path, &list, sum_element, &count, error, sizeof(error)));
        Json_Free(document);
    }
    /* A list closed with the wrong bracket refuses the file, as the parser did. */
    write_stream("{\"fusions\": [1}}");
    assert(!Json_ParseFileDeferring(stream_path, stream_key, 1, &list, error, sizeof(error)) && error[0]);
    /* The first of a key is the one that counts, list or not; an escaped one is not it. */
    write_stream("{\"fusions\": 5, \"fusions\": [1, 2]}");
    document = Json_ParseFileDeferring(stream_path, stream_key, 1, &list, error, sizeof(error));
    assert(document && !list.found && Json_Count(Json_At(Json_Root(document), 1)) == 2);
    Json_Free(document);
    write_stream("{\"fu\\tsions\": [1, 2], \"fusions\\n\": [3]}");
    document = Json_ParseFileDeferring(stream_path, stream_key, 1, &list, error, sizeof(error));
    assert(document && !list.found && Json_Count(Json_At(Json_Root(document), 0)) == 2);
    Json_Free(document);
    /* One entry over the limit is its mistake alone, whichever kind it is. */
    {
        size_t size = 3u << 20;
        char *big = malloc(size);
        size_t used = 0;
        assert(big);
        used += (size_t)sprintf(big + used, "{\"fusions\": [1, {\"s\": \"");
        memset(big + used, 'x', 1500000);
        used += 1500000;
        used += (size_t)sprintf(big + used, "\"}, 2, ");
        big[used++] = '"';
        memset(big + used, 'y', 1200000);
        used += 1200000;
        strcpy(big + used, "\", 4]}");
        write_stream(big);
        free(big);
        document = Json_ParseFileDeferring(stream_path, stream_key, 1, &list, error, sizeof(error));
        assert(document);
        seen_sum = count = 0;
        assert(Json_EachElement(stream_path, &list, sum_element, &count, error, sizeof(error)) == -1);
        assert(seen_sum == 7 && count == 3 && strstr(error, "entry 1:") && strstr(error, "megabyte"));
        Json_Free(document);
    }
    /* A file shorter than it was at the scan says so. */
    write_stream("{\"fusions\": [1, 2, 3, 4, 5, 6, 7, 8]}");
    document = Json_ParseFileDeferring(stream_path, stream_key, 1, &list, error, sizeof(error));
    assert(document);
    write_stream("{\"fusions\": [1, 2");
    seen_sum = count = 0;
    assert(Json_EachElement(stream_path, &list, sum_element, &count, error, sizeof(error)) == -1 && error[0]);
    Json_Free(document);
    /* Two lists, either way round, each with its own hash and read whole. */
    {
        static const char *const both[] = {"fusions", "cards"};
        JsonDeferred two[2], again[2];
        JsonDocument *whole;
        write_stream("{\"cards\": [{\"n\": 5}, 6], \"x\": 1, \"fusions\": [7]}");
        document = Json_ParseFileDeferring(stream_path, both, 2, two, error, sizeof(error));
        assert(document && two[0].found && two[1].found && two[0].filled && two[1].filled);
        assert(Json_Number(Json_Member(Json_Root(document), "x"), 0) == 1);
        assert(!Json_Count(Json_Member(Json_Root(document), "cards")));
        whole = Json_ParseDeferred(stream_path, &two[1], error, sizeof(error), NULL);
        assert(whole && Json_Count(Json_Root(whole)) == 2);
        assert(Json_Number(Json_At(Json_Root(whole), 1), 0) == 6);
        Json_Free(whole);
        Json_Free(document);
        write_stream("{\"fusions\": [7], \"x\": 1, \"cards\": [{\"n\": 5}, 6]}");
        document = Json_ParseFileDeferring(stream_path, both, 2, again, error, sizeof(error));
        assert(document && again[0].hash == two[0].hash && again[1].hash == two[1].hash);
        assert(again[0].hash != again[1].hash);
        Json_Free(document);
    }
    write_stream("{\"fusions\": [{\"n\": 1}]}");
    document = Json_ParseFileDeferring(stream_path, stream_key, 1, &list, error, sizeof(error));
    Json_Free(document);
    /* Whether anything is in it. */
    assert(list.filled);
    write_stream("{\"fusions\": [ \n ]}");
    document = Json_ParseFileDeferring(stream_path, stream_key, 1, &list, error, sizeof(error));
    assert(document && list.found && !list.filled);
    Json_Free(document);
    /* Not a list: stays in the document, as does a second one. */
    write_stream("{\"fusions\": 7, \"x\": 1}");
    document = Json_ParseFileDeferring(stream_path, stream_key, 1, &list, error, sizeof(error));
    assert(document && !list.found && Json_Number(Json_Member(Json_Root(document), "fusions"), 0) == 7);
    Json_Free(document);
    write_stream("{\"fusions\": [1], \"fusions\": [2, 3]}");
    document = Json_ParseFileDeferring(stream_path, stream_key, 1, &list, error, sizeof(error));
    assert(document && list.found && Json_Count(Json_At(Json_Root(document), 1)) == 2);
    Json_Free(document);
    /* A list that does not end, and a mistake outside the list, refuse the file. */
    write_stream("{\"fusions\": [1, 2");
    assert(!Json_ParseFileDeferring(stream_path, stream_key, 1, &list, error, sizeof(error)) && error[0]);
    write_stream("{\"fusions\": [1], \"b\" 2}");
    assert(!Json_ParseFileDeferring(stream_path, stream_key, 1, &list, error, sizeof(error)) && error[0]);
    assert(!Json_ParseFileDeferring("no-such-file.json", stream_key, 1, &list, error, sizeof(error)) && error[0]);
    /* Across the chunks the file is read in, on both sides of the list. */
    {
        size_t size = 5u << 20, used = 0;
        char *big = malloc(size + 64);
        long want = 0, elements = 0;
        assert(big);
        used += (size_t)sprintf(big + used, "{\"pad\": \"");
        memset(big + used, 'x', 1500000);
        used += 1500000;
        used += (size_t)sprintf(big + used, "\", \"fusions\": [");
        while (used < size - (1u << 20)) {
            used += (size_t)sprintf(big + used, "{\"n\":%ld},", elements);
            want += elements++;
        }
        used += (size_t)sprintf(big + used, "], \"tail\": \"");
        memset(big + used, 'y', 1000000);
        used += 1000000;
        strcpy(big + used, "\"}");
        write_stream(big);
        document = Json_ParseFileDeferring(stream_path, stream_key, 1, &list, error, sizeof(error));
        assert(document && list.found);
        assert(strlen(Json_String(Json_Member(Json_Root(document), "pad"), "")) == 1500000);
        assert(strlen(Json_String(Json_Member(Json_Root(document), "tail"), "")) == 1000000);
        seen_sum = count = 0;
        assert(Json_EachElement(stream_path, &list, sum_element, &count, error, sizeof(error)) == elements);
        assert(count == elements && seen_sum == want);
        Json_Free(document);
        free(big);
    }
    remove(stream_path);
}

static const char *manifest =
    "{\n"
    "    \"id\": \"card-tweaks\",\n"
    "    \"name\": \"Card tweaks\",\n"
    "    \"enabled\": true,\n"
    "    \"restart\": false,\n"
    "    \"weight\": 12,\n"
    "    \"note\": \"a \\\"quoted\\\" \\\\ word\\nand a line\",\n"
    "    \"data\": [\n"
    "        { \"file\": \"\\\\DATA\\\\CARD.MRG;1\", \"replace\": \"card.mrg\" },\n"
    "        { \"lba\": 2776, \"patch\": [ { \"at\": \"0x5D800\", \"bytes\": \"26 25\" } ] }\n"
    "    ]\n"
    "}\n";

/* \u escapes are the characters as UTF-8, a surrogate pair one past U+FFFF;
 * a lone surrogate is U+FFFD. */
static void unicode_escapes(void)
{
    char error[128];
    JsonDocument *document = Json_Parse("[\"W\\u00e4chter \\u00df\\u20ac \\uD83D\\uDE00 \\uD800x\", \"\\u0041\"]", error,
                                        sizeof(error));
    const JsonValue *list;
    assert(document);
    list = Json_Root(document);
    assert(!strcmp(Json_String(Json_At(list, 0), ""),
                   "W\xc3\xa4" "chter \xc3\x9f\xe2\x82\xac \xf0\x9f\x98\x80 \xef\xbf\xbd" "x"));
    assert(!strcmp(Json_String(Json_At(list, 1), ""), "A"));
    Json_Free(document);
}

int main(void)
{
    char error[128];
    JsonDocument *document = Json_Parse(manifest, error, sizeof(error));
    unicode_escapes();
    const JsonValue *root, *data, *entry, *patch;
    assert(document);
    root = Json_Root(document);
    assert(Json_TypeOf(root) == JSON_OBJECT);
    assert(!strcmp(Json_String(Json_Member(root, "id"), ""), "card-tweaks"));
    assert(!strcmp(Json_String(Json_Member(root, "name"), ""), "Card tweaks"));
    assert(Json_Bool(Json_Member(root, "enabled"), 0) == 1);
    assert(Json_Bool(Json_Member(root, "restart"), 1) == 0);
    assert(Json_Bool(Json_Member(root, "missing"), 7) == 7);
    assert(Json_Number(Json_Member(root, "weight"), 0) == 12);
    /* Escapes are undone in place, and the text still ends where it should. */
    assert(!strcmp(Json_String(Json_Member(root, "note"), ""), "a \"quoted\" \\ word\nand a line"));
    data = Json_Member(root, "data");
    assert(Json_TypeOf(data) == JSON_ARRAY && Json_Count(data) == 2);
    entry = Json_At(data, 0);
    assert(!strcmp(Json_String(Json_Member(entry, "file"), ""), "\\DATA\\CARD.MRG;1"));
    assert(!strcmp(Json_String(Json_Member(entry, "replace"), ""), "card.mrg"));
    entry = Json_At(data, 1);
    assert(Json_Number(Json_Member(entry, "lba"), -1) == 2776);
    patch = Json_Member(entry, "patch");
    assert(Json_Count(patch) == 1);
    /* An offset written the way a tutorial writes one. */
    assert(Json_Number(Json_Member(Json_At(patch, 0), "at"), -1) == 0x5D800);
    assert(!strcmp(Json_String(Json_Member(Json_At(patch, 0), "bytes"), ""), "26 25"));
    assert(!Json_Count(Json_Member(root, "id")));
    assert(!Json_At(data, 2) && !Json_At(data, -1));
    /* Walking the children one after another sees what Json_At does. */
    assert(Json_Next(Json_At(data, 0)) == Json_At(data, 1) && !Json_Next(Json_At(data, 1)) && !Json_Next(NULL));
    Json_Free(document);

    /* Malformed manifests fail with a reason rather than a crash. */
    assert(!Json_Parse("{ \"id\": }", error, sizeof(error)) && error[0]);
    assert(!Json_Parse("{ \"id\" \"x\" }", error, sizeof(error)) && error[0]);
    assert(!Json_Parse("{ \"id\": \"x\"", error, sizeof(error)) && error[0]);
    assert(!Json_Parse("{} trailing", error, sizeof(error)) && error[0]);
    assert(!Json_Parse("", error, sizeof(error)) && error[0]);
    assert(!Json_Parse("{ \"a\": \"\\q\" }", error, sizeof(error)) && error[0]);
    assert(!Json_ParseFile("/does/not/exist.json", error, sizeof(error)) && error[0]);
    /* Numbers are JSON's, in base 10, and whole. */
    {
        static const struct { const char *text; long value; } good[] = {
            {"[0]", 0}, {"[-0]", 0}, {"[10]", 10}, {"[-42]", -42}, {"[1e3]", 1000}, {"[2.50e1]", 25},
            {"[7.0]", 7}, {"[1000E-3]", 1}, {"[1E+2]", 100}, {"[2147483647]", 2147483647L},
        };
        static const char *bad[] = {
            "[010]", "[08]", "[1-2]", "[1.5]", "[1e-3]", "[1.]", "[.5]", "[-]", "[1e]", "[+1]",
            "[0x10]", "[99999999999999999999]", "[1e400]",
        };
        char text[64];
        unsigned i;
        for (i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
            document = Json_Parse(good[i].text, error, sizeof(error));
            assert(document && Json_Number(Json_At(Json_Root(document), 0), -1) == good[i].value);
            Json_Free(document);
        }
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            assert(!Json_Parse(bad[i], error, sizeof(error)) && error[0]);
        }
        /* A long's own ends, whatever its size on this system. */
        snprintf(text, sizeof(text), "[%ld, %ld]", LONG_MAX, LONG_MIN);
        document = Json_Parse(text, error, sizeof(error));
        assert(document && Json_Number(Json_At(Json_Root(document), 0), 0) == LONG_MAX &&
               Json_Number(Json_At(Json_Root(document), 1), 0) == LONG_MIN);
        Json_Free(document);
        /* Strings that hold a number: hexadecimal with its 0x, else decimal. */
        document = Json_Parse("[\"0x5D800\", \"010\", \" -3 \", \"08\", \"1.5\"]", error, sizeof(error));
        assert(document);
        assert(Json_Number(Json_At(Json_Root(document), 0), -1) == 0x5D800);
        assert(Json_Number(Json_At(Json_Root(document), 1), -1) == 10);
        assert(Json_Number(Json_At(Json_Root(document), 2), -1) == -3);
        assert(Json_Number(Json_At(Json_Root(document), 3), -1) == 8);
        assert(Json_Number(Json_At(Json_Root(document), 4), -1) == -1);
        Json_Free(document);
    }
    /* Nesting has a limit, so a hostile manifest cannot run the stack out. */
    {
        static char deep[200001];
        memset(deep, '[', sizeof(deep) - 1);
        assert(!Json_Parse(deep, error, sizeof(error)) && strstr(error, "nested"));
        memset(deep, 0, sizeof(deep));
        memset(deep, '[', 64);
        memset(deep + 64, ']', 64);
        document = Json_Parse(deep, error, sizeof(error));
        assert(document);
        Json_Free(document);
        deep[128] = ']';
        memmove(deep + 1, deep, 128);
        deep[0] = '[';
        assert(!Json_Parse(deep, error, sizeof(error)) && strstr(error, "nested"));
    }
    /* A trailing comma is common enough in a hand-written manifest to allow. */
    document = Json_Parse("{ \"a\": [1, 2,], }", error, sizeof(error));
    assert(document && Json_Count(Json_Member(Json_Root(document), "a")) == 2);
    Json_Free(document);
    Json_Free(NULL);
    stream_tests();
    return 0;
}
