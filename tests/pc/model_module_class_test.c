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

/* A code mod's rows (mod API 29), as the game's finder gives them: "late"
 * covers any module with first word 0x00ABCDEF after the disc's rows,
 * "early" whatever `early_word` names before them. */
static uint32_t early_word;
static const char *finder(const void *image, int slot, int primary, int over_disc)
{
    uint32_t first = word(image);
    (void)slot;
    (void)primary;
    if (over_disc) return early_word && first == early_word ? "early" : NULL;
    return first == 0x00ABCDEFu ? "late" : NULL;
}

/* The classifier with a finder, and without one as before. */
static void mod_rows(void)
{
    unsigned char *stages = calloc(MODEL_MODULE_STAGES_SECTORS, SECTOR);
    char detail[512];
    const char *owner = (const char *)1;
    int native = -1;
    if (!stages) return;
    stages[0] = 0xEF, stages[1] = 0xCD, stages[2] = 0xAB; /* stance 0 slot A: first word 0x00ABCDEF */
    stages[4] = 0x08, stages[7] = 0x03;                   /* +4: jr ra */
    expect(ModelModule_Classify(stages, 0, 0).kind == MODEL_MODULE_NEW, "no finder: a new first word is not NEW", -1);
    expect(ModelModule_StagesMips(stages, detail, sizeof(detail), &native, &owner) == 1 && native == 0 && !owner,
           "no finder: a new module is not the mod's MIPS", -1);
    ModelModule_SetModRows(finder);
    {
        ModelModuleInfo info = ModelModule_Classify(stages, 0, 0);
        expect(info.kind == MODEL_MODULE_MOD && info.owner && !strcmp(info.owner, "late"),
               "a module a mod's row covers is not MOD", -1);
    }
    expect(ModelModule_StagesMips(stages, detail, sizeof(detail), &native, &owner) == 0 && native == 1 && owner &&
               !strcmp(owner, "late") && strstr(detail, "native C from mod late"),
           "a module a mod's row covers is counted as MIPS", -1);
    expect(!ModelModule_IsModsMips(MODEL_MODULE_MOD), "MOD is the mod's MIPS", -1);
    ModelModule_SetModRows(NULL);
    free(stages);
}

/* --classify FILE: each module of FILE (a byte 1 for a primary, else 0, a
 * byte for the slot, then the image, 0x1000 or 0x5000 bytes) as the game
 * classifies it, one line each: "kind identifier shape code_size span", kind
 * as ModelModuleClass numbers it. tools/pc/test_model_migrate.py holds
 * model_migrate.py's classify() to these lines. */
static int classify_file(const char *path)
{
    static unsigned char image[MODEL_MODULE_VARIANT_SIZE];
    FILE *file = fopen(path, "rb");
    int kind, slot;
    if (!file) {
        fprintf(stderr, "model_module_class_test: cannot read %s\n", path);
        return 1;
    }
    ModelModule_SetModRows(NULL);
    while ((kind = fgetc(file)) != EOF && (slot = fgetc(file)) != EOF) {
        size_t size = kind ? MODEL_MODULE_PRIMARY_SIZE : MODEL_MODULE_VARIANT_SIZE;
        ModelModuleInfo info;
        if (fread(image, 1, size, file) != size) break;
        info = ModelModule_Classify(image, slot, kind);
        printf("%d %u %d %u %u\n", (int)info.kind, (unsigned)info.identifier, info.shape, (unsigned)info.code_size,
               (unsigned)info.span);
    }
    fclose(file);
    return 0;
}

int main(int argc, char **argv)
{
    static const char *const path[] = {"DATA", "MODEL.MRG;1"};
    const char *name = getenv("MEMORIES_DISC");
    unsigned char *stages;
    long lba;
    uint32_t size;
    int records, native = 0, retail = 0, tables = 0, primaries = 0, ordered = 0;
    char detail[1024];
    if (argc == 3 && !strcmp(argv[1], "--classify")) return classify_file(argv[2]);
    stages = malloc((size_t)MODEL_MODULE_STAGES_SECTORS * SECTOR);
    mod_rows();
    if (failures) {
        fprintf(stderr, "model_module_class_test: %d failures without the disc\n", failures);
        free(stages);
        return 1;
    }
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
        expect(ModelModule_StagesMips(stages, detail, sizeof(detail), NULL, NULL) == 0, detail, record);
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
                /* A mod's row over the disc's: after the disc's rows it never
                 * wins; before them (OVER_DISC) it does. */
                if (!ordered++) {
                    ModelModuleInfo got;
                    early_word = 0;
                    ModelModule_SetModRows(finder);
                    expect(ModelModule_Classify(image, slot, primary).kind == MODEL_MODULE_NATIVE,
                           "a late mod row stands over the disc's C", record);
                    early_word = info.identifier;
                    got = ModelModule_Classify(image, slot, primary);
                    expect(got.kind == MODEL_MODULE_MOD && !strcmp(got.owner, "early"),
                           "an OVER_DISC mod row does not stand over the disc's C", record);
                    ModelModule_SetModRows(NULL);
                    early_word = 0;
                }
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
