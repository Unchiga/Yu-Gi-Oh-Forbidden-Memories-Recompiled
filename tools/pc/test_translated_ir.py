#!/usr/bin/env python3
"""Execute structured LLVM pointer, callback and fortified-libc contracts."""

import argparse
import subprocess

from build_arm64 import CHECKED_LIBC, HOST_LIBC, exclude_defined_storage
from build_mod import module_pins
from build_mod_arm64 import mod_pins
from guest_test_compile import run_fixture
from llvm_guest import ROOT, normalize, process, toolchain, translate
from macos_deps import sdk_path
from translate_guest_ir import address_map


def check_pc_defined_storage_pins():
    # These cursor symbols intentionally map to adjacent console bytes. The
    # PC storage is wider and must keep distinct native i16 globals instead.
    pc_storage = """
@gCardGrid_bCursorColumn = global i16 5, align 2
@gCardGrid_bCursorRow = global i16 7, align 2
define i16 @read_column() {
  %value = load i16, ptr @gCardGrid_bCursorColumn, align 2
  ret i16 %value
}
define i16 @read_row() {
  %value = load i16, ptr @gCardGrid_bCursorRow, align 2
  ret i16 %value
}
"""
    # Retail-backed data keeps its console address, including an initializer
    # containing a guest callback word; PC storage filtering must not sweep it.
    retail_storage = """
@retail_callback_slot = global i32 ptrtoint (ptr @retail_callback to i32)
declare void @retail_callback()
define i32 @read_retail_callback() {
  %value = load i32, ptr @retail_callback_slot
  ret i32 %value
}
"""
    external_storage = """
@retail_byte = external global i8
define i8 @read_retail_byte() {
  %value = load i8, ptr @retail_byte
  ret i8 %value
}
"""
    column, row, callback_slot, retail_byte = (
        0x8009B258,
        0x8009B259,
        0x8009B260,
        0x8009B270,
    )
    pins = {
        "resident": {
            "gCardGrid_bCursorColumn": column,
            "gCardGrid_bCursorRow": row,
            "retail_callback_slot": callback_slot,
            "retail_byte": retail_byte,
        },
        "native": {
            "gCardGrid_bCursorColumn": column,
            "gCardGrid_bCursorRow": row,
            "retail_callback_slot": callback_slot,
            "retail_byte": retail_byte,
        },
    }
    # Match build_arm64: only IR from src/pc/game/*.c is offered for exclusion.
    filtered = exclude_defined_storage(pins, [pc_storage])
    for group in filtered:
        assert "gCardGrid_bCursorColumn" not in filtered[group]
        assert "gCardGrid_bCursorRow" not in filtered[group]
        assert filtered[group]["retail_callback_slot"] == callback_slot
        assert filtered[group]["retail_byte"] == retail_byte

    translated_pc = translate(pc_storage, filtered["resident"], game_unit=True)
    assert "@gCardGrid_bCursorColumn = global i16 5, align 2" in translated_pc
    assert "@gCardGrid_bCursorRow = global i16 7, align 2" in translated_pc
    assert "load i16, ptr @gCardGrid_bCursorColumn" in translated_pc
    assert "load i16, ptr @gCardGrid_bCursorRow" in translated_pc
    assert "call ptr @GuestRuntime_ResolveData" not in translated_pc

    translated_retail = translate(retail_storage, filtered["resident"], game_unit=True)
    assert "@retail_callback_slot =" not in translated_retail
    assert f"inttoptr i64 {callback_slot}" in translated_retail
    translated_external = translate(
        external_storage, filtered["resident"], game_unit=True
    )
    assert "@retail_byte = external global i8" not in translated_external
    assert f"inttoptr i64 {retail_byte}" in translated_external
    assert "GuestRuntime_ResolveData" in translated_external


def check_mod_pc_storage_imports():
    cursor, stats = "gCardGrid_bCursorColumn", "gDuel_adwCardStats"
    summary = {"pc_storage_globals": [cursor, stats]}
    pins = mod_pins(
        summary, [{cursor: 0x8009B258, stats: 0x800E8C00, "retail_global": 0x80010000}]
    )
    assert cursor not in pins and stats not in pins
    assert pins["retail_global"] == 0x80010000
    try:
        mod_pins({}, [{cursor: 0x8009B258}])
    except SystemExit as error:
        assert "pc_storage_globals" in str(error)
    else:
        raise AssertionError(
            "mod pins accepted a game summary without PC storage metadata"
        )

    imported_storage = f"""
@{cursor} = external global i16
@{stats} = external global [1024 x i32]
define i16 @read_cursor() {{
  %value = load i16, ptr @{cursor}, align 2
  ret i16 %value
}}
define i32 @read_card_attack() {{
  %value = load i32, ptr @{stats}, align 4
  ret i32 %value
}}
"""
    translated = process(
        "translate",
        imported_storage,
        pins=pins,
        registration="MemoriesModRegisterUnit_test",
        mod_unit=True,
    )
    assert f"@{cursor} = external global i16" in translated
    assert f"@{stats} = external global [1024 x i32]" in translated


def check_module_data_pins():
    # A model module's data name (mod API 29) the mod only declares reads the
    # record where the game loaded it: pinned to its own address, as the
    # game's data is, whether or not the game pins it. One the mod defines
    # (a const copy of bytes inside the hashed span) stays the mod's own,
    # even where the game's table pins that name.
    table, own, pinned = "D_8013FFF0", "D_8013BAE4", "D_8013C6D4"
    unit = f"""
@{table} = external global [0 x i8], align 1
@{own} = internal constant [4 x i32] [i32 1, i32 2, i32 3, i32 4], align 4
@{pinned} = external global [0 x i8], align 1
define i8 @read_table() {{
  %value = load i8, ptr @{table}, align 1
  ret i8 %value
}}
define i32 @read_own() {{
  %value = load i32, ptr @{own}, align 4
  ret i32 %value
}}
define i8 @read_pinned() {{
  %value = load i8, ptr @{pinned}, align 1
  ret i8 %value
}}
"""
    game = {own: 0x8013BAE4, pinned: 0x8013C6D4, "retail_global": 0x80010000}
    pins = module_pins(unit, game)
    assert pins[table] == 0x8013FFF0 and pins[pinned] == 0x8013C6D4
    assert own not in pins and pins["retail_global"] == 0x80010000
    assert module_pins("define void @f() {\n  ret void\n}\n", game) is game
    translated = process(
        "translate",
        unit,
        pins=pins,
        registration="MemoriesModRegisterUnit_test",
        mod_unit=True,
    )
    assert f"@{table} =" not in translated and f"inttoptr i64 {0x8013FFF0}" in translated
    assert f"@{pinned} =" not in translated and f"inttoptr i64 {0x8013C6D4}" in translated
    assert f"@{own} = internal constant" in translated


def run_case(case, args):
    out = ROOT / "tmp/translated-game-ir" / case
    out.mkdir(parents=True, exist_ok=True)
    compiler = str(toolchain() / "bin/clang")
    flags = [
        "-std=c11",
        "-DMEMORIES_PC",
        "-DMEMORIES_TRANSLATED",
        "-DTRANSLATED_GAME_IR",
        "-fms-extensions",
        "-Wno-pointer-to-int-cast",
        "-Wno-gnu-folding-constant",
        "-Isrc",
        "-isysroot",
        str(sdk_path()),
    ]
    source = ROOT / (
        "tests/pc/translated_ir_probe.c"
        if case == "ir"
        else "src/game/duel_effect_basic_commands.c"
    )
    original = source.read_bytes()
    raw = out / "probe.raw.ll"
    subprocess.run(
        [compiler, *flags, "-O0", "-S", "-emit-llvm", str(source), "-o", str(raw)],
        cwd=ROOT,
        check=True,
        timeout=60,
    )
    boundaries = normalize(
        raw.read_text(),
        {name: "GuestRuntime_" + name for name in CHECKED_LIBC | HOST_LIBC},
    )
    pins = (
        {"pinned_record": 0x80018000, "callback_table": 0x80012600}
        if case == "ir"
        else address_map(ROOT / "config/pc/guest_addresses.txt")
    )
    transformed = translate(boundaries, pins)
    unit = out / "probe.ll"
    unit.write_text(transformed)
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    fixture_source = (
        "translated_ir_contract_test.c"
        if case == "ir"
        else "translated_game_unit_test.c"
    )
    run_fixture(
        out,
        [unit, "tests/pc/" + fixture_source],
        [
            "src/pc/memory.c",
            "src/pc/guest/translated_runtime.c",
            "src/pc/guest/translated_libc.c",
            "src/pc/guest/state_io.c",
        ],
        [*flags, "-O2" if args.optimize or case == "game-unit" else "-O0"],
        ["-Wl,-dead_strip"],
    )
    assert source.read_bytes() == original, "source must remain unchanged"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--optimize", action="store_true")
    parser.add_argument("--case", choices=("all", "ir", "game-unit"), default="all")
    args = parser.parse_args()
    if args.case in ("all", "ir"):
        check_pc_defined_storage_pins()
        check_mod_pc_storage_imports()
        check_module_data_pins()
    for case in ("ir", "game-unit") if args.case == "all" else (args.case,):
        run_case(case, args)


if __name__ == "__main__":
    main()
