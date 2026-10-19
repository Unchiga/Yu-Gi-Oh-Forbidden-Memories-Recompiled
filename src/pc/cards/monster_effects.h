#ifndef MEMORIES_PC_MONSTER_EFFECTS_H
#define MEMORIES_PC_MONSTER_EFFECTS_H
/* Monster effects (notes/more-cards.md, "Monster effects"): what a card's
 * "monster_effects" in a mod's cards[] makes a monster do on the field.
 *
 *     "monster_effects": [
 *         { "when": "summon", "do": "magic", "card": "Raigeki" },
 *         { "when": "face_up", "do": "boost", "target": "others", "type":
 * "Dragon", "attack": 300 }, { "when": "combat", "do": "boost", "target":
 * "battle", "attack": -500, "defense": -500 }, { "when": "face_up", "do":
 * "boost", "attack": 300, "for_each": { "whose": "own", "type": "Dragon" } },
 *         { "when": "destroyed", "do": "damage", "amount": 500 }
 *     ]
 *
 * The cards' lists are read with the cards (cards.c); the duel side is
 * monster_effects_duel.c. */
struct JsonValue;

enum {
  MONSTER_WHEN_SUMMON, /* put on the field face up: played, a fusion's or a
                          ritual's monster */
  MONSTER_WHEN_FLIP,   /* attacked while face down: after the battle's damage,
                          destroyed or not */
  MONSTER_WHEN_DRAW, /* the start of its owner's turn, once the card is drawn */
  MONSTER_WHEN_COMBAT,    /* it attacks or is attacked, before the damage */
  MONSTER_WHEN_DESTROYED, /* by a battle or an effect; not as fusion material or
                             a ritual's tribute */
  MONSTER_WHEN_DESTROY_OPPONENT, /* it won a battle that destroyed the other
                                    monster, and is still there */
  MONSTER_WHEN_FACE_UP,          /* all the while it is face up on the field */
  MONSTER_WHEN_COUNT
};
enum {
  MONSTER_DO_MAGIC,   /* a retail magic card's effect, as its owner played it */
  MONSTER_DO_BOOST,   /* ATK and DEF up or down */
  MONSTER_DO_HEAL,    /* its owner's LP up */
  MONSTER_DO_DAMAGE,  /* the other side's LP down */
  MONSTER_DO_DESTROY, /* the other side's monsters (or the one it battles)
                       * destroyed, as Warrior Elimination destroys */
  MONSTER_DO_BATTLE_IMMUNITY, /* cannot be destroyed by battle (API 19) */
  MONSTER_DO_COUNT
};
enum {
  MONSTER_TARGET_SELF,     /* the card itself */
  MONSTER_TARGET_OWN,      /* its owner's monsters, itself too */
  MONSTER_TARGET_OTHERS,   /* its owner's other monsters */
  MONSTER_TARGET_OPPONENT, /* the other side's monsters */
  MONSTER_TARGET_ALL,      /* every monster on the field */
  MONSTER_TARGET_BATTLE,   /* "combat" and "flip": the monster it battles (that
                              attacks it) */
  MONSTER_TARGET_COUNT
};
/* "for_each": whose face-up monsters are counted, the amount or boost
 * made that many times. */
enum {
  MONSTER_EACH_NONE,
  MONSTER_EACH_OWN,      /* its owner's, itself too */
  MONSTER_EACH_OPPONENT, /* the other side's */
  MONSTER_EACH_ALL,      /* both sides' */
  MONSTER_EACH_COUNT
};

/* Conditions (API 19): requirements under which a while face-up effect applies.
 */
enum {
  MONSTER_COND_NONE,
  MONSTER_COND_NAMED_CARD,     /* named card is face up on the field */
  MONSTER_COND_STAT_THRESHOLD, /* this card's current ATK or DEF compared
                                  against a value */
  MONSTER_COND_LOWEST_ATK,  /* this card has the strictly lowest ATK on field */
  MONSTER_COND_HIGHEST_ATK, /* this card has the strictly highest ATK on field
                             */
  MONSTER_COND_FEWER_MONSTERS, /* you control strictly fewer monsters than opponent */
  MONSTER_COND_MORE_MONSTERS,  /* you control strictly more monsters than opponent */
  MONSTER_COND_CONTROL_STAT,   /* control (or not) a monster with (more/less ATK or DEF) */
  MONSTER_COND_CONTROL_LEVEL,  /* control (or not) a monster with Level (specific / higher / fewer) */
  MONSTER_COND_CONTROL_TYPE,   /* control (or not) a monster with Type (at least one or only) */
  MONSTER_COND_CONTROL_ATTRIBUTE, /* control (or not) a monster with Attribute */
  MONSTER_COND_FIELD_SPELL,    /* field spell is active or specific field spell is not active */
  MONSTER_COND_COUNT
};

#define MONSTER_COND_FEWER_MONSTERS_THAN_OPPONENT MONSTER_COND_FEWER_MONSTERS
#define MONSTER_COND_MORE_MONSTERS_THAN_OPPONENT MONSTER_COND_MORE_MONSTERS
#define MONSTER_COND_CONTROL_MONSTER_STAT MONSTER_COND_CONTROL_STAT
#define MONSTER_COND_CONTROL_MONSTER_LEVEL MONSTER_COND_CONTROL_LEVEL
#define MONSTER_COND_CONTROL_MONSTER_TYPE MONSTER_COND_CONTROL_TYPE
#define MONSTER_COND_CONTROL_MONSTER_ATTRIBUTE MONSTER_COND_CONTROL_ATTRIBUTE
#define MONSTER_COND_FIELD MONSTER_COND_FIELD_SPELL
#define MONSTER_COND_TERRAIN MONSTER_COND_FIELD_SPELL

/* Field spell types for MONSTER_COND_FIELD_SPELL (API 19) */
enum {
  MONSTER_FIELD_ANY,       /* 0: Any field spell is active */
  MONSTER_FIELD_FOREST,    /* 1: Forest (card 330) */
  MONSTER_FIELD_WASTELAND, /* 2: Wasteland (card 331) */
  MONSTER_FIELD_MOUNTAIN,  /* 3: Mountain (card 332) */
  MONSTER_FIELD_SOGEN,     /* 4: Sogen / Meadow (card 333) */
  MONSTER_FIELD_UMI,       /* 5: Umi / Sea (card 334) */
  MONSTER_FIELD_YAMI,      /* 6: Yami / Dark (card 335) */
  MONSTER_FIELD_COUNT
};

/* Battle immunity modifiers (API 19): specific to MONSTER_DO_BATTLE_IMMUNITY */
enum {
  MONSTER_BATTLE_MOD_ALWAYS,          /* always immune to battle destruction (default) */
  MONSTER_BATTLE_MOD_FIRST_ATTACKED,  /* the first time it is attacked */
  MONSTER_BATTLE_MOD_AFTER_2_ATTACKS, /* can only be destroyed after two attacks */
  MONSTER_BATTLE_MOD_AFTER_3_ATTACKS, /* can only be destroyed after three attacks */
  MONSTER_BATTLE_MOD_COUNT
};

#define MONSTER_BATTLE_MOD_DESTROYED_AFTER_2_ATTACKS MONSTER_BATTLE_MOD_AFTER_2_ATTACKS
#define MONSTER_BATTLE_MOD_DESTROYED_AFTER_3_ATTACKS MONSTER_BATTLE_MOD_AFTER_3_ATTACKS
#define MONSTER_BATTLE_MOD_SECOND_ATTACKED MONSTER_BATTLE_MOD_AFTER_2_ATTACKS
#define MONSTER_BATTLE_MOD_THIRD_ATTACKED MONSTER_BATTLE_MOD_AFTER_3_ATTACKS

enum {
  MONSTER_OP_LT, /* < strictly lower */
  MONSTER_OP_LE, /* <= lower or equal */
  MONSTER_OP_GT, /* > strictly higher */
  MONSTER_OP_GE, /* >= higher or equal */
  MONSTER_OP_EQ, /* == equal to */
  MONSTER_OP_COUNT
};
enum {
  MONSTER_RESET_COPY, /* this card copy (resets when placed/summoned on field)
                       */
  MONSTER_RESET_SUMMON = MONSTER_RESET_COPY,
  MONSTER_RESET_DUEL, /* once per duel */
  MONSTER_RESET_TURN, /* resets each turn */
  MONSTER_RESET_COUNT
};
enum {
  MONSTER_STAT_ATK,
  MONSTER_STAT_DEF,
  MONSTER_STAT_COUNT
};
enum {
  MONSTER_COND_MODE_ALL, /* all conditions must be met (logical AND) */
  MONSTER_COND_MODE_ANY, /* at least one condition must be met (logical OR) */
  MONSTER_COND_MODE_COUNT
};

enum {
  MONSTER_COND_TYPE_AT_LEAST_ONE, /* at least one monster of type (default) */
  MONSTER_COND_TYPE_ONLY          /* only control this type */
};

#define MONSTER_CONDITIONS_MAX 4

typedef struct MonsterCondition {
  unsigned char type;  /* MONSTER_COND_* */
  unsigned char whose; /* MONSTER_EACH_*: own, opponent, all (for named card) */
  unsigned short card; /* named card id */
  unsigned char stat;  /* MONSTER_STAT_ATK or MONSTER_STAT_DEF */
  unsigned char op;    /* MONSTER_OP_* */
  short value;         /* stat threshold number or level */
  unsigned char presence; /* 1 = control / active (default), 0 = do not control / not active */
  signed char card_type;  /* monster type: -1 any, or type id */
  signed short attribute; /* attribute id: -1 any */
  unsigned char type_mode; /* 0 = at least one (default), 1 = only control this type */
  unsigned char scope;    /* 0 = all own (default), 1 = other monsters (exclude self) */
  unsigned char field_spell; /* MONSTER_FIELD_*: 0 any, 1 Forest ... 6 Yami */
} MonsterCondition;

#define MONSTER_EFFECTS_MAX 8

typedef struct MonsterEffect {
  unsigned char when, action, target;
  signed char
      type; /* a boost's or destroy's type filter: -1 any; read as an
               unsigned char, so a custom type 128-254 is stored negative */
  signed short attribute; /* attribute-bank id, -1 any */
  unsigned short card;    /* MONSTER_DO_MAGIC: the retail magic card */
  short attack, defense;  /* MONSTER_DO_BOOST */
  int amount;             /* MONSTER_DO_HEAL, MONSTER_DO_DAMAGE */
  unsigned char each; /* MONSTER_EACH_*: a boost, heal or damage made once per
                         monster counted */
  signed char each_type;
  signed short each_attribute; /* what is counted: -1 any */
  /* API 19: battle immunity modifiers (only when action == MONSTER_DO_BATTLE_IMMUNITY) */
  unsigned char battle_modifier; /* MONSTER_BATTLE_MOD_* */
  unsigned char battle_reset;    /* MONSTER_RESET_* */
  /* API 19: conditions */
  unsigned char cond_count;
  unsigned char cond_mode; /* MONSTER_COND_MODE_ALL or MONSTER_COND_MODE_ANY */
  MonsterCondition conditions[MONSTER_CONDITIONS_MAX];
} MonsterEffect;

extern const char *const MonsterEffect_WhenNames[MONSTER_WHEN_COUNT];
extern const char *const MonsterEffect_DoNames[MONSTER_DO_COUNT];
extern const char *const MonsterEffect_TargetNames[MONSTER_TARGET_COUNT];
extern const char *const MonsterEffect_EachNames[MONSTER_EACH_COUNT];
extern const char *const MonsterEffect_CondNames[MONSTER_COND_COUNT];
extern const char *const MonsterEffect_BattleModNames[MONSTER_BATTLE_MOD_COUNT];
extern const char *const MonsterEffect_OpNames[MONSTER_OP_COUNT];
extern const char *const MonsterEffect_ResetNames[MONSTER_RESET_COUNT];
extern const char *const MonsterEffect_StatNames[MONSTER_STAT_COUNT];
extern const char *const MonsterEffect_CondModeNames[MONSTER_COND_MODE_COUNT];
extern const char *const MonsterEffect_FieldNames[MONSTER_FIELD_COUNT];

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
int MonsterEffects_Read(const char *mod, int index,
                        const struct JsonValue *list, MonsterEffect *out);

/* The actions available to a spell or trap card's "card_effects" are the
 * same as a monster effect's actions, but they happen when the card is
 * played (or springs), rather than at one of a monster's field events. */
int CardEffects_Read(const char *mod, int index, const struct JsonValue *list,
                     MonsterEffect *out);

/* A card's effects (cards.c): the list and its length, 0 for none. */
int Cards_MonsterEffects(int id, const MonsterEffect **effects);

#endif
