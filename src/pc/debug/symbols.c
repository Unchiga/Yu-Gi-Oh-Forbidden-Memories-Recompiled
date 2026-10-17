#include "pc/compat/fs.h"
#include "symbols.h"
#include "pc/guest/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct RuntimeSymbol {
    uintptr_t address, size;
    char name[72];
    int pinned;      /* a variable: at its retail guest address, not in this process */
    int read_only;   /* a variable: a constant of the build, in a read-only page */
} RuntimeSymbol;

/* One "address size name [pinned|ro]" line of a table. 1 with the entry, 0 at
 * the end of the file, -1 for a line to skip: a name too long for the entry
 * (%71s used to cut it and read the rest as the next line) or a line that does
 * not fit the buffer, whose rest is dropped. */
static int read_entry(FILE *file, RuntimeSymbol *symbol)
{
    char line[512], name[256], flag[16];
    unsigned address, size;
    int fields;
    if (!fgets(line, sizeof(line), file)) return 0;
    if (!strchr(line, '\n') && !feof(file)) {
        int c;
        while ((c = fgetc(file)) != EOF && c != '\n') {}
        return -1;
    }
    flag[0] = '\0';
    fields = sscanf(line, "%x %x %255s %15s", &address, &size, name, flag);
    if (fields < 3 || strlen(name) >= sizeof(symbol->name)) return -1;
    symbol->address = address;
    symbol->size = size;
    symbol->pinned = fields == 4 && !strcmp(flag, "pinned");
    symbol->read_only = fields == 4 && !strcmp(flag, "ro");
    strcpy(symbol->name, name);
    return 1;
}

static RuntimeSymbol *symbols;
static size_t symbol_count;
static RuntimeSymbol *variables;   /* by address, like symbols: the game's data and bss */
static size_t variable_count;

/* symbols/<id>.vars.txt beside the table at `table`: "address size name" for
 * a variable the build defines ("address size name ro" in read-only data), "address 0 name pinned" for one it pins to its
 * retail guest address. */
static void load_variables(const char *table)
{
    char path[660];
    FILE *file;
    size_t length = strlen(table), room = 0;
    if (length < 4 || length + 6 >= sizeof(path)) return;
    snprintf(path, sizeof(path), "%.*s.vars.txt", (int)(length - 4), table);
    file = fopen(path, "r");
    if (!file) return;
    for (;;) {
        RuntimeSymbol symbol;
        int read = read_entry(file, &symbol);
        if (!read) break;
        if (read < 0) continue;
        if (variable_count == room) {
            RuntimeSymbol *grown;
            room = room ? room * 2 : 1024;
            grown = realloc(variables, room * sizeof(*variables));
            if (!grown) {
                free(variables);
                variables = NULL;
                variable_count = 0;
                break;
            }
            variables = grown;
        }
        variables[variable_count++] = symbol;
    }
    fclose(file);
}

int Symbols_FindVariable(const char *name, uintptr_t *address, uintptr_t *size, int *read_only)
{
    size_t at;
    if (!variable_count) return -2;
    for (at = 0; at < variable_count; at++) {
        if (!strcmp(variables[at].name, name)) {
            *address = variables[at].address;
            *size = variables[at].size;
            if (read_only) *read_only = variables[at].read_only;
            return variables[at].pinned;
        }
    }
    return -1;
}

int Symbols_Load(void)
{
    char path[640];
    FILE *file;
    size_t room = 0;
    if (Memories_SymbolTablePath(path, sizeof(path))) return -1;
    load_variables(path);
    file = fopen(path, "r");
    if (!file) return -1;
    for (;;) {
        RuntimeSymbol symbol;
        int read = read_entry(file, &symbol);
        if (!read) break;
        if (read < 0) continue;
        if (symbol_count == room) {
            RuntimeSymbol *grown;
            room = room ? room * 2 : 2048;
            grown = realloc(symbols, room * sizeof(*symbols));
            if (!grown) {
                fclose(file);
                free(symbols);
                symbols = NULL;
                symbol_count = 0;
                return -1;
            }
            symbols = grown;
        }
        symbols[symbol_count++] = symbol;
    }
    fclose(file);
    return symbol_count ? 0 : -1;
}

const char *Symbols_Lookup(uintptr_t address, uintptr_t *offset)
{
    size_t low = 0, high = symbol_count;
    while (low < high) {
        size_t middle = (low + high) / 2;
        if (symbols[middle].address <= address) low = middle + 1;
        else high = middle;
    }
    if (!low) return NULL;
    low--;
    if (address >= symbols[low].address + (symbols[low].size ? symbols[low].size : 1)) return NULL;
    if (offset) *offset = address - symbols[low].address;
    return symbols[low].name;
}

/* The hang reporter reads the table from its own thread, so it is never
 * changed in place: a merged copy is built and published before its count,
 * and the old one is left alone (once per mod, so a handful at most). */
int Symbols_Add(const SymbolsEntry *entries, size_t count)
{
    RuntimeSymbol *merged = malloc((symbol_count + count) * sizeof(*merged));
    size_t i, at = 0, total;
    if (!merged) return -1;
    if (symbol_count) memcpy(merged, symbols, symbol_count * sizeof(*merged));
    total = symbol_count;
    for (i = 0; i < count; i++) {   /* an insertion into sorted order: few entries */
        RuntimeSymbol symbol;
        symbol.address = entries[i].address;
        symbol.size = entries[i].size;
        symbol.pinned = 0;
        symbol.read_only = 0;
        snprintf(symbol.name, sizeof(symbol.name), "%s", entries[i].name);
        at = total;
        while (at > 0 && merged[at - 1].address > symbol.address) {
            merged[at] = merged[at - 1];
            at--;
        }
        merged[at] = symbol;
        total++;
    }
    symbols = merged;
    __asm__ volatile("" ::: "memory");
    symbol_count = total;
    return 0;
}
