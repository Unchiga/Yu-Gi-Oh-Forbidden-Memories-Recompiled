#!/usr/bin/env python3
"""Regression for rejected native-mod callbacks, with one object on both OSes.

--target android-arm64 runs only the function hooks test, built with the NDK
for the arm64 game's layout, on the device adb has (ANDROID_SERIAL picks one):
once patching the text in place, once with MEMORIES_TEST_ANON_TEXT=1, which
makes hooks.c take the anonymous copy an app's SELinux policy may force."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import build_mod
import build_win32_deps

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "tmp/pc/mods-lifecycle"
SOURCES = ["src/pc/compat/fs.c", "tests/pc/mods_lifecycle_test.c", "src/pc/mods/mods.c", "src/pc/mods/events.c", "src/pc/mods/hooks.c",
           "src/pc/mods/manager.c", "src/pc/mods/overlap.c", "src/pc/mods/mod_libc.c", "src/pc/mods/object_loader.c", "src/pc/guest/branch_thunks.c", "src/pc/mod_packets.c",
           "src/pc/mods/json.c", "src/pc/platform/settings.c", "src/pc/platform/paths.c"]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("linux", "windows", "both", "android-arm64"), default="linux")
    args = parser.parse_args()
    os.chdir(ROOT)
    OUT.mkdir(parents=True, exist_ok=True)
    if args.target == "android-arm64":
        android_hooks()
        return
    os.environ["TMPDIR"] = str(OUT)
    fixture = OUT / "rejected.o"
    build_mod.compile_object(["tests/pc/mod_fixtures/rejected.c"], str(fixture), str(OUT / "obj"))
    no_api = OUT / "no_api.o"
    build_mod.compile_object(["tests/pc/mod_fixtures/no_api.c"], str(no_api), str(OUT / "obj-no-api"))
    for target in ("linux", "windows") if args.target == "both" else (args.target,):
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Isrc"]
        environment = dict(os.environ, TMPDIR=str(OUT))
        if target == "linux":
            program = OUT / "lifecycle"
            compile_cmd = ["gcc", "-m32", *flags, *SOURCES, "-lm", "-o", str(program)]
            launch = [str(program)]
        else:
            build_win32_deps.use_toolchain()
            program = OUT / "lifecycle.exe"
            compile_cmd = ["i686-w64-mingw32-clang", *flags, *SOURCES, "-static", "-lshell32", "-lole32", "-lwinpthread", "-o", str(program)]
            launch = [str(program)]
            if sys.platform != "win32":
                launch = ["wine", str(program)]
                environment.update(WINEPREFIX=str(ROOT / "tmp/pc/wine-prefix"), WINEDLLOVERRIDES="mscoree,mshtml=", WINEDEBUG="-all")
        subprocess.run(compile_cmd, check=True)
        subprocess.run([*launch, str(fixture), str(no_api)], env=environment, check=True, timeout=60)
        print(f"mods lifecycle: {target} passed")
        # Function hooks, on targets laid out as the game units are.
        hooks_program = str(OUT / program.name.replace("lifecycle", "hooks"))
        hooks_cmd = [compile_cmd[0], *compile_cmd[1:compile_cmd.index(SOURCES[0])], "-O0",
                     "tests/pc/hooks_test.c", "src/pc/mods/hooks.c", "-o", hooks_program]
        if target == "windows":
            hooks_cmd.insert(-2, "-static")
        subprocess.run(hooks_cmd, check=True)
        subprocess.run([*launch[:-1], hooks_program], env=environment, check=True, timeout=60)
        if target == "linux":
            state_program = str(program) + "-state"
            state_command = [*compile_cmd[:compile_cmd.index(SOURCES[0])], "-ffunction-sections", "-fdata-sections",
                             "tests/pc/mods_state_test.c", "-Wl,--gc-sections", "-o", state_program]
            subprocess.run(state_command, check=True)
            subprocess.run([state_program, str(OUT / "state-chunks.bin")], env=environment, check=True, timeout=60)
            print("mod save-state compatibility: linux passed")

def android_hooks():
    sys.path.insert(0, str(ROOT / "tools/pc/android"))
    import build_android_deps
    import device_run
    program = OUT / "hooks-arm64"
    subprocess.run([str(Path(build_android_deps.llvm_bin()) / "clang"), "--target=aarch64-linux-android24", "-fPIE", "-pie",
                    "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Isrc", "-O0", "-DMEMORIES_TEST_HOOKS",
                    "tests/pc/hooks_test.c", "src/pc/mods/hooks.c", "src/pc/compat/fs.c", "-o", str(program)], check=True)
    remote = f"{device_run.REMOTE}/hooks-test"
    device_run.call("shell", f"mkdir -p {device_run.REMOTE}")
    device_run.call("push", str(program), remote)
    device_run.call("shell", f"chmod 755 {remote}")
    for anonymous, path in (("0", "patched in place"), ("1", "anonymous copy of the text")):
        result = subprocess.run(device_run.adb() + ["shell", f"MEMORIES_TEST_ANON_TEXT={anonymous} {remote}; echo exit=$?"],
                                capture_output=True, text=True, timeout=60)
        output = result.stdout + result.stderr
        if "exit=0" not in output or "hooks: passed" not in output or path not in output:
            sys.exit(f"hooks: android-arm64 (MEMORIES_TEST_ANON_TEXT={anonymous}) failed:\n{output}")
        print(f"hooks: android-arm64 passed, {path}")


if __name__ == "__main__":
    main()
