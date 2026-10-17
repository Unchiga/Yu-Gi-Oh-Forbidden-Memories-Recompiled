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
#include "pc/mods/mods.h"
#include "game/card_constants.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_BANK 13   /* 1-12 the 3D Monsters mod, 14 star icons, 15 glyphs, 0 is real VRAM */
/* An 8bpp texture page starts on a 64-word boundary. */
#define LAYOUT_TPAGE(x, y) (0x80 | (FRAME_BANK << 11) | ((x) / 64) | (((y) / 256) << 4))

/* Every cell function starts here (defined after the caches it drops): a mod
 * switched on or off since the last call may have different pictures under the
 * same paths (see CardLayoutArt_Clear). */
static void sync_with_mods(void);

/* PS1 hardware never actually draws a textured quad's own far-right or
 * far-bottom texel: gl_picture.c's own texel_left_out says so directly
 * ("the rasterizer stops short of the right and bottom edges... where the
 * texels grow towards them, the last"), and tools/pc/hd_assets_pack.py's
 * own first_twice exists to compensate for the same limit in retail's own
 * assets ("a run of texels is drawn one pixel longer than it is, its first
 * texel twice"). Every draw site that reads one of this file's cells
 * compensates the same way, reading one texel past the cell's own w/h
 * (u + w, not u + w - 1) -- so the bank needs that one extra row and
 * column too, each a duplicate of the real edge, not whatever this bank
 * slot held before (found live: every one of this file's cells read short
 * on their own right and bottom edge without this). bank_x/bank_y are in
 * this cell's own texel space (its own u=0, v=0 at (0, 0)), not raw
 * halfword offsets -- every cell here already keeps a *_X_WORD constant
 * for the halfword base a texel column adds onto. */
static unsigned char bank_texel_get(const uint16_t *bank, int bank_x_word, int bank_y, int y, int x)
{
    uint16_t word = bank[(bank_y + y) * SOFT_GPU_WIDTH + bank_x_word + x / 2];
    return (unsigned char)(x & 1 ? (word >> 8) : word);
}

static void bank_texel_set(uint16_t *bank, int bank_x_word, int bank_y, int y, int x, unsigned char value)
{
    uint16_t *word = &bank[(bank_y + y) * SOFT_GPU_WIDTH + bank_x_word + x / 2];
    *word = (uint16_t)(x & 1 ? ((*word & 0x00FF) | ((unsigned)value << 8)) : ((*word & 0xFF00) | value));
}

static void bank_pad_edge(uint16_t *bank, int bank_x_word, int bank_y, int w, int h)
{
    int x, y;
    for (y = 0; y < h; y++)
        bank_texel_set(bank, bank_x_word, bank_y, y, w, bank_texel_get(bank, bank_x_word, bank_y, y, w - 1));
    for (x = 0; x <= w; x++)
        bank_texel_set(bank, bank_x_word, bank_y, h, x, bank_texel_get(bank, bank_x_word, bank_y, h - 1, x));
}

/* A layout mod's own frame PNG is stretched to FRAME_W x FRAME_H texels
 * (CardArt_IndexedImage's own rule, as a "title" mod's image is), not the
 * on-screen draw size (card_layout.h's CardLayout_Part(CARD_LAYOUT_FRAME) w/h,
 * a mod's own choice) but the VRAM texture's own resolution.
 *
 * POLY_GT4's UVs are u8, so one quad can read at most 255 texels an axis.
 * The frame is therefore kept as FRAME_COLS x FRAME_ROWS tiles of
 * FRAME_TILE_W x FRAME_TILE_H texels, each on its own texture page, and drawn
 * as that many abutting quads (func_80028B08.c's CardLayout_DrawFrame). Above
 * 1x the texture pack's own PNG is sampled instead (TexturePack_AddBankSpriteCrop
 * below), so these texels are what a 1x picture shows. All tiles of a frame
 * share one 255-color palette (a median cut of the whole image), kept under
 * the first tile.
 *
 * The quads are not rasterised when they are submitted but when the frame is
 * presented, from whatever the bank holds then. Two cards of different frame
 * styles on screen together (an attack: the attacker's and the defender's;
 * a trap shown beside the attacker) therefore need a copy of each style in
 * the bank at once, or both draw the one stored last. The bank holds
 * FRAME_SLOTS copies: a style takes a slot (the one it is already in, else
 * the one used longest ago), and a slot's tiles and palette are its own.
 *
 * Page p of the bank is at (2 * (p % 8), p / 8) in 64-halfword units (an 8bpp
 * page is 256 texels, 128 halfwords wide, so the pages across are two apart,
 * and a bank is 1024 x 512 halfwords, two pages down). Slot s owns pages
 * s * FRAME_SLOT_PAGES onward, tile i the i-th of them. Its palette is the
 * row FRAME_TILE_H, free in every page (a tile is FRAME_TILE_H tall), at
 * halfword 256 * s. Keep FRAME_TEXELS in tools/pc/card_frame_window.py in
 * sync with FRAME_W/FRAME_H. */
#define FRAME_COLS CARD_LAYOUT_FRAME_COLS   /* card_layout_art.h: the one place the grid is set */
#define FRAME_ROWS CARD_LAYOUT_FRAME_ROWS
#define FRAME_TILE_W 255
#define FRAME_TILE_H 254
#define FRAME_W (FRAME_COLS * FRAME_TILE_W)
#define FRAME_H (FRAME_ROWS * FRAME_TILE_H)
#define FRAME_SLOTS 3
#define FRAME_SLOT_PAGES (FRAME_COLS * FRAME_ROWS)
/* The frames take pages 0 to FRAME_SLOTS * FRAME_SLOT_PAGES - 1 of the bank; the digit strip is on the page after them. */
typedef char frame_slots_fit_before_the_digits[FRAME_SLOTS * FRAME_SLOT_PAGES <= 15 ? 1 : -1];
typedef char frame_palettes_fit_the_bank_width[FRAME_SLOTS * 256 <= 1024 ? 1 : -1];
#define FRAME_CLUT_Y FRAME_TILE_H   /* right after the first tile's last pixel row, same bank, no overlap */

/* The small assets of a frame style: the 52x60 hand, field and face-down
 * sprites, the 12x14 Library icon, the ATK/DFD captions and the
 * "[Trap Card]"-style captions. Cards of several styles are on screen
 * together (a hand, the field, the Library grid, an attack's two cards),
 * and the quads are rasterised when the frame is presented, from whatever
 * the bank holds then, so each kind keeps several pictures in the bank at
 * once (slots, the one asked for longest ago replaced), each its own cell
 * and palette. A picture is decoded once, when it takes a slot.
 *
 * The cells are in the lower half of the bank (see the map above
 * CardLayoutArt_StatLabelCell): x, y in halfwords, and the palette's row;
 * every palette is at SMALL_CLUT_X. */
#define SMALL_CLUT_X 512
typedef struct {
    char path[1024];
    unsigned char *indices;   /* NULL: the picture failed */
    unsigned short clut[256];
    int png_w, png_h;
    int stored;               /* its texels and palette are in the bank, its HD sprite registered */
    unsigned long long used;  /* 0: free; else small_clock when last asked for */
} SmallSlot;
typedef struct {
    const char *what;
    int w, h, pad;            /* texels; pad: bank_pad_edge's extra row and column */
    int count;
    const short (*cell)[3];   /* x, y, palette row */
    SmallSlot *slots;
} SmallKind;
static unsigned long long small_clock;

/* 52x60 cells (27x61 halfwords with the padded edge). */
#define SPRITE_W 52
#define SPRITE_H 60
static const short hand_cells[][3] = {
    {640, 335, 476}, {667, 335, 477}, {694, 335, 478}, {721, 335, 479}, {748, 335, 480}, {775, 335, 481}};
static const short field_cells[][3] = {
    {802, 335, 482}, {829, 335, 483}, {856, 335, 484}, {768, 396, 485}, {795, 396, 486}};
static const short field_back_cells[][3] = {
    {822, 396, 487}, {849, 396, 488}, {612, 256, 489}, {612, 317, 490}, {612, 378, 491}};
/* 12x14 cells (7x15 with the padded edge). */
static const short library_cells[][3] = {
    {784, 320, 468}, {791, 320, 469}, {798, 320, 470}, {805, 320, 471},
    {812, 320, 472}, {819, 320, 473}, {826, 320, 474}, {833, 320, 475}};
/* 64x32 cells (32x32), not padded: the captions' own draw reads inside them. */
static const short stat_label_cells[][3] = {
    {640, 288, 457}, {672, 288, 458}, {704, 288, 459}, {736, 288, 460},
    {768, 288, 461}, {800, 288, 462}, {832, 288, 463}, {864, 288, 464}};
static const short type_label_cells[][3] = {
    {640, 256, 449}, {672, 256, 450}, {704, 256, 451}, {736, 256, 452},
    {768, 256, 453}, {800, 256, 454}, {832, 256, 455}, {864, 256, 456}};
#define SLOTS_OF(cells) ((int)(sizeof(cells) / sizeof(cells[0])))
static SmallSlot hand_slots[SLOTS_OF(hand_cells)], field_slots[SLOTS_OF(field_cells)],
    field_back_slots[SLOTS_OF(field_back_cells)], library_slots[SLOTS_OF(library_cells)],
    stat_label_slots[SLOTS_OF(stat_label_cells)], type_label_slots[SLOTS_OF(type_label_cells)];
static SmallKind hand_kind = {"hand frame", SPRITE_W, SPRITE_H, 1, SLOTS_OF(hand_cells), hand_cells, hand_slots};
static SmallKind field_kind = {"field frame", SPRITE_W, SPRITE_H, 1, SLOTS_OF(field_cells), field_cells, field_slots};
static SmallKind field_back_kind = {"field back", SPRITE_W, SPRITE_H, 1, SLOTS_OF(field_back_cells),
                                    field_back_cells, field_back_slots};
static SmallKind library_kind = {"library icon", 12, 14, 1, SLOTS_OF(library_cells), library_cells, library_slots};
static SmallKind stat_label_kind = {"stat label", 64, 32, 0, SLOTS_OF(stat_label_cells), stat_label_cells,
                                    stat_label_slots};
static SmallKind type_label_kind = {"type label", 64, 32, 0, SLOTS_OF(type_label_cells), type_label_cells,
                                    type_label_slots};
static SmallKind *const small_kinds[] = {&hand_kind, &field_kind, &field_back_kind, &library_kind,
                                         &stat_label_kind, &type_label_kind};

/* `path`'s cell of `kind`, decoded and stored into a slot first when it is in
 * none; 0 when there is no picture or it cannot be read. */
static int small_cell(SmallKind *kind, const char *path, int *tpage, int *u, int *v, int *clut, int *w, int *h)
{
    SmallSlot *slot = NULL;
    uint16_t *bank;
    char why[128];
    int i, x, y, cx, cy, row;

    sync_with_mods();
    if (!path || !*path || !(bank = SoftGpu_Bank(FRAME_BANK))) return 0;
    for (i = 0; i < kind->count && !slot; i++) {
        if (kind->slots[i].used && !strcmp(kind->slots[i].path, path)) slot = &kind->slots[i];
    }
    if (!slot) {
        slot = &kind->slots[0];
        for (i = 1; i < kind->count; i++) {
            if (kind->slots[i].used < slot->used) slot = &kind->slots[i];
        }
        free(slot->indices);
        memset(slot, 0, sizeof(*slot));
        snprintf(slot->path, sizeof(slot->path), "%s", path);
        CardArt_ImageSize(path, &slot->png_w, &slot->png_h);
        slot->indices = malloc((size_t)kind->w * kind->h);
        if (slot->indices &&
            !CardArt_IndexedImage(path, kind->w, kind->h, slot->indices, slot->clut, why, sizeof(why))) {
            LOG(LOG_CARD_LAYOUT, "%s: %s: %s", kind->what, path, why);
            free(slot->indices);
            slot->indices = NULL;
        }
        if (slot->indices && slot->png_w > 0) TexturePack_BankImagePreload(path);
    }
    slot->used = ++small_clock;
    if (!slot->indices) return 0;
    i = (int)(slot - kind->slots);
    cx = kind->cell[i][0];
    cy = kind->cell[i][1];
    row = kind->cell[i][2];
    if (!slot->stored) {
        for (y = 0; y < kind->h; y++) {
            for (x = 0; x < kind->w; x += 2) {
                bank[(cy + y) * SOFT_GPU_WIDTH + cx + x / 2] =
                    (uint16_t)(slot->indices[y * kind->w + x] | (slot->indices[y * kind->w + x + 1] << 8));
            }
        }
        if (kind->pad) bank_pad_edge(bank, cx, cy, kind->w, kind->h);
        memcpy(&bank[row * SOFT_GPU_WIDTH + SMALL_CLUT_X], slot->clut, sizeof(slot->clut));
        slot->stored = 1;
        if (slot->png_w > 0 && slot->png_h > 0) {
            TexturePack_BankSpritesUseOwner(TEXTURE_BANK_OWNER_LAYOUT_FRAME);
            TexturePack_AddBankSprite(FRAME_BANK, cx & ~63, cy & ~255, 1, SMALL_CLUT_X, row, (cx & 63) * 2, cy & 255,
                                      kind->w, kind->h, path);
        }
    }
    *tpage = LAYOUT_TPAGE(cx, cy);
    *u = (cx & 63) * 2;   /* texels: two a halfword at 8bpp */
    *v = cy & 255;
    *clut = (row << 6) | (SMALL_CLUT_X / 16);
    *w = kind->w;
    *h = kind->h;
    return 1;
}

int CardLayoutArt_HandFrameCell(int card_id, int *tpage, int *u, int *v, int *clut, int *w, int *h)
{
    return small_cell(&hand_kind, CardLayout_HandFramePath(card_id), tpage, u, v, clut, w, h);
}

int CardLayoutArt_FieldFrameCell(int card_id, int *tpage, int *u, int *v, int *clut, int *w, int *h)
{
    return small_cell(&field_kind, CardLayout_FieldFramePath(card_id), tpage, u, v, clut, w, h);
}

int CardLayoutArt_FieldBackCell(int card_id, int *tpage, int *u, int *v, int *clut, int *w, int *h)
{
    return small_cell(&field_back_kind, CardLayout_FieldBackPath(card_id), tpage, u, v, clut, w, h);
}

/* Unlike every other cell here,
 * this one's own texel size is not fixed -- back_image stretches to the
 * style's own "width" x "height" (same box "image" fills), so the cached
 * build is keyed on size as well as path: a path alone is not enough to
 * know the bank still holds what a *later* call, from a different style
 * sharing that same image, would need redrawn at a different size. 255 is
 * POLY_FT4's own u8 UV span limit (v + h - 1 must fit a byte), comfortably
 * past the 140x196 default and any ordinary style box; 128 halfwords wide
 * (256 texels at 8bpp) and 256 rows is this one slot's own ceiling, wider
 * than the UV limit already allows. 254, not 255: the padded edge
 * (bank_pad_edge) needs one more row and column past bw/bh, and the CLUT
 * sits one past that again, so the last row/column actually used is
 * bh + 1 (256 rows means valid relative rows 0-255; bh + 1 <= 255 needs
 * bh <= 254, and the same bound covers bw since both share this ceiling). */
#define BACK_PAGE_X 896
#define BACK_PAGE_Y 256
/* The pixels occupy x=896..1023, y=256..510 at most.  Keep the full-width
 * palette at the bank's final row instead of directly below a variable-size
 * image: rows below a short image are also used by the fixed small assets in
 * the x=640..895 band. */
#define BACK_CLUT_X 768
#define BACK_CLUT_Y 511
#define BACK_MAX_SIDE 254
static char back_path[1040];
static char back_bank_path[1040];
static unsigned char *back_indices;
static unsigned short back_clut[256];
static int back_png_w, back_png_h;
static int back_built_w, back_built_h;

static void back_build(const char *key, const char *path, int bw, int bh)
{
    char why[128];

    if (!strcmp(back_path, key)) return;
    free(back_indices);
    back_indices = NULL;
    back_png_w = back_png_h = 0;
    back_bank_path[0] = 0;
    snprintf(back_path, sizeof(back_path), "%s", key);
    CardArt_ImageSize(path, &back_png_w, &back_png_h);
    back_indices = malloc((size_t)bw * bh);
    if (back_indices && !CardArt_IndexedImage(path, bw, bh, back_indices, back_clut, why, sizeof(why))) {
        LOG(LOG_CARD_LAYOUT, "card-viewer back: %s: %s", path, why);
        free(back_indices);
        back_indices = NULL;
    }
    if (back_indices && back_png_w > 0) TexturePack_BankImagePreload(path);
}

int CardLayoutArt_BackCell(int card_id, int *tpage, int *u, int *v, int *clut, int *w, int *h)
{
    const char *path = CardLayout_BackImagePath(card_id);
    CardLayoutStyle style;
    uint16_t *bank;
    char key[1040];
    int x, y, bw, bh;

    sync_with_mods();
    if (!path || !*path || !(bank = SoftGpu_Bank(FRAME_BANK))) return 0;
    if (!CardLayout_StyleOf(card_id, &style)) return 0;
    bw = style.width;
    bh = style.height;
    if (bw <= 0 || bh <= 0 || bw > BACK_MAX_SIDE || bh > BACK_MAX_SIDE) {
        LOG(LOG_CARD_LAYOUT, "card-viewer back: %s: style box %dx%d is out of range (1-%d each)", path, bw, bh,
            BACK_MAX_SIDE);
        return 0;
    }
    snprintf(key, sizeof(key), "%dx%d:%s", bw, bh, path);
    back_build(key, path, bw, bh);
    if (!back_indices) return 0;
    if (strcmp(back_bank_path, key)) {
        for (y = 0; y < bh; y++) {
            for (x = 0; x < bw; x += 2) {
                bank[(BACK_PAGE_Y + y) * SOFT_GPU_WIDTH + BACK_PAGE_X + x / 2] =
                    (uint16_t)(back_indices[y * bw + x] | ((x + 1 < bw ? back_indices[y * bw + x + 1] : 0) << 8));
            }
        }
        bank_pad_edge(bank, BACK_PAGE_X, BACK_PAGE_Y, bw, bh);
        memcpy(&bank[BACK_CLUT_Y * SOFT_GPU_WIDTH + BACK_CLUT_X], back_clut, sizeof(back_clut));
        snprintf(back_bank_path, sizeof(back_bank_path), "%s", key);
        back_built_w = bw;
        back_built_h = bh;
        /* The one back the bank holds, with the one HD sprite of it.  The
         * texture pack keeps a registration per size and finds the newest
         * first, so an earlier style's smaller back left in place would keep
         * covering its corner of this one's (and of a later return to its
         * own).  Its own owner lets it go whole, whether or not this
         * back has a PNG of its own to register. */
        TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_LAYOUT_BACK);
        if (back_png_w > 0 && back_png_h > 0) {
            TexturePack_BankSpritesUseOwner(TEXTURE_BANK_OWNER_LAYOUT_BACK);
            TexturePack_AddBankSprite(FRAME_BANK, BACK_PAGE_X, BACK_PAGE_Y, 1, BACK_CLUT_X, BACK_CLUT_Y, 0, 0,
                                      bw, bh, path);
        }
    }
    *tpage = LAYOUT_TPAGE(BACK_PAGE_X, BACK_PAGE_Y);
    *u = 0;
    *v = 0;
    *clut = (BACK_CLUT_Y << 6) | (BACK_CLUT_X / 16);
    *w = back_built_w;
    *h = back_built_h;
    return 1;
}

int CardLayoutArt_LibraryIconCell(int card_id, int *tpage, int *u, int *v, int *clut, int *w, int *h)
{
    return small_cell(&library_kind, CardLayout_LibraryIconPath(card_id), tpage, u, v, clut, w, h);
}

/* Each frame image's built texels (FRAME_W x FRAME_H palette indices and its
 * palette), kept so that a card of another kind -- another frame image -- or
 * one drawn again later costs a copy into the bank, not a decode and a median
 * cut. The bank holds FRAME_SLOTS of them at a time (slots). */
#define FRAME_CACHE 64   /* one entry a frame picture, not a card; a cache smaller than the set in use rebuilds a picture each time it is drawn */
typedef struct {
    char path[1024];
    unsigned char *indices;   /* NULL: built and failed */
    unsigned short clut[256];
    int png_w, png_h;
    unsigned long long used;   /* 0: free; else image_clock when last asked for */
} FrameImage;
static FrameImage images[FRAME_CACHE];
static unsigned long long image_clock;
static int cache_warned;
typedef struct {
    char path[1024];          /* the path whose texels the slot holds now, "" when none */
    unsigned long long used;  /* when a tile of it was last asked for (slot_clock) */
} FrameSlot;
static FrameSlot slots[FRAME_SLOTS];
static unsigned long long slot_clock;

static FrameImage *image_for(const char *path, int build)
{
    FrameImage *slot = NULL;
    int i;
    char why[128];

    for (i = 0; i < FRAME_CACHE; i++) {
        if (images[i].used && !strcmp(images[i].path, path)) { images[i].used = ++image_clock; return &images[i]; }
    }
    if (!build) return NULL;
    for (i = 0; i < FRAME_CACHE && !slot; i++) {
        if (!images[i].used) slot = &images[i];
    }
    if (!slot) {   /* full: the one asked for longest ago goes */
        slot = &images[0];
        for (i = 1; i < FRAME_CACHE; i++) {
            if (images[i].used < slot->used) slot = &images[i];
        }
        if (!cache_warned) {
            cache_warned = 1;
            fprintf(stderr, "memories-pc: card frames: more than %d frame pictures in use, one is rebuilt each time it is drawn (slow)\n",
                    FRAME_CACHE);
        }
        free(slot->indices);
        for (i = 0; i < FRAME_SLOTS; i++) {
            if (!strcmp(slots[i].path, slot->path)) slots[i].path[0] = 0;
        }
    }
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    slot->used = ++image_clock;
    CardArt_ImageSize(path, &slot->png_w, &slot->png_h);
    slot->indices = malloc((size_t)FRAME_W * FRAME_H);
    if (slot->indices &&
        !CardArt_IndexedImage(path, FRAME_W, FRAME_H, slot->indices, slot->clut, why, sizeof(why))) {
        LOG(LOG_CARD_LAYOUT, "frame: %s: %s", path, why);
        free(slot->indices);
        slot->indices = NULL;
    }
    /* The PNG itself for above 1x (TexturePack_AddBankSprite), read with
     * the rest of the build rather than when a tile is first drawn. */
    if (slot->indices && slot->png_w > 0) TexturePack_BankImagePreload(path);
    return slot;
}

/* The bank page of tile `tile` of slot `slot`. */
static int frame_page(int slot, int tile)
{
    return slot * FRAME_SLOT_PAGES + tile;
}

/* The image's texels into the bank, tile by tile (see the layout above). */
static void store(uint16_t *bank, const FrameImage *image, int slot)
{
    int tile, x, y;

    for (tile = 0; tile < FRAME_COLS * FRAME_ROWS; tile++) {
        int col = tile % FRAME_COLS, row = tile / FRAME_COLS, p = frame_page(slot, tile);
        uint16_t *page = bank + (p / 8 * 256) * SOFT_GPU_WIDTH + p % 8 * 128;
        for (y = 0; y < FRAME_TILE_H; y++) {
            const unsigned char *line = image->indices + (row * FRAME_TILE_H + y) * FRAME_W + col * FRAME_TILE_W;
            for (x = 0; x < FRAME_TILE_W; x += 2) {
                unsigned char lo = line[x];
                unsigned char hi = x + 1 < FRAME_TILE_W ? line[x + 1] : 0;   /* the odd width's last pair */
                page[y * SOFT_GPU_WIDTH + x / 2] = (uint16_t)(lo | (hi << 8));
            }
        }
    }
    memcpy(&bank[FRAME_CLUT_Y * SOFT_GPU_WIDTH + slot * 256], image->clut, 256 * sizeof(uint16_t));
}

/* The slot holding `path`, its texels stored first when it is in none: the
 * slot it is in, else the one asked for longest ago. */
static int slot_for(uint16_t *bank, const char *path, const FrameImage *image)
{
    int i, slot = 0;

    for (i = 0; i < FRAME_SLOTS; i++) {
        if (!strcmp(slots[i].path, path)) {
            slots[i].used = ++slot_clock;
            return i;
        }
    }
    for (i = 1; i < FRAME_SLOTS; i++) {
        if (slots[i].used < slots[slot].used) slot = i;
    }
    store(bank, image, slot);
    snprintf(slots[slot].path, sizeof(slots[slot].path), "%s", path);
    slots[slot].used = ++slot_clock;
    return slot;
}

int CardLayoutArt_FrameTile(int col, int row, int *tpage, int *clut, int *w, int *h)
{
    uint16_t *bank;
    const char *path = CardLayout_FramePath();
    const FrameImage *image;
    int tile = row * FRAME_COLS + col, slot, page;
    int sx, sy, sx1, sy1;
    if (col < 0 || col >= FRAME_COLS || row < 0 || row >= FRAME_ROWS) return 0;
    if (!path || !*path) { LOG(LOG_CARD_LAYOUT, "FrameTile: no path"); return 0; }
    sync_with_mods();
    if (!(bank = SoftGpu_Bank(FRAME_BANK))) { LOG(LOG_CARD_LAYOUT, "FrameTile: no bank"); return 0; }
    image = image_for(path, 1);
    if (!image || !image->indices) return 0;
    slot = slot_for(bank, path, image);
    page = frame_page(slot, tile);
    LOG(LOG_CARD_LAYOUT, "FrameTile: %s slot %d tile %d", path, slot, tile);
    /* getTPage(1, 0, x, y) | bank << 11: 8bpp, this tile's page in the bank (store()'s layout) */
    *tpage = 0x80 | (FRAME_BANK << 11) | (page % 8 * 2) | (page / 8 << 4);
    *clut = (FRAME_CLUT_Y << 6) | (slot * 256 / 16);   /* getClut(256 * slot, FRAME_CLUT_Y) */
    *w = FRAME_TILE_W;
    *h = FRAME_TILE_H;
    if (image->png_w > 0 && image->png_h > 0) {
        TexturePack_BankSpritesUseOwner(TEXTURE_BANK_OWNER_LAYOUT_FRAME);
        sx = col * FRAME_TILE_W * image->png_w / FRAME_W;
        sx1 = (col + 1) * FRAME_TILE_W * image->png_w / FRAME_W;
        sy = row * FRAME_TILE_H * image->png_h / FRAME_H;
        sy1 = (row + 1) * FRAME_TILE_H * image->png_h / FRAME_H;
        TexturePack_AddBankSpriteCrop(FRAME_BANK, (page % 8 * 2) * 64, page / 8 * 256, 1, slot * 256, FRAME_CLUT_Y, 0, 0,
                                      FRAME_TILE_W, FRAME_TILE_H, path, sx, sy, sx1 - sx, sy1 - sy);
    }
    return 1;
}

/* The digit strip: one image, one page of the frame's bank, the page after
 * the frame slots', its palette right under it. */
#define DIGITS_SLOT (FRAME_SLOTS * FRAME_SLOT_PAGES)
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
    if (digits_indices && digits_png_w > 0) TexturePack_BankImagePreload(path);
}

int CardLayoutArt_DigitCell(int digit, int dim, int *tpage, int *u, int *v, int *clut, int *w, int *h)
{
    char path[1024];
    int dw, dh, step, x, y, sx, sy, sx1, sy1;
    uint16_t *bank, *page;

    if (digit < 0 || digit > 9) return 0;
    sync_with_mods();
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
                                      DIGITS_SLOT % 8 * 128, DIGITS_SLOT / 8 * 256 + DIGITS_H, *u, *v, *w, *h, path,
                                      sx, sy, sx1 - sx, sy1 - sy);
    }
    return 1;
}

/* Bank 13 allocation besides the frames (all coordinates are halfwords).
 * The frame slots take x=0..1023, y0..253 and x=0..511, y256..509, their
 * palettes x=0..767, y254; the digit strip x=512..611, y256..447, its
 * palette x=512..767, y448. The rest (the small cells' tables are above
 * small_cell):
 *
 *   type labels  8 x 32x32: x=640..895, y256..287
 *   stat labels  8 x 32x32: x=640..895, y288..319
 *   titles       3 x 48x14: x=640..783, y320..333
 *   library      8 x 7x15:  x=784..839, y320..334
 *   hand, field  27x61:     x=640..882, y335..395; x=768..875, y396..456;
 *   field back              x=612..638, y256..438
 *   their palettes          x=512..767, y449..491, one row each
 *   card back  pixels 896..1023, y256..510; CLUT 768..1023, y511
 *
 * Full 256-entry CLUT widths are called out because treating them as one
 * 128-word page was the source of prior menu/label corruption. */
int CardLayoutArt_StatLabelCell(int dfd, int *tpage, int *u, int *v, int *clut, int *w, int *h)
{
    char path[1024];
    int x, y, draw_w, draw_h;

    /* *w/*h describe the whole source cell for its UVs (the caller has the
     * on-card draw size from CardLayout_StatLabel): the manifest's 18x8 here
     * would crop the transparent top-left of a centred label image. */
    if (!CardLayout_StatLabel(dfd, path, sizeof(path), &x, &y, &draw_w, &draw_h)) return 0;
    return small_cell(&stat_label_kind, path, tpage, u, v, clut, w, h);
}

/* The "[Trap Card]"-style caption retail draws for every non-monster card:
 * a picture a kind (CARD_TYPE_MAGIC..CARD_TYPE_EQUIP), each its own palette,
 * so a style replaces any one kind's art without the others sharing it. 64x32
 * is generous for a caption this size (retail's own is 56x16 texels). */
int CardLayoutArt_TypeLabelCell(int kind, int *tpage, int *u, int *v, int *clut, int *w, int *h)
{
    char path[1024];
    int label_x, label_y, label_w, label_h, label_visible;

    if (kind < CARD_TYPE_MAGIC || kind >= CARD_TYPE_MAGIC + CARD_NON_MONSTER_TYPE_COUNT ||
        !CardLayout_TypeLabel(kind, path, sizeof(path), &label_x, &label_y, &label_w, &label_h, &label_visible))
        return 0;
    return small_cell(&type_label_kind, path, tpage, u, v, clut, w, h);
}
/* The retail name is a black indexed sprite. Copy its non-transparent texels
 * as a one-colour 8bpp mask so vertex RGB can colour it (multiplying the
 * original black pixels could never make them white). */
#define TITLE_W 96
#define TITLE_H 14
/* An attack shows two cards at once: a mask each, in its own slot (see the
 * map above CardLayoutArt_StatLabelCell), the one asked for longest ago
 * replaced. */
#define TITLE_SLOTS 3
#define TITLE_PAGE_X(slot) (640 + (slot) * TITLE_W / 2)
#define TITLE_PAGE_Y 320
#define TITLE_CLUT_Y(slot) (465 + (slot))
typedef struct {
    int source_tpage, source_u, source_v;
    unsigned int hash;
    int valid;
    unsigned long long used;
} TitleMaskCache;
static TitleMaskCache title_masks[TITLE_SLOTS];
static unsigned long long title_clock;
static int title_unavailable_reported;
static int title_unavailable_tpage, title_unavailable_u, title_unavailable_v;

static unsigned int title_source_hash(const uint16_t *vram, int src_x, int src_y, int depth, int source_u, int source_v)
{
    unsigned int hash = 2166136261u;
    int x, y;

    for (y = 0; y < TITLE_H; y++) {
        for (x = 0; x < TITLE_W; x++) {
            int tx = source_u + x, ty = source_v + y;
            uint16_t word = vram[(src_y + ty) * SOFT_GPU_WIDTH + src_x +
                                 (depth == 0 ? tx / 4 : depth == 1 ? tx / 2 : tx)];
            int pixel = depth == 0 ? (word >> ((tx & 3) * 4)) & 15 :
                        depth == 1 ? (word >> ((tx & 1) * 8)) & 255 : word;
            hash = (hash ^ (unsigned int)pixel) * 16777619u;
        }
    }
    return hash;
}

/* Title glyphs are fetched from one PS1 texture page.  Do not let a malformed
 * sprite read into the next page (or past software VRAM) while making its
 * white mask. */
static int title_source_fits(int src_x, int src_y, int depth, int source_u, int source_v)
{
    int texels_per_word;
    int last_word;

    if (depth < 0 || depth > 2 || source_u < 0 || source_v < 0 ||
        source_u > 256 - TITLE_W || source_v > 256 - TITLE_H) return 0;
    texels_per_word = depth == 0 ? 4 : depth == 1 ? 2 : 1;
    last_word = src_x + (source_u + TITLE_W - 1) / texels_per_word;
    return src_x >= 0 && src_y >= 0 && last_word < SOFT_GPU_WIDTH &&
           src_y + source_v + TITLE_H <= SOFT_GPU_HEIGHT;
}

static void title_mask_output(int slot, int *tpage, int *u, int *v, int *clut, int *w, int *h)
{
    *tpage = LAYOUT_TPAGE(TITLE_PAGE_X(slot), TITLE_PAGE_Y);
    *u = (TITLE_PAGE_X(slot) & 63) * 2;
    *v = TITLE_PAGE_Y & 255;
    *clut = (TITLE_CLUT_Y(slot) << 6) | (SMALL_CLUT_X / 16);
    *w = TITLE_W;
    *h = TITLE_H;
}

int CardLayoutArt_TitleCell(int source_tpage, int source_u, int source_v, int source_w, int source_h,
                            int *tpage, int *u, int *v, int *clut, int *w, int *h)
{
    uint16_t *bank = SoftGpu_Bank(FRAME_BANK);
    const uint16_t *vram = SoftGpu_Vram();
    int page_x, page_y = TITLE_PAGE_Y, slot;
    int depth = (source_tpage >> 7) & 3;
    int src_x = (source_tpage & 0xF) * 64, src_y = ((source_tpage >> 4) & 1) * 256;
    unsigned int hash;
    int x, y;

    if (!bank || !vram || source_w != TITLE_W || source_h != TITLE_H ||
        !title_source_fits(src_x, src_y, depth, source_u, source_v)) {
        if (!title_unavailable_reported || title_unavailable_tpage != source_tpage ||
            title_unavailable_u != source_u || title_unavailable_v != source_v) {
            LOG(LOG_CARD_LAYOUT, "title mask: unavailable (bank=%p vram=%p tpage=0x%x depth=%d uv=%d,%d size=%d,%d)",
                (void *)bank, (const void *)vram, source_tpage, depth, source_u, source_v, source_w, source_h);
            title_unavailable_reported = 1;
            title_unavailable_tpage = source_tpage;
            title_unavailable_u = source_u;
            title_unavailable_v = source_v;
        }
        return 0;
    }
    title_unavailable_reported = 0;
    hash = title_source_hash(vram, src_x, src_y, depth, source_u, source_v);
    for (slot = 0; slot < TITLE_SLOTS; slot++) {
        TitleMaskCache *mask = &title_masks[slot];
        if (mask->valid && mask->source_tpage == source_tpage && mask->source_u == source_u &&
            mask->source_v == source_v && mask->hash == hash) {
            mask->used = ++title_clock;
            title_mask_output(slot, tpage, u, v, clut, w, h);
            return 1;
        }
    }
    slot = 0;
    for (x = 1; x < TITLE_SLOTS; x++) {
        if (title_masks[x].used < title_masks[slot].used) slot = x;
    }
    page_x = TITLE_PAGE_X(slot);
    for (y = 0; y < TITLE_H; y++) {
        for (x = 0; x < TITLE_W; x += 2) {
            int j, bits[2] = {0, 0};
            for (j = 0; j < 2; j++) {
                int tx = source_u + x + j, ty = source_v + y;
                uint16_t word = vram[(src_y + ty) * SOFT_GPU_WIDTH + src_x +
                                     (depth == 0 ? tx / 4 : depth == 1 ? tx / 2 : tx)];
                /* A 15-bit opaque black texel is 0x8000: it is a visible
                 * black glyph, not transparent.  Test the whole word there
                 * so such a title becomes a white mask too. */
                int pixel = depth == 0 ? (word >> ((tx & 3) * 4)) & 15 :
                            depth == 1 ? (word >> ((tx & 1) * 8)) & 255 : word;
                bits[j] = pixel != 0;
            }
            bank[(page_y + y) * SOFT_GPU_WIDTH + page_x + x / 2] = (uint16_t)(bits[0] | bits[1] << 8);
        }
    }
    memset(&bank[TITLE_CLUT_Y(slot) * SOFT_GPU_WIDTH + SMALL_CLUT_X], 0, 256 * sizeof(uint16_t));
    bank[TITLE_CLUT_Y(slot) * SOFT_GPU_WIDTH + SMALL_CLUT_X + 1] = 0x7FFF;
    title_masks[slot].source_tpage = source_tpage;
    title_masks[slot].source_u = source_u;
    title_masks[slot].source_v = source_v;
    title_masks[slot].hash = hash;
    title_masks[slot].valid = 1;
    title_masks[slot].used = ++title_clock;
    title_mask_output(slot, tpage, u, v, clut, w, h);
    LOG(LOG_CARD_LAYOUT, "title mask: bank=%d tpage=0x%x clut=0x%x from tpage=0x%x depth=%d uv=%d,%d",
        FRAME_BANK, *tpage, *clut, source_tpage, depth, source_u, source_v);
    return 1;
}

/* TexturePack_BankSpritesClear owns the HD registration lifetime, separately
 * from this file's indexed-pixel cache.  Forget only the latter's "uploaded"
 * markers when that registry is cleared: the next draw reuses decoded pixels
 * but writes/registers them again instead of silently falling back to the
 * software texture. */
static void layout_bank_registrations_lost(void)
{
    int i, j;

    back_bank_path[0] = 0;
    digits_bank_path[0] = 0;
    for (i = 0; i < (int)(sizeof(small_kinds) / sizeof(small_kinds[0])); i++) {
        for (j = 0; j < small_kinds[i]->count; j++) small_kinds[i]->slots[j].stored = 0;
    }
    for (i = 0; i < TITLE_SLOTS; i++) title_masks[i].valid = 0;
}

/* Everything this file keeps of a picture, keyed by its path: a mod's pictures
 * are read once and the path says nothing of the file's content, so a mod
 * turned off and on with its PNGs edited in between (or another mod's under the
 * same path) would go on drawing what was read first.  Drops the decoded
 * pixels, the bank's "stored" bookkeeping, the HD registrations and every
 * decoded PNG the texture pack keeps unused -- not only those still named here:
 * a back, a digit strip or a sprite evicted earlier waits there as a spare, and
 * would come back stale by its path.  The next draw reads them again.  Nothing
 * is read here and the bank's texels stay until a draw writes over them. */
void CardLayoutArt_Clear(void)
{
    int i, j;

    TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_LAYOUT_FRAME);
    TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_LAYOUT_DIGITS);
    TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_LAYOUT_BACK);
    TexturePack_BankImagesForgetSpare();
    for (i = 0; i < FRAME_CACHE; i++) free(images[i].indices);
    memset(images, 0, sizeof(images));
    memset(slots, 0, sizeof(slots));
    for (i = 0; i < (int)(sizeof(small_kinds) / sizeof(small_kinds[0])); i++) {
        for (j = 0; j < small_kinds[i]->count; j++) free(small_kinds[i]->slots[j].indices);
        memset(small_kinds[i]->slots, 0, (size_t)small_kinds[i]->count * sizeof(SmallSlot));
    }
    free(back_indices);
    back_indices = NULL;
    back_path[0] = 0;
    back_bank_path[0] = 0;
    back_png_w = back_png_h = 0;
    free(digits_indices);
    digits_indices = NULL;
    digits_path[0] = 0;
    digits_bank_path[0] = 0;
    digits_png_w = digits_png_h = 0;
    for (i = 0; i < TITLE_SLOTS; i++) title_masks[i].valid = 0;
}

/* Mods_Changes moves when a mod goes on or off, and not otherwise, so a game
 * with no mod change (or none at all) never gets here past the compare. */
static void sync_with_mods(void)
{
    static unsigned seen;
    unsigned now = Mods_Changes();

    if (now == seen) return;
    seen = now;
    CardLayoutArt_Clear();
}

void CardLayoutArt_Prewarm(void)
{
    static unsigned int frame;
    char paths[FRAME_CACHE][1024];
    char digits[1024] = "";
    int i, count, dw, dh, dstep;

    sync_with_mods();
    if (++frame % 30) return;   /* the layout changes with a setting, not a frame */
    if (CardLayout_Digits(digits, sizeof(digits), &dw, &dh, &dstep) && strcmp(digits_path, digits)) {
        digits_build(digits);
        return;
    } else if (!digits[0]) {
        TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_LAYOUT_DIGITS);
        digits_bank_path[0] = 0;
    }
    count = CardLayout_FramePaths(paths, FRAME_CACHE);
    /* An icon/hand-only style has no FramePaths() entry but still owns HD
     * bank sprites.  Clear that owner only when the layout itself has been
     * disabled, not merely because it supplies no large frame image. */
    if (!count && !CardLayout_Active()) {
        TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_LAYOUT_FRAME);
        TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_LAYOUT_BACK);
        layout_bank_registrations_lost();
    }
    for (i = 0; i < count; i++) {
        if (!image_for(paths[i], 0)) {
            image_for(paths[i], 1);
            return;   /* one a call: each is a hitch of its own */
        }
    }
}
