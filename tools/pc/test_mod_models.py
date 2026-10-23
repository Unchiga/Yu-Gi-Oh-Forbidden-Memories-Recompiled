#!/usr/bin/env python3
"""Disc-backed check of the battle models a mod adds ("models", API 21).

A mod gets one more MODEL.MRG record, copied from the player's disc at test
time (Dark Magician's: nothing of the disc is kept in the repository), and
gives it to Blue-eyes White Dragon. Three runs of one duel, the player
playing Blue-eyes face up and attacking with it:

    mod     Blue-eyes takes the mod's model ("model": "test-model")
    disc    Blue-eyes takes Dark Magician's own ("model": "Dark Magician")
    none    Blue-eyes as it is
    changed as mod, but the model's file changes once the game is up: the
            battle does not hang, and loads the model's base (Dark Magician);
            3D Monsters does not load it (a changed record crashed its parse)

The mod's record is read from its file: by 3D Monsters directly, and by the
battle through the slot's window past the disc. So in mod and disc 3D
Monsters must stand the same model on the field (the same picture once it
has landed), and after the attack the battle's model slot must hold the same
model data and the fight look the same, frame for frame from when the
record is in (the window lies further on the simulated drive than the disc's
record, so it arrives a few frames later); none must differ from both.

    python tools/pc/test_mod_models.py --binary tmp/pc/win64/memories-pc.exe --disc game/disc.bin
"""
import argparse
import json
import re
import shutil
import struct
from pathlib import Path

import importlib.util

from yfm_control import Game

ROOT = Path(__file__).resolve().parents[2]
RECORD_SECTORS = 0x114
DARK_MAGICIAN = 35                  # card 35: record 34, below the first gap
MOD_MODEL_CARD = 0x8000 + 1         # the first mod model, as Cards_ModelId gives it
SLOTS = "D_800F2C40"                # the two battle model slots (src/game/model.h)
SLOT_SIZE, SLOT_MODEL = 0xE20, 0xDF8
SLOT_DATA = (0x80100000, 0x80140000)   # where a record's 96 model sectors go
MODEL_DATA = 96 * 2048
SESSION_PATH = ROOT / "tests/pc/replays/full-duel/session.py"
_spec = importlib.util.spec_from_file_location("full_duel_session", SESSION_PATH)
SESSION = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(SESSION)
LOADED = re.compile(r"card (\d+) stance 0 loaded in \d+ us: (\d+) units, (\d+) parts")


def iso_file(disc: Path, path: list[str]) -> tuple[int, int]:
    """(lba, size) of a file on a raw 2352-byte-sector disc image."""
    with disc.open("rb") as image:
        def sector(lba: int) -> bytes:
            image.seek(lba * 2352 + 24)
            return image.read(2048)
        root = sector(16)[156:156 + 34]
        lba, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
        for name in path:
            found = None
            for n in range((size + 2047) // 2048):
                data, at = sector(lba + n), 0
                while at < 2048 and data[at]:
                    entry = data[at:at + data[at]]
                    if entry[33:33 + entry[32]].decode("latin-1").upper() == name:
                        found = struct.unpack_from("<I", entry, 2)[0], struct.unpack_from("<I", entry, 10)[0]
                    at += data[at]
            if not found:
                raise SystemExit(f"{name} is not on {disc}")
            lba, size = found
        return lba, size


def disc_record(disc: Path, card: int) -> bytes:
    lba, _ = iso_file(disc, ["DATA", "MODEL.MRG;1"])
    record = card - 1
    record -= (record >= 721) + 50 * (record >= 700) + 50 * (record >= 350)
    with disc.open("rb") as image:
        sectors = []
        for n in range(RECORD_SECTORS):
            image.seek((lba + record * RECORD_SECTORS + n) * 2352 + 24)
            sectors.append(image.read(2048))
    return b"".join(sectors)


def write_mod(mods: Path, kind: str, record: bytes) -> None:
    folder = mods / "test-models"
    folder.mkdir(parents=True)
    manifest = {"id": "test-models", "name": "Test models", "enabled": True, "min_api": 21}
    if kind in ("mod", "changed"):
        (folder / "models").mkdir()
        (folder / "models" / "test.mrg").write_bytes(record)
        manifest["models"] = [{"name": "test-model", "file": "models/test.mrg", "base": "Dark Magician"}]
        manifest["cards"] = [{"replace": 1, "model": "test-model"}]
    else:
        manifest["cards"] = [{"replace": 1, "model": "Dark Magician"}]
    (folder / "mod.json").write_text(json.dumps(manifest, indent=2) + "\n")


def run(binary: Path, disc: Path, out: Path, kind: str, record: bytes) -> dict:
    mods = out / "mods"
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(binary.parent / "mods", mods)   # the shipped mods, 3D Monsters among them
    if kind != "none":
        write_mod(mods, kind, record)
    result = {}
    with Game(binary, out=out, mods_dir=mods,
              settings={"mod.3d-monsters": 1, "mod.hand-camera": 0, "mod.ai-hard-mode": 0,
                        "mod.yamyi-mods": 0, "mod.drop-missing-cards": 0, "mod.test-models": 1},
              env={"MEMORIES_DISC": str(disc), "MEMORIES_TRACE": "mods", "MEMORIES_SPEED": "-1"}) as game:
        game.wait_until(lambda g: g.resident("main_menu"), 3000)
        if kind == "changed":   # one byte, as a mod updated in place would
            changed = bytearray(record)
            changed[1000] ^= 0xFF
            (mods / "test-models" / "models" / "test.mrg").write_bytes(changed)
        game.goto("duel", opponent=1, deck="1")
        game.duel_ready()
        game.play_card(0, face_up=True)
        game.end_turn()
        if game.duel_over():
            raise SystemExit(f"{kind}: the duel ended before the attack")
        # The field's camera, as test_arm64_3d_monsters.py takes it.
        game.press("circle", after=90)
        assert game.u16(0x800F284C) < 512, "the camera did not return above the field"
        result["field"] = game.shot("field.png").read_bytes()
        # The 3D battle (Square on a monster target, tests/pc/replays/full-duel's
        # attack_3d): the slot that takes Blue-eyes' model, once its record
        # has had a second to load.
        wanted = {"mod": MOD_MODEL_CARD - 1, "changed": MOD_MODEL_CARD - 1, "disc": DARK_MAGICIAN - 1}.get(kind)
        settle = SESSION.settle

        def watch(game, timeout=8000):
            for _ in range(150):
                game.step(4)
                for slot in (0, 1):
                    if wanted is not None and "slot" not in result and \
                            game.u16(SLOTS, slot * SLOT_SIZE + SLOT_MODEL) == wanted:
                        game.step(60)
                        result["slot"], result["data"] = slot, game.peek(SLOT_DATA[slot], MODEL_DATA)
                        for shot in range(14):   # the fight, a second apart: the monster is in by the twelfth
                            game.step(60)
                            result.setdefault("battle", []).append(game.shot(f"battle-{shot + 1}.png").read_bytes())
            settle(game, timeout)
        for _ in range(4):   # a turn with a monster of the opponent's to attack
            game.play_card(0, face_up=True)
            targets = [column for column, card in enumerate(game.field()[1]) if card]
            if targets:
                break
            game.end_turn()
        else:
            raise SystemExit(f"{kind}: the opponent put no monster down to attack")
        column = next(column for column, card in enumerate(game.field()[2]) if card == 1)
        SESSION.settle = watch
        SESSION.attack_3d(game, column, targets[0], animated=True)
        SESSION.settle = settle
    log = (out / "game.log").read_text(errors="replace")
    result["log"] = log
    result["loaded"] = {int(card): (int(units), int(parts)) for card, units, parts in LOADED.findall(log)}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--disc", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "tmp/mod-models-test")
    args = parser.parse_args()
    disc = args.disc.resolve()
    record = disc_record(disc, DARK_MAGICIAN)
    runs = {kind: run(args.binary.resolve(), disc, args.output / kind, kind, record)
            for kind in ("mod", "disc", "none", "changed")}
    mod, same, none, changed = runs["mod"], runs["disc"], runs["none"], runs["changed"]
    assert "test-models: 1 models" in mod["log"], "the test mod's model was not added"
    mine, theirs = mod["loaded"].get(MOD_MODEL_CARD), same["loaded"].get(DARK_MAGICIAN)
    assert mine and mine[0] > 0 and mine == theirs, f"3D Monsters: the mod's model {mine}, the disc's {theirs}"
    assert mod["field"] == same["field"], "the mod's model stands differently from the disc's"
    assert mod["field"] != none["field"], "the field did not change with the model"
    assert "slot" in mod, "no battle slot took the mod's model"
    assert same.get("slot") == mod["slot"], "the disc's model went to another battle slot"
    assert mod["data"] == same["data"], "the battle loaded other model data than the disc's"
    assert mod["battle"] == same["battle"], "the mod's model fights differently from the disc's"
    assert "cannot be read as it was at startup" in changed["log"], "the changed model file was read"
    assert changed.get("data") == same["data"], "a model that cannot be read did not fight as its base"
    assert f"card {MOD_MODEL_CARD} stance" not in changed["log"], "3D Monsters loaded the changed model file"
    slot = mod["slot"]
    print(f"mod models: the mod's record stands on the field and fights in battle slot {slot} as the disc's, "
          f"and a changed file fights as its base and is not drawn; "
          f"{args.output}")


if __name__ == "__main__":
    main()
