/* Unit tests for the Dev Options code mod (mods/dev-options/dev_options.c) */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../mods/dev-options/dev_options.c"

/* Game global definitions for testing */
DuelSideState D_800E9FF0[DUEL_SIDE_COUNT];
u8 D_8009B1D5 = 0;
u16 gDuel_wSceneStateFlags = 0;
u8 D_8009B174 = 0;
u8 D_800E9F10[256];
DuelHandSlot D_800EA030[HAND_SIZE];
DuelCardRecord D_801A7AD8[64];
u16 gInput_wPad1Pressed = 0;
u8 D_8009B26C[16];
DisplayObject *D_8009B188 = NULL;
DisplayObject *D_8009B18C = NULL;

/* Test mocks and tracking variables */
static int s_sound_played = -1;
static int s_released_count = 0;
static void *s_last_released = NULL;

void DisplayObject_ReleaseIfPresent(void *object)
{
    if (object) {
        s_released_count++;
        s_last_released = object;
    }
}

void SD_SEPlayFull(u32 sound)
{
    s_sound_played = (int)sound;
}

/* Mock host settings */
static int s_setting_unlimited_lp = 1;
static int s_setting_end_turn = 1;
static int s_setting_field = 1;
static uint16_t s_mock_pad = 0;

static int mock_setting(const MemoriesModHost *h, const char *key, int fallback)
{
    (void)h;
    if (!strcmp(key, "unlimited_life_points")) return s_setting_unlimited_lp;
    if (!strcmp(key, "end_turn_without_playing")) return s_setting_end_turn;
    if (!strcmp(key, "field_without_playing")) return s_setting_field;
    return fallback;
}

static unsigned short mock_pad(const MemoriesModHost *h, int port)
{
    (void)h; (void)port;
    return s_mock_pad;
}

static MemoriesModCallback s_damage_callback = NULL;

static int mock_subscribe(const MemoriesModHost *h, unsigned event, int priority, MemoriesModCallback cb)
{
    (void)h; (void)priority;
    if (event == MEMORIES_EVENT_DAMAGE) {
        s_damage_callback = cb;
        return 1;
    }
    return 0;
}

static MemoriesModHost mock_host_struct = {
    .setting = mock_setting,
    .pad = mock_pad,
    .subscribe = mock_subscribe,
    .api = MEMORIES_MOD_API
};

static void reset_duel_state(void)
{
    memset(D_800E9FF0, 0, sizeof(D_800E9FF0));
    memset(D_800EA030, 0, sizeof(D_800EA030));
    memset(D_800E9F10, 0, 0x100);
    memset(D_801A7AD8, 0, sizeof(DuelCardRecord) * 30);
    D_8009B26C[0] = MAIN_MODE_DUEL;
    D_8009B1D5 = 0;
    gDuel_wSceneStateFlags = PHASE_HAND;
    D_8009B174 = 1;
    D_8009B188 = 0;
    D_8009B18C = 0;
    gInput_wPad1Pressed = 0;
    s_mock_pad = 0;
    s_prev_pad = 0;
    s_sound_played = -1;
    s_released_count = 0;
    s_last_released = NULL;

    D_800E9FF0[0].life_points.signed_value = 8000;
    D_800E9FF0[0].max_life_points = 8000;
    D_800E9FF0[1].life_points.signed_value = 8000;
    D_800E9FF0[1].max_life_points = 8000;
}

static void test_unlimited_life_points(void)
{
    reset_duel_state();
    s_setting_unlimited_lp = 1;

    /* 1. At 8000 LP, frame() leaves it at 8000 */
    frame();
    assert(D_800E9FF0[0].life_points.signed_value == 8000);

    /* 2. Drops below 6000 (e.g. 5999) -> restored to 8000 */
    D_800E9FF0[0].life_points.signed_value = 5999;
    frame();
    assert(D_800E9FF0[0].life_points.signed_value == 8000);

    /* 3. Severe drop (e.g. 1000) -> restored to 8000 */
    D_800E9FF0[0].life_points.signed_value = 1000;
    frame();
    assert(D_800E9FF0[0].life_points.signed_value == 8000);

    /* 4. When LP is 0 (game over), frame() does not resurrect */
    D_800E9FF0[0].life_points.signed_value = 0;
    frame();
    assert(D_800E9FF0[0].life_points.signed_value == 0);

    /* 5. With switch disabled, LP remains below 6000 */
    s_setting_unlimited_lp = 0;
    D_800E9FF0[0].life_points.signed_value = 4500;
    frame();
    assert(D_800E9FF0[0].life_points.signed_value == 4500);

    /* 6. Test damage event interception */
    s_setting_unlimited_lp = 1;
    assert(s_damage_callback != NULL);
    {
        /* Small damage (takes 8000 to 7000: >= 6000) -> not handled */
        MemoriesModEvent ev = {
            .type = MEMORIES_EVENT_DAMAGE,
            .phase = MEMORIES_BEFORE,
            .a = 0,         /* player side */
            .b = 1000,      /* damage */
            .result = 8000, /* current life */
            .handled = 0
        };
        s_damage_callback(&ev);
        assert(ev.handled == 0);

        /* Large damage (takes 8000 to 5000: < 6000) -> handled and restored to 8000 */
        ev.b = 3000;
        ev.result = 8000;
        ev.handled = 0;
        s_damage_callback(&ev);
        assert(ev.handled == 1);
        assert(ev.result == 8000);

        /* Fatal damage (takes 8000 to -1000) -> handled and clamped to 8000 */
        ev.b = 9000;
        ev.result = 8000;
        ev.handled = 0;
        s_damage_callback(&ev);
        assert(ev.handled == 1);
        assert(ev.result == 8000);

        /* Opponent damage -> never intercepted */
        ev.a = 1;
        ev.b = 9000;
        ev.result = 8000;
        ev.handled = 0;
        s_damage_callback(&ev);
        assert(ev.handled == 0);
    }

    puts("test_unlimited_life_points: PASSED");
}

static void test_end_turn_without_playing(void)
{
    reset_duel_state();
    s_setting_end_turn = 1;

    /* Setup mock hand objects */
    static DisplayObject dummy_cursor;
    static DisplayObject dummy_hand_obj[5];
    static DisplayObject dummy_child_obj;
    DuelSelectionRecord *side = (DuelSelectionRecord *)(D_800E9F10 + D_8009B1D5 * DUEL_SELECTION_SIDE_SIZE);

    side->cursor_object = &dummy_cursor;
    D_800EA030[0].object = (u8 *)&dummy_hand_obj[0];
    D_800EA030[1].object = (u8 *)&dummy_hand_obj[1];
    D_800EA030[1].child = (u8 *)&dummy_child_obj;
    D_800EA030[1].active_09 = 1;
    side->field_15 = 1;

    /* 1. In hand phase, press Select button (0x0001) */
    s_mock_pad = PAD_BTN_SELECT;
    frame();

    /* Hand cursor and objects released */
    assert(side->cursor_object == NULL);
    assert(D_800EA030[0].object == NULL);
    assert(D_800EA030[1].object == NULL);
    assert(D_800EA030[1].child == NULL);
    assert(D_800EA030[1].active_09 == 0);
    assert(side->field_15 == 0);

    /* End turn chime played */
    assert(s_sound_played == SE_END_TURN);

    /* Scene transitioned to turn switch (phase 10) */
    assert(gDuel_wSceneStateFlags == PHASE_TURN_SWITCH);

    /* 2. Test R1 button (0x0800) also triggers end turn */
    reset_duel_state();
    side->cursor_object = &dummy_cursor;
    s_mock_pad = PAD_BTN_R1;
    frame();
    assert(s_sound_played == SE_END_TURN);
    assert(gDuel_wSceneStateFlags == PHASE_TURN_SWITCH);

    /* 3. With switch disabled, pressing Select does not end turn */
    reset_duel_state();
    s_setting_end_turn = 0;
    side->cursor_object = &dummy_cursor;
    s_mock_pad = PAD_BTN_SELECT;
    frame();
    assert(side->cursor_object == &dummy_cursor);
    assert(gDuel_wSceneStateFlags == PHASE_HAND);

    puts("test_end_turn_without_playing: PASSED");
}

static void test_enter_field_without_playing(void)
{
    reset_duel_state();
    s_setting_field = 1;

    static DisplayObject dummy_cursor;
    DuelSelectionRecord *side = (DuelSelectionRecord *)(D_800E9F10 + D_8009B1D5 * DUEL_SELECTION_SIDE_SIZE);
    side->cursor_object = &dummy_cursor;

    /* Press L1 button (0x0400) */
    s_mock_pad = PAD_BTN_L1;
    frame();

    /* Cursor released */
    assert(side->cursor_object == NULL);

    /* Field transition sound played */
    assert(s_sound_played == SE_FIELD_STEP);

    /* Scene transitioned to field actions (phase 5) */
    assert(gDuel_wSceneStateFlags == PHASE_FIELD);

    /* With switch disabled, L1 does nothing */
    reset_duel_state();
    s_setting_field = 0;
    side->cursor_object = &dummy_cursor;
    s_mock_pad = PAD_BTN_L1;
    frame();
    assert(side->cursor_object == &dummy_cursor);
    assert(gDuel_wSceneStateFlags == PHASE_HAND);

    puts("test_enter_field_without_playing: PASSED");
}

int main(void)
{
    MemoriesMod mod_struct;
    int ok;

    memset(&mod_struct, 0, sizeof(mod_struct));
    ok = MemoriesModInit(&mock_host_struct, &mod_struct);
    assert(ok == 1);
    assert(mod_struct.api == MEMORIES_MOD_API);
    assert(mod_struct.frame != NULL);

    test_unlimited_life_points();
    test_end_turn_without_playing();
    test_enter_field_without_playing();

    puts("\nAll Dev Options mod unit tests passed successfully!");
    return 0;
}
