#!/usr/bin/env python3
"""The model modules' gate table without a disc: config/pc/model_gate.csv is
well formed, src/pc/overlays/model_gate_table.h is what it gives, the build
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
