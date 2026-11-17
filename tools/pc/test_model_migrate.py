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
  data     the command table entry the attack picks zeroed (divisors among
           its words): to port by hand, naming the words, no C, mod-row
           prints no row; --accept-data makes the C and warns;
  accept   a texture rectangle's header (shape 105's GsIMAGE: inside the
           hashed span, outside the command table, labelled `texture` in
           config/pc/model_span_words.csv) moved: to port by hand without
           --accept-data (a label is not a proof), the report naming the word
           and its label; with it the C, and the oracle sees the C draw what
           the interpreter draws, other frames than the unpatched ones;
  unread   the word after shape 54's command table (labelled `unread`: not
           read by the module's own C) changed: to port by hand without
           --accept-data, the report naming it and its label; with a copy of
           the table whose digest of shape 54's C, or of the tool, is not the
           tree's (MEMORIES_SPAN_WORDS), the report says the labels are
           stale; --accept-data makes the C in each;
  labels   for each label the table has, a word of it changed: to port by
           hand without --accept-data, the report naming its label; with it
           the C;
  divisor  a word of the span outside the command table labelled `divisor`
           (the analysis cannot rule out that the C divides by it): to port
           by hand without --accept-data, naming the word, no C;
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
           check command battles it (model_oracle.py --models), and mod-row
           prints the same rows.

The commands each case runs are the ones notes/modding.md gives ("Moving a
mod's attacks from MIPS to C"), which the test checks word for word.

    python tools/pc/test_model_migrate.py --out tmp/pc/model-migrate --jobs 6
    python tools/pc/test_model_migrate.py --only class --classifier tmp/pc/tests/memories_model_module_class_test.exe

With --sdk DIR (an unpacked release's `sdk` folder, outside the repository) the
commands are the SDK's own (`python <DIR>/tools/model_gate.py mod-migrate ...`,
`<DIR>/tools/build_mod.py`, `<DIR>/tools/model_oracle.py`), run from the --out
folder, so nothing can come from this checkout: the mods are made here, and
migrated, built and checked by the SDK. The oracle runs the game beside the
SDK unless --executable names another. `--only tables --only models --sdk ...`
is the check that the release's SDK does what the guide says.
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
CWD = ROOT              # where the commands run: the repository, or with --sdk the --out folder
TOOLS = "tools/pc"      # the tools' folder as the commands name it: with --sdk, the SDK's tools/ from CWD
TOOLS_DIR = ROOT / "tools/pc"
# The guide's commands, with the guide's own mod, disc, record and model.
GUIDE = {"mod": "mods/my-mod", "disc": "game/rpg-yfm.bin", "records": "83", "models": "pikachu"}
MIGRATE = "python tools/pc/model_gate.py mod-migrate {mod} --disc {disc}"
BUILD = BUILD_REPOSITORY = "python tools/pc/build_mod.py {mod} --target i386"
ORACLE_RECORDS = "python tools/pc/model_oracle.py --disc {disc} --mod {mod} --records {records}"
ORACLE_MODELS = "python tools/pc/model_oracle.py --disc {disc} --mod {mod} --models {models}"
PLAIN = "python tools/pc/model_oracle.py --disc {disc} --records {records}"
CASES = ("class", "tables", "data", "accept", "unread", "labels", "divisor", "replace", "const", "code", "models")


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
        result = subprocess.run(words, cwd=CWD, capture_output=True, text=True, errors="replace")
        if name:
            (self.arguments.out / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        return result

    def oracle(self, command: str, name: str, extra: list[str] = ()) -> dict:
        out = self.arguments.out / name
        shutil.rmtree(out, ignore_errors=True)
        flags = ["--out", str(out), "--jobs", str(self.arguments.jobs), *extra]
        if self.arguments.executable:
            flags += ["--executable", str(self.arguments.executable)]
        if self.arguments.reference_executable:   # an SDK's oracle builds no reference (reference_build.py)
            flags += ["--reference-executable", str(self.arguments.reference_executable)]
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
            ok &= subprocess.run([sys.executable, str(TOOLS_DIR / "build_mod.py"), str(mod), "--target", target],
                                 cwd=CWD).returncode == 0
        return ok


def rel(path: Path) -> str:
    """A path as the commands take it: from the root when inside the repository."""
    path = Path(path).resolve()
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def guide(test: Test) -> None:
    """The guide gives the commands the cases run, word for word (the repository's
    form of them: for an SDK it says to name tools/pc/ as the SDK's tools/)."""
    text = NOTES.read_text(encoding="utf-8")
    for command in (MIGRATE, BUILD_REPOSITORY, ORACLE_RECORDS, ORACLE_MODELS):
        line = command.format(**GUIDE).replace(TOOLS, "tools/pc")
        test.expect(line in text, f"notes/modding.md gives `{line}`")
    if TOOLS != "tools/pc":
        test.expect("sdk/tools/model_gate.py mod-migrate" in text and "sdk/tools/model_oracle.py" in text,
                    "notes/modding.md gives the SDK's form of them (sdk/tools/...)")
        line = BUILD.format(**GUIDE).replace(TOOLS, "sdk/tools")
        test.expect(line in text, f"notes/modding.md gives the SDK's build command `{line}` (no --target)")


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
    # mod-row on the model file: the same analysis, and the rows mod-migrate registered.
    rows = test.run(f"python {TOOLS}/model_gate.py mod-row {rel(mod / 'models' / 'pikachu.mrg')} "
                    f"--disc {rel(test.arguments.disc)}", name="models-mod-row")
    sha = {m["sha256"] for m in made}
    test.expect(rows.returncode == 0 and all(digest in rows.stdout for digest in sha) and
                rows.stdout.count("sizeof(MemoriesModelModule)") == len(sha),
                f"models: mod-row prints the rows of the modules mod-migrate made C for, and no other "
                f"(exit {rows.returncode})")


def case_data(test: Test, disc: model_oracle.Disc) -> None:
    """Shape 54's command table entry the attack picks zeroed (its divisors
    among them): not one of the disc's entries, so the C would divide by
    what it holds unchecked; the tool says to port it by hand and writes no
    C, mod-row prints no row; --accept-data makes the C and warns."""
    row, record, alternatives, plain, patches = shape54(test, disc)
    table, stride, count, _, _ = row.command_table
    commands = sorted(command for command in test_model_mod_c.setup_commands(plain).get((alternatives[0], "A"), ())
                      if 0 <= command < count)
    at = test_model_mod_c.stage_at(record, alternatives[0], "A") + table + commands[0] * stride
    zero = {"data": [{"file": "\\DATA\\MODEL.MRG;1", "patch": [{"at": at, "bytes": "00" * stride}]}]}
    mod = mod_folder(test.arguments.out / "mods" / "migrate-data", "migrate-data", zero)
    result, report = test.migrate(mod, "data-migrate")
    hand = modules(report, "hand")
    test.expect(result.returncode == 1 and len(hand) == 1 and hand[0]["hashed_words"] and
                "--accept-data" in hand[0]["why"] and not modules(report, "generate"),
                f"data: a zeroed table entry is to port by hand, naming its words "
                f"({[(m['where'], m['action'], m.get('hashed_words')) for m in hand]})")
    test.expect(not list(mod.glob("*.c")), "data: no C is written for it")
    stages = model_oracle.Disc.read(disc, record, model_migrate.MODULES_SECTOR, model_migrate.MODULES_SECTORS)
    image = bytearray(stages)
    offset = (at - (record * 0x114 + model_migrate.MODULES_SECTOR) * model_oracle.SECTOR)
    image[offset:offset + stride] = bytes(stride)
    (mod / "stages.bin").write_bytes(bytes(image))
    rows = test.run(f"python {TOOLS}/model_gate.py mod-row {rel(mod / 'stages.bin')} --disc {rel(test.arguments.disc)}",
                    name="data-mod-row")
    test.expect(rows.returncode == 1 and "port by hand" in rows.stdout and
                rows.stdout.count("sizeof(MemoriesModelModule)") == 0,
                f"data: mod-row says to port it by hand and prints no row for it (exit {rows.returncode})")
    (mod / "stages.bin").unlink()
    result = test.run(MIGRATE.format(mod=rel(mod), disc=rel(test.arguments.disc)), ["--accept-data"], "data-accept")
    report = json.loads((mod / model_migrate.COPIES / "migrate.json").read_text(encoding="utf-8"))
    test.expect(result.returncode == 0 and len(modules(report, "generate")) == 1 and "!! --accept-data" in result.stdout,
                "data: with --accept-data the C is made, and the report warns")


ACCEPT = (105, 0x1424, "D_8013C424")   # a shape, its GsIMAGE header labelled texture, the header's symbol


def case_accept(test: Test, disc: model_oracle.Disc) -> None:
    """A word of the span that is not in the command table, and that the C
    only hands to the texture packing: the page x of shape 105's GsIMAGE
    (D_8013C424), moved to another texture page. config/pc/model_span_words.csv
    labels it `texture`, which is not a proof: without --accept-data the tool
    says to port it by hand, naming the word and its label; with it the C,
    and the oracle shows that it draws what the interpreter draws, and not
    what the unpatched record draws (the patch is seen on screen)."""
    shape, first, symbol = ACCEPT
    row = next(r for r in model_gate.native_rows() if r.shape == shape)
    record, alternatives = test_model_mod_c.find_record(disc, row)
    plain = test.oracle(PLAIN.format(disc=rel(test.arguments.disc), records=record), "accept-unpatched",
                        ["--alternatives", ",".join(map(str, alternatives))])
    word = first + 4   # pmode, then px, py
    found = model_migrate.model_span_words.class_of(model_migrate.span_words(), "variant", shape, word)
    test.expect(found is not None and found[0] == "texture",
                f"accept: config/pc/model_span_words.csv classes the GsIMAGE's px texture ({found})")
    patches = []
    for alternative in alternatives:
        for slot in "AB":
            image = disc.variant(record, alternative, slot)
            test.expect(row.data_start <= word < row.span and not (row.command_table and
                        row.command_table[0] <= word < row.command_table[0] + row.command_table[1] *
                        row.command_table[2]), "accept: the GsIMAGE is inside the span and outside the command table")
            px = struct.unpack_from("<H", image, word)[0]
            patches.append({"at": test_model_mod_c.stage_at(record, alternative, slot) + word,
                            "bytes": struct.pack("<H", px + 0x40).hex()})
    mod = mod_folder(test.arguments.out / "mods" / "migrate-accept", "migrate-accept",
                     {"data": [{"file": "\\DATA\\MODEL.MRG;1", "patch": patches}]})
    result, report = test.migrate(mod, "accept-migrate")
    hand = modules(report, "hand")
    test.expect(result.returncode == 1 and hand and not modules(report, "generate") and
                all(m["hashed_words"] == [f"0x{word & ~3:X}"] and
                    m["span_labels"] == {f"0x{word & ~3:X}": ["texture", symbol]} and
                    f"0x{word & ~3:X} {symbol} (texture: handed to the texture packing)" in m["why"] and
                    "--accept-data at your own risk" in m["why"] for m in hand) and not list(mod.glob("*.c")),
                f"accept: a moved texture rectangle is to port by hand without --accept-data, the report naming "
                f"its word and label ({[(m['where'], m['action'], m.get('hashed_words')) for m in report['modules']]})")
    result = test.run(MIGRATE.format(mod=rel(mod), disc=rel(test.arguments.disc)), ["--accept-data"], "accept-accept")
    report = json.loads((mod / model_migrate.COPIES / "migrate.json").read_text(encoding="utf-8"))
    made = modules(report, "generate")
    test.expect(result.returncode == 0 and made and not modules(report, "hand") and
                "!! --accept-data, at your own risk" in result.stdout,
                f"accept: with --accept-data the C is made, the report warning "
                f"({[(m['where'], m['action']) for m in report['modules']]})")
    want = ORACLE_RECORDS.format(disc=rel(test.arguments.disc), mod=rel(mod), records=record)
    test.expect(test.build(mod, "accept-build"), "accept: the mod builds")
    found = test.oracle(want, "accept-oracle", ["--alternatives", ",".join(map(str, alternatives))])
    tag = "mod:migrate-accept"
    test.expect(found["exit"] == 0, "accept: the C draws what the interpreter draws (oracle exit 0)")
    test.expect(ways(found) == {tag}, f"accept: the subject ran {tag} and nothing else ({sorted(ways(found))})")
    test.expect(bool(digests(found)) and not set(digests(found)) & set(digests(plain)),
                f"accept: the patch changes the frames ({digests(plain)} unpatched, {digests(found)} patched)")


def case_unread(test: Test, disc: model_oracle.Disc) -> None:
    """Shape 54's word after its command table's last entry, which the
    module's own C does not read (labelled `unread`, which is not a proof:
    another module's C may read the bytes): port by hand without
    --accept-data, the report naming the word and its label. Then the same
    mod against a copy of the table whose digest of shape 54's C, or of the
    tool, is another: the report says the labels are stale. --accept-data
    makes the C in each."""
    row, record, alternatives, plain, _ = shape54(test, disc)
    word = 0x17B8
    table = model_migrate.span_words()
    found = model_migrate.model_span_words.class_of(table, "variant", 54, word)
    test.expect(found is not None and found[0] == "unread",
                f"unread: config/pc/model_span_words.csv labels shape 54's 0x{word:X} unread ({found})")
    patches = []
    for alternative in alternatives:
        for slot in "AB":
            image = disc.variant(record, alternative, slot)
            value = struct.unpack_from("<I", image, word)[0]
            patches.append({"at": test_model_mod_c.stage_at(record, alternative, slot) + word,
                            "bytes": struct.pack("<I", value ^ 0x01010101).hex()})
    mod = mod_folder(test.arguments.out / "mods" / "migrate-unread", "migrate-unread",
                     {"data": [{"file": "\\DATA\\MODEL.MRG;1", "patch": patches}]})
    labelled(test, mod, word, "unread", "D_8013C6D4", "unread")
    # The same mod against a table made from other C.
    digest = model_migrate.model_span_words.digests(model_migrate.model_span_words.TABLE)[("variant", 54)]
    copy = test.arguments.out / "stale_span_words.csv"
    text = model_migrate.model_span_words.TABLE.read_bytes().decode("utf-8")
    copy.write_bytes(text.replace(digest, "0" * 16).encode("utf-8"))
    stale_migrate(test, mod, copy, word, "unread-stale", "a table made from other C")
    # A table made by another version of the analysis: every row stale.
    tool = model_migrate.model_span_words.tool_digests(model_migrate.model_span_words.TABLE)
    copy = test.arguments.out / "stale_tool_span_words.csv"
    text = model_migrate.model_span_words.TABLE.read_bytes().decode("utf-8")
    for one in tool:
        text = text.replace(one, "f" * 16)
    copy.write_bytes(text.encode("utf-8"))
    stale_migrate(test, mod, copy, word, "unread-stale-tool", "a table made by another analysis")


def stale_migrate(test: Test, mod: Path, copy: Path, word: int, name: str, what: str) -> None:
    """The mod against a stale copy of the table: port by hand, the report
    saying the table is stale and naming the next steps; with --accept-data
    the C anyway, the report warning."""
    os.environ["MEMORIES_SPAN_WORDS"] = str(copy)
    try:
        result, report = test.migrate(mod, name)
        hand = modules(report, "hand")
        test.expect(result.returncode == 1 and len(hand) == 2 and not modules(report, "generate") and
                    all(m["hashed_words"] == [f"0x{word:X}"] and m["span_words_stale"] and
                        "the labels are stale" in m["why"] and "not a proof" in m["why"] and
                        "port the module by hand" in m["why"] and
                        "--accept-data at your own risk" in m["why"] for m in hand),
                    f"unread: with {what} the word is to port by hand, the labels stale, the next steps named "
                    f"({[(m['where'], m['action'], m.get('span_words_stale')) for m in report['modules']]})")
        result = test.run(MIGRATE.format(mod=rel(mod), disc=rel(test.arguments.disc)), ["--accept-data"],
                          f"{name}-accept")
        report = json.loads((mod / model_migrate.COPIES / "migrate.json").read_text(encoding="utf-8"))
    finally:
        del os.environ["MEMORIES_SPAN_WORDS"]
    made = modules(report, "generate")
    test.expect(result.returncode == 0 and len(made) == 2 and not modules(report, "hand") and
                "!! --accept-data, at your own risk" in result.stdout and "stale" in result.stdout,
                f"unread: with {what} and --accept-data the C is made, the report warning "
                f"({[(m['where'], m['action']) for m in report['modules']]})")


def labelled(test: Test, mod: Path, word: int, label: str, symbol: str, name: str) -> None:
    """mod-migrate on a mod that changes one labelled word: port by hand,
    the report naming the word, its symbol and its label; with --accept-data
    the C, the report warning."""
    result, report = test.migrate(mod, f"{name}-migrate")
    hand = modules(report, "hand")
    test.expect(result.returncode == 1 and hand and not modules(report, "generate") and
                all(m["hashed_words"] == [f"0x{word:X}"] and m["span_labels"] == {f"0x{word:X}": [label, symbol]} and
                    f"0x{word:X} {symbol} ({label}:" in m["why"] and "not a proof" in m["why"] and
                    "port the module by hand" in m["why"] for m in hand) and not list(mod.glob("*.c")),
                f"{name}: a changed `{label}` word is to port by hand without --accept-data, the report naming it "
                f"({[(m['where'], m['action'], m.get('span_labels')) for m in report['modules']]})")
    result = test.run(MIGRATE.format(mod=rel(mod), disc=rel(test.arguments.disc)), ["--accept-data"],
                      f"{name}-accept")
    report = json.loads((mod / model_migrate.COPIES / "migrate.json").read_text(encoding="utf-8"))
    test.expect(result.returncode == 0 and modules(report, "generate") and not modules(report, "hand") and
                "!! --accept-data, at your own risk" in result.stdout,
                f"{name}: with --accept-data the `{label}` word gets the C, the report warning")


def case_labels(test: Test, disc: model_oracle.Disc) -> None:
    """The invariant, per label: for each label config/pc/model_span_words.csv
    has, a word of it in a shape with a disc record, changed, is to port by
    hand without --accept-data; with it, the C."""
    table = model_migrate.span_words()
    rows = {row.shape: row for row in model_gate.native_rows()}
    found_rows: dict[int, tuple[int, list[int]] | None] = {}
    seen = set()
    for (kind, shape), runs in sorted(table.items()):
        if kind != "variant" or shape not in rows:
            continue
        for start, size, label, symbol, _ in runs:
            first = (start + 3) & ~3
            if label in seen or first + 4 > start + size:
                continue
            if shape not in found_rows:
                try:
                    found_rows[shape] = test_model_mod_c.find_record(disc, rows[shape])
                except SystemExit:
                    found_rows[shape] = None
            if found_rows[shape] is None:
                continue
            seen.add(label)
            record, alternatives = found_rows[shape]
            patches = []
            for alternative in alternatives:
                for slot in "AB":
                    image = disc.variant(record, alternative, slot)
                    value = struct.unpack_from("<I", image, first)[0]
                    patches.append({"at": test_model_mod_c.stage_at(record, alternative, slot) + first,
                                    "bytes": struct.pack("<I", value ^ 0x01010101).hex()})
            mod = mod_folder(test.arguments.out / "mods" / f"migrate-label-{label}", f"migrate-label-{label}",
                             {"data": [{"file": "\\DATA\\MODEL.MRG;1", "patch": patches}]})
            labelled(test, mod, first, label, symbol, f"labels-{label}")
    test.expect(seen >= {"divisor", "index", "bound", "address", "unknown", "value", "texture", "unread"},
                f"labels: every label the table has was changed in some shape ({sorted(seen)})")


def divisor_word(disc: model_oracle.Disc) -> tuple[model_gate.Row, int, int, list[int]] | None:
    """A variant row with a word outside its command table labelled `divisor`
    (config/pc/model_span_words.csv), and a record of the disc with its
    modules: (row, word, record, alternatives)."""
    table = model_migrate.span_words()
    rows = {row.shape: row for row in model_gate.native_rows()}
    for (kind, shape), runs in sorted(table.items()):
        if kind != "variant" or shape not in rows:
            continue
        for start, size, cls, _, _ in runs:
            if cls == "divisor" and size >= 4 and start % 4 == 0:
                try:
                    record, alternatives = test_model_mod_c.find_record(disc, rows[shape])
                except SystemExit:
                    break
                return rows[shape], start, record, alternatives
    return None


def case_divisor(test: Test, disc: model_oracle.Disc) -> None:
    """A word of the span outside the command table labelled `divisor` in
    config/pc/model_span_words.csv (the analysis cannot rule out that the C
    divides by it; no word outside a table is one the gate's own scan names),
    changed: the tool says to port by hand, names the word, and writes no C."""
    found = divisor_word(disc)
    test.expect(found is not None, "divisor: a disc record has a shape with a divisor word outside its table")
    if found is None:
        return
    row, word, record, alternatives = found
    patches = []
    for alternative in alternatives:
        for slot in "AB":
            image = disc.variant(record, alternative, slot)
            value = struct.unpack_from("<I", image, word)[0]
            patches.append({"at": test_model_mod_c.stage_at(record, alternative, slot) + word,
                            "bytes": struct.pack("<I", value ^ 1).hex()})
    mod = mod_folder(test.arguments.out / "mods" / "migrate-divisor", "migrate-divisor",
                     {"data": [{"file": "\\DATA\\MODEL.MRG;1", "patch": patches}]})
    result, report = test.migrate(mod, "divisor-migrate")
    hand = modules(report, "hand")
    test.expect(result.returncode == 1 and hand and not modules(report, "generate") and
                all(m["shape"] == row.shape and m["hashed_words"] == [f"0x{word:X}"] and
                    "--accept-data" in m["why"] for m in hand) and not list(mod.glob("*.c")),
                f"divisor: shape {row.shape}'s divisor word 0x{word:X} changed is to port by hand, naming it, "
                f"with no C ({[(m['where'], m['action'], m.get('hashed_words')) for m in report['modules']]})")
    test.expect(all("(divisor: may reach a divisor)" in m["why"] and "not a proof" in m["why"] for m in hand),
                f"divisor: the report labels the word with what it may reach, not a proof "
                f"({[m['why'][:160] for m in hand]})")
    result = test.run(MIGRATE.format(mod=rel(mod), disc=rel(test.arguments.disc)), ["--accept-data"],
                      "divisor-accept")
    report = json.loads((mod / model_migrate.COPIES / "migrate.json").read_text(encoding="utf-8"))
    test.expect(result.returncode == 0 and modules(report, "generate") and not modules(report, "hand") and
                "!! --accept-data, at your own risk" in result.stdout,
                "divisor: with --accept-data the locked word gets the C, the report warning")


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
    parser.add_argument("--reference-executable", type=Path,
                        help="the oracle's reference, a game with the MIPS interpreter (default the oracle's: the "
                             "reference build of this checkout; with --sdk one must be named)")
    parser.add_argument("--out", type=Path, default=ROOT / "tmp/pc/model-migrate")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--only", action="append", choices=CASES, help="the cases to run (default all)")
    parser.add_argument("--classifier", type=Path, help="memories_model_module_class_test (the class case)")
    parser.add_argument("--target", action="append", help="build the mods for these targets too (a build check)")
    parser.add_argument("--sdk", type=Path, help="run the SDK's tools (an unpacked release's sdk folder), "
                                                 "not this checkout's, from the --out folder")
    arguments = parser.parse_args()
    if not arguments.disc or not Path(arguments.disc).is_file():
        print("test_model_migrate: no disc (MEMORIES_DISC or --disc), skipped")
        return 77 if arguments.classifier else 0
    arguments.disc = Path(arguments.disc).resolve()
    arguments.out = arguments.out.resolve()
    arguments.out.mkdir(parents=True, exist_ok=True)
    if arguments.sdk:
        global CWD, TOOLS, TOOLS_DIR, MIGRATE, BUILD, ORACLE_RECORDS, ORACLE_MODELS, PLAIN
        sdk = arguments.sdk.resolve()
        if not (sdk / "tools/model_gate.py").is_file() or ROOT.resolve() in sdk.parents:
            parser.error(f"--sdk {sdk}: not an unpacked SDK outside this checkout (tools/model_gate.py)")
        CWD, TOOLS_DIR = arguments.out, sdk / "tools"
        TOOLS = os.path.relpath(TOOLS_DIR, CWD).replace(os.sep, "/")
        MIGRATE, BUILD, ORACLE_RECORDS, ORACLE_MODELS, PLAIN = (
            text.replace("tools/pc", TOOLS) for text in (MIGRATE, BUILD, ORACLE_RECORDS, ORACLE_MODELS, PLAIN))
        # An SDK's build_mod builds every target (the release beside it may be the 32-bit or the 64-bit game):
        # the tool prints no --target there.
        BUILD = BUILD.replace(" --target i386", "")
        # The game beside an SDK has no MIPS interpreter: its oracle is handed
        # this checkout's reference build unless one is named.
        if not arguments.reference_executable:
            from reference_build import reference_executable
            arguments.reference_executable = reference_executable()
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
