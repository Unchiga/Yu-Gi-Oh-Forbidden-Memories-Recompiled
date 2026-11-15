#!/usr/bin/env python3
"""Where the model tools' files are: in the repository, or in a mod SDK.

model_gate.py mod-migrate and its siblings (model_migrate.py, model_span_words.py,
model_gate_scan.py, model_gate_port.py, model_oracle.py, yfm_control.py) name
every file the way the repository does: src/..., config/pc/..., tools/pc/....
The SDK that ships with a release has another layout (the one build_mod.py
already knows: the headers in include/, the tools beside build_mod.py):

    repository                       SDK (<release>/sdk)
    src/<x>                          include/<x>      (the C of the native modules too)
    config/pc/<x>                    config/<x>
    tools/pc/<x>                     tools/<x>

path() turns the repository's name of a file into where it is here and
logical() turns it back, so what a tool prints and what it digests (a
digest carries a file's repository name: config/pc/model_span_words.csv was
made from src/native_overlays/..., whichever tree checks it) is the same in
both. In the repository both are the identity.

Which one this is, is decided as build_mod.py's SHIPPED is: the repository
has src/pc/mods/sdk. Nothing here reads the disc or starts a program.

The files the SDK needs of the repository, and the check that they are all
there, are in model_sdk.py."""
from __future__ import annotations

import os
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
SHIPPED = not (HERE.parents[1] / "src/pc/mods/sdk").is_dir()
# The SDK's folder (the one with build-mod.bat), or the repository's root.
ROOT = HERE.parent if SHIPPED else HERE.parents[1]
# The repository's name of a folder: where it is in an SDK.
FOLDERS = {"src": "include", "config/pc": "config", "tools/pc": "tools"}


def path(name: str, root: Path | None = None) -> Path:
    """Where the file or folder the repository calls `name` is: in `root`
    (a tree laid out as the repository, a test's) or in this tree."""
    if root is not None and Path(root) != ROOT:
        return Path(root) / name
    if SHIPPED:
        for folder, shipped in FOLDERS.items():
            if name == folder or name.startswith(folder + "/"):
                return ROOT / shipped / name[len(folder):].lstrip("/")
    return ROOT / name


def glob(pattern: str, root: Path | None = None) -> list[Path]:
    """The files of a pattern written the repository's way (`src/native_overlays/x/*.c`)."""
    if root is not None and Path(root) != ROOT:
        return list(Path(root).glob(pattern))
    if SHIPPED:
        for folder, shipped in FOLDERS.items():
            if pattern.startswith(folder + "/"):
                pattern = shipped + pattern[len(folder):]
                break
    return list(ROOT.glob(pattern))


def logical(file: Path | str) -> str:
    """The repository's name of a file of this tree (posix, relative), or its
    own path when it is outside the tree."""
    full = os.path.abspath(file)
    if not os.path.normcase(full).startswith(_PREFIX):   # (strings: Path.relative_to is slow, and this is called by the thousand)
        return Path(full).as_posix()
    inside = full[len(_PREFIX):].replace(os.sep, "/")
    if SHIPPED:
        for folder, shipped in FOLDERS.items():
            if inside == shipped or inside.startswith(shipped + "/"):
                return folder + inside[len(shipped):]
    return inside


def shown(file: Path | str) -> str:
    """A file as the tools' reports name it: the repository's name in a
    checkout (src/native_overlays/...), the SDK's own in an SDK
    (include/native_overlays/..., relative to the SDK's folder)."""
    full = os.path.abspath(file)
    if os.path.normcase(full).startswith(_PREFIX):
        return full[len(_PREFIX):].replace(os.sep, "/")
    return Path(full).as_posix()


def tolerant_output() -> None:
    """A path with a letter the console's code page lacks (an SDK in a folder
    named by its user) must not stop a report: what cannot be written is
    escaped."""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="backslashreplace")
        except (AttributeError, ValueError):
            pass


SRC = path("src")
_PREFIX = os.path.normcase(os.path.abspath(ROOT)) + os.sep
