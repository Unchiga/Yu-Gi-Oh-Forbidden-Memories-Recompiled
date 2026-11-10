#!/usr/bin/env python3
"""A style's ATK/DFD caption picture on retail's own frame (notes/modding.md,
"Frame styles"), seen in the built game's card viewer.

Retail draws its "ATK"/"DFD" captions as sprites of their own
(src/game/func_80028B08.c). A frame style that keeps retail's frame and gives
an `atk_label`/`dfd_label` picture replaces them: the picture is drawn and
retail's sprite is not submitted under it. The cases below read that off the
game's frame, the Library's card viewer with the first card (the area is the
captions' own, left of the digits):

- no mod: retail's captions; a style with both labels `hide`: the plate alone;
- a see-through picture: the plate alone (the picture drew, and retail's
  sprite was left out: were it drawn too, its letters would show), and a
  picture with `hide` the same;
- a bright picture: not retail's, not the plate alone (it is on the screen);
- a picture that is not a PNG: retail's captions (the caption stays when the
  picture does not load);
- the mod's `full_bleed` setting off: retail's captions, whatever the style says.

It fails on a game without the skip (a picture over retail's caption, or
retail's caption left in) and on one that draws the picture but ignores that
it did. Needs the player's disc and a built native game; uses folders of its
own.

    python3 tests/pc/stat_label_runtime.py [--executable PATH] [--out DIR]
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

OUT = ROOT / "tmp/pc/stat-label"
AREA = (68, 160, 30, 30)      # x, y, w, h of the frame: both captions, not the digits
CHEST = bytes([3]) * 722      # every card, so the Library lists the first


def picture(path, colour):
    """A 64x32 PNG of one colour (alpha 0: see-through)."""
    pngio.write(path, pngio.Image(64, 32, bytes(colour) * (64 * 32)))


def make_mod(folder, name, labels, images):
    """A mod `name` whose one style keeps retail's frame (no "image") and has `labels`."""
    mod = folder / name
    mod.mkdir(parents=True)
    for file, make in images.items():
        make(mod / file)
    style = {"hand_color": "gold", **labels}
    manifest = {"id": name, "name": name, "version": "1", "min_api": 25,
                "settings": [{"key": "full_bleed", "label": "Use the layout", "type": "bool", "default": 1}],
                "card_layout": {"frame_styles": {"s": style}, "default_style": "s"}}
    (mod / "mod.json").write_text(json.dumps(manifest), encoding="utf-8")
    return name


def frame_area(executable, out, mods, mod=None, setting=1):
    """The caption area of the Library's card viewer, with `mod` of the folder `mods` on."""
    env = {"MEMORIES_MODS": "0"} if mod is None else {
        "MEMORIES_MOD_" + mod.upper().replace("-", "_"): "1",
        "MEMORIES_MOD_" + mod.upper().replace("-", "_") + "_FULL_BLEED": str(setting)}
    with Game(executable=executable, out=out / (mod or "retail"), env=env, mods_dir=mods) as game:
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
    clear = lambda path: picture(path, (0, 0, 0, 0))                  # noqa: E731
    bright = lambda path: picture(path, (255, 255, 255, 255))         # noqa: E731
    broken = lambda path: path.write_bytes(b"this is not a PNG")      # noqa: E731

    def labels(**extra):
        return {key: {"image": f"{key}.png", "x": 72, "y": 158 if key == "atk_label" else 173,
                      "width": 21, "height": 8, **extra} for key in ("atk_label", "dfd_label")}

    pictures = lambda make: {"atk_label.png": make, "dfd_label.png": make}   # noqa: E731
    names = {
        "hidden": make_mod(mods, "hidden", {"atk_label": {"hide": True}, "dfd_label": {"hide": True}}, {}),
        "clear": make_mod(mods, "clear", labels(), pictures(clear)),
        "clear-hidden": make_mod(mods, "clear-hidden", labels(hide=True), pictures(clear)),
        "bright": make_mod(mods, "bright", labels(), pictures(bright)),
        "broken": make_mod(mods, "broken", labels(), pictures(broken)),
    }
    run = lambda mod=None, setting=1: frame_area(options.executable, options.out, mods, mod, setting)  # noqa: E731
    retail = run()
    plate = run(names["hidden"])
    results = [
        ("retail's captions differ from the plate alone (the test sees them)", retail != plate),
        ("a see-through picture: plate alone, retail's captions left out", run(names["clear"]) == plate),
        ("a picture with hide: plate alone", run(names["clear-hidden"]) == plate),
        ("a bright picture is drawn", (lambda shown: shown != retail and shown != plate)(run(names["bright"]))),
        ("a picture that is not a PNG: retail's captions", run(names["broken"]) == retail),
        ("the setting off: retail's captions", run(names["clear"], 0) == retail),
    ]
    for what, ok in results:
        print(("ok    " if ok else "FAIL  ") + what)
    if not all(ok for _, ok in results):
        sys.exit(1)
    print("stat label runtime: ok")


if __name__ == "__main__":
    main()
