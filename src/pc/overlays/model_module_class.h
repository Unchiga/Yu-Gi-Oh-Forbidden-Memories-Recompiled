#ifndef MEMORIES_PC_OVERLAYS_MODEL_MODULE_CLASS_H
#define MEMORIES_PC_OVERLAYS_MODEL_MODULE_CLASS_H
#include <stddef.h>
#include <stdint.h>

/* What a MODEL.MRG control module is next to the disc's, so the game can say
 * which mods still need the MIPS interpreter (notes/modding.md, "Attack code:
 * C, not MIPS"). The disc's modules are the native rows of config/pc/model_gate.csv
 * (variant) and config/pc/model_primary_gate.csv (primary), and
 * config/pc/model_retail.csv (tools/pc/model_gate.py). */
typedef enum {
    MODEL_MODULE_NATIVE,  /* the disc's code, which the decomp's C runs; a primary that is `return 2` */
    MODEL_MODULE_RETAIL,  /* the disc's code of a shape the port still interprets: the port's
                           * to port, whatever data follows the code, not the mod's */
    MODEL_MODULE_EMPTY,   /* no entry (its word at +4 is 0): the stand-in runs it */
    MODEL_MODULE_TABLES,  /* a native shape's code, with other data where its C reads (code_size..span) */
    MODEL_MODULE_CODE,    /* the code of a shape the disc has, changed */
    MODEL_MODULE_NEW,     /* a first word no disc module of the slot has */
    MODEL_MODULE_MOD      /* a code mod's own C runs it (mod API 29): `owner`'s row */
} ModelModuleClass;

typedef struct {
    ModelModuleClass kind;
    uint32_t identifier; /* the image's first word */
    int shape;           /* the slot A identifier of its shape, 0 when none is known */
    uint32_t code_size;  /* TABLES: where its code ends (the decomp's data_start) */
    uint32_t span;       /* TABLES: where the bytes the C's hash covers end */
    const char *owner;   /* MOD: the id of the mod whose C runs it */
} ModelModuleInfo;

/* The applied code mods' rows (mod API 29): the id of the mod whose row
 * covers `image`, consulted before the disc's rows (over_disc 1) or after
 * them (0), NULL for none. The game gives model_modules.c's
 * (Memories_ModelModRowOwner); without one the mods' rows are not
 * consulted, as in the tests. */
typedef const char *(*ModelModuleModRows)(const void *image, int slot, int primary, int over_disc);
void ModelModule_SetModRows(ModelModuleModRows find);
const char *Memories_ModelModRowOwner(const void *image, int slot, int primary, int over_disc);

/* The variant images are 0x5000 bytes, the primaries 0x1000; `image` may be
 * guest memory. `slot` 0 is A (the variant module at 0x8013B000, the primary
 * at 0x8013A000), 1 is B (0x8017B000, 0x8017A000). */
#define MODEL_MODULE_VARIANT_SIZE 0x5000u
#define MODEL_MODULE_PRIMARY_SIZE 0x1000u
ModelModuleInfo ModelModule_Classify(const void *image, int slot, int primary);

/* The interpreter runs it and it is the mod's to move to C: TABLES, CODE, NEW. */
int ModelModule_IsModsMips(ModelModuleClass kind);
const char *ModelModule_ClassText(ModelModuleClass kind);

/* A record's module stages: 44 sectors from its sector 180 (notes/modding.md,
 * "The record"), the variant images of stance 0 slot A, stance 0 slot B,
 * stance 1 slot A and stance 1 slot B, 10 sectors each, then the primaries
 * of slot A and B, 2 sectors each. */
#define MODEL_MODULE_STAGES_SECTOR 180
#define MODEL_MODULE_STAGES_SECTORS 44

/* How many of the six modules of `stages` are the mod's MIPS
 * (ModelModule_IsModsMips), with a "; "-separated line for each in `detail`;
 * *mod_native (when not NULL) gets how many a code mod's C runs (MOD), with
 * a line each too, and *mod_owner (when not NULL) the first one's mod. */
int ModelModule_StagesMips(const unsigned char *stages, char *detail, size_t size, int *mod_native,
                           const char **mod_owner);

/* The shape (its slot A identifier) of the disc's row with exactly this
 * kind, slot, identifier, span and SHA-256 (64 lowercase hex digits); 0 for
 * none. A mod's row with the disc's bytes runs after the disc's C. */
int ModelModule_DiscShape(int primary, int slot, uint32_t identifier, uint32_t span, const char *sha256);

#endif
