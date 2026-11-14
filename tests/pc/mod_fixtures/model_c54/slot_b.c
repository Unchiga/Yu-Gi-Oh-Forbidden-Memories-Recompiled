/* Shape 54's slot B module (0x8017B000) as the mod's C: the port's wrapper,
 * which names the slot B addresses, around the slot A file. A unit of its
 * own: the slot A header's guard would hide the second slot's externs. */
#include "prefix.h"
#define func_8017B004 model_c54_slot_b
/* Unoptimized, as the game builds the port's C (-O0 -fwrapv): build_mod.py
 * compiles a mod at -O2, and the decomp's C keeps some of the MIPS code's
 * frame (shape 109 writes a fourth entry of a three-entry local array, which
 * at -O0 lands in the next local as on the PS1, and at -O2 does not). */
#if defined(__clang__)
#pragma clang optimize off
#elif defined(__GNUC__)
#pragma GCC optimize ("O0")
#endif
#include "native_overlays/model_variant_slot1/variant54_entry_slot1.c"
