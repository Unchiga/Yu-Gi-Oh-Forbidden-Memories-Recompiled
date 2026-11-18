"""The card-text preview: the card view's layout, the retail font off the
disc and the TrueType reader (card_text.py, ttf.py), on synthetic data."""
import struct
import tempfile
import unittest
from pathlib import Path

import os
import shutil
import subprocess

from fm_editor import card_text, glyph_cells, ttf, validate


def rows_of(lay):
    """The text of each row, as the box lays it."""
    out = {}
    for c, column, row in lay.glyphs:
        out.setdefault(row, {})[column] = c
    return ["".join(out.get(r, {}).get(x, " ") for x in range(card_text.COLUMNS)).rstrip() for r in range(lay.rows)]


class LayoutTest(unittest.TestCase):
    def test_lines_of_twenty_letters(self):
        lay = card_text.layout("A delicate elf that lacks in offense but has a terrific defense backed by mystical power.")
        self.assertEqual(rows_of(lay), ["A delicate elf that", "lacks in offense but", "has a terrific",
                                        "defense backed by", "mystical power."])
        self.assertEqual(lay.cut_rows, [])
        self.assertEqual(lay.hidden, 0)

    def test_the_box_cuts_a_long_word(self):
        # What the game drew for this text (checked in the card view): 21
        # letters on the first row, the rest of the word on the next, and
        # the next word on a row of its own, as the port's line count still
        # holds the whole word; nine rows drawn, the rest never.
        text = "ABCDEFGHIJKLMNOPQRSTUVWXY two\n" + "\n".join(f"row {i} of the text" for i in range(3, 13))
        lay = card_text.layout(text)
        rows = rows_of(lay)
        self.assertEqual(rows[:4], ["ABCDEFGHIJKLMNOPQRSTU", "VWXY", "two", "row 3 of the text"])
        self.assertEqual(rows[card_text.SHOWN_ROWS - 1], "row 8 of the text")
        self.assertEqual(lay.cut_rows, [1])
        self.assertEqual(lay.rows, 13)
        self.assertEqual(lay.hidden, 4)
        self.assertEqual(lay.lines, validate.text_lines(text))

    def test_twenty_one_letters_fit(self):
        lay = card_text.layout("ABCDEFGHIJKLMNOPQRSTU")
        self.assertEqual((lay.rows, lay.cut_rows), (1, []))

    def test_line_breaks_and_spaces(self):
        lay = card_text.layout("one\n\ntwo   three")
        self.assertEqual(rows_of(lay), ["one", "", "two three"])
        self.assertEqual(card_text.layout("").rows, 0)

    def test_the_count_matches_the_tab(self):
        for text in ("a b c", "x" * 45, "word " * 40, "a\nb\nc\nd\ne\nf\ng\nh\ni"):
            self.assertEqual(card_text.layout(text).lines, validate.text_lines(text))


def synthetic_wa():
    """The boot package's font page and colors where the retail disc has
    them: an 'A' that is a 6 x 9 block (index 15, its outline index 1) and
    a ramp from black to white."""
    wa = bytearray((card_text.RAMP_SECTOR + 1) * 2048)
    u, v = card_text._cell_uv("A")
    base = card_text.BOOT_SECTOR * 2048
    for y in range(12):
        for x in range(8):
            index = 15 if 1 <= x <= 6 and 1 <= y <= 9 else 1 if y <= 10 else 0
            tu, tv = u + x, v + y
            at = base + tv * 128 + tu // 2
            wa[at] |= index << (4 * (tu & 1))
    ramp = card_text.RAMP_SECTOR * 2048
    for i in range(16):
        level = i * 31 // 15
        struct.pack_into("<H", wa, ramp + 2 * i, level | level << 5 | level << 10)
    return bytes(wa)


def icon_wa():
    """synthetic_wa with icon 0 (Dragon) drawn: index 1 at its top left
    texel, 2 elsewhere, and its palette (0x200, 0xF9) red and green."""
    wa = bytearray(synthetic_wa())
    wa += bytes(max(0, (card_text.ICON_CLUT_SECTOR + 2) * 2048 - len(wa)))
    page = card_text.ICON_SECTOR * 2048
    for y in range(16):
        for x in range(16):
            u = 0x80 + x
            wa[page + y * 128 + u // 2] |= (1 if x == y == 0 else 2) << 4 * (u & 1)
    clut = card_text.ICON_CLUT_SECTOR * 2048 + (0xF9 - 0xF8) * 512
    struct.pack_into("<HHH", wa, clut, 0, 31, 31 << 5)
    return bytes(wa)


class IconTest(unittest.TestCase):
    """{f8 0B NN} and {f8 0A NN}: the icon off the disc, two letters wide, and
    the color ramps (notes/more-cards.md, "Card text codes")."""

    def test_icon_and_widths(self):
        font = card_text.RetailFont(icon_wa())
        width, height, rgba = font.icon(0)
        self.assertEqual((width, height), (16, 16))
        self.assertEqual(rgba[:4], bytes((248, 0, 0, 255)))
        self.assertEqual(rgba[4:8], bytes((0, 248, 0, 255)))
        self.assertIsNone(font.icon(1))                  # no palette there on this disc
        self.assertIsNone(font.icon(len(card_text.ICON_NAMES)))
        lay = card_text.layout("ab{f8 0B 00}c")
        self.assertEqual([(c, x) for c, x, _ in lay.glyphs], [("a", 0), ("b", 1), ("{f8 0B 00}", 2), ("c", 4)])
        # Twenty letters a line with the icon's two.
        self.assertEqual(card_text.layout("x" * 15 + " {f8 0B 00} y").rows, 1)
        self.assertEqual(card_text.layout("x" * 16 + " {f8 0B 00} y").rows, 2)

    def test_colors_and_picture(self):
        font = card_text.RetailFont(icon_wa())
        self.assertEqual(len(font.ramps), 8)
        lay = card_text.layout("A{f8 0A 02}A{f8 0A 00}A")
        self.assertEqual(lay.colors, [0, 2, 0])
        image, _ = card_text.Renderer(font).render("{f8 0B 00}", 1)
        # Drawn 2 texels up from its cell (its red top left is off the box),
        # 16 across: past its own cell into the next.
        self.assertEqual(image.pixel(0, 0)[:3], (0, 248, 0))
        self.assertEqual(image.pixel(15, 13)[:3], (0, 248, 0))
        self.assertEqual(image.pixel(16, 5)[:3], card_text.PANEL)


class RetailFontTest(unittest.TestCase):
    def test_cells_and_colors(self):
        font = card_text.RetailFont(synthetic_wa())
        cell = font.cell("A")
        self.assertEqual(cell[1 * 8 + 1], 15)
        self.assertEqual(cell[0], 1)
        self.assertEqual(cell[11 * 8], 0)
        self.assertEqual(font.colors[15], (248, 248, 248))
        self.assertEqual(font.cell("À"), cell)          # drawn as its plain letter
        self.assertIsNone(font.cell("☺"))

    def test_no_font(self):
        with self.assertRaises(ValueError):
            card_text.RetailFont(bytes(card_text.RAMP_SECTOR * 2048 + 64))

    def test_picture(self):
        font = card_text.RetailFont(synthetic_wa())
        image, lay = card_text.Renderer(font).render("☺A", 2)
        self.assertEqual(image.size, ((card_text.COLUMNS * 8 + card_text.GUTTER) * 2, card_text.SHOWN_ROWS * 24))
        # The A is the second glyph: its block, each texel 2 x 2 pixels.
        self.assertEqual(image.pixel(8 * 2 + 2, 2)[:3], (248, 248, 248))
        self.assertEqual(image.pixel(8 * 2 + 3, 3)[:3], (248, 248, 248))
        self.assertEqual(image.pixel(8 * 2, 0)[:3], font.colors[1])
        self.assertEqual(image.pixel(30 * 2, 30)[:3], card_text.PANEL)
        # The ninth row is the frame's; the smiley has no retail glyph: a red box.
        self.assertEqual(image.pixel(100, 8 * 24 + 5)[:3], card_text.FRAME)
        self.assertEqual(image.pixel(0, 0)[:3], card_text.MARK)


def tiny_font(path):
    """A TrueType file with one glyph, a square with a square hole, for 'A'."""
    def table(tag, body):
        return tag, body
    outer = [(100, 100), (100, 700), (700, 700), (700, 100)]      # clockwise, y up
    inner = [(300, 300), (500, 300), (500, 500), (300, 500)]
    points = outer + inner
    ends = struct.pack(">hh", 3, 7)
    flags = bytes([1] * 8)
    xs = b"".join(struct.pack(">h", x - px) for (x, _), (px, _) in zip(points, [(0, 0)] + points))
    ys = b"".join(struct.pack(">h", y - py) for (_, y), (_, py) in zip(points, [(0, 0)] + points))
    glyph = struct.pack(">hhhhh", 2, 100, 100, 700, 700) + ends + struct.pack(">H", 0) + flags + xs + ys
    glyph += b"\0" * (-len(glyph) % 4)
    glyf = glyph
    loca = struct.pack(">HHH", 0, 0, len(glyph) // 2)                # glyph 0 empty, glyph 1 the square
    head = bytearray(54)
    struct.pack_into(">H", head, 18, 1000)
    struct.pack_into(">h", head, 50, 0)
    hhea = bytearray(36)
    struct.pack_into(">H", hhea, 34, 2)
    maxp = struct.pack(">IH", 0x5000, 2)
    hmtx = struct.pack(">HhHh", 500, 0, 800, 100)
    segments = [(0x41, 0x41, 1 - 0x41), (0xFFFF, 0xFFFF, 1)]
    cmap4 = struct.pack(">HHHHHHH", 4, 16 + 8 * len(segments), 0, 2 * len(segments), 0, 0, 0)
    cmap4 += b"".join(struct.pack(">H", e) for _, e, _ in segments) + b"\0\0"
    cmap4 += b"".join(struct.pack(">H", s) for s, _, _ in segments)
    cmap4 += b"".join(struct.pack(">h", d) for _, _, d in segments)
    cmap4 += b"\0\0" * len(segments)
    cmap = struct.pack(">HHHHI", 0, 1, 3, 1, 12) + cmap4
    name = "Tiny".encode("utf-16-be")
    names = struct.pack(">HHH", 0, 1, 18) + struct.pack(">6H", 3, 1, 0x409, 4, len(name), 0) + name
    tables = [table(b"cmap", cmap), table(b"glyf", glyf), table(b"head", bytes(head)), table(b"hhea", bytes(hhea)),
              table(b"hmtx", hmtx), table(b"loca", loca), table(b"maxp", maxp), table(b"name", names)]
    out = bytearray(struct.pack(">IHHHH", 0x10000, len(tables), 0, 0, 0))
    offset = 12 + 16 * len(tables)
    bodies = b""
    for tag, body in tables:
        out += struct.pack(">4sIII", tag, 0, offset + len(bodies), len(body))
        bodies += body + b"\0" * (-len(body) % 4)
    Path(path).write_bytes(bytes(out) + bodies)


class TrueTypeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.path = Path(cls.tmp.name) / "tiny.ttf"
        tiny_font(cls.path)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_reads_the_font(self):
        font = ttf.Font(self.path)
        self.assertEqual(font.name, "Tiny")
        self.assertTrue(font.has(0x41))
        self.assertFalse(font.has(0x42))
        self.assertAlmostEqual(font.advance(0x41), 0.8)
        for got, want in zip(ttf.bbox(font.outline(0x41)), (0.1, 0.1, 0.7, 0.7)):
            self.assertAlmostEqual(got, want)
        self.assertIsNone(font.outline(0x42))

    def test_fill_keeps_the_hole(self):
        font = ttf.Font(self.path)
        square = [[(x * 10, (1 - y) * 10) for x, y in c] for c in font.outline(0x41)]   # 1 unit = 10 px, y down
        cover = ttf.fill(square, 10, 10)
        self.assertEqual(cover[5 * 10 + 1], 255)       # the ring
        self.assertEqual(cover[6 * 10 + 3], 0)         # the hole
        self.assertEqual(cover[0], 0)                  # outside
        heavy = ttf.embolden(square, 1.0, 1.0)
        self.assertLess(ttf.bbox(heavy)[0], ttf.bbox(square)[0])
        self.assertGreater(ttf.bbox(heavy)[2], ttf.bbox(square)[2])

    def test_refuses_what_it_cannot_read(self):
        other = Path(self.tmp.name) / "cff.otf"
        other.write_bytes(b"OTTO" + bytes(64))
        with self.assertRaises(ttf.FontError):
            ttf.Font(other)
        other.write_bytes(b"not a font at all")
        with self.assertRaises(ttf.FontError):
            ttf.Font(other)

    def test_a_face_without_lines_keeps_the_retail_letters(self):
        # The port sets nothing in a face whose lines it cannot measure; the
        # preview draws the retail cell as it does.
        retail = card_text.RetailFont(synthetic_wa())
        plain, _ = card_text.Renderer(retail).render("A", 2)
        renderer = card_text.Renderer(retail, ttf.Font(self.path))
        hd, _ = renderer.render("A", 2)
        self.assertFalse(renderer.face_ok)
        self.assertEqual(hd, plain)


class GlyphCellsTest(unittest.TestCase):
    """glyph_cells.py: the letters the port makes (src/pc/text/glyphs.c), on
    the synthetic font (an 'A' block, no other letter)."""

    def setUp(self):
        self.retail = card_text.RetailFont(synthetic_wa())

    def test_kinds(self):
        cells = glyph_cells.GlyphCells(self.retail)
        self.assertEqual(cells.kind("A"), "retail")
        self.assertEqual(cells.kind("’"), "retail")       # typed for '
        self.assertEqual(cells.kind("«"), "retail")       # drawn as <
        self.assertEqual(cells.kind("Á"), "composed")
        self.assertEqual(cells.kind("ệ"), "composed")     # e, ^ and a dot below
        self.assertEqual(cells.kind("¿"), "composed")     # ? turned
        self.assertEqual(cells.kind("ß"), "drawn")
        self.assertEqual(cells.kind(";"), "none")              # no retail glyph: a font's, and there is none
        self.assertIsNone(cells.cell("☺"))
        self.assertEqual(cells.cell("A"), self.retail.cell("A"))
        self.assertEqual(cells.cell(" "), [0] * 96)

    def test_a_capital_gives_up_a_row_for_its_mark(self):
        cell = glyph_cells.GlyphCells(self.retail).cell("Á")
        a = self.retail.cell("A")
        # The acute's two pixels in the letter's color, over its middle, the
        # outline round them; the block a row shorter, its foot where it was.
        self.assertEqual((cell[0 * 8 + 5], cell[1 * 8 + 4]), (15, 15))
        self.assertEqual((cell[0 * 8 + 4], cell[0 * 8 + 6]), (1, 1))
        self.assertEqual(cell[8 * 8:], a[8 * 8:])
        self.assertEqual(cell[2 * 8:8 * 8], a[1 * 8:7 * 8])

    def test_drawn_letter_is_shaded_and_outlined(self):
        cell = glyph_cells.GlyphCells(self.retail).cell("ß")
        row = glyph_cells.LETTERS["ß"]
        for y in range(12):
            for x in range(8):
                if row[y][x] == "#":
                    self.assertEqual(cell[y * 8 + x], 15)    # the capitals' shade on every inked row here
        self.assertEqual(cell[2 * 8 + 1], 1)
        self.assertEqual(cell[0], 0)

    def test_font_fallback(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        tiny_font(Path(tmp.name) / "tiny.ttf")
        face = ttf.Font(Path(tmp.name) / "tiny.ttf")
        face.cmap[0x263A] = face.cmap[0x41]                    # the square for the smiley
        cells = glyph_cells.GlyphCells(self.retail, face)
        self.assertEqual(cells.kind("☺"), "font")
        cell = cells.cell("☺")
        # 0.1-0.7 em at 10 pixels: rows 3-8 under the baseline row 10, the
        # six columns from 1; the hole outlined.
        self.assertEqual(cell[3 * 8 + 1], 15)
        self.assertEqual(cell[8 * 8 + 6], 15)
        self.assertEqual(cell[5 * 8 + 3], 1)
        self.assertEqual(cell[2 * 8 + 1], 1)
        self.assertEqual(cell[1 * 8 + 1], 0)

    def test_matches_the_port(self):
        """Every composed and drawn letter, US and European, against
        glyphs.c built here on the game's own font (skipped without the
        game files, a C compiler or FreeType and fontconfig)."""
        root = Path(__file__).resolve().parents[4]
        wa, exe = root / "game/DATA/WA_MRG.MRG", root / "game/SLUS_014.11"
        cc = shutil.which("cc") or shutil.which("gcc")
        if not (wa.is_file() and exe.is_file() and cc and shutil.which("pkg-config")) or os.name == "nt":
            self.skipTest("needs the game files, cc and pkg-config")
        flags = subprocess.run(["pkg-config", "--cflags", "--libs", "freetype2", "fontconfig"], capture_output=True,
                               text=True)
        if flags.returncode:
            self.skipTest("needs FreeType and fontconfig")
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        source, program = Path(tmp.name) / "harness.c", Path(tmp.name) / "harness"
        source.write_text(PARITY_HARNESS)
        built = subprocess.run([cc, "-O1", "-w", "-I" + str(root / "src"), str(source), str(root / "src/pc/text/serif.c"),
                                "-o", str(program), *flags.stdout.split()], capture_output=True, text=True)
        if built.returncode:
            self.skipTest("the harness does not build here: " + built.stderr[-300:])
        out = subprocess.run([str(program), str(exe), str(wa)], capture_output=True, text=True, check=True).stdout
        retail = card_text.RetailFont(wa.read_bytes())
        cells = [glyph_cells.GlyphCells(retail, european=e) for e in (False, True)]
        lines = [line.split() for line in out.splitlines()]
        self.assertGreater(len(lines), 700)
        for code, european, data in lines:
            with self.subTest(character=code, european=european):
                self.assertEqual("".join("%x" % v for v in cells[int(european)].cell(chr(int(code, 16)))), data)


# The C side of test_matches_the_port: glyphs.c with the font page where the
# game has it (VRAM 0x280, 0) and the executable's glyph table where the
# game has it; each composed and drawn letter's 8x12 cell, and the retail i
# and l with the European serifs.
PARITY_HARNESS = r"""
#include <sys/mman.h>
#include "pc/text/glyphs.c"
static uint16_t vram[SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT];
const uint16_t *SoftGpu_Vram(void) { return vram; }
const uint16_t *SoftGpu_VramRaw(void) { return vram; }
void SoftGpu_Settle(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
uint16_t *SoftGpu_Bank(int bank) { (void)bank; return NULL; }
int Log_Wanted(LogChannel c) { (void)c; return 0; }
void Log_Printf(LogChannel c, const char *f, ...) { (void)c; (void)f; }
static void dump(uint32_t character, int eu)
{
    int code = Glyphs_Code(character), x, y;
    Cell cell;
    if (code < GLYPHS_EXTENDED_FIRST) {
        if (!eu || (character != 'i' && character != 'l')) return;
        read_cell(0xA, (char)character, FONT_SMALL, &cell);
        add_serifs(&cell, 0);
    } else if (added[code - GLYPHS_EXTENDED_FIRST].letter) {
        compose(&added[code - GLYPHS_EXTENDED_FIRST], 0xA, FONT_SMALL, &cell);
    } else {
        render(&added[code - GLYPHS_EXTENDED_FIRST], 0xA, FONT_SMALL, &cell);
    }
    printf("%04X %d ", (unsigned)character, eu);
    for (y = 0; y < 12; y++) for (x = 0; x < 8; x++) printf("%x", cell.pixels[y][x]);
    printf("\n");
}
int main(int argc, char **argv)
{
    FILE *f;
    unsigned char *page = malloc(16 * 2048);
    size_t a;
    int y, eu;
    void *at = mmap((void *)0x801D9000u, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (argc < 3 || at != (void *)0x801D9000u || !(f = fopen(argv[1], "rb"))) return 1;
    fseek(f, 0x801D9000L - 0x80010000L + 0x800L, SEEK_SET);
    if (fread(at, 4, 0x5C, f) != 0x5C) return 1;
    fclose(f);
    if (!(f = fopen(argv[2], "rb"))) return 1;
    fseek(f, 0x1690L * 2048, SEEK_SET);
    if (fread(page, 1, 16 * 2048, f) != 16 * 2048) return 1;
    fclose(f);
    for (y = 0; y < 256; y++) memcpy(&vram[y * SOFT_GPU_WIDTH + 0x280], page + y * 128, 128);
    for (eu = 0; eu < 2; eu++) {
        Glyphs_SetEuropean(eu);
        for (a = 0; a < sizeof(accents) / sizeof(accents[0]); a++) dump(accents[a].character, eu);
        for (a = 0; a < sizeof(letters) / sizeof(letters[0]); a++) dump(letters[a].character, eu);
        dump('i', eu);
        dump('l', eu);
    }
    return 0;
}
"""


if __name__ == "__main__":
    unittest.main()
