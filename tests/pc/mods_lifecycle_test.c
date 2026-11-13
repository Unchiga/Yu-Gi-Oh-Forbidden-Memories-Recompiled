#define main original_mods_test
#include "pc/compat/fs.h"
#include "mods_test.c"
#undef main
#include "pc/mods/events.h"
#include "pc/mods/model_rows.h"
static void unexpected_state(int owner, void *data, size_t size, unsigned version, void *context)
{
    (void)owner;
    (void)data;
    (void)size;
    (void)version;
    (void)context;
    assert(0);
}
/* A fixture object, copied into a mod directory. */
static void install(const char *from, const char *to)
{
    FILE *file;
    long length;
    void *object;
    file = fopen(from, "rb");
    assert(file);
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    rewind(file);
    object = malloc((size_t)length);
    assert(object);
    assert(fread(object, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    write_file(to, object, (size_t)length);
    free(object);
}
/* The row tests/pc/mod_fixtures/model_rows.c registers: its module's 8 bytes. */
static const ModelGateRow *fixture_row(void)
{
    static const unsigned char module[8] = {0xEE, 0xFF, 0xC0, 0x00, 0x08, 0x00, 0xE0, 0x03};
    return ModelRows_Find(0, 0, 0x00C0FFEEu, module, 0, NULL);
}

/* A code mod's control-module rows (mod API 29) through the loader. */
static void model_rows(void)
{
    int ok = find("rows-ok"), refused = find("rows-refused"), bad = find("rows-bad");
    const ModelGateRow *row;
    assert(ok >= 0 && refused >= 0 && bad >= 0);
    /* Registered and applied: found, with its owner and token. */
    assert(Mods_Active(ok) && Settings_GetNamed("mod.rows-ok.token", 0) > 0);
    row = fixture_row();
    assert(row && row->owner == ok && row->token == Settings_GetNamed("mod.rows-ok.token", 0));
    assert(row->entry((void *)0, 7) == 7);
    /* model_module_base is 0 outside an entry; a module-range data name the
     * game does not export is bound to its address. */
    assert(Settings_GetNamed("mod.rows-ok.base_outside", 0) == 1);
    assert(Settings_GetNamed("mod.rows-ok.module_data", 0) == 1);
    /* Removed: still registered, never found; applied again: found. */
    Mods_SetEnabled(ok, 0);
    assert(!Mods_Active(ok) && !fixture_row() && ModelRows_Count() == 1);
    Mods_SetEnabled(ok, 1);
    assert(Mods_Active(ok) && fixture_row() && fixture_row()->owner == ok);
    /* A mod that refused to start leaves no row. */
    assert(Mods_Failed(refused) && !Mods_Active(refused) && Settings_GetNamed("mod.rows-refused.token", 0) > 0);
    /* A bad row: token 0 and the reason in the mod's status; the mod starts. */
    assert(Mods_Active(bad) && Settings_GetNamed("mod.rows-bad.token", -1) == 0);
    assert(strstr(Mods_Status(bad), "model_module: sha256 is not 64 hex digits"));
    assert(ModelRows_Count() == 1);
}

int main(int argc, char **argv)
{
    char path[1024];
    int no_api;
    assert(argc == 3 || argc == 4);
    assert(scratch_dir(root, sizeof(root), "memories-lifecycle"));
    make_dir("mods");
    make_dir("mods/reject");
    install(argv[1], "mods/reject/reject.o");
    write_text("mods/reject/mod.json", "{\"id\":\"reject\",\"library\":\"reject\",\"enabled\":true}");
    make_dir("mods/zz-no-api");
    install(argv[2], "mods/zz-no-api/no-api.o");
    write_text("mods/zz-no-api/mod.json", "{\"id\":\"no-api\",\"library\":\"no-api\",\"enabled\":true}");
    if (argc == 4) {
        static const char *const ids[3] = {"rows-ok", "rows-refused", "rows-bad"};
        for (int i = 0; i < 3; i++) {
            char directory[64], object[96], manifest[160];
            snprintf(directory, sizeof(directory), "mods/zz-%s", ids[i]);
            snprintf(object, sizeof(object), "%s/rows.o", directory);
            snprintf(manifest, sizeof(manifest),
                     "{\"id\":\"%s\",\"library\":\"rows\",\"enabled\":true,\"restart\":false}", ids[i]);
            make_dir(directory);
            install(argv[3], object);
            snprintf(object, sizeof(object), "%s/mod.json", directory);
            write_text(object, manifest);
        }
    }
    snprintf(path, sizeof(path), "%s/mods", root);
    setenv("MEMORIES_MODS_DIR", path, 1);
    snprintf(path, sizeof(path), "%s/settings.txt", root);
    setenv("MEMORIES_SETTINGS", path, 1);
    Settings_Load();
    Mods_Load();
    assert(find("reject") == 0);
    assert(Mods_Failed(0) && !Mods_Active(0));
    /* A mod that leaves mod->api at 0 is refused with that reason. */
    no_api = find("no-api");
    assert(no_api == 1 && Mods_Failed(no_api) && !Mods_Active(no_api));
    assert(strstr(Mods_Status(no_api), "did not set mod->api"));
    if (argc == 4) model_rows();
    Mods_DrawFrame();
    Mods_Reset();
    Mods_Notify(MEMORIES_EVENT_DAMAGE, 0, 100, 0);
    Mods_VisitState(unexpected_state, NULL);
    Mods_Shutdown();
    assert(!Settings_GetNamed("mod.reject.unexpected_callback", 0));
    Mods_SetEnabled(0, 0);
    Mods_SetEnabled(0, 1);
    Mods_DrawFrame();
    assert(!Settings_GetNamed("mod.reject.unexpected_callback", 0));
    return 0;
}
