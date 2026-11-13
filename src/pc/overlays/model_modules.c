#include "pc/compat/fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/model_graphics_state.h"
#include "game/model_spark_effect.h"
#include "game/model_slot_properties.h"
#include "pc/guest/mips.h"
#include "pc/guest/sha256.h"
#include "pc/mods/model_rows.h"
#include "pc/mods/mods.h"
#include "pc/overlays/model_gate_command.h"
#include "pc/overlays/model_gate_row.h"
#include "pc/overlays/model_module_class.h"
#include "pc/debug/log.h"

/* Each MODEL.MRG record carries two small MIPS control modules that the
 * loader swaps into fixed slots: a primary module (slot A 0x8013A000, slot B
 * 0x8017A000; `return 2` in all but ten records, and the disc calls only
 * those ten's) and a variant module (0x8013B000 / 0x8017B000) holding the
 * monster's attack choreography and impact particles. The game calls each
 * through the word at +4. A module no native C stands in for runs through
 * the interpreter, which keeps every monster's own attack movement; one the
 * interpreter cannot run (reported once per loaded module) falls back to the
 * stand-in below.
 *
 * The decomp's C for a code shape runs in its place where the tables say so
 * (config/pc/model_gate.csv for the variant modules and
 * config/pc/model_primary_gate.csv for the primary ones, written into
 * model_gate_table.inc and model_primary_gate_table.inc by
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
 * and says so once on stderr.
 *
 * Code mods may give C of their own for a module (mod API 29,
 * src/pc/mods/model_rows.h): in the default mode a mod's row whose span hashes
 * to the module runs it, after the disc's rows, or before them with
 * MEMORIES_MODEL_OVER_DISC. MEMORIES_MODEL_MOD_C=0 leaves the mods' rows out
 * (the disc's C still runs), to tell a mod's C from its data. */

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

/* Whether the mods' rows may run (MEMORIES_MODEL_MOD_C, unset or 1: yes). */
static int mod_c(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *setting = getenv("MEMORIES_MODEL_MOD_C");
        enabled = !(setting && strcmp(setting, "0") == 0);
    }
    return enabled;
}

#ifdef MEMORIES_TEST_HOOKS
/* MEMORIES_MODEL_STACK_POISON=<byte> (tools/pc/model_oracle.py --poison, a
 * build with test hooks): the stack below run_module's frame is filled with
 * the byte before a native module runs, so a module's C that reads a local it
 * never wrote (as 84's flags[] and 128's quad did, before rule 6) takes the byte, where the
 * interpreter takes what its own stack held. Run with two bytes, such a shape
 * differs from the interpreter for one of them; the scan does not see it. */
static int stack_poison(void)
{
    static int value = -2;
    if (value == -2) {
        const char *setting = getenv("MEMORIES_MODEL_STACK_POISON");
        value = setting && *setting ? (int)(strtol(setting, NULL, 0) & 0xFF) : -1;
    }
    return value;
}

/* Not inlined: its frame is where the module's frames will be. */
static __attribute__((noinline)) void paint_stack(int byte)
{
    volatile unsigned char area[0x8000];
    size_t i;
    for (i = 0; i < sizeof(area); i++) area[i] = (unsigned char)byte;
}
#endif

typedef struct {
    ModelSparkEffect effect;
    int age;
} MemoriesModelImpact;

/* A primary module that is `return 2` (jr ra; addiu v0, zero, 2 at +4): the
 * disc's in all but ten records. native_primary answers it as the module
 * would, and with no side effect to keep it is the native C of all of them. */
static int primary_returns_two(uint32_t module)
{
    const uint32_t *code = (const uint32_t *)(uintptr_t)(module + 4);
    return code[0] == 0x03E00008u && code[1] == 0x24020002u;
}

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
 * (MODEL_GATE_ROW, model_gate_table.inc), each the entry of the C built for its
 * slot, which the build links under the module's prefix (GATED_MODULES). The
 * row type (ModelGateRow) is shared with the mods' rows: model_gate_row.h. */
#define MODEL_GATE_ROW(slot, identifier, span, sha256, entry, entries, table, stride, field, index_width, modulus) \
    extern int entry(void *context, int command);
#include "pc/overlays/model_gate_table.inc"
#undef MODEL_GATE_ROW

static const ModelGateRow gate_rows[] = {
#define MODEL_GATE_ROW(slot, identifier, span, sha256, entry, entries, table, stride, field, index_width, modulus) \
    MODEL_GATE_DISC_ROW(slot, identifier, span, sha256, entry, entries, table, stride, field, index_width, modulus),
#include "pc/overlays/model_gate_table.inc"
#undef MODEL_GATE_ROW
    MODEL_GATE_DISC_ROW(0, 0, 0, NULL, NULL, 0, 0, 0, 0, 0, 0)};

/* The primary modules the decomp's C stands in for (MODEL_PRIMARY_ROW,
 * model_primary_gate_table.inc, from config/pc/model_primary_gate.csv): the
 * disc calls only the ten records' with code, of five shapes; the others are
 * `return 2`, which native_primary answers as the stand-in. */
#define MODEL_PRIMARY_ROW(slot, identifier, span, sha256, entry, entries, table, stride, field, index_width, modulus) \
    extern int entry(void *context, int command);
#include "pc/overlays/model_primary_gate_table.inc"
#undef MODEL_PRIMARY_ROW

static const ModelGateRow primary_gate_rows[] = {
#define MODEL_PRIMARY_ROW(slot, identifier, span, sha256, entry, entries, table, stride, field, index_width, modulus) \
    MODEL_GATE_DISC_ROW(slot, identifier, span, sha256, entry, entries, table, stride, field, index_width, modulus),
#include "pc/overlays/model_primary_gate_table.inc"
#undef MODEL_PRIMARY_ROW
    MODEL_GATE_DISC_ROW(0, 0, 0, NULL, NULL, 0, 0, 0, 0, 0, 0)};

/* A primary that is `return 2`, as a row: what native_row picks for one once
 * mods have rows (with none, run_module answers it before any verdict, as it
 * always has). */
static const ModelGateRow return_two_row =
    MODEL_GATE_DISC_ROW(0, 0, 12, NULL, native_primary, MODEL_GATE_NO_TABLE, 0, 0, 0, 0, 0);

#define IMAGE_WORDS (0x5000u / 4)

/* What a slot's verdict was taken from: while the slot still holds those
 * words the verdict stands. Comparing them costs less than hashing, and it
 * catches every way a module arrives (a disc delivery, a mod's copy, a
 * state) since none has to say so. Word loops, not memcmp: in the translated
 * build each guest access is resolved. A module no row can stand in for is
 * compared by its identifier word alone. With mods' rows (mod API 29) the
 * verdict also holds only while the rows and the applied mods are the ones it
 * was taken with. */
typedef struct {
    uint32_t words[IMAGE_WORDS];
    uint32_t span; /* the bytes compared, in words rounded up; 0: no verdict yet */
    int found;               /* chosen holds the row they hash to; 0: interpret */
    ModelGateRow chosen;     /* a copy: a mod's row may go while the verdict stands */
    int outside_logged;      /* a call outside the row's commands was logged for these words */
    int stand_in;            /* the stand-in took the setup of the attack in progress: it owns its updates */
    unsigned rows_generation, mods_changes; /* ModelRows_Generation, Mods_Changes (0, 0 with no rows) */
    /* A mod's C took the setup of the attack in progress (attack_row, its
     * row): that attack's updates stay with that C whatever the verdict says
     * now, until the next setup, a new module or a loaded state. */
    int attack_claimed;
    ModelGateRow attack_row;
    unsigned state_loads;    /* ModelRows_StateLoads when attack_claimed was set */
} ModelVerdict;

static ModelVerdict verdicts[2], primary_verdicts[2]; /* [slot], for the variant and the primary module */

/* What a verdict depends on beyond the slot's words: the mods' rows and
 * which mods are applied, both 0 while no mod has a row. */
static void rows_state(unsigned *generation, unsigned *changes)
{
    int rows = mod_c() && ModelRows_Count() > 0;
    *generation = rows ? ModelRows_Generation() : 0;
    *changes = rows ? Mods_Changes() : 0;
}

static int hex_equal(const uint8_t digest[32], const char *hex)
{
    char text[65];
    unsigned i;
    for (i = 0; i < 32; i++) {
        snprintf(text + 2 * i, 3, "%02x", digest[i]);
    }
    return strcmp(text, hex) == 0;
}

/* The native row for the variant (or primary) module in a slot, NULL to interpret it. */
static const ModelGateRow *native_row(int primary, uint32_t module, int slot, uint32_t identifier)
{
    ModelVerdict *verdict = primary ? &primary_verdicts[slot] : &verdicts[slot];
    const uint32_t *guest = (const uint32_t *)(uintptr_t)module;
    const ModelGateRow *row;
    uint32_t i, span = 0, longest = 0, longest_after = 0;
    const ModelGateRow *found = NULL;
    unsigned generation, changes;
    int same = 0, mods;

    rows_state(&generation, &changes);
    mods = generation != 0;
    if (verdict->span) {
        for (i = 0; i < (verdict->span + 3) / 4 && guest[i] == verdict->words[i]; i++) {
        }
        same = i == (verdict->span + 3) / 4;
        if (same && verdict->rows_generation == generation && verdict->mods_changes == changes) {
            return verdict->found ? &verdict->chosen : NULL;
        }
    }
    /* The order (notes/modding.md, "Your own C for a module (API 29)"): the
     * mods' rows that ask to go first, the disc's rows, the mods' others. */
    if (mods) {
        found = ModelRows_Find(primary, slot, identifier, guest, 1, &longest);
    }
    for (row = primary ? primary_gate_rows : gate_rows; row->sha256; row++) {
        uint8_t digest[32];
        if (row->slot != slot || row->identifier != identifier) {
            continue;
        }
        if (row->span > span) {
            span = row->span;
        }
        if (found) {
            continue;
        }
        Sha256(guest, row->span, digest);
        if (hex_equal(digest, row->sha256)) {
            found = row;
            span = row->span;
            if (!mods) {
                break;
            }
        }
    }
    if (mods && !found && primary && primary_returns_two(module)) {
        found = &return_two_row;
    }
    if (mods && !found) {
        found = ModelRows_Find(primary, slot, identifier, guest, 0, &longest_after);
    }
    /* The verdict covers the identifier's longest span, so that the next call
     * compares what it was taken from; with mods' rows, the longest of any
     * row of the identifier, disc or mod. */
    if (mods) {
        if (longest > span) span = longest;
        if (longest_after > span) span = longest_after;
        if (found == &return_two_row && span < return_two_row.span) span = return_two_row.span;
    }
    verdict->span = span ? span : 4;
    verdict->found = found != NULL;
    if (found) {
        verdict->chosen = *found;
        verdict->chosen.sha256 = NULL; /* the registry's may not outlive this call */
    }
    verdict->rows_generation = generation;
    verdict->mods_changes = changes;
    if (!same) {
        verdict->outside_logged = 0;
        verdict->stand_in = 0; /* a new module: no attack of the old one goes on */
        verdict->attack_claimed = 0;
    }
    for (i = 0; i < (verdict->span + 3) / 4; i++) {
        verdict->words[i] = guest[i];
    }
    if (Log_Wanted(LOG_MODS)) {
        /* What the interpreter runs, for a mod's author: the disc's module
         * the port has no C for yet, or the mod's own code or tables
         * (model_module_class.h). */
        if (found && found->owner >= 0) {
            LOG(LOG_MODS, "model %s module %u in slot %c: native C from mod %s", primary ? "primary" : "variant",
                (unsigned)identifier, slot ? 'B' : 'A', Mods_Id(found->owner));
        } else {
            ModelModuleInfo info = ModelModule_Classify(guest, slot, primary);
            LOG(LOG_MODS, "model %s module %u in slot %c: %s", primary ? "primary" : "variant", (unsigned)identifier,
                slot ? 'B' : 'A', found ? "retail code, native C" : ModelModule_ClassText(info.kind));
        }
    }
    return found ? &verdict->chosen : NULL;
}

/* The work area's word that tells an update which entry the setup picked: a
 * pointer (index_width 0), or the command the setup stored as a u8, u16 or
 * u32. Read byte by byte, least significant first, as the little-endian guest
 * wrote it, whatever the host is. */
static uint32_t read_entry_word(const void *context, const ModelGateRow *row)
{
    const uint8_t *bytes = (const uint8_t *)context + row->field;
    uint32_t word = 0, i, width = row->index_width ? row->index_width : 4u;
    for (i = 0; i < width; i++) {
        word |= (uint32_t)bytes[i] << (8 * i);
    }
    return word;
}

/* Whether the native C may take this call: the retail code is in the slot
 * (native_row) and the call stays within the entries the hash covers
 * (model_gate_command.h). A call outside them, which only a mod's record can
 * make, is interpreted, and logged once for the code in the slot. So is an
 * update of an attack the stand-in took (verdict->stand_in). *field is the
 * work area's entry POINTER for a pointer row's setup that goes elsewhere
 * (else -1), for run_module to clear if the stand-in takes it. *chosen is
 * the row the verdict picked, whether or not it takes this call. */
static ModelEntry native_entry(int primary, uint32_t module, int slot, uint32_t identifier, void *context,
                               int command, int *field, const ModelGateRow **chosen)
{
    const ModelGateRow *row = native_row(primary, module, slot, identifier);
    ModelVerdict *verdict = primary ? &primary_verdicts[slot] : &verdicts[slot];
    const char *kind = primary ? "primary" : "variant";
    uint32_t entry_word = 0;
    *field = -1;
    *chosen = row;
    if (!row) {
        return NULL;
    }
    if (row->entries != MODEL_GATE_NO_TABLE && command < 0) {
        entry_word = read_entry_word(context, row);
    }
    if (!ModelGate_CommandCoveredModulo(row->entries, row->table, row->stride, module, command, entry_word,
                                        row->index_width, verdict->stand_in, row->modulus)) {
        if (!verdict->outside_logged) {
            verdict->outside_logged = 1;
            if (command >= 0) {
                LOG(LOG_MODS, "model %s module %u in slot %c: command %d is past the %u entries of its table "
                    "the hash covers, interpreter", kind, (unsigned)identifier, slot ? 'B' : 'A', command,
                    (unsigned)row->entries);
            } else if (verdict->stand_in) {
                LOG(LOG_MODS, "model %s module %u in slot %c: an update of an attack the stand-in took stays "
                    "with it", kind, (unsigned)identifier, slot ? 'B' : 'A');
            } else if (row->index_width) {
                LOG(LOG_MODS, "model %s module %u in slot %c: an attack whose index %u is not one of the "
                    "%u the hash covers, interpreter", kind, (unsigned)identifier, slot ? 'B' : 'A',
                    (unsigned)entry_word, (unsigned)row->entries);
            } else {
                LOG(LOG_MODS, "model %s module %u in slot %c: an attack whose entry (0x%08X) is not one of the "
                    "%u the hash covers, interpreter", kind, (unsigned)identifier, slot ? 'B' : 'A',
                    (unsigned)entry_word, (unsigned)row->entries);
            }
        }
        if (command >= 0 && !row->index_width) {
            *field = (int)row->field;
        }
        return NULL;
    }
    return row->entry;
}

/* module: the slot base (the identifier word; the entry is at +4). The
 * trace names the module by its slot and identifier, and what runs it:
 * "native" (the decomp's C), "mod:<id>" (a mod's C, mod API 29),
 * "interpreter" or "stand-in". */
static int run_module(uint32_t module, int variant, void *context, int command)
{
    static uint32_t failed[2][2]; /* [slot][variant]: identifier of a module that failed */
    int slot = module >= 0x8017A000u;
    uint32_t identifier = *(const uint32_t *)(uintptr_t)module;
    uint32_t entry = *(const uint32_t *)(uintptr_t)(module + 4);
    int mode = model_mode();
    int interpret = mode != MODE_SPARK && entry != 0 && failed[slot][variant] != identifier;
    int field = -1;
    ModelVerdict *verdict = variant ? &verdicts[slot] : &primary_verdicts[slot];
    const ModelGateRow *ran = NULL;
    ModelEntry native = NULL;
    char way[80];

    if (mode == MODE_DEFAULT && entry != 0) {
        /* `return 2` is answered before any verdict while no mod has a row,
         * as it always was; with rows, native_row orders it among them. */
        if (!variant && primary_returns_two(module) && !(mod_c() && ModelRows_Count() > 0)) {
            native = native_primary;
        } else {
            native = native_entry(!variant, module, slot, identifier, context, command, &field, &ran);
        }
    }

    /* A setup starts an attack: whoever takes it, the stand-in's claim on the
     * slot's updates (set below) ends, and so does a mod's C's. An update of
     * an attack a mod's C set up goes to that C, whatever the verdict says
     * now (the mod turned off, another's row won): its work area is that C's,
     * which neither the interpreter nor other C can read. */
    if (command >= 0) {
        verdict->stand_in = 0;
        verdict->attack_claimed = 0;
    } else if (verdict->attack_claimed) {
        if (verdict->state_loads != ModelRows_StateLoads()) {
            verdict->attack_claimed = 0; /* a loaded state: its attack is not this C's */
        } else if (mode == MODE_DEFAULT && entry != 0 && native != verdict->attack_row.entry) {
            native = verdict->attack_row.entry;
            ran = &verdict->attack_row;
            field = -1;
        }
    }
    if (native && ran && ran->owner < 0) {
        ran = NULL;   /* the disc's C */
    } else if (!native) {
        ran = NULL;
    }
    if (command >= 0 && ran) {
        verdict->attack_claimed = 1;
        verdict->attack_row = *ran;
        verdict->state_loads = ModelRows_StateLoads();
    }

    if (ran) {
        snprintf(way, sizeof(way), "mod:%s", Mods_Id(ran->owner));
    }
    LOG(LOG_MODEL, "%s slot=%u command=%d module %u in %c: %s", variant ? "variant" : "primary",
        (unsigned)D_8009AFA0, command, (unsigned)identifier, slot ? 'B' : 'A',
        ran ? way : native ? "native" : interpret ? "interpreter" : "stand-in");
    if (native) {
        uint32_t previous;
        int result;
#ifdef MEMORIES_TEST_HOOKS
        if (stack_poison() >= 0) paint_stack(stack_poison());
#endif
        if (!ran) {
            return native(context, command);
        }
        previous = ModelRows_Running();
        ModelRows_SetRunning(module);
        result = native(context, command);
        ModelRows_SetRunning(previous);
        return result;
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
    /* The stand-in takes the attack. Its work area is another structure (a
     * variant's: its first bytes are random sparks; a primary's: native_primary
     * set nothing up, after an interpreter that failed or no entry), so what
     * the native C would read there
     * means nothing, and an index row cannot be told from a native setup's: an
     * index below `entries` is any small number, 0 included, and a stand-in's
     * byte lands there once in count/256 updates. The slot remembers instead
     * (verdict->stand_in) that the stand-in ran, at the setup or at an update
     * the interpreter failed in the middle of an attack (the stand-in writes
     * its sparks over that work area too), and no update of it goes native
     * until a setup or a new module ends the claim. A pointer row is also
     * cleared to 0, which is no entry, as the work area outlives the claim (a
     * new module resets the flag but not the memory). */
    verdict->stand_in = 1;
    if (field >= 0) {
        *(uint32_t *)((uintptr_t)context + (uint32_t)field) = 0;
    }
    return variant ? native_variant(context, command) : native_primary(context, command);
}

/* The classifier's view of the mods' rows (model_module_class.h): what
 * native_row would pick from them for an image, by the same rules. */
const char *Memories_ModelModRowOwner(const void *image, int slot, int primary, int over_disc)
{
    const ModelGateRow *row;
    if (!mod_c() || ModelRows_Count() == 0) {
        return NULL;
    }
    row = ModelRows_Find(primary, slot, *(const uint32_t *)image, image, over_disc, NULL);
    return row ? Mods_Id(row->owner) : NULL;
}

int Memories_ModelPrimaryControlA(void *context, int command) { return run_module(0x8013A000u, 0, context, command); }
int Memories_ModelVariantControlA(void *context, int command) { return run_module(0x8013B000u, 1, context, command); }
int Memories_ModelPrimaryControlB(void *context, int command) { return run_module(0x8017A000u, 0, context, command); }
int Memories_ModelVariantControlB(void *context, int command) { return run_module(0x8017B000u, 1, context, command); }
