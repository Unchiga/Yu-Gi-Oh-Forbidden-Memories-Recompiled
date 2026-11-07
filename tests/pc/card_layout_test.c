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
 * with an explicit frame color (a cards mod's "frame"), a type
 * (card_constants.h), effects, and tags. CARD_TYPE_DRAGON ("monster") with
 * none of the rest is every id's default until a test sets otherwise. */
#define FAKE_CARD_COUNT 8
static int fake_frame_color[FAKE_CARD_COUNT];
static char fake_frame_style[FAKE_CARD_COUNT][32];
static int fake_card_type[FAKE_CARD_COUNT];
static int fake_effects[FAKE_CARD_COUNT];
static char fake_tag[FAKE_CARD_COUNT][16];
static char fake_identity[FAKE_CARD_COUNT][64];   /* "mod:key:1" of a card a mod added, "" for a retail card */
int Cards_FrameOverride(int id) { return (unsigned)id < FAKE_CARD_COUNT ? fake_frame_color[id] : -1; }
const char *Cards_FrameStyle(int id) { return (unsigned)id < FAKE_CARD_COUNT ? fake_frame_style[id] : ""; }
const char *Cards_Identity(int id)
{
    return (unsigned)id < FAKE_CARD_COUNT && fake_identity[id][0] ? fake_identity[id] : NULL;
}
int Cards_Type(int id) { return (unsigned)id < FAKE_CARD_COUNT ? fake_card_type[id] : CARD_TYPE_DRAGON; }
int Cards_TypeNamed(const char *text)
{
    static const char *const names[] = {"Dragon", "Spellcaster", "Zombie", "Warrior", "Beast-Warrior", "Beast",
                                        "Winged Beast", "Fiend", "Fairy", "Insect", "Dinosaur", "Reptile", "Fish",
                                        "Sea Serpent", "Machine", "Thunder", "Aqua", "Pyro", "Rock", "Plant", "Magic",
                                        "Trap", "Ritual", "Equip"};
    int i;
    for (i = 0; text && i < (int)(sizeof(names) / sizeof(names[0])); i++)
        if (!strcmp(text, names[i])) return i;
    return -1;
}
int Cards_Class(int id)
{
    int type = Cards_Type(id);
    if (type == CARD_TYPE_MAGIC) return CARD_CLASS_SPELL;
    if (type == CARD_TYPE_EQUIP) return CARD_CLASS_EQUIP;
    if (type == CARD_TYPE_RITUAL) return CARD_CLASS_RITUAL_SPELL;
    if (type == CARD_TYPE_TRAP) return CARD_CLASS_TRAP;
    return (unsigned)id < FAKE_CARD_COUNT && fake_effects[id] ? CARD_CLASS_EFFECT_MONSTER : CARD_CLASS_MONSTER;
}
const char *Cards_ClassName(int cls)
{
    static const char *const names[CARD_CLASS_COUNT] = {"monster", "effect_monster", "spell", "equip",
                                                        "ritual_spell", "trap"};
    return cls >= 0 && cls < CARD_CLASS_COUNT ? names[cls] : "";
}
int Cards_HasTag(int id, const char *tag) { return (unsigned)id < FAKE_CARD_COUNT && !strcmp(fake_tag[id], tag); }

#define MAX_MODS 4
static struct {
    const char *id;
    const char *directory;
    JsonDocument *document;
    int active;
    const char *setting_key;
    int setting_value;
    const char *setting_key2;
    int setting_value2;
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
        if (fake_mods[m].setting_key2 && !strcmp(fake_mods[m].setting_key2, key)) return fake_mods[m].setting_value2;
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
        fake_frame_style[i][0] = 0;
        fake_card_type[i] = CARD_TYPE_DRAGON;
        fake_effects[i] = 0;
        fake_tag[i][0] = 0;
        fake_identity[i][0] = 0;
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
    fake_mods[fake_mod_count].setting_key2 = NULL;
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

/* The frame picture of the monster card id 0, after a mod was added: a card's
 * style is picked by CardLayout_SetCard, as the card view does. */
static const char *frame_path(void)
{
    CardLayout_SetCard(0);
    return CardLayout_FramePath();
}

static void retail_defaults(void)
{
    CardLayoutPart p;
    reset();
    CHECK(!CardLayout_FullBleed());
    CHECK(notes == 0);
    CHECK(CardLayout_Part(CARD_LAYOUT_FRAME).visible);
    CHECK(CardLayout_Part(CARD_LAYOUT_TITLE).visible);
    CHECK(CardLayout_Part(CARD_LAYOUT_DESCRIPTION).visible);
    p = CardLayout_Part(CARD_LAYOUT_ATK);
    CHECK(p.x == 0x61 && p.y == 0x9D);
    p = CardLayout_Part(CARD_LAYOUT_DEF);
    CHECK(p.x == 0x61 && p.y == 0xAB);
    p = CardLayout_Part(CARD_LAYOUT_LEVEL_STARS);
    CHECK(p.x == 0x77 && p.y == 0x20);
    p = CardLayout_Part(CARD_LAYOUT_ATTRIBUTE);
    CHECK(p.x == 0x6E && p.y == 0xD && p.w == 0 && p.h == 0);
    CHECK(CardLayout_Part(CARD_LAYOUT_ART).w == 0);
    CHECK(!*frame_path());
}

static void full_bleed_mod(void)
{
    CardLayoutPart p;
    reset();
    add_mod("anime-card-frame", "/mods/anime-card-frame", FULL_MANIFEST, 1, "full_bleed", 1);
    CardLayout_SetCard(0);
    CHECK(CardLayout_FullBleed());
    CHECK(notes == 0);
    p = CardLayout_Part(CARD_LAYOUT_FRAME);
    CHECK(!p.visible && p.w == 140 && p.h == 196);
    CHECK(!CardLayout_Part(CARD_LAYOUT_TITLE).visible);
    CHECK(CardLayout_Part(CARD_LAYOUT_DESCRIPTION).visible);
    p = CardLayout_Part(CARD_LAYOUT_ART);
    CHECK(p.x == 4 && p.y == 3 && p.w == 134 && p.h == 138);
    p = CardLayout_Part(CARD_LAYOUT_ATTRIBUTE);
    CHECK(p.x == 114 && p.y == 149 && p.w == 17 && p.h == 17);
    p = CardLayout_Part(CARD_LAYOUT_ATK);
    CHECK(p.x == 38 && p.y == 178);
    p = CardLayout_Part(CARD_LAYOUT_DEF);
    CHECK(p.x == 104 && p.y == 178);
    p = CardLayout_Part(CARD_LAYOUT_LEVEL_STARS);
    CHECK(p.x == 80 && p.y == 153);
    CHECK(CardLayout_StarsCentered());
    CHECK(!strcmp(frame_path(), "/mods/anime-card-frame/frame.png"));
    reset();
    add_mod("title-style", "/mods/title-style",
            "{\"id\": \"title-style\", \"card_layout\": {\"frame_styles\": {\"custom\": {\"image\": \"f.png\","
            " \"hand_color\": \"purple\", \"title\": {\"x\": 18, \"y\": 10}}}, \"default_style\": \"custom\"}}",
            1, "full_bleed", 1);
    CardLayout_SetCard(0);
    p = CardLayout_Part(CARD_LAYOUT_TITLE);
    CHECK(p.visible && p.x == 18 && p.y == 10);
    CHECK(!CardLayout_StarsCentered());
}

/* The same manifest, but the mod's own full_bleed setting is off (a fresh
 * install's default): retail's frame and title plate stay on screen, and
 * every element is where retail has it -- the manifest's places are
 * measured on its own frame, not on the retail plate. A fallback style
 * (a style with no image) can still move parts while the setting is on. */
static void setting_off(void)
{
    CardLayoutPart p;
    reset();
    add_mod("anime-card-frame", "/mods/anime-card-frame", FULL_MANIFEST, 1, "full_bleed", 0);
    CHECK(!CardLayout_FullBleed());
    CHECK(CardLayout_Part(CARD_LAYOUT_FRAME).visible);
    CHECK(CardLayout_Part(CARD_LAYOUT_TITLE).visible);
    p = CardLayout_Part(CARD_LAYOUT_ATK);
    CHECK(p.x == 0x61 && p.y == 0x9D);
    p = CardLayout_Part(CARD_LAYOUT_DEF);
    CHECK(p.x == 0x61 && p.y == 0xAB);
    p = CardLayout_Part(CARD_LAYOUT_LEVEL_STARS);
    CHECK(p.x == 0x77 && p.y == 0x20);
    p = CardLayout_Part(CARD_LAYOUT_ATTRIBUTE);
    CHECK(p.x == 0x6E && p.y == 0xD && p.w == 0 && p.h == 0);
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
    CHECK(CardLayout_Part(CARD_LAYOUT_FRAME).visible);
}

/* A manifest that only gives one element keeps retail's place for every
 * other one, and no frame at all -- a half-written "card_layout" is not an
 * error, each key stands on its own. */
static void partial_manifest_falls_back(void)
{
    CardLayoutPart p;
    reset();
    add_mod("x", "/mods/x", "{\"id\": \"x\", \"card_layout\": {\"stars\": {\"x\": 80, \"y\": 153}}}", 1,
            "full_bleed", 1);
    CardLayout_SetCard(0);
    p = CardLayout_Part(CARD_LAYOUT_LEVEL_STARS);
    CHECK(p.x == 80 && p.y == 153);
    p = CardLayout_Part(CARD_LAYOUT_ATK);
    CHECK(p.x == 0x61 && p.y == 0x9D);
    CHECK(!*frame_path());
    CHECK(notes == 0);
}

static void frame_outside_mod_refused(void)
{
    reset();
    add_mod("x", "/mods/x", "{\"id\": \"x\", \"card_layout\": {\"frame\": {\"monster\": {\"image\": \"../x.png\"}}}}", 1,
            "full_bleed", 1);
    CHECK(!*frame_path());
    CHECK(notes == 1 && strstr(note, "outside the mod"));
}

/* Two mods declaring "card_layout": the later one in load order wins, the
 * same rule every other singular manifest key follows. */
static void later_mod_wins(void)
{
    CardLayoutPart p;
    reset();
    add_mod("a", "/mods/a", "{\"id\": \"a\", \"card_layout\": {\"stars\": {\"x\": 1, \"y\": 2}}}", 1,
            "full_bleed", 1);
    add_mod("b", "/mods/b", "{\"id\": \"b\", \"card_layout\": {\"stars\": {\"x\": 9, \"y\": 9}}}", 1,
            "full_bleed", 1);
    CardLayout_SetCard(0);
    p = CardLayout_Part(CARD_LAYOUT_LEVEL_STARS);
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
    CHECK(CardLayout_Part(CARD_LAYOUT_LEVEL_STARS).x == 0x77);
}

/* A magic card: no elemental attribute or ATK/DEF to draw, so CARD_LAYOUT_ART
 * and CARD_LAYOUT_ATTRIBUTE answer from "spell" instead of "art"/"attribute",
 * and the frame is "frame"."magic". */
static void spell_frame_magic(void)
{
    CardLayoutPart p;
    reset();
    add_mod("x", "/mods/x", FULL_MANIFEST, 1, "full_bleed", 1);
    fake_card_type[1] = CARD_TYPE_MAGIC;
    CardLayout_SetCard(1);
    CHECK(CardLayout_IsSpell());
    p = CardLayout_Part(CARD_LAYOUT_ART);
    CHECK(p.x == 4 && p.y == 3 && p.w == 133 && p.h == 138);
    p = CardLayout_Part(CARD_LAYOUT_ATTRIBUTE);
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

/* FULL_MANIFEST gives "frame" no "ritual" of its own: a ritual card wears
 * magic's frame (the legacy rule), the picture its color in the hand
 * matches. */
static void spell_frame_ritual_wears_magic_frame(void)
{
    reset();
    add_mod("x", "/mods/x", FULL_MANIFEST, 1, "full_bleed", 1);
    fake_card_type[1] = CARD_TYPE_RITUAL;
    CardLayout_SetCard(1);
    CHECK(CardLayout_IsSpell());
    CHECK(!strcmp(CardLayout_FramePath(), "/mods/x/frame_magic.png"));
}

/* Cards_FrameColor is an explicit per-card override "whatever its type"
 * (cards.h): a monster card a mod recolored to the trap frame is still
 * drawn as the trap frame under full-bleed, not its real monster type. */
static void frame_color_override_wins_over_type(void)
{
    reset();
    add_mod("x", "/mods/x", FULL_MANIFEST, 1, "full_bleed", 1);
    fake_card_type[1] = CARD_TYPE_DRAGON;
    fake_frame_color[1] = CARD_FRAME_TRAP;
    CardLayout_SetCard(1);
    CHECK(!CardLayout_IsSpell());   /* laid out by what the card is: a monster still has its ATK/DFD and stars */
    CHECK(!strcmp(CardLayout_FramePath(), "/mods/x/frame_trap.png"));
}

/* "frame_styles", "frame_for" and "default_style" (card_layout.h's
 * CardLayoutFrameStyle): the names the whole feature is built on. */
static const char *const STYLES_MANIFEST =
    "{\"id\": \"x\", \"card_layout\": {"
    "\"frame_styles\": {"
        "\"gold\": {\"image\": \"gold.png\", \"hand_color\": \"gold\"},"
        "\"green\": {\"image\": \"green.png\", \"hand_color\": \"green\"},"
        "\"pink\": {\"image\": \"pink.png\", \"hand_color\": \"pink\"},"
        "\"orange\": {\"image\": \"orange.png\", \"hand_color\": \"orange\"},"
        "\"blue\": {\"image\": \"blue.png\", \"hand_color\": \"blue\"},"
        "\"godly\": {\"image\": \"god.png\", \"hand_color\": \"purple\", \"width\": 150, \"height\": 200}},"
    "\"frame_for\": ["
        "{\"tag\": \"god\", \"style\": \"godly\"},"
        "{\"class\": \"ritual_spell\", \"style\": \"blue\", \"setting\": \"ritual_own_frame\"},"
        "{\"class\": \"ritual_spell\", \"style\": \"green\"},"
        "{\"class\": \"effect_monster\", \"style\": \"orange\"},"
        "{\"class\": \"spell\", \"style\": \"green\"},"
        "{\"class\": \"equip\", \"style\": \"green\"},"
        "{\"class\": \"trap\", \"style\": \"pink\"}],"
    "\"default_style\": \"gold\"}}";

static void add_styles_mod(int own_ritual_frame)
{
    add_mod("x", "/mods/x", STYLES_MANIFEST, 1, "full_bleed", 1);
    fake_mods[0].setting_key2 = "ritual_own_frame";
    fake_mods[0].setting_value2 = own_ritual_frame;
}

static int style_of(int id, const char *want_path)
{
    CardLayoutFrameStyle style;
    CHECK(CardLayout_FrameStyleOf(id, &style));
    CardLayout_SetCard(id);
    CHECK(!strcmp(CardLayout_FramePath(), want_path));
    return style.color;
}

static void styles_by_class(void)
{
    CardLayoutFrameStyle style;
    reset();
    CHECK(!CardLayout_FrameStyleOf(1, &style));   /* no layout: retail */
    add_styles_mod(0);
    CHECK(style_of(1, "/mods/x/gold.png") == CARD_FRAME_MONSTER);
    fake_effects[2] = 1;
    CHECK(style_of(2, "/mods/x/orange.png") == CARD_FRAME_ORANGE);
    fake_card_type[3] = CARD_TYPE_MAGIC;
    CHECK(style_of(3, "/mods/x/green.png") == CARD_FRAME_MAGIC);
    fake_card_type[4] = CARD_TYPE_EQUIP;
    CHECK(style_of(4, "/mods/x/green.png") == CARD_FRAME_MAGIC);
    fake_card_type[5] = CARD_TYPE_TRAP;
    CHECK(style_of(5, "/mods/x/pink.png") == CARD_FRAME_TRAP);
    fake_card_type[6] = CARD_TYPE_RITUAL;
    CHECK(style_of(6, "/mods/x/green.png") == CARD_FRAME_MAGIC);   /* anime: a ritual spell is green */
    CHECK(CardLayout_IsSpell());                                    /* and laid out as a spell */
    CHECK(notes == 0);
}

static void styles_by_tag_and_size(void)
{
    reset();
    add_styles_mod(0);
    fake_tag[1][0] = 'g'; fake_tag[1][1] = 'o'; fake_tag[1][2] = 'd';
    fake_effects[1] = 1;   /* a god with effects: the tag rule comes first */
    CHECK(style_of(1, "/mods/x/god.png") == CARD_FRAME_PURPLE);
    CHECK(!CardLayout_IsSpell());
    CHECK(CardLayout_Part(CARD_LAYOUT_FRAME).w == 140 && CardLayout_Part(CARD_LAYOUT_FRAME).h == 196);
}

static void styles_rule_setting(void)
{
    reset();
    add_styles_mod(1);
    fake_card_type[1] = CARD_TYPE_RITUAL;
    CHECK(style_of(1, "/mods/x/blue.png") == CARD_FRAME_RITUAL);   /* the sub-option on: its own frame */
    CHECK(CardLayout_IsSpell());
    reset();
    add_styles_mod(0);
    fake_card_type[1] = CARD_TYPE_RITUAL;
    CHECK(style_of(1, "/mods/x/green.png") == CARD_FRAME_MAGIC);   /* off: the next rule */
}

static void styles_card_frame_and_default(void)
{
    reset();
    add_styles_mod(0);
    fake_frame_color[1] = CARD_FRAME_ORANGE;   /* a cards mod's "frame": Orange, on a plain monster */
    CHECK(style_of(1, "/mods/x/orange.png") == CARD_FRAME_ORANGE);
    fake_frame_color[2] = CARD_FRAME_PURPLE;   /* no "purple" style: the rules, then the default */
    CHECK(style_of(2, "/mods/x/gold.png") == CARD_FRAME_MONSTER);
    CHECK(notes == 0);
}

/* A card may select a custom frame by name, without coupling a cards mod to
 * the layout's class/tag rules or spending one of the six palette colors. */
static void styles_card_named_frame(void)
{
    reset();
    add_styles_mod(0);
    snprintf(fake_frame_style[1], sizeof(fake_frame_style[1]), "%s", "godly");
    fake_frame_color[1] = CARD_FRAME_ORANGE;  /* the named style wins over it */
    CHECK(style_of(1, "/mods/x/god.png") == CARD_FRAME_PURPLE);
    snprintf(fake_frame_style[2], sizeof(fake_frame_style[2]), "%s", "missing");
    CHECK(!CardLayout_FrameStyleOf(2, &(CardLayoutFrameStyle){0}) && notes == 1);
}

/* Type is more specific than kind, and an explicit card style is stronger
 * than either; both win over a color-named style and frame_for rules, and
 * the global default is the final non-retail fallback. */
static void styles_by_default_kind_and_type(void)
{
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {"
            "\"global\": {\"image\": \"global.png\", \"hand_color\": \"gold\"},"
            "\"kind_monster\": {\"image\": \"monster.png\", \"hand_color\": \"purple\"},"
            "\"kind_magic\": {\"image\": \"magic.png\", \"hand_color\": \"green\"},"
            "\"kind_ritual\": {\"image\": \"ritual.png\", \"hand_color\": \"blue\"},"
            "\"kind_trap\": {\"image\": \"trap.png\", \"hand_color\": \"pink\"},"
            "\"kind_equip\": {\"image\": \"equip.png\", \"hand_color\": \"green\"},"
            "\"dragon\": {\"image\": \"dragon.png\", \"hand_color\": \"purple\"},"
            "\"zombie\": {\"image\": \"zombie.png\", \"hand_color\": \"orange\"},"
            "\"type_equip\": {\"image\": \"type_equip.png\", \"hand_color\": \"orange\"},"
            "\"purple\": {\"image\": \"purple.png\", \"hand_color\": \"purple\"},"
            "\"specific\": {\"image\": \"specific.png\", \"hand_color\": \"pink\"}},"
            "\"default_styles\": {\"kinds\": {\"monster\": \"kind_monster\", \"magic\": \"kind_magic\","
            "\"ritual\": \"kind_ritual\", \"equip\": \"kind_equip\", \"trap\": \"kind_trap\"},"
            "\"types\": {\"Dragon\": \"dragon\", \"Zombie\": \"zombie\", \"Equip\": \"type_equip\"}},"
            "\"default_style\": \"global\"}}",
            1, "full_bleed", 1);
    /* A color-named style is a legacy fallback, not stronger than a type. */
    fake_frame_color[1] = CARD_FRAME_PURPLE;
    CHECK(style_of(1, "/mods/x/dragon.png") == CARD_FRAME_PURPLE);
    fake_card_type[2] = CARD_TYPE_ZOMBIE;
    CHECK(style_of(2, "/mods/x/zombie.png") == CARD_FRAME_ORANGE);
    fake_card_type[3] = CARD_TYPE_MAGIC;
    CHECK(style_of(3, "/mods/x/magic.png") == CARD_FRAME_MAGIC);
    fake_card_type[4] = CARD_TYPE_EQUIP;
    CHECK(style_of(4, "/mods/x/type_equip.png") == CARD_FRAME_ORANGE);
    fake_card_type[5] = CARD_TYPE_RITUAL;
    CHECK(style_of(5, "/mods/x/ritual.png") == CARD_FRAME_RITUAL);
    fake_card_type[6] = CARD_TYPE_TRAP;
    CHECK(style_of(6, "/mods/x/trap.png") == CARD_FRAME_TRAP);
    snprintf(fake_frame_style[7], sizeof(fake_frame_style[7]), "%s", "specific");
    CHECK(style_of(7, "/mods/x/specific.png") == CARD_FRAME_TRAP);

    /* No global default means that cards not caught by a card/type/kind
     * assignment retain the retail layout. */
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {\"only\": {\"image\": \"only.png\","
            "\"hand_color\": \"gold\"}}}}",
            1, "full_bleed", 1);
    CHECK(!CardLayout_StyleOf(1, &(CardLayoutStyle){0}));

    /* A color-named style (the released order) still wins over the global
     * default, though no longer over a type or kind default. */
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {"
            "\"global\": {\"image\": \"global.png\", \"hand_color\": \"gold\"},"
            "\"purple\": {\"image\": \"purple.png\", \"hand_color\": \"purple\"}},"
            "\"default_style\": \"global\"}}",
            1, "full_bleed", 1);
    fake_frame_color[1] = CARD_FRAME_PURPLE;
    CHECK(style_of(1, "/mods/x/purple.png") == CARD_FRAME_PURPLE);
    CHECK(style_of(2, "/mods/x/global.png") == CARD_FRAME_MONSTER);
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {"
            "\"purple\": {\"image\": \"purple.png\", \"hand_color\": \"purple\"}}}}",
            1, "full_bleed", 1);
    fake_frame_color[1] = CARD_FRAME_PURPLE;
    CHECK(style_of(1, "/mods/x/purple.png") == CARD_FRAME_PURPLE);
}

/* A layout can provide palette-only fallback styles.  They keep the normal
 * large-card plate, which lets a mod reserve its custom picture for only
 * the explicitly named cards. */
static void styles_can_preserve_retail_frame(void)
{
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {"
            "\"retail\": {\"hand_color\": \"gold\", \"full_bleed\": false},"
            "\"xyz\": {\"image\": \"xyz.png\", \"hand_color\": \"purple\"}},"
            "\"default_style\": \"retail\"}}",
            1, "full_bleed", 1);
    CardLayout_SetCard(1);
    CHECK(!CardLayout_FullBleed());
    CHECK(CardLayout_Part(CARD_LAYOUT_FRAME).visible);
    CHECK(!*CardLayout_FramePath());
    snprintf(fake_frame_style[2], sizeof(fake_frame_style[2]), "%s", "xyz");
    CardLayout_SetCard(2);
    CHECK(CardLayout_FullBleed());
    CHECK(!CardLayout_Part(CARD_LAYOUT_FRAME).visible);
    CHECK(!strcmp(CardLayout_FramePath(), "/mods/x/xyz.png"));
}

static void styles_hand_frame(void)
{
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {\"gold\": {\"image\": \"g.png\","
            "\"hand_color\": \"gold\", \"hand_image\": \"hand.png\"}}, \"default_style\": \"gold\"}}",
            1, "full_bleed", 1);
    CHECK(!strcmp(CardLayout_HandFramePath(1), "/mods/x/hand.png"));
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {\"gold\": {\"image\": \"g.png\","
            "\"hand_color\": \"gold\", \"hand_image\": \"../hand.png\"}}, \"default_style\": \"gold\"}}",
            1, "full_bleed", 1);
    CHECK(!*CardLayout_HandFramePath(1) && notes == 1);
}

/* Hand and 3D-field frames are independent.  In particular, a hand-only
 * style must not enter the 3D field renderer: it leaves the field card's
 * retail texture intact. */
static void styles_field_frame_is_explicit(void)
{
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {\"gold\": {\"image\": \"g.png\","
            "\"hand_color\": \"gold\", \"hand_image\": \"hand.png\"}}, \"default_style\": \"gold\"}}",
            1, "full_bleed", 1);
    CHECK(!strcmp(CardLayout_HandFramePath(1), "/mods/x/hand.png"));
    CHECK(!*CardLayout_FieldFramePath(1));

    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {\"gold\": {\"image\": \"g.png\","
            "\"hand_color\": \"gold\", \"hand_image\": \"hand.png\", \"field_image\": \"field.png\"}}, \"default_style\": \"gold\"}}",
            1, "full_bleed", 1);
    CHECK(!strcmp(CardLayout_FieldFramePath(1), "/mods/x/field.png"));
}

static void styles_hand_elements(void)
{
    CardLayoutPlacement p;
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {\"gold\": {\"image\": \"g.png\","
            "\"hand_color\": \"gold\", \"hand\": {\"art\": {\"x\": 3, \"y\": 4, \"width\": 30,"
            "\"height\": 25}, \"atk\": {\"x\": 18, \"hide\": true},"
            "\"dfd_label\": {\"y\": 51, \"width\": 10},"
            "\"def\": {\"x\": 999999, \"y\": -999999, \"width\": 999999, \"height\": 0}}}},"
            "\"default_style\": \"gold\"}}",
            1, "full_bleed", 1);
    CHECK(CardLayout_HandStyled(1));
    p = CardLayout_HandGet(1, CARD_LAYOUT_HAND_ART);
    CHECK(p.visible && p.x == 3 && p.y == 4 && p.w == 30 && p.h == 25);
    p = CardLayout_HandGet(1, CARD_LAYOUT_HAND_ATK);
    CHECK(!p.visible && p.x == 18 && p.y == 40 && p.w == 8 && p.h == 8);
    p = CardLayout_HandGet(1, CARD_LAYOUT_HAND_DEF_LABEL);
    CHECK(p.visible && p.x == 5 && p.y == 51 && p.w == 10 && p.h == 8);
    p = CardLayout_HandGet(1, CARD_LAYOUT_HAND_DEF);
    CHECK(p.visible && p.x == 128 && p.y == -128 && p.w == 128 && p.h == 8);
    {
        /* The same through a pointer, which is what a code mod calls. */
        int place[5], element;
        for (element = CARD_LAYOUT_HAND_ART; element <= CARD_LAYOUT_HAND_DEF_LABEL; element++) {
            CardLayoutPlacement want = CardLayout_HandGet(1, (CardLayoutHandElement)element);
            CardLayout_HandPlace(1, element, place);
            CHECK(place[0] == want.visible && place[1] == want.x && place[2] == want.y && place[3] == want.w &&
                  place[4] == want.h);
        }
    }
    reset();
    CHECK(!CardLayout_HandStyled(1));
    p = CardLayout_HandGet(1, CARD_LAYOUT_HAND_ART);
    CHECK(p.visible && p.x == 6 && p.y == 6 && p.w == 40 && p.h == 32);
}

static void styles_library_icon(void)
{
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {\"gold\": {\"image\": \"g.png\","
            "\"hand_color\": \"gold\", \"library_icon\": \"library.png\"}}, \"default_style\": \"gold\"}}",
            1, "full_bleed", 1);
    CHECK(!strcmp(CardLayout_LibraryIconPath(1), "/mods/x/library.png"));
}

static void styles_unusable_is_retail(void)
{
    CardLayoutFrameStyle style;
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {\"gold\": {\"image\": \"g.png\", \"hand_color\": \"lilac\"}}, \"default_style\": \"gold\"}}",
            1, "full_bleed", 1);
    CHECK(!CardLayout_FrameStyleOf(1, &style) && notes == 1);   /* a "hand_color" the disc has no row for */
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {\"gold\": {\"image\": \"g.png\"}}, \"default_style\": \"nothing\"}}",
            1, "full_bleed", 1);
    CHECK(!CardLayout_FrameStyleOf(1, &style) && notes == 1);   /* a style that is not there */
    reset();
    add_mod("x", "/mods/x", STYLES_MANIFEST, 1, "full_bleed", 0);
    CHECK(!CardLayout_FrameStyleOf(1, &style));                  /* anime frame off */
}

/* "hand_colour", the key's first spelling, still names the color, and
 * "hand_color" wins over it when a style has both. */
static void styles_hand_colour_alias(void)
{
    CardLayoutFrameStyle style;
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {\"old\": {\"image\": \"o.png\", \"hand_colour\": \"pink\"}},"
            " \"default_style\": \"old\"}}",
            1, "full_bleed", 1);
    CHECK(CardLayout_FrameStyleOf(1, &style) && style.color == CARD_FRAME_TRAP && notes == 0);
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {\"both\": {\"image\": \"b.png\","
            " \"hand_colour\": \"pink\", \"hand_color\": \"orange\"}}, \"default_style\": \"both\"}}",
            1, "full_bleed", 1);
    CHECK(CardLayout_FrameStyleOf(1, &style) && style.color == CARD_FRAME_ORANGE && notes == 0);
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {\"old\": {\"image\": \"o.png\", \"hand_colour\": \"lilac\"}},"
            " \"default_style\": \"old\"}}",
            1, "full_bleed", 1);
    CHECK(!CardLayout_FrameStyleOf(1, &style) && notes == 1);   /* checked as the new spelling is */
}

static void legacy_ritual_wears_magic(void)
{
    reset();
    add_mod("a", "/mods/a", FULL_MANIFEST, 1, "full_bleed", 1);   /* "frame" per kind, no ritual picture */
    fake_card_type[1] = CARD_TYPE_RITUAL;
    CHECK(style_of(1, "/mods/a/frame_magic.png") == CARD_FRAME_MAGIC);
    reset();
    add_mod("a", "/mods/a",
            "{\"id\": \"x\", \"card_layout\": {\"frame\": {\"monster\": {\"image\": \"m.png\"},"
            "\"magic\": {\"image\": \"g.png\"}, \"ritual\": {\"image\": \"r.png\"}}}}", 1, "full_bleed", 1);
    fake_card_type[1] = CARD_TYPE_RITUAL;
    CHECK(style_of(1, "/mods/a/r.png") == CARD_FRAME_RITUAL);      /* its own ritual picture stays */
}

static void digits_from_mod(void)
{
    char path[1024];
    int w = 0, h = 0, step = 0, x = 0, y = 0;

    reset();
    CHECK(!CardLayout_Digits(path, sizeof(path), &w, &h, &step) && !path[0]);   /* no layout */
    add_mod("a", "/mods/a", FULL_MANIFEST, 1, "full_bleed", 1);
    CHECK(!CardLayout_Digits(path, sizeof(path), &w, &h, &step) && !path[0]);   /* no "digits" */
    reset();
    add_mod("a", "/mods/a",
            "{\"id\": \"x\", \"card_layout\": {\"digits\": {\"image\": \"d.png\", \"width\": 9, \"height\": 10}}}",
            1, "full_bleed", 1);
    CHECK(CardLayout_Digits(path, sizeof(path), &w, &h, &step));
    CHECK(!strcmp(path, "/mods/a/d.png") && w == 9 && h == 10 && step == 9);   /* step defaults to width */
    reset();
    add_mod("a", "/mods/a",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {\"XYZ\": {\"image\": \"f.png\", \"hand_color\": \"purple\","
            " \"digits\": {\"image\": \"xyz.png\", \"width\": 11, \"height\": 13, \"step\": 12},"
            " \"atk_label\": {\"image\": \"atk.png\", \"x\": 7, \"y\": 8, \"width\": 19, \"height\": 9}},"
            " \"other\": {\"image\": \"o.png\", \"hand_color\": \"gold\", \"digits\": {\"image\": \"other.png\"}}},"
            " \"default_style\": \"XYZ\"}}",
            1, "full_bleed", 1);
    CardLayout_SetCard(1);
    CHECK(CardLayout_Digits(path, sizeof(path), &w, &h, &step));
    CHECK(!strcmp(path, "/mods/a/xyz.png") && w == 11 && h == 13 && step == 12);
    CHECK(CardLayout_StatLabel(0, path, sizeof(path), &x, &y, &w, &h));
    CHECK(!strcmp(path, "/mods/a/atk.png") && x == 7 && y == 8 && w == 19 && h == 9);
    CHECK(!CardLayout_StatLabel(1, path, sizeof(path), &x, &y, &w, &h) && !path[0]);
    reset();
    add_mod("a", "/mods/a",
            "{\"id\": \"x\", \"card_layout\": {\"digits\": {\"image\": \"d.png\"}}}", 1, "full_bleed", 0);
    CHECK(!CardLayout_Digits(path, sizeof(path), &w, &h, &step));   /* full-bleed off */
    reset();
    add_mod("a", "/mods/a",
            "{\"id\": \"x\", \"card_layout\": {\"digits\": {\"image\": \"../d.png\"}}}", 1, "full_bleed", 1);
    CHECK(!CardLayout_Digits(path, sizeof(path), &w, &h, &step) && notes == 1);   /* outside the mod */
}

/* "color" (CardLayoutPart's own r/g/b): 0x80 each, the GPU's own
 * neutral multiply, with no mod or no "color" of an element's own; read
 * from the manifest the same whether full_bleed is on or off, like the
 * element's x/y. */
static void color_tint(void)
{
    CardLayoutPart p;

    reset();
    p = CardLayout_Part(CARD_LAYOUT_ATK);           /* no mod at all */
    CHECK(p.r == 0x80 && p.g == 0x80 && p.b == 0x80);

    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {"
            "\"atk\": {\"x\": 38, \"y\": 178, \"color\": {\"r\": 255, \"g\": 96, \"b\": 96}},"
            "\"def\": {\"color\": {\"r\": 96, \"g\": 96, \"b\": 255}},"
            "\"stars\": {\"color\": {\"r\": 200}},"
            "\"atk_label\": {\"color\": {\"r\": 10, \"g\": 20, \"b\": 30}},"
            "\"dfd_label\": {\"x\": 5},"
            "\"attribute\": {\"color\": {\"g\": 50}}}}",
            1, "full_bleed", 1);
    CardLayout_SetCard(0);
    p = CardLayout_Part(CARD_LAYOUT_ATK_LABEL);        /* a label's tint, like the numbers' */
    CHECK(p.r == 10 && p.g == 20 && p.b == 30);
    p = CardLayout_Part(CARD_LAYOUT_DEF_LABEL);
    CHECK(p.r == 0x80 && p.g == 0x80 && p.b == 0x80);
    p = CardLayout_Part(CARD_LAYOUT_ATK);
    CHECK(p.x == 38 && p.y == 178 && p.r == 255 && p.g == 96 && p.b == 96);
    p = CardLayout_Part(CARD_LAYOUT_DEF);
    CHECK(p.r == 96 && p.g == 96 && p.b == 255);
    p = CardLayout_Part(CARD_LAYOUT_LEVEL_STARS);
    CHECK(p.r == 200 && p.g == 0x80 && p.b == 0x80);   /* unset channels stay neutral */
    p = CardLayout_Part(CARD_LAYOUT_ATTRIBUTE);
    CHECK(p.g == 50 && p.r == 0x80 && p.b == 0x80);

    /* full_bleed off: the layout is not in use, retail's own color (as
     * setting_off's retail positions). */
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {"
            "\"def\": {\"color\": {\"r\": 10, \"g\": 20, \"b\": 30}}}}",
            1, "full_bleed", 0);
    p = CardLayout_Part(CARD_LAYOUT_DEF);
    CHECK(p.r == 0x80 && p.g == 0x80 && p.b == 0x80);
}

/* A style's own level star: a picture, drawn by the game in the part's color; 9 x 9 and a step of the width
 * when left out, full-bleed only, and only for the style that names it. */
static void star_picture(void)
{
    char path[1024];
    int w, h, step;

    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {"
            "\"plain\": {\"image\": \"f.png\", \"hand_color\": \"gold\"},"
            "\"starry\": {\"image\": \"f.png\", \"hand_color\": \"gold\", "
            "\"stars\": {\"image\": \"star.png\", \"width\": 12, \"x\": 90, \"y\": 30, \"color\": {\"r\": 5}}},"
            "\"wide\": {\"image\": \"f.png\", \"hand_color\": \"gold\", "
            "\"stars\": {\"image\": \"star.png\", \"width\": 8, \"height\": 7, \"step\": 11}},"
            "\"huge\": {\"image\": \"f.png\", \"hand_color\": \"gold\", "
            "\"stars\": {\"image\": \"star.png\", \"width\": 999, \"step\": 999}}}}}",
            1, "full_bleed", 1);
    snprintf(fake_frame_style[1], sizeof(fake_frame_style[1]), "%s", "plain");
    snprintf(fake_frame_style[2], sizeof(fake_frame_style[2]), "%s", "starry");
    snprintf(fake_frame_style[3], sizeof(fake_frame_style[3]), "%s", "wide");
    snprintf(fake_frame_style[4], sizeof(fake_frame_style[4]), "%s", "huge");
    CardLayout_SetCard(1);
    CHECK(!CardLayout_StarImage(path, sizeof(path), &w, &h, &step) && !path[0]);   /* no picture: the game's */
    CardLayout_SetCard(2);
    CHECK(CardLayout_StarImage(path, sizeof(path), &w, &h, &step));
    CHECK(!strcmp(path, "/mods/x/star.png") && w == 12 && h == 9 && step == 12);
    CHECK(CardLayout_Part(CARD_LAYOUT_LEVEL_STARS).x == 90 && CardLayout_Part(CARD_LAYOUT_LEVEL_STARS).r == 5);
    CardLayout_SetCard(3);
    CHECK(CardLayout_StarImage(path, sizeof(path), &w, &h, &step) && w == 8 && h == 7 && step == 11);
    CardLayout_SetCard(4);
    CHECK(CardLayout_StarImage(path, sizeof(path), &w, &h, &step) && w == 32 && h == 9 && step == 64);   /* bounded */
    /* The mod off: the game's own star. */
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"stars\": {\"image\": \"star.png\"}}}", 1, "full_bleed", 0);
    CardLayout_SetCard(0);
    CHECK(!CardLayout_StarImage(path, sizeof(path), &w, &h, &step));
}

/* "game_frame": a full-bleed style keeps the game's own frame (the frame part stays visible) when it says so. */
static void game_frame_kept(void)
{
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {"
            "\"bare\": {\"full_bleed\": true, \"hand_color\": \"gold\"},"
            "\"kept\": {\"full_bleed\": true, \"game_frame\": true, \"hand_color\": \"gold\"},"
            "\"framed\": {\"image\": \"f.png\", \"hand_color\": \"gold\"}}}}",
            1, "full_bleed", 1);
    snprintf(fake_frame_style[1], sizeof(fake_frame_style[1]), "%s", "bare");
    snprintf(fake_frame_style[2], sizeof(fake_frame_style[2]), "%s", "kept");
    snprintf(fake_frame_style[3], sizeof(fake_frame_style[3]), "%s", "framed");
    CardLayout_SetCard(1);
    CHECK(CardLayout_FullBleed() && !CardLayout_Part(CARD_LAYOUT_FRAME).visible);    /* no frame at all */
    CardLayout_SetCard(2);
    CHECK(CardLayout_FullBleed() && CardLayout_Part(CARD_LAYOUT_FRAME).visible);     /* the game's stays */
    CardLayout_SetCard(3);
    CHECK(CardLayout_FullBleed() && !CardLayout_Part(CARD_LAYOUT_FRAME).visible);    /* its own picture replaces it */
}

/* On the game's own frame, the name, the words and the numbers are the game's unless the style gives them. */
static void game_look_kept(void)
{
    CardLayoutPart p;

    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {"
            "\"bare\": {\"full_bleed\": true, \"hand_color\": \"gold\"},"
            "\"kept\": {\"full_bleed\": true, \"game_frame\": true, \"hand_color\": \"gold\"},"
            "\"given\": {\"full_bleed\": true, \"game_frame\": true, \"hand_color\": \"gold\","
            " \"title\": {\"x\": 5, \"y\": 6}, \"atk_label\": {\"image\": \"a.png\"},"
            " \"digits\": {\"atk\": {\"x\": 50, \"y\": 60}}}}}}",
            1, "full_bleed", 1);
    snprintf(fake_frame_style[1], sizeof(fake_frame_style[1]), "%s", "bare");
    snprintf(fake_frame_style[2], sizeof(fake_frame_style[2]), "%s", "kept");
    snprintf(fake_frame_style[3], sizeof(fake_frame_style[3]), "%s", "given");
    CardLayout_SetCard(1);                                           /* a full bleed with no frame: nothing of the game's */
    CHECK(!CardLayout_GameTitle() && !CardLayout_GameLabel(0) && !CardLayout_GameLabel(1));
    CHECK(!CardLayout_Part(CARD_LAYOUT_TITLE).visible);
    CHECK(CardLayout_Part(CARD_LAYOUT_ATK).x == 0x61 && CardLayout_Part(CARD_LAYOUT_ATK).y == 0x9D);
    CardLayout_SetCard(2);                                           /* the game's frame: its name, words, numbers */
    CHECK(CardLayout_GameTitle() && CardLayout_GameLabel(0) && CardLayout_GameLabel(1));
    p = CardLayout_Part(CARD_LAYOUT_TITLE);
    CHECK(p.visible && p.x == 0xC && p.y == 0xE);
    p = CardLayout_Part(CARD_LAYOUT_ATK);
    CHECK(p.x == 0x61 + 12 && p.y == 0x9D + 6);                      /* the corner of the game's, as a centre */
    p = CardLayout_Part(CARD_LAYOUT_DEF);
    CHECK(p.x == 0x61 + 12 && p.y == 0xAB + 6);
    CardLayout_SetCard(3);                                           /* what the style gives is its own */
    CHECK(!CardLayout_GameTitle() && !CardLayout_GameLabel(0) && CardLayout_GameLabel(1));
    p = CardLayout_Part(CARD_LAYOUT_TITLE);
    CHECK(p.visible && p.x == 5 && p.y == 6);
    p = CardLayout_Part(CARD_LAYOUT_ATK);
    CHECK(p.x == 50 && p.y == 60);
    p = CardLayout_Part(CARD_LAYOUT_DEF);
    CHECK(p.x == 0x61 + 12 && p.y == 0xAB + 6);                      /* DEF not given: the game's */
}

/* "parts_over_art": the picture goes under the card's other parts, for the style that says so. */
static void parts_over_art(void)
{
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {"
            "\"plain\": {\"image\": \"f.png\", \"hand_color\": \"gold\"},"
            "\"over\": {\"image\": \"f.png\", \"hand_color\": \"gold\", \"parts_over_art\": true},"
            "\"no\": {\"image\": \"f.png\", \"hand_color\": \"gold\", \"parts_over_art\": false}}}}",
            1, "full_bleed", 1);
    snprintf(fake_frame_style[1], sizeof(fake_frame_style[1]), "%s", "plain");
    snprintf(fake_frame_style[2], sizeof(fake_frame_style[2]), "%s", "over");
    snprintf(fake_frame_style[3], sizeof(fake_frame_style[3]), "%s", "no");
    CardLayout_SetCard(1);
    CHECK(!CardLayout_PartsOverArt());
    CardLayout_SetCard(2);
    CHECK(CardLayout_PartsOverArt());
    CardLayout_SetCard(3);
    CHECK(!CardLayout_PartsOverArt());
    CardLayout_SetCard(0);
    CHECK(!CardLayout_PartsOverArt());                       /* a card of no style: as always */
}

/* A mod with several full-bleed styles, each its own frame image, can give
 * each its own atk/def/stars/attribute/art instead of them all sharing one
 * top-level box (card_layout.c's layout_part): two cards in two different
 * styles answer with two different layouts, a style missing a key falls
 * back to the layout's shared one, and a style's own key is taken whole --
 * a style that gives "atk" an x does not inherit the layout's y for it. */
static void per_style_layout(void)
{
    CardLayoutPart p;

    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {"
            "\"frame_styles\": {"
                "\"xyz\": {\"image\": \"xyz.png\", \"hand_color\": \"purple\","
                         "\"atk\": {\"x\": 85, \"y\": 172}, \"def\": {\"x\": 113, \"y\": 172}},"
                "\"synchro\": {\"image\": \"synchro.png\", \"hand_color\": \"blue\","
                             "\"atk\": {\"x\": 10, \"y\": 20}}},"
            "\"atk\": {\"x\": 61, \"y\": 157},"      /* the layout's shared point */
            "\"stars\": {\"x\": 77, \"y\": 32}}}",
            1, "full_bleed", 1);
    fake_frame_style[0][0] = 0;
    snprintf(fake_frame_style[0], sizeof(fake_frame_style[0]), "xyz");
    snprintf(fake_frame_style[1], sizeof(fake_frame_style[1]), "synchro");

    CardLayout_SetCard(0);
    p = CardLayout_Part(CARD_LAYOUT_ATK);
    CHECK(p.x == 85 && p.y == 172);                  /* xyz's own atk */
    p = CardLayout_Part(CARD_LAYOUT_DEF);
    CHECK(p.x == 113 && p.y == 172);                 /* xyz's own def */
    p = CardLayout_Part(CARD_LAYOUT_LEVEL_STARS);
    CHECK(p.x == 77 && p.y == 32);                   /* xyz has none: the layout's shared stars */

    CardLayout_SetCard(1);
    p = CardLayout_Part(CARD_LAYOUT_ATK);
    CHECK(p.x == 10 && p.y == 20);                   /* synchro's own atk, independent of xyz's */
    p = CardLayout_Part(CARD_LAYOUT_DEF);
    CHECK(p.x == 0x61 && p.y == 0xAB);                /* synchro has none, and neither does the layout: retail's */
    p = CardLayout_Part(CARD_LAYOUT_LEVEL_STARS);
    CHECK(p.x == 77 && p.y == 32);                   /* the layout's shared stars again */

    /* A style's "atk" naming only x does not pick up the layout's y for it
       (108, unused here) -- missing fields fall to retail's default alone. */
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {"
            "\"frame_styles\": {\"xyz\": {\"image\": \"xyz.png\", \"hand_color\": \"purple\","
                               "\"atk\": {\"x\": 85}}},"
            "\"atk\": {\"x\": 61, \"y\": 108}}}",
            1, "full_bleed", 1);
    snprintf(fake_frame_style[0], sizeof(fake_frame_style[0]), "xyz");
    CardLayout_SetCard(0);
    p = CardLayout_Part(CARD_LAYOUT_ATK);
    CHECK(p.x == 85 && p.y == 0x9D);                  /* x from the style, y retail's -- not the layout's 108 */
}

/* A partial art object is useful when a frame retains retail's art cutout
 * size but moves it.  Its missing values must be the retail rectangle, while
 * no art object at all still returns w/h 0 and leaves retail drawing alone. */
static void partial_art_rect_uses_retail_defaults(void)
{
    CardLayoutPlacement p;

    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {\"link\": {"
            "\"image\": \"link.png\", \"hand_color\": \"purple\", \"art\": {\"y\": 84}}},"
            "\"default_style\": \"link\"}}",
            1, "full_bleed", 1);
    CardLayout_SetCard(0);
    p = CardLayout_Get(CARD_LAYOUT_ART);
    CHECK(p.x == 0x13 && p.y == 84 && p.w == 0x66 && p.h == 0x60);

    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {\"plain\": {"
            "\"image\": \"plain.png\", \"hand_color\": \"purple\"}},"
            "\"default_style\": \"plain\"}}",
            1, "full_bleed", 1);
    CardLayout_SetCard(0);
    p = CardLayout_Get(CARD_LAYOUT_ART);
    CHECK(p.w == 0 && p.h == 0);
}

/* The card viewer's left-side footprint is fixed.  Manifest dimensions may
 * make a frame smaller, but cannot let either style format cover its right
 * information panel or the Library name bar. */
static void frame_box_is_bounded(void)
{
    CardLayoutPart p;

    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {\"wide\": {\"image\": \"wide.png\","
            "\"hand_color\": \"purple\", \"width\": 999, \"height\": 999},"
            "\"small\": {\"image\": \"small.png\", \"hand_color\": \"purple\", \"width\": 80, \"height\": 100}},"
            "\"default_style\": \"wide\"}}",
            1, "full_bleed", 1);
    CardLayout_SetCard(0);
    p = CardLayout_Part(CARD_LAYOUT_FRAME);
    CHECK(p.w == 140 && p.h == 196);
    snprintf(fake_frame_style[0], sizeof(fake_frame_style[0]), "small");
    CardLayout_SetCard(0);
    p = CardLayout_Part(CARD_LAYOUT_FRAME);
    CHECK(p.w == 80 && p.h == 100);

    reset();
    add_mod("legacy", "/mods/legacy",
            "{\"id\": \"legacy\", \"card_layout\": {\"frame\": {\"monster\": {\"image\": \"wide.png\","
            "\"width\": 999, \"height\": 999}}}}",
            1, "full_bleed", 1);
    CardLayout_SetCard(0);
    p = CardLayout_Part(CARD_LAYOUT_FRAME);
    CHECK(p.w == 140 && p.h == 196);
}

/* Every independently placed big-card part accepts hide/hidden/show:false.
 * Style-local entries continue to win over the layout-wide fallback. */
static void movable_parts_can_hide(void)
{
    CardLayoutPart p;

    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {"
            "\"title\": {\"hide\": true}, \"art\": {\"hidden\": true},"
            "\"attribute\": {\"show\": false}, \"stars\": {\"hide\": true},"
            "\"atk\": {\"hide\": true}, \"def\": {\"show\": false},"
            "\"atk_label\": {\"hide\": true}, \"dfd_label\": {\"show\": false},"
            "\"spell\": {\"icon\": {\"hide\": true}}}}",
            1, "full_bleed", 1);
    CardLayout_SetCard(0);
    CHECK(!CardLayout_Part(CARD_LAYOUT_TITLE).visible);
    CHECK(!CardLayout_Part(CARD_LAYOUT_ART).visible);
    CHECK(!CardLayout_Part(CARD_LAYOUT_ATTRIBUTE).visible);
    CHECK(!CardLayout_Part(CARD_LAYOUT_LEVEL_STARS).visible);
    CHECK(!CardLayout_Part(CARD_LAYOUT_ATK).visible);
    CHECK(!CardLayout_Part(CARD_LAYOUT_DEF).visible);
    CHECK(!CardLayout_Part(CARD_LAYOUT_ATK_LABEL).visible);
    CHECK(!CardLayout_Part(CARD_LAYOUT_DEF_LABEL).visible);
    CHECK(CardLayout_Part(CARD_LAYOUT_DESCRIPTION).visible);
    fake_card_type[0] = CARD_TYPE_MAGIC;
    CardLayout_SetCard(0);
    CHECK(!CardLayout_Part(CARD_LAYOUT_ATTRIBUTE).visible);

    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"atk\": {\"hide\": true},"
            "\"frame_styles\": {\"custom\": {\"image\": \"f.png\", \"hand_color\": \"purple\","
            "\"atk\": {\"x\": 80}}}, \"default_style\": \"custom\"}}",
            1, "full_bleed", 1);
    CardLayout_SetCard(0);
    p = CardLayout_Part(CARD_LAYOUT_ATK);
    CHECK(p.visible && p.x == 80);                  /* style owns its whole part */
}

/* CardLayout_TypeLabel: a style's own replacement for retail's "[Normal
 * Magic Card]"/"[Trap Card]"/"[Ritual Magic Card]"/"[Equip Magic Card]"
 * caption, one entry a card type, independently hideable even with no
 * replacement image (a position-only layout can still silence retail's
 * own built-in one). */
static void type_label_replaces_and_hides(void)
{
    char path[1024];
    int x, y, w, h, visible;

    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {\"custom\": {\"image\": \"f.png\","
            "\"hand_color\": \"purple\", \"type_label\": {"
            "\"trap\": {\"image\": \"t.png\", \"x\": 5, \"y\": 6, \"width\": 20, \"height\": 10},"
            "\"equip\": {\"hide\": true}}}},"
            "\"default_style\": \"custom\"}}",
            1, "full_bleed", 1);
    CardLayout_SetCard(0);
    CHECK(CardLayout_TypeLabel(CARD_TYPE_TRAP, path, sizeof(path), &x, &y, &w, &h, &visible));
    CHECK(visible && !strcmp(path, "/mods/x/t.png") && x == 5 && y == 6 && w == 20 && h == 10);
    /* No "magic" entry at all: retail's own built-in artwork stays, still visible. */
    CHECK(!CardLayout_TypeLabel(CARD_TYPE_MAGIC, path, sizeof(path), &x, &y, &w, &h, &visible));
    CHECK(visible && !*path);
    /* "hide" with no image: draw nothing there at all, not even retail's own. */
    CHECK(!CardLayout_TypeLabel(CARD_TYPE_EQUIP, path, sizeof(path), &x, &y, &w, &h, &visible));
    CHECK(!visible && !*path);

    /* "hide" together with an "image" (switching one back off without
     * deleting the other): still nothing, not the image either. */
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {\"custom\": {\"image\": \"f.png\","
            "\"hand_color\": \"purple\", \"type_label\": {"
            "\"trap\": {\"image\": \"t.png\", \"hide\": true}}}},"
            "\"default_style\": \"custom\"}}",
            1, "full_bleed", 1);
    CardLayout_SetCard(0);
    CHECK(!CardLayout_TypeLabel(CARD_TYPE_TRAP, path, sizeof(path), &x, &y, &w, &h, &visible));
    CHECK(!visible && !*path);

    /* An absurd width/height is bounded rather than handed to the renderer
     * raw (card_layout.c's frame_box_dimension, the same clamp the main
     * frame box uses). */
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"frame_styles\": {\"custom\": {\"image\": \"f.png\","
            "\"hand_color\": \"purple\", \"type_label\": {"
            "\"trap\": {\"image\": \"t.png\", \"width\": 99999, \"height\": 99999}}}},"
            "\"default_style\": \"custom\"}}",
            1, "full_bleed", 1);
    CardLayout_SetCard(0);
    CHECK(CardLayout_TypeLabel(CARD_TYPE_TRAP, path, sizeof(path), &x, &y, &w, &h, &visible));
    CHECK(w == CARD_LAYOUT_FRAME_MAX_W && h == CARD_LAYOUT_FRAME_MAX_H);

    /* A position-only layout (full_bleed off, nothing to replace the frame
     * with) can still hide retail's own built-in caption. */
    reset();
    add_mod("x", "/mods/x",
            "{\"id\": \"x\", \"card_layout\": {\"type_label\": {\"ritual\": {\"hide\": true}}}}",
            1, "full_bleed", 0);
    CardLayout_SetCard(0);
    CHECK(!CardLayout_TypeLabel(CARD_TYPE_RITUAL, path, sizeof(path), &x, &y, &w, &h, &visible));
    CHECK(!visible);
}

/* Frame styles from several layouts. "hd" is a whole layout (a released
 * pack's: rules, a default and its own parts); "foil" only adds styles, as a
 * cards mod's own frames do, and its "gold" is named as hd's is. */
static const char *const HD_MANIFEST =
    "{\"id\": \"hd\", \"card_layout\": {\"frame_styles\": {"
        "\"gold\": {\"image\": \"g.png\", \"hand_color\": \"gold\"},"
        "\"orange\": {\"image\": \"orange.png\", \"hand_color\": \"orange\"}},"
    "\"frame_for\": [{\"class\": \"effect_monster\", \"style\": \"orange\"}],"
    "\"default_style\": \"gold\", \"atk\": {\"x\": 38, \"y\": 178}}}";
static const char *const FOIL_MANIFEST =
    "{\"id\": \"foil\", \"card_layout\": {\"frame_styles\": {"
        "\"rainbow\": {\"image\": \"rainbow.png\", \"hand_color\": \"purple\", \"atk\": {\"x\": 11, \"y\": 22}},"
        "\"gold\": {\"image\": \"foilgold.png\", \"hand_color\": \"orange\"}}}}";

static void add_hd_and_foil(int hd_on, int foil_on)
{
    reset();
    add_mod("hd", "/mods/hd", HD_MANIFEST, 1, "full_bleed", hd_on);
    add_mod("foil", "/mods/foil", FOIL_MANIFEST, 1, "full_bleed", foil_on);
    snprintf(fake_frame_style[2], sizeof(fake_frame_style[2]), "%s", "rainbow");
}

static const char *path_of(int id)
{
    CardLayout_SetCard(id);
    return CardLayout_FramePath();
}

/* A layout of frame styles alone does not take the shared layout over: the
 * other mod's rules, default and parts stay what every plain card uses, and a
 * card naming a style of the styles-only mod is drawn by that mod's own. */
static void styles_only_does_not_shadow(void)
{
    CardLayoutFrameStyle style;
    reset();
    CHECK(!styles_only(NULL));
    add_hd_and_foil(1, 1);
    CHECK(find_source().mod == 0);                      /* hd, though foil comes later */
    CHECK(!strcmp(path_of(1), "/mods/hd/g.png"));       /* a plain card: hd's default */
    CHECK(CardLayout_FullBleed());
    CHECK(CardLayout_Part(CARD_LAYOUT_ATK).x == 38);    /* hd's shared part */
    CHECK(CardLayout_FrameStyleOf(1, &style) && style.mod == 0);
    CHECK(!strcmp(path_of(2), "/mods/foil/rainbow.png"));   /* a card of foil's: its style */
    CHECK(CardLayout_FullBleed());
    CHECK(CardLayout_Part(CARD_LAYOUT_ATK).x == 11);    /* foil's own part */
    CHECK(CardLayout_FrameStyleOf(2, &style) && style.mod == 1 && style.color == CARD_FRAME_PURPLE);
    CHECK(!strcmp(path_of(1), "/mods/hd/g.png"));       /* and back: nothing carried over */
    CHECK(CardLayout_Part(CARD_LAYOUT_ATK).x == 38);
    CHECK(notes == 0);
    /* A layout with anything besides "frame_styles" is a whole layout. */
    CHECK(!styles_only(Mods_Manifest(0) ? Json_Member(Mods_Manifest(0), "card_layout") : NULL));
    CHECK(styles_only(Json_Member(Mods_Manifest(1), "card_layout")));
}

/* Each layout's styles are on while that mod's own "full_bleed" is. */
static void style_owner_gate(void)
{
    CardLayoutFrameStyle style;
    /* foil off: its card is drawn as every other of hd's. */
    add_hd_and_foil(1, 0);
    CHECK(!strcmp(path_of(2), "/mods/hd/g.png"));
    CHECK(CardLayout_FrameStyleOf(2, &style) && style.mod == 0);
    CHECK(!strcmp(path_of(1), "/mods/hd/g.png"));
    /* hd off, foil on (the player's own case: retail frames, and a few cards
     * in a frame of their own): plain cards stay retail, with retail's parts. */
    add_hd_and_foil(0, 1);
    CHECK(!CardLayout_FrameStyleOf(1, &style));
    CardLayout_SetCard(1);
    CHECK(!CardLayout_FullBleed() && CardLayout_Part(CARD_LAYOUT_ATK).x == 0x61);
    CHECK(!strcmp(path_of(2), "/mods/foil/rainbow.png"));
    CHECK(CardLayout_FullBleed() && CardLayout_Part(CARD_LAYOUT_ATK).x == 11);
    CHECK(CardLayout_Active());
    /* foil alone, on or off. */
    reset();
    add_mod("foil", "/mods/foil", FOIL_MANIFEST, 1, "full_bleed", 1);
    snprintf(fake_frame_style[2], sizeof(fake_frame_style[2]), "%s", "rainbow");
    CHECK(!CardLayout_FrameStyleOf(1, &style));
    CHECK(!strcmp(path_of(2), "/mods/foil/rainbow.png"));
    reset();
    add_mod("foil", "/mods/foil", FOIL_MANIFEST, 1, "full_bleed", 0);
    snprintf(fake_frame_style[2], sizeof(fake_frame_style[2]), "%s", "rainbow");
    CHECK(!CardLayout_FrameStyleOf(2, &style) && !CardLayout_Active());
    CHECK(notes == 0);                                  /* off is not a missing style */
}

/* A style two layouts declare: the shared layout's rules find theirs first, a
 * card of a mod finds its own mod's, and any other card the newest, noted. */
static void style_name_collision(void)
{
    add_hd_and_foil(1, 1);
    snprintf(fake_frame_style[3], sizeof(fake_frame_style[3]), "%s", "gold");
    snprintf(fake_frame_style[4], sizeof(fake_frame_style[4]), "%s", "gold");
    snprintf(fake_identity[4], sizeof(fake_identity[4]), "%s", "hd:thing:1");
    CHECK(!strcmp(path_of(1), "/mods/hd/g.png"));              /* the default, by the shared layout */
    CHECK(notes == 0);
    CHECK(!strcmp(path_of(4), "/mods/hd/g.png"));              /* a card of hd's, naming it */
    CHECK(notes == 0);
    CHECK(!strcmp(path_of(3), "/mods/foil/foilgold.png"));     /* a retail card: the newest */
    CHECK(notes == 1 && strstr(note, "also declared"));
    CHECK(!strcmp(path_of(3), "/mods/foil/foilgold.png"));
    CHECK(notes == 1);                                          /* said once */
    /* A card of foil's naming it finds foil's whichever mod came last. */
    snprintf(fake_identity[3], sizeof(fake_identity[3]), "%s", "foil:other:1");
    CHECK(!strcmp(path_of(3), "/mods/foil/foilgold.png"));
    /* A name no layout has is noted, and the card is retail. */
    snprintf(fake_frame_style[5], sizeof(fake_frame_style[5]), "%s", "nothing");
    {
        CardLayoutFrameStyle style;
        int before = notes;
        CHECK(!CardLayout_FrameStyleOf(5, &style) && notes == before + 1);
    }
}

/* The pictures every layout in use may draw, once each. */
static void frame_paths_cover_every_layout(void)
{
    static char paths[8][1024];
    add_hd_and_foil(1, 1);
    CHECK(CardLayout_FramePaths(paths, 8) == 4);
    CHECK(!strcmp(paths[0], "/mods/hd/g.png") && !strcmp(paths[1], "/mods/hd/orange.png"));
    CHECK(!strcmp(paths[2], "/mods/foil/rainbow.png") && !strcmp(paths[3], "/mods/foil/foilgold.png"));
    CHECK(CardLayout_FramePaths(paths, 3) == 3);
    add_hd_and_foil(1, 0);
    CHECK(CardLayout_FramePaths(paths, 8) == 2);
    add_hd_and_foil(0, 1);
    CHECK(CardLayout_FramePaths(paths, 8) == 2 && !strcmp(paths[0], "/mods/foil/rainbow.png"));
    add_hd_and_foil(0, 0);
    CHECK(CardLayout_FramePaths(paths, 8) == 0 && !CardLayout_Active());
}

/* A mod's "full_bleed" is its own setting: off until the player turns it on, or on when the
 * settings entry that declares it says so, which is what the Mods window shows. */
static void declared_default_is_the_switch(void)
{
    CardLayoutFrameStyle style;
    static const char *const with_default =
        "{\"id\": \"foil\", \"settings\": [{\"key\": \"full_bleed\", \"type\": \"bool\", \"default\": 1}],"
        " \"card_layout\": {\"frame_styles\": {\"rainbow\": {\"image\": \"rainbow.png\", \"hand_color\": \"purple\"}}}}";
    static const char *const without_default =
        "{\"id\": \"foil\", \"settings\": [{\"key\": \"full_bleed\", \"type\": \"bool\", \"default\": 0}],"
        " \"card_layout\": {\"frame_styles\": {\"rainbow\": {\"image\": \"rainbow.png\", \"hand_color\": \"purple\"}}}}";
    reset();
    add_mod("foil", "/mods/foil", with_default, 1, NULL, 0);
    snprintf(fake_frame_style[2], sizeof(fake_frame_style[2]), "%s", "rainbow");
    CHECK(CardLayout_FrameStyleOf(2, &style) && CardLayout_Active());          /* on by its own default */
    reset();
    add_mod("foil", "/mods/foil", with_default, 1, "full_bleed", 0);
    snprintf(fake_frame_style[2], sizeof(fake_frame_style[2]), "%s", "rainbow");
    CHECK(!CardLayout_FrameStyleOf(2, &style) && !CardLayout_Active());        /* the player's choice wins */
    reset();
    add_mod("foil", "/mods/foil", without_default, 1, NULL, 0);
    snprintf(fake_frame_style[2], sizeof(fake_frame_style[2]), "%s", "rainbow");
    CHECK(!CardLayout_FrameStyleOf(2, &style));                                /* off by default, as HD's is */
    reset();
    add_mod("foil", "/mods/foil", "{\"id\": \"foil\", \"card_layout\": {\"frame_styles\": {\"rainbow\": "
            "{\"image\": \"rainbow.png\", \"hand_color\": \"purple\"}}}}", 1, NULL, 0);
    snprintf(fake_frame_style[2], sizeof(fake_frame_style[2]), "%s", "rainbow");
    CHECK(CardLayout_FrameStyleOf(2, &style));                                 /* no entry, styles alone: on */
    CHECK(notes == 0);
    /* A mod of styles alone that declares no switch has its frames always on: a card has them
     * only by naming one. A whole layout (HD's) with no switch stays off. */
    reset();
    add_mod("foil", "/mods/foil", FOIL_MANIFEST, 1, NULL, 0);
    snprintf(fake_frame_style[2], sizeof(fake_frame_style[2]), "%s", "rainbow");
    CHECK(CardLayout_FrameStyleOf(2, &style) && CardLayout_Active());
    CHECK(!CardLayout_FrameStyleOf(1, &style));                                /* other cards: retail */
    CHECK(!strcmp(path_of(2), "/mods/foil/rainbow.png"));
    CHECK(CardLayout_FullBleed() && CardLayout_Part(CARD_LAYOUT_ATK).x == 11);   /* and its parts */
    reset();
    add_mod("hd", "/mods/hd", HD_MANIFEST, 1, NULL, 0);
    CHECK(!CardLayout_FrameStyleOf(1, &style) && !CardLayout_Active());
    CHECK(notes == 0);
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
    spell_frame_ritual_wears_magic_frame();
    frame_color_override_wins_over_type();
    styles_by_class();
    styles_by_tag_and_size();
    styles_rule_setting();
    styles_card_frame_and_default();
    styles_card_named_frame();
    styles_by_default_kind_and_type();
    styles_can_preserve_retail_frame();
    styles_hand_frame();
    styles_field_frame_is_explicit();
    styles_hand_elements();
    styles_library_icon();
    styles_unusable_is_retail();
    styles_hand_colour_alias();
    legacy_ritual_wears_magic();
    digits_from_mod();
    color_tint();
    star_picture();
    parts_over_art();
    game_frame_kept();
    game_look_kept();
    per_style_layout();
    partial_art_rect_uses_retail_defaults();
    frame_box_is_bounded();
    movable_parts_can_hide();
    type_label_replaces_and_hides();
    styles_only_does_not_shadow();
    style_owner_gate();
    style_name_collision();
    frame_paths_cover_every_layout();
    declared_default_is_the_switch();
    reset();
    printf("card layout: ok\n");
    return 0;
}
