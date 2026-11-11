#include "pc/overlays/model_gate_command.h"
#include <assert.h>
#include <stdio.h>

/* Shape 418's row: 18 entries of 0x44 bytes from 0x3A68 in the slot at 0x8013B000. */
#define COVERED(command, pointer) ModelGate_CommandCovered(18u, 0x3A68u, 0x44u, 0x8013B000u, (command), (pointer), 0u, 0)

/* An index row (the work area keeps the command itself): 3 entries, of a given width. */
#define INDEXED(command, word, width) \
    ModelGate_CommandCovered(3u, 0x1F9Cu, 0x28u, 0x8013B000u, (command), (word), (width), 0)

int main(void)
{
    uint32_t first = 0x8013B000u + 0x3A68u, last = first + 17u * 0x44u;
    /* Setups: the retail commands 0..17, not 18 and beyond, and not the
     * remainder of a negative request. */
    assert(COVERED(0, 0) && COVERED(17, 0));
    assert(!COVERED(18, 0) && !COVERED(19, 0) && !COVERED(999, 0));
    /* Updates follow the entry the setup picked. */
    assert(COVERED(-1, first) && COVERED(-1, last));
    assert(!COVERED(-1, last + 0x44u));   /* entry 18: the setup ran in the interpreter */
    assert(!COVERED(-1, first - 0x44u));
    assert(!COVERED(-1, first + 4u));     /* inside an entry, not at its start */
    assert(!COVERED(-1, 0));              /* no setup: the pointer is unset */
    assert(!COVERED(-1, 0xFFFFFFFFu));
    /* A C with no command index is never bounded. */
    assert(ModelGate_CommandCovered(MODEL_GATE_NO_TABLE, 0, 0, 0x8013B000u, 12345, 0, 0, 0));
    assert(ModelGate_CommandCovered(MODEL_GATE_NO_TABLE, 0, 0, 0x8013B000u, -1, 0, 0, 0));

    /* Index rows: setups are bounded as for a pointer row... */
    assert(INDEXED(0, 0, 1) && INDEXED(2, 0, 1));
    assert(!INDEXED(3, 0, 1) && !INDEXED(999, 0, 2));
    /* ...and an update follows the index the setup stored, as an unsigned value. */
    assert(INDEXED(-1, 0, 1) && INDEXED(-1, 2, 1));   /* in range */
    assert(!INDEXED(-1, 3, 1));                       /* == entries: the interpreter's setup */
    assert(!INDEXED(-1, 4, 1) && !INDEXED(-1, 100, 4));
    assert(!INDEXED(-1, 0xFFu, 1));
    assert(!INDEXED(-1, 0xFFFFu, 2));
    assert(!INDEXED(-1, 0xFFFFFFFFu, 4));
    /* Only the member's width counts: the bytes beside a u8 or u16 are not its value. */
    assert(INDEXED(-1, 0x7F00u | 1u, 1) == 1 && !INDEXED(-1, 0x7F00u | 0xFFu, 1));
    assert(INDEXED(-1, 0x10000u | 2u, 2) == 1 && !INDEXED(-1, 0x10000u | 3u, 2));
    assert(!INDEXED(-1, 0x10000u | 2u, 4));           /* a u32 index reads all four bytes */
    /* A u8 index truncates a setup command >= 256 the interpreter took: the
     * truncated value (300 -> 44) is rejected unless it is a covered entry (258 -> 2). */
    assert(!INDEXED(-1, 300u & 0xFFu, 1));
    assert(INDEXED(-1, 258u & 0xFFu, 1));

    /* The stand-in took the attack's setup: its work area is another structure,
     * so no update goes native whatever it holds (an index in range by chance: the
     * stand-in's first byte is random), in every kind of row. */
    assert(ModelGate_CommandCovered(3u, 0x1F9Cu, 0x28u, 0x8013B000u, -1, 0, 1, 0));
    assert(!ModelGate_CommandCovered(3u, 0x1F9Cu, 0x28u, 0x8013B000u, -1, 0, 1, 1));
    assert(!ModelGate_CommandCovered(3u, 0x1F9Cu, 0x28u, 0x8013B000u, -1, 2, 4, 1));
    assert(!ModelGate_CommandCovered(18u, 0x3A68u, 0x44u, 0x8013B000u, -1, first, 0, 1));
    assert(!ModelGate_CommandCovered(MODEL_GATE_NO_TABLE, 0, 0, 0x8013B000u, -1, 0, 0, 1));
    /* and it does not touch a setup: the next attack's setup is judged on its command. */
    assert(ModelGate_CommandCovered(3u, 0x1F9Cu, 0x28u, 0x8013B000u, 2, 0, 1, 1));
    assert(!ModelGate_CommandCovered(3u, 0x1F9Cu, 0x28u, 0x8013B000u, 3, 0, 1, 1));
    /* A primary's effect (model_primary_gate.csv, mod100) picks entry command
     * % 100 of its 3: the retail commands 0, 1, 101 and 102 are covered, 103
     * is not, and without the modulus 101 is past the table. */
    assert(ModelGate_CommandCoveredModulo(3u, 0xDCu, 0x10u, 0x8013A000u, 0, 0, 0, 0, 100u));
    assert(ModelGate_CommandCoveredModulo(3u, 0xDCu, 0x10u, 0x8013A000u, 101, 0, 0, 0, 100u));
    assert(ModelGate_CommandCoveredModulo(3u, 0xDCu, 0x10u, 0x8013A000u, 102, 0, 0, 0, 100u));
    assert(!ModelGate_CommandCoveredModulo(3u, 0xDCu, 0x10u, 0x8013A000u, 103, 0, 0, 0, 100u));
    assert(!ModelGate_CommandCoveredModulo(3u, 0xDCu, 0x10u, 0x8013A000u, 101, 0, 0, 0, 0u));
    /* Its updates read the entry pointer as any row's. */
    assert(ModelGate_CommandCoveredModulo(3u, 0xDCu, 0x10u, 0x8013A000u, -1, 0x8013A0ECu, 0, 0, 100u));
    assert(!ModelGate_CommandCoveredModulo(3u, 0xDCu, 0x10u, 0x8013A000u, -1, 0x8013A10Cu, 0, 0, 100u));
    puts("model gate command bounds ok");
    return 0;
}
