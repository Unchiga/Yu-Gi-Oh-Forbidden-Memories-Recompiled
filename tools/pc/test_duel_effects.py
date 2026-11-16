#!/usr/bin/env python3
"""The duel effects, native against the interpreter (MEMORIES_DUEL_EFFECTS=
interpreter, on the reference build: the game with the MIPS interpreter,
which no shipped game has; tools/pc/reference_build.py builds it when it is
missing, --reference-executable names another), on the control client: the twenty scenarios of the native
duel-effects review (its fx.py and scenarios.py, which drove one process per
run with frame-numbered presses from a story state). Each side is one game:
every scenario is a duel against Simon Muran reached with goto() and a deck
of the cards under test, played with the client's duel actions. The two
sides must agree on every frame hash and every effect call, and each
scenario must have called an effect.

    python3 tools/pc/test_duel_effects.py [--executable PATH] [--reference-executable PATH] [names...]
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from yfm_control import EXECUTABLE, Game, OUTPUT  # noqa: E402
from reference_build import reference_executable  # noqa: E402

OUT = OUTPUT / "duel-effects"   # a new duel-effects-* folder in it a run, unless --out
SIMON = 1


def opponent_deck(card):
    """The opponent's whole deck made `card` (its id only, as the review's
    state edit did), so the CPU sets it."""
    def edit(game):
        game.poke(game.address("gDuel_aDeckCardRecords") + 6 * 40, b"".join(
            struct.pack("<h", card) + record[2:] for record in
            (game.peek(game.address("gDuel_aDeckCardRecords") + 6 * i, 6) for i in range(40, 80))))
    return edit


def opening(*cards):
    return lambda game: game.arrange_deck(0, cards)


def magic(game):
    game.play_card(0)


def magic_twice(game):
    game.play_card(0)
    game.end_turn()
    game.play_card(0)


def monster(game):
    game.play_card(0, face_up=True)
    game.end_turn()


def monster_battle(game):
    """A monster face up, and on the next turn another, which attacks the
    opponent's first monster (a battle, whatever the CPU did)."""
    monster(game)
    game.play_card(0, face_up=True)
    target = next((column for column, card in enumerate(game.field()[1]) if card), None)
    game.attack(0, target)


def fuse(game):
    game.fuse([0, 1])


def fuse_attack(game):
    game.fuse([0, 1])
    game.end_turn()
    game.play_card(0, face_up=True)
    target = next((column for column, card in enumerate(game.field()[1]) if card), None)
    game.attack(0, target)


def ritual(game):
    """Three turns of a tribute monster (card 1) face up, then the ritual."""
    for _ in range(3):
        hand = [card and card["id"] for card in game.duel()[0]["hand"]]
        game.play_card(hand.index(1), face_up=True)
        game.end_turn()
    hand = [card and card["id"] for card in game.duel()[0]["hand"]]
    game.play_card(hand.index(675))


# name, deck (MEMORIES_DEBUG_DECK's syntax), before the deal, what the player does
SCENARIOS = [
    ("raigeki-t2", "337", None, magic_twice), ("forest", "330", None, magic), ("dark-hole", "336", None, magic),
    ("swords", "348", None, magic), ("harpie", "672", None, magic), ("warrior-elim", "653", None, magic),
    ("crush-card", "661", None, magic), ("stop-defense", "320", None, magic), ("dp-light", "350", None, magic),
    ("cursebreaker", "655", None, magic), ("spellbinding", "349", None, magic_twice),
    ("shadow-spell", "669", None, magic_twice), ("fusion-attack", "425", None, fuse_attack),
    ("equip-sword", "41,301", opening(41, 301), fuse), ("megamorph", "41,657", opening(41, 657), fuse),
    ("kuriboh-battle", "58", None, monster_battle), ("ritual", "1,1,1,675", opening(1, 1, 1, 675), ritual),
    ("exodia", "17-21", opening(17, 18, 19, 20, 21), monster),
    ("bad-reaction", "342", opponent_deck(688), magic_twice), ("goblin-fan", "344", opponent_deck(687), magic_twice),
]
CALL = re.compile(r"frame (\d+) vb \d+ duel_effects\] (id=.*)")


def side(executable, name, env, scenarios):
    """One game through every scenario: {name: (first frame, last frame, picture, error)}."""
    out = OUT / name
    shutil.rmtree(out, ignore_errors=True)
    (out / "mods").mkdir(parents=True)
    env = {**env, "MEMORIES_FRAME_HASHES": str(out / "hashes.txt"), "MEMORIES_TRACE": "duel_effects",
           "MEMORIES_LOG": str(out / "log.txt")}
    spans = {}
    with Game(executable, out=out, settings={"mod.3d-monsters": 0, "mod.hand-camera": 0}, mods_dir=out / "mods",
              env=env) as game:
        for label, deck, before_deal, play in scenarios:
            first, error = game.frame, None
            try:
                game.goto("duel", opponent=SIMON, deck=deck)
                game.duel_ready(before_deal=before_deal)
                play(game)
                game.step(120)
            except (TimeoutError, RuntimeError, StopIteration, ValueError) as failure:
                error = f"{type(failure).__name__}: {failure}"
            spans[label] = (first, game.frame, game.shot(f"{label}.png"), error)
    return out, spans


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--executable", type=Path, help="the native side's game (default yfm_control's)")
    parser.add_argument("--reference-executable", type=Path,
                        help="the interpreted side's game, one with the MIPS interpreter (default the reference "
                             "build, built when it is missing)")
    parser.add_argument("--out", type=Path, help="where the two games go (default: a new tmp/pc/control/"
                                                  "duel-effects-* folder)")
    parser.add_argument("names", nargs="*")
    arguments = parser.parse_args()
    global OUT
    if arguments.out:
        OUT = arguments.out.resolve()
    else:   # a folder of its own: two runs at once must not share one
        OUTPUT.mkdir(parents=True, exist_ok=True)
        OUT = Path(tempfile.mkdtemp(prefix="duel-effects-", dir=OUTPUT))
    scenarios = [s for s in SCENARIOS if not arguments.names or s[0] in arguments.names]
    reference = reference_executable(arguments.reference_executable, subject=arguments.executable or EXECUTABLE)
    with ThreadPoolExecutor(2) as pool:
        native = pool.submit(side, arguments.executable, "native", {}, scenarios)
        interpreted = pool.submit(side, reference, "interpreter",
                                  {"MEMORIES_DUEL_EFFECTS": "interpreter"}, scenarios)
        (native_out, native_spans), (int_out, int_spans) = native.result(), interpreted.result()
    hashes = [dict(line.split() for line in (out / "hashes.txt").read_text().splitlines())
              for out in (native_out, int_out)]
    calls = [[(int(m.group(1)), m.group(2)) for m in map(CALL.search, (out / "log.txt").read_text(errors="replace")
                                                       .splitlines()) if m] for out in (native_out, int_out)]
    failed = 0
    for label, *_ in scenarios:
        (first, last, _, error), other = native_spans[label], int_spans[label]
        frames = [str(frame) for frame in range(first, last + 1)]
        differing = next((frame for frame in frames if hashes[0].get(frame) != hashes[1].get(frame)), None)
        mine = [call for call in calls[0] if first <= call[0] <= last]
        theirs = [call for call in calls[1] if first <= call[0] <= last]
        ids = sorted({int(call[1].split()[0][3:]) for call in mine})
        ok = not error and not other[3] and (first, last) == other[:2] and differing is None and mine == theirs and mine
        failed += not ok
        print(f"duel effects: {label}: {'ok' if ok else 'FAILED'}: frames {first}-{last}, effect ids {ids}, "
              f"{len(mine)} calls" + (f"; {error or other[3]}" if error or other[3] else "") +
              (f"; first differing frame {differing}" if differing else "") +
              ("; the calls differ" if mine != theirs else "") + ("; no effect ran" if not mine else ""))
    print(f"duel effects: {len(scenarios) - failed} of {len(scenarios)} agree; pictures in {native_out}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
