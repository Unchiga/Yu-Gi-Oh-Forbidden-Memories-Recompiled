/* Shape 54's slot A module (0x8013B000) as the mod's C: the port's own file.
 * Its tables past the code (D_8013C648, D_8013C6B8, D_8013C6D4) are extern,
 * so the C reads the record's bytes where the game loaded them (the game
 * binds the module range's D_ names to their address). */
#include "prefix.h"
#define func_8013B004 model_c54_slot_a
#include "native_overlays/model_variant/variant54_entry.c"
