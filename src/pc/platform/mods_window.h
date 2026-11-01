#ifndef MEMORIES_PC_MODS_WINDOW_H
#define MEMORIES_PC_MODS_WINDOW_H
#include "menu.h"
void ModsWindow_Init(void);
void ModsWindow_Resize(int width, int height);
void ModsWindow_Size(int *width, int *height);
void ModsWindow_Draw(MenuCanvas *canvas);
/* Returns 1 to close this window. All events belong exclusively to it. */
int ModsWindow_Event(const MenuEvent *event);
/* Whether `event` (already handled) changed what the window shows. Pointer
 * motion only matters while dragging a slider; backends skip the redraw
 * otherwise and draw at most once per batch of events. */
int ModsWindow_Redraws(const MenuEvent *event);
/* The window's close button: 1 to close now, 0 when unsaved changes need a
 * confirmation first (the window then asks). */
int ModsWindow_RequestClose(void);
/* For a panel inside the game's window, used with a finger (panel.h): the
 * search or profile field takes typing (1 or 2; 0 none), so the platform
 * shows its on-screen keyboard; a press at x, y holds a slider or a
 * scrollbar; a finger that went down at x, y and moved dy pixels scrolls
 * the list or the details under it. */
int ModsWindow_TextFocus(void);
/* Where a widget is, its centre in window pixels as the layout places it
 * now (0 when it is not shown): for tests and scripted input, as
 * ControlsWindow_Locate. ROW_SELECTED and ROW_FIRST are the selected
 * mod's row and the first row the list shows; OPTION + setting * 4 + 0..3 the selected mod's setting's -, value,
 * + and slider on the Settings tab. */
enum {
    MODS_UI_SEARCH = 1, MODS_UI_FILTER, MODS_UI_PROFILE, MODS_UI_SAVE, MODS_UI_LOAD, MODS_UI_TOGGLE, MODS_UI_BACK,
    MODS_UI_DEFAULTS, MODS_UI_CLOSE, MODS_UI_APPLY, MODS_UI_FOLDER, MODS_UI_ROW_SELECTED, MODS_UI_ROW_FIRST,
    MODS_UI_CHECK_SELECTED, MODS_UI_CHECK_FIRST, /* a row's [x], which switches its mod on or off */
    MODS_UI_HD,         /* HD pack... (where the platform can download it, hd_pack.h) */
    MODS_UI_REMOVE,     /* Remove, beside Import mod... (the selected mod's folder goes) */
    MODS_UI_MODSDIR,    /* Mods folder... (where the platform has a folder picker); while its question is up, Use no folder */
    MODS_UI_TAB = 20,   /* + 0 About, 1 Settings, 2 Compatibility */
    MODS_UI_ORDER = 30, /* + 0 -, 1 + */
    MODS_UI_OPTION = 100
};
int ModsWindow_Locate(int id, int *x, int *y);
/* Import mod... (a phone's panel, which has no folder to copy a mod into):
 * `pick` opens the system's file picker and returns at once (0, or -1 with
 * the reason); `picked`, asked once per ModsWindow_Tick while it is open,
 * says 0 while it is, 1 once a file was chosen, -1 when none was and -2 on
 * a failure (why); then, with "Importing..." shown, `fetch` copies the
 * chosen file to `path` (1; 0 with why), a .zip the window imports and
 * then removes. Without them (NULL, the default: every desktop) the panel
 * has no Import button and nothing else changes. Setting them clears what
 * an import cut short left in the mods folder. They also give the panel a
 * Remove button: an app cannot open its mods folder to delete a mod by hand,
 * so it deletes the selected mod's folder itself, after a question. */
void ModsWindow_SetImport(int (*pick)(char *why, size_t why_size), int (*picked)(char *why, size_t why_size),
                          int (*fetch)(char *path, size_t size, char *why, size_t why_size));
/* Mods folder...: `pick` opens the system's folder picker and returns at
 * once (0, or -1 with the reason); `picked`, asked once per ModsWindow_Tick
 * while it is open, says 0 while it is, 1 once a folder was chosen (its full
 * path in `path`), -1 when none was and -2 on a failure (why). The window
 * saves the choice (Mods_SetFolder), says why when that is refused, and
 * offers Apply & restart, since the folder is read at the next launch. It
 * shows the folder and why it was not read (Mods_FolderProblem), and can
 * choose none. Without them (NULL, the default: the developer window) there
 * is no such button. */
void ModsWindow_SetFolderPicker(int (*pick)(char *why, size_t why_size),
                                int (*picked)(char *path, size_t size, char *why, size_t why_size));
/* Where the folder can be read only with a right the player gives (Android's
 * All files access): `have` says whether it is held now (1) or not (0); after
 * a folder was chosen without it, the window asks, in a sentence, whether to
 * `ask`, which opens the system's page for it and returns at once (0, or -1
 * with why), and `asked` is polled with the ticks: 0 while the page is up, 1
 * granted, -1 refused, -2 failed (why). A folder stays chosen when the right
 * is refused; the window says how to give it, whenever it shows a folder it
 * cannot read. `can_ask` says whether the device has a page to give it on
 * (0: the window sends the player to Import mod... instead of asking), and
 * `right` names it ("All files access"; before Android 11 "Storage"); both may
 * be NULL (yes, and "All files access"). Called once at the start, after the
 * mods are scanned, so that the window knows what the start could read.
 * Without them (NULL, the default: every desktop) nothing is asked. */
void ModsWindow_SetFolderAccess(int (*have)(void), int (*ask)(char *why, size_t why_size),
                                int (*asked)(char *why, size_t why_size), int (*can_ask)(void),
                                const char *(*right)(void));
/* The app is back in front: the right may have been given meanwhile, in the
 * system's Settings; it is asked about at the next ModsWindow_Tick. Any thread. */
void ModsWindow_RefreshAccess(void);
/* Once per pump while the panel shows: an import's next step. 1 when the
 * window must be drawn again. */
int ModsWindow_Tick(void);
int ModsWindow_Grabs(int x, int y);
void ModsWindow_Drag(int x, int y, int dy);
#endif
