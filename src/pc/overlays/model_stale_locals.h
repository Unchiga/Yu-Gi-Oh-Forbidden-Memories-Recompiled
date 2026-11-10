#ifndef MEMORIES_PC_OVERLAYS_MODEL_STALE_LOCALS_H
#define MEMORIES_PC_OVERLAYS_MODEL_STALE_LOCALS_H
/* Locals of a model module that the retail code reads before the call
 * writes them, so it takes what the module's previous call in that slot left
 * in the same stack slot. Native C would read the host stack's bytes instead.
 * model_gate.py port gives such a local this storage, one per slot
 * (tools/pc/model_gate_port.py, rule 6; notes/pc-build.md), and `check`
 * fails a native row whose C lacks it. */
#include "types.h"
#include "psyq/libgte.h"

/* Shape 169 (model_variant/variant169_entry.c): `vertices`, by
 * Model_GetActiveSlotIndex(). The trail's ratan2 reads vertices[0].vy/.vz,
 * which the waiting branch wrote on an earlier call; the other branches
 * write the array before they use it. */
extern SVECTOR gModelVariant169Vertices[2][4];

#endif
