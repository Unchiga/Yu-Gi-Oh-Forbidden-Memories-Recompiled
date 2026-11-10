/* The title's two menus as the mods' "menu" key has them (title_menu.h).
 *
 * How the game runs them (src/overlays/main_menu/frontend*.c): eleven
 * sprites, one per entry, the first menu's five and the second's six, of
 * which the ones of the menu shown slide in from alternate sides over 16
 * ticks (MainMenu_StartFrontendEntryTransition) leaving afterimages; the
 * cursor is gMain_bMenuID, the selected entry, which also says which menu
 * is up (under 5 the first). Up and down step it through its menu; a choice
 * either opens a dialog in the menu (load, 2P duel, trade, save) or slides
 * the entries out and returns the entry, which Main_ApplyMenuSelection acts
 * on.
 *
 * Here the cursor is a row in the menu's list of shown items
 * (TitleConfig.order), which may hold buttons, and gMain_bMenuID is kept
 * on the entry under the cursor, or on the menu's first entry while it is
 * on a button (`carrier`): the game draws its own entries' highlight from
 * it and knows the menu by it. The pad's up and down are taken before the
 * game sees them. A choice of an entry that does what it does in the game
 * is left to the game; any other is made here, and one that leaves the
 * title slides the entries out as the game's own do, with the game
 * returning its carrier and this the choice instead. Entries and buttons
 * drawn here are the entries' size and place and slide the game's way,
 * with afterimages of their own; with the mods' "slide": "smooth" every
 * entry (the game's too) takes a 24-tick eased slide instead, without
 * afterimages, from and to beyond the active aspect's edges.
 *
 * An item's "scale" sizes it about its middle: a picture or a label is
 * drawn at that size (title_images.c), and an entry of the game's own is
 * drawn here too, its sprites -- as the game sorts them, the cursor's
 * look or not -- each a quad that size. */
#include "title_menu.h"
#include "title_config.h"
#include "title_images.h"
#include "menu_letters.h"
#include "menu.h"
#include "platform.h"
#include "pc/guest/state.h"
#include "pc/mods/mods.h"
#include "pc/tower/tower.h"
#include "types.h"
#include "game/display_object.h"
#include "game/display_object_helpers.h"
#include "game/display_object_config.h"
#include "game/display_object_render_sprite_sheet.h"
#include "pc/cards/duel_ui.h"
#include "game/input.h"
#include "game/sound.h"
#include "game/ordering_tables.h"
#include "psyq/libgte.h"
#include "psyq/libgpu.h"
#include "psyq/libgs.h"
#include "overlays/main_menu/frontend.h"
#include <stdio.h>
#include <string.h>

enum { MIDDLE = 0xA0, LEFT_OFF = -0xA0, RIGHT_OFF = 0x1E0, GAME_TICKS = 0x10, GHOSTS = 8,
       UNSELECTED_LEVEL = 0x60, FULL = 0x80, LOCKED_LEVEL = 0x38 };
/* The game's sounds (frontend_update.c): the cursor, a choice, back, and
 * the buzz of a choice that is not there. */
enum { SE_MOVE = 6, SE_CHOOSE = 7, SE_BACK = 8, SE_BUZZ = 9 };
/* The debug menu is Main_ApplyMenuSelection's `default`. */
enum { DEBUG_SELECTION = 11 };

typedef struct {
    int x, level;
} Ghost;

/* All of it in one block for save states. */
static struct {
    int open;
    int cursor[TITLE_MENUS];   /* a row of TitleConfig.order */
    int carrier;               /* what gMain_bMenuID was left at, -1 not yet */
    int leaving;               /* a choice made here, returned at the end of the slide; -1 none */
    int left_item, left_menu;  /* the item a choice made here was made on, for coming back to it */
    int prompt;                /* PUSH START BUTTON was up after the last update */
    int from[TITLE_ITEMS], to[TITLE_ITEMS], x[TITLE_ITEMS];
    Ghost ghosts[TITLE_ITEMS][GHOSTS];
} run = {0, {0, 0}, -1, -1, -1, 0, 1, {0}, {0}, {0}, {{{0, 0}}}};

static const TitleConfig *config(void) { return TitleConfig_Get(); }
int TitleMenu_Smooth(void) { return config()->smooth_slide; }
/* A slide's ticks: the game's 16, or the smooth slide's. */
static int ticks(void) { return TitleMenu_Smooth() ? TITLE_MENU_SLIDE_TICKS : GAME_TICKS; }
/* An item's place, its widescreen one while View > Aspect is 16:9. */
static int item_x(const TitleItem *it) { return TitleWide_X(&it->wide, it->x, Platform_Widescreen()); }
static int item_y(const TitleItem *it)
{
    return it->hidden ? TITLE_PARKED_Y : TitleWide_Y(&it->wide, it->y, Platform_Widescreen());
}
static DisplayObject *entry(int i) { return (DisplayObject *)gMain_apMenuEntries[i]; }
static int menu_of(int id) { return id >= TITLE_FIRST_MENU; }
static int base_of(int menu) { return menu ? TITLE_FIRST_MENU : 0; }

/* One of the game's entries at a size of a mod's: drawn here from its
 * sprites (draw_sized). */
static int sized_entry(int i)
{
    return i < TITLE_ENTRIES && config()->items[i].scale != 100 && entry(i) &&
           !TitleImages_Ready(TITLE_IMAGE_ITEM(i, 0), NULL, NULL);
}

/* Drawn here: an item with a picture made for it this time, or a sized entry. */
static int drawn_here(int i)
{
    return TitleImages_Ready(TITLE_IMAGE_ITEM(i, 0), NULL, NULL) || sized_entry(i);
}

static int prompt_up(void)
{
    return D_80184560 && (D_80184560->flags & DISPLAY_OBJECT_FLAG_RENDERABLE);
}

int TitleMenu_Showing(void)
{
    if (!run.open || prompt_up() || !entry(base_of(menu_of(gMain_bMenuID)))) return -1;
    return menu_of(gMain_bMenuID);
}

const char *TitleMenu_ItemName(int index)
{
    if (index < 0 || index >= TITLE_ITEMS || !config()->items[index].used) return NULL;
    return config()->items[index].name;
}

static int row_of(int menu, int item)
{
    int row;
    for (row = 0; row < config()->shown[menu]; row++) {
        if (config()->order[menu][row] == item) return row;
    }
    return -1;
}

static int cursor_item(int menu)
{
    int shown = config()->shown[menu];
    if (!shown) return base_of(menu);
    if (run.cursor[menu] < 0 || run.cursor[menu] >= shown) run.cursor[menu] = 0;
    return config()->order[menu][run.cursor[menu]];
}

/* Each entry of the menu highlighted or not by the cursor, whatever the
 * game made of gMain_bMenuID. */
static void highlight(int menu)
{
    int item = cursor_item(menu), i;
    for (i = base_of(menu); i < base_of(menu) + (menu ? TITLE_ENTRIES - TITLE_FIRST_MENU : TITLE_FIRST_MENU); i++) {
        if (entry(i)) DisplayObject_SetResourceVariant((DisplayObjectConfig *)entry(i), i << 1 | (item != i));
    }
}

/* gMain_bMenuID on the cursor's entry, or the menu's first while it is on a
 * button, and the highlights with it. */
static void settle(int menu)
{
    int item = cursor_item(menu);
    gMain_bMenuID = (u8)(item < TITLE_ENTRIES ? item : base_of(menu));
    run.carrier = gMain_bMenuID;
    highlight(menu);
}

/* The cursor onto entry `id` of its menu, or the menu's first row; the
 * first menu opened from PUSH START BUTTON (on NEW GAME, 0, in the game)
 * opens on its top row, whatever is there. */
static void cursor_to_entry(int id)
{
    int menu = menu_of(id), row = id ? row_of(menu, id) : 0;
    run.cursor[menu] = row < 0 ? 0 : row;
}

/* A slide has begun (MainMenu_StartFrontendEntryTransition, D_80184596
 * in or out): where each item drawn here goes, from and to. */
static void begin_slide(int menu)
{
    int row;
    if (!TitleMenu_Smooth()) {
        for (row = 0; row < config()->shown[menu]; row++) {
            /* Alternate sides down the menu, as the game's own entries go
             * by their number (the second menu's first from the right). */
            int item = config()->order[menu][row], side = ((row + base_of(menu)) & 1) ? RIGHT_OFF : LEFT_OFF;
            int place = MIDDLE + item_x(&config()->items[item]);
            DisplayObject *object = item < TITLE_ENTRIES && !drawn_here(item) ? entry(item) : NULL;
            run.from[item] = D_80184596 ? place : side;
            run.to[item] = D_80184596 ? side : place;
            if (!object) continue;
            /* The game slides its entries to and from the middle: an entry
             * a mod moves (its "x") to and from its own place instead. */
            if (D_80184596) {
                object->field_38.h.field_38 = (s16)side;
                if ((s16)object->field_34.h.field_36 == MIDDLE) {
                    object->field_34.h.field_36 = (s16)place;
                    object->field_30.h.field_30 = (s16)place;
                }
            } else {
                object->field_34.h.field_36 = (s16)side;
                object->field_30.h.field_30 = (s16)side;
                if ((s16)object->field_38.h.field_38 == MIDDLE) object->field_38.h.field_38 = (s16)place;
            }
        }
        return;
    }
    for (row = 0; row < config()->shown[menu]; row++) {
        /* Alternate sides down the menu, as the game's own entries go
         * by their number (the second menu's first from the right). */
        int item = config()->order[menu][row], width = 320 * config()->items[item].scale / 100, selected_width = 0;
        int margin = Platform_Widescreen() ? TITLE_WIDE_MARGIN : 0;
        int side;
        TitleImages_Ready(TITLE_IMAGE_ITEM(item, 0), &width, NULL);
        TitleImages_Ready(TITLE_IMAGE_ITEM(item, 1), &selected_width, NULL);
        if (selected_width > width) width = selected_width;
        side = ((row + base_of(menu)) & 1) ? 320 + margin + width / 2 + 16 : -margin - width / 2 - 16;
        int place = MIDDLE + item_x(&config()->items[item]);
        DisplayObject *object = item < TITLE_ENTRIES && !drawn_here(item) ? entry(item) : NULL;
        run.from[item] = D_80184596 ? place : side;
        run.to[item] = D_80184596 ? side : place;
        run.x[item] = run.from[item];
        if (!object) continue;
        object->field_34.h.field_36 = (s16)run.from[item];
        object->field_38.h.field_38 = (s16)run.to[item];
        object->field_30.h.field_30 = (s16)run.from[item];
    }
}

void TitleMenu_StartSlide(void)
{
    int i;
    if (!TitleMenu_Smooth()) return; /* the game's slide, as the game began it */
    for (i = 0; i < TITLE_ENTRIES; i++) {
        if (entry(i)) entry(i)->field_60 = TITLE_MENU_SLIDE_TICKS;
    }
    if (run.open) begin_slide(menu_of(gMain_bMenuID));
}

void TitleMenu_Opened(void)
{
    int i, id = gMain_bMenuID % TITLE_ENTRIES, menu = menu_of(id);
    run.open = 1;
    run.leaving = -1;
    run.prompt = prompt_up();
    memset(run.ghosts, 0, sizeof(run.ghosts));
    for (i = 0; i < TITLE_ITEMS; i++) run.x[i] = run.from[i] = run.to[i] = LEFT_OFF;
    /* Back from a choice made here: on the item it was made on. */
    if (run.left_item >= 0 && run.left_menu == menu && row_of(menu, run.left_item) >= 0)
        run.cursor[menu] = row_of(menu, run.left_item);
    else
        cursor_to_entry(id);
    run.left_item = -1;
    settle(menu);
    /* MainMenu_InitFrontendMenu has begun the entries' slide in. */
    if (D_80184599) begin_slide(menu);
}

void TitleMenu_Closed(void)
{
    run.open = 0;
}

/* The entries slide out and the title returns `selection`. */
static void leave(int menu, int selection)
{
    SD_SEPlay(SE_CHOOSE, 0xFF, 0);
    run.leaving = selection;
    /* Not load's 1, which the end of the slide takes for "on to the second menu". */
    gMain_bMenuID = (u8)base_of(menu);
    run.carrier = gMain_bMenuID;
    MainMenu_StartFrontendEntryTransition(1);
}

static void quit_chosen(int button, int *quit)
{
    if (button == 0) *quit = 1;
}

/* What item `item` of `menu` does, chosen: 1 when it was done here, 0 when
 * the game is to do it (its own entry's own choice, or a dialog of the
 * menu's, with gMain_bMenuID set to it). */
static int choose(int menu, int item)
{
    const TitleItem *it = &config()->items[item];
    static const char *const ok[] = {"OK"}, *const quit[] = {"Quit", "Cancel"};
    MemoriesModEvent event = {MEMORIES_EVENT_MENU, MEMORIES_BEFORE, 0, 0, 0, -1, 0};
    int action = it->action;
    event.a = item;
    event.b = menu;
    event.c = it->value;
    Mods_Dispatch(&event);
    run.left_item = item;
    run.left_menu = menu;
    if (event.handled) {
        if (event.result >= 0) leave(menu, event.result);
    } else if (action == TITLE_ACTION_OWN || action == 1 || action == 2 || action == 3 || action == 10) {
        /* The game's: the entry's own, or a dialog it opens in the menu
         * (load, 2P duel, trade, save), gMain_bMenuID left on it until the
         * game is done with it. */
        if (action == TITLE_ACTION_OWN) run.left_item = -1;
        else gMain_bMenuID = (u8)action;
        run.carrier = gMain_bMenuID;
        event.phase = MEMORIES_AFTER;
        Mods_Dispatch(&event);
        return 0;
    } else if (action == TITLE_ACTION_TOWER) {
        /* The tower (pc/tower/tower.h), while its "unlock" holds for the
           save loaded; else a buzz, the item dimmed (TitleMenu_Draw). */
        if (Tower_Config() && Tower_Unlocked()) leave(menu, TOWER_MENU_SELECTION);
        else SD_SEPlay(SE_BUZZ, 0xFF, 0);
    } else if (action >= 0 && action < TITLE_ENTRIES) {
        leave(menu, action);
    } else if (action == TITLE_ACTION_BACK) {
        if (menu == 0 && (!config()->press_start || config()->layers[2].hidden)) {
            SD_SEPlay(SE_BUZZ, 0xFF, 0);
        } else {
            /* The game's own way back (frontend_update.c, D_80184595): the
             * second menu to the first, the first to PUSH START BUTTON. */
            SD_SEPlay(SE_BACK, 0xFF, 0);
            gMain_bMenuID = (u8)base_of(menu);
            run.carrier = gMain_bMenuID;
            D_80184595 = 1;
            MainMenu_StartFrontendEntryTransition(1);
        }
    } else if (action == TITLE_ACTION_NOTICE) {
        SD_SEPlay(SE_CHOOSE, 0xFF, 0);
        Menu_ShowNotice(it->notice_title[0] ? it->notice_title : it->label[0] ? it->label : "", it->notice, ok, 1, 0,
                        NULL);
    } else if (action == TITLE_ACTION_QUIT) {
        SD_SEPlay(SE_CHOOSE, 0xFF, 0);
        Menu_ShowNotice("Quit", "Quit the game?", quit, 2, 1, quit_chosen);
    } else if (action == TITLE_ACTION_DEBUG_MENU) {
        leave(menu, DEBUG_SELECTION);
    } else {
        if (action == TITLE_ACTION_EVENT)
            fprintf(stderr, "memories-pc: menu: no code mod took %s's event\n", it->name);
        SD_SEPlay(SE_BUZZ, 0xFF, 0);
    }
    event.phase = MEMORIES_AFTER;
    Mods_Dispatch(&event);
    return 1;
}

/* Waiting for the player: no slide, fade, dialog or notice. */
static int waiting(void)
{
    return run.open && !prompt_up() && !D_80184599 && !D_80184598 && !D_8018459A && !D_8018459B && !D_8018459C &&
           !D_8018459D && run.leaving < 0 && !Menu_NoticeShown() && entry(base_of(menu_of(gMain_bMenuID)));
}

unsigned TitleMenu_Before(void)
{
    static int notice;
    unsigned short repeat = gInput_wPad1Repeat, pressed = gInput_wPad1Pressed;
    int menu = menu_of(gMain_bMenuID), shown = config()->shown[menu], had = notice;
    /* A notice has the pad, and the frame it closes on too: the press that
     * closed it is not a choice. */
    notice = Menu_NoticeShown();
    if ((notice || had) && run.open && !prompt_up()) return 0xFFFF;
    if (!waiting() || !shown) return 0;
    if (repeat & PAD_DIRECTION_VERTICAL_MASK) {
        int step = (repeat & PAD_DIRECTION_UP) ? -1 : 1;
        run.cursor[menu] = (run.cursor[menu] + step + shown) % shown;
        settle(menu);
        SD_SEPlay(SE_MOVE, 0xFF, 0);
        return PAD_DIRECTION_VERTICAL_MASK;
    }
    if ((pressed & (PAD_BUTTON_START | PAD_BUTTON_CANCEL | PAD_BUTTON_CONFIRM_MASK)) && !(pressed & PAD_BUTTON_CANCEL))
        return choose(menu, cursor_item(menu)) ? 0xFFFF : 0;
    return 0;
}

/* The mouse over the menu. An item is where it is drawn: its middle at run.x and its y, as large as its picture (or
 * the game's 96 x 24 entry, at the item's size in percent). Looked up by the item's place and size, not by the
 * game's sprites, which stand off the screen for every entry a mod gave a picture. */
int TitleMenu_PointerAt(int x, int y)
{
    int menu = TitleMenu_Showing(), row;
    if (menu < 0 || !waiting()) return -1;
    for (row = 0; row < config()->shown[menu]; row++) {
        int item = config()->order[menu][row], w = 96, h = 24, which;
        const TitleItem *it = &config()->items[item];
        if (it->hidden) continue;
        which = TITLE_IMAGE_ITEM(item, 0);
        if (!TitleImages_Ready(which, &w, &h)) {
            const int scale = it->scale > 0 ? it->scale : 100;
            w = 96 * scale / 100;
            h = 24 * scale / 100;
        }
        if (x >= run.x[item] - w / 2 && x < run.x[item] - w / 2 + w && y >= item_y(it) - h / 2 &&
            y < item_y(it) - h / 2 + h)
            return item;
    }
    return -1;
}

/* The cursor onto `item`, as the pad's Up and Down would leave it. 1 when it moved. */
int TitleMenu_PointerSelect(int item)
{
    int menu = TitleMenu_Showing(), row;
    if (menu < 0 || !waiting()) return 0;
    row = row_of(menu, item);
    if (row < 0 || row == run.cursor[menu]) return 0;
    run.cursor[menu] = row;
    settle(menu);
    SD_SEPlay(SE_MOVE, 0xFF, 0);
    return 1;
}

/* The game's slide (frontend_update.c): from + sin(90 degrees * t / 16) *
 * (to - from), t the ticks gone. The smooth one: smoothstep, at rest at
 * both ends, the same rounded place for the game's sprites and the port's
 * pictures. */
int TitleMenu_SlidePosition(int from, int to, int timer)
{
    enum { TICKS = TITLE_MENU_SLIDE_TICKS };
    int frame = TICKS - timer, weight, product;
    if (!TitleMenu_Smooth()) {
        frame = GAME_TICKS - timer;
        if (frame == GAME_TICKS) return to;
        product = rsin(frame << 6) * (to - from);
        if (product < 0) product += 0xFFF;
        return from + (product >> 12);
    }
    if (frame <= 0) return from;
    if (frame >= TICKS) return to;
    weight = frame * frame * (3 * TICKS - 2 * frame);
    product = (to - from) * weight;
    return from + (product >= 0 ? (product + TICKS * TICKS * TICKS / 2) / (TICKS * TICKS * TICKS)
                               : -((-product + TICKS * TICKS * TICKS / 2) / (TICKS * TICKS * TICKS)));
}

int TitleMenu_After(int result)
{
    int menu, i, row, timer;
    DisplayObject *reference;
    if (!run.open) return result;
    if (result >= 0 && run.leaving >= 0) {
        result = run.leaving;
        run.leaving = -1;
        return result;
    }
    menu = menu_of(gMain_bMenuID);
    /* The game moved on: the second menu after a load, the first after
     * backing out of it, or the menu after PUSH START BUTTON. */
    if (gMain_bMenuID != run.carrier || (run.prompt && !prompt_up())) {
        cursor_to_entry(gMain_bMenuID);
        settle(menu);
    } else if (waiting()) {
        settle(menu);
    } else {
        highlight(menu);
    }
    run.prompt = prompt_up();
    reference = entry(base_of(menu));
    if (!reference) return result;
    timer = reference->field_60;
    /* A slide begun this update: where each item drawn here goes. */
    if (timer == ticks() && D_80184599) begin_slide(menu);
    for (i = 0; i < TITLE_ITEMS; i++) {
        int g;
        for (g = 0; g < GHOSTS; g++) {
            if (run.ghosts[i][g].level > 0) run.ghosts[i][g].level -= 8;
        }
    }
    for (row = 0; row < config()->shown[menu]; row++) {
        int item = config()->order[menu][row];
        /* At rest after sliding in, its place now: widescreen may have
         * been turned on or off since. */
        run.x[item] = D_80184599 ? TitleMenu_SlidePosition(run.from[item], run.to[item], timer)
                      : D_80184596 ? run.to[item] : MIDDLE + item_x(&config()->items[item]);
        if (!TitleMenu_Smooth() && D_80184599 && ((GAME_TICKS - timer) & 1) && timer < GAME_TICKS) {
            int g, oldest = 0;
            for (g = 1; g < GHOSTS; g++) {
                if (run.ghosts[item][g].level < run.ghosts[item][oldest].level) oldest = g;
            }
            run.ghosts[item][oldest].x = run.x[item];
            run.ghosts[item][oldest].level = FULL;
        }
    }
    return result;
}

/* d * scale / 100, rounded half away from 0: a point's place from an
 * item's middle at its size. */
static int scaled(int d, int scale)
{
    int t = d * scale;
    return t >= 0 ? (t + 50) / 100 : -((-t + 50) / 100);
}

/* One sprite the game sorts for an entry as a quad from x0, y0 to x1, y1,
 * its far edges the texel after its last (at a page's edge, 255, the
 * last), as duel_ui.c draws a sized one. */
static void sized_quad(const GsSPRITE *sprite, u32 attribute, int x0, int y0, int x1, int y1, int r, int g, int b,
                       void *ot, int depth)
{
    POLY_FT4 quad;
    int u0 = sprite->u, v0 = sprite->v, u1 = u0 + sprite->w > 255 ? u0 + sprite->w - 1 : u0 + sprite->w,
        v1 = v0 + sprite->h > 255 ? v0 + sprite->h - 1 : v0 + sprite->h;
    if (x1 <= x0 || y1 <= y0) return;
    memset(&quad, 0, sizeof(quad));
    setPolyFT4(&quad);
    if (attribute & 0x40000000) setSemiTrans(&quad, 1);
    /* Its colors as they are: unmodulated, so not dithered, as the sprite
     * it stands for is not. */
    if ((attribute & 0x40) || (r == 0x80 && g == 0x80 && b == 0x80)) setShadeTex(&quad, 1);
    quad.r0 = (u8)r;
    quad.g0 = (u8)g;
    quad.b0 = (u8)b;
    quad.tpage = (u16)((sprite->tpage & 0x1F) | ((attribute >> 17) & 0x180) | ((attribute >> 23) & 0x60));
    quad.clut = getClut(sprite->cx, sprite->cy);
    quad.x0 = quad.x2 = (short)x0;
    quad.x1 = quad.x3 = (short)x1;
    quad.y0 = quad.y1 = (short)y0;
    quad.y2 = quad.y3 = (short)y1;
    quad.u0 = quad.u2 = (u8)u0;
    quad.u1 = quad.u3 = (u8)u1;
    quad.v0 = quad.v1 = (u8)v0;
    quad.v2 = quad.v3 = (u8)v1;
    GsSortPoly(&quad, (GsOT *)ot, (unsigned short)depth);
}

/* Entry `i` at its size, its middle at x, y: the sprites the game would
 * sort for it, taken with it where the screen keeps them all (the middle
 * of it), each scaled about its place. `fade` 0 for the entry itself, else
 * how much darker an afterimage of it is: added, as the game's own
 * (MainMenu_SpawnFrontendEntryAfterimage), each color down by that. */
static void draw_sized(int i, void *ot, int depth, int x, int y, int fade)
{
    DisplayObject *object = entry(i);
    DisplayObjectCapture caught;
    int scale = config()->items[i].scale, saved_x = object->field_30.h.field_30, saved_y = object->field_30.h.field_32,
        n;
    if (!object->field_4C) return;
    object->field_30.h.field_30 = MIDDLE;
    object->field_30.h.field_32 = 120;
    caught.count = 0;
    DisplayObject_Capture = &caught;
    DisplayObject_RenderSpriteSheet(object, (s32)ot, depth);
    DisplayObject_Capture = NULL;
    object->field_30.h.field_30 = (u16)saved_x;
    object->field_30.h.field_32 = (u16)saved_y;
    for (n = 0; n < caught.count; n++) {
        const GsSPRITE *sprite = (const GsSPRITE *)&caught.sprite[n];
        int left = sprite->x - MIDDLE, top = sprite->y - 120, r = sprite->r, g = sprite->g, b = sprite->b;
        u32 attribute = sprite->attribute;
        if (fade) {
            attribute = (attribute & ~0x30000000u) | 0x50000000u;
            r = r > fade ? r - fade : 0;
            g = g > fade ? g - fade : 0;
            b = b > fade ? b - fade : 0;
            if (!r && !g && !b) continue;
        }
        sized_quad(sprite, attribute, x + scaled(left, scale), y + scaled(top, scale), x + scaled(left + sprite->w, scale),
                   y + scaled(top + sprite->h, scale), r, g, b, ot, depth);
    }
}

void TitleMenu_Draw(void)
{
    int menu = TitleMenu_Showing(), row, depth;
    DisplayObject *reference;
    if (menu < 0) return;
    reference = entry(base_of(menu));
    if (!(reference->flags & DISPLAY_OBJECT_FLAG_RENDERABLE)) return;
    /* A label in the game's letters waits for the entries to have drawn
     * once (menu_letters.h): the pictures made again, now with it. */
    if (MenuLetters_Waiting() && reference->field_4C) TitleImages_Prepare(config());
    depth = D_8009AF74[1];
    for (row = 0; row < config()->shown[menu]; row++) {
        int item = config()->order[menu][row], selected = cursor_item(menu) == item, w, h, which, level, g;
        const TitleItem *it = &config()->items[item];
        if (!drawn_here(item)) continue;
        if (sized_entry(item)) {
            draw_sized(item, D_800E9D90[1], depth, run.x[item], item_y(it), 0);
            for (g = 0; g < GHOSTS; g++) {
                if (run.ghosts[item][g].level > 0)
                    draw_sized(item, D_800E9D90[1], depth + 1, run.ghosts[item][g].x, item_y(it),
                               FULL - run.ghosts[item][g].level);
            }
            continue;
        }
        which = TITLE_IMAGE_ITEM(item, 1);
        /* Without a picture for the cursor, the one picture, darker off it. */
        if (!selected || !TitleImages_Ready(which, NULL, NULL)) which = TITLE_IMAGE_ITEM(item, 0);
        level = selected || TitleImages_Ready(TITLE_IMAGE_ITEM(item, 1), NULL, NULL) ? FULL : UNSELECTED_LEVEL;
        /* A tower still locked for this save: dimmed, the cursor's look
           kept (choose buzzes). */
        if (it->action == TITLE_ACTION_TOWER && !(Tower_Config() && Tower_Unlocked())) level = LOCKED_LEVEL;
        TitleImages_Ready(which, &w, &h);
        TitleImages_Draw(which, D_800E9D90[1], depth, run.x[item] - w / 2, item_y(it) - h / 2,
                         level * (int)(it->tint >> 16 & 0xFF) / 0xFF, level * (int)(it->tint >> 8 & 0xFF) / 0xFF,
                         level * (int)(it->tint & 0xFF) / 0xFF, 0);
        for (g = 0; g < GHOSTS; g++) {
            const Ghost *ghost = &run.ghosts[item][g];
            if (ghost->level <= 0) continue;
            TitleImages_Draw(which, D_800E9D90[1], depth + 1, ghost->x - w / 2, item_y(it) - h / 2, ghost->level, ghost->level,
                             ghost->level, 1);
        }
    }
}

void TitleMenu_Place(void)
{
    int i, menu = menu_of(gMain_bMenuID);
    if (!run.open) return;
    for (i = 0; i < TITLE_ENTRIES; i++) {
        const TitleItem *it = &config()->items[i];
        DisplayObject *object = entry(i);
        if (!object) continue;
        /* The entries drawn here stand off the screen, as hidden ones do. */
        object->field_30.h.field_32 = (s16)(drawn_here(i) ? TITLE_PARKED_Y : item_y(it));
        if (!D_80184599 && !D_80184596 && menu_of(i) == menu && !it->hidden && !drawn_here(i))
            object->field_30.h.field_30 = (s16)(MIDDLE + item_x(it));
    }
}

int TitleMenu_Reopen(int selection, int reopen)
{
    if (selection < TITLE_ENTRIES) return reopen;
    /* One of a mod's own: back to the menu it was made from, past PUSH
     * START BUTTON (any first-menu entry but the first skips it). */
    return run.left_menu ? TITLE_FIRST_MENU : TITLE_FIRST_MENU - 1;
}

void TitleMenu_State(MemoriesState *state)
{
    MemoriesStateField fields[] = {{&run, sizeof(run)}};
    Memories_StateChunk(state, "title-menu", fields, 1);
}
