/* The symbol tables the game reads (src/pc/debug/symbols.c): the one of
 * functions for crash reports and the variable table
 * (symbols/<id>.vars.txt) the control channel names variables from. A name
 * too long for an entry, or a line too long for the reader, is skipped and
 * does not turn the rest of the line into the next entry. */
#include "pc/debug/symbols.h"
#include "pc/guest/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #condition); \
            exit(1); \
        } \
    } while (0)

#define TABLE "symbols_test_table.txt"
#define VARIABLES "symbols_test_table.vars.txt"

int Memories_SymbolTablePath(char *out, size_t size)
{
    snprintf(out, size, "%s", TABLE);
    return 0;
}

static void write(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    CHECK(file && fputs(text, file) >= 0);
    CHECK(fclose(file) == 0);
}

int main(void)
{
    static char text[2048];
    uintptr_t address, size, offset;
    int ro = 0;
    size_t at;
    CHECK(Symbols_FindVariable("gVar", &address, &size, &ro) == -2);   /* nothing loaded: no table at all */

    write(TABLE, "01000000 00000010 func\n"
                 "01000020 00000004 gone_for_a_long_name_the_entry_cannot_hold_gone_for_a_long_name_the_entry_cannot_hold 01000040 4 after\n"
                 "01000100 00000004 next\n");
    at = (size_t)snprintf(text, sizeof(text), "02000000 00000004 gVar\n"
                          "02000010 00000002 gVar\n"
                          "8009b26c 00000000 D_8009B26C pinned\n"
                          "02000050 00000004 gConstant ro\n"
                          "02000020 00000008 gLongLine_");
    memset(text + at, 'x', 600);   /* a line that does not fit the reader: dropped whole */
    at += 600;
    snprintf(text + at, sizeof(text) - at, " 02000099 4 gHidden\n"
             "02000030 00000001 gAfterLongLine\n"
             "02000040 00000001 gNoNewlineAtTheEnd");
    write(VARIABLES, text);

    CHECK(Symbols_Load() == 0);
    CHECK(!strcmp(Symbols_Lookup(0x01000004, &offset), "func") && offset == 4);
    CHECK(Symbols_Lookup(0x01000020, &offset) == NULL);   /* the long name is gone, not cut */
    CHECK(Symbols_Lookup(0x01000040, &offset) == NULL);   /* and its tail is not an entry */
    CHECK(!strcmp(Symbols_Lookup(0x01000100, &offset), "next"));

    CHECK(Symbols_FindVariable("gVar", &address, &size, &ro) == 0 && address == 0x02000000 && size == 4);
    CHECK(Symbols_FindVariable("D_8009B26C", &address, &size, &ro) == 1 && address == 0x8009B26C);
    CHECK(Symbols_FindVariable("gAfterLongLine", &address, &size, &ro) == 0 && address == 0x02000030);
    CHECK(Symbols_FindVariable("gNoNewlineAtTheEnd", &address, &size, &ro) == 0 && address == 0x02000040);
    CHECK(Symbols_FindVariable("gConstant", &address, &size, &ro) == 0 && ro == 1 && address == 0x02000050);
    CHECK(Symbols_FindVariable("gVar", &address, &size, &ro) == 0 && ro == 0);
    CHECK(Symbols_FindVariable("gHidden", &address, &size, &ro) == -1);
    CHECK(Symbols_FindVariable("gNoSuchVariable", &address, &size, &ro) == -1);
    remove(TABLE);
    remove(VARIABLES);
    printf("symbols: all passed\n");
    return 0;
}
