#!/usr/bin/env python3
"""The model-migration tool (model_gate.py mod-migrate) in an SDK, without a
disc or a game (tools/pc/model_sdk.py, model_layout.py; notes/modding.md,
"Moving a mod's attacks from MIPS to C").

An SDK is written as a build writes it (build_game32.write_sdk on a build
folder with nothing in it but an export list), moved to a folder outside this
checkout, and the tool is run from there with nothing else on its path:

- it imports (model_gate, model_migrate, model_oracle, model_span_words,
  yfm_control), from the SDK and not the checkout;
- `mod-check` passes: the C of every native row and the files it includes are
  there and each file is the one the stamp names; the span-word labels'
  digests recompute from the SDK's own files to what the checkout's do;
- it fails, naming the file, when a file of a row's C is gone, when one is
  changed after the SDK was made, or when the stamp is gone; it only WARNS
  when the stamp says the labels were stale when the SDK was made (labels
  describe and gate nothing: mod-migrate's report calls them stale, nothing
  fails); the repository's commands (check, update, write, port) refuse;
- what is shipped is the repository's text and nothing from a disc: every file
  is tracked by git (the decomp's own source and tables), the tables hold no
  run of hexadecimal digits but a SHA-256, a 16-digit digest or a commit, and
  nothing is a disc image;
- the tool's files are the closure of its entry points (model_sdk.tool_modules),
  and the SDK carries every file model_sdk.files() names.

    python tools/pc/test_model_sdk.py
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import model_layout  # noqa: E402
import model_sdk  # noqa: E402

assert not model_layout.SHIPPED, "this test runs in a checkout"


def write_sdk(work: Path) -> Path:
    """<work>/release/sdk as a build's write_sdk writes it (a fresh build
    folder with an export list, which is all the rest of write_sdk needs)."""
    build = work / "build"
    build.mkdir()
    (build / "exports.i386.txt").write_text("rand\n", encoding="utf-8")
    code = ("import sys; sys.path.insert(0, 'tools/pc'); import build_game32; "
            "build_game32.write_sdk(sys.argv[1], 'i386')")
    result = child([sys.executable, "-c", code, str(build)], ROOT)
    if result.returncode:
        raise RuntimeError(f"write_sdk failed:\n{result.stdout}{result.stderr}")
    release = work / "release"
    release.mkdir()
    shutil.move(str(build / "sdk"), str(release / "sdk"))
    return release / "sdk"


def child(command: list[str], cwd: Path) -> subprocess.CompletedProcess:
    """A program of ours, whose text is UTF-8 whatever the console's code page
    (the folders this runs in may be named with any letter: ctest runs it under
    a temporary folder with accents, kanji and an emoji, test_path_layout.py),
    and which has no disc."""
    environment = {name: value for name, value in os.environ.items() if name != "MEMORIES_DISC"}
    environment["PYTHONIOENCODING"] = "utf-8"
    return subprocess.run(command, cwd=cwd, env=environment, capture_output=True, text=True, encoding="utf-8",
                          errors="replace")


def run(sdk: Path, *arguments: str, cwd: Path) -> subprocess.CompletedProcess:
    return child([sys.executable, str(sdk / "tools/model_gate.py"), *arguments], cwd)


class SdkTool(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory(prefix="model-sdk-")
        cls.work = Path(cls.folder.name).resolve()   # as the tool's own paths are (symlinks resolved)
        cls.sdk = write_sdk(cls.work)
        cls.elsewhere = cls.work / "elsewhere"
        cls.elsewhere.mkdir()
        cls.stamp = json.loads((cls.sdk / "config/model_sdk.json").read_text(encoding="utf-8"))

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def sdk_file(self, name: str) -> Path:
        return self.sdk / model_sdk.shipped_name(name)

    def test_modules_import_from_the_sdk(self):
        code = ("import sys; sys.path.insert(0, sys.argv[1]); import model_gate, model_migrate, model_oracle, "
                "model_span_words, yfm_control, model_sdk, model_layout; "
                "import json; print(json.dumps([model_layout.SHIPPED, model_layout.ROOT.as_posix(), "
                "model_migrate.SRC.as_posix(), yfm_control.EXECUTABLE.parent.as_posix()]))")
        result = child([sys.executable, "-c", code, str(self.sdk / "tools")], self.elsewhere)
        self.assertEqual(result.returncode, 0, result.stderr)
        # (JSON: a path may hold spaces and any letter)
        self.assertEqual(json.loads(result.stdout), [True, self.sdk.as_posix(), (self.sdk / "include").as_posix(),
                                                     self.sdk.parent.as_posix()])

    def test_mod_check_passes(self):
        result = run(self.sdk, "mod-check", cwd=self.elsewhere)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        rows = len(model_sdk.native_rows())
        self.assertIn(f"{rows} native rows", result.stdout)
        self.assertGreater(rows, 300)
        self.assertIn("(the SDK's)", result.stdout)

    def test_the_sdk_carries_every_file_the_closure_names(self):
        wanted = model_sdk.files()
        # The stamp holds each file the tools depend on, headers (write_sdk's own copies) too.
        self.assertEqual(set(self.stamp["files"]), set(model_sdk.stamped()))
        self.assertLessEqual(set(wanted), set(self.stamp["files"]))
        for name in self.stamp["files"]:
            self.assertTrue(self.sdk_file(name).is_file(), name)
        self.assertTrue(any(name.startswith("src/native_overlays/") and name.endswith(".h")
                            for name in self.stamp["files"]), "headers of the modules are stamped")
        self.assertIn("src/game/model.h", self.stamp["files"])
        for name in model_sdk.row_sources():
            self.assertIn(name, self.stamp["files"])
        # The tool's Python is the closure of its entry points, and the entry points are all in it.
        tools = {name for name in wanted if name.startswith("tools/pc/")}
        self.assertEqual(tools, {f"tools/pc/{name}" for name in model_sdk.tool_modules()})
        self.assertLessEqual({f"tools/pc/{entry}.py" for entry in model_sdk.ENTRY}, tools)
        self.assertEqual(self.stamp["native_rows"], len(model_sdk.native_rows()))

    def test_the_digests_the_labels_carry_are_the_sdks_too(self):
        """The tool digest and every row's digest name the repository's paths
        and hash the files' text: recomputed in the SDK they are the checkout's."""
        code = ("import sys; sys.path.insert(0, sys.argv[1]); import model_span_words as w; "
                "print(w.tool_digest()); print(*(w.row_digest(row) for row in w.all_rows()), sep=chr(10))")
        mine = child([sys.executable, "-c", code, str(self.sdk / "tools")], self.elsewhere)
        theirs = child([sys.executable, "-c", code, str(HERE)], self.elsewhere)
        self.assertEqual(mine.returncode, 0, mine.stderr)
        self.assertEqual(mine.stdout, theirs.stdout)

    def test_commands_that_need_the_repository_refuse(self):
        for command in ("check", "update", "write", "port"):
            result = run(self.sdk, command, cwd=self.elsewhere)
            self.assertEqual(result.returncode, 2, command)
            self.assertIn("does not have", result.stderr)

    def test_the_tool_without_the_disc_says_so(self):
        mod = self.work / "mod"
        mod.mkdir()
        (mod / "mod.json").write_text('{"id": "x"}', encoding="utf-8")
        result = run(self.sdk, "mod-migrate", str(mod), cwd=self.elsewhere)
        self.assertEqual(result.returncode, 2)
        self.assertIn("needs the disc", result.stderr)

    def broken(self, touched, change, expect: str) -> None:
        """mod-check on the SDK after `change(sdk)`, which touches the files `touched` (names in the SDK, as
        the stamp has them: put back afterwards): it fails, and says `expect`."""
        paths = [self.sdk / name for name in touched]
        saved = {path: path.read_bytes() for path in paths}
        try:
            change(self.sdk)
            result = run(self.sdk, "mod-check", cwd=self.elsewhere)
        finally:
            for path, data in saved.items():
                path.write_bytes(data)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(expect, result.stderr)

    def test_a_missing_c_file_is_named(self):
        name = next(name for name, kind in model_sdk.files().items() if kind == "row" and name.endswith("_entry.c"))
        self.broken([model_sdk.shipped_name(name)], lambda sdk: (sdk / model_sdk.shipped_name(name)).unlink(),
                    f"{model_sdk.shipped_name(name)} is missing")

    def test_a_changed_file_is_named(self):
        name = next(name for name, kind in model_sdk.files().items() if kind == "row" and name.endswith("_entry.c"))
        def change(sdk):
            path = sdk / model_sdk.shipped_name(name)
            path.write_bytes(path.read_bytes() + b"\n/* edited */\n")
        self.broken([model_sdk.shipped_name(name)], change, f"{model_sdk.shipped_name(name)} is not the file the SDK was made with")

    def test_an_altered_header_is_named(self):
        """The stamp holds the headers the tool depends on, not only the C."""
        name = next(name for name in model_sdk.stamped()
                    if name.startswith("src/native_overlays/") and name.endswith(".h"))
        def change(sdk):
            path = sdk / model_sdk.shipped_name(name)
            path.write_bytes(path.read_bytes() + b"\n/* edited */\n")
        self.broken([model_sdk.shipped_name(name)], change, f"{model_sdk.shipped_name(name)} is not the file the SDK was made with")

    def test_a_file_the_tool_would_use_that_the_sdk_was_not_made_with_is_named(self):
        """A row's C that includes a header the SDK has but was not stamped with (or a pattern that finds an extra
        file) is not silently used: mod-check names it."""
        every = set(self.stamp["files"])
        header = next(f"src/game/{path.name}" for path in sorted((self.sdk / "include/game").glob("*.h"))
                      if f"src/game/{path.name}" not in every)
        name = next(name for name, kind in model_sdk.files().items() if kind == "row" and name.endswith("_entry.c"))
        def change(sdk):
            path = sdk / model_sdk.shipped_name(name)
            path.write_bytes(('#include "' + header[len("src/"):] + '"' + chr(10)).encode() + path.read_bytes())
            stamp = json.loads((sdk / "config/model_sdk.json").read_text(encoding="utf-8"))
            stamp["files"][name] = model_sdk.sha256(path)   # a stamp made after the edit: only the header is new
            (sdk / "config/model_sdk.json").write_text(json.dumps(stamp), encoding="utf-8")
        self.broken([model_sdk.shipped_name(name), "config/model_sdk.json"], change,
                    f"{model_sdk.shipped_name(header)} is not a file the SDK was made with")

    def test_what_an_sdk_says_about_stale_labels_never_sends_it_to_the_repository(self):
        """Stale when packed (the stamp says so) or altered since (a digest differs): in an SDK there is no
        command to run, and model_span_words.py itself refuses."""
        code = ("import sys; sys.path.insert(0, sys.argv[1]); import model_span_words as w; print(w.remedy())")
        path = self.sdk / "config/model_sdk.json"
        saved = path.read_bytes()
        try:
            self.assertIn("is not the one it was made with", child([sys.executable, "-c", code,
                                                                    str(self.sdk / "tools")], self.elsewhere).stdout)
            stamp = json.loads(saved.decode("utf-8"))
            stamp["labels"]["problems"] = ["model_span_words.csv was made by another version"]
            path.write_text(json.dumps(stamp), encoding="utf-8")
            said = child([sys.executable, "-c", code, str(self.sdk / "tools")], self.elsewhere).stdout
        finally:
            path.write_bytes(saved)
        self.assertIn("already stale when this SDK was made", said)
        self.assertNotIn("model_span_words.py", said)
        refused = child([sys.executable, str(self.sdk / "tools/model_span_words.py")], self.elsewhere)
        self.assertEqual(refused.returncode, 2)
        self.assertIn("needs the repository", refused.stderr)

    def test_the_fingerprint_watches_every_folder_the_closure_reads_from(self):
        """A new header in src/psyq (globbed by stamped()), in src/game or beside a module (a quoted include
        starts to resolve there), a new folder under src/, a new C file or Python module: each changes a folder
        the fingerprint holds, so the next build does the step again."""
        found = model_sdk.fingerprint(ROOT, self.stamp["files"])
        folders = {name for name in found if name.endswith("/")}
        for folder in ("src/", "src/psyq/", "src/game/", "src/native_overlays/", "src/native_overlays/model_variant/",
                       "tools/pc/", "config/pc/"):
            self.assertIn(folder, folders)
        for name in self.stamp["files"]:
            self.assertIn(name.rsplit("/", 1)[0] + "/", folders, name)

    def test_install_does_nothing_when_nothing_changed(self):
        """A build in which no file the stamp names, and no folder that could hold another, changed skips the
        closure and the digests (about 20 s each build): the checkout's sizes and times are compared."""
        copy = self.work / "incremental" / "sdk"
        shutil.copytree(self.sdk, copy, ignore=shutil.ignore_patterns("examples", "notes", "libc"))
        first = model_sdk.install(copy)   # no inputs file beside it yet: the whole step
        self.assertEqual(set(first["files"]), set(self.stamp["files"]))
        self.assertTrue((copy.parent / "model_sdk_inputs.json").is_file())
        with mock.patch.object(model_sdk, "files", side_effect=AssertionError("the closure was computed again")):
            self.assertEqual(model_sdk.install(copy), first)
            # ... and one more of a file the stamp names, a time changed, does the whole step again.
            name = next(iter(first["files"]))
            inputs = json.loads((copy.parent / "model_sdk_inputs.json").read_text(encoding="utf-8"))
            inputs[name][1] += 1
            (copy.parent / "model_sdk_inputs.json").write_text(json.dumps(inputs), encoding="utf-8")
            with self.assertRaises(AssertionError):
                model_sdk.install(copy)

    def test_no_stamp_is_a_failure(self):
        self.broken(["config/model_sdk.json"], lambda sdk: (sdk / "config/model_sdk.json").unlink(),
                    "is missing: this is not a whole SDK")

    def test_a_stamp_that_calls_the_labels_stale_is_a_warning(self):
        """Stale labels block nothing: mod-check passes, says so on stderr and in its summary."""
        path = self.sdk / "config/model_sdk.json"
        saved = path.read_bytes()
        try:
            stamp = json.loads(saved.decode("utf-8"))
            stamp["labels"]["problems"] = ["model_span_words.csv was made by another version of the analysis"]
            path.write_text(json.dumps(stamp), encoding="utf-8")
            result = run(self.sdk, "mod-check", cwd=self.elsewhere)
        finally:
            path.write_bytes(saved)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WARNING: the span-word labels are stale: the labels were already stale when this SDK was made: "
                      "model_span_words.csv was made by another version", result.stderr)
        self.assertIn("labels STALE", result.stdout)

    def test_a_forgotten_include_is_found(self):
        """port_closure skips an include with no file; mod-check does not."""
        name = next(name for name, kind in model_sdk.files().items() if kind == "row" and name.endswith("_entry.c"))
        def change(sdk):
            path = sdk / model_sdk.shipped_name(name)
            path.write_bytes(b'#include "game/no_such_header_here.h"\n' + path.read_bytes())
            stamp = json.loads((sdk / "config/model_sdk.json").read_text(encoding="utf-8"))
            stamp["files"][name] = model_sdk.sha256(path)   # a stamp made after the edit: only the include is wrong
            (sdk / "config/model_sdk.json").write_text(json.dumps(stamp), encoding="utf-8")
        self.broken([model_sdk.shipped_name(name), "config/model_sdk.json"], change,
                    'no file for ' + model_sdk.shipped_name(name) + ': #include "game/no_such_header_here.h"')

    def test_nothing_from_a_disc(self):
        # Files git knows, or would add (a change in the making): not one that is ignored, like tmp/.
        known = None
        if (ROOT / ".git").exists():
            known = set(subprocess.run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                                       cwd=ROOT, capture_output=True, text=True, check=True).stdout.split("\0"))
        if known is not None:
            self.assertEqual([name for name in self.stamp["files"] if name not in known], [],
                             "files that are not the repository's")
        for name in self.stamp["files"]:
            path = self.sdk_file(name)
            self.assertNotIn(path.suffix.lower(), {".bin", ".cue", ".iso", ".chd", ".mrg", ".mcr", ".exe"}, name)
            data = path.read_bytes()
            self.assertNotIn(b"\0", data, f"{name} is not text")
            if path.suffix != ".h":   # (the game's headers keep their comments' own code page)
                data.decode("utf-8")
            self.assertLess(len(data), 1_000_000, name)
        # The tables: names, offsets, hashes and labels. A run of hexadecimal digits is a SHA-256 (64), a
        # digest of the C (16), or a commit (40): never the bytes of an image.
        for name in self.stamp["files"]:
            if name.startswith("config/pc/") and name.endswith((".csv", ".txt")):
                text = self.sdk_file(name).read_text(encoding="utf-8")
                self.assertTrue({len(run_) for run_ in re.findall(r"[0-9a-fA-F]{16,}", text)} <= {16, 40, 64}, name)

    def test_every_digested_file_is_in_the_sdk(self):
        """What the labels were made from is shipped, or they could not be checked there."""
        for name in model_sdk.digested():
            self.assertTrue(model_sdk.shipped_header(name) or name in self.stamp["files"], name)
            self.assertTrue(self.sdk_file(name).is_file(), name)


if __name__ == "__main__":
    unittest.main(verbosity=2)
