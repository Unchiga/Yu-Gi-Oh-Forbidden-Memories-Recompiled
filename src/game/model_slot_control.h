#ifndef MEMORIES_DECOMP_MODEL_SLOT_CONTROL_H
#define MEMORIES_DECOMP_MODEL_SLOT_CONTROL_H

/* The upstream decomp's name for the slot control header. Its model modules
 * include it (french_model_variant/variant88_entry.h, shape 71) and use
 * nothing from it; upstream's copy asserts ModelControlCommandView against
 * ModelSlot.metadata, which the port calls field_CF8. The port keeps its
 * own headers, which declare the same types and functions, and
 * model_gate.py port keeps this file because the port has it
 * (tools/pc/model_gate_port.py, rule 1). */
#include "model_control.h"
#include "model_control_slot_animation.h"

#endif
