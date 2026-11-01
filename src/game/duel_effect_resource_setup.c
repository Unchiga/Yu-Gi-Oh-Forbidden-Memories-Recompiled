#include "../types.h"
#include "card_constants.h"
#include "file_transfer.h"
#include "duel_effect_resource_record.h"
#include "duel_effect_resource_setup.h"
#include "display_object.h"
#include "display_object_core.h"
#include "display_object_config.h"
#include "display_object_helpers.h"
#include "duel_card.h"
#include "func_800291E0.h"
#include "card_preview_callbacks.h"
#ifdef MEMORIES_PC
#define MAIN_MODE_STATE_ACTIVE_AS_SCALAR
#include "main_mode_state.h"
#include "pc/cards/cards.h"
#include "pc/cards/card_layout.h"
#endif

#define DISPLAY_OBJECT_FIELD_5E_BYTES(object) ((u8 *)&(object)->field_5E)

FileTransferDescriptor *func_80029164(s32 slot, s32 value)
{
    FileTransferDescriptor *object;

    D_800EA0E8[slot].field_30 = value;
#ifdef MEMORIES_PC
    /* A card past the disc's has its base's artwork (Cards_BaseId). */
    object = File_TryRequestAsyncTransfer(
        0, 0, (Cards_BaseId(value) - 1) * 7 + CARD_COUNT, 7, func_800289BC, 0, 0);
#else
    object = File_TryRequestAsyncTransfer(
        0, 0, (value - 1) * 7 + CARD_COUNT, 7, func_800289BC, 0, 0);
#endif
    object->callback_data = (void *G32)slot;
    D_8009B0F4_abs =
        object->status_flags | FILE_TRANSFER_STATE_PRIMARY_ACTIVE;
    return object;
}

/*
 * Retail leaves the primary preview object in $v0 and callers consume that
 * pointer. The second argument supplies the default-path X coordinate before
 * the same local becomes a setup value; negative coordinates select the
 * packed-stat fallback. The primary object is published at record offset zero
 * and linked from the secondary object at offset four.
 */
u8 *func_800291E0(s32 index, s32 x, s32 y)
{
    DuelEffectResourceRecord *entry;
    DisplayObject *object;
    s32 variant;
    s32 setup;
    s32 card_id;
    u8 byte_value;

    setup = x;
    entry = &D_800EA0E8[index];

    object = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 6);
    DisplayObject_ConfigureScreenSprite(object, 0, 0, 0x66, 0x60, 0, 0,
        ((s16)entry->src_x >> 6) + (((s16)entry->src_y >> 8) << 4),
        (s16)entry->field_2C, (s16)entry->field_2E);

    object->field_30.word = 0x001F0048;
    object->field_3C.word = 0x00100038;
    object->field_5E = 0x9E00;
    object->attribute |= 0x01000000;

    card_id = (s16)entry->field_30;
    variant = (gDuel_adwCardStats[card_id - 1] >> 26) & 0x1F;
    object->field_67 = index;
    object->field_68 = (u8)variant;
#ifdef MEMORIES_PC
    /* Ahead of CARD_LAYOUT_FRAME below: harmless for that one call
     * (visibility there is full_bleed-only, not kind-specific), but keeps
     * this object's card_layout.c state from reading as whatever card
     * func_80028B08.c last drew instead of its own. */
    CardLayout_SetCard(card_id);
#endif

    if (variant == 0x15) {
        goto disp_15;
    }
    if (variant >= 0x16) {
        goto disp_ge16;
    }
    if (variant == 0x14) {
        goto disp_14;
    }
    object->field_30.h.field_30 = 0x48;
    goto path_a;

disp_ge16:
    if (variant == 0x16) {
        goto disp_16;
    }
    if (variant == 0x17) {
        goto disp_17;
    }
    object->field_30.h.field_30 = 0x48;
    goto path_a;

disp_16:
    setup = 0x103;
    variant = 1;
    goto shared_tail;

disp_17:
    byte_value = DISPLAY_OBJECT_FIELD_5E_BYTES(object)[1];
    byte_value = (u8)(byte_value + 0x20);
    DISPLAY_OBJECT_FIELD_5E_BYTES(object)[1] = byte_value;
    /* fallthrough */
disp_14:
    setup = 0x101;
    variant = 1;
    goto shared_tail;

disp_15:
    setup = 0x102;
    byte_value = DISPLAY_OBJECT_FIELD_5E_BYTES(object)[1];
    variant = 1;
    byte_value = (u8)(byte_value + 0x10);
    DISPLAY_OBJECT_FIELD_5E_BYTES(object)[1] = byte_value;
    goto shared_tail;

path_a:
    object->field_30.h.field_32 = 0x9E;
    DISPLAY_OBJECT_FIELD_5E_BYTES(object)[1] = 0xCE;
    object->field_3C.h.field_3C = 0x18;
    object->field_3C.h.field_3E = 0xC;

    {
        s16 x_value = setup;
        entry->field_32 = x_value;
        if (x_value < 0) {
            entry->field_32 =
                (s16)((gDuel_adwCardStats[card_id - 1] & 0x1FF) * 10);
        }
    }
    {
        s16 y_value = y;
        entry->field_34 = y_value;
        if (y_value < 0) {
            entry->field_34 =
                (s16)(((gDuel_adwCardStats[card_id - 1] >> 9) & 0x1FF) * 10);
        }
    }
    setup = 0x100;
    variant = 0;
    entry->field_38 = 0;
    entry->field_36 = 0;

shared_tail:
#ifdef MEMORIES_PC
    entry->field_3B = (u8)Cards_Attribute(card_id);
#else
    entry->field_3B = (u8)(gDuel_abCardLevelAttr[card_id] >> 4);
#endif
    entry->field_3A = (u8)(gDuel_abCardLevelAttr[card_id] & 0xF);
    entry->field_3C = 0;

    object->field_4C = (s32)func_80028B08;
    DisplayObject_SelectOrderingTable1(object);

    entry->object_04 = object;

    object = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(),
#ifdef MEMORIES_PC
        /* List 2 is what actually draws a sprite-sheet object's own
         * graphic (DisplayObject_RenderSpriteSheetList); list 0 only ever
         * runs its update callback (DisplayObject_RunUpdateCallbackList),
         * never submits one. func_80028B08 does not care which list this
         * object is in -- it reaches it directly through field_54, below
         * -- so this is invisible to it either way; only the frame's own
         * drawn graphic goes. Its flags (DISPLAY_OBJECT_RENDERABLE_MASK,
         * which func_80028B08 does check) come from AcquireSlot itself
         * and are the same regardless of the list argument.
         *
         * Hiding this the instant full_bleed was on used to leave a real
         * gap: CardLayout_DrawFrame's own replacement does not start
         * drawing until this object is renderable, which the open
         * animation reaches several frames after this one is created, and
         * nothing filled that window -- confirmed live, the frame/art/
         * stats area sat empty through the slide-in where retail always
         * has this sprite showing. A mod with its own frame image always
         * ends up fully covering this by the time it settles (checked:
         * no seam or double-frame), so keep it as a stopgap. Only a mod
         * that configures no frame image at all for this card's kind
         * (notes/modding.md's "art and stats floating with no backing at
         * all") hides it immediately, since nothing will ever cover it. */
        !CardLayout_Part(CARD_LAYOUT_FRAME).visible && !*CardLayout_FramePath() ? 0 : 2
#else
        2
#endif
    );
#ifdef MEMORIES_PC
    /* A mod's frame color (Cards_FrameColor) is the frame's palette row;
     * the layout stays the type's. */
    if (Cards_FrameColor(card_id) >= 0) {
        setup = 0x100 + Cards_FrameColor(card_id);
    }
#endif
    DisplayObject_ConfigureSpriteAtPosition(object, 2, 4, 1, 0, variant, 0x1C, setup + 8);

    object->field_18 = 0x46;
    object->field_48.h.field_48 = 0x46;
    object->field_1A = 0x62;
    object->field_48.h.field_4A = 0x62;
    object->field_6A = (u8)variant;
    object->attribute |= 0x01000000;
    object->flags |= 8;

    DisplayObject_SelectOrderingTable1(object);
#ifdef MEMORIES_PC
    /* Card details use the preview's depth minus one. During duels, keep
     * both the frame and details in front of the FIELD sprite. Other card
     * viewers retain the original depth. */
    DisplayObject_SetDepthOffset(object, (D_8009B26C & 0x1F) == 3 ? 1 : -1);
#else
    DisplayObject_SetDepthOffset(object, -1);
#endif

    ((DisplayObject *)entry->object_04)->field_65 = 3;
    object->field_65 = 3;
    object->field_10 = (u32)CardPreview_UpdateVariant;

    {
        DisplayObject *secondary = entry->object_04;
        DisplayObject *primary;

        primary = object;

        entry->object_00 = primary;
        secondary->field_54 = primary;
    }
    return (u8 *)object;
}
