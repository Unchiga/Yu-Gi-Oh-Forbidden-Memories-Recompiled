/* Unit test for API 19 attacked conditions:
 * - first_attacked, second_attacked, third_attacked
 * - Verifies condition is false when the monster attacks another (attacker)
 * - Verifies condition is true only when the monster is attacked (defender)
 * - Verifies first, second, and third attack counts
 * - Verifies condition is false outside of battle
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "game/duel_card.h"
#include "game/duel_side_state.h"
#include "pc/cards/monster_effects.h"
#include "pc/cards/monster_effects_duel.h"
#include "types.h"

/* Mock game globals required by monster_effects_duel.c */
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
s16 D_8009B22A = 0;
u16 D_8009B178[2] = {0, 0};
u8 D_800E9F64[256];
void *D_800E9EF0[4] = {0};
u8 D_800EAE88[16] = {0};
DuelCardRecord D_801A7AD8[30];
s32 gDuel_adwCardStats[1024];
u8 gDuel_bEffectState = 0;
u16 gDuel_wCardEffectFlags = 0;
u8 gDuel_bQuitDialogState = 0;

MonsterEffectsState gMonsterEffects = {0};
const char *const MonsterEffect_DoNames[MONSTER_DO_COUNT] = {
    "magic", "boost", "heal", "damage", "destroy", "battle_immunity"};
const char *const MonsterEffect_WhenNames[MONSTER_WHEN_COUNT] = {
    "summon",           "flip",   "draw", "combat", "destroyed",
    "destroy_opponent", "face_up"};

void SD_SEPlayFull(u32 sound) { (void)sound; }
void DuelEffect_StartRetailCardEffect(int card, int param) {
  (void)card;
  (void)param;
}
void DuelCard_RemoveFromField(const DuelCardRecord *card) { (void)card; }
int Cards_Valid(int card) { return card > 0 && card <= 722; }
int Cards_CardEffects(int card, const MonsterEffect **effects) {
  (void)card;
  (void)effects;
  return 0;
}
int announce(int card, int record, int when, int phase) {
  (void)card;
  (void)record;
  (void)when;
  (void)phase;
  return 0;
}
int s32_min(int a, int b) { return a < b ? a : b; }
int Log_Wanted(int cat) {
  (void)cat;
  return 0;
}
void Log_Printf(int cat, const char *fmt, ...) {
  (void)cat;
  (void)fmt;
}
void Mods_DamageLife(int side, int damage) {
  (void)side;
  (void)damage;
}
int Mods_Dispatch(int hook, void *data) {
  (void)hook;
  (void)data;
  return 0;
}
int Cards_Type(int card) {
  (void)card;
  return 0;
}
int Cards_Attribute(int card) {
  (void)card;
  return 0;
}
s32 Duel_CalcCardStats(DuelCardRecord *card) {
  (void)card;
  return 0;
}
void DuelEffect_CreateRequest(int a, int b, int c, int d, int e, int f) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  (void)e;
  (void)f;
}
int Tables_StatCapEither(int val, int diff) { return val + diff; }

static MonsterEffect s_test_effects[8];
static int s_test_effect_count = 0;

int Cards_MonsterEffects(int card, const MonsterEffect **effects) {
  (void)card;
  if (effects)
    *effects = s_test_effects;
  return s_test_effect_count;
}

#define PHASE_HAND 4
#define PHASE_FIELD 5
#define PHASE_BATTLE 9

static void reset_test_duel(void) {
  memset(&gMonsterEffects, 0, sizeof(gMonsterEffects));
  memset(D_801A7AD8, 0, sizeof(D_801A7AD8));
  gDuel_wSceneStateFlags = PHASE_FIELD;
  D_8009B1D5 = 0;
  D_8009B1B0[0] = 0;
  D_8009B1B0[1] = 0;
  D_8009B208 = -1;
  D_8009B209 = -1;
  gMonsterEffects.ready = 1;
  gMonsterEffects.trap_battle_record[0] = -1;
  gMonsterEffects.trap_battle_record[1] = -1;

  /* Setup player monster in zone 5 and opponent monster in zone 20 */
  D_801A7AD8[5].card_id = 18; /* Kuriboh */
  D_801A7AD8[5].flags = DUEL_CARD_FLAG_OCCUPIED;
  gMonsterEffects.card[5] = 18;
  gMonsterEffects.face_up[5] = 1;

  D_801A7AD8[20].card_id = 1; /* Blue-Eyes */
  D_801A7AD8[20].flags = DUEL_CARD_FLAG_OCCUPIED;
  gMonsterEffects.card[20] = 1;
  gMonsterEffects.face_up[20] = 1;
}

static void test_attacker_vs_defender(void) {
  MonsterEffect effect;
  memset(&effect, 0, sizeof(effect));
  effect.when = MONSTER_WHEN_FACE_UP;
  effect.action = MONSTER_DO_BATTLE_IMMUNITY;
  effect.target = MONSTER_TARGET_SELF;
  effect.cond_count = 1;
  effect.conditions[0].type = MONSTER_COND_FIRST_ATTACKED;
  effect.conditions[0].reset = MONSTER_RESET_TURN;

  s_test_effects[0] = effect;
  s_test_effect_count = 1;

  reset_test_duel();

  /* --- Case 1: Outside of battle (PHASE_FIELD) --- */
  gDuel_wSceneStateFlags = PHASE_FIELD;
  assert(!MonsterEffects_ConditionsMet(5, &effect));
  assert(!MonsterEffects_ConditionsMet(20, &effect));

  /* --- Case 2: Kuriboh (zone 5) ATTACKS Blue-Eyes (zone 20) --- */
  /* Kuriboh is attacker (trap_battle_record[0]), Blue-Eyes is defender
   * (trap_battle_record[1]) */
  gDuel_wSceneStateFlags = PHASE_BATTLE;
  gMonsterEffects.trap_battle_record[0] = 5;
  gMonsterEffects.trap_battle_record[1] = 20;

  /* Simulate battle increment for defender 20 */
  gMonsterEffects.attacked_turn[20] = 1;
  gMonsterEffects.attacked_turn[5] = 0; /* Kuriboh was not attacked */

  /* The attacker (5) is attacking, NOT attacked -> condition MUST be FALSE! */
  assert(MonsterEffects_ConditionsMet(5, &effect) == 0);
  assert(MonsterEffects_HasBattleImmunity(5) == 0);

  /* The defender (20) was attacked for the first time -> condition is TRUE for
   * 20 */
  assert(MonsterEffects_ConditionsMet(20, &effect) == 1);
  assert(MonsterEffects_HasBattleImmunity(20) == 1);

  puts("test_attacker_vs_defender: PASSED (attacker does not get effect, "
       "defender does)");
}

static void test_first_second_third_attacked_sequence(void) {
  MonsterEffect eff_first, eff_second, eff_third;

  memset(&eff_first, 0, sizeof(eff_first));
  eff_first.cond_count = 1;
  eff_first.conditions[0].type = MONSTER_COND_FIRST_ATTACKED;
  eff_first.conditions[0].reset = MONSTER_RESET_TURN;

  memset(&eff_second, 0, sizeof(eff_second));
  eff_second.cond_count = 1;
  eff_second.conditions[0].type = MONSTER_COND_DESTROYED_AFTER_2_ATTACKS;
  eff_second.conditions[0].reset = MONSTER_RESET_TURN;

  memset(&eff_third, 0, sizeof(eff_third));
  eff_third.cond_count = 1;
  eff_third.conditions[0].type = MONSTER_COND_DESTROYED_AFTER_3_ATTACKS;
  eff_third.conditions[0].reset = MONSTER_RESET_TURN;

  reset_test_duel();
  gDuel_wSceneStateFlags = PHASE_BATTLE;
  gMonsterEffects.trap_battle_record[0] = 20;
  gMonsterEffects.trap_battle_record[1] = 5;

  /* --- Attack 1 --- */
  gMonsterEffects.attacked_turn[5] = 1;
  assert(MonsterEffects_ConditionsMet(5, &eff_first) == 1);
  assert(MonsterEffects_ConditionsMet(5, &eff_second) == 1);
  assert(MonsterEffects_ConditionsMet(5, &eff_third) == 1);

  /* --- Attack 2 --- */
  gMonsterEffects.attacked_turn[5] = 2;
  assert(MonsterEffects_ConditionsMet(5, &eff_first) ==
         0); /* Destroyed after 1 attack */
  assert(MonsterEffects_ConditionsMet(5, &eff_second) ==
         1); /* Still protected: can only be destroyed after two attacks */
  assert(MonsterEffects_ConditionsMet(5, &eff_third) == 1);

  /* --- Attack 4 --- */
  gMonsterEffects.attacked_turn[5] = 4;
  assert(MonsterEffects_ConditionsMet(5, &eff_first) == 0);
  assert(MonsterEffects_ConditionsMet(5, &eff_second) == 0);
  assert(MonsterEffects_ConditionsMet(5, &eff_third) ==
         0); /* Destroyed after three attacks */

  puts("test_first_second_third_attacked_sequence: PASSED (1st, 2-attack, and "
       "3-attack survival match accurately)");
}

static void test_fewer_and_more_monsters_condition(void) {
  MonsterEffect eff_fewer, eff_more;

  memset(&eff_fewer, 0, sizeof(eff_fewer));
  eff_fewer.cond_count = 1;
  eff_fewer.conditions[0].type = MONSTER_COND_FEWER_MONSTERS;

  memset(&eff_more, 0, sizeof(eff_more));
  eff_more.cond_count = 1;
  eff_more.conditions[0].type = MONSTER_COND_MORE_MONSTERS;

  reset_test_duel();

  /* Case 1: Player controls 1 monster (slot 5), Opponent controls 2 monsters (slots 20, 21) */
  D_801A7AD8[21].card_id = 2;
  D_801A7AD8[21].flags = DUEL_CARD_FLAG_OCCUPIED;
  gMonsterEffects.card[21] = 2;
  gMonsterEffects.face_up[21] = 1;

  /* Evaluated from player's monster (slot 5): own=1, opp=2 */
  assert(MonsterEffects_ConditionsMet(5, &eff_fewer) == 1);
  assert(MonsterEffects_ConditionsMet(5, &eff_more) == 0);

  /* Evaluated from opponent's monster (slot 20): own=2, opp=1 */
  assert(MonsterEffects_ConditionsMet(20, &eff_fewer) == 0);
  assert(MonsterEffects_ConditionsMet(20, &eff_more) == 1);

  /* Case 2: Equal monsters: Player controls 2 (slots 5, 6), Opponent controls 2 (slots 20, 21) */
  D_801A7AD8[6].card_id = 3;
  D_801A7AD8[6].flags = DUEL_CARD_FLAG_OCCUPIED;
  gMonsterEffects.card[6] = 3;
  gMonsterEffects.face_up[6] = 1;

  assert(MonsterEffects_ConditionsMet(5, &eff_fewer) == 0);
  assert(MonsterEffects_ConditionsMet(5, &eff_more) == 0);
  assert(MonsterEffects_ConditionsMet(20, &eff_fewer) == 0);
  assert(MonsterEffects_ConditionsMet(20, &eff_more) == 0);

  /* Case 3: Player controls 3 monsters (slots 5, 6, 7), Opponent controls 1 (slot 20) */
  D_801A7AD8[7].card_id = 4;
  D_801A7AD8[7].flags = DUEL_CARD_FLAG_OCCUPIED;
  gMonsterEffects.card[7] = 4;
  gMonsterEffects.face_up[7] = 1;

  /* Remove opponent's second monster */
  D_801A7AD8[21].card_id = 0;
  D_801A7AD8[21].flags = 0;
  gMonsterEffects.card[21] = 0;
  gMonsterEffects.face_up[21] = 0;

  /* Player own=3, opp=1 */
  assert(MonsterEffects_ConditionsMet(5, &eff_fewer) == 0);
  assert(MonsterEffects_ConditionsMet(5, &eff_more) == 1);

  /* Opponent own=1, opp=3 */
  assert(MonsterEffects_ConditionsMet(20, &eff_fewer) == 1);
  assert(MonsterEffects_ConditionsMet(20, &eff_more) == 0);

  /* Case 4: Opponent controls 0 monsters, Player controls 1 monster (slot 5) */
  reset_test_duel();
  /* In reset_test_duel(), slot 5 and slot 20 were set up. Clear slot 20 so opponent has 0 monsters. */
  D_801A7AD8[20].card_id = 0;
  D_801A7AD8[20].flags = 0;
  gMonsterEffects.card[20] = 0;
  gMonsterEffects.face_up[20] = 0;

  /* Evaluated from player (slot 5): own=1, opp=0 -> more is true, fewer is false */
  assert(MonsterEffects_ConditionsMet(5, &eff_more) == 1);
  assert(MonsterEffects_ConditionsMet(5, &eff_fewer) == 0);

  /* Case 5: Player controls 0 monsters, Opponent controls 1 monster (slot 20) */
  D_801A7AD8[5].card_id = 0;
  D_801A7AD8[5].flags = 0;
  gMonsterEffects.card[5] = 0;
  gMonsterEffects.face_up[5] = 0;

  D_801A7AD8[20].card_id = 1;
  D_801A7AD8[20].flags = DUEL_CARD_FLAG_OCCUPIED;
  gMonsterEffects.card[20] = 1;
  gMonsterEffects.face_up[20] = 1;

  /* Evaluated from player perspective (slot 5): own=0, opp=1 -> fewer is true, more is false */
  assert(MonsterEffects_ConditionsMet(5, &eff_fewer) == 1);
  assert(MonsterEffects_ConditionsMet(5, &eff_more) == 0);

  /* Evaluated from opponent perspective (slot 20): own=1, opp=0 -> more is true, fewer is false */
  assert(MonsterEffects_ConditionsMet(20, &eff_more) == 1);
  assert(MonsterEffects_ConditionsMet(20, &eff_fewer) == 0);

  /* Case 6: Tie at 0 monsters (both player and opponent control 0 monsters) */
  D_801A7AD8[20].card_id = 0;
  D_801A7AD8[20].flags = 0;
  gMonsterEffects.card[20] = 0;
  gMonsterEffects.face_up[20] = 0;

  /* Evaluated from player perspective (slot 5): own=0, opp=0 -> tie, both false */
  assert(MonsterEffects_ConditionsMet(5, &eff_fewer) == 0);
  assert(MonsterEffects_ConditionsMet(5, &eff_more) == 0);

  /* Evaluated from opponent perspective (slot 20): own=0, opp=0 -> tie, both false */
  assert(MonsterEffects_ConditionsMet(20, &eff_fewer) == 0);
  assert(MonsterEffects_ConditionsMet(20, &eff_more) == 0);

  /* Case 7: Tie at 1 monster (both player and opponent control 1 monster) */
  D_801A7AD8[5].card_id = 18;
  D_801A7AD8[5].flags = DUEL_CARD_FLAG_OCCUPIED;
  gMonsterEffects.card[5] = 18;
  gMonsterEffects.face_up[5] = 1;

  D_801A7AD8[20].card_id = 1;
  D_801A7AD8[20].flags = DUEL_CARD_FLAG_OCCUPIED;
  gMonsterEffects.card[20] = 1;
  gMonsterEffects.face_up[20] = 1;

  /* Evaluated from player perspective (slot 5): own=1, opp=1 -> tie, both false */
  assert(MonsterEffects_ConditionsMet(5, &eff_fewer) == 0);
  assert(MonsterEffects_ConditionsMet(5, &eff_more) == 0);

  /* Evaluated from opponent perspective (slot 20): own=1, opp=1 -> tie, both false */
  assert(MonsterEffects_ConditionsMet(20, &eff_fewer) == 0);
  assert(MonsterEffects_ConditionsMet(20, &eff_more) == 0);

  puts("test_fewer_and_more_monsters_condition: PASSED (fewer/more monsters with 0 monsters and ties evaluated correctly)");
}

int main(void) {
  test_attacker_vs_defender();
  test_first_second_third_attacked_sequence();
  test_fewer_and_more_monsters_condition();
  puts("\nAll monster attacked condition tests PASSED successfully!");
  return 0;
}
