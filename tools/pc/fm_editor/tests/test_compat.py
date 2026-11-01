"""Which game a mod needs: "min_api" from the features its mod.json uses
(compat.py), and the editor's mod API against the game's.

    python -m unittest discover -s tools/pc/fm_editor/tests -t tools/pc
"""
import re
import json
import tempfile
import unittest
from pathlib import Path

from fm_editor import compat, gamedata as g, manifest, monster_effects, validate
from fm_editor.model import Project
from fm_editor.tests.fixtures import Fixture

ROOT = Path(__file__).resolve().parents[4]
MOD_TYPES = ROOT / "src/pc/mods/mod_types.h"
FIXTURE = None


def fixture() -> Fixture:
    global FIXTURE
    if FIXTURE is None:
        FIXTURE = Fixture()
    return FIXTURE


class HostApiTest(unittest.TestCase):
    @unittest.skipUnless(MOD_TYPES.exists(), "no source tree (the packaged editor)")
    def test_matches_the_game(self):
        """The editor's mod API is the game's (MEMORIES_MOD_API), so a mod
        the game takes is not called too new, nor one it refuses fine."""
        found = re.search(r"^#define\s+MEMORIES_MOD_API\s+(\d+)\s*$", MOD_TYPES.read_text(), re.M)
        self.assertIsNotNone(found, "MEMORIES_MOD_API is not in mod_types.h")
        self.assertEqual(compat.HOST_API, int(found.group(1)),
                         "fm_editor/compat.py HOST_API differs from src/pc/mods/mod_types.h MEMORIES_MOD_API")
        self.assertIs(validate.HOST_API, compat.HOST_API)

    def test_every_feature_is_within_the_host(self):
        self.assertLessEqual(max(compat.RELEASES), compat.HOST_API)

    def test_validate_takes_the_host_api(self):
        p = Project(fixture().game())
        p.info.id = "api"
        for api, error in ((9, False), (10, False), (compat.HOST_API, False), (compat.HOST_API + 1, True)):
            p.other["min_api"] = api
            issues = [i for i in validate.validate(p) if i.where == "min_api"]
            self.assertEqual(bool(issues), error, api)


class FeatureTest(unittest.TestCase):
    """Each feature newer than v0.2.0 (API 9), alone, and the API it needs."""

    def needs(self, manifest_value: dict, project=None):
        return compat.required(manifest_value, project)[0]

    def test_nothing_new(self):
        old = {"id": "old", "cards": [{"replace": 1, "name": "X", "frame": "Purple", "notes": "n"}],
               "fusions": [{"with": [1, 2], "result": 3}, {"remove": [1, 2]}],
               "rituals": [{"card": 690, "tributes": [1, 2, 3], "result": 4}, {"card": 691, "result": None}],
               "equips": [{"card": 650, "bonus": 300}], "limits": {"attack": 30000, "life_points": 16000},
               "title": {"entries": {}}, "menu": {"entries": {}}, "starter_pools": [], "duelists": [{"copy": 1}],
               "packs": [{"name": "P"}], "trap_thresholds": {}}
        self.assertEqual(compat.required(old), (0, []))
        self.assertNotIn("min_api", compat.stamp(dict(old)))

    def test_api_10(self):
        for value in ({"card_text_colors": {}},
                      {"cards": [{"replace": 5, "monster_effects": []}]},
                      {"cards": [{"replace": 690, "trap_threshold": 1500}]},
                      {"cards": [{"copy": 5, "id": "c", "model": "Blue Dragon"}]},
                      {"cards": [{"copy": 610, "id": "c", "effect": "Card 611"}]},
                      {"equips": [{"card": 650, "bonus_attack": 300, "bonus_defense": 0}]},
                      {"packs": [{"name": "P", "image_style": "full"}]}):
            with self.subTest(value=value):
                self.assertEqual(self.needs(value), 10)

    def test_api_11(self):
        for value in ({"assets": "assets"}, {"ui": {"duel": {}}}, {"card_layout": {}}, {"palette_ramps": {"0": "#FF0000"}},
                      {"title": {"images": []}}, {"menu": {"scale": 120}},
                      {"menu": {"buttons": [{"id": "b", "scale": 80}]}},
                      {"title": {"entries": {"logo": {"scale": 50}}}},
                      {"cards": [{"replace": 1, "tags": ["dragon"]}]},
                      {"cards": [{"replace": 1, "frame": "Gold"}]}, {"cards": [{"replace": 1, "frame": "blue"}]},
                      {"cards": [{"replace": 1, "monster_effects": [
                          {"when": "face_up", "do": "boost", "attack": 300, "for_each": {"whose": "own"}}]}]},
                      {"fusions": [{"remove": "all"}]},
                      {"rituals": [{"card": 690, "tributes": [1, 2], "result": 4}]},
                      {"rituals": [{"card": 690, "tributes": [1, 2, 3, 4, 5], "result": 4}]},
                      {"rituals": [{"card": 690, "tributes": [1, 2, 3], "tributes_from": "hand", "result": 4}]},
                      {"limits": {"deck_copies": 5}}, {"limits": {"attack": 9999, "new_game_starchips": 10}}):
            with self.subTest(value=value):
                self.assertEqual(self.needs(value), 11)

    def test_api_13(self):
        """A mod's lock on the cards a won duel deals: a game of API 12 or
        older would leave it out and deal the player's count."""
        self.assertEqual(self.needs({"limits": {"card_drops": 5}}), 13)
        self.assertEqual(self.needs({"limits": {"card_drops": 5, "deck_copies": 4}}), 13)
        api, reasons = compat.required({"limits": {"card_drops": 99, "attack": 9999}})
        self.assertEqual(api, 13)
        self.assertEqual(len(reasons), 1)
        self.assertIn("card_drops", reasons[0])

    def test_api_14(self):
        """The tower (notes/tower-mode.md): a game of API 12 (v0.3.0) would
        leave each of these out."""
        for value in ({"tower": {"floors": [{"duelist": 1}]}},
                      {"duelists": [{"id": "g", "copy": 1, "tower_only": True}]},
                      {"duelists": [{"id": "g", "copy": 1, "unlock": {"tower": 2}}]},
                      {"menu": {"buttons": [{"id": "t", "label": "T", "menu": "second", "action": "tower"}]}}):
            with self.subTest(value=value):
                self.assertEqual(self.needs(value), 14)
        self.assertEqual(self.needs({"duelists": [{"id": "g", "copy": 1, "unlock": {"beat": 2}}]}), 0)
        self.assertIn("newer than v0.3.0", compat.release_text(14))

    def test_api_15_card_types(self):
        value = {"card_types": {"types": [{"id": 0, "name": "Wyrm"}, {"id": 24, "name": "Cyberse"}]}}
        self.assertEqual(self.needs(value), 15)
        p = Project(fixture().game())
        p.other.update(value)
        self.assertFalse([issue for issue in validate.validate(p) if issue.level == "error"])

    def test_custom_type_name_survives_import_and_save(self):
        p = Project(fixture().game())
        manifest.apply(p, {"card_types": {"types": [{"id": 24, "name": "Cyberse"}]},
                           "cards": [{"replace": 1, "type": "Cyberse"}]})
        self.assertEqual(p.cards[1].type, 24)
        self.assertEqual(manifest.build(p)["cards"][0]["type"], "Cyberse")

    def test_type_import_uses_runtime_collisions_aliases_and_equips(self):
        p = Project(fixture().game())
        messages = manifest.apply(p, {
            "card_types": {"types": [
                {"id": 0, "name": "Wyrm"},
                {"id": 24, "name": "Cyberse"},
                {"id": 25, "name": "Dragon"},
            ]},
            "cards": [{"replace": 1, "type": "Dragon"}],
            "equips": [{"card": 660, "add": ["Cyberse"]}],
        })
        self.assertEqual(p.type_names[0], "Wyrm")
        self.assertEqual(p.type_names[24], "Cyberse")
        self.assertIsNone(p.type_names[25])
        self.assertEqual(p.cards[1].type, 0)       # Dragon is Wyrm's retail alias.
        self.assertEqual(manifest._type_choice(p, "Cyber-Se"), 24)
        self.assertEqual(manifest._type_choice(p, 25), -1)
        self.assertTrue(any("already type 0" in message for message in messages))
        self.assertIn((660, manifest.EQUIP_TYPE, 24, True, 1),
                      manifest.equip_rules(p, [{"card": 660, "add": ["Cyberse"]}], []))

    def test_type_name_with_no_glyph_is_never_registered(self):
        # encode_type_name (cards.c) rejects the whole declaration, not just
        # the letters it cannot show: the editor's model must agree, or a
        # card assigned to this id here would point at nothing at runtime.
        p = Project(fixture().game())
        messages = manifest.apply(p, {"card_types": {"types": [{"id": 24, "name": "\U0001F600"}]}})
        self.assertIsNone(p.type_names[24])
        self.assertTrue(any("no glyph" in message for message in messages))

    def test_type_reference_accepts_hex_like_the_runtime(self):
        # choice() (cards.c) falls back to Json_Number for an unmatched
        # string, which reads "0x18" as hex the same as any other number.
        p = Project(fixture().game())
        manifest.apply(p, {"card_types": {"types": [{"id": 24, "name": "Cyberse"}]}})
        self.assertEqual(manifest._type_choice(p, "0x18"), 24)

    def test_api_16_attributes(self):
        self.assertEqual(self.needs({"attributes": {"attributes": [{"id": 8, "name": "Metal"}]}}), 16)

    def test_monster_effect_filter_on_a_custom_type(self):
        # monster_effects.c filter(): a declared monster type filters, 255
        # (the filter's "any" byte) and an undeclared id do not.
        # MonsterCondition.card_type and MonsterEffectEx.attacker_type are
        # signed short (-1 = any), so custom types 128-255 (including 255) work.
        p = Project(fixture().game())
        manifest.apply(p, {"card_types": {"types": [{"id": 200, "name": "Titan"}, {"id": 255, "name": "Omega"}]}})
        boost = {"when": "face_up", "do": "boost", "attack": 100}
        imm = {"when": "face_up", "do": "battle_immunity"}
        pierce = {"when": "face_up", "do": "piercing"}
        for ok in ({**boost, "type": "Titan"}, {**boost, "type": 200}, {**boost, "type": "Dragon"},
                   {**boost, "for_each": {"type": "titan"}},
                   {**imm, "conditions": [{"type": "control_type", "card_type": 200}]},
                   {**imm, "conditions": [{"type": "control_type", "card_type": 255}]},
                   {**imm, "modifier": "attacked_by_type", "attacker_type": 200},
                   {**imm, "modifier": "attacked_by_type", "attacker_type": "Omega"},
                   {**imm, "modifier": "attacked_by_monster", "attacker_type": 200},
                   {**pierce, "modifier": "attacks_type", "defender_type": 200},
                   {**pierce, "modifier": "attacks_monster", "defender_type": "Omega"}):
            with self.subTest(effect=ok):
                self.assertEqual(monster_effects.problems([ok], p.resolve, type_names=p.type_names), [])
        self.assertEqual(monster_effects.normalize({**boost, "type": 200}, p.resolve, type_names=p.type_names)["type"], "Titan")
        self.assertEqual(monster_effects.normalize_condition({"type": "control_type", "card_type": 200}, p.resolve, type_names=p.type_names)["card_type"], "Titan")
        self.assertEqual(monster_effects.normalize({**imm, "modifier": "attacked_by_type", "attacker_type": 200}, p.resolve, type_names=p.type_names)["attacker_type"], "Titan")
        self.assertEqual(monster_effects.normalize({**pierce, "modifier": "attacks_type", "defender_type": 200}, p.resolve, type_names=p.type_names)["defender_type"], "Titan")
        for bad in ({**boost, "type": "Omega"}, {**boost, "type": 201}, {**boost, "type": "Magic"},
                    {**imm, "conditions": [{"type": "control_type", "card_type": 201}]},
                    {**imm, "modifier": "attacked_by_type", "attacker_type": 201},
                    {**pierce, "modifier": "attacks_type", "defender_type": 201}):
            with self.subTest(effect=bad):
                self.assertTrue(monster_effects.problems([bad], p.resolve, type_names=p.type_names))

    def test_api_17(self):
        """A duelist's bust (notes/more-duelists.md): a game of API 16 would
        show the campaign's characters instead."""
        for value in ({"duelists": [{"id": "g", "copy": 1, "bust": "busts/g.png"}]},
                      {"duelists": [{"replace": "Seto", "bust": "busts/s.png", "replace_bust": True}]},
                      {"tower": {"floors": [{"duelist": 1}], "intro": {
                          "characters": {"left": {"duelist": "g", "expression": "angry"}}, "lines": ["Hi."]}}}):
            with self.subTest(value=value):
                self.assertEqual(self.needs(value), 17)
        self.assertIn("newer than v0.3.0", compat.release_text(17))

    def test_api_18(self):
        """The card frames' keys (notes/modding.md, "Frame styles"): a game of
        API 17 reads none of them, so it would draw the retail frame or the
        style's palette alone and play the mod half-loaded."""
        style = lambda **keys: {"card_layout": {"frame_styles": {"s": {"image": "a.png", **keys}}}}  # noqa: E731
        for value in ({"cards": [{"replace": 1, "frame_style": "god"}]},
                      {"card_layout": {"default_styles": {"kinds": {"monster": "gold"}}}},
                      {"card_layout": {"atk_label": {"x": 1, "y": 2}}},
                      {"card_layout": {"dfd_label": {"hide": True}}},
                      {"card_layout": {"type_label": {"trap": {"image": "t.png"}}}},
                      {"card_layout": {"atk": {"hide": True}}},
                      {"card_layout": {"stars": {"x": 1, "y": 2, "color": {"r": 255, "g": 96, "b": 96}}}},
                      {"card_layout": {"digits": {"image": "d.png", "atk": {"x": 38, "y": 179}}}},
                      style(hand_image="h.png"), style(field_image="f.png"), style(field_back_image="b.png"),
                      style(back_image="b.png"), style(library_icon="l.png"), style(hand={"art": {"x": 1}}),
                      style(full_bleed=False), style(atk_label={"x": 1}), style(type_label={}),
                      style(digits={"image": "d.png"}), style(title={"x": 12, "y": 14}),
                      style(atk={"hide": True}), style(art={"x": 3})):
            with self.subTest(value=value):
                self.assertEqual(self.needs(value), 18)
        # What a game of API 11 reads stays at 11.
        for value in ({"card_layout": {"frame_styles": {"g": {"image": "a.png", "hand_color": "gold",
                                                                   "width": 140, "height": 196}},
                                       "frame_for": [{"style": "g", "class": "monster"}], "default_style": "g",
                                       "art": {"x": 3}, "atk": {"x": 38, "y": 179}, "stars": {"x": 59, "y": 153},
                                       "digits": {"image": "d.png", "width": 10, "height": 12, "step": 10}}},
                      {"card_layout": {"frame_styles": "not a table"}},
                      {"cards": [{"replace": 1, "frame": "Gold"}]}):
            with self.subTest(value=value):
                self.assertEqual(self.needs(value), 11)
        api, reasons = compat.required(style(hand_image="h.png", library_icon="l.png"))
        self.assertEqual((api, len(reasons)), (18, 2))
        self.assertIn("newer than v0.3.0", compat.release_text(18))

    def test_api_19(self):
        """Battle immunity, piercing and conditions: a game of API 18 notes the
        unknown "do" and drops the effect, and plays conditions as if absent."""
        effect = lambda **keys: {"cards": [{"replace": 1, "monster_effects": [{"when": "face_up", **keys}]}]}  # noqa: E731
        for value in (effect(do="battle_immunity"), effect(do="piercing"),
                      effect(do="boost", attack=100, conditions=[{"type": "lower_lp"}])):
            with self.subTest(value=value):
                self.assertEqual(self.needs(value), 19)
        self.assertEqual(self.needs(effect(do="boost", attack=100)), 10)
        # Keys/actions the game does not read ("piercing_damage", singular "condition") do not bump to 19
        self.assertEqual(self.needs(effect(do="piercing_damage")), 10)
        self.assertEqual(self.needs(effect(do="boost", attack=100, condition={"type": "lower_lp"})), 10)

    def test_reasons_are_the_highest_apis(self):
        api, reasons = compat.required({"card_text_colors": {}, "ui": {}, "limits": {"swords_turns": 5}})
        self.assertEqual(api, 11)
        self.assertEqual(len(reasons), 2)
        self.assertIn("swords_turns", reasons[1])

    def test_a_field_ritual_of_three_is_old(self):
        self.assertEqual(self.needs({"rituals": [{"card": 690, "tributes": [1, 2, 3], "tributes_from": "field",
                                                  "result": 4}]}), 0)

    def test_disc_frame_names_are_old(self):
        for frame in g.FRAME_NAMES + ["Type"]:
            self.assertEqual(self.needs({"cards": [{"replace": 1, "frame": frame}]}), 0, frame)

    def test_a_copy_of_another_kind(self):
        p = Project(fixture().game())
        monster, magic, equip = 5, 610, 660     # the fixture's types
        self.assertLess(p.cards[monster].type, g.TYPE_MAGIC)
        copy = lambda base, kind: {"cards": [{"copy": base, "id": "c", "type": kind}]}
        self.assertEqual(self.needs(copy(monster, "Dragon"), p), 0)        # a monster stays one
        self.assertEqual(self.needs(copy(monster, "Magic"), p), 10)
        self.assertEqual(self.needs(copy(magic, "Magic"), p), 0)
        self.assertEqual(self.needs(copy(magic, "Warrior"), p), 10)        # any card may become a monster
        self.assertEqual(self.needs(copy(equip, g.TYPE_MAGIC), p), 10)
        self.assertEqual(self.needs(copy(monster, "Magic")), 0)          # no project: the base is not known


class StampTest(unittest.TestCase):
    def setUp(self):
        self.p = Project(fixture().game())
        self.p.info.id = "stamp"

    def test_written_on_save_after_the_info(self):
        self.p.set_tags(1, ["dragon"])
        built = manifest.build(self.p)
        self.assertEqual(built["min_api"], 11)
        keys = list(built)
        self.assertTrue(set(keys[:keys.index("min_api")]) <= set(manifest.INFO_KEYS), keys)
        self.assertNotIn(keys[keys.index("min_api") + 1], manifest.INFO_KEYS)

    def test_retail_writes_none(self):
        self.assertNotIn("min_api", manifest.build(self.p))

    def test_never_lowered(self):
        self.p.equip_bonus[660] = (300, 100)
        self.p.other["min_api"] = 11
        self.assertEqual(manifest.build(self.p)["min_api"], 11)
        self.p.other["min_api"] = 4
        self.assertEqual(manifest.build(self.p)["min_api"], 10)
        self.assertEqual(self.p.other["min_api"], 4)    # what the author wrote stays theirs

    def test_not_a_number_is_left_to_validate(self):
        self.p.set_tags(1, ["dragon"])
        self.p.other["min_api"] = "11"
        self.assertEqual(manifest.build(self.p)["min_api"], "11")

    def test_round_trip(self):
        """Saved and opened again, the mod still says what it needs."""
        import tempfile
        self.p.set_tags(1, ["dragon"])
        with tempfile.TemporaryDirectory() as folder:
            manifest.save_mod(self.p, folder)
            data = manifest.read_json(Path(folder) / "mod.json")
            self.assertEqual(data["min_api"], 11)
            again = Project(fixture().game())
            manifest.apply(again, data)
            self.assertEqual(manifest.build(again)["min_api"], 11)

    def test_release_text(self):
        self.assertIn("v0.2.0", compat.release_text(9))
        self.assertIn("v0.2.1-preview.1 or newer", compat.release_text(10))
        self.assertIn("v0.3.0 or newer", compat.release_text(11))
        self.assertIn("newer than v0.3.0", compat.release_text(13))


class ExternalPacksTest(unittest.TestCase):
    def test_external_list_and_wrapper_survive_save_as(self):
        pack = {"id": "p", "image_style": "full"}
        for value in ([pack], {"packs": [pack], "pack_shop": {"open": False}}):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as temporary:
                source, target = Path(temporary) / "source", Path(temporary) / "target"
                source.mkdir()
                (source / "mod.json").write_text(json.dumps({"id": "external", "packs": "packs.json"}))
                contents = json.dumps(value).encode()
                (source / "packs.json").write_bytes(contents)
                p, _ = manifest.open_mod(fixture().game(), source)
                self.assertEqual(manifest.build(p)["min_api"], 10)
                manifest.save_mod(p, target)
                self.assertEqual((target / "packs.json").read_bytes(), contents)
                again, _ = manifest.open_mod(fixture().game(), target)
                self.assertEqual(manifest.build(again)["min_api"], 10)
                self.assertEqual(manifest.build(again)["packs"], "packs.json")

    def test_staged_file_wins_and_requirement_is_never_lowered(self):
        p = Project(fixture().game())
        p.packs_file = "packs.json"
        p.files["packs.json"] = b'[{"image_style":"full"}]'
        self.assertEqual(manifest.build(p)["min_api"], 10)
        p.other["min_api"] = 11
        self.assertEqual(manifest.build(p)["min_api"], 11)

    def test_unreadable_or_invalid_file_is_not_claimed_compatible(self):
        p = Project(fixture().game())
        p.packs_file = "missing.json"
        self.assertTrue(compat.problems(manifest.build(p), p))
        self.assertTrue(any(i.area == "Packs" and i.level == "error" for i in validate.validate(p)))
        for contents in (b'{', b'null', b'{"packs": "recursive.json"}'):
            p.files[p.packs_file] = contents
            self.assertTrue(compat.problems(manifest.build(p), p))
        # A syntax error is said as one, not as the wrong kind of value.
        p.files[p.packs_file] = b'[{"id": "p",]'
        self.assertEqual(compat.problems(manifest.build(p), p), ["packs file 'missing.json' is not valid JSON"])
        p.files[p.packs_file] = b'null'
        self.assertIn("must contain a pack list", compat.problems(manifest.build(p), p)[0])
        for name in ("../packs.json", "/packs.json", "C:\\packs.json", "..\\packs.json",
                     "packs\\list.json", "./packs.json", "packs//list.json", "packs/"):
            p.packs_file = name
            p.files[name] = b'[]'
            self.assertTrue(compat.problems(manifest.build(p), p))

    def test_game_json_syntax_and_old_packs(self):
        p = Project(fixture().game())
        p.packs_file = "packs.json"
        p.files[p.packs_file] = b'[{"image_style":"full",},]'
        self.assertEqual(manifest.build(p)["min_api"], 10)
        p.files[p.packs_file] = b'[{"id":"old"}]'
        self.assertNotIn("min_api", manifest.build(p))


if __name__ == "__main__":
    unittest.main()
