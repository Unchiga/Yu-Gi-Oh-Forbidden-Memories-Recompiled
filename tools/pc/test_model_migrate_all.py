#!/usr/bin/env python3
"""Every native shape's C, made by model_gate.py mod-migrate into one mod, run
against the interpreter on every record of the disc (MEMORIES_DISC or
--disc; skipped without one). The "mod" is the disc's own records
(--records all) with every row MEMORIES_MODEL_OVER_DISC (--over-disc): its
copies of the 324 variant and 5 primary shapes' C, each module under its
prefix, with rule 6 storage of its own, stand over the disc's C wherever the
game would run that, and the trace must say mod:model-all there.

It runs in parts, each a run of tools/pc/model_oracle.py short enough to
watch:

    python tools/pc/test_model_migrate_all.py generate --out tmp/pc/model-migrate-all
    python tools/pc/test_model_migrate_all.py build --out tmp/pc/model-migrate-all
    python tools/pc/test_model_migrate_all.py run --out tmp/pc/model-migrate-all --part 1 --parts 11
    ...
    python tools/pc/test_model_migrate_all.py report --out tmp/pc/model-migrate-all

`generate` writes the mod; `build` builds it (i386, or --target);
`run` battles one part of the records (both alternatives, ties: both
slots' modules run); `report` adds the parts up: the battles identical,
what ran each battle slot's variant module, and each primary module with
code (exit 0 when every battle is identical and every slot the disc's C
would run ran the mod's C instead).
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import model_oracle  # noqa: E402

MOD_ID = "model-all"
TAG = "mod:" + MOD_ID


def records() -> list[int]:
    return list(range(model_oracle.RECORDS))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("step", choices=("generate", "build", "run", "report"))
    parser.add_argument("--disc", type=Path, default=os.environ.get("MEMORIES_DISC"))
    parser.add_argument("--out", type=Path, default=ROOT / "tmp/pc/model-migrate-all")
    parser.add_argument("--part", type=int, default=1)
    parser.add_argument("--parts", type=int, default=11)
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--executable", type=Path, help="the game (default model_oracle's)")
    parser.add_argument("--target", default="i386", help="build: the target (build_mod.py's; default i386)")
    arguments = parser.parse_args()
    if not arguments.disc or not Path(arguments.disc).is_file():
        print("test_model_migrate_all: no disc (MEMORIES_DISC or --disc), skipped")
        return 0
    out = arguments.out.resolve()
    mod = out / MOD_ID
    if arguments.step == "generate":
        mod.mkdir(parents=True, exist_ok=True)
        (mod / "mod.json").write_text(json.dumps({"id": MOD_ID, "name": "Every native shape's C, as a mod",
                                                  "version": "1.0.0", "enabled": False}, indent=1) + "\n",
                                      encoding="utf-8")
        result = subprocess.run([sys.executable, str(ROOT / "tools/pc/model_gate.py"), "mod-migrate", str(mod),
                                 "--disc", str(arguments.disc), "--records", "all", "--over-disc"], cwd=ROOT,
                                capture_output=True, text=True, errors="replace")
        (out / "migrate.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        report = json.loads((mod / "model_c/migrate.json").read_text(encoding="utf-8"))
        hand = [m for m in report["modules"] if m["action"] == "hand"]
        print(f"test_model_migrate_all: {report['units']} module copies, {report['rows']} rows; "
              f"{report['consts']['plain']} of {report['consts']['total']} consts the C defines are the disc's "
              f"bytes; {len(hand)} to port by hand (exit {result.returncode})")
        return 1 if result.returncode or hand else 0
    if arguments.step == "build":
        built = subprocess.run([sys.executable, str(ROOT / "tools/pc/build_mod.py"), str(mod), "--target",
                                arguments.target], cwd=ROOT, capture_output=True, text=True, errors="replace")
        (out / f"build-{arguments.target}.log").write_text(built.stdout + built.stderr, encoding="utf-8")
        print(f"test_model_migrate_all: build {arguments.target}: exit {built.returncode}")
        if built.returncode:
            print((built.stdout + built.stderr)[-3000:])
        return built.returncode
    if arguments.step == "run":
        every = records()
        size = -(-len(every) // arguments.parts)
        part = every[(arguments.part - 1) * size:arguments.part * size]
        folder = out / f"part-{arguments.part:02}"
        command = [sys.executable, "-u", str(ROOT / "tools/pc/model_oracle.py"), "--disc", str(arguments.disc),
                   "--mod", str(mod), "--records", ",".join(map(str, part)), "--jobs", str(arguments.jobs),
                   "--out", str(folder)]
        if arguments.executable:
            command += ["--executable", str(arguments.executable)]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, errors="replace")
        (out / f"part-{arguments.part:02}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        print("\n".join(line for line in result.stdout.splitlines() if line.startswith(("model oracle:", "  "))))
        return result.returncode
    # report
    verdicts, claims, missing = [], set(), []
    for part in range(1, arguments.parts + 1):
        path = out / f"part-{part:02}" / "report.json"
        if not path.is_file():
            missing.append(part)
            continue
        report = json.loads(path.read_text(encoding="utf-8"))
        verdicts += report["verdicts"]
        claims |= {tuple(claim) for claim in report["claims"]}
    ran: dict[str, int] = {}
    primaries: dict[str, int] = {}
    for verdict in verdicts:
        for ways in verdict.get("backends", {}).get("subject", {}).values():
            for way in ways:
                ran[way] = ran.get(way, 0) + 1
        for module, ways in verdict.get("paths", {}).get("subject", {}).items():
            if module.startswith("primary "):
                for way in ways:
                    primaries[way] = primaries.get(way, 0) + 1
    identical = sum(v["status"] == "identical" for v in verdicts)
    print(f"test_model_migrate_all: {identical} of {len(verdicts)} battles identical "
          f"({sum(v['status'] == 'differs' for v in verdicts)} differ, "
          f"{sum(v['status'] == 'error' for v in verdicts)} errors); {len(claims)} rows registered")
    print(f"  the subject's variant modules (battle slots): {ran}")
    print(f"  the subject's primary module calls, by what ran them: {primaries}")
    if missing:
        print(f"  parts not run: {missing}")
    for verdict in [v for v in verdicts if v["status"] != "identical"][:20]:
        print(f"  {verdict['battle']} {verdict['status']}: {verdict.get('why', '')[:300]}")
    # The oracle errs any battle slot that ran something other than the mod's
    # C where a row covers its image, so identical battles ran mod: there;
    # "native" (the disc's C) must not appear at all.
    good = (not missing and identical == len(verdicts) and verdicts and "native" not in ran and TAG in ran
            and "native" not in primaries and primaries.get(TAG))
    return 0 if good else 1


if __name__ == "__main__":
    raise SystemExit(main())
