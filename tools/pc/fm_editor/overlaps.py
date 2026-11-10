"""What two or more mods change in common, and which of them the game uses:
the same check the Mods window makes (src/pc/mods/overlap.c), line for line
and in the same order, so the editor can say where the mod being edited
meets the other mods the player has installed.

Every mod's manifest becomes claims, one for each thing it sets (a card, a
fusion pair, a pool, a limit, an image...): the thing's key, how the mod
sets it (it sets it, adds to it, defines it) and what it sets. Claims of two
or more mods on one key are an overlap, and the load order decides how it
comes out, the way the game's reader of that key decides it. Some keys reach
keys they do not name ("wide" claims): "all", "stats", a life-point start,
"replace" and a new star's first declaration.

Every key is read as the type its reader takes, a list or an object, and
anything else as nothing, as the readers note it and leave it out; numbers,
booleans and strings as json.c reads them (Json_Number, Json_Bool).

tests/pc/mod_overlaps holds three mods and the lines both this module and
the C engine must find (tests/test_overlaps.py, tests/pc/mods_overlap_test.c).
"""
from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass, field
from pathlib import Path

KINDS = ["Disc data", "Sounds", "Texture images", "Cards", "Fusions", "Equips", "Rituals", "Drops and decks",
         "Starter decks", "Passwords", "Card packs", "Guardian Stars", "Values", "Terrain bonuses", "Attack traps",
         "Duelists", "Text", "Fonts", "Title screen and menus", "Duel pictures", "Code hooks", "Game events"]
(DATA, AUDIO, TEXTURES, CARDS, FUSIONS, EQUIPS, RITUALS, POOLS, STARTER, PASSWORDS, PACKS, STARS, LIMITS, TERRAIN,
 TRAPS, DUELISTS, TEXT, FONT, TITLE, UI, HOOKS, EVENTS) = range(len(KINDS))
INFO, WARNING = 0, 1

SET, ADD, FIXED, BASE, CHAIN, EVENT, FIRST = range(7)
CARD_COUNT = 722
ATTACK_TRAP_FIRST, ATTACK_TRAPS = 681, 6
STARTER_POOLS = 1                 # the starter key of the pools' line (the decks' own is 0)
STARTER_DECK_SIZE = 40
STARTER_WEIGHT_LIMIT = 32767      # a deck's weight (starter.c)
STARTER_POOL_WEIGHT_LIMIT = 65535
ALL = ("all",)
DEFAULT_EQUIP_BONUS = ("default",)
ALL_RECIPES = ("every disc recipe",)   # {"remove": "all"} (overlap.c ALL_RECIPES)
EQUIP_CARDS = 1 << 20   # overlap.c EQUIP_CARDS
STAR_RETAIL = ["", "Mars", "Jupiter", "Saturn", "Uranus", "Pluto", "Neptune", "Mercury", "Sun", "Moon", "Venus"]
TERRAINS = ["", "Forest", "Wasteland", "Mountain", "Sogen", "Umi", "Yami"]
TERRAIN_ALIASES = {"meadow": 4, "sea": 5, "dark": 6}
POOL_WORDS = ["deck", "POW drops", "B/C/D drops", "TEC drops"]
TYPE_NAMES = ["Dragon", "Spellcaster", "Zombie", "Warrior", "Beast-Warrior", "Beast", "Winged Beast", "Fiend",
              "Fairy", "Insect", "Dinosaur", "Reptile", "Fish", "Sea Serpent", "Machine", "Thunder", "Aqua", "Pyro",
              "Rock", "Plant", "Magic", "Trap", "Ritual", "Equip"]
ATTRIBUTE_NAMES = ["Light", "Dark", "Earth", "Water", "Fire", "Wind"]
# What a later replace of a card resets, given or not; the plate ("title") goes with the name.
CARD_RESET_KEYS = ("name", "description", "password", "art", "thumbnail", "title", "field_art",
                   "fusion_groups")
ENTRY_NAMES = ["new_game", "load", "duel", "trade", "options", "campaign", "free_duel", "build_deck", "library",
               "password", "save"]
NO_BONUS = object()
C_SPACE = " \t\n\v\f\r"


def letters(text) -> str:
    """Letters and digits only, lowercased: how the readers compare names."""
    return "".join(c.lower() for c in str(text) if c.isascii() and c.isalnum())


def canonical(value) -> str:
    """What a value says, its own name aside (members' names and order count)."""
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


def _list(value) -> list:
    """A list where the game's readers take only a list; anything else (they
    note it and leave it out) is nothing."""
    return value if isinstance(value, list) else []


def _obj(value) -> dict:
    """The same for an object."""
    return value if isinstance(value, dict) else {}


def _int(value) -> bool:
    """A JSON number (json.c: whole; a boolean is not one)."""
    return isinstance(value, int) and not isinstance(value, bool)


def is_digits(text) -> bool:
    return isinstance(text, str) and text != "" and text.isascii() and text.isdigit()


def _strtol(text: str, base: int):
    """C's strtol over `text`: (value, the rest unread); value 0 and all of
    `text` left when no digit is read. 32-bit longs, as the game's."""
    i, n = 0, len(text)
    while i < n and text[i] in C_SPACE:
        i += 1
    negative = False
    if i < n and text[i] in "+-":
        negative = text[i] == "-"
        i += 1
    digits = "0123456789abcdefABCDEF"[:10 if base == 10 else 22]
    if base == 16 and text[i:i + 2] in ("0x", "0X") and i + 2 < n and text[i + 2] in digits:
        i += 2
    start, value = i, 0
    while i < n and text[i] in digits:
        value = value * base + int(text[i], 16)
        i += 1
    if i == start:
        return 0, text
    value = -value if negative else value
    return max(-2 ** 31, min(2 ** 31 - 1, value)), text[i:]


def _json_number(value, default):
    """json.c Json_Number: a number, a boolean as 0 or 1, and a string that
    holds one, hexadecimal with its "0x" (spaces and a sign allowed)."""
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        stripped = value.lstrip(C_SPACE)
        digits = stripped[1:] if stripped[:1] in "+-" else stripped
        number, rest = _strtol(value, 16 if digits[:2] in ("0x", "0X") else 10)
        if rest != value and not rest.strip(C_SPACE) and -2 ** 31 < number < 2 ** 31 - 1:
            return number
    return default


def _json_bool(value, default=False) -> bool:
    """json.c Json_Bool: a boolean, or a number that is not 0."""
    if isinstance(value, bool):
        return value
    if isinstance(value, int):
        return value != 0
    return default


def _json_string(value):
    return value if isinstance(value, str) else None


class _Refused(ValueError):
    pass


LONG_MIN, LONG_MAX = -2 ** 31, 2 ** 31 - 1   # a long in the game's (32-bit) build
NUMBER = re.compile(r"-?([0-9]+)(?:[.]([0-9]+))?(?:[eE]([-+]?)([0-9]+))?")


def _whole(text: str) -> int:
    """A JSON number as json.c parse_number reads it: whole ("1e3" is 1000,
    "2.50e1" is 25), its digits walked one by one, within a long; anything
    else refused, as json.c refuses the file."""
    number = NUMBER.fullmatch(text)
    if not number:
        raise _Refused(text)
    negative, whole, fraction = text[:1] == "-", number.group(1), number.group(2) or ""
    exponent = 0
    for digit in number.group(4) or "":
        if exponent < 100000:
            exponent = exponent * 10 + int(digit)
    exponent = -exponent if number.group(3) == "-" else exponent
    limit = LONG_MAX + 1 if negative else LONG_MAX
    digits, keep, magnitude = whole + fraction, len(whole) + exponent, 0
    for i, digit in enumerate(int(d) for d in digits):
        if i >= keep:
            if digit:
                raise _Refused(text)    # "a number must be whole"
            continue
        if magnitude > (limit - digit) // 10:
            raise _Refused(text)        # "number out of range"
        magnitude = magnitude * 10 + digit
    i = len(digits)
    while magnitude and i < keep:
        if magnitude > limit // 10:
            raise _Refused(text)
        magnitude *= 10
        i += 1
    return -magnitude if negative else magnitude


def _depth(value, depth=0) -> int:
    if isinstance(value, (list, dict)):
        children = value.values() if isinstance(value, dict) else value
        return max([depth + 1] + [_depth(c, depth + 1) for c in children])
    return depth


def _refuse(text):
    raise _Refused(text)


class JsonObject(dict):
    """An object as json.c keeps one: a name given twice is found by its
    first (Json_Member), and a walk over the members (items(), keys(),
    values(), iterating, len()) meets every one, in order (Json_At)."""

    def __init__(self, pairs=()):
        super().__init__()
        self.pairs = list(pairs)
        for name, member in self.pairs:
            self.setdefault(name, member)

    def items(self):
        return list(self.pairs)

    def keys(self):
        return [name for name, _ in self.pairs]

    def values(self):
        return [member for _, member in self.pairs]

    def __iter__(self):
        return iter(self.keys())

    def __len__(self):
        return len(self.pairs)


HEX_DIGITS = "0123456789abcdefABCDEF"
# A number as json.c parse_number starts one: hexadecimal after 0x, or the
# decimal digits (any leading zeros), then a fraction and exponent left as
# they are for _whole.  What follows it is Python's to refuse.
JSON_NUMBER_TOKEN = re.compile(r"(-?)(?:0[xX]([0-9A-Fa-f]+)|([0-9]+)((?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?))")


def _as_json(text: str) -> str:
    """What json.c reads that Python's json reads otherwise or not at all,
    written as JSON Python reads the same way: a \\u escape is the character
    (a surrogate pair one past U+FFFF) as Python reads it too, but a lone
    surrogate is U+FFFD; a comma may close a list or an object; \\v and \\f
    are spaces between values (C's isspace); a comment ("//" to the end of
    the line, or /* to */) is a space, and one that never ends is left for
    Python to refuse, as json.c refuses it; a number in hexadecimal ("0x1F")
    or with leading zeros ("010", decimal) is written in decimal."""
    out, i, n, last = [], 0, len(text), ""   # last: the last character outside strings and spaces
    while i < n:
        c = text[i]
        if c == "/" and text[i + 1:i + 2] in ("/", "*"):
            end = text.find("\n", i) if text[i + 1] == "/" else text.find("*/", i + 2)
            if end < 0 and text[i + 1] == "*":
                out.append(text[i:])
                break
            i = n if end < 0 else end + (1 if text[i + 1] == "/" else 2)
            out.append(" ")
            continue
        number = JSON_NUMBER_TOKEN.match(text, i) if c == "-" or c.isdigit() else None
        if number:
            sign, hexadecimal, whole, rest = number.groups()
            out.append(sign + (str(int(hexadecimal, 16)) if hexadecimal else (whole.lstrip("0") or "0") + rest))
            i, last = number.end(), "0"
            continue
        if c == '"':
            part, j = ['"'], i + 1
            while j < n and text[j] != '"':
                if text[j] == "\\" and j + 1 < n:
                    code = text[j + 2:j + 6]
                    if text[j + 1] == "u" and len(code) == 4 and all(h in HEX_DIGITS for h in code):
                        number = int(code, 16)
                        low = text[j + 8:j + 12] if text[j + 6:j + 8] == "\\u" else ""
                        if 0xD800 <= number < 0xDC00 and len(low) == 4 and all(h in HEX_DIGITS for h in low) \
                                and 0xDC00 <= int(low, 16) < 0xE000:
                            part.append(text[j:j + 12])     # a pair: one character
                            j += 12
                            continue
                        part.append("\\ufffd" if 0xD800 <= number < 0xE000 else text[j:j + 6])
                        j += 6
                        continue
                    part.append(text[j:j + 2])
                    j += 2
                    continue
                part.append(text[j])
                j += 1
            part.append(text[j:j + 1])
            out.append("".join(part))
            i, last = j + 1, '"'
            continue
        if c in "\v\f":
            out.append(" ")
            i += 1
            continue
        if c in "]}" and last == ",":
            # The comma, unless nothing stands before it ("[,]" is no list).
            k = len(out) - 1
            while len(out[k]) == 1 and out[k] in C_SPACE:
                k -= 1
            m = k - 1
            while m >= 0 and len(out[m]) == 1 and out[m] in C_SPACE:
                m -= 1
            if m >= 0 and out[m] not in ("[", "{", ",", ":"):
                del out[k]
        out.append(c)
        if c not in C_SPACE:
            last = c
        i += 1
    return "".join(out)


def _to_nul(value):
    """A string json.c reads ends at its first NUL (a \\u0000); its bytes are
    held as _read_json holds a file's."""
    if isinstance(value, str):
        return _bytes(value.split("\x00", 1)[0]).decode("utf-8", "surrogateescape")
    if isinstance(value, list):
        return [_to_nul(v) for v in value]
    if isinstance(value, dict):
        return JsonObject((_to_nul(k), _to_nul(v)) for k, v in value.items())
    return value


def parse(text: str):
    """A manifest as json.c reads it, or None: whole numbers within a long,
    64 levels deep at most, control characters in strings, a name given
    twice as JsonObject keeps it, and what _as_json reads. The text is what
    _read_json makes of the file's bytes, which it reads to the first NUL
    (json.c reads a C string)."""
    text = text.split("\x00", 1)[0]
    if text[:1] == "﻿":
        text = text[1:]
    for attempt in ((text,) if "\\u" not in text else ()) + (None,):
        try:
            value = json.loads(_as_json(text) if attempt is None else attempt, strict=False, parse_float=_whole,
                               parse_int=_whole, parse_constant=_refuse, object_pairs_hook=JsonObject)
        except (ValueError, RecursionError):
            continue
        if attempt is None:
            value = _to_nul(value)
        return value if _depth(value) <= 64 else None
    return None


def _bytes(text: str) -> bytes:
    """A string as the C engine holds it: its UTF-8, a byte the file had
    that was not UTF-8 (surrogateescape) as that byte."""
    try:
        return text.encode("utf-8", "surrogateescape")
    except UnicodeEncodeError:
        return text.encode("utf-8", "replace")


def _cut(text: str, size: int) -> str:
    """printf's "%.<size>s": the first `size` bytes, a character cut in two
    and all, as the C engine cuts it."""
    return _bytes(text)[:size].decode("utf-8", "surrogateescape")


def _shown(text: str) -> str:
    """What a line shows: its bytes as UTF-8, a byte that is not shown as
    U+FFFD (as the Mods window, and a reader of the C lines, show it)."""
    return _bytes(text).decode("utf-8", "replace")


def _fit_name(name: str) -> str:
    """A mod's name as the game keeps one (mods.c copy_text, Menu_TextFit):
    95 bytes at most, cut where a character starts (overlap.c fit_name)."""
    data = _bytes(name)
    if len(data) >= 96:
        n = 95
        while n and (data[n] & 0xC0) == 0x80:
            n -= 1
        data = data[:n]
    return data.decode("utf-8", "surrogateescape")


_cache = {}


def _read_json(path: Path):
    """A file as parse() reads it; kept by its size and time, as a check
    reads the same mods' files again on every refresh and changes none."""
    try:
        stat = os.stat(path)
    except OSError:
        return None
    key = (stat.st_mtime_ns, stat.st_size)
    held = _cache.get(str(path))
    if held is not None and held[0] == key:
        return held[1]
    try:
        data = Path(path).read_bytes().split(b"\0", 1)[0]
    except OSError:
        data = None
    if data is not None and data[:3] == b"\xef\xbb\xbf":
        data = data[3:]
    value = None if data is None else parse(data.decode("utf-8", "surrogateescape"))
    _cache[str(path)] = (key, value)
    return value


def _contained(relative) -> bool:
    """paths.c Paths_Contained: a path inside the mod."""
    if not isinstance(relative, str) or not relative or relative[0] in "/\\" or relative[1:2] == ":":
        return False
    if "\\" in relative:
        return False
    return all(part not in ("", ".", "..") for part in relative.split("/"))


@dataclass
class Mod:
    id: str
    name: str
    manifest: dict
    directory: Path | None = None
    broken: bool = False   # mods.c read_manifest: the game lists it but cannot load it


@dataclass
class Claim:
    kind: int
    mod: int
    key: object
    mode: int
    value: object
    src: object = None
    seq: int = 0
    label: str | None = None
    wide: object = None          # a match(key) for a wide claim
    via_wide: bool = False
    aimed: bool = False
    resets: bool = False
    via: str | None = None       # the wide key's own name, for the line
    lo: int = 0
    hi: int = 0
    card_pw: bool = False         # a card's password, beside the packs'


@dataclass
class Overlap:
    kind: int
    claims: list
    severity: int = WARNING
    outcome: str = "later"
    winner: int = -1
    other: int = -1
    best: object = None          # the claim a password gives ("sold")
    label: str = ""
    text: str = ""
    mods: list = field(default_factory=list)


class Source:
    """What the check cannot know from manifests: the cards and opponents the
    game has, and a mod's settings. A method left None is unknown, as an
    empty member of overlap.c's ModsOverlapSource: cards are then matched by
    number and by letters, opponents by name."""
    card = None          # card(text, number) -> id, 0 or less for none (text None: by number)
    card_name = None     # card_name(id) -> name or None
    card_info = None     # card_info(id) -> (base, type, attribute) or None
    duelist = None       # duelist(text) -> id, -1 for none

    def setting(self, mod: Mod, key: str):
        """The mod's setting `key`, or None when it declares none."""
        for spec in _list(mod.manifest.get("settings")):
            if isinstance(spec, dict) and spec.get("key") == key:
                return _json_number(spec.get("default"), 0)
        return None


class _Check:
    def __init__(self, mods, source):
        self.mods = mods
        self.names = [_fit_name(m.name if isinstance(m.name, str) else "") for m in mods]
        self.source = source or Source()
        self.claims = []
        self.seq = 0
        self.memo = {}
        self.star_names = {}
        self.star_declared = set()
        self.defined = []
        self.packs_declared = 0
        self.declared = set()
        for w, mod in enumerate(mods):
            for key in ("after", "requires"):
                for v in _list(mod.manifest.get(key)):
                    target = v if isinstance(v, str) else _json_string(v.get("id")) or "" if isinstance(v, dict) else ""
                    for o, other in enumerate(mods):
                        if other.id == target:
                            self.declared.add((w, o))

    # --- tools ---------------------------------------------------------------

    def member(self, mod, key):
        return self.mods[mod].manifest.get(key)

    def having(self, key) -> int:
        return sum(1 for m in self.mods if key in m.manifest)

    def claim(self, kind, mod, key, mode, value, src=None, label=None) -> Claim:
        self.seq += 1
        c = Claim(kind, mod, key, mode, value, src, self.seq, label)
        self.claims.append(c)
        return c

    def mod_file(self, mod, relative):
        directory = self.mods[mod].directory
        if directory is None or not _contained(relative):
            return None
        return _read_json(Path(directory) / relative)

    def folder_names(self, mod, folder) -> list:
        directory = self.mods[mod].directory
        if directory is None:
            return []
        try:
            names = os.listdir(Path(directory) / folder)
        except OSError:
            return []
        return sorted((n[:-5] for n in names if n.endswith(".json") and len(n) > 5),
                      key=lambda n: n.encode("utf-8", "surrogateescape"))

    def setting(self, mod, key) -> int:
        value = self.source.setting(self.mods[mod], key) if self.source.setting else None
        return -1 if value is None else value

    def switched_on(self, mod, entry) -> bool:
        """mods.c entry_used: "setting" (and "value") of a text file, a
        fusion, equip or ritual entry."""
        key = _json_string(entry.get("setting")) if isinstance(entry, dict) else None
        if not key:
            return True
        value = self.setting(mod, key)
        if value < 0:
            return True
        if "value" in entry:
            return value == entry["value"] if _int(entry["value"]) else True
        return value != 0

    def card_text(self, text):
        name = letters(text)
        if not name:
            return None
        if name not in self.memo:
            if self.source.card:
                self.memo[name] = self.source.card(text, 0) or 0
            else:
                self.memo[name] = int(text) if is_digits(text) else 0
        cid = self.memo[name]
        return cid if cid > 0 else ("text", name)

    def card_key(self, value):
        if _int(value):
            cid = self.source.card(None, value) if self.source.card else value
            return cid if cid and cid > 0 else value if 0 < value < 2 ** 40 else None
        if isinstance(value, str):
            return self.card_text(value)
        return None

    def card_info(self, key):
        """(base, type, attribute) of a card key; -1 for what is unknown."""
        if not isinstance(key, int):
            return 0, -1, -1
        if self.source.card_info:
            info = self.source.card_info(key)
            if info:
                return info
        return key, -1, -1

    def card_words(self, value) -> str:
        number = value if _int(value) else 0
        text = _json_string(value)
        if self.source.card:
            cid = self.source.card(text, number)
        else:
            cid = number if number > 0 else 0
        name = self.source.card_name(cid) if cid and cid > 0 and self.source.card_name else None
        if name:
            return f"'{name}'"
        if text is not None:
            return f"'{text}'"
        return f"#{number}"

    def duelist_key(self, name):
        name = name if isinstance(name, str) else ""
        if letters(name) == "all":
            return ALL
        did = self.source.duelist(name) if self.source.duelist else -1
        if did < 0 and is_digits(name):
            did = int(name)
        return did if did >= 0 else ("text", letters(name))

    def known_duelist(self, who):
        """None when the game can say there is no such opponent."""
        if self.source.duelist and isinstance(who, tuple) and who != ALL:
            return None
        return who

    # --- the kinds -----------------------------------------------------------

    def read_data(self, mod):
        for entry in _list(self.member(mod, "data")):
            entry = _obj(entry)
            file, lba = _json_string(entry.get("file")), _json_number(entry.get("lba"), -1)
            replace = "replace" in entry
            if file:
                name = file.lstrip("\\").split(";")[0]
                self.claim(DATA, mod, ("file", name), SET if replace else ADD, ("own", mod), entry)
            elif lba >= 0:
                sectors = _json_number(entry.get("sectors"), 1)
                lo = hi = -1
                if replace:
                    lo, hi = lba, lba + (sectors if sectors > 0 else 1) - 1
                for p in _list(entry.get("patch")):
                    at, length = _json_number(_obj(p).get("at"), 0), _patch_length(p)
                    if at < 0:
                        continue
                    first, last = lba + at // 2048, lba + (at + (length or 1) - 1) // 2048
                    if lo < 0 or first < lo:
                        lo = first
                    if last > hi:
                        hi = last
                if lo < 0:
                    lo = hi = lba
                c = self.claim(DATA, mod, "raw", SET if replace else ADD, ("own", mod), entry)
                c.lo, c.hi = lo, hi

    def sector_runs(self):
        raw = sorted((c for c in self.claims if c.kind == DATA and c.key == "raw"), key=lambda c: (c.lo, c.seq))
        start = 0
        while start < len(raw):
            end, hi = start + 1, raw[start].hi
            while end < len(raw) and raw[end].lo <= hi:
                hi = max(hi, raw[end].hi)
                end += 1
            label = f"Sector {hi}" if raw[start].lo == hi else f"Sectors {raw[start].lo}-{hi}"
            for c in raw[start:end]:
                c.key, c.label = ("raw", raw[start].lo), label
            start = end

    def read_audio(self, mod):
        audio = _obj(self.member(mod, "audio"))
        for k, kind in enumerate(("music", "xa", "sfx")):
            for name, value in _obj(audio.get(kind)).items():
                number = _audio_id(name)
                if number >= 0:
                    self.claim(AUDIO, mod, (k, number), SET, ("own", mod), name)

    def read_textures(self, mod):
        folder = _json_string(self.member(mod, "textures"))
        if folder is None:
            return
        for entry in _list(self.mod_file(mod, f"{folder}/manifest.json")):
            entry = _obj(entry)
            archive, file = _json_string(entry.get("archive")), _json_string(entry.get("file"))
            offset, words = _json_number(entry.get("offset"), -1), _json_number(entry.get("words"), 0)
            rows, bpp = _json_number(entry.get("rows"), 0), _json_number(entry.get("bpp"), 0)
            stride, clut = _json_number(entry.get("stride"), words), _json_number(entry.get("clut_entries"), 0)
            if archive is None or file is None or not _contained(file) or offset < 0 or not 1 <= words <= 1024 \
                    or not 1 <= rows <= 512 or bpp not in (4, 8, 16) or stride < 1:
                continue
            setting = _json_string(entry.get("setting"))
            if "setting" in entry and setting and self.source.setting and self.setting(mod, setting) == 0:
                continue
            row_offsets = entry.get("row_offsets")
            if isinstance(row_offsets, (list, dict)) and len(row_offsets) == rows:
                stride = 0   # read row by row: no stride (texture_pack.c)
            key = (archive, offset, words, rows, stride, bpp, _json_number(entry.get("clut_offset"), 0) if clut else 0)
            self.claim(TEXTURES, mod, key, SET, ("own", mod), entry)

    def read_cards(self, mod):
        for entry in _list(self.member(mod, "cards")):
            if not isinstance(entry, dict) or "replace" not in entry or _notes_only(entry):
                continue
            replaced = entry["replace"]
            text = _json_string(replaced)
            if text is not None and not (text[:1] and text[:1] in "0123456789"):
                if ":" in text:
                    continue   # an added card's identity: not the disc's
                key = self.card_text(text)
            else:
                number = _json_number(replaced, 0)
                key = number if 1 <= number <= CARD_COUNT else None
            if key is not None and (isinstance(key, tuple) or key <= CARD_COUNT):
                self.claim(CARDS, mod, key, SET, canonical(entry), entry)

    def removes_all(self, value) -> bool:
        """{"remove": "all"} (tables.c): every disc recipe, unless a card is named "all"."""
        if value != "all":
            return False
        cid = self.source.card("all", 0) if self.source.card else None
        return not cid or cid <= 0

    def read_fusions(self, mod):
        for rule in _list(self.member(mod, "fusions")):
            if not self.switched_on(mod, rule):
                continue
            rule = _obj(rule)
            if "remove" in rule:
                key = ALL_RECIPES if self.removes_all(rule["remove"]) else self.card_key(rule["remove"])
                if key is not None:
                    self.claim(FUSIONS, mod, ("remove", key), ADD, 0, rule)
                continue
            with_ = rule.get("with")
            pair = list(with_.values()) if isinstance(with_, dict) else with_ if isinstance(with_, list) else []
            if len(pair) != 2 or "result" not in rule:
                continue
            a, b = self.card_key(pair[0]), self.card_key(pair[1])
            if a is None or b is None:
                continue
            key = ("pair",) + tuple(sorted((a, b), key=repr))
            self.claim(FUSIONS, mod, key, SET, self.card_key(rule["result"]) or 0, rule)

    def read_equips(self, mod):
        for entry in _list(self.member(mod, "equips")):
            key = self.card_key(_obj(entry).get("card"))
            if key is None or not isinstance(entry, dict) or not self.switched_on(mod, entry):
                continue
            # A rule for an equip card is one for its copies too (Tables_Equip
            # matches the card or its base): keyed by base and card, a card's
            # own rule stands beside its copies' rules in other mods.
            if isinstance(key, int) and key < EQUIP_CARDS:
                base = self.card_info(key)[0]
                base = base if isinstance(base, int) and 0 < base < EQUIP_CARDS else key
                key = ("equip", base, key)
            c = self.claim(EQUIPS, mod, key, SET, canonical(entry), entry)
            if isinstance(key, tuple) and key[0] == "equip" and key[1] == key[2]:
                c.wide = lambda other, base=key[1]: isinstance(other, tuple) and other[:2] == ("equip", base)
        if "equip_bonus_default" in self.mods[mod].manifest:
            bonus = self.member(mod, "equip_bonus_default")
            self.claim(EQUIPS, mod, DEFAULT_EQUIP_BONUS, SET, canonical(bonus), bonus)

    def read_rituals(self, mod):
        for entry in _list(self.member(mod, "rituals")):
            e = _obj(entry)
            key = self.card_key(e.get("card"))
            if key is not None and self.switched_on(mod, entry):
                # Where the tributes come from is part of the recipe; "field"
                # written out is the default left out (overlap.c).
                origin = e.get("tributes_from", "field")
                value = (canonical(e["tributes"]) if "tributes" in e else "~",
                         canonical(e["result"]) if "result" in e else "~",
                         "" if origin == "field" else canonical(origin))
                self.claim(RITUALS, mod, key, SET, value, e)

    # duelists, then the pools that may name them

    def duelist_entry(self, mod, did, entry):
        if not isinstance(entry, dict):
            return
        copy = _json_string(entry.get("copy"))
        target = 0
        if "replace" in entry:
            who = _json_string(entry["replace"])
            if who is None:
                return
            number = int(who) if is_digits(who) else -1
            target = self.duelist_key(who)
            if target == ALL or number == 0 or number >= 40 or (isinstance(target, int) and (target == 0 or target >= 40)) \
                    or (self.source.duelist and isinstance(target, tuple)):
                return
            self.claim(DUELISTS, mod, ("replace", target), SET, ("own", mod), entry, f"Duelist '{_cut(who, 80)}' replaced")
        elif not copy:
            return
        if did:
            self.defined.append((mod, did, target, _cut(did if _json_string(entry.get("replace")) is None else entry["replace"], 95)))
        slot = entry.get("slot")
        if "slot" in entry and "replace" not in entry and _int(slot) and 40 <= slot < 128:
            self.claim(DUELISTS, mod, ("slot", slot), FIRST, ("own", mod), entry, f"Free Duel slot {slot}")

    def read_duelists(self, mod):
        for name in self.folder_names(mod, "duelists"):
            self.duelist_entry(mod, name, self.mod_file(mod, f"duelists/{name}.json"))
        listed = self.member(mod, "duelists")
        if isinstance(listed, str):
            listed = self.mod_file(mod, listed)
        for entry in _list(listed):
            self.duelist_entry(mod, _json_string(_obj(entry).get("id")), entry)

    def pool_claim(self, mod, duelist, who, pool, entry):
        if not isinstance(entry, dict):   # a pool is an object of cards (tables.c read_pool)
            return
        e = entry
        fixed_value = e.get("fixed") if pool == 0 else None
        if pool == 0 and "fixed" in e and not isinstance(fixed_value, (bool, int)):
            return
        fixed = _json_bool(fixed_value)
        if fixed and sum(v for k, v in e.items() if k != "fixed" and _int(v)) != 40:
            return
        mode = FIXED if fixed else SET if _json_bool(e.get("replace")) else ADD
        if who == ALL:
            label = f"Every opponent's {POOL_WORDS[pool]}"
        else:
            label = f"{_cut(duelist, 80)}'s {POOL_WORDS[pool]}"
        c = self.claim(POOLS, mod, (who, pool), mode, canonical(entry), entry, label)
        c.resets = mode == SET
        if who == ALL:
            c.wide, c.via = (lambda key, pool=pool: key[1] == pool), "all"

    def pool_table(self, mod, table, decks):
        for name, entry in _obj(table).items():
            who = self.known_duelist(self.duelist_key(name))
            if who is None:
                continue
            if decks:
                self.pool_claim(mod, name, who, 0, entry)
                continue
            for pool_name, pool in _obj(entry).items():
                which = _pool_named(pool_name)
                if which > 0:
                    self.pool_claim(mod, name, who, which, pool)

    def pool_file_duelist(self, mod, name):
        """(the opponent, the label's name): overlap.c pool_file_duelist."""
        for owner, did, replaces, who in self.defined:
            if owner == mod and did == name:
                return (replaces, who) if replaces else (("own", f"{self.mods[mod].id}:{name}"), name)
        return self.known_duelist(self.duelist_key(name)), name

    def read_pools(self, mod):
        for decks, key in ((0, "drops"), (1, "decks")):
            table = self.member(mod, key)
            if isinstance(table, str):
                table = self.mod_file(mod, table)
            self.pool_table(mod, table, decks)
            for name in self.folder_names(mod, key):
                who, label = self.pool_file_duelist(mod, name)
                if who is None:
                    continue
                root = self.mod_file(mod, f"{key}/{name}.json")
                if root is None:
                    continue
                if decks:
                    self.pool_claim(mod, label, who, 0, root)
                else:
                    for pool_name, pool in _obj(root).items():
                        if _pool_named(pool_name) > 0:
                            self.pool_claim(mod, label, who, _pool_named(pool_name), pool)

    def read_passwords(self, mod):
        for name, entry in _obj(self.member(mod, "passwords")).items():
            is_all = letters(name) == "all"
            card = ALL if is_all else self.card_text(name)
            if card is None or not isinstance(entry, dict) or (isinstance(card, int) and card > CARD_COUNT):
                continue
            for f, fields in enumerate((("password",), ("starchips", "starchips_percent"))):
                field_name = next((k for k in fields if k in entry), None)
                if field_name is None:
                    continue
                if is_all:
                    label = f"Every card's {'price' if f else 'password'}"
                else:
                    shown = self.source.card_name(card) if isinstance(card, int) and self.source.card_name else None
                    label = f"{'Price' if f else 'Password'} of '{_cut(shown, 100) if shown else _cut(name, 80)}'"
                c = self.claim(PASSWORDS, mod, (card, f + 1), SET, canonical([field_name, entry[field_name]]),
                               entry, label)
                if is_all:
                    c.wide, c.via = (lambda key, f=f: key[1] == f + 1), "all"

    def pack_rules(self, mod, rules):
        if not isinstance(rules, dict):
            return
        value = canonical([[k, v] for k, v in rules.items() if k != "shops"])
        self.claim(PACKS, mod, "rules", SET, value, rules, "The pack shop's rules")
        for shop in _list(rules.get("shops")):
            sid = _json_string(_obj(shop).get("id")) or ""
            if not isinstance(shop, dict) or not sid:
                continue
            self.claim(PACKS, mod, ("shop", sid), SET, canonical(shop), shop, f"Pack shop '{_cut(sid, 60)}'")

    def pack_passwords(self, mod, packs):
        """A pack's password: of two packs with one, the first in the packs'
        list is sold (by "order", else its place across all mods)."""
        for pack in _list(packs):
            if not isinstance(pack, dict):
                continue
            declared = self.packs_declared
            self.packs_declared += 1
            password = _password_number(pack.get("password"))
            if password < 0:
                continue
            c = self.claim(PACKS, mod, ("password", password), SET, 0, pack, f"Password {password:08d}")
            c.lo = pack["order"] if _int(pack.get("order")) else declared
            c.hi = declared

    def read_packs(self, mod):
        packs = self.member(mod, "packs")
        if isinstance(packs, str):
            found = self.mod_file(mod, packs)
            if isinstance(found, dict):
                self.pack_passwords(mod, found.get("packs"))
                self.pack_rules(mod, found.get("pack_shop"))
            else:
                self.pack_passwords(mod, found)
        else:
            self.pack_passwords(mod, packs)
        self.pack_rules(mod, self.member(mod, "pack_shop"))

    def card_passwords(self, mod):
        """The passwords a mod gives the disc's cards, beside the packs' and
        each other's: of two cards with one, the screen gives the lower number."""
        for name, entry in _obj(self.member(mod, "passwords")).items():
            card = None if letters(name) == "all" else self.card_text(name)
            if card is None or (isinstance(card, int) and card > CARD_COUNT):
                continue
            value = _obj(entry).get("password")
            if isinstance(value, str) and letters(value) == "cardnumber" and isinstance(card, int):
                password = card
            else:
                password = _password_number(value)
            if password < 0:
                continue
            c = self.claim(PACKS, mod, ("password", password), SET, 0, name, f"Password {password:08d}")
            c.card_pw, c.lo = True, card if isinstance(card, int) else 0x7FFFFFFF

    def declare_stars(self, section):
        """overlap.c star_declare: a mod's "stars" before its own matchups; a
        star's name replaces the names it had (stars.c)."""
        for s in _list(section.get("stars")):
            if not isinstance(s, dict) or "id" not in s:
                continue
            sid, name = _json_number(s.get("id"), -1), s.get("name")
            if not 1 <= sid <= 15 or not isinstance(name, (str, dict)):
                continue
            names = [name] if isinstance(name, str) else [n for n in name.values() if isinstance(n, str)]
            self.star_names[sid] = {letters(n) for n in names if letters(n)}

    def star_of(self, value) -> int:
        if _int(value):
            return value
        if not isinstance(value, str):
            return -1
        if is_digits(value):
            return int(value)
        # The disc's ten by their names first, then 1-15 by theirs now (Stars_Find).
        name = letters(value)
        for sid in range(1, 11):
            if letters(STAR_RETAIL[sid]) == name:
                return sid
        return next((sid for sid in range(1, 16) if name in self.star_names.get(sid, ())), -1)

    def star_pair(self, mod, a, d, bonus, src):
        if 0 <= a <= 15 and 0 <= d <= 15:
            self.claim(STARS, mod, ("pair", a, d), SET, bonus, src)

    def read_stars(self, mod):
        section = self.member(mod, "guardian_stars")
        if not isinstance(section, dict):
            return
        self.declare_stars(section)
        bonus = 500
        reset = _json_bool(section.get("replace"))
        if reset:
            c = self.claim(STARS, mod, ("reset",), SET, 0, section.get("replace"))
            c.wide, c.via, c.resets = (lambda key: key[0] == "pair"), "replace", True
        value = section.get("default_bonus")
        if _int(value):
            bonus = value
            for a in range(1, 11) if not reset else ():
                d = a % 6 + 1 if a <= 6 else (a - 7 + 1) % 4 + 7
                self.star_pair(mod, a, d, bonus, value)
                self.star_pair(mod, d, a, -bonus, value)
        if "choice" in section:
            choice = section["choice"]
            self.claim(STARS, mod, ("scalar", 2), SET, letters(choice) if isinstance(choice, str) else "", choice)
        for s in _list(section.get("stars")):
            sid = _json_number(_obj(s).get("id"), -1)
            if not isinstance(s, dict) or "id" not in s or not 1 <= sid <= 15:
                continue
            if sid > 10 and sid not in self.star_declared:
                c = self.claim(STARS, mod, ("declared", sid), SET, 0, s,
                               f"Star {sid} declared (every matchup of it at 0)")
                c.wide = lambda key, sid=sid: key[0] == "pair" and (key[1] == sid or key[2] == sid)
                c.via, c.resets = "stars", True
            self.star_declared.add(sid)
        for s in _list(section.get("stars")):
            sid = _json_number(_obj(s).get("id"), -1)
            if not isinstance(s, dict) or "id" not in s or not 1 <= sid <= 15:
                continue
            for f, name in enumerate(("name", "icon", "palette")):
                if name in s:
                    self.claim(STARS, mod, ("star", sid, f + 1), SET,
                               ("own", mod) if f == 1 else canonical(s[name]), s[name])
            for t in _list(s.get("beats")):
                other = self.star_of(t)
                self.star_pair(mod, sid, other, bonus, t)
                self.star_pair(mod, other, sid, -bonus, t)
        for m in _list(section.get("matchups")):
            if not isinstance(m, dict):
                continue
            a, d = self.star_of(m.get("attacker")), self.star_of(m.get("defender"))
            points = _json_number(m["bonus"], bonus) if "bonus" in m else bonus
            self.star_pair(mod, a, d, points, m)
            if _json_bool(m.get("mirror")):
                self.star_pair(mod, d, a, -points, m)

    def limit_claim(self, mod, path, key, value, src) -> Claim:
        return self.claim(LIMITS, mod, key if key is not None else path, SET, value, src, f"Value {path}")

    def limit_both(self, mod, path, key, one, two, via, value):
        c = self.limit_claim(mod, path, key, canonical(value), value)
        c.wide, c.via = (lambda k, one=one, two=two: k == one or k == two), via

    def read_limits(self, mod):
        for name, m in _obj(self.member(mod, "limits")).items():
            if name == "stats":
                self.limit_both(mod, "stats", "stats", "attack", "defense", "stats", m)
            elif name == "life_points" and not isinstance(m, dict):
                self.limit_both(mod, "life_points", "life_points.start", "life_points.player",
                                "life_points.opponent", "life_points", m)
            elif name == "life_points":
                for k, v in m.items():
                    path = f"life_points.{_cut(k, 60)}"
                    if k == "start":
                        self.limit_both(mod, path, path, "life_points.player", "life_points.opponent",
                                        "life_points.start", v)
                    elif k == "duelists":
                        for d, dv in _obj(v).items():
                            # tables.c read_life_points: "all" is its own (a named duelist wins over it
                            # whatever the order); an opponent the game lacks is left out.
                            who = self.known_duelist(self.duelist_key(d))
                            if who is None:
                                continue
                            key, label = ("lp", who), f"life_points.duelists.{_cut(d, 60)}"
                            if isinstance(dv, dict):
                                for s, sv in dv.items():
                                    if s in ("player", "opponent"):
                                        self.limit_claim(mod, f"{label}.{s}", key + (s,), canonical(sv), sv)
                            elif _int(dv):   # a number is the duelist's own LP: the opponent's side alone
                                self.limit_claim(mod, label, key + ("opponent",), canonical(dv), dv)
                    elif isinstance(v, dict):
                        for s, sv in v.items():
                            self.limit_claim(mod, f"{path}.{_cut(s, 60)}", None, canonical(sv), sv)
                    else:
                        self.limit_claim(mod, path, None, canonical(v), v)
            elif isinstance(m, dict):
                for k, v in m.items():
                    self.limit_claim(mod, f"{_cut(name, 60)}.{_cut(k, 60)}", None, canonical(v), v)
            else:
                self.limit_claim(mod, name, None, canonical(m), m)
        overflow = self.member(mod, "chest_overflow")
        if isinstance(overflow, dict):
            limit, starchips = overflow.get("limit"), overflow.get("starchips")
            # tables.c read_chest_overflow leaves the whole entry out for a
            # limit past 1-255 or starchips past 0-999999.
            bad_limit = "limit" in overflow and not (_int(limit) and 1 <= limit <= 255)
            if bad_limit or ("starchips" in overflow and not (_int(starchips) and 0 <= starchips <= 999999)):
                return
            self.limit_claim(mod, "chest_overflow.limit" if "limit" in overflow else
                             "chest_overflow.limit (250, left out)", "chest",
                             canonical(limit) if "limit" in overflow else canonical(250), overflow)
            self.limit_claim(mod, "chest_overflow.starchips" if "starchips" in overflow else
                             "chest_overflow.starchips (0, left out)", "chest_overflow.starchips",
                             canonical(starchips) if "starchips" in overflow else canonical(0), overflow)

    def read_terrain(self, mod):
        table = self.member(mod, "terrain_bonus")
        if not isinstance(table, dict):
            return
        if _json_bool(table.get("replace")):
            c = self.claim(TERRAIN, mod, ("reset",), SET, 0, table.get("replace"), "Every terrain bonus (\"replace\")")
            c.wide, c.via, c.resets = (lambda key: key[0] == "t"), "replace", True
        for name, types in table.items():
            terrain = _terrain_named(name)
            if not 1 <= terrain <= 6:
                continue
            for type_name, value in _obj(types).items():
                self.claim(TERRAIN, mod, ("t", terrain, letters(type_name)), SET, canonical(value), value,
                           f"{TERRAINS[terrain]} bonus of {_cut(type_name, 60)}")

    def read_traps(self, mod):
        for name, value in _obj(self.member(mod, "trap_thresholds")).items():
            key = self.card_text(name)
            if key is None:
                continue
            base = self.card_info(key)[0]
            if isinstance(key, int) and not ATTACK_TRAP_FIRST <= base < ATTACK_TRAP_FIRST + ATTACK_TRAPS:
                continue
            self.claim(TRAPS, mod, base if isinstance(key, int) else key, SET, canonical(value), name)

    def text_file(self, mod, path):
        try:
            data = Path(path).read_bytes()
        except OSError:
            return
        opened, value = [], []
        for line in data.split(b"\n"):
            if line.endswith(b"\r"):
                line = line[:-1]
            if line[:1] == b"[" or line[:2] == b"{:":
                for c in opened:
                    c.value = tuple(value)
                opened, value = [], []
                close = line.find(b"]") if line[:1] == b"[" else -1
                word = line[1:close].decode("latin-1") if close > 0 else ""
                while word and len(opened) < 64:
                    number, rest = _strtoul16(word)
                    if rest == word:
                        break
                    word = rest.lstrip(" ")
                    if number > 0xFFFF:
                        continue
                    opened.append(self.claim(TEXT, mod, number, SET, ()))
            elif opened and line and line[:1] != b"#":
                value.append(line)
        for c in opened:
            c.value = tuple(value)

    def read_text(self, mod):
        text = self.member(mod, "text")
        directory = self.mods[mod].directory
        for one in text if isinstance(text, list) else [text] if text is not None else []:
            name = _json_string(one) if isinstance(one, str) else _json_string(_obj(one).get("file"))
            if name and directory is not None and _contained(name) and self.switched_on(mod, one):
                self.text_file(mod, Path(directory) / name)

    def title_tree(self, mod, path, key, value, mode, aimed=False):
        if not isinstance(value, dict):
            c = self.claim(TITLE, mod, key, mode, canonical(value), value, path)
            c.aimed = aimed
            return
        for name, inner in value.items():
            self.title_tree(mod, f"{path}.{_cut(name, 60)}", f"{key}.{_cut(name, 60)}", inner, mode, aimed)

    def title_entries(self, mod, path, entries):
        for name, value in _obj(entries).items():
            i = _entry_index(name)
            if i >= 0:
                self.title_tree(mod, f"{path}.{_cut(name, 60)}", f"entries.{i}", value, SET)

    def read_ui(self, mod):
        """"ui": each element's keys the latest mod's (ui_config.c)."""
        for name, element in _obj(_obj(self.member(mod, "ui")).get("duel")).items():
            for key, value in _obj(element).items():
                path = f"ui.duel.{_cut(name, 60)}.{_cut(key, 60)}"
                self.claim(UI, mod, path, SET, canonical(value), value, path)

    def read_title(self, mod):
        own = self.mods[mod].id
        for name, value in _obj(self.member(mod, "title")).items():
            path = f"title.{_cut(name, 60)}"
            if name in ("text", "images"):
                if isinstance(value, list):
                    self.claim(TITLE, mod, path, ADD, 0, value,
                               "title.text (lines)" if name == "text" else "title.images (pictures)")
            elif name == "entries":
                self.title_entries(mod, path, value)
            else:
                self.title_tree(mod, path, "spacing" if name == "spacing" else path, value, SET)
        for name, value in _obj(self.member(mod, "menu")).items():
            if name == "buttons":
                for button in _list(value):
                    bid = _json_string(_obj(button).get("id")) or ""
                    if not isinstance(button, dict) or _entry_index(bid) >= 0 or not bid:
                        continue
                    theirs = ":" in bid and not bid.startswith(own + ":")
                    path = f"menu.buttons.{_cut(bid, 100)}" if ":" in bid else f"menu.buttons.{_cut(own, 60)}:{_cut(bid, 60)}"
                    for k, v in button.items():
                        if k != "id":
                            leaf = f"{path}.{_cut(k, 60)}"
                            self.title_tree(mod, leaf, leaf, v, SET if theirs else BASE, theirs)
            elif name == "order":
                if isinstance(value, list):
                    self.title_tree(mod, "menu.order", "menu.order.first", value, SET)
                else:
                    for k, v in _obj(value).items():
                        if k in ("first", "second"):
                            self.title_tree(mod, f"menu.order.{k}", f"menu.order.{k}", v, SET)
            elif name == "entries":
                self.title_entries(mod, "menu.entries", value)
            else:
                path = f"menu.{_cut(name, 60)}"
                self.title_tree(mod, path, "spacing" if name == "spacing" else path, value, SET)

    # The pools of every mod add up and are dealt from only when their draws
    # make the forty cards of a deck (starter.c Starter_HasPools); else the
    # disc's rows deal it. A written deck any mod offers wins over every mod's
    # pools. Each mod's pools are one claim of the STARTER_POOLS key, their
    # draws in `lo`, and its written decks one more (FIXED, how many in `lo`),
    # as the reader keeps them (overlap.c read_starter).

    def starter_card(self, name) -> bool:
        """A card a deck or a pool names by a member's name: one Cards_Named
        knows. Without the game's cards, any name is one but a number that is not."""
        key = self.card_text(name if isinstance(name, str) else "")
        return key is not None and ((not self.source.card and not is_digits(name)) or isinstance(key, int))

    def starter_pool(self, entry):
        """The draws of a pool read_pool keeps, or None."""
        if not isinstance(entry, dict):
            return None
        count, cards = entry.get("draws"), entry.get("cards")
        if not _int(count) or not 0 <= count <= STARTER_DECK_SIZE or not isinstance(cards, dict):
            return None
        weighted = any(_int(w) and 0 < w <= STARTER_POOL_WEIGHT_LIMIT and self.starter_card(name)
                       for name, w in cards.items())
        return None if count and not weighted else count

    def starter_deck(self, entry) -> bool:
        """A deck read_deck keeps and Starter_Deck can pick: forty cards it
        knows, and a weight above 0."""
        if not isinstance(entry, dict):
            return False
        if "weight" in entry:
            weight = entry["weight"]
            if not _int(weight) or not 0 <= weight <= STARTER_WEIGHT_LIMIT:
                return False
        else:
            weight = 1
        if not weight:
            return False
        total = sum(copies for name, copies in entry.items()
                    if name not in ("name", "weight") and self.starter_card(name) and _int(copies)
                    and 0 <= copies <= STARTER_DECK_SIZE)
        return total == STARTER_DECK_SIZE

    def read_starter(self, mod):
        decks, pools = self.member(mod, "starter"), self.member(mod, "starter_pools")
        dealt = self.starter_deck(decks) if isinstance(decks, dict) else \
            sum(self.starter_deck(d) for d in _list(decks))
        drawn = [self.starter_pool(pools)] if isinstance(pools, dict) else [self.starter_pool(p) for p in _list(pools)]
        drawn = [d for d in drawn if d is not None]
        if dealt:
            self.claim(STARTER, mod, STARTER_POOLS, FIXED, 0, decks).lo = int(dealt)
        if drawn:
            self.claim(STARTER, mod, STARTER_POOLS, ADD, 0, pools).lo = sum(drawn)

    # --- grouping ------------------------------------------------------------

    def run(self, involving=None) -> list:
        if len(self.mods) < 2:
            return []
        for mod in range(len(self.mods)):
            self.read_duelists(mod)
        for mod in range(len(self.mods)):
            if self.having("data") >= 2:
                self.read_data(mod)
            if self.having("audio") >= 2:
                self.read_audio(mod)
            if self.having("textures") >= 2:
                self.read_textures(mod)
            if self.having("cards") >= 2:
                self.read_cards(mod)
            if self.having("fusions") >= 2:
                self.read_fusions(mod)
            if self.having("equips") + self.having("equip_bonus_default") >= 2:
                self.read_equips(mod)
            if self.having("rituals") >= 2:
                self.read_rituals(mod)
            self.read_pools(mod)
            if self.having("starter") >= 2 and "starter" in self.mods[mod].manifest:
                self.claim(STARTER, mod, 0, ADD, 0)
            if self.having("starter_pools"):   # one mod's may make a line too
                self.read_starter(mod)
            if self.having("passwords") >= 2:
                self.read_passwords(mod)
            # A pack's password meets another mod's pack or card password too.
            if self.having("packs") + self.having("pack_shop") >= 2 or \
                    (self.having("packs") and self.having("packs") + self.having("passwords") >= 2):
                self.read_packs(mod)
            if self.having("packs") + self.having("passwords") >= 2 and self.having("passwords"):
                self.card_passwords(mod)
            if self.having("guardian_stars") >= 2:
                self.read_stars(mod)
            if self.having("limits") + self.having("chest_overflow") >= 2:
                self.read_limits(mod)
            if self.having("terrain_bonus") >= 2:
                self.read_terrain(mod)
            if self.having("trap_thresholds") >= 2:
                self.read_traps(mod)
            if self.having("text") >= 2:
                self.read_text(mod)
            if self.having("font") >= 2 and "font" in self.mods[mod].manifest:
                self.claim(FONT, mod, 0, ADD, 0)
            if self.having("title") + self.having("menu") >= 2:
                self.read_title(mod)
            if self.having("ui") >= 2:
                self.read_ui(mod)
        self.sector_runs()
        self.widen()
        groups = {}
        for c in self.claims:
            groups.setdefault((c.kind, repr(c.key)), []).append(c)
        found = []
        for (kind, _), claims in groups.items():
            mods = {c.mod for c in claims}
            claims.sort(key=lambda c: (c.mod, c.seq))
            makes = _starter_line(claims)[0] if kind == STARTER and claims[0].key == STARTER_POOLS else len(mods) >= 2
            if not makes or (involving is not None and involving not in mods):
                continue
            overlap = Overlap(kind, claims)
            self.decide(overlap)
            overlap.label = self.label(overlap)
            overlap.mods = [_shown(self.mods[m].id) for m in _order(claims)]
            overlap.text = _shown(self.text(overlap))
            overlap.label = _shown(overlap.label)
            found.append(overlap)
        # Within a kind, the warnings first, then as the earliest mod's manifest
        # has them, and between the keys an "all" reaches, as the manifests name them.
        found.sort(key=lambda o: (o.kind, -o.severity, o.claims[0].seq,
                                  next((c.seq for c in o.claims if not c.via_wide), o.claims[0].seq)))
        return found

    def widen(self):
        wide = [c for c in self.claims if c.wide]
        if not wide:
            return
        named = {}   # (kind, key) -> (key, {mod: whether a claim of it does more than add})
        for c in self.claims:
            if not c.wide and not c.via_wide:
                mods = named.setdefault((c.kind, repr(c.key)), (c.key, {}))[1]
                mods[c.mod] = mods.get(c.mod, False) or c.mode != ADD
        for w in wide:
            for (kind, _), (key, mods) in named.items():
                # A reset reaches earlier mods' keys only; a pool's "all" also a later replace or fixed deck.
                others = {m for m, more in mods.items()
                          if m != w.mod and (not w.resets or m < w.mod or (w.kind == POOLS and more))}
                if kind == w.kind and others and w.wide(key):
                    copy = Claim(w.kind, w.mod, key, w.mode, w.value, w.src, w.seq, w.label, None, True, w.aimed,
                                 w.resets, w.via)
                    self.claims.append(copy)

    def decide(self, o: Overlap):
        c = o.claims
        o.severity = WARNING
        if o.kind == HOOKS:
            o.winner, o.outcome = c[-1].mod, "chain"
            return
        if o.kind == EVENTS:
            o.outcome, o.severity = "events", INFO
            return
        if o.kind == DATA:
            self.decide_data(o)
            return
        if o.kind == STARTER and c[0].key == STARTER_POOLS:
            # A written deck wins, else pools that draw forty add up.
            _, draws, decks = _starter_line(c)
            o.winner = next((x.mod for x in c if x.mode == FIXED), -1)
            o.outcome = "written" if decks else "add" if draws == STARTER_DECK_SIZE else "dropped"
            if o.outcome == "add":
                o.severity = INFO
            return
        if o.kind == PACKS and isinstance(c[0].key, tuple) and c[0].key[0] == "password":
            # One password: a card's first (the lowest number), else the first pack in the list.
            best = min(c, key=lambda x: (not x.card_pw, x.lo, x.hi))
            o.winner, o.best, o.outcome = best.mod, best, "sold"
            return
        fixed = last = -1
        for i, x in enumerate(c):
            if x.mode == FIXED:
                fixed = i
            if x.mode in (SET, FIXED):
                last = i
        if o.kind == DUELISTS and c[0].mode == FIRST:
            o.winner, o.outcome = c[0].mod, "first"
            return
        if fixed >= 0:
            o.winner = c[fixed].mod
            agree = all(x.mod == o.winner or (x.mode == FIXED and x.value == c[fixed].value) for x in c)
            o.outcome, o.severity = ("agree", INFO) if agree else ("fixed", WARNING)
            return
        for i, x in enumerate(c):
            if x.mode == BASE:
                for y in c[:i]:
                    if y.aimed and y.mod < x.mod:
                        o.winner, o.other, o.outcome = y.mod, x.mod, "early"
                        return
        if last < 0:
            o.outcome, o.severity = "add", INFO
            return
        o.winner = c[last].mod
        losers, agree, all_declared = 0, True, True
        for x in c[:last]:
            if x.mod == o.winner:
                continue
            if x.mode == BASE:
                o.other = x.mod
                continue
            losers += 1
            o.other = x.mod
            if x.mode != SET or x.value != c[last].value:
                agree = False
            if (o.winner, x.mod) not in self.declared:
                all_declared = False
        if not losers:
            for x in c:
                if x.mode == BASE and x.mod != o.winner:
                    o.other = x.mod
            o.outcome, o.severity = ("aimed" if o.other >= 0 else "add"), INFO
            return
        if o.kind == CARDS:
            used, dropped, met, _ = self.card_keys(o)
            if not used and not dropped:
                o.outcome, o.severity = ("agree" if met else "add"), INFO
                return
            o.outcome = "keys"
        elif o.kind == EQUIPS and c[last].key != DEFAULT_EQUIP_BONUS:
            differ = same = False
            for i in range(len(c)):
                for j in range(i + 1, len(c)):
                    if c[i].mod != c[j].mod:
                        d, s = self.equips_meet(c[i].src, c[j].src)
                        differ |= d
                        same |= s
            if not differ:
                o.outcome, o.severity = ("agree" if same else "add"), INFO
                return
            o.outcome = "reset" if _json_bool(_obj(c[last].src).get("replace")) else "later"
        elif agree:
            o.outcome, o.severity = "agree", INFO
            return
        else:
            o.outcome = "reset" if c[last].resets else "later"
        if all_declared:
            o.outcome, o.severity = "after", INFO

    def decide_data(self, o: Overlap):
        c = o.claims
        o.severity = WARNING
        for i in range(len(c) - 1, -1, -1):
            for j in range(i):
                if c[i].mode == SET and c[j].mode == SET and c[i].mod != c[j].mod and _sectors_meet(c[i], c[j]):
                    o.winner, o.other = c[i].mod, c[j].mod
                    after = (o.winner, o.other) in self.declared
                    o.outcome, o.severity = ("after", INFO) if after else ("later", WARNING)
                    return
        for i in range(len(c) - 1, -1, -1):
            for j in range(i):
                if c[i].mode == ADD and c[j].mode == ADD and c[i].mod != c[j].mod and _patches_meet(c[i], c[j]):
                    o.winner, o.other, o.outcome = c[i].mod, c[j].mod, "bytes"
                    return
        for i in range(len(c)):
            for j in range(len(c)):
                if c[i].mode == ADD and c[j].mode == SET and c[i].mod != c[j].mod and _sectors_meet(c[i], c[j]):
                    o.winner, o.other, o.outcome = c[i].mod, c[j].mod, "patched"
                    return
        o.outcome, o.severity = "add", INFO

    def card_keys(self, o: Overlap):
        """The keys a later mod's entry sets differently (used), the reset keys
        an earlier entry sets that a later one leaves out (dropped), and
        whether any key met at all."""
        c, used, dropped, met, dropped_from = o.claims, [], [], False, []
        for i in range(len(c)):
            for j in range(i + 1, len(c)):
                if c[j].mod == c[i].mod:
                    continue
                for name, value in c[i].src.items():
                    if name in ("replace", "notes", "id"):
                        continue
                    if name in c[j].src:
                        met = True
                        if canonical(c[j].src[name]) != canonical(value) and name not in used:
                            used.append(name)
                    elif name in CARD_RESET_KEYS:
                        if name not in dropped:
                            dropped.append(name)
                        if c[i].mod not in dropped_from and len(dropped_from) < 8:
                            dropped_from.append(c[i].mod)
        return used, dropped, met, dropped_from

    def equip_target(self, value):
        """A target of "add" or "remove": ("type", n), a card key, or None."""
        text = _json_string(value)
        t = _type_named(text) if text is not None else -1
        if t >= 0 and (not self.source.card or self.source.card(text, 0) <= 0):
            return ("type", t)
        return self.card_key(value)

    def equip_allows(self, entry, card, type_):
        best, rank = None, 0
        if _json_bool(entry.get("replace")):
            best, rank = False, 1
        for allow, key in ((True, "add"), (False, "remove")):
            for t in _list(entry.get(key)):
                target = self.equip_target(t)
                if target is None:
                    r = 0
                elif isinstance(target, tuple) and target[0] == "type":
                    r = 2 if type_ >= 0 and target[1] == type_ else 0
                else:
                    r = 3 if card is not None and target == card else 0
                if r and r >= rank:
                    best, rank = allow, r
        return best

    def equips_meet(self, early, late):
        cards, types, attributes = [], [-1], [-1]
        for entry in (early, late):
            for key in ("add", "remove"):
                for t in _list(entry.get(key)):
                    target = self.equip_target(t)
                    if target is None:
                        continue
                    if isinstance(target, tuple) and target[0] == "type":
                        if len(types) < 32:
                            types.append(target[1])
                    elif len(cards) < 64:
                        cards.append((target, self.card_info(target)[1]))
            for name, value in _obj(entry.get("bonus_if")).items():
                t, a = _type_named(name), _attribute_named(name)
                if 0 <= t < 20 and len(types) < 32:
                    types.append(t)
                elif a >= 0 and len(attributes) < 8:
                    attributes.append(a)
        differ = same = False
        for card, type_ in cards + [(None, t) for t in types]:
            a, b = self.equip_allows(early, card, type_), self.equip_allows(late, card, type_)
            if a is None or b is None:
                continue
            if a != b:
                differ = True
            else:
                same = True
        for t in types:
            for att in attributes:
                for stat in (0, 1):
                    one, two = _equip_bonus(early, t, att, stat), _equip_bonus(late, t, att, stat)
                    if one is NO_BONUS or two is NO_BONUS:
                        continue
                    if one != two:
                        differ = True
                    else:
                        same = True
        return differ, same

    # --- lines ---------------------------------------------------------------

    def star_words(self, star: int) -> str:
        if 1 <= star <= 10:
            return STAR_RETAIL[star]
        for mod in self.mods:
            for s in _list(_obj(mod.manifest.get("guardian_stars")).get("stars")):
                s = _obj(s)
                if _json_number(s.get("id"), -1) != star:
                    continue
                name = s.get("name")
                if isinstance(name, dict):
                    name = next(iter(name.values()), None)
                if isinstance(name, str):
                    return name
        return f"Star {star}"

    def label(self, o: Overlap) -> str:
        c = next((x for x in o.claims if not x.via_wide), o.claims[0])
        if c.label is not None:
            return c.label
        src, kind = c.src, o.kind
        if kind == DATA:
            return f"File {_json_string(src.get('file')) or '?'}"
        if kind == AUDIO:
            return f"{['Song', 'XA clip', 'Sound effect'][c.key[0]]} 0x{c.key[1]:X}"
        if kind == TEXTURES:
            name = _json_string(src.get("alias")) or _json_string(src.get("file")) or "?"
            return f"Image {name}, {_json_string(src.get('archive')) or '?'} at 0x{_json_number(src.get('offset'), 0):X}"
        if kind == CARDS:
            return f"Card {self.card_words(src.get('replace'))}"
        if kind == FUSIONS:
            if c.key == ("remove", ALL_RECIPES):
                return "Every disc recipe removed"
            if "remove" in src:
                return f"Disc recipes for {self.card_words(src['remove'])} removed"
            with_ = src["with"]
            pair = list(with_.values()) if isinstance(with_, dict) else with_
            return f"Fusion {self.card_words(pair[0])} + {self.card_words(pair[1])}"
        if kind == EQUIPS:
            return "The default equip bonus" if c.key == DEFAULT_EQUIP_BONUS else f"Equip {self.card_words(src.get('card'))}"
        if kind == RITUALS:
            return f"Ritual {self.card_words(src.get('card'))}"
        if kind == STARTER:
            return "Starter pools" if c.key == STARTER_POOLS else "Starter decks"
        if kind == STARS:
            key = c.key
            if key[0] == "pair":
                return f"Matchup {self.star_words(key[1])} attacking {self.star_words(key[2])}"
            if key[0] == "star":
                return f"The {['', 'name', 'icon', 'palette'][key[2]]} of star {key[1]}, {self.star_words(key[1])}"
            if key[0] == "scalar":
                return "How a summon chooses its star"
            return "Every matchup (\"replace\")"
        if kind == TRAPS:
            return f"Attack trap '{src}'"
        if kind == TEXT:
            return f"Text [{c.key:04X}]"
        if kind == FONT:
            return "Fonts"
        return "?"

    def text(self, o: Overlap) -> str:
        names = ", ".join(self.names[m] for m in _order(o.claims))
        winner = self.names[o.winner] if o.winner >= 0 else ""
        other = self.names[o.other] if o.other >= 0 else ""
        via = next((c.via for c in o.claims if c.mod == o.winner and c.via_wide and c.via), None)
        head = f"{o.label} ({names}): "
        distinct = len({c.mod for c in o.claims})
        if o.outcome == "later":
            through = f', through its "{_cut(via, 80)}"' if via else ""
            return head + f"{winner} wins (later in load order{through})"
        if o.outcome == "after":
            return head + f"{winner} wins (it loads after {other} on purpose: after/requires)"
        if o.outcome == "agree":
            return head + ("where they set the same key they agree; the rest combines" if o.kind == CARDS
                           else "the same in each, so no difference")
        if o.outcome == "add":
            extra = {DATA: " (replacements first, then patches of different bytes)",
                     CARDS: " (they set different stats)",
                     POOLS: " (each edits the pool as the mods before left it)",
                     FONT: " (a letter comes from the first font that has it)"}.get(o.kind, "")
            if o.kind == STARTER and o.claims[0].key == STARTER_POOLS:
                extra = " (their draws make the 40 cards of a deck)"
            return head + f"{'all' if distinct > 2 else 'both'} apply and add up{extra}"
        if o.outcome == "reset":
            word = _cut(via, 80) if via else "replace"
            return head + f"{winner}'s \"{word}\" clears what the earlier mods set"
        if o.outcome == "fixed":
            return head + f"{winner}'s fixed deck is dealt; the other edits of it are left out"
        if o.outcome in ("written", "dropped"):
            # Whose written decks win, by name ("A's", "A's and C's"), or how
            # far the pools' draws are from a deck.
            _, draws, decks = _starter_line(o.claims)
            if o.outcome == "dropped":
                return head + f"pools dropped: {draws} draws, not {STARTER_DECK_SIZE}; the disc's starter decks are used"
            writers = [f"{self.names[x.mod]}'s" for x in o.claims if x.mode == FIXED]
            who = writers[0] if len(writers) == 1 else ", ".join(writers[:-1]) + " and " + writers[-1]
            return head + (f"{who} starter deck{'s' if decks > 1 else ''} {'are' if decks > 1 else 'is'} dealt; "
                           "every mod's pools are left out (a written deck wins)")
        if o.outcome == "keys":
            used, dropped, _, dropped_from = self.card_keys(o)
            used_words, dropped_words = ", ".join(used), ", ".join(dropped)
            said = f"{winner}'s {used_words} {'are' if ',' in used_words else 'is'} used" if used else ""
            if dropped:
                names = [self.names[m] for m in dropped_from]
                gave = names[0] if len(names) == 1 else ", ".join(names[:-1]) + " and " + names[-1]
                said += f"{' and ' if said else ''}the {dropped_words} {gave} gave " \
                        f"{'are' if ',' in dropped_words else 'is'} dropped, as {winner}'s replace starts " \
                        f"{'them' if ',' in dropped_words else 'it'} from the disc's"
            return head + f"{said} (later in load order); the rest combines"
        if o.outcome == "bytes":
            return head + f"{winner}'s bytes are read where they patch the same ones"
        if o.outcome == "patched":
            return head + f"{winner}'s patch is written into {other}'s replacement, at the disc's offsets"
        if o.outcome == "chain":
            return head + f"{winner}'s hook runs first (applied last); the others run only if it calls its original"
        if o.outcome == "events":
            return head + "each is called, higher priority first; a before-hook that handles it stops the rest"
        if o.outcome == "first":
            return head + f"{winner} keeps it, earlier in load order; the others take the next free slot"
        if o.outcome == "sold":
            if o.best.card_pw:
                tail = ", not the others' card or pack" if len(o.claims) > 1 else ""
                return head + f"the digits give {winner}'s card '{o.best.src}'{tail}"
            pack = _json_string(o.best.src.get("id")) or _json_string(o.best.src.get("name")) or "?"
            return head + f"{winner}'s pack '{pack}' is sold, the first in the list; the others' with it are not"
        if o.outcome == "early":
            return head + f"{winner}'s change is left out: it loads before {other}, whose button it names"
        return head + f"{winner} changes {other}'s own on purpose"


def _starter_line(claims) -> tuple:
    """Whether the pools' claims make a line (overlap.c starter_line): pools
    beside another mod's written deck, two mods' pools, or one mod's whose
    draws are not a deck's forty (which the game treats the same). Then the
    draws of every pool, and how many written decks win over them."""
    pools = [c for c in claims if c.mode != FIXED]
    written = [c for c in claims if c.mode == FIXED]
    draws, decks = sum(c.lo for c in pools), sum(c.lo for c in written)
    if not pools:
        return False, draws, decks
    if written:
        return len({c.mod for c in claims}) >= 2, draws, decks
    return len(pools) >= 2 or draws != STARTER_DECK_SIZE, draws, decks


def _order(claims) -> list:
    """The mods of a line's claims, in the claims' order, once each."""
    seen = []
    for c in claims:
        if c.mod not in seen:
            seen.append(c.mod)
    return seen


def _password_number(value) -> int:
    """The digits typed on the Password screen, as a number (packs.c
    read_password): up to eight digits, or a number 0-99999999; -1 for none."""
    if _int(value):
        return value if 0 <= value <= 99999999 else -1
    if not isinstance(value, str) or not value or len(value) > 8 or not all(c in "0123456789" for c in value):
        return -1
    return int(value)


def _notes_only(entry: dict) -> bool:
    return all(k in ("replace", "notes", "id") for k in entry) and "notes" in entry


def _audio_id(text) -> int:
    """replace.c AudioReplace_ParseId: 0x-prefixed hexadecimal, else decimal, 0-0xFFFF."""
    if not isinstance(text, str) or not text or text[0] in "-+ ":
        return -1
    if text[:2] in ("0x", "0X"):
        if len(text) == 2:
            return -1
        value, rest = _strtol(text[2:], 16)
    else:
        value, rest = _strtol(text, 10)
    if rest or value < 0 or value > 0xFFFF:
        return -1
    return value


def _strtoul16(word: str):
    """strtoul(word, 16) as listing.c reads an item's ids: (value, the rest);
    the rest is `word` when nothing was read."""
    i, n = 0, len(word)
    while i < n and word[i] in C_SPACE:
        i += 1
    negative = False
    if i < n and word[i] in "+-":
        negative = word[i] == "-"
        i += 1
    hexdigits = "0123456789abcdefABCDEF"
    if word[i:i + 2] in ("0x", "0X") and i + 2 < n and word[i + 2] in hexdigits:
        i += 2
    start, value = i, 0
    while i < n and word[i] in hexdigits:
        value = value * 16 + int(word[i], 16)
        i += 1
    if i == start:
        return 0, word
    if negative and value:
        value = 2 ** 32 - value if value < 2 ** 32 else 2 ** 32
    return value, word[i:]


def _entry_index(name: str) -> int:
    """title_config.c entry_index: an entry's name, or its number 0-10."""
    if name in ENTRY_NAMES:
        return ENTRY_NAMES.index(name)
    value, rest = _strtol(name, 10)
    return value if name and rest == "" and 0 <= value < 11 else -1


def _type_named(text) -> int:
    for i, name in enumerate(TYPE_NAMES):
        if letters(text) and letters(text) == letters(name):
            return i
    return -1


def _attribute_named(text) -> int:
    for i, name in enumerate(ATTRIBUTE_NAMES):
        if letters(text) and letters(text) == letters(name):
            return i
    return -1


def _equip_bonus(entry: dict, type_: int, attribute: int, stat: int):
    """Its ATK (stat 0) or DEF (1) bonus (overlap.c equip_bonus)."""
    for name, value in _obj(entry.get("bonus_if")).items():
        t = _type_named(name)
        a = _attribute_named(name) if t < 0 or t >= 20 else -1
        if not _int(value):
            continue
        if (0 <= t < 20 and t == type_) or (a >= 0 and a == attribute):
            return value
    own = entry.get("bonus_defense" if stat else "bonus_attack")
    if _int(own):
        return own
    return entry["bonus"] if _int(entry.get("bonus")) else NO_BONUS


def _pool_named(name) -> int:
    for i, names in enumerate((("pow", "sa-pow"), ("bcd", "b-c-d"), ("tec", "sa-tec"))):
        if any(letters(name) == letters(n) for n in names):
            return i + 1
    return -1


def _terrain_named(name) -> int:
    for i in range(1, 7):
        if letters(name) == letters(TERRAINS[i]):
            return i
    if letters(name) in TERRAIN_ALIASES:
        return TERRAIN_ALIASES[letters(name)]
    return int(name) if is_digits(name) else -1


def _patch_length(patch) -> int:
    text = _json_string(_obj(patch).get("bytes")) or ""
    return sum(1 for ch in text if ch in "0123456789abcdefABCDEF") // 2


def _patches_meet(a: Claim, b: Claim) -> bool:
    if (a.key == "raw" or isinstance(a.key, tuple) and a.key[0] == "raw") != \
            (b.key == "raw" or isinstance(b.key, tuple) and b.key[0] == "raw"):
        return False

    def runs(c):
        base = _json_number(c.src.get("lba"), 0) * 2048 if isinstance(c.key, tuple) and c.key[0] == "raw" else 0
        for p in _list(c.src.get("patch")):
            yield base + _json_number(_obj(p).get("at"), 0), _patch_length(p)

    return any(pa < qa + ql and qa < pa + pl for pa, pl in runs(a) for qa, ql in runs(b))


def _sectors_meet(a: Claim, b: Claim) -> bool:
    return a.lo <= b.hi and b.lo <= a.hi


def check(mods: list, source: Source = None, involving: int = None) -> list:
    """The overlaps of `mods`, in load order (earliest first); with
    `involving`, only those of mods[involving] (the others are not worked
    out, which is what makes the editor's check quick)."""
    return _Check(mods, source).run(involving)


def line(overlap: Overlap) -> str:
    """kind|severity|outcome|mods|label, as tests/pc/mod_overlaps/expected.txt has them."""
    return "|".join((KINDS[overlap.kind], "warning" if overlap.severity else "info", overlap.outcome,
                     ", ".join(overlap.mods), overlap.label))


# --- the installed mods and their order ------------------------------------------

def read_settings(path: Path) -> dict:
    """The port's settings file (src/pc/platform/settings.c): key=value lines."""
    values = {}
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return values
    for row in text.splitlines():
        key, sep, value = row.partition("=")
        if sep:
            try:
                values[key.strip()] = int(value.strip())
            except ValueError:
                pass
    return values


ID_LETTERS = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")


def _fit(text: str, size: int) -> str:
    """mods.c copy_text: at most size - 1 bytes, cut where a character starts."""
    data = _bytes(text)
    if len(data) >= size:
        n = size - 1
        while n and (data[n] & 0xC0) == 0x80:
            n -= 1
        data = data[:n]
    return data.decode("utf-8", "surrogateescape")


def _identity(manifest, folder_name: str) -> tuple:
    """(id, broken) as mods.c read_manifest gives them: a mod.json that is
    not an object, or an id that is not 1-63 letters, digits, hyphens or
    underscores, is the folder's name and broken; so is a mod whose library
    or textures reach outside it, or whose "data" or "cards" is not a list or
    "audio" not an object. The game lists a broken mod (in its place, over a
    shipped copy of that id) but never loads it."""
    folder_id = _fit(folder_name, 64)
    if not isinstance(manifest, dict):
        return folder_id, True
    text = _json_string(manifest.get("id")) or folder_name
    if len(_bytes(text)) >= 64 or not all(c in ID_LETTERS for c in text):
        return folder_id, True
    broken = any(isinstance(manifest.get(key), str) and manifest[key] and not _contained(manifest[key])
                 for key in ("library", "textures"))
    broken = broken or any(key in manifest and not isinstance(manifest[key], kind)
                           for key, kind in (("data", list), ("audio", dict), ("cards", list)))
    return text, broken


def installed(folders) -> list:
    """Every mod (a folder with a mod.json) in `folders`, in the order the
    game finds them (mods.c scan): folder by folder, names sorted; the first
    of one id is kept within a folder, a later folder's copy replaces an
    earlier's in its place. A broken one (_identity) is listed too, as the
    game lists it, and load_order leaves it out."""
    found = {}
    for folder in folders:
        try:
            names = sorted(os.listdir(folder), key=lambda n: n.encode("utf-8", "surrogateescape"))
        except OSError:
            continue
        here = set()
        for name in names:
            if name.startswith("."):
                continue
            if not (Path(folder) / name / "mod.json").is_file():
                continue
            manifest = _read_json(Path(folder) / name / "mod.json")
            mid, broken = _identity(manifest, name)
            if mid in here:
                continue
            here.add(mid)
            manifest = manifest if isinstance(manifest, dict) else {}
            mod = Mod(mid, manifest.get("name") if isinstance(manifest.get("name"), str) else mid, manifest,
                      Path(folder) / name, broken)
            found[mid] = mod   # a later folder's copy keeps the earlier's place
    return list(found.values())


def _needs(manifest: dict, key: str) -> list:
    return [v if isinstance(v, str) else _json_string(v.get("id")) if isinstance(v, dict) else None
            for v in _list(manifest.get(key))]


def load_order(mods: list, settings: dict = None) -> list:
    """manager.c Mods_Order: the lowest Load order first (the player's
    mod.<id>.order, else the manifest's priority), never before a mod it
    requires or names in "after"; equal ones in the order they were found.
    A broken mod, a mod whose requirement is not there (or is left out
    itself), and the mods of a cycle (and those waiting on them), are left
    out, as the game leaves them out."""
    settings = settings or {}
    mods = [m for m in mods if not getattr(m, "broken", False)]
    while True:   # a mod that requires one left out is left out too, and so on
        ids = {m.id for m in mods}
        kept = [m for m in mods if all(r in ids for r in _needs(m.manifest, "requires") if r)]
        if len(kept) == len(mods):
            break
        mods = kept
    done, order = set(), []
    while len(order) < len(mods):
        best = None
        for i, m in enumerate(mods):
            if m.id in done:
                continue
            waits = _needs(m.manifest, "requires") + _needs(m.manifest, "after")
            if any(w in ids and w not in done for w in waits):
                continue
            rank = settings.get(f"mod.{m.id}.order", _json_number(m.manifest.get("priority"), 0))
            if best is None or rank < best[0]:
                best = (rank, i)
        if best is None:    # a cycle: the rest are not loaded
            break
        order.append(mods[best[1]])
        done.add(mods[best[1]].id)
    return order
