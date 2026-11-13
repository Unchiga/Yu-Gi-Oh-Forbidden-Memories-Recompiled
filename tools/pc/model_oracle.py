#!/usr/bin/env python3
"""The battle models' control modules, two ways, frame for frame: the oracle
for moving MODEL.MRG's variant modules from the MIPS interpreter to native C
(notes/pc-build.md, "MIPS-only effects").

Each record of MODEL.MRG carries four variant module images, one for each
guardian star (the alternative, D_800EF658's last byte) in each battle slot
(A at 0x8013B000, B at 0x8017B000: stages 7-10 of the record's load). For
every record asked for, the game runs one 3D battle per alternative and
outcome with the record's monster on both sides, so that both slots load
and run that alternative's image. A battle is driven the way the duel
starts one (duel_scene_battle.c, phase 5): the two model records poked into
D_800EF658, the mode to come back to into D_8009B269 and the mode byte
D_8009B26C set to the animated battle (1). Every battle starts from the
same save state (the debug menu), so each is the same whatever ran before.

Two sides run every battle: the subject (by default the build's default,
MEMORIES_MODEL_MODULES unset) and the reference (MEMORIES_MODEL_MODULES=
interpreter). For each battle they must agree on every frame hash
(MEMORIES_FRAME_HASHES) and every module call the model trace logs (a
setup command, its slot, module and frame; the updates by count). The
trace also says what ran each module call: "native" (the decomp's C, for
the code shapes config/pc/model_gate.csv ports), "interpreter" or "stand-in".
A default-mode side must have run native C in exactly the slots whose disc
image is a native row's code (the first `span` bytes by SHA-256), and the
interpreter in the rest; a reference in interpreter mode must never run
native: anything else is an error, not a difference. The report gives, per record: identical, or the first differing frame counted
from the battle's start; the modules that ran (identifier and slot); and any
the interpreter could not run. The coverage check hashes the variant image
each slot holds once its load has landed and before its module can run
(record 40's second-star modules write a variable inside their own image)
against the disc's: every distinct image of the records asked for must
have been in its slot and called there, on both sides.

    python tools/pc/model_oracle.py --disc game/disc.bin                # all 621 records
    python tools/pc/model_oracle.py --records 34,100-119 --jobs 8
    python tools/pc/model_oracle.py --records gated                      # the records that have a native-C image
    python tools/pc/model_oracle.py --reference-mode "" --reverse-reference   # the default against itself
    python tools/pc/model_oracle.py --mode spark --records 34 --shots 1   # a difference, with pictures
    python tools/pc/model_oracle.py --records 34 --mod my-mod            # a code mod's C for its modules

With --mod DIR (repeatable) both sides run with that mod on, copied into
each game's own mods folder. The images the battles load are then the disc's
with the mods' "data" patches of MODEL.MRG applied (the only data --mod
reads), and a code mod's registered rows (mod API 29: the "model_module"
lines of the mods trace) are expected to run as "mod:<id>" in a default-mode
side wherever one hashes to the slot's image: before the disc's rows for an
OVER_DISC row, after them for the others. --no-mod-c runs the subject with
MEMORIES_MODEL_MOD_C=0 and expects no mod's C.

Exit status 0 when every battle agrees and every image was covered.
"""
from __future__ import annotations

import argparse
from array import array
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
import threading
import time
from typing import NamedTuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
from yfm_control import Game, GameHung, OUTPUT  # noqa: E402
import model_gate  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
MODEL_HEADER = ROOT / "src/game/model.h"
SECTOR = 2048
# A record's load (model_texture_transfer.c, func_80056D7C): stages 7-10 are
# the variant images, ten sectors each (alternative 0 slot A, 0 B, 1 A, 1 B),
# from sector 180 of the record; stages 11 and 12 the primary modules, two
# sectors each, for slot A and B.
VARIANT_SECTOR, VARIANT_SECTORS = 180, 10
PRIMARY_SECTOR, PRIMARY_SECTORS = 220, 2
VARIANT_BASE = {"A": 0x8013B000, "B": 0x8017B000}
PRIMARY_BASE = {"A": 0x8013A000, "B": 0x8017A000}
# D_800EF658's two records (main_modes.h, AnimatedBattleModelProperties):
# card id (the model is id - 1), field_02 (1: this side wins, as phase 5
# sets it), field_04, field_06 (the defender in defense), field_07 (the
# alternative: the card's second guardian star).
PROPERTIES = struct.Struct("<HhhBB")
# The model slots (model.h, ModelSlot): field_E14 is the slot's load step, 0
# while its disc transfer runs, 1 once every stage has landed (stage 16), then
# one more a frame through func_80056828's steps; step 9 may call the variant
# module's setup.
MODEL_SLOTS, MODEL_SLOT_SIZE, LOAD_STEP, FIRST_CALLING_STEP = "D_800F2C40", 0xE20, 0xE14, 9
OUTCOMES = {"tie": (0, 0), "attacker": (1, 0), "defender": (0, 1)}


def running_slots(outcome: str) -> str:
    """The slots whose variant module a battle runs: the attacker's (A)
    always, the defender's (B) unless the attacker wins."""
    return "A" if outcome == "attacker" else "AB"
ANIMATED_BATTLE = 1
CALL = re.compile(r"frame (\d+) vb \d+ model\] (primary|variant) slot=(\d) command=(-?\d+)"
                  r"(?: module (\d+) in ([AB]): (\S+))?")
CANNOT = re.compile(r"model (primary|variant) module (\d+) \(slot ([AB])\) cannot run")
# A code mod's row (mod API 29), as host->model_module logs it on the mods channel.
CLAIM = re.compile(r"mods\] (\S+): model_module (primary|variant) slot ([AB]) identifier (\d+) span (\d+) "
                   r"sha256 ([0-9a-f]{64})( over the disc)?")
# What the mods say about their models' control modules (mods.c's module notes).
MOD_NOTE = re.compile(r"(attack modules? .*(?:MIPS code|native C from mod)|model_module refused|"
                      r"MODEL\.MRG record \d+ \(card \d+\): .*|: native C from mod \S+)")
STAMP = re.compile(r"\[t \d+ us frame (\d+) ")
HASH = re.compile(r"(\d+) ([0-9a-f]{16})$")
# A primary module's words at +4 that are `return 2` (jr ra; addiu v0, zero, 2).
RETURN_TWO = struct.pack("<II", 0x03E00008, 0x24020002)


def model_ids(header: Path = MODEL_HEADER) -> list[int]:
    """The model ids that have a MODEL.MRG record, in record order
    (model_load_monster_merge.c: the gaps have none, ids above them are
    biased down)."""
    text = header.read_text(encoding="utf-8")
    value = {name: int(number, 0) for name, number in
             re.findall(r"#define (MODEL_MRG_\w+) (0x[0-9A-Fa-f]+|\d+)\s*$", text, re.M)}
    def gap(model: int) -> bool:
        return (value["MODEL_MRG_FIRST_GAP_START"] <= model < value["MODEL_MRG_FIRST_GAP_END"]
                or value["MODEL_MRG_SECOND_GAP_START"] <= model < value["MODEL_MRG_SECOND_GAP_END"]
                or model == value["MODEL_MRG_SINGLE_GAP_ID"])
    return [model for model in range(value["MODEL_MRG_ID_END"]) if not gap(model)]


MODELS = model_ids()
RECORDS = len(MODELS)


def record_card(record: int) -> int:
    """The card id whose 3D model is the record (D_800EF658 holds card ids)."""
    return MODELS[record] + 1


def parse_records(text: str | None) -> list[int]:
    """"34,100-119" -> [34, 100, ..., 119]; None or "all" -> every record."""
    if not text or text == "all":
        return list(range(RECORDS))
    records = []
    for part in text.split(","):
        first, _, last = part.strip().partition("-")
        for record in range(int(first), int(last or first) + 1):
            if not 0 <= record < RECORDS:
                raise ValueError(f"record {record}: MODEL.MRG has records 0-{RECORDS - 1}")
            if record not in records:
                records.append(record)
    return records


def parse_record_selection(text: str | None, disc: "Disc", alternatives: list[int]) -> list[int]:
    """parse_records, plus "gated": the records with a variant image, in the
    alternatives asked for, that config/pc/model_gate.csv runs as native C."""
    if text != "gated":
        return parse_records(text)
    return [record for record in range(RECORDS)
            if any(native_row(disc, record, alternative, slot) for alternative in alternatives for slot in "AB")]


def native_row(disc: "Disc", record: int, alternative: int, slot: str, image: bytes | None = None) \
        -> "model_gate.Row | None":
    """The native row whose code (the first `span` bytes of the image,
    SHA-256) the record's image in a slot has (or `image`, a model's), None
    when the interpreter is to run it."""
    if image is None:
        image = disc.variant(record, alternative, slot)
    for row in model_gate.native_rows():
        if hashlib.sha256(image[:row.span]).hexdigest() == row.sha256(slot):
            return row
    return None


def native_primary_row(disc: "Disc", record: int, slot: str, image: bytes | None = None) -> "model_gate.Row | None":
    """The same for the record's primary module (config/pc/model_primary_gate.csv)."""
    if image is None:
        image = disc.primary(record, slot)
    for row in model_gate.native_primary_rows():
        if hashlib.sha256(image[:row.span]).hexdigest() == row.sha256(slot):
            return row
    return None


class Battle(NamedTuple):
    record: int
    alternative: int
    outcome: str
    model: str = ""     # a mod's model (--models): `record` is then the record whose card fights as it
    card: int = 0       # what D_800EF658 holds for it: a card id, or a mod model's id (Disc.model_id)

    @property
    def key(self) -> str:
        return f"{f'model:{self.model}' if self.model else self.record}/{self.alternative}/{self.outcome}"

    @property
    def place(self) -> str:
        return f"model {self.model}" if self.model else f"record {self.record}"

    def properties(self) -> bytes:
        card, (first, second) = self.card or record_card(self.record), OUTCOMES[self.outcome]
        return (PROPERTIES.pack(card, first, 0, 0, self.alternative) +
                PROPERTIES.pack(card, second, 0, 0, self.alternative))


class Disc:
    """MODEL.MRG on a raw 2352-byte-sector image: the images a record loads."""

    def __init__(self, path: Path):
        self.path = path
        self.file = path.open("rb")
        self.lock = threading.Lock()
        self.lba, size = self._find(["DATA", "MODEL.MRG;1"])
        if size != RECORDS * 0x114 * SECTOR:
            raise SystemExit(f"{path}: MODEL.MRG is {size} bytes, not {RECORDS} records")
        self.patches: dict[int, int] = {}   # MODEL.MRG offset: byte, the --mod mods' "data" patches
        self.models: dict[str, bytes] = {}  # a --mod mod's model: its 44 module sectors (model_migrate.model_site)
        self.model_order: list[str] = []    # every model of the --mod mods, in their order

    def patch(self, manifest: dict, where: str) -> None:
        """A mod's "data" patches of MODEL.MRG, applied to what read() gives,
        later mods over earlier ones as the game applies them, and its
        "models", whose modules a battle of --models reads. Anything else a
        mod's data does is refused: the oracle would not see it."""
        import model_migrate
        for entry in manifest.get("models", []) or []:
            site = model_migrate.model_site(Path(where), entry)
            if site is None:
                raise SystemExit(f"--mod {where}: model {entry.get('name')} has no file")
            self.models[site.model] = site.stages   # a later mod's model of one name wins, as in the game
            self.model_order.append(site.model)
        for entry in manifest.get("data", []):
            name = str(entry.get("file", "")).upper().replace("/", "\\")
            if name not in ("\\DATA\\MODEL.MRG;1", "\\DATA\\MODEL.MRG") or "replace" in entry or "patch" not in entry:
                raise SystemExit(f"--mod {where}: the oracle reads only \"patch\" entries of \\DATA\\MODEL.MRG;1")
            for run in entry["patch"]:
                at = run["at"] if isinstance(run["at"], int) else int(str(run["at"]), 0)
                for n, byte in enumerate(bytes.fromhex(run["bytes"])):
                    self.patches[at + n] = byte

    def _sector(self, lba: int) -> bytes:
        self.file.seek(lba * 2352 + 24)
        return self.file.read(SECTOR)

    def _find(self, path: list[str]) -> tuple[int, int]:
        root = self._sector(16)[156:156 + 34]
        lba, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
        for name in path:
            found = None
            for n in range((size + SECTOR - 1) // SECTOR):
                data, at = self._sector(lba + n), 0
                while at < SECTOR and data[at]:
                    entry = data[at:at + data[at]]
                    if entry[33:33 + entry[32]].decode("latin-1").upper() == name:
                        found = struct.unpack_from("<I", entry, 2)[0], struct.unpack_from("<I", entry, 10)[0]
                    at += data[at]
            if not found:
                raise SystemExit(f"{name} is not on {self.path}")
            lba, size = found
        return lba, size

    def read(self, record: int, sector: int, count: int) -> bytes:
        with self.lock:
            data = b"".join(self._sector(self.lba + record * 0x114 + sector + n) for n in range(count))
        if self.patches:
            first = (record * 0x114 + sector) * SECTOR
            changed = bytearray(data)
            for at, byte in self.patches.items():
                if first <= at < first + len(changed):
                    changed[at - first] = byte
            data = bytes(changed)
        return data

    def variant(self, record: int, alternative: int, slot: str) -> bytes:
        stage = 2 * alternative + (slot == "B")
        return self.read(record, VARIANT_SECTOR + VARIANT_SECTORS * stage, VARIANT_SECTORS)

    def primary(self, record: int, slot: str) -> bytes:
        return self.read(record, PRIMARY_SECTOR + PRIMARY_SECTORS * (slot == "B"), PRIMARY_SECTORS)

    def battle_variant(self, battle: "Battle", slot: str) -> bytes:
        """The variant image a battle loads in a slot: its record's, or its model's."""
        if not battle.model:
            return self.variant(battle.record, battle.alternative, slot)
        at = (VARIANT_SECTORS * (2 * battle.alternative + (slot == "B"))) * SECTOR
        return self.models[battle.model][at:at + VARIANT_SECTORS * SECTOR]

    def model_id(self, name: str) -> int:
        """What a card that names model `name` has as its model, which the
        battle reads in D_800EF658: CARDS_MODEL_MOD_FIRST (0x8000, src/pc/cards/
        cards.h) plus one plus the model's place among every mod's models
        (the --mod mods load in the order given: a later model of one name
        is the one named)."""
        return 0x8001 + len(self.model_order) - 1 - self.model_order[::-1].index(name)

    def battle_primary(self, battle: "Battle", slot: str) -> bytes:
        if not battle.model:
            return self.primary(battle.record, slot)
        at = (PRIMARY_SECTOR - VARIANT_SECTOR + PRIMARY_SECTORS * (slot == "B")) * SECTOR
        return self.models[battle.model][at:at + PRIMARY_SECTORS * SECTOR]


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def stage_of(alternative: int, slot: str) -> int:
    return 7 + 2 * alternative + (slot == "B")


# --- one side ---------------------------------------------------------------------

class Side:
    """One configuration (an executable and its MEMORIES_* variables) running
    battles, each from the debug menu's state. A game that hangs or closes is
    replaced by a new one; each game's files are in a folder of its own, as
    its frame count starts again."""

    def __init__(self, name: str, executable: Path | None, env: dict[str, str], out: Path, disc: Path,
                 chunk: int, cap: int, mods: list[tuple[Path, str]] | None = None):
        self.name, self.executable, self.env = name, executable, env
        self.out = out.resolve()   # the game runs elsewhere: MEMORIES_FRAME_HASHES must be absolute
        self.disc, self.chunk, self.cap = disc, chunk, cap
        self.mods = mods or []     # (folder, id): copied into each game's own mods folder and turned on
        self.games = 0
        self.lock = threading.Lock()

    def _start(self, label: str) -> Game:
        with self.lock:   # games of one side start from several threads
            folder = self.out / f"{label}-game{self.games}"
            self.games += 1
        shutil.rmtree(folder, ignore_errors=True)
        (folder / "mods").mkdir(parents=True)
        env = {**self.env, "MEMORIES_DISC": str(self.disc), "MEMORIES_TRACE": "model,mods" if self.mods else "model",
               "MEMORIES_FRAME_HASHES": str(folder / "hashes.txt")}
        for directory, identifier in self.mods:
            shutil.copytree(directory, folder / "mods" / directory.name)
            env[mod_variable(identifier)] = "1"
        game = Game(self.executable, out=folder, mods_dir=folder / "mods", env=env, command_timeout=120)
        game.goto("debug")
        game.save("base.state")
        game.base_hash = game.hash()
        return game

    def run(self, label: str, battles: list[Battle], keep: bool) -> dict[str, dict]:
        """{battle key: result}; the game's files are read here and, unless
        `keep`, deleted."""
        results: dict[str, dict] = {}
        game = None
        for battle in battles:
            result = {"side": self.name, "battle": battle.key}
            try:
                if game is None:
                    game = self._start(label)
                result["folder"] = str(game.out)
                result["base_hash"] = f"{game.base_hash:016x}"
                game.load("base.state")
                game.poke("D_800EF658", battle.properties())
                game.poke("D_8009B269", bytes([0]))          # back to the debug menu after
                game.poke("D_8009B26C", bytes([ANIMATED_BATTLE]))
                start, vblank = game.frame, game.vblank
                # Each slot's images are hashed once its transfer has landed
                # and before its modules can have run (one module writes a
                # variable inside its own image): a VBlank at a time until then.
                slots = game.address(MODEL_SLOTS)
                requested, images, primary, captured = set(), {}, {}, {}
                while game.u8("D_8009B26C") & 0x1F == ANIMATED_BATTLE:
                    if game.vblank - vblank >= self.cap:
                        raise TimeoutError(f"the battle did not end in {self.cap} VBlanks")
                    for index, slot in enumerate("AB"):
                        if slot in captured:
                            continue
                        step = game.u8(slots + index * MODEL_SLOT_SIZE + LOAD_STEP)
                        if step == 0:
                            requested.add(slot)
                        elif slot in requested and step < FIRST_CALLING_STEP:
                            base = VARIANT_BASE[slot]
                            images[slot] = sha(game.peek(base, 0x4000) + game.peek(base + 0x4000, 0x1000))
                            primary[slot] = sha(game.peek(PRIMARY_BASE[slot], 0x1000))
                            captured[slot] = game.frame - start
                    game.step(1 if len(captured) < 2 else self.chunk)
                result.update(start=start, end=game.frame, captured=captured)
                for slot in "AB":   # not seen loading (a step missed): as the battle left them
                    if slot not in captured:
                        base = VARIANT_BASE[slot]
                        images[slot] = sha(game.peek(base, 0x4000) + game.peek(base + 0x4000, 0x1000))
                        primary[slot] = sha(game.peek(PRIMARY_BASE[slot], 0x1000))
                result["images"], result["primary_images"] = images, primary
            except (GameHung, RuntimeError, OSError) as error:   # ControlError and TimeoutError among them
                result["error"] = f"{type(error).__name__}: {str(error).splitlines()[0]}"
                if game is not None:
                    result.setdefault("start", game.frame)
                    result.setdefault("end", game.frame)
                    game.close()
                    game = None
            results[battle.key] = result
        if game is not None:
            game.quit()
        folders: dict[str, list[dict]] = {}
        for result in results.values():
            if "folder" in result:
                folders.setdefault(result["folder"], []).append(result)
        for folder, owned in folders.items():
            read_traces(Path(folder), owned)
            if not keep:
                for name in ("hashes.txt", "game.log", "base.state"):
                    remove(Path(folder) / name)
        return results


def remove(path: Path) -> None:
    """Delete a game's file; on Windows the game may hold it for a moment
    after it was ended. One that stays is left."""
    for attempt in range(20):
        try:
            path.unlink(missing_ok=True)
            return
        except PermissionError:
            time.sleep(0.25)


def read_traces(folder: Path, results: list[dict]) -> None:
    """Each result's frame hashes (after its start frame, to its end) and
    module calls, from one game's files."""
    hashes = {}
    try:
        for line in (folder / "hashes.txt").read_text().splitlines():
            match = HASH.match(line)
            if match:
                hashes[int(match.group(1))] = int(match.group(2), 16)
    except OSError:
        pass
    calls: list[tuple] = []
    cannot: list[tuple[int, str]] = []
    claims: list[tuple] = []
    notes: list[str] = []
    frame = 0
    try:
        log = (folder / "game.log").read_text(errors="replace").splitlines()
    except OSError:
        log = []
    for line in log:
        stamp = STAMP.search(line)
        if stamp:
            frame = int(stamp.group(1))
        match = CLAIM.search(line)
        if match:
            claims.append((match.group(2), match.group(3), int(match.group(4)), int(match.group(5)), match.group(6),
                           bool(match.group(7)), match.group(1)))
            continue
        match = MOD_NOTE.search(line)
        if match and line.split("] ", 1)[-1] not in notes:
            notes.append(line.split("] ", 1)[-1])
        match = CALL.search(line)
        if match:
            kind, active, command = match.group(2), int(match.group(3)), int(match.group(4))
            slot = match.group(6) or "AB"[active]          # an older build names only the active slot
            module = int(match.group(5)) if match.group(5) else None
            calls.append((int(match.group(1)), kind, slot, module, command, match.group(7) or "?"))
            continue
        match = CANNOT.search(line)
        if match:
            cannot.append((frame, f"{match.group(1)} {match.group(2)} in {match.group(3)}"))
    for result in results:
        start, end = result.get("start", 0), result.get("end", 0)
        result["hashes"] = array("Q", (hashes.get(f, 0) for f in range(start + 1, end + 1)))
        mine = [call for call in calls if start < call[0] <= end]
        result["calls"] = [(call[0] - start, *call[1:5]) for call in mine if call[4] >= 0]
        updates: dict[str, int] = {}
        paths: dict[str, set] = {}
        for call in mine:
            name = f"{call[1]} {call[2]}"
            if call[4] < 0:
                updates[name] = updates.get(name, 0) + 1
            paths.setdefault(f"{call[1]} {call[3]} in {call[2]}", set()).add(call[5])
        result["updates"] = updates
        result["paths"] = {module: sorted(ways) for module, ways in paths.items()}
        result["cannot_run"] = sorted({what for at, what in cannot if start <= at <= end})
        result["claims"] = claims   # the game's, from its start: every battle it ran has them
        result["mod_notes"] = notes


# --- comparing --------------------------------------------------------------------

def first_difference(mine: array, theirs: array) -> int | None:
    """The first frame (1: the first after the battle's start) where the two
    differ, a missing frame counting as different; None when they agree."""
    for index, (a, b) in enumerate(zip(mine, theirs)):
        if a != b:
            return index + 1
    if len(mine) != len(theirs):
        return min(len(mine), len(theirs)) + 1
    return None


def backends(side: dict, kind: str = "variant") -> dict[str, set[str]]:
    """{slot: what ran its variant (or primary) module calls} from a side's paths."""
    found: dict[str, set[str]] = {}
    for module, ways in side["paths"].items():
        what, _, slot = module.rpartition(" in ")
        if what.startswith(kind + " "):
            found.setdefault(slot, set()).update(ways)
    return found


def backend_errors(side: dict, expected: dict[str, bool | str], native_allowed: bool,
                   kind: str = "variant") -> list[str]:
    """Where a side ran something other than what the table says: native C
    in the slots whose image is a native row's code, if it is a default-mode
    side, and the interpreter in the others; never native for a side that
    asked for the interpreter or the stand-in. An expected "mod:<id>" (--mod)
    is a slot whose image a code mod's row covers: a default-mode side must
    have run that mod's C there, and no mod's C anywhere else."""
    errors = []
    ran = backends(side, kind)
    what = "primary " if kind == "primary" else ""   # the variants' messages keep their words
    # A slot that holds a native row's code and made no variant call at all
    # is an error too (compare() has already said so for the slots a battle's
    # outcome runs; expected holds only those).
    for slot in sorted(expected):
        # A primary module is called only by the records with one (the disc
        # calls no `return 2`): a slot that made no primary call is no error.
        if native_allowed and expected[slot] and slot not in ran and kind == "variant":
            errors.append(f"{side['side']}: slot {slot} holds a module the table runs as native C and made no call")
    for slot, ways in sorted(ran.items()):
        want = expected.get(slot, False) if native_allowed else False
        mods = sorted(way for way in ways if way.startswith("mod:"))
        if isinstance(want, str):
            if ways != {want}:
                errors.append(f"{side['side']}: slot {slot} holds a {what}module {want[4:]}'s row runs as its C and "
                              f"ran {', '.join(sorted(ways))}")
        elif want and ways != {"native"}:
            errors.append(f"{side['side']}: slot {slot} holds a {what}module the table runs as native C and ran "
                          f"{', '.join(sorted(ways))}")
        elif not want and "native" in ways:
            errors.append(f"{side['side']}: slot {slot} ran native C{' for its primary module' if what else ''} where the table says "
                          f"{'interpreter' if native_allowed else 'the mode asks for no native C'}")
        if not isinstance(want, str) and mods:
            errors.append(f"{side['side']}: slot {slot} ran a mod's C ({', '.join(mods)}) where "
                          f"{'no mod row covers its module' if native_allowed else 'the mode asks for no native C'}")
    return errors


def mod_variable(identifier: str) -> str:
    """The variable that turns a mod on (mods.c, environment_choice)."""
    return "MEMORIES_MOD_" + re.sub(r"[^A-Z0-9]", "_", identifier.upper())


def expected_way(image: bytes, slot: str, kind: str, disc_native: bool, claims: list[tuple]) -> bool | str:
    """What a default-mode side runs a slot's module with, for compare():
    "mod:<id>" where a code mod's row covers the image (an OVER_DISC row
    before the disc's rows, the others after them; of two, the one
    registered later), else whether a disc row does (True: native C). A
    primary that is `return 2` the game answers itself after the disc's rows
    and before the others (model_modules.c, return_two_row): native."""
    identifier = int.from_bytes(image[:4], "little")
    if kind == "primary" and image[4:12] == RETURN_TWO:
        disc_native = True
    def claimed(over: bool) -> str | None:
        found = None
        for claim_kind, claim_slot, claim_identifier, span, digest, claim_over, owner in claims:
            if (claim_kind, claim_slot, claim_identifier, claim_over) == (kind, slot, identifier, over) and \
                    sha(image[:span]) == digest:
                found = "mod:" + owner
        return found
    return claimed(True) or disc_native or claimed(False) or False


def compare(subject: dict, reference: dict, holds_native: dict[str, bool] | None = None,
            native_allowed: tuple[bool, bool] = (False, False),
            holds_native_primary: dict[str, bool] | None = None) -> dict:
    """What one battle's two runs say: its verdict and why. `holds_native` says
    which slots hold a native row's code, `holds_native_primary` which hold a
    native primary row's; `native_allowed` whether each side (subject,
    reference) is in the mode that runs native C."""
    verdict = {"battle": subject["battle"], "frames": [len(subject.get("hashes", ())), len(reference.get("hashes", ()))],
               # Each side's frame hashes in one digest, to compare runs (test_model_mod_c.py).
               "digests": [hashlib.sha256(side["hashes"].tobytes()).hexdigest()[:16] if side.get("hashes") else ""
                           for side in (subject, reference)],
               # The subject's setup commands ("variant A 3"): the entries the battle's attacks pick.
               "setups": sorted({f"{kind} {slot} {command}" for _, kind, slot, _, command in subject.get("calls", ())})}
    errors = [f"{side['side']}: {side['error']}" for side in (subject, reference) if side.get("error")]
    errors += [f"{side['side']}: no frame hash for {side['hashes'].count(0)} of its {len(side['hashes'])} frames"
               for side in (subject, reference) if not side.get("error") and
               (not side.get("hashes") or 0 in side["hashes"])]
    if not errors:
        # Two runs where no module ran (a battle that never started) agree
        # on everything: such a battle is an error, not a match.
        expected = running_slots(subject["battle"].split("/")[-1])
        for side in (subject, reference):
            called = {where for _, kind, where, _, _ in side["calls"] if kind == "variant"}
            errors += [f"{side['side']}: no variant module called in slot {slot}"
                       for slot in expected if slot not in called]
    if not errors and holds_native is not None:
        for side, allowed in zip((subject, reference), native_allowed):
            errors += backend_errors(side, holds_native, allowed)
            if holds_native_primary is not None:
                errors += backend_errors(side, holds_native_primary, allowed, "primary")
    if errors:
        verdict.update(status="error", why="; ".join(errors))
        return verdict
    verdict["backends"] = {name: {slot: sorted(ways) for slot, ways in backends(side).items()}
                           for name, side in (("subject", subject), ("reference", reference))}
    differing = first_difference(subject["hashes"], reference["hashes"])
    calls_same = subject["calls"] == reference["calls"] and subject["updates"] == reference["updates"]
    verdict["modules"] = sorted({f"{kind} {module} in {slot}" for _, kind, slot, module, _ in subject["calls"]})
    verdict["cannot_run"] = sorted(set(subject["cannot_run"]) | set(reference["cannot_run"]))
    verdict["paths"] = {"subject": subject["paths"], "reference": reference["paths"]}
    if differing is None and calls_same:
        verdict["status"] = "identical"
    else:
        verdict["status"] = "differs"
        why = []
        if differing is not None:
            verdict["first_differing_frame"] = differing
            why.append(f"first differing frame +{differing} (frame {subject['start'] + differing} against "
                       f"{reference['start'] + differing}; {len(subject['hashes'])} and "
                       f"{len(reference['hashes'])} frames)")
        if not calls_same:
            first = next((i for i, (a, b) in enumerate(zip(subject["calls"], reference["calls"])) if a != b),
                         min(len(subject["calls"]), len(reference["calls"])))
            mine = subject["calls"][first] if first < len(subject["calls"]) else None
            theirs = reference["calls"][first] if first < len(reference["calls"]) else None
            why.append(f"module calls differ from call {first + 1}: {mine} against {theirs}"
                       if mine != theirs else
                       f"updates {subject['updates']} against {reference['updates']}")
        verdict["why"] = "; ".join(why)
    return verdict


def coverage(battles: list[Battle], subject: dict, reference: dict, disc: Disc) -> dict:
    """Which distinct variant (and primary) images of the records asked for
    were in their slot and called there, on both sides; slot B's only from
    battles whose outcome runs it."""
    on_disc = {"A": {}, "B": {}}
    covered = {"A": set(), "B": set()}
    primary_disc = {"A": set(), "B": set()}
    primary_seen = {"A": set(), "B": set()}
    mismatched = []
    for battle in battles:
        for slot in running_slots(battle.outcome):   # slot B's image only where its module runs
            image = sha(disc.battle_variant(battle, slot))
            on_disc[slot].setdefault(image, f"{battle.place} stage {stage_of(battle.alternative, slot)}")
            primary = sha(disc.battle_primary(battle, slot))
            primary_disc[slot].add(primary)
            ran = True
            for side in (subject[battle.key], reference[battle.key]):
                if side.get("error"):
                    ran = False
                    continue
                first_call = min((at for at, kind, where, _, _ in side["calls"]
                                  if kind == "variant" and where == slot), default=None)
                taken = side.get("captured", {}).get(slot)
                if taken is None or (first_call is not None and first_call <= taken):
                    mismatched.append(f"{battle.key} slot {slot} ({side['side']}): its image was not hashed "
                                      f"before its module ran")
                    ran = False
                elif side["images"][slot] != image:
                    mismatched.append(f"{battle.key} slot {slot} ({side['side']}): not the "
                                      f"{'model' if battle.model else 'disc'}'s stage "
                                      f"{stage_of(battle.alternative, slot)} image")
                    ran = False
                if first_call is None:
                    ran = False
                if side["primary_images"][slot] == primary and \
                        any(kind == "primary" and where == slot for _, kind, where, _, _ in side["calls"]):
                    primary_seen[slot].add(primary)
            if ran:
                covered[slot].add(image)
    report = {"variant": {}, "primary": {}, "mismatched": mismatched}
    for slot in "AB":
        missing = sorted(where for image, where in on_disc[slot].items() if image not in covered[slot])
        report["variant"][slot] = {"distinct": len(on_disc[slot]), "covered": len(covered[slot]), "missing": missing}
        report["primary"][slot] = {"distinct": len(primary_disc[slot]), "called": len(primary_seen[slot])}
    return report


# --- pictures ---------------------------------------------------------------------

def pictures(side: Side, battle: Battle, offsets: list[int], folder: Path) -> list[Path]:
    """The battle's frames at these offsets from its start, as PNGs."""
    game = side._start(f"pictures-{battle.model or battle.record}-{battle.alternative}")
    shots = []
    try:
        game.load("base.state")
        game.poke("D_800EF658", battle.properties())
        game.poke("D_8009B269", bytes([0]))
        game.poke("D_8009B26C", bytes([ANIMATED_BATTLE]))
        start = game.frame
        for offset in sorted(offsets):
            while game.frame < start + offset:
                game.step(1)
            name = (f"{side.name}-{battle.place.replace(' ', '')}-alt{battle.alternative}-{battle.outcome}"
                    f"-frame+{offset}.png")
            shots.append(game.shot(folder / name))
    finally:
        game.close()
    return shots


# --- main -------------------------------------------------------------------------

def side_env(mode: str | None) -> dict[str, str]:
    return {"MEMORIES_MODEL_MODULES": mode} if mode else {}


def shards(battles: list[Battle], size: int) -> list[list[Battle]]:
    return [battles[at:at + size] for at in range(0, len(battles), size)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--disc", type=Path, default=os.environ.get("MEMORIES_DISC"),
                        help="the disc image (default: MEMORIES_DISC)")
    parser.add_argument("--records", help="records of MODEL.MRG, 0-620 (\"34,100-119\"; default all; \"gated\": "
                                          "the records that have an image config/pc/model_gate.csv runs as native C)")
    parser.add_argument("--alternatives", default="0,1", help="guardian-star alternatives (default 0,1)")
    parser.add_argument("--outcomes", default="tie",
                        help="tie (both modules run), attacker (only slot A's), defender; comma-separated")
    parser.add_argument("--executable", type=Path, help="the subject's game (default yfm_control's)")
    parser.add_argument("--reference-executable", type=Path, help="the reference's game (default the subject's)")
    parser.add_argument("--mode", default="", help="the subject's MEMORIES_MODEL_MODULES (default unset)")
    parser.add_argument("--reference-mode", default="interpreter",
                        help="the reference's MEMORIES_MODEL_MODULES (default interpreter; \"\" for unset)")
    parser.add_argument("--reverse-reference", action="store_true",
                        help="the reference runs the battles in the opposite order, in other games (a battle "
                             "whose module reads stack it never wrote then differs: notes/pc-build.md)")
    parser.add_argument("--poison", help="the subject fills its stack with this byte (\"0xA5\") before each native "
                                         "module call (MEMORIES_MODEL_STACK_POISON, a build with test hooks): C that "
                                         "reads a local it never wrote then takes the byte, and differs from the "
                                         "interpreter for one byte or another (run it with 0x00 and 0xFF)")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1), help="games at once")
    parser.add_argument("--shard", type=int, default=12, help="battles per game (default 12)")
    parser.add_argument("--chunk", type=int, default=16, help="VBlanks a step (default 16)")
    parser.add_argument("--cap", type=int, default=12000, help="VBlanks a battle may take (default 12000)")
    parser.add_argument("--shots", type=int, default=0,
                        help="pictures of up to this many differing battles, both sides, at the first "
                             "differing frame, the one before and the one after (the hash sees the frame being "
                             "drawn, the screen shows it a frame later)")
    parser.add_argument("--pictures", help="frame offsets to picture the first battle at, both sides (\"300,900\")")
    parser.add_argument("--out", type=Path, help="where it all goes (default a new tmp/pc/control/model-oracle-*)")
    parser.add_argument("--keep", action="store_true", help="keep each game's log, frame hashes and state")
    parser.add_argument("--mod", action="append", type=Path, default=[],
                        help="a mod's folder, on for both sides, copied into each game's own mods folder "
                             "(repeatable; its \"data\" may only patch MODEL.MRG)")
    parser.add_argument("--models", help="models of the --mod mods (\"models\" in mod.json), by name and "
                                         "comma-separated: each fights by its model id, as a card that names it "
                                         "would, its own modules in both slots. With "
                                         "--models and no --records, only the models are battled")
    parser.add_argument("--no-mod-c", action="store_true",
                        help="the subject runs with MEMORIES_MODEL_MOD_C=0: no mod's C is expected")
    arguments = parser.parse_args()
    if not arguments.disc or not Path(arguments.disc).is_file():
        parser.error("no disc image: --disc or MEMORIES_DISC")
    disc = Disc(Path(arguments.disc).resolve())
    mods = []
    for directory in arguments.mod:
        directory = directory.resolve()
        try:
            manifest = json.loads((directory / "mod.json").read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            parser.error(f"--mod {directory}: {error}")
        disc.patch(manifest, str(directory))
        mods.append((directory, str(manifest["id"])))
    alternatives = [int(a) for a in arguments.alternatives.split(",")]
    if arguments.models and not arguments.records:
        records = []
    else:
        records = parse_record_selection(arguments.records, disc, alternatives)
    models = [name.strip() for name in (arguments.models or "").split(",") if name.strip()]
    for name in models:
        if name not in disc.models:
            parser.error(f"--models: no --mod mod has a model named {name!r}")
    outcomes = [o.strip() for o in arguments.outcomes.split(",")]
    for outcome in outcomes:
        if outcome not in OUTCOMES:
            parser.error(f"--outcomes: {outcome}? ({', '.join(OUTCOMES)})")
    battles = [Battle(r, a, o) for r in records for a in alternatives for o in outcomes]
    battles += [Battle(0, a, o, name, disc.model_id(name)) for name in models
                for a in alternatives for o in outcomes]
    if arguments.out:
        out = arguments.out.resolve()
        out.mkdir(parents=True, exist_ok=True)
    else:
        OUTPUT.mkdir(parents=True, exist_ok=True)
        out = Path(tempfile.mkdtemp(prefix="model-oracle-", dir=OUTPUT))
    subject_exe = arguments.executable
    reference_exe = arguments.reference_executable or subject_exe
    subject_env = side_env(arguments.mode)
    if arguments.poison is not None:
        try:
            if not 0 <= int(arguments.poison, 0) <= 0xFF:
                raise ValueError
        except ValueError:
            parser.error(f"--poison: {arguments.poison}? (a byte, 0x00 to 0xFF)")
        subject_env["MEMORIES_MODEL_STACK_POISON"] = arguments.poison
    if arguments.no_mod_c:
        subject_env["MEMORIES_MODEL_MOD_C"] = "0"
    sides = {"subject": Side("subject", subject_exe, subject_env, out / "subject", disc.path,
                             arguments.chunk, arguments.cap, mods),
             "reference": Side("reference", reference_exe, side_env(arguments.reference_mode), out / "reference",
                               disc.path, arguments.chunk, arguments.cap, mods)}
    reference_battles = battles[::-1] if arguments.reverse_reference else battles
    reference_size = arguments.shard + 1 if arguments.reverse_reference else arguments.shard
    tasks = [("subject", f"shard{n:03}", part) for n, part in enumerate(shards(battles, arguments.shard))]
    tasks += [("reference", f"shard{n:03}", part)
              for n, part in enumerate(shards(reference_battles, reference_size))]
    tasks.sort(key=lambda task: task[1])   # both sides of the first shards first
    began = time.monotonic()
    print(f"model oracle: {len(battles)} battles ({len(records)} records"
          f"{f' and {len(models)} models' if models else ''} x {len(alternatives)} alternatives x "
          f"{len(outcomes)} outcomes), subject {arguments.mode or 'default'}, reference "
          f"{arguments.reference_mode or 'default'}, {arguments.jobs} games at once; {out}", flush=True)
    results = {"subject": {}, "reference": {}}
    done = [0]
    lock = threading.Lock()

    def work(task):
        name, label, part = task
        try:
            found = sides[name].run(label, part, arguments.keep)
        except Exception as error:   # one shard's failure must not end the sweep
            found = {battle.key: {"side": name, "battle": battle.key,
                                  "error": f"{type(error).__name__}: {error}"} for battle in part}
        with lock:
            results[name].update(found)
            done[0] += len(part)
            print(f"  {done[0]}/{2 * len(battles)} battles, {time.monotonic() - began:.0f} s", flush=True)

    with ThreadPoolExecutor(arguments.jobs) as pool:
        for future in [pool.submit(work, task) for task in tasks]:
            future.result()
    elapsed = time.monotonic() - began
    # The slots whose disc image is a native row's code: where a default-mode
    # side must have run the decomp's C (compare).
    expected = {b.key: {slot: native_row(disc, b.record, b.alternative, slot, disc.battle_variant(b, slot)) is not None
                        for slot in running_slots(b.outcome)} for b in battles}
    allowed = tuple(mode not in ("interpreter", "spark") for mode in (arguments.mode, arguments.reference_mode))
    primaries = {b.key: {slot: native_primary_row(disc, b.record, slot, disc.battle_primary(b, slot)) is not None
                         for slot in "AB"}
                 for b in battles}
    # With --mod: the rows the mods registered (each game logs them; the
    # subject's, which the expectation is for) and where they run instead.
    claims = list(dict.fromkeys(claim for result in results["subject"].values() for claim in result.get("claims", ())))
    mod_notes = sorted({note for side in results.values() for result in side.values()
                        for note in result.get("mod_notes", ())})
    if claims and not arguments.no_mod_c:
        for b in battles:
            for slot in running_slots(b.outcome):
                expected[b.key][slot] = expected_way(disc.battle_variant(b, slot), slot, "variant",
                                                     expected[b.key][slot], claims)
            for slot in "AB":
                primaries[b.key][slot] = expected_way(disc.battle_primary(b, slot), slot, "primary",
                                                      primaries[b.key][slot], claims)
    verdicts = [compare(results["subject"][b.key], results["reference"][b.key], expected[b.key], allowed,
                        primaries[b.key]) for b in battles]
    cover = coverage(battles, results["subject"], results["reference"], disc)
    bases = sorted({r["base_hash"] for side in results.values() for r in side.values() if "base_hash" in r})

    lines = []
    by_record: dict[str, list[dict]] = {}
    for battle, verdict in zip(battles, verdicts):
        by_record.setdefault(f"model {battle.model} (model id 0x{battle.card:X})" if battle.model else
                             f"record {battle.record} (card {record_card(battle.record)})", []).append(verdict)
    for record, found in by_record.items():
        modules = sorted({m for v in found for m in v.get("modules", ())})
        cannot = sorted({c for v in found for c in v.get("cannot_run", ())})
        bad = [v for v in found if v["status"] != "identical"]
        text = (f"{record}: " +
                ("identical" if not bad else "; ".join(f"{v['battle']} {v['status']}: {v.get('why', '')}" for v in bad)) +
                f" | ran: {', '.join(modules) or 'nothing'}" + (f" | cannot run: {', '.join(cannot)}" if cannot else ""))
        lines.append(text)
    identical = sum(v["status"] == "identical" for v in verdicts)
    summary = [f"model oracle: {identical} of {len(verdicts)} battles identical "
               f"({sum(v['status'] == 'differs' for v in verdicts)} differ, "
               f"{sum(v['status'] == 'error' for v in verdicts)} errors) in {elapsed:.0f} s; "
               f"base state hash {', '.join(bases)}"]
    for slot in "AB":
        v, p = cover["variant"][slot], cover["primary"][slot]
        summary.append(f"  slot {slot}: {v['covered']} of {v['distinct']} distinct variant images covered"
                       + (f" (missing: {', '.join(v['missing'][:12])}{' ...' if len(v['missing']) > 12 else ''})"
                          if v["missing"] else "") +
                       f"; {p['called']} of {p['distinct']} primary images called")
    if cover["mismatched"]:
        summary.append(f"  {len(cover['mismatched'])} slots did not hold the disc's image: "
                       + "; ".join(cover["mismatched"][:6]))
    ran = {"subject": {}, "reference": {}}
    for verdict in verdicts:
        for name, slots in verdict.get("backends", {}).items():
            for ways in slots.values():
                for way in ways:
                    ran[name][way] = ran[name].get(way, 0) + 1
    summary.append("  backends (battle slots; what ran the variant module): " + "; ".join(
        f"{name} " + (", ".join(f"{way} {count}" for way, count in sorted(counts.items())) or "none")
        for name, counts in ran.items()) +
        f"; {sum(held is True for slots in expected.values() for held in slots.values())} of them hold a native row's code"
        + (f", {sum(isinstance(held, str) for slots in expected.values() for held in slots.values())} a mod row's"
           if claims else ""))
    if mods:
        summary.append(f"  mods: {', '.join(identifier for _, identifier in mods)}; {len(claims)} module rows registered"
                       + (" (the subject runs with MEMORIES_MODEL_MOD_C=0)" if arguments.no_mod_c else ""))
        summary.extend(f"  mod note: {note}" for note in mod_notes[:12])
    stand_in = sorted({f"{v['battle']}: {module}" for v in verdicts for side in ("subject", "reference")
                       if (side == "subject" and arguments.mode != "spark") or
                       (side == "reference" and arguments.reference_mode != "spark")
                       for module, ways in v.get("paths", {}).get(side, {}).items() if "stand-in" in ways})
    if stand_in:
        summary.append(f"  the stand-in ran in {len(stand_in)} module calls outside spark mode: "
                       + "; ".join(stand_in[:6]))
    text = "\n".join(lines + summary)
    (out / "report.txt").write_text(text + "\n", encoding="utf-8")
    with (out / "report.json").open("w", encoding="utf-8") as file:
        json.dump({"arguments": {k: str(v) for k, v in vars(arguments).items()}, "elapsed": elapsed,
                   "verdicts": verdicts, "coverage": cover, "base_hashes": bases,
                   "mods": [identifier for _, identifier in mods], "claims": claims, "mod_notes": mod_notes},
                  file, indent=1)
    print("\n".join(lines if len(lines) <= 40 else [l for l in lines if "identical |" not in l][:200]))
    print("\n".join(summary))

    if arguments.shots or arguments.pictures:
        folder = out / "pictures"
        wanted = []
        if arguments.pictures:
            wanted.append((battles[0], [int(x) for x in arguments.pictures.split(",")]))
        differing = [(b, v) for b, v in zip(battles, verdicts) if v.get("first_differing_frame")]
        for battle, verdict in differing[:arguments.shots]:
            frame = verdict["first_differing_frame"]
            wanted.append((battle, sorted({max(1, frame - 1), frame, frame + 1})))
        for battle, offsets in wanted:
            for side in sides.values():
                for shot in pictures(side, battle, offsets, folder):
                    print(f"  picture: {shot}")
    complete = all(cover["variant"][slot]["covered"] == cover["variant"][slot]["distinct"] for slot in "AB")
    return 0 if identical == len(verdicts) and complete and not cover["mismatched"] and not stand_in else 1


if __name__ == "__main__":
    raise SystemExit(main())
