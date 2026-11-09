#ifndef MEMORIES_PC_MODS_JSON_H
#define MEMORIES_PC_MODS_JSON_H
/* Enough JSON to read a mod manifest (mods.h): objects, arrays, strings,
 * whole numbers, true/false and null. Numbers keep their text as well, so a
 * manifest can write a disc offset as "0x5D800" and have it read either way.
 * Nothing here allocates while the game runs: a manifest is parsed once, and a
 * list too big to hold is read from the file an element at a time (below).
 * Numbers must be whole and fit a long: base 10 (a leading zero is still
 * decimal) or hexadecimal after 0x. Comments of both of C's kinds and a trailing
 * comma are allowed; arrays and objects nest at most 64 deep. */
#include <stddef.h>

typedef enum {
    JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;
typedef struct JsonDocument JsonDocument;

/* Parses `text`; on failure returns NULL and writes why into `error`. */
JsonDocument *Json_Parse(const char *text, char *error, size_t error_size);
JsonDocument *Json_ParseFile(const char *path, char *error, size_t error_size);

/* A list too big to hold parsed: where it is in the file, and a hash of it. */
typedef struct JsonDeferred {
    long start, end; /* of the '[' and the ']' */
    unsigned hash;
    int found;
    int filled; /* anything in it */
    int named;  /* the key was met, as a list or not: a second one is an ordinary member */
} JsonDeferred;

/* Reads a file a chunk at a time and parses all of it but the top-level members
 * `keys`, each when it is a list: an empty list stands in its place in the document,
 * and deferred[i] says where keys[i] is. Nothing of it is looked into but its
 * brackets and strings; a mistake inside comes out when it is read. */
JsonDocument *Json_ParseFileDeferring(const char *path, const char *const *keys, int count, JsonDeferred *deferred,
                                      char *error, size_t error_size);
/* A list left out as a document of its own, whole. *no_memory, if asked, says that
 * memory is why there is none. */
JsonDocument *Json_ParseDeferred(const char *path, const JsonDeferred *deferred, char *error, size_t error_size,
                                 int *no_memory);
/* Each element of that list in turn, parsed alone and gone when `each` returns;
 * `each` returns 0 to stop. An element that does not parse, or is over a megabyte,
 * is left out and the rest are read, and so is a comma too many or too few. The
 * number of elements seen, or -1 when there was such a mistake or the file could not
 * be read in full (`error` has the first), or JSON_NO_MEMORY when memory ran out: the
 * elements after that were not read. */
#define JSON_NO_MEMORY (-2L)
long Json_EachElement(const char *path, const JsonDeferred *deferred, int (*each)(const JsonValue *, long, void *),
                      void *context, char *error, size_t error_size);
void Json_Free(JsonDocument *document);
const JsonValue *Json_Root(const JsonDocument *document);

JsonType Json_TypeOf(const JsonValue *value);
/* Members of an object, elements of an array; both 0 for anything else. */
const JsonValue *Json_Member(const JsonValue *object, const char *name);
int Json_Count(const JsonValue *value);
const JsonValue *Json_At(const JsonValue *value, int index);
/* The element or member after this one, NULL after the last. Json_At walks
 * from the start every time, so a loop over a long array goes this way:
 * for (item = Json_At(list, 0); item; item = Json_Next(item)). */
const JsonValue *Json_Next(const JsonValue *value);
const char *Json_Name(const JsonValue *member);

const char *Json_String(const JsonValue *value, const char *fallback);
/* Whole numbers, and strings that hold one ("0x5D800", "-3"; decimal
 * unless written with "0x"). */
long Json_Number(const JsonValue *value, long fallback);
int Json_Bool(const JsonValue *value, int fallback);

#endif
