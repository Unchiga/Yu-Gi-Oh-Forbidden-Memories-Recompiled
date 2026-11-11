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
    MODEL_MODULE_NEW      /* a first word no disc module of the slot has */
} ModelModuleClass;

typedef struct {
    ModelModuleClass kind;
    uint32_t identifier; /* the image's first word */
    int shape;           /* the slot A identifier of its shape, 0 when none is known */
    uint32_t code_size;  /* TABLES: where its code ends (the decomp's data_start) */
    uint32_t span;       /* TABLES: where the bytes the C's hash covers end */
} ModelModuleInfo;

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
 * (ModelModule_IsModsMips), with a "; "-separated line for each in `detail`. */
int ModelModule_StagesMips(const unsigned char *stages, char *detail, size_t size);

#endif
