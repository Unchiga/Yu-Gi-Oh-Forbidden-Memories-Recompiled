/* Dev Options mod (mod.json beside this file): developer cheats and quality-of-life
 * controls for duels:
 *  1. Unlimited Life Points: restores player LP to 8000 whenever it drops below 6000.
 *  2. End Turn Without Playing: allows ending your turn directly during hand phase
 *     by pressing Select or R1 without placing a card onto the field.
 *  3. Enter Field Without Playing: allows proceeding to field phase during hand
 *     phase by pressing L1 to inspect the board or attack without playing a card.
 */
#include "types.h"
#include "game/main_modes.h"
#include "game/duel_side_state.h"
#include "game/duel_selection_layout.h"
#include "game/duel_hand.h"
#include "game/duel_card_layout.h"
#include "game/duel_card.h"
#include "game/display_object.h"
#include "game/sound.h"
#define D_8009B_DISPLAY_OBJECTS_VISIBLE
#include "game/main_mode_state.h"
#include "unmatched.h"
#include "pc/mods/modapi.h"
#include <stdint.h>

extern DuelSideState D_800E9FF0[DUEL_SIDE_COUNT];
extern u8 D_8009B1D5;
extern u16 gDuel_wSceneStateFlags;
extern u8 D_8009B174;
extern u8 D_800E9F10[];
extern DuelHandSlot D_800EA030[HAND_SIZE];
extern DuelCardRecord D_801A7AD8[];
extern u16 gInput_wPad1Pressed;

void DisplayObject_ReleaseIfPresent(void *object);
void SD_SEPlayFull(u32 sound);

#define PAD_BTN_SELECT 0x0001
#define PAD_BTN_L1     0x0400
#define PAD_BTN_R1     0x0800

#define PHASE_HAND        4
#define PHASE_FIELD       5
#define PHASE_TURN_SWITCH 10
#define SE_END_TURN       0x30
#define SE_FIELD_STEP     0x07

static const MemoriesModHost *host;
static uint16_t s_prev_pad = 0;

static void cleanup_hand_actions(void)
{
    DuelSelectionRecord *side = (DuelSelectionRecord *)(D_800E9F10 + D_8009B1D5 * DUEL_SELECTION_SIDE_SIZE);
    int n;
    int base_idx = D_8009B1D5 * DUEL_CARD_SIDE_RECORD_COUNT;

    /* Release hand cursor sprite */
    if (side->cursor_object) {
        DisplayObject_ReleaseIfPresent(side->cursor_object);
        side->cursor_object = 0;
    }

    /* Release hand card display objects and any fusion child sprites */
    for (n = 0; n < HAND_SIZE; n++) {
        DuelHandSlot *slot = &D_800EA030[n];
        if (slot->child) {
            DisplayObject_ReleaseIfPresent(slot->child);
            slot->child = 0;
        }
        if (slot->object) {
            DisplayObject_ReleaseIfPresent(slot->object);
            slot->object = 0;
        }
        slot->active_09 = 0;
        D_801A7AD8[base_idx + n].object = 0;
        D_801A7AD8[base_idx + n].flags = 0;
    }
    side->field_15 = 0;

    /* Release preview sprites if present */
    DisplayObject_ReleaseIfPresent(D_8009B188);
    D_8009B188 = 0;
    DisplayObject_ReleaseIfPresent(D_8009B18C);
    D_8009B18C = 0;
}

static void end_turn_now(void)
{
    cleanup_hand_actions();
    SD_SEPlayFull(SE_END_TURN);
    gDuel_wSceneStateFlags = PHASE_TURN_SWITCH;
}

static void enter_field_now(void)
{
    cleanup_hand_actions();
    SD_SEPlayFull(SE_FIELD_STEP);
    gDuel_wSceneStateFlags = PHASE_FIELD;
}

static void on_damage(MemoriesModEvent *event)
{
    if (!host || !host->setting(host, "unlimited_life_points", 1)) return;
    if (event->phase == MEMORIES_BEFORE && event->a == 0) {
        int damage = event->b < 0 ? 0 : event->b;
        int current_life = event->result;
        if (current_life - damage < 6000) {
            event->handled = 1;
            event->result = 8000;
        }
    }
}

static void frame(void)
{
    uint16_t pad;
    uint16_t pressed;

    if (!host) return;

    pad = host->pad(host, 0);
    pressed = (pad & ~s_prev_pad) | gInput_wPad1Pressed;
    s_prev_pad = pad;

    /* Only run in duel mode */
    if ((D_8009B26C[0] & 0x1F) != MAIN_MODE_DUEL) return;

    /* 1. Unlimited Life Points: keep player LP at 8000 if it drops below 6000 */
    if (host->setting(host, "unlimited_life_points", 1)) {
        if (D_800E9FF0[0].life_points.signed_value > 0 &&
            D_800E9FF0[0].life_points.signed_value < 6000) {
            D_800E9FF0[0].life_points.unsigned_value = 8000;
        }
    }

    /* 2. End turn without playing or enter field without playing during Hand phase */
    if (D_8009B1D5 == 0 && (gDuel_wSceneStateFlags & 0x000F) == PHASE_HAND &&
        (D_8009B174 & 0x0F) == 1 && !(gDuel_wSceneStateFlags & 0x4000)) {

        if (host->setting(host, "end_turn_without_playing", 1) &&
            (pressed & (PAD_BTN_SELECT | PAD_BTN_R1))) {
            end_turn_now();
            return;
        }

        if (host->setting(host, "field_without_playing", 1) &&
            (pressed & PAD_BTN_L1)) {
            enter_field_now();
            return;
        }
    }
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    host = from;
    mod->api = MEMORIES_MOD_API;
    mod->frame = frame;
    host->subscribe(host, MEMORIES_EVENT_DAMAGE, 100, on_damage);
    return 1;
}
