#include "../types.h"
#include "../psyq/libgte.h"
#include "../psyq/libgpu.h"
#include "../psyq/libgs.h"
#include "../psyq/libhmd.h"
#include "model.h"
#include "model_packet_handlers.h"
#include "../unmatched.h"
#ifdef MEMORIES_PC
#include "pc/cards/battle_models.h"
#endif

#define MODEL_ANIMATION_SEQUENCE_SLOT(entry) \
    ((GsSEQ *G32 *)&(entry)->field_1E0[(entry)->field_E1B])
#define MODEL_ANIMATION_WORDS(object) ((u32 *G32)(object)[1])

void func_8005C6A0(s32 *object, ModelSlot *entry)
{
    int local[2];
    int count;
    void *handler;

    if (object[0] != 0x03800000) {
        handler = (void *)GsU_00000000;
        *(void *G32 *G32)object[1] = handler;
        return;
    }
    handler = func_8005C7BC;
    *(void *G32 *G32)object[1] = handler;
#ifdef MEMORIES_PC
    /* The slot has MODEL_SLOT_PART_COUNT parts; a mod's bigger model with
       more would be linked past them (the disc's have 42 at most). It is
       not linked, and BattleModels_Check loads the model's base instead. */
    if (entry->field_E1B + ((u16 *)MODEL_ANIMATION_WORDS(object))[3] > MODEL_SLOT_PART_COUNT) {
        BattleModels_TooManyParts();
        *(void *G32 *G32)object[1] = (void *)GsU_00000000;
        return;
    }
#endif
    count = GsLinkAnim(
        MODEL_ANIMATION_SEQUENCE_SLOT(entry),
        MODEL_ANIMATION_WORDS(object)
    );
    if (GsScanAnim(MODEL_ANIMATION_WORDS(object), 0)) {
        while (GsScanAnim(0, (GsTYPEUNIT *)local)) {
            *(void *G32 *G32)local[1] = func_8005C768(local[0]);
        }
        entry->field_E1B += count;
    }
}

void *func_8005C768(u32 value)
{
    if ((value & 0xFFFF0000) != 0x03000000) {
        goto default_case;
    }
    value &= 0xFFFF;
    if (value == 0x2019) {
        goto case_2019;
    }
    if (value == 0x2119) {
        goto case_2119;
    }
    goto default_case;

case_2019:
    return func_8005CEF0;
case_2119:
    return func_8005D378;
default_case:
    return (void *)GsU_00000000;
}
