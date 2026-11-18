#!/usr/bin/env python3
"""Every reader of VRAM's rastered words settles what it reads.

The software GPU may keep a frame's primitives back from the raster (the
deferred raster: notes/pc-build.md, "Settling VRAM before it is read"), so a
reader asks for the words it reads to be drawn first: SoftGpu_Vram() settles
all of VRAM, SoftGpu_Settle() and SoftGpu_SettleTexture() a rectangle, and
SoftGpu_VramRaw() settles nothing. Readers inside soft_gpu.c are covered by
tests/pc/soft_gpu_settle_test.c; this reads the sources for the rest (src/,
mods/ and examples/, .c, .h and .inc), so it runs on any host:

- each function that calls SoftGpu_VramRaw() is listed below, either as one
  that settles what it reads (it must call a settle in its own body) or with
  the reason it needs none; a new raw reader fails until it is listed;
- SoftGpu_VramRaw is only ever called: not named in a macro, not taken as a
  function pointer;
- the functions in MUST_SETTLE settle before their first read;
- VRAM is never written through SoftGpu_Vram's or SoftGpu_VramRaw's pointer.

What it cannot see (notes/pc-build.md lists it too): whether a rectangle is
the right one, whether a raw reader settles before it reads (only that it
settles), a settled pointer kept and read after later drawing (the x11
backend's last frame was one), a pointer handed to another function
(Platform_Present), and soft_gpu.c itself.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from audit import mask_noncode  # noqa: E402

SETTLES = "settles"
# (file, function): SETTLES, or why it needs no settle.
RAW_READERS = {
    ("src/pc/sdk/libgpu.c", "display_words"): SETTLES,
    ("src/pc/sdk/libgpu.c", "displayed_picture"): SETTLES,
    ("src/pc/render/gl_picture.c", "upload_vram"):
        "the OpenGL picture's copy of VRAM: settling at every replay would raster both drawing areas each frame; "
        "nothing is kept back yet, and once something is, a settle crossing held work while a recorder is on is "
        "to record the settled words (OP_LOAD), as apply_move copies inside this copy; a resync settles first",
    ("src/pc/render/texture_dump.c", "TextureDump_Primitive"):
        "reads the primitive's texels through `source`, which soft_gpu.c settles (SoftGpu_SettleTexture) "
        "before calling; the raw pointer is only compared",
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
# Functions that read VRAM whole through a pointer taken elsewhere: a settle
# must come before the first match of the read (upload_vram's exemption above
# rests on resync's).
MUST_SETTLE = {
    ("src/pc/render/gl_picture.c", "resync"): re.compile(r"\b(upload_vram|picture_from_words)\s*\("),
}
SETTLE = re.compile(r"\bSoftGpu_(Settle|SettleTexture|Vram)\s*\(")
RAW = re.compile(r"\bSoftGpu_VramRaw\s*\(")
RAW_NAME = re.compile(r"\bSoftGpu_VramRaw\b(?!\s*\()")
DEFINE = re.compile(r"^[ \t]*#[ \t]*define\b.*(?:\\\n.*)*", re.M)
WRITE = re.compile(r"\(\s*(?:u?int16_t|u16|unsigned\s+short|void)\s*\*\s*\)\s*&?\s*SoftGpu_Vram(?:Raw)?\s*\(")
FUNCTION = re.compile(r"(\w+)\s*\([^;{}]*\)\s*$")
SCANNED = ("src", "mods", "examples")
SKIPPED = ("src/pc/render/soft_gpu.c", "src/pc/render/soft_gpu.h")


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


def problems_in(relative: str, code: str, found: set) -> list[str]:
    problems = []
    line = lambda at: code.count("\n", 0, at) + 1  # noqa: E731
    for match in WRITE.finditer(code):
        problems.append(f"{relative}:{line(match.start())}: VRAM written through SoftGpu_Vram's pointer; "
                        "write with SoftGpu_Load/Fill/Move")
    for match in DEFINE.finditer(code):
        if "SoftGpu_VramRaw" in match[0]:
            problems.append(f"{relative}:{line(match.start())}: SoftGpu_VramRaw in a macro, where this check "
                            "cannot follow it: call it in the function that settles")
    for match in RAW_NAME.finditer(code):
        problems.append(f"{relative}:{line(match.start())}: SoftGpu_VramRaw taken as a pointer, not called")
    for name, first, body in functions(code):
        key = (relative, name)
        if key in MUST_SETTLE:
            found.add(key)
            settle, read = SETTLE.search(body), MUST_SETTLE[key].search(body)
            if not settle or (read and read.start() < settle.start()):
                problems.append(f"{relative}:{first}: {name}() reads VRAM before it settles it")
        if not RAW.search(body):
            continue
        found.add(key)
        why = RAW_READERS.get(key)
        if why is None:
            problems.append(f"{relative}:{first}: {name}() takes SoftGpu_VramRaw() and is not listed in "
                            "tools/pc/check_vram_readers.py: settle what it reads, or say why it needs not")
        elif why == SETTLES and not SETTLE.search(body):
            problems.append(f"{relative}:{first}: {name}() takes SoftGpu_VramRaw() but settles nothing")
    return problems


def self_test() -> list[str]:
    """Each bypass above is caught, and a clean reader passes."""
    listed = "src/pc/text/glyphs.c"
    cases = {
        "unlisted raw reader": ("x.c", """
static int f(void)
{
    return SoftGpu_VramRaw()[0];
}
"""),
        "listed, settles nothing": (listed, """
static void read_cell(void)
{
    use(SoftGpu_VramRaw());
}
"""),
        "macro alias": ("x.c", """
#define RAW() SoftGpu_VramRaw()
static int f(void)
{
    return RAW()[0];
}
"""),
        "function pointer": ("x.c", """
static const unsigned short *(*get)(void) = SoftGpu_VramRaw;
"""),
        "write through the pointer": ("x.c", """
void f(void)
{
    ((uint16_t *)SoftGpu_Vram())[0] = 1;
}
"""),
        "settle after the read": ("src/pc/render/gl_picture.c", """
static void resync(int w)
{
    upload_vram();
    SoftGpu_Settle(0, 0, 1, 1);
}
"""),
    }
    failed = [name for name, (path, code) in cases.items() if not problems_in(path, code, set())]
    clean = """
static void read_cell(void)
{
    const uint16_t *v = SoftGpu_VramRaw();
    SoftGpu_Settle(0, 0, 4, 4);
    use(v);
}
"""
    if problems_in(listed, clean, set()):
        failed.append("a clean listed reader")
    return [f"self-test: not caught as it should be: {name}" for name in failed]


def main() -> int:
    problems = self_test()
    if problems:
        print("\n".join(problems))
        return 1
    units = sorted(p for base in SCANNED if (ROOT / base).is_dir() for p in (ROOT / base).rglob("*")
                   if p.suffix in (".c", ".h", ".inc") and p.is_file())
    found = set()
    for unit in units:
        relative = unit.relative_to(ROOT).as_posix()
        if relative in SKIPPED:
            continue
        problems += problems_in(relative, mask_noncode(unit.read_text(encoding="utf-8", errors="replace")), found)
    for key in sorted((set(RAW_READERS) | set(MUST_SETTLE)) - found):
        problems.append(f"{key[0]}: {key[1]}() is listed but no longer found as listed")
    for problem in problems:
        print(problem)
    if problems:
        print(f"{len(problems)} VRAM reader problem(s) (notes/pc-build.md, \"Settling VRAM before it is read\")")
        return 1
    print(f"{len(found)} VRAM readers outside soft_gpu.c, each settling what it reads or saying why it needs not")
    return 0


if __name__ == "__main__":
    sys.exit(main())
