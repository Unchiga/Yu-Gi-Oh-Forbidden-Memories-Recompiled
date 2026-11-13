#!/usr/bin/env python3
"""From MIPS to C: a mod's battle-model control modules moved to the port's
own C (mod API 29; notes/modding.md, "Moving a mod's attacks from MIPS to C").
Run as `model_gate.py mod-migrate` and `model_gate.py mod-row`.

mod-migrate reads a mod (its folder, with mod.json) and the disc. The
control modules it changes are the ones the game's module note checks
(src/pc/mods/mods.c): those of each "models" file, and those of each record
of MODEL.MRG its "data" replaces or patches in sectors 180 to 223. Each is
classified as src/pc/overlays/model_module_class.c classifies it (classify()
below; tools/pc/test_model_migrate.py holds the two to the same answers):

  NATIVE  the disc's code and the bytes the disc's C hashes: it runs already
  RETAIL  the disc's code of a shape the port still interprets: the port's
  EMPTY   no entry (its word at +4 is 0): the stand-in
  TABLES  a native shape's code with other tables: the C reads them
  CODE    the code of a disc shape, changed
  NEW     a first word no disc module of the slot has

For a TABLES or CODE module the source is the disc's shape with its first
word (of several, the one whose code differs least). Every byte that differs
from the disc's module of that shape is one of:

  (a) code: below the shape's data start and in no const the C defines.
      The C was written for the disc's code: the module is reported, with
      the functions that differ, to be ported by hand, and no C is made;
  (b) a const the port's C defines (a copy of bytes inside the hashed
      span, which the C would read from its own definition and so ignore
      the mod's change): the generated C defines it with the mod's bytes;
  (c) data the C reads from the record (from the data start on): the C
      stands as it is.

A module whose differing bytes are all (b) and (c) gets the shape's C:
  model_c/<prefix>/...  the shape's files from src/native_overlays (slot A's
                        C, slot B's wrapper, their headers), their includes
                        made relative to the copy;
  <prefix>.h            the names the module's C defines, as names of its own
                        (a mod is one object: two shapes' func_8013B004 would
                        clash), the game's resident functions under the names
                        a mod gets them by, and prototypes for the functions
                        libgte.h declares without one;
  <prefix>_<n>.c        one unit for each C file of the module;
  <prefix>_data.c       the module's rule 6 storage, the mod's own (never the
                        port's gModel... variables), and the (b) consts;
  model_rows.c          a MemoriesModelModule row for each module: its
                        identifier, the shape's span, the SHA-256 of the
                        mod's bytes and the shape's command bound; and
                        MemoriesModInit, which registers them
                        (MemoriesModelRows_Register when the mod has its own
                        MemoriesModInit, which must call it).

Every file it writes starts with WRITTEN, and the next run removes those
first. mod.json gains "library" and "min_api": 29 when it lacks them. The
report says, per module, what it is, where it came from, what differs, what
was generated and what needs porting by hand, then the commands that build
the mod and check its C against the interpreter (tools/pc/model_oracle.py).
model_c/migrate.json holds the same for tools.

    python tools/pc/model_gate.py mod-migrate mods/my-mod --disc game/rpg-yfm.bin
    python tools/pc/model_gate.py mod-row mods/my-mod/models/pikachu.mrg

--records adds records of the disc's own MODEL.MRG to what the mod changes,
and --over-disc makes C for NATIVE modules too and flags every row
MEMORIES_MODEL_OVER_DISC: tools/pc/test_model_migrate_all.py runs every
shape's C so, against the interpreter.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import csv
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import model_gate  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
NATIVE_OVERLAYS = SRC / "native_overlays"
RETAIL = ROOT / "config/pc/model_retail.csv"
RENAMES = ROOT / "config/pc/host_symbol_renames.txt"
STALE_HEADER = SRC / "pc/overlays/model_stale_locals.h"
WRITTEN = "/* Written by tools/pc/model_gate.py mod-migrate"
COPIES = "model_c"

SECTOR = 2048
RECORD_SECTORS = 0x114
MODULES_SECTOR = 180          # a record's module stages (notes/modding.md, "The record")
MODULES_SECTORS = 44
STAGE0_SECTORS = 96           # a record's model data
TAIL_SECTORS = 180            # a bigger model's stages after its model data
PAGE_SECTORS = 16             # a texture page
MRG = "\\DATA\\MODEL.MRG"
RETURN_TWO = (0x03E00008, 0x24020002)
# The six module stages of a record's 44 sectors: (name, primary, slot, first sector, sectors).
STAGES = (("stance 0 slot A", False, "A", 0, 10), ("stance 0 slot B", False, "B", 10, 10),
          ("stance 1 slot A", False, "A", 20, 10), ("stance 1 slot B", False, "B", 30, 10),
          ("primary slot A", True, "A", 40, 2), ("primary slot B", True, "B", 42, 2))
# model_module_class.h's ModelModuleClass, in its order (the C test prints the number).
CLASSES = ("NATIVE", "RETAIL", "EMPTY", "TABLES", "CODE", "NEW", "MOD")
CLASS_TEXT = {"NATIVE": "the disc's code, native C",
              "RETAIL": "the disc's code, interpreted until the port has its C",
              "EMPTY": "no entry, the stand-in",
              "TABLES": "the disc's code with other tables, interpreted",
              "CODE": "changed code, interpreted",
              "NEW": "code no disc module has, interpreted"}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def kind_of(primary: bool) -> model_gate.Kind:
    return model_gate.PRIMARY if primary else model_gate.VARIANT


# --- the classifier (src/pc/overlays/model_module_class.c) ----------------------------

class Tables:
    """The disc's modules as the game knows them: the native rows' spans and
    hashes (config/pc/model_gate.csv, model_primary_gate.csv) and each
    shape's code (config/pc/model_retail.csv)."""

    def __init__(self):
        self.native = {False: model_gate.native_rows(), True: model_gate.native_primary_rows()}
        self.span_rows = [(primary, slot, row.identifier(slot), row.span, row.sha256(slot), row)
                          for primary in (False, True) for row in self.native[primary] for slot in "AB"]
        with RETAIL.open(newline="", encoding="utf-8") as handle:
            self.retail = [(entry["kind"] == "primary", entry["slot"], int(entry["identifier"]), int(entry["shape"]),
                            int(entry["size"], 0), entry["sha256"]) for entry in csv.DictReader(handle)]

    def shape_of(self, primary: bool, slot: str, identifier: int) -> int:
        for kind, retail_slot, retail_identifier, shape, _, _ in self.retail:
            if (kind, retail_slot, retail_identifier) == (primary, slot, identifier):
                return shape
        return 0

    def rows_for(self, primary: bool, slot: str, identifier: int) -> list[model_gate.Row]:
        """The native rows with this first word in this slot."""
        return [row for kind, row_slot, row_identifier, _, _, row in self.span_rows
                if (kind, row_slot, row_identifier) == (primary, slot, identifier)]


class Info(dict):
    """What classify() says of one module: kind (CLASSES), identifier, shape,
    code_size and span (TABLES)."""


def classify(image: bytes, slot: str, primary: bool, tables: Tables) -> Info:
    """ModelModule_Classify without mods' rows, in the same order."""
    words = struct.unpack_from("<3I", image, 0)
    info = Info(kind="NEW", identifier=words[0], shape=0, code_size=0, span=0)
    if words[1] == 0:
        info["kind"] = "EMPTY"
        return info
    info["shape"] = tables.shape_of(primary, slot, words[0])
    if primary and words[1:3] == RETURN_TWO:
        info.update(kind="NATIVE", shape=0)
        return info
    for row_primary, row_slot, identifier, span, digest, _ in tables.span_rows:
        if (row_primary, row_slot, identifier) == (primary, slot, words[0]) and sha(image[:span]) == digest:
            info["kind"] = "NATIVE"
            return info
    for kind, retail_slot, identifier, _, size, digest in tables.retail:
        if (kind, retail_slot, identifier) == (primary, slot, words[0]) and sha(image[:size]) == digest:
            info["kind"] = "RETAIL"
            for row_primary, row_slot, row_identifier, span, _, _ in tables.span_rows:
                if (row_primary, row_slot, row_identifier) == (primary, slot, words[0]):
                    info.update(kind="TABLES", code_size=size, span=span)
            return info
    info["kind"] = "CODE" if info["shape"] else "NEW"
    return info


# --- the disc and the mod --------------------------------------------------------------

class Disc:
    """MODEL.MRG of a raw 2352-byte-sector image, as the mod's "data" leaves it."""

    def __init__(self, path: Path):
        self.path = path
        self.file = path.open("rb")
        self.lba, self.size = self._find(["DATA", "MODEL.MRG;1"])
        self.records = self.size // (RECORD_SECTORS * SECTOR)
        self.replaced: bytes | None = None      # a "replace" of the whole file
        self.raw: dict[int, bytes] = {}         # sector of MODEL.MRG: a raw region's bytes
        self.patches: dict[int, int] = {}       # offset in MODEL.MRG: byte
        self.touched: set[int] = set()          # records whose module sectors the mod's data reaches
        self._first: dict[tuple[bool, str, int], tuple[int, int]] | None = None

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
                raise SystemExit(f"mod-migrate: {name} is not on {self.path}")
            lba, size = found
        return lba, size

    def original(self, sector: int, count: int) -> bytes:
        """The disc's own MODEL.MRG sectors (zeros past its end)."""
        return b"".join(self._sector(self.lba + sector + n) if sector + n < self.size // SECTOR else bytes(SECTOR)
                        for n in range(count))

    def read(self, sector: int, count: int) -> bytes:
        """MODEL.MRG's sectors as the mod's data leaves them."""
        if self.replaced is not None:
            data = bytearray(self.replaced[sector * SECTOR:(sector + count) * SECTOR].ljust(count * SECTOR, b"\0"))
        else:
            data = bytearray(self.original(sector, count))
        for n in range(count):
            if sector + n in self.raw:
                data[n * SECTOR:(n + 1) * SECTOR] = self.raw[sector + n]
        first = sector * SECTOR
        for at, byte in self.patches.items():
            if first <= at < first + len(data):
                data[at - first] = byte
        return bytes(data)

    def stages(self, record: int, original: bool = False) -> bytes:
        first = record * RECORD_SECTORS + MODULES_SECTOR
        return self.original(first, MODULES_SECTORS) if original else self.read(first, MODULES_SECTORS)

    def first(self, primary: bool, slot: str, identifier: int) -> tuple[int, int] | None:
        """(record, stage) of the disc's first module with this first word in this slot."""
        if self._first is None:
            self._first = {}
            for record in range(self.records):
                for stage, (_, is_primary, stage_slot, sector, _) in enumerate(STAGES):
                    word = struct.unpack_from("<I", self.original(record * RECORD_SECTORS + MODULES_SECTOR + sector,
                                                                  1), 0)[0]
                    self._first.setdefault((is_primary, stage_slot, word), (record, stage))
        return self._first.get((primary, slot, identifier))

    def module(self, record: int, stage: int) -> bytes:
        _, _, _, sector, count = STAGES[stage]
        return self.original(record * RECORD_SECTORS + MODULES_SECTOR + sector, count)

    def apply(self, manifest: dict, folder: Path) -> None:
        """The mod's "data" entries that reach MODEL.MRG, as the game applies
        them (replacements, then patches); the records whose module sectors
        they reach are `touched` (mods.c, check_disc_models)."""
        sectors = self.size // SECTOR
        reached: list[tuple[int, int]] = []     # (first, last) sector of MODEL.MRG
        for entry in manifest.get("data", []) or []:
            name = str(entry.get("file", "")).upper().replace("/", "\\")
            name = ("\\" + name.lstrip("\\")).split(";")[0] if name else ""
            raw = entry.get("lba")
            if name == MRG:
                base = 0
            elif raw is not None:
                base = int(str(raw), 0) - self.lba
                if not 0 <= base < sectors:
                    continue
            else:
                continue
            if "replace" in entry:
                data = (folder / entry["replace"]).read_bytes()
                if raw is None:
                    self.replaced = data
                    reached.append((0, (len(data) + SECTOR - 1) // SECTOR - 1))
                else:
                    count = int(entry.get("sectors", (len(data) + SECTOR - 1) // SECTOR))
                    data = data.ljust(count * SECTOR, b"\0")
                    for n in range(count):
                        self.raw[base + n] = data[n * SECTOR:(n + 1) * SECTOR]
                    reached.append((base, base + count - 1))
            for run in entry.get("patch", []) or []:
                at = run["at"] if isinstance(run["at"], int) else int(str(run["at"]), 0)
                data = bytes.fromhex(str(run["bytes"]).replace(" ", ""))
                at += base * SECTOR
                for n, byte in enumerate(data):
                    self.patches[at + n] = byte
                if data:
                    reached.append((at // SECTOR, (at + len(data) - 1) // SECTOR))
        records = (len(self.replaced) // (RECORD_SECTORS * SECTOR)) if self.replaced is not None else self.records
        for first, last in reached:
            for record in range(records):
                low = record * RECORD_SECTORS + MODULES_SECTOR
                if first <= low + MODULES_SECTORS - 1 and last >= low:
                    self.touched.add(record)


class Site:
    """Where a mod's modules are: a record of MODEL.MRG or a "models" file."""

    def __init__(self, label: str, stages: bytes, record: int | None = None, model: str | None = None):
        self.label, self.stages, self.record, self.model = label, stages, record, model

    def image(self, stage: int) -> bytes:
        _, _, _, sector, count = STAGES[stage]
        return self.stages[sector * SECTOR:(sector + count) * SECTOR]


def record_card(record: int) -> int:
    import model_oracle
    return model_oracle.record_card(record)


def model_site(folder: Path, entry: dict) -> Site | None:
    """A "models" entry's module stages, where mods.c's check_model_modules
    reads them: after its model data (96 sectors in a record; a bigger
    model's own count, past which come the record's other 180 sectors and
    its texture pages)."""
    path = folder / str(entry.get("file", ""))
    name = str(entry.get("name", path.stem))
    if not path.is_file():
        print(f"mod-migrate: model {name}: no file {path}", file=sys.stderr)
        return None
    size = path.stat().st_size
    geometry = 0
    if size > RECORD_SECTORS * SECTOR:
        geometry = size // SECTOR - TAIL_SECTORS - int(entry.get("texture_pages", 0) or 0) * PAGE_SECTORS
    at = ((geometry or STAGE0_SECTORS) + MODULES_SECTOR - STAGE0_SECTORS) * SECTOR
    with path.open("rb") as handle:
        handle.seek(at)
        stages = handle.read(MODULES_SECTORS * SECTOR).ljust(MODULES_SECTORS * SECTOR, b"\0")
    return Site(f"model {name}", stages, model=name)


def mod_sites(folder: Path, manifest: dict, disc: Disc, records: list[int]) -> list[Site]:
    sites = []
    for entry in manifest.get("models", []) or []:
        site = model_site(folder, entry)
        if site:
            sites.append(site)
    disc.apply(manifest, folder)
    for record in sorted(disc.touched | set(records)):
        sites.append(Site(f"MODEL.MRG record {record} (card {record_card(record)})", disc.stages(record),
                          record=record))
    return sites


# --- the shape's C, read once -------------------------------------------------------

class Symbol:
    def __init__(self, name: str, kind: str, bound: str, offset: int, size: int, data: bytes | None):
        self.name, self.kind, self.bound, self.offset, self.size, self.data = name, kind, bound, offset, size, data


def elf_symbols(path: Path) -> list[Symbol]:
    """The defined symbols of an ELF32 relocatable object, with each
    object's bytes (its section's, at its place: zeros where a relocation
    goes)."""
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1:
        raise SystemExit(f"mod-migrate: {path} is not an ELF32 object")
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    sections = [struct.unpack_from("<IIIIIIIIII", data, shoff + n * shentsize) for n in range(shnum)]
    found = []
    for section in sections:
        if section[1] != 2:   # SHT_SYMTAB
            continue
        strings = sections[section[6]]
        for at in range(section[4], section[4] + section[5], 16):
            name_at, value, size, info, _, index = struct.unpack_from("<IIIBBH", data, at)
            if index == 0 or index >= 0xFF00 or not name_at:
                continue
            end = data.index(b"\0", strings[4] + name_at)
            name = data[strings[4] + name_at:end].decode("latin-1")
            kind = {1: "object", 2: "function"}.get(info & 15)
            if kind is None:
                continue
            bound = {0: "local", 1: "global", 2: "weak"}.get(info >> 4, "other")
            target = sections[index]
            body = None
            if kind == "object" and target[1] != 8:   # not SHT_NOBITS
                body = data[target[4] + value:target[4] + value + size]
            found.append(Symbol(name, kind, bound, value, size, body))
    return found


ADDRESS_NAME = re.compile(r"^(func|D)_([0-9A-F]{8})$")
INCLUDE = re.compile(r'^(\s*#\s*include\s+)"([^"]+)"', re.M)


class Source:
    """One native shape's module in one slot, as the port has it: its C
    files (config/pc/model_gate_sources.txt), every file of src/native_overlays
    they include, and what an -O0 build of them defines."""

    def __init__(self, row: model_gate.Row, slot: str):
        self.row, self.slot = row, slot
        self.kind = row.KIND
        self.base = self.kind.base[slot]
        self.units = [ROOT / path for pattern in model_gate.source_patterns(row, slot)
                      for path in sorted(ROOT.glob(pattern))]
        if not self.units:
            raise SystemExit(f"mod-migrate: shape {row.shape} slot {slot} has no C in {model_gate.SOURCES.name}")
        closure: list[Path] = []
        for unit in self.units:
            for one in model_gate.port_closure(unit):
                one = Path(one).resolve()
                if one not in closure:
                    closure.append(one)
        self.closure = closure
        self.copied = [path for path in closure if NATIVE_OVERLAYS.resolve() in path.parents]
        self.text = "\n".join(model_gate.without_comments(path.read_text(encoding="utf-8", errors="replace"))
                              for path in closure if path.suffix == ".c")
        self.defined: set[str] = set()                 # global names the module's C defines
        self.functions: list[tuple[int, str]] = []     # (offset, func_ name)
        self.consts: dict[int, Symbol] = {}            # offset: a D_ const the C defines

    def analyse(self, folder: Path) -> None:
        import build_mod
        cc, cc_flags, _ = build_mod.compiler("i386")
        flags = build_mod.target_flags("i386", cc_flags)
        for n, unit in enumerate(self.units):
            obj = folder / f"{self.row.module(self.slot)}_{n}.o"
            command = cc + flags + ["-O0", "-g0", "-c", str(unit), "-o", str(obj)]
            result = subprocess.run(command, capture_output=True, text=True, errors="replace")
            if result.returncode:
                raise SystemExit(f"mod-migrate: {unit} does not compile as a mod's C:\n{result.stderr[-3000:]}")
            for symbol in elf_symbols(obj):
                if symbol.bound in ("global", "weak"):
                    self.defined.add(symbol.name)
                match = ADDRESS_NAME.match(symbol.name)
                if not match:
                    continue
                offset = int(match.group(2), 16) - self.base
                if not 0 <= offset < self.kind.image_size:
                    continue
                if match.group(1) == "func" and symbol.kind == "function":
                    self.functions.append((offset, symbol.name))
                elif match.group(1) == "D" and symbol.kind == "object" and symbol.data is not None and symbol.size:
                    self.consts[offset] = symbol
        self.functions.sort()

    def function_at(self, offset: int) -> str | None:
        found = None
        for start, name in self.functions:
            if start <= offset:
                found = name
        return found

    def const_at(self, offset: int) -> Symbol | None:
        for start, symbol in self.consts.items():
            if start <= offset < start + symbol.size:
                return symbol
        return None

    def label(self) -> str:
        return " ".join(path.relative_to(ROOT).as_posix() for path in self.units)


# --- what differs ----------------------------------------------------------------

class Module:
    """One module the mod has: where, which stage, what it is and what to do."""

    def __init__(self, site: Site, stage: int, info: Info, image: bytes):
        self.site, self.stage, self.info, self.image = site, stage, info, image
        name, self.primary, self.slot, _, _ = STAGES[stage]
        self.where = f"{site.label}, {name}"
        self.row: model_gate.Row | None = None
        self.source: Source | None = None
        self.code: list[int] = []                   # (a) offsets
        self.consts: dict[str, bytes] = {}          # (b) slot A name: the mod's bytes
        self.unmapped: list[str] = []               # (b) consts whose bytes are not the disc's (not a plain copy)
        self.data = 0                               # (c) bytes inside the span
        self.past = 0                               # bytes past the span
        self.action = ""                            # "generate", "hand" or "none"
        self.why = ""
        self.unit: str | None = None


def differing(mine: bytes, theirs: bytes) -> list[int]:
    if mine == theirs:
        return []
    found = []
    for at in range(0, len(mine), 64):
        if mine[at:at + 64] != theirs[at:at + 64]:
            found += [at + n for n in range(min(64, len(mine) - at)) if mine[at + n] != theirs[at + n]]
    return found


def nearest(module: Module, tables: Tables, disc: Disc) -> tuple[model_gate.Row, bytes] | None:
    """The disc's native row with the module's first word in its slot (of
    several, the one whose code differs least) and that shape's disc image."""
    best = None
    for row in tables.rows_for(module.primary, module.slot, module.info["identifier"]):
        place = disc.first(module.primary, module.slot, row.identifier(module.slot))
        if place is None:
            continue
        image = disc.module(*place)
        size = model_gate.code_size(row)
        count = len(differing(module.image[:size], image[:size]))
        if best is None or count < best[0]:
            best = (count, row, image)
    return (best[1], best[2]) if best else None


def map_bytes(module: Module, disc_image: bytes) -> None:
    row, source = module.row, module.source
    for at in differing(module.image, disc_image):
        if at >= row.span:
            module.past += 1
        elif row.data_start is not None and at >= row.data_start:
            module.data += 1
        else:
            symbol = source.const_at(at)
            if symbol is None:
                module.code.append(at)
                continue
            start = int(symbol.name[2:], 16) - source.base
            name = f"D_{source.kind.base['A'] + start:08X}"   # slot A's name, which the C's text has
            if symbol.data != disc_image[start:start + symbol.size]:
                # Not a plain copy of the disc's bytes (a relocated pointer, or
                # another layout): the C cannot be given the mod's bytes.
                if name not in module.unmapped:
                    module.unmapped.append(name)
                module.code.append(at)
                continue
            module.consts[name] = module.image[start:start + symbol.size]


def describe_code(module: Module) -> str:
    functions: dict[str, list[int]] = {}
    for at in module.code:
        functions.setdefault(module.source.function_at(at) or "before the first function", []).append(at)
    parts = [f"{name} ({len(offsets)} byte{'s' if len(offsets) != 1 else ''} at 0x{offsets[0]:X}"
             f"{f'-0x{offsets[-1]:X}' if len(offsets) > 1 else ''})" for name, offsets in functions.items()]
    if module.unmapped:
        parts.append(f"the consts {', '.join(module.unmapped)}, which are not plain copies of the disc's bytes")
    return "; ".join(parts)


def plan(modules: list[Module], tables: Tables, disc: Disc, over_disc: bool, work: Path, jobs: int) -> None:
    """Each module's source shape, byte map and action."""
    sources: dict[tuple[bool, int, str], Source] = {}
    for module in modules:
        kind = module.info["kind"]
        if kind == "NATIVE" and not (over_disc and module.info["shape"]):
            module.action, module.why = "none", ("the port answers it (`return 2`)" if not module.info["shape"]
                                                 else "the disc's C runs it already")
            continue
        if kind in ("EMPTY", "RETAIL"):
            module.action, module.why = "none", ("the stand-in runs it" if kind == "EMPTY" else
                                                 f"the disc's code of shape {module.info['shape']}, which the port "
                                                 "interprets until it has its C: the port's to finish")
            continue
        if kind == "NEW":
            module.action, module.why = "hand", (f"no disc module has the first word {module.info['identifier']} in "
                                                 f"slot {module.slot}: port it by hand")
            continue
        found = nearest(module, tables, disc)
        if found is None:
            module.action, module.why = "hand", (f"shape {module.info['shape']} is not native C in the port yet: "
                                                 "port it by hand")
            continue
        module.row = found[0]
        key = (module.primary, module.row.shape, module.slot)
        if key not in sources:
            sources[key] = Source(module.row, module.slot)
        module.source = sources[key]
        module.disc_image = found[1]
    work.mkdir(parents=True, exist_ok=True)
    pending = list(sources.values())
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for future in [pool.submit(source.analyse, work) for source in pending]:
            future.result()
    for module in modules:
        if module.source is None:
            continue
        map_bytes(module, module.disc_image)
        if module.code:
            module.action, module.why = "hand", "the code differs: " + describe_code(module)
        else:
            module.action = "generate"


# --- the C it writes -------------------------------------------------------------------

def written(text: str) -> str:
    return f"{WRITTEN}: do not edit; it is written again. */\n" + text


def rewrite_includes(path: Path, text: str, copy_of: dict[Path, Path], copy: Path) -> str:
    """A copied file's includes: of another copied file, relative to the copy;
    of any other header, from src/ (the game's headers, which build_mod.py
    passes as an include directory)."""
    def one(match: re.Match) -> str:
        name = match.group(2)
        beside = (path.parent / name).resolve()
        target = beside if beside.is_file() else (SRC / name).resolve()
        if target in copy_of:
            relative = os.path.relpath(copy_of[target], copy.parent).replace(os.sep, "/")
        elif target.is_file() and SRC.resolve() in target.parents:
            relative = target.relative_to(SRC.resolve()).as_posix()
        else:
            return match.group(0)
        return f'{match.group(1)}"{relative}"'
    return INCLUDE.sub(one, text)


ALIAS_LINE = re.compile(r"^(\s*#\s*define\s+(?:func|D)_[0-9A-F]{8}\s+)((?:func|D)_[0-9A-F]{8})(\s*)$", re.M)


def rewrite_aliases(text: str, own: set[str], prefix: str) -> str:
    """A wrapper's `#define func_X func_Y` (C written at one address put at
    another) of a name the module defines: to that name's prefixed one, as
    the prefix header names it, which the wrapper would otherwise redefine
    (`#define func_8013B004 func_8013B004` among them)."""
    return ALIAS_LINE.sub(lambda match: match.group(1) + (f"{prefix}__{match.group(2)}" if match.group(2) in own
                                                          else match.group(2)) + match.group(3), text)


DEFINITION = r"^(?P<head>[ \t]*(?:static[ \t]+)?(?P<type>(?:const[ \t]+)?[\w \t\*]*?))[ \t]*\b{name}\b(?P<dims>(?:[ \t]*\[[^\]]*\])*)[ \t]*="


def initializer_end(text: str, at: int) -> int:
    """Where the definition whose "=" is at `at` ends: after its ";" outside braces."""
    depth = 0
    n = at
    while n < len(text):
        if text.startswith("/*", n):
            n = text.index("*/", n) + 2
            continue
        if text.startswith("//", n):
            n = text.index("\n", n)
            continue
        char = text[n]
        if char in "\"'":
            n += 1
            while text[n] != char:
                n += 2 if text[n] == "\\" else 1
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
        elif char == ";" and depth == 0:
            return n + 1
        n += 1
    raise ValueError("no end")


def declare_const(text: str, name: str, size: int) -> str | None:
    """`text` with the definition of `name` made an extern declaration of it
    (its bytes are the mod's, in <prefix>_data.c); None when it has none."""
    match = re.search(DEFINITION.format(name=name), text, re.M)
    if not match:
        return None
    end = initializer_end(text, match.end() - 1)
    kind = re.sub(r"\bstatic\b\s*", "", match.group("type")).strip()
    dims = match.group("dims").strip()
    if dims.startswith("[]") or re.match(r"\[\s*\]", dims):
        rest = re.sub(r"^\[\s*\]", "", dims)
        dims = f"[{size} / sizeof({kind}{rest})]{rest}"
    return (text[:match.start()] + f"extern {kind} {name}{dims}; /* the mod's bytes: {COPIES} data unit */"
            + text[end:])


def stale_storage() -> dict[str, str]:
    """{name: its definition's declarator and type} from model_stale_locals.h."""
    found = {}
    text = model_gate.without_comments(STALE_HEADER.read_text(encoding="utf-8"))
    for match in re.finditer(r"^\s*extern\s+([\w \t\*]+?)\s+(\w+)\s*((?:\[[^\]]*\])+)\s*;", text, re.M):
        found[match.group(2)] = f"{match.group(1)} {match.group(2)}{match.group(3)}"
    return found


def host_renames() -> dict[str, str]:
    found = {}
    for line in RENAMES.read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if len(parts) == 2 and not parts[0].startswith("#"):
            found[parts[0]] = parts[1]
    return found


# Functions a header of the game declares without a prototype that the
# module calls with none: the translated build reads such a declaration as
# variadic, which build_mod.py refuses against the game's definition, so
# the prefix header declares them first.
UNPROTOTYPED = re.compile(r"^(?:extern[ \t]+)?([A-Za-z_][\w \t\*]*?)\b([A-Za-z_]\w*)[ \t]*\([ \t]*\)[ \t]*;", re.M)
NOT_A_TYPE = {"return", "else", "case", "goto", "do", "sizeof", "typedef"}


def prototypes(source: Source) -> list[str]:
    declared: dict[str, str] = {}
    for path in source.closure:
        if path.suffix == ".h":
            for match in UNPROTOTYPED.finditer(model_gate.without_comments(path.read_text(errors="replace"))):
                if match.group(1).split()[0] not in NOT_A_TYPE:
                    declared.setdefault(match.group(2), match.group(1).strip())
    lines = []
    for name, result in sorted(declared.items()):
        calls = re.findall(rf"\b{re.escape(name)}\s*\(([^()]*)\)", source.text)
        if calls and all(not call.strip() for call in calls):
            lines.append(f"{result} {name}(void);")
    return lines


def renamed_declaration(name: str, new: str, source: Source) -> str | None:
    """`name`'s prototype in the module's headers (or the PSY-Q ones), under `new`."""
    paths = [path for path in source.closure if path.suffix == ".h"] + sorted((SRC / "psyq").glob("*.h"))
    for path in paths:
        text = model_gate.without_comments(path.read_text(encoding="utf-8", errors="replace"))
        match = re.search(rf"^[ \t]*(?:extern[ \t]+)?([A-Za-z_][\w \t\*]*?)\b{re.escape(name)}[ \t]*\(([^()]+)\)[ \t]*;",
                          text, re.M)
        if match and match.group(1).split()[0] not in NOT_A_TYPE:
            return f"{match.group(1).strip()} {new}({match.group(2).strip()});"
    return None


class Unit:
    """The C made for the modules of one shape in one slot with one set of const bytes."""

    def __init__(self, prefix: str, source: Source, consts: dict[str, bytes]):
        self.prefix, self.source, self.consts = prefix, source, consts
        self.modules: list[Module] = []
        self.files: list[str] = []


def entry_name(source: Source) -> str:
    return f"func_{source.base + 4:08X}"


def write_unit(unit: Unit, out: Path, renames: dict[str, str], stale: dict[str, str]) -> None:
    source, prefix = unit.source, unit.prefix
    copy = out / COPIES / prefix
    copy_of = {path: copy / path.relative_to(SRC.resolve()) for path in source.copied}
    slot_names = {}
    for name in unit.consts:   # slot A's name; the slot B wrapper #defines it to its own
        slot_names[name] = f"D_{int(name[2:], 16) - source.kind.base['A'] + source.base:08X}"
    used_stale = sorted(name for name in stale if re.search(rf"\b{name}\b", source.text))
    used_renames = sorted(name for name in renames if re.search(rf"\b{re.escape(name)}\s*\(", source.text))
    own = sorted(source.defined | set(used_stale) | set(slot_names.values()))
    remaining = dict(unit.consts)
    for path, target in copy_of.items():
        text = path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")
        for name in list(remaining):
            if path.suffix in (".c", ".h"):
                changed = declare_const(text, name, len(unit.consts[name]))
                if changed is not None:
                    text = changed
                    del remaining[name]
        text = rewrite_aliases(rewrite_includes(path, text, copy_of, target), set(own), prefix)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8", newline="\n")
    if remaining:
        raise SystemExit(f"mod-migrate: shape {source.row.shape}: no definition of {', '.join(remaining)} in its C")
    guard = prefix.upper() + "_H"
    header = [f"/* The names shape {source.row.shape}'s C (slot {source.slot}: {source.label()}) defines, as this\n"
              f" * mod's own: a mod is one object, so each module copy's names are prefixed. */\n",
              f"#ifndef {guard}\n#define {guard}\n"]
    if used_renames:
        header.append("/* The game's resident functions, by the names a mod is lent them under\n"
                      " * (config/pc/host_symbol_renames.txt): a mod's own rand is the mods'. Declared\n"
                      " * here too: the 64-bit builds read the game's headers before this one. */\n")
        for name in used_renames:
            header.append(f"#define {name} {renames[name]}\n")
            declaration = renamed_declaration(name, renames[name], source)
            if declaration:
                header.append(declaration + "\n")
    lines = prototypes(source)
    if lines:
        header.append("/* Declared by the game's headers without a prototype; declared first here,\n"
                      " * the translated build reads them as the game defines them. */\n")
        header += [line + "\n" for line in lines]
    header += [f"#define {name} {prefix}__{name}\n" for name in own]
    # The port's C calls the host C library's printf and bzero, which the
    # game does not lend a mod: a printf that prints nothing (the
    # interpreter only logs a module's format string) and a bzero of the
    # mod's own, declared as psyq/memory.h declares it when that is in.
    if re.search(r"\bprintf\s*\(", source.text):
        header.append(f"static int {prefix}_printf(const char *format, ...) {{ (void)format; return 0; }}\n"
                      f"#define printf {prefix}_printf\n")
    if re.search(r"\bbzero\s*\(", source.text):
        pointer = "unsigned char *" if any(path.name == "memory.h" for path in source.closure) else "void *"
        header.append(f"static void *{prefix}_bzero({pointer}at, int size)\n"
                      f"{{\n    unsigned char *byte = (unsigned char *)at;\n"
                      f"    while (size-- > 0) *byte++ = 0;\n    return at;\n}}\n"
                      f"#define bzero {prefix}_bzero\n")
    header.append("#endif\n")
    (out / f"{prefix}.h").write_text(written("".join(header)), encoding="utf-8", newline="\n")
    for n, path in enumerate(source.units):
        name = f"{prefix}_{n}.c"
        include = (copy_of[path.resolve()]).relative_to(out).as_posix()
        (out / name).write_text(written(f'#include "{prefix}.h"\n#include "{include}"\n'), encoding="utf-8",
                                newline="\n")
        unit.files.append(name)
    if used_stale or unit.consts:
        body = [f'#include "{prefix}.h"\n']
        if used_stale:
            body.append("/* Rule 6's storage (pc/overlays/model_stale_locals.h), this mod's own. */\n"
                        '#include "pc/overlays/model_stale_locals.h"\n')
            body += [f"{stale[name]};\n" for name in used_stale]
        for name, data in sorted(unit.consts.items()):
            body.append(f"/* {slot_names[name]}: the mod's bytes, which the port's C has its own copy of. */\n"
                        f"__attribute__((aligned(8))) const unsigned char {slot_names[name]}[{len(data)}] = {{"
                        + ", ".join(f"0x{byte:02X}" for byte in data) + "};\n")
        (out / f"{prefix}_data.c").write_text(written("".join(body)), encoding="utf-8", newline="\n")
        unit.files.append(f"{prefix}_data.c")


def row_fields(module: Module, over_disc: bool) -> dict:
    row = module.row
    span = row.span
    table = row.command_table
    flags = (["MEMORIES_MODEL_BOUND"] if table else []) + (["MEMORIES_MODEL_OVER_DISC"] if over_disc else [])
    return {"kind": "MEMORIES_MODEL_PRIMARY" if module.primary else "MEMORIES_MODEL_VARIANT",
            "slot": f"MEMORIES_MODEL_SLOT_{module.slot}", "flags": " | ".join(flags) or "0",
            "identifier": module.info["identifier"], "span": span, "sha256": sha(module.image[:span]),
            "bound": (table[2], table[0], table[1], table[3], table[4], row.command_modulus) if table else
            (0, 0, 0, 0, 0, 0)}


def write_rows(out: Path, units: list[Unit], over_disc: bool, init: bool, mod_id: str) -> int:
    rows: dict[tuple, tuple[dict, str, list[str]]] = {}
    for unit in units:
        for module in unit.modules:
            fields = row_fields(module, over_disc)
            key = (fields["kind"], fields["slot"], fields["identifier"], fields["sha256"])
            rows.setdefault(key, (fields, unit.prefix + "__" + entry_name(unit.source), []))[2].append(module.where)
    lines = [f"/* The rows of {len(rows)} module{'s' if len(rows) != 1 else ''} of mod {mod_id}: the C of the shape\n"
             " * each was made from (src/native_overlays) runs it where its bytes hash to the row. */\n",
             "#include <string.h>\n\n#include \"pc/mods/modapi.h\"\n\n"]
    entries = sorted({entry for _, entry, _ in rows.values()})
    lines += [f"int32_t {entry}(uint8_t *context, int32_t command);\n" for entry in entries]
    lines.append("""
static int add(const MemoriesModHost *host, uint32_t kind, uint32_t slot, uint32_t flags, uint32_t identifier,
               uint32_t span, const char *sha256, MemoriesModelEntry entry, uint32_t entries, uint32_t table,
               uint32_t stride, uint32_t field, uint32_t index_width, uint32_t modulus)
{
    MemoriesModelModule row;
    memset(&row, 0, sizeof(row));
    row.size = sizeof(row);
    row.kind = kind;
    row.slot = slot;
    row.flags = flags;
    row.identifier = identifier;
    row.span = span;
    memcpy(row.sha256, sha256, 65);
    row.entry = entry;
    row.entries = entries;
    row.table = table;
    row.stride = stride;
    row.field = field;
    row.index_width = index_width;
    row.modulus = modulus;
    return host->model_module(host, &row) != 0;
}

/* Every row; 0 if the game refused any (the mod's status says why). */
int MemoriesModelRows_Register(const MemoriesModHost *host)
{
    int ok = 1;
""")
    for fields, entry, wheres in rows.values():
        bound = ", ".join(f"0x{value:X}u" for value in fields["bound"])
        lines.append(f"    /* {wheres[0]}{f' and {len(wheres) - 1} more' if len(wheres) > 1 else ''} */\n"
                     f"    ok &= add(host, {fields['kind']}, {fields['slot']}, {fields['flags']}, "
                     f"{fields['identifier']}u, 0x{fields['span']:X}u,\n"
                     f"              \"{fields['sha256']}\", {entry}, {bound});\n")
    lines.append("    return ok;\n}\n")
    if init:
        lines.append("""
int MemoriesModInit(const MemoriesModHost *host, MemoriesMod *mod)
{
    if (host->api < 29) return 0;   /* model_module is mod API 29 */
    mod->api = 29;
    return MemoriesModelRows_Register(host);
}
""")
    (out / "model_rows.c").write_text(written("".join(lines)), encoding="utf-8", newline="\n")
    return len(rows)


def clean(out: Path) -> None:
    """What an earlier run wrote: the files that start with WRITTEN, and model_c."""
    shutil.rmtree(out / COPIES, ignore_errors=True)
    for path in list(out.glob("*.c")) + list(out.glob("*.h")):
        try:
            with path.open(encoding="utf-8", errors="replace") as handle:
                first = handle.read(len(WRITTEN))
        except OSError:
            continue
        if first == WRITTEN:
            path.unlink()


def own_init(out: Path) -> bool:
    """Whether the mod's own C defines MemoriesModInit."""
    for path in out.glob("*.c"):
        text = model_gate.without_comments(path.read_text(encoding="utf-8", errors="replace"))
        if re.search(r"\bMemoriesModInit\s*\([^;]*\)\s*\{", text):
            return True
    return False


# --- the report --------------------------------------------------------------------

def module_report(module: Module) -> dict:
    found = {"where": module.where, "site": module.site.label, "record": module.site.record,
             "model": module.site.model, "stage": STAGES[module.stage][0], "primary": module.primary,
             "slot": module.slot, "class": module.info["kind"], "identifier": module.info["identifier"],
             "shape": module.row.shape if module.row else module.info["shape"], "action": module.action,
             "why": module.why, "unit": module.unit}
    if module.row:
        found.update(code_bytes=len(module.code), consts=sorted(module.consts), data_bytes=module.data,
                     past_span=module.past, span=module.row.span, data_start=module.row.data_start,
                     sha256=sha(module.image[:module.row.span]))
    return found


def summary_line(module: Module) -> str:
    info = module.info
    shape = f"shape {module.row.shape}" if module.row else (f"shape {info['shape']}" if info["shape"] else
                                                             f"first word {info['identifier']}")
    text = f"{module.where}: {shape}, {CLASS_TEXT[info['kind']]}"
    if module.row:
        parts = []
        if module.data:
            parts.append(f"{module.data} bytes of data the C reads from the record "
                         f"(0x{module.row.data_start:X}-0x{module.row.span:X})")
        if module.consts:
            parts.append(f"the const{'s' if len(module.consts) > 1 else ''} {', '.join(sorted(module.consts))}, "
                         "which the C defines: it gets the mod's bytes")
        if module.code:
            parts.append(f"{len(module.code)} bytes of code")
        if module.past:
            parts.append(f"{module.past} bytes past the span, which the C reads from the record")
        text += "; differs from the disc's: " + ("; ".join(parts) if parts else "nothing")
    if module.action == "generate":
        text += f"\n    -> C: {module.unit} (from {module.source.label()})"
    elif module.action == "hand":
        text += f"\n    -> port by hand: {module.why}"
    else:
        text += f"\n    -> nothing to do: {module.why}"
    return text


def migrate(folder: Path, disc_path: Path, records: list[int], over_disc: bool, into: Path | None,
            jobs: int, quiet: bool = False) -> dict:
    manifest_path = folder / "mod.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    mod_id = str(manifest.get("id") or folder.name)
    out = (into or folder).resolve()
    out.mkdir(parents=True, exist_ok=True)
    disc = Disc(disc_path)
    tables = Tables()
    sites = mod_sites(folder, manifest, disc, records)
    # One module per distinct image of a slot; the others are the same bytes.
    modules: list[Module] = []
    seen: dict[tuple, Module] = {}
    duplicates: dict[int, list[tuple]] = {}
    for site in sites:
        for stage, (_, primary, slot, _, _) in enumerate(STAGES):
            image = site.image(stage)
            key = (primary, slot, sha(image))
            if key in seen:
                duplicates.setdefault(id(seen[key]), []).append((f"{site.label}, {STAGES[stage][0]}", site.record, site.model))
                continue
            module = Module(site, stage, classify(image, slot, primary, tables), image)
            seen[key] = module
            modules.append(module)
    with tempfile.TemporaryDirectory(prefix="mod-migrate-") as work:
        plan(modules, tables, disc, over_disc, Path(work), jobs)
    clean(out)
    units: dict[tuple, Unit] = {}
    counts: dict[tuple, int] = {}
    for module in modules:
        if module.action != "generate":
            continue
        consts = tuple(sorted(module.consts.items()))
        key = (module.primary, module.row.shape, module.slot, consts)
        if key not in units:
            base = f"mm_{'p' if module.primary else 'v'}{module.row.shape}{module.slot.lower()}"
            number = counts[key[:3]] = counts.get(key[:3], 0) + 1
            units[key] = Unit(base if number == 1 else f"{base}_{number}", module.source, dict(consts))
        units[key].modules.append(module)
        module.unit = units[key].prefix
    renames, stale = host_renames(), stale_storage()
    for unit in units.values():
        write_unit(unit, out, renames, stale)
    init = not own_init(out)
    rows = write_rows(out, list(units.values()), over_disc, init, mod_id) if units else 0
    changed = []
    if units:
        if not manifest.get("library") and not manifest.get("libraries"):
            manifest["library"] = mod_id
            changed.append(f'"library": "{mod_id}"')
        if int(manifest.get("min_api", 0) or 0) < 29:
            manifest["min_api"] = 29
            changed.append('"min_api": 29')
        if changed:
            manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    generated = [m for m in modules if m.action == "generate"]
    places = [(m.site.record, m.site.model) for m in generated] + \
             [place[1:] for m in generated for place in duplicates.get(id(m), [])]
    generated_records = sorted({record for record, _ in places if record is not None})
    generated_models = sorted({model for _, model in places if model})
    verify = verify_commands(folder, disc_path, generated_records, generated_models)
    # Each const the port's C defines, against the disc's bytes where its
    # name puts it: a plain copy (the same bytes) is one the C can be given
    # the mod's bytes for.
    consts = {"total": 0, "plain": 0, "other": []}
    for module in {id(m.source): m for m in modules if m.source}.values():
        for offset, symbol in sorted(module.source.consts.items()):
            consts["total"] += 1
            if symbol.data == module.disc_image[offset:offset + symbol.size]:
                consts["plain"] += 1
            else:
                consts["other"].append(f"shape {module.row.shape} slot {module.slot} {symbol.name}")
    report = {"mod": mod_id, "folder": str(folder), "out": str(out), "modules": [], "units": len(units), "rows": rows,
              "init": "MemoriesModInit" if init else "MemoriesModelRows_Register", "manifest": changed,
              "verify": verify, "records": generated_records, "models": generated_models, "consts": consts}
    for module in modules:
        entry = module_report(module)
        entry["same_bytes"] = [where for where, _, _ in duplicates.get(id(module), [])]
        report["modules"].append(entry)
    (out / COPIES).mkdir(exist_ok=True)
    (out / COPIES / "migrate.json").write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    if not quiet:
        print_report(report, modules, duplicates)
    return report



def tool_path(name: str) -> str:
    return os.path.relpath(Path(__file__).resolve().parent / name).replace(os.sep, "/")


def ranges(numbers: list[int]) -> str:
    """[1, 2, 3, 7] -> "1-3,7" (model_oracle.py's --records)."""
    parts: list[list[int]] = []
    for number in sorted(numbers):
        if parts and number == parts[-1][1] + 1:
            parts[-1][1] = number
        else:
            parts.append([number, number])
    return ",".join(f"{first}-{last}" if last > first else str(first) for first, last in parts)


def verify_commands(folder: Path, disc: Path, records: list[int], models: list[str]) -> list[str]:
    if not records and not models:
        return []
    mod, disc_name = (str(path).replace(os.sep, "/") for path in (folder, disc))
    commands = [f"python {tool_path('build_mod.py')} {mod} --target i386"]
    oracle = f"python {tool_path('model_oracle.py')} --disc {disc_name} --mod {mod}"
    if records:
        oracle += f" --records {ranges(records)}"
    if models:
        oracle += f" --models {','.join(models)}"
    return commands + [oracle]


def print_report(report: dict, modules: list[Module], duplicates: dict[int, list[tuple]]) -> None:
    shown = [m for m in modules if m.action != "none" or m.info["kind"] != "NATIVE" or m.unit]
    quiet = len(modules) - len(shown)
    print(f"mod-migrate: {report['mod']}: {len(modules)} distinct control modules in what it changes")
    for module in shown:
        print(summary_line(module))
        same = [where for where, _, _ in duplicates.get(id(module), [])]
        if same:
            print(f"    (the same bytes: {'; '.join(same[:3])}{f' and {len(same) - 3} more' if len(same) > 3 else ''})")
    if quiet:
        print(f"{quiet} more are the disc's, which its C runs already")
    hand = [m for m in modules if m.action == "hand"]
    print(f"generated: {report['units']} module copies, {report['rows']} rows in model_rows.c "
          f"({report['init']}); by hand: {len(hand)}")
    if report["init"] != "MemoriesModInit":
        print("    the mod has its own MemoriesModInit: call MemoriesModelRows_Register(host) from it "
              "(host->api >= 29)")
    if report["manifest"]:
        print(f"mod.json: added {', '.join(report['manifest'])}")
    if report["verify"]:
        print("check it, the mod's C against the interpreter:")
        for command in report["verify"]:
            print("    " + command)


# --- mod-row ---------------------------------------------------------------------------

def mod_row(path: Path, over_disc: bool) -> int:
    """The rows for a model file's (or a record's) modules whose first word
    is a native shape's: what model_rows.c registers for them."""
    tables = Tables()
    size = path.stat().st_size
    entry = {"file": path.name, "name": path.stem}
    if size == MODULES_SECTORS * SECTOR:   # the 44 sectors alone
        site = Site(path.name, path.read_bytes())
    else:
        site = model_site(path.parent, entry)
    for stage, (name, primary, slot, _, _) in enumerate(STAGES):
        image = site.image(stage)
        info = classify(image, slot, primary, tables)
        rows = tables.rows_for(primary, slot, info["identifier"])
        if not rows or info["kind"] == "EMPTY":
            print(f"/* {name}: {CLASS_TEXT[info['kind']]}, first word {info['identifier']}: no native shape */")
            continue
        module = Module(site, stage, info, image)
        module.row = rows[0]
        fields = row_fields(module, over_disc)
        bound = ", ".join(f"0x{value:X}u" for value in fields["bound"])
        print(f"/* {name}: shape {module.row.shape}, {CLASS_TEXT[info['kind']]} */\n"
              f"{{sizeof(MemoriesModelModule), {fields['kind']}, {fields['slot']}, {fields['flags']}, "
              f"{fields['identifier']}u, 0x{fields['span']:X}u, \"{fields['sha256']}\", entry, {bound}}}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="model_gate.py mod-migrate", description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("mod-migrate", "mod-row"))
    parser.add_argument("target", type=Path, help="mod-migrate: the mod's folder; mod-row: a model file")
    parser.add_argument("--disc", type=Path, default=os.environ.get("MEMORIES_DISC"),
                        help="the disc image (default: MEMORIES_DISC)")
    parser.add_argument("--into", type=Path, help="where the C goes (default: the mod's folder)")
    parser.add_argument("--records", help="records of the disc's MODEL.MRG to take as the mod's too "
                                          "(\"34,100-119\" or \"all\")")
    parser.add_argument("--over-disc", action="store_true",
                        help="C for the disc's own modules too, and every row MEMORIES_MODEL_OVER_DISC")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    arguments = parser.parse_args(argv)
    if arguments.command == "mod-row":
        return mod_row(arguments.target, arguments.over_disc)
    if not arguments.disc or not Path(arguments.disc).is_file():
        parser.error("mod-migrate needs the disc: --disc or MEMORIES_DISC")
    if not (arguments.target / "mod.json").is_file():
        parser.error(f"{arguments.target}: no mod.json")
    records: list[int] = []
    if arguments.records:
        import model_oracle
        records = model_oracle.parse_records(arguments.records)
    report = migrate(arguments.target, Path(arguments.disc), records, arguments.over_disc, arguments.into,
                     arguments.jobs)
    return 1 if any(module["action"] == "hand" for module in report["modules"]) else 0


if __name__ == "__main__":
    raise SystemExit(main())
