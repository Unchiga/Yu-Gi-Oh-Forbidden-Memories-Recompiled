#define GINPUT_PAD1_HELD_IS_VOLATILE
#define GINPUT_PAD1_PRESSED_IS_VOLATILE
#include "../types.h"
#include "build_deck_active_card.h"
#include "func_80032B38.h"
#include "card_list_sort.h"
#include "card_list_text_boxes.h"
#include "card_constants.h"
#include "duel_card_viewer.h"
#include "input.h"
#include "sound.h"
#include "build_deck_card_counts.h"
#include "build_deck_transition_state.h"
#include "duel_card_stat_display.h"
#include "duel_effect.h"
#include "../unmatched.h"
#include "build_deck_pane_input.h"
#include "graphics_frame.h"
#include "build_deck_update_pane_transition.h"
#ifdef MEMORIES_PC
#include "pc/cards/cards.h"
#include "pc/cards/tables.h"
#include "pc/cards/build_deck_fold.h"
#endif

void BuildDeck_UpdateDeckPaneInput(BuildDeckTransitionState *state) {
    CardList *e;
    s32 r;

    e = &state->lists[state->pane_index];

    BuildDeck_TestAndSetInitialized(state);

    if (BuildDeck_UpdateCardListInput(e) != 0) {
        return;
    }

    if ((gInput_wPad1Pressed & PAD_BUTTON_TRIANGLE) != 0) {
        r = BuildDeck_GetActiveCardID(e);
        if (r != 0) {
            gDuel_bCardViewerYOffset = 0x14;
            gDuel_wViewerCardID = r;
            gDuel_bEffectState = DUEL_EFFECT_STATE_CARD_VIEWER;
        }
        return;
    }

    if (gInput_wPad1Held == PAD_DIRECTION_LEFT) {
        state->state = 1;
        state->viewport_target_x = 0;
        state->next_state = 2;
        return;
    }

    if ((gInput_wPad1Pressed & PAD_BUTTON_CANCEL) != 0) {
        state->state = 4;
        state->next_state = 3;
        return;
    }

    if ((gInput_wPad1Repeat & PAD_BUTTON_CONFIRM_MASK) != 0) {
        r = BuildDeck_GetActiveCardID(e);
        if (r != 0) {
            SD_SEPlayFull(7);
            e->entries[e->first + e->cursor].flags = 0;
            func_80032C48(&state->lists[1]);
            func_8003201C(state);
            BuildDeck_ReturnCardToChest(state, r);
#ifdef MEMORIES_PC
            /* The chest's row shows the card that came back (build_deck_fold.h). */
            if (BuildDeckFold_Active() && Cards_VariantCount(BuildDeckFold_Head(r)) > 0) {
                BuildDeckFold_Pick(state, r);
                func_80031E04(&state->lists[0], 8);
            }
#endif
            BuildDeck_RefreshCountDisplay(state);
            func_80031574(r, 0x234, 0x16, 0x162, 0xA);
            return;
        }
        SD_SEPlayFull(9);
    }
}

void BuildDeck_UpdateChestPaneInput(BuildDeckTransitionState *state)
{
    CardList *e;
    s32 r;
    u32 c;

    e = &state->lists[state->pane_index];
    BuildDeck_TestAndSetInitialized(state);
    if (BuildDeck_UpdateCardListInput(e) != 0) {
        return;
    }
#ifdef MEMORIES_PC
    /* L3 and R3 step through the cards of the row's entry that the player has (build_deck_fold.h): L1/R1 and L2/R2
       are the list's pages. */
    if ((gInput_wPad1Pressed & PAD_BUTTON_L3_R3_MASK) != 0 && BuildDeckFold_Active()) {
        r = BuildDeck_GetActiveCardID(e);
        if (r != 0 && Cards_VariantCount(BuildDeckFold_Head(r)) > 0 &&
            BuildDeckFold_Step(state, state->chest_card_quantities, state->deck_card_quantities,
                               BuildDeckFold_Head(r), (gInput_wPad1Pressed & PAD_BUTTON_R3) ? 1 : -1)) {
            SD_SEPlayFull(6);
            func_80031E04(e, 8);
        }
        return;
    }
#endif

    if ((gInput_wPad1Pressed & PAD_BUTTON_TRIANGLE) != 0) {
        r = BuildDeck_GetActiveCardID(e);
        if (r != 0) {
            gDuel_bCardViewerYOffset = 0x14;
            gDuel_wViewerCardID = r;
            gDuel_bEffectState = DUEL_EFFECT_STATE_CARD_VIEWER;
        }
        return;
    }

    if (gInput_wPad1Held == PAD_DIRECTION_RIGHT) {
        state->viewport_target_x = 0x140;
        state->state = 1;
        state->next_state = 3;
        return;
    }

    if ((gInput_wPad1Pressed & PAD_BUTTON_CANCEL) != 0) {
        state->state = 4;
        state->next_state = 2;
        return;
    }

    if ((gInput_wPad1Repeat & PAD_BUTTON_CONFIRM_MASK) == 0) {
        return;
    }

    r = BuildDeck_GetActiveCardID(e);
    c = 1;
#ifdef MEMORIES_PC
    /* A piece a mod made another card is no longer one of a kind. */
    if (Cards_ExodiaPiece(r)) {
#else
    if ((u32)(r - EXODIA_FIRST_CARD_ID) < EXODIA_PIECE_COUNT) {
#endif
        c = state->deck_card_quantities[r] < c;
    }

    if (r != 0 && c != 0) {
        if (state->deck_total < DECK_SIZE &&
            state->chest_card_quantities[r] != 0 &&
#ifdef MEMORIES_PC
            /* Three, or a mod's "deck_copies" (tables.h), of a card and its
               variants together (cards.h). */
            Cards_FamilyCopies(state->deck_card_quantities, r) <
                Tables_Value(TABLES_VALUE_DECK_COPIES, DECK_CARD_COPY_LIMIT)) {
#else
            state->deck_card_quantities[r] < DECK_CARD_COPY_LIMIT) {
#endif
            SD_SEPlayFull(7);
            BuildDeck_AddCard((s32)state, r);
            BuildDeck_TakeCardFromChest(state, r);
            BuildDeck_RefreshCountDisplay(state);
            func_80031574(r, 3, 0x18, 0x11C, 0xC);
            return;
        }
    }

    SD_SEPlayFull(9);
}

#define BUILD_DECK_PANE_TRANSITION_TICKS 16

void BuildDeck_UpdatePaneTransition(BuildDeckTransitionState *state)
{
    s32 ticks;

    if (BuildDeck_TestAndSetInitialized(state) == 0) {
        s32 diff = state->viewport_target_x - (s16)gGraphics_sViewportX;

        state->viewport_step_x = diff / BUILD_DECK_PANE_TRANSITION_TICKS;
        state->transition_ticks = BUILD_DECK_PANE_TRANSITION_TICKS;
        SD_SEPlayFull(30);
    }

    gGraphics_sViewportX += (u16)state->viewport_step_x;
    ticks = state->transition_ticks - 1;
    state->transition_ticks = ticks;
    if (ticks == 0) {
        u16 position = (u16)state->viewport_target_x;

        state->pane_index = 0;
        gGraphics_sViewportX = position;
        if ((s32)position << 16) {
            state->pane_index = 1;
        }
        state->state = state->next_state;
    }
}
