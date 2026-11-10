/* Browsing cards in the card viewer (card_browse.h).
 *
 * The viewer (DuelEffect_UpdateCardViewerState) opens by loading the card
 * into resource slot 3 (func_80029164 starts the disc read, whose callback
 * puts its art in VRAM), making the card's picture (func_800291E0) and a
 * text box of its description, then sliding them in; while they slide it
 * waits for the read, then fades the card in, then shows it until Circle.
 * Closing lets the card go with func_80029528(3) and destroys the text box.
 *
 * Another card is shown the same way without the slides: the card and its
 * text box are let go as closing does, the opening's steps are taken again
 * with the pieces where they slide to, and the viewer is handed back to its
 * own wait for the read and fade. The background panel stays. So the card
 * is loaded and drawn exactly as when triangle opens it. The VIEWER_*
 * values below are the opening's own, and must follow it if it changes.
 *
 * Each screen that opens the viewer on a list has an entry in `screens`: it
 * moves that list's cursor one card along, as the list's own input would,
 * and says which card is under it. It only does so when the viewer shows
 * the card under the cursor, so a list only browses the viewer opened from
 * it. The lists do not read the pad while the viewer is up, so nothing else
 * moves them meanwhile.
 *
 * A list's page is rebuilt, when the cursor leaves it, before the viewer's
 * text box is made: the rebuild leaves gDuel_wSelectedCardID on its last
 * row, and the description is set from gDuel_wSelectedCardID.
 *
 * L3 and R3 show another variant of the card's entry (build_deck_fold.h), where
 * Build Deck folds the chest's rows: the row's shown card changes with it, so
 * the list is as the viewer leaves it. They work with or without Up/Down
 * browsing.
 *
 * Build Deck (mode 7): the active pane's list (build_deck_pane_input.c).
 * The pane is idle while the viewer is up (func_800339D0 only runs it when
 * DuelEffect_UpdateState is).
 *
 * The pre-duel chest (mode 3, Main_RunDuel's own step 0, "which deck first"
 * before a campaign duel): Build Deck's own screen, reused in place
 * (main_run_duel.c calls the same func_800323F8 Build Deck does, so
 * gBuildDeck_pState is the same list, and move_build_deck_cursor needs no
 * change) -- only while it is actually open and interactive, D_8009B26E ==
 * 0x80 (deck_menu.c's own duel_chest()), never during the duel proper,
 * which reuses the same main mode but a different step. */
#define GINPUT_PAD1_REPEAT_SIZED_VOLATILE /* both pads: [0] and [1] */
#include "card_browse.h"
#include "stars.h"
#include "cards.h"
#include "build_deck_fold.h"
#include "pc/platform/settings.h"
#include "types.h"
#include <stddef.h>
#include "game/input.h"
#include "game/sound.h"
#include "game/main_modes.h"
#include "game/display_object.h"
#include "game/display_object_core.h"
#include "game/display_object_helpers.h"
#include "game/display_object_interpolation.h"
#include "game/duel_effect.h"
#include "game/duel_card.h"
#define DUEL_CARD_VIEWER_ADDRESS_ALIASES /* the viewer's card, background and text box */
#include "game/duel_card_viewer.h"
#include "game/duel_effect_resource_record.h"
#include "game/duel_effect_resource_setup.h"
#include "game/func_800291E0.h"
#include "game/func_80029574.h"
#include "game/card_constants.h"
#include "game/text_box_lifecycle.h"
#include "game/text_box_runtime.h"
#include "game/card_list_text_boxes.h"
#include "game/build_deck_transition_state.h"

extern u8 D_8009B26C; /* main_mode_state.h: the active mode */
extern u8 D_8009B26E; /* main_run_duel.c: Main_RunDuel's own step, 0x80 once its screen is up */

/* DuelEffect_UpdateCardViewerState's opening: the resource slot and where
 * its texture goes, where the card slides to (its slide-in's end), its
 * starting fade and depth, and the description text box. */
#define VIEWER_SLOT 3
#define VIEWER_TEXTURE_X 0
#define VIEWER_TEXTURE_Y 0x100
#define VIEWER_CARD_X 2
#define VIEWER_CARD_FADE 0x80
#define VIEWER_DEPTH 0x14
#define VIEWER_TEXT_BOXES 3
#define VIEWER_TEXT_X 0x148
#define VIEWER_TEXT_Y 0xE
#define VIEWER_TEXT_W 0xA8
#define VIEWER_TEXT_H 0xC0
#define VIEWER_TEXT_STYLE 0x15
/* The viewer's state byte (D_8009B248): 0x40 while its pieces slide and it
 * waits for the card's read, 0x20 once the card has faded in, 0x10 while
 * closing. */
#define VIEWER_WAITING 0x40
#define VIEWER_SHOWN 0x20
#define VIEWER_CLOSING 0x10

/* Sounds: the lists' cursor, and the game's "cannot". */
#define SOUND_CURSOR 6
#define SOUND_CANNOT 9

/* BuildDeck_UpdateCardListInput: eight rows to a page, a row 22 pixels
 * down from 0x2A. */
#define BUILD_DECK_PAGE_ROWS 8
#define BUILD_DECK_ROW_HEIGHT 22
#define BUILD_DECK_ROW_TOP 0x2A

/* The first row of the page of `page_rows` (over a list of `rows`) that
 * shows `row`, the cursor (now at `cursor` on its page) keeping its place
 * where it can. */
static int page_for(int row, int cursor, int rows, int page_rows)
{
    int first = row - cursor;
    if (first > rows - page_rows) first = rows - page_rows;
    return first < 0 ? 0 : first;
}

/* A screen's `move`: NOT_THIS_LIST when the pad's list did not open the
 * viewer (the viewer shows another card than the one under its cursor). */
#define NOT_THIS_LIST 0xFFFF

/* The card a row of the list shows: the chest's rows of a card with variants show the one on show. */
static u16 row_card(BuildDeckTransitionState *state, CardList *list, int row)
{
    u16 id = list->entries[row].id;
    if (list == &state->lists[0] && BuildDeckFold_Active()) return (u16)BuildDeckFold_Shown(state, id);
    return id;
}

/* Build Deck: moves the list's cursor to the next card `step` (1 or -1)
 * along, and returns it; 0 at either end. */
static u16 move_build_deck_cursor(int pad, int step)
{
    BuildDeckTransitionState *state = gBuildDeck_pState;
    CardList *list;
    int rows, row, at;
    if (pad != 0 || !state) return NOT_THIS_LIST;
    list = &state->lists[state->pane_index];
    rows = list->row_count;
    at = list->first + list->cursor;
    if (list->first != list->first_target || row_card(state, list, at) != gDuel_wViewerCardID) return NOT_THIS_LIST;
    for (row = at + step; row >= 0 && row < rows; row += step) {
        int first;
        if (list->entries[row].flags == 0) continue;
        first = page_for(row, list->cursor, rows, BUILD_DECK_PAGE_ROWS);
        list->cursor = (s8)(row - first);
        list->cursor_box->field_30.h.field_32 = list->cursor * BUILD_DECK_ROW_HEIGHT + BUILD_DECK_ROW_TOP;
        if (first != list->first) {
            list->first = list->first_target = (s16)first;
            func_80031E04(list, BUILD_DECK_PAGE_ROWS);
        }
        return row_card(state, list, row);
    }
    return 0;
}

/* L3 / R3 in Build Deck: the next variant of the chest row's entry that the player has; 0 when there is none to
 * show, NOT_THIS_LIST when the viewer is not on a folded row. */
static u16 vary_build_deck(int step)
{
    BuildDeckTransitionState *state = gBuildDeck_pState;
    CardList *list;
    int at, head;
    if (!state || !BuildDeckFold_Active()) return NOT_THIS_LIST;
    list = &state->lists[state->pane_index];
    if (list != &state->lists[0]) return NOT_THIS_LIST;
    at = list->first + list->cursor;
    head = list->entries[at].id;
    if (list->first != list->first_target || Cards_VariantCount(head) == 0 ||
        row_card(state, list, at) != gDuel_wViewerCardID) {
        return NOT_THIS_LIST;
    }
    if (!BuildDeckFold_Step(state, state->chest_card_quantities, state->deck_card_quantities, head, step)) return 0;
    func_80031E04(list, BUILD_DECK_PAGE_ROWS);
    return row_card(state, list, at);
}

static u16 vary_duel_chest(int step)
{
    if (D_8009B26E != 0x80) return NOT_THIS_LIST;
    return vary_build_deck(step);
}

/* The pre-duel chest: Build Deck's own list and cursor (gBuildDeck_pState is
 * never cleared between the two, main_run_duel.c's own func_800323F8 call),
 * so move_build_deck_cursor needs no change -- only guarded to the chest's
 * own step, since Main_RunDuel reuses MAIN_MODE_DUEL for the duel proper
 * too, where gBuildDeck_pState is still whatever the chest last left it
 * (deck_menu.c's own duel_chest() check, the proven way to tell them
 * apart). */
static u16 move_duel_chest_cursor(int pad, int step)
{
    if (D_8009B26E != 0x80) return NOT_THIS_LIST;
    return move_build_deck_cursor(pad, step);
}

/* The screens that open the viewer on a list, by main mode. `move` takes
 * the pad pressed (0 or 1) and the step; it returns the card now under the
 * cursor, 0 at the end of the list, or NOT_THIS_LIST. */
static const struct {
    int mode;
    u16 (*move)(int pad, int step);
    u16 (*vary)(int step);
} screens[] = {
    {MAIN_MODE_BUILD_DECK, move_build_deck_cursor, vary_build_deck},
    {MAIN_MODE_DUEL, move_duel_chest_cursor, vary_duel_chest},
};

/* The viewer's opening for `id`, its pieces already where they slide to. */
static void show(u16 id)
{
    DuelEffectResourceRecord *record = &D_800EA0E8[VIEWER_SLOT];
    DisplayObject *background = D_8009B240, *card;
    DuelEffectChannel *channel = D_800EB0F8;
    int i;

    func_80029528(VIEWER_SLOT);
    if (D_8009B250) TextBox_Destroy(D_8009B250);
    D_8009B250 = 0;

    DuelEffect_ClearResourceObjectPointers(VIEWER_SLOT);
    record->src_y = VIEWER_TEXTURE_Y;
    record->src_x = VIEWER_TEXTURE_X;
    record->field_2C = 0;
    record->field_2E = 0xFF;
    gDuel_wViewerCardID = id;
    func_80029164(VIEWER_SLOT, (s16)id);
    card = (DisplayObject *)func_800291E0(VIEWER_SLOT, -1, -1);
    *(s16 *)&card->field_30.h.field_30 = VIEWER_CARD_X;
    card->field_20.b.field_21 = VIEWER_CARD_FADE;
    card->field_30.h.field_32 += gDuel_bCardViewerYOffset;
    card->flags |= DISPLAY_OBJECT_FLAG_CLIP_TEST;
    DisplayObject_SavePosition((DisplayObjectSnapshot *)card);
    card->field_60 = 0;
    DisplayObject_SelectOrderingTable1(card);
    DisplayObject_SetDepthOffset(card, VIEWER_DEPTH);
    D_8009B24C = card;

    for (i = 0; i < VIEWER_TEXT_BOXES; i++, channel++) {
        DuelEffectChannel *box;
        int kind = 3;
        if (channel->flags_34 & DUEL_EFFECT_CHANNEL_FLAG_ACTIVE) continue;
        gDuel_wSelectedCardID = id;
        /* A monster a mod gave no star has the magic cards' layout too (stars.h). */
        if (((gDuel_adwCardStats[(s16)id - 1] >> CARD_STAT_TYPE_SHIFT) & CARD_STAT_TYPE_MASK) >= CARD_TYPE_MAGIC ||
            Stars_NoStarCard(id)) {
            kind = 4;
        }
        box = TextBox_Create(i, kind, VIEWER_TEXT_X, VIEWER_TEXT_Y, VIEWER_TEXT_W, VIEWER_TEXT_H);
        box->field_53 = 1;
        box->field_54 = 0;
        box->field_59 = VIEWER_TEXT_STYLE;
        D_8009B250 = box;
        func_80039A14((struct DuelEffectChannel *)box);
        TextBox_SetPos(box, *(s16 *)&background->field_30.h.field_30, *(s16 *)&background->field_30.h.field_32);
        break;
    }
    D_8009B248 = (u8)((D_8009B248 | VIEWER_WAITING) & ~(VIEWER_SHOWN | VIEWER_CLOSING));
}

/* 1 for Down, -1 for Up on pad `pad` (0 or 1), else 0. */
static int step_on(int pad)
{
    if (gInput_wPad1Repeat[pad] & PAD_DIRECTION_DOWN) return 1;
    if (gInput_wPad1Repeat[pad] & PAD_DIRECTION_UP) return -1;
    return 0;
}

int CardBrowse_Poll(void)
{
    int mode = D_8009B26C & 0x1F, pad, step, at_end = 0;
    size_t i;
    for (i = 0; i < sizeof(screens) / sizeof(screens[0]); i++) {
        if (screens[i].mode != mode) continue;
        if (gInput_wPad1Pressed & PAD_BUTTON_L3_R3_MASK) {
            u16 id = screens[i].vary((gInput_wPad1Pressed & PAD_BUTTON_R3) ? 1 : -1);
            if (id != NOT_THIS_LIST) {
                if (id) {
                    SD_SEPlayFull(SOUND_CURSOR);
                    show(id);
                    return 1;
                }
                return 0;
            }
        }
    }
    if (!Settings_Get(SET_CARD_BROWSE)) return 0;
    for (i = 0; i < sizeof(screens) / sizeof(screens[0]); i++) {
        if (screens[i].mode != mode) continue;
        for (pad = 0; pad < 2; pad++) {
            u16 id;
            if (!(step = step_on(pad))) continue;
            id = screens[i].move(pad, step);
            if (id == NOT_THIS_LIST) continue;
            if (!id) {
                at_end = 1;
                continue;
            }
            SD_SEPlayFull(SOUND_CURSOR);
            show(id);
            return 1;
        }
    }
    /* At an end of the list the viewer came from: the game's "cannot" (the
     * lists themselves are silent there, but the viewer hides them). */
    if (at_end) SD_SEPlayFull(SOUND_CANNOT);
    return 0;
}
