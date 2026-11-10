#!/usr/bin/env python3
"""The CPU and "combat" monster effects (the FM Editor's "Before combat";
notes/more-cards.md, "Monster effects"), played in the built game (needs
the player's disc): a "combat" boost exists only in its battle, and the CPU
chooses its attacks before one begins, so it must foresee the boost.

The mod is written for the run with the FM Editor's model and manifest
(in --out, never in the repository). Each scenario is a fresh debug duel
against Simon Muran, his deck poked to forty copies of one monster; the
player's first turn puts down one face-up monster; then the CPU plays:

  self-atk    Hitotsu-me Giant (1200 ATK), +1500 ATK before combat, against
              Swamp Battleguard (1800): no attack, the Giant has 2700 in it
  self-def    Feral Imp in defense (1400 DEF), +1500 DEF before combat,
              against Battleguard: no attack into its 2900
  battle-atk  Winged Dragon #1 (1400 ATK), -1000 ATK on the monster it
              battles, against Battleguard: no attack, which would be 800
  own         Rabid Horseman (2000 ATK) against Battle Steer (1800), +1000
              ATK before combat: the Steer attacks, its 2800 wins. The
              CPU's hand play sees printed stats (as with face_up boosts),
              so if it sets the Steer face down and does not attack, the
              Steer is turned face up in attack for one more turn.

No pair of these monsters has a guardian-star matchup, either star. Each
scenario passes when the CPU attacks (an LP change, the player's monster
gone or a battle in the trace) as a CPU that sees the effects would. Every
scenario runs with the retail AI and again with AI Hard Mode (the copy
beside the executable); pictures go in --out/<pass>/game.

    python3 tests/pc/ai_combat_effects_runtime.py [--executable PATH] [--out DIR]
        [--pass retail|hard] [scenarios...]
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
from yfm_control import EXECUTABLE, Game  # noqa: E402

MOD = "ai-combat-test"
SIMON = 1
GIANT, IMP, WINGED, HORSEMAN = 3, 6, 7, 92
BATTLEGUARD, STEER = 12, 14
MEDICINE = 339              # Red Medicine: the player's filler
RECORD = 0x1C
FLAGS = 0x16                # a duel record's flags (yfm_control.Game.duel)
SCENARIOS = {
    "self-atk": dict(player=GIANT, defense=False, cpu=BATTLEGUARD, attacks=False),
    "self-def": dict(player=IMP, defense=True, cpu=BATTLEGUARD, attacks=False),
    "battle-atk": dict(player=WINGED, defense=False, cpu=BATTLEGUARD, attacks=False),
    "own": dict(player=HORSEMAN, defense=False, cpu=STEER, attacks=True, restage=True),
}
EFFECTS = {
    GIANT: [{"when": "combat", "do": "boost", "target": "self", "attack": 1500}],
    IMP: [{"when": "combat", "do": "boost", "target": "self", "defense": 1500}],
    WINGED: [{"when": "combat", "do": "boost", "target": "battle", "attack": -1000}],
    STEER: [{"when": "combat", "do": "boost", "target": "self", "attack": 1000}],
}


def write_mod(mods: Path) -> None:
    """The test's mod, made as the FM Editor makes one."""
    from fm_editor import disc, gamedata, manifest, monster_effects as fx
    from fm_editor.model import Project
    folder = mods / MOD
    shutil.rmtree(folder, ignore_errors=True)
    project = Project(gamedata.load_game(disc.find_game([ROOT / "game"])))
    project.info.id = MOD
    project.info.name = "AI combat effects test"
    for card, effects in EFFECTS.items():
        assert all(fx.normalize(e) for e in effects), effects
        project.set_monster_effects(card, [fx.normalize(e) for e in effects])
    manifest.save_mod(project, folder)


def flags(game: Game, record: int, keep: int = 0xFFFF, add: int = 0) -> None:
    at = game.address("D_801A7AD8") + record * RECORD + FLAGS
    value = struct.unpack("<H", game.peek(at, 2))[0]
    game.poke(at, struct.pack("<H", value & keep | add))


def scenario(g: Game, log: Path, name: str) -> dict:
    s = SCENARIOS[name]
    seen = len(log.read_text(errors="replace")) if log.exists() else 0

    def deal(game: Game) -> None:
        game.arrange_deck(0, [s["player"]])
        base = game.address("gDuel_aDeckCardRecords") + 6 * 40
        records = [game.peek(base + 6 * i, 6) for i in range(40)]
        game.poke(base, b"".join(struct.pack("<h", s["cpu"]) + r[2:] for r in records))

    def mine() -> list:
        return [c and c["id"] for c in g.duel()[0]["monsters"]]

    g.goto("duel", opponent=SIMON, deck=[s["player"]] + [MEDICINE] * 39)
    g.duel_ready(before_deal=deal)
    hand = [c and c["id"] for c in g.duel()[0]["hand"]]
    g.play_card(hand.index(s["player"]), face_up=True)
    g.step(30)
    if s["defense"]:
        flags(g, 5 + mine().index(s["player"]), add=0x0800)
        g.step(10)
    g.shot(f"{name}-player.png")
    before = [d["lp"] for d in g.duel()]
    g.end_turn()
    g.step(30)
    if s.get("restage") and g.duel()[1]["lp"] == before[1] and s["player"] in mine():
        for zone, card in enumerate(g.duel()[1]["monsters"]):
            if card and card["id"] == s["cpu"]:
                flags(g, 20 + zone, keep=~0x1800 & 0xFFFF)
        g.step(10)
        hand = [c and c["id"] for c in g.duel()[0]["hand"]]
        g.play_card(hand.index(MEDICINE), face_up=True)
        g.step(30)
        g.shot(f"{name}-restaged.png")
        g.end_turn()
        g.step(30)
    g.shot(f"{name}-after.png")
    after = [d["lp"] for d in g.duel()]
    text = log.read_text(errors="replace")[seen:]
    battles = re.findall(r"monster effects: battle card (\d+) record (\d+)", text)
    attacked = after != before or s["player"] not in mine() or bool(battles)
    cpu = [c and (c["id"], "down" if c["face_down"] else "up", "def" if c["defense_position"] else "atk")
           for c in g.duel()[1]["monsters"]]
    return {"attacked": attacked, "expected": s["attacks"], "lp": [before, after], "player": mine(), "cpu": cpu,
            "battles": battles}


def run_pass(executable: Path, out: Path, hard: bool, names: list[str]) -> list[str]:
    label = "hard" if hard else "retail"
    folder = out / label
    shutil.rmtree(folder, ignore_errors=True)
    mods = folder / "mods"
    mods.mkdir(parents=True)
    write_mod(mods)
    if hard:
        shutil.copytree(executable.parent / "mods" / "ai-hard-mode", mods / "ai-hard-mode")
    log = folder / "trace.log"
    env = {"MEMORIES_TRACE": "duel_effects,mods", "MEMORIES_LOG": str(log), "MEMORIES_NO_MONITOR": "1"}
    settings = {f"mod.{MOD}": 1, "mod.3d-monsters": 0, "mod.hand-camera": 0, "mod.ai-hard-mode": int(hard)}
    failures, results = [], {}
    print(f"{label}:", flush=True)
    with Game(executable, out=folder / "game", mods_dir=mods, env=env, settings=settings) as g:
        for name in names:
            try:
                r = results[name] = scenario(g, log, name)
            except Exception as problem:   # one scenario's trouble is reported, the rest still run
                failures.append(f"{label} {name}: {type(problem).__name__}: {problem}")
                continue
            ok = r["attacked"] == r["expected"]
            print(f"  {name}: {'passed' if ok else 'FAILED'}: the CPU {'attacked' if r['attacked'] else 'held'}"
                  f" (LP {r['lp'][0]} -> {r['lp'][1]}, player {r['player']}, CPU {r['cpu']})", flush=True)
            if not ok:
                failures.append(f"{label} {name}")
    (folder / "results.json").write_text(json.dumps(results, indent=2))
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--executable", type=Path, default=EXECUTABLE)
    parser.add_argument("--out", type=Path, default=ROOT / "tmp/pc/ai-combat-effects")
    parser.add_argument("--pass", dest="passes", action="append", choices=("retail", "hard"))
    parser.add_argument("scenarios", nargs="*", choices=[[], *SCENARIOS], default=[])
    arguments = parser.parse_args()
    names = arguments.scenarios or list(SCENARIOS)
    executable, out = arguments.executable.resolve(), arguments.out.resolve()
    failures = []
    for label in arguments.passes or ("retail", "hard"):
        failures += run_pass(executable, out, label == "hard", names)
    print("all passed" if not failures else "failed:\n  " + "\n  ".join(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
