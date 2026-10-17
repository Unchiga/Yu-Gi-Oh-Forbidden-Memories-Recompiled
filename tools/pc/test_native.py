#!/usr/bin/env python3
"""ROM-free native component checks; does not build the i386 game runtime."""

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

from guest_test_compile import guest_object
from macos_deps import sdk_path

ROOT = Path(__file__).resolve().parents[2]


def darwin_program_dir_cases(out, compiler, flags):
    binary = out / "darwin-paths"
    subprocess.run(
        [
            compiler,
            *flags,
            "tests/pc/darwin_paths_test.c",
            "src/pc/platform/paths.c",
            "-o",
            str(binary),
        ],
        check=True,
    )
    fixture = out / "darwin-path-fixtures"
    shutil.rmtree(fixture, ignore_errors=True)
    fixture.mkdir(parents=True)

    app = fixture / "Test.app"
    contents = app / "Contents"
    macos = contents / "MacOS"
    resources = contents / "Resources"
    macos.mkdir(parents=True)
    (resources / "languages").mkdir(parents=True)
    (contents / "Info.plist").write_text(
        '<?xml version="1.0"?><plist><dict>'
        "<key>CFBundlePackageType</key><string>APPL</string>"
        "<key>CFBundleExecutable</key><string>memories-arm64</string>"
        "</dict></plist>\n",
        encoding="utf-8",
    )

    def run(executable, expected_program, user_dir, program_override=None):
        executable.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(binary, executable)
        environment = {
            **os.environ,
            "MEMORIES_USER_DIR": str(user_dir),
            "MEMORIES_PROGRAM_DIR": str(program_override or ""),
        }
        subprocess.run(
            [str(executable), str(expected_program), str(user_dir)],
            check=True,
            env=environment,
        )
        if not user_dir.is_dir():
            raise AssertionError(
                f"user directory was not created outside the app: {user_dir}"
            )

    run(macos / "memories-arm64", resources, fixture / "user-bundle")
    run(
        fixture / "standalone" / "memories-arm64",
        fixture / "standalone",
        fixture / "user-standalone",
    )

    decoy_macos = fixture / "Decoy.app" / "Contents" / "MacOS"
    decoy_macos.mkdir(parents=True)
    (decoy_macos.parent / "Resources").mkdir()
    run(decoy_macos / "memories-arm64", decoy_macos, fixture / "user-decoy")

    override = fixture / "program-override"
    run(
        macos / "memories-override-test",
        override,
        fixture / "user-override",
        program_override=override,
    )
    if (resources / "user").exists():
        raise AssertionError("player data was written inside Contents/Resources")
    return 4


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if Path.cwd().resolve() != ROOT:
        parser.error("run from the repository root")
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        parser.error("this runner requires macOS arm64")
    out = ROOT / "tmp" / "native-tests"
    out.mkdir(parents=True, exist_ok=True)
    compiler = os.environ.get("CC", "/usr/bin/clang")
    flags = [
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
        "-D_DARWIN_C_SOURCE",
        "-Isrc",
        "-Itests/pc",
    ]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    cases = [
        ("state-requests", ["tests/pc/state_requests_test.c", "src/pc/guest/state_requests.c"], [], [None]),
        ("ot-chain", ["tests/pc/ot_chain_test.c"], [], [None]),
        ("image-loader", ["tests/pc/image_loader_test.c", "src/pc/guest/image_loader.c"], [], [None]),
        (
            "translated-function-map",
            ["tests/pc/function_map_test.c"],
            [],
            [None],
        ),
        (
            "gte-native-guest-boundary",
            ["tests/pc/gte_test.c", "src/pc/compat/gte.c", "src/pc/compat/pgxp.c",
             "src/pc/guest/translated_runtime.c", "src/pc/memory.c"],
            ["-DMEMORIES_NATIVE_GTE", "-DMEMORIES_TRANSLATED"],
            [None],
        ),
        (
            "darwin-image",
            ["tests/pc/darwin_image_test.c", "src/pc/platform/darwin_image.c"],
            [],
            [None],
        ),
        (
            "translated-runtime",
            [
                "tests/pc/translated_runtime_test.c",
                "src/pc/guest/translated_runtime.c",
                "src/pc/memory.c",
            ],
            [],
            [None],
        ),
        (
            "translated-libc",
            [
                "tests/pc/translated_libc_test.c",
                "src/pc/guest/translated_libc.c",
                "src/pc/guest/translated_runtime.c",
                "src/pc/guest/state_io.c",
                "src/pc/memory.c",
            ],
            [],
            [None],
        ),
        (
            "state-native-memory",
            [
                "tests/pc/state_native_memory_test.c",
                "src/pc/guest/translated_state_memory.c",
                "src/pc/guest/translated_runtime.c",
                "src/pc/guest/state_io.c",
                "src/pc/memory.c",
            ],
            [],
            [None],
        ),
        (
            "state-io",
            ["tests/pc/state_io_test.c", "src/pc/guest/state_io.c"],
            [],
            [None],
        ),
        (
            "core",
            [
                "tests/pc/core_test.c",
                "src/pc/memory.c",
                "src/pc/rng.c",
                "src/game/rand_get_interval.c",
                "src/game/util_compare_s16.c",
            ],
            ["-Drand=Memories_Rand", "-Dsrand=Memories_Srand"],
            [None],
        ),
        (
            "game-files",
            ["tests/pc/game_files_test.c", "src/pc/platform/game_files.c"],
            [],
            "select cancel retry remembered moved headless override picker-failure malformed write-failure".split(),
        ),
        (
            "translated-image",
            [
                "tests/pc/translated_image_test.c",
                "src/pc/guest/translated_image.c",
                "src/pc/memory.c",
            ],
            [],
            [None],
        ),
        (
            "control-protocol",
            ["tests/pc/control_protocol_test.c", "src/pc/debug/control_protocol.c"],
            [],
            [None],
        ),
        (
            "symbols",
            ["tests/pc/symbols_test.c", "src/pc/debug/symbols.c", "src/pc/compat/fs.c"],
            [],
            [None],
        ),
        (
            "control-net",
            ["tests/pc/control_net_test.c", "src/pc/debug/control_net.c"],
            [],
            [None],
        ),
        (
            "state-remap",
            ["tests/pc/state_remap_test.c", "src/pc/guest/state_remap.c"],
            [],
            [None],
        ),
        (
            "entry-layout",
            ["tests/pc/entry_layout_test.c", "src/pc/text/entry_layout.c"],
            [],
            [None],
        ),
        (
            "deck-shop-native",
            ["tests/pc/deck_shop_test.c", "src/pc/saves/deck_shop.c"],
            [],
            [None],
        ),
        (
            "deck-shop-translated",
            ["tests/pc/deck_shop_test.c", "src/pc/saves/deck_shop.c"],
            ["-DMEMORIES_TRANSLATED"],
            [None],
        ),
        (
            "texture-clear-native",
            ["tests/pc/texture_dump_clear_test.c", "src/pc/render/texture_dump.c"],
            ["-Wno-strict-prototypes"],
            [None],
        ),
        (
            "texture-provenance-translated",
            [
                "tests/pc/texture_dump_guest_pointer_test.c",
                "src/pc/render/texture_dump.c",
            ],
            ["-DMEMORIES_TRANSLATED", "-Wno-strict-prototypes"],
            [None],
        ),
        (
            "libpress-contract-fast",
            ["tests/pc/libpress_test.c", "src/pc/sdk/libpress.c"],
            ["-O2", "-Wno-strict-prototypes", "-ffp-contract=fast"],
            [None],
        ),
        (
            "libpress-contract-off",
            ["tests/pc/libpress_test.c", "src/pc/sdk/libpress.c"],
            ["-O2", "-Wno-strict-prototypes", "-ffp-contract=off"],
            [None],
        ),
    ]
    count = 0
    for name, sources, extra, scenarios in cases:
        binary = out / name
        objects = []
        for index, source in enumerate(sources):
            obj = out / f"{name}-{index}.o"
            # Apply case-specific defines to all units; RNG aliases belong only
            # to the decompiled game unit in the core test.
            unit_flags = (
                extra
                if name != "core" or source == "src/game/rand_get_interval.c"
                else []
            )
            subprocess.run(
                [compiler, *flags, *unit_flags, "-c", source, "-o", str(obj)],
                check=True,
            )
            objects.append(str(obj))
        link_flags = ["-lm"] if name.startswith("libpress-") else []
        subprocess.run(
            [compiler, *flags, *objects, *link_flags, "-o", str(binary)], check=True
        )
        for scenario in scenarios:
            subprocess.run(
                [str(binary), *([] if scenario is None else [scenario])],
                check=True,
                env={**os.environ, "TMPDIR": str(out)},
            )
            count += 1
    count += darwin_program_dir_cases(out, compiler, flags)
    compiler_version = subprocess.run(
        [compiler, "--version"], check=True, text=True, stdout=subprocess.PIPE
    ).stdout.lower()
    if "clang" in compiler_version:
        ir = out / "libpress-contract.ll"
        subprocess.run(
            [
                compiler,
                "-O2",
                "-std=c11",
                "-ffp-contract=fast",
                "-Wno-strict-prototypes",
                "-Isrc",
                "-S",
                "-emit-llvm",
                "src/pc/sdk/libpress.c",
                "-o",
                str(ir),
            ],
            check=True,
        )
        contract = re.compile(
            r"^\s+%[^=]+ = f(?:add|sub|mul|div) contract\b", re.MULTILINE
        )
        if contract.search(ir.read_text(encoding="utf-8")):
            parser.error(
                "libpress IDCT allows floating-point contraction in optimized LLVM IR"
            )
        count += 1
    binary = out / "arm64-state-context"
    # Context restoration deliberately switches back to the caller's frame;
    # this assembly boundary is verified without sanitizer stack bookkeeping.
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-O2",
            "-Isrc",
            "tests/pc/state_arm64_context_test.c",
            "src/pc/guest/translated_state_arm64.S",
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
    count += 1
    binary = out / "arm64-game-jumps"
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-O2",
            "-Isrc",
            "tests/pc/translated_jmp_test.c",
            "src/pc/guest/translated_jmp.c",
            "src/pc/guest/translated_setjmp_arm64.S",
            "src/pc/guest/translated_state_arm64.S",
            "src/pc/guest/translated_runtime.c",
            "src/pc/guest/state_io.c",
            "src/pc/memory.c",
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
    count += 1
    binary = out / "native-state-lifecycle"
    state_flags = [
        *flags,
        "-Wno-language-extension-token",
        "-Wno-gnu-folding-constant",
        "-Wno-pointer-to-int-cast",
        "-DMEMORIES_PC",
        "-fms-extensions",
        "-isysroot",
        str(sdk_path()),
    ]
    state_objects = [
        guest_object(source, out / f"native-state-{index}.o", state_flags)
        for index, source in enumerate(
            ["tests/pc/state_translated_test.c", "src/pc/guest/state_translated.c"]
        )
    ]
    subprocess.run(
        [
            compiler,
            *flags,
            *state_objects,
            "src/pc/guest/state_io.c",
            "src/pc/guest/translated_runtime.c",
            "src/pc/memory.c",
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
    count += 1
    subprocess.run(
        [
            sys.executable,
            "tools/pc/test_translated_ir.py",
            "--case",
            "game-unit",
            *(["--sanitize"] if args.sanitize else []),
        ],
        check=True,
        env={**os.environ, "CC": compiler},
    )
    count += 1
    subprocess.run(
        [
            sys.executable,
            "tools/pc/test_arm64_mod_hooks.py",
            *(["--sanitize"] if args.sanitize else []),
        ],
        check=True,
        env={**os.environ, "CC": compiler},
    )
    count += 1
    print(f"{count} native component scenarios passed", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
