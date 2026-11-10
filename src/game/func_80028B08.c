#include "../types.h"
#include "text_encode_decimal_digits.h"
#include "display_object_projection.h"
#include "duel_effect_resource_record.h"
#include "../psyq/libgs.h"
#include "display_object.h"
#include "card_preview_callbacks.h"
#include "sprite_primitive.h"
#define DISPLAY_OBJECT_PACKET_SUBMIT_CARD_LIST
#include "display_object_packet_submit.h"
#include "card_constants.h"
#include "../ygo_types.h"
#ifdef MEMORIES_PC
#include "../psyq/libgpu.h"
#include "pc/cards/tables.h"
#include "pc/cards/pack_shop.h"
#include "pc/cards/card_layout.h"
#include "pc/cards/card_layout_art.h"
#include "pc/text/hd_text.h"
#include "pc/cards/attributes.h"
#include "pc/debug/log.h"
#endif
/*
 * Duel card-detail panel: builds the scratchpad sprite parameters for the
 * panel frame, the card image, the ATK/DEF digit rows, the repeated level
 * icons and the guardian-star tile, and submits each through
 * DisplayObject_SubmitPacket. Byte-exact under gcc_2_8_1_g8_split with no
 * register pins, aliases or inline assembly.
 *
 * Three constructs exist only to steer the compiler and emit no code:
 *
 *  - `sa`/`sb` with the 0x00090009 extent inside a do/while (0): the loop
 *    note after the x load is a scheduling barrier, which gives retail's
 *    `lhu; lui; ori; addiu` order at the level-icon block.
 *
 *  - the two guarded write-backs (`win->flags = flags;` and the second
 *    `PRM->cxcy.h.cy = white;`). Each stores a value its destination already
 *    holds. cse keeps such a store, so the guard and its condition are real
 *    code for flow, both scheduling passes and both register allocators;
 *    reload's cse then deletes the no-op store, and jump2 deletes the empty
 *    branch together with the whole condition chain. Nothing reaches the
 *    object, but while they exist they shape the conflict graph:
 *      - the first makes the 14 (`k`) and the 0xFEFFFFFF mask (`m`) global
 *        pseudos that conflict with each other and with the two temporaries
 *        in v0/v1, so global-alloc gives `k` t1 and `m` t2 as retail has
 *        (local-alloc alone puts the 14 in v0 and the mask in t1). `k` and
 *        `m` are read before their only assignment on purpose: a second
 *        assignment would stop `li k` being placed next to its store and
 *        lift it above the argument moves. `k` is u16 because an SImode 14
 *        lets reload's cse rewrite `addiu t0,t0,14` as `addu t0,t0,t1`.
 *      - the second keeps the 0xF8 (`white`) live while the clamp counter
 *        `i` holds s0, so `white` takes s4. Its condition repeats the shape
 *        of the clamp test that follows, which keeps local-alloc's order
 *        for the two field loads.
 *    With those registers the second scheduling pass floats `li t1,14` and
 *    `li s4,248` to the top of the second submit block by itself.
 */
#ifdef MEMORIES_PC
/* Stretches the card's own picture to fill a "card_layout" mod's larger art
 * rect (card_layout.h's CARD_LAYOUT_ART) instead of retail's small
 * 0x66x0x60 quad -- routed through the projected pipeline (case 5) rather
 * than a hand-built bypass, same as CardLayout_DrawFrame below.
 *
 * `src` is PRM at the point retail has it fully set up for the small
 * picture (attribute/tpage/cxcy/uv/extent all already correct) -- this
 * reads its texel footprint and clut/tpage encoding from there, exactly
 * like the default dispatch case builds a POLY_FT4, but keeps that UV span
 * fixed to the original 0x66x0x60 box while x/y/w/h set the quad's own
 * (bigger) on-screen size independently. Confirmed by research: the shared
 * default dispatch ties vertex size and UV span to the same extent value,
 * so enlarging PRM->extent directly would also enlarge the texel read and
 * sample past the art's real texture block.
 *
 * clut/tpage encode exactly like the default case's own POLY_FT4 build
 * (func_80028B08.c already does this for the digits' packet) -- POLY_FT4
 * and POLY_GT4 share the same byte layout for those two fields, standard
 * PSX GPU packet format, so that formula is reused rather than re-derived.
 * Routed through case 5 (the POLY_GT4 sibling of the plaque's case 4)
 * rather than the default case, since the default case is exactly the
 * extent-coupled path this function exists to avoid. */
static void CardLayout_DrawArt(SpritePrim *src, s32 x, s32 y, s32 w, s32 h,
                                s32 ot, s32 mode, Func80028B08Extra *EXT)
{
    POLY_GT4 art;
    u32 attr = src->attribute;
    s32 projected = ((u32)mode >> 16) == 0xF;
    s32 packet_attr = (projected ? 0x04000000 : 0) | (attr & 0x70000000);
    s32 art_mode = (mode & 0xFFFF) | 0x50000;

    setPolyGT4(&art);
    /* The word after vertex 2's UVs is a bank-sampling polygon's fade
     * (soft_gpu.h's SoftGpu_FadeOf): this packet is a stack local, so
     * without this it is whatever the stack held. */
    art.pad2 = 0;
    art.pad3 = 0;
    setRGB0(&art, (u8)src->rgb, (u8)(src->rgb >> 8), (u8)(src->rgb >> 16));
    setRGB1(&art, (u8)src->rgb, (u8)(src->rgb >> 8), (u8)(src->rgb >> 16));
    setRGB2(&art, (u8)src->rgb, (u8)(src->rgb >> 8), (u8)(src->rgb >> 16));
    setRGB3(&art, (u8)src->rgb, (u8)(src->rgb >> 8), (u8)(src->rgb >> 16));

    art.clut = (src->cxcy.h.cy << 6) | ((src->cxcy.h.cx >> 4) & 0x3F);
    art.tpage = src->tpage | (((attr >> 17) & 0x180) | ((attr >> 23) & 0x60));

    if (attr & 0x800000) {
        art.u1 = art.u3 = src->uv.b.lo;
        art.u0 = art.u2 = src->uv.b.lo + src->extent.wh.w.word - 1;
        art.v0 = art.v1 = src->uv.b.hi;
        art.v2 = art.v3 = src->uv.b.hi + src->extent.wh.h - 1;
    } else {
        art.u0 = art.u2 = src->uv.b.lo;
        art.v0 = art.v1 = src->uv.b.hi;
        if (attr & 0x80) {
            art.u1 = art.u3 = src->uv.b.lo + src->extent.wh.w.word;
            art.v2 = art.v3 = src->uv.b.hi + src->extent.wh.h;
        } else {
            art.u1 = art.u3 = src->uv.b.lo + src->extent.wh.w.word - 1;
            art.v2 = art.v3 = src->uv.b.hi + src->extent.wh.h - 1;
        }
    }

    setXY4(&art, (short)x, (short)y, (short)(x + w), (short)y,
           (short)x, (short)(y + h), (short)(x + w), (short)(y + h));

    DisplayObject_SubmitPacket((SpritePrim *)packet_attr, (Func80028B08Ctx *)&art,
                               ot, art_mode, EXT);
}

/* A "card_layout" mod's own frame (card_layout.h's CardLayout_Get
 * (CARD_LAYOUT_FRAME) w/h): a mod-owned textured quad (card_layout_art.h)
 * through the same case-5 projected dispatch CardLayout_DrawArt above uses
 * (so it rotates with the card too), or simply not drawn if no mod gives
 * one or the asset isn't available (bank allocation or decode failed) --
 * purely decorative, nothing depends on it being there.
 *
 * A *backdrop*, not a mask: submitted last, like every other element
 * here. This port's OT prepends to its bucket's head, so the most recently
 * submitted primitive at a shared depth ends up walked -- drawn -- first,
 * underneath everything submitted before it, which is why this is
 * submitted after art/digits/stars/attribute: it just needs to sit behind
 * them. CardArt_IndexedImage's own entry-0-transparent handling means a
 * genuine cutout in the mod's own art (for the card picture, say) works
 * exactly the same as painted matching background would -- whatever is
 * drawn after this submission paints over it either way, hole or not. */
/* The quad both CardLayout_DrawFrame and a full-bleed spell card's type-icon
 * badge need: cell_w x cell_h texels from (tpage, u, v) through clut,
 * stretched to w x h at x,y -- no src object to inherit blend bits from
 * (neither has a real card sprite behind it), so attribute is built fresh
 * here rather than through CardLayout_DrawArt's src->attribute, which ORs
 * its own extra tpage/blend bits in from whatever that src was last
 * configured for (wrong, and silently so, for an unrelated tpage like the
 * type-icon sheet's 0xB -- confirmed by research after the badge drew
 * nothing). */
static void CardLayout_DrawCell(s32 x, s32 y, s32 w, s32 h, s32 tpage, s32 u, s32 v, s32 clut,
                                s32 cell_w, s32 cell_h, s32 ot, s32 mode, u32 rgb, Func80028B08Extra *EXT)
{
    POLY_GT4 art;
    s32 projected = ((u32)mode >> 16) == 0xF;
    s32 attribute = projected ? 0x04000000 : 0;
    s32 art_mode = (mode & 0xFFFF) | 0x50000;

    setPolyGT4(&art);
    /* The word after vertex 2's UVs is a bank-sampling polygon's fade
     * (soft_gpu.h's SoftGpu_FadeOf): this packet is a stack local, so
     * without this it is whatever the stack held. */
    art.pad2 = 0;
    art.pad3 = 0;
    /* The card's own color (the object's color word, which is what the art
     * and the stars are drawn with): the whole card fades together at the end
     * of an attack, so this quad has to take it, not a fixed neutral grey. */
    setRGB0(&art, (u8)rgb, (u8)(rgb >> 8), (u8)(rgb >> 16));
    setRGB1(&art, (u8)rgb, (u8)(rgb >> 8), (u8)(rgb >> 16));
    setRGB2(&art, (u8)rgb, (u8)(rgb >> 8), (u8)(rgb >> 16));
    setRGB3(&art, (u8)rgb, (u8)(rgb >> 8), (u8)(rgb >> 16));
    art.clut = (u16)clut;
    art.tpage = (u16)tpage;
    art.u0 = art.u2 = (u8)u;
    art.u1 = art.u3 = (u8)(u + cell_w - 1);
    art.v0 = art.v1 = (u8)v;
    art.v2 = art.v3 = (u8)(v + cell_h - 1);
    setXY4(&art, (short)x, (short)y, (short)(x + w), (short)y,
           (short)x, (short)(y + h), (short)(x + w), (short)(y + h));
    DisplayObject_SubmitPacket((SpritePrim *)attribute, (Func80028B08Ctx *)&art,
                               ot, art_mode, EXT);
}

static int CardLayout_DrawFrame(s32 x, s32 y, s32 w, s32 h, s32 ot, s32 mode, u32 rgb, Func80028B08Extra *EXT)
{
    int col, row, tpage, clut, tile_w, tile_h;

    /* The frame's tiles, abutting: each edge is a whole-pixel share of w/h,
     * so neighbours meet exactly with no gap or overlap. */
    for (row = 0; row < CARD_LAYOUT_FRAME_ROWS; row++) {
        for (col = 0; col < CARD_LAYOUT_FRAME_COLS; col++) {
            s32 x0 = x + w * col / CARD_LAYOUT_FRAME_COLS, x1 = x + w * (col + 1) / CARD_LAYOUT_FRAME_COLS;
            s32 y0 = y + h * row / CARD_LAYOUT_FRAME_ROWS, y1 = y + h * (row + 1) / CARD_LAYOUT_FRAME_ROWS;

            if (!CardLayoutArt_FrameTile(col, row, &tpage, &clut, &tile_w, &tile_h)) return 0;
            CardLayout_DrawCell(x0, y0, x1 - x0, y1 - y0, tpage, 0, 0, clut, tile_w, tile_h, ot, mode, rgb, EXT);
        }
    }
    return 1;
}

/* A mod's own digits (card_layout.h's CardLayout_Digits), on a full-bleed
 * frame or on retail's: the visible digits of `buf` (least significant
 * first, a value past 9 is a blank), at the mod's size, centred on (cx, cy)
 * in the card's own units: one to four digits always centred on the box.
 * 0, nothing drawn, when the strip is not there. */
/* buf1/buf2 (func_80028B08's own locals) are each u8[5]: Text_EncodeDecimalDigits
 * never gives this more than five digits, so five resolved cells is always enough. */
#define CARD_LAYOUT_DRAW_DIGITS_MAX 5
static int CardLayout_DrawDigits(s32 base_x, s32 base_y, s32 cx, s32 cy, const u8 *buf, s32 count, s32 dim,
                                 s32 ot, s32 mode, u32 rgb, Func80028B08Extra *EXT)
{
    char path[1024];
    s32 dw, dh, step, tpage, u, v, clut, cw, ch, i, shown = 0, x;
    s32 cell_tpage[CARD_LAYOUT_DRAW_DIGITS_MAX], cell_u[CARD_LAYOUT_DRAW_DIGITS_MAX];
    s32 cell_v[CARD_LAYOUT_DRAW_DIGITS_MAX], cell_clut[CARD_LAYOUT_DRAW_DIGITS_MAX];
    s32 cell_w[CARD_LAYOUT_DRAW_DIGITS_MAX], cell_h[CARD_LAYOUT_DRAW_DIGITS_MAX];

    if (!CardLayout_Digits(path, sizeof(path), &dw, &dh, &step)) return 0;
    if (!CardLayoutArt_DigitCell(0, dim, &tpage, &u, &v, &clut, &cw, &ch)) return 0;
    for (i = 0; i < count; i++) {
        if (buf[i] < 10) shown = i + 1;
    }
    /* Text_EncodeDecimalDigits never blanks the ones place (text_encode_decimal_digits.c),
     * so an actual stat value always has shown >= 1; this is a guard against a future
     * caller that fills `buf` some other way, not a path reachable from ATK/DFD today. */
    if (shown <= 0 || shown > CARD_LAYOUT_DRAW_DIGITS_MAX) return 0;
    if (shown > 4) {   /* a stat past 9999 (a mod's cap): squeezed to the width of four */
        dw = dw * 4 / shown;
        step = step * 4 / shown;
    }
    /* Resolve every digit's cell before drawing any of them, into a local
     * array rather than drawing straight off each resolution: the strip
     * itself is already known good (the probe call above), but a digit
     * failing here too -- not reachable today, every failure path inside
     * CardLayoutArt_DigitCell is state this function already validated for
     * the whole strip, but cheap to guard against a future one that isn't --
     * must not leave a mix of already-drawn custom glyphs and a retail
     * fallback drawn on top of them by the caller. Resolving into an array
     * also means a digit is read back exactly as resolved, not re-resolved
     * (and so implicitly assumed stable) a second time to draw it. */
    for (i = shown - 1; i >= 0; i--) {
        if (!CardLayoutArt_DigitCell(buf[i], dim, &cell_tpage[i], &cell_u[i], &cell_v[i], &cell_clut[i],
                                     &cell_w[i], &cell_h[i])) return 0;
    }
    x = base_x + cx - ((shown - 1) * step + dw) / 2;
    for (i = shown - 1; i >= 0; i--) {
        CardLayout_DrawCell(x, base_y + cy - dh / 2, dw, dh, cell_tpage[i], cell_u[i], cell_v[i], cell_clut[i],
                            cell_w[i], cell_h[i], ot, mode, rgb, EXT);
        x += step;
    }
    return 1;
}

/* A frame style's static ATK/DFD caption. It is separate from the digit
 * strip so a style can use any art and move either caption independently.
 * 1 when the style's image was drawn; 0 when it has none (or it did not
 * load), so the caller draws retail's own caption, as for a type label. */
static int CardLayout_DrawStatLabel(s32 base_x, s32 base_y, s32 dfd, s32 ot, s32 mode, u32 rgb, Func80028B08Extra *EXT)
{
    char path[1024];
    s32 x, y, w, h, tpage, u, v, clut, cell_w, cell_h;

    if (!CardLayout_StatLabel(dfd, path, sizeof(path), &x, &y, &w, &h) ||
        !CardLayoutArt_StatLabelCell(dfd, &tpage, &u, &v, &clut, &cell_w, &cell_h)) return 0;
    CardLayout_DrawCell(base_x + x, base_y + y, w, h, tpage, u, v, clut, cell_w, cell_h, ot, mode, rgb, EXT);
    return 1;
}

/* The "[Trap Card]"-style caption's own replacement image (card_layout.h's
 * CardLayout_TypeLabel), in place of retail's own built-in one for `kind`
 * (CARD_TYPE_MAGIC..CARD_TYPE_EQUIP). 1 when a replacement was drawn.
 * `*visible` is filled in either way: 0 with it still true tells the caller
 * to draw retail's own built-in artwork instead (no replacement image, but
 * not hidden); 0 with it false means draw nothing there at all. */
static int CardLayout_DrawTypeLabel(s32 kind, s32 base_x, s32 base_y, s32 ot, s32 mode, u32 rgb,
                                    Func80028B08Extra *EXT, s32 *visible)
{
    char path[1024];
    s32 x, y, w, h, tpage, u, v, clut, cell_w, cell_h;

    if (!CardLayout_TypeLabel(kind, path, sizeof(path), &x, &y, &w, &h, visible)) return 0;
    if (!CardLayoutArt_TypeLabelCell(kind, &tpage, &u, &v, &clut, &cell_w, &cell_h)) return 0;
    CardLayout_DrawCell(base_x + x, base_y + y, w, h, tpage, u, v, clut, cell_w, cell_h, ot, mode, rgb, EXT);
    return 1;
}

/* With HD Text disabled the retail title plate's ink is black, so a title
 * style uses this white indexed mask. HD mode keeps the original primitive
 * instead: gl_picture.c turns that atlas's ink white at high resolution. */
static int CardLayout_DrawTitleMask(const SpritePrim *src, s32 x, s32 y, s32 ot, s32 mode, u32 rgb,
                                    Func80028B08Extra *EXT)
{
    s32 tpage, u, v, clut, w, h;
    if (!CardLayoutArt_TitleCell(src->tpage, src->uv.b.lo, src->uv.b.hi,
                                 src->extent.wh.w.word, src->extent.wh.h,
                                 &tpage, &u, &v, &clut, &w, &h)) return 0;
    CardLayout_DrawCell(x, y, src->extent.wh.w.word, src->extent.wh.h,
                        tpage, u, v, clut, w, h, ot, mode, rgb, EXT);
    return 1;
}

/* `rgb`'s own three channels, each scaled by `r`/`g`/`b` (CardLayoutPart's
 * convention: 0x80 is 1x, so a mod with no "color" of its own leaves `rgb`
 * unchanged) and clamped to a byte -- a filter over whatever the card's own
 * fade tint (win->field_0C) already is, not a replacement for it, the same
 * way the GPU's own vertex-color multiply works. */
static u32 CardLayout_Tint(u32 rgb, s32 r, s32 g, s32 b)
{
    s32 tr = (s32)(u8)rgb * r / 0x80;
    s32 tg = (s32)(u8)(rgb >> 8) * g / 0x80;
    s32 tb = (s32)(u8)(rgb >> 16) * b / 0x80;
    if (tr > 0xFF) tr = 0xFF;
    if (tg > 0xFF) tg = 0xFF;
    if (tb > 0xFF) tb = 0xFF;
    return (u32)tr | ((u32)tg << 8) | ((u32)tb << 16);
}
#endif

void func_80028B08(DisplayObject *obj, s32 arg1) {
    u8 buf1[5];
    u8 buf2[5];
    Func80028B08Extra *EXT;
    SpritePrim *PRM;
    Func80028B08Ctx *CTX;
    DisplayObject *win;
    DuelEffectResourceRecord *rec;
    s32 arg;
    s32 i;
    u16 flags;
    u32 f4;
    u32 tile;
    u32 lo;
    s32 wrap;
    u16 k;
    s32 white;
    s32 sa;
    s32 sb;
    u32 m;
#ifdef MEMORIES_PC
    /* A "card_layout" mod (card_layout.h): win's own position is close to
     * screen origin (~2,4) -- retail's own offsets below (+0x13, +0x9D,
     * ...) are the entire positioning mechanism, not padding inside an
     * already-placed frame, confirmed by research before touching this a
     * second time. So every placement below stays in retail's own
     * coordinate neighbourhood, not a reset-to-zero origin: card_layout.c
     * is the single place that knows where each element sits when a mod
     * applies. Enlarging the art itself (CardLayout_DrawArt above) needed
     * its own POLY_GT4 build rather than a bigger PRM->extent: extent is
     * the texel-read size for every submission path reachable here, not an
     * independent draw size, confirmed by research -- a bigger extent
     * would sample past the art's real texture block, not just stretch
     * it. */
    /* The record isn't loaded as `rec` until further down (its own
     * assignment stays where retail has it) -- D_800EA0E8[obj->field_67] is
     * read again here, a plain read with no effect on that assignment,
     * only so every CardLayout_Get call below already answers for the
     * right card's frame (cards.h's CARD_FRAME_*), not the previous one
     * this object happened to draw. */
    CardLayout_SetCard((s32)(s16)D_800EA0E8[obj->field_67].field_30);
    CardLayoutPart frame_layout = CardLayout_Part(CARD_LAYOUT_FRAME);
    CardLayoutPart title_layout = CardLayout_Part(CARD_LAYOUT_TITLE);
    CardLayoutPart atk_layout = CardLayout_Part(CARD_LAYOUT_ATK);
    CardLayoutPart def_layout = CardLayout_Part(CARD_LAYOUT_DEF);
    CardLayoutPart atk_label_layout = CardLayout_Part(CARD_LAYOUT_ATK_LABEL);
    CardLayoutPart def_label_layout = CardLayout_Part(CARD_LAYOUT_DEF_LABEL);
    CardLayoutPart star_layout = CardLayout_Part(CARD_LAYOUT_LEVEL_STARS);
    CardLayoutPart attr_layout = CardLayout_Part(CARD_LAYOUT_ATTRIBUTE);
    CardLayoutPart art_layout = CardLayout_Part(CARD_LAYOUT_ART);
#endif

    wrap = 0xFFFF;
    win = (DisplayObject *)obj->field_54;
#ifdef MEMORIES_PC
    /* The retail plate hidden only by a frame drawn this call (the end of
     * this function): a return before it leaves the plate shown. */
    win->attribute &= ~GsDOFF;
#endif
    if ((obj->attribute & (1<<31) ) != 0) {
        return;
    }
    flags = win->flags;
    if ((obj->field_66 & m) ^ (obj->field_67 & k)) {
        win->flags = flags;
    }
    if ((flags & 0x40) == 0) {
        return;
    }
    EXT = (Func80028B08Extra *)SCRATCHPAD_ADDR(0x1F800398);
    PRM = (SpritePrim *)SCRATCHPAD_ADDR(0x1F800320);
    CTX = (Func80028B08Ctx *)SCRATCHPAD_ADDR(0x1F800344);
    arg = (((s16)win->field_14 - 1) & 0xFFFF) | 0x10000;
    if (flags & 0x4) {
        obj->field_20.word = win->field_20.word;
        f4 = obj->attribute & ~(1<<27) ;
        obj->field_44.word = win->field_44.word;
        obj->attribute = f4;
        f4 = f4 | (win->attribute & 0x08000000);
        obj->attribute = f4;
        if (func_80041F90(
                (struct DisplayObject *)obj, (s16)win->field_30.h.field_30 + (s16)win->field_18,
                (s16)win->field_30.h.field_32 + (s16)win->field_1A, (struct ProjectionOut *)EXT
            ) <= 0) {
            return;
        }
        arg = (((s16)win->field_14 - 1) & 0xFFFF) | 0xF0000;
        *(u32 *)&CTX->field_4 = win->field_0C;
        CTX->field_3 = 9;
        CTX->field_7 = 0x2C;
    }

#ifdef MEMORIES_PC
    {
        /* A card pack's whole picture (pack_shop.h): drawn as the art is,
           turning with the card, in the place of all the rest. */
        PackShopPicture picture;

        if (PackShop_Picture(obj, &picture)) {
            PRM->attribute = obj->attribute;
            PRM->xy.h.x = win->field_30.h.field_30 + picture.x;
            PRM->xy.h.y = win->field_30.h.field_32 + picture.y;
            PRM->extent.wh.w.word = picture.width;
            PRM->extent.wh.h = picture.height;
            PRM->rgb = win->field_0C;
            PRM->cxcy.h.cx = picture.clut_x;
            PRM->cxcy.h.cy = picture.clut_y;
            PRM->uv.b.lo = picture.u;
            PRM->uv.b.hi = picture.v;
            PRM->tpage = picture.tpage;
            DisplayObject_SubmitPacket(PRM, CTX, arg1, arg, EXT);
            return;
        }
    }
#endif
    PRM->attribute = obj->attribute;
    PRM->xy.h.x = win->field_30.h.field_30 + 0x13;
    PRM->xy.h.y = win->field_30.h.field_32 + 0x32;
    PRM->extent.wh.w.word = 0x66;
    PRM->extent.wh.h = 0x60;
    PRM->rgb = win->field_0C;
    PRM->cxcy.word = obj->field_40.word;
    PRM->uv.word = obj->field_5C;
    PRM->tpage = obj->field_66;
#ifdef MEMORIES_PC
    /* A "card_layout" mod's full-bleed stretches the art (CardLayout_DrawArt)
     * in place of retail's own small quad -- PRM is still fully set up with
     * the original attribute/tpage/cxcy/uv/extent at this point, which is
     * exactly what that helper reads its texel footprint from. */
    if (art_layout.visible && art_layout.w != 0) {
        CardLayout_DrawArt(PRM, win->field_30.h.field_30 + art_layout.x,
                           win->field_30.h.field_32 + art_layout.y,
                           art_layout.w, art_layout.h, arg1, arg, EXT);
    } else if (art_layout.visible)
#endif
    DisplayObject_SubmitPacket(PRM, CTX, arg1, arg, EXT);

    CTX->field_7 |= 2;
    PRM->xy.h.x = win->field_30.h.field_30 + 0xC;
    PRM->xy.h.y = win->field_30.h.field_32 + 0xE;
    PRM->uv.b.hi = PRM->uv.b.hi + 0x60;
    m = 0xFEFFFFFF;
    PRM->attribute = (PRM->attribute & m) | 0x60000000;
    PRM->extent.wh.w.word = 0x60;
    k = 0xE;
    PRM->extent.wh.h = k;
    PRM->cxcy.h.cx = 0x1E0;
    white = 0xF8;
    PRM->cxcy.h.cy = white;
#ifdef MEMORIES_PC
    /* A full-bleed style opts into this existing localized title sprite with
     * `title: {"x", "y"}`. No name PNG or per-card manifest entry is needed. */
    PRM->xy.h.x = win->field_30.h.field_30 + title_layout.x;
    PRM->xy.h.y = win->field_30.h.field_32 + title_layout.y;
    PRM->rgb = CardLayout_Tint(win->field_0C, title_layout.r, title_layout.g, title_layout.b);
    if (title_layout.visible) {
        if (!HdText_Enabled() && CardLayout_FullBleed() &&
            CardLayout_DrawTitleMask(PRM, PRM->xy.h.x, PRM->xy.h.y, arg1, arg, PRM->rgb, EXT)) {
            /* The low-resolution white mask was submitted. */
        } else
#endif
        DisplayObject_SubmitPacket(PRM, CTX, arg1, arg, EXT);
#ifdef MEMORIES_PC
    }
#endif
#ifdef MEMORIES_PC
    PRM->rgb = win->field_0C;
#endif

    EXT->field_4 = 0;
    rec = &D_800EA0E8[obj->field_67];
    PRM->tpage = 0x1F;
    PRM->cxcy.h.cx = PRM->cxcy.h.cx + 0x10;
    PRM->xy.h.x = win->field_30.h.field_30 + obj->field_30.h.field_30;
    PRM->xy.h.y = win->field_30.h.field_32 + obj->field_30.h.field_32;
    PRM->extent.word = obj->field_3C.word;
    PRM->uv.word = obj->field_5E;
    if (obj->field_68 < 0x14) {
        if (rec->field_3C & 0x80) {
            PRM->cxcy.h.cy = PRM->cxcy.h.cy + 1;
        }
#ifdef MEMORIES_PC
        /* "ATK"/"DFD" (same shared font page as the digits, positioned via
         * obj->field_30 -- a fixed per-screen offset set at setup, never
         * touched by card_layout.c). Retail's stacked layout has room for
         * this label to the left of each value; a full-bleed mod's
         * relocated, side-by-side boxes may not, so it is left out
         * entirely while one is active -- the boxes read by position
         * alone, no label text, as the reference layout this feature
         * followed did.
         * A style that keeps retail's frame may give its own "atk_label"
         * picture: it is drawn here, in the caption's place in the draw
         * order, and retail's sprite (a separate one on the font page, not
         * part of the plate) is not submitted under it. */
        if (!CardLayout_FullBleed() && atk_label_layout.visible &&
            !CardLayout_DrawStatLabel(win->field_30.h.field_30, win->field_30.h.field_32,
                                      0, arg1, arg, win->field_0C, EXT))
#endif
        DisplayObject_SubmitPacket(PRM, CTX, arg1, arg, EXT);
        PRM->cxcy.h.cy = white;
        PRM->uv.b.hi = PRM->uv.b.hi + *(u8 *)&PRM->extent.wh.h;
        PRM->xy.h.y = PRM->xy.h.y + (PRM->extent.wh.h + wrap);
        if (rec->field_3C & 0x40) {
            PRM->cxcy.h.cy = 0xF9;
        }
#ifdef MEMORIES_PC
        if (!CardLayout_FullBleed() && def_label_layout.visible &&
            !CardLayout_DrawStatLabel(win->field_30.h.field_30, win->field_30.h.field_32,
                                      1, arg1, arg, win->field_0C, EXT))
#endif
        DisplayObject_SubmitPacket(PRM, CTX, arg1, arg, EXT);
        PRM->cxcy.h.cy = white;

#ifdef MEMORIES_PC
        {
            /* The caps are a mod's "limits" (pc/cards/tables.h). Past 9999
               the numbers take five digits, five pixels apart (the digits'
               own width, so they touch), where four six apart go: the ATK
               and DFD labels on the left and the plate's edge on the right
               leave no room for more. */
            s32 attack = rec->field_32 + rec->field_36;
            s32 defense = rec->field_34 + rec->field_38;
            s32 digits, step, atk_left, def_left, atk_top, def_top;

            if (attack > Tables_StatCap(0)) {
                attack = Tables_StatCap(0);
            }
            if (defense > Tables_StatCap(1)) {
                defense = Tables_StatCap(1);
            }
            digits = attack >= 10000 || defense >= 10000 ? 5 : 4;
            step = digits == 5 ? 5 : 6;
            atk_left = digits == 5 ? atk_layout.x - 1 : atk_layout.x;
            def_left = digits == 5 ? def_layout.x - 1 : def_layout.x;
            atk_top = atk_layout.y;
            def_top = def_layout.y;
            if (CardLayout_FullBleed()) {
                /* atk_layout.x/y and def_layout.x/y are each stat box's own
                 * centre now (a full-bleed mod's own frame art bakes the
                 * box itself in, no separate plaque sprite to anchor a
                 * corner to), not the retail-stacked-layout's top-left
                 * corner convention above -- centre the digit row under/in
                 * it directly. Replaces the -1 nudge above rather than
                 * adding to it. Height is the glyph's own fixed 0x0D
                 * regardless of digit count; width depends on it
                 * (step/count). */
                s32 digit_width = (digits - 1) * step + 6;
                atk_left = atk_layout.x - digit_width / 2;
                def_left = def_layout.x - digit_width / 2;
                atk_top = atk_layout.y - 0x0D / 2;
                def_top = def_layout.y - 0x0D / 2;
            }
            Text_EncodeDecimalDigits(attack, digits, buf1);
            Text_EncodeDecimalDigits(defense, digits, buf2);
            /* The retail digit code below sets the texture row (uv.b.hi) the
             * level stars are drawn from, too: set it whichever digits draw. */
            PRM->uv.b.hi = (PRM->uv.b.hi & 0x80) + 0x10;
            /* A mod's own digits, centred on each box. The stat the
             * attack screen dims (rec->field_3C's 0x80 ATK, 0x40 DEF: the
             * retail digits' grey palette row) is drawn from the strip's
             * greyed digits. On a style that keeps retail's frame the box is
             * the one retail's digits fill (atk_left/atk_top are its corner,
             * the glyphs 0x0D high); on a full-bleed frame that centre is
             * atk_layout.x/y itself, as computed above. Retail's digits are
             * drawn only where the strip was not. A card no style claims
             * keeps retail's digits: a layout's shared strip is for its
             * styles' cards (and a game before mod API 25 drew it only on a
             * full-bleed frame, which such a card never has). */
            if (CardLayout_FullBleed() && atk_label_layout.visible) {
                CardLayout_DrawStatLabel(win->field_30.h.field_30, win->field_30.h.field_32,
                                         0, arg1, arg, win->field_0C, EXT);
            }
            if (CardLayout_FullBleed() && def_label_layout.visible) {
                CardLayout_DrawStatLabel(win->field_30.h.field_30, win->field_30.h.field_32,
                                         1, arg1, arg, win->field_0C, EXT);
            }
            {
            s32 atk_custom = 0, def_custom = 0;
            s32 row_width = (digits - 1) * step + 6;
            if (CardLayout_Styled() && atk_layout.visible)
                atk_custom = CardLayout_DrawDigits(win->field_30.h.field_30, win->field_30.h.field_32,
                                                    atk_left + row_width / 2, atk_top + 0x0D / 2, buf1, digits,
                                                    (rec->field_3C & 0x80) != 0, arg1, arg, win->field_0C, EXT);
            if (CardLayout_Styled() && def_layout.visible)
                def_custom = CardLayout_DrawDigits(win->field_30.h.field_30, win->field_30.h.field_32,
                                                    def_left + row_width / 2, def_top + 0x0D / 2, buf2, digits,
                                                    (rec->field_3C & 0x40) != 0, arg1, arg, win->field_0C, EXT);

            /* A fallback style's own "color" (card_layout.h's CardLayoutPart):
             * a filter on the card's own fade tint, not a replacement for it,
             * so the box still dims/fades with the rest of the card. */
            if (!atk_custom && atk_layout.visible) {
            PRM->rgb = CardLayout_Tint(win->field_0C, atk_layout.r, atk_layout.g, atk_layout.b);
            PRM->uv.b.hi = (PRM->uv.b.hi & 0x80) + 0x10;
            PRM->xy.h.x = win->field_30.h.field_30 + atk_left;
            PRM->xy.h.y = win->field_30.h.field_32 + atk_top;
            *(u32 *)&PRM->extent = 0x000D0006;
            if (rec->field_3C & 0x80) {
                PRM->cxcy.h.cy = 0xF9;
            }
            for (i = digits - 1; i >= 0; i--) {
                PRM->uv.b.lo = buf1[i] * 6 + 0x10;
                DisplayObject_SubmitPacket(PRM, CTX, arg1, arg, EXT);
                PRM->xy.h.x = PRM->xy.h.x + step;
            }
            }

            if (!def_custom && def_layout.visible) {
            PRM->rgb = CardLayout_Tint(win->field_0C, def_layout.r, def_layout.g, def_layout.b);
            PRM->xy.h.x = win->field_30.h.field_30 + def_left;
            PRM->xy.h.y = win->field_30.h.field_32 + def_top;
            /* ATK's own fallback block (above) is the only other place this
             * digit glyph footprint gets set, and -- now that the two
             * fallbacks run independently -- it may not have run this call
             * (ATK drawn via the custom strip, or hidden outright). Set it
             * here too rather than leaving this block trusting whatever
             * extent the earlier ATK/DFD label sprite (field_3C.word, line
             * ~446) left behind. */
            *(u32 *)&PRM->extent = 0x000D0006;
            PRM->cxcy.h.cy = 0xF8;
            if (rec->field_3C & 0x40) {
                PRM->cxcy.h.cy = 0xF9;
            }
            for (i = digits - 1; i >= 0; i--) {
                PRM->uv.b.lo = buf2[i] * 6 + 0x10;
                DisplayObject_SubmitPacket(PRM, CTX, arg1, arg, EXT);
                PRM->xy.h.x = PRM->xy.h.x + step;
            }
            }
            PRM->rgb = win->field_0C;
            }
        }
#else
        i = rec->field_32 + rec->field_36;
        if (i > 9999) {
            PRM->cxcy.h.cy = white;
        }
        if (i > 9999 ) {
            i = 9999 ;
        }
        Text_EncodeDecimalDigits(i, 4, buf1);
        i = rec->field_34 + rec->field_38;
        if (i > 9999 ) {
            i = 9999 ;
        }
        Text_EncodeDecimalDigits(i, 4, buf2);

        PRM->uv.b.hi = (PRM->uv.b.hi & 0x80) + 0x10;
        PRM->xy.h.x = win->field_30.h.field_30 + 0x61;
        PRM->xy.h.y = win->field_30.h.field_32 + 0x9D;
        *(u32 *)&PRM->extent = 0x000D0006;
        if (rec->field_3C & 0x80) {
            PRM->cxcy.h.cy = 0xF9;
        }
        i = 3;
        do {
            PRM->uv.b.lo = buf1[i] * 6 + 0x10;
            DisplayObject_SubmitPacket(PRM, CTX, arg1, arg, EXT);
            PRM->xy.h.x = PRM->xy.h.x + 6;
            i--;
        } while (i >= 0);

        PRM->xy.h.x = win->field_30.h.field_30 + 0x61;
        PRM->xy.h.y = win->field_30.h.field_32 + 0xAB;
        PRM->cxcy.h.cy = 0xF8;
        if (rec->field_3C & 0x40) {
            PRM->cxcy.h.cy = 0xF9;
        }
        i = 3;
        do {
            PRM->uv.b.lo = buf2[i] * 6 + 0x10;
            DisplayObject_SubmitPacket(PRM, CTX, arg1, arg, EXT);
            PRM->xy.h.x = PRM->xy.h.x + 6;
            i--;
        } while (i >= 0);
#endif

        sa = win->field_30.h.field_30;
        do { sb = 0x00090009; } while (0);
        PRM->xy.h.x = sa + 0x77;
        PRM->xy.h.y = win->field_30.h.field_32 + 0x20;
#ifdef MEMORIES_PC
        PRM->xy.h.x = sa + star_layout.x;
        PRM->xy.h.y = win->field_30.h.field_32 + star_layout.y;
        /* Shared, pre-frame-style layouts (including the bundled HD anime
         * frame) documented their stars.x as the row's centre.  Style-local
         * stars.x is a literal rightmost-star anchor, which makes the Frames
         * editor's drag position stable across card levels. */
        if (CardLayout_StarsCentered()) {
            PRM->xy.h.x += 9 * (s32)rec->field_3A / 2 - 9;
        }
        PRM->rgb = CardLayout_Tint(win->field_0C, star_layout.r, star_layout.g, star_layout.b);
#endif
        *(u32 *)&PRM->extent = sb;
        PRM->uv.b.lo = 0;
        PRM->cxcy.h.cx = 0x1C0;
        PRM->cxcy.h.cy = 0xF8;
        if (star_layout.visible && rec->field_3A != 0) {
            i = 0;
            do {
                DisplayObject_SubmitPacket(PRM, CTX, arg1, arg, EXT);
                PRM->xy.h.x = PRM->xy.h.x - 9;
                i++;
            } while (i < (s32)rec->field_3A);
        }
    } else {
#ifdef MEMORIES_PC
        /* Retail's own "[Normal Magic Card]"/"[Trap Card]"/"[Ritual Magic
         * Card]"/"[Equip Magic Card]" caption, PRM already set up for it
         * (tpage/xy/extent/uv above) from obj->field_30/field_3C/field_5E,
         * baked in once at setup (duel_effect_resource_setup.c) -- this is
         * the one retail sprite in this function that never got the
         * card_layout treatment every other element here has: no visibility
         * check, no reposition, drawn unconditionally regardless of
         * full_bleed, painting over a full-bleed style's own frame art no
         * matter what it draws there. A style's own "type_label" entry
         * replaces it; "hide" leaves nothing there at all, matching every
         * other movable part's convention. */
        s32 type_label_visible = 1;
        if (!CardLayout_DrawTypeLabel(obj->field_68, win->field_30.h.field_30, win->field_30.h.field_32,
                                      arg1, arg, win->field_0C, EXT, &type_label_visible) &&
            type_label_visible)
#endif
        DisplayObject_SubmitPacket(PRM, CTX, arg1, arg, EXT);
    }

    PRM->xy.h.x = win->field_30.h.field_30 + 0x6E;
    PRM->xy.h.y = win->field_30.h.field_32 + 0xD;
#ifdef MEMORIES_PC
    PRM->xy.h.x = win->field_30.h.field_30 + attr_layout.x;
    PRM->xy.h.y = win->field_30.h.field_32 + attr_layout.y;
    PRM->rgb = CardLayout_Tint(win->field_0C, attr_layout.r, attr_layout.g, attr_layout.b);
#endif
    *(u32 *)&PRM->extent = 0x00100010;
    lo = rec->field_3B << 4;
    PRM->uv.b.lo = lo;
    PRM->uv.b.hi = PRM->uv.b.hi & 0x80;
    tile = PRM->uv.b.lo;
    PRM->cxcy.h.cx = win->field_40.h.field_40 + tile;
    PRM->cxcy.h.cy = 0xFF;
#ifdef MEMORIES_PC
    {
        int tpage, u, v, cx, cy;
        if (Attributes_IconCell(rec->field_3B, &tpage, &u, &v, &cx, &cy)) {
            PRM->tpage = (u16)tpage;
            PRM->uv.b.lo = (u8)u;
            PRM->uv.b.hi = (u8)v;
            PRM->cxcy.h.cx = (s16)cx;
            PRM->cxcy.h.cy = (s16)cy;
        } else if (rec->field_3B >= 8) {
            /* A bank-icon attribute no active mod declares (its mod got
             * disabled, or never declared it): lo/cxcy above just read
             * rec->field_3B as if it were a 0-5 retail index into the
             * six-icon strip and walked off its end. Draw attribute 0
             * instead of whatever texel happens to sit past the strip. */
            PRM->uv.b.lo = 0;
            PRM->uv.b.hi = PRM->uv.b.hi & 0x80;
            PRM->cxcy.h.cx = win->field_40.h.field_40;
            PRM->cxcy.h.cy = 0xFF;
        }
    }
    /* A full-bleed mod stretches whatever this slot already draws
     * (retail's own elemental-attribute texture, rec->field_3B/PRM, set up
     * just above -- unchanged, not swapped for anything else) into its own
     * frame cutout (CARD_LAYOUT_ATTRIBUTE's own w/h), the same way the
     * card's own picture is enlarged above. Magic/trap/ritual answer with
     * "spell"."icon" there instead of "attribute" (card_layout.c): same
     * mechanism, just the mod's other position for it. */
    if (attr_layout.visible && attr_layout.w != 0) {
        CardLayout_DrawArt(PRM, win->field_30.h.field_30 + attr_layout.x,
                           win->field_30.h.field_32 + attr_layout.y,
                           attr_layout.w, attr_layout.h, arg1, arg, EXT);
    } else if (attr_layout.visible)
#endif
    DisplayObject_SubmitPacket(PRM, CTX, arg1, arg, EXT);
#ifdef MEMORIES_PC
    /* Last on purpose (CardLayout_DrawFrame's own comment explains why):
     * submitted after art/digits/stars/attribute above, so it ends up
     * drawn before them -- underneath. */
    if (CardLayout_FullBleed()) {
        LOG(LOG_CARD_LAYOUT, "DrawFrame: x=%d y=%d w=%d h=%d ot=%d mode=0x%x",
            win->field_30.h.field_30, win->field_30.h.field_32, frame_layout.w, frame_layout.h,
            (int)arg1, (unsigned)arg);
        /* The retail sprite-sheet frame is opaque and is submitted after
         * these tiles, so it would cover a successfully loaded custom
         * frame. Keep it during the opening animation (when an image may
         * still fail to decode), then turn only that sprite off once all
         * custom tiles are available -- and back on the moment they stop
         * being available (a new "image" still decoding, a save state
         * loaded mid-animation, a mod swapped out): this runs every frame
         * the viewer is open, so GsDOFF must track this frame's own result,
         * never just latch on, or a `win` object a later card reuses could
         * inherit a stale "hidden" bit with nothing behind it to show. */
        /* Keep the replacement in the same draw bucket as the other card
         * contents.  The viewer deliberately puts that bucket one step in
         * front of the retail plate; moving only the replacement back to the
         * plate's depth changes the ordering shared by Build Deck previews
         * and lets cards cross the right-hand panel.  The frame is submitted
         * last, so within this bucket it remains behind the art and text. */
        if (CardLayout_DrawFrame(win->field_30.h.field_30, win->field_30.h.field_32,
                                 frame_layout.w, frame_layout.h, arg1, arg, win->field_0C, EXT))
            win->attribute |= GsDOFF;
        else
            win->attribute &= ~GsDOFF;
    } else {
        win->attribute &= ~GsDOFF;
    }
#endif
}

