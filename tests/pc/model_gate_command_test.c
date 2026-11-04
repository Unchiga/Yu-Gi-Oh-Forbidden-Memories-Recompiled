#include "pc/overlays/model_gate_command.h"
#include <assert.h>
#include <stdio.h>

/* Shape 418's row: 18 entries of 0x44 bytes from 0x3A68 in the slot at 0x8013B000. */
#define COVERED(command, pointer) ModelGate_CommandCovered(18u, 0x3A68u, 0x44u, 0x8013B000u, (command), (pointer))

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
    assert(ModelGate_CommandCovered(MODEL_GATE_NO_TABLE, 0, 0, 0x8013B000u, 12345, 0));
    assert(ModelGate_CommandCovered(MODEL_GATE_NO_TABLE, 0, 0, 0x8013B000u, -1, 0));
    puts("model gate command bounds ok");
    return 0;
}
