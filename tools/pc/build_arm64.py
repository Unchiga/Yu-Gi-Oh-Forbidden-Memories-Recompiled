#!/usr/bin/env python3
"""macOS ARM64 driver for the common PC build.

The driver emits IR under tmp, compiles every resident and configured
module unit, and links native SDK/platform services. Missing game functions use
existing fatal Memories_Unimplemented diagnostics, never success placeholders.

O2 code generation and audited native SoftGpu boundaries are enabled by default.
The guest frontend remains O0 before translation; --no-optimize restores the
fully instrumented O0 diagnostic build, and --instrument-softgpu retains O2
while restoring per-access raster instrumentation for comparisons.
"""

import argparse
import concurrent.futures
import csv
import hashlib
import json
import os
import re
import shutil
import subprocess
from pathlib import Path

from build_config import (
    GATED_MODULES,
    MODULE_CONFIG,
    MODULES,
    TARGETS,
    game_sources,
    native_sources,
)
from direct_overlay_bridges import ENTRIES as DIRECT_OVERLAY_ENTRIES
from direct_overlay_bridges import emit_bridge, validate_signature
from guest_build import (
    CHECKED_LIBC,
    HOST_LIBC,
    darwin_flags,
    guest_frontend_flags,
    maps,
)
from llvm_guest import TranslationError, inspect, normalize, toolchain, translate
from native_call_marshalling import emit_native_calls

SOFT_GPU = "src/pc/render/soft_gpu.c"
# Renderer caches and decoded PNGs never enter 32-bit guest storage.
# Keep their LP64 pointers and lifetimes on the host, outside the token arena.
# State restoration may replace guest heaps, but must not free live GL buffers.
HOST_HEAP_UNITS = {"src/pc/render/texture_pack.c", "src/pc/render/gl_picture.c"}


ROOT = Path(__file__).resolve().parents[2]
# Set by main(): None on macOS; the NDK's bin folder for Android arm64
# (--target android-arm64-translated).
ANDROID_BIN = None
NM = "nm"


def c_symbol(name):
    """An object-file symbol's C name: Mach-O prefixes an underscore, ELF not."""
    if ANDROID_BIN:
        return name
    return name[1:] if name.startswith("_") else None


ORDINARY = {
    "src/pc/memory.c",
    "src/pc/guest/state_translated.c",
    "src/pc/guest/state_requests.c",
    "src/pc/guest/image_loader.c",
    "src/pc/mods/hooks.c",
    "src/pc/mods/object_loader.c",
    # The crash-report table: host memory of its own, never a registered
    # heap region, so a save state neither holds nor replaces it.
    "src/pc/debug/symbols.c",
}


def definitions(ir):
    return set(inspect(ir)["definitions"])


def replace_names(ir, renames):
    return normalize(ir, renames)


def drop_functions(ir, names):
    return normalize(ir, drop_definitions=names)


def exclude_defined_storage(pin_maps, unit_ir):
    """Keep PC-defined storage separate from retail guest address pins."""
    defined_globals = set().union(*(set(inspect(text)["globals"]) for text in unit_ir))
    return {
        group: {
            symbol: address
            for symbol, address in pins.items()
            if symbol not in defined_globals
        }
        for group, pins in pin_maps.items()
    }


def object_arguments(build, objects):
    """Objects as one response file where a command line is short (Windows)."""
    if os.name != "nt":
        return objects
    listing = build / "objects.rsp"
    listing.write_text("\n".join(path.replace("\\", "/") for path in objects) + "\n")
    return ["@" + str(listing)]


def run_logged(command, log):
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    log.write_text(result.stdout + result.stderr)
    return result.returncode


def unit_path(build, kind, source, suffix):
    return build / kind / (source.replace("/", "_") + suffix)


def compile_sources(jobs, ordinary, path_for, cc, flags, game_extra, args):
    """Emit unoptimized guest IR and compile ordinary host units directly."""
    newest = max(p.stat().st_mtime for p in (ROOT / "src").rglob("*.h"))

    def emit(job):
        source, group = job
        raw = path_for("raw", source, ".ll")
        obj = path_for("obj", source, ".o")
        extra = game_extra if group != "native" else []
        if source == "src/pc/compat/gte.c" and source in ordinary:
            extra = [*extra, "-DMEMORIES_NATIVE_GTE"]
        emit_flags = [
            ("-O2" if args.optimize and source in ordinary and flag == "-O0" else flag)
            for flag in flags
        ]
        command = [
            cc,
            *emit_flags,
            *extra,
            "-c" if source in ordinary else "-S",
            *([] if source in ordinary else ["-emit-llvm"]),
            source,
            "-o",
            str(obj if source in ordinary else raw),
        ]
        stamp = path_for("raw", source, ".flags")
        fingerprint = json.dumps(command)
        target = obj if source in ordinary else raw
        if (
            target.exists()
            and stamp.exists()
            and stamp.read_text() == fingerprint
            and target.stat().st_mtime >= max(newest, (ROOT / source).stat().st_mtime)
        ):
            return source, group, 0
        status = run_logged(command, path_for("logs", source, ".emit.log"))
        if not status:
            stamp.write_text(fingerprint)
        return source, group, status

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        emitted = list(pool.map(emit, jobs))
    failures = [s for s, g, status in emitted if status]
    if failures:
        print("IR failures:", len(failures))
        print("\n".join(failures))
    return failures


def prepare_guest_units(jobs, groups, natives, ordinary, path_for, address_tables):
    """Resolve overlays, native overrides, data pins and libc boundaries."""
    raw_text = {
        s: replace_names(path_for("raw", s, ".ll").read_text(), {})
        for s, g in jobs
        if s not in ordinary
    }
    host_renames = dict(
        line.split()
        for line in (ROOT / "config/pc/host_symbol_renames.txt")
        .read_text()
        .splitlines()
        if line.strip()
    )
    for g, sources in groups.items():
        for s in sources:
            raw_text[s] = replace_names(raw_text[s], host_renames)
    native_defs = set().union(
        *(definitions(raw_text[s]) for s in natives if s not in ordinary)
    )
    for s in ordinary:
        result = subprocess.run(
            [NM, "-g", str(path_for("obj", s, ".o"))], capture_output=True, text=True
        )
        native_defs |= {
            c_symbol(line.split()[-1])
            for line in result.stdout.splitlines()
            if len(line.split()) == 3 and line.split()[1] != "U"
        } - {None}
    group_defs = {
        g: set().union(
            *(
                definitions(raw_text[s]) | set(inspect(raw_text[s])["globals"])
                for s in sources
            )
        )
        for g, sources in groups.items()
    }
    renames = {}
    for name, pattern, identifier, bank in MODULES:
        if not bank:
            renames[name] = {}
            continue
        others = set().union(
            *(symbols for g, symbols in group_defs.items() if g != name)
        )
        clashes = group_defs[name] & others
        if name in GATED_MODULES:
            clashes |= group_defs[name]
        renames[name] = {symbol: name + "__" + symbol for symbol in clashes}
        for s in groups[name]:
            raw_text[s] = replace_names(raw_text[s], renames[name])
    # Resident callers enter the existing retail-verified credits gate, just
    # as upstream's fixed branch table does. The module's own definitions
    # retain their prefixed names for the gate's native dispatch.
    credits_imports = {
        "func_801807B0": "Memories_CreditsInit",
        "func_80180A24": "Memories_CreditsUpdate",
        "func_80181C4C": "Memories_CreditsLines",
    }
    for group, sources in groups.items():
        if group != "credits":
            for source in sources:
                raw_text[source] = replace_names(raw_text[source], credits_imports)
    pin_maps = {}
    for group in groups:
        pins = dict(address_tables["SLUS_014.11"])
        pins.update(address_tables.get("main_menu", {}))
        if group != "resident":
            pins.update(
                {
                    renames.get(group, {}).get(k, k): v
                    for k, v in address_tables.get(group, {}).items()
                }
            )
        pin_maps[group] = pins
    pin_maps["native"] = {
        **address_tables["SLUS_014.11"],
        **address_tables.get("main_menu", {}),
    }
    for table in address_tables.values():
        for symbol, address in table.items():
            pin_maps["native"].setdefault(symbol, address)
    for s, g in jobs:
        if s not in ordinary and g != "native":
            raw_text[s] = drop_functions(raw_text[s], native_defs)
    linked_defs = native_defs | set().union(
        *(definitions(raw_text[s]) for g, ss in groups.items() for s in ss)
    )
    # Retail image data remains guest-backed, including its narrow pointer
    # initializers. Upstream's PC game storage replaces/enlarges those tables
    # and widens adjacent byte globals: keep these definitions native rather
    # than overlapping them at their old retail addresses.
    pc_storage_ir = [
        text for source, text in raw_text.items() if source.startswith("src/pc/game/")
    ]
    pc_storage_globals = sorted(
        set().union(*(set(inspect(text)["globals"]) for text in pc_storage_ir))
    )
    pin_maps = exclude_defined_storage(pin_maps, pc_storage_ir)
    rows = list(csv.DictReader((ROOT / "config/slus_01411/functions.csv").open()))
    for row in rows:
        row["name"] = host_renames.get(row["name"], row["name"])
    for name, _, identifier, bank in MODULES:
        for row in csv.DictReader(
            (
                ROOT
                / f"config/slus_01411/overlays/{MODULE_CONFIG.get(name, name)}_functions.csv"
            ).open()
        ):
            row["name"] = host_renames.get(row["name"], row["name"])
            row["name"] = renames.get(name, {}).get(row["name"], row["name"])
            row["bank"] = bank
            row["identifier"] = identifier if bank else 0
            if name not in GATED_MODULES:
                rows.append(row)
    by_address = {
        int(row["address"], 16): row["name"] for row in rows if not row.get("bank")
    }
    aliases = {}
    for s, g in jobs:
        if s in ordinary:
            continue
        declarations = set(inspect(raw_text[s])["declarations"])
        for symbol in declarations - linked_defs:
            if g == "native" and symbol in host_renames:
                continue
            address = pin_maps[g].get(symbol)
            if address is None and re.fullmatch(r"func_[89A-Fa-f0-9]{8}", symbol):
                address = int(symbol[5:], 16)
            target = by_address.get(address)
            if target and target != symbol and target in linked_defs:
                aliases[symbol] = target
        raw_text[s] = replace_names(raw_text[s], aliases)
    checked_seen = set().union(
        *(
            set(inspect(text)["declarations"]) & CHECKED_LIBC
            for text in raw_text.values()
        )
    )
    missing_checked = sorted(
        name for name in checked_seen if "GuestRuntime_" + name not in native_defs
    )
    if missing_checked:
        raise SystemExit(
            "Fortified native pointer bridges required: " + ", ".join(missing_checked)
        )
    libc_renames = {
        name: "GuestRuntime_" + name
        for name in HOST_LIBC | {"malloc", "calloc", "realloc", "free"} | checked_seen
    }

    for source in raw_text:
        renames_for_unit = {
            name: target
            for name, target in libc_renames.items()
            if source not in HOST_HEAP_UNITS
            or name not in {"malloc", "calloc", "realloc", "free"}
        }
        raw_text[source] = replace_names(raw_text[source], renames_for_unit)
    return (
        raw_text,
        pin_maps,
        pc_storage_globals,
        aliases,
        renames,
        rows,
        linked_defs,
        host_renames,
        native_defs,
    )


def compile_guest_units(jobs, ordinary, raw_text, pin_maps, path_for, cc, sdk, args):
    """Lower guest accesses before O2 code generation; register in source order."""
    # Token assignment follows source order, independent of worker completion.
    registrations = {
        source: "GuestRuntime_RegisterUnit_"
        + hashlib.sha256(source.encode()).hexdigest()[:16]
        for source, group in jobs
        if source not in ordinary
    }

    def adapt(job):
        source, group = job
        if source in ordinary:
            return source, 0
        ir = path_for("ir", source, ".ll")
        obj = path_for("obj", source, ".o")
        try:
            routine = registrations[source]
            text = translate(
                raw_text[source],
                pin_maps[group],
                routine,
                game_unit=group != "native",
                hooks=group != "native" or "/overrides/" in source,
            )
        except TranslationError as error:
            path_for("logs", source, ".transform.log").write_text(str(error))
            return source, 1
        if not ir.exists() or ir.read_text() != text:
            ir.write_text(text)
        if obj.exists() and obj.stat().st_mtime >= max(
            ir.stat().st_mtime, Path(__file__).stat().st_mtime
        ):
            return source, 0
        return source, run_logged(
            [
                cc,
                *darwin_flags(sdk),
                "-O2" if args.optimize else "-O0",
                "-fno-strict-aliasing",
                "-c",
                str(ir),
                "-o",
                str(obj),
            ],
            path_for("logs", source, ".object.log"),
        )

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        adapted = list(pool.map(adapt, jobs))
    failures = [s for s, status in adapted if status]
    return registrations, failures


def generate_function_tables(
    raw_text, linked_defs, host_renames, rows, pin_maps, registrations, revision
):
    """Emit typed bridges, dispatch maps and deterministic global registration."""
    all_declarations = set().union(
        *(set(inspect(text)["declarations"]) for text in raw_text.values())
    )
    known_game = {row["name"] for row in rows}
    stub_names = (
        known_game
        | set(host_renames.values())
        | {
            name
            for name in all_declarations
            if re.fullmatch(r"func_[89A-Fa-f0-9]{8}", name)
        }
    )
    # Legacy direct name for the shared-bank name-entry entry point. Its
    # native implementation belongs to the active password/name-entry module,
    # so never register the bridge itself as a resident function.
    direct_overlay_bridges = {
        name: details[0] for name, details in DIRECT_OVERLAY_ENTRIES.items()
    }
    direct_overlay_bridges = {
        name: address
        for name, address in direct_overlay_bridges.items()
        if name in all_declarations and name not in linked_defs
    }
    # Validate every importing unit, since the linker cannot detect a call
    # whose LLVM function type disagrees with the generated C bridge.
    for text in raw_text.values():
        for name, signature in inspect(text)["signatures"].items():
            if name in direct_overlay_bridges:
                validate_signature(name, signature, aapcs64=ANDROID_BIN is not None)
    stubs = sorted(
        (
            all_declarations
            - linked_defs
            - HOST_LIBC
            - set(host_renames)
            - set(direct_overlay_bridges)
        )
        & stub_names
    )
    if set(stubs) & set(host_renames):
        raise SystemExit("Refusing to override native libc with guest stubs")
    bridges = [
        (0x8013A004, "Memories_ModelPrimaryControlA"),
        (0x8013B004, "Memories_ModelVariantControlA"),
        (0x801462B0, "Memories_DuelEffectControl"),
        (0x801807B0, "Memories_CreditsInit"),
        (0x80180A24, "Memories_CreditsUpdate"),
        (0x80181C4C, "Memories_CreditsLines"),
        (0x8017A004, "Memories_ModelPrimaryControlB"),
        (0x8017B004, "Memories_ModelVariantControlB"),
    ]
    mapped = [
        (int(r["address"], 16), r["name"], r.get("bank", 0), r.get("identifier", 0))
        for r in rows
        if r["name"] in linked_defs | set(stubs)
    ]
    mapped += [(a, n, 0, 0) for a, n in bridges if n in linked_defs]
    # The typed effect bridge includes the request header, which already
    # declares these functions. Do not redeclare them with erased signatures.
    typed_declarations = (
        {
            "DuelEffect_AllocateRequest",
            "DuelEffect_CreateRequest",
            "DuelEffect_FindFreeRequest",
            "DuelEffect_UpdateRequests",
            "Memories_DuelEffectControl",
        }
        if "func_801462B0" in direct_overlay_bridges
        else set()
    )
    declarations = "".join(
        f"extern void {name}(void);\n"
        for name in sorted({r[1] for r in mapped} - set(stubs) - typed_declarations)
    )
    tables = '#include "pc/guest/image.h"\n#include "pc/mods/exports.h"\n#include "pc/guest/translated_runtime.h"\n#include <stdint.h>\n'
    tables += "".join(emit_bridge(name) for name in direct_overlay_bridges)
    tables += "".join(f"extern void {name}(void);\n" for name in registrations.values())
    tables += (
        "void GuestRuntime_RegisterAllGlobals(void) {\n"
        + "".join(f"  {name}();\n" for name in registrations.values())
        + "}\n"
    )
    tables += (
        "const unsigned Memories_GameFingerprint = 0x"
        + hashlib.sha256("".join(raw_text.values()).encode()).hexdigest()[:8]
        + "u;\n"
    )
    from build_game32 import release_version

    tables += f"const char Memories_Version[] = {json.dumps(release_version())};\n"
    build_label = os.environ.get("MEMORIES_VERSION") or "dev-" + revision[:12]
    tables += f"const char Memories_BuildIdentity[] = {json.dumps(build_label + ' (' + revision + ')')};\n"
    tables += (
        "".join(f'void {n}(void) {{ Memories_Unimplemented("{n}"); }}\n' for n in stubs)
        + declarations
    )
    tables += (
        "const MemoriesGuestFunction Memories_FunctionMap[] = {\n"
        + "".join(
            f"{{0x{a:08x}u, {n}, 0x{int(b):08x}u, 0x{int(i):x}u}},\n"
            for a, n, b, i in sorted(mapped)
        )
        + "};\n"
    )
    tables += f"const unsigned Memories_FunctionMapCount = {len(mapped)};\n"
    tables += emit_native_calls(mapped, raw_text.values(), stubs)
    shared = [(n, i, b) for n, _, i, b in MODULES if b and n not in GATED_MODULES]
    tables += (
        "const MemoriesModule Memories_Modules[] = {\n"
        + "".join(f'{{"{n}",0x{b:08x}u,0x{i:x}u,0,0,0,0}},\n' for n, i, b in shared)
        + "};\n"
    )
    tables += f"const unsigned Memories_ModuleCount = {len(shared)};\n"
    return tables


def generate_mod_exports(
    build, jobs, path_for, raw_text, aliases, linked_defs, pin_maps, cc, flags
):
    """Publish symbols and LLVM signatures for the separate mod build."""
    defined_symbols = subprocess.check_output(
        [NM, "-gU", *object_arguments(build, [str(path_for("obj", source, ".o")) for source, group in jobs])],
        text=True,
    )
    public_symbols = {
        c_symbol(parts[-1])
        for line in defined_symbols.splitlines()
        if len(parts := line.split()) == 3 and c_symbol(parts[-1])
    }
    exports = {
        name: name
        for name in public_symbols
        if not name.startswith(("GuestRuntime_RegisterUnit_", "Memories_Unhooked_"))
    }
    exports.update(
        {alias: target for alias, target in aliases.items() if target in linked_defs}
    )
    symbols = sorted(set(exports.values()))
    export_table = "#include <stdint.h>\ntypedef struct { const char *name; void *address; } MemoriesModExport;\n"
    prefix = "" if ANDROID_BIN else "_"  # ELF names have no Mach-O underscore
    for index, name in enumerate(symbols):
        export_table += f'extern char ModExport_{index}[] __asm__("{prefix}{name}");\n'
    references = {name: f"ModExport_{index}" for index, name in enumerate(symbols)}
    entries = {name: references[target] for name, target in exports.items()}
    for name, address in pin_maps["native"].items():
        if name not in entries:
            entries[name] = f"(void *)(uintptr_t)0x{address:08x}u"
    export_table += (
        "const MemoriesModExport Memories_ModExports[] = {\n"
        + "".join(
            # Indented as build_game32.py's table: build_mod.provided reads it.
            f'    {{"{name}", {value}}},\n' for name, value in sorted(entries.items())
        )
        + "};\n"
    )
    export_table += f"const unsigned Memories_ModExportCount = {len(entries)};\n"
    export_source = build / "mod_exports.c"
    export_source.write_text(export_table)
    export_object = build / "mod_exports.o"
    if run_logged(
        [cc, *flags, "-c", str(export_source), "-o", str(export_object)],
        build / "logs/mod_exports.log",
    ):
        raise SystemExit(
            "Mod exports compilation failed: " + str(build / "logs/mod_exports.log")
        )
    (build / "exports.txt").write_text("\n".join(sorted(entries)) + "\n")
    signatures = {}
    for text in raw_text.values():
        for name, signature in inspect(text)["signatures"].items():
            if signature["definition"]:
                signatures[name] = signature
    (build / "mod_signatures.json").write_text(json.dumps(signatures, indent=2) + "\n")
    return export_object


def write_mod_translation(build, address_tables):
    """mod_translation.json: what build_mod.py --target aarch64-translated
    puts a mod through the guest pass with, as build_mod_arm64.py does for
    macOS from this folder: the names it renames (the game's aliases, the
    host renames, rand to the mods' own generator, the C library to the
    runtime's guest-pointer wrappers), the guest addresses of the game's
    pinned data, and the pointer-free math the C library may lend
    directly. With mod_signatures.json and exports.txt it is all a mod build
    needs of the game; the SDK carries the three (build_game32.write_sdk)."""
    from build_mod_arm64 import SYSTEM_MATH_IMPORTS, mod_pins

    summary = json.loads((build / "summary.json").read_text())
    aliases = dict(summary["aliases"])
    aliases.update(
        dict(
            line.split()
            for line in (ROOT / "config/pc/host_symbol_renames.txt").read_text().splitlines()
            if line.strip()
        )
    )
    aliases.update(rand="Mods_ModRand", srand="Mods_ModSrand")
    runtime = sorted(HOST_LIBC | CHECKED_LIBC | {"malloc", "calloc", "realloc", "free"})
    table = {
        "format": 1,
        "game_fingerprint": (build / "build-fingerprint.txt").read_text().strip(),
        "aliases": aliases,
        "runtime": {name: "GuestRuntime_" + name for name in runtime},
        "pins": mod_pins(summary, address_tables.values()),
        # abs and labs too: a mod is compiled freestanding, where clang
        # calls them instead of expanding them as macOS's builtins.
        "system_math": sorted(SYSTEM_MATH_IMPORTS | {"abs", "labs"}),
    }
    (build / "mod_translation.json").write_text(json.dumps(table, indent=1, sort_keys=True) + "\n")


def link_game(
    build, jobs, path_for, tables, export_object, cc, sdk, flags, dependencies
):
    """Compile generated tables and link the native executable."""
    generated_tables = build / "tables.c"
    generated_tables.write_text(tables)
    tables_obj = build / "tables.o"
    status = run_logged(
        [cc, *flags, "-c", str(generated_tables), "-o", str(tables_obj)],
        build / "logs/tables.log",
    )
    if status:
        raise SystemExit("Table compilation failed: " + str(build / "logs/tables.log"))
    if ANDROID_BIN:
        # As build_game32.py links the fixed Android game, minus the fixed
        # image base: the translated game loads wherever the system puts it.
        command = [
            cc, *darwin_flags(None), "-shared", "-o", str(build / "libgame.so"),
            "-Wl,-Bsymbolic", "-Wl,--no-undefined", "-Wl,--wrap=exit", "-Wl,-soname,libgame.so",
            "-Wl,-z,noexecstack", "-Wl,--export-dynamic",
            # The state identity (translated_state_backend.c) is the GNU build ID.
            "-Wl,--build-id=sha1",
            *object_arguments(build, [str(path_for("obj", s, ".o")) for s, g in jobs]),
            str(tables_obj), str(export_object),
            f"-L{dependencies / 'lib'}", "-lSDL3", str(dependencies / "lib/libfreetype.a"),
            str(dependencies / "lib/libpng16.a"), "-lz", "-lGLESv2", "-llog", "-landroid", "-lm", "-ldl",
        ]
        (build / "link-command.json").write_text(json.dumps(command, indent=2) + "\n")
        status = run_logged(command, build / "logs/link.log")
        print("Link " + ("failed: " + str(build / "logs/link.log") if status
                         else "succeeded: " + str(build / "libgame.so")), flush=True)
        return status
    command = [
        cc,
        *darwin_flags(sdk),
        *[str(path_for("obj", s, ".o")) for s, g in jobs],
        str(tables_obj),
        str(export_object),
        *[
            str(dependencies / "lib" / name)
            for name in ["libfreetype.a", "libpng16.a", "libz.a", "libSDL3.a"]
        ],
        "-liconv",
        "-framework",
        "Cocoa",
        "-framework",
        "IOKit",
        "-framework",
        "CoreVideo",
        "-framework",
        "CoreAudio",
        "-framework",
        "AudioToolbox",
        "-framework",
        "Metal",
        "-framework",
        "QuartzCore",
        "-framework",
        "GameController",
        "-framework",
        "UniformTypeIdentifiers",
        "-framework",
        "CoreMedia",
        "-framework",
        "AVFoundation",
        "-framework",
        "OpenGL",
        "-framework",
        "Foundation",
        "-framework",
        "CoreText",
        "-framework",
        "ForceFeedback",
        "-framework",
        "Carbon",
        "-framework",
        "CoreHaptics",
        "-Wl,-export_dynamic",
        "-o",
        str(build / "memories-arm64"),
    ]
    (build / "link-command.json").write_text(json.dumps(command, indent=2) + "\n")
    status = run_logged(command, build / "logs/link.log")
    print(
        "Link "
        + (
            "failed: " + str(build / "logs/link.log")
            if status
            else "succeeded: " + str(build / "memories-arm64")
        ),
        flush=True,
    )
    return status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("macos", "android-arm64-translated"), default="macos")
    parser.add_argument("--build", type=Path)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--compile-only", action="store_true")
    parser.add_argument(
        "--release",
        action="store_true",
        help="android-arm64-translated: leave out the test hooks (MEMORIES_TEST_HOOKS), as "
             "build_game32.py --release does",
    )
    parser.add_argument(
        "--instrument-softgpu",
        action="store_true",
        help="Keep full SoftGpu instrumentation for optimized A/B comparisons",
    )
    parser.add_argument(
        "--optimize",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Enable O2 translated IR/native helpers and audited SoftGpu boundaries (default); frontend remains O0",
    )
    args = parser.parse_args()
    # build_game32 (imported for release_version) takes its own --target
    # from sys.argv when it is imported: it must not see this one.
    import sys

    sys.argv = [sys.argv[0]]
    global ANDROID_BIN, NM
    llvm = toolchain()
    if args.target == "android-arm64-translated":
        import build_android_deps

        ANDROID_BIN = Path(build_android_deps.llvm_bin())
        cc = str(ANDROID_BIN / "clang")
        NM = str(ANDROID_BIN / "llvm-nm")
        dependencies = ROOT / "tmp/pc/android-deps/arm64-v8a"
        sdk = None
    else:
        cc = str(llvm / "bin/clang")
        from macos_deps import ensure, sdk_path

        dependencies = ensure(jobs=args.jobs)
        sdk = str(sdk_path())
    build = (args.build or ROOT / TARGETS[args.target]["build"]).resolve()
    for d in ["raw", "ir", "obj", "logs"]:
        (build / d).mkdir(parents=True, exist_ok=True)
    address_tables = maps()
    groups = game_sources()
    target = TARGETS[args.target]
    natives = native_sources(target)
    if ANDROID_BIN:
        # GLES has no desktop GL presenter (build_game32.py ANDROID_BACKEND);
        # the loader is libmain.so's own unit.
        natives = sorted(
            {"src/pc/render/gl_desktop_none.c" if s == "src/pc/render/present_pass.c" else s
             for s in natives}
        )
    required = {
        "src/pc/guest/translated_image_backend.c",
        "src/pc/guest/state_translated.c",
        "src/pc/guest/translated_runtime.c",
    }
    if required - set(natives):
        raise SystemExit(
            "Native backend files missing: " + str(required - set(natives))
        )
    ordinary = {s for s in natives if s in ORDINARY or "/translated_" in s}
    if args.optimize and not args.instrument_softgpu:
        ordinary.add(SOFT_GPU)
    if args.optimize:
        ordinary.add("src/pc/compat/gte.c")
        # This adapter already resolves complete guest OT spans explicitly.
        ordinary.add("src/pc/compat/libgs_ot.c")
    # Local independent runtime helpers must never resolve their own accesses.
    flags = [
        *guest_frontend_flags(sdk),
        "-fcommon",
        "-I" + str(dependencies / "include"),
        "-I" + str(dependencies / "include/freetype2"),
        "-w",
        "-Wno-incompatible-pointer-types",
        "-Wno-int-conversion",
        "-Wno-implicit-function-declaration",
    ]
    game_extra = ["-include", "src/pc/compat/pgxp_game.h"]
    if not args.optimize or args.instrument_softgpu:
        flags.append("-DMEMORIES_INSTRUMENT_SOFTGPU")
    if ANDROID_BIN and not args.release:
        # The test-only paths (MEMORIES_TEST_HOLD_GUEST and the others
        # notes/pc-build.md lists), as in build_game32.py's builds that are
        # not releases. macOS builds keep none.
        flags.append("-DMEMORIES_TEST_HOOKS")
    # Keep compiler outputs tied to the complete toolchain and source state.
    # A reused tmp/pc/macos directory must never hide a translator, option,
    # compiler, or revision change behind timestamps alone.
    fingerprint_inputs = []
    fingerprint_inputs.extend(
        path
        for path in (ROOT / "src").rglob("*")
        if path.is_file() and path.suffix in {".c", ".h", ".S"}
    )
    fingerprint_inputs.extend(
        path
        for path in (ROOT / "tools/pc").glob("*")
        if path.is_file()
        and path.suffix in {".py", ".cpp", ".json"}
        and not path.name.startswith("test_")
        and path.name != "guest_test_compile.py"
    )
    fingerprint_inputs.extend(
        path
        for path in (ROOT / "config/pc").glob("*")
        if path.is_file() and path.suffix in {".txt", ".json"}
    )
    source_state = []
    for path in sorted(fingerprint_inputs):
        source_state.append(
            (
                path.relative_to(ROOT).as_posix(),
                hashlib.sha256(path.read_bytes()).hexdigest(),
            )
        )
    revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
    ).strip()
    compiler_version = subprocess.check_output(
        [cc, "--version"], text=True
    ).splitlines()[0]
    fingerprint_data = {
        "revision": revision,
        "sources": source_state,
        "compiler": cc,
        "compiler_version": compiler_version,
        "llvm": str(llvm),
        "sdk": sdk,
        "dependencies": str(dependencies),
        "flags": flags,
        "game_extra": game_extra,
        "optimize": args.optimize,
        "instrument_softgpu": args.instrument_softgpu,
        "target": target,
    }
    fingerprint = hashlib.sha256(
        json.dumps(fingerprint_data, sort_keys=True).encode()
    ).hexdigest()
    fingerprint_file = build / "build-fingerprint.txt"
    if (
        not fingerprint_file.exists()
        or fingerprint_file.read_text().strip() != fingerprint
    ):
        for kind in ("raw", "ir", "obj"):
            shutil.rmtree(build / kind, ignore_errors=True)
            (build / kind).mkdir(parents=True, exist_ok=True)
    fingerprint_file.write_text(fingerprint + "\n")
    jobs = [(source, group) for group, sources in groups.items() for source in sources]
    jobs += [(source, "native") for source in natives]

    def path_for(kind, source, suffix):
        return unit_path(build, kind, source, suffix)

    if compile_sources(jobs, ordinary, path_for, cc, flags, game_extra, args):
        return 1
    print(
        f"Emitted {len(jobs)} units ({len(ordinary)} ordinary runtime helpers)",
        flush=True,
    )
    (
        raw_text,
        pin_maps,
        pc_storage_globals,
        aliases,
        renames,
        rows,
        linked_defs,
        host_renames,
        native_defs,
    ) = prepare_guest_units(jobs, groups, natives, ordinary, path_for, address_tables)
    registrations, failures = compile_guest_units(
        jobs, ordinary, raw_text, pin_maps, path_for, cc, sdk, args
    )
    (build / "summary.json").write_text(
        json.dumps(
            {
                "units": len(jobs),
                "failures": failures,
                "pc_storage_globals": pc_storage_globals,
                "groups": {g: len(v) for g, v in groups.items()},
                "ordinary": sorted(ordinary),
                "aliases": aliases,
                "renames": renames,
            },
            indent=2,
        )
        + "\n"
    )
    if failures:
        print("Transform/object failures:", len(failures))
        print("\n".join(failures))
        return 1
    print(f"Compiled {len(jobs)} ARM64 objects", flush=True)
    if args.compile_only:
        return 0
    tables = generate_function_tables(
        raw_text, linked_defs, host_renames, rows, pin_maps, registrations, revision
    )
    export_object = generate_mod_exports(
        build, jobs, path_for, raw_text, aliases, linked_defs, pin_maps, cc, flags
    )
    if ANDROID_BIN:
        write_mod_translation(build, address_tables)
    status = link_game(
        build, jobs, path_for, tables, export_object, cc, sdk, flags, dependencies
    )
    from build_game32 import release_version

    if not status:
        identity = dict(
            revision=revision,
            version=release_version(),
            fingerprint=fingerprint_file.read_text().strip(),
            binary_sha256=hashlib.sha256(
                (build / ("libgame.so" if ANDROID_BIN else "memories-arm64")).read_bytes()
            ).hexdigest(),
        )
        (build / "build-identity.json").write_text(
            json.dumps(identity, indent=2) + "\n"
        )
    return status


if __name__ == "__main__":
    raise SystemExit(main())
