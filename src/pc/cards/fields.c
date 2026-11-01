/* Named retail and mod-defined duel fields.  The image is deliberately kept
 * with the field declaration, rather than folded into terrain_bonus: a board
 * renderer can select it independently of the rules it gives monsters. */
#include "fields.h"
#include "pc/compat/fs.h"
#include "pc/mods/json.h"
#include "pc/mods/mods.h"
#include "pc/platform/paths.h"
#include "pc/render/texture_pack.h"
#include "game/duel_package.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const retail_names[FIELDS_RETAIL][2] = {
    {"Forest", NULL}, {"Wasteland", NULL}, {"Mountain", NULL},
    {"Sogen", "Meadow"}, {"Umi", "Sea"}, {"Yami", "Dark"}
};
#define FIELD_NORMAL 0
#define FIELD_NORMAL_NAME "Normal"
static char *names[FIELDS_MAX + 1];
static char *pictures[FIELDS_MAX + 1];
static unsigned char declared[FIELDS_MAX + 1];
static unsigned char mirrors[FIELDS_MAX + 1];
static int active_field = -1;
static int active_pending;    /* Fields_Activate ran; its own terrain commit has not landed yet */
static int built;

/* The floor column of a terrain package: the same five cuts board_art.py
 * writes to a texture-pack manifest, but an author supplies just one image. */
static const struct { int target_y, source_y, rows, palette; } floor_parts[] = {
    /* `source_y` is floor_image/set_floor's consecutive image layout;
     * target_y is the board package's layout, whose centre lives at 208. */
    {0,   0, 52, 0x7F00}, {52,  52, 52, 0x7F20}, {208, 104, 46, 0x7F80},
    {104, 150, 52, 0x7F40}, {156, 202, 52, 0x7F60}
};

/* 1 when every one of the five floor strips registered; 0 when any one of
 * them did not (TexturePack_ImageSize/picture_fits in add() already rules
 * out the likeliest reason, a malformed or wrongly proportioned image, but
 * TexturePack_AddDiscImageGroup can still fail for one it cannot predict,
 * such as out of memory). The five are slices of one image, so a field's
 * picture is all of them or none: a mod's declared picture must never be
 * treated as present (Fields_Picture, Fields_UnmirrorCentre) when it is
 * not really there to show -- an unmirrored centre strip would otherwise
 * remap the UVs of whatever retail art is still showing underneath
 * instead -- and no strip that did install is left behind replacing part
 * of the field's board with the rest still retail. */
static int register_picture(int field, const char *path, unsigned rank)
{
    unsigned base;
    unsigned group;
    unsigned mark;
    int i, ok = 1;
    /* A custom field's rule identity is its field-bank id, not its physical
     * board upload.  It borrows a retail upload only because the original
     * board model exposes six such locations; TexturePack_SelectGroup keeps
     * every custom image in a separate bank so borrowed locations never
     * overwrite each other. */
    if (field < FIELD_NORMAL || field > FIELDS_MAX || !path) return 0;
    group = field > FIELDS_RETAIL ? (unsigned)field : 0;
    if (field > FIELDS_RETAIL) field = 1 + (field - FIELDS_RETAIL - 1) % FIELDS_RETAIL;
    base = (unsigned)(DUEL_TERRAIN_PACKAGE_FIRST_SECTOR + field * DUEL_TERRAIN_PACKAGE_SECTOR_COUNT + 203) * 2048;
    mark = TexturePack_EntryCount();
    for (i = 0; i < (int)(sizeof(floor_parts) / sizeof(*floor_parts)); i++) {
        const int y = floor_parts[i].target_y, rows = floor_parts[i].rows;
        if (!TexturePack_AddDiscImageGroup(group, path, "WA_MRG.MRG", base + (unsigned)y * 128, 64, rows, 4,
                                           base + (unsigned)floor_parts[i].palette, 16, 64, 0, 256,
                                           0, floor_parts[i].source_y, 256, rows, rank)) {
            ok = 0;
        }
    }
    if (!ok) TexturePack_RemoveEntriesFrom(mark);
    return ok;
}

/* Whether every floor strip's crop (floor_parts' source_y/rows, scaled
 * from the FM Editor's 256x254 board layout to the author's own PNG size
 * the same way TexturePack_AddDiscImageGroup's load_pixels does) lies
 * inside the image's real height. register_picture's five calls have no
 * rollback -- a later one failing does not undo an earlier one's already-
 * installed entry -- so this is checked first and all five are made only
 * when every one of them will fit, rather than some succeeding and others
 * not. The width never needs checking: every strip crops the image's full
 * width (source_x 0, source_w 256, the same as the 256 the layout scales
 * from), so it always maps back onto the image's own width exactly. */
static int picture_fits(const char *path)
{
    int width, height, i;
    if (!TexturePack_ImageSize(path, &width, &height) || width < 1 || height < 1) return 0;
    for (i = 0; i < (int)(sizeof(floor_parts) / sizeof(*floor_parts)); i++) {
        long long top = (long long)floor_parts[i].source_y * height / 254;
        long long bottom = (long long)(floor_parts[i].source_y + floor_parts[i].rows) * height / 254;
        if (bottom - top <= 0 || bottom > height) return 0;
    }
    return 1;
}

static int same_letters(const char *a, const char *b)
{
    for (;;) {
        while (*a && !isalnum((unsigned char)*a) && !((unsigned char)*a & 0x80)) a++;
        while (*b && !isalnum((unsigned char)*b) && !((unsigned char)*b & 0x80)) b++;
        if (!*a || !*b) return !*a && !*b;
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    }
}

static char *copy(const char *text)
{
    size_t size = strlen(text) + 1;
    char *out = malloc(size);
    if (out) memcpy(out, text, size);
    return out;
}

static int name_has_letters(const char *name)
{
    for (; *name; name++)
        if (isalnum((unsigned char)*name) || ((unsigned char)*name & 0x80)) return 1;
    return 0;
}

static int is_image(const char *path)
{
    const char *extension = strrchr(path, '.');
    return extension && extension[1] && extension[2] && extension[3] && !extension[4] &&
           extension[0] == '.' && tolower((unsigned char)extension[1]) == 'p' &&
           tolower((unsigned char)extension[2]) == 'n' && tolower((unsigned char)extension[3]) == 'g';
}

static int name_taken(const char *name, int except)
{
    int id;
    if (except != FIELD_NORMAL && same_letters(name, FIELD_NORMAL_NAME)) return 1;
    for (id = 1; id <= FIELDS_RETAIL; id++)
        if (id != except && (same_letters(name, retail_names[id - 1][0]) ||
                             (retail_names[id - 1][1] && same_letters(name, retail_names[id - 1][1])) ||
                             same_letters(name, Fields_Name(id)))) return 1;
    for (id = FIELDS_RETAIL + 1; id <= FIELDS_MAX; id++)
        if (id != except && names[id] && same_letters(name, names[id])) return 1;
    return 0;
}

void Fields_Clear(void)
{
    int id;
    for (id = 0; id <= FIELDS_MAX; id++) {
        free(names[id]);
        free(pictures[id]);
        names[id] = pictures[id] = NULL;
        declared[id] = 0;
        mirrors[id] = 1;
    }
    built = 0;
    active_field = -1;
    active_pending = 0;
    TexturePack_SelectGroup(0);
}

void Fields_Activate(int field)
{
    active_field = field;
    active_pending = 1;
    TexturePack_SelectGroup(field > FIELDS_RETAIL ? (unsigned)field : 0);
}
int Fields_Active(void) { return active_field; }
/* A duel that starts on `field` (a tower floor's): active at once, with no
 * terrain effect of its own to wait for, so the first field spell played
 * makes it stale (Fields_Observe). */
void Fields_Start(int field)
{
    Fields_Activate(field);
    active_pending = 0;
}
/* func_80024DC8 (a new duel's common setup, every entry point) calls this
 * the moment it resets gDuel_bTerrain: the field bank otherwise has no
 * signal that a new duel even started, and active_field would carry a
 * previous duel's field (even its id, were a later mod to reuse it).
 * Restores the texture group too (Fields_Activate's own counterpart),
 * since a stale selected group is exactly the same kind of leftover. */
void Fields_ResetActive(void)
{
    active_field = -1;
    active_pending = 0;
    TexturePack_SelectGroup(0);
}
/* DuelEffect_ApplyTerrain (func_80024E58.c) calls this every time it
 * commits a new gDuel_bTerrain, retail field card or Fields_Activate's own
 * dispatched one alike. The first commit after Fields_Activate is that
 * activation's own effect landing (change_field dispatches the carrier's
 * retail card, which resolves through this same routine) and is left
 * alone; since nothing else can commit a terrain in between in a
 * single-threaded duel, any later commit is a different field's effect
 * resolving, which makes whatever custom field Fields_Activate last
 * remembered stale -- an ordinary retail field spell played after a custom
 * one otherwise leaves the field bank still reporting the custom one, and
 * (Fields_ResetActive) the custom one's texture group still selected. */
void Fields_Observe(void)
{
    if (active_pending) {
        active_pending = 0;
        return;
    }
    Fields_ResetActive();
}
int Fields_Carrier(int field) { return field <= FIELDS_RETAIL ? field : 1 + (field - FIELDS_RETAIL - 1) % FIELDS_RETAIL; }
int Fields_UnmirrorCentre(void)
{
    return active_field > FIELDS_RETAIL && active_field <= FIELDS_MAX &&
           pictures[active_field] != NULL && !mirrors[active_field];
}

/* The stage model's centre-strip quad: the retail driver reads the same
 * texture row range (208-253) for both the far half (z >= 0) and the near
 * half (z < 0), U reversed on the near half, which is how it draws one
 * picture twice, turned round, across the raised strip. A field opting out
 * of that (Fields_UnmirrorCentre) wants its one picture read once, right
 * way round, across the whole strip: each half gets its own half of the 46
 * rows (208-230 far, 231-253 near, continuing from the far half's at the
 * seam) and the near half's U unreversed. `uv` is each corner's raw u8 u,
 * u8 v (v in the high byte); `z` is each corner's model-space Z. Left
 * untouched unless every corner's V lies in that retail centre band. */
void Fields_UnmirrorQuadUV(unsigned short uv[4], const int z[4])
{
    int i, zsum = 0, far;
    if (!Fields_UnmirrorCentre()) return;
    for (i = 0; i < 4; i++) {
        int v = (uv[i] >> 8) & 0xff;
        if (v < 208 || v >= 254) return;
        zsum += z[i];
    }
    far = zsum >= 0;
    for (i = 0; i < 4; i++) {
        unsigned u = uv[i] & 0xff, v = (uv[i] >> 8) & 0xff;
        if (far) {
            v = 208 + (v - 208) / 2;
        } else {
            u = 255 - u;
            v = 253 - (v - 208) / 2;
        }
        uv[i] = (unsigned short)((v << 8) | u);
    }
}

const char *Fields_Name(int field)
{
    if (field == FIELD_NORMAL) return FIELD_NORMAL_NAME;
    if (field >= 1 && field <= FIELDS_RETAIL)
        return names[field] ? names[field] : retail_names[field - 1][0];
    return field > FIELDS_RETAIL && field <= FIELDS_MAX && names[field] ? names[field] : "";
}

/* Whether Fields_Name(field) is a mod's own word rather than the retail
 * carrier's (FIELD_NORMAL and an undeclared field never are; a replaced
 * retail field only is when the mod gave it a "name" of its own). */
int Fields_HasCustomName(int field)
{
    if (field > FIELDS_RETAIL) return Fields_Declared(field);
    return field >= 1 && field <= FIELDS_RETAIL && names[field] != NULL;
}

const char *Fields_Picture(int field)
{
    return field >= FIELD_NORMAL && field <= FIELDS_MAX ? pictures[field] : NULL;
}

int Fields_Declared(int field)
{
    return field >= 1 && field <= FIELDS_MAX && (field <= FIELDS_RETAIL || declared[field]);
}

int Fields_Find(const char *name)
{
    char *end;
    long id;
    if (!name || !*name) return -1;
    id = strtol(name, &end, 10);
    if (!*end && id >= 1 && id <= FIELDS_MAX && Fields_Declared((int)id)) return (int)id;
    if (same_letters(name, FIELD_NORMAL_NAME)) return FIELD_NORMAL;
    for (int i = 1; i <= FIELDS_RETAIL; i++)
        if (same_letters(name, retail_names[i - 1][0]) ||
            (retail_names[i - 1][1] && same_letters(name, retail_names[i - 1][1])) || same_letters(name, Fields_Name(i))) return i;
    for (int i = FIELDS_RETAIL + 1; i <= FIELDS_MAX; i++)
        if (names[i] && same_letters(name, names[i])) return i;
    return -1;
}

int Fields_Value(const JsonValue *value)
{
    if (!value) return -1;
    if (Json_TypeOf(value) == JSON_NUMBER) {
        long id = Json_Number(value, -1);
        return id >= 1 && id <= FIELDS_MAX && Fields_Declared((int)id) ? (int)id : -1;
    }
    return Json_TypeOf(value) == JSON_STRING ? Fields_Find(Json_String(value, "")) : -1;
}

static int next_id(void)
{
    int id;
    for (id = FIELDS_RETAIL + 1; id <= FIELDS_MAX; id++) if (!declared[id]) return id;
    return -1;
}

static void add(const char *mod, const char *directory, const JsonValue *manifest, unsigned rank)
{
    const JsonValue *section = Json_Member(manifest, "fields");
    const JsonValue *list;
    int index = 0;
    if (!section) return;
    list = Json_Member(section, "fields");
    if (Json_TypeOf(section) != JSON_OBJECT || Json_TypeOf(list) != JSON_ARRAY) {
        Mods_Note(mod, "fields: an object with a \"fields\" list of {\"id\", \"name\", \"picture\"}");
        return;
    }
    for (const JsonValue *entry = Json_At(list, 0); entry; entry = Json_Next(entry), index++) {
        const JsonValue *id_value = Json_Member(entry, "id");
        const char *replace = Json_String(Json_Member(entry, "replace"), NULL);
        const char *name = Json_String(Json_Member(entry, "name"), NULL);
        const char *picture = Json_String(Json_Member(entry, "image"), Json_String(Json_Member(entry, "picture"), NULL));
        const JsonValue *mirror_value = Json_Member(entry, "mirror");
        long raw_id = Json_Number(id_value, -1);
        int explicit_id = !replace && id_value && Json_TypeOf(id_value) == JSON_NUMBER;
        int id;
        if (Json_TypeOf(entry) != JSON_OBJECT) {
            Mods_Note(mod, "fields: fields[%d]: an object with \"name\" and optional \"image\" or \"replace\"", index);
            continue;
        }
        /* Checked before the cast: a raw_id outside int's own range would
         * truncate to an implementation-defined value, which could by
         * chance land inside 1-255 and so pass the range check below.
         * An explicit id starts at 1, not FIELD_NORMAL: Fields_Name and
         * Fields_Find both treat 0 as Normal unconditionally, so an
         * explicit "id": 0 would pass every check here and then have no
         * way to be named or found -- "replace": "Normal" is how Normal's
         * image and name are meant to be given. */
        id = replace ? Fields_Find(replace)
            : explicit_id ? (raw_id >= 1 && raw_id <= FIELDS_MAX ? (int)raw_id : -1) : next_id();
        if (replace && (id < FIELD_NORMAL || id > FIELDS_RETAIL)) {
            Mods_Note(mod, "fields: fields[%d]: \"replace\" must name a retail field", index);
        } else if (id < FIELD_NORMAL || id > FIELDS_MAX) {
            Mods_Note(mod, "fields: fields[%d]: a field id is 1-255", index);
        } else if (explicit_id && Fields_Declared(id)) {
            /* "replace" is the sanctioned way to retarget an id a mod (this
             * one or an earlier one) already declared; a bare numeric id
             * colliding with one is a mistake, not an override -- silently
             * taking the later entry would leave the first one's picture,
             * if it had one, orphaned in the texture pack. */
            Mods_Note(mod, "fields: fields[%d]: id %d is already a field; use \"replace\" to retarget it by name",
                      index, id);
        } else if ((!name && !replace) || (name && (!*name || !name_has_letters(name)))) {
            Mods_Note(mod, "fields: fields[%d]: \"name\" is a non-empty string with a letter or number", index);
        } else if (name && name_taken(name, id)) {
            Mods_Note(mod, "fields: fields[%d]: \"name\" duplicates another field", index);
        } else if (mirror_value && Json_TypeOf(mirror_value) != JSON_BOOL) {
            Mods_Note(mod, "fields: fields[%d]: \"mirror\" must be true or false", index);
        } else {
            char *new_name = name ? copy(name) : NULL;
            if (name && !new_name) continue;
            if (new_name) { free(names[id]); names[id] = new_name; }
            declared[id] = 1;
            mirrors[id] = (unsigned char)Json_Bool(mirror_value, 1);
            free(pictures[id]);
            pictures[id] = NULL;
            if (picture) {
                char path[1200];
                FILE *probe;
                if (!*picture || !is_image(picture) || !directory || !Paths_Contained(picture) ||
                    snprintf(path, sizeof(path), "%s/%s", directory, picture) >= (int)sizeof(path)) {
                    Mods_Note(mod, "fields: fields[%d]: \"image\" names a PNG inside the mod", index);
                } else if (!(probe = fopen(path, "rb"))) {
                    Mods_Note(mod, "fields: fields[%d]: \"picture\": %s is not there", index, picture);
                } else {
                    fclose(probe);
                    if (!picture_fits(path)) {
                        Mods_Note(mod, "fields: fields[%d]: \"picture\": %s is not a readable 256x254-proportioned PNG",
                                  index, picture);
                    } else {
                        pictures[id] = copy(path);
                        if (pictures[id] && !register_picture(id, pictures[id], rank)) {
                            Mods_Note(mod, "fields: fields[%d]: \"picture\": %s could not be read as a board image",
                                      index, picture);
                            free(pictures[id]);
                            pictures[id] = NULL;
                        }
                    }
                }
            }
        }
    }
}

void Fields_Add(const char *mod, const JsonValue *manifest)
{
    add(mod, ".", manifest, 0);
}

void Fields_Build(void)
{
    if (built) return;
    built = 1;
    for (int i = 0; i < Mods_LoadedCount(); i++) {
        int mod = Mods_Loaded(i);
        if (Mods_Active(mod)) add(Mods_Id(mod), Mods_Directory(mod), Mods_Manifest(mod), (unsigned)i + 1);
    }
}
