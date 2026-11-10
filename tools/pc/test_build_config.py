#!/usr/bin/env python3
"""build_config.newest_input: an object is stale when a file its unit
includes is newer (a slot B wrapper includes slot A's C)."""
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_config  # noqa: E402


class NewestInput(unittest.TestCase):
    def setUp(self):
        self.saved = build_config.ROOT, build_config._SRC
        build_config._INCLUDES.clear()
        build_config._MTIMES.clear()

    def tearDown(self):
        build_config.ROOT, build_config._SRC = self.saved
        build_config._INCLUDES.clear()
        build_config._MTIMES.clear()

    def test_an_included_c_file_and_a_header_through_src_count(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build_config.ROOT, build_config._SRC = root, os.path.join(temporary, "src")
            (root / "src/native_overlays/model_variant").mkdir(parents=True)
            (root / "src/native_overlays/model_variant_slot1").mkdir(parents=True)
            (root / "src/game").mkdir(parents=True)
            entry = root / "src/native_overlays/model_variant/variant1_entry.c"
            wrapper = root / "src/native_overlays/model_variant_slot1/variant1_entry_slot1.c"
            header = root / "src/game/thing.h"
            entry.write_text('#include "game/thing.h"\n#include "variant1_entry.c"\nint f(void) { return 1; }\n')
            wrapper.write_text('#define f g\n#include "../model_variant/variant1_entry.c"\n')
            header.write_text("int f(void);\n")
            os.utime(wrapper, (1000, 1000))
            os.utime(entry, (2000, 2000))   # it includes itself: a cycle, followed once
            os.utime(header, (3000, 3000))
            source = "src/native_overlays/model_variant_slot1/variant1_entry_slot1.c"
            self.assertEqual(build_config.newest_input(source), 3000)
            build_config._MTIMES.clear()
            os.utime(header, (500, 500))
            self.assertEqual(build_config.newest_input(source), 2000)

    def test_every_game_unit_resolves(self):
        for units in build_config.game_sources().values():
            for unit in units:
                self.assertGreater(build_config.newest_input(unit), 0, unit)


if __name__ == "__main__":
    unittest.main()
