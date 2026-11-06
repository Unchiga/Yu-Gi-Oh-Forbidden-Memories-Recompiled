/* The CPU's view of "combat" monster effects (monster_effects_duel.h:
 * MonsterEffects_AiStats, MonsterEffects_AiBattleEdge), the AI snapshot and
 * the attack search that use it (func_80027DF8, AiScript_FindKiller), and
 * that view held against the battle itself. Compile as 32-bit for the
 * game's display-object layout. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/pc/cards/monster_effects_duel.c"
#include "../../src/game/ai_script_find_killer.c"
#include "../../src/game/duel_calc_card_stats.c"
#include "../../src/game/func_80027DF8.c"

/* The duel's state the effects read. */
MonsterEffectsState gMonsterEffects;
DuelCardRecord D_801A7AD8[30];
DuelSideState D_800E9FF0[2];
DuelSideState *D_8009B1C8;
u8 D_8009B1D5, D_8009B1B8, D_800907D8[40];
u8 *D_8009B22C;
u16 D_8009B162, gDuel_wSceneStateFlags, gDuel_wCardEffectFlags;
u8 gDuel_bEffectState, gDuel_bQuitDialogState;
s16 D_8009B22A;
DisplayObject *D_800E9EF0[2];
u16 D_8009B178[2];
AiActiveCard gDuel_aActiveCards[112];
DuelDeckCardRecord gDuel_aDeckCardRecords[80];
DuelCardStagingDeckView D_8015C424_cards;
s32 gDuel_adwCardStats[1024];
s32 gAiScript_aMemory[AI_SCRIPT_MEMORY_COUNT];
const char *const MonsterEffect_WhenNames[MONSTER_WHEN_COUNT];
const char *const MonsterEffect_DoNames[MONSTER_DO_COUNT];

/* Cards: an id's effects, type and attribute. */
static MonsterEffect effects[16][4];
static int effect_count[16], types[16], attributes[16];
int Cards_MonsterEffects(int id, const MonsterEffect **out)
{
    *out = effects[id];
    return id > 0 && id < 16 ? effect_count[id] : 0;
}
int Cards_CardEffects(int id, const MonsterEffect **out) { (void)id; *out = 0; return 0; }
int Cards_Type(int id) { return types[id]; }
int Cards_Attribute(int id) { return attributes[id]; }
int Cards_Valid(int id) { return id > 0 && id < 16; }
int Tables_StatCap(int defense) { (void)defense; return 9999; }
int Tables_StatCapEither(void) { return 9999; }
s32 Duel_CalcGuardianStarMatchup(s32 attacker, s32 defender) { (void)attacker; (void)defender; return 0; }

/* A battle tells the mods and plays a sound. */
void Mods_Dispatch(MemoriesModEvent *event) { (void)event; }
void SD_SEPlayFull(u32 sound) { (void)sound; }

/* What runs only when an effect is made, never by the CPU's view. */
int Log_Wanted(LogChannel channel) { (void)channel; return 0; }
void Log_Printf(LogChannel channel, const char *format, ...) { (void)channel; (void)format; }
int Mods_DamageLife(int side, int life, int amount, int battle) { (void)side; (void)amount; (void)battle; assert(0); return life; }
void DuelEffect_StartRetailCardEffect(s32 card, s32 step) { (void)card; (void)step; assert(0); }
DuelEffectRequest *DuelEffect_CreateRequest(s32 id) { (void)id; assert(0); return 0; }
void DuelCard_RemoveFromField(DuelCardRecord *card) { (void)card; assert(0); }
int Fields_Declared(int field) { (void)field; assert(0); return 0; }
int Fields_Carrier(int field) { (void)field; assert(0); return 0; }
void Fields_Activate(int field) { (void)field; assert(0); }

/* FindKiller's operands: the target slot's register, the mode's, the answer's. */
static u8 operands[3], cursor;
s32 AiScript_ReadByte(void) { return operands[cursor++]; }

enum { CPU = 1, PLAYER = 0 };
enum { GIANT = 1, STEER = 2, DRAGON_FOE = 3, WARRIOR = 4, WINGED = 5, BABY = 6 };
#define TYPE_DRAGON 0
#define TYPE_WARRIOR 3

static DuelCardDisplayObject card_objects[30];

static void put(int side, int zone, int card, int face_up)
{
    int record = side * 15 + 5 + zone;
    D_801A7AD8[record].object = &card_objects[record];
    D_801A7AD8[record].card_id = (s16)card;
    D_801A7AD8[record].flags = DUEL_CARD_FLAG_OCCUPIED | (face_up ? 0 : DUEL_CARD_FLAG_FACE_DOWN);
    gMonsterEffects.card[record] = (short)card;
    gMonsterEffects.face_up[record] = (unsigned char)face_up;
}
static void effect(int card, MonsterEffect e) { effects[card][effect_count[card]++] = e; }
/* A "combat" boost of `target` (any type, unless `type` >= 0). */
static MonsterEffect combat(int target, int attack, int defense, int type)
{
    MonsterEffect e;
    memset(&e, 0, sizeof(e));
    e.when = MONSTER_WHEN_COMBAT; e.action = MONSTER_DO_BOOST; e.target = (unsigned char)target;
    e.attack = (short)attack; e.defense = (short)defense; e.type = (signed char)type;
    e.attribute = e.each_attribute = -1; e.each_type = -1;
    return e;
}
static MonsterEffect each(MonsterEffect e, int whose, int type)
{
    e.each = (unsigned char)whose; e.each_type = (signed char)type;
    return e;
}
static int stats(int attack, int defense) { return defense << 16 | attack; }
static int ai(int side, int zone, int attack, int defense)
{
    return MonsterEffects_AiStats(&D_801A7AD8[side * 15 + 5 + zone], stats(attack, defense));
}
static void setup(void)
{
    memset(&gMonsterEffects, 0, sizeof(gMonsterEffects));
    memset(D_801A7AD8, 0, sizeof(D_801A7AD8));
    memset(effects, 0, sizeof(effects));
    memset(effect_count, 0, sizeof(effect_count));
    memset(gDuel_aActiveCards, 0, sizeof(gDuel_aActiveCards));
    gMonsterEffects.ready = 1;
    gDuel_wSceneStateFlags = PHASE_FIELD;
    D_8009B1D5 = CPU;
    memset(D_800E9FF0, -1, sizeof(D_800E9FF0));     /* no hand */
    D_800E9FF0[0].deck_draw_cursor = D_800E9FF0[1].deck_draw_cursor = DECK_SIZE;
    types[GIANT] = types[STEER] = types[WARRIOR] = TYPE_WARRIOR;
    types[DRAGON_FOE] = types[WINGED] = types[BABY] = TYPE_DRAGON;
}
/* FindKiller for target slot `target` against its ATK (0) or DEF (1): the
 * own slot chosen, 0 none. */
static int killer(int target, int mode)
{
    gAiScript_aMemory[1] = target;
    gAiScript_aMemory[2] = mode;
    operands[0] = 1; operands[1] = 2; operands[2] = 3; cursor = 0;
    AiScript_FindKiller();
    return gAiScript_aMemory[3];
}

/* The field of a run of preview_against_battle(): six cards with up to two
 * "combat" boosts each, some monsters on each side, face up or down. */
static unsigned seed = 12345;
static int rnd(int n)
{
    seed = seed * 1103515245u + 12345u;
    return (int)((seed >> 16) % (unsigned)n);
}
static void random_duel(void)
{
    static const int whose[] = {MONSTER_EACH_NONE, MONSTER_EACH_OWN, MONSTER_EACH_OPPONENT, MONSTER_EACH_ALL};
    int side, zone, card, n;
    setup();
    for (card = 1; card <= 6; card++) {
        for (n = rnd(3); n > 0; n--) {
            int type = rnd(2) ? -1 : rnd(2) ? TYPE_DRAGON : TYPE_WARRIOR;
            int counted_type = rnd(2) ? -1 : rnd(2) ? TYPE_DRAGON : TYPE_WARRIOR;
            effect(card, each(combat(rnd(2) ? MONSTER_TARGET_BATTLE : MONSTER_TARGET_SELF, (rnd(5) - 2) * 50,
                                     (rnd(5) - 2) * 50, type), whose[rnd(4)], counted_type));
        }
    }
    for (side = 0; side < 2; side++) {
        for (zone = 0; zone < DUEL_FIELD_ROW_SIZE; zone++) {
            if (!rnd(2)) continue;
            put(side, zone, 1 + rnd(6), rnd(2));
            /* Well inside 0 and the cap: their clamps are the battle's alone. */
            D_801A7AD8[side * 15 + 5 + zone].attack = (s16)(4000 + rnd(10) * 100);
            D_801A7AD8[side * 15 + 5 + zone].defense = (s16)(4000 + rnd(10) * 100);
        }
    }
}

/* What the CPU works out from the snapshot (func_80027DF8) and the edge, as
 * FindKiller does, against what MonsterEffects_Battle then gives the two. */
static void preview_against_battle(void)
{
    static DisplayObject attacker_object, target_object;
    int run, battles = 0, boosted = 0;
    for (run = 0; run < 40000; run++) {
        int a = rnd(DUEL_FIELD_ROW_SIZE), t = rnd(DUEL_FIELD_ROW_SIZE), against_defense = rnd(2);
        int turn = rnd(2), attacker, target, foreseen, packed_a, packed_t, real;
        random_duel();
        D_8009B1D5 = (u8)turn;
        attacker = turn * 15 + 5 + a;
        target = (turn ^ 1) * 15 + 5 + t;
        if (!D_801A7AD8[attacker].card_id || !D_801A7AD8[target].card_id) continue;
        if (against_defense) D_801A7AD8[target].flags |= DUEL_CARD_FLAG_DEFENSE_POSITION;
        func_80027DF8(&gDuel_aActiveCards[AI_SLOT_OWN_MONSTER_FIRST], turn);
        func_80027DF8(&gDuel_aActiveCards[AI_SLOT_OPPONENT_MONSTER_FIRST], turn ^ 1);
        foreseen = gDuel_aActiveCards[AI_SLOT_OWN_MONSTER_FIRST + a].attack -
            (against_defense ? gDuel_aActiveCards[AI_SLOT_OPPONENT_MONSTER_FIRST + t].defense
                             : gDuel_aActiveCards[AI_SLOT_OPPONENT_MONSTER_FIRST + t].attack) +
            MonsterEffects_AiBattleEdge(AI_SLOT_OWN_MONSTER_FIRST + a, AI_SLOT_OPPONENT_MONSTER_FIRST + t,
                                        against_defense);
        attacker_object.field_6A = (s16)attacker;
        target_object.field_6A = (s16)target;
        D_800E9EF0[0] = &attacker_object;
        D_800E9EF0[1] = &target_object;
        D_8009B178[1] = D_801A7AD8[target].flags;
        gDuel_wSceneStateFlags = PHASE_BATTLE;
        MonsterEffects_Battle();
        packed_a = Duel_CalcCardStats(&D_801A7AD8[attacker]);
        packed_t = Duel_CalcCardStats(&D_801A7AD8[target]);
        real = (packed_a & 0xFFFF) - (against_defense ? packed_t >> 16 : packed_t & 0xFFFF);
        assert(foreseen == real);
        battles++;
        boosted += gMonsterEffects.battle_attack[attacker] || gMonsterEffects.battle_attack[target] ||
            gMonsterEffects.battle_defense[target];
    }
    assert(battles > 5000 && boosted > battles / 2);
}

int main(void)
{
    preview_against_battle();
    setup();
    /* No monster declares an effect: the disc's stats, nothing added. */
    put(PLAYER, 0, GIANT, 1); put(CPU, 0, STEER, 1);
    assert(ai(PLAYER, 0, 1200, 1000) == stats(1200, 1000));
    assert(MonsterEffects_AiBattleEdge(1, 56, 0) == 0 && MonsterEffects_AiBattleEdge(56, 1, 1) == 0);
    gMonsterEffects.ready = 0;
    effect(GIANT, combat(MONSTER_TARGET_SELF, 1500, 0, -1));
    assert(ai(PLAYER, 0, 1200, 1000) == stats(1200, 1000));   /* the duel's look not taken yet */
    gMonsterEffects.ready = 1;

    /* Its own boost, which any battle of its gives it; only on itself. */
    assert(ai(PLAYER, 0, 1200, 1000) == stats(2700, 1000));
    assert(ai(CPU, 0, 1800, 1300) == stats(1800, 1300));
    assert(ai(PLAYER, 0, 9000, 1000) == stats(9999, 1000));    /* within the cap */
    /* The battle's edges leave it out: it is in the snapshot's stats. */
    assert(MonsterEffects_AiBattleEdge(1, 56, 0) == 0);
    /* Not a combat boost: a face_up boost is Duel_CalcCardStats's, a heal no stat. */
    setup(); put(PLAYER, 0, GIANT, 1);
    {
        MonsterEffect up = combat(MONSTER_TARGET_SELF, 500, 0, -1), heal = combat(MONSTER_TARGET_SELF, 0, 0, -1);
        up.when = MONSTER_WHEN_FACE_UP;
        heal.action = MONSTER_DO_HEAL; heal.amount = 500;
        effect(GIANT, up); effect(GIANT, heal);
    }
    assert(ai(PLAYER, 0, 1200, 1000) == stats(1200, 1000));
    /* A DEF boost; a cut below 0 stops at 0. */
    setup(); put(PLAYER, 0, GIANT, 1);
    effect(GIANT, combat(MONSTER_TARGET_SELF, -2000, 1500, -1));
    assert(ai(PLAYER, 0, 1200, 1000) == stats(0, 2500));

    /* "battle": the player's Winged Dragon takes 1000 ATK off what it
     * battles, Dragons excepted; the edge is the attacker's ATK less the
     * target's, attacking it and attacked by it. */
    setup(); put(PLAYER, 1, WINGED, 1); put(CPU, 0, STEER, 1); put(CPU, 1, BABY, 1);
    effect(WINGED, combat(MONSTER_TARGET_BATTLE, -1000, -300, TYPE_WARRIOR));
    assert(ai(CPU, 0, 1800, 1300) == stats(1800, 1300));
    assert(MonsterEffects_AiBattleEdge(1, 57, 0) == -1000);    /* Steer attacks it */
    assert(MonsterEffects_AiBattleEdge(1, 57, 1) == -1000);    /* ... in defense: its own DEF is not cut */
    assert(MonsterEffects_AiBattleEdge(57, 1, 0) == 1000);     /* it attacks Steer in attack */
    assert(MonsterEffects_AiBattleEdge(57, 1, 1) == 300);      /* ... in defense */
    assert(MonsterEffects_AiBattleEdge(2, 57, 0) == 0);        /* Baby Dragon is no Warrior */
    assert(MonsterEffects_AiBattleEdge(1, 58, 0) == 0 && MonsterEffects_AiBattleEdge(6, 57, 0) == 0);
    /* Seen from the other side's turn the slots swap. */
    D_8009B1D5 = PLAYER;
    assert(MonsterEffects_AiBattleEdge(2, 56, 0) == 1000 && MonsterEffects_AiBattleEdge(56, 2, 0) == -1000);

    /* for_each: counted with both of the battle's monsters face up, as the
     * battle counts them; the snapshot's own boost counts the card itself. */
    setup(); put(CPU, 0, BABY, 0); put(CPU, 1, DRAGON_FOE, 1); put(CPU, 2, WINGED, 0); put(CPU, 3, STEER, 1);
    effect(BABY, each(combat(MONSTER_TARGET_SELF, 300, 0, -1), MONSTER_EACH_OWN, TYPE_DRAGON));
    assert(ai(CPU, 0, 1200, 700) == stats(1800, 700));          /* itself and the face-up one */
    put(PLAYER, 0, WINGED, 0);
    effect(WINGED, each(combat(MONSTER_TARGET_BATTLE, -100, 0, -1), MONSTER_EACH_ALL, TYPE_DRAGON));
    /* The player's face-down Winged Dragon against Baby Dragon: both count,
     * the CPU's face-down Winged Dragon does not. */
    assert(MonsterEffects_AiBattleEdge(1, 56, 0) == -300);

    /* A face-down attacker and target: the snapshot counts each one alone
     * for its own "for_each", the edge adds the other, as the battle does. */
    setup(); put(CPU, 0, BABY, 0); put(PLAYER, 0, WINGED, 0);
    effect(BABY, each(combat(MONSTER_TARGET_SELF, 300, 0, -1), MONSTER_EACH_ALL, TYPE_DRAGON));
    effect(WINGED, each(combat(MONSTER_TARGET_SELF, 0, 200, -1), MONSTER_EACH_ALL, TYPE_DRAGON));
    assert(ai(CPU, 0, 1200, 700) == stats(1500, 700));
    assert(ai(PLAYER, 0, 1400, 1200) == stats(1400, 1400));
    assert(MonsterEffects_AiBattleEdge(1, 56, 0) == 300);
    assert(MonsterEffects_AiBattleEdge(1, 56, 1) == 100);

    /* FindKiller: the CPU's Steer (1800) against the player's Giant (1200)
     * in attack, Giant +1500 ATK before combat, then a Winged Dragon
     * cutting what it battles by 1000. */
    setup(); put(PLAYER, 0, GIANT, 1); put(PLAYER, 1, WINGED, 1); put(CPU, 0, STEER, 1);
    gDuel_aActiveCards[1] = (AiActiveCard){STEER, 1800, 1300, DUEL_CARD_FLAG_OCCUPIED, 0, 0, 0, 0};
    gDuel_aActiveCards[56] = (AiActiveCard){GIANT, 1200, 1000, DUEL_CARD_FLAG_OCCUPIED, 0, 0, 0, 0};
    gDuel_aActiveCards[57] = (AiActiveCard){WINGED, 1400, 1200, DUEL_CARD_FLAG_OCCUPIED, 0, 0, 0, 0};
    assert(killer(56, 0) == 1 && killer(57, 0) == 1);
    effect(GIANT, combat(MONSTER_TARGET_SELF, 1500, 0, -1));
    {
        int packed = ai(PLAYER, 0, 1200, 1000);
        gDuel_aActiveCards[56].attack = (s16)packed;
    }
    assert(killer(56, 0) == 0);
    effect(WINGED, combat(MONSTER_TARGET_BATTLE, -1000, 0, -1));
    assert(killer(57, 0) == 0 && killer(57, 1) == 0);          /* 800 beats neither its ATK nor its DEF */
    /* The CPU's own boost on what it battles: Steer -600 DEF. */
    effect(STEER, combat(MONSTER_TARGET_BATTLE, 0, -600, -1));
    gDuel_aActiveCards[56].defense = 2000;
    assert(killer(56, 1) == 1);
    puts("monster effects: the CPU's view of combat boosts passed");
    return 0;
}
