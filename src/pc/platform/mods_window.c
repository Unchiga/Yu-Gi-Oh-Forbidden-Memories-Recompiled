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
#include "../../types.h"
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
        order[2], defaults, folder, remove, back, hd;
    int name_y, meta_y; /* the middles of the details' name and metadata lines */
    int lift;           /* with a finger: how far a long message raises the footer's top */
} Layout;
static int width, height, unit, selected, scroll, detail_scroll, tab, filter, focus, pending;
/* touch: the finger's target in pixels (0 with a mouse). compact: touch, and
 * too small for the list beside the details, so `page` shows one of them
 * (0 the list, 1 the details). drag_rest: a drag's pixels short of a row. */
static int touch, compact, page, drag_rest;
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
/* Remove (touch panels with Import mod...): the mod its question (pending
 * 5) is about. */
static int remove_target = -1;
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
    if (import_pick && (!compact || page == 0)) {
        int w = max(120 * unit, width_text("Import mod...") + 24 * unit);
        l->folder = rect(l->close.x - g - w, footer, w, t);
    }
    /* Remove, where Import is (an app cannot open its mods folder to delete
     * one by hand): beside the selected mod's details, so on a phone it is
     * on their page, where Import is not; else left of Import. While its
     * question is up (pending 5) the answer is the Apply button, which reads
     * Remove, so this one steps aside rather than showing a second, dead one. */
    l->remove = none();
    if (import_pick && selected >= 0 && pending != 5 && (!compact || page == 1)) {
        int w = max(90 * unit, width_text("Remove") + 24 * unit), edge = l->folder.w ? l->folder.x : l->close.x;
        l->remove = rect(edge - g - w, footer, w, t);
    }
    {   /* A message of two to four lines beside the buttons (a refusal, a
         * mod that cannot run here) raises the footer's top instead of
         * being cut: the list and the details end above it. */
        int room = (l->remove.w ? l->remove.x : l->folder.w ? l->folder.x : l->close.x) - 20 * unit;
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
    l->filter = rect(p, 104 * unit, left - p, 28 * unit);
    l->list = rect(p, top, left - p, footer - top - 12 * unit);
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
    l->folder = rect(width - p - 418 * unit, height - 48 * unit, 158 * unit, 30 * unit);
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
    remove_target = -1;
    query[0] = status[0] = 0;
    for (int i = 0; i < MODS_MAX; i++) {
        free(values[i]);
        values[i] = NULL;
        counts[i] = 0;
    }
    for (int i = 0; i < Mods_Count(); i++)
        adopt(i);
    if (import_waiting && import_step != 3) { /* a Replace question the last showing left open */
        Mods_ImportClose(import_waiting);
        import_waiting = NULL;
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
                 Mods_Metadata(selected, "author"), Mods_Origin(selected));
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
static const char *apply_label(void)
{
    return pending == 5 ? "Remove" : pending == 4 ? "Download" : pending == 3 ? "Replace" : pending == 2 ? "Discard changes" : pending == 1 ? "Apply & restart" : "Apply changes";
}
/* With a finger: the fields and the list, the details (side by side or the
 * page shown), and the footer: the message (or the counts) beside Close and
 * Apply, up to four lines in the footer's band (layout_touch raises it). */
/* Where the footer's buttons start: Remove when it is shown, else the
 * files slot when it has a button, else Close. */
static int footer_left(const Layout *l) { return l->remove.w ? l->remove.x : l->folder.w ? l->folder.x : l->close.x; }
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
    button(c, l->close, pending ? "Cancel" : "Close", 0);
    button(c, l->apply, apply_label(), changed() || pending);
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
    text(c, 20 * unit, 29 * unit, width - 40 * unit, "Mod library", TEXT);
    counts_line(line, sizeof(line), width / 2 - 20 * unit);
    text(c, width / 2, 29 * unit, width / 2 - 20 * unit, line, DIM);
    draw_fields(c, &l);
    text(c, l.profile.x, 118 * unit, width - l.profile.x - 20 * unit, "Named profile  /  Save uses applied settings",
         DIM);
    draw_list(c, &l);
    draw_details(c, &l);
    extra = status_extra() * LINE;
    fill(c, rect(0, height - 72 * unit - extra, width, 1), EDGE);
    if (extra) /* the last line where a one-line status goes */
        wrap(c, 20 * unit, height - 59 * unit - extra - LINE / 2, width - 40 * unit, status, pending ? WARN : DIM);
    else
        text(c, 20 * unit, height - 59 * unit, width - 40 * unit,
             *status ? status : "Changes are staged. Apply once when you are ready.", pending ? WARN : DIM);
    text(c, 20 * unit, height - 32 * unit, l.folder.x - 28 * unit,
         "Arrow keys: select / toggle   Tab: search   Mouse wheel: scroll", DIM);
    button(c, l.folder, "Open mods folder", 0);
    button(c, l.close, pending ? "Cancel" : "Close", 0);
    button(c, l.apply, apply_label(), changed() || pending);
}
static int needs_restart(void)
{
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
    if (!changed()) {
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
    int taken = 0, fresh = 0;
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
    if (!taken) {
        import_install();
        return;
    }
    pending = 3;
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
        return;
    }
    import_step = 2;
    put(status, sizeof(status), "Importing...");
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
int ModsWindow_TextFocus(void) { return focus; }
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
    return e->type != MENU_EVENT_MOTION || slider_drag >= 0 || bar_drag;
}
int ModsWindow_RequestClose(void)
{
    if (pending == 3 || pending == 4 || pending == 5)
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
            else if (hd_step)
                put(status, sizeof(status), "Wait for the HD pack to finish, or stop it, before applying changes.");
            else if (import_step >= 2)
                put(status, sizeof(status), "Wait for the import to finish, or stop it, before applying changes.");
            else
                apply();
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
