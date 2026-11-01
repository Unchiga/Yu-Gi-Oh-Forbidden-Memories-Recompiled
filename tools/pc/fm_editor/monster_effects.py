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
DO = ("magic", "boost", "heal", "damage", "destroy", "change_field", "battle_immunity", "piercing")
DO_LABELS = ("Magic card effect", "Boost ATK/DEF", "Heal its owner's LP", "Damage the opponent's LP",
             "Destroy monsters", "Change field", "Cannot be destroyed by battle", "Piercing damage")
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
    "lower_lp",
    "higher_lp",
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
    "If player's LP lower than opponent's",
    "If player's LP higher than opponent's",
    "Control monster with ATK/DEF",
    "Control monster with Level",
    "Monster Type present on field",
    "Monster Attribute present on field",
    "Field spell active or not active",
)
CONDITION_HINTS = (
    "A specific card is face up on the field (own, opponent, or whole field).",
    "This card's current effective ATK or DEF compared against a value (<, <=, >, >=).",
    "This card has strictly lower ATK than every other face-up monster on the field (ties disqualify).",
    "This card has strictly higher ATK than every other face-up monster on the field (ties disqualify).",
    "Active only while you control strictly fewer monsters than your opponent (having 0 monsters counts; ties disqualify).",
    "Active only while you control strictly more monsters than your opponent (opponent having 0 monsters counts; ties disqualify).",
    "Active only while your Life Points are strictly lower than your opponent's (ties disqualify).",
    "Active only while your Life Points are strictly higher than your opponent's (ties disqualify).",
    "Control (or do not control) a monster with ATK or DEF compared against a value.",
    "Control (or do not control) a monster with a specific Level, or higher/fewer Level than a number.",
    "A monster of a specific Type is present (or not present) on the field (own, opponent's, or both fields).",
    "A monster of a specific Attribute is present (or not present) on the field (own, opponent's, or both fields).",
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

MODIFIERS = (
    "always",
    "first_attacked",
    "destroyed_after_2_attacks",
    "destroyed_after_3_attacks",
    "attacked_by_type",
    "attacked_by_attribute",
    "attacked_by_stat",
    "attacked_by_level",
    "attacked_by_monster",
)
MODIFIER_LABELS = (
    "Always",
    "The first time it is attacked",
    "Can only be destroyed after two attacks",
    "Can only be destroyed after three attacks",
    "If attacked by monster of Type",
    "If attacked by monster of Attribute",
    "If attacked by a monster with higher / lower ATK or DEF",
    "If attacked by a monster with higher/lower level",
    "If attacked by monster (combined criteria)",
)

ATTACK_COUNT_MODIFIERS = (
    "first_attacked",
    "destroyed_after_2_attacks",
    "destroyed_after_3_attacks",
)

PIERCING_MODIFIERS = (
    "always",
    "attacks_type",
    "attacks_attribute",
    "attacks_stat",
    "attacks_level",
    "attacks_monster",
)
PIERCING_MODIFIER_LABELS = (
    "Always",
    "If it attacks a monster of Type",
    "If it attacks a monster of Attribute",
    "If it attacks a monster with higher / lower ATK or DEF",
    "If it attacks a monster with higher/lower level",
    "If it attacks a monster (combined criteria)",
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
    if do == "change_field":
        return when not in ("face_up", "combat")
    if when == "face_up":
        return (do == "boost" and target != "battle") or (do in ("battle_immunity", "piercing") and (target is None or target == "self"))
    if do in ("battle_immunity", "piercing"):
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
    if do in ("battle_immunity", "piercing"):
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


def normalize_condition(cond: dict, resolve=None, type_names=None) -> dict | None:
    """A condition entry with its names in game spelling, or None if invalid."""
    if not isinstance(cond, dict):
        return None
    c_type = _name(CONDITIONS, cond.get("type"))
    if not c_type:
        return None
    out = {"type": c_type}
    if c_type in ("control_stat", "control_level", "control_type", "control_attribute"):
        if "presence" in cond:
            pres = _name(PRESENCE, cond["presence"])
            if pres is None:
                return None
            if pres == "not_control":
                out["presence"] = "not_control"
        if "scope" in cond:
            sc = _name(SCOPE, cond["scope"])
            if sc is None:
                return None
            if sc == "other":
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
        stat = _name(STATS, cond.get("stat", "atk"))
        if stat is None:
            return None
        out["stat"] = stat
        op = cond.get("op", ">=")
        if op not in OPS:
            return None
        out["op"] = op
        val = cond.get("value", 0)
        if not isinstance(val, int) or isinstance(val, bool) or not (0 <= val <= AMOUNT_MAX):
            return None
        out["value"] = val
    elif c_type == "control_stat":
        stat = _name(STATS, cond.get("stat", "atk"))
        if stat is None:
            return None
        out["stat"] = stat
        op = cond.get("op", ">=")
        if op not in OPS:
            return None
        out["op"] = op
        val = cond.get("value", 0)
        if not isinstance(val, int) or isinstance(val, bool) or not (0 <= val <= AMOUNT_MAX):
            return None
        out["value"] = val
    elif c_type == "control_level":
        op = cond.get("op", "==")
        if op not in OPS:
            return None
        out["op"] = op
        val = cond.get("level")
        if not isinstance(val, int) or isinstance(val, bool) or not (1 <= val <= 12):
            return None
        out["level"] = val
    elif c_type == "control_type":
        if "whose" in cond:
            whose = _name(EACH, cond["whose"])
            if whose is None:
                return None
            if whose != "own":
                out["whose"] = whose
        if "mode" in cond:
            mode = _name(TYPE_MODES, cond["mode"])
            if mode is None:
                return None
            if mode == "only":
                out["mode"] = "only"
        t = cond.get("card_type")
        if t is None:
            return None
        names = type_names or TYPE_NAMES
        if isinstance(t, str):
            found = _letters_index(t, [name or "" for name in names])
            t = found if found is not None else _letters_index(t, TYPE_NAMES)
        if not isinstance(t, int) or isinstance(t, bool) or not is_monster_type(t):
            return None
        if t >= TYPE_MAGIC and not (t < len(names) and names[t]):
            return None
        out["card_type"] = names[t] if t < len(names) and names[t] else TYPE_NAMES[t]
    elif c_type == "control_attribute":
        if "whose" in cond:
            whose = _name(EACH, cond["whose"])
            if whose is None:
                return None
            if whose != "own":
                out["whose"] = whose
        if "mode" in cond:
            mode = _name(TYPE_MODES, cond["mode"])
            if mode is None:
                return None
            if mode == "only":
                out["mode"] = "only"
        attr = cond.get("attribute")
        if attr is not None:
            out["attribute"] = str(attr)
    elif c_type == "field_spell":
        fld = cond.get("field", "any")
        if isinstance(fld, int) and not isinstance(fld, bool):
            if 330 <= fld <= 335:
                fld = FIELD_SPELLS[fld - 330 + 1]
            elif 0 <= fld < len(FIELD_SPELLS):
                fld = FIELD_SPELLS[fld]
            else:
                return None
        elif isinstance(fld, str):
            fld_val = _name(FIELD_SPELLS, fld)
            if fld_val is None:
                return None
            fld = fld_val
        else:
            return None
        out["field"] = fld

        if "presence" in cond:
            status = _name(FIELD_STATUS, cond["presence"])
            if status is None:
                return None
            out["presence"] = status
        else:
            out["presence"] = "active"
    elif c_type in ("lowest_atk", "highest_atk", "fewer_monsters", "more_monsters", "lower_lp", "higher_lp"):
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
    elif do == "change_field":
        field = effect.get("field")
        if not isinstance(field, (str, int)) or isinstance(field, bool):
            return None
        out["field"] = field
    elif do == "battle_immunity":
        out["target"] = target
        if "modifier" in effect:
            mod_name = _name(MODIFIERS, effect["modifier"])
            if mod_name is None:
                return None
            if mod_name != "always":
                out["modifier"] = mod_name
                if mod_name in ATTACK_COUNT_MODIFIERS:
                    rst = _name(RESET, effect.get("reset", "turn"))
                    if rst is None:
                        return None
                    out["reset"] = rst
                if mod_name in ("attacked_by_type", "attacked_by_monster"):
                    t = effect.get("attacker_type")
                    if t is not None:
                        names = type_names or TYPE_NAMES
                        if isinstance(t, str):
                            found = _letters_index(t, [name or "" for name in names])
                            t = found if found is not None else _letters_index(t, TYPE_NAMES)
                        if isinstance(t, int) and not isinstance(t, bool) and is_monster_type(t) and                                 (t < TYPE_MAGIC or (t < len(names) and bool(names[t]))):
                            out["attacker_type"] = names[t] if t < len(names) and names[t] else TYPE_NAMES[t]
                        elif mod_name == "attacked_by_type":
                            return None
                    elif mod_name == "attacked_by_type":
                        return None
                if mod_name in ("attacked_by_attribute", "attacked_by_monster"):
                    a = effect.get("attacker_attribute")
                    if a is not None:
                        a = attribute_resolve(a) if attribute_resolve else (_letters_index(a, ATTRIBUTE_NAMES) if isinstance(a, str) else a)
                        if isinstance(a, int) and not isinstance(a, bool) and 0 <= a <= 255 and a not in (6, 7):
                            out["attacker_attribute"] = ATTRIBUTE_NAMES[a] if a < len(ATTRIBUTE_NAMES) else a
                        elif mod_name == "attacked_by_attribute":
                            return None
                    elif mod_name == "attacked_by_attribute":
                        return None
                if mod_name in ("attacked_by_stat", "attacked_by_monster"):
                    st = _name(STATS, effect.get("attacker_stat"))
                    if st is not None:
                        out["attacker_stat"] = st
                    amt = effect.get("attacker_amount")
                    if amt is not None:
                        if isinstance(amt, int) and not isinstance(amt, bool) and 0 <= amt <= AMOUNT_MAX:
                            out["attacker_amount"] = amt
                            if "attacker_stat" not in out:
                                out["attacker_stat"] = "atk"
                        elif mod_name == "attacked_by_stat":
                            return None
                    elif mod_name == "attacked_by_stat":
                        return None
                    sop = effect.get("attacker_stat_op")
                    if sop is None and (mod_name == "attacked_by_stat" or "attacker_stat" in out):
                        sop = ">"
                    if sop is not None:
                        if sop not in OPS:
                            return None
                        out["attacker_stat_op"] = sop
                if mod_name in ("attacked_by_level", "attacked_by_monster"):
                    lvl = effect.get("attacker_level")
                    if lvl is not None:
                        if isinstance(lvl, int) and not isinstance(lvl, bool) and 1 <= lvl <= 12:
                            out["attacker_level"] = lvl
                        elif mod_name == "attacked_by_level":
                            return None
                    elif mod_name == "attacked_by_level":
                        return None
                    lop = effect.get("attacker_level_op")
                    if lop is None and (mod_name == "attacked_by_level" or "attacker_level" in out):
                        lop = ">"
                    if lop is not None:
                        if lop not in OPS:
                            return None
                        out["attacker_level_op"] = lop
    elif do == "piercing":
        out["target"] = target
        if "modifier" in effect:
            mod_name = _name(PIERCING_MODIFIERS, effect["modifier"])
            if mod_name is None:
                return None
            if mod_name != "always":
                out["modifier"] = mod_name
                if mod_name in ("attacks_type", "attacks_monster"):
                    t = effect.get("defender_type")
                    if t is not None:
                        names = type_names or TYPE_NAMES
                        if isinstance(t, str):
                            found = _letters_index(t, [name or "" for name in names])
                            t = found if found is not None else _letters_index(t, TYPE_NAMES)
                        if isinstance(t, int) and not isinstance(t, bool) and is_monster_type(t) and                                 (t < TYPE_MAGIC or (t < len(names) and bool(names[t]))):
                            out["defender_type"] = names[t] if t < len(names) and names[t] else TYPE_NAMES[t]
                        elif mod_name == "attacks_type":
                            return None
                    elif mod_name == "attacks_type":
                        return None
                if mod_name in ("attacks_attribute", "attacks_monster"):
                    a = effect.get("defender_attribute")
                    if a is not None:
                        a = attribute_resolve(a) if attribute_resolve else (_letters_index(a, ATTRIBUTE_NAMES) if isinstance(a, str) else a)
                        if isinstance(a, int) and not isinstance(a, bool) and 0 <= a <= 255 and a not in (6, 7):
                            out["defender_attribute"] = ATTRIBUTE_NAMES[a] if a < len(ATTRIBUTE_NAMES) else a
                        elif mod_name == "attacks_attribute":
                            return None
                    elif mod_name == "attacks_attribute":
                        return None
                if mod_name in ("attacks_stat", "attacks_monster"):
                    st = _name(STATS, effect.get("defender_stat"))
                    if st is not None:
                        out["defender_stat"] = st
                    amt = effect.get("defender_amount")
                    if amt is not None:
                        if isinstance(amt, int) and not isinstance(amt, bool) and 0 <= amt <= AMOUNT_MAX:
                            out["defender_amount"] = amt
                            if "defender_stat" not in out:
                                out["defender_stat"] = "atk"
                        elif mod_name == "attacks_stat":
                            return None
                    elif mod_name == "attacks_stat":
                        return None
                    sop = effect.get("defender_stat_op")
                    if sop is None and (mod_name == "attacks_stat" or "defender_stat" in out):
                        sop = ">"
                    if sop is not None:
                        if sop not in OPS:
                            return None
                        out["defender_stat_op"] = sop
                if mod_name in ("attacks_level", "attacks_monster"):
                    lvl = effect.get("defender_level")
                    if lvl is not None:
                        if isinstance(lvl, int) and not isinstance(lvl, bool) and 1 <= lvl <= 12:
                            out["defender_level"] = lvl
                        elif mod_name == "attacks_level":
                            return None
                    elif mod_name == "attacks_level":
                        return None
                    lop = effect.get("defender_level_op")
                    if lop is None and (mod_name == "attacks_level" or "defender_level" in out):
                        lop = ">"
                    if lop is not None:
                        if lop not in OPS:
                            return None
                        out["defender_level_op"] = lop
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
        if when != "face_up":
            return None
        m = _name(COND_MODES, effect["condition_mode"])
        if m is None:
            return None
        out["condition_mode"] = m

    if "conditions" in effect:
        if when != "face_up":
            return None
        raw_conds = effect["conditions"]
        if not isinstance(raw_conds, list):
            return None
        norm_conds = []
        for c in raw_conds[:MAX_CONDITIONS]:
            nc = normalize_condition(c, resolve, type_names)
            if nc is None:
                return None
            norm_conds.append(nc)
        if norm_conds:
            out["conditions"] = norm_conds

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
    """True if effect has a battle immunity attack count modifier."""
    if not isinstance(effect, dict):
        return False
    if effect.get("do") == "battle_immunity":
        mod = effect.get("modifier")
        if mod and mod in ATTACK_COUNT_MODIFIERS:
            return True
    return False


def monster_has_attack_condition(effects: list, exclude_index: int | None = None) -> bool:
    """True if any effect other than exclude_index has an attack count modifier."""
    if not isinstance(effects, list):
        return False
    for i, e in enumerate(effects):
        if exclude_index is not None and i == exclude_index:
            continue
        if effect_has_attack_condition(e):
            return True
    return False


def count_monster_attack_conditions(effects: list) -> int:
    """Total count of attack count modifiers across all effects of a monster."""
    if not isinstance(effects, list):
        return 0
    return sum(1 for e in effects if effect_has_attack_condition(e))


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
    if c_type in ("lower_lp", "lp_lower", "fewer_lp"):
        return "your LP is lower than opponent's"
    if c_type in ("higher_lp", "lp_higher", "more_lp"):
        return "your LP is higher than opponent's"
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
        p = cond.get("presence") in ("not_control", "not_present")
        t_name = str(cond.get("card_type") or cond.get("monster_type") or (cond.get("type") if cond.get("type") not in CONDITIONS else "Dragon"))
        whose = cond.get("whose") or cond.get("field") or "own"
        if whose == "opponent":
            if cond.get("mode") == "only":
                return f"the opponent does not only control {t_name} monsters" if p else f"the opponent only controls {t_name} monsters"
            return f"no {t_name} monster is present on opponent's field" if p else f"a {t_name} monster is present on opponent's field"
        elif whose in ("all", "both"):
            if cond.get("mode") == "only":
                return f"the field does not only contain {t_name} monsters" if p else f"the field only contains {t_name} monsters"
            return f"no {t_name} monster is present on the field" if p else f"a {t_name} monster is present on the field"
        else:
            if cond.get("mode") == "only":
                sc = " other" if cond.get("scope") == "other" else ""
                return f"you do not only control{sc} {t_name} monsters" if p else f"you only control{sc} {t_name} monsters"
            p_str = "you do not control" if p else "you control"
            sc = " another" if cond.get("scope") == "other" else " a"
            return f"{p_str}{sc} {t_name} monster"
    if c_type == "control_attribute":
        p = cond.get("presence") in ("not_control", "not_present")
        attr = str(cond.get("attribute", "LIGHT")).upper()
        whose = cond.get("whose") or cond.get("field") or "own"
        if whose == "opponent":
            if cond.get("mode") == "only":
                return f"the opponent does not only control {attr} monsters" if p else f"the opponent only controls {attr} monsters"
            return f"no {attr} monster is present on opponent's field" if p else f"a {attr} monster is present on opponent's field"
        elif whose in ("all", "both"):
            if cond.get("mode") == "only":
                return f"the field does not only contain {attr} monsters" if p else f"the field only contains {attr} monsters"
            return f"no {attr} monster is present on the field" if p else f"a {attr} monster is present on the field"
        else:
            if cond.get("mode") == "only":
                sc = " other" if cond.get("scope") == "other" else ""
                return f"you do not only control{sc} {attr} monsters" if p else f"you only control{sc} {attr} monsters"
            p_str = "you do not control" if p else "you control"
            sc = " another" if cond.get("scope") == "other" else " a"
            return f"{p_str}{sc} {attr} monster"
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
        if mod in ATTACK_COUNT_MODIFIERS:
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
        elif mod == "attacked_by_type":
            t = effect.get("attacker_type", "monster")
            return f"Cannot be destroyed by battle if attacked by a {t} monster{conds}"
        elif mod == "attacked_by_attribute":
            a = effect.get("attacker_attribute", "")
            return f"Cannot be destroyed by battle if attacked by a {a} monster{conds}"
        elif mod == "attacked_by_stat":
            s = effect.get("attacker_stat", "atk").upper()
            amt = effect.get("attacker_amount", 0)
            op = effect.get("attacker_stat_op") or effect.get("comparison") or effect.get("op", ">")
            if op == "<":
                comp_str = f"lower {s} than {amt}"
            elif op == "<=":
                comp_str = f"{s} <= {amt}"
            elif op == ">=":
                comp_str = f"{s} >= {amt}"
            elif op in ("==", "="):
                comp_str = f"{s} equal to {amt}"
            else:
                comp_str = f"higher {s} than {amt}"
            return f"Cannot be destroyed by battle if attacked by a monster with {comp_str}{conds}"
        elif mod == "attacked_by_level":
            lvl = effect.get("attacker_level", 1)
            op = effect.get("attacker_level_op") or effect.get("comparison") or effect.get("op", ">")
            if op == "<":
                comp_str = f"lower Level than {lvl}"
            elif op == "<=":
                comp_str = f"Level <= {lvl}"
            elif op == ">=":
                comp_str = f"Level >= {lvl}"
            elif op in ("==", "="):
                comp_str = f"Level equal to {lvl}"
            else:
                comp_str = f"higher Level than {lvl}"
            return f"Cannot be destroyed by battle if attacked by a monster with {comp_str}{conds}"
        elif mod == "attacked_by_monster":
            parts = []
            if "attacker_attribute" in effect:
                parts.append(str(effect["attacker_attribute"]))
            if "attacker_type" in effect:
                parts.append(str(effect["attacker_type"]))
            crit = " ".join(parts) if parts else "monster"
            extra = []
            if "attacker_stat" in effect and "attacker_amount" in effect:
                s = effect["attacker_stat"].upper()
                amt = effect["attacker_amount"]
                op = effect.get("attacker_stat_op") or effect.get("comparison") or effect.get("op", ">")
                if op == "<":
                    extra.append(f"lower {s} than {amt}")
                elif op == "<=":
                    extra.append(f"{s} <= {amt}")
                elif op == ">=":
                    extra.append(f"{s} >= {amt}")
                elif op in ("==", "="):
                    extra.append(f"{s} equal to {amt}")
                else:
                    extra.append(f"higher {s} than {amt}")
            if "attacker_level" in effect:
                lvl = effect["attacker_level"]
                op = effect.get("attacker_level_op") or effect.get("comparison") or effect.get("op", ">")
                if op == "<":
                    extra.append(f"lower Level than {lvl}")
                elif op == "<=":
                    extra.append(f"Level <= {lvl}")
                elif op == ">=":
                    extra.append(f"Level >= {lvl}")
                elif op in ("==", "="):
                    extra.append(f"Level equal to {lvl}")
                else:
                    extra.append(f"higher Level than {lvl}")
            with_str = f" with {' and '.join(extra)}" if extra else ""
            return f"Cannot be destroyed by battle if attacked by a {crit}{with_str}{conds}"
        return f"Cannot be destroyed by battle{conds}"
    if do == "piercing":
        mod = effect.get("modifier", "always")
        if mod == "attacks_type":
            t = effect.get("defender_type") or effect.get("target_type") or effect.get("attacker_type", "monster")
            return f"Inflicts piercing damage when attacking a {t} monster{conds}"
        elif mod == "attacks_attribute":
            a = effect.get("defender_attribute") or effect.get("target_attribute") or effect.get("attacker_attribute", "")
            return f"Inflicts piercing damage when attacking a {a} monster{conds}"
        elif mod == "attacks_stat":
            s = str(effect.get("defender_stat") or effect.get("target_stat") or effect.get("attacker_stat", "atk")).upper()
            amt = effect.get("defender_amount") if "defender_amount" in effect else (
                effect.get("target_amount") if "target_amount" in effect else effect.get("attacker_amount", 0)
            )
            op = effect.get("defender_stat_op") or effect.get("target_stat_op") or effect.get("attacker_stat_op") or effect.get("comparison") or effect.get("op", ">")
            if op == "<":
                comp_str = f"lower {s} than {amt}"
            elif op == "<=":
                comp_str = f"{s} <= {amt}"
            elif op == ">=":
                comp_str = f"{s} >= {amt}"
            elif op in ("==", "="):
                comp_str = f"{s} equal to {amt}"
            else:
                comp_str = f"higher {s} than {amt}"
            return f"Inflicts piercing damage when attacking a monster with {comp_str}{conds}"
        elif mod == "attacks_level":
            lvl = effect.get("defender_level") if "defender_level" in effect else (
                effect.get("target_level") if "target_level" in effect else effect.get("attacker_level", 1)
            )
            op = effect.get("defender_level_op") or effect.get("target_level_op") or effect.get("attacker_level_op") or effect.get("comparison") or effect.get("op", ">")
            if op == "<":
                comp_str = f"lower Level than {lvl}"
            elif op == "<=":
                comp_str = f"Level <= {lvl}"
            elif op == ">=":
                comp_str = f"Level >= {lvl}"
            elif op in ("==", "="):
                comp_str = f"Level equal to {lvl}"
            else:
                comp_str = f"higher Level than {lvl}"
            return f"Inflicts piercing damage when attacking a monster with {comp_str}{conds}"
        elif mod == "attacks_monster":
            parts = []
            a = effect.get("defender_attribute") or effect.get("target_attribute") or effect.get("attacker_attribute")
            if a:
                parts.append(str(a))
            t = effect.get("defender_type") or effect.get("target_type") or effect.get("attacker_type")
            if t:
                parts.append(str(t))
            crit = " ".join(parts) if parts else "monster"
            extra = []
            stat = effect.get("defender_stat") or effect.get("target_stat") or effect.get("attacker_stat")
            amt = effect.get("defender_amount") if "defender_amount" in effect else (
                effect.get("target_amount") if "target_amount" in effect else effect.get("attacker_amount")
            )
            if stat and amt is not None:
                s = str(stat).upper()
                op = effect.get("defender_stat_op") or effect.get("target_stat_op") or effect.get("attacker_stat_op") or effect.get("comparison") or effect.get("op", ">")
                if op == "<":
                    extra.append(f"lower {s} than {amt}")
                elif op == "<=":
                    extra.append(f"{s} <= {amt}")
                elif op == ">=":
                    extra.append(f"{s} >= {amt}")
                elif op in ("==", "="):
                    extra.append(f"{s} equal to {amt}")
                else:
                    extra.append(f"higher {s} than {amt}")
            lvl = effect.get("defender_level") if "defender_level" in effect else (
                effect.get("target_level") if "target_level" in effect else effect.get("attacker_level")
            )
            if lvl is not None:
                op = effect.get("defender_level_op") or effect.get("target_level_op") or effect.get("attacker_level_op") or effect.get("comparison") or effect.get("op", ">")
                if op == "<":
                    extra.append(f"lower Level than {lvl}")
                elif op == "<=":
                    extra.append(f"Level <= {lvl}")
                elif op == ">=":
                    extra.append(f"Level >= {lvl}")
                elif op in ("==", "="):
                    extra.append(f"Level equal to {lvl}")
                else:
                    extra.append(f"higher Level than {lvl}")
            with_str = f" with {' and '.join(extra)}" if extra else ""
            return f"Inflicts piercing damage when attacking a {crit}{with_str}{conds}"
        return f"Inflicts piercing damage{conds}"
    if do == "magic":
        return f"{card_name(effect.get('card'))} (its effect){conds}"
    if do == "heal":
        return f"Its owner gains {effect.get('amount')} LP{each}{conds}"
    if do == "damage":
        return f"The opponent loses {effect.get('amount')} LP{each}{conds}"
    if do == "change_field":
        return f"Change the field to {effect.get('field')}"
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

