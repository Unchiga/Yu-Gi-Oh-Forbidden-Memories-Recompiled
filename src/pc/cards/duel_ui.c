/* The mods' "ui" key in the duel (duel_ui.h). Every element is a display
 * object the game draws through DisplayObject_RenderSpriteSheet; the ones a
 * mod changes are drawn here instead, from the very sprites the game would
 * have sorted (DisplayObject_Capture catches them), moved, sized and
 * colored, so whatever the game does to them -- its slides, the panel's
 * palette at the turn, the cursor's animation -- shows through. The
 * life-point panel is one 64 x 40 sprite of both sides; it is cut into its
 * halves here, each with its own digits (Duel_DrawLifePointsAndDeckCounts
 * asks DuelUi_DrawDigits). The card bar's words, numbers and icons are
 * not the bar's but a text's, written over it: its parts are moved as
 * that text is drawn (the end of this file).
 *
 * A mod's picture is made into 8-bit texels at the size it is drawn, and
 * again up to four times that for an internal resolution above the
 * console's, in texture bank 14 of the software GPU (soft_gpu.h) beside the
 * mods' star icons: none of the duel's VRAM is touched. */
#include "duel_ui.h"
#include "art.h"
#include "stars.h"
#include "pc/mods/mods.h"
#include "pc/platform/settings.h"
#include "pc/platform/ui_config.h"
#include "pc/render/soft_gpu.h"
#include "pc/render/texture_pack.h"
#include "types.h"
#include "psyq/libgte.h"
#include "psyq/libgpu.h"
#include "psyq/libgs.h"
#include "game/display_object_layout.h"
#include "game/display_object_packet_submit.h"
#include "game/display_object_render_sprite_sheet.h"
#include "game/duel_effect.h"
#include "game/duel_init_scene.h"
#include "game/ordering_tables.h"
#include "game/text_encode_decimal_digits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned char D_8009B26C;

/* The panel (Duel_InitScene): its sprite's halves are rows 0-19, the
 * opponent's LP and COM, and 20-39, YOU and the player's LP; the fifth
 * digit widens each by one digit to the left from column 32, as
 * duel_draw_status_numbers.c does the whole. */
enum { PANEL_W = 64, PANEL_H = 40, HALF_H = 20, PANEL_SPLIT = 32, DIGIT_W = 8 };
/* Bank 14 (star_icons.c keeps x 0-63 and the palette rows 256-271): the
 * pictures on shelves from x 256 to the right edge, each starting on a page
 * (64 words) and inside one of the two rows of pages, their palettes in rows
 * 480-511 at x 0. A picture is drawn in strips of 128 texels, each inside
 * its page, as title_images.c draws the title's. */
enum { BANK = 14, SHELF_X = 256, PAGE_WORDS = 64, BAND = 256, CLUT_ROW = 480, CLUT_ROWS = 32, STRIP = 128,
       MAX_W = (SOFT_GPU_WIDTH - SHELF_X) * 2, MAX_H = BAND, SIZES = 4 };

DisplayObjectCapture *DisplayObject_Capture = NULL;

static int prepared, wide;
static const UiConfig *config;

typedef struct {
    int made, ready, w, h;         /* made: tried for this w x h */
    int png_w, png_h;
    int x, y, clut_y;              /* its place in the bank (words) and its palette's row */
} BankPicture;
/* Each element's at the console's resolution and above it, at up to SIZES
 * sizes (the card bar has two looks, the hand's and the field's). */
static BankPicture pictures[UI_ELEMENTS][2][SIZES];
/* The shelves: in each band of 256 rows, the next free x and the shelf's
 * top and height. */
static struct { int x, top, height; } shelves[2];
static int cluts;

void DuelUi_Prepare(void)
{
    int band;
    config = UiConfig_Load();
    TexturePack_BankSpritesClear(TEXTURE_BANK_OWNER_DUEL_UI);
    memset(pictures, 0, sizeof(pictures));
    for (band = 0; band < 2; band++) {
        shelves[band].x = SHELF_X;
        shelves[band].top = band * BAND;
        shelves[band].height = 0;
    }
    cluts = 0;
    prepared = 1;
}

static const UiElement *element(int which)
{
    if (!prepared) DuelUi_Prepare();
    return &config->element[which];
}

/* Whether `which` changes anything at all: an element a mod names with
 * nothing in it, or only what it already is, is the game's to draw. */
static int changed(int which)
{
    const UiElement *e = element(which);
    return e->set && (e->x || e->y || e->scale != 100 || e->tint != 0xFFFFFF || e->hidden || e->image.file[0] ||
                      ((which == UI_LP_OPPONENT || which == UI_LP_PLAYER) && (e->digits != 0xFFFFFF || e->label[0])));
}

void DuelUi_Offset(int which, int *x, int *y)
{
    *x = *y = 0;
    if (which < 0 || which >= UI_ELEMENTS || !changed(which)) return;
    *x = element(which)->x;
    *y = element(which)->y;
}

const char *DuelUi_Label(int which)
{
    const UiElement *e = element(which ? UI_LP_PLAYER : UI_LP_OPPONENT);
    return e->set && e->label[0] ? e->label : NULL;
}

/* --- where things go ----------------------------------------------------- */

/* An element's move and size: a point x, y goes to px + dx + (x - px) *
 * scale / 100, about its middle px, py. */
typedef struct {
    int px, py, dx, dy, scale;
    uint32_t tint;
} Place;

static int place_x(const Place *p, int x)
{
    int d = (x - p->px) * p->scale;
    return p->px + p->dx + (d >= 0 ? (d + 50) / 100 : -((-d + 50) / 100));
}

static int place_y(const Place *p, int y)
{
    int d = (y - p->py) * p->scale;
    return p->py + p->dy + (d >= 0 ? (d + 50) / 100 : -((-d + 50) / 100));
}

static u8 tinted(int level, uint32_t tint, int shift)
{
    return (u8)(level * (int)(tint >> shift & 0xFF) / 0xFF);
}

/* The screen's rule for a sprite (DisplayObject_RenderSpriteSheet's, for
 * the screen's sprites): one wholly off the 320 x 240 is not drawn, one
 * partly on is drawn whole (widescreen shows past the sides). Here it is
 * applied to where a piece is drawn, not where the game has it. */
static int off_screen(int x0, int y0, int x1, int y1)
{
    return x1 <= 0 || x0 >= 320 || y1 <= 0 || y0 >= 240;
}

/* A sprite's texels u, v (w x h of them, from its own u, v) drawn at x, y
 * (the game's place for that corner) as `place` puts them: the game's own
 * fast sprite at its own size, else a quad whose far edges are the texel
 * after the last, one texel to a pixel at 100. */
static void draw_piece(const GsSPRITE *sprite, int mode, int du, int dv, int w, int h, int x, int y,
                       const Place *place, s32 ot, s32 depth)
{
    GsSPRITE piece = *sprite;
    POLY_FT4 quad;
    u32 attribute = sprite->attribute;
    int u0, u1, v0, v1, x0 = place_x(place, x), y0 = place_y(place, y), x1 = place_x(place, x + w),
        y1 = place_y(place, y + h);
    piece.u = (u8)(sprite->u + du);
    piece.v = (u8)(sprite->v + dv);
    piece.w = (u16)w;
    piece.h = (u16)h;
    piece.r = tinted(sprite->r, place->tint, 16);
    piece.g = tinted(sprite->g, place->tint, 8);
    piece.b = tinted(sprite->b, place->tint, 0);
    if (place->scale == 100 || mode == 3) {
        if (off_screen(x0, y0, x0 + w, y0 + h)) return;
        piece.x = (short)x0;
        piece.y = (short)y0;
        if (mode == 2) GsSortFlipSprite(&piece, (GsOT *G32)ot, (u16)depth);
        else if (mode == 3) GsSortSprite(&piece, (GsOT *G32)ot, (u16)depth);
        else GsSortFastSprite(&piece, (GsOT *G32)ot, (u16)depth);
        return;
    }
    if (x1 <= x0 || y1 <= y0 || off_screen(x0, y0, x1, y1)) return;
    memset(&quad, 0, sizeof(quad));
    setPolyFT4(&quad);
    if (attribute & 0x40000000) setSemiTrans(&quad, 1);
    /* Its colors as they are: unmodulated, so not dithered, as the sprite
     * it stands for is not. */
    if ((attribute & 0x40) || (piece.r == 0x80 && piece.g == 0x80 && piece.b == 0x80)) setShadeTex(&quad, 1);
    quad.r0 = piece.r;
    quad.g0 = piece.g;
    quad.b0 = piece.b;
    quad.tpage = (u16)((sprite->tpage & 0x1F) | ((attribute >> 17) & 0x180) | ((attribute >> 23) & 0x60));
    quad.clut = getClut(sprite->cx, sprite->cy);
    /* As GsSortFlipSprite draws a sprite: from its first texel to the one
     * after its last, or mirrored (the attribute's 0x800000, 0x400000) from
     * its last to the one before its first; at a page's edge (255) the last. */
    if (attribute & 0x800000) {
        u0 = piece.u + w - 1;
        u1 = piece.u ? piece.u - 1 : 0;
    } else {
        u0 = piece.u;
        u1 = u0 + w > 255 ? u0 + w - 1 : u0 + w;
    }
    if (attribute & 0x400000) {
        v0 = piece.v + h - 1;
        v1 = piece.v ? piece.v - 1 : 0;
    } else {
        v0 = piece.v;
        v1 = v0 + h > 255 ? v0 + h - 1 : v0 + h;
    }
    quad.x0 = quad.x2 = (short)x0;
    quad.x1 = quad.x3 = (short)x1;
    quad.y0 = quad.y1 = (short)y0;
    quad.y2 = quad.y3 = (short)y1;
    quad.u0 = quad.u2 = (u8)u0;
    quad.u1 = quad.u3 = (u8)u1;
    quad.v0 = quad.v1 = (u8)v0;
    quad.v2 = quad.v3 = (u8)v1;
    GsSortPoly(&quad, (GsOT *G32)ot, (u16)depth);
}

/* --- the mods' pictures ---------------------------------------------------- */

static int internal_scale(void)
{
    int scale = Settings_Get(SET_INTERNAL_SCALE);
    return scale > 1 ? scale : 1;
}

/* A place for w x h texels (8 bits: two a word) on the shelves. */
static int shelve(int w, int h, int *x, int *y)
{
    int band, words = (w + 1) / 2;
    words = (words + PAGE_WORDS - 1) / PAGE_WORDS * PAGE_WORDS;
    for (band = 0; band < 2; band++) {
        if (shelves[band].x + words > SOFT_GPU_WIDTH) {
            shelves[band].x = SHELF_X;
            shelves[band].top += shelves[band].height;
            shelves[band].height = 0;
        }
        if (shelves[band].top + h > (band + 1) * BAND || shelves[band].x + words > SOFT_GPU_WIDTH) continue;
        *x = shelves[band].x;
        *y = shelves[band].top;
        shelves[band].x += words;
        if (h > shelves[band].height) shelves[band].height = h;
        return 1;
    }
    return 0;
}

/* Element `which`'s PNG made at w x h (hd 0), or at up to four times that
 * (hd 1, the internal resolution's), in the bank; NULL when it cannot be. */
static const BankPicture *picture(int which, int w, int h, int hd)
{
    BankPicture *picture = NULL;
    const UiImage *image = &element(which)->image;
    unsigned char *texels;
    unsigned short clut[256];
    uint16_t *bank;
    char why[1300];
    int factor = 1, png_w, png_h, y, x;
    if (!image->file[0] || w < 1 || h < 1) return NULL;
    if (hd) {
        factor = internal_scale() < 4 ? internal_scale() : 4;
        while (factor > 1 && (w * factor > MAX_W || h * factor > MAX_H)) factor--;
        if (factor < 2) return NULL;
    }
    for (x = 0; x < SIZES; x++) {
        BankPicture *size = &pictures[which][hd][x];
        if (size->made && size->w == w * factor && size->h == h * factor) return size->ready ? size : NULL;
        if (!size->made && !picture) picture = size;
    }
    if (!CardArt_ImageSize(image->file, &png_w, &png_h)) return NULL;
    if (!picture) {
        Mods_Note(image->mod, "ui: %s is drawn at more than %d sizes", image->file, SIZES);
        return NULL;
    }
    picture->made = 1;
    picture->ready = 0;
    picture->w = w * factor;
    picture->h = h * factor;
    if (picture->w > MAX_W || picture->h > MAX_H) {
        Mods_Note(image->mod, "ui: %s is drawn larger than %d x %d", image->file, MAX_W, MAX_H);
        return NULL;
    }
    /* Above the console's resolution only when the PNG has the detail. */
    if (hd && png_w <= w && png_h <= h) return NULL;
    if (cluts == CLUT_ROWS || !shelve(picture->w, picture->h, &picture->x, &picture->y)) {
        Mods_Note(image->mod, "ui: no room left for %s", image->file);
        return NULL;
    }
    picture->clut_y = CLUT_ROW + cluts++;
    picture->png_w = png_w;
    picture->png_h = png_h;
    if (!(bank = SoftGpu_Bank(BANK)) || !(texels = malloc((size_t)picture->w * picture->h))) return NULL;
    if (!CardArt_IndexedImage(image->file, picture->w, picture->h, texels, clut, why, sizeof(why))) {
        if (!hd) Mods_Note(image->mod, "ui: %s", why);
        free(texels);
        return NULL;
    }
    for (y = 0; y < picture->h; y++) {
        uint16_t *row = &bank[(picture->y + y) * SOFT_GPU_WIDTH + picture->x];
        for (x = 0; x < picture->w; x += 2)
            row[x / 2] = (uint16_t)(texels[y * picture->w + x] |
                                    (x + 1 < picture->w ? texels[y * picture->w + x + 1] << 8 : 0));
    }
    memcpy(&bank[picture->clut_y * SOFT_GPU_WIDTH], clut, sizeof(clut));
    free(texels);
    picture->ready = 1;
    return picture;
}

/* The element's picture over x, y, w x h (the game's place), where `place`
 * puts that, or at the mod's "width" and "height" about its middle. 0 when
 * there is none to draw (the game's own is drawn then). */
static int draw_picture(int which, int x, int y, int w, int h, const Place *place, s32 ot, s32 depth)
{
    const UiImage *image = &element(which)->image;
    const BankPicture *bank_picture;
    POLY_FT4 strip;
    int x0, y0, x1, y1, left;
    if (!image->file[0]) return 0;
    if (image->width || image->height) {
        int mid_x = x + w / 2, mid_y = y + h / 2, given_w = image->width, given_h = image->height;
        if (!given_w) given_w = w * given_h / h;
        if (!given_h) given_h = h * given_w / w;
        w = given_w;
        h = given_h;
        x = mid_x - w / 2;
        y = mid_y - h / 2;
    }
    x0 = place_x(place, x);
    y0 = place_y(place, y);
    x1 = place_x(place, x + w);
    y1 = place_y(place, y + h);
    if (x1 <= x0 || y1 <= y0 || off_screen(x0, y0, x1, y1)) return 1;
    bank_picture = internal_scale() > 1 ? picture(which, x1 - x0, y1 - y0, 1) : NULL;
    if (!bank_picture && !(bank_picture = picture(which, x1 - x0, y1 - y0, 0))) return 0;
    /* The pads too: a bank's polygon reads its third one as a fade
     * (soft_gpu.h, SOFT_GPU_FADE). */
    memset(&strip, 0, sizeof(strip));
    setPolyFT4(&strip);
    strip.r0 = tinted(0x80, place->tint, 16);
    strip.g0 = tinted(0x80, place->tint, 8);
    strip.b0 = tinted(0x80, place->tint, 0);
    strip.clut = getClut(0, bank_picture->clut_y);
    if (place->tint == 0xFFFFFF) setShadeTex(&strip, 1);    /* as it is: not dithered */
    /* Each strip from its first texel to the one after its last: one texel
     * to a pixel when the picture is drawn at its own size. */
    for (left = 0; left < bank_picture->w; left += STRIP) {
        int width = bank_picture->w - left < STRIP ? bank_picture->w - left : STRIP;
        int word = bank_picture->x + left / 2;
        int page_x = word & ~(PAGE_WORDS - 1);
        int page_y = bank_picture->y & ~(BAND - 1);
        int source_x = left * bank_picture->png_w / bank_picture->w;
        int source_x1 = (left + width) * bank_picture->png_w / bank_picture->w;
        strip.tpage = (u16)(getTPage(1, 0, word, bank_picture->y & ~(BAND - 1)) | (BANK << 11));
        strip.x0 = strip.x2 = (short)(x0 + (x1 - x0) * left / bank_picture->w);
        strip.x1 = strip.x3 = (short)(x0 + (x1 - x0) * (left + width) / bank_picture->w);
        strip.y0 = strip.y1 = (short)y0;
        strip.y2 = strip.y3 = (short)y1;
        strip.u0 = strip.u2 = 0;
        strip.u1 = strip.u3 = (u8)width;
        strip.v0 = strip.v1 = (u8)(bank_picture->y & (BAND - 1));
        strip.v2 = strip.v3 = (u8)((bank_picture->y & (BAND - 1)) + bank_picture->h > 255
                                       ? 255 : (bank_picture->y & (BAND - 1)) + bank_picture->h);
        TexturePack_BankSpritesUseOwner(TEXTURE_BANK_OWNER_DUEL_UI);
        TexturePack_AddBankSpriteCrop(BANK, page_x, page_y, 1, (word - page_x) * 2,
                                      bank_picture->y & (BAND - 1), width, bank_picture->h, image->file,
                                      source_x, 0, source_x1 - source_x, bank_picture->png_h);
        GsSortPoly(&strip, (GsOT *G32)ot, (u16)depth);
    }
    return 1;
}

/* --- the elements ------------------------------------------------------- */

static int in_duel(void)
{
    return (D_8009B26C & 0x1F) == 3;
}

/* Which element the object is: the panel (both halves, UI_LP_OPPONENT) and
 * the FIELD box by the duel's own pointers to them and what they show, the
 * card bar and the cursors by what they show (Duel_InitScene,
 * func_80018150, DuelScene_UpdateHandActions, func_800234E4). */
static int element_of(const DisplayObject *o)
{
    if (o == D_8009B21C && o->field_67 == 4 && o->field_66 == 11) return UI_LP_OPPONENT;
    if (o == D_8009B214 && o->field_67 == 4 && o->field_68 == 2) return UI_FIELD;
    if (o->field_67 == 0 && o->field_68 == 1 && o->field_66 == 0x1F && o->field_40.h.field_40 == 256) return UI_CARD_BAR;
    if (o->field_67 == 3 && o->field_68 == 0 && o->field_66 == 0xB) return UI_HAND_CURSOR;
    if (o->field_67 == 4 && o->field_68 == 3 && o->field_66 == 0x1F) return UI_FIELD_CURSOR;
    return -1;
}

static void make_place(int which, int px, int py, Place *place)
{
    const UiElement *e = element(which);
    int on = changed(which);
    place->px = px;
    place->py = py;
    place->dx = on ? e->x : 0;
    place->dy = on ? e->y : 0;
    place->scale = on ? e->scale : 100;
    place->tint = on ? e->tint : 0xFFFFFF;
}

/* The sprites the game would sort for `object` (it sorts nothing itself). */
static int capture(DisplayObject *object, s32 ot, s32 depth, DisplayObjectCapture *caught)
{
    caught->count = 0;
    DisplayObject_Capture = caught;
    DisplayObject_RenderSpriteSheet(object, ot, depth);
    DisplayObject_Capture = NULL;
    return caught->count;
}

static int sheet_extent(DisplayObject *object, int *x0, int *y0, int *x1, int *y1);

/* All of a screen sprite sheet's sprites, wherever the game has it, moved
 * by dx, dy: taken with the object where the screen keeps them all (the
 * game leaves out a sprite only when it is wholly off the screen) and put
 * back, so the screen's rule is applied where each is drawn (draw_piece),
 * as a slide carries a moved or sized one off. Any other kind of object:
 * its sprites as the game sorts them. */
static int capture_all(DisplayObject *object, s32 ot, s32 depth, int dx, int dy, DisplayObjectCapture *caught)
{
    int x0, y0, x1, y1, sx = 0, sy = 0, i;
    if (sheet_extent(object, &x0, &y0, &x1, &y1)) {
        if (x0 < 0 || x1 > 320) sx = 160 - (x0 + x1) / 2;
        if (y0 < 0 || y1 > 240) sy = 120 - (y0 + y1) / 2;
    }
    object->field_30.h.field_30 += sx;
    object->field_30.h.field_32 += sy;
    capture(object, ot, depth, caught);
    object->field_30.h.field_30 -= sx;
    object->field_30.h.field_32 -= sy;
    for (i = 0; i < caught->count; i++) {
        GsSPRITE *sprite = (GsSPRITE *)&caught->sprite[i];
        sprite->x = (short)(sprite->x + dx - sx);
        sprite->y = (short)(sprite->y + dy - sy);
    }
    return caught->count;
}

/* The panel as its two halves, each moved, sized and colored on its own,
 * or a picture, or nothing; widened a digit for a fifth (as
 * Duel_DrawWideLifePointPanel widens the whole). Submitted last first, as
 * the ordering table draws them the other way round. */
static void draw_panel(DisplayObject *object, s32 ot, s32 depth)
{
    DisplayObjectCapture caught;
    const GsSPRITE *sprite;
    int half;
    if (capture_all(object, ot, depth, 0, 0, &caught) != 1 || caught.sprite[0].extent.wh.w.word != PANEL_W ||
        caught.sprite[0].extent.wh.h != PANEL_H) {
        /* Not the plain panel: the game's own, as the game leaves them out. */
        int i;
        for (i = 0; i < caught.count; i++) {
            const GsSPRITE *s = (const GsSPRITE *)&caught.sprite[i];
            if (!off_screen(s->x, s->y, s->x + s->w, s->y + s->h))
                DisplayObject_SubmitPacket(&caught.sprite[i], 0, ot, caught.mode[i], 0);
        }
        return;
    }
    sprite = (const GsSPRITE *)&caught.sprite[0];
    for (half = 1; half >= 0; half--) {
        int which = half ? UI_LP_PLAYER : UI_LP_OPPONENT, top = sprite->y + half * HALF_H;
        int mode = (int)((u32)caught.mode[0] >> 16);
        Place place;
        make_place(which, sprite->x + PANEL_W / 2, top + HALF_H / 2, &place);
        if (changed(which) && element(which)->hidden) continue;
        if (changed(which) && draw_picture(which, sprite->x, top, PANEL_W, HALF_H, &place, ot, depth)) continue;
        if (wide) {
            draw_piece(sprite, mode, PANEL_SPLIT, half * HALF_H, PANEL_W - PANEL_SPLIT, HALF_H, sprite->x + PANEL_SPLIT,
                       top, &place, ot, depth);
            draw_piece(sprite, mode, 0, half * HALF_H, PANEL_W, HALF_H, sprite->x - DIGIT_W, top, &place, ot, depth);
        }
        draw_piece(sprite, mode, 0, half * HALF_H, PANEL_W, HALF_H, sprite->x, top, &place, ot, depth);
    }
}

/* Where all of a plain screen sprite sheet's parts go (as
 * DisplayObject_RenderSpriteSheet places them), whether or not the screen
 * culls them: 0 for any other kind of object. */
static int sheet_extent(DisplayObject *object, int *x0, int *y0, int *x1, int *y1)
{
    const SpriteSheetHeader *header = (SpriteSheetHeader *G32)object->field_4C;
    const SpriteSheetPart *part;
    int i;
    if (!header || !header->count || !(object->attribute & 0x08000000) ||
        (object->flags & DISPLAY_OBJECT_FLAG_CLIP_TEST) || !(object->flags & DISPLAY_OBJECT_FLAG_SCREEN_SPACE))
        return 0;
    part = (const SpriteSheetPart *)(header + 1);
    *x0 = *y0 = 0x7FFF;
    *x1 = *y1 = -0x8000;
    for (i = 0; i < header->count; i++, part++) {
        int w = ((part->size >> 2) & 0x78) + 8, h = ((part->size >> 6) & 0x78) + 8, dx = part->dx, dy = part->dy;
        if (header->flags & 0x10) {
            dx = (u8)part->dx | ((part->cell & 0xC000) >> 6);
            if (dx & 0x200) dx |= ~0x1FF;
            dy = (u8)part->dy | ((part->size & 0xC000) >> 6);
            if (dy & 0x200) dy |= ~0x1FF;
        }
        if (object->attribute & 0x800000) dx = -(dx + w);
        dx += (s16)object->field_30.h.field_30;
        dy += (s16)object->field_30.h.field_32;
        if (dx < *x0) *x0 = dx;
        if (dy < *y0) *y0 = dy;
        if (dx + w > *x1) *x1 = dx + w;
        if (dy + h > *y1) *y1 = dy + h;
    }
    return 1;
}

int DuelUi_RenderObject(DisplayObject *object, s32 ot, s32 depth)
{
    DisplayObjectCapture caught;
    const UiElement *e;
    int which, i, x0, y0, x1, y1;
    Place place;
    if (!prepared) DuelUi_Prepare();
    if (!config->any || !in_duel() || (which = element_of(object)) < 0) return 0;
    if (which == UI_LP_OPPONENT) {
        if (!changed(UI_LP_OPPONENT) && !changed(UI_LP_PLAYER)) return 0;
        draw_panel(object, ot, depth);
        return 1;
    }
    if (!changed(which)) return 0;
    e = element(which);
    if (e->hidden || !capture_all(object, ot, depth, e->x, e->y, &caught)) return 1;
    /* Its place, moved: where all of a sheet's parts go, or where all of
     * its sprites are (a variant the bar takes in some views). */
    if (sheet_extent(object, &x0, &y0, &x1, &y1)) {
        x0 += e->x;
        x1 += e->x;
        y0 += e->y;
        y1 += e->y;
    } else {
        x0 = y0 = 0x7FFF;
        x1 = y1 = -0x8000;
        for (i = 0; i < caught.count; i++) {
            const GsSPRITE *sprite = (const GsSPRITE *)&caught.sprite[i];
            if (sprite->x < x0) x0 = sprite->x;
            if (sprite->y < y0) y0 = sprite->y;
            if (sprite->x + sprite->w > x1) x1 = sprite->x + sprite->w;
            if (sprite->y + sprite->h > y1) y1 = sprite->y + sprite->h;
        }
    }
    make_place(which, (x0 + x1) / 2, (y0 + y1) / 2, &place);
    place.dx = place.dy = 0;
    if (draw_picture(which, x0, y0, x1 - x0, y1 - y0, &place, ot, depth)) return 1;
    for (i = 0; i < caught.count; i++) {
        const GsSPRITE *s = (const GsSPRITE *)&caught.sprite[i];
        draw_piece(s, (int)((u32)caught.mode[i] >> 16), 0, 0, s->w, s->h, s->x, s->y, &place, ot, depth);
    }
    return 1;
}

static int panel_changed(void)
{
    if (!prepared) DuelUi_Prepare();
    return config->any && (changed(UI_LP_OPPONENT) || changed(UI_LP_PLAYER));
}

int DuelUi_PanelCut(void)
{
    return prepared && config->any && (changed(UI_LP_OPPONENT) || changed(UI_LP_PLAYER));
}

int DuelUi_PanelDrawn(int digits)
{
    wide = digits > 4;
    return panel_changed();
}

/* The panel's top left: its object's place and its one part's offset. */
static int panel_corner(DisplayObject *panel, int *x, int *y)
{
    const SpriteSheetHeader *header = (SpriteSheetHeader *G32)panel->field_4C;
    const SpriteSheetPart *part;
    if (!header || header->count != 1 || (header->flags & 0x10)) return 0;
    part = (const SpriteSheetPart *)(header + 1);
    *x = (s16)panel->field_30.h.field_30 + part->dx;
    *y = (s16)panel->field_30.h.field_32 + part->dy;
    return 1;
}

int DuelUi_DrawDigits(int side, DisplayObject *panel, void *digit_sprite, int value, int count)
{
    GsSPRITE *digit = digit_sprite;
    int which = side ? UI_LP_OPPONENT : UI_LP_PLAYER, half = side ? 0 : 1, x, y, i;
    const UiElement *e;
    u8 temp[8];
    Place place;
    if (!panel_changed() || !changed(which) || !panel_corner(panel, &x, &y)) return 0;
    e = element(which);
    if (e->hidden) return 1;
    /* Each digit by the screen's rule where it is drawn (draw_piece): off
     * it with its half on a slide away. */
    make_place(which, x + PANEL_W / 2, y + half * HALF_H + HALF_H / 2, &place);
    place.tint = e->digits;
    Text_EncodeDecimalDigits(value, count, temp);
    for (i = count - 1; i >= 0; i--) {
        digit->u = (u8)(temp[i] << 3);
        draw_piece(digit, 1, 0, 0, digit->w, digit->h, digit->x, digit->y, &place, (s32)D_800E9D90[panel->ot_index],
                   panel->field_14);
        digit->x += 8;
    }
    return 1;
}

/* --- the card bar's parts --------------------------------------------------- */

/* The strings func_80023144 writes the bar with: the hand's and the
 * field's, a monster's and another card's, with the Swords' turns and the
 * GUARDIAN STAR line over them (0x52 to 0x55). */
enum { BAR_TEXT_FIRST = 0x50, BAR_TEXT_LAST = 0x55 };
/* The 8 x 8 font's sword and shield, each at the head of its row of
 * digits (the strings' L09D3). */
enum { SJIS_SWORD = 0x8189, SJIS_SHIELD = 0x818A };
/* An icon past the type icons: the stars' are 0x18 to 0x21 (star + 0x17);
 * a word is 0x17 with the card's type, 0x14 to 0x17, beside it
 * (func_80035E20). */
enum { STAR_ICON_FIRST = 0x18, KIND_TYPE_FIRST = 0x14 };

/* Per channel: the depth of the name's stream while it is read (0 for
 * none), and the row of the 8 x 8 font being laid out. */
static int name_depth[DUEL_EFFECT_CHANNEL_COUNT];
static int stats_row[DUEL_EFFECT_CHANNEL_COUNT];

static int bar_text(const DuelEffectChannel *channel)
{
    return channel->field_36 >= BAR_TEXT_FIRST && channel->field_36 <= BAR_TEXT_LAST;
}

static int parts_on(void)
{
    return prepared && config->parts && in_duel();
}

void DuelUi_NameStream(void *at, int on)
{
    DuelEffectChannel *channel = at;
    if (channel->index_57 >= DUEL_EFFECT_CHANNEL_COUNT) return;
    name_depth[channel->index_57] = on ? channel->stream_58 + 1 : 0;
    if (!on) stats_row[channel->index_57] = 0;
}

void DuelUi_TagEntry(void *at, void *added)
{
    DuelEffectChannel *channel = at;
    DuelEffectEntry *entry = added;
    int index = channel->index_57, part = -1;
    if (!parts_on() || index >= DUEL_EFFECT_CHANNEL_COUNT || !bar_text(channel)) return;
    if (name_depth[index] && channel->stream_58 + 1 < name_depth[index]) name_depth[index] = 0;
    if (entry->flags_11 == 0xC0) {
        /* The 8 x 8 font: the sword and the shield each begin their row. */
        if (entry->code_00 == SJIS_SWORD) stats_row[index] = 1 + UI_PART_ATK;
        else if (entry->code_00 == SJIS_SHIELD) stats_row[index] = 1 + UI_PART_DEF;
        part = stats_row[index] - 1;
    } else if (entry->flags_11 == 0xA0) {
        if (entry->field_17 >= KIND_TYPE_FIRST && entry->field_10 < STAR_ICON_FIRST) part = UI_PART_KIND;
        else if (entry->field_10 >= STAR_ICON_FIRST || (entry->code_00 & 0xFFF0) == STARS_ICON_CODE)
            part = UI_PART_STARS;
        else part = UI_PART_TYPE;
    } else if (entry->flags_11 == 0x80 && name_depth[index]) {
        part = UI_PART_NAME;
    }
    entry->pad_19[0] = (u8)(part + 1);
    /* Letters since F8 07, the name's limit, just before it. */
    entry->pad_19[1] = channel->field_60;
}

int DuelUi_BarEntry(DisplayObject *text, const void *at, void *drawn)
{
    const DuelEffectEntry *entry = at;
    GsSPRITE *sprite = drawn;
    const UiPart *part;
    int which;
    if (!parts_on() || text->field_67 >= DUEL_EFFECT_CHANNEL_COUNT || !bar_text(&D_800EB0F8[text->field_67]))
        return 0;
    which = entry->pad_19[0] - 1;
    if (which < 0 || which >= UI_PARTS || !(part = &config->part[which])->set) return 0;
    if (part->hidden) return 1;
    sprite->x = (short)(sprite->x + part->x + (which == UI_PART_NAME ? part->spacing * entry->pad_19[1] : 0));
    sprite->y = (short)(sprite->y + part->y);
    sprite->r = tinted(sprite->r, part->tint, 16);
    sprite->g = tinted(sprite->g, part->tint, 8);
    sprite->b = tinted(sprite->b, part->tint, 0);
    return 0;
}
