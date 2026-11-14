/* Shape 54's slot A module (0x8013B000) as the mod's C: the port's own file.
 * Its tables past the code (D_8013C648, D_8013C6B8, D_8013C6D4) are extern,
 * so the C reads the record's bytes where the game loaded them (the game
 * binds the module range's D_ names to their address). */
#include "prefix.h"
#define func_8013B004 model_c54_slot_a
/* Unoptimized, as the game builds the port's C (-O0 -fwrapv): build_mod.py
 * compiles a mod at -O2, and the decomp's C keeps some of the MIPS code's
 * frame (shape 109 writes a fourth entry of a three-entry local array, which
 * at -O0 lands in the next local as on the PS1, and at -O2 does not). */
#if defined(__clang__)
#pragma clang optimize off
#elif defined(__GNUC__)
#pragma GCC optimize ("O0")
#endif
#include "native_overlays/model_variant/variant54_entry.c"
