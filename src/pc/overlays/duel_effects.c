#include "pc/compat/fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/duel_effect_request.h"
#include "game/duel_side_state.h"
#include "pc/guest/mips.h"
#include "pc/guest/retail_image.h"
#include "pc/debug/log.h"
#include "pc/cards/stars.h"
#include "pc/mods/mods.h"

extern unsigned char D_8009B261;
extern unsigned Memories_PresentedFrames(void);

/* The bank's entry, src/overlays/duel_effects/dispatch.c (the decomp names
 * its functions by their PAL addresses; this one is at 0x801462B0 here).
 * The build prefixes every name the module defines (GATED_MODULES). */
extern void duel_effects__func_80146258(int effect, int phase, void *buffer, DuelEffectRequest *context);

enum { MODE_AUTO, MODE_INTERPRETER, MODE_SKIP };

/* The common WA overlay dispatcher at 0x801462B0, the entry of the bank the
 * game's loader places at 0x80146000 with every duel package. It holds all
 * the field and card effects (fusion, battle damage, destruction, magic,
 * trap, ritual, terrain, and the rest: ids 0-24).
 *
 * The decomp matched the whole North American bank, and its C
 * (src/overlays/duel_effects) is linked in: that runs whenever the bytes the
 * disc delivered are the retail bank (src/pc/guest/retail_image.c). The game
 * has no MIPS interpreter (notes/pc-build.md, "The reference build"), so a
 * bank a mod or disc patch changed has its effects reported complete on
 * their first update, said once. MEMORIES_TRACE=mods reports which of the two
 * each delivery got. MEMORIES_DUEL_EFFECTS=skip restores the bring-up
 * behaviour (it was called `native` before the bank ran as native C;
 * `native` now means the default): fusion (1), battle damage (2) and
 * destruction (3) only (the bank's C, for the retail bank), everything else
 * complete on its first update.
 *
 * A reference build (MEMORIES_REFERENCE, for tests) has the interpreter: it
 * runs a changed bank's delivered MIPS, bridging its resident SDK and game
 * calls to the native port; MEMORIES_DUEL_EFFECTS=interpreter interprets the
 * bank even when it is retail (the reference for comparing the two; a game
 * without the interpreter says so once and runs the default), skip
 * interprets ids 1-3, and an effect the interpreter cannot run is reported
 * once and completed at once from then on. */
#ifndef MEMORIES_REFERENCE
/* An effect completed at once and unseen, in a game without the MIPS
 * interpreter: the request leaves the pool (D_8009B261), with its hand-off
 * byte (field_1D) where its caller waits for it, as the bank's effect leaves
 * it, so the duel goes on as on the console:
 *   0x10 (Raigeki) raises the byte once a bolt, 1 to 5, and its caller
 *        destroys the opponent's monster of zone N when the byte is N + 1
 *        (DuelEffect_ApplyRaigeki): one step an update, then the request
 *        ends, so every zone is cleared;
 *   0x15 (Swords of Revealing Light's swords) lives for the turns the swords
 *        stay: its caller keeps the request in gDuel_apSwordsEffectObjects
 *        and writes the end into it at expiry (duel_phase_entry.c), before
 *        it forgets it. The byte is 2 (the swords are down,
 *        DuelEffect_ApplySwords), and the request ends once neither side's
 *        pointer is it (expired, or another Swords took the side), so no
 *        pointer is ever a freed entry;
 *   every other id: 1, which every caller tests for nonzero (the terrain,
 *        the rituals, the equips, the battles; without it their card's use
 *        never ended). */
static void complete_at_once(unsigned index, DuelEffectRequest *request)
{
    if (index == 0x10) {
        if (request->field_1D < 5) {
            request->field_1D++;
            return;
        }
    } else if (index == 0x15) {
        request->field_1D = 2;
        if ((DuelFieldEffectObject *)request == gDuel_apSwordsEffectObjects[0] ||
            (DuelFieldEffectObject *)request == gDuel_apSwordsEffectObjects[1])
            return;
    } else {
        request->field_1D = 1;
    }
    D_8009B261 = 1;
}
#endif

void Memories_DuelEffectControl(short id, short state, int buffer, DuelEffectRequest *request)
{
    static int mode = -1;
    static unsigned failed[8];
#ifdef MEMORIES_REFERENCE
    uint32_t args[4], result;
#else
    static int said;
#endif
    int native, tpage, u, v, clut;
    unsigned index = (unsigned)(unsigned short)id;

    if (mode < 0) {
        const char *setting = getenv("MEMORIES_DUEL_EFFECTS");
        mode = !setting ? MODE_AUTO
             : strcmp(setting, "interpreter") == 0 ? MODE_INTERPRETER
             : strcmp(setting, "skip") == 0 ? MODE_SKIP
             : MODE_AUTO;
#ifndef MEMORIES_REFERENCE
        if (mode == MODE_INTERPRETER) {
            Memories_NoInterpreter("MEMORIES_DUEL_EFFECTS=interpreter");
            mode = MODE_AUTO;
        }
#endif
    }
    LOG(LOG_DUEL_EFFECTS, "id=%d state=%d buffer=%08x payload=%d,%d,%d damage=%d",
        id, state, (unsigned)buffer, request->field_00, request->field_02, request->field_04, request->field_12);
#ifdef MEMORIES_REFERENCE
    native = mode == MODE_AUTO && RetailImage_Verified(RETAIL_IMAGE_DUEL_EFFECTS);
#else
    /* No interpreter: the bank's C runs whatever the mode, for the retail
     * bank (skip leaves out all but 1-3, below), and for a bank whose code
     * is the retail one when an applied mod asks for its own data
     * ("duel_effects": "own data", Mods_DuelEffectsOwnData): the C reads the
     * bank's tables where the disc put them, so it then runs the mod's,
     * unchecked. Anything else completes at once. */
    native = RetailImage_Verified(RETAIL_IMAGE_DUEL_EFFECTS);
    if (!native) {
        int data_only = RetailImage_CodeVerified(RETAIL_IMAGE_DUEL_EFFECTS);
        const char *owner = data_only ? Mods_DuelEffectsOwnData() : NULL;
        if (owner) {
            native = 1;
            if (said != 2) {
                said = 2;
                fprintf(stderr, "memories-pc: mod %s: the duel effect bank's data is the mod's own (\"duel_effects\": "
                                "\"own data\"), and its code the disc's: the bank's C runs the mod's tables, which "
                                "nothing checks (a value the C divides by or indexes with can stop the game)\n", owner);
            }
        } else {
            if (!said) {
                said = 1;
                if (data_only)
                    fprintf(stderr, "memories-pc: the duel effect bank's data is not the disc's (a mod or patch changed "
                                    "its tables; its code is the disc's), and this game has no MIPS interpreter: its "
                                    "effects complete at once, unseen. A mod that means it adds \"duel_effects\": "
                                    "\"own data\" to its mod.json (sdk/tools/model_gate.py effect-migrate writes it), "
                                    "and the bank's C then runs its tables (effect %d first)\n", id);
                else
                    fprintf(stderr, "memories-pc: the duel effect bank's code is not the disc's (a mod or patch changed "
                                    "it), and this game has no MIPS interpreter: its effects complete at once, unseen "
                                    "(sdk/tools/model_gate.py effect-migrate names what changed; effect %d first)\n",
                            id);
            }
            complete_at_once(index, request);
            return;
        }
    }
    if (mode == MODE_SKIP && !(id >= 1 && id <= 3)) {
        complete_at_once(index, request);
        return;
    }
#endif
    /* The battle's guardian star (0xE): its first update gets the star
       less one and picks the icon's cell, u at +0x8C4, v +0x8C6, page
       +0x8C8, palette +0x8CA; past the disc's ten it draws an error cross.
       A star with a mod's icon, or past ten, starts as the first star and
       then takes its own cell (stars.h), native or interpreted. */
    if (id == 0xE && state >= 0 && mode != MODE_SKIP && index < 256 && !(failed[index >> 5] & (1u << (index & 31))) &&
        Stars_EffectCell(state + 1, &tpage, &u, &v, &clut)) {
        uint16_t *cell = (uint16_t *)(uintptr_t)(unsigned)(buffer + 0x8C4);

        if (native) {
            duel_effects__func_80146258(id, 0, (void *)(uintptr_t)(unsigned)buffer, request);
        }
#ifdef MEMORIES_REFERENCE
        else {
            args[0] = index;
            args[1] = 0;
            args[2] = (uint32_t)buffer;
            args[3] = (uint32_t)(uintptr_t)request;
            if (Memories_MipsTry(0x801462B0u, args, 4, &result)) {
                fprintf(stderr, "memories-pc: duel effect %d cannot run; completing it at once from now on\n", id);
                failed[index >> 5] |= 1u << (index & 31);
                D_8009B261 = 1;
                return;
            }
        }
#endif
        cell[0] = (uint16_t)u;
        cell[1] = (uint16_t)v;
        cell[2] = (uint16_t)((cell[2] & 0x60) | tpage);
        cell[3] = (uint16_t)clut;
        return;
    }
    if (native) {
        duel_effects__func_80146258(id, state, (void *)(uintptr_t)(unsigned)buffer, request);
        return;
    }
#ifdef MEMORIES_REFERENCE
    if ((mode == MODE_SKIP && !(id >= 1 && id <= 3)) || index >= 256 || (failed[index >> 5] & (1u << (index & 31)))) {
        D_8009B261 = 1;
        return;
    }
    args[0] = index;
    args[1] = (uint32_t)(int)state;
    args[2] = (uint32_t)buffer;
    args[3] = (uint32_t)(uintptr_t)request;
    if (Memories_MipsTry(0x801462B0u, args, 4, &result)) {
        fprintf(stderr, "memories-pc: duel effect %d cannot run; completing it at once from now on\n", id);
        failed[index >> 5] |= 1u << (index & 31);
        D_8009B261 = 1;
    }
#endif
}
