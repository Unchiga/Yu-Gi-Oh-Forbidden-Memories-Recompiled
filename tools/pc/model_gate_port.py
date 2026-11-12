#!/usr/bin/env python3
"""Port a variant shape's C from the upstream decomp (`model_gate.py port`).

The upstream tree (krystalgamer/memories-decomp, the `upstream` remote) builds
every MODEL.MRG variant module of the US disc from C: its config gives each
module a layout (config/slus_01411/overlays/<module>.yaml) whose `c`
subsegments are the module's C files, at the slot base of each slot. This
copies a shape's files into src/native_overlays without editing them, except
by these rules, which are the whole difference between the two trees:

  1. Every file the layout names, and every file those include, is copied to
     the same place under src/native_overlays that it has under
     src/overlays (model_variant, french_model_variant, spanish_model_variant:
     the folders are one level below src/ in both, so `../../types.h` and
     `../../game/...` stay right). A header outside the overlay folders that
     the port does not have (src/game/model_copy_slot_position.h) is copied
     too; one the port has stays the port's.
  2. An include that reaches another overlay folder through src/
     (`../../overlays/model_variant/model_variant.h`) is written relative to
     src/native_overlays (`../model_variant/model_variant.h`).
  3. Slot B's files (the layout of the module at 0x8017B000: `#define`s of
     the 0x8017Bxxx names around an #include of slot A's C) go to
     `<folder>_slot1`, and an include of a file beside them is written
     `../<folder>/<file>`. The build then finds slot A's C and slot B's
     wrappers in folders of their own.
  4. G32 where tools/pc/check_x64_casts.py finds a cast the 64-bit build gets
     wrong (its `--fix`, on the copied units only; a cast in a macro is fixed
     at the macro's definition).
  5. A copied file that calls a resident function its headers do not declare
     includes src/native_overlays/model_gate_names.h first (declaration_pass).
  6. A local that the retail code reads before the call writes it (STALE_LOCALS)
     takes the storage src/pc/overlays/model_stale_locals.h declares, one per
     slot, in place of the stack: the MIPS code reads what the slot's previous
     call left there (or, where no call writes it, what the oracle shows acts
     as zero), which native C would take from the host's stack. A struct local
     becomes a pointer to its storage, and a file may take several. The
     storage is a game unit's (src/pc/game/zzz_model_stale_locals.c), so it is
     in every save state as the game stack is. `model_gate.py check` fails a
     native row whose C lacks the rule.
  7. A slot whose images have several layouts takes the shortest when every
     other is it plus `c` files that no image of theirs can reach (no j/jal
     to them, no word or lui/addiu pair building their address): code the
     linker left behind (shared_layout).

The module's file list (slot A, slot B) goes to config/pc/model_gate_sources.txt,
which the build compiles: the names of the upstream files do not say which
shape they belong to (shape 59 is french_model_variant/variant76_entry.c,
shape 433 has two of header 450's). The layouts of a shape's images must name
the same files, but for rule 7, or the shape is refused.

The resident functions the C calls by a name the port spells otherwise
(upstream's Model_CopySlotPosition is the port's Model_CopySlotU16Values,
0x80058FB0) are found from upstream's symbol files by address and written to
config/pc/model_gate_names.txt, which the build reads as more names of those
addresses (build_game32.py's aliases)."""
from __future__ import annotations

import collections
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
NATIVE = "src/native_overlays"
SOURCES = ROOT / "config/pc/model_gate_sources.txt"
NAMES = ROOT / "config/pc/model_gate_names.txt"
OVERLAY_FOLDERS = ("model_variant", "french_model_variant", "spanish_model_variant", "spanish_model_primary")
BASES = {"A": 0x8013B000, "B": 0x8017B000}
# The upstream module names of each kind (model_gate.Kind): the variant
# modules (model_variant_*, model_image_*) and the primaries (model_primary_*,
# whose C upstream keeps in spanish_model_primary for every region).
MODULE_PREFIXES = {"variant": ("model_variant_", "model_image_"), "primary": ("model_primary_",)}
INCLUDE = re.compile(r'^(\s*#\s*include\s+")([^"]+)(")', re.M)
SUBSEGMENT = re.compile(r"^\s*-\s*\[(0x[0-9A-Fa-f]+|\d+),\s*([\w.]+),\s*([^\],]+)\]", re.M)


class Upstream:
    """Files of the upstream tree at a git revision, read through one
    `git cat-file --batch` (thousands of small files)."""

    def __init__(self, revision: str = "upstream/master", repository: Path = ROOT):
        self.repository = repository
        self.revision = subprocess.run(["git", "rev-parse", revision], cwd=repository, capture_output=True,
                                       text=True, check=True).stdout.strip()
        self.process = subprocess.Popen(["git", "cat-file", "--batch"], cwd=repository, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE)
        self.cache: dict[str, bytes | None] = {}

    def read(self, path: str) -> bytes | None:
        if path not in self.cache:
            self.process.stdin.write(f"{self.revision}:{path}\n".encode())
            self.process.stdin.flush()
            header = self.process.stdout.readline().split()
            if len(header) == 2 and header[1] == b"missing" or len(header) < 3:
                self.cache[path] = None
            else:
                size = int(header[2])
                data = self.process.stdout.read(size)
                self.process.stdout.read(1)
                self.cache[path] = data
        return self.cache[path]

    def text(self, path: str) -> str:
        data = self.read(path)
        if data is None:
            raise SystemExit(f"upstream {self.revision[:10]} has no {path}")
        return data.decode("utf-8")


# --- the upstream layouts --------------------------------------------------------------

def layouts(upstream: Upstream, disc, kind=None) -> dict[int, dict[str, list[dict]]]:
    """{slot A identifier: {"A": [layout, ...], "B": [...]}} for the US
    MODEL.MRG variant modules. A layout: the module names, the `c`
    subsegments' paths (from src/, no extension) and offsets, the data start
    (the first data or bin subsegment after the header word)."""
    modules = json.loads(upstream.text("config/slus_01411/overlays.json"))["modules"]
    by_word: dict[int, dict[str, dict]] = collections.defaultdict(lambda: {"A": {}, "B": {}})
    first_words = {}
    for module in modules:
        if not module["archive"].upper().endswith("MODEL.MRG"):
            continue
        bases = kind.base if kind is not None else BASES
        image_size = kind.image_size if kind is not None else 0x5000
        slot = {bases["A"]: "A", bases["B"]: "B"}.get(int(module["load_address"], 16))
        if slot is None or not module["name"].startswith(MODULE_PREFIXES[kind.name if kind is not None else "variant"]):
            continue
        text = upstream.text(module["layout"])
        subsegments = [(int(at, 0), kind, path.strip()) for at, kind, path in SUBSEGMENT.findall(text)]
        files = tuple(path for at, kind, path in subsegments if kind == "c")
        # Where each `c` file ends: the next subsegment, or the image's end.
        offsets = sorted({at for at, _, _ in subsegments} | {image_size})
        ends = [next(o for o in offsets if o > at) for at, kind, path in subsegments if kind == "c"]
        starts = tuple(at for at, kind, path in subsegments if kind == "c")
        data_start = min((at for at, kind, path in subsegments if kind in ("data", "bin") and at > 0), default=image_size)
        word = disc.module_word(module["sector_offset"])
        first_words[module["name"]] = word
        key = (files, starts, data_start)
        entry = by_word[word][slot].setdefault(key, {"files": list(files), "starts": list(starts),
                                                     "data_start": data_start, "modules": [], "sectors": [],
                                                     "base": bases[slot], "ends": ends})
        entry["modules"].append(module["name"])
        entry["sectors"].append(module["sector_offset"])
    return {word: {slot: list(found[slot].values()) for slot in "AB"} for word, found in by_word.items()}


class MergeDisc:
    """MODEL.MRG's sectors on the disc image, by sector offset in the file."""

    def __init__(self, path: Path):
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import model_oracle
        self.disc = model_oracle.Disc(path)

    def module_word(self, sector_offset: int) -> int:
        return struct.unpack_from("<I", self.disc._sector(self.disc.lba + sector_offset), 0)[0]

    def references(self, sector_offset: int, base: int, ranges: list[tuple[int, int]], sectors: int = 10) -> bool:
        """Whether the module image at `sector_offset` (loaded at `base`) can
        reach an address of `ranges` ([low, high) each) from outside them: a
        j or jal into one, a word holding one (any segment), or an addiu/ori
        whose immediate is one's low half (the second half of a lui pair, how
        MIPS code builds a pointer to call)."""
        image = b"".join(self.disc._sector(self.disc.lba + sector_offset + n) for n in range(sectors))

        def inside(address: int) -> bool:
            return any(low <= address < high for low, high in ranges)

        lows = {address & 0xFFFF for low, high in ranges for address in range(low, high, 4)}
        for at, word in enumerate(struct.unpack_from("<%dI" % (len(image) // 4), image)):
            if inside(base + 4 * at):
                continue
            if (word >> 26) in (0x02, 0x03) and inside((base & 0xF0000000) | ((word & 0x3FFFFFF) << 2)):
                return True
            if inside((word & 0x1FFFFFFF) | (base & 0xE0000000)):
                return True
            if (word >> 26) in (0x09, 0x0D) and word & 0xFFFF in lows:
                return True
        return False


def shared_layout(options: list[dict], disc) -> dict | None:
    """Rule 7: a slot whose images have several layouts takes the shortest when
    every other is it plus `c` files after its data that no image of theirs
    can reach from outside them, at any address of theirs
    (MergeDisc.references): code the linker left behind, which the shared C
    stands in for (shape 402's variant552_strand in one slot B image,
    668's variant685_rings in one slot A image). None otherwise."""
    shortest = min(options, key=lambda option: len(option["files"]))
    count = len(shortest["files"])
    for option in options:
        if (option["files"][:count] != shortest["files"] or option["starts"][:count] != shortest["starts"]
                or option["data_start"] != shortest["data_start"]):
            return None
        ranges = [(option["base"] + start, option["base"] + end)
                  for start, end in zip(option["starts"][count:], option["ends"][count:])]
        if any(low < option["base"] + option["data_start"] for low, _ in ranges):
            return None   # extra code before the data: not left behind
        if ranges and any(disc.references(sector, option["base"], ranges) for sector in option["sectors"]):
            return None
    merged = dict(shortest)
    merged["modules"] = [name for option in options for name in option["modules"]]
    return merged


# --- copying -------------------------------------------------------------------------

def overlay_folder(path: str) -> str | None:
    """The overlay folder of a path under src/overlays, else None."""
    parts = PurePosixPath(path).parts
    return parts[2] if len(parts) > 3 and parts[:2] == ("src", "overlays") else None


def resolve(upstream: Upstream, including: str, name: str) -> str:
    """Where an #include lands in the upstream tree: next to the file, as the
    preprocessor looks first, else from src/ (-Isrc)."""
    beside = posixpath.normpath(posixpath.join(posixpath.dirname(including), name))
    if upstream.read(beside) is not None:
        return beside
    from_src = posixpath.normpath("src/" + name)
    return from_src if upstream.read(from_src) is not None else beside


def port_path(path: str, slot1: bool = False) -> str:
    """Where an upstream file goes in the port (rule 1; rule 3 for slot B's)."""
    folder = overlay_folder(path)
    if folder is None:
        return path
    return f"{NATIVE}/{folder}{'_slot1' if slot1 else ''}/{PurePosixPath(path).name}"


def rewrite_includes(upstream: Upstream, path: str, text: str, slot1: bool) -> str:
    """Rules 2 and 3: an include that lands in an overlay folder, written
    relative to the file's place in the port."""
    here = posixpath.dirname(port_path(path, slot1))

    def fix(match):
        name = match.group(2)
        target = resolve(upstream, path, name)
        if overlay_folder(target) is None:
            return match.group(0)
        relative = posixpath.relpath(port_path(target), here)
        if relative == name:
            return match.group(0)
        return match.group(1) + relative + match.group(3)

    return INCLUDE.sub(fix, text)


def closure(upstream: Upstream, paths: list[str], root: Path = ROOT) -> list[str]:
    """The files, and every file they include, transitively (upstream paths).
    A file outside the overlay folders that the port has is the port's: its
    includes are the port's own, not followed here."""
    seen, todo = [], list(paths)
    while todo:
        path = todo.pop(0)
        if path in seen:
            continue
        seen.append(path)
        data = upstream.read(path)
        if data is None or overlay_folder(path) is None and (root / path).exists():
            continue
        for match in INCLUDE.finditer(data.decode("utf-8")):
            todo.append(resolve(upstream, path, match.group(2)))
    return seen


class Plan:
    """What porting a shape writes: {port path: text}, the module file lists
    and the headers the port gains."""

    def __init__(self):
        self.files: dict[str, str] = {}
        self.lists: dict[str, list[str]] = {}
        self.headers: list[str] = []
        self.refused: list[str] = []


def plan_shape(upstream: Upstream, shape: int, found: dict[str, list[dict]], plan: Plan,
               root: Path = ROOT, disc=None) -> dict | None:
    """Add a shape's files to the plan. Returns {"data_start", "files": {slot: [...]}} or None (refused)."""
    lists = {}
    for slot in "AB":
        options = found.get(slot, [])
        if len(options) > 1 and disc is not None:
            shared = shared_layout(options, disc)
            options = [shared] if shared else options
        if len(options) != 1:
            plan.refused.append(f"shape {shape}: slot {slot} has {len(options)} layouts: "
                                + "; ".join(f"{', '.join(PurePosixPath(p).name for p in o['files'])} "
                                            f"({len(o['modules'])} images)" for o in options))
            return None
        lists[slot] = options[0]
    if lists["A"]["data_start"] != lists["B"]["data_start"]:
        plan.refused.append(f"shape {shape}: data starts 0x{lists['A']['data_start']:X} (A) and "
                            f"0x{lists['B']['data_start']:X} (B)")
        return None
    module_files = {}
    for slot in "AB":
        tops = ["src/" + stem + ".c" for stem in lists[slot]["files"]]
        module_files[slot] = []
        for top in tops:
            module_files[slot].append(port_path(top, slot1=(slot == "B")))
        for path in closure(upstream, tops, root):
            data = upstream.read(path)
            if data is None:
                plan.refused.append(f"shape {shape}: {path} is not in upstream")
                return None
            slot1 = slot == "B" and path in tops
            target = port_path(path, slot1)
            if overlay_folder(path) is None:
                if not (root / path).exists() and target not in plan.files:
                    plan.files[target] = data.decode("utf-8")
                    plan.headers.append(target)
                continue
            text = rewrite_includes(upstream, path, data.decode("utf-8"), slot1)
            if plan.files.setdefault(target, text) != text:
                plan.refused.append(f"shape {shape}: {target} would be written two ways")
                return None
    return {"data_start": lists["A"]["data_start"], "files": module_files,
            "images": {slot: sum(len(o["modules"]) for o in found.get(slot, [])) for slot in "AB"}}


# --- the module file lists -----------------------------------------------------------------

def read_sections(path: Path) -> dict[str, list[str]]:
    sections: dict[str, list[str]] = {}
    current = None
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("["):
                current = sections.setdefault(line[1:-1], [])
            elif current is not None:
                current.append(line)
    return sections


SOURCES_HEADER = ("# The C files of each native variant module (tools/pc/model_gate.py port writes this from\n"
                  "# the upstream layouts; tools/pc/build_config.py compiles them). Paths from the repository root.\n")
NAMES_HEADER = ("# Upstream's names for resident functions the ported model modules call, which the port names\n"
                "# otherwise, with their address (tools/pc/model_gate.py port, from upstream's symbol files).\n"
                "# tools/pc/build_game32.py resolves them to the port's name at that address.\n")


def write_sections(path: Path, header: str, sections: dict[str, list[str]]) -> None:
    def order(name):
        digits = re.findall(r"\d+", name)
        return (int(digits[0]) if digits else 0, name)
    lines = [header]
    for name in sorted(sections, key=order):
        lines.append(f"[{name}]\n")
        lines.extend(f"{entry}\n" for entry in sections[name])
    path.write_text("".join(lines), encoding="utf-8", newline="\n")


def read_names(path: Path = NAMES) -> dict[str, int]:
    names = {}
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            parts = line.split()
            if len(parts) == 2 and not parts[0].startswith("#"):
                names[parts[0]] = int(parts[1], 16)
    return names


def upstream_absolute_names(upstream: Upstream) -> dict[str, int]:
    """Every resident name upstream gives an address: its function list and
    its model modules' symbol files."""
    modules = json.loads(upstream.text("config/slus_01411/overlays.json"))["modules"]
    found = {}
    for line in upstream.text("config/slus_01411/functions.csv").splitlines()[1:]:
        parts = line.split(",")
        if len(parts) > 2 and parts[0].startswith("0x"):
            found.setdefault(parts[2], int(parts[0], 16))
    for module in modules:
        if not module["archive"].upper().endswith("MODEL.MRG") or not module["name"].startswith("model_"):
            continue
        text = upstream.read(module["layout"].replace(".yaml", "_symbols.txt"))
        if text is None:
            continue
        for match in re.finditer(r"^(\w+) = (0x[0-9A-Fa-f]+);.*absolute:true", text.decode(), re.M):
            found.setdefault(match.group(1), int(match.group(2), 16))
    return found


# --- writing ---------------------------------------------------------------------------

def write_plan(plan: Plan, root: Path = ROOT) -> None:
    """The files, LF line ends (the repository's; Git checks them out as it is set to).
    Rule 6 runs over every file before any is written, so a refusal leaves
    the tree as it was."""
    texts = {path: stale_local_text(path, text) for path, text in plan.files.items()}
    for path, text in texts.items():
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8", newline="\n")


# Rule 6: {file: ((the local's declaration, what takes its place, the name of
# a struct local whose uses become a pointer's, and any (old, new) rewrites of
# uses that reach the local through another's address), ...)}. Shape 169's trail
# aims its ratan2 at vertices[0].vy/.vz, which only the waiting branch writes
# (retail images wait 65 frames first). Shapes 89, 137 and 163 submit `quad` in
# frames that do not write its colour, so the packet carries the colour of the
# last frame that did. Shape 102 reads depths[109] and flags[109], one past
# what its RotTransPersN writes. Shape 831's halo reads `points`, which only
# the gathering frames write. Shape 769 and image31368_entry.c (shape 772's
# body, and the French header 789's) submit `quad` after the burst without
# writing its UVs, and their spray reads screen[0], which only the gathering
# frames and the burst's first frames write. The oracle matches the
# interpreter with the slot's last value (notes/pc-build.md).
def _stale_quad(name: str) -> tuple[str, str, str]:
    return ("    POLY_FT4 quad;\n",
            f"    POLY_FT4 *quad = &{name}Quad[Model_GetActiveSlotIndex() & 1];\n", "quad")


def _stale_screen(name: str) -> tuple[str, str, None]:
    return ("    DVECTOR screen[128];\n",
            f"    DVECTOR *screen = {name}Screen[Model_GetActiveSlotIndex() & 1];\n", None)


STALE_LOCALS = {
    f"{NATIVE}/model_variant/variant169_entry.c": ((
        "    SVECTOR vertices[4];\n",
        "    SVECTOR *vertices = gModelVariant169Vertices[Model_GetActiveSlotIndex() & 1];\n", None),),
    **{f"{NATIVE}/model_variant/variant{shape}_entry.c": (_stale_quad(f"gModelVariant{shape}"),)
       for shape in (89, 137, 163)},
    f"{NATIVE}/model_variant/variant102_entry.c": ((
        "    u16 depths[110], projection[110], flags[110];\n",
        "    u16 *depths = gModelVariant102Depths[Model_GetActiveSlotIndex() & 1], projection[110],"
        " *flags = gModelVariant102Flags[Model_GetActiveSlotIndex() & 1];\n", None),),
    f"{NATIVE}/model_variant/variant831_entry.c": ((
        "    SVECTOR points[2];\n",
        "    SVECTOR *points = gModelVariant831Points[Model_GetActiveSlotIndex() & 1];\n", None),),
    f"{NATIVE}/model_variant/variant769_entry.c": (
        _stale_quad("gModelVariant769"), _stale_screen("gModelVariant769")),
    f"{NATIVE}/french_model_variant/image31368_entry.c": (
        _stale_quad("gModelImage31368"), _stale_screen("gModelImage31368")),
    f"{NATIVE}/french_model_variant/variant101_entry.c": ((
        "    u16 flags[96];\n",
        "    u16 *flags = gModelFrenchVariant101Flags[Model_GetActiveSlotIndex() & 1];\n", None),),
    f"{NATIVE}/model_variant/variant783_entry.c": (
        _stale_quad("gModelVariant783"), _stale_screen("gModelVariant783")),
    # Shape 448's fan never names `col`: it reaches it as (s32)&rot + 240..331,
    # where the MIPS frame keeps it, which native C's frame does not. The
    # storage takes it, and the uses address it from there: (s32)&rot + 240
    # is col. A game unit's storage has a 32-bit address, as the casts need.
    f"{NATIVE}/model_variant/variant448_fan.c": ((
        "    u8 col[6][16];\n",
        "    u8 (*col)[16] = gModelVariant448Col[Model_GetActiveSlotIndex() & 1];\n", None,
        ("(s32)&rot", "((s32)col - 240)")),),
}
STALE_INCLUDE = '#include "pc/overlays/model_stale_locals.h"\n'


def stale_local_text(path: str, text: str) -> str:
    """Rule 6 on a copied file's text: each declaration once, replaced, a
    struct local's uses made a pointer's, and the storage's header included
    after the file's first include."""
    rules = STALE_LOCALS.get(path)
    if rules is None:
        return text
    for declaration, replacement, pointer, *uses in rules:
        if text.count(declaration) != 1:
            raise SystemExit(f"model_gate port: rule 6: {path} does not declare `{declaration.strip()}` once")
        text = text.replace(declaration, replacement)
        if pointer is not None:
            text = re.sub(rf"&{pointer}\b(?!\.)", pointer, text)
            text = re.sub(rf"(?<![\w.>]){pointer}\.", f"{pointer}->", text)
        # A local the code reaches only through another's address (shape
        # 448's col, as (s32)&rot + 240): its uses, written from the storage.
        for old, new in uses:
            if old not in text:
                raise SystemExit(f"model_gate port: rule 6: {path} has no `{old}`")
            text = text.replace(old, new)
    first = text.find("#include")
    end = text.index("\n", first) + 1 if first >= 0 else 0
    return text[:end] + STALE_INCLUDE + text[end:]


def stale_local_problems(listed: dict[str, list[str]], native_modules: set[str], root: Path = ROOT) -> list[str]:
    """`model_gate.py check`: a file of a native unit (the unit or one it
    includes, as variant772_entry.c includes image31368_entry.c) that rule 6
    names but whose text lacks it (a copy made without the rule reads the
    host's stack)."""
    from model_gate import port_closure
    bad, seen = [], set()
    for module in sorted(native_modules):
        for unit in listed.get(module, []):
            if not (root / unit).is_file():   # a missing file: check_sources says so
                continue
            for one in port_closure(root / unit, root):
                path = Path(os.path.relpath(one, root)).as_posix()
                rules = STALE_LOCALS.get(path)
                if rules is None or path in seen:
                    continue
                seen.add(path)
                text = (root / path).read_text(encoding="utf-8").replace("\r\n", "\n")
                for declaration, replacement, _, *uses in rules:
                    if replacement not in text or STALE_INCLUDE not in text:
                        bad.append(f"{path}: `{declaration.strip()}` is read before the call writes it, and the C "
                                   f"lacks rule 6's storage (model_gate_port.py STALE_LOCALS): port the shape again")
                    for old, _new in uses:
                        if old in text:
                            bad.append(f"{path}: `{old}` is still there, which rule 6 rewrites "
                                       f"(model_gate_port.py STALE_LOCALS): port the shape again")
    return bad


def cast_pass(units: list[str], jobs: int) -> list[str]:
    """Rule 4: G32 on the casts check_x64_casts.py finds in the copied files.
    Returns what it could not fix (and casts it found outside the copies)."""
    import concurrent.futures
    saved = list(sys.argv)
    try:
        import check_x64_casts
    finally:
        sys.argv = saved
    sites, unreadable = {}, []
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for unit, found, error in pool.map(check_x64_casts.scan, units):
            if error is not None:
                unreadable.append(f"{unit}: could not parse: {' '.join(error)}")
            for site in found:
                site["path"] = posixpath.normpath(site["path"])   # a slot B wrapper's view of slot A's C
                sites.setdefault((site["path"], site["offset"], site["line"], site["col"]), site)
    ours = [site for site in sites.values() if site["path"].startswith(NATIVE + "/")]
    others = [site for site in sites.values() if not site["path"].startswith(NATIVE + "/")]
    # A `widened` cast (`(T *)(i + sizeof(x))`) takes G32 too, the remedy
    # check_x64_casts.py names for it, which its --fix leaves to the author.
    for site in ours:
        if "widened" in site["kinds"]:
            site["kinds"] = ["signed" if kind == "widened" else kind for kind in site["kinds"]]
    left = check_x64_casts.fix(ours) if ours else []
    report = [f"{site['path']}:{site['line']}:{site['col']}: {'/'.join(site['kinds'])} cast to {site['type']} "
              f"(not fixed by rule)" for site in left]
    report += [f"{site['path']}:{site['line']}:{site['col']}: {'/'.join(site['kinds'])} cast to {site['type']} "
               f"(outside src/native_overlays)" for site in others]
    print(f"model_gate port: G32 on {len(ours) - len(left)} cast(s) in {len(units)} unit(s)")
    return report + unreadable


def port(shapes: list[int], disc_path: Path, revision: str, jobs: int, root: Path = ROOT, kind=None) -> int:
    """Copy the shapes' C and slot B wrappers, list their files, apply the
    cast rule over every listed unit, and give the table rows their data
    start and sources. Backend, span and command_table are not set here."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import model_gate
    kind = kind or model_gate.VARIANT
    upstream = Upstream(revision, root)
    disc = MergeDisc(disc_path)
    found = layouts(upstream, disc, kind)
    plan = Plan()
    ported = {}
    pairs = {row.shape: row.identifier("B") for row in model_gate.load_table(kind.table, kind)}
    for shape in shapes:
        if shape not in found or shape not in pairs:
            plan.refused.append(f"shape {shape}: upstream has no US module with that first word, or the table no row")
            continue
        both = {"A": found[shape]["A"], "B": found.get(pairs[shape], {}).get("B", [])}
        result = plan_shape(upstream, shape, both, plan, root, disc)
        if result:
            ported[shape] = result
    for line in plan.refused:
        print("model_gate port: refused " + line, file=sys.stderr)
    write_plan(plan, root)
    sections = read_sections(SOURCES)
    rows = model_gate.load_table(kind.table, kind)
    by_shape = {row.shape: row for row in rows}
    for shape, result in ported.items():
        row = by_shape[shape]
        for slot in "AB":
            sections[row.module(slot)] = result["files"][slot]
        row["data_start"] = f"0x{result['data_start']:X}"
        headers = sorted({int(n) for path in result["files"]["A"]
                          for n in re.findall(r"(?:variant|image)(\d+)", PurePosixPath(path).name)[:1]})
        row["sources"] = "" if headers == [shape] or kind.name == "primary" else ";".join(str(n) for n in headers)
    write_sections(SOURCES, SOURCES_HEADER, sections)
    model_gate.write_table(rows, kind.table)
    names = resident_names(upstream, [path for files in sections.values() for path in files], root)
    write_sections_flat(NAMES, NAMES_HEADER, names)
    units = sorted({path for files in sections.values() for path in files})
    given, errors = declaration_pass(upstream, units, jobs, root)
    print(f"model_gate port: {len(given)} file(s) include {NAMES_INCLUDE}")
    for line in errors:
        print("model_gate port: does not compile: " + line, file=sys.stderr)
    left = cast_pass(units, jobs)
    for line in left:
        print("model_gate port: " + line, file=sys.stderr)
    print(f"model_gate port: {len(ported)} shape(s) written from upstream {upstream.revision[:10]}, "
          f"{len(plan.files)} file(s), {len(plan.headers)} header(s) the port lacked "
          f"({', '.join(plan.headers) or 'none'}), {len(plan.refused)} refused; "
          f"{len(names)} resident name(s) in {NAMES.relative_to(root)}")
    return 1 if plan.refused or errors else 0


def write_sections_flat(path: Path, header: str, names: dict[str, int]) -> None:
    path.write_text(header + "".join(f"{name} {address:08X}\n" for name, address in sorted(names.items())),
                    encoding="utf-8", newline="\n")


IDENTIFIER = re.compile(r"\b[A-Za-z_]\w*\b")


def resident_names(upstream: Upstream, units: list[str], root: Path = ROOT) -> dict[str, int]:
    """The names upstream gives a resident address that the units' text uses
    and the port does not define under that name: the port knows the
    address by another name (build_game32.py aliases them)."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import model_gate
    known = set()
    for line in (root / "config/pc/guest_addresses.txt").read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if len(parts) == 2 and not line.startswith(("[", "#")):
            known.add(parts[0])
    used = set()
    for unit in units:
        for path in model_gate.port_closure(root / unit, root):
            if NATIVE not in path.as_posix():
                continue
            used.update(IDENTIFIER.findall(model_gate.without_comments(path.read_text(encoding="utf-8",
                                                                                        errors="replace"))))
    absolute = upstream_absolute_names(upstream)
    return {name: address for name, address in absolute.items()
            if name in used and name not in known and not re.fullmatch(r"func_8[0-9A-F]{7}", name)}


# --- declarations ------------------------------------------------------------------------

NAMES_INCLUDE = f"{NATIVE}/model_gate_names.h"
CHECK_CC = "i686-w64-mingw32-clang" if sys.platform == "win32" else "gcc"
CHECK_FLAGS = ["-std=gnu11", "-fsyntax-only", "-O0", "-fno-strict-aliasing", "-fwrapv", "-fcommon", "-DMEMORIES_PC",
               "-D_LANGUAGE_C", "-DLANGUAGE_C", "-Isrc", "-include", "src/pc/compat/pgxp_game.h", "-w",
               "-Werror=implicit-function-declaration", "-Werror=implicit-int", "-Werror=int-conversion",
               "-Werror=return-type", "-ferror-limit=0"] + (["-mno-ms-bitfields"] if sys.platform == "win32" else ["-m32"])
UNDECLARED = re.compile(r"^(.+?):\d+:\d+: error: (?:call to undeclared function|implicit declaration of function) '(\w+)'",
                        re.M)
ERROR = re.compile(r"^(.+?):(\d+):\d+: error: (.*)$", re.M)


def compile_errors(units: list[str], jobs: int, root: Path = ROOT) -> dict[str, str]:
    """{unit: the compiler's errors} for the units that do not compile with
    the game's flags and the warnings that hide a wrong call made errors (the
    build's -w hides an undeclared function, which then returns int)."""
    import concurrent.futures

    def one(unit):
        result = subprocess.run([CHECK_CC, *CHECK_FLAGS, unit], cwd=root, capture_output=True, text=True)
        return unit, result.returncode, result.stderr

    failed = {}
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for unit, code, errors in pool.map(one, units):
            if code:
                failed[unit] = errors
    return failed


def port_declarations(root: Path = ROOT) -> dict[str, list[str]]:
    """{function name: [headers]} of the prototypes in the port's own headers."""
    found = collections.defaultdict(list)
    prototype = re.compile(r"^[^#\n/*]*?\b(\w+)\s*\([^;{]*\)\s*(?:asm\s*\([^)]*\)\s*)?;", re.M)
    for folder in ("src/game", "src/psyq"):
        for path in sorted((root / folder).glob("*.h")):
            text = COMMENTS.sub(" ", path.read_text(encoding="utf-8", errors="replace"))
            for name in prototype.findall(text):
                found[name].append(path.relative_to(root).as_posix())
    return found


COMMENTS = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
NAMES_TEXT = ("/* Written by tools/pc/model_gate.py port: do not edit. The resident functions the\n"
              " * ported model modules call where the port's headers do not declare them under the\n"
              " * name the decomp's C uses: the port's header that declares the function, and for a\n"
              " * function the port names otherwise (same address, upstream's symbol files) the\n"
              " * decomp's name as the port's. A copied file that needs it includes it first. */\n")


def declaration_pass(upstream: Upstream, units: list[str], jobs: int, root: Path = ROOT) -> tuple[list[str], list[str]]:
    """Rule 5: a copied file that calls a resident function its headers do
    not declare (the port's header of that name declares it under the port's
    name, or not at all) includes src/native_overlays/model_gate_names.h
    first, which declares it. Returns (files given the include, errors left)."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import csv as csv_module
    with (root / "config/slus_01411/functions.csv").open(newline="") as handle:
        port_by_address = {int(row["address"], 16): row["name"] for row in csv_module.DictReader(handle)}
    absolute = upstream_absolute_names(upstream)
    declared = port_declarations(root)
    header = root / NAMES_INCLUDE
    current = header.read_text(encoding="utf-8") if header.exists() else NAMES_TEXT
    includes = set(re.findall(r'^#include "([^"]+)"', current, re.M))
    defines = dict(re.findall(r"^#define (\w+) (\w+)$", current, re.M))
    given, left = [], []
    for attempt in range(3):
        failed = compile_errors(units, jobs, root)
        wanted = collections.defaultdict(set)
        for unit, errors in failed.items():
            for path, name in UNDECLARED.findall(errors):
                wanted[name].add(posixpath.normpath(path.replace("\\", "/")))
        if not wanted:
            break
        progress = False
        for name, paths in sorted(wanted.items()):
            port_name = name
            if name not in declared and name in absolute:
                port_name = port_by_address.get(absolute[name], name)
            homes = declared.get(port_name)
            if not homes:
                continue
            home = posixpath.relpath(homes[0], NATIVE)
            if home not in includes or (port_name != name and defines.get(name) != port_name):
                includes.add(home)
                if port_name != name:
                    defines[name] = port_name
                progress = True
            for path in paths:
                if not path.startswith(NATIVE + "/"):
                    continue
                target = root / path
                text = target.read_text(encoding="utf-8")
                line = f'#include "{posixpath.relpath(NAMES_INCLUDE, posixpath.dirname(path))}"\n'
                if line not in text:
                    target.write_text(line + text, encoding="utf-8", newline="\n")
                    given.append(path)
                    progress = True
        body = NAMES_TEXT + "#ifndef MEMORIES_MODEL_GATE_NAMES_H\n#define MEMORIES_MODEL_GATE_NAMES_H\n"
        body += "".join(f'#include "{one}"\n' for one in sorted(includes))
        body += "".join(f"#define {old} {new}\n" for old, new in sorted(defines.items()))
        body += "#endif\n"
        header.write_text(body, encoding="utf-8", newline="\n")
        if not progress:
            break
    failed = compile_errors(units, jobs, root)
    for unit, errors in sorted(failed.items()):
        for path, line, message in ERROR.findall(errors):
            left.append(f"{posixpath.normpath(path)}:{line}: {message}")
    return sorted(set(given)), sorted(set(left))
