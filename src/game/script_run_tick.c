#define D_800EB12C_IN_DATA
#include "../types.h"
#include "../ygo_types.h"
#include "../psyq/rand.h"
#include "duel_effect.h"
#include "func_80039794.h"
#include "script_op_show_dialog.h"
#include "script_command_table.h"
#include "script_run_tick.h"
#include "script_state.h"
#ifdef MEMORIES_PC
#include "input.h"
#include "pc/text/message_wait.h"
#endif

void Script_RunTick(void)
{
    u16 f;
    s32 g;
    s32 b;

    rand();
    f = D_8009B2A4;
    if (f & DUEL_EVENT_SCRIPT_FLAG_DIALOG_ACTIVE) {
        func_80039794();
        if ((D_800EB12C & TEXT_BOX_COMPLETION_MASK) == TEXT_BOX_FLAG_DONE) {
            D_8009B2A4 &= ~DUEL_EVENT_SCRIPT_FLAG_DIALOG_ACTIVE;
        }
#ifdef MEMORIES_PC
        /* A community image holds its own text, and the story shows the
         * image's text at the retail's id: a text the box can never finish
         * (it has no end of its own) would hold the story here for ever. The
         * message is on screen and the player has asked for it to go on;
         * after a while the story goes on without it. A box whose text has
         * ended (the retail's own text) waits for the player's dismiss, as
         * it always did.
         *
         * Only a box that is printing (no state of its own) is counted: a
         * box that waits for a page or for a pause is waiting for the
         * player's press, as it always did. */
        else if (D_800EB0F8[0].state_51 == 0) {
            if (MessageWait_Frame(MESSAGE_WAIT_STORY,
                                  (gInput_wPad1Pressed & PAD_BUTTON_CONFIRM_MASK) != 0,
                                  D_800EB0F8[0].field_60,
                                  (D_800EB12C & TEXT_BOX_FLAG_DONE) != 0)) {
                D_8009B2A4 &= ~DUEL_EVENT_SCRIPT_FLAG_DIALOG_ACTIVE;
            }
        } else {
            MessageWait_Begin(MESSAGE_WAIT_STORY);
        }
#endif
        return;
    }
    if (f == 0) {
        return;
    }
    if ((f & DUEL_EVENT_SCRIPT_FLAG_STARTED) == 0) {
        g = D_8009B2A4;
        D_8009B290 = D_801A8000 + ((u16 *)D_801A8000)[g];
        b = *D_8009B290++;
        D_8009B28C = 0;
        D_8009B2A4 = g | DUEL_EVENT_SCRIPT_FLAG_STARTED;
        D_8009B27C = b;
    }
    if (D_8009B28C != 0) {
        D_8009B27C = D_8009B28C;
    }
    while (1) {
        D_80090C50[D_8009B27C & 0x1F]();
        if (D_8009B27C != 0) {
            return;
        }
        D_8009B27C = *D_8009B290++;
    }
}
