/* The on-screen controller's layout, fingers and bits (touch_pad.h). */
#include "touch_pad.h"
#include <math.h>
#include <string.h>

#define FINGERS 10

typedef enum { ZONE_NONE, ZONE_DPAD, ZONE_FACE, ZONE_BUTTON } Zone;
typedef struct {
    int used;
    uint64_t id;
    Zone zone;
    TouchButton button; /* ZONE_BUTTON's */
    uint16_t bits;
    int x, y; /* where it is now */
} Finger;

/* PS1 pad bits, as Platform_Pad reports them. */
static const uint16_t bits_of[TOUCH_BUTTONS] = {
    [TOUCH_UP] = 0x0010, [TOUCH_RIGHT] = 0x0020, [TOUCH_DOWN] = 0x0040, [TOUCH_LEFT] = 0x0080,
    [TOUCH_TRIANGLE] = 0x1000, [TOUCH_CIRCLE] = 0x2000, [TOUCH_CROSS] = 0x4000, [TOUCH_SQUARE] = 0x8000,
    [TOUCH_L2] = 0x0100, [TOUCH_R2] = 0x0200, [TOUCH_L1] = 0x0400, [TOUCH_R1] = 0x0800,
    [TOUCH_SELECT] = 0x0001, [TOUCH_START] = 0x0008};

static int mode, touched, blocked, width, height, top;
static float density; /* window pixels per dp, 0 unknown (TouchPad_SetDensity) */
static int rects[TOUCH_BUTTONS][4];
static int unit, dpad_x, dpad_y, face_x, face_y;
static Finger fingers[FINGERS];
static uint16_t held, tapped, presses;
/* The top row (TOUCH_MENU on): held now, and taps not yet taken. */
#define TOP_ROW(button) ((button) >= TOUCH_MENU)
static int row_down[TOUCH_BUTTONS], row_taps[TOUCH_BUTTONS];

uint16_t TouchPad_Bit(TouchButton button) { return button < TOUCH_BUTTONS ? bits_of[button] : 0; }

int TouchPad_Shown(void)
{
    return width > 0 && height > 0 && !blocked && (mode == TOUCH_PAD_SHOW || (mode == TOUCH_PAD_AUTO && touched));
}

static void place(TouchButton button, int x, int y, int w, int h)
{
    rects[button][0] = x;
    rects[button][1] = y;
    rects[button][2] = w;
    rects[button][3] = h;
}

/* A button is about a thumb's width: 13% of the window's short side, and
 * where the density is known 48 to 80 dp of it (the smallest target a
 * finger hits, and no bigger than one, so a tablet's buttons stay at the
 * edges), but never above 16% of the short side, nor so big that the
 * columns reach the D-pad's ring below them (6.4 buttons tall in all).
 *
 * Laid out as a handheld's, the picture's middle left clear: L2, L1 and
 * SELECT down the top left corner and R2, R1 and START down the top right,
 * each a frame 1.6 buttons wide; the D-pad in the bottom left corner and
 * the face buttons in the bottom right, each a ring 3.2 buttons across with
 * its buttons a button from its middle; MENU, STATES and SPEED in a row in
 * the middle of the top edge, about half a button tall. */
#define COLUMN_W(s) ((s) * 8 / 5)
#define TALL_H(s) ((s) * 4 / 5)
#define SHORT_H(s) ((s) * 5 / 8)
#define GAP(s) ((s) * 22 / 100)
#define RING(s) ((s) * 8 / 5)
#define ROW_H(s) ((s) * 11 / 20)
static void lay_out(void)
{
    int shorter = width < height ? width : height, s = shorter * 13 / 100, m, y, row_x, row_w[3], i;
    if (density > 0) {
        int least = (int)(48 * density + 0.5f), most = (int)(80 * density + 0.5f);
        if (s < least) s = least;
        if (s > most) s = most;
        if (s > shorter * 16 / 100) s = shorter * 16 / 100;
    }
    if (s * 64 / 10 > height - top) s = (height - top) * 10 / 64;
    if (s < 1) s = 1;
    m = s * 35 / 100;
    unit = s;
    y = top + m / 2;
    place(TOUCH_L2, m, y, COLUMN_W(s), TALL_H(s));
    place(TOUCH_R2, width - m - COLUMN_W(s), y, COLUMN_W(s), TALL_H(s));
    y += TALL_H(s) + GAP(s);
    place(TOUCH_L1, m, y, COLUMN_W(s), TALL_H(s));
    place(TOUCH_R1, width - m - COLUMN_W(s), y, COLUMN_W(s), TALL_H(s));
    y += TALL_H(s) + GAP(s);
    place(TOUCH_SELECT, m, y, COLUMN_W(s), SHORT_H(s));
    place(TOUCH_START, width - m - COLUMN_W(s), y, COLUMN_W(s), SHORT_H(s));
    dpad_x = m + RING(s);
    face_x = width - m - RING(s);
    dpad_y = face_y = height - m / 2 - RING(s);
    place(TOUCH_UP, dpad_x - s / 2, dpad_y - s * 3 / 2, s, s);
    place(TOUCH_DOWN, dpad_x - s / 2, dpad_y + s / 2, s, s);
    place(TOUCH_LEFT, dpad_x - s * 3 / 2, dpad_y - s / 2, s, s);
    place(TOUCH_RIGHT, dpad_x + s / 2, dpad_y - s / 2, s, s);
    place(TOUCH_TRIANGLE, face_x - s / 2, face_y - s * 3 / 2, s, s);
    place(TOUCH_CROSS, face_x - s / 2, face_y + s / 2, s, s);
    place(TOUCH_SQUARE, face_x - s * 3 / 2, face_y - s / 2, s, s);
    place(TOUCH_CIRCLE, face_x + s / 2, face_y - s / 2, s, s);
    /* MENU and STATES hold an icon, SPEED an icon and "x4". */
    row_w[0] = row_w[1] = s * 19 / 20;
    row_w[2] = s * 6 / 5;
    row_x = width / 2 - (row_w[0] + row_w[1] + row_w[2] + 2 * (s / 10)) / 2;
    for (i = 0; i < 3; i++) {
        place((TouchButton)(TOUCH_MENU + i), row_x, top, row_w[i], ROW_H(s));
        row_x += row_w[i] + s / 10;
    }
}

int TouchPad_SetDensity(float pixels_per_dp)
{
    if (pixels_per_dp == density) return 0;
    density = pixels_per_dp;
    lay_out();
    return TouchPad_Shown();
}

int TouchPad_Layout(int window_w, int window_h, int bar)
{
    if (window_w == width && window_h == height && bar == top) return 0;
    width = window_w;
    height = window_h;
    top = bar;
    lay_out();
    return TouchPad_Shown();
}

int TouchPad_Rect(TouchButton button, int *x, int *y, int *w, int *h)
{
    if (!TouchPad_Shown() || button >= TOUCH_BUTTONS) return 0;
    *x = rects[button][0];
    *y = rects[button][1];
    *w = rects[button][2];
    *h = rects[button][3];
    return 1;
}

int TouchPad_Held(TouchButton button)
{
    if (button >= TOUCH_BUTTONS) return 0;
    if (TOP_ROW(button)) return row_down[button];
    return (held & bits_of[button]) != 0;
}

void TouchPad_Centre(int face, int *x, int *y)
{
    *x = face ? face_x : dpad_x;
    *y = face ? face_y : dpad_y;
}

int TouchPad_Unit(void) { return unit; }

static long distance2(int x, int y, int cx, int cy) { return (long)(x - cx) * (x - cx) + (long)(y - cy) * (y - cy); }

/* A button's rectangle, a little larger to aim at; the short ones (the
 * column's, the top row's) as tall as a button to tap. */
static int in_button(TouchButton button, int x, int y)
{
    int grow = unit * 15 / 100, top = rects[button][1], h = rects[button][3];
    if (h < unit) {
        top -= (unit - h) / 2;
        h = unit;
    }
    return x >= rects[button][0] - grow && y >= top - grow && x < rects[button][0] + rects[button][2] + grow &&
           y < top + h + grow;
}

/* The D-pad's and the face buttons' rings first, then the button whose
 * middle is nearest of those whose larger rectangle takes the touch (a
 * column's stacked buttons reach into each other's). */
static Zone zone_at(int x, int y, TouchButton *button)
{
    TouchButton b;
    long reach = (long)unit * unit * 4, best = -1; /* two buttons from the middle */
    if (distance2(x, y, dpad_x, dpad_y) <= reach) return ZONE_DPAD;
    if (distance2(x, y, face_x, face_y) <= reach) return ZONE_FACE;
    for (b = TOUCH_L1; b < TOUCH_BUTTONS; b++) {
        long d;
        if (!in_button(b, x, y)) continue;
        d = distance2(x, y, rects[b][0] + rects[b][2] / 2, rects[b][1] + rects[b][3] / 2);
        if (best < 0 || d < best) {
            best = d;
            *button = b;
        }
    }
    return best < 0 ? ZONE_NONE : ZONE_BUTTON;
}

/* What a finger of its zone presses at x, y. The D-pad goes by the angle
 * from its middle, so the diagonals between two arrows press both; the
 * face buttons by the nearest one. */
static uint16_t press_at(const Finger *finger, int x, int y)
{
    int dx = x - dpad_x, dy = y - dpad_y, i, nearest = -1;
    long best = 0;
    double length, c, s;
    static const TouchButton face[] = {TOUCH_TRIANGLE, TOUCH_CIRCLE, TOUCH_CROSS, TOUCH_SQUARE};
    uint16_t out = 0;
    switch (finger->zone) {
    case ZONE_DPAD:
        length = sqrt((double)dx * dx + (double)dy * dy);
        if (length < unit * 0.3) return 0; /* the middle presses nothing */
        c = dx / length;
        s = dy / length;
        if (c > 0.383) out |= bits_of[TOUCH_RIGHT]; /* 67.5 degrees each way */
        if (c < -0.383) out |= bits_of[TOUCH_LEFT];
        if (s > 0.383) out |= bits_of[TOUCH_DOWN];
        if (s < -0.383) out |= bits_of[TOUCH_UP];
        return out;
    case ZONE_FACE:
        if (distance2(x, y, face_x, face_y) > (long)unit * unit * 9) return 0; /* slid well away */
        for (i = 0; i < 4; i++) {
            const int *r = rects[face[i]];
            long d = distance2(x, y, r[0] + r[2] / 2, r[1] + r[3] / 2);
            if (nearest < 0 || d < best) {
                nearest = i;
                best = d;
            }
        }
        return bits_of[face[nearest]];
    case ZONE_BUTTON:
        return in_button(finger->button, x, y) ? bits_of[finger->button] : 0;
    default:
        return 0;
    }
}

static int refresh(void)
{
    uint16_t now = 0;
    int i, down[TOUCH_BUTTONS] = {0}, changed = 0;
    for (i = 0; i < FINGERS; i++) {
        if (fingers[i].used) now |= fingers[i].bits;
        if (fingers[i].used && fingers[i].zone == ZONE_BUTTON && TOP_ROW(fingers[i].button))
            down[fingers[i].button] |= in_button(fingers[i].button, fingers[i].x, fingers[i].y);
    }
    presses |= now & ~held;
    tapped |= now & ~held;
    for (i = TOUCH_MENU; i < TOUCH_BUTTONS; i++) {
        changed |= down[i] != row_down[i];
        row_down[i] = down[i];
    }
    if (now == held && !changed) return 0;
    held = now;
    return 1;
}

static void release_all(void)
{
    memset(fingers, 0, sizeof(fingers));
    refresh();
}

int TouchPad_SetMode(int wanted)
{
    int was = TouchPad_Shown();
    if (wanted == mode) return 0;
    mode = wanted;
    if (!TouchPad_Shown()) release_all();
    return was != TouchPad_Shown();
}

int TouchPad_Block(int wanted)
{
    int was = TouchPad_Shown();
    wanted = !!wanted;
    if (wanted == blocked) return 0;
    blocked = wanted;
    if (blocked) release_all();
    return was != TouchPad_Shown();
}

static int take(TouchButton button)
{
    int taps = row_taps[button];
    row_taps[button] = 0;
    return taps != 0;
}

int TouchPad_TakeMenu(void) { return take(TOUCH_MENU); }
int TouchPad_TakeStates(void) { return take(TOUCH_STATES); }
int TouchPad_TakeSpeed(void) { return take(TOUCH_SPEED); }

/* The columns' and the rings' inner edges; the top row, in the middle, is
 * left to TouchPad_TopRowBottom. */
int TouchPad_FreeSpan(int *left, int *right)
{
    int i, grow = unit * 15 / 100;
    if (!TouchPad_Shown()) return 0;
    *left = dpad_x + 2 * unit; /* the D-pad's and the face buttons' reach (zone_at) */
    *right = face_x - 2 * unit;
    for (i = 0; i < TOUCH_MENU; i++) {
        const int *r = rects[i];
        if (r[0] + r[2] / 2 < width / 2) {
            if (r[0] + r[2] + grow > *left) *left = r[0] + r[2] + grow;
        } else if (r[0] - grow < *right) {
            *right = r[0] - grow;
        }
    }
    return 1;
}

int TouchPad_TopRowBottom(void)
{
    int grow = unit * 15 / 100, h = rects[TOUCH_MENU][3];
    if (!TouchPad_Shown()) return 0;
    return rects[TOUCH_MENU][1] + h + (h < unit ? (unit - h) / 2 : 0) + grow; /* in_button's reach */
}

int TouchPad_OtherInput(void)
{
    if (mode != TOUCH_PAD_AUTO || !touched) return 0;
    touched = 0;
    release_all();
    return 1;
}

int TouchPad_Covers(int x, int y)
{
    TouchButton button;
    return TouchPad_Shown() && zone_at(x, y, &button) != ZONE_NONE;
}

int TouchPad_Finger(int kind, uint64_t id, int x, int y)
{
    Finger *finger = NULL;
    int i;
    for (i = 0; i < FINGERS; i++) {
        if (fingers[i].used && fingers[i].id == id) finger = &fingers[i];
    }
    if (kind == TOUCH_FINGER_DOWN) {
        TouchButton button = TOUCH_UP;
        Zone zone;
        if (mode == TOUCH_PAD_HIDE || blocked) return 0;
        if (!TouchPad_Shown()) {
            touched = 1; /* the first touch shows the pad; it presses nothing */
            return 1;
        }
        zone = zone_at(x, y, &button);
        if (zone == ZONE_NONE || finger) return 0;
        for (i = 0; i < FINGERS && !finger; i++) {
            if (!fingers[i].used) finger = &fingers[i];
        }
        if (!finger) return 0;
        finger->used = 1;
        finger->id = id;
        finger->zone = zone;
        finger->button = button;
        finger->x = x;
        finger->y = y;
        finger->bits = press_at(finger, x, y);
        if (zone == ZONE_BUTTON && TOP_ROW(button)) row_taps[button]++;
        return refresh();
    }
    if (!finger) return 0;
    if (kind == TOUCH_FINGER_MOVE) {
        finger->x = x;
        finger->y = y;
        finger->bits = press_at(finger, x, y);
    } else {
        memset(finger, 0, sizeof(*finger));
    }
    return refresh();
}

uint16_t TouchPad_Update(void)
{
    uint16_t out = held | tapped;
    tapped = 0;
    return out;
}

uint16_t TouchPad_TakePresses(void)
{
    uint16_t out = presses;
    presses = 0;
    return out;
}
