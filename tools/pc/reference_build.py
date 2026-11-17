#!/usr/bin/env python3
"""The reference build: the game WITH the MIPS interpreter, for tests and the
oracle only (notes/pc-build.md, "The reference build").

The game every target ships has no MIPS interpreter (src/pc/guest/mips.c is
not compiled in). `tools/pc/build_game32.py --reference` builds the same tree
with it, into tmp/pc/reference/<build> (tmp/pc/reference/game32 for the
default build, .../win64 for --target windows-x64); it is never packaged
(--reference --release is refused). The tools that compare the port's C with
the disc's MIPS (model_oracle.py, test_duel_effects.py, the interpreter passes
of tests/pc/*_runtime.py) run the interpreter's side on it:

    from reference_build import reference_executable
    exe = reference_executable()          # built first when it is missing

In an SDK (a release's `sdk` folder) nothing is built: the reference is
whatever --reference-executable names, else the game beside the SDK if it
has the interpreter (the releases before it left the shipped game did); one without it is
refused with a message saying so. Where modders get a reference build is not
settled yet (notes/modding.md, "Moving a mod's attacks from MIPS to C").
"""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import model_layout as layout  # noqa: E402  (the repository's, or an SDK's)

ROOT = layout.ROOT
# What only the interpreter's code holds (its crash report's title): a game
# without it has no interpreter. test_package.py refuses a package whose
# executable has it.
MARKER = b"MIPS overlay"
EXE = "memories-pc.exe" if sys.platform == "win32" else "memories-pc"
# The default build folder of each target build_game32.py builds here, as
# its --build defaults (the folder's name is the reference's).
DEFAULT_BUILDS = {"windows": "game32" if sys.platform == "win32" else "win32", "linux": "game32",
                  "windows-x64": "win64"}
NO_REFERENCE = ("this game has no MIPS interpreter: the check needs a reference build, the game built with "
                "it (tools/pc/build_game32.py --reference, which is never packaged); name one with "
                "--reference-executable PATH")


def default_target() -> str:
    return os.environ.get("MEMORIES_TARGET") or ("windows" if sys.platform == "win32" else "linux")


def reference_folder(target: str | None = None) -> Path:
    """tmp/pc/reference/<build>: where build_game32.py --reference puts it."""
    target = target or default_target()
    return ROOT / "tmp/pc/reference" / DEFAULT_BUILDS.get(target, target)


def reference_path(target: str | None = None) -> Path:
    target = target or default_target()
    return reference_folder(target) / ("memories-pc.exe" if target.startswith("windows") else "memories-pc")


def has_interpreter(executable: Path) -> bool:
    """Whether a game has the MIPS interpreter (its bytes hold MARKER)."""
    try:
        with open(executable, "rb") as handle:
            data = handle.read()
    except OSError:
        return False
    return MARKER in data


def build_reference(target: str | None = None) -> Path:
    """Build the reference (build_game32.py --reference), in the foreground."""
    target = target or default_target()
    if layout.SHIPPED:
        raise SystemExit(f"reference_build: an SDK cannot build the reference; {NO_REFERENCE}")
    # Not one literal path: model_sdk.py would ship the file named so (this
    # module is in the SDK, which builds nothing).
    command = [sys.executable, str(ROOT / "tools" / "pc" / "build_game32.py"), "--reference", "--target", target]
    print(f"reference_build: building the reference ({' '.join(command[1:])})", flush=True)
    subprocess.run(command, cwd=ROOT, check=True)
    return reference_path(target)


def stale_note(reference: Path, subject: Path | None) -> str | None:
    """A line when the reference was built from other sources than the
    subject (their `source` stamps differ): rebuild it to compare like with like."""
    if subject is None:
        return None
    try:
        theirs = (reference.parent / "source").read_text().strip()
        ours = (Path(subject).parent / "source").read_text().strip()
    except OSError:
        return None
    if theirs != ours:
        return (f"reference_build: {reference} was built from other sources than {subject}; "
                f"rebuild it (tools/pc/build_game32.py --reference) to compare like with like")
    return None


def reference_executable(named: Path | None = None, *, subject: Path | None = None, target: str | None = None,
                         build: bool = True) -> Path:
    """The game the interpreter's side runs: `named` (a --reference-executable),
    else the reference build of this tree, built first when it is missing
    (`build`). Exits with a message when the game has no interpreter, or in
    an SDK when none is named."""
    if named is not None:
        executable = Path(named).resolve()
    elif layout.SHIPPED:
        # The game beside the SDK, if it is one of the releases that had the
        # interpreter; a game of this tree has none.
        from yfm_control import EXECUTABLE
        if not has_interpreter(EXECUTABLE):
            raise SystemExit(f"reference_build: {EXECUTABLE}: {NO_REFERENCE}")
        executable = EXECUTABLE
    else:
        executable = reference_path(target)
        if not executable.is_file():
            if not build:
                raise SystemExit(f"reference_build: no reference build at {executable}: "
                                 "tools/pc/build_game32.py --reference")
            build_reference(target)
    if not executable.is_file():
        raise SystemExit(f"reference_build: no game at {executable}")
    if not has_interpreter(executable):
        raise SystemExit(f"reference_build: {executable}: {NO_REFERENCE}")
    note = stale_note(executable, subject)
    if note:
        print(note, file=sys.stderr, flush=True)
    return executable


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Print the reference build's path, building it when it is missing.")
    parser.add_argument("--target", default=None)
    parser.add_argument("--no-build", action="store_true")
    options = parser.parse_args()
    print(reference_executable(target=options.target, build=not options.no_build))
