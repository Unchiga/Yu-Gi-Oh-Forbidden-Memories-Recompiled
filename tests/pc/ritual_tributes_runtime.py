#!/usr/bin/env python3
"""Rituals of one to five tributes, from the field, the hand or both, played
in the built game (needs the player's disc).

A mod written for the run (in --out, never in the repository) gives some of
the disc's ritual cards new recipes ("tributes" of 1 to 5, "tributes_from"),
and fixes Simon Muran's deck to Forest so that nothing of the player's is
attacked while the field is built. Each scenario is a duel against him
reached with goto(), the deck dealt in the order it needs; the player's
plays go through the pad as a player's do. What each checks:

  retail      Ultimate Dragon's own recipe (three Blue-eyes on the field)
  one         one tribute from the field, the result in its zone
  hand        two from the hand only, the field empty: the first free zone
  both        four from hand and field: a hand copy before a field one
  five        five from the field; before it a hand ritual with the field
              full is refused
  refused     a hand ritual whose tribute is on the field, and a field one
              whose tribute is in the hand, take nothing
  cpu         the CPU plays a hand ritual and a one-tribute field ritual

Each pass writes pictures to --out/<pass>/ (look at them). With
--interpreter the scenarios run again with the duel effects interpreted
(MEMORIES_DUEL_EFFECTS=interpreter, the disc's own MIPS, on the reference
build: tools/pc/reference_build.py, built when it is missing, or
--reference-executable): the outcome must be the same and nothing may crash. --editor makes the mod with the FM
Editor's model and manifest instead of writing it here.

    python3 tests/pc/ritual_tributes_runtime.py [--executable PATH] [--out DIR]
        [--interpreter [--reference-executable PATH]] [--editor] [scenarios...]
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/pc"))
from yfm_control import Game, DUEL_PHASES, EXECUTABLE  # noqa: E402
from reference_build import reference_executable  # noqa: E402

MOD = "ritual-tributes-test"
SIMON = 1
FOREST = 330
# The disc's ritual cards the mod gives new recipes, and what each makes.
ONE, FIVE, HAND, BOTH = 670, 671, 673, 674            # Black Luster, Zera, War-lion, Beastry Mirror
HAND_MISSING, FIELD_MISSING, HAND_ANY = 676, 677, 678  # Commencement Dance, Hamburger, Sennen Genjin
CPU_HAND, CPU_FIELD = 679, 680                        # Novox's Prayer, Curse of Tri-Horned Dragon
ULTIMATE = 675                                        # left as the disc has it: three Blue-eyes
RESULT = {ONE: 364, FIVE: 360, HAND: 356, BOTH: 365, HAND_MISSING: 701, FIELD_MISSING: 702, HAND_ANY: 703,
          CPU_HAND: 704, CPU_FIELD: 705, ULTIMATE: 380}
# Monsters: Blue-eyes (Dragon), Mystical Elf (Spellcaster), Hitotsu-me Giant
# (Beast-Warrior), Baby Dragon (Dragon), Feral Imp (Fiend), Winged Dragon #1
# (Dragon), Mushroom Man (Plant), Shadow Specter (Zombie), Skull Servant.
BLUE_EYES, ELF, GIANT, BABY, IMP, WINGED, MUSHROOM, SPECTER, SERVANT = 1, 2, 3, 4, 6, 7, 8, 9, 24

RITUALS = [
    {"card": ONE, "tributes": [BLUE_EYES], "result": RESULT[ONE]},
    {"card": FIVE, "tributes": [BLUE_EYES, GIANT, BABY, {"type": "Fiend"}, {"min_attack": 0}],
     "result": RESULT[FIVE]},
    {"card": HAND, "tributes_from": "hand", "tributes": [BLUE_EYES, {"type": "Dragon"}], "result": RESULT[HAND]},
    {"card": BOTH, "tributes_from": "both", "tributes": [BLUE_EYES, GIANT, BABY, IMP], "result": RESULT[BOTH]},
    {"card": HAND_MISSING, "tributes_from": "hand", "tributes": [BLUE_EYES, ELF], "result": RESULT[HAND_MISSING]},
    {"card": FIELD_MISSING, "tributes": [GIANT], "result": RESULT[FIELD_MISSING]},
    {"card": HAND_ANY, "tributes_from": "hand", "tributes": [{"min_attack": 0}], "result": RESULT[HAND_ANY]},
    {"card": CPU_HAND, "tributes_from": "hand", "tributes": [SPECTER, SERVANT], "result": RESULT[CPU_HAND]},
    {"card": CPU_FIELD, "tributes": [{"max_attack": 900}], "result": RESULT[CPU_FIELD]},
]
CALL = re.compile(r"frame (\d+) vb \d+ duel_effects\] id=(\d+)")


def write_mod(mods: Path, editor: bool) -> dict:
    """The test's mod in `mods`; its manifest as the game reads it."""
    folder = mods / MOD
    shutil.rmtree(folder, ignore_errors=True)
    decks = {"Simon Muran": {"fixed": True, str(FOREST): 40}}
    if editor:
        from fm_editor import disc, fixed_decks, gamedata, manifest
        from fm_editor.model import Project
        retail = gamedata.load_game(disc.find_game([ROOT / "game"]))
        project = Project(retail)
        project.info.id = MOD
        project.info.name = "Ritual tributes test"
        for entry in RITUALS:      # as the Rituals tab stores a recipe
            tributes = [t if isinstance(t, dict) else {"card": t} for t in entry["tributes"]]
            project.set_ritual(entry["card"], tributes, entry["result"], entry.get("tributes_from", "field"))
        messages = []
        fixed_decks.read_entry(project, gamedata.DUELIST_NAMES[SIMON], decks["Simon Muran"], messages)
        assert not messages, messages
        manifest.save_mod(project, folder)
        reopened, messages = manifest.open_mod(retail, folder)
        assert not messages, messages
        assert manifest.build(reopened) == manifest.build(project), "save/reopen changed the mod"
    else:
        folder.mkdir(parents=True)
        (folder / "mod.json").write_text(json.dumps({
            "id": MOD, "name": "Ritual tributes test", "version": "1", "rituals": RITUALS, "decks": decks},
            indent=1), encoding="utf-8")
    return json.loads((folder / "mod.json").read_text(encoding="utf-8"))


class Run:
    """One game, the scenarios' checks and pictures."""

    def __init__(self, game: Game, log: Path, label: str):
        self.game, self.log, self.label = game, log, label
        self.failures: list[str] = []
        self.passed = 0

    def check(self, what: str, ok: bool, detail: object = "") -> bool:
        if ok:
            self.passed += 1
        else:
            self.failures.append(f"{what}: {detail}")
        print(f"  {'ok' if ok else 'FAILED'}: {what}" + ("" if ok else f" ({detail})"), flush=True)
        return ok

    def shot(self, name: str) -> Path:
        return self.game.shot(f"shots/{self.label}-{name}.png")

    def effects(self, first: int) -> list[int]:
        """The duel effects called since frame `first` (their ids)."""
        found = []
        for _, effect in self.calls(first):
            if effect not in found:
                found.append(effect)
        return found

    def calls(self, first: int) -> list[tuple[int, int]]:
        """Every duel effect call since frame `first`: (frame, id)."""
        found = []
        for line in self.log.read_text(errors="replace").splitlines():
            m = CALL.search(line)
            if m and int(m.group(1)) >= first:
                found.append((int(m.group(1)), int(m.group(2))))
        return found

    # The duel.

    def duel(self, deck: list[int], top: list[int], opponent_top: list[int] | None = None,
             opponent: dict | None = None) -> None:
        g = self.game

        def deal(game):
            game.arrange_deck(0, top)
            if opponent:
                base = game.address("gDuel_aDeckCardRecords") + 6 * 40
                records = [game.peek(base + 6 * i, 6) for i in range(40)]
                cards = opponent["deck"]
                game.poke(base, b"".join(struct.pack("<h", cards[i % len(cards)]) + r[2:]
                                         for i, r in enumerate(records)))
            if opponent_top:
                game.arrange_deck(1, opponent_top)
        g.goto("duel", opponent=SIMON, deck=deck)
        g.duel_ready(before_deal=deal)

    def end_turn(self, watch=None, name: str = "") -> None:
        """End the player's turn; back at their next hand (or the duel's
        end). A picture `name` the first time `watch()` holds on the way."""
        g = self.game
        g.wait_turn()
        g.press_until(lambda g: g.turn() != 0 or g.duel_over(), "start", every=60, timeout=1200,
                      what="the end of the turn")
        start, taken = g.vblank, False
        while not (g.duel_over() or (g.turn() == 0 and g.phase() == DUEL_PHASES["hand"])):
            if g.vblank - start > 30000:
                raise TimeoutError("the CPU's turn did not end")
            g.step(8)
            if watch and not taken and watch():
                g.step(24)
                self.shot(name)
                taken = True

    def hand(self, side: int = 0) -> list[int | None]:
        return [c and c["id"] for c in self.game.duel()[side]["hand"]]

    def monsters(self, side: int = 0) -> list[int | None]:
        return [c and c["id"] for c in self.game.duel()[side]["monsters"]]

    def play(self, card: int, monster: bool = True) -> None:
        """Play `card` from the hand: a monster face up, a ritual activated."""
        slot = self.hand().index(card)
        self.game.play_card(slot, face_up=monster)

    def ritual(self, card: int, film: str = "") -> dict:
        """Play the ritual card; what the field and hand were and became.
        With `film`, pictures every 40 frames of its effect (film-NN)."""
        g = self.game
        before = {"monsters": self.monsters(), "hand": self.hand(), "frame": g.frame}
        if film:
            g.wait_turn(("hand",))
            g._hand_to(self.hand().index(card))
            g.press("cross", hold=4, after=30)
            g.press_until(lambda g: g.phase() != DUEL_PHASES["hand"] or g.duel_over(), "cross", every=40,
                          timeout=1200, what="the ritual's activation")
            for n in range(16):
                g.step(40)
                self.shot(f"{film}-{n:02d}")
            g._settle()
        else:
            self.play(card, monster=False)
        g.step(60)
        after = {"monsters": self.monsters(), "hand": self.hand(), "effects": self.effects(before["frame"]),
                 "checked": g.u16("D_8009B1A0"), "busy": g.u16("gDuel_wCardEffectFlags")}
        return {"before": before, "after": after}


def spent(before: list, after: list) -> list:
    """The cards gone from `before` (as a multiset), in order."""
    rest = list(after)
    gone = []
    for card in before:
        if card is None:
            continue
        if card in rest:
            rest.remove(card)
        else:
            gone.append(card)
    return gone


# --- the scenarios ----------------------------------------------------------

def retail(run: Run) -> None:
    """The disc's own ritual, untouched by the mod: three Blue-eyes."""
    run.duel([BLUE_EYES, BLUE_EYES, BLUE_EYES, ULTIMATE, ELF], [BLUE_EYES, BLUE_EYES, BLUE_EYES, ULTIMATE, ELF])
    for _ in range(3):
        run.play(BLUE_EYES)
        run.game.end_turn()
    r = run.ritual(ULTIMATE)
    run.shot("retail")
    run.check("retail: Blue-eyes Ultimate Dragon summoned", RESULT[ULTIMATE] in r["after"]["monsters"], r)
    run.check("retail: the three Blue-eyes spent", BLUE_EYES not in r["after"]["monsters"], r)
    run.check("retail: the 3D ritual effect (22) ran", 22 in r["after"]["effects"], r["after"]["effects"])


def one(run: Run) -> None:
    run.duel([BLUE_EYES, ONE, ELF, GIANT, MUSHROOM], [BLUE_EYES, ONE, ELF, GIANT, MUSHROOM])
    run.play(BLUE_EYES)
    zone = run.monsters().index(BLUE_EYES)
    run.game.end_turn()
    run.shot("one-before")
    r = run.ritual(ONE)
    run.shot("one-after")
    m = r["after"]["monsters"]
    run.check("one: Black Luster Soldier in Blue-eyes' zone", m[zone] == RESULT[ONE], r)
    run.check("one: nothing else on the field", sum(1 for c in m if c) == 1, m)
    run.check("one: the hand kept its monsters", spent(r["before"]["hand"], r["after"]["hand"]) == [ONE], r)
    run.check("one: effect 22 ran, the check gave the result", 22 in r["after"]["effects"] and
              r["after"]["checked"] == RESULT[ONE], r["after"])


def hand(run: Run) -> None:
    run.duel([HAND, BLUE_EYES, BABY, GIANT, ELF], [HAND, BLUE_EYES, BABY, GIANT, ELF])
    run.shot("hand-before")
    r = run.ritual(HAND, film="hand-film")
    run.shot("hand-after")
    m, h = r["after"]["monsters"], r["after"]["hand"]
    run.check("hand: Super War-lion summoned with an empty field", [c for c in m if c] == [RESULT[HAND]], m)
    run.check("hand: it took the first free zone", run.game.field()[2][0] == RESULT[HAND], run.game.field())
    run.check("hand: Blue-eyes and Baby Dragon left the hand",
              sorted(spent(r["before"]["hand"], h)) == sorted([HAND, BLUE_EYES, BABY]), r)
    run.check("hand: Giant and Elf stay in the hand", sorted(c for c in h if c) == sorted([GIANT, ELF]), h)
    slots = run.game.duel()[0]["hand_slots"]
    run.check("hand: three hand slots empty", sum(1 for s in slots if s < 0) == 3, slots)
    # The next turn deals the hand back up to five, from what is left.
    run.game.end_turn()
    run.shot("hand-next-turn")
    h = [c for c in run.hand() if c]
    run.check("hand: the next turn's hand is five, Giant and Elf among them",
              len(h) == 5 and GIANT in h and ELF in h, h)
    run.check("hand: the result is still on the field", RESULT[HAND] in run.monsters(), run.monsters())


def both(run: Run) -> None:
    # Blue-eyes then the Giant go down; the third turn's hand is the ritual,
    # Baby Dragon, Feral Imp, the second Blue-eyes and Mystical Elf.
    deck = [BLUE_EYES, GIANT, BOTH, BABY, IMP, ELF]
    run.duel(deck, [BLUE_EYES, GIANT, BOTH, BABY, IMP, BLUE_EYES, ELF])
    run.play(BLUE_EYES)
    run.game.end_turn()
    run.play(GIANT)
    giant = run.monsters().index(GIANT)
    run.game.end_turn()
    run.check("both: the hand has the ritual, a Blue-eyes, Baby Dragon and Feral Imp",
              all(c in run.hand() for c in (BOTH, BLUE_EYES, BABY, IMP)), run.hand())
    run.shot("both-before")
    r = run.ritual(BOTH, film="both-film")
    run.shot("both-after")
    m = r["after"]["monsters"]
    run.check("both: Fiend's Mirror in the Giant's zone (the one field tribute)", m[giant] == RESULT[BOTH], m)
    run.check("both: the field's Blue-eyes stays (the hand's was spent)", BLUE_EYES in m, m)
    run.check("both: Blue-eyes, Baby Dragon and Feral Imp left the hand",
              sorted(spent(r["before"]["hand"], r["after"]["hand"])) == sorted([BOTH, BLUE_EYES, BABY, IMP]), r)


def five(run: Run) -> None:
    order = [BLUE_EYES, GIANT, BABY, IMP, WINGED]
    deck = order + [FIVE, HAND_ANY, MUSHROOM, ELF, SPECTER, SERVANT]
    run.duel(deck, order + [HAND_ANY, MUSHROOM, ELF, FIVE, SPECTER, SERVANT])
    for card in order:
        run.play(card)
        run.game.end_turn()
    zones = run.monsters()
    run.check("five: the field is full", all(zones), zones)
    run.shot("five-full")
    # A hand ritual with no zone free cannot take place.
    r = run.ritual(HAND_ANY)
    run.shot("five-refused")
    run.check("five: hand ritual refused with the field full", r["after"]["monsters"] == zones and
              22 not in r["after"]["effects"] and r["after"]["checked"] == 0, r)
    run.check("five: the hand lost only the ritual card", spent(r["before"]["hand"], r["after"]["hand"]) == [HAND_ANY],
              r)
    run.game.end_turn()
    r = run.ritual(FIVE, film="five-film")
    run.shot("five-after")
    m = r["after"]["monsters"]
    run.check("five: Zera summoned in Baby Dragon's zone (the middle tribute)", m[zones.index(BABY)] == RESULT[FIVE], m)
    run.check("five: all five tributes spent", [c for c in m if c] == [RESULT[FIVE]], m)
    run.check("five: effect 22 ran", 22 in r["after"]["effects"], r["after"]["effects"])


def refused(run: Run) -> None:
    run.duel([ELF, HAND_MISSING, BLUE_EYES, FIELD_MISSING, GIANT, MUSHROOM, SPECTER],
             [ELF, HAND_MISSING, BLUE_EYES, GIANT, MUSHROOM, FIELD_MISSING, SPECTER])
    run.play(ELF)
    run.game.end_turn()
    r = run.ritual(HAND_MISSING)
    run.check("refused: hand ritual whose Mystical Elf is on the field takes nothing",
              r["after"]["monsters"] == r["before"]["monsters"] and r["after"]["checked"] == 0 and
              spent(r["before"]["hand"], r["after"]["hand"]) == [HAND_MISSING] and 22 not in r["after"]["effects"], r)
    run.game.end_turn()
    r = run.ritual(FIELD_MISSING)
    run.shot("refused")
    run.check("refused: field ritual whose Giant is in the hand takes nothing",
              r["after"]["monsters"] == r["before"]["monsters"] and r["after"]["checked"] == 0 and
              spent(r["before"]["hand"], r["after"]["hand"]) == [FIELD_MISSING] and 22 not in r["after"]["effects"], r)


def cpu(run: Run, hand_only: bool = False) -> None:
    """The CPU's deck: Novox's Prayer with Shadow Specter and Skull Servant
    in its hand (a hand ritual), and Curse of Tri-Horned Dragon for a weak
    monster of its field; no other magic. The player only plays Forest:
    with the player's field empty the CPU's hand script takes its "ahead"
    path, whose strategy C sets a ritual in its hand face down, and its
    field script activates a set ritual once Duel_CheckRitual says it can
    (notes/ai-hard-mode-research.md)."""
    g = run.game
    run.duel([FOREST], [FOREST] * 5, opponent={"deck": [CPU_HAND, SPECTER, SERVANT, CPU_FIELD, MUSHROOM]},
             opponent_top=[CPU_HAND, SPECTER, SERVANT, MUSHROOM, CPU_FIELD])
    seen = {"hand": False} if hand_only else {"hand": False, "field": False}
    results = {"hand": RESULT[CPU_HAND], "field": RESULT[CPU_FIELD]}
    for turn in range(30):
        # The CPU attacks the empty field every turn: the player's LP is
        # kept up, so the duel lasts as long as the CPU takes to choose.
        g.poke(g.address("D_800E9FF0") + 0x12, struct.pack("<hH", 30000, 30000))
        run.play(FOREST, monster=False)
        start = g.frame
        # The CPU's side every few frames of its turn. Its tributes leave
        # as the 3D effect (22) ends; the look before that effect began is
        # what its ritual found, the look when the result appears what it
        # left.
        history = [(g.frame, {"hand": run.hand(1), "monsters": run.monsters(1)})]
        found = {}

        def watch():
            now = {"hand": run.hand(1), "monsters": run.monsters(1)}
            for kind, result in results.items():
                if kind in seen and not seen[kind] and kind not in found and \
                        result in now["monsters"] and result not in history[-1][1]["monsters"]:
                    found[kind] = now
            history.append((g.frame, now))
            return bool(found)
        run.end_turn(watch, f"cpu-turn{turn}-ritual")
        effects = run.effects(start)
        rituals = [frame for frame, effect in run.calls(start) if effect == 22]
        print(f"  turn {turn}: CPU field {run.monsters(1)}, hand {run.hand(1)}, effects {effects}", flush=True)
        for kind, after in found.items():
            before = [look for frame, look in history if frame < rituals[0]][-1] if rituals else history[0][1]
            seen[kind] = True
            zone = after["monsters"].index(results[kind])
            if kind == "hand":
                run.check("cpu: its hand ritual spent Shadow Specter and Skull Servant from its hand",
                          22 in effects and before["hand"].count(SPECTER) - after["hand"].count(SPECTER) == 1 and
                          before["hand"].count(SERVANT) - after["hand"].count(SERVANT) == 1 and
                          before["monsters"][zone] is None and
                          [c for i, c in enumerate(after["monsters"]) if i != zone] ==
                          [c for i, c in enumerate(before["monsters"]) if i != zone], (before, after))
            else:
                run.check("cpu: its field ritual put the result in its weak tribute's zone",
                          22 in effects and before["monsters"][zone] in (SPECTER, SERVANT, MUSHROOM) and
                          after["hand"] == before["hand"] and
                          [c for i, c in enumerate(after["monsters"]) if i != zone] ==
                          [c for i, c in enumerate(before["monsters"]) if i != zone], (before, after))
        if all(seen.values()) or g.duel_over():
            print(f"  ended: seen {seen}, duel over {g.duel_over()}, LP {[d['lp'] for d in g.duel()]}", flush=True)
            break
    for kind in seen:
        if kind == "hand" and not hand_only and not seen[kind]:
            # The disc's script sets a ritual only on a roll of its
            # strategy (30-40% for Simon), while its tributes are still in
            # its hand: cpu-hard asks for this one.
            print("  (the disc's script did not choose its hand ritual this run; cpu-hard covers it)")
            continue
        run.check(f"cpu: the CPU performed its {kind} ritual", seen[kind])


def cpu_hard(run: Run) -> None:
    """The same with AI Hard Mode (the game's own optional mod): its planner
    asks Duel_CheckRitual of each ritual in the hand and sets one that can
    take place, which the CPU's field script then activates. The disc's
    script seldom sets a ritual while its tributes are still in its hand."""
    cpu(run, hand_only=True)


SCENARIOS = {"retail": retail, "one": one, "hand": hand, "both": both, "five": five, "refused": refused,
             "cpu": cpu, "cpu-hard": cpu_hard}
# A scenario that needs settings of its own runs in a game of its own.
SETTINGS = {"cpu-hard": {"mod.ai-hard-mode": 1}}


def run_pass(executable: Path | None, out: Path, label: str, env: dict, names: list[str],
             settings: dict | None = None) -> Run:
    folder = out / label
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    log = folder / "log.txt"
    env = {"MEMORIES_TRACE": "duel_effects,mods", "MEMORIES_LOG": str(log), "MEMORIES_NO_MONITOR": "1", **env}
    print(f"{label}:", flush=True)
    with Game(executable, out=folder, mods_dir=out / "mods", env=env,
              settings={f"mod.{MOD}": 1, "mod.3d-monsters": 0, "mod.hand-camera": 0, "mod.ai-hard-mode": 0,
                        **(settings or {})}) as game:
        run = Run(game, log, label)
        for name in names:
            print(f" {name}", flush=True)
            try:
                SCENARIOS[name](run)
            except Exception as failure:   # one scenario's trouble is reported, the rest still run
                run.check(f"{name}: ran to the end", False, f"{type(failure).__name__}: {failure}")
                run.shot(f"{name}-error")
        # The game read every ritual of the mod and noted nothing against it
        # (Mods_Note: "memories-pc: mod <id>: ...").
        notes = [line for line in (folder / "game.log").read_text(errors="replace").splitlines()
                 if line.startswith(f"memories-pc: mod {MOD}:")]
        tables = re.search(r"tables: .*?(\d+) rituals", log.read_text(errors="replace"))
        run.check("the game noted nothing against the mod", not notes, notes)
        run.check(f"the game read the mod's {len(RITUALS)} rituals", tables and int(tables.group(1)) == len(RITUALS),
                  tables and tables.group(0))
    return run


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--out", type=Path, default=ROOT / "tmp/pc/ritual-tributes")
    parser.add_argument("--interpreter", action="store_true", help="also run with the duel effects interpreted")
    parser.add_argument("--reference-executable", type=Path,
                        help="--interpreter's game, one with the MIPS interpreter (default the reference build)")
    parser.add_argument("--editor", action="store_true", help="make the mod with the FM Editor's model")
    parser.add_argument("scenarios", nargs="*", choices=[[], *SCENARIOS], default=[])
    arguments = parser.parse_args()
    out = arguments.out.resolve()
    names = arguments.scenarios or list(SCENARIOS)
    out.mkdir(parents=True, exist_ok=True)
    manifest = write_mod(out / "mods", arguments.editor)
    print("rituals as the game reads them:", json.dumps(manifest.get("rituals"))[:400], flush=True)
    if any(name in SETTINGS for name in names):
        # AI Hard Mode as the game ships it, beside the executable.
        shipped = (arguments.executable or ROOT / "tmp/pc/game32/memories-pc").resolve().parent / "mods/ai-hard-mode"
        shutil.rmtree(out / "mods/ai-hard-mode", ignore_errors=True)
        shutil.copytree(shipped, out / "mods/ai-hard-mode")
    passes = [("", {}, arguments.executable)]
    if arguments.interpreter:   # the interpreter is in no shipped game: the reference build's
        passes.append(("interpreter-", {"MEMORIES_DUEL_EFFECTS": "interpreter"},
                       reference_executable(arguments.reference_executable,
                                            subject=arguments.executable or EXECUTABLE)))
    runs = []
    for prefix, env, executable in passes:
        plain = [name for name in names if name not in SETTINGS]
        if plain:
            runs.append(run_pass(executable, out, prefix + "native" if not prefix else "interpreter",
                                 env, plain))
        for name in names:
            if name in SETTINGS:
                runs.append(run_pass(executable, out, prefix + name, env, [name], SETTINGS[name]))
    failed = sum(len(r.failures) for r in runs)
    for r in runs:
        print(f"{r.label}: {r.passed} passed, {len(r.failures)} failed" +
              "".join(f"\n  {f}" for f in r.failures))
    print(f"pictures in {out}/*/shots")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
