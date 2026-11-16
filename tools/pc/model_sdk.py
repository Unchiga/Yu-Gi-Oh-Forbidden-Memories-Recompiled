#!/usr/bin/env python3
"""What the model-migration tool needs of the repository, to run from an SDK
(model_gate.py mod-migrate / mod-row / mod-check; notes/modding.md, "Moving a
mod's attacks from MIPS to C"), computed, not listed:

  tools    the tool's own Python: the local modules the entry points
           (ENTRY) import, anywhere in their source (a function's own import
           too), transitively, but not into what the SDK ships already
           (BUILT_IN: build_mod.py's) or into NOT_SHIPPED
  data     every file of the repository a shipped module names by a string
           literal ("config/pc/model_gate.csv", "src/game/model.h", ...)
  rows     the C of every native row (config/pc/model_gate.csv,
           model_primary_gate.csv; both slots): the units
           config/pc/model_gate_sources.txt names and every file of the
           repository they include

The files the span-word labels (config/pc/model_span_words.csv) were made from
are among these (digested(): the tool's files, the headers, each row's C):
`check` recomputes the digests in the SDK, which holds when none is missing.

Headers under src/ are the SDK's already (build_game32.write_sdk copies every
src/**/*.h but the Android stand-ins, and src/pc/mods/sdk goes to include/libc);
this module checks that every header it wants is among them and lists the rest.
The result is {the repository's name of a file: its source}, which write_sdk
copies to where model_layout.py puts it, and which the packaging test and
`model_gate.py mod-check` hold the SDK to: sdk/config/model_sdk.json carries
each file's SHA-256, and what the labels check found when the SDK was made
(told by `mod-check` and the report as a warning: stale labels block nothing).

Nothing here reads the disc."""
from __future__ import annotations

import ast
import functools
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import model_layout as layout  # noqa: E402

# The programs of the model tools: `model_gate.py mod-migrate`, which imports
# model_migrate, and the oracle it names in its report; `effect-migrate`'s
# effect_migrate and the comparison it names (test_duel_effects).
ENTRY = ("model_gate", "model_migrate", "model_oracle", "effect_migrate", "test_duel_effects")
# Shipped by write_sdk for every mod (and imported by name, not copied again).
BUILT_IN = ("build_mod", "build_process", "ptr32_stores")
# Modules the entry points import that the SDK does not need, and why:
#   smoke           yfm_control falls back to a launcher of its own without it
#   check_x64_casts model_gate_port.port, copying shapes from upstream (a repository's job)
NOT_SHIPPED = ("smoke", "check_x64_casts")
# Files the tools name that only the repository's own commands read (`model_gate.py
# check`, `update`, `write`, `port`: they refuse in an SDK), which the SDK leaves out:
#   config/pc/guest_addresses.txt  `check` holds the address sections to the C; yfm_control reads it only
#                                  when it is there, for a game too old to be asked for its variables' names
#   the three .inc tables          the game's own build headers, written from the CSVs by `write`
REPO_ONLY = ("config/pc/guest_addresses.txt", "src/pc/overlays/model_gate_table.inc",
             "src/pc/overlays/model_primary_gate_table.inc", "src/pc/overlays/model_retail_table.inc")
# Where the SDK keeps its stamp (the repository's name, model_layout.py maps it).
STAMP = "config/pc/model_sdk.json"
# What the repository calls the tools' own folder, for string literals.
PREFIXES = ("config/pc/", "src/", "tools/pc/")
# The SDK's copy of a header comes from build_game32.write_sdk, with these exceptions.
ANDROID_STAND_INS = "src/pc/compat/android/"
LIBC_HEADERS = "src/pc/mods/sdk/"


def repository_root() -> Path:
    if layout.SHIPPED:
        raise SystemExit("model_sdk: the repository's files are computed from a checkout, not an SDK")
    return layout.ROOT


def local_imports(path: Path, modules: set[str]) -> set[str]:
    """The names of `modules` a Python file imports, wherever in it."""
    tree = ast.parse(path.read_text(encoding="utf-8"))
    found = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            found |= {alias.name.split(".")[0] for alias in node.names}
        elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
            found.add(node.module.split(".")[0])
    return found & modules


def tool_modules() -> list[str]:
    """The tool's Python files, by name (`model_gate.py`), sorted: ENTRY and
    what it imports, without BUILT_IN and NOT_SHIPPED."""
    folder = repository_root() / "tools/pc"
    modules = {path.stem for path in folder.glob("*.py")}
    todo, seen = list(ENTRY), set()
    while todo:
        name = todo.pop()
        if name in seen or name in BUILT_IN or name in NOT_SHIPPED:
            continue
        seen.add(name)
        todo += local_imports(folder / f"{name}.py", modules)
    return sorted(f"{name}.py" for name in seen)


def string_literals(path: Path) -> set[str]:
    """The strings of a Python file, without its docstrings (prose that names
    files is not a file the program opens)."""
    tree = ast.parse(path.read_text(encoding="utf-8"))
    docs = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)) and node.body:
            first = node.body[0]
            if isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant):
                docs.add(id(first.value))
    return {node.value for node in ast.walk(tree)
            if isinstance(node, ast.Constant) and isinstance(node.value, str) and id(node) not in docs}


def named_files(modules: list[str]) -> set[str]:
    """The repository's files the modules name in a string: a literal that is
    the name of a file of the checkout ("config/pc/model_gate.csv"; a
    pattern, a folder, a sentence or a name of something the tool writes is
    not)."""
    root = repository_root()
    return {text for module in modules for text in string_literals(root / "tools/pc" / module)
            if text.startswith(PREFIXES) and text not in REPO_ONLY and (root / text).is_file()}


def native_rows() -> list:
    import model_gate
    return model_gate.native_rows() + model_gate.native_primary_rows()


@functools.lru_cache(maxsize=None)
def row_sources(root: Path | None = None) -> frozenset[str]:
    """The repository's names of the C units of every native row, both slots,
    and every file of the repository they include: what mod-migrate copies
    into a mod, and what the span-word digests are made of. Works on a
    checkout and on an SDK alike (model_layout.py)."""
    import model_gate
    found = set()
    for row in native_rows():
        for slot in "AB":
            for pattern in model_gate.source_patterns(row, slot):
                for unit in sorted(layout.glob(pattern, root)):
                    for one in model_gate.port_closure(unit):
                        found.add(layout.logical(one))
    return frozenset(found)


def digested() -> set[str]:
    """The files model_span_words.py digests (tool files and shared headers,
    and each row's own C), by the repository's names."""
    import model_span_words
    names = {layout.logical(path) for path in model_span_words.shared_files()}
    names |= {name for name in model_span_words.TOOL_FILES}
    for row in model_span_words.all_rows():
        names |= {layout.logical(path) for path in model_span_words.row_files(row)}
    return names


def shipped_header(name: str) -> bool:
    """Whether write_sdk copies a header of the checkout (src/**/*.h) as it is
    to include/<same name>."""
    return name.startswith("src/") and name.endswith(".h") and not name.startswith(ANDROID_STAND_INS) \
        and not name.startswith(LIBC_HEADERS)


def wanted() -> dict[str, str]:
    """{the repository's name of a file: "tool", "data" or "row"} for every
    file the model tools need, headers included. Raises SystemExit naming what
    is wrong when a file the tools need is one write_sdk would not carry."""
    modules = tool_modules()
    wanted: dict[str, str] = {f"tools/pc/{name}": "tool" for name in modules}
    for kind, names in (("data", named_files(modules)), ("row", row_sources())):
        for name in sorted(names):
            wanted.setdefault(name, kind)
    root = repository_root()
    missing = [name for name in wanted if not (root / name).is_file()]
    if missing:
        raise SystemExit("model_sdk: the tools need files the checkout lacks: " + ", ".join(sorted(missing)[:8]))
    lost = [name for name in wanted if name.startswith(("src/pc/compat/android/", LIBC_HEADERS))]
    if lost:
        raise SystemExit("model_sdk: the tools need headers the SDK does not carry as they are: " + ", ".join(lost[:8]))
    return dict(sorted(wanted.items()))


def files() -> dict[str, str]:
    """wanted() less the headers: what install copies, beyond what write_sdk
    copies for every mod (src/**/*.h)."""
    return {name: kind for name, kind in wanted().items() if not shipped_header(name)}


def stamped() -> list[str]:
    """Every file the tools depend on, headers included, by the repository's
    names: what the stamp holds each to (a header altered after the SDK was
    made changes what mod-migrate copies into a mod and what its labels
    were made from). wanted(), what the digests are made of (digested()) and
    src/psyq/*.h, which the generated header takes the game's prototypes from."""
    names = set(wanted()) | digested()
    names |= {layout.logical(path) for path in (layout.SRC / "psyq").glob("*.h")}
    return sorted(name for name in names if not name.startswith(LIBC_HEADERS))

def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


# --- the SDK: writing it, and holding it to what it was written as -----------------------

def labels_problems() -> list[str]:
    """Why the span-word labels (config/pc/model_span_words.csv) do not belong
    to the tree: the tool digest, or a row's C digest, is not the table's.
    mod-migrate reports the labels as stale then. Labels describe and gate
    nothing, and only Windows with clang can regenerate them, so this is told
    (build_game32.py and package.py warn, the SDK's stamp records it) and
    never stops a build or a release."""
    import model_span_words
    return model_span_words.tool_causes() + model_span_words.stale_causes()


def install(sdk: Path, copy=None) -> dict:
    """Write the tools' files into an SDK folder (build_game32.write_sdk's
    <build>/sdk) where model_layout.py puts them, remove what an earlier
    version of this list put there and this one does not, and write the stamp
    (STAMP: each file's SHA-256, how many native rows, and the result of
    labels_problems() now). Returns the stamp. `copy(source, destination)`
    is build_game32's copy_if_changed."""
    import shutil
    root = repository_root()

    def put(source: Path, destination: Path) -> None:
        if copy:
            copy(str(source), str(destination))
        else:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)

    stamp_file = sdk / "config" / "model_sdk.json"
    inputs_file = sdk.parent / "model_sdk_inputs.json"   # beside the SDK, not in it: mtimes of this checkout
    inside = lambda name: sdk / shipped_name(name)   # noqa: E731
    try:
        earlier_stamp = json.loads(stamp_file.read_text(encoding="utf-8"))
        earlier_inputs = json.loads(inputs_file.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        earlier_stamp = earlier_inputs = None
    # Nothing to do when no file the stamp names, and no folder that could hold
    # another, has changed (size and time of the checkout's files) and the SDK
    # still has each of them: what a rebuild of nothing costs is a few thousand stats.
    if earlier_stamp and earlier_inputs and earlier_inputs == fingerprint(root, earlier_stamp["files"]) and all(
            inside(name).is_file() and inside(name).stat().st_size == (root / name).stat().st_size
            for name in earlier_stamp["files"]):
        return earlier_stamp
    started = time.time_ns()
    names = files()
    every = stamped()
    before = fingerprint(root, every)   # before the hashes and the labels are taken, and again after (below)
    earlier = earlier_stamp["files"] if earlier_stamp else {}
    for name in sorted(set(earlier) - set(every)):
        if not shipped_header(name) and inside(name).is_file():   # (headers are write_sdk's)
            inside(name).unlink()
    for name in names:
        put(root / name, inside(name))
    stamp = {"files": {name: sha256(root / name) for name in every},
             "native_rows": len(native_rows()),
             "labels": {"problems": labels_problems(), "table": sha256(root / "config/pc/model_span_words.csv")}}
    stamp_file.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(stamp, indent=1, sort_keys=True) + "\n"
    if not stamp_file.is_file() or stamp_file.read_text(encoding="utf-8") != text:
        stamp_file.write_text(text, encoding="utf-8", newline="\n")
    # What was read must be what is there now, and not a file written in the last moments (a time that
    # is this close to the start says nothing a clock of two seconds could tell from an edit in the
    # middle of the step): else the next build does the step again.
    after = fingerprint(root, every)
    newest = max([value[1] for value in after.values() if isinstance(value, list)]
                 + [value for value in after.values() if isinstance(value, int)])
    if before == after and newest < started - 2_000_000_000:
        inputs_file.write_text(json.dumps(after, sort_keys=True), encoding="utf-8")
    else:
        inputs_file.unlink(missing_ok=True)
    return stamp


# The folders whose entries decide what the tools find: a new C file or Python
# module appears as a change of the folder. src/native_overlays is watched down
# to its last folder, the others (here) at their top; and the folder of every
# file the stamp names (a quoted include is looked for beside the includer
# first, then from src/, and new names appear in the folder it ends in), src/
# itself (a new folder under it) and src/psyq, which stamped() globs.
WATCHED = ("src", "src/psyq", "tools/pc", "config/pc")


def fingerprint(root: Path, names) -> dict:
    """{name: [size, mtime_ns]} of each file, and {folder/: mtime_ns} of
    WATCHED and of every folder of src/native_overlays, as the checkout has
    them now: what install compares to know that nothing it reads changed."""
    found = {}
    for name in names:
        try:
            info = (root / name).stat()
            found[name] = [info.st_size, info.st_mtime_ns]
        except OSError:
            found[name] = None
    folders = set(WATCHED) | {PurePosixPath(name).parent.as_posix() for name in names}
    for directory, _, _ in os.walk(root / "src/native_overlays"):
        folders.add(Path(directory).relative_to(root).as_posix())
    for folder in folders:
        try:
            found[folder + "/"] = os.stat(root / folder).st_mtime_ns
        except OSError:
            found[folder + "/"] = None
    return found


def display(name: str) -> str:
    """The repository's name of a file as this tree's reports say it: the same
    in a checkout, include/..., config/... or tools/... in an SDK."""
    try:
        return shipped_name(name) if layout.SHIPPED else name
    except ValueError:
        return name


def shipped_name(name: str) -> str:
    """Where in an SDK the repository's file `name` goes (model_layout.FOLDERS)."""
    for folder, shipped in layout.FOLDERS.items():
        if name.startswith(folder + "/"):
            return shipped + name[len(folder):]
    raise ValueError(name)


def read_stamp() -> dict | None:
    """The stamp of this tree's SDK, or None in the repository (and in an SDK
    that has none)."""
    try:
        return json.loads(layout.path(STAMP).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def unresolved_includes() -> list[str]:
    """Quoted includes of the rows' C that name no file (beside the includer
    or under src/): port_closure skips them, so a file the SDK forgot would
    not be noticed by it."""
    import model_gate
    found = []
    for name in sorted(row_sources()):
        path = layout.path(name)
        if path.suffix not in (".c", ".h", ".inc"):
            continue
        for include in model_gate.INCLUDE.findall(model_gate._text(path)):
            if not ((path.parent / include).is_file() or (layout.SRC / include).is_file()):
                found.append(f"{display(name)}: #include \"{include}\"")
    return found


def check() -> list[str]:
    """What is wrong with the tools' files in this tree, without the disc: a
    file the stamp names that is missing or is not the stamped bytes, a native
    row with no C or an include with no file, a table that does not parse.
    Empty when the tree can run mod-migrate (stale labels are not a problem
    here: label_warnings). Works on a checkout and on an SDK."""
    import model_gate
    problems: list[str] = []
    stamp = read_stamp()
    if layout.SHIPPED:
        if stamp is None:
            return [f"{layout.path(STAMP)} is missing: this is not a whole SDK"]
        for name, digest in sorted(stamp["files"].items()):
            path = layout.path(name)
            if not path.is_file():
                problems.append(f"{display(name)} is missing from the SDK")
            elif sha256(path) != digest:
                problems.append(f"{display(name)} is not the file the SDK was made with")
    rows = native_rows()
    if stamp is not None and stamp["native_rows"] != len(rows):
        problems.append(f"the SDK was made with {stamp['native_rows']} native rows and its tables have {len(rows)}")
    problems += model_gate.problems(model_gate.load_table()) + model_gate.problems(model_gate.primary_rows())
    for row in rows:
        for slot in "AB":
            for pattern in model_gate.source_patterns(row, slot):
                if not layout.glob(pattern):
                    problems.append(f"{row.KIND.name} {row.shape} slot {slot}: no C matches {display(pattern)}")
    problems += [f"no file for {text}" for text in unresolved_includes()]
    if stamp is not None:
        # A file the tool would use (matched by a row's source pattern, or included from one) that the SDK
        # was not made with: an extra C file beside a module's, a header from elsewhere.
        problems += [f"{display(name)} is not a file the SDK was made with" for name in sorted(row_sources())
                     if name not in stamp["files"]]
    return problems


def label_warnings() -> list[str]:
    """Why mod-migrate calls the span-word labels stale here: what the stamp
    found when the SDK was made, and what the digests of this tree's files say
    now (a file changed after the SDK was made shows in both this and check()),
    each with what to do about it (model_span_words.remedy)."""
    import model_span_words
    stamp = read_stamp()
    if stamp and stamp.get("labels", {}).get("problems"):
        return [f"the labels were already stale when this SDK was made: {text}" for text in stamp["labels"]["problems"]]
    return [f"{text}; {model_span_words.remedy()}" for text in labels_problems()]
