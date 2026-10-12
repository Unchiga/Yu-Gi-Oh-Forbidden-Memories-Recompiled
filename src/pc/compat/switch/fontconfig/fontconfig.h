/* The few fontconfig calls the port makes (menu.c, glyphs.c, cards/art.c,
 * libapi_krom.c), for the Switch, which has no fontconfig: every pattern
 * gets the console's own shared font (Japanese and Latin in one), copied to
 * the SD card as a TTF on first use (src/pc/platform/switch_fonts.c). Only
 * the Switch build has this folder on its include path
 * (tools/pc/switch_deps.py). The Android stand-in has the same shape. */
#ifndef MEMORIES_PC_COMPAT_SWITCH_FONTCONFIG_H
#define MEMORIES_PC_COMPAT_SWITCH_FONTCONFIG_H
#include <stdlib.h>
#include <string.h>

typedef unsigned char FcChar8;
typedef int FcBool;
typedef enum { FcResultMatch, FcResultNoMatch } FcResult;
typedef enum { FcMatchPattern } FcMatchKind;
typedef struct { const char *file; } FcPattern;
#define FC_FILE "file"

/* The shared font's TTF on the SD card, or NULL when it cannot be had. */
const char *SwitchFonts_Path(void);

static inline FcBool FcInit(void) { return 1; }

static inline FcPattern *FcNameParse(const FcChar8 *name)
{
    FcPattern *pattern = (FcPattern *)malloc(sizeof(FcPattern));
    (void)name;
    if (!pattern) return NULL;
    pattern->file = SwitchFonts_Path();
    return pattern;
}

static inline FcBool FcConfigSubstitute(void *config, FcPattern *pattern, FcMatchKind kind)
{
    (void)config; (void)pattern; (void)kind;
    return 1;
}

static inline void FcDefaultSubstitute(FcPattern *pattern) { (void)pattern; }

static inline FcPattern *FcFontMatch(void *config, FcPattern *pattern, FcResult *result)
{
    FcPattern *match;
    (void)config;
    if (!pattern || !pattern->file || !(match = (FcPattern *)malloc(sizeof(FcPattern)))) {
        *result = FcResultNoMatch;
        return NULL;
    }
    *match = *pattern;
    *result = FcResultMatch;
    return match;
}

static inline FcResult FcPatternGetString(const FcPattern *pattern, const char *object, int n, FcChar8 **value)
{
    if (!pattern || !pattern->file || n || strcmp(object, FC_FILE)) return FcResultNoMatch;
    *value = (FcChar8 *)pattern->file;
    return FcResultMatch;
}

static inline void FcPatternDestroy(FcPattern *pattern) { free(pattern); }
#endif
