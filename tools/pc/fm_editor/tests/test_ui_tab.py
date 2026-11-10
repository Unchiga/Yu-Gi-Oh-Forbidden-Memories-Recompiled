"""The UI tab (ui_tab.py, ui_title.py, ui_duel.py) and what it writes: the
menus laid out as title_config.c lays them out, the checks ui_config.c
makes (ui_rules.py), and each page's edits landing in "title", "menu" and
"ui" as the game reads them, saved and opened again.

    python -m unittest discover -s tools/pc/fm_editor/tests -t tools/pc
"""
import struct
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest import mock

from fm_editor import manifest, pngio, ui_assets, ui_rules, validate
from fm_editor.model import Project
from fm_editor.tests.test_data import fixture
from fm_editor.tests.test_gui import GuiCase, tk

# CI's lightweight Python deliberately has no Tk package.  The non-widget
# checks below can still run there; the checks that inspect UiTab itself need
# its tkinter import and are skipped, just like the GUI cases.
try:
    from fm_editor import ui_tab
except ModuleNotFoundError as problem:
    if problem.name != "tkinter":
        raise
    ui_tab = None


def project(other=None) -> Project:
    p = Project(fixture().game())
    p.info.id = "uimod"
    p.other = dict(other or {})
    return p


def png(path: Path, width=16, height=8, color=(200, 40, 40, 255)) -> Path:
    pngio.write(path, pngio.Image(width, height, bytes(color) * (width * height)))
    return path


@unittest.skipIf(tk is None, "ui_title needs Tk")
class SceneTest(unittest.TestCase):
    """ui_title.Scene against tests/pc/title_config_test.c's layout cases."""

    def places(self, other, menu=0):
        from fm_editor.ui_title import Scene
        return Scene(project(other)).places(menu)

    def test_retail(self):
        places = self.places({})
        self.assertEqual([places[n][1] for n in ("new_game", "load", "duel", "trade", "options")], [50, 82, 114, 146, 178])
        self.assertEqual(self.places({}, 1)["campaign"], (160, 42))

    def test_hidden_close_up(self):
        places = self.places({"title": {"entries": {"duel": {"hide": True}, "trade": {"hide": True}}}})
        self.assertEqual((places["new_game"][1], places["load"][1], places["options"][1]), (82, 114, 146))
        self.assertNotIn("duel", places)

    def test_spacing_and_own_y(self):
        places = self.places({"title": {"spacing": 40, "entries": {"options": {"y": 200}}}})
        self.assertEqual((places["new_game"][1], places["load"][1], places["options"][1]), (34, 74, 200))

    def test_all_hidden_shows_the_entries(self):
        hidden = {name: {"hide": True} for name in ("new_game", "load", "duel", "trade", "options")}
        places = self.places({"title": {"entries": hidden}})
        self.assertEqual(places["new_game"][1], 50)

    def test_buttons_and_order(self):
        from fm_editor.ui_title import Scene
        other = {"menu": {"buttons": [{"id": "credits", "label": "CREDITS"},
                                      {"id": "quick", "menu": "second", "label": "QUICK"}],
                          "order": {"first": ["new_game", "credits", "load"]}}}
        scene = Scene(project(other))
        self.assertEqual([i["name"] for i, _ in scene.order(0)],
                         ["new_game", "uimod:credits", "load", "duel", "trade", "options"])
        self.assertEqual([i["name"] for i, _ in scene.order(1)][-1], "uimod:quick")
        self.assertEqual(scene.places(0)["uimod:credits"], (160, 50 + 1 * 32 - 16))

    def test_sizes(self):
        """Each item's "scale", its own or the menu's, and the room it takes:
        the numbers tests/pc/title_config_test.c sizes() holds the game to."""
        from fm_editor.ui_title import Scene
        places = self.places({"menu": {"scale": 120}})
        self.assertEqual([places[n][1] for n in ("new_game", "load", "duel", "trade", "options")],
                         [38, 76, 114, 153, 191])
        places = self.places({"menu": {"scale": 120}}, 1)
        self.assertEqual((places["campaign"][1], places["free_duel"][1], places["save"][1]), (21, 57, 201))
        other = {"menu": {"scale": 80, "entries": {"load": {"scale": 150}},
                          "buttons": [{"id": "big", "label": "BIG", "menu": "second", "scale": 60}]}}
        scene = Scene(project(other))
        self.assertEqual([scene.scale(i) for i, _ in scene.order(0)][:2], [80, 150])
        places = scene.places(0)
        self.assertEqual([places[n][1] for n in ("new_game", "load", "duel", "trade", "options")],
                         [52, 88, 125, 151, 176])
        places = scene.places(1)
        self.assertEqual((places["campaign"][1], places["free_duel"][1], places["uimod:big"][1]), (47, 72, 197))
        places = self.places({"menu": {"entries": {"trade": {"scale": 300, "hide": True}}}})
        self.assertEqual((places["new_game"][1], places["load"][1], places["options"][1]), (66, 98, 162))
        places = self.places({"menu": {"scale": 150, "spacing": 20}})
        self.assertEqual((places["new_game"][1], places["load"][1], places["options"][1]), (54, 84, 174))
        places = self.places({"title": {"spacing": 24}, "menu": {"scale": 150}})
        self.assertEqual((places["new_game"][1], places["load"][1], places["options"][1]), (42, 78, 186))
        # One out of 25 to 400 is left out: the menu's, or 100.
        scene = Scene(project({"menu": {"scale": 500, "entries": {"load": {"scale": "big"}, "duel": {"scale": 10}}}}))
        self.assertEqual([scene.scale(i) for i, _ in scene.order(0)][:3], [100, 100, 100])

    def test_fit(self):
        from fm_editor.ui_title import fit
        self.assertEqual(fit(1280, 960, 320, 240), (320, 240))          # 4x a whole screen
        self.assertEqual(fit(800, 64, 320, 120, want_w=200), (200, 16))
        self.assertEqual(fit(400, 112, 256, 64, guess_h=32), (100, 28))  # an item drawn at 4x


@unittest.skipIf(ui_tab is None, "ui_tab needs Tk")
class RulesTest(unittest.TestCase):
    def test_frames_page_is_a_card_layout_page(self):
        """The UI tab owns the new editor page, including its revert scope
        and its separate frame-image folder."""
        self.assertIn("frames", ui_tab.PAGE_KEYS)
        self.assertEqual(ui_tab.PAGE_KEYS["frames"], [("card_layout", None)])
        self.assertIn("card_layout", ui_tab.KEYS)
        self.assertEqual(ui_tab.FRAME_IMAGE_DIR, "frames")
        self.assertIn("frames", [key for key, *_ in ui_tab.UiTab.PAGES])

    def test_frame_png_keeps_its_folder_when_renamed(self):
        p = project()
        with tempfile.TemporaryDirectory() as folder:
            first = png(Path(folder) / "one.png", color=(1, 2, 3, 255))
            second = png(Path(folder) / "two.png", color=(4, 5, 6, 255))
            self.assertEqual(ui_tab.add_image_file(p, first, "style", "frames"), "frames/style.png")
            self.assertEqual(ui_tab.add_image_file(p, second, "style", "frames"), "frames/style-2.png")

    def test_clean(self):
        p = project({"ui": {"duel": {"lp_player": {"y": -20, "scale": 150, "tint": "#FF0000", "label": "ME",
                                                   "digits": 0x00FF00, "hide": False},
                                     "hand_cursor": {"x": 12, "y": -30, "scale": 400}}}})
        self.assertEqual(ui_rules.check(p, lambda name: True), [])

    def test_sliding_ones(self):
        """What the game slides off the screen sideways moves up and down
        only, no larger than leaves the screen with the game's (the numbers
        tests/pc/ui_config_test.c holds the game to)."""
        self.assertEqual([ui_rules.scale_max(n) for n in ui_rules.ELEMENTS], [161, 161, 130, 100, 400, 400])
        self.assertEqual(ui_rules.scale_max("lp_player", True), 201)
        self.assertEqual(ui_rules.scale_max("field", True, 144, 48), 50)
        self.assertEqual((ui_rules.reach("lp_player", 161), ui_rules.reach("lp_player", 162)), (64, 65))
        p = project({"ui": {"duel": {"field": {"x": 170, "scale": 200}, "lp_player": {"scale": 161},
                                     "lp_opponent": {"image": "a.png", "scale": 201}}}})
        found = [(where, message) for _, where, message in ui_rules.check(p, lambda name: True)]
        self.assertEqual([w for w, _ in found], ["ui.duel.field", "ui.duel.field"])
        self.assertIn("\"x\" is left out", found[0][1])
        self.assertIn("drawn at 130%", found[1][1])

    def test_mistakes(self):
        p = project({"ui": {"duel": {"card_bar": {"x": 4}, "field": {"label": "F", "scale": 999},
                                     "lp": {}, "hand_cursor": {"tint": "red", "image": "../x.png"},
                                     "field_cursor": {"image": "ui/missing.png"}}, "menu": 1},
                     "title": {"images": [{"x": 3}] + [{"image": "a.png"}] * 8}})
        found = {(where, message.split(" ")[0]) for _, where, message in ui_rules.check(p, lambda n: n == "a.png")}
        self.assertIn(("ui.menu", "unknown"), found)
        self.assertIn(("ui.duel.card_bar", "the"), found)
        self.assertIn(("ui.duel.field", "has"), found)
        self.assertIn(("ui.duel.field", "\"scale\""), found)
        self.assertIn(("ui.duel.lp", "no"), found)
        self.assertIn(("ui.duel.hand_cursor", "\"tint\""), found)
        self.assertIn(("ui.duel.hand_cursor", "\"image\""), found)
        self.assertIn(("ui.duel.field_cursor", "ui/missing.png"), found)
        self.assertIn(("title.images", "at"), found)
        self.assertIn(("title.images[0]", "a"), found)

    def test_menu_sizes(self):
        p = project({"menu": {"scale": 120, "entries": {"load": {"scale": 400}},
                              "buttons": [{"id": "a", "label": "A", "scale": 25}]},
                     "title": {"entries": {"options": {"scale": 90}}}})
        self.assertEqual(ui_rules.check(p, lambda name: True), [])
        p = project({"menu": {"scale": 401, "entries": {"load": {"scale": True}},
                              "buttons": [{"id": "a", "label": "A", "scale": 24}]},
                     "title": {"entries": {"options": {"scale": "90"}}}})
        found = [(where, message.split(" ")[0]) for _, where, message in ui_rules.check(p, lambda name: True)]
        self.assertEqual(found, [("menu", "\"scale\""), ("menu.entries.load", "\"scale\""),
                                 ("menu.buttons.a", "\"scale\""), ("title.entries.options", "\"scale\"")])

    def test_card_bar_parts(self):
        """The card bar's parts kept on the bar as ui_config.c keeps them
        (the numbers tests/pc/ui_config_test.c holds the game to)."""
        self.assertEqual(ui_rules.part_home("name"), (0, 2, 224, 12))
        self.assertEqual(ui_rules.part_home("name", 2), (0, 2, 224 + 54, 12))
        self.assertEqual(ui_rules.part_range("name"), (-2, 66, -4, 4))
        self.assertEqual(ui_rules.part_range("atk"), (-197, 55, -2, 10))
        self.assertEqual(ui_rules.part_range("def")[2:], (-10, 2))
        self.assertEqual(ui_rules.part_range("stars"), (-257, 1, -2, 2))
        self.assertEqual(ui_rules.part_range("name", ui_rules.SPACING_MAX)[:2], (-2, 12))
        self.assertEqual(ui_rules.part_offset("atk", {"x": 200, "y": -30}), (55, -2))
        self.assertEqual(ui_rules.part_offset("name", {"x": 80, "spacing": 1}), (39, 0))
        self.assertEqual(ui_rules.part_offset("name", {"x": 999}), (0, 0))         # out of range: left out
        clean = project({"ui": {"duel": {"card_bar": {"tint": "#C0C0FF", "name": {"x": 30, "spacing": 1,
                                                                                   "tint": "#FFE040"},
                                                      "atk": {"x": 55}, "stars": {"hide": True}}}}})
        self.assertEqual(ui_rules.check(clean, lambda name: True), [])
        wrong = project({"ui": {"duel": {"card_bar": {"name": 3, "atk": {"spacing": 1, "spin": 2, "x": 200},
                                                      "def": {"tint": "blue"}, "type": {"hide": 1},
                                                      "kind": {"y": 9999}}}}})
        found = [(where, message) for _, where, message in ui_rules.check(wrong, lambda name: True)]
        self.assertEqual([w for w, _ in found], ["ui.duel.card_bar.name", "ui.duel.card_bar.atk",
                                                 "ui.duel.card_bar.atk", "ui.duel.card_bar.atk",
                                                 "ui.duel.card_bar.def", "ui.duel.card_bar.type",
                                                 "ui.duel.card_bar.kind"])
        self.assertIn("moved by 55, 0 instead", found[3][1])

    def test_validate_takes_the_keys(self):
        p = project({"title": {"logo": {"tint": "#FF0000"}}, "menu": {"spacing": 28}, "ui": {"duel": {}}})
        issues = validate.validate(p)
        self.assertFalse([i for i in issues if i.area == "Mod info" and i.where in ("title", "menu", "ui")])


class AssetsTest(unittest.TestCase):
    def test_vram_sprite(self):
        vram = ui_assets.Vram()
        # A 4-bit texel 3 at u 1 of page (64, 0), palette 0 at (0, 10): red.
        vram.words[0 * 1024 + 64] = 0x0030
        vram.words[10 * 1024 + 3] = 0x001F
        image = vram.sprite(64, 0, 4, 0, 10, 0, 0, 2, 1)
        self.assertEqual(image.pixel(0, 0), (0, 0, 0, 0))
        self.assertEqual(image.pixel(1, 0), (255, 0, 0, 255))
        self.assertEqual(vram.sprite(64, 0, 4, 0, 10, 0, 0, 2, 1, mirrored=True).pixel(0, 0), (255, 0, 0, 255))

    def test_columns(self):
        vram = ui_assets.Vram()
        data = bytearray(2048 * 17)
        struct.pack_into("<H", data, 2048 * 16, 0x1234)      # sector 16: the second column's first tile
        vram.columns(bytes(data), 0, 17, 512, 256)
        self.assertEqual(vram.words[256 * 1024 + 512 + 64], 0x1234)

    def test_without_the_disc(self):
        self.assertFalse(ui_assets.TitleArt(None).ok)
        self.assertFalse(ui_assets.DuelArt(b"").ok)

    def test_tint_and_subtract(self):
        image = pngio.Image(1, 1, bytes((200, 100, 50, 255)))
        self.assertEqual(ui_assets.tint(image, 0x808080).pixel(0, 0)[:3], (100, 50, 25))
        self.assertEqual(ui_assets.subtract(image, 64).pixel(0, 0), (136, 36, 0, 255))
        self.assertEqual(ui_assets.parse_color("#80c0ff"), 0x80C0FF)
        self.assertEqual(ui_assets.parse_color("nope", 7), 7)


class DuelScreenTest(unittest.TestCase):
    """duel_screen.py: the duel's opening screen under the Duel page's pictures."""

    def test_without_the_disc(self):
        from fm_editor import duel_screen
        image = duel_screen.board_backdrop(None)
        self.assertEqual(image.size, (320, 240))
        self.assertEqual(image.pixel(160, 120), (0, 0, 0, 255))


class BarWordsTest(unittest.TestCase):
    """duel_screen.bar_words: the card bar's parts moved, colored, hidden
    and the name's letters spread as duel_ui.c draws them."""

    def words(self, card_bar=None, cid=None):
        from types import SimpleNamespace
        from fm_editor import duel_screen

        class Font:
            colors = [None, (255, 255, 255)]

            def cell(self, c):
                return [1] * (8 * 12)

            def icon(self, n):
                return (16, 16, bytes((255, 255, 255, 255)) * 256)
        p = project()
        vram = SimpleNamespace(sprite=lambda *a, **k: pngio.Image(a[-2] if len(a) > 8 else 8, 8,
                                                                  bytes((255, 255, 255, 255)) * 64 *
                                                                  (a[-2] // 8 if len(a) > 8 else 1)))
        duel = SimpleNamespace(vram=vram)
        cid = cid or next(c for c in sorted(p.cards) if p.cards[c].is_monster() and len(p.cards[c].name) > 4)
        return p.cards[cid], duel_screen.bar_words(p, None, duel, Font(), cid, card_bar)

    def test_parts(self):
        card, retail = self.words()
        places = {part: [(x, y) for _, x, y, p in retail if p == part] for part in ui_rules.PARTS}
        self.assertEqual(places["name"][0], (16, 212))
        self.assertEqual(places["atk"][0], (211, 210))
        self.assertEqual(places["def"][0], (211, 218))
        self.assertEqual(places["type"], [(253, 210)])
        self.assertEqual(places["stars"][0], (271, 210))
        bar = {"name": {"x": 10, "y": 2, "spacing": 2, "tint": "#808080"}, "atk": {"x": -195}, "stars": {"hide": True},
               "type": {"x": 999}}
        _, moved = self.words(bar)
        got = {part: [(x, y) for _, x, y, p in moved if p == part] for part in ui_rules.PARTS}
        name = [i for i, c in enumerate(card.name[:24]) if c != " "]
        self.assertEqual(got["name"], [(16 + 10 + 10 * i, 214) for i in name])    # 8 + 2 a letter, spaces too
        self.assertEqual(got["atk"][0], (16, 210))
        self.assertEqual(got["def"], places["def"])
        self.assertEqual(got["stars"], [])
        self.assertEqual(got["type"], [(253, 210)])                                  # out of range: left out
        picture = next(pic for pic, _, _, p in moved if p == "name")
        self.assertEqual(picture.pixel(0, 0)[:3], (128, 128, 128))


class UiTabTest(GuiCase):
    def setUp(self):
        super().setUp()
        self.tab = self.app.ui
        self.app.notebook.select(self.tab)
        self.app.update()
        self.folder = Path(tempfile.mkdtemp())
        self.picture = png(self.folder / "art.png", 64, 20)

    def choose(self, path):
        return mock.patch("fm_editor.file_dialogs.askopenfilename", return_value=str(path))

    def page(self, name):
        self.tab.page_name.set(name)
        self.tab.show_page()
        self.app.update()
        return self.tab.pages[name]

    def test_duel_page(self):
        page = self.page("duel")
        page.select("lp_player")
        page.moved("lp_player", -230, 12)       # up and down only: the x is left out
        page.wheel("lp_player", 1)
        page.wheel("lp_player", 1)
        page.tint.set(0xFF8080)
        page.set_color("tint", 0xFF8080)
        page.set_color("digits", 0x80FF80)
        page.vars["label"].set("ME")
        page.typed("label")
        with self.choose(self.picture):
            page.choose_image()
        page.select("card_bar")
        page.moved("card_bar", 10, 10)          # stays put: nothing written
        page.set_color("tint", 0xC0C0FF)
        page.select("field_cursor")
        page.hidden.set(True)
        page.set_hidden()
        duel = self.app.project.other["ui"]["duel"]
        self.assertEqual(duel["lp_player"], {"y": 12, "scale": 120, "tint": "#FF8080", "digits": "#80FF80",
                                             "label": "ME", "image": "ui/duel-lp-player.png"})
        self.assertEqual(duel["card_bar"], {"tint": "#C0C0FF"})
        self.assertEqual(duel["field_cursor"], {"hide": True})
        self.assertIn("ui/duel-lp-player.png", self.app.project.files)
        self.assertTrue(self.app.dirty)
        # Back to the game's: the element and then the whole key go.
        page.select("field_cursor")
        page.reset()
        self.assertNotIn("field_cursor", self.app.project.other["ui"]["duel"])
        with mock.patch("fm_editor.ui_tab.messagebox.askyesno", return_value=True):
            self.tab.revert_page(page)
        self.assertNotIn("ui", self.app.project.other)
        self.assertNotIn("ui/duel-lp-player.png", self.app.project.files)

    def test_card_bar_parts(self):
        """The card bar's parts, listed under it: each moved on the bar only,
        colored, hidden, the name spread; back to the game's one by one."""
        page = self.page("duel")
        rows = page.list.tree.get_children("card_bar")
        self.assertEqual(rows, tuple(f"card_bar.{p}" for p in ui_rules.PARTS))
        page.select("card_bar.name")
        self.assertEqual(page.title.cget("text"), "Card bar: Name")
        self.assertTrue(page.spacing_box.winfo_ismapped() or page.spacing_box.grid_info())
        self.assertFalse(page.size_row.grid_info())
        self.assertFalse(page.picture_row.grid_info())
        page.moved("card_bar.name", 300, 1)              # kept on the bar
        page.set_color("tint", 0xFFE040)
        page.vars["spacing"].set("2")
        page.typed("spacing")                            # spread: brought back on the bar
        page.select("card_bar.atk")
        self.assertFalse(page.spacing_box.grid_info())
        page.moved("card_bar.atk", -195, 0)
        page.vars["y"].set("-40")
        page.typed("y")
        page.select("card_bar.stars")
        page.hidden.set(True)
        page.set_hidden()
        page.select("card_bar")
        page.set_color("tint", 0xC0C0FF)
        bar = self.app.project.other["ui"]["duel"]["card_bar"]
        self.assertEqual(bar, {"name": {"x": 12, "y": 1, "tint": "#FFE040", "spacing": 2},
                               "atk": {"x": -195, "y": -2}, "stars": {"hide": True}, "tint": "#C0C0FF"})
        self.assertEqual(ui_rules.check(self.app.project, lambda name: True), [])
        self.assertTrue(page.list.tree.item("card_bar.name", "values")[-1])
        page.select("card_bar.name")
        page.reset()
        page.select("card_bar.atk")
        page.reset()
        page.select("card_bar.stars")
        page.reset()
        self.assertEqual(self.app.project.other["ui"]["duel"]["card_bar"], {"tint": "#C0C0FF"})
        page.select("card_bar")
        page.reset()
        self.assertNotIn("card_bar", self.app.project.other.get("ui", {}).get("duel", {}))

    def test_title_page(self):
        page = self.page("title")
        page.select("background")
        page.background.set("color", "#102040")
        page.background.picture.set(False)
        page.background.set("picture", False, True)
        with self.choose(self.picture):
            page.add_picture()
        page.moved(("picture", 0), 10, -60)
        page.add_text()
        page.text_vars["text"].set("v1.0")
        page.text_typed("text")
        page.moved(("text", 0), 100, 0)
        page.select("logo")
        page.moved("logo", 0, -20)
        page.set_layer("tint", "#FFD060", "#FFFFFF")
        page.select("copyright")
        page.layer_hidden.set(True)
        page.set_layer("hide", True)
        title = self.app.project.other["title"]
        self.assertEqual(title["background"], {"color": "#102040", "picture": False})
        self.assertEqual(title["images"], [{"image": "ui/title-picture.png", "x": 170, "y": 60}])
        self.assertEqual(title["text"][0]["text"], "v1.0")
        self.assertEqual(title["text"][0]["x"], 260)
        self.assertEqual(title["logo"], {"y": -20, "tint": "#FFD060"})
        self.assertEqual(title["copyright"], {"hide": True})
        page.select(("picture", 0))
        page.remove_picture()
        self.assertNotIn("images", self.app.project.other["title"])

    def test_menu_page(self):
        page = self.page("menu")
        page.add_button()
        name = f"{self.app.project.info.id}:button1"
        self.assertEqual(page.chosen, name)
        page.item_vars["label"].set("CREDITS")
        page.item_typed("label")
        page.move(-1)
        page.move(-1)
        page.chosen = "trade"
        page.toggle_hidden()
        page.chosen = "options"
        page.item_vars["label"].set("SETTINGS")
        page.item_typed("label")
        page.chosen = name
        page.fill_form()
        page.action_box.current(page.action_choices.index("quit"))
        page.set_action()
        menu = self.app.project.other["menu"]
        self.assertEqual(menu["buttons"][0]["label"], "CREDITS")
        self.assertEqual(menu["buttons"][0]["action"], "quit")
        self.assertEqual(menu["order"]["first"], ["new_game", "load", "duel", "button1", "trade", "options"])
        self.assertEqual(menu["entries"]["trade"], {"hide": True})
        self.assertEqual(menu["entries"]["options"], {"label": "SETTINGS"})
        page.remove()
        self.assertNotIn("buttons", self.app.project.other["menu"])
        self.assertNotIn("button1", self.app.project.other["menu"]["order"]["first"])

    def test_menu_sizes(self):
        """Sizes through the page's own controls: an item's Size, the wheel
        over it, All buttons; what can look off said so; saved and opened."""
        page = self.page("menu")
        page.chosen = "load"
        page.fill_form()
        self.assertEqual(page.item_vars["scale"].get(), "100")
        page.item_vars["scale"].set("150")
        page.item_typed("scale")
        menu = self.app.project.other["menu"]
        self.assertEqual(menu["entries"]["load"], {"scale": 150})
        page.wheel(("item", "load"), 1)
        page.wheel(("item", "load"), 1)
        self.assertEqual(menu["entries"]["load"]["scale"], 170)
        page.slid("60")
        self.assertEqual(menu["entries"]["load"]["scale"], 60)
        page.item_vars["scale"].set("999")
        page.item_typed("scale")
        self.assertEqual(menu["entries"]["load"]["scale"], 400)
        # All buttons: every item's size but those with their own; one at the
        # new size needs none of its own.
        page.add_button()
        button = page.chosen
        page.all_scale.set("120")
        page.all_typed()
        self.assertEqual(menu["scale"], 120)
        page.chosen = button
        page.fill_form()
        self.assertEqual(page.item_vars["scale"].get(), "120")
        self.assertEqual(page.scale_note.cget("text"), "all buttons'")
        page.all_scale.set("400")
        page.all_typed()
        self.assertNotIn("load", menu.get("entries", {}))   # at 400 like all the others: nothing of its own
        # At 400 the menu's items run into each other, and LOAD past the screen.
        page.chosen = "load"
        page.draw()
        text = page.warning.cget("text")
        self.assertIn("Runs into", text)
        self.assertIn("\u26a0", text)
        page.all_scale.set("100")
        page.all_typed()
        self.assertNotIn("scale", menu)
        page.draw()
        self.assertEqual(page.warning.cget("text"), "")
        # A button too wide for the screen at 300, moved right.
        page.chosen = button
        page.fill_form()
        page.item_vars["x"].set("120")
        page.item_typed("x")
        page.item_vars["scale"].set("300")
        page.item_typed("scale")
        self.assertIn("past the edge of the screen", page.warning.cget("text"))
        folder = self.folder / "sized"
        manifest.save_mod(self.app.project, folder)
        opened, messages = manifest.open_mod(self.app.retail, folder)
        self.assertEqual(opened.other["menu"], self.app.project.other["menu"])
        self.assertFalse([i for i in validate.validate(opened) if i.area == "UI"], messages)

    def test_saved_and_opened_again(self):
        page = self.page("duel")
        page.select("field")
        page.moved("field", 240, 0)
        with self.choose(self.picture):
            page.choose_image()
        title = self.page("title")
        with self.choose(self.picture):
            title.add_picture()
        folder = self.folder / "mod"
        manifest.save_mod(self.app.project, folder)
        self.assertTrue((folder / "ui" / "duel-field.png").is_file())
        self.assertTrue((folder / "ui" / "title-picture.png").is_file())
        opened, messages = manifest.open_mod(self.app.retail, folder)
        self.assertEqual(opened.other["ui"], self.app.project.other["ui"])
        self.assertEqual(opened.other["title"], self.app.project.other["title"])
        self.assertFalse([i for i in validate.validate(opened) if i.area == "UI"], messages)
        # Shown again from the folder (the pictures are read from it).
        self.app.set_project(opened)
        self.page("duel").draw()
        self.page("title").draw()

    def edit_every_page(self):
        duel = self.page("duel")
        duel.select("field")
        duel.moved("field", 0, 40)
        with self.choose(self.picture):
            duel.choose_image()
        title = self.page("title")
        with self.choose(self.picture):
            title.add_picture()
        title.select("logo")
        title.moved("logo", 0, -10)
        menu = self.page("menu")
        menu.add_button()
        menu.chosen = "trade"
        menu.toggle_hidden()
        self.app.project.other["keep"] = {"a": 1}
        self.app.flush_history()

    def test_revert_to_retail_and_undo(self):
        self.edit_every_page()
        other = self.app.project.other
        before = {key: other[key] for key in ("title", "menu", "ui")}
        self.assertEqual(sorted(n for n in self.app.project.files if n.startswith("ui/")),
                         ["ui/duel-field.png", "ui/title-picture.png"])
        self.assertEqual(self.tab.revert_button.instate(["disabled"]), False)
        with mock.patch("fm_editor.ui_tab.messagebox.askyesno", return_value=False) as asked:
            self.tab.revert_all()
        self.assertIn("2 picture(s) in ui/", asked.call_args[0][1])
        self.assertIn("1 button(s) of the mod's own", asked.call_args[0][1])
        self.assertEqual(self.app.project.other["ui"], before["ui"])           # said no: nothing changed
        with mock.patch("fm_editor.ui_tab.messagebox.askyesno", return_value=True):
            self.tab.revert_all()
        self.assertEqual(list(self.app.project.other), ["keep"])               # the tab's keys only
        self.assertFalse([n for n in self.app.project.files if n.startswith("ui/")])
        self.assertTrue(self.tab.revert_button.instate(["disabled"]))
        for name in ("title", "menu", "duel"):
            self.page(name)                                                      # each draws as the game's
        # One undo step brings all of it back, the pictures too.
        self.app.undo()
        self.app.update()
        self.assertEqual({key: self.app.project.other[key] for key in ("title", "menu", "ui")}, before)
        self.assertIn("ui/duel-field.png", self.app.project.files)
        self.assertIn("ui/title-picture.png", self.app.project.files)
        self.app.redo()
        self.assertEqual(list(self.app.project.other), ["keep"])

    def test_revert_page(self):
        self.edit_every_page()
        menu_before = self.app.project.other["menu"]
        self.app.project.other["title"]["spacing"] = 40          # the menus' own, under "title"
        with mock.patch("fm_editor.ui_tab.messagebox.askyesno", return_value=True):
            self.tab.revert_page(self.tab.pages["title"])
        self.assertEqual(self.app.project.other["title"], {"spacing": 40})
        self.assertEqual(self.app.project.other["menu"], menu_before)
        self.assertNotIn("ui/title-picture.png", self.app.project.files)
        self.assertIn("ui/duel-field.png", self.app.project.files)
        with mock.patch("fm_editor.ui_tab.messagebox.askyesno", return_value=True):
            self.tab.revert_page(self.tab.pages["menu"])
        self.assertNotIn("menu", self.app.project.other)
        self.assertNotIn("title", self.app.project.other)
        self.assertIn("ui", self.app.project.other)
        self.assertTrue(self.tab.pages["menu"].revert_page.instate(["disabled"]))

    def test_drag_at_a_bigger_zoom(self):
        """The preview grows with the room; a drag is in the game's pixels at any zoom."""
        from types import SimpleNamespace as Event
        page = self.page("duel")
        with mock.patch.object(page.view, "winfo_width", return_value=1000), \
                mock.patch.object(page.view, "winfo_height", return_value=800):
            page.view.seen = None
            page.view.fit()
        self.assertEqual(page.stage.zoom, 3)
        x, y, w, h = page.stage.boxes["field"]
        at = (int((x + w / 2) * 3), int((y + h / 2) * 3))
        page.stage._press(Event(x=at[0], y=at[1]))
        self.assertEqual(page.chosen, "field")
        page.stage._motion(Event(x=at[0] + 31, y=at[1] - 14))      # 10.3 and -4.7 of the game's pixels
        self.assertEqual(page.vars["y"].get(), "-5")                # the form, live; up and down only
        page.stage._release(Event(x=at[0] + 31, y=at[1] - 14))
        self.assertEqual(self.app.project.other["ui"]["duel"]["field"], {"y": -5})
        page.stage._nudge(Event(state=0), 1, 0)                    # an arrow across: nothing
        page.stage._nudge(Event(state=1), 0, 1)                    # Shift: eight
        self.assertEqual(self.app.project.other["ui"]["duel"]["field"], {"y": 3})
        # The hand's cursor moves both ways; the card bar not at all.
        x, y, w, h = page.stage.boxes["hand_cursor"]
        at = (int((x + w / 2) * 3), int((y + h / 2) * 3))
        page.stage._press(Event(x=at[0], y=at[1]))
        page.stage._motion(Event(x=at[0] + 30, y=at[1] - 15))
        page.stage._release(Event(x=at[0] + 30, y=at[1] - 15))
        self.assertEqual(self.app.project.other["ui"]["duel"]["hand_cursor"], {"x": 10, "y": -5})
        page.stage._press(Event(x=160 * 3, y=230 * 3))
        self.assertEqual(page.chosen, "card_bar")
        page.stage._motion(Event(x=170 * 3, y=200 * 3))
        page.stage._release(Event(x=170 * 3, y=200 * 3))
        page.stage._nudge(Event(state=0), 1, 1)
        self.assertNotIn("card_bar", self.app.project.other["ui"]["duel"])
        with mock.patch.object(page.view, "winfo_width", return_value=700), \
                mock.patch.object(page.view, "winfo_height", return_value=2000):
            page.view.seen = None
            page.view.fit()
        self.assertEqual(page.stage.zoom, 2)

    def test_frames_preview_is_a_scaled_card(self):
        """Frames uses all its column for the 140x196 card, not a 320x240 screen."""
        page = self.page("frames")
        self.assertEqual((page.stage.game_width, page.stage.game_height), (140, 196))
        self.assertTrue(page.view.smooth)
        with mock.patch.object(page.view, "winfo_width", return_value=800), \
                mock.patch.object(page.view, "winfo_height", return_value=600):
            page.view.seen = None
            page.view.fit()
        self.assertAlmostEqual(page.stage.zoom, (600 - page.view.tools.winfo_reqheight() - 6) / 196)

    def test_frames_label_on_the_games_own_frame(self):
        """A style with no frame image keeps retail's frame, and the game
        draws retail's ATK/DFD caption there until a picture replaces it: the
        preview keeps that caption still, then the picture starts over it."""
        from fm_editor import ui_frames
        page = self.page("frames")
        layout = self.app.project.other.setdefault("card_layout", {})
        layout["frame_styles"] = {"hp": {"hand_color": "gold"}}
        page.style_name.set("hp")
        style = layout["frame_styles"]["hp"]
        page.chosen = page.list_chosen = "dfd_label"
        self.assertTrue(page.keeps_retail_frame())
        self.assertTrue(page.fixed_caption("dfd_label"))
        self.assertEqual(page.rect("dfd_label"), ui_frames.RETAIL_CAPTIONS["dfd_label"])
        page.paint()
        self.assertNotIn("dfd_label", page.stage.draggable)         # retail's own sprite: nothing to drag
        page.moved("dfd_label", 5, 5); page.wheel("dfd_label", 1)
        self.assertEqual(style, {"hand_color": "gold"})              # and nothing written
        # A picture replaces it, starting where retail's caption is.
        page.set_asset("dfd_label", "frames/hp.png")
        self.assertEqual(style["dfd_label"], {"image": "frames/hp.png", "x": 72, "y": 173, "width": 21, "height": 8})
        self.assertFalse(page.fixed_caption("dfd_label"))
        page.paint()
        self.assertIn("dfd_label", page.stage.draggable)
        page.moved("dfd_label", -72, -173)                           # the game's own (0, 0) is written too
        self.assertEqual((style["dfd_label"]["x"], style["dfd_label"]["y"]), (0, 0))
        self.assertEqual(page.rect("dfd_label"), (0, 0, 21, 8))
        page.chosen = "dfd_label"
        for name, value in (("x", 12), ("y", 184)):                 # the editor's old start places, typed
            page.vars[name].set(str(value)); page.typed(name)
        self.assertEqual((style["dfd_label"]["x"], style["dfd_label"]["y"]), (12, 184))
        self.assertEqual(page.rect("dfd_label")[:2], (12, 184))
        del style["dfd_label"]["x"], style["dfd_label"]["y"]         # a hand-written one: the game's places
        self.assertEqual(page.rect("dfd_label")[:2], (0, 0))
        page.set_asset("dfd_label", None)
        self.assertTrue(page.fixed_caption("dfd_label"))
        # A style with a frame image of its own: the editor's start places, as before.
        style["image"] = "frames/f.png"
        self.assertFalse(page.keeps_retail_frame())
        self.assertFalse(page.fixed_caption("atk_label"))
        style["dfd_label"] = {}
        page.set_asset("atk_label", "frames/a.png")
        self.assertEqual(style["atk_label"], {"image": "frames/a.png", "x": 12, "y": 171, "width": 16, "height": 8})
        # Replacing the picture of a hand-written label with no x/y moves nothing: it is at the game's (0, 0).
        style["dfd_label"] = {"image": "frames/old.png"}
        page.set_asset("dfd_label", "frames/new.png")
        self.assertEqual(style["dfd_label"], {"image": "frames/new.png"})
        self.assertEqual(page.rect("dfd_label"), (0, 0, 16, 8))
        style["full_bleed"] = False                                  # a frame image the style says not to use
        self.assertTrue(page.keeps_retail_frame())
        style["full_bleed"] = 0                                      # the game reads a number as a bool
        self.assertTrue(page.keeps_retail_frame())
        style["full_bleed"] = 1
        self.assertFalse(page.keeps_retail_frame())
        del style["image"]
        self.assertFalse(page.keeps_retail_frame())                  # 1 without an image: full-bleed, as the game has it

    def test_frames_digits_on_the_games_own_frame(self):
        """ATK/DFD left where they are: the game's own places (0x61, 0x9D and
        0x61, 0xAB), a box centre on a style's own frame and, on one that
        keeps retail's frame, the corner of retail's 24 x 13 digit row, which
        the game centres a strip on (func_80028B08.c)."""
        from fm_editor import pngio, ui_frames
        page = self.page("frames")
        layout = self.app.project.other.setdefault("card_layout", {})
        layout["frame_styles"] = {"hp": {"hand_color": "gold"}}
        page.style_name.set("hp")
        style = layout["frame_styles"]["hp"]
        self.assertEqual(page.rect("atk")[:2], (97, 157))
        self.assertEqual(page.rect("def")[:2], (97, 171))
        page.moved("atk", 0, 12)                                     # y 169 is not the game's: it is written
        self.assertEqual(style["digits"]["atk"], {"y": 169})
        del style["digits"]
        strip = pngio.Image(200, 192, bytes(200 * 192 * 4))
        real = ui_frames.mod_image
        with mock.patch.object(ui_frames, "mod_image",
                               side_effect=lambda project, name: strip if name == "frames/d.png" else real(project, name)):
            # (Retail's own digits, with no strip, are previewed as text, whose
            # box Tk measures in the system's font: not compared here.)
            style["digits"] = {"image": "frames/d.png", "width": 10, "height": 12}
            page.paint()
            # 3000 in 10-wide digits, 40 x 12, centred on (109, 163)
            self.assertEqual(page.stage.boxes["atk"], (89, 157, 40, 12))
            self.assertEqual(page.stage.boxes["def"], (89, 171, 40, 12))
            style["image"] = "frames/f.png"                              # a frame of its own: x/y is the centre
            page.paint()
            self.assertEqual(page.stage.boxes["atk"], (77, 151, 40, 12))

    def test_deleting_frame_style_clears_its_assignments(self):
        """Deleting a referenced style must not leave stale fallback rules."""
        from fm_editor import ui_frames
        page = self.page("frames")
        layout = self.app.project.other.setdefault("card_layout", {})
        layout.update({
            "frame_styles": {"remove-me": {"hand_color": "gold"}, "keep-me": {}},
            "default_style": "remove-me",
            "default_styles": {"kinds": {"monster": "remove-me"},
                               "types": {"Dragon": "remove-me"}},
            "frame_for": [{"class": "trap", "style": "remove-me"}, {"class": "spell", "style": "keep-me"}],
        })
        self.app.project.card_extra[1] = {"frame_style": "remove-me", "art": "images/keep.png"}
        self.app.project.card_extra[999] = {"frame_style": "remove-me"}  # stale project data is still editable
        added = self.app.project.add_card(1)
        self.app.project.added[added].extra["frame_style"] = "remove-me"
        page.style_name.set("remove-me")
        with mock.patch.object(ui_frames.messagebox, "askyesno", return_value=False) as ask:
            page.delete_style()
        self.assertIn("remove-me", layout["frame_styles"])
        self.assertEqual(layout["default_style"], "remove-me")
        self.assertTrue(ask.called)
        self.assertIn("Card:", ask.call_args.args[1])
        self.assertIn("Card: 999", ask.call_args.args[1])
        self.assertIn("Rule: class trap", ask.call_args.args[1])

        with mock.patch.object(ui_frames.messagebox, "askyesno", return_value=True):
            page.delete_style()
        self.assertNotIn("remove-me", layout["frame_styles"])
        self.assertIn("keep-me", layout["frame_styles"])   # an empty style is still a style
        self.assertEqual(layout["frame_for"], [{"class": "spell", "style": "keep-me"}])
        self.assertNotIn("default_style", layout)
        self.assertNotIn("default_styles", layout)
        self.assertEqual(self.app.project.card_extra[1], {"art": "images/keep.png"})
        self.assertNotIn(999, self.app.project.card_extra)
        self.assertNotIn("frame_style", self.app.project.added[added].extra)

    def test_hold_the_games(self):
        page = self.page("duel")
        page.moved("lp_player", 0, 40)
        self.tab.comparing_now(True)
        self.assertEqual(self.app.project.other["ui"]["duel"]["lp_player"], {"y": 40})   # drawn without it only
        self.assertEqual(page.stage.boxes["lp_player"][1], ui_assets.DUEL_RECTS["lp_player"][1])
        self.tab.comparing_now(False)
        self.assertEqual(page.stage.boxes["lp_player"][1], ui_assets.DUEL_RECTS["lp_player"][1] + 40)

    def test_rough_edges(self):
        from fm_editor.ui_duel import rough_edges
        from fm_editor.ui_tab import brightens
        self.assertEqual(rough_edges("lp_player", {}), [])
        self.assertEqual(rough_edges("lp_player", {"y": -10, "scale": 161}), [])    # slides with the game's
        self.assertEqual(len(rough_edges("field", {"y": 40})), 1)                    # over the field's cards
        self.assertEqual(len(rough_edges("lp_player", {"x": 4})), 1)                # a hand-written x
        self.assertEqual(len(rough_edges("field", {"scale": 200})), 1)               # drawn at 130 %
        self.assertEqual(rough_edges("hand_cursor", {"x": 4, "scale": 400}), [])
        self.assertEqual(len(rough_edges("lp_opponent", {"label": "RIVAL\u00c9", "image": "ui/a.png"})), 2)
        from fm_editor.ui_duel import part_rough_edges
        self.assertEqual(part_rough_edges("atk", {}), [])
        self.assertEqual(len(part_rough_edges("atk", {"atk": {"x": -100}})), 1)     # where a long name reaches
        self.assertEqual(part_rough_edges("atk", {"atk": {"x": -2}}), [])           # a monster's: 24 letters
        self.assertEqual(len(part_rough_edges("type", {"type": {"x": -14}})), 1)    # a magic card's: 28
        self.assertEqual(part_rough_edges("atk", {"atk": {"x": -100}, "name": {"hide": True}}), [])
        self.assertEqual(len(part_rough_edges("name", {"type": {"x": -100}})), 1)
        self.assertEqual(len(part_rough_edges("def", {}, picture=True)), 1)       # dark cells over a picture
        self.assertEqual(part_rough_edges("type", {}, picture=True), [])
        self.assertTrue(brightens(0xFFFF40))
        self.assertFalse(brightens(0xFFFFFF))
        self.assertFalse(brightens(0x404040))

    def test_pages_draw_without_the_disc(self):
        for name in ("title", "menu", "duel"):
            page = self.page(name)
            page.fill()
        self.assertFalse(self.tab.title_art().ok)
