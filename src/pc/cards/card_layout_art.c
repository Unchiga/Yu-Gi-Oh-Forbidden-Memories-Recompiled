/* See card_layout_art.h. Decode-once-and-cache + store-into-bank, the same
 * shape as star_icons.c's make()/store()/Stars_IconCell() trio -- the
 * difference is color depth: guardian star icons and glyphs are 4 bits a
 * texel (packed four to a VRAM word, tpage depth field 0), this asset is
 * CardArt_IndexedImage's own 8-bit-a-texel output (up to 255 colors plus
 * transparent 0), packed two to a word, tpage depth field 1 (getTPage's own
 * encoding, psyq/libgpu.h) -- there is no existing 8bpp bank user in this
 * codebase to copy, so that packing and tpage math are derived here from
 * the PS1's own hardware layout, not guessed. */
#include "card_layout_art.h"
#include "card_layout.h"
#include "art.h"
#include "pc/render/soft_gpu.h"
#include "pc/render/texture_pack.h"
#include "pc/debug/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_BANK 13   /* 1-12 the 3D Monsters mod, 14 star icons, 15 glyphs, 0 is real VRAM */
/* A layout mod's own frame PNG is stretched to FRAME_W x FRAME_H texels
 * (CardArt_IndexedImage's own rule, as a "title" mod's image is), not the
 * on-screen draw size (card_layout.h's CardLayout_Get(CARD_LAYOUT_FRAME) w/h,
 * a mod's own choice) but the VRAM texture's own resolution.
 *
 * POLY_GT4's UVs are u8, so one quad can read at most 255 texels an axis.
 * The frame is therefore kept as FRAME_COLS x FRAME_ROWS tiles of
 * FRAME_TILE_W x FRAME_TILE_H texels, each on its own texture page, and drawn
 * as that many abutting quads (func_80028B08.c's CardLayout_DrawFrame). One
 * 177x254 texture (the old budget) put a 919x1319 frame through a ~27x area
 * reduction and, drawn at 560x784 on screen (Internal 4x), a 3.2x upscale: the
 * stat boxes' borders and the marbling went blocky. 3x3 tiles are 531x762,
 * about one texel to a screen pixel at 4x. All tiles share one 255-color
 * palette (a median cut of the whole image), kept under the first tile.
 *
 * Tile i sits at page (2 * (i % 8), i / 8): a 8bpp page is 256 texels (128
 * halfwords) wide, so the pages across are two apart, and a bank is 1024 x
 * 512 halfwords, two pages down. The palette row, FRAME_TILE_H, is free in
 * every page (a tile is FRAME_TILE_H tall). Keep FRAME_TEXELS in
 * tools/pc/card_frame_window.py in sync with FRAME_W/FRAME_H. */
#define FRAME_COLS CARD_LAYOUT_FRAME_COLS   /* card_layout_art.h: the one place the grid is set */
#define FRAME_ROWS CARD_LAYOUT_FRAME_ROWS
#define FRAME_TILE_W 177
#define FRAME_TILE_H 254
#define FRAME_W (FRAME_COLS * FRAME_TILE_W)
#define FRAME_H (FRAME_ROWS * FRAME_TILE_H)
/* The tiles take pages 0 to FRAME_COLS * FRAME_ROWS - 1 of the bank; the digit strip is on page 9. */
typedef char frame_tiles_fit_before_the_digits[FRAME_COLS * FRAME_ROWS <= 9 ? 1 : -1];
#define FRAME_CLUT_Y FRAME_TILE_H   /* right after the first tile's last pixel row, same bank, no overlap */

/* Each frame image's built texels (FRAME_W x FRAME_H palette indices and its
 * palette), kept so that a card of another kind -- another frame image -- or
 * one drawn again later costs a copy into the bank, not a decode and a median
 * cut. The bank holds one frame at a time (bank_path). */
#define FRAME_CACHE 8
typedef struct {
    char path[1024];
    unsigned char *indices;   /* NULL: built and failed */
    unsigned short clut[256];
    int png_w, png_h;
    int used;
} FrameImage;
static FrameImage images[FRAME_CACHE];
static char bank_path[1024];    /* the path whose texels the bank holds now */

static FrameImage *image_for(const char *path, int build)
{
    FrameImage *slot = NULL;
    int i;
    char why[128];

    for (i = 0; i < FRAME_CACHE; i++) {
        if (images[i].used && !strcmp(images[i].path, path)) return &images[i];
    }
    if (!build) return NULL;
    for (i = 0; i < FRAME_CACHE && !slot; i++) {
        if (!images[i].used) slot = &images[i];
    }
    if (!slot) {   /* full: the first goes (a layout has at most one image per kind) */
        slot = &images[0];
        free(slot->indices);
        if (!strcmp(bank_path, slot->path)) bank_path[0] = 0;
    }
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    slot->used = 1;
    CardArt_ImageSize(path, &slot->png_w, &slot->png_h);
    slot->indices = malloc((size_t)FRAME_W * FRAME_H);
    if (slot->indices &&
        !CardArt_IndexedImage(path, FRAME_W, FRAME_H, slot->indices, slot->clut, why, sizeof(why))) {
        LOG(LOG_CARD_LAYOUT, "frame: %s: %s", path, why);
        free(slot->indices);
        slot->indices = NULL;
    }
    return slot;
}

/* The image's texels into the bank, tile by tile (see the layout above). */
static void store(uint16_t *bank, const FrameImage *image)
{
    int tile, x, y;

    for (tile = 0; tile < FRAME_COLS * FRAME_ROWS; tile++) {
        int col = tile % FRAME_COLS, row = tile / FRAME_COLS;
        uint16_t *page = bank + (tile / 8 * 256) * SOFT_GPU_WIDTH + tile % 8 * 128;
        for (y = 0; y < FRAME_TILE_H; y++) {
            const unsigned char *line = image->indices + (row * FRAME_TILE_H + y) * FRAME_W + col * FRAME_TILE_W;
            for (x = 0; x < FRAME_TILE_W; x += 2) {
                unsigned char lo = line[x];
                unsigned char hi = x + 1 < FRAME_TILE_W ? line[x + 1] : 0;   /* the odd width's last pair */
                page[y * SOFT_GPU_WIDTH + x / 2] = (uint16_t)(lo | (hi << 8));
            }
        }
    }
    memcpy(&bank[FRAME_CLUT_Y * SOFT_GPU_WIDTH], image->clut, 256 * sizeof(uint16_t));
}

int CardLayoutArt_FrameTile(int col, int row, int *tpage, int *clut, int *w, int *h)
{
    uint16_t *bank;
    const char *path = CardLayout_FramePath();
    const FrameImage *image;
    int tile = row * FRAME_COLS + col;
    int sx, sy, sx1, sy1;
    if (col < 0 || col >= FRAME_COLS || row < 0 || row >= FRAME_ROWS) return 0;
    if (!path || !*path) { LOG(LOG_CARD_LAYOUT, "FrameTile: no path"); return 0; }
    if (!(bank = SoftGpu_Bank(FRAME_BANK))) { LOG(LOG_CARD_LAYOUT, "FrameTile: no bank"); return 0; }
    image = image_for(path, 1);
    if (!image || !image->indices) return 0;
    if (strcmp(bank_path, path)) {
        store(bank, image);
        snprintf(bank_path, sizeof(bank_path), "%s", path);
    }
    /* getTPage(1, 0, x, y) | bank << 11: 8bpp, this tile's page in the bank (store()'s layout) */
    *tpage = 0x80 | (FRAME_BANK << 11) | (tile % 8 * 2) | (tile / 8 << 4);
    *clut = (FRAME_CLUT_Y << 6) | 0;   /* getClut(0, FRAME_CLUT_Y) */
    *w = FRAME_TILE_W;
    *h = FRAME_TILE_H;
    if (image->png_w > 0 && image->png_h > 0) {
        TexturePack_BankSpritesUseOwner(TEXTURE_BANK_OWNER_LAYOUT_FRAME);
        sx = col * FRAME_TILE_W * image->png_w / FRAME_W;
        sx1 = (col + 1) * FRAME_TILE_W * image->png_w / FRAME_W;
        sy = row * FRAME_TILE_H * image->png_h / FRAME_H;
        sy1 = (row + 1) * FRAME_TILE_H * image->png_h / FRAME_H;
        TexturePack_AddBankSpriteCrop(FRAME_BANK, (tile % 8 * 2) * 64, tile / 8 * 256, 1, 0, 0,
                                      FRAME_TILE_W, FRAME_TILE_H, path, sx, sy, sx1 - sx, sy1 - sy);
    }
    return 1;
}

/* The digit strip: one image, one page of the frame's bank. The frame fills
 * the first tile row's pages and the first of the second row's (tile 8);
 * the digits are on the second row's next page, tile slot 9, their palette
 * right under them. */
#define DIGITS_SLOT 9
#define DIGITS_W (CARD_LAYOUT_DIGIT_COLS * CARD_LAYOUT_DIGIT_CELL_W)
#define DIGITS_H (CARD_LAYOUT_DIGIT_ROWS * CARD_LAYOUT_DIGIT_CELL_H)
static char digits_path[1024];          /* the strip built (or failed) */
static unsigned char *digits_indices;
static unsigned short digits_clut[256];
static int digits_png_w, digits_png_h;
static char digits_bank_path[1024];     /* the strip the bank holds now */

static void digits_build(const char *path)
{
    char why[128];

    if (!strcmp(digits_path, path)) return;
    free(digits_indices);
    snprintf(digits_path, sizeof(digits_path), "%s", path);
    digits_png_w = digits_png_h = 0;
    CardArt_ImageSize(path, &digits_png_w, &digits_png_h);
    digits_bank_path[0] = 0;
    digits_indices = malloc((size_t)DIGITS_W * DIGITS_H);
    if (digits_indices &&
        !CardArt_IndexedImage(path, DIGITS_W, DIGITS_H, digits_indices, digits_clut, why, sizeof(why))) {
        LOG(LOG_CARD_LAYOUT, "digits: %s: %s", path, why);
        free(digits_indices);
        digits_indices = NULL;
    }
}

int CardLayoutArt_DigitCell(int digit, int dim, int *tpage, int *u, int *v, int *clut, int *w, int *h)
{
    char path[1024];
    int dw, dh, step, x, y, sx, sy, sx1, sy1;
    uint16_t *bank, *page;

    if (digit < 0 || digit > 9) return 0;
    if (!CardLayout_Digits(path, sizeof(path), &dw, &dh, &step)) return 0;
    if (!(bank = SoftGpu_Bank(FRAME_BANK))) return 0;
    digits_build(path);
    if (!digits_indices) return 0;
    page = bank + (DIGITS_SLOT / 8 * 256) * SOFT_GPU_WIDTH + DIGITS_SLOT % 8 * 128;
    if (strcmp(digits_bank_path, path)) {
        for (y = 0; y < DIGITS_H; y++) {
            for (x = 0; x < DIGITS_W; x += 2) {
                page[y * SOFT_GPU_WIDTH + x / 2] =
                    (uint16_t)(digits_indices[y * DIGITS_W + x] | (digits_indices[y * DIGITS_W + x + 1] << 8));
            }
        }
        memcpy(&page[DIGITS_H * SOFT_GPU_WIDTH], digits_clut, 256 * sizeof(uint16_t));
        snprintf(digits_bank_path, sizeof(digits_bank_path), "%s", path);
    }
    *tpage = 0x80 | (FRAME_BANK << 11) | (DIGITS_SLOT % 8 * 2) | (DIGITS_SLOT / 8 << 4);
    *clut = ((DIGITS_SLOT / 8 * 256 + DIGITS_H) << 6) | (DIGITS_SLOT % 8 * 128 / 16);
    *u = digit % CARD_LAYOUT_DIGIT_COLS * CARD_LAYOUT_DIGIT_CELL_W;
    *v = (digit / CARD_LAYOUT_DIGIT_COLS + (dim ? 2 : 0)) * CARD_LAYOUT_DIGIT_CELL_H;
    *w = CARD_LAYOUT_DIGIT_CELL_W;
    *h = CARD_LAYOUT_DIGIT_CELL_H;
    if (digits_png_w > 0 && digits_png_h > 0) {
        TexturePack_BankSpritesUseOwner(TEXTURE_BANK_OWNER_LAYOUT_DIGITS);
        sx = *u * digits_png_w / DIGITS_W;
        sx1 = (*u + *w) * digits_png_w / DIGITS_W;
        sy = *v * digits_png_h / DIGITS_H;
        sy1 = (*v + *h) * digits_png_h / DIGITS_H;
        TexturePack_AddBankSpriteCrop(FRAME_BANK, (DIGITS_SLOT % 8 * 2) * 64, DIGITS_SLOT / 8 * 256, 1,
                                      *u, *v, *w, *h, path, sx, sy, sx1 - sx, sy1 - sy);
    }
    return 1;
}

void CardLayoutArt_Prewarm(void)
{
    static unsigned int frame;
    char paths[FRAME_CACHE][1024];
    char digits[1024] = "";
    int i, count, dw, dh, dstep;

    if (++frame % 30) return;   /* the layout changes with a setting, not a frame */
    if (CardLayout_Digits(digits, sizeof(digits), &dw, &dh, &dstep) && strcmp(digits_path, digits)) {
        digits_build(digits);
        return;
    } else if (!digits[0]) TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_LAYOUT_DIGITS);
    count = CardLayout_FramePaths(paths, FRAME_CACHE);
    if (!count) TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_LAYOUT_FRAME);
    for (i = 0; i < count; i++) {
        if (!image_for(paths[i], 0)) {
            image_for(paths[i], 1);
            return;   /* one a call: each is a hitch of its own */
        }
    }
}
