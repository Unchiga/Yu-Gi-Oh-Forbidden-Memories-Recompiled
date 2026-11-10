"""What mods change in common (overlaps.py): the same lines as the game's
Mods window (src/pc/mods/overlap.c) for the mods in tests/pc/mod_overlaps,
the load order the game would give them, and the Conflicts tab's check of
the mod being edited against the other installed mods.

    python -m unittest discover -s tools/pc/fm_editor/tests -t tools/pc
"""
import json
import os
import shutil
import tempfile
import unittest
from pathlib import Path

from fm_editor import manifest, overlaps, validate
from fm_editor.model import Project
from fm_editor.tests.test_data import fixture

FIXTURE = Path(__file__).resolve().parents[4] / "tests" / "pc" / "mod_overlaps"
# What tests/pc/mod_overlaps/duelists-64 makes (tests/pc/mods_overlap_test.c too).
DUELISTS_64 = "Heishin's POW drops (B, C): C's \"replace\" clears what the earlier mods set"


class FixtureSource(overlaps.Source):
    """The cards and opponents tests/pc/mods_overlap_test.c's resolver knows."""

    def __init__(self, setup):
        self.cards = {int(k): v for k, v in setup["cards"].items()}
        self.types = {int(k): v for k, v in setup["types"].items()}
        self.bases = {int(k): v for k, v in setup.get("bases", {}).items()}
        self.duelists = setup["duelists"]

    def card_info(self, cid):
        if cid not in self.types:
            return None
        return self.bases.get(cid, cid), self.types[cid][0], self.types[cid][1]

    def card(self, text, number):
        for cid, name in self.cards.items():
            if text is None and number == cid:
                return cid
            if text is not None and (overlaps.letters(text) == overlaps.letters(name) or text == str(cid)):
                return cid
        return -1

    def card_name(self, cid):
        return self.cards.get(cid)

    def duelist(self, text):
        for name, did in self.duelists.items():
            if overlaps.letters(text) == overlaps.letters(name) or (overlaps.is_digits(text) and int(text) == did):
                return did
        return -1


def fixture_mods():
    setup = json.loads((FIXTURE / "fixture.json").read_text(encoding="utf-8"))
    mods = []
    for mid in setup["order"]:
        manifest = overlaps._read_json(FIXTURE / mid / "mod.json")   # as json.c reads it (delta's needs it)
        mods.append(overlaps.Mod(mid, manifest["name"], manifest, FIXTURE / mid))
    return setup, mods


class SameAsTheGame(unittest.TestCase):
    def test_the_fixture_lines(self):
        # The lines and their texts, in the order the Mods window lists them.
        setup, mods = fixture_mods()
        found = overlaps.check(mods, FixtureSource(setup))
        expected = [row for row in (FIXTURE / "expected.txt").read_text(encoding="utf-8").splitlines() if row]
        self.assertEqual([f"{overlaps.line(o)}\t{o.text}" for o in found], expected)

    def test_involving_one_mod(self):
        # The editor's check works out only the overlaps of the mod it edits.
        setup, mods = fixture_mods()
        every = [o.text for o in overlaps.check(mods, FixtureSource(setup)) if "Gamma" in o.text.split(": ")[0]]
        only = [o.text for o in overlaps.check(mods, FixtureSource(setup), involving=2)]
        self.assertEqual(only, every)

    def test_the_texts(self):
        setup, mods = fixture_mods()
        texts = {o.label: o.text for o in overlaps.check(mods, FixtureSource(setup))}
        self.assertEqual(texts["Price of 'Mystical Elf'"],
                         "Price of 'Mystical Elf' (Alpha, Beta): Beta wins (later in load order, through its \"all\")")
        self.assertEqual(texts["menu.spacing"],
                         "menu.spacing (Beta, Gamma): Gamma wins (it loads after Beta on purpose: after/requires)")
        self.assertEqual(texts["Card 'Blue-eyes White Dragon'"],
                         "Card 'Blue-eyes White Dragon' (Alpha, Beta): Beta's description is used (later in load order); "
                         "the rest combines")

    def test_wrong_types_are_nothing(self):
        # A list where an object belongs, a number where a list does: the game
        # notes them and reads on, so a player has such a mod installed, and
        # the check (which runs as any mod is opened) must read on too.
        wrong = [
            {"limits": [1], "title": [{}], "menu": [[1]], "terrain_bonus": [1], "decks": [{}],
             "drops": [{"pow": {}}], "passwords": [1], "trap_thresholds": [2], "audio": {"music": ["a"]}},
            {"cards": 5, "fusions": 5, "equips": 5, "rituals": 5, "data": 5, "textures": 5,
             "guardian_stars": {"stars": 5, "matchups": 5}, "menu": {"buttons": [[1], 5, {"id": 5}]}, "text": 5,
             "settings": 5, "after": 5},
            {"limits": {"life_points": {"duelists": [1]}}, "guardian_stars": {"stars": [{"id": 11, "beats": 5}]},
             "drops": {"all": [1]}, "decks": {"all": 5}, "terrain_bonus": {"Forest": [1]}}]
        for a in wrong:
            for b in wrong:
                mods = [overlaps.Mod("w0", "w0", dict(a, id="w0")), overlaps.Mod("w1", "w1", dict(b, id="w1"))]
                for found in overlaps.check(mods):
                    self.assertTrue(found.text)
                overlaps.load_order(mods)

    def test_cards_in_load_order(self):
        # The cards follow the load order (mods.c Mods_VisitCards); a later
        # replace that gives no plate drops the earlier's, as it does the name.
        a = overlaps.Mod("a", "A", {"cards": [{"replace": 1, "attack": 100, "title": "A"}]})
        b = overlaps.Mod("b", "B", {"cards": [{"replace": 1, "attack": 200}]})
        found = overlaps.check([a, b])
        self.assertEqual(found[0].text, "Card #1 (A, B): B's attack is used and the title A gave is dropped, as B's "
                                        "replace starts it from the disc's (later in load order); the rest combines")

    def test_pack_password_beside_one_mods_card(self):
        # One mod's pack and another's card password, and no second pack:
        # the digits give the card (pack_shop.c check_passwords).
        a = overlaps.Mod("a", "A", {"passwords": {"Kuriboh": {"password": "87654321"}}})
        b = overlaps.Mod("b", "B", {"packs": [{"name": "B", "cards": [1], "password": "87654321"}]})
        found = overlaps.check([a, b])
        self.assertEqual([o.text for o in found], ["Password 87654321 (A, B): the digits give A's card 'Kuriboh', "
                                                   "not the others' card or pack"])

    def test_duelists_64(self):
        # 64 duelists in one mod's folder, then one of its list without an
        # "id", then another mod's duelist whose pool file a third mod meets
        # (the C engine once lost or freed its list here).
        setup, _ = fixture_mods()
        folder = FIXTURE / "duelists-64"
        mods = [overlaps.Mod(m, m.upper(), overlaps._read_json(folder / m / "mod.json"), folder / m)
                for i, m in enumerate(("a", "b", "c"))]
        found = overlaps.check(mods, FixtureSource(setup))
        self.assertEqual([o.text for o in found], [DUELISTS_64])

    def test_ritual_tributes_from(self):
        # Where a ritual's tributes come from is part of its recipe; "field"
        # written out is the default (mods_overlap_test.c too).
        setup, _ = fixture_mods()
        entry = {"card": "Black Luster Ritual", "tributes": ["Kuriboh"], "result": "Black Luster Soldier"}
        plain = overlaps.Mod("r1", "r1", {"id": "r1", "rituals": [entry]}, FIXTURE)
        for origin, outcome in (("field", "agree"), ("hand", "later")):
            other = overlaps.Mod("r2", "r2", {"id": "r2", "rituals": [dict(entry, tributes_from=origin)]}, FIXTURE)
            found = overlaps.check([plain, other], FixtureSource(setup))
            self.assertEqual([o.outcome for o in found], [outcome], origin)

    def test_starter_pools(self):
        # tests/pc/mod_overlaps/starter-pools: each "# a, b" a set of its mods
        # in load order, then the lines it makes (mods_overlap_test.c too).
        # Pools add up and deal only at forty draws; a written deck wins.
        setup, _ = fixture_mods()
        folder = FIXTURE / "starter-pools"
        sets = []
        for row in (folder / "expected.txt").read_text(encoding="utf-8").splitlines():
            if row.startswith("# "):
                sets.append((row[2:].split(", "), []))
            elif row:
                sets[-1][1].append(row)
        self.assertGreaterEqual(len(sets), 8)
        for ids, expected in sets:
            mods = []
            for mid in ids:
                manifest = overlaps._read_json(folder / mid / "mod.json")
                mods.append(overlaps.Mod(mid, manifest["name"], manifest, folder / mid))
            found = overlaps.check(mods, FixtureSource(setup))
            self.assertEqual([f"{overlaps.line(o)}\t{o.text}" for o in found], expected, ids)
            # The editor's check of one mod: a line one mod makes alone is its.
            for m in range(len(mods)):
                mine = [o.text for o in overlaps.check(mods, FixtureSource(setup), involving=m)]
                self.assertEqual(mine, [row.split("\t")[1] for row in expected
                                        if mods[m].id in row.split("|")[3].split(", ")], (ids, m))

    def test_names_as_the_game_keeps_them(self):
        # mods_overlap_test.c's: an escaped letter (its UTF-8), and six
        # names of 95 bytes, each cut where a letter starts, all in the line.
        escaped = overlaps.parse('"' + chr(92) + 'u00c9clair"')
        names = [escaped] + [f"{m}{0:093d}\u00f1tail" for m in range(1, 6)]
        mods = [overlaps.Mod(f"m{m}", names[m], {"font": "f.ttf"}) for m in range(6)]
        found = overlaps.check(mods[:2])
        self.assertEqual(found[0].text, f"Fonts (\u00c9clair, 1{0:093d}): both apply and add up "
                                        "(a letter comes from the first font that has it)")
        found = overlaps.check(mods[1:])
        self.assertIn(f", 5{0:093d}): all apply", found[0].text)

    def test_read_as_json_c_reads(self):
        b = chr(92)
        self.assertEqual(overlaps.parse('{"a": 1, "a": 2,}').pairs, [("a", 1), ("a", 2)])
        self.assertEqual(overlaps.parse('{"a": 1, "a": 2}')["a"], 1)
        self.assertEqual(overlaps.parse('\v[1e3, 2.50e1, -0]\f'), [1000, 25, 0])
        for refused in ('[,]', '{"a":,}', '[1,,]', '2.5000000000000001e1', '1e-1', '2147483648', 'NaN', '[01.5]',
                        '[0x]', '[0x1G]', '[0x1.5]', '[0x80000000]', '[- 0x1]', '[1 / 2]', '[1] /', '[1] /* never ends',
                        '[1, /* */ 2 /*', '[1 // no end of the list'):
            self.assertIsNone(overlaps.parse(refused), refused)
        # Hexadecimal after 0x, leading zeros as decimal, and comments, as
        # json.c reads a hand-written mod.json; not inside a string.
        self.assertEqual(overlaps.parse('[010, 08, 00, -007, 010e1, 0x10, -0x10, 0X1f, 0x7FFFFFFF, -0x80000000]'),
                         [10, 8, 0, -7, 100, 16, -16, 31, 2 ** 31 - 1, -2 ** 31])
        value = overlaps.parse('/* head */ { // a "z": [\n "a": /* x */ 1, // b,\n "b" /* c */ : [1, /* 2, */ 3 // ]\n'
                               ' ], "c": "// not /* this */", /**/ "d": 0x1F, /***/ } // end')
        self.assertEqual(value.pairs, [("a", 1), ("b", [1, 3]), ("c", "// not /* this */"), ("d", 31)])
        self.assertEqual(overlaps.parse('[1, 2 // a trailing comma after this\n , ]'), [1, 2])
        # A \u escape is its character (json.c writes it as UTF-8), a
        # surrogate pair one, a lone surrogate U+FFFD; a NUL ends the string.
        self.assertEqual(overlaps.parse(f'"{b}u00c3{b}u00a9 {b}u4e2d a{b}u0000b"'), "\u00c3\u00a9 \u4e2d a")
        self.assertEqual(overlaps.parse(f'"{b}ud83d{b}ude00 {b}ud800x"'), "\U0001f600 \ufffdx")
        self.assertEqual(overlaps.parse(f'"{b}{b}u0041"'), f"{b}u0041")

    def test_one_mod_is_nothing(self):
        setup, mods = fixture_mods()
        self.assertEqual(overlaps.check(mods[:1], FixtureSource(setup)), [])

    def test_load_order(self):
        mods = fixture_mods()[1][:3]   # alpha, beta, gamma
        # Gamma names Beta in "after": however low its rank, it loads after it.
        order = overlaps.load_order(list(reversed(mods)), {"mod.gamma.order": -5})
        self.assertEqual([m.id for m in order], ["alpha", "beta", "gamma"])
        order = overlaps.load_order(mods, {"mod.alpha.order": 10})
        self.assertEqual([m.id for m in order], ["beta", "gamma", "alpha"])


class ConflictsTab(unittest.TestCase):
    """validate.cross_mod: the mod being edited against the installed ones."""

    def setUp(self):
        self.folder = Path(tempfile.mkdtemp())
        for mid in ("alpha", "beta"):
            shutil.copytree(FIXTURE / mid, self.folder / mid)

    def tearDown(self):
        shutil.rmtree(self.folder, ignore_errors=True)

    def test_against_installed_mods(self):
        project = Project(fixture().game())
        project.info.id = "mine"
        project.info.name = "Mine"
        project.other["priority"] = 5
        project.other["limits"] = {"stats": 12000}
        (self.folder / "settings.txt").write_text("mod.alpha=1\nmod.beta=0\n", encoding="utf-8")
        issues, summary = validate.cross_mod(project, [self.folder], self.folder / "settings.txt")
        stats = [i for i in issues if i.where.startswith("Value stats")]
        self.assertEqual(len(stats), 1)
        self.assertEqual(stats[0].level, "warning")
        self.assertIn("Mine wins (later in load order)", stats[0].message)
        self.assertIn("2 installed mods", summary)
        self.assertIn("beta is off in the game now", summary)

    def test_two_card_drops_locks(self):
        # "limits" "card_drops": two mods' locks meet, and the later one
        # deals (tables.c read_values notes the earlier one in the game).
        (self.folder / "gamma").mkdir()
        (self.folder / "gamma" / "mod.json").write_text(
            json.dumps({"id": "gamma", "name": "Gamma", "limits": {"card_drops": 7}}), encoding="utf-8")
        project = Project(fixture().game())
        project.info.id = "mine"
        project.info.name = "Mine"
        project.other["priority"] = 5
        project.other["limits"] = {"card_drops": 5}
        (self.folder / "settings.txt").write_text("mod.alpha=0\nmod.beta=0\nmod.gamma=1\n", encoding="utf-8")
        issues, _ = validate.cross_mod(project, [self.folder], self.folder / "settings.txt")
        drops = [i for i in issues if i.where.startswith("Value card_drops")]
        self.assertEqual(len(drops), 1)
        self.assertIn("Gamma", drops[0].message)
        self.assertIn("Mine wins (later in load order)", drops[0].message)

    def test_renamed_mod_is_itself(self):
        # Opened from alpha/ and given another id: the folder it came from is
        # still this mod, not another installed one meeting it everywhere.
        project, _ = manifest.open_mod(fixture().game(), self.folder / "alpha")
        project.info.id = "renamed"
        issues, summary = validate.cross_mod(project, [self.folder], False)
        self.assertIn("Checked against 1 installed mods", summary)
        self.assertFalse(any("renamed" in i.message and "Alpha" in i.message and "Beta" not in i.message
                             for i in issues))

    def test_cycle_is_left_out(self):
        a = overlaps.Mod("a", "A", {"after": ["b"]})
        b = overlaps.Mod("b", "B", {"after": ["a"]})
        c = overlaps.Mod("c", "C", {"requires": ["missing"]})
        d = overlaps.Mod("d", "D", {})
        self.assertEqual([m.id for m in overlaps.load_order([a, b, c, d])], ["d"])

    def test_broken_mods_keep_their_place(self):
        # mods.c: a mod.json that is not an object, or an id that is not one,
        # is listed under its folder's name (a player's copy over a shipped
        # one) and never loaded; nor is a mod requiring it, and so on.
        shipped, user = self.folder / "shipped", self.folder / "user"
        manifests = {shipped / "x": '{"id": "x", "name": "X"}', user / "x": "[1]",
                     user / "y": '{"id": "not an id!"}', user / "z": '{"id": "z", "requires": ["y"]}',
                     user / "w": '{"id": "w", "requires": ["z"]}', user / "v": '{"id": "v"}'}
        for folder, text in manifests.items():
            folder.mkdir(parents=True)
            (folder / "mod.json").write_text(text, encoding="utf-8")
        every = overlaps.installed([shipped, user])
        self.assertEqual([(m.id, m.broken) for m in every],
                         [("x", True), ("v", False), ("w", False), ("y", True), ("z", False)])
        self.assertEqual([m.id for m in overlaps.load_order(every)], ["v"])

    def test_card_named_as_the_game_names_it(self):
        # The last replace in load order names the card; one without a name
        # leaves the disc's, whatever the edited mod calls it.
        project = Project(fixture().game())
        cid = next(iter(project.retail.cards))
        retail = project.retail.cards[cid].name
        project.cards[cid].name = "Mine"
        a = overlaps.Mod("a", "A", {"cards": [{"replace": cid, "name": "Red"}]})
        b = overlaps.Mod("b", "B", {"cards": [{"replace": cid, "attack": 5}]})
        self.assertEqual(validate._ProjectSource(project, {}, [a, b]).card_name(cid), retail)
        self.assertEqual(validate._ProjectSource(project, {}, [b, a]).card_name(cid), "Red")

    def test_applied(self):
        mod = overlaps.Mod("x-y", "X", {"legacy_setting": "old_x"})
        self.assertFalse(validate.applied(mod, {}))
        self.assertTrue(validate.applied(mod, {"old_x": 1}))
        self.assertFalse(validate.applied(mod, {"old_x": 1, "mod.x-y": 0}))
        os.environ["MEMORIES_MOD_X_Y"] = "1"
        try:
            self.assertTrue(validate.applied(mod, {"mod.x-y": 0}))
        finally:
            del os.environ["MEMORIES_MOD_X_Y"]

    def test_no_other_mods(self):
        project = Project(fixture().game())
        issues, summary = validate.cross_mod(project, [self.folder / "nothing"], None)
        self.assertEqual(issues, [])
        self.assertIn("no other installed mods", summary)


if __name__ == "__main__":
    unittest.main()
