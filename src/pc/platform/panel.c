/* The Mods and Controls panels inside the game's window: see panel.h. */
#include "panel.h"
#include "controls_runtime.h"
#include "controls_window.h"
#include "mods_window.h"
#include "pc/debug/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What fills the window around the safe area: each module's background. */
#define MODS_BACKGROUND 0xff181b20u
#define CONTROLS_BACKGROUND 0xff1e1f22u

static int shown;
static int window_w, window_h, area_x, area_y, area_w, area_h;
/* A finger's press: down at x0, y0; moved (a drag, dragged to `last_y`),
 * or held by the module (grabbed: a slider or a scrollbar). */
static struct {
    int down, moved, grabbed, x0, y0, last_y;
} press;
static uint64_t last_tick;
/* The last release of the pointer was a tap (or a mouse's click), not the
 * end of a drag: only a tap on a field shows the on-screen keyboard again. */
static int tapped;

int Panel_Shown(void) { return shown; }
int Panel_Left(void) { return area_x; }
int Panel_Top(void) { return area_y; }

static void resize_module(void)
{
    if (shown == PANEL_MODS && area_w > 0 && area_h > 0)
        ModsWindow_Resize(area_w, area_h);
}

void Panel_Layout(int ww, int wh, int x, int y, int w, int h)
{
    window_w = ww;
    window_h = wh;
    /* A safe area that is not inside the window is no help: the whole of it. */
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > ww || y + h > wh) {
        x = y = 0;
        w = ww;
        h = wh;
    }
    area_x = x;
    area_y = y;
    area_w = w;
    area_h = h;
    resize_module();
}

void Panel_Close(void)
{
    if (shown != PANEL_NONE)
        ControlsRuntime_Block(0); /* the game's bindings and hotkeys again */
    shown = PANEL_NONE;
    memset(&press, 0, sizeof(press));
}

void Panel_RequestClose(void)
{
    if (shown == PANEL_MODS) {
        if (ModsWindow_RequestClose())
            Panel_Close();
    } else if (shown == PANEL_CONTROLS) {
        ControlsWindow_RequestClose();
        ControlsWindow_Tick();
        if (ControlsWindow_ShouldClose())
            Panel_Close();
    }
}

int Panel_Open(int kind)
{
    if (kind != PANEL_MODS && kind != PANEL_CONTROLS)
        return 0;
    if (shown == kind)
        return 1;
    Panel_Close();
    shown = kind;
    /* Neither panel lets a key or a controller reach the hidden game: no
     * hotkey (Turbo, Pause, Exit, a state) acts behind it, and what is held
     * now does nothing until it is let go after the panel closes. */
    ControlsRuntime_Block(1);
    if (kind == PANEL_MODS) {
        ModsWindow_Init();
        resize_module();
    } else
        ControlsWindow_Init();
    last_tick = 0;
    return 1;
}

/* MEMORIES_TRACE=window: where the panel's widgets are now, in window
 * pixels, each time that changes, for scripted taps on a phone (adb shell
 * input tap) without guessing a layout from a picture. */
static void log_widgets(void)
{
    static const struct {
        int id;
        const char *name;
    } mods[] = {{MODS_UI_SEARCH, "search"},   {MODS_UI_FILTER, "filter"},   {MODS_UI_PROFILE, "profile"},
                {MODS_UI_SAVE, "save"},       {MODS_UI_LOAD, "load"},       {MODS_UI_TOGGLE, "toggle"},
                {MODS_UI_BACK, "back"},       {MODS_UI_DEFAULTS, "defaults"}, {MODS_UI_CLOSE, "close"},
                {MODS_UI_APPLY, "apply"},     {MODS_UI_FOLDER, "import"},   {MODS_UI_HD, "hd-pack"}, {MODS_UI_REMOVE, "remove"},   {MODS_UI_MODSDIR, "mods-folder"}, {MODS_UI_ROW_SELECTED, "selected-row"},
                {MODS_UI_ROW_FIRST, "first-row"}, {MODS_UI_CHECK_SELECTED, "selected-check"},
                {MODS_UI_CHECK_FIRST, "first-check"},
                {MODS_UI_TAB, "tab-about"},   {MODS_UI_TAB + 1, "tab-settings"}, {MODS_UI_TAB + 2, "tab-compat"},
                {MODS_UI_ORDER, "order-down"}, {MODS_UI_ORDER + 1, "order-up"},
                {MODS_UI_OPTION + 0, "option0-minus"}, {MODS_UI_OPTION + 2, "option0-plus"},
                {MODS_UI_OPTION + 3, "option0-slider"}},
      controls[] = {{CONTROLS_UI_PLAYER_1, "player1"}, {CONTROLS_UI_PLAYER_2, "player2"},
                    {CONTROLS_UI_DEVICE, "device"},     {CONTROLS_UI_KEYBOARD, "keyboard"},
                    {CONTROLS_UI_CONTROLLER, "controller"}, {CONTROLS_UI_REBIND, "rebind"},
                    {CONTROLS_UI_CLEAR, "clear"},       {CONTROLS_UI_DEFAULTS, "defaults"},
                    {CONTROLS_UI_CANCEL, "cancel"},     {CONTROLS_UI_APPLY, "apply"},
                    {CONTROLS_UI_OK, "ok"},             {CONTROLS_UI_YES, "yes"},
                    {CONTROLS_UI_NO, "no"},             {CONTROLS_UI_KEEP, "keep"}};
    static char last[2048];
    char line[2048];
    size_t used = 0;
    int count = shown == PANEL_MODS ? (int)(sizeof(mods) / sizeof(mods[0])) : (int)(sizeof(controls) / sizeof(controls[0]));
    if (!Log_Wanted(LOG_WINDOW))
        return;
    line[0] = 0;
    for (int i = 0; i < count && used + 64 < sizeof(line); i++) {
        int x, y, id = shown == PANEL_MODS ? mods[i].id : controls[i].id;
        if (shown == PANEL_MODS ? ModsWindow_Locate(id, &x, &y) : ControlsWindow_Locate(id, &x, &y))
            used += (size_t)snprintf(line + used, sizeof(line) - used, " %s=%d,%d",
                                     shown == PANEL_MODS ? mods[i].name : controls[i].name, x + area_x, y + area_y);
    }
    /* The bindings the page shows, by their row (controls.h). */
    for (int row = 0; shown == PANEL_CONTROLS && row < CTRL_ROW_COUNT && used + 64 < sizeof(line); row++) {
        int x, y;
        if (ControlsWindow_Locate(CONTROLS_UI_BINDING + row * 2, &x, &y))
            used += (size_t)snprintf(line + used, sizeof(line) - used, " row%d=%d,%d", row, x + area_x, y + area_y);
    }
    if (strcmp(line, last)) {
        snprintf(last, sizeof(last), "%s", line);
        LOG(LOG_WINDOW, "panel widgets:%s", line);
    }
}

void Panel_Draw(MenuCanvas *window)
{
    uint32_t background = shown == PANEL_MODS ? MODS_BACKGROUND : CONTROLS_BACKGROUND;
    MenuCanvas area;
    int w = window->width, h = window->height;
    if (!shown)
        return;
    for (int y = 0; y < h; y++) {
        uint32_t *row = window->pixels + (size_t)y * (size_t)window->stride;
        if (y < area_y || y >= area_y + area_h)
            for (int x = 0; x < w; x++)
                row[x] = background;
        else {
            for (int x = 0; x < area_x && x < w; x++)
                row[x] = background;
            for (int x = area_x + area_w; x < w; x++)
                row[x] = background;
        }
    }
    if (area_x + area_w > w || area_y + area_h > h)
        return; /* laid out for another size: the next layout fixes it */
    area = *window;
    area.pixels = window->pixels + (size_t)area_y * (size_t)window->stride + (size_t)area_x;
    area.width = area_w;
    area.height = area_h;
    if (shown == PANEL_MODS)
        ModsWindow_Draw(&area);
    else
        ControlsWindow_Draw(&area);
    log_widgets();
}

/* An event for the module, in panel pixels. */
static void deliver(const MenuEvent *event)
{
    MenuEvent local = *event;
    local.x -= area_x;
    local.y -= area_y;
    if (shown == PANEL_MODS) {
        if (ModsWindow_Event(&local))
            Panel_Close();
    } else if (shown == PANEL_CONTROLS) {
        ControlsWindow_Event(&local);
        ControlsWindow_Tick();
        if (ControlsWindow_ShouldClose())
            Panel_Close();
    }
}

static void deliver_at(MenuEventType type, int x, int y)
{
    MenuEvent event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.button = 1;
    event.x = x;
    event.y = y;
    deliver(&event);
}

static int grabs(int x, int y)
{
    return shown == PANEL_MODS && ModsWindow_Grabs(x - area_x, y - area_y);
}

static void drag(int dy)
{
    if (shown == PANEL_MODS)
        ModsWindow_Drag(press.x0 - area_x, press.y0 - area_y, dy);
    else if (shown == PANEL_CONTROLS)
        ControlsWindow_Drag(press.x0 - area_x, press.y0 - area_y, dy);
}

int Panel_Pointer(const MenuEvent *event, int finger)
{
    int slop = Menu_TouchTarget() / 6 > 8 ? Menu_TouchTarget() / 6 : 8; /* 8 dp */
    if (!shown)
        return 0;
    if (event->type == MENU_EVENT_BUTTON_UP)
        tapped = !finger || (press.down && !press.moved && !press.grabbed);
    if (!finger) { /* a mouse: as in the window */
        deliver(event);
        return shown != PANEL_MODS || event->type != MENU_EVENT_MOTION || ModsWindow_Redraws(event);
    }
    switch (event->type) {
    case MENU_EVENT_BUTTON_DOWN:
        if (event->button != 1)
            return 0;
        memset(&press, 0, sizeof(press));
        press.down = 1;
        press.x0 = event->x;
        press.y0 = press.last_y = event->y;
        if (grabs(event->x, event->y)) {
            press.grabbed = 1;
            deliver(event);
            return 1;
        }
        return 0;
    case MENU_EVENT_MOTION:
        if (!press.down)
            return 0;
        if (press.grabbed) {
            deliver(event);
            return 1;
        }
        if (!press.moved && abs(event->y - press.y0) < slop && abs(event->x - press.x0) < slop)
            return 0;
        press.moved = 1;
        drag(event->y - press.last_y);
        press.last_y = event->y;
        return 1;
    case MENU_EVENT_BUTTON_UP:
        if (event->button != 1 || !press.down)
            return 0;
        if (press.grabbed)
            deliver(event);
        else if (!press.moved) { /* a tap: the press where it went down, then the release */
            deliver_at(MENU_EVENT_BUTTON_DOWN, press.x0, press.y0);
            if (shown)
                deliver_at(MENU_EVENT_BUTTON_UP, press.x0, press.y0);
        }
        memset(&press, 0, sizeof(press));
        if (shown)
            deliver_at(MENU_EVENT_LEAVE, 0, 0); /* a finger leaves no hover behind */
        return 1;
    default:
        return 0;
    }
}

int Panel_Key(const MenuEvent *event, int control_key, int repeat, int modifiers)
{
    if (!shown)
        return 0;
    if (shown == PANEL_MODS) {
        if (event->type == MENU_EVENT_KEY_UP)
            return 0;
        if (ModsWindow_Event(event))
            Panel_Close();
        return 1;
    }
    if (event->type != MENU_EVENT_KEY_DOWN && event->type != MENU_EVENT_KEY_UP)
        return 0;
    if (repeat)
        return 0;
    ControlsRuntime_Key(control_key, event->type == MENU_EVENT_KEY_DOWN);
    ControlsWindow_Key(control_key, event->type == MENU_EVENT_KEY_DOWN, repeat, modifiers);
    ControlsWindow_Tick();
    if (ControlsWindow_ShouldClose())
        Panel_Close();
    return 1;
}

int Panel_Tick(void)
{
    uint64_t now;
    if (shown == PANEL_MODS)
        return ModsWindow_Tick(); /* an import's next step */
    if (shown != PANEL_CONTROLS)
        return 0;
    ControlsWindow_Tick();
    if (ControlsWindow_ShouldClose()) {
        Panel_Close();
        return 1;
    }
    /* The pad picture and the lists light what is held, and a capture
     * counts down: ten pictures a second, the window's whole canvas each. */
    now = ControlsRuntime_Now();
    if (now - last_tick < 100000)
        return 0;
    last_tick = now;
    return 1;
}

int Panel_TextFocus(void) { return shown == PANEL_MODS && ModsWindow_TextFocus(); }
int Panel_Tapped(void) { return tapped; }
