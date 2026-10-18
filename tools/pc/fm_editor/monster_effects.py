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
    "first_attacked",
    "destroyed_after_2_attacks",
    "destroyed_after_3_attacks",
    "stat_threshold",
    "lowest_atk",
    "highest_atk",
)
CONDITION_LABELS = (
    "Named card on field",
    "The first time it is attacked",
 
    "Can only be destroyed after two attacks",
    "Can only be destroyed after three attacks",
    "Compare ATK/DEF/Level against a number",
    "Has the lowest ATK on field",
    "Has the highest ATK on field",
)
CONDITION_HINTS = (
    "A specific card is face up on the field (own, opponent, or whole field).",
    "The first time this card is attacked (survives 1 attack, can be destroyed on the second attack).",
    "Can only be destroyed after two attacks (survives up to 2 attacks, can be destroyed on the third attack).",
    "Can only be destroyed after three attacks (survives up to 3 attacks, can be destroyed on the fourth attack).",
    "This card's current effective ATK or DEF, or its printed Level, compared against a value (<, <=, >, >=).",
    "This card has strictly lower ATK than every other face-up monster on the field (ties disqualify).",
    "This card has strictly higher ATK than every other face-up monster on the field (ties disqualify).",
)
RESET = ("copy", "duel", "turn")
RESET_LABELS = ("This card copy", "In a duel", "In a turn")
OPS = ("<", "<=", ">", ">=")
OP_LABELS = ("Lower than (<)", "Lower or equal (<=)", "Higher than (>)", "Higher or equal (>=)")
STATS = ("atk", "def", "level")
STAT_LABELS = ("ATK", "DEF", "Level")
COND_MODES = ("all", "any")
COND_MODE_LABELS = ("All conditions must be met (AND)", "Any condition can be met (OR)")
MAX_CONDITIONS = 4

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
    if raw_type in ("second_attacked", "destroyed_after_two_attacks", "after_two_attacks", "can_only_be_destroyed_after_two_attacks"):
        raw_type = "destroyed_after_2_attacks"
    elif raw_type in ("third_attacked", "destroyed_after_three_attacks", "after_three_attacks", "can_only_be_destroyed_after_three_attacks"):
        raw_type = "destroyed_after_3_attacks"
    elif raw_type in ("after_one_attack", "destroyed_after_1_attack"):
        raw_type = "first_attacked"
    c_type = _name(CONDITIONS, raw_type)
    if not c_type:
        return None
    out = {"type": c_type}
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


    elif c_type in ("first_attacked", "destroyed_after_2_attacks", "destroyed_after_3_attacks"):
        if "reset" in cond:
            reset_val = cond["reset"]
            if reset_val == "summon":
                reset_val = "copy"
            reset = _name(RESET, reset_val)
            if reset is None:
                return None
            out["reset"] = reset
        if "comparison" in cond or "op" in cond:
            op = cond.get("comparison") or cond.get("op")
            if op in ("<=", "le", "lower_equal"):
                out["comparison"] = "<="
    elif c_type == "stat_threshold":
        stat = cond.get("stat", "atk")
        if isinstance(stat, str):
            s = stat.lower()
            if s in ("def", "defense"):
                stat = "def"
            elif s in ("atk", "attack"):
                stat = "atk"
            elif s in ("level", "lvl", "stars", "star"):
                stat = "level"
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
            else:
                return None
        else:
            return None
        out["comparison"] = op

        val = cond.get("value", 0)
        min_val = 1 if stat == "level" else 0
        max_val = 12 if stat == "level" else AMOUNT_MAX
        if not isinstance(val, int) or isinstance(val, bool) or not (min_val <= val <= max_val):
            return None
        out["value"] = val
    elif c_type in ("lowest_atk", "highest_atk"):
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
    if c_type in ("first_attacked", "destroyed_after_2_attacks", "destroyed_after_3_attacks","second_attacked", "third_attacked"):
        reset = cond.get("reset", "copy")
        if reset == "summon":
            reset = "copy"
        scope = "this turn" if reset == "turn" else ("this duel" if reset == "duel" else "this card copy")



        if c_type in ("destroyed_after_2_attacks", "second_attacked"):


            return f"can only be destroyed after two attacks ({scope})"
        
        elif c_type == "destroyed_after_3_attacks":
            return f"can only be destroyed after three attacks ({scope})"
        return f"the first time attacked ({scope})"
    if c_type == "stat_threshold":
        stat_raw = str(cond.get("stat", "atk")).lower()
        stat_name = "Level" if stat_raw in ("level", "lvl", "stars", "star") else stat_raw.upper()
        op = cond.get("comparison") or cond.get("op", ">=")
        val = cond.get("value", 0)
        return f"{stat_name} {op} {val}"
    if c_type == "lowest_atk":
        return "lowest ATK on field"
    if c_type == "highest_atk":
        return "highest ATK on field"
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

