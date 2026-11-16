#include "retail_image.h"
#include "sha256.h"
#include "state.h"
#include "pc/debug/log.h"
#include <stdio.h>
#include <string.h>

/* CODE_RETAIL: changed, but only in the image's data ranges (its code, and
 * what the code hands the host as strings, are the retail bytes). Kept in
 * save states as the others are; a state from before reads it as CHANGED's
 * neighbour value and nothing else. */
enum { UNKNOWN, PENDING, RETAIL, CHANGED, CODE_RETAIL };
/* What a changed image gets: the interpreter in a reference build alone
 * (MEMORIES_REFERENCE); a shipped game skips it (duel_effects.c, credits.c). */
#ifdef MEMORIES_REFERENCE
#define CHANGED_RUNS "interpreter"
#define DATA_RUNS "interpreter"
#else
#define CHANGED_RUNS "skipped (no MIPS interpreter)"
#define DATA_RUNS "native C on a mod's own data (\"duel_effects\": \"own data\"), else skipped (no MIPS interpreter)"
#endif

typedef struct Range {
    uint32_t start, end; /* offsets in the image */
} Range;

typedef struct Image {
    const char *name;
    uint32_t base, size;
    const char *sha256; /* of the retail image, as config/slus_01411/overlays.json records it */
    /* The image's code ranges, and the SHA-256 of the retail bytes of all
     * of them in order; none for an image whose data no mod may bring. */
    const Range *code;
    unsigned code_count;
    const char *code_sha256;
} Image;

/* The effect bank's code (config/slus_01411/overlays/duel_effects.yaml): the
 * identifier, the two format strings the bank's C hands the host's printf
 * (cross_lines.c), the effect 22 jump table and the text. The rest is the
 * data the bank's C reads where the disc put it (its D_ names are pinned to
 * these addresses): four scale vectors in the header (0x4-0x14, 0x6C-0x9C),
 * 0x180-0x2B0 and 0x14270-0x16000. config/pc/duel_effect_bank.csv lists
 * both, symbol by symbol (tools/pc/effect_migrate.py). */
static const Range duel_effects_code[] = {{0x0u, 0x4u}, {0x14u, 0x6Cu}, {0x9Cu, 0x180u}, {0x2B0u, 0x14270u}};

static const Image images[RETAIL_IMAGE_COUNT] = {
    {"duel_effects", 0x80146000u, 0x16000u, "baa203b937dc6bdf91b1826c5832f0f32e11ae5fe9d05193a4361bc08158b9e0",
     duel_effects_code, 4, "85d81752a9f233b41cd5be7557359e9eec31e9c586743888a27764835b1b82db"},
    {"credits", 0x80180000u, 0x8000u, "f125a2a6a8b57d222df544a7a02bf8c639c1fdde5cf978f80a56ea3fba2b836a",
     NULL, 0, NULL},
};

static volatile unsigned char verdicts[RETAIL_IMAGE_COUNT];

static void hex_of(const uint8_t digest[32], char hex[65])
{
    unsigned i;
    for (i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
}

/* Whether the image's code ranges hold the retail bytes. */
static int code_retail(const Image *image)
{
    static uint8_t joined[0x16000]; /* the largest image's size */
    uint8_t digest[32];
    char hex[65];
    size_t used = 0;
    unsigned i;
    if (!image->code_count) return 0;
    for (i = 0; i < image->code_count; i++) {
        size_t length = image->code[i].end - image->code[i].start;
        if (used + length > sizeof(joined)) return 0;
        memcpy(joined + used, (const void *)(uintptr_t)(image->base + image->code[i].start), length);
        used += length;
    }
    Sha256(joined, used, digest);
    hex_of(digest, hex);
    return strcmp(hex, image->code_sha256) == 0;
}

/* Hash a pending image. A delivery during the hash marks it pending again,
 * and the hash is taken again until one ends with no delivery (each is a
 * new copy of the image, written once), so a verdict is never left pending
 * for a call to take as "not retail". */
static void settle(RetailImageId id)
{
    const Image *image = &images[id];
    while (verdicts[id] == PENDING) {
        uint8_t digest[32];
        char hex[65];
        int verdict;
        verdicts[id] = UNKNOWN;
        Sha256((const void *)(uintptr_t)image->base, image->size, digest);
        hex_of(digest, hex);
        verdict = strcmp(hex, image->sha256) == 0 ? RETAIL : code_retail(image) ? CODE_RETAIL : CHANGED;
        if (verdicts[id] != UNKNOWN) continue; /* delivered again meanwhile */
        verdicts[id] = (unsigned char)verdict;
        LOG(LOG_MODS, "%s: %s bytes at 0x%08X (sha256 %s): %s", image->name,
            verdict == RETAIL ? "retail" : verdict == CODE_RETAIL ? "changed data, retail code" : "changed",
            (unsigned)image->base, hex,
            verdict == RETAIL ? "native C" : verdict == CODE_RETAIL ? DATA_RUNS : CHANGED_RUNS);
    }
}

int RetailImage_Verified(RetailImageId id)
{
    settle(id);
    return verdicts[id] == RETAIL;
}

int RetailImage_CodeVerified(RetailImageId id)
{
    settle(id);
    return verdicts[id] == RETAIL || verdicts[id] == CODE_RETAIL;
}

void RetailImage_Written(uintptr_t destination, size_t length)
{
    unsigned i;
    for (i = 0; i < RETAIL_IMAGE_COUNT; i++) {
        /* A new copy of the image starts with its first sector. The game
         * also streams other data into an image's tail while it runs (the
         * credits load 20 sectors at 0x80185CD4); that is not a new image. */
        if (destination <= (uintptr_t)images[i].base && images[i].base < destination + length) {
            verdicts[i] = PENDING;
        }
    }
}

void RetailImage_State(MemoriesState *state)
{
    unsigned char saved[RETAIL_IMAGE_COUNT];
    MemoriesStateField field = {saved, sizeof(saved)};
    unsigned i;
    memcpy(saved, (const void *)verdicts, sizeof(saved));
    if (!Memories_StateChunk(state, "retail-images", &field, 1)) {
        if (Memories_StateLoading(state)) {
            /* A state from before the check: what is in memory is unproven. */
            memset((void *)verdicts, UNKNOWN, sizeof(saved));
        }
        return;
    }
    if (Memories_StateLoading(state)) {
        for (i = 0; i < RETAIL_IMAGE_COUNT; i++) {
            verdicts[i] = saved[i];
        }
    }
}
