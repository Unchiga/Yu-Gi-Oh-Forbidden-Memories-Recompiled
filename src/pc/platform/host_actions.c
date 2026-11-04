/* The Game list's actions (host_actions.h). Before they were bindable each
 * was a key in both backends' event loops; these are those same steps, now
 * reached through controls_runtime.c, so a binding decides the key. */
#include "host_actions.h"
#include "controls_runtime.h"
#include "platform.h"
#include "quit_prompt.h"
#include "settings.h"
#include "pc/audio/spu.h"
#include "pc/debug/log.h"
#include "pc/guest/state.h"
#include "pc/saves/deck_menu.h"

void HostActions_StepVolume(int up)
{
    int lowest = Settings_Min(SET_MASTER_VOLUME), highest = Settings_Max(SET_MASTER_VOLUME);
    int volume = Settings_Get(SET_MASTER_VOLUME) + (up ? 5 : -5);
    volume = volume < lowest ? lowest : volume > highest ? highest : volume;
    if (volume != Settings_Get(SET_MASTER_VOLUME)) {
        Settings_Set(SET_MASTER_VOLUME, volume); /* the observer applies it (menu.c) */
        Settings_Save();
    }
    LOG(LOG_MENU, "master volume %d%s", volume, Spu_Muted() ? " (muted)" : "");
}

static int (*exit_claim)(void);
void HostActions_SetExitClaim(int (*claim)(void)) { exit_claim = claim; }

static void press(int action, int *quit)
{
    switch (action) {
    case CTRL_HOST_EXIT: /* asks first, as File > Exit, unless a mod's battle takes it */
        if (!exit_claim || !exit_claim()) QuitPrompt_Request(quit);
        break;
    case CTRL_HOST_FULLSCREEN:
        if (Platform_HasWindowModes()) {
            int on = Settings_Get(SET_FULLSCREEN) || Settings_Get(SET_BORDERLESS);
            Settings_Set(SET_FULLSCREEN, !on);
            if (on) Settings_Set(SET_BORDERLESS, 0);
            Settings_Save();
            Platform_ApplyDisplaySettings();
        }
        break;
    /* With Shift held, the whole window, menu included. */
    case CTRL_HOST_SCREENSHOT:
        Platform_Screenshot(ControlsRuntime_KeyDown(CTRL_KEY_LEFT_SHIFT) || ControlsRuntime_KeyDown(CTRL_KEY_RIGHT_SHIFT));
        break;
    case CTRL_HOST_MUTE: Spu_SetMuted(!Spu_Muted()); break;
    case CTRL_HOST_VOLUME_UP: case CTRL_HOST_VOLUME_DOWN: HostActions_StepVolume(action == CTRL_HOST_VOLUME_UP); break;
    case CTRL_HOST_SAVE_STATE: case CTRL_HOST_LOAD_STATE:
        Memories_StateRequest(action == CTRL_HOST_SAVE_STATE ? 1 : 2, Platform_StateSlot());
        break;
    case CTRL_HOST_SLOT_1: case CTRL_HOST_SLOT_2: case CTRL_HOST_SLOT_3: case CTRL_HOST_SLOT_4:
        Platform_SetStateSlot(action - CTRL_HOST_SLOT_1 + 1);
        break;
    case CTRL_HOST_PAUSE: Platform_SetClockRate(Platform_ClockRate() == 0 ? Settings_Get(SET_SPEED) : 0); break;
    case CTRL_HOST_FRAME_STEP:
        if (Platform_ClockRate() == 0) Platform_StepFrame();
        break;
    case CTRL_HOST_HUD:
        Settings_Set(SET_SHOW_HUD, (Settings_Get(SET_SHOW_HUD) + 1) % 3);
        Settings_Save();
        break;
    case CTRL_HOST_DECK_SLOTS: DeckMenu_Request(); break;
    default: break;
    }
}

int HostActions_Run(int *quit)
{
    static uint32_t was;
    uint32_t pressed = ControlsRuntime_TakeHost(), held = ControlsRuntime_HostHeld(), changed = held ^ was;
    was = held;
    /* Turbo runs the game at 400% while held. Letting go while paused (P,
     * or the window losing focus) leaves the pause alone. */
    if (changed >> CTRL_HOST_TURBO & 1 && (held >> CTRL_HOST_TURBO & 1 || Platform_ClockRate() != 0))
        Platform_SetClockRate(held >> CTRL_HOST_TURBO & 1 ? 400 : Settings_Get(SET_SPEED));
    for (int h = 0; h < CTRL_HOST_COUNT; h++)
        if (pressed >> h & 1 && Controls_HostActions[h].mode != CTRL_HOST_HOLD) press(h, quit);
    return pressed != 0;
}
