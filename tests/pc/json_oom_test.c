/* json.c with its Nth allocation failing: what each caller is told. Memory running
 * out is its own answer, never a mistake in the mod: Json_EachElement and
 * Json_ParseDeferred must say so wherever it happens. */
#include "pc/compat/fs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int countdown = -1;
static int fail_now(void) { return countdown >= 0 && countdown-- == 0; }
static void *test_malloc(size_t n) { return fail_now() ? NULL : malloc(n); }
static void *test_calloc(size_t a, size_t b) { return fail_now() ? NULL : calloc(a, b); }
static void *test_realloc(void *p, size_t n) { return fail_now() ? NULL : realloc(p, n); }
#define malloc test_malloc
#define calloc test_calloc
#define realloc test_realloc
#include "pc/mods/json.c"
#undef malloc
#undef calloc
#undef realloc

static int seen;
static int on(const JsonValue *value, long index, void *context)
{
    (void)value;
    (void)index;
    (void)context;
    seen++;
    return 1;
}

int main(void)
{
    const char *path = "json_oom_test.tmp";
    FILE *file = fopen(path, "wb");
    static const char *const keys[] = {"fusions", "cards"};
    JsonDeferred lists[2];
    JsonDocument *document;
    char error[160];
    int failed = 0;
    assert(file);
    fputs("{\"id\":\"x\",\"cards\":[{\"a\":1}],\"fusions\":[", file);
    for (int i = 0; i < 300; i++) fprintf(file, "{\"with\":[%d,2],\"result\":3},", i + 1);
    fputs("{\"with\":[1,2],\"result\":3}]}", file);
    assert(!fclose(file));
    document = Json_ParseFileDeferring(path, keys, 2, lists, error, sizeof(error));
    assert(document);
    /* The whole list as a document. */
    for (int n = 0;; n++) {
        int no_memory = -1;
        JsonDocument *cards;
        countdown = n;
        cards = Json_ParseDeferred(path, &lists[1], error, sizeof(error), &no_memory);
        if (cards) {
            assert(!no_memory);
            Json_Free(cards);
            break;
        }
        assert(no_memory == 1);
    }
    /* Entry by entry: every allocation, the buffers, each block of values. */
    for (int n = 0;; n++) {
        long read;
        seen = 0;
        countdown = n;
        read = Json_EachElement(path, &lists[0], on, NULL, error, sizeof(error));
        if (countdown >= 0) { /* it never got that far */
            assert(read == 301 && seen == 301);
            break;
        }
        assert(read == JSON_NO_MEMORY);
        failed++;
    }
    countdown = -1;
    assert(failed);
    Json_Free(document);
    remove(path);
    printf("JSON out of memory: %d allocations of the list, each told as such\n", failed);
    return 0;
}
