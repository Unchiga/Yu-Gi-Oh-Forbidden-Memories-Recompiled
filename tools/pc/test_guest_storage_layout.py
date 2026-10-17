#!/usr/bin/env python3
"""Check guest-width retail storage and backend-specific C-defined globals."""

import re
import subprocess

from build_config import FIXED_MEMORY, native_sources
from build_config import TARGETS as BUILD_TARGETS
from guest_test_compile import PS1_TARGETS as TARGETS
from llvm_guest import ROOT, toolchain

# ABI coverage only: the current Linux game builder targets i386. A 64-bit
# Linux host must not implicitly select the translated memory model either.
ABI_TARGETS = {
    **TARGETS,
    "linux-x64": ["--target=x86_64-pc-linux-gnu", "-DMEMORIES_PC"],
    "android-x86": ["--target=i686-linux-android", "-DMEMORIES_PC"],
}

SOURCE = r"""
#if defined(MEMORIES_PC) && (defined(__x86_64__) || defined(__aarch64__)) && !defined(MEMORIES_TRANSLATED)
/* Native fixed-memory 64-bit backends keep host pointers in these globals. */
#define GLOBAL_POINTER_SIZE sizeof(void *)
#else
#define GLOBAL_POINTER_SIZE 4
#endif
#include "game/main_modes.h"
#include "game/display_object_list_renderer_table.h"
#include "game/duel_effect_tables.h"
#include "game/display_effect_step_table.h"
#include "game/file_transfer.h"
#include "game/model_graphics_state.h"
#include "game/main_services.h"
#define ORDERING_TABLE_SLOT2_ARRAY
#include "game/ordering_tables.h"
_Static_assert(sizeof(D_800E9D90) == 16, "retail ordering-table pointer slots");
_Static_assert(sizeof(D_800E9D98[0]) == 4, "retail ordering-table slot alias");
_Static_assert(sizeof(D_800E9DB0) == 16, "retail frame-service callback slots");
_Static_assert(sizeof(D_8009B0B8) == 4, "retail extra frame-service callback");
_Static_assert(sizeof(GsOT) == 20, "retail ordering-table descriptor");
_Static_assert(sizeof(PSXLONG) == 4, "Psy-Q long width");
_Static_assert(sizeof(gMain_apfnModeRunner) == MAIN_MODE_COUNT * GLOBAL_POINTER_SIZE, "mode slots");
_Static_assert(sizeof(gDisplayObject_ListRenderers[0]) == GLOBAL_POINTER_SIZE, "renderer callback slot");
_Static_assert(sizeof(gDuelEffect_apfnStateHandler) == 5 * GLOBAL_POINTER_SIZE, "state slots");
_Static_assert(sizeof(D_80090F68[0]) == GLOBAL_POINTER_SIZE, "display callback slot");
_Static_assert(sizeof(D_8009AF18) == GLOBAL_POINTER_SIZE, "transfer descriptor token");
_Static_assert(sizeof(D_8009AF88) == GLOBAL_POINTER_SIZE, "model token");
_Static_assert(sizeof(((TextStreamOwner *)0)->streams[0]) == 4, "stream token");
void *native_pointer;
_Static_assert(sizeof(native_pointer) == sizeof(void *), "native pointer preserved");
"""

# This models native fixed-memory 64-bit compilation of callback tables. These
# globals are host-backed in the native backend, so their initializers must
# accept ordinary 64-bit function pointers rather than guest-width pointers.
NATIVE_CALLBACK_SOURCE = r"""
#include "game/main_modes.h"
#include "game/display_object_list_renderer_table.h"
_Static_assert(sizeof(gMain_apfnModeRunner[0]) == sizeof(void (*)(void)),
               "native mode callback width");
_Static_assert(sizeof(gDisplayObject_ListRenderers[0]) == sizeof(void (*)(void)),
               "native renderer callback width");
void NativeTestCallback(void) {}
MainModeRunner gMain_apfnModeRunner[MAIN_MODE_COUNT] = { NativeTestCallback };
void (*gDisplayObject_ListRenderers[DISPLAY_OBJECT_LIST_COUNT])(void) = {
    NativeTestCallback
};
"""


def check_mod_retail_slots(compiler, flags):
    # Probe the declarations from the actual mods: testing only the shared
    # headers would miss a private declaration reverting to native pointers.
    # A mod either declares the table itself (checked here) or includes the
    # game header that declares it (checked through that header).
    headers = {
        "D_800E9D90": "game/ordering_tables.h",
        "D_800E9DB0": "game/main_services.h",
    }
    mods = {
        "mods/3d-monsters/field_models.c": ("D_800E9D90", "D_800E9DB0"),
        "mods/3d-monsters/field_art.c": ("D_800E9D90",),
        "mods/hand-camera/hand_camera.c": ("D_800E9DB0",),
        "mods/yamyi-mods/menu_back_confirm.c": ("D_800E9D90",),
    }
    for path, symbols in mods.items():
        text = (ROOT / path).read_text()
        declarations = []
        for symbol in symbols:
            matches = re.findall(rf"^extern [^;\n]*\b{symbol}\b[^;\n]*;", text, re.MULTILINE)
            header = headers[symbol]
            included = re.search(rf'^#include "{re.escape(header)}"', text, re.MULTILINE)
            assert len(matches) == 1 or (not matches and included), (path, symbol, matches)
            declarations.append(matches[0] if matches else f'#include "{header}"')
            declarations.append(
                f'_Static_assert(sizeof({symbol}[0]) == 4, "{path}: {symbol} retail slot");'
            )
        check_source(
            compiler, flags,
            '#include "psyq/libgte.h"\n#include "psyq/libgpu.h"\n#include "psyq/libgs.h"\n'
            + "\n".join(declarations),
        )


def check_target_source_contract():
    assert set(BUILD_TARGETS) == {
        "linux",
        "windows",
        "windows-x64",
        "macos",
        "android-arm64-v8a",
        "android-arm64-translated",
        "android-x86",
    }
    fixed_targets = (
        "linux",
        "windows",
        "windows-x64",
        "android-arm64-v8a",
        "android-x86",
    )
    assert all(
        BUILD_TARGETS[name]["memory_model"] == "fixed-32" for name in fixed_targets
    )
    assert BUILD_TARGETS["macos"]["memory_model"] == "translated-32"

    for name in fixed_targets:
        sources = set(native_sources(BUILD_TARGETS[name], None))
        assert FIXED_MEMORY <= sources
        assert "src/pc/memory.c" not in sources
        assert "src/pc/guest/state_translated.c" not in sources
        assert not any(
            source.startswith("src/pc/guest/translated") for source in sources
        )

    android = set(native_sources(BUILD_TARGETS["android-arm64-v8a"], None))
    assert "src/pc/guest/state_aarch64.S" in android
    assert "src/pc/guest/setjmp_aarch64.S" in android
    assert "src/pc/platform/android_loader.c" not in android
    assert (
        BUILD_TARGETS["android-arm64-v8a"]["architecture"]
        == BUILD_TARGETS["macos"]["architecture"]
    )

    translated = set(native_sources(BUILD_TARGETS["macos"], None))
    assert "src/pc/memory.c" in translated
    assert "src/pc/guest/state_translated.c" in translated
    assert "src/pc/guest/translated_state_arm64.S" in translated
    assert not (FIXED_MEMORY & translated)
    assert "src/pc/guest/setjmp_x86_64.S" not in translated

    # Android arm64 on the translated backend: the macOS memory contract on
    # the same CPU, without the fixed-address loader.
    android_translated = BUILD_TARGETS["android-arm64-translated"]
    assert android_translated["memory_model"] == "translated-32"
    assert android_translated["architecture"] == BUILD_TARGETS["macos"]["architecture"]
    translated_android = set(native_sources(android_translated, None))
    assert "src/pc/memory.c" in translated_android
    assert "src/pc/guest/state_translated.c" in translated_android
    assert "src/pc/guest/translated_state_arm64.S" in translated_android
    assert not (FIXED_MEMORY & translated_android)
    assert "src/pc/guest/state_aarch64.S" not in translated_android
    assert "src/pc/platform/android_loader.c" not in translated_android

    try:
        native_sources("arm64", None)
    except ValueError as error:
        assert "complete PC target descriptor" in str(error)
    else:
        raise AssertionError(
            "legacy architecture string selected a memory model implicitly"
        )


def check_source(compiler, flags, source):
    subprocess.run(
        [
            compiler,
            *flags,
            "-ffreestanding",
            "-D_LANGUAGE_C",
            "-DLANGUAGE_C",
            "-fms-extensions",
            "-Isrc",
            "-std=c11",
            "-Werror",
            "-Wno-gnu-folding-constant",
            "-Wno-pointer-to-int-cast",
            "-fsyntax-only",
            "-x",
            "c",
            "-",
        ],
        input=source,
        text=True,
        cwd=ROOT,
        check=True,
        timeout=60,
    )


def main():
    check_target_source_contract()
    compiler = str(toolchain() / "bin/clang")
    for target, flags in ABI_TARGETS.items():
        if target == "macos":
            flags = [*flags, "-DMEMORIES_TRANSLATED"]
        check_source(compiler, flags, SOURCE)
        check_mod_retail_slots(compiler, flags)
        print(
            f"{target}: retail pointer slots are four bytes; C-defined globals match the backend"
        )

    for target in ("windows-x64", "android-arm64", "linux-x64"):
        check_source(compiler, ABI_TARGETS[target], NATIVE_CALLBACK_SOURCE)
        print(f"{target}: native callback tables accept static host-pointer initializers")


if __name__ == "__main__":
    main()
