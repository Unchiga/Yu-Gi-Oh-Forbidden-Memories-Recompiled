/* A control module that is only C (mod API 29), registered once for both
 * slots (MEMORIES_MODEL_SLOT_BOTH), for tools/pc/test_model_mod_c.py. The
 * mod's data puts the same 56 bytes at the start of a record's variant
 * module in both slots: first word 0x00C54B07, then the smallest attack
 * that ends, as MIPS (what the game runs with the mod off):
 *
 *     bltz  a1, update      ; a setup:
 *     li    v0, 0
 *     jr    ra              ;   return 0, the work area's first byte 0
 *     sb    zero, 0(a0)
 *   update:
 *     lbu   v0, 0(a0)
 *     nop
 *     bnez  v0, done
 *     li    v1, 1
 *     sb    v1, 0(a0)       ; the first update: 3, the defender's dying
 *     jr    ra              ;   animation starts
 *     li    v0, 3
 *   done:
 *     jr    ra              ; then 2: the attack is over
 *     li    v0, 2
 *
 * (A variant that only returns 2 never ends its attack: the battle waits.)
 * The C does the same, and logs the slot base model_module_base gives and
 * the byte it reads there, the module's first, so the test sees which slot
 * each call ran for. */
#include <stdint.h>
#include <string.h>

#include "types.h"
#include "pc/mods/modapi.h"

static const MemoriesModHost *host;

static int32_t entry(uint8_t *context, int32_t command)
{
    uint32_t base = host->model_module_base(host);
    const volatile uint8_t *first = (const volatile uint8_t *)(uintptr_t)base;
    host->log(host, "slot base 0x%08X command %d first byte 0x%02X", (unsigned)base, (int)command,
              base ? (unsigned)first[0] : 0u);
    if (command >= 0) {
        context[0] = 0;
        return 0;
    }
    if (!context[0]) {
        context[0] = 1;
        return 3;
    }
    return 2;
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    MemoriesModelModule row;
    host = from;
    if (host->api < 29) return 0;
    mod->api = 29;
    memset(&row, 0, sizeof(row));
    row.size = sizeof(row);
    row.kind = MEMORIES_MODEL_VARIANT;
    row.slot = MEMORIES_MODEL_SLOT_BOTH;
    row.identifier = 0x00C54B07u;
    row.span = 56;
    memcpy(row.sha256, "96c498af438dc0488d0e8080cbad4811da82d0363c9c3bebfa8407381bf88bf3", 65);
    row.entry = entry;
    return host->model_module(host, &row) != 0;
}
