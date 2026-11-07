#include "../types.h"
#include <string.h>
#include "../psyq/libgte.h"
#include "../psyq/libgpu.h"
#include "text_encode_decimal_digits.h"
#include "duel_card.h"
#include "display_object_projection.h"
#include "display_object_layout.h"
#include "display_object.h"
#include "card_constants.h"
#include "duel_card_layout.h"
#include "sprite_primitive.h"
#include "display_object_packet_submit.h"
#include "func_80016784.h"
#include "duel_draw_card_output_position.h"
#ifdef MEMORIES_PC
#include "../psyq/libgs.h"
#include "pc/cards/cards.h"
#include "pc/cards/card_layout.h"
#include "pc/cards/card_layout_art.h"
#endif

#define POLY_FT4_BYTES(packet) ((u8 *)(packet))
#define CARD_FRAME_SCRATCH_BYTES(scratch) ((u8 *)(scratch))

#ifdef MEMORIES_PC
/* A port-built quad for a hand card, sorted the way retail submits that
 * card's own pieces: `fl` is func_80016784's DisplayObject_SubmitPacket
 * mode. A clip-tested card (0xF0000: being played, flipped or otherwise
 * turned) was projected by func_80041F90 into the GTE and CardFrameScratch's
 * origin, so its pieces go through that projection too; any other card's
 * corners are already the flat screen rectangle. */
static void CardLayout_SortHandQuad(POLY_FT4 *quad, s32 ot, s32 fl, u8 *origin)
{
    if ((fl & 0xF0000) == 0xF0000) {
        DisplayObject_SubmitProjectedQuad((u8 *)quad, ot, fl, origin);
    } else {
        GsSortPoly(quad, (GsOT *G32)ot, (u16)fl);
    }
}

/* A hand card's retail art, labels and digits all have fixed texture cells.
 * A custom hand layout changes their on-screen rectangles, not the source
 * cells: changing SpritePrim's extent crops or spills into its neighbours.
 * Draw a quad with the original UV footprint instead. `attribute` is the
 * object's own word (func_80016784's `t`), not `src`'s scratchpad copy: a
 * clip-tested cell before this one may have run DivideFT4, whose work area
 * at 0x1F800000 covers `z`'s attribute. */
static void CardLayout_DrawHandCell(const SpritePrim *src, u32 attribute, s32 x, s32 y, s32 w, s32 h,
                                    u8 u, u8 v, u8 cell_w, u8 cell_h, s32 ot, s32 fl, u8 *origin)
{
    POLY_FT4 quad;

    if (w <= 0 || h <= 0) return;
    memset(&quad, 0, sizeof(quad));
    setPolyFT4(&quad);
    if (attribute & 0x40000000) setSemiTrans(&quad, 1);
    if ((attribute & 0x40) || (src->rgb & 0xFFFFFF) == 0x808080) setShadeTex(&quad, 1);
    quad.r0 = (u8)src->rgb;
    quad.g0 = (u8)(src->rgb >> 8);
    quad.b0 = (u8)(src->rgb >> 16);
    quad.tpage = (u16)(src->tpage | ((attribute >> 17) & 0x180) | ((attribute >> 23) & 0x60));
    quad.clut = (u16)((src->cxcy.h.cy << 6) | ((src->cxcy.h.cx >> 4) & 0x3F));
    quad.x0 = quad.x2 = (short)x;
    quad.x1 = quad.x3 = (short)(x + w);
    quad.y0 = quad.y1 = (short)y;
    quad.y2 = quad.y3 = (short)(y + h);
    quad.u0 = quad.u2 = u;
    quad.u1 = quad.u3 = (u8)(u + cell_w);
    quad.v0 = quad.v1 = v;
    quad.v2 = quad.v3 = (u8)(v + cell_h);
    CardLayout_SortHandQuad(&quad, ot, fl, origin);
}
#endif

/* Draws one card's frame on the duel field: position, the hand/field
 * flags, the card-number digits (via Duel_CalcCardStats and
 * Text_EncodeDecimalDigits) and the face/back sprite, through
 * DisplayObject_SubmitPacket on the 0x1F8003xx scratchpad records.
 *
 * The two records it fills are SpritePrim, the GsSPRITE sprite_primitive.h
 * already spells for the two display-object renderers, which build theirs at
 * the same 0x1F800320. This one builds a second at 0x1F800000 for the card
 * art and reuses 0x1F800320 for the number digits, the spell-class frame and
 * the card face, resetting the fields it needs between submissions.
 *
 * The third scratch record, at 0x1F800344, is the POLY_FT4 the clip path
 * primes (len 9, code 0x2C) with the object's color, and the record at
 * 0x1F8003E0 is CardFrameScratch above.
 *
 * All three reads of the color word at 0x0C sit between scratchpad stores,
 * and the target keeps every one of them where the source puts it. That used
 * to need the read spelled through a cast, because a member read is a struct
 * reference and GCC 2.8.1 floats one across stores that are not. With the
 * scratchpad records typed, those stores are struct references too, the
 * reads stay put, and every offset this function touches is a member. */
void func_80016784(DisplayObject *object, s32 arg1, s32 arg2, s32 arg3) {
    u8 sp18[8];
    u8 sp20[8];
    DuelCardRecord *card;
    s32 fl;
    s32 i;
    SpritePrim *k;
    CardFrameScratch *o;
    s32 n;
    s32 d;
    s32 g1;
    s32 g2;
    s32 t;
    SpritePrim *z;
    POLY_FT4 *y;
#ifdef MEMORIES_PC
    s32 wide;
    s32 hand_tpage, hand_u, hand_v, hand_clut, hand_w, hand_h;
    s32 back_tpage, back_u, back_v, back_clut, back_w, back_h;
    s32 hand_styled;
    s32 hand_digits;
    CardLayoutPlacement hand_art, hand_atk, hand_def, hand_atk_label, hand_def_label;
#endif

    if ((u32)(arg2 + 0x33) < 0x173) {
        if (arg3 >= -0x3B) {
            if (arg3 < 0xF0) {
                o = (CardFrameScratch *)SCRATCHPAD_ADDR(0x1F8003E0);
                k = (SpritePrim *)SCRATCHPAD_ADDR(0x1F800320);
                y = (POLY_FT4 *)SCRATCHPAD_ADDR(0x1F800344);
                z = (SpritePrim *)SCRATCHPAD_ADDR(0x1F800000);
                card = &D_801A7AD8[object->field_6A];
#ifdef MEMORIES_PC
                hand_styled = CardLayout_HandStyled(card->card_id);
                if (hand_styled) {
                    hand_art = CardLayout_HandGet(card->card_id, CARD_LAYOUT_HAND_ART);
                    hand_atk = CardLayout_HandGet(card->card_id, CARD_LAYOUT_HAND_ATK);
                    hand_def = CardLayout_HandGet(card->card_id, CARD_LAYOUT_HAND_DEF);
                    hand_atk_label = CardLayout_HandGet(card->card_id, CARD_LAYOUT_HAND_ATK_LABEL);
                    hand_def_label = CardLayout_HandGet(card->card_id, CARD_LAYOUT_HAND_DEF_LABEL);
                }
#endif
                o->pos.h.x = arg2;
                o->pos.h.y = arg3;
                fl = object->field_14 | 0x10000;
                t = object->attribute;
                k->attribute = t;
                z->attribute = t;
                object->field_69 = 0;
                if (object->flags & DISPLAY_OBJECT_FLAG_CLIP_TEST) {
                    fl = object->field_14 | 0xF0000;
                    if (func_80041F90(object,
                                      (s16)o->pos.h.x + 0x1A,
                                      (s16)o->pos.h.y + 0x1E,
                                      (struct ProjectionOut *)SCRATCHPAD_ADDR(0x1F8003E0)) < 0) {
                        return;
                    }
                    g1 = object->field_0C;
                    setlen(y, 9);
                    *(s32 *)&y->r0 = g1;
                    y->code = 0x2C;
                    if ((object->field_20.word & 0xFFFFFF) == 0) {
                        k->attribute = k->attribute | 0x80;
                        z->attribute = k->attribute;
                    }
                }
                k->tpage = 0x1E;
                g2 = object->field_0C;
                k->cxcy.word = 0xF10100;
                k->rgb = g2;
                n = object->field_67;
                if (n != 0) {
                    if (n < 0x29) {
                        k->extent.word = 0x10000C;
                        k->uv.b.hi = 0x70;
                        k->xy.h.x = o->pos.h.x + 0xF;
                        k->xy.h.y = o->pos.h.y + 0xF;
                        k->uv.b.lo = (n / 10) * 0xC;
                        DisplayObject_SubmitPacket(
                            k, POLY_FT4_BYTES(y), arg1, fl,
                            CARD_FRAME_SCRATCH_BYTES(o));
                        k->uv.b.lo = (n % 10) * 0xC;
                        k->xy.h.x = k->xy.h.x + 0xC;
                        DisplayObject_SubmitPacket(
                            k, POLY_FT4_BYTES(y), arg1, fl,
                            CARD_FRAME_SCRATCH_BYTES(o));
                    }
                } else if (object->field_69 == 0) {
                    k->extent.word = 0x100020;
                    k->uv.word = 0x6000;
                    k->xy.h.x = o->pos.h.x + 0xA;
                    k->xy.h.y = o->pos.h.y + 0x28;
                    switch (object->field_68) {
                    case CARD_TYPE_EQUIP:
                        k->uv.b.lo = 0x20;
                    case CARD_TYPE_MAGIC:
                        DisplayObject_SubmitPacket(
                            k, POLY_FT4_BYTES(y), arg1, fl,
                            CARD_FRAME_SCRATCH_BYTES(o));
                        k->cxcy.h.cy = k->cxcy.h.cy + 1;
                        break;
                    case CARD_TYPE_TRAP:
                        k->uv.b.lo = 0x40;
                        DisplayObject_SubmitPacket(
                            k, POLY_FT4_BYTES(y), arg1, fl,
                            CARD_FRAME_SCRATCH_BYTES(o));
                        k->cxcy.h.cy = k->cxcy.h.cy + 2;
                        break;
                    case CARD_TYPE_RITUAL:
                        k->uv.b.lo = 0x60;
                        DisplayObject_SubmitPacket(
                            k, POLY_FT4_BYTES(y), arg1, fl,
                            CARD_FRAME_SCRATCH_BYTES(o));
                        k->cxcy.h.cy = k->cxcy.h.cy + 3;
                        break;
                    default:
#ifdef MEMORIES_PC
                        if (hand_styled) {
                            d = Duel_CalcCardStats(card);
                            /* Keep the retail five-digit capability for a
                             * mod that raises a card stat above 9999. */
                            hand_digits = (d & 0xFFFF) >= 10000 || ((u32)d >> 16) >= 10000 ? 5 : 4;
                            Text_EncodeDecimalDigits((s16)d, hand_digits, sp18);
                            Text_EncodeDecimalDigits(d >> 0x10, hand_digits, sp20);
                            if (hand_atk_label.visible) {
                                CardLayout_DrawHandCell(k, t, o->pos.h.x + hand_atk_label.x,
                                                        o->pos.h.y + hand_atk_label.y,
                                                        hand_atk_label.w, hand_atk_label.h,
                                                        0x78, 0x70, 8, 8, arg1, fl,
                                                        CARD_FRAME_SCRATCH_BYTES(o));
                            }
                            if (hand_def_label.visible) {
                                CardLayout_DrawHandCell(k, t, o->pos.h.x + hand_def_label.x,
                                                        o->pos.h.y + hand_def_label.y,
                                                        hand_def_label.w, hand_def_label.h,
                                                        0x78, 0x78, 8, 8, arg1, fl,
                                                        CARD_FRAME_SCRATCH_BYTES(o));
                            }
                            /* A digit at its own size (8 x 8) is drawn as the retail one is, through
                             * DisplayObject_SubmitPacket, which is where a mod that colors the numbers (the
                             * field's boost, say) takes hold of it; one made bigger or smaller is a quad. */
                            for (i = hand_digits - 1; i >= 0; i--) {
                                if (hand_atk.visible) {
                                    if (hand_atk.w == 8 && hand_atk.h == 8) {
                                        k->extent.word = 0x80008;
                                        k->uv.b.hi = 0x58;
                                        k->uv.b.lo = sp18[i] * 8;
                                        k->xy.h.x = o->pos.h.x + hand_atk.x + (hand_digits - 1 - i) * hand_atk.w;
                                        k->xy.h.y = o->pos.h.y + hand_atk.y;
                                        DisplayObject_SubmitPacket(k, POLY_FT4_BYTES(y), arg1, fl,
                                                                   CARD_FRAME_SCRATCH_BYTES(o));
                                    } else
                                        CardLayout_DrawHandCell(k, t,
                                                                o->pos.h.x + hand_atk.x + (hand_digits - 1 - i) * hand_atk.w,
                                                                o->pos.h.y + hand_atk.y,
                                                                hand_atk.w, hand_atk.h,
                                                                sp18[i] * 8, 0x58, 8, 8, arg1, fl,
                                                                CARD_FRAME_SCRATCH_BYTES(o));
                                }
                            }
                            for (i = hand_digits - 1; i >= 0; i--) {
                                if (hand_def.visible) {
                                    if (hand_def.w == 8 && hand_def.h == 8) {
                                        k->extent.word = 0x80008;
                                        k->uv.b.hi = 0x58;
                                        k->uv.b.lo = sp20[i] * 8;
                                        k->xy.h.x = o->pos.h.x + hand_def.x + (hand_digits - 1 - i) * hand_def.w;
                                        k->xy.h.y = o->pos.h.y + hand_def.y;
                                        DisplayObject_SubmitPacket(k, POLY_FT4_BYTES(y), arg1, fl,
                                                                   CARD_FRAME_SCRATCH_BYTES(o));
                                    } else
                                        CardLayout_DrawHandCell(k, t,
                                                                o->pos.h.x + hand_def.x + (hand_digits - 1 - i) * hand_def.w,
                                                                o->pos.h.y + hand_def.y,
                                                                hand_def.w, hand_def.h,
                                                                sp20[i] * 8, 0x58, 8, 8, arg1, fl,
                                                                CARD_FRAME_SCRATCH_BYTES(o));
                                }
                            }
                            break;
                        }
                        /* ATK or DEF past 9999 (a mod's "limits",
                           pc/cards/tables.h): five digits a row, seven
                           pixels apart from two further left, and the
                           sword and shield two to the left, all inside the
                           card. Under 10000 nothing moves. */
                        d = Duel_CalcCardStats(card);
                        wide = (d & 0xFFFF) >= 10000 || ((u32)d >> 16) >= 10000;
#endif
                        k->extent.word = 0x100008;
                        k->uv.word = 0x7078;
                        k->xy.h.x = o->pos.h.x + 5;
#ifdef MEMORIES_PC
                        if (wide) {
                            k->xy.h.x = o->pos.h.x + 3;
                        }
#endif
                        DisplayObject_SubmitPacket(
                            k, POLY_FT4_BYTES(y), arg1, fl,
                            CARD_FRAME_SCRATCH_BYTES(o));
                        k->uv.b.hi = 0x58;
                        k->extent.wh.h = 8;
                        k->xy.h.x = o->pos.h.x + 0xE;
#ifdef MEMORIES_PC
                        if (wide) {
                            k->xy.h.x = o->pos.h.x + 0xB;
                            Text_EncodeDecimalDigits(d & 0xFFFF, 5, sp18);
                            Text_EncodeDecimalDigits((u32)d >> 16, 5, sp20);
                            i = 4;
                            do {
                                k->uv.b.lo = sp18[i] * 8;
                                DisplayObject_SubmitPacket(
                                    k, POLY_FT4_BYTES(y), arg1, fl,
                                    CARD_FRAME_SCRATCH_BYTES(o));
                                k->xy.h.y = k->xy.h.y + 8;
                                k->uv.b.lo = sp20[i] * 8;
                                DisplayObject_SubmitPacket(
                                    k, POLY_FT4_BYTES(y), arg1, fl,
                                    CARD_FRAME_SCRATCH_BYTES(o));
                                i--;
                                k->xy.h.x = k->xy.h.x + 7;
                                k->xy.h.y = k->xy.h.y - 8;
                            } while (i >= 0);
                            break;
                        }
#endif
                        d = Duel_CalcCardStats(card);
                        Text_EncodeDecimalDigits((s16)d, 4, sp18);
                        Text_EncodeDecimalDigits(d >> 0x10, 4, sp20);
                        i = 3;
                        do {
                            k->uv.b.lo = sp18[i] * 8;
                            DisplayObject_SubmitPacket(
                                k, POLY_FT4_BYTES(y), arg1, fl,
                                CARD_FRAME_SCRATCH_BYTES(o));
                            k->xy.h.y = k->xy.h.y + 8;
                            k->uv.b.lo = sp20[i] * 8;
                            DisplayObject_SubmitPacket(
                                k, POLY_FT4_BYTES(y), arg1, fl,
                                CARD_FRAME_SCRATCH_BYTES(o));
                            i--;
                            k->xy.h.x = k->xy.h.x + 8;
                            k->xy.h.y = k->xy.h.y - 8;
                        } while (i >= 0);
                        break;
                    }
#ifdef MEMORIES_PC
                    /* A mod's frame color (Cards_FrameColor): the face's
                     * palette row, after the type's kind word or stats. */
                    if (Cards_FrameColor(card->card_id) >= 0) {
                        k->cxcy.h.cy = 0xF1 + Cards_FrameColor(card->card_id);
                    }
#endif
                    z->tpage = 0xE;
                    z->rgb = object->field_0C;
#ifdef MEMORIES_PC
                    z->xy.h.x = o->pos.h.x + (hand_styled ? hand_art.x : 6);
                    z->xy.h.y = o->pos.h.y + (hand_styled ? hand_art.y : 6);
#else
                    z->xy.h.x = o->pos.h.x + 6;
                    z->xy.h.y = o->pos.h.y + 6;
#endif
                    i = (s8)card->table_index;
                    z->cxcy.h.cx = 0x380;
                    z->cxcy.h.cy = i + 0xE0;
                    z->uv.b.lo = (i % 5) * 0x28;
#ifdef MEMORIES_PC
                    /* Keep the retail 40x32 source cell here.  A styled
                     * rectangle is scaled by CardLayout_DrawHandArt below;
                     * putting its size in extent would expand the source UV
                     * read and bleed into adjacent thumbnail cells. */
                    z->extent.word = 0x200028;
#else
                    z->extent.word = 0x200028;
#endif
                    z->uv.b.hi = (i / 5) << 5;
#ifdef MEMORIES_PC
                    if (hand_styled && hand_art.visible) {
                        CardLayout_DrawHandCell(z, t, o->pos.h.x + hand_art.x, o->pos.h.y + hand_art.y,
                                                hand_art.w, hand_art.h, z->uv.b.lo, z->uv.b.hi,
                                                40, 32, arg1, fl,
                                                CARD_FRAME_SCRATCH_BYTES(o));
                    } else if (!hand_styled)
#endif
                        DisplayObject_SubmitPacket(z, POLY_FT4_BYTES(y), arg1, fl,
                                                   CARD_FRAME_SCRATCH_BYTES(o));
                }
                k->extent.word = 0x3C0034;
                k->uv.word = 0x8000;
                k->xy.word = o->pos.word;
                if (object->field_67 != 0) {
                    k->uv.word = 0xC000;
                }
                if (object->field_69 != 0) {
                    k->uv.word = 0x8038;
                    k->cxcy.word = 0xF10100;
                }
#ifdef MEMORIES_PC
                /* A frame style's hand_image replaces only this flat 52x60
                 * duel sprite, and only while the card is face up: object->
                 * field_69 just above picked retail's own generic card-back
                 * uv/clut for a face-down card (including mid-flip, which
                 * draws through this same function), and a style's art has
                 * no "back" of its own to show there. Its card art, stats
                 * and type marker above remain the game's own overlays. */
                if (object->field_69 == 0 && object->field_67 == 0 &&   /* not the 0xC000 variant */
                    CardLayoutArt_HandFrameCell(card->card_id, &hand_tpage, &hand_u, &hand_v,
                                                 &hand_clut, &hand_w, &hand_h)) {
                    /* `fl` here is field_14|0x10000 for an ordinary (not
                     * clip-tested) card -- DisplayObject_SubmitPacket's case
                     * 1, GsSortFastSprite, whose own packet keeps only 5 bits
                     * of tpage (page location alone; depth and bank come
                     * from whatever DrawMode the GPU already has set, not
                     * from the sprite). Fine for retail's own flat sprite,
                     * always real VRAM; not for a style's own hand_image, in
                     * its own bank at 8bpp, whose bank and depth bits that
                     * mask silently drops -- the same limit func_80029EC4.c's
                     * Library icon hits. Submitted as its own textured quad
                     * instead, whose tpage field is a full word. A
                     * clip-tested card (fl's 0xF0000: being played, flipped
                     * or otherwise turned) is where retail projects `k`
                     * below; CardLayout_SortHandQuad projects this quad
                     * the same way then, so the frame turns and scales with
                     * the card. */
                    POLY_FT4 quad;
                    setPolyFT4(&quad);
                    /* pad1/pad2 are the upper half of a bank-sampling
                     * polygon's own fade (soft_gpu.h's SoftGpu_FadeOf) -- a
                     * stack local leaves them whatever the stack held, which
                     * CardLayout_DrawCell (func_80028B08.c) already has to
                     * zero for the same reason. Left alone, a stray 0x8000
                     * bit fades the hand frame into whatever is behind it at
                     * a random amount -- the "semi transparent" look. */
                    quad.pad1 = quad.pad2 = 0;
                    quad.r0 = (u8)g2;
                    quad.g0 = (u8)(g2 >> 8);
                    quad.b0 = (u8)(g2 >> 16);
                    quad.tpage = (u16)hand_tpage;
                    quad.clut = (u16)hand_clut;
                    quad.x0 = quad.x2 = o->pos.h.x;
                    quad.x1 = quad.x3 = (short)(o->pos.h.x + hand_w);
                    quad.y0 = quad.y1 = o->pos.h.y;
                    quad.y2 = quad.y3 = (short)(o->pos.h.y + hand_h);
                    quad.u0 = quad.u2 = (u8)hand_u;
                    quad.u1 = quad.u3 = (u8)(hand_u + hand_w);
                    quad.v0 = quad.v1 = (u8)hand_v;
                    quad.v2 = quad.v3 = (u8)(hand_v + hand_h);
                    CardLayout_SortHandQuad(&quad, arg1, fl, CARD_FRAME_SCRATCH_BYTES(o));
                    return;
                }
                /* The mirror case: the card is face down (or mid-flip,
                 * which also draws through this function), and the style
                 * has its own "field_back_image" for what retail would
                 * otherwise draw from k->uv/cxcy just above. Same bank/tpage
                 * reasoning as the face-up quad above. */
                if (object->field_69 != 0 &&
                    CardLayoutArt_FieldBackCell(card->card_id, &back_tpage, &back_u, &back_v,
                                                 &back_clut, &back_w, &back_h)) {
                    POLY_FT4 quad;
                    setPolyFT4(&quad);
                    quad.pad1 = quad.pad2 = 0;
                    quad.r0 = (u8)g2;
                    quad.g0 = (u8)(g2 >> 8);
                    quad.b0 = (u8)(g2 >> 16);
                    quad.tpage = (u16)back_tpage;
                    quad.clut = (u16)back_clut;
                    quad.x0 = quad.x2 = o->pos.h.x;
                    quad.x1 = quad.x3 = (short)(o->pos.h.x + back_w);
                    quad.y0 = quad.y1 = o->pos.h.y;
                    quad.y2 = quad.y3 = (short)(o->pos.h.y + back_h);
                    quad.u0 = quad.u2 = (u8)back_u;
                    quad.u1 = quad.u3 = (u8)(back_u + back_w);
                    quad.v0 = quad.v1 = (u8)back_v;
                    quad.v2 = quad.v3 = (u8)(back_v + back_h);
                    CardLayout_SortHandQuad(&quad, arg1, fl, CARD_FRAME_SCRATCH_BYTES(o));
                    return;
                }
#endif
                DisplayObject_SubmitPacket(
                    k, POLY_FT4_BYTES(y), arg1, fl,
                    CARD_FRAME_SCRATCH_BYTES(o));
            }
        }
    }
}

void Duel_DrawCardAtOutputPosition(DisplayObjectPosition *object, s32 arg1)
{
    func_80016784((DisplayObject *)object, arg1, object->out_x, object->out_y);
}
