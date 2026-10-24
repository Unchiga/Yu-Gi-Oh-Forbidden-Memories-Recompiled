#ifndef MEMORIES_PC_MONSTER_EFFECTS_H
#define MEMORIES_PC_MONSTER_EFFECTS_H
/* Monster effects (notes/more-cards.md, "Monster effects"): what a card's
 * "monster_effects" in a mod's cards[] makes a monster do on the field.
 *
 *     "monster_effects": [
 *         { "when": "summon", "do": "magic", "card": "Raigeki" },
 *         { "when": "face_up", "do": "boost", "target": "others", "type": "Dragon", "attack": 300 },
 *         { "when": "combat", "do": "boost", "target": "battle", "attack": -500, "defense": -500 },
 *         { "when": "face_up", "do": "boost", "attack": 300, "for_each": { "whose": "own", "type": "Dragon" } },
 *         { "when": "destroyed", "do": "damage", "amount": 500 }
 *     ]
 *
 * The cards' lists are read with the cards (cards.c); the duel side is
 * monster_effects_duel.c. */
struct JsonValue;

enum {
    MONSTER_WHEN_SUMMON,     /* put on the field face up: played, a fusion's or a ritual's monster */
    MONSTER_WHEN_FLIP,       /* attacked while face down: after the battle's damage, destroyed or not */
    MONSTER_WHEN_DRAW,       /* the start of its owner's turn, once the card is drawn */
    MONSTER_WHEN_COMBAT,     /* it attacks or is attacked, before the damage */
    MONSTER_WHEN_DESTROYED,  /* by a battle or an effect; not as fusion material or a ritual's tribute */
    MONSTER_WHEN_DESTROY_OPPONENT, /* it won a battle that destroyed the other monster, and is still there */
    MONSTER_WHEN_FACE_UP,    /* all the while it is face up on the field */
    MONSTER_WHEN_COUNT
};
enum {
    MONSTER_DO_MAGIC,        /* a retail magic card's effect, as its owner played it */
    MONSTER_DO_BOOST,        /* ATK and DEF up or down */
    MONSTER_DO_HEAL,         /* its owner's LP up */
    MONSTER_DO_DAMAGE,       /* the other side's LP down */
    MONSTER_DO_DESTROY,      /* the other side's monsters (or the one it battles) destroyed, as Warrior
                              * Elimination destroys */
    MONSTER_DO_CHANGE_FIELD, /* make the selected declared field active */
    /* The three post-damage battle outcomes offered by TEA Phoenix's
     * Battlefield Demolisher.  They are deliberately separate from
     * DESTROY: that action removes field records before a battle, whereas
     * these alter its already-calculated survivor flags. */
    MONSTER_DO_BATTLE_SURVIVE,
    MONSTER_DO_BATTLE_DESTROY_BOTH_EXCEPT,
    MONSTER_DO_BATTLE_DESTROY_BOTH,
    MONSTER_DO_PIERCING,     /* excess ATK against a defending monster damages its owner */
    MONSTER_DO_MULTIPLE_ATTACKS, /* `amount` attacks total each turn */
    MONSTER_DO_COUNT
};
enum {
    MONSTER_TARGET_SELF,     /* the card itself */
    MONSTER_TARGET_OWN,      /* its owner's monsters, itself too */
    MONSTER_TARGET_OTHERS,   /* its owner's other monsters */
    MONSTER_TARGET_OPPONENT, /* the other side's monsters */
    MONSTER_TARGET_ALL,      /* every monster on the field */
    MONSTER_TARGET_BATTLE,   /* "combat" and "flip": the monster it battles (that attacks it) */
    MONSTER_TARGET_COUNT
};
/* "for_each": whose face-up monsters are counted, the amount or boost
 * made that many times. */
enum {
    MONSTER_EACH_NONE,
    MONSTER_EACH_OWN,        /* its owner's, itself too */
    MONSTER_EACH_OPPONENT,   /* the other side's */
    MONSTER_EACH_ALL,        /* both sides' */
    MONSTER_EACH_COUNT
};
#define MONSTER_EFFECTS_MAX 8

typedef struct MonsterEffect {
    unsigned char when, action, target;
    signed char type;                /* a boost's or destroy's type filter: -1 any; read as an
                                        unsigned char, so a custom type 128-254 is stored negative */
    signed short attribute;          /* attribute-bank id, -1 any */
    unsigned short card;             /* MONSTER_DO_MAGIC: retail magic; MONSTER_DO_CHANGE_FIELD: field id */
    short attack, defense;           /* MONSTER_DO_BOOST */
    int amount;                      /* MONSTER_DO_HEAL, MONSTER_DO_DAMAGE */
    unsigned char each;              /* MONSTER_EACH_*: a boost, heal or damage made once per monster counted */
    signed char each_type;
    signed short each_attribute;     /* what is counted: -1 any */
} MonsterEffect;

/* A compact view for decision makers which need to predict a combat without
 * owning the effect list.  The amount returned below is the total attacks a
 * monster may make in its owner's turn (one when it has no such effect). */
enum {
    MONSTER_COMBAT_BATTLE_IMMUNE = 1 << 0,
    MONSTER_COMBAT_DESTROY_NON_IMMUNE = 1 << 1,
    MONSTER_COMBAT_DESTROY_BOTH = 1 << 2,
    MONSTER_COMBAT_PIERCING = 1 << 3
};
unsigned MonsterEffects_CombatFlags(int card);
int MonsterEffects_CombatAttackLimit(int card);

extern const char *const MonsterEffect_WhenNames[MONSTER_WHEN_COUNT];
extern const char *const MonsterEffect_DoNames[MONSTER_DO_COUNT];
extern const char *const MonsterEffect_TargetNames[MONSTER_TARGET_COUNT];
extern const char *const MonsterEffect_EachNames[MONSTER_EACH_COUNT];

/* Whether `card` is a retail magic card whose effect a monster may use: a
 * Magic card of the disc with play-time work that asks the player nothing
 * (not a ritual, an equip or a trap). */
int MonsterEffect_MagicUsable(int card);
/* Whether `action` (and `target`) can go with `when`. */
int MonsterEffect_Allowed(int when, int action, int target);
/* Whether `action` may have a "for_each": a boost, heal or damage (a
 * number to multiply), on any "when" that takes it. */
int MonsterEffect_EachAllowed(int action);
/* The target an entry without one has. */
int MonsterEffect_DefaultTarget(int when, int action);

/* A card entry's "monster_effects": the list read into out (up to
 * MONSTER_EFFECTS_MAX), its length returned; -1 when the entry has none.
 * What is wrong is noted for the mod and left out. */
int MonsterEffects_Read(const char *mod, int index, const struct JsonValue *list, MonsterEffect *out);

/* The actions available to a spell or trap card's "card_effects" are the
 * same as a monster effect's actions, but they happen when the card is
 * played (or springs), rather than at one of a monster's field events. */
int CardEffects_Read(const char *mod, int index, const struct JsonValue *list, MonsterEffect *out);

/* A card's effects (cards.c): the list and its length, 0 for none. */
int Cards_MonsterEffects(int id, const MonsterEffect **effects);

#endif
