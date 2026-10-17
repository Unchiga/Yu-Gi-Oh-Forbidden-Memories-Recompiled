#!/usr/bin/env python3
"""Compile and link the resident game C as a 32-bit Linux or Windows executable,
or as the Android app's libgame.so (--target android-<abi>).

Bring-up driver for the fixed-address memory model (src/pc/guest/image.h):
  * every game unit is compiled with the host GCC as ILP32;
  * data symbols the units leave undefined or tentative are pinned to their
    retail addresses, read from the matching build's ELF;
  * undefined functions get generated stubs that name themselves and exit,
    unless a native source under src/pc already defines them.
The retail addresses come from config/pc/guest_addresses.txt, which this
script writes from the matching build's ELFs (make match match-overlays)
whenever they are present, so a checkout without the MIPS toolchain builds too.

On Windows the toolchain is llvm-mingw (i686-w64-mingw32-clang, lld and the
llvm binutils) and the libraries come from tools/pc/build_win32_deps.py. PE
differs from ELF in ways the link below works around: C symbols carry a
leading underscore; sections cannot be placed at chosen addresses, so the
fixed game sections (save states across rebuilds) are not available; the
section renames edit the COFF headers directly (rename_coff_sections) and
__start_/__stop_ come from grouped marker sections; overrides win by link order instead of weakened symbols.

--target windows-x64 is the native 64-bit Windows build (notes/pc-build.md,
"64-bit Windows"): x86_64-w64-mingw32-clang, clang 21 or later, the game's
stored pointers 4 bytes wide through G32 (src/port_ptr.h), and the image
linked at 0x40000000 without ASLR so that host code stays below 4 GB and
within reach of the pinned guest addresses. Its default build directory is
tmp/pc/win64.

On Android (--target android-arm64-v8a: 64-bit ARM; android-x86 builds for
development and is not packaged) the toolchain is the NDK's clang and LLVM
tools, and the libraries come from tools/pc/build_android_deps.py. The arm64
app's game is the translated one (--android-memory translated, the default):
tools/pc/build_arm64.py --target android-arm64-translated builds libgame.so,
linked at 0 with no fixed host address, and build_android_translated adds the
loader and the APK. --android-memory fixed (MEMORIES_ANDROID_MEMORY=fixed)
builds the fixed game this file builds for the other targets, with G32 as
windows-x64, described next. The fixed game is a position-independent shared
object, libgame.so, linked at a fixed base; libmain.so, which SDL's Java shell
loads and whose SDL_main it runs, is a loader that puts the game at that base
(src/pc/platform/android_loader.c) and starts the port's main through
src/pc/platform/android.c. It is ELF, so the Linux steps apply, except that
sections cannot be placed at chosen addresses (as on Windows). The APK is packaged beside it (<build>/memories-<abi>.apk, by
package_android.py) with the SDK's build tools; notes/pc-build.md, "Android"."""
import argparse, concurrent.futures, csv, filecmp, glob, hashlib, json, os, re, shutil, struct, subprocess, sys
import build_process
from build_config import BACKENDS, GATED_MODULES, MODULE_CONFIG, MODULES, TARGETS, game_sources, native_sources
import ptr32_stores

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ELF = "tmp/project-build/SLUS_014.11.elf"
ADDRESSES = "config/pc/guest_addresses.txt"
# --target windows on Linux cross-compiles with the llvm-mingw that
# build_win32_deps.py fetches; the rest of this file only asks WINDOWS. Read
# before argparse because the flags and tools below depend on it.
TARGET = next((sys.argv[i + 1] for i, word in enumerate(sys.argv[:-1]) if word == "--target"),
              os.environ.get("MEMORIES_TARGET") or ("windows" if sys.platform == "win32" else "linux"))
X64 = TARGET == "windows-x64"
WINDOWS = TARGET in ("windows", "windows-x64")
# android-<abi>: the ABI is a parameter of everything Android below.
ANDROID_ABI = TARGET[len("android-"):] if TARGET.startswith("android-") else None
ANDROID = ANDROID_ABI is not None
# 64-bit ARM (AArch64): the Android arm64-v8a build. The game's stored
# pointers are 4 bytes wide through G32 as on windows-x64 (WIDE: the
# 64-bit builds); its assembly is src/pc/guest/*_aarch64.S, and its branch
# thunks are clang's -mharden-sls=blr ones (check_arm_branches).
A64 = ANDROID_ABI == "arm64-v8a"
WIDE = X64 or A64
# The ABIs an APK is made for. android-x86 (the emulator images M1-M4 were
# made on) still builds, as a development target, but is neither run nor
# shipped.
ANDROID_PACKAGED = ("arm64-v8a",)
# arm64: every C unit compile_unit builds (the game's and the port's)
# through tools/pc/ptr32_stores.py (an LLVM AArch64 bug, notes/pc-build.md
# "Android arm64"); main() checks the compiler first. The generated
# guest_branches.c, stubs and mod_exports.c hold no G32 stores and are
# compiled directly.
PTR32_PASS = A64
if TARGET not in ("linux", "windows", "windows-x64") and not ANDROID:
    sys.exit(f"--target {TARGET}: linux, windows, windows-x64 or android-<abi>")
# tools/pc/build_win32_deps.py (--arch x86_64 for the 64-bit build)
WIN32_DEPS = "tmp/pc/win64-deps" if X64 else "tmp/pc/win32-deps"
CC, OBJCOPY, NM, READELF, OBJDUMP = (("x86_64-w64-mingw32-clang" if X64 else "i686-w64-mingw32-clang", "llvm-objcopy",
                                      "llvm-nm", "llvm-readelf", "llvm-objdump")
                                     if WINDOWS else ("gcc", "objcopy", "nm", "readelf", "objdump"))
if ANDROID:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import build_android_deps
    if ANDROID_ABI not in build_android_deps.READY:
        sys.exit(f"--target android-{ANDROID_ABI}: " + build_android_deps.NOT_READY.get(
            ANDROID_ABI, f"unknown ABI; known: {', '.join('android-' + abi for abi in build_android_deps.READY)}"))
    CC, OBJCOPY, NM, READELF, OBJDUMP = (os.path.join(build_android_deps.llvm_bin(), tool) for tool in
                                         ("clang", "llvm-objcopy", "llvm-nm", "llvm-readelf", "llvm-objdump"))
    ANDROID_DEPS = os.path.join("tmp", "pc", "android-deps", ANDROID_ABI)
ANDROID_COMPAT = "src/pc/compat/android/android_compat.h"
PREFIX = "_" if WINDOWS and not X64 else ""  # C symbol names in the object files
# Linux builds are made against Debian 11's libraries
# (tools/pc/build_linux_sysroot.py fetches them), not this machine's: the
# executable then asks for glibc 2.29 rather than whatever is installed here,
# and runs on other people's Linux as well. FreeType, fontconfig and libpng
# are linked in. The one a developer runs is the one that is shared.
PORTABLE = not WINDOWS and not ANDROID
if PORTABLE:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import build_linux_sysroot
if WINDOWS:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import build_win32_deps
    build_win32_deps.use_toolchain()
if sys.platform == "win32":
    # The paths below are written, compared and turned into object names
    # with forward slashes; Windows glob returns backslashes.
    _glob = glob.glob
    glob.glob = lambda *args, **kwargs: [path.replace(os.sep, "/") for path in _glob(*args, **kwargs)]
CFLAGS = ["-m32", "-std=gnu11", "-fpermissive", "-w", "-O0", "-g", "-fno-strict-aliasing",
          # Room for a mod to hook any game function (src/pc/mods/hooks.c).
          "-fpatchable-function-entry=8,6",
          "-fwrapv", "-fcommon", "-fno-pie", "-fno-stack-protector", "-DMEMORIES_PC",
          "-D_LANGUAGE_C", "-DLANGUAGE_C", "-Isrc",
          # PGXP's addPrim, for game units only (see the header).
          "-include", "src/pc/compat/pgxp_game.h"]
if WINDOWS:
    # -fpermissive is GCC's; clang needs this one of its errors turned off.
    # -mno-ms-bitfields: MinGW lays bitfields out as MSVC does, where fields
    # of different declared types do not share a unit; the game's layouts are
    # GCC's (GsOT_TAG's `unsigned p:24; unsigned char num:8` is 4 bytes, not
    # 8, or every LIBGS ordering table has the wrong stride).
    # -gcodeview: the debug info goes in the PDB the link writes beside the
    # executable (memories-pc.pdb), which Windows debuggers and profilers
    # (Visual Studio, WinDbg, Superluminal) read; they do not read DWARF.
    CFLAGS = [f for f in CFLAGS if f not in ("-m32", "-fno-pie")] + ["-Wno-incompatible-pointer-types", "-mno-ms-bitfields",
                                                                    "-gcodeview"]
# The 64-bit build: -fms-extensions for clang's __ptr32 (G32 in
# src/port_ptr.h), and src/pc/compat/ptr32.h first, which keeps mingw's
# headers from defining __ptr32 away. -fno-jump-tables: LLVM can turn a
# switch that stores guest addresses into a table of 32-bit pointers that it
# cannot emit (upstream #6623); the 32-bit builds make compare trees under
# the branch thunks anyway.
X64_FLAGS = ["-include", "src/pc/compat/ptr32.h", "-fms-extensions", "-fno-jump-tables"]
if WIDE:
    CFLAGS = X64_FLAGS + CFLAGS
# -O0 for game units: original busy-waits poll non-volatile globals that the
# VBlank handler updates, and must not be hoisted out of their loops.
NATIVE_CFLAGS = ["-m32", "-std=gnu11", "-O2", "-g", "-Wall", "-fno-pie", "-fno-omit-frame-pointer", "-fno-strict-aliasing",
                 "-Wno-builtin-declaration-mismatch", "-DMEMORIES_PC", "-D_LANGUAGE_C", "-DLANGUAGE_C", "-Isrc",
                 "-I/usr/include/freetype2",
                 # 64-bit stat/readdir/lseek: the 32-bit calls fail with
                 # EOVERFLOW on a file whose inode number needs more than 32
                 # bits (btrfs, XFS, NFS, a mounted Windows drive), so the
                 # disc, the mods and the user folder could not be read there.
                 "-D_FILE_OFFSET_BITS=64"]
if WINDOWS:
    NATIVE_CFLAGS = [f for f in NATIVE_CFLAGS if f not in ("-m32", "-fno-pie", "-I/usr/include/freetype2",
                                                           "-Wno-builtin-declaration-mismatch",
                                                           "-D_FILE_OFFSET_BITS=64")] + [
        f"-I{WIN32_DEPS}/sdl/include", f"-I{WIN32_DEPS}/include", f"-I{WIN32_DEPS}/include/freetype2",
        "-mno-ms-bitfields",  # the game's structures, shared with native code (see CFLAGS)
        "-gcodeview"]
    if X64:
        # Code mods are x86-64 objects of their own here (build_mod.py
        # --target x86_64-windows; src/pc/mods/object_loader.c).
        NATIVE_CFLAGS = X64_FLAGS + NATIVE_CFLAGS
# Every unit's indirect calls and jumps go through __x86_indirect_thunk_<reg>
# (src/pc/guest/branch_thunks.c), which sends a target in guest memory to its
# native function: tables in the retail data image hold MIPS addresses, and
# a call through one must not depend on DEP faulting it (notes/pc-build.md).
# clang also turns switch jump tables into compare trees; GCC jumps through
# the thunk for those, which lets a host target straight through.
BRANCH_THUNKS = (["-mretpoline-external-thunk"] if WINDOWS else
                 build_android_deps.READY[ANDROID_ABI]["thunks"] if ANDROID else
                 ["-mindirect-branch=thunk-extern", "-mindirect-branch-register"])
if ANDROID:
    # clang for the ABI at the deps' API level, position-independent (a
    # shared object; Android refuses text relocations), with GCC's leniency
    # for the game units as on Windows. The ABI's own flags follow
    # (-fsigned-char: MIPS char is signed, ARM's is not).
    ANDROID_FLAGS = [f"--target={build_android_deps.TRIPLES[ANDROID_ABI]}{build_android_deps.API}", "-fPIC",
                     *build_android_deps.READY[ANDROID_ABI]["flags"]]
    CFLAGS = [f for f in CFLAGS if f not in ("-m32", "-fno-pie")] + ANDROID_FLAGS + [
        "-Wno-incompatible-pointer-types", "-Wno-int-conversion", "-Wno-implicit-function-declaration",
        "-Wno-implicit-int"]
    NATIVE_CFLAGS = [f for f in NATIVE_CFLAGS if f not in ("-m32", "-fno-pie", "-I/usr/include/freetype2",
                                                           "-Wno-builtin-declaration-mismatch")] + ANDROID_FLAGS + [
        f"-I{ANDROID_DEPS}/include", f"-I{ANDROID_DEPS}/include/freetype2",
        # Fontconfig's few calls, answered from the system fonts, and what
        # bionic lacks below the API level (memfd_create, iconv).
        "-Isrc/pc/compat/android", "-include", ANDROID_COMPAT]
    if A64:
        # G32 as on windows-x64; code mods are aarch64 objects of their own
        # (build_mod.py --target aarch64; src/pc/mods/object_loader.c).
        NATIVE_CFLAGS = X64_FLAGS + NATIVE_CFLAGS
        # No fused multiply-add: AArch64 has it and clang contracts a*b+c
        # into it by default, x86 (no -mfma) does not, so float code (LIBPRESS's
        # IDCT holds 83 of them) rounded differently from the other builds: the
        # menus replay had two VRAM pixels off in 36 of its 1039 frames.
        CFLAGS = CFLAGS + ["-ffp-contract=off"]
        NATIVE_CFLAGS = NATIVE_CFLAGS + ["-ffp-contract=off"]
if A64:
    # Room for a mod's hook as AArch64 has it (src/pc/mods/hooks.c): three
    # nop words before each game function's entry and one at it.
    CFLAGS = [("-fpatchable-function-entry=4,3" if f.startswith("-fpatchable-function-entry") else f) for f in CFLAGS]
# Floating point in SSE registers, as clang does for the Windows build: GCC's
# 32-bit default is the x87 with its 80-bit intermediates, which rounded the
# MDEC's IDCT (libpress.c) a step apart from Windows and so broke replays
# recorded on one when played on the other. Android's clang does the same
# (its x86 ABI has SSSE3; ARM has no x87).
FLOAT_MATH = [] if WINDOWS or ANDROID else ["-msse2", "-mfpmath=sse"]
CFLAGS = CFLAGS + BRANCH_THUNKS + FLOAT_MATH
NATIVE_CFLAGS = NATIVE_CFLAGS + BRANCH_THUNKS + FLOAT_MATH
if PORTABLE:
    SYSROOT_COMPILE, SYSROOT_LINK = build_linux_sysroot.flags()
    CFLAGS = CFLAGS + SYSROOT_COMPILE
    NATIVE_CFLAGS = [f for f in NATIVE_CFLAGS if f != "-I/usr/include/freetype2"] + SYSROOT_COMPILE + [
        "-I" + os.path.join(build_linux_sysroot.SYSROOT, "usr/include/freetype2")]
# Window backends (src/pc/platform): SDL3 when its 32-bit static build exists
# (see notes/pc-build.md), else X11. --backend or MEMORIES_BACKEND picks.
SDL_BUILD = "tmp/pc/sdl-m32-portable"   # build_linux_sysroot.py
SDL_SOURCE = "tmp/pc/sdl-source/SDL3-3.4.16"
ANDROID_BACKEND = {"src/pc/render/present_pass.c": "src/pc/render/gl_desktop_none.c"}
# Android's libmain.so is only a loader (src/pc/platform/android_loader.c)
# that puts the game, libgame.so, at the address it is linked at: save
# states and crash symbols then hold from one launch to the next. The range
# is free in every app process seen (below ART's heap at 0x12C00000, above
# the fixed game sections the Linux build places from 0x01000000, clear of
# the guest's own ranges); the span is checked after the link. 64 MiB on
# every ABI: the 32-bit image outgrew 32 MiB (0x2201000 bytes, most of it
# .bss, on master after the 64-bit Windows build), and 0x0C000000 is still
# below ART's heap.
ANDROID_LOADER = "src/pc/platform/android_loader.c"
# arm64-v8a: ART fills a 64-bit app process's low 4 GB from the bottom up:
# on a Xiaomi 11T Pro (Android 14, heapsize 512m) its heap at 0x02000000-
# 0x22000000, a free list at 0x42000000, the JIT caches at 0x62000000-
# 0x6A000000, its boot image and spaces at 0x6FCFC000-0x76000000, and nothing
# from there to 4 GB but a sentinel page at 0xEBAD6000. A bigger heap moves
# all of that up, so the game goes above the guest (0x80000000-0xB0800000),
# at 0xC0000000: below 4 GB, so its function addresses fit the game's 4-byte
# slots (zero-extended, G32 is __uptr); bit 30 set, the branch thunks' fast
# path; outside every guest range the port tests (they are explicit ranges).
# The image is about 35 MiB (most of it .bss).
ANDROID_GAME_BASE = 0xC0000000 if A64 else 0x08000000
ANDROID_GAME_SPAN = 0x04000000
NATIVE = native_sources(TARGETS[TARGET], None)

# Same contract as the host C library, so the host's version is used directly.
# Runtime-loaded modules linked into the executable: name, sources, identifier
# word at the start of the image, and load bank. main_menu has its load address
# (0x80180000) to itself, so it is linked like resident code (bank 0). The
# 0x80168000 modules replace one another there; src/pc/guest/modules.c
# explains what that takes. name_entry is an entry into the password image.
# The overworld's two packages (before and after the coup) are one program:
# their images differ only in the data blob behind the C, which stays in guest
# memory, so one module serves both, configured from the first.
# Modules entered only through a native gate that checks the delivered bytes
# first (src/pc/overlays/duel_effects.c, credits.c): their guest addresses
# stay out of Memories_FunctionMap, so a call into a modded image is
# interpreted instead. They must have no variables of their own (theirs stay
# in guest memory), so they are left out of the module registry too, which
# would otherwise tell the interpreter their range holds native code.

# Save states outlive native rebuilds because everything a state can point at
# in the game objects stays put (src/pc/guest/state.h): their code and
# variables are collected into sections linked at these addresses.
FIXED_SECTIONS = {"game_text": 0x01000000, "game_rodata": 0x03000000, "game_data": 0x04000000,
                  "game_bss": 0x05000000}
MODULE_SECTIONS = 0x06000000  # then 0x00400000 per module: data, and bss 0x00200000 above it

# The scratchpad's retail address, and the console's other view of it where
# the port maps it (src/pc/guest/image.h): an Android app has its Java heap
# at the retail one.
SCRATCHPAD_RETAIL, SCRATCHPAD_SIZE = 0x1F800000, 0x400

def host_address(address):
    """Where a pinned guest variable is natively: its retail address, or for
    a scratchpad variable the port's view of the scratchpad."""
    return address | 0x80000000 if SCRATCHPAD_RETAIL <= address < SCRATCHPAD_RETAIL + SCRATCHPAD_SIZE else address

HOST_LIBC = {"printf", "sprintf", "strcmp", "strcpy", "bzero", "qsort", "memcpy", "memset",
             "memmove", "strlen", "strcat", "strncmp", "strncpy", "memcmp"}

def c_name(symbol):
    """The C name of an object-file symbol, or None for toolchain symbols."""
    if X64:
        # No underscore; the toolchain's own are .refptr.*, __imp_* and such.
        return None if symbol.startswith((".", "__imp_", "$")) else symbol
    if not WINDOWS:
        return symbol
    return symbol[1:] if symbol.startswith("_") else None

def run(command):
    result = build_process.run(command)
    if result.returncode:
        sys.exit(f"{' '.join(command[:6])} ...\n{result.stderr}")
    return result.stdout

def flags_changed(path, flags):
    """When these flags were last different: path keeps them, rewritten
    only when they change, so its time is when they did (--release adds or
    drops one, in the same build directory)."""
    text = "\n".join(flags) + "\n"
    try:
        with open(path) as handle:
            same = handle.read() == text
    except OSError:
        same = False
    if not same:
        with open(path, "w") as handle:
            handle.write(text)
    return os.path.getmtime(path)


def link_android_loader(build, game, translated=False):
    """libmain.so, the loader SDL's Java shell runs (ANDROID_LOADER), once
    the game's load span is known to fit the range it reserves; for the
    translated game (linked at 0), one that opens it anywhere."""
    if translated:
        run([CC, *ANDROID_FLAGS, "-O2", "-Wall", "-Werror", "-Isrc", "-shared", "-o", f"{build}/libmain.so",
             "-Wl,--no-undefined", "-Wl,-z,noexecstack", "-DMEMORIES_ANDROID_TRANSLATED", ANDROID_LOADER,
             "-llog", "-ldl"])
        return
    low, high = None, 0
    for line in run([READELF, "-lW", game]).splitlines():
        parts = line.split()
        if parts and parts[0] == "LOAD":
            address, size = int(parts[2], 16), int(parts[5], 16)
            low = address if low is None else min(low, address)
            high = max(high, address + size)
    if low != ANDROID_GAME_BASE or high - low > ANDROID_GAME_SPAN:
        sys.exit(f"{game}: loads at 0x{low or 0:08X}-0x{high:08X}; the loader reserves "
                 f"0x{ANDROID_GAME_BASE:08X}-0x{ANDROID_GAME_BASE + ANDROID_GAME_SPAN:08X}")
    run([CC, *ANDROID_FLAGS, "-O2", "-Wall", "-Werror", "-Isrc", "-shared", "-o", f"{build}/libmain.so", "-Wl,--no-undefined",
         "-Wl,-z,noexecstack", f"-DMEMORIES_ANDROID_GAME_BASE=0x{ANDROID_GAME_BASE:08X}u",
         f"-DMEMORIES_ANDROID_GAME_SPAN=0x{ANDROID_GAME_SPAN:08X}u", ANDROID_LOADER, "-llog", "-ldl"])

def compile_unit(job):
    source, obj, flags, renames, newest = job
    if os.path.exists(obj) and os.path.getmtime(obj) >= newest and \
            os.path.getmtime(obj) >= os.path.getmtime(source):
        return
    if PTR32_PASS and source.endswith(".c"):
        # LLVM's AArch64 back end drops the truncation of a store through a
        # __ptr32 pointer (G32), so a u8/u16 store writes 4 bytes: the unit
        # goes through IR with those stores sent through 64-bit pointers
        # (tools/pc/ptr32_stores.py). One optimization pipeline still runs,
        # in the second step.
        ir = obj[:-2] + ".ll"
        run([CC, *flags, "-S", "-emit-llvm", "-Xclang", "-disable-llvm-passes", source, "-o", ir])
        with open(ir, encoding="utf-8") as handle:
            text = handle.read()
        try:
            text = ptr32_stores.rewrite(text)
        except ValueError as error:
            sys.exit(f"{source}: {error}")
        with open(ir, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        run([CC, *flags, "-Wno-unused-command-line-argument", "-c", ir, "-o", obj])
    else:
        run([CC, *flags, "-c", source, "-o", obj])
    if os.path.exists(obj + ".aliased"):
        os.remove(obj + ".aliased")  # the fresh object names them itself (see main)
    if WINDOWS and not X64:
        # asm("name") labels in the sources name C symbols, which COFF spells
        # with a leading underscore; everything else from C already has one.
        labels = {line.split()[-1] for line in run([NM, "-g", obj]).splitlines()
                  if line.split() and line.split()[-1][:1].isalpha()}
        if labels:
            with open(obj + ".labels", "w") as handle:
                handle.writelines(f"{name} _{name}\n" for name in sorted(labels))
            run([OBJCOPY, f"--redefine-syms={obj}.labels", obj])
    if renames:
        run([OBJCOPY, f"--redefine-syms={renames}", obj])

def symbols(objects):
    defined, tentative, undefined = set(), set(), set()
    for line in run([NM, "-g", *objects]).splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] in "UCTDBRVW" and not line.endswith(":") and c_name(parts[-1]):
            {"U": undefined, "C": tentative}.get(parts[-2], defined).add(c_name(parts[-1]))
    return defined, tentative, undefined

def rename_coff_sections(path, renames):
    """objcopy --rename-section for COFF objects, which llvm-objcopy lacks.
    The contents get a $m suffix on Windows: lld sorts a section's $-suffixed
    parts by suffix, which puts them between the $a and $z markers that stand
    in for ELF's __start_ and __stop_ symbols."""
    with open(path, "rb") as handle:
        data = bytearray(handle.read())
    _, count, _, symbols_at, symbol_count, optional_size, _ = struct.unpack_from("<HHIIIHH", data, 0)
    strings_at = symbols_at + symbol_count * 18
    strings_size = struct.unpack_from("<I", data, strings_at)[0]
    if strings_at + strings_size != len(data):
        sys.exit(f"{path}: the string table does not end the file")
    extra = bytearray()
    for index in range(count):
        at = 20 + optional_size + index * 40
        field = bytes(data[at:at + 8]).rstrip(b"\0")
        if field.startswith(b"/"):
            start = strings_at + int(field[1:])
            field = bytes(data[start:data.index(b"\0", start)])
        new = renames.get(field.decode())
        if new is None:
            continue
        encoded = new.encode()
        if len(encoded) > 8:
            reference = f"/{strings_size + len(extra)}".encode()
            extra += encoded + b"\0"
            encoded = reference
        data[at:at + 8] = encoded.ljust(8, b"\0")
    if extra:
        data += extra
        struct.pack_into("<I", data, strings_at, strings_size + len(extra))
    with open(path, "wb") as handle:
        handle.write(data)

def unset_coff_commons(path, names):
    """Turn COMMON symbols (tentative definitions) named in `names` into
    undefined references: lld prefers a COMMON over an absolute definition,
    where a GNU linker script assignment overrides it. A COFF COMMON symbol
    is an external one in no section whose value is its size."""
    with open(path, "rb") as handle:
        data = bytearray(handle.read())
    _, _, _, symbols_at, symbol_count, _, _ = struct.unpack_from("<HHIIIHH", data, 0)
    strings_at = symbols_at + symbol_count * 18
    changed, index = False, 0
    while index < symbol_count:
        at = symbols_at + index * 18
        value, section, _, storage, auxiliary = struct.unpack_from("<IhHBB", data, at + 8)
        if section == 0 and value and storage == 2:  # IMAGE_SYM_CLASS_EXTERNAL
            raw = bytes(data[at:at + 8])
            if raw[:4] == b"\0\0\0\0":
                start = strings_at + struct.unpack_from("<I", raw, 4)[0]
                raw = bytes(data[start:data.index(b"\0", start)])
            if c_name(raw.rstrip(b"\0").decode()) in names:
                struct.pack_into("<I", data, at + 8, 0)
                changed = True
        index += 1 + auxiliary
    if changed:
        with open(path, "wb") as handle:
            handle.write(data)

def definitions(objects):
    """How many of the objects define each name."""
    counts, current = {}, None
    for line in run([NM, "-g", "--defined-only", *objects]).splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] in "TDBRV" and c_name(parts[-1]):
            counts[c_name(parts[-1])] = counts.get(c_name(parts[-1]), 0) + 1
    return counts

def direct_branches(objects, names):
    """The names in `names` the objects call or jump to by name (a
    pc-relative relocation, the only kind -fno-pie code has for a branch). One
    that is also used as a value ends the build."""
    relative = ({"IMAGE_REL_I386_REL32"} if WINDOWS else {"R_AARCH64_CALL26", "R_AARCH64_JUMP26"} if A64
                else {"R_386_PC32", "R_386_PLT32"})
    called, used = set(), set()
    if X64:
        # x86-64 reaches data pc-relative too, so a relocation is a branch
        # only under a call or jmp: read each from the disassembly, where it
        # follows its instruction.
        instruction = ""
        for line in run([OBJDUMP, "-dr", "--no-show-raw-insn", *objects]).splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1].startswith("IMAGE_REL_AMD64_"):
                if c_name(parts[2]) in names:
                    branch = parts[1] == "IMAGE_REL_AMD64_REL32" and instruction in ("callq", "call", "jmp", "jmpq")
                    (called if branch else used).add(c_name(parts[2]))
            elif len(parts) >= 2 and parts[0].endswith(":"):
                instruction = parts[1]
    for line in run([OBJDUMP, "-r", *objects]).splitlines() if not X64 else []:
        parts = line.split()
        if len(parts) == 3 and c_name(parts[2]) in names:
            (called if parts[1] in relative else used).add(c_name(parts[2]))
    both = sorted(called & used)
    if both:
        # One symbol cannot be the guest address (as a value) and a host
        # stub (as a call); left pinned, the calls would need DEP again.
        sys.exit("called by name and also used as an address, so no host stub can take the calls without "
                 "changing the address: " + ", ".join(both) + " (call it through a pointer, or give the address "
                 "a name of its own)")
    return called

# The thunks src/pc/guest/branch_thunks.c defines for AArch64 (x0-x29 but
# x18, the platform register). clang emits weak copies of its own in every
# unit (and of names it never calls), so a call to one the port does not
# define would link silently and skip the resolver.
ARM_THUNKS = {f"__llvm_slsblr_thunk_x{n}" for n in range(30) if n != 18}
ARM_BRANCH_EXEMPT = ("src_pc_guest_branch_thunks.c.o",)   # the thunks themselves


def check_arm_branches(objects):
    """AArch64: every indirect call of the compiled code goes through a thunk
    the port defines, and no other indirect branch is left (the x86
    compilers' flags guarantee that; -mharden-sls=blr covers calls only, and
    -fno-optimize-sibling-calls and -fno-jump-tables the rest). `ret` is not
    an indirect branch to worry about."""
    called = set()
    for line in run([OBJDUMP, "-r", *objects]).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1].startswith(("R_AARCH64_CALL26", "R_AARCH64_JUMP26")) and \
                "__llvm_slsblr_thunk_" in parts[2]:
            called.add(parts[2])
    unknown = sorted(called - ARM_THUNKS)
    if unknown:
        sys.exit("calls to branch thunks src/pc/guest/branch_thunks.c does not define (a renamed copy?): " +
                 ", ".join(unknown))
    # blr/br and their pointer-authenticated forms.
    branch = re.compile(r"\t(blr|br|blraa|blrab|braa|brab)\w*\t")
    found, checked = [], [path for path in objects if os.path.basename(path) not in ARM_BRANCH_EXEMPT]
    for start in range(0, len(checked), 64):
        unit = where = None
        for line in run([OBJDUMP, "-d", "--no-show-raw-insn", *checked[start:start + 64]]).splitlines():
            if "file format" in line:
                unit = os.path.basename(line.split(":", 1)[0] if not line[1:3] == ":/" else line.rsplit(":", 1)[0])
            elif line.endswith(">:"):
                where = line.split("<", 1)[-1][:-2]
            elif branch.search(line) and not where.startswith("__llvm_slsblr_thunk_"):   # clang's weak copies
                found.append(f"{unit}: {where}: {line.strip()}")
    if found:
        sys.exit("indirect branches that bypass the branch thunks:\n  " + "\n  ".join(found[:20]))


def write_guest_branches(build, branches):
    """guest_branches.o: a host entry for each pinned module function that
    code calls by name (see main), which hands its guest address to the
    branch thunks' resolver (src/pc/guest/branch_thunks.c)."""
    with open(f"{build}/guest_branches.c", "w") as handle:
        handle.write("/* Written by tools/pc/build_game32.py. */\nextern void Memories_GuestBranchDirect(void);\n")
        if A64:
            # The address in X16 (IP0), which a call may change (AAPCS64).
            handle.writelines(f'__asm__(".text\\n.p2align 2\\n.globl {name}\\n.type {name}, %function\\n{name}:\\n'
                              f'    movz x16, #0x{address & 0xFFFF:04X}\\n    movk x16, #0x{address >> 16:04X}, lsl #16\\n'
                              f'    b Memories_GuestBranchDirect\\n");\n' for name, address in sorted(branches.items()))
        else:
            # x86-64 has no push of a 32-bit unsigned immediate: the slot is
            # made and its low half written (the resolver reads only that).
            push = "subq $8, %rsp\\n    movl $0x{0:08X}, (%rsp)" if X64 else "pushl $0x{0:08X}"
            handle.writelines(f'__asm__(".text\\n.globl {PREFIX}{name}\\n{PREFIX}{name}:\\n    ' + push.format(address) +
                              f'\\n    jmp {PREFIX}Memories_GuestBranchDirect\\n");\n' for name, address in sorted(branches.items()))
    run([CC, *NATIVE_CFLAGS, "-c", f"{build}/guest_branches.c", "-o", f"{build}/guest_branches.o"])
    return f"{build}/guest_branches.o"

MOD_INTERNALS = ("Mods_", "Json_", "ObjectLoader_")  # the mod system itself (src/pc/mods)
# Guest variables a release exported that the port's own code no longer
# uses: still pinned, so a mod built against that release still loads
# (tools/pc/check_mod_abi.py). D_801A8008: the disc's password table from
# card 1 (overlays/password/shop.h), which the PC shop no longer walks.
KEPT_FOR_MODS = {"D_801A8008"}

def write_mod_exports(build, names, aliases):
    """mod_exports.c: every name a code mod may bind to (src/pc/mods/exports.h).

    `names` are the globals of the game units and the port, the pinned guest
    variables and the stubs; `aliases` map an address-based name to the
    definition it stands for. The linker fills in each address, which is
    what makes the table the same on both systems: no -rdynamic, no export
    table, and the pinned variables are there too (the runtime symbol table
    has none of them). The host's C library and the mod system's own entry
    points are left out, as are the linker's (__start_...); a mod gets C
    library functions from mod_libc.c."""
    identifier = lambda name: (name[:1].isalpha() or name[:1] == "_") and all(c.isalnum() or c == "_" for c in name)
    table = {name: name for name in names
             if identifier(name) and name not in HOST_LIBC and name != "main"
             and not name.startswith(MOD_INTERNALS + ("__",))}
    table.update((name, target) for name, target in aliases.items() if target in table)
    with open(f"{build}/mod_exports.c", "w") as handle:
        handle.write('#include "pc/mods/exports.h"\n')
        handle.writelines(f"extern char {name}[];\n" for name in sorted(set(table.values())))
        handle.write("const MemoriesModExport Memories_ModExports[] = {\n")
        handle.writelines(f'    {{"{name}", {table[name]}}},\n' for name in sorted(table))
        handle.write(f"}};\nconst unsigned Memories_ModExportCount = {len(table)};\n")
    # -fno-builtin: every declaration above is a char array, including the
    # ones that share a name with something the compiler knows.
    # Without Android's compat header, whose functions are char arrays here.
    flags = list(NATIVE_CFLAGS)
    if ANDROID_COMPAT in flags:
        del flags[flags.index(ANDROID_COMPAT) - 1:flags.index(ANDROID_COMPAT) + 1]   # and its -include
    run([CC, *flags, "-fno-builtin", "-w", "-c", f"{build}/mod_exports.c", "-o", f"{build}/mod_exports.o"])

def guest_addresses():
    """The retail address of every global in the resident image and in each
    module, and the resident .text range: from the matching build's ELFs
    when they are here, which also refreshes ADDRESSES, else from ADDRESSES.
    Returns ({name: address}, {module: {name: address}}, (start, end))."""
    elfs = {"SLUS_014.11": ELF}
    elfs.update((name, "tmp/overlays/{0}/build/{0}.elf".format(MODULE_CONFIG.get(name, name))) for name, _, _, _ in MODULES)
    if all(os.path.exists(path) for path in elfs.values()):
        tables = {}
        for name, path in elfs.items():
            found = {}
            for line in run([READELF, "-sW", path]).splitlines():
                parts = line.split()
                if len(parts) == 8 and parts[4] == "GLOBAL" and parts[6] != "UND":
                    found.setdefault(parts[7], int(parts[1], 16))
            tables[name] = found
        text = (0, 0)
        for line in run([READELF, "-SW", ELF]).splitlines():
            parts = line.replace("[", " ").replace("]", " ").split()
            if len(parts) > 5 and parts[1] == ".text":
                text = (int(parts[3], 16), int(parts[3], 16) + int(parts[5], 16))
        lines = ["# Retail addresses of the game's globals, for tools/pc/build_game32.py. Written by it\n",
                 "# from the matching build's ELFs when they are present; commit it when it changes.\n",
                 f"text {text[0]:08X} {text[1]:08X}\n"]
        for name, found in tables.items():
            lines.append(f"[{name}]\n")
            lines.extend(f"{symbol} {address:08X}\n" for symbol, address in sorted(found.items()))
        current = open(ADDRESSES).read() if os.path.exists(ADDRESSES) else None
        if current != "".join(lines):
            with open(ADDRESSES, "w", newline="\n") as handle:
                handle.writelines(lines)
            print(f"{ADDRESSES}: updated from the matching build; commit it")
    elif not os.path.exists(ADDRESSES):
        sys.exit(f"{ADDRESSES} is missing, and so are the matching build's ELFs (make match match-overlays)")
    tables, text, section = {}, (0, 0), None
    with open(ADDRESSES) as handle:
        for line in handle:
            parts = line.split()
            if not parts or parts[0].startswith("#"):
                continue
            if parts[0] == "text":
                text = (int(parts[1], 16), int(parts[2], 16))
            elif parts[0].startswith("["):
                section = tables.setdefault(parts[0][1:-1], {})
            else:
                section[parts[0]] = int(parts[1], 16)
    missing = [name for name in elfs if name not in tables]
    if missing:
        sys.exit(f"{ADDRESSES} has no section for {', '.join(missing)}")
    return tables["SLUS_014.11"], {name: tables[name] for name, _, _, _ in MODULES}, text


def exe_icon(build, windres):
    """The executable's icon on Windows: the game's memory card icon (the
    save header template, src/pc/platform/save_icon.h), made here from the
    game's own game/SLUS_014.11 and never kept in the repository. The .ico's
    path, or None without the game, the template, or a resource compiler."""
    try:
        with open("config/pc/guest_addresses.txt") as table:
            address = next(int(line.split()[1], 16) for line in table
                           if line.split()[:1] == ["gSaveData_aHeaderTemplate"])
        with open("game/SLUS_014.11", "rb") as executable:
            image = executable.read()
    except (OSError, StopIteration, ValueError, IndexError):
        return None
    # A PS-X EXE: its text loads at t_addr (+0x18) from file offset 0x800.
    at = address - struct.unpack_from("<I", image, 0x18)[0] + 0x800
    header = image[at:at + 0x200] if 0x800 <= at <= len(image) - 0x200 else b""
    if not windres or header[:2] != b"SC" or not 0x11 <= header[2] <= 0x13:
        return None
    clut = [struct.unpack_from("<H", header, 0x60 + 2 * i)[0] for i in range(16)]

    def bgra(x, y):  # frame 0, 16x16 at 4 bits, the low nibble first
        byte = header[0x80 + (y * 16 + x) // 2]
        color = clut[byte >> 4 if x & 1 else byte & 15]
        return ((color >> 10 & 31) * 255 // 31, (color >> 5 & 31) * 255 // 31, (color & 31) * 255 // 31,
                255 if color else 0)

    entries = []
    for size in (16, 32, 48, 64):  # pixel-doubled, bottom-up 32-bit DIBs with an empty AND mask
        k = size // 16
        rows = bytes(v for y in range(size - 1, -1, -1) for x in range(size) for v in bgra(x // k, y // k))
        mask = bytes((size + 31) // 32 * 4 * size)
        entries.append(struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(rows) + len(mask), 0, 0, 0, 0)
                       + rows + mask)
    icon = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    for size, dib in zip((16, 32, 48, 64), entries):
        icon += struct.pack("<BBBBHHII", size, size, 0, 0, 1, 32, len(dib), offset)
        offset += len(dib)
    ico = os.path.abspath(f"{build}/icon.ico").replace("\\", "/")
    with open(ico, "wb") as out:
        out.write(icon + b"".join(entries))
    return ico


def exe_resources(build, release):
    """The Windows executable's resources: version information (product,
    description, version), which virus scanners' heuristics hold against a
    program that has none, and outside releases the icon above. A release
    fails without a resource compiler; other builds go without."""
    windres = shutil.which(CC.replace("clang", "windres"))
    if not windres:
        if release:
            sys.exit(f"{CC.replace('clang', 'windres')} not found: a release carries version information")
        return []
    ico = None if release else exe_icon(build, windres)
    label = release_version()
    numbers = [int(n) for n in re.match(r"v(\d+)\.(\d+)\.(\d+)", label).groups()] if label else [0, 0, 0]
    number = ",".join(str(n) for n in numbers + [0])
    text = label or "development build"
    strings = {"CompanyName": "Yu-Gi-Oh! Forbidden Memories Recompiled",
               "FileDescription": "Yu-Gi-Oh! Forbidden Memories Recompiled",
               "FileVersion": text, "InternalName": "memories-pc", "OriginalFilename": "memories-pc.exe",
               "ProductName": "Yu-Gi-Oh! Forbidden Memories Recompiled", "ProductVersion": text,
               "Comments": "https://github.com/Unchiga/Yu-Gi-Oh-Forbidden-Memories-Recompiled"}
    rc = [f'1 ICON "{ico}"'] if ico else []
    rc += ["1 VERSIONINFO", f"FILEVERSION {number}", f"PRODUCTVERSION {number}", "FILEOS 0x40004", "FILETYPE 0x1",
           "BEGIN", '  BLOCK "StringFileInfo"', "  BEGIN", '    BLOCK "040904B0"', "    BEGIN",
           *[f'      VALUE "{key}", "{value}"' for key, value in strings.items()],
           "    END", "  END", '  BLOCK "VarFileInfo"', "  BEGIN", '    VALUE "Translation", 0x409, 1200', "  END",
           "END"]
    with open(f"{build}/resources.rc", "w") as handle:
        handle.write("\n".join(rc) + "\n")
    if subprocess.run([windres, f"{build}/resources.rc", "-O", "coff", "-o", f"{build}/resources.o"]).returncode:
        if release:
            sys.exit(f"{build}/resources.rc: the resource compiler failed")
        return []
    return [f"{build}/resources.o"]


def library(source_dir):
    """The manifest's "library" (or "libraries"): the mod has code."""
    with open(f"{source_dir}/mod.json", encoding="utf-8") as handle:
        manifest = json.load(handle)
    return bool(manifest.get("library") or manifest.get("libraries"))


def build_mods(build, release=False, code=True, target="i386"):
    """Each directory under mods/ becomes a mod directory beside the game.

    A mod is its manifest and whatever it ships; if it has C, that becomes
    one object file for this game's target (tools/pc/build_mod.py), which
    the game's own loader links in when the mod is applied
    (src/pc/mods/object_loader.c). One i386 object serves both 32-bit
    systems, so the Linux and the Windows game can carry the same file; the
    64-bit Windows game carries the x86_64-windows one. build_mod.py keeps
    it in tmp/pc/mod-build/<mod>-<key>, the key a digest of the compiler,
    the flags and the preprocessed sources: every checkout shares tmp (the
    worktrees link it), and one reuses an object only when it would build
    the same one. It is copied
    beside this game when the copy there differs, and checked against this
    game's exports either way.

    The SDK goes beside the game too, so a release carries what a mod author
    builds against: modapi.h and the game's headers under sdk/include, the C
    library a mod may use under sdk/include/libc, build_mod.py and the texture
    pack tools under sdk/tools, the example mods under sdk/examples/mods and
    the modding notes under sdk/notes.

    With code=False (a build that links no code mods) only the data mods go
    beside it: shipping a code mod would put one refusal per code mod in
    every player's Mods window. No SDK goes beside it either."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import build_mod
    out_root = f"{build}/mods"
    tracked = None
    if release:
        tracked = set(subprocess.check_output(["git", "ls-files", "-z", "mods"], text=True).split("\0"))
        shutil.rmtree(out_root, ignore_errors=True)
    os.makedirs(out_root, exist_ok=True)
    if code:
        write_sdk(build, target)
    mods, skipped = [], []
    for manifest in sorted(glob.glob("mods/*/mod.json")):
        if tracked is not None and manifest not in tracked:
            continue
        source_dir = os.path.dirname(manifest)
        name = os.path.basename(source_dir)
        out_dir = f"{out_root}/{name}"
        if not code and library(source_dir):
            shutil.rmtree(out_dir, ignore_errors=True)   # from a build that copied it
            skipped.append(name)
            continue
        os.makedirs(out_dir, exist_ok=True)
        for path in sorted(glob.glob(f"{source_dir}/**/*", recursive=True)):
            if tracked is not None and path not in tracked:
                continue
            if path.endswith(".c") or path.endswith(".h") or os.path.isdir(path):
                continue
            copy_if_changed(path, os.path.join(out_dir, os.path.relpath(path, source_dir)))
        mods.append((name, source_dir, out_dir))
    # All at once: a mod whose key is not remembered starts the preprocessor,
    # and a new key the compiler. Checked against this build's own export
    # table: the other system's may be older than this build. Written where
    # the manifest's "library" puts it, which may be a subdirectory.
    if not code:
        print(f"{out_root}: " + ", ".join(f"{name} (data)" for name, _, _ in mods) +
              (f"; left out, code: {', '.join(skipped)}" if skipped else ""))
        return
    with concurrent.futures.ThreadPoolExecutor(max(1, len(mods))) as pool:
        objects = list(pool.map(lambda mod: build_mod.build(mod[1], out_dir=mod[2], games=[build], quiet=True,
                                                            target=target), mods))
    built = []
    for (name, source_dir, out_dir), obj in zip(mods, objects):
        if not obj:
            built.append(f"{name} (data)")
            continue
        for stale in glob.glob(f"{out_dir}/*.so") + glob.glob(f"{out_dir}/*.dll"):
            os.remove(stale)   # native libraries from before mods were objects
        for each in build_mod.TARGETS:   # another target's object, from a build folder used for it before
            other = os.path.join(out_dir, build_mod.library_name(source_dir, each))
            if each != target and os.path.abspath(other) != os.path.abspath(obj) and os.path.exists(other):
                os.remove(other)
        built.append(name)
    if built:
        print(f"{out_root}: " + ", ".join(built))


def copy_languages(build, release=False):
    """languages/*.txt, the official European languages' text (Game >
    Language, src/pc/text/language.h), into <build>/languages beside the
    game. A release takes only the packs git tracks."""
    out_root = f"{build}/languages"
    packs = sorted(glob.glob("languages/*.txt"))
    if release:
        tracked = set(subprocess.check_output(["git", "ls-files", "-z", "languages"], text=True).split("\0"))
        packs = [path for path in packs if path.replace(os.sep, "/") in tracked]
        shutil.rmtree(out_root, ignore_errors=True)
    for path in packs:
        copy_if_changed(path, os.path.join(out_root, os.path.basename(path)))
    if packs:
        print(f"{out_root}: " + ", ".join(os.path.splitext(os.path.basename(path))[0] for path in packs))


def copy_if_changed(source, destination):
    """Copy when the destination is missing or its bytes differ. Not by
    date: a build folder in tmp is shared by every worktree (a junction),
    and a newer file there may be another checkout's."""
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    if not os.path.exists(destination) or not filecmp.cmp(source, destination, shallow=False):
        shutil.copy2(source, destination)


TRANSLATION_TABLES = ("mod_translation.json", "mod_signatures.json")

def write_sdk(build, target="i386"):
    """<build>/sdk: what a mod is built against, beside the game."""
    sdk = f"{build}/sdk"
    for header in glob.glob("src/**/*.h", recursive=True):
        relative = os.path.relpath(header, "src")
        if relative.startswith(os.path.join("pc", "compat", "android")):
            continue   # the Android build's own stand-ins for system headers, not the game's
        if relative.startswith(os.path.join("pc", "mods", "sdk")):
            relative = os.path.join("libc", os.path.relpath(header, "src/pc/mods/sdk"))
        copy_if_changed(header, os.path.join(sdk, "include", relative))
    # ptr32_stores.py: the IR pass build_mod.py runs on aarch64 units.
    for name in ("build_mod.py", "build_process.py", "ptr32_stores.py"):
        copy_if_changed(f"tools/pc/{name}", f"{sdk}/tools/{name}")
    # What else a mod author needs beside the headers: the texture pack tools
    # (the standard library only; upscale_pack.py also wants Pillow and
    # Upscayl, which it asks for), the example mods, and the notes that
    # describe all of it.
    for name in ("extract_images.py", "upscale_pack.py"):
        copy_if_changed(f"tools/pc/{name}", f"{sdk}/tools/{name}")
    for path in glob.glob("examples/mods/**/*", recursive=True):
        if os.path.isfile(path):
            copy_if_changed(path, os.path.join(sdk, os.path.relpath(path)))
    for name in ("modding.md", "mod-api-3.md", "more-cards.md"):
        copy_if_changed(f"notes/{name}", f"{sdk}/notes/{name}")
    # What this game lends a mod, for build_mod.py's check beside the game:
    # exports.<target>.txt, and for a 32-bit game also exports.txt, the name
    # SDKs had before the 64-bit targets (check_mod_abi.py reads it).
    # Only this build's own: tools/pc/package.py adds the other targets'
    # lists to a release's SDK from their own builds of the same tree.
    import build_mod
    names = "".join(name + "\n" for name in sorted(build_mod.provided(build, target)))
    own = [f"exports.{target}.txt"] + (["exports.txt"] if target == "i386" else [])
    for listed in own:
        with open(f"{sdk}/{listed}", "w") as handle:
            handle.write(names)
    # The translated game's tables, which build_mod.py puts a mod through
    # the guest pass with (aarch64-translated: build_arm64.py writes them),
    # and the desktop SDKs carry too (tools/pc/package.py).
    for name in TRANSLATION_TABLES:
        if target == "aarch64-translated":
            copy_if_changed(f"{build}/{name}", f"{sdk}/{name}")
        elif os.path.exists(f"{sdk}/{name}"):
            os.remove(f"{sdk}/{name}")   # from a build folder used for the Android game before
    for path in glob.glob(f"{sdk}/exports*.txt"):
        if os.path.basename(path) not in own:
            os.remove(path)   # another target's, in a folder reused for this one
    stale = f"{build}/include"   # the lone modapi.h copy from before the SDK
    if os.path.isdir(stale):
        shutil.rmtree(stale)

def write_build_identity(build, output, keep, older=None):
    """<build>/symbols/<id>.txt, buildid, commit and source for `output`;
    the build id is returned.

    Save states are carried between builds with these tables
    (src/pc/guest/state.c): every function in the executable, because the
    game keeps pointers to native routines as well as its own (HMD
    primitive drivers, callbacks), and the game objects' variables: the
    symbols keep(kind, address) takes. "address size name"; repeats of a
    static name get #2, #3... in address order, which follows the link
    order. The table's hash is the build id; `older`, a second name for the
    same table."""
    os.makedirs(f"{build}/symbols", exist_ok=True)
    seen, table = {}, []
    for line in run([NM, "-n", "-S", output]).splitlines():
        parts = line.split()
        if len(parts) != 4 or not c_name(parts[3]):
            continue
        parts[3] = c_name(parts[3])
        if keep(parts[2], int(parts[0], 16)):
            seen[parts[3]] = seen.get(parts[3], 0) + 1
            name = parts[3] if seen[parts[3]] == 1 else f"{parts[3]}#{seen[parts[3]]}"
            table.append(f"{parts[0]} {parts[1]} {name}\n")
    if WINDOWS:
        # PE symbols carry no sizes: a function runs to the next symbol, so
        # crash and hang reports can name the routine an address is in.
        rows = [row.split() for row in table]
        for index, row in enumerate(rows):
            if int(row[1], 16) == 0 and index + 1 < len(rows):
                row[1] = f"{int(rows[index + 1][0], 16) - int(row[0], 16):08x}"
        table = [" ".join(row) + "\n" for row in rows]
    if not table:
        sys.exit(f"{output}: no symbols to carry save states between builds with (the link dropped its symbol table)")
    build_id = hashlib.sha256("".join(table).encode()).hexdigest()[:8]
    for name in (build_id, older) if older else (build_id,):
        with open(f"{build}/symbols/{name}.txt", "w") as handle:
            handle.writelines(table)
    with open(f"{build}/buildid", "w") as handle:
        handle.write(build_id + "\n")
    # The commit, for crash reports (src/pc/debug/monitor.c): "unknown" in
    # a tree that is not a git checkout.
    try:
        commit = subprocess.run(["git", "describe", "--always", "--dirty", "--abbrev=10"], capture_output=True,
                                text=True, check=True).stdout.strip() or "unknown"
    except (OSError, subprocess.CalledProcessError):
        commit = "unknown"
    with open(f"{build}/commit", "w") as handle:
        handle.write(commit + "\n")
    with open(f"{build}/source", "w") as handle:
        handle.write(source_stamp() + "\n")
    return build_id

def package_android_build(build, game, build_id):
    """The APK: libmain.so and `game` (libgame.so), with this build's id,
    commit and symbol table (save states, crash reports) and what the
    desktop games have beside them: the shipped mods (their objects checked
    against this build's own export table) and the language packs, all
    under assets/build/, which android.c unpacks into the program directory."""
    import package_android
    assets = {"build/buildid": f"{build}/buildid", "build/commit": f"{build}/commit",
              f"build/symbols/{build_id}.txt": f"{build}/symbols/{build_id}.txt"}
    assets.update(package_android.program_files(build, ("mods", "languages")))
    if ANDROID_ABI in ANDROID_PACKAGED:
        package_android.package(build, ANDROID_ABI, f"{build}/libmain.so", game, assets)
    else:
        print(f"{build}: android-{ANDROID_ABI} builds for development and is not packaged; the app is "
              f"{', '.join('android-' + abi for abi in ANDROID_PACKAGED)}")

def build_android_translated(options):
    """--target android-arm64-v8a (the app): the translated game.
    build_arm64.py --target android-arm64-translated compiles the game
    through the guest pass (tools/pc/llvm_guest.py) and links libgame.so at
    0 (notes/pc-build.md, "Android arm64"); here the loader, the data mods,
    the language packs, the identity files and the APK, as for the fixed
    game, and the code mods through the same guest pass (build_mod.py
    --target aarch64-translated) with the SDK that builds them."""
    command = [sys.executable, os.path.join(ROOT, "tools/pc/build_arm64.py"), "--target", "android-arm64-translated",
               "--build", options.build, *(["--release"] if options.release else [])]
    print("Android arm64: the translated game (" + " ".join(command[1:]) + ")", flush=True)
    if subprocess.run(command, cwd=ROOT).returncode:
        sys.exit("build_arm64.py --target android-arm64-translated failed (its logs are in "
                 f"{options.build}/logs)")
    output = f"{options.build}/libgame.so"
    link_android_loader(options.build, output, translated=True)
    build_mods(options.build, options.release, target="aarch64-translated")
    copy_languages(options.build, options.release)
    build_id = write_build_identity(options.build, output, lambda kind, address: kind in "TtDdBbRr")
    # The table the app ships is version 2: offsets into libgame.so (linked
    # at 0) with content hashes and unit-qualified statics, which crash
    # reports and save states of another build go by
    # (tools/pc/translated_symbols.py, translated_state_symbols.c).
    import translated_symbols
    with open(f"{options.build}/symbols/{build_id}.txt", "w", newline="\n") as handle:
        handle.write(translated_symbols.table(output))
    package_android_build(options.build, output, build_id)

def source_stamp():
    """The tree a build was made from, written to <build>/source: the
    commit, and a digest of the changes to tracked files when there are
    any. tools/pc/package.py packs another target's export list into a
    release's SDK only from a build with this one's stamp. "unknown" outside
    a git checkout, which matches nothing."""
    try:
        head = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
        changes = subprocess.run(["git", "diff", "HEAD", "--binary"], capture_output=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return "unknown"
    return head + (f" changes {hashlib.sha256(changes).hexdigest()[:16]}" if changes else "") if head else "unknown"

VERSION_PATTERN = r"v\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?"

def release_version():
    """The release this build is, for the update check (notes/updates.md):
    MEMORIES_VERSION when it is set (tools/pc/package.py sets it from
    --version, the tag in CI), else the v* tag the checkout is exactly at.
    Anything that is not vX.Y.Z or vX.Y.Z-PRE is a development build: ""."""
    import re
    version = os.environ.get("MEMORIES_VERSION")
    if version is None:
        try:
            version = subprocess.run(["git", "describe", "--tags", "--exact-match", "--match", "v[0-9]*"],
                                     capture_output=True, text=True).stdout.strip()
        except OSError:
            version = ""
    return version if re.fullmatch(VERSION_PATTERN, version) else ""

def write_version(build, force=False):
    """version.c: Memories_Version, rewritten only when it changes."""
    text = f'const char Memories_Version[] = "{release_version()}";\n'
    path = f"{build}/version.c"
    if not os.path.exists(path) or open(path).read() != text:
        with open(path, "w") as handle:
            handle.write(text)
    stale = not os.path.exists(f"{build}/version.o") or os.path.getmtime(f"{build}/version.o") < os.path.getmtime(path)
    if force or stale:
        run([CC, *NATIVE_CFLAGS, "-c", path, "-o", f"{build}/version.o"])
    return f"{build}/version.o"

X64_GATE = "tools/pc/x64_compiler_gate.c"

def check_x64_compiler(build):
    """Refuse a clang that miscompiles G32: clang 12 indexes an array of
    __ptr32 function pointers with an 8-byte stride although sizeof says 4
    (x64 gate 1, 2026-09-29). The gate is built with the game's flags at
    -O0 and -O2 and run, once per compiler; the version it passed with is
    kept in the build directory."""
    version = run([CC, "--version"]).splitlines()[0]
    stamp = f"{build}/compiler-gate.txt"
    if os.path.exists(stamp) and open(stamp).read() == version + "\n" and \
            os.path.getmtime(stamp) >= os.path.getmtime(X64_GATE):
        return
    os.makedirs(build, exist_ok=True)
    exe = f"{build}/x64_compiler_gate.exe"
    for level in ("-O0", "-O2"):
        result = build_process.run([CC, *X64_FLAGS, "-DMEMORIES_PC", "-Isrc", level, "-Wl,--image-base=0x40000000",
                                    "-Wl,--disable-dynamicbase", "-Wl,--disable-high-entropy-va", X64_GATE, "-o", exe])
        if result.returncode:
            sys.exit(f"{CC} ({version}) cannot build the 64-bit game:\n{result.stderr}")
        command = [os.path.abspath(exe)]
        if sys.platform != "win32":
            # Built on Linux: Wine runs it (binfmt may not be set up for .exe).
            if not shutil.which("wine"):
                print(f"build: wine is missing, so {CC} ({version}) is not checked for the clang 12 "
                      "G32 miscompile; use clang 21 or later", file=sys.stderr)
                return
            command.insert(0, "wine")
        checked = subprocess.run(command, capture_output=True, text=True,
                                 env=dict(os.environ, WINEDEBUG="-all", WINEDLLOVERRIDES="mscoree,mshtml="))
        if checked.returncode:
            sys.exit(f"{CC} ({version}) miscompiles 32-bit guest pointers at {level}; the 64-bit build needs "
                     f"clang 21 or later:\n{checked.stdout}{checked.stderr}")
    with open(stamp, "w") as handle:
        handle.write(version + "\n")

def main():
    global NEWEST_HEADER, PTR32_PASS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=list(BACKENDS), default=os.environ.get("MEMORIES_BACKEND") or
                        "sdl")
    parser.add_argument("--target", default=TARGET,
                        help="linux, windows, windows-x64, or android-<abi>: android-arm64-v8a (64-bit "
                             "ARM phones) or android-x86 (development, not packaged; notes/pc-build.md, \"Android\")")
    parser.add_argument("--release", action="store_true",
                        help="Windows GUI executable; omit the optional disc-derived executable icon")
    parser.add_argument("--android-memory", choices=("translated", "fixed"),
                        default=os.environ.get("MEMORIES_ANDROID_MEMORY") or "translated",
                        help="android-arm64-v8a: the translated game (the app's: no fixed host address, "
                             "build_arm64.py) or the fixed one (guest memory at its PlayStation addresses, "
                             "for comparison and code mods; notes/pc-build.md, \"Android arm64\")")
    # A Windows build made on Linux gets a directory of its own, so both
    # executables and their objects sit side by side.
    parser.add_argument("--build", default="tmp/pc/win64" if X64 else
                        "tmp/pc/win32" if WINDOWS and sys.platform != "win32" else
                        f"tmp/pc/android-{ANDROID_ABI}" if ANDROID else "tmp/pc/game32")
    options = parser.parse_args()
    # Forward slashes, as build.py on Windows hands over str(Path) with
    # backslashes: the object paths below are this folder plus "/obj/...",
    # and the Android build's `nm -A` check matches llvm-nm's listing against
    # them, where a backslash comes back doubled ("tmp\\pc\\x/obj/...").
    if os.sep == "\\":
        options.build = options.build.replace("\\", "/")
    # Android's GLES has none of desktop GL's fixed function, which the
    # present pass and sdl.c's GL presenter call: that path is never taken
    # there (Platform_HasDesktopGL), and gl_desktop_none.c stands in for it.
    NATIVE.extend(ANDROID_BACKEND.get(source, source) if ANDROID else source for source in BACKENDS[options.backend])
    NATIVE.sort()
    if ANDROID:
        if options.backend != "sdl":
            sys.exit("Android builds use the SDL backend")
        os.chdir(ROOT)
        build_android_deps.main(ANDROID_ABI)   # the first build: SDL3, libpng and FreeType for the ABI
        if A64 and options.android_memory == "translated":
            build_android_translated(options)
            return
    elif WINDOWS:
        if options.backend != "sdl":
            sys.exit("Windows builds use the SDL backend")
        if not os.path.exists(f"{WIN32_DEPS}/lib/libfreetype.a") or not os.path.exists(f"{WIN32_DEPS}/sdl"):
            os.chdir(ROOT)
            build_win32_deps.configure("x86_64" if X64 else "i686")
            build_win32_deps.main()   # the first build: the Windows libraries
        if not shutil.which(CC):
            sys.exit(f"{CC} is not on PATH (llvm-mingw)")
        if X64:
            os.chdir(ROOT)
            check_x64_compiler(options.build)
    else:
        # The Debian libraries and SDL3, fetched and built the first time.
        build_linux_sysroot.main()
        if options.backend == "sdl":
            NATIVE_CFLAGS.extend([f"-I{SDL_SOURCE}/include", f"-I{SDL_BUILD}/include-revision"])
    os.chdir(ROOT)
    os.makedirs(options.build + "/obj", exist_ok=True)
    if A64:
        # The canary: is the compiler's AArch64 __ptr32 store bug still there?
        # MEMORIES_PTR32_PASS=0 skips the pass, which is refused while it is.
        bug = ptr32_stores.compiler_bug(CC, ANDROID_FLAGS, options.build)
        skip = os.environ.get("MEMORIES_PTR32_PASS") == "0"
        if bug and skip:
            sys.exit("ptr32: compiler bug still present (" + ", ".join(bug) + "): the pass is needed; "
                     "MEMORIES_PTR32_PASS=0 refused")
        print("ptr32: compiler bug still present (" + ", ".join(bug) + "): pass needed" if bug else
              "ptr32: the compiler keeps store widths through __ptr32: the pass can be retired "
              "(notes/pc-build.md)" + ("; skipped" if skip else ""))
        PTR32_PASS = not skip
    headers = glob.glob("src/**/*.h", recursive=True) + glob.glob("mods/**/*.h", recursive=True) + [__file__, "config/pc/host_symbol_renames.txt"]
    if A64:
        headers.append("tools/pc/ptr32_stores.py")   # it rewrites every unit's IR (compile_unit)
    NEWEST_HEADER = max(os.path.getmtime(path) for path in headers)
    if A64:
        # The pass on or off (MEMORIES_PTR32_PASS) changes every unit's code:
        # a change of it recompiles them all, as a change of flags does.
        NEWEST_HEADER = max(NEWEST_HEADER, flags_changed(f"{options.build}/ptr32-pass.txt",
                                                         [f"ptr32_stores={int(PTR32_PASS)}"]))
    # A build folder may have been built last by another checkout: tmp is
    # shared by every worktree (a junction), and an object there newer than
    # this checkout's source can be another checkout's code. checkout.txt
    # names the checkout the folder was last built from, written when a
    # build ends; when it is not this one (or not there), everything is
    # compiled again. Another checkout's is removed first, so when this
    # build stops halfway the next one compiles everything too. Two builds
    # into one folder at once are not supported (no lock).
    checkout = f"{options.build}/checkout.txt"
    try:
        with open(checkout, encoding="utf-8") as handle:
            ours = handle.read() == os.path.realpath(ROOT) + "\n"
    except OSError:
        ours = False
    if not ours:
        if os.path.exists(checkout):
            os.remove(checkout)
        NEWEST_HEADER = float("inf")
    obj = lambda source: f"{options.build}/obj/{source.replace('/', '_')}.o"
    # main_menu is the only overlay with a private load address (0x80180000),
    # so it can simply be linked in. The 0x80168000 modules share one address
    # and need a loaded-module registry first.
    # src/pc/game holds the port's own game-side variables (the card tables
    # sized for more cards than the disc has): compiled and placed like game
    # code, so they sit in the fixed sections a save state carries.
    inventory = game_sources()
    resident = inventory["resident"]
    module_sources = {name: inventory[name] for name, _, _, _ in MODULES}
    game = resident + [source for name, _, _, _ in MODULES for source in module_sources[name]]
    renames_file = "config/pc/host_symbol_renames.txt"
    if WINDOWS:
        with open(renames_file) as handle, open(f"{options.build}/host_symbol_renames.txt", "w") as out:
            for line in handle:
                if line.split():
                    out.write(" ".join(PREFIX + name for name in line.split()) + "\n")
        renames_file = f"{options.build}/host_symbol_renames.txt"
    # Test-only paths (MEMORIES_TEST_EXEC_GUEST, src/pc/guest/image.c) are
    # left out of a release: virus scanners' heuristics hold executable
    # memory against a program. smoke.py runs them in the other builds.
    if not options.release:
        NATIVE_CFLAGS.append("-DMEMORIES_TEST_HOOKS")
    native_newest = max(NEWEST_HEADER, flags_changed(f"{options.build}/native-flags.txt", NATIVE_CFLAGS))
    jobs = [(s, obj(s), CFLAGS, renames_file, NEWEST_HEADER) for s in game]
    jobs += [(s, obj(s), NATIVE_CFLAGS, None, native_newest) for s in NATIVE]
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        list(pool.map(compile_unit, jobs))

    # Shared-bank modules: a symbol that another module or the resident image
    # also defines, or that their ELFs place at a different address, becomes
    # <module>__<name> inside that module. Their variables move to sections
    # of their own so the registry can reinitialize them on every load.
    resident_elf, module_elf, text = guest_addresses()
    module_symbols = {name: symbols([obj(s) for s in module_sources[name]]) for name, _, _, _ in MODULES}
    resident_defined = symbols([obj(s) for s in resident])[0]
    renamed, sections = {}, {}
    for name, _, _, bank in MODULES:
        if not bank:
            continue
        defined, common, wanted_here = module_symbols[name]
        others = [other for other, _, _, _ in MODULES if other != name]
        clash = {symbol for symbol in defined | common
                 if symbol in resident_defined or any(symbol in module_symbols[o][0] | module_symbols[o][1] for o in others)}
        clash |= {symbol for symbol in (wanted_here | common) - defined
                  if symbol in module_elf[name] and symbol not in resident_defined and any(
                      places.get(symbol, module_elf[name][symbol]) != module_elf[name][symbol]
                      for places in [resident_elf] + [module_elf[o] for o in others])}
        if name in GATED_MODULES:
            # Everything it defines: a resident call by the retail name (the
            # credits' func_801807B0) must still reach the gate at that address.
            clash |= defined | common
        # clang's weak copies of the AArch64 branch thunks are in every object and must stay the one name the
        # port's strong thunks replace (check_arm_branches).
        renamed[name] = {symbol: f"{name}__{symbol}" for symbol in clash
                         if not symbol.startswith(f"{name}__") and not symbol.startswith("__llvm_slsblr_thunk_")}
        for source in module_sources[name]:
            command = [OBJCOPY]
            for old, new in sorted(renamed[name].items()):
                command.append(f"--redefine-sym={PREFIX}{old}={PREFIX}{new}")
            if WINDOWS:
                rename_coff_sections(obj(source), {".data": f"ovl_{name}_data$m", ".bss": f"ovl_{name}_bss$m",
                                                   ".sdata": f"ovl_{name}_data$n", ".sbss": f"ovl_{name}_data$n"})
            else:
                for section in (".data", ".sdata"):
                    command.append(f"--rename-section={section}=ovl_{name}_data")
                for section in (".bss", ".sbss"):
                    command.append(f"--rename-section={section}=ovl_{name}_bss")
            if len(command) > 1:
                run(command + [obj(source)])
        headers_text = run([OBJDUMP, "-h", *[obj(s) for s in module_sources[name]]])
        sections[name] = [kind for kind in ("data", "bss") if f"ovl_{name}_{kind}" in headers_text]
        # A tentative definition (-fcommon) is in no section yet: it would
        # become host data the image never sees, so it counts too.
        if name in GATED_MODULES and (common or any(
                len(parts) > 2 and parts[1].startswith(f"ovl_{name}_") and int(parts[2], 16)
                for parts in (line.split() for line in headers_text.splitlines()))):
            sys.exit(f"{name}: a gated module with variables of its own")

    # The game's small-data variables (section(".sdata") in the sources, the
    # retail .sdata the matching build keeps them in) are game variables as
    # much as .data's: in a host section of their own they were in no save
    # state, so a state loaded in the opening movie decoded into the wrong
    # one of its two buffers (D_8009B066) from then on. ELF renames them
    # with .data (below); here they go after it ($n), where no variable
    # that was there before moves, and states from before keep loading
    # (state.c takes a shorter variables chunk as the start of these).
    # .sbss too: COFF has it as initialized data (zeros), and with game_bss's
    # uninitialized parts lld would make it a second game_bss outside the
    # markers.
    for source in game if WINDOWS else []:
        rename_coff_sections(obj(source), {".text": "game_text$m", ".rdata": "game_rodata$m",
                                           ".data": "game_data$m", ".bss": "game_bss$m",
                                           ".sdata": "game_data$n", ".sbss": "game_data$n"})
    for source in game if not WINDOWS else []:
        run([OBJCOPY, "--rename-section=.text=game_text", "--rename-section=.rodata=game_rodata",
             "--rename-section=.data=game_data", "--rename-section=.sdata=game_data",
             "--rename-section=.bss=game_bss", "--rename-section=.sbss=game_bss", obj(source)])
    # A shared object (Android) is loaded where the system chooses, so its
    # sections cannot be fixed either.
    fixed = dict(FIXED_SECTIONS) if not WINDOWS and not ANDROID else {}
    for index, (name, _, _, bank) in enumerate(module for module in MODULES if module[3] and fixed):
        fixed[f"ovl_{name}_data"] = MODULE_SECTIONS + index * 0x400000
        fixed[f"ovl_{name}_bss"] = MODULE_SECTIONS + index * 0x400000 + 0x200000
    digest = hashlib.sha256()
    for path in sorted(game + [h for h in glob.glob("src/**/*.h", recursive=True) if not h.startswith("src/pc/")]):
        with open(path, "rb") as handle:
            digest.update(path.encode() + b"\0" + handle.read())
    digest.update(" ".join(CFLAGS).encode() + repr(sorted(fixed.items())).encode())

    game_defined, tentative, undefined = symbols([obj(s) for s in game])
    for source in game:
        # Names an earlier build renamed away in this object (below): still
        # wanted, or the rebuilt table loses them (func_8001352C, for one).
        if os.path.exists(obj(source) + ".aliased"):
            with open(obj(source) + ".aliased") as handle:
                undefined.update(handle.read().split())
    native_defined, _, native_undefined = symbols([obj(s) for s in NATIVE])
    with open("config/slus_01411/functions.csv") as handle:
        rows = list(csv.DictReader(handle))
    functions = {row["name"]: row["status"] for row in rows}
    overlay_rows = []
    for name, _, identifier, bank in MODULES:
        with open(f"config/slus_01411/overlays/{MODULE_CONFIG.get(name, name)}_functions.csv") as handle:
            for row in csv.DictReader(handle):
                row["name"] = renamed.get(name, {}).get(row["name"], row["name"])
                row["bank"], row["identifier"] = bank, identifier if bank else 0
                if name not in GATED_MODULES:
                    overlay_rows.append(row)
    by_address = {int(row["address"], 16): row["name"] for row in rows}
    addresses = dict(resident_elf)
    for name, _, _, _ in MODULES:
        for symbol, address in module_elf[name].items():
            addresses.setdefault(renamed.get(name, {}).get(symbol, symbol), address)

    # A native definition replaces the game's: weaken the original so the
    # linker prefers src/pc/overrides (calls are symbol-relative at -O0).
    overridden = sorted(game_defined & native_defined)
    set_overridden = set(overridden)
    if WINDOWS:
        # No weak COFF definitions from objcopy: the native objects come
        # first in the link and lld keeps the first definition. Anything else
        # defined twice is still an error, as on Linux.
        twice = sorted(name for name, count in definitions([obj(s) for s in game + NATIVE]).items()
                       if count > 1 and name not in overridden)
        if twice:
            sys.exit("defined more than once: " + ", ".join(twice[:20]))
    elif overridden:
        # One listing of every game object rather than one `nm` for each:
        # spawning 546 of them cost five seconds of every build, which is
        # most of what `./play.sh` spends before the game appears. -A puts
        # the file each symbol came from at the head of its line.
        weaken, objects = {}, {obj(s) for s in game}
        for line in run([NM, "-A", "-g", "--defined-only", *sorted(objects)]).splitlines():
            # The file, then ":" and the address (GNU nm writes no space after
            # the colon, llvm-nm one): the last such colon, since a Windows
            # path's drive has one too.
            found = re.match(r"(.*):\s*[0-9A-Fa-f]+\s", line)
            path, rest = (found.group(1), line[found.end(1) + 1:]) if found else (line, "")
            if path not in objects:
                sys.exit(f"{NM} -A named an object the build does not know: {line}")
            if rest.split() and c_name(rest.split()[-1]) in set_overridden:
                weaken.setdefault(path, []).append(c_name(rest.split()[-1]))
        for source in game:
            hits = weaken.get(obj(source))
            if hits:
                run([OBJCOPY, *[f"--weaken-symbol={name}" for name in hits], obj(source)])
    # Position-independent i386 code (Android) names the GOT, which the
    # linker defines.
    wanted = (undefined | tentative | KEPT_FOR_MODS) - game_defined - native_defined - HOST_LIBC - {"_GLOBAL_OFFSET_TABLE_"}
    pinned, stubs, unknown, aliases = {}, [], [], {}
    for name in sorted(wanted):
        address = addresses.get(name)
        # Sources still using a function's address-based name reach the
        # renamed C definition, as the PS1 link's symbol aliases do.
        current = by_address.get(address if address is not None else
                                 int(name[5:], 16) if name.startswith("func_8") and len(name) == 13 else -1)
        if current and current != name and current in game_defined | native_defined:
            aliases[name] = current
            continue
        if name in functions or (address is not None and text[0] <= address < text[1]):
            stubs.append(name)
        elif address is not None:
            pinned[name] = host_address(address)
        else:
            unknown.append(name)
    # SDK globals that native library ports share with game code.
    for name in native_undefined - game_defined - native_defined:
        if name.startswith("D_8") and name in addresses:
            pinned[name] = host_address(addresses[name])
    # Overlay entry points and data live outside the resident image.
    stubs += [name for name in unknown if name in undefined]
    # Some pinned names are functions of a loadable module that the C calls
    # by name (func_8016AA6C: name entry, in the shared 0x80168000 bank). A
    # direct call to a guest address never reaches the indirect-branch
    # thunks, so each of those becomes a host entry that sends its address
    # through their resolver instead, which picks the resident module's
    # native function (or the MIPS interpreter) as a call through a pointer
    # would. Without DEP the pinned call ran the MIPS bytes.
    branches = {name: pinned.pop(name) for name in sorted(direct_branches([obj(s) for s in game + NATIVE], set(pinned)))}
    guest_branches = write_guest_branches(options.build, branches)
    if WINDOWS:
        # lld reads no GNU linker scripts: pins are absolute symbols from an
        # assembly file, and aliases rename the references in the objects
        # (lld does not resolve a symbol defined as another undefined one).
        with open(f"{options.build}/guest_symbols.s", "w") as handle:
            handle.writelines(f".globl {PREFIX}{name}\n.set {PREFIX}{name}, 0x{address:08X}\n"
                              for name, address in pinned.items())
        for source in game:
            unset_coff_commons(obj(source), set(pinned))
        if aliases:
            with open(f"{options.build}/aliases.txt", "w") as handle:
                handle.writelines(f"{PREFIX}{name} {PREFIX}{target}\n" for name, target in sorted(aliases.items()))
            for source in game:
                # The rename is in place, so the object no longer shows the
                # names: .aliased keeps them for the next build's alias list
                # (and so the mod exports), until the object is recompiled.
                # An object renamed before keeps its earlier names too.
                hits = {name[len(PREFIX):] for name in run([NM, "-u", obj(source)]).split()} & set(aliases)
                if hits:
                    if os.path.exists(obj(source) + ".aliased"):
                        with open(obj(source) + ".aliased") as handle:
                            hits.update(handle.read().split())
                    with open(obj(source) + ".aliased", "w") as handle:
                        handle.writelines(f"{name}\n" for name in sorted(hits))
                    run([OBJCOPY, f"--redefine-syms={options.build}/aliases.txt", obj(source)])
        # __start_/__stop_ for the sections state.c and the module registry
        # walk: marker sections that sort before and after the contents.
        with open(f"{options.build}/section_markers.s", "w") as handle:
            marked = [("game_text", "xr"), ("game_data", "dw"), ("game_bss", "bw")]
            marked += [(f"ovl_{name}_{kind}", "dw" if kind == "data" else "bw")
                       for name, _, _, bank in MODULES if bank for kind in sections[name]]
            for section, flags in marked:
                start, stop = f"{PREFIX}__start_{section}", f"{PREFIX}__stop_{section}"
                handle.write(f'.section {section}$a,"{flags}"\n.globl {start}\n{start}:\n')
                handle.write(f'.section {section}$z,"{flags}"\n.globl {stop}\n{stop}:\n')
    else:
        # HIDDEN: in a shared object (Android's libgame.so) the dynamic
        # loader adds the load bias to an absolute symbol of default
        # visibility, as bionic does on every ABI; a hidden one is resolved
        # at link time. In an executable it only keeps the pins out of the
        # dynamic symbol table (nm shows them as local).
        with open(f"{options.build}/guest_symbols.ld", "w") as handle:
            handle.writelines(f"HIDDEN({name} = 0x{address:08X});\n" for name, address in pinned.items())
            handle.writelines(f"{name} = {target};\n" for name, target in aliases.items())
    with open(f"{options.build}/stubs.c", "w") as handle:
        handle.write('#include "pc/guest/image.h"\n')
        handle.write(f"const unsigned Memories_GameFingerprint = 0x{digest.hexdigest()[:8]}u;\n")
        handle.writelines(f'void {name}(void) {{ Memories_Unimplemented("{name}"); }}\n'
                          for name in sorted(stubs))
        # Guest address -> native function, for calls through pointers stored
        # in the retail data image (see on_fault in src/pc/guest/image.c).
        linked = game_defined | native_defined | set(stubs) | set(aliases)
        mapped = [(int(row["address"], 16), row["name"], row.get("bank", 0), row.get("identifier", 0))
                  for row in rows + overlay_rows if row["name"] in linked]
        # MODEL.MRG swaps per-monster MIPS control modules into these four
        # fixed entry addresses. Native bridges preserve their lifecycle
        # contract until the individual choreography modules are translated.
        mapped += [(0x8013A004, "Memories_ModelPrimaryControlA", 0, 0),
                   (0x8013B004, "Memories_ModelVariantControlA", 0, 0),
                   (0x801462B0, "Memories_DuelEffectControl", 0, 0),
                   (0x801807B0, "Memories_CreditsInit", 0, 0),
                   (0x80180A24, "Memories_CreditsUpdate", 0, 0),
                   (0x80181C4C, "Memories_CreditsLines", 0, 0),
                   (0x8017A004, "Memories_ModelPrimaryControlB", 0, 0),
                   (0x8017B004, "Memories_ModelVariantControlB", 0, 0)]
        mapped.sort()
        handle.writelines(f"extern void {name}(void);\n" for name in sorted({m[1] for m in mapped} - set(stubs)))
        handle.write("const MemoriesGuestFunction Memories_FunctionMap[] = {\n")
        handle.writelines(f"    {{0x{address:08X}u, {name}, 0x{bank:08X}u, 0x{identifier:X}u}},\n"
                          for address, name, bank, identifier in mapped)
        handle.write(f"}};\nconst unsigned Memories_FunctionMapCount = {len(mapped)};\n")
        shared = [(name, identifier, bank) for name, _, identifier, bank in MODULES
                  if bank and name not in GATED_MODULES]
        for name, _, _ in shared:
            for kind in sections[name]:
                handle.write(f"extern char __start_ovl_{name}_{kind}[], __stop_ovl_{name}_{kind}[];\n")
        handle.write("const MemoriesModule Memories_Modules[] = {\n")
        for name, identifier, bank in shared:
            ranges = [f"__start_ovl_{name}_{kind}, __stop_ovl_{name}_{kind}" if kind in sections[name] else "0, 0"
                      for kind in ("data", "bss")]
            handle.write(f'    {{"{name}", 0x{bank:08X}u, 0x{identifier:X}u, {", ".join(ranges)}}},\n')
        handle.write(f"}};\nconst unsigned Memories_ModuleCount = {len(shared)};\n")
    run([CC, *NATIVE_CFLAGS, "-c", f"{options.build}/stubs.c", "-o", f"{options.build}/stubs.o"])
    if A64:
        check_arm_branches([obj(s) for s in game + NATIVE] + [guest_branches])
    write_mod_exports(options.build, game_defined | native_defined | tentative | set(pinned) | set(branches) | set(stubs),
                      aliases)
    version = write_version(options.build, force=not ours)
    output = f"{options.build}/memories-pc"
    if WINDOWS:
        output += ".exe"
        resources = exe_resources(options.build, options.release)
        for name in ("guest_symbols", "section_markers"):
            run([CC, "-c", f"{options.build}/{name}.s", "-o", f"{options.build}/{name}.o"])
        # The pins first: a game unit's tentative definition of a pinned
        # variable is a COMMON symbol, which the linker script's assignment
        # overrides on Linux; lld keeps whichever it saw first. Then the
        # native objects, which win over the game definitions they override.
        # Large-address-aware for guest RAM at 0x80000000, fixed base (like
        # -no-pie) for the symbol table, NX for the guest-call trap. Mods
        # bind through mod_exports.o, not an export table.
        # -debug:symtab keeps the COFF symbol table beside the PDB (--pdb
        # alone drops it): the save-state tables below are read from it
        # with nm, and an empty one gave every build the same id.
        # -pdbaltpath records the PDB by bare name, not the builder's path:
        # the GitHub runner's D:/a/... path was enough for Bitdefender to
        # flag the CI builds (Gen:Variant.Yogi) when local ones passed.
        # The 64-bit image goes at 0x40000000, without ASLR: game code keeps
        # native function addresses in 4-byte guest slots, and reaches the
        # pinned guest variables (0x80000000 up to the stack at 0xB0800000)
        # RIP-relative, within 2 GB (notes/pc-build.md, "64-bit Windows").
        layout = (["-Wl,--image-base=0x40000000", "-Wl,--disable-dynamicbase", "-Wl,--disable-high-entropy-va"]
                  if X64 else ["-Wl,--large-address-aware", "-Wl,--disable-dynamicbase"])
        run([CC, *(["-mwindows"] if options.release else []), "-o", output, f"-Wl,--pdb={options.build}/memories-pc.pdb",
             "-Wl,-Xlink=-debug:symtab", "-Wl,-Xlink=-pdbaltpath:%_PDB%",
             *layout, "-Wl,--nxcompat",
             "-Wl,--allow-multiple-definition", f"{options.build}/guest_symbols.o",
             *[obj(s) for s in NATIVE + game], f"{options.build}/stubs.o", guest_branches, f"{options.build}/mod_exports.o",
             version, f"{options.build}/section_markers.o", *resources,
             f"{WIN32_DEPS}/sdl/lib/libSDL3.dll.a", "-lopengl32", f"{WIN32_DEPS}/lib/libfreetype.a",
             f"{WIN32_DEPS}/lib/libpng16.a", f"{WIN32_DEPS}/lib/libzs.a", "-ldbghelp", "-lwinhttp", "-lws2_32", "-lwinmm", "-static", "-lpthread"])
        shutil.copy(f"{WIN32_DEPS}/sdl/bin/SDL3.dll", options.build)
    elif ANDROID:
        # The game is libgame.so, linked at ANDROID_GAME_BASE; libmain.so,
        # the name SDL's Java shell loads, is the loader that puts it there
        # (ANDROID_LOADER). -Bsymbolic: every reference to the object's own
        # globals binds inside it, so the hand-written assembly and the
        # generated stubs reach them without a PLT (whose i386 form needs
        # EBX to hold the GOT), and a system library of the same name cannot
        # take them over. Everything the link leaves undefined is an error,
        # as in an executable. The libraries: SDL3's shared object (packaged
        # beside it), libpng and FreeType linked in, the system's zlib, GLES
        # (sdl.c's glGetString), the log and the NDK's native window.
        # --wrap=exit: the game's exit() is android.c's __wrap_exit, which
        # ends the process without the system libraries' static destructors
        # (the activity's threads still use what they free).
        output = f"{options.build}/libgame.so"
        run([CC, *ANDROID_FLAGS, "-shared", "-o", output, "-Wl,-Bsymbolic", "-Wl,--no-undefined", "-Wl,--wrap=exit",
             f"-Wl,--image-base=0x{ANDROID_GAME_BASE:08X}", "-Wl,-soname,libgame.so",
             "-Wl,-z,noexecstack", *[obj(s) for s in game + NATIVE],
             f"{options.build}/stubs.o", guest_branches, f"{options.build}/mod_exports.o", version,
             f"{options.build}/guest_symbols.ld", f"-L{ANDROID_DEPS}/lib", "-lSDL3", f"{ANDROID_DEPS}/lib/libfreetype.a",
             f"{ANDROID_DEPS}/lib/libpng16.a", "-lz", "-lGLESv2", "-llog", "-landroid", "-lm", "-ldl"])
    else:
        # Mods bind through mod_exports.o, so nothing needs -rdynamic.
        # FreeType, fontconfig and libpng linked in, so the player needs no
        # 32-bit copies of them: only libc and the GL driver (or X11 and
        # ALSA for the x11 backend), which come with the system.
        fonts = ["-Wl,-Bstatic", "-lfontconfig", "-lfreetype", "-lpng16", "-lbrotlidec", "-lbrotlicommon", "-lbz2",
                 "-lexpat", "-luuid", "-lz", "-Wl,-Bdynamic"]
        system = ["-ldl", "-lpthread", "-lrt", *SYSROOT_LINK]   # glibc < 2.34 keeps timers in librt
        libraries = ["-lm", f"{SDL_BUILD}/libSDL3.a", *fonts, "-lGL", *system]
        run(["gcc", "-m32", "-no-pie", "-o", output, *build_linux_sysroot.startfiles(),
             *[f"-Wl,--section-start={name}=0x{address:08X}" for name, address in sorted(fixed.items())],
             *[obj(s) for s in game + NATIVE],
             f"{options.build}/stubs.o", guest_branches, f"{options.build}/mod_exports.o", version, f"{options.build}/guest_symbols.ld", *(libraries if options.backend == "sdl"
               else ["-lm", *fonts, "-lX11", "-lXext", "-lasound", *system]), *build_linux_sysroot.endfiles()])
    if ANDROID:
        link_android_loader(options.build, output)
    # Code mods: an object per target (build_mod.py), the one for this game
    # beside it (and in the APK's assets).
    # The aarch64 objects need clang with lld: the NDK's has both, where a
    # CI runner's /usr/bin/clang comes without ld.lld (build_mod.py then
    # falls back to gcc and stops).
    if A64 and not os.environ.get("MEMORIES_MOD_CC"):
        os.environ["MEMORIES_MOD_CC"] = CC
    build_mods(options.build, options.release, target="x86_64-windows" if X64 else "aarch64" if A64 else "i386")
    copy_languages(options.build, options.release)
    in_game = lambda address: any(start <= address < start + 0x00400000 for start in fixed.values())
    build_id = write_build_identity(options.build, output, lambda kind, address: kind in "Tt" or
                                    (kind in "DdBb" and in_game(address)),
                                    older=digest.hexdigest()[:8])  # serves states saved before build ids
    if ANDROID:
        package_android_build(options.build, output, build_id)
    kinds = {name: functions.get(name, "outside_resident_image") for name in stubs}
    report = {"game_units": len(game), "pinned_data_symbols": len(pinned),
              "stubbed": {kind: sorted(n for n in stubs if kinds[n] == kind)
                          for kind in sorted(set(kinds.values()))}}
    with open(f"{options.build}/link-report.json", "w") as handle:
        json.dump(report, handle, indent=1)
    print(f"{output}: {len(game)} game units, {len(pinned)} pinned data symbols, " +
          ", ".join(f"{len(v)} {k} stubs" for k, v in report["stubbed"].items()))
    with open(checkout, "w", encoding="utf-8") as handle:
        handle.write(os.path.realpath(ROOT) + "\n")

if __name__ == "__main__":
    main()
