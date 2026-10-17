#ifndef MEMORIES_PC_CARDS_CARD_LAYOUT_ART_H
#define MEMORIES_PC_CARDS_CARD_LAYOUT_ART_H
/* The unified card frame (card_layout.h's CardLayout_Part(CARD_LAYOUT_FRAME)
 * w/h) -- a layout mod's own PNG, no disc counterpart and no counterpart
 * compiled into the port either (card_layout.h's CardLayout_FramePath),
 * decoded once into a SoftGpu_Bank the same way src/pc/cards/star_icons.c
 * and src/pc/text/glyphs.c already do for theirs, then read back as an
 * ordinary tpage/u/v/clut cell. */

/* The frame is drawn as COLS x ROWS abutting tiles, each one quad on its own
 * texture page (card_layout_art.c explains why: a quad reads at most 255
 * texels an axis). This is the one place the grid is set: func_80028B08.c's
 * CardLayout_DrawFrame loops over it, card_layout_art.c's tile layout and
 * its FRAME_W/FRAME_H follow it, and tools/pc/card_frame_window.py's
 * FRAME_TEXELS is the same grid of 255x254 texel tiles (2x2 = 510x508).
 * A row or column left out of the loop is a part of the frame never drawn.
 * Changing either number means changing all of those. The bank has 16
 * pages: FRAME_SLOTS frame styles of COLS * ROWS pages each, and the digit
 * strip's page after them (card_layout_art.c), so the grid stays small:
 * several cards of different styles are on screen together (an attack, a
 * trap shown beside the attacker) and each needs its own copy. */
#define CARD_LAYOUT_FRAME_COLS 2
#define CARD_LAYOUT_FRAME_ROWS 2

/* 1 on success (`*tpage`/`*clut` filled, the tile's own texel w/h in
 * `*w`/`*h`; its texels start at u = v = 0 of that page), 0 when no mod
 * gives a frame path, or it could not be decoded or the bank could not be
 * allocated -- the caller (func_80028B08.c) simply doesn't draw it then,
 * purely decorative. */
int CardLayoutArt_FrameTile(int col, int row, int *tpage, int *clut, int *w, int *h);

/* The current card style's optional 52x60 duel hand/flat-field frame. The
 * returned cell is ready for a SpritePrim; 0 leaves the retail sprite alone. */
int CardLayoutArt_HandFrameCell(int card_id, int *tpage, int *u, int *v, int *clut, int *w, int *h);

/* The current style's optional 52x60 texture for the 3D duel-field card
 * quad (func_80015EF4's POLY_GT4; card_layout.h's CardLayout_FieldFramePath).
 * 0 leaves retail's Cards_FrameColor palette-row recolor in place. */
int CardLayoutArt_FieldFrameCell(int card_id, int *tpage, int *u, int *v, int *clut, int *w, int *h);

/* The current style's optional 52x60 face-down texture for the flat hand/
 * field sprite (card_layout.h's CardLayout_FieldBackPath). 0 leaves
 * retail's own generic card-back sprite alone. */
int CardLayoutArt_FieldBackCell(int card_id, int *tpage, int *u, int *v, int *clut, int *w, int *h);

/* The current style's optional texture for the big card-viewer's rotating
 * "card turning" sprite (card_layout.h's CardLayout_BackImagePath), stretched
 * to the style's own "width" x "height". *w/*h on success describe that
 * replacement texture's source dimensions; the renderer maps each retail
 * turning-sheet part into them while retaining the retail geometry that
 * drives the turn animation. (The style box is already capped at 140x196,
 * card_layout.c's CARD_LAYOUT_FRAME_MAX_W/H; card_layout_art.c's
 * BACK_MAX_SIDE, 254 an axis -- one short of POLY_FT4's own u8 UV span, for
 * the padded edge this file's cells all keep against the PS1 GPU's
 * last-texel-drop quirk -- is a guard behind that.) 0 leaves retail's own
 * swirl sprite alone. */
int CardLayoutArt_BackCell(int card_id, int *tpage, int *u, int *v, int *clut, int *w, int *h);

/* The current style's optional 12x14 Library-grid cell. */
int CardLayoutArt_LibraryIconCell(int card_id, int *tpage, int *u, int *v, int *clut, int *w, int *h);

/* The layout's ATK/DFD digits (card_layout.h's CardLayout_Digits): one PNG,
 * DIGIT_COLS x DIGIT_ROWS cells of DIGIT_CELL_W x DIGIT_CELL_H texels, digit
 * d in column d % 5, row d / 5, and the same ten greyed (`dim`: the stat
 * the attack screen dims) two rows further down -- 200x192 (4 texels a card
 * unit, one a screen pixel at Internal 4x), which tools/pc/card_digits.py
 * draws from a font. 1 on success (the cell's page, u, v, clut and texel
 * size filled), 0 when the layout gives none or it could not be built: the
 * caller draws the retail digits then. */
#define CARD_LAYOUT_DIGIT_COLS 5
#define CARD_LAYOUT_DIGIT_ROWS 4
#define CARD_LAYOUT_DIGIT_CELL_W 40
#define CARD_LAYOUT_DIGIT_CELL_H 48
int CardLayoutArt_DigitCell(int digit, int dim, int *tpage, int *u, int *v, int *clut, int *w, int *h);

/* The current style's optional static ATK (dfd = 0) or DFD (dfd != 0)
 * label, read from card_layout.h's CardLayout_StatLabel. */
int CardLayoutArt_StatLabelCell(int dfd, int *tpage, int *u, int *v, int *clut, int *w, int *h);

/* `kind`'s (card_constants.h's CARD_TYPE_MAGIC..CARD_TYPE_EQUIP) optional
 * replacement "[Trap Card]"-style caption, read from card_layout.h's
 * CardLayout_TypeLabel. */
int CardLayoutArt_TypeLabelCell(int kind, int *tpage, int *u, int *v, int *clut, int *w, int *h);

/* Copies the current retail title glyphs into a white, transparent mask in a
 * layout bank. The caller can then use vertex colour for a real title tint. */
int CardLayoutArt_TitleCell(int source_tpage, int source_u, int source_v, int source_w, int source_h,
                            int *tpage, int *u, int *v, int *clut, int *w, int *h);

/* Once a frame (cards.c's Cards_Frame): builds, one a call and only now and
 * then, any frame image of the active layout not built yet, so the cost of
 * building one (decoding a 919x1319 PNG and a 255-color median cut of it,
 * tens to hundreds of milliseconds) is paid off screen, not on the frame a
 * card first draws. */
void CardLayoutArt_Prewarm(void);

/* Drops every picture this file kept by path (decoded pixels, HD registrations,
 * the texture pack's decoded PNGs) so the next draw reads them again.  Each cell
 * function does it by itself when a mod has gone on or off since its last call
 * (Mods_Changes); public for the test. */
void CardLayoutArt_Clear(void);

#endif
