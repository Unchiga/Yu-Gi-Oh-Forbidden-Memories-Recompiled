#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "planner.h"

static int probes;
static HmCard card(int id)
{
    HmCard c = {id, id, 0, 1000, 800, 1, 2, 0, 0};
    if (id == 100) c.attack = c.defense = 3000;
    if (id == 101) c.attack = c.defense = 3500;
    return c;
}
static int fusion(int a, int b)
{
    probes++;
    if ((a == 10 && b == 11) || (a == 11 && b == 10)) return 100;
    if ((a == 100 && b == 12) || (a == 12 && b == 100)) return 101;
    return 0;
}
static int equip(int a, int b) { return a == 657 && b > 0; }
static int terrain(int type, int field) { return type == 0 && field == 3 ? 500 : 0; }
static int ritual(int id) { return id == 700; }
/* A mod's table: star 11 beats Saturn (3) by 1000 and nothing else counts. */
static int custom_matchup(int a, int d) { return a == 11 && d == 3 ? 1000 : a == 3 && d == 11 ? -1000 : 0; }
static const HmRules rules = {card, fusion, equip, terrain, ritual};
static HmOptions opts(void)
{
    HmOptions o = {1,1,1,1,1,1,1,4,6000,50,150,1};
    return o;
}
static HmBoard empty(void)
{
    HmBoard b = {0};
    b.lp = b.max_lp = b.enemy_lp = 8000;
    b.spellbinding = 500; b.shadow_spell = 1000;
    return b;
}
static HmCard monster(int id, int atk, int def)
{
    HmCard c = card(id); c.attack = atk; c.defense = def; return c;
}
static HmCard magic(int id, int type)
{
    HmCard c = {id,id,type,0,0,0,0,0,0}; return c;
}

static void tactical_cases(void)
{
    HmBoard b = empty(); HmOptions o = opts(); HmDecision d;
    b.own[0] = monster(2, 800, 2000); b.enemy_lp = 500;
    d = Hm_PlanField(&b,&o,&rules,0);
    assert(d.selection[9] == 1 && d.selection[10] == 58 && d.score >= HM_WIN);
    b.pinned = 1;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 0);
    b.pinned = 0; b.enemy_lp = 8000; b.enemy[0] = monster(20,1800,1000);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[11] == 1);
    b.own[0].flags |= HM_USED;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.result == 3);
    /* Equally safe stances (an empty or pinned opponent): a wall takes
       defense, an attacker keeps attack. */
    b = empty(); b.own[0] = monster(2, 0, 3000);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 1 && d.selection[11] == 1);
    b.own[0] = monster(2, 1800, 1000); b.pinned = 1;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 1 && d.selection[11] == 0);
    b.pinned = 0;
    b.own[0] = monster(2, 500, 3500); b.enemy[0] = monster(20,2600,1000); b.enemy_pinned = 1;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 1 && d.selection[11] == 1);

    b = empty(); b.enemy[0] = monster(20,2100,1000); b.enemy[0].star = 3;
    assert(Hm_ChooseStar(&b,monster(10,1800,1000),0) == 1);
    assert(Hm_StarBonus(2,3) == 500 && Hm_StarBonus(3,2) == -500);
    assert(Hm_StarBonus(6,1) == 500 && Hm_StarBonus(10,7) == 500);
    assert(Hm_StarBonus(6,7) == 0 && Hm_StarBonus(0,1) == 0);
    /* In the game the planner scores stars by the game's matchup, which a
       mod's "guardian_stars" changes (stars 11-15 included). */
    Hm_StarMatchup = custom_matchup;
    assert(Hm_StarBonus(11,3) == 1000 && Hm_StarBonus(3,11) == -1000 && Hm_StarBonus(2,3) == 0);
    assert(Hm_StarBonus(0,11) == 0 && Hm_StarBonus(11,16) == 0);
    b = empty(); b.enemy[0] = monster(20,2100,1000); b.enemy[0].star = 3;
    {
        HmCard c = monster(10,1800,1000);
        c.star = 2; c.star2 = 11;
        assert(Hm_ChooseStar(&b,c,0) == 1);
    }
    Hm_StarMatchup = 0;

    b = empty(); b.own[0] = monster(1,3000,2500); b.own[1] = monster(3,1600,1000);
    b.enemy[0] = monster(4,1500,1000); b.enemy_lp = 3000;
    d = Hm_PlanField(&b,&o,&rules,0);
    assert(d.selection[9] == 2 && d.selection[10] == 56 && d.score >= HM_WIN);
    b.own[0] = (HmCard){0}; b.own[1].attack = 1500; b.enemy[0].defense = 3000;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 2 && d.selection[10] == 56);
    o.trades = 0;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 0);

    /* A 0 ATK wall does not lead an attack it adds nothing to. */
    b = empty(); o = opts(); b.own[0] = monster(2,0,3000); b.own[1] = monster(3,3000,2500);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 2 && d.selection[10] == 58);
    /* A 500/3500 wall takes defense rather than hit for 500; a mid monster
       still attacks an empty field, and a wall still makes a lethal hit. */
    b.own[1] = (HmCard){0}; b.own[0] = monster(2,500,3500);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 1 && d.selection[10] == 0 && d.selection[11] == 1);
    b.own[0] = monster(2,1500,1800);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 1 && d.selection[10] == 58);
    b.own[0] = monster(2,400,2200); b.enemy_lp = 300;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 58 && d.score > HM_WIN / 2);
    /* An equal trade with the last monster, which would hold in defense,
       opens the field to whatever the opponent plays next. */
    b = empty(); b.own[0] = monster(3,1500,2000); b.enemy[0] = monster(4,1500,2100);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 1 && d.selection[10] == 0 && d.selection[11] == 1);
    b.own[1] = monster(6,1000,2500); b.own[1].flags = HM_USED;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 1 && d.selection[10] == 56);

    b = empty(); o = opts(); b.own[0] = monster(1,3000,2500);
    b.enemy[0] = (HmCard){-1,0,0,0,0,0,0,HM_DOWN,0}; o.blind_risk = 0;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 0);
    o.blind_risk = 100;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 56 && d.score < HM_WIN);
}

static void spell_cases(void)
{
    HmBoard b = empty(); HmOptions o = opts(); HmDecision d; int i;
    b.enemy_lp = 900; b.back[0] = magic(347,20);
    d = Hm_PlanField(&b,&o,&rules,0);
    assert(d.selection[9] == 6 && d.selection[10] == 6 && d.score >= HM_WIN);
    b.enemy_back[0] = magic(687,21);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.result == 3);
    b.enemy_back[0] = (HmCard){-1,0,21,0,0,0,0,HM_DOWN,0};
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 6);

    b = empty(); b.back[0] = magic(342,20);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.result == 3);
    b.lp = 1000;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 6);
    b = empty(); b.back[0] = magic(672,20);
    b.enemy_back[0] = (HmCard){-1,0,21,0,0,0,0,HM_DOWN,0};
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 6);

    b = empty(); b.back[0] = magic(336,20); b.own[0] = monster(1,3000,2500);
    b.enemy[0] = monster(4,500,500); b.pinned = 1;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] <= 5);
    b.own[0] = (HmCard){0};
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 6);

    b = empty(); b.back[0] = magic(332,20); b.own[0] = monster(10,1000,1000); b.pinned = 1;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 6);
    b.terrain = 3;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] != 6);

    b = empty(); b.own[0] = monster(10,1000,1000); b.own[1] = monster(11,2000,1000);
    b.enemy[0] = monster(12,2500,2000); b.back[0] = magic(657,23);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 6 && d.selection[10] == 2);
    /* An equip goes before the attack it improves, even a direct one. */
    b = empty(); b.own[0] = monster(1,3000,2500); b.back[0] = magic(657,23);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 6 && d.selection[10] == 1);
    /* Stop Defense goes before the attacks it opens up. */
    b = empty(); b.own[0] = monster(10,2000,1000); b.own[1] = monster(11,2000,1000);
    b.enemy[0] = monster(12,1500,2500); b.enemy[0].flags = HM_DEFENSE; b.enemy[1] = monster(13,1000,500);
    b.back[0] = magic(320,20);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 6);
    /* Shadow Spell only when it changes a fight or a threat. */
    b = empty(); b.own[0] = monster(1,3000,2500); b.own[0].flags = HM_USED;
    b.enemy[0] = monster(12,1000,800); b.enemy[0].flags = HM_DEFENSE; b.back[0] = magic(669,20);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] != 6);
    b.own[0] = monster(10,1800,1000); b.enemy[0] = monster(12,2200,2000);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 6);
    /* Swords only against a threat. */
    b = empty(); b.own[0] = monster(1,3000,2500); b.own[0].flags = HM_USED;
    b.enemy[0] = monster(12,1000,800); b.back[0] = magic(348,20);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] != 6);
    b.own[0] = monster(10,1000,800); b.own[0].flags = HM_USED; b.enemy[0] = monster(12,2500,2000);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 6);
    /* The reported board: weak defenders an unused attacker beats anyway. */
    b = empty(); b.own[0] = monster(1,3000,2500); b.back[0] = magic(669,20);
    for (i = 0; i < 3; i++) { b.enemy[i] = monster(12 + i,1000,800); b.enemy[i].flags = HM_DEFENSE; }
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 1);
    /* Spellbinding Circle's one level turns this fight. */
    b = empty(); b.own[0] = monster(10,1800,1000); b.enemy[0] = monster(12,2200,2000);
    b.back[0] = magic(349,20);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 6);
    /* No equip first on a turn the attack already wins. */
    b = empty(); b.own[0] = monster(1,2000,1500); b.enemy_lp = 1500; b.back[0] = magic(657,23);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 1 && d.score >= HM_WIN);
    /* From the hand, Swords over a weak monster when the opponent's next
       turn would end the duel. */
    b = empty(); b.lp = 1500; b.own[0] = monster(10,1000,800);
    for (i = 0; i < 3; i++) b.enemy[i] = monster(12 + i,2000,1000);
    b.hand[0] = magic(348,20); b.hand[1] = monster(11,1200,1000); b.hand_count = 2;
    d = Hm_PlanHand(&b,&o,&rules); assert(d.selection[0] == 11 && !d.selection[8]);
    b = empty(); b.back[0] = magic(999,20);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.result == 3);
}

static void hand_cases(void)
{
    HmBoard b = empty(); HmOptions o = opts(); HmDecision d;
    b.hand_count = 2; b.hand[0] = card(10); b.hand[1] = card(11);
    d = Hm_PlanHand(&b,&o,&rules);
    assert(d.result == 1 && d.selection[0] >= 11 && d.selection[1] >= 11 && !d.selection[2]);
    assert(d.selection[0] != d.selection[1] && d.selection[6] == 1);
    o.fusion = 0; d = Hm_PlanHand(&b,&o,&rules); assert(d.selection[1] == 0);
    o.fusion = 1; b.own[0] = card(10); b.hand[0] = card(11); b.hand_count = 1;
    d = Hm_PlanHand(&b,&o,&rules);
    assert(d.selection[0] == 11 && !d.selection[1] && !d.selection[8]);
    b = empty(); b.hand_count = 3; b.hand[0] = card(10); b.hand[1] = card(11); b.hand[2] = card(12);
    o.materials = 2; d = Hm_PlanHand(&b,&o,&rules); assert(!d.selection[2]);
    o.materials = 3; o.material_cost = 0; d = Hm_PlanHand(&b,&o,&rules); assert(d.selection[2] >= 11);
    b = empty(); b.hand_count = 1; b.hand[0] = magic(347,20); b.enemy_lp = 900;
    d = Hm_PlanHand(&b,&o,&rules); assert(d.selection[0] == 11 && !d.selection[8]);
    /* A replacement Magic with an ai_effect acts like that analogue; without
     * one it is safely set rather than guessed as its obsolete retail card.
     * A trap analogue is still a reactive face-down card, never a spell. */
    b.hand[0] = (HmCard){500,347,20,0,0,0,0,0,0};
    d = Hm_PlanHand(&b,&o,&rules); assert(d.selection[0] == 11 && !d.selection[8]);
    b.hand[0].effect = 0;
    d = Hm_PlanHand(&b,&o,&rules); assert(d.selection[0] == 11 && d.selection[8]);
    b.hand[0] = (HmCard){501,681,21,0,0,0,0,0,0};
    d = Hm_PlanHand(&b,&o,&rules);
    assert(d.selection[0] == 11 && d.selection[6] == 6 && d.selection[8]);
    /* Retail trap analogues currently have no utility score. Keep this
     * deliberately cross-type fixture as a future-proof guard: even if a
     * Trap is assigned positive utility, it must still be set, never cast. */
    b.hand[0] = (HmCard){502,347,21,0,0,0,0,0,0};
    d = Hm_PlanHand(&b,&o,&rules); assert(d.selection[0] == 11 && d.selection[8]);
    b = empty(); b.hand_count = 1; b.hand[0] = magic(657,23);
    b.own[0] = monster(10,1800,1000); b.own[0].star = 2;
    b.enemy[0] = monster(20,2600,1000); b.enemy[0].star = 3;
    o.fusion = 0;
    d = Hm_PlanHand(&b,&o,&rules);
    assert(d.selection[6] == 1 && !d.selection[8] && d.selection[7] == 1);
}

static void retail_cases(void)
{
    HmBoard b = empty(); HmOptions o = opts(); HmDecision d, retail = {{0},3,0,0};
    b.own[0] = monster(2,800,2000); b.enemy[0] = monster(3,3000,3000);
    o.attacks = o.defense = o.spells = 0;
    d = Hm_PlanField(&b,&o,&rules,&retail); assert(d.result == 3);
    o.attacks = 1;
    d = Hm_PlanField(&b,&o,&rules,&retail); assert(d.result == 3);
    retail.result = 2; retail.selection[9] = 1; retail.selection[11] = 1;
    d = Hm_PlanField(&b,&o,&rules,&retail);
    assert(d.result == 2 && d.selection[9] == 1 && d.selection[11] == 1);
}

static unsigned random_state = 1;
static unsigned rnd(void) { random_state = random_state * 1664525u + 1013904223u; return random_state; }
static void bounded_fixtures(void)
{
    int round, i, j;
    HmOptions o = opts(); o.budget = 128; o.materials = 5;
    for (round = 0; round < 500; round++) {
        HmBoard b = empty(); HmBoard saved; HmDecision d;
        b.hand_count = 1 + rnd() % 20;
        for (i = 0; i < b.hand_count; i++) b.hand[i] = card(10 + rnd() % 3);
        for (i = 0; i < 5; i++) {
            if (rnd() % 2) b.own[i] = monster(10 + i, rnd() % 4000, rnd() % 4000);
            if (rnd() % 2) b.enemy[i] = monster(20 + i, rnd() % 4000, rnd() % 4000);
            b.enemy[i].flags = rnd() % 2 ? HM_DEFENSE : 0;
        }
        saved = b; probes = 0; d = Hm_PlanHand(&b,&o,&rules);
        assert(d.result == 1 && probes <= o.budget && d.nodes <= o.budget);
        assert(!d.selection[5] && d.selection[6] >= 1 && d.selection[6] <= 5);
        for (i = 0; i < 5 && d.selection[i]; i++) {
            assert(d.selection[i] >= 11 && d.selection[i] < 11 + b.hand_count);
            for (j = 0; j < i; j++) assert(d.selection[i] != d.selection[j]);
        }
        d = Hm_PlanField(&b,&o,&rules,0);
        assert(d.nodes <= o.budget && (d.result == 2 || d.result == 3));
        if (d.result == 2) {
            assert(d.selection[9] >= 1 && d.selection[9] <= 5);
            assert(b.own[d.selection[9]-1].id);
            if (d.selection[10]) {
                int t = d.selection[10] - 56;
                assert(t >= 0 && t < 5);
                assert(b.enemy[t].id || d.selection[10] == 58);
            }
        }
        assert(!memcmp(&b,&saved,sizeof(b)));
    }
}

int main(void)
{
    tactical_cases(); spell_cases(); hand_cases(); retail_cases(); bounded_fixtures();
    puts("AI hard mode: tactical, spell, fusion and 500 bounded fixtures passed");
    return 0;
}
