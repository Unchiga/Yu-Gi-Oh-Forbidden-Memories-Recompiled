#!/usr/bin/env python3
"""Build a code mod: every .c in its directory, merged into one object per
target (--target, default the first three):

  i386                <library>.o, the 32-bit games' (Linux and Windows)
  x86_64-windows      <library>.x86_64-windows.o, the 64-bit Windows game's
  aarch64-translated  <library>.aarch64-translated.o, the Android app's
                      (the translated game: its section below)
  aarch64             <library>.aarch64.o, the fixed-memory Android game's
                      (build_game32.py --android-memory fixed; not shipped,
                      built only when named)

("library": "x" in mod.json; "x.o" names the same three. A "libraries"
object, {"x86_64-windows": "file.o", ...}, names a target's file outright,
as the game reads it: read_manifest in src/pc/mods/mods.c.)

The i386 object runs on both the Linux and the Windows game, which load it
with their own loader (src/pc/mods/object_loader.c), so a mod is built once,
on either system, with the same result. That only holds because the flags
below close every gap between the two 32-bit ABIs
(notes/portable-mods-plan.md):

  -fno-pic -fno-common            plain relocations only; no GOT, no COMMON
  -fno-stack-protector            the canary lives in Linux thread storage
  -march=i686 -mno-sse            x87 floating point, no SSE alignment needs
  -mstackrealign                  every function aligns its own stack to 16:
                                  Windows only promises 4 on the way in, and
                                  the Linux game (SSE2) needs 16 on the way
                                  out, or it faults on the first movaps
  -fstack-clash-protection        touches each stack page on the way down,
                                  as Windows' guard page requires
  -ffreestanding -nostdinc        no system C library: the SDK's headers
                                  (src/pc/mods/sdk) declare what the game lends
  -mretpoline-external-thunk      (clang; GCC: -mindirect-branch=thunk-extern
                                  -mindirect-branch-register) indirect calls go
                                  through the game's __x86_indirect_thunk_*,
                                  so a call through a guest function pointer
                                  works without DEP, as in the game's own code

The compiler is clang (on Windows, llvm-mingw's, targeting i386 Linux ELF),
else gcc -m32. MEMORIES_MOD_CC names another. The game's headers come from
src/ (in this repository) or from the sdk/include the game ships beside it,
where this script is sdk/tools/build_mod.py.

The 64-bit targets need clang (the same one builds all three) and differ:

  x86_64-windows  the Windows x64 calling convention in an ELF64 object
                  (--target=x86_64-w64-windows-gnu-elf: one loader reads
                  every target's objects); -mno-ms-bitfields, the game's
                  layouts
  aarch64         AAPCS64 as the Android game (aarch64-linux-android24 and
                  its flags), through tools/pc/ptr32_stores.py as every game
                  unit is (LLVM's AArch64 back end mis-sizes stores through a
                  4-byte pointer)
  aarch64-translated  through the game's guest pass (aarch64-translated,
                  below), which needs the pass itself (the SDK ships it) and
                  a clang of the LLVM the pass is built against
  both            -fms-extensions: the game's stored pointers stay 4 bytes
                  (G32, src/port_ptr.h); src/pc/mods/prelude64.h first, which
                  also declares the guest tables mods reach most, so a mod
                  that declares one itself without G32 does not compile; the
                  game's headers as system headers, and the mod's own pointer
                  and integer mix-ups (a pointer cast to a 32-bit int, a
                  pointer of another type) as errors, since on 64 bits they
                  lose half an address. A `.memories.abi` section names the
                  target, which the loader checks: x86-64 objects of the
                  Linux and of the Windows ABI look alike.

The names the mod leaves undefined are checked against what the game
provides: in the repository, each game build (--game, default both); beside
a game, the list its SDK was shipped with. A mod that would load on one
system and not the other fails here rather than in the game.

Objects are kept in tmp/pc/mod-build/<mod>-<key>, where the key is a digest
of everything that goes into the object: the compiler and linker (their
files), the flags, the include-path environment variables (ENVIRONMENT),
this script, and each source as the preprocessor sees
it, so with every header it includes (inputs_key). tmp is shared by every
checkout of the repository (the worktrees link it), and an object is reused
only where its inputs are the same, never because it is newer. A memo in
tmp/pc/mod-build/.memo finds the key again without the preprocessor while
none of the files it read has changed."""
import argparse, concurrent.futures, filecmp, functools, glob, hashlib, json, os, re, shutil, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# Before the first import of a sibling module: Windows' embeddable Python
# (it has a ._pth file) and `python -I -P` start without the script's folder
# on sys.path, and sdk/build-mod.bat runs the former.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_process
# In the repository, or the copy in the sdk/ directory beside a game
# (sdk/tools/build_mod.py, with the headers in sdk/include).
SHIPPED = not os.path.isdir(os.path.join(ROOT, "src/pc/mods/sdk"))
SDK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LLVM_MINGW = os.path.join(ROOT, "tmp/pc/llvm-mingw/bin")
CACHE = os.path.join(ROOT, "tmp", "pc", "mod-build")
# aarch64-translated: the Android app's game, the translated one (below).
TRANSLATED = "aarch64-translated"
TARGETS = ("i386", "x86_64-windows", "aarch64", TRANSLATED)
# What is built when no --target names one: the 32-bit games', the 64-bit
# Windows game's and the Android app's. aarch64, the fixed-memory Android
# game's (build_game32.py --android-memory fixed, not shipped), only when
# named.
DEFAULT_TARGETS = ("i386", "x86_64-windows", TRANSLATED)
# The game builds a target's names are checked against (--game overrides);
# beside a game, the lists its SDK was shipped with.
TARGET_BUILDS = {"i386": ["tmp/pc/game32", "tmp/pc/win32"], "x86_64-windows": ["tmp/pc/win64"],
                 "aarch64": ["tmp/pc/android-arm64-v8a"], TRANSLATED: ["tmp/pc/android-arm64-v8a"]}
GAME_BUILDS = [SDK] if SHIPPED else [os.path.join(ROOT, path) for path in TARGET_BUILDS["i386"]]


def game_builds(target):
    return [SDK] if SHIPPED else [os.path.join(ROOT, path) for path in TARGET_BUILDS[target]]


FLAGS = ["-std=gnu11", "-O2", "-g", "-fno-pic", "-fno-pie", "-fno-common", "-fno-stack-protector",
         "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-fstack-clash-protection",
         "-march=i686", "-mno-sse", "-mno-mmx", "-ffreestanding", "-nostdinc",
         "-DMEMORIES_PC", "-DMEMORIES_MOD", "-D_LANGUAGE_C", "-DLANGUAGE_C", "-Wall",
         "-Wno-unused-function", "-Wno-missing-braces"]
CLANG_FLAGS = ["--target=i386-pc-linux-gnu", "-mstackrealign", "-mretpoline-external-thunk"]
GCC_FLAGS = ["-m32", "-mstackrealign", "-mincoming-stack-boundary=2", "-mindirect-branch=thunk-extern",
             "-mindirect-branch-register"]
ENVIRONMENT = ["CPATH", "C_INCLUDE_PATH", "CCC_OVERRIDE_OPTIONS", "GCC_EXEC_PREFIX", "COMPILER_PATH"]
# The 64-bit targets (above): FLAGS without what is i386's own.
FLAGS64 = ["-std=gnu11", "-O2", "-g", "-fno-pic", "-fno-pie", "-fno-common", "-fno-stack-protector",
           "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-ffreestanding", "-nostdinc",
           "-DMEMORIES_PC", "-DMEMORIES_MOD", "-D_LANGUAGE_C", "-DLANGUAGE_C", "-Wall",
           "-Wno-unused-function", "-Wno-missing-braces", "-fms-extensions", "-fno-jump-tables",
           "-Werror=incompatible-pointer-types", "-Werror=pointer-to-int-cast", "-Werror=int-to-pointer-cast",
           "-Werror=int-conversion"]
TARGET_FLAGS = {
    # The 64-bit Windows game's calling convention, layouts and indirect-branch
    # thunk (__x86_indirect_thunk_r11), in an ELF64 object.
    "x86_64-windows": ["--target=x86_64-w64-windows-gnu-elf", "-mretpoline-external-thunk", "-mno-ms-bitfields"],
    # The arm64 Android game's (build_android_deps.READY["arm64-v8a"] and
    # build_game32.py's arm64 flags): clang's SLS thunks as the indirect-branch
    # thunks, no fused multiply-add, MIPS's signed char.
    "aarch64": ["--target=aarch64-linux-android24", "-mharden-sls=blr", "-fno-optimize-sibling-calls",
                "-mno-outline-atomics", "-ffp-contract=off", "-fsigned-char"],
    # The translated Android game's (tools/pc/guest_build.py's Android
    # flags): position-independent, so the code reaches the game through
    # GOT entries wherever the loader puts it; no outlined atomics, which
    # would call helpers the game does not lend.
    TRANSLATED: ["--target=aarch64-linux-android24", "-fPIC", "-fsigned-char", "-mno-outline-atomics"],
}
# aarch64-translated: the C as the translated game's own units are compiled
# for the guest pass (guest_build.guest_frontend_flags, and build_mod_arm64.py
# for macOS): unoptimized, the PS1's layouts kept (MEMORIES_TRANSLATED), no
# fused multiply-add, wrapping arithmetic. Freestanding with the SDK's C
# library, as the other targets, so any clang up to 21 builds it.
TRANSLATED_FLAGS = ["-std=gnu11", "-O0", "-fno-common", "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
                    "-fno-unwind-tables", "-ffreestanding", "-nostdinc", "-fms-extensions", "-fno-strict-aliasing",
                    "-ffp-contract=off", "-fwrapv", "-DMEMORIES_PC", "-DMEMORIES_MOD", "-DMEMORIES_TRANSLATED",
                    "-D_LANGUAGE_C", "-DLANGUAGE_C", "-Wall", "-Wno-unused-function", "-Wno-missing-braces",
                    "-Wno-gnu-folding-constant", "-Wno-pointer-to-int-cast"]
# The LLVM a guest pass is taken to be built against when it cannot say (one
# built before memories-guest-ir had --llvm-major): the game's own,
# tools/pc/macos_toolchain.json. The pass in an SDK is built against the LLVM
# of the SDK's clang (tools/pc/sdk_llvm.json: llvm-mingw's, LLVM_MINGW_CLANG
# below); in a checkout, tools/pc/llvm_guest.py builds it against the game's.
GUEST_LLVM_DEFAULT = 21


# The toolchain the game itself is built with, which has clang, ld.lld and
# llvm-objcopy and builds every target: the llvm-mingw release
# tools/pc/fetch_tools.py (Windows) and tools/pc/build_win32_deps.py (Linux)
# fetch. Named here for a modder who has none (toolchain_help), since the
# SDK ships neither script; test_build_mod_toolchain.py keeps them the same.
LLVM_MINGW_RELEASE = "20260922"
# That release's clang, and so the LLVM the SDK's guest pass is built against
# (tools/pc/sdk_llvm.json; test_build_mod_toolchain.py keeps them the same).
LLVM_MINGW_CLANG = 23
LLVM_MINGW_DOWNLOADS = {
    "windows": "https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/"
               "llvm-mingw-20260922-ucrt-x86_64.zip",
    "linux": "https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/"
             "llvm-mingw-20260922-ucrt-ubuntu-22.04-x86_64.tar.xz",
}
WIDE_TARGETS = ("x86_64-windows", "aarch64", TRANSLATED)


def tool(name):
    """A program from PATH, or from the llvm-mingw this repository fetches."""
    found = shutil.which(name)
    if not found and os.path.exists(os.path.join(LLVM_MINGW, name + (".exe" if os.name == "nt" else ""))):
        found = os.path.join(LLVM_MINGW, name)
    return found


def beside(program, name):
    """`name` in the folder of `program`, with Windows' .exe spelled out:
    "ld.lld" already has an extension, so none is added to it."""
    path = os.path.join(os.path.dirname(program), name)
    return next((p for p in (path + ".exe", path) if os.path.exists(p)), None)


def linker_for(clang):
    """The lld that came with `clang`, else one on PATH."""
    return beside(clang, "ld.lld") or tool("ld.lld")


def objcopy_for(clang):
    """llvm-objcopy beside `clang` and beside the clang a link points to
    (Debian's /usr/bin/clang is /usr/lib/llvm-NN/bin/clang, the only place
    its llvm-objcopy has no version suffix), then on PATH and in llvm-mingw."""
    for program in dict.fromkeys((clang, os.path.realpath(clang)) if clang else ()):
        found = beside(program, "llvm-objcopy")
        if found:
            return found
    return tool("llvm-objcopy")


def clang_candidates(named):
    """The compilers build_mod.py tries, in order (programs): `named`
    (MEMORIES_MOD_CC) alone when set."""
    return [named] if named else [os.path.join(LLVM_MINGW, "clang"), "clang", "gcc"]


def resolve(candidate):
    return candidate if os.path.isabs(candidate) and os.path.exists(candidate) else tool(candidate)


def missing_64bit():
    """What the 64-bit targets need that this machine lacks, by name: clang,
    ld.lld (beside it or on PATH) and llvm-objcopy. [] when all are here."""
    found = [path for path in map(resolve, clang_candidates(os.environ.get("MEMORIES_MOD_CC")))
             if path and "clang" in os.path.basename(path)]
    clang = next((path for path in found if linker_for(path)), found[0] if found else None)
    missing = [] if clang else ["clang"]
    if not (linker_for(clang) if clang else tool("ld.lld")):
        missing.append("ld.lld")
    if not objcopy_for(clang):
        missing.append("llvm-objcopy")
    return missing


def toolchain_help(missing, built=None, wanted=WIDE_TARGETS, compiler=True, outcome=True):
    """What to tell a modder whose 64-bit objects cannot be built: what is
    missing, what was built (with `outcome`: the i386 object at `built`, or
    nothing; with no compiler at all, `compiler` false, nothing), and where
    to get the toolchain the game is built with. No download here: the
    modder gets it."""
    named = os.environ.get("MEMORIES_MOD_CC")
    not_found = "Not found: " + ", ".join(missing)
    if named and "clang" in missing:
        not_found += f" (MEMORIES_MOD_CC names {named}, which is {'not clang' if resolve(named) else 'not there'})"
    games = {"x86_64-windows": "the 64-bit Windows game", "aarch64": "Android", TRANSLATED: "Android"}
    named_objects = ", ".join(f"{target}: {games[target]}" for target in wanted)
    if not compiler:
        lines = ["build_mod: nothing was built: no compiler found.",
                 "Every object builds with clang, ld.lld and llvm-objcopy; gcc with 32-bit support builds the "
                 f"32-bit one only. {not_found}."]
    else:
        lines = [f"build_mod: only the 32-bit object was built, {built} (the 32-bit Windows and Linux games)."
                 if built else "build_mod: nothing was built."] if outcome else []
        lines.append(f"The 64-bit object{'s' if len(wanted) > 1 else ''} ({named_objects}) "
                     f"need{'' if len(wanted) > 1 else 's'} clang with ld.lld and llvm-objcopy. {not_found}.")
    if sys.platform == "win32":
        lines += [f"Get llvm-mingw {LLVM_MINGW_RELEASE}, the toolchain the game is built with (it has all three):",
                  f"  {LLVM_MINGW_DOWNLOADS['windows']}",
                  "unzip it, put its bin folder on PATH (or set MEMORIES_MOD_CC to its bin\\clang.exe), "
                  "and run build_mod.py again."]
    elif sys.platform == "darwin":
        lines += ["Install them with Homebrew (brew install llvm lld), put $(brew --prefix llvm)/bin on PATH "
                  "(or set MEMORIES_MOD_CC to its clang), and run build_mod.py again."]
    else:
        lines += [f"Get llvm-mingw {LLVM_MINGW_RELEASE}, the toolchain the game is built with (Linux x86-64):",
                  f"  {LLVM_MINGW_DOWNLOADS['linux']}",
                  "and put its bin folder on PATH (or set MEMORIES_MOD_CC to its bin/clang); or install your "
                  "distribution's clang, lld and llvm (llvm-objcopy is in llvm):",
                  "  Debian/Ubuntu: sudo apt install clang lld llvm    Fedora: sudo dnf install clang lld llvm",
                  "  Arch: sudo pacman -S clang lld llvm",
                  "The 64-bit game is built with clang 21 or later (older ones are not verified with its 4-byte "
                  "pointers; clang 12 miscompiles them): prefer one that new. Then run build_mod.py again."]
    return "\n".join(lines)


def compiler(target="i386"):
    """(command, the compiler's own flags and headers for `target`, linker
    command); target_flags adds the rest."""
    if target == TRANSLATED:
        path = translated_clang()
        return [path], TARGET_FLAGS[target] + ["-isystem", builtin_headers(path, True)], [linker_for(path), "-r"]
    path, clang, linker = programs()
    if target == "i386":
        return [path], (CLANG_FLAGS if clang else GCC_FLAGS) + ["-isystem", builtin_headers(path, clang)], list(linker)
    if not clang:
        sys.exit(f"build_mod: the {target} object needs clang (with lld); {path} is not clang\n" +
                 toolchain_help(missing_64bit() or ["clang"], wanted=(target,), outcome=False))
    return [path], TARGET_FLAGS[target] + ["-isystem", builtin_headers(path, clang)], list(linker)


def target_flags(target, cc_flags):
    """Every flag a unit is compiled with for `target`, before the mod's own
    (-I <its directory>): the i386 list exactly as it has always been."""
    if target == "i386":
        return FLAGS + cc_flags + headers()
    if target == TRANSLATED:
        return TRANSLATED_FLAGS + cc_flags + headers(system=True)
    return FLAGS64 + cc_flags + headers(system=True) + [
        "-include", os.path.join(include_root(), "pc", "mods", "prelude64.h")]


def programs():
    """(compiler, whether it is clang, linker command), found without
    starting either: the cache keys need only their files."""
    found = find_programs(os.environ.get("MEMORIES_MOD_CC"))
    if not found:
        sys.exit(toolchain_help(missing_64bit(), compiler=False))
    return found


@functools.lru_cache(maxsize=None)
def find_programs(named):
    for candidate in clang_candidates(named):
        path = resolve(candidate)
        if not path:
            continue
        if "clang" in os.path.basename(path):
            linker = linker_for(path)
            if not linker:
                continue  # clang without lld cannot merge the objects; try the next compiler
            return path, True, (linker, "-r")
        return path, False, (tool("ld") or "ld", "-m", "elf_i386", "-r")
    return None


@functools.lru_cache(maxsize=None)
def builtin_headers(path, clang):
    """The compiler's own headers (stddef.h, stdarg.h and the like)."""
    if clang:
        resource = subprocess.run([path, "-print-resource-dir"], capture_output=True, text=True, encoding="utf-8", errors="replace").stdout.strip()
        return os.path.join(resource, "include")
    return subprocess.run([path, "-m32", "-print-file-name=include"], capture_output=True, text=True, encoding="utf-8", errors="replace").stdout.strip()


def include_root():
    """Where the game's headers are: src/, or the SDK's include/."""
    return os.path.join(SDK, "include") if SHIPPED else os.path.join(ROOT, "src")


def headers(system=False):
    """The SDK's C library, then the game's headers (as system headers for
    the 64-bit targets: their warnings are the game's; the mod's own are
    errors there)."""
    libc = os.path.join(SDK, "include", "libc") if SHIPPED else os.path.join(ROOT, "src/pc/mods/sdk")
    return ["-isystem", libc, "-isystem" if system else "-I", include_root()]


def run(command):
    result = build_process.run(command)
    if result.returncode:
        sys.exit(f"build_mod: {' '.join(command[:3])} ... failed\n{result.stdout}{result.stderr}")
    return result.stdout


def provided(build, target="i386"):
    """The names a game build of `target` lends mods: its export table and
    the C library list, or None if that build is not there. A game build in
    the repository has its generated table; an SDK beside a game has the
    list it was shipped with (exports.<target>.txt; a 32-bit game's is also
    exports.txt, the one name SDKs had before the 64-bit targets)."""
    table = os.path.join(build, "mod_exports.c")
    if os.path.exists(table):
        with open(table) as handle:
            names = set(re.findall(r'^    \{"([^"]+)"', handle.read(), re.M))
        return names | (translated_libc(build) if target == TRANSLATED else libc_names(target))
    for shipped in [os.path.join(build, f"exports.{target}.txt")] + \
            ([os.path.join(build, "exports.txt")] if target == "i386" else []):
        if os.path.exists(shipped):
            with open(shipped) as handle:
                return set(handle.read().split())
    return None


def libc_names(target="i386"):
    """The C library list in src/pc/mods/mod_libc.c for `target`: the names
    every target has, and those of its own block (`#if defined(__i386__)`,
    `#elif defined(__x86_64__) ...`)."""
    with open(os.path.join(ROOT, "src/pc/mods/mod_libc.c")) as handle:
        text = handle.read()
    table = text[text.index("functions[] = {"):]
    table = table[:table.index("};")]
    names, block = set(), None
    for line in table.splitlines():
        stripped = line.strip()
        if stripped.startswith(("#if", "#elif")):
            block = ("i386" if "__i386__" in stripped else "x86_64-windows" if "__x86_64__" in stripped
                     else "aarch64" if "__aarch64__" in stripped else "?")
            continue
        if stripped.startswith("#endif"):
            block = None
            continue
        if block in (None, target):
            # F(name), or AS(name, function) for one the host implements itself.
            names.update(re.findall(r"\b(?:F\(|AS\()(\w+)\b", line))
    return names


def library_name(directory, target="i386"):
    """The object's file name for `target`, relative to the mod's directory,
    by the game's own rule (read_manifest in src/pc/mods/mods.c): the
    target's entry in "libraries" as written; else from "library", for
    i386 as written when it has a '.' anywhere in it (else with ".o"
    added) and for the others <library>.<target>.o, a ".o" taken off first.
    A mod with "libraries" and no "library" goes by its id. The game loads
    no code for a mod with neither, so neither is one built."""
    manifest = os.path.join(directory, "mod.json")
    if not os.path.exists(manifest):
        sys.exit(f"{directory}: no mod.json (notes/modding.md)")
    # utf-8-sig: a manifest saved with a byte order mark, as the game allows.
    with open(manifest, encoding="utf-8-sig") as handle:
        data = json.load(handle)
    libraries = data.get("libraries")
    named = libraries.get(target) if isinstance(libraries, dict) else None
    if isinstance(named, str) and named:
        return named
    name = data.get("library")
    if not name and isinstance(libraries, dict) and libraries:
        name = data.get("id") or os.path.basename(os.path.normpath(directory))
    if not name:
        sys.exit(f'{manifest}: the mod has C sources but no "library": the game would load none of its code. '
                 f'Add "library": "{os.path.basename(os.path.normpath(directory))}"')
    if target == "i386":
        return name if "." in name else name + ".o"
    stem = name[:-2] if len(name) > 2 and name.endswith(".o") else name
    return f"{stem}.{target}.o"


def identity(program):
    """A compiler or linker as the keys know it: its file's name, size and
    time, as ccache's compiler_check=mtime does, so no process starts. The
    same file through another worktree's junction is the same file."""
    found = program if os.path.dirname(program) else shutil.which(program)
    for candidate in (found, found + ".exe") if found else ():
        if os.path.isfile(candidate):
            real = os.path.realpath(candidate)
            stat = os.stat(real)
            return f"{os.path.basename(real)} {stat.st_size} {stat.st_mtime_ns}"
    sys.exit(f"build_mod: {program} not found")


def portable(text):
    """`text` with this checkout's root spelled the same in every checkout."""
    for root in sorted({ROOT, ROOT.replace("\\", "/"), ROOT.replace("/", "\\")}, key=len, reverse=True):
        text = text.replace(root, "<root>")
    return text


def file_digest(path):
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def script_digest(path):
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read().replace(b"\r\n", b"\n")).hexdigest()   # either checkout's line ends


def settings_digest(directory, extra_flags, target="i386"):
    """What both of an object's digests start from: this script, the
    compiler and linker, the target, the flags and the library name, and for
    aarch64 the IR pass every unit goes through."""
    if target == TRANSLATED:
        (path,), _, linker = compiler(target)
        clang = True
    else:
        path, clang, linker = programs()
    if target == "i386":
        flags = FLAGS + (CLANG_FLAGS if clang else GCC_FLAGS) + headers() + list(extra_flags)
    else:
        flags = target_flags(target, TARGET_FLAGS[target]) + list(extra_flags)
    digest = hashlib.sha256()
    script = script_digest(os.path.abspath(__file__))
    # What the compiler reads from the environment that can change the
    # object: CPATH and C_INCLUDE_PATH add include directories even with
    # -nostdinc, so a memo would otherwise find the key of other headers.
    environment = "\n".join(f"{name}={os.environ.get(name, '')}" for name in ENVIRONMENT)
    parts = [("script", script), ("compiler", identity(path)), ("linker", identity(linker[0])),
             ("flags", portable("\n".join(flags))), ("library", library_name(directory, target)),
             ("environment", environment)]
    if target != "i386":   # the i386 keys stay what they were
        parts.append(("target", target))
    if target == "aarch64":
        parts.append(("pass", script_digest(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                         "ptr32_stores.py"))))
    if target == TRANSLATED:   # the guest pass and the game's tables it is given
        context = translation_context()
        parts.append(("translation", identity(context["tool"]) + " " + context["digest"]))
    for label, text in parts:
        digest.update(f"{label}\0{text}\0".encode("utf-8", "surrogateescape"))
    return digest


# A line marker in preprocessed C: # <line> "<file>" <flags>. The key keeps
# the line, which the debug information records, and drops the file, which
# names the checkout; the file goes in the memo's list instead.
LINE_MARKER = re.compile(rb'^(#(?: line)? \d+) "((?:[^"\\]|\\.)*)"', re.M)


def inputs_key(directory, sources, extra_flags, into, target="i386"):
    """The key an object is kept under in tmp/pc/mod-build: settings_digest
    and each source after the preprocessor, which takes in every header it
    includes from wherever it is. The preprocessed sources are written to
    `into` (<source>.i), and publish compiles those, not the sources: the
    object is made of exactly what the key was taken from, even when a
    header changes while the mod builds. No time goes into the key: another
    checkout sharing tmp gets the same object only when it has the same
    inputs. Returns (key, the preprocessed files, the files the
    preprocessor read, or None when one of them cannot be named, and when
    it started)."""
    cc, cc_flags, _ = compiler(target)
    flags = target_flags(target, cc_flags) + list(extra_flags)
    digest = settings_digest(directory, extra_flags, target)
    started = time.time()
    preprocessed = [os.path.join(into, os.path.basename(source) + ".i") for source in sources]
    with concurrent.futures.ThreadPoolExecutor(len(sources)) as pool:
        list(pool.map(lambda pair: run(cc + flags + ["-E", pair[0], "-o", pair[1]]), zip(sources, preprocessed)))
    read, unnamed = set(), False
    for source, path in zip(sources, preprocessed):
        with open(path, "rb") as handle:
            text = handle.read().replace(b"\r\n", b"\n")   # clang on Windows writes CRLF
        for match in LINE_MARKER.finditer(text):
            if match.group(2).startswith(b"<"):
                continue   # <built-in>, <command line>
            name = os.path.normpath(os.path.abspath(
                re.sub(rb"\\(.)", rb"\1", match.group(2)).decode("utf-8", "surrogateescape")))
            if os.path.isfile(name):
                read.add(name)
            elif not os.path.isdir(name):   # GCC names the working directory with -g
                unnamed = True   # a file the memo could not watch: no memo
        digest.update(os.path.basename(source).encode() + b"\0" + LINE_MARKER.sub(rb"\1", text) + b"\0")
    return digest.hexdigest(), preprocessed, None if unnamed else read, started


# The memo: the key inputs_key gave, remembered with the content of every
# file the preprocessor read, so that a build with none of them changed
# finds the key again without starting the preprocessor (as ccache's direct
# mode). It is only a way to the key, never to an object: a memo that does
# not hold sends the build to inputs_key. The memos are in
# tmp/pc/mod-build/.memo.


def memo_folder(directory, sources, extra_flags, target="i386"):
    """Where the memos for these sources are: named by settings_digest, the
    sources and the names of every file that could be included (in the
    game's headers or the SDK's, and in the mod's directory), so that a new
    file that would be found first, and so was never read, also misses."""
    digest = settings_digest(directory, extra_flags, target)
    for source in sources:
        digest.update(os.path.basename(source).encode() + b"\0" + file_digest(source).encode() + b"\0")
    include = os.path.join(SDK, "include") if SHIPPED else os.path.join(ROOT, "src")
    names = glob.glob(os.path.join(include, "**", "*"), recursive=True)
    # Not the objects build_mod.py writes beside the sources by default.
    outputs = {os.path.abspath(os.path.join(directory, library_name(directory, each))) for each in TARGETS}
    names += [name for name in glob.glob(os.path.join(directory, "**", "*"), recursive=True)
              if os.path.abspath(name) not in outputs]
    listing = "\n".join(sorted(portable(os.path.abspath(name)) for name in names))
    digest.update(listing.encode("utf-8", "surrogateescape"))
    return os.path.join(CACHE, ".memo", f"{os.path.basename(os.path.abspath(directory))}-{digest.hexdigest()[:24]}")


def recall(folder):
    """The key of a memo in `folder` whose files are all as they were."""
    for path in sorted(glob.glob(os.path.join(folder, "*.json"))):
        try:
            with open(path, encoding="utf-8") as handle:
                memo = json.load(handle)
            if all(file_digest(name.replace("<root>", ROOT)) == digest for name, digest in memo["files"].items()):
                try:
                    os.utime(folder)   # in use: kept by prune
                except OSError:
                    pass
                return memo["key"]
        except (OSError, ValueError, KeyError, AttributeError):
            continue   # a file gone, or a memo from another version of this script
    return None


def remember(folder, key, read, started):
    if read is None:
        return
    files = {}
    try:
        for path in sorted(read):
            if os.path.getmtime(path) >= started:
                return   # changed while the preprocessor ran: the key may not be of what is there now
            files[portable(path)] = file_digest(path)
        os.makedirs(folder, exist_ok=True)
        handle, temporary = tempfile.mkstemp(dir=folder, suffix=".tmp")
    except OSError:
        # A header gone since the preprocessor read it (a checkout while the
        # mod builds), or the folder pruned by another build just now.
        return
    try:
        with os.fdopen(handle, "w", encoding="utf-8") as stream:
            json.dump({"key": key, "files": files}, stream, indent=0)
        os.replace(temporary, os.path.join(folder, key[:16] + ".json"))
    except OSError:
        # Windows will not replace a file another build is reading (recall),
        # and that file is this same memo. A memo is only a shortcut.
        try:
            os.remove(temporary)
        except OSError:
            pass


def compile_object(sources, output, objects_dir, extra_flags=(), target="i386"):
    """Compile `sources` with the mod flags for `target` (then `extra_flags`,
    which the loader's tests use to make broken objects) and merge them into
    `output`; a 64-bit object gets its `.memories.abi` section."""
    cc, cc_flags, linker = compiler(target)
    flags = target_flags(target, cc_flags)
    objects = []
    os.makedirs(objects_dir, exist_ok=True)
    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    clang = True if target == TRANSLATED else programs()[1]
    entry = False
    for index, source in enumerate(sources):
        obj = os.path.join(objects_dir, os.path.basename(source) + ".o")
        # clang names the compile unit after the file it compiles, here the
        # preprocessed copy in a staging folder that is gone a moment later
        # (and has a random name, so no two builds were the same bytes).
        # Given that same path as the main file name, it takes the name in
        # the first line marker instead, the source's, as GCC does.
        named = ["-Xclang", "-main-file-name", "-Xclang", source] if clang and "cpp-output" in extra_flags else []
        if target == "aarch64":
            through_pass(cc + flags + [*extra_flags, *named], source, obj)
        elif target == TRANSLATED:
            if translate_unit(cc + flags + [*extra_flags, *named], source, obj, index):
                entry = True
        else:
            run(cc + flags + [*extra_flags, *named, "-c", source, "-o", obj])
        objects.append(obj)
    if target == TRANSLATED:
        if not entry:
            sys.exit(f"build_mod: {output}: the mod's sources do not define MemoriesModInit")
        objects.append(registration_object(cc[0], len(sources), objects_dir))
    run(linker + ["-o", output] + objects)
    if target != "i386":
        tag_abi(output, target)
    return output


def through_pass(command, source, obj):
    """An aarch64 unit as the arm64 game's are (build_game32.py's
    compile_unit): to IR, every store through a 4-byte pointer sent through
    an ordinary one (tools/pc/ptr32_stores.py), then to the object."""
    import ptr32_stores
    ir = obj[:-2] + ".ll"
    run(command + ["-S", "-emit-llvm", "-Xclang", "-disable-llvm-passes", source, "-o", ir])
    with open(ir, encoding="utf-8") as handle:
        text = handle.read()
    try:
        text = ptr32_stores.rewrite(text)
    except ValueError as error:
        sys.exit(f"build_mod: {source}: {error}")
    with open(ir, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    # The IR is compiled with the unit's flags less the language's (it is
    # LLVM IR now, not C or preprocessed C).
    flags = [flag for flag in command[1:] if flag not in ("-x", "cpp-output", "c")]
    run([command[0], *flags, "-Wno-unused-command-line-argument", "-x", "ir", "-c", ir, "-o", obj])


# --- aarch64-translated: the Android app's game ---------------------------
#
# The Android app runs the translated game (notes/pc-build.md, "Android
# arm64"): no guest memory at a fixed host address, every guest access of
# the game's code lowered by the guest pass (tools/pc/llvm/guest_ir.cpp,
# through tools/pc/llvm_guest.py). A mod's code goes through the same pass,
# as on macOS (build_mod_arm64.py): each unit to LLVM IR at -O0, its names
# renamed as the game's are (the game's aliases, rand to the mods' own
# generator, the C library to the runtime's wrappers that take guest
# pointers), checked against the game's signatures, translated with the
# game's pinned data and a registration routine for its own globals, then
# compiled at -O2. MemoriesModRegisterGlobals and
# MemoriesModUnregisterGlobals call those routines, and the loader
# (src/pc/mods/object_loader.c) calls the pair. What the pass is given comes
# from the game build (mod_translation.json and mod_signatures.json, which
# build_arm64.py writes and the SDK carries), and the pass itself from
# MEMORIES_GUEST_IR, the SDK's tools/memories-guest-ir, or in the repository
# llvm_guest.py, which builds it.
#
# The clang writes the IR the pass reads and compiles the IR the pass writes
# (the same clang, start to end), and LLVM reads the IR of its own version
# or an older one, never a newer: the pass cannot read a newer clang's IR,
# and an older clang cannot be counted on to read the pass's. Only a clang
# of the LLVM the pass is built against (`memories-guest-ir --llvm-major`)
# does both: the SDK's pass is built against its llvm-mingw's clang, the
# checkout's against the game's LLVM 21 (where the NDK's clang 21 goes, not
# llvm-mingw's 23). MEMORIES_MOD_ALLOW_OTHER_CLANG=1 lets an older clang
# through, with a warning, for a modder who knows his clang reads it.
ALLOW_OTHER_CLANG = "MEMORIES_MOD_ALLOW_OTHER_CLANG"

_translation = {}   # translation_problem's findings, for the games being built for


@functools.lru_cache(maxsize=None)
def clang_version_text(path):
    """What `path --version` prints, "" when it cannot run."""
    try:
        return subprocess.run([path, "--version"], capture_output=True, text=True, encoding="utf-8",
                              errors="replace").stdout
    except OSError:
        return ""


def is_apple_clang(path):
    """Whether `path` is Apple's clang, whose version numbers (Apple clang 17)
    are Xcode's, not LLVM's: no LLVM major can be read off them."""
    return "Apple clang" in clang_version_text(path)


def clang_major(path):
    """The LLVM major version of the clang at `path`, or None: also for
    Apple's, which is_apple_clang tells apart."""
    if is_apple_clang(path):
        return None
    match = re.search(r"clang version (\d+)", clang_version_text(path))
    return int(match.group(1)) if match else None


class PassCannotRun(Exception):
    """The guest pass could not be started at all."""


def cannot_run(tool, error):
    return PassCannotRun(f"build_mod: could not run the guest pass {tool}: {error}. An antivirus may have blocked "
                         "or quarantined it (it is an unsigned program): restore it or allow it, or unpack the SDK again")


@functools.lru_cache(maxsize=None)
def tool_llvm_major(tool):
    """The LLVM major version the guest pass `tool` is built against (its
    --llvm-major), None for one that runs but cannot say (built before it
    could: it exits with its usage). Raises PassCannotRun for one that does
    not start (a blocked or quarantined file, a wrong kind of program)."""
    try:
        result = subprocess.run([tool, "--llvm-major"], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError) as error:
        raise cannot_run(tool, error) from None
    text = result.stdout.strip()
    return int(text) if result.returncode == 0 and text.isdigit() else None


def guest_pass_major(tool=None):
    """The LLVM major version of the guest pass in use (`tool`, else the one
    translation_problem found, else one built before in this checkout),
    GUEST_LLVM_DEFAULT when it cannot say (PassCannotRun when it does not run)."""
    tool = tool or _translation.get("tool") or guest_ir_tool(False)[0]
    return (tool_llvm_major(tool) if tool else None) or GUEST_LLVM_DEFAULT


def clang_fits(path, major, pass_major):
    """Whether a clang (`major`, its LLVM version) can go with a guest pass of
    LLVM `pass_major`: the same LLVM (the pass reads its IR and it reads the
    pass's), or with MEMORIES_MOD_ALLOW_OTHER_CLANG=1 an older one. Apple's
    has no LLVM version to compare: it is tried, and the first IR that does
    not parse says so (hint_for)."""
    if major is None:
        return is_apple_clang(path)
    return major == pass_major or (major < pass_major and os.environ.get(ALLOW_OTHER_CLANG) == "1")


def pick_clang(candidates, pass_major):
    """The first of `candidates` ([(clang, its major)]) that fits the pass and
    has ld.lld: one of the pass's own LLVM first, then (allowed) an older one,
    then any Apple's."""
    for rank in range(3):
        for path, major in candidates:
            mine = 2 if major is None else 0 if major == pass_major else 1
            if mine == rank and clang_fits(path, major, pass_major) and linker_for(path):
                return path
    return None


_warned = set()


def warn_other_clang(path, pass_major):
    """Once per clang: it is not the pass's LLVM (MEMORIES_MOD_ALLOW_OTHER_CLANG)."""
    major = clang_major(path)
    if major is not None and major != pass_major and path not in _warned:
        _warned.add(path)
        print(f"build_mod: warning: {path} is clang {major} and the guest pass is LLVM {pass_major} "
              f"({ALLOW_OTHER_CLANG}): the pass reads its IR, but it may not read the IR the pass writes",
              file=sys.stderr)


def ndk_clang():
    """The Android NDK's clang (ANDROID_NDK_ROOT, ANDROID_NDK_HOME), or None."""
    for variable in ("ANDROID_NDK_ROOT", "ANDROID_NDK_HOME"):
        root = os.environ.get(variable)
        if not root:
            continue
        for found in sorted(glob.glob(os.path.join(root, "toolchains", "llvm", "prebuilt", "*", "bin", "clang*"))):
            if os.path.splitext(os.path.basename(found))[0] == "clang":
                return found
    return None


@functools.lru_cache(maxsize=None)
def translated_candidates():
    """[(clang, its major version)] build_mod.py tries for aarch64-translated,
    in order: MEMORIES_MOD_CC alone when it is set; else the clang the other
    targets use, then the Android NDK's."""
    named = os.environ.get("MEMORIES_MOD_CC")
    if named:
        paths = [resolve(named)]
    else:
        found = find_programs(None)
        paths = [found[0] if found and found[1] else None, ndk_clang()]
    return [(path, clang_major(path))
            for path in dict.fromkeys(path for path in paths if path and "clang" in os.path.basename(path))]


def translated_clang():
    """The clang for aarch64-translated: the first candidate no newer than
    the guest pass's LLVM (pick_clang), with ld.lld beside it or on PATH."""
    try:
        major = guest_pass_major()
    except PassCannotRun as error:
        sys.exit(str(error))
    path = pick_clang(translated_candidates(), major)
    if path:
        warn_other_clang(path, major)
        return path
    sys.exit(translation_problem(None) or "build_mod: no clang for aarch64-translated")


def guest_ir_tool(build_tool=True):
    """(the guest pass's executable, None), or (None, why there is none).
    In the repository llvm_guest.py builds it, which can mean fetching
    LLVM 21 first: only with `build_tool` (the target asked for by name, a
    game build); otherwise only one built before is taken."""
    named = os.environ.get("MEMORIES_GUEST_IR")
    if named:
        return (named, None) if os.path.isfile(named) else (None, f"MEMORIES_GUEST_IR names {named}, which is not there")
    shipped = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "memories-guest-ir" + (".exe" if os.name == "nt" else ""))
    if os.path.isfile(shipped):
        return shipped, None
    if SHIPPED:
        return None, "the SDK has no tools/memories-guest-ir and MEMORIES_GUEST_IR is not set"
    import llvm_guest
    llvm_guest.FETCH = False   # a mod build never downloads: it says what to get
    if not build_tool:
        built = llvm_guest.OUT / ("memories-guest-ir" + (".exe" if os.name == "nt" else ""))
        if built.is_file():
            return str(built), None
        return None, ("it is not built yet in this checkout (python tools/pc/llvm_guest.py builds it, or name "
                      f"--target {TRANSLATED}, which does)")
    try:
        return str(llvm_guest.compiler()), None
    except (Exception, SystemExit) as error:   # no LLVM 21 libraries, no C++ compiler: said below
        return None, f"tools/pc/llvm_guest.py could not build it ({str(error).strip() or type(error).__name__})"


GUEST_IR_HELP = (
    "The guest pass is memories-guest-ir (tools/pc/llvm/guest_ir.cpp of the game's source). A release's SDK "
    "carries it in its tools folder, built for the clang of the toolchain the SDK names (llvm-mingw "
    + LLVM_MINGW_RELEASE + ", clang " + str(LLVM_MINGW_CLANG) + "): unpack the whole SDK, or a newer release's. In a "
    "checkout of the source, `python tools/pc/llvm_guest.py` builds it against the game's LLVM 21.1.8 and "
    "prints where it is (on Windows with MEMORIES_GUEST_LLVM naming an LLVM 21.1.8 build and "
    "MEMORIES_GUEST_CXX the C++ compiler it was built with; on Linux and macOS it uses the official LLVM "
    "21.1.8 release). Set MEMORIES_GUEST_IR to a pass, or put it in the SDK's tools folder, and run "
    "build_mod.py again.")


def clang_problem(candidates, tool, major):
    """Why no clang of `candidates` goes with the guest pass `tool` (LLVM
    `major`), and which would."""
    found = "; ".join(f"{path} is clang {each}" if each is not None else
                      f"{path} is Apple's clang (no ld.lld beside it or on PATH)" if is_apple_clang(path) else
                      f"{path} gives no version" for path, each in candidates) or "no clang was found"
    ways = []
    if major == LLVM_MINGW_CLANG:
        ways.append(f"llvm-mingw {LLVM_MINGW_RELEASE}'s clang is {LLVM_MINGW_CLANG}, the toolchain the game is built "
                    f"with ({LLVM_MINGW_DOWNLOADS['windows' if os.name == 'nt' else 'linux']}; put its bin folder on "
                    "PATH or set MEMORIES_MOD_CC to its clang)")
    if major == 21:
        ways.append("the Android NDK r29's clang is 21 (set ANDROID_NDK_ROOT to the NDK, with MEMORIES_MOD_CC "
                    "unset: build_mod.py then takes its clang when the other one does not fit)")
    ways.append(f"set MEMORIES_MOD_CC to a clang of LLVM {major} with ld.lld beside it")
    ways.append(f"or, at your own risk, {ALLOW_OTHER_CLANG}=1 takes an older clang (the pass reads its IR; it may "
                "not read the IR the pass writes)")
    return (f"build_mod: the Android object ({TRANSLATED}) needs a clang of LLVM {major}, with ld.lld: "
            f"the guest pass ({tool}) is built against LLVM {major}; it cannot read the IR of a newer clang, and "
            f"a clang of another version is not certain to read what it writes ({found}). Use one of: "
            + "; ".join(ways) + ".")


def hint_for(clang, stage):
    """A line for a failure of the guest pass or of the clang on the IR it
    wrote (`stage`: "pass" or "clang"), when the clang could be the cause."""
    try:
        pass_major = guest_pass_major()
    except PassCannotRun:
        return ""   # said already
    if is_apple_clang(clang):
        return (f"\n{clang} is Apple's clang, whose version numbers are not LLVM's, so build_mod.py cannot tell "
                f"whether it matches the guest pass (LLVM {pass_major}). If the error above is about the IR, use an "
                f"LLVM clang of version {pass_major} or older instead (Homebrew: brew install llvm lld, then "
                "MEMORIES_MOD_CC=$(brew --prefix llvm)/bin/clang).")
    major = clang_major(clang)
    if stage == "pass" and major is not None and major > pass_major:
        return f"\n{clang} is clang {major}, newer than the guest pass's LLVM {pass_major}: use a clang {pass_major} or older."
    if stage == "clang" and major is not None and major < pass_major:
        return (f"\n{clang} is clang {major}, older than the guest pass's LLVM {pass_major}, whose IR it may not "
                f"read: if the error above is about the IR, use a clang {pass_major}.")
    return ""


def translation_problem(games, build_tool=True):
    """Why aarch64-translated cannot be built here (a message), or None.
    With `games`, it also finds the game's tables (mod_translation.json and
    mod_signatures.json in the first of `games` that has them) and keeps
    them, with the pass, for the build."""
    # The pass first: which clangs fit depends on the LLVM it is built against.
    tool, why = guest_ir_tool(build_tool)
    if not tool:
        return f"build_mod: the Android object ({TRANSLATED}) needs the game's guest pass: {why}.\n{GUEST_IR_HELP}"
    candidates = translated_candidates()
    try:
        major = guest_pass_major(tool)
    except PassCannotRun as error:
        return str(error)
    if not pick_clang(candidates, major):
        return clang_problem(candidates, tool, major)
    if games is None:
        return None
    for build in games:
        table = os.path.join(build, "mod_translation.json")
        signatures = os.path.join(build, "mod_signatures.json")
        if os.path.isfile(table) and os.path.isfile(signatures):
            with open(table, "rb") as handle:
                table_bytes = handle.read()
            with open(signatures, "rb") as handle:
                signature_bytes = handle.read()
            data = json.loads(table_bytes)
            if data.get("format") != 1:
                return f"build_mod: {table} is of another version of build_mod.py; use the SDK it came with"
            _translation.update(data, tool=tool, signatures=json.loads(signature_bytes),
                                digest=hashlib.sha256(table_bytes + b"\0" + signature_bytes).hexdigest())
            return None
    where = ", ".join(games) or "none named"
    return (f"build_mod: the Android object ({TRANSLATED}) is translated with the game's tables "
            f"(mod_translation.json, mod_signatures.json), and no game build has them ({where}). Build the "
            "Android game first (python tools/pc/build_game32.py --target android-arm64-v8a), use the SDK of an "
            "Android release, or name a build with --game.")


def translation_context():
    if not _translation:
        sys.exit("build_mod: the aarch64-translated tables were not read (translation_problem)")
    return _translation


def translated_libc(build):
    """What the C library lends an aarch64-translated mod directly: the
    pointer-free math only (mod_translation.json); everything that takes a
    pointer goes through the runtime's wrappers, in the export table."""
    try:
        with open(os.path.join(build, "mod_translation.json"), encoding="utf-8") as handle:
            return set(json.load(handle).get("system_math", ()))
    except (OSError, ValueError):
        return set()


def guest_pass(operation, text, hint="", **options):
    """One operation of the guest pass (llvm_guest.process, without its cache);
    `hint` is added to what a failure says."""
    tool = translation_context()["tool"]
    with tempfile.TemporaryDirectory() as folder:
        source, output, config = (os.path.join(folder, name) for name in ("input.ll", "output", "options.json"))
        with open(source, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        with open(config, "w", encoding="utf-8") as handle:
            json.dump(options, handle)
        try:
            result = subprocess.run([tool, operation, source, output, config], capture_output=True, text=True,
                                    encoding="utf-8", errors="replace")
        except OSError as error:
            sys.exit(str(cannot_run(tool, error)))
        if result.returncode:
            sys.exit(f"build_mod: the guest pass ({operation}) failed:\n{result.stderr.strip()}{hint}")
        with open(output, encoding="utf-8") as handle:
            result = handle.read()
    return json.loads(result) if operation == "inspect" else result


def check_signatures(found, expected, source):
    """The checks build_mod_arm64.validate_imports makes for macOS: the
    entry's ABI, and each game function the unit calls as the game defines
    it. Returns whether the unit defines MemoriesModInit."""
    has_entry = False
    for name, signature in found.items():
        entry = name == "MemoriesModInit" and signature["definition"]
        if not entry and (signature["definition"] or name not in expected):
            continue
        if signature["calling_convention"] != 0 or signature["unsupported_abi_attributes"]:
            sys.exit(f"build_mod: {source}: {name} has a calling convention or ABI attributes the game does not use")
        if entry:
            pointer = {"kind": "ptr", "signext": False}
            if signature["result"] != "i32" or signature["variadic"] or signature["parameters"] != [pointer, pointer]:
                sys.exit("build_mod: MemoriesModInit must return int and accept "
                         "(const MemoriesModHost *, MemoriesMod *)")
            has_entry = True
            continue
        for key in ("result", "result_signext", "parameters", "variadic", "calling_convention"):
            if signature[key] != expected[name][key]:
                sys.exit(f"build_mod: {source}: {name} is not declared as the game defines it ({key} differs)")
    return has_entry


def translate_unit(command, source, obj, index):
    """An aarch64-translated unit (above): to IR, through the guest pass,
    then to the object with the translated game's own object flags.
    Returns whether it defines MemoriesModInit."""
    context = translation_context()
    raw = obj[:-2] + ".raw.ll"
    run(command + ["-S", "-emit-llvm", source, "-o", raw])
    with open(raw, encoding="utf-8") as handle:
        text = handle.read()
    text = guest_pass("normalize", text, hint_for(command[0], "pass"), renames=context["aliases"],
                      drop_definitions=[], export_functions=[])
    entry = check_signatures(guest_pass("inspect", text)["signatures"], context["signatures"], source)
    text = guest_pass("normalize", text, renames=context["runtime"], drop_definitions=[], export_functions=[])
    text = guest_pass("translate", text, pins=context["pins"], registration=f"MemoriesModRegisterUnit_{index}",
                      mod_unit=True)
    ir = obj[:-2] + ".ll"
    with open(ir, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    try:
        run([command[0], *TARGET_FLAGS[TRANSLATED], "-O2", "-fno-strict-aliasing",
             "-Wno-unused-command-line-argument", "-x", "ir", "-c", ir, "-o", obj])
    except SystemExit as failed:
        sys.exit(f"{failed.code}{hint_for(command[0], 'clang')}")
    return entry


def registration_object(clang, count, folder):
    """MemoriesModRegisterGlobals and MemoriesModUnregisterGlobals: each
    unit's routine, and each one's _Unregister in the other order."""
    routines = [f"MemoriesModRegisterUnit_{index}" for index in range(count)]
    source = os.path.join(folder, "registration.c")
    with open(source, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("".join(f"extern void {name}(void); extern void {name}_Unregister(void);\n" for name in routines)
                     + "void MemoriesModRegisterGlobals(void) {\n" + "".join(f"    {name}();\n" for name in routines)
                     + "}\nvoid MemoriesModUnregisterGlobals(void) {\n"
                     + "".join(f"    {name}_Unregister();\n" for name in reversed(routines)) + "}\n")
    obj = os.path.join(folder, "registration.o")
    run([clang, *TARGET_FLAGS[TRANSLATED], "-O2", "-ffreestanding", "-nostdinc", "-c", source, "-o", obj])
    return obj


@functools.lru_cache(maxsize=None)
def find_objcopy(target=None):
    """llvm-objcopy for the compiler in use (objcopy_for), or None."""
    if target == TRANSLATED:
        return objcopy_for(translated_clang())
    path, clang, _ = programs()
    return objcopy_for(path if clang else None)


def tag_abi(output, target):
    """The `.memories.abi` section: the target's name, NUL-terminated, which
    the game's loader holds against its own (object_loader.c)."""
    objcopy = find_objcopy(target if target == TRANSLATED else None)
    if not objcopy:
        sys.exit("build_mod: llvm-objcopy (beside clang, or on PATH) is needed for the 64-bit targets\n" +
                 toolchain_help(["llvm-objcopy"], wanted=(target,), outcome=False))
    with tempfile.NamedTemporaryFile("wb", suffix=".abi", delete=False) as handle:
        handle.write(target.encode() + b"\0")
    try:
        run([objcopy, "--add-section", f".memories.abi={handle.name}", output])
    finally:
        os.remove(handle.name)


def build(directory, out_dir=None, objects_dir=None, extra_flags=(), games=None, quiet=False, target="i386",
          build_tool=True):
    """Build the mod in `directory` for `target`; returns the path of its
    object, or None when it has no C (a data-only mod). The object is built
    once per inputs_key, into tmp/pc/mod-build/<mod>-<key>, and copied to
    `out_dir` (default: the mod's directory) when what is there differs. It
    is checked against the target's game builds (`games`, default
    game_builds(target)) every time, reused or not: what a game lends comes
    from the game's own sources, which the key does not cover."""
    sources = sorted(glob.glob(os.path.join(directory, "*.c")))
    if not sources:
        return None
    prune()
    if games is None:
        games = game_builds(target)
    if target == TRANSLATED:
        problem = translation_problem(games, build_tool)
        if problem:
            sys.exit(problem)
    name = library_name(directory, target)
    output = os.path.join(out_dir or directory, name)
    extra_flags = ["-I", directory, *extra_flags]
    mod = os.path.basename(os.path.abspath(directory))
    memo = memo_folder(directory, sources, extra_flags, target)
    key = recall(memo)
    state = "up to date"
    try:
        if key is None or not os.path.exists(os.path.join(CACHE, f"{mod}-{key[:16]}", name)):
            # The key again from the preprocessor: a memo is never trusted to
            # say what an object is made of, only that it is already built.
            os.makedirs(CACHE, exist_ok=True)
            stage = tempfile.mkdtemp(prefix=f".{mod}-", dir=CACHE)
            try:
                parts = os.path.join(stage, "parts")
                os.makedirs(parts)
                key, preprocessed, read, started = inputs_key(directory, sources, extra_flags, parts, target)
                remember(memo, key, read, started)
                entry = os.path.join(CACHE, f"{mod}-{key[:16]}")
                if not os.path.exists(os.path.join(entry, name)):
                    publish(entry, name, stage, preprocessed, objects_dir or parts, extra_flags, games, target)
                    state = f"{len(sources)} source{'s' if len(sources) != 1 else ''}"
            finally:
                shutil.rmtree(stage, ignore_errors=True)
        entry = os.path.join(CACHE, f"{mod}-{key[:16]}")
        cached = os.path.join(entry, name)
        if state == "up to date":
            try:
                with open(cached + ".undefined") as handle:
                    undefined = set(handle.read().split())
            except OSError:
                undefined = None
            checked = check(cached, games, undefined, target)
        else:   # publish checked it
            checked = any(provided(build, target) is not None for build in games)
    except SystemExit:
        if os.path.exists(output):
            os.remove(output)   # not left there to pass for this mod's object
        raise
    if not os.path.exists(output) or not filecmp.cmp(cached, output, shallow=False):
        os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)   # "library": "sub/rules"
        shutil.copyfile(cached, output)
    if not quiet:
        unchecked = "" if checked else f"; not checked: no {target} game build or exports list was found"
        print(f"{output}: {state} ({os.path.relpath(entry, ROOT)}){unchecked}")
    return output


def publish(entry, name, stage, preprocessed, objects_dir, extra_flags, games, target="i386"):
    """Compile the preprocessed sources into `stage`, check the object, and
    rename its folder to `entry`: a folder there is always a whole object
    that passed, with the names it leaves undefined beside it
    (<library>.undefined). When another checkout gets there first with the
    same key, its object is the same and this one is dropped."""
    staged = os.path.join(stage, "object")
    output = compile_object(preprocessed, os.path.join(staged, name), objects_dir,
                            [*extra_flags, "-x", "cpp-output"], target)
    undefined = undefined_names(output)
    check(output, games, undefined, target)
    with open(output + ".undefined", "w") as handle:
        handle.writelines(symbol + "\n" for symbol in sorted(undefined))
    for attempt in range(40):
        try:
            os.rename(staged, entry)
            return
        except OSError:
            if os.path.exists(os.path.join(entry, name)):
                return
            if os.path.isdir(entry):
                shutil.rmtree(entry, ignore_errors=True)   # emptied by hand: not an object
            if attempt == 39:
                raise
            time.sleep(0.25)   # a virus scanner still holding the new file (Windows)


@functools.lru_cache(maxsize=None)
def prune():
    """Once per process: drop what nothing will use. A staging folder
    (.<mod>-XXXX) a day old is from a build that was stopped; a memo folder
    no build has found a key in for 30 days is from sources that are gone.
    Objects stay: each is small, and one may be about to be copied."""
    now = time.time()
    for path in glob.glob(os.path.join(CACHE, ".*-*")) + glob.glob(os.path.join(CACHE, ".memo", "*")):
        try:
            age = now - os.path.getmtime(path)
        except OSError:
            continue
        if age > (30 if os.path.basename(os.path.dirname(path)) == ".memo" else 1) * 86400:
            shutil.rmtree(path, ignore_errors=True)


def undefined_names(output):
    """The names the object leaves for the game to lend."""
    reader = tool("llvm-readelf") or tool("readelf") or next(
        (found for path, _ in translated_candidates() if (found := beside(path, "llvm-readelf"))), None)
    undefined = set()
    for line in run([reader, "-sW", output]).splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[6] == "UND" and parts[4] != "WEAK":
            undefined.add(parts[7])
    return undefined


def check(output, games, undefined=None, target="i386"):
    """What the object leaves undefined must be there on every game build
    of its target. Returns whether any build (or list) was there to check
    against."""
    if undefined is None:
        undefined = undefined_names(output)
    for bad, why in (("_GLOBAL_OFFSET_TABLE_", "it is position-independent"),
                     ("__stack_chk_fail", "it uses the stack protector"),
                     ("__stack_chk_fail_local", "it uses the stack protector")):
        if bad in undefined:
            sys.exit(f"build_mod: {output}: {why}; build it with the flags in this script")
    checked = False
    for build in games:
        names = provided(build, target)
        if names is None:
            continue
        checked = True
        missing = sorted(undefined - names)
        if missing:
            try:
                build = os.path.relpath(build, ROOT)
            except ValueError:
                pass   # on another drive (Windows)
            sys.exit(f"build_mod: {output} needs names the game at {build} "
                     f"does not provide: {' '.join(missing)}")
    return checked


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory", help="the mod's directory (its mod.json and .c files)")
    parser.add_argument("--out", help="where the objects go (default: the mod's directory)")
    parser.add_argument("--target", action="append", choices=TARGETS + ("macos",),
                        help="a target to build for (repeatable; default: i386, x86_64-windows and "
                             "aarch64-translated, the Android app's, or i386 alone when the compiler is gcc). "
                             "aarch64 (the fixed-memory Android game, not shipped) and macos (the translated "
                             "macOS ARM64 dylib, build_mod_arm64.py) are built only when named")
    parser.add_argument("--ir-out", metavar="DIR",
                        help="keep the LLVM IR of an aarch64-translated build in DIR: each unit's <name>.raw.ll "
                             "from clang and <name>.ll as the guest pass wrote it (a build that is already up to "
                             "date writes none)")
    parser.add_argument("--game", action="append",
                        help="a game build directory to check the mod's names against (repeatable, for one "
                             "--target; default: the target's builds in tmp/pc when they exist)")
    options = parser.parse_args()
    if not glob.glob(os.path.join(options.directory, "*.c")):
        print(f"{options.directory}: no C sources; a data-only mod needs no build")
        return
    # The 64-bit targets need clang with ld.lld and llvm-objcopy. Not asked
    # for by name, they are left out without them: the i386 object is built,
    # as before there were others, and the modder told what to get.
    # aarch64-translated alone, asked for by name, needs none of that: its
    # own clang (the NDK's will do) and the guest pass (translation_problem).
    if options.target and set(options.target) == {TRANSLATED}:
        problem = translation_problem(options.game or game_builds(TRANSLATED))
        if problem:
            sys.exit(problem)
        wide = True
    else:
        wide = programs()[1] and find_objcopy()
    asked = [target for target in options.target or () if target in WIDE_TARGETS and target != TRANSLATED]
    if asked and not wide:
        sys.exit(toolchain_help(missing_64bit() or ["llvm-objcopy"], wanted=asked))
    targets = options.target or (list(DEFAULT_TARGETS) if wide else ["i386"])
    left_out = [target for target in DEFAULT_TARGETS if target not in targets] if not options.target else []
    if options.game and len(targets) != 1:
        parser.error("--game checks one target's names: name it with --target")
    if TRANSLATED in targets and TRANSLATED not in (options.target or ()):
        # Not asked for by name: left out, with what it needs, when this
        # machine cannot build it (clang newer than 21, no guest pass, no
        # Android game's tables), as the 64-bit targets are without clang.
        problem = translation_problem(options.game or game_builds(TRANSLATED), build_tool=False)
        if problem:
            print(f"{problem}\nThe other objects are built; the Android one is not.", file=sys.stderr)
            targets.remove(TRANSLATED)
            left_out.append(TRANSLATED)
    built = []
    for target in targets:
        if target == "macos":
            from build_mod_arm64 import build as build_arm64_mod
            build_arm64_mod(options.directory, options.out,
                            game=options.game[0] if options.game else None)
            continue
        output = build(options.directory, options.out, objects_dir=options.ir_out if target == TRANSLATED else None,
                       games=options.game, target=target, build_tool=target in (options.target or ()))
        if not output:
            print(f"{options.directory}: no C sources; a data-only mod needs no build")
            break
        built.append(target)
        if not options.target and not wide:
            print(toolchain_help(missing_64bit() or ["llvm-objcopy"], built=output), file=sys.stderr)
    # Which objects came out, last, for a modder who looks only at the end
    # (sdk/build-mod.bat): what an earlier message says was left out, and why.
    if built:
        print(f"build_mod: built {', '.join(built)}" +
              (f"; NOT built: {', '.join(left_out)} (the message above says why)" if left_out else ""))


if __name__ == "__main__":
    main()
