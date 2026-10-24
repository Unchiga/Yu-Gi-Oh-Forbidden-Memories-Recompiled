/* Monster effects: reading a card's "monster_effects" (monster_effects.h). */
#include "monster_effects.h"
#include "attributes.h"
#include "cards.h"
#include "fields.h"
#include "pc/mods/json.h"
#include "pc/mods/mods.h"
#include "game/card_constants.h"
#include <string.h>

/* The group each effect id dispatches to (src/game/duel_effect_tables.c):
 * declared here rather than through its header, which brings the game's
 * 32-bit layouts the host tests do not build with. */
extern unsigned char gDuelEffect_abGroupByEffectId[];
#define GROUP_NONE 0        /* DUEL_EFFECT_GROUP_NONE */
#define GROUP_RITUAL 12     /* DUEL_EFFECT_GROUP_RITUAL */

const char *const MonsterEffect_WhenNames[MONSTER_WHEN_COUNT] = {"summon", "flip", "draw", "combat", "destroyed",
                                                                 "destroy_opponent", "face_up"};
const char *const MonsterEffect_DoNames[MONSTER_DO_COUNT] = {
    "magic", "boost", "heal", "damage", "destroy", "change_field",
    "cannot_be_destroyed_by_battle", "destroy_both_non_battle_immune", "destroy_both", "piercing", "multiple_attacks"
};
const char *const MonsterEffect_TargetNames[MONSTER_TARGET_COUNT] = {"self", "own", "others", "opponent", "all",
                                                                     "battle"};
const char *const MonsterEffect_EachNames[MONSTER_EACH_COUNT] = {"", "own", "opponent", "all"};
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
    if (action == MONSTER_DO_BATTLE_SURVIVE || action == MONSTER_DO_BATTLE_DESTROY_BOTH_EXCEPT ||
        action == MONSTER_DO_BATTLE_DESTROY_BOTH || action == MONSTER_DO_PIERCING ||
        action == MONSTER_DO_MULTIPLE_ATTACKS)
        return when == MONSTER_WHEN_COMBAT && target == MONSTER_TARGET_SELF;
    if (action == MONSTER_DO_CHANGE_FIELD)
        return when != MONSTER_WHEN_FACE_UP && when != MONSTER_WHEN_COMBAT;
    if (when == MONSTER_WHEN_FACE_UP)
        return action == MONSTER_DO_BOOST && target != MONSTER_TARGET_BATTLE;
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

static int read_one(const char *mod, int index, int n, const JsonValue *entry, MonsterEffect *out, int card_effect)
{
    const JsonValue *value;
    int when, action, target, v;
    memset(out, 0, sizeof(*out));
    out->type = out->attribute = out->each_type = out->each_attribute = -1;
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
        Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"do\" must be magic, boost, heal, damage, destroy, change_field or a battle outcome", index,
                  n);
        return 0;
    }
    value = Json_Member(entry, "target");
    target = action == MONSTER_DO_CHANGE_FIELD ? MONSTER_TARGET_SELF
             : value ? named(Json_String(value, NULL), MonsterEffect_TargetNames, MONSTER_TARGET_COUNT)
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
                  : when == MONSTER_WHEN_FACE_UP ? " (boosts only)"
                  : action == MONSTER_DO_DESTROY ? " (destroy takes opponent, or battle on flip)" : "");
        return 0;
    }
    if (card_effect && (action == MONSTER_DO_BATTLE_SURVIVE || action == MONSTER_DO_BATTLE_DESTROY_BOTH_EXCEPT ||
                        action == MONSTER_DO_BATTLE_DESTROY_BOTH || action == MONSTER_DO_PIERCING ||
                        action == MONSTER_DO_MULTIPLE_ATTACKS)) {
        Mods_Note(mod, "cards[%d]: card_effects[%d]: a battle outcome belongs to a monster's combat", index, n);
        return 0;
    }
    /* A spell or trap is not a monster on the field: there is no useful
     * "self", "others" or battle target.  Its owner is the active side
     * while it resolves, so own/opponent/all work just as they do for a
     * monster effect. */
    if (card_effect && action != MONSTER_DO_CHANGE_FIELD && (target == MONSTER_TARGET_SELF || target == MONSTER_TARGET_OTHERS ||
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
    case MONSTER_DO_CHANGE_FIELD:
        v = Fields_Value(Json_Member(entry, "field"));
        if (v < 1) {
            Mods_Note(mod, "cards[%d]: monster_effects[%d]: \"field\" must name a declared field", index, n);
            return 0;
        }
        out->card = (unsigned short)v;
        break;
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
    case MONSTER_DO_BATTLE_SURVIVE:
    case MONSTER_DO_BATTLE_DESTROY_BOTH_EXCEPT:
    case MONSTER_DO_BATTLE_DESTROY_BOTH:
    case MONSTER_DO_PIERCING:
        break;
    case MONSTER_DO_MULTIPLE_ATTACKS:
        v = 0;
        if (number(mod, index, n, entry, "amount", 8, &v) < 0 || v < 2) {
            Mods_Note(mod, "cards[%d]: monster_effects[%d]: multiple_attacks needs an \"amount\" from 2 to 8", index, n);
            return 0;
        }
        out->amount = v;
        break;
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
    return 1;
}

int MonsterEffects_Read(const char *mod, int index, const JsonValue *list, MonsterEffect *out)
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
        count += read_one(mod, index, n, entry, out + count, 0);
    }
    return count;
}

int CardEffects_Read(const char *mod, int index, const JsonValue *list, MonsterEffect *out)
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
        count += read_one(mod, index, n, entry, out + count, 1);
    }
    return count;
}
