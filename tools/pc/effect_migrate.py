#!/usr/bin/env python3
"""A mod's duel effect bank, against the disc's (`model_gate.py
effect-migrate`; notes/modding.md, "A mod's duel effect bank"): each byte the
mod changes in each copy of the bank in WA_MRG.MRG, named by what holds it
(the function, the table, the effect ids that use it and the game's code that
starts them) and whether it is code or data.

  data only  the game runs the bank's C on the mod's tables when its mod.json
             says "duel_effects": "own data" (src/pc/overlays/duel_effects.c):
             this writes it, and prints the commands that check it, the
             bank's C with the mod's data against the MIPS interpreter
             running the mod's bank on a reference build (test_duel_effects.py
             --mod: every frame, every effect call and the duel's state after
             each scenario)
  code       nothing is converted: the bank's code has no mod API for native C
             yet, so its effects complete at once in the game (a reference
             build interprets them). The report names the changed functions;
             exit status 1

    python tools/pc/model_gate.py effect-migrate mods/my-mod --disc game/rpg-yfm.bin
    python tools/pc/model_gate.py effect-migrate mods/my-mod --disc game/rpg-yfm.bin --report-only

The table it reads, config/pc/duel_effect_bank.csv (every byte range of the
bank: start, end, code or data, what, symbol, effect ids, label) and
config/pc/duel_effect_ids.csv (an effect id: its function, its file, the
game's functions that start it), are made from the decomp in a checkout:

    python tools/pc/effect_migrate.py write    # rewrite both from config/slus_01411 and src/
    python tools/pc/effect_migrate.py check    # they are current, and the code ranges are the game's
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import model_layout as layout  # noqa: E402

BANK_TABLE = "config/pc/duel_effect_bank.csv"
IDS_TABLE = "config/pc/duel_effect_ids.csv"
BASE, SIZE = 0x80146000, 0x16000
BANK_SHA = "baa203b937dc6bdf91b1826c5832f0f32e11ae5fe9d05193a4361bc08158b9e0"
IDENTIFIER = b"\x18\0\0\0"
SECTOR = 2048
WA = "\\DATA\\WA_MRG.MRG"
OPT_IN = ("duel_effects", "own data")
# The bank's segments (config/slus_01411/overlays/duel_effects.yaml) and what
# each is: the header's identifier and format strings, the jump table and the
# text are code (the game checks them, src/pc/guest/retail_image.c); the rest
# is data.
TEXT = (0x2B0, 0x14270)
JUMP_TABLE = (0x9C, 0x180)
SEGMENTS = ((0x0, 0x9C), JUMP_TABLE, (0x180, 0x2B0), TEXT, (0x14270, SIZE))
FIELDS = ("start", "end", "kind", "what", "symbol", "effects", "label")


# --- the table (a checkout's) ---------------------------------------------------------

def _repository() -> Path:
    root = Path(__file__).resolve().parents[2]
    if not (root / "config/slus_01411/overlays/duel_effects.yaml").is_file():
        raise SystemExit("effect_migrate: write and check work on a checkout of the game's source")
    return root


def _definitions(text: str) -> dict[str, str]:
    """{function: its body} of a C file (definitions start at column 0)."""
    bodies, name, lines = {}, None, []
    for line in text.splitlines():
        found = re.match(r"[A-Za-z_][\w \t\*]*?\b(func_[0-9A-F]{8})\s*\([^;]*$", line)
        if found and not line.startswith((" ", "\t", "#", "/", "*")):
            if name:
                bodies[name] = "\n".join(lines)
            name, lines = found.group(1), []
        elif name:
            lines.append(line)
    if name:
        bodies[name] = "\n".join(lines)
    return bodies


def _callers(root: Path) -> dict[int, list[str]]:
    """{effect id: the game's functions that start it} (DuelEffect_AllocateRequest
    or DuelEffect_CreateRequest with the id written out)."""
    found: dict[int, set[str]] = {}
    for path in sorted((root / "src/game").glob("*.c")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for call in re.finditer(r"DuelEffect_(?:Allocate|Create)Request\(\s*(0x[0-9A-Fa-f]+|\d+)\s*\)", text):
            before = text[:call.start()]
            heads = re.findall(r"^[A-Za-z_][^;\n]*?\b(\w+)\s*\([^;\n]*\)?\s*\n?\{", before, re.M)
            function = heads[-1] if heads else path.stem
            if function in ("if", "while", "for", "switch"):
                function = path.stem
            found.setdefault(int(call.group(1), 0), set()).add(function)
    return {key: sorted(value) for key, value in found.items()}


def generate(root: Path | None = None) -> tuple[str, str]:
    """(duel_effect_bank.csv, duel_effect_ids.csv) from the decomp."""
    root = root or _repository()
    overlays, source = root / "config/slus_01411/overlays", root / "src/overlays/duel_effects"
    functions = []
    with (overlays / "duel_effects_functions.csv").open(encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            functions.append((int(row["address"], 16) - BASE, int(row["size"], 16), row["name"]))
    bodies, files, externs = {}, {}, {}
    for path in sorted(source.glob("*.[ch]")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for name, body in _definitions(text).items() if path.suffix == ".c" else ():
            bodies[name], files[name] = body, path.name
        for match in re.finditer(r"extern\s+([\w \t\*]+?)\s*\**\b(\w+)\s*((?:\[[^\]]*\])*)\s*;", text):
            externs.setdefault(match.group(2), (match.group(1).strip() + " " + match.group(3)).strip())
    dispatch = (source / "dispatch.c").read_text(encoding="utf-8")
    roots = {int(m.group(1)): m.group(2) for m in
             re.finditer(r"if \(effect == (\d+)\) \{.*?\b(func_[0-9A-F]{8})\(", dispatch, re.S)}
    calls = {name: set(re.findall(r"\b(func_[0-9A-F]{8})\s*\(", body)) - {name} for name, body in bodies.items()}
    reached: dict[str, set[int]] = {}
    for effect, first in roots.items():
        todo, seen = [first], set()
        while todo:
            name = todo.pop()
            if name in seen:
                continue
            seen.add(name)
            reached.setdefault(name, set()).add(effect)
            todo.extend(calls.get(name, ()))
    dispatcher = files and next((name for name, file in files.items() if file == "dispatch.c"), None)
    rows = []
    for offset, size, name in functions:
        effects = sorted(reached.get(name, ()))
        rows.append((offset, offset + size, "code", "function", name,
                     "all" if name == dispatcher else " ".join(map(str, effects)), files.get(name, "")))
    symbols = []
    for line in (overlays / "duel_effects_symbols.txt").read_text(encoding="utf-8").splitlines():
        match = re.match(r"(\w+) = 0x([0-9A-Fa-f]+);(.*)", line)
        if not match or match.group(1).startswith("func_") or "absolute" in match.group(3):
            continue
        offset = int(match.group(2), 16) - BASE
        if 0 <= offset < SIZE and not TEXT[0] <= offset < TEXT[1]:
            symbols.append((offset, match.group(1)))
    symbols.sort()
    for index, (offset, name) in enumerate(symbols):
        segment_end = next(end for start, end in SEGMENTS if start <= offset < end)
        end = min(segment_end, symbols[index + 1][0] if index + 1 < len(symbols) else SIZE)
        if externs.get(name, "").endswith("*G32") or externs.get(name, "").endswith("*"):
            end = min(end, offset + 4)   # a pointer: what follows the last one is padding
        users = sorted({effect for function, body in bodies.items() if re.search(rf"\b{name}\b", body)
                        for effect in reached.get(function, ())})
        users_all = any(re.search(rf"\b{name}\b", body) for function, body in bodies.items()
                        if function == dispatcher)
        kind, what = "data", "table"
        written = rf"(?:\b{name}\b(?:\s*(?:\.|->)\s*\w+|\s*\[[^\]]*\])*\s*[-+*/|&^]?=(?!=)|setVector\(\s*&{name}\b)"
        if any(re.search(written, body) for body in bodies.values()):
            what = "variable"   # the bank's C writes it as it runs
        if offset == 0:
            what = "identifier"
            kind = "code"
        elif name.startswith("gNorthAmerican_") or JUMP_TABLE[0] <= offset < JUMP_TABLE[1]:
            kind, what = "code", "format string" if offset < JUMP_TABLE[0] else "jump table"
        rows.append((offset, end, kind, what, name, "all" if users_all else " ".join(map(str, users)),
                     externs.get(name, "")))
    # What no symbol names: the identifier word, the jump table, and gaps.
    if not any(row[0] == 0 for row in rows):
        rows.append((0, 4, "code", "identifier", "", "", "the bank's first word, 0x18"))
    if not any(row[0] == JUMP_TABLE[0] for row in rows):
        rows.append((JUMP_TABLE[0], JUMP_TABLE[1], "code", "jump table", "", "22", "effect_22.c's switch"))
    rows.sort()
    filled, at = [], 0
    for row in rows:
        if row[0] > at:
            inside_text = TEXT[0] <= at < TEXT[1]
            filled.append((at, row[0], "code" if inside_text else "data", "padding", "", "", ""))
        filled.append(row)
        at = max(at, row[1])
    if at < SIZE:
        filled.append((at, SIZE, "data", "padding", "", "", ""))
    bank = io.StringIO()
    writer = csv.writer(bank, lineterminator="\n")
    writer.writerow(FIELDS)
    for start, end, kind, what, symbol, effects, label in filled:
        writer.writerow((f"0x{start:05X}", f"0x{end:05X}", kind, what, symbol, effects, label))
    ids = io.StringIO()
    writer = csv.writer(ids, lineterminator="\n")
    writer.writerow(("id", "function", "file", "started_by"))
    callers = _callers(root)
    for effect, first in sorted(roots.items()):
        writer.writerow((effect, first, files.get(first, ""), " ".join(callers.get(effect, []))))
    return bank.getvalue(), ids.getvalue()


def code_ranges_in_game(root: Path) -> list[tuple[int, int]]:
    """The code ranges src/pc/guest/retail_image.c checks."""
    text = (root / "src/pc/guest/retail_image.c").read_text(encoding="utf-8")
    block = re.search(r"duel_effects_code\[\]\s*=\s*\{(.*?)\};", text, re.S).group(1)
    return [(int(a, 16), int(b, 16)) for a, b in re.findall(r"\{\s*(0x[0-9A-Fa-f]+)u?\s*,\s*(0x[0-9A-Fa-f]+)u?\s*\}",
                                                             block)]


def merged(ranges: list[tuple[int, int]]) -> list[tuple[int, int]]:
    out: list[list[int]] = []
    for start, end in sorted(ranges):
        if out and start <= out[-1][1]:
            out[-1][1] = max(out[-1][1], end)
        else:
            out.append([start, end])
    return [(start, end) for start, end in out]


def check_tables(root: Path | None = None) -> list[str]:
    root = root or _repository()
    bank, ids = generate(root)
    problems = []
    for name, made in ((BANK_TABLE, bank), (IDS_TABLE, ids)):
        path = root / name
        if not path.is_file() or path.read_text(encoding="utf-8") != made:
            problems.append(f"{name} is not what the decomp makes: python tools/pc/effect_migrate.py write")
    code = merged([(row.start, row.end) for row in read_bank(io.StringIO(bank)) if row.kind == "code"])
    game = merged(code_ranges_in_game(root))
    if code != game:
        problems.append(f"the table's code ranges {[(hex(a), hex(b)) for a, b in code]} are not the ones "
                        f"src/pc/guest/retail_image.c checks {[(hex(a), hex(b)) for a, b in game]}")
    return problems


# --- the table, read (a checkout's or an SDK's) ------------------------------------------

class Row:
    def __init__(self, record: dict):
        self.start, self.end = int(record["start"], 16), int(record["end"], 16)
        self.kind, self.what, self.symbol = record["kind"], record["what"], record["symbol"]
        self.effects, self.label = record["effects"], record["label"]

    def named(self, offset: int) -> str:
        symbol = self.symbol or f"({self.what})"
        return symbol + (f"+0x{offset - self.start:X}" if offset > self.start else "")


def read_bank(handle=None) -> list[Row]:
    if handle is None:
        handle = layout.path(BANK_TABLE).open(encoding="utf-8")
    with handle:
        return [Row(record) for record in csv.DictReader(handle)]


def read_ids() -> dict[int, dict]:
    with layout.path(IDS_TABLE).open(encoding="utf-8") as handle:
        return {int(record["id"]): record for record in csv.DictReader(handle)}


def row_at(rows: list[Row], offset: int) -> Row:
    return next(row for row in rows if row.start <= offset < row.end)


# --- a mod --------------------------------------------------------------------------

def wa_disc(disc_path: Path):
    """model_migrate's Disc for WA_MRG.MRG: the file as the mod's "data"
    entries leave it (replacements, then patches)."""
    import model_migrate

    class WaDisc(model_migrate.Disc):
        FILE, NAME = ["DATA", "WA_MRG.MRG;1"], WA
    return WaDisc(disc_path)


def bank_copies(original: bytes) -> list[int]:
    """Where the disc's WA_MRG.MRG holds the retail bank, by sector."""
    return [at for at in range(0, len(original) - SIZE + 1, SECTOR)
            if original[at:at + 4] == IDENTIFIER and hashlib.sha256(original[at:at + SIZE]).hexdigest() == BANK_SHA]


def runs(offsets: list[int]) -> list[tuple[int, int]]:
    out: list[list[int]] = []
    for offset in offsets:
        if out and offset == out[-1][1]:
            out[-1][1] += 1
        else:
            out.append([offset, offset + 1])
    return [(start, end) for start, end in out]


def describe_effects(effects: str, ids: dict[int, dict]) -> str:
    if not effects:
        return "no effect's code names it"
    if effects == "all":
        return "every effect (the dispatcher's)"
    parts = []
    for effect in map(int, effects.split()):
        started = ids.get(effect, {}).get("started_by", "")
        parts.append(f"{effect} (0x{effect:X}" + (f": {started.split()[0]}" + (" and more" if " " in started else "")
                                                 if started else "") + ")")
    return "effect" + ("s " if len(parts) > 1 else " ") + ", ".join(parts)


def analyse(folder: Path, disc_path: Path) -> dict:
    manifest = json.loads((folder / "mod.json").read_text(encoding="utf-8-sig"))
    disc = wa_disc(disc_path)
    sectors = disc.size // SECTOR
    original = disc.original(0, sectors)[:disc.size]
    disc.apply(manifest, folder)
    modded = disc.read(0, sectors)[:disc.size]
    disc.file.close()
    rows = read_bank()
    copies = []
    for at in bank_copies(original):
        mine = modded[at:at + SIZE]
        changed = [i for i in range(SIZE) if mine[i] != original[at + i]]
        copies.append({"at": at, "changed": changed, "bytes": bytes(mine[i] for i in changed)})
    spans, code, data, effects = [], set(), set(), set()
    first = next((copy for copy in copies if copy["changed"]), None)
    for copy in copies:
        for start, end in runs(copy["changed"]):
            offset = start
            while offset < end:
                row = row_at(rows, offset)
                stop = min(end, row.end)
                key = (offset, stop)
                if key not in {(s["start"], s["end"]) for s in spans}:
                    spans.append({"start": offset, "end": stop, "kind": row.kind, "what": row.what,
                                  "symbol": row.named(offset), "effects": row.effects, "label": row.label})
                (code if row.kind == "code" else data).add(row.symbol or row.what)
                if row.effects and row.effects != "all":
                    effects.update(map(int, row.effects.split()))
                offset = stop
    spans.sort(key=lambda span: span["start"])
    groups: list[list[dict]] = []
    for copy in copies:
        for group in groups:
            if group[0]["changed"] == copy["changed"] and group[0]["bytes"] == copy["bytes"]:
                group.append(copy)
                break
        else:
            groups.append([copy])
    verdict = "disc" if not first else "code" if code else "data"
    return {"mod": str(manifest.get("id") or folder.name), "manifest": manifest, "copies": copies, "groups": groups,
            "spans": spans, "verdict": verdict, "code": sorted(code), "effects": sorted(effects),
            "opted": manifest.get(OPT_IN[0]) == OPT_IN[1], "library": bool(manifest.get("library") or
                                                                          manifest.get("libraries"))}


def write_opt_in(folder: Path) -> bool:
    path = folder / "mod.json"
    manifest = json.loads(path.read_text(encoding="utf-8-sig"))
    if manifest.get(OPT_IN[0]) == OPT_IN[1]:
        return False
    manifest[OPT_IN[0]] = OPT_IN[1]
    path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return True


def scenarios_for(effects: list[int]) -> tuple[list[str], list[int]]:
    """(test_duel_effects.py's scenarios that play these effects, the effects none plays)."""
    import test_duel_effects
    names, covered = [], set()
    for name, played in test_duel_effects.EFFECT_IDS.items():
        if set(played) & set(effects):
            names.append(name)
            covered.update(set(played) & set(effects))
    return names, [effect for effect in effects if effect not in covered]


def verify_commands(folder: Path, names: list[str]) -> list[str]:
    import model_migrate
    mod = model_migrate.quote(str(folder).replace(os.sep, "/"))
    commands = []
    if not layout.SHIPPED:
        commands.append(f"python {model_migrate.quote(model_migrate.tool_path('build_game32.py'))} --reference")
    test = f"python {model_migrate.quote(model_migrate.tool_path('test_duel_effects.py'))} --mod {mod}"
    if layout.SHIPPED:
        test += " --reference-executable PATH"
    return commands + [test + "".join(" " + name for name in names)]


def report(result: dict, folder: Path, wrote: bool, quiet: bool = False) -> None:
    ids = read_ids()
    say = (lambda *a, **k: None) if quiet else print
    copies, groups = result["copies"], result["groups"]
    say(f"effect-migrate: {result['mod']}: the duel effect bank, {len(copies)} copies in WA_MRG.MRG")
    if not copies:
        say("    the disc's WA_MRG.MRG holds no copy of the retail bank: is it the US disc?")
        return
    if result["verdict"] == "disc":
        say("    the mod changes none of them: the bank's C runs the disc's bank; nothing to do")
        return
    for group in groups:
        where = ", ".join(f"+0x{copy['at']:X}" for copy in group)
        count = len(group[0]["changed"])
        say(f"  {'copy' if len(group) == 1 else f'{len(group)} copies'} at {where}: "
            + (f"{count} byte{'s' if count > 1 else ''} changed" if count else "the disc's bytes"))
    if len(groups) > 1:
        say("  WARNING: the copies differ: which one the game loads depends on where it is (each is a scene's)")
    for span in result["spans"]:
        last = span["end"] - 1
        where = f"0x{span['start']:05X}" + (f"-0x{last:05X}" if last > span["start"] else "")
        say(f"    {where}  {span['kind']:4}  {span['symbol']}  {span['what']}"
            + (f" {span['label']}" if span["label"] else "") + f": {describe_effects(span['effects'], ids)}")
    if result["verdict"] == "code":
        say(f"code changed ({', '.join(result['code'])}): not converted. The bank's code has no mod API for "
            "native C yet (a later mod API entry); in the game its effects complete at once, unseen, and a "
            "reference build interprets them.")
        return
    say("data only: the bank's C runs the mod's tables when its mod.json says \"duel_effects\": \"own data\" "
        "(nothing checks them: a value the C divides by or indexes with can stop the game)")
    if wrote:
        say("mod.json: added \"duel_effects\": \"own data\"")
    elif result["opted"]:
        say("mod.json: has \"duel_effects\": \"own data\" already")
    else:
        say("mod.json: not written (--report-only): add \"duel_effects\": \"own data\"")
    names, uncovered = scenarios_for(result["effects"])
    say("check it, the bank's C with the mod's data against the interpreter running the mod's bank "
        "(a reference build: every frame, every effect call, the duel after each scenario):")
    for command in verify_commands(folder, names):
        say("    " + command)
    for effect in uncovered:
        say(f"    effect {effect} ({ids.get(effect, {}).get('started_by', '') or 'no game code names it'}): no "
            "scenario of test_duel_effects.py plays it; check it in a duel")


def migrate(folder: Path, disc: Path, report_only: bool = False, quiet: bool = False) -> dict:
    result = analyse(folder, disc)
    wrote = result["verdict"] == "data" and not report_only and write_opt_in(folder)
    report(result, folder, wrote, quiet)
    result["wrote"] = wrote
    return result


def main(argv: list[str] | None = None) -> int:
    layout.tolerant_output()
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] in (["write"], ["check"]):
        root = _repository()
        if argv[0] == "write":
            bank, ids = generate(root)
            (root / BANK_TABLE).write_text(bank, encoding="utf-8", newline="\n")
            (root / IDS_TABLE).write_text(ids, encoding="utf-8", newline="\n")
            print(f"effect_migrate: wrote {BANK_TABLE} ({bank.count(chr(10)) - 1} ranges) and {IDS_TABLE}")
            return 0
        problems = check_tables(root)
        for problem in problems:
            print("effect_migrate: " + problem, file=sys.stderr)
        print(f"effect_migrate: check: {len(problems)} problem(s)")
        return 1 if problems else 0
    parser = argparse.ArgumentParser(prog="model_gate.py effect-migrate", description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("effect-migrate",))
    parser.add_argument("target", type=Path, help="the mod's folder")
    parser.add_argument("--disc", type=Path, default=os.environ.get("MEMORIES_DISC"),
                        help="the disc image (default: MEMORIES_DISC)")
    parser.add_argument("--report-only", action="store_true", help="say what changed; write nothing")
    arguments = parser.parse_args(argv)
    if not arguments.disc or not Path(arguments.disc).is_file():
        parser.error("effect-migrate needs the disc: --disc or MEMORIES_DISC")
    if not (arguments.target / "mod.json").is_file():
        parser.error(f"{arguments.target}: no mod.json")
    result = migrate(arguments.target, Path(arguments.disc), arguments.report_only)
    return 1 if result["verdict"] == "code" else 0


if __name__ == "__main__":
    raise SystemExit(main())
