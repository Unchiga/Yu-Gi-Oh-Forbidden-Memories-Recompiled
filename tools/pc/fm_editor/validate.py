"""The checks the port's loader makes, run before saving, so a problem shows
in the editor rather than as a line in the Mods window.

Errors are what the port would refuse (the mod, or that one rule); warnings
are what it would accept but likely not as meant.
"""
from __future__ import annotations

import re
from dataclasses import dataclass

from pathlib import Path

from .gamedata import (CARD_COUNT, DECK_COPY_LIMIT, DECK_POOL_MIN_CARDS, DECK_SIZE, DUELIST_NAMES, POOLS,
                       POOL_LABELS, POOL_TOTAL, RITUAL_ORIGINS, RITUAL_TRIBUTE_MAX, TAG_LENGTH_MAX, TAGS_MAX, TYPE_MAGIC, TYPE_EQUIP, TYPE_LIMIT, TYPE_NAMES,
                       TYPE_RITUAL, exodia_piece)
from . import art, board_art, campaign_map, card_text, fixed_decks, guardian_stars, packs as packmath, values
from . import manifest, monster_effects, roster, starter_pools, tower, ui_rules
from .compat import HOST_API, external_packs
from .model import KEY_RE, Project, duelist_named

MOD_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,63}$")
SETTING_TYPES = ("int", "bool", "choice", "key")
MANIFEST_KEYS = ("id", "name", "version", "author", "description", "library", "enabled", "restart",
                 "legacy_setting", "data", "textures", "assets", "cards", "audio", "min_api", "game", "requires", "after",
                 "conflicts", "priority", "settings", "fusions", "equips", "rituals", "drops", "decks", "duelists", "text",
                 "font",
                 "chest_overflow", "terrain_bonus", "trap_thresholds", "equip_bonus_default", "passwords",
                 "starter", "starter_pools", "limits", "guardian_stars", "packs", "pack_shop", "card_text_colors", "card_layout",
                 "palette_ramps", "title", "menu", "ui", "tower", "card_types", "attributes", "fields",
                 "models")


@dataclass
class Issue:
    level: str          # "error" or "warning"
    area: str           # "Mod info", "Cards", "Art", "Fusions", "Equips", "Rituals", "Duelists"
    where: str
    message: str
    target: object = None   # what the GUI selects to show it (a card id, a pair, (duelist, pool)...)

    def __str__(self):
        return f"{self.level}: {self.area}: {self.where}: {self.message}"


def text_lines(text: str) -> int:
    """How many lines the port's card-text wrapping makes (cards.c
    encode_description): 20 letters a line, broken at spaces and at \\n;
    an icon code is two letters, a color code none."""
    return 1 + card_text.encode(text).count("\n")


def _check_info(project: Project, out: list):
    info = project.info
    if not MOD_ID_RE.match(info.id or ""):
        out.append(Issue("error", "Mod info", "id", "id must be 1-63 letters, digits, hyphens or underscores"))
    if info.version and not re.match(r"^\d+(\.\d+)*$", info.version):
        out.append(Issue("warning", "Mod info", "version",
                         "a version is a dotted number (1.2.0), which \"requires\" bounds compare"))
    if not info.name:
        out.append(Issue("warning", "Mod info", "name", "the Mods window shows the id when there is no name"))
    seen = set()
    for i, setting in enumerate(info.settings):
        where = f"settings[{i}]"
        if not isinstance(setting, dict):
            out.append(Issue("error", "Mod info", where, "Invalid setting schema: not an object"))
            continue
        key = setting.get("key")
        if not isinstance(key, str) or not KEY_RE.match(key) or key == "order":
            out.append(Issue("error", "Mod info", where, f"Invalid setting schema: {key!r} is not a usable key"))
            continue
        if key in seen:
            out.append(Issue("error", "Mod info", where, f"Duplicate setting: {key}"))
        seen.add(key)
        kind = setting.get("type", "int")
        if kind not in SETTING_TYPES:
            out.append(Issue("error", "Mod info", where, f"Invalid setting schema: {key} (type is int, bool, choice or key)"))
        for number in ("default", "min", "max", "step"):
            if number in setting and (isinstance(setting[number], bool) or not isinstance(setting[number], int)):
                out.append(Issue("error", "Mod info", where, f"Invalid setting schema: {key} ({number} is a whole number)"))
        if kind == "choice":
            choices = setting.get("choices")
            if not isinstance(choices, list) or not choices or not all(isinstance(c, str) for c in choices):
                out.append(Issue("error", "Mod info", where, f"Invalid setting schema: {key} (a choice needs \"choices\")"))
        # manager.c Mods_CheckManifest and Mods_OptionValid: either refuses the whole mod.
        whole = lambda name, fallback: setting[name] if isinstance(setting.get(name), int) and \
            not isinstance(setting.get(name), bool) else fallback
        low, high, step, default = whole("min", 0), whole("max", 100), whole("step", 1), whole("default", 0)
        if low > high:
            out.append(Issue("error", "Mod info", where, f"Invalid setting schema: {key} (min is more than max)"))
        if step < 1:
            out.append(Issue("error", "Mod info", where, f"Invalid setting schema: {key} (step is at least 1)"))
        if kind == "bool":
            low, high = 0, 1
        elif kind == "choice":
            choices = setting.get("choices")
            low, high = 0, (len(choices) if isinstance(choices, list) else 0) - 1
        elif kind == "key":
            low, high = 0, 0xFFFF
        if not low <= default <= high:
            out.append(Issue("error", "Mod info", where,
                             f"Invalid setting schema: {key} (default {default} is outside {low} to {high})"))
        if len(f"mod.{info.id}.{key}") >= 256:
            out.append(Issue("error", "Mod info", where, f"Invalid setting schema: {key} (the key is too long)"))
    for key in project.other:
        if key not in MANIFEST_KEYS:
            out.append(Issue("warning", "Mod info", key, f"unknown key '{key}'"))
    game = project.other.get("game")
    if game is not None and game != "slus_01411":
        out.append(Issue("error", "Mod info", "game", "\"game\" must be \"slus_01411\""))
    api = project.other.get("min_api")
    if api is not None and (not isinstance(api, int) or api > HOST_API):
        out.append(Issue("error", "Mod info", "min_api", f"this game has mod API {HOST_API}"))
    for key in ("enabled", "restart"):
        if key in project.other and not isinstance(project.other[key], bool):
            out.append(Issue("warning", "Mod info", key, f"\"{key}\" should be true or false"))


def _check_card(project: Project, cid: int, out: list):
    card = project.cards[cid]
    where = project.card_label(cid)
    add = lambda level, message: out.append(Issue(level, "Cards", where, message, cid))
    for label, value in (("ATK", card.attack), ("DEF", card.defense)):
        if not isinstance(value, int) or not 0 <= value <= 5110:
            add("error", f"{label} is 0 to 5110")
        elif value % 10:
            add("error", f"{label} is stored in tens: {value} would become {value // 10 * 10}")
    if not 0 <= card.level <= 12:
        add("error", "level is 0 to 12")
    if not 0 <= card.type < TYPE_LIMIT:
        add("error", "type is one of the 256 type-bank entries")
    if manifest._attribute_choice(card.attribute, project.other.get("attributes")) < 0:
        add("error", "attribute is a retail attribute or a declared id from 8 to 255")
    count = guardian_stars.count(project.other.get("guardian_stars"))
    if not (0 <= card.star1 <= guardian_stars.MAX_STARS and 0 <= card.star2 <= guardian_stars.MAX_STARS):
        add("error", f"guardian stars are 1 to {guardian_stars.MAX_STARS}, or none (a card holds them in 4 bits)")
    elif card.star1 > count or card.star2 > count:
        add("warning", f"a guardian star past the {count} the mod has: declare it in the Guardian Stars tab")
    elif card.is_monster() and not card.star1 and card.star2:
        # stars.c Stars_Normalize: the game reads [none, X] as [X, none].
        name = guardian_stars.choices(project.other.get("guardian_stars"))[card.star2]
        add("warning", f"no first guardian star with a second: the game gives the card the one star {name}, "
                       "as if it were first (put it first, or both none for no star)")
    if not card.name.strip():
        add("warning", "the card has no name")
    elif len(card.name) > 32:
        add("warning", f"a long name ({len(card.name)} letters): its plate squeezes it")
    lines = text_lines(card.description)
    if lines > 8:
        add("warning", f"its text runs to {lines} lines; the card view shows 8")
    if any(ord(c) < 32 and c != "\n" for c in card.name + card.description):
        add("error", "a control character in its name or text")
    _check_password(project, cid, add)
    if cid in project.added:
        added = project.added[cid]
        base = project.cards.get(added.base)
        if not KEY_RE.match(added.key) or len(added.key) > 80:
            add("error", "invalid stable id: letters, digits, _ and - (at most 80)")
        if sum(1 for a in project.added.values() if a.key == added.key) > 1:
            add("error", f"duplicate card identity {added.key}")
        effect = project.retail.cards.get(project.effect_of(cid))
        # An equip needs none: its targets and boosts are the mod's rules.
        matching_effect = not card.is_monster() and (card.type == 23 or (effect and effect.type == card.type))
        if base and base.is_monster() and not card.is_monster() and not matching_effect:
            add("error", "a copy needs a matching retail effect to become a non-monster")
        elif base and not base.is_monster() and not card.is_monster() and card.type != base.type and not matching_effect:
            add("error", "a non-monster copy needs a matching retail effect to change type")
        if card.is_monster() and not project.has_model(cid):
            add("warning", "a card made a monster fights without a 3D model unless \"model\" names one")
    elif cid in project.retail.cards:
        retail = project.retail.cards[cid]
        extra = project.card_extra.get(cid, {})
        if retail.is_monster() and not card.is_monster() and card.type != 23 and "effect" not in extra:
            add("warning", "a monster made a non-monster does nothing when played unless \"effect\" names a card")
        if not retail.is_monster() and card.is_monster() and "model" not in extra:
            add("warning", "a card made a monster fights without a 3D model unless \"model\" names one")


    extra = project.added[cid].extra if cid in project.added else project.card_extra.get(cid, {})
    threshold = extra.get("trap_threshold")
    if threshold is not None:
        if type(threshold) is not int or not 0 <= threshold <= 65535:
            add("error", '"trap_threshold" must be a whole number from 0 to 65535, or null')
        elif card.type != 21 or project.trap_threshold_default(project.effect_of(cid)) is None:
            add("warning", "the saved attack threshold only applies to an attack-destruction trap effect")
    if "monster_effects" in extra:
        attribute_resolve = lambda value: manifest._attribute_choice(value, project.other.get("attributes"))
        for problem in monster_effects.problems(extra["monster_effects"], project.resolve, attribute_resolve,
                                                project.type_names):
            add("error", problem)
        if extra["monster_effects"] and not card.is_monster():
            add("warning", "monster effects only work for a monster on the field; this card is not one")
    if "tags" in extra:
        tags = extra["tags"]
        if not isinstance(tags, list):
            add("warning", '"tags" is a list of words, such as ["god"]; the game reads none from this')
        else:
            for tag in tags:
                if not isinstance(tag, str) or not tag:
                    add("warning", "an empty tag (or not text): the game notes it and leaves it out")
                elif len(tag.encode()) > TAG_LENGTH_MAX:
                    add("warning", f'the tag "{tag}" is over {TAG_LENGTH_MAX} letters: the game notes it and '
                                   "leaves it out")
    effect = project.retail.cards.get(project.effect_of(cid)) if "effect" in extra else None
    if effect and not card.is_monster() and effect.type != card.type:
        name = lambda t: project.type_names[t] if 0 <= t < len(project.type_names) and project.type_names[t] else str(t)
        add("warning", f"its effect is {effect.name}'s, a {name(effect.type)} card's: the CPU does not play it, "
                       f"and as a {name(card.type)} card it may do nothing")


def _check_card_types(project: Project, out: list):
    """Check the runtime registry: IDs, normalized names and display text."""
    key, section = "card_types", project.other.get("card_types")
    if section is None:
        return
    if not isinstance(section, dict) or not isinstance(section.get("types"), list):
        out.append(Issue("error", "Mod info", key, "must be an object with a \"types\" list"))
        return
    seen = set()
    names = {"".join(c.lower() for c in name if c.isascii() and c.isalnum()): i
             for i, name in enumerate(TYPE_NAMES)}
    for index, entry in enumerate(section["types"]):
        where = f"{key}.types[{index}]"
        if not isinstance(entry, dict) or isinstance(entry.get("id"), bool) or not isinstance(entry.get("id"), int) \
                or not isinstance(entry.get("name"), str):
            out.append(Issue("error", "Mod info", where, "must have numeric \"id\" and string \"name\""))
            continue
        type_, name = entry["id"], entry["name"]
        if not 0 <= type_ < TYPE_LIMIT:
            out.append(Issue("error", "Mod info", where,
                             "id is an existing type (0-23) or a new monster type (24-255)"))
        elif type_ in seen:
            out.append(Issue("warning", "Mod info", where, f"redefines type {type_}; the later name wins"))
        seen.add(type_)
        if not name.strip():
            out.append(Issue("error", "Mod info", where, "\"name\" must not be empty"))
            continue
        normalized = "".join(c.lower() for c in name if c.isascii() and c.isalnum())
        if not normalized:
            out.append(Issue("error", "Mod info", where, "\"name\" has no glyph the game can display"))
        elif normalized in names and names[normalized] != type_:
            out.append(Issue("error", "Mod info", where,
                             f"\"name\" is already type {names[normalized]}'s name or retail alias"))
        else:
            names[normalized] = type_


def _check_fields(project: Project, out: list):
    """Match fields.c's registry checks before a manifest is saved."""
    section = project.other.get("fields")
    if section is None:
        return
    if not isinstance(section, dict) or not isinstance(section.get("fields"), list):
        out.append(Issue("error", "Mod info", "fields", "must be an object with a \"fields\" list"))
        return
    retail = {"normal", "forest", "wasteland", "mountain", "sogen", "meadow", "umi", "sea", "yami", "dark"}
    names = dict.fromkeys(retail)
    for index, entry in enumerate(section["fields"]):
        where = f"fields.fields[{index}]"
        if not isinstance(entry, dict):
            out.append(Issue("error", "Mod info", where, "must be an object with numeric id and name"))
            continue
        field_id, name, replace = entry.get("id"), entry.get("name"), entry.get("replace")
        if field_id is not None and (not isinstance(field_id, int) or isinstance(field_id, bool) or not 1 <= field_id <= 255):
            out.append(Issue("error", "Mod info", where, "id is 1 to 255"))
        if replace is not None and (not isinstance(replace, str) or replace.lower() not in retail):
            out.append(Issue("error", "Mod info", where, "replace names a retail field"))
        if (not isinstance(name, str) or not name or not any(c.isalnum() for c in name)) and replace is None:
            out.append(Issue("error", "Mod info", where, "name is a non-empty string with a letter or number"))
            continue
        if isinstance(name, str):
            normalized = "".join(c.lower() for c in name if c.isalnum())
            key = field_id if field_id is not None else replace if replace is not None else index + 7
            if normalized in names and names[normalized] != key:
                out.append(Issue("error", "Mod info", where, "name is already another field's name or retail alias"))
            else:
                names[normalized] = key
        picture = entry.get("image", entry.get("picture"))
        if picture is not None and (not isinstance(picture, str) or not picture or not picture.lower().endswith(".png")):
            out.append(Issue("error", "Mod info", where, "image names a PNG in the mod"))
        if "mirror" in entry and not isinstance(entry["mirror"], bool):
            out.append(Issue("error", "Mod info", where, "mirror is true or false"))


def _check_tags(project: Project, out: list):
    """More than the 32 different tags the game has room for (cards.c
    tag_bit): those met after them, in the order the mod's cards are
    written, are left out. Another mod's own count too."""
    seen, over = [], {}
    owners = [(cid, project.card_extra[cid]) for cid in sorted(project.card_extra)]
    owners += [(cid, project.added[cid].extra) for cid in sorted(project.added)]
    for cid, extra in owners:
        tags = extra.get("tags")
        for tag in tags if isinstance(tags, list) else ():
            if not isinstance(tag, str) or not tag or len(tag.encode()) > TAG_LENGTH_MAX or tag in seen:
                continue
            if len(seen) < TAGS_MAX:
                seen.append(tag)
            elif tag not in over:
                over[tag] = cid
    if over:
        out.append(Issue("warning", "Cards", project.card_label(next(iter(over.values()))),
                         f"{len(seen) + len(over)} different tags, past the {TAGS_MAX} the game has room for "
                         f"(with every mod's): {', '.join(over)} left out", next(iter(over.values()))))


def _check_password(project: Project, cid: int, add):
    """A password the mod sets: 8 digits (tables.c read_shop_password), and
    no other card's. The Password screen gives the lower card number of two
    with the same password, and View > Card passwords would show both."""
    if not project.password_changed(cid):
        return
    value = project.passwords[cid]
    if not value:
        return
    if not re.fullmatch(r"\d{8}", value):
        add("error", "a password is up to 8 digits")
        return
    others = [other for other in sorted(project.cards) if other != cid and project.password(other) == value]
    if others:
        named = ", ".join(project.card_label(o) for o in others[:3]) + (" and more" if len(others) > 3 else "")
        add("error", f"password {value} is also {named}'s: the Password screen gives the lower card number")


def _check_tables(project: Project, out: list):
    valid = lambda cid: cid in project.cards
    for pair, result in project.fusions.items():
        if project.retail.fusions.get(pair) == result:
            continue
        where = f"{project.card_label(pair[0])} + {project.card_label(pair[1])}"
        if not (valid(pair[0]) and valid(pair[1]) and (result == 0 or valid(result))):
            out.append(Issue("error", "Fusions", where, "names a card that does not exist", pair))
        elif result and not project.cards[result].is_monster():
            out.append(Issue("warning", "Fusions", where, "the result is not a monster", pair))
    for equip, monsters in project.equips.items():
        baseline = project.equip_baseline(equip) if equip in project.cards else set()
        if baseline == monsters:
            continue
        where = project.card_label(equip)
        # A card made something else keeps the list it had as an equip (its
        # disc card's, a copy's base's): nothing is written for it.
        if (valid(equip) and project.cards[equip].type != TYPE_EQUIP and
                {m for m in monsters if m <= CARD_COUNT} == set(project.retail.equips.get(project.base_of(equip), ()))):
            continue
        if not valid(equip) or project.cards[equip].type != TYPE_EQUIP:
            out.append(Issue("error", "Equips", where, "\"card\" is not an equip card", equip))
        for m in sorted(monsters - baseline):
            if not valid(m):
                out.append(Issue("error", "Equips", where, f"no card {m}", equip))
            elif not project.cards[m].is_monster():
                out.append(Issue("warning", "Equips", where, f"{project.card_label(m)} is not a monster", equip))
    for ritual, recipe in project.rituals.items():
        if recipe is None:      # an added copy with no recipe (Project.remove_ritual)
            continue
        conditional = project.ritual_requirements.get(ritual)
        origin = project.ritual_from.get(ritual, "field")
        if project.retail.rituals.get(ritual) == recipe and not conditional and origin == "field":
            continue
        where = project.card_label(ritual)
        if not valid(ritual) or not project.is_ritual(ritual):
            out.append(Issue("error", "Rituals", where, "\"card\" must be a ritual card whose effect is a ritual's "
                             "(a copy of one, or \"effect\" naming one)", ritual))
        if not 2 <= len(recipe) <= RITUAL_TRIBUTE_MAX + 1 or not valid(recipe[-1]):
            out.append(Issue("error", "Rituals", where, "one to five tributes and a valid result card are required",
                             ritual))
            continue
        if origin not in RITUAL_ORIGINS:
            out.append(Issue("error", "Rituals", where, "tributes come from the field, the hand or both", ritual))
        tributes = len(recipe) - 1
        if origin == "hand" and tributes == RITUAL_TRIBUTE_MAX:
            # The ritual card is in the hand too when played from it.
            out.append(Issue("warning", "Rituals", where, "five tributes from the hand: only when the ritual card was "
                             "set face down first (played from the hand, four other cards are there)", ritual))
        if conditional:
            if len(conditional) != tributes or any(not req for req in conditional):
                out.append(Issue("error", "Rituals", where, "every tribute needs a requirement", ritual))
            for req in conditional:
                cid = req.get("card")
                if cid and not valid(cid):
                    out.append(Issue("error", "Rituals", where, f"no card {cid}", ritual))
                elif cid and not project.cards[cid].is_monster():
                    out.append(Issue("warning", "Rituals", where, f"{project.card_label(cid)} is not a monster", ritual))
            if not project.cards[recipe[-1]].is_monster():
                out.append(Issue("warning", "Rituals", where, "the result should be a monster", ritual))
        elif not all(valid(c) for c in recipe):
            out.append(Issue("error", "Rituals", where, "every tribute and the result must be a card", ritual))
        elif not all(project.cards[c].is_monster() for c in recipe):
            out.append(Issue("warning", "Rituals", where, "tributes and result should be monsters", ritual))
    for d, pools in enumerate(project.pools):
        for pool in POOLS:
            weights = pools[pool]
            if weights == project.retail.pools[d][pool]:
                continue
            where = f"{DUELIST_NAMES[d]} {POOL_LABELS[pool]}"
            target = (d, pool)
            total = sum(weights.values())
            cards = sum(1 for w in weights.values() if w)
            if any(w < 0 or w > 0xFFFF for w in weights.values()):
                out.append(Issue("error", "Duelists", where, "a weight is a whole number, 0 to 65535", target))
            if any(not valid(c) for c in weights):
                out.append(Issue("error", "Duelists", where, "names a card that does not exist", target))
            if pool == "deck" and cards < DECK_POOL_MIN_CARDS:
                out.append(Issue("error", "Duelists", where,
                                 f"a deck is dealt from at least {DECK_POOL_MIN_CARDS} cards; it has {cards}", target))
            elif pool != "deck" and not cards:
                out.append(Issue("error", "Duelists", where, "no card would be left to win", target))
            if total != POOL_TOTAL:
                out.append(Issue("error", "Duelists", where,
                                 f"the weights add up to {total}, not {POOL_TOTAL} (Normalize fixes it)", target))


def _check_starter(project: Project, out: list):
    """The decks a new game may be dealt. The port deals a deck only when it
    is exactly DECK_SIZE cards, so anything else is an error here rather than
    a line in the Mods window after the fact."""
    decks = project.starter
    for i, deck in enumerate(decks):
        where = deck.name or f"deck {i + 1}"
        total = deck.total()
        if total != DECK_SIZE:
            out.append(Issue("error", "Starter decks", where,
                             f"a starter deck is {DECK_SIZE} cards, and this one has {total}", i))
        for name in deck.kept:
            out.append(Issue("error", "Starter decks", where, f"\"{name}\" is no card the editor knows", i))
        for cid, copies in sorted(deck.cards.items()):
            if cid not in project.cards:
                out.append(Issue("error", "Starter decks", where, f"no card {cid}", i))
            elif copies > DECK_COPY_LIMIT:
                out.append(Issue("warning", "Starter decks", where,
                                 f"{copies} copies of {project.card_label(cid)}: dealt as written, but Build Deck "
                                 f"takes back only {DECK_COPY_LIMIT}", i))
            elif exodia_piece(cid) and copies > 1:
                out.append(Issue("warning", "Starter decks", where,
                                 f"{copies} copies of {project.card_label(cid)}: Build Deck takes back only one "
                                 "of each Exodia piece", i))
    if decks and not any(deck.weight for deck in decks):
        out.append(Issue("warning", "Starter decks", "every deck",
                         "every deck weighs 0, so none is ever picked and the disc's own pools deal the deck", 0))


def pack_resolver(project: Project):
    """How the packs' cards are named for the checks and Simulate: as the
    game's Cards_Reference, the cards this project knows (a mod's own by its
    identity)."""
    def resolve(value):
        if isinstance(value, float) and value.is_integer():
            value = int(value)          # the game's JSON reads 5.0 as 5
        cid = project.resolve(value)
        return cid if cid else -1
    return resolve


def _check_packs(project: Project, out: list):
    """What the port's reader says of "packs" and "pack_shop" (packs.c), in
    its words: an error is a pack the game leaves out."""
    for level, message in packmath.check_rules(project.pack_shop):
        out.append(Issue(level, "Packs", "pack_shop", message, None))
    if project.packs_file is not None:
        _, problem = external_packs(project.packs_file, project)
        if problem:
            out.append(Issue("error", "Packs", "packs", problem + "; compatibility cannot be determined"))
        return
    resolve = pack_resolver(project)
    ids, passwords = set(), {}
    card_passwords = {}
    for cid in project.cards:
        text = project.password(cid)
        if text:
            card_passwords.setdefault(int(text, 16) if text.isdigit() else None, cid)
    names = {packmath.pack_id(e) for e in project.packs}
    read, declared = [], 0
    for i, entry in enumerate(project.packs):
        # A pack without "order" is ordered by its place among those past
        # their id, as the game counts them (packs.read_packs).
        place = declared if packmath.entry_id(entry, ids) is not None else None
        declared += place is not None
        pack, notes = packmath.read_pack(entry, resolve, project.info.id, i, ids, place)
        where = packmath.pack_id(entry) if isinstance(entry, dict) else f"packs[{i}]"
        for level, message in notes:
            out.append(Issue(level, "Packs", where, message, i))
        if pack is None:
            continue
        ids.add(pack.id)
        read.append((pack, i))
        for level, message in packmath.shop_notes(pack, project.pack_shop):
            out.append(Issue(level, "Packs", where, message + " in this mod's \"pack_shop\" (another mod may add it)", i))
        image = entry.get("image")
        if isinstance(image, str) and image and image not in project.files and not (
                project.source_dir and (Path(project.source_dir) / image).is_file()):
            out.append(Issue("warning", "Packs", where, f"\"image\" {image} cannot be read; its cover is shown "
                                                         "instead", i))
        if pack.password is not None:
            if pack.password in card_passwords:
                out.append(Issue("warning", "Packs", where,
                                 f"its password is {project.card_label(card_passwords[pack.password])}'s: the card's "
                                 "comes first on the Password screen", i))
        unlock = entry.get("unlock") if isinstance(entry.get("unlock"), dict) else {}
        if "beat" in unlock and duelist_named(unlock["beat"]) < 0:
            out.append(Issue("warning", "Packs", where, f"\"unlock\" \"beat\" names no duelist of the disc "
                                                         f"(\"{unlock['beat']}\"); a mod's own, if it is not "
                                                         "applied, keeps the pack locked", i))
        if "card" in unlock and resolve(unlock["card"]) <= 0:
            out.append(Issue("warning", "Packs", where, f"\"unlock\" \"card\" names no card the editor knows "
                                                         f"(\"{unlock['card']}\"); the pack stays locked", i))
        opened = unlock.get("opened") if isinstance(unlock.get("opened"), dict) else {}
        for name in opened:
            if name not in names and name.split(":", 1)[-1] not in names:
                out.append(Issue("warning", "Packs", where, f"\"unlock\" \"opened\" names no pack of this mod "
                                                             f"(\"{name}\"); another mod's, if it is not applied, "
                                                             "keeps the pack locked", i))
    # Of two packs with one password the game sells the first in the list's
    # order ("order", then as declared), as packs.c Packs_Finish says.
    read.sort(key=lambda pair: pair[0].order)
    for pack, i in read:
        if pack.password is None:
            continue
        if pack.password in passwords:
            out.append(Issue("warning", "Packs", pack.id, f"its password is pack \"{passwords[pack.password]}\"'s "
                                                          "too; that one is sold", i))
        passwords.setdefault(pack.password, pack.id)
    if len(project.packs) > packmath.PACKS_MAX:
        out.append(Issue("error", "Packs", "packs", f"at most {packmath.PACKS_MAX} packs; the rest are left out",
                         packmath.PACKS_MAX))


def validate(project: Project) -> list:
    out = []
    _check_info(project, out)
    _check_card_types(project, out)
    _check_fields(project, out)
    for cid in sorted(project.cards):
        if project.card_changed(cid):
            _check_card(project, cid, out)
    _check_tags(project, out)
    _check_tables(project, out)
    _check_starter(project, out)
    for level, where, message in values.check(project.other.get("limits"), lambda name: roster.named(project, name)):
        out.append(Issue(level, "Values", where, message))
    stars = {}
    for card in project.cards.values():
        if card.is_monster():
            for star in (card.star1, card.star2):
                stars[star] = stars.get(star, 0) + 1
    # The ATK/DEF cap a bonus is measured against: the Values tab's, else 9999.
    flat = values.flatten(project.other.get("limits"))
    caps = [flat[key] for key in ("stats", "attack", "defense") if isinstance(flat.get(key), int)]
    cap = max(caps) if caps else guardian_stars.STAT_CAP
    for level, where, message in guardian_stars.check(project.other.get("guardian_stars"), stat_cap=cap,
                                                      card_stars=stars):
        out.append(Issue(level, "Guardian Stars", where, message))
    _check_packs(project, out)
    for level, where, message in tower.check(project.other.get("tower"), lambda name: roster.named(project, name),
                                             lambda name: project.resolve(name),
                                             lambda name: roster.bust_of(project, roster.named(project, name)),
                                             lambda name: roster.shows_bust(project, roster.named(project, name))):
        out.append(Issue(level, "Tower", where, message))
    fixed_decks.check(project, out)
    roster.check(project, out)
    art.check(project, out)
    campaign_map.check(project, out)
    board_art.check(project, out)
    starter_pools.check(project, out)

    def has_file(name):
        return name in project.files or bool(project.source_dir and (Path(project.source_dir) / name).is_file())
    for level, where, message in ui_rules.check(project, has_file):
        out.append(Issue(level, "UI", where, message))
    return out


def validate_card(project: Project, cid: int) -> list:
    out = []
    _check_card(project, cid, out)
    return out


def errors(issues) -> list:
    return [i for i in issues if i.level == "error"]


# --- the other installed mods ------------------------------------------------------

def _overlaps():
    from . import overlaps
    return overlaps


class _ProjectSource:
    """overlaps.Source for the editor: the cards and opponents of the game the
    editor read, and the mods' settings as the port's settings file has them.
    (Made from overlaps.Source when first used: validate imports nothing of
    it at load.)"""

    def __new__(cls, project: Project, settings: dict, order: list = ()):
        overlaps = _overlaps()
        shown = {}   # a card's name in the game: the last replace in load order that names it

        class Source(overlaps.Source):
            def card(self, text, number):
                if text is None:
                    return number if number in project.cards else -1
                return project.resolve(text) or -1

            def card_name(self, cid):
                # As the game's window names it (cards.c, the cards read in
                # load order): the name of the last mod's replace of it, the
                # disc's when that replace gives none; not the edited mod's
                # own unless it is the last.
                if not shown and order:
                    for mod in order:
                        for entry in overlaps._list(mod.manifest.get("cards")):
                            if not isinstance(entry, dict) or "replace" not in entry:
                                continue
                            replace = entry["replace"]
                            target = self.card(None, replace) if overlaps._int(replace) else \
                                self.card(replace, 0) if isinstance(replace, str) else -1
                            if target and target > 0:
                                name = entry.get("name")
                                shown[target] = name if isinstance(name, str) and name else None
                    shown.setdefault(0, None)
                if shown.get(cid):
                    return shown[cid]
                retail = getattr(getattr(project, "retail", None), "cards", None) or {}
                card = retail.get(cid) if isinstance(retail, dict) else None
                card = card if card is not None else project.cards.get(cid)
                return card.name if card is not None and card.name else None

            def card_info(self, cid):
                card = project.cards.get(cid)
                if card is None:
                    return None
                return project.base_of(cid), card.type, card.attribute

            def duelist(self, text):
                from .model import duelist_named
                return duelist_named(text)

            def setting(self, mod, key):
                for spec in overlaps._list(mod.manifest.get("settings")):
                    if isinstance(spec, dict) and spec.get("key") == key:
                        return settings.get(f"mod.{mod.id}.{key}", overlaps._json_number(spec.get("default"), 0))
                return None

        return Source()


def shipped_folder():
    """The mods folder beside the game, which the game reads first (mods.c,
    Paths_Program "mods"), when the editor can find the game: beside the
    editor's own program, beside the game files the port was last pointed at
    (disc-path.txt, in the game's folder or its game/ folder), or the source
    tree's mods/. None when none of them is there."""
    from pathlib import Path
    from . import disc
    places = []
    places.append(disc.program_dir() / "mods")
    try:
        text = (disc.user_dir() / "disc-path.txt").read_text(encoding="utf-8", errors="replace").strip()
        if text:
            game = Path(text)
            folder = game if game.is_dir() else game.parent
            places += [folder / "mods", folder.parent / "mods"]
    except OSError:
        pass
    here = Path(__file__).resolve()
    if len(here.parents) > 3:
        places.append(here.parents[3] / "mods")
    for place in places:
        try:
            if place.is_dir() and any((child / "mod.json").is_file() for child in place.iterdir()):
                return place
        except OSError:
            continue
    return None


def mod_folders(project: Project = None) -> list:
    """Where the game finds mods, in its order: MEMORIES_MODS_DIR when set
    (the only folder it then reads); else the mods beside the game
    (shipped_folder) and the user directory's mods folder, whose copy of a
    mod replaces the shipped one. And the folder the mod being edited was
    opened from, when it is another."""
    import os
    from pathlib import Path
    from . import disc
    named = os.environ.get("MEMORIES_MODS_DIR")
    if named:
        folders = [Path(named)]
    else:
        shipped = shipped_folder()
        folders = ([shipped] if shipped else []) + [disc.user_dir() / "mods"]
    source = getattr(project, "source_dir", None)
    if source is not None and Path(source).parent.resolve() not in [f.resolve() for f in folders]:
        folders.append(Path(source).parent)
    return folders


def settings_file():
    """The port's settings file (settings.c): MEMORIES_SETTINGS, else the user
    directory's settings.txt."""
    import os
    from pathlib import Path
    from . import disc
    named = os.environ.get("MEMORIES_SETTINGS")
    return Path(named) if named else disc.user_dir() / "settings.txt"


def applied(mod, settings: dict) -> bool:
    """Whether the game applies `mod` now (mods.c Mods_Load): MEMORIES_MODS=0
    turns every mod off; MEMORIES_MOD_<ID> settles one for the run; else the
    player's mod.<id>, or the older key a "legacy_setting" names, or the
    manifest's "enabled" (off when left out)."""
    import os
    overlaps = _overlaps()
    if os.environ.get("MEMORIES_MODS") in ("0", "off"):
        return False
    variable = "MEMORIES_MOD_" + "".join(c.upper() if c.isascii() and c.isalnum() else "_" for c in mod.id)
    if os.environ.get(variable) not in (None, ""):
        return os.environ[variable] not in ("0", "off")
    if f"mod.{mod.id}" in settings:
        return bool(settings[f"mod.{mod.id}"])
    legacy = mod.manifest.get("legacy_setting")
    if isinstance(legacy, str) and legacy in settings:
        return bool(settings[legacy])
    return overlaps._json_bool(mod.manifest.get("enabled"))


def cross_mod(project: Project, folders=None, settings_path=None) -> tuple:
    """(issues, summary): where the mod being edited meets the other mods
    installed in `folders` (mod_folders()), in the order the game would load
    them with the player's Load order (settings_path, settings_file()), the
    way the game's Mods window says it (overlaps.py, src/pc/mods/overlap.c).
    Every installed mod counts, applied or not, so a modder sees a clash
    before a player does; the summary names the ones off in the game now.
    The folder the mod was opened from is this mod, whatever id it has now.
    A real override is a warning; changes that add up, agree, or follow an
    after/requires the winner declared are notes."""
    from pathlib import Path
    from . import manifest
    overlaps = _overlaps()
    folders = mod_folders(project) if folders is None else list(folders)
    settings = overlaps.read_settings(settings_file() if settings_path is None else settings_path) \
        if settings_path is not False else {}
    source_dir = getattr(project, "source_dir", None)
    mine = overlaps.Mod(project.info.id or "this-mod", project.info.name or project.info.id or "This mod",
                        manifest.build(project), source_dir)

    def same_folder(mod) -> bool:
        try:
            return source_dir is not None and Path(mod.directory).resolve() == Path(source_dir).resolve()
        except OSError:
            return False

    every = overlaps.installed(folders)
    others = [m for m in every if m.id != mine.id and not same_folder(m)]
    # Where the game finds this mod (which breaks a tie in the load order):
    # where its folder is, else after the rest.
    listed = [mine if same_folder(m) else m for m in every if same_folder(m) or m.id != mine.id]
    if not any(m is mine for m in listed):
        listed.append(mine)
    if not others:
        return [], "There are no other installed mods to check against (" + ", ".join(str(f) for f in folders) + ")."
    order = overlaps.load_order(listed, settings)
    left = [m.id for m in others if m not in order]
    if mine not in order:
        return [], ("This mod would not load: its \"requires\" names a mod that is not installed, or its "
                    "\"after\" and \"requires\" make a cycle.")
    place = order.index(mine)
    issues = []
    off = [m.id for m in others if m in order and not applied(m, settings)]
    for overlap in overlaps.check(order, _ProjectSource(project, settings, order), involving=place):
        where = overlap.label + (" (with a mod off now)" if any(m in off for m in overlap.mods) else "")
        message = overlap.text[len(overlap.label) + 1:]
        # The target: what the overlap is about (overlaps.KINDS) and its key,
        # for Conflicts to go to the tab that edits it.
        key = overlap.claims[0].key if overlap.claims else None
        issues.append(Issue("warning" if overlap.severity else "note", "Other mods", where, message,
                            (overlap.kind, key)))
    summary = f"Checked against {len(order) - 1} installed mods in " + ", ".join(str(f) for f in folders) + \
        f", in the order the game loads them; this mod loads {place + 1} of {len(order)}."
    if off:
        summary += " " + ", ".join(off) + (" is" if len(off) == 1 else " are") + " off in the game now."
    if left:
        summary += " Left out, as the game would not load them (a broken mod.json, a missing requirement or a " \
            "cycle): " + \
            ", ".join(left) + "."
    if source_dir is None:
        summary += " Files beside mod.json (pools, duelists, text, the pack) are read once the mod is saved."
    return issues, summary
