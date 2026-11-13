#ifndef MEMORIES_PC_OVERLAYS_MODEL_CLAIM_H
#define MEMORIES_PC_OVERLAYS_MODEL_CLAIM_H
#include <stddef.h>

#include "pc/overlays/model_gate_row.h"

/* Who runs an update of a battle model's control module: what took the
 * setup of the attack in progress, or what the slot's verdict picks now
 * (src/pc/overlays/model_modules.c). Pure, so tests/pc/model_claim_test.c
 * runs the very code the game does.
 *
 * An attack's work area belongs to what set it up. The verdict can change
 * while the slot keeps the same words: a mod applied or removed, a row
 * added (mod API 29). Then the attack's updates stay with what set it up:
 *   - C (a mod's or the disc's): the row's copy, kept in the slot;
 *   - the port's own handling (the interpreter, or the stand-in);
 * until the next setup, new words in the slot or a loaded state. A mod's C
 * also keeps updates its bound would not take. The stand-in's claim (it ran
 * the setup, or an update the interpreter failed in) holds every update
 * until the next setup or new words, a loaded state included, as it always
 * has. With no mod row the verdict never changes under the same words, so
 * the result is the stand-in's claim alone, as before mod rows. */

/* The row a verdict picked, as the claim compares it (found 0: none). */
typedef struct {
    int found;
    ModelEntry entry;
    int owner, token;
} ModelChoice;

enum { MODEL_CLAIM_NONE, MODEL_CLAIM_C, MODEL_CLAIM_PORT };

typedef struct {
    int setup;           /* MODEL_CLAIM_*: what took the setup of the attack in progress */
    ModelChoice choice;  /* the verdict's choice then */
    int owner;           /* the C's row's owner (MODEL_CLAIM_C): -1 the disc's */
    int stand_in;        /* the stand-in took the attack: it owns its updates */
    unsigned state_loads;
} ModelClaim;

/* What runs a call. */
typedef enum {
    MODEL_RUN_CHOICE,     /* the verdict's choice now, within its bound */
    MODEL_RUN_CLAIMED_C,  /* the C that took the setup (the slot's copy of its row) */
    MODEL_RUN_PORT,       /* the port's own handling: an attack the port set up */
    MODEL_RUN_STAND_IN    /* the port's own handling: an attack the stand-in took */
} ModelRun;

static inline ModelChoice ModelClaim_Choice(const ModelGateRow *row)
{
    ModelChoice choice = {0, NULL, -1, 0};
    if (row) {
        choice.found = 1;
        choice.entry = row->entry;
        choice.owner = row->owner;
        choice.token = row->token;
    }
    return choice;
}

static inline int ModelClaim_Same(ModelChoice a, ModelChoice b)
{
    if (!a.found || !b.found) {
        return a.found == b.found;
    }
    return a.entry == b.entry && a.owner == b.owner && a.token == b.token;
}

/* New words in the slot: no attack of the old module goes on. */
static inline void ModelClaim_NewWords(ModelClaim *claim)
{
    claim->setup = MODEL_CLAIM_NONE;
    claim->stand_in = 0;
}

/* A call: `setup` (command >= 0) or an update, the verdict's choice `now`,
 * and the count of loaded states. A setup ends every claim. */
static inline ModelRun ModelClaim_Decide(ModelClaim *claim, ModelChoice now, int setup, unsigned state_loads)
{
    if (setup) {
        claim->setup = MODEL_CLAIM_NONE;
        claim->stand_in = 0;
        return MODEL_RUN_CHOICE;
    }
    if (claim->setup != MODEL_CLAIM_NONE && claim->state_loads != state_loads) {
        claim->setup = MODEL_CLAIM_NONE; /* a loaded state: its attack is not the one set up here */
    }
    if (claim->stand_in) {
        return MODEL_RUN_STAND_IN;
    }
    if (claim->setup == MODEL_CLAIM_C && (claim->owner >= 0 || !ModelClaim_Same(claim->choice, now))) {
        return MODEL_RUN_CLAIMED_C;
    }
    if (claim->setup == MODEL_CLAIM_PORT && !ModelClaim_Same(claim->choice, now)) {
        return MODEL_RUN_PORT;
    }
    return MODEL_RUN_CHOICE;
}

/* What took a setup: C (`ran_c`, its row's `owner`) or the port, under the
 * choice `now`. */
static inline void ModelClaim_Took(ModelClaim *claim, ModelChoice now, int ran_c, int owner, unsigned state_loads)
{
    claim->setup = ran_c ? MODEL_CLAIM_C : MODEL_CLAIM_PORT;
    claim->choice = now;
    claim->owner = ran_c ? owner : -1;
    claim->state_loads = state_loads;
}

/* The stand-in ran (at the setup, or at an update the interpreter failed in). */
static inline void ModelClaim_StandIn(ModelClaim *claim) { claim->stand_in = 1; }

#endif
