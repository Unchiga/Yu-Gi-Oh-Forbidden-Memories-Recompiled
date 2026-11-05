#!/usr/bin/env python3
"""Every function the mod SDK's C library declares is lent to a translated mod.

A code mod is built against src/pc/mods/sdk, which promises the list in
src/pc/mods/mod_libc.c. The i386 and 64-bit Windows games lend exactly that
list, but the translated game (Android arm64, macOS) lends a mod only the
pointer-free maths directly and the runtime's guest-pointer wrappers
(GuestRuntime_*, src/pc/guest/translated_libc.c) for the rest, so a name the
SDK declares and the translated game does not lend builds for Windows and is
refused for Android (fread and ftell once were). No compiler starts here.

- the SDK's headers and mod_libc.c's list name the same functions;
- each of them is in what write_mod_translation (build_arm64.py) gives a
  translated mod (renamed to the runtime's wrappers, lent as maths, or
  aliased), or is in NOT_LENT, which says why it is not;
- each wrapper it renames to is declared in translated_libc.h and defined in
  translated_libc.c or translated_alloc.c, so the rename cannot name nothing."""
import json
import pathlib
import re
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import build_arm64
import build_mod

ROOT = pathlib.Path(__file__).resolve().parents[2]
SDK = ROOT / "src/pc/mods/sdk"

# SDK functions a translated mod cannot link, each with the reason. A name here
# is a promise the SDK makes and the Android app breaks: say so in
# notes/modding.md (Android code mods) too.
NOT_LENT = {}

def sdk_functions():
    """The functions the SDK's headers declare: prototypes, not the inline
    definitions of ctype.h or the macros that follow math.h and string.h
    (mod_libc.c's compiler helpers are in no header). Attributes and the
    bodies of inline functions are cut first, so a prototype after either is
    read as its own statement."""
    names = set()
    for header in sorted(SDK.glob("*.h")):
        text = re.sub(r"/\*.*?\*/", "", header.read_text(), flags=re.S)
        text = re.sub(r"__attribute__\s*\(\((?:[^()]|\((?:[^()]|\([^()]*\))*\))*\)\)", "", text)
        text = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
        # A body ends the statement it belongs to: `{...}` becomes `;`, innermost first.
        while True:
            reduced = re.sub(r"\{[^{}]*\}", ";", text)
            if reduced == text:
                break
            text = reduced
        for statement in text.split(";"):
            declaration = " ".join(statement.split())
            if not declaration or "static inline" in declaration or declaration.startswith("typedef"):
                continue
            found = re.match(r"^[\w\s\*]+?\b(\w+)\s*\(", declaration)
            if found:
                names.add(found.group(1))
    return names


def translated_names():
    """What a translated mod is lent, from the file build_arm64.py writes for
    build_mod.py --target aarch64-translated (and build_mod_arm64.py for macOS)."""
    with tempfile.TemporaryDirectory(prefix="mod-libc-lent-") as folder:
        build = pathlib.Path(folder)
        (build / "summary.json").write_text(json.dumps({"aliases": {}, "pc_storage_globals": []}))
        (build / "build-fingerprint.txt").write_text("test\n")
        build_arm64.write_mod_translation(build, {})
        table = json.loads((build / "mod_translation.json").read_text())
    renamed = set(table["runtime"])
    lent = set(table["system_math"])
    aliased = {name for name in ("rand", "srand") if table["aliases"].get(name)}
    return renamed, lent, aliased, table


class SdkLibc(unittest.TestCase):
    def test_sdk_and_mod_libc_list_agree(self):
        listed = {name for name in build_mod.libc_names("i386") if not name.startswith("__")}
        declared = sdk_functions()
        self.assertEqual(sorted(declared - listed), [], "declared by the SDK, not lent by mod_libc.c")
        self.assertEqual(sorted(listed - declared), [], "lent by mod_libc.c, not declared by the SDK")

    def test_every_sdk_function_reaches_a_translated_mod(self):
        renamed, lent, aliased, _ = translated_names()
        missing = sdk_functions() - renamed - lent - aliased - set(NOT_LENT)
        self.assertEqual(sorted(missing), [],
                         "the SDK declares these, the translated game does not lend them "
                         "(add each to HOST_LIBC in guest_build.py with a GuestRuntime_ wrapper, "
                         "or to NOT_LENT here and notes/modding.md)")
        self.assertEqual(sorted(set(NOT_LENT) & (renamed | lent | aliased)), [], "lent after all: drop it from NOT_LENT")

    def test_translated_renames_have_wrappers(self):
        _, _, _, table = translated_names()
        header = (ROOT / "src/pc/guest/translated_libc.h").read_text()
        source = (ROOT / "src/pc/guest/translated_libc.c").read_text() + \
            (ROOT / "src/pc/guest/translated_alloc.c").read_text()
        for name in sdk_functions() & set(table["runtime"]):
            wrapper = table["runtime"][name]
            declared = re.search(rf"\b{wrapper}\(", header) or wrapper in ("GuestRuntime_malloc", "GuestRuntime_calloc",
                                                                           "GuestRuntime_realloc", "GuestRuntime_free")
            defined = re.search(rf"^[\w\s\*]+\b{wrapper}\(", source, re.M)
            self.assertTrue(declared, f"{wrapper} is not declared in translated_libc.h")
            self.assertTrue(defined, f"{wrapper} is not defined in translated_libc.c or translated_alloc.c")


if __name__ == "__main__":
    unittest.main()
