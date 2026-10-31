#!/usr/bin/env python3
"""Disc-backed check of the battle models a mod adds ("models", API 21).

A mod gets one more MODEL.MRG record, copied from the player's disc at test
time (Dark Magician's: nothing of the disc is kept in the repository), and
gives it to Blue-eyes White Dragon. Five runs of one duel, the player
playing Blue-eyes face up and attacking with it, and one of a save state:

    mod     Blue-eyes takes the mod's model ("model": "test-model")
    disc    Blue-eyes takes Dark Magician's own ("model": "Dark Magician")
    none    Blue-eyes as it is
    changed as mod, but the model's file changes once the game is up: the
            battle does not hang, and loads the model's base (Dark Magician);
            3D Monsters stands the base too (a changed record crashed its
            parse)
    big     as mod, with the model data followed by BIG_PADDING sectors of
            zeros: a model bigger than a record, whose model data is parsed
            in the slot's arena past guest RAM and whose packets go to the
            slot's own RAM (src/pc/cards/battle_models.h); it must fight as
            the disc's frame for frame, and 3D Monsters stands its base. A
            state saved mid-fight is loaded there, and in
    bigload a game just started (its arenas empty), which must fight on
            from it frame for frame as the big run did
    pages   as big, with the polygons on the record's first texture page
            moved to page 14, and the record's first two pages given as the
            model's own 14 and 15 ("texture_pages": 2; an 8-bit polygon
            reads its page and the next): the slot samples a texture bank of
            its own and must fight as the disc's frame for frame
    pagesblank  the same, its pages all zeros: it must not
    pagesload  pages' state saved mid-fight, in a game just started: its bank
            is made again from VRAM and the file, frame for frame as pages
    pagesmissing  the same polygons with no page given: the check refuses
            the model, which fights as its base (Dark Magician's record)

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
BIG_PADDING = 40                    # sectors: a 316-sector file, bigger than a record
PAGE = 16 * 2048                    # a texture page, 64x256 halfwords
SAVED_FROM = 6                      # the big run saves a state before its seventh shot
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


def page_words(record: bytes, parsed: bytes) -> list[int]:
    """Where the record's model data holds the page word of a polygon on its
    first texture page (authored column 10 of the lower row, 8 or 4 bits): a
    halfword that func_8004D134 lowered by 10 in the battle's parsed copy."""
    found = []
    for at in range(0, MODEL_DATA, 2):
        authored, = struct.unpack_from("<H", record, at)
        if authored & 0x1F == 0x1A and (authored >> 7) & 3 < 2 and \
                struct.unpack_from("<H", parsed, at)[0] == (authored - 10) & 0xFFFF:
            found.append(at)
    return found


def paged(record: bytes, words: list[int], page: bytes | None) -> bytes:
    """The record with those polygons on page 14 instead, followed by `page`;
    with none, a bigger model all the same, as big's (zeros after its data)."""
    data = bytearray(record[:MODEL_DATA])
    for at in words:
        struct.pack_into("<H", data, at, struct.unpack_from("<H", data, at)[0] + 4)
    if page is None:
        return bytes(data) + bytes(BIG_PADDING * 2048) + record[MODEL_DATA:]
    return bytes(data) + record[MODEL_DATA:] + page


def write_mod(mods: Path, kind: str, record: bytes, words: list[int] | None = None) -> None:
    folder = mods / "test-models"
    folder.mkdir(parents=True)
    manifest = {"id": "test-models", "name": "Test models", "enabled": True, "min_api": 21}
    if kind in ("mod", "changed", "big", "bigload", "pages", "pagesblank", "pagesmissing", "pagesload"):
        (folder / "models").mkdir()
        if kind in ("big", "bigload"):
            record = record[:MODEL_DATA] + bytes(BIG_PADDING * 2048) + record[MODEL_DATA:]
        elif kind.startswith("pages"):   # the record's first two pages (stage 1's first 32 sectors) as its own
            first = record[MODEL_DATA:MODEL_DATA + 2 * PAGE]
            record = paged(record, words, {"pages": first, "pagesload": first, "pagesblank": bytes(2 * PAGE)}.get(kind))
        (folder / "models" / "test.mrg").write_bytes(record)
        manifest["models"] = [{"name": "test-model", "file": "models/test.mrg", "base": "Dark Magician"}]
        if kind in ("pages", "pagesblank", "pagesload"):
            manifest["models"][0]["texture_pages"] = 2
        manifest["cards"] = [{"replace": 1, "model": "test-model"}]
    else:
        manifest["cards"] = [{"replace": 1, "model": "Dark Magician"}]
    (folder / "mod.json").write_text(json.dumps(manifest, indent=2) + "\n")


def replay(game, result: dict) -> None:
    """The fight from the big run's save state, one frame into it: the
    shots from the SAVED_FROM-th on, at the frames the run took them."""
    game.step(59)
    for shot in range(SAVED_FROM, 14):
        if shot > SAVED_FROM:
            game.step(60)
        result.setdefault("again", []).append(game.shot(f"again-{shot + 1}.png").read_bytes())


def run(binary: Path, disc: Path, out: Path, kind: str, record: bytes, state: Path | None = None,
        words: list[int] | None = None) -> dict:
    mods = out / "mods"
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(binary.parent / "mods", mods)   # the shipped mods, 3D Monsters among them
    if kind != "none":
        write_mod(mods, kind, record, words)
    result = {}
    with Game(binary, out=out, mods_dir=mods,
              settings={"mod.3d-monsters": 1, "mod.hand-camera": 0, "mod.ai-hard-mode": 0,
                        "mod.yamyi-mods": 0, "mod.drop-missing-cards": 0, "mod.test-models": 1},
              env={"MEMORIES_DISC": str(disc), "MEMORIES_TRACE": "mods", "MEMORIES_SPEED": "-1"}) as game:
        game.wait_until(lambda g: g.resident("main_menu"), 3000)
        if kind in ("bigload", "pagesload"):   # the big (pages) run's state, in a game just started
            game.load(state)
            replay(game, result)
            result["log"] = ""
            return result
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
        wanted = {"mod": MOD_MODEL_CARD - 1, "changed": MOD_MODEL_CARD - 1, "big": MOD_MODEL_CARD - 1,
                  "pages": MOD_MODEL_CARD - 1, "pagesblank": MOD_MODEL_CARD - 1, "pagesmissing": MOD_MODEL_CARD - 1,
                  "disc": DARK_MAGICIAN - 1}.get(kind)
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
                            if kind in ("big", "pages") and shot == SAVED_FROM:
                                game.save("battle.state")
                            game.step(60)
                            result.setdefault("battle", []).append(game.shot(f"battle-{shot + 1}.png").read_bytes())
                        if kind == "disc":   # parsed by now (page_words)
                            result["parsed"] = game.peek(SLOT_DATA[slot], MODEL_DATA)
                        if kind in ("big", "pages"):   # the fight again from the save state
                            game.load("battle.state")
                            replay(game, result)
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
            for kind in ("mod", "disc", "none", "changed", "big")}
    runs["bigload"] = run(args.binary.resolve(), disc, args.output / "bigload", "bigload", record,
                          (args.output / "big" / "battle.state").resolve())
    words = page_words(record, runs["disc"]["parsed"])
    assert len(words) > 100, f"only {len(words)} polygons on the record's first page were found"
    for kind in ("pages", "pagesblank", "pagesmissing"):
        runs[kind] = run(args.binary.resolve(), disc, args.output / kind, kind, record, words=words)
    runs["pagesload"] = run(args.binary.resolve(), disc, args.output / "pagesload", "pagesload", record,
                            (args.output / "pages" / "battle.state").resolve(), words)
    mod, same, none, changed, big = runs["mod"], runs["disc"], runs["none"], runs["changed"], runs["big"]
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
    assert changed["loaded"].get(MOD_MODEL_CARD) == theirs, "3D Monsters did not stand the changed model's base"
    checked = re.search(r"a bigger model, \d+ bytes of model data, \d+ parts, (\d+) bytes of packets", big["log"])
    drawn = re.search(r"battle slot \d: (\d+) bytes of packets drawn in its own RAM", big["log"])
    assert checked and int(checked[1]) > 0, "the bigger model was not taken as one"
    assert drawn and int(drawn[1]) > 0, "the bigger model's packets were not drawn in its slot's own RAM"
    assert big.get("slot") == mod["slot"], "the bigger model went to another battle slot"
    assert big["battle"] == same["battle"], "the bigger model fights differently from the disc's"
    # A state loaded fights on as from where it was saved, in the game that
    # saved it and in one just started, whose arenas held nothing: the state
    # brings them (the fight after a load is not the first run's frame for
    # frame, a disc model's alike, so a load is held to a load).
    assert runs["bigload"]["again"] == big["again"], "a save state did not bring the bigger model's arena"
    assert big["loaded"].get(MOD_MODEL_CARD) == theirs and big["field"] == same["field"],         "3D Monsters did not stand the bigger model's base"
    pages, blank, missing = runs["pages"], runs["pagesblank"], runs["pagesmissing"]
    assert "2 texture pages of its own, in texture bank 17" in pages["log"], "the model's page was not banked"
    assert pages.get("slot") == mod["slot"], "the model with a page went to another battle slot"
    assert pages["battle"] == same["battle"], "the model drawn from its own page fights differently from the disc's"
    assert blank["battle"] != same["battle"], "the model's own page is not what its polygons sample"
    assert runs["pagesload"]["again"] == pages["again"], "a save state's load did not make the model's bank again"
    assert "use texture page 14 of row 1" in missing["log"], "a polygon on a page not given was not refused"
    assert missing.get("data") == same["data"], "the model refused for its pages did not fight as its base"
    slot = mod["slot"]
    print(f"mod models: the mod's record stands on the field and fights in battle slot {slot} as the disc's, "
          f"a changed file fights and stands as its base, and the same model bigger than a record fights "
          f"as the disc's from its arena, and with {len(words)} polygons moved to a page of its own from "
          f"its own texture bank; {args.output}")


if __name__ == "__main__":
    main()
