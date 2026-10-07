#!/usr/bin/env python3
"""The art window of a full-bleed card frame, measured, and the art rect checked against it.

A "card_layout" frame PNG has a see-through hole where the card's art shows.
The art quad is drawn OVER the frame (src/game/func_80028B08.c), so its
rect, not the PNG's hole, sets where the visible border begins -- but the
hole is the area the art has to cover, or the retail gold plate under the
frame shows through.

The game does not see the PNG's own pixels: src/pc/cards/card_layout_art.c
squashes it to FRAME_TEXELS and src/pc/cards/art.c's CardArt_IndexedImage
clears every texel under half covered. The window here is measured the same
way (same integer bins, same half rule), in card pixels (CARD_SIZE), so
"covered" means covered as the game draws it.

Parameters (everything the placement depends on):
  CARD_SIZE        the frame's on-screen size in card pixels (manifest
                   card_layout.frame.<kind>.width/height)
  FRAME_TEXELS     the VRAM texture the game squashes any frame PNG to (3x3
                   tiles of 177x254); keep in sync with FRAME_W/FRAME_H in
                   card_layout_art.c
  MAX_OVERLAP      how far the art may reach past the window onto the frame's
                   border before a check fails (card pixels)
  MAX_ASYMMETRY    how much thicker the left border may be than the right
                   (visible border = art start vs. card edge)

    python tools/pc/card_frame_window.py <mod folder with mod.json>
"""
import json
import math
import os
import sys

import numpy as np
from PIL import Image

CARD_SIZE = (140, 196)
FRAME_TEXELS = (531, 762)
MAX_OVERLAP = 1.5
MAX_ASYMMETRY = 1.0


def _clear_texels(alpha):
    """The FRAME_TEXELS mask of texels the game treats as clear (entry 0)."""
    tw, th = FRAME_TEXELS
    sh, sw = alpha.shape
    tops = [sh * y // th for y in range(th)]
    bottoms = [max(sh * (y + 1) // th, tops[y] + 1) for y in range(th)]
    lefts = [sw * x // tw for x in range(tw)]
    rights = [max(sw * (x + 1) // tw, lefts[x] + 1) for x in range(tw)]
    a = alpha.astype(np.int64)
    rows = np.add.reduceat(a, tops, axis=0)   # bins are contiguous: bottom == next top
    cells = np.add.reduceat(rows, lefts, axis=1)
    area = np.outer(np.array(bottoms) - tops, np.array(rights) - lefts)
    # the last bin can end past the next start only if bins overlap; they do not
    return cells * 2 < area * 255


def measure_window(png):
    """(x0, y0, x1, y1) of the frame's art hole in card pixels, as the game draws it."""
    alpha = np.array(Image.open(png).convert("RGBA"))[:, :, 3]
    clear = _clear_texels(alpha)
    th, tw = clear.shape
    cx, cy = tw // 2, th // 3
    if not clear[cy, cx]:
        raise ValueError(f"{png}: no see-through window at the card's upper middle")
    x0 = x1 = cx
    while x0 > 0 and clear[cy, x0 - 1]:
        x0 -= 1
    while x1 < tw and clear[cy, x1]:
        x1 += 1
    y0 = y1 = cy
    while y0 > 0 and clear[y0 - 1, cx]:
        y0 -= 1
    while y1 < th and clear[y1, cx]:
        y1 += 1
    sx, sy = CARD_SIZE[0] / tw, CARD_SIZE[1] / th
    return (x0 * sx, y0 * sy, x1 * sx, y1 * sy)


def art_rect(windows):
    """The smallest whole-pixel art rect covering every window given."""
    x0 = math.floor(min(w[0] for w in windows) + 1e-6)
    y0 = math.floor(min(w[1] for w in windows) + 1e-6)
    x1 = math.ceil(max(w[2] for w in windows) - 1e-6)
    y1 = math.ceil(max(w[3] for w in windows) - 1e-6)
    return {"x": x0, "y": y0, "width": x1 - x0, "height": y1 - y0}


def check(name, art, window):
    """Problems (a list of strings) with an art rect against one window."""
    ax0, ay0 = art["x"], art["y"]
    ax1, ay1 = ax0 + art["width"], ay0 + art["height"]
    wx0, wy0, wx1, wy1 = window
    problems = []
    for side, gap in (("left", wx0 - ax0), ("top", wy0 - ay0), ("right", ax1 - wx1), ("bottom", ay1 - wy1)):
        if gap < -1e-6:
            problems.append(f"{name}: art leaves {-gap:.2f}px of the {side} of the window uncovered (retail plate shows)")
        elif gap > MAX_OVERLAP:
            problems.append(f"{name}: art reaches {gap:.2f}px past the window's {side} (more than {MAX_OVERLAP})")
    left, right = ax0, CARD_SIZE[0] - ax1
    if abs(left - right) > MAX_ASYMMETRY:
        problems.append(f"{name}: visible border is {left}px left but {right}px right")
    return problems


def describe(name, art, window):
    ax1, ay1 = art["x"] + art["width"], art["y"] + art["height"]
    return (f"{name}: window x {window[0]:.2f}..{window[2]:.2f} y {window[1]:.2f}..{window[3]:.2f}; "
            f"art x {art['x']}..{ax1} y {art['y']}..{ay1}; "
            f"borders L{art['x']} R{CARD_SIZE[0] - ax1} T{art['y']}")


def verify_manifest(mod_dir):
    """Check a built mod's card_layout against its own frame PNGs; returns the problems."""
    manifest = json.load(open(os.path.join(mod_dir, "mod.json"), encoding="utf-8"))
    layout = manifest.get("card_layout") or {}
    problems = []
    for kind, frame in (layout.get("frame") or {}).items():
        art = (layout.get("spell") or {}).get("art") if kind in ("magic", "trap", "ritual") else layout.get("art")
        if not art:
            problems.append(f"{kind}: no art rect in card_layout")
            continue
        window = measure_window(os.path.join(mod_dir, frame["image"]))
        print(describe(kind, art, window))
        problems += check(kind, art, window)
    return problems


if __name__ == "__main__":
    bad = verify_manifest(sys.argv[1] if len(sys.argv) > 1 else ".")
    for line in bad:
        print("FAIL", line)
    print("art placement OK" if not bad else f"{len(bad)} problem(s)")
    sys.exit(1 if bad else 0)
