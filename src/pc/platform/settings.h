#ifndef MEMORIES_PC_SETTINGS_H
#define MEMORIES_PC_SETTINGS_H

/* The port's stored settings (settings.txt in the user directory, see
 * paths.h; MEMORIES_SETTINGS names another file). Every key has a default, a
 * range and an environment override MEMORIES_<KEY IN UPPER CASE> (legacy
 * names are also honoured). Settings are never part of save states.
 *
 * Code mods are built against these numbers (Settings_Get(SET_PGXP)), so an
 * id keeps its number from release to release: a new one goes at the end,
 * before SET_COUNT, and one no longer used stays, retired, with no key
 * (Settings_Key is NULL, Settings_Get 0). tools/pc/check_mod_abi.py fails
 * the build otherwise. */
typedef enum {
    SET_MASTER_VOLUME,
    SET_MUSIC_VOLUME,
    SET_SFX_VOLUME,
    SET_STREAM_VOLUME,
    SET_SCALE,
    SET_FULLSCREEN,
    SET_BORDERLESS,
    SET_SCALING,
    SET_ASPECT,
    SET_FILTER,
    SET_VSYNC,
    SET_SPEED,
    SET_FPS,
    SET_SHOW_MENU_FULLSCREEN,
    SET_PAUSE_ON_FOCUS_LOSS,
    SET_MUTE_ON_FOCUS_LOSS,
    SET_HIDE_CURSOR,
    SET_SHOW_HUD,
    SET_MENU_SCALE,
    SET_WINDOW_X,
    SET_WINDOW_Y,
    SET_AUDIO_INTERPOLATION,
    SET_INTERNAL_SCALE,
    SET_RETURN_AFTER_CREDITS,   /* retired: no key, always 0 */
    SET_DECK_SLOTS,
    SET_BRIGHTNESS,
    SET_CONTRAST,
    SET_SATURATION,
    SET_GAMMA,
    SET_CRT,
    SET_FLASH,
    SET_XBR,
    SET_HD_TEXT,
    SET_FUSION_HELPER,
    SET_MSAA,
    SET_HD_HUD,                 /* retired: part of SET_HD_TEXT */
    SET_OPPONENT_NAME,
    SET_PGXP,
    SET_CARD_DROPS,
    /* Everything up to here is where release v0.1.2 had it. */
    SET_FREE_DUEL_PROGRESS,
    SET_UPDATE_CHECK,
    SET_UPDATE_PRERELEASES,
    SET_RANK_METER,
    SET_REWIND,                 /* retired: rewind was removed */
    SET_LIBRARY_ALL_CARDS,
    SET_CHEAT_LIFE_POINTS,
    SET_CHEAT_SHOW_HAND,
    SET_CHEAT_FREE_SPENDING,
    SET_CARD_PASSWORDS,
    SET_JP_BUTTONS,
    SET_CARD_BROWSE,
    SET_LANGUAGE,
    SET_CONFIRM_QUIT,
    SET_TOUCH_PAD,
    SET_CRASH_REPORT_OFFER,
    SET_SCREEN_ROTATION,
    SET_TOUCH_OPACITY,
    SET_COUNT
} SettingId;

void Settings_Load(void);
/* Returns nonzero after settings were successfully written. */
int Settings_Save(void);
/* Why the last save failed, "Could not save settings to <path>: <reason>."
 * (paths.h, Paths_WriteError), or "" once one has succeeded. */
const char *Settings_LastError(void);
/* That text the first time it is asked for after saves began failing, or
 * failing for another reason; NULL otherwise. Whoever tells the player takes
 * it (a notice, menu.c; the Mods window's status, manager.c), so a failure
 * is told once and not on every save a moved window makes. */
const char *Settings_TakeNewError(void);
int Settings_Get(SettingId id);
void Settings_Set(SettingId id, int value);
const char *Settings_Key(SettingId id);
int Settings_Min(SettingId id);
int Settings_Max(SettingId id);
void Settings_Observe(void (*changed)(SettingId id, int value));

/* Keys outside the fixed list, kept in the same file and written back
 * unchanged if nothing claims them: whether a mod is applied (`mod.<id>`)
 * and a mod's own settings (`mod.<id>.<key>`). Unlike the fixed settings
 * these have no range, so a mod validates its own values. */
int Settings_GetNamed(const char *key, int fallback);
void Settings_SetNamed(const char *key, int value);

void Settings_VisitNamed(void (*visit)(const char *, int, void *), void *context);

#endif
