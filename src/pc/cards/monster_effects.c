/* Monster effects: reading a card's "monster_effects" (monster_effects.h). */
#include "monster_effects.h"
#include "attributes.h"
#include "cards.h"
#include "pc/mods/json.h"
#include "pc/mods/mods.h"
#include "game/card_constants.h"
#include <stdio.h>
#include <string.h>

/* The group each effect id dispatches to (src/game/duel_effect_tables.c):
 * declared here rather than through its header, which brings the game's
 * 32-bit layouts the host tests do not build with. */
extern unsigned char gDuelEffect_abGroupByEffectId[];
#define GROUP_NONE 0        /* DUEL_EFFECT_GROUP_NONE */
#define GROUP_RITUAL 12     /* DUEL_EFFECT_GROUP_RITUAL */

const char *const MonsterEffect_WhenNames[MONSTER_WHEN_COUNT] = {"summon", "flip", "draw", "combat", "destroyed",
                                                                 "destroy_opponent", "face_up"};
const char *const MonsterEffect_DoNames[MONSTER_DO_COUNT] = {"magic", "boost", "heal", "damage", "destroy",
                                                             "battle_immunity", "piercing"};
const char *const MonsterEffect_TargetNames[MONSTER_TARGET_COUNT] = {"self", "own", "others", "opponent", "all",
                                                                     "battle"};
const char *const MonsterEffect_EachNames[MONSTER_EACH_COUNT] = {"", "own", "opponent", "all"};
const char *const MonsterEffect_CondNames[MONSTER_COND_COUNT] = {
    "none", "named_card", "stat_threshold", "lowest_atk", "highest_atk",
    "fewer_monsters", "more_monsters", "control_stat", "control_level",
    "control_type", "control_attribute", "field_spell", "lower_lp", "higher_lp"
};
const char *const MonsterEffect_FieldNames[MONSTER_FIELD_COUNT] = {
    "any", "forest", "wasteland", "mountain", "sogen", "umi", "yami"
};
const char *const MonsterEffect_BattleModNames[MONSTER_BATTLE_MOD_COUNT] = {
    "always", "first_attacked", "destroyed_after_2_attacks", "destroyed_after_3_attacks",
    "attacked_by_type", "attacked_by_attribute", "attacked_by_stat",
    "attacked_by_level", "attacked_by_monster"
};
#define MONSTER_PIERCING_MOD_COUNT 6
const char *const MonsterEffect_PiercingModNames[MONSTER_PIERCING_MOD_COUNT] = {
    "always", "attacks_type", "attacks_attribute", "attacks_stat",
    "attacks_level", "attacks_monster"
};
static const unsigned char s_piercing_mod_values[MONSTER_PIERCING_MOD_COUNT] = {
    MONSTER_PIERCING_MOD_ALWAYS,
    MONSTER_PIERCING_MOD_ATTACKS_TYPE,
    MONSTER_PIERCING_MOD_ATTACKS_ATTRIBUTE,
    MONSTER_PIERCING_MOD_ATTACKS_STAT,
    MONSTER_PIERCING_MOD_ATTACKS_LEVEL,
    MONSTER_PIERCING_MOD_ATTACKS_MONSTER
};
static const char *const s_control_presence_names[] = {"not_control", "control"};
static const char *const s_field_presence_names[] = {"not_active", "active"};
static const char *const s_scope_names[] = {"own", "other"};
static const char *const s_type_mode_names[] = {"at_least_one", "only"};
const char *const MonsterEffect_OpNames[MONSTER_OP_COUNT] = {"<", "<=", ">", ">=", "=="};
const char *const MonsterEffect_ResetNames[MONSTER_RESET_COUNT] = {"copy", "duel", "turn"};
const char *const MonsterEffect_StatNames[MONSTER_STAT_COUNT] = {"atk", "def"};
const char *const MonsterEffect_CondModeNames[MONSTER_COND_MODE_COUNT] = {"all", "any"};
#define AMOUNT_MAX 9999
#define BOOST_MAX 9999

int MonsterEffect_MagicUsable(int card)
{
    int index, group;
    if (Cards_RetailType(card) != CARD_TYPE_MAGIC) return 0;
    if ((unsigned)(card - DUEL_EFFECT_FIRST_BLOCK_CARD_ID) < DUEL_EFFECT_CARD_BLOCK_SIZE)
        index = card - DUEL_EFFECT_FIRST_BLOCK_CARD_ID;
    else if ((unsigned)(card - DUEL_EFFECT_SECOND_BLOCK_CARD_ID) < DUEL_EFFECT_CARD_BLOCK_SIZE)
        index = card - (DUEL_EFFECT_SECOND_BLOCK_CARD_ID - DUEL_EFFECT_CARD_BLOCK_SIZE);
    else
        return 0;
    group = gDuelEffect_abGroupByEffectId[index];
    return group != GROUP_NONE && group != GROUP_RITUAL;
}

int MonsterEffect_Allowed(int when, int action, int target)
{
    if (when == MONSTER_WHEN_FACE_UP) {
        return (action == MONSTER_DO_BOOST && target != MONSTER_TARGET_BATTLE) ||
               (action == MONSTER_DO_BATTLE_IMMUNITY && target == MONSTER_TARGET_SELF) ||
               (action == MONSTER_DO_PIERCING && target == MONSTER_TARGET_SELF);
    }
    if (action == MONSTER_DO_BATTLE_IMMUNITY || action == MONSTER_DO_PIERCING) return 0;
    if (when == MONSTER_WHEN_COMBAT)
        return action == MONSTER_DO_HEAL || action == MONSTER_DO_DAMAGE ||
               (action == MONSTER_DO_BOOST && (target == MONSTER_TARGET_SELF || target == MONSTER_TARGET_BATTLE));
    /* Only an attacked monster's flip has a monster it battles outside one. */
    if (target == MONSTER_TARGET_BATTLE && when != MONSTER_WHEN_FLIP) return 0;
    if (action == MONSTER_DO_DESTROY) return target == MONSTER_TARGET_OPPONENT || target == MONSTER_TARGET_BATTLE;
    if (action != MONSTER_DO_BOOST) return 1;
    /* A destroyed card has no self to boost. */
    return !(when == MONSTER_WHEN_DESTROYED && target == MONSTER_TARGET_SELF);
}

int MonsterEffect_EachAllowed(int action)
{
    return action == MONSTER_DO_BOOST || action == MONSTER_DO_HEAL || action == MONSTER_DO_DAMAGE;
}

int MonsterEffect_DefaultTarget(int when, int action)
{
    if (action == MONSTER_DO_BATTLE_IMMUNITY || action == MONSTER_DO_PIERCING) return MONSTER_TARGET_SELF;
    if (action == MONSTER_DO_DESTROY) return when == MONSTER_WHEN_FLIP ? MONSTER_TARGET_BATTLE : MONSTER_TARGET_OPPONENT;
    return when == MONSTER_WHEN_DESTROYED ? MONSTER_TARGET_OWN : MONSTER_TARGET_SELF;
}

static int named(const char *text, const char *const *names, int count)
{
    int i;
    if (!text) return -1;
    for (i = 0; i < count; i++) {
        const char *a = text, *b = names[i];
        /* "face up", "Face-Up" and "face_up" are all one. */
        while (*a && *b) {
            char x = (char)(*a == ' ' || *a == '-' ? '_' : *a), y = *b;
            if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
            if (x != y) break;
            a++, b++;
        }
        if (!*a && !*b) return i;
    }
    return -1;
}

static int number(const char *mod, int index, int n, const JsonValue *entry, const char *key, int limit, int *out)
{
    const JsonValue *value = Json_Member(entry, key);
    long v;
    if (!value) return 0;
    v = Json_Number(value, 0);
    if (Json_TypeOf(value) != JSON_NUMBER || v < -limit || v > limit) {
        Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"%s\" must be a whole number, %d to %d", index, n, key,
                  -limit, limit);
        return -1;
    }
    *out = (int)v;
    return 1;
}

/* An object's "type" and "attribute" (`where` names it in the notes):
 * 0 when one is wrong. */
static int filter(const char *mod, int index, int n, const char *where, const JsonValue *object, signed char *type,
                  signed short *attribute)
{
    const JsonValue *value = Json_Member(object, "type");
    int v;
    if (value) {
        long raw = Json_TypeOf(value) == JSON_NUMBER ? Json_Number(value, -1) : Cards_TypeNamed(Json_String(value, ""));
        if (raw < 0 || raw > 255 || !Cards_IsMonsterType((int)raw)) {
            Mods_Note(mod, "cards[%d]: monster_effects[%d]: %s\"type\" must be a monster type", index, n, where);
            return 0;
        }
        v = (int)raw;
        /* The filter is a byte whose 0xFF is "any" (MonsterEffect.type). */
        if (v == 0xFF) {
            Mods_Note(mod, "cards[%d]: monster_effects[%d]: %s\"type\" 255 cannot filter a monster effect", index,
                      n, where);
            return 0;
        }
        *type = (signed char)v;
    }
    value = Json_Member(object, "attribute");
    if (value) {
        v = Attributes_Value(value);
        if (v < 0) {
            Mods_Note(mod, "cards[%d]: monster_effects[%d]: %s\"attribute\" must be a retail or declared attribute",
                      index, n, where);
            return 0;
        }
        *attribute = (signed short)v;
    }
    return 1;
}

/* "for_each": what is counted. 0 when it is wrong. */
static int read_each(const char *mod, int index, int n, const JsonValue *each, int action, MonsterEffect *out)
{
    const JsonValue *whose = Json_Member(each, "whose");
    if (!MonsterEffect_EachAllowed(action)) {
        Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"for_each\" goes only with boost, heal or damage", index,
                  n);
        return 0;
    }
    if (Json_TypeOf(each) != JSON_OBJECT) {
        Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"for_each\" must be an object", index, n);
        return 0;
    }
    out->each = MONSTER_EACH_ALL;
    if (whose) {
        int v = named(Json_String(whose, NULL), MonsterEffect_EachNames + 1, MONSTER_EACH_COUNT - 1);
        if (v < 0) {
            Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"for_each\": \"whose\" must be own, opponent or all",
                      index, n);
            return 0;
        }
        out->each = (unsigned char)(v + 1);
    }
    return filter(mod, index, n, "\"for_each\": ", each, &out->each_type, &out->each_attribute);
}

static int read_one_condition(const char *mod, int index, int n, int c_idx, const JsonValue *entry,
                              MonsterCondition *cond)
{
    const JsonValue *val;
    int type, v;
    memset(cond, 0, sizeof(*cond));
    cond->presence = 1;
    cond->card_type = -1;
    cond->attribute = -1;
    if (Json_TypeOf(entry) != JSON_OBJECT) {
        Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d] is not an object", index, n, c_idx);
        return 0;
    }
    const char *type_str = Json_String(Json_Member(entry, "type"), NULL);
    type = named(type_str, MonsterEffect_CondNames, MONSTER_COND_COUNT);
    if (type <= MONSTER_COND_NONE || type >= MONSTER_COND_COUNT) {
        Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"type\" must be named_card, "
                  "stat_threshold, lowest_atk, highest_atk, fewer_monsters, more_monsters, "
                  "control_stat, control_level, control_type, control_attribute, "
                  "field_spell, lower_lp or higher_lp", index, n, c_idx);
        return 0;
    }
    cond->type = (unsigned char)type;
    switch (type) {
    case MONSTER_COND_NAMED_CARD:
        v = Cards_Reference(Json_Member(entry, "card"));
        if (v < 1 || v > CARD_COUNT) {
            Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"card\" must name a valid card",
                      index, n, c_idx);
            return 0;
        }
        cond->card = (unsigned short)v;
        val = Json_Member(entry, "whose");
        cond->whose = MONSTER_EACH_ALL;
        if (val) {
            v = named(Json_String(val, NULL), MonsterEffect_EachNames + 1, MONSTER_EACH_COUNT - 1);
            if (v < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"whose\" must be own, opponent or all",
                          index, n, c_idx);
                return 0;
            }
            cond->whose = (unsigned char)(v + 1);
        }
        break;
    case MONSTER_COND_STAT_THRESHOLD:
        val = Json_Member(entry, "stat");
        cond->stat = MONSTER_STAT_ATK;
        if (val) {
            v = named(Json_String(val, NULL), MonsterEffect_StatNames, MONSTER_STAT_COUNT);
            if (v < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"stat\" must be atk or def",
                          index, n, c_idx);
                return 0;
            }
            cond->stat = (unsigned char)v;
        }
        val = Json_Member(entry, "op");
        cond->op = MONSTER_OP_GE;
        if (val) {
            const char *op_str = Json_String(val, "");
            v = named(op_str, MonsterEffect_OpNames, MONSTER_OP_COUNT);
            if (v < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: invalid comparison \"%s\"",
                          index, n, c_idx, op_str);
                return 0;
            }
            cond->op = (unsigned char)v;
        }
        v = 0;
        if (number(mod, index, n, entry, "value", AMOUNT_MAX, &v) < 0) return 0;
        cond->value = (short)v;
        break;
    case MONSTER_COND_LOWEST_ATK:
    case MONSTER_COND_HIGHEST_ATK:
    case MONSTER_COND_FEWER_MONSTERS:
    case MONSTER_COND_MORE_MONSTERS:
        break;
    case MONSTER_COND_CONTROL_STAT: {
        val = Json_Member(entry, "presence");
        if (val) {
            v = named(Json_String(val, NULL), s_control_presence_names, 2);
            if (v < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"presence\" must be control or not_control",
                          index, n, c_idx);
                return 0;
            }
            cond->presence = (unsigned char)v;
        }
        val = Json_Member(entry, "scope");
        if (val) {
            v = named(Json_String(val, NULL), s_scope_names, 2);
            if (v < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"scope\" must be own or other",
                          index, n, c_idx);
                return 0;
            }
            cond->scope = (unsigned char)v;
        }
        val = Json_Member(entry, "stat");
        cond->stat = MONSTER_STAT_ATK;
        if (val) {
            v = named(Json_String(val, NULL), MonsterEffect_StatNames, MONSTER_STAT_COUNT);
            if (v < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"stat\" must be atk or def",
                          index, n, c_idx);
                return 0;
            }
            cond->stat = (unsigned char)v;
        }
        val = Json_Member(entry, "op");
        cond->op = MONSTER_OP_GE;
        if (val) {
            const char *op_str = Json_String(val, "");
            v = named(op_str, MonsterEffect_OpNames, MONSTER_OP_COUNT);
            if (v < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: invalid comparison \"%s\"",
                          index, n, c_idx, op_str);
                return 0;
            }
            cond->op = (unsigned char)v;
        }
        v = 0;
        if (number(mod, index, n, entry, "value", AMOUNT_MAX, &v) < 0) return 0;
        cond->value = (short)v;
        break;
    }
    case MONSTER_COND_CONTROL_LEVEL: {
        val = Json_Member(entry, "presence");
        if (val) {
            v = named(Json_String(val, NULL), s_control_presence_names, 2);
            if (v < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"presence\" must be control or not_control",
                          index, n, c_idx);
                return 0;
            }
            cond->presence = (unsigned char)v;
        }
        val = Json_Member(entry, "scope");
        if (val) {
            v = named(Json_String(val, NULL), s_scope_names, 2);
            if (v < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"scope\" must be own or other",
                          index, n, c_idx);
                return 0;
            }
            cond->scope = (unsigned char)v;
        }
        val = Json_Member(entry, "op");
        cond->op = MONSTER_OP_EQ;
        if (val) {
            const char *op_str = Json_String(val, "");
            v = named(op_str, MonsterEffect_OpNames, MONSTER_OP_COUNT);
            if (v < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: invalid comparison \"%s\"",
                          index, n, c_idx, op_str);
                return 0;
            }
            cond->op = (unsigned char)v;
        }
        val = Json_Member(entry, "level");
        if (!val || Json_TypeOf(val) != JSON_NUMBER) {
            Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"level\" must be 1 to 12",
                      index, n, c_idx);
            return 0;
        }
        v = (int)Json_Number(val, 0);
        if (v < 1 || v > 12) {
            Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"level\" must be 1 to 12",
                      index, n, c_idx);
            return 0;
        }
        cond->value = (short)v;
        break;
    }
    case MONSTER_COND_CONTROL_TYPE: {
        val = Json_Member(entry, "presence");
        if (val) {
            v = named(Json_String(val, NULL), s_control_presence_names, 2);
            if (v < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"presence\" must be control or not_control",
                          index, n, c_idx);
                return 0;
            }
            cond->presence = (unsigned char)v;
        }
        cond->whose = MONSTER_EACH_OWN;
        val = Json_Member(entry, "whose");
        if (val) {
            v = named(Json_String(val, NULL), MonsterEffect_EachNames + 1, MONSTER_EACH_COUNT - 1);
            if (v < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"whose\" must be own, opponent or all",
                          index, n, c_idx);
                return 0;
            }
            cond->whose = (unsigned char)(v + 1);
        }
        val = Json_Member(entry, "scope");
        if (val) {
            v = named(Json_String(val, NULL), s_scope_names, 2);
            if (v < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"scope\" must be own or other",
                          index, n, c_idx);
                return 0;
            }
            cond->scope = (unsigned char)v;
        }
        val = Json_Member(entry, "mode");
        cond->type_mode = MONSTER_COND_TYPE_AT_LEAST_ONE;
        if (val) {
            v = named(Json_String(val, NULL), s_type_mode_names, 2);
            if (v < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"mode\" must be at_least_one or only",
                          index, n, c_idx);
                return 0;
            }
            cond->type_mode = (unsigned char)v;
        }
        val = Json_Member(entry, "card_type");
        if (!val) {
            Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"card_type\" must be a monster type",
                      index, n, c_idx);
            return 0;
        }
        long raw = Json_TypeOf(val) == JSON_NUMBER ? Json_Number(val, -1) : Cards_TypeNamed(Json_String(val, ""));
        if (raw < 0 || raw > 255 || !Cards_IsMonsterType((int)raw)) {
            Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"card_type\" must be a monster type",
                      index, n, c_idx);
            return 0;
        }
        cond->card_type = (signed char)raw;
        break;
    }
    case MONSTER_COND_CONTROL_ATTRIBUTE: {
        val = Json_Member(entry, "presence");
        if (val) {
            v = named(Json_String(val, NULL), s_control_presence_names, 2);
            if (v < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"presence\" must be control or not_control",
                          index, n, c_idx);
                return 0;
            }
            cond->presence = (unsigned char)v;
        }
        cond->whose = MONSTER_EACH_OWN;
        val = Json_Member(entry, "whose");
        if (val) {
            v = named(Json_String(val, NULL), MonsterEffect_EachNames + 1, MONSTER_EACH_COUNT - 1);
            if (v < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"whose\" must be own, opponent or all",
                          index, n, c_idx);
                return 0;
            }
            cond->whose = (unsigned char)(v + 1);
        }
        val = Json_Member(entry, "scope");
        if (val) {
            v = named(Json_String(val, NULL), s_scope_names, 2);
            if (v < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"scope\" must be own or other",
                          index, n, c_idx);
                return 0;
            }
            cond->scope = (unsigned char)v;
        }
        val = Json_Member(entry, "mode");
        cond->type_mode = MONSTER_COND_TYPE_AT_LEAST_ONE;
        if (val) {
            v = named(Json_String(val, NULL), s_type_mode_names, 2);
            if (v < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"mode\" must be at_least_one or only",
                          index, n, c_idx);
                return 0;
            }
            cond->type_mode = (unsigned char)v;
        }
        val = Json_Member(entry, "attribute");
        if (!val) {
            Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"attribute\" must be a retail or declared attribute",
                      index, n, c_idx);
            return 0;
        }
        v = Attributes_Value(val);
        if (v < 0) {
            Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"attribute\" must be a retail or declared attribute",
                      index, n, c_idx);
            return 0;
        }
        cond->attribute = (signed short)v;
        break;
    }
    case MONSTER_COND_LOWER_LP:
    case MONSTER_COND_HIGHER_LP:
        break;
    case MONSTER_COND_FIELD_SPELL: {
        val = Json_Member(entry, "presence");
        if (val) {
            v = named(Json_String(val, NULL), s_field_presence_names, 2);
            if (v < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"presence\" must be active or not_active",
                          index, n, c_idx);
                return 0;
            }
            cond->presence = (unsigned char)v;
        }
        val = Json_Member(entry, "field");
        cond->field_spell = MONSTER_FIELD_ANY;
        if (val) {
            if (Json_TypeOf(val) == JSON_NUMBER) {
                int num = (int)Json_Number(val, 0);
                if (num >= 330 && num <= 335)
                    cond->field_spell = (unsigned char)(num - 329); /* 330->1 (Forest) .. 335->6 (Yami) */
                else if (num >= 1 && num <= 6)
                    cond->field_spell = (unsigned char)num;
                else if (num == 0)
                    cond->field_spell = MONSTER_FIELD_ANY;
                else {
                    Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"%s\": no field spell %d",
                              index, n, c_idx, Json_Name(val), num);
                    return 0;
                }
            } else if (Json_TypeOf(val) == JSON_STRING) {
                const char *s = Json_String(val, "any");
                int f = named(s, MonsterEffect_FieldNames, MONSTER_FIELD_COUNT);
                if (f >= 0) {
                    cond->field_spell = (unsigned char)f;
                } else {
                    Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"%s\": no field spell \"%s\"",
                              index, n, c_idx, Json_Name(val), s);
                    return 0;
                }
            } else {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: conditions[%d]: \"%s\": field spell must be a name or number",
                          index, n, c_idx, Json_Name(val));
                return 0;
            }
        }
        break;
    }
    }
    return 1;
}

static int read_conditions(const char *mod, int index, int n, const JsonValue *entry, MonsterEffectEx *out_ex)
{
    const JsonValue *conds = Json_Member(entry, "conditions");
    const JsonValue *mode = Json_Member(entry, "condition_mode");
    if (!out_ex) return 1;
    out_ex->cond_mode = MONSTER_COND_MODE_ALL;
    if (mode) {
        int m = named(Json_String(mode, NULL), MonsterEffect_CondModeNames, MONSTER_COND_MODE_COUNT);
        if (m < 0) {
            Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"condition_mode\" must be all or any", index, n);
            return 0;
        }
        out_ex->cond_mode = (unsigned char)m;
    }
    if (conds) {
        const JsonValue *item;
        int c_idx = 0;
        if (Json_TypeOf(conds) != JSON_ARRAY) {
            Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"conditions\" must be a list", index, n);
            return 0;
        }
        for (item = Json_At(conds, 0); item; item = Json_Next(item), c_idx++) {
            if (out_ex->cond_count >= MONSTER_CONDITIONS_MAX) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: at most %d conditions allowed", index, n,
                          MONSTER_CONDITIONS_MAX);
                break;
            }
            if (!read_one_condition(mod, index, n, c_idx, item, &out_ex->conditions[out_ex->cond_count])) return 0;
            out_ex->cond_count++;
        }
    }
    return 1;
}

static int read_one(const char *mod, int index, int n, const JsonValue *entry, MonsterEffect *out,
                    MonsterEffectEx *out_ex, int card_effect)
{
    const JsonValue *value;
    int when, action, target, v;
    memset(out, 0, sizeof(*out));
    out->type = out->attribute = out->each_type = out->each_attribute = -1;
    if (out_ex) {
        memset(out_ex, 0, sizeof(*out_ex));
        out_ex->attacker_type = -1;
        out_ex->attacker_attribute = -1;
        out_ex->attacker_stat_op = MONSTER_OP_GT;
        out_ex->attacker_level_op = MONSTER_OP_GT;
    }
    if (Json_TypeOf(entry) != JSON_OBJECT) {
        Mods_Note(mod, "cards[%d]: monster_effects[%d] is not an object", index, n);
        return 0;
    }
    when = card_effect ? MONSTER_WHEN_SUMMON
                       : named(Json_String(Json_Member(entry, "when"), NULL), MonsterEffect_WhenNames,
                               MONSTER_WHEN_COUNT);
    action = named(Json_String(Json_Member(entry, "do"), NULL), MonsterEffect_DoNames, MONSTER_DO_COUNT);
    if (when < 0) {
        Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"when\" must be summon, flip, draw, combat, destroyed, "
                  "destroy_opponent or face_up", index, n);
        return 0;
    }
    if (action < 0) {
        Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"do\" must be magic, boost, heal, damage, destroy, "
                  "battle_immunity or piercing", index, n);
        return 0;
    }
    value = Json_Member(entry, "target");
    target = value ? named(Json_String(value, NULL), MonsterEffect_TargetNames, MONSTER_TARGET_COUNT)
                   : card_effect ? ((action == MONSTER_DO_DAMAGE || action == MONSTER_DO_DESTROY)
                                        ? MONSTER_TARGET_OPPONENT : MONSTER_TARGET_OWN)
                                 : MonsterEffect_DefaultTarget(when, action);
    if (target < 0) {
        Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"target\" must be self, own, others, opponent, all or battle",
                  index, n);
        return 0;
    }
    if (!card_effect && !MonsterEffect_Allowed(when, action, target)) {
        Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"%s\" cannot be done on \"%s\"%s", index, n,
                  action == MONSTER_DO_BOOST || action == MONSTER_DO_DESTROY ? MonsterEffect_TargetNames[target]
                                                                             : MonsterEffect_DoNames[action],
                  MonsterEffect_WhenNames[when],
                  when == MONSTER_WHEN_COMBAT ? " (a boost of self or battle, heal or damage)"
                  : when == MONSTER_WHEN_FACE_UP ? " (boosts, battle immunity and piercing only)"
                  : action == MONSTER_DO_DESTROY ? " (destroy takes opponent, or battle on flip)" : "");
        return 0;
    }
    /* A spell or trap is not a monster on the field: there is no useful
     * "self", "others" or battle target.  Its owner is the active side
     * while it resolves, so own/opponent/all work just as they do for a
     * monster effect. */
    if (card_effect && (target == MONSTER_TARGET_SELF || target == MONSTER_TARGET_OTHERS ||
                        target == MONSTER_TARGET_BATTLE ||
                        ((action == MONSTER_DO_HEAL || action == MONSTER_DO_DAMAGE) &&
                         target == MONSTER_TARGET_ALL))) {
        Mods_Note(mod, "cards[%d]: card_effects[%d]: a spell or trap target must be own, opponent or all", index, n);
        return 0;
    }
    out->when = (unsigned char)when;
    out->action = (unsigned char)action;
    out->target = (unsigned char)target;
    switch (action) {
    case MONSTER_DO_MAGIC:
        v = Cards_Reference(Json_Member(entry, "card"));
        if (v < 1 || v > CARD_COUNT || !MonsterEffect_MagicUsable(v)) {
            Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"card\" must name a magic card of the disc with an "
                      "effect (not a ritual, equip or trap)", index, n);
            return 0;
        }
        out->card = (unsigned short)v;
        break;
    case MONSTER_DO_BOOST:
    case MONSTER_DO_DESTROY: {
        int attack = 0, defense = 0, a, d;
        if (action == MONSTER_DO_BOOST) {
            a = number(mod, index, n, entry, "attack", BOOST_MAX, &attack);
            d = number(mod, index, n, entry, "defense", BOOST_MAX, &defense);
            if (a < 0 || d < 0) return 0;
            if (!attack && !defense) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: a boost needs \"attack\" or \"defense\"", index, n);
                return 0;
            }
        }
        out->attack = (short)attack;
        out->defense = (short)defense;
        if (!filter(mod, index, n, "", entry, &out->type, &out->attribute)) return 0;
        break;
    }
    case MONSTER_DO_BATTLE_IMMUNITY:
    case MONSTER_DO_PIERCING: {
        const JsonValue *mod_val = Json_Member(entry, "modifier");
        const JsonValue *rst_val = Json_Member(entry, "reset");
        int battle_modifier = MONSTER_BATTLE_MOD_ALWAYS;
        int battle_reset = MONSTER_RESET_TURN;
        int attacker_type = -1;
        int attacker_attribute = -1;
        int attacker_stat = 0;
        int attacker_stat_op = MONSTER_OP_GT;
        int attacker_amount = 0;
        int attacker_level = 0;
        int attacker_level_op = MONSTER_OP_GT;
        const char *prefix = (action == MONSTER_DO_PIERCING) ? "defender_" : "attacker_";
        char key[32];

        if (mod_val) {
            const char *m_str = Json_String(mod_val, "");
            if (action == MONSTER_DO_BATTLE_IMMUNITY) {
                int m = named(m_str, MonsterEffect_BattleModNames, MONSTER_BATTLE_MOD_COUNT);
                if (m < 0) {
                    Mods_Note(mod, "cards[%d]: monster_effects[%d]: unknown battle immunity modifier \"%s\"", index, n,
                              m_str);
                    return 0;
                }
                battle_modifier = m;
            } else {
                int m = named(m_str, MonsterEffect_PiercingModNames, MONSTER_PIERCING_MOD_COUNT);
                if (m < 0) {
                    Mods_Note(mod, "cards[%d]: monster_effects[%d]: unknown piercing modifier \"%s\"", index, n,
                              m_str);
                    return 0;
                }
                battle_modifier = s_piercing_mod_values[m];
            }
        }
        if (rst_val) {
            const char *r_str = Json_String(rst_val, "");
            int r = named(r_str, MonsterEffect_ResetNames, MONSTER_RESET_COUNT);
            if (r < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"reset\" must be copy, duel or turn", index, n);
                return 0;
            }
            battle_reset = r;
        }

        /* Target criteria (attacker_* for battle immunity, defender_* for piercing) */
        snprintf(key, sizeof(key), "%stype", prefix);
        const JsonValue *t_val = Json_Member(entry, key);
        if (t_val) {
            long raw = Json_TypeOf(t_val) == JSON_NUMBER ? (long)Json_Number(t_val, -1)
                                                         : Cards_TypeNamed(Json_String(t_val, ""));
            if (raw >= 0 && raw <= 255 && Cards_IsMonsterType((int)raw)) {
                attacker_type = (int)raw;
            } else {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"%s\" must be a monster type", index, n, key);
                return 0;
            }
        }

        snprintf(key, sizeof(key), "%sattribute", prefix);
        const JsonValue *a_val = Json_Member(entry, key);
        if (a_val) {
            int av = Attributes_Value(a_val);
            if (av >= 0) {
                attacker_attribute = av;
            } else {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"%s\" must be an attribute", index, n, key);
                return 0;
            }
        }

        snprintf(key, sizeof(key), "%sstat", prefix);
        const JsonValue *s_val = Json_Member(entry, key);
        if (s_val) {
            int s = named(Json_String(s_val, NULL), MonsterEffect_StatNames, MONSTER_STAT_COUNT);
            if (s < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"%s\" must be atk or def", index, n, key);
                return 0;
            }
            attacker_stat = (unsigned char)(s + 1); /* 1: ATK, 2: DEF */
        }

        snprintf(key, sizeof(key), "%sstat_op", prefix);
        const JsonValue *sop_val = Json_Member(entry, key);
        if (sop_val) {
            const char *op_str = Json_String(sop_val, "");
            int op_idx = named(op_str, MonsterEffect_OpNames, MONSTER_OP_COUNT);
            if (op_idx < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"%s\": invalid comparison \"%s\"", index, n, key, op_str);
                return 0;
            }
            attacker_stat_op = (unsigned char)op_idx;
        }

        snprintf(key, sizeof(key), "%samount", prefix);
        const JsonValue *am_val = Json_Member(entry, key);
        if (am_val && Json_TypeOf(am_val) == JSON_NUMBER) {
            int amt = (int)Json_Number(am_val, 0);
            if (amt >= 0 && amt <= AMOUNT_MAX) {
                attacker_amount = (short)amt;
                if (attacker_stat == 0) attacker_stat = 1; /* default to ATK */
            }
        }

        snprintf(key, sizeof(key), "%slevel_op", prefix);
        const JsonValue *lop_val = Json_Member(entry, key);
        if (lop_val) {
            const char *op_str = Json_String(lop_val, "");
            int op_idx = named(op_str, MonsterEffect_OpNames, MONSTER_OP_COUNT);
            if (op_idx < 0) {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"%s\": invalid comparison \"%s\"", index, n, key, op_str);
                return 0;
            }
            attacker_level_op = (unsigned char)op_idx;
        }

        snprintf(key, sizeof(key), "%slevel", prefix);
        const JsonValue *l_val = Json_Member(entry, key);
        if (l_val && Json_TypeOf(l_val) == JSON_NUMBER) {
            int lvl = (int)Json_Number(l_val, 0);
            if (lvl >= 1 && lvl <= 12) {
                attacker_level = lvl;
            } else {
                Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"%s\" must be 1 to 12", index, n, key);
                return 0;
            }
        }
        if (out_ex) {
            out_ex->battle_modifier = (unsigned char)battle_modifier;
            out_ex->battle_reset = (unsigned char)battle_reset;
            out_ex->attacker_type = (signed char)attacker_type;
            out_ex->attacker_attribute = (signed short)attacker_attribute;
            out_ex->attacker_stat = (unsigned char)attacker_stat;
            out_ex->attacker_stat_op = (unsigned char)attacker_stat_op;
            out_ex->attacker_amount = (short)attacker_amount;
            out_ex->attacker_level = (unsigned char)attacker_level;
            out_ex->attacker_level_op = (unsigned char)attacker_level_op;
        }
        break;
    }
    default:
        v = 0;
        if (number(mod, index, n, entry, "amount", AMOUNT_MAX, &v) < 0) return 0;
        if (v <= 0) {
            Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"amount\" must be 1 to %d", index, n, AMOUNT_MAX);
            return 0;
        }
        out->amount = v;
        break;
    }
    value = Json_Member(entry, "for_each");
    if (value && !read_each(mod, index, n, value, action, out)) return 0;
    if (!read_conditions(mod, index, n, entry, out_ex)) return 0;
    return 1;
}

int MonsterEffects_ReadEx(const char *mod, int index, const JsonValue *list, MonsterEffect *out,
                          MonsterEffectEx *out_ex)
{
    const JsonValue *entry;
    int n, count = 0;
    if (!list) return -1;
    if (Json_TypeOf(list) == JSON_NULL) return 0;
    if (Json_TypeOf(list) != JSON_ARRAY) {
        Mods_Note(mod, "cards[%d]: \"monster_effects\" must be a list", index);
        return -1;
    }
    for (n = 0, entry = Json_At(list, 0); entry; n++, entry = Json_Next(entry)) {
        if (count == MONSTER_EFFECTS_MAX) {
            Mods_Note(mod, "cards[%d]: at most %d monster_effects; the rest are left out", index, MONSTER_EFFECTS_MAX);
            break;
        }
        count += read_one(mod, index, n, entry, out + count, out_ex ? out_ex + count : NULL, 0);
    }
    return count;
}

int MonsterEffects_Read(const char *mod, int index, const JsonValue *list, MonsterEffect *out)
{
    return MonsterEffects_ReadEx(mod, index, list, out, NULL);
}

int CardEffects_ReadEx(const char *mod, int index, const JsonValue *list, MonsterEffect *out,
                       MonsterEffectEx *out_ex)
{
    const JsonValue *entry;
    int n, count = 0;
    if (!list) return -1;
    if (Json_TypeOf(list) == JSON_NULL) return 0;
    if (Json_TypeOf(list) != JSON_ARRAY) {
        Mods_Note(mod, "cards[%d]: \"card_effects\" must be a list", index);
        return -1;
    }
    for (n = 0, entry = Json_At(list, 0); entry; n++, entry = Json_Next(entry)) {
        if (count == MONSTER_EFFECTS_MAX) {
            Mods_Note(mod, "cards[%d]: at most %d card_effects; the rest are left out", index, MONSTER_EFFECTS_MAX);
            break;
        }
        count += read_one(mod, index, n, entry, out + count, out_ex ? out_ex + count : NULL, 1);
    }
    return count;
}

int CardEffects_Read(const char *mod, int index, const JsonValue *list, MonsterEffect *out)
{
    return CardEffects_ReadEx(mod, index, list, out, NULL);
}
