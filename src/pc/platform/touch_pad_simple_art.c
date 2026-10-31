/* The on-screen controller's Simple pictures (touch_pad_art.h). */
#include "touch_pad_art.h"
#include "touch_pad.h"
#include "settings.h"
#include "pc/text/overlay_text.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The words in the frames, in the game text's font (overlay_text.h). */
static const char *const labels[TOUCH_BUTTONS] = {
    [TOUCH_L1] = "L1", [TOUCH_L2] = "L2", [TOUCH_R1] = "R1", [TOUCH_R2] = "R2",
    [TOUCH_SELECT] = "SELECT", [TOUCH_START] = "START"};

/* One grey scale: dark fills, a light edge and light marks and words, all
 * of them drawn whole into a layer and laid over the picture at the
 * opacity. The D-pad's arrows are dark discs without an edge in its ring. */
#define FILL 0x2A2B30u
#define FILL_ARROW 0x3A3B41u
#define FILL_HELD 0x6E7078u
#define EDGE 0xC8C9CEu
#define INK 0xE6E7EBu

/* The layer each part is drawn into before it is laid over the canvas, the
 * window's size. */
static struct {
    uint32_t *pixels;
    int width, height;
} layer;

/* Shapes, by their distance from each pixel's middle (negative inside), with
 * a pixel of soft edge. */
static float rounded_distance(float px, float py, float x, float y, float w, float h, float r)
{
    float qx = fabsf(px - (x + w / 2)) - (w / 2 - r), qy = fabsf(py - (y + h / 2)) - (h / 2 - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, inside = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (inside < 0 ? inside : 0) - r;
}

static float coverage(float d)
{
    return d <= -0.5f ? 1.0f : d >= 0.5f ? 0.0f : 0.5f - d;
}

/* A rounded rectangle (kind 0) or a disc (kind 1, w across): its inside in
 * `fill` (none when 0) and an edge `edge` pixels thick in `edge_color`. */
static void shape(MenuCanvas *into, int kind, float x, float y, float w, float h, float r, float edge,
                  uint32_t fill, uint32_t edge_color)
{
    int i, j, x0 = (int)floorf(x) - 1, y0 = (int)floorf(y) - 1, x1 = (int)ceilf(x + w) + 1,
           y1 = (int)ceilf(y + h) + 1;
    for (j = y0; j < y1; j++) {
        for (i = x0; i < x1; i++) {
            float px = i + 0.5f, py = j + 0.5f, d, outer, inner;
            if (kind == 0) {
                d = rounded_distance(px, py, x, y, w, h, r);
            } else {
                float dx = px - (x + w / 2), dy = py - (y + h / 2);
                d = sqrtf(dx * dx + dy * dy) - w / 2;
            }
            outer = coverage(d);
            if (outer <= 0) continue;
            inner = edge > 0 ? coverage(d + edge) : outer;
            if (inner > 0 && fill) OverlayText_Blend(into, i, j, fill, (unsigned)(inner * 255 + 0.5f));
            if (outer - inner > 0)
                OverlayText_Blend(into, i, j, edge_color, (unsigned)((outer - inner) * 255 + 0.5f));
        }
    }
}

/* A stroke from a to b, `width` thick with round ends. */
static void stroke(MenuCanvas *into, float ax, float ay, float bx, float by, float width, uint32_t color)
{
    int i, j;
    float dx = bx - ax, dy = by - ay, length2 = dx * dx + dy * dy, half = width / 2;
    for (j = (int)floorf((ay < by ? ay : by) - half) - 1; j <= (int)ceilf((ay > by ? ay : by) + half) + 1; j++) {
        for (i = (int)floorf((ax < bx ? ax : bx) - half) - 1; i <= (int)ceilf((ax > bx ? ax : bx) + half) + 1; i++) {
            float px = i + 0.5f - ax, py = j + 0.5f - ay, ex, ey, c;
            float t = length2 > 0 ? (px * dx + py * dy) / length2 : 0;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            ex = px - t * dx;
            ey = py - t * dy;
            c = coverage(sqrtf(ex * ex + ey * ey) - half);
            if (c > 0) OverlayText_Blend(into, i, j, color, (unsigned)(c * 255 + 0.5f));
        }
    }
}

/* A filled polygon (the arrows, STATES' and SPEED's triangles), sampled 4 x 4
 * a pixel; the points go round either way. */
static void polygon(MenuCanvas *into, const float *xy, int n, uint32_t color)
{
    float x0 = xy[0], y0 = xy[1], x1 = xy[0], y1 = xy[1];
    int i, j, k, s;
    for (k = 1; k < n; k++) {
        if (xy[2 * k] < x0) x0 = xy[2 * k];
        if (xy[2 * k] > x1) x1 = xy[2 * k];
        if (xy[2 * k + 1] < y0) y0 = xy[2 * k + 1];
        if (xy[2 * k + 1] > y1) y1 = xy[2 * k + 1];
    }
    for (j = (int)floorf(y0); j <= (int)ceilf(y1); j++) {
        for (i = (int)floorf(x0); i <= (int)ceilf(x1); i++) {
            int hits = 0;
            for (s = 0; s < 16; s++) {
                float px = i + (s % 4 + 0.5f) / 4, py = j + (s / 4 + 0.5f) / 4;
                int inside = 1, sign = 0;
                for (k = 0; k < n && inside; k++) {
                    const float *a = xy + 2 * k, *b = xy + 2 * ((k + 1) % n);
                    float cross = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
                    int side = cross > 0 ? 1 : cross < 0 ? -1 : 0;
                    if (side && sign && side != sign) inside = 0;
                    if (side) sign = side;
                }
                hits += inside;
            }
            if (hits) OverlayText_Blend(into, i, j, color, (unsigned)(hits * 255 / 16));
        }
    }
}

static void triangle(MenuCanvas *into, float ax, float ay, float bx, float by, float cx, float cy, uint32_t color)
{
    const float xy[] = {ax, ay, bx, by, cx, cy};
    polygon(into, xy, 3, color);
}

/* MENU's cog: eight teeth round a ring, sampled 4 x 4 a pixel. */
static void cog(MenuCanvas *into, float cx, float cy, float radius, uint32_t color)
{
    int i, j, s;
    const float body = radius * 0.72f, hole = radius * 0.32f;
    for (j = (int)floorf(cy - radius); j <= (int)ceilf(cy + radius); j++) {
        for (i = (int)floorf(cx - radius); i <= (int)ceilf(cx + radius); i++) {
            int hits = 0;
            for (s = 0; s < 16; s++) {
                float dx = i + (s % 4 + 0.5f) / 4 - cx, dy = j + (s / 4 + 0.5f) / 4 - cy;
                float d = sqrtf(dx * dx + dy * dy), turn = atan2f(dy, dx) * 8 / 6.2831853f + 8;
                float outer = turn - floorf(turn) < 0.5f ? radius : body;
                hits += d < outer && d > hole;
            }
            if (hits) OverlayText_Blend(into, i, j, color, (unsigned)(hits * 255 / 16));
        }
    }
}

static void words(MenuCanvas *into, int cx, int cy, const char *text, int pixels)
{
    int width = OverlayText_Width(text, pixels);
    OverlayText_Draw(into, cx - width / 2, cy, cx + width / 2 + pixels, text, pixels, INK);
}

/* A button's mark, `size` across round cx, cy: a solid arrow, or the face
 * buttons' outlined triangle, circle, cross and square. */
static void mark(MenuCanvas *into, TouchButton button, float cx, float cy, float size)
{
    float h = size / 2, t = size / 11 > 1.2f ? size / 11 : 1.2f;
    switch (button) {
    case TOUCH_UP: triangle(into, cx, cy - h, cx + h * 1.1f, cy + h * 0.7f, cx - h * 1.1f, cy + h * 0.7f, INK); break;
    case TOUCH_DOWN: triangle(into, cx, cy + h, cx - h * 1.1f, cy - h * 0.7f, cx + h * 1.1f, cy - h * 0.7f, INK); break;
    case TOUCH_LEFT: triangle(into, cx - h, cy, cx + h * 0.7f, cy - h * 1.1f, cx + h * 0.7f, cy + h * 1.1f, INK); break;
    case TOUCH_RIGHT:
        triangle(into, cx + h, cy, cx - h * 0.7f, cy + h * 1.1f, cx - h * 0.7f, cy - h * 1.1f, INK);
        break;
    case TOUCH_TRIANGLE:
        stroke(into, cx, cy - h, cx + h * 1.05f, cy + h * 0.75f, t, INK);
        stroke(into, cx + h * 1.05f, cy + h * 0.75f, cx - h * 1.05f, cy + h * 0.75f, t, INK);
        stroke(into, cx - h * 1.05f, cy + h * 0.75f, cx, cy - h, t, INK);
        break;
    case TOUCH_CIRCLE: shape(into, 1, cx - h, cy - h, size, size, 0, t, 0, INK); break;
    case TOUCH_SQUARE:
        shape(into, 0, cx - h * 0.85f, cy - h * 0.85f, size * 0.85f, size * 0.85f, t / 2, t, 0, INK);
        break;
    case TOUCH_CROSS:
        stroke(into, cx - h * 0.85f, cy - h * 0.85f, cx + h * 0.85f, cy + h * 0.85f, t * 1.15f, INK);
        stroke(into, cx + h * 0.85f, cy - h * 0.85f, cx - h * 0.85f, cy + h * 0.85f, t * 1.15f, INK);
        break;
    default: break;
    }
}

/* SPEED's word: the speed as a multiple of the console's (x2, x1.5), MAX
 * uncapped. */
static void speed_words(char *out, size_t size)
{
    int speed = Settings_Get(SET_SPEED);
    if (speed < 0) snprintf(out, size, "MAX");
    else if (speed % 100 == 0) snprintf(out, size, "\xC3\x97%d", speed / 100);
    else snprintf(out, size, "\xC3\x97%d.%d", speed / 100, speed % 100 / 10);
}

/* One part, drawn whole into the layer, then laid over the canvas at its
 * opacity, so its edge, fill and words fade together. */
static void begin(int x0, int y0, int x1, int y1)
{
    int y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > layer.width) x1 = layer.width;
    if (y1 > layer.height) y1 = layer.height;
    if (x1 <= x0) return;
    for (y = y0; y < y1; y++) memset(layer.pixels + (size_t)y * layer.width + x0, 0, (size_t)(x1 - x0) * 4);
}

static void lay(MenuCanvas *canvas, int x0, int y0, int x1, int y1, unsigned opacity)
{
    int x, y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > layer.width) x1 = layer.width;
    if (y1 > layer.height) y1 = layer.height;
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            uint32_t pixel = layer.pixels[(size_t)y * layer.width + x];
            if (pixel >> 24) OverlayText_Blend(canvas, x, y, pixel & 0xFFFFFFu, (pixel >> 24) * opacity / 255);
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

void TouchPadArt_DrawSimple(MenuCanvas *canvas, int *x, int *y, int *w, int *h)
{
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30), i, s, cx, cy;
    int opacity = Settings_Get(SET_TOUCH_OPACITY);
    unsigned light, held;
    MenuCanvas into;
    *x = *y = *w = *h = 0;
    if (!TouchPad_Shown()) return;
    if (layer.width != canvas->width || layer.height != canvas->height) {
        uint32_t *pixels = realloc(layer.pixels, (size_t)canvas->width * canvas->height * 4);
        if (!pixels) return;
        layer.pixels = pixels;
        layer.width = canvas->width;
        layer.height = canvas->height;
    }
    into.pixels = layer.pixels;
    into.stride = layer.width;
    into.width = layer.width;
    into.height = layer.height;
    into.alpha = 1;
    light = (unsigned)(opacity > 0 ? opacity : 25) * 255 / 100; /* the style's own: 25% */
    held = light * 2 > 255 ? 255 : light * 2;
    s = TouchPad_Unit();

    /* The D-pad's ring, under its arrows: all it takes a touch in. */
    TouchPad_Centre(0, &cx, &cy);
    {
        int r = TouchPad_RingRadius();
        begin(cx - r - 2, cy - r - 2, cx + r + 2, cy + r + 2);
        shape(&into, 1, (float)(cx - r), (float)(cy - r), (float)(2 * r), (float)(2 * r), 0,
              s / 24.0f > 1.5f ? s / 24.0f : 1.5f, 0, EDGE);
        lay(canvas, cx - r - 2, cy - r - 2, cx + r + 2, cy + r + 2, light);
        grow(&x0, &y0, &x1, &y1, cx - r, cy - r, 2 * r, 2 * r);
    }
    for (i = 0; i < TOUCH_BUTTONS; i++) {
        TouchButton b = (TouchButton)i;
        int bx, by, bw, bh, down = TouchPad_Held(b), ex0, ey0, ex1, ey1;
        float edge;
        if (!TouchPad_Rect(b, &bx, &by, &bw, &bh)) continue;
        edge = bh / 26.0f > 1.5f ? bh / 26.0f : 1.5f;
        if (b < TOUCH_L1) {
            /* Discs: an arrow's a little inside its square, without an edge;
             * a face button's a little beyond it, with one. */
            int arrow = b <= TOUCH_RIGHT;
            float r = arrow ? bw * 0.47f : bw * 0.58f, mx = bx + bw / 2.0f, my = by + bh / 2.0f;
            ex0 = (int)(mx - r) - 2;
            ey0 = (int)(my - r) - 2;
            ex1 = (int)(mx + r) + 3;
            ey1 = (int)(my + r) + 3;
            begin(ex0, ey0, ex1, ey1);
            shape(&into, 1, mx - r, my - r, 2 * r, 2 * r, 0, arrow ? 0 : edge,
                  down ? FILL_HELD : arrow ? FILL_ARROW : FILL, EDGE);
            mark(&into, b, mx, my, arrow ? bw * 0.36f : bw * 0.5f);
        } else {
            /* Frames: the column's words, the top row's marks. */
            int mx = bx + bw / 2, my = by + bh / 2;
            uint32_t fill = down ? FILL_HELD : FILL;
            ex0 = bx - 2;
            ey0 = by - 2;
            ex1 = bx + bw + 3;
            ey1 = by + bh + 3;
            begin(ex0, ey0, ex1, ey1);
            shape(&into, 0, (float)bx, (float)by, (float)bw, (float)bh, bh * 0.3f, edge, fill, EDGE);
            if (labels[b]) {
                words(&into, mx, my, labels[b], bh * (b == TOUCH_SELECT || b == TOUCH_START ? 40 : 48) / 100);
            } else if (b == TOUCH_MENU) {
                cog(&into, (float)mx, (float)my, bh * 0.36f, INK);
                shape(&into, 1, mx - bh * 0.13f, my - bh * 0.13f, bh * 0.26f, bh * 0.26f, 0, 0, fill, EDGE);
            } else if (b == TOUCH_STATES) {
                triangle(&into, mx - bh * 0.3f, my - bh * 0.17f, mx + bh * 0.3f, my - bh * 0.17f, (float)mx,
                         my + bh * 0.25f, INK);
            } else {
                char text[16];
                float l = bx + bw * 0.12f, t = bh * 0.22f;
                triangle(&into, l, my - t, l + t * 1.3f, (float)my, l, my + t, INK);
                triangle(&into, l + t * 1.2f, my - t, l + t * 2.5f, (float)my, l + t * 1.2f, my + t, INK);
                speed_words(text, sizeof(text));
                words(&into, (int)(l + t * 2.5f + (bx + bw - (l + t * 2.5f)) / 2), my, text, bh * 46 / 100);
            }
        }
        lay(canvas, ex0, ey0, ex1, ey1, down ? held : light);
        grow(&x0, &y0, &x1, &y1, ex0, ey0, ex1 - ex0, ey1 - ey0);
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
