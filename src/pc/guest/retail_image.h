#ifndef MEMORIES_PC_GUEST_RETAIL_IMAGE_H
#define MEMORIES_PC_GUEST_RETAIL_IMAGE_H
#include <stddef.h>
#include <stdint.h>

/* Runtime-loaded code the port also carries as native C, gated on the bytes
 * the disc delivered. The native C is the decomp of the retail image, so it
 * only stands in for that image: a mod or a disc patch that changed any of
 * its bytes gets the delivered code run by the interpreter instead (a
 * reference build), or a fallback (the shipped game: duel_effects.c,
 * credits.c), unless only data changed and the mod asks for its own data.
 *
 * The check hashes the whole image (SHA-256) once, at the first call after
 * a delivery wrote its first word (a new copy of it), before any of its
 * code has run and written
 * to its own variables. The verdict is kept in save states. */
typedef enum RetailImageId {
    RETAIL_IMAGE_DUEL_EFFECTS, /* the WA duel-effect bank at 0x80146000 */
    RETAIL_IMAGE_CREDITS,      /* the SU credits module at 0x80180000 */
    RETAIL_IMAGE_COUNT
} RetailImageId;

/* 1 when the image in guest memory is the retail one (run the native C),
 * 0 when it changed or was never delivered (interpret it). The first call
 * after a delivery hashes it and reports the verdict (MEMORIES_TRACE=mods). */
int RetailImage_Verified(RetailImageId id);
/* 1 when the image's code is the retail one, whatever its data: the
 * retail image, or one a mod changed in its data ranges only (the effect
 * bank's tables; the credits have none, so for them it is
 * RetailImage_Verified). The native C then runs on the mod's data only when
 * a mod asks for it (Mods_DuelEffectsOwnData). */
int RetailImage_CodeVerified(RetailImageId id);

/* A disc delivery wrote [destination, destination + length). Async-signal-safe. */
void RetailImage_Written(uintptr_t destination, size_t length);

struct MemoriesState;
void RetailImage_State(struct MemoriesState *state);

#endif
