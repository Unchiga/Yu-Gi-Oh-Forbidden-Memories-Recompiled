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
            /* Piercing attackers deliberately consider defenders too. */
#ifdef MEMORIES_PC
            if ((f & DUEL_CARD_FLAG_DEFENSE_POSITION) != 0 &&
                !(MonsterEffects_CombatFlags(r->card_id) & MONSTER_COMBAT_PIERCING)) {
#else
            if ((f & DUEL_CARD_FLAG_DEFENSE_POSITION) != 0) {
#endif
                continue;
            }
            if (want != 0) {
                if ((f & DUEL_CARD_FLAG_FACE_DOWN) != 0) {
                    continue;
                }
            }
            v = r->attack - ((f & DUEL_CARD_FLAG_DEFENSE_POSITION) ? e->defense : e->attack);
            v += Duel_CalcGuardianStarMatchup(r->guardian_star, e->guardian_star);
#ifdef MEMORIES_PC
            {
                unsigned attacker_effects = MonsterEffects_CombatFlags(r->card_id);
                unsigned defender_effects = MonsterEffects_CombatFlags(e->card_id);
                int forced = attacker_effects & (MONSTER_COMBAT_DESTROY_BOTH | MONSTER_COMBAT_DESTROY_NON_IMMUNE);
                if ((defender_effects & MONSTER_COMBAT_BATTLE_IMMUNE) &&
                    !(attacker_effects & MONSTER_COMBAT_DESTROY_BOTH))
                    continue;
                /* A forced destruction can deliberately trade a weaker
                 * monster for a stronger one; make it selectable. */
                if (forced && !(defender_effects & MONSTER_COMBAT_BATTLE_IMMUNE) && v <= 0) v = 1;
            }
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
