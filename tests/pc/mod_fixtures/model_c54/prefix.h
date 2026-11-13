/* The port's C for model shape 54 (src/native_overlays), built into a code mod
 * as its own C for a control module (mod API 29; tools/pc/test_model_mod_c.py).
 * A mod's object is one unit, so each slot's entry gets a name of its own,
 * as the game build gives each gated module its prefix; and the resident
 * functions the game's build renames (config/pc/host_symbol_renames.txt) are
 * called by the names the game lends them under: a mod's own `rand` is the
 * mods' (src/pc/mods/mod_libc.c), not the game's, which the module's MIPS calls. */
#ifndef MODEL_C54_PREFIX_H
#define MODEL_C54_PREFIX_H
#define rand Memories_Rand
#define srand Memories_Srand
#define ccos Psx_ccos
#define csin Psx_csin
/* libgte.h declares these without a prototype, which the translated build
 * (aarch64-translated) reads as variadic and refuses against the game's
 * definitions; declared first, the later () declarations keep these. */
void PopMatrix(void);
void PushMatrix(void);
#endif
