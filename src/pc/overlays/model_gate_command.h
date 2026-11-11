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
 *   setup (command >= 0): the command picks entry `command`, which must be
 *   below `entries`;
 *   update (command < 0): what the setup left in the work area must still
 *   name a covered entry, which is also how an attack the interpreter set up
 *   (a command past the table) stays with the interpreter, and a state loaded
 *   in the middle of one. The setup leaves it in one of two forms:
 *
 *     index_width == 0: a pointer to the entry the setup picked
 *       (`work->config = &D_...[command]`); it must be one of the `entries`
 *       entries of `stride` bytes from `table`, an exact entry address;
 *     index_width 1, 2 or 4: the command itself, stored as a u8, u16 or u32
 *       (`work->command = command`, which every update then indexes the table
 *       with); `entry_word` is that member read as its width, and it must be
 *       below `entries`. A narrow member truncates a setup command the
 *       interpreter took past its range, but a truncated index is accepted
 *       only when it is itself a covered entry, so the table read stays
 *       inside the hash.
 *
 * An index is weaker evidence than a pointer: any small number in the member
 * passes, a work area of zeros reads as index 0, and nothing the stand-in
 * leaves there can be told from a native setup's. So the work area is not the
 * only witness. `stand_in` says the stand-in ran in the attack in progress,
 * at its setup or at an update the interpreter failed in (model_modules.c
 * keeps it for the slot until the next setup), and then no update goes
 * native, whatever the work area holds: the stand-in's work area is another
 * structure (its first bytes are random sparks) and the native C would run
 * on it. Where an update's provenance otherwise comes from is model_control.c:
 * a model's state runs the setup before its first update.
 *
 * entries == MODEL_GATE_NO_TABLE: the C takes no command index (the stand-in
 * rule still holds). */
#define MODEL_GATE_NO_TABLE 0xFFFFFFFFu

static inline int ModelGate_CommandCovered(uint32_t entries, uint32_t table, uint32_t stride, uint32_t base,
                                           int command, uint32_t entry_word, uint32_t index_width, int stand_in)
{
    uint32_t first = base + table;
    if (command < 0 && stand_in) {
        return 0;
    }
    if (entries == MODEL_GATE_NO_TABLE) {
        return 1;
    }
    if (command >= 0) {
        return (uint32_t)command < entries;
    }
    if (index_width != 0) {
        uint32_t mask = index_width >= 4 ? 0xFFFFFFFFu : (1u << (8 * index_width)) - 1u;
        return (entry_word & mask) < entries;
    }
    return entry_word >= first && entry_word - first < entries * stride && (entry_word - first) % stride == 0;
}

/* A primary module's row may pick its entry with command % modulus
 * (config/pc/model_primary_gate.csv's `modN`: `effect` takes entry command %
 * 100 and waits for animation command / 100 + 3), so a setup is covered when
 * that entry is; modulus 0 is ModelGate_CommandCovered. The updates read the
 * work area as for any row. */
static inline int ModelGate_CommandCoveredModulo(uint32_t entries, uint32_t table, uint32_t stride, uint32_t base,
                                                 int command, uint32_t entry_word, uint32_t index_width,
                                                 int stand_in, uint32_t modulus)
{
    if (modulus != 0 && command >= 0) {
        command = (int)((uint32_t)command % modulus);
    }
    return ModelGate_CommandCovered(entries, table, stride, base, command, entry_word, index_width, stand_in);
}

#endif
