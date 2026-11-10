/* Written by tools/pc/model_gate.py port: do not edit. The resident functions the
 * ported model modules call where the port's headers do not declare them under the
 * name the decomp's C uses: the port's header that declares the function, and for a
 * function the port names otherwise (same address, upstream's symbol files) the
 * decomp's name as the port's. A copied file that needs it includes it first. */
#ifndef MEMORIES_MODEL_GATE_NAMES_H
#define MEMORIES_MODEL_GATE_NAMES_H
#include "../game/func_80058E1C.h"
#include "../game/model_copy_slot_position.h"
#include "../game/model_effect_requests.h"
#include "../game/model_effect_state.h"
#include "../game/model_slot_properties.h"
#include "../game/model_slot_state_updates.h"
#include "../game/model_texture_upload.h"
#define ModelTexture_PackPageClutCopy func_80059A50
#define Model_QueueCameraOrbitFromPreset func_8005F5C8
#define Model_QueueCameraShake func_8005F7B0
#define Model_RequestCameraMoveToSlots func_80059F18
#define Model_SetCameraOrbitFromPreset func_8005F27C
#define Model_SetSlotAmbient func_800595C8
#define Model_SetSlotPlaybackDirection func_80059700
#define Model_SetSlotScale func_800594C0
#define Model_TransformPartVertex func_800593D0
#endif
