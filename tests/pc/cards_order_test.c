/* Two mods' "cards" in the player's load order (notes/more-cards.md): the
 * real mod loader (mods.c, manager.c) finds two mods in a scratch folder and
 * orders them by the settings the Mods window writes, and cards.c's
 * Cards_Build reads them through Mods_VisitCards, on a stand-in for the
 * disc's tables.
 *
 * The bug this keeps away: the cards were read in the order the mod folders
 * were found (sorted by name) whatever the load order, so where two mods
 * replaced one card the winner ignored the order the player set, while every
 * other table followed it.
 *
 * Mods_Load and Cards_Build run once a process, so each order is a run of
 * its own: "found" leaves the order alone (cardorder-a, then cardorder-b, as
 * the folders sort), "b-first" puts cardorder-b first (mod.<id>.order). */
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

/* --- what the mod loader reaches (as tests/pc/mods_test.c) ------------ */

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

/* --- what cards.c reaches that these checks never get to -------------- */

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
int CardLayout_RitualWearsMagic(void) { return 0; }
void CardLayoutArt_Prewarm(void) {}
int CardNotes_Tag(const char *text, const char *key, char *out, size_t size)
{
    (void)text; (void)key; (void)out; (void)size;
    return 0;
}
void Duelists_Build(void) {}
uint32_t Glyphs_Character(int code) { return (uint32_t)code; }
int Glyphs_Code(uint32_t character) { return character < 0x80 ? (int)character : -1; }
uint32_t Glyphs_NextCharacter(const char **text) { return (unsigned char)*(*text)++; }
void Library_UpdateCardUsedFlag(int flag) { (void)flag; }
int Memories_Rand(void) { return 0; }
void Starter_Build(void) {}
void Packs_Build(void) {}
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
int Language_Current(void) { return 0; }
const char *Language_Code(int which) { (void)which; return "en-us"; }

/* --- fixtures -------------------------------------------------------- */

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

int main(int argc, char **argv)
{
    char path[2048];
    int b_first = argc > 1 && !strcmp(argv[1], "b-first");
    const char *first = b_first ? "cardorder-b" : "cardorder-a", *last = b_first ? "cardorder-a" : "cardorder-b";
    int id;
    if (argc < 2 || (!b_first && strcmp(argv[1], "found"))) {
        fprintf(stderr, "usage: %s found|b-first\n", argv[0]);
        return 2;
    }
    /* Every card of the disc a monster of 1000 ATK. */
    for (id = 1; id <= CARD_COUNT; id++) test_stats[id - 1] = 100u;

    assert(scratch_dir(root, sizeof(root), "memories-card-order"));
    make_dir("mods");
    /* Each replaces card 1 with its own name and ATK and adds a copy; b's
     * copy of card 1 comes before its own replace, so it is card 1 as the
     * mods before b left it. Card 4: a gives it a name (and so a plate), b
     * only an ATK. */
    make_dir("mods/cardorder-a");
    write_text("mods/cardorder-a/mod.json",
               "{\"id\":\"cardorder-a\",\"cards\":["
               "{\"replace\":1,\"name\":\"Alpha\",\"attack\":1100},"
               "{\"copy\":2,\"id\":\"twin\",\"name\":\"Alpha twin\"},"
               "{\"replace\":4,\"name\":\"Alpha baby\"}]}");
    make_dir("mods/cardorder-b");
    write_text("mods/cardorder-b/mod.json",
               "{\"id\":\"cardorder-b\",\"cards\":["
               "{\"copy\":1,\"id\":\"of-one\",\"name\":\"Beta copy\"},"
               "{\"replace\":1,\"name\":\"Beta\",\"attack\":2200},"
               "{\"replace\":4,\"attack\":500}]}");
    /* What the Mods window writes: both applied, and b moved up. */
    write_text("settings.txt", b_first ? "mod.cardorder-a=1\nmod.cardorder-b=1\nmod.cardorder-b.order=-1\n"
                                       : "mod.cardorder-a=1\nmod.cardorder-b=1\n");
    snprintf(path, sizeof(path), "%s/mods", root);
    assert(!setenv("MEMORIES_MODS_DIR", path, 1));
    snprintf(path, sizeof(path), "%s/settings.txt", root);
    assert(!setenv("MEMORIES_SETTINGS", path, 1));
    assert(!setenv("MEMORIES_USER_DIR", root, 1));
    Settings_Load();
    Mods_Load();
    assert(Mods_LoadedCount() == 2);
    assert(!strcmp(Mods_Id(Mods_Loaded(0)), first) && !strcmp(Mods_Id(Mods_Loaded(1)), last));

    Cards_Build();

    /* The later mod in load order has the last word on card 1. */
    assert(replaced[1] && !strcmp(replaced[1], last));
    assert(attack(1) == (b_first ? 1100 : 2200));
    /* The copies take their ids in load order; their identities stay. */
    assert(gCard_nCount == CARD_COUNT + 2);
    assert(!strcmp(Cards_Identity(CARD_COUNT + 1), b_first ? "cardorder-b:of-one:1" : "cardorder-a:twin:1"));
    assert(!strcmp(Cards_Identity(CARD_COUNT + 2), b_first ? "cardorder-a:twin:1" : "cardorder-b:of-one:1"));
    assert(Cards_FindIdentity("cardorder-a:twin:1") && Cards_FindIdentity("cardorder-b:of-one:1"));
    assert(Cards_BaseId(Cards_FindIdentity("cardorder-a:twin:1")) == 2);
    /* b's copy of card 1: card 1 as a left it when a came first, the disc's
     * when b did. */
    assert(attack(Cards_FindIdentity("cardorder-b:of-one:1")) == (b_first ? 1000 : 1100));

    /* Card 4: b's replace, later, gives no name, so a's name and the plate
     * that says it go, and the picture shows the card's own name; first, it
     * leaves them as a set them. b's ATK either way. */
    assert(attack(4) == 500);
    assert(b_first ? names[4] && plates[4] : !names[4] && !plates[4]);

    printf("cards order (%s): ok\n", argv[1]);
    return 0;
}
