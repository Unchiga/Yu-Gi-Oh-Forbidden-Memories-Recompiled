#!/usr/bin/env python3
"""The table that decides which MODEL.MRG variant modules run as the decomp's
C and which stay on the MIPS interpreter (notes/pc-build.md, "MIPS-only
effects"), and the tool that keeps it true to the disc.

config/pc/model_gate.csv has a row for every code shape of the variant
modules (324 of them: the first word of the slot A image names one; slot B's
image of the same shape is the same code linked at 0x8017B000, with a first
word of its own):

    shape        the slot A identifier word, which the sources' names carry
                 (src/native_overlays/model_variant/variant<shape>_*.c)
    backend      native      the decomp's C runs it when the bytes match;
                 interpret   not ported yet (the default of a new row);
                 keep        must stay interpreted, the note says why
    span         the bytes from the slot base the hash covers. By default the
                 shape's code and constants, where the decomp's layout
                 starts the module's data (`data_start`); WIDER when the C
                 divides by, or indexes with, a word of the slot's data
                 (see below). A row that is not native gets the code the
                 images have in common, the most a hash can cover
    data_start   the decomp's data start for the shape (native rows)
    command_table  where the C indexes the slot's data by the command its
                 caller passes (a native row's `table:stride:count:field`
                 or `table:stride:count:field:indexN`, or `none` when it
                 does not): the table's offset from the slot base, the entry
                 size, how many entries the hash covers, and the offset in
                 the work area of what the setup left for the updates. That
                 is the POINTER to the entry the setup chose
                 (`work->config = &D_...[command]`), or with `index8`,
                 `index16` or `index32` the COMMAND ITSELF, which the setup
                 stored as a u8, u16 or u32 member and every update indexes
                 the table with (`work->command = command`). A setup command
                 past `count`, or an update whose pointer is not one of the
                 covered entries (whose index is not below `count`), runs
                 the interpreter (model_modules.c). `check` fails a native
                 row whose C indexes a table with the command and whose
                 command_table is `none` or does not cover that table
    sources      the numbers of the upstream headers whose C the module is
                 made of (variant<N>_*.c), separated by `;`; empty: the
                 shape's own number. A few modules mix two
    identifier_a, identifier_b   the first word of each slot's image
    images       distinct slot A images of the shape on the disc
    sha256_a, sha256_b   SHA-256 of the first `span` bytes of each slot's image
    note         why a row is `keep`

The hash is of the code and constants, not of the record's data, which the C
reads where the disc put it: a mod that changes a model's tables or textures
still runs the C, and one that changes the code gets the interpreter
(src/pc/overlays/model_modules.c). **Widening rule for a batch:** the span is
the data start unless the C divides by, takes a remainder by, or indexes an
array with a word it reads from the slot's data, because the interpreter
survives a zero divisor (MIPS divu gives lo = 0xFFFFFFFF, `break 7` is a
no-op) and an out-of-range index where native C crashes (x86) or reads
elsewhere. Then the span grows to cover those words (up to what every retail
image agrees on: `check --disc` fails when the images of a row differ inside
its span), so that a mod that changes them gets the interpreter, and the
row's note says which words and why. The reviewer of a batch greps the
shape's C for `/`, `%` and `[` on values derived from the slot, **and on the
command**, which the caller takes from the record (a mod controls it): an
index from the caller is bounded by `command_table`, not by the span.
Everything but backend, note, data_start, sources, command_table and a
native row's span is written from the disc by `update`, never by hand.

    python tools/pc/model_gate.py check                  # the table, the header and the address sections agree
    python tools/pc/model_gate.py check --disc game/rpg-yfm.bin   # and the disc: hashes, spans, boundaries
    python tools/pc/model_gate.py update --disc game/rpg-yfm.bin  # rewrite the table from the disc
    python tools/pc/model_gate.py write                  # rewrite src/pc/overlays/model_gate_table.inc
    python tools/pc/model_gate.py port 418 433 --disc game/rpg-yfm.bin   # copy shapes' C from upstream

To port a shape: `port` copies the decomp's C by rule (model_gate_port.py; the
slot B wrappers go to src/native_overlays/model_variant_slot1) and sets the
row's data start and sources; model_gate_scan.py proposes its backend, span and
command_table. Then run `update --disc` and
`python tools/pc/build_game32.py` to write the address sections
(config/pc/guest_addresses.txt). `check --disc` reports how far the images
agree beyond the span.

Exit status 0 when everything agrees.
"""
from __future__ import annotations

import argparse
import collections
import csv
import hashlib
import os
from pathlib import Path
import re
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
ROOT = Path(__file__).resolve().parents[2]
TABLE = ROOT / "config/pc/model_gate.csv"
HEADER = ROOT / "src/pc/overlays/model_gate_table.inc"
ADDRESSES = ROOT / "config/pc/guest_addresses.txt"
COLUMNS = ("shape", "backend", "span", "data_start", "sources", "command_table", "identifier_a", "identifier_b",
           "images", "sha256_a", "sha256_b", "note")
NO_TABLE = 0xFFFFFFFF   # model_modules.c: the C takes no command index
BACKENDS = ("native", "interpret", "keep")
INDEX_WIDTH = {"index8": 1, "index16": 2, "index32": 4}   # a command_table's stored index, in bytes
MAX_WORK_FIELD = 0x10000   # no variant module's work area is larger (the largest state is about 16 KB)
IMAGE_SIZE = 0x5000        # the ten sectors a variant image is (model_oracle.py, VARIANT_SECTORS)
BASE = {"A": 0x8013B000, "B": 0x8017B000}
SLOT_SUFFIX = {"A": "", "B": "_slot1"}


class Row(dict):
    """One shape's row; the numbers parsed."""

    @property
    def shape(self) -> int:
        return int(self["shape"])

    @property
    def span(self) -> int:
        return int(self["span"], 0)

    @property
    def data_start(self) -> int | None:
        return int(self["data_start"], 0) if self["data_start"] else None

    @property
    def sources(self) -> list[int]:
        """The upstream header numbers the module's C is named after."""
        return [int(number) for number in self["sources"].split(";")] if self["sources"] else [self.shape]

    @property
    def command_table(self) -> tuple[int, int, int, int, int] | None:
        """(table offset, entry size, entries covered, work-area offset of the
        entry pointer or of the stored command index, the index's width in
        bytes: 0 for a pointer), None for `none` or no table."""
        text = self["command_table"]
        if text in ("", "none"):
            return None
        parts = text.split(":")
        if len(parts) not in (4, 5):
            raise ValueError(text)
        table, stride, count, field = (int(part, 0) for part in parts[:4])
        if min(table, stride, count, field) < 0:
            raise ValueError(text)
        width = 0
        if len(parts) == 5:
            if parts[4] not in INDEX_WIDTH:
                raise ValueError(text)
            width = INDEX_WIDTH[parts[4]]
        return table, stride, count, field, width

    def identifier(self, slot: str) -> int:
        return int(self["identifier_" + slot.lower()])

    def sha256(self, slot: str) -> str:
        return self["sha256_" + slot.lower()]

    def module(self, slot: str) -> str:
        """The build's name for the shape's module in a slot (tools/pc/build_config.py)."""
        return f"model_variant_{self.shape}{SLOT_SUFFIX[slot]}"

    def entry(self, slot: str) -> str:
        """The C's entry as the build names it: the module's prefix and the
        decomp's name for the function at the slot base + 4 (GATED_MODULES)."""
        return f"{self.module(slot)}__func_{BASE[slot] + 4:08X}"


def load_table(path: Path = TABLE) -> list[Row]:
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if tuple(reader.fieldnames or ()) != COLUMNS:
            raise SystemExit(f"{path}: the columns are {reader.fieldnames}, not {list(COLUMNS)}")
        return [Row(row) for row in reader]


def native_rows(path: Path = TABLE) -> list[Row]:
    return [row for row in load_table(path) if row["backend"] == "native"]


def write_table(rows: list[Row], path: Path = TABLE) -> None:
    """Written whole or not at all: to a temporary file, then in place."""
    temporary = path.with_suffix(".csv.new")
    with temporary.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, COLUMNS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(sorted(rows, key=lambda row: row.shape))
    temporary.replace(path)


# --- what the build needs ---------------------------------------------------------

SOURCES = ROOT / "config/pc/model_gate_sources.txt"


_LISTED: dict[tuple, dict[str, list[str]]] = {}
_TEXTS: dict[str, str] = {}


def listed_sources(root: Path = ROOT) -> dict[str, list[str]]:
    """{module: [C files]} from config/pc/model_gate_sources.txt, which
    `port` writes from the upstream layouts (model_gate_port.py); read once
    while the file stays the same."""
    import model_gate_port
    path = root / SOURCES.relative_to(ROOT)
    key = (str(path), path.stat().st_mtime_ns if path.exists() else 0)
    if key not in _LISTED:
        _LISTED[key] = model_gate_port.read_sections(path)
    return _LISTED[key]


def source_patterns(row: Row, slot: str, root: Path = ROOT) -> tuple[str, ...]:
    """The module's C files in a slot: the list `port` wrote, or for a shape
    it has no list of, the globs of the shape's own headers (a tuple: a module
    may be made of the C of several)."""
    listed = listed_sources(root).get(row.module(slot))
    if listed:
        return tuple(listed)
    folder = "model_variant" if slot == "A" else "model_variant_slot1"
    return tuple(f"src/native_overlays/{folder}/variant{number}_*.c" for number in row.sources)


SYMBOL = re.compile(r"\b(func|D)_([0-9A-F]{8})\b")
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
ALIAS = re.compile(r"^\s*#\s*define\s+((?:func|D)_[0-9A-F]{8})\s+((?:func|D)_[0-9A-F]{8})\s*$", re.M)
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def without_comments(text: str) -> str:
    return COMMENT.sub(" ", text)


def _text(path: Path) -> str:
    """A file's text without comments, read once a process."""
    key = str(path)
    if key not in _TEXTS:
        _TEXTS[key] = without_comments(path.read_text(encoding="utf-8", errors="replace"))
    return _TEXTS[key]


_CLOSURES: dict[str, list[Path]] = {}


def port_closure(path: Path, root: Path = ROOT) -> list[Path]:
    """A C file and every file of the repository it includes, transitively
    (beside the including file first, then from src/); once a process."""
    start = os.path.normpath(str(path))
    if start in _CLOSURES:
        return _CLOSURES[start]
    seen, todo = [], [start]
    while todo:
        current = todo.pop(0)
        if current in seen or not os.path.isfile(current):
            continue
        seen.append(current)
        for name in INCLUDE.findall(_text(Path(current))):
            beside = os.path.normpath(os.path.join(os.path.dirname(current), name))
            todo.append(beside if os.path.isfile(beside) else os.path.normpath(os.path.join(root, "src", name)))
    _CLOSURES[start] = [Path(one) for one in seen]
    return _CLOSURES[start]


def module_addresses(row: Row, slot: str, root: Path = ROOT) -> dict[str, int]:
    """{name: address} for the module's own functions and data, for
    config/pc/guest_addresses.txt. Retail ELFs give these for the modules
    that have one; the decomp's names carry their address, so the module's C
    (and, for slot B, its wrappers) gives them here: every func_ and D_ name
    the module's C files and the C files they include use outside comments
    that falls inside the slot's image (a header's prototypes of another
    module's functions are not the module's; build_game32.py stops on a name
    of the module's objects the section lacks). A `#define func_X func_Y` (a
    wrapper putting C written at one address at another: slot B's, or another
    region's C at the US address) makes func_X func_Y."""
    base = BASE[slot]
    texts = []
    for pattern in source_patterns(row, slot, root):
        for path in sorted(root.glob(pattern)):
            for one in port_closure(path, root):
                if one.suffix == ".c":
                    texts.append(_text(one))
    renamed = {old: new for text in texts for old, new in ALIAS.findall(text)}
    found = {}
    for text in texts:
        for kind, digits in SYMBOL.findall(ALIAS.sub(lambda match: match.group(2), text)):
            name = f"{kind}_{digits}"
            seen = set()
            while name in renamed and name not in seen:
                seen.add(name)
                name = renamed[name]
            address = int(name.split("_")[1], 16)
            if base <= address < base + IMAGE_SIZE:
                found[name] = address
    return found


def address_sections(root: Path = ROOT) -> dict[str, dict[str, int]]:
    """{module name: addresses} for every native shape, in the table's order."""
    sections = {}
    for row in native_rows(root / "config/pc/model_gate.csv"):
        for slot in "AB":
            sections[row.module(slot)] = module_addresses(row, slot, root)
    return sections


def header_text(rows: list[Row]) -> str:
    lines = ["/* Written by tools/pc/model_gate.py from config/pc/model_gate.csv: do not edit.\n",
             " * One row for each variant module the decomp's C stands in for:\n",
             " * MODEL_GATE_ROW(slot, identifier, span, sha256, entry, entries, table, stride, field, index_width):\n",
             " * entries is NO_TABLE for a C that takes no command index; otherwise the setup command\n",
             " * must be below it, and an update's entry (at `field` in the work area) covered: with\n",
             " * index_width 0 a pointer to one of the `entries` of `stride` bytes from `table` (offsets\n",
             " * from the slot base), else the stored command itself, a u8, u16 or u32, below `entries`. */\n"]
    for row in sorted(rows, key=lambda row: row.shape):
        if row["backend"] != "native":
            continue
        for slot in "AB":
            table = row.command_table
            bound = (f"0x{table[2]:X}u, 0x{table[0]:X}u, 0x{table[1]:X}u, 0x{table[3]:X}u, {table[4]}u" if table
                     else f"0x{NO_TABLE:X}u, 0u, 0u, 0u, 0u")
            lines.append(f'MODEL_GATE_ROW({"AB".index(slot)}, {row.identifier(slot)}u, 0x{row.span:X}u, '
                         f'"{row.sha256(slot)}", {row.entry(slot)}, {bound}) /* shape {row.shape} */\n')
    return "".join(lines)


# --- the disc -----------------------------------------------------------------------

def common_prefix(images: list[bytes]) -> int:
    """How many leading bytes (a whole number of words) every image shares."""
    first = images[0]
    length = IMAGE_SIZE
    for image in images[1:]:
        at = 0
        while at < length and first[at:at + 4] == image[at:at + 4]:
            at += 4
        length = at
    return length


def divergence(images: list[bytes]) -> int | None:
    """The first offset where the images differ, None when they all agree."""
    length = common_prefix(images)
    return None if length >= IMAGE_SIZE else length


class Shape:
    """What the disc has of one shape: the distinct images of each slot."""

    def __init__(self, shape: int, identifier_b: int):
        self.shape, self.identifier_b = shape, identifier_b
        self.images = {"A": {}, "B": {}}     # sha256 of the whole image -> bytes
        self.records: set[tuple[int, int]] = set()

    def common(self, slot: str) -> int:
        return common_prefix(list(self.images[slot].values()))


def read_shapes(disc_path: Path) -> dict[int, Shape]:
    """Every variant image on the disc, grouped by shape (the slot A
    identifier). Each shape has one slot B identifier and the other way
    round, or this stops: the pairing is how a row has both slots."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import model_oracle
    disc = model_oracle.Disc(disc_path)
    shapes: dict[int, Shape] = {}
    partner: dict[int, int] = {}
    for record in range(model_oracle.RECORDS):
        for alternative in (0, 1):
            image = {slot: disc.variant(record, alternative, slot)[:IMAGE_SIZE] for slot in "AB"}
            first, second = (struct.unpack_from("<I", image[slot], 0)[0] for slot in "AB")
            if partner.setdefault(second, first) != first:
                raise SystemExit(f"slot B identifier {second} is shape {partner[second]} and {first}")
            shape = shapes.setdefault(first, Shape(first, second))
            if shape.identifier_b != second:
                raise SystemExit(f"shape {first} has slot B identifiers {shape.identifier_b} and {second}")
            shape.records.add((record, alternative))
            for slot in "AB":
                shape.images[slot].setdefault(hashlib.sha256(image[slot]).hexdigest(), image[slot])
    return shapes


def hashed(shape: Shape, slot: str, span: int) -> str:
    """SHA-256 of the first `span` bytes, which every image of the shape in the slot must share."""
    digests = {hashlib.sha256(image[:span]).hexdigest() for image in shape.images[slot].values()}
    if len(digests) != 1:
        raise SystemExit(f"shape {shape.shape} slot {slot}: its images differ within the first 0x{span:X} bytes "
                         f"(they agree to 0x{shape.common(slot):X}): the span is too long, or the shape is two")
    return digests.pop()


def rewrite(shapes: dict[int, Shape], old: list[Row]) -> list[Row]:
    """The table the disc gives. A row keeps its backend and note, and a
    native or kept row its span; the rest is the disc's."""
    by_shape = {row.shape: row for row in old}
    rows = []
    for number in sorted(shapes):
        shape, row = shapes[number], by_shape.get(number)
        backend = row["backend"] if row else "interpret"
        provisional = min(shape.common("A"), shape.common("B"))
        span = row.span if row and backend in ("native", "keep") else provisional
        rows.append(Row(shape=number, backend=backend, span=f"0x{span:X}",
                        data_start=row["data_start"] if row else "", sources=row["sources"] if row else "",
                        command_table=row["command_table"] if row else "",
                        identifier_a=number, identifier_b=shape.identifier_b,
                        images=len(shape.images["A"]),
                        sha256_a=hashed(shape, "A", span), sha256_b=hashed(shape, "B", span),
                        note=row["note"] if row else ""))
    return rows


def boundaries(shapes: dict[int, Shape], rows: list[Row]) -> list[str]:
    """For each native shape and slot: the images, the bytes they share and
    the span, which may not be longer."""
    lines = []
    for row in rows:
        if row["backend"] != "native":
            continue
        shape = shapes[row.shape]
        for slot in "AB":
            common = shape.common(slot)
            data_start = row.data_start
            lines.append(f"shape {row.shape} slot {slot}: {len(shape.images[slot])} distinct images, "
                         f"{len(shape.records)} (record, alternative) pairs, common prefix 0x{common:X}, "
                         f"span 0x{row.span:X}"
                         + (f" (data start 0x{data_start:X}, widened by {row.span - data_start})"
                            if data_start is not None and row.span != data_start else "")
                         + f", {common - row.span} bytes shared beyond it"
                         + ("" if common >= row.span else "  ** THE SPAN IS LONGER THAN WHAT THEY SHARE **"))
    return lines


# --- checks ---------------------------------------------------------------------------

def problems(rows: list[Row]) -> list[str]:
    bad = []
    for row in rows:
        where = f"shape {row['shape']}"
        if row["backend"] not in BACKENDS:
            bad.append(f"{where}: backend {row['backend']!r} is not one of {BACKENDS}")
        if row["backend"] == "keep" and not row["note"].strip():
            bad.append(f"{where}: a kept row says why in its note")
        try:
            span = row.span
        except ValueError:
            bad.append(f"{where}: span {row['span']!r}")
            continue
        if span % 4 or not 0 < span <= IMAGE_SIZE:
            bad.append(f"{where}: span 0x{span:X} is not a word count inside the image")
        try:
            row.sources
        except ValueError:
            bad.append(f"{where}: sources {row['sources']!r} is not header numbers separated by ;")
        try:
            table = row.command_table
        except ValueError:
            bad.append(f"{where}: command_table {row['command_table']!r} is not table:stride:count:field[:index8|index16|index32]")
            table = None
        if row["backend"] == "native":
            if row["command_table"] == "":
                bad.append(f"{where}: a native row gives its command_table, or `none` when its C takes no command index")
            if table:
                offset, stride, count, field, width = table
                if count <= 0 or stride <= 0:
                    bad.append(f"{where}: command_table has no entries")
                elif field + max(width, 4) > MAX_WORK_FIELD:
                    bad.append(f"{where}: command_table field 0x{field:X} is past any work area (0x{MAX_WORK_FIELD:X} bytes)")
                elif width and count > (1 << (8 * width)):
                    bad.append(f"{where}: an index{8 * width} command_table covers {count} entries, but a "
                               f"u{8 * width} index names {1 << (8 * width)} at most")
                elif offset + stride * count > span:
                    bad.append(f"{where}: the {count} command_table entries end at 0x{offset + stride * count:X}, "
                               f"past the span 0x{span:X}: the hash must cover every entry the commands can pick")
                elif row["data_start"] != "" and offset < row.data_start:
                    bad.append(f"{where}: command_table starts before the data start")
            if row["data_start"] == "":
                bad.append(f"{where}: a native row gives the decomp's data start")
            elif row.data_start > span:
                bad.append(f"{where}: span 0x{span:X} stops before the data start 0x{row.data_start:X}")
            elif row.data_start < span and not row["note"].strip():
                bad.append(f"{where}: the span is wider than the data start: the note says which words it covers")
        for slot in "AB":
            if not re.fullmatch(r"[0-9a-f]{64}", row.sha256(slot)):
                bad.append(f"{where}: slot {slot} has no SHA-256")
    shapes = collections.Counter(row["shape"] for row in rows)
    bad += [f"shape {shape} has {count} rows" for shape, count in shapes.items() if count > 1]
    hashes = collections.Counter((slot, row.sha256(slot)) for row in rows if row["backend"] == "native" for slot in "AB")
    bad += [f"slot {slot}: native rows share the hash {sha[:12]}" for (slot, sha), count in hashes.items() if count > 1]
    return bad


# The command a caller passes (a word of the record, % 1000: a MODEL.MRG mod controls it)
# picks a row of a table in the slot's data. The forms the decomp's C writes it in:
#   D_8013xxxx[command]   &D_8013xxxx[command]   D_8013xxxx[work->command]   D_8013xxxx[(work)->command]
#   D_8013xxxx[command % 100]
#   (T *)(D_8013xxxx + 0xFC) + command
COMMAND_MEMBER = r"(?:\(?\s*\w+\s*\)?\s*->\s*)?command\b"   # command, work->command, (work)->command
COMMAND_INDEX = re.compile(r"\b(D_8013[0-9A-F]{4})\s*\[\s*" + COMMAND_MEMBER + r"[^\]]*\]")
COMMAND_OFFSET = re.compile(r"\(\s*(D_8013[0-9A-F]{4})\s*\+\s*(0x[0-9A-Fa-f]+|\d+)\s*\)\s*\+\s*" + COMMAND_MEMBER)


def command_sites(text: str, renamed: dict[str, str] | None = None) -> list[tuple[int, int]]:
    """(offset in the slot A image, line) of each table the C indexes with the
    command, in a C file's text (comments are blanked with their newlines kept;
    a `#define D_X D_Y` of the text, or of `renamed`, is followed first: a
    wrapper puts C written at another address at this one)."""
    text = COMMENT.sub(lambda match: "\n" * match.group().count("\n"), text)
    aliases = {**(renamed or {}), **dict(ALIAS.findall(text))}
    found = []
    for pattern, offset_group in ((COMMAND_INDEX, None), (COMMAND_OFFSET, 2)):
        for match in pattern.finditer(text):
            symbol = aliases.get(match.group(1), match.group(1))
            extra = int(match.group(offset_group), 0) if offset_group else 0
            found.append((int(symbol[2:], 16) - BASE["A"] + extra, text.count("\n", 0, match.start()) + 1))
    return found


def indexed_tables(row: Row) -> list[tuple[int, str, int]]:
    """(table offset, file, line) for each use of the command as a table index
    in the shape's C (slot A; slot B's files are #defines around it)."""
    files = []
    for pattern in source_patterns(row, "A"):
        for source in sorted(ROOT.glob(pattern)):
            files += [path for path in port_closure(source) if path.suffix == ".c"]
    raws = {path: path.read_text(encoding="utf-8", errors="replace") for path in files}
    renamed = {old: new for raw in raws.values() for old, new in ALIAS.findall(COMMENT.sub(" ", raw))}
    found = set()
    for path, raw in raws.items():
        for offset, line in command_sites(raw, renamed):
            found.add((offset, path.relative_to(ROOT).as_posix(), line))
    return sorted(found)


def command_problems(rows: list[Row], find=indexed_tables) -> list[str]:
    """A native row whose C indexes a table of the slot's data with the command
    must bound it: command_table has to cover that table inside the span, or
    a mod could make the C read (and, through the entry's fields, loop or
    write) past the hash. `none` is for C that takes no command index."""
    bad = []
    for row in rows:
        if row["backend"] != "native":
            continue
        try:
            table = row.command_table
        except ValueError:
            continue
        sites: dict[tuple[int, str], list[int]] = {}
        for offset, path, line in find(row):
            sites.setdefault((offset, path), []).append(line)
        for (offset, path), lines in sites.items():
            where = (f"shape {row.shape}: {path}:{','.join(map(str, lines[:4]))}{'...' if len(lines) > 4 else ''} "
                     f"indexes the table at 0x{offset:X} with the command")
            if table is None:
                bad.append(f"{where}, and the row's command_table is `none` (give table:stride:count:field[:indexN])")
            elif not table[0] <= offset < table[0] + table[1] * table[2]:
                bad.append(f"{where}, outside the command_table's 0x{table[0]:X}..0x{table[0] + table[1] * table[2]:X}")
    return bad


PRINTF = re.compile(r"\bprintf\s*\(\s*\(?\s*(?:const\s+)?(?:char\s*\*\s*\)\s*)?(D_8013[0-9A-F]{4})\b")


def printf_formats(row: Row) -> list[int]:
    """Offsets in the slot A image of the format strings the shape's C hands
    to printf. The interpreter only logs a module's format string
    (mips.c); native C calls the host's printf with it, so a mod that
    changed the string could make it read or write anywhere (%s, %n). The
    span must therefore cover each one through its NUL (check --disc)."""
    found = set()
    for pattern in source_patterns(row, "A"):
        for source in ROOT.glob(pattern):
            for path in port_closure(source):
                text = without_comments(_text(path))
                aliases = dict(ALIAS.findall(text))
                for symbol in PRINTF.findall(text):
                    symbol = aliases.get(symbol, symbol)
                    found.add(int(symbol[2:], 16) - 0x8013B000)
    return sorted(found)


def printf_problems(shapes: dict[int, Shape], rows: list[Row]) -> list[str]:
    bad = []
    for row in rows:
        if row["backend"] != "native":
            continue
        for offset in printf_formats(row):
            for image in shapes[row.shape].images["A"].values():
                end = image.find(b"\0", offset)
                if end < 0 or end + 1 > row.span:
                    bad.append(f"shape {row.shape}: printf's format string at 0x{offset:X} ends at "
                               f"0x{end + 1:X}, past the span 0x{row.span:X}: a mod could change it, and native C "
                               f"passes it to the host's printf (widen the span over it)")
                    break
    return bad


def check_sources(rows: list[Row]) -> list[str]:
    """A native shape has the C of its slot A and the wrappers of slot B,
    and the committed address sections are what the C gives."""
    bad = []
    for row in rows:
        if row["backend"] != "native":
            continue
        for slot in "AB":
            for pattern in source_patterns(row, slot):
                if not list(ROOT.glob(pattern)):
                    bad.append(f"shape {row.shape} is native but {pattern} matches nothing")
    sections: dict[str, dict[str, int]] = {}
    current = None
    for line in ADDRESSES.read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if not parts or parts[0].startswith("#") or parts[0] == "text":
            continue
        if parts[0].startswith("["):
            current = sections.setdefault(parts[0][1:-1], {})
        elif current is not None:
            current[parts[0]] = int(parts[1], 16)
    for row in rows:
        if row["backend"] != "native":
            continue
        for slot in "AB":
            want = module_addresses(row, slot)
            if sections.get(row.module(slot)) != want:
                bad.append(f"config/pc/guest_addresses.txt: [{row.module(slot)}] is not what the C gives "
                           f"({len(want)} names); python tools/pc/build_game32.py writes it")
    return bad


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("check", "update", "write", "port"))
    parser.add_argument("shapes", nargs="*", type=int, help="port: the shapes to copy from upstream")
    parser.add_argument("--disc", type=Path, default=os.environ.get("MEMORIES_DISC"),
                        help="the disc image (default: MEMORIES_DISC)")
    parser.add_argument("--upstream", default="upstream/master",
                        help="port: the upstream decomp's revision (default: upstream/master)")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    arguments = parser.parse_args()
    if arguments.command == "port":
        if not arguments.disc or not Path(arguments.disc).is_file():
            parser.error("port needs the disc (the first word of each module names its shape): --disc or MEMORIES_DISC")
        if not arguments.shapes:
            parser.error("port: name the shapes")
        import model_gate_port
        return model_gate_port.port(arguments.shapes, Path(arguments.disc), arguments.upstream, arguments.jobs)
    rows = load_table()
    failed = problems(rows)
    if arguments.command == "update":
        if not arguments.disc or not Path(arguments.disc).is_file():
            parser.error("update needs the disc: --disc or MEMORIES_DISC")
        shapes = read_shapes(Path(arguments.disc))
        rows = rewrite(shapes, rows)
        write_table(rows)
        print(f"{TABLE.relative_to(ROOT)}: {len(rows)} shapes, {sum(r['backend'] == 'native' for r in rows)} native, "
              f"{sum(r['backend'] == 'keep' for r in rows)} kept")
        print("\n".join(boundaries(shapes, rows)))
        failed = problems(rows)
    if arguments.command in ("update", "write"):
        HEADER.write_text(header_text(rows), encoding="utf-8", newline="\n")
        print(f"{HEADER.relative_to(ROOT)} written")
    if arguments.command == "check":
        if not HEADER.exists() or HEADER.read_text(encoding="utf-8").replace("\r\n", "\n") != header_text(rows):
            failed.append(f"{HEADER.relative_to(ROOT)} is not what the table gives: python tools/pc/model_gate.py write")
        failed += check_sources(rows)
        failed += command_problems(rows)
        if arguments.disc and Path(arguments.disc).is_file():
            shapes = read_shapes(Path(arguments.disc))
            fresh = {row.shape: row for row in rewrite(shapes, rows)}
            for row in rows:
                want = fresh.get(row.shape)
                if want is None:
                    failed.append(f"shape {row.shape} is not on the disc")
                elif [c for c in COLUMNS if str(want[c]) != row[c]]:
                    differing = [c for c in COLUMNS if str(want[c]) != row[c]]
                    failed.append(f"shape {row.shape}: {', '.join(differing)} differ from the disc: update --disc")
            failed += [f"shape {number} is on the disc and not in the table" for number in shapes if number not in {r.shape for r in rows}]
            failed += printf_problems(shapes, rows)
            print("\n".join(boundaries(shapes, rows)))
    for line in failed:
        print("model_gate: " + line, file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
