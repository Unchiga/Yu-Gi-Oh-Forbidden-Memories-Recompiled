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
        self.assertEqual(self.row().command_table, (0x3A68, 0x44, 18, 0x1B74))

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
        self.assertIn("0x12u, 0x3A68u, 0x44u, 0x1B74u)", text)
        self.assertIn("0xFFFFFFFFu, 0u, 0u, 0u)", text)

    def test_the_committed_418_row_bounds_to_the_retail_table(self):
        row = model_gate.native_rows()[0]
        table, stride, count, field = row.command_table
        self.assertEqual((count, stride), (18, 0x44))
        self.assertEqual(table + count * stride, 0x3F30)   # where the retail table ends, inside the span
        self.assertLessEqual(table + count * stride, row.span)

    def test_a_module_may_be_made_of_two_headers(self):
        row = self.row(sources="433;450")
        self.assertEqual(row.sources, [433, 450])
        self.assertEqual(len(model_gate.source_patterns(row, "A")), 2)
        self.assertEqual(self.row().sources, [418])


class OracleBackends(unittest.TestCase):
    """The oracle's check that native C ran exactly where the table says."""

    def verdict(self, subject, reference, holds=None, allowed=(True, False)):
        return model_oracle.compare(subject, reference, holds or {"A": True, "B": False}, allowed)

    def test_native_where_the_table_says_is_a_match(self):
        verdict = self.verdict(side("subject", "native"), side("reference", "interpreter"))
        self.assertEqual(verdict["status"], "identical")

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


if __name__ == "__main__":
    unittest.main()
