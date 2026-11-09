#ifndef MEMORIES_PC_OVERLAYS_MODEL_STALE_LOCALS_H
#define MEMORIES_PC_OVERLAYS_MODEL_STALE_LOCALS_H
/* Locals of a model module that the retail code reads before the call
 * writes them, so it takes what the module's previous call in that slot left
 * in the same stack slot (shape 102's: a slot no call writes, zero here).
 * Native C would read the host stack's bytes instead.
 * model_gate.py port gives such a local this storage, one per slot
 * (tools/pc/model_gate_port.py, rule 6; notes/pc-build.md), and `check`
 * fails a native row whose C lacks it. */
#include "types.h"
#include "psyq/libgte.h"
#include "psyq/libgpu.h"

/* Shape 169 (model_variant/variant169_entry.c): `vertices`, by
 * Model_GetActiveSlotIndex(). The trail's ratan2 reads vertices[0].vy/.vz,
 * which the waiting branch wrote on an earlier call; the other branches
 * write the array before they use it. */
extern SVECTOR gModelVariant169Vertices[2][4];

/* Shapes 89, 137 and 163: `quad`, by Model_GetActiveSlotIndex(). Some
 * branches submit it without writing its colour, so the packet carries the
 * colour of the last frame that did. */
extern POLY_FT4 gModelVariant89Quad[2];
extern POLY_FT4 gModelVariant137Quad[2];
extern POLY_FT4 gModelVariant163Quad[2];

/* Shape 102: `depths` and `flags`. The ring's cap reads depths[109] and
 * flags[109], one past the 109 points RotTransPersN writes. */
extern u16 gModelVariant102Depths[2][110];
extern u16 gModelVariant102Flags[2][110];

#endif
