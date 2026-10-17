"""peek and poke by name reach the game's small data (.sdata and .sbss).

The game's small-data variables (gGraphics_sViewportX, section(".sbss")) are
defined by game units, so a build keeps them in its own game_data or
game_bss, not at the retail address config/pc/guest_addresses.txt lists (only
a variable the units leave undefined is pinned there). A peek by that address
read the guest RAM beside it: 0 with the Deck on screen, and a poke of 40 did
nothing. Game.peek and Game.poke now ask the game where a name is
(`peek @NAME`, from the build's symbols/<id>.vars.txt).

In the running game, on Build Deck (the Chest and the Deck sit side by side
and the view slides between them by gGraphics_sViewportX, 0 on the Chest and
320 on the Deck):
  * the name reads 320 on the Deck and 0 back on the Chest;
  * a poke by name shows (the picture four frames on, from the same state, differs) and
    a peek by name reads it;
  * a read or write past the variable's end is an error, not the next
    variable, and a name nothing knows is a KeyError;
  * a pinned global (D_8009B26C, the mode) still reads by name as at its
    retail address, which the game's table gives;
  * a variable native code defines outside game_data (gRand_dwSeed, which
    runs on after a poke) and a module's (gCampaignMap_Location, an ovl_*
    section) are found in the process, not at their retail addresses;
  * a constant in read-only data (D_80180004) reads and a poke into it is
    refused ("read-only"), where writing it would fault the game;
  * a retail name the build neither defines nor pins (gDuel_wPlayerLifePoint:
    no code of the game's refers to it) is read from guest RAM at its retail
    address, 8000 at the start of a duel;
  * a name with an offset in it ("gFoo+4") is refused, not sent.
An executable from before the fix reads 0 on the Deck and fails these. It
needs the disc, so it is a local gate like the replays (notes/agent-control.md).

    python tests/pc/control_small_data_runtime.py --exe tmp/pc/game32/memories-pc.exe --out <folder>
"""
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/pc"))
from yfm_control import ControlError, Game  # noqa: E402

SETTINGS = {"mod.3d-monsters": 0, "mod.hand-camera": 0, "deck_slots": 0}
MODE = 0x8009B26C   # D_8009B26C, pinned: the active mode

checks: list[tuple[str, bool]] = []


def check(what: str, ok: bool) -> bool:
    checks.append((what, bool(ok)))
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    return ok


def raises(error: type[Exception], action) -> bool:
    try:
        action()
    except error:
        return True
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--exe", help="the game (default: yfm_control's)")
    parser.add_argument("--out", default=str(ROOT / "tmp/pc/control-small-data"))
    args = parser.parse_args()
    out = Path(args.out)
    shutil.rmtree(out, ignore_errors=True)
    with Game(args.exe, out=out, settings=SETTINGS) as game:
        game.step(900)   # the logos, the movie and the title
        game.goto("build_deck")
        game.step(120)
        check("the view reads 0 on the Chest", game.u16("gGraphics_sViewportX") == 0)
        game.press("right", hold=4, after=90)
        check("the view reads 320 on the Deck", game.u16("gGraphics_sViewportX") == 320)
        game.press("left", hold=4, after=90)
        check("and 0 again back on the Chest", game.u16("gGraphics_sViewportX") == 0)
        game.save("chest.state")
        game.load("chest.state")
        game.step(4)
        reference = game.hash()
        game.load("chest.state")
        game.step(4)
        check("from the same state the picture four frames on is the same", game.hash() == reference)
        game.load("chest.state")
        game.poke("gGraphics_sViewportX", (40).to_bytes(2, "little"))
        check("a poke by name reads back", game.u16("gGraphics_sViewportX") == 40)
        game.step(4)
        check("a poke by name moves the picture", game.hash() != reference)
        check("the poke is in the variable, not beside it",
              game.peek("gGraphics_sViewportX", 2, 0) == (40).to_bytes(2, "little")
              and game.peek("gGraphics_sViewportX", 1, 1) == b"\0")
        check("a read past the variable's end is an error",
              raises(ControlError, lambda: game.peek("gGraphics_sViewportX", 3)))
        check("a write past the variable's end is an error",
              raises(ControlError, lambda: game.poke("gGraphics_sViewportX", bytes(3))))
        check("a name nothing knows is a KeyError", raises(KeyError, lambda: game.peek("gNoSuchVariable", 1)))
        check("a name with an offset in it is refused, not sent", raises(ValueError, lambda: game.peek("gFoo+4", 1)))
        check("a function's static (a dot in its name) is a name, not an error: unknown, so a KeyError",
              raises(KeyError, lambda: game.peek("gNoFunction.gNoStatic", 1)))
        # A variable the native code defines, outside game_data: the live one
        # runs on after a poke, and the retail address it is listed at does not.
        game.poke("gRand_dwSeed", (1234).to_bytes(4, "little"))
        game.step(30)
        check("a variable native code defines (gRand_dwSeed) is the live one",
              game.u32("gRand_dwSeed") != 1234 and isinstance(game.locate("gRand_dwSeed"), str))
        # A module's variable (an ovl_* section): in the process as well.
        game.poke("gCampaignMap_Location", bytes([9]))
        check("a module's variable (gCampaignMap_Location) is in the process, not at its retail address",
              isinstance(game.locate("gCampaignMap_Location"), str) and game.u8("gCampaignMap_Location") == 9
              and game.peek(0x8016960C, 1) != bytes([9]))
        check("a pinned global is at the retail address the build gives",
              game.locate("D_8009B26C") == MODE)
        # Another module's (password's; which modules have ovl_* sections of
        # their own differs by target, the build's markers say).
        game.poke("gPassword_ModuleState", bytes([5]))
        check("another module's variable (gPassword_ModuleState) is in the process",
              isinstance(game.locate("gPassword_ModuleState"), str) and game.u8("gPassword_ModuleState") == 5)
        # A constant in read-only data: read, and a write is refused, not a crash.
        check("a constant (D_80180004, read-only data) reads", len(game.peek("D_80180004", 4)) == 4)
        check("a poke into it is refused as read-only, and the game goes on",
              raises(ControlError, lambda: game.poke("D_80180004", bytes(4))) and game.step(1) is not None)
        # A name the build neither defines nor pins (no code of the game's
        # refers to it): its only copy is guest RAM at the retail address.
        game.goto("duel", opponent=3, deck="1-40")
        game.step(200)
        game.press("circle")
        game.duel_ready()
        check("a retail name the build does not define reads from guest RAM (the life points, 8000)",
              isinstance(game.locate("gDuel_wPlayerLifePoint"), int) and game.u16("gDuel_wPlayerLifePoint") == 8000)
        check("a pinned global still reads by name, as at its retail address",
              game.u8("D_8009B26C") == game.peek(MODE, 1)[0] != 0)
    failed = [what for what, ok in checks if not ok]
    print(f"{len(checks) - len(failed)}/{len(checks)} checks hold")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
