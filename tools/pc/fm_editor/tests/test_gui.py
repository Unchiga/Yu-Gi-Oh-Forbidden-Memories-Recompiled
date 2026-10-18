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


def entry_count(entry):
    """The cards a pack deals at a time (packs.py's DEFAULT_COUNT unless set)."""
    from fm_editor import packs
    count = entry.get("count", packs.DEFAULT_COUNT)
    return count if isinstance(count, int) and count > 0 else packs.DEFAULT_COUNT


class GuiCase(unittest.TestCase):
    """The window on the fixture's game files, for each test."""

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

    def pause(self):
        """As a person pauses between two edits: the undo step is taken."""
        from fm_editor.app import HISTORY_PAUSE
        self.app.update()
        self.app.after(HISTORY_PAUSE + 50)
        self.app.update()

    def click_heading(self, tree, column):
        tree.tk.call(tree.heading(column, "command"))


class GuiTest(GuiCase):

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
        # The left-hand lists sort too, retaining their order on refill (the
        # duelists within each Free Duel page, the pages staying in order).
        for tree, refill, rows in ((equips.equips, equips.fill_equips, lambda t: t.get_children()),
                                   (duelists.list, duelists.fill_list, lambda t: t.get_children("page:1"))):
            self.click_heading(tree, "id")
            self.click_heading(tree, "id")
            expected = tuple(sorted(rows(tree), key=int, reverse=True))
            refill()
            self.assertEqual(rows(tree), expected)
            name = "name" if tree is equips.equips else "#0"
            self.click_heading(tree, name)
            names = [(tree.set(iid, name) if name != "#0" else tree.item(iid, "text")).strip().casefold()
                     for iid in rows(tree)]
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

    def test_magic_card_has_an_effect_not_monster_stats(self):
        app = self.app
        cards = app.cards
        monster_widgets = cards.monster_rows

        def shown(widgets):
            return all(w.winfo_manager() for w in widgets)

        cards.tree.selection_set("1")
        cards.select()
        self.assertTrue(shown(monster_widgets))
        self.assertFalse(any(w.winfo_manager() for w in cards.effect_row))
        cards.vars["type"].set("Magic")
        self.assertFalse(any(w.winfo_manager() for w in monster_widgets))
        self.assertTrue(shown(cards.effect_row))
        # The disc's magic cards to pick from, and none for a monster made one.
        values = cards.effect_box.cget("values")
        self.assertEqual(values[0], "(none)")
        self.assertIn("Card 601", values)
        self.assertNotIn("Card 651", values)        # an equip
        self.assertEqual(cards.vars["effect"].get(), "(none)")
        cards.vars["effect"].set("Card 605")
        self.assertTrue(cards.apply())
        card = app.project.cards[1]
        self.assertEqual((card.type, card.attack, card.defense, card.level, card.star1, card.star2, card.attribute),
                         (20, 0, 0, 0, 0, 0, 6))
        self.assertEqual(app.project.card_extra[1]["effect"], 605)
        self.assertEqual(app.project.effect_of(1), 605)
        # Shown again as it was stored.
        cards.tree.selection_set("2")
        cards.select()
        cards.tree.selection_set("1")
        cards.select()
        self.assertEqual(cards.vars["effect"].get(), "Card 605")
        # A disc magic card has its own effect: no "(none)", and picking it
        # again writes nothing.
        cards.tree.selection_set("610")
        cards.select()
        self.assertTrue(shown(cards.effect_row))
        self.assertNotIn("(none)", cards.effect_box.cget("values"))
        self.assertEqual(cards.vars["effect"].get(), "Card 610")
        cards.vars["effect"].set("Card 620")
        self.assertTrue(cards.apply())
        self.assertEqual(app.project.card_extra[610], {"effect": 620})
        cards.vars["effect"].set("Card 610")
        self.assertTrue(cards.apply())
        self.assertNotIn(610, app.project.card_extra)
        # A trap lists traps.
        cards.vars["type"].set("Trap")
        self.assertIn("Card 701", cards.effect_box.cget("values"))
        self.assertNotIn("Card 601", cards.effect_box.cget("values"))
        cards.vars["type"].set("Magic")
        # Saved as the game reads it, and back.
        app.info.vars["id"].set("gui-test")
        self.assertTrue(app.info.commit())
        out = Path(self.tmp.name) / "saved-effect"
        app.project.source_dir = out
        self.assertTrue(app.save())
        data = json.loads((out / "mod.json").read_text(encoding="utf-8"))
        entry = next(e for e in data["cards"] if e.get("replace") == 1)
        self.assertEqual((entry["type"], entry["effect"]), ("Magic", 605))
        app.load_mod(out)
        self.assertEqual(app.project.effect_of(1), 605)
        from fm_editor import validate
        self.assertEqual([i.message for i in validate.validate_card(app.project, 1) if "effect" in i.message], [])

    def test_monster_again_gets_its_stats_back(self):
        app = self.app
        cards = app.cards
        disc = app.project.retail.cards[1]
        cards.tree.selection_set("1")
        cards.select()
        cards.vars["type"].set("Magic")
        self.assertTrue(cards.apply())
        self.assertEqual(app.project.cards[1].attack, 0)
        # Shown again, with the zeros it was stored with.
        cards.tree.selection_set("2")
        cards.select()
        cards.tree.selection_set("1")
        cards.select()
        from fm_editor.tabs import type_label
        cards.vars["type"].set(type_label(disc.type))
        self.assertTrue(cards.apply())
        card = app.project.cards[1]
        self.assertEqual((card.attack, card.defense, card.level, card.star1, card.star2, card.attribute),
                         (disc.attack, disc.defense, disc.level, disc.star1, disc.star2, disc.attribute))

    def test_untouched_effect_stays_as_written(self):
        app = self.app
        cards = app.cards
        # A trap's effect on a magic card: shown as none, kept unless changed.
        cards.show(None)                # the model changes under no form
        app.project.cards[1] = app.project.cards[1].copy(type=20, attack=0, defense=0, level=0, star1=0, star2=0,
                                                         attribute=6)
        app.project.card_extra[1] = {"effect": 701}
        cards.tree.selection_set("1")
        cards.select()
        self.assertEqual(cards.vars["effect"].get(), "(none)")
        cards.tree.selection_set("2")
        cards.select()
        self.assertEqual(app.project.card_extra[1], {"effect": 701})
        from fm_editor import validate
        self.assertTrue(any("CPU" in i.message for i in validate.validate_card(app.project, 1)))
        # Made a trap, the card shows that effect and keeps it.
        cards.tree.selection_set("1")
        cards.select()
        cards.vars["type"].set("Trap")
        self.assertEqual(cards.vars["effect"].get(), "Card 701")
        self.assertTrue(cards.apply())
        self.assertEqual(app.project.card_extra[1], {"effect": 701})

    def test_converted_equip_displays_and_edits_effect_targets(self):
        app, p = self.app, self.app.project
        equip = p.add_card(3, "converted-equip")
        p.cards[equip] = p.cards[equip].copy(type=23, attack=0, defense=0)
        p.added[equip].extra["effect"] = 651
        trap = p.add_card(5, "converted-trap")
        p.cards[trap] = p.cards[trap].copy(type=21, attack=0, defense=0)
        p.added[trap].extra["effect"] = 701
        p.cards[652] = p.cards[652].copy(type=0)
        tab = app.equips
        tab.refresh()
        self.assertFalse(tab.equips.exists("652"))
        tab.current = equip
        tab.fill()
        expected = set(p.retail.equips[651])
        self.assertEqual(set(map(int, tab.monsters.get_children())), expected)
        self.assertNotIn(trap, p.equip_targets(651))
        self.assertEqual(int(tab.equips.set(str(equip), "n")), len(expected))
        tab.monsters.selection_set("5")
        tab.remove()
        self.assertEqual(p.equip_targets(equip), expected - {5})
        self.assertEqual(set(map(int, tab.monsters.get_children())), expected)
        self.assertEqual(tab.monsters.set("5", "state"), "removed")

    def test_cards_to_equips_workflow_survives_save_and_reopen(self):
        from fm_editor.model import Project
        from fm_editor import tabs
        app = self.app
        for added in (False, True):
            with self.subTest(added=added):
                app.set_project(Project(app.retail))
                app.notebook.select(app.cards)
                app.cards.goto(1)
                app.update()
                if added:
                    app.cards.add_card()
                    app.update()
                cid = app.cards.current
                app.cards.vars["type"].set("Equip")     # no effect to choose: no targets yet
                if not added:
                    self.assertTrue(app.cards.apply())
                # Switching tabs must commit the form and populate Equips;
                # do not refresh the tab or assign its current card by hand.
                app.notebook.select(app.equips)
                app.update()
                self.assertEqual(app.project.cards[cid].type, 23)
                self.assertTrue(app.equips.equips.exists(str(cid)))
                app.equips.equips.selection_set(str(cid))
                app.update()
                self.assertEqual(app.equips.current, cid)
                baseline = app.project.equip_baseline(cid)
                self.assertEqual(set(map(int, app.equips.monsters.get_children())), baseline)
                for monster in (5, 100):
                    with mock.patch.object(tabs, "pick_card", return_value=monster):
                        app.equips.add()
                expected = baseline | {5, 100}
                self.assertEqual(app.project.equip_targets(cid), expected)
                self.assertEqual(int(app.equips.equips.set(str(cid), "n")), len(expected))
                folder = Path(self.tmp.name) / f"equip-workflow-{added}"
                app.project.source_dir = folder
                with mock.patch("fm_editor.app.messagebox.askyesno", return_value=False) as warning:
                    self.assertTrue(app.save())
                    warning.assert_not_called()
                app.load_mod(folder)
                app.notebook.select(app.equips)
                app.update()
                app.equips.equips.selection_set(str(cid))
                app.update()
                self.assertEqual(app.project.equip_targets(cid), expected)
                self.assertEqual(app.equips.monsters.set("5", "state"), "added")
                self.assertEqual(app.equips.monsters.set("100", "state"), "added")
                app.equips.revert()
                self.assertEqual(app.project.equip_targets(cid), baseline)
                app.notebook.select(app.cards)
                app.cards.goto(cid)
                app.update()
                app.cards.vars["type"].set("Dragon")
                app.notebook.select(app.equips)
                app.update()
                self.assertFalse(app.equips.equips.exists(str(cid)),
                                 (app.cards.current, app.project.cards[cid].type,
                                  app.cards.vars["type"].get(), app.cards.status.cget("text")))
                self.assertTrue(all(w.instate(["disabled"]) for w in app.equips.actions.winfo_children()))

    def test_edit_equips_shortcut_applies_and_selects_the_card(self):
        app, cards = self.app, self.app.cards
        # Equips opens on its first equip card, its buttons ready.
        self.assertEqual(app.equips.current, int(app.equips.equips.get_children()[0]))
        self.assertTrue(all(not w.instate(["disabled"]) for w in app.equips.actions.winfo_children()))
        cards.goto(1)
        app.update()
        self.assertEqual(cards.edit_equips_button.winfo_manager(), "")
        cards.vars["type"].set("Equip")
        self.assertEqual(cards.edit_equips_button.winfo_manager(), "grid")
        cards.edit_equips_button.invoke()
        app.update()
        self.assertIs(app.notebook.current(), app.equips)
        self.assertEqual(app.equips.current, 1)
        self.assertEqual(app.equips.equips.selection(), ("1",))
        self.assertEqual(app.project.effect_of(1), 1)          # an equip has no effect to choose
        self.assertEqual(set(map(int, app.equips.monsters.get_children())), app.project.equip_baseline(1))
        self.assertTrue(all(w.instate(["!disabled"]) for w in app.equips.actions.winfo_children()))

    def test_equip_boosts_atk_and_def(self):
        app, cards = self.app, self.app.cards
        cards.goto(651)
        app.update()
        # An equip has no Retail effect: its boosts are the whole of it.
        self.assertEqual(cards.effect_box.winfo_manager(), "")
        self.assertEqual((cards.vars["equip_attack"].get(), cards.vars["equip_defense"].get()), ("500", "500"))
        self.assertEqual(cards.hints["equip_attack"].cget("text"), "")
        cards.vars["equip_attack"].set("1200")
        cards.vars["equip_defense"].set("-300")
        self.assertTrue(cards.apply())
        self.assertEqual(app.project.equip_bonus, {651: (1200, -300)})
        app.update()
        self.assertEqual(str(cards.hints["equip_attack"].cget("text")), "Retail: +500 (restore)")
        cards.restore("equip_defense")
        self.assertTrue(cards.apply())
        self.assertEqual(app.project.equip_bonus, {651: (1200, 500)})
        cards.vars["equip_attack"].set("")       # empty: the default again
        self.assertTrue(cards.apply())
        self.assertEqual(app.project.equip_bonus, {})
        cards.vars["equip_defense"].set("1e3")
        self.assertFalse(cards.apply())
        self.assertIn("DEF boosts", str(cards.status.cget("text")))
        cards.vars["equip_defense"].set("500")
        self.assertTrue(cards.apply())
        # Megamorph's are +1000.
        cards.goto(657)
        app.update()
        self.assertEqual((cards.vars["equip_attack"].get(), cards.vars["equip_defense"].get()), ("1000", "1000"))
        # A magic card made an equip loses its magic effect and starts at +500.
        cards.goto(601)
        app.update()
        app.project.card_extra[601] = {"effect": 602}
        cards.vars["type"].set("Equip")
        self.assertEqual(cards.effect_box.winfo_manager(), "")
        self.assertTrue(cards.apply())
        self.assertNotIn(601, app.project.card_extra)
        self.assertEqual(cards.vars["equip_attack"].get(), "500")
        cards.goto(1)
        app.update()
        self.assertEqual(cards.vars["equip_attack"].get(), "")

    def test_password_takes_up_to_8_digits(self):
        cards = self.app.cards
        cards.goto(2)
        self.app.update()
        entry = next(w for w in cards.form.winfo_children()
                     if w.winfo_class() == "TEntry" and str(w.cget("textvariable")) == str(cards.vars["password"]))
        entry.delete(0, "end")
        for ch in "1234x5678 9":
            entry.insert("end", ch)
        self.assertEqual(entry.get(), "12345678")
        entry.delete(0, "end")
        entry.insert(0, "123456789")
        self.assertEqual(entry.get(), "")

    def test_export_makes_a_folder_named_after_the_mod(self):
        app = self.app
        app.info.vars["id"].set("export-test")
        with tempfile.TemporaryDirectory() as where, \
                mock.patch("fm_editor.file_dialogs.askdirectory", return_value=where):
            self.assertTrue(app.save(ask=True, export=True))
            self.assertTrue((Path(where) / "export-test" / "mod.json").is_file())
            self.assertEqual(Path(app.project.source_dir), Path(where) / "export-test")

    def test_right_click_menu_closes(self):
        from fm_editor import card_links
        app = self.app
        # Only X11 menus stay up on their own: on Windows (and macOS) a posted
        # menu is native and modal -- "post" does not return until it is
        # dismissed, which would hang here -- and closes itself on a click away.
        if app.tk.call("tk", "windowingsystem") != "x11":
            self.skipTest("posted menus are modal outside X11")
        app.deiconify()
        for close in (lambda: app.notebook.select(app.fusions),
                      lambda: app.cards.tree.event_generate("<ButtonPress-1>", x=5, y=5)):
            app.notebook.select(app.cards)
            app.update()
            menu = card_links._open_menu = tk.Menu(app.cards.tree, tearoff=False)
            menu.add_command(label="Where it's used...")
            menu.post(app.winfo_rootx(), app.winfo_rooty())
            app.update()
            close()
            app.update()
            self.assertFalse(menu.winfo_exists())
            self.assertIsNone(card_links._open_menu)

    def test_card_navigation_keeps_invalid_pending_edits(self):
        cards = self.app.cards
        cards.goto(1)
        self.app.update()
        cards.vars["attack"].set("unfinished")
        self.assertFalse(cards.goto(2))
        self.app.update()
        self.assertEqual(cards.current, 1)
        self.assertEqual(cards.tree.selection(), ("1",))
        self.assertEqual(cards.vars["attack"].get(), "unfinished")

    def test_copy_has_its_replaced_base_effect(self):
        # A copy with no "effect" plays its base's (cards.c Cards_EffectId).
        project = self.app.project
        project.card_extra[610] = {"effect": 620}
        copy = project.add_card(610)
        self.assertEqual(project.effect_of(copy), 620)
        self.assertEqual(project.effect_of(611), 611)

    def test_any_card_can_use_an_independent_retail_magic_effect(self):
        from fm_editor import manifest, validate
        from fm_editor.model import Project
        app, tab = self.app, self.app.cards
        # Monster, magic, equip, ritual and trap, plus a new copy of each.
        originals = [1, 610, 651, 681, 701]
        added = [app.project.add_card(cid) for cid in originals]
        tab.refresh()
        for cid in originals + added:
            tab.tree.selection_set(str(cid))
            tab.select()
            tab.vars["type"].set("Magic")
            tab.vars["effect"].set("Card 605")
            self.assertTrue(tab.apply())
            self.assertEqual(app.project.effect_of(cid), 605)
            self.assertEqual(app.project.cards[cid].attribute, 6)
            self.assertFalse([i for i in validate.validate_card(app.project, cid) if i.level == "error"])
        # The effect source becomes a monster with another name. The effect
        # list and numeric references must keep the original retail behavior.
        tab.tree.selection_set("605")
        tab.select()
        tab.vars["type"].set("Dragon")
        tab.vars["name"].set("New monster")
        self.assertTrue(tab.apply())
        tab.tree.selection_set("1")
        tab.select()
        self.assertEqual(tab.vars["effect"].get(), "Card 605")
        built = manifest.build(app.project)
        restored = Project(app.project.retail)
        manifest.apply(restored, built)
        for cid in originals + added:
            self.assertEqual(restored.effect_of(cid), 605)
            self.assertEqual(restored.cards[cid].type, 20)
        # Changing the source to another magic effect also leaves explicit
        # users of its original effect alone.
        app.project.card_extra[605] = {"effect": 620}
        self.assertEqual(app.project.effect_of(1), 605)

    def test_duplicate_retail_effect_names_remain_distinct(self):
        tab, project = self.app.cards, self.app.project
        project.retail.cards[605].name = project.retail.cards[606].name = "Same effect name"
        tab.tree.selection_set("1")
        tab.select()
        tab.vars["type"].set("Magic")
        self.assertIn("Same effect name (605)", tab.effect_box.cget("values"))
        tab.vars["effect"].set("Same effect name (606)")
        self.assertTrue(tab.apply())
        self.assertEqual(project.effect_of(1), 606)

    def test_trap_form_thresholds_and_independent_copy_roundtrip(self):
        from fm_editor import manifest, validate
        from fm_editor.model import Project
        tab, p = self.app.cards, self.app.project
        # This synthetic disc normally places traps at 701 onward.
        for cid in range(681, 691):
            p.retail.cards[cid].type = p.cards[cid].type = 21
        copy = p.add_card(1)
        tab.refresh()
        tab.tree.selection_set(str(copy))
        tab.select()
        tab.vars["type"].set("Trap")
        tab.vars["effect"].set("Card 681")
        self.assertTrue(all(w.winfo_manager() for w in tab.effect_row + tab.trap_rows))
        self.assertFalse(any(w.winfo_manager() for w in tab.monster_rows))
        tab.vars["trap_threshold"].set("1234")
        self.assertTrue(tab.apply())
        self.assertEqual(p.cards[copy].attribute, 7)
        self.assertEqual(tab.row(copy)[0][4:6], ("", ""))
        self.assertEqual((p.cards[copy].attack, p.cards[copy].defense, p.cards[copy].star1, p.cards[copy].star2), (0, 0, 0, 0))
        self.assertFalse([i for i in validate.validate_card(p, copy) if i.level == "error"])
        built = manifest.build(p)
        entry = next(e for e in built["cards"] if "copy" in e)
        self.assertEqual((entry["type"], entry["effect"], entry["trap_threshold"]), ("Trap", 681, 1234))
        self.assertTrue(p.cards[1].is_monster())  # the copy's base stays a monster
        restored = Project(p.retail)
        manifest.apply(restored, built)
        self.assertEqual(restored.cards[copy].type, 21)
        self.assertEqual(restored.trap_threshold_override(copy), 1234)
        for invalid in ("-1", "65536", "1.5", "bad"):
            tab.vars["trap_threshold"].set(invalid)
            self.assertFalse(tab.apply())
            self.assertEqual(p.trap_threshold_override(copy), 1234)
        for valid in ("0", "65535", ""):
            tab.vars["trap_threshold"].set(valid)
            self.assertTrue(tab.apply())
            self.assertEqual(p.trap_threshold_override(copy), int(valid) if valid else None)
        tab.vars["trap_threshold"].set("700")
        self.assertTrue(tab.apply())
        # Special traps have no attack threshold. Applying the new effect
        # clears the old threshold and does not reveal monster statistics.
        tab.vars["effect"].set("Card 687")
        self.assertFalse(any(w.winfo_manager() for w in tab.trap_rows + tab.monster_rows))
        self.assertTrue(tab.apply())
        self.assertIsNone(p.trap_threshold_override(copy))
        tab.vars["type"].set("Magic")
        tab.vars["effect"].set("Card 605")
        self.assertTrue(tab.apply())
        self.assertEqual(p.cards[copy].attribute, 6)
        self.assertEqual(next(e for e in manifest.build(p)["cards"] if "copy" in e)["type"], "Magic")

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
        self.assertFalse(cards.price.instate(["disabled"]))
        self.assertEqual(cards.vars["starchips"].get(), "999999")
        cards.vars["starchips"].set("100")
        cards.vars["password"].set("00000723")
        self.assertTrue(cards.apply())
        added = max(app.project.added)
        self.assertEqual(app.project.starchip_cost(added), 100)
        self.assertEqual(app.project.password(added), "00000723")

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
        # Apply is at the top, beside the card's number; the Notes, last,
        # scroll into view when they get the focus.
        heads = [w for w in cards.form.winfo_children() if w.winfo_class() == "TFrame"]
        self.assertTrue(any(w.winfo_class() == "TButton" and w.cget("text") == "Apply"
                            for w in heads[0].winfo_children()))
        apply = cards.notes
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
        app.update()
        # Windows keeps a window within wm maxsize (the screen: 1024x768 on
        # CI), so check "no scrollbars" only where the window got the size.
        roomy = app.winfo_width() >= 1600 and app.winfo_height() >= 960
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
        if roomy:
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
        if roomy:
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
        app.notebook.select(app.values)
        app.update()
        scroll = app.values.scroll
        # Windows fonts can fit the whole form at 640px. Size the viewport
        # from the form itself so this exercises overflowing content there too.
        chrome_height = app.winfo_height() - scroll.canvas.winfo_height()
        app.minsize(1, 1)
        app.geometry(f"1100x{chrome_height + scroll.body.winfo_reqheight() // 2}")
        app.update()
        self.assertGreater(scroll.body.winfo_reqheight(), scroll.canvas.winfo_height())
        scroll.canvas.yview_moveto(1)
        self.assertGreater(scroll.canvas.yview()[0], 0)

    def test_conflicts_go_to_what_another_mod_changes_too(self):
        """A line about another mod goes to the tab that edits the thing, on
        its card; a starter pools line to Mod info, where they are kept."""
        import json as json_
        app = self.app
        p = app.project
        other = Path(self.tmp.name) / "other-mods"
        mod = other / "rival"
        mod.mkdir(parents=True, exist_ok=True)
        (mod / "mod.json").write_text(json_.dumps({
            "id": "rival", "name": "Rival", "version": "1",
            "cards": [{"replace": 5, "attack": 100}],
            "fusions": [{"with": [1, 2], "result": 3}],
            "equips": [{"card": 651, "add": [7]}]}))
        p.cards[5] = p.cards[5].copy(attack=4000)
        p.set_fusion(1, 2, 400)
        p.equips[651].add(9)
        app.conflicts.set_folder(str(other))
        issues = [i for i in app.conflicts.run() if i.area == "Other mods"]
        self.assertTrue(issues, [str(i) for i in app.conflicts.issues])
        for issue in issues:
            app.go_to(issue)
            app.update()
            kind = issue.target[0]
            tab = app.notebook.nametowidget(app.notebook.select()).tab
            if issue.where.startswith("Card"):
                self.assertIs(tab, app.cards)
                self.assertEqual(app.cards.current, 5)
            elif issue.where.startswith("Fusion"):
                self.assertIs(tab, app.fusions)
                self.assertTrue(app.fusions.tree.exists("1:2"), (issue.target, app.fusions.search.get(), app.fusions.tree.get_children()[:5]))
            elif issue.where.startswith("Equip"):
                self.assertIs(tab, app.equips)
                self.assertEqual(app.equips.current, 651)
            else:
                self.assertIsNotNone(kind)
        from fm_editor import validate as v
        app.go_to(v.Issue("error", "Starter pools", "pool 1", "x", 0))
        app.update()
        self.assertIs(app.notebook.nametowidget(app.notebook.select()).tab, app.starter)
        self.assertEqual(app.starter.pages.select(), str(app.starter.pools))

    def test_packs_empty_tab_and_a_pack_of_drops(self):
        """No pack: the tab says what packs are, only the ways to make one
        work; a pack of an opponent's drops deals its cards at their drop
        weights."""
        from fm_editor import packs as packmath
        app, tab = self.app, self.app.packs
        app.notebook.select(tab)
        app.update()
        self.assertTrue(tab.empty.winfo_manager())
        self.assertTrue(tab.add_button.instate(["!disabled"]))
        self.assertTrue(all(w.instate(["disabled"]) for w in (tab.export_button, tab.revert_button)))
        dialog = tab.add_from_drops()
        fields = [w for w in dialog.winfo_children()[0].winfo_children() if w.winfo_class() == "TCombobox"]
        fields[0].set(f"{1} {__import__('fm_editor.gamedata', fromlist=['x']).DUELIST_NAMES[1]}")
        fields[1].set("B/C/D drops")
        dialog.ok()
        self.assertEqual(len(app.project.packs), 1)
        entry = app.project.packs[0]
        want = {c: w for c, w in app.project.pools[1]["bcd"].items() if w > 0}
        got = {app.project.resolve(ref): w for ref, w in packmath.tier_pool(entry, "cards")}
        self.assertEqual(got, want)
        self.assertLessEqual(len(entry["name"]), packmath.NAME_LETTERS)
        self.assertFalse(tab.empty.winfo_manager())
        self.assertTrue(any(w.instate(["!disabled"]) for w in tab.right.winfo_children()[0].winfo_children()))

    def test_a_new_pack_asks_for_cards_not_an_error(self):
        app, tab = self.app, self.app.packs
        app.notebook.select(tab)
        app.update()
        tab.add_pack()
        self.assertEqual(str(tab.problem.cget("style")), "Hint.TLabel")
        self.assertIn("Add cards", tab.problem.cget("text"))
        tab.add_cards([1])
        self.assertNotIn("Add cards", tab.problem.cget("text"))

    def test_pack_tiers_slots_and_remove(self):
        """Tiers and slots keep a pack whole: the last tier stays, a removed
        tier leaves no guarantee, pity or slot naming it, an action keeps
        what the form holds, and a removed pack takes its picture along."""
        from fm_editor import pngio
        from fm_editor.tests.test_art import gradient
        app = self.app
        tab = app.packs
        app.notebook.select(tab)
        app.update()
        tab.add_pack()
        tab.add_cards([1, 2])
        entry = tab.current()
        tier = lambda name: (lambda title, n, t, on_ok: on_ok(name, {"odds": 1}))
        with mock.patch.object(tab, "tier_dialog", tier("rare")):
            tab.add_tier()
        self.assertEqual([n for n, _ in __import__("fm_editor.packs", fromlist=["x"]).tiers_of(entry)], ["cards", "rare"])
        tab.toggle_advanced()
        tab.adv["guarantee"].set("rare=1")
        self.assertTrue(tab.commit())
        # Slots: ticking them keeps the name typed (the form is stored first).
        tab.vars["name"].set("Typed name")
        tab.adv["use_slots"].set(True)
        tab.toggle_slots()
        self.assertEqual(entry["name"], "Typed name")
        self.assertEqual(entry["slots"], ["cards"] * entry_count(entry))
        with mock.patch.object(tab, "slot_dialog", lambda title, rule, on_ok: on_ok("rare")):
            tab.add_slot()
        self.assertEqual(entry["slots"][-1], "rare")
        tab.slots.selection_set("0")
        tab.move_slot(1)
        self.assertEqual(entry["slots"][1], "cards")
        tab.slots.selection_set(str(len(entry["slots"]) - 1))
        tab.remove_slot()
        # Removing "rare": no guarantee or slot names it any more.
        tab.fill()
        tab.tiers.selection_set("1")
        with mock.patch("fm_editor.packs_tab.messagebox.askyesno", return_value=True):
            tab.remove_tier()
        self.assertNotIn("rare", entry["tiers"])
        self.assertNotIn("guarantee", entry)
        self.assertNotIn("rare", entry["slots"])
        # The last tier stays: a pack deals from one at least, and adding a
        # card still works.
        tab.tiers.selection_set("0")
        with mock.patch("fm_editor.packs_tab.messagebox.showinfo") as told:
            tab.remove_tier()
        told.assert_called_once()
        self.assertEqual(len(entry["tiers"]), 1)
        tab.add_cards([3])
        # A pack of the mod's own with "tiers": {} opens and takes cards.
        entry["tiers"] = {}
        tab.fill()
        tab.add_cards([4])
        # A removed pack takes its picture along; one another pack shares stays.
        picture = Path(self.tmp.name) / "pack.png"
        pngio.write(picture, gradient(204, 192))
        tab.use_file(str(picture))
        image = tab.current()["image"]
        self.assertIn(image, app.project.files)
        with mock.patch("fm_editor.packs_tab.messagebox.askyesno", return_value=True):
            tab.remove()
        self.assertEqual(app.project.packs, [])
        self.assertNotIn(image, app.project.files)

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
        # "image_style": "full" shows the whole picture; "card" is written as no key.
        tab.vars["image_style"].set("full")
        tab.show_picture()
        self.assertEqual(tab.photos["card"].height(), 196 * 4)
        # At 1x a pixel under half opaque is clear (black here), the rest opaque, as the game's texture.
        from fm_editor.packs_tab import full_picture
        half = pngio.Image(140, 196, bytes((200, 100, 50, 100)) * (140 * 98) + bytes((200, 100, 50, 200)) * (140 * 98))
        shown = full_picture(half, 1).rgba
        self.assertEqual(shown[:4], bytes((0, 0, 0, 255)))
        self.assertEqual(shown[-4:], bytes((200, 100, 50, 255)))
        self.assertTrue(tab.commit())
        self.assertEqual(app.project.packs[0]["image_style"], "full")
        tab.vars["image_style"].set("card")
        self.assertTrue(tab.commit())
        from fm_editor import packs as packmath
        self.assertNotIn("image_style", packmath.minimize(app.project.packs[0]))
        tab.zoom.set(1)
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
        app.cards.show(None)
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
        app.fusions.fill()
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
        tab.fill()
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

    def test_icon_lists_count_as_form_edits_and_no_card_greys_the_form(self):
        from tkinter import ttk
        app, cards = self.app, self.app.cards
        cards.goto(1)
        app.update()
        self.assertNotIn(cards, app._pending)
        box = cards.star_boxes[0]
        app._remember_input(box)                    # pressed: what it held before the pick
        box._fill()
        other = next(i for i in range(1, box.menu.index("end") + 1) if box.menu.entrycget(i, "label") != box.get())
        box.menu.invoke(other)                      # another star picked from the menu
        app.update()
        self.assertIn(cards, app._pending)
        self.assertIn("Unapplied", app.edit_state.cget("text"))
        self.assertTrue(cards.apply())
        # No card: Apply, Revert, the icon lists are greyed; a card again,
        # they are back.
        cards.show(None)
        greyed = [w for w, _ in cards._idled]
        self.assertIn(box, greyed)
        self.assertTrue(any(isinstance(w, ttk.Button) and w.cget("text") == "Apply" for w in greyed))
        self.assertTrue(all(w.instate(["disabled"]) for w in greyed))
        cards.show(1)
        self.assertTrue(all(w.instate(["!disabled"]) for w in greyed))

    def test_card_heading_shows_the_picture_and_opens_the_art(self):
        app, cards = self.app, self.app.cards
        cards.goto(3)
        app.update()
        self.assertIsNotNone(cards._picture)         # the fixture's game files have pictures
        self.assertIn(str(cards._picture), str(cards.picture.cget("image")))
        cards.art_link.event_generate("<Button-1>")
        app.update()
        self.assertIs(app.notebook.current(), app.art)
        self.assertEqual(app.art.current, 3)
        # No effect: Add... and a line, no empty list.
        self.assertFalse(cards.effects_box.list_frame.winfo_ismapped() and not cards.effects_box.effects)
        cards.show(None)
        self.assertFalse(str(cards.picture.cget("image")))

    def test_small_checks_fusion_result_ritual_menu_pack_name(self):
        app, p = self.app, self.app.project
        # A fusion makes a monster: a magic card as the result is refused.
        magic = next(c for c in sorted(p.cards) if not p.cards[c].is_monster())
        dialog = app.fusions.add()
        for key, value in zip("abr", (1, 2, magic)):
            dialog.fields[key].set(value)
        dialog.ok()
        self.assertIn("makes a monster", dialog.error.cget("text"))
        dialog.destroy()
        # A ritual copy's right-click lists its base's tributes.
        ritual = sorted(p.retail.rituals)[0]
        copy = p.add_card(ritual, "rit")
        app.rituals.fill()
        cards = [c for c in p.rituals[ritual] if c]
        self.assertEqual(app.rituals.tree.cards_of(str(copy)), [copy] + cards)
        # A pack's name: its letters counted against what the shop shows.
        app.packs.add_pack()
        app.packs.vars["name"].set("A" * 20)
        self.assertEqual(app.packs.name_count.cget("text"), "20/16")
        self.assertEqual(str(app.packs.name_count.cget("style")), "Error.TLabel")

    def test_card_revert_asks_first(self):
        app, cards = self.app, self.app.cards
        cards.goto(3)
        cards.vars["attack"].set("2999")
        self.assertTrue(cards.apply())
        with mock.patch("fm_editor.tabs.messagebox.askyesno", return_value=False) as asked:
            cards.revert()
        asked.assert_called_once()
        self.assertEqual(app.project.cards[3].attack, 2999)
        with mock.patch("fm_editor.tabs.messagebox.askyesno", return_value=True):
            cards.revert()
        self.assertEqual(app.project.cards[3].attack, app.project.retail.cards[3].attack)
        with mock.patch("fm_editor.tabs.messagebox.askyesno") as asked:
            cards.revert()                  # as the disc has it: nothing to ask
        asked.assert_not_called()

    def test_welcome_without_game_files(self):
        """No game files: a panel over the tabs says what the editor needs,
        with a button to choose them; gone once a game is read."""
        from fm_editor.app import App
        with mock.patch("fm_editor.disc.find_game", return_value=None):
            app = App(ask=False, autostart=False)
            try:
                app.update()
                app.start(None, None, False)
                app.update()
                self.assertTrue(app.welcome.winfo_manager())
                buttons = [w.cget("text") for w in app.welcome.winfo_children() if w.winfo_class() == "TButton"]
                self.assertIn("Choose the game files...", buttons)
                app.start(str(self.game), None, False)
                app.update()
                self.assertFalse(app.welcome.winfo_manager())
            finally:
                app.dirty = False
                app.destroy()
        self.assertFalse(self.app.welcome.winfo_manager())      # the usual window: game files read

    def test_help_opens_the_guides(self):
        from pathlib import Path as P
        app = self.app
        helps = app.nametowidget(app.nametowidget(app.cget("menu")).entrycget("end", "menu"))
        labels = [helps.entrycget(i, "label") for i in range(helps.index("end") + 1) if helps.type(i) == "command"]
        self.assertIn("FM Editor guide", labels)
        root = P(__file__).resolve().parents[4]
        with mock.patch("webbrowser.open") as opened:
            for i in range(helps.index("end") + 1):
                if helps.type(i) == "command" and helps.entrycget(i, "label") != "About":
                    helps.invoke(i)
        urls = [call.args[0] for call in opened.call_args_list]
        self.assertEqual(len(urls), len(labels) - 1)
        for url in urls:                    # each a file of the repository
            self.assertTrue((root / url.split("/blob/master/")[1]).exists(), url)

    def test_ctrl_tab_goes_from_tab_to_tab(self):
        app = self.app
        app.notebook.select(app.cards)
        app.update()
        app.tk.call("ttk::notebook::CycleTab", str(app.notebook), 1)     # what Ctrl+Tab calls
        app.update()
        self.assertIs(app.notebook.current(), app.art)
        app.tk.call("ttk::notebook::CycleTab", str(app.notebook), -1)
        app.update()
        self.assertIs(app.notebook.current(), app.cards)

    def test_ctrl_f_goes_to_the_search(self):
        app = self.app
        app.deiconify()
        for tab in (app.cards, app.art, app.fusions):
            app.notebook.select(tab)
            app.update()
            tab.search.set("Dragon")
            app.find()
            app.update()
            # The window's own focus, which focus_get() gives only while the
            # desktop lets it be the active window (another program may have
            # the foreground): Tk keeps it and gives it back when it is.
            self.assertIs(app.focus_lastfor(), tab.search_entry)
            self.assertTrue(tab.search_entry.selection_present())
            tab.search.set("")
        app.notebook.select(app.values)
        app.update()
        app.find()                      # a tab with no search: nothing happens

    def test_undo_keeps_the_card_the_window_follows(self):
        app, cards = self.app, self.app.cards
        cards.goto(50)
        cards.vars["attack"].set("1234")
        self.assertTrue(cards.apply())
        app.update()
        app.undo()
        app.update()
        self.assertEqual(app.current_card, 50)      # not the Art tab's card
        app.notebook.select(app.fusions)
        app.update()
        self.assertEqual(app.fusions.search.get(), app.project.card_label(50))

    def test_conflicts_go_to_an_equip_another_mod_changes(self):
        from fm_editor import overlaps as ov, validate as v
        app = self.app
        equip = app.project.equip_cards()[-1]          # not the first, which the tab opens on
        app.go_to(v.Issue("warning", "Other mods", "Equip", "x", (ov.EQUIPS, ("equip", equip, equip))))
        app.update()
        self.assertIs(app.notebook.current(), app.equips)
        self.assertEqual(app.equips.current, equip)
        app.go_to(v.Issue("warning", "Other mods", "Card", "x", (ov.CARDS, ("text", 5))))
        app.update()
        self.assertIs(app.notebook.current(), app.cards)

    def test_settings_dialog_stays_open_when_mod_info_cannot_apply(self):
        app, info = self.app, self.app.info
        info.refresh()
        info.other.insert("1.0", '{"cards": 1}')         # a key the tabs write: refused
        dialog = info.add_setting()
        dialog.ok()
        self.assertTrue(dialog.winfo_exists())
        self.assertTrue(dialog.error.cget("text"))
        self.assertIn(info, app._pending)
        dialog.destroy()

    def test_review_fixes_packs_file_stars_gap_limits_and_slots(self):
        from fm_editor import guardian_stars as gs
        app = self.app
        # A mod whose packs are in a file: not "This mod sells no card packs".
        app.packs.refresh()
        self.assertTrue(app.packs.empty.winfo_manager())
        app.project.packs_file = "packs.json"
        app.packs.refresh()
        self.assertFalse(app.packs.empty.winfo_manager())
        app.project.packs_file = None
        # Guardian Stars: a gap below the last declared star is a star the
        # grid shows, its matchups kept.
        stars = app.stars
        app.project.other["guardian_stars"] = {"stars": [{"id": 11}, {"id": 13}]}
        stars.refresh()
        stars.pick_cell(12, 1)
        stars.value.set("300")
        stars.set_cell()
        self.assertEqual(gs.read(app.project.other["guardian_stars"]).grid[12][1], 300)
        # Values: what the opened mod sets, each time, with ↺ for it alone.
        app.project.other["limits"] = {"two_player": {"step": 1000}}
        app.values.refresh()
        self.assertEqual(app.values.vars["two_player.step"].get(), "1000")
        self.assertEqual(str(app.values.reverts["two_player.step"].cget("state")), "normal")
        app.values.revert("two_player.step")
        self.assertNotIn("limits", app.project.other)
        self.assertEqual(str(app.values.reverts["two_player.step"].cget("state")), "disabled")
        # A slot left with weights that add up to nothing deals from the first tier.
        entry = {"tiers": {"common": {"cards": []}, "rare": {"cards": []}},
                 "slots": [{"tiers": {"common": 0, "rare": 5}}]}
        del entry["tiers"]["rare"]
        app.packs.forget_tier(entry, "rare", "common")
        self.assertEqual(entry["slots"], ["common"])

    def test_every_tab_fits_a_1280_window(self):
        """No tab asks for more than a 1280x800 window gives it across: the
        bottom scrollbar shows on none (a long hint, three fields to a row
        or panes asking for their own sizes once made the Map, Packs,
        Rituals, Conflicts and Cards tabs wider)."""
        app = self.app
        app.deiconify()
        app.geometry("1280x800")
        app.update()
        if app.winfo_width() < 1280:
            self.skipTest("the screen is smaller than 1280 wide")
        from tkinter import font
        wide, why = [], [f"TkDefaultFont {font.nametofont('TkDefaultFont').actual()}, "
                         f"tk scaling {app.tk.call('tk', 'scaling')}"]
        for tab in app.tabs:
            app.notebook.select(tab)
            for _ in range(3):
                app.update()
                app.after(50)
            if tab.page.xbar.winfo_ismapped():
                wide.append((app.notebook.tab(tab.page, "text"), tab.winfo_reqwidth()))
                why.append(wide[-1][0] + ":")
                self.widest(tab, tab.winfo_reqwidth() * 2 // 5, 1, why)
        self.assertEqual(wide, [], "\n".join(why))

    def widest(self, widget, least, depth, lines):
        """For a failure, what makes a tab as wide as it is: the widgets in it
        asking for at least `least` pixels across, six levels down."""
        for child in sorted(widget.winfo_children(), key=lambda c: -c.winfo_reqwidth()):
            if child.winfo_reqwidth() < least or depth > 6:
                return
            try:
                text = repr(str(child.cget("text"))[:60])
            except tk.TclError:
                text = ""
            lines.append(f"{'  ' * depth}{child.winfo_class()} {child.winfo_reqwidth()} {text}")
            self.widest(child, least, depth + 1, lines)

    def test_cards_tab_fits_the_window_in_both_looks(self):
        """The Cards tab asks for what its list and form ask, not for its
        panes' sizes as they are: at 1400 wide no bottom scrollbar, light or
        dark; and a half-ticked box shows in the dark look too."""
        from tkinter import ttk
        from fm_editor import theme
        app = self.app
        app.deiconify()
        app.geometry("1400x900")
        app.cards.goto(1)
        for dark in (False, True):
            app.dark.set(dark)
            app.toggle_dark()
            for _ in range(3):
                app.update()
                app.after(100)
            if app.winfo_width() >= 1400:
                self.assertFalse(app.cards.page.xbar.winfo_ismapped(), dark)
            self.assertLess(app.cards.winfo_reqwidth(), 1000)
        style = ttk.Style(app)
        self.assertEqual(style.theme_use(), theme.DARK_THEME)
        self.assertIn("alternate", str(style.map("TCheckbutton", "indicatorbackground")))
        app.dark.set(False)
        app.toggle_dark()

    def test_art_tab_keeps_its_card_through_undo(self):
        from fm_editor import pngio
        from fm_editor.tests.test_art import gradient
        app, tab = self.app, self.app.art
        app.notebook.select(tab)
        app.update()
        tab.goto(2)
        picture = Path(self.tmp.name) / "art2.png"
        pngio.write(picture, gradient(102, 96))
        tab.use_file("art", str(picture))
        app.update()
        app.undo()
        app.update()
        self.assertEqual(app.art.current, 2)
        self.assertEqual(app.art.tree.selection(), ("2",))
        # No card: the import and export buttons are greyed.
        tab.show(None)
        buttons = [b for row in tab.rows.values() for b in row["side"].buttons.winfo_children()]
        self.assertTrue(all(b.instate(["disabled"]) for b in buttons))
        tab.show(2)
        self.assertTrue(any(b.instate(["!disabled"]) for b in buttons))

    def test_conflicts_check_only_when_shown_and_pool_revert_asks(self):
        app = self.app
        app.notebook.select(app.cards)
        app.update()
        with mock.patch("fm_editor.validate.validate", return_value=[]) as checked:
            app.conflicts.refresh()             # as Undo does, the tab not up
            checked.assert_not_called()
            self.assertIn("Not checked", app.conflicts.summary.cget("text"))
            app.show_conflicts()
            app.update()
            self.assertEqual(checked.call_count, 1)     # once, not twice
            app.show_conflicts()
            self.assertEqual(checked.call_count, 2)
        # Revert pool asks first, and does nothing to a pool as the disc has it.
        tab = app.duelists
        d = 1
        tab.goto((d, "deck"))
        card = next(iter(app.project.pools[d]["deck"]))
        app.project.pools[d]["deck"][card] = 7
        with mock.patch("fm_editor.tabs.messagebox.askyesno", return_value=False) as asked:
            tab.revert()
        asked.assert_called_once()
        self.assertEqual(app.project.pools[d]["deck"][card], 7)
        with mock.patch("fm_editor.tabs.messagebox.askyesno", return_value=True):
            tab.revert()
        self.assertEqual({c: w for c, w in app.project.pools[d]["deck"].items() if w},
                         app.project.retail.pools[d]["deck"])
        with mock.patch("fm_editor.tabs.messagebox.askyesno") as asked:
            tab.revert()
        asked.assert_not_called()
        self.assertEqual(tab.list.set("0", "state"), "not used")

    def test_conflicts_levels_and_clean_state_and_pool_summary(self):
        from fm_editor import validate as v
        app = self.app
        tab = app.conflicts
        tab.issues = []
        tab.show_issues()
        self.assertTrue(tab.clean.winfo_manager())
        tab.issues = [v.Issue("error", "Cards", "#1", "bad", 1), v.Issue("note", "Cards", "#2", "fine", 2),
                      v.Issue("warning", "Cards", "#3", "hm", 3)]
        tab.level.set("error")
        tab.show_issues()
        self.assertEqual(tab.tree.get_children(), ("0",))
        self.assertFalse(tab.clean.winfo_manager())
        tab.level.set("all")
        tab.show_issues()
        self.assertEqual(len(tab.tree.get_children()), 3)
        # Duelists: a line of what the pool deals, and the disc's once changed.
        duel = app.duelists
        duel.goto((1, "deck"))
        self.assertTrue(duel.summary.cget("text").startswith("The 40 it deals most often: Monsters"))
        self.assertNotIn("the disc's", duel.summary.cget("text"))
        strongest = max(app.project.monsters(), key=lambda c: app.project.cards[c].attack)
        app.project.pools[1]["deck"][strongest] = 900
        duel.fill()
        self.assertIn("the disc's", duel.summary.cget("text"))
        duel.goto((1, "bcd"))
        self.assertTrue(duel.summary.cget("text").startswith("A drop: monsters"))

    def test_mod_info_settings_list(self):
        """Settings by a list and a dialog, written into the JSON (and the
        mod) at once; a key unknown to the dialog stays; a setting entries
        use asks before it goes."""
        app, info = self.app, self.app.info
        p = app.project
        info.refresh()
        dialog = info.add_setting()
        dialog.fields["key"].set("hard")
        dialog.fields["label"].set("Hard fusions")
        dialog.ok()
        self.assertEqual(p.info.settings, [{"key": "hard", "label": "Hard fusions", "type": "bool", "default": 0}])
        self.assertTrue(app.dirty)
        # A choice: two choices at least, its default one of them.
        dialog = info.add_setting()
        dialog.fields["key"].set("mode")
        dialog.fields["type"].set("choice")
        dialog.fields["choices"].insert("1.0", "Classic")
        dialog.ok()
        self.assertIn("two choices", dialog.error.cget("text"))
        dialog.fields["choices"].insert("end", "\nCustom")
        dialog.fields["default"].set("5")
        dialog.ok()
        self.assertIn("number of a choice", dialog.error.cget("text"))
        dialog.fields["default"].set("1")
        dialog.ok()
        self.assertEqual(p.info.settings[1]["choices"], ["Classic", "Custom"])
        self.assertEqual(info.settings_tree.set("1", "default"), "Custom")
        # The same key twice is refused.
        dialog = info.add_setting()
        dialog.fields["key"].set("hard")
        dialog.ok()
        self.assertIn("another setting", dialog.error.cget("text"))
        dialog.destroy()
        # Edited as an int; a key the dialog has no field for is kept.
        p.info.settings[0]["later_key"] = 7
        info.refresh()
        info.settings_tree.selection_set("0")
        dialog = info.edit_setting()
        dialog.fields["type"].set("int")
        dialog.fields["min"].set("10")
        dialog.fields["max"].set("5")
        dialog.ok()
        self.assertIn("Lowest", dialog.error.cget("text"))
        dialog.fields["max"].set("300")
        dialog.fields["suffix"].set("%")
        dialog.ok()
        self.assertEqual(p.info.settings[0], {"key": "hard", "label": "Hard fusions", "type": "int", "default": 0,
                                              "later_key": 7, "min": 10, "max": 300, "suffix": "%"})
        # Up and Down; Remove asks when an entry is switched by it.
        info.settings_tree.selection_set("1")
        info.move_setting(-1)
        self.assertEqual([s["key"] for s in p.info.settings], ["mode", "hard"])
        p.kept["fusions"].append({"with": [1, 2], "result": 3, "setting": "hard"})
        info.fill_settings()
        self.assertEqual(info.settings_tree.set("1", "used"), "1 entry")
        info.settings_tree.selection_set("1")
        with mock.patch("fm_editor.tabs.messagebox.askyesno", return_value=False) as asked:
            info.remove_setting()
        asked.assert_called_once()
        self.assertEqual(len(p.info.settings), 2)
        info.settings_tree.selection_set("0")
        info.remove_setting()               # "mode" switches nothing: no question
        self.assertEqual([s["key"] for s in p.info.settings], ["hard"])
        # The JSON page is the same list; one that does not read is said.
        info.settings.delete("1.0", "end")
        info.settings.insert("1.0", "[{")
        info.fill_settings()
        self.assertIn("does not read", info.settings_note.cget("text"))

    def test_mod_info_keeps_what_other_tabs_put_beside_its_box(self):
        """Mod info's other keys were rebuilt from its box as it was filled:
        a key another tab wrote since (starter_pools, a password taken out
        with a removed card) was lost on the next commit, a save included.
        Only what is typed in the box changes the mod now."""
        app, info = self.app, self.app.info
        info.refresh()
        app.project.other["text"] = {"a": 1}                     # as if another tab wrote it
        app.project.other["starter_pools"] = [{"draws": 40, "cards": {"2": 1}}]
        self.assertTrue(info.commit())
        self.assertEqual(app.project.other["text"], {"a": 1})
        self.assertIn("starter_pools", app.project.other)
        # What is typed is applied, and a key taken out of the box goes.
        info.refresh()
        info.other.delete("1.0", "end")
        info.other.insert("1.0", json.dumps({"audio": {"x": 2}}))
        self.assertTrue(info.commit())
        self.assertEqual(app.project.other.get("audio"), {"x": 2})
        self.assertNotIn("text", app.project.other)
        self.assertIn("starter_pools", app.project.other)       # not the box's to take away

    def test_pools_follow_renames_and_removed_cards_and_null_keys(self):
        """Starter pools are written by their cards as named at save (the
        mod's id or an added card's key changed since); a removed added card
        leaves them; a key typed as null in Mod info is a key."""
        from fm_editor import manifest, starter_pools as sp
        app, p = self.app, self.app.project
        p.info.id = "mymod"
        added = p.add_card(2, "card-1")
        page = app.starter.pools
        dialog = page.add_pool()
        dialog.fields["draws"].set("40")
        dialog.ok()
        with mock.patch("fm_editor.starter_pools_view.pick_card", return_value=added):
            page.weight.set("5")
            page.add_card()
        with mock.patch("fm_editor.starter_pools_view.pick_card", return_value=3):
            page.add_card()
        p.info.id = "renamed"
        built = manifest.build(p)["starter_pools"]
        self.assertIn("renamed:card-1:1", built[0]["cards"])
        self.assertNotIn("mymod:card-1:1", built[0]["cards"])
        # Removed, the card leaves the pools; the next card added is not in them.
        p.remove_card(added)
        self.assertEqual(sp.state(p)[0].cards, {3: 5})
        again = p.add_card(2, "card-2")
        self.assertNotIn(again, sp.state(p)[0].cards)
        # Mod info: null is a value; a readable pools section is the page's.
        info = app.info
        info.refresh()
        self.assertNotIn("starter_pools", info.other.get("1.0", "end"))
        info.other.delete("1.0", "end")
        info.other.insert("1.0", json.dumps({"foo": None, "bar": 1}))
        self.assertTrue(info.commit())
        self.assertIn("foo", p.other)
        self.assertIsNone(p.other["foo"])
        # A section the page cannot read shows in the box, to mend by hand.
        p.other["starter_pools"] = "pools.json"
        info.refresh()
        self.assertIn("pools.json", info.other.get("1.0", "end"))

    def test_mod_info_refuses_keys_the_tabs_write(self):
        app = self.app
        info = app.info
        info.refresh()
        for key in ("packs", "starter", "pack_shop", "fusions", "limits", "guardian_stars"):
            info.other.delete("1.0", "end")
            info.other.insert("1.0", json.dumps({key: []}))
            self.assertFalse(info.commit(), key)
        info.other.delete("1.0", "end")
        info.other.insert("1.0", json.dumps({"text": {"x": 1}}))
        self.assertTrue(info.commit())
        self.assertEqual(app.project.other["text"], {"x": 1})

    def test_mod_info_says_the_game_it_needs(self):
        """Mod info names the mod API a save writes as "min_api", and why;
        one the author wrote higher stays."""
        app = self.app
        info = app.info
        info.refresh()
        self.assertIn("needs no \"min_api\"", info.api.cget("text"))
        app.project.set_tags(1, ["dragon"])
        info.refresh()
        text = info.api.cget("text")
        self.assertIn("Mod API 11", text)
        self.assertIn("card tags", text)
        self.assertIn("v0.3.0 or newer", text)
        app.project.set_tags(1, None)
        info.other.delete("1.0", "end")
        info.other.insert("1.0", json.dumps({"min_api": 10}))
        self.assertTrue(info.commit())
        text = info.api.cget("text")
        self.assertIn("Mod API 10", text)
        self.assertIn("As written", text)

    def test_equip_types_panel(self):
        """A type's box ticks every monster of it on or off, shows a half
        tick for some, and its count lists that type's monsters alone."""
        from fm_editor.gamedata import TYPE_NAMES
        app, tab = self.app, self.app.equips
        p = tab.project
        equip = tab.current
        dragon = TYPE_NAMES.index("Dragon")
        dragons = {c for c in p.monsters() if p.cards[c].type == dragon}
        box, var = tab.type_boxes[dragon]
        var.set(1)
        tab.toggle_type(dragon)
        self.assertLessEqual(dragons, p.equip_targets(equip))
        self.assertTrue(var.get() and not box.instate(["alternate"]))
        self.assertEqual(tab.type_counts[dragon].cget("text"), f"{len(dragons)}/{len(dragons)}")
        # Take one away by hand: half ticked.
        one = min(dragons)
        tab.monsters.selection_set(str(one))
        tab.remove()
        self.assertTrue(box.instate(["alternate"]) or len(dragons) == 1)
        # A half-ticked box clicked ticks them all; clicked again, none.
        var.set(1)
        tab.toggle_type(dragon)
        self.assertLessEqual(dragons, p.equip_targets(equip))
        var.set(0)
        tab.toggle_type(dragon)
        self.assertFalse(dragons & p.equip_targets(equip))
        # The count lists that type's monsters only, and again all of them.
        var.set(1)
        tab.toggle_type(dragon)
        tab.show_type(dragon)
        shown = {int(i) for i in tab.monsters.get_children()}
        self.assertTrue(shown and all(p.cards[c].type == dragon for c in shown))
        tab.show_type(dragon)
        self.assertGreater(len(tab.monsters.get_children()), len(shown) - 1)
        self.assertIsNone(tab.only_type)

    def test_ritual_remove_and_revert(self):
        """Remove recipe on a disc ritual and on an added copy (which would
        otherwise keep its base's), the row still selected; revert."""
        from fm_editor import manifest
        app = self.app
        tab = app.rituals
        p = tab.project
        p.info.id = "rit"
        ritual = sorted(p.retail.rituals)[0]
        copy = p.add_card(ritual, "copy")
        tab.fill()
        self.assertEqual(tab.tree.set(str(copy), "state"), "as base")
        for card in (ritual, copy):
            tab.tree.selection_set(str(card))
            tab.remove()
            self.assertEqual(tab.tree.selection(), (str(card),))
            self.assertEqual(tab.tree.set(str(card), "state"), "removed")
        built = manifest.build_rituals(p)
        self.assertIn({"card": p.ref(ritual), "result": None}, built)
        self.assertIn({"card": p.ref(copy), "result": None}, built)
        from fm_editor.model import Project
        again = Project(p.retail)
        manifest.apply(again, json.loads(manifest.dumps(manifest.build(p))))
        self.assertEqual(again.ritual_status(copy), "removed")
        self.assertEqual(manifest.build_rituals(again), built)
        # Reverted, the copy is its base's ritual again; the disc's is back.
        for card in (ritual, copy):
            tab.tree.selection_set(str(card))
            tab.revert()
        self.assertEqual(tab.tree.set(str(copy), "state"), "as base")
        self.assertEqual(manifest.build_rituals(p), [])
        from fm_editor import validate
        validate.validate(p)

    def test_fusion_actions_keep_the_search_and_selection(self):
        """Add, change, remove and revert from the Fusions tab: the modder's
        search stays, the pair acted on stays selected, a typing pause
        refills the list, and a header double-click opens nothing."""
        from fm_editor import manifest
        app = self.app
        tab = app.fusions
        p = tab.project
        app.notebook.select(tab)
        app.update()
        tab.search.set("Mystic")
        self.assertTrue(tab._fill_job)            # waiting for the typing to stop
        app.after(250)
        app.update()
        self.assertIsNone(tab._fill_job)
        rows = tab.tree.get_children()
        self.assertTrue(rows)
        pair = tuple(int(x) for x in rows[0].split(":"))
        # Change result: prefilled with what the row shows, the row kept.
        tab.tree.selection_set(rows[0])
        dialog = tab.edit()
        self.assertEqual(dialog.fields["r"].get(), p.fusions.get(pair) or p.retail.fusions.get(pair))
        dialog.fields["r"].set(500)
        dialog.ok()
        self.assertEqual(p.fusions[pair], 500)
        self.assertEqual(tab.search.get(), "Mystic")
        self.assertEqual(tab.tree.selection(), (rows[0],))
        # Remove, then revert: still selected, back to the disc's.
        tab.remove()
        self.assertEqual(tab.tree.selection(), (rows[0],))
        self.assertEqual(tab.tree.set(rows[0], "state"), "removed")
        self.assertIn(", 1 removed", tab.count.cget("text"))
        tab.revert()
        self.assertEqual(tab.tree.selection(), (rows[0],))
        self.assertEqual(p.fusions.get(pair), p.retail.fusions.get(pair))
        # Add a pair the search does not show: the tab shows card A's.
        new = next((a, b) for a in range(1, 40) for b in range(a, 40) if (a, b) not in p.retail.fusions)
        dialog = tab.add()
        for key, value in zip("abr", (new[0], new[1], 600)):
            dialog.fields[key].set(value)
        dialog.ok()
        self.assertEqual(p.fusions[new], 600)
        self.assertEqual(tab.tree.selection(), (f"{new[0]}:{new[1]}",))
        self.assertIn({"with": [p.ref(new[0]), p.ref(new[1])], "result": p.ref(600)}, manifest.build_fusions(p))
        # Changed only: what the mod changed.
        tab.changed_only.set(True)
        tab.fill()
        self.assertEqual(tab.rows(), [f"{new[0]}:{new[1]}"])
        tab.changed_only.set(False)
        # A double-click on a heading is no double-click on the selected row.
        tab.tree.update_idletasks()
        group = tab.tree.get_children()[0]          # card A's: "... fuses with: N pairs"
        self.assertTrue(group.startswith("group:"))
        self.assertIn("fuses with", tab.tree.set(group, "a"))
        heading = mock.Mock(x=10, y=3)
        on_group = mock.Mock(x=10, y=tab.tree.bbox(group)[1] + 2)
        row = mock.Mock(x=10, y=tab.tree.bbox(tab.rows()[0])[1] + 2)
        with mock.patch.object(tab, "edit") as edit:
            tab.double_click(heading)
            tab.double_click(on_group)
            edit.assert_not_called()
            tab.double_click(row)
            edit.assert_called_once()

    def test_remove_all_fusions(self):
        from fm_editor import manifest
        app = self.app
        tab = app.fusions
        p = tab.project
        p.set_fusion(1, 2, 500)
        app.changed()
        self.pause()
        tab.fill()
        self.assertEqual(tab.all_button.cget("text"), "Remove all fusions...")
        with mock.patch("fm_editor.tabs.messagebox.askyesno", return_value=False):
            tab.all_button.invoke()
        self.assertFalse(p.fusion_remove_all)
        with mock.patch("fm_editor.tabs.messagebox.askyesno", return_value=True):
            tab.all_button.invoke()
        self.assertTrue(p.fusion_remove_all and app.dirty)
        self.assertEqual(manifest.build_fusions(p), [{"remove": "all"}])
        self.assertFalse([pair for pair, made in p.fusions.items() if made])
        self.assertEqual(tab.all_button.cget("text"), "Restore disc fusions")
        app.update()
        self.assertTrue(tab.all_banner.winfo_ismapped())
        self.assertIn("Every disc fusion is removed", tab.all_note.cget("text"))
        # The disc's pairs are hidden: the list is what still fuses.
        self.assertEqual(tab.tree.get_children(), ())
        self.assertTrue(tab.count.cget("text").startswith("0 fusions"))
        tab.changed_only.set(False)
        tab.search.set("Blue Dragon")
        tab.fill()
        self.assertFalse(tab.tree.exists("1:2"))
        tab.show_removed.set(True)
        tab.fill()
        self.assertEqual(tab.tree.set("1:2", "state"), "removed")
        tab.show_removed.set(False)
        p.set_fusion(1, 2, 500)
        tab.fill()
        self.assertEqual(tab.tree.get_children(), ("1:2",))
        p.set_fusion(1, 2, None)
        # Undo brings it all back, the button too.
        app.update()
        app.undo()
        app.update()
        p = app.project
        self.assertFalse(p.fusion_remove_all)
        self.assertEqual(p.fusions[(1, 2)], 500)
        self.assertEqual(tab.all_button.cget("text"), "Remove all fusions...")
        app.update()
        self.assertFalse(tab.all_banner.winfo_ismapped())
        with mock.patch("fm_editor.tabs.messagebox.askyesno", return_value=True):
            tab.all_button.invoke()
            tab.all_button.invoke()             # Restore disc fusions
        self.assertFalse(p.fusion_remove_all)
        self.assertEqual(manifest.build_fusions(p), [])
        self.assertEqual(p.fusions, p.retail.fusions)

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

    def test_history_card_conversion_and_equip_targets(self):
        from fm_editor.gamedata import TYPE_NAMES
        app = self.app
        original = app.project.cards[1].type
        app.cards.goto(1)
        app.cards.vars["type"].set(TYPE_NAMES[23])
        app.cards.show_kind()
        app.cards.vars["effect"].set(app.cards.effect_label(301))
        self.assertTrue(app.cards.apply(quiet=True))
        self.pause()
        app.notebook.select(app.equips)
        app.update()
        self.assertTrue(app.equips.equips.exists("1"))
        app.project.equips[1] = {2, 3}
        app.changed()
        self.pause()
        app.undo()
        app.update()
        self.assertNotIn(1, app.project.equips)
        self.assertEqual(app.project.cards[1].type, 23)
        app.undo()
        app.update()
        self.assertEqual(app.project.cards[1].type, original)
        self.assertFalse(app.equips.equips.exists("1"))
        self.assertFalse(app.dirty)
        app.redo()
        app.update()
        app.redo()
        app.update()
        self.assertEqual(app.project.equips[1], {2, 3})
        self.assertTrue(app.equips.equips.exists("1"))

    def test_invalid_form_stays_visible_when_switching_tabs(self):
        app = self.app
        app.cards.goto(1)
        app.cards.vars["attack"].set("unfinished")
        app.notebook.select(app.equips)
        app.update()
        self.assertIs(app.notebook.current(), app.cards)
        self.assertEqual(app.cards.vars["attack"].get(), "unfinished")
        self.assertTrue(app.cards.status.cget("text"))
        app.cards.vars["attack"].set("1234")
        app.notebook.select(app.equips)
        app.update()
        self.assertIs(app.notebook.current(), app.equips)
        self.assertEqual(app.project.cards[1].attack, 1234)

    def test_undo_commits_pending_text_and_keeps_selection(self):
        app = self.app
        app.cards.goto(10)
        old = app.project.cards[10].name
        app.cards.vars["name"].set("Pending undo")
        app.undo()
        app.update()
        self.assertEqual(app.project.cards[10].name, old)
        self.assertEqual(app.cards.current, 10)
        self.assertEqual(app.cards.vars["name"].get(), old)
        app.redo()
        app.update()
        self.assertEqual(app.cards.vars["name"].get(), "Pending undo")

    def test_pending_drafts_and_recovery_failure(self):
        from fm_editor import recovery
        app = self.app
        app.cards.goto(1)
        widget = app.cards.text
        app._remember_input(widget)
        widget.insert("end", "Draft text")
        app._form_input(app.cards, widget)
        self.assertTrue(app.title().startswith("*"))
        self.assertIn(app.cards, app._pending)
        app.cards.vars["attack"].set("unfinished")
        app.autosave()
        rows = [row for row in recovery.records() if row[0].parent == app.recovery.folder]
        self.assertEqual(len(rows), 1)
        forms = rows[0][2]["forms"]
        self.assertEqual(forms["cards"]["vars"]["attack"], "unfinished")
        app.discard_forms()
        self.assertNotEqual(app.cards.vars["attack"].get(), "unfinished")
        app.restore_drafts(forms)
        self.assertEqual(app.cards.vars["attack"].get(), "unfinished")
        self.assertTrue(widget.get("1.0", "end-1c").endswith("Draft text"))
        with mock.patch.object(app.recovery, "write_job", side_effect=OSError("disk full")):
            app.autosave()
        self.assertIn("Recovery copy failed", app.edit_state.cget("text"))
        self.assertTrue(rows[0][1].exists())

    def test_save_backup_failure_does_not_overwrite_mod(self):
        from fm_editor import manifest, recovery
        app = self.app
        folder = Path(self.tmp.name) / "history-save"
        manifest.save_mod(app.project, folder)
        before = (folder / "mod.json").read_bytes()
        app.cards.goto(1)
        app.cards.vars["name"].set("Unsaved name")
        with mock.patch.object(recovery, "backup", side_effect=OSError("backup failed")), \
                mock.patch("fm_editor.app.messagebox.showerror") as error:
            self.assertFalse(app.save())
        error.assert_called_once()
        self.assertEqual((folder / "mod.json").read_bytes(), before)
        self.assertTrue(app.dirty)
        self.assertTrue(app.save())
        self.assertFalse(app.dirty)
        app.undo()
        app.update()
        self.assertTrue(app.dirty)
        app.redo()
        app.update()
        self.assertFalse(app.dirty)


    def test_keyboard_pending_apply_and_undo_shortcuts(self):
        app = self.app
        app.deiconify()
        app.cards.goto(1)
        app.update()
        text = app.cards.text
        text.focus_force()
        app.update()
        original = text.get("1.0", "end-1c")
        text.mark_set("insert", "end-1c")
        text.event_generate("<KeyPress-x>")
        text.event_generate("<KeyRelease-x>")
        app.update()
        self.assertIn(app.cards, app._pending)
        self.assertTrue(app.title().startswith("*"))
        self.assertTrue(app.cards.apply(quiet=True))
        app.update()
        self.assertNotIn(app.cards, app._pending)
        text.event_generate("<Control-z>")
        text.event_generate("<KeyRelease-z>", state=4)
        app.update()
        self.assertEqual(text.get("1.0", "end-1c"), original)
        self.assertFalse(app._pending)
        text.event_generate("<Control-Shift-Z>")
        app.update()
        self.assertEqual(text.get("1.0", "end-1c"), original + "x")

    def test_autosave_uses_the_history_snapshot_only_when_current(self):
        from fm_editor import manifest, recovery
        app = self.app

        def copied():
            row = next(row for row in recovery.records() if row[0].parent == app.recovery.folder)
            return manifest.open_mod(app.retail, row[1])[0].cards[1].name

        app.project.cards[1].name = "Recorded"
        app.changed()
        self.pause()                        # the history records it once the changes pause
        self.assertIsNone(app._history_job)
        with mock.patch.object(recovery, "Snapshot", side_effect=AssertionError("snapshot taken twice")):
            app.autosave()
        self.assertEqual(copied(), "Recorded")
        app.project.cards[1].name = "Not recorded yet"
        app.changed()                       # no idle time: the history is behind
        self.assertIsNotNone(app._history_job)
        app.autosave()
        self.assertEqual(copied(), "Not recorded yet")

    def test_recovered_copy_survives_multiple_autosaves_and_save_as(self):
        from fm_editor import manifest, recovery
        app = self.app
        app.project.files["extra.bin"] = b"keep across autosaves"
        app.project.cards[1].name = "Crash recovery"
        prior = recovery.Recovery()
        prior.write(app.project)
        row = next(row for row in recovery.records() if row[0].parent == prior.folder)
        app.set_project(type(app.project)(app.retail))
        self.assertTrue(app.open_recovery(row[1], row[2]))
        app.update()
        self.assertTrue(app._recovered)
        self.assertEqual(app.project.cards[1].name, "Crash recovery")
        app.autosave()
        app.project.info.author = "More edits"
        app.changed()
        app.autosave()
        latest = next(row for row in recovery.records() if row[0].parent == app.recovery.folder)
        self.assertEqual((latest[1] / "extra.bin").read_bytes(), b"keep across autosaves")
        self.assertTrue(row[1].exists(), "original recovery copy remains until saved")
        source = app.project.source_dir
        destination = Path(self.tmp.name) / "recovered-save"
        with mock.patch("fm_editor.file_dialogs.askdirectory", return_value=str(destination)) as choose:
            self.assertTrue(app.save())
        choose.assert_called_once()
        self.assertFalse(app._recovered)
        self.assertFalse(source.exists())
        # Saved now: the crashed session is no longer offered at start.
        self.assertFalse(prior.folder.exists())
        reopened, _ = manifest.open_mod(app.retail, destination)
        self.assertEqual(reopened.cards[1].name, "Crash recovery")
        self.assertEqual((destination / "extra.bin").read_bytes(), b"keep across autosaves")


    def test_card_form_marks_what_differs_from_the_disc(self):
        cards, p = self.app.cards, self.app.project
        cards.tree.selection_set("1")
        cards.select()
        self.app.update()
        # As the disc has it: no disc values repeated, no caption marked.
        for key in ("name", "attack", "defense", "level", "password", "starchips", "text"):
            with self.subTest(key=key):
                self.assertEqual(cards.hints[key].cget("text"), "")
        self.assertEqual(str(cards.captions["attack"].cget("style")), "TLabel")
        retail = p.retail.cards[1].attack
        cards.vars["attack"].set(str(retail + 200))
        self.app.update()
        self.assertEqual(cards.hints["attack"].cget("text"), f"Retail: {retail} (restore)")
        self.assertEqual(str(cards.hints["attack"].cget("style")), "Changed.TLabel")
        self.assertEqual(str(cards.captions["attack"].cget("style")), "Changed.TLabel")
        self.assertEqual(cards.hints["defense"].cget("text"), "")
        # Applied, it stays marked; a click on the disc value puts it back
        # in the form, unapplied until Apply.
        self.assertTrue(cards.apply())
        self.app.update()
        self.assertEqual(p.cards[1].attack, retail + 200)
        self.assertIn("Retail:", cards.hints["attack"].cget("text"))
        cards.hints["attack"].event_generate("<Button-1>")
        self.app.update()
        self.assertEqual(cards.vars["attack"].get(), str(retail))
        self.assertEqual(cards.hints["attack"].cget("text"), "")
        self.assertIn(cards, self.app._pending)
        self.assertTrue(cards.apply())
        self.assertEqual(p.cards[1].attack, retail)
        # The card text too.
        cards.text.insert("end", " more")
        cards.text.event_generate("<KeyRelease>")
        self.app.update()
        self.assertEqual(cards.hints["text"].cget("text"), "Restore retail text")
        cards.restore("text")
        self.assertEqual(cards.text.get("1.0", "end-1c"), p.retail.cards[1].description)
        # The window's line says unapplied and unsaved apart.
        self.assertEqual(str(self.app.edit_state.cget("style")), "Warning.TLabel")
        self.assertTrue(self.app.commit_all())
        self.assertEqual(str(self.app.edit_state.cget("style")),
                         "Changed.TLabel" if self.app.dirty else "TLabel")

    def test_card_tags_field(self):
        from fm_editor import manifest, tabs
        app, cards, p = self.app, self.app.cards, self.app.project
        # The frames by their color too.
        cards.goto(1)
        self.assertEqual(cards.vars["frame"].get(), "By type")
        cards.vars["frame"].set("Monster (gold)")
        cards.vars["tags"].set(" god ,dragon, god")
        app.update()
        self.assertEqual(cards.hints["tags"].cget("text"), "Retail: none (restore)")
        self.assertEqual(str(cards.captions["tags"].cget("style")), "Changed.TLabel")
        self.assertTrue(cards.apply())
        self.assertEqual(cards.vars["tags"].get(), "god, dragon")
        self.assertEqual(p.tags_of(1), (["god", "dragon"], False))
        self.assertIn({"replace": 1, "frame": "Monster", "tags": ["god", "dragon"]}, manifest.build(p)["cards"])
        self.assertNotIn("tags", cards.extra.cget("text"))     # a field of its own, not kept as written
        self.assertEqual(tabs.frame_value("Ritual (blue)"), 3)
        self.assertEqual(tabs.frame_value("Ritual"), 3)          # as earlier editors showed it
        # The search finds them.
        cards.search.set("drag")
        app.update()
        self.assertTrue(cards.tree.exists("1"))
        cards.search.set("")
        # An added card: blank is its base's, said beside it; [] is none.
        cards.add_card()
        added = cards.current
        app.update()
        self.assertEqual(cards.vars["tags"].get(), "")
        self.assertEqual(cards.hints["tags"].cget("text"), "Base's: god, dragon")
        self.assertEqual(p.tags_of(added), (["god", "dragon"], True))
        cards.vars["tags"].set("[]")
        app.update()
        self.assertEqual(cards.hints["tags"].cget("text"), "Base: god, dragon (restore)")
        self.assertTrue(cards.apply())
        self.assertEqual(p.tags_of(added), ([], False))
        self.assertEqual(manifest.build(p)["cards"][-1]["tags"], [])
        # The link blanks the field: the base's again.
        cards.restore("tags")
        self.assertEqual(cards.vars["tags"].get(), "")
        self.assertTrue(cards.apply())
        self.assertEqual(p.tags_of(added), (["god", "dragon"], True))
        self.assertNotIn("tags", manifest.build(p)["cards"][-1])
        # Undo puts the form back; Revert to retail takes them away.
        self.pause()
        cards.goto(1)
        cards.vars["tags"].set("fiend")
        self.assertTrue(cards.apply())
        self.pause()
        app.undo()
        app.update()
        self.assertEqual(app.project.tags_of(1), (["god", "dragon"], False))
        cards.goto(1)
        self.assertEqual(cards.vars["tags"].get(), "god, dragon")
        with mock.patch("fm_editor.tabs.messagebox.askyesno", return_value=True):
            cards.revert()
        self.assertEqual(app.project.tags_of(1), ([], False))
        self.assertEqual(cards.vars["tags"].get(), "")
        # Left untouched, what a mod wrote stays as written; validate says what the game leaves out.
        app.project.card_extra[2] = {"tags": ["", "x" * 32]}
        cards.goto(2)
        self.assertTrue(cards.apply())
        self.assertEqual(app.project.card_extra[2]["tags"], ["", "x" * 32])
        cards.vars["attack"].set("1230")
        self.assertTrue(cards.apply())
        self.assertIn("empty tag", cards.status.cget("text"))
        self.assertIn("over 31 letters", cards.status.cget("text"))

    def test_card_links_between_tabs(self):
        from fm_editor import card_links
        app, p = self.app, self.app.project
        # The card Cards shows is the one Art shows, and Fusions follows it
        # with that card's fusions only.
        app.cards.show_card(2)
        app.notebook.select(app.art)
        app.update()
        self.assertEqual(app.art.current, 2)
        app.notebook.select(app.fusions)
        app.update()
        self.assertEqual(app.fusions.search.get(), p.card_label(2))
        rows = [tuple(int(x) for x in iid.split(":")) for iid in app.fusions.rows()]
        self.assertIn((1, 2), rows)
        self.assertTrue(all(2 in pair or p.fusions.get(pair) == 2 for pair in rows))
        # A search of the modder's own is kept.
        app.fusions.search.set("zzz")
        app.fusions.fill()
        app.notebook.select(app.cards)
        app.cards.show_card(3)
        app.notebook.select(app.fusions)
        app.update()
        self.assertEqual(app.fusions.search.get(), "zzz")
        # Where it's used: the fusion making card 3, and a pool with it, each
        # going to its tab.
        lines = card_links.uses(app, 3)
        self.assertIn(("Fusions", "Material in"), {(w, t[:11]) for w, t, _ in lines})
        made = next(t for w, t, _ in lines if w == "Fusions")
        self.assertNotIn("made by 0 ", made)
        d, pool = next((d, pool) for d in range(1, len(p.pools)) for pool in ("deck", "pow", "bcd", "tec")
                       if p.pools[d][pool].get(3))
        go = next(g for w, t, g in lines if w == "Duelists")
        app.open_pool(d, pool, 3)
        app.update()
        self.assertIs(app.notebook.current(), app.duelists)
        self.assertEqual((app.duelists.duelist, app.duelists.pool.get()), (d, pool))
        self.assertEqual(app.duelists.tree.selection(), ("3",))
        go()
        app.update()
        self.assertIs(app.notebook.current(), app.duelists)
        window = card_links.UsesWindow(app, 3)
        self.assertEqual(len(window.tree.get_children()), len(lines))
        window.destroy()
        # The right-click menu names Cards, Art and fusions, and opens them.
        menu = tk.Menu(app, tearoff=False)
        card_links.fill_menu(menu, app, app.duelists, 3)
        labels = [menu.entrycget(i, "label") for i in range(menu.index("end") + 1) if menu.type(i) == "command"]
        self.assertEqual(labels[:3], ["Open in Cards", "Open in Art", "Show its fusions"])
        menu.invoke(0)
        app.update()
        self.assertIs(app.notebook.current(), app.cards)
        self.assertEqual(app.cards.current, 3)
        menu.destroy()

    def test_card_opened_from_another_tab_wins(self):
        from fm_editor import card_links
        app, p = self.app, self.app.project

        def invoke(tab, cid, label):
            menu = tk.Menu(app, tearoff=False)
            card_links.fill_menu(menu, app, tab, cid)
            labels = [menu.entrycget(i, "label") if menu.type(i) == "command" else None
                      for i in range(menu.index("end") + 1)]
            menu.invoke(labels.index(label))
            app.update()
            menu.destroy()

        # The tab-changed event's follow() must not put back the Cards card.
        app.cards.show_card(2)
        app.notebook.select(app.duelists)
        app.update()
        invoke(app.duelists, 3, "Show its fusions")
        self.assertEqual(app.fusions.search.get(), p.card_label(3))
        equips = p.equip_cards()
        app.notebook.select(app.cards)
        app.cards.show_card(equips[0])
        app.notebook.select(app.duelists)
        app.update()
        invoke(app.duelists, equips[1], "Edit its equip targets")
        self.assertEqual(app.equips.current, equips[1])
        # After a refresh (Undo, another mod) a search naming a whole card
        # still follows the window's card.
        app.fusions.refresh()
        app.notebook.select(app.cards)
        app.cards.show_card(4)
        app.notebook.select(app.fusions)
        app.update()
        self.assertEqual(app.fusions.search.get(), p.card_label(4))

    def test_where_used_lists_pack_unlock_and_starter_pools(self):
        from fm_editor import card_links
        app, p = self.app, self.app.project
        p.packs.append({"name": "Locked", "cards": [1], "unlock": {"card": 5}})
        p.other["starter_pools"] = [{"name": "Mine", "draws": 4, "cards": {"5": 7}}]
        p.starter_pool_state = None
        lines = card_links.uses(app, 5)
        whats = {(w, t) for w, t, _ in lines}
        self.assertIn(("Packs", "Locked: unlocked by owning it"), whats)
        self.assertIn(("Starter pools", "Mine: weight 7"), whats)          # a link to the Weighted pools page
        window = card_links.UsesWindow(app, 5)
        row = next(i for i, (w, _, _) in enumerate(window.lines) if w == "Starter pools")
        window.tree.selection_set(str(row))
        window.go()     # a line with nowhere to go does nothing
        window.destroy()

    def test_recovered_type_change_restores_effect_controls(self):
        from fm_editor.gamedata import TYPE_NAMES
        app = self.app
        app.cards.search.set("no matching card")
        app.restore_drafts({"cards": {"current": 1, "vars": {
            "type": TYPE_NAMES[22], "effect": app.cards.effect_label(681)}}})
        app.update()
        self.assertEqual(app.cards.current, 1)
        self.assertEqual(app.cards.vars["type"].get(), TYPE_NAMES[22])
        self.assertEqual(app.cards.effect_box.winfo_manager(), "grid")
        self.assertEqual(app.cards.vars["effect"].get(), app.cards.effect_label(681))
        self.assertTrue(app.cards.apply(quiet=True))
        self.assertEqual(app.project.cards[1].type, 22)



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



class MonsterEffectsGuiTest(GuiCase):
    """The Cards tab's Monster effects box and the card text's right-click
    menu."""

    def test_effects_box(self):
        app, cards = self.app, self.app.cards
        cards.tree.selection_set("1")
        cards.select()
        box = cards.effects_box
        self.assertTrue(box.winfo_manager())
        self.assertEqual(box.tree.get_children(), ())
        box.effects.append({"when": "summon", "do": "heal", "amount": 500})
        box.effects.append({"when": "face_up", "do": "boost", "target": "others", "attack": 300})
        box.store()
        self.assertEqual(app.project.monster_effects_of(1)[0][1]["when"], "face_up")
        self.assertEqual([box.tree.set(i, "when") for i in box.tree.get_children()], ["On summon", "While face up"])
        box.tree.selection_set("1")
        box.move(-1)
        self.assertEqual([e["when"] for e in app.project.monster_effects_of(1)[0]], ["face_up", "summon"])
        box.tree.selection_set("0")
        box.remove()
        self.assertEqual(app.project.monster_effects_of(1)[0], [{"when": "summon", "do": "heal", "amount": 500}])
        self.assertTrue(app.dirty)
        # Kept across selecting another card; gone for a magic card.
        cards.tree.selection_set("2")
        cards.select()
        self.assertEqual(box.tree.get_children(), ())
        cards.tree.selection_set("1")
        cards.select()
        self.assertEqual(len(box.tree.get_children()), 1)
        cards.vars["type"].set("Magic")
        self.assertFalse(box.winfo_manager())

    def test_copy_of_a_magic_card_made_a_monster(self):
        from fm_editor import manifest, validate
        app, cards = self.app, self.app.cards
        p = app.project
        magic = next(cid for cid, card in sorted(p.cards.items()) if card.type == 20)
        cid = p.add_card(magic, "magic-monster")
        cards.fill()
        cards.show_card(cid)
        self.assertFalse(cards.effects_box.winfo_manager())         # a magic card: no monster effects
        cards.vars["type"].set("Dragon")
        self.assertTrue(cards.effects_box.winfo_manager())
        for key, value in (("attack", "1500"), ("defense", "1200"), ("level", "4"), ("star1", "Mars"),
                           ("star2", "Jupiter")):
            cards.vars[key].set(value)
        self.assertTrue(cards.apply())
        self.assertEqual((p.cards[cid].type, p.cards[cid].attack), (0, 1500))
        cards.effects_box.effects.append({"when": "summon", "do": "heal", "amount": 500})
        cards.effects_box.store()
        self.assertEqual([i.message for i in validate.validate_card(p, cid) if i.level == "error"], [])
        entry = next(e for e in manifest.build(p)["cards"] if e.get("id") == "magic-monster")
        self.assertEqual((entry["type"], entry["attack"], entry["monster_effects"][0]["do"]), ("Dragon", 1500, "heal"))

    def test_none_box_for_disc_cards(self):
        app, cards = self.app, self.app.cards
        cards.tree.selection_set("2")
        cards.select()
        box = cards.effects_box
        self.assertTrue(box.none_box.winfo_manager())
        box.none.set(True)
        box.store()
        self.assertEqual(app.project.card_extra[2]["monster_effects"], [])
        cards.tree.selection_set("1")
        cards.select()
        cards.tree.selection_set("2")
        cards.select()
        self.assertTrue(box.none.get())
        box.effects.append({"when": "summon", "do": "heal", "amount": 500})
        box.store()
        self.assertFalse(box.none_box.winfo_manager())       # a list of its own says it all
        box.tree.selection_set("0")
        box.none.set(False)
        box.remove()
        self.assertNotIn(2, app.project.card_extra)
        # An added card's empty list is always its own: no box.
        cid = app.project.add_card(1, "x")
        box.show(cid)
        self.assertFalse(box.none_box.winfo_manager())

    def test_effect_dialog(self):
        from fm_editor.monster_effects_ui import EffectDialog
        app = self.app
        dialog = EffectDialog(app, app.project, {"when": "combat", "do": "boost", "target": "battle",
                                                 "attack": -700}, str)
        self.assertEqual(dialog.vars["when"].get(), "Before combat")
        self.assertEqual(dialog.vars["target"].get(), "The monster it battles")
        # Combat offers no magic; face up only boosts.
        self.assertNotIn("Magic card effect", dialog.do_box.cget("values"))
        dialog.vars["when"].set("While face up")
        self.assertEqual(list(dialog.do_box.cget("values")), ["Boost ATK/DEF", "Cannot be destroyed by battle"])
        self.assertNotIn("The monster it battles", dialog.target_box.cget("values"))
        dialog.vars["type"].set("Dragon")
        dialog.ok()
        self.assertEqual(dialog.result, {"when": "face_up", "do": "boost", "target": "self", "attack": -700,
                                         "type": "Dragon"})

    def test_effect_dialog_for_each(self):
        from fm_editor import monster_effects as fx
        from fm_editor.monster_effects_ui import EffectDialog
        app = self.app
        dialog = EffectDialog(app, app.project, {"when": "summon", "do": "magic", "card": 337}, str)
        # Off by default, and not offered for a magic card's effect.
        self.assertEqual(dialog.vars["each"].get(), fx.NO_EACH)
        self.assertFalse(dialog.rows["each"][1].winfo_manager())
        dialog.vars["when"].set("While face up")
        self.assertTrue(dialog.rows["each"][1].winfo_manager())
        self.assertFalse(dialog.rows["each_type"][1].winfo_manager())
        dialog.vars["each"].set("On its owner's field")
        self.assertTrue(dialog.rows["each_type"][1].winfo_manager())
        dialog.vars["attack"].set("300")
        dialog.vars["defense"].set("300")
        dialog.vars["each_type"].set("Dragon")
        dialog.ok()
        self.assertEqual(dialog.result, {"when": "face_up", "do": "boost", "target": "self", "attack": 300,
                                         "defense": 300, "for_each": {"whose": "own", "type": "Dragon"}})
        # Opened again, it shows what it counts; "—" takes it away.
        dialog = EffectDialog(app, app.project, dialog.result, str)
        self.assertEqual((dialog.vars["each"].get(), dialog.vars["each_type"].get()),
                         ("On its owner's field", "Dragon"))
        dialog.vars["each"].set(fx.NO_EACH)
        dialog.ok()
        self.assertNotIn("for_each", dialog.result)
        # Changed to a destroy, the row goes and so does what it said.
        dialog = EffectDialog(app, app.project, {"when": "summon", "do": "heal", "amount": 200,
                                                 "for_each": {"whose": "all"}}, str)
        self.assertTrue(dialog.rows["each"][1].winfo_manager())
        dialog.vars["do"].set("Destroy monsters")
        self.assertFalse(dialog.rows["each"][1].winfo_manager())
        dialog.ok()
        self.assertEqual(dialog.result, {"when": "summon", "do": "destroy", "target": "opponent"})

    def test_condition_dialog(self):
        from fm_editor.monster_effects_ui import ConditionDialog
        from fm_editor import monster_effects as fx
        app = self.app
        # 1. Open with stat_threshold atk
        dialog = ConditionDialog(app, app.project, {"type": "stat_threshold", "stat": "atk", "comparison": ">=", "value": 1500}, str)
        self.assertEqual(dialog.vars["stat"].get(), "ATK")
        self.assertEqual(dialog.vars["value"].get(), "1500")
        dialog.ok()
        self.assertEqual(dialog.result, {"type": "stat_threshold", "stat": "atk", "comparison": ">=", "value": 1500})

        # 2. Switch to DEF
        dialog = ConditionDialog(app, app.project, {"type": "stat_threshold", "stat": "atk", "comparison": "<=", "value": 1500}, str)
        dialog.vars["stat"].set("DEF")
        dialog.vars["value"].set("2000")
        dialog.ok()
        self.assertEqual(dialog.result, {"type": "stat_threshold", "stat": "def", "comparison": "<=", "value": 2000})

        # 3. Invalid value validation
        dialog = ConditionDialog(app, app.project, {"type": "stat_threshold", "stat": "atk", "value": 1500}, str)
        dialog.vars["value"].set("-5")
        err = dialog.read(dialog)
        self.assertEqual(err, f"Threshold must be 0 to {fx.AMOUNT_MAX}.")

        # 4. Default reset is "In a turn"
        dialog = ConditionDialog(app, app.project, None, str)
        self.assertEqual(dialog.vars["reset"].get(), "In a turn")

        # 5. Attack conditions disallowed when allow_attack=False
        dialog = ConditionDialog(app, app.project, None, str, allow_attack=False)
        self.assertNotIn("The first time it is attacked", dialog.cond_labels)
        self.assertNotIn("first_attacked", dialog.cond_types)
        # Attempting to force an attack condition type fails validation
        dialog.vars["type"].set("The first time it is attacked")
        err = dialog.read(dialog)
        self.assertIsNotNone(err)

        # 6. Fewer and more monsters conditions
        dialog = ConditionDialog(app, app.project, None, str)
        dialog.vars["type"].set("If you control fewer monsters than opponent")
        dialog.ok()
        self.assertEqual(dialog.result, {"type": "fewer_monsters"})

        dialog = ConditionDialog(app, app.project, None, str)
        dialog.vars["type"].set("If you control more monsters than your opponent")
        dialog.ok()
        self.assertEqual(dialog.result, {"type": "more_monsters"})

        # 7. Control stat condition
        dialog = ConditionDialog(app, app.project, None, str)
        dialog.vars["type"].set(fx.CONDITION_LABELS[fx.CONDITIONS.index("control_stat")])
        dialog.vars["presence"].set(fx.PRESENCE_LABELS[1])
        dialog.vars["scope"].set(fx.SCOPE_LABELS[1])
        dialog.vars["stat"].set("DEF")
        dialog.vars["op"].set(fx.OP_LABELS[fx.OPS.index("<=")])
        dialog.vars["value"].set("1500")
        dialog.ok()
        self.assertEqual(dialog.result, {"type": "control_stat", "presence": "not_control", "scope": "other", "stat": "def", "comparison": "<=", "value": 1500})

        # 8. Control level condition
        dialog = ConditionDialog(app, app.project, None, str)
        dialog.vars["type"].set(fx.CONDITION_LABELS[fx.CONDITIONS.index("control_level")])
        dialog.vars["presence"].set(fx.PRESENCE_LABELS[0])
        dialog.vars["scope"].set(fx.SCOPE_LABELS[0])
        dialog.vars["op"].set(fx.OP_LABELS[fx.OPS.index(">=")])
        dialog.vars["level"].set("7")
        dialog.ok()
        self.assertEqual(dialog.result, {"type": "control_level", "comparison": ">=", "level": 7})

        # 9. Control type condition
        dialog = ConditionDialog(app, app.project, None, str)
        dialog.vars["type"].set(fx.CONDITION_LABELS[fx.CONDITIONS.index("control_type")])
        dialog.vars["presence"].set(fx.PRESENCE_LABELS[0])
        dialog.vars["type_mode"].set(fx.TYPE_MODE_LABELS[1])
        dialog.vars["card_type"].set("Dragon")
        dialog.ok()
        self.assertEqual(dialog.result, {"type": "control_type", "mode": "only", "card_type": "Dragon"})

        # 10. Control attribute condition
        dialog = ConditionDialog(app, app.project, None, str)
        dialog.vars["type"].set(fx.CONDITION_LABELS[fx.CONDITIONS.index("control_attribute")])
        dialog.vars["presence"].set(fx.PRESENCE_LABELS[1])
        dialog.vars["scope"].set(fx.SCOPE_LABELS[1])
        dialog.vars["attribute"].set("LIGHT")
        dialog.ok()
        self.assertEqual(dialog.result, {"type": "control_attribute", "presence": "not_control", "scope": "other", "attribute": "LIGHT"})

    def test_effect_dialog_modifiers(self):
        from fm_editor.monster_effects_ui import EffectDialog
        from fm_editor import monster_effects as fx
        app = self.app

        # 1. Battle immunity defaults to "Always" and "In a turn", modifier row shown, reset hidden
        dialog = EffectDialog(app, app.project, {"when": "face_up", "do": "battle_immunity"}, str)
        self.assertEqual(dialog.vars["modifier"].get(), "Always")
        self.assertEqual(dialog.vars["modifier_reset"].get(), "In a turn")
        self.assertTrue(dialog.rows["modifier"][1].winfo_manager())
        self.assertFalse(dialog.rows["modifier_reset"][1].winfo_manager())
        dialog.ok()
        self.assertEqual(dialog.result, {"when": "face_up", "do": "battle_immunity", "target": "self"})

        # 2. Select "The first time it is attacked" -> Resets dropdown appears, defaults to "In a turn"
        dialog = EffectDialog(app, app.project, {"when": "face_up", "do": "battle_immunity"}, str)
        dialog.vars["modifier"].set("The first time it is attacked")
        self.assertTrue(dialog.rows["modifier_reset"][1].winfo_manager())
        self.assertEqual(dialog.vars["modifier_reset"].get(), "In a turn")
        dialog.ok()
        self.assertEqual(dialog.result, {
            "when": "face_up",
            "do": "battle_immunity",
            "target": "self",
            "conditions": [{"type": "first_attacked", "reset": "turn"}]
        })

        # 3. Re-open effect with attack condition: cleanly unpacked into Modifiers and Resets, not in tree
        dialog = EffectDialog(app, app.project, dialog.result, str)
        self.assertEqual(dialog.vars["modifier"].get(), "The first time it is attacked")
        self.assertEqual(dialog.vars["modifier_reset"].get(), "In a turn")
        self.assertEqual(len(dialog.conditions), 0)
        # Change reset to "This card copy" and modifier to "Can only be destroyed after two attacks"
        dialog.vars["modifier"].set("Can only be destroyed after two attacks")
        dialog.vars["modifier_reset"].set("This card copy")
        dialog.ok()
        self.assertEqual(dialog.result, {
            "when": "face_up",
            "do": "battle_immunity",
            "target": "self",
            "conditions": [{"type": "destroyed_after_2_attacks", "reset": "copy"}]
        })

        # 4. Modifiers and reset are hidden for other effects like "boost"
        dialog = EffectDialog(app, app.project, {"when": "face_up", "do": "boost", "target": "self", "attack": 500}, str)
        self.assertFalse(dialog.rows["modifier"][1].winfo_manager())
        self.assertFalse(dialog.rows["modifier_reset"][1].winfo_manager())

        # 5. Enforcement: monster cannot have more than 1 attack condition across its effects
        existing_effect = {"when": "face_up", "do": "battle_immunity", "target": "self",
                           "conditions": [{"type": "first_attacked", "reset": "turn"}]}
        dialog = EffectDialog(app, app.project, None, str, effects=[existing_effect], index=None)
        dialog.vars["when"].set("While face up")
        dialog.vars["do"].set("Cannot be destroyed by battle")
        dialog.vars["modifier"].set("Can only be destroyed after two attacks")
        err = dialog.read(dialog)
        self.assertEqual(err, "A monster can only have one condition related to a number of attacks.")

    def test_text_menu(self):
        from fm_editor import text_menu
        cards = self.app.cards
        cards.tree.selection_set("1")
        cards.select()
        text = cards.text
        text.delete("1.0", "end")
        text.insert("1.0", "Can attack 2x a turn.")
        text.mark_set("insert", "1.0")
        text_menu.insert_code(text, "{f8 0B 00}")
        text.tag_add("sel", "1.21", "1.23")
        text_menu.color(text, 6)
        self.assertEqual(text.get("1.0", "end-1c"), "{f8 0B 00}Can attack {f8 0A 06}2x{f8 0A 00} a turn.")
        menu = tk.Menu(text, tearoff=False)
        text_menu.fill(menu, self.app, text, lambda: None)
        labels = [menu.entrycget(i, "label") for i in range(menu.index("end") + 1) if menu.type(i) != "separator"]
        self.assertEqual(labels, ["Cut", "Copy", "Paste", "Insert icon...", "Text color"])
        menu.destroy()
        # The picker: every icon, no taller than the screen, the icon going
        # where the cursor was when it opened.
        text.mark_set("insert", "end-1c")
        picker = text_menu.IconPicker(self.app, text, lambda: None, 0, 0)
        self.assertEqual(sorted(picker.buttons), list(range(41)))
        self.assertLessEqual(picker.winfo_reqheight(), picker.winfo_screenheight())
        text.mark_set("insert", "1.0")
        picker.pick(0x26)
        self.assertTrue(text.get("1.0", "end-1c").endswith("a turn.{f8 0B 26}"))
        self.assertFalse(picker.winfo_exists())

    def test_effect_monster_swatch_is_orange(self):
        from fm_editor.tabs import FRAME_COLORS
        cards = self.app.cards
        cards.tree.selection_set("1")
        cards.select()
        self.assertEqual(cards.swatch.cget("background"), FRAME_COLORS[0])
        cards.effects_box.effects.append({"when": "summon", "do": "heal", "amount": 500})
        cards.effects_box.store()
        self.assertEqual(cards.swatch.cget("background"), FRAME_COLORS[5])


class CardTextBoxTest(GuiCase):
    """The card text box shows codes as pictures and gives them back."""

    def test_codes_shown_and_kept(self):
        cards = self.app.cards
        cards.tree.selection_set("1")
        cards.select()
        box = cards.text
        # Game files with the Dragon icon (the fixture's have none).
        from fm_editor.tests.test_card_text import icon_wa
        with mock.patch.object(self.app.files, "wa", icon_wa()):
            self.assertTrue(box._pictures())
        box.delete("1.0", "end")
        text = "{f8 0A 05}<Effect>{f8 0A 00} {f8 0B 00}x"
        box.insert("1.0", text)
        self.assertEqual(box.get("1.0", "end-1c"), text)
        self.assertEqual(len(box.image_names()), 3)
        self.assertIn("color5", box.tag_names("1.2"))
        # Typed by hand (Tcl's own insert, as a key does): a picture once whole.
        box.tk.call(box._w, "insert", "end", " {f8 0b 00}")
        self.assertTrue(box.bind("<KeyRelease>"))      # a key's release runs it (the window is withdrawn here)
        box.layout()
        self.assertEqual(box.get("1.0", "end-1c"), text + " {f8 0B 00}")
        self.assertEqual(len(box.image_names()), 4)
        # The clipboard carries the codes.
        box.tag_add("sel", "1.0", "end-1c")
        box.event_generate("<<Copy>>")
        self.assertEqual(box.clipboard_get(), text + " {f8 0B 00}")
        box.delete("1.0", "end")
        box.event_generate("<<Paste>>")
        self.assertEqual(box.get("1.0", "end-1c"), text + " {f8 0B 00}")
        self.assertEqual(len(box.image_names()), 4)
        # Lines broken where the game breaks them: the space shows as a line's
        # end and reads back as the space.
        long = "aaaaaaaaaaaaaaa {f8 0B 00}{f8 0B 00} b"
        box.delete("1.0", "end")
        box.insert("1.0", long)
        self.assertEqual(box.get("1.0", "end-1c"), long)
        self.assertEqual(box.index("end-1c").split(".")[0], "2")
        box.delete("1.0", "end")
        box.insert("1.0", text + " {f8 0B 00}")
        # Applied as written.
        self.assertTrue(cards.apply())
        self.assertEqual(self.app.project.cards[1].description, text + " {f8 0B 00}")


class ColumnWidthTest(GuiCase):
    def test_a_dragged_column_keeps_its_width(self):
        from types import SimpleNamespace
        from fm_editor import widgets
        app = self.app
        app.deiconify()
        app.geometry("1400x800")
        app.update()
        tree = app.cards.tree
        x = next(x for x in range(tree.winfo_width()) if tree.identify_region(x, 10) == "separator")
        widgets._free_columns(tree, SimpleNamespace(x=x, y=10))
        columns = list(tree["columns"])
        self.assertEqual([tree.column(c, "stretch") for c in columns],
                         [False] * (len(columns) - 1) + [True])
        # Narrower than it stretched to: it stays so (it took the width back before).
        tree.column("name", width=150)
        app.update()
        self.assertEqual(tree.column("name", "width"), 150)
        app.withdraw()

    def test_horizontal_scrolled_tree(self):
        from fm_editor import widgets
        from tkinter import ttk
        container = ttk.Frame(self.app)
        frame, tree = widgets.scrolled_tree(container, [("a", "Col A"), ("b", "Col B")], [150, 400], 4, horizontal=True)
        self.assertIsNotNone(tree)
        self.assertIsNotNone(frame)
        self.assertFalse(tree.column("a", "stretch"))
        self.assertFalse(tree.column("b", "stretch"))
        scrollbars = [w for w in frame.winfo_children() if isinstance(w, ttk.Scrollbar)]
        self.assertEqual(len(scrollbars), 2)
        container.destroy()


class CardViewPreviewTest(GuiCase):
    def test_follows_the_form(self):
        cards = self.app.cards
        cards.tree.selection_set("1")
        cards.select()
        values = cards.card_view_values()
        self.assertEqual(values[3], self.app.project.cards[1].description)
        cards.vars["type"].set("Magic")
        self.assertEqual(cards.card_view_values()[1:3], (0, 0))      # no stars on a magic card
        cards.card_view.draw()      # the fixture's files: drawn or explained, never an error
        self.assertTrue(cards.card_view.picture.cget("image") or cards.card_view.note.cget("text") is not None)


class IconChoiceTest(GuiCase):
    def test_type_and_star_lists(self):
        cards = self.app.cards
        cards.tree.selection_set("1")
        cards.select()
        star = cards.star_boxes[0]
        self.assertEqual(star.cget("values")[0], "(none)")
        star.configure(values=("(none)", "Mars"))
        self.assertEqual(star.cget("values"), ("(none)", "Mars"))
        star._fill()
        self.assertEqual(star.menu.index("end"), 1)
        star.menu.invoke(1)
        self.assertEqual(cards.vars["star1"].get(), "Mars")
        self.assertEqual(cards.star_icon("Mars"), 0x18)
        self.assertIsNone(cards.star_icon("(none)"))


class CardsPanesTest(GuiCase):
    def test_list_and_form_share_the_width(self):
        app, cards = self.app, self.app.cards
        app.deiconify()
        app.geometry("1600x960")
        cards.tree.selection_set("1")
        cards.select()
        for _ in range(5):          # the window takes its size over a few rounds
            app.update()
            cards.place_sash()
        app.update()
        scroll = cards.card_scroll
        self.assertGreaterEqual(scroll.canvas.winfo_width(), scroll.body.winfo_reqwidth())   # all of the form
        self.assertFalse(scroll.xbar.winfo_manager())
        # Dragged right, the form scrolls across.
        cards._sash_dragged = True
        cards.panes.sashpos(0, cards.panes.winfo_width() - 300)
        for _ in range(3):
            app.update()
        self.assertTrue(scroll.xbar.winfo_manager())
        app.withdraw()


class ZoomTest(unittest.TestCase):
    def setUp(self):
        if tk is None:          # zoom.py sizes Tk's fonts: no Tk, no zoom
            raise unittest.SkipTest("this Python has no Tk")

    def test_fit_and_sizes(self):
        from fm_editor import zoom
        self.assertEqual(zoom.fit_factor(1600, 960, 1.0), 1.0)
        self.assertEqual(zoom.fit_factor(1000, 600, 1.0), 1.0)          # never below 1
        self.assertEqual(zoom.fit_factor(3840, 2100, 1.0), 2.125)       # the smaller stretch, in eighths
        self.assertEqual(zoom.fit_factor(3840, 2100, 1.5), 1.375)       # over the desktop's own scale
        self.assertEqual(zoom.fit_factor(20000, 20000, 1.0), zoom.MOST)
        self.assertEqual(zoom.font_size(10, 1.25), 13)
        self.assertEqual(zoom.font_size(-12, 1.5), -18)                 # pixels stay pixels
        self.assertEqual([zoom.parse(v) for v in ("fit", 150, "150", 140, None)],
                         [zoom.FIT, 150, 150, zoom.FIT, zoom.FIT])


class InterfaceSizeTest(GuiCase):
    def test_a_size_grows_text_lists_and_pictures(self):
        import tkinter.font as tkfont
        from fm_editor import settings, widgets
        app = self.app
        font = tkfont.nametofont("TkDefaultFont", root=app)
        size, row = font.cget("size"), app.theme.row_height
        widths = dict(app.cards.tree.widths)
        heading = tkfont.nametofont(widgets.ui_font(11), root=app).cget("size")
        app.zoom.choose(200)
        self.assertEqual(settings.load()["ui_size"], 200)
        self.assertEqual(font.cget("size"), zoom_size(size, 2))
        self.assertEqual(tkfont.nametofont(widgets.ui_font(11), root=app).cget("size"), zoom_size(heading, 2))
        self.assertGreater(app.theme.row_height, row)
        self.assertEqual(widgets.px(app, 100), round(100 * app.zoom.desktop * 2))
        self.assertEqual(app.cards.tree.widths, {key: width * 2 for key, width in widths.items()})
        # A font made at 200% goes back with the others.
        made = tkfont.nametofont(widgets.fixed_font(13), root=app)
        self.assertEqual(made.cget("size"), 26)
        app.zoom.step(-1)
        self.assertEqual(app.zoom.factor, 1.75)
        app.zoom.choose(100)
        self.assertEqual(font.cget("size"), size)
        self.assertEqual(made.cget("size"), 13)
        self.assertEqual(app.cards.tree.widths, widths)

    def test_fit_to_window_follows_the_window(self):
        app = self.app
        app.zoom.choose("fit")
        with mock.patch.object(app, "winfo_ismapped", return_value=True), \
                mock.patch.object(app, "winfo_width", return_value=round(3200 * app.zoom.desktop)), \
                mock.patch.object(app, "winfo_height", return_value=round(1920 * app.zoom.desktop)):
            app.zoom.update()
        self.assertEqual(app.zoom.factor, 2.0)
        self.assertIn("200%", app.zoom.menu.entrycget(0, "label"))

    def test_restored_window_gives_the_card_form_its_room(self):
        """Maximized, then restored: the Cards list went back to its width
        (a Panedwindow asked for the maximized width once the fonts changed)."""
        import time
        app = self.app
        app.deiconify()

        def size(geometry):
            app.geometry(geometry)
            end = time.monotonic() + 0.5       # Fit to window's wait for a drag to settle, too
            while time.monotonic() < end:
                app.update()
                time.sleep(0.01)

        app.notebook.select(app.cards)
        size("1400x850")
        page, panes = app.cards.page, app.cards.panes
        size("3600x2000")
        from fm_editor.zoom import fit_factor
        if fit_factor(app.winfo_width(), app.winfo_height(), app.zoom.desktop) <= 1.0:
            # Windows keeps a window within the screen (1024x768 on CI); two
            # screens side by side give the width but not the height, which
            # Fit to window needs as much.
            self.skipTest("the screen is too small for a window this big")
        self.assertGreater(app.zoom.factor, 1.0)
        app.notebook.select(app.art)        # other tabs on the way, as a user goes
        size("3600x2000")
        app.notebook.select(app.cards)
        size("1400x850")
        self.assertEqual(app.zoom.factor, 1.0)
        self.assertFalse(page.xbar.winfo_ismapped())
        self.assertLessEqual(panes.winfo_width(), page.canvas.winfo_width())
        scroll = app.cards.card_scroll           # the form as wide as it asks
        self.assertGreaterEqual(scroll.canvas.winfo_width(), scroll.body.winfo_reqwidth())
        # Restored while another tab shows: the line is placed when it shows.
        size("3600x2000")
        app.notebook.select(app.art)
        size("1400x850")
        app.notebook.select(app.cards)
        size("1400x850")
        self.assertFalse(page.xbar.winfo_ismapped())
        self.assertGreaterEqual(scroll.canvas.winfo_width(), scroll.body.winfo_reqwidth())
        app.withdraw()

    def test_art_view_switch(self):
        """One picture a part in the chosen view; the disc's beside it once
        the mod changes it; the choice remembered."""
        from fm_editor import pngio, settings
        from fm_editor.tests.test_art import gradient
        app, tab = self.app, self.app.art
        app.notebook.select(tab)
        tab.goto(2)

        def captions(part):
            return [c.cget("text") for c in tab.rows[part]["captions"] if c.winfo_manager()]

        self.assertEqual(tab.view.get(), "game")
        self.assertEqual(captions("art"), ["In game (1x)"])
        picture = Path(self.tmp.name) / "view.png"
        pngio.write(picture, gradient(204, 192))
        self.assertTrue(tab.use_file("art", str(picture)))
        self.assertEqual(captions("art"), ["Disc (before)", "In game (1x)"])
        tab.view.set("internal")
        tab.view_changed()
        self.assertEqual(captions("art"), ["Disc (before)", "Internal 2x"])
        self.assertEqual(captions("title"), ["In game (1x)"])      # a plate has no internal view
        self.assertEqual(settings.load()["art_view"], "internal")
        tab.view.set("disc")
        tab.view_changed()
        self.assertEqual(captions("art"), ["Disc"])

    def test_art_pictures_grow_with_the_room(self):
        """The pictures take the room the page has, in eighths, at any
        window size, and never push the page into scrolling sideways."""
        import time
        app = self.app
        app.deiconify()

        def size(geometry):
            app.geometry(geometry)
            end = time.monotonic() + 0.5       # Fit to window's wait for a drag, the tab's fit
            while time.monotonic() < end:
                app.update()
                time.sleep(0.01)

        app.notebook.select(app.art)
        app.art.goto(2)
        shares = []
        for geometry in ("1900x1050", "3800x2100", "1900x1050"):
            size(geometry)
            wide, tall = (int(n) for n in geometry.split("x"))
            if app.winfo_width() < wide - 40 or app.winfo_height() < tall - 40:
                # Windows keeps a window within the screen (1024x768 on CI); two
                # screens side by side give the width but not the height.
                self.skipTest("the screen is too small for a window this big")
            k = app.art.k
            self.assertEqual(k * 8, int(k * 8))
            widths = {image.width() for (part, _), image in app.art.photos.items() if part == "art"}
            self.assertEqual(widths, {round(102 * 2 * k)})       # the three views alike
            self.assertFalse(app.art.page.xbar.winfo_ismapped())
            shares.append(round(102 * 2 * k) / app.winfo_width())
        self.assertGreater(shares[1], shares[0] * 0.8)           # about the same share, maximized or not
        self.assertEqual(shares[2], shares[0])                   # and back as it was
        app.withdraw()

def zoom_size(size, factor):
    from fm_editor.zoom import font_size
    return font_size(size, factor)


if __name__ == "__main__":
    unittest.main()
