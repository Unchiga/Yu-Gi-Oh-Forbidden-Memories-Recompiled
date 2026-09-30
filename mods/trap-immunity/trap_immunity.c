#include "types.h"
#include "pc/mods/modapi.h"
#include "card_constants.h"
#include "duel_card.h"
#include "duel_action_lock.h"
#include "duel_side_state.h"
#include "duel_trap_resolution.h"

static const MemoriesModHost *host;
static void *original_select_attack_trap;
static void *original_remove_from_field;

static int trap_immune_setting(s16 card_id)
{
    switch (card_id) {
    case 380: return host->setting(host, "card_380", 1);
    case 357: return host->setting(host, "card_357", 1);
    case 529: return host->setting(host, "card_529", 1);
    case 613: return host->setting(host, "card_613", 1);
    case 32:  return host->setting(host, "card_32", 1);
    case 298: return host->setting(host, "card_298", 1);
    case 218: return host->setting(host, "card_218", 1);
    case 359: return host->setting(host, "card_359", 1);
    case 63:  return host->setting(host, "card_63", 1);
    case 532: return host->setting(host, "card_532", 1);
    case 248: return host->setting(host, "card_248", 1);
    default: return 0;
    }
}

static int card_is_trap(s16 card_id)
{
    if (card_id <= 0 || card_id > CARD_TABLE_COUNT)
        return 0;

    return ((gDuel_adwCardStats[card_id - 1] >> CARD_STAT_TYPE_SHIFT) &
            CARD_STAT_TYPE_MASK) == CARD_TYPE_TRAP;
}

static int trap_is_active(void)
{
    /* Normal trap effects use gDuel_wEffectCardID. Attack traps use
       D_8009B22A while the battle trap is being selected/presented. */
    return card_is_trap(gDuel_wEffectCardID) ||
           card_is_trap(D_8009B22A);
}

static int monster_is_immune(DuelCardRecord *card)
{
    return card != 0 && trap_immune_setting(card->card_id) != 0;
}

static s32 select_attack_trap(u8 *record)
{
    DuelCardRecord *card = (DuelCardRecord *)record;

    if (card != 0 && monster_is_immune(card))
        return 0;

    return ((s32 (*)(u8 *))original_select_attack_trap)(record);
}

static void remove_from_field(DuelCardRecord *card)
{
    if (card != 0 && monster_is_immune(card) && trap_is_active())
        return;

    ((void (*)(DuelCardRecord *))original_remove_from_field)(card);
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    int a;
    int b;

    if (from->api < 4)
        return 0;

    host = from;
    mod->api = 4;

    a = host->hook(host,
                   (void *)Duel_SelectAttackTrap,
                   (void *)select_attack_trap,
                   &original_select_attack_trap);
    if (!a)
        return 0;

    b = host->hook(host,
                   (void *)DuelCard_RemoveFromField,
                   (void *)remove_from_field,
                   &original_remove_from_field);
    if (!b) {
        host->unhook(host, a);
        return 0;
    }

    return 1;
}
