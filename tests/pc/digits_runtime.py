#!/usr/bin/env python3
"""A style's "digits" strip on retail's own frame (notes/modding.md, "Frame
styles"), seen in the built game's card viewer.

A frame style that keeps retail's frame (no "image") and gives a digit strip
has the ATK/DFD numbers drawn from it, centred on the row retail's digits
fill, and retail's digits are not drawn under it (src/game/func_80028B08.c).
The cases below read that off the game's frame, the Library's card viewer
with the first card (the area is the numbers', right of the captions):

- no mod: retail's digits; a style whose `digits` hides both numbers: the
  plate alone;
- a see-through strip: the plate alone (the strip drew, and retail's digits
  were left out: were they drawn too, they would show);
- a bright strip: not retail's, not the plate alone (it is on the screen);
- a strip that is not a PNG: retail's digits (the numbers stay when the
  strip does not load);
- the mod's `full_bleed` setting off: retail's digits, whatever the style says.

It fails on a game that draws a strip only on a full-bleed frame (retail's
digits stay, and the see-through and bright strips show them). Needs the
player's disc and a built native game; uses folders of its own.

    python3 tests/pc/digits_runtime.py [--executable PATH] [--out DIR]
"""
import argparse
import json
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/pc"))
from fm_editor import pngio  # noqa: E402
from yfm_control import Game  # noqa: E402

OUT = ROOT / "tmp/pc/digits-runtime"
AREA = (94, 158, 32, 30)      # x, y, w, h of the frame: both numbers, not the captions
CHEST = bytes([3]) * 722      # every card, so the Library lists the first


def strip(path, colour):
    """A 256x256 digit strip of one colour (alpha 0: see-through)."""
    pngio.write(path, pngio.Image(256, 256, bytes(colour) * (256 * 256)))


def make_mod(folder, name, digits, images):
    """A mod `name` whose one style keeps retail's frame (no "image") and has `digits`."""
    mod = folder / name
    mod.mkdir(parents=True)
    for file, make in images.items():
        make(mod / file)
    style = {"hand_color": "gold", "digits": digits}
    manifest = {"id": name, "name": name, "version": "1", "min_api": 25,
                "settings": [{"key": "full_bleed", "label": "Use the layout", "type": "bool", "default": 1}],
                "card_layout": {"frame_styles": {"s": style}, "default_style": "s"}}
    (mod / "mod.json").write_text(json.dumps(manifest), encoding="utf-8")
    return name


def frame_area(executable, out, mods, mod=None, setting=1):
    """The numbers' area of the Library's card viewer, with `mod` of the folder `mods` on."""
    env = {"MEMORIES_MODS": "0"} if mod is None else {
        "MEMORIES_MOD_" + mod.upper().replace("-", "_"): "1",
        "MEMORIES_MOD_" + mod.upper().replace("-", "_") + "_FULL_BLEED": str(setting)}
    with Game(executable=executable, out=out / (mod or "retail") / str(setting), env=env, mods_dir=mods) as game:
        game.goto("title")
        game.step(30)
        game.poke("gLibrary_abCardChest", CHEST)
        game.goto("library")
        game.step(120)
        game.press("cross")                  # the card viewer
        game.step(90)
        shot = pngio.read(game.shot("viewer.png"))
    return pngio.crop(shot, *AREA)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--executable")
    parser.add_argument("--out", type=Path, default=OUT)
    options = parser.parse_args()
    shutil.rmtree(options.out, ignore_errors=True)
    mods = options.out / "mods"
    mods.mkdir(parents=True)
    clear = lambda path: strip(path, (0, 0, 0, 0))                    # noqa: E731
    bright = lambda path: strip(path, (255, 255, 255, 255))           # noqa: E731
    broken = lambda path: path.write_bytes(b"this is not a PNG")      # noqa: E731
    digits = {"image": "digits.png", "width": 6, "height": 12}
    names = {
        "hidden": make_mod(mods, "hidden", {"atk": {"hide": True}, "def": {"hide": True}}, {}),
        "clear": make_mod(mods, "clear", digits, {"digits.png": clear}),
        "bright": make_mod(mods, "bright", digits, {"digits.png": bright}),
        "broken": make_mod(mods, "broken", digits, {"digits.png": broken}),
    }
    run = lambda mod=None, setting=1: frame_area(options.executable, options.out, mods, mod, setting)  # noqa: E731
    retail = run()
    plate = run(names["hidden"])
    results = [
        ("retail's digits differ from the plate alone (the test sees them)", retail != plate),
        ("a see-through strip: plate alone, retail's digits left out", run(names["clear"]) == plate),
        ("a bright strip is drawn", (lambda shown: shown != retail and shown != plate)(run(names["bright"]))),
        ("a strip that is not a PNG: retail's digits", run(names["broken"]) == retail),
        ("the setting off: retail's digits", run(names["clear"], 0) == retail),
    ]
    for what, ok in results:
        print(("ok    " if ok else "FAIL  ") + what)
    if not all(ok for _, ok in results):
        sys.exit(1)
    print("digits runtime: ok")


if __name__ == "__main__":
    main()
