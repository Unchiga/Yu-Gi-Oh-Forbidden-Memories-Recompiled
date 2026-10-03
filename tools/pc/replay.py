#!/usr/bin/env python3
"""Replays: a run of the game kept as a file, played again and checked
(notes/agent-control.md, step 4).

A replay is a folder (tests/pc/replays/<name>/, text that diffs) or the same
files in a zip (.yfmreplay, to pass around):

    replay.json    {"format": 1, "kind": "recorded" | "scripted", "header": {...},
                    "start": {"kind": "boot"} | {"kind": "state", "file": "start.state"},
                    "recording": "recording.txt", "script": "scenario.py"}
    recording.txt  recorded: MEMORIES_RECORD's lines (src/pc/debug/recorder.h): the
                   pad bits per VBlank, a frame hash at every safe point (H), save
                   states (S) and the end (E)
    scenario.py    scripted: run(game, out) on tools/pc/yfm_control.py; its
                   assertions are the verdict, so it survives timing changes
    *.state        a start state and state checkpoints (never in tests/pc/replays:
                   a state holds the game's RAM, which is the disc's data)

The header holds the build id, the OS, the settings and the mods the run had
(the game's own facts, the recording's F lines) and the language.

    replay.py record OUT --session FILE.py   run FILE.py's run(game, out) on the
                                             client, recording; a recorded replay
    replay.py record OUT --recording REC     package a MEMORIES_RECORD file
    replay.py record OUT --scripted FILE.py  a scripted replay of FILE.py
    replay.py play FILE [--check] [--update] play it; --check reports the first
                                             VBlank whose frame differs (and, with
                                             state checkpoints, the first RAM range)
    replay.py run [DIR]                      every replay in tests/pc/replays with
                                             --check, like smoke.py: a local gate
                                             (CI has no disc)
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from smoke import launcher  # noqa: E402
import yfm_control  # noqa: E402


def report(message: str, *, file=None) -> None:
    """Keep diagnostics usable with Unicode paths on legacy Windows consoles."""
    stream = sys.stdout if file is None else file
    encoding = getattr(stream, "encoding", None) or "utf-8"
    print(message.encode(encoding, errors="backslashreplace").decode(encoding), file=stream)


ROOT = Path(__file__).resolve().parents[2]
REPLAYS = ROOT / "tests/pc/replays"
OUTPUT = ROOT / "tmp/pc/replays"
GUEST_RAM, RAM_SIZE, SCRATCHPAD = 0x80000000, 0x200000, 0x1F800000
# RAM a deterministic run does not hold the same from run to run: the sound
# driver's work area (g_SDValue), which the mixer thread updates in real time.
MASKS = [(0x801E0384, 0x801E1B44)]


# The container: a folder or a zip of the same files.

class Replay:
    def __init__(self, path: Path):
        self.path = Path(path)
        self.folder = self.path if self.path.is_dir() else Path(tempfile.mkdtemp(prefix="replay-"))
        if not self.path.is_dir():
            with zipfile.ZipFile(self.path) as archive:
                archive.extractall(self.folder)
        self.meta = json.loads((self.folder / "replay.json").read_text(encoding="utf-8"))
        if self.meta.get("format") != 1 or self.meta.get("kind") not in ("recorded", "scripted"):
            raise SystemExit(f"{path}: not a replay this tool reads")

    @property
    def name(self) -> str:
        return self.path.stem if self.path.suffix == ".yfmreplay" else self.path.name

    def file(self, name: str) -> Path:
        return self.folder / name


def write_replay(out: Path, meta: dict, files: dict[str, Path | str]) -> None:
    """`files`: name in the replay -> a path to copy, or text."""
    public = out.resolve().is_relative_to(REPLAYS.resolve())
    states = [name for name in files if name.endswith(".state")]
    if public and states:
        raise SystemExit(f"{out}: a replay in tests/pc/replays carries no save state (it holds the game's RAM); "
                         "record from boot, or keep it outside the repository")
    # Read first: a source may sit in the folder about to be replaced.
    files = {name: source.read_bytes() if isinstance(source, Path) else source.encode()
             for name, source in files.items()}
    out_dir = out if out.suffix != ".yfmreplay" else Path(tempfile.mkdtemp(prefix="replay-"))
    out_dir.mkdir(parents=True, exist_ok=True)
    for stale in [*out_dir.glob("*.state"), out_dir / "recording.txt", out_dir / "scenario.py"]:
        stale.unlink(missing_ok=True)   # what else is there (a session.py) stays
    (out_dir / "replay.json").write_text(json.dumps(meta, indent=1) + "\n", encoding="utf-8")
    for name, data in files.items():
        (out_dir / name).write_bytes(data)
    if out.suffix == ".yfmreplay":
        out.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as archive:
            for item in sorted(out_dir.iterdir()):
                archive.write(item, item.name)
        shutil.rmtree(out_dir, ignore_errors=True)
    report(f"replay: wrote {out}")


# Recordings (recorder.h's lines).

def parse(path: Path) -> dict:
    result = {"I": [], "H": {}, "S": {}, "F": {}, "E": None, "start": "boot", "build": None}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        kind, _, rest = line.partition(" ")
        if kind == "I":
            result["I"].append(line)
        elif kind == "H":
            index, frame, value = rest.split()
            result["H"][(int(index), int(frame))] = value
        elif kind == "S":
            index, frame, state = rest.split(" ", 2)
            result["S"][int(index)] = (int(frame), state)
        elif kind == "F":
            key, _, value = rest.partition(": ")
            result["F"][key] = value
        elif kind == "E":
            result["E"] = tuple(int(x) for x in rest.split())
        elif kind == "start":
            result["start"] = rest
        elif kind == "build":
            result["build"] = rest
    return result


def header_from(recording: dict, extra_settings: dict | None = None) -> dict:
    settings = dict(item.split("=", 1) for item in recording["F"].get("settings", "").split() if "=" in item)
    settings.update({key: str(value) for key, value in (extra_settings or {}).items()})
    mods = recording["F"].get("mods", "none")
    return {"build": recording["build"], "clock": recording["F"].get("clock", "virtual"),
            "os": recording["F"].get("os", sys.platform), "recorded_on": sys.platform,
            "commit": recording["F"].get("build", ""), "settings": settings,
            "mods": [] if mods == "none" else mods.split(), "language": settings.get("language", "0")}


def thin(lines: list[str], every: int) -> list[str]:
    """Keep every `every`-th H line (by order); I, S, F and the rest stay."""
    kept, count = [], 0
    for line in lines:
        if line.startswith("H "):
            count += 1
            if (count - 1) % every:
                continue
        kept.append(line)
    return kept


def package_recording(out: Path, recording_path: Path, start_state: Path | None, hash_every: int,
                      settings: dict | None, notes: str = "") -> None:
    recording = parse(recording_path)
    if recording["E"] is None:
        raise SystemExit(f"{recording_path}: no E line (the game did not end cleanly); quit it, don't kill it")
    if recording["F"].get("clock") == "real":
        report(f"replay: WARNING: {recording_path} ran on the real-time clock: it will not play back the same "
              "(record with MEMORIES_DETERMINISTIC=1; notes/agent-control.md, step 3)", file=sys.stderr)
    lines = thin(recording_path.read_text(encoding="utf-8").splitlines(), max(hash_every, 1))
    files: dict[str, Path | str] = {}
    states = []
    for index, (frame, state) in sorted(recording["S"].items()):
        name = f"checkpoint-{index}.state"
        files[name] = Path(state)
        states.append({"index": index, "frame": frame, "file": name})
    lines = [line if not line.startswith("S ") else
             f"S {line.split()[1]} {line.split()[2]} checkpoint-{line.split()[1]}.state" for line in lines]
    files["recording.txt"] = "\n".join(lines) + "\n"
    start = {"kind": "boot"}
    if start_state:
        files["start.state"] = start_state
        start = {"kind": "state", "file": "start.state"}
    every = sorted(states[i + 1]["index"] - states[i]["index"] for i in range(len(states) - 1))
    meta = {"format": 1, "kind": "recorded", "header": header_from(recording, settings), "start": start,
            "recording": "recording.txt", "states_every": every[0] if every else 0, "notes": notes}
    write_replay(out, meta, files)


# Running the game.

def environment(settings_file: Path, user: Path, extra: dict[str, str]) -> dict[str, str]:
    env = {key: value for key, value in os.environ.items() if not key.startswith("MEMORIES_")}
    if os.environ.get("MEMORIES_DISC"):
        env["MEMORIES_DISC"] = os.environ["MEMORIES_DISC"]
    env.update(MEMORIES_HEADLESS="1", MEMORIES_DETERMINISTIC="1", MEMORIES_NO_AUDIO="1", MEMORIES_NO_GAMEPAD="1",
               MEMORIES_NO_UPDATE_CHECK="1", MEMORIES_WATCHDOG="0", MEMORIES_SHOW_HUD="0",
               MEMORIES_SETTINGS=str(settings_file), MEMORIES_USER_DIR=str(user))
    env.update(extra)
    return env


def settings_text(header: dict, executable: Path) -> str:
    """The recorded settings, and each mod beside the executable on or off as
    the recording had it."""
    settings = dict(header.get("settings", {}))
    for manifest in sorted((executable.parent / "mods").glob("*/mod.json")):
        settings[f"mod.{manifest.parent.name}"] = "1" if manifest.parent.name in header.get("mods", []) else "0"
    return "".join(f"{key}={value}\n" for key, value in settings.items())


def play_recorded(replay: Replay, executable: Path, out: Path, timeout: float,
                  more_env: dict[str, str] | None = None) -> Path | None:
    out.mkdir(parents=True, exist_ok=True)
    (out / "user").mkdir(exist_ok=True)
    settings = out / "settings.txt"
    settings.write_text(settings_text(replay.meta["header"], executable), encoding="utf-8")
    actual = out / "actual.txt"
    extra = {"MEMORIES_PLAY": str(replay.file(replay.meta["recording"])), "MEMORIES_RECORD": str(actual)}
    if replay.meta["start"]["kind"] == "state":
        extra["MEMORIES_LOAD_STATE"] = str(replay.file(replay.meta["start"]["file"]))
    if replay.meta.get("states_every"):
        extra["MEMORIES_RECORD_STATES"] = str(replay.meta["states_every"])
    extra.update(more_env or {})
    command, wine = launcher(executable)
    started = time.monotonic()
    with (out / "game.log").open("wb") as log:
        try:
            code = subprocess.run(command, cwd=ROOT, env={**environment(settings, out / "user", extra), **wine},
                                  stdout=log, stderr=subprocess.STDOUT, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
    report(f"replay: {replay.name}: played in {time.monotonic() - started:.0f} s (exit {code})")
    if code != 0:
        report(f"replay: {replay.name}: FAILED: game exit {code}; see {out / 'game.log'}")
        return None
    if not actual.is_file():
        report(f"replay: {replay.name}: FAILED: the game wrote no recording; see {out / 'game.log'}")
        return None
    return actual


def memory(path: Path) -> bytes:
    data = path.read_bytes()
    at = 16
    while at + 20 <= len(data):
        tag = data[at:at + 16].split(b"\0")[0]
        size, = struct.unpack_from("<I", data, at + 16)
        if tag == b"memory":
            return data[at + 20:at + 20 + size]
        at += 20 + size
    raise SystemExit(f"{path}: no memory chunk")


def first_ram_difference(expected: Path, actual: Path) -> str | None:
    """The first differing range of guest RAM and the scratchpad, with the
    masks applied; None when they agree."""
    a, b = bytearray(memory(expected)), bytearray(memory(actual))
    for low, high in MASKS:
        a[low - GUEST_RAM:high - GUEST_RAM] = b[low - GUEST_RAM:high - GUEST_RAM] = bytes(high - low)
    for at in range(0, min(len(a), len(b)), 4096):
        if a[at:at + 4096] != b[at:at + 4096]:
            start = next(i for i in range(at, at + 4096) if a[i] != b[i])
            end = start
            while end < len(a) and (a[end] != b[end] or a[end + 1:end + 16] != b[end + 1:end + 16]):
                end += 1
            base = GUEST_RAM if start < RAM_SIZE else SCRATCHPAD - RAM_SIZE
            count = sum(1 for i in range(len(a)) if a[i] != b[i])
            return f"0x{base + start:08X}..0x{base + end:08X} ({count} bytes differ in all)"
    return None


def completed(expected: dict, actual: dict, name: str) -> bool:
    """Require a clean, complete run even when hashes are thinned or updated."""
    if expected["E"] is None or actual["E"] is None:
        which = "expected recording" if expected["E"] is None else "playback"
        report(f"replay: {name}: FAILED: {which} has no end marker")
        return False
    if any(got < wanted for got, wanted in zip(actual["E"], expected["E"])):
        report(f"replay: {name}: ended at VBlank {actual['E'][0]} (frame {actual['E'][1]}), the recording "
              f"at {expected['E'][0]} ({expected['E'][1]})")
        return False
    return True


def check(replay: Replay, actual_path: Path) -> bool:
    expected = parse(replay.file(replay.meta["recording"]))
    actual = parse(actual_path)
    if not completed(expected, actual, replay.name):
        return False
    ok = True
    # A game closed while it waited for a VBlank presents its last frame
    # without one (the wait ends on the quit): the frames at the recording's
    # last VBlank are looked up by frame alone, as the play reaches them a
    # VBlank later.
    by_frame = {frame: value for (_, frame), value in actual["H"].items()}
    last = expected["E"][0] if expected["E"] else None
    for (index, frame), value in sorted(expected["H"].items()):
        got = actual["H"].get((index, frame)) or (by_frame.get(frame) if index == last else None)
        if got != value:
            report(f"replay: {replay.name}: FIRST DIFFERENCE at VBlank {index} (frame {frame} when recorded): "
                  f"expected {value}, got {got or 'no such frame (the run ended or took another path)'}")
            ok = False
            break
    else:
        report(f"replay: {replay.name}: {len(expected['H'])} frames as recorded")
    for index, (frame, name) in sorted(expected["S"].items()):
        mine = actual["S"].get(index)
        if not mine:
            report(f"replay: {replay.name}: no state taken at VBlank {index} in the play")
            ok = False
            continue
        difference = first_ram_difference(replay.file(Path(name).name), Path(mine[1]))
        if difference:
            report(f"replay: {replay.name}: RAM at the state checkpoint of VBlank {index} differs first at "
                  f"{difference} (g_SDValue masked)")
            ok = False
            break
    return ok


def play_scripted(replay: Replay, executable: Path, out: Path) -> bool:
    spec = importlib.util.spec_from_file_location("scenario", replay.file(replay.meta["script"]))
    scenario = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(scenario)
    out.mkdir(parents=True, exist_ok=True)
    try:
        scenario.run(executable, out)
    except AssertionError as failure:
        report(f"replay: {replay.name}: FAILED: {failure}")
        return False
    except Exception as failure:   # the scenario could not run on this build
        report(f"replay: {replay.name}: FAILED to run: {type(failure).__name__}: {failure}")
        return False
    report(f"replay: {replay.name}: passed")
    return True


def play(path: Path, executable: Path, do_check: bool, update: bool, timeout: float,
         more_env: dict[str, str] | None = None, keep: bool = False) -> bool:
    """Play a replay in a folder of its own under tmp/pc/replays, removed when
    it passes and kept when it fails, for a look."""
    out: list[Path] = []
    passed = False
    try:
        passed = play_in(path, executable, do_check, update, timeout, out, more_env)
    finally:
        if out and passed and not keep:
            shutil.rmtree(out[0], ignore_errors=True)
        elif out:
            report(f"replay: kept {out[0]}")
    return passed


def play_in(path: Path, executable: Path, do_check: bool, update: bool, timeout: float, folder: list,
            more_env: dict[str, str] | None = None) -> bool:
    replay = Replay(path)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix=f"{replay.name}-", dir=OUTPUT))
    folder.append(out)
    if replay.meta["kind"] == "scripted":
        return play_scripted(replay, executable, out)
    header = replay.meta["header"]
    if header.get("clock") == "real":
        report(f"replay: {replay.name}: recorded on the real-time clock, where each VBlank came at its own moment: "
              "it cannot be played back (only a run with MEMORIES_DETERMINISTIC=1 can)")
        return False
    build = (executable.parent / "buildid").read_text().strip() if (executable.parent / "buildid").exists() else "?"
    if header.get("build") and header["build"] != build:
        report(f"replay: {replay.name}: recorded by build {header['build']}, playing on {build}")
    actual = play_recorded(replay, executable, out, timeout, more_env)
    if actual is None:
        return False
    if not completed(parse(replay.file(replay.meta["recording"])), parse(actual), replay.name):
        return False
    if update:
        recording = replay.file(replay.meta["recording"])
        keep = {tuple(line.split()[1:3]) for line in recording.read_text().splitlines() if line.startswith("H ")}
        lines = [line for line in actual.read_text().splitlines()
                 if not line.startswith("H ") or tuple(line.split()[1:3]) in keep]
        if not path.is_dir():
            raise SystemExit("--update rewrites a replay folder, not a zip")
        (path / replay.meta["recording"]).write_text("\n".join(lines) + "\n", encoding="utf-8")
        report(f"replay: {replay.name}: checkpoints updated from this build")
        return True
    return check(replay, actual) if do_check else True


def record(arguments) -> int:
    settings = dict(item.split("=", 1) for item in arguments.settings or [])
    out = arguments.out.resolve()
    if arguments.scripted:
        meta = {"format": 1, "kind": "scripted", "header": {"os": sys.platform, "settings": settings},
                "start": {"kind": "boot"}, "script": "scenario.py", "notes": arguments.notes or ""}
        write_replay(out, meta, {"scenario.py": arguments.scripted})
        return 0
    if arguments.recording:
        package_recording(out, arguments.recording.resolve(), arguments.state, arguments.hash_every, settings,
                          arguments.notes or "")
        return 0
    # A client session, recorded.
    OUTPUT.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="record-", dir=OUTPUT))
    spec = importlib.util.spec_from_file_location("session", arguments.session)
    session = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(session)
    env = {"MEMORIES_RECORD": str(work / "recording.txt")}
    if arguments.state:
        env["MEMORIES_LOAD_STATE"] = str(arguments.state.resolve())
    if arguments.states_every:
        env["MEMORIES_RECORD_STATES"] = str(arguments.states_every)
    with yfm_control.Game(arguments.executable, out=work / "game", settings=settings, env=env) as game:
        session.run(game, work)
    package_recording(out, work / "recording.txt", arguments.state, arguments.hash_every, settings,
                      arguments.notes or "")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    making = commands.add_parser("record", help="make a replay")
    making.add_argument("out", type=Path, help="a folder, or a .yfmreplay zip")
    source = making.add_mutually_exclusive_group(required=True)
    source.add_argument("--session", type=Path, help="a Python file with run(game, out), recorded")
    source.add_argument("--recording", type=Path, help="a MEMORIES_RECORD file")
    source.add_argument("--scripted", type=Path, help="a Python file with run(executable, out)")
    making.add_argument("--state", type=Path, help="the state the recording starts at")
    making.add_argument("--states-every", type=int, help="a state checkpoint every N VBlanks")
    making.add_argument("--hash-every", type=int, default=1, help="keep every Nth frame hash")
    making.add_argument("--settings", nargs="*", help="key=value settings of the run (the session's)")
    making.add_argument("--notes", help="what the replay is for")
    making.add_argument("--executable", type=Path)
    playing = commands.add_parser("play", help="play a replay")
    playing.add_argument("replay", type=Path)
    playing.add_argument("--check", action="store_true")
    playing.add_argument("--update", action="store_true", help="take this build's frame hashes as the expected")
    playing.add_argument("--executable", type=Path)
    playing.add_argument("--timeout", type=float, default=900)
    playing.add_argument("--env", nargs="*", default=[], help="KEY=VALUE variables for the game (MEMORIES_TRACE...)")
    playing.add_argument("--keep", action="store_true", help="keep the play's folder (its log) when it passes")
    running = commands.add_parser("run", help="every replay in a folder, checked")
    running.add_argument("folder", type=Path, nargs="?", default=REPLAYS)
    running.add_argument("--executable", type=Path)
    running.add_argument("--timeout", type=float, default=900)
    arguments = parser.parse_args()
    executable = Path(arguments.executable or os.environ.get("YFM_EXECUTABLE") or yfm_control.EXECUTABLE).resolve()
    if arguments.command == "record":
        arguments.executable = executable
        return record(arguments)
    if arguments.command == "play":
        more_env = dict(item.split("=", 1) for item in arguments.env)
        return 0 if play(arguments.replay, executable, arguments.check, arguments.update, arguments.timeout,
                         more_env, arguments.keep) else 1
    replays = sorted(path for path in arguments.folder.iterdir()
                     if (path / "replay.json").exists() or path.suffix == ".yfmreplay")
    failed = [path.name for path in replays if not play(path, executable, True, False, arguments.timeout)]
    report(f"replay: {len(replays) - len(failed)} of {len(replays)} passed" + (f"; failed: {', '.join(failed)}"
                                                                               if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
