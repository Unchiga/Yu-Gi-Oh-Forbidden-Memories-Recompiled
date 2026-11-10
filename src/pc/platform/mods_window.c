/* Searchable mod library, details, staged settings and named profiles.
 * The same bounded layout drives painting and input on SDL and X11, in a
 * window of its own or as a panel inside the game's window (panel.h).
 *
 * With a finger (Menu_TouchTarget: a panel on a phone or tablet) the rows
 * and buttons are at least a finger's target tall, the unit stays the
 * menu's (the screen's density), the window's title and keyboard hints give
 * their room to the list, and the counts move to the footer's message line.
 * Where the list and the details do not fit side by side (a phone) they are
 * two pages: the list, and the selected mod's details with a Back button.
 * A drag scrolls what it starts on (ModsWindow_Drag); a slider or a
 * scrollbar is held at once instead (ModsWindow_Grabs). */
#include "mods_window.h"
#include "text_edit.h"
#include "../../types.h"
#include "pc/compat/fs.h"
#include "pc/mods/hd_pack.h"
#include "pc/mods/import.h"
#include "pc/mods/import_job.h"
#include "pc/mods/json.h"
#include "pc/mods/mods.h"
#include "pc/mods/overlap.h"
#include "paths.h"
#include "platform.h"
#include "settings.h"
#include "update.h"
#include <ctype.h>
#include <png.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BG 0x181b20u
#define PANEL 0x22262du
#define EDGE 0x363d49u
#define TEXT 0xe9edf3u
#define DIM 0x9aa8bau
#define BLUE 0x639cffu
#define ACCENT 0x356dd1u
#define GREEN 0x77d6a0u
#define WARN 0xf4bd6au
#define RED 0xff9292u

typedef struct {
    int x, y, w, h;
} Rect;
typedef struct {
    Rect search, filter, list, list_bar, detail, toggle, tabs[3], view, bar, apply, close, profile, save, load,
        order[2], defaults, folder, remove, back, hd, modsdir;
    int name_y, meta_y; /* the middles of the details' name and metadata lines */
    int lift;           /* with a finger: how far a long message raises the footer's top */
} Layout;
static int width, height, unit, selected, scroll, detail_scroll, tab, filter, focus, pending;
/* touch: the finger's target in pixels (0 with a mouse). compact: touch, and
 * too small for the list beside the details, so `page` shows one of them
 * (0 the list, 1 the details). drag_rest: a drag's pixels short of a row. */
static int touch, compact, page, drag_rest;
/* The Modpack tab (below, before ModsWindow_Draw). */
#define PACKS_MAX 64
/* Export's form (pack_form), its fields (pack_focus 1 name, 2 version, 3 author, 4 description) */
static int pack_form, pack_focus, pack_lock;
static int export_scroll, export_sel;
static int pack_detail_scroll, pack_detail_max, description_scroll, description_max;
static int pack_form_scroll, pack_form_h; /* the Export form scrolled, and its whole height */
static int pack_preview_h;                 /* its Preview's height at the last draw */
static TextEdit description_edit = {0, 0, -1};
static int description_drag;
static int (*clipboard_read)(char *, size_t), (*clipboard_write)(const char *);
void ModsWindow_SetClipboard(int (*read)(char *, size_t), int (*write)(const char *))
{
    clipboard_read = read;
    clipboard_write = write;
}
static char pack_query[96];
static int pack_filter; /* 0 all, 1 selected, 2 unselected; focus 5 is search */
static int packs, pack_sel, pack_scroll, pack_count, pack_mods[PACKS_MAX], pack_ask; /* pack_ask: 2 Restart */
static int pack_restart(void);
static int wanted[MODS_MAX], ranks[MODS_MAX], *values[MODS_MAX], counts[MODS_MAX];
static int slider_drag = -1, bar_drag, bar_grab; /* bar_drag: 1 the list's scrollbar, 2 the details' */
/* Import mod... (ModsWindow_SetImport): the platform's picker, the step an
 * import is at (1 the picker is open, 2 the copy and the reading run on
 * the job's thread, 3 the unpacking does; import_job.h), the share of the
 * step last shown (a new one draws the window again), the .zip, and the
 * import that waits for Replace or Cancel (pending 3) or is unpacked. */
static int (*import_pick)(char *, size_t), (*import_picked)(char *, size_t),
    (*import_fetch)(char *, size_t, char *, size_t, unsigned *, unsigned *, const int *);
static int import_step, import_shown = -1;
static char import_zip[1024];
static ModsImport *import_waiting;
/* A .zip the player opened with the game (ModsWindow_ImportOpened) is
 * imported as the picked one is, but asks first, whether it replaces a mod
 * or not: import_outside while the import is under way, import_plain while
 * the question is not a Replace one. */
static int import_outside, import_plain;
/* Remove (touch panels with Import mod...): the mod its question (pending
 * 5) is about. */
static int remove_target = -1;
/* Mods folder... (ModsWindow_SetFolderPicker): the platform's folder picker,
 * whether it is open (folder_step 1), and the folder the game was started
 * with: a choice is read at the next launch, so a different one is a restart
 * to offer (folder_restart). Pending 6 is the question that asks what to do
 * with the folder. */
static int (*folder_pick)(char *, size_t), (*folder_picked)(char *, size_t, char *, size_t);
static int folder_step, launch_known;
static char launch_folder[1024];
/* Where the right to read the folder is the player's to give (Android's All
 * files access, ModsWindow_SetFolderAccess): whether it is held now, whether
 * the folder the game started with could not be read (so that the right,
 * given since, is a restart too), and the question that asks for it (pending
 * 7; folder_step 2 while the system's page is up). */
static int (*access_have)(void), (*access_ask)(char *, size_t), (*access_asked)(char *, size_t);
static int (*access_can_ask)(void);
static const char *(*access_right)(void);
static int access_now = 1, launch_unread, launch_access = -1;
static volatile int access_recheck_now; /* the app came back to the front (ModsWindow_RefreshAccess) */
static int folder_restart(void);
static void folder_launch(void);
static const char *access_hint(void);
static void put(char *out, size_t size, const char *format, ...);
/* Where a mod comes from, as the details say it (Mods_Origin). */
static const char *origin_text(const char *origin) { return !strcmp(origin, "folder") ? "from your mods folder" : origin; }
/* HD pack... (hd_pack.h, where the platform has a network for it): what
 * the window did with the job (0 nothing, 1 asked GitHub, 2 asks the
 * player (pending 4), 3 downloads, 4 unpacks), and the share of the step
 * last shown, in thousandths (a new one draws the window again). */
static int hd_step, hd_shown = -1, hd_clearing;
static char query[96], profile[65] = "Default", status[512];
static const char *filters[] = {"All mods", "Enabled", "Disabled", "Issues"};
static const char *pad_names[] = {"Select", "L3", "R3", "Start", "Up",       "Right",  "Down",  "Left",
                                  "L2",     "R2", "L1", "R1",    "Triangle", "Circle", "Cross", "Square"};
#define BAR (10 * unit)
#define LINE (22 * unit)
/* The list's row pitch, and the settings' - value + buttons. */
#define PITCH (touch ? max(58 * unit, touch + 2 * unit) : 58 * unit)
#define CONTROL (touch ? max(28 * unit, touch) : 28 * unit)
static int width_text(const char *s) { return Menu_TextWidthScaled(s, unit); }
static Rect rect(int x, int y, int w, int h)
{
    Rect r = {x, y, w, h};
    return r;
}
static int inside(Rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
static int max(int a, int b) { return a > b ? a : b; }
static int min(int a, int b) { return a < b ? a : b; }
static int wrap(MenuCanvas *c, int x, int y, int w, const char *s, unsigned color);
/* The lines a status too long for one (a save's path and reason) needs
 * beyond the first; the footer grows by them. */
static int status_extra(void)
{
    if (!*status || width_text(status) <= width - 40 * unit)
        return 0;
    return min(max(wrap(NULL, 0, 0, width - 40 * unit, status, 0) / LINE, 1), 6) - 1;
}
static int filter_width(void)
{
    int widest = 0;
    for (int i = 0; i < 4; i++)
        widest = max(widest, width_text(filters[i]));
    return widest + 24 * unit;
}
static Rect none(void) { return rect(0, 0, 0, 0); }
static int defaults_width(void) { return max(150 * unit, width_text("Restore defaults") + 24 * unit); }
/* On a phone's page the settings' count and Restore defaults scroll with
 * the settings, as their first row, rather than taking a row of the view. */
static int toolbar_in_body(void) { return touch && compact && tab == 1 && selected >= 0 && counts[selected]; }
static int options_top(void) { return -detail_scroll + (toolbar_in_body() ? touch + 8 * unit : 0); }
/* With a finger: one row at the top (search and filter; the profile, Save
 * and Load), one at the bottom (the message, Close and Apply), the list and
 * the details between, side by side or one page at a time. */
static void layout_touch(Layout *l)
{
    int p = 12 * unit, g = 8 * unit, t = touch, footer = height - p - t, left = compact ? width : width * 36 / 100;
    int top = p + t + g, save_w = max(64 * unit, width_text("Save") + 24 * unit), right, x, view_top;
    int defaults_w = defaults_width();
    memset(l, 0, sizeof(*l));
    l->apply = rect(width - p - 150 * unit, footer, 150 * unit, t);
    l->close = rect(l->apply.x - g - 94 * unit, footer, 94 * unit, t);
    /* The footer's slot for the mods' files, left of Close (footer_left()
     * gives the message the room up to it): Import mod..., where the
     * platform has a file picker for it (an app has no folder window to
     * open: Platform_OpenFolder fails on Android); on the list's page. */
    l->folder = none();
    if (import_pick && pending != 6 && (!compact || page == 0)) {
        int w = max(120 * unit, width_text("Import mod...") + 24 * unit);
        l->folder = rect(l->close.x - g - w, footer, w, t);
    }
    /* Remove, where Import is (an app cannot open its mods folder to delete
     * one by hand): beside the selected mod's details, so on a phone it is
     * on their page, where Import is not; else left of Import. While its
     * question is up (pending 5) the answer is the Apply button, which reads
     * Remove, so this one steps aside rather than showing a second, dead one. */
    l->remove = none();
    if (import_pick && selected >= 0 && pending != 5 && pending != 6 && (!compact || page == 1)) {
        int w = max(90 * unit, width_text("Remove") + 24 * unit), edge = l->folder.w ? l->folder.x : l->close.x;
        l->remove = rect(edge - g - w, footer, w, t);
    }
    /* Mods folder..., where the platform has a folder picker for it: the
     * leftmost of the footer's buttons on the list's page, if the message
     * keeps room beside them (a few words: counts or a line that wraps). */
    l->modsdir = none();
    if (folder_pick && pending != 7 && (!compact || page == 0)) {
        int w = max(130 * unit, width_text("Mods folder...") + 24 * unit);
        int edge = l->remove.w ? l->remove.x : l->folder.w ? l->folder.x : l->close.x;
        if (edge - g - w >= 140 * unit)
            l->modsdir = rect(edge - g - w, footer, w, t);
    }
    {   /* A message of two to four lines beside the buttons (a refusal, a
         * mod that cannot run here) raises the footer's top instead of
         * being cut: the list and the details end above it. */
        int room = (l->modsdir.w ? l->modsdir.x : l->remove.w ? l->remove.x : l->folder.w ? l->folder.x : l->close.x) -
                   20 * unit;
        int lines = *status ? min(4, max(1, wrap(NULL, 0, 0, room, status, 0) / LINE)) : 1;
        l->lift = max(0, lines * LINE + 6 * unit - t);
        footer -= l->lift; /* where the list and the details end; the buttons are placed */
    }
    if (!compact || page == 0) {
        int fw = filter_width();
        x = width - p - 2 * save_w - g;
        l->save = rect(x, p, save_w, t);
        l->load = rect(x + save_w + g, p, save_w, t);
        /* HD pack..., where the platform can download it: left of Save,
         * the footer being Import's and the message's (its progress and
         * its errors take two lines on a phone). The search field (on a
         * phone) or the profile field gives it the room. */
        if (HdPack_Available()) {
            int w = max(width_text("HD pack...") + 24 * unit, width_text("Stop") + 24 * unit);
            l->hd = rect(x - g - w, p, w, t);
            x = l->hd.x;
        }
        if (compact) { /* across the whole width: search, filter, profile, Save, Load */
            l->profile = rect(x - g - 120 * unit, p, 120 * unit, t);
            l->filter = rect(l->profile.x - g - fw, p, fw, t);
            l->search = rect(p, p, l->filter.x - g - p, t);
            l->list = rect(p, top, width - 2 * p, footer - g - top);
        } else {
            l->filter = rect(left - fw, p, fw, t);
            l->search = rect(p, p, l->filter.x - g - p, t);
            l->profile = rect(left + 16 * unit, p, max(80 * unit, x - g - left - 16 * unit), t);
            l->list = rect(p, top, left - p, footer - g - top);
        }
        l->list_bar = rect(l->list.x + l->list.w - BAR - 3 * unit, l->list.y + 4 * unit, BAR, l->list.h - 8 * unit);
    }
    if (compact && page == 0)
        return;
    l->detail = compact ? rect(p, p, width - 2 * p, footer - g - p)
                        : rect(left + 16 * unit, top, width - left - 16 * unit - p, footer - g - top);
    right = l->detail.x + l->detail.w - g;
    {
        int y = l->detail.y + g;
        l->back = compact ? rect(l->detail.x + g, y, t, t) : none();
        l->toggle = rect(right - 120 * unit, y, 120 * unit, t);
        l->order[1] = rect(l->toggle.x - g - t, y, t, t);
        l->order[0] = rect(l->order[1].x - 40 * unit - t, y, t, t);
        l->name_y = y + t / 2 - 10 * unit;
        l->meta_y = y + t / 2 + 11 * unit;
        for (int i = 0; i < 3; i++)
            l->tabs[i] = rect(l->detail.x + i * l->detail.w / 3, y + t + g,
                              (i + 1) * l->detail.w / 3 - i * l->detail.w / 3, t);
        view_top = y + 2 * (t + g);
    }
    l->defaults = rect(right - defaults_w, view_top, defaults_w, t);
    if (tab == 1 && selected >= 0 && counts[selected] && !compact)
        view_top += t + g;
    l->view = rect(l->detail.x + 16 * unit, view_top, l->detail.w - 22 * unit,
                   max(0, l->detail.y + l->detail.h - 8 * unit - view_top));
    l->bar = rect(l->view.x + l->view.w - BAR, l->view.y, BAR, l->view.h);
    if (compact) { /* in the scrolled settings, where the view shows it */
        int top = l->view.y - detail_scroll, bottom = min(top + t, l->view.y + l->view.h);
        top = max(top, l->view.y);
        l->defaults = bottom > top ? rect(l->view.x + l->view.w - BAR - 12 * unit - defaults_w, top, defaults_w,
                                          bottom - top)
                                   : none();
    }
}
static void layout(Layout *l)
{
    if (touch) {
        layout_touch(l);
        return;
    }
    int p = 20 * unit, gap = 16 * unit, left = width * 36 / 100, footer = height - 72 * unit - status_extra() * LINE,
        top = 142 * unit, right, view_top;
    l->search = rect(p, 64 * unit, left - p, 32 * unit);
    l->filter = rect(p, 140 * unit, left - p, 28 * unit); /* below the Modpack button (104) */
    l->list = rect(p, top + 36 * unit, left - p, footer - top - 48 * unit);
    l->list_bar = rect(l->list.x + l->list.w - BAR - 3 * unit, l->list.y + 4 * unit, BAR, l->list.h - 8 * unit);
    l->detail = rect(left + gap, top, width - left - gap - p, footer - top - 12 * unit);
    l->profile = rect(left + gap, 64 * unit, max(80 * unit, width - left - gap - p - 160 * unit), 32 * unit);
    l->save = rect(width - p - 152 * unit, 64 * unit, 72 * unit, 32 * unit);
    l->load = rect(width - p - 72 * unit, 64 * unit, 72 * unit, 32 * unit);
    /* Name and metadata on the left of the details, switch and order on the right. */
    right = l->detail.x + l->detail.w - 16 * unit;
    l->toggle = rect(right - 120 * unit, l->detail.y + 10 * unit, 120 * unit, 28 * unit);
    l->order[0] = rect(right - 120 * unit, l->detail.y + 44 * unit, 28 * unit, 26 * unit);
    l->order[1] = rect(right - 28 * unit, l->detail.y + 44 * unit, 28 * unit, 26 * unit);
    for (int i = 0; i < 3; i++)
        l->tabs[i] = rect(l->detail.x + i * l->detail.w / 3, l->detail.y + 80 * unit,
                          (i + 1) * l->detail.w / 3 - i * l->detail.w / 3, 32 * unit);
    view_top = l->detail.y + 124 * unit;
    l->defaults = rect(right - 150 * unit, view_top, 150 * unit, 28 * unit);
    if (tab == 1 && selected >= 0 && counts[selected])
        view_top += 40 * unit; /* the settings' toolbar stays put above the scrolling list */
    l->view = rect(l->detail.x + 16 * unit, view_top, l->detail.w - 22 * unit,
                   max(0, l->detail.y + l->detail.h - 8 * unit - view_top));
    l->bar = rect(l->view.x + l->view.w - BAR, l->view.y, BAR, l->view.h);
    l->apply = rect(width - p - 150 * unit, height - 48 * unit, 150 * unit, 30 * unit);
    l->close = rect(width - p - 252 * unit, height - 48 * unit, 94 * unit, 30 * unit);
    if (packs) /* no footer Restart there: Close keeps the right margin */
        l->close.x = width - p - l->close.w;
    l->folder = rect(width - p - 418 * unit, height - 48 * unit, 158 * unit, 30 * unit);
    l->modsdir = folder_pick ? rect(l->folder.x - 8 * unit - 130 * unit, height - 48 * unit, 130 * unit, 30 * unit)
                             : none();
    l->back = rect(0, 0, 0, 0);
    l->hd = none(); /* a phone's panel only (layout_touch) */
    l->remove = none(); /* so is Remove */
    l->name_y = l->detail.y + 24 * unit;
    l->meta_y = l->detail.y + 57 * unit;
}
static const char *str(const JsonValue *v, const char *key, const char *fallback)
{
    return Json_String(Json_Member(v, key), fallback);
}
static int num(const JsonValue *v, const char *key, int fallback)
{
    return (int)Json_Number(Json_Member(v, key), fallback);
}
static int contains(const char *text, const char *part)
{
    size_t i, n = strlen(part);
    if (!n)
        return 1;
    for (; *text; text++) {
        for (i = 0; i < n && text[i] && tolower((unsigned char)text[i]) == tolower((unsigned char)part[i]); i++) {
        }
        if (i == n)
            return 1;
    }
    return 0;
}
static int visible(int mod)
{
    if (Mods_Removed(mod))
        return 0;
    if (filter == 1 && !wanted[mod])
        return 0;
    if (filter == 2 && wanted[mod])
        return 0;
    if (filter == 3 && !Mods_Status(mod)[0])
        return 0;
    return contains(Mods_Name(mod), query) || contains(Mods_Id(mod), query) ||
           contains(Mods_Metadata(mod, "author"), query);
}
static int shown(int row)
{
    for (int i = 0; i < Mods_Count(); i++)
        if (visible(i) && row-- == 0)
            return i;
    return -1;
}
static int rows(void)
{
    Layout l;
    layout(&l);
    return max(1, l.list.h / PITCH);
}
static int shown_count(void)
{
    int n = 0;
    for (int i = 0; i < Mods_Count(); i++)
        n += visible(i);
    return n;
}
static int changed(void)
{
    for (int i = 0; i < Mods_Count(); i++) {
        char key[160];
        snprintf(key, sizeof(key), "mod.%s.order", Mods_Id(i));
        if (wanted[i] != Mods_Enabled(i) || ranks[i] != Settings_GetNamed(key, num(Mods_Manifest(i), "priority", 0)))
            return 1;
        for (int j = 0; j < counts[i]; j++)
            if (values[i][j] != Mods_OptionValue(i, j))
                return 1;
    }
    return 0;
}
/* What the window stages for mod `i`, as the mod is now: at the start, and
 * for a mod an import adds or changes. */
static void adopt(int i)
{
    char key[160];
    wanted[i] = Mods_Enabled(i);
    snprintf(key, sizeof(key), "mod.%s.order", Mods_Id(i));
    ranks[i] = Settings_GetNamed(key, num(Mods_Manifest(i), "priority", 0));
    free(values[i]);
    counts[i] = Mods_OptionCount(i);
    values[i] = calloc((size_t)max(1, counts[i]), sizeof(int));
    if (!values[i]) {
        counts[i] = 0;
        snprintf(status, sizeof(status), "Could not allocate mod settings");
        return;
    }
    for (int j = 0; j < counts[i]; j++)
        values[i][j] = Mods_OptionValue(i, j);
}
void ModsWindow_Init(void)
{
    unit = Menu_Scale();
    touch = Menu_TouchTarget();
    folder_launch();
    compact = page = drag_rest = 0;
    width = 920 * unit;
    height = 640 * unit;
    slider_drag = -1;
    bar_drag = 0;
    selected = -1;
    for (int i = Mods_Count() - 1; i >= 0; i--)
        if (!Mods_Removed(i))
            selected = i; /* the first one the list shows */
    scroll = detail_scroll = tab = filter = focus = pending = 0;
    packs = pack_sel = pack_scroll = pack_ask = pack_form = pack_focus = 0;
    description_drag = 0;
    TextEdit_Move(&description_edit, 0, 0);
    pack_query[0] = 0;
    pack_filter = 0;
    remove_target = -1;
    query[0] = status[0] = 0;
    for (int i = 0; i < MODS_MAX; i++) {
        free(values[i]);
        values[i] = NULL;
        counts[i] = 0;
    }
    for (int i = 0; i < Mods_Count(); i++)
        adopt(i);
    {   /* a mods folder the game could not read this time */
        char path[1024];
        access_now = access_have ? access_have() : 1;
        access_recheck_now = 0;
        if (!Mods_Folder(path, sizeof(path)) && !access_now)
            /* a phone's footer has few lines: the hint, without the path */
            put(status, sizeof(status), touch ? "%.0s%s" : "%.300s: %s", path, access_hint());
        else if (!Mods_Folder(path, sizeof(path)) && *Mods_FolderProblem())
            put(status, sizeof(status), "%s", Mods_FolderProblem());
    }
    if (Mods_ModpackProblem()[0])
        put(status, sizeof(status), "%s", Mods_ModpackProblem());
    else if (Mods_ModpackLocked())
        put(status, sizeof(status), "Modpack %s: its author locked its settings.", Mods_ModpackLaunched());
    if (import_waiting && import_step != 3) { /* a Replace question the last showing left open */
        Mods_ImportClose(import_waiting);
        import_waiting = NULL;
        import_plain = 0;
        Mods_ImportRemoveTree(import_zip);
    }
    if (hd_step == 2) { /* the HD pack's Download question, left open */
        HdPack_Reset();
        hd_step = 0;
    }
    hd_shown = import_shown = -1;
}
void ModsWindow_Resize(int w, int h)
{
    touch = Menu_TouchTarget();
    if (touch) {
        /* The whole screen: the menu's unit (the density) unless even a
         * phone's page cannot hold it; side by side from a tablet's size. */
        width = max(320, w);
        height = max(240, h);
        unit = Menu_Scale();
        while (unit > 1 && (width < 560 * unit || height < 330 * unit))
            unit--;
        compact = width < 760 * unit || height < 600 * unit;
        return;
    }
    width = max(480, w);
    height = max(360, h);
    unit = Menu_Scale();
    while (unit > 1 && (width < 700 * unit || height < 460 * unit))
        unit--;
}
void ModsWindow_Size(int *w, int *h)
{
    *w = width;
    *h = height;
}
static void fill(MenuCanvas *c, Rect r, unsigned color)
{
    int x0 = max(0, r.x), y0 = max(0, r.y), x1 = r.x + r.w, y1 = r.y + r.h;
    if (!c)
        return;
    if (x1 > c->width)
        x1 = c->width;
    if (y1 > c->height)
        y1 = c->height;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            c->pixels[y * c->stride + x] = 0xff000000u | color;
}
static void text(MenuCanvas *c, int x, int y, int w, const char *s, unsigned color)
{
    char line[512];
    size_t n = strlen(s);
    if (!c || w <= 0)
        return;
    if (n >= sizeof(line))
        n = Menu_TextFit(s, sizeof(line) - 1);
    memcpy(line, s, n);
    line[n] = 0;
    if (width_text(line) > w) {
        while (n && width_text(line) + width_text("...") > w)
            line[n = Menu_TextBack(line, n)] = 0;
        if (n + 3 < sizeof(line))
            strcat(line, "...");
    }
    Menu_DrawTextScaled(c, x, y, line, color, unit);
}
static void button(MenuCanvas *c, Rect r, const char *label, int accent)
{
    /* A label that does not fit the usual margins (the Load order's "+" in
     * its 28-pixel button) is centred in what there is, not cut to "...". */
    int pad = max(2 * unit, min(10 * unit, (r.w - width_text(label)) / 2));
    fill(c, r, accent ? ACCENT : EDGE);
    text(c, r.x + pad, r.y + r.h / 2, r.w - 2 * pad, label, TEXT);
}
static void centred(MenuCanvas *c, Rect r, const char *s, unsigned color)
{
    text(c, r.x + max(6 * unit, (r.w - width_text(s)) / 2), r.y + r.h / 2, r.w - 12 * unit, s, color);
}
/* Word-wraps `s` into `w` pixels, its first line's top at `y`, and returns the
 * top of the line after it. Without a canvas it only measures. */
static int wrap(MenuCanvas *c, int x, int y, int w, const char *s, unsigned color)
{
    char line[512];
    int n = 0;
    while (*s) {
        /* A whole character at a time, so no line ends inside one. */
        do
            line[n++] = *s++;
        while (n < (int)sizeof(line) - 1 && ((unsigned char)*s & 0xC0) == 0x80); /* bad UTF-8: a long tail */
        line[n] = 0;
        if (n >= 500 || *s == '\n' || !*s || width_text(line) > w) {
            if (width_text(line) > w && Menu_TextBack(line, (size_t)n) > 0) {
                int split = n - 1;
                while (split > 0 && line[split] != ' ')
                    split--;
                if (!split)
                    split = (int)Menu_TextBack(line, (size_t)n);
                s -= n - split;
                n = split;
                line[n] = 0;
            }
            text(c, x, y + LINE / 2, w, line, color);
            y += LINE;
            n = 0;
            while (*s == ' ' || *s == '\n')
                s++;
        }
    }
    return y;
}
static void option_label(int mod, int index, char *out, size_t size)
{
    const JsonValue *spec = Mods_Option(mod, index);
    const char *type = str(spec, "type", "int");
    int value = values[mod][index];
    if (!strcmp(type, "bool"))
        snprintf(out, size, "%s", value ? "On" : "Off");
    else if (!strcmp(type, "choice"))
        snprintf(out, size, "%s", Json_String(Json_At(Json_Member(spec, "choices"), value), "Invalid choice"));
    else if (!strcmp(type, "key")) {
        int bit = 0;
        while (bit < 16 && value != (1 << bit))
            bit++;
        snprintf(out, size, "%s", bit < 16 ? pad_names[bit] : "None");
    } else
        snprintf(out, size, "%d%s", value, str(spec, "suffix", ""));
}
/* Wide enough for every value the setting can take, so none is cut off. */
static int value_width(int option, int w)
{
    const JsonValue *spec = Mods_Option(selected, option);
    const char *type = str(spec, "type", "int");
    char s[96];
    int widest = width_text("Off");
    if (!strcmp(type, "choice")) {
        const JsonValue *choices = Json_Member(spec, "choices");
        for (int i = 0; i < Json_Count(choices); i++)
            widest = max(widest, width_text(Json_String(Json_At(choices, i), "")));
    } else if (!strcmp(type, "key")) {
        for (int i = 0; i < 16; i++)
            widest = max(widest, width_text(pad_names[i]));
    } else if (strcmp(type, "bool")) {
        snprintf(s, sizeof(s), "%d%s", num(spec, "min", 0), str(spec, "suffix", ""));
        widest = max(widest, width_text(s));
        snprintf(s, sizeof(s), "%d%s", num(spec, "max", 100), str(spec, "suffix", ""));
        widest = max(widest, width_text(s));
    }
    return min(max(widest + 24 * unit, 64 * unit), w * 2 / 5);
}
static int is_int(int option) { return !strcmp(str(Mods_Option(selected, option), "type", "int"), "int"); }
typedef struct {
    Rect minus, value, plus, slider;
    int height;
} OptionBox;
/* One setting of the selected mod, `w` wide with its top at `y`: the label
 * wraps beside - value +, the description wraps under both, then an int's
 * slider. Fills `o` in the same coordinates; draws only with a canvas. */
static void option(MenuCanvas *c, int index, int w, int y, OptionBox *o)
{
    const JsonValue *spec = Mods_Option(selected, index);
    const char *about = str(spec, "description", "");
    char value[160];
    int box = value_width(index, w), bottom;
    o->plus = rect(w - CONTROL, y, CONTROL, CONTROL);
    o->value = rect(o->plus.x - 4 * unit - box, y, box, CONTROL);
    o->minus = rect(o->value.x - 4 * unit - CONTROL, y, CONTROL, CONTROL);
    bottom = wrap(c, 0, y + 3 * unit, o->minus.x - 16 * unit, str(spec, "label", str(spec, "key", "Setting")), TEXT);
    bottom = max(y + CONTROL, bottom);
    if (*about)
        bottom = wrap(c, 0, bottom + 4 * unit, w, about, DIM);
    if (Json_Bool(Json_Member(spec, "restart"), 0))
        bottom = wrap(c, 0, bottom + (*about ? 0 : 4 * unit), w, "Requires a restart", WARN);
    o->slider = rect(0, bottom + 2 * unit, w, !is_int(index) ? 0 : touch ? max(22 * unit, touch * 2 / 3) : 22 * unit);
    o->height = o->slider.y + o->slider.h + 14 * unit - y;
    if (!c)
        return;
    option_label(selected, index, value, sizeof(value));
    button(c, o->minus, "-", 0);
    fill(c, o->value, BG);
    centred(c, o->value, value,
            !strcmp(str(spec, "type", "int"), "bool") ? (values[selected][index] ? GREEN : DIM) : BLUE);
    button(c, o->plus, "+", 0);
    if (o->slider.h) {
        int low = num(spec, "min", 0), high = num(spec, "max", 100), span = w - 10 * unit;
        int mid = o->slider.y + o->slider.h / 2;
        int at = high > low ? (int)(((int64_t)values[selected][index] - low) * span / ((int64_t)high - low)) : 0;
        at = min(max(at, 0), span);
        fill(c, rect(0, mid - 2 * unit, w, 4 * unit), EDGE);
        fill(c, rect(0, mid - 2 * unit, at, 4 * unit), ACCENT);
        fill(c, rect(at, mid - 7 * unit, 10 * unit, 14 * unit), BLUE);
    }
    fill(c, rect(0, y + o->height - 1, w, 1), EDGE);
}
/* What the selected mod changes that another enabled mod changes too
 * (overlap.h), by kind: a heading with the count, the first few lines, and
 * "and N more" to show the rest (at most OVERLAPS_OPEN), the log having them
 * all. A real override is in the warning color; what adds up, agrees or
 * follows an `after` is dim. Where each "more" line is, for the press that
 * opens it, is kept in more_top/more_bottom. */
#define OVERLAPS_SHOWN 4
#define OVERLAPS_OPEN 200
static int opened[MODS_OVERLAP_KINDS], opened_for = -1, more_top[MODS_OVERLAP_KINDS], more_bottom[MODS_OVERLAP_KINDS];
static int overlaps(MenuCanvas *c, int w, int y)
{
    const ModsOverlaps *found = Mods_Overlaps(wanted, ranks, (const int *const *)values);
    int place = Mods_OverlapPlace(selected), total = 0, warnings = 0, alone = 0, n = Mods_OverlapCount(found);
    char line[1024];
    if (opened_for != selected)
        memset(opened, 0, sizeof(opened));
    opened_for = selected;
    for (int k = 0; k < MODS_OVERLAP_KINDS; k++)
        more_top[k] = more_bottom[k] = 0;
    if (!wanted[selected] || place < 0)
        return wrap(c, 0, y, w, "Enable this mod to see what it changes that other enabled mods change too.", DIM);
    /* A line about this mod alone (overlap.h Mods_OverlapModCount: starter
     * pools the game leaves out) is no other mod's change: said apart. */
    for (int i = 0; i < n; i++)
        if (Mods_OverlapInvolves(found, i, place)) {
            if (Mods_OverlapModCount(found, i) < 2) {
                alone++;
                continue;
            }
            total++;
            warnings += Mods_OverlapSeverity(found, i) == MODS_OVERLAP_WARNING;
        }
    if (!total && !alone)
        return wrap(c, 0, y, w, "Nothing it changes is changed by another enabled mod.", GREEN);
    if (total) {
        snprintf(line, sizeof(line),
                 "Also changed by other enabled mods: %d thing%s, %d where only one mod's change is used.", total,
                 total == 1 ? "" : "s", warnings);
        y = wrap(c, 0, y, w, line, warnings ? WARN : TEXT);
    } else
        y = wrap(c, 0, y, w, "Nothing it changes is changed by another enabled mod.", GREEN);
    if (alone) {
        snprintf(line, sizeof(line), "Left out by the game with these mods enabled: %d thing%s of its own.", alone,
                 alone == 1 ? "" : "s");
        y = wrap(c, 0, y, w, line, WARN);
    }
    for (int kind = 0, i = 0; kind < MODS_OVERLAP_KINDS; kind++) {
        int count = 0, kind_warnings = 0, shown = 0, limit = opened[kind] ? OVERLAPS_OPEN : OVERLAPS_SHOWN;
        int first = i;
        while (i < n && Mods_OverlapKind(found, i) == kind) {
            if (Mods_OverlapInvolves(found, i, place)) {
                count++;
                kind_warnings += Mods_OverlapSeverity(found, i) == MODS_OVERLAP_WARNING;
            }
            i++;
        }
        if (!count)
            continue;
        snprintf(line, sizeof(line), "%s: %d (%d warning%s)", Mods_OverlapKindName(kind), count, kind_warnings,
                 kind_warnings == 1 ? "" : "s");
        y = wrap(c, 0, y + 10 * unit, w, line, TEXT);
        for (int j = first; j < i && shown < limit; j++)
            if (Mods_OverlapInvolves(found, j, place)) {
                Mods_OverlapText(found, j, line, sizeof(line));
                y = wrap(c, 12 * unit, y, w - 12 * unit, line,
                         Mods_OverlapSeverity(found, j) == MODS_OVERLAP_WARNING ? WARN : DIM);
                shown++;
            }
        if (count > shown || opened[kind]) {
            if (count > shown && opened[kind])
                snprintf(line, sizeof(line), "...and %d more, in the log (MEMORIES_TRACE=mods). Show fewer", count - shown);
            else if (count > shown)
                snprintf(line, sizeof(line), "...and %d more: show %s", count - shown,
                         count - shown > OVERLAPS_OPEN - OVERLAPS_SHOWN ? "the next ones" : "them");
            else
                snprintf(line, sizeof(line), "Show fewer");
            more_top[kind] = y;
            y = wrap(c, 12 * unit, y, w - 12 * unit, line, BLUE);
            more_bottom[kind] = y;
        }
    }
    return wrap(c, 0, y + 10 * unit, w, "Every line is in the log with MEMORIES_TRACE=mods.", DIM);
}
/* The selected tab's contents, `w` wide from `y`; returns where they end. */
static int body(MenuCanvas *c, int w, int y)
{
    char line[512];
    if (tab == 0) {
        y = wrap(c, 0, y, w, Mods_Metadata(selected, "description"), TEXT) + 16 * unit;
        snprintf(line, sizeof(line), "ID: %s", Mods_Id(selected));
        y = wrap(c, 0, y, w, line, DIM);
        y = wrap(c, 0, y, w, Mods_Directory(selected), DIM) + 16 * unit;
        y = wrap(c, 0, y, w,
                 Mods_HasCode(selected) ? "Native code mod: runs game code from this author."
                                        : "Content mod: assets, cards or data patches.",
                 DIM);
        if (Mods_Status(selected)[0])
            y = wrap(c, 0, y + 16 * unit, w, Mods_Status(selected), Mods_Failed(selected) ? RED : WARN);
    } else if (tab == 1) {
        if (!counts[selected])
            return wrap(c, 0, y, w, "This mod does not declare configurable settings.", DIM);
        if (toolbar_in_body()) {
            if (c) {
                snprintf(line, sizeof(line), "%d setting%s", counts[selected], counts[selected] == 1 ? "" : "s");
                text(c, 0, y + touch / 2, w - defaults_width() - 12 * unit, line, DIM);
                button(c, rect(w - defaults_width(), y, defaults_width(), touch), "Restore defaults", 0);
            }
            y += touch + 8 * unit;
        }
        for (int j = 0; j < counts[selected]; j++) {
            OptionBox o;
            option(c, j, w, y + 10 * unit, &o);
            y += 10 * unit + o.height;
        }
    } else {
        const char *keys[] = {"requires", "after", "conflicts"};
        const char *labels[] = {"Requires: ", "Load after: ", "Conflicts: "};
        y = wrap(c, 0, y, w,
                 Mods_RequiresRestart(selected) ? "Changes require a restart." : "This mod supports live changes.",
                 DIM) +
            12 * unit;
        for (int k = 0; k < 3; k++) {
            const JsonValue *list = Json_Member(Mods_Manifest(selected), keys[k]);
            for (int j = 0; j < Json_Count(list); j++) {
                const JsonValue *v = Json_At(list, j);
                snprintf(line, sizeof(line), "%s%s", labels[k], Json_String(v, str(v, "id", "")));
                y = wrap(c, 0, y, w, line, TEXT);
            }
        }
        if (!Mods_Validate(wanted, line, sizeof(line)))
            y = wrap(c, 0, y + 12 * unit, w, line, WARN);
        else
            y = wrap(c, 0, y + 12 * unit, w, "Dependencies and declared conflicts are satisfied.", GREEN);
        y = overlaps(c, w, y + 12 * unit);
    }
    return y;
}
/* The details scroll by pixels inside `view`, left of their scrollbar. */
static int body_width(const Layout *l) { return l->view.w - BAR - 12 * unit; }
static int body_height(const Layout *l) { return selected < 0 ? 0 : body(NULL, body_width(l), 0) + 12 * unit; }
static int detail_limit(const Layout *l) { return max(0, body_height(l) - l->view.h); }
static Rect thumb(Rect track, int total, int shown, int at)
{
    int h = min(track.h, max(28 * unit, (int)((int64_t)track.h * shown / max(1, total))));
    return rect(track.x, track.y + (int)((int64_t)(track.h - h) * at / max(1, total - shown)), track.w, h);
}
/* The scroll position that puts the thumb's top at `y`. */
static int thumb_to(Rect track, int total, int shown, int y)
{
    int travel = track.h - thumb(track, total, shown, 0).h;
    if (travel <= 0)
        return 0;
    y = min(max(y - track.y, 0), travel);
    return (int)(((int64_t)y * (total - shown) + travel / 2) / travel);
}
static void scrollbar(MenuCanvas *c, Rect track, int total, int shown, int at, int held)
{
    if (total <= shown)
        return;
    fill(c, track, BG);
    fill(c, thumb(track, total, shown, at), held ? BLUE : 0x5d6b80u);
}
/* What the window counts: shown in its title row, or with a finger in the
 * footer's message line while there is no message. */
static void counts_line(char *line, size_t size, int room)
{
    int enabled = 0, overlap_count = 0, overlap_warnings = 0, listed = 0;
    for (int i = 0; i < Mods_Count(); i++) {
        enabled += !!wanted[i];
        listed += !Mods_Removed(i);
    }
    if (enabled > 1) {
        const ModsOverlaps *found = Mods_Overlaps(wanted, ranks, (const int *const *)values);
        overlap_count = Mods_OverlapCount(found);
        for (int i = 0; i < overlap_count; i++)
            overlap_warnings += Mods_OverlapSeverity(found, i) == MODS_OVERLAP_WARNING;
    }
    if (overlap_count)
        snprintf(line, size, "%d installed  /  %d enabled  /  %d overlap%s, %d warning%s%s", listed,
                 enabled, overlap_count, overlap_count == 1 ? "" : "s", overlap_warnings,
                 overlap_warnings == 1 ? "" : "s", changed() ? "  /  Unsaved changes" : "");
    if (overlap_count && width_text(line) > room) /* a small window: the tab has the warnings */
        snprintf(line, size, "%d installed  /  %d enabled  /  %d overlap%s%s", listed, enabled,
                 overlap_count, overlap_count == 1 ? "" : "s", changed() ? "  /  Unsaved" : "");
    else if (!overlap_count)
        snprintf(line, size, "%d installed  /  %d enabled%s", listed, enabled,
                 changed() ? "  /  Unsaved changes" : "");
}
/* The search, filter and profile fields with Save and Load. */
static void draw_fields(MenuCanvas *c, const Layout *l)
{
    fill(c, l->search, focus == 1 ? EDGE : PANEL);
    text(c, l->search.x + 10 * unit, l->search.y + l->search.h / 2, l->search.w - 20 * unit,
         *query ? query : "Search mods, IDs or authors...", *query ? TEXT : DIM);
    button(c, l->filter, filters[filter], 0);
    fill(c, l->profile, focus == 2 ? EDGE : PANEL);
    text(c, l->profile.x + 10 * unit, l->profile.y + l->profile.h / 2, l->profile.w - 20 * unit, profile, TEXT);
    button(c, l->save, "Save", 0);
    button(c, l->load, "Load", 0);
    if (l->hd.w)
        button(c, l->hd, hd_step == 3 || (hd_step && hd_step != 2 && HdPack_Busy()) ? "Stop" : "HD pack...",
               hd_step != 0); /* Stop where hd_tap stops */
}
static void draw_list(MenuCanvas *c, const Layout *l)
{
    char line[512];
    int list_bar = shown_count() > rows(), lift = (PITCH - 58 * unit) / 2;
    fill(c, l->list, PANEL);
    for (int r = 0; r < rows(); r++) {
        int mod = shown(scroll + r), w = l->list.w - 54 * unit - (list_bar ? BAR + 6 * unit : 0);
        Rect row = rect(l->list.x, l->list.y + r * PITCH, l->list.w, PITCH - 2 * unit);
        if (mod < 0)
            break;
        if (mod == selected)
            fill(c, row, 0x293e60u);
        text(c, row.x + 12 * unit, row.y + lift + 18 * unit, 24 * unit, wanted[mod] ? "[x]" : "[ ]",
             wanted[mod] ? GREEN : DIM);
        text(c, row.x + 44 * unit, row.y + lift + 18 * unit, w, Mods_Name(mod), TEXT);
        snprintf(line, sizeof(line), "%s%s%s",
                 Mods_Failed(mod)      ? "Error"
                 : Mods_Status(mod)[0] ? "Warning"
                 : Mods_Active(mod)    ? "Active"
                                       : "Inactive",
                 wanted[mod] != Mods_Active(mod) ? " / pending" : "", Mods_RequiresRestart(mod) ? " / restart" : "");
        text(c, row.x + 44 * unit, row.y + lift + 39 * unit, w, line, Mods_Failed(mod) ? RED : DIM);
    }
    if (!shown_count())
        text(c, l->list.x + 16 * unit, l->list.y + 32 * unit, l->list.w - 32 * unit, "No matching mods", DIM);
    scrollbar(c, l->list_bar, shown_count(), rows(), scroll, bar_drag == 1);
}
static void draw_details(MenuCanvas *c, const Layout *l)
{
    char line[512];
    fill(c, l->detail, PANEL);
    if (selected >= 0) {
        int name_x = l->back.w ? l->back.x + l->back.w + 8 * unit : l->detail.x + 16 * unit, total = body_height(l);
        int order_x = l->order[0].x - 10 * unit - width_text("Load order"), label = 1;
        /* With a finger the name shares its row with the load order: the
         * label goes where the name would have too little room. */
        if (touch && order_x - name_x < 140 * unit) {
            order_x = l->order[0].x;
            label = 0;
        }
        if (l->back.w)
            button(c, l->back, "Back", 0);
        text(c, name_x, l->name_y, (touch ? order_x : l->toggle.x) - 12 * unit - name_x, Mods_Name(selected), TEXT);
        button(c, l->toggle, wanted[selected] ? "Enabled" : "Disabled", wanted[selected]);
        snprintf(line, sizeof(line), "%s  /  %s  /  %s", Mods_Metadata(selected, "version"),
                 Mods_Metadata(selected, "author"), origin_text(Mods_Origin(selected)));
        text(c, name_x, l->meta_y, order_x - 12 * unit - name_x, line, DIM);
        if (label)
            text(c, order_x, touch ? l->order[0].y + l->order[0].h / 2 : l->meta_y, width_text("Load order") + unit,
                 "Load order", DIM);
        button(c, l->order[0], "-", 0);
        snprintf(line, sizeof(line), "%d", ranks[selected]);
        centred(c, rect(l->order[0].x + l->order[0].w, l->order[0].y, l->order[1].x - l->order[0].x - l->order[0].w,
                        l->order[0].h),
                line, TEXT);
        button(c, l->order[1], "+", 0);
        button(c, l->tabs[0], "About", tab == 0);
        button(c, l->tabs[1], "Settings", tab == 1);
        button(c, l->tabs[2], "Compatibility", tab == 2);
        if (tab == 1 && counts[selected] && !toolbar_in_body()) {
            snprintf(line, sizeof(line), "%d setting%s", counts[selected], counts[selected] == 1 ? "" : "s");
            text(c, l->view.x, l->defaults.y + l->defaults.h / 2, l->defaults.x - l->view.x - 12 * unit, line, DIM);
            button(c, l->defaults, "Restore defaults", 0);
            fill(c, rect(l->view.x, l->view.y - 7 * unit, l->view.w, 1), EDGE);
        }
        detail_scroll = min(detail_scroll, max(0, total - l->view.h));
        if (l->view.h > 0 && l->view.x + l->view.w <= c->width && l->view.y + l->view.h <= c->height) {
            /* A canvas over just the viewport clips the scrolled contents. */
            MenuCanvas view = *c;
            view.pixels = c->pixels + (size_t)l->view.y * c->stride + l->view.x;
            view.width = body_width(l);
            view.height = l->view.h;
            body(&view, view.width, -detail_scroll);
        }
        scrollbar(c, l->bar, total, l->view.h, detail_scroll, bar_drag == 2);
    } else
        text(c, l->detail.x + 20 * unit, l->detail.y + 30 * unit, l->detail.w - 40 * unit,
             "Select a mod to view its details", DIM);
}
/* Mods folder..., or while its question is up (pending 6) the answer that
 * chooses none, if a folder is chosen. */
static void draw_modsdir(MenuCanvas *c, const Layout *l)
{
    char path[1024];
    if (!l->modsdir.w)
        return;
    if (pending != 6)
        button(c, l->modsdir, "Mods folder...", 0);
    else if (!Mods_Folder(path, sizeof(path)))
        button(c, l->modsdir, "Use no folder", 0);
}
static const char *apply_label(void)
{
    return pending == 7 ? "Allow" : pending == 6 ? "Choose folder..." : pending == 5 ? "Remove" : pending == 4 ? "Download" : pending == 3 ? (import_plain ? "Import" : "Replace") : pending == 2 ? "Discard changes" : pending == 1 ? "Apply & restart" : "Apply changes";
}
/* With a finger: the fields and the list, the details (side by side or the
 * page shown), and the footer: the message (or the counts) beside Close and
 * Apply, up to four lines in the footer's band (layout_touch raises it). */
/* Where the footer's buttons start: Mods folder... when it is shown, else
 * Remove, else the files slot when it has a button, else Close. */
static int footer_left(const Layout *l)
{
    return l->modsdir.w ? l->modsdir.x : l->remove.w ? l->remove.x : l->folder.w ? l->folder.x : l->close.x;
}
static void draw_touch(MenuCanvas *c, const Layout *l)
{
    char line[512];
    int footer = l->apply.y - l->lift, room = footer_left(l) - 8 * unit - 12 * unit;
    if (!compact || page == 0) {
        draw_fields(c, l);
        draw_list(c, l);
    }
    if (!compact || page == 1)
        draw_details(c, l);
    fill(c, rect(0, footer - 4 * unit, width, 1), EDGE);
    if (*status)
        snprintf(line, sizeof(line), "%s", status);
    else
        counts_line(line, sizeof(line), room);
    if (l->apply.y + l->apply.h <= c->height) {
        /* A band from the line above the buttons to the bottom clips it. */
        MenuCanvas band = *c;
        int top = footer - 3 * unit, lines = min(4, max(1, wrap(NULL, 0, 0, room, line, 0) / LINE));
        band.pixels = c->pixels + (size_t)top * c->stride;
        band.height = c->height - top;
        /* centred between the line above and the buttons' bottom */
        wrap(&band, 12 * unit, footer - top + (l->lift + l->apply.h) / 2 - lines * LINE / 2, room, line,
             pending ? WARN : *status ? TEXT : DIM);
    }
    if (l->folder.w)
        button(c, l->folder, import_step >= 2 ? "Stop" : "Import mod...",
               import_step >= 2); /* Stop where import_tap stops */
    if (l->remove.w)
        button(c, l->remove, "Remove", 0);
    draw_modsdir(c, l);
    button(c, l->close, pending ? "Cancel" : "Close", 0);
    button(c, l->apply, apply_label(), changed() || pending || folder_restart());
}
/* --- Modpack tab (desktop): profiles of mods made from a .zip ---------- *
 * Each modpack is a folder of mods in the user directory's modpacks/ (the
 * .zip's mods, unpacked by Mods_ImportInstall), with the modpack.json and
 * poster.png an export wrote. The one chosen is read at the next launch
 * instead of the normal mods folders (Mods_ModpackSelect); with none chosen
 * the game starts with the normal mods. Export writes such a .zip from the
 * mods chosen in a separate export list, their settings and load order. */
static char pack_names[PACKS_MAX][128], pack_chosen[128];
static int pack_confirm; /* a row's ( ) clicked once: 1 + its row, asking before it is activated */
/* pick(kind): 0 a modpack .zip to open, 1 a poster .png, 2 where to save an export */
static int (*pack_pick)(int, char *, size_t), (*pack_picked)(char *, size_t, char *, size_t);
static int pack_picking, pack_kind;
static char pack_name[64], pack_version[32], pack_author[64], pack_description[8192], pack_poster[1024];
/* The shown modpack's modpack.json and poster (for pack_info_of) */
static JsonDocument *pack_info;
static int pack_info_of = -1;
static uint32_t *poster_pixels;
static int poster_w, poster_h;
void ModsWindow_SetModpackPicker(int (*pick)(int kind, char *why, size_t why_size),
                                 int (*picked)(char *path, size_t size, char *why, size_t why_size))
{
    pack_pick = pick;
    pack_picked = picked;
}
static int pack_folder(int i, char *out, size_t size)
{
    char dir[1024];
    if (Mods_ModpackDirectory(dir, sizeof(dir))) return -1;
    return (size_t)snprintf(out, size, "%s/%s", dir, pack_names[i]) >= size ? -1 : 0;
}
static int pack_count_mods(const char *folder)
{
    char path[1200], id[64];
    int n = 0;
    DIR *d = opendir(folder);
    struct dirent *e;
    if (!d) return 0;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.' && (size_t)snprintf(path, sizeof(path), "%s/%s", folder, e->d_name) < sizeof(path) &&
            Mods_ImportFolderId(path, id, sizeof(id)))
            n++;
    closedir(d);
    return n;
}
static void pack_forget_info(void)
{
    if (pack_info) Json_Free(pack_info);
    pack_info = NULL;
    free(poster_pixels);
    poster_pixels = NULL;
    poster_w = poster_h = 0;
    pack_info_of = -1;
}
/* A PNG's pixels (0xRRGGBB), and its size; NULL when it cannot be read. */
static uint32_t *png_pixels(const char *path, int *w, int *h)
{
    png_image image;
    unsigned char *rgb;
    uint32_t *out;
    FILE *file = fopen(path, "rb");
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!file) return NULL;
    if (!png_image_begin_read_from_stdio(&image, file) || image.width > 8192 || image.height > 8192) {
        png_image_free(&image);
        fclose(file);
        return NULL;
    }
    image.format = PNG_FORMAT_RGB;
    rgb = malloc(PNG_IMAGE_SIZE(image));
    out = malloc((size_t)image.width * image.height * 4);
    if (!rgb || !out || !png_image_finish_read(&image, NULL, rgb, 0, NULL)) {
        free(rgb);
        free(out);
        png_image_free(&image);
        fclose(file);
        return NULL;
    }
    fclose(file);
    for (size_t i = 0; i < (size_t)image.width * image.height; i++)
        out[i] = (uint32_t)rgb[i * 3] << 16 | (uint32_t)rgb[i * 3 + 1] << 8 | rgb[i * 3 + 2];
    free(rgb);
    *w = (int)image.width;
    *h = (int)image.height;
    return out;
}
/* The selected modpack's modpack.json and poster, read once per selection. */
static void pack_load_info(void)
{
    char folder[1200], path[1300], error[160];
    if (pack_info_of == pack_sel) return;
    pack_detail_scroll = pack_detail_max = 0;
    pack_forget_info();
    pack_info_of = pack_sel;
    if (!pack_sel || pack_folder(pack_sel - 1, folder, sizeof(folder))) return;
    snprintf(path, sizeof(path), "%s/modpack.json", folder);
    pack_info = Json_ParseFile(path, error, sizeof(error));
    snprintf(path, sizeof(path), "%s/poster.png", folder);
    poster_pixels = png_pixels(path, &poster_w, &poster_h);
}
static void packs_scan(void)
{
    char dir[1024], path[1200];
    DIR *d;
    struct dirent *e;
    pack_count = 0;
    pack_forget_info();
    Mods_ModpackSelected(pack_chosen, sizeof(pack_chosen));
    if (Mods_ModpackDirectory(dir, sizeof(dir)) || !(d = opendir(dir))) return;
    while ((e = readdir(d)) && pack_count < PACKS_MAX) {
        struct stat info;
        if (!Mods_ModpackNameValid(e->d_name) || strlen(e->d_name) >= sizeof(pack_names[0])) continue;
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= sizeof(path)) continue;
        if (stat(path, &info) || !S_ISDIR(info.st_mode)) continue;
        memcpy(pack_names[pack_count], e->d_name, strlen(e->d_name) + 1);
        pack_mods[pack_count] = pack_count_mods(path);
        pack_count++;
    }
    closedir(d);
    for (int i = 1; i < pack_count; i++) /* sorted, as the mods are */
        for (int j = i; j > 0 && strcmp(pack_names[j - 1], pack_names[j]) > 0; j--) {
            char name[128];
            int n = pack_mods[j];
            memcpy(name, pack_names[j], sizeof(name));
            memcpy(pack_names[j], pack_names[j - 1], sizeof(name));
            memcpy(pack_names[j - 1], name, sizeof(name));
            pack_mods[j] = pack_mods[j - 1];
            pack_mods[j - 1] = n;
        }
    if (pack_sel > pack_count) pack_sel = 0;
}
/* The modpack the next launch reads differs from this launch's. */
static int pack_restart(void) { return strcmp(pack_chosen, Mods_ModpackLaunched()) != 0; }
/* A name for a folder: its characters a folder can take, no dot or space at its ends. */
static void folder_name(char *name, size_t size, const char *from)
{
    size_t n = strlen(from);
    if (n >= size) n = size - 1;
    if (n > 100) n = 100;
    memcpy(name, from, n);
    name[n] = 0;
    for (char *c = name; *c; c++)
        if ((unsigned char)*c < 32 || strchr("/\\:*?\"<>|", *c)) *c = '_';
    while (name[0] == '.' || name[0] == ' ') memmove(name, name + 1, strlen(name));
    while ((n = strlen(name)) && (name[n - 1] == '.' || name[n - 1] == ' ')) name[n - 1] = 0;
    if (!name[0]) snprintf(name, size, "Modpack");
}
/* --- Import of a modpack .zip, off the window's thread ----------------- *
 * The .zip read, its mods unpacked into a new folder of modpacks/ and its
 * modpack.json and poster put beside them, on a thread of its own: the bar
 * in the footer shows the bytes unpacked of all (Mods_ImportWatch), and Stop
 * ends it (the new folder removed). import_poll ends it on the window's
 * thread, where the list is read again. */
static struct {
    pthread_t thread;
    int running;                  /* from pack_import until import_poll joins it */
    int finished, ok, cancel;     /* atomic */
    unsigned done;                /* atomic: bytes unpacked (Mods_ImportWatch) */
    unsigned long long total;     /* atomic: all of them */
    int shown, count;             /* the permille last drawn; the mods in it */
    char zip[1100], name[128], why[400];
} import_job;
static void *pack_import_step(void *unused)
{
    char dir[1024], dest[1200], path[1300], why[400] = "";
    const char *base = import_job.zip, *s;
    ModsImport *import = NULL;
    int ok = 0, made = 0;
    size_t n;
    (void)unused;
    for (s = import_job.zip; *s; s++)
        if (*s == '/' || *s == '\\') base = s + 1;
    {
        char plain[128];
        n = strlen(base);
        if (n > 4 && (!strcmp(base + n - 4, ".zip") || !strcmp(base + n - 4, ".ZIP"))) n -= 4;
        if (n >= sizeof(plain)) n = sizeof(plain) - 1;
        memcpy(plain, base, n);
        plain[n] = 0;
        folder_name(import_job.name, sizeof(import_job.name), plain);
    }
    if (Mods_ModpackDirectory(dir, sizeof(dir))) {
        snprintf(why, sizeof(why), "Could not make the modpacks folder.");
        goto done;
    }
    if (!(import = Mods_ImportOpen(import_job.zip, why, sizeof(why)))) goto done;
    if (!(import_job.count = Mods_ImportCount(import))) {
        snprintf(why, sizeof(why), "That .zip has no mod (no folder with a mod.json).");
        goto done;
    }
    __atomic_store_n(&import_job.total, Mods_ImportBytes(import), __ATOMIC_RELAXED);
    {   /* the modpack's own name, when an export wrote one */
        char temporary[1100], error[160];
        JsonDocument *info;
        snprintf(temporary, sizeof(temporary), "%s/.incoming-modpack.json", dir);
        if (Mods_ImportExtract(import, "modpack.json", temporary, why, sizeof(why)) == 1 &&
            (info = Json_ParseFile(temporary, error, sizeof(error)))) {
            const char *named = Json_String(Json_Member(Json_Root(info), "name"), "");
            if (*named) folder_name(import_job.name, sizeof(import_job.name), named);
            Json_Free(info);
        }
        remove(temporary);
        why[0] = 0;
    }
    {   /* a name not taken: "Name", "Name 2"... */
        char base_name[128];
        struct stat info;
        snprintf(base_name, sizeof(base_name), "%s", import_job.name);
        for (int k = 2;; k++) {
            snprintf(dest, sizeof(dest), "%s/%s", dir, import_job.name);
            if (stat(dest, &info)) break;
            snprintf(import_job.name, sizeof(import_job.name), "%.100s %d", base_name, k);
        }
    }
    if (Paths_MakeDirs(dest)) {
        snprintf(why, sizeof(why), "Could not make %.300s.", dest);
        goto done;
    }
    made = 1;
    for (int i = 0; i < import_job.count; i++) Mods_ImportMod(import, i)->replace[0] = 0;
    Mods_ImportWatch(import, &import_job.done, &import_job.cancel);
    if (!Mods_ImportInstall(import, dest, why, sizeof(why))) goto done;
    snprintf(path, sizeof(path), "%s/modpack.json", dest);
    if (Mods_ImportExtract(import, "modpack.json", path, why, sizeof(why)) < 0) goto done;
    snprintf(path, sizeof(path), "%s/poster.png", dest);
    if (Mods_ImportExtract(import, "poster.png", path, why, sizeof(why)) < 0) goto done;
    ok = 1;
done:
    if (import) Mods_ImportClose(import);
    if (!ok && made) Mods_ImportRemoveTree(dest);
    if (!ok && __atomic_load_n(&import_job.cancel, __ATOMIC_RELAXED)) snprintf(why, sizeof(why), "Import stopped.");
    snprintf(import_job.why, sizeof(import_job.why), "%s", why);
    __atomic_store_n(&import_job.ok, ok, __ATOMIC_RELAXED);
    __atomic_store_n(&import_job.finished, 1, __ATOMIC_RELEASE);
    return NULL;
}
static void pack_import(const char *zip)
{
    if (import_job.running) return;
    snprintf(import_job.zip, sizeof(import_job.zip), "%s", zip);
    import_job.name[0] = import_job.why[0] = 0;
    import_job.done = 0;
    import_job.total = 0;
    import_job.count = import_job.finished = import_job.ok = import_job.cancel = 0;
    import_job.shown = -1;
    if (pthread_create(&import_job.thread, NULL, pack_import_step, NULL)) {
        put(status, sizeof(status), "Could not start the import.");
        return;
    }
    import_job.running = 1;
    put(status, sizeof(status), "Importing %s", zip);
}
/* Each tick: 1 when the bar moved or the import ended (a redraw). */
static int import_poll(void)
{
    if (!import_job.running) return 0;
    if (!__atomic_load_n(&import_job.finished, __ATOMIC_ACQUIRE)) {
        unsigned long long total = __atomic_load_n(&import_job.total, __ATOMIC_RELAXED);
        unsigned done = __atomic_load_n(&import_job.done, __ATOMIC_RELAXED);
        int permille = total ? (int)((unsigned long long)done * 1000 / total) : 0;
        if (permille == import_job.shown) return 0;
        import_job.shown = permille;
        return 1;
    }
    pthread_join(import_job.thread, NULL);
    import_job.running = 0;
    if (!import_job.ok) {
        put(status, sizeof(status), "%s", import_job.why);
        return 1;
    }
    packs_scan();
    for (int i = 0; i < pack_count; i++)
        if (!strcmp(pack_names[i], import_job.name)) pack_sel = i + 1;
    put(status, sizeof(status), "Modpack \"%s\" imported: %d mod(s). Click Activate modpack to use it.",
        import_job.name, import_job.count);
    return 1;
}
/* The footer's bar while the import runs, in `r`. */
static void import_bar(MenuCanvas *c, Rect r)
{
    char line[200];
    unsigned long long total = __atomic_load_n(&import_job.total, __ATOMIC_RELAXED);
    unsigned done = __atomic_load_n(&import_job.done, __ATOMIC_RELAXED);
    int percent = total ? (int)min(100ULL, (unsigned long long)done * 100 / total) : 0, pw = width_text("100%") + 12 * unit;
    fill(c, r, BG);
    fill(c, rect(r.x, r.y, (int)((long long)r.w * percent / 100), r.h), 0x2f6f4fu);
    put(line, sizeof(line), "%d%%", percent);
    text(c, r.x + r.w - pw, r.y + r.h / 2, pw, line, TEXT);
    put(line, sizeof(line), "Importing %s...", import_job.name[0] ? import_job.name : "the modpack");
    text(c, r.x + 10 * unit, r.y + r.h / 2, r.w - pw - 16 * unit, line, TEXT);
}

/* --- Export ----------------------------------------------------------- */
/* A fresh inventory of the normal mods folders, independent of the running
 * modpack and of the library's enabled/disabled checkboxes. */
typedef struct {
    char directory[1200], id[64];
    JsonDocument *manifest;
    int selected, loaded;
} ExportMod;
static ExportMod export_mods[MODS_MAX];
static int export_count;
static void export_scan_folder(const char *directory)
{
    DIR *d = opendir(directory);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d))) {
        char path[1200], file[1300], id[64], error[160];
        JsonDocument *doc;
        int i;
        if (e->d_name[0] == '.' ||
            (size_t)snprintf(path, sizeof(path), "%s/%s", directory, e->d_name) >= sizeof(path) ||
            !Mods_ImportFolderId(path, id, sizeof(id))) continue;
        snprintf(file, sizeof(file), "%s/mod.json", path);
        if (!(doc = Json_ParseFile(file, error, sizeof(error)))) continue;
        for (i = 0; i < export_count && strcmp(export_mods[i].id, id); i++);
        if (i == MODS_MAX) {
            Json_Free(doc);
            put(status, sizeof(status), "Only the first %d mods can be listed for export.", MODS_MAX);
            continue;
        }
        if (i == export_count) export_count++;
        else Json_Free(export_mods[i].manifest);
        ExportMod *m = &export_mods[i];
        memset(m, 0, sizeof(*m));
        snprintf(m->directory, sizeof(m->directory), "%s", path);
        snprintf(m->id, sizeof(m->id), "%s", id);
        m->manifest = doc;
        m->loaded = -1;
        for (int j = 0; j < Mods_Count(); j++)
            if (!Mods_Removed(j) && !strcmp(Mods_Directory(j), path)) { m->loaded = j; break; }
    }
    closedir(d);
}
static const char *export_name(int i)
{
    return Json_String(Json_Member(Json_Root(export_mods[i].manifest), "name"), export_mods[i].id);
}
static void export_scan(void)
{
    char path[1024];
    const char *named = getenv("MEMORIES_MODS_DIR");
    for (int i = 0; i < export_count; i++) Json_Free(export_mods[i].manifest);
    memset(export_mods, 0, sizeof(export_mods));
    export_count = export_scroll = export_sel = 0;
    if (named && *named) export_scan_folder(named);
    else {
        if (!Paths_Program(path, sizeof(path), "mods")) export_scan_folder(path);
        if (!Mods_Folder(path, sizeof(path))) export_scan_folder(path);
        if (!Paths_User(path, sizeof(path), "mods")) export_scan_folder(path);
    }
    for (int i = 1; i < export_count; i++)
        for (int j = i; j > 0 && strcmp(export_name(j - 1), export_name(j)) > 0; j--) {
            ExportMod swap = export_mods[j];
            export_mods[j] = export_mods[j - 1];
            export_mods[j - 1] = swap;
        }
}
static int export_selected(void)
{
    int n = 0;
    for (int i = 0; i < export_count; i++) n += export_mods[i].selected;
    return n;
}
static int pack_shown(int row)
{
    int total = pack_form ? export_count : pack_count + 1;
    for (int i = 0; i < total; i++) {
        const char *name = pack_form ? export_name(i) : i ? pack_names[i - 1] : "No modpack (normal mods)";
        int chosen = pack_form ? export_mods[i].selected : i ? !strcmp(pack_chosen, name) : !pack_chosen[0];
        int match = contains(name, pack_query);
        if (pack_form)
            match |= contains(export_mods[i].id, pack_query) ||
                     contains(Json_String(Json_Member(Json_Root(export_mods[i].manifest), "author"), ""), pack_query);
        if (!match || (pack_filter == 1 && !chosen) || (pack_filter == 2 && chosen)) continue;
        if (row-- == 0) return i;
    }
    return -1;
}
static int pack_visible_count(void)
{
    int n = 0;
    while (pack_shown(n) >= 0) n++;
    return n;
}
typedef struct {
    char *text;
    size_t length, room;
    int failed;
} Text;
static void add(Text *t, const char *s, size_t n)
{
    if (t->failed) return;
    if (t->length + n + 1 > t->room) {
        size_t room = t->room ? t->room : 4096;
        char *grown;
        while (t->length + n + 1 > room) room *= 2;
        if (!(grown = realloc(t->text, room))) { t->failed = 1; return; }
        t->text = grown;
        t->room = room;
    }
    memcpy(t->text + t->length, s, n);
    t->length += n;
    t->text[t->length] = 0;
}
static void add_text(Text *t, const char *s) { add(t, s, strlen(s)); }
static void add_string(Text *t, const char *s) /* a JSON string */
{
    char escape[8];
    add(t, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { escape[0] = '\\'; escape[1] = (char)c; add(t, escape, 2); }
        else if (c < 32) { snprintf(escape, sizeof(escape), "\\u%04x", c); add(t, escape, 6); }
        else add(t, s, 1);
    }
    add(t, "\"", 1);
}
static void add_number(Text *t, long v)
{
    char number[32];
    snprintf(number, sizeof(number), "%ld", v);
    add_text(t, number);
}
/* Every file under `directory` into the .zip as `prefix`/..., its SHA-256 in `files`. */
/* --- Export's .zip, written off the window's thread -------------------- *
 * pack_export reads what only the window's thread may (the mods' order and
 * settings) into modpack.json's head; a thread of its own then hashes every
 * file and packs it, the bar in the footer showing how far (each file
 * counts twice: read for its SHA-256, then packed), and Stop ends it (the
 * .zip removed). export_poll ends the job on the window's thread. */
static struct {
    pthread_t thread;
    int running;                     /* from pack_export until export_poll joins it */
    int finished, ok, cancel;        /* atomic */
    long long done, total;           /* atomic, bytes (twice each file's) */
    int shown, count;                /* the permille last drawn; the mods packed */
    char path[1100], poster[1024], name[64], why[400];
    Text json;                       /* the head; the thread adds the files and the end */
    ModsZipOut *zip;
    int mods;
    struct { char directory[1200], folder[16]; } *mod;
} export_job;
static int export_skipped(const char *name)
{
    return !strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".settings-applied") || !strncmp(name, ".import", 7);
}
/* The bytes of the files export_tree would pack. */
static long long export_size(const char *directory)
{
    DIR *d = opendir(directory);
    struct dirent *e;
    long long sum = 0;
    if (!d) return 0;
    while ((e = readdir(d))) {
        char path[1200];
        struct stat info;
        if (export_skipped(e->d_name) || (size_t)snprintf(path, sizeof(path), "%s/%s", directory, e->d_name) >= sizeof(path) ||
            stat(path, &info))
            continue;
        sum += S_ISDIR(info.st_mode) ? export_size(path) : (long long)info.st_size;
    }
    closedir(d);
    return sum;
}
static int export_tree(ModsZipOut *zip, Text *files, const char *directory, const char *prefix, char *why, size_t size)
{
    DIR *d = opendir(directory);
    struct dirent *e;
    int ok = 1;
    if (!d) { snprintf(why, size, "Could not read %s.", directory); return 0; }
    while (ok && (e = readdir(d))) {
        char path[1200], name[1200], hex[65];
        struct stat info;
        if (export_skipped(e->d_name))
            continue;
        if (__atomic_load_n(&export_job.cancel, __ATOMIC_RELAXED)) {
            snprintf(why, size, "Export stopped.");
            ok = 0;
        } else if ((size_t)snprintf(path, sizeof(path), "%s/%s", directory, e->d_name) >= sizeof(path) ||
            (size_t)snprintf(name, sizeof(name), "%s/%s", prefix, e->d_name) >= sizeof(name) || stat(path, &info)) {
            snprintf(why, size, "Could not read %s.", path);
            ok = 0;
        } else if (S_ISDIR(info.st_mode)) {
            ok = export_tree(zip, files, path, name, why, size);
        } else if (!Mods_Sha256FileProgress(path, hex, &export_job.done, &export_job.cancel)) {
            if (__atomic_load_n(&export_job.cancel, __ATOMIC_RELAXED)) snprintf(why, size, "Export stopped.");
            else snprintf(why, size, "Could not read %s.", path);
            ok = 0;
        } else {
            if ((ok = Mods_ZipAddFile(zip, name, path, why, size))) {
                if (files->length) add_text(files, ",\n    ");
                add_string(files, name);
                add_text(files, ": ");
                add_string(files, hex);
            }
        }
    }
    closedir(d);
    return ok;
}
static void *export_step(void *unused)
{
    Text files = {0};
    long long total = 0;
    int ok = 1;
    struct stat info;
    (void)unused;
    for (int i = 0; i < export_job.mods; i++) total += 2 * export_size(export_job.mod[i].directory);
    if (export_job.poster[0] && !stat(export_job.poster, &info)) total += (long long)info.st_size;
    __atomic_store_n(&export_job.total, total, __ATOMIC_RELAXED);
    Mods_ZipProgress(export_job.zip, &export_job.done, &export_job.cancel);
    for (int i = 0; ok && i < export_job.mods; i++)
        ok = export_tree(export_job.zip, &files, export_job.mod[i].directory, export_job.mod[i].folder, export_job.why, sizeof(export_job.why));
    add_text(&export_job.json, "\n  ],\n  \"files\": {\n    ");
    if (files.text) add_text(&export_job.json, files.text);
    add_text(&export_job.json, "\n  }\n}\n");
    if (ok && (export_job.json.failed || files.failed)) { snprintf(export_job.why, sizeof(export_job.why), "Out of memory."); ok = 0; }
    if (ok && export_job.poster[0]) ok = Mods_ZipAddFile(export_job.zip, "poster.png", export_job.poster, export_job.why, sizeof(export_job.why));
    if (ok) ok = Mods_ZipAddData(export_job.zip, "modpack.json", export_job.json.text, export_job.json.length, export_job.why, sizeof(export_job.why));
    if (!ok) Mods_ZipAbandon(export_job.zip); /* Finish removes it */
    ok = Mods_ZipFinish(export_job.zip, export_job.why, sizeof(export_job.why)) && ok; /* keeps an abandoned one's why */
    export_job.zip = NULL;
    free(files.text);
    __atomic_store_n(&export_job.done, total, __ATOMIC_RELAXED);
    __atomic_store_n(&export_job.ok, ok, __ATOMIC_RELAXED);
    __atomic_store_n(&export_job.finished, 1, __ATOMIC_RELEASE);
    return NULL;
}
static void pack_export(const char *chosen)
{
    char why[400] = "";
    Text json = {0};
    int count = 0;
    size_t n = strlen(chosen);
    if (export_job.running) return;
    snprintf(export_job.path, sizeof(export_job.path), "%s%s", chosen, n > 4 && !strcmp(chosen + n - 4, ".zip") ? "" : ".zip");
    for (int i = 0; i < export_count; i++) count += export_mods[i].selected;
    if (!count) {
        put(status, sizeof(status), "Tick at least one mod in the list to export it.");
        return;
    }
    free(export_job.mod);
    if (!(export_job.mod = calloc((size_t)count, sizeof(*export_job.mod)))) {
        put(status, sizeof(status), "Out of memory.");
        return;
    }
    if (!(export_job.zip = Mods_ZipCreate(export_job.path, why, sizeof(why)))) {
        put(status, sizeof(status), "%s", why);
        return;
    }
    add_text(&json, "{\n  \"name\": ");
    add_string(&json, pack_name);
    add_text(&json, ",\n  \"version\": ");
    add_string(&json, pack_version);
    add_text(&json, ",\n  \"author\": ");
    add_string(&json, pack_author);
    add_text(&json, ",\n  \"description\": ");
    add_string(&json, pack_description);
    add_text(&json, ",\n  \"locked\": ");
    add_text(&json, pack_lock ? "true" : "false");
    if (pack_poster[0]) add_text(&json, ",\n  \"poster\": \"poster.png\"");
    add_text(&json, ",\n  \"mods\": [");
    export_job.mods = 0;
    for (int i = 0; i < export_count; i++) {
        ExportMod *m = &export_mods[i];
        const JsonValue *root = Json_Root(m->manifest), *options = Json_Member(root, "settings");
        if (!m->selected) continue;
        /* Unique archive directories even when two source roots use the same basename. */
        snprintf(export_job.mod[export_job.mods].folder, sizeof(export_job.mod[0].folder), "mod-%03d", i + 1);
        snprintf(export_job.mod[export_job.mods].directory, sizeof(export_job.mod[0].directory), "%s", m->directory);
        add_text(&json, export_job.mods ? ",\n    {" : "\n    {");
        add_text(&json, "\"id\": ");
        add_string(&json, m->id);
        add_text(&json, ", \"name\": ");
        add_string(&json, export_name(i));
        add_text(&json, ", \"author\": ");
        add_string(&json, Json_String(Json_Member(root, "author"), ""));
        add_text(&json, ", \"folder\": ");
        add_string(&json, export_job.mod[export_job.mods].folder);
        add_text(&json, ", \"order\": ");
        add_number(&json, m->loaded >= 0 ? ranks[m->loaded] : Json_Number(Json_Member(root, "priority"), 0));
        add_text(&json, ", \"settings\": {");
        for (int j = 0; j < Json_Count(options); j++) {
            const JsonValue *option = Json_At(options, j);
            const char *key = str(option, "key", "");
            int value = Mods_Setting(m->id, key, Json_Number(Json_Member(option, "default"), 0));
            if (m->loaded >= 0)
                for (int v = 0; v < counts[m->loaded]; v++)
                    if (!strcmp(str(Mods_Option(m->loaded, v), "key", ""), key)) value = values[m->loaded][v];
            if (j) add_text(&json, ", ");
            add_string(&json, key);
            add_text(&json, ": ");
            add_number(&json, value);
        }
        add_text(&json, "}}");
        export_job.mods++;
    }
    export_job.json = json;
    export_job.count = count;
    snprintf(export_job.poster, sizeof(export_job.poster), "%s", pack_poster);
    snprintf(export_job.name, sizeof(export_job.name), "%s", pack_name);
    export_job.why[0] = 0;
    export_job.done = export_job.total = 0;
    export_job.finished = export_job.ok = export_job.cancel = 0;
    export_job.shown = -1;
    if (pthread_create(&export_job.thread, NULL, export_step, NULL)) {
        char ignored[8];
        Mods_ZipAbandon(export_job.zip);
        Mods_ZipFinish(export_job.zip, ignored, sizeof(ignored));
        export_job.zip = NULL;
        free(export_job.json.text);
        export_job.json.text = NULL;
        put(status, sizeof(status), "Could not start the export.");
        return;
    }
    export_job.running = 1;
    put(status, sizeof(status), "Exporting %s to %s", pack_name, export_job.path);
}
/* Each tick: 1 when the bar moved or the export ended (a redraw). */
static int export_poll(void)
{
    if (!export_job.running) return 0;
    if (!__atomic_load_n(&export_job.finished, __ATOMIC_ACQUIRE)) {
        long long done = __atomic_load_n(&export_job.done, __ATOMIC_RELAXED), total = __atomic_load_n(&export_job.total, __ATOMIC_RELAXED);
        int permille = total > 0 ? (int)(done * 1000 / total) : 0;
        if (permille == export_job.shown) return 0;
        export_job.shown = permille;
        return 1;
    }
    pthread_join(export_job.thread, NULL);
    export_job.running = 0;
    free(export_job.json.text);
    memset(&export_job.json, 0, sizeof(export_job.json));
    if (export_job.ok) {
        pack_form = 0;
        put(status, sizeof(status), "Exported %d mod(s) to %s", export_job.count, export_job.path);
    } else
        put(status, sizeof(status), "%s", export_job.why);
    return 1;
}
/* The footer's bar while the export runs, in `r`. */
static void export_bar(MenuCanvas *c, Rect r)
{
    char line[160];
    long long done = __atomic_load_n(&export_job.done, __ATOMIC_RELAXED), total = __atomic_load_n(&export_job.total, __ATOMIC_RELAXED);
    int percent = total > 0 ? (int)min(100, done * 100 / total) : 0, pw = width_text("100%") + 12 * unit;
    fill(c, r, BG);
    fill(c, rect(r.x, r.y, (int)((long long)r.w * min(percent, 100) / 100), r.h), 0x2f6f4fu);
    put(line, sizeof(line), "%d%%", percent);
    text(c, r.x + r.w - pw, r.y + r.h / 2, pw, line, TEXT);
    put(line, sizeof(line), "Exporting %s...", export_job.name);
    text(c, r.x + 10 * unit, r.y + r.h / 2, r.w - pw - 16 * unit, line, TEXT);
}
static void pack_set_poster(const char *path)
{
    int w, h;
    uint32_t *pixels = png_pixels(path, &w, &h);
    free(pixels);
    if (!pixels) { put(status, sizeof(status), "The poster must be a .png image."); return; }
    snprintf(pack_poster, sizeof(pack_poster), "%s", path);
    status[0] = 0;
}
static int pack_poll(void)
{
    char path[1024], why[300];
    int result;
    if (!pack_picking || !pack_picked) return 0;
    result = pack_picked(path, sizeof(path), why, sizeof(why));
    if (!result) return 0;
    pack_picking = 0;
    if (result == 1) {
        if (pack_kind == 0) pack_import(path);
        else if (pack_kind == 1) pack_set_poster(path);
        else pack_export(path);
    } else if (result == -2)
        put(status, sizeof(status), "%s", why);
    else
        status[0] = 0;
    return 1;
}
/* The file an export suggests: <modpacks folder>/<name>_<version>.zip */
static char export_suggestion[1300];
const char *ModsWindow_ExportSuggestion(void)
{
    char dir[1024], name[200], both[200];
    snprintf(both, sizeof(both), "%.90s_%.40s", pack_name, pack_version);
    folder_name(name, sizeof(name), both);
    if (Mods_ModpackDirectory(dir, sizeof(dir))) snprintf(export_suggestion, sizeof(export_suggestion), "%s.zip", name);
    else snprintf(export_suggestion, sizeof(export_suggestion), "%s/%s.zip", dir, name);
#ifdef _WIN32
    for (char *c = export_suggestion; *c; c++) if (*c == '/') *c = '\\';
#endif
    return export_suggestion;
}
static void pack_ask_file(int kind)
{
    char why[300] = "";
    if (pack_picking) return;
    if (!pack_pick) {
        put(status, sizeof(status), "This build has no file picker.");
        return;
    }
    pack_kind = kind;
    if (pack_pick(kind, why, sizeof(why))) put(status, sizeof(status), "%s", why);
    else pack_picking = 1;
}

/* --- The tab's layout, drawing and presses ---------------------------- */
typedef struct {
    Rect tab, list, detail, import, folder, export_;
    Rect activate, bar;                         /* the detail's foot: Activate modpack, its progress */
    Rect fields[4], poster, lock, save, cancel; /* Export's form */
} PackLayout;
static void pack_layout(const Layout *l, PackLayout *k)
{
    int b = 34 * unit, g = 10 * unit, x, y, w, w2;
    memset(k, 0, sizeof(*k));
    k->tab = rect(l->search.x, 104 * unit, l->search.w, 28 * unit); /* under the search, over the filter */
    if (!packs) return;
    k->list = l->list;
    k->detail = rect(l->detail.x, 64 * unit, l->detail.w, l->detail.y + l->detail.h - 64 * unit);
    x = k->detail.x + 16 * unit;
    w = min(260 * unit, (k->detail.w - 32 * unit - g) / 2);
    w2 = w;
    if (pack_form) {
        /* the fields, the description at least 132 high, the poster, the
         * lock and the hint line; the buttons are the footer's */
        int rest = 52 * unit + 3 * (b + g) + g + b + g + b + 40 * unit + LINE;
        int form_h = max(k->detail.h, rest + 132 * unit); /* the form; the preview follows it */
        pack_form_h = form_h + pack_preview_h;
        pack_form_scroll = max(0, min(pack_form_scroll, pack_form_h - k->detail.h));
        y = k->detail.y + 52 * unit - pack_form_scroll;
        for (int i = 0; i < 3; i++, y += b + g)
            k->fields[i] = rect(x + 90 * unit, y, k->detail.w - 122 * unit, b);
        int description_h = form_h - rest;
        k->fields[3] = rect(x + 90 * unit, y, k->detail.w - 122 * unit, description_h);
        y += description_h + g;
        k->poster = rect(x + 90 * unit, y, w, b);
        y += b + g;
        k->lock = rect(x, y, k->detail.w - 32 * unit, b);
        (void)w2;
        k->cancel = rect(l->close.x - 8 * unit - l->close.w, l->close.y, l->close.w, l->close.h);
        w = max(120 * unit, width_text("Choosing where...") + 24 * unit);
        k->save = rect(k->cancel.x - 8 * unit - w, l->close.y, w, l->close.h);
        return;
    }
    w = max(width_text("Restart to activate"), width_text("Activate modpack")) + 28 * unit;
    k->activate = rect(k->detail.x + k->detail.w - 16 * unit - w, k->detail.y + k->detail.h - 36 * unit, w, 28 * unit);
    k->bar = rect(k->detail.x + 16 * unit, k->activate.y, k->activate.x - k->detail.x - 26 * unit, 28 * unit);
    /* The Modpacks actions share the footer with Close. */
    g = 8 * unit;
    w = max(130 * unit, max(width_text("Import..."), max(width_text("Export..."), width_text("Open folder"))) + 24 * unit);
    w = min(w, (l->close.x - 20 * unit - 3 * g) / 3);
    x = l->close.x - 3 * (w + g);
    y = l->close.y;
    k->import = rect(x, y, w, l->close.h);
    k->export_ = rect(x + w + g, y, w, l->close.h);
    k->folder = rect(x + 2 * (w + g), y, w, l->close.h);
}
static int pack_rows(const PackLayout *k) { return max(1, k->list.h / (44 * unit)); }
/* --- The description, formatted as Discord formats a message ----------- *
 * Lines: "# ", "## ", "### " headings, "-# " small print, "> " a quote and
 * ">>> " the rest quoted, "- " or "* " a list item (two spaces before it a
 * level deeper), "1. " a numbered one, and ``` fences round a code block.
 * Inline: **bold**, *italic* or _italic_, __underline__, ~~strikethrough~~
 * (nested as in ***bold italic***), `code`, ||spoiler||, [text](link) and
 * plain links; "\" before a mark shows the mark. A mark with no partner
 * later shows as typed. The editor keeps the markup visible; empty lines are
 * kept. Without a canvas it only measures. */
enum { MD_BOLD = 1, MD_ITALIC = 2, MD_UNDERLINE = 4, MD_STRIKE = 8, MD_CODE = 16, MD_SPOILER = 32, MD_LINK = 64 };
#define MD_QUOTE_BAR 0x4e5058u
#define MD_CODE_BG 0x1a1d22u
#define MD_SPOILER_BG 0x15171bu
/* The same mark later in the text: its partner. */
static int md_partner(const char *after, const char *mark)
{
    return strstr(after, mark) != NULL;
}
/* A lone _ marks only at a word's edge (snake_case stays as typed). */
static int md_edge(const char *s, const char *start, int opening)
{
    unsigned char before = s > start ? (unsigned char)s[-1] : ' ', after = (unsigned char)s[1];
    return opening ? (isspace(before) || ispunct(before)) && after && !isspace(after)
                   : !isspace(before) && (!after || isspace(after) || ispunct(after));
}
/* The width of the word starting at `s` (to a space or the line's end), its
 * marks left out: a word that does not fit goes whole to the next line. */
static int md_word(const char *s, const char *end, int scale)
{
    char glyph[8];
    int w = 0;
    while (s < end && !isspace((unsigned char)*s)) {
        int n = 0;
        if (strchr("*_~`|\\", *s)) { s++; continue; }
        do { glyph[n++] = *s++; } while (n < 4 && s < end && ((unsigned char)*s & 0xc0) == 0x80);
        glyph[n] = 0;
        w += Menu_TextWidthScaled(glyph, unit * scale);
    }
    return w;
}
/* One glyph with its marks, at `at` on the line whose top is `top`, `lh` high. */
static void md_glyph(MenuCanvas *c, int at, int top, int lh, int scale, const char *g, int advance, unsigned color,
                     int flags)
{
    int mid = top + lh / 2, s = unit * scale;
    if (!c || top + lh <= 0 || top >= c->height) return;
    if (flags & MD_CODE) fill(c, rect(at, top + 2 * unit, advance, lh - 4 * unit), 0x363b46u); /* lighter than the panel */
    if (flags & MD_SPOILER) {
        fill(c, rect(at, top + 2 * unit, advance, lh - 4 * unit), MD_SPOILER_BG);
        color = 0x3a3e46u; /* hidden, as Discord's is until clicked */
    }
    if (flags & MD_ITALIC) {
        /* No italic face: the glyph drawn upright on a copy of what is
         * under it, then put back slanted, its top leaning right. */
        int w = advance + lh / 4 + 2 * unit, h = lh;
        uint32_t *before = malloc((size_t)w * h * 4), *after = malloc((size_t)w * h * 4);
        if (before && after) {
            MenuCanvas b = {0};
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    int px = at + x, py = top + y;
                    before[y * w + x] = px >= 0 && px < c->width && py >= 0 && py < c->height ? c->pixels[py * c->stride + px] : 0;
                }
            memcpy(after, before, (size_t)w * h * 4);
            b.pixels = after, b.stride = b.width = w, b.height = h;
            Menu_DrawTextScaled(&b, 0, h / 2, g, color, s);
            if (flags & MD_BOLD) Menu_DrawTextScaled(&b, unit, h / 2, g, color, s);
            for (int y = 0; y < h; y++) {
                int lean = (h - y) / 5, py = top + y;
                if (py < 0 || py >= c->height) continue;
                for (int x = 0; x < w; x++) {
                    int px = at + x + lean;
                    if (after[y * w + x] != before[y * w + x] && px >= 0 && px < c->width)
                        c->pixels[py * c->stride + px] = after[y * w + x];
                }
            }
        }
        free(before);
        free(after);
    } else {
        Menu_DrawTextScaled(c, at, mid, g, color, s);
        if (flags & MD_BOLD) Menu_DrawTextScaled(c, at + unit, mid, g, color, s);
    }
    if (flags & (MD_UNDERLINE | MD_LINK)) fill(c, rect(at, top + lh - 4 * unit, advance, unit), color);
    if (flags & MD_STRIKE) fill(c, rect(at, mid, advance, unit), color);
}
static int pack_description_draw(MenuCanvas *c, int x, int y, int w, const char *s, int formatted)
{
    const char *whole = s;
    int flags = 0, quote_rest = 0, code_block = 0;
    const char *link_text_end = NULL, *link_end = NULL, *url_end = NULL;
    if (!formatted) { /* as typed */
        int at = x;
        while (*s) {
            char glyph[8];
            int n = 0, advance;
            if (*s == '\r') { s++; continue; }
            if (*s == '\n') { y += LINE; at = x; s++; continue; }
            do { glyph[n++] = *s++; } while (n < 4 && ((unsigned char)*s & 0xc0) == 0x80);
            glyph[n] = 0;
            advance = width_text(glyph);
            if (at > x && at + advance > x + w) { y += LINE; at = x; }
            if (c && y + LINE > 0 && y < c->height) text(c, at, y + LINE / 2, advance + unit, glyph, TEXT);
            at += advance;
        }
        return y + LINE;
    }
    while (*s) {
        /* --- one line of the text: what it is, from its start --- */
        const char *end = strchr(s, '\n');
        int scale = 1, bold_line = 0, quote = quote_rest, indent = 0, lh, at, left, line_flags;
        unsigned color = TEXT;
        char marker[16] = "";
        if (!end) end = s + strlen(s);
        if (end - s >= 3 && !strncmp(s, "```", 3)) { /* a fence: not shown */
            code_block = !code_block;
            s = *end ? end + 1 : end;
            continue;
        }
        if (code_block) {
            /* the block's line as typed, on its dark ground */
            lh = LINE;
            at = x + 8 * unit;
            if (c) fill(c, rect(x, y, w, lh), MD_CODE_BG);
            for (const char *p = s; p < end;) {
                char glyph[8];
                int n = 0, advance;
                if (*p == '\r') { p++; continue; }
                do { glyph[n++] = *p++; } while (n < 4 && p < end && ((unsigned char)*p & 0xc0) == 0x80);
                glyph[n] = 0;
                advance = width_text(glyph);
                if (at > x + 8 * unit && at + advance > x + w - 8 * unit) {
                    y += lh;
                    at = x + 8 * unit;
                    if (c) fill(c, rect(x, y, w, lh), MD_CODE_BG);
                }
                md_glyph(c, at, y, lh, 1, glyph, advance, 0xd7dbe2u, 0);
                at += advance;
            }
            y += lh;
            s = *end ? end + 1 : end;
            continue;
        }
        if (!strncmp(s, ">>> ", 4)) { quote = quote_rest = 1; s += 4; }
        else if (!strncmp(s, "> ", 2)) { quote = 1; s += 2; }
        if (!strncmp(s, "### ", 4)) { bold_line = 1; color = 0xd5d9e0u; s += 4; }
        else if (!strncmp(s, "## ", 3)) { bold_line = 1; s += 3; }
        else if (!strncmp(s, "# ", 2)) { scale = 2; bold_line = 1; s += 2; }
        else if (!strncmp(s, "-# ", 3)) { color = DIM; s += 3; }
        else {
            const char *p = s;
            int spaces = 0;
            while (*p == ' ') { spaces++; p++; }
            if ((p[0] == '-' || p[0] == '*') && p[1] == ' ') {
                indent = 1 + spaces / 2;
                snprintf(marker, sizeof(marker), "%s", "\x01"); /* a bullet */
                s = p + 2;
            } else if (isdigit((unsigned char)p[0])) {
                const char *q = p;
                while (isdigit((unsigned char)*q) && q - p < 9) q++;
                if (q[0] == '.' && q[1] == ' ') {
                    indent = 1 + spaces / 2;
                    snprintf(marker, sizeof(marker), "%.*s.", (int)(q - p), p);
                    s = q + 2;
                }
            }
        }
        lh = LINE * scale + (bold_line && scale == 1 ? 4 * unit : 0);
        left = x + (quote ? 14 * unit : 0) + indent * 18 * unit;
        at = left;
        if (c && quote) fill(c, rect(x, y, 4 * unit, lh), MD_QUOTE_BAR);
        if (c && *marker) {
            if (marker[0] == '\x01') {
                int d = 5 * unit;
                if (indent > 1) { /* a deeper level: a hollow square */
                    fill(c, rect(left - 11 * unit, y + lh / 2 - d / 2, d, d), color);
                    fill(c, rect(left - 11 * unit + unit, y + lh / 2 - d / 2 + unit, d - 2 * unit, d - 2 * unit), PANEL);
                } else
                    fill(c, rect(left - 11 * unit, y + lh / 2 - d / 2, d, d), color);
            } else
                text(c, left - 4 * unit - width_text(marker), y + lh / 2, width_text(marker) + unit, marker, DIM);
        }
        line_flags = bold_line ? MD_BOLD : 0;
        /* --- its characters --- */
        while (s < end) {
            char glyph[8];
            int n = 0, advance, f;
            unsigned col = color;
            if (*s == '\r') { s++; continue; }
            if (link_end && s == link_text_end) { s = url_end + 1; link_end = link_text_end = url_end = NULL; flags &= ~MD_LINK; continue; }
            if (*s == '\\' && s + 1 < end && strchr("*_~`|>#-[\\", s[1])) {
                s++; /* the mark shown as typed */
            } else if (flags & MD_CODE) {
                if (*s == '`') { flags &= ~MD_CODE; s++; continue; }
            } else if (*s == '`' && md_partner(s + 1, "`")) { flags |= MD_CODE; s++; continue; }
            else if (!strncmp(s, "||", 2) && ((flags & MD_SPOILER) || md_partner(s + 2, "||"))) {
                flags ^= MD_SPOILER; s += 2; continue;
            } else if (!strncmp(s, "~~", 2) && ((flags & MD_STRIKE) || md_partner(s + 2, "~~"))) {
                flags ^= MD_STRIKE; s += 2; continue;
            } else if (!strncmp(s, "**", 2) && ((flags & MD_BOLD) || md_partner(s + 2, "**"))) {
                flags ^= MD_BOLD; s += 2; continue;
            } else if (!strncmp(s, "__", 2) && ((flags & MD_UNDERLINE) || md_partner(s + 2, "__"))) {
                flags ^= MD_UNDERLINE; s += 2; continue;
            } else if (*s == '*' && ((flags & MD_ITALIC) ? 1 : s[1] != ' ' && md_partner(s + 1, "*"))) {
                flags ^= MD_ITALIC; s++; continue;
            } else if (*s == '_' && md_edge(s, whole, !(flags & MD_ITALIC)) && ((flags & MD_ITALIC) || md_partner(s + 1, "_"))) {
                flags ^= MD_ITALIC; s++; continue;
            } else if (*s == '[' && !link_end) {
                const char *close = strstr(s, "](");
                const char *paren = close ? strchr(close + 2, ')') : NULL;
                if (close && paren && close < end && paren < end && close > s + 1 &&
                    (!strncmp(close + 2, "http://", 7) || !strncmp(close + 2, "https://", 8))) {
                    link_text_end = close;
                    link_end = close;
                    url_end = paren;
                    flags |= MD_LINK;
                    s++;
                    continue;
                }
            }
            f = flags | line_flags;
            if (!(f & MD_CODE) && (s == whole || isspace((unsigned char)s[-1])) &&
                (!strncmp(s, "https://", 8) || !strncmp(s, "http://", 7))) {
                /* a plain link: blue to the next space */
                while (s < end && !isspace((unsigned char)*s)) {
                    char g2[8];
                    int m = 0, a2;
                    do { g2[m++] = *s++; } while (m < 4 && s < end && ((unsigned char)*s & 0xc0) == 0x80);
                    g2[m] = 0;
                    a2 = Menu_TextWidthScaled(g2, unit * scale);
                    if (at > left && at + a2 > x + w) {
                        y += lh; at = left;
                        if (c && quote) fill(c, rect(x, y, 4 * unit, lh), MD_QUOTE_BAR);
                    }
                    md_glyph(c, at, y, lh, scale, g2, a2, BLUE, f | MD_LINK);
                    at += a2;
                }
                continue;
            }
            if (at > left && !isspace((unsigned char)*s) && isspace((unsigned char)s[-1])) {
                int word = md_word(s, end, scale);
                if (word <= w - (left - x) && at + word > x + w) { /* the word moves down whole */
                    y += lh;
                    at = left;
                    if (c && quote) fill(c, rect(x, y, 4 * unit, lh), MD_QUOTE_BAR);
                }
            }
            do { glyph[n++] = *s++; } while (n < 4 && s < end && ((unsigned char)*s & 0xc0) == 0x80);
            glyph[n] = 0;
            advance = Menu_TextWidthScaled(glyph, unit * scale) + ((f & MD_BOLD) ? unit : 0);
            if (at == left && glyph[0] == ' ' && !glyph[1] && s - 1 > whole && s[-2] != '\n')
                continue; /* the space a line broke at */
            if (at > left && at + advance > x + w) {
                y += lh;
                at = left;
                if (c && quote) fill(c, rect(x, y, 4 * unit, lh), MD_QUOTE_BAR);
            }
            if (f & MD_LINK) col = BLUE;
            if (f & MD_CODE) col = 0xd7dbe2u;
            md_glyph(c, at, y, lh, scale, glyph, advance, col, f);
            at += advance;
        }
        y += lh;
        if (!*end) break;
        s = end + 1;
        if (!*s) { /* a last empty line still takes its room, as typed */
            y += LINE;
            break;
        }
    }
    return *whole ? y : y + LINE; /* nothing typed: one empty line */
}
static void description_reveal(const PackLayout *k)
{
    int x, y, h = k->fields[3].h - 12 * unit;
    TextEdit_Position(pack_description, description_edit.cursor, k->fields[3].w - 20 * unit,
                      LINE, width_text, &x, &y);
    if (y < description_scroll) description_scroll = y;
    if (y + LINE > description_scroll + h) description_scroll = y + LINE - h;
}
static int description_insert(const char *s)
{
    if (!TextEdit_Insert(&description_edit, pack_description, sizeof(pack_description), s)) {
        put(status, sizeof(status), "Description is too long (maximum 8191 UTF-8 bytes). Nothing was pasted.");
        return 0;
    }
    status[0] = 0;
    return 1;
}
static void description_paint(MenuCanvas *c, int offset, int w)
{
    int x = 0, y = offset;
    size_t lo = min(description_edit.cursor, description_edit.anchor);
    size_t hi = max(description_edit.cursor, description_edit.anchor);
    for (size_t i = 0;;) {
        size_t next = TextEdit_Next(pack_description, i), n = next - i;
        char glyph[8];
        if (n > 4) n = 4;
        memcpy(glyph, pack_description + i, n); glyph[n] = 0;
        int advance = pack_description[i] == '\n' ? 0 : width_text(glyph);
        if (x && x + advance > w) { x = 0; y += LINE; }
        if (pack_focus == 4 && i >= lo && i < hi)
            fill(c, rect(x, y, max(advance, 5 * unit), LINE), ACCENT);
        if (pack_focus == 4 && i == description_edit.cursor)
            fill(c, rect(x, y, max(1, unit), LINE), TEXT);
        if (!pack_description[i]) break;
        if (pack_description[i] == '\n') { x = 0; y += LINE; }
        else {
            text(c, x, y + LINE / 2, advance + unit, glyph, TEXT);
            x += advance;
        }
        i = next;
    }
}
static int description_key(const MenuEvent *e)
{
    Layout l;
    PackLayout k;
    int shift = (e->modifiers & MENU_MOD_SHIFT) != 0, ctrl = (e->modifiers & MENU_MOD_CTRL) != 0;
    size_t n = strlen(pack_description), at = description_edit.cursor;
    if (e->type != MENU_EVENT_KEY_DOWN && e->type != MENU_EVENT_TEXT) return 0;
    layout(&l); pack_layout(&l, &k);
    if (e->type == MENU_EVENT_TEXT) {
        if (!ctrl) description_insert(e->text);
    } else if (ctrl && e->key == MENU_KEY_A) {
        description_edit.anchor = 0; description_edit.cursor = n;
    } else if (ctrl && (e->key == MENU_KEY_C || e->key == MENU_KEY_X)) {
        size_t lo = min(at, description_edit.anchor), hi = max(at, description_edit.anchor);
        if (hi > lo) {
            char copy[sizeof(pack_description)];
            memcpy(copy, pack_description + lo, hi - lo); copy[hi - lo] = 0;
            if (!clipboard_write || !clipboard_write(copy))
                put(status, sizeof(status), "Could not copy the selected text.");
            else if (e->key == MENU_KEY_X) description_insert("");
        }
    } else if (ctrl && e->key == MENU_KEY_V) {
        char paste[sizeof(pack_description)];
        if (!clipboard_read || !clipboard_read(paste, sizeof(paste)))
            put(status, sizeof(status), "Could not paste: clipboard unavailable or text exceeds 8191 UTF-8 bytes.");
        else description_insert(paste);
    } else if (e->key == MENU_KEY_LEFT || e->key == MENU_KEY_RIGHT) {
        int right = e->key == MENU_KEY_RIGHT;
        if (!shift && at != description_edit.anchor)
            at = right ? max(at, description_edit.anchor) : min(at, description_edit.anchor);
        else if (ctrl) {
            if (right) {
                while (at < n && !isspace((unsigned char)pack_description[at])) at = TextEdit_Next(pack_description, at);
                while (at < n && isspace((unsigned char)pack_description[at])) at++;
            } else {
                while (at && isspace((unsigned char)pack_description[at - 1])) at--;
                while (at && !isspace((unsigned char)pack_description[at - 1])) at = TextEdit_Previous(pack_description, at);
            }
        } else at = right ? TextEdit_Next(pack_description, at) : TextEdit_Previous(pack_description, at);
        TextEdit_Move(&description_edit, at, shift);
    } else if (e->key == MENU_KEY_HOME || e->key == MENU_KEY_END) {
        if (ctrl) at = e->key == MENU_KEY_HOME ? 0 : n;
        else if (e->key == MENU_KEY_HOME) { while (at && pack_description[at - 1] != '\n') at--; }
        else { while (pack_description[at] && pack_description[at] != '\n') at++; }
        TextEdit_Move(&description_edit, at, shift);
    } else if (e->key == MENU_KEY_UP || e->key == MENU_KEY_DOWN ||
               e->key == MENU_KEY_PAGE_UP || e->key == MENU_KEY_PAGE_DOWN) {
        int x, y, w = k.fields[3].w - 20 * unit;
        int step = (e->key == MENU_KEY_PAGE_UP || e->key == MENU_KEY_PAGE_DOWN) ? max(LINE, k.fields[3].h - LINE) : LINE;
        TextEdit_Position(pack_description, at, w, LINE, width_text, &x, &y);
        if (description_edit.preferred_x >= 0) x = description_edit.preferred_x;
        if (e->key == MENU_KEY_UP || e->key == MENU_KEY_PAGE_UP) step = -step;
        TextEdit_Move(&description_edit, TextEdit_Hit(pack_description, w, LINE, width_text, x, y + step), shift);
        description_edit.preferred_x = x;
    } else if (e->key == MENU_KEY_BACKSPACE || e->key == MENU_KEY_DELETE) {
        if (at == description_edit.anchor)
            description_edit.anchor = e->key == MENU_KEY_BACKSPACE ? TextEdit_Previous(pack_description, at)
                                                                  : TextEdit_Next(pack_description, at);
        description_insert("");
    } else if (e->key == MENU_KEY_ENTER && e->button != ' ') description_insert("\n");
    else if (e->key == MENU_KEY_TAB || e->key == MENU_KEY_ESCAPE) pack_focus = 0;
    /* Printable keys, including Space, arrive once through TEXT_INPUT. */
    description_reveal(&k);
    return 1;
}
/* The poster, scaled into `r` (nearest pixel), letterboxed. */
/* What the detail shows of a modpack, and the Export form's preview of the
 * one being made: drawn the same way by pack_view_draw. */
typedef struct {
    const char *name, *version, *author, *description;
    int locked;
    const uint32_t *poster;
    int poster_w, poster_h;
    int mods;                                                    /* how many cards */
    void (*mod)(int i, const char **name, const char **author); /* the i-th card's */
} PackView;
static void poster_paint(MenuCanvas *c, Rect r, const uint32_t *pixels, int pw, int ph)
{
    int w = r.w, h = r.w * ph / max(1, pw);
    if (h > r.h) { h = r.h; w = r.h * pw / max(1, ph); }
    for (int y = 0; y < h; y++) {
        int py = r.y + y, sy = y * ph / h;
        if (py < 0 || py >= c->height) continue;
        for (int x = 0; x < w; x++) {
            int px = r.x + (r.w - w) / 2 + x;
            if (px >= 0 && px < c->width)
                c->pixels[py * c->stride + px] = 0xff000000u | pixels[(size_t)sy * pw + x * pw / w];
        }
    }
}
/* Poster, name, version and author, description, the lock and a card per mod
 * from `y`; the y under them. */
static int pack_view_draw(MenuCanvas *c, int x, int y, int w, const PackView *v)
{
    char line[512];
    if (v->poster) {
        /* Twice the previous 320x180 box, constrained only by the panel width. */
        int bw = min(w, 640 * unit), bh = min(360 * unit, bw * v->poster_h / max(1, v->poster_w));
        Rect r = rect(x, y, bw, bh);
        poster_paint(c, r, v->poster, v->poster_w, v->poster_h);
        y += r.h + 8 * unit;
    }
    text(c, x, y + 10 * unit, w, v->name, TEXT);
    y += 24 * unit;
    if (*v->version || *v->author) {
        put(line, sizeof(line), "%s%s%s%s", *v->version ? "Version " : "", v->version,
            *v->version && *v->author ? "  /  " : "", v->author);
        text(c, x, y + 8 * unit, w, line, DIM);
        y += 22 * unit;
    }
    if (*v->description) {
        y = pack_description_draw(c, x, y, w, v->description, 1);
        y += 4 * unit;
    }
    if (v->locked) {
        text(c, x, y + 8 * unit, w, "Settings locked by its author", WARN);
        y += 22 * unit;
    }
    if (v->mods) {
        /* A card per mod: its name on the left, its author in a chip on
         * the right. */
        int card = 24 * unit, gap = 4 * unit;
        put(line, sizeof(line), "Mods (%d)", v->mods);
        text(c, x, y + 10 * unit, w, line, DIM);
        y += 22 * unit;
        for (int i = 0; i < v->mods; i++) {
            const char *name = "?", *who = "";
            v->mod(i, &name, &who);
            int chip_w = *who ? min(width_text(who) + 16 * unit, w / 2) : 0;
            Rect row = rect(x, y, w, card), chip = rect(x + w - chip_w - 4 * unit, y + 3 * unit, chip_w, card - 6 * unit);
            fill(c, row, i % 2 ? 0x262b33u : 0x2a303au);
            fill(c, rect(x, y, 3 * unit, card), ACCENT); /* a stripe on the card's left */
            text(c, x + 12 * unit, y + card / 2, w - chip_w - 24 * unit, name, TEXT);
            if (chip_w) {
                fill(c, chip, 0x1f3354u);
                centred(c, chip, who, BLUE);
            }
            y += card + gap;
        }
    }
    return y;
}
/* The shown modpack's i-th mod, from its modpack.json. */
static void detail_mod(int i, const char **name, const char **author)
{
    const JsonValue *m = Json_At(Json_Member(Json_Root(pack_info), "mods"), i);
    *name = Json_String(Json_Member(m, "name"), "?");
    *author = Json_String(Json_Member(m, "author"), "");
}
/* The Export form's preview: the i-th ticked mod. */
static void export_preview_mod(int i, const char **name, const char **author)
{
    for (int j = 0; j < export_count; j++)
        if (export_mods[j].selected && !i--) {
            *name = export_name(j);
            *author = Json_String(Json_Member(Json_Root(export_mods[j].manifest), "author"), "");
            return;
        }
}
/* The poster chosen for the export, read once per path for the preview. */
static uint32_t *preview_pixels;
static int preview_w, preview_h;
static char preview_of[1024];
static void preview_poster(void)
{
    if (!strcmp(preview_of, pack_poster)) return;
    free(preview_pixels);
    preview_pixels = pack_poster[0] ? png_pixels(pack_poster, &preview_w, &preview_h) : NULL;
    snprintf(preview_of, sizeof(preview_of), "%s", pack_poster);
}
/* A scroll bar at the right of `r` for content scrolled by `at` of `max` more
 * than shows (none when it all shows). */
static void pack_scrollbar(MenuCanvas *c, Rect r, int at, int max_)
{
    if (max_ <= 0) return;
    int h = max(20 * unit, r.h * r.h / (r.h + max_));
    Rect track = rect(r.x + r.w - BAR - 2 * unit, r.y, BAR, r.h);
    fill(c, track, BG);
    fill(c, rect(track.x, track.y + (track.h - h) * min(at, max_) / max_, BAR, h), DIM);
}
static void draw_form(MenuCanvas *c, const PackLayout *k)
{
    static const char *labels[4] = {"Name *", "Version *", "Author *", "Description"};
    char *values_[4] = {pack_name, pack_version, pack_author, pack_description};
    char line[sizeof(pack_description) + 2];
    int x = k->detail.x + 16 * unit;
    put(line, sizeof(line), "%d of %d mods selected. Tick the mods to include in this export.", export_selected(), export_count);
    text(c, x, k->detail.y + 24 * unit - pack_form_scroll, k->detail.w - 32 * unit, line, DIM);
    for (int i = 0; i < 4; i++) {
        Rect f = k->fields[i];
        const char *shown_ = line;
        text(c, x, f.y + f.h / 2, 86 * unit, labels[i], TEXT);
        fill(c, f, pack_focus == i + 1 ? 0x2b3a52u : BG);
        snprintf(line, sizeof(line), "%s%s", values_[i], pack_focus == i + 1 ? "_" : "");
        if (i == 3) {
            MenuCanvas field = *c;
            int top = max(0, f.y + 6 * unit), bottom = min(c->height, f.y + f.h - 6 * unit);
            if (bottom <= top) continue;
            field.pixels += top * c->stride + f.x + 10 * unit;
            field.width = f.w - 20 * unit;
            field.height = bottom - top;
            description_max = max(0, pack_description_draw(NULL, 0, 0, field.width, pack_description, 0) - (f.h - 12 * unit));
            description_scroll = min(description_scroll, description_max);
            description_paint(&field, f.y + 6 * unit - top - description_scroll, field.width);
            continue;
        }
        while (*shown_ && width_text(shown_) > f.w - 20 * unit) /* a long one shows its end, where the typing is */
            do shown_++; while (((unsigned char)*shown_ & 0xC0) == 0x80);
        text(c, f.x + 10 * unit, f.y + f.h / 2, f.w - 20 * unit, shown_, TEXT);
    }
    text(c, x, k->poster.y + k->poster.h / 2, 86 * unit, "Poster", TEXT);
    button(c, k->poster, pack_picking && pack_kind == 1 ? "Choosing..." : "Choose .png (optional)...", 0);
    {
        const char *base = pack_poster, *s;
        for (s = pack_poster; *s; s++)
            if (*s == '/' || *s == '\\') base = s + 1;
        text(c, k->poster.x + k->poster.w + 10 * unit, k->poster.y + k->poster.h / 2,
             k->detail.x + k->detail.w - 16 * unit - (k->poster.x + k->poster.w + 10 * unit),
             pack_poster[0] ? base : "none", pack_poster[0] ? GREEN : DIM);
    }
    text(c, x, k->lock.y + k->lock.h / 2, 30 * unit, pack_lock ? "[x]" : "[ ]", pack_lock ? GREEN : DIM);
    text(c, x + 34 * unit, k->lock.y + k->lock.h / 2, k->lock.w - 34 * unit,
         "Lock the settings (players cannot change them)", TEXT);
    text(c, x, k->lock.y + k->lock.h + 16 * unit, k->detail.w - 32 * unit,
         "Enter: new line | Win + .: emoji | Discord formatting:", DIM);
    text(c, x, k->lock.y + k->lock.h + 16 * unit + LINE, k->detail.w - 32 * unit,
         "**bold** *italic* __underline__ ~~strike~~ `code` ||spoiler|| # heading > quote - list", DIM);
    {   /* Preview: the modpack as the one who imports it will see it */
        PackView v = {0};
        int top = k->lock.y + k->lock.h + 40 * unit + LINE, y, w = k->detail.w - 32 * unit - BAR;
        preview_poster();
        v.name = pack_name[0] ? pack_name : "Modpack name";
        v.version = pack_version;
        v.author = pack_author;
        v.description = pack_description;
        v.locked = pack_lock;
        v.poster = preview_pixels, v.poster_w = preview_w, v.poster_h = preview_h;
        v.mods = export_selected();
        v.mod = export_preview_mod;
        text(c, x, top + 10 * unit, w, "Preview", TEXT);
        fill(c, rect(x, top + 24 * unit, w, 1 * unit), EDGE);
        y = pack_view_draw(c, x + 12 * unit, top + 36 * unit, w - 24 * unit, &v);
        /* its height from the form's end, for the next layout's scrolling */
        pack_preview_h = y + 16 * unit - top;
    }
}
static void draw_activate(MenuCanvas *c, const PackLayout *k, int x);
static void draw_packs(MenuCanvas *c, const Layout *l)
{
    PackLayout k;
    int pitch = 44 * unit, y, x, w;
    char line[600];
    pack_layout(l, &k);
    fill(c, l->search, pack_focus == 5 ? 0x2b3a52u : PANEL);
    snprintf(line, sizeof(line), "%s%s", pack_query, pack_focus == 5 ? "_" : "");
    text(c, l->search.x + 10 * unit, l->search.y + l->search.h / 2, l->search.w - 20 * unit,
         line[0] ? line : pack_form ? "Search mods, IDs or authors..." : "Search modpacks...", line[0] ? TEXT : DIM);
    button(c, l->filter, pack_filter == 1 ? "Selected" : pack_filter == 2 ? "Not selected" :
           pack_form ? "All mods" : "All modpacks", 0);
    fill(c, k.list, PANEL);
    if (pack_form) {
        export_scroll = min(export_scroll, max(0, pack_visible_count() - pack_rows(&k)));
        for (int r = 0; r < pack_rows(&k); r++) {
            int i = pack_shown(export_scroll + r);
            Rect row = rect(k.list.x, k.list.y + r * pitch, k.list.w, pitch - 2 * unit);
            if (i < 0) break;
            if (i == export_sel) fill(c, row, 0x293e60u);
            text(c, row.x + 12 * unit, row.y + 14 * unit, 24 * unit,
                 export_mods[i].selected ? "[x]" : "[ ]", export_mods[i].selected ? GREEN : DIM);
            text(c, row.x + 44 * unit, row.y + 14 * unit, row.w - 56 * unit, export_name(i), TEXT);
            text(c, row.x + 44 * unit, row.y + 32 * unit, row.w - 56 * unit, export_mods[i].id, DIM);
        }
        if (!pack_visible_count())
            wrap(c, k.list.x + 12 * unit, k.list.y + 16 * unit, k.list.w - 24 * unit,
                 export_count ? "No matching mods." : "No mods found in the mods folders.", DIM);
        pack_scrollbar(c, k.list, export_scroll * pitch, max(0, pack_visible_count() - pack_rows(&k)) * pitch);
        fill(c, k.detail, PANEL);
        MenuCanvas form = *c;
        form.pixels += k.detail.y * c->stride + k.detail.x;
        form.width = k.detail.w;
        form.height = k.detail.h;
        Rect whole = k.detail;
        Rect *fields[] = {&k.fields[0], &k.fields[1], &k.fields[2], &k.fields[3], &k.poster, &k.lock};
        for (int i = 0; i < 6; i++) { fields[i]->x -= k.detail.x; fields[i]->y -= k.detail.y; }
        k.detail.x = k.detail.y = 0;
        draw_form(&form, &k);
        pack_scrollbar(c, whole, pack_form_scroll, pack_form_h - whole.h);
        /* the form's buttons, in the footer beside Close */
        button(c, k.save, export_job.running ? "Exporting..." : pack_picking && pack_kind == 2 ? "Choosing where..." : "Export...",
               !export_job.running);
        button(c, k.cancel, export_job.running ? "Stop" : "Cancel", 0);
        return;
    }
    pack_scroll = min(pack_scroll, max(0, pack_visible_count() - pack_rows(&k)));
    if (!pack_visible_count())
        text(c, k.list.x + 12 * unit, k.list.y + 22 * unit, k.list.w - 24 * unit, "No matching modpacks.", DIM);
    for (int r = 0; r < pack_rows(&k); r++) {
        int i = pack_shown(pack_scroll + r); /* 0: no modpack; i: pack_names[i - 1] */
        Rect row = rect(k.list.x, k.list.y + r * pitch, k.list.w, pitch - 2 * unit);
        if (i < 0) break;
        const char *name = i ? pack_names[i - 1] : "No modpack (normal mods)";
        int chosen = i ? !strcmp(pack_chosen, name) : !pack_chosen[0];
        int running = i ? !strcmp(Mods_ModpackLaunched(), name) : !Mods_ModpackLaunched()[0];
        if (i == pack_sel) fill(c, row, 0x293e60u);
        text(c, row.x + 12 * unit, row.y + 14 * unit, 24 * unit, chosen ? "(o)" : pack_confirm == i + 1 ? "(?)" : "( )",
             chosen ? GREEN : pack_confirm == i + 1 ? WARN : DIM);
        text(c, row.x + 44 * unit, row.y + 14 * unit, row.w - 56 * unit, name, TEXT);
        if (i) snprintf(line, sizeof(line), "%d mod(s)%s", pack_mods[i - 1], running ? " / running now" : "");
        else snprintf(line, sizeof(line), "%s", running ? "running now" : "the mods folder's mods");
        text(c, row.x + 44 * unit, row.y + 32 * unit, row.w - 56 * unit, line, running ? GREEN : DIM);
    }
    fill(c, k.detail, PANEL);
    pack_load_info();
    MenuCanvas *outer = c, viewport = *c;
    Rect detail_bounds = k.detail;
    viewport.pixels += k.detail.y * c->stride + k.detail.x;
    viewport.width = k.detail.w - BAR - 4 * unit;
    viewport.height = k.detail.h - 40 * unit;
    c = &viewport;
    k.detail = rect(0, 0, viewport.width, viewport.height);
    x = k.detail.x + 16 * unit;
    w = k.detail.w - 32 * unit;
    y = 12 * unit - pack_detail_scroll;
    {
        const JsonValue *root = pack_info ? Json_Root(pack_info) : NULL;
        PackView v = {0};
        v.name = pack_sel ? Json_String(Json_Member(root, "name"), pack_names[pack_sel - 1]) : "No modpack (normal mods)";
        v.version = Json_String(Json_Member(root, "version"), "");
        v.author = Json_String(Json_Member(root, "author"), "");
        v.description = Json_String(Json_Member(root, "description"), "");
        v.locked = pack_sel && Json_Bool(Json_Member(root, "locked"), 0);
        v.poster = poster_pixels, v.poster_w = poster_w, v.poster_h = poster_h;
        v.mods = root ? Json_Count(Json_Member(root, "mods")) : 0;
        v.mod = detail_mod;
        y = pack_view_draw(c, x, y, w, &v);
        if (v.mods) {
        } else if (pack_sel) {
            put(line, sizeof(line), "%d mod(s), all on when the game starts with this modpack.", pack_mods[pack_sel - 1]);
            y = wrap(c, x, y, w, line, DIM);
        } else
            y = wrap(c, x, y, w, "The game starts with the mods of the mods folder, as without modpacks.", DIM);
    }
    pack_detail_max = max(0, y + pack_detail_scroll + 12 * unit - viewport.height);
    pack_detail_scroll = min(pack_detail_scroll, pack_detail_max);
    c = outer;
    k.detail = detail_bounds;
    x = k.detail.x + 16 * unit;
    if (pack_detail_max) {
        int h = max(20 * unit, viewport.height * viewport.height / (viewport.height + pack_detail_max));
        Rect track = rect(k.detail.x + k.detail.w - BAR - 2 * unit, k.detail.y, BAR, viewport.height);
        fill(c, track, BG);
        fill(c, rect(track.x, track.y + (track.h - h) * pack_detail_scroll / pack_detail_max, BAR, h), DIM);
    }
    draw_activate(c, &k, x);
    button(c, k.import, import_job.running ? "Stop" : pack_picking && pack_kind == 0 ? "Choosing..." : "Import...", 0);
    button(c, k.export_, "Export...", 0);
    button(c, k.folder, "Open folder", 0);
}
static void pack_choose(void)
{
    const char *name = pack_sel ? pack_names[pack_sel - 1] : "";
    if (Mods_ModpackSelect(name)) {
        put(status, sizeof(status), "Could not save the choice of modpack.");
        return;
    }
    snprintf(pack_chosen, sizeof(pack_chosen), "%s", name);
    put(status, sizeof(status), pack_restart() ? "Chosen. Restart to start the game with it." : "Chosen: it is the one running now.");
}
/* --- Activate modpack: its files read once before the restart ---------- *
 * A thread of its own reads every file of the pack (Mods_ModpackPrepare):
 * the SHA-256 check the start would make is made now, with a bar, and its
 * record lets the start skip it; the reading leaves the files in the
 * system's cache, so the restart loads them sooner. The pack is chosen
 * only when it passed; then the foot offers Restart to activate. */
static struct {
    pthread_t thread;
    int running;                 /* from prep_start until prep_poll joins it */
    int finished, ok, cancel;    /* atomic */
    long long done, total;       /* atomic, bytes */
    int shown;                   /* the permille last drawn */
    char name[128], why[300];
} prep;
static void *prep_step(void *unused)
{
    int ok;
    (void)unused;
    ok = Mods_ModpackPrepare(prep.name, &prep.done, &prep.total, &prep.cancel, prep.why, sizeof(prep.why));
    __atomic_store_n(&prep.ok, ok, __ATOMIC_RELAXED);
    __atomic_store_n(&prep.finished, 1, __ATOMIC_RELEASE);
    return NULL;
}
/* The selected row's pack, read before it is chosen (no modpack: chosen at once). */
static void prep_start(void)
{
    if (prep.running) return;
    pack_ask = 0;
    if (!pack_sel) {
        pack_choose();
        return;
    }
    snprintf(prep.name, sizeof(prep.name), "%s", pack_names[pack_sel - 1]);
    prep.why[0] = 0;
    prep.done = prep.total = 0;
    prep.cancel = prep.finished = prep.ok = 0;
    prep.shown = -1;
    if (pthread_create(&prep.thread, NULL, prep_step, NULL)) {
        put(status, sizeof(status), "Could not start loading the modpack.");
        return;
    }
    prep.running = 1;
    put(status, sizeof(status), "Loading %s: reading and checking its files before the restart.", prep.name);
}
/* Each tick: 1 when the bar moved or the reading ended (a redraw). */
static int prep_poll(void)
{
    long long done, total;
    int permille;
    if (!prep.running) return 0;
    if (!__atomic_load_n(&prep.finished, __ATOMIC_ACQUIRE)) {
        done = __atomic_load_n(&prep.done, __ATOMIC_RELAXED);
        total = __atomic_load_n(&prep.total, __ATOMIC_RELAXED);
        permille = total > 0 ? (int)(done * 1000 / total) : 0;
        if (permille == prep.shown) return 0;
        prep.shown = permille;
        return 1;
    }
    pthread_join(prep.thread, NULL);
    prep.running = 0;
    if (!prep.ok)
        put(status, sizeof(status), "%s was not activated: %s", prep.name, prep.why);
    else if (Mods_ModpackSelect(prep.name))
        put(status, sizeof(status), "Could not save the choice of modpack.");
    else {
        snprintf(pack_chosen, sizeof(pack_chosen), "%s", prep.name);
        put(status, sizeof(status), pack_restart() ? "%s is loaded. Restart to activate it." : "%s is the one running now.",
            prep.name);
    }
    return 1;
}
/* What the foot's button is: 0 none, 1 Activate, 2 Restart to activate, 3 Cancel. */
static int activate_kind(void)
{
    const char *row = pack_sel ? pack_names[pack_sel - 1] : "";
    if (prep.running) return 3;
    if (strcmp(row, pack_chosen)) return 1;
    return pack_restart() ? 2 : 0;
}
static void draw_activate(MenuCanvas *c, const PackLayout *k, int x)
{
    char line[512];
    int kind = activate_kind();
    if (kind == 3) {
        long long done = __atomic_load_n(&prep.done, __ATOMIC_RELAXED);
        long long total = __atomic_load_n(&prep.total, __ATOMIC_RELAXED);
        int percent = total > 0 ? (int)(done * 100 / total) : 0;
        Rect bar = k->bar;
        fill(c, bar, BG);
        fill(c, rect(bar.x, bar.y, (int)((long long)bar.w * min(percent, 100) / 100), bar.h), 0x2f6f4fu);
        put(line, sizeof(line), "%d%%", percent);
        int pw = width_text("100%") + 12 * unit; /* the share keeps its place at the bar's right */
        text(c, bar.x + bar.w - pw, bar.y + bar.h / 2, pw, line, TEXT);
        put(line, sizeof(line), "Loading %s...", prep.name);
        text(c, bar.x + 10 * unit, bar.y + bar.h / 2, bar.w - pw - 16 * unit, line, TEXT);
        button(c, k->activate, "Cancel", 0);
        return;
    }
    put(line, sizeof(line), "Next launch: %s", pack_chosen[0] ? pack_chosen : "no modpack (normal mods)");
    text(c, x, k->detail.y + k->detail.h - 22 * unit, kind ? k->activate.x - x - 10 * unit : k->detail.w - 32 * unit, line,
         pack_restart() ? WARN : GREEN);
    if (kind == 1) button(c, k->activate, pack_sel ? "Activate modpack" : "Use normal mods", 1);
    if (kind == 2) button(c, k->activate, pack_ask == 2 ? "Restart now" : "Restart to activate", 1);
}
/* Restart (the footer's, or Restart to activate): asked once, then done. */
static void pack_restart_press(void)
{
    if (!pack_restart()) {
        put(status, sizeof(status), "Nothing to restart for: the chosen modpack is running.");
        return;
    }
    if (pack_ask != 2) {
        pack_ask = 2;
        put(status, sizeof(status), "Restart the game now? Unsaved game progress will be lost. Click Restart again.");
        return;
    }
    pack_ask = 0;
    if (Platform_RestartGame() < 0) put(status, sizeof(status), "Restart failed; relaunch the game.");
}
/* Typing into Export's fields: 1 when the event was one. */
static int pack_type(const MenuEvent *e)
{
    char *target = pack_focus == 5 ? pack_query : pack_focus == 1 ? pack_name : pack_focus == 2 ? pack_version : pack_focus == 3 ? pack_author : pack_description;
    size_t cap = pack_focus == 5 ? sizeof(pack_query) : pack_focus == 1 ? sizeof(pack_name) : pack_focus == 2 ? sizeof(pack_version)
               : pack_focus == 3 ? sizeof(pack_author) : sizeof(pack_description);
    size_t n = strlen(target);
    static int space_typed; /* the Space bar's key, whose text " " may follow */
    if ((!pack_form && pack_focus != 5) || !pack_focus) return 0;
    if (pack_focus == 4) return description_key(e);
    if (pack_focus == 5 && (e->type == MENU_EVENT_KEY_DOWN || e->type == MENU_EVENT_TEXT)) {
        pack_scroll = export_scroll = 0;
        if (e->type == MENU_EVENT_KEY_DOWN && (e->key == MENU_KEY_TAB || e->key == MENU_KEY_ESCAPE ||
            (e->key == MENU_KEY_ENTER && e->button != ' '))) {
            pack_focus = 0;
            return 1;
        }
    }
    if (e->type == MENU_EVENT_TEXT && space_typed && !strcmp(e->text, " ")) {
        space_typed = 0; /* typed already, at its key */
        return 1;
    }
    if (e->type == MENU_EVENT_KEY_DOWN || e->type == MENU_EVENT_TEXT)
        space_typed = 0;
    if (e->type == MENU_EVENT_KEY_DOWN && e->key == MENU_KEY_ENTER && e->button != ' ') {
        if (pack_focus == 4) {
            if (n + 1 < cap) strcat(target, "\n");
        } else pack_focus = pack_focus % 4 + 1;
        return 1;
    }
    if (e->type == MENU_EVENT_KEY_DOWN && e->key == MENU_KEY_ENTER) {
        /* The Space bar, which the platforms give as Enter (menu.h): a space,
         * at once, whether or not its text comes after. */
        if (n + 1 < cap) strcat(target, " ");
        space_typed = 1;
        return 1;
    }
    if (e->type == MENU_EVENT_KEY_DOWN) {
        if (e->key == MENU_KEY_BACKSPACE && n) target[Menu_TextBack(target, n)] = 0;
        else if (e->key == MENU_KEY_TAB) pack_focus = pack_focus % 4 + 1;
        else if (e->key == MENU_KEY_ESCAPE) pack_focus = 0;
        else if (e->text[0] && n + strlen(e->text) < cap) strcat(target, e->text);
        return 1;
    }
    if (e->type == MENU_EVENT_TEXT) {
        if (n + strlen(e->text) < cap) strcat(target, e->text);
        return 1;
    }
    return 0;
}
/* A press on the Modpack tab's widgets: 1 when it was one of them. */
static int pack_press(const Layout *l, int x, int y)
{
    PackLayout k;
    pack_layout(l, &k);
    if (inside(k.tab, x, y)) {
        packs = !packs;
        pack_ask = pack_form = pack_focus = 0;
        pack_query[0] = 0;
        pack_filter = 0;
        status[0] = 0;
        if (packs) packs_scan();
        return 1;
    }
    if (!packs) return 0;
    if (!pack_form && pack_detail_max && inside(k.detail, x, y) &&
        x >= k.detail.x + k.detail.w - BAR - 4 * unit) {
        int h = k.detail.h - 40 * unit;
        pack_detail_scroll = min(pack_detail_max, max(0, (y - k.detail.y) * pack_detail_max / max(1, h)));
        return 1;
    }
    /* a press on a scroll bar jumps there, as the detail's does */
    if (pack_form && pack_form_h > k.detail.h && inside(k.detail, x, y) && x >= k.detail.x + k.detail.w - BAR - 4 * unit) {
        pack_form_scroll = (y - k.detail.y) * (pack_form_h - k.detail.h) / max(1, k.detail.h);
        return 1;
    }
    if (pack_form && inside(k.list, x, y) && x >= k.list.x + k.list.w - BAR - 4 * unit &&
        pack_visible_count() > pack_rows(&k)) {
        export_scroll = (y - k.list.y) * (pack_visible_count() - pack_rows(&k)) / max(1, k.list.h);
        return 1;
    }
    if (inside(l->search, x, y)) { pack_focus = 5; return 1; }
    if (inside(l->filter, x, y)) {
        pack_filter = (pack_filter + 1) % 3;
        pack_scroll = export_scroll = pack_focus = 0;
        return 1;
    }
    if (inside(k.list, x, y)) {
        pack_focus = 0;
        if (pack_form) {
            int i = pack_shown(export_scroll + (y - k.list.y) / (44 * unit));
            if (i >= 0) {
                export_sel = i;
                export_mods[i].selected = !export_mods[i].selected;
                pack_focus = 0;
                status[0] = 0;
            }
            return 1;
        }
        int i = pack_shown(pack_scroll + (y - k.list.y) / (44 * unit));
        if (i >= 0) {
            const char *name = i ? pack_names[i - 1] : "";
            pack_sel = i;
            if (x >= k.list.x + 40 * unit || prep.running || !strcmp(name, pack_chosen))
                pack_confirm = 0;
            else if (pack_confirm == i + 1) {
                pack_confirm = 0;
                prep_start();
            } else {
                /* ( ) is small and near the names: a stray click asks first */
                pack_confirm = i + 1;
                if (i)
                    put(status, sizeof(status), "Activate the modpack \"%s\"? Click its ( ) again, or Activate modpack, to confirm.",
                        name);
                else
                    put(status, sizeof(status), "Use the normal mods (no modpack)? Click its ( ) again, or Use normal mods, to confirm.");
            }
        }
        pack_ask = 0;
        return 1;
    }
    if (pack_form && export_job.running && inside(k.cancel, x, y)) {
        __atomic_store_n(&export_job.cancel, 1, __ATOMIC_RELAXED);
        put(status, sizeof(status), "Stopping the export...");
        return 1;
    }
    if (pack_form && export_job.running && inside(k.save, x, y)) return 1; /* one export at a time */
    if (pack_form && inside(k.cancel, x, y)) { pack_form = 0; status[0] = 0; return 1; }
    if (pack_form && inside(k.save, x, y)) {
        pack_focus = 0;
        if (!export_selected()) {
            put(status, sizeof(status), "Select at least one mod in the list before exporting.");
        } else if (!pack_name[0] || !pack_version[0] || !pack_author[0]) {
            put(status, sizeof(status), "Name, Version and Author are required.");
            pack_focus = !pack_name[0] ? 1 : !pack_version[0] ? 2 : 3;
        } else
            pack_ask_file(2);
        return 1;
    }
    if (pack_form) {
        if (!inside(k.detail, x, y)) return 0;
        pack_focus = 0;
        for (int i = 0; i < 4; i++)
            if (inside(k.fields[i], x, y)) pack_focus = i + 1;
        if (inside(k.poster, x, y)) pack_ask_file(1);
        else if (inside(k.lock, x, y)) pack_lock = !pack_lock;
        return inside(k.detail, x, y);
    }
    if (inside(k.import, x, y)) {
        pack_ask = pack_confirm = 0;
        if (import_job.running) {
            __atomic_store_n(&import_job.cancel, 1, __ATOMIC_RELAXED);
            put(status, sizeof(status), "Stopping the import...");
        } else
            pack_ask_file(0);
        return 1;
    }
    if (inside(k.export_, x, y)) {
        pack_ask = pack_confirm = 0;
        pack_form = 1;
        pack_form_scroll = description_scroll = 0;
        pack_focus = 0;
        pack_query[0] = 0;
        pack_filter = 0;
        status[0] = 0;
        export_scan();
        return 1;
    }
    if (inside(k.folder, x, y)) {
        char dir[1024];
        if (Mods_ModpackDirectory(dir, sizeof(dir)) || Platform_OpenFolder(dir))
            put(status, sizeof(status), "Could not open the modpacks folder.");
        return 1;
    }
    if (inside(k.activate, x, y) && activate_kind()) {
        int kind = activate_kind();
        pack_confirm = 0;
        if (kind == 3) {
            __atomic_store_n(&prep.cancel, 1, __ATOMIC_RELAXED);
            put(status, sizeof(status), "Stopping...");
        } else if (kind == 1)
            prep_start();
        else
            pack_restart_press();
        return 1;
    }

    return inside(k.list, x, y) || inside(k.detail, x, y);
}
/* With a modpack whose author locked its settings: the presses and keys that
 * would change a mod. 1 when the event is one (it is then refused). */
static int pack_locked_out(const Layout *l, const MenuEvent *e)
{
    if (!Mods_ModpackLocked() || packs) return 0;
    if (e->type == MENU_EVENT_KEY_DOWN && !focus &&
        (e->key == MENU_KEY_LEFT || e->key == MENU_KEY_RIGHT || e->key == MENU_KEY_ENTER))
        return 1;
    if (e->type != MENU_EVENT_BUTTON_DOWN || e->button != 1) return 0;
    return inside(l->toggle, e->x, e->y) || inside(l->order[0], e->x, e->y) || inside(l->order[1], e->x, e->y) ||
           inside(l->defaults, e->x, e->y) || inside(l->load, e->x, e->y) ||
           (tab == 1 && inside(l->view, e->x, e->y)) ||
           (inside(l->list, e->x, e->y) && e->x < l->list.x + 40 * unit);
}
void ModsWindow_Draw(MenuCanvas *c)
{
    Layout l;
    char line[512];
    int extra;
    layout(&l);
    fill(c, rect(0, 0, c->width, c->height), BG);
    if (touch) {
        draw_touch(c, &l);
        return;
    }
    text(c, 20 * unit, 29 * unit, width - 40 * unit, packs ? "Modpacks" : "Mod library", TEXT);
    {
        PackLayout k;
        pack_layout(&l, &k);
        button(c, k.tab, packs ? "Mods" : "Modpack", packs);
        if (packs) {
            draw_packs(c, &l);
        } else {
            counts_line(line, sizeof(line), width / 2 - 20 * unit);
            text(c, width / 2, 29 * unit, width / 2 - 20 * unit, line, DIM);
            draw_fields(c, &l);
            text(c, l.profile.x, 118 * unit, width - l.profile.x - 20 * unit,
                 "Named profile  /  Save uses applied settings", DIM);
            draw_list(c, &l);
            draw_details(c, &l);
        }
    }
    extra = status_extra() * LINE;
    fill(c, rect(0, height - 72 * unit - extra, width, 1), EDGE);
    if (extra) /* the last line where a one-line status goes */
        wrap(c, 20 * unit, height - 59 * unit - extra - LINE / 2, width - 40 * unit, status, pending ? WARN : DIM);
    else
        text(c, 20 * unit, height - 59 * unit, width - 40 * unit,
             *status ? status : packs ? (pack_form ? "Select the mods to include, then fill in the export details."
                                                  : "Select a modpack, then Activate modpack to use it.")
                                     : "Changes are staged. Apply once when you are ready.", pending ? WARN : DIM);
    {   /* the keys, as fully as the room beside the buttons lets */
        int room = (l.modsdir.w ? l.modsdir.x : l.folder.x) - 28 * unit;
        const char *hint = "Arrow keys: select / toggle   Tab: search   Mouse wheel: scroll";
        if (packs) {
            PackLayout k;
            pack_layout(&l, &k);
            room = max(0, (pack_form ? k.save.x : k.import.x) - 28 * unit);
        }
        if (packs) hint = pack_form ? "Arrows: select   Space: tick   Wheel: scroll" : "Wheel: scroll";
        if (width_text(hint) > room)
            hint = "Arrows: select   Tab: search   Wheel: scroll";
        if (width_text(hint) > room)
            hint = "Tab: search   Wheel: scroll";
        if (packs && pack_form && export_job.running)
            export_bar(c, rect(20 * unit, l.close.y, max(0, room + 16 * unit), l.close.h));
        else if (packs && !pack_form && import_job.running)
            import_bar(c, rect(20 * unit, l.close.y, max(0, room + 16 * unit), l.close.h));
        else
            text(c, 20 * unit, height - 32 * unit, room, hint, DIM);
    }
    if (!packs) {
        button(c, l.folder, "Open mods folder", 0);
        draw_modsdir(c, &l);
    }
    button(c, l.close, pending ? "Cancel" : "Close", 0);
    if (packs) {
        /* Restart is the detail's own (Restart to activate), not the footer's */
    }
    else
        button(c, l.apply, apply_label(), changed() || pending || folder_restart());
}
static int needs_restart(void)
{
    if (folder_restart())
        return 1;
    for (int i = 0; i < Mods_Count(); i++) {
        char key[160];
        snprintf(key, sizeof(key), "mod.%s.order", Mods_Id(i));
        if (ranks[i] != Settings_GetNamed(key, num(Mods_Manifest(i), "priority", 0)))
            return 1;
        /* A live mod whose requirement is applied at the next launch waits
         * for that launch too (Mods_WaitsForRestart). */
        if (wanted[i] != Mods_Enabled(i) &&
            (Mods_RequiresRestart(i) || (wanted[i] && Mods_WaitsForRestart(i, wanted) >= 0)))
            return 1;
        for (int j = 0; j < counts[i]; j++)
            if (values[i][j] != Mods_OptionValue(i, j) &&
                (Mods_RequiresRestart(i) || Json_Bool(Json_Member(Mods_Option(i, j), "restart"), 0)))
                return 1;
    }
    return 0;
}
static void apply(void)
{
    int restart = needs_restart(), old_ranks[MODS_MAX], *old_values[MODS_MAX] = {0};
    if (!changed() && !folder_restart()) {
        snprintf(status, sizeof(status), "No pending changes");
        return;
    }
    if (restart && pending != 1) {
        pending = 1;
        snprintf(status, sizeof(status),
                 "Applying these changes restarts the game. Unsaved game progress "
                 "will be lost.");
        return;
    }
    pending = 0;
    if (!Mods_Validate(wanted, status, sizeof(status)))
        return;
    for (int i = 0; i < Mods_Count(); i++)
        for (int j = 0; j < counts[i]; j++) {
            if (!Mods_Failed(i) && !Mods_OptionValid(i, j, values[i][j])) {
                snprintf(status, sizeof(status), "Invalid setting: %s / %s", Mods_Name(i),
                         str(Mods_Option(i, j), "key", ""));
                return;
            }
        }
    for (int i = 0; i < Mods_Count(); i++) {
        char key[160];
        snprintf(key, sizeof(key), "mod.%s.order", Mods_Id(i));
        old_ranks[i] = Settings_GetNamed(key, num(Mods_Manifest(i), "priority", 0));
        old_values[i] = calloc((size_t)max(1, counts[i]), sizeof(int));
        if (!old_values[i]) {
            for (int j = 0; j < i; j++)
                free(old_values[j]);
            snprintf(status, sizeof(status), "Could not prepare settings");
            return;
        }
        for (int j = 0; j < counts[i]; j++)
            old_values[i][j] = Mods_OptionValue(i, j);
    }
    for (int i = 0; i < Mods_Count(); i++) {
        char key[160];
        snprintf(key, sizeof(key), "mod.%s.order", Mods_Id(i));
        if (ranks[i] != old_ranks[i]) /* untouched mods keep following their manifest */
            Settings_SetNamed(key, ranks[i]);
        for (int j = 0; j < counts[i]; j++)
            Mods_OptionSet(i, j, values[i][j]);
    }
    if (!Mods_Apply(wanted, status, sizeof(status))) {
        for (int i = 0; i < Mods_Count(); i++) {
            char key[160];
            snprintf(key, sizeof(key), "mod.%s.order", Mods_Id(i));
            if (ranks[i] != old_ranks[i])
                Settings_SetNamed(key, old_ranks[i]);
            for (int j = 0; j < counts[i]; j++)
                Mods_OptionSet(i, j, old_values[i][j]);
        }
        Settings_Save(); /* Also restore options if activation failed after persistence. */
    } else {
        for (int i = 0; i < Mods_Count(); i++)
            for (int j = 0; j < counts[i]; j++)
                if (values[i][j] != old_values[i][j]) {
                    MemoriesModEvent event = {MEMORIES_EVENT_SETTINGS, MEMORIES_AFTER, i, j, values[i][j], 0, 0};
                    Mods_Dispatch(&event);
                }
        snprintf(status, sizeof(status), "Changes applied.");
        if (restart && Platform_RestartGame() < 0)
            snprintf(status, sizeof(status),
                     "Settings saved. Restart failed; relaunch the game to finish "
                     "applying them.");
    }
    for (int i = 0; i < Mods_Count(); i++)
        free(old_values[i]);
}
/* --- Import mod... (ModsWindow_SetImport) ----------------------------- */

/* snprintf, for lines that may be cut short at the buffer's end. */
static void put(char *out, size_t size, const char *format, ...)
{
    va_list list;
    va_start(list, format);
    vsnprintf(out, size, format, list);
    va_end(list);
}
void ModsWindow_SetImport(int (*pick)(char *, size_t), int (*picked)(char *, size_t),
                          int (*fetch)(char *, size_t, char *, size_t, unsigned *, unsigned *, const int *))
{
    char folder[1024];
    import_pick = pick;
    import_picked = picked;
    import_fetch = fetch;
    /* what an import cut short (the app ended in it) left in the mods
     * folder; never while the HD pack's job has files there */
    if (pick && !import_step && !import_waiting && !ImportJob_Busy() && HdPack_State() == HD_IDLE &&
        !Mods_InstallDirectory(folder, sizeof(folder))) {
        Mods_ImportCleanup(folder);
        if (HdPack_Available() && !Paths_User(folder, sizeof(folder), "downloads"))
            HdPack_Cleanup(folder);
    }
}
static void import_done(void)
{
    Mods_ImportClose(import_waiting);
    import_waiting = NULL;
    import_plain = 0;
    Mods_ImportRemoveTree(import_zip);
}
/* The names of the mods of `import` that `pick` picks, "A", "A and B",
 * "A, B and C", into `out`; how many. */
static int import_names(ModsImport *import, int (*pick)(const ModsImportMod *), char *out, size_t size)
{
    int n = 0, total = 0;
    for (int i = 0; i < Mods_ImportCount(import); i++)
        total += pick(Mods_ImportMod(import, i));
    out[0] = 0;
    for (int i = 0; i < Mods_ImportCount(import); i++) {
        const ModsImportMod *mod = Mods_ImportMod(import, i);
        size_t used = strlen(out);
        if (!pick(mod))
            continue;
        put(out + used, size - used, "%s%s", n == 0 ? "" : n == total - 1 ? " and " : ", ", mod->name);
        n++;
    }
    return n;
}
static int any_mod(const ModsImportMod *mod) { return mod != NULL; }
/* Code this game cannot run: every code mod in a game built without the
 * code mod loader (MEMORIES_NO_CODE_MODS, where mods.c leaves such a mod
 * off; no build target sets it since the arm64 game loads code mods). A
 * game with the loader finds out at Apply whether the mod has an object
 * for it, and says so beside the mod (mods.c). */
#ifdef __ANDROID__
#define HERE "on Android" /* where code_off's mods cannot run */
#else
#define HERE "in this build"
#endif
static int code_off(const ModsImportMod *mod)
{
#ifdef MEMORIES_NO_CODE_MODS
    return mod->code;
#else
    (void)mod;
    return 0;
#endif
}
static int replacing(const ModsImportMod *mod) { return mod->replace[0] != 0; }
/* Every mod of the waiting import into the mods folder, then into the
 * list (Mods_Discover), off: the first of them selected, and a word on
 * what came, what waits for a restart and what cannot run here. */
/* Every mod of `import`, in the mods folder `folder` now, into the list
 * (Mods_Discover), off: the first one's index (else -1), whether one waits
 * for a restart (*later) and how many could not be added (*lost). */
static int import_discover(ModsImport *import, const char *folder, int *later, int *lost)
{
    int first = -1, old_count = Mods_Count();
    *later = *lost = 0;
    for (int i = 0; i < Mods_ImportCount(import); i++) {
        const ModsImportMod *mod = Mods_ImportMod(import, i);
        char path[1200];
        int waits = 0, index;
        put(path, sizeof(path), "%s/%s", folder, mod->folder);
        index = Mods_Discover(path, &waits);
        if (index < 0) {
            (*lost)++;
            continue;
        }
        /* Code that cannot run here: it stays off. */
        if (code_off(mod) && Mods_Enabled(index))
            Mods_SetEnabled(index, 0);
        *later |= waits;
        if (index >= old_count || !waits)
            adopt(index);
        wanted[index] = Mods_Enabled(index); /* nothing staged for it stays */
        if (first < 0)
            first = index;
    }
    if (Settings_Save() < 0)
        fprintf(stderr, "memories-pc: the settings could not be saved after an import\n");
    return first;
}
/* Mod `first` shown, selected and in view (the filter and search cleared). */
static void import_show(int first)
{
    int at = 0;
    if (first < 0)
        return;
    filter = 0;
    query[0] = 0;
    selected = first;
    detail_scroll = 0;
    for (int i = 0; i < first; i++)
        at += visible(i);
    if (at < scroll || at >= scroll + rows())
        scroll = max(0, min(at, shown_count() - rows()));
}
/* A step of the import ended in IMPORT_JOB_FAILED: why, in the words the
 * player always had (what a Stop left out, and what the unpacking refused,
 * "Nothing was imported."), and the job over. Its files are gone: the copy
 * with the reading, the hidden folder with the unpacking. */
static void import_failed(void)
{
    const char *why = ImportJob_Why();
    if (ImportJob_Cancelled())
        put(status, sizeof(status), "The import was stopped. Nothing was imported.");
    else if (import_waiting) /* the unpacking */
        put(status, sizeof(status), "%s%s", strncmp(why, "Nothing was", 11) ? "Nothing was imported. " : "", why);
    else
        put(status, sizeof(status), "%s", why);
    ImportJob_Reset();
    import_step = 0;
    import_outside = 0;
    if (import_waiting)
        import_done();
}
/* The mods are in place: into the list, and the word on what came. */
static void import_finish(void)
{
    char folder[1024], names[300], line[512];
    int first, later, n, several, lost;
    ImportJob_Reset();
    import_step = 0;
    if (Mods_InstallDirectory(folder, sizeof(folder))) { /* it was made for the unpacking */
        put(status, sizeof(status), "The mods are in the mods folder, but could not be added to the list. Restart "
            "the game to see them.");
        import_done();
        return;
    }
    first = import_discover(import_waiting, folder, &later, &lost);
    n = import_names(import_waiting, any_mod, names, sizeof(names));
    if (n == 1 && replacing(Mods_ImportMod(import_waiting, 0)))
        put(line, sizeof(line), "Replaced %s.", names);
    else if (n == 1)
        put(line, sizeof(line), "Imported %s.", names);
    else {
        char replaced[300];
        int k = import_names(import_waiting, replacing, replaced, sizeof(replaced));
        put(line, sizeof(line), "Imported %d mods: %s.", n, names);
        if (k)
            put(line + strlen(line), sizeof(line) - strlen(line), " %d of them replaced the installed ones.", k);
    }
    if (later) /* a mod with that id in place this launch (the release's own) */
        put(line + strlen(line), sizeof(line) - strlen(line), " It is used after a restart.");
    if (lost)
        put(line + strlen(line), sizeof(line) - strlen(line), " %d could not be added to the list.", lost);
    several = import_names(import_waiting, code_off, names, sizeof(names)) > 1;
    if (*names && n == 1)
        put(status, sizeof(status), "%s This mod has code, which the game cannot run %s yet. It stays off and "
            "changes nothing in the game.", line, HERE);
    else if (*names)
        put(status, sizeof(status), "%s %s %s code, which the game cannot run %s yet. %s off and change%s nothing "
            "in the game.", line, names, several ? "have" : "has", HERE, several ? "They stay" : "It stays",
            several ? "" : "s");
    else
        put(status, sizeof(status), "%s", line);
    fprintf(stderr, "memories-pc: import: %s\n", status);
    import_done();
    import_show(first);
}
/* The mods go into the mods folder, on the job's thread: after the reading
 * when no mod is installed already, else after Replace. */
static void import_install(void)
{
    char folder[1024];
    if (Mods_InstallDirectory(folder, sizeof(folder))) {
        put(status, sizeof(status), "Could not make the mods folder.");
        import_done();
        return;
    }
    import_shown = -1;
    if (!ImportJob_Install(import_waiting, folder)) { /* a Stop that came after the reading, or no thread */
        import_failed();
        return;
    }
    import_step = 3;
    put(status, sizeof(status), "Unpacking the mod...");
}
/* Another mod of `import` than mod `index` has the folder `name` (its own,
 * or the free one it was given), letter case aside: one folder on a
 * case-blind file system. */
static int import_clash(ModsImport *import, const char *name, int index)
{
    for (int j = 0; j < Mods_ImportCount(import); j++) {
        const char *a = name, *b = Mods_ImportMod(import, j)->folder;
        while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b))
            a++, b++;
        if (j != index && !*a && !*b)
            return 1;
    }
    return 0;
}
/* The installed mod mod `index` of `import` would replace: its folder when
 * the folder is that mod's (the same id), else an installed mod with its
 * id. A folder that holds something else, or whose name another mod of the
 * .zip has (import_clash), gives the new mod a free name (<folder>-2, ...)
 * instead: Replace never removes another mod or a folder of the player's.
 * 0, or -1 with the reason in `status`. */
static int import_target(ModsImport *import, int index, const char *folder)
{
    ModsImportMod *mod = Mods_ImportMod(import, index);
    char path[1200], id[64];
    int taken, clash;
    mod->replace[0] = 0;
    taken = Mods_ImportTaken(folder, mod->folder, path, sizeof(path));
    clash = import_clash(import, mod->folder, index);
    if (taken || clash) {
        if (!clash && Mods_ImportFolderId(path, id, sizeof(id)) && !strcmp(id, mod->id))
            put(mod->replace, sizeof(mod->replace), "%s", path);
        else {
            char base[sizeof(mod->folder)], name[sizeof(mod->folder)];
            int k;
            put(base, sizeof(base), "%.*s", (int)sizeof(base) - 5, mod->folder);
            for (k = 2; k < 100; k++) {
                put(name, sizeof(name), "%s-%d", base, k);
                if (!Mods_ImportTaken(folder, name, path, sizeof(path)) && !import_clash(import, name, index))
                    break;
            }
            if (k == 100) {
                put(status, sizeof(status), "Nothing was imported: the mods folder has no free name for %s.", mod->name);
                return -1;
            }
            put(mod->folder, sizeof(mod->folder), "%s", name);
        }
    }
    for (int j = 0; j < Mods_Count(); j++) {
        if (strcmp(Mods_Id(j), mod->id))
            continue;
        if (!mod->replace[0] && !strcmp(Mods_Origin(j), "installed") &&
            Mods_ImportTaken(Mods_Directory(j), ".", path, sizeof(path)))
            put(mod->replace, sizeof(mod->replace), "%s", Mods_Directory(j));
        /* Its code, data or pictures are this launch's files: they must
         * not change under it. */
        if (mod->replace[0] && Mods_InUse(j)) {
            put(status, sizeof(status), "%s is in use, so its files cannot be replaced now. Turn it off, Apply "
                "& restart, then import it again.", Mods_Name(j));
            return -1;
        }
    }
    return 0;
}
/* A .zip came and was copied and read, on the job's thread: unpacked, or
 * held for Replace/Cancel when a mod in it is installed already. These
 * questions read the mod list, so they are the game thread's. */
static void import_opened(void)
{
    char folder[1024], names[300];
    int taken = 0, fresh = 0, outside = import_outside;
    import_outside = 0;
    import_step = 0;
    import_waiting = ImportJob_Take(); /* with at least one mod */
    if (ImportJob_StopAsked()) { /* a Stop that came as the reading ended */
        ImportJob_Reset();
        put(status, sizeof(status), "The import was stopped. Nothing was imported.");
        import_done();
        return;
    }
    if (Mods_InstallDirectory(folder, sizeof(folder))) {
        put(status, sizeof(status), "Could not make the mods folder.");
        import_done();
        return;
    }
    for (int i = 0; i < Mods_ImportCount(import_waiting); i++) {
        ModsImportMod *mod = Mods_ImportMod(import_waiting, i);
        int known = 0;
        if (import_target(import_waiting, i, folder)) {
            import_done();
            return;
        }
        taken += mod->replace[0] != 0;
        for (int j = 0; j < Mods_Count(); j++)
            known |= !strcmp(Mods_Id(j), mod->id);
        fresh += !known;
    }
    if (Mods_Count() + fresh > MODS_MAX) {
        put(status, sizeof(status), "Nothing was imported: the game takes at most %d mods.", MODS_MAX);
        import_done();
        return;
    }
    if (!taken && !outside) {
        import_install();
        return;
    }
    pending = 3;
    if (!taken) {
        import_plain = 1;
        import_names(import_waiting, any_mod, names, sizeof(names));
        put(status, sizeof(status), "Import %s from the .zip you opened with the game?", names);
        return;
    }
    import_names(import_waiting, replacing, names, sizeof(names));
    if (taken == 1)
        put(status, sizeof(status), "%s is already installed. Replace it with the one in this .zip?", names);
    else
        put(status, sizeof(status), "%s are already installed. Replace them with the ones in this .zip?", names);
}
static void import_start(void)
{
    char why[400];
    if (import_step)
        return;
    if (import_pick(why, sizeof(why))) {
        put(status, sizeof(status), "%s", why);
        return;
    }
    import_step = 1;
    put(status, sizeof(status), "Choose a mod's .zip.");
}
/* The picker answered: the copy and the reading, on the job's thread. */
static void import_begin(void)
{
    import_shown = -1;
    if (!ImportJob_Read(import_fetch, import_zip, sizeof(import_zip))) {
        put(status, sizeof(status), "%s", ImportJob_State() == IMPORT_JOB_FAILED ? ImportJob_Why()
                                                                                  : "Could not start importing.");
        ImportJob_Reset();
        import_outside = 0;
        return;
    }
    import_step = 2;
    put(status, sizeof(status), "Importing...");
}
int ModsWindow_ImportBusy(int shown)
{
    /* a question the panel left open (the HD pack's Download, a Replace) is
     * answered no when it shows again (Init): it holds nothing up */
    return (hd_step && (shown || hd_step != 2)) || import_step || (shown && (import_waiting || pending));
}
int ModsWindow_ImportOpened(const char *refusal)
{
    if (!import_pick)
        return 0;
    if (refusal) {
        put(status, sizeof(status), "%s", refusal);
        return 0;
    }
    if (ModsWindow_ImportBusy(1)) /* the caller waits for it (sdl.c); its question stays as it is */
        return 0;
    import_outside = 1;
    import_start();
    if (!import_step)
        import_outside = 0;
    return import_step != 0;
}
/* --- Remove ------------------------------------------------------------ */

/* The selected mod's Remove was pressed: the question (pending 5), or why
 * not. Only a mod of the player's own (an installed one) that nothing was
 * loaded from since the game started (Mods_InUse): its code, data and
 * pictures are files the running game holds, and they must not go away
 * under it. A mod that is on, or was, has to be turned off and applied,
 * and the game started again, first. Anything else can go, as it is. */
static void remove_start(void)
{
    const char *name;
    if (selected < 0 || Mods_Removed(selected))
        return;
    name = Mods_Name(selected);
    if (hd_step || import_step || import_waiting) {
        put(status, sizeof(status), "Wait for the import or the HD pack to finish first.");
        return;
    }
    if (!strcmp(Mods_Origin(selected), "folder")) {
        put(status, sizeof(status), "%s is in your own mods folder, which the game does not delete from. "
            "Delete it there.", name);
        return;
    }
    if (strcmp(Mods_Origin(selected), "installed")) {
        put(status, sizeof(status), "%s came with the game, so it cannot be removed.", name);
        return;
    }
    if (Mods_InUse(selected)) {
        put(status, sizeof(status), Mods_Active(selected)
            ? "%s is on, so the game is using its files. Turn it off, apply, restart the game, then remove it."
            : "%s was tried since the game started, so the game may still hold its files. Turn it off, apply, "
              "restart the game, then remove it.",
            name);
        return;
    }
    remove_target = selected;
    pending = 5;
    put(status, sizeof(status), "Remove %s? Its folder is deleted from the mods folder, with everything in it. "
        "This cannot be undone.", name);
}
/* Remove's answer was yes: the folder goes, the mod leaves the list now
 * (Mods_Forget keeps its slot, so no other mod's index moves), and the
 * next mod is shown. */
static void remove_confirm(void)
{
    char folder[1024], why[300], name[160];
    int mod = remove_target, next = -1;
    remove_target = -1;
    pending = 0;
    if (mod < 0 || mod >= Mods_Count() || Mods_Removed(mod) || Mods_InUse(mod)) {
        put(status, sizeof(status), "Nothing was removed.");
        return;
    }
    put(name, sizeof(name), "%s", Mods_Name(mod));
    if (Mods_InstallDirectory(folder, sizeof(folder))) {
        put(status, sizeof(status), "Could not open the mods folder. Nothing was removed.");
        return;
    }
    if (Mods_ImportRemoveInstalled(folder, Mods_Directory(mod), why, sizeof(why))) {
        put(status, sizeof(status), "%s It stays in the list.", why);
        fprintf(stderr, "memories-pc: remove: %s\n", why);
        return;
    }
    if (Mods_Forget(mod)) {
        put(status, sizeof(status), "%s was removed, but it leaves the list after a restart.", name);
        return;
    }
    adopt(mod); /* what was staged for it (a rank, a setting, On) goes with it: no "Unsaved changes" for a mod that is gone */
    wanted[mod] = 0;
    if (Settings_Save() < 0)
        fprintf(stderr, "memories-pc: the settings could not be saved after a remove\n");
    if (selected == mod) {
        for (int i = mod + 1; i < Mods_Count() && next < 0; i++)
            if (visible(i))
                next = i;
        for (int i = mod - 1; i >= 0 && next < 0; i--)
            if (visible(i))
                next = i;
        selected = next;
        detail_scroll = 0;
        tab = 0;
    }
    if (compact)
        page = 0;
    scroll = max(0, min(scroll, shown_count() - rows()));
    put(status, sizeof(status), "Removed %s.", name);
    fprintf(stderr, "memories-pc: remove: %s\n", status);
}
/* --- Mods folder... (ModsWindow_SetFolderPicker) ------------------------- */

void ModsWindow_SetFolderPicker(int (*pick)(char *why, size_t why_size),
                                int (*picked)(char *path, size_t size, char *why, size_t why_size))
{
    if (pick == folder_pick && picked == folder_picked)   /* the platform sets them whenever the window is asked for */
        return;
    folder_pick = pick;
    folder_picked = picked;
    folder_step = 0;
}
/* The folder is read at the next launch, not this one: a restart is due
 * when the choice is not the one the game started with. Not when
 * MEMORIES_MODS_DIR names the only folder read, which a restart does not
 * change. */
static int folder_restart(void)
{
    char now[1024];
    const char *only = getenv("MEMORIES_MODS_DIR");
    if (!launch_known || (only && *only))
        return 0;
    if (Mods_Folder(now, sizeof(now)))
        now[0] = 0;
    return strcmp(now, launch_folder) != 0 ||
           (launch_folder[0] && (launch_unread || launch_access == 0) && access_have && access_now);
}
/* Note which folder the game started with: at the first showing, after the
 * mods were scanned. */
static void folder_launch(void)
{
    if (launch_known)
        return;
    if (Mods_Folder(launch_folder, sizeof(launch_folder)))
        launch_folder[0] = 0;
    launch_unread = launch_folder[0] && *Mods_FolderProblem();
    launch_known = 1;
}
void ModsWindow_SetFolderAccess(int (*have)(void), int (*ask)(char *why, size_t why_size),
                                int (*asked)(char *why, size_t why_size), int (*can_ask)(void),
                                const char *(*right)(void))
{
    folder_launch(); /* the mods are scanned by now */
    access_have = have;
    access_ask = ask;
    access_asked = asked;
    access_can_ask = can_ask;
    access_right = right;
    access_now = have ? have() : 1;
    if (have && launch_access < 0) /* as the start scanned the folder, whatever the player gave since */
        launch_access = access_now;
}
void ModsWindow_RefreshAccess(void)
{
    access_recheck_now = 1;
}
/* The right's name, as Android calls it (All files access from Android 11). */
static const char *access_name(void)
{
    return access_right ? access_right() : "All files access";
}
/* Whether the device has a way to give the right at all. */
static int access_possible(void)
{
    return !access_can_ask || access_can_ask();
}
/* The folder is chosen, and the game cannot read it for want of the right:
 * what to do about it, for the message line. */
static const char *access_hint(void)
{
    static char hint[256];
    if (!access_possible())
        put(hint, sizeof(hint), "This device cannot give the game %s. Use Import mod... instead.", access_name());
    else
        put(hint, sizeof(hint), "Without %s the game cannot read it. Allow it in this app's settings.",
            access_name());
    return hint;
}
/* The right is held after all (given on the system's page, or in Settings while
 * the game was behind it): what the window says, and 1 to draw again. */
static int access_came(void)
{
    char path[1024];
    access_now = 1;
    if (pending == 7) /* the question is answered */
        pending = 0;
    /* A question or a step has the message line: the Apply button is lit already,
     * and the line says nothing of the right that it would have to push out. */
    if (Mods_Folder(path, sizeof(path)) || pending || hd_step || import_step || import_waiting)
        return 1;
    put(status, sizeof(status), "%s is on.%s", access_name(),
        folder_restart() ? " The game reads the folder after a restart: Apply changes." : "");
    return 1;
}
/* While a folder is chosen that the game cannot read: whether the right came
 * (asked of the system about twice a second, and at once when the app returns
 * to the front). 1 when the window must be drawn again. */
static int access_recheck(void)
{
    static int ticks;
    char path[1024];
    int forced = access_recheck_now;
    access_recheck_now = 0;
    if (!access_have || access_now)
        return 0;
    if (!forced && ++ticks < 30)
        return 0;
    ticks = 0;
    if (Mods_Folder(path, sizeof(path)))
        return 0;
    return access_have() ? access_came() : 0;
}
/* What the question says (pending 6): the folder, why it is not read if it
 * is not, and the choices. */
static void folder_ask(void)
{
    char path[1024];
    pending = 6;
    if (Mods_Folder(path, sizeof(path)))
        put(status, sizeof(status), "Mods folder: none. Choose a folder to read its mods as well as the game's own?");
    else
        /* in a few lines on a phone: the reason was said when the window opened */
        put(status, sizeof(status), "Mods folder: %.300s %s Choose another folder, or use none?", path,
            !access_now ? (touch ? "The game cannot read it yet." : access_hint())
            : *Mods_FolderProblem() ? (touch ? "It was not read this time." : Mods_FolderProblem())
                                    : "It is read when the game starts.");
}
/* The folder was chosen on a phone that has not given the right to read it:
 * the question (pending 7) says why it is asked, with one sentence. */
static void folder_offer_access(const char *path)
{
    if (!access_possible()) {
        pending = 0;
        put(status, sizeof(status), "%s", access_hint());
        return;
    }
    pending = 7;
    put(status, sizeof(status), "To read %.300s the game needs %s, which Android asks for on its next screen. "
        "Allow it?", path, access_name());
}
/* Allow was answered yes: Android's page, and the answer at the next ticks. */
static void folder_allow(void)
{
    char why[400];
    pending = 0;
    if (access_ask(why, sizeof(why))) {
        put(status, sizeof(status), "%s", access_possible() ? why : access_hint());
        return;
    }
    folder_step = 2;
    put(status, sizeof(status), "Allow %s for the game, then come back.", access_name());
}
/* The folder the player chose, or none (path NULL): saved now, read at the
 * next launch (Apply & restart). */
static void folder_set(const char *path)
{
    char why[400], now[1024];
    pending = 0;
    if (Mods_SetFolder(path, why, sizeof(why))) {
        put(status, sizeof(status), "%s", why);
        return;
    }
    if (Mods_Folder(now, sizeof(now)))
        put(status, sizeof(status), "Mods folder: none.");
    else
        put(status, sizeof(status), "Mods folder: %.300s", now);
    if (*why)
        put(status + strlen(status), sizeof(status) - strlen(status), " %s", why);
    else if (folder_restart())
        put(status + strlen(status), sizeof(status) - strlen(status),
            " The game reads it after a restart: Apply changes.");
    if (path && access_ask && !access_have()) {
        access_now = 0;
        folder_offer_access(now);
    }
    fprintf(stderr, "memories-pc: mods folder: %s\n", status);
}
static void folder_choose(void)
{
    char why[400];
    pending = 0;
    if (folder_step)
        return;
    if (folder_pick(why, sizeof(why))) {
        put(status, sizeof(status), "%s", why);
        return;
    }
    folder_step = 1;
    put(status, sizeof(status), "Choose the folder your mods are in.");
}
/* Mods folder... was pressed, or (pending 6) its second button, Use no
 * folder. */
static void folder_tap(void)
{
    char path[1024];
    if (pending == 6) {
        if (!Mods_Folder(path, sizeof(path)))
            folder_set(NULL);
        return;
    }
    if (hd_step || import_step || import_waiting) {
        put(status, sizeof(status), "Wait for the import or the HD pack to finish first.");
        return;
    }
    if (!folder_step)
        folder_ask();
}
/* The picker's answer, once per tick: the folder, or why not. 1 when the
 * window must be drawn again. */
static int folder_poll(void)
{
    char path[1024], why[400];
    int result;
    if (folder_step == 2) {
        result = access_asked(why, sizeof(why));
        if (!result)
            return 0;
        folder_step = 0;
        if (result > 0) {
            access_came();
        } else {
            access_now = 0;
            /* what the system said, or when it has no page to give (the same, in the panel's words) the hint */
            put(status, sizeof(status), "%s", result < -1 && access_possible() ? why : access_hint());
        }
        return 1;
    }
    if (folder_step != 1 || !folder_picked)
        return access_recheck();
    result = folder_picked(path, sizeof(path), why, sizeof(why));
    if (!result)
        return 0;
    folder_step = 0;
    if (result > 0)
        folder_set(path);
    else if (result == -1)
        status[0] = 0;
    else
        put(status, sizeof(status), "%s", why);
    return 1;
}
/* --- HD pack... (hd_pack.h) -------------------------------------------- */

/* The installed HD pack's index (an installed mod with its id), else -1. */
static int hd_installed(void)
{
    for (int j = 0; j < Mods_Count(); j++)
        if (!strcmp(Mods_Id(j), HD_PACK_ID) && !Mods_Removed(j))
            return j;
    return -1;
}
static void hd_end(void)
{
    HdPack_Reset();
    hd_step = 0;
    hd_shown = -1;
}
/* Decimal megabytes, as Android's storage settings count them. */
static unsigned long megabytes(unsigned long long bytes) { return (unsigned long)((bytes + 500000) / 1000000); }
/* GitHub named the latest release: already there, newer there, in use, no
 * room, or the question (pending 4) with its size and the room it needs. */
static void hd_found(void)
{
    const HdRelease *release = HdPack_Release();
    char tag[64];
    int j = hd_installed(), known = j >= 0 && HdPack_InstalledTag(Mods_Directory(j), tag, sizeof(tag));
    unsigned long long need = HdPack_SpaceNeeded(release);
    long long room = HdPack_FreeBytes(Paths_UserDir()); /* the downloads folder's and the mods folder's */
    UpdateVersion have, latest;
    if (known && !strcmp(tag, release->tag)) {
        put(status, sizeof(status), "The HD pack is already installed (%s).", tag);
        hd_end();
        return;
    }
    if (known && Update_ParseVersion(tag, &have) && Update_ParseVersion(release->tag, &latest) &&
        Update_CompareVersions(&have, &latest) > 0) {
        put(status, sizeof(status), "The installed HD pack (%s) is newer than the latest release (%s).", tag,
            release->tag);
        hd_end();
        return;
    }
    if (j >= 0 && Mods_InUse(j)) {
        put(status, sizeof(status), "The HD pack is in use, so it cannot be replaced now. Turn it off, Apply & "
            "restart, then tap HD pack... again.");
        hd_end();
        return;
    }
    if (j < 0 && Mods_Count() >= MODS_MAX) {
        put(status, sizeof(status), "The game takes at most %d mods: remove one to add the HD pack.", MODS_MAX);
        hd_end();
        return;
    }
    if (room >= 0 && (unsigned long long)room < need) {
        put(status, sizeof(status), "Not enough free space for the HD pack: it needs about %lu MB, and %lu MB are "
            "free.", megabytes(need), megabytes((unsigned long long)room));
        hd_end();
        return;
    }
    if (j < 0)
        put(status, sizeof(status), "Download the HD pack (%s)? %lu MB; Wi-Fi recommended. Needs ~%lu MB free.",
            release->tag, megabytes(release->size), megabytes(need));
    else if (known)
        put(status, sizeof(status), "Update the HD pack from %s to %s? %lu MB; Wi-Fi recommended. Needs ~%lu MB "
            "free.", tag, release->tag, megabytes(release->size), megabytes(need));
    else
        put(status, sizeof(status), "Download the HD pack (%s) over the installed one? %lu MB; Wi-Fi recommended. "
            "Needs ~%lu MB free.", release->tag, megabytes(release->size), megabytes(need));
    pending = 4;
    hd_step = 2;
}
/* Download, to the question. */
static void hd_confirm(void)
{
    char folder[1024];
    pending = 0;
    if (Paths_User(folder, sizeof(folder), "downloads") || Paths_MakeDirs(folder)) {
        put(status, sizeof(status), "Could not make the app's downloads folder.");
        hd_end();
        return;
    }
    if (!HdPack_Download(folder)) {
        put(status, sizeof(status), "%s", *HdPack_Why() ? HdPack_Why() : "Could not start the download.");
        hd_end();
        return;
    }
    hd_step = 3;
    hd_shown = -1;
    put(status, sizeof(status), "Downloading the HD pack (%s)...", HdPack_Release()->tag);
}
/* The .zip is checked: where it goes (a copy there is replaced, as Import
 * replaces the same mod), then its unpacking on the job's thread. */
static void hd_downloaded(void)
{
    char folder[1024];
    ModsImport *import = HdPack_Import();
    if (Mods_InstallDirectory(folder, sizeof(folder))) {
        put(status, sizeof(status), "Could not make the mods folder.");
        hd_end();
        return;
    }
    if (import_target(import, 0, folder)) { /* the reason is in `status` */
        hd_end();
        return;
    }
    if (!HdPack_Install(folder)) {
        if (HdPack_State() == HD_FAILED)
            return; /* a Stop that came after the download, or no thread: hd_tick says so next */
        put(status, sizeof(status), "Could not start unpacking the HD pack.");
        hd_end();
        return;
    }
    hd_step = 4;
    hd_shown = -1;
}
/* In the mods folder: into the list (off, as an import), selected. */
static void hd_done(void)
{
    char folder[1024];
    const ModsImportMod *mod = Mods_ImportMod(HdPack_Import(), 0);
    int replaced = mod->replace[0] != 0, first = -1, later = 0, lost = 0;
    if (!Mods_InstallDirectory(folder, sizeof(folder)))
        first = import_discover(HdPack_Import(), folder, &later, &lost);
    if (first < 0)
        put(status, sizeof(status), "The HD pack (%s) is in the mods folder, but could not be added to the list. "
            "Restart the game to see it.", HdPack_Release()->tag);
    else
        put(status, sizeof(status), "HD pack %s (%s). %s", replaced ? "updated" : "installed", HdPack_Release()->tag,
            later ? "It is used after a restart." : Mods_Enabled(first) ? "Apply to use it."
                                                                         : "Tick it and apply to use it.");
    fprintf(stderr, "memories-pc: HD pack: %s\n", status);
    hd_end();
    import_show(first);
}
/* The step's share in the message; 1 when it changed (draw again). */
static int hd_progress(const char *what)
{
    unsigned long done = HdPack_Done(), total = HdPack_Total();
    int shown = total ? (int)((unsigned long long)done * 100 / total) : 0, waiting = HdPack_Waiting();
    if (shown * 3 + waiting == hd_shown)
        return 0;
    hd_shown = shown * 3 + waiting;
    if (waiting) /* the download manager waits for a connection (or retries), or for Wi-Fi */
        put(status, sizeof(status), "Waiting for %s to go on with the HD pack (%s): %d%% of %lu MB.",
            waiting == 2 ? "Wi-Fi" : "the network", HdPack_Release()->tag, shown, megabytes(total));
    else
        put(status, sizeof(status), "%s the HD pack (%s): %d%% of %lu MB.", what, HdPack_Release()->tag, shown,
            megabytes(total));
    return 1;
}
/* Once per tick while the window has a job: its next step. */
static int hd_tick(void)
{
    int state = HdPack_State();
    /* The window's own question (Discard your changes?) keeps the message
     * line and the buttons until it is answered; the job waits. */
    if (pending && pending != 4)
        return 0;
    if (state == HD_FAILED) {
        if (HdPack_Cancelled())
            put(status, sizeof(status), "The HD pack's download was stopped. Nothing was installed.");
        else
            put(status, sizeof(status), "%s", HdPack_Why());
        fprintf(stderr, "memories-pc: HD pack: %s\n", status);
        hd_end();
        return 1;
    }
    if (hd_step == 1 && state == HD_FOUND) {
        hd_found();
        return 1;
    }
    if (hd_step == 3 && state == HD_DOWNLOADING)
        return hd_progress("Downloading");
    if (hd_step == 3 && state == HD_DOWNLOADED) {
        hd_downloaded();
        return 1;
    }
    if (hd_step == 4 && state == HD_INSTALLING)
        return hd_progress("Unpacking");
    if (hd_step == 4 && state == HD_INSTALLED) {
        hd_done();
        return 1;
    }
    return 0;
}
/* HD pack...: ask GitHub (the network's first use), or Stop. */
static void hd_tap(void)
{
    if (!hd_step && HdPack_State() == HD_CLEANING) { /* what a cut-short job left, being removed */
        put(status, sizeof(status), "Clearing an earlier download; tap HD pack... again in a moment.");
        hd_clearing = 1;
        return;
    }
    /* Stop while a step runs, or in the frames between the download's
     * end and the unpacking's start (hd_step 3): HdPack_Install then
     * refuses. Once the unpacking ends the pack is in. */
    if (HdPack_Busy() || hd_step == 3) {
        HdPack_Cancel();
        put(status, sizeof(status), "Stopping...");
        return;
    }
    if (hd_step)
        return;
    if (import_step || import_waiting) {
        put(status, sizeof(status), "Wait for the import to finish first.");
        return;
    }
    if (!HdPack_Lookup()) {
        put(status, sizeof(status), "%s", *HdPack_Why() ? HdPack_Why() : "Could not start the download.");
        hd_end();
        return;
    }
    hd_step = 1;
    put(status, sizeof(status), "Asking GitHub for the latest HD pack...");
}
/* The import's step share in the message, as hd_progress says the HD pack's;
 * 1 when it changed (draw again). A copy the provider gave no size of says
 * the megabytes so far. */
static int import_progress(const char *what)
{
    unsigned long done = ImportJob_Done(), total = ImportJob_Total();
    int shown = total ? (int)((unsigned long long)done * 100 / total) : (int)(done >> 20);
    if (ImportJob_StopAsked() || shown == import_shown) /* "Stopping..." stays */
        return 0;
    import_shown = shown;
    if (total)
        put(status, sizeof(status), "%s: %d%% of %lu MB.", what, shown, megabytes(total));
    else
        put(status, sizeof(status), "%s: %lu MB.", what, megabytes(done));
    return 1;
}
/* Once per tick while the import's thread has a step (import_step 2, 3):
 * its next step. The window's own question (Discard your changes?) keeps the
 * message line and the buttons until it is answered; the job waits. */
static int import_tick(void)
{
    int state = ImportJob_State();
    if (pending)
        return 0;
    if (state == IMPORT_JOB_FAILED) {
        import_failed();
        return 1;
    }
    if (import_step == 2 && state == IMPORT_JOB_READ) {
        import_opened();
        return 1;
    }
    if (import_step == 2 && state == IMPORT_JOB_READING)
        return import_progress("Copying the .zip");
    if (import_step == 3 && state == IMPORT_JOB_UNPACKED) {
        import_finish();
        return 1;
    }
    if (import_step == 3 && state == IMPORT_JOB_UNPACKING)
        return import_progress("Unpacking the mod");
    return 0;
}
/* Import mod... while the import runs is Stop: the thread ends its step at
 * the next chunk and removes what it wrote. A Stop that comes after the
 * unpacking ended changes nothing: the mods are in. */
static void import_tap(void)
{
    if (import_step >= 2) {
        ImportJob_Cancel();
        put(status, sizeof(status), "Stopping...");
        return;
    }
    import_start();
}
int ModsWindow_Tick(void)
{
    char why[400];
    int result;
    if (pack_poll())
        return 1;
    if (prep_poll())
        return 1;
    if (export_poll())
        return 1;
    if (import_poll())
        return 1;
    if (folder_poll())
        return 1;
    if (hd_step)
        return hd_tick();
    if (hd_clearing && HdPack_State() != HD_CLEANING) { /* that message's moment is over */
        hd_clearing = 0;
        if (!pending)
            put(status, sizeof(status), "Ready: tap HD pack... again.");
        return 1;
    }
    if (import_step >= 2)
        return import_tick();
    if (import_step != 1 || !import_picked)
        return 0;
    result = import_picked(why, sizeof(why));
    if (!result)
        return 0;
    import_step = 0;
    if (result <= 0)
        import_outside = 0;
    if (result > 0)
        import_begin();
    else if (result == -1)
        status[0] = 0;
    else
        put(status, sizeof(status), "%s", why);
    return 1;
}
/* Escape, Cancel or closing the window answer a question with no: an
 * import's Replace goes with it. */
static void cancel_pending(void)
{
    if (pending == 7) { /* the folder stays chosen, unreadable until it is allowed */
        pending = 0;
        put(status, sizeof(status), "%s", access_hint());
        return;
    }
    if (pending == 3)
        import_done();
    remove_target = -1;
    if (pending == 4) {
        HdPack_Reset();
        hd_step = 0;
    }
    pending = 0;
    hd_shown = import_shown = -1; /* a job's share is said again */
    status[0] = 0;
}

static void select_step(int step)
{
    int at = -1, n = shown_count();
    if (!n)
        return;
    for (int i = 0; i < n; i++)
        if (shown(i) == selected)
            at = i;
    at = (at + step + n) % n;
    selected = shown(at);
    detail_scroll = 0;
    slider_drag = -1; /* a held slider belonged to the previous mod */
    if (at < scroll)
        scroll = at;
    if (at >= scroll + rows())
        scroll = at - rows() + 1;
}
static void adjust(int option, int direction)
{
    const JsonValue *spec = Mods_Option(selected, option);
    const char *type = str(spec, "type", "int");
    int low = num(spec, "min", 0), high = num(spec, "max", 100), step = max(1, num(spec, "step", 1)),
        value = values[selected][option];
    if (!strcmp(type, "bool"))
        value = !value;
    else if (!strcmp(type, "key")) {
        int bit = 0;
        while (bit < 16 && value != (1 << bit))
            bit++;
        bit = (bit + direction + 17) % 17;
        value = bit == 16 ? 0 : 1 << bit;
    } else if (!strcmp(type, "choice")) {
        /* A closed, named set of options, the same as "key"'s pad buttons
         * are: wraps around rather than clamping, so either arrow always
         * does something, never leaving a press at either end with nothing
         * to do but press the other way. Unlike a plain int (below), there
         * is no meaningful "one past the end" to stop at. */
        int count = Json_Count(Json_Member(spec, "choices"));
        if (count > 0)
            value = ((value + (direction > 0 ? 1 : -1)) % count + count) % count;
    } else {
        int64_t next = (int64_t)value + (direction > 0 ? step : -step);
        value = next > high ? high : next < low ? low : (int)next;
    }
    values[selected][option] = value;
}
static void slider_value(int option, int x)
{
    Layout l;
    layout(&l);
    const JsonValue *spec = Mods_Option(selected, option);
    int low = num(spec, "min", 0), high = num(spec, "max", 100), step = max(1, num(spec, "step", 1));
    int span = body_width(&l) - 10 * unit, at = x - l.view.x - 5 * unit;
    if (span <= 0 || high <= low)
        return;
    at = min(max(at, 0), span);
    int64_t value = low + ((int64_t)high - low) * at / span;
    value = low + ((value - low + step / 2) / step) * step;
    if (value > high)
        value = high;
    values[selected][option] = (int)value;
}
/* Pointer at `y` on a held scrollbar: the thumb follows it. */
static void bar_move(const Layout *l, int y)
{
    if (bar_drag == 1)
        scroll = thumb_to(l->list_bar, shown_count(), rows(), y - bar_grab);
    else if (bar_drag == 2)
        detail_scroll = thumb_to(l->bar, body_height(l), l->view.h, y - bar_grab);
}
/* Whether a press at x, y lands on scrollbar `which` (1 the list's, 2 the
 * details'), with a margin beside it: wider for a finger. One test for
 * bar_press and ModsWindow_Grabs, so a press the panel hands over as held
 * is one the bar takes. */
static int bar_hit(const Layout *l, int which, int x, int y)
{
    Rect track = which == 1 ? l->list_bar : l->bar;
    int total = which == 1 ? shown_count() : body_height(l), shown = which == 1 ? rows() : l->view.h,
        margin = (touch ? 8 : 3) * unit;
    return total > shown && inside(rect(track.x - margin, track.y, track.w + 2 * margin, track.h), x, y);
}
/* Pressing a scrollbar grabs its thumb, or centres the thumb on the pointer. */
static int bar_press(const Layout *l, int which, int x, int y)
{
    Rect track = which == 1 ? l->list_bar : l->bar, t;
    int total = which == 1 ? shown_count() : body_height(l), shown = which == 1 ? rows() : l->view.h,
        at = which == 1 ? scroll : detail_scroll, margin = (touch ? 8 : 3) * unit;
    if (!bar_hit(l, which, x, y))
        return 0;
    t = thumb(track, total, shown, at);
    bar_drag = which;
    bar_grab = inside(rect(track.x - margin, t.y, track.w + 2 * margin, t.h), x, y) ? y - t.y : t.h / 2;
    bar_move(l, y);
    return 1;
}
/* A press on the settings list: the - value + buttons or an int's slider.
 * With `act` 0 it only says whether the press would hold a slider. */
static int option_press(const Layout *l, int x, int y, int act)
{
    int w = body_width(l), top = options_top();
    x -= l->view.x;
    y -= l->view.y;
    for (int j = 0; j < counts[selected]; j++) {
        OptionBox o;
        option(NULL, j, w, top + 10 * unit, &o);
        top += 10 * unit + o.height;
        if (y >= top)
            continue;
        if (inside(o.minus, x, y)) {
            if (act)
                adjust(j, -1);
        } else if (inside(o.plus, x, y) || inside(o.value, x, y)) {
            if (act)
                adjust(j, 1);
        } else if (o.slider.h &&
                   inside(rect(o.slider.x, o.slider.y - 4 * unit, o.slider.w, o.slider.h + 8 * unit), x, y)) {
            if (act) {
                slider_drag = j;
                slider_value(j, x + l->view.x);
            }
            return 1;
        }
        return 0;
    }
    return 0;
}
int ModsWindow_TextFocus(void) { return packs ? pack_focus : focus; }
int ModsWindow_Locate(int id, int *x, int *y)
{
    Layout l;
    Rect r = none();
    layout(&l);
    switch (id) {
    case MODS_UI_SEARCH: r = l.search; break;
    case MODS_UI_FILTER: r = l.filter; break;
    case MODS_UI_PROFILE: r = l.profile; break;
    case MODS_UI_SAVE: r = l.save; break;
    case MODS_UI_LOAD: r = l.load; break;
    case MODS_UI_TOGGLE: r = selected >= 0 ? l.toggle : none(); break;
    case MODS_UI_BACK: r = l.back; break;
    case MODS_UI_DEFAULTS: r = tab == 1 && selected >= 0 && counts[selected] ? l.defaults : none(); break;
    case MODS_UI_CLOSE: r = l.close; break;
    case MODS_UI_APPLY: r = l.apply; break;
    case MODS_UI_FOLDER: r = l.folder; break;
    case MODS_UI_HD: r = l.hd; break;
    case MODS_UI_REMOVE: r = l.remove; break;
    case MODS_UI_MODSDIR: {
        char path[1024];
        r = pending == 6 && Mods_Folder(path, sizeof(path)) ? none() : l.modsdir;
        break;
    }
    case MODS_UI_TAB: case MODS_UI_TAB + 1: case MODS_UI_TAB + 2: r = selected >= 0 ? l.tabs[id - MODS_UI_TAB] : none(); break;
    case MODS_UI_ORDER: case MODS_UI_ORDER + 1: r = selected >= 0 ? l.order[id - MODS_UI_ORDER] : none(); break;
    case MODS_UI_ROW_SELECTED: case MODS_UI_ROW_FIRST: case MODS_UI_CHECK_SELECTED: case MODS_UI_CHECK_FIRST:
        for (int row = 0; row < rows() && l.list.w; row++) {
            int mod = shown(scroll + row), check = id == MODS_UI_CHECK_SELECTED || id == MODS_UI_CHECK_FIRST;
            if (mod < 0 || ((id == MODS_UI_ROW_SELECTED || id == MODS_UI_CHECK_SELECTED) && mod != selected))
                continue;
            r = check ? rect(l.list.x, l.list.y + row * PITCH, touch ? max(40 * unit, touch) : 40 * unit, PITCH - 2 * unit)
                      : rect(l.list.x + l.list.w / 2, l.list.y + row * PITCH, 1, PITCH - 2 * unit);
            break;
        }
        break;
    default:
        if (id >= MODS_UI_OPTION && selected >= 0 && tab == 1 && (id - MODS_UI_OPTION) / 4 < counts[selected]) {
            int top = options_top(), option_id = (id - MODS_UI_OPTION) / 4, part = (id - MODS_UI_OPTION) % 4;
            OptionBox o = {0};
            for (int j = 0; j <= option_id; j++) {
                option(NULL, j, body_width(&l), top + 10 * unit, &o);
                if (j < option_id)
                    top += 10 * unit + o.height;
            }
            r = part == 0 ? o.minus : part == 1 ? o.value : part == 2 ? o.plus : o.slider;
            r.x += l.view.x;
            r.y += l.view.y;
            if (r.y < l.view.y || r.y + r.h > l.view.y + l.view.h)
                r = none(); /* scrolled out of the view */
        }
    }
    if (!r.w || !r.h)
        return 0;
    *x = r.x + r.w / 2;
    *y = r.y + r.h / 2;
    return 1;
}
/* A press here holds something that follows the finger (a slider, a
 * scrollbar's thumb) rather than starting a drag that scrolls. */
int ModsWindow_Grabs(int x, int y)
{
    Layout l;
    layout(&l);
    if (pending)
        return 0;
    if (bar_hit(&l, 1, x, y))
        return 1;
    if (selected < 0)
        return 0;
    if (bar_hit(&l, 2, x, y))
        return 1;
    return tab == 1 && counts[selected] && inside(l.view, x, y) && option_press(&l, x, y, 0);
}
/* A finger that went down at x, y moved `dy` pixels since the last call:
 * the list moves a row per row's height, the details by the pixel. */
void ModsWindow_Drag(int x, int y, int dy)
{
    Layout l;
    layout(&l);
    if (pending)
        return;
    if (inside(l.list, x, y)) {
        int most = max(0, shown_count() - rows()), step;
        drag_rest += dy;
        step = drag_rest / PITCH;
        drag_rest -= step * PITCH;
        scroll = min(max(scroll - step, 0), most);
        if ((scroll == 0 && drag_rest > 0) || (scroll == most && drag_rest < 0))
            drag_rest = 0; /* at an end: no debt to pay back first */
    } else if (selected >= 0 && inside(l.detail, x, y))
        detail_scroll = min(max(0, detail_scroll - dy), detail_limit(&l));
}
int ModsWindow_Redraws(const MenuEvent *e)
{
    return e->type != MENU_EVENT_MOTION || slider_drag >= 0 || bar_drag || description_drag;
}
int ModsWindow_RequestClose(void)
{
    if (pending == 3 || pending == 4 || pending == 5 || pending == 6 || pending == 7)
        cancel_pending();
    focus = 0;
    slider_drag = -1;
    bar_drag = 0;
    if (pending == 2 || !changed())
        return 1;
    pending = 2;
    snprintf(status, sizeof(status), "Discard your unsaved mod changes?");
    return 0;
}
int ModsWindow_Event(const MenuEvent *e)
{
    Layout l;
    layout(&l);
    /* Keep the pending save dialog's export selection and metadata stable. */
    if (packs && pack_picking) return 0;
    if (e->type == MENU_EVENT_BUTTON_UP || e->type == MENU_EVENT_LEAVE) description_drag = 0;
    if (packs && pack_form && !pending) {
        PackLayout k;
        pack_layout(&l, &k);
        int click = e->type == MENU_EVENT_BUTTON_DOWN && e->button == 1 &&
                    inside(k.detail, e->x, e->y) && inside(k.fields[3], e->x, e->y);
        if (click || (description_drag && e->type == MENU_EVENT_MOTION)) {
            Rect f = k.fields[3];
            int top = max(f.y + 6 * unit, k.detail.y), bottom = min(f.y + f.h - 6 * unit, k.detail.y + k.detail.h);
            if (!click) {
                if (e->y < top) description_scroll = max(0, description_scroll - LINE);
                if (e->y > bottom) description_scroll = min(description_max, description_scroll + LINE);
            }
            size_t at = TextEdit_Hit(pack_description, f.w - 20 * unit, LINE, width_text,
                                    e->x - f.x - 10 * unit, e->y - f.y - 6 * unit + description_scroll);
            TextEdit_Move(&description_edit, at, !click || (e->modifiers & MENU_MOD_SHIFT));
            pack_focus = 4;
            description_drag = 1;
            return 0;
        }
    }
    if (e->type == MENU_EVENT_BUTTON_UP || e->type == MENU_EVENT_LEAVE)
        slider_drag = -1, bar_drag = 0, drag_rest = 0;
    if (e->type == MENU_EVENT_MOTION && bar_drag) {
        bar_move(&l, e->y);
        return 0;
    }
    if (e->type == MENU_EVENT_MOTION && slider_drag >= 0 && selected >= 0 && !pending && tab == 1 &&
        slider_drag < counts[selected]) {
        slider_value(slider_drag, e->x);
        return 0;
    }
    if (packs && !touch && !pending && pack_type(e))
        return 0;
    if (pack_locked_out(&l, e)) {
        put(status, sizeof(status), "This modpack's author locked its settings.");
        return 0;
    }
    if (packs && !touch && !pending) { /* the Modpack tab: its own keys, wheel and presses */
        if (e->type == MENU_EVENT_KEY_DOWN && e->key == MENU_KEY_ESCAPE && pack_form) {
            pack_form = pack_focus = 0;
            status[0] = 0;
            return 0;
        }
        if (e->type == MENU_EVENT_KEY_DOWN && e->key == MENU_KEY_ESCAPE) {
            packs = pack_ask = pack_confirm = 0;
            status[0] = 0;
            return 0;
        }
        if (e->type == MENU_EVENT_KEY_DOWN && (e->key == MENU_KEY_UP || e->key == MENU_KEY_DOWN)) {
            PackLayout k;
            int delta = e->key == MENU_KEY_DOWN ? 1 : -1;
            int n = pack_visible_count(), row = 0;
            int *sel = pack_form ? &export_sel : &pack_sel;
            int *offset = pack_form ? &export_scroll : &pack_scroll;
            pack_layout(&l, &k);
            while (row < n && pack_shown(row) != *sel) row++;
            if (n) {
                row = row == n ? 0 : min(n - 1, max(0, row + delta));
                *sel = pack_shown(row);
                *offset = max(min(*offset, row), row - pack_rows(&k) + 1);
            }
            pack_ask = 0;
            return 0;
        }
        if (e->type == MENU_EVENT_KEY_DOWN && e->key == MENU_KEY_ENTER) {
            int sel = pack_form ? export_sel : pack_sel;
            for (int row = 0, i; (i = pack_shown(row)) >= 0; row++)
                if (i == sel) {
                    if (pack_form) export_mods[i].selected = !export_mods[i].selected;
                    else pack_choose();
                    break;
                }
            return 0;
        }
        if (e->type == MENU_EVENT_KEY_DOWN && e->key == MENU_KEY_TAB) {
            pack_focus = 5;
            return 0;
        }
        if (e->type == MENU_EVENT_KEY_DOWN || e->type == MENU_EVENT_TEXT)
            return 0;
        if (e->type == MENU_EVENT_WHEEL) {
            PackLayout k;
            pack_layout(&l, &k);
            if (!pack_form && inside(k.detail, e->x, e->y)) {
                pack_detail_scroll = min(pack_detail_max, max(0, pack_detail_scroll - e->wheel * 3 * LINE));
                return 0;
            }
            if (pack_form && inside(k.fields[3], e->x, e->y)) {
                int next = min(description_max, max(0, description_scroll - e->wheel * 3 * LINE));
                if (next != description_scroll) { description_scroll = next; return 0; }
            }
            if (pack_form && inside(k.detail, e->x, e->y)) {
                pack_form_scroll = min(max(0, pack_form_h - k.detail.h), max(0, pack_form_scroll - e->wheel * 3 * LINE));
                return 0;
            }
            if (pack_form)
                export_scroll = min(max(0, export_scroll - e->wheel), max(0, pack_visible_count() - pack_rows(&k)));
            else
                pack_scroll = min(max(0, pack_scroll - e->wheel), max(0, pack_visible_count() - pack_rows(&k)));
            return 0;
        }
        if (e->type == MENU_EVENT_BUTTON_DOWN && e->button == 1 && pack_press(&l, e->x, e->y))
            return 0;
        if (e->type == MENU_EVENT_BUTTON_DOWN && !inside(l.close, e->x, e->y))
            return 0; /* the mods' list and details are not shown */
    } else if (!touch && !pending && e->type == MENU_EVENT_BUTTON_DOWN && e->button == 1 && pack_press(&l, e->x, e->y))
        return 0;
    if (e->type == MENU_EVENT_KEY_DOWN) {
        if (e->key == MENU_KEY_ESCAPE) {
            if (pending) {
                cancel_pending();
                return 0;
            }
            if (focus) {
                focus = 0;
                return 0;
            }
            if (compact && page) { /* the details page goes back to the list */
                page = 0;
                return 0;
            }
            if (changed()) {
                pending = 2;
                snprintf(status, sizeof(status), "Discard your unsaved mod changes?");
                return 0;
            }
            return 1;
        }
        if (e->key == MENU_KEY_TAB) {
            focus = (focus + 1) % 3;
            return 0;
        }
        if (focus && touch && e->key == MENU_KEY_ENTER) {
            focus = 0; /* the on-screen keyboard's Enter: done typing */
            return 0;
        }
        if (focus) {
            char *target = focus == 1 ? query : profile;
            size_t cap = focus == 1 ? sizeof(query) : sizeof(profile), n = strlen(target);
            if (e->key == MENU_KEY_BACKSPACE && n)
                target[Menu_TextBack(target, n)] = 0;
            if (e->text[0] && n + strlen(e->text) < cap)
                strcat(target, e->text);
            scroll = 0;
            return 0;
        }
        if (pending) {
            if (e->key == MENU_KEY_ENTER) {
                if (pending == 2)
                    return 1;
                if (pending == 3) {
                    pending = 0;
                    import_install();
                } else if (pending == 4)
                    hd_confirm();
                else if (pending == 5)
                    remove_confirm();
                else if (pending == 6)
                    folder_choose();
                else if (pending == 7)
                    folder_allow();
                else
                    apply();
            }
            return 0;
        }
        if (e->key == MENU_KEY_UP || e->key == MENU_KEY_DOWN)
            select_step(e->key == MENU_KEY_DOWN ? 1 : -1);
        if (selected >= 0 && (e->key == MENU_KEY_LEFT || e->key == MENU_KEY_RIGHT || e->key == MENU_KEY_ENTER))
            wanted[selected] = e->key == MENU_KEY_LEFT ? 0 : e->key == MENU_KEY_RIGHT ? 1 : !wanted[selected];
    } else if (e->type == MENU_EVENT_TEXT && focus) {
        char *target = focus == 1 ? query : profile;
        size_t cap = focus == 1 ? sizeof(query) : sizeof(profile);
        if (strlen(target) + strlen(e->text) < cap)
            strcat(target, e->text);
        scroll = 0;
    } else if (e->type == MENU_EVENT_WHEEL && !pending) {
        if (inside(l.list, e->x, e->y)) {
            scroll -= e->wheel * 3;
            scroll = max(0, scroll);
            if (scroll > max(0, shown_count() - rows()))
                scroll = max(0, shown_count() - rows());
        } else if (inside(l.detail, e->x, e->y) && selected >= 0)
            detail_scroll = min(max(0, detail_scroll - e->wheel * 3 * LINE), detail_limit(&l));
    } else if (e->type == MENU_EVENT_BUTTON_DOWN && e->button == 1) {
        if (inside(l.close, e->x, e->y)) {
            if (pending)
                cancel_pending();
            else if (changed()) {
                pending = 2;
                snprintf(status, sizeof(status), "Discard your unsaved mod changes?");
            } else
                return 1;
        } else if (inside(l.apply, e->x, e->y)) {
            if (pending == 2)
                return 1;
            if (pending == 3) {
                pending = 0;
                import_install();
            } else if (pending == 4)
                hd_confirm();
            else if (pending == 5)
                remove_confirm();
            else if (pending == 6)
                folder_choose();
            else if (pending == 7)
                folder_allow();
            else if (hd_step)
                put(status, sizeof(status), "Wait for the HD pack to finish, or stop it, before applying changes.");
            else if (import_step >= 2)
                put(status, sizeof(status), "Wait for the import to finish, or stop it, before applying changes.");
            else
                apply();
        } else if ((!pending || pending == 6) && l.modsdir.w && inside(l.modsdir, e->x, e->y)) {
            folder_tap();
        } else if (!pending && l.hd.w && inside(l.hd, e->x, e->y)) {
            hd_tap();
        } else if (!pending && touch && l.remove.w && inside(l.remove, e->x, e->y)) {
            remove_start();
        } else if (!pending && touch && import_pick && inside(l.folder, e->x, e->y)) {
            if (hd_step)
                put(status, sizeof(status), "Wait for the HD pack to finish, or stop it, before importing.");
            else
                import_tap();
        } else if (!pending && inside(l.folder, e->x, e->y)) {
            char path[1024];
            if (Mods_InstallDirectory(path, sizeof(path)))
                snprintf(status, sizeof(status), "Could not create the mods folder.");
            else if (Platform_OpenFolder(path))
                snprintf(status, sizeof(status), "Could not open %.400s", path);
            else
                snprintf(status, sizeof(status), "Opened %.400s. New mods appear after a restart.", path);
        } else if (!pending) {
            focus = inside(l.search, e->x, e->y) ? 1 : inside(l.profile, e->x, e->y) ? 2 : 0;
            if (inside(l.filter, e->x, e->y)) {
                filter = (filter + 1) % 4;
                scroll = 0;
            }
            if (bar_press(&l, 1, e->x, e->y) || (selected >= 0 && bar_press(&l, 2, e->x, e->y)))
                return 0;
            if (inside(l.back, e->x, e->y)) {
                page = 0;
                return 0;
            }
            if (inside(l.list, e->x, e->y) && (e->y - l.list.y) / PITCH < rows()) {
                int mod = shown(scroll + (e->y - l.list.y) / PITCH);
                if (mod >= 0) {
                    selected = mod;
                    detail_scroll = 0;
                    if (e->x < l.list.x + (touch ? max(40 * unit, touch) : 40 * unit))
                        wanted[mod] = !wanted[mod];
                    else if (compact)
                        page = 1; /* a phone: the row opens the mod's page */
                }
                if (compact)
                    return 0;
            }
            if (inside(l.save, e->x, e->y)) {
                if (changed())
                    snprintf(status, sizeof(status), "Apply your changes before saving a profile.");
                else if (Mods_ProfileSave(profile))
                    snprintf(status, sizeof(status), "Profile saved.");
                else if (*Mods_ProfileSaveError())
                    snprintf(status, sizeof(status), "Could not save the profile to %s", Mods_ProfileSaveError());
                else
                    snprintf(status, sizeof(status), "Could not save profile; use a simple name.");
            }
            if (inside(l.load, e->x, e->y)) {
                if (Mods_ProfileRead(profile, wanted)) {
                    for (int i = 0; i < Mods_Count(); i++) {
                        char key[256];
                        if (Mods_Removed(i)) { /* a profile cannot bring a removed mod back, or stage its settings */
                            wanted[i] = 0;
                            continue;
                        }
                        snprintf(key, sizeof(key), "mod.%s.order", Mods_Id(i));
                        ranks[i] = Mods_ProfileValue(profile, key, num(Mods_Manifest(i), "priority", 0));
                        for (int j = 0; j < counts[i]; j++) {
                            snprintf(key, sizeof(key), "mod.%s.%s", Mods_Id(i), str(Mods_Option(i, j), "key", ""));
                            values[i][j] = Mods_ProfileValue(profile, key, num(Mods_Option(i, j), "default", 0));
                        }
                    }
                    snprintf(status, sizeof(status), "Profile loaded for review. Apply changes when ready.");
                } else
                    snprintf(status, sizeof(status), "Profile missing or references unavailable mods.");
            }
            if (selected >= 0) {
                if (inside(l.toggle, e->x, e->y))
                    wanted[selected] = !wanted[selected];
                for (int i = 0; i < 3; i++)
                    if (inside(l.tabs[i], e->x, e->y)) {
                        tab = i;
                        detail_scroll = 0;
                    }
                if (inside(l.order[0], e->x, e->y) && ranks[selected] > -100000)
                    ranks[selected]--;
                if (inside(l.order[1], e->x, e->y) && ranks[selected] < 100000)
                    ranks[selected]++;
                if (tab == 2 && inside(l.view, e->x, e->y)) {
                    /* Where the "and N more" lines are now (overlaps()). */
                    int at = e->y - l.view.y;
                    body(NULL, body_width(&l), -detail_scroll);
                    for (int k = 0; k < MODS_OVERLAP_KINDS; k++)
                        if (more_bottom[k] > more_top[k] && at >= more_top[k] && at < more_bottom[k])
                            opened[k] = !opened[k];
                }
                if (tab == 1 && counts[selected]) {
                    if (inside(l.defaults, e->x, e->y))
                        for (int j = 0; j < counts[selected]; j++)
                            values[selected][j] = num(Mods_Option(selected, j), "default", 0);
                    else if (inside(l.view, e->x, e->y))
                        option_press(&l, e->x, e->y, 1);
                }
            }
        }
    }
    return 0;
}
