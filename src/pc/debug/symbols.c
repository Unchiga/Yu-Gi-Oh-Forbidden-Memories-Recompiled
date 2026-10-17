#include "pc/compat/fs.h"
#include "symbols.h"
#include "pc/guest/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef MEMORIES_TRANSLATED
#include "pc/guest/translated_state_symbols.h"
#endif

typedef struct RuntimeSymbol {
    uintptr_t address, size;
    char name[72];
} RuntimeSymbol;

static RuntimeSymbol *symbols;
static size_t symbol_count;

int Symbols_Load(void)
{
    char path[640];
    FILE *file;
    size_t room = 0;
    uintptr_t base = 0;
    if (Memories_SymbolTablePath(path, sizeof(path))) return -1;
#ifdef MEMORIES_TRANSLATED
    /* The translated game's table holds offsets into its image
     * (state_translated.c): add where it is loaded. */
    base = Memories_NativeImageBase();
    if (!base) return -1;
#endif
    file = fopen(path, "r");
    if (!file) return -1;
    for (;;) {
        RuntimeSymbol symbol;
        unsigned long long address, size;
        char line[1100], kind = 'F';
        if (!fgets(line, sizeof(line), file)) break;
        if (line[0] == '#') continue;   /* the translated game's table, version 2 (tools/pc/translated_symbols.py) */
        if (sscanf(line, "%llx %llx %c %*s %*s %*s %71s", &address, &size, &kind, symbol.name) != 4 &&
            sscanf(line, "%llx %llx %71s", &address, &size, symbol.name) != 3) {
            break;
        }
        if (kind == 'S' || kind == 'X') continue;   /* a section's start, not a symbol */
        symbol.address = base + (uintptr_t)address;
        symbol.size = (uintptr_t)size;
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
