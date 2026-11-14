/* A code mod's control-module rows (mod API 29) through the loader, for
 * tests/pc/mods_lifecycle_test.c. One object, installed as three mods; its
 * id says which:
 *   rows-ok       registers a row for the module whose 8 bytes are first word
 *                 0x00C0FFEE and `jr ra`, and starts;
 *   rows-refused  registers the same row, then refuses to start: the row goes;
 *   rows-bad      registers a row whose sha256 is not 64 hex digits (token 0,
 *                 the reason in its status), and starts.
 * It also binds a module-range data name the game exports nowhere
 * (D_8013FFF0), which the loader binds to its address. */
#include <stdint.h>
#include <string.h>

#include "types.h"
#include "pc/mods/modapi.h"

extern unsigned char D_8013FFF0[];

static int32_t entry(uint8_t *context, int32_t command)
{
    (void)context;
    return command;
}

int MemoriesModInit(const MemoriesModHost *host, MemoriesMod *mod)
{
    MemoriesModelModule row;
    int token;
    if (host->api < 29) return 0;
    mod->api = 29;
    memset(&row, 0, sizeof(row));
    row.size = sizeof(row);
    row.kind = MEMORIES_MODEL_VARIANT;
    row.slot = MEMORIES_MODEL_SLOT_A;
    row.identifier = 0x00C0FFEEu;
    row.span = 8;
    memcpy(row.sha256, "0af009f3f4aed0d3cb4563ad07d9da732398f4cb971fe1e8384b85e4a6efa1fe", 65);
    if (!strcmp(host->id, "rows-bad")) row.sha256[3] = 'x';
    row.entry = entry;
    token = host->model_module(host, &row);
    host->set_setting(host, "token", token);
    host->set_setting(host, "base_outside", host->model_module_base(host) == 0);
    host->set_setting(host, "module_data", (uintptr_t)D_8013FFF0 == 0x8013FFF0u);
    /* The same in the trace (MEMORIES_TRACE=mods), for a game's own run. */
    host->log(host, "token %d, module_data %d, D_8013FFF0 at 0x%08lX", token,
              (uintptr_t)D_8013FFF0 == 0x8013FFF0u, (unsigned long)(uintptr_t)D_8013FFF0);
    return strcmp(host->id, "rows-refused") != 0;
}
