#include "../types.h"
#include "ai.h"
#include "ai_constants.h"
#include "duel_calc_guardian_star_matchup.h"
#include "duel_card_layout.h"
#include "duel_grid.h"
#include "ai_script_commands.h"
#ifdef MEMORIES_PC
#include "ai_opponent_data.h"   /* gDuel_bOpponentID */
#include "pc/free_duel/duelists.h"
#include "pc/cards/monster_effects.h"
#endif
#include "ai_script_read_byte.h"
void AiScript_FindBestAttack(void) {
    AiActiveCard *t;
    AiActiveCard *r;
    AiActiveCard *e;
    s32 want;
    s32 out;
    s32 i;
    s32 j;
    s32 v;
    u16 f;

    want = gAiScript_aMemory[AiScript_ReadByte()];
#ifdef MEMORIES_PC
    /* "sight" in the duelist's "ai" over what the script asked for
       (pc/free_duel/duelists.h); its own answer when it says nothing. */
    want = Duelists_HidesFaceDown(gDuel_bOpponentID, want);
#endif
    out = AiScript_ReadByte();
    i = 1;
    t = gDuel_aActiveCards;
    r = &t[AI_SLOT_OWN_MONSTER_FIRST];
    gAiScript_State.attack_best_stat = 0;

    while (i < DUEL_FIELD_ROW_SIZE + 1) {
        if (r->card_id == 0) {
            continue;
        }
        if ((r->flags & DUEL_CARD_FLAG_USED_THIS_TURN) != 0) {
            continue;
        }

        for (j = AI_SLOT_OPPONENT_MONSTER_FIRST,
             e = &t[AI_SLOT_OPPONENT_MONSTER_FIRST];
             j < AI_SLOT_OPPONENT_MONSTER_FIRST + AI_ACTIVE_CARD_ROW_SLOT_COUNT;
             j++, e++) {
            if (e->card_id == 0) {
                continue;
            }
            f = e->flags;
            if ((f & DUEL_CARD_FLAG_USED_THIS_TURN) != 0) {
                continue;
            }
#ifdef MEMORIES_PC
            {
                unsigned attacker_effects = MonsterEffects_CombatFlags(r->card_id);
                unsigned defender_effects = MonsterEffects_CombatFlags(e->card_id);
                unsigned combined = attacker_effects | defender_effects;
                int forced_kill = (combined & MONSTER_COMBAT_DESTROY_BOTH) ||
                    ((combined & MONSTER_COMBAT_DESTROY_NON_IMMUNE) &&
                     !(defender_effects & MONSTER_COMBAT_BATTLE_IMMUNE));
                /* Piercing and either combatant's forced-destruction rule
                 * both make a defender a meaningful attack target. */
                if ((f & DUEL_CARD_FLAG_DEFENSE_POSITION) != 0 &&
                    !(attacker_effects & MONSTER_COMBAT_PIERCING) && !forced_kill)
                    continue;
                if (want != 0 && (f & DUEL_CARD_FLAG_FACE_DOWN) != 0) continue;
                v = r->attack - ((f & DUEL_CARD_FLAG_DEFENSE_POSITION) ? e->defense : e->attack);
                v += Duel_CalcGuardianStarMatchup(r->guardian_star, e->guardian_star);
                /* Immunity prevents removal, not LP damage from winning an
                 * attack-position battle.  destroy_both overrides it. */
                if ((defender_effects & MONSTER_COMBAT_BATTLE_IMMUNE) &&
                    !(combined & MONSTER_COMBAT_DESTROY_BOTH) && v <= 0)
                    continue;
                if (forced_kill && v <= 0) v = 1;
            }
#else
            if ((f & DUEL_CARD_FLAG_DEFENSE_POSITION) != 0) {
                continue;
            }
            if (want != 0) {
                if ((f & DUEL_CARD_FLAG_FACE_DOWN) != 0) {
                    continue;
                }
            }
            v = r->attack - ((f & DUEL_CARD_FLAG_DEFENSE_POSITION) ? e->defense : e->attack);
            v += Duel_CalcGuardianStarMatchup(r->guardian_star, e->guardian_star);
#endif
            if (gAiScript_State.attack_best_stat < v) {
                gAiScript_State.attack_best_stat = v;
                gAiScript_State.attack_best_slot = i;
                gAiScript_State.attack_best_target = j;
            }
        }
    }

    if (gAi_wBestDifference[0] != 0) {
        gAiScript_aMemory[out] = 0;
    } else {
        gAiScript_aMemory[out] = 1;
    }
}
