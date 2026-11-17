#include "pc/compat/fs.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc/guest/mips.h"
#include "pc/guest/retail_image.h"

/* The credits module, 16 SU sectors the ending (model_intro_controller.c)
 * loads over the main menu at 0x80180000 and enters at three addresses:
 * set-up, the per-frame update (nonzero when the roll is over) and the
 * start of a group of lines. The decomp matched all of it
 * (src/overlays/credits, prefixed credits__ by the build), and that C runs
 * whenever the delivered bytes are the retail module
 * (src/pc/guest/retail_image.c). The game has no MIPS interpreter
 * (notes/pc-build.md, "The reference build"): credits a mod or patch changed
 * are skipped, said once, the update reporting the roll over at once so the
 * ending goes on. A reference build (MEMORIES_REFERENCE, for tests)
 * interprets them instead, and MEMORIES_CREDITS=interpreter interprets them
 * even when they are retail there (a game without the interpreter says so
 * once and runs the default). */
extern void credits__func_801807B0(void);
extern int credits__func_80180A24(void);
extern void credits__func_80181C4C(int group);

static int native(void)
{
    static int forced = -1;
    if (forced < 0) {
        const char *setting = getenv("MEMORIES_CREDITS");
        forced = setting && strcmp(setting, "interpreter") == 0;
#ifndef MEMORIES_REFERENCE
        if (forced) Memories_NoInterpreter("MEMORIES_CREDITS=interpreter");
        forced = 0;
#endif
    }
    return !forced && RetailImage_Verified(RETAIL_IMAGE_CREDITS);
}

#ifndef MEMORIES_REFERENCE
/* Credits that are not the disc's, in a game without the interpreter. */
static void skipped(void)
{
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "memories-pc: the credits are not the disc's (a mod or patch changed them), and this game has "
                        "no MIPS interpreter: they are skipped\n");
    }
}
#endif

void Memories_CreditsInit(void)
{
    if (native()) {
        credits__func_801807B0();
    } else {
#ifdef MEMORIES_REFERENCE
        Memories_MipsCall(0x801807B0u, 0, 0, 0, 0);
#else
        skipped();
#endif
    }
}

int Memories_CreditsUpdate(void)
{
    if (native()) {
        return credits__func_80180A24();
    }
#ifdef MEMORIES_REFERENCE
    return (int)Memories_MipsCall(0x80180A24u, 0, 0, 0, 0);
#else
    skipped();
    return 1; /* the roll is over */
#endif
}

void Memories_CreditsLines(int group)
{
    if (native()) {
        credits__func_80181C4C(group);
    } else {
#ifdef MEMORIES_REFERENCE
        Memories_MipsCall(0x80181C4Cu, (uint32_t)group, 0, 0, 0);
#else
        (void)group;
        skipped();
#endif
    }
}
