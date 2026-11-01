/* The mod-defined field registry: retail aliases, custom ids and their
 * picture paths. */
#include "../../src/pc/cards/fields.c"
#include <assert.h>
#include <stdarg.h>

static int notes;
void Mods_Note(const char *id, const char *format, ...)
{
    va_list args;
    (void)id;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    notes++;
}
int Mods_LoadedCount(void) { return 0; }
int Mods_Loaded(int index) { return index; }
int Mods_Active(int mod) { return mod >= 0; }
const char *Mods_Id(int mod) { (void)mod; return ""; }
const char *Mods_Directory(int mod) { (void)mod; return "."; }
const JsonValue *Mods_Manifest(int mod) { (void)mod; return NULL; }
int Paths_Contained(const char *path) { return path && path[0] != '/'; }
static int texture_pack_call, texture_pack_fail_at = -1;    /* the 0-based call to fail, or -1 for none */
static unsigned texture_entry_count;
int TexturePack_AddDiscImageGroup(unsigned group, const char *file, const char *archive, unsigned offset, int words,
                                  int rows, int bpp, unsigned clut_offset, int clut_entries, int stride,
                                  int crop_left, int crop_width, int source_x, int source_y, int source_w,
                                  int source_h, unsigned rank)
{
    (void)group; (void)file; (void)archive; (void)offset; (void)words; (void)rows; (void)bpp; (void)clut_offset;
    (void)clut_entries; (void)stride; (void)crop_left; (void)crop_width; (void)source_x; (void)source_y;
    (void)source_w; (void)source_h; (void)rank;
    if (texture_pack_call++ == texture_pack_fail_at) return 0;
    texture_entry_count++;
    return 1;
}
unsigned TexturePack_EntryCount(void) { return texture_entry_count; }
void TexturePack_RemoveEntriesFrom(unsigned mark) { texture_entry_count = mark; }
static int image_size_ok = 1, image_size_w = 256, image_size_h = 254;
int TexturePack_ImageSize(const char *path, int *width, int *height)
{
    (void)path;
    if (!image_size_ok) return 0;
    *width = image_size_w;
    *height = image_size_h;
    return 1;
}
static unsigned selected_group = ~0u;
void TexturePack_SelectGroup(unsigned group) { selected_group = group; }

int main(void)
{
    char error[128];
    JsonDocument *first = Json_Parse("{\"fields\":{\"fields\":[{\"replace\":\"Normal\"},{\"replace\":\"Forest\",\"name\":\"Grove\"},"
                                     "{\"name\":\"Volcano\"}]}}", error, sizeof(error));
    JsonDocument *second = Json_Parse("{\"fields\":{\"fields\":[{\"id\":8,\"name\":\"Volcano\"},"
                                      "{\"id\":256,\"name\":\"Sky\"}]}}", error, sizeof(error));
    assert(first && second);
    Fields_Clear();
    assert(Fields_Find("Normal") == 0 && Fields_Find("Forest") == 1 && Fields_Find("Sea") == 5);
    Fields_Add("one", Json_Root(first));
    assert(Fields_Find("Grove") == 1 && Fields_Find("Forest") == 1);
    assert(Fields_Find("Volcano") == 7 && Fields_Declared(7));
    assert(Fields_Value(Json_Member(Json_At(Json_Member(Json_Member(Json_Root(first), "fields"), "fields"), 1), "replace")) == 1);
    Fields_Add("two", Json_Root(second));
    assert(Fields_Find("Sky") == -1 && notes == 2);

    /* A bare numeric id colliding with one already declared (here id 7,
     * Volcano, from "first") is rejected rather than silently renaming it:
     * "replace" is the sanctioned way to retarget an id by name. */
    JsonDocument *third = Json_Parse("{\"fields\":{\"fields\":[{\"id\":7,\"name\":\"Lava\"}]}}", error, sizeof(error));
    assert(third);
    Fields_Add("three", Json_Root(third));
    assert(Fields_Find("Volcano") == 7 && Fields_Find("Lava") == -1 && notes == 3);
    Json_Free(third);

    /* An explicit "id": 0 passes the id range check but can never be
     * named or found (Fields_Name/Fields_Find both treat 0 as Normal
     * unconditionally): rejected rather than silently accepted and
     * left unusable. "replace": "Normal" (the first entry of "first",
     * above) is the sanctioned way to give Normal its own image/name. */
    JsonDocument *fourth = Json_Parse("{\"fields\":{\"fields\":[{\"id\":0,\"name\":\"Custom Normal\"}]}}", error,
                                      sizeof(error));
    assert(fourth);
    Fields_Add("four", Json_Root(fourth));
    assert(Fields_Find("Custom Normal") == -1 && Fields_Name(0)[0] == 'N' && notes == 4);
    Json_Free(fourth);

    /* A picture whose file exists and opens but whose texture-pack
     * registration fails for even one of its five floor strips (a
     * malformed PNG, a bad crop, out of memory) must not leave the field
     * looking like it has a working picture: an unmirrored centre strip
     * (Fields_UnmirrorCentre) would otherwise remap the UVs of whatever
     * retail art is still showing there instead of the picture that
     * never actually loaded. Failing just the centre strip (floor_parts'
     * third entry) checks exactly that case, not only a total failure. */
    {
        FILE *picture = fopen("fields_test_picture.png", "wb");
        unsigned original_count;
        JsonDocument *fifth = Json_Parse(
            "{\"fields\":{\"fields\":[{\"name\":\"Scorched\",\"image\":\"fields_test_picture.png\",\"mirror\":false}]}}",
            error, sizeof(error));
        assert(picture && fifth);
        fputs("x", picture);
        fclose(picture);

        texture_pack_call = 0;
        texture_entry_count = 11; /* Entries registered by earlier mods stay intact. */
        original_count = texture_entry_count;
        texture_pack_fail_at = 2;
        Fields_Add("five", Json_Root(fifth));
        texture_pack_fail_at = -1;
        assert(Fields_Find("Scorched") == 8 && !Fields_Picture(8) && notes == 5);
        assert(texture_entry_count == original_count);
        Fields_Activate(8);
        assert(!Fields_UnmirrorCentre());
        Fields_ResetActive();

        Json_Free(fifth);
        remove("fields_test_picture.png");
    }

    /* A picture that cannot even be read as a PNG (picture_fits,
     * TexturePack_ImageSize) must reject the field before any of its five
     * floor strips are registered, not after some of them already are:
     * register_picture's own calls have no rollback once one installs. */
    {
        FILE *picture = fopen("fields_test_picture2.png", "wb");
        JsonDocument *sixth = Json_Parse(
            "{\"fields\":{\"fields\":[{\"name\":\"Broken\",\"image\":\"fields_test_picture2.png\"}]}}", error,
            sizeof(error));
        assert(picture && sixth);
        fputs("x", picture);
        fclose(picture);

        image_size_ok = 0;
        texture_pack_call = 0;
        Fields_Add("six", Json_Root(sixth));
        image_size_ok = 1;
        /* The field itself still declares (its name and rules stand on
         * their own); only the picture -- rejected before any of its five
         * strips registered -- is missing. */
        assert(Fields_Find("Broken") == 9 && !Fields_Picture(9) && texture_pack_call == 0 && notes == 6);

        Json_Free(sixth);
        remove("fields_test_picture2.png");
    }

    Json_Free(first);
    Json_Free(second);

    /* change_field's own dispatch: Fields_Activate, then the retail terrain
     * effect it started lands and confirms it with Fields_Observe. The
     * field bank must still report the custom field, not fall back to -1
     * the moment its own carrier's effect resolves, and its texture group
     * (7, > FIELDS_RETAIL) must stay selected. */
    Fields_Activate(7);
    assert(Fields_Active() == 7 && selected_group == 7);
    Fields_Observe();
    assert(Fields_Active() == 7 && selected_group == 7);

    /* An ordinary retail field spell played after that (or anything else
     * that resolves DuelEffect_ApplyTerrain without going through
     * Fields_Activate first) must make the custom field stale rather than
     * leave the field bank still reporting Volcano, and must restore the
     * retail texture group (0), not leave Volcano's bank selected. */
    Fields_Observe();
    assert(Fields_Active() == -1 && selected_group == 0);

    /* A new duel (func_80024DC8's reset) must clear a field left active at
     * the end of the previous one, even mid-dispatch (active_pending set),
     * and its texture group with it. */
    Fields_Activate(7);
    assert(Fields_Active() == 7 && selected_group == 7);
    Fields_ResetActive();
    assert(Fields_Active() == -1 && selected_group == 0);
    Fields_Observe();
    assert(Fields_Active() == -1 && selected_group == 0);

    /* A tower floor's added field: active from the start, and the first
       field spell played makes it stale, not confirms it. */
    Fields_Start(7);
    assert(Fields_Active() == 7 && selected_group == 7 && Fields_Carrier(7) == 1);
    Fields_Observe();
    assert(Fields_Active() == -1 && selected_group == 0);

    Fields_Clear();
    return 0;
}
