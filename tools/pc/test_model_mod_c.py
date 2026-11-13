#!/usr/bin/env python3
"""A code mod's own C for a battle model's control modules (mod API 29), end
to end on the disc (MEMORIES_DISC or --disc; skipped without one).

The mod: a "data" patch of one MODEL.MRG record whose variant modules are
shape 54's, swapping two entries of the command table the shape's C reads
(inside the span config/pc/model_gate.csv hashes), so the disc's rows no
longer cover those modules and the game would interpret them; and code that
registers rows for the patched modules, whose C is the port's own for shape
54 (tests/pc/mod_fixtures/model_c54, which #includes src/native_overlays
under a prefix header). The patched bytes and the rows' hashes are made
from the disc here, at run time: no game data is kept.

Three oracle runs (tools/pc/model_oracle.py --mod), each the subject in the
default mode against the interpreter:
  1. the mod: identical frames, and the subject ran "mod:model-c54" in the
     covered slots (the oracle checks that from the rows the mod logged);
  2. the same data without the code: the record is interpreted, and the
     mod is warned that its modules are MIPS;
  3. the mod with MEMORIES_MODEL_MOD_C=0 on the subject: the interpreter.

    python tools/pc/test_model_mod_c.py --out tmp/pc/model-mod-c --jobs 6
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import model_gate  # noqa: E402
import model_oracle  # noqa: E402

FIXTURE = ROOT / "tests/pc/mod_fixtures/model_c54"
MOD_ID = "model-c54"
SHAPE = 54


def find_record(disc: model_oracle.Disc, row: model_gate.Row) -> tuple[int, list[int]]:
    """The first record with shape 54's modules, and its alternatives that have them."""
    for record in range(model_oracle.RECORDS):
        alternatives = [alternative for alternative in (0, 1)
                        if all((found := model_oracle.native_row(disc, record, alternative, slot)) is not None
                               and found.shape == row.shape for slot in "AB")]
        if alternatives:
            return record, alternatives
    sys.exit(f"test_model_mod_c: no record of the disc has shape {SHAPE}'s modules")


def patched(disc: model_oracle.Disc, row: model_gate.Row, record: int, alternatives: list[int]):
    """The data patches (two table entries swapped in each of the record's
    shape-54 images) and the rows for the patched images."""
    table, stride, count, _, _ = row.command_table
    patches, rows = [], []
    for alternative in alternatives:
        for slot in "AB":
            image = bytearray(disc.variant(record, alternative, slot))
            entries = [bytes(image[table + n * stride:table + (n + 1) * stride]) for n in range(count)]
            other = next((n for n in range(1, count) if entries[n] != entries[0]), None)
            if other is None:
                sys.exit(f"test_model_mod_c: record {record}'s table entries are all the same")
            entries[0], entries[other] = entries[other], entries[0]
            image[table:table + count * stride] = b"".join(entries)
            stage = 2 * alternative + (slot == "B")
            at = (record * 0x114 + model_oracle.VARIANT_SECTOR + model_oracle.VARIANT_SECTORS * stage) * \
                model_oracle.SECTOR + table
            patches.append({"at": at, "bytes": bytes(image[table:table + count * stride]).hex().upper()})
            digest = hashlib.sha256(bytes(image[:row.span])).hexdigest()
            if digest == row.sha256(slot):
                sys.exit("test_model_mod_c: the patch left the disc's hash")
            rows.append((slot, int.from_bytes(image[:4], "little"), row.span, digest))
    return patches, sorted(set(rows))


def write_mod(folder: Path, patches: list[dict], rows: list[tuple], code: bool) -> None:
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    manifest = {"id": MOD_ID if code else MOD_ID + "-data", "name": "Shape 54's C for a patched record",
                "version": "1.0.0", "min_api": 29 if code else 1, "enabled": False,
                "data": [{"file": "\\DATA\\MODEL.MRG;1", "patch": patches}]}
    if code:
        manifest["library"] = MOD_ID
        for name in ("prefix.h", "slot_a.c", "slot_b.c", "rows.c"):
            shutil.copy(FIXTURE / name, folder / name)
        with (folder / "rows.inc").open("w", encoding="utf-8", newline="\n") as handle:
            handle.write("/* Written by tools/pc/test_model_mod_c.py from the patched record. */\n")
            for slot, identifier, span, digest in rows:
                handle.write(f"MODEL_C54_ROW(MEMORIES_MODEL_SLOT_{slot}, {identifier}u, 0x{span:X}u, \"{digest}\")\n")
    (folder / "mod.json").write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")


def oracle(arguments, name: str, record: int, alternatives: list[int], mod: Path, extra: list[str]) -> dict:
    out = arguments.out / name
    shutil.rmtree(out, ignore_errors=True)
    command = [sys.executable, "-u", str(ROOT / "tools/pc/model_oracle.py"), "--disc", str(arguments.disc),
               "--records", str(record), "--alternatives", ",".join(map(str, alternatives)),
               "--jobs", str(arguments.jobs), "--mod", str(mod), "--out", str(out), *extra]
    if arguments.executable:
        command += ["--executable", str(arguments.executable)]
    print(f"test_model_mod_c: {name}: {' '.join(command[2:])}", flush=True)
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, errors="replace")
    (arguments.out / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    try:
        report = json.loads((out / "report.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        report = {}
    report["exit"] = result.returncode
    report["summary"] = [line for line in result.stdout.splitlines() if line.startswith(("model oracle:", "  "))]
    return report


def subject_ways(report: dict) -> set[str]:
    return {way for verdict in report.get("verdicts", ()) for ways in verdict.get("backends", {}).get("subject", {}).values()
            for way in ways}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--disc", type=Path, default=os.environ.get("MEMORIES_DISC"))
    parser.add_argument("--executable", type=Path, help="the game (default model_oracle's)")
    parser.add_argument("--out", type=Path, default=ROOT / "tmp/pc/model-mod-c")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--target", action="append", help="build_mod.py targets to build the mod for "
                                                            "(default i386; the oracle runs the i386 game)")
    arguments = parser.parse_args()
    if not arguments.disc or not Path(arguments.disc).is_file():
        print("test_model_mod_c: no disc (MEMORIES_DISC or --disc), skipped")
        return 0
    arguments.disc = Path(arguments.disc).resolve()
    arguments.out = arguments.out.resolve()
    arguments.out.mkdir(parents=True, exist_ok=True)
    disc = model_oracle.Disc(arguments.disc)
    row = next(r for r in model_gate.native_rows() if r.shape == SHAPE)
    record, alternatives = find_record(disc, row)
    patches, rows = patched(disc, row, record, alternatives)
    print(f"test_model_mod_c: record {record}, alternatives {alternatives}, {len(rows)} rows", flush=True)
    code, data = arguments.out / "mods" / MOD_ID, arguments.out / "mods" / (MOD_ID + "-data")
    write_mod(code, patches, rows, True)
    write_mod(data, patches, rows, False)
    for target in arguments.target or ["i386"]:
        subprocess.run([sys.executable, str(ROOT / "tools/pc/build_mod.py"), str(code), "--target", target],
                       cwd=ROOT, check=True)
    failures = []

    def expect(condition: bool, text: str) -> None:
        print(f"  {'ok' if condition else 'FAILED'}: {text}", flush=True)
        if not condition:
            failures.append(text)

    tag = "mod:" + MOD_ID
    report = oracle(arguments, "with-c", record, alternatives, code, [])
    print("\n".join(report["summary"]))
    expect(report["exit"] == 0, "with the mod's C: every battle identical and covered (oracle exit 0)")
    expect(subject_ways(report) == {tag}, f"with the mod's C: the subject ran {tag} and nothing else "
                                          f"({sorted(subject_ways(report))})")
    expect(len(report.get("claims", ())) == len(rows), f"with the mod's C: {len(rows)} rows registered")
    expect(not any("MIPS code" in note for note in report.get("mod_notes", ())),
           "with the mod's C: no MIPS warning for the mod")

    report = oracle(arguments, "data-only", record, alternatives, data, [])
    print("\n".join(report["summary"]))
    expect(report["exit"] == 0, "without the code: identical (oracle exit 0)")
    expect(subject_ways(report) == {"interpreter"}, f"without the code: the subject interpreted "
                                                    f"({sorted(subject_ways(report))})")
    expect(any("MIPS code" in note for note in report.get("mod_notes", ())),
           "without the code: the mod is warned that its modules are MIPS")

    report = oracle(arguments, "mod-c-off", record, alternatives, code, ["--no-mod-c"])
    print("\n".join(report["summary"]))
    expect(report["exit"] == 0, "MEMORIES_MODEL_MOD_C=0: identical (oracle exit 0)")
    expect(subject_ways(report) == {"interpreter"}, f"MEMORIES_MODEL_MOD_C=0: the subject interpreted "
                                                    f"({sorted(subject_ways(report))})")

    print(f"test_model_mod_c: {'FAILED: ' + '; '.join(failures) if failures else 'all passed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
