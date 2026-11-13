"""Build native macOS code mods while retaining PS1 guest data layouts.

This uses the game's structured LLVM translation, not an ordinary LP64
recompile. The existing ELF object is preserved for Linux/Windows.
"""

import hashlib
import json
import re
import subprocess
import tempfile
from pathlib import Path

from build_config import MACOS_MINIMUM
from build_mod import module_pins
from guest_build import (
    CHECKED_LIBC,
    HOST_LIBC,
    guest_frontend_flags,
    maps,
)
from llvm_guest import ROOT, inspect, normalize, process, toolchain, validate_c_abi
from macos_deps import sdk_path


# Pointer-free math entries from src/pc/mods/mod_libc.c. Calls taking guest
# pointers must use the game's translated wrappers, never libSystem directly.
SYSTEM_MATH_IMPORTS = frozenset({
    "acos", "asin", "atan", "atan2", "atan2f", "ceil", "ceilf", "cos", "cosf",
    "exp", "expf", "fabs", "fabsf", "floor", "floorf", "fmod", "fmodf", "log",
    "log10", "logf", "pow", "powf", "sin", "sinf", "sqrt", "sqrtf", "tan", "tanf",
})
SYSTEM_RUNTIME_IMPORTS = {"abort", "dyld_stub_binder"}


def mod_pins(summary, pin_tables):
    """Build mod guest pins without replacing native PC-owned storage."""
    pc_storage_globals = summary.get("pc_storage_globals")
    if not isinstance(pc_storage_globals, list) or any(
        not isinstance(symbol, str) for symbol in pc_storage_globals
    ):
        raise SystemExit(
            "game summary is missing the pc_storage_globals list; rebuild the ARM64 game"
        )
    native_storage = set(pc_storage_globals)
    pins = {}
    for table in pin_tables:
        for symbol, address in table.items():
            if symbol not in native_storage:
                pins.setdefault(symbol, address)
    return pins


def validate_imports(text, signatures):
    """Validate the entry ABI and every known game import before translation."""
    has_entry = False
    for name, signature in inspect(text)["signatures"].items():
        if name == "MemoriesModInit" and signature["definition"]:
            validate_c_abi(name, signature)
            if (
                signature["result"] != "i32"
                or signature["variadic"]
                or signature["parameters"]
                != [
                    {"kind": "ptr", "signext": False},
                    {"kind": "ptr", "signext": False},
                ]
            ):
                raise SystemExit(
                    "MemoriesModInit must return int and accept (const MemoriesModHost *, MemoriesMod *)"
                )
            has_entry = True
        if signature["definition"] or name not in signatures:
            continue
        validate_c_abi(name, signature)
        expected = signatures[name]
        for key in (
            "result",
            "result_signext",
            "parameters",
            "variadic",
            "calling_convention",
        ):
            if signature[key] != expected[key]:
                raise SystemExit(f"ARM64 mod ABI mismatch for {name}: {key}")
    return has_entry


def compile_units(sources, folder, compiler, sdk, flags, aliases, signatures, pins):
    """Compile original C at O0, lower guest accesses, then generate O2 objects."""
    objects, routines = [], []
    has_entry = False
    for index, source in enumerate(sources):
        raw = folder / f"{index}.raw.ll"
        subprocess.run(
            [compiler, *flags, "-S", "-emit-llvm", str(source), "-o", str(raw)],
            check=True,
        )
        routine = f"MemoriesModRegisterUnit_{index}"
        text = normalize(raw.read_text(), aliases)
        has_entry = validate_imports(text, signatures) or has_entry
        text = normalize(
            text,
            {
                name: "GuestRuntime_" + name
                for name in HOST_LIBC
                | CHECKED_LIBC
                | {"malloc", "calloc", "realloc", "free"}
            },
        )
        text = process(
            "translate",
            text,
            pins=module_pins(text, pins),
            registration=routine,
            mod_unit=True,
        )
        ir = folder / f"{index}.ll"
        ir.write_text(text)
        obj = folder / f"{index}.o"
        subprocess.run(
            [
                compiler,
                "-isysroot",
                sdk,
                f"-mmacosx-version-min={MACOS_MINIMUM}",
                "-O2",
                "-c",
                str(ir),
                "-o",
                str(obj),
            ],
            check=True,
        )
        objects.append(str(obj))
        routines.append(routine)
    if not has_entry:
        raise SystemExit("mod sources do not define MemoriesModInit")
    return objects, routines


def link_library(folder, library_name, objects, routines, compiler, sdk, exported):
    """Pair global registration/cleanup and reject imports absent from the game."""
    registration = folder / "registration.c"
    registration.write_text(
        "".join(
            f"extern void {name}(void); extern void {name}_Unregister(void);\n"
            for name in routines
        )
        + "void MemoriesModRegisterGlobals(void) {\n"
        + "".join(f"{name}();\n" for name in routines)
        + "}\n"
        + "void MemoriesModUnregisterGlobals(void) {\n"
        + "".join(f"{name}_Unregister();\n" for name in reversed(routines))
        + "}\n"
    )
    staged = folder / library_name
    staged.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            compiler,
            "-isysroot",
            sdk,
            f"-mmacosx-version-min={MACOS_MINIMUM}",
            "-dynamiclib",
            "-Wl,-undefined,dynamic_lookup",
            *objects,
            str(registration),
            "-o",
            str(staged),
        ],
        check=True,
    )
    # dyld resolves these without consulting game exports. Only pointer-free
    # math and runtime helpers may bypass the translated guest wrappers.
    rows = subprocess.check_output(["nm", "-u", "-m", str(staged)], text=True).splitlines()
    system_imports = set()
    for line in rows:
        match = re.search(r' external (\S+) \(from libSystem\)$', line)
        if match:
            name = match.group(1)
            system_imports.add(name[1:] if name.startswith('_') else name)
    # Mach-O prepends one underscore; preserve names beginning with one.
    imports = {
        line.strip()[1:] if line.strip().startswith("_") else line.strip()
        for line in subprocess.check_output(
            ["nm", "-u", str(staged)], text=True
        ).splitlines()
        if line.strip()
    }
    unsafe_system = system_imports - SYSTEM_MATH_IMPORTS - SYSTEM_RUNTIME_IMPORTS
    if unsafe_system:
        raise SystemExit(
            "ARM64 mod cannot import from libSystem: "
            + ", ".join(sorted(unsafe_system))
        )
    missing = imports - exported - system_imports - SYSTEM_RUNTIME_IMPORTS
    if missing:
        raise SystemExit("ARM64 game does not provide: " + ", ".join(sorted(missing)))
    return staged


def build(directory, out=None, *, game=None):
    directory = Path(directory).resolve()
    sources = sorted(directory.glob("*.c"))
    if not sources:
        raise SystemExit(f"{directory}: no C sources")
    manifest = json.loads((directory / "mod.json").read_text(encoding="utf-8-sig"))
    library = manifest.get("library")
    if not library or Path(library).is_absolute() or ".." in Path(library).parts:
        raise SystemExit("mod.json must specify a relative library name inside the mod")
    library_name = (
        library
        if "." in library and not library.endswith(".o")
        else (library[:-2] if library.endswith(".o") else library) + ".dylib"
    )
    output = Path(out).resolve() / library_name if out else directory / library_name
    output.parent.mkdir(parents=True, exist_ok=True)
    game = Path(game).resolve() if game else ROOT / "tmp/pc/macos"
    if not (game / "exports.txt").is_file():
        raise SystemExit(
            "build the ARM64 game first; its exports.txt validates mod imports"
        )
    exported = set((game / "exports.txt").read_text().split())
    signatures = json.loads((game / "mod_signatures.json").read_text())
    game_summary_path = game / "summary.json"
    game_summary_bytes = game_summary_path.read_bytes()
    game_summary = json.loads(game_summary_bytes)
    aliases = dict(game_summary["aliases"])
    aliases.update(
        dict(
            line.split()
            for line in (ROOT / "config/pc/host_symbol_renames.txt")
            .read_text()
            .splitlines()
            if line.strip()
        )
    )
    aliases.update(rand="Mods_ModRand", srand="Mods_ModSrand")
    compiler = str(toolchain() / "bin/clang")
    sdk = str(sdk_path())
    flags = [
        *guest_frontend_flags(sdk),
        "-fno-common",
        "-DMEMORIES_MOD",
        "-I" + str(directory),
        "-Wno-gnu-folding-constant",
        "-Wno-pointer-to-int-cast",
    ]
    revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
    ).strip()
    compiler_version = subprocess.check_output(
        [compiler, "--version"], text=True
    ).splitlines()[0]
    game_fingerprint = game / "build-fingerprint.txt"
    fingerprint_data = {
        "revision": revision,
        "compiler": compiler,
        "compiler_version": compiler_version,
        "sdk": sdk,
        "flags": flags,
        "object_flags": ["-O2"],
        "mod_builder": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "sources": [
            (path.name, hashlib.sha256(path.read_bytes()).hexdigest())
            for path in sources
        ],
        "mod_directory": str(directory),
        "manifest": hashlib.sha256((directory / "mod.json").read_bytes()).hexdigest(),
        "game_fingerprint": game_fingerprint.read_text().strip()
        if game_fingerprint.exists()
        else None,
        "game_summary": hashlib.sha256(game_summary_bytes).hexdigest(),
        "llvm_guest": hashlib.sha256(
            (ROOT / "tools/pc/llvm_guest.py").read_bytes()
        ).hexdigest(),
        "guest_ir": hashlib.sha256(
            (ROOT / "tools/pc/llvm/guest_ir.cpp").read_bytes()
        ).hexdigest(),
        "guest_build": hashlib.sha256(
            (ROOT / "tools/pc/guest_build.py").read_bytes()
        ).hexdigest(),
        "marshalling": hashlib.sha256(
            (ROOT / "tools/pc/native_call_marshalling.py").read_bytes()
        ).hexdigest(),
        "toolchain_config": hashlib.sha256(
            (ROOT / "tools/pc/macos_toolchain.json").read_bytes()
        ).hexdigest(),
    }
    cache_key = hashlib.sha256(
        json.dumps(fingerprint_data, sort_keys=True).encode()
    ).hexdigest()
    folder = ROOT / "tmp/pc/mod-build-arm64" / cache_key[:20]
    folder.mkdir(parents=True, exist_ok=True)
    pins = mod_pins(game_summary, maps().values())
    objects, routines = compile_units(
        sources, folder, compiler, sdk, flags, aliases, signatures, pins
    )
    staged = link_library(
        folder, library_name, objects, routines, compiler, sdk, exported
    )
    with tempfile.NamedTemporaryFile(
        dir=output.parent, prefix="." + output.name, delete=False
    ) as temporary:
        temporary.write(staged.read_bytes())
        temporary_path = Path(temporary.name)
    temporary_path.chmod(0o755)
    temporary_path.replace(output)
    print(f"{output}: macOS ARM64, {len(sources)} translated source(s)")
    return output
