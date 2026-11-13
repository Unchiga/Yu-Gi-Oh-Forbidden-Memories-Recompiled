#ifndef MEMORIES_PC_OVERLAYS_MODEL_GATE_ROW_H
#define MEMORIES_PC_OVERLAYS_MODEL_GATE_ROW_H
#include <stdint.h>

/* A control module that native C runs in place of the MIPS interpreter
 * (src/pc/overlays/model_modules.c): one of the disc's rows
 * (config/pc/model_gate.csv and model_primary_gate.csv, MODEL_GATE_ROW and
 * MODEL_PRIMARY_ROW) or one a code mod registered (API 29,
 * src/pc/mods/model_rows.h). */
typedef int (*ModelEntry)(void *context, int command);

typedef struct {
    int slot;
    uint32_t identifier; /* the first word of the image */
    uint32_t span;       /* the bytes from the slot base the hash covers */
    const char *sha256;
    ModelEntry entry;
    uint32_t entries, table, stride, field, index_width; /* the command bound: model_gate_command.h */
    uint32_t modulus;    /* the setup picks entry command % modulus (0: command) */
    int owner;           /* -1 for a disc row, else the mod's index */
    int token;           /* the mod's token for it (host->model_module); 0 for a disc row */
} ModelGateRow;

/* A disc row's initializer, from the tables' MODEL_GATE_ROW / MODEL_PRIMARY_ROW lines. */
#define MODEL_GATE_DISC_ROW(slot, identifier, span, sha256, entry, entries, table, stride, field, index_width, \
                            modulus)                                                                         \
    {slot, identifier, span, sha256, entry, entries, table, stride, field, index_width, modulus, -1, 0}

#endif
