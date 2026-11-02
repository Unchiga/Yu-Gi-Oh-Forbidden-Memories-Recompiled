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
import build_config  # noqa: E402
import model_gate  # noqa: E402


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


if __name__ == "__main__":
    unittest.main()
