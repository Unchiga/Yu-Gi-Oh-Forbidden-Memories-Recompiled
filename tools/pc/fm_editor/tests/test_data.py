"""The data layer on synthetic files: reading the tables, the diff to
mod.json, reading it back, and the port's pool arithmetic.

    python -m unittest discover -s tools/pc/fm_editor/tests -t tools/pc
"""
import json
import tempfile
import unittest
from unittest import mock
from pathlib import Path

from fm_editor import disc, gamedata as g, manifest, pools, validate
from fm_editor.model import Project, card_matches
from fm_editor.tests.fixtures import Fixture, make_iso

FIXTURE = None


def fixture() -> Fixture:
    global FIXTURE
    if FIXTURE is None:
        FIXTURE = Fixture()
    return FIXTURE


def state(project: Project):
    """Everything a mod can change, for comparing two projects."""
    return ({cid: vars(card) for cid, card in project.cards.items()},
            {cid: (a.key, a.base, a.drops, a.opponents, a.extra) for cid, a in project.added.items()},
            project.fusions, project.active_removes(), project.fusion_remove_all, {e: m for e, m in project.equips.items() if m or e in project.retail.equips},
            project.rituals, [{k: {c: w for c, w in v.items() if w} for k, v in d.items()} for d in project.pools],
            project.card_extra, project.notes)


class ReadTest(unittest.TestCase):
    def test_cards(self):
        f = fixture()
        data = f.game()
        self.assertEqual(len(data.cards), g.CARD_COUNT)
        for cid in (1, 2, 3, 250, 600, 601, 651, 681, 722):
            self.assertTrue(data.cards[cid].same(f.cards[cid]), (data.cards[cid], f.cards[cid]))

    def test_tables(self):
        f = fixture()
        data = f.game()
        without_glitch = {p: r for p, r in data.fusions.items() if p not in data.glitch_fusions}
        self.assertEqual(without_glitch, f.fusions)
        self.assertEqual(data.equips, f.equips)
        self.assertEqual(data.rituals, f.rituals)
        self.assertEqual(data.pools, f.pools)
        self.assertEqual(data.notes, [])

    def test_fusion_counts_past_255(self):
        pairs = {(1, b): 700 for b in range(2, 300)}
        pairs[(2, 3)] = 4
        decoded, _ = g.decode_fusions(g.encode_fusions(pairs))
        self.assertEqual({p: r for p, r in decoded.items() if p in pairs}, pairs)

    def test_glitch_pair(self):
        # One pair under card 5: a three-byte group; the game compares a second
        # pair made of the next record's first two bytes: card 8's count (7,
        # the partner) and its first group's control byte (the result):
        # partners 300 and 301 (high bits 1, 1), results 700 (2, 2): 1 | 2 << 2
        # | 1 << 4 | 2 << 6 = 153.
        pairs = {(5, 6): 7}
        pairs.update({(8, 300 + i): 700 for i in range(7)})
        decoded, glitch = g.decode_fusions(g.encode_fusions(pairs))
        for pair, result in pairs.items():
            self.assertEqual(decoded[pair], result)
        self.assertEqual(glitch, {(5, 7)})
        self.assertEqual(decoded[(5, 7)], 153)

    def test_disc_images(self):
        f = fixture()
        files = {"SLUS_014.11": f.slus, "DATA/WA_MRG.MRG": f.wa}
        with tempfile.TemporaryDirectory() as tmp:
            for raw, name in ((True, "game.bin"), (False, "game.iso")):
                path = Path(tmp) / name
                path.write_bytes(make_iso(files, raw))
                loaded = disc.load(path)
                self.assertEqual(loaded.slus, f.slus)
                self.assertEqual(loaded.wa, f.wa)
            folder = Path(tmp) / "extracted"
            (folder / "DATA").mkdir(parents=True)
            (folder / "SLUS_014.11").write_bytes(f.slus)
            (folder / "DATA" / "WA_MRG.MRG").write_bytes(f.wa)
            self.assertEqual(disc.load(folder).wa, f.wa)
            self.assertEqual(disc.load(folder / "SLUS_014.11").slus, f.slus)
            bad = Path(tmp) / "bad.bin"
            bad.write_bytes(bytes(2352 * 20))
            with self.assertRaises(disc.GameFilesError):
                disc.load(bad)


class PoolMathTest(unittest.TestCase):
    def test_scale_largest_remainder(self):
        weights = {1: 1, 2: 1, 3: 1}
        self.assertTrue(pools.scale(weights, [1, 2, 3], 2048))
        self.assertEqual(weights, {1: 683, 2: 683, 3: 682})   # lower id first between equals

    def test_replace_in_proportion(self):
        self.assertEqual(pools.apply_edit({5: 2048}, {1: 1, 2: 3}, replace=True), {1: 512, 2: 1536})

    def test_listed_keep_their_weight(self):
        result = pools.apply_edit({1: 1024, 2: 1024}, {3: 48})
        self.assertEqual(result, {1: 1000, 2: 1000, 3: 48})
        self.assertEqual(pools.apply_edit({1: 1024, 2: 1024}, {1: 0}), {2: 2048})

    def test_refusals(self):
        self.assertIsNone(pools.apply_edit({1: 2048}, {1: 0}))
        deck = {cid: 128 for cid in range(1, 17)}
        self.assertIsNone(pools.apply_edit(deck, {1: 0, 2: 0, 3: 0}, deck=True))
        self.assertIsNotNone(pools.apply_edit(deck, {1: 0, 2: 0}, deck=True))

    def test_edit_for_is_exact(self):
        retail = fixture().pools[3]["pow"]
        edited = dict(retail)
        moved = next(iter(edited))
        edited[moved] -= 40
        edited[599] = edited.get(599, 0) + 40
        listed, replace = pools.edit_for(retail, edited)
        self.assertFalse(replace)
        self.assertEqual(set(listed), {moved, 599})
        self.assertEqual(pools.apply_edit(retail, listed, replace), {c: w for c, w in edited.items() if w})
        self.assertIsNone(pools.edit_for(retail, dict(retail)))

    def test_normalize(self):
        self.assertEqual(sum(pools.normalize({1: 7, 2: 9, 3: 1}).values()), 2048)


class ManifestTest(unittest.TestCase):
    def setUp(self):
        self.retail = fixture().game()

    def test_retail_is_an_empty_mod(self):
        built = manifest.build(Project(self.retail))
        self.assertEqual(set(built), {"id", "name", "version"})

    def edited(self) -> Project:
        p = Project(self.retail)
        p.info.id = "test-mod"
        p.info.author = "Tester"
        # cards
        p.cards[1].name = "Bulbasaur"
        p.cards[1].attack = 1180
        p.cards[2].star1, p.cards[2].star2 = 8, 9
        p.cards[2].type = 19
        p.cards[3].description = "A round fellow.\nAlways late."
        p.card_extra[4] = {"art": "images/four.png"}
        new = p.add_card(3, "dingus")
        p.cards[new].name = "Dingus Shmingus"
        p.cards[new].attack = 2500
        p.added[new].opponents = True
        p.set_notes(5, "Untouched; tagged. <burn: 300>")
        p.set_notes(new, "Plan: a stronger Dingus later.")
        # fusions: change, forbid, add, and one with the new card
        pairs = sorted(self.retail.fusions)
        p.set_fusion(*pairs[0], 3)
        p.set_fusion(*pairs[1], None)
        p.set_fusion(597, 598, 599)
        p.set_fusion(new, 1, 2)
        # equips
        p.equips[651].discard(1)
        p.equips[651].add(new)
        p.equips[653] = {1}
        p.equips[652] |= {c for c in range(20, 601, 20) if c != 40}   # the Dragons (type 0) but one
        # rituals
        p.rituals[681] = (4, 5, 6, new)
        del p.rituals[682]
        # pools
        deck = p.pools[8]["deck"]
        first = next(iter(deck))
        deck[first] -= 100
        deck[new] = 100
        drop = p.pools[1]["pow"]
        gone = sorted(drop)[0]
        drop[sorted(drop)[1]] += drop.pop(gone)
        return p

    def test_round_trip(self):
        p = self.edited()
        self.assertEqual(validate.errors(validate.validate(p)), [])
        text = manifest.dumps(manifest.build(p))
        data = json.loads(text)
        self.assertEqual(data["cards"][0], {"replace": 1, "name": "Bulbasaur", "attack": 1180})
        self.assertEqual(data["cards"][1]["stars"], ["Sun", "Moon"])
        self.assertEqual(data["cards"][1]["type"], "Plant")
        self.assertEqual(data["cards"][-1]["copy"], 3)
        self.assertIn({"card": "Card 682", "result": None}, data["rituals"])
        self.assertEqual(set(data["decks"]), {"Heishin"})
        equip = next(e for e in data["equips"] if e["card"] == "Card 652")
        self.assertEqual((equip["add"], equip["remove"]), (["Dragon"], ["Card 40"]))
        self.assertEqual(set(data["drops"]), {"Simon Muran"})
        again = Project(self.retail)
        messages = manifest.apply(again, data)
        self.assertEqual(messages, [])
        self.assertEqual(state(again), state(p))
        self.assertEqual(manifest.build(again), manifest.build(p))

    def test_mod_folder(self):
        p = self.edited()
        p.other = {"text": "text.txt", "data": [{"file": "\\DATA\\WA_MRG.MRG;1", "patch": [{"at": "0x10", "bytes": "01"}]}],
                   "requires": ["other-mod"]}
        with tempfile.TemporaryDirectory() as tmp:
            first = Path(tmp) / "first"
            manifest.save_mod(p, first)
            (first / "text.txt").write_text("@bank names\n", encoding="utf-8")
            (first / "images").mkdir()
            (first / "images" / "four.png").write_bytes(b"png")
            opened, messages = manifest.open_mod(self.retail, first)
            self.assertEqual(messages, [])
            self.assertEqual(state(opened), state(p))
            self.assertEqual(opened.other, p.other)
            second = Path(tmp) / "second"
            manifest.save_mod(opened, second)
            self.assertTrue((second / "images" / "four.png").exists())
            self.assertEqual(manifest.read_json(second / "mod.json"), manifest.read_json(first / "mod.json"))
            game = Path(tmp) / "game"
            game.mkdir()
            (game / "SLUS_014.11").write_bytes(b"x")
            with self.assertRaises(ValueError):
                manifest.save_mod(opened, game)

    def test_save_as_inside_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "original"
            p = Project(self.retail)
            manifest.save_mod(p, source)
            (source / "text.txt").write_bytes(b"source text")
            destination = source / "copies" / "second"
            destination.mkdir(parents=True)
            (destination / "destination-only.txt").write_bytes(b"keep")
            copy_file = manifest.shutil.copy2

            def bounded_copy(item, target):
                # Fail promptly on the old recursive copy, without filling
                # the temporary folder to the OS's path-length limit.
                self.assertLess(len(target.relative_to(source).parts), 8)
                return copy_file(item, target)

            with mock.patch.object(manifest.shutil, "copy2", side_effect=bounded_copy):
                manifest.save_mod(p, destination)
            self.assertEqual((destination / "text.txt").read_bytes(), b"source text")
            self.assertEqual((destination / "destination-only.txt").read_bytes(), b"keep")
            self.assertFalse((destination / "copies").exists())
            self.assertEqual(p.source_dir, destination)

    def test_save_as_replaces_assets_with_source_versions(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, destination = Path(tmp) / "source", Path(tmp) / "destination"
            p = Project(self.retail)
            p.other["text"] = "text.txt"
            manifest.save_mod(p, source)
            (source / "text.txt").write_bytes(b"new text")
            destination.mkdir()
            (destination / "mod.json").write_text('{"id":"old"}')
            (destination / "text.txt").write_bytes(b"old text")
            manifest.save_mod(p, destination)
            self.assertEqual((destination / "text.txt").read_bytes(), b"new text")
            self.assertEqual((source / "text.txt").read_bytes(), b"new text")
            # Saving in place must not try to copy an asset onto itself.
            manifest.save_mod(p, destination)
            self.assertEqual((destination / "text.txt").read_bytes(), b"new text")

    def test_reads_what_people_write(self):
        p = Project(self.retail)
        data = {
            "id": "hand", "cards": [{"replace": "blue dragon", "attack": "1500", "stars": ["mars", "venus"]},
                                    {"copy": "Kuriboh", "id": "k2", "name": "Kuriboh 2", "type": "Dragon"}],
            "fusions": [{"with": ["Blue-Dragon", "mystic elf"], "result": 0},
                        {"remove": 3}, {"with": ["hand:k2:1", 1], "result": 2},
                        {"with": ["other:x:1", 1], "result": 2}],
            "equips": [{"card": 652, "add": ["Dragon"], "remove": ["Card 20"]}],
            "drops": {"all": {"sa-tec": {"Card 5": 0}}, "Seto": {"POW": {"replace": True, "Kuriboh": 1, "Mystic Elf": 3}}},
            "decks": {"8": {"replace": True, "Kuriboh": 1, "Mystic Elf": 3}},
        }
        messages = manifest.apply(p, data)
        self.assertEqual(p.cards[1].attack, 1500)
        self.assertEqual((p.cards[1].star1, p.cards[1].star2), (1, 10))
        self.assertNotIn((1, 2), p.fusions)
        self.assertFalse([pair for pair, r in p.fusions.items() if r == 3 and self.retail.fusions.get(pair) == 3])
        copy = max(p.added)
        self.assertEqual(p.fusions[(1, copy)], 2)
        self.assertEqual(p.cards[copy].type, 0)   # Dragon: still a monster, so allowed
        self.assertIn(20, [c for c in range(1, 601) if self.retail.cards[c].type == 0])
        self.assertNotIn(20, p.equips[652])
        self.assertIn(40, p.equips[652])
        self.assertEqual(p.pools[7]["pow"], {3: 512, 2: 1536})
        self.assertEqual(p.pools[8]["deck"], self.retail.pools[8]["deck"])   # two cards: the port refuses
        self.assertTrue(any("left as it was" in m for m in messages))
        self.assertEqual(len(p.kept["fusions"]), 1)
        self.assertTrue(any("cannot place" in m for m in messages))
        built = manifest.build(p)
        self.assertIn({"with": ["other:x:1", 1], "result": 2}, built["fusions"])

    # --- {"remove": C} ------------------------------------------------------

    def removed_result(self):
        """A card two disc recipes or more make, and those recipes."""
        p = Project(self.retail)
        return next((c, p.retail_recipes(c)) for c in sorted(set(self.retail.fusions.values()))
                    if len(p.retail_recipes(c)) >= 3)

    def remove_mod(self):
        """A mod as the editor writes one: a remove, a changed pair, a pair
        taken away and a pair added, none of them a recipe of the removed card."""
        c, recipes = self.removed_result()
        p = Project(self.retail)
        others = [pair for pair in sorted(self.retail.fusions) if self.retail.fusions[pair] != c]
        changed, forbidden = others[0], others[1]
        new = next((a, b) for a in range(1, 50) for b in range(a, 50) if (a, b) not in self.retail.fusions)
        rules = [{"remove": p.ref(c)}]
        for pair, result in sorted({changed: c, forbidden: None, new: c}.items()):
            rules.append({"with": [p.ref(pair[0]), p.ref(pair[1])], "result": p.ref(result) if result else None})
        return c, recipes, {"id": "rm", "name": "Remove", "fusions": rules}

    def game_fusions(self, p: Project, rules):
        """What the port makes of written fusion rules, pair by pair
        (duel_card_checks.c CardRules_Fusion): Tables_Fusion, then the cards'
        own "fusions" lists as the project keeps them (cards.c Cards_Fusion),
        then the disc's table by the bases, filtered."""
        own = {cid: extra.get("fusions") or [] for cid, extra in p.card_extra.items()}
        own.update({cid: added.extra.get("fusions") or [] for cid, added in p.added.items()})
        pairs, removes, every = {}, set(), False
        for order, rule in enumerate(rules):
            if "remove" in rule:
                if rule["remove"] == "all" and not p.resolve("all"):      # tables.c removes_all
                    every = True
                removes.add(p.resolve(rule["remove"]))
                continue
            a, b = (p.resolve(c) for c in rule["with"])
            pairs[p.pair(a, b)] = (order, p.resolve(rule["result"]) if rule["result"] else 0)

        def fusion(a, b):
            find = lambda x, y: pairs.get(p.pair(x, y))
            base_a, base_b = p.base_of(a), p.base_of(b)
            rule = find(a, b)
            if not rule:
                one = find(a, base_b) if base_b != b else None
                other = find(base_a, b) if base_a != a else None
                rule = one if one and (not other or one[0] > other[0]) else other
            if not rule and base_a != a and base_b != b:
                rule = find(base_a, base_b)
            if rule:
                return rule[1]
            for x, y in ((a, b), (b, a)):
                for listed in own.get(x, []):
                    if p.resolve(listed.get("with")) == y:
                        return p.resolve(listed.get("result")) if listed.get("result") else 0
            made = self.retail.fusions.get(p.pair(base_a, base_b), 0)
            return 0 if every or made in removes else made
        return fusion

    def assert_same_game(self, p: Project, rules, other_rules=None, cards=()):
        """The written rules make, for every pair a table names (and the
        pairs of `cards` with them), what the project holds, or what
        `other_rules` make."""
        game = self.game_fusions(p, rules)
        want = self.game_fusions(p, other_rules) if other_rules is not None else \
            (lambda a, b: p.fusions.get(p.pair(a, b)) or 0)
        pairs = set(self.retail.fusions) | set(p.fusions)
        for card in cards:
            base = p.base_of(card)
            pairs |= {p.pair(card, b if a == base else a) for a, b in self.retail.fusions if base in (a, b)}
        for pair in sorted(pairs):
            self.assertEqual(game(*pair), want(*pair), pair)

    def test_fusion_remove_kept_as_written(self):
        from fm_editor import bulk_fusions
        c, recipes, mod = self.remove_mod()
        p = Project(self.retail)
        self.assertEqual(manifest.apply(p, json.loads(json.dumps(mod))), [])
        self.assertEqual(p.fusion_removes, [c])
        self.assertFalse([pair for pair in recipes if pair in p.fusions])
        built = manifest.build(p)["fusions"]
        self.assertEqual(manifest.dumps({"fusions": built}), manifest.dumps({"fusions": mod["fusions"]}))
        self.assertEqual(bulk_fusions.rule_count(p), len(built))
        self.assert_same_game(p, built, mod["fusions"])
        self.assert_same_game(p, built)
        self.assertEqual(state(self.reopen(p)), state(p))
        # A copy of a recipe's card fuses as its base: the remove holds for it.
        copy = p.add_card(recipes[0][0], "c")
        self.assertEqual(self.game_fusions(p, built)(copy, recipes[0][1]), 0)
        self.assert_same_game(p, manifest.build(p)["fusions"], mod["fusions"], cards=[copy])

    def test_fusion_remove_with_a_recipe_back(self):
        from fm_editor import bulk_fusions
        c, recipes, mod = self.remove_mod()
        p = Project(self.retail)
        manifest.apply(p, mod)
        p.revert_fusion(recipes[1])
        built = manifest.build(p)["fusions"]
        self.assertEqual(built[0], {"remove": p.ref(c)})
        back = {"with": [p.ref(recipes[1][0]), p.ref(recipes[1][1])], "result": p.ref(c)}
        self.assertIn(back, built)
        self.assertEqual(len(built), len(mod["fusions"]) + 1)
        self.assertEqual(bulk_fusions.rule_count(p), len(built))
        self.assert_same_game(p, built)
        self.assertEqual(state(self.reopen(p)), state(p))
        # A recipe changed to another card stays a rule of its own, the remove too.
        p.set_fusion(*recipes[2], recipes[2][0])
        built = manifest.build(p)["fusions"]
        self.assertEqual(built[0], {"remove": p.ref(c)})
        self.assert_same_game(p, built)
        # Every recipe back: the remove is gone, and with it the rules it needed.
        for pair in recipes:
            p.revert_fusion(pair)
        built = manifest.build(p)["fusions"]
        self.assertEqual((p.fusion_removes, p.active_removes()), ([], []))
        self.assertEqual(built, mod["fusions"][1:])
        self.assertEqual(bulk_fusions.rule_count(p), len(built))
        self.assert_same_game(p, built)
        self.assertEqual(state(self.reopen(p)), state(p))
        # Taking one away again writes a rule for it alone, as it does in the
        # mod saved and opened again: the remove does not come back.
        again = self.reopen(p)
        for project in (p, again):
            project.set_fusion(*recipes[0], None)
        self.assertEqual(manifest.build(p)["fusions"], manifest.build(again)["fusions"])
        self.assertIn({"with": [p.ref(recipes[0][0]), p.ref(recipes[0][1])], "result": None},
                      manifest.build(p)["fusions"])

    def test_fusion_remove_edge_cases(self):
        c, recipes = self.removed_result()
        p = Project(self.retail)
        p.info.id = "rm"
        copy = p.add_card(5, "c5")
        none_makes = next(x for x in range(1, 723) if not p.retail_recipes(x))
        rules = [{"remove": p.ref(none_makes)}, {"remove": p.ref(c)}, {"remove": p.ref(c)},
                 {"remove": "rm:c5:1"}]
        rules += [{"with": [p.ref(a), p.ref(b)], "result": p.ref(c)} for a, b in recipes]
        manifest.apply(p, {"id": "rm", "fusions": rules})
        # A remove no disc recipe answers to is the mod's, and stays; one of a
        # card whose every recipe the mod puts back does nothing, and goes
        # (the rules that put them back are the mod's, and stay).
        self.assertEqual(p.active_removes(), [none_makes, copy])
        built = manifest.build(p)["fusions"]
        kept = [{"with": [p.ref(a), p.ref(b)], "result": p.ref(c)} for a, b in recipes]
        self.assertEqual(built, [{"remove": p.ref(none_makes)}, {"remove": "rm:c5:1"}] + kept)
        self.assert_same_game(p, built, rules)
        p.remove_card(copy)
        self.assertEqual(manifest.build(p)["fusions"], [{"remove": p.ref(none_makes)}] + kept)
        # The Fusions tab's "remove every disc recipe": the pairs go, and a
        # pair the mod has changed stays changed.
        p = Project(self.retail)
        p.set_fusion(*recipes[0], recipes[0][0])
        p.remove_recipes(c)
        self.assertEqual([pair for pair in recipes if pair in p.fusions], [recipes[0]])
        built = manifest.build(p)["fusions"]
        self.assertEqual(built[0], {"remove": p.ref(c)})
        self.assertEqual(len(built), 2)
        self.assert_same_game(p, built)

    def test_fusion_remove_all(self):
        from fm_editor import bulk_fusions
        c, recipes, mod = self.remove_mod()
        # {"remove": "all"} read: no disc pair is left but the mod's own
        # rules; a remove of one card beside it is needless and goes.
        rules = [{"remove": "all"}] + mod["fusions"]
        p = Project(self.retail)
        self.assertEqual(manifest.apply(p, {"id": "rm", "fusions": json.loads(json.dumps(rules))}), [])
        self.assertTrue(p.fusion_remove_all)
        self.assertEqual(p.active_removes(), [])
        own = {p.pair(*(p.resolve(x) for x in r["with"])) for r in mod["fusions"] if r.get("result")}
        self.assertEqual({pair for pair, made in p.fusions.items() if made}, own)
        built = manifest.build(p)["fusions"]
        self.assertEqual(built, [{"remove": "all"}] + mod["fusions"][1:])
        self.assertEqual(bulk_fusions.rule_count(p), len(built))
        self.assert_same_game(p, built, rules)
        self.assert_same_game(p, built)
        self.assertEqual(state(self.reopen(p)), state(p))
        # A disc pair put back is a rule of its own under the remove.
        p.revert_fusion(recipes[0])
        built = manifest.build(p)["fusions"]
        self.assertIn({"with": [p.ref(recipes[0][0]), p.ref(recipes[0][1])], "result": p.ref(c)}, built)
        self.assertEqual(bulk_fusions.rule_count(p), len(built))
        self.assert_same_game(p, built)
        self.assertEqual(state(self.reopen(p)), state(p))
        # A copy fuses as its base: the disc's recipe is gone for it too.
        copy = p.add_card(recipes[1][0], "c")
        self.assertEqual(self.game_fusions(p, manifest.build(p)["fusions"])(copy, recipes[1][1]), 0)
        self.assert_same_game(p, manifest.build(p)["fusions"], cards=[copy])

    def test_remove_all_fusions(self):
        from fm_editor import bulk_fusions
        c, recipes, mod = self.remove_mod()
        p = Project(self.retail)
        p.info.id = "rm"
        manifest.apply(p, json.loads(json.dumps(mod)))
        # An added card whose own list makes a fusion, and a rule the editor
        # keeps as written: the Fusions tab's "Remove all fusions" leaves none.
        added = p.add_card(5, "own")
        p.added[added].extra = {"fusions": [{"with": 7, "result": c}]}
        p._own_pairs = None
        p.kept["fusions"].append({"with": [1, 2], "result": 5, "setting": "x"})
        p.remove_all_fusions()
        built = manifest.build(p)["fusions"]
        self.assertEqual(built, [{"remove": "all"}, {"with": [p.ref(7), "rm:own:1"], "result": None}])
        self.assertEqual(bulk_fusions.rule_count(p), len(built))
        game = self.game_fusions(p, built)
        for pair in sorted(set(self.retail.fusions) | {p.pair(7, added)}):
            self.assertEqual(game(*pair), 0, pair)
        self.assertEqual(game(added, recipes[0][1] if recipes[0][0] == 5 else 7), 0)
        self.assertEqual(state(self.reopen(p)), state(p))
        # A fusion added afterwards is the one fusion there is.
        p.set_fusion(1, 2, 500)
        built = manifest.build(p)["fusions"]
        game = self.game_fusions(p, built)
        self.assertEqual(game(1, 2), 500)
        self.assertEqual(sum(1 for pair in self.retail.fusions if game(*pair)), 1 if (1, 2) in self.retail.fusions else 0)
        self.assert_same_game(p, built, cards=[added])
        # Restore: the disc's table back, the mod's own rules kept.
        p.restore_disc_fusions()
        built = manifest.build(p)["fusions"]
        self.assertFalse(p.fusion_remove_all)
        self.assertEqual(built, [{"with": [p.ref(1), p.ref(2)], "result": p.ref(500)}, {"with": [p.ref(7), "rm:own:1"], "result": None}]
                         if (1, 2) != p.pair(7, added) else built)
        self.assert_same_game(p, built)
        for pair, made in self.retail.fusions.items():
            if pair != (1, 2):
                self.assertEqual(p.fusions[pair], made)
        self.assertEqual(state(self.reopen(p)), state(p))

    def test_remove_all_card_named_all(self):
        # A card named "all" is that card, as tables.c reads it.
        p = Project(self.retail)
        with mock.patch.object(p.names, "find", side_effect=lambda text: 3 if text == "all" else 0):
            self.assertFalse(p.removes_all("all"))
            manifest.apply(p, {"fusions": [{"remove": "all"}]})
            self.assertFalse(p.fusion_remove_all)
            self.assertEqual(p.fusion_removes, [3])

    def test_fusion_rules_before_own_lists(self):
        """A rule of the mod's is asked before a card's own "fusions" list:
        one the result alone needs none for (a null rule on a recipe a
        remove takes away, a rule giving the disc's result) still decides."""
        c, recipes = self.removed_result()
        a, b = recipes[0]
        used = {a, b}
        others = []
        for pair, made in sorted(self.retail.fusions.items()):
            if made != c and pair[0] != pair[1] and not used & set(pair) and len(others) < 4:
                others.append(pair)
                used |= set(pair)
        (d1, d2), (e, f), (g, h), (i, j) = others
        p = Project(self.retail)
        ref = p.ref
        mod = {"id": "own", "name": "Own lists",
               "cards": [{"replace": ref(a), "fusions": [{"with": ref(b), "result": ref(600)}]},
                         {"replace": ref(d1), "fusions": [{"with": ref(d2), "result": ref(599)},
                                                         {"with": ref(h), "result": ref(598)}]},
                         {"replace": ref(g), "fusions": [{"with": ref(h), "result": ref(596)}]},
                         {"copy": e, "id": "k", "fusions": [{"with": ref(f), "result": ref(597)}]}]}
        rules = [{"remove": ref(c)}]
        for pair, result in sorted({(a, b): None, (d1, d2): self.retail.fusions[(d1, d2)],
                                    (e, f): self.retail.fusions[(e, f)]}.items()):
            rules.append({"with": [ref(pair[0]), ref(pair[1])], "result": ref(result) if result else None})
        mod["fusions"] = rules
        p = Project(self.retail)
        manifest.apply(p, json.loads(json.dumps(mod)))
        copy = max(p.added)
        game = self.game_fusions(p, rules)
        self.assertEqual((game(a, b), game(d1, d2), game(copy, f), game(g, h)),
                         (0, self.retail.fusions[(d1, d2)], self.retail.fusions[(e, f)], 596))
        built = manifest.build(p)
        self.assertEqual(manifest.dumps({"fusions": built["fusions"]}), manifest.dumps({"fusions": rules}))
        self.assert_same_game(p, built["fusions"], rules, cards=[copy])
        self.assertEqual(manifest.dumps(manifest.build(self.reopen(p))), manifest.dumps(built))
        # The modder's edits of a disc pair an own list names keep a rule, so
        # the game plays the result the tab shows: a recipe back under the
        # remove, and the disc's result where the list would otherwise win.
        p.revert_fusion((a, b))
        p.set_fusion(g, h, 5)
        p.revert_fusion((g, h))
        game = self.game_fusions(p, manifest.build(p)["fusions"])
        self.assertEqual((game(a, b), game(g, h)), (c, self.retail.fusions[(g, h)]))
        # A pair with no disc result reverted is no rule at all: the list decides.
        p.set_fusion(d1, h, 5)
        p.revert_fusion(p.pair(d1, h))
        self.assertNotIn(p.pair(d1, h), p.fusion_explicit)
        self.assertEqual(self.game_fusions(p, manifest.build(p)["fusions"])(d1, h),
                         self.retail.fusions.get(p.pair(d1, h)) or 598)
        p.set_fusion(copy, f, 5)
        p.revert_fusion(p.pair(copy, f))
        self.assertNotIn(p.pair(copy, f), p.fusion_explicit)
        # A pair no own list names goes back to no rule at all.
        p.set_fusion(i, j, 5)
        p.revert_fusion((i, j))
        self.assertNotIn((i, j), p.fusion_explicit)
        self.assertNotIn([ref(i), ref(j)], [rule.get("with") for rule in manifest.build(p)["fusions"]])
        # The file's rule for the base pair a copy's own list falls back on
        # stays through an edit: without it the copy's list would decide.
        p.set_fusion(e, f, 5)
        p.revert_fusion((e, f))
        self.assertIn((e, f), p.fusion_explicit)
        self.assertEqual(self.game_fusions(p, manifest.build(p)["fusions"])(copy, f), self.retail.fusions[(e, f)])
        from fm_editor import bulk_fusions
        self.assertEqual(bulk_fusions.rule_count(p), len(manifest.build(p)["fusions"]))
        p.fusion_explicit.discard((e, f))
        self.assertEqual(self.game_fusions(p, manifest.build(p)["fusions"])(copy, f), 597)
        # "Remove recipes of...": the remove leaves the own lists their pairs,
        # and own_fusion (the tab's "own list" rows) says what they make.
        p.remove_recipes(c)
        removes = set(p.active_removes())
        game = self.game_fusions(p, manifest.build(p)["fusions"])
        self.assertEqual((p.own_fusion((a, b), removes), game(a, b)), (600, 600))
        for pair in p.own_fusion_pairs()[0]:
            own = p.own_fusion(pair, removes)
            self.assertEqual(game(*pair), own if own is not None else game(*pair), pair)
        self.assertIsNone(p.own_fusion((d1, d2), removes))       # the mod's rule decides

    def test_deleted_card_leaves_no_needless_null_rule(self):
        p = Project(self.retail)
        free = [(a, b) for a in range(1, 40) for b in range(a + 1, 40) if (a, b) not in self.retail.fusions]
        (x, y), (u, v) = free[0], free[1]
        disc = next(pair for pair in sorted(self.retail.fusions) if not {x, y, u, v} & set(pair))
        mod = {"id": "d", "name": "Delete",
               "cards": [{"copy": 5, "id": "c"}, {"replace": u, "fusions": [{"with": v, "result": 400}]}],
               "fusions": [{"with": [p.ref(a), p.ref(b)], "result": "d:c:1"} for a, b in sorted([(x, y), (u, v), disc])]}
        manifest.apply(p, mod)
        p.remove_card(max(p.added))
        built = manifest.build(p)["fusions"]
        # the disc pair's rule forbids its disc fusion, and the own list's pair
        # keeps its null rule (it decided the pair before the list); the other goes
        self.assertEqual(built, [{"with": [p.ref(a), p.ref(b)], "result": None} for a, b in sorted([(u, v), disc])])
        self.assertEqual((p.fusion_status((u, v)), p.fusion_status(disc)), ("removed", "removed"))
        self.assertNotIn((x, y), p.fusion_explicit)

    def reopen(self, p: Project) -> Project:
        again = Project(self.retail)
        manifest.apply(again, json.loads(manifest.dumps(manifest.build(p))))
        return again

    def game_equips(self, p: Project) -> dict:
        """What the port makes of the written equips."""
        built = manifest.build(p)
        return manifest.simulate_equips(p, manifest.equip_rules(p, built.get("equips", []), []))

    def test_copies_in_equips(self):
        p = Project(self.retail)
        p.info.id = "t"
        monster = p.add_card(5, "c5")        # 652 fits 5, so its copy too
        equip = p.add_card(652, "e2")        # a copy of 652 fits what 652 does
        self.assertIn(monster, p.equips[652])
        self.assertEqual(p.equips[equip], p.equips[652])
        # swap 5 for its copy on 652: the rule for 5 must not take the copy out
        p.equips[652].discard(5)
        # the copied equip: only 6 and the copy of 5, not 5 itself
        p.equips[equip] = {6, monster}
        # 651 fits 1-30: take the copy of 5 out of it only
        p.equips[651].discard(monster)
        want = {e: p.equips.get(e, p.equip_baseline(e)) for e in p.equip_cards()}
        self.assertEqual(self.game_equips(p), want)
        again = self.reopen(p)
        for e in p.equip_cards():
            self.assertEqual(again.equips.get(e, again.equip_baseline(e)), want[e], e)

    def test_converted_copies_are_not_equip_targets(self):
        for kind in (g.TYPE_MAGIC, g.TYPE_TRAP, g.TYPE_RITUAL, g.TYPE_EQUIP):
            with self.subTest(kind=kind):
                p = Project(self.retail)
                cid = p.add_card(5, "converted")
                self.assertIn(cid, p.equips[652])
                effect = next(i for i, card in self.retail.cards.items() if card.type == kind)
                p.cards[cid] = p.cards[cid].copy(type=kind, attack=0, defense=0)
                p.added[cid].extra["effect"] = effect
                built = manifest.build(p)
                self.assertNotIn(cid, self.game_equips(p)[652])
                self.assertEqual(manifest.build(self.reopen(p)), built)
                self.assertFalse(p.equip_retail(652, cid))

    def test_converted_equip_uses_its_retail_effect_targets(self):
        p = Project(self.retail)
        cid = p.add_card(3, "equip")
        p.cards[cid] = p.cards[cid].copy(type=g.TYPE_EQUIP)
        p.added[cid].extra["effect"] = 652
        # Changing the original effect slot must not change its behavior.
        p.cards[652] = p.cards[652].copy(type=0)
        self.assertEqual(p.equip_baseline(cid), set(self.retail.equips[652]))
        self.assertEqual(self.game_equips(p)[cid], set(self.retail.equips[652]))
        self.assertFalse(p.equip_retail(cid, 652))

    def test_converted_retail_cards_do_not_leak_into_equip_rules(self):
        p = Project(self.retail)
        p.cards[5] = p.cards[5].copy(type=g.TYPE_MAGIC)
        p.equips[651] = {5, 6}
        # An equip changed into a monster must not export its old edits.
        p.cards[652] = p.cards[652].copy(type=0)
        p.equips[652] = {1}
        built = manifest.build(p)
        messages = []
        again = Project(self.retail)
        manifest.apply(again, built, messages)
        self.assertFalse(messages)
        self.assertEqual(manifest.build(again), built)
        self.assertEqual(self.game_equips(p)[651], {6})
        self.assertTrue(all(p.resolve(entry["card"]) != 652 for entry in built["equips"]))

    def test_rules_switched_by_a_setting_are_kept(self):
        # The editor shows the disc's table, not a setting's: a rule with
        # "setting" is written back as it came, and does not change the table.
        p = Project(self.retail)
        fusion = {"with": [1, 2], "result": 5, "setting": "thunder"}
        equip = {"card": 652, "add": ["Dragon"], "setting": "thunder"}
        ritual = {"card": 665, "tributes": [1, 2, 3], "result": 4, "setting": "pick", "value": 2}
        data = {"id": "s", "settings": [{"key": "thunder", "type": "bool", "default": 1}],
                "fusions": [fusion], "equips": [equip], "rituals": [ritual]}
        messages = manifest.apply(p, data)
        self.assertEqual(p.fusions.get((1, 2)), self.retail.fusions.get((1, 2)))
        self.assertEqual(sum("switched by setting" in m for m in messages), 3)
        built = json.loads(manifest.dumps(manifest.build(p)))
        self.assertEqual((built["fusions"], built["equips"], built["rituals"]), ([fusion], [equip], [ritual]))

    def test_forbidden_copy_fusion(self):
        p = Project(self.retail)
        p.info.id = "t"
        copy = p.add_card(1, "c1")
        p.set_fusion(copy, 2, None)        # 1 + 2 makes 3; the copy of 1 must not
        self.assertEqual(p.fusions[(2, copy)], 0)
        built = manifest.build(p)
        self.assertIn({"with": [p.ref(2), "t:c1:1"], "result": None}, built["fusions"])
        again = self.reopen(p)
        self.assertEqual(again.fusions.get((2, copy)), 0)
        again.revert_fusion((2, copy))
        self.assertNotIn((2, copy), again.fusions)

    def test_added_ritual(self):
        # A copy of a ritual card is listed, has its base's recipe until it
        # is given one, and its own recipe goes out and comes back by id.
        p = Project(self.retail)
        p.info.id = "t"
        base = p.ritual_cards()[0]
        copy = p.add_card(base, "r1")
        monster = p.add_card(1, "m1")
        self.assertIn(copy, p.ritual_cards())
        self.assertNotIn(monster, p.ritual_cards())
        self.assertNotIn("rituals", manifest.build(p))
        p.rituals[copy] = (1, 2, 3, 4)
        self.assertEqual(p.ritual_status(copy), "added")
        self.assertIn({"card": "t:r1:1", "tributes": [p.ref(1), p.ref(2), p.ref(3)], "result": p.ref(4)},
                      manifest.build(p)["rituals"])
        self.assertFalse([i for i in validate.validate(p) if i.area == "Rituals"])
        again = self.reopen(p)
        self.assertEqual(again.rituals.get(copy), (1, 2, 3, 4))
        self.assertEqual(again.rituals.get(base), p.rituals.get(base))
        p.remove_card(copy)
        self.assertNotIn(copy, p.rituals)

    def test_card_made_a_ritual(self):
        # A disc monster typed Ritual is a ritual only with "effect" naming
        # one (it is played with that card's effect); its recipe round-trips.
        p = Project(self.retail)
        p.info.id = "t"
        base = p.ritual_cards()[0]
        p.cards[1] = p.cards[1].copy(type=g.TYPE_RITUAL)
        self.assertNotIn(1, p.ritual_cards())
        p.card_extra[1] = {"effect": base}
        self.assertIn(1, p.ritual_cards())
        p.rituals[1] = (2, 3, 4, 5)
        self.assertFalse([i for i in validate.validate(p) if i.area == "Rituals"])
        again = self.reopen(p)
        self.assertIn(1, again.ritual_cards())
        self.assertEqual(again.rituals.get(1), (2, 3, 4, 5))

    def test_reverts(self):
        # What the tabs' Revert buttons do, in the model a front end calls.
        p = Project(self.retail)
        p.rituals[681] = (4, 5, 6, 500)
        p.rituals.pop(682)
        p.rituals[683] = (1, 2, 3, 502)
        self.assertEqual([p.ritual_status(r) for r in (681, 682, 683, 684)], ["changed", "removed", "added", ""])
        for r in (681, 682, 683):
            p.revert_ritual(r)
        self.assertEqual(p.rituals, self.retail.rituals)
        p.pools[1]["pow"] = {1: 2048}
        p.revert_pool(1, "pow")
        self.assertEqual(p.pools[1]["pow"], self.retail.pools[1]["pow"])
        self.assertIsNot(p.pools[1]["pow"], self.retail.pools[1]["pow"])
        p.cards[1] = p.cards[1].copy(attack=1230)
        copy = p.add_card(1, "c1")
        p.cards[copy] = p.cards[copy].copy(name="Another", defense=10)
        p.set_password(copy, "12345678")
        p.set_notes(copy, "mine")
        p.revert_card(copy)            # back to its base as the mod has it
        self.assertTrue(p.cards[copy].same(p.cards[1].copy(id=copy)))
        self.assertEqual((p.password(copy), p.notes[copy]), ("", "mine"))
        self.assertTrue(card_matches(p, copy, str(copy)))
        self.assertTrue(card_matches(p, copy, p.cards[1].name[1:4].upper()))
        self.assertFalse(card_matches(p, 2, str(copy)))
        self.assertTrue(card_matches(p, 2, ""))

    def test_notes(self):
        p = self.edited()
        cards = manifest.build(p)["cards"]
        # A card with nothing but notes is noted, not changed (cards.c notes_only).
        self.assertIn({"replace": 5, "notes": "Untouched; tagged. <burn: 300>"}, cards)
        self.assertEqual(cards[-1]["notes"], "Plan: a stronger Dingus later.")
        self.assertFalse(p.card_changed(5))
        p.revert_card(5)
        self.assertIn(5, p.notes)                      # the modder's, not the disc's
        p.set_notes(5, "  \n")
        self.assertNotIn(5, p.notes)
        again = Project(self.retail)
        messages = manifest.apply(again, {"id": "t", "cards": [
            {"replace": 7, "notes": "one <a: 1>"}, {"replace": 7, "attack": 100, "notes": "two"},
            {"replace": 8, "notes": 12}]})
        self.assertEqual(again.notes[7], "one <a: 1>\ntwo")    # as the game joins them
        self.assertEqual(again.card_extra[8], {"notes": 12})
        self.assertEqual(messages, ["cards[2]: \"notes\" must be text; kept as written"])
        built = manifest.build(again)["cards"]
        self.assertEqual(built[0], {"replace": 7, "attack": 100, "notes": "one <a: 1>\ntwo"})
        p.remove_card(max(p.added))
        self.assertEqual(set(p.notes), set())

    def test_made_monster_stars(self):
        """A card made a monster: its stars always written (the game would
        give defaults), and read without them as the game gives them."""
        p = Project(self.retail)
        p.cards[337].type, p.cards[337].star1, p.cards[337].star2 = g.TYPE_MAGIC, 0, 0   # a magic card, as on the disc
        copy = p.add_card(337, "rd")
        p.cards[copy].type, p.cards[copy].star1, p.cards[copy].star2 = 0, 0, 0
        cards = manifest.build(p)["cards"]
        self.assertEqual(cards[-1]["stars"], [0, 0])
        self.assertEqual((self.reopen(p).cards[copy].star1, self.reopen(p).cards[copy].star2), (0, 0))
        other = Project(self.retail)
        for base in (336, 337, 338):    # magic cards with no stars, as on the disc
            for card in (other.cards[base], other.retail.cards[base]):
                card.type, card.star1, card.star2 = g.TYPE_MAGIC, 0, 0
        manifest.apply(other, {"id": "t", "cards": [
            {"replace": 336, "type": "Fiend"}, {"copy": 337, "id": "a", "type": "Dragon", "model": 58},
            {"copy": 338, "id": "b", "type": "Dragon", "stars": ["Mars", "none"]}]})
        sun_moon = (g.STAR_NAMES.index("Sun"), g.STAR_NAMES.index("Moon"))
        kuriboh = (self.retail.cards[58].star1, self.retail.cards[58].star2)
        added = sorted(other.added)
        self.assertEqual((other.cards[336].star1, other.cards[336].star2), sun_moon)
        self.assertEqual((other.cards[added[0]].star1, other.cards[added[0]].star2), kuriboh)
        self.assertEqual((other.cards[added[1]].star1, other.cards[added[1]].star2), (1, 0))

    def test_frame(self):
        p = Project(self.retail)
        p.cards[1].frame = 4
        copy = p.add_card(2, "c1")
        p.cards[copy].frame = 5
        cards = manifest.build(p)["cards"]
        self.assertIn({"replace": 1, "frame": "Purple"}, cards)
        self.assertEqual(cards[-1]["frame"], "Orange")
        self.assertTrue(p.card_changed(1))
        again = self.reopen(p)
        self.assertEqual((again.cards[1].frame, again.cards[max(again.added)].frame), (4, 5))
        self.assertEqual(again.cards[1].shown_frame(), 4)
        self.assertEqual(again.cards[2].shown_frame(), g.type_frame(again.cards[2].type))
        again.revert_card(1)
        self.assertEqual(again.cards[1].frame, -1)
        # As cards.c reads it: names in any case, numbers, "Type" back to the type's.
        other = Project(self.retail)
        messages = manifest.apply(other, {"id": "t", "cards": [
            {"replace": 3, "frame": "ritual"}, {"replace": 4, "frame": 1}, {"replace": 4, "frame": "Type"},
            {"replace": 5, "frame": "Mauve"}]})
        self.assertEqual([other.cards[c].frame for c in (3, 4, 5)], [3, -2, -1])
        self.assertEqual(messages, ["cards[3]: \"frame\" is Gold, Green, Pink, Blue, Purple, Orange or Type (or the "
                                    "disc's Monster, Magic, Trap, Ritual); left out"])
        self.assertEqual(other.cards[4].shown_frame(), g.type_frame(other.cards[4].type))
        # "Type" (an effect monster kept gold) is not the same as leaving it out (orange): kept through a save.
        typed = Project(self.retail)
        typed.cards[1].frame = -2
        typed.set_monster_effects(1, [{"when": "summon", "do": "heal", "amount": 100}])
        self.assertEqual(manifest.build(typed)["cards"][0]["frame"], "Type")
        self.assertEqual(self.reopen(typed).cards[1].frame, -2)

    def test_frame_color_names(self):
        # cards.c frame_color_names: Gold, Green, Pink and Blue are Monster,
        # Magic, Trap and Ritual, in any case; saved as the disc's names.
        p = Project(self.retail)
        messages = manifest.apply(p, {"id": "t", "cards": [
            {"replace": 1, "frame": "Gold"}, {"replace": 2, "frame": "green"}, {"replace": 3, "frame": "PINK"},
            {"replace": 4, "frame": "Blue"}, {"copy": 5, "id": "c", "frame": "Gold"}, {"replace": 6, "frame": 7}]})
        self.assertEqual([p.cards[c].frame for c in (1, 2, 3, 4, max(p.added), 6)], [0, 1, 2, 3, 0, -1])
        self.assertEqual(len(messages), 1)
        self.assertIn("cards[5]", messages[0])
        cards = manifest.build(p)["cards"]
        self.assertEqual([e.get("frame") for e in cards], ["Monster", "Magic", "Trap", "Ritual", "Monster"])

    def test_tags_round_trip(self):
        """A card's "tags" (cards.c): the editor's own field, written back as
        read; left out an added card has its base's, [] is none."""
        from fm_editor.model import parse_tags, tags_text
        written = {"id": "t", "cards": [
            {"replace": 1, "tags": ["god", "dragon"]},
            {"replace": 2, "frame": "Gold", "tags": []},
            {"copy": 1, "id": "own", "name": "A", "tags": ["fiend"]},
            {"copy": 1, "id": "inherits", "name": "B"},
            {"copy": 1, "id": "none", "name": "C", "tags": []}]}
        p = Project(self.retail)
        self.assertEqual(manifest.apply(p, json.loads(json.dumps(written))), [])
        own, inherits, none = sorted(p.added)
        self.assertEqual(p.tags_of(1), (["god", "dragon"], False))
        self.assertEqual(p.tags_of(2), ([], False))
        self.assertEqual(p.tags_of(own), (["fiend"], False))
        self.assertEqual(p.tags_of(inherits), (["god", "dragon"], True))
        self.assertEqual(p.tags_of(none), ([], False))
        self.assertEqual(p.tags_of(3), ([], False))
        built = manifest.build(p)
        self.assertEqual(built["cards"], [
            {"replace": 1, "tags": ["god", "dragon"]}, {"replace": 2, "frame": "Monster", "tags": []},
            {"copy": 1, "id": "own", "name": "A", "tags": ["fiend"]}, {"copy": 1, "id": "inherits", "name": "B"},
            {"copy": 1, "id": "none", "name": "C", "tags": []}])
        self.assertEqual(manifest.build(self.reopen(p)), built)
        # Set and taken away: a disc card's entry goes with its last key.
        p.set_tags(3, ["x"])
        self.assertIn({"replace": 3, "tags": ["x"]}, manifest.build(p)["cards"])
        p.set_tags(3, None)
        self.assertNotIn(3, p.card_extra)
        p.set_tags(none, None)
        self.assertEqual(p.tags_of(none), (["god", "dragon"], True))
        # Revert: a disc card loses them, an added card has its base's again.
        p.revert_card(1)
        p.revert_card(own)
        self.assertEqual((p.tags_of(1), p.tags_of(own)), (([], False), ([], True)))
        # The form's text.
        self.assertEqual([tags_text(t) for t in (None, [], ["a", "b c"])], ["", "[]", "a, b c"])
        self.assertEqual([parse_tags(t) for t in ("", " ", "[]", ",", " god ,fiend, god,")],
                         [None, None, [], None, ["god", "fiend"]])

    def test_unnamed_copy_keeps_the_disc_name(self):
        p = Project(self.retail)
        data = {"id": "t", "cards": [{"replace": 3, "name": "New Three"}, {"copy": 3, "id": "c3"}]}
        manifest.apply(p, data)
        copy = max(p.added)
        self.assertEqual(p.cards[3].name, "New Three")
        self.assertEqual(p.cards[copy].name, self.retail.cards[3].name)
        self.assertEqual(manifest.build(p)["cards"][1]["name"], self.retail.cards[3].name)

    def test_json_like_the_port(self):
        p = Project(self.retail)
        data = {"id": "t", "cards": [{"copy": 3, "id": "a", "drops": None, "opponents": 1, "attack": 2000.0},
                                     {"replace": 4, "password": "12345678", "defense": True}],
                "equips": [{"card": 653, "replace": 1, "add": [1], "bonus": 300, "bonus_if": {"Dragon": 900}}],
                "decks": {"Simon Muran": {"fixed": True, "Card 1": 40}},
                "drops": {"all": {"pow": {"Nobody At All": 5, "Card 1": 10}}}}
        messages = manifest.apply(p, data)
        copy = max(p.added)
        self.assertTrue(p.added[copy].drops)
        self.assertTrue(p.added[copy].opponents)
        self.assertEqual(p.cards[copy].attack, 2000)
        self.assertEqual(p.cards[4].defense, 0)            # true is 1, stored in tens
        self.assertEqual(p.equips[653], {1})
        built = manifest.build(p)
        self.assertEqual(built["cards"][0]["password"], "12345678")
        self.assertEqual(p.equip_bonus, {653: (300, 300)})      # the Cards tab's ATK and DEF boosts
        self.assertEqual(built["equips"][-2:], [{"card": p.ref(653), "bonus": 300},
                                                {"card": 653, "bonus_if": {"Dragon": 900}}])
        self.assertEqual(built["decks"]["Simon Muran"], {"fixed": True, "Card 1": 40})
        self.assertEqual(list(built["drops"])[0], "all")
        self.assertEqual(built["drops"]["all"]["pow"]["Nobody At All"], 5)
        self.assertFalse(any("Nobody At All" in str(v) for k, v in built["drops"].items() if k != "all"))
        # A fixed deck is now edited in the Duelists tab, not kept aside; the name it
        # cannot place is kept under the name it was written with.
        self.assertEqual(p.fixed["Simon Muran"].duelist, 1)
        self.assertEqual(p.fixed["Simon Muran"].kept, {"Card 1": 40})

    def test_a_mods_own_duelists_are_its_own_to_edit(self):
        """A duelist the mod adds is one of the editor's (roster.py): "decks"
        and "drops" naming it by its identity or its slot reach it, and it
        goes back into "duelists" as written, its pools into its own files;
        one naming another mod's duelist is still kept as written."""
        p = Project(self.retail)
        data = {"id": "t", "duelists": [{"id": "dark-simon", "copy": "Heishin", "slot": 45}],
                "decks": {"t:dark-simon": {"replace": True, "Blue Dragon": 200},
                          "other:somebody": {"Card 3": 9},
                          "Heishin": {"Card 2": 100}},
                "drops": {"45": {"tec": {"replace": True, "Blue Dragon": 1}}}}
        messages = manifest.apply(p, data)
        built = manifest.build(p)
        self.assertEqual(built["duelists"], data["duelists"])
        e = p.roster[0]
        self.assertEqual((e.key, e.base, e.slot), ("dark-simon", 8, 45))
        self.assertEqual(e.pools["tec"], {1: 2048})
        # One card is no deck: the game refuses the edit, and so the deck stays Heishin's.
        self.assertEqual(e.pools["deck"], p.retail.pools[8]["deck"])
        self.assertTrue(any("refuses" in m for m in messages))
        self.assertNotIn("t:dark-simon", built["decks"])
        self.assertNotIn("45", built.get("drops", {}))
        self.assertEqual(built["decks"]["other:somebody"], {"Card 3": 9})
        self.assertIn("Card 2", str(built["decks"]["Heishin"]))        # and the disc's own is edited
        drops, decks = manifest._pool_tables(p)
        self.assertEqual(drops[e], {"tec": {"replace": True, "Blue Dragon": 2048}})

    def test_a_table_named_as_a_file_stays_that_file(self):
        """"decks": "tables/decks.json" is a file the editor does not read, so
        it must not be replaced by the pools the editor holds."""
        p = Project(self.retail)
        manifest.apply(p, {"id": "t", "decks": "tables/decks.json", "drops": {"Heishin": {"pow": {"Card 1": 50}}}})
        built = manifest.build(p)
        self.assertEqual(built["decks"], "tables/decks.json")
        self.assertIn("Heishin", built["drops"])

    def test_settings_the_port_refuses(self):
        p = Project(self.retail)
        p.info.settings = [{"key": "a", "min": 5, "max": 1}, {"key": "b", "step": 0},
                           {"key": "c", "type": "bool", "default": 2}, {"key": "d", "type": "choice",
                                                                         "choices": ["x"], "default": 1}]
        text = "\n".join(i.message for i in validate.validate(p) if i.level == "error")
        self.assertIn("a (min is more than max)", text)
        self.assertIn("b (step is at least 1)", text)
        self.assertIn("c (default 2 is outside 0 to 1)", text)
        self.assertIn("d (default 1 is outside 0 to 0)", text)
        messages = manifest.apply(Project(self.retail), {"id": "t", "settings": {}})
        self.assertTrue(any("settings" in m for m in messages))


class ValidateTest(unittest.TestCase):
    def test_tags(self):
        p = Project(fixture().game())
        p.set_tags(1, ["", "x" * 31, "y" * 32, 7])
        found = [i.message for i in validate.validate_card(p, 1)]
        self.assertEqual(sum("empty tag" in m for m in found), 2)
        self.assertEqual([m for m in found if "over 31 letters" in m], [f'the tag "{"y" * 32}" is over 31 letters: '
                                                                        "the game notes it and leaves it out"])
        p.card_extra[1]["tags"] = "god"         # as a mod may write it
        self.assertTrue(any('"tags" is a list' in i.message for i in validate.validate_card(p, 1)))
        # 32 different in all; the 33rd met (cards in the order written) is left out.
        p.set_tags(1, [f"t{n}" for n in range(30)])
        p.set_tags(2, ["t0", "t30", "t31"])
        self.assertFalse([i for i in validate.validate(p) if "different tags" in i.message])
        added = p.add_card(3)
        p.set_tags(added, ["t5", "t32", "t33"])
        over = [i for i in validate.validate(p) if "different tags" in i.message]
        self.assertEqual(len(over), 1)
        self.assertEqual((over[0].level, over[0].target), ("warning", added))
        self.assertIn("34 different tags", over[0].message)
        self.assertIn("t32, t33 left out", over[0].message)

    def test_trap_threshold_inheritance_and_effect_defaults(self):
        p = Project(fixture().game())
        p.retail.cards[681].type = p.cards[681].type = g.TYPE_TRAP
        p.cards[1] = p.cards[1].copy(type=g.TYPE_TRAP)
        p.card_extra[1] = {"effect": 681, "trap_threshold": 1234}
        p.card_extra[681] = {"trap_threshold": 9000}
        p.other["trap_thresholds"] = {"681": 800}
        added = p.add_card(1)
        self.assertEqual(p.trap_threshold_override(added), 1234)
        self.assertEqual(p.trap_threshold_default(p.effect_of(added)), 800)
        p.set_trap_threshold(added, None)
        restored = Project(p.retail)
        manifest.apply(restored, manifest.build(p))
        self.assertIsNone(restored.trap_threshold_override(added))
        self.assertEqual(restored.trap_threshold_default(restored.effect_of(added)), 800)
        for invalid in (-1, 65536, True, "1500", 1.5):
            p.set_trap_threshold(added, invalid)
            self.assertTrue(any(i.level == "error" and "trap_threshold" in i.message
                                for i in validate.validate_card(p, added)))

    def test_clearing_a_retail_trap_threshold_writes_no_entry(self):
        p = Project(fixture().game())
        p.retail.cards[681].type = p.cards[681].type = g.TYPE_TRAP
        p.set_trap_threshold(681, 1234)
        self.assertEqual(manifest.build_cards(p), [{"replace": 681, "trap_threshold": 1234}])
        p.set_trap_threshold(681, None)
        self.assertEqual(manifest.build_cards(p), [])
        self.assertNotIn(681, p.card_extra)

    def test_problems(self):
        p = Project(fixture().game())
        p.info.id = "bad id!"
        p.cards[1].attack = 1234
        p.cards[2].level = 13
        new = p.add_card(1, "x")
        p.cards[new].type = g.TYPE_MAGIC
        p.pools[1]["deck"] = {1: 2000}
        p.equips[1] = {2}
        found = [(i.area, i.message) for i in validate.validate(p) if i.level == "error"]
        text = "\n".join(m for _, m in found)
        self.assertIn("id must be 1-63", text)
        self.assertIn("stored in tens", text)
        self.assertIn("level is 0 to 12", text)
        self.assertIn("matching retail effect", text)
        self.assertIn("at least 14 cards", text)
        self.assertIn("add up to 2000", text)
        self.assertIn("not an equip card", text)

    def test_text_lines(self):
        self.assertEqual(validate.text_lines("a b"), 1)
        self.assertEqual(validate.text_lines("x" * 20 + " y"), 2)
        self.assertEqual(validate.text_lines("a\nb\nc"), 3)
        # An icon is two letters of the line, a color none (cards.c text_code).
        self.assertEqual(validate.text_lines("x" * 15 + " {f8 0B 04} y"), 1)
        self.assertEqual(validate.text_lines("x" * 16 + " {f8 0B 04} y"), 2)
        self.assertEqual(validate.text_lines("{f8 0A 02}" + "x" * 18 + " y"), 1)



class StarchipTest(unittest.TestCase):
    def setUp(self):
        self.project = Project(fixture().game())

    def test_price_round_trip_and_revert(self):
        p = self.project
        self.assertEqual(p.retail.starchips[1], 10)
        self.assertEqual(p.retail.starchips[g.CARD_COUNT], g.CARD_COUNT * 10)
        for cid, cost in ((1, 0), (2, 999999)):
            p.set_starchips(cid, cost)
            self.assertTrue(p.card_changed(cid))
        p.set_password(1, "00000001")
        data = manifest.build(p)
        self.assertEqual(data["passwords"], {
            "Blue Dragon": {"password": "00000001", "starchips": 0},
            "Mystic Elf": {"starchips": 999999}})
        again = Project(p.retail)
        self.assertEqual(manifest.apply(again, data), [])
        self.assertEqual(again.starchip_cost(1), 0)
        self.assertEqual(again.starchip_cost(2), 999999)
        self.assertEqual(manifest.build(again), data)
        again.revert_card(2)
        self.assertEqual(again.starchip_cost(2), 20)
        self.assertNotIn("Mystic Elf", manifest.build(again)["passwords"])
        self.assertFalse(again.card_changed(2))

    def test_added_password_and_price_round_trip(self):
        p = self.project
        cid = p.add_card(1, "aurora-wing")
        p.set_password(cid, "00000723")
        p.set_starchips(cid, 100)
        data = manifest.build(p)
        identity = p.identity(cid)
        self.assertEqual(data["passwords"][identity], {"starchips": 100})
        again = Project(p.retail)
        self.assertEqual(manifest.apply(again, data), [])
        self.assertEqual(again.password(cid), "00000723")
        self.assertEqual(again.starchip_cost(cid), 100)
        self.assertEqual(manifest.build(again), data)
        # A table's explicit password overrides the card entry, even empty.
        data["passwords"][identity]["password"] = "87654321"
        again = Project(p.retail)
        manifest.apply(again, data)
        self.assertEqual(again.password(cid), "87654321")
        again.set_password(cid, "00001234")
        self.assertEqual(manifest.build(again)["passwords"][identity]["password"], "00001234")
        again.set_card_key(cid, "renamed-wing")
        renamed = again.identity(cid)
        rebuilt = manifest.build(again)
        self.assertNotIn(identity, rebuilt["passwords"])
        self.assertEqual(rebuilt["passwords"][renamed], {"password": "00001234", "starchips": 100})
        again.remove_card(cid)
        self.assertNotIn(renamed, manifest.build(again).get("passwords", {}))

    def test_preserve_rules_and_replace_alias_prices(self):
        p = self.project
        table = {"all": {"starchips": 500},
                 "1": {"password": "card number", "starchips": 70, "extra": 42},
                 "Blue Dragon": {"starchips_percent": 25},
                 "Mystic Elf": {"starchips": 100, "starchips_percent": 50},
                 "Unknown": {"starchips": "keep this"}}
        manifest.apply(p, {"passwords": table})
        self.assertEqual(manifest.build(p)["passwords"], table)
        # Percentages apply to the disc price, rounded like Tables_PasswordShop.
        self.assertEqual(p.starchip_cost(1), 3)
        self.assertEqual(p.starchip_cost(2), 100)
        self.assertEqual(p.starchip_cost(3), 500)
        p.set_starchips(1, 0)
        edited = manifest.build(p)["passwords"]
        self.assertEqual(edited["1"], {"password": "card number", "extra": 42})
        self.assertEqual(edited["Blue Dragon"], {"starchips": 0})
        for key in ("all", "Mystic Elf", "Unknown"):
            self.assertEqual(edited[key], table[key])
        again = Project(p.retail)
        manifest.apply(again, {"passwords": edited})
        self.assertEqual(again.starchip_cost(1), 0)
        again.set_starchips(1, None)
        self.assertEqual(again.starchip_cost(1), 500)
        self.assertNotIn("Blue Dragon", manifest.build(again)["passwords"])
        self.assertEqual(manifest.build(again)["passwords"]["1"], {"password": "card number", "extra": 42})

    def test_cost_validation_and_percent_limits(self):
        p = self.project
        for bad in (-1, 1000000, True, 1.5, "100"):
            with self.assertRaises(ValueError):
                p.set_starchips(1, bad)
        added = p.add_card(1, "added")
        self.assertEqual(p.starchip_cost(added), 999999)
        p.set_starchips(added, 100)
        self.assertEqual(p.starchip_cost(added), 100)
        for percent, expected in ((0, 0), (1, 1), (25, 3), (1000, 100)):
            p.other["passwords"] = {"all": {"starchips_percent": percent}}
            self.assertEqual(p.starchip_cost(1), expected)
        p.retail.starchips[1] = 999999
        self.assertEqual(p.starchip_cost(1), 999999)


class EquipBonusTest(unittest.TestCase):
    def setUp(self):
        self.project = Project(fixture().game())

    def test_bonus_round_trip_and_megamorph(self):
        p = self.project
        self.assertEqual(p.equip_bonus_of(651), (500, 500))
        self.assertEqual(p.equip_bonus_of(g.MEGAMORPH), (1000, 1000))
        copy = p.add_card(g.MEGAMORPH, "mega-copy")
        self.assertEqual(p.equip_bonus_of(copy), (1000, 1000))      # it plays as Megamorph
        p.set_equip_bonus(g.MEGAMORPH, 2500)
        self.assertEqual(p.equip_bonus_of(copy), (2500, 2500))      # a rule for its base holds for it
        p.set_equip_bonus(copy, 1000, 300)
        p.set_equip_bonus(651, 500, 500)                             # the disc's: nothing to write
        self.assertEqual(p.equip_bonus, {g.MEGAMORPH: (2500, 2500), copy: (1000, 300)})
        self.assertTrue(p.card_changed(g.MEGAMORPH))
        data = manifest.build(p)
        self.assertEqual([e for e in data["equips"] if "card" in e and any(k.startswith("bonus") for k in e)],
                         [{"card": p.ref(g.MEGAMORPH), "bonus": 2500},
                          {"card": p.ref(copy), "bonus_attack": 1000, "bonus_defense": 300}])
        again = Project(p.retail)
        self.assertEqual(manifest.apply(again, data), [])
        self.assertEqual(again.equip_bonus, p.equip_bonus)
        self.assertEqual(manifest.build(again), data)
        again.revert_card(g.MEGAMORPH)
        self.assertEqual(again.equip_bonus, {copy: (1000, 300)})
        again.remove_card(copy)
        self.assertNotIn("equips", manifest.build(again))

    def test_bonus_keys_and_bonus_if(self):
        p = self.project
        data = manifest.build(p)
        data["equips"] = [{"card": 651, "bonus_if": {"Dragon": 900}}, {"card": 652, "bonus_if": {"Dragon": 900}},
                          {"card": 651, "bonus": 700}, {"card": 653, "bonus": 800, "bonus_if": {"Fiend": 100}},
                          {"card": 654, "bonus_defense": 1200}, {"card": 655, "bonus": 100, "bonus_attack": 50}]
        self.assertEqual(manifest.apply(p, data), [])
        self.assertEqual(p.equip_bonus, {651: (700, 700), 653: (800, 800), 654: (500, 1200), 655: (50, 100)})
        # 651's "bonus_if" came before a "bonus" for every monster: it did nothing.
        self.assertEqual(p.kept["equips"], [{"card": 652, "bonus_if": {"Dragon": 900}},
                                            {"card": 653, "bonus_if": {"Fiend": 100}}])
        self.assertEqual(manifest.build(p)["equips"],
                         [{"card": p.ref(651), "bonus": 700}, {"card": p.ref(653), "bonus": 800},
                          {"card": p.ref(654), "bonus_attack": 500, "bonus_defense": 1200},
                          {"card": p.ref(655), "bonus_attack": 50, "bonus_defense": 100},
                          {"card": 652, "bonus_if": {"Dragon": 900}}, {"card": 653, "bonus_if": {"Fiend": 100}}])
        # A bonus the game would refuse stays as written.
        again = Project(p.retail)
        data["equips"] = [{"card": 651, "bonus_defense": "lots"}]
        manifest.apply(again, data)
        self.assertEqual((again.equip_bonus, again.kept["equips"]), ({}, [{"card": 651, "bonus_defense": "lots"}]))


class PasswordTest(unittest.TestCase):
    """A card's password in the Cards tab: a disc card's goes in "passwords"
    (the Password screen's), an added card's in its entry (the card view's)."""

    def setUp(self):
        self.retail = fixture().game()

    def project(self) -> Project:
        p = Project(self.retail)
        p.info.id = "pw"
        return p

    def test_disc_table(self):
        from fm_editor.tests.fixtures import password_of
        self.assertEqual(self.retail.passwords[1], password_of(1))
        self.assertEqual(self.retail.passwords[g.CARD_COUNT], "")
        self.assertEqual(g.read_passwords(b"short"), {})

    def test_port_spellings(self):
        self.assertEqual(manifest.password_text("8124921"), "08124921")
        self.assertEqual(manifest.password_text(1), "00000001")
        self.assertEqual(manifest.password_text(""), "")
        self.assertEqual(manifest.password_text(None), "")
        for kept in ("card number", "123456789", "12a", True, -1, 100000000):
            self.assertIsNone(manifest.password_text(kept), kept)

    def test_added_card_round_trip(self):
        p = self.project()
        cid = p.add_card(3, "mine")
        p.set_password(cid, "00001234")
        data = manifest.build(p)
        self.assertEqual(data["cards"][0]["password"], "00001234")
        self.assertNotIn("passwords", data)
        again = self.project()
        self.assertEqual(manifest.apply(again, json.loads(manifest.dumps(data))), [])
        self.assertEqual(again.password(cid), "00001234")
        self.assertEqual(again.added[cid].extra, {})
        self.assertEqual(manifest.build(again), data)
        fresh = again.add_card(3, "other")
        self.assertEqual(again.password(fresh), "")     # a copy has none of its own

    def test_disc_card_round_trip(self):
        p = self.project()
        p.set_password(1, "00000001")
        p.set_password(2, "")                       # the screen cannot give it
        p.set_password(4, self.retail.passwords[4])  # the disc's: nothing to write
        data = manifest.build(p)
        self.assertEqual(data["passwords"], {"Blue Dragon": {"password": "00000001"},
                                             "Mystic Elf": {"password": ""}})
        again = self.project()
        self.assertEqual(manifest.apply(again, data), [])
        self.assertEqual((again.password(1), again.password(2)), ("00000001", ""))
        self.assertTrue(again.card_changed(1))
        self.assertFalse(again.card_changed(4))
        self.assertEqual(manifest.build(again), data)
        again.revert_card(1)
        self.assertEqual(manifest.build(again)["passwords"], {"Mystic Elf": {"password": ""}})

    def test_kept_as_written(self):
        from fm_editor.tests.fixtures import password_of
        written = {"id": "pw", "name": "pw", "passwords": {
            "all": {"password": "card number", "starchips_percent": 10},
            "Kuriboh": {"password": 77, "starchips": 5},
            "Card 5": {"password": "card number"},
            "Blue Dragon": {"password": password_of(1)},    # its own, out of "all"
            "pw:mine:1": {"password": "00000009"}},
            "cards": [{"copy": 3, "id": "mine", "password": "letters"}]}
        p = self.project()
        messages = manifest.apply(p, json.loads(json.dumps(written)))
        self.assertTrue(any("\"password\" is up to 8 digits" in m for m in messages), messages)
        self.assertEqual(p.password(3), "00000077")
        cid = next(iter(p.added))
        self.assertEqual(p.added[cid].extra, {"password": "letters"})
        p.set_password(3, "00000078")
        data = manifest.build(p)
        self.assertEqual(data["passwords"], {"all": {"password": "card number", "starchips_percent": 10},
                                             "Kuriboh": {"starchips": 5, "password": "00000078"},
                                             "Card 5": {"password": "card number"},
                                             "Blue Dragon": {"password": password_of(1)},
                                             "pw:mine:1": {"password": "00000009"}})
        self.assertEqual(data["cards"][0]["password"], "letters")

    def test_validation(self):
        p = self.project()
        a, b = p.add_card(3, "a"), p.add_card(3, "b")
        p.set_password(a, self.retail.passwords[10])     # a disc card's
        p.set_password(b, "00000042")
        p.set_password(1, "00000042")                    # an added card's
        p.set_password(2, "12x")
        found = {(i.target, i.message) for i in validate.validate(p) if i.level == "error"}
        text = "\n".join(m for _, m in found)
        self.assertIn(f"password {self.retail.passwords[10]} is also 10 Card 10's", text)
        self.assertIn(f"password 00000042 is also {b} Kuriboh's", text)
        self.assertIn("a password is up to 8 digits", text)
        p.set_password(a, "00000043")
        p.set_password(b, "")
        p.set_password(2, self.retail.passwords[1])      # swapped with card 1, which moved away
        self.assertEqual(validate.errors(validate.validate(p)), [])



class DumpsTest(unittest.TestCase):
    def test_a_number_key_is_written_as_a_string(self):
        """A card named by its number (two share its name) is a key like any:
        a long object laid over lines wrote it bare, which no JSON reader
        takes."""
        written = manifest.dumps({"starter_pools": [{"draws": 40, "cards": {
            **{f"Card name number {n}": n for n in range(12)}, 480: 5}}]})
        self.assertEqual(json.loads(written)["starter_pools"][0]["cards"]["480"], 5)


if __name__ == "__main__":
    unittest.main()


class MonsterEffectsTest(unittest.TestCase):
    """A card's "monster_effects" (monster_effects.py): as the game reads
    them, kept in the card's entry, inherited by a copy, checked."""

    def setUp(self):
        self.project = Project(fixture().game())

    def test_normalize_and_allowed(self):
        from fm_editor import monster_effects as fx
        self.assertEqual(fx.normalize({"when": "Face Up", "do": "boost", "target": "others", "type": "Dragon",
                                       "attack": 300}),
                         {"when": "face_up", "do": "boost", "target": "others", "attack": 300, "type": "Dragon"})
        self.assertEqual(fx.normalize({"when": "summon", "do": "magic", "card": 337}),
                         {"when": "summon", "do": "magic", "card": 337})
        # No trimming, as monster_effects.c named(): "summon " is no name.
        self.assertIsNone(fx.normalize({"when": "summon ", "do": "heal", "amount": 100}))
        # An entry that is not an object is kept as written (the game notes and skips it).
        self.project.set_monster_effects(1, ["summon", {"when": "summon", "do": "heal", "amount": 100}])
        self.assertEqual(self.project.monster_effects_of(1)[0][0], "summon")
        # What the game leaves out (monster_effects.c MonsterEffect_Allowed).
        for effect in ({"when": "face_up", "do": "magic", "card": 337}, {"when": "combat", "do": "magic", "card": 337},
                       {"when": "combat", "do": "boost", "target": "own", "attack": 1},
                       {"when": "destroyed", "do": "boost", "target": "self", "attack": 1},
                       {"when": "summon", "do": "boost", "target": "battle", "attack": 1},
                       {"when": "summon", "do": "magic", "card": 675},      # a ritual
                       {"when": "summon", "do": "boost"}, {"when": "summon", "do": "heal", "amount": 0},
                       {"when": "later", "do": "heal", "amount": 1}, {"when": "summon", "do": "heal", "amount": True},
                       {"when": "summon", "do": "destroy", "target": "own"},
                       {"when": "destroyed", "do": "destroy", "target": "battle"},
                       {"when": "combat", "do": "destroy", "target": "battle"}):
            self.assertIsNone(fx.normalize(effect), effect)
        # The target left out: the card itself, its side when destroyed.
        self.assertEqual(fx.normalize({"when": "destroyed", "do": "boost", "attack": 100})["target"], "own")
        self.assertEqual(fx.actions("face_up"), ["boost", "battle_immunity"])
        self.assertEqual(fx.normalize({"when": "face_up", "do": "battle_immunity"}),
                         {"when": "face_up", "do": "battle_immunity", "target": "self"})
        self.assertEqual(fx.describe({"when": "face_up", "do": "battle_immunity"}),
                         "Cannot be destroyed by battle")
        # Battle immunity cannot be applied to other triggers like combat or summon
        self.assertIsNone(fx.normalize({"when": "combat", "do": "battle_immunity"}))
        self.assertIsNone(fx.normalize({"when": "summon", "do": "battle_immunity"}))
        # Conditions normalization and description
        cond_card = fx.normalize_condition({"type": "named_card", "card": 1, "whose": "own"})
        self.assertEqual(cond_card, {"type": "named_card", "card": 1, "whose": "own"})
        cond_atk = fx.normalize_condition({"type": "stat_threshold", "stat": "atk", "comparison": "<", "value": 1500})
        self.assertEqual(cond_atk, {"type": "stat_threshold", "stat": "atk", "comparison": "<", "value": 1500})
        cond_lvl = fx.normalize_condition({"type": "stat_threshold", "stat": "level", "comparison": ">=", "value": 7})
        self.assertEqual(cond_lvl, {"type": "stat_threshold", "stat": "level", "comparison": ">=", "value": 7})
        self.assertEqual(fx.condition_words(cond_lvl), "Level >= 7")
        self.assertEqual(fx.normalize_condition({"type": "stat_threshold", "stat": "stars", "comparison": "<=", "value": 4})["stat"], "level")
        self.assertIsNone(fx.normalize_condition({"type": "stat_threshold", "stat": "level", "value": 13}))
        self.assertIsNone(fx.normalize_condition({"type": "stat_threshold", "stat": "level", "value": 0}))
        eff_cond = fx.normalize({"when": "face_up", "do": "battle_immunity",
                                 "conditions": [{"type": "lowest_atk"},
                                                {"type": "first_attacked", "reset": "turn"}],
                                 "condition_mode": "any"})
        self.assertEqual(eff_cond["conditions"], [{"type": "lowest_atk"}, {"type": "first_attacked", "reset": "turn"}])
        self.assertEqual(eff_cond["condition_mode"], "any")
        self.assertEqual(fx.describe(eff_cond),
                         "Cannot be destroyed by battle (if lowest ATK on field or the first time attacked (this turn))")
        cond_second = fx.normalize_condition({"type": "destroyed_after_2_attacks", "reset": "duel"})
        self.assertEqual(cond_second, {"type": "destroyed_after_2_attacks", "reset": "duel"})


        self.assertEqual(fx.describe({"when": "face_up", "do": "battle_immunity", "conditions": [cond_second]}),
                         "Cannot be destroyed by battle (if can only be destroyed after two attacks (this duel))")
        cond_third = fx.normalize_condition({"type": "destroyed_after_3_attacks", "reset": "copy"})
        self.assertEqual(cond_third, {"type": "destroyed_after_3_attacks", "reset": "copy"})
        self.assertEqual(fx.describe({"when": "face_up", "do": "battle_immunity", "conditions": [cond_third]}),
                         "Cannot be destroyed by battle (if can only be destroyed after three attacks (this card copy))")
        # Aliases
        self.assertEqual(fx.normalize_condition({"type": "second_attacked"})["type"], "destroyed_after_2_attacks")
        self.assertEqual(fx.normalize_condition({"type": "third_attacked"})["type"], "destroyed_after_3_attacks")
        self.assertEqual(fx.describe({"when": "face_up", "do": "battle_immunity", "conditions": [{"type": "first_attacked"}]}),
                         "Cannot be destroyed by battle (if the first time attacked (this card copy))")
        self.assertEqual(fx.targets("combat"), ["self", "battle"])
        # A destroy: the opponent's, or on a flip the monster attacking it (its default there).
        self.assertEqual(fx.targets("flip", "destroy"), ["opponent", "battle"])
        self.assertEqual(fx.targets("destroy_opponent", "destroy"), ["opponent"])
        self.assertEqual(fx.normalize({"when": "flip", "do": "destroy"}),
                         {"when": "flip", "do": "destroy", "target": "battle"})
        self.assertEqual(fx.describe({"when": "flip", "do": "destroy", "target": "battle"}),
                         "Destroy the monster it battles")
        self.assertNotIn("destroy", fx.actions("combat"))
        self.assertEqual(fx.describe({"when": "combat", "do": "boost", "target": "battle", "attack": -500}),
                         "The monster it battles: -500 ATK for the battle")

    def test_round_trip_copies_and_checks(self):
        p = self.project
        effects = [{"when": "summon", "do": "magic", "card": 337},
                   {"when": "face_up", "do": "boost", "target": "others", "attack": 300, "defense": 300}]
        p.set_monster_effects(1, effects)
        copy = p.add_card(1, "copy")
        self.assertEqual(p.monster_effects_of(copy), (effects, True))       # its base's
        none = p.add_card(1, "none")
        p.set_monster_effects(none, [])
        self.assertEqual(p.monster_effects_of(none), ([], False))           # none at all, not its base's
        self.assertTrue(p.card_changed(1))
        data = manifest.build(p)
        entries = {json.dumps(e.get("replace", e.get("id"))): e for e in data["cards"]}
        self.assertEqual(entries["1"]["monster_effects"], effects)
        self.assertNotIn("monster_effects", entries['"copy"'])
        self.assertEqual(entries['"none"']["monster_effects"], [])
        again = Project(p.retail)
        self.assertEqual(manifest.apply(again, data), [])
        self.assertEqual(manifest.build(again), data)
        self.assertEqual(again.monster_effects_of(1), (effects, False))
        # Taking a disc card's away leaves no entry; reverting the card does too.
        p.set_monster_effects(1, [])
        self.assertNotIn(1, p.card_extra)
        p.set_monster_effects(1, effects)
        p.revert_card(1)
        self.assertEqual(p.monster_effects_of(1), ([], False))
        # What the game would not take is an error.
        p.set_monster_effects(2, [{"when": "face_up", "do": "damage", "amount": 100}])
        self.assertTrue(any("not one the game takes" in i.message and i.level == "error"
                            for i in validate.validate_card(p, 2)))
        p.set_monster_effects(2, [{"when": "summon", "do": "heal", "amount": 100}] * 9)
        self.assertTrue(any("at most 8" in i.message for i in validate.validate_card(p, 2)))

    def test_for_each(self):
        """"for_each": a boost, heal or damage made once per face-up monster
        counted, as monster_effects.c read_each() takes it."""
        from fm_editor import monster_effects as fx
        boost = {"when": "face_up", "do": "boost", "target": "self", "attack": 300, "defense": 300,
                 "for_each": {"whose": "Own", "type": "dragon"}}
        self.assertEqual(fx.normalize(boost)["for_each"], {"whose": "own", "type": "Dragon"})
        self.assertEqual(fx.describe(fx.normalize(boost)),
                         "This card: +300 ATK +300 DEF for each face-up Dragon on its owner's field")
        # "whose" left out: both sides'.
        heal = {"when": "summon", "do": "heal", "amount": 200, "for_each": {"attribute": "Light"}}
        self.assertEqual(fx.normalize(heal)["for_each"], {"whose": "all", "attribute": "Light"})
        self.assertEqual(fx.describe(fx.normalize(heal)),
                         "Its owner gains 200 LP for each face-up Light monster on the field")
        combat = {"when": "combat", "do": "boost", "target": "battle", "attack": -100,
                  "for_each": {"whose": "opponent", "type": "Dragon", "attribute": "Light"}}
        self.assertEqual(fx.describe(fx.normalize(combat)),
                         "The monster it battles: -100 ATK for each face-up Light Dragon on the opponent's field "
                         "for the battle")
        # Not on a magic or destroy, nor whose it does not know, nor a Trap counted.
        for effect in ({"when": "summon", "do": "magic", "card": 337, "for_each": {}},
                       {"when": "summon", "do": "destroy", "for_each": {}},
                       {"when": "summon", "do": "heal", "amount": 1, "for_each": {"whose": "mine"}},
                       {"when": "summon", "do": "heal", "amount": 1, "for_each": "Dragon"},
                       {"when": "summon", "do": "heal", "amount": 1, "for_each": None},
                       {"when": "summon", "do": "heal", "amount": 1, "for_each": {"type": "Trap"}}):
            self.assertIsNone(fx.normalize(effect), effect)
        # Kept through the manifest, checked by validate.
        p = self.project
        p.set_monster_effects(1, [fx.normalize(boost), fx.normalize(heal)])
        data = manifest.build(p)
        again = Project(p.retail)
        self.assertEqual(manifest.apply(again, data), [])
        self.assertEqual(again.monster_effects_of(1)[0], [fx.normalize(boost), fx.normalize(heal)])
        self.assertEqual([i.message for i in validate.validate_card(again, 1) if i.level == "error"], [])
        p.set_monster_effects(2, [{"when": "summon", "do": "destroy", "for_each": {}}])
        self.assertTrue(any("not one the game takes" in i.message for i in validate.validate_card(p, 2)))

    def test_counted_and_named_as_the_game_does(self):
        from fm_editor import monster_effects as fx
        heal = {"when": "summon", "do": "heal", "amount": 100}
        bad = {"when": "face_up", "do": "damage", "amount": 100}
        # One the game leaves out takes no place: the 9th is the 8th it takes.
        found = fx.problems([heal, bad] + [heal] * 7)
        self.assertEqual(len(found), 1)
        self.assertIn("effect 2 is not", found[0])
        found = fx.problems([heal] * 8 + [bad, heal])
        self.assertEqual(found, ["at most 8 monster effects; the rest (from effect 9) are left out"])
        # Type and attribute names in any case and spacing (cards.c same_letters).
        for t, a in (("dragon", "light"), ("WingedBeast", "LIGHT"), ("winged-beast", " Dark ")):
            out = fx.normalize({"when": "face_up", "do": "boost", "type": t, "attribute": a, "attack": 100})
            self.assertIsNotNone(out, t)
            self.assertIn(out["type"], ("Dragon", "Winged Beast"))
        for t in (True, "Magic", "dragons", ""):
            self.assertIsNone(fx.normalize({"when": "face_up", "do": "boost", "type": t, "attack": 1}), t)

    def test_equip_made_a_monster_has_no_equips_error(self):
        p = self.project
        equip = next(cid for cid in sorted(p.retail.equips) if p.retail.equips[cid])
        copy = p.add_card(equip, "equip-monster")
        for cid in (equip, copy):
            p.cards[cid].type, p.cards[cid].attack, p.cards[cid].star1, p.cards[cid].star2 = 0, 1000, 1, 2
        self.assertEqual([i.message for i in validate.validate(p) if i.area == "Equips"], [])
        # A list the mod changes for it is still one for a card that is no equip.
        p.equips[copy] = {next(m for m in p.monsters() if m not in p.retail.equips[equip])}
        self.assertTrue(any("not an equip card" in i.message for i in validate.validate(p) if i.area == "Equips"))

    def test_disc_card_keeps_none_only_when_asked(self):
        p = self.project
        p.set_monster_effects(2, [], keep_empty=True)
        self.assertEqual(manifest.build_cards(p), [{"replace": 2, "monster_effects": []}])
        p.set_monster_effects(2, [])
        self.assertEqual(manifest.build_cards(p), [])

    def test_copy_of_a_magic_card_made_a_monster(self):
        p = self.project
        magic = next(cid for cid, card in sorted(p.cards.items()) if card.type == g.TYPE_MAGIC)
        cid = p.add_card(magic, "magic-monster")
        card = p.cards[cid]
        card.type, card.attribute, card.attack, card.defense, card.level, card.star1, card.star2 = 0, 0, 1500, 1200, 4, 1, 2
        p.set_monster_effects(cid, [{"when": "summon", "do": "heal", "amount": 500}])
        found = validate.validate_card(p, cid)
        self.assertEqual([i.message for i in found if i.level == "error"], [])
        self.assertTrue(any("without a 3D model" in i.message for i in found))
        p.added[cid].extra["model"] = 1
        self.assertFalse(any("3D model" in i.message for i in validate.validate_card(p, cid)))
        data = manifest.build(p)
        entry = next(e for e in data["cards"] if e.get("id") == "magic-monster")
        self.assertEqual(entry["type"], "Dragon")
        again = Project(p.retail)
        self.assertEqual(manifest.apply(again, data), [])
        self.assertEqual(again.cards[cid].type, 0)
        self.assertEqual(again.monster_effects_of(cid)[0], [{"when": "summon", "do": "heal", "amount": 500}])
        # Made another non-monster without a matching effect: still refused.
        card.type = g.TYPE_TRAP
        self.assertTrue(any("matching retail effect" in i.message for i in validate.validate_card(p, cid)))
        self.assertNotIn("type", next(e for e in manifest.build(p)["cards"] if e.get("id") == "magic-monster"))
