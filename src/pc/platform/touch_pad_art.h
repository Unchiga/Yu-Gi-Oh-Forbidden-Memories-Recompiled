#ifndef MEMORIES_PC_TOUCH_PAD_ART_H
#define MEMORIES_PC_TOUCH_PAD_ART_H
/* The on-screen controller's pictures (touch_pad.h), in one grey scale and
 * see-through (View > Touch controls > Opacity; a button held is twice as
 * solid): frames and discs with a light edge, the D-pad's solid arrows in a
 * ring, the face buttons' outlined triangle, circle, cross and square, MENU's
 * cog and STATES' and SPEED's triangles, all drawn; L1/L2/R1/R2, SELECT,
 * START and the speed in the game text's font. Main thread only. */
#include "menu.h"

/* Draws the pad where touch_pad.c has laid it out (nothing while hidden),
 * and gives the bounds it covered (all 0 when nothing). */
void TouchPadArt_Draw(MenuCanvas *canvas, int *x, int *y, int *w, int *h);
/* Frees the layer the pad is drawn in; the next draw makes it again. */
void TouchPadArt_Reset(void);

#endif
