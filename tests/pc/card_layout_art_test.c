/* The card frames' pictures in the VRAM bank (src/pc/cards/card_layout_art.c),
 * against the real texture pack (src/pc/render/texture_pack.c):
 *
 *  - the card-viewer back: going from a style with a small back to one with a
 *    larger back and then to the first again shows each style's own HD picture
 *    in every part of its box, not the small one's in the corner;
 *  - a mod turned off and on again (Mods_Changes) with its PNGs edited in
 *    between draws the new pictures, the decoded pixels, the bank's texels and
 *    the HD copy alike;
 *  - with no mod change nothing is read twice. */
#include "pc/render/texture_pack.h"
#include "pc/render/soft_gpu.h"
#include "pc/debug/log.h"
#include "pc/compat/fs.h"
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

#include "../../src/pc/cards/card_layout_art.c"

static char root[SCRATCH_MAX];

/* --- stand-ins for what card_layout_art.c reads ------------------------- */

static char back_file[1024], hand_file[1024];
static int style_w, style_h;
static unsigned mod_changes;
static int index_reads;           /* CardArt_IndexedImage calls */
static unsigned char disk_index;  /* what a "decoded" picture holds right now */

int Memories_DiscReadSectors(int lba, int sectors, void *out) { (void)lba; (void)sectors; (void)out; return 0; }
unsigned Mods_Changes(void) { return mod_changes; }
int Log_Wanted(LogChannel channel) { (void)channel; return 0; }
void Log_Printf(LogChannel channel, const char *format, ...) { (void)channel; (void)format; }

static uint16_t bank[SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT];
uint16_t *SoftGpu_Bank(int which) { return which == 13 ? bank : NULL; }
const uint16_t *SoftGpu_Vram(void) { return NULL; }

int CardArt_ImageSize(const char *path, int *width, int *height)
{
    (void)path;
    *width = *height = 1;   /* a real PNG: the HD copy is registered */
    return 1;
}

int CardArt_IndexedImage(const char *path, int w, int h, unsigned char *indices, unsigned short *clut, char *why,
                         size_t why_size)
{
    (void)path;
    (void)why;
    (void)why_size;
    index_reads++;
    memset(indices, disk_index, (size_t)w * h);
    memset(clut, 0, 256 * sizeof(*clut));
    return 1;
}

const char *CardLayout_HandFramePath(int card_id) { (void)card_id; return hand_file; }
const char *CardLayout_FieldFramePath(int card_id) { (void)card_id; return NULL; }
const char *CardLayout_FieldBackPath(int card_id) { (void)card_id; return NULL; }
const char *CardLayout_LibraryIconPath(int card_id) { (void)card_id; return NULL; }
const char *CardLayout_BackImagePath(int card_id) { (void)card_id; return back_file; }
int CardLayout_StyleOf(int card_id, CardLayoutStyle *style)
{
    (void)card_id;
    memset(style, 0, sizeof(*style));
    style->width = style_w;
    style->height = style_h;
    return 1;
}
int CardLayout_Digits(char *path, size_t size, int *w, int *h, int *step)
{
    (void)path; (void)size; (void)w; (void)h; (void)step;
    return 0;
}
int CardLayout_StatLabel(int dfd, char *path, size_t size, int *x, int *y, int *w, int *h)
{
    (void)dfd; (void)path; (void)size; (void)x; (void)y; (void)w; (void)h;
    return 0;
}
int CardLayout_TypeLabel(int kind, char *path, size_t size, int *x, int *y, int *w, int *h, int *visible)
{
    (void)kind; (void)path; (void)size; (void)x; (void)y; (void)w; (void)h; (void)visible;
    return 0;
}
const char *CardLayout_FramePath(void) { return NULL; }
int CardLayout_FramePaths(char (*paths)[1024], int max) { (void)paths; (void)max; return 0; }
int CardLayout_Active(void) { return 1; }

/* --- helpers ------------------------------------------------------------ */

static void write_color_png(const char *name, unsigned char red, unsigned char green, unsigned char blue)
{
    unsigned char pixel[4] = {red, green, blue, 255}, encoded[256];
    png_alloc_size_t size = sizeof(encoded);
    png_image png = {0};
    char path[1024];
    FILE *file;
    png.version = PNG_IMAGE_VERSION;
    png.width = png.height = 1;
    png.format = PNG_FORMAT_RGBA;
    assert(png_image_write_to_memory(&png, encoded, &size, 0, pixel, 0, NULL));
    snprintf(path, sizeof(path), "%s/%s", root, name);
    file = fopen(path, "wb");
    assert(file && fwrite(encoded, 1, size, file) == size && !fclose(file));
}

static void path_of(char *out, const char *name) { snprintf(out, 1024, "%s/%s", root, name); }

/* The HD color the texture pack answers at texel (u, v) of the card back, or
 * -1 when no registration covers it. */
static long back_color(int u, int v)
{
    const unsigned char *pixels;
    int entry = TexturePack_BankEntryFor(FRAME_BANK, BACK_PAGE_X, BACK_PAGE_Y, 1, BACK_CLUT_X, BACK_CLUT_Y, u, v), w, h;
    if (!entry || !TexturePack_BankEntryLevel(entry, 0, &pixels, &w, &h)) return -1;
    return (long)pixels[0] << 16 | pixels[1] << 8 | pixels[2];
}

static void draw_back(const char *name, int w, int h)
{
    int tpage, u, v, clut, cw, ch;
    path_of(back_file, name);
    style_w = w;
    style_h = h;
    assert(CardLayoutArt_BackCell(1, &tpage, &u, &v, &clut, &cw, &ch) && cw == w && ch == h);
}

/* Style A's back is 140x196 and red, style B's 60x80 and green: A's picture
 * fills A's box wherever the player comes back to it from. */
static void back_follows_its_style(void)
{
    write_color_png("a.png", 255, 0, 0);
    write_color_png("b.png", 0, 255, 0);
    draw_back("a.png", 140, 196);
    assert(back_color(0, 0) == 0xff0000 && back_color(100, 150) == 0xff0000);
    draw_back("b.png", 60, 80);
    assert(back_color(0, 0) == 0x00ff00 && back_color(100, 150) == -1);
    draw_back("a.png", 140, 196);
    assert(back_color(0, 0) == 0xff0000 && back_color(100, 150) == 0xff0000);
    /* The other way round: the larger one next to a smaller own box. */
    draw_back("b.png", 60, 80);
    draw_back("a.png", 140, 196);
    draw_back("b.png", 60, 80);
    assert(back_color(0, 0) == 0x00ff00 && back_color(100, 150) == -1);
    CardLayoutArt_Clear();
    assert(back_color(0, 0) == -1);
}

static long hand_color(void)
{
    int tpage, u, v, clut, w, h;
    const unsigned char *pixels;
    int entry;
    assert(CardLayoutArt_HandFrameCell(1, &tpage, &u, &v, &clut, &w, &h));
    entry = TexturePack_BankEntryFor(FRAME_BANK, (hand_cells[0][0]) & ~63, (hand_cells[0][1]) & ~255, 1, SMALL_CLUT_X,
                                     hand_cells[0][2], u, v);
    if (!entry || !TexturePack_BankEntryLevel(entry, 0, &pixels, &w, &h)) return -1;
    return (long)pixels[0] << 16 | pixels[1] << 8 | pixels[2];
}

static unsigned char hand_texel(void)
{
    return (unsigned char)bank[hand_cells[0][1] * SOFT_GPU_WIDTH + hand_cells[0][0]];
}

/* A picture read once stays read while no mod changes; after a mod goes off
 * and on, a changed file is read again, down to the HD copy. */
static void pictures_are_read_again_after_a_mod_change(void)
{
    CardLayoutArt_Clear();
    path_of(hand_file, "hand.png");
    write_color_png("hand.png", 255, 0, 0);
    disk_index = 7;
    index_reads = 0;
    assert(hand_color() == 0xff0000 && hand_texel() == 7 && index_reads == 1);
    assert(hand_color() == 0xff0000 && index_reads == 1);   /* cached */
    /* The file is edited; nothing has been switched, so the old picture stays. */
    write_color_png("hand.png", 0, 0, 255);
    disk_index = 9;
    assert(hand_color() == 0xff0000 && hand_texel() == 7 && index_reads == 1);
    /* The mod goes off and on: the next draw reads it all again. */
    mod_changes += 2;
    assert(hand_color() == 0x0000ff && hand_texel() == 9 && index_reads == 2);
    assert(hand_color() == 0x0000ff && index_reads == 2);
}

/* A back the texture pack keeps only as a spare (a style left for another) is
 * read again too, though nothing in card_layout_art.c names it any more. */
static void a_spare_back_is_read_again(void)
{
    CardLayoutArt_Clear();
    write_color_png("a.png", 255, 0, 0);
    write_color_png("b.png", 0, 255, 0);
    draw_back("a.png", 140, 196);
    assert(back_color(0, 0) == 0xff0000);
    draw_back("b.png", 60, 80);
    assert(back_color(0, 0) == 0x00ff00);
    write_color_png("a.png", 0, 0, 255);   /* edited while the mod was off */
    mod_changes += 2;
    draw_back("a.png", 140, 196);
    assert(back_color(0, 0) == 0x0000ff);
}

static long field_color(const char *name)
{
    int tpage, u, v, clut, w, h, entry, slot;
    const unsigned char *pixels;
    char file[1024];
    path_of(file, name);
    assert(small_cell(&field_kind, file, &tpage, &u, &v, &clut, &w, &h));
    for (slot = 0; slot < field_kind.count && strcmp(field_slots[slot].path, file); slot++) continue;
    assert(slot < field_kind.count);
    entry = TexturePack_BankEntryFor(FRAME_BANK, field_cells[slot][0] & ~63, field_cells[slot][1] & ~255, 1,
                                     SMALL_CLUT_X, field_cells[slot][2], u, v);
    if (!entry || !TexturePack_BankEntryLevel(entry, 0, &pixels, &w, &h)) return -1;
    return (long)pixels[0] << 16 | pixels[1] << 8 | pixels[2];
}

/* A small picture pushed out of its kind's slots is a spare too. */
static void an_evicted_small_picture_is_read_again(void)
{
    char name[32];
    int i;
    CardLayoutArt_Clear();
    for (i = 0; i < field_kind.count + 1; i++) {   /* one file more than slots: f0 goes */
        snprintf(name, sizeof(name), "f%d.png", i);
        write_color_png(name, 255, 0, 0);
        assert(field_color(name) == 0xff0000);
    }
    write_color_png("f0.png", 0, 0, 255);
    mod_changes += 2;
    assert(field_color("f0.png") == 0x0000ff);
}

int main(void)
{
    assert(scratch_dir(root, sizeof(root), "memories-card-layout-art"));
    back_follows_its_style();
    pictures_are_read_again_after_a_mod_change();
    a_spare_back_is_read_again();
    an_evicted_small_picture_is_read_again();
    CardLayoutArt_Clear();
    TexturePack_Unload();
    puts("card layout art tests passed");
    return 0;
}
