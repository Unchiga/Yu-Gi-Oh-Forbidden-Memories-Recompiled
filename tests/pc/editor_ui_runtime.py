#!/usr/bin/env python3
"""Live FM Editor UI tab check (built game and user-supplied disc).

Makes a mod through the UI tab the way a modder would (its pages' own
controls, the file dialogs answered with pictures drawn here):

* the title: a picture of its own for the background over a color, the
  logo and the copyright line recolored and moved, PUSH START BUTTON
  moved, two pictures added and a line of words;
* the menus: TRADE hidden, OPTION renamed SETTINGS, a button of its own that
  opens the options screen and one that shows a notice, put in among the
  game's entries;
* the duel: the life-point panel's halves swapped -- the opponent's moved
  down under the player's, recolored, with RIVAL for COM and yellow
  digits; the player's moved up, larger, drawn from a picture, green
  digits; the FIELD box moved down, larger, bluer; the card bar drawn from
  a picture; the hand's cursor larger, yellow and moved; the field's cursor
  recolored. The LP halves and the FIELD box move up and down only (the
  game slides them off the side), so the page is asked to drag them across
  too and must not; then, as an old hand-written mod would, the saved
  manifest is given an "x" for them and a size too large to leave the
  screen, which the game must leave out and note.

Then plays it through tools/pc/yfm_control.py and takes pictures of each:

1. the title and its menu, without the mod and with it;
2. the added button pressed: the options screen opens;
3. a whole duel, turn after turn -- a fusion, a magic card that burns the
   opponent's LP, battles, the opponent's turns, until it is won -- with a
   picture at each step, each checked: the panel is drawn where the mod
   put it and not where the game has it, its digits are the LP the game
   holds (five of them too: LP over 9999 widen each half), lit on the
   opponent's turn, and for the first battle a picture every frame of the
   slide: the moved halves and the FIELD box slide with the game's,
   pixel for pixel, and leave the screen when it does;
4. the duel's results;
5. at Internal 2x in a window (the OpenGL picture, with HD text), the title,
   the menu and a duel; the title's words and the notice there too (the
   port draws those over the picture);
6. the duel again with MEMORIES_DUEL_EFFECTS=interpreter, on the reference
   build (the game with the MIPS interpreter: tools/pc/reference_build.py,
   built when it is missing, or --reference-executable);
7. without the mod, the same frames as the build given by --baseline
   (VRAM hashes from boot through the title, the menu and two turns).

The pictures are drawn here; nothing is kept in the repository. Shots go
to <out>/shots, the windowed ones to <out>/shots/window.

    xvfb-run -a python3 tests/pc/editor_ui_runtime.py [--executable PATH] [--out DIR] [--baseline PATH]
        [--reference-executable PATH]

Needs a display for Tk (xvfb-run works) and ImageMagick's `import` for the
window pictures.
"""
import argparse
import json
import math
import struct
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/pc"))
EXECUTABLE = ROOT / "tmp/pc/game32/memories-pc"
MOD = "uitab-test"
OPPONENT = 1                    # Simon Muran: a duel the player's deck wins
# The panel as the game has it (Duel_InitScene): its 64 x 40 at 248, 16; its halves.
PANEL = (248, 16, 64, 40)
GAME_OPPONENT_HALF = (248, 16, 64, 20)
# Where the mod puts the halves: the opponent's 24 down; the player's 28 up, at 120 %.
OPPONENT_HALF = (248, 40, 64, 20)
PLAYER_HALF = (242, 6, 76, 24)    # 64 x 20 at 120 % about its middle (280, 46), moved by (0, -28)
OPPONENT_DIGITS = (277, 43)       # the panel's (277, 19), moved with the half
# The FIELD box (12, 24, 56 x 24) 40 down, at 130 % (the most that leaves the
# screen with the game's: the mod asks 300) about its middle (40, 36 + 40).
FIELD_BOX = (4, 60, 72, 32)
GAME_FIELD_BOX = (12, 24, 56, 24)

results = []
SLIDE_FRAMES = 24       # the slide off takes 16 (func_8001ED20), after the attack is chosen


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(f"  {'ok  ' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}", flush=True)
    return ok


# --- pictures -----------------------------------------------------------------------

def drawn(w, h, kind):
    """A test picture, drawn rather than kept."""
    from fm_editor import pngio
    out = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            u, v = x / w, y / h
            a = 255
            if kind == "night":           # a sky over dunes, a moon
                r, g, b = int(20 + 40 * v), int(10 + 30 * v), int(60 + 90 * v)
                if math.hypot(u - 0.78, v - 0.22) < 0.08:
                    r, g, b = 240, 230, 190
                if v > 0.72 + 0.06 * math.sin(u * 9):
                    r, g, b = int(150 - 60 * v), int(110 - 50 * v), 40
                if ((x * 73 + y * 151) % 997) == 0 and v < 0.6:
                    r, g, b = 255, 255, 255
            elif kind == "seal":          # gold rings, clear outside
                d = math.hypot(u - 0.5, v - 0.5) * 2
                if d > 1:
                    a = 0
                    r = g = b = 0
                else:
                    t = 0.5 + 0.5 * math.cos(d * 22)
                    r, g, b = int(200 + 55 * t), int(150 + 80 * t), int(30 + 40 * t)
            elif kind == "panel":         # a teal plate with a gold rim
                rim = min(x, y, w - 1 - x, h - 1 - y) < max(2, h // 12)
                r, g, b = (240, 200, 80) if rim else (20, int(90 + 60 * v), int(110 + 60 * u))
            elif kind == "bar":           # a dark blue strip, gold rim and studs
                rim = min(x, y, w - 1 - x, h - 1 - y) < max(2, h // 36)
                stud = math.hypot((x % (w // 16)) - w // 32, y - h // 8) < h // 20
                r, g, b = (240, 200, 80) if rim or stud else (16, int(28 + 40 * v), int(80 + 70 * v))
            else:                         # "button": a red plate, orange rim
                rim = min(x, y, w - 1 - x, h - 1 - y) < max(2, h // 10)
                r, g, b = (240, 150, 20) if rim else (int(120 + 80 * v), 20, 30)
            out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes((r, g, b, a))
    return pngio.Image(w, h, bytes(out))


def pictures(folder: Path) -> dict:
    from fm_editor import pngio
    folder.mkdir(parents=True, exist_ok=True)
    made = {"night": drawn(640, 480, "night"), "seal": drawn(128, 128, "seal"), "panel": drawn(256, 80, "panel"),
            "bar": drawn(960, 216, "bar"), "button": drawn(312, 84, "button")}
    paths = {}
    for name, image in made.items():
        paths[name] = folder / f"{name}.png"
        pngio.write(paths[name], image)
    return paths


# --- the editor ------------------------------------------------------------------------

def make_mod(game: Path, folder: Path, art: dict) -> list:
    """The mod, through the UI tab's pages; what was done."""
    from fm_editor import manifest
    from fm_editor.app import App
    done = []
    app = App(ask=False, autostart=False)
    app.withdraw()
    app.update()
    app.start(str(game), None, False)
    app.update()
    app.info.vars["id"].set(MOD)
    app.info.vars["name"].set("UI tab test")
    assert app.info.commit()
    tab = app.ui
    app.notebook.select(tab)

    def page(name):
        tab.page_name.set(name)
        tab.show_page()
        app.update()
        return tab.pages[name]

    def choose(path):
        return mock.patch("fm_editor.file_dialogs.askopenfilename", return_value=str(path))

    title = page("title")
    title.select("background")
    with choose(art["night"]):
        title.background.choose()
    title.background.color.set(0x102040)
    title.background.set("color", "#102040")
    title.select("logo")
    title.moved("logo", 0, -14)
    title.layer_tint.set(0xFFD870)
    title.set_layer("tint", "#FFD870", "#FFFFFF")
    title.select("copyright")
    title.moved("copyright", 0, 8)
    title.set_layer("tint", "#A0C0FF", "#FFFFFF")
    title.select("prompt")
    title.moved("prompt", 0, -16)
    for x in (40, 280):
        with choose(art["seal"]):
            title.add_picture()
        title.moved(title.chosen, x - 160, 64 - 120)
    title.set_picture("width", 56)
    title.add_text()
    title.text_vars["text"].set("UI TAB TEST")
    title.text_typed("text")
    title.set_text("color", "#FFE060", "#FFFFFF")
    done.append("title: background picture over #102040, logo and copyright recolored and moved, PUSH START "
                "moved, two seals added, a line of words")

    menu = page("menu")
    menu.chosen = "trade"
    menu.toggle_hidden()
    menu.chosen = "options"
    menu.fill_form()
    menu.item_vars["label"].set("SETTINGS")
    menu.item_typed("label")
    menu.add_button()
    menu.item_vars["label"].set("GO OPTIONS")
    menu.item_typed("label")
    menu.action_box.current(menu.action_choices.index("options"))
    menu.set_action()
    with choose(art["button"]):
        menu.item_image("image")
    for _ in range(2):
        menu.move(-1)                           # above TRADE and SETTINGS: the fourth row
    menu.add_button()
    menu.item_vars["label"].set("CREDITS")
    menu.item_typed("label")
    menu.item_vars["notice_title"].set("UI tab")
    menu.item_vars["notice"].set("Made with the FM Editor's UI tab.")
    menu.item_typed("notice")
    done.append("menus: TRADE hidden, OPTION as SETTINGS, GO OPTIONS (a picture, opens the options) fourth, "
                "CREDITS (a notice) last")

    duel = page("duel")
    duel.select("lp_opponent")
    duel.moved("lp_opponent", -236, 24)         # across is left out: the game slides it off the side
    duel.set_color("tint", 0xFF9090)
    duel.set_color("digits", 0xFFE040)
    duel.vars["label"].set("RIVAL")
    duel.typed("label")
    duel.select("lp_player")
    duel.moved("lp_player", 0, -28)
    duel.wheel("lp_player", 1)
    duel.wheel("lp_player", 1)
    with choose(art["panel"]):
        duel.choose_image()
    duel.set_color("digits", 0x80FF80)
    duel.select("field")
    duel.moved("field", 170, 40)
    duel.wheel("field", 1)
    for _ in range(4):
        duel.wheel("field", 1)                  # no larger than 130 %: it would not leave the screen
    sizes = duel.element("field").get("scale")
    for _ in range(2):
        duel.wheel("field", -1)
    duel.set_color("tint", 0x80C0FF)
    duel.select("card_bar")
    with choose(art["bar"]):
        duel.choose_image()
    duel.select("hand_cursor")
    duel.vars["scale"].set("150")
    duel.typed("scale")
    duel.moved("hand_cursor", 2, -4)
    duel.set_color("tint", 0xFFFF40)
    duel.select("field_cursor")
    duel.set_color("tint", 0x40E0FF)
    done.append("duel: both LP halves, the FIELD box, the card bar and both cursors")
    elements = app.project.other["ui"]["duel"]
    check("the page moves the LP halves and the FIELD box up and down only",
          all("x" not in elements[n] for n in ("lp_opponent", "lp_player", "field")) and
          elements["lp_opponent"].get("y") == 24 and elements["field"].get("y") == 40,
          f"{ {n: elements[n].get('x') for n in ('lp_opponent', 'lp_player', 'field')} }")
    check("the page sizes the FIELD box no larger than leaves the screen with the game's",
          sizes == 130 and elements["field"].get("scale") == 110, f"at most {sizes}, then {elements['field'].get('scale')}")
    check("the hand's cursor moves both ways", (elements["hand_cursor"].get("x"), elements["hand_cursor"].get("y"))
          == (2, -4), str(elements["hand_cursor"]))
    from fm_editor import validate
    issues = [str(i) for i in validate.validate(app.project) if i.area in ("UI", "Mod info")]
    check("the editor finds nothing wrong with the mod", not issues, "; ".join(issues))
    manifest.save_mod(app.project, folder)
    app.dirty = False
    app.destroy()
    # An old hand-written mod: an "x" for what the game slides off the side,
    # a size that would not leave the screen with it. The game leaves out
    # the first and draws the second at the most that does, and notes both.
    path = folder / "mod.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    data["ui"]["duel"]["lp_opponent"]["x"] = -236
    data["ui"]["duel"]["field"]["x"] = 170
    data["ui"]["duel"]["field"]["scale"] = 300
    path.write_text(json.dumps(data, indent=4), encoding="utf-8")
    done.append("then by hand: lp_opponent x -236, field x 170 and scale 300")
    return done


# --- the game -----------------------------------------------------------------------

def game(executable, out, mods, name, mod_on=True, headless=True, env=None, settings=None):
    from yfm_control import Game
    flags = {f"mod.{MOD}": 1 if mod_on else 0, "mod.3d-monsters": 0, "mod.hand-camera": 0, "mod.ai-hard-mode": 0}
    flags.update(settings or {})
    return Game(executable=executable, out=out / name, mods_dir=mods, settings=flags, headless=headless,
                env=dict({"MEMORIES_DUMP_PICTURE": "1"}, **(env or {})))


def to_title(g):
    for i in range(240):
        g.step(30)
        if g.u32("D_80184558"):
            break
        if i % 4 == 3:
            g.press("start")
    g.step(150)


def read_png(path):
    from fm_editor import pngio
    return pngio.read(path)


def count_in(image, box, test, scale=1):
    """Pixels passing `test` (r, g, b) in box (x, y, w, h) of the game's 320 x 240."""
    x0, y0, w, h = box
    return sum(1 for y in range(max(0, y0 * scale), min(image.height, (y0 + h) * scale))
               for x in range(max(0, x0 * scale), min(image.width, (x0 + w) * scale)) if test(*image.pixel(x, y)[:3]))


def rim_in(image, box, scale=1):
    """The opponent's half's rim and words, white made pink by its #FF9090:
    nothing else on the duel's screen is that color (the field is yellow)."""
    return count_in(image, box, lambda r, g, b: r > 200 and 100 < g < 190 and 100 < b < 190, scale)


def panel_blue_in(image, box, scale=1):
    """The panel's own deep blue (the opponent's half as the game draws it)."""
    return count_in(image, box, lambda r, g, b: b > 100 and r < 70 and g < 70, scale)


def plate_in(image, box):
    """Pixels of the player's half's picture (teal plate) in the box."""
    x0, y0, w, h = box
    return sum(1 for y in range(y0, y0 + h) for x in range(x0, x0 + w)
               if image.pixel(x, y)[0] < 60 and image.pixel(x, y)[1] > 70 and image.pixel(x, y)[2] > 90)


def title_and_menu(executable, out, mods, shots):
    for on in (False, True):
        tag = "mod" if on else "retail"
        with game(executable, out, mods, f"title-{tag}", mod_on=on) as g:
            to_title(g)
            g.shot(shots / f"01-title-{tag}.png")
            g.press("start")
            g.step(120)
            g.shot(shots / f"02-menu-{tag}.png")
            if not on:
                continue
            shot = read_png(shots / "01-title-mod.png")
            # The background is the mod's: the night sky's blue at the top
            # left, not the hieroglyph wall's grey.
            r, gr, b, _ = shot.pixel(4, 4)
            check("the title's background is the mod's picture", b > r + 20, f"pixel (4, 4) = {(r, gr, b)}")
            # GO OPTIONS: three down from NEW GAME, then Cross.
            for _ in range(3):
                g.press("down", hold=4, after=16)
            g.step(30)
            g.shot(shots / "03-menu-go-options-chosen.png")
            g.press("cross", hold=4, after=20)
            g.wait_until(lambda g: g.mode() == 11, 900, what="the options screen")
            g.step(90)
            g.shot(shots / "04-go-options-pressed.png")
            check("the added button opens the options screen", g.mode() == 11, f"mode {g.mode()}")


def digits_wrong(image, art, value, lit, count=4):
    """Pixels of the opponent's LP digits that are not the game's digits for
    `value`, yellow (#FFE040) and lit or dimmed, where the mod moved them:
    the panel's (277, 19) moved with its half (a fifth digit one more to
    the left). Clear texels are skipped."""
    from fm_editor import ui_assets
    wrong = 0
    left = OPPONENT_DIGITS[0] - 8 * (count - 4)
    for i, ch in enumerate(str(value).rjust(count)):
        if ch == " ":
            continue
        digit = ui_assets.tint(art.digit(int(ch)), 0xFFE040, 255 if lit else 128)
        for y in range(8):
            for x in range(8):
                want = digit.pixel(x, y)
                if not want[3]:
                    continue
                got = image.pixel(left + 8 * i + x, OPPONENT_DIGITS[1] + y)
                if any(abs(got[c] - want[c]) > 24 for c in range(3)):
                    wrong += 1
    return wrong


def pink(r, g, b):
    """The opponent's half's rim and words, white made pink by its #FF9090."""
    return r > 200 and 100 < g < 190 and 100 < b < 190


def light_blue(r, g, b):
    """The FIELD box's rim and words through its #80C0FF."""
    return b > 200 and r < 150 and g > 90


def plate(r, g, b):
    """The player's half's picture (a teal plate)."""
    return r < 60 and g > 70 and b > 90


def extent(image, rows, test, columns=(0, 320)):
    """The first and last column (x) in rows (y0, y1) and columns (x0, x1) of
    the game's 320 x 240 with a pixel passing `test`, or None."""
    xs = [x for y in range(*rows) for x in range(*columns) if test(*image.pixel(x, y)[:3])]
    return (min(xs), max(xs)) if xs else None


def object_x(g, name):
    """Where the game has its panel (D_8009B21C) or FIELD box (D_8009B214):
    the object's x (DisplayObject field_30)."""
    import struct
    at = g.u32(name)
    return struct.unpack("<h", g.peek(at, 0x34)[0x30:0x32])[0] if at else None


def settle_lp(g):
    """Until both readouts have counted to the LP the game holds."""
    g.wait_until(lambda g: all(s["displayed_lp"] == s["lp"] for s in g.duel()), 1200, every=4,
                 what="the LP readouts")


def attack(g, column, target, shoot, slide=None):
    """Game.attack, with pictures while the battle plays: the panel leaves
    with the game's for it. slide(g) is asked after each of the first
    frames once the attack is chosen (the panel and the box sliding off)."""
    from yfm_control import DUEL_PHASES, FIELD_CURSOR, TARGET_CURSOR, ControlError
    g.wait_turn()
    if g.phase() != DUEL_PHASES["field"] or not g.field()[2][column]:
        raise ControlError(f"nothing to attack with in column {column}")
    for _ in range(4):
        row = g.u8(FIELD_CURSOR + 1)
        if row == 2:
            break
        g.press("down" if row < 2 else "up", hold=4, after=16)
    g._cursor_to(FIELD_CURSOR, column, "field")
    g.press("cross", hold=4, after=40)
    if target is not None:
        g._cursor_to(TARGET_CURSOR, target, "target")
    if slide:
        g.press("cross", hold=1, after=0)
        for _ in range(SLIDE_FRAMES):
            g.step(1)
            slide(g)
    else:
        g.press("cross", hold=4, after=10)
    pictures = [shoot(f"battle-{k}") for k in range(3) if not g.step(25) or True]
    g._settle()
    return pictures


def slid_with_the_game(name, frames):
    """Frame by frame as the game slides its panel right and its FIELD box
    left off the screen for a battle: each moved one exactly as far as the
    game's has gone (where it is drawn less where the game has it the same
    every frame), and gone once wholly off the screen. A picture shows the
    game's places of two frames before (the game moves its objects for the
    frame after the one it draws, and the picture is the one shown)."""
    # (rows, its pixels, which edge is followed (0 its left, 1 its right), the
    # game's object (1 the panel, 2 the box), how far that edge is from the
    # object's x: the halves' left edges (the panel's middle less half their
    # width), the box's right (its middle 28 right of its x, plus 36 at 130 %).
    rows = {"the opponent's half": ((40, 60), pink, (0, 320), 0, 1, -32),
            "the player's half": ((6, 30), plate, (0, 320), 0, 1, -38),
            "the FIELD box": ((FIELD_BOX[1], FIELD_BOX[1] + FIELD_BOX[3]), light_blue, (0, 160), 1, 2, 64)}
    lag = 2
    for what, (band, test, columns, edge, which, reach) in rows.items():
        bad, seen, gone, inset = [], 0, 0, None
        for i, (image, _, _, path) in enumerate(frames):
            if i < lag:
                continue
            _, panel_x, box_x, _ = frames[i - lag]
            at = panel_x if which == 1 else box_x
            drawn = extent(image, band, test, columns)
            if inset is None and drawn is not None:
                inset = drawn[edge] - (at + reach)      # its first pixel of that kind from its edge
            if inset is None:
                bad.append(f"{path.name}: not drawn")
                continue
            geometric = at + reach
            off = geometric >= 320 if edge == 0 else geometric <= 0
            want = geometric + inset
            if off:
                gone += 1
                if drawn is not None:
                    bad.append(f"{path.name}: still at {drawn} with the game's at {at}")
            elif (want < 320 if edge == 0 else want >= 0):
                seen += 1
                if drawn is None or drawn[edge] != want:
                    bad.append(f"{path.name}: {drawn}, want {'left' if edge == 0 else 'right'} {want} "
                               f"(the game's at {at})")
        check(f"{name}: {what} slides with the game's and leaves with it, frame by frame",
              not bad and seen > 2 and gone > 2, f"{seen} frames on, {gone} off; " + "; ".join(bad[:4]))


def play_duel(executable, out, mods, shots, name, env=None, settings=None, prefix="10"):
    """A whole duel with the mod; pictures and checks along the way."""
    from fm_editor import disc, gamedata
    from yfm_control import DUEL_PHASES
    files = disc.load(ROOT / "game")
    data = gamedata.load_game(files)
    names = {c.id: c.name for c in data.cards.values()}
    card = {n: next(cid for cid, name in names.items() if name == n) for n in
            ("Mystical Elf", "Blue-eyes White Dragon", "Dian Keto the Cure Master")}
    burn = next(cid for cid, name in names.items() if name == "Hinotama" and data.cards[cid].type == gamedata.TYPE_MAGIC)
    pair = next((a, b) for (a, b), r in data.fusions.items() if a == card["Mystical Elf"] and r and
                names.get(r) == "Dark Elf")
    hand = [pair[0], pair[1], burn, card["Blue-eyes White Dragon"], card["Blue-eyes White Dragon"]]
    deck = hand + [card["Blue-eyes White Dragon"], card["Dian Keto the Cure Master"]] + list(range(3, 36))
    count = 0

    def shot(label):
        nonlocal count
        count += 1
        path = shots / f"{prefix}-{count:02d}-{label}.png"
        g.shot(path)
        return path

    from fm_editor import ui_assets
    art = ui_assets.DuelArt(files.wa)
    with game(executable, out, mods, name, env=env, settings=settings) as g:
        g.goto("duel", opponent=OPPONENT, deck=deck[:40])
        g.duel_ready(before_deal=lambda g: g.arrange_deck(0, hand))
        start = shot("duel-start")
        image = read_png(start)
        check(f"{name}: the opponent's half drawn where the mod put it (its x left out)",
              rim_in(image, OPPONENT_HALF) > 40 and rim_in(image, GAME_OPPONENT_HALF) == 0 and
              extent(image, (40, 60), pink)[1] == OPPONENT_HALF[0] + OPPONENT_HALF[2] - 1,
              f"{rim_in(image, OPPONENT_HALF)} pixels there, {rim_in(image, GAME_OPPONENT_HALF)} at the game's place, "
              f"from x {extent(image, (40, 60), pink)}")
        check(f"{name}: the player's half is the mod's picture", plate_in(image, PLAYER_HALF) > 200,
              f"{plate_in(image, PLAYER_HALF)} plate pixels")
        wrong = digits_wrong(image, art, g.duel()[1]["displayed_lp"], g.turn() == 1)
        check(f"{name}: the opponent's LP digits are the game's, yellow, where the mod put them", wrong == 0,
              f"{wrong} pixels differ")
        box = extent(image, (FIELD_BOX[1], FIELD_BOX[1] + FIELD_BOX[3]), light_blue, (0, 160))
        # Its rim one in from its edges: 130 % reaches 3 further each side than 110 % would.
        check(f"{name}: the FIELD box 40 down at 130 % (its x left out, its 300 % brought down)",
              box is not None and FIELD_BOX[0] <= box[0] <= FIELD_BOX[0] + 2 and
              FIELD_BOX[0] + FIELD_BOX[2] - 3 <= box[1] < FIELD_BOX[0] + FIELD_BOX[2] and
              count_in(image, GAME_FIELD_BOX, light_blue) == 0, f"x {box}, want {FIELD_BOX}")
        log = (out / name / "game.log").read_text(errors="replace")
        for what in ("duel lp_opponent is slid off the screen sideways", "duel field is slid off the screen sideways",
                     "duel field at 300% would not leave the screen with the game's; drawn at 130%"):
            check(f"{name}: the Mods window notes \"{what}...\"", what in log)
        # A fifth digit: LP over 9999 widen each half by a digit to the left, as the game widens the panel.
        sides = g.address("D_800E9FF0")
        lp = g.duel()[0]["lp"]
        for side_at in (sides, sides + 0x20):
            g.poke(side_at + 0x12, struct.pack("<hH", 12000, 12000))
        g.step(4)
        wide = shot("five-digits")
        image = read_png(wide)
        wrong = digits_wrong(image, art, 12000, g.turn() == 1, 5)
        before = extent(read_png(start), (40, 60), pink)
        check(f"{name}: with 12000 LP each half widens a digit and the digits read 12000",
              extent(image, (40, 60), pink) == (before[0] - 8, before[1]) and wrong == 0,
              f"from x {extent(image, (40, 60), pink)} (with four digits {before}), {wrong} digit pixels differ")
        for side_at, value in ((sides, lp), (sides + 0x20, 8000)):
            g.poke(side_at + 0x12, struct.pack("<hH", value, value))
        g.step(4)
        g.fuse([0, 1], face_up=True)
        shot("fused")
        turns, lp_seen, battles = 0, set(), 0
        while not g.duel_over() and turns < 30:
            turns += 1
            g.wait_turn()
            if g.duel_over():
                break
            before = [s["lp"] for s in g.duel()]
            if turns == 1:
                pass                            # the fusion was this turn's card
            elif any(c and c["id"] == burn for c in g.duel()[0]["hand"]):
                slot = next(i for i, c in enumerate(g.duel()[0]["hand"]) if c and c["id"] == burn)
                g.play_card(slot)                # a magic card goes up as it is: activated
                settle_lp(g)
                after = [s["lp"] for s in g.duel()]
                shot(f"turn{turns}-magic-lp-{after[1]}")
                check(f"{name}: the magic card burns the opponent's LP", after[1] < before[1],
                      f"{before[1]} -> {after[1]}")
            else:
                slots = [i for i, c in enumerate(g.duel()[0]["hand"]) if c and c["attack"] > 0]
                if slots and not g.duel_over():
                    best = max(slots, key=lambda i: g.duel()[0]["hand"][i]["attack"])
                    g.play_card(best, face_up=True)
                    shot(f"turn{turns}-played")
            for column in range(5 if turns > 1 else 0):     # the opening turn has no battle
                if g.duel_over() or g.turn() != 0:
                    break
                mine = g.field()[2][column]
                if not mine or g.phase() != DUEL_PHASES["field"]:
                    continue
                theirs = [i for i, c in enumerate(g.field()[1]) if c]
                frames = []

                def slide(g):
                    path = shot("slide")
                    frames.append((read_png(path), object_x(g, "D_8009B21C"), object_x(g, "D_8009B214"), path))
                try:
                    during = attack(g, column, theirs[0] if theirs else None, shot, slide if not battles else None)
                    battles += 1
                except Exception as problem:     # noqa: BLE001 -- a monster that cannot attack
                    print(f"    (no attack from column {column}: {problem})", flush=True)
                    continue
                if g.duel_over():
                    break
                if frames:
                    slid_with_the_game(name, frames)
                image = read_png(during[-1])
                # (A battle's cards are drawn there now: a pixel or two of theirs can be pink.)
                check(f"{name}: during battle {battles}, the moved panel gone with the game's",
                      rim_in(image, OPPONENT_HALF) < 4, f"{rim_in(image, OPPONENT_HALF)} of its pixels")
                g.step(60)                      # the panel slides back in
                settle_lp(g)
                lp = [s["lp"] for s in g.duel()]
                lp_seen.add(tuple(lp))
                path = shot(f"turn{turns}-battle-lp-{lp[0]}-{lp[1]}")
                if 0 in lp:
                    break                       # the duel's end: the panel leaves for the outro
                image = read_png(path)
                check(f"{name}: after battle {battles}, the panel back where the mod put it",
                      rim_in(image, OPPONENT_HALF) > 40 and rim_in(image, GAME_OPPONENT_HALF) == 0,
                      f"moved {rim_in(image, OPPONENT_HALF)}, game's place {rim_in(image, GAME_OPPONENT_HALF)}")
                wrong = digits_wrong(image, art, lp[1], False)
                check(f"{name}: after battle {battles}, the digits read the opponent's {lp[1]} LP", wrong == 0,
                      f"{wrong} pixels differ")
            if g.duel_over():
                break
            g.press_until(lambda g: g.turn() != 0 or g.duel_over(), "start", every=60, timeout=1200,
                          what="the end of the turn")
            g.wait_until(lambda g: g.duel_over() or g.turn() == 1, 600, what="the opponent's turn")
            if not g.duel_over():
                g.step(90)
                image = read_png(shot(f"turn{turns}-opponent"))
                if g.turn() == 1 and not g.duel_over() and turns == 1:
                    # The opponent's turn: the panel in its turn's palette, their digits lit.
                    value = g.duel()[1]["displayed_lp"]
                    wrong = digits_wrong(image, art, value, True)
                    check(f"{name}: on the opponent's turn their digits are lit, where the mod put them",
                          wrong == 0, f"{wrong} pixels differ from {value} lit")
            g.wait_turn(("hand",), 30000)
            if not g.duel_over():
                shot(f"turn{turns + 1}-hand")
        check(f"{name}: the duel ends", g.duel_over(), f"after {turns} turns, {battles} battles, LP {[s['lp'] for s in g.duel()]}")
        for _ in range(12):
            g.step(120)
            if g.phase() in (DUEL_PHASES["result"], DUEL_PHASES["rewards"]):
                break
        shot("result")
        for _ in range(6):
            g.press("cross", hold=4, after=60)
            g.step(60)
            shot("result-page")
            if g.mode() != 3:
                break
        g.step(240)
        shot("after")
        check(f"{name}: the game goes on after the duel", g.mode() != 3 or g.phase() == DUEL_PHASES["rewards"],
              f"mode {g.mode()}, phase {g.phase()}")


def window_shots(executable, out, mods, shots):
    """At Internal 2x in a window: the OpenGL picture (HD text on), and the
    window itself for what the port draws over it."""
    folder = shots / "window"
    folder.mkdir(parents=True, exist_ok=True)

    def screen(path):
        subprocess.run(["import", "-window", "root", str(path)], check=False)

    settings = {"internal_scale": 2, "hd_text": 1, "scale": 2}
    with game(executable, out, mods, "window", headless=False, settings=settings) as g:
        to_title(g)
        g.shot(folder / "20-title-2x.png")
        g.step(30)
        screen(folder / "21-title-window.png")
        g.press("start")
        g.step(120)
        g.shot(folder / "22-menu-2x.png")
        for _ in range(5):                      # CREDITS: the last row
            g.press("down", hold=4, after=16)
        g.press("cross", hold=4, after=60)
        g.step(30)
        screen(folder / "23-credits-notice-window.png")
        g.press("cross", hold=4, after=60)
        g.goto("duel", opponent=OPPONENT, deck="1-40")
        g.duel_ready()
        g.shot(folder / "24-duel-2x.png")
        image = read_png(folder / "24-duel-2x.png")
        check("internal 2x: the opponent's half where the mod put it", rim_in(image, OPPONENT_HALF, 2) > 160,
              f"{rim_in(image, OPPONENT_HALF, 2)} pixels")
        check("internal 2x: not where the game has it", rim_in(image, GAME_OPPONENT_HALF, 2) == 0,
              f"{rim_in(image, GAME_OPPONENT_HALF, 2)} pixels")
        g.play_card(3, face_up=True)
        g.shot(folder / "25-duel-played-2x.png")


def baseline(executable, base, out, mods):
    """Without the mod: the same VRAM hashes as the build before."""
    from yfm_control import Game
    sequences = []
    for exe, name in ((base, "baseline"), (executable, "this-build")):
        hashes = []
        with Game(executable=exe, out=out / f"nomod-{name}", mods_dir=mods,
                  settings={f"mod.{MOD}": 0, "mod.3d-monsters": 0, "mod.hand-camera": 0,
                            "mod.ai-hard-mode": 0}) as g:
            for i in range(60):
                g.step(30)
                hashes.append(g.hash())
                if g.u32("D_80184558"):
                    break
                if i % 4 == 3:
                    g.press("start")
            for _ in range(10):
                g.step(20)
                hashes.append(g.hash())
            g.press("start")
            for _ in range(10):
                g.step(12)
                hashes.append(g.hash())
            g.goto("duel", opponent=3, deck="1-40")
            g.duel_ready()
            for _ in range(5):
                g.step(10)
                hashes.append(g.hash())
            g.play_card(0, face_up=True)
            for _ in range(10):
                g.step(10)
                hashes.append(g.hash())
            g.end_turn()
            for _ in range(30):
                g.step(20)
                hashes.append(g.hash())
        sequences.append(hashes)
    same = sum(1 for a, b in zip(*sequences) if a == b)
    check("without the mod, every frame as the baseline build's", sequences[0] == sequences[1],
          f"{same} of {len(sequences[0])} VRAM hashes equal")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--executable", default=str(EXECUTABLE))
    parser.add_argument("--out", default=str(ROOT / "tmp/pc/editor-ui"))
    parser.add_argument("--baseline", help="a build without the UI tab's game side, for the no-mod frames")
    parser.add_argument("--skip", default="", help="steps to leave out: title,duel,window,interpreter")
    parser.add_argument("--reference-executable",
                        help="the interpreter step's game, one with the MIPS interpreter (default the reference build)")
    args = parser.parse_args()
    os.environ.setdefault("MEMORIES_NO_MONITOR", "1")
    out = Path(args.out).resolve()
    shots = out / "shots"
    for folder in (out / "mods", shots):
        shutil.rmtree(folder, ignore_errors=True)
        folder.mkdir(parents=True)
    skip = set(filter(None, args.skip.split(",")))
    art = pictures(out / "art")
    print("the mod, through the UI tab:", flush=True)
    for line in make_mod(ROOT / "game", out / "mods" / MOD, art):
        print("  " + line, flush=True)
    mods = out / "mods"
    if "title" not in skip:
        print("the title and its menu:", flush=True)
        title_and_menu(args.executable, out / "runs", mods, shots)
    if "duel" not in skip:
        print("a whole duel:", flush=True)
        play_duel(args.executable, out / "runs", mods, shots, "duel")
    if "window" not in skip:
        print("in a window at Internal 2x:", flush=True)
        window_shots(args.executable, out / "runs", mods, shots)
    if "interpreter" not in skip:
        print("the duel with the effects interpreted:", flush=True)
        from reference_build import reference_executable
        reference = reference_executable(Path(args.reference_executable) if args.reference_executable else None,
                                         subject=Path(args.executable))
        play_duel(str(reference), out / "runs", mods, shots, "interpreter",
                  env={"MEMORIES_DUEL_EFFECTS": "interpreter"}, prefix="30")
    if args.baseline:
        print("without the mod:", flush=True)
        baseline(args.executable, args.baseline, out / "runs", mods)
    failed = [name for name, ok, _ in results if not ok]
    print(f"{len(results) - len(failed)} of {len(results)} checks passed; pictures in {shots}", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
