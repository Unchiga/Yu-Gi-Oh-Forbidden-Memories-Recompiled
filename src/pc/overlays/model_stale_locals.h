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
/* Model_GetActiveSlotIndex, which picks a slot's storage in every rule 6 file. */
#include "game/model_graphics_state.h"

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

/* Shape 831: `points`. The halo reads it, and only the gathering frames
 * (elapsed <= gather_end) write it. */
extern SVECTOR gModelVariant831Points[2][2];

/* Shape 769, and image31368_entry.c (shape 772's body): `quad` and `screen`.
 * After the burst, `quad` is submitted without its UVs, which the frames
 * before the burst wrote; the spray reads screen[0], which only the
 * gathering frames and the burst's first frames write. */
extern POLY_FT4 gModelVariant769Quad[2];
extern DVECTOR gModelVariant769Screen[2][128];
extern POLY_FT4 gModelImage31368Quad[2];
extern DVECTOR gModelImage31368Screen[2][128];

/* Shape 84 (french_model_variant/variant101_entry.c): `flags`. The particles'
 * first loop tests flags[i] before RotTransPersN writes the array; the MIPS
 * code reads what the slot's previous call left there. */
extern u16 gModelFrenchVariant101Flags[2][96];

/* Shape 783: `quad` and `screen`, as 769. One path submits `quad` without
 * its UVs, which only the other path writes; the spray sorts a box at
 * screen[0] on calls that do not write it. */
extern POLY_FT4 gModelVariant783Quad[2];
extern DVECTOR gModelVariant783Screen[2][128];

/* Shape 448 (variant448_fan.c): `col`, which the code never names and reaches
 * as (s32)&rot + 240..331, its place in the MIPS frame; written and read in the
 * same call (the fan's colours), so storage per slot only gives it a place. */
extern u8 gModelVariant448Col[2][6][16];

#endif
