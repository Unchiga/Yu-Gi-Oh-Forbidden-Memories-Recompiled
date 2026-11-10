#ifndef MEMORIES_PC_PLATFORM_OVERLAY_POINTER_H
#define MEMORIES_PC_PLATFORM_OVERLAY_POINTER_H
/* The mouse for the port's own overlay menus (the save slots, the deck
 * slots): the window's pixels, which is what those menus draw in, with what
 * changed since the last read. A menu reads it once a frame while it is up.
 *
 * Returns 0 when there is no mouse for the menu (no pointer in the window,
 * a touch, the menu bar, a menu or a panel has it, a mod has captured it).
 * The first read after the menu has been away reports no movement and no
 * click, so that a button already held, or a pointer resting over the list
 * as it opens, does nothing until the player moves or presses again. */
int OverlayPointer_Read(int *x, int *y, int *moved, int *clicked);
#endif
