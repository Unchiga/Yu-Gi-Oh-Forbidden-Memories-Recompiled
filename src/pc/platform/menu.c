/* The window's menu bar, composited in software (see menu.h).
 *
 * Nothing here touches the X server: Menu_Draw paints into the frame the
 * platform is about to show, and Menu_Bounds tells the platform which part
 * of that frame the menu covers so that a hover or a slider drag repaints
 * and uploads only that rectangle, not a whole 1280x960 frame. The bar is
 * repainted with every game frame, which is about a thousand pixels of
 * text over a flat fill and costs nothing measurable.
 *
 * Text is anti-aliased through FreeType, in whatever face fontconfig names
 * for "sans-serif" at 13 px, cached as coverage bitmaps: printable ASCII
 * once at start, any other character (text is UTF-8) the first time it is
 * drawn or measured, "?" where the face has none. A machine without either
 * falls back to the 5x7 bitmap font at the end of the file, drawn at twice
 * its size. */
#include "menu.h"
#include "platform.h"
#include "settings.h"
#include "pc/audio/spu.h"
#include "pc/audio/replace.h"
#include "pc/debug/cheats.h"
#include "pc/tower/tower.h"
#include "pc/debug/log.h"
#include "pc/guest/state.h"
#include "pc/mods/mods.h"
#include "pc/cards/tables.h"
#include "pc/render/texture_pack.h"
#include "paths.h"
#include "pc/sdk/display.h"
#include "title_jump.h"
#include "quit_prompt.h"
#include "button_layout.h"
#include "controls_runtime.h"
#include "update_check.h"
#include "pc/debug/monitor.h"
#include "pc/saves/deck_menu.h"
#include "pc/text/language.h"
#ifdef _WIN32
#include "win32.h"
#else
#ifdef __APPLE__
#include "macos_fonts.h"
#else
#include <fontconfig/fontconfig.h>
#endif
#endif
#include "pc/debug/crash.h"
#include "pc/text/glyphs.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include "pc/compat/font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Geometry, in multiples of `ui` (Menu_SetScale): 1 suits a 720p window,
 * a 4K display wants 3. On a touch screen the bar and the rows are at least
 * a finger's target tall (Menu_SetTouchTarget), whatever `ui` is. */
static int ui = 1, touch_row;
#define MENU_H (26 * ui > touch_row ? 26 * ui : touch_row)
#define ITEM_H (26 * ui > touch_row ? 26 * ui : touch_row)
#define BAR_PAD (12 * ui)    /* left and right of a bar label */
#define ITEM_PAD (12 * ui)   /* dropdown edge to the mark column */
#define MARK_W (22 * ui)     /* the check or radio column */
#define SHORTCUT_GAP (28 * ui)
#define DROP_PAD (4 * ui)    /* dropdown border to the first item */
#define SEP_H (9 * ui)
#define SLIDER_W (160 * ui)
#define SLIDER_H (4 * ui)
#define KNOB_R (6 * ui)
#define SHADOW (6 * ui)
#define FONT_PX (13 * ui)

/* Colors: a dark bar that stays out of the picture's way. */
#define C_BAR 0x1e1f22u
#define C_BAR_EDGE 0x0b0b0dU
#define C_BAR_HOVER 0x33353aU
#define C_ACCENT 0x3b82f6u
#define C_TEXT 0xe8e8ea
#define C_TEXT_DIM 0x8b8d93
#define C_TEXT_ON_ACCENT 0xffffffu
#define C_DROP 0x27282cU
#define C_DROP_EDGE 0x3f4147u
#define C_SEP 0x3f4147u
#define C_TRACK 0x4c4e55u
#define C_KNOB 0xf2f2f4u
#define C_KNOB_EDGE 0x1e1f22u
#define C_MARK 0x9a9ca3u

/* ITEM_SUBMENU opens submenus[value] beside its row; one level deep. An
 * ITEM_SLIDER's value is how far a key or wheel step moves it (0: 5). */
typedef enum { ITEM_ACTION, ITEM_CHECK, ITEM_RADIO, ITEM_SLIDER, ITEM_SEPARATOR, ITEM_SUBMENU } ItemKind;
enum { ITEM_DISABLED = 1, ITEM_GROUP_BREAK = 2 };

enum {
    ACT_SAVE_STATE = 1, ACT_LOAD_STATE, ACT_SCREENSHOT, ACT_EXIT, ACT_GIVE_CARDS,
    ACT_MODS, ACT_CONTROLS, ACT_RELOAD_SETTINGS, ACT_PAUSE, ACT_FRAME_STEP, ACT_DUMP_FRAME, ACT_DUMP_VRAM,
    SLIDER_MASTER, SLIDER_MUSIC, SLIDER_SFX, CHECK_MUTE,
    CHECK_HUD, CHECK_HUD_FULL, RADIO_STATE_SLOT, ACT_UNLOCK_FREE_DUELISTS, ACT_RESET_COLOR,
    ACT_CHECK_UPDATES, ACT_RELEASES, ACT_VERSION, ACT_SET_STARCHIPS, RADIO_LANGUAGE, ACT_SYSTEM_INFO, ACT_JUMP,
    CHECK_TRACE = 300  /* value is a LogChannel */
};

typedef struct {
    const char *label;
    const char *shortcut;
    ItemKind kind;
    int id;
    SettingId setting;
    int value;
    int flags;
} Item;
typedef struct { const char *label; Item items[20]; int count; int x, w; } Menu;

enum { MENU_FILE, MENU_VIDEO, MENU_AUDIO, MENU_GAME, MENU_VIEW, MENU_DEBUG, MENU_HELP, MENU_COUNT };
#ifdef __ANDROID__
#define HELP_ITEMS 7 /* with Offer crash reports at start */
#else
#define HELP_ITEMS 6
#endif
enum { SUB_SCALE, SUB_MENU_SIZE, SUB_SPEED, SUB_FPS, SUB_CHEATS, SUB_TRACE, SUB_SCALING, SUB_ASPECT, SUB_RESOLUTION, SUB_COLOR, SUB_EFFECTS, SUB_JUMP, SUB_ANTIALIAS, SUB_FILTER, SUB_RANK, SUB_LANGUAGE, SUB_PGXP, SUB_TOUCH, SUB_ROTATION, SUB_COUNT };
#ifdef __ANDROID__
#define VIDEO_ITEMS 15 /* with Screen rotation */
#else
#define VIDEO_ITEMS 14
#endif
static Menu menus[MENU_COUNT] = {
    {"File", {{"Save state", "F5", ITEM_ACTION, ACT_SAVE_STATE, -1},
              {"Load state", "F7", ITEM_ACTION, ACT_LOAD_STATE, -1},
              {"State slot 1", 0, ITEM_RADIO, RADIO_STATE_SLOT, -1, 1, ITEM_GROUP_BREAK},
              {"State slot 2", 0, ITEM_RADIO, RADIO_STATE_SLOT, -1, 2},
              {"State slot 3", 0, ITEM_RADIO, RADIO_STATE_SLOT, -1, 3},
              {"State slot 4", 0, ITEM_RADIO, RADIO_STATE_SLOT, -1, 4},
              {"Screenshot", "F12", ITEM_ACTION, ACT_SCREENSHOT, -1, 0, ITEM_GROUP_BREAK},
              {"Reload settings", 0, ITEM_ACTION, ACT_RELOAD_SETTINGS, -1},
              {"Confirm before quitting", 0, ITEM_CHECK, 0, SET_CONFIRM_QUIT, 0, ITEM_GROUP_BREAK},
              {"Exit", "Esc", ITEM_ACTION, ACT_EXIT, -1}}, 10},
    {"Video", {{"Window scale", 0, ITEM_SUBMENU, 0, -1, SUB_SCALE},
              {"Menu size", 0, ITEM_SUBMENU, 0, -1, SUB_MENU_SIZE},
              {"Fullscreen", "F11", ITEM_CHECK, MENU_ITEM_FULLSCREEN, SET_FULLSCREEN, 0, ITEM_GROUP_BREAK},
              {"Borderless fullscreen", 0, ITEM_CHECK, MENU_ITEM_BORDERLESS, SET_BORDERLESS},
#ifdef __ANDROID__
              {"Screen rotation", 0, ITEM_SUBMENU, 0, -1, SUB_ROTATION},
#endif
              {"Scaling", 0, ITEM_SUBMENU, 0, -1, SUB_SCALING, ITEM_GROUP_BREAK},
              {"Aspect Ratio", 0, ITEM_SUBMENU, 0, -1, SUB_ASPECT},
              {"Resolution", 0, ITEM_SUBMENU, 0, -1, SUB_RESOLUTION},
              {"HD text", 0, ITEM_CHECK, MENU_ITEM_HD_TEXT, SET_HD_TEXT},
              {"Anti-aliasing", 0, ITEM_SUBMENU, 0, -1, SUB_ANTIALIAS},
              {"Filtering", 0, ITEM_SUBMENU, MENU_ITEM_FILTER, -1, SUB_FILTER, ITEM_GROUP_BREAK},
              {"VSync", 0, ITEM_CHECK, MENU_ITEM_VSYNC, SET_VSYNC},
              {"Color", 0, ITEM_SUBMENU, MENU_ITEM_COLOR, -1, SUB_COLOR, ITEM_GROUP_BREAK},
              {"Effects", 0, ITEM_SUBMENU, 0, -1, SUB_EFFECTS},
              {"Precise geometry", 0, ITEM_SUBMENU, MENU_ITEM_PGXP, -1, SUB_PGXP, ITEM_GROUP_BREAK}}, VIDEO_ITEMS},
    {"Audio", {{"Master", 0, ITEM_SLIDER, SLIDER_MASTER, SET_MASTER_VOLUME},
               {"Music", 0, ITEM_SLIDER, SLIDER_MUSIC, SET_MUSIC_VOLUME},
               {"Sound FX", 0, ITEM_SLIDER, SLIDER_SFX, SET_SFX_VOLUME},
               {0, 0, ITEM_SEPARATOR, 0, -1},
               {"Mute all", "M", ITEM_CHECK, CHECK_MUTE, -1},
               {"Mute on focus loss", 0, ITEM_CHECK, 0, SET_MUTE_ON_FOCUS_LOSS},
               {"Console sound (Gaussian)", 0, ITEM_RADIO, 0, SET_AUDIO_INTERPOLATION, 0, ITEM_GROUP_BREAK},
               {"Sharper sound (cubic)", 0, ITEM_RADIO, 0, SET_AUDIO_INTERPOLATION, 1}}, 8},
    /* Game speed scales the game clock (music keeps its tempo); the frame
     * rate is how many of those game frames reach the window. Tab holds 400%. */
    {"Game", {{"Controls...", 0, ITEM_ACTION, ACT_CONTROLS, -1},
              {"Mods", 0, ITEM_ACTION, ACT_MODS, -1},
              {"Game speed", 0, ITEM_SUBMENU, 0, -1, SUB_SPEED},
              {"Frame rate", 0, ITEM_SUBMENU, 0, -1, SUB_FPS},
              {"Card drops", 0, ITEM_SLIDER, 0, SET_CARD_DROPS, 1, ITEM_GROUP_BREAK},
              {"Deck slots...", "F6", ITEM_ACTION, MENU_ITEM_DECKS, -1, 0, ITEM_GROUP_BREAK | ITEM_DISABLED},
              {"Use deck slots", 0, ITEM_CHECK, 0, SET_DECK_SLOTS},
              {"Browse cards with Up/Down", 0, ITEM_CHECK, 0, SET_CARD_BROWSE, 0, ITEM_GROUP_BREAK},
              {"Cheats", 0, ITEM_SUBMENU, 0, -1, SUB_CHEATS, ITEM_GROUP_BREAK},
              {"Language", 0, ITEM_SUBMENU, 0, -1, SUB_LANGUAGE},
              {"Restart game...", 0, ITEM_ACTION, MENU_ITEM_RESTART, -1, 0, ITEM_GROUP_BREAK | ITEM_DISABLED}}, 11},
    {"View", {{"Fusion helper", 0, ITEM_CHECK, 0, SET_FUSION_HELPER},
              {"Card passwords", 0, ITEM_CHECK, 0, SET_CARD_PASSWORDS},
              {"Library: show every card", 0, ITEM_CHECK, 0, SET_LIBRARY_ALL_CARDS},
              {"Free Duel progress", 0, ITEM_CHECK, 0, SET_FREE_DUEL_PROGRESS},
              {"Duel rank", 0, ITEM_SUBMENU, 0, -1, SUB_RANK},
              {"Opponent's name for COM", 0, ITEM_CHECK, MENU_ITEM_OPPONENT_NAME, SET_OPPONENT_NAME, 0, ITEM_GROUP_BREAK},
              {"Japanese buttons (Circle confirms)", 0, ITEM_CHECK, 0, SET_JP_BUTTONS},
              {"Touch controls", 0, ITEM_SUBMENU, 0, -1, SUB_TOUCH}}, 8},
    {"Debug", {{"Jump to", 0, ITEM_SUBMENU, 0, -1, SUB_JUMP},
               {"Show HUD", "F3", ITEM_CHECK, CHECK_HUD, -1, 0, ITEM_GROUP_BREAK},
               {"Full stats", 0, ITEM_CHECK, CHECK_HUD_FULL, -1},
               {"Pause", "P", ITEM_CHECK, ACT_PAUSE, -1, 0, ITEM_GROUP_BREAK},
               {"Frame step", ".", ITEM_ACTION, ACT_FRAME_STEP, -1},
               {"Dump frame (PPM)", 0, ITEM_ACTION, ACT_DUMP_FRAME, -1, 0, ITEM_GROUP_BREAK},
               {"Dump VRAM (PPM)", 0, ITEM_ACTION, ACT_DUMP_VRAM, -1},
               {"Trace", 0, ITEM_SUBMENU, 0, -1, SUB_TRACE, ITEM_GROUP_BREAK}}, 8},
    /* Update checks against the project's GitHub releases (update_check.h).
     * On Android, whether the last crash's report is offered at start
     * (android_report.c), the way back from its "Don't ask again". The last
     * row's label is this build's version, set by Menu_Init. */
    {"Help", {{"Check for updates at start", 0, ITEM_CHECK, 0, SET_UPDATE_CHECK},
              {"Include pre-releases", 0, ITEM_CHECK, 0, SET_UPDATE_PRERELEASES},
              {"Check for updates now", 0, ITEM_ACTION, ACT_CHECK_UPDATES, -1, 0, ITEM_GROUP_BREAK},
              {"Releases page", 0, ITEM_ACTION, ACT_RELEASES, -1},
              {"System info for bug reports...", 0, ITEM_ACTION, ACT_SYSTEM_INFO, -1, 0, ITEM_GROUP_BREAK},
#ifdef __ANDROID__
              {"Offer crash reports at start", 0, ITEM_CHECK, 0, SET_CRASH_REPORT_OFFER},
#endif
              {"Version", 0, ITEM_ACTION, ACT_VERSION, -1, 0, ITEM_GROUP_BREAK | ITEM_DISABLED}}, HELP_ITEMS},
};
static Menu submenus[SUB_COUNT] = {
    {"Window scale", {{"1x", 0, ITEM_RADIO, MENU_ITEM_SCALE_1, SET_SCALE, 1},
                      {"2x", 0, ITEM_RADIO, MENU_ITEM_SCALE_2, SET_SCALE, 2},
                      {"3x", 0, ITEM_RADIO, MENU_ITEM_SCALE_3, SET_SCALE, 3},
                      {"4x", 0, ITEM_RADIO, MENU_ITEM_SCALE_4, SET_SCALE, 4},
                      {"5x", 0, ITEM_RADIO, MENU_ITEM_SCALE_5, SET_SCALE, 5},
                      {"6x", 0, ITEM_RADIO, MENU_ITEM_SCALE_6, SET_SCALE, 6}}, 6},
    {"Menu size", {{"Automatic", 0, ITEM_RADIO, 0, SET_MENU_SCALE, 0},
                   {"1x", 0, ITEM_RADIO, 0, SET_MENU_SCALE, 1, ITEM_GROUP_BREAK},
                   {"2x", 0, ITEM_RADIO, 0, SET_MENU_SCALE, 2},
                   {"3x", 0, ITEM_RADIO, 0, SET_MENU_SCALE, 3},
                   {"4x", 0, ITEM_RADIO, 0, SET_MENU_SCALE, 4}}, 5},
    {"Game speed", {{"50%", 0, ITEM_RADIO, 0, SET_SPEED, 50},
                    {"100%", 0, ITEM_RADIO, 0, SET_SPEED, 100},
                    {"150%", 0, ITEM_RADIO, 0, SET_SPEED, 150},
                    {"200%", 0, ITEM_RADIO, 0, SET_SPEED, 200},
                    {"300%", 0, ITEM_RADIO, 0, SET_SPEED, 300},
                    {"400%", "Tab", ITEM_RADIO, 0, SET_SPEED, 400},
                    {"Uncapped", 0, ITEM_RADIO, 0, SET_SPEED, -1}}, 7},
    {"Frame rate", {{"Display refresh", 0, ITEM_RADIO, 0, SET_FPS, 0},
                    {"30", 0, ITEM_RADIO, 0, SET_FPS, 30},
                    {"60", 0, ITEM_RADIO, 0, SET_FPS, 60},
                    {"120", 0, ITEM_RADIO, 0, SET_FPS, 120},
                    {"144", 0, ITEM_RADIO, 0, SET_FPS, 144},
                    {"240", 0, ITEM_RADIO, 0, SET_FPS, 240},
                    {"Every game frame", 0, ITEM_RADIO, 0, SET_FPS, -1}}, 7},
    /* Give N: the row's value is the copies of every card (cheats.h). */
    {"Cheats", {{"Give 1 of every card", 0, ITEM_ACTION, ACT_GIVE_CARDS, -1, 1},
                {"Give 2 of every card", 0, ITEM_ACTION, ACT_GIVE_CARDS, -1, 2},
                {"Give 3 of every card", 0, ITEM_ACTION, ACT_GIVE_CARDS, -1, 3},
                {"Unlock all Free Duel CPU duelists", 0, ITEM_ACTION, ACT_UNLOCK_FREE_DUELISTS, -1, 0, ITEM_GROUP_BREAK},
                {"Set StarChips to 999999", 0, ITEM_ACTION, ACT_SET_STARCHIPS, -1, 999999, ITEM_GROUP_BREAK},
                {"Free spending (Password)", 0, ITEM_CHECK, 0, SET_CHEAT_FREE_SPENDING},
                {"Starting LP 1000", 0, ITEM_RADIO, 0, SET_CHEAT_LIFE_POINTS, 1000, ITEM_GROUP_BREAK},
                {"Starting LP 4000", 0, ITEM_RADIO, 0, SET_CHEAT_LIFE_POINTS, 4000},
                {"Starting LP 8000 (console)", 0, ITEM_RADIO, 0, SET_CHEAT_LIFE_POINTS, 8000},
                {"Starting LP 9999", 0, ITEM_RADIO, 0, SET_CHEAT_LIFE_POINTS, 9999},
                {"Show CPU's hand", 0, ITEM_CHECK, 0, SET_CHEAT_SHOW_HAND, 0, ITEM_GROUP_BREAK}}, 11},
    {"Trace", {{"Frames", 0, ITEM_CHECK, CHECK_TRACE, -1, LOG_FRAMES},
               {"Disc", 0, ITEM_CHECK, CHECK_TRACE, -1, LOG_DISC},
               {"SPU", 0, ITEM_CHECK, CHECK_TRACE, -1, LOG_SPU},
               {"Input", 0, ITEM_CHECK, CHECK_TRACE, -1, LOG_INPUT},
               {"State", 0, ITEM_CHECK, CHECK_TRACE, -1, LOG_STATE}}, 5},
    {"Scaling", {{"Integer scaling", 0, ITEM_RADIO, MENU_ITEM_SCALING_INTEGER, SET_SCALING, 0},
                  {"Fit to window", 0, ITEM_RADIO, MENU_ITEM_SCALING_FIT, SET_SCALING, 1},
                  {"Stretch", 0, ITEM_RADIO, MENU_ITEM_SCALING_STRETCH, SET_SCALING, 2}}, 3},
    {"Aspect Ratio", {{"4:3 aspect", 0, ITEM_RADIO, MENU_ITEM_ASPECT_4_3, SET_ASPECT, 0},
                       {"Square pixels", 0, ITEM_RADIO, MENU_ITEM_ASPECT_SQUARE, SET_ASPECT, 1},
                       {"Widescreen (16:9)", 0, ITEM_RADIO, MENU_ITEM_ASPECT_WIDESCREEN, SET_ASPECT, 2}}, 3},
    {"Resolution", {{"Console resolution", 0, ITEM_RADIO, 0, SET_INTERNAL_SCALE, 1},
                     {"Internal 2x", 0, ITEM_RADIO, 0, SET_INTERNAL_SCALE, 2},
                     {"Internal 4x", 0, ITEM_RADIO, 0, SET_INTERNAL_SCALE, 4}}, 3},
    /* The present pass (src/pc/render/present_pass.c); 100 is the picture as is. */
    {"Color", {{"Brightness", 0, ITEM_SLIDER, 0, SET_BRIGHTNESS},
               {"Contrast", 0, ITEM_SLIDER, 0, SET_CONTRAST},
               {"Saturation", 0, ITEM_SLIDER, 0, SET_SATURATION},
               {"Gamma", 0, ITEM_SLIDER, 0, SET_GAMMA},
               {"Reset", 0, ITEM_ACTION, ACT_RESET_COLOR, -1, 0, ITEM_GROUP_BREAK}}, 5},
    {"Effects", {{"CRT scanlines", 0, ITEM_CHECK, MENU_ITEM_CRT, SET_CRT},
                 {"Reduce flashes", 0, ITEM_CHECK, MENU_ITEM_FLASH, SET_FLASH},
                 {"xBR pixel smoothing", 0, ITEM_CHECK, MENU_ITEM_XBR, SET_XBR}}, 3},
    /* The others go by way of the title and the game's debug menu, as the
     * control channel's `jump` does (title_jump.h); a duel needs a deck. */
    {"Jump to", {{"Title Screen", 0, ITEM_ACTION, MENU_ITEM_TITLE, -1, 0, ITEM_DISABLED},
                 {"Debug Menu", 0, ITEM_ACTION, ACT_JUMP, -1, JUMP_DEBUG_MENU, ITEM_GROUP_BREAK},
                 {"Free Duel", 0, ITEM_ACTION, ACT_JUMP, -1, JUMP_FREE_DUEL},
                 {"Build Deck", 0, ITEM_ACTION, ACT_JUMP, -1, JUMP_BUILD_DECK},
                 {"Library", 0, ITEM_ACTION, ACT_JUMP, -1, JUMP_LIBRARY},
                 {"Password", 0, ITEM_ACTION, ACT_JUMP, -1, JUMP_PASSWORD},
                 {"Map", 0, ITEM_ACTION, ACT_JUMP, -1, JUMP_MAP},
                 {"Options", 0, ITEM_ACTION, ACT_JUMP, -1, JUMP_OPTIONS},
                 {"Credits", 0, ITEM_ACTION, ACT_JUMP, -1, JUMP_CREDITS}}, 9},
    {"Anti-aliasing", {{"Off", 0, ITEM_RADIO, 0, SET_MSAA, 0},
                       {"2x", 0, ITEM_RADIO, 0, SET_MSAA, 2},
                       {"4x", 0, ITEM_RADIO, 0, SET_MSAA, 4},
                       {"8x", 0, ITEM_RADIO, 0, SET_MSAA, 8}}, 4},
    {"Filtering", {{"Nearest", 0, ITEM_RADIO, MENU_ITEM_FILTER_NEAREST, SET_FILTER, 0},
                   {"Smooth (bilinear)", 0, ITEM_RADIO, MENU_ITEM_FILTER_LINEAR, SET_FILTER, 1},
                   {"Sharp bilinear", 0, ITEM_RADIO, MENU_ITEM_FILTER_SHARP, SET_FILTER, 2}}, 3},
    /* The rank a duel against the computer is heading for (rank_meter.h). */
    {"Duel rank", {{"Off", 0, ITEM_RADIO, 0, SET_RANK_METER, 0},
                   {"Rank", 0, ITEM_RADIO, 0, SET_RANK_METER, 1},
                   {"Rank and score", 0, ITEM_RADIO, 0, SET_RANK_METER, 2}}, 3},
    /* The game's own translations (language.h); labels and states set by
     * update_language_items. */
    {"Language", {{"", 0, ITEM_RADIO, RADIO_LANGUAGE, SET_LANGUAGE, LANGUAGE_US},
                  {"", 0, ITEM_RADIO, RADIO_LANGUAGE, SET_LANGUAGE, LANGUAGE_EN_EU, ITEM_GROUP_BREAK},
                  {"", 0, ITEM_RADIO, RADIO_LANGUAGE, SET_LANGUAGE, LANGUAGE_FR},
                  {"", 0, ITEM_RADIO, RADIO_LANGUAGE, SET_LANGUAGE, LANGUAGE_DE},
                  {"", 0, ITEM_RADIO, RADIO_LANGUAGE, SET_LANGUAGE, LANGUAGE_IT},
                  {"", 0, ITEM_RADIO, RADIO_LANGUAGE, SET_LANGUAGE, LANGUAGE_ES}}, 6},
    /* PGXP recovers the GTE's discarded depth to fix affine texture warping
     * on tilted polygons (pgxp.h). Level 2, precise positions, is not
     * offered yet (settings.c clamps it). */
    {"Precise geometry", {{"Off", 0, ITEM_RADIO, 0, SET_PGXP, 0},
                          {"Textures", 0, ITEM_RADIO, 0, SET_PGXP, 1}}, 2},
    /* The on-screen controller for touch screens (touch_pad.h). */
    {"Touch controls", {{"Automatic (after a touch)", 0, ITEM_RADIO, 0, SET_TOUCH_PAD, 0},
                        {"Show", 0, ITEM_RADIO, 0, SET_TOUCH_PAD, 1},
                        {"Hide", 0, ITEM_RADIO, 0, SET_TOUCH_PAD, 2},
                        {"Opacity 25%", 0, ITEM_RADIO, 0, SET_TOUCH_OPACITY, 25, ITEM_GROUP_BREAK},
                        {"Opacity 50%", 0, ITEM_RADIO, 0, SET_TOUCH_OPACITY, 50},
                        {"Opacity 75%", 0, ITEM_RADIO, 0, SET_TOUCH_OPACITY, 75}}, 6},
    /* Android (android.c, Android_ApplyScreenRotation): turned over with the
     * phone by its sensor, or only as the system's auto-rotate allows. */
    {"Screen rotation", {{"Turn with the phone", 0, ITEM_RADIO, 0, SET_SCREEN_ROTATION, 0},
                         {"Follow auto-rotate", 0, ITEM_RADIO, 0, SET_SCREEN_ROTATION, 1}}, 2},
};

static int open_menu = -1, hot_item = -1, hover_bar = -1, grabbed, ready, visible = 1;
/* The open submenu (index into submenus), its row in the bar menu, and its hot row. */
static int open_sub = -1, sub_item = -1, hot_sub = -1;
static int consumed_press[8];
/* A menu taller than the window is cut to it and its rows scroll (by a
 * drag, the wheel, the arrow bands at its ends, or the keyboard keeping
 * the lit row in view): how far each open level is scrolled, in pixels.
 * A press on a row of such a menu acts when it comes up without having
 * dragged, so a finger can scroll it without choosing what it lands on. */
static int scroll[2];
static struct { int active, level, x, y, from, dragged; } drag;
#define SCROLL_BAND (ITEM_H / 2)
/* The part of the window overlays keep within (Menu_SetOverlayArea). */
static int span_left, span_right, span_top = -1;

/* One notice at a time, over the middle of the picture (menu.h). */
#define NOTICE_MAX_W (500 * ui)
#define NOTICE_PAD (18 * ui)
#define NOTICE_LINE (19 * ui)
#define NOTICE_BUTTON_H (28 * ui > touch_row ? 28 * ui : touch_row)
#define NOTICE_BUTTON_PAD (14 * ui)
#define NOTICE_BUTTON_GAP (8 * ui)
#define NOTICE_LINES 24

static struct {
    int shown, count, focus, hover;
    char title[128], text[1024], buttons[MENU_NOTICE_BUTTONS][48];
    void (*chosen)(int button, int *quit);
    /* The text wrapped to the box: where each line starts and its length. */
    int line_start[NOTICE_LINES], line_length[NOTICE_LINES], lines, wrapped_ui, wrapped_width;
} notice;
static int changed;

/* --- text ------------------------------------------------------------ */

typedef struct { unsigned char *coverage; int w, h, left, top, advance; } Glyph;
static Glyph glyphs[96];
static int font_ascent, font_descent, font_loaded;
/* Past ASCII: the face stays open and each character is rendered the first
 * time it is asked for, kept in an open-addressed table (code 0 is a free
 * slot); `missing` marks one the face has no glyph for. */
typedef struct { uint32_t code; int missing; Glyph glyph; } ExtraGlyph;
#define EXTRA_GLYPHS_MAX 4096
static FT_Library font_library;
static FT_Face font_face;
static ExtraGlyph *extra_glyphs;
static unsigned extra_mask, extra_count;

typedef struct { char code; unsigned char rows[7]; } BitmapGlyph;
/* --- the fallback font ----------------------------------------------- */

/* 5x7 glyphs, one bit per pixel, the top row first. */
static const BitmapGlyph bitmap_font[] = {
    {' ', {0, 0, 0, 0, 0, 0, 0}},
    {'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}}, {'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}}, {'3', {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e}},
    {'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}}, {'5', {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e}},
    {'6', {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}}, {'7', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}}, {'9', {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}},
    {'A', {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}}, {'B', {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}},
    {'C', {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}}, {'D', {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}},
    {'E', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}}, {'F', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}},
    {'G', {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}}, {'H', {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'I', {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}},
    {'M', {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}}, {'P', {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}},
    {'Q', {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}}, {'R', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}},
    {'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}}, {'T', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}}, {'X', {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04}}, {'Z', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}},
    {'a', {0x00, 0x00, 0x0e, 0x01, 0x0f, 0x11, 0x0f}}, {'b', {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x1e}},
    {'c', {0x00, 0x00, 0x0e, 0x10, 0x10, 0x11, 0x0e}}, {'d', {0x01, 0x01, 0x0d, 0x13, 0x11, 0x11, 0x0f}},
    {'e', {0x00, 0x00, 0x0e, 0x11, 0x1f, 0x10, 0x0e}}, {'f', {0x06, 0x09, 0x08, 0x1c, 0x08, 0x08, 0x08}},
    {'g', {0x00, 0x0f, 0x11, 0x11, 0x0f, 0x01, 0x0e}}, {'h', {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11}},
    {'i', {0x04, 0x00, 0x0c, 0x04, 0x04, 0x04, 0x0e}}, {'j', {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0c}},
    {'k', {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}}, {'l', {0x0c, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'m', {0x00, 0x00, 0x1a, 0x15, 0x15, 0x15, 0x15}}, {'n', {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11}},
    {'o', {0x00, 0x00, 0x0e, 0x11, 0x11, 0x11, 0x0e}}, {'p', {0x00, 0x00, 0x1e, 0x11, 0x1e, 0x10, 0x10}},
    {'q', {0x00, 0x00, 0x0d, 0x13, 0x0f, 0x01, 0x01}}, {'r', {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}},
    {'s', {0x00, 0x00, 0x0f, 0x10, 0x0e, 0x01, 0x1e}}, {'t', {0x08, 0x08, 0x1c, 0x08, 0x08, 0x09, 0x06}},
    {'u', {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0d}}, {'v', {0x00, 0x00, 0x11, 0x11, 0x11, 0x0a, 0x04}},
    {'w', {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0a}}, {'x', {0x00, 0x00, 0x11, 0x0a, 0x04, 0x0a, 0x11}},
    {'y', {0x00, 0x00, 0x11, 0x11, 0x0f, 0x01, 0x0e}}, {'z', {0x00, 0x00, 0x1f, 0x02, 0x04, 0x08, 0x1f}},
    {'-', {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}}, {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c}},
    {':', {0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00}}, {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}}, {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x0c, 0x04, 0x08}}, {';', {0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x04, 0x08}},
    {'\'', {0x0c, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}}, {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
    {'?', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
};
static const int bitmap_font_count = (int)(sizeof(bitmap_font) / sizeof(bitmap_font[0]));

/* Renders one character of `face` into `g`; 0 when the face cannot. */
static int render_glyph(FT_Face face, FT_ULong character, Glyph *g)
{
    FT_Bitmap *b;
    /* Outlines only: a font's embedded bitmaps (Wine's Tahoma has them
     * at 8-16 px) come back 1 bit per pixel, not the bytes read below. */
    if (FT_Load_Char(face, character, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP)) {
        return 0;
    }
    b = &face->glyph->bitmap;
    g->w = (int)b->width;
    g->h = (int)b->rows;
    g->left = face->glyph->bitmap_left;
    g->top = face->glyph->bitmap_top;
    g->advance = (int)((face->glyph->advance.x + 32) >> 6);
    g->coverage = malloc((size_t)g->w * (size_t)g->h + 1);
    if (g->coverage) {
        int row;
        for (row = 0; row < g->h; row++) {
            memcpy(g->coverage + row * g->w, b->buffer + row * b->pitch, (size_t)g->w);
        }
    }
    return 1;
}

/* Forgets the characters past ASCII and closes the face (a new size renders
 * them again). */
static void forget_big_glyphs(void);

static void free_extra_glyphs(void)
{
    unsigned i;
    forget_big_glyphs();
    for (i = 0; extra_glyphs && i <= extra_mask; i++) free(extra_glyphs[i].glyph.coverage);
    free(extra_glyphs);
    extra_glyphs = NULL;
    extra_mask = extra_count = 0;
    if (font_face) FT_Done_Face(font_face);
    if (font_library) FT_Done_FreeType(font_library);
    font_face = NULL;
    font_library = NULL;
}

static void load_font(void)
{
    FT_Library library;
    FT_Face face;
#if defined(_WIN32) || defined(__APPLE__)
#ifdef _WIN32
    const char *file = Win32_FontPath(0);
#else
    const char *file = MacOS_FontPath(MEMORIES_FONT_UI);
#endif
#else
    FcPattern *pattern, *match;
    FcResult result;
    FcChar8 *file = NULL;
#endif
    int c;
    for (c = 0; c < 96; c++) {
        free(glyphs[c].coverage);
        memset(&glyphs[c], 0, sizeof(glyphs[c]));
    }
    free_extra_glyphs();
    font_loaded = 0;
#if defined(_WIN32) || defined(__APPLE__)
    if (!file || FT_Init_FreeType(&library)) {
        return;
    }
    if (FT_New_Face(library, file, 0, &face) || FT_Set_Pixel_Sizes(face, 0, FONT_PX)) {
        return;
    }
#else
    if (!FcInit() || FT_Init_FreeType(&library)) {
        return;
    }
    pattern = FcNameParse((const FcChar8 *)"sans-serif");
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    match = FcFontMatch(NULL, pattern, &result);
    if (!match || FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch ||
        FT_New_Face(library, (const char *)file, 0, &face) || FT_Set_Pixel_Sizes(face, 0, FONT_PX)) {
        return;
    }
#endif
    for (c = 32; c < 127; c++) {
        render_glyph(face, (FT_ULong)c, &glyphs[c - 32]);
    }
    font_ascent = (int)(face->size->metrics.ascender >> 6);
    font_descent = (int)(-face->size->metrics.descender >> 6);
    font_loaded = 1;
#if !defined(_WIN32) && !defined(__APPLE__)
    FcPatternDestroy(pattern);
    FcPatternDestroy(match);
#endif
    font_face = face;
    font_library = library;
}

/* The glyph a character is drawn with: control characters (and DEL) take
 * the space's room as they always have; anything the face cannot draw, and
 * any byte that is not UTF-8, is "?". Needs font_loaded. */
static const Glyph *glyph_for(uint32_t character)
{
    const Glyph *unknown = &glyphs['?' - 32];
    unsigned slot;
    if (character < 128) return &glyphs[character >= 32 && character < 127 ? character - 32 : 0];
    if (character == GLYPHS_NOT_UTF8 || !font_face) return unknown;
    if (extra_glyphs) {
        for (slot = (character * 2654435761u) & extra_mask; extra_glyphs[slot].code; slot = (slot + 1) & extra_mask)
            if (extra_glyphs[slot].code == character)
                return extra_glyphs[slot].missing ? unknown : &extra_glyphs[slot].glyph;
    }
    if (extra_count >= EXTRA_GLYPHS_MAX) return unknown;
    /* Keep the table at most half full: grow it (and rehash) first. */
    if (!extra_glyphs || (extra_count + 1) * 2 > extra_mask + 1) {
        unsigned size = extra_glyphs ? (extra_mask + 1) * 2 : 256, i;
        ExtraGlyph *table = calloc(size, sizeof(*table));
        if (!table) return unknown;
        for (i = 0; extra_glyphs && i <= extra_mask; i++) {
            if (!extra_glyphs[i].code) continue;
            for (slot = (extra_glyphs[i].code * 2654435761u) & (size - 1); table[slot].code;
                 slot = (slot + 1) & (size - 1)) {}
            table[slot] = extra_glyphs[i];
        }
        free(extra_glyphs);
        extra_glyphs = table;
        extra_mask = size - 1;
    }
    for (slot = (character * 2654435761u) & extra_mask; extra_glyphs[slot].code; slot = (slot + 1) & extra_mask) {}
    extra_glyphs[slot].code = character;
    extra_glyphs[slot].missing = !FT_Get_Char_Index(font_face, (FT_ULong)character) ||
                                 !render_glyph(font_face, (FT_ULong)character, &extra_glyphs[slot].glyph);
    extra_count++;
    return extra_glyphs[slot].missing ? unknown : &extra_glyphs[slot].glyph;
}

static void layout_bar(void);

int Menu_Scale(void) { return ui; }

void Menu_SetScale(int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    if (scale == ui) return;
    ui = scale;
    if (ready) {
        load_font();
        layout_bar();
    }
}

/* One step below the original automatic size, with a 1x minimum:
 * a 4x window is 1, a 4K display 2. Manual sizes are unchanged. */
int Menu_AutoScale(int window_h)
{
    int scale = (window_h + 240) / 480;
    scale = scale < 1 ? 1 : scale > 3 ? 3 : scale;
    return scale > 1 ? scale - 1 : 1;
}

static MenuCanvas *canvas;
/* Rows drawn outside these canvas rows are cut (a scrolled menu's). */
static int clip_top, clip_bottom = 1 << 30;

static int text_width(const char *text)
{
    int width = 0;
    while (*text) {
        uint32_t character = Glyphs_NextCharacter(&text);
        width += font_loaded ? glyph_for(character)->advance : 12 * ui;
    }
    return width;
}

/* Source over destination. On an opaque canvas the destination's alpha is
 * ignored and the result is fully opaque (a backend may upload the canvas to
 * a texture that blends by alpha); on an overlay both alphas take part (straight, not premultiplied)
 * so the platform can blend the result over the picture. */
static inline uint32_t blend(uint32_t under, uint32_t over, unsigned alpha)
{
    unsigned inverse = 255 - alpha, r, g, b;
    if (canvas->alpha) {
        unsigned da = under >> 24, out_a = alpha + da * inverse / 255, back = da * inverse / 255;
        if (!out_a) {
            return 0;
        }
        r = ((over >> 16 & 0xff) * alpha + (under >> 16 & 0xff) * back) / out_a;
        g = ((over >> 8 & 0xff) * alpha + (under >> 8 & 0xff) * back) / out_a;
        b = ((over & 0xff) * alpha + (under & 0xff) * back) / out_a;
        return out_a << 24 | r << 16 | g << 8 | b;
    }
    r = ((over >> 16 & 0xff) * alpha + (under >> 16 & 0xff) * inverse + 127) / 255;
    g = ((over >> 8 & 0xff) * alpha + (under >> 8 & 0xff) * inverse + 127) / 255;
    b = ((over & 0xff) * alpha + (under & 0xff) * inverse + 127) / 255;
    return 0xff000000u | r << 16 | g << 8 | b;
}

static void put(int x, int y, uint32_t color, unsigned alpha)
{
    uint32_t *at;
    if (x < 0 || y < 0 || x >= canvas->width || y >= canvas->height || !alpha || y < clip_top || y >= clip_bottom) {
        return;
    }
    at = canvas->pixels + (size_t)y * (size_t)canvas->stride + (size_t)x;
    *at = alpha >= 255 ? (color | 0xff000000u) : blend(*at, color, alpha);
}

static void fill(int x, int y, int w, int h, uint32_t color, unsigned alpha)
{
    int i, j;
    if (x < 0) { w += x; x = 0; }
    if (y < clip_top) { h -= clip_top - y; y = clip_top; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > canvas->width) { w = canvas->width - x; }
    if (y + h > canvas->height) { h = canvas->height - y; }
    if (y + h > clip_bottom) { h = clip_bottom - y; }
    for (j = 0; j < h; j++) {
        uint32_t *row = canvas->pixels + (size_t)(y + j) * (size_t)canvas->stride + (size_t)x;
        if (alpha >= 255) {
            for (i = 0; i < w; i++) row[i] = color | 0xff000000u;
        } else {
            for (i = 0; i < w; i++) row[i] = blend(row[i], color, alpha);
        }
    }
}

static void clear_alpha_rect(int x, int y, int w, int h)
{
    int row;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > canvas->width) w = canvas->width - x;
    if (y + h > canvas->height) h = canvas->height - y;
    if (w <= 0 || h <= 0) return;
    for (row = y; row < y + h; row++) {
        memset(canvas->pixels + (size_t)row * (size_t)canvas->stride + (size_t)x, 0, (size_t)w * 4);
    }
}

static void outline(int x, int y, int w, int h, uint32_t color)
{
    fill(x, y, w, 1, color, 255);
    fill(x, y + h - 1, w, 1, color, 255);
    fill(x, y, 1, h, color, 255);
    fill(x + w - 1, y, 1, h, color, 255);
}

/* An anti-aliased disc: coverage from the distance to the edge. */
static void disc(int cx, int cy, int radius, uint32_t color)
{
    int x, y;
    for (y = -radius - 1; y <= radius + 1; y++) {
        for (x = -radius - 1; x <= radius + 1; x++) {
            float d = (float)__builtin_sqrtf((float)(x * x + y * y)) - (float)radius + 0.5f;
            unsigned alpha = d <= 0 ? 255 : d >= 1 ? 0 : (unsigned)((1 - d) * 255);
            put(cx + x, cy + y, color, alpha);
        }
    }
}

static void draw_bitmap_text(int x, int middle, const char *text, uint32_t color)
{
    for (; *text; x += 12 * ui) {
        uint32_t character = Glyphs_NextCharacter(&text);
        char code = character < 128 ? (char)character : '?';
        int i;
        for (i = 0; i < bitmap_font_count; i++) {
            int row, column;
            if (bitmap_font[i].code != code) {
                continue;
            }
            for (row = 0; row < 7; row++) {
                for (column = 0; column < 5; column++) {
                    if (bitmap_font[i].rows[row] & (0x10 >> column)) {
                        fill(x + column * 2 * ui, middle - 7 * ui + row * 2 * ui, 2 * ui, 2 * ui, color, 255);
                    }
                }
            }
            break;
        }
    }
}

/* Text with its vertical centre on `middle`. */
static void draw_text(int x, int middle, const char *text, uint32_t color)
{
    int baseline;
    if (!font_loaded) {
        draw_bitmap_text(x, middle, text, color);
        return;
    }
    baseline = middle + (font_ascent - font_descent + 1) / 2;
    while (*text) {
        const Glyph *g = glyph_for(Glyphs_NextCharacter(&text));
        int row, column;
        for (row = 0; row < g->h && g->coverage; row++) {
            for (column = 0; column < g->w; column++) {
                put(x + g->left + column, baseline - g->top + row, color, g->coverage[row * g->w + column]);
            }
        }
        x += g->advance;
    }
}

void Menu_DrawText(MenuCanvas *into, int x, int y, const char *text, uint32_t color)
{
    canvas = into;
    draw_text(x, y, text, color);
}

int Menu_TextWidth(const char *text) { return text_width(text); }

int Menu_TextWidthScaled(const char *text, int scale) { return text_width(text) * scale / ui; }
/* Characters bigger than the menu's own (Menu_DrawTextScaled at a scale
 * above it: the title's text lines, the side windows), rendered at their
 * size rather than magnified, kept by character and scale until the table
 * fills, when it starts again. */
typedef struct { uint32_t code; int scale, missing; Glyph glyph; } BigGlyph;
#define BIG_GLYPHS 1024
static BigGlyph big_glyphs[BIG_GLYPHS];
static int big_count;

static void forget_big_glyphs(void)
{
    int i;
    for (i = 0; i < BIG_GLYPHS; i++) free(big_glyphs[i].glyph.coverage);
    memset(big_glyphs, 0, sizeof(big_glyphs));
    big_count = 0;
}

static const Glyph *big_glyph_for(uint32_t character, int scale)
{
    unsigned slot = (character * 2654435761u + (unsigned)scale * 40503u) & (BIG_GLYPHS - 1);
    BigGlyph *b;
    for (b = &big_glyphs[slot]; b->code; slot = (slot + 1) & (BIG_GLYPHS - 1), b = &big_glyphs[slot]) {
        if (b->code == character && b->scale == scale) return b->missing ? NULL : &b->glyph;
    }
    if (big_count * 2 >= BIG_GLYPHS) {
        forget_big_glyphs();
        slot = (character * 2654435761u + (unsigned)scale * 40503u) & (BIG_GLYPHS - 1);
        b = &big_glyphs[slot];
    }
    b->code = character;
    b->scale = scale;
    big_count++;
    /* The face is the menu's: at the character's size for this one, then
     * back to the menu's for everything else. */
    b->missing = FT_Set_Pixel_Sizes(font_face, 0, (FT_UInt)(13 * scale)) ||
                 !FT_Get_Char_Index(font_face, (FT_ULong)character) ||
                 !render_glyph(font_face, (FT_ULong)character, &b->glyph);
    FT_Set_Pixel_Sizes(font_face, 0, FONT_PX);
    return b->missing ? NULL : &b->glyph;
}

void Menu_DrawTextScaled(MenuCanvas *into, int x, int middle, const char *text, uint32_t color, int scale)
{
    canvas = into;
    if (scale == ui) { draw_text(x, middle, text, color); return; }
    if (!font_loaded) {
        int previous = ui; ui = scale; draw_bitmap_text(x, middle, text, color); ui = previous; return;
    }
    int baseline = middle + (font_ascent - font_descent + 1) * scale / (2 * ui);
    int advance = 0;
    while (*text) {
        uint32_t character = Glyphs_NextCharacter(&text);
        const Glyph *g = glyph_for(character), *big = scale > ui && font_face ? big_glyph_for(character, scale) : NULL;
        /* Placed by the menu-size glyph's advance, so widths are what
         * Menu_TextWidthScaled says; drawn from the one at its size. */
        if (big) {
            for (int row = 0; row < big->h && big->coverage; row++)
                for (int col = 0; col < big->w; col++)
                    put(x + advance * scale / ui + big->left + col, baseline - big->top + row, color,
                        big->coverage[row * big->w + col]);
            advance += g->advance;
            continue;
        }
        for (int row = 0; row < g->h * scale / ui && g->coverage; row++)
            for (int col = 0; col < g->w * scale / ui; col++)
                put(x + advance * scale / ui + g->left * scale / ui + col,
                    baseline - g->top * scale / ui + row, color,
                    g->coverage[(row * ui / scale) * g->w + col * ui / scale]);
        advance += g->advance;
    }
}

void Menu_LoadSettings(void)
{
    Paths_MigrateLegacySaves(); /* what older builds left in ./saves */
    Settings_Load();
    Spu_SetOutputVolume(Settings_Get(SET_MASTER_VOLUME));
    Spu_SetBusVolume(SPU_BUS_MUSIC, Settings_Get(SET_MUSIC_VOLUME));
    Spu_SetBusVolume(SPU_BUS_SFX, Settings_Get(SET_SFX_VOLUME));
    Spu_SetBusVolume(SPU_BUS_STREAM, Settings_Get(SET_STREAM_VOLUME));
    Spu_SetInterpolation((SpuInterpolation)Settings_Get(SET_AUDIO_INTERPOLATION));
    Platform_SetScale(Settings_Get(SET_SCALE));
    Memories_SetInternalScale(Settings_Get(SET_INTERNAL_SCALE));
    Platform_SetClockRate(Settings_Get(SET_SPEED));
    Platform_SetPresentCap(Settings_Get(SET_FPS));
    Mods_SetTexturePack(TexturePack_Load, TexturePack_Unload);
    Mods_SetAssets(TexturePack_LoadAssets, TexturePack_LoadAssetFolder);
    Mods_SetAudio(AudioReplace_Load, AudioReplace_Unload);
    Mods_SetAudioClips(AudioReplace_AddClip, AudioReplace_PlayClip, AudioReplace_FreeClip);
    Mods_Load(); /* the mods the settings say are applied, once they are read */
}

static void update_hd_items(void);

static void setting_changed(SettingId id, int value)
{
    switch (id) {
    case SET_MASTER_VOLUME: Spu_SetOutputVolume(value); break;
    case SET_MUSIC_VOLUME: Spu_SetBusVolume(SPU_BUS_MUSIC, value); break;
    case SET_SFX_VOLUME: Spu_SetBusVolume(SPU_BUS_SFX, value); break;
    case SET_STREAM_VOLUME: Spu_SetBusVolume(SPU_BUS_STREAM, value); break;
    case SET_AUDIO_INTERPOLATION: Spu_SetInterpolation((SpuInterpolation)value); break;
    case SET_SCALE: Platform_SetScale(value); break;
    case SET_INTERNAL_SCALE: Memories_SetInternalScale(value); update_hd_items(); break;
    case SET_SPEED: Platform_SetClockRate(value); break;
    case SET_FPS: Platform_SetPresentCap(value); break;
    case SET_MENU_SCALE: Platform_ApplyDisplaySettings(); break;
    case SET_SCREEN_ROTATION: Platform_ApplyDisplaySettings(); break;
    default: break;
    }
}

/* --- layout ---------------------------------------------------------- */

int Menu_Height(void) { return MENU_H; }

static void layout_bar(void)
{
    int i, at = 0;
    for (i = 0; i < MENU_COUNT; i++) {
        menus[i].w = text_width(menus[i].label) + BAR_PAD * 2;
        menus[i].x = at;
        at += menus[i].w;
    }
}

/* A language whose text is not there (language.h) cannot be chosen. */
static void update_language_items(void)
{
    Menu *menu = &submenus[SUB_LANGUAGE];
    int i;
    for (i = 0; i < LANGUAGE_COUNT; i++) {
        Item *item = &menu->items[i];
        int available = Language_Available(item->value);
        item->label = Language_Label(item->value);
        item->flags = (item->flags & ~ITEM_DISABLED) | (available ? 0 : ITEM_DISABLED);
        if (!available) LOG(LOG_MENU, "menu: no text for %s; its Language entry is off", item->label);
    }
}

static int language_before;

static void language_chosen(int button, int *quit)
{
    static const char *const ok[] = {"OK"};
    (void)quit;
    if (button == 0 && Platform_RestartGame() < 0) {
        Menu_ShowNotice("Language", "The game could not restart itself. The language is saved; start the game "
                        "again to change it.", ok, 1, 0, NULL);
    } else if (button == 2) {
        Settings_Set(SET_LANGUAGE, language_before);
        Settings_Save();
    }
}

/* Saved at once, taken up at the next launch (the text is compiled at
 * startup, as a mod's translation is): restart now, later, or cancel. */
static void choose_language(int language)
{
    static const char *const buttons[] = {"Restart now", "Later", "Cancel"};
    static const char *const ok[] = {"OK"};
    char text[400];
    if (language == Settings_Get(SET_LANGUAGE)) return;
    language_before = Settings_Get(SET_LANGUAGE);
    Settings_Set(SET_LANGUAGE, language);
    if (!Settings_Save()) {
        char why[sizeof(notice.text)];
        Settings_Set(SET_LANGUAGE, language_before);
        snprintf(why, sizeof(why), "%s The language stays as it was.", Settings_LastError());
        Settings_TakeNewError(); /* told here */
        Menu_ShowNotice("Language", why, ok, 1, 0, NULL);
        return;
    }
    if (language == Language_Current()) return; /* back to the one this launch has */
    snprintf(text, sizeof(text),
             "%s takes effect when the game starts again. Restart now? Progress since your last save is lost.",
             Language_Label(language));
    Menu_ShowNotice("Language", text, buttons, 3, 0, language_chosen);
}

void Menu_Init(void)
{
    int i;
    for (i = 0; i < menus[MENU_HELP].count; i++)
        if (menus[MENU_HELP].items[i].id == ACT_VERSION) menus[MENU_HELP].items[i].label = Update_VersionLabel();
    load_font();
    layout_bar();
    Settings_Observe(setting_changed);
    update_hd_items(); /* until the backend says its picture pass is on */
    update_language_items();
    ready = 1;
}

void Menu_SetItemEnabled(int id, int enabled)
{
    int menu, item;
    for (menu = 0; menu < MENU_COUNT + SUB_COUNT; menu++) {
        Menu *m = menu < MENU_COUNT ? &menus[menu] : &submenus[menu - MENU_COUNT];
        for (item = 0; item < m->count; item++) {
            if (m->items[item].id == id) {
                if (enabled) m->items[item].flags &= ~ITEM_DISABLED;
                else m->items[item].flags |= ITEM_DISABLED;
            }
        }
    }
}

void Menu_SetPlatformItems(int windows, int window_modes, int update_check)
{
    int menu, item;
    for (menu = 0; menu < MENU_COUNT + SUB_COUNT; menu++) {
        Menu *m = menu < MENU_COUNT ? &menus[menu] : &submenus[menu - MENU_COUNT];
        for (item = 0; item < m->count; item++) {
            Item *it = &m->items[item];
            int keep = 1;
            if (it->kind == ITEM_ACTION && (it->id == ACT_CONTROLS || it->id == ACT_MODS)) keep = windows;
            if ((it->kind == ITEM_SUBMENU && it->value == SUB_SCALE) || it->id == MENU_ITEM_FULLSCREEN ||
                it->id == MENU_ITEM_BORDERLESS)
                keep = keep && window_modes;
            if ((it->kind == ITEM_ACTION && it->id == ACT_CHECK_UPDATES) || it->setting == SET_UPDATE_CHECK ||
                it->setting == SET_UPDATE_PRERELEASES)
                keep = keep && update_check;
            if (!keep) it->flags |= ITEM_DISABLED;
        }
    }
}

static int hd_picture, present_pass = 1;

/* An item's reason beside it, and dimmed while it has one. Only a row
 * dimmed here is enabled again (these rows have no shortcut of their own):
 * a backend may dim one for good (x11.c, Sharp bilinear). */
static void set_reason(int id, const char *why)
{
    int menu, item, had = 0;
    for (menu = 0; menu < MENU_COUNT + SUB_COUNT; menu++) {
        Menu *m = menu < MENU_COUNT ? &menus[menu] : &submenus[menu - MENU_COUNT];
        for (item = 0; item < m->count; item++) {
            if (m->items[item].id != id) continue;
            had |= m->items[item].shortcut != NULL;
            m->items[item].shortcut = why;
        }
    }
    if (why || had) Menu_SetItemEnabled(id, !why);
}

/* The desktop presenter's effects (present_pass.c) are not run on the
 * OpenGL ES path (Android, sdl.c's es_picture) or by the SDL fallback.
 * xBR is also the OpenGL picture pass's, on the textures, at Internal 2x
 * and up (gl_picture.c). */
static void update_effect_items(int console, const char *no_picture)
{
#ifdef __ANDROID__
    const char *why = present_pass ? NULL : "not on Android yet";
#else
    const char *why = present_pass ? NULL : "needs desktop OpenGL";
#endif
    set_reason(MENU_ITEM_COLOR, why);
    set_reason(MENU_ITEM_CRT, why);
    set_reason(MENU_ITEM_FLASH, why);
    set_reason(MENU_ITEM_FILTER_SHARP, why);
    set_reason(MENU_ITEM_XBR, present_pass ? NULL : !hd_picture ? no_picture : console ? "needs Internal 2x" : NULL);
}

/* HD text takes effect in the OpenGL pass at Internal 2x and up; the
 * opponent's name also at 1x, where the software GPU draws it. */
static void update_hd_items(void)
{
    static const int ids[] = {MENU_ITEM_HD_TEXT, MENU_ITEM_OPPONENT_NAME, MENU_ITEM_PGXP};
    int console = Settings_Get(SET_INTERNAL_SCALE) < 2;
#ifdef __ANDROID__ /* the system's GL is GLES (sdl.c, es_wanted) */
    const char *hd_why = !hd_picture ? "needs OpenGL ES 3" : console ? "needs Internal 2x" : NULL;
    const char *name_why = !hd_picture && !console ? "needs OpenGL ES 3 or 1x" : NULL;
#else
    const char *hd_why = !hd_picture ? "needs OpenGL 3" : console ? "needs Internal 2x" : NULL;
    const char *name_why = !hd_picture && !console ? "needs OpenGL 3 or 1x" : NULL;
#endif
    int menu, item;
    unsigned i;
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        Menu_SetItemEnabled(ids[i], !(ids[i] == MENU_ITEM_OPPONENT_NAME ? name_why : hd_why));
    }
    for (menu = 0; menu < MENU_COUNT; menu++) {
        for (item = 0; item < menus[menu].count; item++) {
            for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
                if (menus[menu].items[item].id == ids[i]) {
                    menus[menu].items[item].shortcut = ids[i] == MENU_ITEM_OPPONENT_NAME ? name_why : hd_why;
                }
            }
        }
    }
#ifdef __ANDROID__
    update_effect_items(console, "needs OpenGL ES 3");
#else
    update_effect_items(console, "needs OpenGL 3");
#endif
}

void Menu_SetHdPicture(int on)
{
    hd_picture = !!on;
    update_hd_items();
}

void Menu_SetPresentPass(int on)
{
    present_pass = !!on;
    update_hd_items();
}

void Menu_SetVisible(int wanted) { visible = !!wanted; }
int Menu_IsOpen(void) { return open_menu >= 0; }

void Menu_SetTouchTarget(int pixels) { touch_row = pixels > 0 ? pixels : 0; }
int Menu_TouchTarget(void) { return touch_row; }

static void close_menu(void);

_Static_assert(MENU_BAR_FILE == MENU_FILE && MENU_BAR_GAME == MENU_GAME, "menu.h's bar indices");

void Menu_OpenAt(int which)
{
    if (!ready || notice.shown || which < 0 || which >= MENU_COUNT) return;
    close_menu();
    open_menu = which;
    changed = 1;
}

void Menu_Open(void) { Menu_OpenAt(MENU_BAR_FILE); }

void Menu_SetOverlayArea(int left, int right, int top)
{
    span_left = left;
    span_right = right;
    span_top = top;
}

void Menu_OverlayArea(const MenuCanvas *on, int *left, int *right, int *top)
{
    *left = 0;
    *right = on ? on->width : 0;
    *top = span_top < 0 ? MENU_H : span_top;
    if (span_right <= span_left || !on) return;
    if (span_left > 0 && span_left < on->width) *left = span_left;
    if (span_right < on->width && span_right > *left) *right = span_right;
}

static int item_height(const Item *item)
{
    return item->kind == ITEM_SEPARATOR ? SEP_H : ITEM_H + (item->flags & ITEM_GROUP_BREAK ? SEP_H : 0);
}

/* Level 0 is the open bar menu, level 1 the open submenu. */
static const Menu *level_menu(int level) { return level ? &submenus[open_sub] : &menus[open_menu]; }

static int item_top(int level, int index);

/* A menu's box as drawn, without its shadow, and the height its rows need
 * (`full`). A submenu sits beside its parent row, or to its left when the
 * window is too narrow; one that would run off the bottom moves up. A menu
 * still taller than the window is cut to it and scrolls (`full` > `h`). */
static void drop_box(int level, int *x, int *y, int *w, int *h, int *full)
{
    const Menu *menu = level_menu(level);
    int i, widest = 0, shortcuts = 0, height = DROP_PAD * 2;
    for (i = 0; i < menu->count; i++) {
        const Item *item = &menu->items[i];
        int wide;
        height += item_height(item);
        if (item->kind == ITEM_SEPARATOR) {
            continue;
        }
        wide = text_width(item->label);
        if (item->kind == ITEM_SLIDER) {
            wide += 12 * ui + SLIDER_W + 12 * ui + text_width("100");
        }
        widest = wide > widest ? wide : widest;
        if (item->shortcut) {
            int s = text_width(item->shortcut);
            shortcuts = s > shortcuts ? s : shortcuts;
        } else if (item->kind == ITEM_SUBMENU) {
            shortcuts = shortcuts > 8 * ui ? shortcuts : 8 * ui;
        }
    }
    *w = ITEM_PAD + MARK_W + widest + (shortcuts ? SHORTCUT_GAP + shortcuts : 0) + ITEM_PAD;
    *h = *full = height;
    if (level == 0) {
        *x = menu->x;
        *y = MENU_H;
        if (*w < menu->w) {
            *w = menu->w;
        }
        if (canvas && *x + *w > canvas->width) *x = canvas->width > *w ? canvas->width - *w : 0;
    } else {
        int px, py, pw, ph, pfull;
        drop_box(0, &px, &py, &pw, &ph, &pfull);
        *x = px + pw - 2;
        *y = item_top(0, sub_item) - DROP_PAD;
        if (canvas && *x + *w + SHADOW > canvas->width && px - *w + 2 >= 0) *x = px - *w + 2;
        else if (canvas && *x + *w > canvas->width) *x = canvas->width > *w ? canvas->width - *w : 0;
        if (canvas && *y + *h > canvas->height) *y = canvas->height - *h > MENU_H ? canvas->height - *h : MENU_H;
    }
    if (canvas && *y + *h > canvas->height) {
        int most;
        *h = canvas->height - *y;
        if (*h < 2 * SCROLL_BAND + ITEM_H) *h = 2 * SCROLL_BAND + ITEM_H;
        most = *full - 2 * DROP_PAD - (*h - 2 * SCROLL_BAND);
        if (scroll[level] > most) scroll[level] = most;
        if (scroll[level] < 0) scroll[level] = 0;
    } else {
        scroll[level] = 0;
    }
}

static void drop_geometry(int level, int *x, int *y, int *w, int *h)
{
    int full;
    drop_box(level, x, y, w, h, &full);
}

/* Where a menu's rows start (scrolled), and whether it scrolls. */
static int rows_origin(int level, int *origin)
{
    int x, y, w, h, full;
    drop_box(level, &x, &y, &w, &h, &full);
    *origin = full > h ? y + SCROLL_BAND - scroll[level] : y + DROP_PAD;
    return full > h;
}

static void notice_geometry(int *x, int *y, int *w, int *h);

void Menu_Bounds(int *x, int *y, int *w, int *h)
{
    int level;
    *x = *y = *w = *h = 0;
    if (visible) {
        *w = canvas ? canvas->width : 0;
        *h = MENU_H;
        for (level = 0; level < (open_sub >= 0 ? 2 : open_menu >= 0 ? 1 : 0); level++) {
            int dx, dy, dw, dh;
            drop_geometry(level, &dx, &dy, &dw, &dh);
            if (dy + dh + SHADOW > *h) *h = dy + dh + SHADOW;
            if (dx + dw + SHADOW > *w) *w = dx + dw + SHADOW;
        }
    }
    if (notice.shown && ready && canvas) {
        /* From the top, as the bar is: one rectangle holds both. */
        int nx, ny, nw, nh;
        notice_geometry(&nx, &ny, &nw, &nh);
        if (*w < canvas->width) *w = canvas->width;
        if (*h < ny + nh + SHADOW) *h = ny + nh + SHADOW;
    }
}

/* Row `index` of a menu: its top. */
static int item_top(int level, int index)
{
    int y, i;
    const Menu *menu = level_menu(level);
    rows_origin(level, &y);
    for (i = 0; i < index; i++) {
        y += item_height(&menu->items[i]);
    }
    if (menu->items[index].flags & ITEM_GROUP_BREAK) y += SEP_H;
    return y;
}

static int inside_drop(int level, int px, int py)
{
    int x, y, w, h;
    drop_geometry(level, &x, &y, &w, &h);
    return px >= x && px < x + w && py >= y && py < y + h;
}

static int item_at(int level, int px, int py)
{
    int x, y, w, h, i, full;
    const Menu *menu = level_menu(level);
    drop_box(level, &x, &y, &w, &h, &full);
    if (full > h) {
        if (px < x || px >= x + w || py < y + SCROLL_BAND || py >= y + h - SCROLL_BAND) return -1;
        y += SCROLL_BAND - scroll[level];
    } else {
        if (px < x || px >= x + w || py < y + DROP_PAD || py >= y + h - DROP_PAD) {
            return -1;
        }
        y += DROP_PAD;
    }
    for (i = 0; i < menu->count; i++) {
        const Item *item = &menu->items[i];
        int height = item_height(item);
        if ((item->flags & ITEM_GROUP_BREAK) && py < y + SEP_H) return -1;
        if (py < y + height) {
            return item->kind == ITEM_SEPARATOR ? -1 : i;
        }
        y += height;
    }
    return -1;
}

/* -1 on a scrolling menu's top arrow band, 1 on its bottom one, else 0. */
static int scroll_band_at(int level, int px, int py)
{
    int x, y, w, h, full;
    drop_box(level, &x, &y, &w, &h, &full);
    if (full <= h || px < x || px >= x + w || py < y || py >= y + h) return 0;
    return py < y + SCROLL_BAND ? -1 : py >= y + h - SCROLL_BAND ? 1 : 0;
}

/* Scroll a menu so that row `index` is in view. */
static void keep_shown(int level, int index)
{
    int x, y, w, h, full, top;
    if (index < 0) return;
    drop_box(level, &x, &y, &w, &h, &full);
    if (full <= h) return;
    top = item_top(level, index);
    if (top < y + SCROLL_BAND) scroll[level] -= y + SCROLL_BAND - top;
    else if (top + ITEM_H > y + h - SCROLL_BAND) scroll[level] += top + ITEM_H - (y + h - SCROLL_BAND);
    drop_box(level, &x, &y, &w, &h, &full); /* clamps it */
}

static int bar_item_at(int x, int y)
{
    int i;
    if (y < 0 || y >= MENU_H) {
        return -1;
    }
    for (i = 0; i < MENU_COUNT; i++) {
        if (x >= menus[i].x && x < menus[i].x + menus[i].w) {
            return i;
        }
    }
    return -1;
}

static void slider_geometry(int level, int index, int *sx, int *middle)
{
    int x, y, w, h;
    drop_geometry(level, &x, &y, &w, &h);
    *sx = x + ITEM_PAD + MARK_W + text_width(level_menu(level)->items[index].label) + 12 * ui;
    *middle = item_top(level, index) + ITEM_H / 2;
}

/* A small triangle pointing right, for rows that open a submenu. */
static void draw_arrow(int x, int middle, uint32_t color)
{
    int span = 4 * ui, c;
    for (c = 0; c < span; c++) {
        int half = (span - c) * 7 * ui / (2 * span);
        fill(x + c, middle - half, 1, 2 * half + 1, color, 255);
    }
}

/* A triangle pointing up (-1) or down (1) with its middle at cx, middle:
 * a scrolling menu's arrow band. */
static void draw_band_arrow(int cx, int middle, int direction, uint32_t color)
{
    int span = 5 * ui, r;
    for (r = 0; r < span; r++) {
        int half = direction < 0 ? r : span - 1 - r;
        fill(cx - half * 3 / 2, middle - span / 2 + r, half * 3 + 1, 1, color, 255);
    }
}

/* --- drawing --------------------------------------------------------- */

static void draw_check(int x, int middle, int checked)
{
    int size = 14 * ui, top = middle - size / 2;
    if (checked) {
        int i;
        fill(x, top, size, size, C_ACCENT, 255);
        /* A tick: a short stroke down-right, a long one up-right. */
        for (i = 0; i < 3 * ui; i++) {
            fill(x + 3 * ui + i, top + 7 * ui + i, 2 * ui, 2 * ui, C_TEXT_ON_ACCENT, 255);
        }
        for (i = 0; i < 6 * ui; i++) {
            fill(x + 5 * ui + i, top + 9 * ui - i, 2 * ui, 2 * ui, C_TEXT_ON_ACCENT, 255);
        }
    } else {
        int i;
        for (i = 0; i < ui; i++) outline(x + i, top + i, size - 2 * i, size - 2 * i, C_MARK);
    }
}

static void draw_radio(int x, int middle, int on)
{
    if (on) {
        disc(x + 7 * ui, middle, 7 * ui, C_ACCENT);
        disc(x + 7 * ui, middle, 3 * ui, C_TEXT_ON_ACCENT);
    } else {
        disc(x + 7 * ui, middle, 7 * ui, C_MARK);
        disc(x + 7 * ui, middle, 6 * ui, C_DROP);
    }
}

static int item_state(const Item *item)
{
    if (item->id == CHECK_MUTE) return Spu_Muted();
    if (item->id == CHECK_HUD) return Settings_Get(SET_SHOW_HUD) != 0;
    if (item->id == CHECK_HUD_FULL) return Settings_Get(SET_SHOW_HUD) == 2;
    if (item->id == ACT_PAUSE) return Platform_ClockRate() == 0;
    if (item->id == RADIO_STATE_SLOT) return Platform_StateSlot() == item->value;
    if (item->id == CHECK_TRACE) return Log_Enabled((LogChannel)item->value);
    if (item->setting >= 0 && item->kind == ITEM_CHECK) return Settings_Get(item->setting) != 0;
    if (item->setting >= 0 && item->kind == ITEM_RADIO) return Settings_Get(item->setting) == item->value;
    return 0;
}

static void close_menu(void);

/* --- notices --------------------------------------------------------- */


static int span_width(const char *text, int length)
{
    char part[256];
    if (length >= (int)sizeof(part)) length = (int)Menu_TextFit(text, sizeof(part) - 1);
    memcpy(part, text, (size_t)length);
    part[length] = '\0';
    return text_width(part);
}

static int notice_buttons_width(void)
{
    int i, width = 0;
    for (i = 0; i < notice.count; i++)
        width += text_width(notice.buttons[i]) + NOTICE_BUTTON_PAD * 2 + (i ? NOTICE_BUTTON_GAP : 0);
    return width;
}

/* Break the text at spaces to fit `width`, and at every newline. */
static void wrap_notice(int width)
{
    const char *text = notice.text;
    int at = 0;
    notice.lines = 0;
    while (text[at] && notice.lines < NOTICE_LINES) {
        int end = at, fit = -1;
        while (text[end] && text[end] != '\n') {
            int next = end;
            while (text[next] && text[next] != ' ' && text[next] != '\n') next++;
            if (fit >= 0 && span_width(text + at, next - at) > width) break;
            fit = next;
            end = next;
            if (text[end] == ' ') end++;
        }
        if (fit < 0) fit = end;
        notice.line_start[notice.lines] = at;
        notice.line_length[notice.lines++] = fit - at;
        at = fit;
        while (text[at] == ' ') at++;
        if (text[at] == '\n') at++;
    }
    notice.wrapped_ui = ui;
    notice.wrapped_width = width;
}

static void notice_geometry(int *x, int *y, int *w, int *h)
{
    int canvas_w = canvas ? canvas->width : 640, canvas_h = canvas ? canvas->height : 480;
    int inner = NOTICE_MAX_W, least = notice_buttons_width(), title = text_width(notice.title);
    if (least < title) least = title;
    if (inner < least) inner = least;
    if (inner > canvas_w - NOTICE_PAD * 2 - 16 * ui) inner = canvas_w - NOTICE_PAD * 2 - 16 * ui;
    if (notice.wrapped_ui != ui || notice.wrapped_width != inner) wrap_notice(inner);
    *w = inner + NOTICE_PAD * 2;
    *h = NOTICE_PAD + NOTICE_LINE + 6 * ui + notice.lines * NOTICE_LINE + NOTICE_PAD + NOTICE_BUTTON_H + NOTICE_PAD;
    *x = (canvas_w - *w) / 2;
    *y = (canvas_h - *h) / 2;
    if (*y < MENU_H + 8 * ui) *y = MENU_H + 8 * ui;
}

static void notice_button(int index, int *bx, int *by, int *bw)
{
    int x, y, w, h, i, at;
    notice_geometry(&x, &y, &w, &h);
    at = x + w - NOTICE_PAD - notice_buttons_width();
    for (i = 0; i < index; i++) at += text_width(notice.buttons[i]) + NOTICE_BUTTON_PAD * 2 + NOTICE_BUTTON_GAP;
    *bx = at;
    *by = y + h - NOTICE_PAD - NOTICE_BUTTON_H;
    *bw = text_width(notice.buttons[index]) + NOTICE_BUTTON_PAD * 2;
}

static int notice_button_at(int px, int py)
{
    int i, bx, by, bw;
    for (i = 0; i < notice.count; i++) {
        notice_button(i, &bx, &by, &bw);
        if (px >= bx && px < bx + bw && py >= by && py < by + NOTICE_BUTTON_H) return i;
    }
    return -1;
}

static void draw_notice(void)
{
    int x, y, w, h, i, top;
    notice_geometry(&x, &y, &w, &h);
    if (canvas->alpha) clear_alpha_rect(x, y, w + SHADOW, h + SHADOW);
    for (i = SHADOW; i > 0; i--) {
        unsigned alpha = (unsigned)(10 + (SHADOW - i) * 8 / ui);
        fill(x + w, y + i, i, h, 0x000000u, alpha);
        fill(x + i, y + h, w, i, 0x000000u, alpha);
    }
    fill(x, y, w, h, C_DROP, 255);
    outline(x, y, w, h, C_DROP_EDGE);
    fill(x, y, w, 3 * ui, C_ACCENT, 255);
    top = y + NOTICE_PAD;
    draw_text(x + NOTICE_PAD, top + NOTICE_LINE / 2, notice.title, C_TEXT_ON_ACCENT);
    top += NOTICE_LINE + 6 * ui;
    for (i = 0; i < notice.lines; i++, top += NOTICE_LINE) {
        char line[256];
        const char *start = notice.text + notice.line_start[i];
        int length = notice.line_length[i] < (int)sizeof(line) - 1 ? notice.line_length[i]
                                                                   : (int)Menu_TextFit(start, sizeof(line) - 1);
        memcpy(line, start, (size_t)length);
        line[length] = '\0';
        draw_text(x + NOTICE_PAD, top + NOTICE_LINE / 2, line, C_TEXT);
    }
    for (i = 0; i < notice.count; i++) {
        int bx, by, bw, focus = i == notice.focus;
        notice_button(i, &bx, &by, &bw);
        fill(bx, by, bw, NOTICE_BUTTON_H, focus ? C_ACCENT : i == notice.hover ? C_TRACK : C_BAR_HOVER, 255);
        if (!focus) outline(bx, by, bw, NOTICE_BUTTON_H, C_DROP_EDGE);
        draw_text(bx + NOTICE_BUTTON_PAD, by + NOTICE_BUTTON_H / 2, notice.buttons[i], focus ? C_TEXT_ON_ACCENT : C_TEXT);
    }
}

void Menu_ShowNotice(const char *title, const char *text, const char *const *buttons, int count, int focus,
                     void (*chosen)(int button, int *quit))
{
    int i;
    if (count < 1) count = 1;
    if (count > MENU_NOTICE_BUTTONS) count = MENU_NOTICE_BUTTONS;
    close_menu();
    snprintf(notice.title, sizeof(notice.title), "%s", title ? title : "");
    snprintf(notice.text, sizeof(notice.text), "%s", text ? text : "");
    Menu_TextTrim(notice.title);
    Menu_TextTrim(notice.text);
    for (i = 0; i < count; i++) {
        snprintf(notice.buttons[i], sizeof(notice.buttons[i]), "%s", buttons && buttons[i] ? buttons[i] : "OK");
        Menu_TextTrim(notice.buttons[i]);
    }
    notice.count = count;
    notice.focus = focus >= 0 && focus < count ? focus : count - 1;
    notice.hover = -1;
    notice.chosen = chosen;
    notice.wrapped_ui = 0;
    notice.shown = 1;
    changed = 1;
    LOG(LOG_MENU, "notice \"%s\" with %d buttons", notice.title, count);
}

static void choose_notice(int button, int *quit);

int Menu_NoticeShown(void) { return ready && notice.shown; }

int Menu_NoticePad(uint16_t pressed, int *quit)
{
    if (!Menu_NoticeShown() || !pressed) return 0;
    /* In the game's own layout: Cross confirms and Circle backs out, or the
     * other way round with View > Japanese buttons. */
    pressed = ButtonLayout_Apply(pressed, 0, Settings_Get(SET_JP_BUTTONS));
    if (pressed & BUTTON_LAYOUT_CROSS) choose_notice(notice.focus, quit);
    else if (pressed & BUTTON_LAYOUT_CIRCLE) choose_notice(notice.count - 1, quit);
    else if (pressed & 0x0090) notice.focus = (notice.focus + notice.count - 1) % notice.count; /* Up, Left */
    else if (pressed & 0x0060) notice.focus = (notice.focus + 1) % notice.count;               /* Right, Down */
    else return 0;
    changed = 1;
    return 1;
}

void Menu_CloseNotice(void)
{
    if (!notice.shown) return;
    notice.shown = 0;
    changed = 1;
}

/* A settings save that failed (settings.h), told once in a notice: most
 * saves (a menu item, a slider, a moved window, a hotkey) do not look at
 * the result themselves. Waits while another notice is up. */
static void tell_settings_error(void)
{
    static const char *const ok[] = {"OK"};
    const char *error;
    if (Menu_NoticeShown() || !(error = Settings_TakeNewError())) return;
    Menu_ShowNotice("Settings not saved", error, ok, 1, 0, NULL);
}

int Menu_TakeChanged(void)
{
    int was;
    tell_settings_error();
    was = changed;
    changed = 0;
    return was;
}

static void choose_notice(int button, int *quit)
{
    void (*chosen)(int, int *) = notice.chosen;
    LOG(LOG_MENU, "notice \"%s\": %s", notice.title, notice.buttons[button]);
    notice.shown = 0; /* before the callback, which may show the next one */
    if (chosen) chosen(button, quit);
}

/* The notice has the window's mouse and keyboard while it is up. */
static int notice_event(const MenuEvent *event, int *quit)
{
    switch (event->type) {
    case MENU_EVENT_BUTTON_DOWN: {
        int button = event->button == 1 ? notice_button_at(event->x, event->y) : -1;
        if (button >= 0) choose_notice(button, quit);
        return 1;
    }
    case MENU_EVENT_MOTION: {
        int hover = notice_button_at(event->x, event->y);
        if (hover == notice.hover) return 0;
        notice.hover = hover;
        return 1;
    }
    case MENU_EVENT_LEAVE:
        if (notice.hover < 0) return 0;
        notice.hover = -1;
        return 1;
    case MENU_EVENT_KEY_DOWN:
        switch (event->key) {
        case MENU_KEY_LEFT: case MENU_KEY_UP:
            notice.focus = (notice.focus + notice.count - 1) % notice.count;
            break;
        case MENU_KEY_RIGHT: case MENU_KEY_DOWN: case MENU_KEY_TAB:
            notice.focus = (notice.focus + 1) % notice.count;
            break;
        case MENU_KEY_ENTER: choose_notice(notice.focus, quit); break;
        case MENU_KEY_ESCAPE: choose_notice(notice.count - 1, quit); break;
        default: break;
        }
        return 1;
    case MENU_EVENT_NONE:
        return 0;
    default:
        return 1; /* button up, wheel, key up, text */
    }
}


static void update_card_drops_item(void);
static int slider_value(const Item *item);

void Menu_Draw(MenuCanvas *into)
{
    int i, level;
    canvas = into;
    if (!ready) {
        return;
    }
    update_card_drops_item();
    if (!visible) {
        if (notice.shown) draw_notice();
        return;
    }
    if (canvas->alpha) {
        clear_alpha_rect(0, 0, canvas->width, MENU_H);
        for (level = 0; level < (open_sub >= 0 ? 2 : open_menu >= 0 ? 1 : 0); level++) {
            int x, y, w, h;
            drop_geometry(level, &x, &y, &w, &h);
            clear_alpha_rect(x, y, w + SHADOW, h + SHADOW);
        }
    }
    fill(0, 0, canvas->width, MENU_H - 1, C_BAR, 255);
    fill(0, MENU_H - 1, canvas->width, 1, C_BAR_EDGE, 255);
    for (i = 0; i < MENU_COUNT; i++) {
        const Menu *menu = &menus[i];
        uint32_t ink = C_TEXT;
        if (i == open_menu) {
            fill(menu->x, 0, menu->w, MENU_H - 1, C_ACCENT, 255);
            ink = C_TEXT_ON_ACCENT;
        } else if (i == hover_bar) {
            fill(menu->x, 0, menu->w, MENU_H - 1, C_BAR_HOVER, 255);
        }
        draw_text(menu->x + BAR_PAD, MENU_H / 2, menu->label, ink);
    }
    for (level = 0; level < (open_sub >= 0 ? 2 : open_menu >= 0 ? 1 : 0); level++) {
        const Menu *menu = level_menu(level);
        int x, y, w, h, top, hot_row = level ? hot_sub : hot_item, full, scrolls;
        drop_box(level, &x, &y, &w, &h, &full);
        /* The shadow: SHADOW copies of the box, each offset one more pixel,
         * blended over each other. Only the part outside the box shows (the
         * box is opaque), so blend just each copy's right and bottom strips:
         * blending whole boxes was 12 alpha passes over a 4K dropdown every
         * frame, and the game crawled while a menu was open. */
        for (i = SHADOW; i > 0; i--) {
            unsigned alpha = (unsigned)(10 + (SHADOW - i) * 8 / ui);
            fill(x + w, y + i, i, h, 0x000000u, alpha);
            fill(x + i, y + h, w, i, 0x000000u, alpha);
        }
        fill(x, y, w, h, C_DROP, 255);
        outline(x, y, w, h, C_DROP_EDGE);
        scrolls = rows_origin(level, &top);
        if (scrolls) {
            clip_top = y + SCROLL_BAND;
            clip_bottom = y + h - SCROLL_BAND;
        }
        for (i = 0; i < menu->count; i++) {
            const Item *item = &menu->items[i];
            int middle, hot = i == hot_row;
            int disabled = item->flags & ITEM_DISABLED;
            uint32_t ink = disabled ? C_TEXT_DIM : hot ? C_TEXT_ON_ACCENT : C_TEXT;
            uint32_t dim = disabled ? C_TEXT_DIM : hot ? C_TEXT_ON_ACCENT : C_TEXT_DIM;
            if (item->kind == ITEM_SEPARATOR) {
                fill(x + ITEM_PAD, top + SEP_H / 2, w - ITEM_PAD * 2, 1, C_SEP, 255);
                top += SEP_H;
                continue;
            }
            if (item->flags & ITEM_GROUP_BREAK) {
                fill(x + ITEM_PAD, top + SEP_H / 2, w - ITEM_PAD * 2, 1, C_SEP, 255);
                top += SEP_H;
            }
            middle = top + ITEM_H / 2;
            if (hot && !disabled) {
                fill(x + 3 * ui, top, w - 6 * ui, ITEM_H, C_ACCENT, 255);
            }
            if (item->kind == ITEM_CHECK) {
                draw_check(x + ITEM_PAD, middle, item_state(item));
            } else if (item->kind == ITEM_RADIO) {
                draw_radio(x + ITEM_PAD, middle, item_state(item));
            }
            draw_text(x + ITEM_PAD + MARK_W, middle, item->label, ink);
            if (item->shortcut) {
                draw_text(x + w - ITEM_PAD - text_width(item->shortcut), middle, item->shortcut, dim);
            } else if (item->kind == ITEM_SUBMENU) {
                draw_arrow(x + w - ITEM_PAD - 6 * ui, middle, ink);
            }
            if (item->kind == ITEM_SLIDER) {
                int sx, sm, knob, filled;
                char value[8];
                int minimum = Settings_Min(item->setting), maximum = Settings_Max(item->setting);
                int setting = slider_value(item);
                slider_geometry(level, i, &sx, &sm);
                knob = sx + KNOB_R + (setting - minimum) * (SLIDER_W - KNOB_R * 2) / (maximum - minimum);
                filled = knob - sx;
                fill(sx, sm - SLIDER_H / 2, SLIDER_W, SLIDER_H, C_TRACK, 255);
                /* A slider that is off (a mod's Card drops) is grey all
                 * through, as its label is. */
                fill(sx, sm - SLIDER_H / 2, filled, SLIDER_H,
                     disabled ? C_TEXT_DIM : hot ? C_TEXT_ON_ACCENT : C_ACCENT, 255);
                disc(knob, sm, KNOB_R, C_KNOB_EDGE);
                disc(knob, sm, KNOB_R - 1, disabled ? C_TEXT_DIM : C_KNOB);
                snprintf(value, sizeof(value), "%d", setting);
                draw_text(sx + SLIDER_W + 12 * ui, middle, value, setting ? ink : dim);
            }
            top += ITEM_H;
        }
        clip_top = 0;
        clip_bottom = 1 << 30;
        if (scrolls) {
            /* The arrow bands: lit while there is more that way. */
            int most = full - 2 * DROP_PAD - (h - 2 * SCROLL_BAND);
            fill(x + ITEM_PAD, y + SCROLL_BAND - 1, w - ITEM_PAD * 2, 1, C_SEP, 255);
            fill(x + ITEM_PAD, y + h - SCROLL_BAND, w - ITEM_PAD * 2, 1, C_SEP, 255);
            draw_band_arrow(x + w / 2, y + SCROLL_BAND / 2, -1, scroll[level] > 0 ? C_TEXT : C_SEP);
            draw_band_arrow(x + w / 2, y + h - SCROLL_BAND / 2, 1, scroll[level] < most ? C_TEXT : C_SEP);
        }
    }
    if (notice.shown) draw_notice();
}


/* --- behaviour ------------------------------------------------------- */

/* A slider that is off (Card drops while a mod locks it) keeps the
 * player's value as it is, whichever way it is reached. */
static void set_slider(const Item *item, int value)
{
    if (item->flags & ITEM_DISABLED) return;
    Settings_Set(item->setting, value);
}

static int slider_step(const Item *item)
{
    return item->value > 0 ? item->value : 5;
}

static void slider_from_pointer(int level, int index, int px)
{
    const Item *item = &level_menu(level)->items[index];
    int sx, middle, span = SLIDER_W - KNOB_R * 2;
    int minimum = Settings_Min(item->setting), maximum = Settings_Max(item->setting);
    slider_geometry(level, index, &sx, &middle);
    set_slider(item, minimum + ((px - sx - KNOB_R) * (maximum - minimum) + span / 2) /
               (span > 0 ? span : 1));
}

static void close_submenu(void)
{
    open_sub = -1;
    sub_item = -1;
    hot_sub = -1;
    scroll[1] = 0;
    if (drag.level == 1) drag.active = 0;
}

static void close_menu(void)
{
    open_menu = -1;
    hot_item = -1;
    grabbed = 0;
    close_submenu();
    scroll[0] = 0;
    drag.active = 0;
}

static void open_submenu(int index)
{
    const Item *item = &menus[open_menu].items[index];
    if (open_sub == item->value && sub_item == index) return;
    close_submenu();
    open_sub = item->value;
    sub_item = index;
    hot_item = index;
}

static void activate(const Item *item, int *quit);

/* A press on row `index` of an open menu: a submenu opens, a slider takes
 * the pointer, anything else is done. */
static void press_row(int level, int index, int px, int *quit)
{
    const Item *item = &level_menu(level)->items[index];
    if (level == 0 && item->kind == ITEM_SUBMENU) {
        open_submenu(index);
    } else if (item->kind == ITEM_SLIDER && !(item->flags & ITEM_DISABLED)) {
        *(level ? &hot_sub : &hot_item) = index;
        slider_from_pointer(level, index, px);
        grabbed = level + 1;
    } else {
        activate(item, quit);
    }
}

/* A press in a menu that scrolls: an arrow band moves it a row; on a row it
 * waits for the release (a drag scrolls instead), but for a slider's,
 * which takes the pointer at once. 0 when the menu does not scroll. */
static int scroll_press(int level, int px, int py)
{
    int x, y, w, h, full, band = scroll_band_at(level, px, py), index;
    drop_box(level, &x, &y, &w, &h, &full);
    if (full <= h) return 0;
    if (band) {
        scroll[level] += band * ITEM_H;
        if (level == 0) close_submenu();
        drop_box(level, &x, &y, &w, &h, &full);
        return 1;
    }
    index = item_at(level, px, py);
    if (index >= 0 && level_menu(level)->items[index].kind == ITEM_SLIDER) return 0;
    drag.active = 1;
    drag.level = level;
    drag.x = px;
    drag.y = py;
    drag.from = scroll[level];
    drag.dragged = 0;
    return 1;
}

/* The row the keyboard works on: the submenu's when one is open. */
static int active_level(void) { return open_sub >= 0 ? 1 : 0; }
static int *active_hot(void) { return open_sub >= 0 ? &hot_sub : &hot_item; }

/* The cheats that change the save refuse before a game is loaded. */
/* Help > System info for bug reports: this build's version and what a crash
 * report starts with (monitor.h: the build, the system, the GPU, the
 * settings, the mods), put on the clipboard and in system-info.txt in the
 * user folder, so a report carries them without a crash. The notice shows
 * it but for the settings, a long line. The file is opened before the facts
 * are read, so their "user dir" line (paths.h) carries its outcome when
 * nothing had been saved yet. */
static void show_system_info(void)
{
    static const char *const ok[] = {"OK"};
    static char facts[MONITOR_FACTS_SIZE], text[MONITOR_FACTS_SIZE + 128];
    char shown[sizeof(notice.text)], path[1100], why[400] = "", full[1400];
    const char *at;
    size_t used;
    int copied, saved = 0;
    FILE *file;
    snprintf(path, sizeof(path), "%s/system-info.txt", Paths_UserDir());
    Paths_WriteBegin();
    file = fopen(path, "w");
    if (file) {
        Paths_WriteDone(path);
    } else { /* before anything changes the reason */
        Paths_WriteError(full, sizeof(full), path);
        Paths_WriteReason(why, sizeof(why), path);
    }
    Monitor_Facts(facts, sizeof(facts));
    snprintf(text, sizeof(text), "YFM Re-Decomp %s\n%s", Update_VersionLabel(), facts);
    if (file) {
        saved = fputs(text, file) >= 0;
        saved = !fclose(file) && saved;
        if (!saved) {
            Paths_WriteError(full, sizeof(full), path);
            Paths_WriteReason(why, sizeof(why), path);
        }
    }
    if (!saved) fprintf(stderr, "memories-pc: could not write %s\n", full); /* the folder is the "user dir" fact's */
    copied = Platform_CopyText(text);
    used =(size_t)snprintf(shown, sizeof(shown), "%s\n", Update_VersionLabel());
    for (at = facts; *at && used < sizeof(shown);) {
        const char *end = strchr(at, '\n');
        size_t length = end ? (size_t)(end - at) + 1 : strlen(at);
        if (strncmp(at, "settings:", 9) && strncmp(at, "started:", 8))
            used += (size_t)snprintf(shown + used, sizeof(shown) - used, "%.*s", (int)length, at);
        at += length;
    }
    if (used < sizeof(shown)) {
        snprintf(shown + used, sizeof(shown) - used, "\n%s%s%s%s%s%s",
                 copied ? "Copied to the clipboard: paste it into your bug report." : "",
                 copied && saved ? " Also in " : saved ? "Attach this file to your bug report: " : "",
                 saved ? path : "", saved ? "." : "",
                 saved ? "" : copied ? " Could not write system-info.txt: " : "Could not write system-info.txt: ",
                 saved ? "" : why);
    }
    Menu_ShowNotice("System info", shown, ok, 1, 0, NULL);
}

static void need_save(int done)
{
    static const char *const ok[] = {"OK"};
    if (!done) {
        Menu_ShowNotice("Load a save first", "This cheat changes the game in progress. "
                        "Start a new game or load a save, then choose it again.", ok, 1, 0, NULL);
    }
}

/* Build Deck lists its own copy of the chest and puts it back on leaving,
 * so cards given while it is open would be taken away again. */
static void give_cards(int count)
{
    static const char *const ok[] = {"OK"};
    if (Cheats_SaveLoaded() && Cheats_ChestOnScreen()) {
        Menu_ShowNotice("Leave Build Deck first", "Build Deck works on its own copy of the chest and "
                        "puts it back when you leave, so cards given now would be lost. "
                        "Leave Build Deck, then choose this again.", ok, 1, 0, NULL);
        return;
    }
    need_save(Cheats_TopUpAllCards(count));
}

static void activate(const Item *item, int *quit)
{
    if (item->flags & ITEM_DISABLED) return;
    switch (item->id) {
    case ACT_MODS: Platform_OpenMods(); break;
    case ACT_CONTROLS: Platform_OpenControls(); break;
    case ACT_SAVE_STATE: Memories_StateRequest(1, Platform_StateSlot()); break;
    case ACT_LOAD_STATE: Memories_StateRequest(2, Platform_StateSlot()); break;
    case ACT_SCREENSHOT: Platform_Screenshot(0); break;
    case ACT_EXIT: QuitPrompt_Request(quit); break;
    /* A tower whose mod turns cheats off refuses them while the player is
     * in it (tower.h, Tower_Guard), with a notice. */
    case ACT_GIVE_CARDS: if (!Tower_Guard(TOWER_GUARD_CHEAT, 1, NULL, 0)) give_cards(item->value); break;
    case ACT_UNLOCK_FREE_DUELISTS:
        if (!Tower_Guard(TOWER_GUARD_CHEAT, 1, NULL, 0)) need_save(Cheats_UnlockAllFreeDuelists());
        break;
    case ACT_SET_STARCHIPS:
        if (!Tower_Guard(TOWER_GUARD_CHEAT, 1, NULL, 0)) need_save(Cheats_SetStarchips((unsigned)item->value));
        break;
    case ACT_RESET_COLOR:
        Settings_Set(SET_BRIGHTNESS, 100);
        Settings_Set(SET_CONTRAST, 100);
        Settings_Set(SET_SATURATION, 100);
        Settings_Set(SET_GAMMA, 100);
        Settings_Save();
        break;
    case ACT_CHECK_UPDATES: Update_CheckNow(); break;
    case ACT_RELEASES: Update_OpenReleases(); break;
    case ACT_SYSTEM_INFO: show_system_info(); break;
    case MENU_ITEM_TITLE: TitleJump_Request(); break;
    case ACT_JUMP: {
        char why[120];
        if (Tower_Guard(TOWER_GUARD_JUMP, 1, NULL, 0)) break;   /* a tower's duel: refused, with a notice */
        if (TitleJump_RequestTo(item->value, 0, NULL, why, sizeof(why))) fprintf(stderr, "memories-pc: jump: %s\n", why);
        break;
    }
    case MENU_ITEM_RESTART: TitleJump_Confirm(); break;
    case MENU_ITEM_DECKS: DeckMenu_Request(); break;
    case ACT_RELOAD_SETTINGS:
        Menu_LoadSettings();
        Platform_ApplyDisplaySettings();
        break;
    case ACT_PAUSE:
        Platform_SetClockRate(Platform_ClockRate() == 0 ? Settings_Get(SET_SPEED) : 0);
        break;
    case ACT_FRAME_STEP: Platform_StepFrame(); break;
    case ACT_DUMP_FRAME: case ACT_DUMP_VRAM: {
        char path[640];
        snprintf(path, sizeof(path), "%s/%s.ppm", Crash_ReportDir, item->id == ACT_DUMP_VRAM ? "vram" : "frame");
        Memories_DumpFrame(path, item->id == ACT_DUMP_VRAM);
        break;
    }
    case CHECK_MUTE: Spu_SetMuted(!Spu_Muted()); break;
    case CHECK_HUD:
        Settings_Set(SET_SHOW_HUD, Settings_Get(SET_SHOW_HUD) ? 0 : 1);
        Settings_Save();
        break;
    case CHECK_HUD_FULL:
        Settings_Set(SET_SHOW_HUD, Settings_Get(SET_SHOW_HUD) == 2 ? 1 : 2);
        Settings_Save();
        break;
    case RADIO_STATE_SLOT: Platform_SetStateSlot(item->value); break;
    case RADIO_LANGUAGE: choose_language(item->value); break;
    case CHECK_TRACE: Log_Enable((LogChannel)item->value, !Log_Enabled((LogChannel)item->value)); break;
    default:
        if (item->setting >= 0 && item->kind == ITEM_CHECK) {
            Settings_Set(item->setting, !Settings_Get(item->setting));
        } else if (item->setting >= 0 && item->kind == ITEM_RADIO) {
            Settings_Set(item->setting, item->value);
        }
        Settings_Save();
        if (item->id >= MENU_ITEM_SCALE_1 && item->id <= MENU_ITEM_VSYNC) {
            Platform_ApplyDisplaySettings();
        }
    }
    close_menu();
}

/* The next selectable row after `from` in `direction`, wrapping. */
static int step_item(int level, int from, int direction)
{
    const Menu *menu = level_menu(level);
    int i, index = from;
    for (i = 0; i < menu->count; i++) {
        index = (index + direction + menu->count) % menu->count;
        if (menu->items[index].kind != ITEM_SEPARATOR && !(menu->items[index].flags & ITEM_DISABLED)) {
            return index;
        }
    }
    return -1;
}

/* The Game list action an item does, or -1: its shortcut hint is that
 * action's keyboard binding (Game > Controls), or none while unbound. */
static int item_action(const Item *item)
{
    switch (item->id) {
    case ACT_EXIT: return CTRL_HOST_EXIT;
    case ACT_SAVE_STATE: return CTRL_HOST_SAVE_STATE;
    case ACT_LOAD_STATE: return CTRL_HOST_LOAD_STATE;
    case RADIO_STATE_SLOT: return CTRL_HOST_SLOT_1 + item->value - 1;
    case ACT_SCREENSHOT: return CTRL_HOST_SCREENSHOT;
    case MENU_ITEM_FULLSCREEN: return CTRL_HOST_FULLSCREEN;
    case CHECK_MUTE: return CTRL_HOST_MUTE;
    case MENU_ITEM_DECKS: return CTRL_HOST_DECK_SLOTS;
    case CHECK_HUD: return CTRL_HOST_HUD;
    case ACT_PAUSE: return CTRL_HOST_PAUSE;
    case ACT_FRAME_STEP: return CTRL_HOST_FRAME_STEP;
    default: return -1;
    }
}
/* Game > Card drops while a mod locks it ("limits": {"card_drops": n},
 * notes/card-drops.md): the row is off, its slider stands at the mod's
 * number and its label names the mod. The player's own value stays in the
 * settings, untouched, for when the mod is gone. The lock is read at
 * startup (tables.c), after the menu is made, so this is asked as the menu
 * is drawn and used. */
static int card_drops_lock;
static void update_card_drops_item(void)
{
    static char label[96];
    static Item *item;
    static int shown = -1;
    const char *id = NULL, *name = NULL;
    int i;
    card_drops_lock = Tables_CardDropsLock(&id);
    if (!item) {
        for (i = 0; i < menus[MENU_GAME].count && !item; i++)
            if (menus[MENU_GAME].items[i].setting == SET_CARD_DROPS) item = &menus[MENU_GAME].items[i];
        if (!item) return;
    }
    if (card_drops_lock == shown) return;
    shown = card_drops_lock;
    if (!card_drops_lock) {
        item->label = "Card drops";
        item->flags &= ~ITEM_DISABLED;
        return;
    }
    for (i = 0; i < Mods_Count() && !name; i++)
        if (id && !strcmp(Mods_Id(i), id)) name = Mods_Name(i);
    snprintf(label, sizeof(label), "Card drops (set by %.48s)", name ? name : id ? id : "a mod");
    item->label = label;
    item->flags |= ITEM_DISABLED;
}

/* What a slider shows: Card drops shows a mod's lock in place of the
 * player's own value. */
static int slider_value(const Item *item)
{
    if (item->setting == SET_CARD_DROPS && card_drops_lock) return card_drops_lock;
    return Settings_Get(item->setting);
}

static void update_shortcuts(void)
{
    static char labels[CTRL_HOST_COUNT][32];
    const ControlsProfile *kb = &ControlsRuntime_Config()->kb;
    for (int h = 0; h < CTRL_HOST_COUNT; h++) {
        snprintf(labels[h], sizeof(labels[h]), "%s",
                 kb->host[h][0].kind == CTRL_SRC_KEY ? Controls_SourceName(&kb->host[h][0]) : "");
        if (labels[h][0] >= 'a' && labels[h][0] <= 'z' && !labels[h][1]) labels[h][0] -= 'a' - 'A'; /* "M", as it was */
    }
    for (int m = 0; m < MENU_COUNT; m++)
        for (int i = 0; i < menus[m].count; i++) {
            int h = item_action(&menus[m].items[i]);
            if (h >= 0) menus[m].items[i].shortcut = labels[h][0] ? labels[h] : NULL;
        }
}

int Menu_Event(const MenuEvent *event, int *quit)
{
    update_shortcuts();
    update_card_drops_item();
    if (ready && notice.shown) {
        return notice_event(event, quit);
    }
    if (!ready || !visible) {
        return 0;
    }
    switch (event->type) {
    case MENU_EVENT_BUTTON_DOWN: {
        int px = event->x, py = event->y, bar = bar_item_at(px, py);
        LOG(LOG_MENU, "press b%d at %d,%d open=%d", event->button, px, py, open_menu);
        if (event->button < 1 || event->button > 3) {
            return 0;
        }
        /* While a menu is open the window belongs to it: nothing reaches the
         * pad, not even the wheel, which would otherwise step the game's
         * cursor and play its sound behind the menu. */
        if (py >= MENU_H && open_menu < 0) {
            return 0;
        }
        if (py < MENU_H) {
            if (event->button == 1) {
                if (bar >= 0 && bar != open_menu) {
                    close_menu();
                    open_menu = bar;
                } else {
                    close_menu();
                }
            }
        } else if (open_sub >= 0 && inside_drop(1, px, py)) {
            int index = item_at(1, px, py);
            if (event->button == 1 && !scroll_press(1, px, py) && index >= 0) press_row(1, index, px, quit);
        } else if (inside_drop(0, px, py)) {
            int index = item_at(0, px, py);
            if (event->button == 1 && !scroll_press(0, px, py) && index >= 0) press_row(0, index, px, quit);
        } else {
            close_menu(); /* a click outside an open menu only closes it */
        }
        consumed_press[event->button] = 1;
        return 1;
    }
    case MENU_EVENT_BUTTON_UP:
        if (event->button < 1 || event->button > 3 || !consumed_press[event->button]) {
            return 0;
        }
        consumed_press[event->button] = 0;
        if (grabbed && event->button == 1) {
            grabbed = 0;
            Settings_Save();
        }
        if (drag.active && event->button == 1) {
            int level = drag.level, index;
            drag.active = 0;
            if (!drag.dragged && open_menu >= 0 && (level == 0 || open_sub >= 0) &&
                (index = item_at(level, drag.x, drag.y)) >= 0)
                press_row(level, index, drag.x, quit);
        }
        return 1;
    case MENU_EVENT_WHEEL:
        if (open_menu >= 0 && *active_hot() >= 0) {
            int level = active_level(), index = *active_hot(), x, y, w, h, full, sx = 0, middle;
            const Item *item = &level_menu(level)->items[index];
            /* In a menu that scrolls, the wheel scrolls it unless the pointer
             * is on the slider itself (its row, right of the label): the lit
             * row is where the pointer last was, and a wheel meant to scroll
             * past it changed the setting. A menu that fits keeps the
             * wheel on a lit slider, as before. */
            drop_box(level, &x, &y, &w, &h, &full);
            if (full > h) slider_geometry(level, index, &sx, &middle);
            if (item->kind == ITEM_SLIDER && !(item->flags & ITEM_DISABLED) &&
                (full <= h || (item_at(level, event->x, event->y) == index && event->x >= sx))) {
                set_slider(item, Settings_Get(item->setting) + slider_step(item) * event->wheel);
                Settings_Save();
                return 1;
            }
        }
        if (open_menu >= 0) {
            int level = open_sub >= 0 && inside_drop(1, event->x, event->y) ? 1 : 0, x, y, w, h, full;
            drop_box(level, &x, &y, &w, &h, &full);
            if (full > h) {
                scroll[level] -= event->wheel * ITEM_H;
                if (level == 0) close_submenu();
                drop_box(level, &x, &y, &w, &h, &full);
                return 1;
            }
        }
        return open_menu >= 0 || event->y < MENU_H;
    case MENU_EVENT_MOTION: {
        int px = event->x, py = event->y;
        int bar = bar_item_at(px, py), was_hot = hot_item, was_sub = hot_sub, was_open = open_sub;
        int was_bar = hover_bar, index;
        if (grabbed) {
            slider_from_pointer(grabbed - 1, grabbed == 2 ? hot_sub : hot_item, px);
            return 1;
        }
        if (drag.active) {
            int x, y, w, h, full;
            if (!drag.dragged && (py - drag.y > ITEM_H / 3 || drag.y - py > ITEM_H / 3)) drag.dragged = 1;
            if (drag.dragged) {
                scroll[drag.level] = drag.from - (py - drag.y);
                if (drag.level == 0) {
                    close_submenu();
                    hot_item = -1;
                }
                hot_sub = -1;
                drop_box(drag.level, &x, &y, &w, &h, &full);
                return 1;
            }
        }
        hover_bar = bar;
        if (open_menu >= 0) {
            if (bar >= 0 && bar != open_menu) {
                close_menu(); /* dragging along the bar opens the next menu */
                open_menu = bar;
                return 1;
            }
            if (open_sub >= 0 && inside_drop(1, px, py)) {
                hot_sub = item_at(1, px, py);
            } else if ((index = item_at(0, px, py)) >= 0) {
                hot_item = index;
                if (menus[open_menu].items[index].kind == ITEM_SUBMENU) open_submenu(index);
                else close_submenu();
            } else if (open_sub < 0) {
                hot_item = -1; /* with a submenu open its parent row stays lit */
            } else {
                hot_sub = -1;
            }
            return was_hot != hot_item || was_sub != hot_sub || was_open != open_sub || was_bar != hover_bar;
        }
        return was_bar != hover_bar;
    }
    case MENU_EVENT_LEAVE:
        if (hover_bar >= 0 && open_menu < 0) {
            hover_bar = -1;
            return 1;
        }
        return 0;
    case MENU_EVENT_KEY_DOWN:
        LOG(LOG_MENU, "key %d open=%d sub=%d hot=%d/%d", (int)event->key, open_menu, open_sub, hot_item, hot_sub);
        if (open_menu < 0) {
            if (event->key == MENU_KEY_F10) {
                open_menu = 0;
                hot_item = step_item(0, -1, 1);
                keep_shown(0, hot_item);
                return 1;
            }
            return 0;
        }
        switch (event->key) {
        case MENU_KEY_ESCAPE:
            if (open_sub >= 0) close_submenu();
            else close_menu();
            return 1;
        case MENU_KEY_LEFT: case MENU_KEY_RIGHT: {
            int level = active_level(), *hot = active_hot();
            const Item *item = *hot >= 0 ? &level_menu(level)->items[*hot] : NULL;
            if (item && item->kind == ITEM_SLIDER) {
                /* The pointer can rest on a slider that is off: the key
                 * does nothing there. */
                if (!(item->flags & ITEM_DISABLED)) {
                    set_slider(item, Settings_Get(item->setting) + (event->key == MENU_KEY_RIGHT ? 1 : -1) * slider_step(item));
                    Settings_Save();
                }
            } else if (event->key == MENU_KEY_RIGHT && item && item->kind == ITEM_SUBMENU) {
                open_submenu(*hot);
                hot_sub = step_item(1, -1, 1);
                keep_shown(1, hot_sub);
            } else if (event->key == MENU_KEY_LEFT && open_sub >= 0) {
                close_submenu();
            } else {
                int next = (open_menu + (event->key == MENU_KEY_RIGHT ? 1 : MENU_COUNT - 1)) % MENU_COUNT;
                close_menu();
                open_menu = next;
                hot_item = step_item(0, -1, 1);
                keep_shown(0, hot_item);
            }
            return 1;
        }
        case MENU_KEY_UP:
            *active_hot() = step_item(active_level(), *active_hot() < 0 ? 0 : *active_hot(), -1);
            keep_shown(active_level(), *active_hot());
            return 1;
        case MENU_KEY_DOWN:
            *active_hot() = step_item(active_level(), *active_hot(), 1);
            keep_shown(active_level(), *active_hot());
            return 1;
        case MENU_KEY_ENTER: {
            int level = active_level(), *hot = active_hot();
            const Item *item = *hot >= 0 ? &level_menu(level)->items[*hot] : NULL;
            if (item && item->kind == ITEM_SUBMENU) {
                open_submenu(*hot);
                hot_sub = step_item(1, -1, 1);
                keep_shown(1, hot_sub);
            } else if (item && item->kind != ITEM_SLIDER) {
                activate(item, quit);
            }
            return 1;
        }
        default: return 1; /* the open menu swallows other keys */
        }
    case MENU_EVENT_KEY_UP:
        return open_menu >= 0;
    default:
        return 0;
    }
}
