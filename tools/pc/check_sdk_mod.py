#!/usr/bin/env python3
"""Build a bundled code mod for Android from a packed SDK, as a modder would.

  python tools/pc/check_sdk_mod.py dist/yfm-redecomp-<v>-windows.zip --ir-out tmp/sdk-mod-ir

Unpacks the archive, puts the SDK's own toolchain (llvm-mingw, the one
build_mod.py names: tools/pc/fetch_tools.py on Windows, build_win32_deps.py
on Linux) first on PATH and nothing else of the build's environment (no NDK,
no MEMORIES_GUEST_IR), and runs the SDK's tools/build_mod.py on a mod of this
checkout for aarch64-translated: the SDK's guest pass, tables and headers
with the SDK's clang, which is the path a Windows modder takes (a regression
against v0.3.0 when the SDK shipped no pass). The IR it keeps (--ir-out:
<unit>.raw.ll from clang, <unit>.ll from the pass) and a versions.txt go to
the release workflow's artifact, to be diffed against the pass the game
itself is translated with (LLVM 21).
"""
import argparse, hashlib, os, shutil, subprocess, sys, tarfile, tempfile, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
EXE = ".exe" if sys.platform == "win32" else ""


def unpack(archive, folder):
    if archive.endswith(".zip"):
        with zipfile.ZipFile(archive) as bundle:
            bundle.extractall(folder)
    else:
        with tarfile.open(archive) as bundle:
            if hasattr(tarfile, "data_filter"):
                bundle.extractall(folder, filter="data")
            else:
                bundle.extractall(folder)
    (top,) = [name for name in os.listdir(folder)]
    return os.path.join(folder, top, "sdk")


def toolchain():
    """The SDK's named toolchain on PATH: llvm-mingw."""
    if sys.platform == "win32":
        import fetch_tools
        fetch_tools.ensure("llvm-mingw")
    else:
        import build_win32_deps
        if not os.path.isdir(build_win32_deps.TOOLCHAIN):
            os.replace(build_win32_deps.fetch("llvm-mingw"), build_win32_deps.TOOLCHAIN)
        build_win32_deps.use_toolchain()
    clang = shutil.which("clang")
    if not clang:
        sys.exit("check_sdk_mod: llvm-mingw's clang is not on PATH")
    return clang


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("archive", help="the packed SDK (.zip or .tar.gz)")
    parser.add_argument("--mod", default=os.path.join(ROOT, "mods", "hand-camera"), help="the mod to build")
    parser.add_argument("--ir-out", required=True, help="where the IR and versions.txt go")
    options = parser.parse_args()
    clang = toolchain()
    environment = {name: value for name, value in os.environ.items()
                   if name not in ("MEMORIES_GUEST_IR", "MEMORIES_GUEST_LLVM", "MEMORIES_MOD_CC",
                                   "ANDROID_NDK_ROOT", "ANDROID_NDK_HOME")}
    os.makedirs(options.ir_out, exist_ok=True)
    with tempfile.TemporaryDirectory() as folder:
        sdk = unpack(options.archive, os.path.join(folder, "sdk"))
        tools = os.path.join(sdk, "tools")
        guest_ir = os.path.join(tools, "memories-guest-ir" + EXE)
        if not os.path.isfile(guest_ir):
            sys.exit(f"check_sdk_mod: {options.archive} has no sdk/tools/memories-guest-ir{EXE}")
        out = os.path.join(folder, "out")
        # No --game: the SDK's own tables (mod_translation.json, mod_signatures.json) are what is checked.
        command = [sys.executable, os.path.join(tools, "build_mod.py"), options.mod, "--target", "aarch64-translated",
                   "--out", out, "--ir-out", options.ir_out]
        print("+ " + " ".join(command), flush=True)
        if subprocess.run(command, env=environment).returncode:
            sys.exit("check_sdk_mod: the SDK could not build the Android object of " + options.mod)
        objects = [name for name in os.listdir(out) if name.endswith(".aarch64-translated.o")] if os.path.isdir(out) else []
        if not objects:
            sys.exit(f"check_sdk_mod: build_mod.py made no .aarch64-translated.o in {out}")
        units = sorted(name for name in os.listdir(options.ir_out) if name.endswith(".ll"))
        if not any(name.endswith(".raw.ll") for name in units) or not any(not name.endswith(".raw.ll") for name in units):
            sys.exit(f"check_sdk_mod: no raw and translated IR in {options.ir_out} ({units})")
        lines = []
        for label, program in (("memories-guest-ir", guest_ir), ("clang", clang)):
            said = subprocess.run([program, "--version"], capture_output=True, text=True).stdout.splitlines()
            lines.append(f"{label}: {said[0] if said else '?'}")
        lines.append(f"pass LLVM major: {subprocess.run([guest_ir, '--llvm-major'], capture_output=True, text=True).stdout.strip()}")
        lines.append(f"object: {objects[0]} {hashlib.sha256(open(os.path.join(out, objects[0]), 'rb').read()).hexdigest()}")
        for name in units:
            with open(os.path.join(options.ir_out, name), "rb") as handle:
                lines.append(f"{name}: {hashlib.sha256(handle.read()).hexdigest()}")
        with open(os.path.join(options.ir_out, "versions.txt"), "w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join(lines) + "\n")
        print("\n".join(lines))


if __name__ == "__main__":
    main()
