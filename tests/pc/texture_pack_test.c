/* A texture pack's manifest entries and the settings that switch them
 * (src/pc/render/texture_pack.c): an entry whose setting is on is used, one
 * whose setting is off is left out, and one naming a setting the mod does
 * not declare is a problem and used. Also exercise field-thumbnail uploads
 * after a duplicate from a battle/effect's full-art record was delivered,
 * images the port makes (a mod card's art), known by their bytes, and a
 * palette whose entries carry the semi-transparency bit (the campaign map's). */
#include "pc/compat/fs.h"
#include "pc/render/texture_pack.h"
#include "pc/mods/json.h"
#include "pc/render/texture_dump.h"
#include "pc/render/soft_gpu.h"
/* Tests must execute their checks in Release CI too. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <png.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "pc/compat/posix.h"
#include "scratch.h"

static char root[SCRATCH_MAX];
static unsigned char disc[3 * 2048];
static int disc_ready;

int Memories_DiscReadSectors(int lba, int sectors, void *out)
{
    if (!disc_ready || lba < 0 || sectors < 0 || lba + sectors > 3) return 0;
    memcpy(out, disc + lba * 2048, (size_t)sectors * 2048);
    return sectors;
}

static int disc_file(const char *path, int *lba, unsigned *size)
{
    assert(!strcmp(path, "\\DATA\\WA_MRG.MRG;1"));
    *lba = 1;
    *size = 2048;
    return 0;
}

static void write_text(const char *relative, const char *text)
{
    char path[1024];
    FILE *file;
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    file = fopen(path, "wb");
    assert(file);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    assert(!fclose(file));
}

static void make_dir(const char *relative)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    assert(!mkdir(path, 0777));
}

static void write_png(const char *relative, const unsigned char *rgba, int width, int height)
{
    char path[1024];
    unsigned char encoded[1024];
    png_alloc_size_t size = sizeof(encoded);
    png_image png = {0};
    FILE *file;
    png.version = PNG_IMAGE_VERSION;
    png.width = (png_uint_32)width;
    png.height = (png_uint_32)height;
    png.format = PNG_FORMAT_RGBA;
    assert(png_image_write_to_memory(&png, encoded, &size, 0, rgba, 0, NULL) && size <= sizeof(encoded));
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    file = fopen(path, "wb");
    assert(file && fwrite(encoded, 1, size, file) == size && !fclose(file));
}

static void write_solid_png(const char *relative, unsigned char red, unsigned char green, unsigned char blue)
{
    unsigned char pixels[4 * 4 * 4], encoded[512];
    png_image png = {0};
    png_alloc_size_t size = sizeof(encoded);
    char path[1024];
    FILE *file;
    int i;
    for (i = 0; i < 16; i++) {
        pixels[i * 4] = red;
        pixels[i * 4 + 1] = green;
        pixels[i * 4 + 2] = blue;
        pixels[i * 4 + 3] = 255;
    }
    png.version = PNG_IMAGE_VERSION;
    png.width = png.height = 4;
    png.format = PNG_FORMAT_RGBA;
    assert(png_image_write_to_memory(&png, encoded, &size, 0, pixels, 0, NULL));
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    file = fopen(path, "wb");
    assert(file);
    assert(fwrite(encoded, 1, size, file) == size);
    assert(!fclose(file));
}

/* A mod whose settings are named after the catalog's own folders. */
static int flat_part(const char *setting, void *context)
{
    assert(context == root);
    return !strcmp(setting, "monster_type") ? 1 : !strcmp(setting, "thumbnails") ? 0 : -1;
}

/* The mod's settings: "on" is on, "off" is off, nothing else is declared. */
static int part(const char *setting, void *context)
{
    assert(context == root);
    return !strcmp(setting, "on") ? 1 : !strcmp(setting, "off") ? 0 : -1;
}

#define ENTRY(file, offset, extra) \
    "{\"file\":\"" file "\",\"archive\":\"WA_MRG.MRG\",\"offset\":" #offset ",\"words\":1,\"rows\":1,\"bpp\":16" extra "}"

static void field_thumbnail(int named)
{
    uint16_t original[704], duplicate[704], copied[704];
    const unsigned char green[] = {0, 255, 0, 255};
    png_image png = {0};
    char path[1024];
    unsigned char encoded[256];
    png_alloc_size_t size;
    FILE *file;
    uint32_t rgb;
    int i, entry, written;
    for (i = 0; i < 640; i++) original[i] = (uint16_t)(1 + i % 63) * 0x101;
    for (i = 0; i < 64; i++) original[640 + i] = (uint16_t)i;
    memcpy(disc + 2048, original, sizeof(original));
    memcpy(disc + 4096, original, sizeof(original));
    disc_ready = 1;
    TextureDump_SetDiscFiles(disc_file);
    if (!named) make_dir("field");
    snprintf(path, sizeof(path), "%s/field/thumb.png", root);
    png.version = PNG_IMAGE_VERSION;
    png.width = png.height = 1;
    png.format = PNG_FORMAT_RGBA;
    /* Through fopen (Memories_Fopen, UTF-8) like the other files here, not
     * libpng's own fopen. */
    size = sizeof(encoded);
    written = png_image_write_to_memory(&png, encoded, &size, 0, green, 0, NULL);
    if (!written) fprintf(stderr, "png: %s\n", png.message);
    assert(written && size <= sizeof(encoded));
    file = fopen(path, "wb");
    assert(file);
    assert(fwrite(encoded, 1, size, file) == size);
    assert(!fclose(file));
    write_text("field/manifest.json", "[{\"file\":\"thumb.png\",\"archive\":\"WA_MRG.MRG\","
               "\"offset\":0,\"words\":20,\"rows\":32,\"bpp\":8,\"clut_offset\":1280,\"clut_entries\":64}]");
    snprintf(path, sizeof(path), "%s/field", root);
    if (named) {
        JsonDocument *doc = Json_Parse("{\"thumbnails/001\":{\"image\":\"thumb.png\"}}", NULL, 0);
        assert(doc);
        assert(TexturePack_LoadAssets(path, Json_Root(doc), 1, NULL, NULL, NULL, 0) == 1);
        Json_Free(doc);
    } else assert(TexturePack_Load(path, 1, NULL, NULL, NULL, 0) == 1);
    TexturePack_Service();

    /* Initial hand/field upload from the thumbnail sector. */
    TextureDump_Delivered(original, sizeof(original), 1, 0);
    SoftGpu_Load(896, 0, 20, 32, original);
    SoftGpu_Load(896, 224, 64, 1, original + 640);
    TexturePack_Service();
    entry = TexturePack_EntryFor(896, 0, 1, 896, 224, 0, 0);
    assert(entry != 0);

    /* Battle/equip/fusion loads read the same bytes from a full-art record.
     * Field updates upload a copy from the deck table or a VRAM readback.
     * The newest delivery names the duplicate, absent from the manifest. */
    memcpy(duplicate, original, sizeof(original));
    TextureDump_Delivered(duplicate, sizeof(duplicate), 2, 0);
    memcpy(copied, duplicate, sizeof(copied));
    SoftGpu_Load(896, 0, 20, 32, copied);
    assert(TexturePack_EntryFor(896, 0, 1, 896, 224, 0, 0) == entry);
    SoftGpu_Load(896, 224, 64, 1, copied + 640);
    assert(TexturePack_EntryFor(896, 0, 1, 896, 224, 0, 0) == entry);
    assert(TextureDump_Sample(896, 0, 1, 10 << 16, 10 << 16, &rgb) == 1 && rgb == 0x00ff00);
    assert(*TextureDump_Cell(896, 0, 0) == (0x8000 | 0x03e0));
    assert(SoftGpu_Vram()[896] == original[0]); /* Native game pixels stay intact. */

    /* Card effects also upload that block as an 8x88 strip, read it back,
     * then reuse the bytes as a field thumbnail and its palette. */
    SoftGpu_Load(832, 256, 8, 88, duplicate);
    SoftGpu_Store(832, 256, 8, 88, copied);
    assert(!memcmp(copied, original, sizeof(copied)));
    SoftGpu_Load(896, 32, 20, 32, copied);
    SoftGpu_Load(896, 229, 64, 1, copied + 640);
    assert(TexturePack_EntryFor(896, 0, 1, 896, 229, 0, 32) == entry);

    /* A matching prefix is not enough, nor is a truncated block. */
    assert(TextureDump_Recall(copied, 640) == 2048);
    assert(TextureDump_Recall(copied, 16) == 0);
    assert(TextureDump_Recall(copied, 704) == 0);
    copied[639] ^= 1;
    assert(TextureDump_Recall(copied, 640) == 0);
    copied[703] ^= 1;
    assert(TextureDump_Recall(copied + 640, 64) == 0);
    TexturePack_Unload();
    assert(TexturePack_EntryFor(896, 0, 1, 896, 224, 0, 0) == 0);
}

/* A field's board picture (TexturePack_AddDiscImageGroup, fields.c):
 * shown only while its group is selected, and still there after the packs
 * reload (a texture-pack mod toggled, an HD pack setting changed). Runs
 * after field_thumbnail, whose disc it reads. */
static void disc_image_reload(void)
{
    uint16_t original[704];
    const unsigned char blue[] = {0, 0, 255, 255};
    char path[1024];
    uint32_t rgb;
    unsigned mark;
    memcpy(original, disc + 2048, sizeof(original));
    make_dir("board");
    write_png("board/board.png", blue, 1, 1);
    snprintf(path, sizeof(path), "%s/board/board.png", root);
    assert(TexturePack_AddDiscImageGroup(7, path, "WA_MRG.MRG", 0, 20, 32, 8, 1280, 64, 20, 0, 40, 0, 0, 256, 254, 1));
    /* A rolled back group (fields.c's five strips, one failing) is gone,
     * through reloads too. */
    mark = TexturePack_EntryCount();
    assert(TexturePack_AddDiscImageGroup(8, path, "WA_MRG.MRG", 0, 20, 32, 8, 1280, 64, 20, 0, 40, 0, 0, 256, 254, 1));
    TexturePack_RemoveEntriesFrom(mark);
    TexturePack_Service();
    TextureDump_Delivered(original, sizeof(original), 1, 0);
    SoftGpu_Load(640, 256, 20, 32, original);
    SoftGpu_Load(640, 500, 64, 1, original + 640);
    TexturePack_Service();
    assert(TexturePack_EntryFor(640, 256, 1, 640, 500, 0, 0) == 0); /* group 0 selected */
    TexturePack_SelectGroup(7);
    assert(TexturePack_EntryFor(640, 256, 1, 640, 500, 0, 0) != 0);
    assert(TextureDump_Sample(640, 256, 1, 10 << 16, 10 << 16, &rgb) == 1 && rgb == 0x0000ff);
    TexturePack_Unload();
    TexturePack_Service();
    TextureDump_Delivered(original, sizeof(original), 1, 0);
    SoftGpu_Load(640, 256, 20, 32, original);
    SoftGpu_Load(640, 500, 64, 1, original + 640);
    TexturePack_Service();
    assert(TexturePack_EntryFor(640, 256, 1, 640, 500, 0, 0) != 0);
    assert(TextureDump_Sample(640, 256, 1, 10 << 16, 10 << 16, &rgb) == 1 && rgb == 0x0000ff);
    TexturePack_SelectGroup(8);
    assert(TexturePack_EntryFor(640, 256, 1, 640, 500, 0, 0) == 0);
    TexturePack_SelectGroup(0);
}

/* A mod card's thumbnail made from an 8x4 PNG whose middle four columns
 * are its picture: red, then blue, black cut off at each side. */
static void made_image(void)
{
    static const unsigned char rgba[4][8][4] = {
        {{0, 0, 0, 255}, {0, 0, 0, 255}, {255, 0, 0, 255}, {255, 0, 0, 255}, {0, 0, 255, 255}, {0, 0, 255, 255}, {0, 0, 0, 255}, {0, 0, 0, 255}},
        {{0, 0, 0, 255}, {0, 0, 0, 255}, {255, 0, 0, 255}, {255, 0, 0, 255}, {0, 0, 255, 255}, {0, 0, 255, 255}, {0, 0, 0, 255}, {0, 0, 0, 255}},
        {{0, 0, 0, 255}, {0, 0, 0, 255}, {255, 0, 0, 255}, {255, 0, 0, 255}, {0, 0, 255, 255}, {0, 0, 255, 255}, {0, 0, 0, 255}, {0, 0, 0, 255}},
        {{0, 0, 0, 255}, {0, 0, 0, 255}, {255, 0, 0, 255}, {255, 0, 0, 255}, {0, 0, 255, 255}, {0, 0, 255, 255}, {0, 0, 0, 255}, {0, 0, 0, 255}},
    };
    uint16_t pixels[640], clut[64], upload[640];
    unsigned char encoded[512];
    png_alloc_size_t size;
    FILE *file;
    png_image png = {0};
    char path[1024];
    uint32_t rgb;
    int i, entry;
    /* The first rows one color (a plain sky): the whole block tells it apart. */
    for (i = 0; i < 640; i++) pixels[i] = i < 60 ? 0x0505 : (uint16_t)((i * 7 + 3) % 63 + 1) * 0x101;
    for (i = 0; i < 64; i++) clut[i] = (uint16_t)(0x4000 + i * 3);
    make_dir("made");
    snprintf(path, sizeof(path), "%s/made/art.png", root);
    png.version = PNG_IMAGE_VERSION;
    png.width = 8;
    png.height = 4;
    png.format = PNG_FORMAT_RGBA;
    size = sizeof(encoded); /* through fopen, as field_thumbnail writes its PNG */
    assert(png_image_write_to_memory(&png, encoded, &size, 0, rgba, 0, NULL) && size <= sizeof(encoded));
    file = fopen(path, "wb");
    assert(file);
    assert(fwrite(encoded, 1, size, file) == size);
    assert(!fclose(file));
    assert(TexturePack_AddMade(pixels, 20, 32, 8, clut, 64, path, 2, 0, 4, 4) == 1);
    assert(TexturePack_AddMade(pixels, 20, 32, 8, clut, 64, path, 2, 0, 4, 4) == 1); /* kept once */
    TexturePack_Service();
    /* Bytes no delivery wrote: known by their content. */
    assert(TextureDump_Recall(pixels, 640) >= TEXTURE_MADE_BASE);
    assert(TextureDump_Recall(clut, 64) >= TEXTURE_MADE_BASE);
    memcpy(upload, pixels, sizeof(upload));
    upload[639] ^= 1;
    assert(TextureDump_Recall(upload, 640) == 0);
    SoftGpu_Load(640, 0, 20, 32, pixels);
    SoftGpu_Load(640, 300, 64, 1, clut);
    TexturePack_Service(); /* reads the PNG the upload asked for */
    entry = TexturePack_EntryFor(640, 0, 1, 640, 300, 0, 0);
    assert(entry != 0);
    assert(TextureDump_Sample(640, 0, 1, 5 << 16, 5 << 16, &rgb) == 1 && rgb == 0xff0000);
    assert(TextureDump_Sample(640, 0, 1, 30 << 16, 5 << 16, &rgb) == 1 && rgb == 0x0000ff);
    /* Another palette over the same words is not the made picture. */
    SoftGpu_Load(640, 301, 64, 1, pixels);
    assert(TexturePack_EntryFor(640, 0, 1, 640, 301, 0, 0) == 0);
    /* Unloading the packs keeps what the port made. */
    TexturePack_Unload();
    TexturePack_Service(); /* sorted in again, the words painted: they ask for the PNG */
    TexturePack_Service(); /* read again */
    assert(TexturePack_EntryFor(640, 0, 1, 640, 300, 0, 0) != 0);
    assert(TextureDump_Sample(640, 0, 1, 30 << 16, 5 << 16, &rgb) == 1 && rgb == 0x0000ff);
}

/* The campaign map's palettes reach VRAM from memory with the
 * semi-transparency bit set on every entry but the first. A pack entry
 * naming the palette on the disc still replaces the texture (the FM
 * Editor's Map tab writes such entries): the palette rule keys on the first
 * entry, which the game leaves as the disc has it. A first entry of its own
 * is another palette. */
static void stp_palette(void)
{
    uint16_t block[48];
    unsigned char encoded[512], pixels[16 * 8 * 4];
    png_alloc_size_t size;
    png_image png = {0};
    char path[1024];
    FILE *file;
    uint32_t rgb;
    int i;
    for (i = 0; i < 32; i++) block[i] = (uint16_t)(0x1234 + i * 0x0111);
    for (i = 0; i < 16; i++) block[32 + i] = (uint16_t)(i * 0x0421);  /* entry 0 clear */
    memcpy(disc + 4096, block, sizeof(block));
    make_dir("stp");
    for (i = 0; i < 16 * 8; i++) {
        pixels[i * 4] = 255;
        pixels[i * 4 + 1] = pixels[i * 4 + 2] = 0;
        pixels[i * 4 + 3] = 255;
    }
    png.version = PNG_IMAGE_VERSION;
    png.width = 16;
    png.height = 8;
    png.format = PNG_FORMAT_RGBA;
    size = sizeof(encoded);
    assert(png_image_write_to_memory(&png, encoded, &size, 0, pixels, 0, NULL) && size <= sizeof(encoded));
    snprintf(path, sizeof(path), "%s/stp/t.png", root);
    file = fopen(path, "wb");
    assert(file);
    assert(fwrite(encoded, 1, size, file) == size);
    assert(!fclose(file));
    /* The archive starts at sector 1: the delivery from sector 2 is offset 2048. */
    write_text("stp/manifest.json", "[{\"file\":\"t.png\",\"archive\":\"WA_MRG.MRG\",\"offset\":2048,\"words\":4,"
               "\"rows\":8,\"bpp\":4,\"clut_offset\":2112,\"clut_entries\":16}]");
    snprintf(path, sizeof(path), "%s/stp", root);
    assert(TexturePack_Load(path, 1, NULL, NULL, NULL, 0) == 1);
    TexturePack_Service();
    TextureDump_Delivered(block, sizeof(block), 2, 0);
    for (i = 1; i < 16; i++) block[32 + i] |= 0x8000;             /* as the map's model setup leaves them */
    SoftGpu_Load(768, 0, 4, 8, block);
    SoftGpu_Load(768, 400, 16, 1, block + 32);
    TexturePack_Service();
    assert(TexturePack_EntryFor(768, 0, 0, 768, 400, 0, 0) != 0);
    assert(TextureDump_Sample(768, 0, 0, 2 << 16, 2 << 16, &rgb) == 1 && rgb == 0xff0000);
    block[32] ^= 0x0001;                                          /* a color of its own: not the disc's */
    SoftGpu_Load(768, 400, 16, 1, block + 32);
    assert(TexturePack_EntryFor(768, 0, 0, 768, 400, 0, 0) == 0);
    TexturePack_Unload();
}

static int named_load(const char *json, unsigned rank, char *problems, size_t size)
{
    JsonDocument *doc = Json_Parse(json, problems, size);
    int result;
    assert(doc);
    result = TexturePack_LoadAssets(root, Json_Root(doc), rank, part, root, problems, size);
    Json_Free(doc);
    return result;
}

static void named_assets(void)
{
    char problems[1024];
    write_text("named.png", "\x89PNG\r\n\x1a\n");
    assert(named_load("{\"card_art/001\":{\"image\":\"named.png\"},"
                      "\"monster_type/dragon\":{\"image\":\"named.png\"},"
                      "\"duel/field_word\":{\"image\":\"named.png\"},"
                      "\"attributes/light\":{\"image\":\"named.png\"}}",
                      1, problems, sizeof(problems)) == 13);
    assert(!problems[0]);
    TexturePack_Unload();
    /* The named sprites: one inside a sheet, cropped to its own rect, and the
     * card panel's digit, which every card UI package repeats. */
    assert(named_load("{\"card_frames/digit_7\":{\"image\":\"named.png\"},"
                      "\"card_frames/label_atk\":{\"image\":\"named.png\"},"
                      "\"monster_type/dragon\":{\"image\":\"named.png\"},"
                      "\"build_deck/cursor_bar\":{\"image\":\"named.png\"},"
                      "\"duel/turn_arrow-00\":{\"image\":\"named.png\"}}",
                      1, problems, sizeof(problems)) == 23);
    assert(!problems[0]);
    TexturePack_Unload();
    assert(named_load("{\"thumbnails/002\":{\"image\":\"named.png\",\"setting\":\"off\"}}",
                      1, problems, sizeof(problems)) == 0 && !problems[0]);
    assert(named_load("{}", 1, problems, sizeof(problems)) == 0 && !problems[0]);
    assert(named_load("[]", 1, problems, sizeof(problems)) == -1 && strstr(problems, "not an object"));
    assert(named_load("{\"typo\":{\"image\":\"named.png\"}}", 1, problems, sizeof(problems)) == -1);
    assert(strstr(problems, "unknown asset") && strstr(problems, "typo"));
    assert(named_load("{\"thumbnails/002\":{\"image\":\"../named.png\"}}",
                      1, problems, sizeof(problems)) == -1 && strstr(problems, "outside"));
    assert(named_load("{\"thumbnails/002\":{\"image\":\"missing.png\"}}",
                      1, problems, sizeof(problems)) == -1 && strstr(problems, "could not be read"));
    assert(named_load("{\"thumbnails/002\":42}", 1, problems, sizeof(problems)) == -1);
    assert(strstr(problems, "invalid asset image object"));
    TexturePack_Unload();
}

/* "assets": "<directory>": the PNGs are named by their path under it. */
static void named_folder(void)
{
    char path[1024], problems[1024];
    make_dir("ship");
    make_dir("ship/monster_type");
    make_dir("ship/card_frames");
    make_dir("ship/thumbnails");
    make_dir("ship/nested");
    write_text("ship/monster_type/dragon.png", "\x89PNG\r\n\x1a\n");
    write_text("ship/card_frames/digit_7.png", "\x89PNG\r\n\x1a\n");
    write_text("ship/thumbnails/001.png", "\x89PNG\r\n\x1a\n");
    write_text("ship/monster_type/dragn.png", "\x89PNG\r\n\x1a\n");   /* a typo: reported */
    write_text("ship/monster_type/notes.txt", "not a picture");                /* ignored */
    write_text("ship/nested/deep.png", "\x89PNG\r\n\x1a\n");           /* no such name */
    snprintf(path, sizeof(path), "%s/ship", root);
    /* digit_7 is in all ten card UI packages, the other two once each. */
    assert(TexturePack_LoadAssetFolder(path, 1, NULL, NULL, problems, sizeof(problems)) == 12);
    assert(strstr(problems, "unknown asset") && strstr(problems, "dragn"));
    TexturePack_Unload();
    /* A directory named after one of the mod's settings switches everything
     * under it. "off" is the setting part() reports as switched off; "thumbnails"
     * is also a family, so the whole path still wins there. */
    make_dir("ship/off");
    make_dir("ship/off/card_frames");
    make_dir("ship/on");
    make_dir("ship/on/thumbnails");
    write_text("ship/off/card_frames/digit_0.png", "\x89PNG\r\n\x1a\n");
    write_text("ship/on/thumbnails/003.png", "\x89PNG\r\n\x1a\n");
    snprintf(path, sizeof(path), "%s/ship", root);
    /* The four that loaded before, plus thumbnails/003 under the "on" setting;
     * the ten digit_0 readings under "off" are left out, not reported. */
    assert(TexturePack_LoadAssetFolder(path, 1, part, root, problems, sizeof(problems)) == 13);
    assert(strstr(problems, "unknown asset") && !strstr(problems, "digit_0"));
    TexturePack_Unload();

    /* A setting named after the folder switches it with nothing to nest:
     * "on" and "off" above are the mod's own names, but a mod may just as
     * well call a setting "thumbnails" and switch assets/thumbnails/... */
    make_dir("flat");
    make_dir("flat/thumbnails");
    make_dir("flat/monster_type");
    write_text("flat/thumbnails/004.png", "\x89PNG\r\n\x1a\n");
    write_text("flat/monster_type/zombie.png", "\x89PNG\r\n\x1a\n");
    snprintf(path, sizeof(path), "%s/flat", root);
    /* flat_part() declares "thumbnails" off and "monster_type" on. */
    assert(TexturePack_LoadAssetFolder(path, 1, flat_part, root, problems, sizeof(problems)) == 1);
    assert(!strstr(problems, "unknown"));
    TexturePack_Unload();

    /* An empty directory, and one that is not there at all, are not errors
     * of the pack's: nothing is replaced. */
    make_dir("bare");
    snprintf(path, sizeof(path), "%s/bare", root);
    assert(TexturePack_LoadAssetFolder(path, 1, NULL, NULL, problems, sizeof(problems)) == 0);
    snprintf(path, sizeof(path), "%s/no-such-directory", root);
    assert(TexturePack_LoadAssetFolder(path, 1, NULL, NULL, problems, sizeof(problems)) == 0);
    TexturePack_Unload();
}

/* Two 4-bit images that share a VRAM word: the first owns the word's lower
 * texels and the second its upper ones, as card_frames/digit_0 and digit_1
 * do (words 4-5 and 5-6 of the card panel). Both must reach their own
 * texels; a word belongs to one entry, so the renderer has to decide per
 * texel, not per word. */
static void shared_word(void)
{
    char path[1024];
    png_image png;
    unsigned char pixels[6 * 4], encoded[4096];
    png_alloc_size_t size;
    FILE *file;
    int i, x;
    make_dir("share");
    for (i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s/share/%s.png", root, i ? "b" : "a");
        memset(&png, 0, sizeof(png));
        png.version = PNG_IMAGE_VERSION;
        png.width = 6;
        png.height = 1;
        png.format = PNG_FORMAT_RGBA;
        for (x = 0; x < 6; x++) {
            pixels[x * 4] = (unsigned char)(i ? 0 : 255);
            pixels[x * 4 + 1] = 0;
            pixels[x * 4 + 2] = (unsigned char)(i ? 255 : 0);
            pixels[x * 4 + 3] = 255;
        }
        /* Through fopen (Memories_Fopen, UTF-8) like the other files here,
         * not libpng's own fopen, which cannot open a path of this kind. */
        size = sizeof(encoded);
        assert(png_image_write_to_memory(&png, encoded, &size, 0, pixels, 0, NULL));
        assert(size <= sizeof(encoded));
        file = fopen(path, "wb");
        assert(file);
        assert(fwrite(encoded, 1, size, file) == size);
        assert(!fclose(file));
    }
    /* a: words 0-1, texels 0-5.  b: words 1-2, texels 6-11. Word 1 is shared,
     * holding 4 and 5 of a and 6 and 7 of b. */
    write_text("share/manifest.json",
               "[{\"file\":\"a.png\",\"archive\":\"WA_MRG.MRG\",\"offset\":0,\"words\":2,\"rows\":1,"
               "\"bpp\":4,\"stride\":64,\"clut_offset\":1536,\"clut_entries\":16,\"crop_left\":0,\"width\":6},"
               "{\"file\":\"b.png\",\"archive\":\"WA_MRG.MRG\",\"offset\":2,\"words\":2,\"rows\":1,"
               "\"bpp\":4,\"stride\":64,\"clut_offset\":1536,\"clut_entries\":16,\"crop_left\":2,\"width\":6}]");
    memset(disc, 0, sizeof(disc));
    for (i = 0; i < 2048; i++) disc[2048 + i] = (unsigned char)(0x11 * (i % 15 + 1));
    for (i = 0; i < 16; i++) {          /* a palette whose every entry shows */
        disc[2048 + 1536 + i * 2] = (unsigned char)(i * 2 + 1);
        disc[2048 + 1536 + i * 2 + 1] = 0x10;
    }
    disc_ready = 1;
    TextureDump_SetDiscFiles(disc_file);
    snprintf(path, sizeof(path), "%s/share", root);
    assert(TexturePack_Load(path, 1, NULL, NULL, NULL, 0) == 2);
    TexturePack_Service();
    TextureDump_Delivered(disc + 2048, 2048, 1, 0);
    SoftGpu_Load(0, 0, 64, 16, (const uint16_t *)(disc + 2048));
    TexturePack_Service();
    for (i = 0; i < 12; i++) {
        uint16_t cell = *TextureDump_Cell(i / 4, 0, i % 4);
        if (!cell) fprintf(stderr, "shared_word: texel %d was not replaced\n", i);
        assert(cell);
    }
    /* The scaled picture reads the PNGs themselves: each texel must come from
     * the image that owns it, including the two of a and the two of b that
     * share word 1. */
    assert(TextureDump_Prepare(0, 0, 0, 0, 12, 0, 0) > 0);
    for (i = 0; i < 12; i++) {
        uint32_t rgb = 0;
        int got = TextureDump_Sample(0, 0, 0, i << 16, 0, &rgb);
        if (got != 1) fprintf(stderr, "shared_word: texel %d is not sampled (%d)\n", i, got);
        assert(got == 1);
        if (rgb != (i < 6 ? 0xff0000u : 0x0000ffu))
            fprintf(stderr, "shared_word: texel %d sampled %06x\n", i, rgb);
        assert(rgb == (i < 6 ? 0xff0000u : 0x0000ffu));
    }
    TexturePack_Unload();
    disc_ready = 0;
}

/* The pack's images are read on other threads (texture_pack.c, Decode) and
 * still given by the Service after the upload or the draw that wanted them:
 * a sheet's head (a, through palette 1536) and another block naming the
 * same PNG with the same geometry (c) share one read; the head's other
 * reading (b, palette 1568) is read ahead, so the Service that gives it has
 * nothing left to read; and packs unloaded while a read ahead is under way
 * leave nothing behind when loaded again. */
static void read_ahead(void)
{
    static const char manifest[] =
        "[{\"file\":\"a.png\",\"archive\":\"WA_MRG.MRG\",\"offset\":0,\"words\":2,\"rows\":1,"
        "\"bpp\":4,\"stride\":64,\"clut_offset\":1536,\"clut_entries\":16,\"crop_left\":0,\"width\":6},"
        "{\"file\":\"b.png\",\"archive\":\"WA_MRG.MRG\",\"offset\":0,\"words\":2,\"rows\":1,"
        "\"bpp\":4,\"stride\":64,\"clut_offset\":1568,\"clut_entries\":16,\"crop_left\":0,\"width\":6},"
        "{\"file\":\"a.png\",\"archive\":\"WA_MRG.MRG\",\"offset\":64,\"words\":2,\"rows\":1,"
        "\"bpp\":4,\"stride\":64,\"clut_offset\":1536,\"clut_entries\":16,\"crop_left\":0,\"width\":6}]";
    unsigned char red[6 * 4], green[6 * 4];
    unsigned now, ahead, shared, now0, ahead0, shared0;
    char path[1024];
    uint32_t rgb;
    int i, threads, round;
    for (i = 0; i < 6; i++) {
        red[i * 4] = 255, red[i * 4 + 1] = 0, red[i * 4 + 2] = 0, red[i * 4 + 3] = 255;
        green[i * 4] = 0, green[i * 4 + 1] = 255, green[i * 4 + 2] = 0, green[i * 4 + 3] = 255;
    }
    make_dir("ahead");
    write_png("ahead/a.png", red, 6, 1);
    write_png("ahead/b.png", green, 6, 1);
    write_text("ahead/manifest.json", manifest);
    memset(disc, 0, sizeof(disc));
    for (i = 0; i < 2048; i++) disc[2048 + i] = (unsigned char)(0x11 * (i % 15 + 1));
    for (i = 0; i < 32; i++) {          /* two palettes whose every entry shows */
        disc[2048 + 1536 + i * 2] = (unsigned char)(i * 2 + 1);
        disc[2048 + 1536 + i * 2 + 1] = 0x10;
    }
    disc_ready = 1;
    TextureDump_SetDiscFiles(disc_file);
    snprintf(path, sizeof(path), "%s/ahead", root);
    for (round = 0; round < 2; round++) {
        assert(TexturePack_Load(path, 1, NULL, NULL, NULL, 0) == 3);
        TexturePack_Service();
        TexturePack_DecodeStats(&now0, &ahead0, &shared0, &threads);
        TextureDump_Delivered(disc + 2048, 2048, 1, 0);
        SoftGpu_Load(0, 0, 64, 16, (const uint16_t *)(disc + 2048));
        TexturePack_Service();
        /* Both heads have their image now, read once between them. */
        assert(*TextureDump_Cell(0, 0, 0) && *TextureDump_Cell(32, 0, 0));
        TexturePack_DecodeStats(&now, &ahead, &shared, &threads);
        assert(threads > 0);
        assert(now == now0 + 1 && shared == shared0 + 1 && ahead == ahead0);
        if (!round) {
            TexturePack_Unload();   /* b may be under way: its thread drops it */
            continue;
        }
        TexturePack_DecodeSettle();
        /* b is drawn through its palette: nothing yet, as before, the next
         * Service gives what was read ahead. */
        assert(TextureDump_Prepare(0, 0, 0, 16, 12, 0, 0) == 0);
        TexturePack_Service();
        TexturePack_DecodeStats(&now, &ahead, &shared, &threads);
        assert(now == now0 + 1 && ahead == ahead0 + 1 && shared == shared0 + 1);
        assert(TextureDump_Prepare(0, 0, 0, 16, 12, 0, 0) == 2);
        assert(TextureDump_Sample(0, 0, 0, 1 << 16, 0, &rgb) == 1 && rgb == 0x00ff00);
        assert(TextureDump_Prepare(0, 0, 0, 0, 12, 0, 0) == 1);
        assert(TextureDump_Sample(0, 0, 0, 1 << 16, 0, &rgb) == 1 && rgb == 0xff0000);
    }
    TexturePack_Unload();
    disc_ready = 0;
}

/* A wide replacement and one replacement per row describe the same 4x4
 * upload. A later-loaded pack replaces every covered row, regardless of
 * whether its entry is wide or one-row specific. */
static void rank_preference(void)
{
    char wide[1024], rows[1024];
    uint16_t upload[16] = {0};
    int i, y;
    make_dir("rank");
    make_dir("rank/wide");
    make_dir("rank/rows");
    write_solid_png("rank/wide/wide.png", 255, 0, 0);
    for (i = 0; i < 4; i++) {
        char name[64];
        snprintf(name, sizeof(name), "rank/rows/row%d.png", i);
        write_solid_png(name, 0, 255, 0);
    }
    write_text("rank/wide/manifest.json",
               "[{\"file\":\"wide.png\",\"archive\":\"WA_MRG.MRG\",\"offset\":0,\"words\":4,\"rows\":4,\"bpp\":16}]");
    write_text("rank/rows/manifest.json",
               "[{\"file\":\"row0.png\",\"archive\":\"WA_MRG.MRG\",\"offset\":0,\"words\":4,\"rows\":1,\"bpp\":16,\"row_offsets\":[0]},"
               "{\"file\":\"row1.png\",\"archive\":\"WA_MRG.MRG\",\"offset\":8,\"words\":4,\"rows\":1,\"bpp\":16,\"row_offsets\":[0]},"
               "{\"file\":\"row2.png\",\"archive\":\"WA_MRG.MRG\",\"offset\":16,\"words\":4,\"rows\":1,\"bpp\":16,\"row_offsets\":[0]},"
               "{\"file\":\"row3.png\",\"archive\":\"WA_MRG.MRG\",\"offset\":24,\"words\":4,\"rows\":1,\"bpp\":16,\"row_offsets\":[0]}]");
    snprintf(wide, sizeof(wide), "%s/rank/wide", root);
    snprintf(rows, sizeof(rows), "%s/rank/rows", root);
    memset(disc, 0, sizeof(disc));
    memcpy(disc + 2048, upload, sizeof(upload));
    disc_ready = 1;
    TextureDump_SetDiscFiles(disc_file);

    /* The later per-row pack wins the offset-zero tie: all rows are green. */
    assert(TexturePack_Load(wide, 1, NULL, NULL, NULL, 0) == 1);
    assert(TexturePack_Load(rows, 2, NULL, NULL, NULL, 0) == 4);
    TexturePack_Service();
    TextureDump_Delivered(upload, sizeof(upload), 1, 0);
    SoftGpu_Load(0, 0, 4, 4, upload);
    TexturePack_Service();
    for (y = 0; y < 4; y++) assert(*TextureDump_Cell(0, y, 0) == (0x8000 | 0x03e0));
    TexturePack_Unload();

    /* The later wide pack overrides every earlier per-row replacement. */
    assert(TexturePack_Load(rows, 1, NULL, NULL, NULL, 0) == 4);
    assert(TexturePack_Load(wide, 2, NULL, NULL, NULL, 0) == 1);
    TexturePack_Service();
    TextureDump_Delivered(upload, sizeof(upload), 1, 0);
    SoftGpu_Load(0, 0, 4, 4, upload);
    TexturePack_Service();
    for (y = 0; y < 4; y++) assert(*TextureDump_Cell(0, y, 0) == (0x8000 | 0x001f));
    TexturePack_Unload();
    disc_ready = 0;
}

/* A later wide image must still override all rows when an earlier pack has
 * more entries than the former bounded reverse lookup considered. */
static void large_rank_preference(void)
{
    char wide[1024], rows[1024], manifest[32768];
    uint16_t upload[4 * 128] = {0};
    size_t used;
    int i, y;
    make_dir("large-rank");
    make_dir("large-rank/wide");
    make_dir("large-rank/rows");
    write_solid_png("large-rank/wide/wide.png", 255, 0, 0);
    used = (size_t)snprintf(manifest, sizeof(manifest), "[");
    for (i = 0; i < 128; i++) {
        char name[64];
        int written;
        snprintf(name, sizeof(name), "large-rank/rows/row%d.png", i);
        write_solid_png(name, 0, 255, 0);
        written = snprintf(manifest + used, sizeof(manifest) - used,
                           "%s{\"file\":\"row%d.png\",\"archive\":\"WA_MRG.MRG\",\"offset\":%d,\"words\":4,\"rows\":1,\"bpp\":16}",
                           i ? "," : "", i, i * 8);
        assert(written > 0 && (size_t)written < sizeof(manifest) - used);
        used += (size_t)written;
    }
    assert(used + 2 <= sizeof(manifest));
    strcpy(manifest + used, "]");
    write_text("large-rank/wide/manifest.json",
               "[{\"file\":\"wide.png\",\"archive\":\"WA_MRG.MRG\",\"offset\":0,\"words\":4,\"rows\":128,\"bpp\":16}]");
    write_text("large-rank/rows/manifest.json", manifest);
    snprintf(wide, sizeof(wide), "%s/large-rank/wide", root);
    snprintf(rows, sizeof(rows), "%s/large-rank/rows", root);
    memset(disc, 0, sizeof(disc));
    memcpy(disc + 2048, upload, sizeof(upload));
    disc_ready = 1;
    TextureDump_SetDiscFiles(disc_file);
    assert(TexturePack_Load(rows, 1, NULL, NULL, NULL, 0) == 128);
    assert(TexturePack_Load(wide, 2, NULL, NULL, NULL, 0) == 1);
    TexturePack_Service();
    TextureDump_Delivered(upload, sizeof(upload), 1, 0);
    SoftGpu_Load(0, 0, 4, 128, upload);
    TexturePack_Service();
    for (y = 0; y < 128; y++) assert(*TextureDump_Cell(0, y, 0) == (0x8000 | 0x001f));
    TexturePack_Unload();
    disc_ready = 0;
}

/* The same asset shipped twice, once under a setting that is on and once
 * under one that is off: the enabled copy must be the one used. */
static void duplicate_settings(void)
{
    char path[1024], problems[512];
    make_dir("dup");
    make_dir("dup/on");
    make_dir("dup/on/monster_type");
    make_dir("dup/off");
    make_dir("dup/off/monster_type");
    write_text("dup/on/monster_type/dragon.png", "\x89PNG\r\n\x1a\n");
    write_text("dup/off/monster_type/dragon.png", "\x89PNG\r\n\x1a\n");
    snprintf(path, sizeof(path), "%s/dup", root);
    assert(TexturePack_LoadAssetFolder(path, 1, part, root, problems, sizeof(problems)) == 1);
    TexturePack_Unload();
}

/* Bank-backed pictures have independent owners, newest-overlap priority,
 * half-open UV bounds, their palette, one read of a PNG for all sprites of
 * it, levels for drawing small, and survive an ordinary texture-pack reload
 * without touching its generation. */
static void bank_sprites(void)
{
    static const unsigned char red[] = {255, 0, 0, 255};
    static const unsigned char green[] = {0, 255, 0, 255};
    unsigned char big[8 * 8 * 4];
    char a[1024], b[1024], c[1024];
    uint32_t rgb;
    unsigned generation, key;
    const unsigned char *pixels;
    int i, w, h;
    make_dir("bank");
    write_png("bank/a.png", red, 1, 1);
    write_png("bank/b.png", green, 1, 1);
    /* 8x8: left half white, right half clear; level 1 4x4, level 3 1x1. */
    for (i = 0; i < 64; i++) {
        unsigned char *p = &big[i * 4];
        p[0] = p[1] = p[2] = i % 8 < 4 ? 255 : 0;
        p[3] = i % 8 < 4 ? 255 : 0;
    }
    write_png("bank/c.png", big, 8, 8);
    snprintf(a, sizeof(a), "%s/bank/a.png", root);
    snprintf(b, sizeof(b), "%s/bank/b.png", root);
    snprintf(c, sizeof(c), "%s/bank/c.png", root);
    TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_STARS);
    TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_DUEL_UI);
    generation = TexturePack_Generation();
    TexturePack_BankSpritesUseOwner(TEXTURE_BANK_OWNER_STARS);
    assert(TexturePack_AddBankSprite(14, 0, 0, 0, 0, 257, 0, 0, 16, 16, a));
    assert(TexturePack_BankSample(14, 0, 0, 0, 0, 257, 1 << 16, 1 << 16, 2, &rgb) == 1 && rgb == 0xff0000);
    /* Another palette (an effect's pass) draws the bank's own texels. */
    assert(TexturePack_BankSample(14, 0, 0, 0, 544, 255, 1 << 16, 1 << 16, 2, &rgb) == 0);
    assert(!TexturePack_BankEntryFor(14, 0, 0, 0, 544, 255, 1, 1));
    TexturePack_BankSpritesUseOwner(TEXTURE_BANK_OWNER_DUEL_UI);
    assert(TexturePack_AddBankSprite(14, 0, 0, 0, 0, 257, 8, 0, 8, 16, b));
    /* Cache the older sprite first: the overlap must still choose newer. */
    assert(TexturePack_BankSample(14, 0, 0, 0, 0, 257, 1 << 16, 1 << 16, 2, &rgb) == 1 && rgb == 0xff0000);
    assert(TexturePack_BankSample(14, 0, 0, 0, 0, 257, 10 << 16, 1 << 16, 2, &rgb) == 1 && rgb == 0x00ff00);
    assert(TexturePack_BankSample(14, 0, 0, 0, 0, 257, 1 << 16, 1 << 16, 2, &rgb) == 1 && rgb == 0xff0000);
    TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_DUEL_UI);
    assert(TexturePack_BankSample(14, 0, 0, 0, 0, 257, 10 << 16, 1 << 16, 2, &rgb) == 1 && rgb == 0xff0000);
    TexturePack_BankSpritesUseOwner(TEXTURE_BANK_OWNER_STARS);
    assert(TexturePack_AddBankSprite(14, 0, 0, 0, 0, 257, 16, 0, 16, 16, b));
    assert(TexturePack_BankEntryForRegion(14, 0, 0, 0, 0, 257, 0, 0, 16, 16) == -1);
    /* Two sprites of one PNG share it; placing one anew leaves the pack's
     * generation (its textures) alone. */
    TexturePack_BankSpritesUseOwner(TEXTURE_BANK_OWNER_LAYOUT_FRAME);
    assert(TexturePack_AddBankSpriteCrop(13, 0, 0, 1, 0, 254, 0, 0, 4, 8, c, 0, 0, 4, 8));
    assert(TexturePack_AddBankSpriteCrop(13, 128, 0, 1, 0, 254, 0, 0, 4, 8, c, 4, 0, 4, 8));
    key = TexturePack_BankEntryKey(TexturePack_BankEntryFor(13, 0, 0, 1, 0, 254, 0, 0));
    assert(key && key == TexturePack_BankEntryKey(TexturePack_BankEntryFor(13, 128, 0, 1, 0, 254, 0, 0)));
    assert(TexturePack_BankEntryKey(TexturePack_BankEntryFor(14, 0, 0, 0, 0, 257, 0, 0)) != key);
    assert(TexturePack_AddBankSpriteCrop(13, 0, 0, 1, 0, 254, 0, 0, 4, 8, a, 0, 0, 0, 0));
    assert(TexturePack_AddBankSpriteCrop(13, 0, 0, 1, 0, 254, 0, 0, 4, 8, c, 0, 0, 4, 8));
    assert(TexturePack_Generation() == generation && TexturePack_BankKeyLive(key));
    /* A 4x8 crop on 4x8 texels has a PNG pixel to a texel: level 0 at any
     * scale. The whole 8x8 on 2x2 texels is four to one: level 2 at 1x,
     * level 1 at 2x, level 0 at 4x. */
    i = TexturePack_BankEntryFor(13, 0, 0, 1, 0, 254, 0, 0);
    assert(TexturePack_BankEntryLevelFor(i, 1) == 0 && TexturePack_BankEntryLevelFor(i, 2) == 0);
    assert(TexturePack_BankEntryLevel(i, 1, &pixels, &w, &h) && w == 4 && h == 4);
    assert(pixels[0] == 255 && pixels[3] == 255 && pixels[2 * 4 + 3] == 0);
    assert(TexturePack_BankEntryLevel(i, 3, &pixels, &w, &h) && w == 1 && h == 1);
    /* Half covered: white at half alpha, the clear texels' black unmixed. */
    assert(pixels[0] == 255 && pixels[3] == 127);
    assert(!TexturePack_BankEntryLevel(i, 4, &pixels, &w, &h));
    assert(TexturePack_AddBankSpriteCrop(13, 256, 0, 1, 0, 254, 0, 0, 2, 2, c, 0, 0, 8, 8));
    i = TexturePack_BankEntryFor(13, 256, 0, 1, 0, 254, 0, 0);
    assert(TexturePack_BankEntryLevelFor(i, 1) == 2 && TexturePack_BankEntryLevelFor(i, 2) == 1);
    assert(TexturePack_BankEntryLevelFor(i, 4) == 0);
    /* A level's size is max(1, floor(level 0's size >> level)), the one
     * bind_pack_entry's glTexImage2D calls build a real GL mip chain from --
     * rounding a level's own half up instead (6 -> "4" at level 2) agrees
     * with that for a power-of-two image (where up and down never differ)
     * but not for this one: level 2 of 6x6 must be 1x1, not 2x2, or the
     * uploaded texture is mipmap-incomplete and GL_NEAREST_MIPMAP_NEAREST
     * samples it as nothing. */
    {
        unsigned char six[6 * 6 * 4];
        char d[1024];
        for (i = 0; i < 6 * 6; i++) { unsigned char *p = &six[i * 4]; p[0] = p[1] = p[2] = p[3] = 255; }
        write_png("bank/d.png", six, 6, 6);
        snprintf(d, sizeof(d), "%s/bank/d.png", root);
        assert(TexturePack_AddBankSpriteCrop(13, 384, 0, 1, 0, 254, 0, 0, 6, 6, d, 0, 0, 6, 6));
        i = TexturePack_BankEntryFor(13, 384, 0, 1, 0, 254, 0, 0);
        assert(TexturePack_BankEntryLevel(i, 2, &pixels, &w, &h) && w == 1 && h == 1);
    }
    TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_LAYOUT_FRAME);
    TexturePack_Unload();
    assert(TexturePack_BankSample(14, 0, 0, 0, 0, 257, 1 << 16, 1 << 16, 2, &rgb) == 1 && rgb == 0xff0000);
    TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_STARS);
    /* A PNG read once is kept by its path even after no sprite shows it (the
     * spare images), so one edited on disk is read again only once the spares
     * are forgotten; one a sprite still shows is not dropped. */
    write_png("bank/e.png", red, 1, 1);
    snprintf(a, sizeof(a), "%s/bank/e.png", root);
    TexturePack_BankSpritesUseOwner(TEXTURE_BANK_OWNER_LAYOUT_FRAME);
    assert(TexturePack_AddBankSprite(13, 0, 0, 1, 0, 254, 0, 0, 4, 4, a));
    TexturePack_BankImagesForgetSpare();   /* still shown: kept */
    assert(TexturePack_BankSample(13, 0, 0, 1, 0, 254, 1 << 16, 1 << 16, 2, &rgb) == 1 && rgb == 0xff0000);
    TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_LAYOUT_FRAME);
    write_png("bank/e.png", green, 1, 1);
    assert(TexturePack_AddBankSprite(13, 0, 0, 1, 0, 254, 0, 0, 4, 4, a));
    assert(TexturePack_BankSample(13, 0, 0, 1, 0, 254, 1 << 16, 1 << 16, 2, &rgb) == 1 && rgb == 0xff0000);
    TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_LAYOUT_FRAME);
    TexturePack_BankImagesForgetSpare();
    assert(TexturePack_AddBankSprite(13, 0, 0, 1, 0, 254, 0, 0, 4, 4, a));
    assert(TexturePack_BankSample(13, 0, 0, 1, 0, 254, 1 << 16, 1 << 16, 2, &rgb) == 1 && rgb == 0x00ff00);
    TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_LAYOUT_FRAME);
    TexturePack_BankImagesForgetSpare();
    TexturePack_BankImagesForgetSpare();   /* none left: nothing to do */
}

int main(void)
{
    char path[1024], problems[256];
    unsigned now, ahead, shared, made_now, made_ahead, made_shared;
    int threads;
    assert(scratch_dir(root, sizeof(root), "memories-texture-pack"));
    /* No pack image wanted yet: no thread to read one. */
    TexturePack_DecodeStats(&now, &ahead, &shared, &threads);
    assert(threads == 0);
    make_dir("pack");
    write_text("pack/a.png", "\x89PNG\r\n\x1a\n");
    write_text("pack/manifest.json",
               "[" ENTRY("a.png", 0, ",\"setting\":\"on\"") "," ENTRY("a.png", 2, ",\"setting\":\"off\"") ","
               ENTRY("a.png", 4, ",\"setting\":\"nope\"") "," ENTRY("a.png", 6, "") "]");
    snprintf(path, sizeof(path), "%s/pack", root);
    /* on, the undeclared one and the plain one; off is left out */
    assert(TexturePack_Load(path, 1, part, root, problems, sizeof(problems)) == 3);
    assert(!strcmp(problems, "1 image names a setting the mod does not declare (first: nope)"));
    TexturePack_Unload();
    /* Without the mod's settings every "setting" is undeclared: all used. */
    assert(TexturePack_Load(path, 1, NULL, NULL, problems, sizeof(problems)) == 4);
    assert(!strcmp(problems, "3 images name a setting the mod does not declare (first: on)"));
    TexturePack_Unload();
    /* Every part switched off is nothing wrong with the pack: 0, not -1. */
    make_dir("off");
    write_text("off/a.png", "\x89PNG\r\n\x1a\n");
    write_text("off/manifest.json", "[" ENTRY("a.png", 0, ",\"setting\":\"off\"") "]");
    snprintf(path, sizeof(path), "%s/off", root);
    assert(TexturePack_Load(path, 1, part, root, problems, sizeof(problems)) == 0 && !problems[0]);
    write_text("off/manifest.json", "[]");
    assert(TexturePack_Load(path, 1, part, root, problems, sizeof(problems)) == -1);
    TexturePack_Unload();
    named_assets();
    named_folder();
    shared_word();
    read_ahead();
    rank_preference();
    large_rank_preference();
    duplicate_settings();
    bank_sprites();
    field_thumbnail(0);
    field_thumbnail(1);
    disc_image_reload();
    /* A made image is read where it is wanted, by none of the threads. */
    TexturePack_DecodeStats(&made_now, &made_ahead, &made_shared, &threads);
    made_image();
    TexturePack_DecodeStats(&now, &ahead, &shared, &threads);
    assert(now == made_now && ahead == made_ahead && shared == made_shared);
    stp_palette();
    puts("texture pack tests passed");
    return 0;
}
