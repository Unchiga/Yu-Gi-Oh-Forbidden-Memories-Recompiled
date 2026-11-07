#include "../types.h"
#include "../psyq/libgte.h"
#include "../psyq/libgpu.h"
#include "../psyq/libgs.h"
#include "display_object.h"
#include "text_encode_decimal_digits.h"
#include "text_encode_decimal_no_padding.h"
#include "card_type_icon_table.h"
#include "build_deck_transition_state.h"
#include "color_constants.h"
#ifdef MEMORIES_PC
#include "pc/cards/cards.h"
#include "pc/cards/tables.h"
#include "pc/cards/build_deck_fold.h"
#include "duel_card.h"
#endif
#include "duel_card_stat_display.h"
#include "func_80031784.h"
#define GRAPHICS_VIEWPORT_IN_DATA
#include "graphics_frame.h"
#include "func_80031874.h"

#define GS_SPRITE_COLOR_WORD(sprite) (*(u32 *)&(sprite)->r)

/* Both sprites are GsSPRITE records in the scratchpad: `sprite` at 0x1F800020
 * draws the rows' digits and icons, `header` at 0x1F800060 the sort menu, and
 * the digit text is built at 0x1F800000. `row` points 8 bytes into the page's
 * first CardEntry: -4 is the card id, -2 and 0 its ATK and DEF, +2 its type
 * (below 0x14 for monsters) and +5 its flags.
 *
 * The empty loop ends the prologue's basic block before the list lookup. With
 * it, GCC's scheduler keeps the viewport subtraction for x where retail has
 * it, instead of sinking it to the branch. */
void func_80031874(DisplayObject *obj, GsOT *ot)
{
    u8 *text;
    GsSPRITE *sprite = (GsSPRITE *)SCRATCHPAD_ADDR(0x1F800020);
    GsSPRITE *header = (GsSPRITE *)SCRATCHPAD_ADDR(0x1F800060);
    CardList *list;
    u8 *row;
    s32 kind;
    s32 x0;
    s32 x;
    s32 vy;
    s32 y0;
    s32 y;
    s32 i;
    s32 ry;
    s32 id;
    s32 n;
    u32 attr;
#ifdef MEMORIES_PC
    s32 head, atk, def, folded;
#endif

    x0 = (s16)obj->field_30.h.field_30;
    y0 = (s16)obj->field_30.h.field_32;
    attr = obj->attribute;
    header->tpage = 0xB;
    sprite->tpage = 0xB;
    GS_SPRITE_COLOR_WORD(sprite) = COLOR_RGB24_NEUTRAL_GREY;
    *(u32 *)&sprite->w = 0x80008;
    *(u32 *)&header->w = 0x100010;
    sprite->cx = 0x290;
    sprite->cy = 0xFA;
    vy = gGraphics_sViewportY;
    x = x0 - gGraphics_sViewportX;
    header->attribute = attr;
    sprite->attribute = attr;
    kind = obj->field_67;
    y = y0 - vy;
    do {
    } while (0);
    list = &gBuildDeck_pState->lists[kind];
    row = (u8 *)&list->entries[list->first];
    text = (u8 *)SCRATCHPAD_ADDR(0x1F800000);
    if (kind == 0) {
        header->x = x + 0x88;
        header->y = y + 0xF;
        func_80031784(header, ot, D_80090DD8, list->sort_mode);
    } else {
        header->x = x + 0x6A;
        header->y = y + 0xF;
        func_80031784(header, ot, &D_80090DD8[kind * 16],
                      list->sort_mode);
    }
    i = 0;
    ry = 0x2B;
    row += 8;
    do {
        sprite->x = x + 4;
        sprite->y = ry;
        if (row[5] != 0) {
            GS_SPRITE_COLOR_WORD(sprite) = COLOR_RGB24_NEUTRAL_GREY;
            id = *(s16 *)(row - 4);
            if (row[5] & 0x80) {
                GS_SPRITE_COLOR_WORD(sprite) = COLOR_RGB24_DIM_GREY;
            }
            if (kind != 0) {
                sprite->x = x + 0x11;
                Text_EncodeDecimalNoPadding(list->first + i + 1, 2, text);
                func_800316F0(sprite, ot, text, 2);
                sprite->x += 4;
            } else if (gBuildDeck_pState->card_sort_rank[id] != 0) {
                sprite->v = 0x68;
                sprite->w = 0x18;
                sprite->u = 0xE8;
                sprite->y += 8;
                GsSortFastSprite(sprite, ot, 0);
                sprite->w = 8;
                sprite->y -= 8;
            }
#ifdef MEMORIES_PC
            /* A row of the chest that is a card and its variants (build_deck_fold.h) shows the card on show: its
               number, its stats and its own count; the deck's count is the entry's, as the limit is. */
            head = id;
            atk = *(s16 *)(row - 2);
            def = *(s16 *)row;
            folded = kind == 0 && BuildDeckFold_Active() && Cards_VariantCount(head) > 0;
            if (folded) {
                id = BuildDeckFold_Shown(gBuildDeck_pState, head);
                if (id != head) {
                    s32 stats = gDuel_adwCardStats[id - 1];
                    atk = (stats & 0x1FF) * 10;
                    def = ((stats >> 9) & 0x1FF) * 10;
                }
            }
            /* The PC port's ids reach past 999 (card_constants.h): those
               are drawn with narrower steps, in the same 24 pixels. */
            if (id >= 1000) {
                s32 digits = id >= 10000 ? 5 : 4;
                s32 step = digits == 5 ? 5 : 6;
                s32 at = sprite->x;
                s32 d;

                Text_EncodeDecimalNoPadding(id, digits, text);
                sprite->v = 0x70;
                for (d = digits - 1; d >= 0; d--) {
                    if (text[d] < 0xA) {
                        sprite->u = text[d] * 8 - 0x80;
                        GsSortFastSprite(sprite, ot, 0);
                    }
                    sprite->x += step;
                }
                sprite->x = at + 3 * 8;
            } else {
                Text_EncodeDecimalNoPadding(id, 3, text);
                func_800316F0(sprite, ot, text, 3);
            }
#else
            Text_EncodeDecimalNoPadding(id, 3, text);
            func_800316F0(sprite, ot, text, 3);
#endif
            sprite->x += 0x88;
            if (row[2] < 0x14) {
                /* u 0xD0, v 0x58: the ATK label. */
                *(u16 *)&sprite->u = 0x58D0;
                GsSortFastSprite(sprite, ot, 0);
                sprite->x += 8;
#ifdef MEMORIES_PC
                Text_EncodeDecimalDigits(atk, 4, text);
#else
                Text_EncodeDecimalDigits(*(s16 *)(row - 2), 4, text);
#endif
                func_800316F0(sprite, ot, text, 4);
                /* u 0xD8, v 0x58: the DEF label, one line down. */
                *(u16 *)&sprite->u = 0x58D8;
                sprite->x -= 0x28;
                sprite->y += 8;
                GsSortFastSprite(sprite, ot, 0);
                sprite->x += 8;
#ifdef MEMORIES_PC
                Text_EncodeDecimalDigits(def, 4, text);
#else
                Text_EncodeDecimalDigits(*(s16 *)row, 4, text);
#endif
                func_800316F0(sprite, ot, text, 4);
                sprite->y -= 8;
            }
            if (kind == 0) {
                sprite->x = x + 0x107;
                sprite->y += 8;
                Text_EncodeDecimalDigits(
                    gBuildDeck_pState->chest_card_quantities[id], 3, text);
                func_800316F0(sprite, ot, text, 3);
                n = gBuildDeck_pState->deck_card_quantities[id];
#ifdef MEMORIES_PC
                if (folded) {
                    /* Another variant's copies are not this one's own count, though the limit is shared: the
                       entry's copies in the deck, red at the limit as ever. */
                    n = Cards_FamilyCopies(gBuildDeck_pState->deck_card_quantities, id);
                }
#endif
                /* Red once the deck holds the limit: three copies, or one of
                   ids 0x11-0x15. */
#ifdef MEMORIES_PC
                /* At a mod's "deck_copies" (tables.h), else the three; a
                   variant counts with its card (cards.h). */
                if (Cards_FamilyCopies(gBuildDeck_pState->deck_card_quantities, id) >=
                        Tables_Value(TABLES_VALUE_DECK_COPIES, 3) ||
                    ((u32)(id - 0x11) < 5 && n != 0)) {
#else
                if (n >= 3 || ((u32)(id - 0x11) < 5 && n != 0)) {
#endif
                    GS_SPRITE_COLOR_WORD(sprite) = 0x2020FF;
                }
                sprite->x = x + 0x122;
                Text_EncodeDecimalDigits(n, 2, text);
                func_800316F0(sprite, ot, text, 2);
                GS_SPRITE_COLOR_WORD(sprite) = COLOR_RGB24_NEUTRAL_GREY;
                sprite->y -= 8;
#ifdef MEMORIES_PC
                if (folded) {
                    /* What the player has of the whole entry, when that is more than the card on show's own,
                       dimmer, on the line above the count. */
                    s32 whole = BuildDeckFold_ChestTotal(gBuildDeck_pState->chest_card_quantities, head);
                    if (whole != gBuildDeck_pState->chest_card_quantities[id]) {
                        GS_SPRITE_COLOR_WORD(sprite) = COLOR_RGB24_DIM_GREY;
                        sprite->x = x + 0x107;
                        /* Right-aligned, no leading zeros (a glyph past 9 is left blank). */
                        text[0] = whole % 10;
                        text[1] = whole >= 10 ? (whole / 10) % 10 : 0xFF;
                        text[2] = whole >= 100 ? (whole / 100) % 10 : 0xFF;
                        func_800316F0(sprite, ot, text, 3);
                        GS_SPRITE_COLOR_WORD(sprite) = COLOR_RGB24_NEUTRAL_GREY;
                    }
                }
#endif
            }
        }
        row += sizeof(CardEntry);
        i++;
        ry += 0x16;
    } while (i < 8);
}
