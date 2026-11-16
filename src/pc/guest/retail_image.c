#include "retail_image.h"
#include "sha256.h"
#include "state.h"
#include "pc/debug/log.h"
#include <stdio.h>
#include <string.h>

enum { UNKNOWN, PENDING, RETAIL, CHANGED };
/* What a changed image gets: the interpreter in a reference build alone
 * (MEMORIES_REFERENCE); a shipped game skips it (duel_effects.c, credits.c). */
#ifdef MEMORIES_REFERENCE
#define CHANGED_RUNS "interpreter"
#else
#define CHANGED_RUNS "skipped (no MIPS interpreter)"
#endif

typedef struct Image {
    const char *name;
    uint32_t base, size;
    const char *sha256; /* of the retail image, as config/slus_01411/overlays.json records it */
} Image;

static const Image images[RETAIL_IMAGE_COUNT] = {
    {"duel_effects", 0x80146000u, 0x16000u, "baa203b937dc6bdf91b1826c5832f0f32e11ae5fe9d05193a4361bc08158b9e0"},
    {"credits", 0x80180000u, 0x8000u, "f125a2a6a8b57d222df544a7a02bf8c639c1fdde5cf978f80a56ea3fba2b836a"},
};

static volatile unsigned char verdicts[RETAIL_IMAGE_COUNT];

int RetailImage_Verified(RetailImageId id)
{
    const Image *image = &images[id];
    if (verdicts[id] == PENDING) {
        uint8_t digest[32];
        char hex[65];
        unsigned i;
        verdicts[id] = UNKNOWN; /* a delivery during the hash marks it pending again */
        Sha256((const void *)(uintptr_t)image->base, image->size, digest);
        for (i = 0; i < 32; i++) {
            snprintf(hex + 2 * i, 3, "%02x", digest[i]);
        }
        if (verdicts[id] == UNKNOWN) {
            verdicts[id] = strcmp(hex, image->sha256) == 0 ? RETAIL : CHANGED;
        }
        LOG(LOG_MODS, "%s: %s bytes at 0x%08X (sha256 %s): %s", image->name,
            verdicts[id] == RETAIL ? "retail" : "changed", (unsigned)image->base, hex,
            verdicts[id] == RETAIL ? "native C" : CHANGED_RUNS);
    }
    return verdicts[id] == RETAIL;
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
