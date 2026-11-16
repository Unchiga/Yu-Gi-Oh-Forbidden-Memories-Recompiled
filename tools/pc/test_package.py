#!/usr/bin/env python3
"""Check release archives without a ROM: layout, modes, no user data."""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath
import tarfile
import zipfile

from pe_layout import link_layout_problems, pe_layout_gaps

# build_mod.py's targets: each has its export list in every SDK.
TARGETS = ("i386", "x86_64-windows", "aarch64-translated")
# The MIPS interpreter (src/pc/guest/mips.c) is in no shipped game, only in
# the reference build for tests (tools/pc/build_game32.py --reference): a
# package whose tables name its functions, or whose executable holds the
# title of its crash report, was packed from one. By bytes, not nm: the
# shipped executable is stripped.
INTERPRETER_NAME = "Memories_Mips"
INTERPRETER_MARKER = b"MIPS overlay"   # tools/pc/reference_build.py's MARKER


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
    # Sections adjacent from the headers on: with a hole Windows does not
    # load it at all (pe_layout.py; master's packages after the model
    # modules, whose empty sections grew lld's headers).
    gaps = pe_layout_gaps(image)
    assert not gaps, f"{label} has a hole in its image, which Windows will not load: {'; '.join(gaps)}"
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


def check_layout_guard():
    """pe_layout.py on images made here: sections adjacent from the headers
    on pass; a stripped master build's shape (.text at 0x6000 behind 0x400
    bytes of headers), a hole between two sections, a SectionAlignment of 0
    or below a page do not; a VirtualSize of 0 counts SizeOfRawData; and a
    link whose headers are over one page is refused before any strip."""
    def image(text, data, alignment=0x1000, headers=0x400, sizes=(0x1800, 0x1800), raw=0):
        made = bytearray(0x400)
        made[:2] = b"MZ"
        struct.pack_into("<I", made, 0x3C, 0x80)
        made[0x80:0x84] = b"PE\0\0"
        struct.pack_into("<HH", made, 0x84, 0x14C, 2)                  # i386, two sections
        struct.pack_into("<H", made, 0x94, 0xE0)                       # SizeOfOptionalHeader
        struct.pack_into("<H", made, 0x98, 0x10B)                      # PE32
        struct.pack_into("<II", made, 0x98 + 32, alignment, 0x200)     # SectionAlignment, FileAlignment
        struct.pack_into("<I", made, 0x98 + 60, headers)               # SizeOfHeaders
        for index, (name, address, size) in enumerate(((b".text", text, sizes[0]), (b".data", data, sizes[1]))):
            at = 0x98 + 0xE0 + 40 * index
            made[at:at + 8] = name.ljust(8, b"\0")
            struct.pack_into("<III", made, at + 8, size, address, raw)  # VirtualSize, VirtualAddress, SizeOfRawData
        return bytes(made)
    assert not pe_layout_gaps(image(0x1000, 0x3000)), "pe_layout_gaps refuses adjacent sections"
    assert pe_layout_gaps(image(0x6000, 0x8000)), "pe_layout_gaps passes a hole after the headers"
    assert pe_layout_gaps(image(0x1000, 0x4000)), "pe_layout_gaps passes a hole between sections"
    assert pe_layout_gaps(image(0x1000, 0x3000, alignment=0)), "pe_layout_gaps passes a SectionAlignment of 0"
    assert pe_layout_gaps(image(0x200, 0x1A00, alignment=0x200)), "pe_layout_gaps passes an alignment below a page"
    # VirtualSize 0: the section takes SizeOfRawData (0x1800), so .data follows at 0x3000.
    assert not pe_layout_gaps(image(0x1000, 0x3000, sizes=(0, 0x1800), raw=0x1800)), \
        "pe_layout_gaps does not take SizeOfRawData for a VirtualSize of 0"
    assert pe_layout_gaps(image(0x1000, 0x2000, sizes=(0, 0x1800), raw=0x1800)), \
        "pe_layout_gaps lets .data overlap a section of VirtualSize 0"
    # The link: adjacent, headers within one page; and the master link's
    # shape (0x5200 bytes of headers, .text at 0x6000), adjacent until stripped.
    assert not link_layout_problems(image(0x1000, 0x3000)), "link_layout_problems refuses a good link"
    assert not pe_layout_gaps(image(0x6000, 0x8000, headers=0x5200)), "pe_layout_gaps refuses the unstripped master link"
    assert link_layout_problems(image(0x6000, 0x8000, headers=0x5200)), \
        "link_layout_problems passes headers over one page"


def interpreter_problems(executable, tables):
    """Why a game (its executable's bytes; {name: text} of its symbols/*.txt
    and sdk/exports*.txt) has the MIPS interpreter, or [] for none."""
    problems = []
    if INTERPRETER_MARKER in executable:
        problems.append(f"the executable holds {INTERPRETER_MARKER.decode()!r}, the interpreter's crash report")
    for name, text in sorted(tables.items()):
        found = sorted({word for word in text.split() if word.startswith(INTERPRETER_NAME)})
        if found:
            problems.append(f"{name} names {', '.join(found[:3])}{' and more' if len(found) > 3 else ''}")
    return problems


def check_interpreter_guard():
    """interpreter_problems on made-up games: it passes a game without the
    interpreter and refuses each sign of one."""
    clean = b"MZ...memories-pc: call into guest code at 0x%08x, which has no native function"
    tables = {"symbols/0123abcd.txt": "00401000 00000010 Memories_Main\n",
              "sdk/exports.i386.txt": "Memories_Main\nModel_QueueTintRequest\n"}
    assert not interpreter_problems(clean, tables), "interpreter_problems refuses a game without the interpreter"
    assert interpreter_problems(clean + b"MIPS overlay\0", tables), "interpreter_problems passes mips.c's string"
    for name, line in (("symbols/0123abcd.txt", "00402000 00000040 Memories_MipsTry\n"),
                       ("sdk/exports.txt", "Memories_MipsThunkTarget\n")):
        assert interpreter_problems(clean, {**tables, name: tables.get(name, "") + line}), \
            f"interpreter_problems passes {line.split()[-1]} in {name}"


def check_build_folder(folder):
    """The guard on a build folder (its executable, symbols/ and sdk/), for
    telling a reference build from a game: the problems, or []."""
    folder = Path(folder)
    executable = next((folder / name for name in ("memories-pc.exe", "memories-pc") if (folder / name).is_file()), None)
    assert executable, f"{folder}: no memories-pc(.exe)"
    # The build's own symbol tables, as package.py ships them: a build folder
    # keeps its earlier builds' too, for carrying save states.
    build_id = (folder / "buildid").read_text().strip()
    tables = {str(path.relative_to(folder).as_posix()): path.read_text(errors="replace")
              for pattern in (f"symbols/{build_id}.txt", f"symbols/{build_id}.vars.txt", "sdk/exports*.txt")
              for path in folder.glob(pattern)}
    return interpreter_problems(executable.read_bytes(), tables)


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


# Where an SDK keeps what the repository calls tools/pc, config/pc and src (tools/pc/model_layout.py).
SDK_FOLDERS = {"src/": "sdk/include/", "config/pc/": "sdk/config/", "tools/pc/": "sdk/tools/"}


def sdk_files(path, root):
    """{name inside the archive's top folder: bytes} of the SDK's tools/,
    config/ and include/, in one pass: a .tar.gz is decompressed from its start
    for every member opened by name (about 1,700 of them here), so it is read
    once."""
    prefixes = tuple(f"{root}/sdk/{folder}/" for folder in ("tools", "config", "include"))
    found = {}
    if path.suffix == ".zip":
        with zipfile.ZipFile(path) as archive:
            for info in archive.infolist():
                if info.filename.startswith(prefixes) and not info.is_dir() and "memories-guest-ir" not in info.filename:
                    found[info.filename[len(root) + 1:]] = archive.read(info)
    else:
        with tarfile.open(path) as archive:
            for member in archive:
                if member.isfile() and member.name.startswith(prefixes) and "memories-guest-ir" not in member.name:
                    found[member.name[len(root) + 1:]] = archive.extractfile(member).read()
    return found


def check_model_tool(path, root, contents):
    """The model-migration tool in the SDK (tools/pc/model_sdk.py): its Python,
    its tables and the C of every native row are there, each as the stamp
    says, and `model_gate.py mod-check` passes from the SDK unpacked on its
    own, outside any checkout. Whether the span-word labels were current when
    it was packed is told (a warning), not asserted: stale labels block nothing.
    Nothing here reads a disc: the C is the decomp's own, the tables hold
    offsets, symbols, hashes and labels."""
    blobs = sdk_files(path, root)
    assert "sdk/config/model_sdk.json" in blobs, "the SDK has no sdk/config/model_sdk.json (its model tool's stamp)"
    stamp = json.loads(blobs["sdk/config/model_sdk.json"])
    for problem in stamp["labels"]["problems"][:1]:
        print(f"{path.name}: WARNING: the SDK's span-word labels were stale when it was made: {problem}",
              file=sys.stderr)
    assert stamp["native_rows"] > 300, f"the SDK's tool knows {stamp['native_rows']} native rows"
    shipped = {}
    for name, digest in stamp["files"].items():
        folder = next((folder for folder in SDK_FOLDERS if name.startswith(folder)), None)
        assert folder, f"the stamp names {name}, which has no place in the SDK"
        shipped[name] = SDK_FOLDERS[folder] + name[len(folder):]
        assert shipped[name] in blobs, f"the SDK lacks {shipped[name]}, which its model tool needs"
        found = hashlib.sha256(blobs[shipped[name]]).hexdigest()
        assert found == digest, f"{shipped[name]} is not the file the SDK was stamped with"
    tools = {name for name in shipped.values() if name.startswith("sdk/tools/")}
    assert {"sdk/tools/model_gate.py", "sdk/tools/model_migrate.py", "sdk/tools/model_oracle.py",
            "sdk/tools/model_layout.py", "sdk/tools/yfm_control.py"} <= tools, "the SDK lacks a file of its model tool"
    modules = [name for name in shipped.values() if name.startswith("sdk/include/native_overlays/") and name.endswith(".c")]
    assert len(modules) > 1000, f"the SDK has only {len(modules)} of the native modules' C files"
    assert not any(PurePosixPath(name).suffix.lower() in {".bin", ".cue", ".iso", ".mrg"} for name in shipped.values())
    with tempfile.TemporaryDirectory(prefix="model-tool-") as work:
        release = Path(work) / "release"
        for name, data in blobs.items():   # the SDK's headers (include/) are most of what the C includes
            target = release / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        elsewhere = Path(work) / "elsewhere"
        elsewhere.mkdir()
        environment = dict(os.environ, PYTHONIOENCODING="utf-8")
        done = subprocess.run([sys.executable, str(release / "sdk/tools/model_gate.py"), "mod-check"], cwd=elsewhere,
                              env=environment, capture_output=True, text=True, encoding="utf-8", errors="replace")
        said = done.stdout + done.stderr
        assert done.returncode == 0 and "(the SDK's)" in done.stdout, \
            f"mod-check from the unpacked SDK failed: {said}"
        print(f"{path.name}: {done.stdout.strip()}")


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
    # The variable table the control channel names variables from, beside
    # the symbol table of the same build id.
    tables = {name for name in contents if name.startswith("symbols/") and name.endswith(".vars.txt")}
    assert tables, "missing symbols/<build id>.vars.txt"
    assert all(name[:-len(".vars.txt")] + ".txt" in contents for name in tables), \
        f"{sorted(tables)}: not beside the symbol table of its build"
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
    check_model_tool(path, next(iter(roots)), contents)
    shipped = {name: read_member(path, f"{next(iter(roots))}/{name}") for name in sorted(lists)}
    assert all(len(text.split()) > 1000 for text in shipped.values()), "an SDK export list is (nearly) empty"
    # No MIPS interpreter: the game was not packed from a reference build.
    executable = read_bytes(path, f"{next(iter(roots))}/{'memories-pc.exe' if windows else 'memories-pc'}")
    symbols = {name: read_member(path, f"{next(iter(roots))}/{name}") for name in sorted(contents)
               if name.startswith("symbols/") and name.endswith(".txt")}
    problems = interpreter_problems(executable, {**shipped, **symbols})
    assert not problems, (f"{path.name} has the MIPS interpreter, which only a reference build may have: "
                          + "; ".join(problems))
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
    parser.add_argument("--archives", type=Path)
    parser.add_argument("--build-folder", type=Path, action="append", default=[],
                        help="only the MIPS interpreter guard, on a build folder (exit 1 if it has the interpreter)")
    parser.add_argument("--no-guest-ir", action="store_true",
                        help="archives packed with package.py --no-guest-ir (a pull request's check, where the "
                             "pass was not built): the SDK must not have one")
    args = parser.parse_args()
    check_layout_guard()
    check_interpreter_guard()
    if args.build_folder:
        found = {folder: check_build_folder(folder) for folder in args.build_folder}
        for folder, problems in found.items():
            print(f"{folder}: {'; '.join(problems) if problems else 'no MIPS interpreter'}")
        sys.exit(1 if any(found.values()) else 0)
    if not args.archives:
        parser.error("--archives (or --build-folder)")
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
