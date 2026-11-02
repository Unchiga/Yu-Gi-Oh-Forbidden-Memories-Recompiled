"""Finding and reading the player's game files.

The editor reads two files of the retail disc: the executable SLUS_014.11
(card stats, levels, attributes, names and texts) and DATA/WA_MRG.MRG (the
fusion, equip and ritual tables and the opponents' deck and drop pools). It
takes them from a raw .bin image (what the port itself runs from), an ISO of
2048-byte sectors, or a folder holding SLUS_014.11 and DATA/WA_MRG.MRG (an
extracted disc, as the source tree's game/ folder is).

Nothing here writes: the editor never changes the disc or game/.
"""
from __future__ import annotations

import os
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

EXECUTABLE = "SLUS_014.11"
ARCHIVE = "WA_MRG.MRG"
SLUS_SIZE = 0x1D0800            # the retail executable's length
WA_SIZE = 0x2400000             # the retail archive's length (NTSC-U)
WA_LBA = 10102                  # where WA_MRG.MRG starts on the retail disc


class GameFilesError(Exception):
    pass


@dataclass
class GameFiles:
    """The two files the editor reads, and where they came from."""
    slus: bytes
    wa: bytes
    source: str
    wa_lba: int = WA_LBA


class DiscImage:
    """A CD image of 2352-byte raw sectors or 2048-byte ones, read through
    its ISO 9660 directories."""

    def __init__(self, path):
        self.path = Path(path)
        self.file = open(self.path, "rb")
        size = os.fstat(self.file.fileno()).st_size
        for sector, skip in ((2352, 24), (2352, 16), (2048, 0)):
            if size % sector or size < 17 * sector:
                continue
            self.sector, self.skip = sector, skip
            head = self.read_sector(16)
            if head[1:6] == b"CD001":
                break
        else:
            self.file.close()
            raise GameFilesError(f"{self.path.name} is not a CD image this editor can read "
                                 "(a raw .bin of 2352-byte sectors, or an ISO)")

    def close(self):
        self.file.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def read_sector(self, lba: int) -> bytes:
        self.file.seek(lba * self.sector + self.skip)
        data = self.file.read(2048)
        if len(data) != 2048:
            raise GameFilesError(f"{self.path.name} ends at sector {lba}")
        return data

    def read(self, lba: int, size: int) -> bytes:
        out = bytearray()
        for i in range((size + 2047) // 2048):
            out += self.read_sector(lba + i)
        return bytes(out[:size])

    def _directory(self, lba: int, length: int):
        data = self.read(lba, length)
        at = 0
        while at < len(data):
            record = data[at]
            if not record:
                at = (at // 2048 + 1) * 2048
                continue
            name = data[at + 33:at + 33 + data[at + 32]].decode("ascii", "replace")
            start, size = struct.unpack_from("<I", data, at + 2)[0], struct.unpack_from("<I", data, at + 10)[0]
            flags = data[at + 25]
            yield name.split(";")[0].upper(), start, size, bool(flags & 2)
            at += record

    def find(self, path: str):
        """(lba, size) of a file such as "DATA/WA_MRG.MRG", or None."""
        root = self.read_sector(16)[156:156 + 34]
        lba, length = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
        parts = [p.upper() for p in path.replace("\\", "/").split("/") if p]
        for i, part in enumerate(parts):
            for name, start, size, is_dir in self._directory(lba, length):
                if name == part:
                    if i == len(parts) - 1:
                        return start, size
                    if is_dir:
                        lba, length = start, size
                        break
            else:
                return None
        return None


def _read_disc(path: Path) -> GameFiles:
    with DiscImage(path) as disc:
        slus = disc.find(EXECUTABLE)
        wa = disc.find("DATA/" + ARCHIVE)
        if not slus or not wa:
            raise GameFilesError(f"{path.name} is not the Yu-Gi-Oh! Forbidden Memories (USA, SLUS-01411) disc")
        return GameFiles(disc.read(*slus), disc.read(*wa), str(path), wa[0])


def _read_folder(folder: Path) -> GameFiles:
    slus = folder / EXECUTABLE
    wa = folder / "DATA" / ARCHIVE
    if slus.is_file() and wa.is_file():
        return GameFiles(slus.read_bytes(), wa.read_bytes(), str(folder))
    discs = sorted(p for p in folder.glob("*") if p.suffix.lower() == ".bin" and p.is_file())
    preferred = folder / "rpg-yfm.bin"
    if preferred.is_file():
        discs.insert(0, preferred)
    for disc in discs:
        try:
            return _read_disc(disc)
        except GameFilesError:
            continue
    raise GameFilesError(f"{folder} holds neither SLUS_014.11 with DATA/WA_MRG.MRG nor a .bin image of the disc")


def load(path) -> GameFiles:
    """The game files from a disc image, a folder, or SLUS_014.11 itself (with
    DATA/WA_MRG.MRG beside it)."""
    path = Path(path)
    if path.is_dir():
        return _read_folder(path)
    if not path.is_file():
        raise GameFilesError(f"{path} does not exist")
    if path.name.upper() == EXECUTABLE:
        return _read_folder(path.parent)
    return _read_disc(path)


def load_pair(slus_path, wa_path) -> GameFiles:
    """The two files named one by one (a community mod's modified copies)."""
    return GameFiles(Path(slus_path).read_bytes(), Path(wa_path).read_bytes(), f"{slus_path} + {wa_path}")


def _windows_documents():
    """Documents as the shell has it (it may be moved, to OneDrive say):
    SHGetFolderPathW(CSIDL_PERSONAL), as paths.c asks."""
    try:
        import ctypes
        buffer = ctypes.create_unicode_buffer(260)
        if ctypes.windll.shell32.SHGetFolderPathW(None, 5, None, 0, buffer) == 0 and buffer.value:
            return Path(buffer.value)
    except (AttributeError, OSError):
        pass
    return Path(os.environ.get("USERPROFILE") or ".") / "Documents"


def user_dir() -> Path:
    """The port's user directory (src/pc/platform/paths.c): MEMORIES_USER_DIR,
    else Documents/My Games on Windows, ~/Library/Application Support on macOS,
    else user/ in the program directory when portable.txt is there (portable
    mode), else Documents/My Games on Windows, ~/Library/Application Support
    on macOS, an absolute XDG_DATA_HOME or ~/.local/share."""
    named = os.environ.get("MEMORIES_USER_DIR")
    if named:
        return Path(named)
    program = program_dir()
    try:
        if (program / "portable.txt").exists():
            return program / "user"
    except OSError:
        pass
    if sys.platform == "win32":
        return _windows_documents() / "My Games" / "YFM Re-Decomp"
    if sys.platform == "darwin":
        return Path(os.path.expanduser("~")) / "Library" / "Application Support" / "YFM Re-Decomp"
    xdg = os.environ.get("XDG_DATA_HOME", "")
    base = xdg if xdg.startswith("/") else os.path.join(os.path.expanduser("~"), ".local", "share")
    return Path(base) / "YFM Re-Decomp"


def program_dir() -> Path:
    """The folder the program lives in, which is where the game's files sit
    beside it. A packed program is its executable's folder, except in a macOS
    app (X.app/Contents/MacOS/exe), where it is the folder holding X.app: the
    app's inside is no place a player puts game/ or mods/. From the source it
    is the script's folder, which is not where a game is: MEMORIES_PROGRAM_DIR
    names it there (paths.c Paths_ProgramDir, which it wins in the game too)."""
    named = os.environ.get("MEMORIES_PROGRAM_DIR")
    if named and len(named.encode("utf-8", "surrogateescape")) < 1024:
        return Path(named)
    if not getattr(sys, "frozen", False):
        return Path(sys.argv[0] or ".").resolve().parent
    folder = Path(sys.executable).resolve().parent
    if (sys.platform == "darwin" and folder.name == "MacOS" and folder.parent.name == "Contents"
            and folder.parent.parent.suffix == ".app"):
        return folder.parent.parent.parent
    return folder


def candidates(extra=()) -> list:
    """Where to look for the game, in the order the port does: MEMORIES_DISC,
    the disc the port was last pointed at, game/ beside the program, the
    program's folder, game/ in the user directory, and ./game."""
    found = [Path(p) for p in extra]
    named = os.environ.get("MEMORIES_DISC")
    if named:
        found.append(Path(named))
    saved = user_dir() / "disc-path.txt"
    try:
        text = saved.read_text(encoding="utf-8", errors="replace").strip()
        if text:
            found.append(Path(text))
    except OSError:
        pass
    program = program_dir()
    found += [program / "game", program, user_dir() / "game", Path.cwd() / "game"]
    here = Path(__file__).resolve()
    if len(here.parents) > 3:
        found.append(here.parents[3] / "game")      # the source tree's game/
    return found


def find_game(extra=()):
    for place in candidates(extra):
        try:
            if place.exists():
                return load(place)
        except (GameFilesError, OSError):
            continue
    return None
