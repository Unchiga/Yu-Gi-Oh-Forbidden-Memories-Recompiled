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
    /* 1: states kept in memory every few frames, and holding F8 goes back
     * through them (src/pc/guest/state.c, rewind.h). Off by default: it
     * costs memory and a little time per frame. */
    [SET_REWIND] = {"rewind", NULL, "MEMORIES_REWIND", NULL, 0, 0, 1},
    /* Game > Cheats (src/pc/debug/cheats.h): the life points both sides
     * start a duel against the CPU with; 8000 is the console's. */
    [SET_CHEAT_LIFE_POINTS] = {"cheat_life_points", NULL, "MEMORIES_CHEAT_LIFE_POINTS", NULL, 8000, 1, 9999},
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
    /* Percent, the present pass's colour (src/pc/render/present_pass.c); 100 leaves the picture alone. */
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
    /* Samples a pixel of the OpenGL picture is drawn with: 0 (off), 2, 4, 8
     * (gl_picture.c). */
    [SET_MSAA] = {"msaa", NULL, "MEMORIES_MSAA", NULL, 0, 0, 8},
    /* Precise geometry (pgxp.h): 0 off, 1 textures (perspective-correct,
     * drawn at the console's whole-pixel vertices). Video > Precise
     * geometry; drawn by the OpenGL picture pass at Internal 2x and up,
     * gated like the opponent's name (update_hd_items, menu.c). */
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
        const char *text = getenv(info[id].env);
        int value;
        if ((!text || !*text) && info[id].legacy_env) text = getenv(info[id].legacy_env);
        if (text && *text && parse_value(text, &value)) values[id] = clamp((SettingId)id, value);
    }
}

int Settings_Save(void)
{
    const char *path = settings_path();
    char temporary[1024];
    FILE *file;
    int id, i;

    if (named_error) return 0; /* Never report success after dropping a setting. */
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary)) return 0;
    file = fopen(temporary, "w");
    if (!file) return 0;
    for (id = 0; id < SET_COUNT; id++) {
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
        if (failed || rename(temporary, path)) { remove(temporary); return 0; }
    }
    return 1;
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
