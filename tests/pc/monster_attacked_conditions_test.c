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
u8 gDuel_bTerrain = 0;

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
static int s_mock_type[1024] = {0};
static int s_mock_attr[1024] = {0};
static int s_mock_level[1024] = {0};

int Cards_Type(int card) {
  if (card > 0 && card < 1024) return s_mock_type[card];
  return 0;
}
int Cards_Attribute(int card) {
  if (card > 0 && card < 1024) return s_mock_attr[card];
  return 0;
}
int Cards_Level(int card) {
  if (card > 0 && card < 1024) return s_mock_level[card];
  return 0;
}
s32 Duel_CalcCardStats(DuelCardRecord *card) {
  int slot = card - &D_801A7AD8[0];
  if (slot >= 0 && slot < 30) {
    return gDuel_adwCardStats[slot];
  }
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
  effect.battle_modifier = MONSTER_BATTLE_MOD_FIRST_ATTACKED;
  effect.battle_reset = MONSTER_RESET_TURN;

  s_test_effects[0] = effect;
  s_test_effect_count = 1;

  reset_test_duel();

  /* --- Case 1: Outside of battle (PHASE_FIELD) --- */
  gDuel_wSceneStateFlags = PHASE_FIELD;
  assert(!MonsterEffects_HasBattleImmunity(5));
  assert(!MonsterEffects_HasBattleImmunity(20));

  /* --- Case 2: Kuriboh (zone 5) ATTACKS Blue-Eyes (zone 20) --- */
  /* Kuriboh is attacker (trap_battle_record[0]), Blue-Eyes is defender
   * (trap_battle_record[1]) */
  gDuel_wSceneStateFlags = PHASE_BATTLE;
  gMonsterEffects.trap_battle_record[0] = 5;
  gMonsterEffects.trap_battle_record[1] = 20;

  /* Simulate battle increment for defender 20 */
  gMonsterEffects.attacked_turn[20] = 1;
  gMonsterEffects.attacked_turn[5] = 0; /* Kuriboh was not attacked */

  /* The attacker (5) is attacking, NOT attacked -> battle immunity MUST be FALSE! */
  assert(MonsterEffects_HasBattleImmunity(5) == 0);

  /* The defender (20) was attacked for the first time -> battle immunity is TRUE for 20 */
  assert(MonsterEffects_HasBattleImmunity(20) == 1);

  puts("test_attacker_vs_defender: PASSED (attacker does not get battle immunity, "
       "defender does)");
}

static void test_first_second_third_attacked_sequence(void) {
  MonsterEffect eff_first, eff_second, eff_third, eff_boost;

  memset(&eff_first, 0, sizeof(eff_first));
  eff_first.when = MONSTER_WHEN_FACE_UP;
  eff_first.action = MONSTER_DO_BATTLE_IMMUNITY;
  eff_first.target = MONSTER_TARGET_SELF;
  eff_first.battle_modifier = MONSTER_BATTLE_MOD_FIRST_ATTACKED;
  eff_first.battle_reset = MONSTER_RESET_TURN;

  memset(&eff_second, 0, sizeof(eff_second));
  eff_second.when = MONSTER_WHEN_FACE_UP;
  eff_second.action = MONSTER_DO_BATTLE_IMMUNITY;
  eff_second.target = MONSTER_TARGET_SELF;
  eff_second.battle_modifier = MONSTER_BATTLE_MOD_AFTER_2_ATTACKS;
  eff_second.battle_reset = MONSTER_RESET_TURN;

  memset(&eff_third, 0, sizeof(eff_third));
  eff_third.when = MONSTER_WHEN_FACE_UP;
  eff_third.action = MONSTER_DO_BATTLE_IMMUNITY;
  eff_third.target = MONSTER_TARGET_SELF;
  eff_third.battle_modifier = MONSTER_BATTLE_MOD_AFTER_3_ATTACKS;
  eff_third.battle_reset = MONSTER_RESET_TURN;

  /* Verify boosts are never affected by battle modifiers */
  memset(&eff_boost, 0, sizeof(eff_boost));
  eff_boost.when = MONSTER_WHEN_FACE_UP;
  eff_boost.action = MONSTER_DO_BOOST;
  eff_boost.target = MONSTER_TARGET_SELF;
  eff_boost.attack = 500;
  eff_boost.battle_modifier = MONSTER_BATTLE_MOD_FIRST_ATTACKED;

  reset_test_duel();
  gDuel_wSceneStateFlags = PHASE_BATTLE;
  gMonsterEffects.trap_battle_record[0] = 20;
  gMonsterEffects.trap_battle_record[1] = 5;

  /* --- Attack 1 --- */
  gMonsterEffects.attacked_turn[5] = 1;
  s_test_effects[0] = eff_first;
  assert(MonsterEffects_HasBattleImmunity(5) == 1);
  s_test_effects[0] = eff_second;
  assert(MonsterEffects_HasBattleImmunity(5) == 1);
  s_test_effects[0] = eff_third;
  assert(MonsterEffects_HasBattleImmunity(5) == 1);

  /* Boost is not affected by battle modifier, conditions met is true */
  assert(MonsterEffects_ConditionsMet(5, &eff_boost) == 1);

  /* --- Attack 2 --- */
  gMonsterEffects.attacked_turn[5] = 2;
  s_test_effects[0] = eff_first;
  assert(MonsterEffects_HasBattleImmunity(5) == 0); /* Immune only 1st attack */
  s_test_effects[0] = eff_second;
  assert(MonsterEffects_HasBattleImmunity(5) == 1); /* Still protected */
  s_test_effects[0] = eff_third;
  assert(MonsterEffects_HasBattleImmunity(5) == 1);

  /* --- Attack 4 --- */
  gMonsterEffects.attacked_turn[5] = 4;
  s_test_effects[0] = eff_first;
  assert(MonsterEffects_HasBattleImmunity(5) == 0);
  s_test_effects[0] = eff_second;
  assert(MonsterEffects_HasBattleImmunity(5) == 0);
  s_test_effects[0] = eff_third;
  assert(MonsterEffects_HasBattleImmunity(5) == 0); /* Destroyed after 3 attacks */

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

static void test_control_stat_condition(void) {
  MonsterEffect eff;
  MonsterCondition *cond;

  memset(&eff, 0, sizeof(eff));
  eff.cond_count = 1;
  eff.when = MONSTER_WHEN_FACE_UP;
  cond = &eff.conditions[0];

  /* Setup player monsters:
   * Slot 5: card 10, ATK 1500, DEF 1200
   * Slot 6: card 20, ATK 2500, DEF 2100
   */
  memset(D_801A7AD8, 0, sizeof(D_801A7AD8));
  memset(&gMonsterEffects, 0, sizeof(gMonsterEffects));

  D_801A7AD8[5].card_id = 10;
  D_801A7AD8[5].flags = DUEL_CARD_FLAG_OCCUPIED;
  gMonsterEffects.card[5] = 10;
  gMonsterEffects.face_up[5] = 1;
  gDuel_adwCardStats[5] = (1200 << 16) | 1500;

  D_801A7AD8[6].card_id = 20;
  D_801A7AD8[6].flags = DUEL_CARD_FLAG_OCCUPIED;
  gMonsterEffects.card[6] = 20;
  gMonsterEffects.face_up[6] = 1;
  gDuel_adwCardStats[6] = (2100 << 16) | 2500;

  /* Test 1: control ATK > 2000 (presence=1, stat=ATK, op=GT, val=2000, scope=0) */
  cond->type = MONSTER_COND_CONTROL_STAT;
  cond->presence = 1;
  cond->stat = MONSTER_STAT_ATK;
  cond->op = MONSTER_OP_GT;
  cond->value = 2000;
  cond->scope = 0;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 1);
  assert(MonsterEffects_ConditionsMet(6, &eff) == 1);

  /* Test 2: control other monster with ATK > 2000 (scope=1) */
  cond->scope = 1;
  /* Evaluated from slot 5: other monster (slot 6) has 2500 > 2000 -> true */
  assert(MonsterEffects_ConditionsMet(5, &eff) == 1);
  /* Evaluated from slot 6: other monster (slot 5) has 1500, not > 2000 -> false */
  assert(MonsterEffects_ConditionsMet(6, &eff) == 0);

  /* Test 3: not control monster with ATK > 3000 (presence=0, op=GT, val=3000, scope=0) -> true */
  cond->presence = 0;
  cond->scope = 0;
  cond->value = 3000;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 1);

  /* Test 4: not control monster with ATK > 2000 (presence=0, op=GT, val=2000, scope=0) -> false (slot 6 has 2500) */
  cond->value = 2000;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 0);

  /* Test 5: DEF == 1200 (presence=1, stat=DEF, op=EQ, val=1200) -> true */
  cond->presence = 1;
  cond->stat = MONSTER_STAT_DEF;
  cond->op = MONSTER_OP_EQ;
  cond->value = 1200;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 1);

  /* Test 6: Face-down slot 6 does not count */
  cond->stat = MONSTER_STAT_ATK;
  cond->op = MONSTER_OP_GT;
  cond->value = 2000;
  cond->scope = 0;
  D_801A7AD8[6].flags |= DUEL_CARD_FLAG_FACE_DOWN;
  /* Now only slot 5 is face-up (1500 ATK), so ATK > 2000 is false */
  assert(MonsterEffects_ConditionsMet(5, &eff) == 0);

  puts("test_control_stat_condition: PASSED");
}

static void test_control_level_condition(void) {
  MonsterEffect eff;
  MonsterCondition *cond;

  memset(&eff, 0, sizeof(eff));
  eff.cond_count = 1;
  eff.when = MONSTER_WHEN_FACE_UP;
  cond = &eff.conditions[0];

  memset(D_801A7AD8, 0, sizeof(D_801A7AD8));
  memset(&gMonsterEffects, 0, sizeof(gMonsterEffects));

  /* Slot 5: card 10, Level 4 */
  D_801A7AD8[5].card_id = 10;
  D_801A7AD8[5].flags = DUEL_CARD_FLAG_OCCUPIED;
  gMonsterEffects.card[5] = 10;
  gMonsterEffects.face_up[5] = 1;
  s_mock_level[10] = 4;

  /* Slot 6: card 20, Level 7 */
  D_801A7AD8[6].card_id = 20;
  D_801A7AD8[6].flags = DUEL_CARD_FLAG_OCCUPIED;
  gMonsterEffects.card[6] = 20;
  gMonsterEffects.face_up[6] = 1;
  s_mock_level[20] = 7;

  /* Test 1: control Level == 7 (presence=1, op=EQ, val=7, scope=0) -> true */
  cond->type = MONSTER_COND_CONTROL_LEVEL;
  cond->presence = 1;
  cond->op = MONSTER_OP_EQ;
  cond->value = 7;
  cond->scope = 0;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 1);

  /* Test 2: control other monster with Level >= 7 (scope=1) */
  cond->scope = 1;
  cond->op = MONSTER_OP_GE;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 1);
  assert(MonsterEffects_ConditionsMet(6, &eff) == 0);

  /* Test 3: not control monster with Level >= 8 (presence=0, op=GE, val=8) -> true */
  cond->presence = 0;
  cond->scope = 0;
  cond->value = 8;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 1);

  /* Test 4: not control monster with Level < 5 (presence=0, op=LT, val=5) -> false (slot 5 has level 4) */
  cond->op = MONSTER_OP_LT;
  cond->value = 5;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 0);

  puts("test_control_level_condition: PASSED");
}

static void test_control_type_condition(void) {
  MonsterEffect eff;
  MonsterCondition *cond;

  memset(&eff, 0, sizeof(eff));
  eff.cond_count = 1;
  eff.when = MONSTER_WHEN_FACE_UP;
  cond = &eff.conditions[0];

  memset(D_801A7AD8, 0, sizeof(D_801A7AD8));
  memset(&gMonsterEffects, 0, sizeof(gMonsterEffects));

  /* Slot 5: card 10, Type 1 */
  D_801A7AD8[5].card_id = 10;
  D_801A7AD8[5].flags = DUEL_CARD_FLAG_OCCUPIED;
  gMonsterEffects.card[5] = 10;
  gMonsterEffects.face_up[5] = 1;
  s_mock_type[10] = 1;

  /* Slot 6: card 20, Type 0 */
  D_801A7AD8[6].card_id = 20;
  D_801A7AD8[6].flags = DUEL_CARD_FLAG_OCCUPIED;
  gMonsterEffects.card[6] = 20;
  gMonsterEffects.face_up[6] = 1;
  s_mock_type[20] = 0;

  /* Test 1: control at least one Dragon (type=0, mode=at_least_one, presence=1) -> true */
  cond->type = MONSTER_COND_CONTROL_TYPE;
  cond->card_type = 0;
  cond->type_mode = MONSTER_COND_TYPE_AT_LEAST_ONE;
  cond->presence = 1;
  cond->scope = 0;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 1);

  /* Test 2: only control Dragon (type=0, mode=only, presence=1) -> false (slot 5 is Type 1) */
  cond->type_mode = MONSTER_COND_TYPE_ONLY;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 0);

  /* If slot 5 is cleared, only slot 6 (Type 0) is controlled -> only Dragon is true */
  D_801A7AD8[5].flags = 0;
  assert(MonsterEffects_ConditionsMet(6, &eff) == 1);

  /* Test 3: not control Zombie (type=2, presence=0, mode=at_least_one) -> true */
  cond->card_type = 2;
  cond->presence = 0;
  cond->type_mode = MONSTER_COND_TYPE_AT_LEAST_ONE;
  assert(MonsterEffects_ConditionsMet(6, &eff) == 1);

  /* Test 4: scope=1 (another Dragon you control) */
  cond->card_type = 0;
  cond->presence = 1;
  cond->scope = 1;
  assert(MonsterEffects_ConditionsMet(6, &eff) == 0);

  puts("test_control_type_condition: PASSED");
}

static void test_control_attribute_condition(void) {
  MonsterEffect eff;
  MonsterCondition *cond;

  memset(&eff, 0, sizeof(eff));
  eff.cond_count = 1;
  eff.when = MONSTER_WHEN_FACE_UP;
  cond = &eff.conditions[0];

  memset(D_801A7AD8, 0, sizeof(D_801A7AD8));
  memset(&gMonsterEffects, 0, sizeof(gMonsterEffects));

  /* Slot 5: card 10, Attribute 2 (LIGHT) */
  D_801A7AD8[5].card_id = 10;
  D_801A7AD8[5].flags = DUEL_CARD_FLAG_OCCUPIED;
  gMonsterEffects.card[5] = 10;
  gMonsterEffects.face_up[5] = 1;
  s_mock_attr[10] = 2;

  /* Slot 6: card 20, Attribute 1 (DARK) */
  D_801A7AD8[6].card_id = 20;
  D_801A7AD8[6].flags = DUEL_CARD_FLAG_OCCUPIED;
  gMonsterEffects.card[6] = 20;
  gMonsterEffects.face_up[6] = 1;
  s_mock_attr[20] = 1;

  /* Test 1: control LIGHT (attr=2, presence=1, scope=0) -> true */
  cond->type = MONSTER_COND_CONTROL_ATTRIBUTE;
  cond->attribute = 2;
  cond->presence = 1;
  cond->scope = 0;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 1);

  /* Test 2: control other LIGHT monster (scope=1) */
  cond->scope = 1;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 0);
  assert(MonsterEffects_ConditionsMet(6, &eff) == 1);

  /* Test 3: not control WATER (attr=3, presence=0) -> true */
  cond->attribute = 3;
  cond->presence = 0;
  cond->scope = 0;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 1);

  /* Test 4: not control LIGHT (attr=2, presence=0) -> false */
  cond->attribute = 2;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 0);

  puts("test_control_attribute_condition: PASSED");
}

static void test_field_spell_condition(void) {
  MonsterEffect eff;
  MonsterCondition *cond;

  memset(&eff, 0, sizeof(eff));
  eff.cond_count = 1;
  eff.when = MONSTER_WHEN_FACE_UP;
  cond = &eff.conditions[0];
  cond->type = MONSTER_COND_FIELD_SPELL;

  memset(D_801A7AD8, 0, sizeof(D_801A7AD8));
  memset(&gMonsterEffects, 0, sizeof(gMonsterEffects));

  /* Slot 5 monster */
  D_801A7AD8[5].card_id = 10;
  D_801A7AD8[5].flags = DUEL_CARD_FLAG_OCCUPIED;
  gMonsterEffects.card[5] = 10;
  gMonsterEffects.face_up[5] = 1;

  /* Test 1: Any field spell active (field=0, presence=1) */
  cond->field_spell = MONSTER_FIELD_ANY;
  cond->presence = 1;
  gDuel_bTerrain = 0; /* Normal / Arena / No field */
  assert(MonsterEffects_ConditionsMet(5, &eff) == 0);

  gDuel_bTerrain = MONSTER_FIELD_FOREST;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 1);

  gDuel_bTerrain = MONSTER_FIELD_YAMI;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 1);

  /* Test 2: Any field spell NOT active (field=0, presence=0) */
  cond->presence = 0;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 0);

  gDuel_bTerrain = 0;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 1);

  /* Test 3: Specific field spell active: Umi (field=5, presence=1) */
  cond->field_spell = MONSTER_FIELD_UMI;
  cond->presence = 1;
  gDuel_bTerrain = MONSTER_FIELD_UMI;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 1);

  gDuel_bTerrain = MONSTER_FIELD_FOREST;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 0);

  gDuel_bTerrain = 0;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 0);

  /* Test 4: Specific field spell NOT active: Forest (field=1, presence=0) */
  cond->field_spell = MONSTER_FIELD_FOREST;
  cond->presence = 0;
  gDuel_bTerrain = MONSTER_FIELD_FOREST;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 0);

  gDuel_bTerrain = MONSTER_FIELD_WASTELAND;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 1);

  gDuel_bTerrain = 0;
  assert(MonsterEffects_ConditionsMet(5, &eff) == 1);

  /* Test 5: Verify all field spell types 1 to 6 (Forest..Yami) */
  for (int f = MONSTER_FIELD_FOREST; f <= MONSTER_FIELD_YAMI; f++) {
    cond->field_spell = (unsigned char)f;
    cond->presence = 1;
    gDuel_bTerrain = (u8)f;
    assert(MonsterEffects_ConditionsMet(5, &eff) == 1);

    gDuel_bTerrain = (u8)((f == MONSTER_FIELD_YAMI) ? MONSTER_FIELD_FOREST : (f + 1));
    assert(MonsterEffects_ConditionsMet(5, &eff) == 0);
  }

  puts("test_field_spell_condition: PASSED");
}

static void test_attacker_criteria_immunity(void) {
  MonsterEffect eff;
  reset_test_duel();

  /* Zone 5 is defender (card 18), Zone 20 is attacker (card 1) */
  gDuel_wSceneStateFlags = PHASE_BATTLE;
  gMonsterEffects.trap_battle_record[0] = 20;
  gMonsterEffects.trap_battle_record[1] = 5;

  s_test_effect_count = 1;

  /* --- Test 1: MONSTER_BATTLE_MOD_ATTACKED_BY_TYPE --- */
  memset(&eff, 0, sizeof(eff));
  eff.when = MONSTER_WHEN_FACE_UP;
  eff.action = MONSTER_DO_BATTLE_IMMUNITY;
  eff.target = MONSTER_TARGET_SELF;
  eff.battle_modifier = MONSTER_BATTLE_MOD_ATTACKED_BY_TYPE;
  eff.attacker_type = 1; /* Dragon */
  s_test_effects[0] = eff;

  s_mock_type[1] = 1; /* Attacker is Dragon */
  assert(MonsterEffects_HasBattleImmunity(5) == 1);
  /* Attacker itself does not have immunity */
  assert(MonsterEffects_HasBattleImmunity(20) == 0);

  s_mock_type[1] = 2; /* Attacker is Spellcaster */
  assert(MonsterEffects_HasBattleImmunity(5) == 0);

  /* Outside battle, immunity must be 0 */
  gMonsterEffects.trap_battle_record[0] = -1;
  gMonsterEffects.trap_battle_record[1] = -1;
  assert(MonsterEffects_HasBattleImmunity(5) == 0);
  gMonsterEffects.trap_battle_record[0] = 20;
  gMonsterEffects.trap_battle_record[1] = 5;

  /* --- Test 2: MONSTER_BATTLE_MOD_ATTACKED_BY_ATTRIBUTE --- */
  memset(&eff, 0, sizeof(eff));
  eff.when = MONSTER_WHEN_FACE_UP;
  eff.action = MONSTER_DO_BATTLE_IMMUNITY;
  eff.target = MONSTER_TARGET_SELF;
  eff.battle_modifier = MONSTER_BATTLE_MOD_ATTACKED_BY_ATTRIBUTE;
  eff.attacker_attribute = 1; /* LIGHT */
  s_test_effects[0] = eff;

  s_mock_attr[1] = 1; /* Attacker is LIGHT */
  assert(MonsterEffects_HasBattleImmunity(5) == 1);

  s_mock_attr[1] = 2; /* Attacker is DARK */
  assert(MonsterEffects_HasBattleImmunity(5) == 0);

  /* --- Test 3: MONSTER_BATTLE_MOD_ATTACKED_BY_STAT --- */
  /* Check higher ATK (default / MONSTER_OP_GT) */
  memset(&eff, 0, sizeof(eff));
  eff.when = MONSTER_WHEN_FACE_UP;
  eff.action = MONSTER_DO_BATTLE_IMMUNITY;
  eff.target = MONSTER_TARGET_SELF;
  eff.battle_modifier = MONSTER_BATTLE_MOD_ATTACKED_BY_STAT;
  eff.attacker_stat = 1; /* ATK */
  eff.attacker_stat_op = MONSTER_OP_GT;
  eff.attacker_amount = 2500;
  s_test_effects[0] = eff;

  gDuel_adwCardStats[20] = 3000 | (2500 << 16); /* 3000 ATK, 2500 DEF */
  assert(MonsterEffects_HasBattleImmunity(5) == 1);

  gDuel_adwCardStats[20] = 2500 | (2500 << 16); /* Equal -> strictly higher required */
  assert(MonsterEffects_HasBattleImmunity(5) == 0);

  gDuel_adwCardStats[20] = 2000 | (2500 << 16); /* Lower -> 0 */
  assert(MonsterEffects_HasBattleImmunity(5) == 0);

  /* Check lower ATK (MONSTER_OP_LT) */
  eff.attacker_stat_op = MONSTER_OP_LT;
  eff.attacker_amount = 2500;
  s_test_effects[0] = eff;

  gDuel_adwCardStats[20] = 2000 | (2500 << 16); /* 2000 < 2500 -> immune */
  assert(MonsterEffects_HasBattleImmunity(5) == 1);

  gDuel_adwCardStats[20] = 2500 | (2500 << 16); /* 2500 < 2500 is false */
  assert(MonsterEffects_HasBattleImmunity(5) == 0);

  gDuel_adwCardStats[20] = 3000 | (2500 << 16); /* 3000 < 2500 is false */
  assert(MonsterEffects_HasBattleImmunity(5) == 0);

  /* Check higher DEF */
  eff.attacker_stat = 2; /* DEF */
  eff.attacker_stat_op = MONSTER_OP_GT;
  eff.attacker_amount = 2000;
  s_test_effects[0] = eff;

  gDuel_adwCardStats[20] = 1000 | (2500 << 16); /* 2500 DEF > 2000 */
  assert(MonsterEffects_HasBattleImmunity(5) == 1);

  gDuel_adwCardStats[20] = 1000 | (1800 << 16); /* 1800 DEF <= 2000 */
  assert(MonsterEffects_HasBattleImmunity(5) == 0);

  /* Check lower DEF */
  eff.attacker_stat_op = MONSTER_OP_LT;
  eff.attacker_amount = 2000;
  s_test_effects[0] = eff;

  gDuel_adwCardStats[20] = 1000 | (1500 << 16); /* 1500 DEF < 2000 */
  assert(MonsterEffects_HasBattleImmunity(5) == 1);

  gDuel_adwCardStats[20] = 1000 | (2500 << 16); /* 2500 DEF < 2000 is false */
  assert(MonsterEffects_HasBattleImmunity(5) == 0);

  /* --- Test 4: MONSTER_BATTLE_MOD_ATTACKED_BY_LEVEL --- */
  /* Higher level (MONSTER_OP_GT) */
  memset(&eff, 0, sizeof(eff));
  eff.when = MONSTER_WHEN_FACE_UP;
  eff.action = MONSTER_DO_BATTLE_IMMUNITY;
  eff.target = MONSTER_TARGET_SELF;
  eff.battle_modifier = MONSTER_BATTLE_MOD_ATTACKED_BY_LEVEL;
  eff.attacker_level = 7;
  eff.attacker_level_op = MONSTER_OP_GT;
  s_test_effects[0] = eff;

  s_mock_level[1] = 8; /* Level 8 > 7 */
  assert(MonsterEffects_HasBattleImmunity(5) == 1);

  s_mock_level[1] = 7; /* Equal -> 0 */
  assert(MonsterEffects_HasBattleImmunity(5) == 0);

  s_mock_level[1] = 4; /* Level 4 <= 7 */
  assert(MonsterEffects_HasBattleImmunity(5) == 0);

  /* Lower level (MONSTER_OP_LT) */
  eff.attacker_level_op = MONSTER_OP_LT;
  eff.attacker_level = 5;
  s_test_effects[0] = eff;

  s_mock_level[1] = 4; /* Level 4 < 5 -> immune */
  assert(MonsterEffects_HasBattleImmunity(5) == 1);

  s_mock_level[1] = 5; /* Level 5 < 5 is false */
  assert(MonsterEffects_HasBattleImmunity(5) == 0);

  s_mock_level[1] = 8; /* Level 8 < 5 is false */
  assert(MonsterEffects_HasBattleImmunity(5) == 0);

  /* --- Test 5: MONSTER_BATTLE_MOD_ATTACKED_BY_MONSTER (combined criteria) --- */
  memset(&eff, 0, sizeof(eff));
  eff.when = MONSTER_WHEN_FACE_UP;
  eff.action = MONSTER_DO_BATTLE_IMMUNITY;
  eff.target = MONSTER_TARGET_SELF;
  eff.battle_modifier = MONSTER_BATTLE_MOD_ATTACKED_BY_MONSTER;
  eff.attacker_type = 1;       /* Dragon */
  eff.attacker_attribute = 1;  /* LIGHT */
  eff.attacker_stat = 1;       /* ATK */
  eff.attacker_stat_op = MONSTER_OP_GT;
  eff.attacker_amount = 2500;  /* ATK > 2500 */
  eff.attacker_level = 7;      /* Level > 7 */
  eff.attacker_level_op = MONSTER_OP_GT;
  s_test_effects[0] = eff;

  /* Satisfy all: Dragon, LIGHT, ATK 3000, Level 8 */
  s_mock_type[1] = 1;
  s_mock_attr[1] = 1;
  gDuel_adwCardStats[20] = 3000 | (2500 << 16);
  s_mock_level[1] = 8;
  assert(MonsterEffects_HasBattleImmunity(5) == 1);

  /* Test with lower stat / lower level in combined */
  eff.attacker_stat_op = MONSTER_OP_LT;
  eff.attacker_amount = 3500; /* ATK < 3500 */
  eff.attacker_level_op = MONSTER_OP_LE;
  eff.attacker_level = 8;    /* Level <= 8 */
  s_test_effects[0] = eff;
  assert(MonsterEffects_HasBattleImmunity(5) == 1);

  /* Fail type */
  s_mock_type[1] = 2;
  assert(MonsterEffects_HasBattleImmunity(5) == 0);
  s_mock_type[1] = 1;

  /* Fail attribute */
  s_mock_attr[1] = 2;
  assert(MonsterEffects_HasBattleImmunity(5) == 0);
  s_mock_attr[1] = 1;

  /* Fail ATK */
  gDuel_adwCardStats[20] = 4000 | (2500 << 16); /* 4000 is NOT < 3500 */
  assert(MonsterEffects_HasBattleImmunity(5) == 0);
  gDuel_adwCardStats[20] = 3000 | (2500 << 16);

  /* Fail Level */
  s_mock_level[1] = 9; /* 9 is NOT <= 8 */
  assert(MonsterEffects_HasBattleImmunity(5) == 0);

  puts("test_attacker_criteria_immunity: PASSED");
}

int main(void) {
  test_attacker_vs_defender();
  test_first_second_third_attacked_sequence();
  test_fewer_and_more_monsters_condition();
  test_control_stat_condition();
  test_control_level_condition();
  test_control_type_condition();
  test_control_attribute_condition();
  test_field_spell_condition();
  test_attacker_criteria_immunity();
  puts("\nAll monster attacked condition tests PASSED successfully!");
  return 0;
}
