/* See overlay_pointer.h. */
#include "overlay_pointer.h"
#include "platform.h"

unsigned Memories_PresentedFrames(void);

int OverlayPointer_Read(int *x, int *y, int *moved, int *clicked)
{
    static int have, last_x, last_y, was_down;
    static unsigned last_frame;
    unsigned frame = Memories_PresentedFrames(), buttons = 0;
    int px = 0, py = 0, down;

    if (!Platform_WindowPointer(&px, &py, &buttons)) {
        have = 0;
        return 0;
    }
    down = (buttons & 1u) != 0;
    /* Away for a while (the menu was closed, or another thing had the mouse): a new start. */
    if (!have || frame - last_frame > 3u) {
        have = 1;
        last_x = px;
        last_y = py;
        was_down = down;
        *moved = *clicked = 0;
    } else {
        *moved = px != last_x || py != last_y;
        *clicked = down && !was_down;
    }
    last_x = px;
    last_y = py;
    was_down = down;
    last_frame = frame;
    *x = px;
    *y = py;
    return 1;
}
