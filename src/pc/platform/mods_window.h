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
    MODS_UI_TAB = 20,   /* + 0 About, 1 Settings, 2 Compatibility */
    MODS_UI_ORDER = 30, /* + 0 -, 1 + */
    MODS_UI_OPTION = 100
};
int ModsWindow_Locate(int id, int *x, int *y);
/* Import mod... (a phone's panel, which has no folder to copy a mod into):
 * `pick` opens the system's file picker and returns at once (0, or -1 with
 * the reason); `picked`, asked once per ModsWindow_Tick while it is open,
 * says 0 while it is, 1 once a file was chosen, -1 when none was and -2 on
 * a failure (why); then `fetch` copies the chosen file to `path` (1; 0 with
 * why), a .zip the window imports and then removes. The copy, the reading
 * and the unpacking run on a thread of their own (import_job.h), `fetch`
 * with it: it reports its progress and takes a Stop as ImportJobFetch says
 * (platform.h, Platform_FetchModZip), and the panel shows how far the import
 * is, with a Stop button. Without them (NULL, the default: every desktop) the panel
 * has no Import button and nothing else changes. Setting them clears what
 * an import cut short left in the mods folder. They also give the panel a
 * Remove button: an app cannot open its mods folder to delete a mod by hand,
 * so it deletes the selected mod's folder itself, after a question. */
void ModsWindow_SetImport(int (*pick)(char *why, size_t why_size), int (*picked)(char *why, size_t why_size),
                          int (*fetch)(char *path, size_t size, char *why, size_t why_size, unsigned *done,
                                       unsigned *total, const int *cancel));
/* A .zip the player opened with the game (Android's "Open with" and Share):
 * the import Import mod... runs, with the picker's answer already `fetch`'s
 * file, which asks before it imports (the Replace question, or "Import X
 * from the .zip you opened with the game?"). `refusal`, when the platform
 * could not take the file, is shown instead. 1 when the import started; 0
 * when it did not (the message says why, but for a busy panel, which is left
 * as it is). The panel is busy while a
 * question is up or an import or the HD pack runs (ModsWindow_ImportBusy;
 * `shown`: the panel is open, which a question left open by a closed panel
 * does not count): the caller waits for it to end rather than open the
 * panel on it, or ask. */
int ModsWindow_ImportBusy(int shown);
int ModsWindow_ImportOpened(const char *refusal);
/* Once per pump while the panel shows: an import's next step. 1 when the
 * window must be drawn again. */
int ModsWindow_Tick(void);
int ModsWindow_Grabs(int x, int y);
void ModsWindow_Drag(int x, int y, int dy);
#endif
