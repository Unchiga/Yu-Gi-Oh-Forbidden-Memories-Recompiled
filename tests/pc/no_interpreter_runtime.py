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
  effects   a data mod that changes the duel effect bank's code (every copy
            in WA_MRG.MRG): a field magic's effect (Forest: its card's use
            waits for the effect's hand-off) completes at once, said once, and
            the duel goes on to the next turn
  raigeki   Raigeki on a full opponent's field, with the disc's bank (its C)
            and with one whose code a mod changed (completed at once): every
            opponent's monster is destroyed both times, as on the console
            (the bolt count its caller destroys zone by zone)
  swords    Swords of Revealing Light, then Forest a turn, with the disc's
            bank (its C) and with one whose code a mod changed (completed at
            once): the swords hold the opponent's side as many turns both
            times, and when they end no Swords request is left in the pool
            (no pointer to a freed entry, none kept forever); the duel goes on
  own-data  a data mod that changes a table of the bank only (Swords of
            Revealing Light's sword positions), without and with
            "duel_effects": "own data" in its mod.json: without, completed at
            once and the opt-in named; with, the bank's C runs the mod's
            table, said once
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
RAIGEKI, SWORDS, KURIBOH = 337, 348, 58
# Swords of Revealing Light's sword positions in the bank (D_8015AB68, at
# 0x8015AC34 here: config/pc/duel_effect_bank.csv), a table its C reads; and
# where the bank's dispatcher keeps the request's position (D_8015B7F8), which
# only the bank's C writes.
SWORD_POSITIONS, DISPATCH_POSITION = 0x14C34, 0x8015B7D0
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


def write_mod(folder: Path, identifier: str, file: str, patches: list[tuple[int, bytes]], **more) -> Path:
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    manifest = {"id": identifier, "name": identifier, "version": "1.0.0", "enabled": True,
                "data": [{"file": file, "patch": [{"at": hex(at), "bytes": data.hex().upper()} for at, data in patches]}],
                **more}
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


def wa_mrg(disc: Path) -> bytes:
    with DiscImage(disc) as image:
        lba, size = image.find("DATA/WA_MRG.MRG")
        return image.read(lba, size)


def bank_said(text: str) -> list[str]:
    """What the game said of a changed bank, on stderr (duel_effects.c)."""
    return [line for line in text.splitlines() if "memories-pc:" in line and "duel effect bank" in line]


def verdicts_of(text: str) -> list[tuple[str, str]]:
    return re.findall(r"duel_effects: (retail|changed|changed data, retail code) bytes .*?: (.*)", text)


def hand_index(game, card: int) -> int:
    hand = [slot and slot["id"] for slot in game.duel()[0]["hand"]]
    return hand.index(card)


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
    said = bank_said(text)
    verdicts = verdicts_of(text)
    calls = re.findall(r"duel_effects\] id=(\d+)", text)
    run.check("the game saw the changed bank", any(kind == "changed" for kind, _ in verdicts), verdicts[:2])
    run.check(f"effects were called ({len(calls)} calls, ids {sorted(set(calls))})", bool(calls))
    if reference:
        run.check("interpreted, and nothing said", not said and all(way == "interpreter" for kind, way in verdicts
                                                                    if kind == "changed"), said[:1])
    else:
        run.check("completed at once, said once, naming the code", len(said) == 1 and "code is not" in said[0], said)
        run.check("no interpreter named", all("skipped" in way for kind, way in verdicts if kind == "changed"),
                  verdicts[:2])
    return run


def raigeki_duel(executable: Path, out: Path) -> tuple[list, list, str]:
    """Four turns of Forest while the opponent, whose deck is all Kuriboh,
    sets a monster a turn; then Raigeki: (the opponent's monsters before it,
    after it, the log)."""
    def deal(game):
        game.arrange_deck(0, [FOREST] * 4 + [RAIGEKI])
        game.poke(game.address("gDuel_aDeckCardRecords") + 6 * 40, b"".join(
            struct.pack("<h", KURIBOH) + record[2:] for record in
            (game.peek(game.address("gDuel_aDeckCardRecords") + 6 * i, 6) for i in range(40, 80))))
    with Game(executable, out=out, settings=SETTINGS, mods_dir=out / "mods",
              env={"MEMORIES_TRACE": "duel_effects,mods"}) as game:
        game.goto("duel", opponent=1, deck=f"{FOREST},{RAIGEKI}")
        game.duel_ready(before_deal=deal)
        for _ in range(4):
            game.play_card(hand_index(game, FOREST))
            game.end_turn()
        before = game.field()[1]
        game.play_card(hand_index(game, RAIGEKI))
        game.step(240)
        after = game.field()[1]
    return before, after, log_text(out)


def case_raigeki(executable: Path, out: Path, disc: Path, reference: bool) -> Run:
    run = Run("raigeki" + (" (reference)" if reference else ""))
    wa = wa_mrg(disc)
    copies = bank_copies(wa)
    results = {}
    for label, patches in (("the disc's bank", None),
                           ("a bank whose code a mod changed", [(at + 0x100, changed(wa, at + 0x100))
                                                                for at in copies])):
        game_out = out / ("retail" if patches is None else "changed")
        (game_out / "mods").mkdir(parents=True)
        if patches:
            write_mod(game_out / "mods" / "no-interp-raigeki", "no-interp-raigeki", "\\DATA\\WA_MRG.MRG;1", patches)
        before, after, text = raigeki_duel(executable, game_out)
        results[label] = (before, after)
        monsters = [card for card in before if card]
        run.check(f"{label}: the opponent had monsters in more than one zone before Raigeki", len(monsters) >= 2,
                  before)
        run.check(f"{label}: Raigeki destroyed every one", not any(after), after)
        if patches and not reference:
            said = bank_said(text)
            run.check(f"{label}: completed at once, said once", len(said) == 1 and "code is not" in said[0], said)
    return run


def swords_entries(game, addresses: set[int]) -> list[int]:
    """Of these effect pool entries, the ones in use by a Swords request (id
    0x15 at +0x18, DUEL_EFFECT_REQUEST_FLAG_ACTIVE in the flags at +0x1C)."""
    found = []
    for address in sorted(addresses):
        identifier, _, flags = struct.unpack("<hhB", game.peek(address + 0x18, 5))
        if identifier == 0x15 and flags & 0x80:
            found.append(address)
    return found


def swords_duel(executable: Path, out: Path) -> tuple[list, list, bool]:
    """Swords of Revealing Light once, then Forest a turn: (the opponent's
    side's pointer after each turn, the Swords requests still in the pool
    when the swords have ended, whether the duel went on)."""
    with Game(executable, out=out, settings=SETTINGS, mods_dir=out / "mods",
              env={"MEMORIES_TRACE": "duel_effects,mods"}) as game:
        game.goto("duel", opponent=1, deck=f"{SWORDS},{FOREST}")
        game.duel_ready(before_deal=lambda g: g.arrange_deck(0, [SWORDS] + [FOREST] * 4))
        pointers = game.address("gDuel_apSwordsEffectObjects")
        game.play_card(hand_index(game, SWORDS))
        game.step(60)
        seen, held, went_on = set(), [], True
        try:
            for _ in range(6):
                pointer = struct.unpack("<2I", game.peek(pointers, 8))[1]
                held.append(pointer)
                if not pointer:
                    break
                seen.add(pointer)
                game.end_turn()
                game.play_card(hand_index(game, FOREST))
                game.step(30)
            game.step(240)
            left = swords_entries(game, seen)
            game.end_turn()
        except (TimeoutError, ValueError) as error:
            went_on, left = False, []
            print(f"  swords: {error}", flush=True)
    return held, left, went_on


def case_swords(executable: Path, out: Path, disc: Path, reference: bool) -> Run:
    run = Run("swords" + (" (reference)" if reference else ""))
    wa = wa_mrg(disc)
    copies = bank_copies(wa)
    results = {}
    for label, patches in (("the disc's bank", None),
                           ("a bank whose code a mod changed", [(at + 0x100, changed(wa, at + 0x100))
                                                                for at in copies])):
        game_out = out / ("retail" if patches is None else "changed")
        (game_out / "mods").mkdir(parents=True)
        if patches:
            write_mod(game_out / "mods" / "no-interp-swords", "no-interp-swords", "\\DATA\\WA_MRG.MRG;1", patches)
        held, left, went_on = swords_duel(executable, game_out)
        results[label] = held
        turns = sum(1 for pointer in held if pointer)
        run.check(f"{label}: the swords held the opponent's side for some turns, then ended", 0 < turns < len(held),
                  [hex(pointer) for pointer in held])
        run.check(f"{label}: no Swords request left in the pool after they ended", not left, [hex(a) for a in left])
        run.check(f"{label}: the duel went on", went_on)
    lengths = [sum(1 for pointer in held if pointer) for held in results.values()]
    run.check("the swords lasted as many turns both times", len(set(lengths)) == 1, lengths)
    return run


def case_own_data(executable: Path, out: Path, disc: Path, reference: bool) -> Run:
    run = Run("own-data" + (" (reference)" if reference else ""))
    wa = wa_mrg(disc)
    copies = bank_copies(wa)
    # A sword's x, a little to the side: a table's value, not code.
    patches = [(at + SWORD_POSITIONS, bytes([wa[at + SWORD_POSITIONS] ^ 0x10])) for at in copies]
    for opted in (False, True):
        game_out = out / ("opted-in" if opted else "not-opted-in")
        (game_out / "mods").mkdir(parents=True)
        write_mod(game_out / "mods" / "own-data-test", "own-data-test", "\\DATA\\WA_MRG.MRG;1", patches,
                  **({"duel_effects": "own data"} if opted else {}))
        with Game(executable, out=game_out, settings=SETTINGS, mods_dir=game_out / "mods",
                  env={"MEMORIES_TRACE": "duel_effects,mods"}) as game:
            game.goto("duel", opponent=1, deck=str(SWORDS))
            game.duel_ready()
            original = game.peek(DISPATCH_POSITION, 8)
            game.play_card(0)
            game.step(120)
            position = game.peek(DISPATCH_POSITION, 8)
        text = log_text(game_out)
        said, verdicts = bank_said(text), verdicts_of(text)
        how = "with the opt-in" if opted else "without the opt-in"
        run.check(f"{how}: the game saw a change in the bank's data only",
                  any(kind == "changed data, retail code" for kind, _ in verdicts), verdicts[:2])
        ran = position != original
        if reference:
            run.check(f"{how}: interpreted, nothing said", not said and ran, said[:1])
        elif opted:
            run.check(f"{how}: said once, naming the mod and that nothing checks the data",
                      len(said) == 1 and "own-data-test" in said[0] and "nothing checks" in said[0], said)
            run.check(f"{how}: the bank's C ran (its dispatcher wrote the request's position)", ran,
                      f"{original.hex()} -> {position.hex()}")
        else:
            run.check(f"{how}: completed at once, said once, naming the opt-in",
                      len(said) == 1 and '"own data"' in said[0] and "effect-migrate" in said[0], said)
            run.check(f"{how}: the bank's C did not run", not ran, f"{original.hex()} -> {position.hex()}")
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


CASES = ("settings", "model", "effects", "raigeki", "swords", "own-data", "credits", "overlay")


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
                runs.append(globals()["case_" + case.replace("-", "_")](game, out, disc, reference))
    failed = sum(len(run.failures) for run in runs)
    for run in runs:
        print(f"{run.label}: {run.passed} passed, {len(run.failures)} failed" +
              "".join(f"\n  {failure}" for failure in run.failures))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
