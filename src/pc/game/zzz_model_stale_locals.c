/* The storage of the model modules' stale locals (pc/overlays/model_stale_locals.h).
 *
 * A game unit, like trigger_state.c, so it is in the game's data and in every
 * save state, as the retail stack it stands for (the game stack) is: a state
 * saved in the middle of an attack comes back reading what it read. Game
 * units link in name order, and "zzz_" keeps this one after
 * zz_tower_storage.c, so no existing variable moves. Its zeros still make
 * the game's bss longer, and state.c restores a bss chunk only of this
 * build's size, so a state from an earlier build loads without the game's
 * zeroed variables (as after zz_tower_storage.c came in). The translated
 * build (Android) numbers its globals in link order, so there a state from
 * an earlier build is refused, as after any new game variable or module.
 *
 * Zero at boot. The retail images of shape 169 all wait (`delay` 65) before
 * the trail moves, and the waiting branch writes vertices[0], so the first
 * read takes this attack's value. A mod's row with `delay` 0 reads the slot's
 * previous attack's value (or zero), where retail read whatever its stack
 * held. Shapes 89, 137 and 163 write `quad`'s colour in a phase before the
 * one that reads it stale, so their zeros are not read either; shape 102's
 * depths[109] and flags[109] are written by no call and stay zero. Shape
 * 831's `points` and the UVs and screen[0] of shape 769 and image31368_entry.c
 * are written by earlier frames of the same attack before they are read, as
 * are shape 783's (769's code with its own table). Shape 84's first loop
 * draws the points the previous call projected with the flags that call's
 * last RotTransPersN left; a slot's first call reads zeros there, where
 * retail read its stack (record 130 matches the interpreter). */
#include "pc/overlays/model_stale_locals.h"

SVECTOR gModelVariant169Vertices[2][4] = {{{0}}};
POLY_FT4 gModelVariant89Quad[2] = {{0}};
POLY_FT4 gModelVariant137Quad[2] = {{0}};
POLY_FT4 gModelVariant163Quad[2] = {{0}};
u16 gModelVariant102Depths[2][110] = {{0}};
u16 gModelVariant102Flags[2][110] = {{0}};
SVECTOR gModelVariant831Points[2][2] = {{{0}}};
POLY_FT4 gModelVariant769Quad[2] = {{0}};
DVECTOR gModelVariant769Screen[2][128] = {{{0}}};
POLY_FT4 gModelImage31368Quad[2] = {{0}};
DVECTOR gModelImage31368Screen[2][128] = {{{0}}};
u16 gModelFrenchVariant101Flags[2][96] = {{0}};
POLY_FT4 gModelVariant783Quad[2] = {{0}};
DVECTOR gModelVariant783Screen[2][128] = {{{0}}};
u8 gModelVariant448Col[2][6][16] = {{{0}}};
POLY_FT4 gModelVariant128Quad[2] = {{0}};
