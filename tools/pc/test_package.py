#!/usr/bin/env python3
"""Check release archives without a ROM: layout, modes, no user data."""
import argparse
import struct
from pathlib import Path, PurePosixPath
import tarfile
import zipfile

# build_mod.py's targets: each has its export list in every SDK.
TARGETS = ("i386", "x86_64-windows", "aarch64")


def read_member(path, name):
    if path.suffix == ".zip":
        with zipfile.ZipFile(path) as archive:
            return archive.read(name).decode()
    with tarfile.open(path) as archive:
        return archive.extractfile(name).read().decode()


def check_exe(image):
    """The shipped .exe: stripped (tools/pc/package.py strip) and with a
    resource section (version information), since virus scanners flag data
    after the last section and programs that say nothing about themselves.
    Its PDB is named without the builder's path: Bitdefender flagged the CI
    builds, whose path was the GitHub runner's D:/a/..."""
    header = struct.unpack_from("<I", image, 0x3C)[0]
    sections, _, symbols, symbol_count, optional = struct.unpack_from("<HIIIH", image, header + 6)
    table = header + 24 + optional
    names, end = set(), 0
    for index in range(sections):
        at = table + 40 * index
        names.add(image[at:at + 8].rstrip(b"\0"))
        size, offset = struct.unpack_from("<II", image, at + 16)
        end = max(end, offset + size)
    assert symbols == 0 and symbol_count == 0, "memories-pc.exe keeps its COFF symbol table"
    assert len(image) == end, f"memories-pc.exe has {len(image) - end} bytes after its last section"
    assert b".rsrc" in names, "memories-pc.exe has no resources (version information)"
    marker = image.find(b"RSDS")
    assert marker >= 0, "memories-pc.exe has no CodeView record"
    pdb = image[marker + 24:image.index(b"\0", marker + 24)]
    assert pdb == b"memories-pc.pdb", f"memories-pc.exe names its PDB {pdb!r}, not by bare name"
    # What v0.1.4-preview.1 gained and 11 engines' models held against it
    # (notes/pc-release.md): DEP-policy imports and the executable-guest-RAM
    # test path, which a release leaves out (MEMORIES_TEST_HOOKS).
    for name in (b"SetProcessDEPPolicy", b"GetProcessDEPPolicy", b"GetSystemDEPPolicy", b"MEMORIES_TEST_EXEC_GUEST"):
        assert name not in image, f"memories-pc.exe contains {name.decode()}"
    # The PE checksum package.py fills in after the strip: ImageHlp's sum
    # of the file's 16-bit words (its own field as 0), folded, plus the length.
    field = header + 24 + 64
    stored = struct.unpack_from("<I", image, field)[0]
    padded = image[:field] + bytes(4) + image[field + 4:] + bytes(len(image) % 2)
    total = sum(struct.unpack(f"<{len(padded) // 2}H", padded))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    assert stored == total + len(image), f"memories-pc.exe's PE checksum is {stored:#x}, not {total + len(image):#x}"


def check(path):
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
    # The variable table the control channel names variables from, beside
    # the symbol table of the same build id.
    tables = {name for name in contents if name.startswith("symbols/") and name.endswith(".vars.txt")}
    assert tables, "missing symbols/<build id>.vars.txt"
    assert all(name[:-len(".vars.txt")] + ".txt" in contents for name in tables), \
        f"{sorted(tables)}: not beside the symbol table of its build"
    # Each code mod beside the game has the object for this game's target,
    # and not another target's.
    objects = [name for name in contents if name.startswith("mods/") and name.endswith(".o")]
    ours = [name for name in objects if name.endswith(".x86_64-windows.o") == wide and not name.endswith(".aarch64.o")]
    assert objects and ours == objects, f"mods/ has another target's objects: {sorted(set(objects) - set(ours))}"
    # The SDK's export lists: one per target build_mod.py builds for, so it
    # checks every object it makes (tools/pc/package.py), and for a 32-bit
    # game also exports.txt, the i386 list under the name SDKs had before.
    lists = {f"sdk/exports.{target}.txt" for target in TARGETS}
    if not wide:
        lists.add("sdk/exports.txt")
    assert lists <= contents, f"the SDK lacks export lists: {sorted(lists - contents)}"
    assert {name for name in contents if name.startswith("sdk/exports")} == lists, "the SDK has other export lists"
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
    args = parser.parse_args()
    archives = sorted(args.archives.glob("*.zip")) + sorted(args.archives.glob("*.tar.gz"))
    assert archives, "no release archives found"
    sdks = {path: check(path) for path in archives}
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
