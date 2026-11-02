#!/usr/bin/env python3
"""Runtime combat scenarios test for Mod API 19: Battle Immunity & Piercing Damage.

Tests 10 total combat scenarios across both 2D and 3D battle modes:
  1. CPU Piercing vs Player Immune DEF (CPU pierces 0-battle-damage wall for LP damage)
  2. CPU Multi-Attack Focus vs 2-Hit Shield (CPU coordinates attacks to break shield and destroy)
  3. Player Piercing vs CPU Immune DEF & Non-Immune DEF (Damage and survival/destruction checks)
  4. CPU Attacks Player Battle-Immune ATK (Battle damage dealt, monster survives)
  5. Player Attacks CPU Battle-Immune ATK/DEF (Battle damage dealt, monster survives)
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "tmp/pc/combat-immunity-piercing-tests"
sys.path.insert(0, str(ROOT / "tools/pc"))
import build_win32_deps


def run_scenarios(target: str = "windows") -> int:
    os.chdir(ROOT)
    OUT.mkdir(parents=True, exist_ok=True)

    flags = [
        "-std=gnu11",
        "-O1",
        "-g",
        "-DMEMORIES_PC",
        "-Isrc",
        "-Imods/ai-hard-mode",
    ]

    if target == "linux":
        compiler = ["gcc", "-m32"]
        flags += ["-Wall", "-Wextra"]
        launcher = []
        env = dict(os.environ, TMPDIR=str(OUT))
    else:
        build_win32_deps.use_toolchain()
        compiler = ["i686-w64-mingw32-clang"]
        flags += ["-static", "-mno-ms-bitfields", "-Wno-gnu-folding-constant"]
        launcher = [] if sys.platform == "win32" else ["wine"]
        env = dict(os.environ, TMPDIR=str(OUT))
        if sys.platform != "win32":
            env.update(
                WINEPREFIX=str(ROOT / "tmp/pc/wine-prefix"),
                WINEDLLOVERRIDES="mscoree,mshtml=",
                WINEDEBUG="-all",
            )

    program = OUT / ("combat_immunity_piercing_runtime" + (".exe" if target == "windows" else ""))
    sources = [
        "tests/pc/combat_immunity_piercing_runtime.c",
        "src/pc/cards/monster_effects_duel.c",
        "mods/ai-hard-mode/planner.c",
    ]

    print(f"[TEST] Compiling {target} combat immunity and piercing runtime scenarios...", flush=True)
    compile_cmd = [*compiler, *flags, *sources, "-o", str(program)]
    subprocess.run(compile_cmd, check=True)

    print(f"[TEST] Executing 10 scenarios in 2D and 3D presentation ({target})...\n", flush=True)
    run_cmd = [*launcher, str(program)]
    result = subprocess.run(run_cmd, env=env, check=True)
    return result.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    default_target = "windows" if sys.platform == "win32" else "linux"
    parser.add_argument("--target", choices=("linux", "windows", "both"), default=default_target)
    args = parser.parse_args()

    targets = ("linux", "windows") if args.target == "both" else (args.target,)
    for tgt in targets:
        code = run_scenarios(tgt)
        if code != 0:
            return code
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

