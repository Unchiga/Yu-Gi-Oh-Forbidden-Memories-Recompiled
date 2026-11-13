/* What a MODEL.MRG control module is next to the disc's: model_module_class.h. */
#include "pc/overlays/model_module_class.h"

#include <stdio.h>
#include <string.h>

#include "pc/guest/sha256.h"

/* The native rows' spans and hashes, variant and primary
 * (model_gate_table.inc and model_primary_gate_table.inc, which
 * model_modules.c also reads for the C's entries). */
typedef struct {
    int primary, slot;
    uint32_t identifier, span;
    const char *sha256;
} SpanRow;

static const SpanRow span_rows[] = {
#define MODEL_GATE_ROW(slot, identifier, span, sha256, entry, entries, table, stride, field, index_width, modulus) \
    {0, slot, identifier, span, sha256},
#include "pc/overlays/model_gate_table.inc"
#undef MODEL_GATE_ROW
#define MODEL_PRIMARY_ROW(slot, identifier, span, sha256, entry, entries, table, stride, field, index_width, modulus) \
    {1, slot, identifier, span, sha256},
#include "pc/overlays/model_primary_gate_table.inc"
#undef MODEL_PRIMARY_ROW
    {0, 0, 0, 0, NULL}};

/* A shape's code (model_retail_table.inc): CODE a variant shape's, PRIMARY a primary one's. */
enum { RETAIL_CODE, RETAIL_PRIMARY };

typedef struct {
    int kind, slot;
    uint32_t identifier;
    int shape;
    uint32_t size;
    const char *sha256;
} RetailRow;

static const RetailRow retail_rows[] = {
#define MODEL_RETAIL(kind, slot, identifier, shape, size, sha256) {RETAIL_##kind, slot, identifier, shape, size, sha256},
#include "pc/overlays/model_retail_table.inc"
#undef MODEL_RETAIL
    {0, 0, 0, 0, 0, NULL}};

/* The words at +4 of a primary module that is `return 2` (jr ra; addiu v0, zero, 2). */
#define RETURN_TWO_0 0x03E00008u
#define RETURN_TWO_1 0x24020002u

/* Digests of one image by length: an image is hashed over its span and its
 * code, and a few rows share each length. */
typedef struct {
    const void *image;
    uint32_t sizes[8];
    uint8_t digests[8][32];
    int count;
} Digests;

static const uint8_t *digest_of(Digests *cache, uint32_t size)
{
    int i;
    for (i = 0; i < cache->count; i++) {
        if (cache->sizes[i] == size) return cache->digests[i];
    }
    i = cache->count < 8 ? cache->count++ : 7;
    cache->sizes[i] = size;
    Sha256(cache->image, size, cache->digests[i]);
    return cache->digests[i];
}

static int hex_equal(const uint8_t digest[32], const char *hex)
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        if (hex[2 * i] != digits[digest[i] >> 4] || hex[2 * i + 1] != digits[digest[i] & 15]) return 0;
    }
    return 1;
}

static int shape_of(int primary, int slot, uint32_t identifier)
{
    int kind = primary ? RETAIL_PRIMARY : RETAIL_CODE;
    for (const RetailRow *row = retail_rows; row->sha256; row++) {
        if (row->kind == kind && row->slot == slot && row->identifier == identifier) return row->shape;
    }
    return 0;
}

static ModelModuleModRows mod_rows;

void ModelModule_SetModRows(ModelModuleModRows find) { mod_rows = find; }

ModelModuleInfo ModelModule_Classify(const void *image, int slot, int primary)
{
    const uint32_t *words = image;
    int kind = primary ? RETAIL_PRIMARY : RETAIL_CODE;
    ModelModuleInfo info = {MODEL_MODULE_NEW, 0, 0, 0, 0, NULL};
    Digests cache;
    cache.image = image;
    cache.count = 0;
    info.identifier = words[0];
    if (words[1] == 0) {
        info.kind = MODEL_MODULE_EMPTY;
        return info;
    }
    info.shape = shape_of(primary, slot, info.identifier);
    /* The order the game runs them in (model_modules.c): a mod's row that
     * goes first, the disc's, a mod's other rows. */
    if (mod_rows && (info.owner = mod_rows(image, slot, primary, 1)) != NULL) {
        info.kind = MODEL_MODULE_MOD;
        return info;
    }
    /* The disc's other primaries: `return 2`, which the port answers itself. */
    if (primary && words[1] == RETURN_TWO_0 && words[2] == RETURN_TWO_1) {
        info.kind = MODEL_MODULE_NATIVE;
        info.shape = 0;
        return info;
    }
    for (const SpanRow *row = span_rows; row->sha256; row++) {
        if (row->primary == primary && row->slot == slot && row->identifier == info.identifier &&
            hex_equal(digest_of(&cache, row->span), row->sha256)) {
            info.kind = MODEL_MODULE_NATIVE;
            return info;
        }
    }
    if (mod_rows && (info.owner = mod_rows(image, slot, primary, 0)) != NULL) {
        info.kind = MODEL_MODULE_MOD;
        return info;
    }
    /* The shape's code with other bytes after it: a native shape's tables,
     * which its C reads (the mod's), or the data of a shape the port still
     * interprets whatever its data (the port's). */
    for (const RetailRow *row = retail_rows; row->sha256; row++) {
        if (row->kind == kind && row->slot == slot && row->identifier == info.identifier &&
            hex_equal(digest_of(&cache, row->size), row->sha256)) {
            info.kind = MODEL_MODULE_RETAIL;
            for (const SpanRow *span = span_rows; span->sha256; span++) {
                if (span->primary == primary && span->slot == slot && span->identifier == info.identifier) {
                    info.kind = MODEL_MODULE_TABLES;
                    info.code_size = row->size;
                    info.span = span->span;
                }
            }
            return info;
        }
    }
    info.kind = info.shape ? MODEL_MODULE_CODE : MODEL_MODULE_NEW;
    return info;
}

int ModelModule_IsModsMips(ModelModuleClass kind)
{
    return kind == MODEL_MODULE_TABLES || kind == MODEL_MODULE_CODE || kind == MODEL_MODULE_NEW;
}

const char *ModelModule_ClassText(ModelModuleClass kind)
{
    switch (kind) {
    case MODEL_MODULE_NATIVE: return "the disc's code, native C";
    case MODEL_MODULE_RETAIL: return "the disc's code, interpreted until the port has its C";
    case MODEL_MODULE_EMPTY: return "no entry, the stand-in";
    case MODEL_MODULE_TABLES: return "the disc's code with other tables, interpreted";
    case MODEL_MODULE_CODE: return "changed code, interpreted";
    case MODEL_MODULE_NEW: return "code no disc module has, interpreted";
    case MODEL_MODULE_MOD: return "native C from a code mod";
    }
    return "?";
}

int ModelModule_DiscShape(int primary, int slot, uint32_t identifier, uint32_t span, const char *sha256)
{
    for (const SpanRow *row = span_rows; row->sha256; row++) {
        if (row->primary == primary && row->slot == slot && row->identifier == identifier && row->span == span &&
            !strcmp(row->sha256, sha256)) {
            int shape = shape_of(primary, slot, identifier);
            return shape ? shape : (int)identifier;
        }
    }
    return 0;
}

int ModelModule_StagesMips(const unsigned char *stages, char *detail, size_t size, int *mod_native,
                           const char **mod_owner)
{
    static const char *const names[6] = {"stance 0 slot A", "stance 0 slot B", "stance 1 slot A",
                                         "stance 1 slot B", "primary slot A", "primary slot B"};
    size_t length = 0;
    int count = 0;
    if (detail && size) detail[0] = '\0';
    if (mod_native) *mod_native = 0;
    if (mod_owner) *mod_owner = NULL;
    for (int i = 0; i < 6; i++) {
        int primary = i >= 4;
        const unsigned char *image = stages + (primary ? 40 + 2 * (i - 4) : 10 * i) * 2048;
        ModelModuleInfo info = ModelModule_Classify(image, primary ? i - 4 : i & 1, primary);
        if (info.kind == MODEL_MODULE_MOD) {
            if (mod_native) ++*mod_native;
            if (mod_owner && !*mod_owner) *mod_owner = info.owner;
            if (detail && length < size) {
                int wrote = snprintf(detail + length, size - length, "%s%s (first word %u): native C from mod %s",
                                     length ? "; " : "", names[i], (unsigned)info.identifier, info.owner);
                if (wrote > 0) length += (size_t)wrote;
            }
            continue;
        }
        if (!ModelModule_IsModsMips(info.kind)) continue;
        count++;
        if (detail && length < size) {
            int wrote;
            if (info.kind == MODEL_MODULE_TABLES)
                wrote = snprintf(detail + length, size - length, "%s%s (shape %d): %s, bytes 0x%X-0x%X",
                                 length ? "; " : "", names[i], info.shape, ModelModule_ClassText(info.kind),
                                 (unsigned)info.code_size, (unsigned)info.span);
            else if (info.shape)
                wrote = snprintf(detail + length, size - length, "%s%s (shape %d): %s", length ? "; " : "",
                                 names[i], info.shape, ModelModule_ClassText(info.kind));
            else
                wrote = snprintf(detail + length, size - length, "%s%s (first word %u): %s", length ? "; " : "",
                                 names[i], (unsigned)info.identifier, ModelModule_ClassText(info.kind));
            if (wrote > 0) length += (size_t)wrote;
        }
    }
    return count;
}
