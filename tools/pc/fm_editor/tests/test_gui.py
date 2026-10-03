"""The window, driven as a user would, on synthetic game files. Skipped
where there is no Tk or no display (Tk cannot start)."""
import json
import tempfile
import unittest
from unittest import mock

try:
    import tkinter as tk
except ImportError:     # a Python built without Tk
    tk = None
from pathlib import Path

from fm_editor.tests.test_data import fixture


class GuiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if tk is None:
            raise unittest.SkipTest("this Python has no Tk")
        try:
            probe = tk.Tk()
            probe.destroy()
        except tk.TclError as problem:
            raise unittest.SkipTest(f"no display for Tk: {problem}")
        cls.tmp = tempfile.TemporaryDirectory()
        folder = Path(cls.tmp.name) / "game"
        (folder / "DATA").mkdir(parents=True)
        f = fixture()
        (folder / "SLUS_014.11").write_bytes(f.slus)
        (folder / "DATA" / "WA_MRG.MRG").write_bytes(f.wa)
        cls.game = folder

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        from fm_editor import settings
        from fm_editor.app import App
        # Never the user's own settings (a dark mode they chose, say).
        self.settings = Path(self.tmp.name) / "config" / "settings.json"
        self.settings.unlink(missing_ok=True)
        patcher = mock.patch.object(settings, "path", lambda: self.settings)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.app = App(ask=False, autostart=False)
        self.app.withdraw()
        self.app.update()
        self.app.start(str(self.game), None, False)
        self.app.update()

    def tearDown(self):
        self.app.dirty = False
        self.app.destroy()

    def click_heading(self, tree, column):
        tree.tk.call(tree.heading(column, "command"))

    def test_card_heading_sort_and_pending_edit(self):
        tab, p = self.app.cards, self.app.project
        for cid, name, attack, defense, kind in (
                (2, "Sort zebra", 100, 2000, 0),
                (10, "Sort alpha", 2000, 90, 1),
                (100, "Sort Bravo", 900, 100, 2)):
            p.cards[cid] = p.cards[cid].copy(name=name, attack=attack, defense=defense, type=kind)
        tab.search.set("Sort ")
        tab.tree.selection_set("10")
        tab.select()
        tab.vars["name"].set("Pending name")
        self.app.dirty = False
        expected = {"id": ("2", "10", "100"), "name": ("10", "100", "2"),
                    "atk": ("2", "100", "10"), "def": ("10", "100", "2")}
        from fm_editor.gamedata import TYPE_NAMES
        expected["type"] = tuple(str(cid) for cid in sorted(
            (2, 10, 100), key=lambda cid: TYPE_NAMES[p.cards[cid].type].casefold()))
        for column, ascending in expected.items():
            with self.subTest(column=column):
                self.click_heading(tab.tree, column)
                self.assertEqual(tab.tree.get_children(), ascending)
                self.assertTrue(tab.tree.heading(column, "text").endswith("▲"))
                self.click_heading(tab.tree, column)
                self.assertEqual(tab.tree.get_children(), tuple(reversed(ascending)))
                self.assertTrue(tab.tree.heading(column, "text").endswith("▼"))
        self.app.update()
        self.assertEqual(tab.tree.selection(), ("10",))
        self.assertEqual(tab.vars["name"].get(), "Pending name")
        self.assertEqual(p.cards[10].name, "Sort alpha")
        self.assertFalse(self.app.dirty)
        # Filtering and editing a sort value both retain the active order.
        self.click_heading(tab.tree, "atk")
        tab.search.set("Sort a")
        tab.search.set("Sort ")
        self.assertEqual(tab.tree.get_children(), expected["atk"])
        tab.vars["name"].set("Sort alpha")
        tab.vars["attack"].set("50")
        self.assertTrue(tab.apply())
        self.assertEqual(tab.tree.get_children(), ("10", "2", "100"))

    def test_equips_and_duelist_heading_sorts(self):
        app, p = self.app, self.app.project
        for cid, attack, defense in ((2, 100, 2000), (10, 2000, 90), (100, 900, 100)):
            p.cards[cid] = p.cards[cid].copy(attack=attack, defense=defense)
        equips, duelists = app.equips, app.duelists
        equip = p.equip_cards()[0]
        p.equips[equip] = {2, 10, 100}
        equips.current = equip
        equips.fill_equips()
        equips.fill()
        before = set(equips.monsters.get_children())
        self.click_heading(equips.monsters, "atk")
        selected = tuple(sorted(before, key=lambda cid: (p.cards[int(cid)].attack, int(cid))))
        self.assertEqual(equips.monsters.get_children(), selected)
        equips.monsters.selection_set("2", "10")
        self.click_heading(equips.monsters, "atk")
        self.assertEqual(set(equips.monsters.selection()), {"2", "10"})
        equips.remove()
        self.assertEqual(p.equips[equip], {100})
        self.assertTrue(equips.monsters.heading("atk", "text").endswith("▼"))
        # The left-hand lists sort too, retaining their order on refill.
        for tree, refill in ((equips.equips, equips.fill_equips), (duelists.list, duelists.fill_list)):
            self.click_heading(tree, "id")
            self.click_heading(tree, "id")
            expected = tuple(sorted(tree.get_children(), key=int, reverse=True))
            refill()
            self.assertEqual(tree.get_children(), expected)
            self.click_heading(tree, "name")
            names = [tree.set(iid, "name").casefold() for iid in tree.get_children()]
            self.assertEqual(names, sorted(names))
        for pool in ("deck", "pow", "bcd", "tec"):
            p.pools[duelists.duelist][pool] = {2: 100, 10: 900, 100: 1048}
        self.click_heading(duelists.tree, "def")
        for pool in ("deck", "pow", "bcd", "tec"):
            duelists.pool.set(pool)
            duelists.fill()
            rows = duelists.tree.get_children()
            self.assertEqual([cid for cid in rows if cid in {"2", "10", "100"}], ["10", "100", "2"])
        duelists.tree.selection_set("10")
        duelists.weight.set("50")
        duelists.set_weight()
        self.assertEqual(p.pools[duelists.duelist]["tec"][10], 50)
        self.click_heading(duelists.tree, "w")
        weights = [int(duelists.tree.set(iid, "w")) for iid in duelists.tree.get_children()]
        self.assertEqual(weights, sorted(weights))
        self.click_heading(duelists.tree, "pct")
        self.assertEqual([int(duelists.tree.set(iid, "w")) for iid in duelists.tree.get_children()], weights)

    def test_fixed_deck_sort_with_missing_cards(self):
        from fm_editor import fixed_decks
        tab, p = self.app.duelists, self.app.project
        fixed_decks.set_deck(p, tab.duelist, {2: 2, 10: 10, 100: 28})
        deck = fixed_decks.deck_of(p, tab.duelist)
        deck.kept["Missing card"] = 1
        tab.fill()
        tree = tab.fixed.tree
        self.click_heading(tree, "id")
        self.assertEqual(tree.get_children(), ("2", "10", "100", "kept:Missing card"))
        self.click_heading(tree, "id")
        self.assertEqual(tree.get_children(), ("100", "10", "2", "kept:Missing card"))
        for column in ("atk", "def", "type", "name", "copies", "weight"):
            self.click_heading(tree, column)
            if column in ("atk", "def"):
                self.assertEqual(tree.get_children()[-1], "kept:Missing card")
        self.click_heading(tree, "copies")
        tree.selection_set("10")
        tab.fixed.copies.set("5")
        tab.fixed.set_copies()
        self.assertEqual(deck.cards[10], 5)
        self.assertEqual(tree.get_children(), ("kept:Missing card", "2", "10", "100"))

    def test_edit_and_save(self):
        app = self.app
        cards = app.cards
        cards.tree.selection_set("1")
        cards.select()
        cards.vars["name"].set("Bulbasaur")
        cards.vars["attack"].set("1180")
        cards.vars["star1"].set("Venus")
        cards.notes.insert("1.0", "Buffed for the early game. <burn: 300>")
        self.assertTrue(cards.apply())
        self.assertEqual(app.project.cards[1].name, "Bulbasaur")
        self.assertEqual(app.project.notes[1], "Buffed for the early game. <burn: 300>")
        cards.search.set("early game")
        self.assertEqual(cards.tree.get_children(), ("1",))
        cards.search.set("")
        cards.filter.set("With notes")
        self.assertEqual(cards.tree.get_children(), ("1",))
        cards.filter.set(cards.FILTERS[0])
        self.assertTrue(app.dirty)
        cards.add_card()
        new = max(app.project.added)
        self.assertEqual(app.project.added[new].base, 1)
        # a pool weight
        app.duelists.duelist = 2
        app.duelists.pool.set("pow")
        app.duelists.fill()
        first = app.duelists.tree.get_children()[0]
        app.duelists.tree.selection_set(first)
        app.duelists.weight.set("0")
        app.duelists.set_weight()
        self.assertNotIn(int(first), app.project.pools[2]["pow"])
        issues = app.conflicts.run()
        self.assertTrue(any("add up to" in i.message for i in issues))
        app.duelists.normalize()
        self.assertEqual(sum(app.project.pools[2]["pow"].values()), 2048)
        # mod info
        app.info.vars["id"].set("gui-test")
        self.assertTrue(app.info.commit())
        out = Path(self.tmp.name) / "saved"
        app.project.source_dir = out
        self.assertTrue(app.save())
        data = json.loads((out / "mod.json").read_text(encoding="utf-8"))
        self.assertEqual(data["id"], "gui-test")
        self.assertEqual(data["cards"][0], {"replace": 1, "name": "Bulbasaur", "attack": 1180,
                                            "stars": ["Venus", data["cards"][0]["stars"][1]],
                                            "notes": "Buffed for the early game. <burn: 300>"})
        self.assertEqual(data["cards"][1]["copy"], 1)
        self.assertIn("Teana", data["drops"])
        # and back
        app.load_mod(out)
        self.assertEqual(app.project.cards[1].name, "Bulbasaur")
        self.assertEqual(len(app.project.added), 1)

    def test_art(self):
        from fm_editor import art, pngio
        from fm_editor.tests.test_art import gradient
        app = self.app
        app.notebook.select(app.art)
        app.update()
        app.art.goto(2)
        self.assertEqual(app.art.current, 2)
        picture = Path(self.tmp.name) / "picture.png"
        pngio.write(picture, gradient(408, 384))
        self.assertTrue(app.art.use_file("art", str(picture)))
        self.assertEqual(app.art.tree.item("2", "values")[3], "picture, thumbnail")
        self.assertTrue(app.dirty)
        out = Path(self.tmp.name) / "saved-art"
        app.project.info.id = "art-test"
        app.project.source_dir = out
        self.assertTrue(app.save())
        data = json.loads((out / "mod.json").read_text(encoding="utf-8"))
        self.assertEqual(data["textures"], "textures")
        app.load_mod(out)
        app.notebook.select(app.art)
        app.update()
        app.art.goto(2)
        self.assertEqual(art.changed_cards(app.project), {2})
        self.assertIn("Internal 4x", app.art.rows["art"]["info"].cget("text"))
        app.art.revert("art")
        self.assertEqual(art.changed_cards(app.project), set())

    def test_card_starchips_save_load_and_validation(self):
        from fm_editor import manifest
        app = self.app
        cards = app.cards
        cards.tree.selection_set("1")
        cards.select()
        self.assertEqual(cards.vars["starchips"].get(), "10")
        for invalid in ("-1", "1000000", "1.5", "abc"):
            cards.vars["starchips"].set(invalid)
            self.assertFalse(cards.apply())
            self.assertEqual(app.project.starchip_cost(1), 10)
        cards.vars["starchips"].set("0")
        self.assertTrue(cards.apply())
        cards.filter.set("Changed")
        self.assertEqual(cards.tree.get_children(), ("1",))
        out = Path(self.tmp.name) / "saved-starchips"
        app.project.source_dir = out
        self.assertTrue(app.save())
        data = json.loads((out / "mod.json").read_text(encoding="utf-8"))
        self.assertEqual(data["passwords"], {"Blue Dragon": {"starchips": 0}})
        app.load_mod(out)
        cards.tree.selection_set("1")
        cards.select()
        self.assertEqual(cards.vars["starchips"].get(), "0")
        cards.vars["starchips"].set("")
        self.assertTrue(cards.apply())
        self.assertEqual(cards.vars["starchips"].get(), "10")
        self.assertNotIn("passwords", manifest.build(app.project))
        cards.filter.set("All cards")
        cards.tree.selection_set("1")
        cards.select()
        cards.add_card()
        app.update()
        self.assertTrue(cards.price.instate(["disabled"]))
        self.assertEqual(cards.vars["starchips"].get(), "")

    def test_card_cost_does_not_flatten_percentage_rules(self):
        from fm_editor import manifest
        app = self.app
        cards = app.cards
        cards.tree.selection_set("1")
        cards.select()
        # Editing the raw rules in Mod info must not let an unchanged Cards
        # form write its old displayed price back over those rules on Save.
        app.info.other.insert("1.0", json.dumps({"passwords": {"all": {"starchips_percent": 25}}}))
        self.assertTrue(app.commit_all())
        self.assertTrue(app.commit_all())
        self.assertEqual(cards.vars["starchips"].get(), "3")
        self.assertEqual(manifest.build(app.project)["passwords"], {"all": {"starchips_percent": 25}})
        cards.vars["starchips"].set("200")
        self.assertTrue(cards.apply())
        self.assertTrue(app.commit_all())
        self.assertEqual(manifest.build(app.project)["passwords"]["Blue Dragon"], {"starchips": 200})

    def test_card_form_scrolls_and_reveals_keyboard_focus(self):
        app = self.app
        app.deiconify()
        app.geometry("1100x640")
        cards = app.cards
        cards.tree.selection_set("1")
        cards.select()
        app.update()
        scroll = cards.card_scroll
        self.assertGreater(scroll.body.winfo_reqheight(), scroll.canvas.winfo_height())
        self.assertTrue(scroll.bar.winfo_ismapped())
        self.assertGreater(scroll.canvas.winfo_width(), 200)
        self.assertTrue(app.status.winfo_ismapped())
        add = next(w for w in cards.count.master.winfo_children() if w.winfo_class() == "TButton")
        self.assertTrue(add.winfo_ismapped())
        # Wheel over an entry scrolls the form, leaving its value untouched.
        before = cards.vars["starchips"].get()
        cards.price.event_generate("<MouseWheel>", delta=-120)
        app.update()
        self.assertGreater(scroll.canvas.yview()[0], 0)
        self.assertEqual(cards.vars["starchips"].get(), before)
        cards.price.event_generate("<Button-4>")
        app.update()
        scroll.canvas.yview_moveto(0)
        buttons = [w for w in cards.form.winfo_children() if w.winfo_class() == "TFrame"]
        apply = next(w for box in buttons for w in box.winfo_children() if w.cget("text") == "Apply")
        apply.focus_force()
        app.update()
        self.assertGreater(scroll.canvas.yview()[0], 0)
        self.assertGreaterEqual(apply.winfo_rooty(), scroll.canvas.winfo_rooty())
        self.assertLessEqual(apply.winfo_rooty() + apply.winfo_height(),
                             scroll.canvas.winfo_rooty() + scroll.canvas.winfo_height())
        cards.price.focus_force()
        app.update()
        # At a taller window, the scroll range contracts again. The window
        # manager may cap the requested height to the available desktop.
        first, last = scroll.canvas.yview()
        short_fraction = last - first
        app.geometry("1100x1000")
        app.update()
        first, last = scroll.canvas.yview()
        self.assertGreater(last - first, short_fraction)
        body_height = scroll.body.winfo_reqheight()
        expected = min(1.0, scroll.canvas.winfo_height() / body_height)
        self.assertAlmostEqual(last - first, expected, delta=1 / body_height)
        if expected == 1.0:
            self.assertEqual((first, last), (0.0, 1.0))

    def test_card_hints_fit_and_tabs_scroll_when_the_window_is_small(self):
        from tkinter import ttk
        app = self.app
        app.deiconify()
        app.geometry("1600x960")
        cards = app.cards
        cards.tree.selection_set("1")
        cards.select()
        app.update()
        # The form is as wide as it asks, whatever the card: the hints, set
        # after the window was shown, are not cut off.
        scroll = cards.card_scroll
        self.assertGreaterEqual(scroll.canvas.winfo_width(), scroll.body.winfo_reqwidth())
        hint = cards.hints["name"]
        self.assertGreaterEqual(hint.winfo_width(), hint.winfo_reqwidth())
        self.assertFalse(cards.page.xbar.winfo_ismapped() or cards.page.ybar.winfo_ismapped())
        # A window smaller than a tab scrolls the tab instead of cutting it off.
        app.notebook.select(app.stars)
        self.assertIs(app.notebook.current(), app.stars)
        app.minsize(1, 1)
        app.geometry("700x400")
        app.update()
        page = app.stars.page
        self.assertTrue(page.xbar.winfo_ismapped() and page.ybar.winfo_ismapped())
        self.assertGreaterEqual(app.stars.winfo_height(), app.stars.winfo_reqheight())
        page.canvas.yview_moveto(1)
        app.update()
        self.assertGreater(page.canvas.yview()[0], 0)
        app.geometry("1600x960")
        app.update()
        self.assertFalse(page.xbar.winfo_ismapped() or page.ybar.winfo_ismapped())
        self.assertEqual(page.canvas.cget("background"), ttk.Style(app).lookup("TFrame", "background"))

    def test_scrolled_options_keep_text_scroll_and_dark_background(self):
        from tkinter import ttk
        app = self.app
        app.deiconify()
        app.geometry("1100x640")
        cards = app.cards
        cards.tree.selection_set("1")
        cards.select()
        app.update()
        cards.text.delete("1.0", "end")
        cards.text.insert("1.0", "line\n" * 40)
        cards.text.yview_moveto(0)
        before = cards.card_scroll.canvas.yview()
        if app.tk.call("tk", "windowingsystem") == "x11":
            cards.text.event_generate("<Button-5>")
        else:
            cards.text.event_generate("<MouseWheel>", delta=-120)
        app.update()
        self.assertGreater(cards.text.yview()[0], 0)
        self.assertEqual(cards.card_scroll.canvas.yview(), before)
        app.dark.set(True)
        app.toggle_dark()
        app.update()
        self.assertEqual(cards.card_scroll.canvas.cget("background"), ttk.Style(app).lookup("TFrame", "background"))
        app.notebook.select(app.limits)
        app.limits.advanced_shown.set(True)
        app.limits._show_advanced()
        app.update()
        scroll = app.limits.scroll
        # Windows fonts can fit the whole form at 640px. Size the viewport
        # from the form itself so this exercises overflowing content there too.
        chrome_height = app.winfo_height() - scroll.canvas.winfo_height()
        app.minsize(1, 1)
        app.geometry(f"1100x{chrome_height + scroll.body.winfo_reqheight() // 2}")
        app.update()
        self.assertGreater(scroll.body.winfo_reqheight(), scroll.canvas.winfo_height())
        scroll.canvas.yview_moveto(1)
        self.assertGreater(scroll.canvas.yview()[0], 0)

    def test_packs(self):
        from fm_editor import pngio
        from fm_editor.packs_tab import SimulateDialog
        from fm_editor.tests.test_art import gradient
        app = self.app
        tab = app.packs
        app.notebook.select(tab)
        app.update()
        tab.add_pack()
        self.assertEqual(len(app.project.packs), 1)
        tab.vars["name"].set("Dragons")
        tab.vars["price"].set("50")
        self.assertTrue(tab.commit())
        tab.add_cards([1, 2, 3])
        self.assertEqual(len(tab.tree.get_children()), 3)
        tab.tree.selection_set("0:2")
        tab.weight.set("5")
        tab.set_weight()
        self.assertEqual(str(tab.tree.item("0:2", "values")[3]), "5")
        self.assertTrue(tab.tree.item("0:0", "values")[4].endswith("%"))
        # Advanced: a guarantee needs a tier of that name.
        tab.toggle_advanced()
        tab.adv["guarantee"].set("rare=1")
        self.assertTrue(tab.commit())
        self.assertTrue(any("not a tier of the pack" in i.message for i in app.conflicts.run() if i.area == "Packs"))
        tab.adv["guarantee"].set("")
        tab.adv["stock"].set("3")
        self.assertTrue(tab.commit())
        picture = Path(self.tmp.name) / "pack.png"
        pngio.write(picture, gradient(204, 192))
        tab.use_file(str(picture))
        self.assertEqual(app.project.packs[0]["image"], "packs/pack-1.png")
        for zoom in (1, 2, 4):
            tab.zoom.set(zoom)
            tab.show_picture()
        dialog = SimulateDialog(tab, tab.parsed()[0])
        while dialog.running:            # opened a slice at a time, the window answering between
            app.update()
        self.assertEqual(dialog.result.draws, 1000 * 5 * 4)
        # Stop shows what came so far; the window never waits for all of them.
        dialog.count.set("1000000")   # 5 cards a pack: within the cap
        dialog.run()
        app.update()
        self.assertTrue(dialog.running)
        dialog.stop()
        self.assertFalse(dialog.running)
        self.assertLess(dialog.result.packs, 1000000)
        self.assertEqual(dialog.result.draws, dialog.result.packs * 5 * 4)
        dialog.destroy()
        self.assertFalse([i for i in app.conflicts.run() if i.area == "Packs" and i.level == "error"])
        out = Path(self.tmp.name) / "saved-packs"
        app.project.info.id = "packs-test"
        app.project.source_dir = out
        self.assertTrue(app.save())
        data = json.loads((out / "mod.json").read_text(encoding="utf-8"))
        self.assertEqual(data["packs"], [{"id": "pack-1", "name": "Dragons", "price": 50,
                                          "cards": {str(app.project.ref(1)): 1, str(app.project.ref(2)): 1,
                                                    str(app.project.ref(3)): 5},
                                          "stock": 3, "image": "packs/pack-1.png"}])
        self.assertTrue((out / "packs" / "pack-1.png").is_file())
        app.load_mod(out)
        app.notebook.select(tab)
        app.update()
        self.assertEqual(tab.vars["name"].get(), "Dragons")

    def test_packs_keep_what_is_written(self):
        """Opening a mod and moving through its packs changes nothing of it;
        a copy's picture is its own; Shop settings keep a shop's other keys."""
        from fm_editor import manifest, packs as packmath, pngio
        from fm_editor.tests.test_art import gradient
        app, tab = self.app, self.app.packs
        mod = Path(self.tmp.name) / "packs-as-written"
        mod.mkdir(exist_ok=True)
        source = {"id": "written", "name": "Written", "packs": [
            {"name": "Alpha", "cards": [1, 2, 3], "cover": 2, "price": 100.0},
            {"name": "Beta", "price": 100, "duplicates": "allow", "cards": {"4": 1, "5": 1}, "mystery": 1},
            {"name": "Gamma", "cards": [6], "include_added_cards": "no", "stock": "5"}],
            "pack_shop": {"rng": "game", "shops": [{"id": "a", "name": "A", "where": "password", "extra": 1}]}}
        (mod / "mod.json").write_text(json.dumps(source), encoding="utf-8")
        app.load_mod(mod)
        app.notebook.select(tab)
        app.update()
        before = json.dumps(app.project.packs)
        for i in (1, 2, 0, 2):
            tab.list.selection_set(str(i))
            app.update()
        self.assertFalse(app.dirty)
        self.assertEqual(json.dumps(app.project.packs), before)
        # An edit changes what it edits, and leaves the rest as written.
        tab.vars["description"].set("Three cards")
        self.assertTrue(tab.commit())
        self.assertTrue(app.dirty)
        self.assertEqual(app.project.packs[2]["stock"], "5")
        self.assertEqual(app.project.packs[2]["include_added_cards"], "no")
        tab.list.selection_set("0")
        app.update()
        tab.adv["when_nothing_left"].set("sell")
        self.assertTrue(tab.commit())
        self.assertEqual(app.project.packs[0]["cover"], 2)
        self.assertEqual(app.project.packs[0]["when_nothing_left"], "sell")
        # A copy gets a picture of its own: importing on it leaves the first's.
        picture = Path(self.tmp.name) / "packs-own.png"
        pngio.write(picture, gradient(102, 96))
        tab.use_file(str(picture))
        first = app.project.packs[0]["image"]
        tab.duplicate()
        copy_image = app.project.packs[1]["image"]
        self.assertNotEqual(copy_image, first)
        self.assertEqual(app.project.files[copy_image], app.project.files[first])
        tab.use_file(str(picture))
        tab.revert_png()
        self.assertIn(first, app.project.files)
        self.assertNotIn(copy_image, app.project.files)
        # Shop settings: OK with nothing typed changes nothing; a shop's other keys stay.
        with mock.patch("fm_editor.packs_tab.FormDialog") as form:
            tab.shop_settings()
            build, ok = form.call_args[0][2], form.call_args[0][3]
            body = tk.Frame(app)
            build(None, body)
            self.assertIsNone(ok(None))
            self.assertEqual(app.project.pack_shop, packmath.minimize_rules(source["pack_shop"]))
            tab.shop_settings()
            build, ok = form.call_args[0][2], form.call_args[0][3]
            body = tk.Frame(app)
            build(None, body)
            texts = [w for w in body.grid_slaves() if isinstance(w, tk.Text)]
            texts[0].delete("1.0", "end")
            texts[0].insert("1.0", "a | Shop A\nb\n")
            self.assertIsNone(ok(None))
        self.assertEqual(app.project.pack_shop["shops"], [{"id": "a", "name": "Shop A", "where": "password",
                                                           "extra": 1}, {"id": "b"}])
        self.assertEqual(manifest.build(app.project)["pack_shop"]["shops"][0]["where"], "password")

    def test_packs_file_greys_the_tab(self):
        """"packs" naming a file: the editor does not edit it, so nothing of a
        pack is offered, but Shop settings (the manifest's) is."""
        from fm_editor.packs_tab import PacksTab
        app, tab = self.app, self.app.packs

        def enabled():
            out = []

            def walk(widget):
                for child in widget.winfo_children():
                    if isinstance(child, PacksTab.EDITABLE) and not child.instate(["disabled"]):
                        out.append(child)
                    walk(child)
            walk(tab)
            return out

        mod = Path(self.tmp.name) / "packs-in-a-file"
        mod.mkdir(exist_ok=True)
        (mod / "mod.json").write_text(json.dumps({"id": "pf", "name": "PF", "packs": "packs.json"}), encoding="utf-8")
        (mod / "packs.json").write_text(json.dumps([{"name": "Z", "cards": [1]}]), encoding="utf-8")
        app.load_mod(mod)
        app.notebook.select(tab)
        app.update()
        self.assertEqual(enabled(), [tab.shop_button])
        mod = Path(self.tmp.name) / "packs-in-the-manifest"
        mod.mkdir(exist_ok=True)
        (mod / "mod.json").write_text(json.dumps({"id": "pm", "name": "PM", "packs": [{"name": "Z", "cards": [1]}]}),
                                      encoding="utf-8")
        app.load_mod(mod)
        app.update()
        texts = {str(w.cget("text")) for w in enabled() if isinstance(w, tk.ttk.Button)}
        self.assertTrue({"Add pack", "Simulate...", "Apply", "Import PNG...", "Add tier"} <= texts, texts)
        self.assertNotIn("Export...", texts)     # no picture of its own to export

    def test_text_preview(self):
        import dataclasses
        from fm_editor import card_text
        from fm_editor.tests.test_card_text import synthetic_wa
        app = self.app
        app.show_text_preview()
        preview = app.text_preview
        app.update()
        self.assertIn("Choose a card", preview.notes.cget("text"))
        app.cards.tree.selection_set("1")
        app.cards.select()
        preview.refresh()
        self.assertIn("no font", preview.notes.cget("text"))      # the synthetic disc has none
        wa = bytearray(app.files.wa)
        start, end = card_text.BOOT_SECTOR * 2048, (card_text.RAMP_SECTOR + 1) * 2048
        wa[start:end] = synthetic_wa()[start:end]
        app.files = dataclasses.replace(app.files, wa=bytes(wa), source="with a font")
        app.cards.text.delete("1.0", "end")
        app.cards.text.insert("1.0", "A " * 100)
        app.cards.count_lines()
        preview.refresh()
        self.assertIsNotNone(preview.image)
        self.assertEqual(preview.image.width(), (card_text.COLUMNS * 8 + card_text.GUTTER) * 2)
        self.assertIn("will not show in the game", preview.notes.cget("text"))
        # A font file cut short says so and keeps the retail picture.
        from fm_editor import preview as preview_module
        from fm_editor.tests.test_card_text import tiny_font
        with tempfile.TemporaryDirectory() as tmp:
            whole = Path(tmp) / "whole.ttf"
            tiny_font(whole)
            for size in (40, 100, len(whole.read_bytes()) // 2):
                cut = Path(tmp) / f"cut{size}.ttf"
                cut.write_bytes(whole.read_bytes()[:size])
                preview.font_path = str(cut)
                preview.mode.set(preview_module.FILE)
                preview.refresh()
                self.assertTrue(preview.face_label.cget("text"), size)
                self.assertEqual(str(preview.cget("cursor")), "")
        preview.mode.set(preview_module.RETAIL)
        preview.close()
        self.assertIsNone(app.text_preview)

    def test_preview_cancels_pending_refresh(self):
        app = self.app
        app.show_text_preview()
        preview = app.text_preview
        preview.later()
        pending = preview.pending
        self.assertIn(pending, app.tk.call("after", "info"))
        # Changing a preview option redraws immediately, while a typing
        # refresh may still be scheduled.
        preview.refresh()
        self.assertNotIn(pending, app.tk.call("after", "info"))
        preview.later()
        pending = preview.pending
        preview.close()
        self.assertNotIn(pending, app.tk.call("after", "info"))
        self.assertIsNone(app.text_preview)
        # Tk destroys child windows directly when the editor closes.
        app.show_text_preview()
        preview = app.text_preview
        preview.later()
        pending = preview.pending
        preview.destroy()
        self.assertNotIn(pending, app.tk.call("after", "info"))
        self.assertIsNone(app.text_preview)

    def test_tabs_fill(self):
        app = self.app
        for tab in app.tabs:
            app.notebook.select(tab)
            app.update()
        app.fusions.search.set("Blue Dragon")
        self.assertTrue(app.fusions.tree.get_children())
        app.equips.equips.selection_set("651")
        app.equips.select()
        self.assertEqual(len(app.equips.monsters.get_children()), 30)
        self.assertEqual(len(app.rituals.tree.get_children()), 20)   # every ritual card, with or without a recipe

    def test_remove_disc_recipes(self):
        from fm_editor import manifest
        app = self.app
        tab = app.fusions
        p = tab.project
        recipes = p.retail_recipes(3)
        dialog = tab.remove_result()
        dialog.fields["r"].set(610)                  # a magic card: no disc recipe makes it
        dialog.ok()
        self.assertIn("no recipe", dialog.error.cget("text"))
        dialog.fields["r"].set(3)
        dialog.ok()
        self.assertEqual(p.fusion_removes, [3])
        self.assertFalse([pair for pair in recipes if pair in p.fusions])
        self.assertTrue(app.dirty)
        self.assertEqual(manifest.build_fusions(p), [{"remove": "Kuriboh"}])
        # An own "fusions" list naming the pair makes its card now: the row says so.
        p.card_extra[1] = {"fusions": [{"with": 2, "result": 500}]}
        p._own_pairs = None
        tab.search.set("Blue Dragon")
        self.assertEqual(tab.tree.set("1:2", "state"), "own list")
        self.assertIn("Card 500", tab.tree.set("1:2", "result"))
        del p.card_extra[1]
        p._own_pairs = None
        tab.fill()
        self.assertEqual(tab.tree.set("1:2", "state"), "removed")
        tab.tree.selection_set("1:2")
        tab.revert()
        self.assertEqual(p.fusions[(1, 2)], 3)
        if len(recipes) > 1:
            self.assertIn({"with": ["Blue Dragon", "Mystic Elf"], "result": "Kuriboh"}, manifest.build_fusions(p))
        else:
            self.assertEqual(manifest.build_fusions(p), [])
        # A copy's own list that its base's rule answers first is no row of
        # its own: it would read "forbidden" where the game plays the rule.
        copy = p.add_card(1, "x")
        p.added[copy].extra = {"fusions": [{"with": 2, "result": 500}]}
        p.set_fusion(1, 2, 599)
        p._own_pairs = None
        tab.search.set("")
        tab.fill()
        self.assertFalse(tab.tree.exists(f"2:{copy}"))

    def test_dark_mode(self):
        from fm_editor import theme
        from fm_editor.app import App
        app = self.app
        text = app.info.description
        light = text.cget("background")
        app.dark.set(True)
        app.toggle_dark()
        self.assertEqual(json.loads(self.settings.read_text(encoding="utf-8")), {"dark": True})
        self.assertEqual(app.theme.style.theme_use(), theme.DARK_THEME)
        self.assertEqual(text.cget("background"), theme.FIELD)
        self.assertEqual(str(app.cards.tree.tag_configure("changed", "foreground")), theme.TAGS["changed"][1])
        dialog = tk.Toplevel(app)       # made after the switch: the option database
        self.assertEqual(dialog.cget("background"), theme.BG)
        dialog.destroy()
        # remembered at the next start, where an importer adds its menu entry
        # after the window (and, on Windows, the strip's clone of File) is made
        other = App(ask=False, autostart=False)
        other.withdraw()
        self.assertTrue(other.dark.get())
        self.assertEqual(other.theme.style.theme_use(), theme.DARK_THEME)
        other.add_import("Probe...", lambda: None)
        if other.theme.strip is not None:
            clone = other.theme.strip.winfo_children()[0].cget("menu")      # a Tcl-made menu
            self.assertEqual(other.tk.call(clone, "index", "end"), other.file_menu.index("end"))
            self.assertEqual(other.tk.call(clone, "entrycget", other.import_index - 1, "-label"), "Probe...")
        other.destroy()
        app.dark.set(False)
        app.toggle_dark()
        self.assertEqual(json.loads(self.settings.read_text(encoding="utf-8")), {"dark": False})
        self.assertEqual(app.theme.style.theme_use(), app.theme.light)
        self.assertEqual(text.cget("background"), light)
        self.assertEqual(str(app.cards.tree.tag_configure("changed", "foreground")), theme.TAGS["changed"][0])
        self.assertTrue(app.cget("menu"))       # the window's own menu bar is back


class SettingsTest(unittest.TestCase):
    def test_missing_or_broken(self):
        from fm_editor import settings
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "sub" / "settings.json"
            with mock.patch.object(settings, "path", lambda: path):
                self.assertEqual(settings.load(), {})
                self.assertIsNone(settings.save("dark", True))
                self.assertEqual(settings.load(), {"dark": True})
                path.write_text("[not json", encoding="utf-8")
                self.assertEqual(settings.load(), {})
                path.write_text("[1, 2]", encoding="utf-8")
                self.assertEqual(settings.load(), {})


if __name__ == "__main__":
    unittest.main()
