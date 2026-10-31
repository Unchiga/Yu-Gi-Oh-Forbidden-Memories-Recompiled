#include "settings.h"
#include "paths.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "pc/compat/posix.h"

#define MAX_UNKNOWN 64
#define MAX_KEY 256
#define MAX_LINE 1024
#define MAX_OBSERVERS 8

typedef struct {
    const char *key;
    const char *legacy_key;
    const char *env;
    const char *legacy_env;
    int def, min, max;
} SettingInfo;

static const SettingInfo info[SET_COUNT] = {
    [SET_FUSION_HELPER] = {"fusion_helper", NULL, "MEMORIES_FUSION_HELPER", NULL, 0, 0, 1},
    /* View > Free Duel progress (src/pc/cards/free_duel_progress.h): owned
     * and obtainable cards of the opponent under the Free Duel cursor. */
    [SET_FREE_DUEL_PROGRESS] = {"free_duel_progress", NULL, "MEMORIES_FREE_DUEL_PROGRESS", NULL, 0, 0, 1},
    /* 1: the opponent's name in place of COM (hd_text.h). */
    [SET_OPPONENT_NAME] = {"opponent_name", NULL, "MEMORIES_OPPONENT_NAME", NULL, 0, 0, 1},
    /* Cards a won duel deals; 1 is the console's (src/pc/cards/drops.h). */
    /* Game > Language (src/pc/text/language.h): 0 English (US), 1 English (Europe), 2 French, 3 German,
     * 4 Italian, 5 Spanish, from the player's PAL disc; at the next launch. */
    [SET_LANGUAGE] = {"language", NULL, "MEMORIES_LANGUAGE", NULL, 0, 0, 5},
    [SET_CARD_DROPS] = {"card_drops", NULL, "MEMORIES_CARD_DROPS", NULL, 1, 1, 99},
    /* 1: the Library lays out every card never seen as a seen one, while it
     * is open; nothing is given or saved (Cards_LibraryPlaceholder, cards.h). */
    [SET_LIBRARY_ALL_CARDS] = {"library_all_cards", NULL, "MEMORIES_LIBRARY_ALL_CARDS", NULL, 0, 0, 1},
    /* Help > updates (update_check.h): look for a newer release at start
     * (on unless the player turns it off: it only tells, never installs)
     * and count pre-releases as newer. */
    [SET_UPDATE_CHECK] = {"check_for_updates", NULL, "MEMORIES_CHECK_FOR_UPDATES", NULL, 1, 0, 1},
    [SET_UPDATE_PRERELEASES] = {"update_prereleases", NULL, "MEMORIES_UPDATE_PRERELEASES", NULL, 0, 0, 1},
    /* View > Duel rank (src/pc/cards/rank_meter.h): 0 off, 1 the rank,
     * 2 the rank and the score. */
    [SET_RANK_METER] = {"rank_meter", NULL, "MEMORIES_RANK_METER", NULL, 0, 0, 2},
    /* Game > Cheats (src/pc/debug/cheats.h): the life points both sides
     * start a duel against the CPU with; 8000 is the console's. Up to the
     * 16 bits a side's LP is kept in, for a mod whose "limits" go past 9999. */
    [SET_CHEAT_LIFE_POINTS] = {"cheat_life_points", NULL, "MEMORIES_CHEAT_LIFE_POINTS", NULL, 8000, 1, 32767},
    /* 1: the CPU's hand drawn face up, as the player's is. */
    [SET_CHEAT_SHOW_HAND] = {"cheat_show_hand", NULL, "MEMORIES_CHEAT_SHOW_HAND", NULL, 0, 0, 1},
    /* 1: the Password screen's purchases leave the StarChips alone. */
    [SET_CHEAT_FREE_SPENDING] = {"cheat_free_spending", NULL, "MEMORIES_CHEAT_FREE_SPENDING", NULL, 0, 0, 1},
    /* View > Card passwords: the card's eight-digit password in the card
     * viewer (src/pc/cards/passwords.h). */
    [SET_CARD_PASSWORDS] = {"card_passwords", NULL, "MEMORIES_CARD_PASSWORDS", NULL, 0, 0, 1},
    /* 1: the Japanese release's buttons, Circle confirms and Cross cancels
     * (src/pc/platform/button_layout.h). */
    [SET_JP_BUTTONS] = {"jp_buttons", NULL, "MEMORIES_JP_BUTTONS", NULL, 0, 0, 1},
    /* 1: Up and Down in the card viewer show the list's next card (src/pc/cards/card_browse.h). */
    [SET_CARD_BROWSE] = {"card_browse", NULL, "MEMORIES_CARD_BROWSE", NULL, 0, 0, 1},
    /* 1: every way out (Esc, File > Exit, closing the window) asks first
     * (quit_prompt.c). */
    [SET_CONFIRM_QUIT] = {"confirm_quit", NULL, "MEMORIES_CONFIRM_QUIT", NULL, 1, 0, 1},
    /* View > Touch controls: 0 shown once the screen is touched (hidden
     * again by a key or a controller), 1 always, 2 never (touch_pad.h). */
    [SET_TOUCH_PAD] = {"touch_controls", NULL, "MEMORIES_TOUCH_CONTROLS", NULL, 0, 0, 2},
    /* Android, Help > Offer crash reports at start: the last crash's report
     * offered to share or save at the next launch (android_report.c); the
     * offer's "Don't ask again" sets 0. Nothing reads it elsewhere. */
    [SET_CRASH_REPORT_OFFER] = {"offer_crash_reports", NULL, "MEMORIES_OFFER_CRASH_REPORTS", NULL, 1, 0, 1},
    /* Android, Video > Screen rotation: 0 the picture turns over with the
     * phone whether auto-rotate is on or not, 1 it follows the system's
     * auto-rotate (android.c, Android_ApplyScreenRotation). */
    [SET_SCREEN_ROTATION] = {"screen_rotation", NULL, "MEMORIES_SCREEN_ROTATION", NULL, 0, 0, 1},
    /* View > Touch controls > Opacity: how much of a touch control a button
     * let go covers, in percent; held, it is twice that (touch_pad_art.c).
     * 0, the default, is the style's own: 59% with the game's buttons (as
     * they always were), 25% Simple. */
    [SET_TOUCH_OPACITY] = {"touch_opacity", NULL, "MEMORIES_TOUCH_OPACITY", NULL, 0, 0, 100},
    /* View > Touch controls: 0 the game's buttons, where they always were;
     * 1 Simple, a handheld's layout in grey with MENU, STATES and SPEED along
     * the top (touch_pad.h). */
    [SET_TOUCH_STYLE] = {"touch_style", NULL, "MEMORIES_TOUCH_STYLE", NULL, 0, 0, 1},
    [SET_MASTER_VOLUME] = {"master_volume", "volume", "MEMORIES_MASTER_VOLUME", "MEMORIES_VOLUME", 100, 0, 100},
    [SET_MUSIC_VOLUME] = {"music_volume", NULL, "MEMORIES_MUSIC_VOLUME", NULL, 100, 0, 100},
    [SET_SFX_VOLUME] = {"sfx_volume", NULL, "MEMORIES_SFX_VOLUME", NULL, 100, 0, 100},
    [SET_STREAM_VOLUME] = {"stream_volume", NULL, "MEMORIES_STREAM_VOLUME", NULL, 100, 0, 100},
    [SET_SCALE] = {"scale", "scale", "MEMORIES_SCALE", "MEMORIES_SCALE", 4, 1, 8},
    [SET_FULLSCREEN] = {"fullscreen", NULL, "MEMORIES_FULLSCREEN", NULL, 0, 0, 2},
    [SET_BORDERLESS] = {"borderless", NULL, "MEMORIES_BORDERLESS", NULL, 0, 0, 1},
    [SET_SCALING] = {"scaling", NULL, "MEMORIES_SCALING", NULL, 0, 0, 2},
    /* 0 is corrected 4:3, 1 uses source pixels, and 2 widens the view to 16:9. */
    [SET_ASPECT] = {"aspect", NULL, "MEMORIES_ASPECT", NULL, 0, 0, 2},
    /* 0 nearest, 1 bilinear, 2 sharp bilinear (present_pass.c). */
    [SET_FILTER] = {"filter", NULL, "MEMORIES_FILTER", NULL, 0, 0, 2},
    [SET_VSYNC] = {"vsync", NULL, "MEMORIES_VSYNC", NULL, 0, 0, 1},
    [SET_SPEED] = {"speed", NULL, "MEMORIES_SPEED", NULL, 100, -1, 400},
    /* Presented frames per second: 0 follows the display's refresh rate, -1 shows every game frame. */
    [SET_FPS] = {"fps", NULL, "MEMORIES_FPS", NULL, 0, -1, 1000},
    [SET_SHOW_MENU_FULLSCREEN] = {"show_menu_fullscreen", NULL, "MEMORIES_SHOW_MENU_FULLSCREEN", NULL, 0, 0, 1},
    [SET_PAUSE_ON_FOCUS_LOSS] = {"pause_on_focus_loss", NULL, "MEMORIES_PAUSE_ON_FOCUS_LOSS", NULL, 0, 0, 1},
    [SET_MUTE_ON_FOCUS_LOSS] = {"mute_on_focus_loss", NULL, "MEMORIES_MUTE_ON_FOCUS_LOSS", NULL, 0, 0, 1},
    [SET_HIDE_CURSOR] = {"hide_cursor", NULL, "MEMORIES_HIDE_CURSOR", NULL, 1, 0, 1},
    [SET_SHOW_HUD] = {"show_hud", NULL, "MEMORIES_SHOW_HUD", NULL, 0, 0, 2},
    /* Menu and HUD size: 0 follows the window's height, else a multiple. */
    [SET_MENU_SCALE] = {"menu_scale", NULL, "MEMORIES_MENU_SCALE", NULL, 0, 0, 4},
    [SET_WINDOW_X] = {"window_x", NULL, "MEMORIES_WINDOW_X", NULL, -1, -16384, 16384},
    [SET_WINDOW_Y] = {"window_y", NULL, "MEMORIES_WINDOW_Y", NULL, -1, -16384, 16384},
    /* 0 the console's Gaussian filter, 1 a sharper cubic (SpuInterpolation). */
    [SET_AUDIO_INTERPOLATION] = {"audio_interpolation", NULL, "MEMORIES_AUDIO_INTERPOLATION", NULL, 0, 0, 1},
    /* Pixels drawn per VRAM word each way (soft_gpu.h, SoftGpu_SetScale): 1 is the console's. */
    [SET_INTERNAL_SCALE] = {"internal_scale", NULL, "MEMORIES_INTERNAL_SCALE", NULL, 1, 1, 8},
    /* 1: Game > Deck slots and F6 keep and switch decks (src/pc/saves/deck_menu.c). */
    [SET_DECK_SLOTS] = {"deck_slots", NULL, "MEMORIES_DECK_SLOTS", NULL, 1, 0, 1},
    /* Percent, the present pass's color (src/pc/render/present_pass.c); 100 leaves the picture alone. */
    [SET_BRIGHTNESS] = {"brightness", NULL, "MEMORIES_BRIGHTNESS", NULL, 100, 50, 150},
    [SET_CONTRAST] = {"contrast", NULL, "MEMORIES_CONTRAST", NULL, 100, 50, 150},
    [SET_SATURATION] = {"saturation", NULL, "MEMORIES_SATURATION", NULL, 100, 0, 200},
    [SET_GAMMA] = {"gamma", NULL, "MEMORIES_GAMMA", NULL, 100, 50, 200},
    /* 1: scanlines and an aperture grille on the picture (present_pass.c). */
    [SET_CRT] = {"crt", NULL, "MEMORIES_CRT", NULL, 0, 0, 1},
    /* 1: the picture's average brightness rises no faster than a set rate
     * (present_pass.c). */
    [SET_FLASH] = {"reduce_flashes", NULL, "MEMORIES_REDUCE_FLASHES", NULL, 0, 0, 1},
    /* 1: xBR smoothing of the picture's pixel art (present_pass.c). */
    [SET_XBR] = {"xbr", NULL, "MEMORIES_XBR", NULL, 0, 0, 1},
    /* 1: text, and the duel's numbers and labels, set in a font at the
     * internal resolution (src/pc/text/hd_text.h). */
    [SET_HD_TEXT] = {"hd_text", NULL, "MEMORIES_HD_TEXT", NULL, 0, 0, 1},
    /* Samples a pixel of the OpenGL picture is drawn with: 0 (off), 2, 4, 8
     * (gl_picture.c). */
    [SET_MSAA] = {"msaa", NULL, "MEMORIES_MSAA", NULL, 0, 0, 8},
    /* 0 Off, 1 Textures (fixes affine texture warping; libgpu.c, pgxp.h). */
    [SET_PGXP] = {"pgxp", NULL, "MEMORIES_PGXP", NULL, 0, 0, 1},
};

/* A key the fixed list does not know: a mod's, or one this build dropped.
 * Whole numbers are kept as values so a mod can read and write them; the
 * rest of a settings file is carried through as the lines it arrived as. */
typedef struct { char key[MAX_KEY]; int value; } Named;

static int values[SET_COUNT];
static int stored[SET_COUNT];
static char unknown[MAX_UNKNOWN][MAX_LINE];
static int unknown_count;
static Named *named;
static int named_count, named_capacity, named_error;
static void (*observers[MAX_OBSERVERS])(SettingId, int);
static int observer_count;
/* Why the last save failed ("" after one succeeded), and whether that is
 * news: a first failure, or another reason (Settings_TakeNewError). */
static char last_error[1400];
static int error_is_new;

static const char *settings_path(void)
{
    static char path[1024];
    const char *named_path = getenv("MEMORIES_SETTINGS");
    if (named_path && *named_path) return named_path;
    if (!path[0] && Paths_User(path, sizeof(path), "settings.txt")) return "settings.txt";
    return path;
}

static int clamp(SettingId id, int value)
{
    if (id == SET_SPEED && value != -1 && value < 25) {
        return 25;
    }
    if (value < info[id].min) return info[id].min;
    if (value > info[id].max) return info[id].max;
    return value;
}

static int find_key(const char *key)
{
    int id;
    for (id = 0; id < SET_COUNT; id++) {
        if (!info[id].key) continue;   /* retired (settings.h) */
        if (!strcmp(key, info[id].key) ||
            (info[id].legacy_key && !strcmp(key, info[id].legacy_key))) {
            return id;
        }
    }
    return -1;
}

static int parse_value(const char *text, int *value)
{
    char *end;
    long parsed;
    errno = 0;
    parsed = strtol(text, &end, 10);
    while (isspace((unsigned char)*end)) end++;
    if (errno || end == text || *end || parsed < INT_MIN || parsed > INT_MAX) return 0;
    *value = (int)parsed;
    return 1;
}

/* The named entry for a key, made if it is missing and there is room. */
static Named *find_named(const char *key, int make)
{
    int i;
    for (i = 0; i < named_count; i++) {
        if (!strcmp(named[i].key, key)) return &named[i];
    }
    if (!make) return NULL;
    if (strlen(key) >= MAX_KEY) { named_error = 1; return NULL; }
    /* A name must read back as itself: one line, split at the first '='. */
    if (!*key || strpbrk(key, "=\r\n") || isspace((unsigned char)key[0]) || isspace((unsigned char)key[strlen(key) - 1]))
        return NULL;
    if (named_count == named_capacity) {
        int capacity = named_capacity ? named_capacity * 2 : 128;
        Named *grown = realloc(named, (size_t)capacity * sizeof(*named));
        if (!grown) { named_error = 1; return NULL; }
        named = grown;
        named_capacity = capacity;
    }
    snprintf(named[named_count].key, MAX_KEY, "%s", key);
    named[named_count].value = 0;
    return &named[named_count++];
}

static void set_named(const char *key, int value)
{
    Named *entry = find_named(key, 1);
    if (entry) entry->value = value;
}

void Settings_Load(void)
{
    FILE *file;
    char line[MAX_LINE];
    int id;
    unknown_count = named_count = named_error = 0;
    for (id = 0; id < SET_COUNT; id++) values[id] = stored[id] = info[id].def;
    file = fopen(settings_path(), "r");
    if (file) {
        while (fgets(line, sizeof(line), file)) {
            char copy[MAX_LINE], *equals, *key, *end;
            int value;
            memcpy(copy, line, sizeof(copy));
            copy[sizeof(copy) - 1] = '\0';
            equals = strchr(copy, '=');
            if (equals) {
                *equals++ = '\0';
                key = copy;
                while (isspace((unsigned char)*key)) key++;
                end = key + strlen(key);
                while (end > key && isspace((unsigned char)end[-1])) *--end = '\0';
                id = find_key(key);
                if (id >= 0 && parse_value(equals, &value)) {
                    values[id] = stored[id] = clamp((SettingId)id, value);
                    continue;
                }
                if (id < 0 && *key && parse_value(equals, &value) && strlen(key) < MAX_KEY) {
                    set_named(key, value);
                    continue;
                }
            }
            if (unknown_count < MAX_UNKNOWN) {
                size_t length = strlen(line);
                if (length && line[length - 1] == '\n') line[length - 1] = '\0';
                snprintf(unknown[unknown_count++], MAX_LINE, "%s", line);
            }
        }
        fclose(file);
    }
    for (id = 0; id < SET_COUNT; id++) {
        const char *text = info[id].env ? getenv(info[id].env) : NULL;
        int value;
        if ((!text || !*text) && info[id].legacy_env) text = getenv(info[id].legacy_env);
        if (text && *text && parse_value(text, &value)) values[id] = clamp((SettingId)id, value);
    }
}

/* Keeps why a save failed, and says it on stderr (the crash reports' console
 * lines) when it is news -- not again on every save a moved window makes. */
static int save_failed(const char *text)
{
    if (strcmp(text, last_error)) {
        snprintf(last_error, sizeof(last_error), "%s", text);
        error_is_new = 1;
        fprintf(stderr, "memories-pc: %s\n", last_error);
    }
    return 0;
}

static int save_failed_writing(const char *path)
{
    char why[1200], text[sizeof(last_error)];
    Paths_WriteError(why, sizeof(why), path);
    snprintf(text, sizeof(text), "Could not save settings to %s", why);
    return save_failed(text);
}

int Settings_Save(void)
{
    const char *path = settings_path();
    char temporary[1024];
    FILE *file;
    int id, i;

    /* Never report success after dropping a setting. */
    if (named_error) return save_failed("Could not save settings: a mod's setting could not be kept.");
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary))
        return save_failed("Could not save settings: the settings file's path is too long.");
    Paths_WriteBegin();
    file = fopen(temporary, "w");
    if (!file) return save_failed_writing(path);
    for (id = 0; id < SET_COUNT; id++) {
        if (!info[id].key) continue;
        fprintf(file, "%s=%d\n", info[id].key, stored[id]);
        /* TODO remove legacy keys after one compatibility release. */
        if (info[id].legacy_key && strcmp(info[id].legacy_key, info[id].key)) {
            fprintf(file, "%s=%d\n", info[id].legacy_key, stored[id]);
        }
    }
    for (i = 0; i < named_count; i++) fprintf(file, "%s=%d\n", named[i].key, named[i].value);
    for (i = 0; i < unknown_count; i++) fprintf(file, "%s\n", unknown[i]);
    {
        int failed = ferror(file);
        if (fclose(file)) failed = 1;
        if (failed || rename(temporary, path)) {
            save_failed_writing(path); /* before remove() changes the reason */
            remove(temporary);
            return 0;
        }
    }
    last_error[0] = '\0';
    error_is_new = 0;
    Paths_WriteDone(path);
    return 1;
}

const char *Settings_LastError(void) { return last_error; }

const char *Settings_TakeNewError(void)
{
    if (!error_is_new || !last_error[0]) return NULL;
    error_is_new = 0;
    return last_error;
}

int Settings_Get(SettingId id)
{
    return id >= 0 && id < SET_COUNT ? values[id] : 0;
}

void Settings_Set(SettingId id, int value)
{
    int i;
    if (id < 0 || id >= SET_COUNT) return;
    value = clamp(id, value);
    stored[id] = value;
    if (values[id] == value) return;
    values[id] = value;
    for (i = 0; i < observer_count; i++) observers[i](id, value);
}

const char *Settings_Key(SettingId id) { return id >= 0 && id < SET_COUNT ? info[id].key : NULL; }
int Settings_Min(SettingId id) { return id >= 0 && id < SET_COUNT ? info[id].min : 0; }
int Settings_Max(SettingId id) { return id >= 0 && id < SET_COUNT ? info[id].max : 0; }

int Settings_GetNamed(const char *key, int fallback)
{
    Named *entry = key ? find_named(key, 0) : NULL;
    return entry ? entry->value : fallback;
}

void Settings_SetNamed(const char *key, int value)
{
    if (key && *key) set_named(key, value);
}

void Settings_Observe(void (*changed)(SettingId id, int value))
{
    int i;
    if (!changed) return;
    for (i = 0; i < observer_count; i++) if (observers[i] == changed) return;
    if (observer_count < MAX_OBSERVERS) observers[observer_count++] = changed;
}

void Settings_VisitNamed(void (*visit)(const char *, int, void *), void *context)
{
    int i;
    for (i = 0; i < named_count; i++) visit(named[i].key, named[i].value, context);
}
