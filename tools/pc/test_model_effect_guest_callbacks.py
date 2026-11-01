#!/usr/bin/env python3
"""Test the real animation controller's 16-byte callback-table copy."""

import argparse
from pathlib import Path

from guest_test_ir import read_guest_source, run_translated_fixture, select_functions
from llvm_guest import inspect
from translate_guest_ir import address_map, translate

ROOT = Path(__file__).resolve().parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--sanitize", action="store_true")
    a = p.parse_args()
    out = ROOT / "tmp/model-effect-guest-callbacks"
    out.mkdir(parents=True, exist_ok=True)
    raw = read_guest_source(ROOT / "src/game/model_intro_controller.c", out)
    ir = translate(
        select_functions(raw, {"func_8004EB00"}),
        address_map(ROOT / "config/pc/guest_addresses.txt"),
    )
    (out / "controller.ll").write_text(ir)
    names = set(inspect(ir)["declarations"])
    unused = sorted(
        n
        for n in names
        if not n.startswith(("llvm.", "GuestRuntime_"))
        and n not in ("func_8005A188", "Cards_ModelStatsIndex")
    )
    (out / "unused.c").write_text(
        "#include <stdlib.h>\n"
        + "".join(
            ("int " if n == "rand" else "void ") + n + "(void) { abort(); }\n"
            for n in unused
        )
    )
    run_translated_fixture(
        [out / "controller.ll"],
        ROOT / "tests/pc/model_effect_guest_callbacks_test.c",
        out,
        sanitize=a.sanitize,
        extra_sources=[out / "unused.c"],
    )


if __name__ == "__main__":
    main()
