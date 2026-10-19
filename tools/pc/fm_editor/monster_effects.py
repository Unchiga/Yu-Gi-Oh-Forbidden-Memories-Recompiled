"""Monster effects (notes/more-cards.md, "Monster effects"): what a card's
"monster_effects" make a monster do on the field, as the game reads them
(src/pc/cards/monster_effects.c). The list lives in the card's mod.json
entry, so the editor keeps it there (card_extra, or an added card's extra)."""
from __future__ import annotations

from .gamedata import ATTRIBUTE_NAMES, TYPE_NAMES, TYPE_MAGIC, is_monster_type, CARD_ID_LIMIT

# The game's names, in the order of its enums, and the editor's for them.
WHEN = ("summon", "flip", "draw", "combat", "destroyed", "destroy_opponent", "face_up")
WHEN_LABELS = ("On summon", "On flip", "On draw phase", "Before combat", "When destroyed", "Destroy opponent monster",
               "While face up")
WHEN_HINTS = (
    "Put on the field face up, once: played, fused or a ritual's monster. A face-down play is no summon (the CPU "
    "puts its monsters down face down).",
    "Attacked while face down, once: as in Yu-Gi-Oh!, the battle goes on as usual and the flip resolves after the "
    "damage, even if the battle destroyed it (not when a trap stops the attack). Attacking face down or a reveal "
    "(Dark-piercing Light) is no flip.",
    "At the start of its owner's every turn, once the hand is drawn, while it is face up.",
    "It attacks or is attacked, before the damage (no trap sprang). Boosts last the battle.",
    "Destroyed by a battle or an effect (not when used for a fusion or a ritual).",
    "It won a battle that destroyed the other monster, and is still on the field: after the battle.",
    "All the while it is face up on the field: a boost that goes when it does.",
)
DO = ("magic", "boost", "heal", "damage", "destroy", "battle_immunity")
DO_LABELS = ("Magic card effect", "Boost ATK/DEF", "Heal its owner's LP", "Damage the opponent's LP",
             "Destroy monsters", "Cannot be destroyed by battle")
TARGET = ("self", "own", "others", "opponent", "all", "battle")
TARGET_LABELS = ("This card", "Its owner's monsters", "Its owner's other monsters", "The opponent's monsters",
                 "Every monster", "The monster it battles")
# Whose: the boosts' and destroys' choices for each "when" (allowed()).
FILTERED = ("boost", "destroy")
# "for_each": whose face-up monsters are counted, the number made that many
# times (monster_effects.c MonsterEffect_EachAllowed: a boost, heal or damage).
EACH = ("own", "opponent", "all")
EACH_LABELS = ("On its owner's field", "On the opponent's field", "On the whole field")
EACH_FIELDS = ("its owner's field", "the opponent's field", "the field")
EACH_DO = ("boost", "heal", "damage")
NO_EACH = "\u2014"

# Conditions (API 19): requirements under which a while face-up effect applies.
CONDITIONS = (
    "named_card",
    "stat_threshold",
    "lowest_atk",
    "highest_atk",
    "fewer_monsters",
    "more_monsters",
    "control_stat",
    "control_level",
    "control_type",
    "control_attribute",
    "field_spell",
)
CONDITION_LABELS = (
    "Named card on field",
    "Compare ATK/DEF against a number",
    "Has the lowest ATK on field",
    "Has the highest ATK on field",
    "If you control fewer monsters than opponent",
    "If you control more monsters than your opponent",
    "Control monster with ATK/DEF",
    "Control monster with Level",
    "Control monster with Type",
    "Control monster with Attribute",
    "Field spell active or not active",
)
CONDITION_HINTS = (
    "A specific card is face up on the field (own, opponent, or whole field).",
    "This card's current effective ATK or DEF compared against a value (<, <=, >, >=).",
    "This card has strictly lower ATK than every other face-up monster on the field (ties disqualify).",
    "This card has strictly higher ATK than every other face-up monster on the field (ties disqualify).",
    "Active only while you control strictly fewer monsters than your opponent (having 0 monsters counts; ties disqualify).",
    "Active only while you control strictly more monsters than your opponent (opponent having 0 monsters counts; ties disqualify).",
    "Control (or do not control) a monster with ATK or DEF compared against a value.",
    "Control (or do not control) a monster with a specific Level, or higher/fewer Level than a number.",
    "Control (or do not control) a monster of a specific Type (at least one, or only control that type).",
    "Control (or do not control) a monster of a specific Attribute.",
    "A field spell is active, or a specific field spell is active or not active (Forest, Wasteland, Mountain, Sogen, Umi, Yami).",
)
RESET = ("turn", "copy", "duel")
RESET_LABELS = ("In a turn", "This card copy", "In a duel")
OPS = ("<", "<=", ">", ">=", "==")
OP_LABELS = ("Lower than (<)", "Lower or equal (<=)", "Higher than (>)", "Higher or equal (>=)", "Equal to (=)")
STATS = ("atk", "def")
STAT_LABELS = ("ATK", "DEF")
COND_MODES = ("all", "any")
COND_MODE_LABELS = ("All conditions must be met (AND)", "Any condition can be met (OR)")
MAX_CONDITIONS = 4
PRESENCE = ("control", "not_control")
PRESENCE_LABELS = ("You control", "You do not control")
SCOPE = ("own", "other")
SCOPE_LABELS = ("Any monster you control", "Another monster you control (exclude self)")
TYPE_MODES = ("at_least_one", "only")
TYPE_MODE_LABELS = ("At least one monster of this type", "Only control this type")
FIELD_SPELLS = ("any", "forest", "wasteland", "mountain", "sogen", "umi", "yami")
FIELD_SPELL_LABELS = (
    "Any field spell",
    "Forest",
    "Wasteland",
    "Mountain",
    "Sogen (Meadow)",
    "Umi (Sea)",
    "Yami (Dark)",
)
FIELD_STATUS = ("active", "not_active")
FIELD_STATUS_LABELS = ("Is active", "Is not active")

ATTACK_CONDITIONS = (
    "first_attacked",
    "destroyed_after_2_attacks",
    "destroyed_after_3_attacks",
    "second_attacked",
    "third_attacked",
    "destroyed_after_two_attacks",
    "destroyed_after_three_attacks",
    "can_only_be_destroyed_after_two_attacks",
    "can_only_be_destroyed_after_three_attacks",
    "after_one_attack",
    "destroyed_after_1_attack",
)

MODIFIERS = ("always", "first_attacked", "destroyed_after_2_attacks", "destroyed_after_3_attacks")
MODIFIER_LABELS = (
    "Always",
    "The first time it is attacked",
    "Can only be destroyed after two attacks",
    "Can only be destroyed after three attacks",
)

# The disc's magic cards whose effect a monster may use: Magic cards whose
# effect group does something at play time and asks nothing (no ritual).
MAGIC = (320, 329, 330, 331, 332, 333, 334, 335, 336, 337, 338, 339, 340, 341, 342, 343, 344, 345, 346, 347, 348,
         349, 350, 653, 655, 656, 660, 661, 662, 663, 664, 669, 672)
AMOUNT_MAX = 9999
BOOST_MAX = 9999
MAX_EFFECTS = 8


def allowed(when: str, do: str, target: str | None = None) -> bool:
    """What the game takes (monster_effects.c MonsterEffect_Allowed)."""
    if when == "face_up":
        return (do == "boost" and target != "battle") or (do == "battle_immunity" and (target is None or target == "self"))
    if do == "battle_immunity":
        return False
    if when == "combat":
        return do in ("heal", "damage") or (do == "boost" and target in ("self", "battle"))
    if target == "battle" and when != "flip":
        return False
    if do == "destroy":
        return target in ("opponent", "battle")
    if do != "boost":
        return True
    return not (when == "destroyed" and target == "self")


def actions(when: str) -> list:
    return [do for do in DO if targets(when, do)]


def targets(when: str, do: str = "boost") -> list:
    return [t for t in TARGET if allowed(when, do, t)]


def default_target(when: str, do: str = "boost") -> str:
    if do == "battle_immunity":
        return "self"
    if do == "destroy":
        return "battle" if when == "flip" else "opponent"
    return "own" if when == "destroyed" else "self"


def _letters_index(text: str, names) -> int | None:
    """The index of the name `text` is in letters and digits, any case."""
    key = "".join(c.lower() for c in text if c.isascii() and c.isalnum())
    return next((i for i, name in enumerate(names)
                 if "".join(c.lower() for c in name if c.isalnum()) == key and key), None)


def _name(names, value) -> str | None:
    """A game name for `value` as the game reads it ("Face Up" is "face_up")."""
    if not isinstance(value, str):
        return None
    # No trimming: monster_effects.c named() takes "summon " as no name.
    key = value.lower().replace(" ", "_").replace("-", "_")
    return key if key in names else None


def normalize_condition(cond: dict, resolve=None) -> dict | None:
    """A condition entry with its names in game spelling, or None if invalid."""
    if not isinstance(cond, dict):
        return None
    raw_type = cond.get("type")
    if raw_type in ("fewer_monsters_than_opponent", "fewer_monsters_than_opp", "control_fewer_monsters"):
        raw_type = "fewer_monsters"
    elif raw_type in ("more_monsters_than_opponent", "more_monsters_than_opp", "control_more_monsters"):
        raw_type = "more_monsters"
    elif raw_type in ("control_monster_stat", "monster_stat", "not_control_stat", "not_control_monster_stat"):
        raw_type = "control_stat"
    elif raw_type in ("control_monster_level", "monster_level", "not_control_level", "not_control_monster_level"):
        raw_type = "control_level"
    elif raw_type in ("control_monster_type", "monster_type", "not_control_type", "not_control_monster_type"):
        raw_type = "control_type"
    elif raw_type in ("control_monster_attribute", "monster_attribute", "not_control_attribute", "not_control_monster_attribute"):
        raw_type = "control_attribute"
    elif raw_type in ("field_spell", "field", "terrain", "field_magic", "not_field_spell", "not_field", "not_terrain"):
        raw_type = "field_spell"
    c_type = _name(CONDITIONS, raw_type)
    if not c_type:
        return None
    out = {"type": c_type}
    if c_type in ("control_stat", "control_level", "control_type", "control_attribute"):
        is_not = str(cond.get("type", "")).startswith("not_") or cond.get("presence") in ("not_control", "not", "none", "no", "do_not_control")
        if is_not:
            out["presence"] = "not_control"
        if cond.get("scope") in ("other", "others", "exclude_self"):
            out["scope"] = "other"
    if c_type == "named_card":
        card = cond.get("card")
        card = resolve(card) if resolve and card is not None else card
        if not isinstance(card, int) or isinstance(card, bool) or not (1 <= card <= CARD_ID_LIMIT):
            return None
        out["card"] = card
        if "whose" in cond:
            whose = _name(EACH, cond["whose"])
            if whose is None:
                return None
            out["whose"] = whose
    elif c_type == "stat_threshold":
        stat = cond.get("stat", "atk")
        if isinstance(stat, str):
            s = stat.lower()
            if s in ("def", "defense"):
                stat = "def"
            elif s in ("atk", "attack"):
                stat = "atk"
            else:
                return None
        else:
            return None
        out["stat"] = stat

        op = cond.get("comparison") or cond.get("op") or ">="
        if isinstance(op, str):
            op = op.lower()
            if op in ("<", "lower", "lt"):
                op = "<"
            elif op in ("<=", "lower_equal", "le"):
                op = "<="
            elif op in (">", "higher", "gt"):
                op = ">"
            elif op in (">=", "higher_equal", "ge"):
                op = ">="
            elif op in ("==", "=", "equal", "eq"):
                op = "=="
            else:
                return None
        else:
            return None
        out["comparison"] = op

        val = cond.get("value", 0)
        if not isinstance(val, int) or isinstance(val, bool) or not (0 <= val <= AMOUNT_MAX):
            return None
        out["value"] = val
    elif c_type == "control_stat":
        stat = cond.get("stat", "atk")
        if isinstance(stat, str):
            s = stat.lower()
            if s in ("def", "defense"):
                stat = "def"
            elif s in ("atk", "attack"):
                stat = "atk"
            else:
                return None
        else:
            return None
        out["stat"] = stat

        op = cond.get("comparison") or cond.get("op") or ">="
        if isinstance(op, str):
            op = op.lower()
            if op in ("<", "lower", "lt"):
                op = "<"
            elif op in ("<=", "lower_equal", "le"):
                op = "<="
            elif op in (">", "higher", "gt"):
                op = ">"
            elif op in (">=", "higher_equal", "ge"):
                op = ">="
            elif op in ("==", "=", "equal", "eq"):
                op = "=="
            else:
                return None
        else:
            return None
        out["comparison"] = op

        val = cond.get("value", 0)
        if not isinstance(val, int) or isinstance(val, bool) or not (0 <= val <= AMOUNT_MAX):
            return None
        out["value"] = val
    elif c_type == "control_level":
        op = cond.get("comparison") or cond.get("op") or "=="
        if isinstance(op, str):
            op = op.lower()
            if op in ("==", "=", "equal", "eq"):
                op = "=="
            elif op in ("<", "lower", "lt", "fewer"):
                op = "<"
            elif op in ("<=", "lower_equal", "le", "fewer_equal"):
                op = "<="
            elif op in (">", "higher", "gt"):
                op = ">"
            elif op in (">=", "higher_equal", "ge"):
                op = ">="
            else:
                return None
        else:
            return None
        out["comparison"] = op

        val = cond.get("level") if "level" in cond else cond.get("value", 1)
        if not isinstance(val, int) or isinstance(val, bool) or not (1 <= val <= 12):
            return None
        out["level"] = val
    elif c_type == "control_type":
        mode = cond.get("mode") or cond.get("type_mode")
        if mode in ("only", "only_control"):
            out["mode"] = "only"
        t = cond.get("card_type") or cond.get("monster_type")
        if not t and cond.get("type") not in ("control_type", "not_control_type", "control_monster_type", "not_control_monster_type"):
            t = cond.get("type")
        if t is not None:
            out["card_type"] = str(t)
    elif c_type == "control_attribute":
        attr = cond.get("attribute")
        if attr is not None:
            out["attribute"] = str(attr)
    elif c_type == "field_spell":
        fld = cond.get("field") or cond.get("field_spell") or cond.get("terrain") or cond.get("card") or "any"
        if isinstance(fld, int) and not isinstance(fld, bool):
            if 330 <= fld <= 335:
                fld = FIELD_SPELLS[fld - 330 + 1]
            elif 0 <= fld < len(FIELD_SPELLS):
                fld = FIELD_SPELLS[fld]
            else:
                return None
        elif isinstance(fld, str):
            fld_lower = fld.lower()
            if fld_lower in ("sea", "ocean"):
                fld = "umi"
            elif fld_lower in ("meadow", "grassland"):
                fld = "sogen"
            elif fld_lower in ("dark", "darkness"):
                fld = "yami"
            else:
                fld_val = _name(FIELD_SPELLS, fld_lower)
                if fld_val is None:
                    return None
                fld = fld_val
        else:
            return None
        out["field"] = fld

        status = cond.get("presence") or cond.get("status") or cond.get("state")
        is_not = str(cond.get("type", "")).startswith("not_") or status in ("not_active", "inactive", "not", "none", "no", "not_control", "false", False)
        if is_not:
            out["presence"] = "not_active"
        else:
            out["presence"] = "active"
    elif c_type in ("lowest_atk", "highest_atk", "fewer_monsters", "more_monsters"):
        pass
    return out


def normalize(effect: dict, resolve=None, attribute_resolve=None, type_names=None) -> dict | None:
    """The entry with its names in the game's spelling, or None when the
    game would leave it out. `resolve` turns a card reference into an id;
    `type_names` is the mod's type bank (Project.type_names), for its own
    monster types."""
    if not isinstance(effect, dict):
        return None
    when, do = _name(WHEN, effect.get("when")), _name(DO, effect.get("do"))
    target = _name(TARGET, effect.get("target")) if "target" in effect else (default_target(when, do) if when else None)
    if not when or not do or not target or not allowed(when, do, target):
        return None
    out = {"when": when, "do": do}
    if do == "magic":
        card = effect.get("card")
        card = resolve(card) if resolve and card is not None else card
        if card not in MAGIC:
            return None
        out["card"] = card
    elif do == "battle_immunity":
        out["target"] = target
        mod = effect.get("modifier") or effect.get("battle_modifier")
        if mod:
            if mod in ("first_time_attacked", "after_one_attack", "destroyed_after_1_attack"):
                mod = "first_attacked"
            elif mod in ("second_attacked", "destroyed_after_two_attacks", "after_two_attacks", "can_only_be_destroyed_after_two_attacks"):
                mod = "destroyed_after_2_attacks"
            elif mod in ("third_attacked", "destroyed_after_three_attacks", "after_three_attacks", "can_only_be_destroyed_after_three_attacks"):
                mod = "destroyed_after_3_attacks"
            mod_name = _name(MODIFIERS, mod)
            if mod_name is None:
                return None
            if mod_name != "always":
                out["modifier"] = mod_name
                rst = effect.get("reset") or effect.get("modifier_reset") or effect.get("battle_reset") or "turn"
                if rst in ("summon", "this_copy", "this_card_copy"):
                    rst = "copy"
                rst_name = _name(RESET, rst)
                if rst_name is None:
                    return None
                out["reset"] = rst_name
    elif do in FILTERED:
        out["target"] = target
        for key in ("attack", "defense") if do == "boost" else ():
            value = effect.get(key, 0)
            if not isinstance(value, int) or isinstance(value, bool) or abs(value) > BOOST_MAX:
                return None
            if value:
                out[key] = value
        if do == "boost" and "attack" not in out and "defense" not in out:
            return None
        if not _filter(effect, out, attribute_resolve, type_names):
            return None
    else:
        amount = effect.get("amount")
        if not isinstance(amount, int) or isinstance(amount, bool) or not 0 < amount <= AMOUNT_MAX:
            return None
        out["amount"] = amount
    if "for_each" in effect:
        each = effect["for_each"]
        if do not in EACH_DO or not isinstance(each, dict):
            return None
        whose = _name(EACH, each["whose"]) if "whose" in each else "all"
        if whose is None:
            return None
        out["for_each"] = {"whose": whose}
        if not _filter(each, out["for_each"], attribute_resolve, type_names):
            return None

    if "condition_mode" in effect:
        m = _name(COND_MODES, effect["condition_mode"])
        if m is None:
            return None
        out["condition_mode"] = m

    if "conditions" in effect:
        raw_conds = effect["conditions"]
        if not isinstance(raw_conds, list):
            return None
        norm_conds = []
        for c in raw_conds[:MAX_CONDITIONS]:
            nc = normalize_condition(c, resolve)
            if nc is None:
                return None
            norm_conds.append(nc)
        if norm_conds:
            out["conditions"] = norm_conds
    elif "condition" in effect:
        nc = normalize_condition(effect["condition"], resolve)
        if nc is None:
            return None
        out["conditions"] = [nc]

    return out


def _filter(source: dict, out: dict, attribute_resolve=None, type_names=None) -> bool:
    """`source`'s "type" and "attribute" into `out` by name; False when the
    game would not take one (monster_effects.c filter())."""
    names = type_names or TYPE_NAMES
    # Names in any case and spacing, as cards.c same_letters() takes them;
    # a renamed retail type keeps its disc spelling too.
    if "type" in source:
        t = source["type"]
        if isinstance(t, str):
            found = _letters_index(t, [name or "" for name in names])
            t = found if found is not None else _letters_index(t, TYPE_NAMES)
        # 255 is the game's "any" byte (MonsterEffect.type), so no filter.
        if not isinstance(t, int) or isinstance(t, bool) or not is_monster_type(t) or t == 0xFF:
            return False
        if t >= TYPE_MAGIC and not (t < len(names) and names[t]):
            return False
        out["type"] = names[t] if t < len(names) and names[t] else TYPE_NAMES[t]
    if "attribute" in source:
        a = source["attribute"]
        a = attribute_resolve(a) if attribute_resolve else (_letters_index(a, ATTRIBUTE_NAMES) if isinstance(a, str) else a)
        if type(a) is not int or not 0 <= a <= 255 or a in (6, 7) or \
                (attribute_resolve is None and a >= len(ATTRIBUTE_NAMES)):
            return False
        out["attribute"] = ATTRIBUTE_NAMES[a] if a < len(ATTRIBUTE_NAMES) else a
    return True


def effect_has_attack_condition(effect: dict) -> bool:
    """True if effect has a battle immunity attack modifier or (legacy) attack condition."""
    if not isinstance(effect, dict):
        return False
    if effect.get("do") == "battle_immunity":
        mod = effect.get("modifier") or effect.get("battle_modifier")
        if mod and mod != "always":
            return True
    conds = effect.get("conditions")
    if not conds and "condition" in effect:
        conds = [effect["condition"]]
    if isinstance(conds, list):
        return any(isinstance(c, dict) and c.get("type") in ATTACK_CONDITIONS for c in conds)
    return False


def monster_has_attack_condition(effects: list, exclude_index: int | None = None) -> bool:
    """True if any effect other than exclude_index has an attack condition or modifier."""
    if not isinstance(effects, list):
        return False
    for i, e in enumerate(effects):
        if exclude_index is not None and i == exclude_index:
            continue
        if effect_has_attack_condition(e):
            return True
    return False


def count_monster_attack_conditions(effects: list) -> int:
    """Total count of attack conditions and modifiers across all effects of a monster."""
    if not isinstance(effects, list):
        return 0
    total = 0
    for e in effects:
        if not isinstance(e, dict):
            continue
        if e.get("do") == "battle_immunity":
            mod = e.get("modifier") or e.get("battle_modifier")
            if mod and mod != "always":
                total += 1
        conds = e.get("conditions")
        if not conds and "condition" in e:
            conds = [e["condition"]]
        if isinstance(conds, list):
            for c in conds:
                if isinstance(c, dict) and c.get("type") in ATTACK_CONDITIONS:
                    total += 1
    return total


def problems(effects, resolve=None, attribute_resolve=None, type_names=None) -> list:
    """What the game would say of a card's "monster_effects"."""
    if effects is None:
        return []
    if not isinstance(effects, list):
        return ['"monster_effects" must be a list']
    # As the game reads them (monster_effects.c MonsterEffects_Read): one
    # it leaves out takes no place, and past the MAX_EFFECTS it takes the
    # rest are left out.
    out, taken = [], 0
    for n, effect in enumerate(effects):
        if taken == MAX_EFFECTS:
            out.append(f"at most {MAX_EFFECTS} monster effects; the rest (from effect {n + 1}) are left out")
            break
        if normalize(effect, resolve, attribute_resolve, type_names) is None:
            out.append(f"monster effect {n + 1} is not one the game takes ({describe_raw(effect)})")
        else:
            taken += 1
    if count_monster_attack_conditions(effects) > 1:
        out.append("A monster can only have one condition related to a number of attacks.")
    return out


def describe_raw(effect) -> str:
    return ", ".join(f"{k}: {v}" for k, v in effect.items()) if isinstance(effect, dict) else repr(effect)


def when_label(when: str) -> str:
    return WHEN_LABELS[WHEN.index(when)] if when in WHEN else str(when)


def condition_words(cond: dict, card_name=lambda cid: f"#{cid}") -> str:
    """A short description of a condition."""
    if not isinstance(cond, dict):
        return ""
    c_type = cond.get("type")
    if c_type == "named_card":
        card = cond.get("card")
        cname = card_name(card) if card else "Card"
        whose = cond.get("whose", "all")
        where = EACH_FIELDS[EACH.index(whose)] if whose in EACH else whose
        return f"{cname} is on {where}"
    if c_type == "stat_threshold":
        stat = str(cond.get("stat", "atk")).upper()
        op = cond.get("comparison") or cond.get("op", ">=")
        val = cond.get("value", 0)
        return f"{stat} {op} {val}"
    if c_type == "lowest_atk":
        return "lowest ATK on field"
    if c_type == "highest_atk":
        return "highest ATK on field"
    if c_type == "fewer_monsters":
        return "you control fewer monsters than opponent"
    if c_type == "more_monsters":
        return "you control more monsters than your opponent"
    if c_type == "control_stat":
        p = "you do not control" if cond.get("presence") == "not_control" else "you control"
        sc = " another" if cond.get("scope") == "other" else " a"
        stat = str(cond.get("stat", "atk")).upper()
        op = cond.get("comparison") or cond.get("op", ">=")
        val = cond.get("value", 0)
        return f"{p}{sc} monster with {stat} {op} {val}"
    if c_type == "control_level":
        p = "you do not control" if cond.get("presence") == "not_control" else "you control"
        sc = " another" if cond.get("scope") == "other" else " a"
        op = cond.get("comparison") or cond.get("op", "==")
        val = cond.get("level") if "level" in cond else cond.get("value", 1)
        if op in ("==", "="):
            return f"{p}{sc} Level {val} monster"
        return f"{p}{sc} Level {op} {val} monster"
    if c_type == "control_type":
        p = cond.get("presence") == "not_control"
        sc = " other" if cond.get("scope") == "other" else ""
        t_name = str(cond.get("card_type") or cond.get("monster_type") or (cond.get("type") if cond.get("type") not in CONDITIONS else "Dragon"))
        if cond.get("mode") == "only":
            return f"you do not only control{sc} {t_name} monsters" if p else f"you only control{sc} {t_name} monsters"
        else:
            return f"you do not control{sc} a {t_name} monster" if p else f"you control{sc} a {t_name} monster"
    if c_type == "control_attribute":
        p = "you do not control" if cond.get("presence") == "not_control" else "you control"
        sc = " another" if cond.get("scope") == "other" else " a"
        attr = str(cond.get("attribute", "LIGHT")).upper()
        return f"{p}{sc} {attr} monster"
    if c_type == "field_spell":
        fld = cond.get("field", "any")
        is_active = cond.get("presence", "active") != "not_active"
        if fld in FIELD_SPELLS:
            idx = FIELD_SPELLS.index(fld)
            fld_label = FIELD_SPELL_LABELS[idx]
        else:
            fld_label = str(fld)
        if fld == "any":
            return "a field spell is active" if is_active else "no field spell is active"
        return f"{fld_label} is active" if is_active else f"{fld_label} is not active"
    return str(c_type)


def conditions_words(effect: dict, card_name=lambda cid: f"#{cid}") -> str:
    """" (if ...)" string for conditions on an effect, or ""."""
    conds = effect.get("conditions")
    if not conds and "condition" in effect:
        conds = [effect["condition"]]
    if not conds:
        return ""
    mode = effect.get("condition_mode", "all")
    connector = " and " if mode == "all" else " or "
    texts = [condition_words(c, card_name) for c in conds if isinstance(c, dict)]
    texts = [t for t in texts if t]
    if not texts:
        return ""
    return f" (if {connector.join(texts)})"


def describe(effect: dict, card_name=lambda cid: f"#{cid}") -> str:
    """What the effect does, in a line: "Raigeki", "Its owner's other
    Dragon monsters +500 ATK", "Heal its owner 800 LP", "This card: +300
    ATK for each face-up Dragon on the field"."""
    do = effect.get("do")
    each = for_each_words(effect.get("for_each"))
    conds = conditions_words(effect, card_name)
    if do == "battle_immunity":
        mod = effect.get("modifier", "always")
        if mod != "always":
            reset = effect.get("reset", "turn")
            if reset == "summon":
                reset = "copy"
            scope_str = "this turn" if reset == "turn" else ("this duel" if reset == "duel" else "this card copy")
            if mod == "first_attacked":
                return f"Cannot be destroyed by battle the first time attacked ({scope_str}){conds}"
            elif mod == "destroyed_after_2_attacks":
                return f"Cannot be destroyed by battle after two attacks ({scope_str}){conds}"
            elif mod == "destroyed_after_3_attacks":
                return f"Cannot be destroyed by battle after three attacks ({scope_str}){conds}"
        return f"Cannot be destroyed by battle{conds}"
    if do == "magic":
        return f"{card_name(effect.get('card'))} (its effect){conds}"
    if do == "heal":
        return f"Its owner gains {effect.get('amount')} LP{each}{conds}"
    if do == "damage":
        return f"The opponent loses {effect.get('amount')} LP{each}{conds}"
    target = effect.get("target", default_target(effect.get("when"), do))
    who = TARGET_LABELS[TARGET.index(target)] if target in TARGET else str(target)
    which = " ".join(str(effect[k]) for k in ("attribute", "type") if k in effect)
    if which:
        who += f" ({which} only)"
    if do == "destroy":
        return f"Destroy {who[0].lower()}{who[1:]}{conds}"
    stats = " ".join(f"{effect[k]:+d} {label}" for k, label in (("attack", "ATK"), ("defense", "DEF"))
                     if effect.get(k))
    lasting = " for the battle" if effect.get("when") == "combat" else ""
    return f"{who}: {stats}{each}{lasting}{conds}"


def for_each_words(each) -> str:
    """" for each face-up Light Dragon on the field", or "" without one."""
    if not isinstance(each, dict):
        return ""
    which = " ".join(str(each[k]) for k in ("attribute", "type") if k in each)
    if "type" not in each:
        which = f"{which} monster" if which else "monster"
    whose = each.get("whose", "all")
    field = EACH_FIELDS[EACH.index(whose)] if whose in EACH else str(whose)
    return f" for each face-up {which} on {field}"

