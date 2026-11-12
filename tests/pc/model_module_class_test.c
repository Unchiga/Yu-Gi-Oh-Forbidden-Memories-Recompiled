/* src/pc/overlays/model_module_class.c against the disc (MEMORIES_DISC, a raw
 * 2352-byte-sector image; skipped without one): no module of the disc is a
 * mod's MIPS, and a module changed past its span, in its tables, in its code
 * or in its first word is told apart. The fixtures are the disc's modules,
 * changed here, never stored. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc/overlays/model_module_class.h"

#define SECTOR 2048
#define RAW_SECTOR 2352
#define RECORD_SECTORS 0x114

typedef struct {
    int primary, slot;
    uint32_t identifier, span;
} SpanRow;

static const SpanRow span_rows[] = {
#define MODEL_GATE_ROW(slot, identifier, span, sha256, entry, entries, table, stride, field, index_width, modulus) \
    {0, slot, identifier, span},
#include "pc/overlays/model_gate_table.inc"
#undef MODEL_GATE_ROW
#define MODEL_PRIMARY_ROW(slot, identifier, span, sha256, entry, entries, table, stride, field, index_width, modulus) \
    {1, slot, identifier, span},
#include "pc/overlays/model_primary_gate_table.inc"
#undef MODEL_PRIMARY_ROW
    {0, 0, 0, 0}};

typedef struct {
    int primary, slot;
    uint32_t identifier, size;
} CodeRow;

#define CODE 0
#define PRIMARY 1
static const CodeRow code_rows[] = {
#define MODEL_RETAIL(kind, slot, identifier, shape, size, sha256) {kind, slot, identifier, size},
#include "pc/overlays/model_retail_table.inc"
#undef MODEL_RETAIL
    {0, 0, 0, 0}};

static FILE *disc;
static int failures;

static int sector(long lba, unsigned char *out)
{
    return fseek(disc, lba * RAW_SECTOR + 24, SEEK_SET) == 0 && fread(out, 1, SECTOR, disc) == SECTOR;
}

static uint32_t word(const unsigned char *at) { return at[0] | at[1] << 8 | at[2] << 16 | (uint32_t)at[3] << 24; }

/* An ISO 9660 path from the root: its first sector and size. */
static int find(const char *const *names, int count, long *lba, uint32_t *size)
{
    unsigned char data[SECTOR];
    if (!sector(16, data)) return 0;
    *lba = (long)word(data + 156 + 2);
    *size = word(data + 156 + 10);
    for (int i = 0; i < count; i++) {
        int found = 0;
        for (uint32_t n = 0; n < (*size + SECTOR - 1) / SECTOR && !found; n++) {
            if (!sector(*lba + (long)n, data)) return 0;
            for (int at = 0; at < SECTOR && data[at]; at += data[at]) {
                size_t length = strlen(names[i]);
                if (data[at + 32] == length && !memcmp(data + at + 33, names[i], length)) {
                    *lba = (long)word(data + at + 2);
                    *size = word(data + at + 10);
                    found = 1;
                    break;
                }
            }
        }
        if (!found) return 0;
    }
    return 1;
}

static void expect(int condition, const char *what, int record)
{
    if (!condition) {
        fprintf(stderr, "model_module_class_test: record %d: %s\n", record, what);
        failures++;
    }
}

static const SpanRow *span_row(int primary, int slot, uint32_t identifier)
{
    for (const SpanRow *row = span_rows; row->identifier; row++)
        if (row->primary == primary && row->slot == slot && row->identifier == identifier) return row;
    return NULL;
}

static uint32_t code_size(int primary, int slot, uint32_t identifier)
{
    for (const CodeRow *row = code_rows; row->identifier; row++)
        if (row->primary == primary && row->slot == slot && row->identifier == identifier) return row->size;
    return 0;
}

static ModelModuleClass changed(const unsigned char *image, size_t size, uint32_t at, int slot, int primary)
{
    unsigned char copy[MODEL_MODULE_VARIANT_SIZE];
    memcpy(copy, image, size);
    copy[at] ^= 0x5A;
    return ModelModule_Classify(copy, slot, primary).kind;
}

int main(void)
{
    static const char *const path[] = {"DATA", "MODEL.MRG;1"};
    const char *name = getenv("MEMORIES_DISC");
    unsigned char *stages = malloc((size_t)MODEL_MODULE_STAGES_SECTORS * SECTOR);
    long lba;
    uint32_t size;
    int records, native = 0, retail = 0, tables = 0, primaries = 0;
    char detail[1024];
    if (!name || !*name || !(disc = fopen(name, "rb"))) {
        printf("model_module_class_test: no disc (MEMORIES_DISC), skipped\n");
        free(stages);
        return 77;
    }
    if (!stages || !find(path, 2, &lba, &size)) {
        fprintf(stderr, "model_module_class_test: no MODEL.MRG on %s\n", name);
        return 1;
    }
    records = (int)(size / (RECORD_SECTORS * SECTOR));
    for (int record = 0; record < records; record++) {
        for (int n = 0; n < MODEL_MODULE_STAGES_SECTORS; n++)
            if (!sector(lba + (long)record * RECORD_SECTORS + MODEL_MODULE_STAGES_SECTOR + n, stages + n * SECTOR)) {
                fprintf(stderr, "model_module_class_test: record %d cannot be read\n", record);
                return 1;
            }
        /* The disc is no mod: none of its modules is a mod's MIPS. */
        expect(ModelModule_StagesMips(stages, detail, sizeof(detail)) == 0, detail, record);
        for (int i = 0; i < 6; i++) {
            int primary = i >= 4, slot = primary ? i - 4 : i & 1;
            const unsigned char *image = stages + (primary ? 40 + 2 * (i - 4) : 10 * i) * SECTOR;
            size_t bytes = primary ? MODEL_MODULE_PRIMARY_SIZE : MODEL_MODULE_VARIANT_SIZE;
            ModelModuleInfo info = ModelModule_Classify(image, slot, primary);
            /* A primary that is `return 2` the port answers itself. */
            if (primary && word(image + 4) == 0x03E00008u && word(image + 8) == 0x24020002u) {
                expect(info.kind == MODEL_MODULE_NATIVE, "a `return 2` primary is not NATIVE", record);
                continue;
            }
            /* The disc's primaries with code are its five shapes, all native. */
            if (primary) {
                primaries++;
                expect(info.kind == MODEL_MODULE_NATIVE, "a primary with code is not NATIVE", record);
            }
            if (info.kind == MODEL_MODULE_RETAIL) {
                /* A shape the port still interprets: its code changed is the
                 * mod's, other data after the code the port's. */
                uint32_t code = code_size(primary, slot, info.identifier);
                retail++;
                expect(span_row(primary, slot, info.identifier) == NULL && code, "an interpreted image has no code row",
                       record);
                expect(changed(image, bytes, 8, slot, primary) == MODEL_MODULE_CODE,
                       "a changed interpreted image is not CODE", record);
                if (code && code < bytes)
                    expect(changed(image, bytes, code, slot, primary) == MODEL_MODULE_RETAIL,
                           "an interpreted image with other data is not RETAIL", record);
            }
            if (info.kind == MODEL_MODULE_NATIVE) {
                const SpanRow *row = span_row(primary, slot, info.identifier);
                uint32_t code = code_size(primary, slot, info.identifier);
                unsigned char copy[MODEL_MODULE_VARIANT_SIZE];
                native++;
                expect(row && code, "a native image has no span or code row", record);
                if (!row || !code) continue;
                /* Past the span: the record's own data, still native. */
                if (row->span < bytes)
                    expect(changed(image, bytes, row->span, slot, primary) == MODEL_MODULE_NATIVE, "a byte past the span", record);
                /* Between the code and the span: the C's tables, the mod's. */
                if (code < row->span) {
                    ModelModuleInfo got;
                    memcpy(copy, image, bytes);
                    copy[row->span - 1] ^= 0x5A;
                    got = ModelModule_Classify(copy, slot, primary);
                    tables++;
                    expect(got.kind == MODEL_MODULE_TABLES && got.code_size == code && got.span == row->span,
                           "a byte in the tables is not TABLES", record);
                }
                /* In the code. */
                expect(changed(image, bytes, 8, slot, primary) == MODEL_MODULE_CODE, "a byte in the code is not CODE", record);
                /* A first word no disc module has. */
                memcpy(copy, image, bytes);
                copy[0] = 0xFF, copy[1] = 0xFF, copy[2] = 0xFF, copy[3] = 0x7F;
                expect(ModelModule_Classify(copy, slot, primary).kind == MODEL_MODULE_NEW, "an unknown first word is not NEW",
                       record);
                /* No entry: the stand-in. */
                memset(copy + 4, 0, 4);
                expect(ModelModule_Classify(copy, slot, primary).kind == MODEL_MODULE_EMPTY, "no entry is not EMPTY", record);
            }
        }
    }
    /* Each kind was met: the disc has native images, native shapes with
     * tables and primaries with code; and none the port still interprets,
     * every shape of the disc being native (test_model_gate.py). */
    expect(native > 0 && tables > 0 && primaries > 0, "a kind of module was never met", -1);
    expect(retail == 0, "a module of the disc is one the port still interprets", -1);
    printf("model_module_class_test: %d records; %d native images (%d with tables), %d interpreted, %d primaries with "
           "code; %d failures\n", records, native, tables, retail, primaries, failures);
    fclose(disc);
    free(stages);
    return failures != 0;
}
