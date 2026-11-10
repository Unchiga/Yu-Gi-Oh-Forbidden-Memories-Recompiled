/* A mod's "card_types" (cards.c, notes/modding.md): a
 * retail type renamed, custom monster types across the whole 24-255 bank,
 * a retail card converted to and from a custom monster type, a name that
 * collides with an earlier type's, two mods redefining the same id, and a
 * name the game has no letters for.
 *
 * Cards_Build runs once a process (the `built` static), so each scenario is
 * its own run, picked by argv[1], the way cards_order_test.c's "found" and
 * "b-first" are. */
#define _POSIX_C_SOURCE 200809L /* before any header: cards.c's strdup */
#include <stdint.h>

/* The disc's tables, here. cards.c reads them at fixed addresses. */
static unsigned test_stats[722];
static short test_sort_keys[722];
static unsigned char test_level_attr[723];
static unsigned short test_name_offsets[723];
static unsigned char test_text_bank[16] = {0xFF};
static unsigned test_glyphs[256];
#define RETAIL_STATS ((uintptr_t)test_stats)
#define RETAIL_SORT_KEYS ((uintptr_t)test_sort_keys)
#define RETAIL_LEVEL_ATTR ((uintptr_t)test_level_attr)
#define RETAIL_NAME_OFFSETS ((uintptr_t)test_name_offsets)
#define TEXT_BANK ((uintptr_t)test_text_bank)
#define GLYPH_TABLE ((uintptr_t)test_glyphs)

#include "../../src/pc/cards/cards.c"
#include "pc/debug/symbols.h"
#include "pc/mods/exports.h"
#include "scratch.h"
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>

int gCard_nCount = CARD_COUNT, gCard_nExtraOwner;
unsigned short gCard_awBaseId[CARD_TABLE_ID_END], gDuel_awPlayerDeck[1024];
unsigned char gCard_abExtraChest[CARD_TABLE_ID_END], gCard_abExtraSeen[(CARD_TABLE_ID_END + 7) / 8];
unsigned char gCard_abPairChest[2][CARD_TABLE_ID_END], gCard_abPairPending[2][CARD_TABLE_ID_END];
int gDuel_adwCardStats[CARD_TABLE_ID_END];
short gCard_asNameSortKey[CARD_TABLE_ID_END];
unsigned char gDuel_abCardLevelAttr[CARD_TABLE_ID_END];

/* --- what the mod loader reaches (as cards_order_test.c) ---------------- */

int Log_Enabled(LogChannel channel) { (void)channel; return 0; }
int Log_Wanted(LogChannel channel) { (void)channel; return 0; }
void Log_Printf(LogChannel channel, const char *format, ...) { (void)channel; (void)format; }
unsigned short Platform_Pad(int port) { (void)port; return 0; }
int Symbols_Add(const SymbolsEntry *entries, size_t count) { (void)entries; (void)count; return 0; }
int Memories_DiscReadSectors(int lba, int sectors, void *out)
{
    (void)lba;
    memset(out, 0, (size_t)sectors * 2048);
    return sectors;
}
int Memories_DiscSectorCount(void) { return 10000; }
int Memories_DiscOriginalFileInfo(const char *path, int *lba, unsigned *size)
{
    (void)path; (void)lba; (void)size;
    return -1;
}
int Memories_DiscFileInfo(const char *path, int *lba, unsigned *size)
{
    (void)path; (void)lba; (void)size;
    return -1;
}
int Memories_DiscFileStart(const char *path) { (void)path; return -1; }
const MemoriesModExport Memories_ModExports[] = {{"D_80010000", (void *)0x80010000u}};
const unsigned Memories_ModExportCount = 1;

/* --- what cards.c reaches that these checks never get to ---------------- */

int Campaign_TestStoryFlag(int flag) { (void)flag; return 0; }
int CardArt_Crop(const char *path, int w, int h, int *x, int *y, int *cw, int *ch, int *width, int *height)
{
    (void)path; (void)w; (void)h; (void)x; (void)y; (void)cw; (void)ch; (void)width; (void)height;
    return 0;
}
int CardArt_FromImage(const char *path, unsigned char *record, char *why, size_t why_size)
{
    (void)path; (void)record; (void)why; (void)why_size;
    return 0;
}
int CardArt_FromImageSeeThrough(const char *path, unsigned char *record, char *why, size_t why_size)
{
    (void)path; (void)record; (void)why; (void)why_size;
    return 0;
}
int CardArt_ThumbnailFromImageSeeThrough(const char *path, unsigned char *record, char *why, size_t why_size)
{
    (void)path; (void)record; (void)why; (void)why_size;
    return 0;
}
int CardArt_HasHoles(const char *path)
{
    (void)path;
    return 0;
}
int CardArt_ThumbnailFromImage(const char *path, unsigned char *record, char *why, size_t why_size)
{
    (void)path; (void)record; (void)why; (void)why_size;
    return 0;
}
int CardArt_FieldArtFromImage(const char *path, unsigned char *record, char *why, size_t why_size)
{
    (void)path; (void)record; (void)why; (void)why_size;
    return 0;
}
int CardArt_TitleFromImage(const char *path, unsigned char *plate, char *why, size_t why_size)
{
    (void)path; (void)plate; (void)why; (void)why_size;
    return 0;
}
int CardArt_TitleFromName(const char *name, unsigned char *plate) { (void)name; (void)plate; return 0; }
int CardLayout_FullBleed(void) { return 0; }
int CardLayout_StyleOf(int card_id, CardLayoutStyle *style) { (void)card_id; (void)style; return 0; }
void CardLayoutArt_Prewarm(void) {}
int CardNotes_Tag(const char *text, const char *key, char *out, size_t size)
{
    (void)text; (void)key; (void)out; (void)size;
    return 0;
}
void Duelists_Build(void) {}
/* ASCII only, one byte a glyph: a card_types "name" past 0x7F (bad_glyphs
 * below) has no letter here, the way a kana or an accent the disc's own
 * 8x8 font lacks does on the console. */
uint32_t Glyphs_Character(int code) { return (uint32_t)code; }
int Glyphs_Code(uint32_t character) { return character < 0x80 ? (int)character : -1; }
uint32_t Glyphs_NextCharacter(const char **text) { return (unsigned char)*(*text)++; }
void Library_UpdateCardUsedFlag(int flag) { (void)flag; }
int Memories_Rand(void) { return 0; }
void Starter_Build(void) {}
void Packs_Build(void) {}
void Tower_Build(void) {}
const UiConfig *UiConfig_Load(void) { return NULL; }
unsigned Packs_Signature(void) { return 0; }
void Tables_Build(void) {}
long Tables_Limit(const char *name) { (void)name; return -1; }
int Tables_StatCapEither(void) { return 9999; }
int Text_Overridden(int id) { (void)id; return 0; }
const unsigned char *Text_Resolve(int id, const unsigned char *retail) { (void)id; return retail; }
void TextureDump_Written(const void *destination, unsigned bytes) { (void)destination; (void)bytes; }
int TexturePack_AddMade(const void *pixels, int words, int rows, int bpp, const void *clut, int clut_entries,
                        const char *file, int x, int y, int w, int h)
{
    (void)pixels; (void)words; (void)rows; (void)bpp; (void)clut; (void)clut_entries; (void)file;
    (void)x; (void)y; (void)w; (void)h;
    return 0;
}
int TexturePack_AddMadeSeeThrough(const void *pixels, int words, int rows, int bpp, const void *clut,
                                  int clut_entries, const char *file, int x, int y, int w, int h)
{
    (void)pixels; (void)words; (void)rows; (void)bpp; (void)clut; (void)clut_entries; (void)file;
    (void)x; (void)y; (void)w; (void)h;
    return 0;
}
int Language_Current(void) { return 0; }
const char *Language_Code(int which) { (void)which; return "en-us"; }
/* The custom type icon bank (type_icons.c): no scenario here asks a
 * drawing function for a type's icon, so a no-op stands in for it. */
void TypeIcons_Clear(void) {}

/* --- fixtures ------------------------------------------------------------ */

static char root[SCRATCH_MAX];

static void write_text(const char *relative, const char *text)
{
    char path[2048];
    FILE *file;
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    file = fopen(path, "wb");
    assert(file);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    assert(!fclose(file));
}

static void make_dir(const char *relative)
{
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    assert(!mkdir(path, 0777));
}

static int attack(int id) { return (int)((unsigned)gDuel_adwCardStats[id - 1] & 0x1FF) * 10; }
static int raw_type(int id) { return (int)(((unsigned)gDuel_adwCardStats[id - 1] >> CARD_STAT_TYPE_SHIFT) & CARD_STAT_TYPE_MASK); }

/* Starts the real mod loader against "%s/mods", one mod folder per `names`
 * (already written by the caller), and builds the cards. */
static void load_and_build(const char *const *names, size_t count)
{
    char path[2048], settings[4096] = {0};
    size_t i, n = 0;
    for (i = 0; i < count; i++)
        n += (size_t)snprintf(settings + n, sizeof(settings) - n, "mod.%s=1\n", names[i]);
    write_text("settings.txt", settings);
    snprintf(path, sizeof(path), "%s/mods", root);
    assert(!setenv("MEMORIES_MODS_DIR", path, 1));
    snprintf(path, sizeof(path), "%s/settings.txt", root);
    assert(!setenv("MEMORIES_SETTINGS", path, 1));
    assert(!setenv("MEMORIES_USER_DIR", root, 1));
    Settings_Load();
    Mods_Load();
    assert(Mods_LoadedCount() == (int)count);
    Cards_Build();
}

static void setup(void)
{
    int id;
    for (id = 1; id <= CARD_COUNT; id++) test_stats[id - 1] = 100u; /* a Dragon, 1000 ATK, 0 DEF */
    assert(scratch_dir(root, sizeof(root), "memories-card-types"));
    make_dir("mods");
}

/* --- scenarios ------------------------------------------------------------ */

/* A retail type renamed (notes/modding.md): the disc's own 0-23 id, never
 * banked (cards.c's extended_types), with the mod's spelling wherever the
 * game shows the type -- Cards_TypeName (tooling, UTF-8) and
 * Cards_TypeNameText (the card view's glyph-encoded stream). */
static void rename_retail(void)
{
    static const unsigned char expect[] = {'W', 'y', 'r', 'm', 0xFF};
    setup();
    make_dir("mods/rename");
    write_text("mods/rename/mod.json",
               "{\"id\":\"rename\",\"min_api\":15,"
               "\"card_types\":{\"types\":[{\"id\":0,\"name\":\"Wyrm\"}]},"
               "\"cards\":[{\"replace\":1,\"type\":\"Dragon\"}]}");
    {
        static const char *const mods[] = {"rename"};
        load_and_build(mods, 1);
    }
    assert(!strcmp(Cards_TypeName(CARD_TYPE_DRAGON), "Wyrm"));
    assert(Cards_TypeNameText(CARD_TYPE_DRAGON) && !memcmp(Cards_TypeNameText(CARD_TYPE_DRAGON), expect, sizeof(expect)));
    assert(Cards_TypeNamed("Wyrm") == CARD_TYPE_DRAGON);
    assert(Cards_TypeNamed("Dragon") == CARD_TYPE_DRAGON); /* retail alias survives the rename */
    /* A plain card of the disc, left alone: still type Dragon, now Wyrm's
     * own spelling wherever that is shown; never banked. */
    assert(Cards_Type(1) == CARD_TYPE_DRAGON);
    assert(extended_types[1] < 0);
    printf("card_types rename: ok\n");
}

/* Every id in the custom bank a mod may use (24-255), at both ends and in
 * the middle: a card built into each keeps its own identity through
 * Cards_Type (the banked lookup), reads as a monster, finds itself by
 * name, and the stats word a card reader that never calls Cards_Type
 * still sees keeps the Dragon compatibility value (cards.c). */
static void custom_range(void)
{
    static const int ids[] = {24, 31, 32, 127, 200, 255};
    static const char *const names[] = {"Cyberse", "Psychic", "Fortress", "Phantom", "Titan", "Omega"};
    const MonsterEffect *effects;
    char manifest[4096];
    size_t i, n;
    setup();
    make_dir("mods/range");
    n = (size_t)snprintf(manifest, sizeof(manifest), "{\"id\":\"range\",\"min_api\":15,\"card_types\":{\"types\":[");
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        n += (size_t)snprintf(manifest + n, sizeof(manifest) - n, "%s{\"id\":%d,\"name\":\"%s\"}",
                              i ? "," : "", ids[i], names[i]);
    n += (size_t)snprintf(manifest + n, sizeof(manifest) - n, "]},\"cards\":[");
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        n += (size_t)snprintf(manifest + n, sizeof(manifest) - n, "%s{\"replace\":%d,\"type\":%d}",
                              i ? "," : "", (int)(10 + i), ids[i]);
    /* Monster effects filtered on a custom type: 200 is stored as a
     * negative byte and still matches only Titan; 255 is the filter's
     * "any" byte, so it cannot be one. */
    n += (size_t)snprintf(manifest + n, sizeof(manifest) - n,
                          ",{\"replace\":20,\"monster_effects\":["
                          "{\"when\":\"face_up\",\"do\":\"boost\",\"attack\":100,\"type\":\"Titan\"},"
                          "{\"when\":\"face_up\",\"do\":\"boost\",\"attack\":100,\"type\":\"Omega\"},"
                          "{\"when\":\"face_up\",\"do\":\"boost\",\"attack\":100,"
                          "\"for_each\":{\"type\":\"Titan\"}}]}");
    snprintf(manifest + n, sizeof(manifest) - n, "]}");
    write_text("mods/range/mod.json", manifest);
    {
        static const char *const mods[] = {"range"};
        load_and_build(mods, 1);
    }
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        int card = (int)(10 + i);
        assert(Cards_Type(card) == ids[i]);
        assert(Cards_IsMonsterType(ids[i]));
        assert(Cards_TypeNamed(names[i]) == ids[i]);
        assert(!strcmp(Cards_TypeName(ids[i]), names[i]));
        /* Banked: the stats word itself cannot hold 24-255 in five bits, so
         * it keeps the Dragon placeholder for any reader that is not
         * Cards_Type. */
        assert(raw_type(card) == CARD_TYPE_DRAGON);
        assert(extended_types[card] == ids[i]);
    }
    assert(Cards_MonsterEffects(20, &effects) == 2);
    assert(effects[0].type != -1 && (unsigned char)effects[0].type == 200);
    assert(effects[1].type == -1 && (unsigned char)effects[1].each_type == 200);
    printf("card_types custom range: ok\n");
}

/* A retail Magic card's "type" changed to a custom monster type: the
 * conversion rules (cards.c's add_entry) treat it as becoming a monster --
 * Cards_IsMonsterType(Cards_Type(...)) true -- and a monster's ATK/DEF are
 * kept, not stripped as a non-monster's are. */
static void magic_to_monster(void)
{
    setup();
    test_stats[0] = 150u | ((unsigned)CARD_TYPE_MAGIC << CARD_STAT_TYPE_SHIFT); /* card 1: Magic, 1500 ATK */
    make_dir("mods/conv");
    write_text("mods/conv/mod.json",
               "{\"id\":\"conv\",\"min_api\":15,"
               "\"card_types\":{\"types\":[{\"id\":24,\"name\":\"Cyberse\"}]},"
               "\"cards\":[{\"replace\":1,\"type\":\"Cyberse\",\"attack\":1800,\"defense\":1200}]}");
    {
        static const char *const mods[] = {"conv"};
        load_and_build(mods, 1);
    }
    assert(Cards_Type(1) == 24);
    assert(Cards_IsMonsterType(Cards_Type(1)));
    assert(attack(1) == 1800);
    assert((unsigned)((gDuel_adwCardStats[0] >> CARD_STAT_DEFENSE_SHIFT) & 0x1FF) * 10 == 1200); /* DEF kept, a monster now */
    printf("card_types magic to monster: ok\n");
}

/* A retail monster's "type" changed to Trap (a built-in non-monster kind,
 * not banked: it fits the stats word's five bits as-is): the conversion
 * strips ATK/DEF, as a non-monster's always are, and Cards_IsMonsterType
 * is false. */
static void monster_to_trap(void)
{
    setup(); /* card 1 already a 1000 ATK monster */
    make_dir("mods/trapify");
    write_text("mods/trapify/mod.json",
               "{\"id\":\"trapify\",\"cards\":[{\"replace\":1,\"type\":\"Trap\"}]}");
    {
        static const char *const mods[] = {"trapify"};
        load_and_build(mods, 1);
    }
    assert(Cards_Type(1) == CARD_TYPE_TRAP);
    assert(!Cards_IsMonsterType(Cards_Type(1)));
    assert(attack(1) == 0);
    assert(extended_types[1] < 0); /* Trap fits the stats word; never banked */
    printf("card_types monster to trap: ok\n");
}

/* A custom type cannot reuse an earlier normalized name (here the disc's
 * Dragon): registering it would make a textual type reference ambiguous. */
static void name_collision(void)
{
    setup();
    make_dir("mods/collide");
    write_text("mods/collide/mod.json",
               "{\"id\":\"collide\",\"min_api\":15,"
               "\"card_types\":{\"types\":[{\"id\":24,\"name\":\"Dragon\"}]},"
               "\"cards\":[{\"replace\":1,\"type\":\"Spelcaster\"}]}");
    {
        static const char *const mods[] = {"collide"};
        load_and_build(mods, 1);
    }
    assert(Cards_TypeNamed("Dragon") == CARD_TYPE_DRAGON);
    assert(!Cards_TypeName(24)[0]);
    /* A misspelled name scans the sparse 256-slot registry without ever
     * comparing text to a NULL entry, and leaves this retail card alone. */
    assert(Cards_Type(1) == CARD_TYPE_DRAGON);
    assert(strstr(Mods_Status(Mods_Loaded(0)), "already type"));
    printf("card_types name collision: ok\n");
}

/* Two mods both declaring type 24: the later-loaded mod's name and icon
 * win, the same precedence a replaced card already has (cards.c's own
 * comment: "later mods deliberately replace earlier names"). */
static void multi_mod_redefine(void)
{
    setup();
    make_dir("mods/first");
    /* "icon" only has to be a path inside the mod (Paths_Contained): this
     * scenario checks the registry's precedence, not TypeIcons_Cell's own
     * decoding, so neither a.png nor b.png need to exist. */
    write_text("mods/first/mod.json",
               "{\"id\":\"first\",\"min_api\":15,"
               "\"card_types\":{\"types\":[{\"id\":24,\"name\":\"Alpha\",\"icon\":\"a.png\"}]}}");
    make_dir("mods/second");
    write_text("mods/second/mod.json",
               "{\"id\":\"second\",\"min_api\":15,"
               "\"card_types\":{\"types\":[{\"id\":24,\"name\":\"Beta\",\"icon\":\"b.png\"}]}}");
    {
        static const char *const mods[] = {"first", "second"};
        load_and_build(mods, 2);
    }
    assert(!strcmp(Cards_TypeName(24), "Beta"));
    assert(Cards_TypeIcon(24) && strstr(Cards_TypeIcon(24), "second"));
    printf("card_types multiple mods: ok\n");
}

/* A name the game has no letters for (here, every character past ASCII,
 * which this stub's Glyphs_Code never accepts): the type is left out
 * entirely, not registered with a blank or partial name. */
static void bad_glyphs(void)
{
    setup();
    make_dir("mods/bad");
    /* U+00E9 "e acute", UTF-8 C3 A9: valid UTF-8, no glyph in this stub. */
    write_text("mods/bad/mod.json",
               "{\"id\":\"bad\",\"min_api\":15,"
               "\"card_types\":{\"types\":[{\"id\":24,\"name\":\"\xC3\xA9\"}]}}");
    {
        static const char *const mods[] = {"bad"};
        load_and_build(mods, 1);
    }
    assert(Cards_TypeNamed("\xC3\xA9") == -1);
    assert(!Cards_IsMonsterType(24));
    assert(strstr(Mods_Status(Mods_Loaded(0)), "no letter"));
    printf("card_types bad glyphs: ok\n");
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s rename|range|magic-to-monster|monster-to-trap|collision|multi-mod|bad-glyphs\n",
                argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "rename")) rename_retail();
    else if (!strcmp(argv[1], "range")) custom_range();
    else if (!strcmp(argv[1], "magic-to-monster")) magic_to_monster();
    else if (!strcmp(argv[1], "monster-to-trap")) monster_to_trap();
    else if (!strcmp(argv[1], "collision")) name_collision();
    else if (!strcmp(argv[1], "multi-mod")) multi_mod_redefine();
    else if (!strcmp(argv[1], "bad-glyphs")) bad_glyphs();
    else { fprintf(stderr, "unknown scenario %s\n", argv[1]); return 2; }
    return 0;
}
