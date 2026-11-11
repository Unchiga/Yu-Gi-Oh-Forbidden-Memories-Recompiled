"""mod.json out of a Project (only what differs from retail), and back.

The schema is the one the port reads (notes/modding.md, notes/more-cards.md,
notes/gameplay-tables.md; src/pc/mods/mods.c, src/pc/cards/cards.c and
tables.c, starter.c and packs.c): "cards" (replace and copy), "fusions",
"equips", "rituals", "drops", "decks", "starter", "packs", "pack_shop" and
"duelists". Every other top-level key a mod has (data, text, textures, audio,
library, requires...) is kept as it was written.

The duelists a mod adds (notes/more-duelists.md) are roster.py's: its own,
from "duelists" and the duelists/, decks/, drops/ and portraits/ folders,
are read and written back as editable duelists. A "decks" or "drops" entry
naming a duelist neither the disc nor this mod has -- another mod's -- or
either table given as the name of a file, is kept as it was written.
"""
from __future__ import annotations

import copy
import json
import os
import re
import shutil
from pathlib import Path

from .gamedata import (FUSION_GROUPS, RITUAL_ORIGINS, RITUAL_REQUIREMENT_KEYS, RITUAL_TRIBUTE_MAX, fusion_group_named, ATTRIBUTE_NAMES, CARD_COUNT, DECK_SIZE, DUELIST_NAMES, FRAME_COLOR_NAMES, FRAME_NAMES, POOLS, STAR_NAMES,
                       EQUIP_BONUS_MAX, STARTER_WEIGHT_LIMIT, TYPE_NAMES, TYPE_MAGIC, TYPE_LIMIT, GameData, is_monster_type)
from .model import AddedCard, ModInfo, Project, StarterDeck, duelist_named, same_letters, KEY_RE
from . import art, board_art, campaign_map, compat, fixed_decks, guardian_stars, packs as packmath, pools as poolmath, roster

INFO_KEYS = ("id", "name", "version", "author", "description")

# The switch the game gates a mod's whole "card_layout" on (card_layout.c,
# full_bleed_of): a bool the mod declares. Without it the layout never applies,
# so the Frames page declares it, on, with the first frame style.
FULL_BLEED_KEY = "full_bleed"
FULL_BLEED_SETTING = {"key": FULL_BLEED_KEY, "label": "Custom card frame", "type": "bool", "default": 1,
                      "description": "Draw this mod's card frames and layout instead of the game's own."}


def declares_full_bleed(project: Project) -> bool:
    return any(isinstance(s, dict) and s.get("key") == FULL_BLEED_KEY for s in project.info.settings)


def declare_full_bleed(project: Project) -> bool:
    """Add the "full_bleed" setting, on by default, unless the mod declares
    one already (whatever its default: that is the modder's to choose).
    Returns whether it was added."""
    if declares_full_bleed(project):
        return False
    project.info.settings = [*project.info.settings, dict(FULL_BLEED_SETTING)]
    return True
TABLE_KEYS = ("settings", "cards", "fusions", "equips", "rituals", "drops", "decks", "starter", "packs", "pack_shop",
              "duelists")
REPLACE_EXTRA = ("art", "thumbnail", "title", "model", "effect", "exodia")
POOL_ALIASES = {"deck": "deck", "pow": "pow", "sapow": "pow", "bcd": "bcd", "tec": "tec", "satec": "tec"}


# --- writing ----------------------------------------------------------------

def password_text(value):
    """A "password" as the port reads it (cards.c read_password, tables.c
    read_shop_password): up to eight digits as a string or a number, "" or
    null for none. Its 8 digits, "" for none, or None for anything else
    ("card number", a letter...), which the editor keeps as written."""
    if value is None:
        return ""
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return f"{value:08d}" if 0 <= value <= 99999999 else None
    if isinstance(value, str):
        if not value:
            return ""
        return value.zfill(8) if len(value) <= 8 and value.isdigit() and value.isascii() else None
    return None


def _type_value(t: int, project=None):
    names = getattr(project, "type_names", TYPE_NAMES) if project is not None else TYPE_NAMES
    return names[t] if 0 <= t < len(names) and names[t] else (TYPE_NAMES[t] if 0 <= t < len(TYPE_NAMES) else t)


def _attribute_value(a: int):
    return ATTRIBUTE_NAMES[a] if 0 <= a < len(ATTRIBUTE_NAMES) else a


def _star_value(s: int):
    return STAR_NAMES[s] if 1 <= s < len(STAR_NAMES) else s


def _card_fields(card, base, everything=False, project=None) -> dict:
    out = {}
    if everything or card.name != base.name:
        out["name"] = card.name
    if everything or card.description != base.description:
        out["description"] = card.description
    if everything or card.attack != base.attack:
        out["attack"] = card.attack
    if everything or card.defense != base.defense:
        out["defense"] = card.defense
    if everything or card.type != base.type:
        out["type"] = _type_value(card.type, project)
    if everything or card.attribute != base.attribute:
        out["attribute"] = _attribute_value(card.attribute)
    if everything or card.level != base.level:
        out["level"] = card.level
    # A card made a monster without "stars" gets default ones in the game
    # (cards.c default_stars): written, even as none, they stay as shown.
    if everything or (card.star1, card.star2) != (base.star1, base.star2) or \
            is_monster_type(card.type) and not is_monster_type(base.type):
        out["stars"] = [_star_value(card.star1), _star_value(card.star2)]
    if card.frame != base.frame:
        out["frame"] = FRAME_NAMES[card.frame] if 0 <= card.frame < len(FRAME_NAMES) else "Type"
    return out


def build_cards(project: Project) -> list:
    entries = []
    for cid in sorted(project.retail.cards):
        card, retail = project.cards[cid], project.retail.cards[cid]
        fields = _card_fields(card, retail, project=project)
        for name, value in project.text_cards.get(cid, {}).items():
            if fields.get(name) == value:
                del fields[name]        # the mod's text file carries it, codes and all
        extra = project.card_extra.get(cid, {})
        notes = project.notes.get(cid)
        if fields or extra or notes:
            # With "notes" alone the entry only notes the card (cards.c).
            entry = {"replace": cid}
            entry.update(fields)
            entry.update(extra)
            if notes:
                entry["notes"] = notes
            entries.append(entry)
    for cid in sorted(project.added):
        added = project.added[cid]
        card = project.cards[cid]
        base = project.cards[added.base]
        entry = {"copy": added.base, "id": added.key}
        fields = {"name": card.name}
        fields.update(_card_fields(card, base, project=project))     # what is left out is the base's, as the port has it
        if not card.is_monster() and card.type != base.type:
            effect = project.retail.cards.get(project.effect_of(cid))
            # The port refuses a copy made another non-monster without a
            # matching effect, but for an equip, which needs none; any card
            # may become a monster (cards.c).
            if card.type != 23 and (effect is None or effect.type != card.type):
                fields.pop("type", None)
        entry.update(fields)
        if project.passwords.get(cid):
            entry["password"] = project.passwords[cid]
        if not added.drops:
            entry["drops"] = False
        if added.opponents:
            entry["opponents"] = True
        entry.update(added.extra)
        if project.notes.get(cid):
            entry["notes"] = project.notes[cid]
        entries.append(entry)
    return entries


def build_fusions(project: Project) -> list:
    """The mod's removes first, then a rule per pair that differs. A pair
    the remove takes away is left to it; one of its disc recipes the mod
    keeps (or changes) is written, as the remove would take it away too.
    A rule the mod wrote (or an own "fusions" list needs) is written even
    where the result alone needs none, as it comes before such a list."""
    rules = [{"remove": "all"}] if project.fusion_remove_all else []
    rules += [{"remove": project.ref(result)} for result in project.active_removes()]
    active = project.removed_results()
    retail = project.retail.fusions
    for pair in sorted(set(retail) | set(project.fusions) | project.fusion_explicit):
        now = project.fusions.get(pair)
        if not project.fusion_rule(pair, now, active):
            continue
        rules.append({"with": [project.ref(pair[0]), project.ref(pair[1])],
                      "result": project.ref(now) if now else None})
    return rules + project.kept["fusions"]


def build_equips(project: Project) -> list:
    """The disc's equips first, by retail cards (with types where that is
    shorter); then what the added cards need, checked against the game's
    own reading of those rules (equip_rules/simulate_equips)."""
    entries = []
    retail = {e: set(m) for e, m in project.retail.equips.items()}
    monsters = set(project.monsters())
    for equip in sorted(e for e in set(retail) | set(project.equips)
                        if e <= CARD_COUNT and project.cards[e].type == 23):
        before = retail.get(equip, set()) & monsters
        after = {m for m in project.equips.get(equip, set()) if m <= CARD_COUNT and m in monsters}
        if before == after:
            continue
        add, remove = after - before, before - after
        entry = {"card": project.ref(equip)}
        if before and len(after) < len(add) + len(remove):
            entry["replace"] = True
            entry["add"] = [project.ref(m) for m in sorted(after)]
            entries.append(entry)
            continue
        # A whole monster type allowed (or taken away) is written as the type;
        # within an entry a named card still goes over its type.
        add_types, remove_types = [], []
        for t in range(TYPE_MAGIC):
            members = {cid for cid, card in project.cards.items() if card.type == t and cid <= CARD_COUNT}
            if project.resolve(TYPE_NAMES[t]):
                continue        # a card of that name: the port would read the card
            if not members:
                continue
            if len(members & add) >= 3 and len(members - after) < len(members & add):
                add_types.append(TYPE_NAMES[t])
                add -= members
                remove |= members - after       # the type's cards it may not equip, named
            elif len(members & remove) >= 3 and len(members & after) < len(members & remove):
                remove_types.append(TYPE_NAMES[t])
                remove -= members
                add |= members & after
        if add or add_types:
            entry["add"] = add_types + [project.ref(m) for m in sorted(add)]
        if remove or remove_types:
            entry["remove"] = remove_types + [project.ref(m) for m in sorted(remove)]
        entries.append(entry)
    entries += _equip_fixes(project, entries)
    # The Cards tab's ATK and DEF boosts, before the kept "bonus_if" entries that
    # come after it in the mod (_read_equip_bonus).
    for equip, (attack, defense) in sorted(project.equip_bonus.items()):
        if equip in project.cards and project.cards[equip].type == 23:
            entries.append({"card": project.ref(equip), "bonus": attack} if attack == defense else
                           {"card": project.ref(equip), "bonus_attack": attack, "bonus_defense": defense})
    return entries + project.kept["equips"]


def _equip_fixes(project: Project, entries: list) -> list:
    """Entries after the disc's cards' own that make every equip, added
    cards included, fit what the project says. A rule for a card is also
    one for its copies, and the later entry decides (Tables_Equip), so:
    the retail monsters an entry got wrong, then the copies, each in an
    entry of its own; an added equip card gets a "replace" of its own
    whose removes, coming after its adds, take out a copy whose base it
    fits."""
    got = simulate_equips(project, equip_rules(project, entries, []))
    fixes = []
    for equip in sorted(got):
        # Changing a copied monster to magic/trap/ritual/equip can leave
        # its old membership in the editing sets. Such a card is no longer
        # an equip target; emitting an "add" for it also loses that rule
        # on reopen, since simulate_equips only considers monsters.
        want = project.equip_targets(equip)
        have = got[equip]
        if want == have:
            continue
        card = project.ref(equip)
        if equip in project.added:
            copies_out = sorted(m for m in project.added if m not in want and project.base_of(m) in want)
            entry = {"card": card, "replace": True, "add": [project.ref(m) for m in sorted(want)]}
            if copies_out:
                entry["remove"] = [project.ref(m) for m in copies_out]
            fixes.append(entry)
            continue
        for retail_pass in (True, False):
            wrong = {m for m in want ^ have if (m <= CARD_COUNT) == retail_pass}
            if retail_pass and not wrong:
                continue
            if not retail_pass:     # the retail fix may have moved copies too
                have = simulate_equips(project, equip_rules(project, entries + fixes, []))[equip]
                wrong = {m for m in want ^ have if m > CARD_COUNT}
                if not wrong:
                    continue
            entry = {"card": card}
            if wrong & want:
                entry["add"] = [project.ref(m) for m in sorted(wrong & want)]
            if wrong - want:
                entry["remove"] = [project.ref(m) for m in sorted(wrong - want)]
            fixes.append(entry)
    return fixes


def build_rituals(project: Project) -> list:
    entries = []
    retail = project.retail.rituals
    for ritual in sorted(set(retail) | set(project.rituals)):
        now = project.rituals.get(ritual)
        if retail.get(ritual) == now and ritual not in project.ritual_requirements and \
                ritual not in project.ritual_from and not (now is None and project.ritual_removed(ritual)):
            continue
        if now is None:     # a disc ritual's taken away, or an added copy's (its base's too)
            entries.append({"card": project.ref(ritual), "result": None})
            continue
        entry = {"card": project.ref(ritual)}
        if ritual in project.ritual_from:
            entry["tributes_from"] = project.ritual_from[ritual]
        requirements = project.ritual_requirements.get(ritual)
        if requirements:
            tributes = []
            for req in requirements:
                body = dict(req)
                if body.get("card"):
                    body["card"] = project.ref(body["card"])
                else:
                    body.pop("card", None)
                tributes.append(body)
            entry["tributes"] = tributes
        else:
            entry["tributes"] = [project.ref(t) for t in now[:-1]]
        entry["result"] = project.ref(now[-1])
        entries.append(entry)
    return entries + project.kept["rituals"]


def _duelist_key(d: int) -> str:
    return DUELIST_NAMES[d] if d else "0"


def _pool_body(project: Project, listed: dict, replace: bool, kept: dict) -> dict:
    body = {"replace": True} if replace else {}
    for cid, weight in sorted(listed.items(), key=lambda item: (-item[1], item[0])):
        body[str(project.ref(cid))] = weight
    body.update(kept)
    return body


def _pool_edits(project: Project, pool: str):
    """{duelist key: body} for one kind of pool: a disc duelist's by its
    name, a copy the mod adds by its roster entry (its own file). An edit
    that turns every opponent's pool into the edited one (say, one card
    taken out everywhere) is written once, as "all", before the opponents'
    own; "all" reaches the mod's own duelists too, as it does in the game."""
    deck = pool == "deck"
    subjects = roster.subjects(project)
    count = len(subjects)
    retail = [roster.retail_pool(project, s, pool) for s in subjects]
    edited = [{c: w for c, w in roster.pools_of(project, s)[pool].items() if w} for s in subjects]
    edits = [poolmath.edit_for(retail[d], edited[d], deck) for d in range(count)]
    out = {}
    bases = retail
    candidates = {}
    for edit in edits:
        if edit and not edit[1]:
            key = tuple(sorted(edit[0].items()))
            candidates[key] = candidates.get(key, 0) + 1
    for key, uses in sorted(candidates.items(), key=lambda item: -item[1]):
        if uses < 2:
            break
        listed = dict(key)
        after = [poolmath.apply_edit(retail[d], listed, False, deck) for d in range(count)]
        if any(a is None for a in after):
            continue
        later = [poolmath.edit_for(after[d], edited[d], deck) for d in range(count)]
        if sum(1 for e in later if e) + 1 < sum(1 for e in edits if e):
            out["all"] = _pool_body(project, listed, False, {})
            bases, edits = after, later
        break
    kept_all = project.kept_pools.get(("all", pool), {})
    if kept_all:
        body = out.get("all", {})
        body.update(kept_all)
        out["all"] = body
    for d, s in enumerate(subjects):
        kept = roster.kept_of(project, s, pool)
        if edits[d] is None and not kept:
            continue
        listed, replace = edits[d] if edits[d] else ({}, False)
        out[_duelist_key(s) if isinstance(s, int) else s] = _pool_body(project, listed, replace, kept)
    return out


def _pool_tables(project: Project):
    """(drops, decks) by duelist: mod.json's keys, and a copy's roster entry
    for what goes in its own files (roster.files)."""
    decks = _pool_edits(project, "deck")
    decks.update(fixed_decks.build(project))     # a fixed deck wins over weighted edits of it anyway
    if "all" in decks:
        decks = {"all": decks.pop("all"), **decks}
    drops = {}
    for pool in ("pow", "bcd", "tec"):
        for name, body in _pool_edits(project, pool).items():
            drops.setdefault(name, {})[pool] = body
    if "all" in drops:     # "all" first, so the opponents' own edits go over it
        drops = {"all": drops.pop("all"), **drops}
    return drops, decks


def build_pools(project: Project):
    drops, decks = _pool_tables(project)
    drops = {k: v for k, v in drops.items() if isinstance(k, str)}
    decks = {k: v for k, v in decks.items() if isinstance(k, str)}
    # Entries naming a duelist the editor could not place -- another mod's --
    # go back after the ones it wrote, where the mod had them.
    decks.update(project.kept_opponents.get("decks", {}))
    drops.update(project.kept_opponents.get("drops", {}))
    # A table the mod gave as a filename stays that filename: the editor read
    # nothing from it, so it has nothing of its own to write in its place.
    return project.pool_files.get("drops", drops), project.pool_files.get("decks", decks)


def build_starter(project: Project):
    """"starter": the decks a new game may be dealt, cards named as the rules
    name them. One deck is written as the object itself, as the port reads it;
    a deck's cards go in id order, and a card the editor could not place goes
    back after them, under the name it was written with."""
    if project.starter_file is not None:
        return project.starter_file
    out = []
    for deck in project.starter:
        entry = {}
        if deck.name:
            entry["name"] = deck.name
        if deck.weight != 1:
            entry["weight"] = deck.weight
        entry.update(deck.extra)
        for cid in sorted(deck.cards):
            if deck.cards[cid]:
                entry[str(project.ref(cid))] = deck.cards[cid]
        entry.update(deck.kept)
        out.append(entry)
    return out[0] if len(out) == 1 else out


def build_packs(project: Project):
    """"packs": the file the mod names, or each pack with only what differs
    from the defaults (packs.minimize); None for no packs."""
    if project.packs_file is not None:
        return project.packs_file
    return [packmath.minimize(entry) for entry in project.packs] or None


def build_passwords(project: Project):
    """"passwords" (gameplay-tables.md): the entries the mod had, with the
    disc cards' passwords the editor changed written into them. None when
    there is nothing to write, or when the mod's "passwords" is not an object
    (kept as written)."""
    kept = project.other.get("passwords")
    if kept is not None and not isinstance(kept, dict):
        return None
    table = copy.deepcopy(kept) if kept else {}
    for cid in sorted(project.passwords):
        if cid not in project.retail.cards and cid not in project.password_keys:
            continue
        key = project.password_keys.get(cid) or str(project.ref(cid))
        if not isinstance(table.get(key), dict):
            table[key] = {}
        table[key]["password"] = project.passwords[cid]
    for cid, price in project.starchips.items():
        # Remove every alias's old price, so an earlier name/number cannot
        # override the edit. Preserve passwords and unrelated entry fields.
        key = project.password_keys.get(cid) or str(project.ref(cid))
        for name, entry in list(table.items()):
            if same_all(name) or project.resolve(name) != cid:
                continue
            if isinstance(entry, dict):
                key = name
                entry.pop("starchips", None)
                entry.pop("starchips_percent", None)
                if not entry:
                    del table[name]
        if price is not None:
            if not isinstance(table.get(key), dict):
                table[key] = {}
            table[key]["starchips"] = price
    return table or None


def build(project: Project) -> dict:
    """The mod.json object: the mod's own keys, then only what differs."""
    info = project.info
    manifest = {"id": info.id, "name": info.name}
    for key in ("version", "author", "description"):
        if getattr(info, key):
            manifest[key] = getattr(info, key)
    manifest.update(project.other)
    # Starter pools by their cards as they are named now (the mod's id, an
    # added card's key may have changed since the Weighted pools page wrote
    # them), as the written decks are.
    if getattr(project, "starter_pool_state", None) is not None and \
            isinstance(project.other.get("starter_pools"), (list, dict)):
        from . import starter_pools as sp
        pools = sp.build(sp.state(project), project.ref)
        if pools is None:
            manifest.pop("starter_pools", None)
        else:
            manifest["starter_pools"] = pools
    passwords = build_passwords(project)
    if passwords:
        manifest["passwords"] = passwords
    elif isinstance(manifest.get("passwords"), dict):
        del manifest["passwords"]
    if info.settings:
        manifest["settings"] = info.settings
    cards = build_cards(project)
    if cards:
        manifest["cards"] = cards
    for key, builder in (("fusions", build_fusions), ("equips", build_equips), ("rituals", build_rituals)):
        rules = builder(project)
        if rules:
            manifest[key] = rules
    duelists = roster.manifest_value(project)
    if duelists is not None:
        manifest["duelists"] = duelists
    drops, decks = build_pools(project)
    if drops:
        manifest["drops"] = drops
    if decks:
        manifest["decks"] = decks
    starter = build_starter(project)
    if starter:
        manifest["starter"] = starter
    campaign_map.build_into(project, manifest)
    board_art.build_into(project, manifest)
    packs = build_packs(project)
    if packs:
        manifest["packs"] = packs
    rules = packmath.minimize_rules(project.pack_shop) if project.pack_shop is not None else None
    if rules:
        manifest["pack_shop"] = rules
    # A game older than a feature used would play the mod without it: the
    # mod says which game it needs (compat.py), and an older one refuses it.
    return compat.stamp(manifest, project)


def _format(value, indent: int) -> str:
    one_line = json.dumps(value, ensure_ascii=False)
    if not isinstance(value, (dict, list)) or not value:
        return one_line
    pad = " " * (indent + 4)
    # A rule, a card entry or a list of names reads best on one line if it fits.
    if len(one_line) + indent <= 110 and (isinstance(value, list) or indent >= 8):
        return one_line
    if isinstance(value, list):
        items = [pad + _format(item, indent + 4) for item in value]
        return "[\n" + ",\n".join(items) + "\n" + " " * indent + "]"
    # A key is a string in JSON: a card named by its number (480, Kuwagata α,
    # whose name does not name it back) was written 480: and the game read
    # no mod.json at all.
    items = [pad + json.dumps(str(key), ensure_ascii=False) + ": " + _format(item, indent + 4)
             for key, item in value.items()]
    return "{\n" + ",\n".join(items) + "\n" + " " * indent + "}"


def dumps(manifest: dict) -> str:
    """JSON laid out as the example mods are: four spaces, and each rule or
    card entry on a line of its own when it fits."""
    return _format(manifest, 0) + "\n"


# --- the card texts a text listing carries ---------------------------------------

ITEM_START = re.compile(r"^(\[[0-9A-Fa-f ]+\]|\{:L[0-9A-Fa-f]{4}\})")


def listing_items(listing: str) -> dict:
    """{bank: {item key: text}} of a text listing (tools/pc/text_listing.py),
    comments left out; an item's text starts with its key line."""
    banks, bank, key = {}, None, None
    for line in listing.split("\n"):
        if line.startswith("@bank "):
            bank, key = line.split()[1], None
            banks.setdefault(bank, {})
            continue
        if bank is None:
            continue
        match = ITEM_START.match(line)
        if match:
            key = match.group(1)
            banks[bank][key] = [line.split("#")[0].rstrip() if line.startswith("[") else line]
            continue
        if key is not None:
            banks[bank][key].append(line)
    for items in banks.values():
        for lines in items.values():
            while len(lines) > 1 and (not lines[-1].strip() or lines[-1].startswith("#")):
                lines.pop()             # the comments and blank lines before the next item
    return {b: {k: "\n".join(v) for k, v in items.items()} for b, items in banks.items()}


def item_ids(key: str) -> list:
    return [int(i, 16) for i in key[1:-1].split()] if key.startswith("[") else []


def listing_plain(item: str) -> str:
    """An item's text as the editor shows it: the key line and the end
    left out, line breaks as lines; color and icon codes kept as spelled."""
    body = item.split("\n", 1)[1] if "\n" in item else ""
    for code in ("{end}", "{cont}"):
        body = body.replace(code, "")
    return body.replace("{nl}", "\n").replace("{sp}", " ").strip("\n")


def plain_name(text: str) -> str:
    return re.sub(r"\{[^{}]*\}", "", listing_plain(text)).strip()


def card_texts(listing: str) -> dict:
    """{(card id, "name" or "description"): text as the editor shows it} for
    the card names and texts a listing carries."""
    out = {}
    items = listing_items(listing)
    for bank, first, field, plain in (("names", 0x8000, "name", plain_name),
                                      ("descriptions", 0xD100, "description", listing_plain)):
        for key, item in items.get(bank, {}).items():
            for i in item_ids(key):
                if 1 <= i - first <= CARD_COUNT:
                    out[(i - first, field)] = plain(item)
    return out


def read_text_cards(project: Project, folder: Path, messages: list):
    """Cards whose name or text the mod's "text" file carries (an imported
    mod's colored names, texts with codes, texts empty on purpose): shown
    in the editor, and left to the file while unchanged."""
    name = project.other.get("text")
    if not isinstance(name, str) or not (folder / name).is_file():
        return
    try:
        listing = (folder / name).read_text(encoding="utf-8-sig")
    except (OSError, UnicodeDecodeError) as problem:
        messages.append(f"{name}: not read ({problem})")
        return
    for (cid, field), value in card_texts(listing).items():
        if getattr(project.cards[cid], field) == getattr(project.retail.cards[cid], field):   # cards[] says nothing
            setattr(project.cards[cid], field, value)
            project.text_cards.setdefault(cid, {})[field] = value


# --- reading ----------------------------------------------------------------

def _choice(value, names):
    """cards.c choice(): a number, or one of the names in any case; -1."""
    if isinstance(value, bool):
        return -1
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        for i, name in enumerate(names):
            if name and value.lower() == name.lower():
                return i
        # choice() falls back to Json_Number for an unmatched string, which
        # reads "0x18" as hex the same as a plain number (json.c); _number
        # already does that.
        return _number(value)
    return -1


def _type_choice(project, value):
    """The runtime type resolver, including retail aliases after a rename."""
    names = getattr(project, "type_names", TYPE_NAMES)
    choice = _choice(value, names)
    # cards.c accepts a numeric choice only when the slot is registered.
    if 0 <= choice < len(names) and names[choice]:
        return choice
    if isinstance(value, str):
        choice = next((i for i, name in enumerate(names) if name and same_letters(value, name)), -1)
        if choice >= 0:
            return choice
        choice = next((i for i, name in enumerate(TYPE_NAMES) if same_letters(value, name)), -1)
        if choice >= 0 and choice < len(names) and names[choice]:
            return choice
    return -1


def read_card_types(project: Project, section, messages: list):
    """Read the registry before cards so a card may name a new type."""
    if section is None:
        return
    if not isinstance(section, dict) or not isinstance(section.get("types"), list):
        messages.append("\"card_types\" is an object with a \"types\" list; left out")
        return
    for index, entry in enumerate(section["types"]):
        where = f"card_types.types[{index}]"
        if not isinstance(entry, dict) or type(entry.get("id")) is not int or not isinstance(entry.get("name"), str):
            messages.append(f"{where}: an object with numeric \"id\" and string \"name\"; left out")
            continue
        type_, name = entry["id"], entry["name"]
        if not (0 <= type_ < TYPE_LIMIT) or not name:
            messages.append(f"{where}: id is 0-23 or 24-255 with a non-empty name; left out")
            continue
        normalized = "".join(c.lower() for c in name if c.isascii() and c.isalnum())
        # Match encode_type_name(): a name with no glyph the game can show
        # is never registered, the same as an empty one -- the editor's
        # model must not hold a type the runtime would refuse to build, or
        # a card assigned to it here would point at nothing there.
        if not normalized:
            messages.append(f'{where}: "{name}" has no glyph the game can display; declaration left out')
            continue
        # Match add_card_types_section(): a later declaration may replace its
        # own ID, but must not shadow another active spelling or a retail
        # alias retained after that retail type was renamed.
        collision = next((other for other, other_name in enumerate(project.type_names)
                          if other != type_ and other_name and
                          "".join(c.lower() for c in other_name if c.isascii() and c.isalnum()) == normalized), None)
        if collision is None:
            collision = next((other for other, other_name in enumerate(TYPE_NAMES)
                              if other != type_ and
                              "".join(c.lower() for c in other_name if c.isascii() and c.isalnum()) == normalized), None)
        if collision is not None:
            messages.append(f'{where}: "{name}" is already type {collision}\'s name or retail alias; declaration left out')
            continue
        project.type_names[type_] = name


def _number(value, default=-1):
    """json.c Json_Number: a whole number (2048.0 is one), true as 1, or a
    number written as a string."""
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value) if value.is_integer() else default
    if isinstance(value, str):
        text = value.strip().lower()
        try:
            return int(text, 16) if text.startswith("0x") else int(text)
        except ValueError:
            return default
    return default


def _clamp(value, low, high):
    return max(low, min(high, value))


def _attribute_letters(value: str) -> bytes:
    """attributes.c same_letters(): punctuation skipped, ASCII folded only.

    The runtime compares UTF-8 bytes rather than Unicode case-folded
    characters. Keep that deliberately narrow rule here so the editor never
    resolves a declaration differently from the game.
    """
    return bytes(c for c in value.encode("utf-8")
                 if 48 <= c <= 57 or 65 <= c <= 90 or 97 <= c <= 122 or c >= 0x80).lower()


def _attribute_name_has_letters(value: str) -> bool:
    return bool(_attribute_letters(value))


def _attribute_numeric_name(value: str) -> bool:
    folded = _attribute_letters(value)
    return bool(folded) and all(48 <= c <= 57 for c in folded)


def _attribute_registry(section):
    """The declarations Attributes_Add() would accept from this manifest.

    The editor loads one mod manifest at a time, so this deliberately models
    that manifest's registry, including its declaration order. Runtime mods
    may additionally provide declarations from earlier active mods.
    """
    names = list(ATTRIBUTE_NAMES) + [""] * (256 - len(ATTRIBUTE_NAMES))
    declared = set()
    entries = section.get("attributes") if isinstance(section, dict) else None
    if not isinstance(entries, list):
        return names, declared
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        ident, name = entry.get("id"), entry.get("name")
        if type(ident) is not int or not isinstance(name, str) or not name:
            continue
        if not 0 <= ident <= 255 or ident in (6, 7):
            continue
        if not _attribute_name_has_letters(name) or _attribute_numeric_name(name):
            continue
        duplicate = any(i != ident and _attribute_letters(name) == _attribute_letters(retail)
                        for i, retail in enumerate(ATTRIBUTE_NAMES))
        duplicate = duplicate or any(i != ident and existing and _attribute_letters(name) == _attribute_letters(existing)
                                     for i, existing in enumerate(names))
        if duplicate:
            continue
        names[ident] = name
        declared.add(ident)
    return names, declared


def _attribute_choice(value, section) -> int:
    """The runtime-valid retail or declared attribute represented by value."""
    names, declared = _attribute_registry(section)
    found = -1
    if type(value) is int:
        found = value
    elif isinstance(value, str):
        # Attributes_Find uses strtol(..., 10) before names. Python's int()
        # accepts trailing whitespace, which strtol does not, so keep the
        # spelling rule explicit.
        if re.fullmatch(r"[ \t\n\r\f\v]*[+-]?[0-9]+", value):
            found = int(value, 10)
        else:
            folded = _attribute_letters(value)
            for i, name in enumerate(ATTRIBUTE_NAMES):
                if folded == _attribute_letters(name) or folded == _attribute_letters(names[i]):
                    found = i
                    break
            if found < 0:
                for i in range(len(ATTRIBUTE_NAMES), len(names)):
                    if names[i] and folded == _attribute_letters(names[i]):
                        found = i
                        break
    # 6 and 7 are the retail magic/trap attributes: never declarable, but a
    # card may carry them as a number (the editor writes them for a
    # Magic/Trap card), as the game takes them (cards.c add_entry).
    if found in (6, 7):
        return found if type(value) is int else -1
    return found if 0 <= found < len(ATTRIBUTE_NAMES) or found in declared else -1


def _apply_fields(project: Project, card, entry: dict, is_replace: bool, messages: list, where: str,
                  stars_section=None, attributes_section=None, effect_type=None):
    name = entry.get("name")
    if isinstance(name, str) and name:
        card.name = name
    text = entry.get("description")
    if isinstance(text, str) and text:
        card.description = text
    value = _number(entry.get("attack"))
    if value >= 0:
        card.attack = _clamp(value // 10, 0, 0x1FF) * 10
    value = _number(entry.get("defense"))
    if value >= 0:
        card.defense = _clamp(value // 10, 0, 0x1FF) * 10
    if "type" in entry:
        value = _type_choice(project, entry["type"])
        if value >= 0:
            value = _clamp(value, 0, TYPE_LIMIT - 1)
            if (not is_replace and not is_monster_type(value) and value != card.type and value != effect_type and
                    value != 23):     # cards.c
                messages.append(f"{where}: a copy becomes a non-monster only with a matching retail effect; "
                                f"\"type\" left out")
            else:
                card.type = value
    stars = entry.get("stars")
    if "stars" in entry and not (isinstance(stars, list) and len(stars) == 2):
        messages.append(f"{where}: \"stars\" is a list of two, [first, second] (none for no star); left out")
    elif "stars" in entry:
        # A number, the disc's names, a name the mod's "guardian_stars" gives
        # or none (0, null, "none", "(none)"), up to the card record's 15
        # (stars.c Stars_Value). [none, X] is kept as written: the game reads
        # it as the one star X, which validate says.
        first, second = (guardian_stars.card_star(stars[0], stars_section),
                         guardian_stars.card_star(stars[1], stars_section))
        if first > guardian_stars.MAX_STARS or second > guardian_stars.MAX_STARS:
            messages.append(f"{where}: a card holds a guardian star in 4 bits: 15 at most")
        if first == -1 or second == -1:
            messages.append(f"{where}: \"stars\": not a guardian star; left out")
        if first >= 0:
            card.star1 = _clamp(first, 0, guardian_stars.MAX_STARS)
        if second >= 0:
            card.star2 = _clamp(second, 0, guardian_stars.MAX_STARS)
    value = _number(entry.get("level"))
    if value >= 0:
        card.level = _clamp(value, 0, 12)
    if "attribute" in entry:
        value = _attribute_choice(entry["attribute"], attributes_section)
        if value >= 0:
            card.attribute = _clamp(value, 0, 255)
        else:
            messages.append(f"{where}: \"attribute\" is a retail attribute, or an id/name this mod's own "
                            f"\"attributes\" declares; left out")
    if "frame" in entry:
        value = _choice(entry["frame"], FRAME_NAMES + ["Type"])
        if not 0 <= value <= len(FRAME_NAMES):
            # Gold, Green, Pink and Blue: Monster, Magic, Trap and Ritual by their color.
            value = _choice(entry["frame"], FRAME_COLOR_NAMES)
            if value >= len(FRAME_COLOR_NAMES):
                value = -1
        if 0 <= value <= len(FRAME_NAMES):
            card.frame = -2 if value == len(FRAME_NAMES) else value  # -2: "Type", never orange
        else:
            messages.append(f"{where}: \"frame\" is Gold, Green, Pink, Blue, Purple, Orange or Type (or the disc's "
                            f"Monster, Magic, Trap, Ritual); left out")


def _base_id(project: Project, value) -> int:
    if isinstance(value, str) and value and not value[0].isdigit():
        return project.names.find(value)
    return _number(value, 0)


def _read_notes(entry: dict, messages: list, where: str):
    """An entry's "notes": its text, or None when it has none the editor
    shows (not text: kept as written, as the game leaves it out)."""
    if "notes" not in entry:
        return None
    if isinstance(entry["notes"], str):
        return entry["notes"]
    messages.append(f"{where}: \"notes\" must be text; kept as written")
    return None


def _default_stars(project: Project, card, entry: dict, was_monster: bool, model_default: int):
    """As cards.c default_stars: a card made a monster with no "stars" and
    none of its own takes its model's, or the Sun and the Moon."""
    if not is_monster_type(card.type) or was_monster or "stars" in entry or card.star1 or card.star2:
        return
    model = project.resolve(entry.get("model")) if "model" in entry else model_default
    source = project.retail.cards.get(model) if 1 <= model <= CARD_COUNT else None
    if source is not None and is_monster_type(source.type):
        card.star1, card.star2 = source.star1, source.star2
    else:
        card.star1, card.star2 = STAR_NAMES.index("Sun"), STAR_NAMES.index("Moon")


def read_cards(project: Project, entries, messages: list):
    if entries is None:
        return
    if not isinstance(entries, list):
        messages.append("\"cards\" is not an array; left out")
        return
    for index, entry in enumerate(entries):
        where = f"cards[{index}]"
        if not isinstance(entry, dict):
            messages.append(f"{where} is not an object; left out")
            continue
        is_replace = "replace" in entry
        base = _base_id(project, entry.get("replace") if is_replace else entry.get("copy"))
        if not 1 <= base <= CARD_COUNT:
            messages.append(f"{where}: \"{'replace' if is_replace else 'copy'}\" must name a card of the disc, 1 to 722")
            continue
        notes = _read_notes(entry, messages, where)
        if is_replace:
            was_monster = is_monster_type(project.cards[base].type)
            _apply_fields(project, project.cards[base], entry, True, messages, where, project.other.get("guardian_stars"),
                          project.other.get("attributes"))
            _default_stars(project, project.cards[base], entry, was_monster, base)
            if notes:
                had = project.notes.get(base)
                project.set_notes(base, f"{had}\n{notes}" if had else notes)
            # What the editor does not show (art, password...) is kept as written.
            shown = ("replace", "name", "description", "attack", "defense", "type", "attribute", "level", "stars",
                     "frame")
            if notes is not None:
                shown += ("notes",)
            extra = {k: v for k, v in entry.items() if k not in shown}
            if extra:
                project.card_extra.setdefault(base, {}).update(extra)
            continue
        key = entry.get("id")
        if not isinstance(key, str) or not key:
            key = f"entry-{index}"
        if len(key) > 80 or not KEY_RE.match(key):
            messages.append(f"{where}: invalid stable id; left out")
            continue
        if any(a.key == key for a in project.added.values()):
            messages.append(f"{where}: duplicate card identity {key}; left out")
            continue
        cid = project.add_card(base, key)
        # A copy with no name of its own shows its base's name from the disc,
        # not the name a "replace" gave the base (cards.c Cards_NameCodes).
        project.cards[cid].name = project.retail.cards[base].name
        effect_id = project.resolve(entry.get("effect"))
        if not effect_id or effect_id > CARD_COUNT:
            effect_id = project.effect_of(base)
        effect = project.retail.cards.get(effect_id)
        was_monster = is_monster_type(project.cards[cid].type)
        _apply_fields(project, project.cards[cid], entry, False, messages, where, project.other.get("guardian_stars"),
                      project.other.get("attributes"), effect.type if effect else None)
        _default_stars(project, project.cards[cid], entry, was_monster, base)
        added = project.added[cid]
        added.drops = _json_bool(entry.get("drops"), True)
        added.opponents = _json_bool(entry.get("opponents"), False)
        skip = ("copy", "id", "name", "description", "attack", "defense", "type", "attribute", "level", "stars",
                "frame", "drops", "opponents")
        if notes is not None:
            skip += ("notes",)
            project.set_notes(cid, notes)
        password = password_text(entry.get("password")) if "password" in entry else None
        if password is not None:
            skip += ("password",)
            project.set_password(cid, password)
        elif "password" in entry:
            messages.append(f"{where}: \"password\" is up to 8 digits, or \"\" for none; kept as written")
        added.extra = {k: v for k, v in entry.items() if k not in skip}
        if _number(entry.get("count"), 1) != 1 or entry.get("count_setting"):
            messages.append(f"{where}: adds several cards (\"count\"); the editor shows the first and keeps the count")


def read_fusions(project: Project, rules, messages: list):
    if rules is None:
        return
    if not isinstance(rules, list):
        messages.append("\"fusions\" is not an array; left out")
        return
    set_rules, removed, remove_all = {}, [], False
    project._own_pairs = None           # read_cards has read the own "fusions" lists
    for i, rule in enumerate(rules):
        where = f"fusions[{i}]"
        if not isinstance(rule, dict):
            messages.append(f"{where} is not an object; left out")
            continue
        if "setting" in rule:     # switched by the mod's settings: the editor shows the disc's table
            messages.append(f"{where}: switched by setting {rule['setting']!r}; kept as written")
            project.kept["fusions"].append(rule)
            continue
        if "remove" in rule:
            cid = project.resolve(rule["remove"])
            if project.removes_all(rule["remove"]):
                remove_all = True
            elif cid:
                removed.append(cid)
            else:
                messages.append(f"{where}: no card {rule['remove']!r}; kept as written")
                project.kept["fusions"].append(rule)
            continue
        with_ = rule.get("with")
        if not isinstance(with_, list) or len(with_) != 2:
            messages.append(f"{where}: \"with\" names two cards; left out")
            continue
        a, b = project.resolve(with_[0]), project.resolve(with_[1])
        result = rule.get("result", "missing")
        if result == "missing":
            messages.append(f"{where}: \"result\" is a card, or null to forbid the fusion; left out")
            continue
        made = 0 if result is None or result == 0 else project.resolve(result)
        if not a or not b or (result not in (None, 0) and not made):
            messages.append(f"{where}: names a card the editor cannot place; kept as written")
            project.kept["fusions"].append(rule)
            continue
        set_rules[Project.pair(a, b)] = made
    # The removes first: a rule of the mod's for one of the pairs still
    # makes the card (Tables_Fusion is asked before the filtered disc table).
    if remove_all:
        project.remove_disc_fusions()
    for result in removed if not remove_all else ():
        project.remove_recipes(result)
    for pair, made in set_rules.items():
        project.set_fusion(pair[0], pair[1], made)
        project.fusion_explicit.add(pair)


def _json_bool(value, default: bool) -> bool:
    """json.c Json_Bool: true/false, a number (not 0), else the default."""
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    return default


EQUIP_ANY, EQUIP_TYPE, EQUIP_CARD = 0, 1, 2


def _read_equip_bonus(project: Project, equip: int, entry: dict, kept: list):
    """An entry's "bonus" ("bonus_attack", "bonus_defense") into the Cards
    tab's ATK and DEF boosts; "bonus_if" (and a bonus the game would refuse)
    kept as written. For each of ATK and DEF the latest entry that fits a
    monster decides (Tables_EquipBonuses), so one setting both leaves an
    earlier kept "bonus_if" of the card's nothing to do."""
    def points(key):
        value = entry.get(key)
        return value if type(value) is int and -EQUIP_BONUS_MAX <= value <= EQUIP_BONUS_MAX else None

    keys = ("bonus", "bonus_attack", "bonus_defense")
    if any(key in entry and points(key) is None for key in keys):
        rest = {k: entry[k] for k in keys + ("bonus_if",) if k in entry}
    else:
        attack, defense = project.equip_bonus_of(equip)
        if "bonus" in entry:
            attack = defense = entry["bonus"]
        attack = entry.get("bonus_attack", attack)
        defense = entry.get("bonus_defense", defense)
        if "bonus" in entry or ("bonus_attack" in entry and "bonus_defense" in entry):
            kept[:] = [k for k in kept if set(k) != {"card", "bonus_if"} or project.resolve(k["card"]) != equip]
        if any(key in entry for key in keys):
            project.equip_bonus[equip] = (attack, defense)
        rest = {"bonus_if": entry["bonus_if"]} if "bonus_if" in entry else {}
    if rest:
        kept.append({"card": entry.get("card"), **rest})


def equip_rules(project: Project, entries, messages: list, kept: list = None) -> list:
    """The equip rules the port makes of "equips" (tables.c read_equips):
    (equip, kind, target, allow, order), an entry's adds before its removes."""
    rules = []
    for i, entry in enumerate(entries):
        where = f"equips[{i}]"
        if not isinstance(entry, dict):
            messages.append(f"{where} is not an object; left out")
            continue
        if "setting" in entry:
            messages.append(f"{where}: switched by setting {entry['setting']!r}; kept as written")
            if kept is not None:
                kept.append(entry)
            continue
        order = i + 1
        equip = project.resolve(entry.get("card"))
        if not equip:
            messages.append(f"{where}: no card {entry.get('card')!r}; kept as written")
            if kept is not None:
                kept.append(entry)
            continue
        if project.cards[equip].type != 23:
            messages.append(f"{where}: \"card\" is not an equip card; left out")
            continue
        if kept is not None:
            _read_equip_bonus(project, equip, entry, kept)
        if _json_bool(entry.get("replace"), False):
            rules.append((equip, EQUIP_ANY, 0, False, order))
        unplaced = False
        for allow, key in ((True, "add"), (False, "remove")):
            targets = entry.get(key)
            for target in targets if isinstance(targets, list) else []:
                # tables.c checks a type name before a card reference.  Use
                # the project registry so custom types and retail aliases
                # behave exactly as they do in the game.
                t = _type_choice(project, target) if isinstance(target, str) else -1
                if t >= 0 and project.resolve(target) <= 0:
                    rules.append((equip, EQUIP_TYPE, t, allow, order))
                    continue
                cid = project.resolve(target)
                if cid:
                    rules.append((equip, EQUIP_CARD, cid, allow, order))
                else:
                    unplaced = True
        if unplaced:
            messages.append(f"{where}: names cards the editor cannot place; those are left out")
    return rules


def simulate_equips(project: Project, rules: list) -> dict:
    """{equip card: the monsters it fits} as Tables_Equip answers, with the
    disc's table by base ids where no rule says anything."""
    by_equip = {}
    for rule in rules:
        by_equip.setdefault(rule[0], []).append(rule)
    monsters = project.monsters()
    out = {}
    for equip in project.equip_cards():
        mine = by_equip.get(equip, []) + (by_equip.get(project.base_of(equip), [])
                                         if project.base_of(equip) != equip else [])
        mine.sort(key=lambda r: r[4])       # by order; within an entry as listed
        fits = set()
        for monster in monsters:
            base, kind_of = project.base_of(monster), project.cards[monster].type
            best = None
            for rule in mine:
                _, kind, target, allow, order = rule
                if kind == EQUIP_TYPE and target != kind_of:
                    continue
                if kind == EQUIP_CARD and target not in (monster, base):
                    continue
                if best is None or order > best[4] or (order == best[4] and kind >= best[1]):
                    best = rule
            if best[3] if best else project.equip_retail(equip, monster):
                fits.add(monster)
        out[equip] = fits
    return out


def read_equips(project: Project, entries, messages: list):
    if entries is None:
        return
    if not isinstance(entries, list):
        messages.append("\"equips\" is not an array; left out")
        return
    rules = equip_rules(project, entries, messages, project.kept["equips"])
    if rules:
        project.equips.update(simulate_equips(project, rules))


def read_rituals(project: Project, entries, messages: list):
    if entries is None:
        return
    if not isinstance(entries, list):
        messages.append("\"rituals\" is not an array; left out")
        return
    for i, entry in enumerate(entries):
        where = f"rituals[{i}]"
        if not isinstance(entry, dict):
            continue
        if "setting" in entry:
            messages.append(f"{where}: switched by setting {entry['setting']!r}; kept as written")
            project.kept["rituals"].append(entry)
            continue
        ritual = project.resolve(entry.get("card"))
        if not ritual or not project.is_ritual(ritual):
            messages.append(f"{where}: \"card\" must be a ritual card whose effect is a ritual's (a copy of one, "
                            "or \"effect\" naming one); left out")
            continue
        if "result" in entry and entry["result"] is None:
            project.remove_ritual(ritual)
            continue
        tributes = entry.get("tributes")
        if not isinstance(tributes, list) or not 1 <= len(tributes) <= RITUAL_TRIBUTE_MAX:
            messages.append(f"{where}: \"tributes\" names one to five monsters; left out")
            continue
        origin = entry.get("tributes_from", "field")
        if origin not in RITUAL_ORIGINS:
            messages.append(f"{where}: \"tributes_from\" is \"field\", \"hand\" or \"both\"; left out")
            continue
        result = project.resolve(entry.get("result"))
        conditional = any(isinstance(t, dict) for t in tributes)
        if conditional:
            requirements, display_ids, valid = [], [], bool(result)
            for tribute in tributes:
                if isinstance(tribute, dict):
                    req = {}
                    if any(key not in RITUAL_REQUIREMENT_KEYS for key in tribute):
                        valid = False   # a key of a later build: kept as written, not dropped
                    if "card" in tribute:
                        cid = project.resolve(tribute.get("card"))
                        if not cid:
                            valid = False
                        else:
                            req["card"] = cid
                    if "type" in tribute:
                        value = tribute["type"]
                        named = _type_choice(project, value)
                        if named < 0 or not is_monster_type(named):
                            valid = False
                        else:
                            req["type"] = _type_value(named, project)
                    if "fusion_group" in tribute:
                        value = fusion_group_named(tribute["fusion_group"])
                        if not value:
                            valid = False
                        else:
                            req["fusion_group"] = value
                    for key in ("min_attack", "min_defense", "max_attack", "max_defense"):
                        if key in tribute:
                            value = tribute[key]
                            if isinstance(value, bool) or not isinstance(value, (int, float)) or int(value) != value or not 0 <= int(value) <= 9999:
                                valid = False
                            else:
                                req[key] = int(value)
                    if (req.get("min_attack") is not None and req.get("max_attack") is not None
                            and req["min_attack"] > req["max_attack"]):
                        valid = False
                    if (req.get("min_defense") is not None and req.get("max_defense") is not None
                            and req["min_defense"] > req["max_defense"]):
                        valid = False
                    for key in ("min_level", "max_level"):
                        if key in tribute:
                            value = tribute[key]
                            if isinstance(value, bool) or not isinstance(value, (int, float)) or int(value) != value or not 0 <= int(value) <= 12:
                                valid = False
                            else:
                                req[key] = int(value)
                    if (req.get("min_level") is not None and req.get("max_level") is not None
                            and req["min_level"] > req["max_level"]):
                        valid = False
                    if "defense_gt_attack" in tribute:
                        if not isinstance(tribute["defense_gt_attack"], bool):
                            valid = False
                        elif tribute["defense_gt_attack"]:
                            req["defense_gt_attack"] = True
                    if not req:
                        valid = False
                    requirements.append(req)
                    display_ids.append(req.get("card", 0))
                else:
                    cid = project.resolve(tribute)
                    valid &= bool(cid)
                    requirements.append({"card": cid} if cid else {})
                    display_ids.append(cid)
            if not valid:
                messages.append(f"{where}: has a ritual requirement the editor cannot place; kept as written")
                project.kept["rituals"].append(entry)
                continue
            project.set_ritual(ritual, requirements, result, origin, objects=True)
        else:
            ids = [project.resolve(t) for t in tributes] + [result]
            if not all(ids):
                messages.append(f"{where}: names a card the editor cannot place; kept as written")
                project.kept["rituals"].append(entry)
                continue
            # A later mod's plain recipe wins over conditions and a place.
            project.set_ritual(ritual, [{"card": c} for c in ids[:-1]], result, origin)


def _read_pool(project: Project, where, duelists, pool, body, messages):
    if not isinstance(body, dict):
        messages.append(f"{where}: a pool is an object of cards and their weights; left out")
        return
    listed, kept = {}, {}
    for name, weight in body.items():
        if name in ("replace", "fixed"):
            continue
        if isinstance(weight, bool) or not isinstance(weight, (int, float)) or _number(weight) < 0:
            messages.append(f"{where} \"{name}\": a weight is a whole number, 0 or more; left out")
            continue
        weight = _number(weight)
        cid = project.resolve(name)
        if not cid:
            messages.append(f"{where} \"{name}\": no such card; kept as written")
            kept[name] = weight
            continue
        listed[cid] = min(weight, 0xFFFF)
    replace = _json_bool(body.get("replace"), False)
    if kept:
        # Kept under the name they were written for: an "all" edit's own
        # stays one body, before the opponents' edits.
        if len(duelists) > 1:
            project.kept_pools.setdefault(("all", pool), {}).update(kept)
        else:
            roster.keep(project, duelists[0], pool, kept)
    for d in duelists:
        if not listed and not replace:
            continue
        pools = roster.pools_of(project, d)
        result = poolmath.apply_edit(pools[pool], listed, replace, pool == "deck")
        if result is None:
            messages.append(f"{where}: {roster.label(project, d)}'s {pool} left as it was (the port refuses this edit)")
        else:
            pools[pool] = result


def read_pools(project: Project, table, decks: bool, messages: list):
    if table is None:
        return
    label = "decks" if decks else "drops"
    if isinstance(table, str):
        # A file of the mod's holding what the key would have held. The editor
        # does not read it, so it is kept as written and the pools stay retail's
        # rather than the file being replaced by them.
        project.pool_files[label] = table
        messages.append(f"\"{label}\" names the file {table}; kept as written (the editor does not read it)")
        return
    if not isinstance(table, dict):
        messages.append(f"\"{label}\" is an object of opponents; left out")
        return
    for name, entry in table.items():
        if decks and isinstance(entry, dict) and _json_bool(entry.get("fixed"), False):
            fixed_decks.read_entry(project, name, entry, messages)
            continue
        if same_all(name):
            duelists = roster.subjects(project)
        else:
            d = duelist_named(name)
            if d < 0:
                d = roster.named(project, name)
                d = -1 if d is None else d
            if isinstance(d, int) and d < 0:
                # A duelist another mod adds, or a misspelling: either way
                # the editor cannot place it, and dropping it would throw away
                # somebody else's roster. Kept exactly as written.
                project.kept_opponents.setdefault(label, {})[name] = entry
                messages.append(f"{label}: \"{name}\" is no duelist the disc has; kept as written")
                continue
            duelists = [d]
        if decks:
            _read_pool(project, f"decks \"{name}\"", duelists, "deck", entry, messages)
            continue
        if not isinstance(entry, dict):
            messages.append(f"drops \"{name}\": an object of pools (pow, bcd, tec); left out")
            continue
        for pool_name, body in entry.items():
            pool = POOL_ALIASES.get("".join(c for c in pool_name.lower() if c.isalnum()))
            if pool in (None, "deck"):
                messages.append(f"drops \"{name}\" \"{pool_name}\": the pools are pow, bcd and tec; left out")
                continue
            _read_pool(project, f"drops \"{name}\" \"{pool_name}\"", duelists, pool, body, messages)


def _read_starter_deck(project: Project, where, entry, messages) -> StarterDeck:
    """One deck of "starter": its name, its weight and its cards by their
    copies. None when the port would refuse the entry outright."""
    if not isinstance(entry, dict):
        messages.append(f"{where}: a starter deck is an object of cards and their copies; left out")
        return None
    deck = StarterDeck()
    name = entry.get("name")
    if isinstance(name, str):
        deck.name = name
    elif name is not None:
        messages.append(f"{where} \"name\": a deck's name is text; left out")
    weight = entry.get("weight")
    if weight is not None:
        if (isinstance(weight, bool) or not isinstance(weight, (int, float))
                or not 0 <= _number(weight) <= STARTER_WEIGHT_LIMIT):
            messages.append(f"{where} \"weight\": a whole number, 0 to {STARTER_WEIGHT_LIMIT}; "
                            "the port leaves the deck out")
            return None
        deck.weight = _number(weight)
    for key, copies in entry.items():
        if key in ("name", "weight"):
            continue
        if isinstance(copies, bool) or not isinstance(copies, (int, float)) or not 0 <= _number(copies) <= DECK_SIZE:
            messages.append(f"{where} \"{key}\": copies are a whole number, 0 to {DECK_SIZE}; left out")
            continue
        copies = _number(copies)
        if not copies:
            continue
        cid = project.resolve(key)
        if not cid:
            messages.append(f"{where} \"{key}\": no such card; kept as written")
            deck.kept[key] = deck.kept.get(key, 0) + copies
            continue
        # Two names for one card (its number and its name) are its copies
        # added up, as the port adds them up.
        deck.cards[cid] = deck.cards.get(cid, 0) + copies
    return deck


def read_starter(project: Project, value, messages: list):
    """"starter": one deck, or a list of them (notes/starter-deck.md)."""
    if value is None:
        return
    if isinstance(value, str):
        # A file of the mod's holding what the key would have held; the editor
        # does not read it, so it stays that filename.
        project.starter_file = value
        messages.append(f"\"starter\" names the file {value}; kept as written (the editor does not read it)")
        return
    if isinstance(value, dict):
        entries = [("starter", value)]
    elif isinstance(value, list):
        entries = [(f"starter {i + 1}", entry) for i, entry in enumerate(value)]
    else:
        messages.append("\"starter\" is a deck, or a list of decks; left out")
        return
    for where, entry in entries:
        deck = _read_starter_deck(project, where, entry, messages)
        if deck is None:
            continue
        if not deck.complete():
            # Kept anyway: the editor is where a deck is put together, and the
            # Conflicts tab says what is still wrong with it.
            messages.append(f"{where}: a starter deck is {DECK_SIZE} cards, and this one has {deck.total()}; "
                            "the port leaves it out until it is")
        project.starter.append(deck)


def read_packs(project: Project, manifest: dict, messages: list):
    """"packs" and "pack_shop" (notes/card-packs.md): each pack kept as the
    object the mod wrote, for the Packs tab to edit; "packs" naming a file of
    the mod is kept as that name."""
    value = manifest.get("packs")
    project.packs, project.packs_file = [], None
    if isinstance(value, str):
        project.packs_file = value
        messages.append(f"\"packs\" names the file {value}; kept as written (checked for compatibility, not editable here)")
    elif isinstance(value, list):
        project.packs = copy.deepcopy(value)
    elif value is not None:
        messages.append("\"packs\" is a list of packs, or the name of a file that holds them; left out")
    rules = manifest.get("pack_shop")
    if rules is None or isinstance(rules, dict):
        project.pack_shop = copy.deepcopy(rules)
    else:
        messages.append("\"pack_shop\" is an object of the shop's rules; left out")
        project.pack_shop = None


def read_passwords(project: Project, messages: list):
    """Explicit table passwords override cards[].password for every loaded
    card. Preserve other rules (all, prices, card number) as written."""
    table = project.other.get("passwords")
    if table is None:
        return
    if not isinstance(table, dict):
        messages.append("\"passwords\" is not an object; kept as written")
        return
    kept = {}
    # With an "all" that sets passwords, a card's own entry keeps it out of
    # "all" even when it names the disc's password: kept as an edit.
    every = any(same_all(key) and isinstance(entry, dict) and "password" in entry for key, entry in table.items())
    for key, entry in table.items():
        cid = 0 if same_all(key) else project.resolve(key)
        password = password_text(entry.get("password")) if isinstance(entry, dict) and "password" in entry else None
        if cid in project.cards and password is not None and cid not in project.password_keys:
            if every or cid in project.added:
                project.passwords[cid] = password
            else:
                project.set_password(cid, password)
            project.password_keys[cid] = key
            rest = {k: v for k, v in entry.items() if k != "password"}
            if rest:
                kept[key] = rest
            continue
        kept[key] = entry
    if kept:
        project.other["passwords"] = kept
    else:
        del project.other["passwords"]


def same_all(name: str) -> bool:
    return "".join(c for c in name.lower() if c.isalnum()) == "all"


def apply(project: Project, manifest: dict, messages: list = None, default_id: str = None, folder=None) -> list:
    """Lay a mod.json over the project (which should be retail): what the
    editor understands becomes edits, the rest is kept as written. A mod
    with no "id" is named after its folder, as the port does. With the
    folder, the files beside mod.json the roster is made of are read too."""
    messages = [] if messages is None else messages
    if not isinstance(manifest, dict):
        raise ValueError("mod.json is not a JSON object")
    info = ModInfo(id=default_id or ModInfo.id)
    for key in INFO_KEYS:
        value = manifest.get(key)
        if isinstance(value, str):
            setattr(info, key, value)
    settings = manifest.get("settings")
    if settings is not None and not isinstance(settings, list):
        messages.append("\"settings\" is not an array: the port refuses the mod; left out")
    info.settings = settings if isinstance(settings, list) else []
    project.info = info
    project.other = {k: v for k, v in manifest.items() if k not in INFO_KEYS and k not in TABLE_KEYS}
    read_card_types(project, manifest.get("card_types"), messages)
    read_cards(project, manifest.get("cards"), messages)
    read_fusions(project, manifest.get("fusions"), messages)
    read_equips(project, manifest.get("equips"), messages)
    read_rituals(project, manifest.get("rituals"), messages)
    # The duelists before the pools, which may name them; the folders' pools
    # after the manifest's, as tables.c reads them.
    roster.read(project, manifest.get("duelists"), folder, messages)
    read_pools(project, manifest.get("drops"), False, messages)
    read_pools(project, manifest.get("decks"), True, messages)
    roster.read_pool_folders(project, folder, messages)
    read_starter(project, manifest.get("starter"), messages)
    read_packs(project, manifest, messages)
    read_passwords(project, messages)
    campaign_map.read_mod(project, messages)
    board_art.read_patches(project, messages)
    return messages


# --- folders ------------------------------------------------------------------

class JsonObject(dict):
    """An object that had a key twice in its file: Python keeps the last, as
    json does; `duplicates` names them, since the game's reader sees both (a
    pack's tier named twice leaves the pack out, packs.c)."""
    duplicates = ()


def _object(pairs):
    out = dict(pairs)
    if len(out) == len(pairs):
        return out
    seen, twice = set(), []
    for key, _ in pairs:
        if key in seen and key not in twice:
            twice.append(key)
        seen.add(key)
    marked = JsonObject(out)
    marked.duplicates = tuple(twice)
    return marked


def read_json(path: Path):
    text = Path(path).read_text(encoding="utf-8-sig")
    return json.loads(text, object_pairs_hook=_object)


def open_mod(retail: GameData, folder) -> tuple:
    """(project, messages): retail with the mod folder's mod.json on top."""
    folder = Path(folder)
    project = Project(retail)
    messages = apply(project, read_json(folder / "mod.json"), default_id=folder.name, folder=folder)
    read_text_cards(project, folder, messages)
    project.source_dir = folder
    art.read_mod(project, folder, messages)
    return project, messages


def is_game_folder(folder: Path) -> bool:
    folder = Path(folder)
    return (folder / "SLUS_014.11").exists() or (folder / "DATA" / "WA_MRG.MRG").exists() or \
        any(p.suffix.lower() in (".bin", ".cue") and p.stat().st_size > 100_000_000
            for p in folder.glob("*") if p.is_file())


def save_mod(project: Project, folder, manifest: dict = None, copy_source: bool = True) -> Path:
    """Write the mod folder: mod.json, and when it is a new place, the files
    of the folder the mod was opened from (art, text, textures...).
    copy_source=False leaves those out (recovery fills them in itself)."""
    folder = Path(folder)
    if folder.exists() and is_game_folder(folder):
        raise ValueError(f"{folder} holds game files; the editor writes mod folders only")
    folder.mkdir(parents=True, exist_ok=True)
    source = project.source_dir
    if copy_source and source and Path(source).resolve() != folder.resolve() and Path(source).is_dir():
        destination = folder.resolve()
        elsewhere = roster.used_elsewhere(project)
        for directory, subdirs, files in os.walk(source):
            directory = Path(directory)
            # Save As may put the new mod inside the old one. Never walk
            # into the destination, including files a previous save left.
            subdirs[:] = [name for name in subdirs if (directory / name).resolve() != destination]
            for name in files:
                item = directory / name
                # The roster's files it read are written afresh below, or
                # are gone with the duelist they were for.
                if item.is_file() and item.name != "mod.json" and \
                        not roster.owned(project, item.relative_to(source).as_posix(), elsewhere):
                    target = folder / item.relative_to(source)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(item, target)
    for name, blob in project.files.items():
        target = folder / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(blob)
    art.write_mod(project, folder)     # the art's PNGs and texture pack, and "textures" in mod.json
    drops, decks = _pool_tables(project)
    roster.write(project, folder, decks, drops)
    manifest = build(project) if manifest is None else manifest
    path = folder / "mod.json"
    temporary = folder / "mod.json.tmp"
    with open(temporary, "w", encoding="utf-8", newline="\n") as out:     # write_text(newline=) is 3.10+
        out.write(dumps(manifest))
    temporary.replace(path)
    project.source_dir = folder
    return path
