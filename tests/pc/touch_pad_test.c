/* The on-screen controller's logic (src/pc/platform/touch_pad.c): when it
 * shows, what a finger presses where, and that a tap shorter than a pump
 * still counts once; in the Game style (the default) and in Simple, and
 * that each button either lays out takes the touches on it. */
#include "pc/platform/touch_pad.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void centre(TouchButton button, int *x, int *y)
{
    int bx, by, bw, bh;
    assert(TouchPad_Rect(button, &bx, &by, &bw, &bh));
    *x = bx + bw / 2;
    *y = by + bh / 2;
}

static void take_all(void)
{
    TouchPad_TakeMenu();
    TouchPad_TakeStates();
    TouchPad_TakeSpeed();
}

/* Every button laid out takes the touches across what is drawn of it (a
 * grid inside its rectangle, a tenth in from each side): its bit, an
 * arrow's at least (between two arrows the D-pad presses both), or, for
 * one without a bit, held. The D-pad and the face buttons take nothing
 * beyond their reach. */
static void check_hits(const char *style, int window_w, int window_h, int top, float density)
{
    int b, i, j, cx, cy, reach;
    TouchPad_SetDensity(density);
    TouchPad_Layout(window_w, window_h, top);
    for (b = 0; b < TOUCH_BUTTONS; b++) {
        int bx, by, bw, bh;
        if (!TouchPad_Rect((TouchButton)b, &bx, &by, &bw, &bh)) continue;
        for (j = 1; j <= 9; j += 2) {
            for (i = 1; i <= 9; i += 2) {
                int x = bx + bw * i / 10, y = by + bh * j / 10, ok;
                uint16_t bits, bit = TouchPad_Bit((TouchButton)b);
                TouchPad_Finger(TOUCH_FINGER_DOWN, 20, x, y);
                bits = TouchPad_Update();
                if (b <= TOUCH_RIGHT) ok = (bits & bit) && (i != 5 || j != 5 || bits == bit);
                else if (bit) ok = bits == bit;
                else ok = !bits && TouchPad_Held((TouchButton)b);
                if (!ok) {
                    fprintf(stderr, "%s %dx%d top %d at %.2f dp: button %d (%d,%d %dx%d) at %d,%d pressed %04x\n",
                            style, window_w, window_h, top, density, b, bx, by, bw, bh, x, y, bits);
                    abort();
                }
                TouchPad_Finger(TOUCH_FINGER_UP, 20, x, y);
                TouchPad_Update();
                take_all();
            }
        }
    }
    reach = TouchPad_RingRadius();
    TouchPad_Centre(0, &cx, &cy);
    assert(TouchPad_Covers(cx + reach, cy) && !TouchPad_Covers(cx + reach + 2, cy));
    TouchPad_Centre(1, &cx, &cy);
    assert(TouchPad_Covers(cx - reach, cy) && !TouchPad_Covers(cx - reach - 2, cy));
}

int main(void)
{
    int x, y, ux, uy, dx, dy;
    /* Automatic: hidden, and nothing is the pad's, until a touch. */
    TouchPad_SetMode(TOUCH_PAD_AUTO);
    TouchPad_Layout(2280, 1080, 0);
    assert(TouchPad_Style() == TOUCH_STYLE_GAME);
    assert(!TouchPad_Shown() && !TouchPad_Covers(100, 600) && !TouchPad_Rect(TOUCH_CROSS, &x, &y, &x, &y));
    assert(TouchPad_Finger(TOUCH_FINGER_DOWN, 1, 100, 600) && TouchPad_Shown());
    assert(TouchPad_Update() == 0); /* the touch that showed it pressed nothing */
    TouchPad_Finger(TOUCH_FINGER_UP, 1, 100, 600);

    /* A D-pad arrow, held across updates and let go. */
    centre(TOUCH_UP, &ux, &uy);
    assert(TouchPad_Covers(ux, uy) && !TouchPad_Covers(1140, 540));
    assert(TouchPad_Finger(TOUCH_FINGER_DOWN, 2, ux, uy));
    assert(TouchPad_Update() == TouchPad_Bit(TOUCH_UP) && TouchPad_Update() == TouchPad_Bit(TOUCH_UP));
    assert(TouchPad_Held(TOUCH_UP) && TouchPad_TakePresses() == TouchPad_Bit(TOUCH_UP) && !TouchPad_TakePresses());
    /* Sliding to the right arrow's side of the middle turns it into Right. */
    centre(TOUCH_RIGHT, &x, &y);
    TouchPad_Finger(TOUCH_FINGER_MOVE, 2, x, y);
    assert(TouchPad_Update() == TouchPad_Bit(TOUCH_RIGHT));
    /* Between Up and Right: both. */
    centre(TOUCH_DOWN, &dx, &dy);
    TouchPad_Finger(TOUCH_FINGER_MOVE, 2, x, uy);
    assert(TouchPad_Update() == (TouchPad_Bit(TOUCH_UP) | TouchPad_Bit(TOUCH_RIGHT)));
    TouchPad_Finger(TOUCH_FINGER_UP, 2, x, uy);
    assert(TouchPad_Update() == 0);

    /* A tap down and up between two updates is seen by one of them. */
    centre(TOUCH_CROSS, &x, &y);
    TouchPad_Finger(TOUCH_FINGER_DOWN, 3, x, y);
    TouchPad_Finger(TOUCH_FINGER_UP, 3, x, y);
    assert(TouchPad_Update() == TouchPad_Bit(TOUCH_CROSS) && TouchPad_Update() == 0);
    /* A touch the system cancels lets go, as a lift does. */
    TouchPad_Finger(TOUCH_FINGER_DOWN, 3, x, y);
    assert(TouchPad_Update() == TouchPad_Bit(TOUCH_CROSS));
    assert(TouchPad_Finger(TOUCH_FINGER_CANCEL, 3, x, y) && TouchPad_Update() == 0);

    /* Two fingers: the D-pad and a face button together; a face finger
     * slides to the nearest button. */
    TouchPad_Finger(TOUCH_FINGER_DOWN, 4, dx, dy);
    TouchPad_Finger(TOUCH_FINGER_DOWN, 5, x, y);
    assert(TouchPad_Update() == (TouchPad_Bit(TOUCH_DOWN) | TouchPad_Bit(TOUCH_CROSS)));
    centre(TOUCH_CIRCLE, &x, &y);
    TouchPad_Finger(TOUCH_FINGER_MOVE, 5, x, y);
    assert(TouchPad_Update() == (TouchPad_Bit(TOUCH_DOWN) | TouchPad_Bit(TOUCH_CIRCLE)));
    TouchPad_Finger(TOUCH_FINGER_UP, 4, dx, dy);
    TouchPad_Finger(TOUCH_FINGER_UP, 5, x, y);
    TouchPad_Update();

    /* The small buttons, each its own bit; one the finger slides off lets go. */
    centre(TOUCH_START, &x, &y);
    TouchPad_Finger(TOUCH_FINGER_DOWN, 6, x, y);
    assert(TouchPad_Update() == 0x0008);
    TouchPad_Finger(TOUCH_FINGER_MOVE, 6, 1140, 540);
    assert(TouchPad_Update() == 0);
    TouchPad_Finger(TOUCH_FINGER_UP, 6, 1140, 540);
    centre(TOUCH_L1, &x, &y);
    TouchPad_Finger(TOUCH_FINGER_DOWN, 7, x, y);
    assert(TouchPad_Update() == 0x0400);

    /* A key hides it again, and lets go of what it held. */
    assert(TouchPad_OtherInput() && !TouchPad_Shown() && !TouchPad_Covers(x, y));
    assert(TouchPad_Update() == 0);
    TouchPad_Finger(TOUCH_FINGER_UP, 7, x, y);

    /* Show keeps it without a touch; Hide never takes one. */
    assert(TouchPad_SetMode(TOUCH_PAD_SHOW) && TouchPad_Shown() && !TouchPad_OtherInput() && TouchPad_Shown());
    assert(TouchPad_SetMode(TOUCH_PAD_HIDE) && !TouchPad_Shown());
    assert(!TouchPad_Finger(TOUCH_FINGER_DOWN, 8, 100, 600) && !TouchPad_Shown() && TouchPad_Update() == 0);

    /* MENU: tapped once, it presses no pad button; it sits clear of the
     * D-pad (a touch on it is not an arrow) and is a button tall. The Game
     * style has no STATES, SPEED or top row. */
    assert(TouchPad_SetMode(TOUCH_PAD_SHOW) && TouchPad_Shown());
    centre(TOUCH_MENU, &x, &y);
    assert(TouchPad_Covers(x, y) && !TouchPad_TakeMenu());
    assert(TouchPad_Finger(TOUCH_FINGER_DOWN, 9, x, y) && TouchPad_Held(TOUCH_MENU));
    assert(TouchPad_Update() == 0 && TouchPad_TakeMenu() && !TouchPad_TakeMenu());
    {
        int mx, my, mw, mh, ax, ay, aw, ah;
        TouchPad_Rect(TOUCH_MENU, &mx, &my, &mw, &mh);
        TouchPad_Rect(TOUCH_UP, &ax, &ay, &aw, &ah);
        assert(my + mh <= ay);
        assert(TouchPad_Covers(x, my - (1080 * 13 / 100 - mh) / 2 + 2)); /* the taller hit box */
        assert(!TouchPad_Rect(TOUCH_STATES, &ax, &ay, &aw, &ah) && !TouchPad_Rect(TOUCH_SPEED, &ax, &ay, &aw, &ah));
        assert(!TouchPad_TopRowBottom());
    }

    /* Blocked (a menu is up): nothing drawn, no touch taken, held let go. */
    centre(TOUCH_CROSS, &x, &y);
    TouchPad_Finger(TOUCH_FINGER_DOWN, 10, x, y);
    assert(TouchPad_Update() == TouchPad_Bit(TOUCH_CROSS));
    assert(TouchPad_Block(1) && !TouchPad_Shown() && !TouchPad_Covers(x, y) && !TouchPad_Held(TOUCH_MENU));
    assert(TouchPad_Update() == 0 && !TouchPad_Finger(TOUCH_FINGER_DOWN, 11, x, y) && !TouchPad_Block(1));
    assert(TouchPad_Block(0) && TouchPad_Shown() && TouchPad_Covers(x, y));
    TouchPad_Finger(TOUCH_FINGER_UP, 9, x, y);
    TouchPad_Finger(TOUCH_FINGER_UP, 10, x, y);

    /* The free span: between the columns, the picture's middle in it. */
    {
        int left, right;
        assert(TouchPad_FreeSpan(&left, &right) && left < 1140 && right > 1140 && left > 0 && right < 2280);
        assert(!TouchPad_Covers(left, 540) && !TouchPad_Covers(right - 1, 540));
        /* With the density: a thumb is 48 dp at least and 80 dp at most. */
        TouchPad_Layout(2560, 1600, 0);
        assert(TouchPad_SetDensity(2.0f));
        centre(TOUCH_CROSS, &x, &y);
        TouchPad_Rect(TOUCH_CROSS, &dx, &dy, &ux, &uy);
        assert(ux == 160 && uy == 160);
        TouchPad_Layout(1280, 720, 0);
        TouchPad_Rect(TOUCH_CROSS, &dx, &dy, &ux, &uy);
        assert(ux == 96);
    }

    /* The Game style's buttons each take the touches on them, where its
     * buttons do not overlap (a shorter window, or a bar above the pad at
     * 2400 x 1080 and 440 dpi, brings SELECT and START into the D-pad's and
     * the face buttons' reach, as it always has). */
    check_hits("game", 2280, 1080, 0, 0.0f);
    check_hits("game", 1920, 1080, 0, 0.0f);
    check_hits("game", 2560, 1600, 0, 2.0f);
    check_hits("game", 1280, 720, 0, 2.0f);
    check_hits("game", 2400, 1080, 0, 2.75f);

    /* Simple: changing the style lays the pad out again and lets go of what
     * it held. */
    TouchPad_SetDensity(0.0f);
    TouchPad_Layout(2280, 1080, 0);
    centre(TOUCH_CROSS, &x, &y);
    TouchPad_Finger(TOUCH_FINGER_DOWN, 12, x, y);
    assert(TouchPad_Update() == TouchPad_Bit(TOUCH_CROSS));
    assert(TouchPad_SetStyle(TOUCH_STYLE_SIMPLE) && !TouchPad_SetStyle(TOUCH_STYLE_SIMPLE));
    assert(TouchPad_Style() == TOUCH_STYLE_SIMPLE && TouchPad_Update() == 0 && !TouchPad_Held(TOUCH_CROSS));
    TouchPad_Finger(TOUCH_FINGER_UP, 12, x, y);

    /* The top row: each tapped once presses no pad button, and only its own
     * Take sees it, once the finger lifts on it; the row sits in the middle
     * of the top edge, between the columns and above the rings, and is a
     * button tall to tap. */
    {
        static const TouchButton row[] = {TOUCH_MENU, TOUCH_STATES, TOUCH_SPEED};
        int k, mx, my, mw, mh, lx, ly, lw, lh, rx, ry, rw, rh, cx, cy;
        for (k = 0; k < 3; k++) {
            centre(row[k], &x, &y);
            assert(TouchPad_Covers(x, y) && !TouchPad_TakeMenu() && !TouchPad_TakeStates() && !TouchPad_TakeSpeed());
            assert(TouchPad_Finger(TOUCH_FINGER_DOWN, 13, x, y) && TouchPad_Held(row[k]));
            assert(TouchPad_Update() == 0);
            assert(!TouchPad_TakeMenu() && !TouchPad_TakeStates() && !TouchPad_TakeSpeed());
            assert(TouchPad_Finger(TOUCH_FINGER_UP, 13, x, y) && !TouchPad_Held(row[k]));
            assert(TouchPad_TakeMenu() == (k == 0) && TouchPad_TakeStates() == (k == 1) &&
                   TouchPad_TakeSpeed() == (k == 2));
            assert(!TouchPad_TakeMenu() && !TouchPad_TakeStates() && !TouchPad_TakeSpeed());
            /* Brushed: lifted off the button, or slid onto it from the
             * picture (a finger that is not the pad's), it does nothing;
             * nor does a touch on it the system cancels. */
            TouchPad_Finger(TOUCH_FINGER_DOWN, 13, x, y);
            TouchPad_Finger(TOUCH_FINGER_MOVE, 13, 1140, 540);
            assert(!TouchPad_Held(row[k]));
            TouchPad_Finger(TOUCH_FINGER_UP, 13, 1140, 540);
            assert(!TouchPad_Finger(TOUCH_FINGER_DOWN, 14, 1140, 540));
            TouchPad_Finger(TOUCH_FINGER_MOVE, 14, x, y);
            TouchPad_Finger(TOUCH_FINGER_UP, 14, x, y);
            assert(TouchPad_Finger(TOUCH_FINGER_DOWN, 15, x, y) && TouchPad_Held(row[k]));
            assert(TouchPad_Finger(TOUCH_FINGER_CANCEL, 15, x, y) && !TouchPad_Held(row[k]));
            assert(!TouchPad_Finger(TOUCH_FINGER_UP, 15, x, y)); /* gone: a later lift is no one's */
            assert(!TouchPad_TakeMenu() && !TouchPad_TakeStates() && !TouchPad_TakeSpeed());
            TouchPad_Rect(row[k], &mx, &my, &mw, &mh);
            TouchPad_Rect(TOUCH_L2, &lx, &ly, &lw, &lh);
            TouchPad_Rect(TOUCH_R2, &rx, &ry, &rw, &rh);
            TouchPad_Centre(0, &cx, &cy);
            assert(mx > lx + lw && mx + mw < rx && my + mh < cy - TouchPad_RingRadius());
            assert(TouchPad_Covers(x, my + mh + 2)); /* the taller hit box */
            assert(TouchPad_TopRowBottom() > my + mh);
        }
        /* The columns: L2, L1 and SELECT down the left, clear of the D-pad's
         * ring; a touch between two takes the nearer. */
        TouchPad_Rect(TOUCH_L1, &mx, &my, &mw, &mh);
        TouchPad_Rect(TOUCH_SELECT, &rx, &ry, &rw, &rh);
        assert(lx == mx && mx == rx && ly + lh < my && my + mh < ry && ry + rh < cy - TouchPad_RingRadius());
        TouchPad_Finger(TOUCH_FINGER_DOWN, 13, lx + lw / 2, ly + lh + (my - ly - lh) / 2 + 3);
        assert(TouchPad_Update() == TouchPad_Bit(TOUCH_L1));
        TouchPad_Finger(TOUCH_FINGER_UP, 13, lx + lw / 2, my - 3);
        TouchPad_Update();
    }

    /* Simple's free span: between the rings and the columns. */
    {
        int left, right;
        assert(TouchPad_FreeSpan(&left, &right) && left < 1140 && right > 1140 && left > 0 && right < 2280);
        assert(!TouchPad_Covers(left, 540) && !TouchPad_Covers(right - 1, 540));
    }

    /* Simple's buttons each take the touches on them, and its rings only
     * those inside them, down to short windows (1280 x 720 and 1024 x 768 at
     * 440 dpi, under 307 dp tall). */
    check_hits("simple", 2280, 1080, 0, 0.0f);
    check_hits("simple", 1920, 1080, 0, 0.0f);
    check_hits("simple", 2560, 1600, 0, 2.0f);
    check_hits("simple", 1280, 720, 0, 2.75f);
    check_hits("simple", 1024, 768, 0, 2.75f);
    check_hits("simple", 2400, 1080, 72, 2.75f);
    check_hits("simple", 800, 480, 0, 1.5f);
    check_hits("simple", 1280, 400, 0, 3.0f);

    /* So short a window that 48 dp would put the columns into the ring: the
     * buttons get smaller instead. */
    {
        int left, right;
        TouchPad_Layout(1280, 400, 0);
        TouchPad_SetDensity(3.0f);
        TouchPad_Rect(TOUCH_SELECT, &dx, &dy, &ux, &uy);
        TouchPad_Centre(0, &x, &y);
        assert(TouchPad_Unit() < 64 && dy + uy < y - TouchPad_RingRadius());
        assert(!TouchPad_SetMode(TOUCH_PAD_HIDE) || !TouchPad_FreeSpan(&left, &right));
        assert(!TouchPad_TopRowBottom());
    }
    puts("touch pad: showing, D-pad angles, face buttons, taps, fingers, hiding, MENU, blocking, density, "
         "both styles' hits, Simple's top row and columns passed");
    return 0;
}
