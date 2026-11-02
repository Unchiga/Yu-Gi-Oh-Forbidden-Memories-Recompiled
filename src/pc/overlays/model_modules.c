#include "pc/compat/fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/model_graphics_state.h"
#include "game/model_spark_effect.h"
#include "game/model_slot_properties.h"
#include "pc/guest/mips.h"
#include "pc/guest/sha256.h"
#include "pc/debug/log.h"

/* Each MODEL.MRG record carries two small MIPS control modules that the
 * loader swaps into fixed slots: a primary module (slot A 0x8013A000, slot B
 * 0x8017A000; in every record seen it is `return 2`) and a variant module
 * (0x8013B000 / 0x8017B000) holding the monster's attack choreography and
 * impact particles. The game calls each through the word at +4. There are
 * 1,181 distinct variant modules, so they are not translated one by one:
 * the default runs the loaded module through the interpreter, which keeps
 * every monster's own attack movement. A module the interpreter cannot run
 * (reported once per loaded module) falls back to the stand-in below.
 *
 * The decomp's C for a code shape runs in its place where the table says so
 * (config/pc/model_gate.csv, written into model_gate_table.h by
 * tools/pc/model_gate.py): when the module in the slot is that shape's
 * retail code and constants, the first `span` bytes of its image by SHA-256.
 * What follows is the record's own data, which the C reads where the disc
 * put it, so a mod that changes only a model's tables or textures still runs
 * the C, and one that changes the code gets the interpreter.
 *
 * MEMORIES_MODEL_MODULES picks:
 *   unset, `native`  the default: the decomp's C for the shapes the table
 *                    ports, the interpreter for the rest;
 *   `interpreter`    every module interpreted, even one native C exists for:
 *                    the reference tools/pc/model_oracle.py compares against;
 *   `spark`          the stand-in for every module (the resident spark burst,
 *                    called `native` before native C was planned).
 * Anything else is the default, as MEMORIES_DUEL_EFFECTS takes its values,
 * and says so once on stderr. */

enum { MODE_DEFAULT, MODE_INTERPRETER, MODE_SPARK };

static int model_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *setting = getenv("MEMORIES_MODEL_MODULES");
        mode = !setting ? MODE_DEFAULT
             : strcmp(setting, "interpreter") == 0 ? MODE_INTERPRETER
             : strcmp(setting, "spark") == 0 ? MODE_SPARK
             : MODE_DEFAULT;
        if (setting && *setting && mode == MODE_DEFAULT && strcmp(setting, "native") != 0) {
            fprintf(stderr, "memories-pc: MEMORIES_MODEL_MODULES=%s is not native, interpreter or spark; "
                    "using the default\n", setting);
        }
    }
    return mode;
}

typedef struct {
    ModelSparkEffect effect;
    int age;
} MemoriesModelImpact;

static int native_primary(void *context, int command)
{
    (void)context;
    (void)command;
    return 2;
}

/* The bring-up stand-in for a variant module: the resident spark burst on
 * the defender in place of the monster's own choreography. Effects follow
 * Model_GetActiveSlotIndex(), so it points at the defender while the burst
 * is initialized and drawn. */
static int native_variant(void *context, int command)
{
    MemoriesModelImpact *impact = context;
    unsigned char attacker = D_8009AFA0;
    int i;
    int result;

    D_8009AFA0 = attacker ^ 1;
    if (command >= 0) {
        impact->age = 0;
        result = func_8006F1B4(&impact->effect, command);
        /* Only the compact spark phase: the full presentation grows white
         * quads until they cover the monster, which is not a contact hit. */
        impact->effect.spark_count = 2;
        impact->effect.flash_count = 0;
        impact->effect.mode = 1;
        for (i = 0; i < impact->effect.spark_count; i++) {
            impact->effect.sizes[i].size = 1;
            impact->effect.sizes[i].grow = 8;
        }
    } else {
        if (impact->age < 6) {
            result = func_8006F1B4(&impact->effect, command);
            impact->age++;
            /* func_8006F1B4 fades its subject to black as part of a longer
             * presentation; an impact must leave the defender visible until
             * its own dying animation. */
            Model_SetSlotTintTarget(D_8009AFA0, 0, 0x80, 0x80, 0x80);
            /* 3 starts the losing monster's dying animation: after the last
             * impact frame, then complete on the next update. */
            if (impact->age == 6) {
                result = 3;
            }
        } else {
            result = 2;
        }
    }
    D_8009AFA0 = attacker;
    return result;
}

/* The variant modules the decomp's C stands in for: the table's native rows
 * (MODEL_GATE_ROW, model_gate_table.h), each the entry of the C built for its
 * slot, which the build links under the module's prefix (GATED_MODULES). */
typedef int (*ModelEntry)(void *context, int command);

#define MODEL_GATE_ROW(slot, identifier, span, sha256, entry) extern int entry(void *context, int command);
#include "pc/overlays/model_gate_table.h"
#undef MODEL_GATE_ROW

typedef struct {
    int slot;
    uint32_t identifier; /* the first word of the image */
    uint32_t span;       /* the bytes from the slot base the hash covers */
    const char *sha256;
    ModelEntry entry;
} ModelGateRow;

static const ModelGateRow gate_rows[] = {
#define MODEL_GATE_ROW(slot, identifier, span, sha256, entry) {slot, identifier, span, sha256, entry},
#include "pc/overlays/model_gate_table.h"
#undef MODEL_GATE_ROW
    {0, 0, 0, NULL, NULL}};

#define IMAGE_WORDS (0x5000u / 4)

/* What a slot's verdict was taken from: while the slot still holds those
 * words the verdict stands. Comparing them costs less than hashing, and it
 * catches every way a module arrives (a disc delivery, a mod's copy, a
 * state) since none has to say so. Word loops, not memcmp: in the translated
 * build each guest access is resolved. A module no row can stand in for is
 * compared by its identifier word alone. */
typedef struct {
    uint32_t words[IMAGE_WORDS];
    uint32_t span; /* the words compared; 0: no verdict yet */
    ModelEntry entry;
} ModelVerdict;

static ModelVerdict verdicts[2];

static int hex_equal(const uint8_t digest[32], const char *hex)
{
    char text[65];
    unsigned i;
    for (i = 0; i < 32; i++) {
        snprintf(text + 2 * i, 3, "%02x", digest[i]);
    }
    return strcmp(text, hex) == 0;
}

/* The decomp's C for the variant module in a slot, NULL to interpret it. */
static ModelEntry native_variant_entry(uint32_t module, int slot, uint32_t identifier)
{
    ModelVerdict *verdict = &verdicts[slot];
    const uint32_t *guest = (const uint32_t *)(uintptr_t)module;
    const ModelGateRow *row;
    uint32_t i, span = 0;
    ModelEntry entry = NULL;

    if (verdict->span) {
        for (i = 0; i < verdict->span / 4 && guest[i] == verdict->words[i]; i++) {
        }
        if (i == verdict->span / 4) {
            return verdict->entry;
        }
    }
    for (row = gate_rows; row->sha256; row++) {
        uint8_t digest[32];
        if (row->slot != slot || row->identifier != identifier) {
            continue;
        }
        if (row->span > span) {
            span = row->span;
        }
        Sha256(guest, row->span, digest);
        if (hex_equal(digest, row->sha256)) {
            entry = row->entry;
            span = row->span;
            break;
        }
    }
    /* The verdict covers the identifier's longest span, so that the next call
     * compares what it was taken from. */
    verdict->span = span ? span : 4;
    verdict->entry = entry;
    for (i = 0; i < verdict->span / 4; i++) {
        verdict->words[i] = guest[i];
    }
    LOG(LOG_MODS, "model variant module %u in slot %c: %s", (unsigned)identifier, slot ? 'B' : 'A',
        entry ? "retail code, native C" : span ? "changed code, interpreter" : "interpreter");
    return entry;
}

/* module: the slot base (the identifier word; the entry is at +4). The
 * trace names the module by its slot and identifier, and what runs it:
 * "native" (the decomp's C), "interpreter" or "stand-in". */
static int run_module(uint32_t module, int variant, void *context, int command)
{
    static uint32_t failed[2][2]; /* [slot][variant]: identifier of a module that failed */
    int slot = module >= 0x8017A000u;
    uint32_t identifier = *(const uint32_t *)(uintptr_t)module;
    uint32_t entry = *(const uint32_t *)(uintptr_t)(module + 4);
    int mode = model_mode();
    int interpret = mode != MODE_SPARK && entry != 0 && failed[slot][variant] != identifier;
    ModelEntry native = variant && mode == MODE_DEFAULT && entry != 0 ? native_variant_entry(module, slot, identifier) : NULL;

    LOG(LOG_MODEL, "%s slot=%u command=%d module %u in %c: %s", variant ? "variant" : "primary",
        (unsigned)D_8009AFA0, command, (unsigned)identifier, slot ? 'B' : 'A',
        native ? "native" : interpret ? "interpreter" : "stand-in");
    if (native) {
        return native(context, command);
    }
    if (interpret) {
        uint32_t args[2], result;
        args[0] = (uint32_t)(uintptr_t)context;
        args[1] = (uint32_t)command;
        Memories_MipsAttribute(identifier);
        if (!Memories_MipsTry(module + 4, args, 2, &result)) {
            return (int)result;
        }
        fprintf(stderr, "memories-pc: model %s module %u (slot %c) cannot run; using the stand-in\n",
                variant ? "variant" : "primary", (unsigned)identifier, slot ? 'B' : 'A');
        failed[slot][variant] = identifier;
    }
    return variant ? native_variant(context, command) : native_primary(context, command);
}

int Memories_ModelPrimaryControlA(void *context, int command) { return run_module(0x8013A000u, 0, context, command); }
int Memories_ModelVariantControlA(void *context, int command) { return run_module(0x8013B000u, 1, context, command); }
int Memories_ModelPrimaryControlB(void *context, int command) { return run_module(0x8017A000u, 0, context, command); }
int Memories_ModelVariantControlB(void *context, int command) { return run_module(0x8017B000u, 1, context, command); }
