#include "../types.h"
#include "build_deck_card_counts.h"
#include "build_deck_transition_state.h"
#include "card_constants.h"
#include "card_list_sort.h"
#include "display_object_layout.h"
#include "duel_card.h"
#include "text_box_lifecycle.h"
#include "text_box_runtime.h"
#include "text_staging.h"
#include "display_object.h"
#ifdef MEMORIES_PC
#include "pc/cards/tables.h"
#include "pc/cards/cards.h"
#include "pc/cards/build_deck_fold.h"
#endif

/* The Build Deck screen's three contiguous count helpers. They refresh the
   count box, return a card to the chest, and remove a card from it. The unit
   is the complete gcc_2_8_1_g0_split run between the card-list text boxes and
   func_8003201C.

   All three take the screen record as itself. An earlier note here said the state had to be cast at each use because a
   BuildDeckTransitionState * local cost an instruction; that is true of a
   local, and not of the parameter, which is the same incoming register
   either way. */

#ifdef MEMORIES_PC
/* After a card moved between the chest and the deck: the entry's row is shown or not by what the player has of the
   whole entry, the variants' rows never, the card on show is one the player has, and the list is sorted again. */
void BuildDeck_FoldRow(BuildDeckTransitionState *state, s32 id)
{
    CardList *chest = &state->lists[0];
    s32 head = BuildDeckFold_Head(id);
    s32 i;

    if (!BuildDeckFold_Active() || Cards_VariantCount(head) == 0) {
        return;
    }
    for (i = 0; i < chest->row_count; i++) {
        s32 row = (s16)chest->entries[i].id;
        if (row == head) {
            chest->entries[i].flags = BuildDeckFold_RowFlag(state->chest_card_quantities,
                                                           state->deck_card_quantities, head, 1, 0x80);
        } else if (BuildDeckFold_Head(row) == head) {
            chest->entries[i].flags = 0;
        }
    }
    BuildDeckFold_Settle(state, state->chest_card_quantities, state->deck_card_quantities, head);
    func_80032C48(chest);
}
#endif
void BuildDeck_RefreshCountDisplay(BuildDeckTransitionState *record) {
    DuelEffectChannel *p;
    D_801D5608[0].build_deck.chest = record->chest_total;
    D_801D5608[0].build_deck.deck = record->deck_total;
    p = TextBox_CreateFlagged(3, 0xE, 0x16, 0x17, 0x280, 0x10, 0x100);
    func_80039A14(p);
    p->field_28->flags &= ~DISPLAY_OBJECT_FLAG_SCREEN_SPACE;
}

void BuildDeck_ReturnCardToChest(BuildDeckTransitionState *base, s32 index)
{
    u32 raw = base->chest_card_quantities[index];
    s32 count = raw & 255;

    if (count == 0) {
        CardListSortItem *entry =
            (CardListSortItem *)base->lists[0].entries;

        base->chest_total++;
        base->chest_card_quantities[index]++;
        do {
            s32 id = entry->card_id;

            entry++;
            if (id == index)
                break;
        } while (1);
        entry--;
        /* Keep the post-search adjustment separate from the flag store. */
        ((volatile CardListSortItem *)entry)->field_0D = 1;
        func_80032C48(&base->lists[0]);
#ifdef MEMORIES_PC
    /* Up to 250, or a mod's chest past it (tables.h); below the room, as
       an 8-bit count at 255 must not wrap to none. */
    } else if (count < Tables_ChestRoom()) {
#else
    } else if (count != CARD_CHEST_QUANTITY_MAX) {
#endif
        s32 next = raw + 1;

        base->chest_card_quantities[index] = next;
        base->chest_total++;
    }
#ifdef MEMORIES_PC
    BuildDeck_FoldRow(base, index);
#endif
}

void BuildDeck_TakeCardFromChest(BuildDeckTransitionState *state, s32 id)
{
    s32 count = state->chest_card_quantities[id];

    if (count != 0) {
        count--;
        if (count == 0) {
            CardEntry *record = state->lists[0].entries;

            while (1) {
                if ((s16)record->id == id) {
                    break;
                }
                record++;
            }

            record->flags = 0;
            if (state->chest_card_quantities[id] != 0) {
                record->flags = 0x80;
            }
            func_80032C48(&state->lists[0]);
        }
        state->chest_card_quantities[id] = count;
        state->chest_total -= 1;
    }
#ifdef MEMORIES_PC
    BuildDeck_FoldRow(state, id);
#endif
}
