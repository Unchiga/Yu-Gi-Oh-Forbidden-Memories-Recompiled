/* A mod refused for lack of memory (mods.c, mod_budget.c): the game restarts once and
 * only when that is on the disk, the mod is off and says why, nothing else is held
 * up, and turning it on again lifts it. Mods_Load scans once, so each case is a process
 * of its own. */
#define main original_mods_test
#include "pc/compat/fs.h"
#include "mods_test.c"
#undef main
#include "pc/mods/mod_budget.h"

static int restarts;
static int fake_restart(void)
{
    restarts++;
    return 0;
}

static int mod_named(const char *id)
{
    for (int i = 0; i < Mods_Count(); i++)
        if (!strcmp(Mods_Id(i), id)) return i;
    assert(!"no such mod");
    return -1;
}

static void setup(const char *user)
{
    char path[1024];
    make_dir("mods");
    make_dir("mods/big");
    write_text("mods/big/mod.json", "{\"id\":\"big\",\"enabled\":true,\"fusions\":[{\"with\":[1,2],\"result\":3}]}");
    make_dir("mods/needs-big");
    write_text("mods/needs-big/mod.json", "{\"id\":\"needs-big\",\"enabled\":true,\"requires\":[\"big\"]}");
    make_dir("mods/small");
    write_text("mods/small/mod.json", "{\"id\":\"small\",\"enabled\":true}");
    snprintf(path, sizeof(path), "%s/mods", root);
    assert(!setenv("MEMORIES_MODS_DIR", path, 1));
    assert(!setenv("MEMORIES_USER_DIR", user, 1));
    snprintf(path, sizeof(path), "%s/settings", root);
    assert(!setenv("MEMORIES_SETTINGS", path, 1));
}

int main(int argc, char **argv)
{
    char listed[1024], error[256];
    int enabled[MODS_MAX] = {0};
    assert(argc == 2 && scratch_dir(root, sizeof(root), "memories-refused"));
    Mods_SetRestart(fake_restart, 7);
    if (!strcmp(argv[1], "budget")) {
        /* The mod too big stays off, and what needs it sets nothing aside. */
        setup(root);
        assert(!setenv("MEMORIES_MOD_BUDGET_MB", "1", 1));
        write_text("mods/big/mod.json", "{\"id\":\"big\",\"enabled\":true,\"pad\":\"");
        {
            char path[1024];
            FILE *file;
            snprintf(path, sizeof(path), "%s/mods/big/mod.json", root);
            assert((file = fopen(path, "ab")) != NULL);
            /* A quarter of it is over the megabyte the mods are given. */
            for (int i = 0; i < 110000; i++) assert(fputs("0123456789012345678901234567890123456789", file) >= 0);
            assert(fputs("\"}", file) >= 0 && !fclose(file));
        }
        Settings_Load();
        Mods_Load();
        assert(Mods_Refused(mod_named("big")) && !Mods_Active(mod_named("big")) && !Mods_Enabled(mod_named("big")));
        assert(strstr(Mods_Status(mod_named("big")), "too big") && !Mods_Failed(mod_named("big")));
        assert(!Mods_Active(mod_named("needs-big")) && Mods_Active(mod_named("small")));
        assert(!restarts);
        /* Neither stops another change; the setting of each stays on. */
        for (int i = 0; i < Mods_Count(); i++) enabled[i] = Mods_Enabled(i);
        assert(Mods_Validate(enabled, error, sizeof(error)));
        assert(Settings_GetNamed("mod.big", 1) == 1 && Settings_GetNamed("mod.needs-big", 1) == 1);
    } else if (!strcmp(argv[1], "restart")) {
        setup(root);
        Settings_Load();
        Mods_Load();
        assert(Mods_Active(mod_named("big")));
        /* The first time it restarts: the fake comes back, as a failed restart would. */
        Mods_RefuseApplying("big", "not enough memory for its fusions");
        assert(restarts == 1 && !Mods_Active(mod_named("big")) && Mods_Refused(mod_named("big")));
        assert(strstr(Mods_Status(mod_named("big")), "not enough memory") && !Mods_Failed(mod_named("big")));
        /* A second one, with the restart still on the disk, does not. */
        Mods_RefuseApplying("small", "not enough memory for its fusions");
        assert(restarts == 1 && !Mods_Active(mod_named("small")));
        snprintf(listed, sizeof(listed), "%s/refused-mods.txt", root);
        assert(ModBudget_Restarted(listed));
        Mods_Applied();
        assert(!ModBudget_Restarted(listed));
        /* A refused mod stops no other change. */
        for (int i = 0; i < Mods_Count(); i++) enabled[i] = Mods_Enabled(i);
        assert(!enabled[mod_named("big")]);
        assert(Mods_Validate(enabled, error, sizeof(error)));
        /* Turned on again it is tried at the next start: the line goes. */
        {
            FILE *file;
            char text[2048] = "";
            Mods_SetEnabled(mod_named("big"), 1);
            assert(!Mods_Refused(mod_named("big")));
            assert((file = fopen(listed, "rb")) != NULL);
            text[fread(text, 1, sizeof(text) - 1, file)] = 0;
            fclose(file);
            assert(!strstr(text, "big\t") && strstr(text, "small\t"));
        }
    } else if (!strcmp(argv[1], "unwritable")) {
        /* A full disk, a folder it may not write: no line, no restart, however often. */
        make_dir("refused-mods.txt.tmp");
        setup(root);
        Settings_Load();
        Mods_Load();
        Mods_RefuseApplying("big", "not enough memory for its fusions");
        Mods_RefuseApplying("small", "not enough memory for its fusions");
        assert(!restarts && !Mods_Active(mod_named("big")) && !Mods_Active(mod_named("small")));
        assert(Mods_Refused(mod_named("big")));
    } else {
        return 2;
    }
    printf("mods refused (%s): ok\n", argv[1]);
    return 0;
}
