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
- the OpenGL picture's raw copy of VRAM notes what it missed right after it
  takes it (SoftGpu_CopyTaken), and its replay takes the patches for that
  (SoftGpu_CopyPatches) before anything in its record (COPY_KEEPING);
- each function that calls SoftGpu_StateData() is listed in STATE_USERS:
  its index 0 is VRAM itself, settled whole as it is handed out but
  writable, so a caller that kept the pointer, or wrote through it while
  work is held, would get past every settle; and SoftGpu_StateData is only
  ever called, inside a function body: not named in a macro, not taken as
  a function pointer, not called in an initializer outside a function;
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
        "what it misses of held work comes back as patches (SoftGpu_CopyTaken right after the read, "
        "SoftGpu_CopyPatches at the next replay's start: COPY_KEEPING below); a resync settles first",
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
# (file, function): why its SoftGpu_StateData(0) pointer is safe. It settles
# all of VRAM as it returns, and nothing can be held until the next drawing.
STATE_USERS = {
    ("src/pc/guest/state_subsystems.c", "Memories_StateSubsystems"):
        "the save state's soft_gpu chunk: taken, then read or written whole at once in the same call "
        "(Memories_StateChunk), before anything is drawn; a load then calls SoftGpu_PictureFromVram",
}
# Functions that read VRAM whole through a pointer taken elsewhere: a settle
# must come before the first match of the read (upload_vram's exemption above
# rests on resync's).
MUST_SETTLE = {
    ("src/pc/render/gl_picture.c", "resync"): re.compile(r"\b(upload_vram|picture_from_words)\s*\("),
}
# The OpenGL picture's copy of VRAM, taken raw, and the deferred raster's
# patches for what it missed (soft_gpu.h, SoftGpu_CopyTaken): (file,
# function) -> (the call it must make, the anchor, "after" or "before" the
# anchor's first match). upload_vram notes what the copy missed right after
# it reads VRAM; the replay hands the patches over before anything in its
# record reads the copy.
COPY_KEEPING = {
    ("src/pc/render/gl_picture.c", "upload_vram"): (re.compile(r"\bSoftGpu_CopyTaken\s*\("),
                                                     re.compile(r"\bSoftGpu_VramRaw\s*\("), "after"),
    ("src/pc/render/gl_picture.c", "GlPicture_Replay"): (re.compile(r"\bSoftGpu_CopyPatches\s*\("),
                                                          re.compile(r"\b(gp0|apply_\w+|resync)\s*\("), "before"),
    # The replay's own host lookups settle too (HdText_Hud and the others,
    # inside the replay): their patches before the primitives draw, and
    # before the transfer that flushes them reads the copy, even when
    # nothing is drawn (its early return).
    ("src/pc/render/gl_picture.c", "flush_runs"): (re.compile(r"\bSoftGpu_CopyPatches\s*\("),
                                                    re.compile(r"\breturn\b"), "before"),
}
SETTLE = re.compile(r"\bSoftGpu_(Settle|SettleTexture|Vram)\s*\(")
RAW = re.compile(r"\bSoftGpu_VramRaw\s*\(")
STATE = re.compile(r"\bSoftGpu_StateData\s*\(")
RAW_NAME = re.compile(r"\bSoftGpu_VramRaw\b(?!\s*\()")
STATE_NAME = re.compile(r"\bSoftGpu_StateData\b(?!\s*\()")
DEFINE = re.compile(r"^[ \t]*#[ \t]*define\b.*(?:\\\n.*)*", re.M)
WRITE = re.compile(r"\(\s*(?:u?int16_t|u16|unsigned\s+short|void)\s*\*\s*\)\s*&?\s*SoftGpu_Vram(?:Raw)?\s*\(")
FUNCTION = re.compile(r"(\w+)\s*\([^;{}]*\)\s*$")
SCANNED = ("src", "mods", "examples")
SKIPPED = ("src/pc/render/soft_gpu.c", "src/pc/render/soft_gpu.h")


def functions(code: str):
    """Top-level function bodies: (name, first line, body text, offset)."""
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
                    yield match[1], code.count("\n", 0, start) + 1, code[start:at + 1], start
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
    for match in DEFINE.finditer(code):
        if "SoftGpu_StateData" in match[0]:
            problems.append(f"{relative}:{line(match.start())}: SoftGpu_StateData in a macro, where this check "
                            "cannot follow it: call it in a function listed in STATE_USERS")
    for match in STATE_NAME.finditer(code):
        problems.append(f"{relative}:{line(match.start())}: SoftGpu_StateData taken as a pointer, not called")
    bodies = []
    for name, first, body, start in functions(code):
        bodies.append((start, start + len(body)))
        key = (relative, name)
        if key in MUST_SETTLE:
            found.add(key)
            settle, read = SETTLE.search(body), MUST_SETTLE[key].search(body)
            if not settle or (read and read.start() < settle.start()):
                problems.append(f"{relative}:{first}: {name}() reads VRAM before it settles it")
        if key in COPY_KEEPING:
            found.add(key)
            call, anchor, where = COPY_KEEPING[key]
            made, at = call.search(body), anchor.search(body)
            if not made or not at or (made.start() < at.start()) != (where == "before"):
                problems.append(f"{relative}:{first}: {name}() must call /{call.pattern}/ {where} its first "
                                f"/{anchor.pattern}/ (the copy of VRAM and the deferred raster's patches)")
        if STATE.search(body):
            found.add(("state", relative, name))
            if key not in STATE_USERS:
                problems.append(f"{relative}:{first}: {name}() takes SoftGpu_StateData() (VRAM, writable) and is "
                                "not listed in STATE_USERS of tools/pc/check_vram_readers.py: say why it is safe")
        if not RAW.search(body):
            continue
        found.add(key)
        why = RAW_READERS.get(key)
        if why is None:
            problems.append(f"{relative}:{first}: {name}() takes SoftGpu_VramRaw() and is not listed in "
                            "tools/pc/check_vram_readers.py: settle what it reads, or say why it needs not")
        elif why == SETTLES and not SETTLE.search(body):
            problems.append(f"{relative}:{first}: {name}() takes SoftGpu_VramRaw() but settles nothing")
    macros = [match.span() for match in DEFINE.finditer(code)]  # reported above
    for match in STATE.finditer(code):
        if not any(start <= match.start() < end for start, end in bodies + macros):
            problems.append(f"{relative}:{line(match.start())}: SoftGpu_StateData() called outside a function "
                            "body (an initializer), where no STATE_USERS entry can vouch for it")
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
        "unlisted state user": ("x.c", """
static void f(void)
{
    size_t size;
    use(SoftGpu_StateData(0, &size));
}
"""),
        "state in a macro": ("x.c", """
#define VRAM_OF(size) SoftGpu_StateData(0, size)
static void f(void)
{
    size_t size;
    use(VRAM_OF(&size));
}
"""),
        "state as a function pointer": ("x.c", """
static void *(*state)(int, size_t *) = SoftGpu_StateData;
"""),
        "state in a global initializer": ("x.c", """
static size_t size;
static struct { void *vram; } kept = { SoftGpu_StateData(0, &size) };
"""),
        "settle after the read": ("src/pc/render/gl_picture.c", """
static void resync(int w)
{
    upload_vram();
    SoftGpu_Settle(0, 0, 1, 1);
}
"""),
        "copy taken unnoted": ("src/pc/render/gl_picture.c", """
static void upload_vram(void)
{
    upload(SoftGpu_VramRaw());
}
"""),
        "copy noted before it is taken": ("src/pc/render/gl_picture.c", """
static void upload_vram(void)
{
    SoftGpu_CopyTaken();
    upload(SoftGpu_VramRaw());
}
"""),
        "patches after the record": ("src/pc/render/gl_picture.c", """
int GlPicture_Replay(void)
{
    gp0(words, 6);
    SoftGpu_CopyPatches();
}
"""),
        "no patches": ("src/pc/render/gl_picture.c", """
int GlPicture_Replay(void)
{
    apply_load(0, 0, 1, 1, words);
}
"""),
        "patches only for a flush that draws": ("src/pc/render/gl_picture.c", """
static void flush_runs(void)
{
    if (!vertex_count) {
        run_count = 0;
        return;
    }
    SoftGpu_CopyPatches();
    draw();
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
    state_user = """
void Memories_StateSubsystems(void *state)
{
    size_t size;
    chunk(state, SoftGpu_StateData(0, &size), size);
}
"""
    if problems_in("src/pc/guest/state_subsystems.c", state_user, set()):
        failed.append("a listed state user")
    keeper = """
static void upload_vram(void)
{
    upload(SoftGpu_VramRaw());
    SoftGpu_CopyTaken();
}
static void flush_runs(void)
{
    if (scale >= 2) SoftGpu_CopyPatches();
    if (!vertex_count) return;
    draw();
}
int GlPicture_Replay(void)
{
    SoftGpu_CopyPatches();
    if (overflow) resync(1, words);
    gp0(words, 6);
    upload_vram();
}
"""
    if problems_in("src/pc/render/gl_picture.c", keeper, set()):
        failed.append("a copy kept as it should be")
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
    for key in sorted((set(RAW_READERS) | set(MUST_SETTLE) | set(COPY_KEEPING)) - found):
        problems.append(f"{key[0]}: {key[1]}() is listed but no longer found as listed")
    for key in sorted(set(STATE_USERS) - {(entry[1], entry[2]) for entry in found if entry[0] == "state"}):
        problems.append(f"{key[0]}: {key[1]}() is listed in STATE_USERS but no longer found")
    for problem in problems:
        print(problem)
    if problems:
        print(f"{len(problems)} VRAM reader problem(s) (notes/pc-build.md, \"Settling VRAM before it is read\")")
        return 1
    users = len([key for key in found if key[0] == "state"])
    print(f"{len(found) - users} VRAM readers and {users} state user(s) outside soft_gpu.c, each settling what it "
          "reads or saying why it needs not")
    return 0


if __name__ == "__main__":
    sys.exit(main())
