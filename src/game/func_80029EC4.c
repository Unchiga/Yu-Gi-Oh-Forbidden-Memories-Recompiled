#define GRAPHICS_VIEWPORT_IN_DATA
#define D_8009B09C_IN_DATA
#include "../types.h"
#include "graphics_frame.h"
#include "library_runtime.h"
#include "func_80029EB0.h"
#include "card_constants.h"
#include "../psyq/libgs.h"
#include "ordering_tables.h"
#include "func_80029EC4.h"
#include "card_grid.h"
#ifdef MEMORIES_PC
#include "../psyq/libgpu.h"
#include "pc/cards/cards.h"
#include "pc/cards/card_layout_art.h"
#endif

#define GS_SPRITE_COLOR_WORD(sprite) (*(u32 *)&(sprite)->r)

#ifdef MEMORIES_PC
/* GsSortFastSprite's own packet (libgs.c's sort_plain_sprite) keeps only
 * the low 5 bits of a sprite's tpage -- page location alone, no bank, no
 * depth field of its own; the real PS1 fast-sprite command relies on those
 * instead coming from whatever DrawMode the GPU already has set, which is
 * exactly right for retail's own icon (always real VRAM, always the
 * state the duel already set up) but silently drops a style's own
 * CardLayoutArt_LibraryIconCell bank and 8bpp depth bits -- the custom icon
 * then samples whatever retail's current page happens to be, not its own
 * texels. A textured quad's own tpage field has room for all of it, the
 * same device CardLayout_DrawFrame's own tiles use (func_80028B08.c), so a
 * cell the layout drew is submitted as one instead of as a GsSPRITE. */
static void Library_DrawCustomIcon(const GsSPRITE *p, int tpage, GsOT *ot)
{
    POLY_FT4 quad;
    setPolyFT4(&quad);
    /* pad1/pad2 are the upper half of a bank-sampling polygon's own fade
     * (soft_gpu.h's SoftGpu_FadeOf) -- a stack local leaves them whatever
     * the stack held, which CardLayout_DrawCell (func_80028B08.c) already
     * has to zero for the same reason. Left alone, a stray 0x8000 bit
     * fades the icon into whatever is behind it at a random amount. */
    quad.pad1 = quad.pad2 = 0;
    quad.r0 = p->r;
    quad.g0 = p->g;
    quad.b0 = p->b;
    quad.tpage = (u16)tpage;
    quad.clut = getClut(p->cx, p->cy);
    quad.x0 = quad.x2 = p->x;
    quad.x1 = quad.x3 = (short)(p->x + p->w);
    quad.y0 = quad.y1 = p->y;
    quad.y2 = quad.y3 = (short)(p->y + p->h);
    quad.u0 = quad.u2 = p->u;
    quad.u1 = quad.u3 = (u8)(p->u + p->w);
    quad.v0 = quad.v1 = p->v;
    quad.v2 = quad.v3 = (u8)(p->v + p->h);
    GsSortPoly(&quad, ot, 2);
}
#endif

/* Draws the scrolling card-list grid straight into the scratchpad primitive at
   0x1F800320. The first visible row comes from the viewport scroll divided by
   the 178-pixel row pitch; from there four rows of ten lines are walked, each
   line holding ten cells on the left at column 8 and, while the paired index
   is still below CARD_ID_END, ten more on the right at column 0xA8. A cell is drawn
   only when Library_GetCardFlags reports bit 0x80, tinted 0x808080 or 0x404040 on
   bit 0, and the run stops as soon as a line falls off the bottom of the
   screen. The tail then builds the cursor box at 0x1F800000, coloring it from
   the low seven bits of D_8009B09C through a four-way ramp, and draws its four
   edges with GsSortGLine.

   Pure C under the uniform gcc_2_8_1_g8_split profile, the profile of the unit
   that follows it (func_8002A3CC.c). What the -G0 candidate could not reach:

   - Under -G8 the viewport scalars and D_8009B09C are small data, so their
     loads stay one unsplit instruction through sched2 and the assembler
     expands each into an adjacent lui/load pair in one register. The two
     IN_DATA macros above keep them absolute rather than gp-relative. The
     ordering table is read as D_800E9D90[3]: the 16-byte array is not small
     data, so that address stays split and reload stores the pointer to its
     stack slot through the dying %hi register ($v0) instead of $t1.
   - `first` is a single-set local, so sched1 keeps its shift next to the
     multiply that consumes it, after the p->x store. That store sits between
     the spill of `b` and its reuse, which is what makes reload load `b` back
     into $t1 rather than reuse $v1.
   - The attribute word is set ahead of the block that stores it. The loop
     note keeps the constant live across the stores before it, which is what
     gives it $v1 and lets sched2 float its lui above the p->y store.
   - The cursor's y is a chained assignment, y1 then y0, from one value. */
void Library_DrawCardGrid(void)
{
    GsSPRITE *p;
    GsGLINE *q;
    u8 *pj;
    u8 *pk;
    GsOT *ot;
    s32 n;
    s32 idx;
    s32 i;
    s32 c;
    s32 j;
    s32 k;
    s32 r;
    s32 white;
    s32 grey;
    s32 a;
    s32 b;
    s32 y;
    s32 v;
    u8 *list;
    u8 *tb;
    s32 e;
    s32 f;
    s32 first;
    s32 attribute;
#ifdef MEMORIES_PC
    s32 icon_tpage, icon_u, icon_v, icon_clut, icon_w, icon_h, icon_custom;
    s32 card_j, card_k;   /* the cards of cells j and k (cards.h, Cards_LibraryEntry) */
#endif

#ifdef MEMORIES_PC
    /* The panels behind section rows the disc's cards do not reach. */
    Cards_DrawLibraryPanels();
#endif
    p = (GsSPRITE *)SCRATCHPAD_ADDR(0x1F800320);
    n = (gGraphics_sViewportY - 8) / 178;
    ot = D_800E9D90[3];
    if (n < 0) {
        return;
    }
    e = 0xE000C;
    f = 0xF70130;
    white = 0x808080;
    grey = 0x404040;
    a = n * 178 + 8;
    b = n * 25;
    p->x = 8;
    first = b * 8;
    p->y = first * 178 + 8;
    *(u32 *)&p->w = e;
    *(u32 *)&p->cx = f;
    attribute = 0x8000000;
    do {
        p->cy = 0xF7;
        *(u16 *)&p->u = 0xF060;
        p->attribute = attribute;
        p->tpage = 0x1B;
    } while (0);
    do {
        idx = b * 8;
        p->y = a;
        p->y = a - gGraphics_sViewportY;
        for (i = 0; i < 10; idx += 10, i++) {
            y = p->y;
            j = idx + 1;
            tb = D_800EA1E8;
            if (y + p->w > 0) {
                if (y >= 0xF0) {
                    goto done;
                }
                k = idx + 0x65;
                c = 0;
                pk = &D_800EA1E8[k * 4];
                pj = &D_800EA1E8[j * 4];
                do {
#ifdef MEMORIES_PC
                    /* The last section row may be short (card_grid.h), and a
                       cell shows its card, whose variants have none. */
                    card_j = Cards_LibraryEntry(j);
                    r = card_j ? Library_GetCardFlags(tb, card_j) : 0;
#else
                    r = Library_GetCardFlags(tb, j);
#endif
                    if (r & 0x80) {
                        GS_SPRITE_COLOR_WORD(p) = white;
                        if (r & 1) {
                            GS_SPRITE_COLOR_WORD(p) = grey;
                        }
                        p->x = c + 8;
#ifdef MEMORIES_PC
                        p->cx = *(u16 *)(tb + card_j * 4 + 0x54);
#else
                        p->cx = *(u16 *)(pj + 0x54);
#endif
#ifdef MEMORIES_PC
                        icon_custom = CardLayoutArt_LibraryIconCell(card_j, &icon_tpage, &icon_u, &icon_v,
                                                                    &icon_clut, &icon_w, &icon_h);
                        if (icon_custom) {
                            p->tpage = icon_tpage;
                            p->u = icon_u;
                            p->v = icon_v;
                            p->cx = (icon_clut & 0x3F) * 16;
                            p->cy = icon_clut >> 6;
                            p->w = icon_w;
                            p->h = icon_h;
                        } else {
                            p->cy = 0xF7;
                            *(u16 *)&p->u = 0xF060;
                            *(u32 *)&p->w = e;
                            p->tpage = 0x1B;
                        }
                        if (icon_custom) Library_DrawCustomIcon(p, icon_tpage, ot);
                        else
#endif
                        GsSortFastSprite(p, ot, 2);
                    }
#ifdef MEMORIES_PC
                    card_k = Cards_LibraryEntry(k);
                    if (card_k) {
                        r = Library_GetCardFlags(tb, card_k);
#else
                    if (k < CARD_ID_END_LIVE) {
                        r = Library_GetCardFlags(tb, k);
#endif
                        if (r & 0x80) {
                            GS_SPRITE_COLOR_WORD(p) = white;
                            if (r & 1) {
                                GS_SPRITE_COLOR_WORD(p) = grey;
                            }
                            p->x = c + 0xA8;
#ifdef MEMORIES_PC
                            p->cx = *(u16 *)(tb + card_k * 4 + 0x54);
#else
                            p->cx = *(u16 *)(pk + 0x54);
#endif
#ifdef MEMORIES_PC
                            icon_custom = CardLayoutArt_LibraryIconCell(card_k, &icon_tpage, &icon_u, &icon_v,
                                                                        &icon_clut, &icon_w, &icon_h);
                            if (icon_custom) {
                                p->tpage = icon_tpage;
                                p->u = icon_u;
                                p->v = icon_v;
                                p->cx = (icon_clut & 0x3F) * 16;
                                p->cy = icon_clut >> 6;
                                p->w = icon_w;
                                p->h = icon_h;
                            } else {
                                p->cy = 0xF7;
                                *(u16 *)&p->u = 0xF060;
                                *(u32 *)&p->w = e;
                                p->tpage = 0x1B;
                            }
                            if (icon_custom) Library_DrawCustomIcon(p, icon_tpage, ot);
                            else
#endif
                            GsSortFastSprite(p, ot, 2);
                        }
                    }
                    pj += 4;
                    j++;
                    pk += 4;
                    c += 0xE;
                    k++;
                } while (c < 0x8A);
            }
            p->y = p->y + 0x10;
        }
        a += 178;
        n++;
        b += 25;
    } while (n < CARD_GRID_SECTION_ROW_COUNT);

done:
    q = (GsGLINE *)SCRATCHPAD_ADDR(0x1F800000);
    v = D_8009B09C & 0x7F;
    q->attribute = 0x50000000;
    q->b0 = 0;
    q->g0 = 0;
    q->r0 = 0;
    q->b1 = 0;
    q->g1 = 0;
    q->r1 = 0;
    list = D_800EA1E8;
    switch (v / 32) {
    case 0:
        q->g0 = v * 8;
        break;
    case 1:
        q->g0 = 0xFF;
        q->g1 = (v - 0x20) * 8;
        break;
    case 2:
        q->g0 = (0x5F - v) * 8;
        q->g1 = 0xFF;
        break;
    case 3:
        q->g1 = (0x7F - v) * 8;
        break;
    }
    q->x1 = 0;
    q->x0 = *(u16 *)(list + 8) - gGraphics_sViewportX;
    q->y0 = q->y1 = *(u16 *)(list + 0xA) - gGraphics_sViewportY;
    GsSortGLine(q, ot, 1);
    q->x1 = 0x140;
    GsSortGLine(q, ot, 1);
    q->y1 = 0;
    q->x1 = q->x0;
    GsSortGLine(q, ot, 1);
    q->y1 = 0xF0;
    GsSortGLine(q, ot, 1);
}
