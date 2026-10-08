/* The mods' "card_layout" key as read (src/pc/cards/card_layout.c): real
 * manifests through the real JSON reader. What is checked without a
 * screen: retail's own defaults with no mod applied, every key landing
 * where func_80028B08.c/func_800283F4.c/func_800291E0 look for it, a
 * missing key falling back to retail rather than a half-finished look, an
 * inactive or losing mod being ignored, and the frame image path being
 * joined with its mod's own directory (or refused outside it). */
#include "../../src/pc/cards/card_layout.c"
#include <stdarg.h>
#include <stdlib.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);     \
            exit(1);                                                           \
        }                                                                       \
    } while (0)

static int notes;
static char note[512];
void Mods_Note(const char *id, const char *format, ...)
{
    va_list arguments;
    (void)id;
    notes++;
    va_start(arguments, format);
    vsnprintf(note, sizeof(note), format, arguments);
    va_end(arguments);
}

/* paths.c's rule, as far as these tests need it. */
int Paths_Contained(const char *relative)
{
    return relative && *relative && *relative != '/' && !strstr(relative, "..") && !strchr(relative, '\\');
}

int Log_Wanted(LogChannel channel) { (void)channel; return 0; }
void Log_Printf(LogChannel channel, const char *format, ...) { (void)channel; (void)format; }

/* cards.c stand-ins: a handful of ids CardLayout_SetCard's tests name, each
 * either an explicit frame colour (cards.h's "whatever its type") or a
 * type to bucket (card_constants.h). -1/CARD_TYPE_DRAGON ("monster") is
 * every id's default until a test sets otherwise. */
#define FAKE_CARD_COUNT 8
static int fake_frame_color[FAKE_CARD_COUNT];
static int fake_card_type[FAKE_CARD_COUNT];
int Cards_FrameColor(int id) { return (unsigned)id < FAKE_CARD_COUNT ? fake_frame_color[id] : -1; }
int Cards_Type(int id) { return (unsigned)id < FAKE_CARD_COUNT ? fake_card_type[id] : CARD_TYPE_DRAGON; }

#define MAX_MODS 4
static struct {
    const char *id;
    const char *directory;
    JsonDocument *document;
    int active;
    const char *setting_key;
    int setting_value;
} fake_mods[MAX_MODS];
static int fake_mod_count;

int Mods_LoadedCount(void) { return fake_mod_count; }
int Mods_Loaded(int index) { return index; }
int Mods_Active(int mod) { return fake_mods[mod].active; }
const char *Mods_Id(int mod) { return fake_mods[mod].id; }
const char *Mods_Directory(int mod) { return fake_mods[mod].directory; }
const JsonValue *Mods_Manifest(int mod) { return Json_Root(fake_mods[mod].document); }

int Mods_Setting(const char *id, const char *key, int fallback)
{
    int m;
    for (m = 0; m < fake_mod_count; m++) {
        if (strcmp(fake_mods[m].id, id)) continue;
        if (fake_mods[m].setting_key && !strcmp(fake_mods[m].setting_key, key)) return fake_mods[m].setting_value;
    }
    return fallback;
}

static void reset(void)
{
    int i;
    for (i = 0; i < fake_mod_count; i++) Json_Free(fake_mods[i].document);
    fake_mod_count = 0;
    notes = 0;
    note[0] = 0;
    for (i = 0; i < FAKE_CARD_COUNT; i++) {
        fake_frame_color[i] = -1;
        fake_card_type[i] = CARD_TYPE_DRAGON;
    }
    CardLayout_SetCard(0);   /* back to the monster bucket between tests */
}

static void add_mod(const char *id, const char *directory, const char *text, int active,
                    const char *setting_key, int setting_value)
{
    char error[256];
    JsonDocument *document = Json_Parse(text, error, sizeof(error));
    if (!document) fprintf(stderr, "%s\n", error);
    CHECK(document != NULL);
    CHECK(fake_mod_count < MAX_MODS);
    fake_mods[fake_mod_count].id = id;
    fake_mods[fake_mod_count].directory = directory;
    fake_mods[fake_mod_count].document = document;
    fake_mods[fake_mod_count].active = active;
    fake_mods[fake_mod_count].setting_key = setting_key;
    fake_mods[fake_mod_count].setting_value = setting_value;
    fake_mod_count++;
}

static const char *const FULL_MANIFEST =
    "{\"id\": \"x\", \"card_layout\": {"
    "\"frame\": {\"monster\": {\"image\": \"frame.png\", \"width\": 140, \"height\": 196},"
             "\"magic\": {\"image\": \"frame_magic.png\"}, \"trap\": {\"image\": \"frame_trap.png\"}},"
    "\"art\": {\"x\": 4, \"y\": 3, \"width\": 134, \"height\": 138},"
    "\"attribute\": {\"x\": 114, \"y\": 149, \"width\": 17, \"height\": 17},"
    "\"atk\": {\"x\": 38, \"y\": 178}, \"def\": {\"x\": 104, \"y\": 178},"
    "\"stars\": {\"x\": 80, \"y\": 153},"
    "\"spell\": {\"art\": {\"x\": 4, \"y\": 3, \"width\": 133, \"height\": 138},"
               "\"icon\": {\"x\": 62, \"y\": 163, \"width\": 16, \"height\": 15}}}}";

static void retail_defaults(void)
{
    CardLayoutPlacement p;
    reset();
    CHECK(!CardLayout_FullBleed());
    CHECK(notes == 0);
    CHECK(CardLayout_Get(CARD_LAYOUT_FRAME).visible);
    CHECK(CardLayout_Get(CARD_LAYOUT_TITLE).visible);
    CHECK(CardLayout_Get(CARD_LAYOUT_DESCRIPTION).visible);
    p = CardLayout_Get(CARD_LAYOUT_ATK);
    CHECK(p.x == 0x61 && p.y == 0x9D);
    p = CardLayout_Get(CARD_LAYOUT_DEF);
    CHECK(p.x == 0x61 && p.y == 0xAB);
    p = CardLayout_Get(CARD_LAYOUT_LEVEL_STARS);
    CHECK(p.x == 0x77 && p.y == 0x20);
    p = CardLayout_Get(CARD_LAYOUT_ATTRIBUTE);
    CHECK(p.x == 0x6E && p.y == 0xD && p.w == 0 && p.h == 0);
    CHECK(CardLayout_Get(CARD_LAYOUT_ART).w == 0);
    CHECK(!*CardLayout_FramePath());
}

static void full_bleed_mod(void)
{
    CardLayoutPlacement p;
    reset();
    add_mod("anime-card-frame", "/mods/anime-card-frame", FULL_MANIFEST, 1, "full_bleed", 1);
    CHECK(CardLayout_FullBleed());
    CHECK(notes == 0);
    p = CardLayout_Get(CARD_LAYOUT_FRAME);
    CHECK(!p.visible && p.w == 140 && p.h == 196);
    CHECK(!CardLayout_Get(CARD_LAYOUT_TITLE).visible);
    CHECK(CardLayout_Get(CARD_LAYOUT_DESCRIPTION).visible);
    p = CardLayout_Get(CARD_LAYOUT_ART);
    CHECK(p.x == 4 && p.y == 3 && p.w == 134 && p.h == 138);
    p = CardLayout_Get(CARD_LAYOUT_ATTRIBUTE);
    CHECK(p.x == 114 && p.y == 149 && p.w == 17 && p.h == 17);
    p = CardLayout_Get(CARD_LAYOUT_ATK);
    CHECK(p.x == 38 && p.y == 178);
    p = CardLayout_Get(CARD_LAYOUT_DEF);
    CHECK(p.x == 104 && p.y == 178);
    p = CardLayout_Get(CARD_LAYOUT_LEVEL_STARS);
    CHECK(p.x == 80 && p.y == 153);
    CHECK(!strcmp(CardLayout_FramePath(), "/mods/anime-card-frame/frame.png"));
}

/* The same manifest, but the mod's own full_bleed setting is off: every
 * element answers with retail's own place, same as no mod at all -- the
 * manifest's positions are not a second toggle of their own. */
static void setting_off(void)
{
    CardLayoutPlacement p;
    reset();
    add_mod("anime-card-frame", "/mods/anime-card-frame", FULL_MANIFEST, 1, "full_bleed", 0);
    CHECK(!CardLayout_FullBleed());
    CHECK(CardLayout_Get(CARD_LAYOUT_FRAME).visible);
    p = CardLayout_Get(CARD_LAYOUT_ATK);
    CHECK(p.x == 0x61 && p.y == 0x9D);
}

/* A player who has never touched the setting (no key at all, not even an
 * explicit 0): off, same as setting_off's explicit case -- a mod's own
 * manifest "default" (hd_assets_pack.py's "default": 0, notes/modding.md)
 * is display-only, read nowhere near this file; this engine-level fallback
 * (full_bleed_of's own Mods_Setting(..., 0)) is what a fresh install
 * actually gets, and it is this, not the manifest, that must change if the
 * answer should ever be different. */
static void setting_unset(void)
{
    reset();
    add_mod("anime-card-frame", "/mods/anime-card-frame", FULL_MANIFEST, 1, NULL, 0);
    CHECK(!CardLayout_FullBleed());
    CHECK(CardLayout_Get(CARD_LAYOUT_FRAME).visible);
}

/* A manifest that only gives one element keeps retail's place for every
 * other one, and no frame at all -- a half-written "card_layout" is not an
 * error, each key stands on its own. */
static void partial_manifest_falls_back(void)
{
    CardLayoutPlacement p;
    reset();
    add_mod("x", "/mods/x", "{\"id\": \"x\", \"card_layout\": {\"stars\": {\"x\": 80, \"y\": 153}}}", 1,
            "full_bleed", 1);
    p = CardLayout_Get(CARD_LAYOUT_LEVEL_STARS);
    CHECK(p.x == 80 && p.y == 153);
    p = CardLayout_Get(CARD_LAYOUT_ATK);
    CHECK(p.x == 0x61 && p.y == 0x9D);
    CHECK(!*CardLayout_FramePath());
    CHECK(notes == 0);
}

static void frame_outside_mod_refused(void)
{
    reset();
    add_mod("x", "/mods/x", "{\"id\": \"x\", \"card_layout\": {\"frame\": {\"monster\": {\"image\": \"../x.png\"}}}}", 1,
            "full_bleed", 1);
    CHECK(!*CardLayout_FramePath());
    CHECK(notes == 1 && strstr(note, "outside the mod"));
}

/* Two mods declaring "card_layout": the later one in load order wins, the
 * same rule every other singular manifest key follows. */
static void later_mod_wins(void)
{
    CardLayoutPlacement p;
    reset();
    add_mod("a", "/mods/a", "{\"id\": \"a\", \"card_layout\": {\"stars\": {\"x\": 1, \"y\": 2}}}", 1,
            "full_bleed", 1);
    add_mod("b", "/mods/b", "{\"id\": \"b\", \"card_layout\": {\"stars\": {\"x\": 9, \"y\": 9}}}", 1,
            "full_bleed", 1);
    p = CardLayout_Get(CARD_LAYOUT_LEVEL_STARS);
    CHECK(p.x == 9 && p.y == 9);
}

/* A mod the player has not applied is invisible to card_layout.c, same as
 * every other manifest key. */
static void inactive_mod_ignored(void)
{
    reset();
    add_mod("x", "/mods/x", "{\"id\": \"x\", \"card_layout\": {\"stars\": {\"x\": 9, \"y\": 9}}}", 0,
            "full_bleed", 1);
    CHECK(!CardLayout_FullBleed());
    CHECK(CardLayout_Get(CARD_LAYOUT_LEVEL_STARS).x == 0x77);
}

/* A magic card: no elemental attribute or ATK/DEF to draw, so CARD_LAYOUT_ART
 * and CARD_LAYOUT_ATTRIBUTE answer from "spell" instead of "art"/"attribute",
 * and the frame is "frame"."magic". */
static void spell_frame_magic(void)
{
    CardLayoutPlacement p;
    reset();
    add_mod("x", "/mods/x", FULL_MANIFEST, 1, "full_bleed", 1);
    fake_card_type[1] = CARD_TYPE_MAGIC;
    CardLayout_SetCard(1);
    CHECK(CardLayout_IsSpell());
    p = CardLayout_Get(CARD_LAYOUT_ART);
    CHECK(p.x == 4 && p.y == 3 && p.w == 133 && p.h == 138);
    p = CardLayout_Get(CARD_LAYOUT_ATTRIBUTE);
    CHECK(p.x == 62 && p.y == 163 && p.w == 16 && p.h == 15);
    CHECK(!strcmp(CardLayout_FramePath(), "/mods/x/frame_magic.png"));
}

/* A trap card: its own frame image. */
static void spell_frame_trap(void)
{
    reset();
    add_mod("x", "/mods/x", FULL_MANIFEST, 1, "full_bleed", 1);
    fake_card_type[1] = CARD_TYPE_TRAP;
    CardLayout_SetCard(1);
    CHECK(CardLayout_IsSpell());
    CHECK(!strcmp(CardLayout_FramePath(), "/mods/x/frame_trap.png"));
}

/* An equip card takes magic's frame, same as duel_effect_resource_setup.c's
 * own retail row mapping (CARD_TYPE_EQUIP falls into disp_14, magic's
 * setup, there). */
static void spell_frame_equip_uses_magic(void)
{
    reset();
    add_mod("x", "/mods/x", FULL_MANIFEST, 1, "full_bleed", 1);
    fake_card_type[1] = CARD_TYPE_EQUIP;
    CardLayout_SetCard(1);
    CHECK(CardLayout_IsSpell());
    CHECK(!strcmp(CardLayout_FramePath(), "/mods/x/frame_magic.png"));
}

/* FULL_MANIFEST gives "frame" no "ritual" of its own: a ritual card's frame
 * falls back to "monster"'s, same as any kind a mod has not drawn yet --
 * still a real frame, not none. */
static void spell_frame_ritual_falls_back_to_monster_frame(void)
{
    reset();
    add_mod("x", "/mods/x", FULL_MANIFEST, 1, "full_bleed", 1);
    fake_card_type[1] = CARD_TYPE_RITUAL;
    CardLayout_SetCard(1);
    CHECK(CardLayout_IsSpell());
    CHECK(!strcmp(CardLayout_FramePath(), "/mods/x/frame.png"));
}

/* Cards_FrameColor is an explicit per-card override "whatever its type"
 * (cards.h): a monster card a mod recoloured to the trap frame is still
 * drawn as the trap frame under full-bleed, not its real monster type. */
static void frame_color_override_wins_over_type(void)
{
    reset();
    add_mod("x", "/mods/x", FULL_MANIFEST, 1, "full_bleed", 1);
    fake_card_type[1] = CARD_TYPE_DRAGON;
    fake_frame_color[1] = CARD_FRAME_TRAP;
    CardLayout_SetCard(1);
    CHECK(CardLayout_IsSpell());
    CHECK(!strcmp(CardLayout_FramePath(), "/mods/x/frame_trap.png"));
}

static void ritual_wears_magic_unless_own_frame(void)
{
    reset();
    CHECK(!CardLayout_RitualWearsMagic());   /* no layout */
    add_mod("a", "/mods/a", FULL_MANIFEST, 1, "full_bleed", 1);
    CHECK(CardLayout_RitualWearsMagic());   /* no ritual image of its own */
    reset();
    add_mod("a", "/mods/a",
            "{\"id\": \"x\", \"card_layout\": {\"frame\": {\"monster\": {\"image\": \"m.png\"},"
            "\"magic\": {\"image\": \"g.png\"}, \"ritual\": {\"image\": \"g.png\"}}}}", 1, "full_bleed", 1);
    CHECK(CardLayout_RitualWearsMagic());   /* same file as magic's */
    reset();
    add_mod("a", "/mods/a",
            "{\"id\": \"x\", \"card_layout\": {\"frame\": {\"monster\": {\"image\": \"m.png\"},"
            "\"magic\": {\"image\": \"g.png\"}, \"ritual\": {\"image\": \"r.png\"}}}}", 1, "full_bleed", 1);
    CHECK(!CardLayout_RitualWearsMagic());  /* its own ritual frame stays */
}

int main(void)
{
    retail_defaults();
    full_bleed_mod();
    setting_off();
    setting_unset();
    partial_manifest_falls_back();
    frame_outside_mod_refused();
    later_mod_wins();
    inactive_mod_ignored();
    spell_frame_magic();
    spell_frame_trap();
    spell_frame_equip_uses_magic();
    spell_frame_ritual_falls_back_to_monster_frame();
    frame_color_override_wins_over_type();
    ritual_wears_magic_unless_own_frame();
    reset();
    printf("card layout: ok\n");
    return 0;
}
