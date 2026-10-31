/* Real ISO lookup, replacement composition and asynchronous drive delivery. */
#define MODS_REAL_DISC
#define main original_mods_test
#include "pc/compat/fs.h"
#include "mods_test.c"
#undef main
#include "psyq/libds.h"
#include "pc/sdk/disc.h"
#include "pc/guest/state.h"
#include "pc/audio/spu.h"
#include "pc/render/texture_dump.h"
#include "pc/guest/image.h"
#include <stdint.h>

static char image_path[1024];
static int delivered_lba, callback_count, expected_lba, audio_blocks;
static int model_file;   /* the models case: the directory has a MODEL.MRG of two records */
#define RECORD (0x114 * 2048)
static unsigned char callback_data[5 * 2048];
const char *GameFiles_Disc(char *why, size_t size) { (void)why; (void)size; return image_path; }
void Crash_ReportFatal(const char *kind, const char *detail) { fprintf(stderr, "%s: %s\n", kind, detail); abort(); }
void Spu_CdFlush(void) {}
size_t Spu_CdWrite(const int16_t *frames, size_t count, unsigned rate) { (void)frames; (void)rate; audio_blocks++; return count; }
int AudioReplace_XaSectorMuted(int lba) { (void)lba; return 0; }
void Memories_GuestWritten(void *destination, size_t bytes) { (void)destination; (void)bytes; }
void TextureDump_SetDiscFiles(int (*info)(const char *, int *, unsigned *)) { (void)info; }
void TextureDump_Delivered(const void *destination, unsigned bytes, int lba, unsigned offset)
{ (void)destination; (void)bytes; (void)offset; delivered_lba = lba; }
void Log_Signal(int channel, const char *format, long a, long b, long c, long d, long e, long f)
{ (void)channel; (void)format; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }
int Memories_StateChunk(MemoriesState *state, const char *tag, const MemoriesStateField *fields, size_t count)
{ (void)state; (void)tag; (void)fields; (void)count; return 0; }
extern int CdGetSector(void *, int);
extern int CdControlB(u8, u8 *, u8 *);
extern DslLOC *CdIntToPos_8007E600(int, DslLOC *);
extern int CdPosToInt_8007E710(const DslLOC *);

static void le(unsigned char *p, unsigned value)
{ for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(value >> (8 * i)); }
static int record(unsigned char *p, const char *name, unsigned lba, unsigned size)
{
    int length = (int)strlen(name), bytes = (33 + length + 1) & ~1;
    p[0] = (unsigned char)bytes; le(p + 2, lba); le(p + 10, size);
    p[32] = (unsigned char)length; memcpy(p + 33, name, (size_t)length);
    return bytes;
}
#ifdef _WIN32
/* Declared here: <windows.h> clashes with the game's headers. */
__declspec(dllimport) int __stdcall DeviceIoControl(void *, unsigned long, void *, unsigned long, void *, unsigned long,
                                                    unsigned long *, void *);
#endif
/* The capacity case is a 1 GB image of which 40 sectors are written: a hole
 * on Linux, but NTFS allocates the whole of it unless it is marked sparse. */
static void sparse(FILE *image)
{
#ifdef _WIN32
    unsigned long bytes;
    DeviceIoControl((void *)_get_osfhandle(_fileno(image)), 0x000900C4 /* FSCTL_SET_SPARSE */, NULL, 0, NULL, 0,
                    &bytes, NULL);
#else
    (void)image;
#endif
}
static void fixture(int sectors)
{
    unsigned char raw[2352];
    FILE *image;
    snprintf(image_path, sizeof(image_path), "%s/disc.bin", root);
    image = fopen(image_path, "wb"); assert(image);
    sparse(image);
    for (int lba = 0; lba < 40; lba++) {
        memset(raw, 0, sizeof(raw)); raw[15] = 2; raw[18] = raw[22] = 8;
        if (lba == 16) { le(raw + 24 + 156 + 2, 17); le(raw + 24 + 156 + 10, 2048); }
        if (lba == 17) record(raw + 24, "DATA", 18, 2048);
        if (lba == 18) {
            int at = record(raw + 24, "CARD.MRG;1", 20, 5000);
            at += record(raw + 24 + at, "NEXT.DAT;1", 23, 2048);
            at += record(raw + 24 + at, "SECOND.MRG;1", 24, 2048);
            if (model_file) record(raw + 24 + at, "MODEL.MRG;1", 20, 2 * RECORD);
        }
        if (lba >= 20) memset(raw + 24, lba, 2048);
        if (lba == 30) { memset(raw + 24, 0, 2048); raw[18] = raw[22] = 0x64; }
        assert(fwrite(raw, 1, sizeof(raw), image) == sizeof(raw));
    }
    assert(!fseek(image, (long)sectors * 2352 - 1, SEEK_SET));
    assert(fputc(0, image) != EOF); assert(!fclose(image));
}
static void ready(u8 result, u8 *header, u32 *words)
{
    DslLOC loc;
    (void)words;
    assert(result == 1 && header[3] == 2 && header[6] == 8);
    memcpy(&loc, header, sizeof(loc));
    assert(CdPosToInt_8007E710(&loc) == expected_lba + callback_count);
    assert(callback_count < 5);
    assert(CdGetSector(callback_data + callback_count * 2048, 512));
    callback_count++;
    if (callback_count == 5) CdControlB(DslPause, NULL, NULL);
}
static void manifest(const char *id, const char *text)
{
    char path[256];
    snprintf(path, sizeof(path), "mods/%s", id); make_dir(path);
    snprintf(path, sizeof(path), "mods/%s/mod.json", id); write_text(path, text);
}
static int inspect_startup(const char *directory, unsigned rank, int (*part)(const char *, void *), void *context,
                           char *problems, size_t size)
{
    (void)directory; (void)rank; (void)part; (void)context; (void)problems; (void)size;
    /* Even an early mod's initialization sees later replacements. */
    assert(Memories_DiscFileStart("\\DATA\\CARD.MRG;1") == 40);
    return 1;
}
static void unload_pack(void) {}

/* "bigger-models": a model bigger than a record, its model data and then the
 * record's other stages. It has no sector; its window holds those stages
 * after a first stage of zeros, and its model data goes to its slot's arena
 * (at MODEL_ARENA_BASE, the test's own). A file that is not whole sectors,
 * or has more model data than an arena takes, is refused. */
static int bigger_models_case(void)
{
    enum { DATA = 136, TAIL = 180, PAGE = 16 };
    static unsigned char big[(DATA + TAIL) * 2048], odd[RECORD + 100], huge[(512 + 1 + TAIL) * 2048], got[2048];
    static unsigned char paged[(96 + TAIL + 2 * PAGE) * 2048], record[RECORD], thin[(80 + TAIL + 2 * PAGE) * 2048];
    unsigned char *arena;
    const unsigned char *pages;
    int count;
    char path[1024];
    int window, *windows;
    unsigned size;
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (unsigned char)(i / 2048 + 1);
    manifest("b-big", "{\"enabled\":true,\"models\":[{\"name\":\"big\",\"file\":\"big.bin\"}]}");
    write_file("mods/b-big/big.bin", big, sizeof(big));
    manifest("c-odd", "{\"enabled\":true,\"models\":[{\"name\":\"odd\",\"file\":\"odd.bin\"}]}");
    write_file("mods/c-odd/odd.bin", odd, sizeof(odd));
    manifest("d-huge", "{\"enabled\":true,\"models\":[{\"name\":\"huge\",\"file\":\"huge.bin\"}]}");
    write_file("mods/d-huge/huge.bin", huge, sizeof(huge));
    /* "texture_pages": after the record's other stages, 16 sectors each. */
    for (size_t i = 0; i < sizeof(paged); i++) paged[i] = (unsigned char)(i / 2048 + 1);
    manifest("e-paged", "{\"enabled\":true,\"models\":[{\"name\":\"paged\",\"file\":\"p.bin\",\"texture_pages\":2}]}");
    write_file("mods/e-paged/p.bin", paged, sizeof(paged));
    manifest("f-short", "{\"enabled\":true,\"models\":[{\"name\":\"short\",\"file\":\"r.bin\",\"texture_pages\":1}]}");
    write_file("mods/f-short/r.bin", record, sizeof(record));
    manifest("g-many", "{\"enabled\":true,\"models\":[{\"name\":\"many\",\"file\":\"p.bin\",\"texture_pages\":9}]}");
    write_file("mods/g-many/p.bin", paged, sizeof(paged));
    manifest("h-thin", "{\"enabled\":true,\"models\":[{\"name\":\"thin\",\"file\":\"t.bin\",\"texture_pages\":2}]}");
    write_file("mods/h-thin/t.bin", thin, sizeof(thin));   /* 80 sectors of model data: fewer than 96 */
    snprintf(path, sizeof(path), "%s/mods", root); assert(!setenv("MEMORIES_MODS_DIR", path, 1));
    assert(!setenv("MEMORIES_USER_DIR", root, 1));
    snprintf(path, sizeof(path), "%s/settings", root); assert(!setenv("MEMORIES_SETTINGS", path, 1));
    Settings_Load(); Mods_Load();
    assert(Mods_Active(find("b-big")));
    assert(!Mods_Active(find("c-odd")) && strstr(Mods_Status(find("c-odd")), "not a bigger model"));
    assert(!Mods_Active(find("d-huge")) && strstr(Mods_Status(find("d-huge")), "not a bigger model"));
    assert(Mods_Active(find("e-paged")));
    assert(!Mods_Active(find("f-short")) && strstr(Mods_Status(find("f-short")), "no larger than a record"));
    assert(!Mods_Active(find("g-many")) && strstr(Mods_Status(find("g-many")), "\"texture_pages\" is a count"));
    assert(!Mods_Active(find("h-thin")) && strstr(Mods_Status(find("h-thin")), "not a bigger model"));
    assert(Mods_ModelCount() == 2 && Mods_ModelGeometry(0) == DATA * 2048 && Mods_ModelGeometry(1) == 96 * 2048);
    /* Its window holds the record's other stages, the slot its pages. */
    assert(!Mods_ModelPages(0, &count) && count == 0);
    window = Mods_ModelWindow(0, 1);
    assert(window >= 0 && Mods_ModelPending(0));
    assert(Memories_DiscReadSectors(window + 96, 1, got) == 1 && got[0] == 97);
    assert(Memories_DiscReadSectors(window + 275, 1, got) == 1 && got[0] == (unsigned char)(96 + TAIL));
    pages = Mods_ModelPages(0, &count);
    assert(pages && count == 2 && pages[0] == (unsigned char)(96 + TAIL + 1));
    assert(pages[(2 * PAGE - 1) * 2048] == (unsigned char)(96 + TAIL + 2 * PAGE));
    Mods_ModelRebind();   /* a save state's load: the pages again */
    assert(Mods_ModelPages(0, &count) && count == 2);
    Mods_ModelForget(0);
    assert(!Mods_ModelPages(0, &count) && count == 0);
    /* No sector: a native extra reads its base instead (3D Monsters). */
    assert(Mods_ModelSector(0) == -1 && Mods_ModelRead(0x1000000, 1, got) == 0);
    arena = Mods_ModelArena(0, &size);
    assert(arena && (uintptr_t)arena == 0x50000000u && size == 0x180000);
    assert((uintptr_t)Mods_ModelArena(2, NULL) == 0x50000000u + 2 * 0x180000 && !Mods_ModelArena(3, NULL));
    window = Mods_ModelWindow(1, 0);
    assert(window >= 0 && Mods_ModelPending(1) && !Mods_ModelPending(0));
    assert(Memories_DiscReadSectors(window + 5, 1, got) == 1 && got[0] == 0 && got[2047] == 0);
    assert(Memories_DiscReadSectors(window + 96, 1, got) == 1 && got[0] == (unsigned char)(DATA + 1));
    assert(Memories_DiscReadSectors(window + 275, 1, got) == 1 && got[0] == (unsigned char)(DATA + TAIL));
    arena = Mods_ModelArena(1, NULL);
    assert(arena[0] == 1 && arena[(DATA - 1) * 2048] == DATA && arena[DATA * 2048] == 0);
    Mods_ModelParsed(1);
    assert(!Mods_ModelPending(1));
    /* A save state's load reads the window again but leaves the arena, which
       the state brings as the game left it, and the pending flag with it. */
    arena[0] = 0x5A;
    Mods_ModelRebind();
    assert(arena[0] == 0x5A && !Mods_ModelPending(1));
    assert(Memories_DiscReadSectors(window + 96, 1, got) == 1 && got[0] == (unsigned char)(DATA + 1));
    /* The slot loading something else lets the window go. */
    Mods_ModelForget(1);
    windows = Mods_ModelWindows(&size);
    assert(windows[1] == -1 && Memories_DiscReadSectors(window + 96, 1, got) == 1 && got[0] == 0);
    Mods_Shutdown();
    return 0;
}

/* "models": two records of a mod's own, read from its files into the battle
 * slots' windows past the disc and the files it relocates, which the drive
 * reads; a file larger than a record is refused, and one changed since
 * startup is not read. */
static int models_case(void)
{
    static unsigned char one[RECORD], two[RECORD / 2], big[RECORD + 1], whole[2 * RECORD + 2048], got[2048];
    const char *name;
    char path[1024];
    int lba, mod, window, *windows;
    unsigned bytes;
    memset(one, 0x11, sizeof(one));
    for (size_t i = 0; i < sizeof(two); i++) two[i] = (unsigned char)(i / 2048 + 1);
    manifest("m-models", "{\"enabled\":true,\"models\":[{\"name\":\"one\",\"file\":\"one.bin\"},"
                         "{\"name\":\"two\",\"file\":\"two.bin\",\"base\":\"Mystical Elf\"}]}");
    write_file("mods/m-models/one.bin", one, sizeof(one));
    write_file("mods/m-models/two.bin", two, sizeof(two));
    manifest("n-big", "{\"enabled\":true,\"models\":[{\"name\":\"big\",\"file\":\"big.bin\"}]}");
    write_file("mods/n-big/big.bin", big, sizeof(big));   /* one byte past a record */
    /* Loaded after them, MODEL.MRG replaced whole by a larger file, which
       moves past the disc's end: the windows follow it, and stay the
       models'. */
    manifest("o-whole", "{\"enabled\":true,\"data\":[{\"file\":\"\\\\DATA\\\\MODEL.MRG;1\",\"replace\":\"model.bin\"}]}");
    memset(whole, 0x77, sizeof(whole));
    write_file("mods/o-whole/model.bin", whole, sizeof(whole));
    snprintf(path, sizeof(path), "%s/mods", root); assert(!setenv("MEMORIES_MODS_DIR", path, 1));
    assert(!setenv("MEMORIES_USER_DIR", root, 1));
    snprintf(path, sizeof(path), "%s/settings", root); assert(!setenv("MEMORIES_SETTINGS", path, 1));
    Settings_Load(); Mods_Load();
    assert(Mods_Active(find("m-models")) && Mods_RequiresRestart(find("m-models")));
    assert(!Mods_Active(find("n-big")) && strstr(Mods_Status(find("n-big")), "model record"));
    assert(Mods_ModelCount() == 2);
    assert(!Mods_Model(0, &name, NULL, &mod) && !strcmp(name, "one") && mod == find("m-models"));
    assert(!Mods_Model(1, &name, NULL, NULL) && !strcmp(name, "two"));
    assert(Mods_Model(2, NULL, NULL, NULL) == -1);
    assert(Mods_ModelSector(0) == 0x1000000 && Mods_ModelSector(1) == 0x1000000 + 0x114);
    assert(Mods_ModelSector(2) == -1 && Mods_ModelSector(-1) == -1);
    /* o-whole's MODEL.MRG, past the disc's 600 sectors, and each slot's
       window after it and its guard sector. */
    assert(!Memories_DiscFileInfo("\\DATA\\MODEL.MRG;1", &lba, &bytes) && lba == 600 && bytes == sizeof(whole));
    assert(Memories_DiscReadSectors(lba + 2 * 0x114, 1, got) == 1 && got[0] == 0x77);
    window = Mods_ModelWindow(0, 1);
    assert(window == lba + 2 * 0x114 + 1 + 1 && Mods_ModelWindow(1, 0) == window + 0x114);
    assert(Memories_DiscReadSectors(window + 5, 1, got) == 1 && got[0] == 6);              /* two */
    assert(Memories_DiscReadSectors(window + 0x113, 1, got) == 1 && got[0] == 0);          /* past its file */
    assert(Memories_DiscReadSectors(window + 0x114 + 7, 1, got) == 1 && got[0] == 0x11);   /* one */
    assert(Mods_ModelWindow(0, 0) == window && Memories_DiscReadSectors(window + 5, 1, got) == 1 && got[0] == 0x11);
    assert(Mods_ModelWindow(0, 2) == -1 && Mods_ModelWindow(2, 0) == -1);
    /* What save states carry, and reading it back after a load. */
    windows = Mods_ModelWindows(&bytes);
    assert(bytes == 2 * sizeof(int) && windows[0] == -1 && windows[1] == 0);
    windows[0] = 1;
    Mods_ModelRebind();
    assert(Memories_DiscReadSectors(window + 5, 1, got) == 1 && got[0] == 6);
    /* A model file changed since startup, in its bytes or its size, is not
       read: the window is zeros and the loader takes the model's base. */
    /* A native extra reads a model at its sector, within its record. */
    assert(Mods_ModelRead(0x1000000 + 0x114 + 5, 1, got) == 1 && got[0] == 6);
    assert(Mods_ModelRead(0x1000000 + 0x113, 4, got) == 1 && got[0] == 0x11);
    two[5 * 2048] = 0x55;
    write_file("mods/m-models/two.bin", two, sizeof(two));
    assert(Mods_ModelWindow(0, 1) == -1 && windows[0] == -1);
    assert(Memories_DiscReadSectors(window + 5, 1, got) == 1 && got[0] == 0);
    /* Nor by a native extra, whatever sectors it asks for (3D Monsters
       parsed a changed record of the same size and crashed). */
    assert(Mods_ModelRead(0x1000000 + 0x114 + 5, 1, got) == 0);
    assert(Mods_ModelRead(0x1000000 + 0x114 + 100, 1, got) == 0);
    /* Once found changed, it has no sector (3D Monsters then stops asking
       every frame) and is not read again until the next launch, even with
       its file put back. */
    assert(Mods_ModelSector(1) == -1 && Mods_ModelSector(0) == 0x1000000);
    two[5 * 2048] = 6;
    write_file("mods/m-models/two.bin", two, sizeof(two));
    assert(Mods_ModelWindow(0, 1) == -1 && Mods_ModelRead(0x1000000 + 0x114 + 5, 1, got) == 0);
    write_file("mods/m-models/two.bin", two, sizeof(two) - 1);
    assert(Mods_ModelWindow(0, 1) == -1 && Mods_ModelRead(0x1000000 + 0x114, 1, got) == 0);
    assert(Mods_ModelWindow(0, 0) == window);   /* the other model still reads */
    assert(Mods_ModelRead(0x1000000 + 7, 1, got) == 1 && got[0] == 0x11);
    /* A file gone is stale too (one that cannot be opened for another
       reason is tried again: a scanner may be holding it). */
    snprintf(path, sizeof(path), "%s/mods/m-models/one.bin", root);
    assert(!remove(path));
    assert(Mods_ModelRead(0x1000000 + 7, 1, got) == 0 && Mods_ModelSector(0) == -1);
    write_file("mods/m-models/one.bin", one, sizeof(one));
    assert(Mods_ModelWindow(0, 0) == -1);
    Mods_Shutdown();
    return 0;
}

int main(int argc, char **argv)
{
    unsigned char replacement[9000], lower[12000], got[5 * 2048], alias[2048];
    char path[1024];
    int lba, capacity = argc > 1 && !strcmp(argv[1], "capacity");
    unsigned bytes, signature;
    int failed_code = argc > 1 && !strcmp(argv[1], "failed-code");
    DslFILE file;
    DslLOC loc;
    assert(scratch_dir(root, sizeof(root), "memories-disc")); make_dir("mods");
    if (argc > 1 && !strcmp(argv[1], "models")) {
        model_file = 1;
        fixture(600);
        return models_case();
    }
    if (argc > 1 && !strcmp(argv[1], "bigger-models")) {
        model_file = 1;
        fixture(600);
        return bigger_models_case();
    }
    fixture(capacity ? MEMORIES_DISC_MAX_LBA - 2 : 40);
    for (size_t i = 0; i < sizeof(replacement); i++) replacement[i] = (unsigned char)(i * 13 + 7);
    memset(lower, 0xCC, sizeof(lower));
    manifest("a-patch", "{\"enabled\":true,\"restart\":false,\"data\":[{\"file\":\"\\\\DATA\\\\CARD.MRG;1\",\"patch\":[{\"at\":2047,\"bytes\":\"AABBCC\"},{\"at\":8999,\"bytes\":\"DD\"}]}]}");
    if (!capacity) {
        manifest("a-check", "{\"enabled\":true,\"textures\":\"images\"}");
        Mods_SetTexturePack(inspect_startup, unload_pack);
    }
    if (!failed_code) manifest("b-low", "{\"enabled\":true,\"data\":[{\"file\":\"\\\\DATA\\\\CARD.MRG;1\",\"replace\":\"data.bin\"}]}");
    if (!failed_code) write_file("mods/b-low/data.bin", lower, sizeof(lower));
    manifest("c-high", "{\"enabled\":true,\"restart\":false,\"data\":[{\"file\":\"\\\\DATA\\\\CARD.MRG;1\",\"replace\":\"data.bin\"}]}");
    if (failed_code) write_text("mods/c-high/mod.json",
        "{\"enabled\":true,\"library\":\"missing\",\"data\":[{\"file\":\"\\\\DATA\\\\CARD.MRG;1\",\"replace\":\"data.bin\"}]}");
    write_file("mods/c-high/data.bin", replacement, sizeof(replacement));
    manifest("d-bad", "{\"enabled\":true,\"data\":[{\"file\":\"\\\\DATA\\\\CARD.MRG;1\",\"replace\":\"data.bin\",\"patch\":[{\"at\":0,\"bytes\":\"GG\"}]}]}");
    write_file("mods/d-bad/data.bin", lower, sizeof(lower));
    manifest("e-raw", "{\"enabled\":true,\"restart\":false,\"data\":[{\"lba\":20,\"patch\":[{\"at\":1,\"bytes\":\"FE\"},{\"at\":6143,\"bytes\":\"ABCD\"}]}]}");
    manifest("f-second", "{\"enabled\":true,\"data\":[{\"file\":\"\\\\DATA\\\\SECOND.MRG;1\",\"replace\":\"data.bin\"}]}");
    write_file("mods/f-second/data.bin", replacement, sizeof(replacement));
    manifest("g-live", "{\"enabled\":true,\"restart\":false,\"data\":[{\"lba\":32,\"sectors\":1,\"replace\":\"data.bin\"}]}");
    write_text("mods/g-live/data.bin", "first");
    manifest("h-overflow", "{\"enabled\":true,\"data\":[{\"lba\":\"0x100000014\",\"patch\":[{\"at\":0,\"bytes\":\"FF\"}]}]}");
    snprintf(path, sizeof(path), "%s/mods", root); assert(!setenv("MEMORIES_MODS_DIR", path, 1));
    assert(!setenv("MEMORIES_USER_DIR", root, 1));
    snprintf(path, sizeof(path), "%s/settings", root); assert(!setenv("MEMORIES_SETTINGS", path, 1));
    Settings_Load(); Mods_Load();
    assert(!Memories_DiscOriginalFileInfo("\\DATA\\CARD.MRG;1", &lba, &bytes) && lba == 20 && bytes == 5000);
    if (capacity) {
        assert(!Mods_Active(find("c-high")) && strstr(Mods_Status(find("c-high")), "address space"));
        assert(!Mods_Active(find("a-patch")));
        assert(Memories_DiscFileStart("\\DATA\\CARD.MRG;1") == 20);
        Mods_Shutdown(); return 0;
    }
    assert(!Mods_Active(find("h-overflow")));
    if (failed_code) {
        assert(!Mods_Active(find("c-high")) && !Mods_Active(find("a-patch")));
        assert(strstr(Mods_Status(find("a-patch")), "final replacement"));
        assert(!Memories_DiscFileInfo("\\DATA\\CARD.MRG;1", &lba, &bytes) && lba == 40 && bytes == 5000);
        assert(Memories_DiscReadSectors(lba, 1, alias) == 1 && alias[0] == 20 && alias[1] == 0xFE);
        assert(Memories_DiscReadSectors(lba + 3, 1, alias) == 0);
        Mods_Shutdown(); return 0;
    }
    assert(Mods_Active(find("c-high")) && Mods_RequiresRestart(find("c-high")));
    assert(Mods_Active(find("a-patch")) && !Mods_Active(find("d-bad")));
    assert(!Memories_DiscFileInfo("\\DATA\\CARD.MRG;1", &lba, &bytes) && lba == 40 && bytes == sizeof(replacement));
    assert(DsSearchFile(&file, "\\DATA\\CARD.MRG") && file.size == bytes && CdPosToInt_8007E710(&file.pos) == lba);
    assert(Memories_DiscFileStart("\\DATA\\SECOND.MRG;1") == 47); /* lower candidate reserves six + guard */
    assert(Memories_DiscReadSectors(lba, 5, got) == 5 && delivered_lba == lba + 4);
    replacement[1] = 0xFE; replacement[2047] = 0xAA; replacement[2048] = 0xBB;
    replacement[2049] = 0xCC; replacement[6143] = 0xAB; replacement[8999] = 0xDD;
    assert(!memcmp(got, replacement, sizeof(replacement)));
    for (size_t i = sizeof(replacement); i < sizeof(got); i++) assert(got[i] == 0);
    for (int i = 0; i < 3; i++) {
        assert(Memories_DiscReadSectors(20 + i, 1, alias) == 1 && delivered_lba == lba + i);
        assert(!memcmp(alias, got + i * 2048, 2048));
    }
    assert(Memories_DiscReadSectors(23, 1, alias) == 1 && alias[0] == 0xCD && alias[1] == 23);
    assert(got[6144] != 0xCD); /* raw crossing never becomes an expanded-tail patch */
    assert(Memories_DiscReadSectors(lba + 5, 1, alias) == 0); /* unused reservation */
    assert(Memories_DiscReadSectors(lba + 6, 1, alias) == 0); /* guard */
    assert(Memories_DiscReadSectors(-1, 1, alias) == 0);
    assert(Memories_DiscReadSectors(INT32_MAX, 2, alias) == 0);
    expected_lba = lba;
    CdIntToPos_8007E600(lba, &loc);
    DsStartReadySystem(ready, 0); DsPacket(0, &loc, DslReadN, NULL, 0);
    for (int i = 0; i < 3; i++) Memories_DiscService((uint64_t)i * 1000);
    assert(callback_count == 5 && !memcmp(callback_data, got, sizeof(got)));
    signature = Mods_Signature();
    Mods_SetEnabled(find("a-patch"), 0);
    assert(Mods_Signature() != signature);
    assert(Memories_DiscReadSectors(lba + 4, 1, alias) == 1 && alias[807] != 0xDD);
    Mods_SetEnabled(find("a-patch"), 1);
    /* Reactivation changes the manager's activation order/signature. */
    assert(Memories_DiscReadSectors(lba, 5, callback_data) == 5 && !memcmp(callback_data, got, sizeof(got)));
    Mods_SetEnabled(find("c-high"), 0);
    assert(Mods_Active(find("c-high")) && Memories_DiscFileStart("\\DATA\\CARD.MRG;1") == lba);
    assert(Memories_DiscReadSectors(lba, 5, callback_data) == 5 && !memcmp(callback_data, got, sizeof(got)));
    signature = Mods_DiscSignature();
    Mods_SetEnabled(find("g-live"), 0);
    write_text("mods/g-live/data.bin", "other");
    Mods_SetEnabled(find("g-live"), 1);
    assert(Mods_DiscSignature() != signature); /* same name/length, changed backing bytes */
    assert(Memories_DiscReadSectors(32, 1, alias) == 1 && !memcmp(alias, "other", 5));
    DsEndReadySystem();
    CdIntToPos_8007E600(30, &loc);
    DsPacket(DslModeRT, &loc, DslReadS, NULL, 0);
    Memories_DiscService(1000000);
    assert(audio_blocks == 1); /* physical XA subheaders still reach the decoder */
    CdControlB(DslPause, NULL, NULL);
    CdIntToPos_8007E600(MEMORIES_DISC_MAX_LBA, &loc);
    assert(CdPosToInt_8007E710(&loc) == MEMORIES_DISC_MAX_LBA);
    Mods_Shutdown();
    return 0;
}
