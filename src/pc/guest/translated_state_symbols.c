/* Save states across builds of the translated game (the Android app's).
 * libgame.so is linked at 0; its symbol table, version 2
 * (tools/pc/translated_symbols.py), lists every function, data object and
 * section start by offset, with a hash of its content and of the gap after
 * it (position-dependent instruction fields and dynamically relocated
 * words left out), and with statics qualified by their translation unit.
 * A state carries its build's table ("arm64-symbols"). A word of a state
 * of another build that points into the old image is carried over only
 * when the result is certain, and the load is refused otherwise, before
 * anything is applied (Memories_NativeSymbolsScan, and the same
 * Memories_NativeSymbolsMap when applying):
 *
 * - the exact start of a function: the function of the same name;
 * - the start or the inside of a data object: only when the object has the
 *   same size and content in the new build;
 * - the inside of a function (a return address): only on the native stack,
 *   and only when the function has the same size and content;
 * - the gap after a data object or at a data section's start (string
 *   pools): only when the object, the gap's size and its bytes are the
 *   same;
 * - nothing else: a name that is not unique in either table, aliases at
 *   one address that would go to different places, padding and veneers in
 *   code, an offset past the old image or a result past the new one.
 *
 * As desktop v0.3.0's state.c, this trusts that a function of the same
 * name is the function the state meant (its address is what the game
 * stored); unlike it, an interior is never trusted on size alone. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* Dl_info, dladdr (glibc) */
#endif
#include "translated_state_symbols.h"
#include "pc/compat/fs.h"   /* fopen through the UTF-8 file boundary (Windows) */
#include "state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif

#define TABLE_HEADER "# memories-symbols 2 "
#define LINE_MAX_ 1024

typedef struct TableSymbol {
    uint64_t start, size, gap;
    char kind;
    char hash[17], gap_hash[17];
    char *name;
} TableSymbol;

typedef struct Table {
    TableSymbol *symbols;   /* by offset */
    unsigned *by_name;      /* indexes, by name */
    size_t count;
    uint64_t image_size;
    char *names;
} Table;

static Table old_table, new_table;
static int active;

static const Table *sorting;
static int compare_names(const void *left, const void *right)
{
    return strcmp(sorting->symbols[*(const unsigned *)left].name, sorting->symbols[*(const unsigned *)right].name);
}

static void clear(Table *table)
{
    free(table->symbols);
    free(table->by_name);
    free(table->names);
    memset(table, 0, sizeof(*table));
}

/* The whole table, or -1 (and nothing kept) for anything not exactly this
 * format: a line too long to read whole, a field missing, rows out of
 * order or past the image. */
static int parse(Table *table, const char *text, size_t length, uint64_t image_size)
{
    size_t at = 0, room = 0, name_room = 0, name_used = 0;
    char line[LINE_MAX_];
    clear(table);
    table->image_size = image_size;
    if (length < sizeof(TABLE_HEADER) - 1 || memcmp(text, TABLE_HEADER, sizeof(TABLE_HEADER) - 1)) return -1;
    while (at < length && text[at] != '\n') at++;
    at++;
    while (at < length) {
        size_t end = at;
        unsigned long long start, size, gap;
        char kind, hash[17], gap_hash[17], name[LINE_MAX_];
        int used = 0;
        TableSymbol *symbol;
        while (end < length && text[end] != '\n') end++;
        if (end - at >= sizeof(line)) goto bad;
        memcpy(line, text + at, end - at);
        line[end - at] = '\0';
        at = end + 1;
        if (!line[0]) continue;
        if (sscanf(line, "%llx %llx %c %16s %llx %16s %1023s%n", &start, &size, &kind, hash, &gap, gap_hash, name,
                   &used) != 7 || line[used] || strlen(hash) != 16 || strlen(gap_hash) != 16 ||
            !strchr("FDSX", kind) || start > image_size || size > image_size - start ||
            gap > image_size - start - size || (table->count && start < table->symbols[table->count - 1].start)) {
            goto bad;
        }
        if (table->count == room) {
            TableSymbol *grown;
            room = room ? room * 2 : 4096;
            grown = realloc(table->symbols, room * sizeof(*grown));
            if (!grown) goto bad;
            table->symbols = grown;
        }
        if (name_used + strlen(name) + 1 > name_room) {
            char *grown;
            size_t i;
            name_room = (name_room ? name_room * 2 : 1u << 18) + strlen(name) + 1;
            grown = realloc(table->names, name_room);
            if (!grown) goto bad;
            for (i = 0; i < table->count; i++) table->symbols[i].name = grown + (table->symbols[i].name - table->names);
            table->names = grown;
        }
        symbol = &table->symbols[table->count++];
        symbol->start = start;
        symbol->size = size;
        symbol->gap = gap;
        symbol->kind = kind;
        memcpy(symbol->hash, hash, 17);
        memcpy(symbol->gap_hash, gap_hash, 17);
        symbol->name = table->names + name_used;
        memcpy(symbol->name, name, strlen(name) + 1);
        name_used += strlen(name) + 1;
    }
    if (!table->count || !(table->by_name = malloc(table->count * sizeof(*table->by_name)))) goto bad;
    for (size_t i = 0; i < table->count; i++) table->by_name[i] = (unsigned)i;
    sorting = table;
    qsort(table->by_name, table->count, sizeof(*table->by_name), compare_names);
    return 0;
bad:
    clear(table);
    return -1;
}

/* The one row of this name, or NULL when there is none or more than one. */
static const TableSymbol *unique(const Table *table, const char *name)
{
    size_t low = 0, high = table->count;
    while (low < high) {
        size_t middle = (low + high) / 2;
        int order = strcmp(table->symbols[table->by_name[middle]].name, name);
        if (!order) {
            if ((middle && !strcmp(table->symbols[table->by_name[middle - 1]].name, name)) ||
                (middle + 1 < table->count && !strcmp(table->symbols[table->by_name[middle + 1]].name, name)))
                return NULL;
            return &table->symbols[table->by_name[middle]];
        }
        if (order < 0) low = middle + 1;
        else high = middle;
    }
    return NULL;
}

uintptr_t Memories_NativeImageBase(void)
{
#ifndef _WIN32
    static const char anchor = 0;   /* a data address: no function pointer cast (ISO C) */
    Dl_info info;
    if (dladdr(&anchor, &info) && info.dli_fbase) return (uintptr_t)info.dli_fbase;
#endif
    return 0;
}

char *Memories_NativeSymbolsText(size_t *length)
{
    char path[640];
    FILE *file;
    long size;
    char *text = NULL;
    *length = 0;
    if (Memories_SymbolTablePath(path, sizeof(path)) || !(file = fopen(path, "rb"))) return NULL;
    if (!fseek(file, 0, SEEK_END) && (size = ftell(file)) > 0 && !fseek(file, 0, SEEK_SET) &&
        (text = malloc((size_t)size)) && fread(text, 1, (size_t)size, file) == (size_t)size) {
        *length = (size_t)size;
    } else {
        free(text);
        text = NULL;
    }
    fclose(file);
    return text;
}

int Memories_NativeSymbolsPrepareTables(const void *saved, size_t saved_length, uint64_t old_image_size,
                                        const char *current, size_t current_length, uint64_t new_image_size)
{
    Memories_NativeSymbolsClear();
    if (!saved || !current || parse(&old_table, saved, saved_length, old_image_size) ||
        parse(&new_table, current, current_length, new_image_size)) {
        Memories_NativeSymbolsClear();
        return -1;
    }
    active = 1;
    return 0;
}

int Memories_NativeSymbolsPrepare(const void *saved, size_t saved_length, uint64_t old_image_size,
                                  uint64_t new_image_size)
{
    size_t length;
    char *text = Memories_NativeSymbolsText(&length);
    int result = Memories_NativeSymbolsPrepareTables(saved, saved_length, old_image_size, text, length,
                                                     new_image_size);
    free(text);
    return result;
}

int Memories_NativeSymbolsActive(void) { return active; }

void Memories_NativeSymbolsClear(void)
{
    clear(&old_table);
    clear(&new_table);
    active = 0;
}

static int same_content(const TableSymbol *old, const TableSymbol *now)
{
    return old->size == now->size && !strcmp(old->hash, now->hash);
}

/* `offset` by one old row starting at or before it: 0 and the new offset,
 * or -1. */
static int map_row(const TableSymbol *old, uint64_t offset, int stack, uint64_t *mapped)
{
    const TableSymbol *now = unique(&new_table, old->name);
    uint64_t delta;
    if (!unique(&old_table, old->name) || !now || now->kind != old->kind) return -1;
    if (offset < old->start + old->size) {
        delta = offset - old->start;
        if (old->kind == 'F') {
            if (delta && (!stack || !same_content(old, now))) return -1;
        } else if (!same_content(old, now)) {
            return -1;
        }
        *mapped = now->start + delta;
        return 0;
    }
    delta = offset - old->start - old->size;
    if (delta >= old->gap || (old->kind != 'D' && old->kind != 'S') || !same_content(old, now) ||
        old->gap != now->gap || strcmp(old->gap_hash, now->gap_hash)) {
        return -1;
    }
    *mapped = now->start + now->size + delta;
    return 0;
}

int Memories_NativeSymbolsMap(uint64_t offset, int stack, uint64_t *mapped)
{
    size_t low = 0, high = old_table.count, first, i;
    uint64_t start, result = 0;
    int found = 0;
    if (!active || offset >= old_table.image_size) return -1;
    while (low < high) {
        size_t middle = (low + high) / 2;
        if (old_table.symbols[middle].start <= offset) low = middle + 1;
        else high = middle;
    }
    if (!low) return -1;
    start = old_table.symbols[low - 1].start;
    for (first = low - 1; first && old_table.symbols[first - 1].start == start; first--) {}
    /* Every row at that address (aliases, a section's start) whose span or
     * gap holds the offset must agree. */
    for (i = first; i < low; i++) {
        const TableSymbol *old = &old_table.symbols[i];
        uint64_t one;
        if (offset >= old->start + old->size + old->gap) continue;
        if (map_row(old, offset, stack, &one) || (found && one != result)) return -1;
        result = one;
        found = 1;
    }
    if (!found || result >= new_table.image_size) return -1;
    *mapped = result;
    return 0;
}

size_t Memories_NativeSymbolsScan(const uint8_t *data, size_t size, uint64_t old_image, int stack, int aligned)
{
    size_t missing = 0;
    for (size_t at = 0; at + sizeof(uint64_t) <= size; at += aligned ? sizeof(uint64_t) : 1) {
        uint64_t word, mapped;
        memcpy(&word, data + at, sizeof(word));
        if (word >= old_image && word - old_image < old_table.image_size &&
            Memories_NativeSymbolsMap(word - old_image, stack, &mapped))
            missing++;
    }
    return missing;
}

uint64_t Memories_NativeSymbolsOldSize(void) { return old_table.image_size; }
