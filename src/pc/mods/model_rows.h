#ifndef MEMORIES_PC_MODS_MODEL_ROWS_H
#define MEMORIES_PC_MODS_MODEL_ROWS_H
/* The control-module rows code mods register (mod API 29, host->model_module):
 * native C a mod gives for a battle model's control module, which
 * src/pc/overlays/model_modules.c runs where the module's first `span` bytes
 * hash to the row (notes/modding.md, "Your own C for a module (API 29)").
 *
 * Static storage only, never malloc: a mod's entry is host code (above 4 GB
 * in the translated build), kept where no save state reaches, as events.c
 * keeps the mods' callbacks. */
#include <stddef.h>
#include <stdint.h>

#include "pc/mods/modapi.h"
#include "pc/overlays/model_gate_row.h"

/* Rows for all mods together (a SLOT_BOTH registration is two): about 2 MB
 * of static storage, room for four rows of each model of a pack of 4,000. */
#define MODEL_ROWS_MAX 16384

/* A token (both rows of a SLOT_BOTH registration share it), or 0 with the
 * reason in `why`. */
int ModelRows_Add(int owner, const MemoriesModelModule *row, char *why, size_t size);
void ModelRows_Remove(int owner, int token);
void ModelRows_Clear(int owner);
/* Rows registered now, whether or not their mod is applied. */
int ModelRows_Count(void);
/* Bumped by every Add, Remove and Clear that changed something. */
unsigned ModelRows_Generation(void);
/* The applied mods' row for this module, before (over_disc 1) or after (0)
 * the disc's rows: the one whose span hashes to `image`, newest applied
 * first; NULL for none. *longest (when not NULL) gets the longest span of any
 * applied row of this kind, slot and identifier, either flag. The row stays
 * valid until the next Add, Remove or Clear: copy it to keep it. */
const ModelGateRow *ModelRows_Find(int primary, int slot, uint32_t identifier, const void *image, int over_disc,
                                   uint32_t *longest);
/* The slot base of the module whose mod entry runs now (host->model_module_base). */
void ModelRows_SetRunning(uint32_t base);
uint32_t ModelRows_Running(void);
/* A save state was loaded: an attack a mod's C set up is not the state's
 * (model_modules.c compares the count). */
void ModelRows_StateLoaded(void);
unsigned ModelRows_StateLoads(void);

#endif
