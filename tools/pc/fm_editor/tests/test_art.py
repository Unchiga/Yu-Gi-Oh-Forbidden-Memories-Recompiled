"""Card art on synthetic data: PNG in and out, the disc's pictures, and a
mod's own (texture pack for retail cards, "art"/"thumbnail"/"title" keys)
saved and read back.

    python -m unittest discover -s tools/pc/fm_editor/tests -t tools/pc
"""
import json
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

from fm_editor import art, manifest, pngio, validate
from fm_editor.model import Project
from fm_editor.pngio import Image
from fm_editor.tests.fixtures import art_colour
from fm_editor.tests.test_data import fixture


def gradient(width, height, alpha=255) -> Image:
    return Image(width, height, b"".join(bytes(((x * 255) // max(1, width - 1), (y * 255) // max(1, height - 1),
                                                (x + y) & 0xFF, alpha))
                                         for y in range(height) for x in range(width)))


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)


def png(width, height, colour, bits, rows, extra=b"", interlace=0):
    """A PNG from already-filtered row data (each row its filter byte first)."""
    return (pngio.SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, bits, colour, 0, 0, interlace)) +
            extra + chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))


def expand(word):
    r, g, b = word & 0x1F, (word >> 5) & 0x1F, (word >> 10) & 0x1F
    return (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255)


class PngTest(unittest.TestCase):
    def test_round_trip(self):
        for image in (gradient(7, 5), gradient(3, 4, alpha=128)):
            self.assertEqual(pngio.decode(pngio.encode(image)), image)

    def test_filters(self):
        # Sub, Up, Average and Paeth rows of a 3x4 RGB image, filtered here.
        pixels = [[(10 * x + y, 20 * y, 7 * x * y) for x in range(3)] for y in range(4)]
        raw = [bytes(c for p in row for c in p) for row in pixels]
        rows, previous = [], bytes(9)
        for kind, line in zip((1, 2, 3, 4), raw):
            out = bytearray()
            for i, v in enumerate(line):
                a = line[i - 3] if i >= 3 else 0
                b = previous[i]
                c = previous[i - 3] if i >= 3 else 0
                predictor = {1: a, 2: b, 3: (a + b) >> 1, 4: pngio._paeth(a, b, c)}[kind]
                out.append((v - predictor) & 0xFF)
            rows.append(bytes([kind]) + bytes(out))
            previous = line
        image = pngio.decode(png(3, 4, 2, 8, rows))
        for y in range(4):
            for x in range(3):
                self.assertEqual(image.pixel(x, y), pixels[y][x] + (255,))

    def test_other_layouts(self):
        # Palette with tRNS
        image = pngio.decode(png(2, 1, 3, 8, [b"\x00\x00\x01"], chunk(b"PLTE", b"\x01\x02\x03\x04\x05\x06") +
                                 chunk(b"tRNS", b"\x00")))
        self.assertEqual(image.pixel(0, 0), (1, 2, 3, 0))
        self.assertEqual(image.pixel(1, 0), (4, 5, 6, 255))
        # Grey with alpha, 16-bit RGB, 1-bit grey
        self.assertEqual(pngio.decode(png(1, 1, 4, 8, [b"\x00\x80\x40"])).pixel(0, 0), (128, 128, 128, 64))
        self.assertEqual(pngio.decode(png(1, 1, 2, 16, [b"\x00\x12\x34\x56\x78\x9a\xbc"])).pixel(0, 0),
                         (0x12, 0x56, 0x9a, 255))
        grey = pngio.decode(png(3, 1, 0, 1, [b"\x00\xa0"]))
        self.assertEqual([grey.pixel(x, 0)[0] for x in range(3)], [255, 0, 255])

    def test_interlaced(self):
        width, height = 5, 5
        source = gradient(width, height)
        rows = []
        for x0, y0, dx, dy in ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4),
                               (1, 0, 2, 2), (0, 1, 1, 2)):
            for y in range(y0, height, dy):
                xs = list(range(x0, width, dx))
                if xs:
                    rows.append(b"\x00" + b"".join(bytes(source.pixel(x, y)) for x in xs))
        self.assertEqual(pngio.decode(png(width, height, 6, 8, rows, interlace=1)), source)

    def test_not_png(self):
        with self.assertRaises(pngio.PngError):
            pngio.decode(b"GIF89a")

    def test_damaged(self):
        good = png(2, 2, 2, 8, [b"\x00" + bytes(6)] * 2)
        at = good.index(b"IDAT") + 6
        cases = {
            "CRC": good[:at] + bytes([good[at] ^ 1]) + good[at + 1:],
            "short IHDR": pngio.SIGNATURE + chunk(b"IHDR", b"\x00\x00\x00\x02") + chunk(b"IEND", b""),
            "cut chunk": good[:at + 4],
            "rows end on a boundary": png(2, 2, 2, 8, [b"\x00" + bytes(6)]),
            "RGB at 4 bits": png(2, 1, 2, 4, [b"\x00\x12\x34\x56"]),
            "interlace 2": png(2, 2, 2, 8, [b"\x00" + bytes(6)] * 2, interlace=2),
        }
        for name, data in cases.items():
            with self.subTest(name), self.assertRaises(pngio.PngError):
                pngio.decode(data)
        # Decompression stops at what the rows can hold.
        bomb = (pngio.SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 0, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(bytes(50_000_000))) + chunk(b"IEND", b""))
        self.assertEqual(pngio.decode(bomb).size, (1, 1))


class DiscArtTest(unittest.TestCase):
    def test_pictures(self):
        wa = fixture().wa
        picture = art.disc_image(wa, 2, "art")
        self.assertEqual(picture.size, (102, 96))
        self.assertEqual(picture.pixel(0, 0), expand(art_colour(2, 1 + 2 % 255)))
        self.assertEqual(picture.pixel(5, 3), expand(art_colour(2, 1 + (5 + 6 + 2) % 255)))
        thumb = art.disc_image(wa, 2, "thumbnail")
        self.assertEqual(thumb.size, (40, 32))
        self.assertEqual(thumb.pixel(4, 1), expand(art_colour(3, 1 + (12 + 1 + 2) % 63)))
        inks = art.disc_plate_inks(wa, 2)
        self.assertEqual(len(inks), 96 * 14)
        self.assertEqual(inks[:4], [0, (0 + 2) % 8, 1, (1 + 2) % 8])
        # A plate exported as dark ink on white reads back as the same inks.
        self.assertEqual(art.plate_inks(art.plate_image(inks)), inks)

    def test_identities(self):
        for cid in (1, 722):
            for part in ("art", "thumbnail"):
                entry = art.pack_entry(cid, part, "x.png")
                self.assertEqual(art.entry_card(entry), (cid, part))
        entry = art.pack_entry(5, "art", "x.png")
        self.assertEqual(entry["offset"], (4 * 7 + 722) * 2048)
        self.assertEqual(entry["clut_offset"], entry["offset"] + 0x2640)
        self.assertIsNone(art.entry_card(dict(entry, bpp=4)))
        self.assertIsNone(art.entry_card(dict(entry, clut_offset=0)))
        self.assertIsNone(art.entry_card(dict(entry, archive="SU.MRG")))


class ArtModTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.game = fixture().game()

    def tearDown(self):
        self.tmp.cleanup()

    def reopen(self, folder):
        project, messages = manifest.open_mod(self.game, folder)
        return project, messages

    def test_normalize(self):
        image, notes = art.normalize(gradient(300, 100), "art")
        self.assertEqual(image.size, (106, 100))
        self.assertTrue(any("shape" in n for n in notes))
        image, notes = art.normalize(gradient(816, 768, alpha=0), "art")
        self.assertEqual(image.size, (408, 384))
        self.assertTrue(all(image.rgba[i] == 255 for i in range(3, len(image.rgba), 4)))
        image, notes = art.normalize(gradient(51, 48), "art")
        self.assertTrue(any("blurred" in n for n in notes))

    def test_retail_art_in_the_pack(self):
        project = Project(self.game)
        big = gradient(408, 384)
        notes = art.set_image(project, 2, "art", big)
        self.assertTrue(any("thumbnail" in n for n in notes))
        self.assertEqual(art.in_game(project, fixture().wa, 2, "art", 1).size, (102, 96))
        self.assertEqual(art.in_game(project, fixture().wa, 2, "art", 2).size, (204, 192))
        folder = self.root / "mod"
        manifest.save_mod(project, folder)
        written = json.loads((folder / "mod.json").read_text(encoding="utf-8"))
        self.assertEqual(written["textures"], "textures")
        self.assertNotIn("cards", written)
        entries = json.loads((folder / "textures" / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual([art.entry_card(e) for e in entries], [(2, "art"), (2, "thumbnail")])
        self.assertEqual(pngio.read(folder / "textures" / entries[0]["file"]), big)
        self.assertEqual(pngio.read(folder / "textures" / entries[1]["file"]).size, (160, 128))
        self.assertEqual(validate.errors(validate.validate(project)), [])
        # Read back: the same pictures, and saving again changes nothing.
        again, messages = self.reopen(folder)
        self.assertEqual(messages, [])
        self.assertEqual(art.replacement_image(again, 2, "art"), big)
        self.assertEqual(art.changed_cards(again), {2})
        manifest.save_mod(again, folder)
        self.assertEqual(json.loads((folder / "textures" / "manifest.json").read_text(encoding="utf-8")), entries)
        # Reverted: the pack goes, and "textures" with it.
        art.revert(again, 2, "art")
        self.assertEqual(art.changed_cards(again), set())
        manifest.save_mod(again, folder)
        written = json.loads((folder / "mod.json").read_text(encoding="utf-8"))
        self.assertNotIn("textures", written)

    def test_added_card_and_plates(self):
        project = Project(self.game)
        cid = project.add_card(3, "dragon")
        art.set_image(project, cid, "art", gradient(204, 192))
        art.set_image(project, cid, "title", gradient(96, 14))
        art.set_image(project, 5, "title", gradient(192, 28))
        wa = fixture().wa
        self.assertIn("own picture", art.shown_image(project, wa, cid, "thumbnail")[1])
        self.assertEqual(art.in_game(project, wa, cid, "art", 2).size, (204, 192))
        folder = self.root / "mod"
        manifest.save_mod(project, folder)
        written = json.loads((folder / "mod.json").read_text(encoding="utf-8"))
        self.assertNotIn("textures", written)
        copy = next(e for e in written["cards"] if e.get("copy"))
        self.assertEqual((copy["art"], copy["title"]), ("art/dragon.png", "art/dragon.title.png"))
        replace = next(e for e in written["cards"] if e.get("replace"))
        self.assertEqual(replace, {"replace": 5, "title": "art/0005.title.png"})
        for file in ("art/dragon.png", "art/dragon.title.png", "art/0005.title.png"):
            self.assertTrue((folder / file).is_file(), file)
        again, _ = self.reopen(folder)
        added = next(c for c, a in again.added.items() if a.key == "dragon")
        self.assertEqual(art.replacement_image(again, added, "art"), gradient(204, 192))
        self.assertEqual(art.changed_cards(again), {added, 5})
        # A copy without art of its own shows its base's in the mod.
        other = again.add_card(2, "plain")
        art.set_image(again, 2, "art", gradient(102, 96))
        self.assertIn("base", art.shown_image(again, wa, other, "art")[1])
        # Its plate is its name set by the game, which the tab does not draw.
        image, where = art.shown_image(again, wa, other, "title")
        self.assertIsNone(image)
        self.assertIn("Times", where)
        self.assertEqual(art.shown_image(again, wa, 2, "title")[1], "the disc")

    def test_added_card_art_is_drawn_at_its_resolution(self):
        # cards.c draws a mod card's PNG bigger than the part at its own
        # resolution above 1x, the thumbnail cut from the picture too.
        project = Project(self.game)
        cid = project.add_card(3, "dragon")
        art.set_image(project, cid, "art", gradient(408, 384))
        wa = fixture().wa
        for part in ("art", "thumbnail"):
            small, big = art.in_game(project, wa, cid, part, 1), art.in_game(project, wa, cid, part, 4)
            self.assertEqual(big.size, (small.width * 4, small.height * 4))
            self.assertNotEqual(big, pngio.scale_nearest(small, 4), part)
        self.assertIn("Internal 4x shows all of it", art.describe(project, cid, "art"))

    def test_a_retail_art_key_moves_to_the_pack(self):
        folder = self.root / "mod"
        (folder / "images").mkdir(parents=True)
        pngio.write(folder / "images" / "old.png", gradient(102, 96))
        pngio.write(folder / "images" / "thumb.png", gradient(40, 32))
        (folder / "mod.json").write_text(json.dumps({"id": "m", "name": "M", "cards": [
            {"replace": 1, "art": "images/old.png", "thumbnail": "images/thumb.png", "attack": 100}]}),
            encoding="utf-8")
        project, _ = self.reopen(folder)
        self.assertEqual(art.state(project).images[(1, "art")].kind, "key")
        notes = art.set_image(project, 1, "art", gradient(204, 192))
        self.assertTrue(any("moves" in n for n in notes))
        manifest.save_mod(project, folder)
        written = json.loads((folder / "mod.json").read_text(encoding="utf-8"))
        self.assertEqual(written["cards"], [{"replace": 1, "attack": 100}])
        entries = json.loads((folder / "textures" / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(sorted(art.entry_card(e) for e in entries), [(1, "art"), (1, "thumbnail")])
        thumb = next(e for e in entries if art.entry_card(e) == (1, "thumbnail"))
        self.assertEqual(pngio.read(folder / "textures" / thumb["file"]), gradient(40, 32))

    def test_a_retail_art_key_moves_with_a_new_thumbnail(self):
        folder = self.root / "mod"
        (folder / "images").mkdir(parents=True)
        pngio.write(folder / "images" / "old.png", gradient(102, 96))
        (folder / "mod.json").write_text(json.dumps({"id": "m", "name": "M", "cards": [
            {"replace": 1, "art": "images/old.png"}]}), encoding="utf-8")
        project, _ = self.reopen(folder)
        art.set_image(project, 1, "thumbnail", gradient(80, 64))
        manifest.save_mod(project, folder)
        written = json.loads((folder / "mod.json").read_text(encoding="utf-8"))
        self.assertNotIn("cards", written)
        entries = json.loads((folder / "textures" / "manifest.json").read_text(encoding="utf-8"))
        files = {art.entry_card(e): e["file"] for e in entries}
        self.assertEqual(sorted(files), [(1, "art"), (1, "thumbnail")])
        self.assertEqual(pngio.read(folder / "textures" / files[(1, "art")]), gradient(102, 96))
        self.assertEqual(pngio.read(folder / "textures" / files[(1, "thumbnail")]), gradient(80, 64))

    def test_a_pack_of_others_is_kept(self):
        folder = self.root / "hd"
        pack = folder / "images"
        pack.mkdir(parents=True)
        for name in ("sheet.png", "shared.png", "one.png"):
            pngio.write(pack / name, gradient(4, 4))
        foreign = [
            {"file": "sheet.png", "archive": "SU.MRG", "offset": 0, "words": 64, "rows": 256, "bpp": 4,
             "clut_offset": 100, "clut_entries": 16, "setting": "screens", "alias": "menu"},
            dict(art.pack_entry(2, "art", "shared.png"), setting="cards"),
            art.pack_entry(3, "art", "shared.png"),
            art.pack_entry(5, "art", "shared.png"),
            art.pack_entry(1, "thumbnail", "one.png"),
        ]
        (pack / "manifest.json").write_text(json.dumps(foreign), encoding="utf-8")
        (folder / "mod.json").write_text(json.dumps({"id": "hd", "name": "HD", "textures": "images", "settings": [
            {"key": "screens", "label": "Screens", "type": "bool", "default": 1},
            {"key": "cards", "label": "Cards", "type": "bool", "default": 1}]}), encoding="utf-8")
        project, messages = self.reopen(folder)
        self.assertEqual(messages, [])
        # Card 2's entry is a part the settings switch: not the editor's.
        self.assertEqual(art.changed_cards(project), {3, 5, 1})
        art.set_image(project, 3, "art", gradient(102, 96, alpha=255))
        art.revert(project, 1, "thumbnail")
        manifest.save_mod(project, folder)
        entries = json.loads((pack / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(entries[:2], foreign[:2])
        # The file card 5 shares is left to it; card 3 gets one of its own.
        self.assertEqual(entries[2]["file"], "shared-2.png")
        self.assertEqual(dict(entries[2], file="shared.png"), foreign[2])
        self.assertEqual(entries[3], foreign[3])
        self.assertEqual(pngio.read(pack / "shared.png"), gradient(4, 4))
        self.assertEqual(pngio.read(pack / "shared-2.png"), gradient(102, 96))
        # The thumbnail card 3 got from its new picture, and card 1's reverted one gone.
        self.assertEqual([art.entry_card(e) for e in entries[4:]], [(3, "thumbnail")])
        written = json.loads((folder / "mod.json").read_text(encoding="utf-8"))
        self.assertEqual(written["textures"], "images")
        self.assertEqual(validate.errors(validate.validate(project)), [])

    def test_a_picture_a_setting_switches(self):
        """assets-hd's card entries each name a setting: an imported picture
        goes before the card's entry, which the port would draw first."""
        folder = self.root / "hd"
        pack = folder / "textures"
        pack.mkdir(parents=True)
        pngio.write(pack / "card-005.png", gradient(204, 192))
        gated = [dict(art.pack_entry(5, "art", "card-005.png"), setting="card_art"),
                 dict(art.pack_entry(5, "thumbnail", "card-005.png"), setting="thumbnails")]
        (pack / "manifest.json").write_text(json.dumps(gated), encoding="utf-8")
        (folder / "mod.json").write_text(json.dumps({"id": "hd", "name": "HD", "textures": "textures", "settings": [
            {"key": "card_art", "label": "Art", "type": "bool", "default": 1},
            {"key": "thumbnails", "label": "Thumbs", "type": "bool", "default": 1}]}), encoding="utf-8")
        project, messages = self.reopen(folder)
        self.assertEqual(messages, [])
        image, where = art.shown_image(project, fixture().wa, 5, "art")
        self.assertEqual((image, "card_art" in where), (gradient(204, 192), True))
        self.assertIn("card_art", art.describe(project, 5, "art"))
        art.set_image(project, 5, "art", gradient(102, 96, alpha=255))
        manifest.save_mod(project, folder)
        entries = json.loads((pack / "manifest.json").read_text(encoding="utf-8"))
        arts = [e for e in entries if art.entry_card(e) == (5, "art")]
        thumbs = [e for e in entries if art.entry_card(e) == (5, "thumbnail")]
        self.assertEqual([e.get("setting") for e in arts], [None, "card_art"])
        self.assertEqual([e.get("setting") for e in thumbs], [None, "thumbnails"])
        self.assertEqual(pngio.read(pack / arts[0]["file"]), gradient(102, 96))
        # Saved again, the order holds.
        again, _ = self.reopen(folder)
        manifest.save_mod(again, folder)
        self.assertEqual(json.loads((pack / "manifest.json").read_text(encoding="utf-8")), entries)

    def test_a_shared_key_file_is_not_overwritten(self):
        folder = self.root / "mod"
        (folder / "images").mkdir(parents=True)
        pngio.write(folder / "images" / "shared.png", gradient(102, 96))
        (folder / "mod.json").write_text(json.dumps({"id": "m", "name": "M", "cards": [
            {"copy": 3, "id": "a", "name": "A", "art": "images/shared.png"},
            {"copy": 3, "id": "b", "name": "B", "art": "images/shared.png"}]}), encoding="utf-8")
        project, _ = self.reopen(folder)
        first = next(c for c, a in project.added.items() if a.key == "a")
        art.set_image(project, first, "art", gradient(51, 48))
        manifest.save_mod(project, folder)
        written = json.loads((folder / "mod.json").read_text(encoding="utf-8"))
        self.assertEqual([e["art"] for e in written["cards"]], ["art/a.png", "images/shared.png"])
        self.assertEqual(pngio.read(folder / "images" / "shared.png"), gradient(102, 96))
        self.assertEqual(pngio.read(folder / "art" / "a.png"), gradient(51, 48))
        # Its own file now: a second import writes over it.
        again, _ = self.reopen(folder)
        first = next(c for c, a in again.added.items() if a.key == "a")
        art.set_image(again, first, "art", gradient(60, 56))
        self.assertEqual(art.state(again).images[(first, "art")].file, "art/a.png")

    def test_an_unreadable_art_key_stops_the_import(self):
        folder = self.root / "mod"
        folder.mkdir()
        (folder / "mod.json").write_text(json.dumps({"id": "m", "name": "M", "cards": [
            {"replace": 1, "art": "images/missing.png"}]}), encoding="utf-8")
        project, _ = self.reopen(folder)
        with self.assertRaises(ValueError):
            art.set_image(project, 1, "thumbnail", gradient(40, 32))
        self.assertEqual(project.card_extra[1], {"art": "images/missing.png"})
        self.assertNotIn((1, "thumbnail"), art.state(project).images)

    def test_checks(self):
        folder = self.root / "mod"
        pack = folder / "textures"
        pack.mkdir(parents=True)
        pngio.write(pack / "ok.png", gradient(4, 4))
        good = art.pack_entry(4, "art", "ok.png")
        entries = [
            good,
            dict(good, file="../x.png"),
            dict(good, file="missing.png"),
            dict(good, words=2000),
            dict(good, bpp=5),
            dict(good, crop_left=102),
            dict(good, width=103),
            dict(good, row_offsets=[0, 1]),
            dict(good, setting="nope"),
            {"file": "ok.png", "offset": 0},
        ]
        (pack / "manifest.json").write_text(json.dumps(entries), encoding="utf-8")
        (folder / "art").mkdir()
        (folder / "art" / "bad.png").write_bytes(b"not a png")
        (folder / "mod.json").write_text(json.dumps({"id": "m", "name": "M", "textures": "textures", "cards": [
            {"replace": 7, "title": "art/bad.png"}, {"replace": 8, "art": "art/none.png"},
            {"replace": 9, "art": "C:/x.png"}]}), encoding="utf-8")
        project, _ = self.reopen(folder)
        issues = [i for i in validate.validate(project) if i.area == "Art"]
        text = [f"{i.level} {i.where} {i.message}" for i in issues]

        def has(level, where, words):
            self.assertTrue(any(t.startswith(f"{level} {where}") and words in t for t in text), (where, words, text))
        has("error", "textures[1]", "outside the pack")
        has("error", "textures[2]", "not there")
        has("error", "textures[3]", "out of range")
        has("error", "textures[4]", "out of range")
        has("error", "textures[5]", "out of range")
        has("error", "textures[6]", "width")
        has("warning", "textures[7]", "row_offsets")
        has("warning", "textures[8]", "setting")
        has("error", "textures[9]", "no file or archive")
        has("error", "card 7 \"title\"", "not a PNG")
        has("error", "card 8 \"art\"", "not there")
        has("error", "card 9 \"art\"", "outside the mod")
        self.assertFalse(any(t.startswith("error textures[0]") for t in text))
        # A pack whose manifest cannot be read is left as it is, and said.
        (pack / "manifest.json").write_text("{", encoding="utf-8")
        project, messages = self.reopen(folder)
        self.assertTrue(any("textures" in m for m in messages))
        self.assertTrue(any(i.where == "textures" and i.level == "error" for i in validate.validate(project)))
        with self.assertRaises(ValueError):
            art.set_image(project, 4, "art", gradient(102, 96))
        (folder / "mod.json").write_text(json.dumps({"id": "m", "name": "M", "textures": "../out"}), encoding="utf-8")
        project, _ = self.reopen(folder)
        self.assertTrue(any("outside the mod" in i.message for i in validate.validate(project)))


if __name__ == "__main__":
    unittest.main()
