#!/usr/bin/env python3
"""The model modules' gate table without a disc: config/pc/model_gate.csv is
well formed, src/pc/overlays/model_gate_table.inc is what it gives, the build
has a gated module for each slot of each native shape, and the committed
config/pc/guest_addresses.txt has the sections the C gives
(tools/pc/model_gate.py; notes/pc-build.md, "MIPS-only effects")."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from array import array  # noqa: E402
from unittest import mock  # noqa: E402
import build_config  # noqa: E402
import model_gate  # noqa: E402
import model_oracle  # noqa: E402


class ModelGate(unittest.TestCase):
    def setUp(self):
        self.rows = model_gate.load_table()

    def test_table_is_well_formed(self):
        self.assertEqual(model_gate.problems(self.rows), [])
        self.assertGreater(len(self.rows), 300)

    def test_one_native_shape_at_least(self):
        self.assertTrue(model_gate.native_rows())

    def test_header_matches_table(self):
        text = model_gate.HEADER.read_text(encoding="utf-8").replace("\r\n", "\n")
        self.assertEqual(text, model_gate.header_text(self.rows))

    def test_sources_and_address_sections(self):
        self.assertEqual(model_gate.check_sources(self.rows), [])

    def test_retail_list_matches_table(self):
        """config/pc/model_retail.csv: code rows for each native shape, images
        for the rest, and its table is what it gives."""
        retail = model_gate.load_retail()
        self.assertEqual(model_gate.retail_problems(self.rows, retail, model_gate.primary_rows()), [])
        text = model_gate.RETAIL_HEADER.read_text(encoding="utf-8").replace("\r\n", "\n")
        self.assertEqual(text, model_gate.retail_header_text(retail))
        self.assertEqual(sum(entry["kind"] == "primary" for entry in retail), 2 * len(model_gate.primary_rows()))

    def test_retail_problems_names_a_missing_or_wrong_row(self):
        retail = model_gate.load_retail()
        native = next(row for row in self.rows if row["backend"] == "native")
        # A row the port still interprets (none on the disc now): its code row is its own hash.
        other = model_gate.Row(dict(native, backend="interpret", data_start="", note=""))
        without = [entry for entry in retail if not (entry["shape"] == str(native.shape) and entry["slot"] == "B")]
        self.assertTrue(any(f"shape {native.shape} slot B has 0 code rows" in line
                            for line in model_gate.retail_problems(self.rows, without)))
        stale = [dict(entry, size="0x4") if entry["shape"] == str(native.shape) else entry for entry in retail]
        self.assertTrue(any(f"shape {native.shape} slot A: its code row" in line
                            for line in model_gate.retail_problems(self.rows, stale)))
        own = [{"kind": "code", "slot": slot, "identifier": str(other.identifier(slot)), "shape": str(other.shape),
                "size": f"0x{other.span:X}", "sha256": "0" * 64 if slot == "A" else other.sha256(slot), "records": ""}
               for slot in "AB"]
        self.assertTrue(any(f"shape {other.shape} slot A: its code row's hash" in line
                            for line in model_gate.retail_problems([other], own)))

    def test_the_disc_runs_no_module_on_the_interpreter(self):
        """Every variant and primary shape of the disc is native: a shape left
        on the interpreter (or kept) fails here, in CI, without the disc. The
        oracle's sweep is the test with the disc (notes/pc-build.md). No row may
        be `keep` or `interpret` any more: a new one needs this test changed."""
        for table in (self.rows, model_gate.primary_rows()):
            self.assertTrue(table)
            self.assertEqual([row.shape for row in table if row["backend"] != "native"], [])

    def test_every_native_slot_is_a_gated_module(self):
        modules = {name: (pattern, bank) for name, pattern, _, bank in build_config.MODULES}
        for row in model_gate.native_rows():
            for slot in "AB":
                name = row.module(slot)
                self.assertIn(name, modules)
                self.assertIn(name, build_config.GATED_MODULES)
                self.assertIn(name, build_config.SOURCE_ADDRESSED_MODULES)
                self.assertEqual(modules[name][1], model_gate.BASE[slot])

    def test_entry_is_the_function_at_base_plus_4(self):
        for row in model_gate.native_rows():
            for slot in "AB":
                names = model_gate.module_addresses(row, slot)
                entry = row.entry(slot).split("__", 1)[1]
                self.assertEqual(names.get(entry), model_gate.BASE[slot] + 4)

    def test_kept_rows_say_why(self):
        kept = [row for row in self.rows if row["backend"] == "keep"]
        for row in kept:
            self.assertTrue(row["note"].strip())


def side(name, ways_a, ways_b="interpreter"):
    """A battle's run as model_oracle.read_traces gives it, for a tie."""
    return {"side": name, "battle": "34/0/tie", "start": 0, "end": 3, "hashes": array("Q", [1, 2, 3]),
            "calls": [(1, "variant", "A", 418, 1), (1, "variant", "B", 568, 1)],
            "updates": {"variant A": 2, "variant B": 2}, "cannot_run": [],
            "paths": {"variant 418 in A": [ways_a], "variant 568 in B": [ways_b]}}


class SpanRules(unittest.TestCase):
    """What a native row's span has to satisfy."""

    @staticmethod
    def shape(first_difference):
        shape = model_gate.Shape(418, 568)
        for slot in "AB":
            for fill in (1, 2):
                image = bytearray(model_gate.IMAGE_SIZE)
                image[first_difference:] = bytes([fill]) * (model_gate.IMAGE_SIZE - first_difference)
                shape.images[slot][str(fill)] = bytes(image)
        return shape

    def test_images_must_agree_within_the_span(self):
        shape = self.shape(0x400)
        self.assertEqual(shape.common("A"), 0x400)
        model_gate.hashed(shape, "A", 0x400)
        with self.assertRaises(SystemExit):
            model_gate.hashed(shape, "A", 0x404)

    def test_a_span_may_end_inside_a_word(self):
        """A table whose last entry ends two bytes into a word that differs per
        record past it: the span stops at its last byte."""
        shape = self.shape(0x402)
        self.assertEqual(shape.common("A"), 0x400)
        self.assertEqual(shape.common("A", 1), 0x402)
        model_gate.hashed(shape, "A", 0x402)
        with self.assertRaises(SystemExit):
            model_gate.hashed(shape, "A", 0x403)
        self.assertEqual(model_gate.problems([self.row(span="0x3F62")]), [])
        self.assertTrue(model_gate.problems([self.row(span="0x5001")]))

    def test_printf_format_strings_are_found(self):
        """A format string from the slot's data handed to printf, directly or
        cast, is found by its symbol (native C passes it to the host's printf)."""
        text = 'printf(D_8013D298, sizeof(Model643State));\nprintf((char *)D_8013CB00, x);\nGPU_printf(D_8013C000);'
        self.assertEqual(model_gate.PRINTF.findall(text), ["D_8013D298", "D_8013CB00"])

    def test_a_format_string_past_the_span_is_a_problem(self):
        shape = self.shape(0x400)
        for image in list(shape.images["A"]):
            data = bytearray(shape.images["A"][image])
            data[0x200:0x208] = b"%d bad\0\0"
            shape.images["A"][image] = bytes(data)
        row = self.row(span="0x204")
        with mock.patch.object(model_gate, "printf_formats", return_value=[0x200]):
            self.assertTrue(model_gate.printf_problems({418: shape}, [row]))
            self.assertFalse(model_gate.printf_problems({418: shape}, [self.row(span="0x208")]))

    def row(self, **fields):
        base = dict(shape="418", backend="native", span="0x3F60", data_start="0x396C", sources="",
                    command_table="0x3A68:0x44:18:0x1B74", identifier_a="418", identifier_b="568", images="11", sha256_a="a" * 64,
                    sha256_b="b" * 64, note="widened: divides by slot words")
        base.update(fields)
        return model_gate.Row(base)

    def test_a_span_wider_than_the_data_start_says_why(self):
        self.assertEqual(model_gate.problems([self.row()]), [])
        self.assertTrue(model_gate.problems([self.row(note="")]))

    def test_a_span_may_not_stop_before_the_data_start(self):
        self.assertTrue(model_gate.problems([self.row(span="0x3000")]))

    def test_a_native_row_gives_its_data_start(self):
        self.assertTrue(model_gate.problems([self.row(data_start="")]))

    def test_a_native_row_bounds_its_commands_or_says_it_takes_none(self):
        self.assertTrue(model_gate.problems([self.row(command_table="")]))
        self.assertEqual(model_gate.problems([self.row(command_table="none")]), [])
        self.assertIsNone(self.row(command_table="none").command_table)
        self.assertEqual(self.row().command_table, (0x3A68, 0x44, 18, 0x1B74, 0))

    def test_the_hash_must_cover_every_entry_the_commands_can_pick(self):
        # 19 entries of 0x44 from 0x3A68 end at 0x3F74, past the span 0x3F60.
        self.assertTrue(model_gate.problems([self.row(command_table="0x3A68:0x44:19:0x1B74")]))
        self.assertEqual(model_gate.problems([self.row(command_table="0x3A68:0x44:18:0x1B74")]), [])
        self.assertTrue(model_gate.problems([self.row(command_table="0x3000:0x44:18:0x1B74")]))
        self.assertTrue(model_gate.problems([self.row(command_table="0x3A68:0x44:x:0x1B74")]))

    def test_the_header_carries_the_bound(self):
        text = model_gate.header_text([self.row(), self.row(shape="500", command_table="none", sha256_a="c" * 64,
                                                            sha256_b="d" * 64, identifier_a="500",
                                                            identifier_b="650")])
        self.assertIn("0x12u, 0x3A68u, 0x44u, 0x1B74u, 0u, 0u)", text)
        self.assertIn("0xFFFFFFFFu, 0u, 0u, 0u, 0u, 0u)", text)

    def test_a_command_table_may_bound_an_index_word(self):
        """`:indexN` says the work area keeps the command itself as a uN."""
        for suffix, width in (("index8", 1), ("index16", 2), ("index32", 4)):
            row = self.row(command_table=f"0x3A68:0x44:18:0x20:{suffix}")
            self.assertEqual(row.command_table, (0x3A68, 0x44, 18, 0x20, width))
            self.assertEqual(model_gate.problems([row]), [])
        self.assertTrue(model_gate.problems([self.row(command_table="0x3A68:0x44:18:0x20:index64")]))
        self.assertTrue(model_gate.problems([self.row(command_table="0x3A68:0x44:18:0x20:pointer")]))
        self.assertTrue(model_gate.problems([self.row(command_table="0x3A68:0x44:18:0x20:index8:x")]))

    def test_a_command_table_may_take_the_command_modulo(self):
        """`:modN` (last) says the C picks entry command % N: a pointer row's,
        never with an index (its updates compare the stored command itself)."""
        row = self.row(command_table="0x3A68:0x44:18:0x20:mod100")
        self.assertEqual((row.command_table, row.command_modulus), ((0x3A68, 0x44, 18, 0x20, 0), 100))
        self.assertEqual(model_gate.problems([row]), [])
        self.assertIn(", 100u)", model_gate.header_text([row]))
        self.assertTrue(model_gate.problems([self.row(command_table="0x3A68:0x44:18:0x20:index8:mod100")]))
        self.assertTrue(model_gate.problems([self.row(command_table="0x3A68:0x44:18:0x20:mod0")]))
        self.assertTrue(model_gate.problems([self.row(command_table="0x3A68:0x44:18:0x20:mod100:index8")]))

    def test_a_command_table_is_well_formed(self):
        """No negative number (it would not compile in the table), a field inside a
        work area, and an index that can name the entries it covers."""
        for bad in ("-0x1:0x44:18:0x20", "0x3A68:-0x44:18:0x20", "0x3A68:0x44:-18:0x20", "0x3A68:0x44:18:-0x20:index8"):
            self.assertTrue(model_gate.problems([self.row(command_table=bad)]), bad)
        self.assertTrue(model_gate.problems([self.row(command_table="0x3A68:0x44:18:0x10000")]))
        self.assertTrue(model_gate.problems([self.row(command_table="0x3A68:0x44:18:0xFFFF:index32")]))
        self.assertEqual(model_gate.problems([self.row(command_table="0x3A68:0x44:18:0xFFFC:index32")]), [])
        wide = self.row(span="0x6000", command_table="0x3A68:0x44:257:0x20:index8")
        self.assertTrue([p for p in model_gate.problems([wide]) if "names 256 at most" in p])
        self.assertFalse([p for p in model_gate.problems([self.row(command_table="0x3A68:0x44:18:0x20:index8")]) if "at most" in p])

    def test_the_header_carries_the_index_width(self):
        text = model_gate.header_text([self.row(command_table="0x3A68:0x44:18:0x20:index16")])
        self.assertIn("0x12u, 0x3A68u, 0x44u, 0x20u, 2u, 0u)", text)

    def test_the_tables_the_command_indexes_are_found(self):
        """Each form the decomp's C picks a row with; a wrapper's #define puts
        C written at another address at this one."""
        text = chr(10).join([
            "work->config = &D_8013D674[command];",
            "/* D_8013D000[command] is in a comment */",
            "work->config = (Variant460Config *)(D_8013CDCC + 0xFC) + command;",
            "work->radius = D_8013C800[work->command].radius;",
            "config = &D_8013D298[command % 100];",
            "#define COLOR(i) (D_8013D418[(work)->command].color[i])",
            "x = D_8013C7C8[1];",
        ])
        offsets = sorted(offset for offset, _ in model_gate.command_sites(text))
        self.assertEqual(offsets, sorted([0x2674, 0x1800, 0x2298, 0x1DCC + 0xFC, 0x2418]))
        self.assertEqual(model_gate.command_sites("work->config = &D_8013D674[command];", {"D_8013D674": "D_8013D700"}),
                         [(0x2700, 1)])

    def test_a_native_row_that_indexes_a_table_by_the_command_bounds_it(self):
        """A `none` row for C that indexes a table of the slot's data with the
        command, past the span, let a mod's row run the C past its work area
        (shape 834 of a later batch)."""
        found = lambda row: [(0x2674, "src/native_overlays/model_variant/variant834_entry.c", 39)]
        self.assertTrue(model_gate.command_problems([self.row(shape="834", command_table="none", span="0x2620")], found))
        self.assertTrue(model_gate.command_problems([self.row(shape="834", command_table="")], found))
        self.assertEqual(model_gate.command_problems([self.row(shape="834", command_table="0x2674:0x26:2:0x0")], found), [])
        self.assertEqual(model_gate.command_problems([self.row(shape="834", command_table="0x2674:0x26:2:0x0:index8")], found), [])
        # against the real C of a shape native here: 418's pointer form
        real = {r.shape: r for r in model_gate.load_table()}[418]
        self.assertEqual(model_gate.command_problems([real]), [])
        self.assertTrue(model_gate.command_problems([self.row(shape="418", command_table="none")]))
        # a table the bound does not cover is a problem too, and so is a row past its entries
        self.assertTrue(model_gate.command_problems([self.row(shape="834", command_table="0x2700:0x26:2:0x0")], found))
        self.assertTrue(model_gate.command_problems([self.row(shape="834", command_table="0x2600:0x26:2:0x0")], found))
        # C that takes no command index may say `none`; a row that is not native is not looked at
        self.assertEqual(model_gate.command_problems([self.row(command_table="none")], lambda row: []), [])
        self.assertEqual(model_gate.command_problems([self.row(backend="keep", command_table="")], found), [])

    def test_the_committed_418_row_bounds_to_the_retail_table(self):
        row = next(r for r in model_gate.native_rows() if r.shape == 418)
        table, stride, count, field, width = row.command_table
        self.assertEqual((count, stride), (18, 0x44))
        self.assertEqual(table + count * stride, 0x3F30)   # where the retail table ends, inside the span
        self.assertLessEqual(table + count * stride, row.span)

    def test_a_module_may_be_made_of_two_headers(self):
        # A shape config/pc/model_gate_sources.txt does not list (shape 9999)
        # falls back to the globs of its headers.
        row = self.row(shape="9999", sources="433;450")
        self.assertEqual(row.sources, [433, 450])
        self.assertEqual(len(model_gate.source_patterns(row, "A")), 2)
        self.assertEqual(self.row().sources, [418])


class PortRules(unittest.TestCase):
    """model_gate_port.py's rules for where upstream's files go, and the
    addresses a module's C gives through a wrapper's #defines."""

    def test_files_keep_their_folder_and_slot_b_wrappers_get_their_own(self):
        import model_gate_port as port
        self.assertEqual(port.port_path("src/overlays/model_variant/variant418_entry.c"),
                         "src/native_overlays/model_variant/variant418_entry.c")
        self.assertEqual(port.port_path("src/overlays/french_model_variant/variant76_entry_slot1.c", slot1=True),
                         "src/native_overlays/french_model_variant_slot1/variant76_entry_slot1.c")
        self.assertEqual(port.port_path("src/game/model_copy_slot_position.h"), "src/game/model_copy_slot_position.h")

    def test_includes_into_an_overlay_folder_are_rewritten_by_rule(self):
        import model_gate_port as port

        class Tree:
            files = {"src/overlays/model_variant/model_variant.h", "src/overlays/model_variant/variant1_entry.c",
                     "src/overlays/french_model_variant/variant2_entry.h", "src/types.h"}

            def read(self, path):
                return b"" if path in self.files else None

        tree = Tree()
        header = '#include "../../overlays/model_variant/model_variant.h"\n#include "../../types.h"\n'
        self.assertEqual(port.rewrite_includes(tree, "src/overlays/french_model_variant/variant2_entry.h", header, False),
                         '#include "../model_variant/model_variant.h"\n#include "../../types.h"\n')
        wrapper = '#include "../../types.h"\n#define func_8013B004 func_8017B004\n#include "variant1_entry.c"\n'
        self.assertEqual(port.rewrite_includes(tree, "src/overlays/model_variant/variant1_entry_slot1.c", wrapper, True),
                         wrapper.replace('"variant1_entry.c"', '"../model_variant/variant1_entry.c"'))
        # Slot A's own includes stay as upstream wrote them.
        self.assertEqual(port.rewrite_includes(tree, "src/overlays/model_variant/variant1_entry.c", wrapper, False),
                         wrapper)

    def test_a_wrapper_define_names_the_address_the_module_has(self):
        import tempfile
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "config/pc").mkdir(parents=True)
            (root / "src/native_overlays/model_variant").mkdir(parents=True)
            (root / "src/native_overlays/spanish_model_variant").mkdir(parents=True)
            (root / "src/native_overlays/spanish_model_variant/variant9_bands.c").write_text(
                "/* func_8013C000 in a comment is no name */\nvoid func_8013CAC4(void) {}\nextern int D_8013D000;\n")
            (root / "src/native_overlays/model_variant/variant7_bands.c").write_text(
                '#define func_8013CAC4 func_8013CB18\n#include "../spanish_model_variant/variant9_bands.c"\n')
            (root / "config/pc/model_gate_sources.txt").write_text(
                "[model_variant_7]\nsrc/native_overlays/model_variant/variant7_bands.c\n")
            row = SpanRules().row(shape="7", identifier_a="7")
            self.assertEqual(model_gate.module_addresses(row, "A", root),
                             {"func_8013CB18": 0x8013CB18, "D_8013D000": 0x8013D000})

    def test_a_stale_local_takes_the_slot_storage(self):
        import model_gate_port as port
        path = "src/native_overlays/model_variant/variant169_entry.c"
        (declaration, replacement, _), = port.STALE_LOCALS[path]
        text = '#include "../../types.h"\n#include "variant169_entry.h"\n\ns32 f(void)\n{\n' + declaration + "}\n"
        self.assertEqual(port.stale_local_text(path, text),
                         '#include "../../types.h"\n' + port.STALE_INCLUDE + '#include "variant169_entry.h"\n\n'
                         's32 f(void)\n{\n' + replacement + "}\n")
        # Another file is copied as it is; a declaration the rule cannot find
        # stops the port rather than copy the stack read.
        self.assertEqual(port.stale_local_text("src/native_overlays/model_variant/variant1_entry.c", text), text)
        with self.assertRaises(SystemExit):
            port.stale_local_text(path, text.replace(declaration, ""))

    def test_a_stale_struct_local_becomes_a_pointer(self):
        import model_gate_port as port
        path = "src/native_overlays/model_variant/variant163_entry.c"
        (declaration, replacement, _), = port.STALE_LOCALS[path]
        text = ('#include "../../types.h"\n\ns32 f(void)\n{\n' + declaration +
                "    SetPolyFT4(&quad);\n    quad.r0 = 1;\n    g(&quad.x0, (u32 *)&quad);\n"
                "    work->quad.r0 = quads.r0;\n}\n")
        self.assertEqual(port.stale_local_text(path, text),
                         '#include "../../types.h"\n' + port.STALE_INCLUDE + '\ns32 f(void)\n{\n' + replacement +
                         "    SetPolyFT4(quad);\n    quad->r0 = 1;\n    g(&quad->x0, (u32 *)quad);\n"
                         "    work->quad.r0 = quads.r0;\n}\n")

    def test_check_fails_a_native_copy_without_the_stale_local_rule(self):
        import tempfile
        import model_gate_port as port
        path = "src/native_overlays/model_variant/variant169_entry.c"
        (declaration, *_), = port.STALE_LOCALS[path]
        text = '#include "../../types.h"\n\ns32 f(void)\n{\n' + declaration + "}\n"
        listed = {"model_variant_169": [path]}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / path).parent.mkdir(parents=True)
            (root / path).write_text(text)
            self.assertEqual(len(port.stale_local_problems(listed, {"model_variant_169"}, root)), 1)
            self.assertEqual(port.stale_local_problems(listed, set(), root), [])   # not native: no rule needed
            (root / path).write_text(port.stale_local_text(path, text).replace("\n", "\r\n"), newline="")
            self.assertEqual(port.stale_local_problems(listed, {"model_variant_169"}, root), [])

    def test_a_stale_local_reached_through_another_address(self):
        """Shape 448's col: the declaration takes the storage, every (s32)&rot
        becomes ((s32)col - 240), and check fails a copy that keeps one."""
        import tempfile
        import model_gate_port as port
        path = "src/native_overlays/model_variant/variant448_fan.c"
        (declaration, storage, pointer, (old, new)), = port.STALE_LOCALS[path]
        text = ('#include "../../types.h"\n\ns32 f(void)\n{\n' + declaration +
                "    SVECTOR rot;\n    RotMatrix(&rot, 0);\n    u = t * 6 + (s32)&rot + 240;\n}\n")
        ported = port.stale_local_text(path, text)
        self.assertIn(storage, ported)
        self.assertIn("u = t * 6 + ((s32)col - 240) + 240;", ported)
        self.assertIn("RotMatrix(&rot, 0);", ported)
        self.assertNotIn(old, ported)
        listed = {"model_variant_448": [path]}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / path).parent.mkdir(parents=True)
            (root / path).write_text(ported)
            self.assertEqual(port.stale_local_problems(listed, {"model_variant_448"}, root), [])
            (root / path).write_text(ported + "    v = (s32)&rot + 256;\n")
            self.assertEqual(len(port.stale_local_problems(listed, {"model_variant_448"}, root)), 1)

    def test_a_file_takes_each_of_its_stale_locals(self):
        import model_gate_port as port
        path = "src/native_overlays/model_variant/variant769_entry.c"
        (quad, quad_storage, _), (screen, screen_storage, _) = port.STALE_LOCALS[path]
        text = ('#include "../../types.h"\n\ns32 f(void)\n{\n' + quad + screen +
                "    setlen(&quad, 9);\n    quad.u0 = 0;\n    box.x = screen[0].vx;\n}\n")
        self.assertEqual(port.stale_local_text(path, text),
                         '#include "../../types.h"\n' + port.STALE_INCLUDE + '\ns32 f(void)\n{\n' +
                         quad_storage + screen_storage +
                         "    setlen(quad, 9);\n    quad->u0 = 0;\n    box.x = screen[0].vx;\n}\n")
        with self.assertRaises(SystemExit):   # each declaration must be there
            port.stale_local_text(path, text.replace(screen, ""))

    def test_check_reads_the_files_a_native_unit_includes(self):
        # Shape 772's unit is a wrapper; rule 6 is on the body it includes.
        import tempfile
        import model_gate_port as port
        body = "src/native_overlays/french_model_variant/image31368_entry.c"
        unit = "src/native_overlays/model_variant/variant772_entry.c"
        text = "s32 f(void)\n{\n" + "".join(rule[0] for rule in port.STALE_LOCALS[body]) + "}\n"
        listed = {"model_variant_772": [unit]}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for path in (unit, body):
                (root / path).parent.mkdir(parents=True, exist_ok=True)
            (root / unit).write_text('#include "../french_model_variant/image31368_entry.c"\n')
            (root / body).write_text(text)
            self.assertEqual(len(port.stale_local_problems(listed, {"model_variant_772"}, root)), 2)
            (root / body).write_text(port.stale_local_text(body, text))
            self.assertEqual(port.stale_local_problems(listed, {"model_variant_772"}, root), [])

    def test_every_native_module_lists_its_files(self):
        listed = model_gate.listed_sources()
        for row in model_gate.native_rows():
            for slot in "AB":
                self.assertIn(row.module(slot), listed)
                for path in listed[row.module(slot)]:
                    self.assertTrue((model_gate.ROOT / path).is_file(), path)


class OracleBackends(unittest.TestCase):
    """The oracle's check that native C ran exactly where the table says."""

    def verdict(self, subject, reference, holds=None, allowed=(True, False)):
        return model_oracle.compare(subject, reference, holds or {"A": True, "B": False}, allowed)

    def test_native_where_the_table_says_is_a_match(self):
        verdict = self.verdict(side("subject", "native"), side("reference", "interpreter"))
        self.assertEqual(verdict["status"], "identical")

    @staticmethod
    def with_primary(run, ways):
        """A run that also called slot A's primary module (record 116's effect, 39)."""
        run["calls"] = run["calls"] + [(1, "primary", "A", 39, 0)]
        run["paths"] = dict(run["paths"], **{"primary 39 in A": [ways]})
        return run

    def test_primary_backends_are_checked_as_the_variants(self):
        subject = self.with_primary(side("subject", "native"), "native")
        reference = self.with_primary(side("reference", "interpreter"), "interpreter")
        holds = {"A": True, "B": False}
        self.assertEqual(model_oracle.compare(subject, reference, holds, (True, False), holds)["status"], "identical")
        subject = self.with_primary(side("subject", "native"), "interpreter")
        verdict = model_oracle.compare(subject, reference, holds, (True, False), holds)
        self.assertEqual(verdict["status"], "error")
        self.assertIn("slot A holds a primary module the table runs as native C and ran interpreter", verdict["why"])
        # The reference asked for the interpreter: native C there is an error.
        reference = self.with_primary(side("reference", "interpreter"), "native")
        verdict = model_oracle.compare(self.with_primary(side("subject", "native"), "native"), reference, holds,
                                       (True, False), holds)
        self.assertIn("ran native C for its primary module", verdict["why"])
        # A slot whose primary holds native code but made no primary call (a
        # record without one calls none) is no error.
        self.assertEqual(model_oracle.compare(side("subject", "native"), side("reference", "interpreter"), holds,
                                              (True, False), {"A": True, "B": True})["status"], "identical")

    def test_interpreter_where_native_is_expected_is_an_error(self):
        verdict = self.verdict(side("subject", "interpreter"), side("reference", "interpreter"))
        self.assertEqual(verdict["status"], "error")
        self.assertIn("slot A holds a module the table runs as native C", verdict["why"])

    def test_native_where_the_table_says_interpreter_is_an_error(self):
        verdict = self.verdict(side("subject", "native", "native"), side("reference", "interpreter"))
        self.assertEqual(verdict["status"], "error")
        self.assertIn("slot B ran native C", verdict["why"])

    def test_a_native_slot_with_no_call_is_an_error(self):
        silent = side("subject", "native")
        silent["paths"] = {"variant 568 in B": ["interpreter"]}
        errors = model_oracle.backend_errors(silent, {"A": True, "B": False}, True)
        self.assertEqual(len(errors), 1)
        self.assertIn("slot A holds a module the table runs as native C and made no call", errors[0])
        # A side that asked for the interpreter is not expected to run native.
        self.assertEqual(model_oracle.backend_errors(silent, {"A": True, "B": False}, False), [])

    def test_a_reference_in_interpreter_mode_never_runs_native(self):
        verdict = self.verdict(side("subject", "native"), side("reference", "native"))
        self.assertEqual(verdict["status"], "error")
        self.assertIn("reference", verdict["why"])

    def test_an_image_of_other_code_is_no_native_row(self):
        class Disc:
            def __init__(self, image):
                self.image = image

            def variant(self, record, alternative, slot):
                return self.image

        row = model_gate.native_rows()[0]
        image = bytes(range(256)) * 80
        # Whatever the code is, an image of other bytes is no native row's.
        self.assertIsNone(model_oracle.native_row(Disc(image), 0, 0, "A"))
        self.assertEqual(row["backend"], "native")   # and the one table row there is has no image here

    # --mod: a code mod's rows (mod API 29), the trace's mod:<id>.

    def test_a_mod_s_c_where_its_row_covers_is_a_match(self):
        verdict = self.verdict(side("subject", "mod:pikachu-c"), side("reference", "interpreter"),
                               {"A": "mod:pikachu-c", "B": False})
        self.assertEqual(verdict["status"], "identical")

    def test_native_or_interpreter_where_a_mod_row_covers_is_an_error(self):
        for ways in ("native", "interpreter"):
            verdict = self.verdict(side("subject", ways), side("reference", "interpreter"),
                                   {"A": "mod:pikachu-c", "B": False})
            self.assertEqual(verdict["status"], "error")
            self.assertIn("pikachu-c's row runs as its C", verdict["why"])

    def test_another_mod_s_c_is_an_error(self):
        verdict = self.verdict(side("subject", "mod:other"), side("reference", "interpreter"),
                               {"A": "mod:pikachu-c", "B": False})
        self.assertEqual(verdict["status"], "error")

    def test_a_mod_s_c_where_no_row_covers_is_an_error(self):
        verdict = self.verdict(side("subject", "native", "mod:pikachu-c"), side("reference", "interpreter"))
        self.assertEqual(verdict["status"], "error")
        self.assertIn("slot B ran a mod's C (mod:pikachu-c) where no mod row covers its module", verdict["why"])

    def test_a_reference_in_interpreter_mode_never_runs_a_mod_s_c(self):
        verdict = self.verdict(side("subject", "mod:pikachu-c"), side("reference", "mod:pikachu-c"),
                               {"A": "mod:pikachu-c", "B": False})
        self.assertEqual(verdict["status"], "error")
        self.assertIn("reference: slot A ran a mod's C", verdict["why"])

    def test_the_expected_way_follows_the_game_s_order(self):
        image = (0x12345).to_bytes(4, "little") + bytes(range(252)) * 4
        digest = model_oracle.sha(image[:0x40])
        late = ("variant", "A", 0x12345, 0x40, digest, False, "late")
        early = ("variant", "A", 0x12345, 0x40, digest, True, "early")
        way = model_oracle.expected_way
        self.assertIs(way(image, "A", "variant", False, []), False)
        self.assertEqual(way(image, "A", "variant", False, [late]), "mod:late")
        # The disc's row before a row without OVER_DISC; one with it before the disc's.
        self.assertIs(way(image, "A", "variant", True, [late]), True)
        self.assertEqual(way(image, "A", "variant", True, [late, early]), "mod:early")
        # Not for another slot, kind, identifier or bytes; of two, the later registered.
        self.assertIs(way(image, "B", "variant", False, [late]), False)
        self.assertIs(way(image, "A", "primary", False, [late]), False)
        self.assertIs(way(b"\0" + image[1:], "A", "variant", False, [late]), False)
        self.assertEqual(way(image, "A", "variant", False, [late, late[:6] + ("later",)]), "mod:later")

    def test_a_return_two_primary_is_answered_before_a_late_row(self):
        image = (0x12345).to_bytes(4, "little") + model_oracle.RETURN_TWO + bytes(0x1000 - 12)
        digest = model_oracle.sha(image[:0x40])
        late = ("primary", "A", 0x12345, 0x40, digest, False, "late")
        early = ("primary", "A", 0x12345, 0x40, digest, True, "early")
        way = model_oracle.expected_way
        self.assertIs(way(image, "A", "primary", False, [late]), True)
        self.assertEqual(way(image, "A", "primary", False, [late, early]), "mod:early")
        # Not a variant: a variant has no `return 2` of the game's own.
        self.assertEqual(way(image, "A", "variant", False, [late[:0] + ("variant",) + late[1:]]), "mod:late")

    def test_a_mod_s_patches_are_what_the_disc_reads(self):
        class Disc(model_oracle.Disc):
            def __init__(self):
                self.patches, self.lock, self.lba = {}, __import__("threading").Lock(), 0

            def _sector(self, lba):
                return bytes(model_oracle.SECTOR)

        disc = Disc()
        disc.patch({"data": [{"file": "\\DATA\\MODEL.MRG;1", "patch": [{"at": "0x800", "bytes": "AB01"}]}]}, "m")
        self.assertEqual(disc.read(0, 1, 1)[:3], b"\xab\x01\x00")
        self.assertEqual(disc.read(0, 0, 1), bytes(model_oracle.SECTOR))
        with self.assertRaises(SystemExit):
            disc.patch({"data": [{"file": "\\DATA\\MODEL.MRG;1", "replace": "x.bin"}]}, "m")
        with self.assertRaises(SystemExit):
            disc.patch({"data": [{"file": "\\DATA\\WA_MRG.MRG;1", "patch": []}]}, "m")
        self.assertEqual(model_oracle.mod_variable("3d-monsters"), "MEMORIES_MOD_3D_MONSTERS")


if __name__ == "__main__":
    unittest.main()
