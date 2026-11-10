/* The manifest reader (json.h). One pass over a private copy of the text:
 * strings are unescaped in place, which only ever shortens them, and values
 * come from arena blocks so that a parsed document's pointers stay put. */
#include "pc/compat/fs.h"
#include "json.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK 32
/* Arrays and objects inside each other: a manifest needs a handful, and the
 * parser recurses once a level, so a file of ten thousand "[" must fail
 * rather than run the stack out. */
#define DEPTH_MAX 64

struct JsonValue {
    JsonType type;
    const char *name;   /* the member name, inside an object */
    char *text;         /* strings and numbers: their text */
    long number;
    int boolean;
    JsonValue *first, *next; /* children, and the next value beside this one */
};

typedef struct Block { struct Block *next; int used; JsonValue values[BLOCK]; } Block;

struct JsonDocument {
    char *text;
    Block *blocks;
    JsonValue *root;
};

typedef struct {
    JsonDocument *document;
    char *at;
    char *error;
    size_t error_size;
    int depth;
    int no_memory;
} Parser;

static void fail(Parser *parser, const char *why)
{
    if (parser->error && parser->error_size && !parser->error[0]) {
        snprintf(parser->error, parser->error_size, "%s", why);
    }
}

static JsonValue *make(Parser *parser)
{
    Block *block = parser->document->blocks;
    if (!block || block->used == BLOCK) {
        block = calloc(1, sizeof(*block));
        if (!block) { fail(parser, "out of memory"); parser->no_memory = 1; return NULL; }
        block->next = parser->document->blocks;
        parser->document->blocks = block;
    }
    {
        JsonValue *value = &block->values[block->used++];
        memset(value, 0, sizeof(*value)); /* blocks are used again (Json_EachElement) */
        return value;
    }
}

static void skip_space(Parser *parser)
{
    while (isspace((unsigned char)*parser->at)) parser->at++;
}

/* Four hex digits: 0, and their value in *code; 1 when they are not. */
static int hex4(const char *at, unsigned *code)
{
    int i;
    *code = 0;
    for (i = 0; i < 4; i++) {
        if (!isxdigit((unsigned char)at[i])) return 1;
        *code = *code * 16 + (unsigned)(isdigit((unsigned char)at[i]) ? at[i] - '0'
                                        : tolower((unsigned char)at[i]) - 'a' + 10);
    }
    return 0;
}

/* A string, unescaped over itself and terminated where its quote was. */
static char *parse_string(Parser *parser)
{
    char *out, *start;
    if (*parser->at != '"') { fail(parser, "expected a string"); return NULL; }
    parser->at++;
    out = start = parser->at;
    while (*parser->at && *parser->at != '"') {
        if (*parser->at != '\\') { *out++ = *parser->at++; continue; }
        parser->at++;
        switch (*parser->at) {
        case 'n': *out++ = '\n'; break;
        case 't': *out++ = '\t'; break;
        case 'r': *out++ = '\r'; break;
        case 'b': *out++ = '\b'; break;
        case 'f': *out++ = '\f'; break;
        case '\\': case '/': case '"': *out++ = *parser->at; break;
        case 'u': {
            /* A character as UTF-8 (a surrogate pair, one past U+FFFF): the
             * texts are UTF-8 (a description's accents), and a tool that
             * writes them escaped ("W\u00e4chter") means the same letters.
             * Never longer than the escape, so it fits where that was. */
            unsigned code = 0, low = 0;
            if (hex4(parser->at + 1, &code)) { fail(parser, "bad \\u escape"); return NULL; }
            parser->at += 4;
            if (code >= 0xD800 && code < 0xDC00 && parser->at[1] == '\\' && parser->at[2] == 'u' &&
                !hex4(parser->at + 3, &low) && low >= 0xDC00 && low < 0xE000) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                parser->at += 6;
            } else if (code >= 0xD800 && code < 0xE000) {
                code = 0xFFFD;      /* a lone surrogate: no character */
            }
            if (code < 0x80) {
                *out++ = (char)code;
            } else if (code < 0x800) {
                *out++ = (char)(0xC0 | code >> 6);
                *out++ = (char)(0x80 | (code & 0x3F));
            } else if (code < 0x10000) {
                *out++ = (char)(0xE0 | code >> 12);
                *out++ = (char)(0x80 | (code >> 6 & 0x3F));
                *out++ = (char)(0x80 | (code & 0x3F));
            } else {
                *out++ = (char)(0xF0 | code >> 18);
                *out++ = (char)(0x80 | (code >> 12 & 0x3F));
                *out++ = (char)(0x80 | (code >> 6 & 0x3F));
                *out++ = (char)(0x80 | (code & 0x3F));
            }
            break;
        }
        default: fail(parser, "bad escape"); return NULL;
        }
        parser->at++;
    }
    if (*parser->at != '"') { fail(parser, "unterminated string"); return NULL; }
    parser->at++;      /* past the closing quote, which the copy may not reach */
    *out = '\0';
    return start;
}

static JsonValue *parse_value(Parser *parser);

static JsonValue *parse_container(Parser *parser, JsonValue *value, char close)
{
    JsonValue *last = NULL;
    parser->at++;
    for (;;) {
        JsonValue *child;
        const char *name = NULL;
        skip_space(parser);
        if (*parser->at == close) { parser->at++; return value; }
        if (last) {
            if (*parser->at != ',') { fail(parser, "expected a comma"); return NULL; }
            parser->at++;
            skip_space(parser);
            if (*parser->at == close) { parser->at++; return value; } /* a trailing comma */
        }
        if (close == '}') {
            name = parse_string(parser);
            if (!name) return NULL;
            skip_space(parser);
            if (*parser->at != ':') { fail(parser, "expected a colon"); return NULL; }
            parser->at++;
        }
        child = parse_value(parser);
        if (!child) return NULL;
        child->name = name;
        if (last) last->next = child;
        else value->first = child;
        last = child;
    }
}

/* A number as JSON writes one, in base 10: "010" and "08" are not numbers,
 * and neither is "1-2". A fraction or an exponent is fine while the value
 * is still whole ("1e3" is 1000, "2.50e1" is 25); anything else, and
 * anything past a long, is an error rather than a quietly different value. */
static int parse_number(Parser *parser, long *out)
{
    const char *at = parser->at, *digits, *fraction = "";
    int negative = 0, whole_count, fraction_count = 0, i, total;
    long exponent = 0, keep;
    unsigned long magnitude = 0, limit;
    if (*at == '-') { negative = 1; at++; }
    if (!isdigit((unsigned char)*at)) { fail(parser, "bad number"); return 0; }
    if (at[0] == '0' && isdigit((unsigned char)at[1])) { fail(parser, "a number may not start with 0"); return 0; }
    digits = at;
    while (isdigit((unsigned char)*at)) at++;
    whole_count = (int)(at - digits);
    if (*at == '.') {
        at++;
        if (!isdigit((unsigned char)*at)) { fail(parser, "bad number"); return 0; }
        fraction = at;
        while (isdigit((unsigned char)*at)) at++;
        fraction_count = (int)(at - fraction);
    }
    if (*at == 'e' || *at == 'E') {
        int minus = 0;
        at++;
        if (*at == '+' || *at == '-') minus = *at++ == '-';
        if (!isdigit((unsigned char)*at)) { fail(parser, "bad number"); return 0; }
        while (isdigit((unsigned char)*at)) {
            if (exponent < 100000) exponent = exponent * 10 + (*at - '0');
            at++;
        }
        if (minus) exponent = -exponent;
    }
    /* The digits, whole part then fraction, are the value times a power of
     * ten; the first `keep` of them are its whole part, and the rest must
     * be zeroes. */
    limit = negative ? (unsigned long)LONG_MAX + 1 : (unsigned long)LONG_MAX;
    total = whole_count + fraction_count;
    keep = (long)whole_count + exponent;
    for (i = 0; i < total; i++) {
        int digit = (i < whole_count ? digits[i] : fraction[i - whole_count]) - '0';
        if (i >= keep) {
            if (digit) { fail(parser, "a number must be whole"); return 0; }
            continue;
        }
        if (magnitude > (limit - (unsigned long)digit) / 10) { fail(parser, "number out of range"); return 0; }
        magnitude = magnitude * 10 + (unsigned long)digit;
    }
    for (; magnitude && i < keep; i++) {
        if (magnitude > limit / 10) { fail(parser, "number out of range"); return 0; }
        magnitude *= 10;
    }
    if (negative) *out = magnitude > (unsigned long)LONG_MAX ? LONG_MIN : -(long)magnitude;
    else *out = (long)magnitude;
    parser->at = (char *)at;
    return 1;
}

static JsonValue *parse_value(Parser *parser)
{
    JsonValue *value;
    skip_space(parser);
    value = make(parser);
    if (!value) return NULL;
    memset(value, 0, sizeof(*value));
    switch (*parser->at) {
    case '{':
    case '[':
        if (parser->depth == DEPTH_MAX) { fail(parser, "nested too deeply"); return NULL; }
        parser->depth++;
        value->type = *parser->at == '{' ? JSON_OBJECT : JSON_ARRAY;
        value = parse_container(parser, value, *parser->at == '{' ? '}' : ']');
        parser->depth--;
        return value;
    case '"':
        value->type = JSON_STRING;
        value->text = parse_string(parser);
        return value->text ? value : NULL;
    default:
        if (!strncmp(parser->at, "true", 4)) { parser->at += 4; value->type = JSON_BOOL; value->boolean = 1; return value; }
        if (!strncmp(parser->at, "false", 5)) { parser->at += 5; value->type = JSON_BOOL; return value; }
        if (!strncmp(parser->at, "null", 4)) { parser->at += 4; value->type = JSON_NULL; return value; }
        if (*parser->at == '-' || isdigit((unsigned char)*parser->at)) {
            value->type = JSON_NUMBER;
            value->text = parser->at;
            return parse_number(parser, &value->number) ? value : NULL;
        }
        fail(parser, *parser->at ? "unexpected character" : "unexpected end of file");
        return NULL;
    }
}

JsonDocument *Json_Parse(const char *text, char *error, size_t error_size)
{
    JsonDocument *document;
    Parser parser;
    if (error && error_size) error[0] = '\0';
    if (!text) return NULL;
    document = calloc(1, sizeof(*document));
    if (!document) return NULL;
    document->text = malloc(strlen(text) + 1);
    if (!document->text) { free(document); return NULL; }
    memcpy(document->text, text, strlen(text) + 1);
    parser.document = document;
    parser.at = document->text;
    /* Windows editors often save UTF-8 with a byte order mark first. */
    if (!strncmp(parser.at, "\xEF\xBB\xBF", 3)) parser.at += 3;
    parser.error = error;
    parser.error_size = error_size;
    parser.depth = 0;
    document->root = parse_value(&parser);
    if (document->root) {
        skip_space(&parser);
        if (*parser.at) { fail(&parser, "trailing text"); document->root = NULL; }
    }
    if (!document->root) { Json_Free(document); return NULL; }
    return document;
}

/* `text` is the caller's and becomes the document's. */
static JsonDocument *parse_owned(char *text, char *error, size_t error_size, int *no_memory)
{
    JsonDocument *document = calloc(1, sizeof(*document));
    Parser parser;
    if (error && error_size) error[0] = '\0';
    if (!document) {
        free(text);
        if (no_memory) *no_memory = 1;
        return NULL;
    }
    document->text = text;
    parser.no_memory = 0;
    parser.document = document;
    parser.at = text;
    if (!strncmp(parser.at, "\xEF\xBB\xBF", 3)) parser.at += 3;
    parser.error = error;
    parser.error_size = error_size;
    parser.depth = 0;
    document->root = parse_value(&parser);
    if (document->root) {
        skip_space(&parser);
        if (*parser.at) { fail(&parser, "trailing text"); document->root = NULL; }
    }
    if (!document->root) {
        if (no_memory && parser.no_memory) *no_memory = 1;
        Json_Free(document);
        return NULL;
    }
    return document;
}

JsonDocument *Json_ParseFile(const char *path, char *error, size_t error_size)
{
    JsonDocument *document;
    char *text;
    long size;
    FILE *file = fopen(path, "rb");
    if (error && error_size) error[0] = '\0';
    if (!file) {
        if (error && error_size) snprintf(error, error_size, "cannot be opened");
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        if (error && error_size) snprintf(error, error_size, "cannot be read");
        return NULL;
    }
    text = malloc((size_t)size + 1);
    if (!text) { fclose(file); return NULL; }
    if (fread(text, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(text);
        if (error && error_size) snprintf(error, error_size, "cannot be read");
        return NULL;
    }
    fclose(file);
    text[size] = '\0';
    document = parse_owned(text, error, error_size, NULL);
    return document;
}

void Json_Free(JsonDocument *document)
{
    Block *block;
    if (!document) return;
    block = document->blocks;
    while (block) {
        Block *next = block->next;
        free(block);
        block = next;
    }
    free(document->text);
    free(document);
}

const JsonValue *Json_Root(const JsonDocument *document) { return document ? document->root : NULL; }
JsonType Json_TypeOf(const JsonValue *value) { return value ? value->type : JSON_NULL; }
const char *Json_Name(const JsonValue *member) { return member ? member->name : NULL; }

const JsonValue *Json_Member(const JsonValue *object, const char *name)
{
    const JsonValue *child;
    if (!object || object->type != JSON_OBJECT || !name) return NULL;
    for (child = object->first; child; child = child->next) {
        if (child->name && !strcmp(child->name, name)) return child;
    }
    return NULL;
}

int Json_Count(const JsonValue *value)
{
    const JsonValue *child;
    int count = 0;
    if (!value || (value->type != JSON_ARRAY && value->type != JSON_OBJECT)) return 0;
    for (child = value->first; child; child = child->next) count++;
    return count;
}

const JsonValue *Json_At(const JsonValue *value, int index)
{
    const JsonValue *child;
    if (!value || index < 0) return NULL;
    for (child = value->first; child; child = child->next) {
        if (!index--) return child;
    }
    return NULL;
}

const JsonValue *Json_Next(const JsonValue *value) { return value ? value->next : NULL; }

const char *Json_String(const JsonValue *value, const char *fallback)
{
    return value && value->type == JSON_STRING ? value->text : fallback;
}

long Json_Number(const JsonValue *value, long fallback)
{
    if (!value) return fallback;
    if (value->type == JSON_NUMBER) return value->number;
    if (value->type == JSON_BOOL) return value->boolean;
    if (value->type == JSON_STRING && value->text) {
        /* Hexadecimal with its "0x", else decimal: a leading 0 is not octal. */
        const char *digits = value->text;
        char *end;
        long parsed;
        while (isspace((unsigned char)*digits)) digits++;
        if (*digits == '-' || *digits == '+') digits++;
        errno = 0;
        parsed = strtol(value->text, &end, digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X') ? 16 : 10);
        while (isspace((unsigned char)*end)) end++;
        if (end != value->text && !*end && !errno) return parsed;
    }
    return fallback;
}

int Json_Bool(const JsonValue *value, int fallback)
{
    if (!value) return fallback;
    if (value->type == JSON_BOOL) return value->boolean;
    if (value->type == JSON_NUMBER) return value->number != 0;
    return fallback;
}

/* --- a member read from the file as it is used ---------------------------- */

#define STREAM_CHUNK (1u << 20)
#define ELEMENT_MAX (1u << 20) /* one element read from the file: a fusion is a hundred bytes */

JsonDocument *Json_ParseFileDeferring(const char *path, const char *const *keys, int count, JsonDeferred *deferred,
                                      char *error, size_t error_size)
{
    FILE *file = fopen(path, "rb");
    unsigned char *chunk = malloc(STREAM_CHUNK);
    char *kept = NULL, name[64];
    size_t kept_used = 0, kept_room = 0, length, name_used = 0;
    long at = 0;
    int depth = 0, in_string = 0, escape = 0, named = 0, unmatched = 0;
    int armed = 0, skipping = 0; /* 1 + the index in `keys` */
    unsigned hash = 2166136261u;
    memset(deferred, 0, (size_t)count * sizeof(*deferred));
    if (error && error_size) error[0] = '\0';
    if (!file || !chunk) {
        if (error && error_size) snprintf(error, error_size, "cannot read %s", path);
        if (file) fclose(file);
        free(chunk);
        return NULL;
    }
    while ((length = fread(chunk, 1, STREAM_CHUNK, file)) > 0) {
        size_t run = 0; /* the bytes of this chunk from here on are kept */
        if (at > LONG_MAX - (long)STREAM_CHUNK) { /* offsets are longs, for fseek */
            fclose(file);
            free(chunk);
            free(kept);
            if (error && error_size) snprintf(error, error_size, "too large");
            return NULL;
        }
        for (size_t i = 0; i < length; i++, at++) {
            unsigned char c = chunk[i];
            if (skipping) {
                hash = (hash ^ c) * 16777619u;
                if (!isspace(c) && c != ']') deferred[skipping - 1].filled = 1;
            }
            if (in_string) {
                if (escape) escape = 0;
                else if (c == '\\') { escape = 1; name_used = sizeof(name); } /* the parser's to read: not the key */
                else if (c == '"') { in_string = 0; named = depth == 1 && !skipping; }
                else if (depth == 1 && !skipping && name_used < sizeof(name)) name[name_used++] = (char)c;
                continue;
            }
            if (c == '"') { in_string = 1; escape = 0; name_used = 0; named = 0; continue; }
            if (c == '{' || c == '[') {
                if (armed && !skipping && depth == 1 && c == '[') {
                    /* What came before stays, and an empty list stands in. */
                    size_t need = i + 1 - run + 2;
                    if (kept_used + need + STREAM_CHUNK + 1 > kept_room) {
                        size_t room = kept_room ? kept_room * 2 : 4u * STREAM_CHUNK;
                        char *bigger = realloc(kept, room);
                        if (!bigger) goto no_room;
                        kept = bigger;
                        kept_room = room;
                    }
                    memcpy(kept + kept_used, chunk + run, i + 1 - run);
                    kept_used += i + 1 - run;
                    kept[kept_used++] = ']';
                    skipping = armed;
                    deferred[skipping - 1].start = at;
                    deferred[skipping - 1].found = 1;
                    hash = (2166136261u ^ c) * 16777619u;
                }
                depth++;
                armed = 0;
                continue;
            }
            if (c == '}' || c == ']') {
                if (--depth == 1 && skipping) {
                    deferred[skipping - 1].end = at;
                    deferred[skipping - 1].hash = hash;
                    skipping = 0;
                    run = i + 1;
                    unmatched |= c != ']';
                }
                armed = 0;
                continue;
            }
            if (c == ':' && depth == 1 && !skipping) {
                armed = 0;
                for (int k = 0; named && k < count; k++)
                    if (!deferred[k].named && name_used == strlen(keys[k]) && !memcmp(name, keys[k], name_used)) {
                        armed = k + 1;
                        deferred[k].named = 1;
                    }
                named = 0;
            } else if (!isspace(c)) {
                armed = 0;
            }
        }
        if (!skipping && length > run) {
            if (kept_used + (length - run) + 1 > kept_room) {
                size_t room = kept_room ? kept_room * 2 : 4u * STREAM_CHUNK;
                char *bigger;
                while (room < kept_used + (length - run) + 1) room *= 2;
                bigger = realloc(kept, room);
                if (!bigger) goto no_room;
                kept = bigger;
                kept_room = room;
            }
            memcpy(kept + kept_used, chunk + run, length - run);
            kept_used += length - run;
        }
    }
    fclose(file);
    free(chunk);
    if (unmatched) {
        free(kept);
        if (error && error_size) snprintf(error, error_size, "a list closed with }");
        return NULL;
    }
    if (skipping) {
        free(kept);
        if (error && error_size) snprintf(error, error_size, "a list that does not end");
        return NULL;
    }
    if (!kept) kept = calloc(1, 1);
    if (!kept) {
        if (error && error_size) snprintf(error, error_size, "out of memory");
        return NULL;
    }
    { /* the room was grown for reading; the manifest keeps only the text */
        char *fit = realloc(kept, kept_used + 1);
        if (fit) kept = fit;
    }
    kept[kept_used] = 0;
    return parse_owned(kept, error, error_size, NULL);
no_room:
    fclose(file);
    free(chunk);
    free(kept);
    if (error && error_size) snprintf(error, error_size, "out of memory");
    return NULL;
}

/* One element: `text` is the caller's buffer, parsed where it is, with the
 * blocks of `document` used again. */
static const JsonValue *parse_element(JsonDocument *document, char *text, char *error, size_t error_size,
                                      int *no_memory)
{
    Parser parser;
    Block *block = document->blocks;
    while (block && block->next) {
        Block *next = block->next;
        free(block);
        block = next;
    }
    document->blocks = block;
    if (block) block->used = 0;
    parser.document = document;
    parser.at = text;
    parser.error = error;
    parser.error_size = error_size;
    parser.depth = 0;
    parser.no_memory = 0;
    if (error && error_size) error[0] = '\0';
    {
        JsonValue *value = parse_value(&parser);
        if (!value) {
            *no_memory = parser.no_memory;
            return NULL;
        }
        skip_space(&parser);
        if (*parser.at) { fail(&parser, "trailing text"); return NULL; }
        return value;
    }
}

/* The first mistake in a list; the rest of it is still read. */
static void list_mistake(int *mistakes, long index, const char *why, char *error, size_t error_size)
{
    if (!(*mistakes)++ && error && error_size) snprintf(error, error_size, "entry %ld: %s", index, why);
}

/* One whole element in `element`: `each` has it, or `error` the mistake. 0 to stop. */
static int read_element(JsonDocument *document, char *element, size_t used, int (*each)(const JsonValue *, long, void *),
                        long *index, void *context, char *error, size_t error_size, int *mistakes, int *no_memory)
{
    char why[64];
    const JsonValue *value;
    element[used] = 0;
    value = parse_element(document, element, why, sizeof(why), no_memory);
    if (value) return each(value, (*index)++, context);
    if (*no_memory) return 0;
    list_mistake(mistakes, *index, why, error, error_size);
    (*index)++;
    return 1;
}

long Json_EachElement(const char *path, const JsonDeferred *deferred, int (*each)(const JsonValue *, long, void *),
                      void *context, char *error, size_t error_size)
{
    FILE *file;
    unsigned char *chunk;
    char *element = NULL;
    size_t used = 0, room = 0;
    long left, index = 0;
    int depth = 0, in_string = 0, escape = 0, inside = 0, good = 1, mistakes = 0;
    int no_memory = 0;
    int after = 0;     /* an element has ended and its comma has not come */
    int too_long = 0;  /* this element is being passed over */
    JsonDocument document;
    if (error && error_size) error[0] = '\0';
    if (!deferred->found) return 0;
    file = fopen(path, "rb");
    chunk = malloc(STREAM_CHUNK);
    memset(&document, 0, sizeof(document));
    if (!file || !chunk || fseek(file, deferred->start + 1, SEEK_SET)) {
        if (error && error_size) snprintf(error, error_size, "cannot read %s", path);
        if (file) fclose(file);
        no_memory = file && !chunk;
        free(chunk);
        return no_memory ? JSON_NO_MEMORY : -1;
    }
    left = deferred->end - deferred->start - 1;
    while (good && left > 0) {
        size_t length = fread(chunk, 1, left < (long)STREAM_CHUNK ? (size_t)left : STREAM_CHUNK, file);
        if (!length) { /* it was shorter than the scan found it */
            list_mistake(&mistakes, index, "the file ended", error, error_size);
            break;
        }
        left -= (long)length;
        for (size_t i = 0; good && i < length; i++) {
            unsigned char c = chunk[i];
            int ends = 0;
            if (!inside) {
                if (isspace(c)) continue;
                if (c == ',') {
                    if (!after) list_mistake(&mistakes, index, "a comma with no entry before it", error, error_size);
                    after = 0;
                    continue;
                }
                if (after) list_mistake(&mistakes, index, "no comma before it", error, error_size);
                after = 0;
                inside = 1;
                depth = 0;
            }
            if (used + 2 > room && !too_long) {
                size_t wanted = room ? room * 2 : 4096;
                char *bigger = NULL;
                if (wanted > ELEMENT_MAX) too_long = 1;
                else if (!(bigger = realloc(element, wanted))) {
                    no_memory = 1;
                    good = 0;
                    break;
                }
                if (bigger) {
                    element = bigger;
                    room = wanted;
                }
            }
            if (in_string) {
                if (!too_long) element[used++] = (char)c;
                if (escape) escape = 0;
                else if (c == '\\') escape = 1;
                else if (c == '"') { in_string = 0; ends = !depth; }
                if (!ends) continue;
                after = 1;
            } else if (!depth && c == ',') {
                ends = 1; /* a number or a word, whole, and its comma */
            } else {
                if (!too_long) element[used++] = (char)c;
                if (c == '"') in_string = 1;
                else if (c == '{' || c == '[') depth++;
                else if (c == '}' || c == ']') ends = after = !--depth;
            }
            if (ends) {
                if (too_long) list_mistake(&mistakes, index++, "over a megabyte", error, error_size);
                else good = read_element(&document, element, used, each, &index, context, error, error_size, &mistakes,
                                         &no_memory);
                used = 0;
                inside = too_long = 0;
            }
        }
    }
    if (good && inside && too_long) list_mistake(&mistakes, index++, "over a megabyte", error, error_size);
    else if (good && inside && used)
        read_element(&document, element, used, each, &index, context, error, error_size, &mistakes, &no_memory);
    while (document.blocks) {
        Block *next = document.blocks->next;
        free(document.blocks);
        document.blocks = next;
    }
    free(element);
    free(chunk);
    fclose(file);
    if (no_memory) {
        if (error && error_size) snprintf(error, error_size, "out of memory");
        return JSON_NO_MEMORY;
    }
    return mistakes ? -1 : index;
}

JsonDocument *Json_ParseDeferred(const char *path, const JsonDeferred *deferred, char *error, size_t error_size,
                                 int *no_memory)
{
    FILE *file = fopen(path, "rb");
    size_t length = (size_t)(deferred->end - deferred->start + 1);
    char *text = malloc(length + 1);
    if (no_memory) *no_memory = 0;
    if (!file || !text || fseek(file, deferred->start, SEEK_SET) || fread(text, 1, length, file) != length) {
        if (no_memory) *no_memory = !text;
        if (file) fclose(file);
        free(text);
        if (error && error_size) snprintf(error, error_size, "cannot read %s", path);
        return NULL;
    }
    fclose(file);
    text[length] = 0;
    return parse_owned(text, error, error_size, no_memory);
}
