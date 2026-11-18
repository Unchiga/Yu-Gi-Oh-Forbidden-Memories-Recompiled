#!/usr/bin/env python3
"""Every reader of VRAM's rastered words settles what it reads.

The software GPU may keep a frame's primitives back from the raster (the
deferred raster: notes/pc-build.md, "Settling VRAM before it is read"), so a
reader asks for the words it reads to be drawn first: SoftGpu_Vram() settles
all of VRAM, SoftGpu_Settle() and SoftGpu_SettleTexture() a rectangle, and
SoftGpu_VramRaw() settles nothing. Readers inside soft_gpu.c are covered by
tests/pc/soft_gpu_settle_test.c; this reads the sources for the rest, so it
runs on any host: each function that takes the raw pointer is listed below,
either as one that settles what it reads (it must call a settle in its own
body) or with the reason it needs none. A new raw reader fails until it is
listed. VRAM is never written through either pointer.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from audit import mask_noncode  # noqa: E402

SETTLES = "settles"
# (file, function): SETTLES, or why it reads nothing a primitive draws.
RAW_READERS = {
    ("src/pc/sdk/libgpu.c", "display_words"): SETTLES,
    ("src/pc/sdk/libgpu.c", "displayed_picture"): SETTLES,
    ("src/pc/render/gl_picture.c", "upload_vram"):
        "the OpenGL picture's copy of VRAM, for what its primitives sample: it draws the drawing areas itself, "
        "the game textures from them only through a move (which settles its source), and a resync settles first",
    ("src/pc/render/texture_dump.c", "TextureDump_Primitive"):
        "compares the pointer only; soft_gpu.c settles the primitive's texture before calling",
    ("src/pc/text/hd_text.c", "HdText_Cell"): SETTLES,
    ("src/pc/text/hd_text.c", "HdText_TitleUploaded"): SETTLES,
    ("src/pc/text/hd_text.c", "HdText_TitleCard"): SETTLES,
    ("src/pc/text/hd_text.c", "HdText_NameBox"): SETTLES,
    ("src/pc/text/hd_text.c", "HdText_FieldWord"): SETTLES,
    ("src/pc/text/hd_text.c", "HdText_Hud"): SETTLES,
    ("src/pc/text/glyphs.c", "read_cell"): SETTLES,
    ("src/pc/text/glyphs.c", "copy_palettes"): SETTLES,
    ("src/pc/cards/star_icons.c", "star_palette"): SETTLES,
    ("src/pc/cards/star_icons.c", "plain_disc"): SETTLES,
    ("src/pc/cards/star_icons.c", "Stars_IconCell"): SETTLES,
    ("src/pc/cards/card_layout_art.c", "CardLayoutArt_TitleCell"): SETTLES,
    ("src/pc/cards/battle_models.c", "textures"): SETTLES,
    ("src/pc/platform/menu_letters.c", "texel"):
        "per texel; its caller settles each sprite's texture (SoftGpu_SettleTexture) before the loop",
}
# Functions that read VRAM whole through a pointer taken elsewhere: they must
# settle it themselves (upload_vram's exemption above rests on this one).
MUST_SETTLE = {
    ("src/pc/render/gl_picture.c", "resync"): "uploads VRAM whole and redraws the picture from it",
}
SETTLE = re.compile(r"\bSoftGpu_(Settle|SettleTexture|Vram)\s*\(")
RAW = re.compile(r"\bSoftGpu_VramRaw\s*\(")
WRITE = re.compile(r"\(\s*(?:u?int16_t|u16|unsigned\s+short|void)\s*\*\s*\)\s*&?\s*SoftGpu_Vram(?:Raw)?\s*\(")
FUNCTION = re.compile(r"(\w+)\s*\([^;{}]*\)\s*$")


def functions(code: str):
    """Top-level function bodies: (name, first line, body text)."""
    depth, start, header_end = 0, None, 0
    for at, char in enumerate(code):
        if char == "{":
            if depth == 0:
                start = at
                header = code[header_end:at]
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0 and start is not None:
                match = FUNCTION.search(header.rstrip())
                if match and not header.rstrip().endswith("="):
                    yield match[1], code.count("\n", 0, start) + 1, code[start:at + 1]
                start, header_end = None, at + 1
        elif char == ";" and depth == 0:
            header_end = at + 1


def main() -> int:
    units = sorted(p for base in ("src", "mods") for p in (ROOT / base).rglob("*") if p.suffix in (".c", ".h"))
    problems, found = [], set()
    for unit in units:
        relative = unit.relative_to(ROOT).as_posix()
        if relative in ("src/pc/render/soft_gpu.c", "src/pc/render/soft_gpu.h"):
            continue
        code = mask_noncode(unit.read_text(encoding="utf-8", errors="replace"))
        for match in WRITE.finditer(code):
            problems.append(f"{relative}:{code.count(chr(10), 0, match.start()) + 1}: VRAM written through "
                            "SoftGpu_Vram's pointer; write with SoftGpu_Load/Fill/Move")
        for name, line, body in functions(code):
            if (relative, name) in MUST_SETTLE:
                found.add((relative, name))
                if not SETTLE.search(body):
                    problems.append(f"{relative}:{line}: {name}() {MUST_SETTLE[(relative, name)]} but settles nothing")
        if not RAW.search(code):
            continue
        for name, line, body in functions(code):
            if not RAW.search(body):
                continue
            key = (relative, name)
            found.add(key)
            why = RAW_READERS.get(key)
            if why is None:
                problems.append(f"{relative}:{line}: {name}() takes SoftGpu_VramRaw() and is not listed in "
                                "tools/pc/check_vram_readers.py: settle what it reads, or say why it needs not")
            elif why == SETTLES and not SETTLE.search(body):
                problems.append(f"{relative}:{line}: {name}() takes SoftGpu_VramRaw() but settles nothing")
    for key in sorted((set(RAW_READERS) | set(MUST_SETTLE)) - found):
        problems.append(f"{key[0]}: {key[1]}() is listed but takes no SoftGpu_VramRaw() any more")
    for problem in problems:
        print(problem)
    if problems:
        print(f"{len(problems)} VRAM reader problem(s) (notes/pc-build.md, \"Settling VRAM before it is read\")")
        return 1
    print(f"{len(found)} VRAM readers outside soft_gpu.c, each settling what it reads or saying why it needs not")
    return 0


if __name__ == "__main__":
    sys.exit(main())
