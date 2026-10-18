"""A duelist's bust (bust.py, notes/more-duelists.md "bust"): its pictures
read and written with the roster, the checks busts.c makes, its mod API,
the game's numbering and preview, the tower stories naming an expression,
and the Duelists tab's Bust section and the Tower tab's picker.

    python -m unittest discover -s tools/pc/fm_editor/tests -t tools/pc
"""
import json
import re
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fm_editor import bust as bustm, compat, manifest, pngio, roster, tower, validate
from fm_editor.model import Project
from fm_editor.tests.test_data import fixture
from fm_editor.tests.test_gui import GuiCase

W, H = 64, 96


def figure(mark, mouth=False, shut=False, w=W, h=H) -> bytes:
    """A figure on a clear ground: the test's own picture, by its colors."""
    out = bytearray()
    for y in range(h):
        for x in range(w):
            if shut and 20 <= y < 24 and 20 <= x < 44:
                out += bytes((10, 10, 10, 255))
            elif mouth and 34 <= y < 40 and 26 <= x < 38:
                out += bytes((250, 250, 250, 255))
            elif 4 <= y < 12 and 24 <= x < 40:
                out += bytes(mark) + b"\xff"
            elif (x - w // 2) ** 2 + (y - 26) ** 2 < 18 ** 2 or (y >= 46 and abs(x - w // 2) < 14 + (y - 46) // 3):
                out += bytes((200, 160, 120, 255))
            else:
                out += b"\x00\x00\x00\x00"
    return pngio.encode(pngio.Image(w, h, bytes(out)))


CALM, ANGRY, SAD = (40, 200, 60), (220, 40, 40), (40, 90, 230)


def three() -> bustm.Bust:
    b = bustm.short(figure(CALM), "calm")
    b.expressions[0].talk = [figure(CALM, mouth=True)]
    b.expressions[0].talk_paths = [None]
    b.expressions[0].blink = figure(CALM, shut=True)
    bustm.add(b, "angry", figure(ANGRY))
    bustm.add(b, "sad", figure(SAD))
    return b


class BustTest(unittest.TestCase):
    def setUp(self):
        self.p = Project(fixture().game())
        self.p.info.id = "m"
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.folder = Path(self.tmp.name) / "m"

    def files(self):
        return sorted(f.relative_to(self.folder).as_posix() for f in self.folder.rglob("*") if f.is_file())

    def test_round_trip(self):
        """An added duelist's bust and a disc duelist's with replace_bust,
        saved as their pictures under busts/<id>/, opened again the same,
        saved again byte for byte; renamed and dropped, the old files go."""
        p = self.p
        ghost = roster.add_copy(p, 1, "Ghost")
        ghost.bust = three()
        roster.give_bust(p, 7, figure(SAD), "smug")
        roster.set_replace_bust(p, 7, True)
        manifest.save_mod(p, self.folder)
        self.assertEqual([f for f in self.files() if f.startswith("busts/")],
                         ["busts/ghost/angry.png", "busts/ghost/calm-blink.png", "busts/ghost/calm-talk1.png",
                          "busts/ghost/calm.png", "busts/ghost/sad.png", "busts/seto/smug.png"])
        written = json.loads((self.folder / "duelists/ghost.json").read_text())
        self.assertEqual(written["bust"], {"default": "calm", "expressions": [
            {"name": "calm", "image": "busts/ghost/calm.png", "talk": ["busts/ghost/calm-talk1.png"],
             "blink": "busts/ghost/calm-blink.png"},
            {"name": "angry", "image": "busts/ghost/angry.png"},
            {"name": "sad", "image": "busts/ghost/sad.png"}]})
        seto = json.loads((self.folder / "duelists/seto.json").read_text())
        self.assertEqual(seto, {"replace": "Seto", "bust": {"default": "smug", "expressions": [
            {"name": "smug", "image": "busts/seto/smug.png"}]}, "replace_bust": True})
        again, messages = manifest.open_mod(p.retail, self.folder)
        self.assertEqual(messages, [])
        mine = roster.find(again, "ghost")
        self.assertEqual([e.name for e in mine.bust.expressions], ["calm", "angry", "sad"])
        self.assertEqual(mine.bust.expressions[0].talk, ghost.bust.expressions[0].talk)
        self.assertEqual(mine.bust.expressions[0].blink, ghost.bust.expressions[0].blink)
        self.assertTrue(roster.replacement(again, 7).replace_bust)
        before = {f: (self.folder / f).read_bytes() for f in self.files()}
        manifest.save_mod(again, self.folder)
        self.assertEqual({f: (self.folder / f).read_bytes() for f in self.files()}, before)
        # Renamed: its files are named for it; the old ones go.
        bustm.rename(mine.bust, bustm.find(mine.bust, "calm"), "quiet")
        roster.drop_bust(again, 7)
        manifest.save_mod(again, self.folder)
        busts = [f for f in self.files() if f.startswith("busts/")]
        self.assertEqual(busts, ["busts/ghost/angry.png", "busts/ghost/quiet-blink.png", "busts/ghost/quiet-talk1.png",
                                 "busts/ghost/quiet.png", "busts/ghost/sad.png"])
        self.assertNotIn("duelists/seto.json", self.files())        # nothing left of the replacement
        self.assertEqual(json.loads((self.folder / "duelists/ghost.json").read_text())["bust"]["default"], "quiet")

    def test_spelling_kept(self):
        """A bust written by hand keeps its paths and its short form."""
        self.folder.mkdir(parents=True)
        (self.folder / "art").mkdir()
        (self.folder / "art/g.png").write_bytes(figure(CALM))
        (self.folder / "art/g-open.png").write_bytes(figure(CALM, mouth=True))
        (self.folder / "mod.json").write_text(json.dumps({"id": "m", "name": "M", "duelists": [
            {"id": "a", "copy": 1, "bust": "art/g.png"},
            {"id": "b", "copy": 2, "bust": {"default": "x", "expressions": [
                {"name": "x", "image": "art/g.png", "talk": "art/g-open.png"}]}},
            {"id": "c", "copy": 3, "bust": {"default": "y", "expressions": [{"name": "y", "image": "art/none.png"}]}}]}))
        p, messages = manifest.open_mod(self.p.retail, self.folder)
        self.assertEqual(messages, ["duelist c bust expressions[0]: no file art/none.png; kept as written"])
        manifest.save_mod(p, self.folder)
        listed = {e["id"]: e for e in json.loads((self.folder / "mod.json").read_text())["duelists"]}
        self.assertEqual(listed["a"]["bust"], "art/g.png")
        self.assertEqual(listed["b"]["bust"]["expressions"][0]["talk"], "art/g-open.png")
        self.assertEqual(listed["c"]["bust"], {"default": "y", "expressions": [{"name": "y", "image": "art/none.png"}]})
        self.assertTrue((self.folder / "art/g-open.png").is_file())

    def test_partly_unread_bust_keeps_its_pictures(self):
        """A bust one of whose pictures is missing is kept as written: Save
        leaves the pictures it has where they are, Save As carries them (the
        bug: they were the editor's, unwritten, so Save deleted them)."""
        self.folder.mkdir(parents=True)
        (self.folder / "art").mkdir()
        (self.folder / "art/g.png").write_bytes(figure(CALM))
        (self.folder / "art/g-blink.png").write_bytes(figure(CALM, shut=True))
        written = {"default": "x", "expressions": [
            {"name": "x", "image": "art/g.png", "talk": "art/missing.png", "blink": "art/g-blink.png"}]}
        (self.folder / "mod.json").write_text(json.dumps({"id": "m", "name": "M", "duelists": [
            {"id": "g", "copy": 1, "bust": written}]}))
        p, messages = manifest.open_mod(self.p.retail, self.folder)
        self.assertEqual(messages, ["duelist g bust expressions[0]: no file art/missing.png; kept as written"])
        manifest.save_mod(p, self.folder)
        self.assertEqual((self.folder / "art/g.png").read_bytes(), figure(CALM))
        self.assertEqual((self.folder / "art/g-blink.png").read_bytes(), figure(CALM, shut=True))
        self.assertEqual(json.loads((self.folder / "mod.json").read_text())["duelists"][0]["bust"], written)
        again, _ = manifest.open_mod(self.p.retail, self.folder)
        elsewhere = Path(self.tmp.name) / "copy"
        manifest.save_mod(again, elsewhere)
        self.assertEqual((elsewhere / "art/g.png").read_bytes(), figure(CALM))
        self.assertEqual((elsewhere / "art/g-blink.png").read_bytes(), figure(CALM, shut=True))
        self.assertEqual(json.loads((elsewhere / "mod.json").read_text())["duelists"][0]["bust"], written)

    def test_shared_picture_outlives_the_bust(self):
        """A PNG a bust and a tower story both show stays when the bust is
        taken out, on Save and on Save As (the bug: the bust's going took
        the story's picture with it)."""
        self.folder.mkdir(parents=True)
        (self.folder / "art").mkdir()
        (self.folder / "art/g.png").write_bytes(figure(CALM))
        (self.folder / "mod.json").write_text(json.dumps({"id": "m", "name": "M", "duelists": [
            {"id": "g", "copy": 1, "bust": "art/g.png"}],
            "tower": {"intro": {"characters": {"left": "art/g.png"}, "lines": ["Hi."]},
                      "floors": [{"duelist": 1}]}}))
        p, messages = manifest.open_mod(self.p.retail, self.folder)
        self.assertEqual(messages, [])
        g = roster.find(p, "g")
        roster.drop_bust(p, g)
        elsewhere = Path(self.tmp.name) / "copy"
        manifest.save_mod(p, elsewhere)
        self.assertEqual((elsewhere / "art/g.png").read_bytes(), figure(CALM))
        manifest.save_mod(p, self.folder)
        self.assertEqual((self.folder / "art/g.png").read_bytes(), figure(CALM))
        self.assertNotIn("bust", json.loads((self.folder / "mod.json").read_text())["duelists"][0])
        # Not shared, a bust's picture still goes with it.
        (self.folder / "art/h.png").write_bytes(figure(SAD))
        data = json.loads((self.folder / "mod.json").read_text())
        data["duelists"][0]["bust"] = "art/h.png"
        (self.folder / "mod.json").write_text(json.dumps(data))
        p, _ = manifest.open_mod(self.p.retail, self.folder)
        roster.drop_bust(p, roster.find(p, "g"))
        manifest.save_mod(p, self.folder)
        self.assertFalse((self.folder / "art/h.png").exists())
        self.assertTrue((self.folder / "art/g.png").is_file())

    def test_shared_picture_changed_in_a_file_of_its_own(self):
        """A picture a story or another expression shows too, changed on the
        Bust page, goes in a file of its own (the bug: the new picture was
        written over the shared file, changing the story's or the other
        expression's); a duplicate of a bust kept as written names the same
        files (the bug: it named files under its own id that never were)."""
        self.folder.mkdir(parents=True)
        (self.folder / "art").mkdir()
        (self.folder / "art/g.png").write_bytes(figure(CALM))
        (self.folder / "mod.json").write_text(json.dumps({"id": "m", "name": "M", "duelists": [
            {"id": "g", "copy": 1, "bust": {"default": "x", "expressions": [
                {"name": "x", "image": "art/g.png"}, {"name": "y", "image": "art/g.png"}]}},
            {"id": "h", "copy": 2, "bust": "art/g.png"},
            {"id": "u", "copy": 3, "bust": {"default": "z", "expressions": [
                {"name": "z", "image": "art/g.png", "talk": "art/missing.png"}]}}],
            "tower": {"intro": {"characters": {"left": "art/g.png"}, "lines": ["Hi."]},
                      "floors": [{"duelist": 1}]}}))
        p, _ = manifest.open_mod(self.p.retail, self.folder)
        g, h = roster.find(p, "g"), roster.find(p, "h")
        self.assertTrue(roster.picture_shared(p, "art/g.png"))
        self.assertFalse(roster.picture_shared(p, "art/other.png"))
        # As BustView.choose_picture does it.
        for e in (bustm.find(g.bust, "y"), h.bust.expressions[0]):
            if roster.picture_shared(p, e.image_path):
                e.image_path = None
            e.image = figure(ANGRY)
        copy = roster.duplicate(p, roster.find(p, "u"))
        manifest.save_mod(p, self.folder)
        self.assertEqual((self.folder / "art/g.png").read_bytes(), figure(CALM))
        again, _ = manifest.open_mod(self.p.retail, self.folder)
        self.assertEqual(bustm.find(roster.find(again, "g").bust, "x").image, figure(CALM))
        self.assertEqual(bustm.find(roster.find(again, "g").bust, "y").image, figure(ANGRY))
        self.assertEqual(roster.find(again, "h").bust.expressions[0].image, figure(ANGRY))
        written = json.loads((self.folder / f"duelists/{copy.key}.json").read_text())
        self.assertEqual(written["bust"], {"default": "z", "expressions": [
            {"name": "z", "image": "art/g.png", "talk": "art/missing.png"}]})

    def test_checks(self):
        p = self.p
        ghost = roster.add_copy(p, 1, "Ghost")
        b = ghost.bust = three()
        issues = lambda: [(i.level, i.message) for i in validate.validate(p) if i.area == "Duelists"]  # noqa: E731
        self.assertEqual(issues(), [])
        b.expressions[1].talk.append(figure(ANGRY, w=W + 8))
        b.expressions[1].talk_paths.append(None)
        for name in ("e4", "e5"):
            bustm.add(b, name, figure(SAD))
        b.default = "nobody"
        found = issues()
        self.assertIn(("warning", "its bust's default is none of its expressions; the first, \"calm\", is taken"), found)
        self.assertIn(("warning", f"expression \"angry\": talking frame 1 is {W + 8} x {H}, the default's {W} x {H}; "
                                  "the game stretches it to that"), found)
        self.assertIn(("warning", "5 expressions: the game changes 4 of one record in place; the fifth on come in "
                                  "as another character, sliding"), found)
        # replace_bust: a disc duelist's, which the campaign shows as a character.
        roster.give_bust(p, 4, figure(CALM))
        roster.set_replace_bust(p, 4, True)
        self.assertIn(("warning", "the campaign shows Villager 1 as no character: replace_bust changes nothing "
                                  "there"), issues())
        with self.assertRaises(ValueError):
            roster.set_replace_bust(p, ghost, True)
        # Names the editor makes.
        self.assertEqual(bustm.check_name(b, ""), "an expression has a name")
        self.assertIn("already", bustm.check_name(b, "ANGRY"))
        self.assertIn("at most 31", bustm.check_name(b, "x" * 32))
        with self.assertRaises(ValueError):
            bustm.add(b, "sad", figure(SAD))

    def test_room_for_the_frames(self):
        """A picture that fills the game's page leaves no room for its
        frames' changes: Conflicts says what the game leaves out and what
        the expression then does, as busts.c's note does; a smaller one is
        quiet, and the room is busts.c's (the page less the picture rounded
        up to 8)."""
        self.assertEqual(bustm.room((256, 256)), (256, 256, 0))
        self.assertEqual(bustm.room((240, 256)), (240, 256, 64))
        self.assertEqual(bustm.room((100, 120)), (104, 120, 1024 - 13 * 15))
        self.assertEqual(bustm.room((600, 400)), (256, 176, 1024 - 32 * 22))     # shrunk to 256 x 170 first
        p = self.p
        ghost = roster.add_copy(p, 1, "Ghost")
        found = lambda: [m for lv, m in ((i.level, i.message) for i in validate.validate(p))  # noqa: E731
                         if "squares" in m]
        big = lambda **k: figure(CALM, w=256, h=256, **k)  # noqa: E731
        ghost.bust = bustm.short(big(), "calm")
        e = ghost.bust.expressions[0]
        e.talk, e.talk_paths, e.blink = [big(mouth=True)], [None], big(shut=True)
        # The mouth two squares, the eyes four of which two alike: five.
        self.assertEqual(found(), ["expression \"calm\": its frames change about 5 of the 8 x 8 squares, and its "
                                   "picture leaves 0 of the game's 256 x 256 page free for them: the game makes it "
                                   "without its blink frame and its talking frame, so it never blinks and never "
                                   "talks (a smaller picture leaves more room: see the page's hint)"])
        ghost.bust = three()
        self.assertEqual(found(), [])

    def test_numbers_and_characters(self):
        """The game numbers the default 0, then the list's order; the disc
        duelists' campaign characters are tower_config.c's."""
        b = three()
        b.default = "angry"
        self.assertEqual([e.name for e in bustm.order(b)], ["angry", "calm", "sad"])
        self.assertEqual(bustm.number(b, bustm.find(b, "sad")), 2)
        source = (Path(__file__).resolve().parents[4] / "src/pc/tower/tower_config.c").read_text(encoding="utf-8")
        table = re.search(r"duelist_characters\[39\] = \{(.*?)\};", source, re.S).group(1)
        table = re.sub(r"/\*.*?\*/", "", table, flags=re.S)
        self.assertEqual(tuple(int(v, 0) for v in re.findall(r"0x[0-9A-Fa-f]+|\b\d+\b", table)),
                         bustm.DUELIST_CHARACTERS)
        self.assertEqual(bustm.sharing(7), [7, 32, 36])
        self.assertEqual(bustm.campaign_character(23), 0)
        header = (Path(__file__).resolve().parents[4] / "src/pc/free_duel/busts.h").read_text(encoding="utf-8")
        for name, value in (("BUST_EXPRESSIONS", bustm.EXPRESSIONS), ("BUST_TALK_FRAMES", bustm.TALK_FRAMES),
                            ("BUST_IN_PLACE", bustm.IN_PLACE), ("BUST_SIDE", bustm.SIDE)):
            self.assertRegex(header, rf"#define {name} {value}\b")

    def test_preview_where_the_game_stands_it(self):
        """Its middle over the side's place, its bottom on the box (under
        178 rows), and the frames drawn over the picture."""
        b = three()
        calm = bustm.find(b, "calm")
        screen = bustm.preview(bustm.frame(calm), "left")
        self.assertEqual(screen.size, (320, 240))
        left, top = 0x68 - W // 2, 0xB2 - H
        self.assertEqual(screen.pixel(left + 30, top + 6)[:3], CALM)
        talking = bustm.preview(bustm.frame(calm, ("talk", 0)), "right")
        self.assertEqual(talking.pixel(0xD8 - W // 2 + 30, top + 36)[:3], (250, 250, 250))
        shut = bustm.frame(calm, "blink")
        self.assertEqual(shut.pixel(30, 21)[:3], (10, 10, 10))
        big = bustm.fitted(pngio.Image(512, 300, b"\x10\x20\x30\x80" * (512 * 300)))
        self.assertEqual(big.size, (256, 150))
        self.assertEqual(big.pixel(0, 0)[3], 255)

    def test_mod_api(self):
        """A bust, replace_bust and a story's expression by name need mod API
        17; so does a bust in a duelist's own file."""
        needs = lambda value, project=None: compat.required(value, project)[0]  # noqa: E731
        self.assertEqual(needs({"duelists": [{"id": "g", "copy": 1, "bust": "b.png"}]}), 17)
        self.assertEqual(needs({"duelists": [{"replace": 7, "replace_bust": True}]}), 17)
        self.assertEqual(needs({"tower": {"floors": [{"duelist": 1, "before": {"characters": {
            "left": {"duelist": "g", "expression": "angry"}}, "lines": ["Hi."]}}]}}), 17)
        self.assertEqual(needs({"tower": {"floors": [{"duelist": 1, "before": {"characters": {
            "left": {"duelist": "g", "expression": 2}}, "lines": ["Hi."]}}]}}), 14)
        # A number past 3 is a bust's too; the forms the game turns down need
        # nothing newer (validate says they are left out).
        side = lambda cast: {"tower": {"floors": [{"duelist": 1, "before": {"characters": {  # noqa: E731
            "left": cast}, "lines": ["Hi."]}}]}}
        self.assertEqual(needs(side({"duelist": "g", "expression": 7})), 17)
        self.assertEqual(needs(side({"character": 4, "expression": "angry"})), 14)
        self.assertEqual(needs(side({"duelist": "g", "image": "a.png", "expression": "angry"})), 14)
        self.assertEqual(needs(side({"duelist": "g", "expression": "x" * 32})), 14)
        self.assertEqual(needs(side({"duelist": "g", "expression": 3})), 14)
        p = self.p
        roster.add_copy(p, 1, "Ghost").bust = three()
        self.assertEqual(needs(manifest.build(p), p), 17)
        self.assertEqual(compat.HOST_API, 19)

    def test_tower_names_an_expression(self):
        """[left: Ghost expression angry] and back; the checks know the bust."""
        for text, value in (("Ghost expression angry", {"duelist": "Ghost", "expression": "angry"}),
                            ('Ghost expression "very angry"', {"duelist": "Ghost", "expression": "very angry"}),
                            ("opponent expression sad", {"duelist": "opponent", "expression": "sad"}),
                            ("4 expression 2", {"character": 4, "expression": 2})):
            self.assertEqual(tower._cast_value(text), value)
            self.assertEqual(tower._cast_text(value), text)
        p = self.p
        ghost = roster.add_copy(p, 1, "Ghost")
        ghost.bust = three()
        story = {"characters": {"left": {"duelist": "Ghost", "expression": "glad"},
                                "right": {"duelist": "Seto", "expression": "calm"}}, "lines": ["Hi."]}
        found = tower.check({"intro": story, "floors": [{"duelist": 1}]}, lambda n: roster.named(p, n),
                            lambda n: p.resolve(n), lambda n: roster.bust_of(p, roster.named(p, n)))
        # Each says what the game shows instead (tower_config.c bust_expression).
        self.assertIn(("warning", "tower.intro.characters.left.expression",
                       "Ghost's bust has no expression \"glad\"; its default, \"calm\", is shown instead"), found)
        self.assertIn(("warning", "tower.intro.characters.right.expression",
                       "Seto has no bust of its own (the Duelists tab's Bust); \"calm\" is left out, and the campaign's "
                       "character is shown as it first comes in (expression 0)"), found)
        story["characters"] = {"left": {"duelist": "Ghost", "expression": "angry"}}
        self.assertEqual(tower.check({"intro": story, "floors": [{"duelist": 1}]}, lambda n: roster.named(p, n),
                                     lambda n: p.resolve(n), lambda n: roster.bust_of(p, roster.named(p, n))), [])

    def test_tower_expression_checks(self):
        """As tower_config.c reads them: numbers 0-15 for a duelist (its
        bust's, or 0-3 a disc duelist's campaign character's), the floor's
        opponent against that duelist's bust, a long name, and the forms the
        game turns down."""
        p = self.p
        ghost = roster.add_copy(p, 1, "Ghost")
        ghost.bust = three()
        roster.give_bust(p, 7, figure(SAD), "smug")

        def found(story, floor="Ghost"):
            return [(w, m) for _, w, m in tower.check(
                {"intro": story, "floors": [{"duelist": floor, "before": story}]}, lambda n: roster.named(p, n),
                lambda n: p.resolve(n), lambda n: roster.bust_of(p, roster.named(p, n)),
                lambda n: roster.shows_bust(p, roster.named(p, n)))]
        story = lambda **sides: {"characters": sides, "lines": ["Hi."]}  # noqa: E731
        at = "tower.intro.characters.left.expression"
        self.assertEqual(found(story(left={"duelist": "Ghost", "expression": 2},
                                     right={"duelist": "Seto", "expression": 3})), [])
        self.assertIn((at, "Ghost's bust has 3 expressions (0-2); for 7 its default, \"calm\", is shown instead"),
                      found(story(left={"duelist": "Ghost", "expression": 7})))
        self.assertIn((at, "Seto's bust has 1 expression (0-0); for 4 its default, \"smug\", is shown instead"),
                      found(story(left={"duelist": "Seto", "expression": 4})))
        self.assertIn((at, "Simon Muran has no bust of its own (the Duelists tab's Bust); 5 (past the campaign's 0 "
                           "to 3) is left out, and the campaign's character is shown as it first comes in "
                           "(expression 0)"),
                      found(story(left={"duelist": "Simon Muran", "expression": 5})))
        self.assertIn((at, "is the name of one of the duelist's bust's, or a number: 0 to 3 (the campaign "
                           "character's, or the bust's), 4 to 15 (the bust's)"),
                      found(story(left={"duelist": "Ghost", "expression": 16})))
        self.assertIn((at, "is 0 to 3"), found(story(left={"character": 4, "expression": 5})))
        self.assertIn((at, "by name is a duelist's bust's ({\"duelist\": ..., \"expression\": ...}); a campaign "
                           "character's is 0 to 3; left out"),
                      found(story(left={"character": 4, "expression": "calm"})))
        self.assertIn((at, "\"aaaaaaaaaaaaaaaaaaaaaaaa...\" is longer than a bust's expression's name can be (31 "
                           "bytes at most); left out"),
                      found(story(left={"duelist": "Ghost", "expression": "a" * 32})))
        # The floor's opponent: checked against its duelist's bust, in the floor's story only.
        result = found(story(right={"duelist": "opponent", "expression": "glad"}))
        self.assertIn(("tower.floors[0].before.characters.right.expression", "the opponent (Ghost)'s bust has no "
                       "expression \"glad\"; its default, \"calm\", is shown instead"), result)
        self.assertFalse(any(w == "tower.intro.characters.right.expression" for w, _ in result))
        self.assertIn(("tower.floors[0].before.characters.right.expression", "the opponent (Simon Muran) has no "
                       "bust of its own (the Duelists tab's Bust); \"calm\" is left out, and the campaign's "
                       "character is shown as it first comes in (expression 0)"),
                      found(story(right={"duelist": "opponent", "expression": "calm"}), floor="Simon Muran"))
        self.assertNotIn("floors", " ".join(w for w, _ in found(story(right={"duelist": "opponent",
                                                                            "expression": "sad"}))))

    def test_stories_follow_the_bust(self):
        """A tower's sides naming a duelist's expression (by its name, or as
        the floor's opponent) are found and renamed; numbers are counted,
        0-3 only where the bust is shown unasked."""
        data = {"intro": {"characters": {"left": {"duelist": "Ghost", "expression": "Calm"},
                                         "right": {"duelist": "Seto", "expression": "calm"}},
                          "lines": ["Hi.", {"text": "Two.", "characters": {"left": {"duelist": "Ghost",
                                                                                     "expression": 2}}}]},
                "floors": [{"duelist": "Ghost", "after": [{"text": "Bye.", "characters": {
                    "right": {"duelist": "opponent", "expression": "calm"},
                    "left": {"duelist": "Ghost", "expression": 5}}}]}]}
        is_ghost = lambda who: str(who) == "Ghost"  # noqa: E731
        self.assertEqual(len(tower.bust_casts(data, is_ghost)), 4)
        self.assertEqual(tower.rename_expression(data, is_ghost, "calm", "quiet"), 2)
        self.assertEqual(data["intro"]["characters"]["left"]["expression"], "quiet")
        self.assertEqual(data["intro"]["characters"]["right"]["expression"], "calm")      # Seto's, not Ghost's
        self.assertEqual(data["floors"][0]["after"][0]["characters"]["right"]["expression"], "quiet")
        self.assertEqual(tower.numbered_expressions(data, is_ghost, True), 2)
        self.assertEqual(tower.numbered_expressions(data, is_ghost, False), 1)


class BustTabTest(GuiCase):
    def test_the_bust_section(self):
        """Add an expression, rename it, its frames, the default, the order,
        the campaign option (asked first) and the preview; the Tower tab's
        picker writes the duelist and its expression."""
        app, tab = self.app, self.app.duelists
        p = app.project
        app.notebook.select(tab)
        app.update()
        view = tab.bust_view
        tab.sections.select(view.frame)
        tab.goto((7, None))
        folder = Path(self.tmp.name) / "pictures"
        folder.mkdir(exist_ok=True)
        for name, data in (("calm", figure(CALM)), ("angry", figure(ANGRY)), ("open", figure(CALM, mouth=True)),
                           ("shut", figure(CALM, shut=True))):
            (folder / f"{name}.png").write_bytes(data)
        self.assertIsNone(view.add(str(folder / "calm.png"), "calm"))
        b = roster.bust_of(p, 7)
        self.assertEqual([e.name for e in b.expressions], ["calm"])
        self.assertEqual(roster.replacement(p, 7).bust, b)
        self.assertIn("bust", tab.list.set("7", "state"))
        self.assertIn("already", view.add(str(folder / "angry.png"), "calm"))      # a name used: refused
        view.add(str(folder / "angry.png"), "angry")
        self.assertEqual(len(view.list.get_children()), 2)
        view.list.selection_set(view.list.get_children()[0])
        view.add_talk(str(folder / "open.png"))
        view.choose_blink(str(folder / "shut.png"))
        e = bustm.find(b, "calm")
        self.assertEqual(len(e.talk), 1)
        self.assertIsNotNone(e.blink)
        self.assertEqual(view.list.set(str(id(e)), "frames"), "talk 1, blink")
        view.which.set("talk0")
        view.draw()
        self.assertIn("320×240", view.caption.cget("text"))
        view.rename("quiet")
        self.assertEqual(b.default, "quiet")
        view.list.selection_set(str(id(bustm.find(b, "angry"))))
        view.make_default()
        self.assertEqual([x.name for x in bustm.order(b)], ["angry", "quiet"])
        # The campaign option: asked first, said beside it.
        with mock.patch("tkinter.messagebox.askyesno", return_value=False):
            view.replace.set(True)
            view.toggle_replace()
        self.assertFalse(roster.replacement(p, 7).replace_bust)
        with mock.patch("tkinter.messagebox.askyesno", return_value=True):
            view.replace.set(True)
            view.toggle_replace()
        self.assertTrue(roster.replacement(p, 7).replace_bust)
        self.assertIn("CHANGES THE CAMPAIGN", view.warning.cget("text"))
        self.assertIn("Seto 2nd, Seto 3rd", view.warning.cget("text"))
        # The Tower tab's picker.
        towers = app.tower
        app.notebook.select(towers)
        towers.has.set(True)
        towers.toggle()
        app.update()
        towers._bust_duelists()
        self.assertIn("Seto", towers.stage_duelists.cget("values"))
        towers.stage_duelist.set("Seto")
        towers._bust_expressions()
        self.assertEqual(list(towers.stage_bust_expressions.cget("values")), ["angry", "quiet"])
        towers.stage_bust_expression.set("quiet")
        towers.last_story = "intro"
        towers._bust()
        self.assertIn("[left: Seto expression quiet]", towers.texts["intro"].get("1.0", "end"))
        # The stories follow the page: a rename, a removal (to the default),
        # and an order that changes what a number shows, said.
        app.notebook.select(tab)
        tab.goto((7, None))
        app.update()
        p.other["tower"]["intro"] = {"characters": {"left": {"duelist": "Seto", "expression": "quiet"},
                                                    "right": {"duelist": "Seto", "expression": "angry"}},
                                     "lines": ["Hi.", {"text": "Two.", "characters": {
                                         "left": {"duelist": "Seto", "expression": 5}}}]}
        view.list.selection_set(str(id(bustm.find(b, "quiet"))))
        view.rename("calm")
        self.assertEqual(p.other["tower"]["intro"]["characters"]["left"]["expression"], "calm")
        self.assertIn("now names \"calm\"", view.story_note.cget("text"))
        self.assertIn("[left: Seto expression calm]", towers.texts["intro"].get("1.0", "end"))
        view.list.selection_set(str(id(bustm.find(b, "calm"))))
        view.move(1)
        view.list.selection_set(str(id(bustm.find(b, "angry"))))
        view.make_default()
        self.assertIn("1 story side of the Tower tab asks for this bust by number", view.story_note.cget("text"))
        view.list.selection_set(str(id(bustm.find(b, "calm"))))
        view.remove()
        self.assertEqual(p.other["tower"]["intro"]["characters"]["left"]["expression"], "angry")
        self.assertIn("now name the default, \"angry\"", view.story_note.cget("text"))
        # Gone: the replacement with it.
        view.drop(ask=False)
        self.assertIsNone(roster.replacement(p, 7))
