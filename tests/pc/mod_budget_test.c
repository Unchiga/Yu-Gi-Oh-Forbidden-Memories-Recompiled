/* What a mod may take, and the list of those refused (src/pc/mods/mod_budget.c). */
#define _POSIX_C_SOURCE 200809L
#include "pc/compat/fs.h"
#include "pc/mods/mod_budget.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    const char *path = "mod_budget_test.txt";
    char why[256] = "";
    remove(path);
    /* What the file is worth; the budget by hand. */
    assert(ModBudget_Need(4ll << 30) == 1ll << 30);
    assert(ModBudget_Bytes() > 0);
    assert(!setenv("MEMORIES_MOD_BUDGET_MB", "50", 1));
    assert(ModBudget_Bytes() == 50ll << 20);
    /* A value that makes no sense is no budget: nothing is refused for it. */
    {
        static const char *const bad[] = {"0", "-5", "lots", "12x", " "};
        long long worked_out;
        assert(!setenv("MEMORIES_MOD_BUDGET_MB", "", 1));
        worked_out = ModBudget_Bytes();
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            assert(!setenv("MEMORIES_MOD_BUDGET_MB", bad[i], 1));
            assert(ModBudget_Bytes() == worked_out);
        }
    }
    /* Nowhere to write: each says so, and no restart reads back as set. */
    {
        const char *nowhere = "no-such-folder/refused.txt";
        assert(!ModBudget_Refuse(nowhere, "big", 1, 2, 3, "why"));
        assert(!ModBudget_SetRestarted(nowhere, 1));
        assert(!ModBudget_Restarted(nowhere));
    }
    /* No file: nothing refused, no restart. */
    assert(!ModBudget_Refused(path, "big", 10, 20, 30, why, sizeof(why)));
    assert(!ModBudget_Restarted(path));
    /* A line holds for that file and that build. */
    ModBudget_Refuse(path, "big", 10, 20, 30, "not enough memory\nfor it");
    assert(ModBudget_Refused(path, "big", 10, 20, 30, why, sizeof(why)) && !strcmp(why, "not enough memory"));
    assert(!ModBudget_Refused(path, "big", 11, 20, 30, why, sizeof(why)));
    assert(!ModBudget_Refused(path, "big", 10, 21, 30, why, sizeof(why)));
    assert(!ModBudget_Refused(path, "big", 10, 20, 31, why, sizeof(why)));
    assert(!ModBudget_Refused(path, "bi", 10, 20, 30, why, sizeof(why)));
    /* Another mod, and the same one anew: one line each. */
    ModBudget_Refuse(path, "bigger", 1, 2, 3, "too");
    ModBudget_Refuse(path, "big", 12, 20, 30, "again");
    assert(ModBudget_Refused(path, "bigger", 1, 2, 3, why, sizeof(why)) && !strcmp(why, "too"));
    assert(ModBudget_Refused(path, "big", 12, 20, 30, why, sizeof(why)) && !strcmp(why, "again"));
    assert(!ModBudget_Refused(path, "big", 10, 20, 30, why, sizeof(why)));
    /* A last line cut short does not swallow the next. */
    {
        FILE *file = fopen(path, "ab");
        assert(file && fputs("cut\t1\t2\t3\tshort", file) >= 0 && !fclose(file));
        assert(ModBudget_Refuse(path, "after", 4, 5, 6, "fine"));
        assert(ModBudget_Refused(path, "after", 4, 5, 6, why, sizeof(why)) && !strcmp(why, "fine"));
        assert(ModBudget_Refused(path, "cut", 1, 2, 3, why, sizeof(why)) && !strcmp(why, "short"));
    }
    /* The player turns one on again. */
    assert(ModBudget_Forget(path, "after"));
    assert(!ModBudget_Refused(path, "after", 4, 5, 6, why, sizeof(why)));
    assert(ModBudget_Refused(path, "bigger", 1, 2, 3, why, sizeof(why)));
    /* A restart once, whatever else the file has. */
    assert(ModBudget_SetRestarted(path, 1));
    assert(ModBudget_Restarted(path));
    assert(ModBudget_Refused(path, "bigger", 1, 2, 3, why, sizeof(why)));
    ModBudget_SetRestarted(path, 0);
    assert(!ModBudget_Restarted(path));
    assert(ModBudget_Refused(path, "bigger", 1, 2, 3, why, sizeof(why)));
    remove(path);
    puts("Mod budget: refusals by file and build");
    return 0;
}
