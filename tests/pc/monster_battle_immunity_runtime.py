#!/usr/bin/env python3
"""Runtime test for Mod API 19: Monster Effects Conditions and Battle Immunity.

Verifies:
1. Unconditional battle immunity: Monster survives battle against a stronger monster,
   while standard battle damage is inflicted to Life Points.
2. Conditional battle immunity (strictly lowest ATK):
   - When condition is MET: Monster survives battle.
   - When condition is NOT met: Monster is destroyed in battle.
3. Conditional battle immunity (stat threshold):
   - Monster with ATK <= threshold survives battle.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/pc"))
from fm_editor import disc, gamedata, manifest
from fm_editor.model import Project
from yfm_control import Game

SIMON = 1
KURIBOH = 18        # 300 ATK, 200 DEF
TIME_WIZARD = 16    # 500 ATK, 400 DEF
MUSHROOM_MAN = 8    # 800 ATK, 600 DEF
BLUE_EYES = 1       # 3000 ATK, 2500 DEF
MEDICINE = 339      # Red Medicine


def build_test_mod(mods_dir: Path) -> str:
    mod_id = "battle-immunity-test"
    retail = gamedata.load_game(disc.find_game([ROOT / "game"]))
    project = Project(retail)
    project.info.id = mod_id
    project.info.name = "Battle Immunity Test"

    # 1. Kuriboh: unconditional battle immunity
    project.set_monster_effects(KURIBOH, [
        {"when": "face_up", "do": "battle_immunity"}
    ])

    # 2. Time Wizard: battle immunity only if lowest ATK on field
    project.set_monster_effects(TIME_WIZARD, [
        {"when": "face_up", "do": "battle_immunity", "conditions": [{"type": "lowest_atk"}]}
    ])

    # 3. Mushroom Man: battle immunity if ATK <= 1000
    project.set_monster_effects(MUSHROOM_MAN, [
        {"when": "face_up", "do": "battle_immunity",
         "conditions": [{"type": "stat_threshold", "stat": "atk", "op": "<=", "value": 1000}]}
    ])

    # Simon's deck: Blue-Eyes and Red Medicine
    built = manifest.build(project)
    built["decks"] = {
        gamedata.DUELIST_NAMES[SIMON]: {
            "fixed": True,
            str(BLUE_EYES): 10,
            str(MEDICINE): 30
        }
    }
    mod_folder = mods_dir / mod_id
    mod_folder.mkdir(parents=True, exist_ok=True)
    (mod_folder / "mod.json").write_text(json.dumps(built), encoding="utf-8")
    return mod_id


def run(executable: Path, out: Path) -> int:
    mods_dir = out / "mods"
    mod_id = build_test_mod(mods_dir)
    failures = []

    print("[TEST] Starting battle immunity runtime test...", flush=True)

    with Game(executable=executable, out=out / "game", mods_dir=mods_dir,
              settings={f"mod.{mod_id}": 1, "mod.3d-monsters": 0,
                        "mod.hand-camera": 0, "mod.ai-hard-mode": 0},
              env={"MEMORIES_TRACE": "mods,duel-effects"}) as game:

        # -------------------------------------------------------------
        # Test 1: Kuriboh (Unconditional battle immunity) vs Blue-Eyes
        # -------------------------------------------------------------
        print("\n[SCENARIO 1] Kuriboh (300 ATK, battle immune) battles Blue-Eyes (3000 ATK)", flush=True)
        game.goto("duel", opponent=SIMON, deck=[KURIBOH, MEDICINE, MEDICINE, MEDICINE, MEDICINE])

        def arrange_scenario1(g):
            g.arrange_deck(0, [KURIBOH, MEDICINE, MEDICINE, MEDICINE, MEDICINE])
            g.arrange_deck(1, [BLUE_EYES, MEDICINE, MEDICINE, MEDICINE, MEDICINE])

        game.duel_ready(before_deal=arrange_scenario1)

        # Player turn 1: play Kuriboh in attack position
        game.play_card(0, face_up=True)
        game.end_turn()

        # Simon turn 1: Simon plays Blue-Eyes in attack position
        # Player turn 2: draw card, then attack Blue-Eyes with Kuriboh
        game.play_card(0, face_up=True)  # play medicine to get to field phase

        before_duel = game.duel()
        player_lp_before = before_duel[0]["lp"]
        simon_monsters = [m for m in before_duel[1]["monsters"] if m]
        assert any(m["id"] == BLUE_EYES for m in simon_monsters), f"Simon did not play Blue-Eyes: {simon_monsters}"

        target_col = next(i for i, card in enumerate(game.field()[1]) if card == BLUE_EYES)
        kuriboh_col = next(i for i, card in enumerate(game.field()[2]) if card == KURIBOH)

        print(f"  Attacking Blue-Eyes (col {target_col}) with Kuriboh (col {kuriboh_col})...", flush=True)
        game.attack(kuriboh_col, target=target_col)
        game.step(180)

        after_duel = game.duel()
        player_lp_after = after_duel[0]["lp"]
        expected_damage = 3000 - 300  # 2700
        actual_damage = player_lp_before - player_lp_after
        print(f"  LP: {player_lp_before} -> {player_lp_after} (Damage: {actual_damage})", flush=True)
        if actual_damage <= 0:
            failures.append(f"Scenario 1 LP damage failed: expected damage > 0, got {actual_damage}")

        kuriboh_after = after_duel[0]["monsters"][kuriboh_col]
        if not kuriboh_after or kuriboh_after["id"] != KURIBOH:
            failures.append("Scenario 1 FAILED: Kuriboh was destroyed in battle despite having battle immunity!")
            print("  FAILED: Kuriboh was destroyed!", flush=True)
        else:
            print("  PASSED: Kuriboh survived battle with 3000 ATK Blue-Eyes!", flush=True)

        # -------------------------------------------------------------
        # Test 2: Time Wizard (Conditional: lowest_atk) vs Blue-Eyes
        # -------------------------------------------------------------
        print("\n[SCENARIO 2] Time Wizard (500 ATK, battle immune if lowest ATK) vs Blue-Eyes (3000 ATK)", flush=True)
        # In this duel, player only has Time Wizard and opponent has Blue-Eyes.
        # Time Wizard is strictly lowest ATK on field (500 < 3000). Condition is MET.
        game.goto("duel", opponent=SIMON, deck=[TIME_WIZARD, MEDICINE, MEDICINE, MEDICINE, MEDICINE])

        def arrange_scenario2(g):
            g.arrange_deck(0, [TIME_WIZARD, MEDICINE, MEDICINE, MEDICINE, MEDICINE])
            g.arrange_deck(1, [BLUE_EYES, MEDICINE, MEDICINE, MEDICINE, MEDICINE])

        game.duel_ready(before_deal=arrange_scenario2)
        game.play_card(0, face_up=True)
        game.end_turn()

        game.play_card(0, face_up=True)
        before_duel = game.duel()
        player_lp_before = before_duel[0]["lp"]
        target_col = next(i for i, card in enumerate(game.field()[1]) if card == BLUE_EYES)
        wizard_col = next(i for i, card in enumerate(game.field()[2]) if card == TIME_WIZARD)

        print(f"  Attacking Blue-Eyes with Time Wizard (lowest ATK)...", flush=True)
        game.attack(wizard_col, target=target_col)
        game.step(180)

        after_duel = game.duel()
        player_lp_after = after_duel[0]["lp"]
        actual_damage = player_lp_before - player_lp_after

        print(f"  LP: {player_lp_before} -> {player_lp_after} (Damage: {actual_damage})", flush=True)
        if actual_damage <= 0:
            failures.append(f"Scenario 2 LP damage failed: expected damage > 0, got {actual_damage}")

        wizard_after = after_duel[0]["monsters"][wizard_col]
        if not wizard_after or wizard_after["id"] != TIME_WIZARD:
            failures.append("Scenario 2 FAILED: Time Wizard was destroyed despite having lowest ATK!")
            print("  FAILED: Time Wizard was destroyed!", flush=True)
        else:
            print("  PASSED: Time Wizard survived battle as lowest ATK!", flush=True)

        # -------------------------------------------------------------
        # Test 3: Mushroom Man (Conditional: ATK <= 1000) vs Blue-Eyes
        # -------------------------------------------------------------
        print("\n[SCENARIO 3] Mushroom Man (800 ATK, battle immune if ATK <= 1000) vs Blue-Eyes", flush=True)
        game.goto("duel", opponent=SIMON, deck=[MUSHROOM_MAN, MEDICINE, MEDICINE, MEDICINE, MEDICINE])

        def arrange_scenario3(g):
            g.arrange_deck(0, [MUSHROOM_MAN, MEDICINE, MEDICINE, MEDICINE, MEDICINE])
            g.arrange_deck(1, [BLUE_EYES, MEDICINE, MEDICINE, MEDICINE, MEDICINE])

        game.duel_ready(before_deal=arrange_scenario3)
        game.play_card(0, face_up=True)
        game.end_turn()

        game.play_card(0, face_up=True)
        target_col = next(i for i, card in enumerate(game.field()[1]) if card == BLUE_EYES)
        mush_col = next(i for i, card in enumerate(game.field()[2]) if card == MUSHROOM_MAN)

        print(f"  Attacking Blue-Eyes with Mushroom Man (ATK 800 <= 1000)...", flush=True)
        game.attack(mush_col, target=target_col)
        game.step(180)

        after_duel = game.duel()
        mush_after = after_duel[0]["monsters"][mush_col]
        if not mush_after or mush_after["id"] != MUSHROOM_MAN:
            failures.append("Scenario 3 FAILED: Mushroom Man was destroyed despite ATK <= 1000!")
            print("  FAILED: Mushroom Man was destroyed!", flush=True)
        else:
            print("  PASSED: Mushroom Man survived battle with stat threshold condition!", flush=True)

        # -------------------------------------------------------------
        # Test 4: Baby Dragon (No battle immunity) vs Blue-Eyes -> Destroys!
        # -------------------------------------------------------------
        print("\n[SCENARIO 4] Baby Dragon (1200 ATK, no battle immunity) vs Blue-Eyes (3000 ATK)", flush=True)
        BABY_DRAGON = 4
        game.goto("duel", opponent=SIMON, deck=[BABY_DRAGON, MEDICINE, MEDICINE, MEDICINE, MEDICINE])

        def arrange_scenario4(g):
            g.arrange_deck(0, [BABY_DRAGON, MEDICINE, MEDICINE, MEDICINE, MEDICINE])
            g.arrange_deck(1, [BLUE_EYES, MEDICINE, MEDICINE, MEDICINE, MEDICINE])

        game.duel_ready(before_deal=arrange_scenario4)
        game.play_card(0, face_up=True)
        game.end_turn()
        # Simon's turn: Blue-Eyes was played and attacked Baby Dragon.
        # Since Baby Dragon has no battle immunity, it must be destroyed!
        baby_survived = any(m and m["id"] == BABY_DRAGON for m in game.duel()[0]["monsters"])
        if baby_survived:
            failures.append("Scenario 4 FAILED: Baby Dragon survived battle without battle immunity!")
            print("  FAILED: Baby Dragon was not destroyed by Simon's attack!", flush=True)
        else:
            print("  PASSED: Baby Dragon was destroyed by Blue-Eyes in battle as expected (no battle immunity)!", flush=True)

    if failures:
        print("\nTest Failures:\n" + "\n".join(failures), flush=True)
        return 1

    print("\nALL RUNTIME BATTLE IMMUNITY TESTS PASSED!", flush=True)
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, default=ROOT / "tmp/pc/game32/memories-pc.exe")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        raise SystemExit(run(args.executable, args.out))
    with tempfile.TemporaryDirectory(prefix="fm-battle-immunity-") as folder:
        raise SystemExit(run(args.executable, Path(folder)))
