/* src/pc/mods/model_rows.c: the control-module rows code mods register (mod
 * API 29). Which mods are applied, and in what order, is stubbed here. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pc/guest/sha256.h"
#include "pc/mods/model_rows.h"
#include "pc/overlays/model_gate_command.h"

static int active[4] = {1, 1, 1, 1};
static unsigned sequence[4] = {1, 2, 3, 4};
int Mods_Active(int mod) { return mod >= 0 && mod < 4 && active[mod]; }
unsigned Mods_Sequence(int mod) { return mod >= 0 && mod < 4 ? sequence[mod] : 0; }

static int failures;
#define EXPECT(condition)                                                        \
    do {                                                                         \
        if (!(condition)) {                                                      \
            fprintf(stderr, "model_rows_test:%d: %s\n", __LINE__, #condition); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static int32_t first(uint8_t *context, int32_t command) { (void)context; return command + 1; }
static int32_t second(uint8_t *context, int32_t command) { (void)context; return command + 2; }

static uint8_t image[0x6000]; /* past the largest span a refused row names */

static void hex(const void *data, uint32_t size, char out[65], int upper)
{
    uint8_t digest[32];
    Sha256(data, size, digest);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, upper ? "%02X" : "%02x", digest[i]);
}

static MemoriesModelModule row_for(uint32_t kind, uint32_t slot, uint32_t span, MemoriesModelEntry entry)
{
    MemoriesModelModule row;
    memset(&row, 0, sizeof(row));
    row.size = sizeof(row);
    row.kind = kind;
    row.slot = slot;
    row.identifier = 0x12345u;
    row.span = span;
    row.entry = entry;
    hex(image, span, row.sha256, 0);
    return row;
}

static int refused(MemoriesModelModule row, const char *reason)
{
    char why[160];
    int token = ModelRows_Add(0, &row, why, sizeof(why));
    if (token || !strstr(why, reason)) {
        fprintf(stderr, "model_rows_test: expected a refusal with \"%s\", got token %d, \"%s\"\n", reason, token, why);
        return 0;
    }
    return 1;
}

int main(void)
{
    MemoriesModelModule row;
    const ModelGateRow *found;
    uint32_t longest;
    unsigned generation;
    int token, other, both;
    char why[160];

    for (unsigned i = 0; i < sizeof(image); i++) image[i] = (uint8_t)(i * 7 + 3);
    memcpy(image, "\x45\x23\x01\x00", 4); /* identifier 0x12345 */

    /* Validation: what model_gate.py check guarantees for a disc row. */
    row = row_for(0, 0, 0x100, first);
    row.sha256[10] = 'g';
    EXPECT(refused(row, "64 hex digits"));
    row = row_for(0, 0, 0x100, first);
    row.sha256[64] = 'a';
    EXPECT(refused(row, "64 hex digits"));
    EXPECT(refused(row_for(0, 0, 0x5004, first), "span"));
    EXPECT(refused(row_for(1, 0, 0x1004, first), "span"));
    EXPECT(refused(row_for(0, 0, 4, first), "span"));
    EXPECT(refused(row_for(0, 0, 0x100, NULL), "no entry"));
    EXPECT(refused(row_for(2, 0, 0x100, first), "kind"));
    EXPECT(refused(row_for(0, 3, 0x100, first), "slot"));
    row = row_for(0, 0, 0x100, first);
    row.size = 8;
    EXPECT(refused(row, "size"));
    row = row_for(0, 0, 0x100, first);
    row.flags = MEMORIES_MODEL_BOUND;
    row.entries = 4, row.table = 0xF0, row.stride = 8;   /* 0xF0 + 32 > 0x100 */
    EXPECT(refused(row, "inside the span"));
    row.table = 0xE0;
    row.modulus = 100, row.index_width = 1;
    EXPECT(refused(row, "modulus"));
    row.modulus = 0, row.index_width = 3;
    EXPECT(refused(row, "index_width"));
    row.index_width = 0, row.stride = 0;
    EXPECT(refused(row, "inside the span"));
    row.stride = 8, row.field = 0xFFE;
    EXPECT(refused(row, "field"));
    EXPECT(ModelRows_Count() == 0 && ModelRows_Generation() == 0);

    /* A bound row is kept as given; one without the flag takes every command. */
    row.field = 0x10;
    generation = ModelRows_Generation();
    token = ModelRows_Add(0, &row, why, sizeof(why));
    EXPECT(token > 0 && ModelRows_Generation() != generation);
    found = ModelRows_Find(0, 0, 0x12345u, image, 0, &longest);
    EXPECT(found && found->entries == 4 && found->table == 0xE0 && found->stride == 8 && found->field == 0x10 &&
           found->owner == 0 && found->token == token && longest == 0x100);
    ModelRows_Remove(0, token);
    EXPECT(ModelRows_Count() == 0 && !ModelRows_Find(0, 0, 0x12345u, image, 0, NULL));

    /* An uppercase sha256 is accepted and found; the stored one is lowercase. */
    row = row_for(0, 0, 0x200, first);
    hex(image, 0x200, row.sha256, 1);
    token = ModelRows_Add(0, &row, why, sizeof(why));
    EXPECT(token > 0);
    found = ModelRows_Find(0, 0, 0x12345u, image, 0, NULL);
    EXPECT(found && found->entries == MODEL_GATE_NO_TABLE && found->entry((void *)image, 5) == 6);
    EXPECT(found && found->sha256[0] >= '0' && (found->sha256[0] <= '9' || found->sha256[0] >= 'a'));
    /* Not for another kind, slot, identifier or other bytes. */
    EXPECT(!ModelRows_Find(1, 0, 0x12345u, image, 0, NULL));
    EXPECT(!ModelRows_Find(0, 1, 0x12345u, image, 0, NULL));
    EXPECT(!ModelRows_Find(0, 0, 0x12346u, image, 0, NULL));
    image[0x1FF] ^= 1;
    EXPECT(!ModelRows_Find(0, 0, 0x12345u, image, 0, &longest) && longest == 0x200);
    image[0x1FF] ^= 1;
    /* A byte past the span is the record's own data. */
    image[0x200] ^= 1;
    EXPECT(ModelRows_Find(0, 0, 0x12345u, image, 0, NULL));
    image[0x200] ^= 1;

    /* An owner that is not applied: not found, and no span either. */
    active[0] = 0;
    EXPECT(!ModelRows_Find(0, 0, 0x12345u, image, 0, &longest) && longest == 0);
    active[0] = 1;

    /* Two owners on one key: the newest applied wins. */
    row.entry = second;
    other = ModelRows_Add(1, &row, why, sizeof(why));
    EXPECT(other > 0 && other != token);
    found = ModelRows_Find(0, 0, 0x12345u, image, 0, NULL);
    EXPECT(found && found->owner == 1);
    sequence[0] = 9;
    found = ModelRows_Find(0, 0, 0x12345u, image, 0, NULL);
    EXPECT(found && found->owner == 0);
    sequence[0] = 1;

    /* OVER_DISC rows are found only before the disc's, the others only after. */
    row = row_for(0, 0, 0x300, first);
    row.flags = MEMORIES_MODEL_OVER_DISC;
    EXPECT(ModelRows_Add(2, &row, why, sizeof(why)) > 0);
    found = ModelRows_Find(0, 0, 0x12345u, image, 1, &longest);
    EXPECT(found && found->owner == 2 && longest == 0x300);
    found = ModelRows_Find(0, 0, 0x12345u, image, 0, NULL);
    EXPECT(found && found->owner == 1);

    /* SLOT_BOTH: two rows, one token, one for each slot. */
    row = row_for(1, MEMORIES_MODEL_SLOT_BOTH, 0x40, first);
    generation = ModelRows_Generation();
    both = ModelRows_Add(3, &row, why, sizeof(why));
    EXPECT(both > 0 && ModelRows_Generation() != generation);
    EXPECT(ModelRows_Count() == 5);
    found = ModelRows_Find(1, 0, 0x12345u, image, 0, NULL);
    EXPECT(found && found->token == both && found->slot == 0);
    found = ModelRows_Find(1, 1, 0x12345u, image, 0, NULL);
    EXPECT(found && found->token == both && found->slot == 1);
    ModelRows_Remove(3, both);
    EXPECT(ModelRows_Count() == 3 && !ModelRows_Find(1, 1, 0x12345u, image, 0, NULL));
    /* Another owner's token removes nothing. */
    generation = ModelRows_Generation();
    ModelRows_Remove(3, token);
    EXPECT(ModelRows_Count() == 3 && ModelRows_Generation() == generation);

    /* Clear: that owner's rows only, and the generation moves. */
    ModelRows_Clear(1);
    EXPECT(ModelRows_Count() == 2 && ModelRows_Generation() != generation);
    found = ModelRows_Find(0, 0, 0x12345u, image, 0, NULL);
    EXPECT(found && found->owner == 0);
    generation = ModelRows_Generation();
    ModelRows_Clear(1);
    EXPECT(ModelRows_Generation() == generation);

    /* The running base and the state loads. */
    EXPECT(ModelRows_Running() == 0);
    ModelRows_SetRunning(0x8017B000u);
    EXPECT(ModelRows_Running() == 0x8017B000u);
    ModelRows_SetRunning(0);
    generation = ModelRows_StateLoads();
    ModelRows_StateLoaded();
    EXPECT(ModelRows_StateLoads() == generation + 1);

    printf("model_rows_test: %d failures\n", failures);
    return failures != 0;
}
