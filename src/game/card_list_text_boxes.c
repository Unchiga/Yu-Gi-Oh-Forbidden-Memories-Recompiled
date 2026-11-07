#include "../types.h"
#include "card_constants.h"
#include "display_object.h"
#include "display_object_layout.h"
#include "duel_effect.h"
#include "text_box_lifecycle.h"
#include "text_box_runtime.h"
#include "card_list_text_boxes.h"
#ifdef MEMORIES_PC
#include "build_deck_transition_state.h"
#include "pc/cards/build_deck_fold.h"
#include "pc/cards/cards.h"
#endif

/* Builds the text box for one card slot of a trade or deck screen. The entry
   is picked out of the list at the scroll offset plus the slot, and the list
   kind at +0x2D47 selects both the box template and a fixed 0x160 shift. */

void CardList_CreateSlotTextBox(CardList *list, s32 slot)
{
    DuelEffectChannel *box;
    s32 style;

    gDuel_wSelectedCardID = list->entries[list->first + slot].id;
#ifdef MEMORIES_PC
    /* The chest's row of a card with variants shows the one on show (build_deck_fold.h). */
    if (gBuildDeck_pState != 0 && list == &gBuildDeck_pState->lists[0] && BuildDeckFold_Active()) {
        gDuel_wSelectedCardID = BuildDeckFold_Shown(gBuildDeck_pState, gDuel_wSelectedCardID);
    }
#endif
    style = 0;
    if (list->entries[list->first + slot].flags != 0) {
        style = 6;
    }
#ifdef MEMORIES_PC
    Cards_RowNames(1);   /* a variant's row names it with its label */
#endif
    box = TextBox_Create(list->kind + 1, style, 0x22, 0x2B, 0x120, 0xB0);
    box->field_3A = slot * 22;
    box->field_28->flags &=
        ~DISPLAY_OBJECT_FLAG_SCREEN_SPACE;
    if ((list->entries[list->first + slot].flags & 0x80) != 0) {
        box->field_54 = 4;
    }
    if (list->kind != 0) {
        *(u16 *)&box->field_3C += 0x160;
    }
    if (slot != 0) {
        box->flags_34 |= 0x40;
    }
    func_80039A14(box);
#ifdef MEMORIES_PC
    Cards_RowNames(0);
#endif
}

void func_80031E04(CardList *list, s32 count)
{
    s32 i;

    for (i = 0; i < count; i++)
        CardList_CreateSlotTextBox(list, i);
}
