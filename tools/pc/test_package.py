#!/usr/bin/env python3
"""Check release archives without a ROM: layout, modes, no user data."""
import argparse
import json
import struct
from pathlib import Path, PurePosixPath
import tarfile
import zipfile

# build_mod.py's targets: each has its export list in every SDK.
TARGETS = ("i386", "x86_64-windows", "aarch64-translated")


def read_bytes(path, name):
    if path.suffix == ".zip":
        with zipfile.ZipFile(path) as archive:
            return archive.read(name)
    with tarfile.open(path) as archive:
        return archive.extractfile(name).read()


def read_member(path, name):
    return read_bytes(path, name).decode()


def check_pe(image, label):
    """What virus scanners' heuristics hold against a program (notes/
    pc-release.md, "VirusTotal"): a COFF symbol table, data after the last
    section, no resource section (version information), a zero PE checksum
    (tools/pc/package.py strip fills it in). Returns the PE header's offset."""
    header = struct.unpack_from("<I", image, 0x3C)[0]
    sections, _, symbols, symbol_count, optional = struct.unpack_from("<HIIIH", image, header + 6)
    table = header + 24 + optional
    names, end = set(), 0
    for index in range(sections):
        at = table + 40 * index
        names.add(image[at:at + 8].rstrip(b"\0"))
        size, offset = struct.unpack_from("<II", image, at + 16)
        end = max(end, offset + size)
    assert symbols == 0 and symbol_count == 0, f"{label} keeps its COFF symbol table"
    assert len(image) == end, f"{label} has {len(image) - end} bytes after its last section"
    assert b".rsrc" in names, f"{label} has no resources (version information)"
    # The PE checksum package.py fills in after the strip: ImageHlp's sum
    # of the file's 16-bit words (its own field as 0), folded, plus the length.
    field = header + 24 + 64
    stored = struct.unpack_from("<I", image, field)[0]
    padded = image[:field] + bytes(4) + image[field + 4:] + bytes(len(image) % 2)
    total = sum(struct.unpack(f"<{len(padded) // 2}H", padded))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    assert stored == total + len(image), f"{label}'s PE checksum is {stored:#x}, not {total + len(image):#x}"
    return header


def check_exe(image):
    """The shipped .exe: stripped (tools/pc/package.py strip) and with a
    resource section (version information), since virus scanners flag data
    after the last section and programs that say nothing about themselves.
    Its PDB is named without the builder's path: Bitdefender flagged the CI
    builds, whose path was the GitHub runner's D:/a/..."""
    header = check_pe(image, "memories-pc.exe")
    marker = image.find(b"RSDS")
    assert marker >= 0, "memories-pc.exe has no CodeView record"
    pdb = image[marker + 24:image.index(b"\0", marker + 24)]
    assert pdb == b"memories-pc.pdb", f"memories-pc.exe names its PDB {pdb!r}, not by bare name"
    # What v0.1.4-preview.1 gained and 11 engines' models held against it
    # (notes/pc-release.md): DEP-policy imports and the executable-guest-RAM
    # test path, which a release leaves out (MEMORIES_TEST_HOOKS).
    for name in (b"SetProcessDEPPolicy", b"GetProcessDEPPolicy", b"GetSystemDEPPolicy", b"MEMORIES_TEST_EXEC_GUEST"):
        assert name not in image, f"memories-pc.exe contains {name.decode()}"


def check_launchers(path, root, contents, windows):
    """The one-click build (tools/pc/sdk): both launchers and the pins they download,
    in every SDK. The batch file with Windows' line ends (a bare LF mis-reads its
    labels), the shell script with LF and executable, and the hashes they carry
    the JSON's."""
    names = ("sdk/build-mod.bat", "sdk/build-mod.sh", "sdk/toolchain-pins.json")
    assert set(names) <= contents, f"the SDK lacks the one-click build: {sorted(set(names) - contents)}"
    bat = read_bytes(path, f"{root}/sdk/build-mod.bat")
    sh = read_bytes(path, f"{root}/sdk/build-mod.sh")
    assert bat.count(b"\n") == bat.count(b"\r\n") > 100, "sdk/build-mod.bat needs CRLF line ends"
    assert b"\r" not in sh and sh.startswith(b"#!/bin/sh\n"), "sdk/build-mod.sh needs LF line ends and a #!/bin/sh line"
    pins = json.loads(read_bytes(path, f"{root}/sdk/toolchain-pins.json"))
    for text, wanted in ((bat, (pins["llvm_mingw"]["windows"], pins["python"]["windows"])), (sh, (pins["llvm_mingw"]["linux"],))):
        for entry in wanted:
            assert entry["url"].encode() in text and entry["sha256"].encode() in text, \
                f"a launcher does not carry the pin {entry['url']} of toolchain-pins.json"
    if not windows:
        with tarfile.open(path) as archive:
            assert archive.getmember(f"{root}/sdk/build-mod.sh").mode & 0o111, "sdk/build-mod.sh must be executable"


def check(path, guest_ir_wanted=True):
    windows = path.suffix == ".zip"
    # The 64-bit Windows game (package.py windows-x64): an x86-64
    # executable, with its code mods' x86_64-windows objects.
    wide = path.name.endswith("-windows-x64.zip")
    if windows:
        with zipfile.ZipFile(path) as archive:
            assert archive.testzip() is None
            names = archive.namelist()
            image = archive.read(next(name for name in names if name.endswith("/memories-pc.exe")))
            check_exe(image)
            header = struct.unpack_from("<I", image, 0x3C)[0]
            machine = struct.unpack_from("<H", image, header + 4)[0]
            assert machine == (0x8664 if wide else 0x14C), f"memories-pc.exe is for machine {machine:#x}"
            sdl = archive.read(next(name for name in names if name.endswith("/SDL3.dll")))
            header = struct.unpack_from("<I", sdl, 0x3C)[0]
            machine = struct.unpack_from("<H", sdl, header + 4)[0]
            assert machine == (0x8664 if wide else 0x14C), f"SDL3.dll is for machine {machine:#x}"
            readme = archive.read(next(name for name in names if name.endswith("/README.txt") and name.count("/") == 1))
            assert wide == (b"needs a 64-bit build of this mod" in readme), \
                "the README's Mods section is not this archive's"
    else:
        with tarfile.open(path) as archive:
            members = archive.getmembers()
            names = [item.name for item in members if item.isfile()]
            assert all(item.isfile() or item.isdir() for item in members), "unexpected links or special files"
            executable = [item for item in members if item.name.endswith("/memories-pc")]
            assert len(executable) == 1 and executable[0].mode & 0o111, "Linux executable must be executable"
    roots = {PurePosixPath(name).parts[0] for name in names}
    assert len(roots) == 1, "archive needs one top-level folder"
    if windows:
        assert next(iter(roots)).endswith("-x64") == wide, "the two Windows archives need distinct top folders"
    assert all(not PurePosixPath(name).is_absolute() and ".." not in PurePosixPath(name).parts for name in names)
    contents = {str(PurePosixPath(name).relative_to(next(iter(roots)))) for name in names}
    required = {"README.txt", "LICENSE", "buildid", "commit", "game/README.txt",
                "memories-pc.exe" if windows else "memories-pc"}
    if windows:
        required |= {"SDL3.dll", "memories-pc.pdb"}
    assert required <= contents, f"missing files: {required - contents}"
    for directory in ("mods/", "symbols/", "sdk/"):
        assert any(name.startswith(directory) for name in contents), f"missing {directory}"
    # Each code mod beside the game has the object for this game's target,
    # and not another target's.
    objects = [name for name in contents if name.startswith("mods/") and name.endswith(".o")]
    ours = [name for name in objects if name.endswith(".x86_64-windows.o") == wide
            and not name.endswith((".aarch64.o", ".aarch64-translated.o"))]
    assert objects and ours == objects, f"mods/ has another target's objects: {sorted(set(objects) - set(ours))}"
    # The SDK's export lists: one per target build_mod.py builds for, so it
    # checks every object it makes (tools/pc/package.py), and for a 32-bit
    # game also exports.txt, the i386 list under the name SDKs had before.
    lists = {f"sdk/exports.{target}.txt" for target in TARGETS}
    if not wide:
        lists.add("sdk/exports.txt")
    assert lists <= contents, f"the SDK lacks export lists: {sorted(lists - contents)}"
    assert {name for name in contents if name.startswith("sdk/exports")} == lists, "the SDK has other export lists"
    # The Android game's guest-pass tables, for its aarch64-translated object.
    tables = {"sdk/mod_translation.json", "sdk/mod_signatures.json"}
    assert tables <= contents, f"the SDK lacks the Android game's tables: {sorted(tables - contents)}"
    # The guest pass, which build_mod.py runs the Android object through
    # (tools/pc/build_sdk_guest_ir.py): without it a modder cannot build that
    # object from the SDK. A program of this archive's system, a real one
    # (the LLVM it carries makes it megabytes), executable on Linux.
    guest_ir = "sdk/tools/memories-guest-ir" + (".exe" if windows else "")
    assert "sdk/tools/build_mod.py" in contents, "the SDK lacks tools/build_mod.py"
    if not guest_ir_wanted:
        assert guest_ir not in contents, f"{guest_ir} is in an SDK packed with --no-guest-ir"
    else:
        assert guest_ir in contents, f"the SDK lacks the guest pass ({guest_ir}): the Android object cannot be built from it"
        program = read_bytes(path, f"{next(iter(roots))}/{guest_ir}")
        assert program[:2] == b"MZ" if windows else program[:4] == b"\x7fELF", \
            f"{guest_ir} is not a {'Windows' if windows else 'Linux'} program"
        assert len(program) > 1_000_000, f"{guest_ir} is only {len(program)} bytes"
        if windows:
            # Treated as memories-pc.exe is, for the same scanners: no symbols,
            # nothing after the last section, version information, the checksum.
            header = check_pe(program, guest_ir)
            assert struct.unpack_from("<H", program, header + 4)[0] == 0x8664, f"{guest_ir} is not an x86-64 program"
        else:
            with tarfile.open(path) as archive:
                member = archive.getmember(f"{next(iter(roots))}/{guest_ir}")
                assert member.mode & 0o111, f"{guest_ir} must be executable"
    check_launchers(path, next(iter(roots)), contents, windows)
    shipped = {name: read_member(path, f"{next(iter(roots))}/{name}") for name in sorted(lists)}
    assert all(len(text.split()) > 1000 for text in shipped.values()), "an SDK export list is (nearly) empty"
    if not wide:
        assert shipped["sdk/exports.txt"] == shipped["sdk/exports.i386.txt"], "exports.txt is not the i386 list"
    languages = {f"languages/{name}.txt" for name in ("en-eu", "fr", "de", "it", "es")}
    assert {name for name in contents if name.startswith("languages/")} == languages, "languages/ needs the five packs"
    assert {name for name in contents if name.startswith("game/")} == {"game/README.txt"}
    assert not any(name.startswith(("saves/", "reports/", "tmp/", "mods/assets-hd/")) or
                   name in {"disc-path.txt", "settings.ini", "controls.ini"} or
                   PurePosixPath(name).suffix.lower() in {".bin", ".cue", ".iso", ".chd", ".mcr"}
                   for name in contents), "archive contains personal or disc data"
    print(f"{path.name}: layout passed ({len(contents)} files)")
    return shipped


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archives", type=Path, required=True)
    parser.add_argument("--no-guest-ir", action="store_true",
                        help="archives packed with package.py --no-guest-ir (a pull request's check, where the "
                             "pass was not built): the SDK must not have one")
    args = parser.parse_args()
    archives = sorted(args.archives.glob("*.zip")) + sorted(args.archives.glob("*.tar.gz"))
    assert archives, "no release archives found"
    sdks = {path: check(path, not args.no_guest_ir) for path in archives}
    # A target's list is one build's (tools/pc/package.py), the same in every
    # archive of one version; the i386 one is each system's own, the same in
    # both Windows archives.
    def release(path):
        return next(path.name[:len(path.name) - len(end)] for end in
                    ("-windows-x64.zip", "-windows.zip", "-linux.tar.gz", "") if path.name.endswith(end))
    for target in TARGETS:
        name = f"sdk/exports.{target}.txt"
        for version in {release(path) for path in sdks}:
            group = [path for path in sdks if release(path) == version and (target != "i386" or path.suffix == ".zip")]
            for path in group[1:]:
                assert sdks[path][name] == sdks[group[0]][name], \
                    f"{name} differs between {group[0].name} and {path.name}"

if __name__ == "__main__":
    main()
