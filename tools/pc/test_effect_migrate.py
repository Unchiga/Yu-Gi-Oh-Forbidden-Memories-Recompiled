#!/usr/bin/env python3
"""`model_gate.py effect-migrate` (tools/pc/effect_migrate.py): its tables
are what the decomp makes and their code ranges the ones the game checks; on
the disc (MEMORIES_DISC or --disc; those cases are skipped without one), mods
made here from the disc at run time: one that changes a table of the bank
(data only: the opt-in written, the table, its effect and the commands
named), one that changes an effect's function (code: named, nothing
written, exit 1), and one whose copies differ.

    python tools/pc/test_effect_migrate.py [--disc PATH]
"""
from __future__ import annotations

import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import effect_migrate  # noqa: E402

DISC = os.environ.get("MEMORIES_DISC")
if "--disc" in sys.argv:   # before the classes: their skip reads it
    _at = sys.argv.index("--disc")
    DISC = sys.argv[_at + 1]
    del sys.argv[_at:_at + 2]
SWORD_POSITIONS = 0x14C3C      # D_8015AB68[0][1], the first sword of the opponent's side (effect 21)


def table_row(symbol: str) -> effect_migrate.Row:
    return next(row for row in effect_migrate.read_bank() if row.symbol == symbol)


class TableTests(unittest.TestCase):
    def test_tables_current(self):
        self.assertEqual(effect_migrate.check_tables(), [])

    def test_every_byte_once(self):
        rows = effect_migrate.read_bank()
        at = 0
        for row in rows:
            self.assertEqual(row.start, at, f"{row.symbol or row.what} at 0x{row.start:X}")
            self.assertGreater(row.end, row.start)
            at = row.end
        self.assertEqual(at, effect_migrate.SIZE)

    def test_known_rows(self):
        self.assertEqual(table_row("D_8015AB68").kind, "data")
        self.assertEqual(table_row("D_8015AB68").effects, "21")
        self.assertEqual(table_row("func_80151558").kind, "code")   # effect_16.c, Raigeki
        self.assertEqual(table_row("func_80151558").effects, "16 20")
        self.assertEqual(table_row("gNorthAmerican_D_80146014").kind, "code")   # printf's format
        self.assertEqual(effect_migrate.read_ids()[16]["started_by"], "DuelEffect_ApplyRaigeki")


@unittest.skipUnless(DISC and Path(DISC).is_file(), "no disc (MEMORIES_DISC or --disc)")
class ModTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        disc = effect_migrate.wa_disc(Path(DISC))
        cls.wa = disc.original(0, disc.size // effect_migrate.SECTOR)[:disc.size]
        disc.file.close()
        cls.copies = effect_migrate.bank_copies(cls.wa)
        cls.folder = Path(tempfile.mkdtemp(prefix="effect-migrate-"))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.folder, ignore_errors=True)

    def mod(self, name: str, patches: list[tuple[int, bytes]]) -> Path:
        folder = self.folder / name
        shutil.rmtree(folder, ignore_errors=True)
        folder.mkdir(parents=True)
        manifest = {"id": name, "name": name, "version": "1.0.0", "data": [{"file": "\\DATA\\WA_MRG.MRG;1", "patch": [
            {"at": hex(at), "bytes": data.hex().upper()} for at, data in patches]}]}
        (folder / "mod.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        return folder

    def flipped(self, offset: int, copies=None) -> list[tuple[int, bytes]]:
        return [(at + offset, bytes([self.wa[at + offset] ^ 0x10])) for at in (copies or self.copies)]

    def run_tool(self, folder: Path, *more: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            status = effect_migrate.main(["effect-migrate", str(folder), "--disc", DISC, *more])
        print(out.getvalue())
        return status, out.getvalue()

    def test_seven_copies(self):
        self.assertEqual(len(self.copies), 7)

    def test_data_only(self):
        folder = self.mod("synthetic-data", self.flipped(SWORD_POSITIONS))
        status, text = self.run_tool(folder)
        self.assertEqual(status, 0)
        self.assertIn("D_8015AB68", text)
        self.assertIn("effect 21 (0x15: DuelEffect_ApplySwords", text)
        self.assertIn("mod.json: added \"duel_effects\": \"own data\"", text)
        self.assertIn("test_duel_effects.py --mod", text)
        self.assertIn(" swords", text)
        manifest = json.loads((folder / "mod.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["duel_effects"], "own data")
        status, text = self.run_tool(folder)   # again: already there
        self.assertIn("has \"duel_effects\": \"own data\" already", text)

    def test_report_only(self):
        folder = self.mod("synthetic-report", self.flipped(SWORD_POSITIONS))
        status, text = self.run_tool(folder, "--report-only")
        self.assertEqual(status, 0)
        self.assertNotIn("duel_effects", json.loads((folder / "mod.json").read_text(encoding="utf-8")))

    def test_code(self):
        raigeki = table_row("func_80151558")
        folder = self.mod("synthetic-code", self.flipped(raigeki.start + 0x40))
        before = (folder / "mod.json").read_text(encoding="utf-8")
        status, text = self.run_tool(folder)
        self.assertEqual(status, 1)
        self.assertIn("func_80151558+0x40", text)
        self.assertIn("effects 16 (0x10: DuelEffect_ApplyRaigeki)", text)
        self.assertIn("no mod API for native C yet", text)
        self.assertEqual((folder / "mod.json").read_text(encoding="utf-8"), before)

    def test_copies_differ(self):
        folder = self.mod("synthetic-differ", self.flipped(SWORD_POSITIONS, self.copies[:1]))
        status, text = self.run_tool(folder, "--report-only")
        self.assertIn("the copies differ", text)

    def test_disc(self):
        folder = self.mod("synthetic-none", [])
        status, text = self.run_tool(folder)
        self.assertEqual(status, 0)
        self.assertIn("nothing to do", text)


if __name__ == "__main__":
    unittest.main()
