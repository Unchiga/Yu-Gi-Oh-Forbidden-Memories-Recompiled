#!/usr/bin/env python3
"""model_gate.py mod-migrate (tools/pc/model_migrate.py) end to end on the
disc (MEMORIES_DISC or --disc; skipped without one, exit 77 with
--classifier). Every mod is made here from the disc at run time: no game
data is kept.

  class    model_migrate.classify() against the game's classifier
           (memories_model_module_class_test --classify, --classifier):
           every disc shape's module in each slot as the disc has it, with
           a byte of its tables, of its code, its first word or its entry
           word changed;
  tables   a MODEL.MRG patch of shape 54's command table (as
           test_model_mod_c.py's): the tool makes the shape's C, and the
           oracle runs it against the interpreter on the patched record;
  replace  the same bytes as a raw replacement of the record's module
           sectors, in a mod with its own MemoriesModInit: the same C, rows
           registered through MemoriesModelRows_Register, and no oracle
           command (the oracle reads MODEL.MRG patches only); a build;
  const   a MODEL.MRG patch of a const the port's C defines (a scale
           vector, picked at run time so that the interpreter draws other
           frames with it): the generated C has the mod's bytes and draws
           what the interpreter draws; given the disc's bytes instead (the
           control) it draws something else;
  code     a patch of shape 54's code: the tool says to port it by hand,
           names the function, and writes no C;
  models   a "models" file (record 83 with the tables patch): the tool's
           check command battles it (model_oracle.py --models).

The commands each case runs are the ones notes/modding.md gives ("Moving a
mod's attacks from MIPS to C"), which the test checks word for word.

    python tools/pc/test_model_migrate.py --out tmp/pc/model-migrate --jobs 6
    python tools/pc/test_model_migrate.py --only class --classifier tmp/pc/tests/memories_model_module_class_test.exe
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import model_gate  # noqa: E402
import model_migrate  # noqa: E402
import model_oracle  # noqa: E402
import test_model_mod_c  # noqa: E402

NOTES = ROOT / "notes/modding.md"
# The guide's commands, with the guide's own mod, disc, record and model.
GUIDE = {"mod": "mods/my-mod", "disc": "game/rpg-yfm.bin", "records": "83", "models": "pikachu"}
MIGRATE = "python tools/pc/model_gate.py mod-migrate {mod} --disc {disc}"
BUILD = "python tools/pc/build_mod.py {mod} --target i386"
ORACLE_RECORDS = "python tools/pc/model_oracle.py --disc {disc} --mod {mod} --records {records}"
ORACLE_MODELS = "python tools/pc/model_oracle.py --disc {disc} --mod {mod} --models {models}"
PLAIN = "python tools/pc/model_oracle.py --disc {disc} --records {records}"
CASES = ("class", "tables", "replace", "const", "code", "models")


class Test:
    def __init__(self, arguments):
        self.arguments = arguments
        self.failures: list[str] = []

    def expect(self, condition: bool, text: str) -> None:
        print(f"  {'ok' if condition else 'FAILED'}: {text}", flush=True)
        if not condition:
            self.failures.append(text)

    def run(self, command: str, extra: list[str] = (), name: str = "") -> subprocess.CompletedProcess:
        """A guide command (words split on spaces: no path here has one), from
        the repository's root."""
        words = command.split() + list(extra)
        words[0] = sys.executable
        print(f"test_model_migrate: {name}: {command}{' ' + ' '.join(extra) if extra else ''}", flush=True)
        result = subprocess.run(words, cwd=ROOT, capture_output=True, text=True, errors="replace")
        if name:
            (self.arguments.out / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        return result

    def oracle(self, command: str, name: str, extra: list[str] = ()) -> dict:
        out = self.arguments.out / name
        shutil.rmtree(out, ignore_errors=True)
        flags = ["--out", str(out), "--jobs", str(self.arguments.jobs), *extra]
        if self.arguments.executable:
            flags += ["--executable", str(self.arguments.executable)]
        result = self.run(command, flags, name)
        try:
            report = json.loads((out / "report.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            report = {}
        report["exit"] = result.returncode
        summary = [line for line in result.stdout.splitlines() if line.startswith(("model oracle:", "  "))]
        print("\n".join(summary[-6:]))
        return report

    def migrate(self, mod: Path, name: str) -> tuple[subprocess.CompletedProcess, dict]:
        result = self.run(MIGRATE.format(mod=rel(mod), disc=rel(self.arguments.disc)), name=name)
        print("\n".join(result.stdout.splitlines()[-12:]))
        try:
            report = json.loads((mod / model_migrate.COPIES / "migrate.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            report = {"modules": []}
        return result, report

    def build(self, mod: Path, name: str) -> bool:
        ok = self.run(BUILD.format(mod=rel(mod)), name=name).returncode == 0
        for target in self.arguments.target or []:
            ok &= subprocess.run([sys.executable, str(ROOT / "tools/pc/build_mod.py"), str(mod), "--target", target],
                                 cwd=ROOT).returncode == 0
        return ok


def rel(path: Path) -> str:
    """A path as the commands take it: from the root when inside the repository."""
    path = Path(path).resolve()
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def guide(test: Test) -> None:
    """The guide gives the commands the cases run, word for word."""
    text = NOTES.read_text(encoding="utf-8")
    for command in (MIGRATE, BUILD, ORACLE_RECORDS, ORACLE_MODELS):
        line = command.format(**GUIDE)
        test.expect(line in text, f"notes/modding.md gives `{line}`")


def mod_folder(folder: Path, identifier: str, manifest: dict) -> Path:
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    (folder / "mod.json").write_text(json.dumps({"id": identifier, "name": identifier, "version": "1.0.0",
                                                 "enabled": False, **manifest}, indent=1) + "\n", encoding="utf-8")
    return folder


def ways(report: dict) -> set[str]:
    return test_model_mod_c.subject_ways(report)


def digests(report: dict) -> list[str]:
    return test_model_mod_c.digests(report)


def modules(report: dict, action: str | None = None) -> list[dict]:
    return [m for m in report["modules"] if action is None or m["action"] == action]


# --- class -----------------------------------------------------------------------------

def case_class(test: Test, disc: model_oracle.Disc) -> None:
    classifier = test.arguments.classifier
    if not classifier or not Path(classifier).is_file():
        test.expect(False, f"the game's classifier is there ({classifier}; --classifier)")
        return
    tables = model_migrate.Tables()
    mdisc = model_migrate.Disc(test.arguments.disc)
    images: list[tuple[bool, str, bytes]] = []
    for primary in (False, True):
        for row in tables.native[primary]:
            for slot in "AB":
                place = mdisc.first(primary, slot, row.identifier(slot))
                image = mdisc.module(*place)
                images.append((primary, slot, image))
                for at in (row.data_start or 8, row.span - 1, 8, 0, 4):
                    changed = bytearray(image)
                    if at == 4:
                        changed[4:8] = bytes(4)
                    else:
                        changed[at] ^= 0x5A
                    images.append((primary, slot, bytes(changed)))
    # Every record's primaries (`return 2` among them) as the disc has them.
    for record in range(0, mdisc.records, 7):
        for stage in (4, 5):
            images.append((True, "AB"[stage - 4], mdisc.module(record, stage)))
    path = test.arguments.out / "classify.bin"
    with path.open("wb") as handle:
        for primary, slot, image in images:
            handle.write(bytes([int(primary), "AB".index(slot)]) + image)
    result = subprocess.run([str(classifier), "--classify", str(path)], capture_output=True, text=True)
    lines = result.stdout.splitlines()
    test.expect(result.returncode == 0 and len(lines) == len(images),
                f"the game classified {len(lines)} of the {len(images)} modules")
    differ = []
    seen = set()
    for (primary, slot, image), line in zip(images, lines):
        kind, identifier, shape, code_size, span = line.split()
        info = model_migrate.classify(image, slot, primary, tables)
        seen.add(info["kind"])
        mine = (model_migrate.CLASSES.index(info["kind"]), info["identifier"], info["shape"], info["code_size"],
                info["span"])
        if mine != (int(kind), int(identifier), int(shape), int(code_size), int(span)):
            differ.append(f"{'primary' if primary else 'variant'} slot {slot} first word {info['identifier']}: "
                          f"game {line}, model_migrate {mine}")
    test.expect(not differ, f"model_migrate.classify() says what the game says of all {len(images)} modules "
                            f"({'; '.join(differ[:4])})")
    test.expect(seen >= {"NATIVE", "TABLES", "CODE", "NEW", "EMPTY"},
                f"the modules cover NATIVE, TABLES, CODE, NEW and EMPTY ({sorted(seen)})")


# --- tables, models ------------------------------------------------------------------------

def shape54(test: Test, disc: model_oracle.Disc):
    """Record 83's shape-54 table patch (test_model_mod_c.py's) and its unpatched oracle run."""
    if getattr(test, "_shape54", None):
        return test._shape54
    row = next(r for r in model_gate.native_rows() if r.shape == test_model_mod_c.SHAPE)
    record, alternatives = test_model_mod_c.find_record(disc, row)
    plain = test.oracle(PLAIN.format(disc=rel(test.arguments.disc), records=record), "unpatched",
                        ["--alternatives", ",".join(map(str, alternatives))])
    test.expect(plain["exit"] == 0, "the unpatched record: identical (oracle exit 0)")
    patches, rows = test_model_mod_c.patched(disc, row, record, alternatives, test_model_mod_c.setup_commands(plain))
    test._shape54 = (row, record, alternatives, plain, patches)
    return test._shape54


def case_tables(test: Test, disc: model_oracle.Disc) -> None:
    row, record, alternatives, plain, patches = shape54(test, disc)
    mod = mod_folder(test.arguments.out / "mods" / "migrate-tables", "migrate-tables",
                     {"data": [{"file": "\\DATA\\MODEL.MRG;1", "patch": patches}]})
    result, report = test.migrate(mod, "tables-migrate")
    made = modules(report, "generate")
    test.expect(result.returncode == 0 and made and all(m["class"] == "TABLES" and m["shape"] == 54 for m in made),
                f"tables: the patched modules are TABLES of shape 54 and get its C "
                f"({[(m['where'], m['class'], m['action']) for m in report['modules']]})")
    test.expect(all(m["code_bytes"] == 0 and not m["consts"] and m["data_bytes"] for m in made),
                "tables: what differs is data the C reads from the record")
    want = ORACLE_RECORDS.format(disc=rel(test.arguments.disc), mod=rel(mod), records=record)
    test.expect(report.get("verify") == [BUILD.format(mod=rel(mod)), want],
                f"tables: the tool's check commands are the guide's ({report.get('verify')})")
    test.expect(test.build(mod, "tables-build"), "tables: the mod builds")
    found = test.oracle(want, "tables-oracle", ["--alternatives", ",".join(map(str, alternatives))])
    tag = "mod:migrate-tables"
    test.expect(found["exit"] == 0, "tables: identical to the interpreter (oracle exit 0)")
    test.expect(ways(found) == {tag}, f"tables: the subject ran {tag} and nothing else ({sorted(ways(found))})")
    test.expect(bool(digests(found)) and not set(digests(found)) & set(digests(plain)),
                f"tables: the patch changes the frames ({digests(plain)} unpatched, {digests(found)} patched)")


def case_models(test: Test, disc: model_oracle.Disc) -> None:
    row, record, alternatives, plain, patches = shape54(test, disc)
    data = bytearray(disc.read(record, 0, 0x114))   # the whole record, as the disc has it
    first = record * 0x114 * model_oracle.SECTOR
    for run in patches:
        at = int(run["at"]) - first if isinstance(run["at"], int) else int(str(run["at"]), 0) - first
        piece = bytes.fromhex(run["bytes"])
        data[at:at + len(piece)] = piece
    mod = mod_folder(test.arguments.out / "mods" / "migrate-models", "migrate-models",
                     {"min_api": 21, "models": [{"name": GUIDE["models"], "file": "models/pikachu.mrg"}]})
    (mod / "models").mkdir()
    (mod / "models" / "pikachu.mrg").write_bytes(bytes(data))
    result, report = test.migrate(mod, "models-migrate")
    made = modules(report, "generate")
    test.expect(result.returncode == 0 and made and all(m["model"] == GUIDE["models"] and m["class"] == "TABLES"
                                                        for m in made),
                f"models: the model's patched modules are TABLES and get the C "
                f"({[(m['where'], m['class'], m['action']) for m in report['modules']]})")
    want = ORACLE_MODELS.format(disc=rel(test.arguments.disc), mod=rel(mod), models=GUIDE["models"])
    test.expect(report.get("verify") == [BUILD.format(mod=rel(mod)), want],
                f"models: the tool's check commands are the guide's ({report.get('verify')})")
    test.expect(test.build(mod, "models-build"), "models: the mod builds")
    found = test.oracle(want, "models-oracle", ["--alternatives", ",".join(map(str, alternatives))])
    tag = "mod:migrate-models"
    test.expect(found["exit"] == 0, "models: identical to the interpreter (oracle exit 0)")
    test.expect(ways(found) == {tag} and all(v["battle"].startswith("model:") for v in found.get("verdicts", ())),
                f"models: the model fought, and the subject ran {tag} and nothing else ({sorted(ways(found))})")


OWN_INIT = """/* The mod's own entry, which registers mod-migrate's rows itself. */
#include "pc/mods/modapi.h"
#include "model_rows.h"

int MemoriesModInit(const MemoriesModHost *host, MemoriesMod *mod)
{
    if (host->api < 29) return 0;
    mod->api = 29;
    return MemoriesModelRows_Register(host);
}
"""


def case_replace(test: Test, disc: model_oracle.Disc) -> None:
    """The tables patch as a raw replacement of the record's 44 module
    sectors (an "lba" entry with "sectors"), in a mod with a MemoriesModInit
    of its own: the same modules and C as the patch, the rows registered by
    MemoriesModelRows_Register, and no oracle command (the oracle reads
    MODEL.MRG patches only)."""
    row, record, alternatives, plain, patches = shape54(test, disc)
    stages = bytearray(disc.read(record, model_migrate.MODULES_SECTOR, model_migrate.MODULES_SECTORS))
    first = (record * 0x114 + model_migrate.MODULES_SECTOR) * model_oracle.SECTOR
    for run in patches:
        piece = bytes.fromhex(run["bytes"])
        stages[run["at"] - first:run["at"] - first + len(piece)] = piece
    lba = model_migrate.Disc(test.arguments.disc).lba + record * 0x114 + model_migrate.MODULES_SECTOR
    mod = mod_folder(test.arguments.out / "mods" / "migrate-replace", "migrate-replace",
                     {"library": "migrate-replace", "min_api": 29,
                      "data": [{"lba": lba, "sectors": model_migrate.MODULES_SECTORS, "replace": "stages.bin"}]})
    (mod / "stages.bin").write_bytes(bytes(stages))
    (mod / "main.c").write_text(OWN_INIT, encoding="utf-8")
    result, report = test.migrate(mod, "replace-migrate")
    made = modules(report, "generate")
    test.expect(result.returncode == 0 and len(made) == 2 * len(alternatives) and
                all(m["class"] == "TABLES" and m["shape"] == 54 for m in made),
                f"replace: the replaced sectors' modules are the patch's, TABLES of shape 54 "
                f"({[(m['where'], m['class'], m['action']) for m in report['modules']]})")
    test.expect(report.get("init") == "MemoriesModelRows_Register" and (mod / "model_rows.h").is_file() and
                "int MemoriesModInit" not in (mod / "model_rows.c").read_text(),
                "replace: with the mod's own MemoriesModInit, model_rows.c gives MemoriesModelRows_Register "
                "(model_rows.h) and no MemoriesModInit")
    test.expect(report.get("verify") == [BUILD.format(mod=rel(mod))] and "cannot check" in report.get("oracle", ""),
                f"replace: no oracle command, and the report says why ({report.get('verify')}, {report.get('oracle')!r})")
    test.expect(test.build(mod, "replace-build"), "replace: the mod (its main.c calling the rows) builds")


# --- const ------------------------------------------------------------------------------

def const_candidates(disc: model_oracle.Disc):
    """(row, record, alternative, offset, size) of each const a native
    shape's C defines, of 16 bytes (a VECTOR) whose words are not all zero,
    on a record whose image has that shape in both slots."""
    work = Path(os.environ.get("TEMP", "/tmp")) / "test-model-migrate-consts"
    work.mkdir(parents=True, exist_ok=True)
    found = []
    for row in model_gate.native_rows():
        if not re.search(r"\bconst\s+VECTOR\s+D_8013", "".join(
                path.read_text(errors="replace") for pattern in model_gate.source_patterns(row, "A")
                for path in ROOT.glob(pattern))):
            continue
        source = model_migrate.Source(row, "A")
        source.analyse(work)
        for offset, symbol in sorted(source.consts.items()):
            if symbol.size == 16 and any(struct.unpack("<4i", symbol.data)[:3]):
                found.append((row, offset, symbol))
    return found


def case_const(test: Test, disc: model_oracle.Disc) -> None:
    for row, offset, symbol in const_candidates(disc):
        try:
            record, alternatives = test_model_mod_c.find_record(disc, row)
        except SystemExit:
            continue
        alternative = alternatives[0]
        # Each word of the vector doubled: a scale of 1.0 (4096) becomes 2.0.
        values = struct.unpack("<4i", symbol.data)
        changed = struct.pack("<4i", *(value * 2 for value in values))
        patches = [{"at": test_model_mod_c.stage_at(record, alternative, slot) + offset, "bytes": changed.hex().upper()}
                   for slot in "AB"]
        data = mod_folder(test.arguments.out / "mods" / "migrate-const-data", "migrate-const-data",
                          {"data": [{"file": "\\DATA\\MODEL.MRG;1", "patch": patches}]})
        plain = test.oracle(PLAIN.format(disc=rel(test.arguments.disc), records=record), "const-unpatched",
                            ["--alternatives", str(alternative)])
        patched = test.oracle(ORACLE_RECORDS.format(disc=rel(test.arguments.disc), mod=rel(data), records=record),
                              "const-data-only", ["--alternatives", str(alternative)])
        if not (plain["exit"] == 0 and patched["exit"] == 0 and digests(plain) and digests(patched)
                and set(digests(plain)).isdisjoint(digests(patched))):
            print(f"test_model_migrate: const: shape {row.shape} {symbol.name}: the interpreter draws the same "
                  f"frames with it doubled; the next const")
            continue
        print(f"test_model_migrate: const: shape {row.shape} {symbol.name} (0x{offset:X}) on record {record} "
              f"alternative {alternative}")
        mod = mod_folder(test.arguments.out / "mods" / "migrate-const", "migrate-const",
                         {"data": [{"file": "\\DATA\\MODEL.MRG;1", "patch": patches}]})
        result, report = test.migrate(mod, "const-migrate")
        made = modules(report, "generate")
        name = f"D_{0x8013B000 + offset:08X}"
        test.expect(result.returncode == 0 and made and all(m["class"] == "CODE" and m["consts"] == [name] and
                                                            m["code_bytes"] == 0 for m in made),
                    f"const: the modules are CODE, and the only difference is {name}, which the C gets "
                    f"({[(m['where'], m['class'], m['action'], m.get('consts')) for m in report['modules']]})")
        units = list(mod.glob("mm_*_data.c"))
        test.expect(len(units) == 2 and all(", ".join(f"0x{b:02X}" for b in changed) in unit.read_text()
                                            for unit in units),
                    f"const: each slot's data unit defines the mod's bytes ({[u.name for u in units]})")
        test.expect(test.build(mod, "const-build"), "const: the mod builds")
        want = ORACLE_RECORDS.format(disc=rel(test.arguments.disc), mod=rel(mod), records=record)
        found = test.oracle(want, "const-oracle", ["--alternatives", str(alternative)])
        tag = "mod:migrate-const"
        test.expect(found["exit"] == 0, "const: identical to the interpreter (oracle exit 0)")
        test.expect(ways(found) == {tag}, f"const: the subject ran {tag} and nothing else ({sorted(ways(found))})")
        test.expect(digests(found) == digests(patched),
                    f"const: the C draws what the MIPS draws with the mod's bytes ({digests(found)} with C, "
                    f"{digests(patched)} interpreted)")
        # The control: the same C with the disc's bytes in the definition.
        for unit in units:
            unit.write_text(unit.read_text().replace(", ".join(f"0x{b:02X}" for b in changed),
                                                     ", ".join(f"0x{b:02X}" for b in symbol.data)))
        test.expect(test.build(mod, "const-control-build"), "const control: the mod builds with the disc's bytes")
        found = test.oracle(want, "const-control", ["--alternatives", str(alternative)])
        test.expect(found["exit"] != 0 and any(v["status"] == "differs" for v in found.get("verdicts", ())),
                    "const control: with the disc's bytes in its definition, the C differs from the interpreter")
        return
    test.expect(False, "const: a const of the port's C whose doubling the interpreter draws")


# --- code -------------------------------------------------------------------------------

def case_code(test: Test, disc: model_oracle.Disc) -> None:
    row = next(r for r in model_gate.native_rows() if r.shape == test_model_mod_c.SHAPE)
    record, alternatives = test_model_mod_c.find_record(disc, row)
    at = test_model_mod_c.stage_at(record, alternatives[0], "A") + 0x100
    image = disc.variant(record, alternatives[0], "A")
    word = struct.unpack_from("<I", image, 0x100)[0] ^ 0x00000010
    mod = mod_folder(test.arguments.out / "mods" / "migrate-code", "migrate-code",
                     {"data": [{"file": "\\DATA\\MODEL.MRG;1", "patch": [{"at": at,
                                                                         "bytes": struct.pack("<I", word).hex()}]}]})
    result, report = test.migrate(mod, "code-migrate")
    hand = modules(report, "hand")
    test.expect(result.returncode == 1 and len(hand) == 1 and hand[0]["class"] == "CODE" and
                re.search(r"func_8013B[0-9A-F]{3} \(1 byte at 0x100\)", hand[0]["why"]),
                f"code: the module is CODE and to be ported by hand, in the function with the byte "
                f"({[(m['where'], m['class'], m['action'], m['why']) for m in hand]})")
    test.expect(not modules(report, "generate") and not list(mod.glob("*.c")) and not (mod / "model_rows.c").exists(),
                "code: no C is written for it")
    test.expect("port by hand" in result.stdout, "code: the report says to port it by hand")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--disc", type=Path, default=os.environ.get("MEMORIES_DISC"))
    parser.add_argument("--executable", type=Path, help="the game (default model_oracle's)")
    parser.add_argument("--out", type=Path, default=ROOT / "tmp/pc/model-migrate")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--only", action="append", choices=CASES, help="the cases to run (default all)")
    parser.add_argument("--classifier", type=Path, help="memories_model_module_class_test (the class case)")
    parser.add_argument("--target", action="append", help="build the mods for these targets too (a build check)")
    arguments = parser.parse_args()
    if not arguments.disc or not Path(arguments.disc).is_file():
        print("test_model_migrate: no disc (MEMORIES_DISC or --disc), skipped")
        return 77 if arguments.classifier else 0
    arguments.disc = Path(arguments.disc).resolve()
    arguments.out = arguments.out.resolve()
    arguments.out.mkdir(parents=True, exist_ok=True)
    disc = model_oracle.Disc(arguments.disc)
    test = Test(arguments)
    cases = arguments.only or list(CASES)
    if cases != ["class"]:
        guide(test)
    for case in cases:
        print(f"test_model_migrate: --- {case}", flush=True)
        globals()[f"case_{case}"](test, disc)
    print(f"test_model_migrate: {'FAILED: ' + '; '.join(test.failures) if test.failures else 'all passed'}")
    return 1 if test.failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
