#include "../types.h"
#include "card_list_text_boxes.h"
#include "build_deck_active_card.h"
#ifdef MEMORIES_PC
#include "build_deck_transition_state.h"
#include "pc/cards/build_deck_fold.h"
#endif

s32 BuildDeck_GetActiveCardID(CardList *list)
{
    s32 index = list->first + list->cursor;
    CardEntry *entry = &list->entries[index];
    if (entry->flags == 0) {
        return 0;
    }
#ifdef MEMORIES_PC
    /* A row of the chest with variants is the card on show (build_deck_fold.h). */
    if (gBuildDeck_pState != 0 && list == &gBuildDeck_pState->lists[0] && BuildDeckFold_Active()) {
        return BuildDeckFold_Shown(gBuildDeck_pState, (s16)entry->id);
    }
#endif
    return (s16)entry->id;
}
