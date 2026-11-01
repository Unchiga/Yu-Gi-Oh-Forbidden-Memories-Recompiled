#ifndef MEMORIES_PC_HD_TEXT_H
#define MEMORIES_PC_HD_TEXT_H
/* HD text (Video > HD text, SET_HD_TEXT): at an internal resolution above the console's,
 * the OpenGL picture (gl_picture.c) draws the text's glyphs from pictures
 * set in a font at that resolution instead of the retail 8x12 and 16x16
 * cells. The pictures are 4-bit indices like the cells, with the cells'
 * dark outline and their shading row by row, so they go through
 * the text's own palettes: colors, fades, flashes and semi-transparency are
 * the game's. func_80035E20 marks the glyph primitives (HD_TEXT_MARK), and
 * only those are drawn from the pictures; a glyph no font can set (an icon)
 * stays as it was. The software picture, and 1x, never change. */
#include <stdint.h>

/* The mark on a glyph's texture-page word: bit 15, which the hardware
 * leaves unused (bits 11-14 are the texture bank, soft_gpu.h). */
#define HD_TEXT_MARK 0x8000

int HdText_Enabled(void);

/* The picture of the glyph cell at u, v of a 4-bit texture page (in VRAM,
 * or in texture bank `bank`), `large` for the 16x16 font else the 8x12, at
 * `factor` pixels per texel: where it is in the atlas, in texels (a cell is
 * 16 x 16 of them; multiply by the factor for pixels). Made the first time,
 * and again when the cell's pixels change. Returns 0 when there is none. */
int HdText_Cell(int bank, int page_x, int page_y, int large, int u, int v, int factor, int *atlas_u, int *atlas_v);

/* Card titles: the game's plate for a card (96 x 14 texels, 4-bit) set
 * anew from the card's name (CardArt_TitlePicture) at `factor`, so a card
 * a mod adds and a translated name read like the rest. func_800289BC tells
 * where it put a card's plate; a 4-bit primitive sampling those words
 * (while they are still the plate) draws from the picture: where the
 * title's texel (0, 0) is in the atlas, and in the page (title_u, title_v),
 * so a texel u, v is at atlas u + (u - title_u). */
void HdText_TitleUploaded(int card, int x, int y);
int HdText_Title(int page_x, int page_y, int u, int v, int factor, int *atlas_u, int *atlas_v, int *title_u,
                 int *title_v);
/* HdText_Title, and the card whose title it is in `*card` (NULL: not wanted). */
int HdText_TitleCard(int page_x, int page_y, int u, int v, int factor, int *atlas_u, int *atlas_v, int *title_u,
                     int *title_v, int *card);

/* HD numbers and labels (with HD text, HdText_Enabled): the duel's digits
 * (life points, deck counts, the field cards' ATK and DEF, the card view's,
 * the menus'), the life-point panel's LP, COM and YOU, the card kinds, the
 * FIELD box and the terrains' names, which are sprites from sheets of their
 * own, not the font's cells. A digit's picture is set in the font like a
 * glyph's, to the lines, weight and colors measured from its sheet's ten
 * digits; the panel's is its texels made larger with the labels set anew;
 * a word's is set over its sprites in their colors. Only where the retail
 * sheets and panel are: a sheet whose digits cannot be measured, or another
 * panel (a mod's), stays. The
 * picture of the sprite at u, v, w x h of a page (depth 0 4-bit, 1 8-bit)
 * through the palette at clut_x, clut_y: where its texel u, v is in the
 * atlas. Returns 0 for any other sprite. */
int HdText_HudEnabled(void);
int HdText_Hud(int depth, int page_x, int page_y, int clut_x, int clut_y, int u, int v, int w, int h, int factor,
               int *atlas_u, int *atlas_v);
/* The active field's own name over the FIELD box's terrain word (fields.h,
 * Fields_HasCustomName), like HdText_TitleCard: checked and drawn whether
 * or not a texture pack already painted that sprite, so a renamed or
 * modded field's name is never left wrong by one. Returns 0 at once, no
 * texture pack touched, when the active field has no name of its own. */
int HdText_FieldWord(int depth, int page_x, int page_y, int clut_x, int clut_y, int u, int v, int w, int h,
                     int factor, int *atlas_u, int *atlas_v);
/* The opponent's name in place of COM (View > Opponent's name for COM),
 * (at 1x too: HdText_NamePixels): a box made from COM's, as long as the name needs
 * (Tables_DuelistShortName), drawn over the life-point panel at x, y
 * (panel texels; x runs left of the panel for a long name), width x height,
 * its picture at atlas_u, atlas_v; `which` 1 is YOU's box with the
 * player's name from name entry, or You. 0 with no opponent (2P) or no retail panel. */
int HdText_NameEnabled(void);
/* A mod's "ui" draws the panel in pieces, at any size (pc/cards/duel_ui.h):
 * the names then go over pieces drawn as quads too. */
int HdText_PanelCut(void);
int HdText_NameBox(int factor, int which, int *atlas_u, int *atlas_v, int *x, int *y, int *width, int *height);
/* The same box at the console's resolution (soft_gpu.c draws it there):
 * width x height palette indices of the panel's CLUT, `stride` apart, to
 * draw at x, y from the panel; NULL when there is none. Makes the atlas
 * 1x, so only while the OpenGL picture is not drawing (at 1x). */
const uint8_t *HdText_NamePixels(int which, int *x, int *y, int *width, int *height, int *stride);
/* The player's name as YOU's box and the result screens show it: the
 * save's, in ASCII (SaveSlots_StateName), or You when it has none. */
const char *HdText_PlayerName(void);

/* The atlas: 8-bit indices, *side x *side pixels (0 before the first
 * cell). The rows from *first to *last have changed since the last call
 * (none when *last < *first); the generation changes when the atlas is
 * made anew (another factor). */
const uint8_t *HdText_Atlas(int *side, int *first, int *last, unsigned *generation);
#endif
