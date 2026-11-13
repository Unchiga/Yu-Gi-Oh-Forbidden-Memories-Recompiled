/* src/pc/overlays/model_claim.h: who runs an update of a battle model's
 * control module, the code src/pc/overlays/model_modules.c runs. Each case
 * plays the calls a slot gets (setups, updates, a mod applied or removed in
 * between, a loaded state, new words) and checks what runs each update. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc/overlays/model_claim.h"
#include "pc/overlays/model_gate_command.h"

static int failures;
#define EXPECT(condition)                                                        \
    do {                                                                         \
        if (!(condition)) {                                                      \
            fprintf(stderr, "model_claim_test:%d: %s\n", __LINE__, #condition); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static int disc_entry(void *context, int command) { (void)context; return command; }
static int mod_entry(void *context, int command) { (void)context; return command + 1; }
static int other_mod_entry(void *context, int command) { (void)context; return command + 2; }

static ModelGateRow disc_row, mod_row, over_row;

/* A slot as run_module drives it: `chosen` is the verdict's choice now and
 * `takes` whether its bound takes the call; returns what runs it and, at a
 * setup, records what took it. */
static unsigned loads;
static ModelRun call(ModelClaim *claim, const ModelGateRow *chosen, int setup, int takes)
{
    ModelRun run = ModelClaim_Decide(claim, ModelClaim_Choice(chosen), setup, loads);
    if (setup) {
        int c = chosen && takes;
        ModelClaim_Took(claim, ModelClaim_Choice(chosen), c, c ? chosen->owner : -1, loads);
    }
    return run;
}

/* The rule before mod rows: the stand-in's claim alone (its updates go to
 * the port), every other call to the verdict's choice and its bound. */
static ModelRun old_rule(int stand_in, int setup) { return !setup && stand_in ? MODEL_RUN_STAND_IN : MODEL_RUN_CHOICE; }

int main(void)
{
    ModelClaim claim;
    disc_row = (ModelGateRow)MODEL_GATE_DISC_ROW(0, 54, 0x17BC, "", disc_entry, 6, 0x16D4, 0x26, 0, 0, 0);
    mod_row = disc_row;
    mod_row.entry = mod_entry, mod_row.owner = 3, mod_row.token = 7;
    over_row = mod_row;
    over_row.entry = other_mod_entry, over_row.owner = 5, over_row.token = 9;

    /* A mod applied mid-attack over an interpreter setup: the interpreter
     * (NULL choice) took it; its updates stay with the port though the mod's
     * row (unbound: it would take every call) is now the choice. */
    memset(&claim, 0, sizeof(claim));
    EXPECT(call(&claim, NULL, 1, 0) == MODEL_RUN_CHOICE);
    EXPECT(call(&claim, NULL, 0, 0) == MODEL_RUN_CHOICE);
    EXPECT(call(&claim, &mod_row, 0, 1) == MODEL_RUN_PORT);
    EXPECT(call(&claim, &mod_row, 0, 1) == MODEL_RUN_PORT);
    /* The next setup is the mod's. */
    EXPECT(call(&claim, &mod_row, 1, 1) == MODEL_RUN_CHOICE);
    EXPECT(call(&claim, &mod_row, 0, 1) == MODEL_RUN_CLAIMED_C);

    /* A mod removed mid-attack after its C set up: its C keeps the updates,
     * whatever the choice is now (the interpreter, the disc's C). */
    memset(&claim, 0, sizeof(claim));
    EXPECT(call(&claim, &mod_row, 1, 1) == MODEL_RUN_CHOICE);
    EXPECT(claim.setup == MODEL_CLAIM_C && claim.owner == 3);
    EXPECT(call(&claim, NULL, 0, 0) == MODEL_RUN_CLAIMED_C);
    EXPECT(call(&claim, &disc_row, 0, 1) == MODEL_RUN_CLAIMED_C);
    /* A mod's C keeps an update its own bound would not take (same choice). */
    EXPECT(call(&claim, &mod_row, 0, 0) == MODEL_RUN_CLAIMED_C);

    /* An OVER_DISC row applied over an attack the disc's C set up: the
     * disc's C keeps it. With the choice unchanged the disc's C goes by its
     * bound, as ever (the choice runs). */
    memset(&claim, 0, sizeof(claim));
    EXPECT(call(&claim, &disc_row, 1, 1) == MODEL_RUN_CHOICE);
    EXPECT(claim.setup == MODEL_CLAIM_C && claim.owner == -1);
    EXPECT(call(&claim, &disc_row, 0, 1) == MODEL_RUN_CHOICE);
    EXPECT(call(&claim, &over_row, 0, 1) == MODEL_RUN_CLAIMED_C);
    /* Removed again: the choice is the setup's, the bound decides. */
    EXPECT(call(&claim, &disc_row, 0, 0) == MODEL_RUN_CHOICE);

    /* A `return 2` primary (native_primary, the disc's): its setup taken with
     * no rows, then an OVER_DISC row for it comes mid-attack; and the other
     * way, the mod's row's C took the setup and the rows go. Each attack
     * stays with what set it up. */
    {
        ModelGateRow two = MODEL_GATE_DISC_ROW(0, 0, 12, NULL, disc_entry, MODEL_GATE_NO_TABLE, 0, 0, 0, 0, 0);
        memset(&claim, 0, sizeof(claim));
        EXPECT(call(&claim, &two, 1, 1) == MODEL_RUN_CHOICE);
        EXPECT(call(&claim, &two, 0, 1) == MODEL_RUN_CHOICE);
        EXPECT(call(&claim, &over_row, 0, 1) == MODEL_RUN_CLAIMED_C && claim.owner == -1);
        memset(&claim, 0, sizeof(claim));
        EXPECT(call(&claim, &over_row, 1, 1) == MODEL_RUN_CHOICE);
        EXPECT(call(&claim, &two, 0, 1) == MODEL_RUN_CLAIMED_C && claim.owner == 5);
    }

    /* The stand-in's claim: every update, whatever the choice, until a setup. */
    memset(&claim, 0, sizeof(claim));
    EXPECT(call(&claim, &disc_row, 1, 0) == MODEL_RUN_CHOICE);
    ModelClaim_StandIn(&claim);
    EXPECT(call(&claim, &disc_row, 0, 1) == MODEL_RUN_STAND_IN);
    EXPECT(call(&claim, &mod_row, 0, 1) == MODEL_RUN_STAND_IN);
    loads++;   /* a loaded state does not end it, as before mod rows */
    EXPECT(call(&claim, &disc_row, 0, 1) == MODEL_RUN_STAND_IN);

    /* A new setup ends a claim. */
    EXPECT(call(&claim, &disc_row, 1, 1) == MODEL_RUN_CHOICE);
    EXPECT(!claim.stand_in && claim.setup == MODEL_CLAIM_C);
    EXPECT(call(&claim, &mod_row, 1, 1) == MODEL_RUN_CHOICE);
    EXPECT(claim.owner == 3);
    EXPECT(call(&claim, &mod_row, 0, 1) == MODEL_RUN_CLAIMED_C);

    /* New words end it: an unbound mod row's update then runs the choice. */
    ModelClaim_NewWords(&claim);
    EXPECT(call(&claim, NULL, 0, 0) == MODEL_RUN_CHOICE);
    memset(&claim, 0, sizeof(claim));
    EXPECT(call(&claim, NULL, 1, 0) == MODEL_RUN_CHOICE);
    ModelClaim_StandIn(&claim);
    ModelClaim_NewWords(&claim);
    EXPECT(call(&claim, &mod_row, 0, 1) == MODEL_RUN_CHOICE);

    /* A loaded state ends a setup's claim: the attack is the state's. */
    memset(&claim, 0, sizeof(claim));
    EXPECT(call(&claim, &mod_row, 1, 1) == MODEL_RUN_CHOICE);
    EXPECT(call(&claim, NULL, 0, 0) == MODEL_RUN_CLAIMED_C);
    loads++;
    EXPECT(call(&claim, NULL, 0, 0) == MODEL_RUN_CHOICE);
    EXPECT(call(&claim, &mod_row, 0, 1) == MODEL_RUN_CHOICE);
    memset(&claim, 0, sizeof(claim));
    EXPECT(call(&claim, NULL, 1, 0) == MODEL_RUN_CHOICE);
    loads++;
    EXPECT(call(&claim, &mod_row, 0, 1) == MODEL_RUN_CHOICE);

    /* With no mod rows the choice never changes under the same words: every
     * call runs as the rule before them, for a long run of setups, updates,
     * stand-ins, new words and loaded states, whatever the choice is. */
    {
        const ModelGateRow *choices[2] = {NULL, &disc_row};
        unsigned seed = 12345;
        for (int c = 0; c < 2; c++) {
            int stand_in = 0;
            memset(&claim, 0, sizeof(claim));
            for (int n = 0; n < 20000; n++) {
                int what;
                seed = seed * 1103515245u + 12345u;
                what = (int)((seed >> 16) % 10);
                if (what == 0) {
                    ModelClaim_NewWords(&claim);
                    stand_in = 0;
                } else if (what == 1) {
                    loads++;
                } else if (what == 2) {
                    ModelClaim_StandIn(&claim);
                    stand_in = 1;
                } else {
                    int setup = what < 5, takes = (int)((seed >> 8) & 1);
                    ModelRun run = call(&claim, choices[c], setup, takes);
                    if (run != old_rule(stand_in, setup)) {
                        fprintf(stderr, "model_claim_test: no rows, choice %d, call %d: %d, before %d\n", c, n,
                                (int)run, (int)old_rule(stand_in, setup));
                        failures++;
                        break;
                    }
                    if (setup) stand_in = 0;
                }
            }
        }
    }

    printf("model_claim_test: %d failures\n", failures);
    return failures != 0;
}
