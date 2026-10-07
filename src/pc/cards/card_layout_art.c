/* See card_layout_art.h. Decode-once-and-cache + store-into-bank, the same
 * shape as star_icons.c's make()/store()/Stars_IconCell() trio -- the
 * difference is colour depth: guardian star icons and glyphs are 4 bits a
 * texel (packed four to a VRAM word, tpage depth field 0), this asset is
 * CardArt_IndexedImage's own 8-bit-a-texel output (up to 255 colours plus
 * transparent 0), packed two to a word, tpage depth field 1 (getTPage's own
 * encoding, psyq/libgpu.h) -- there is no existing 8bpp bank user in this
 * codebase to copy, so that packing and tpage math are derived here from
 * the PS1's own hardware layout, not guessed. */
#include "card_layout_art.h"
#include "card_layout.h"
#include "art.h"
#include "pc/render/soft_gpu.h"
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
 * about one texel to a screen pixel at 4x. All tiles share one 255-colour
 * palette (a median cut of the whole image), kept under the first tile.
 *
 * Tile i sits at page (2 * (i % 8), i / 8): a 8bpp page is 256 texels (128
 * halfwords) wide, so the pages across are two apart, and a bank is 1024 x
 * 512 halfwords, two pages down. The palette row, FRAME_TILE_H, is free in
 * every page (a tile is FRAME_TILE_H tall). Keep FRAME_TEXELS in
 * tools/pc/card_frame_window.py in sync with FRAME_W/FRAME_H. */
#define FRAME_COLS 3
#define FRAME_ROWS 3
#define FRAME_TILE_W 177
#define FRAME_TILE_H 254
#define FRAME_W (FRAME_COLS * FRAME_TILE_W)
#define FRAME_H (FRAME_ROWS * FRAME_TILE_H)
#define FRAME_CLUT_Y FRAME_TILE_H   /* right after the first tile's last pixel row, same bank, no overlap */

static unsigned char made;      /* 0 not yet, 1 made, 2 failed */
static char made_path[1024];    /* the path "made" was decoded from, to notice a different mod's art */

static int make(uint16_t *bank, const char *path)
{
    unsigned char *indices = malloc((size_t)FRAME_W * FRAME_H);
    unsigned short clut[256];
    char why[128];
    int x, y, tile;
    if (!indices) return 0;
    if (!CardArt_IndexedImage(path, FRAME_W, FRAME_H, indices, clut, why, sizeof(why))) {
        LOG(LOG_CARD_LAYOUT, "frame: %s: %s", path, why);
        free(indices);
        return 0;
    }
    for (tile = 0; tile < FRAME_COLS * FRAME_ROWS; tile++) {
        int col = tile % FRAME_COLS, row = tile / FRAME_COLS;
        uint16_t *page = bank + (tile / 8 * 256) * SOFT_GPU_WIDTH + tile % 8 * 128;
        for (y = 0; y < FRAME_TILE_H; y++) {
            const unsigned char *line = indices + (row * FRAME_TILE_H + y) * FRAME_W + col * FRAME_TILE_W;
            for (x = 0; x < FRAME_TILE_W; x += 2) {
                unsigned char lo = line[x];
                unsigned char hi = x + 1 < FRAME_TILE_W ? line[x + 1] : 0;   /* the odd width's last pair */
                page[y * SOFT_GPU_WIDTH + x / 2] = (uint16_t)(lo | (hi << 8));
            }
        }
    }
    memcpy(&bank[FRAME_CLUT_Y * SOFT_GPU_WIDTH], clut, 256 * sizeof(uint16_t));
    free(indices);
    return 1;
}

int CardLayoutArt_FrameTile(int col, int row, int *tpage, int *clut, int *w, int *h)
{
    uint16_t *bank;
    const char *path = CardLayout_FramePath();
    int tile = row * FRAME_COLS + col;
    if (col < 0 || col >= FRAME_COLS || row < 0 || row >= FRAME_ROWS) return 0;
    if (!path || !*path) { LOG(LOG_CARD_LAYOUT, "FrameTile: no path"); return 0; }
    if (strcmp(path, made_path)) {
        /* A different mod's frame art (or the same mod's art changed under
         * it) than whatever is cached: decode again, same as the first
         * time. Mods rarely change their own shipped art mid-session, so
         * this is a cheap strcmp on the common path (nothing changed) and
         * only actually re-decodes on the rare path (it did). */
        made = 0;
        snprintf(made_path, sizeof(made_path), "%s", path);
    }
    if (made == 2) return 0;
    if (!(bank = SoftGpu_Bank(FRAME_BANK))) { LOG(LOG_CARD_LAYOUT, "FrameTile: no bank"); return 0; }
    if (!made) made = make(bank, path) ? 1 : 2;
    if (made != 1) return 0;
    /* getTPage(1, 0, x, y) | bank << 11: 8bpp, this tile's page in the bank (make()'s layout) */
    *tpage = 0x80 | (FRAME_BANK << 11) | (tile % 8 * 2) | (tile / 8 << 4);
    *clut = (FRAME_CLUT_Y << 6) | 0;   /* getClut(0, FRAME_CLUT_Y) */
    *w = FRAME_TILE_W;
    *h = FRAME_TILE_H;
    return 1;
}
