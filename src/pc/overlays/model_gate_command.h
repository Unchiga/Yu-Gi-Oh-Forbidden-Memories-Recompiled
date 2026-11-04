#ifndef MEMORIES_PC_OVERLAYS_MODEL_GATE_COMMAND_H
#define MEMORIES_PC_OVERLAYS_MODEL_GATE_COMMAND_H
#include <stdint.h>

/* A native variant module may index the slot's data by the command its
 * caller passes (model_control.c, model_load_step.c: a word of the record,
 * % 1000, which a MODEL.MRG mod controls), and the hash of the module's
 * image covers only the entries the retail table has. config/pc/model_gate.csv
 * says how many (command_table), and a call the hash does not cover goes to
 * the interpreter, which survives what native C would fault on.
 *
 *   setup (command >= 0): the command picks entry `command`;
 *   update (command < 0): the entry the setup picked is the pointer the C
 *   keeps in the work area, so it must be one of the covered entries, which
 *   is also how an attack the interpreter set up (a command past the table)
 *   stays with the interpreter, and a state loaded in the middle of one.
 *
 * entries == MODEL_GATE_NO_TABLE: the C takes no command index. */
#define MODEL_GATE_NO_TABLE 0xFFFFFFFFu

static inline int ModelGate_CommandCovered(uint32_t entries, uint32_t table, uint32_t stride, uint32_t base,
                                           int command, uint32_t entry_pointer)
{
    uint32_t first = base + table;
    if (entries == MODEL_GATE_NO_TABLE) {
        return 1;
    }
    if (command >= 0) {
        return (uint32_t)command < entries;
    }
    return entry_pointer >= first && entry_pointer - first < entries * stride && (entry_pointer - first) % stride == 0;
}

#endif
