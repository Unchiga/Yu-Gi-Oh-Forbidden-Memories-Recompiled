#!/usr/bin/env python3
"""Build the game for sharing: archives for Windows (32-bit and 64-bit) and Linux.

    python3 tools/pc/package.py                # all three, into dist/
    python3 tools/pc/package.py windows-x64    # the 64-bit Windows one only

Packing all three where the 64-bit Windows game cannot be built (no
x86_64-w64-mingw32-clang 21 or later) skips that one with a message and
packs the other two, whose SDKs then need its export list from a build of
the same tree made elsewhere (below); asking for windows-x64 by name stops
there instead.

Each archive is a folder a player unpacks and runs: the executable, the mods
the release ships (the same object files for both systems), the mod SDK,
the official European languages' text (languages/, Game > Language), the
symbol table save states and crash reports use, an empty "game" folder for
the player's own disc image, and README.txt (tools/pc/release). Nothing else
from the game's discs is included.

The Linux executable is built against Debian 11's libraries
(tools/pc/build_linux_sysroot.py), as every Linux build is, so it runs on
other people's Linux. Every build is smoke tested before it is packed.

The 64-bit Windows archive (-windows-x64.zip, notes/pc-build.md "64-bit
Windows") unpacks to a folder of its own (yfm-redecomp-<version>-x64). Its
mods are the same, with each code mod's x86_64-windows object in place of
the 32-bit one, its SDK the same (build_mod.py builds every target), and
its README's Mods section says that a code mod needs a 64-bit build
there.

Every archive's SDK carries the export list of each target build_mod.py
builds for (sdk/exports.<target>.txt: i386, x86_64-windows,
aarch64-translated, with the Android game's guest-pass tables beside its
list: TRANSLATION_TABLES), so it checks every object it makes, not only the
one for the game beside it.
A game build writes only its own target's list; the others come from those
targets' game builds of the same tree (their <build>/source, which
build_game32.py writes, must be this build's): tmp/pc/win32 (or game32),
tmp/pc/win64 and tmp/pc/android-arm64-v8a, or the build folders
--exports-from names (CI, where each is built in its own job and only its
sdk/exports.<target>.txt and source are handed on). Packing stops when one
is missing (before anything is built) or from another tree (after the
builds). So packing the default three needs the Android build of the same
tree first (build_game32.py --target android-arm64-v8a --release; notes/
pc-release.md, "Local commands").

Every SDK also carries the one-click build (tools/pc/sdk): sdk/build-mod.bat
and sdk/build-mod.sh, which fetch their compiler (and, on Windows, a Python)
once and build a code mod with nothing installed, and sdk/toolchain-pins.json,
what they download.

Every SDK also carries the guest pass (sdk/tools/memories-guest-ir[.exe]),
which build_mod.py runs an Android (aarch64-translated) object through: a
program of the archive's system, built against the LLVM of the SDK's own
clang (tools/pc/build_sdk_guest_ir.py; the release workflow's sdk-guest-ir
job), named with --guest-ir (default: tmp/pc/sdk-guest-ir). Packing stops,
before anything is built, without one.

    python3 tools/pc/package.py android-arm64  # the Android app, signed

android-arm64 is not among the default ones (it needs the Android SDK, the
NDK and a JDK; notes/pc-build.md, "Android"). Its package is the APK itself,
dist/yfm-redecomp-<version>-android-arm64.apk, and it is made only with the
release key: MEMORIES_ANDROID_KEYSTORE and its password must be set
(tools/pc/package_android.py, notes/pc-release.md "Android signing"), and an
APK signed with the debug key is never put in dist/. A build_game32.py
--target android-arm64-v8a build without them is the debug-signed APK for
testing, in its build folder."""
import argparse, datetime, os, re, shutil, struct, subprocess, sys, tarfile, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DIST = os.path.join(ROOT, "dist")
NAME = "yfm-redecomp"
# memories-pc.pdb: the Windows build's symbols, for a debugger or profiler.
BUILDS = {"windows": ("tmp/pc/win32", "memories-pc.exe", ["SDL3.dll", "memories-pc.pdb"]),
          "windows-x64": ("tmp/pc/win64", "memories-pc.exe", ["SDL3.dll", "memories-pc.pdb"]),
          "linux": ("tmp/pc/game32", "memories-pc", []),
          "android-arm64": ("tmp/pc/android-arm64-v8a", "memories-arm64-v8a.apk", [])}
SUFFIX = {"windows": "-windows.zip", "windows-x64": "-windows-x64.zip", "linux": "-linux.tar.gz",
          "android-arm64": "-android-arm64.apk"}
# build_game32.py's --target for a system, where it is not the same name.
TARGETS = {"android-arm64": "android-arm64-v8a"}
# The archive's top folder: the two Windows archives unpacked side by side
# must not mix (one game's mod objects beside the other executable).
FOLDER = {"windows-x64": "-x64"}
# The mod target each system's game loads (build_mod.py's TARGETS), and the
# builds another system's archive takes that target's export list from, in
# the order they are looked in (the 32-bit Windows game's for i386 first).
MOD_TARGET = {"windows": "i386", "linux": "i386", "windows-x64": "x86_64-windows",
              "android-arm64": "aarch64-translated"}
SDK_TARGETS = ("i386", "x86_64-windows", "aarch64-translated")
EXPORT_BUILDS = {"i386": ["windows", "linux"], "x86_64-windows": ["windows-x64"],
                 "aarch64-translated": ["android-arm64"]}
# What build_mod.py also needs of the Android game to build its object (the
# guest pass's tables, build_game32.write_sdk), beside its export list.
TRANSLATION_TABLES = ("mod_translation.json", "mod_signatures.json")
# The one-click build, from tools/pc/sdk into every SDK: the launchers and the
# pins they download (the launchers carry the same values; test_sdk_launchers.py).
SDK_LAUNCHERS = ("build-mod.bat", "build-mod.sh", "toolchain-pins.json")
LAUNCHER_SH = "build-mod.sh"
# What the README's Mods section adds for the 64-bit Windows game, before
# its paragraph on installing mods (tools/pc/release/README.txt).
X64_MODS = """This is the 64-bit Windows game. A mod that contains code needs a 64-bit
build of it here (a <name>.x86_64-windows.o beside its 32-bit <name>.o);
the mods above come with theirs. One that has only the 32-bit build stays
off, with "needs a 64-bit build of this mod" beside it in Game > Mods: play
it with the 32-bit game, or ask its author for the 64-bit build.

"""
GAME_README = """Start memories-pc and choose your own ROM in the welcome screen.
Alternatively, put your raw image of Forbidden Memories (USA, SLUS-01411)
here: the .bin file of a .bin/.cue pair. Any file name ending in .bin will do.
"""


def version():
    commit = subprocess.run(["git", "describe", "--always", "--dirty"], cwd=ROOT, capture_output=True,
                            text=True).stdout.strip() or "unknown"
    return f"{datetime.date.today():%Y%m%d}-{commit}"


def x64_toolchain_missing():
    """Why the 64-bit Windows game cannot be built here, or None: it needs
    x86_64-w64-mingw32-clang, clang 21 or later (build_game32.py's compiler
    gate refuses older ones), looked for where the build looks too."""
    if sys.platform != "win32":     # the llvm-mingw fetched into tmp/pc (on Windows it would fetch one)
        import build_win32_deps
        build_win32_deps.use_toolchain()
    compiler = shutil.which("x86_64-w64-mingw32-clang")
    if not compiler:
        return "x86_64-w64-mingw32-clang is not on PATH"
    first = (subprocess.run([compiler, "--version"], capture_output=True, text=True).stdout.splitlines() or ["?"])[0]
    match = re.search(r"clang version (\d+)", first)
    if not match or int(match.group(1)) < 21:
        return f"{compiler} is {first}, and the 64-bit game needs clang 21 or later"
    return None


def build(system, label, skip_smoke=False):
    command = [sys.executable, "tools/pc/build_game32.py", "--target", TARGETS.get(system, system),
               "--backend", "sdl", "--release", "--build", BUILDS[system][0]]
    # The label is the version the update check compares (notes/updates.md);
    # one that is not vX.Y.Z[-PRE] makes a build that never checks.
    subprocess.run(command, cwd=ROOT, check=True, env=dict(os.environ, MEMORIES_VERSION=label))
    if skip_smoke or system == "android-arm64":
        print("package: gameplay smoke tests skipped " +
              ("(no disc required)" if skip_smoke else "(the smoke test runs desktop games only)"))
        return
    smoke = [sys.executable, "tools/pc/smoke.py"]
    smoke += ["--windows"] if system == "windows" else ["--executable", os.path.join(BUILDS[system][0], BUILDS[system][1])]
    if subprocess.run(smoke, cwd=ROOT).returncode:
        sys.exit(f"package: the {system} build failed its smoke test; nothing was packed")


def strip(executable):
    """Drop the COFF symbol table and DWARF from the shipped .exe. The build
    keeps the table only to write symbols/<id>.txt, which save states and
    crash reports read; memories-pc.pdb serves debuggers. Left in, it is
    some 350 KB after the last section, where scanners' heuristics expect a
    dropper's payload (Bitdefender flagged v0.1.2 as Gen:Variant.Yogi)."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    if sys.platform != "win32":     # the llvm-mingw fetched into tmp/pc (on Windows it would fetch one)
        import build_win32_deps
        build_win32_deps.use_toolchain()
    subprocess.run(["llvm-strip", "--strip-all", executable], check=True)
    set_pe_checksum(executable)


def set_pe_checksum(executable):
    """Fill in the PE header's CheckSum, which the linker leaves 0 (Windows
    checks it only for drivers). Virus scanners' models read a zero or wrong
    one as a sign of a hand-made or patched file; the Microsoft toolchain's
    /RELEASE writes it. The algorithm is ImageHlp's CheckSumMappedFile."""
    with open(executable, "rb") as handle:
        data = bytearray(handle.read())
    field = int.from_bytes(data[0x3C:0x40], "little") + 24 + 64   # e_lfanew, PE signature + COFF header, CheckSum
    data[field:field + 4] = bytes(4)
    padded = data + bytes(len(data) % 2)
    total = 0
    for (word,) in struct.iter_unpack("<H", padded):
        total += word
        total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    data[field:field + 4] = (total + len(data)).to_bytes(4, "little")
    with open(executable, "wb") as handle:
        handle.write(data)


def read_source(build_dir):
    """The tree a game build was made from (build_game32.py's source_stamp),
    or None."""
    try:
        with open(os.path.join(build_dir, "source")) as handle:
            return handle.read().strip() or None
    except OSError:
        return None


def shown(path):
    """`path` as messages name it: from the checkout's root when it is on
    the same drive."""
    try:
        return os.path.relpath(path, ROOT)
    except ValueError:
        return path


def export_folders(target, exports_from=()):
    """Where `target`'s export list is looked for, in order: the folders
    --exports-from names, then the target's game builds in tmp/pc."""
    return ([os.path.abspath(path) for path in exports_from] +
            [os.path.join(ROOT, BUILDS[other][0]) for other in EXPORT_BUILDS[target]])


def build_command(target):
    system = EXPORT_BUILDS[target][0]
    return (f"python tools/pc/build_game32.py --target {TARGETS.get(system, system)} --backend sdl --release "
            f"--build {BUILDS[system][0]}")


def exports_unavailable(systems, exports_from=(), skipped=None):
    """Before anything is built: why an SDK of `systems` could not get
    another target's export list (none of `systems` builds that target, and
    no folder has a list of it), or None. Whether a list is of this tree is
    checked after the builds (sdk_exports)."""
    for folder in exports_from:
        if not os.path.isdir(folder):
            return f"--exports-from {folder}: no such folder"
    built = {MOD_TARGET[system] for system in systems}
    for system in systems:
        if system == "android-arm64":
            continue
        for target in SDK_TARGETS:
            if target in built or any(os.path.isfile(os.path.join(folder, "sdk", f"exports.{target}.txt"))
                                      for folder in export_folders(target, exports_from)):
                continue
            why = (f"{EXPORT_BUILDS[target][0]} is skipped ({skipped})" if skipped and target == "x86_64-windows"
                   else f"{EXPORT_BUILDS[target][0]} is not packed here")
            return (f"the {system} SDK needs the {target} game's export list: {why}, and no build has one "
                    f"({', '.join(shown(folder) for folder in export_folders(target, exports_from))}). "
                    f"Build it ({build_command(target)}) or name a build of this tree with --exports-from")
    return None


# What starts a program of each system (the guest pass in an SDK).
PROGRAM_MAGIC = {"windows": b"MZ", "windows-x64": b"MZ", "linux": b"\x7fELF"}
GUEST_IR = "memories-guest-ir"


def guest_ir_for(system, paths=()):
    """The guest pass (tools/pc/build_sdk_guest_ir.py) for `system`'s SDK
    among `paths` (default: the one built for this host), or None: a program
    of that system's kind, by its first bytes."""
    paths = list(paths) or [os.path.join(ROOT, "tmp", "pc", "sdk-guest-ir", GUEST_IR + (".exe" if sys.platform == "win32" else ""))]
    for path in paths:
        try:
            with open(path, "rb") as handle:
                if handle.read(len(PROGRAM_MAGIC[system])) == PROGRAM_MAGIC[system]:
                    return os.path.abspath(path)
        except OSError:
            continue
    return None


def guest_ir_unavailable(systems, paths=()):
    """Before anything is built: why an SDK of `systems` could not get its
    guest pass, or None."""
    for system in systems:
        if system != "android-arm64" and not guest_ir_for(system, paths):
            return (f"the {system} SDK needs the guest pass (a {system} program memories-guest-ir"
                    f"{'.exe' if system.startswith('windows') else ''}): none of "
                    f"{', '.join(shown(path) for path in paths) or 'tmp/pc/sdk-guest-ir'} is one. Build it on "
                    "that system with `python tools/pc/build_sdk_guest_ir.py` (the release workflow's sdk-guest-ir "
                    "job) or name it with --guest-ir")
    return None


def sdk_exports(system, exports_from=()):
    """{target: its export list} for the other targets than `system`'s own,
    each from a game build of the same tree as `system`'s (export_folders).
    Stops, with what to build, when one is missing or was built from
    another tree."""
    build_dir = os.path.join(ROOT, BUILDS[system][0])
    own = read_source(build_dir)
    if own in (None, "unknown"):
        sys.exit(f"package: {shown(build_dir)} has no source stamp: build it again with "
                 "tools/pc/build_game32.py, in a git checkout; nothing was packed")
    lists = {}
    for target in SDK_TARGETS:
        if target == MOD_TARGET[system]:
            continue
        others = []
        for folder in export_folders(target, exports_from):
            path = os.path.join(folder, "sdk", f"exports.{target}.txt")
            if not os.path.isfile(path) or (target == "aarch64-translated" and not all(
                    os.path.isfile(os.path.join(folder, "sdk", name)) for name in TRANSLATION_TABLES)):
                continue
            source = read_source(folder)
            if source == own:
                lists[target] = path
                break
            others.append(f"{shown(folder)} is from {source or 'an unknown tree'}")
        if target not in lists:
            found = "; ".join(others) if others else f"no build has its exports.{target}.txt"
            sys.exit(f"package: the {system} SDK needs the {target} game's export list from a build of this tree "
                     f"({own}): {found}. Build it ({build_command(target)}) or name one with --exports-from; "
                     "nothing was packed")
    return lists


def add_launchers(sdk):
    """The one-click build into `sdk` (the folder's sdk/): build-mod.bat with
    Windows' line ends (a batch file with bare LF mis-reads its labels),
    build-mod.sh and the JSON with LF, and build-mod.sh executable."""
    for name in SDK_LAUNCHERS:
        with open(os.path.join(ROOT, "tools", "pc", "sdk", name), "rb") as handle:
            data = handle.read().replace(b"\r\n", b"\n")
        if name.endswith(".bat"):
            data = data.replace(b"\n", b"\r\n")
        with open(os.path.join(sdk, name), "wb") as handle:
            handle.write(data)
    os.chmod(os.path.join(sdk, LAUNCHER_SH), 0o755)


def stage(system, label, exports=None, guest_ir=None):
    build_dir, executable, extras = BUILDS[system]
    build_dir = os.path.join(ROOT, build_dir)
    folder = os.path.join(DIST, "stage", system, f"{NAME}-{label}{FOLDER.get(system, '')}")
    shutil.rmtree(os.path.dirname(folder), ignore_errors=True)
    os.makedirs(os.path.join(folder, "game"))
    for name in [executable, "buildid", "commit"] + extras:
        shutil.copy2(os.path.join(build_dir, name), folder)
    if system.startswith("windows"):
        strip(os.path.join(folder, executable))
    for name in ("mods", "sdk", "languages"):
        shutil.copytree(os.path.join(build_dir, name), os.path.join(folder, name))
    # The guest pass build_mod.py runs the Android object through, where it
    # looks for it (beside build_mod.py).
    if guest_ir:
        target = os.path.join(folder, "sdk", "tools", GUEST_IR + (".exe" if system.startswith("windows") else ""))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copyfile(guest_ir, target)
        os.chmod(target, 0o755)
        if system.startswith("windows"):   # as the game's: stripped, with the PE checksum
            strip(target)
    add_launchers(os.path.join(folder, "sdk"))
    # The other targets' export lists, beside this game's own (sdk_exports).
    for target, path in (exports or {}).items():
        shutil.copyfile(path, os.path.join(folder, "sdk", f"exports.{target}.txt"))
        if target == "aarch64-translated":
            for name in TRANSLATION_TABLES:
                shutil.copyfile(os.path.join(os.path.dirname(path), name), os.path.join(folder, "sdk", name))
    # This build's symbol table, under its build id and under the game
    # fingerprint (the same table): not the ones earlier builds left there.
    with open(os.path.join(build_dir, "buildid")) as handle:
        current = os.path.join(build_dir, "symbols", handle.read().strip() + ".txt")
    os.makedirs(os.path.join(folder, "symbols"))
    for name in os.listdir(os.path.join(build_dir, "symbols")):
        path = os.path.join(build_dir, "symbols", name)
        with open(path, "rb") as a, open(current, "rb") as b:
            if a.read() == b.read():
                shutil.copy2(path, os.path.join(folder, "symbols", name))
    shutil.copy2(os.path.join(ROOT, "tools/pc/release/README.txt"), folder)
    if system == "windows-x64":
        with open(os.path.join(folder, "README.txt"), encoding="utf-8") as handle:
            text = handle.read()
        at = text.index("To install someone else's mod")
        with open(os.path.join(folder, "README.txt"), "w", encoding="utf-8") as handle:
            handle.write(text[:at] + X64_MODS + text[at:])
    shutil.copy2(os.path.join(ROOT, "LICENSE"), folder)
    with open(os.path.join(folder, "game", "README.txt"), "w", newline="\r\n" if system.startswith("windows") else "\n") as handle:
        handle.write(GAME_README)
    if system.startswith("windows"):   # Notepad and friends
        with open(os.path.join(folder, "README.txt"), encoding="utf-8") as handle:
            text = handle.read()
        with open(os.path.join(folder, "README.txt"), "w", encoding="utf-8", newline="\r\n") as handle:
            handle.write(text)
    return folder


def release_key_missing():
    """Why the Android APK cannot be packed, or None: only the release key
    signs what goes in dist/ (package_android.signing_key)."""
    if not os.environ.get("MEMORIES_ANDROID_KEYSTORE"):
        return ("MEMORIES_ANDROID_KEYSTORE is not set, and the APK in dist/ is signed with the release key only "
                "(notes/pc-release.md, \"Android signing\"); build_game32.py --target android-arm64-v8a makes "
                "a debug-signed one for testing")
    return None


def pack_apk(label):
    """dist/yfm-redecomp-<version>-android-arm64.apk: the APK the build
    signed, checked again here. The key named by MEMORIES_ANDROID_KEYSTORE
    signed it, which this checks as far as it can without the key: one
    signer, not the debug key, and MEMORIES_ANDROID_CERT_SHA256 if set."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import package_android
    build_dir, apk, _ = BUILDS["android-arm64"]
    source = os.path.join(ROOT, build_dir, apk)
    certificate = package_android.signer(source)
    if certificate["dn"] == package_android.DEBUG_DN:
        sys.exit(f"package: {os.path.relpath(source, ROOT)} is signed with the debug key; nothing was packed")
    expected = os.environ.get("MEMORIES_ANDROID_CERT_SHA256")
    mismatch = expected and package_android.check_fingerprint(certificate["sha256"], expected)
    if mismatch:
        sys.exit(f"package: {os.path.relpath(source, ROOT)}: {mismatch}; nothing was packed")
    # The version the APK was built with must be this package's (--version):
    # --no-build packs what is there, which may be an earlier build's.
    os.environ["MEMORIES_VERSION"] = label
    wanted, found = package_android.app_version(), package_android.manifest_version(source)
    if found != wanted:
        sys.exit(f"package: {os.path.relpath(source, ROOT)} is version {found[1]} (versionCode {found[0]}), "
                 f"not {wanted[1]} ({wanted[0]}) as --version {label} makes it: build it again; nothing was packed")
    os.makedirs(DIST, exist_ok=True)
    target = os.path.join(DIST, f"{NAME}-{label}{SUFFIX['android-arm64']}")
    shutil.copyfile(source, target)
    print(f"package: {os.path.relpath(target, ROOT)}: {found[1]} (versionCode {found[0]}), "
          f"signed by {certificate['dn']}, SHA-256 {package_android.colons(certificate['sha256'])}")
    return target


def pack(system, folder):
    name = os.path.basename(folder)
    name = name[:len(name) - len(FOLDER.get(system, ""))]
    base = os.path.join(DIST, name + SUFFIX[system])
    parent = os.path.dirname(folder)
    if system.startswith("windows"):
        with zipfile.ZipFile(base, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for directory, _, files in os.walk(folder):
                for name in sorted(files):
                    path = os.path.join(directory, name)
                    archive.write(path, os.path.relpath(path, parent))
    else:
        def executable(info):   # the program runs; everything else is plain data
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            if info.isfile():
                info.mode = 0o755 if info.name.endswith(("/memories-pc", "/sdk/tools/" + GUEST_IR, "/sdk/" + LAUNCHER_SH)) else 0o644
            return info
        with tarfile.open(base, "w:gz") as archive:
            archive.add(folder, os.path.basename(folder), filter=executable)
    return base


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("systems", nargs="*", help="windows, windows-x64, linux (default: those three), "
                                                    "android-arm64 (the APK, release key only)")
    parser.add_argument("--no-build", action="store_true", help="pack what is already built")
    parser.add_argument("--skip-smoke", action="store_true", help="build without ROM-dependent gameplay tests (CI)")
    parser.add_argument("--version", help="archive version, e.g. v0.1.0 or dev-abcdef0")
    parser.add_argument("--exports-from", action="append", default=[], metavar="BUILD",
                        help="another target's game build folder (its sdk/exports.<target>.txt and source) to take "
                             "that target's export list from for the SDK (repeatable; default: tmp/pc's builds)")
    parser.add_argument("--no-guest-ir", action="store_true",
                        help="pack the SDKs without the guest pass (the release workflow, on a pull request that "
                             "does not touch it and has no cached LLVM to build it from; never for a release)")
    parser.add_argument("--guest-ir", action="append", default=[], metavar="FILE",
                        help="a memories-guest-ir for the SDKs (tools/pc/build_sdk_guest_ir.py makes it, for the "
                             "system it runs on; repeatable, each SDK takes the one of its system; default: the "
                             "one in tmp/pc/sdk-guest-ir)")
    options = parser.parse_args()
    systems = options.systems or ["windows", "windows-x64", "linux"]
    if set(systems) - set(BUILDS):
        parser.error("systems are windows, windows-x64, linux and android-arm64")
    if "android-arm64" in systems and release_key_missing():
        sys.exit(f"package: cannot pack android-arm64: {release_key_missing()}")
    label = options.version or version()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,99}", label):
        parser.error("version must be 1-100 letters, numbers, dots, underscores or hyphens, starting with a letter or number")
    if "windows-x64" not in systems:
        missing = None
    elif options.no_build:
        executable = os.path.join(ROOT, BUILDS["windows-x64"][0], BUILDS["windows-x64"][1])
        missing = None if os.path.isfile(executable) else f"{os.path.relpath(executable, ROOT)} is not built"
    else:
        missing = x64_toolchain_missing()
    if missing and options.systems:
        sys.exit(f"package: cannot pack windows-x64: {missing}")
    if missing:
        # Packing everything: the other archives do not wait on it, when
        # their SDKs can have its export list from elsewhere (a build of the
        # same tree in tmp/pc/win64, or --exports-from).
        print(f"package: windows-x64 skipped: {missing}; the other archives are packed", file=sys.stderr)
        systems = [system for system in systems if system != "windows-x64"]
    unavailable = (exports_unavailable(systems, options.exports_from, missing) or
                   (None if options.no_guest_ir else guest_ir_unavailable(systems, options.guest_ir)))
    if unavailable:
        sys.exit(f"package: {unavailable}; nothing was packed")
    if not options.no_build:
        for system in systems:
            build(system, label, options.skip_smoke)
    # Every build first: each SDK takes the other targets' export lists from
    # theirs, and nothing is packed while one is missing.
    exports = {system: sdk_exports(system, options.exports_from) for system in systems if system != "android-arm64"}
    made = []
    for system in systems:
        made.append(pack_apk(label) if system == "android-arm64" else pack(system, stage(system, label, exports[system], None if options.no_guest_ir else guest_ir_for(system, options.guest_ir))))
    shutil.rmtree(os.path.join(DIST, "stage"), ignore_errors=True)
    for path in made:
        print(f"{os.path.relpath(path, ROOT)}: {os.path.getsize(path) / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
