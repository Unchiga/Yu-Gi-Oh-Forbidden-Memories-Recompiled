#ifndef MEMORIES_PC_DEBUG_HUD_H
#define MEMORIES_PC_DEBUG_HUD_H
#include "pc/platform/menu.h"

/* The statistics and, when it is open, the save slot menu (pc/saves). */
void Hud_Draw(MenuCanvas *canvas);
/* The mods' overlays (MemoriesMod.overlay) on their own, for a window that
 * draws something of its own over them (the touch pad): the next Hud_Draw,
 * which must follow in the same composition, leaves them out, and its
 * bounds still cover them. */
void Hud_DrawMods(MenuCanvas *canvas);
/* The mods' overlays once more where a sink draws them (Mods_SetOverlaySink). */
void Hud_ListMods(MenuCanvas *canvas);
void Hud_Bounds(int *x, int *y, int *w, int *h);
/* Of those, what the last Hud_Draw drew that is not the mods' overlays. */
void Hud_OtherBounds(int *x, int *y, int *w, int *h);
/* Changes whenever what the HUD would draw changes; the full statistics
 * level changes every frame. Hud_ModsSignature is the mods' overlays' part
 * of it and Hud_OtherSignature the rest, for a window that draws the
 * overlays again where they changed when only they did. */
unsigned Hud_Signature(void);
unsigned Hud_OtherSignature(void);
unsigned Hud_ModsSignature(void);

#endif
