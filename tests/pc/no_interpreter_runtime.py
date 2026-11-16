#!/usr/bin/env python3
"""The game without the MIPS interpreter, end to end on the disc (MEMORIES_DISC
or --disc; skipped without one): each place that used to interpret does what
notes/pc-build.md ("The reference build") says, and says so once.

  settings  MEMORIES_MODEL_MODULES=interpreter, MEMORIES_DUEL_EFFECTS=
            interpreter, MEMORIES_CREDITS=interpreter and
            MEMORIES_TRACE_MIPS_PRINTF: each said once on stderr, and the
            default runs (an animated battle's modules run native C)
  model     a data mod whose variant module's code is changed: the stand-in
            plays it, said once naming the mod and model_gate.py mod-migrate,
            and the battle ends
  effects   a data mod that changes the duel effect bank (every copy in
            WA_MRG.MRG): a field magic's effect (Forest: its card's use waits
            for the effect's hand-off) completes at once, said once, and the
            duel goes on to the next turn
  credits   a data mod that changes the credits module (SU.MRG): the credits
            are skipped (no roll; the ending's scene runs on until its music
            ends, as with the roll), said once, and the presentation is
            complete: the game goes on to its restart
  overlay   a native call into an overlay window with no native function
            (MEMORIES_CRASH_TEST=overlay): the "no native function" report,
            and the game stops; it never falls through

With --reference the cases also run on the reference build (the game with the
interpreter: tools/pc/reference_build.py, built when it is missing), where
the changed effect bank, credits and module are interpreted and nothing says
the interpreter is missing.

    python tests/pc/no_interpreter_runtime.py [--executable PATH] [--reference] [cases...]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/pc"))
from yfm_control import EXECUTABLE, Game  # noqa: E402
from fm_editor.disc import DiscImage  # noqa: E402

SETTINGS = {"mod.3d-monsters": 0, "mod.hand-camera": 0, "mod.ai-hard-mode": 0}
NO_INTERPRETER = "this game has no MIPS interpreter"
SECTOR = 2048
# The images the game checks before running their C (src/pc/guest/retail_image.c).
BANK_SIZE, BANK_SHA = 0x16000, "baa203b937dc6bdf91b1826c5832f0f32e11ae5fe9d05193a4361bc08158b9e0"
CREDITS_SECTOR, CREDITS_SIZE = 1223, 0x8000   # func_800507D0's 16 SU sectors from 0x4C7
CREDITS_SHA = "f125a2a6a8b57d222df544a7a02bf8c639c1fdde5cf978f80a56ea3fba2b836a"
RECORD_SECTORS, VARIANT_SECTOR = 0x114, 180    # a MODEL.MRG record, its stance 0 slot A variant image
CARD, RECORD = 381, 330                         # card 381's model is record 330
FOREST = 330                                    # a field magic: the bank's effect when played
STAND_IN = re.compile(r"memories-pc: (mod (\S+): )?model variant module (\d+) \(slot A\) is MIPS code")


class Run:
    def __init__(self, label: str):
        self.label, self.failures, self.passed = label, [], 0

    def check(self, what: str, ok: bool, detail=None) -> None:
        print(f"  {self.label}: {'ok' if ok else 'FAILED'}: {what}" + (f" ({detail})" if detail is not None else ""),
              flush=True)
        if ok:
            self.passed += 1
        else:
            self.failures.append(what)


def write_mod(folder: Path, identifier: str, file: str, patches: list[tuple[int, bytes]]) -> Path:
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    manifest = {"id": identifier, "name": identifier, "version": "1.0.0", "enabled": True,
                "data": [{"file": file, "patch": [{"at": hex(at), "bytes": data.hex().upper()} for at, data in patches]}]}
    (folder / "mod.json").write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
    return folder


def changed(original: bytes, at: int) -> bytes:
    """Four bytes that differ from the disc's at `at`."""
    return bytes(b ^ 0xA5 for b in original[at:at + 4])


def bank_copies(wa: bytes) -> list[int]:
    """Where WA_MRG.MRG holds the retail effect bank: the offsets, by sector."""
    found = []
    for at in range(0, len(wa) - BANK_SIZE + 1, SECTOR):
        if wa[at:at + 4] == b"\x18\0\0\0" and hashlib.sha256(wa[at:at + BANK_SIZE]).hexdigest() == BANK_SHA:
            found.append(at)
    return found


def log_text(game_out: Path) -> str:
    log = game_out / "game.log"
    return log.read_text(errors="replace") if log.exists() else ""


def battle(game, card: int, alternative: int, cap: int = 12000) -> int:
    """An animated battle of `card` on both sides from the debug menu, as the
    duel starts one (duel_scene_battle.c, phase 5), run to its end: its frames."""
    record = struct.pack("<HhhBB", card, 0, 0, 0, alternative)
    game.poke("D_800EF658", record + record)
    game.poke("D_8009B269", bytes([0]))
    game.poke("D_8009B26C", bytes([1]))
    start = game.frame
    game.step(16)
    while game.u8("D_8009B26C") & 0x1F == 1:
        if game.frame - start > cap:
            raise TimeoutError(f"card {card}'s battle did not end in {cap} frames")
        game.step(16)
    return game.frame - start


def case_settings(executable: Path, out: Path, reference: bool) -> Run:
    run = Run("settings" + (" (reference)" if reference else ""))
    env = {"MEMORIES_MODEL_MODULES": "interpreter", "MEMORIES_DUEL_EFFECTS": "interpreter",
           "MEMORIES_CREDITS": "interpreter", "MEMORIES_TRACE_MIPS_PRINTF": "1", "MEMORIES_TRACE": "model"}
    with Game(executable, out=out, settings=SETTINGS, mods_dir=out / "mods", env=env) as game:
        game.goto("debug")
        battle(game, CARD, 1)
        game.goto("duel", opponent=1, deck=str(FOREST))   # the effects' setting is read at the first effect
        game.duel_ready()
        game.play_card(0)
        game.goto("credits")   # the credits' setting is read when they first run
        game.step(450)
        game.press("cross")
        game.step(300)
        game.press("cross")
        game.step(600)
    text = log_text(out)
    for setting in ("MEMORIES_MODEL_MODULES=interpreter", "MEMORIES_DUEL_EFFECTS=interpreter",
                    "MEMORIES_CREDITS=interpreter", "MEMORIES_TRACE_MIPS_PRINTF"):
        said = [line for line in text.splitlines() if setting in line and NO_INTERPRETER in line]
        if reference:
            run.check(f"{setting}: nothing said in the reference build", not said, said[:1])
        else:
            run.check(f"{setting}: said once", len(said) == 1, said)
    ways = set(re.findall(r"model\] variant slot=\d command=-?\d+ module \d+ in [AB]: (\S+)", text))
    run.check("the battle's variant modules ran " + ("interpreted" if reference else "native C (the default)"),
              ways == ({"interpreter"} if reference else {"native"}), sorted(ways))
    return run


def case_model(executable: Path, out: Path, disc: Path, reference: bool) -> Run:
    run = Run("model" + (" (reference)" if reference else ""))
    with DiscImage(disc) as image:
        lba, _ = image.find("DATA/MODEL.MRG")
        at = (RECORD * RECORD_SECTORS + VARIANT_SECTOR) * SECTOR
        variant = image.read(lba + at // SECTOR, 0x400)
    identifier = struct.unpack_from("<I", variant, 0)[0]
    mod = write_mod(out / "mods" / "no-interp-model", "no-interp-model", "\\DATA\\MODEL.MRG;1",
                    [(at + 0x200, changed(variant, 0x200))])
    with Game(executable, out=out, settings=SETTINGS, mods_dir=out / "mods",
              env={"MEMORIES_TRACE": "model,mods"}) as game:
        game.goto("debug")
        frames = battle(game, CARD, 0)
    text = log_text(out)
    said = [m.groups() for m in map(STAND_IN.search, text.splitlines()) if m]
    ways = set(re.findall(r"model\] variant slot=\d command=-?\d+ module " + str(identifier) + r" in A: (\S+)", text))
    run.check(f"the battle ended ({frames} frames)", True)
    if reference:
        run.check("the changed module ran interpreted", "interpreter" in ways, sorted(ways))
        run.check("nothing said of a stand-in", not said, said)
    else:
        run.check("the changed module ran the stand-in", ways == {"stand-in"}, sorted(ways))
        run.check("said once, naming the mod and mod-migrate",
                  said == [("mod no-interp-model: ", "no-interp-model", str(identifier))] and
                  all("model_gate.py mod-migrate" in line for line in text.splitlines() if STAND_IN.search(line)),
                  said)
    shutil.rmtree(mod, ignore_errors=True)
    return run


def case_effects(executable: Path, out: Path, disc: Path, reference: bool) -> Run:
    run = Run("effects" + (" (reference)" if reference else ""))
    with DiscImage(disc) as image:
        lba, size = image.find("DATA/WA_MRG.MRG")
        wa = image.read(lba, size)
    copies = bank_copies(wa)
    run.check(f"the disc's effect bank found in WA_MRG.MRG ({len(copies)} copies)", len(copies) >= 1, copies[:8])
    write_mod(out / "mods" / "no-interp-effects", "no-interp-effects", "\\DATA\\WA_MRG.MRG;1",
              [(at + 0x100, changed(wa, at + 0x100)) for at in copies])
    with Game(executable, out=out, settings=SETTINGS, mods_dir=out / "mods",
              env={"MEMORIES_TRACE": "duel_effects,mods"}) as game:
        game.goto("duel", opponent=1, deck=str(FOREST))
        game.duel_ready()
        went_on = True
        try:
            game.play_card(0)   # waits until the card's use has ended
            game.end_turn()
            game.wait_turn()
        except TimeoutError as error:
            went_on = False
            print(f"  effects: {error}", flush=True)
    text = log_text(out)
    run.check("the duel went on past the effect, to the player's next turn", went_on)
    said = [line for line in text.splitlines() if "duel effect bank is not the disc's" in line]
    verdicts = re.findall(r"duel_effects: (retail|changed) bytes .*?: (.*)", text)
    calls = re.findall(r"duel_effects\] id=(\d+)", text)
    run.check("the game saw the changed bank", any(kind == "changed" for kind, _ in verdicts), verdicts[:2])
    run.check(f"effects were called ({len(calls)} calls, ids {sorted(set(calls))})", bool(calls))
    if reference:
        run.check("interpreted, and nothing said", not said and all(way == "interpreter" for kind, way in verdicts
                                                                    if kind == "changed"), said[:1])
    else:
        run.check("completed at once, said once", len(said) == 1, said)
        run.check("no interpreter named", all("skipped" in way for kind, way in verdicts if kind == "changed"),
                  verdicts[:2])
    return run


def case_credits(executable: Path, out: Path, disc: Path, reference: bool) -> Run:
    run = Run("credits" + (" (reference)" if reference else ""))
    with DiscImage(disc) as image:
        lba, _ = image.find("DATA/SU.MRG")
        module = image.read(lba + CREDITS_SECTOR, CREDITS_SIZE)
    run.check("the disc's credits module found", hashlib.sha256(module).hexdigest() == CREDITS_SHA)
    at = CREDITS_SIZE - 0x10   # inside the hashed module, past the code the roll runs
    write_mod(out / "mods" / "no-interp-credits", "no-interp-credits", "\\DATA\\SU.MRG;1",
              [(CREDITS_SECTOR * SECTOR + at, changed(module, at))])
    with Game(executable, out=out, settings=SETTINGS, mods_dir=out / "mods", env={"MEMORIES_TRACE": "mods"}) as game:
        game.goto("credits")
        game.step(450)
        game.press("cross")
        game.step(300)
        game.press("cross")
        game.wait_until(lambda g: g.resident("credits"), 3000, what="the credits module")
        start = game.frame
        complete = None
        while game.frame - start < 9000:
            if struct.unpack("<b", game.peek("D_8009AF9A", 1))[0] == -2:
                complete = game.frame - start
                break
            game.step(4)
    text = log_text(out)
    said = [line for line in text.splitlines() if "the credits are not the disc's" in line]
    run.check("the game saw the changed credits", "credits: changed bytes" in text)
    run.check("the presentation is complete", complete is not None, f"after {complete} frames")
    if reference:
        run.check("interpreted, and nothing said", not said)
    else:
        run.check("skipped, said once", len(said) == 1, said)
    return run


def case_overlay(executable: Path, out: Path, reference: bool) -> Run:
    run = Run("overlay" + (" (reference)" if reference else ""))
    stopped = False
    try:
        with Game(executable, out=out, settings=SETTINGS, mods_dir=out / "mods",
                  env={"MEMORIES_CRASH_TEST": "overlay@120"}, command_timeout=60) as game:
            game.step(600)
    except Exception as error:   # the game stops: the client loses it
        stopped = True
        print(f"  overlay: the game stopped ({type(error).__name__})", flush=True)
    text = log_text(out)
    run.check("the game stopped", stopped)
    if reference:
        run.check("the interpreter took the call", "MIPS overlay" in text)
    else:
        run.check("the \"no native function\" report", "call into guest code at 0x80146100, which has no native "
                                                         "function" in text,
                  [line for line in text.splitlines() if "0x80146100" in line][:2])
    return run


CASES = ("settings", "model", "effects", "credits", "overlay")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--disc", type=Path, default=os.environ.get("MEMORIES_DISC"))
    parser.add_argument("--executable", type=Path, help="the game (default yfm_control's)")
    parser.add_argument("--reference", action="store_true", help="also run the cases on the reference build")
    parser.add_argument("--reference-executable", type=Path, help="the reference build (default tmp/pc/reference)")
    parser.add_argument("--out", type=Path, default=ROOT / "tmp/pc/no-interpreter")
    parser.add_argument("cases", nargs="*", choices=[[], *CASES], default=[])
    arguments = parser.parse_args()
    if not arguments.disc or not Path(arguments.disc).is_file():
        print("no_interpreter_runtime: no disc (MEMORIES_DISC or --disc), skipped")
        return 0
    disc = Path(arguments.disc).resolve()
    executable = (arguments.executable or EXECUTABLE).resolve()
    from reference_build import has_interpreter, reference_executable
    if has_interpreter(executable):
        print(f"no_interpreter_runtime: {executable} has the MIPS interpreter (a reference build); name the game")
        return 1
    games = [(executable, False)]
    if arguments.reference:
        games.append((reference_executable(arguments.reference_executable, subject=executable), True))
    runs = []
    for game, reference in games:
        for case in arguments.cases or CASES:
            out = arguments.out / (case + ("-reference" if reference else ""))
            shutil.rmtree(out, ignore_errors=True)
            out.mkdir(parents=True)
            print(f"{case}{' (reference)' if reference else ''}:", flush=True)
            if case in ("settings", "overlay"):
                runs.append(globals()["case_" + case](game, out, reference))
            else:
                runs.append(globals()["case_" + case](game, out, disc, reference))
    failed = sum(len(run.failures) for run in runs)
    for run in runs:
        print(f"{run.label}: {run.passed} passed, {len(run.failures)} failed" +
              "".join(f"\n  {failure}" for failure in run.failures))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
