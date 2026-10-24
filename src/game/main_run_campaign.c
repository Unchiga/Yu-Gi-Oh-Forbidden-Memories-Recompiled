#define GDUEL_BEFFECTSTATE_IN_DATA
#define MAIN_MODE_STATE_NEXT_AS_SCALAR
#define MAIN_MODE_STATE_ACTIVE_AS_SCALAR
#include "../types.h"
#include "scene_script.h"
#include "duel_effect.h"
#include "fade.h"
#include "script_run_tick.h"
#include "script_state.h"
#include "sound.h"
#include "campaign_scene_package.h"
#include "main_modes.h"
#ifdef MEMORIES_PC
#include "fade_constants.h"
#endif
#include "../unmatched.h"
#include "main_mode_state.h"

#ifdef MEMORIES_PC
/* The frames a campaign scene has been with its script stopped, and no fade
 * running: a scene that waits for a text that will never end, or for a
 * script that has ended where the retail's opens the campaign map. */
static unsigned scene_stuck_frames;
#endif

void Main_RunCampaign(void) {
    u8 flags = D_8009B26C;

    if ((flags & 0x40) == 0) {
        D_8009B26C = flags | 0x40;
        Campaign_LoadScenePackage(gCampaignSceneIndex);
        gDuel_bEffectState = 0;
#ifdef MEMORIES_PC
        scene_stuck_frames = 0;
#endif
        return;
    }

    SceneScript_UpdateSlots(D_800EAE98);
    Script_RunTick();
#ifdef MEMORIES_PC
    /* A community image holds its own scene script, and it may stop where
     * the retail's opens the campaign map: the scene would stay up for ever,
     * with the game waiting for it. A scene whose script has stopped for
     * half a minute ends the way the retail's fade-out ends it. The retail's
     * own scenes never sit that long: their script runs, and the fade that
     * follows it is running all the while. */
    if ((D_8009B26C & 0x40) && (D_8009B27C & 0x1F) == 0 &&
        (gFade_State.flags & FADE_FLAG_ACTIVE) == 0) {
        if (++scene_stuck_frames >= 1800) {
            D_8009B26C = MAIN_MODE_CAMPAIGN_MAP;
        }
    } else {
        scene_stuck_frames = 0;
    }
#endif
    if (D_8009B26C & 0x40)
        return;
    SD_BGMFadeOut();
    Fade_WaitOut();
}
