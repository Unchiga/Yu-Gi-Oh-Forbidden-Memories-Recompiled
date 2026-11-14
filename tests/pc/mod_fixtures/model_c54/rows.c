/* The rows that run shape 54's C (slot_a.c, slot_b.c) for the modules this
 * mod's data patched: rows.inc, which tools/pc/test_model_mod_c.py writes from
 * the patched record at run time (no game bytes are kept here). Each row is
 * bound as config/pc/model_gate.csv bounds shape 54: a pointer at +0 of the
 * work area to one of the 6 entries of 0x26 bytes at 0x16D4. */
#include <string.h>

#include "types.h"
#include "pc/mods/modapi.h"

int32_t model_c54_slot_a(uint8_t *context, int32_t command);
int32_t model_c54_slot_b(uint8_t *context, int32_t command);

static int add(const MemoriesModHost *host, uint32_t slot, uint32_t identifier, uint32_t span, const char *sha256)
{
    MemoriesModelModule row;
    memset(&row, 0, sizeof(row));
    row.size = sizeof(row);
    row.kind = MEMORIES_MODEL_VARIANT;
    row.slot = slot;
    row.flags = MEMORIES_MODEL_BOUND;
    row.identifier = identifier;
    row.span = span;
    memcpy(row.sha256, sha256, 65);
    row.entry = slot == MEMORIES_MODEL_SLOT_A ? model_c54_slot_a : model_c54_slot_b;
    row.entries = 6;
    row.table = 0x16D4;
    row.stride = 0x26;
    row.field = 0;
    row.index_width = 0;
    return host->model_module(host, &row) != 0;
}

int MemoriesModInit(const MemoriesModHost *host, MemoriesMod *mod)
{
    if (host->api < 29) return 0;
    mod->api = 29;
#define MODEL_C54_ROW(slot, identifier, span, sha256) \
    if (!add(host, slot, identifier, span, sha256)) return 0;
#include "rows.inc"
#undef MODEL_C54_ROW
    return 1;
}
