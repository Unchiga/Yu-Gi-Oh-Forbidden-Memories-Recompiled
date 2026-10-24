/* A mod's model bigger than a MODEL.MRG record in the battle: battle_models.h. */
#include "battle_models.h"
#include "pc/mods/mods.h"
#include "pc/guest/state.h"
#include "pc/debug/log.h"
#include "psyq/libgte.h"
#include "psyq/libgpu.h"
#include "psyq/libgs.h"
#include "game/model.h"
#define MODEL_SLOT_SETUP_EXPLICIT_TRANSFER_ARGS
#include "game/model_slot_setup.h"
#include <stdint.h>
#include <string.h>

extern s32 D_800FE240;   /* GsSetWorkBase's cursor */
extern u8 D_8009AFA2;    /* the frame's half of the packet buffers */

#define ARENAS 3                          /* slot 0's, slot 1's, the check's */
#define PACKETS(slot) (0x80100000u + (unsigned)(slot) * 0x40000u)
#define PACKET_AREA 0x35000u              /* the record's model data and what follows it */
#define HALF (PACKET_AREA / 2)
#define WORK_PARTS (MODEL_SLOT_PART_COUNT * 0x14)   /* func_80056828's phase 4, at most */

/* What func_8004CB0C counted of the model each arena holds, for the cursor
 * of the half each slot draws into. */
static struct {
    s32 packets[ARENAS], normals[ARENAS];
} parsed;
static s32 cursor[2];
static int begun[2], too_many_parts, refused_draw[2], drawn[2];

static int arena_of(const void *at)
{
    for (int k = 0; k < ARENAS; k++) {
        unsigned size;
        const unsigned char *arena = Mods_ModelArena(k, &size);
        /* Strictly inside: the arenas are contiguous, and a model's data
         * ends before its arena does (BattleModels_Check). */
        if (arena && (uintptr_t)at >= (uintptr_t)arena && (uintptr_t)at - (uintptr_t)arena < size) return k;
    }
    return -1;
}

/* The battle slot of index `index` if it holds a bigger model: its arena. */
static int slot_arena(int index)
{
    int k;
    if (index < 0 || index >= 2) return -1;
    k = arena_of((const void *)(uintptr_t)D_800F2C40[index].field_DE0);
    return k < 2 ? k : -1;
}

/* The half of slot k's packet area this frame draws into, from its start. */
static void begin_frame(int k)
{
    if (!begun[k]) {
        begun[k] = 1;
        cursor[k] = (s32)(PACKETS(k) + D_8009AFA2 * HALF);
        refused_draw[k] = 0;
    }
}

void BattleModels_BeginFrame(void) { begun[0] = begun[1] = 0; }

static s32 room(int k)
{
    begin_frame(k);
    return (s32)(PACKETS(k) + D_8009AFA2 * HALF + HALF) - cursor[k];
}

void BattleModels_Parsed(const void *hmd, int packet_bytes, int normals)
{
    int k = hmd ? arena_of(hmd) : -1;
    if (k >= 0) {
        parsed.packets[k] = packet_bytes;
        parsed.normals[k] = normals;
    }
}

void BattleModels_TooManyParts(void) { too_many_parts = 1; }

int BattleModels_Check(int slot)
{
    static ModelSlot keep;
    unsigned size;
    unsigned char *arena = Mods_ModelArena(slot, &size), *check = Mods_ModelArena(2, NULL);
    ModelSlot *p;
    s32 hmd_size, work;
    u32 units;
    int parts, inside, ok;
    if (slot < 0 || slot >= 2 || !arena || !check || !Mods_ModelPending(slot)) return 0;
    /* Parsed in a copy, which the parse rewrites, with the slot as it is. */
    memcpy(check, arena, size);
    hmd_size = *(s32 *)check ? *(s32 *)check : 0xC000;
    if (hmd_size <= 0 || (unsigned)hmd_size >= size) {
        LOG(LOG_MODS, "battle slot %d: a bigger model's data says it is %d bytes, more than its arena; "
                      "it fights as its base", slot, hmd_size);
        return 0;
    }
    /* func_8004CB0C takes 60 units and drops the rest, as a byte. */
    units = *(u32 *)(check + 0xC);
    if (units > 0x3C) {
        LOG(LOG_MODS, "battle slot %d: a bigger model has %u units (60 at most); it fights as its base", slot,
            units);
        return 0;
    }
    p = &D_800F2C40[slot];
    keep = *p;
    too_many_parts = 0;
    parsed.packets[2] = parsed.normals[2] = 0;
    func_8004CB0C(slot, check, hmd_size, -1);
    parts = p->field_E1B;
    /* Its work area, from its type-2 unit on, must be in the arena: the
       slot's draws are told from others' by it (slot_arena). */
    inside = (uintptr_t)p->field_DE0 >= (uintptr_t)check && (uintptr_t)p->field_DE0 - (uintptr_t)check < size;
    work = inside ? (s32)((uintptr_t)p->field_DE0 - (uintptr_t)check) + WORK_PARTS + parsed.normals[2] * 4 : -1;
    *p = keep;
    ok = !too_many_parts && inside && parsed.normals[2] <= 0xFFFF &&
         parsed.packets[2] <= (s32)HALF - MODEL_DATA_MIN_FREE_BYTES && work <= (s32)size;
    if (ok) {
        LOG(LOG_MODS, "battle slot %d: a bigger model, %d bytes of model data, %d parts, %d bytes of packets, "
                      "drawn from its own RAM", slot, hmd_size, parts, parsed.packets[2]);
        drawn[slot] = 0;
    } else
        LOG(LOG_MODS, "battle slot %d: a bigger model does not fit: %s%d parts (%d at most), %d bytes of "
                      "packets (%d at most), %d normals (65535 at most), %s%d bytes with its work area (%u at "
                      "most); it fights as its base", slot, too_many_parts ? "over " : "", parts,
            MODEL_SLOT_PART_COUNT, parsed.packets[2], (int)HALF - MODEL_DATA_MIN_FREE_BYTES, parsed.normals[2],
            inside ? "" : "no type-2 unit in its data, ", work, size);
    too_many_parts = 0;
    return ok;
}

int BattleModels_BeginSort(int index, s32 *saved)
{
    int k = slot_arena(index);
    if (k < 0) return 0;
    if (room(k) - parsed.packets[k] < MODEL_DATA_MIN_FREE_BYTES) {
        if (!refused_draw[k]) LOG(LOG_MODS, "battle slot %d: no room left this frame for its model's packets", k);
        refused_draw[k] = 1;
        return -1;
    }
    *saved = D_800FE240;
    D_800FE240 = cursor[k];
    return k + 1;
}

void BattleModels_EndSort(int begun, s32 saved)
{
    int k = begun - 1;
    if (begun <= 0) return;
    if (!drawn[k] && D_800FE240 > cursor[k]) {   /* once a load, when it is first seen */
        LOG(LOG_MODS, "battle slot %d: %d bytes of packets drawn in its own RAM", k, D_800FE240 - cursor[k]);
        drawn[k] = 1;
    }
    cursor[k] = D_800FE240;
    D_800FE240 = saved;
}

int BattleModels_Insufficient(int index, int type)
{
    int k = slot_arena(index);
    if (k < 0) return -1;
    return room(k) - parsed.packets[k] * (type == 5 ? 2 : 1) < MODEL_DATA_MIN_FREE_BYTES;
}

/* The arenas as the game left them (parsed in place, with absolute pointers
 * the slots in RAM keep), their counts, and which slot's load has not
 * parsed yet: after the windows' chunk, whose Mods_ModelRebind clears that. */
void BattleModels_State(struct MemoriesState *state)
{
    MemoriesStateField fields[4];
    unsigned size;
    fields[0].data = &parsed;
    fields[0].size = sizeof(parsed);
    fields[1].data = Mods_ModelPendings(&size);
    fields[1].size = size;
    fields[2].data = Mods_ModelArena(0, &size);
    fields[2].size = size;
    fields[3].data = Mods_ModelArena(1, &size);
    fields[3].size = size;
    if (!fields[2].data || !fields[3].data) return;
    Memories_StateChunk(state, "mod-arenas", fields, 4);
    if (Memories_StateLoading(state)) BattleModels_BeginFrame();   /* the halves start over */
}
