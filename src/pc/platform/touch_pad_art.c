/* The on-screen controller's pictures (touch_pad_art.h). */
#include "touch_pad_art.h"
#include "touch_pad.h"
#include "settings.h"
#include "pc/cards/disc_art.h"
#include "pc/text/overlay_text.h"
#include <stdio.h>
#include <string.h>

/* Main_RunBootSequence's package (font_art.c has its layout): 48 sectors of
 * image from VRAM 0x280, 0, then two sectors of palettes loaded at 0x200,
 * 0xF8 (256 x 8), then the text color ramps at 0x280, 0xE8. The buttons
 * are 4-bit on the page at 0x2C0 (page 11) with the palette at 0x200, 0xFC,
 * the same the password screen's OK/END row and the duel's help use; the
 * text font is on the page at 0x280. */
enum {
    BOOT_SECTOR = 0x1690, IMAGE_SECTORS = 48, IMAGE_X = 0x280, IMAGE_Y = 0,
    PALETTE_SECTOR = BOOT_SECTOR + 48, PALETTE_X = 0x200, PALETTE_Y = 0xF8, PALETTE_W = 0x100, PALETTE_H = 8,
    RAMP_SECTOR = BOOT_SECTOR + 50, RAMP_X = 0x280, RAMP_Y = 0xE8, RAMP_W = 16, RAMP_H = 8,
    BUTTON_PAGE = 0x2C0, BUTTON_CLUT_X = 0x200, BUTTON_CLUT_Y = 0xFC, FONT_PAGE = 0x280
};

typedef struct { int u, v, w, h; } Cell;
/* Where each button's picture is on the button page. Left is Right's arrow
 * turned round: the page has no box pointing left. */
static const Cell cells[TOUCH_BUTTONS] = {
    [TOUCH_UP] = {112, 176, 16, 16}, [TOUCH_DOWN] = {112, 160, 16, 16},
    [TOUCH_LEFT] = {96, 160, 16, 16}, [TOUCH_RIGHT] = {96, 160, 16, 16},
    [TOUCH_CROSS] = {16, 128, 16, 16}, [TOUCH_TRIANGLE] = {32, 128, 16, 16},
    [TOUCH_SQUARE] = {48, 128, 16, 16}, [TOUCH_CIRCLE] = {64, 128, 16, 16},
    [TOUCH_L1] = {0, 216, 16, 16}, [TOUCH_R1] = {16, 216, 16, 16},
    [TOUCH_L2] = {32, 224, 16, 16}, [TOUCH_R2] = {48, 216, 16, 16},
    [TOUCH_START] = {96, 128, 32, 16}};
/* SELECT and MENU in the text font (8 x 12 cells of its full-width capitals;
 * glyphs.c's retail_cell). */
static const char select_text[] = "SELECT", menu_text[] = "MENU";
static const Cell letters[] = {{16, 36, 8, 12}, {32, 24, 8, 12}, {88, 24, 8, 12}, {32, 24, 8, 12}, {16, 24, 8, 12},
                               {24, 36, 8, 12}};
static const Cell menu_letters[] = {{96, 24, 8, 12}, {32, 24, 8, 12}, {104, 24, 8, 12}, {32, 36, 8, 12}};

static struct {
    int state; /* 0 unread, 1 read, -1 not readable */
    DiscArt button[TOUCH_BUTTONS], letter[sizeof(select_text) - 1], menu_letter[sizeof(menu_text) - 1];
} art;

static int read_art(void)
{
    int i;
    if (!DiscArt_LoadImage(BOOT_SECTOR, IMAGE_SECTORS, IMAGE_X, IMAGE_Y) ||
        !DiscArt_LoadRect(PALETTE_SECTOR, 0, PALETTE_X, PALETTE_Y, PALETTE_W, PALETTE_H) ||
        !DiscArt_LoadRect(RAMP_SECTOR, 0, RAMP_X, RAMP_Y, RAMP_W, RAMP_H))
        return 0;
    for (i = 0; i < TOUCH_BUTTONS; i++) {
        if (i == TOUCH_SELECT || i >= TOUCH_MENU) continue; /* in the font, or Simple's */
        if (!DiscArt_Cut(&art.button[i], BUTTON_PAGE, 0, 4, cells[i].u, cells[i].v, cells[i].w, cells[i].h,
                         BUTTON_CLUT_X, BUTTON_CLUT_Y))
            return 0;
    }
    for (i = 0; select_text[i]; i++) {
        if (!DiscArt_Cut(&art.letter[i], FONT_PAGE, 0, 4, letters[i].u, letters[i].v, letters[i].w, letters[i].h,
                         RAMP_X, RAMP_Y))
            return 0;
    }
    for (i = 0; menu_text[i]; i++) {
        if (!DiscArt_Cut(&art.menu_letter[i], FONT_PAGE, 0, 4, menu_letters[i].u, menu_letters[i].v,
                         menu_letters[i].w, menu_letters[i].h, RAMP_X, RAMP_Y))
            return 0;
    }
    return 1;
}

void TouchPadArt_Reset(void)
{
    int i;
    for (i = 0; i < TOUCH_BUTTONS; i++) DiscArt_Free(&art.button[i]);
    for (i = 0; select_text[i]; i++) DiscArt_Free(&art.letter[i]);
    for (i = 0; menu_text[i]; i++) DiscArt_Free(&art.menu_letter[i]);
    art.state = 0;
}

static int ready(void)
{
    if (!art.state) {
        if (!read_art()) {
            TouchPadArt_Reset();
            art.state = -1;
            fprintf(stderr, "memories-pc: touch controls: the game's button pictures are not on this disc\n");
        } else {
            art.state = 1;
        }
        DiscArt_Reset(); /* the pictures are cut; the private VRAM is not needed again */
    }
    return art.state > 0;
}

/* The texels window column (or row) i of n covers, of a source of s: the one
 * under its centre when enlarged, all it spans when shrunk (disc_art.c). */
static void span(int i, int n, int s, int *first, int *last)
{
    if (n >= s) {
        *first = *last = (int)(((2 * (long long)i + 1) * s) / (2 * (long long)n));
        return;
    }
    *first = (int)((long long)i * s / n);
    *last = (int)((long long)(i + 1) * s / n) - 1;
    if (*last < *first) *last = *first;
}

/* DiscArt_Draw with an opacity (0-255), and turned round left to right. */
static void draw(MenuCanvas *canvas, const DiscArt *picture, int x, int y, int w, int h, unsigned opacity, int mirror)
{
    int i, j;
    if (!picture->pixels || w <= 0 || h <= 0) return;
    for (j = 0; j < h; j++) {
        int top, bottom;
        if (y + j < 0 || y + j >= canvas->height) continue;
        span(j, h, picture->height, &top, &bottom);
        for (i = 0; i < w; i++) {
            int left, right, sx, sy;
            unsigned r = 0, g = 0, b = 0, opaque = 0, total = 0;
            if (x + i < 0 || x + i >= canvas->width) continue;
            span(mirror ? w - 1 - i : i, w, picture->width, &left, &right);
            for (sy = top; sy <= bottom; sy++) {
                for (sx = left; sx <= right; sx++) {
                    uint32_t texel = picture->pixels[sy * picture->width + sx];
                    total++;
                    if (!(texel >> 24)) continue;
                    opaque++;
                    r += texel >> 16 & 255;
                    g += texel >> 8 & 255;
                    b += texel & 255;
                }
            }
            if (!opaque) continue;
            OverlayText_Blend(canvas, x + i, y + j, (r / opaque) << 16 | (g / opaque) << 8 | b / opaque,
                              opaque * opacity / total);
        }
    }
}

static void grow(int *x0, int *y0, int *x1, int *y1, int x, int y, int w, int h)
{
    if (x < *x0) *x0 = x;
    if (y < *y0) *y0 = y;
    if (x + w > *x1) *x1 = x + w;
    if (y + h > *y1) *y1 = y + h;
}

void TouchPadArt_Draw(MenuCanvas *canvas, int *x, int *y, int *w, int *h)
{
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30), i, bx, by, bw, bh;
    int setting = Settings_Get(SET_TOUCH_OPACITY);
    /* Let go, the picture is see-through over what is behind it, by default
     * 150 of 255 as it always was; held, it is twice as solid (so solid by
     * default) and a little smaller, as if pushed in. */
    unsigned light = setting > 0 ? (unsigned)setting * 255 / 100 : 150, held = light * 2 > 255 ? 255 : light * 2;
    if (TouchPad_Style() == TOUCH_STYLE_SIMPLE) {
        TouchPadArt_DrawSimple(canvas, x, y, w, h);
        return;
    }
    *x = *y = *w = *h = 0;
    if (!TouchPad_Shown() || !ready()) return;
    for (i = 0; i < TOUCH_BUTTONS; i++) {
        int down = TouchPad_Held((TouchButton)i), inset;
        unsigned opacity = down ? held : light;
        if (!TouchPad_Rect((TouchButton)i, &bx, &by, &bw, &bh)) continue; /* Simple's only */
        grow(&x0, &y0, &x1, &y1, bx, by, bw, bh);
        inset = down ? bw / 16 : 0;
        if (i == TOUCH_SELECT || i == TOUCH_MENU) {
            const DiscArt *word = i == TOUCH_SELECT ? art.letter : art.menu_letter;
            int n = (int)(i == TOUCH_SELECT ? sizeof(select_text) : sizeof(menu_text)) - 1, k, letter_w = bw / n;
            for (k = 0; k < n; k++)
                draw(canvas, &word[k], bx + k * letter_w + inset / 2, by + inset / 2, letter_w - inset,
                     bh - inset, opacity, 0);
        } else {
            draw(canvas, &art.button[i], bx + inset, by + inset * bh / bw, bw - 2 * inset, bh - 2 * inset * bh / bw,
                 opacity, i == TOUCH_LEFT);
        }
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > canvas->width) x1 = canvas->width;
    if (y1 > canvas->height) y1 = canvas->height;
    if (x1 <= x0 || y1 <= y0) return;
    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
}
