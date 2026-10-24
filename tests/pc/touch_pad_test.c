/* The on-screen controller's logic (src/pc/platform/touch_pad.c): when it
 * shows, what a finger presses where, and that a tap shorter than a pump
 * still counts once. */
#include "pc/platform/touch_pad.h"
#include <assert.h>
#include <stdio.h>

static void centre(TouchButton button, int *x, int *y)
{
    int bx, by, bw, bh;
    assert(TouchPad_Rect(button, &bx, &by, &bw, &bh));
    *x = bx + bw / 2;
    *y = by + bh / 2;
}

int main(void)
{
    int x, y, ux, uy, dx, dy;
    /* Automatic: hidden, and nothing is the pad's, until a touch. */
    TouchPad_SetMode(TOUCH_PAD_AUTO);
    TouchPad_Layout(2280, 1080, 0);
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

    /* The top row: each tapped once presses no pad button, and only its own
     * Take sees it; the row sits in the middle of the top edge, between the
     * columns and above the rings, and is a button tall to tap. */
    assert(TouchPad_SetMode(TOUCH_PAD_SHOW) && TouchPad_Shown());
    {
        static const TouchButton row[] = {TOUCH_MENU, TOUCH_STATES, TOUCH_SPEED};
        int k, mx, my, mw, mh, lx, ly, lw, lh, rx, ry, rw, rh, cx, cy;
        for (k = 0; k < 3; k++) {
            centre(row[k], &x, &y);
            assert(TouchPad_Covers(x, y) && !TouchPad_TakeMenu() && !TouchPad_TakeStates() && !TouchPad_TakeSpeed());
            assert(TouchPad_Finger(TOUCH_FINGER_DOWN, 9, x, y) && TouchPad_Held(row[k]));
            assert(TouchPad_Update() == 0);
            assert(TouchPad_TakeMenu() == (k == 0) && TouchPad_TakeStates() == (k == 1) &&
                   TouchPad_TakeSpeed() == (k == 2));
            assert(!TouchPad_TakeMenu() && !TouchPad_TakeStates() && !TouchPad_TakeSpeed());
            TouchPad_Finger(TOUCH_FINGER_UP, 9, x, y);
            assert(!TouchPad_Held(row[k]));
            TouchPad_Rect(row[k], &mx, &my, &mw, &mh);
            TouchPad_Rect(TOUCH_L2, &lx, &ly, &lw, &lh);
            TouchPad_Rect(TOUCH_R2, &rx, &ry, &rw, &rh);
            TouchPad_Centre(0, &cx, &cy);
            assert(mx > lx + lw && mx + mw < rx && my + mh < cy - 2 * (1080 * 13 / 100));
            assert(TouchPad_Covers(x, my + mh + 2)); /* the taller hit box */
            assert(TouchPad_TopRowBottom() > my + mh);
        }
        /* The columns: L2, L1 and SELECT down the left, clear of the D-pad's
         * ring; a touch between two takes the nearer. */
        TouchPad_Rect(TOUCH_L1, &mx, &my, &mw, &mh);
        TouchPad_Rect(TOUCH_SELECT, &rx, &ry, &rw, &rh);
        assert(lx == mx && mx == rx && ly + lh < my && my + mh < ry && ry + rh < cy - (1080 * 13 / 100) * 8 / 5);
        TouchPad_Finger(TOUCH_FINGER_DOWN, 9, lx + lw / 2, ly + lh + (my - ly - lh) / 2 + 3);
        assert(TouchPad_Update() == TouchPad_Bit(TOUCH_L1));
        TouchPad_Finger(TOUCH_FINGER_UP, 9, lx + lw / 2, my - 3);
        TouchPad_Update();
        centre(TOUCH_MENU, &x, &y);
        TouchPad_Finger(TOUCH_FINGER_DOWN, 9, x, y);
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
        /* So short a window that 48 dp would put the columns into the ring:
         * the buttons get smaller instead. */
        TouchPad_Layout(1280, 400, 0);
        assert(TouchPad_SetDensity(3.0f));
        TouchPad_Rect(TOUCH_SELECT, &dx, &dy, &ux, &uy);
        TouchPad_Centre(0, &x, &y);
        assert(TouchPad_Unit() < 64 && dy + uy < y - TouchPad_Unit() * 8 / 5);
        assert(!TouchPad_SetMode(TOUCH_PAD_HIDE) || !TouchPad_FreeSpan(&left, &right));
        assert(!TouchPad_TopRowBottom());
    }
    puts("touch pad: showing, D-pad angles, face buttons, taps, fingers, hiding, top row, columns, blocking, "
         "density passed");
    return 0;
}
