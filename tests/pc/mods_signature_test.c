/* A save state is held to Mods_Signature, so the signature of a mod with no fusions
 * must not move with how the fusions are read: the number below is what it was when
 * the whole manifest was parsed and hashed. */
#define main original_mods_test
#include "pc/compat/fs.h"
#include "mods_test.c"
#undef main

#define PLAIN_SIGNATURE 0x90522D95u

int main(void)
{
    unsigned signature;
    assert(scratch_dir(root, sizeof(root), "memories-signature"));
    make_dir("mods");
    make_dir("mods/plain");
    write_text("mods/plain/mod.json", "{\"id\":\"plain\",\"name\":\"Plain\",\"version\":\"1.0\",\"enabled\":true,"
                                      "\"description\":\"no fusions, no cards\"}");
    make_dir("mods/empty");
    write_text("mods/empty/mod.json", "{\"id\":\"empty\",\"enabled\":true,\"fusions\":[]}");
    {
        char path[1024];
        snprintf(path, sizeof(path), "%s/mods", root);
        assert(!setenv("MEMORIES_MODS_DIR", path, 1));
        assert(!setenv("MEMORIES_USER_DIR", root, 1));
        snprintf(path, sizeof(path), "%s/settings", root);
        assert(!setenv("MEMORIES_SETTINGS", path, 1));
    }
    Settings_Load();
    Mods_Load();
    assert(Mods_Active(0) && Mods_Active(1));
    signature = Mods_Signature();
    printf("signature 0x%08X\n", signature);
    assert(signature == PLAIN_SIGNATURE);
    return 0;
}
