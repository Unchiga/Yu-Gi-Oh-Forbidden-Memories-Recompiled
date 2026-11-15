#!/usr/bin/env python3
"""How the native C of each native control-module row uses the words its
hash covers past the code and outside the command table
(config/pc/model_span_words.csv; notes/modding.md, "Moving a mod's attacks
from MIPS to C").

A native row's hashed span (config/pc/model_gate.csv,
config/pc/model_primary_gate.csv) is the shape's code and constants, widened
over the command table and over the words its C divides by or indexes with
(model_gate_scan.py). A span is one run of bytes, so it also takes in the
words between: a texture rectangle's GsIMAGE header, colours and timings the
C only computes with, padding. mod-migrate (model_migrate.py) makes C for a
mod's module only where the C cannot behave otherwise than the interpreter
whatever the mod put there, and this table says which words those are.

Each word of [data_start, span) outside the command table's entries gets one
class, from the slot A unit(s) of the row (slot B's C is the same C renamed),
by following every read of the module's data through clang's AST: the
module's functions, their locals, parameters and return values, the work
area's members (by byte offset, from clang's record layouts, as
model_gate_scan.py does; a store and a read of one member meet whatever
their offsets in it), and conservatively through everything else (a store
to the game's memory is read back by every read of a place of that type; a
call to the game's code gives back everything its arguments carry, and puts
it in the memory its pointer arguments reach; a function of the module
whose address it takes may be called by the game, or through a pointer,
with anything the module handed the game or left in its memory):

  texture       its only use is its address handed to the texture packing
                (ModelTexture_PackPageClut, func_80059A50): a GsIMAGE header,
                which the game's own native code reads in either backend
  value         read and computed with, and nothing below
  divisor       a divisor or a remainder's, or the dividend of a division by a
                non-constant (x86 faults on a zero and on INT_MIN / -1; MIPS
                does not)
  index         an array subscript or a pointer step
  bound         a branch on it (a loop's condition, an if, a switch, a ?:)
  pointer       used as an address (dereferenced, cast to a pointer, a pointer
                argument)
  shift         a shift count
  address       its address goes where the analysis cannot follow (a game
                function other than the texture packing, untracked memory,
                arithmetic on it)
  copy          the C defines its own copy of it (the mod's bytes are ignored)
  unknown       the analysis did not settle (a row whose values did not
                converge, a read whose width is unknown, a construct it does
                not model)
  unread        not read by this module's own C (padding, a symbol it does not
                read; another module's C may: a variant's work area runs into
                the primary's image)

No class lets mod-migrate make C when a mod changes the word: each is a
label for the report, describing what the module's own C does with it, from
a walk that is not sound in general (notes/pc-build.md lists its known gaps).
A label is not a proof; a changed word needs a port by hand or --accept-data.
A word whose bytes have several classes gets the first of: divisor, index,
bound, pointer, shift, copy, address, unknown, value, texture, unread.

    python tools/pc/model_span_words.py            # write the table
    python tools/pc/model_span_words.py --check    # the table is what the C says, and covers every row
    python tools/pc/model_span_words.py --shape 54 --explain   # one row's words, each with every use

Needs the port's check compiler (model_gate_port.CHECK_CC). Exit status 0
when everything agrees."""
from __future__ import annotations

import argparse
import collections
import concurrent.futures
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import posixpath
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import model_layout as layout  # noqa: E402  (the repository's, or an SDK's: model_layout.py)
import model_sdk  # noqa: E402
import model_gate  # noqa: E402
import model_gate_port  # noqa: E402
import model_gate_scan as scan  # noqa: E402
from model_gate_scan import tag, CONST, COMMAND, WORK, OTHER  # noqa: E402

GAME = tag("game", "memory")   # a value read from the game's memory; tag("game", f) one game function f returned
# The game's functions that cannot give back what the module handed them:
# pure arithmetic, and queries of the engine's own state (the slot, the frame
# step, the ordering table, a texture page or CLUT the game owns). A value of
# any other game function, or a global of the game, at a divisor, index,
# shift or address in a module that hands the game its data locks the row.
PURE = frozenset({"rand", "ccos", "csin", "rcos", "rsin", "ratan2", "SquareRoot0", "SquareRoot12",
                  "Model_GetFrameStep", "Model_GetActiveSlotIndex", "GetClut", "GetTPage", "func_80058F10",
                  # the texture packing: its result is its arguments' (carried), it keeps nothing
                  "ModelTexture_PackPageClut", "func_80059A50", "ModelTexture_PackPageClutCopy"})
COPIES = ("memcpy", "memmove", "bcopy", "__builtin_memcpy", "__builtin_memmove", "__builtin___memcpy_chk",
          "__builtin___memmove_chk")

ROOT = layout.ROOT
TABLE = layout.path("config/pc/model_span_words.csv")
COLUMNS = ("kind", "shape", "offset", "size", "symbol", "class", "evidence", "digest", "tool", "compiler")
SAFE: tuple[str, ...] = ()   # no class makes C by itself: every label is a description
ORDER = ("divisor", "index", "bound", "pointer", "shift", "copy", "address", "unknown", "value", "texture",
         "unread")
CLASSES = frozenset(ORDER)
# The game's functions that pack a texture page and CLUT from a GsIMAGE
# header (src/game/model_texture_upload.c, src/game/func_80059A50.c;
# ModelTexture_PackPageClutCopy is func_80059A50): the argument that is the
# header. Both backends hand them the same header, and they are the game's
# native code either way.
TEXTURE = {"ModelTexture_PackPageClut": 2, "func_80059A50": 2, "ModelTexture_PackPageClutCopy": 2}
GSIMAGE = 0x1C
DATA_VALUE = ("slotword", "slotdynword", "tablefield")
DATA_POINTER = ("slot", "slotdyn", "tableptr")
ROUNDS = 60        # a row whose values still change after this many rounds is all `unknown`
WIDEN = 16         # distinct offsets of one pointer in one place before it is taken as dynamic
NEAR = 16          # a work-area read with no record reaches the stores this many bytes around it


# --- clang ------------------------------------------------------------------------------

def unit_ast(unit: str) -> list[dict]:
    """The unit's top-level declarations, all of them: the module's
    functions, the helpers its headers define and its data declarations."""
    result = subprocess.run([model_gate_port.CHECK_CC, *scan.FLAGS, "-fsyntax-only", "-Xclang", "-ast-dump=json",
                             unit], cwd=ROOT, capture_output=True)
    if result.returncode:
        raise RuntimeError(f"{unit} does not compile: {result.stderr.decode(errors='replace')[-800:]}")
    tree = json.loads(result.stdout.decode("utf-8", errors="replace"))
    return tree.get("inner") or []


def relative(path: str | None) -> str:
    if not path:
        return "?"
    path = posixpath.normpath(path.replace("\\", "/"))
    root = str(ROOT).replace("\\", "/")
    return path[len(root) + 1:] if path.startswith(root) else path


# --- the C a row's words come from -----------------------------------------------------

# What a full regeneration needs: the repository and the check compiler (clang:
# model_gate_port.CHECK_CC, llvm-mingw's i686-w64-mingw32-clang on Windows,
# which tools/pc/fetch_tools.py fetches); no disc, overlay build or game.
REGENERATE = ("python tools/pc/model_span_words.py   (needs only the repository and clang as "
              "model_gate_port.CHECK_CC on PATH: llvm-mingw's i686-w64-mingw32-clang, tools/pc/fetch_tools.py; "
              "no disc; about 4 minutes)")


def remedy() -> str:
    """What to do about labels that are not the tree's: in a checkout,
    regenerate them; in an SDK they cannot be (they were made from the tree it
    was packed from), so say which of the two it is: stale when packed (the
    stamp says so), or a file changed since (mod-check names it). Never the
    repository's command."""
    if not layout.SHIPPED:
        return f"regenerate: {REGENERATE}"
    stamp = model_sdk.read_stamp()
    if stamp and stamp.get("labels", {}).get("problems"):
        return "the labels were already stale when this SDK was made; nothing here can regenerate them"
    return "a file of this SDK is not the one it was made with (model_gate.py mod-check says which)"
# The tool's own files: the analysis, the scan it subclasses, the flags and
# compiler it runs (model_gate_port.py), and the rows, sources and names it
# reads (model_gate.py). Whole files: a superset of what decides a class.
TOOL_FILES = ("tools/pc/model_span_words.py", "tools/pc/model_gate_scan.py", "tools/pc/model_gate_port.py",
              "tools/pc/model_gate.py",
              # the texture packing: the gaps between and after GsIMAGE headers
              # are labelled unread because of the fields it reads
              "src/game/model_texture_upload.c", "src/game/func_80059A50.c")


def compiler_version() -> str:
    """The check compiler's first --version line ("" without one): recorded,
    and reported when it differs, but no reason for staleness."""
    try:
        result = subprocess.run([model_gate_port.CHECK_CC, "--version"], capture_output=True, text=True,
                                timeout=60)
    except (OSError, subprocess.SubprocessError):
        return ""
    return (result.stdout.splitlines() or [""])[0].strip()


def normalised(path: Path) -> str:
    """A file's text with its line ends made \\n (a Windows checkout's CRLF
    and a Linux one's LF digest alike)."""
    return path.read_bytes().decode("utf-8", errors="replace").replace("\r\n", "\n")


_IS_FILE: dict[str, bool] = {}
_INCLUDES: dict[str, list[str]] = {}


def closure(path) -> list[str]:
    """model_gate.port_closure's files (the file and everything of the
    repository it includes, beside it first, then from src/), with each
    file's includes and each existence test made once a process: the same
    files, without its cost over every row."""
    def is_file(name):
        if name not in _IS_FILE:
            _IS_FILE[name] = os.path.isfile(name)
        return _IS_FILE[name]
    seen, todo = set(), [os.path.normpath(str(path))]
    while todo:
        current = todo.pop()
        if current in seen or not is_file(current):
            continue
        seen.add(current)
        if current not in _INCLUDES:
            names = model_gate.INCLUDE.findall(model_gate._text(Path(current)))
            beside = [os.path.normpath(os.path.join(os.path.dirname(current), name)) for name in names]
            _INCLUDES[current] = [one if is_file(one) else os.path.normpath(layout.path("src") / name)
                                  for one, name in zip(beside, names)]
        todo += _INCLUDES[current]
    return sorted(seen)


def _files(paths) -> list[Path]:
    """Paths as repository files, once each, in the order of their names."""
    root = os.path.normcase(os.path.abspath(ROOT))
    out = {}
    for path in paths:
        full = os.path.abspath(path)
        if os.path.isfile(full) and os.path.normcase(full).startswith(root + os.sep):
            out[layout.logical(full)] = Path(full)
    return [out[name] for name in sorted(out)]


_SHARED: list[Path] | None = None


def shared_files() -> list[Path]:
    """The files the rows' C includes from outside src/native_overlays (the
    game's prototypes and types: src/game, src/psyq, src/types.h), and the
    header every unit is compiled with (-include src/pc/compat/pgxp_game.h):
    what the analysis reads besides each row's own C. Once a process."""
    global _SHARED
    if _SHARED is None:
        native = os.path.normcase(os.path.abspath(layout.path(model_gate_port.NATIVE))) + os.sep
        found = []
        for flag in scan.FLAGS:
            if flag.endswith(".h"):
                found += closure(layout.path(flag))
        for row in all_rows():
            for slot in "AB":
                for pattern in model_gate.source_patterns(row, slot):
                    for unit in sorted(layout.glob(pattern)):
                        found += [one for one in closure(unit)
                                  if not os.path.normcase(os.path.abspath(one)).startswith(native)]
        _SHARED = _files(found)
    return _SHARED


def tool_digest(read=None) -> str:
    """SHA-256 (16 hex digits) of the tool's files (TOOL_FILES) and the shared
    headers (shared_files), each by its path and its text, line ends made \\n:
    a change to either changes every row's classes."""
    read = read or normalised
    digest = hashlib.sha256()
    for path in [layout.path(name) for name in TOOL_FILES] + shared_files():
        text = read(path).replace("\r\n", "\n")
        digest.update(layout.logical(path).encode() + b"\0" + text.encode("utf-8") + b"\0")
    return digest.hexdigest()[:16]

def row_files(row: model_gate.Row) -> list[Path]:
    """The row's C units in both slots and the files of src/native_overlays
    they include (the module's own headers, which declare its work-area
    records, and the C they share): what its words were classed from."""
    native = os.path.normcase(os.path.abspath(layout.path(model_gate_port.NATIVE))) + os.sep
    found = []
    for slot in "AB":
        for pattern in model_gate.source_patterns(row, slot):
            for unit in sorted(layout.glob(pattern)):
                found += [one for one in closure(unit)
                          if os.path.normcase(os.path.abspath(one)).startswith(native)]
    return _files(found)


def row_digest(row: model_gate.Row, read=None) -> str:
    """SHA-256 (16 hex digits) of the row's region (span, data start, command
    table) and its files (row_files), each by its path and its text with the
    line ends made \\n, so a checkout on Windows and one on Linux agree."""
    read = read or (lambda path: path.read_bytes().decode("utf-8", errors="replace"))
    digest = hashlib.sha256(f"{row['span']}|{row['data_start']}|{row['command_table']}\n".encode())
    for path in row_files(row):
        text = read(path).replace("\r\n", "\n")
        digest.update(layout.logical(path).encode() + b"\0" + text.encode("utf-8") + b"\0")
    return digest.hexdigest()[:16]


# --- the analysis ----------------------------------------------------------------------

class Analysis(scan.Module):
    """model_gate_scan.Module's values, followed forward from every read of
    the module's data to every use, and conservative where the scan only
    needed to be complete for divisors and indexes."""

    def __init__(self, row: model_gate.Row, names: dict[str, int] | None = None):
        super().__init__(row)
        self.base = row.KIND.base["A"]
        self.size = row.KIND.image_size
        names = model_gate.module_addresses(row, "A") if names is None else names
        self.functions = {name for name in names if name.startswith("func_")}
        self.data = {name: address - self.base for name, address in names.items() if name.startswith("D_")}
        self.starts = sorted(set(self.data.values()))
        self.sizes: dict[int, int] = {}   # a symbol's start: its declared size, where the C gives one
        self.returns: dict[str, set] = collections.defaultdict(set)
        # What went into the game's memory, by the type of the place it went
        # (strict aliasing): ("field", record, member), ("type", type),
        # ("var", name); ("any",) for a place of no known type.
        self.pool: dict[tuple, set] = collections.defaultdict(set)
        self.slot_stores: set = set()     # what the C wrote into the module's own data
        self.events: list[tuple[str, frozenset, str]] = []
        self.where = "?"
        self.loops = 0
        self.unhandled: collections.Counter = collections.Counter()
        self.work_records: set[str] = set()
        self.external: set = set()   # everything the module handed the game's functions
        # Constructs the analysis does not model: any one locks every word of
        # the row (`unknown`, the reason as the evidence). {reason: where}
        self.locks: dict[str, str] = {}
        self.game_sinks: dict[str, str] = {}   # a sink fed by a value of the game's: {kind: where}
        self.setters: dict[str, str] = {}      # a game call handed the module's data values: {callee: where}
        self.statements: set[int] = set()      # the calls made as statements (their result unused)
        self.ranges: dict[tuple[int, int], set] = collections.defaultdict(set)   # work-area member: values

    def lock(self, reason: str) -> None:
        self.locks.setdefault(reason, self.where)

    # where ----------------------------------------------------------------------------
    def note(self, node) -> None:
        where = node.get("_where") if isinstance(node, dict) else None
        if where and where[0]:
            self.where = f"{relative(where[0])}:{where[1]}"

    def event(self, kind: str, tags, where: str | None = None, game: bool = True) -> None:
        if game and kind in ("divisor", "index", "shift", "pointer"):
            # A divisor, index, shift count or address the game's code gave
            # (not a branch on one, nor a dividend: those every module has).
            for t in tags:
                if t[0] == "game" and t[1] != "memory" and t[1] not in PURE:
                    self.game_sinks.setdefault((kind, t[1]), where or self.where)
        tags = frozenset(t for t in tags if t[0] in DATA_VALUE + DATA_POINTER)
        if tags:
            self.events.append((kind, tags, where or self.where))

    # memory ---------------------------------------------------------------------------
    @staticmethod
    def kept(values: set) -> set:
        return {v for v in values if v != CONST}

    def grow(self, target: set, values: set) -> bool:
        values = self.widen(target | self.kept(values))
        if values != target:
            target.clear()
            target |= values
            return True
        return False

    def widen(self, values: set) -> set:
        """Many offsets of one pointer in one place (a pointer stepped in a
        loop): any offset, of the work area or of the data symbol. A dynamic
        pointer takes in the fixed ones it covers, so a place only grows."""
        out = set(values)
        if sum(1 for v in out if v[0] == "workptr") > WIDEN:
            for v in [v for v in out if v[0] == "workptr"]:
                field = self.work_field(v[1])
                out.discard(v)
                out.add(tag("workrangeptr", *field) if field else tag("workdyn"))
        ranges = [v for v in out if v[0] == "workrangeptr"]
        if ranges:
            out = {v for v in out if not (v[0] == "workptr" and any(r[1] <= v[1] < r[2] for r in ranges))}
        if tag("workdyn") in out:
            out = {v for v in out if v[0] != "workptr"}
        tables = collections.Counter(v[1] for v in out if v[0] == "tableptr")
        for start, count in tables.items():
            if count > WIDEN:
                out = {v for v in out if not (v[0] == "tableptr" and v[1] == start)} | {tag("slotdyn", start)}
        symbols = collections.defaultdict(list)
        for v in out:
            if v[0] == "slot":
                symbols[self.extent(v[1]).start].append(v)
        for start, found in symbols.items():
            if len(found) > WIDEN:
                out.add(tag("slotdyn", start))
        dynamic = [self.extent(v[1]) for v in out if v[0] == "slotdyn"]
        if dynamic:
            out = {v for v in out if not (v[0] in ("slot", "tableptr") and any(v[1] in one for one in dynamic))}
        return out

    def store(self, locations: set, values: set) -> None:
        for location in locations:
            kind = location[0]
            if kind == "local":
                changed = self.grow(self.locals[location[1]], values)
            elif kind == "param":
                changed = self.grow(self.params[(location[1], location[2])], values)
            elif kind == "work":
                # The whole member it lies in (a store to work->v and a read of
                # work->v.vy are one place), or, where the work area has no
                # record, the word itself, read back from 16 bytes around.
                field = self.work_field(location[1])
                changed = self.grow(self.ranges[field] if field else self.fields[location[1]], values)
            elif kind == "workdyn":
                changed = self.grow(self.fields[-1], values)
            elif kind == "workrange":
                changed = self.grow(self.ranges[(location[1], location[2])], values)
            elif kind in ("slotloc", "slotdynloc", "tableloc"):
                changed = self.grow(self.slot_stores, values)
            else:   # the game's memory
                self.event("address", {v for v in values if v[0] in DATA_POINTER})
                changed = self.grow(self.pool[self.pool_key(location)], values)
            self.changed |= changed

    def read(self, locations: set, width: int | None = None) -> set:
        out = set()
        for location in locations:
            kind = location[0]
            if kind == "local":
                out |= self.locals.get(location[1]) or {OTHER}
            elif kind == "param":
                out |= self.params.get((location[1], location[2])) or {OTHER}
            elif kind == "slotloc":
                value = tag("slotword", location[1], width)
                self.event("value", {value})
                out |= {value} | self.slot_stores
            elif kind == "slotdynloc":
                value = tag("slotdynword", location[1])
                self.event("value", {value})
                out |= {value} | self.slot_stores
            elif kind == "tableloc":
                value = tag("tablefield", location[1], location[2], location[3], width)
                self.event("value", {value})
                out |= {value} | self.slot_stores
            elif kind == "work":
                at = location[1]
                field = self.work_field(at) or (at - NEAR, at + NEAR)
                if width:
                    # A read wider than its member (a cast) reaches the next ones.
                    field = (min(field[0], at), max(field[1], at + width))
                out |= (self.fields.get(-1) or set()) | {OTHER}
                for key, values in self.fields.items():
                    if field[0] <= key < field[1]:
                        out |= values
                for (start, end), values in self.ranges.items():
                    if start < field[1] and field[0] < end:
                        out |= values
            elif kind == "workrange":
                start, end = location[1], location[2]
                for at, values in self.fields.items():
                    if at == -1 or start <= at < end:
                        out |= values
                for (first, last), values in self.ranges.items():
                    if first < end and start < last:
                        out |= values
                out.add(OTHER)
            elif kind == "workdyn":
                for values in self.fields.values():
                    out |= values
                out.add(OTHER)
            else:
                # The game's memory: a store through a pointer or a cast may
                # reach any place (a global by name, another type), so every
                # read gets every store, but that of another member of a record.
                key = self.pool_key(location)
                for stored, values in self.pool.items():
                    if key[0] == "field" and stored[0] == "field" and stored != key:
                        continue
                    out |= values
                out.add(tag("game", f"global {key[1]}") if key[0] == "var" else GAME)
        return out

    @staticmethod
    def pool_key(location) -> tuple:
        if location[0] in ("globalfield", "globaltype", "globalvar"):
            return {"globalfield": "field", "globaltype": "type", "globalvar": "var"}[location[0]], *location[1:]
        return ("any",)

    def assign(self, target, value) -> set:
        values = self.rv(value)
        locations = self.lv(target)
        self.check_width(locations, self.width(target))
        self.store(locations, values)
        return values

    def check_width(self, locations: set, width: int | None) -> None:
        """A store to the work area wider than the member it starts in (a
        cast), or, with no record, wider than NEAR: the walk files it under
        one member, so it locks the row."""
        if not width:
            return
        for location in locations:
            if location[0] != "work":
                continue
            field = self.work_field(location[1])
            if field is None and width > NEAR:
                self.lock(f"a {width}-byte store to a work area with no record (wider than {NEAR})")
            elif field is not None and location[1] + width > field[1]:
                self.lock(f"a {width}-byte store at work +0x{location[1]:X}, wider than its member")

    def address(self, locations: set) -> set:
        out = set()
        for location in locations:
            kind = location[0]
            if kind == "local":
                out.add(tag("localptr", location[1]))
            elif kind == "param":
                out.add(tag("paramptr", location[1], location[2]))
            elif kind == "workrange":
                out.add(tag("workrangeptr", location[1], location[2]))
            else:
                out |= super().address({location})
        return out

    def deref(self, pointers: set, node) -> set:
        self.event("pointer", {p for p in pointers if p[0] in DATA_VALUE})
        out = set()
        known = {p for p in pointers if p[0] in ("slot", "slotdyn", "tableptr", "work", "workptr", "workdyn",
                                                 "workrangeptr", "localptr", "paramptr")}
        if known:
            # A pointer the walk knows the target of, besides the "not known
            # yet" every read carries (`other`): not the game's memory too.
            pointers = known | {p for p in pointers if p[0] in DATA_VALUE + ("unknownptr",)}
        for p in pointers:
            if p[0] == "localptr":
                out.add(tag("local", p[1]))
            elif p[0] == "paramptr":
                out.add(tag("param", p[1], p[2]))
            elif p[0] == "workrangeptr":
                out.add(tag("workrange", p[1], p[2]))
            elif p[0] in DATA_VALUE:
                out.add(tag("unknownloc"))
            else:
                out |= super().deref({p}, node)
        return out

    def arith(self, values: set) -> set:  # noqa: C901
        """A value computed from others: an address of the module's data
        stops being one (the analysis loses which bytes it reaches)."""
        self.event("address", {v for v in values if v[0] in DATA_POINTER})
        out = set()
        for value in values:
            if value[0] in ("slot", "slotdyn", "tableptr", "workptr", "work", "workdyn", "unknownptr", "localptr",
                            "paramptr", "workrangeptr"):
                out.add(OTHER)
            else:
                out.add(value)
        return out or {OTHER}

    def step(self, pointers: set, delta: int | None, size: int, index: set) -> set:
        out = set()
        for p in pointers:
            if p[0] in ("slotdyn", "localptr", "paramptr", "workdyn", "workrangeptr"):
                out.add(p)
            elif p[0] == "tableptr" and (delta is None or p[3] == -1):
                # Anywhere in the entry the command picked (config->parts[i]):
                # a field of -1.
                out.add(tag("tableptr", p[1], p[2], -1))
            elif p[0] == "workptr" and delta is None:
                # A pointer stepped through a field of the work area (p++ over
                # work->ring): anywhere in that field.
                field = self.work_field(p[1])
                out.add(tag("workrangeptr", *field) if field else tag("workdyn"))
            elif p[0] == "workptr" and (self.work_field(p[1]) or (None,))[0] is not None and \
                    self.work_field(p[1]) != self.work_field(p[1] + delta):
                # A step out of the member it points into (corner += 4 in a
                # loop): both members, anywhere, so a loop's steps stop.
                one, other = self.work_field(p[1]), self.work_field(p[1] + delta)
                if p[1] + delta == one[1]:
                    other = one   # one past the member's end, as a loop leaves it
                if other is None:
                    out.add(tag("workdyn"))
                else:
                    out.add(tag("workrangeptr", min(one[0], other[0]), max(one[1], other[1])))
            elif p[0] == "slot" and delta is not None and p[1] + delta not in self.extent(p[1]):
                out.add(tag("slotdyn", p[1]))
                if p[1] + delta != self.extent(p[1]).stop:   # not just one past the symbol's end
                    out.add(tag("slotdyn", p[1] + delta))
            else:
                out |= super().step({p}, delta, size, index)
        return out

    def work_field(self, offset: int) -> tuple[int, int] | None:
        """The extent of the work area's member that holds offset, from the
        largest record the module reaches its work area as: a smaller one
        reached at the work area's start is an element of its first member
        (variant 460's Panel477 at work->panels), and a union of views of the
        whole area (variant 98's Model98Workspace: the state, or its bytes)
        counts only where no other record has a member there."""
        records, views = [], []
        for record in self.work_records:
            layout = self.layouts.get(record)
            if not layout:
                return None
            starts = sorted({start for start, _ in layout[1].values()}) + [layout[0]]
            (views if len(starts) == 2 else records).append((layout[0], starts))
        found = None
        for chosen in ([one for one in records if one[0] == max(size for size, _ in records)] if records else [],
                       views):
            for _, starts in chosen:
                # A member reaches the next one's offset (its padding too: a
                # superset), the last the record's size.
                for start, end in zip(starts, starts[1:]):
                    if start <= offset < end:
                        found = (start, end) if found is None else (min(found[0], start), max(found[1], end))
            if found:
                return found
        return None

    def lv(self, node) -> set:
        if node.get("kind") == "ArraySubscriptExpr":
            # An array member indexed with a value (work->texture[i]): that
            # member, which its type sizes.
            inner = node.get("inner") or []
            base = inner[0] if inner else None
            if base is not None and base.get("kind") == "ImplicitCastExpr" and                     base.get("castKind") == "ArrayToPointerDecay" and self.const(inner[1]) is None:
                array = base["inner"][0]
                t = array.get("type", {})
                size = self.size_of(t.get("qualType", "")) or self.size_of(t.get("desugaredQualType", ""))
                pointers = self.rv(base)
                if size and pointers and all(p[0] == "workptr" for p in pointers):
                    self.site(node, "[]", inner[1], pointers)
                    self.rv(inner[1])
                    return {tag("workrange", p[1], p[1] + size) for p in pointers}
        return self.lv_member(node)

    def member(self, places: set, offset: int | None) -> set:
        out = set()
        for p in places:
            if p[0] == "tableloc" and p[3] == -1:
                out.add(p)
            else:
                out |= super().member({p}, offset)
        return out

    def lv_member(self, node) -> set:
        kind = node.get("kind")
        if kind == "MemberExpr" and node.get("isArrow"):
            base = (node.get("inner") or [None])[0]
            if base is not None and ({WORK, tag("workptr", 0)} & self.rv(base)):
                record = self.record_of(base, True)
                if record not in self.work_records:
                    self.work_records.add(record)
                    self.changed = True
        found = super().lv(node)
        if not any(p[0] in ("global", "unknownloc") for p in found):
            return found
        # The game's memory, named by the type of the place.
        if kind == "MemberExpr":
            base = (node.get("inner") or [None])[0]
            name = (tag("globalfield", self.record_of(base, node.get("isArrow", False)), node.get("name", "")))
        elif kind == "DeclRefExpr":
            name = tag("globalvar", node.get("referencedDecl", {}).get("name", ""))
        else:
            t = node.get("type", {})
            name = tag("globaltype", re.sub(r"\b(const|volatile|struct|union)\b", "",
                                            t.get("desugaredQualType") or t.get("qualType", "")).strip())
        return {name if p[0] in ("global", "unknownloc") else p for p in found}

    # sinks ----------------------------------------------------------------------------
    def site(self, node, op, operand, base=None) -> None:
        if self.const(operand) is not None:
            return
        self.note(node)
        values = self.rv(operand)
        if op in ("/", "%"):
            self.event("divisor", values)
        elif op in ("[]", "+"):
            self.event("index", values)
        elif op == "<<":
            self.event("shift", values)
        if base:
            self.event("pointer", {v for v in base if v[0] in DATA_VALUE})

    def condition(self, node, kind: str) -> None:
        if node:
            self.note(node)
            self.event(kind, {v for v in self.operands(node) if v[0] in DATA_VALUE}, game=False)

    def operands(self, node) -> set:
        """What a condition's comparisons compare (rv gives a comparison
        no source: its result is 0 or 1)."""
        node = self.strip(node)
        while node and node.get("kind") in ("ImplicitCastExpr", "CStyleCastExpr", "ParenExpr"):
            node = (node.get("inner") or [None])[0]
        if not node:
            return set()
        if node.get("kind") == "BinaryOperator" and node.get("opcode") in ("<", ">", "<=", ">=", "==", "!=", "&&",
                                                                          "||"):
            return set().union(*(self.operands(child) for child in node.get("inner") or []))
        if node.get("kind") == "UnaryOperator" and node.get("opcode") == "!":
            return self.operands(node["inner"][0])
        return self.rv(node)

    # values ---------------------------------------------------------------------------
    def rv(self, node) -> set:
        if node is None or not isinstance(node, dict) or not node:
            return {OTHER}
        self.note(node)
        number = self.const(node)
        if number is not None:
            if self.base <= (number & 0xFFFFFFFF) < self.base + self.size:
                # An address of the module written as a number.
                return {tag("slot", (number & 0xFFFFFFFF) - self.base)}
            return {CONST}
        kind = node.get("kind")
        inner = node.get("inner") or []
        if kind == "CallExpr":
            return self.call(node)
        if kind == "DeclRefExpr" and node.get("referencedDecl", {}).get("kind") in ("EnumConstantDecl",
                                                                                    "FunctionDecl"):
            return {CONST}
        if kind == "ConditionalOperator":
            self.condition(inner[0], "bound")
            return self.rv(inner[1]) | self.rv(inner[2])
        if kind == "StmtExpr":
            last = {OTHER}
            for statement in inner:
                for child in statement.get("inner") or []:
                    last = self.statement_value(child)
            return last
        if kind in ("ImplicitCastExpr", "CStyleCastExpr") and node.get("castKind") == "IntegralToPointer":
            values = self.rv(inner[0])
            self.event("pointer", {v for v in values if v[0] in DATA_VALUE})
            return values
        if kind == "UnaryOperator" and node.get("opcode") in ("++", "--") and self.is_pointer(node):
            location = self.lv(inner[0])
            values = self.read(location, self.width(inner[0]))
            self.store(location, self.step(values, None, 1, {OTHER}))
            return values
        if kind == "BinaryOperator" and node.get("opcode") in ("+", "-") and len(inner) == 2:
            for one, other in ((inner[0], inner[1]), (inner[1], inner[0])):
                amount = self.const(other)
                if amount and (node.get("opcode") == "+" or one is inner[0]) and COMMAND in self.rv(one):
                    self.lock("the command plus or minus a constant (another entry than the command's)")
        if kind == "BinaryOperator" and node.get("opcode") in ("<<", ">>"):
            self.site(node, "<<", inner[1])
        if kind == "BinaryOperator" and node.get("opcode") in ("/", "%") and self.const(inner[1]) is None:
            self.note(node)
            self.event("divisor", self.rv(inner[0]), game=False)   # the dividend: INT_MIN / -1
        if kind == "CompoundAssignOperator":
            op = node.get("opcode")
            if op in ("<<=", ">>="):
                self.site(node, "<<", inner[1])
            if op in ("/=", "%=") and self.const(inner[1]) is None:
                self.event("divisor", self.read(self.lv(inner[0]), self.width(inner[0])))
            if op in ("+=", "-=") and self.is_pointer(inner[0]):
                location = self.lv(inner[0])
                values = self.read(location, self.width(inner[0]))
                amount = self.const(inner[1])
                size = self.size_of(self.pointee(inner[0])) or 1
                if amount is None:
                    self.site(node, "+", inner[1])
                    self.rv(inner[1])
                stepped = self.step(values, None if amount is None else (amount if op == "+=" else -amount) * size,
                                    size, self.rv(inner[1]))
                self.store(location, stepped)
                return stepped
        if kind in ("ImplicitCastExpr", "ParenExpr", "CStyleCastExpr", "DeclRefExpr", "UnaryOperator",
                    "MemberExpr", "ArraySubscriptExpr", "BinaryOperator", "CompoundAssignOperator",
                    "ConditionalOperator"):
            return super().rv(node)
        if kind in ("StringLiteral", "UnaryExprOrTypeTraitExpr", "PredefinedExpr", "FloatingLiteral",
                    "ImplicitValueInitExpr", "OffsetOfExpr", "GNUNullExpr"):
            return {OTHER}
        # Anything else (an initializer list, a compound literal, a GNU ?:):
        # whatever its parts carry.
        if kind not in ("ConstantExpr", "InitListExpr", "CompoundLiteralExpr"):
            self.unhandled[kind] += 1
        out = set()
        for child in inner:
            if isinstance(child, dict) and child.get("kind", "").endswith(("Expr", "Operator", "Literal")):
                out |= self.rv(child)
            elif isinstance(child, dict) and child:
                self.walk(child)
        return out or {OTHER}

    def is_pointer(self, node) -> bool:
        t = node.get("type", {})
        text = t.get("desugaredQualType") or t.get("qualType", "")
        return text.rstrip().endswith("*")

    def statement_value(self, node) -> set:
        if node.get("kind", "").endswith(("Expr", "Operator", "Literal")):
            return self.rv(node)
        self.walk(node)
        return {OTHER}

    def call(self, node) -> set:
        inner = node.get("inner") or []
        self.note(node)
        callee = inner[0] if inner else None
        while callee is not None and callee.get("kind") in ("ImplicitCastExpr", "ParenExpr"):
            callee = (callee.get("inner") or [None])[0]
        name = None
        if callee is not None and callee.get("kind") == "DeclRefExpr" and \
                callee.get("referencedDecl", {}).get("kind") == "FunctionDecl":
            name = callee.get("referencedDecl", {}).get("name")
        else:
            self.event("pointer", {v for v in self.rv(inner[0]) if v[0] in DATA_VALUE} if inner else set())
            self.lock("a call through a function pointer")
        if name in COPIES:
            self.lock(f"{name}, whose copy the walk does not follow")
        arguments = [self.rv(argument) for argument in inner[1:]]
        if name in self.function_params:
            params = self.function_params[name]
            for index, values in enumerate(arguments):
                if index < len(params):
                    self.changed |= self.grow(self.params[(name, params[index])], values)
            return set(self.returns.get(name) or ()) | {OTHER}
        if name is None:
            # Through a pointer: it may be any of the module's functions.
            out = {OTHER}
            for function, params in self.function_params.items():
                for index, values in enumerate(arguments):
                    if index < len(params):
                        self.changed |= self.grow(self.params[(function, params[index])], values)
                out |= self.returns.get(function) or set()
            for values in arguments:
                out |= {v for v in values if v[0] in DATA_VALUE}
                self.event("address", {v for v in values if v[0] in DATA_POINTER})
            return out
        # The game's own code: it computes from everything it is given and
        # may write what it computed through every pointer it is given.
        carried = set()
        for index, (argument, values) in enumerate(zip(inner[1:], arguments)):
            pointers = {v for v in values if v[0] in DATA_POINTER}
            if pointers:
                if TEXTURE.get(name) == index and all(p[0] in ("slot", "slotdyn") for p in pointers):
                    # A header at a known offset, or one of an array of them
                    # (&D_8013C648[i]): the symbol's every header.
                    for p in pointers:
                        self.event("texture", {p})
                        carried.add(tag("slotword", p[1], GSIMAGE) if p[0] == "slot" else tag("slotdynword", p[1]))
                else:
                    self.event("address", pointers, f"{self.where} ({name})")
                    carried |= {tag("slotdynword", p[1]) for p in pointers}
            if self.is_pointer(argument):
                self.event("pointer", {v for v in values if v[0] in DATA_VALUE})
            carried |= {v for v in values if v[0] in DATA_VALUE}
        # What its pointer arguments point at goes in too (ScaleMatrix(&m,
        # &scale) makes m from the scale's words), and comes out of every one.
        if name not in TEXTURE:
            for argument, values in zip(inner[1:], arguments):
                pointers = {v for v in values if v[0] in ("localptr", "paramptr", "workrangeptr", "workptr", "work",
                                                          "workdyn")}
                if pointers and self.is_pointer(argument):
                    carried |= {v for v in self.read(self.deref(pointers, argument)) if v[0] in DATA_VALUE}
        if any(v[0] in DATA_VALUE for v in carried) and id(node) in self.statements:
            # A call made for what it does, handed the module's data: the game
            # may keep it (a setter, GsSetLsMatrix) and give it back later.
            self.setters.setdefault(name, self.where)
        if carried:
            for argument, values in zip(inner[1:], arguments):
                targets = {v for v in values if v[0] in ("localptr", "paramptr", "workptr", "work", "workdyn",
                                                         "workrangeptr", "unknownptr", "other")}
                if not self.is_pointer(argument):
                    continue
                for target in targets:
                    if target[0] == "workptr":
                        # The game writes a whole pointee there: wider than the member, a lock.
                        self.check_width({tag("work", target[1])}, self.size_of(self.pointee(argument)))
                    if target[0] == "localptr":
                        self.store({tag("local", target[1])}, carried)
                    elif target[0] == "paramptr":
                        self.store({tag("param", target[1], target[2])}, carried)
                    elif target[0] == "workrangeptr":
                        self.store({tag("workrange", target[1], target[2])}, carried)
                    elif target[0] == "workptr" and self.work_field(target[1]):
                        self.store({tag("workrange", *self.work_field(target[1]))}, carried)
                    elif target[0] in ("workptr", "work", "workdyn"):
                        self.store({tag("workdyn")}, carried)
                    else:
                        pointee = re.sub(r"\b(const|volatile|struct|union)\b", "", self.pointee(argument)).strip()
                        self.store({tag("globaltype", pointee) if pointee and pointee != "void" else tag("global")},
                                   carried)
        # Whatever the game is handed it may hand back to a callback of the
        # module whose address it was given (run(): `escaped`).
        given = carried | {v for values in arguments for v in values if v[0] in DATA_VALUE + DATA_POINTER}
        self.changed |= self.grow(self.external, given)
        return carried | {OTHER, tag("game", name)}

    # statements -----------------------------------------------------------------------
    def walk(self, node) -> None:
        if not isinstance(node, dict) or not node:
            return
        self.note(node)
        kind = node.get("kind")
        inner = node.get("inner") or []
        if kind == "ReturnStmt":
            for child in inner:
                self.changed |= self.grow(self.returns[self.function], self.rv(child))
            return
        if kind in ("ForStmt", "WhileStmt", "DoStmt"):
            if kind == "ForStmt":
                cond = inner[2] if len(inner) > 2 else None
            elif kind == "WhileStmt":
                cond = inner[-2] if len(inner) >= 2 else None
            else:
                cond = inner[-1] if inner else None
            self.condition(cond, "bound")
            for child in inner:
                self.bound(child)
            self.loops += 1
            for child in inner:
                self.walk(child)
            self.loops -= 1
            return
        if kind in ("IfStmt", "SwitchStmt") and inner:
            # Any branch on the data: what it guards may index, divide or end a
            # loop (a counter the data resets, a local set on some paths only).
            self.condition(inner[0], "bound")
        if kind == "StmtExpr":
            self.rv(node)
            return
        if kind == "VarDecl" and inner and node.get("storageClass") not in ("extern", "static"):
            self.store({tag("local", (self.function, node.get("name")))}, self.rv(inner[-1]))
            return
        if kind.endswith(("Expr", "Operator", "Literal")):
            bare = node
            while bare.get("kind") in ("ImplicitCastExpr", "CStyleCastExpr", "ParenExpr") and bare.get("inner"):
                bare = bare["inner"][0]
            if bare.get("kind") == "CallExpr":
                self.statements.add(id(bare))
            self.rv(node)
            return
        for child in inner:
            self.walk(child)

    @staticmethod
    def leaves(node, depth=0) -> bool:
        """A break, goto or return in a branch (a break only outside a loop
        or switch of its own)."""
        if not isinstance(node, dict):
            return False
        kind = node.get("kind")
        if kind in ("ReturnStmt", "GotoStmt", "IndirectGotoStmt"):
            return True
        if kind == "BreakStmt" and depth == 0:
            return True
        nested = depth + (kind in ("ForStmt", "WhileStmt", "DoStmt", "SwitchStmt"))
        return any(Analysis.leaves(child, nested) for child in node.get("inner") or [])

    # the bytes ------------------------------------------------------------------------
    def symbol_at(self, offset: int) -> tuple[str | None, int, int]:
        """The D_ symbol whose extent (up to the next symbol) holds offset:
        (name, start, end); before the first, (None, offset, first)."""
        start = None
        for one in self.starts:
            if one <= offset:
                start = one
        following = [one for one in self.starts if one > offset]
        end = following[0] if following else self.size
        if start is None:
            return None, offset, end
        names = sorted(name for name, at in self.data.items() if at == start)
        # Not the declared size: a variable index or a walk to a sentinel may
        # run past it, up to the next symbol.
        return names[0], start, end

    def extent(self, offset: int) -> range:
        _, start, end = self.symbol_at(offset)
        return range(start, end)

    def byte_ranges(self, value) -> list[range]:
        table = self.row.command_table
        kind = value[0]
        if kind == "slotword":
            width = value[2]
            if width is None:
                return [range(value[1], self.extent(value[1]).stop)]
            return [range(value[1], value[1] + width)]
        if kind == "slot":
            return [range(value[1], self.extent(value[1]).stop)]
        if kind in ("slotdynword", "slotdyn"):
            return [self.extent(value[1])]
        if kind in ("tablefield", "tableptr"):
            start, stride = value[1], value[2]
            if table and (start, stride) == (table[0], table[1]) and kind == "tablefield" and value[3] == -1:
                return [range(start, start + stride * table[2])]
            if table and (start, stride) == (table[0], table[1]) and kind == "tablefield":
                width = value[4] or stride - value[3]
                return [range(start + n * stride + value[3], start + n * stride + value[3] + width)
                        for n in range(table[2])]
            return [self.extent(start)]
        return []


# --- one row ---------------------------------------------------------------------------

def analyse(row: model_gate.Row, units: list[str] | None = None, names: dict[str, int] | None = None) -> dict:
    """{offset: (class, symbol, evidence)} for each byte of the row's
    [data_start, span) outside its command table, and what the analysis saw.
    `units` and `names` (the C files, and the module's {name: address}) are
    the row's own unless given (test_model_span_words.py's made-up modules)."""
    own = units is not None
    if units is None:
        units = [layout.logical(path) for pattern in model_gate.source_patterns(row, "A")
                 for path in sorted(layout.glob(pattern))]
    if not units:
        raise RuntimeError(f"{row.KIND.name} {row.shape}: no C")
    layouts, functions, declared = {}, {}, {}
    if names is None:
        names = model_gate.module_addresses(row, "A")
    for unit in units:
        layouts.update(scan.layouts(unit))
        tree = unit_ast(unit)
        scan.annotate(tree)
        for node in tree:
            kind = node.get("kind")
            if kind == "FunctionDecl" and any(c.get("kind") == "CompoundStmt" for c in node.get("inner") or []):
                functions.setdefault(node["name"], node)
            elif kind == "VarDecl" and node.get("name") in names:
                declared.setdefault(node["name"], []).append(node)
    # The work area's records are known once the module has been walked:
    # walk again from nothing until they stay the same, so no pointer was
    # taken as anywhere in the work area for want of its member's extent.
    # Every name of the module's image the C refers to is one of the data
    # names module_addresses found: one it missed would read as the game's
    # memory, its uses unseen.
    base, size = row.KIND.base["A"], row.KIND.image_size
    missing = set()

    def names_used(node):
        if node.get("kind") == "DeclRefExpr":
            name = node.get("referencedDecl", {}).get("name", "")
            match = re.fullmatch(r"D_([0-9A-F]{8})", name)
            if match and base <= int(match.group(1), 16) < base + size and name not in names:
                missing.add(name)
        for child in node.get("inner") or []:
            names_used(child)
    for node in functions.values():
        names_used(node)
    if missing:
        raise RuntimeError(f"{row.KIND.name} {row.shape}: the C uses {sorted(missing)}, which "
                           "model_gate.module_addresses does not list")
    # Only the module's own functions and what they call.
    homes = {posixpath.normpath(str(Path(unit).resolve()).replace("\\", "/")) for unit in units} if own else set()
    reach = [name for name, node in functions.items()
             if model_gate_port.NATIVE in posixpath.normpath((node.get("_where") or ("",))[0] or "")
             or posixpath.normpath(str(Path((node.get("_where") or ("",))[0] or ".").resolve()).replace("\\", "/"))
             in homes]
    seen = set(reach)
    while reach:
        name = reach.pop()

        def callees(node):
            if node.get("kind") == "DeclRefExpr" and node.get("referencedDecl", {}).get("kind") == "FunctionDecl":
                callee = node["referencedDecl"].get("name")
                if callee in functions and callee not in seen:
                    seen.add(callee)
                    reach.append(callee)
            for child in node.get("inner") or []:
                callees(child)
        callees(functions[name])
    functions = {name: node for name, node in functions.items() if name in seen}
    records = set(run(row, layouts, functions, set(), declared, names, rounds=3).work_records)
    for _ in range(4):
        module = run(row, layouts, functions, records, declared, names)
        if module.work_records <= records:
            break
        records |= module.work_records
    return finish(row, module, declared, units)


def run(row: model_gate.Row, layouts: dict, functions: dict, records: set[str], declared: dict,
        names: dict[str, int], rounds: int = ROUNDS) -> Analysis:
    module = Analysis(row, names)
    module.layouts.update(layouts)
    module.work_records |= records
    for name, nodes in declared.items():
        for node in nodes:
            t = node.get("type", {})
            size = module.size_of(t.get("desugaredQualType") or t.get("qualType", ""))
            if size:
                start = module.data[name]
                module.sizes[start] = max(size, module.sizes.get(start, 0))
    module.function_params = {name: [c.get("name") for c in node.get("inner") or [] if c.get("kind") == "ParmVarDecl"]
                              for name, node in functions.items()}
    entry = f"func_{module.base + 4:08X}"
    if entry not in module.function_params:
        raise RuntimeError(f"{row.KIND.name} {row.shape}: no {entry}")
    params = module.function_params[entry]
    if params:
        module.params[(entry, params[0])] = {WORK}
    if len(params) > 1:
        module.params[(entry, params[1])] = {COMMAND}
    called, escaped = set(), set()

    def calls(node, callee=False):
        if node.get("kind") == "DeclRefExpr" and node.get("referencedDecl", {}).get("kind") == "FunctionDecl":
            name = node["referencedDecl"].get("name")
            called.add(name)
            if not callee and name in module.function_params:
                escaped.add(name)   # its address taken: a callback, or called through a pointer
        inner = node.get("inner") or []
        for n, child in enumerate(inner):
            through = node.get("kind") == "CallExpr" and n == 0 or \
                callee and node.get("kind") in ("ImplicitCastExpr", "ParenExpr")
            calls(child, through)
    for node in functions.values():
        calls(node)
    for name in sorted(escaped):
        module.locks.setdefault(f"{name}'s address taken (a callback, or a call through a pointer)", name)
    for name, params in module.function_params.items():
        for param in params:
            module.params.setdefault((name, param), set())
        if name != entry and name not in called and params and name in module.functions:
            # A callback the game calls: the work area first (model_gate_scan.py).
            module.params[(name, params[0])] |= {WORK}
            for param in params[1:]:
                module.params[(name, param)] |= {OTHER}
    limit, rounds = rounds, 0
    module.changed = True
    while module.changed and rounds < limit:
        module.changed = False
        rounds += 1
        module.events = []
        # A function whose address the module gives away may be called by the
        # game with anything the module handed the game or left in its memory.
        handed = set(module.external)
        for values in module.pool.values():
            handed |= {v for v in values if v[0] in DATA_VALUE + DATA_POINTER}
        for name in escaped:
            for param in module.function_params[name]:
                module.changed |= module.grow(module.params[(name, param)], handed)
        for name, node in functions.items():
            module.function = name
            module.function_locals = set()

            def collect(n):
                if n.get("kind") == "VarDecl" and n.get("storageClass") not in ("extern", "static"):
                    module.function_locals.add(n.get("name"))
                for c in n.get("inner") or []:
                    collect(c)
            collect(node)
            module.current_sites = []
            for child in node.get("inner") or []:
                if child.get("kind") == "CompoundStmt":
                    module.walk(child)
    module.rounds = rounds
    return module


def finish(row: model_gate.Row, module: Analysis, declared: dict, units: list[str]) -> dict:
    rounds = module.rounds
    converged = not module.changed
    if module.game_sinks and module.setters:
        # The game may keep what the module hands it and give it back
        # (a setter and a getter, or a game global).
        (kind, origin), where = sorted(module.game_sinks.items())[0]
        module.locks.setdefault(f"a {kind} from the game ({origin}) in a module that hands the game its data "
                                f"({sorted(module.setters)[0]})", where)
    locks = dict(module.locks)
    table = row.command_table
    first, last = (table[0], table[0] + table[1] * table[2]) if table else (0, 0)
    region = [at for at in range(row.data_start, row.span) if not first <= at < last]
    uses: dict[int, dict[str, str]] = collections.defaultdict(dict)
    if converged:
        for kind, tags, where in module.events:
            for value in tags:
                if kind == "texture" and value[0] == "slot":
                    spans = [range(value[1], value[1] + GSIMAGE)]
                elif kind == "texture" and value[0] == "slotdyn":
                    whole = module.extent(value[1])
                    if len(whole) % GSIMAGE:
                        spans = [whole]
                        kind = "address"   # not an array of headers: the packing may read anything
                    else:
                        spans = [whole]
                elif kind == "address" or value[0] in DATA_VALUE:
                    spans = module.byte_ranges(value)
                else:
                    continue   # an address used as a divisor or an index: not the data's value
                for one in spans:
                    for at in one:
                        uses[at].setdefault(kind, where)
    for name, nodes in declared.items():
        if any(node.get("storageClass") != "extern" and node.get("inner") for node in nodes):
            for at in module.extent(module.data[name]):
                uses[at].setdefault("copy", f"{name} defined in the C")
    out = {}
    for at in region:
        symbol, start, _ = module.symbol_at(at)
        label = f"{symbol}+0x{at - start:X}" if symbol and at != start else (symbol or "-")
        if not converged:
            out[at] = ("unknown", label, f"values did not settle in {ROUNDS} rounds")
            continue
        if locks:
            reason, where = sorted(locks.items())[0]
            out[at] = ("unknown", label, f"row locked: {reason} ({where})")
            continue
        found = uses.get(at, {})
        for kind in ORDER:
            if kind in found:
                out[at] = (kind, label, found[kind])
                break
        else:
            out[at] = ("unread", label, "not read by this module's own C")
    return {"bytes": out, "rounds": rounds, "unhandled": dict(module.unhandled), "converged": converged,
            "units": units, "locks": locks}


def words(row: model_gate.Row, found: dict) -> list[dict]:
    """The row's words, each the first class (ORDER) of its bytes, in runs
    of words with the same symbol, class and evidence."""
    by_word: dict[int, list[int]] = collections.defaultdict(list)
    for at in sorted(found):
        by_word[at & ~3].append(at)
    runs: list[dict] = []
    for word in sorted(by_word):
        offsets = by_word[word]
        best = min((found[at] for at in offsets), key=lambda c: ORDER.index(c[0]))
        symbol = found[offsets[0]][1]
        symbol = re.sub(r"\+0x[0-9A-F]+$", "", symbol)
        start, size = offsets[0], offsets[-1] + 1 - offsets[0]
        last = runs[-1] if runs else None
        if last and last["offset"] + last["size"] == start and \
                (last["symbol"], last["class"], last["evidence"]) == (symbol, best[0], best[2]) and \
                len(offsets) == size:
            last["size"] += size
            continue
        runs.append({"kind": row.KIND.name, "shape": row.shape, "offset": start, "size": size, "symbol": symbol,
                     "class": best[0], "evidence": best[2]})
    return runs


def _one(job: tuple[str, int]) -> tuple[str, int, list[dict] | None, dict]:
    kind, shape = job
    rows = {(r.KIND.name, r.shape): r for r in all_rows()}
    row = rows[(kind, shape)]
    try:
        result = analyse(row)
        if kind == "variant":
            # The words model_gate_scan.py finds the C divides by or indexes
            # with at a fixed offset: each must come out unsafe here.
            result["slot_words"] = sorted(scan.classify(row, row.KIND.image_size, row.data_start)["slot_words"])
    except Exception as error:   # noqa: BLE001 - one row's failure is reported, not fatal
        return kind, shape, None, {"error": repr(error)}
    runs = words(row, result["bytes"])
    digest = row_digest(row)
    for run in runs:
        run["digest"] = digest
    return kind, shape, runs, {k: v for k, v in result.items() if k != "bytes"}


def all_rows() -> list[model_gate.Row]:
    return [row for row in model_gate.native_rows() + model_gate.native_primary_rows()
            if row.data_start is not None and row.span > row.data_start]


def generate(jobs: int, only: set[tuple[str, int]] | None = None) -> tuple[list[dict], list[str], dict]:
    work = [(row.KIND.name, row.shape) for row in all_rows() if only is None or (row.KIND.name, row.shape) in only]
    entries, problems, info = [], [], {}
    with concurrent.futures.ProcessPoolExecutor(max(1, jobs)) as pool:
        for kind, shape, runs, extra in pool.map(_one, work):
            info[(kind, shape)] = extra
            if runs is None:
                problems.append(f"{kind} {shape}: {extra['error']}")
                continue
            entries += runs
    entries.sort(key=lambda e: (e["kind"] != "variant", e["shape"], e["offset"]))
    tool, compiler = tool_digest(), compiler_version()
    for entry in entries:
        entry["tool"], entry["compiler"] = tool, compiler
    return entries, problems, info


# --- the table -------------------------------------------------------------------------

def render(entries: list[dict]) -> str:
    handle = io.StringIO()
    writer = csv.DictWriter(handle, COLUMNS, lineterminator="\n")
    writer.writeheader()
    for entry in entries:
        writer.writerow({**entry, "offset": f"0x{entry['offset']:X}", "size": entry["size"]})
    return handle.getvalue()


def load(path: Path = TABLE) -> dict[tuple[str, int], list[tuple[int, int, str, str, str]]]:
    """{(kind, shape): [(offset, size, class, symbol, evidence)]}; empty
    without the table."""
    out: dict[tuple[str, int], list] = collections.defaultdict(list)
    if not path.exists():
        return out
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if tuple(reader.fieldnames or ()) != COLUMNS:
            raise SystemExit(f"{path}: the columns are {reader.fieldnames}, not {list(COLUMNS)}")
        for line in reader:
            out[(line["kind"], int(line["shape"]))].append(
                (int(line["offset"], 0), int(line["size"]), line["class"], line["symbol"], line["evidence"]))
    return out


def tool_digests(path: Path = TABLE) -> set[str]:
    """The tool digests the table's lines carry (one, unless it is broken)."""
    if not path.exists():
        return set()
    with path.open(newline="", encoding="utf-8") as handle:
        return {line.get("tool") or "" for line in csv.DictReader(handle)}


def compiler_note(path: Path = TABLE) -> str:
    """A note when the table was made with another check compiler than the
    one on PATH (classes come from its AST): reported, not a failure."""
    if not path.exists():
        return ""
    with path.open(newline="", encoding="utf-8") as handle:
        stored = {line.get("compiler") or "" for line in csv.DictReader(handle)}
    now = compiler_version()
    if now and stored and stored != {now}:
        return f"{path.name} was made with {', '.join(sorted(stored))}; the check compiler here is {now}"
    return ""


def tool_causes(path: Path = TABLE) -> list[str]:
    """Why the table's tool digest is not the tree's tool (its files or the
    shared headers changed): every word of the table is to be distrusted."""
    stored = tool_digests(path)
    actual = tool_digest()
    if stored and stored != {actual}:
        return [f"{path.name} was made by another version of the analysis or of the headers it reads (tool digest "
                f"{', '.join(sorted(stored))}, now {actual})"]
    return []


def tool_stale(path: Path = TABLE) -> list[str]:
    """tool_causes, each with what to do about it (remedy)."""
    return [f"{cause}; {remedy()}" for cause in tool_causes(path)]


def digests(path: Path = TABLE) -> dict[tuple[str, int], str]:
    """{(kind, shape): the digest of the C the row's words were classed from}."""
    out: dict[tuple[str, int], str] = {}
    if not path.exists():
        return out
    with path.open(newline="", encoding="utf-8") as handle:
        for line in csv.DictReader(handle):
            out[(line["kind"], int(line["shape"]))] = line.get("digest") or ""
    return out


def stale_rows(path: Path = TABLE, keys=None) -> list[tuple[tuple[str, int], str, str]]:
    """[((kind, shape), the table's digest, the C's now)] of the rows whose C
    (row_digest) is not the C the table was made from."""
    stored = digests(path)
    rows = {(row.KIND.name, row.shape): row for row in all_rows()}
    found = []
    for key in sorted(keys if keys is not None else stored):
        row = rows.get(key)
        if row is None or key not in stored:
            continue
        actual = row_digest(row)
        if stored[key] != actual:
            found.append((key, stored[key], actual))
    return found


def stale_causes(path: Path = TABLE, keys=None) -> list[str]:
    """Why the labels of the rows whose C is not the C the table was made from
    cannot be trusted (mod-migrate says so in its report)."""
    return [f"{key[0]} {key[1]}: its C changed since {path.name} was made (digest {then}, now {now})"
            for key, then, now in stale_rows(path, keys)]


def stale(path: Path = TABLE, keys=None) -> list[str]:
    """stale_causes, each with what to do about it (remedy)."""
    return [f"{cause}; {remedy()}" for cause in stale_causes(path, keys)]


def stale_warnings(path: Path = TABLE) -> list[str]:
    """What to say, as a warning, when the table is not the tree's: the tool
    digest, and which rows' C (once, with the regeneration command). Labels
    describe and gate nothing, so a stale table is told and never fails a test
    or a release; coverage (a native row without labels, a span that changed
    size) does fail."""
    warnings = tool_stale(path)
    rows = stale_rows(path)
    if rows:
        names = [f"{kind} {shape}" for (kind, shape), _, _ in rows]
        more = f" and {len(names) - 20} more" if len(names) > 20 else ""
        warnings.append(f"{len(rows)} rows' C changed since {path.name} was made, so mod-migrate calls their labels "
                        f"stale: {', '.join(names[:20])}{more}; {remedy()}")
    return warnings


def warn(warnings: list[str]) -> None:
    for text in warnings:
        print(f"model_span_words: WARNING: {text}", file=sys.stderr)


def class_of(table: dict, kind: str, shape: int, offset: int) -> tuple[str, str, str] | None:
    """(class, symbol, evidence) of the byte at offset of a row's span, None
    when the table has no word there."""
    for start, size, cls, symbol, evidence in table.get((kind, shape), ()):
        if start <= offset < start + size:
            return cls, symbol, evidence
    return None


def coverage(table: dict) -> list[str]:
    """Every byte of every native row's [data_start, span) outside its
    command table has a word, and nothing else does; classes are known."""
    problems = []
    rows = {(row.KIND.name, row.shape): row for row in all_rows()}
    for key in table:
        if key not in rows:
            problems.append(f"{key[0]} {key[1]}: in {TABLE.name} but not a native row with words past its data start")
    for key, row in rows.items():
        commanded = row.command_table
        first, last = (commanded[0], commanded[0] + commanded[1] * commanded[2]) if commanded else (0, 0)
        wanted = {at for at in range(row.data_start, row.span) if not first <= at < last}
        have = collections.Counter()
        for start, size, cls, _, _ in table.get(key, ()):
            if cls not in CLASSES:
                problems.append(f"{key[0]} {key[1]} 0x{start:X}: class {cls!r}")
            for at in range(start, start + size):
                have[at] += 1
        missing = sorted(wanted - set(have))
        extra = sorted(set(have) - wanted)
        twice = sorted(at for at, n in have.items() if n > 1)
        for what, offsets in (("lacks", missing), ("has bytes outside its region at", extra), ("repeats", twice)):
            if offsets:
                problems.append(f"{key[0]} {key[1]}: {TABLE.name} {what} 0x{offsets[0]:X}"
                                f"{f' and {len(offsets) - 1} more' if len(offsets) > 1 else ''} "
                                f"(region 0x{row.data_start:X}-0x{row.span:X}, table 0x{first:X}-0x{last:X})")
    return problems


def wrappers() -> list[str]:
    """Slot B's C is slot A's renamed: the C files of slot B's closure that
    are more than #include and #define lines are slot A's, so slot A's
    classes are slot B's."""
    problems = []
    directive = re.compile(r"#\s*(include|define|undef)\b")

    def code(paths) -> set:
        found = set()
        for path in paths:
            for one in model_gate.port_closure(path):
                one = Path(one).resolve()
                if one.suffix != ".c":
                    continue
                text = model_gate.without_comments(one.read_text(encoding="utf-8", errors="replace"))
                if any(line.strip() and not directive.match(line.strip()) for line in text.splitlines()):
                    found.add(one)
        return found
    for row in all_rows():
        a = code([unit for pattern in model_gate.source_patterns(row, "A") for unit in sorted(ROOT.glob(pattern))])
        b = code([unit for pattern in model_gate.source_patterns(row, "B") for unit in sorted(ROOT.glob(pattern))])
        if not b or a != b:
            problems.append(f"{row.KIND.name} {row.shape}: slot B's C is not slot A's renamed "
                            f"({sorted(p.name for p in b ^ a)})")
    return problems


UNSAFE = ("divisor", "index", "bound", "pointer", "shift", "unknown")


def widened(table: dict, info: dict) -> tuple[list[str], int]:
    """Each word model_gate_scan.py finds the C divides by or indexes with at
    a fixed offset (its slot words) that lies in a row's region must be
    classed unsafe here: (problems, how many such words were checked)."""
    problems, checked = [], 0
    for (kind, shape), extra in sorted(info.items()):
        for word in extra.get("slot_words", ()):
            found = class_of(table, kind, shape, word)
            if found is None:
                continue
            checked += 1
            if found[0] not in UNSAFE:
                problems.append(f"{kind} {shape}: model_gate_scan.py's slot word 0x{word:X} is {found[0]} here")
    return problems, checked


def main() -> int:
    if layout.SHIPPED:
        print("model_span_words: regenerating the labels needs the repository (the C, the analysis and clang): "
              "an SDK carries the table its release was made with, and model_gate.py mod-check says whether it is "
              "intact", file=sys.stderr)
        return 2
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--check", action="store_true", help="compare the table with a fresh analysis (a table that "
                        "differs is a warning; a row without labels or a span that changed size fails)")
    parser.add_argument("--shape", type=int, action="append", help="only these shapes (with --explain)")
    parser.add_argument("--kind", choices=("variant", "primary"), default=None)
    parser.add_argument("--explain", action="store_true", help="print the rows' words, do not write")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    arguments = parser.parse_args()
    only = None
    if arguments.shape:
        only = {(row.KIND.name, row.shape) for row in all_rows() if row.shape in arguments.shape and
                (arguments.kind is None or row.KIND.name == arguments.kind)}
    entries, problems, info = generate(arguments.jobs, only)
    for key, extra in sorted(info.items()):
        if extra.get("unhandled"):
            print(f"{key[0]} {key[1]}: constructs taken as their parts: {extra['unhandled']}", file=sys.stderr)
        if extra.get("converged") is False:
            print(f"{key[0]} {key[1]}: values did not settle in {ROUNDS} rounds; every word is unknown",
                  file=sys.stderr)
    text = render(entries)
    if arguments.explain:
        sys.stdout.write(text)
    elif arguments.check:
        warnings = []   # labels that are not the analysis's or the tree's: told, never a failure (stale_warnings)
        if only is None:
            current = TABLE.read_text(encoding="utf-8") if TABLE.exists() else ""
            if current != text:
                old = set(current.splitlines())
                new = set(text.splitlines())
                for line in sorted(old - new)[:10]:
                    warnings.append(f"{TABLE.name} has {line}")
                for line in sorted(new - old)[:10]:
                    warnings.append(f"the analysis gives {line}")
                warnings.append(f"{TABLE.relative_to(ROOT)} is stale; regenerate: {REGENERATE}")
        else:
            table = load()
            fresh = load_text(text)
            for key, runs in fresh.items():
                if table.get(key) != runs:
                    warnings.append(f"{key[0]} {key[1]}: {TABLE.name} differs from the analysis; regenerate: {REGENERATE}")
        table = load()
        found, checked = widened(table, info)
        print(f"model_span_words: {checked} of model_gate_scan.py's slot words lie in the rows' regions",
              file=sys.stderr)
        problems += coverage(table) + found + wrappers()
        warn(warnings + stale_warnings())
        if compiler_note():
            print(f"model_span_words: note: {compiler_note()}", file=sys.stderr)
    else:
        if only is not None:
            raise SystemExit("model_span_words: --shape writes nothing without --explain or --check")
        if problems:
            for problem in problems:
                print(problem, file=sys.stderr)
            return 1
        TABLE.write_text(text, encoding="utf-8", newline="\n")
        table = load()
        found, checked = widened(table, info)
        print(f"model_span_words: {checked} of model_gate_scan.py's slot words lie in the rows' regions",
              file=sys.stderr)
        problems = coverage(table) + found + wrappers()
        warn(stale_warnings())
        counts = collections.Counter()
        for runs in table.values():
            for _, size, cls, _, _ in runs:
                counts[cls] += (size + 3) // 4
        print(f"{TABLE.relative_to(ROOT)}: {len(entries)} runs over {len(table)} rows; words by class: "
              + ", ".join(f"{k} {counts[k]}" for k in ORDER if counts[k]))
    for problem in problems:
        print(problem, file=sys.stderr)
    return 1 if problems else 0


def load_text(text: str) -> dict:
    out: dict = collections.defaultdict(list)
    for line in csv.DictReader(io.StringIO(text)):
        out[(line["kind"], int(line["shape"]))].append(
            (int(line["offset"], 0), int(line["size"]), line["class"], line["symbol"], line["evidence"]))
    return out


if __name__ == "__main__":
    sys.exit(main())
