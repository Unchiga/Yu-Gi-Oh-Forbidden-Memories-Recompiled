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
    HmCard c = {id, id, 0, 1000, 800, 1, 2, 0, 0, 0};
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
    HmOptions o = {1,1,1,1,1,1,1,4,6000,50,150,1,1};
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
    HmCard c = {id,id,type,0,0,0,0,0,0,0}; return c;
}

static void tactical_cases(void)
{
    HmBoard b = empty(); HmOptions o = opts(); HmDecision d; int i;
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
    assert(Hm_ChooseStar(&b,&o,monster(10,1800,1000),0) == 1);
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
        assert(Hm_ChooseStar(&b,&o,c,0) == 1);
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
    b.own[0] = monster(3,1500,1800);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 1 && d.selection[10] == 58);
    b.own[0] = monster(2,400,2200); b.enemy_lp = 300;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 58 && d.score > HM_WIN / 2);
    /* An equal trade with the last monster, which would hold in defense,
       opens the field to whatever the opponent plays next. */
    b = empty(); b.own[0] = monster(3,1500,2000); b.enemy[0] = monster(4,1500,2100);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 1 && d.selection[10] == 0 && d.selection[11] == 1);
    b.own[1] = monster(6,1000,2500); b.own[1].flags = HM_USED;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 1 && d.selection[10] == 56);

    /* Millennium Shield beats a DEF 100 defender only by its star (0 + 500):
       it does not take it out so the others can attack directly, and stay
       the opponent's free hit, unless that wins the duel or Swords holds the
       opponent through its next turn. A 0/3000 off the original script's
       list is a wall by its stats alike. */
    for (i = 0; i < 2; i++) {
        b = empty(); o = opts();
        b.own[0] = monster(i ? 5 : 362,0,3000); b.own[0].star = 9;
        b.own[1] = monster(3,2700,0); b.own[2] = monster(4,2300,1900);
        b.enemy[0] = monster(20,900,100); b.enemy[0].star = 10; b.enemy[0].flags = HM_DEFENSE;
        b.enemy[1] = monster(21,2600,2500); b.enemy[1].star = 8; b.enemy[1].flags = HM_DEFENSE;
        d = Hm_PlanField(&b,&o,&rules,0); assert(d.result == 2 && d.selection[9] != 1);
        b.own[1].flags = b.own[2].flags = HM_USED;
        d = Hm_PlanField(&b,&o,&rules,0);
        assert(d.selection[9] == 1 && !d.selection[10] && d.selection[11] == 1);
        b.own[1].flags = b.own[2].flags = 0; b.enemy_pinned = 1;
        d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 1 && d.selection[10] == 56);
        b.enemy_pinned = 0; b.enemy_lp = 2300;
        d = Hm_PlanField(&b,&o,&rules,0);
        assert(d.selection[9] == 1 && d.selection[10] == 56 && d.score >= HM_WIN);
    }
    /* No free direct damage either: an 1800/3600 that nothing seen or
       unseen beats stays a wall, unless the hit wins the duel. */
    b = empty(); b.own[0] = monster(5,1800,3600);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 1 && !d.selection[10] && d.selection[11] == 1);
    b.enemy_lp = 1800;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 58 && d.score >= HM_WIN);
    /* Nor is a wall's star chosen for a fight it will not make: this 800/1700
       takes Mercury, which holds against the Neptune 1500, not Mars, which
       would only beat the Jupiter defender. */
    b = empty();
    b.enemy[0] = monster(20,1500,500); b.enemy[0].star = 6;
    b.enemy[1] = monster(21,1700,1100); b.enemy[1].star = 2; b.enemy[1].flags = HM_DEFENSE;
    {
        HmCard c = monster(30,800,1700);
        c.star = 1; c.star2 = 7;
        assert(Hm_ChooseStar(&b,&o,c,0) == 1);
        c.star = 7; b.own[0] = c; b.own[0].zone = 1;
        d = Hm_PlanField(&b,&o,&rules,0);
        assert(d.selection[9] == 1 && !d.selection[10] && d.selection[11] == 1);
    }

    /* A star is chosen for the plan it leads to: this 1800/2200 is not
       worth sending at the 0/300 (the 2600 hits back), so it defends, and
       its first star holds against the 2600 where the second, which only
       helps that attack, does not. */
    b = empty();
    b.enemy[0] = monster(20,2600,1000); b.enemy[0].star = 2;
    b.enemy[1] = monster(21,0,300); b.enemy[1].star = 5; b.enemy[1].flags = HM_DEFENSE;
    {
        HmCard c = monster(10,1800,2200);
        c.star = 1; c.star2 = 4;
        assert(Hm_ChooseStar(&b,&o,c,0) == 0);
        b.own[0] = c; b.own[0].zone = 1;
        d = Hm_PlanField(&b,&o,&rules,0);
        assert(d.selection[9] == 1 && !d.selection[10] && d.selection[11] == 1);
    }

    b = empty(); o = opts(); b.own[0] = monster(1,3000,2500);
    b.enemy[0] = (HmCard){-1,0,0,0,0,0,0,HM_DOWN,0,0}; o.blind_risk = 0;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 0);
    o.blind_risk = 100;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 56 && d.score < HM_WIN);
    b.enemy[0].flags |= HM_DEFENSE; o.blind_risk = 0;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 0);

    /* Face-down monsters, by their position alone. In attack position an
       1800 attacker stays below the 2000 the default risk asks for; in
       defense a failed attack costs only the difference, so it goes, unless
       the option is off; a 1000 attacker would mostly bounce, and does not. */
    b = empty(); o = opts(); b.own[0] = monster(3,1800,1000); b.own[0].zone = 1;
    b.enemy[0] = (HmCard){-1,0,0,0,0,0,0,HM_DOWN,0,0};
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 1 && d.selection[10] == 0);
    b.enemy[0].flags |= HM_DEFENSE;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 1 && d.selection[10] == 56);
    o.blind_defense = 0;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 0);
    o = opts(); b.own[0] = monster(3,1000,1000);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 0);
    /* A cautious risk setting still raises the bar: 1800 at 10. */
    o.blind_risk = 10; b.own[0] = monster(3,1700,1000);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 0);
    b.own[0].attack = 1800;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 56);
    o = opts();
    /* The defender's price, term by term (default risk): a 1500 goes, a
       1300 does not (the 500 LP a failed attack may cost decides); at risk
       30 the 1500 does not either (the removal weighs less); and an 1800
       that a visible 2600 would punish left in attack stays back. */
    b.own[0] = monster(3,1500,1000);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 56);
    b.own[0].attack = 1300;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 0);
    o.blind_risk = 30; b.own[0].attack = 1500;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 0);
    o = opts(); b.own[0] = monster(3,1800,2700); b.enemy[1] = monster(20,2600,500);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 1 && d.selection[10] != 56);
    /* Whatever a hidden defender hides, the price is the same. */
    {
        HmBoard leaked;
        HmDecision seen;
        b = empty(); b.own[0] = monster(3,1500,1000); b.own[0].zone = 1;
        b.enemy[0] = (HmCard){-1,0,0,0,0,0,0,HM_DOWN | HM_DEFENSE,0,0};
        d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 56);
        for (i = 0; i < 2; i++) {
            leaked = b; leaked.enemy[0].attack = 3000; leaked.enemy[0].defense = i ? 4000 : 100;
            leaked.enemy[0].star = 3; leaked.enemy[0].type = 4;
            seen = Hm_PlanField(&leaked,&o,&rules,0);
            assert(seen.result == d.result && !memcmp(seen.selection, d.selection, 12));
        }
    }
    /* A 2000 attacker no longer hits a face-down attacker blind when a
       visible 2600 would punish it left in attack position. */
    b = empty(); b.own[0] = monster(3,2000,2500); b.own[0].zone = 1;
    b.enemy[0] = (HmCard){-1,0,0,0,0,0,0,HM_DOWN,0,0};
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] == 56);
    b.enemy[1] = monster(20,2600,500);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 1 && d.selection[10] != 56);
}

static void spell_cases(void)
{
    HmBoard b = empty(); HmOptions o = opts(); HmDecision d; int i;
    b.enemy_lp = 900; b.back[0] = magic(347,20);
    d = Hm_PlanField(&b,&o,&rules,0);
    assert(d.selection[9] == 6 && d.selection[10] == 6 && d.score >= HM_WIN);
    b.enemy_back[0] = magic(687,21);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.result == 3);
    b.enemy_back[0] = (HmCard){-1,0,21,0,0,0,0,HM_DOWN,0,0};
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 6);

    b = empty(); b.back[0] = magic(342,20);
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.result == 3);
    b.lp = 1000;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 6);
    b = empty(); b.back[0] = magic(672,20);
    b.enemy_back[0] = (HmCard){-1,0,21,0,0,0,0,HM_DOWN,0,0};
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
    b.hand[0] = (HmCard){500,347,20,0,0,0,0,0,0,0};
    d = Hm_PlanHand(&b,&o,&rules); assert(d.selection[0] == 11 && !d.selection[8]);
    b.hand[0].effect = 0;
    d = Hm_PlanHand(&b,&o,&rules); assert(d.selection[0] == 11 && d.selection[8]);
    b.hand[0] = (HmCard){501,681,21,0,0,0,0,0,0,0};
    d = Hm_PlanHand(&b,&o,&rules);
    assert(d.selection[0] == 11 && d.selection[6] == 6 && d.selection[8]);
    /* Retail trap analogues currently have no utility score. Keep this
     * deliberately cross-type fixture as a future-proof guard: even if a
     * Trap is assigned positive utility, it must still be set, never cast. */
    b.hand[0] = (HmCard){502,347,21,0,0,0,0,0,0,0};
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
    /* The script ends the field phase only with no unused monster left. */
    o.attacks = 1; b.own[0].flags = HM_USED;
    d = Hm_PlanField(&b,&o,&rules,&retail); assert(d.result == 3);
    b.own[0].flags = 0;
    retail.result = 2; retail.selection[9] = 1; retail.selection[11] = 1;
    d = Hm_PlanField(&b,&o,&rules,&retail);
    assert(d.result == 2 && d.selection[9] == 1 && d.selection[11] == 1);

    /* Without situational defense, a back-row card the planner turns down
       still leaves the script's rules for the field: its walls (Millennium
       Shield) to defense, then, no attack being worth it, the rest but the
       eight that stay. */
    b = empty(); o = opts(); o.defense = 0;
    b.own[0] = monster(3,1800,1000); b.own[1] = monster(362,0,3000); b.own[1].zone = 2;
    b.enemy[0] = monster(20,2500,1000); b.back[0] = magic(999,20);
    retail = (HmDecision){{0},2,0,0}; retail.selection[9] = 6; retail.selection[10] = 6;
    d = Hm_PlanField(&b,&o,&rules,&retail);
    assert(d.result == 2 && d.selection[9] == 2 && !d.selection[10] && d.selection[11] == 1);
    b.own[1].flags = HM_USED | HM_DEFENSE;
    d = Hm_PlanField(&b,&o,&rules,&retail);
    assert(d.result == 2 && d.selection[9] == 1 && !d.selection[10] && d.selection[11] == 1);
    b.own[0] = monster(1,3000,2500); b.enemy[0] = monster(20,3500,1000);
    d = Hm_PlanField(&b,&o,&rules,&retail);
    assert(d.result == 2 && d.selection[9] == 1 && !d.selection[10] && !d.selection[11]);
    /* The script's own order: a back-row card the planner plays still
       comes before the walls. */
    b.own[1].flags = 0; b.back[0] = magic(347,20); b.enemy_lp = 900;
    d = Hm_PlanField(&b,&o,&rules,&retail); assert(d.result == 2 && d.selection[9] == 6);
    b.back[0] = magic(999,20); b.enemy_lp = 8000; b.own[1].flags = HM_USED | HM_DEFENSE;
    /* Without attack planning either, the script's attacks are unknown. */
    o.attacks = 0;
    d = Hm_PlanField(&b,&o,&rules,&retail); assert(d.result == 3);
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
        /* Only a hidden card's position counts: whatever it hides (here
           stats, star, type and a trap's identity left in the record)
           changes no decision. */
        {
            HmBoard hidden = b, leaked; HmDecision x, y;
            hidden.back[0] = rnd() % 2 ? magic(343 + rnd() % 5, 20) : magic(338 + rnd() % 5, 20);
            hidden.lp = 1000;
            for (i = 0; i < 5; i++) {
                if (rnd() % 2) hidden.enemy[i] = (HmCard){-1,0,0,0,0,0,0,HM_DOWN | (rnd() % 2 ? HM_DEFENSE : 0),0,0};
                if (rnd() % 2) hidden.enemy_back[i] = (HmCard){-1,0,21,0,0,0,0,HM_DOWN,0,0};
            }
            leaked = hidden;
            for (i = 0; i < 5; i++) {
                if (leaked.enemy[i].id < 0) {
                    leaked.enemy[i].attack = rnd() % 4000; leaked.enemy[i].defense = rnd() % 4000;
                    leaked.enemy[i].star = 1 + rnd() % 10; leaked.enemy[i].type = rnd() % 20;
                    leaked.enemy[i].effect = rnd() % 700; leaked.enemy[i].worth = rnd() % 900;
                }
                if (leaked.enemy_back[i].id < 0) leaked.enemy_back[i].effect = 687 + rnd() % 2;
            }
            x = Hm_PlanField(&hidden,&o,&rules,0); y = Hm_PlanField(&leaked,&o,&rules,0);
            assert(x.result == y.result && !memcmp(x.selection, y.selection, 12));
            x = Hm_PlanHand(&hidden,&o,&rules); y = Hm_PlanHand(&leaked,&o,&rules);
            assert(x.result == y.result && !memcmp(x.selection, y.selection, 12));
        }
    }
}

/* The "combat" boosts each monster puts on the one it battles (the board's
 * edge and hit, which the game foresees): an attack they turn into a loss
 * is not made, and a monster they let the enemy beat goes to defense. */
static void combat_cases(void)
{
    HmBoard b = empty(); HmOptions o = opts(); HmDecision d;
    b.own[0] = monster(2, 1500, 500); b.own[0].zone = 1;
    b.enemy[0] = monster(20, 1000, 1000);
    d = Hm_PlanField(&b,&o,&rules,0);
    assert(d.result == 2 && d.selection[9] == 1 && d.selection[10] == 56);
    /* The enemy takes 800 ATK off what it battles, attacking or attacked. */
    b.edge[0][0][0] = b.edge[0][0][1] = -800;
    b.hit[0][0][0] = b.hit[0][0][1] = 800;
    d = Hm_PlanField(&b,&o,&rules,0);
    assert(d.result == 2 && d.selection[9] == 1 && !d.selection[10] && d.selection[11] == 1);
    /* Its own boost on the enemy in defense: a stronger wall is still beaten. */
    b = empty(); b.own[0] = monster(2, 1500, 500); b.own[0].zone = 1;
    b.enemy[0] = monster(20, 1000, 1800); b.enemy[0].flags = HM_DEFENSE;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[10] != 56);
    b.edge[0][0][1] = 400;
    d = Hm_PlanField(&b,&o,&rules,0); assert(d.selection[9] == 1 && d.selection[10] == 56);
    /* An equip on a field monster: the attack it would win counts its edge. */
    {
        HmDecision free_hit, cut_hit;
        o.spells = 0;
        b = empty(); b.own[0] = monster(2, 1500, 500); b.own[0].zone = 1;
        b.enemy[0] = monster(20, 2000, 1000);
        b.hand[0] = magic(657, 23); b.hand_count = 1;
        free_hit = Hm_PlanHand(&b,&o,&rules);
        b.edge[0][0][0] = -1000;
        cut_hit = Hm_PlanHand(&b,&o,&rules);
        assert(free_hit.result == 1 && cut_hit.result == 1 && cut_hit.score < free_hit.score);
    }
}

int main(void)
{
    tactical_cases(); spell_cases(); hand_cases(); retail_cases(); combat_cases(); bounded_fixtures();
    puts("AI hard mode: tactical, spell, fusion, combat and 500 bounded fixtures passed");
    return 0;
}
