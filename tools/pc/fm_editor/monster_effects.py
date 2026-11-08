"""Monster effects (notes/more-cards.md, "Monster effects"): what a card's
"monster_effects" make a monster do on the field, as the game reads them
(src/pc/cards/monster_effects.c). The list lives in the card's mod.json
entry, so the editor keeps it there (card_extra, or an added card's extra)."""
from __future__ import annotations

from .gamedata import ATTRIBUTE_NAMES, TYPE_NAMES, TYPE_MAGIC, is_monster_type

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
DO = ("magic", "boost", "heal", "damage", "destroy", "change_field", "cannot_be_destroyed_by_battle",
      "destroy_both_non_battle_immune", "destroy_both", "piercing", "multiple_attacks")
DO_LABELS = ("Magic card effect", "Boost ATK/DEF", "Heal its owner's LP", "Damage the opponent's LP",
             "Destroy monsters", "Change field", "Battle immunity", "Destroy both except immune cards",
             "Destroy both cards", "Piercing battle damage", "Multiple attacks")
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
        return do == "boost" and target != "battle"
    if when == "combat":
        return do in ("heal", "damage") or (do in ("cannot_be_destroyed_by_battle", "destroy_both_non_battle_immune", "destroy_both", "piercing", "multiple_attacks")
                                            and target == "self") or (do == "boost" and target in ("self", "battle"))
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
    elif do in ("cannot_be_destroyed_by_battle", "destroy_both_non_battle_immune", "destroy_both", "piercing"):
        pass
    elif do == "multiple_attacks":
        amount = effect.get("amount")
        if not isinstance(amount, int) or isinstance(amount, bool) or not 2 <= amount <= 8:
            return None
        out["amount"] = amount
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


def describe(effect: dict, card_name=lambda cid: f"#{cid}") -> str:
    """What the effect does, in a line: "Raigeki", "Its owner's other
    Dragon monsters +500 ATK", "Heal its owner 800 LP", "This card: +300
    ATK for each face-up Dragon on the field"."""
    do = effect.get("do")
    each = for_each_words(effect.get("for_each"))
    if do == "magic":
        return f"{card_name(effect.get('card'))} (its effect)"
    if do == "heal":
        return f"Its owner gains {effect.get('amount')} LP{each}"
    if do == "damage":
        return f"The opponent loses {effect.get('amount')} LP{each}"
    if do == "change_field":
        return f"Change the field to {effect.get('field')}"
    if do == "cannot_be_destroyed_by_battle":
        return "This card survives the battle"
    if do == "destroy_both":
        return "Destroy both battling cards after damage"
    if do == "destroy_both_non_battle_immune":
        return "Destroy both after damage unless they have battle immunity"
    if do == "piercing":
        return "Excess ATK against a defending monster damages its owner"
    if do == "multiple_attacks":
        return f"May attack {effect.get('amount')} times each turn"
    target = effect.get("target", default_target(effect.get("when"), do))
    who = TARGET_LABELS[TARGET.index(target)] if target in TARGET else str(target)
    which = " ".join(str(effect[k]) for k in ("attribute", "type") if k in effect)
    if which:
        who += f" ({which} only)"
    if do == "destroy":
        return f"Destroy {who[0].lower()}{who[1:]}"
    stats = " ".join(f"{effect[k]:+d} {label}" for k, label in (("attack", "ATK"), ("defense", "DEF"))
                     if effect.get(k))
    lasting = " for the battle" if effect.get("when") == "combat" else ""
    return f"{who}: {stats}{each}{lasting}"


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
