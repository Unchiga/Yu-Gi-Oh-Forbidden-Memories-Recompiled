/* Runtime combat scenarios for Battle Immunity and Piercing in 2D and 3D battle modes.
 *
 * Verifies:
 * Scenario 1 (2D & 3D): CPU Piercing vs Player Immune DEF:
 *   - AI Hard Mode chooses to attack Player's immune DEF wall for piercing damage.
 *   - 2D (PHASE_BATTLE): Piercing LP damage is inflicted; defender is preserved on field.
 *   - 3D (PHASE_RESUME): Piercing LP damage is inflicted; defender is spared from D_8009B208.
 *
 * Scenario 2 (2D & 3D): CPU Multi-Attack Focus vs 2-Hit Shield:
 *   - AI Hard Mode identifies 2-hit shield (destroyed_after_2_attacks) and coordinates 2 attacks.
 *   - Hit 1: Monster survives battle, shield absorbs destruction, attacked counter increments.
 *   - Hit 2: Shield is broken, monster is destroyed.
 *
 * Scenario 3 (2D & 3D): Player Piercing vs CPU Immune DEF & Non-Immune DEF:
 *   - 3a: Player attacks CPU immune DEF monster: LP damage inflicted, CPU monster survives.
 *   - 3b: Player attacks CPU normal DEF monster: LP damage inflicted, CPU monster is destroyed.
 *
 * Scenario 4 (2D & 3D): CPU Attacks Player Battle-Immune ATK:
 *   - AI Hard Mode attacks Player's battle-immune ATK monster for battle LP damage.
 *   - Battle damage is inflicted, Player monster survives.
 *
 * Scenario 5 (2D & 3D): Player Attacks CPU Battle-Immune ATK/DEF:
 *   - Player attacks CPU battle-immune monster.
 *   - Normal battle damage is dealt, CPU monster survives.
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#define D_8009B360_AS_SIDE_ARRAY

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "game/display_object.h"
#include "game/duel_card.h"
#include "game/duel_effect_request.h"
#include "game/duel_side_state.h"
#include "pc/cards/monster_effects.h"
#include "pc/cards/monster_effects_duel.h"
#include "types.h"

#include "../../mods/ai-hard-mode/planner.h"
#include "../../mods/ai-hard-mode/ai_hard_mode.c"

/* --- Game State Globals --- */
u16 gDuel_wSceneStateFlags = 0;
DuelSideState D_800E9FF0[DUEL_SIDE_COUNT];
DuelSideState *D_8009B1C8 = &D_800E9FF0[0];
u8 D_800907D8[40] = {0};
u8 *D_8009B22C = 0;
u8 D_8009B1D5 = 0;
u8 D_8009B174 = 0;
u8 D_8009B1B8 = 0;
u16 D_8009B162 = 0;
s8 D_8009B1B0[2] = {0, 0};
s8 D_8009B208 = -1;
s8 D_8009B209 = -1;
u8 D_8009B229 = 0; /* 3D presentation flag */
s16 D_8009B22A = 0;
u16 D_8009B178[2] = {0, 0};
u16 D_8009B1A4[2] = {0, 0};
s8 D_8009B1B9 = 0;
u8 *D_8009B17C = 0;
u8 D_800E9F64[256];
void *D_800E9EF0[4] = {0};
u8 D_800EAE88[16] = {0};
DuelCardRecord D_801A7AD8[30];
s32 gDuel_adwCardStats[1024];
u8 gDuel_bEffectState = 0;
u16 gDuel_wCardEffectFlags = 0;
u8 gDuel_bQuitDialogState = 0;
u8 gDuel_bTerrain = 0;

/* AI Script and Adapter Globals */
AiScriptState gAiScript_State;
AiActiveCard gDuel_aActiveCards[112];
s32 gAiScript_aMemory[20];
AiOpponentData gDuel_aOpponentData[40];
s8 gDuel_bOpponentID = 8;
s8 D_8009B360[DUEL_SIDE_COUNT] = {-1, 8};
u8 D_801A8000[6144], D_801A9800[6144], D_800907CC[10];
DuelDeckCardRecord gDuel_aDeckCardRecords[80];
DuelHandSlot D_800EA030[5];
s8 gDuel_aTerrainBoost[20][6];
static DuelCardDisplayObject s_objects[10];
static int s_removed_records[30];
static int s_removed_count = 0;

/* Monster effects registered in test */
MonsterEffectsState gMonsterEffects = {0};
const char *const MonsterEffect_DoNames[MONSTER_DO_COUNT] = {
    "magic", "boost", "heal", "damage", "destroy", "change_field", "battle_immunity", "piercing"
};
const char *const MonsterEffect_WhenNames[MONSTER_WHEN_COUNT] = {
    "summon", "flip", "draw", "combat", "destroyed", "destroy_opponent", "face_up"
};

/* Registered mod effects by card ID */
static MonsterEffect s_card_effects[1024][4];
static MonsterEffectEx s_card_effects_ex[1024][4];
static int s_card_effect_counts[1024];

static inline int is_monster_zone(int rec)
{
    return (rec >= 5 && rec <= 9) || (rec >= 20 && rec <= 24);
}

int Cards_Valid(int card) { return card > 0 && card < 1024; }
int Cards_AiId(int id) { return id; }
int Cards_EffectId(int id) { return id; }

int Cards_MonsterEffects(int card, const MonsterEffect **effects)
{
    if (card <= 0 || card >= 1024 || s_card_effect_counts[card] == 0) return 0;
    if (effects) *effects = s_card_effects[card];
    return s_card_effect_counts[card];
}

int Cards_CardEffects(int card, const MonsterEffect **effects)
{
    return Cards_MonsterEffects(card, effects);
}

int Cards_MonsterEffectsEx(int card, const MonsterEffectEx **effects_ex)
{
    if (card <= 0 || card >= 1024 || s_card_effect_counts[card] == 0) {
        if (effects_ex) *effects_ex = NULL;
        return 0;
    }
    if (effects_ex) *effects_ex = s_card_effects_ex[card];
    return s_card_effect_counts[card];
}

int Cards_Type(int card) { (void)card; return 0; }
int Cards_Attribute(int card) { (void)card; return 0; }
int Cards_Level(int card) { (void)card; return 1; }
int Tables_StatCapEither(void) { return 9999; }
int Mods_Dispatch(int hook, void *data) { (void)hook; (void)data; return 0; }

DuelEffectRequest *DuelEffect_CreateRequest(int id)
{
    (void)id;
    static DuelEffectRequest dummy;
    memset(&dummy, 0, sizeof(dummy));
    return &dummy;
}

int Fields_Declared(int field) { return field >= 1 && field <= 6; }
int Fields_Carrier(int field) { return field; }
void Fields_Activate(int field) { (void)field; }
void SD_SEPlayFull(u32 sound) { (void)sound; }
void DuelEffect_StartRetailCardEffect(int card, int param) { (void)card; (void)param; }

void DuelCard_RemoveFromField(const DuelCardRecord *card)
{
    if (!card) return;
    int idx = (int)(card - D_801A7AD8);
    if (idx >= 0 && idx < 30) {
        s_removed_records[s_removed_count++] = idx;
        D_801A7AD8[idx].flags &= ~DUEL_CARD_FLAG_OCCUPIED;
    }
}

int announce(int card, int record, int when, int phase)
{
    (void)card; (void)record; (void)when; (void)phase;
    return 0;
}

int s32_min(int a, int b) { return a < b ? a : b; }
int Log_Wanted(int cat) { (void)cat; return 0; }
void Log_Printf(int cat, const char *fmt, ...) { (void)cat; (void)fmt; }

int Mods_DamageLife(int side, int life, int damage, int kind)
{
    (void)kind;
    int remaining = life - damage;
    if (remaining < 0) remaining = 0;
    D_800E9FF0[side].life_points.signed_value = (short)remaining;
    return remaining;
}

s32 Duel_CalcBattleAttack(DuelCardRecord *attacker, DuelCardRecord *defender)
{
    (void)defender;
    int rec = (int)(attacker - D_801A7AD8);
    return gDuel_adwCardStats[rec] & 0xFFFF;
}

s32 Duel_CalcCardStats(DuelCardRecord *card)
{
    int rec = (int)(card - D_801A7AD8);
    int atk = gDuel_adwCardStats[rec] & 0xFFFF;
    int def = (gDuel_adwCardStats[rec] >> 16) & 0xFFFF;
    return (s32)((unsigned int)atk | ((unsigned int)def << 16));
}

int func_8001EFD4(DisplayObject *attacker, DisplayObject *defender)
{
    int att_rec = attacker->field_6A;
    int def_rec = defender->field_6A;
    int att_atk = gDuel_adwCardStats[att_rec] & 0xFFFF;
    int def_val = (D_801A7AD8[def_rec].flags & DUEL_CARD_FLAG_DEFENSE_POSITION)
                      ? ((gDuel_adwCardStats[def_rec] >> 16) & 0xFFFF)
                      : (gDuel_adwCardStats[def_rec] & 0xFFFF);
    return att_atk - def_val;
}

int Tables_TerrainBonus(int field, int type, int *bonus)
{
    (void)field; (void)type; (void)bonus;
    return 0;
}

long Tables_Value(int which, long retail) { (void)which; return retail; }
s32 Duel_CalcGuardianStarMatchup(s32 attacker, s32 defender) { (void)attacker; (void)defender; return 0; }
s32 Duel_CheckFusion(s32 a, s32 b) { (void)a; (void)b; return 0; }
s32 Duel_CheckEquip(s32 a, s32 b) { (void)a; (void)b; return 0; }
s32 Duel_CheckRitual(DuelRitualResult *out, s32 id) { (void)out; (void)id; return 0; }
void func_80028220(void) { }
void AiScript_Init(u8 *script) { memset(&gAiScript_State, 0, sizeof(gAiScript_State)); gAiScript_State.script_base = script; }
s32 AiScript_Run(void) { return 1; }
s32 Ai_GetHandSize(void) { return 5; }
void func_8001BAF0(void) { }
void AiScript_FindStrongest(void) { }
void AiScript_FindWeakest(void) { }
void AiScript_FindCard(void) { }
void AiScript_MatchType(void) { }
void AiScript_FindDefenseStopper(void) { }

u8 *Duel_SetupCardRecord(s32 index, s32 deck)
{
    D_801A7AD8[index].card_id = gDuel_aDeckCardRecords[deck].id;
    return 0;
}

DuelCardDisplayObject *func_80018004(DuelCardRecord *record, s32 x, s32 y)
{
    (void)record; (void)x; (void)y;
    return &s_objects[0];
}

void DisplayObject_ReleaseIfPresent(void *p) { (void)p; }

static int host_setting(const MemoriesModHost *h, const char *key, int fallback)
{
    (void)h;
    if (!strcmp(key, "deck_access")) return 0;
    if (!strcmp(key, "hidden_cards")) return 2; /* Full visibility for testing */
    if (!strcmp(key, "hand_planning")) return 1;
    return fallback;
}

static int host_hook(const MemoriesModHost *h, void *fn, void *replacement, void **previous)
{
    (void)h; (void)fn; (void)replacement;
    if (previous) *previous = fn;
    return 1;
}

static MemoriesModHost s_test_host;

/* --- Helpers to Configure Cards & Board --- */
static void clear_all(void)
{
    memset(s_card_effects, 0, sizeof(s_card_effects));
    memset(s_card_effects_ex, 0, sizeof(s_card_effects_ex));
    memset(s_card_effect_counts, 0, sizeof(s_card_effect_counts));
    memset(&gMonsterEffects, 0, sizeof(gMonsterEffects));
    memset(D_801A7AD8, 0, sizeof(D_801A7AD8));
    memset(gDuel_aActiveCards, 0, sizeof(gDuel_aActiveCards));
    memset(D_800E9FF0, 0, sizeof(D_800E9FF0));
    memset(s_removed_records, 0, sizeof(s_removed_records));
    s_removed_count = 0;

    for (int i = 0; i < DUEL_SIDE_COUNT; i++) {
        D_800E9FF0[i].life_points.signed_value = 8000;
        D_800E9FF0[i].max_life_points = 8000;
    }
    D_8009B1D5 = 0;
    D_8009B174 = 0;
    D_8009B1B0[0] = D_8009B1B0[1] = 0;
    D_8009B208 = D_8009B209 = -1;
    D_8009B229 = 0;
    D_8009B178[0] = D_8009B178[1] = 0;
    D_8009B1A4[0] = D_8009B1A4[1] = 0;
    gDuel_wSceneStateFlags = 0;
    gMonsterEffects.ready = 1;
    initialize(D_801A8000);
}

static void register_effect(int card_id, int action, int battle_mod, int battle_reset)
{
    int count = s_card_effect_counts[card_id];
    assert(count < 4);
    s_card_effects[card_id][count].when = MONSTER_WHEN_FACE_UP;
    s_card_effects[card_id][count].action = (unsigned char)action;
    s_card_effects[card_id][count].target = MONSTER_TARGET_SELF;

    s_card_effects_ex[card_id][count].battle_modifier = (unsigned char)battle_mod;
    s_card_effects_ex[card_id][count].battle_reset = (unsigned char)battle_reset;
    s_card_effect_counts[card_id] = count + 1;
}

static DisplayObject s_disp_obj_att = {0};
static DisplayObject s_disp_obj_def = {0};

/* Setup a monster on board:
 * side 0 (player): record = 5 + zone (5..9)
 * side 1 (cpu):    record = 20 + zone (20..24)
 */
static void place_monster(int side, int zone, int card_id, int atk, int def, int is_def_pos)
{
    int rec = (side == 0) ? (5 + zone) : (20 + zone);
    D_801A7AD8[rec].card_id = (short)card_id;
    D_801A7AD8[rec].flags = DUEL_CARD_FLAG_OCCUPIED | (is_def_pos ? DUEL_CARD_FLAG_DEFENSE_POSITION : 0);
    gDuel_adwCardStats[rec] = (atk & 0xFFFF) | ((def & 0xFFFF) << 16);

    gMonsterEffects.card[rec] = (short)card_id;
    gMonsterEffects.face_up[rec] = 1;
    gMonsterEffects.attacked_duel[rec] = (unsigned char)MonsterEffects_DuelCardAttacks(side, card_id);

    /* Populate AI active cards view */
    int active_idx = (side == 0) ? (56 + zone) : (1 + zone);
    gDuel_aActiveCards[active_idx].card_id = (short)card_id;
    gDuel_aActiveCards[active_idx].attack = (short)atk;
    gDuel_aActiveCards[active_idx].defense = (short)def;
    gDuel_aActiveCards[active_idx].flags = (is_def_pos ? HM_DEFENSE : 0);
}

/* Execute battle in 2D or 3D presentation:
 * attacker_rec: record of attacking monster
 * defender_rec: record of defending monster
 * is_3d: 0 for 2D, 1 for 3D
 */
static void execute_combat(int attacker_rec, int defender_rec, int is_3d)
{
    int att_side = (attacker_rec >= 20) ? 1 : 0;
    int def_side = att_side ^ 1;
    D_8009B1D5 = (u8)att_side;

    D_800E9EF0[0] = &s_disp_obj_att;
    D_800E9EF0[1] = &s_disp_obj_def;
    s_disp_obj_att.field_6A = (s8)attacker_rec;
    s_disp_obj_def.field_6A = (s8)defender_rec;

    gMonsterEffects.trap_battle_record[0] = (s8)attacker_rec;
    gMonsterEffects.trap_battle_record[1] = (s8)defender_rec;

    MonsterEffects_TrackBattleParticipants(attacker_rec, defender_rec);
    MonsterEffects_Battle();

    int is_def = (D_801A7AD8[defender_rec].flags & DUEL_CARD_FLAG_DEFENSE_POSITION) != 0;
    D_8009B178[0] = 0;
    D_8009B178[1] = is_def ? 0x800 : 0;

    int att_atk = gDuel_adwCardStats[attacker_rec] & 0xFFFF;
    int def_stat = is_def ? ((gDuel_adwCardStats[defender_rec] >> 16) & 0xFFFF)
                          : (gDuel_adwCardStats[defender_rec] & 0xFFFF);
    int diff = att_atk - def_stat;

    D_8009B229 = (u8)is_3d;
    D_8009B1B0[0] = 0;
    D_8009B1B0[1] = 0;
    D_8009B208 = -1;
    D_8009B209 = -1;

    /* Step 3: Base retail damage and defeat markings */
    if (diff > 0) {
        if (!is_def) {
            Mods_DamageLife(def_side, D_800E9FF0[def_side].life_points.signed_value, diff, 0);
        }
        D_8009B1B0[1] = -1; /* Defender marked destroyed */
        D_8009B208 = (s8)defender_rec;
    } else if (diff < 0) {
        Mods_DamageLife(att_side, D_800E9FF0[att_side].life_points.signed_value, -diff, 0);
        if (!is_def) {
            D_8009B1B0[0] = -1; /* Attacker marked destroyed */
            D_8009B208 = (s8)attacker_rec;
        }
    } else {
        if (!is_def) {
            D_8009B1B0[0] = -1;
            D_8009B1B0[1] = -1;
            D_8009B208 = (s8)attacker_rec;
            D_8009B209 = (s8)defender_rec;
        }
    }

    /* Step 4: Run MonsterEffects_Update() in PHASE_BATTLE */
    gDuel_wSceneStateFlags = 9; /* PHASE_BATTLE */
    D_8009B174 = 4;
    MonsterEffects_Update();

    if (is_3d) {
        /* In 3D presentation, battle scene transitions to models, then returns in PHASE_RESUME (11) */
        gDuel_wSceneStateFlags = 11; /* PHASE_RESUME */
        MonsterEffects_Update();

        /* DuelScene_UpdateResume removes cards if D_8009B208/D_8009B209 remain >= 0 */
        if (D_8009B208 >= 0 && is_monster_zone(D_8009B208)) {
            DuelCard_RemoveFromField(&D_801A7AD8[D_8009B208]);
        }
        if (D_8009B209 >= 0 && is_monster_zone(D_8009B209)) {
            DuelCard_RemoveFromField(&D_801A7AD8[D_8009B209]);
        }
    } else {
        /* In 2D presentation, DuelScene_UpdateBattle removes cards marked -1 in D_8009B1B0 */
        if (D_8009B1B0[0] == -1) DuelCard_RemoveFromField(&D_801A7AD8[attacker_rec]);
        if (D_8009B1B0[1] == -1) DuelCard_RemoveFromField(&D_801A7AD8[defender_rec]);
    }

    /* Back to field phase */
    gDuel_wSceneStateFlags = 5;
    D_8009B174 = 0;
    MonsterEffects_Update();
}

/* ========================================================================= */
/* SCENARIO 1: CPU Piercing vs Player Immune DEF                             */
/* ========================================================================= */
static void run_scenario_1(int is_3d)
{
    clear_all();
    const char *view = is_3d ? "3D" : "2D";

    /* Player: Kuriboh (card 18) in zone 0 DEF (200 DEF), unconditional battle immunity */
    register_effect(18, MONSTER_DO_BATTLE_IMMUNITY, MONSTER_BATTLE_MOD_ALWAYS, MONSTER_RESET_TURN);
    place_monster(0, 0, 18, 300, 200, 1);

    /* CPU: Meteor B. Dragon (card 713) in zone 0 ATK (3500 ATK), piercing damage */
    register_effect(713, MONSTER_DO_PIERCING, MONSTER_PIERCING_MOD_ALWAYS, MONSTER_RESET_TURN);
    place_monster(1, 0, 713, 3500, 2000, 0);

    /* 1. Verify AI Hard Mode planner decides to attack the immune DEF wall for piercing damage */
    D_8009B1D5 = 1; /* CPU turn */
    HmOptions opt = options();
    opt.attacks = 1;
    HmBoard b = board(&opt);
    assert(b.own_pierce[0] & (1u << 0));           /* CPU slot 0 pierces Player slot 0 */
    assert(b.enemy_immune[0] & 1);                 /* Player slot 0 has battle immunity */

    HmDecision dec = Hm_PlanField(&b, &opt, &rules, NULL);
    assert(dec.result == 2);
    assert(dec.selection[9] == 1);                 /* CPU slot 0 attacks */
    assert(dec.selection[10] == 56);               /* Targets Player slot 0 */

    /* 2. Execute combat in chosen view (2D or 3D) */
    int player_lp_before = D_800E9FF0[0].life_points.signed_value;
    execute_combat(20, 5, is_3d);
    int player_lp_after = D_800E9FF0[0].life_points.signed_value;

    /* Verify: Player took 3300 piercing LP damage */
    assert(player_lp_before - player_lp_after == 3300);
    /* Verify: Kuriboh survived battle (not destroyed, still occupied) */
    assert((D_801A7AD8[5].flags & DUEL_CARD_FLAG_OCCUPIED) != 0);

    printf("[%s] Scenario 1: CPU Piercing vs Player Immune DEF -> PASSED\n", view);
}

/* ========================================================================= */
/* SCENARIO 2: CPU Multi-Attack Focus vs 2-Hit Shield                        */
/* ========================================================================= */
static void run_scenario_2(int is_3d)
{
    clear_all();
    const char *view = is_3d ? "3D" : "2D";

    /* Player: Monster in zone 0 (1000 ATK, 1000 DEF) with multi-hit shield (first_attacked: survives 1st attack, destroyed on 2nd) */
    register_effect(50, MONSTER_DO_BATTLE_IMMUNITY, MONSTER_BATTLE_MOD_FIRST_ATTACKED, MONSTER_RESET_TURN);
    place_monster(0, 0, 50, 1000, 1000, 0);

    /* CPU: Two 3000 ATK monsters in zones 0 and 1 */
    place_monster(1, 0, 1, 3000, 2500, 0);
    place_monster(1, 1, 2, 2800, 2000, 0);

    /* 1. Verify AI Hard Mode planner detects multi-hit shield and chains both attacks */
    D_8009B1D5 = 1; /* CPU turn */
    gMonsterEffects.turn_side = 1;
    HmOptions opt = options();
    opt.attacks = 1;
    HmBoard b = board(&opt);
    assert(b.enemy_shields[0] == 1);               /* 1-hit shield recognized */

    HmDecision dec1 = Hm_PlanField(&b, &opt, &rules, NULL);
    assert(dec1.result == 2);
    assert(dec1.selection[10] == 56);              /* First attack targets slot 0 */

    /* 2. Execute first attack */
    execute_combat(20, 5, is_3d);
    /* Shield absorbed destruction: monster survived hit 1! */
    assert((D_801A7AD8[5].flags & DUEL_CARD_FLAG_OCCUPIED) != 0);
    assert(gMonsterEffects.attacked_turn[5] == 1);

    /* After first attack: monster has survived 1 hit, so 0 shields left */
    b = board(&opt);
    assert(b.enemy_shields[0] == 0);
    b.own[dec1.selection[9]-1].flags |= HM_USED;
    HmDecision dec2 = Hm_PlanField(&b, &opt, &rules, NULL);
    assert(dec2.result == 2);
    assert(dec2.selection[10] == 56);              /* Second attack finishes slot 0! */

    /* 3. Execute second attack */
    execute_combat(21, 5, is_3d);
    /* Shield broken: monster destroyed on hit 2! */
    assert((D_801A7AD8[5].flags & DUEL_CARD_FLAG_OCCUPIED) == 0);

    printf("[%s] Scenario 2: CPU Multi-Attack Focus vs 2-Hit Shield -> PASSED\n", view);
}

/* ========================================================================= */
/* SCENARIO 3: Player Piercing vs CPU Immune DEF & Non-Immune DEF            */
/* ========================================================================= */
static void run_scenario_3(int is_3d)
{
    const char *view = is_3d ? "3D" : "2D";

    /* 3a: Player Piercing vs CPU Immune DEF */
    {
        clear_all();
        /* Player: Piercing monster (3500 ATK) */
        register_effect(713, MONSTER_DO_PIERCING, MONSTER_PIERCING_MOD_ALWAYS, MONSTER_RESET_TURN);
        place_monster(0, 0, 713, 3500, 2000, 0);

        /* CPU: Battle-immune monster in DEF (200 DEF) */
        register_effect(18, MONSTER_DO_BATTLE_IMMUNITY, MONSTER_BATTLE_MOD_ALWAYS, MONSTER_RESET_TURN);
        place_monster(1, 0, 18, 300, 200, 1);

        int cpu_lp_before = D_800E9FF0[1].life_points.signed_value;
        execute_combat(5, 20, is_3d);
        int cpu_lp_after = D_800E9FF0[1].life_points.signed_value;

        /* Piercing dealt 3300 damage to CPU */
        assert(cpu_lp_before - cpu_lp_after == 3300);
        /* CPU monster survived */
        assert((D_801A7AD8[20].flags & DUEL_CARD_FLAG_OCCUPIED) != 0);
    }

    /* 3b: Player Piercing vs CPU Non-Immune DEF */
    {
        clear_all();
        /* Player: Piercing monster (3500 ATK) */
        register_effect(713, MONSTER_DO_PIERCING, MONSTER_PIERCING_MOD_ALWAYS, MONSTER_RESET_TURN);
        place_monster(0, 0, 713, 3500, 2000, 0);

        /* CPU: Normal monster in DEF (200 DEF) */
        place_monster(1, 0, 100, 300, 200, 1);

        int cpu_lp_before = D_800E9FF0[1].life_points.signed_value;
        execute_combat(5, 20, is_3d);
        int cpu_lp_after = D_800E9FF0[1].life_points.signed_value;

        /* Piercing dealt 3300 damage to CPU */
        assert(cpu_lp_before - cpu_lp_after == 3300);
        /* CPU monster was destroyed */
        assert((D_801A7AD8[20].flags & DUEL_CARD_FLAG_OCCUPIED) == 0);
    }

    printf("[%s] Scenario 3: Player Piercing vs CPU Immune DEF & Non-Immune DEF -> PASSED\n", view);
}

/* ========================================================================= */
/* SCENARIO 4: CPU Attacks Player Battle-Immune ATK                          */
/* ========================================================================= */
static void run_scenario_4(int is_3d)
{
    clear_all();
    const char *view = is_3d ? "3D" : "2D";

    /* Player: Kuriboh in ATK position (300 ATK), unconditional battle immunity */
    register_effect(18, MONSTER_DO_BATTLE_IMMUNITY, MONSTER_BATTLE_MOD_ALWAYS, MONSTER_RESET_TURN);
    place_monster(0, 0, 18, 300, 200, 0);

    /* CPU: Blue-Eyes (3000 ATK) in ATK position */
    place_monster(1, 0, 1, 3000, 2500, 0);

    /* AI Hard Mode attacks Kuriboh to deal 2700 battle damage to Player's LP */
    D_8009B1D5 = 1; /* CPU turn */
    HmOptions opt = options();
    opt.attacks = 1;
    HmBoard b = board(&opt);
    HmDecision dec = Hm_PlanField(&b, &opt, &rules, NULL);
    assert(dec.result == 2);
    assert(dec.selection[9] == 1);
    assert(dec.selection[10] == 56);

    int player_lp_before = D_800E9FF0[0].life_points.signed_value;
    execute_combat(20, 5, is_3d);
    int player_lp_after = D_800E9FF0[0].life_points.signed_value;

    /* Player took 2700 battle damage */
    assert(player_lp_before - player_lp_after == 2700);
    /* Kuriboh survived battle */
    assert((D_801A7AD8[5].flags & DUEL_CARD_FLAG_OCCUPIED) != 0);

    printf("[%s] Scenario 4: CPU Attacks Player Battle-Immune ATK -> PASSED\n", view);
}

/* ========================================================================= */
/* SCENARIO 5: Player Attacks CPU Battle-Immune ATK/DEF                      */
/* ========================================================================= */
static void run_scenario_5(int is_3d)
{
    clear_all();
    const char *view = is_3d ? "3D" : "2D";

    /* Player: Blue-Eyes (3000 ATK) */
    place_monster(0, 0, 1, 3000, 2500, 0);

    /* CPU: Kuriboh in ATK position (300 ATK), battle immune */
    register_effect(18, MONSTER_DO_BATTLE_IMMUNITY, MONSTER_BATTLE_MOD_ALWAYS, MONSTER_RESET_TURN);
    place_monster(1, 0, 18, 300, 200, 0);

    int cpu_lp_before = D_800E9FF0[1].life_points.signed_value;
    execute_combat(5, 20, is_3d);
    int cpu_lp_after = D_800E9FF0[1].life_points.signed_value;

    /* CPU took 2700 battle damage */
    assert(cpu_lp_before - cpu_lp_after == 2700);
    /* CPU Kuriboh survived battle */
    assert((D_801A7AD8[20].flags & DUEL_CARD_FLAG_OCCUPIED) != 0);

    printf("[%s] Scenario 5: Player Attacks CPU Battle-Immune ATK/DEF -> PASSED\n", view);
}

/* ========================================================================= */
/* SCENARIO 6: CPU Persistent Shield Chipping Across Turns (Copy / Duel)      */
/* ========================================================================= */
static void run_scenario_6(int is_3d)
{
    clear_all();
    const char *view = is_3d ? "3D" : "2D";

    /* Player: 1-hit shield with MONSTER_RESET_SUMMON (per copy) in DEF position (1000 ATK, 1500 DEF) */
    register_effect(18, MONSTER_DO_BATTLE_IMMUNITY, MONSTER_BATTLE_MOD_FIRST_ATTACKED, MONSTER_RESET_SUMMON);
    place_monster(0, 0, 18, 1000, 1500, 1);

    /* Turn 1: CPU controls only ONE attacker (2500 ATK, zone 0 / record 20) */
    place_monster(1, 0, 1, 2500, 2000, 0);

    /* Turn 1: AI Hard Mode plans attack sequence */
    HmOptions o = options();
    D_8009B1D5 = 1;
    HmBoard b = board(&o);
    HmDecision dec = Hm_PlanField(&b, &o, &rules, NULL);

    /* CPU attacks to chip the persistent shield */
    assert(dec.selection[9] == 1);
    assert(dec.selection[10] == 56);

    /* Execute Attack 1 */
    execute_combat(20, 5, is_3d);

    /* Player's monster absorbed hit 1 and survived */
    assert((D_801A7AD8[5].flags & DUEL_CARD_FLAG_OCCUPIED) != 0);
    assert(gMonsterEffects.attacked_summon[5] == 1);

    /* Turn 2: Turn ends, attacked_turn resets to 0, but attacked_summon remains 1! */
    gMonsterEffects.attacked_turn[5] = 0;
    assert(MonsterEffects_BattleImmunity(-1, 5) == 0); /* 0 shields remaining: next hit destroys! */

    /* Turn 2: CPU attacks with 2500 ATK monster */
    b = board(&o);
    assert(b.enemy_shields[0] == 0);
    assert(b.enemy_shield_reset[0] == HM_RESET_SUMMON);
    dec = Hm_PlanField(&b, &o, &rules, NULL);
    assert(dec.selection[9] == 1);
    assert(dec.selection[10] == 56);

    /* Execute Attack 2 */
    execute_combat(20, 5, is_3d);

    /* Player's monster destroyed on 2nd hit! */
    assert((D_801A7AD8[5].flags & DUEL_CARD_FLAG_OCCUPIED) == 0);

    printf("[%s] Scenario 6: CPU Persistent Shield Chipping Across Turns -> PASSED\n", view);
}

/* ========================================================================= */
/* SCENARIO 7: In a Duel Reset Shared Across Card Copies in Deck/Field      */
/* ========================================================================= */
static void run_scenario_7(int is_3d)
{
    clear_all();
    const char *view = is_3d ? "3D" : "2D";

    /* Player: Card 18 with 2-hit shield in a duel (MONSTER_RESET_DUEL) in DEF position (1000 ATK, 1500 DEF) */
    register_effect(18, MONSTER_DO_BATTLE_IMMUNITY, MONSTER_BATTLE_MOD_AFTER_2_ATTACKS, MONSTER_RESET_DUEL);
    place_monster(0, 0, 18, 1000, 1500, 1);

    /* CPU: Blue-Eyes (3000 ATK, zone 0 / record 20) in ATK position */
    place_monster(1, 0, 1, 3000, 2500, 0);

    /* Hit 1 on Copy 1: survives (1/2 attacks) */
    execute_combat(20, 5, is_3d);
    assert((D_801A7AD8[5].flags & DUEL_CARD_FLAG_OCCUPIED) != 0);
    assert(gMonsterEffects.attacked_duel[5] == 1);

    /* Hit 2 on Copy 1: survives (2/2 attacks) */
    execute_combat(20, 5, is_3d);
    assert((D_801A7AD8[5].flags & DUEL_CARD_FLAG_OCCUPIED) != 0);
    assert(gMonsterEffects.attacked_duel[5] == 2);

    /* Hit 3 on Copy 1: shield exhausted, Copy 1 is destroyed */
    execute_combat(20, 5, is_3d);
    assert((D_801A7AD8[5].flags & DUEL_CARD_FLAG_OCCUPIED) == 0);

    /* Now Player summons Copy 2 of Card 18 from deck into zone 5 */
    place_monster(0, 0, 18, 1000, 1500, 1);
    assert(gMonsterEffects.attacked_duel[5] == 3);
    assert(MonsterEffects_BattleImmunity(-1, 5) == 0); /* 0 shields left in this duel */

    /* CPU attacks Copy 2: destroyed on the very first attack because Card 18
     * already exhausted its 2-attack duel shield on Copy 1! */
    execute_combat(20, 5, is_3d);
    assert((D_801A7AD8[5].flags & DUEL_CARD_FLAG_OCCUPIED) == 0);

    printf("[%s] Scenario 7: In a Duel Reset Shared Across Card Copies -> PASSED\n", view);
}

int main(void)
{
    MemoriesMod mod = {0};
    s_test_host.api = 4;
    s_test_host.setting = host_setting;
    s_test_host.hook = host_hook;
    assert(MemoriesModInit(&s_test_host, &mod));

    puts("--- Running 2D Battle Mode Scenarios ---");
    run_scenario_1(0);
    run_scenario_2(0);
    run_scenario_3(0);
    run_scenario_4(0);
    run_scenario_5(0);
    run_scenario_6(0);
    run_scenario_7(0);

    puts("\n--- Running 3D Battle Mode Scenarios ---");
    run_scenario_1(1);
    run_scenario_2(1);
    run_scenario_3(1);
    run_scenario_4(1);
    run_scenario_5(1);
    run_scenario_6(1);
    run_scenario_7(1);

    puts("\nAll 14 combat immunity and piercing runtime scenarios PASSED successfully!");
    return 0;
}
