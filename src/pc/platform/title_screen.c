/* The mods' "title" key (title_screen.h). The title is the main-menu
 * module's (src/overlays/main_menu): three sprites from its resource bank --
 * the logo, the "(c) 1996 KAZUKI TAKAHASHI" line and PUSH START BUTTON -- over a tiled picture and a shade
 * drawn by MainMenu_DrawFrontendBackground, and the eleven entries of its
 * two menus, sprites too. Nothing on it is text, and every place and color
 * is a number in the code, so what a mod changes is applied here to the
 * objects the game made, after it made them and after each update; what
 * they show is a texture pack's (notes/modding.md). */
#include "title_screen.h"
#include "menu.h"
#include "platform.h"
#include "pc/cards/fusion_helper.h"
#include "title_config.h"
#include "title_images.h"
#include "title_menu.h"
#include "title_jump.h"
#include "pc/mods/mods.h"
#include "pc/saves/save_menu.h"
#include "pc/saves/deck_menu.h"
#include "pc/guest/state.h"
#include "pc/tower/tower.h"
#include "types.h"
#include "game/display_object.h"
#include "game/display_object_layout.h"
#include "game/display_object_config.h"
#include "game/display_object_helpers.h"
#include "game/fade.h"
#include "game/input.h"
#include "overlays/main_menu/frontend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The middle of each of the game's three pictures, measured on the retail
 * title (2026-09-29): a picture of a mod's own stands there, moved by the
 * layer's x and y. */
static const struct { int x, y; } middles[TITLE_LAYERS] = {{162, 90}, {163, 207}, {160, 185}};
enum { OFF_SCREEN = -400 };
/* The added pictures' place in the background's ordering table. */
enum { PICTURE_DEPTH = 1024 };

static int open;                /* between TitleScreen_Opened and _Closed */
/* View > Aspect 16:9: the picture has TITLE_WIDE_MARGIN more either side. */
static int wide(void) { return Platform_Widescreen(); }
static void show_layers(void);
static void step_fade(void);
static int prompt_level = 0x80; /* PRESS START's own pulse, under its tint */
static int idle;
/* A "fade" (title_config.h): the background drawn, the title's (0) or the
 * menus' (1), lags the menu by a fade to black, then out of it; -1 takes
 * the menu's at once (the title just opened). `fade` is the black, 0-255. */
static int shown = -1, fade;

int TitleScreen_Song(void)
{
    return TitleConfig_Load()->song;
}

int TitleScreen_SkipMovie(void)
{
    int skip = TitleConfig_Load()->skip_movie; /* read on every entry: the title's update uses it */
    return TitleJump_Pending() ? 1 : skip;   /* on its way to another screen (title_jump.h) */
}

/* 0xRRGGBB over the game's 128 = unchanged. */
static int scale(uint32_t tint, int shift, int level)
{
    return level * (int)(tint >> shift & 0xFF) / 0xFF;
}

static void paint(DisplayObject *object, uint32_t tint, int level)
{
    u8 *color = (u8 *)&object->field_0C;
    color[0] = (u8)scale(tint, 16, level);
    color[1] = (u8)scale(tint, 8, level);
    color[2] = (u8)scale(tint, 0, level);
}

static DisplayObject *layer(int i)
{
    return i == 0 ? D_80184558 : i == 1 ? D_8018455C : D_80184560;
}

static DisplayObject *entry(int i)
{
    return (DisplayObject *)gMain_apMenuEntries[i];
}

static void tint_entries(void)
{
    int i;
    for (i = 0; i < TITLE_ENTRIES; i++) {
        if (entry(i) && TitleConfig_Get()->items[i].tint != 0xFFFFFF) paint(entry(i), TitleConfig_Get()->items[i].tint, 0x80);
    }
}

void TitleScreen_Opened(void)
{
    int i;
    DisplayObject *prompt = D_80184560;
    open = 1;
    idle = 0;
    prompt_level = 0x80;
    TitleImages_Prepare(TitleConfig_Get());
    for (i = 0; i < TITLE_LAYERS; i++) {
        DisplayObject *object = layer(i);
        if (!object) continue;
        /* A picture of the mod's own stands in: the game's goes off the
         * screen but keeps running, PUSH START BUTTON's pulse and all. */
        if (TitleImages_Ready(TITLE_IMAGE_LOGO + i, NULL, NULL)) object->field_30.h.field_32 = OFF_SCREEN;
        object->field_30.h.field_30 += TitleWide_X(&TitleConfig_Get()->layers[i].wide, TitleConfig_Get()->layers[i].x, wide());
        object->field_30.h.field_32 += TitleWide_Y(&TitleConfig_Get()->layers[i].wide, TitleConfig_Get()->layers[i].y, wide());
        if (TitleConfig_Get()->layers[i].hidden && i != 2) object->flags &= ~DISPLAY_OBJECT_FLAG_RENDERABLE;
        if (TitleConfig_Get()->layers[i].tint != 0xFFFFFF) paint(object, TitleConfig_Get()->layers[i].tint, 0x80);
    }
    /* PRESS START hidden or skipped: straight to the menu, as the game opens
     * it on any entry but the first (frontend.c). The entries' slide in is
     * already under way. */
    if (prompt && (!TitleConfig_Get()->press_start || TitleConfig_Get()->layers[2].hidden) && (prompt->flags & DISPLAY_OBJECT_FLAG_RENDERABLE)) {
        prompt->flags &= ~DISPLAY_OBJECT_FLAG_RENDERABLE;
        D_80184597 = 0x80;
    }
    Mods_SetMenuItemSource(TitleMenu_ItemName);
    TitleMenu_Opened();
    TitleMenu_Place();
    tint_entries();
    show_layers();
}

void TitleScreen_Closed(void)
{
    open = 0;
    shown = -1;
    fade = 0;
    TitleMenu_Closed();
}

void TitleScreen_State(MemoriesState *state)
{
    MemoriesStateField fields[] = {{&open, sizeof(open)}, {&idle, sizeof(idle)}, {&prompt_level, sizeof(prompt_level)}};
    TitleMenu_State(state);
    if (!Memories_StateLoading(state)) {
        Memories_StateChunk(state, "title-screen", fields, 3);
        return;
    }
    /* A state from before the chunk: the title is taken as closed. */
    open = 0;
    idle = 0;
    prompt_level = 0x80;
    shown = -1;
    fade = 0;
    if (Memories_StateChunk(state, "title-screen", fields, 3) && open) TitleImages_Prepare(TitleConfig_Load());
}

static int prompt_showing(void)
{
    return open && D_80184560 && (D_80184560->flags & DISPLAY_OBJECT_FLAG_RENDERABLE);
}

int TitleScreen_Update(void)
{
    DisplayObject *prompt = D_80184560;
    int tinted = TitleConfig_Get()->layers[2].tint != 0xFFFFFF, prompting = prompt_showing(), result;
    u8 *color;
    unsigned short repeat = gInput_wPad1Repeat, pressed = gInput_wPad1Pressed;
    unsigned hidden;

    /* The pulse runs on the game's grey; the tint goes on after. */
    if (prompting && tinted) {
        color = (u8 *)&prompt->field_0C;
        color[0] = color[1] = color[2] = (u8)prompt_level;
    }
    if (prompting && TitleConfig_Get()->idle_frames >= 0) prompt->field_34.h.field_36 = 0;

    /* The menus' cursor and the choices the game would not make
     * (title_menu.h): the pad bits it took are the port's, and the game
     * does not see them. */
    /* Debug > Jump to or the control channel's `jump` (title_jump.h): the
     * game's own debug menu, which Main_ApplyMenuSelection runs for any
     * choice it has no case for (10, the hidden SAVE). */
    if (TitleJump_Pending()) {
        TitleJump_TitleGaveWay();
        return 10;
    }
    hidden = open ? TitleMenu_Before() : 0;
    if (hidden) {
        gInput_wPad1Repeat = (u16)(repeat & ~hidden);
        gInput_wPad1Pressed = (u16)(pressed & ~hidden);
    }
    result = MainMenu_UpdateFrontendMenu();
    if (!open) return result;
    result = TitleMenu_After(result);
    if (result != -1) return result;
    step_fade();

    if (prompt_showing() && tinted) {
        color = (u8 *)&prompt->field_0C;
        prompt_level = color[2];
        paint(prompt, TitleConfig_Get()->layers[2].tint, prompt_level);
    }
    if (prompting && prompt_showing() && TitleConfig_Get()->idle_frames > 0 && ++idle >= TitleConfig_Get()->idle_frames) {
        idle = 0;
        return -2;
    }
    if (!prompt_showing()) idle = 0;
    TitleMenu_Place();
    tint_entries();
    show_layers();
    return result;
}

/* The background shown: the menus' while one is up, else the title's;
 * with a "fade", the one the screen last went black to. */
static int shown_background(void)
{
    return shown >= 0 ? shown : TitleMenu_Showing() >= 0;
}

/* A frame of the "fade": down to black while the background shown is not
 * the one wanted, the switch at black, then back up. */
static void step_fade(void)
{
    int target = TitleMenu_Showing() >= 0, frames = TitleConfig_Get()->fade, step;
    if (frames <= 0 || shown < 0) {
        shown = target;
        fade = 0;
        return;
    }
    step = (255 + frames - 1) / frames;
    if (shown != target) {
        fade += step;
        if (fade >= 255) {
            fade = 255;
            shown = target;
        }
    } else if (fade > 0) {
        fade = fade > step ? fade - step : 0;
    }
}

static const TitleBackground *background(void)
{
    return &TitleConfig_Get()->background[shown_background()];
}

void TitleScreen_BackgroundTint(unsigned char *r, unsigned char *g, unsigned char *b)
{
    *r = (unsigned char)scale(background()->tint, 16, 128);
    *g = (unsigned char)scale(background()->tint, 8, 128);
    *b = (unsigned char)scale(background()->tint, 0, 128);
}

/* The tower's screen (pc/tower/tower.h) draws the title's background with
 * the game's own MainMenu_DrawFrontendBackground: the wall, with none of
 * the mods' pictures, which are put in VRAM only while the title is up. */
static int in_tower(void)
{
    extern u8 D_8009B26C;
    return (D_8009B26C & 0x1F) == TOWER_MODE;
}

/* The mod's picture for the background shown: the menus' own, or the
 * title's (which the menus' is when a mod gave them none). */
static int background_image(void)
{
    if (in_tower()) return -1;
    /* In widescreen, filling it, the picture made for it if there is one. */
    if (TitleScreen_BackgroundMargin() && background()->wide_image.file[0]) {
        if (shown_background() && TitleImages_Ready(TITLE_IMAGE_WIDE_MENU_BACKGROUND, NULL, NULL))
            return TITLE_IMAGE_WIDE_MENU_BACKGROUND;
        if (TitleImages_Ready(TITLE_IMAGE_WIDE_BACKGROUND, NULL, NULL)) return TITLE_IMAGE_WIDE_BACKGROUND;
    }
    if (!background()->image.file[0]) return -1;
    if (shown_background() && TitleImages_Ready(TITLE_IMAGE_MENU_BACKGROUND, NULL, NULL))
        return TITLE_IMAGE_MENU_BACKGROUND;
    return TitleImages_Ready(TITLE_IMAGE_BACKGROUND, NULL, NULL) ? TITLE_IMAGE_BACKGROUND : -1;
}

int TitleScreen_BackgroundMargin(void)
{
    return open && wide() && background()->wide ? TITLE_WIDE_MARGIN : 0;
}

int TitleScreen_ShowPicture(void)
{
    return background()->picture && background_image() < 0;
}

/* The logo and the copyright line, shown or not by their "show" (PUSH START
 * BUTTON is the game's to show). */
static void show_layers(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        DisplayObject *object = layer(i);
        int show = TitleConfig_Get()->layers[i].show, on;
        if (!object || TitleConfig_Get()->layers[i].hidden || show == TITLE_SHOW_ALWAYS) continue;
        on = (show == TITLE_SHOW_MENU) == shown_background();
        if (on) object->flags |= DISPLAY_OBJECT_FLAG_RENDERABLE;
        else object->flags &= ~DISPLAY_OBJECT_FLAG_RENDERABLE;
    }
}

/* The mod's own pictures, in the background's ordering table: its
 * background where the game's wall goes (4095), the others over the shade
 * (4094) and under the menu's dimming (0), where the game's own are seen. */
void TitleScreen_DrawImages(void *ot)
{
    const TitleConfig *config = TitleConfig_Get();
    unsigned char r, g, b;
    int i, w, h, picture = background_image();
    if (in_tower()) return;
    if (background()->picture && picture >= 0) {
        int left = picture == TITLE_IMAGE_WIDE_BACKGROUND || picture == TITLE_IMAGE_WIDE_MENU_BACKGROUND
                       ? -TITLE_WIDE_MARGIN : 0;
        TitleScreen_BackgroundTint(&r, &g, &b);
        TitleImages_Draw(picture, ot, 4095, left, 0, r, g, b, 0);
    }
    /* The added pictures over the logo and the copyright line (the game's,
     * at 2048 and 2047, and a mod's own) and under the menu's dimming (0),
     * PUSH START BUTTON and the menus (another table, drawn after this
     * one); the first given lowest (a slot draws what was added to it last
     * first). */
    for (i = config->pictures - 1; i >= 0; i--) {
        const TitlePicture *added = &config->picture[i];
        if (!TitleImages_Ready(TITLE_IMAGE_PICTURE(i), &w, &h)) continue;
        if (added->show == TITLE_SHOW_PROMPT && shown_background()) continue;
        if (added->show == TITLE_SHOW_MENU && !shown_background()) continue;
        TitleImages_Draw(TITLE_IMAGE_PICTURE(i), ot, PICTURE_DEPTH, TitleWide_X(&added->wide, added->x, wide()) - w / 2,
                         TitleWide_Y(&added->wide, added->y, wide()) - h / 2, scale(added->tint, 16, 128),
                         scale(added->tint, 8, 128), scale(added->tint, 0, 128), 0);
    }
    for (i = 0; i < TITLE_LAYERS; i++) {
        DisplayObject *object = layer(i);
        const u8 *color;
        if (!object || !TitleImages_Ready(TITLE_IMAGE_LOGO + i, &w, &h) || config->layers[i].hidden) continue;
        if (!(object->flags & DISPLAY_OBJECT_FLAG_RENDERABLE)) continue;
        /* The game's color for it: the tint, and PUSH START BUTTON's pulse. */
        color = (const u8 *)&object->field_0C;
        TitleImages_Draw(TITLE_IMAGE_LOGO + i, ot, 4093,
                         middles[i].x + TitleWide_X(&config->layers[i].wide, config->layers[i].x, wide()) - w / 2,
                         middles[i].y + TitleWide_Y(&config->layers[i].wide, config->layers[i].y, wide()) - h / 2,
                         color[0], color[1], color[2], 0);
    }
}

void TitleScreen_DrawMenu(void)
{
    if (open) TitleMenu_Draw();
}
/* The tower's screen keeps the wall down to its foot, where its boxes are:
 * no dark-to-light shade over it there. */
int TitleScreen_ShowShade(void) { return !in_tower() && background()->shade; }
long TitleScreen_BackgroundColour(void) { return background()->color; }
/* The menu's dimming, and the "fade"'s black with it: both a subtraction
 * over the background, under the menus. */
int TitleScreen_Dim(int level)
{
    int dim = level * TitleConfig_Get()->dim / 0x80;
    return open && fade > dim ? fade : dim;
}

/* --- the text lines ------------------------------------------------------ */

static int line_shown(const TitleLine *line)
{
    if (line->show == TITLE_SHOW_PROMPT) return prompt_showing();
    if (line->show == TITLE_SHOW_MENU) return !prompt_showing();
    return 1;
}

static int visible(void)
{
    return open && TitleConfig_Get()->lines && !(gFade_State.flags & FADE_FLAG_ACTIVE) && !SaveMenu_Active() &&
           !DeckMenu_Active();
}

unsigned TitleScreen_Signature(void)
{
    unsigned signature = 0;
    int x, y, w, h, i;
    if (!visible()) return 0;
    FusionHelper_GetViewport(&x, &y, &w, &h);
    for (i = 0; i < TitleConfig_Get()->lines; i++) signature = signature * 3u + (unsigned)line_shown(&TitleConfig_Get()->line[i]);
    return ((signature * 31u + (unsigned)x) * 31u + (unsigned)y) * 31u + (unsigned)w * 7u + (unsigned)h + 1u;
}

/* Each line at its place in the game's 320 x 240, which grows with the
 * window and stays centred in widescreen, in the port's menu font with a
 * shadow; size 1 is about the game's own 12-pixel letters. */
void TitleScreen_Draw(MenuCanvas *canvas, int *x, int *y, int *w, int *h)
{
    int vx, vy, vw, vh, width_2d, i, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    *x = *y = *w = *h = 0;
    if (!visible()) return;
    FusionHelper_GetViewport(&vx, &vy, &vw, &vh);
    if (vw <= 0 || vh <= 0) return;
    width_2d = wide() ? TITLE_WIDE_WIDTH : 320;
    for (i = 0; i < TitleConfig_Get()->lines; i++) {
        const TitleLine *line = &TitleConfig_Get()->line[i];
        int size = vh / 240 * line->size, left, middle, width;
        if (size < line->size) size = line->size;
        if (!line_shown(line)) continue;
        width = Menu_TextWidthScaled(line->text, size);
        left = vx + vw / 2 + (TitleWide_X(&line->wide, line->x, wide()) - 160) * vw / width_2d;
        if (line->align == TITLE_ALIGN_CENTRE) left -= width / 2;
        else if (line->align == TITLE_ALIGN_RIGHT) left -= width;
        middle = vy + TitleWide_Y(&line->wide, line->y, wide()) * vh / 240;
        Menu_DrawTextScaled(canvas, left + size, middle + size, line->text, 0x000000, size);
        Menu_DrawTextScaled(canvas, left, middle, line->text, line->color, size);
        if (x0 >= x1) { x0 = left; y0 = middle - 10 * size; x1 = left + width + size; y1 = middle + 11 * size; }
        if (left < x0) x0 = left;
        if (middle - 10 * size < y0) y0 = middle - 10 * size;
        if (left + width + size > x1) x1 = left + width + size;
        if (middle + 11 * size > y1) y1 = middle + 11 * size;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > canvas->width) x1 = canvas->width;
    if (y1 > canvas->height) y1 = canvas->height;
    if (x1 <= x0 || y1 <= y0) return;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
}
