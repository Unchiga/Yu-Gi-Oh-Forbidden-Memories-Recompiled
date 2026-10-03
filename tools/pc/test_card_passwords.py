#!/usr/bin/env python3
"""Live password purchases and viewer regression (built game + user's disc).

Creates isolated mods/saves, buys added and replaced cards, checks payment,
repeat purchases, free cards, insufficient funds, and the on-card viewer.
Runs both the normal shop and the optional card-number-passwords mod. Images
stay in --out. No player settings or saves are changed.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import tempfile

import build_mod
from yfm_control import Game

ROOT = Path(__file__).resolve().parents[2]


def buy(game, password, card, price):
    before = game.u32("gLibrary_dwStarchips")
    game.poke("gPassword_abDigits", bytes(map(int, password)))
    game.press("cross", after=160)
    assert game.u16("D_8016D4DC") == card
    assert game.u32("D_801D5608") == price
    game.shot(f"price-{card}.png")
    game.press("cross", after=450)
    assert game.u32("gLibrary_dwStarchips") == before - price
    assert game.u16(0x801D07BC) == card  # newest awarded card, SaveDataState
    assert game.u16("D_8016D424") & 31 == 0


def viewer(game, card):
    game.goto("duel", opponent=3, deck=str(card))
    game.duel_ready()
    game.press("triangle", after=180)
    assert game.u16("gDuel_wViewerCardID") == card
    box = game.u32("D_8009B250")
    assert box and game.u16(box + 0x36) == 0xFFFE  # composed password layout
    game.shot(f"viewer-{card}.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, default=ROOT / "tmp/pc/game32/memories-pc")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    out = args.out or Path(tempfile.mkdtemp(prefix="card-passwords-", dir=ROOT / "tmp/pc"))
    mods = out.resolve() / "mods"
    fixture = mods / "password-test"
    fixture.mkdir(parents=True, exist_ok=True)
    (fixture / "mod.json").write_text(json.dumps({
        "id": "password-test", "enabled": True,
        "cards": [{"replace": 58, "name": "Aurora Wing", "password": "00000723"},
                  {"replace": 59, "password": "00000726"},
                  {"copy": 58, "id": "added", "name": "Added Wing", "password": "00001723"},
                  {"copy": 58, "id": "free", "name": "Free Wing", "password": "87654321"},
                  {"copy": 58, "id": "number", "name": "Number Wing"},
                  {"copy": 58, "id": "shadowed", "name": "Shadowed Wing"}],
        "passwords": {"58": {"password": "00000723", "starchips": 100},
                      "password-test:added:1": {"starchips": 100},
                      "password-test:free:1": {"starchips": 0},
                      "password-test:number:1": {"starchips": 55}}}), encoding="utf-8")
    numbered = mods / "card-number-passwords"
    shutil.copytree(ROOT / "mods/card-number-passwords", numbered, dirs_exist_ok=True)
    build_mod.build(str(numbered), games=[str(args.executable.resolve().parent)])
    for enabled in (False, True):
        name = "number-mod" if enabled else "core"
        with Game(args.executable, out=out / name, mods_dir=mods,
                  settings={"card_passwords": 1, "mod.card-number-passwords": int(enabled)}) as game:
            game.goto("password")
            game.step(300)
            game.poke("gLibrary_dwStarchips", struct.pack("<I", 1000))
            buy(game, "00000723", 58, 100)  # explicit password beats ID 723
            buy(game, "00001723", 723, 100)
            buy(game, "00001723", 723, 100)  # added cards have no retail used flag
            buy(game, "87654321", 724, 0)
            if enabled:
                buy(game, "00000725", 725, 55)
            # Not enough starchips: the default answer is QUIT; no award/payment.
            game.poke("gLibrary_dwStarchips", struct.pack("<I", 1))
            game.poke("gPassword_abDigits", bytes(map(int, "00001723")))
            game.press("cross", after=160)
            recent = game.u16(0x801D07BC)
            game.press("cross", after=450)
            assert game.u32("gLibrary_dwStarchips") == 1 and game.u16(0x801D07BC) == recent
            viewer(game, 723)
            if enabled:
                viewer(game, 725)  # numeric policy is displayed, not just accepted
                game.goto("duel", opponent=3, deck="726")
                game.duel_ready()
                game.press("triangle", after=180)
                assert game.u16("gDuel_wViewerCardID") == 726
                box = game.u32("D_8009B250")
                assert box and game.u16(box + 0x36) != 0xFFFE
                game.shot("viewer-shadowed-726.png")  # 00000726 buys 59; never print it on 726
        print(f"card passwords: {name} purchases and viewer passed ({out / name})", flush=True)


if __name__ == "__main__":
    main()
