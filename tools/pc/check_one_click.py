#!/usr/bin/env python3
"""Check what the SDK's one-click build (sdk/build-mod.bat or build-mod.sh) made.

  python tools/pc/check_one_click.py MOD_FOLDER --sdk UNPACKED/sdk

The release workflow's clean-machine job unpacks an SDK, runs the launcher on a
copy of examples/mods/gameplay-rules with nothing but the operating system on
PATH, and calls this afterwards with the runner's own Python (this script needs
nothing the launcher fetched). It asserts:

- the objects are there and are what their names say, read from the ELF header:
  <library>.o 32-bit x86, <library>.x86_64-windows.o x86-64,
  <library>.aarch64-translated.o AArch64, each a relocatable object, the two
  64-bit ones with the .memories.abi section that names their target for the
  game's loader;
- the Android object is among them exactly when the SDK carries the guest
  pass, and all three are required then. An SDK without the pass (package.py
  --no-guest-ir) fails, unless --allow-no-guest-pass says it is a pull
  request's, where the pass is not always built: that run prints a
  ::warning:: that the Android object was not proven. Only llvm-mingw's clang
  23 from sdk/toolchain can make the object, which is the proof that the
  launcher's downloaded compiler was the one used;
- the launcher fetched what it says it does: sdk/toolchain/llvm-mingw-<release>/
  (and python-<version>/ on Windows), no archive left in downloads/ and no
  half-unpacked .unpack-* folder.
"""
import argparse, glob, json, os, struct, sys

# (suffix, ELF class, e_machine, has .memories.abi)
OBJECTS = (
    (".o", 1, 3, False),                        # EM_386
    (".x86_64-windows.o", 2, 62, True),         # EM_X86_64
    (".aarch64-translated.o", 2, 183, True),    # EM_AARCH64
)


def check_object(path, elf_class, machine, abi):
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:4] != b"\x7fELF":
        sys.exit(f"check_one_click: {path} is not an ELF object")
    if data[4] != elf_class:
        sys.exit(f"check_one_click: {path} is ELF class {data[4]}, not {elf_class}")
    kind, found = struct.unpack_from("<HH", data, 16)
    if kind != 1 or found != machine:
        sys.exit(f"check_one_click: {path} is type {kind} for machine {found}, not a relocatable object for {machine}")
    if abi and b".memories.abi" not in data:
        sys.exit(f"check_one_click: {path} has no .memories.abi section")
    if len(data) < 1000:
        sys.exit(f"check_one_click: {path} is only {len(data)} bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mod", help="the mod's folder, built by the launcher")
    parser.add_argument("--sdk", required=True, help="the unpacked SDK (its sdk/ folder)")
    parser.add_argument("--android", choices=("auto", "yes", "no"), default="auto",
                        help="whether the Android object is expected (default: when the SDK has the guest pass)")
    parser.add_argument("--allow-no-guest-pass", action="store_true",
                        help="a pull request's SDK may have no guest pass: warn instead of failing")
    parser.add_argument("--no-download", action="store_true",
                        help="the launcher was given its own tools: sdk/toolchain is not expected")
    options = parser.parse_args()
    with open(os.path.join(options.sdk, "toolchain-pins.json"), encoding="utf-8") as handle:
        pins = json.load(handle)
    guest_pass = [name for name in ("memories-guest-ir", "memories-guest-ir.exe")
                  if os.path.isfile(os.path.join(options.sdk, "tools", name))]
    android = options.android == "yes" or (options.android == "auto" and bool(guest_pass))
    if options.android == "auto" and not guest_pass:
        if not options.allow_no_guest_pass:
            sys.exit("check_one_click: the SDK has no tools/memories-guest-ir, so the Android object cannot be built: "
                     "a release SDK carries the guest pass (a pull request's may lack it: --allow-no-guest-pass)")
        print("::warning title=Android object not proven::this pull request's SDK has no guest pass "
              "(tools/memories-guest-ir), so the aarch64-translated object was not built or checked")
    with open(os.path.join(options.mod, "mod.json"), encoding="utf-8") as handle:
        library = json.load(handle).get("library")
    if not library:
        sys.exit("check_one_click: the mod has no \"library\" in its mod.json")
    for suffix, elf_class, machine, abi in OBJECTS:
        path = os.path.join(options.mod, library + suffix)
        if suffix.startswith(".aarch64") and not android:
            if os.path.exists(path):
                sys.exit(f"check_one_click: {path} was made, but this SDK has no guest pass")
            continue
        if not os.path.isfile(path):
            sys.exit(f"check_one_click: {path} was not made (the Android object needs the SDK's guest pass and "
                     "llvm-mingw's clang 23)" if suffix.startswith(".aarch64") else f"check_one_click: {path} was not made")
        check_object(path, elf_class, machine, abi)
        print(f"ok: {os.path.basename(path)}")
    if not options.no_download:
        toolchain = os.path.join(options.sdk, "toolchain")
        wanted = [f"llvm-mingw-{pins['llvm_mingw']['release']}"]
        if os.name == "nt":
            wanted.append(f"python-{pins['python']['windows']['version']}")
        for name in wanted:
            if not os.path.isdir(os.path.join(toolchain, name)):
                sys.exit(f"check_one_click: the launcher fetched no {name} into {toolchain}")
        left = glob.glob(os.path.join(toolchain, "downloads", "*")) + glob.glob(os.path.join(toolchain, ".unpack-*"))
        if left:
            sys.exit(f"check_one_click: left over in {toolchain}: {left}")
        print(f"ok: {', '.join(wanted)} in sdk/toolchain, nothing left over")
    print("check_one_click: passed" + ("" if android else " (without the Android object: this SDK has no guest pass)"))


if __name__ == "__main__":
    main()
