#ifndef MEMORIES_PC_MENU_H
#define MEMORIES_PC_MENU_H
#include <stddef.h>
#include <stdint.h>

/* The window's menu bar. It is composited in software into a buffer the
 * platform owns, so the port needs no toolkit and every backend shows the
 * same menu; text is FreeType through fontconfig's default sans-serif face,
 * with a built-in bitmap font when no face loads. Main thread only.
 *
 * File  > Save state, Load state, Exit
 * Audio > Volume slider over the whole mix (src/pc/audio/spu.c)
 * View  > Scale 1x-4x
 * Game  > Mods opens the searchable mod manager; Restart game goes back to
 *         the title screen once confirmed (title_jump.h)
 * Debug > development helpers (src/pc/debug)
 * Help  > update checks and this build's version (update_check.h); on
 *         Android, whether a crash's report is offered (android_report.c) */

typedef struct MenuCanvas {
    uint32_t *pixels; /* 0xAARRGGBB (alpha ignored unless `alpha`), row-major */
    int stride;       /* pixels per row */
    int width, height;
    /* 0: the canvas already holds the picture and the menu paints over it.
     * 1: the canvas is a transparent overlay the platform blends over the
     * picture itself; Menu_Draw clears its bounds to transparent first. */
    int alpha;
} MenuCanvas;

typedef enum {
    MENU_EVENT_NONE,
    MENU_EVENT_BUTTON_DOWN, /* button: 1 left, 2 middle, 3 right; x, y */
    MENU_EVENT_BUTTON_UP,
    MENU_EVENT_MOTION,      /* x, y */
    MENU_EVENT_WHEEL,       /* wheel: +1 up, -1 down; x, y */
    MENU_EVENT_LEAVE,       /* the pointer left the window */
    MENU_EVENT_KEY_DOWN,    /* key */
    MENU_EVENT_KEY_UP,
    MENU_EVENT_TEXT
} MenuEventType;

typedef enum {
    MENU_KEY_OTHER, MENU_KEY_ESCAPE, MENU_KEY_F10, MENU_KEY_LEFT, MENU_KEY_RIGHT, MENU_KEY_UP, MENU_KEY_DOWN,
    MENU_KEY_ENTER, MENU_KEY_TAB, MENU_KEY_BACKSPACE
} MenuKey;

typedef struct MenuEvent {
    MenuEventType type;
    int x, y, button, wheel;
    MenuKey key;
    char text[32];
} MenuEvent;

/* A code mod may name these (Menu_SetItemEnabled), so each keeps its number
 * from release to release: new ones go at the end, retired ones stay
 * (tools/pc/check_mod_abi.py). */
typedef enum {
    MENU_ITEM_SCALE_1 = 100,
    MENU_ITEM_SCALE_2,
    MENU_ITEM_SCALE_3,
    MENU_ITEM_SCALE_4,
    MENU_ITEM_SCALE_5,
    MENU_ITEM_SCALE_6,
    MENU_ITEM_FULLSCREEN = 120,
    MENU_ITEM_BORDERLESS,
    MENU_ITEM_SCALING_INTEGER,
    MENU_ITEM_SCALING_FIT,
    MENU_ITEM_SCALING_STRETCH,
    MENU_ITEM_ASPECT_4_3,
    MENU_ITEM_ASPECT_SQUARE,
    MENU_ITEM_ASPECT_WIDESCREEN,
    MENU_ITEM_FILTER,
    MENU_ITEM_VSYNC,
    MENU_ITEM_TITLE, /* Debug > Jump to > Title Screen: enabled once Main_Loop runs */
    MENU_ITEM_DECKS, /* Game > Deck slots: enabled where the deck can change (deck_menu.c) */
    MENU_ITEM_FILTER_NEAREST,
    MENU_ITEM_FILTER_LINEAR,
    MENU_ITEM_FILTER_SHARP,
    MENU_ITEM_HD_TEXT, /* Video > HD text: drawn by the OpenGL picture pass at Internal 2x and up */
    MENU_ITEM_HD_HUD,  /* retired (a code mod may still name it): the menu has no such item */
    MENU_ITEM_OPPONENT_NAME, /* View > Opponent's name for COM: drawn by the OpenGL picture pass at
                              * Internal 2x and up (Menu_SetHdPicture), by the software GPU at 1x */
    MENU_ITEM_RESTART, /* Game > Restart game: asks, then goes back as MENU_ITEM_TITLE does */
    MENU_ITEM_PGXP, /* Video > Precise geometry: needs the OpenGL picture pass at Internal 2x and up,
                     * no software-GPU fallback (Menu_SetHdPicture) */
    MENU_ITEM_COLOR, /* Video > Color, and Effects > CRT scanlines and Reduce flashes: the desktop
                      * presenter's (present_pass.c, Menu_SetPresentPass) */
    MENU_ITEM_CRT,
    MENU_ITEM_FLASH,
    MENU_ITEM_XBR /* Video > Effects > xBR: the desktop presenter's, or the OpenGL picture
                   * pass's at Internal 2x and up */
} MenuItemId;

/* The stored settings (settings.txt in the user directory, see paths.h;
 * MEMORIES_SETTINGS elsewhere; MEMORIES_VOLUME and MEMORIES_SCALE override),
 * and the mods they say are applied. Applied whether or not a window opens,
 * so headless runs honour them too. */
void Menu_LoadSettings(void);
int Menu_Height(void);
/* Prepare fonts and layout; needs no display. */
void Menu_Init(void);
/* The menu's size multiple: bar, rows, font and HUD scale together. The
 * platform sets it from the setting, or from the window height when that is
 * 0 (Menu_AutoScale), before it sizes a window or lays one out. */
int Menu_Scale(void);
void Menu_SetScale(int scale);
int Menu_AutoScale(int window_h);
/* Draw the bar and, when open, its menu. */
void Menu_Draw(MenuCanvas *canvas);
/* Text is UTF-8: a character the face has no glyph for, or a byte that is
 * not UTF-8, is drawn as "?". */
void Menu_DrawText(MenuCanvas *canvas, int x, int y, const char *text, uint32_t color);
int Menu_TextWidth(const char *text);
/* Auxiliary windows can fit their UI without changing the game menu scale. */
void Menu_DrawTextScaled(MenuCanvas *canvas, int x, int y, const char *text, uint32_t color, int scale);
int Menu_TextWidthScaled(const char *text, int scale);

/* For code that cuts UTF-8 text to fit, so a cut never splits a character.
 * Menu_TextBack: where the character that ends at byte `at` starts.
 * Menu_TextFit: the longest prefix of at most `length` bytes that ends
 * between characters (`text` must hold `length` + 1 bytes or end sooner).
 * Menu_TextTrim: drops a last character cut short (by snprintf, say).
 * (No <string.h> here: game units that declare their own include this.) */
static inline size_t Menu_TextBack(const char *text, size_t at)
{
    while (at && ((unsigned char)text[--at] & 0xC0) == 0x80) {}
    return at;
}
static inline size_t Menu_TextFit(const char *text, size_t length)
{
    size_t end = 0;
    while (end < length && text[end]) end++;
    if (end < length || !text[end]) return end;
    while (length && ((unsigned char)text[length] & 0xC0) == 0x80) length--;
    return length;
}
static inline void Menu_TextTrim(char *text)
{
    size_t length = 0, lead;
    while (text[length]) length++;
    lead = Menu_TextBack(text, length);
    unsigned char c = (unsigned char)text[lead];
    size_t need = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    if (length && length - lead < need) text[lead] = '\0';
}
void Menu_SetVisible(int visible);
int Menu_IsOpen(void);
/* Opens the bar's first menu, or the one at `which` (MENU_BAR_*), with no
 * row lit: a touch screen's MENU and STATES buttons (touch_pad.h). Nothing
 * while a notice is up. */
enum { MENU_BAR_FILE = 0, MENU_BAR_GAME = 3 };
void Menu_Open(void);
void Menu_OpenAt(int which);
/* On a touch screen, the smallest height a finger hits in window pixels
 * (48 dp): the bar, the rows and a notice's buttons are at least that tall.
 * 0, the default, keeps the mouse's sizes. */
void Menu_SetTouchTarget(int pixels);
/* That height (0 with a mouse): the Mods and Controls panels drawn inside
 * the window (panel.h) size their rows and buttons by it too. */
int Menu_TouchTarget(void);
/* The part of the window what is drawn over the picture keeps within (the
 * save slot and deck slot menus): across, between `left` and `right` (the
 * touch controls hold the sides while they show; the whole width when
 * right <= left); down, from `top` (below the bar when negative, the
 * default; 0 where the bar is drawn over the picture and hidden).
 * Menu_OverlayArea gives it for a canvas. */
void Menu_SetOverlayArea(int left, int right, int top);
void Menu_OverlayArea(const MenuCanvas *canvas, int *left, int *right, int *top);
/* The rectangle the menu currently covers (the bar, plus an open menu). */
void Menu_Bounds(int *x, int *y, int *w, int *h);
/* Handle one event. Returns 1 when the menu took it (and must be redrawn)
 * and the game must not see it; sets *quit when File > Exit was chosen. */
int Menu_Event(const MenuEvent *event, int *quit);
/* Enable or disable an item by its backend-independent id. Disabled items
 * are dimmed, cannot be selected with the keyboard and ignore clicks. */
void Menu_SetItemEnabled(int id, int enabled);
/* What the platform lacks, dimmed once after Menu_Init: 0 for `windows`
 * dims Game > Controls... and Mods (second windows), for `window_modes`
 * Video > Window scale, Fullscreen and Borderless, for `update_check` Help's
 * update check rows. The SDL backend on Android passes 1, 0, 0: Mods and
 * Controls open as panels inside the window there (panel.h). */
void Menu_SetPlatformItems(int windows, int window_modes, int update_check);
/* Whether the backend runs the OpenGL picture pass (gl_picture.h). Without
 * it, or at console resolution, the Video menu's HD items could show
 * nothing: they are dimmed with the reason beside them. */
void Menu_SetHdPicture(int on);
/* Whether the backend runs the present pass (present_pass.h: the desktop
 * OpenGL presenter). Without it Video > Color, CRT scanlines, Reduce
 * flashes and Sharp bilinear would do nothing, and xBR needs the OpenGL
 * picture pass at Internal 2x: they are dimmed with the reason beside
 * them. On until the backend says otherwise. */
void Menu_SetPresentPass(int on);

/* A notice over the middle of the picture, drawn with the menu (also while
 * the bar is hidden in fullscreen): a title, text wrapped to fit (newlines
 * break it) and up to MENU_NOTICE_BUTTONS buttons in a row. While it is up
 * it has the window's mouse and keyboard: Left and Right (or Tab) move the
 * focus, Enter presses the focused button, Escape the last one. `chosen`
 * gets the button's index after the notice has closed, and may show
 * another; *quit ends the game as File > Exit does. One at a time: a new
 * notice replaces the one shown. Main thread only. */
#define MENU_NOTICE_BUTTONS 4
void Menu_ShowNotice(const char *title, const char *text, const char *const *buttons, int count, int focus,
                     void (*chosen)(int button, int *quit));
void Menu_CloseNotice(void);
int Menu_NoticeShown(void);
/* A controller's newly pressed pad buttons (PS1 bits) for the notice shown:
 * the D-pad moves the focus, Cross presses the focused button and Circle the
 * last one, exchanged with View > Japanese buttons. Returns 1 when the
 * notice took them. */
int Menu_NoticePad(uint16_t pressed, int *quit);
/* Nonzero once after a notice appeared, changed or closed other than by an
 * event: the backend repaints the menu then. */
int Menu_TakeChanged(void);

/* Provided by the platform for the Video menu. */
int Platform_Scale(void);
void Platform_SetScale(int scale);
#endif
