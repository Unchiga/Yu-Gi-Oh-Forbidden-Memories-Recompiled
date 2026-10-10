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
 * half-open UV bounds, and survive an ordinary texture-pack reload. */
static void bank_sprites(void)
{
    static const unsigned char red[] = {255, 0, 0, 255};
    static const unsigned char green[] = {0, 255, 0, 255};
    char a[1024], b[1024];
    uint32_t rgb;
    make_dir("bank");
    write_png("bank/a.png", red, 1, 1);
    write_png("bank/b.png", green, 1, 1);
    snprintf(a, sizeof(a), "%s/bank/a.png", root);
    snprintf(b, sizeof(b), "%s/bank/b.png", root);
    TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_STARS);
    TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_DUEL_UI);
    TexturePack_BankSpritesUseOwner(TEXTURE_BANK_OWNER_STARS);
    assert(TexturePack_AddBankSprite(14, 0, 0, 0, 0, 0, 16, 16, a));
    assert(TexturePack_BankSample(14, 0, 0, 0, 1 << 16, 1 << 16, &rgb) == 1 && rgb == 0xff0000);
    TexturePack_BankSpritesUseOwner(TEXTURE_BANK_OWNER_DUEL_UI);
    assert(TexturePack_AddBankSprite(14, 0, 0, 0, 8, 0, 8, 16, b));
    assert(TexturePack_BankSample(14, 0, 0, 0, 10 << 16, 1 << 16, &rgb) == 1 && rgb == 0x00ff00);
    assert(TexturePack_BankSample(14, 0, 0, 0, 1 << 16, 1 << 16, &rgb) == 1 && rgb == 0xff0000);
    TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_DUEL_UI);
    assert(TexturePack_BankSample(14, 0, 0, 0, 10 << 16, 1 << 16, &rgb) == 1 && rgb == 0xff0000);
    TexturePack_BankSpritesUseOwner(TEXTURE_BANK_OWNER_STARS);
    assert(TexturePack_AddBankSprite(14, 0, 0, 0, 16, 0, 16, 16, b));
    assert(TexturePack_BankEntryForRegion(14, 0, 0, 0, 0, 0, 16, 16) == -1);
    TexturePack_Unload();
    assert(TexturePack_BankSample(14, 0, 0, 0, 1 << 16, 1 << 16, &rgb) == 1 && rgb == 0xff0000);
    TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_STARS);
}

int main(void)
{
    char path[1024], problems[256];
    assert(scratch_dir(root, sizeof(root), "memories-texture-pack"));
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
    duplicate_settings();
    bank_sprites();
    field_thumbnail(0);
    field_thumbnail(1);
    made_image();
    stp_palette();
    puts("texture pack tests passed");
    return 0;
}
