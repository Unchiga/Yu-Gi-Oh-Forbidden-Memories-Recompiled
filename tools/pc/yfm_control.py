#!/usr/bin/env python3
"""Drive the native game through its control channel (MEMORIES_CONTROL,
notes/agent-control.md): launch it with folders of its own, step it, press
buttons, read its state as data, take pictures and save states, all in one
process.

    from yfm_control import Game
    with Game() as game:                       # boots; the game waits for it
        game.wait_until(lambda g: g.resident("main_menu"), 3000)   # the title
        game.press_until(lambda g: g.resident("password"), ["start", "cross"])  # NEW GAME
        game.shot("name-entry.png")
        print(game.state())

tools/pc/examples/control_first_duel.py goes on to the first duel.

The game runs only when told to (`step`); with MEMORIES_DETERMINISTIC=1,
which Game sets, the same calls give the same frames on every run. All the
game knowledge lives here; the C side only steps, presses and reads memory.

Run as a script for a quick look:  python3 tools/pc/yfm_control.py [--frames N]
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
from typing import Callable, Iterable
import weakref

sys.path.insert(0, str(Path(__file__).resolve().parent))
from smoke import launcher  # noqa: E402  (Wine for a Windows build off Windows)

ROOT = Path(__file__).resolve().parents[2]
EXECUTABLE = ROOT / "tmp/pc/game32" / ("memories-pc.exe" if sys.platform == "win32" else "memories-pc")
OUTPUT = ROOT / "tmp/pc/control"   # a run-XXXXXXXX folder each, as smoke.py does
# Variables every game this client starts gets, before a Game's own env:
# replay.py play/run --env sets them for a scripted replay's games.
EXTRA_ENV: dict[str, str] = {}
ADDRESSES = ROOT / "config/pc/guest_addresses.txt"

# PS1 digital pad bits, active high (src/pc/platform/platform.h, Platform_Pad).
BUTTONS = {"select": 0x0001, "l3": 0x0002, "r3": 0x0004, "start": 0x0008, "up": 0x0010, "right": 0x0020,
           "down": 0x0040, "left": 0x0080, "l2": 0x0100, "r2": 0x0200, "l1": 0x0400, "r1": 0x0800,
           "triangle": 0x1000, "circle": 0x2000, "cross": 0x4000, "square": 0x8000}

# The low five bits of the mode byte (src/game/main_modes.h, MainMode).
MODE = {"debug": 0, "animated_battle": 1, "campaign": 2, "duel": 3, "library": 4, "campaign_map": 5,
        "free_duel": 6, "build_deck": 7, "menu": 8, "name_entry": 9, "password": 10, "options": 11,
        "game_over": 12, "unused_developer": 13, "trade": 14, "credits": 15, "two_player_setup": 16}
MODE_NAMES = {value: name for name, value in MODE.items()}
# The duel scene's phases: gDuel_wSceneStateFlags & 0xF indexes
# gDuel_apfnSceneStateHandler (src/game/duel_scene_callbacks.c).
DUEL_PHASES = {"effect_preview": 0, "startup": 1, "draw": 2, "draw_resolution": 3, "hand": 4, "field": 5,
               "card_use": 6, "placement": 7, "position": 8, "battle": 9, "turn_switch": 10, "resume": 11,
               "result": 12, "rewards": 13, "exodia": 14}
# The duel's cursors, found by watching RAM while pressing: the hand's slot,
# and the column bytes of the field's and the attack target's grid cursors
# (duel_grid.h's DuelFieldCursor.col; .row follows).
HAND_CURSOR, FIELD_CURSOR, TARGET_CURSOR = 0x800E9F1E, 0x800E9F57, 0x800E9F73
CARD_OCCUPIED = 0x8000   # duel_card_layout.h, DUEL_CARD_FLAG_OCCUPIED
# Where each `jump` target lands (title_jump.h): the mode it runs.
JUMP_MODES = {"title": MODE["menu"], "debug": MODE["debug"], "duel": MODE["duel"], "free_duel": MODE["free_duel"],
              "build_deck": MODE["build_deck"], "library": MODE["library"], "password": MODE["password"],
              "map": MODE["campaign_map"], "credits": MODE["credits"], "options": MODE["options"]}

# Guest addresses by name. A variable a game unit (or the port's native code)
# defines, all of the small data (.sdata and .sbss: gGraphics_sViewportX)
# and a module's among them (gCampaignMap_Location), is not at its retail
# address in a build, only a pinned one is. The build says which is which in
# symbols/<id>.vars.txt, and peek and poke ask the game (Game.locate): a
# defined name is read in the process, a pinned one at the retail address the
# game gives, and a name the build has neither way is an error. This table,
# config/pc/guest_addresses.txt (read when present, a checkout) with these
# fallbacks from the headers named, is only for an executable too old to be
# asked, which warns.
FALLBACK = {
    "D_8009B26C": 0x8009B26C,             # main_mode_state.h: the active mode and its flags
    "gDuel_bOpponentID": 0x8009B361,      # the duellist the next duel is against
    "D_800E9FF0": 0x800E9FF0,             # duel_side_state.h: DuelSideState[2], 0x20 each
    "D_801A7AD8": 0x801A7AD8,             # duel_card.h: DuelCardRecord[30], 0x1C each
    "D_800907D8": 0x800907D8,             # duel_grid.h: the field grid, u8[2][20] record indices
    "gDuel_awPlayerDeck": 0x801D0200,     # save_data.h: the player's deck, u16[40] card ids
    "gLibrary_abCardChest": 0x801D0250,   # save_data.h: copies in the chest, u8 by card id - 1
    "gLibrary_dwStarchips": 0x801D07E0,   # save_data.h: SaveDataState.starchips, u32
    "gSaveData_aPlayerNameSjis": 0x801D060C,  # save_data.h: the player's name, Shift JIS
    "gDuel_wSceneStateFlags": 0x8009B23A, # duel_scene_state.h: the duel's phase in the low four bits
    "gDuel_wSelectedCardID": 0x8009B338,  # the card under the duel's cursor
    "D_8009B1D5": 0x8009B1D5,             # duel_side_state.h: whose turn, 0 the player
    "gDuel_aDeckCardRecords": 0x801A7E20, # duel_deck_card.h: both decks, 6 bytes a card
    "D_8009B26E": 0x8009B26E,             # a mode's step; 0x80 in a duel: the deck screen before it
}
# Overlay modules (tools/pc/build_game32.py, MODULES): the load address and
# the identifier word an image starts with while it is resident.
MODULES = {"main_menu": (0x80180000, 0x0F), "credits": (0x80180000, 0x10), "free_duel": (0x80168000, 0x13),
           "overworld": (0x80168000, 0x14), "password": (0x80168000, 0x15), "duel_effects": (0x80146000, 0x18)}
SIDE_SIZE, RECORD_SIZE, HAND_SIZE, DECK_SIZE, CARDS = 0x20, 0x1C, 5, 40, 722
# duel_field_layout.c: a side's card records are 15 in a row from 15 * side:
# five hand slots (D_800907CC), then the field (D_800907D8): five monster
# zones and five spell and trap zones.
HAND_RECORDS, MONSTER_RECORDS, SPELL_RECORDS = range(0, 5), range(5, 10), range(10, 15)


# What a variable's name can be: C's, a function's static as the compiler
# names it (Main_RunPasswordMenu.passwords: GCC may add a number to it on
# Linux, which changes from one build to the next) and the build's #2 for a
# repeated static. Not a '+' (peek's offset argument) nor a blank.
VARIABLE_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_.]*(#[0-9]+)?")


class ControlError(RuntimeError):
    """The game answered "err ..."."""


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def buttons(keys: str | Iterable[str] | int) -> int:
    """Pad bits from "cross", "up+cross", ["l1", "r1"] or the bits themselves."""
    if isinstance(keys, int):
        return keys
    if isinstance(keys, str):
        keys = keys.replace("+", " ").split()
    bits = 0
    for key in keys:
        if key.lower() not in BUTTONS:
            raise ValueError(f"no button {key!r}; buttons are {', '.join(BUTTONS)}")
        bits |= BUTTONS[key.lower()]
    return bits


def guest_addresses(path: Path = ADDRESSES) -> dict[str, int]:
    table = dict(FALLBACK)
    try:
        for line in path.read_text(encoding="utf-8").splitlines():
            parts = line.split()
            if len(parts) == 2 and not line.startswith("#"):
                table[parts[0]] = int(parts[1], 16)
    except OSError:
        pass
    return table


def _end(process: subprocess.Popen) -> None:
    if process.poll() is None:
        process.kill()
        process.wait()


class Game:
    """One game process and its control connection.

    executable  the game (default tmp/pc/game32/memories-pc[.exe])
    out         where its settings, user folder, log and pictures go
                (default a new tmp/pc/control/run-XXXXXXXX)
    settings    the player's settings by key ("mod.3d-monsters": 0, ...);
                everything else is the default
    env         more MEMORIES_* variables (MEMORIES_INPUT, MEMORIES_LOAD_STATE...)
    headless    no window (the default); a window shows each frame as it is stepped
    mods_dir    a mods folder of its own (MEMORIES_MODS_DIR); by default the
                mods shipped beside the executable, as smoke.py runs them
    """

    def __init__(self, executable: Path | str | None = None, out: Path | str | None = None,
                 settings: dict[str, object] | None = None, env: dict[str, str] | None = None,
                 headless: bool = True, mods_dir: Path | str | None = None, timeout: float = 60.0):
        self.executable = Path(executable or os.environ.get("YFM_EXECUTABLE") or EXECUTABLE).resolve()
        if not self.executable.is_file():
            raise FileNotFoundError(f"no game at {self.executable} (build it with tools/pc/build_game32.py)")
        if out is None:
            OUTPUT.mkdir(parents=True, exist_ok=True)
            out = tempfile.mkdtemp(prefix="run-", dir=OUTPUT)
        self.out = Path(out).resolve()
        self.out.mkdir(parents=True, exist_ok=True)
        self.symbols = guest_addresses()
        self._located: dict[str, int | str] = {}
        self.frame = self.vblank = 0
        self._buffer = b""
        settings_file = self.out / "settings.txt"
        settings_file.write_text("".join(f"{key}={value}\n" for key, value in (settings or {}).items()),
                                 encoding="utf-8")
        (self.out / "user").mkdir(exist_ok=True)
        environment = {key: value for key, value in os.environ.items() if not key.startswith("MEMORIES_")}
        if os.environ.get("MEMORIES_DISC"):
            environment["MEMORIES_DISC"] = os.environ["MEMORIES_DISC"]
        environment.update(MEMORIES_DETERMINISTIC="1", MEMORIES_NO_UPDATE_CHECK="1", MEMORIES_NO_GAMEPAD="1",
                           MEMORIES_SHOW_HUD="0", MEMORIES_WATCHDOG="0",
                           MEMORIES_CONTROL_WAIT="-1",   # this game is the client's: it waits as long as needed
                           MEMORIES_SETTINGS=str(settings_file),
                           MEMORIES_USER_DIR=str(self.out / "user"))
        if headless:
            environment.update(MEMORIES_HEADLESS="1", MEMORIES_NO_AUDIO="1")
        if mods_dir:
            environment["MEMORIES_MODS_DIR"] = str(Path(mods_dir).resolve())
        environment.update(EXTRA_ENV)
        environment.update(env or {})
        command, extra = launcher(self.executable)
        environment.update(extra)
        self.log_path = self.out / "game.log"
        for attempt in range(3):   # a port taken between free_port() and the game's bind
            self.port = free_port()
            environment["MEMORIES_CONTROL"] = str(self.port)
            self._log = self.log_path.open("ab")
            self.process = subprocess.Popen(command, cwd=ROOT, env=environment, stdout=self._log,
                                            stderr=subprocess.STDOUT)
            # Ended with this object or this interpreter, whatever happens:
            # a game whose client has gone runs on by itself.
            self._finalizer = weakref.finalize(self, _end, self.process)
            self.connection = self._connect(timeout)
            if self.connection:
                break
            self.close()
        else:
            raise RuntimeError(f"the game never opened its control channel; see {self.log_path}")
        self.info()

    def _connect(self, timeout: float) -> socket.socket | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and self.process.poll() is None:
            try:
                connection = socket.create_connection(("127.0.0.1", self.port), timeout=5)
            except OSError:
                time.sleep(0.05)
                continue
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            connection.settimeout(None)
            return connection
        return None

    # The protocol: one line out, one line back.

    def command(self, line: str) -> str:
        """Send one command; its reply without "ok". ControlError on "err"."""
        self.connection.sendall(line.encode() + b"\n")
        while b"\n" not in self._buffer:
            chunk = self.connection.recv(1 << 20)
            if not chunk:
                raise ControlError(f"the game closed the channel after {line!r}; see {self.log_path}")
            self._buffer += chunk
        reply, self._buffer = self._buffer.split(b"\n", 1)
        text = reply.decode()
        if text.startswith("err"):
            raise ControlError(f"{line.split()[0]}: {text[4:]}")
        return text[3:] if text.startswith("ok ") else ""

    def _stopped(self, reply: str) -> dict[str, object]:
        fields = reply.split()
        values = {}
        for key, value in zip(fields[0::2], fields[1::2]):
            values[key] = int(value, 16) if key == "build" else int(value) if value.isdigit() else value
        self.frame, self.vblank = values["frame"], values["vblank"]
        return values

    # Primitives.

    def step(self, vblanks: int = 1) -> dict[str, int]:
        """Run `vblanks` VBlanks and stop at the next safe point (the end of
        the next VSync(0)): {"frame", "vblank"} where it stopped."""
        return self._stopped(self.command(f"step {int(vblanks)}"))

    def info(self) -> dict[str, int]:
        """{"frame", "vblank", "mode" (the raw byte), "build"}."""
        return self._stopped(self.command("info"))

    def mode(self) -> int:
        """The active main mode (MODE), without its flags."""
        return self.u8("D_8009B26C") & 0x1F

    def resident(self, module: str) -> bool:
        """Whether an overlay module (MODULES) is loaded now."""
        address, identifier = MODULES[module]
        return self.u32(address) == identifier

    def player_name(self) -> bytes:
        """The name entered at NEW GAME, as the save holds it (Shift JIS)."""
        return self.peek("gSaveData_aPlayerNameSjis", 16).split(b"\0")[0]

    def pad(self, keys: str | Iterable[str] | int = 0, port: int = 1) -> None:
        """Hold these buttons from now on (0 releases), on pad 1 or 2."""
        self.command(f"pad {port} {buttons(keys):04x}")

    def press(self, keys: str | Iterable[str] | int, hold: int = 6, after: int = 6, port: int = 1) -> None:
        """Press and release: held for `hold` VBlanks, then `after` more with
        nothing, so the next press is a press of its own."""
        self.pad(keys, port)
        self.step(hold)
        self.pad(0, port)
        if after:
            self.step(after)

    def presses(self, *keys: str, hold: int = 6, after: int = 6) -> None:
        for key in keys:
            self.press(key, hold, after)

    def wait_until(self, predicate: Callable[["Game"], object], timeout: int = 600, every: int = 1,
                   what: str = "") -> int:
        """Step `every` VBlanks at a time until predicate(game) holds; at most
        `timeout` VBlanks. The VBlanks it took; TimeoutError past it."""
        start = self.vblank
        while not predicate(self):
            if self.vblank - start >= timeout:
                raise TimeoutError(f"{what or 'condition'} not reached in {timeout} VBlanks "
                                   f"(frame {self.frame}, mode {self.mode()})")
            self.step(every)
        return self.vblank - start

    def press_until(self, predicate: Callable[["Game"], object], keys: str | list[str], every: int = 30,
                    timeout: int = 6000, what: str = "") -> int:
        """Until predicate(game) holds, press the keys in turn (a name, or a
        list of them cycled), one press every `every` VBlanks: dialogue,
        menus and screens that wait for a button, without counting frames.
        The presses it took; TimeoutError past `timeout` VBlanks."""
        keys = [keys] if isinstance(keys, str) else list(keys)
        start, count = self.vblank, 0
        while not predicate(self):
            if self.vblank - start >= timeout:
                raise TimeoutError(f"{what or 'condition'} not reached in {timeout} VBlanks "
                                   f"(frame {self.frame}, mode {self.mode()})")
            self.press(keys[count % len(keys)], hold=6, after=max(every - 6, 0))
            count += 1
        return count

    def wait_mode(self, mode: int | str, timeout: int = 3000) -> int:
        """Until the main mode is `mode` (MODE's names or numbers)."""
        wanted = MODE[mode] if isinstance(mode, str) else mode
        return self.wait_until(lambda game: game.mode() == wanted, timeout,
                               what=f"mode {MODE_NAMES.get(wanted, wanted)}")

    # Where the game is going: Debug > Jump to's path (title_jump.h).

    def goto(self, target: str, opponent: int | None = None, deck: str | Iterable[int] | None = None,
             timeout: int = 6000) -> dict[str, int]:
        """Jump to a screen the way Debug > Jump to does (the title, then the
        game's own debug menu and its entry), and step until it runs:
        "debug", "duel" (against `opponent`, with `deck`: ids or ranges as
        MEMORIES_DEBUG_DECK takes them, repeated to forty), "free_duel",
        "build_deck", "library", "password", "map", "credits", "options",
        "title". A duel stops at the deck screen the game shows before every
        duel (leave it with circle; duel_ready() waits for the hand); its
        deck, when given, replaces the save's deck (as MEMORIES_DEBUG_DECK
        does), and without one the save's is used. The credits begin, as
        retail does, with the save prompt (the port's slot menu, which
        shot() does not see) and the SECRET NO. screen."""
        if deck is not None and not isinstance(deck, str):
            deck = ",".join(str(card) for card in deck)
        line = f"jump {target}"
        if opponent is not None or deck:
            line += f" {opponent or 0}" + (f" {deck}" if deck else "")
        jumps, start = self.info()["jumps"], self.vblank
        self.command(line)   # refused at once (ControlError) when the game cannot take it
        # Landed when the game counts it, even when the screen asked for is
        # the one already running (the game leaves it and comes back).
        self.wait_until(lambda g: g.info()["jumps"] > jumps, timeout,
                        what=f"the jump to {target} (see {self.log_path})")
        wanted = JUMP_MODES[target]
        if target == "title":
            # The title's own loop runs before Main_Loop starts a mode: the
            # byte has no flags (0 at boot, MAIN_MODE_MENU after a jump).
            self.wait_until(lambda g: g.u8("D_8009B26C") in (0, wanted) and g.resident("main_menu"), timeout,
                            what="the title")
        elif target == "debug":
            self.wait_until(lambda g: g.u8("D_8009B26C") == 0xC0, timeout, what="the debug menu")
        else:
            self.wait_until(lambda g: g.u8("D_8009B26C") & 0x9F == 0x80 | wanted, timeout, what=target)
        return {"vblanks": self.vblank - start}   # all the jump took

    def duel_ready(self, timeout: int = 6000, before_deal: Callable[["Game"], object] | None = None) -> int:
        """In a duel: past the deck screen before it (circle), until the
        player's five cards are dealt and the hand takes input. `before_deal`
        runs once both decks are shuffled and before a card is dealt (the
        one frame for arrange_deck and the like)."""
        start = self.vblank
        self.wait_until(lambda g: g.u8("D_8009B26E") == 0x80, timeout, what="the deck screen")
        self.press_until(lambda g: g.u8("D_8009B26E") != 0x80, "circle", every=60, timeout=timeout,
                         what="the duel field")
        # The duel's start (phase 1, no card dealt): the last duel's hand and
        # decks stay in memory until then, and its decks until the new ones
        # are written, on the last frame before the deal.
        self.wait_until(lambda g: g.phase() == DUEL_PHASES["startup"] and
                        all(slot < 0 for slot in g.duel()[0]["hand_slots"]), timeout, what="the duel's start")
        if before_deal:
            old = self.peek("gDuel_aDeckCardRecords", 80 * 6)
            self.wait_until(lambda g: g.peek("gDuel_aDeckCardRecords", 80 * 6) != old or
                            g.phase() != DUEL_PHASES["startup"], timeout, what="the shuffled decks")
            if self.phase() != DUEL_PHASES["startup"]:
                raise ControlError("the cards were dealt before the decks could be changed (the same decks as the "
                                   "duel before?)")
            before_deal(self)
        self.wait_until(lambda g: all(g.duel()[0]["hand"]), timeout, every=10, what="the hand")
        self.step(200)   # the cards come up from the deck
        return self.vblank - start

    def decks(self) -> list[list[int]]:
        """Both duel decks in draw order, card ids (gDuel_aDeckCardRecords,
        duel_deck_card.h: 6 bytes a card, the player's 40 then the
        opponent's 40)."""
        data = self.peek("gDuel_aDeckCardRecords", 80 * 6)
        ids = [struct.unpack_from("<h", data, 6 * i)[0] for i in range(80)]
        return [ids[:40], ids[40:]]

    def arrange_deck(self, side: int, cards: Iterable[int]) -> None:
        """Bring these cards to the top of a shuffled deck (the first five
        are the opening hand), the card's id and data block swapped with the
        card there: for duel_ready's before_deal."""
        data = bytearray(self.peek("gDuel_aDeckCardRecords", 80 * 6))
        base = 40 * side
        for position, card in enumerate(cards):
            at = next(i for i in range(base + position, base + 40)
                      if struct.unpack_from("<h", data, 6 * i)[0] == card)
            top, there = 6 * (base + position), 6 * at
            for offset in (0, 1, 3):   # the id and the card data block; deck_index and the flags stay
                data[top + offset], data[there + offset] = data[there + offset], data[top + offset]
        self.poke("gDuel_aDeckCardRecords", bytes(data))

    # The duel, as a player plays it (pad presses, and waits on what the game
    # holds; duel_scene_callbacks.c names the phases).

    def phase(self) -> int:
        """The duel scene's phase (DUEL_PHASES): gDuel_wSceneStateFlags & 0xF."""
        return self.u16("gDuel_wSceneStateFlags") & 0xF

    def turn(self) -> int:
        """Whose turn it is: 0 the player, 1 the opponent (D_8009B1D5)."""
        return self.u8("D_8009B1D5")

    def duel_over(self) -> bool:
        return self.mode() != MODE["duel"] or self.phase() in (DUEL_PHASES["result"], DUEL_PHASES["rewards"],
                                                               DUEL_PHASES["exodia"])

    def wait_turn(self, phases=("hand", "field"), timeout: int = 20000) -> int:
        """Until the player can act (their turn, in one of these phases) or
        the duel is over."""
        wanted = {DUEL_PHASES[name] for name in phases}
        return self.wait_until(lambda g: g.duel_over() or (g.turn() == 0 and g.phase() in wanted), timeout,
                               every=2, what="the player's turn")

    def _settle(self, timeout: int = 6000) -> None:
        """After an action: until the player can act again or it is no longer
        their turn (the card's effect, the battle and the camera are done).
        A question on the way (the guardian star of a ritual's monster,
        asked during the card's use) gets Cross, the first answer offered,
        once the game has stood in one phase for four seconds."""
        start = changed = self.vblank
        phase = self.phase()
        while not (self.duel_over() or self.turn() != 0 or phase in (DUEL_PHASES["hand"], DUEL_PHASES["field"])):
            if self.vblank - start >= timeout:
                raise TimeoutError(f"the action to end not reached in {timeout} VBlanks (frame {self.frame}, "
                                   f"phase {phase})")
            if phase == DUEL_PHASES["position"] or self.vblank - changed >= 240:
                self.press("cross", hold=4, after=36)
                changed = self.vblank
            else:
                self.step(2)
            if self.phase() != phase:
                phase, changed = self.phase(), self.vblank

    def hand_cursor(self) -> int:
        """The hand slot under the cursor (0-4)."""
        return self.u8(HAND_CURSOR)

    def _hand_to(self, slot: int) -> None:
        for _ in range(10):
            at = self.hand_cursor()
            if at == slot:
                return
            self.press("right" if at < slot else "left", hold=4, after=8)
        raise ControlError(f"the hand's cursor did not reach slot {slot}")

    def _place(self, face_up: bool, star: int) -> None:
        """Past the raised card: the side it is put down on (the card shows
        its back; Left or Right turns it), the zone (the game's first free
        one), then for a monster its guardian star."""
        if face_up:
            self.press("right", hold=4, after=12)
        self.press_until(lambda g: g.phase() != DUEL_PHASES["hand"] or g.duel_over(), "cross", every=40,
                         timeout=1200, what="the zone choice")
        self.press_until(lambda g: g.phase() != DUEL_PHASES["placement"] or g.duel_over(), "cross", every=40,
                         timeout=1200, what="the zone")
        if self.phase() == DUEL_PHASES["position"] and star:
            for _ in range(star):
                self.press("down", hold=4, after=12)
        if self.phase() == DUEL_PHASES["position"]:
            self.press_until(lambda g: g.phase() != DUEL_PHASES["position"] or g.duel_over(), "cross", every=40,
                             timeout=1200, what="the guardian star")
        self._settle()

    def play_card(self, slot: int, face_up: bool = False, star: int = 0) -> None:
        """Play the card in hand slot `slot` (0-4): face down unless asked,
        to the first free zone, with its first guardian star (or the
        `star`th after it). A magic card played face up is activated."""
        self.wait_turn(("hand",))
        self._hand_to(slot)
        self.press("cross", hold=4, after=30)
        self._place(face_up, star)

    def fuse(self, slots: Iterable[int], face_up: bool = False, star: int = 0) -> None:
        """Fuse hand cards in this order: each marked with Up, then Cross."""
        self.wait_turn(("hand",))
        for slot in slots:
            self._hand_to(slot)
            self.press("up", hold=4, after=16)
        self.press("cross", hold=4, after=30)
        self._place(face_up, star)

    def _cursor_to(self, column_address: int, column: int, what: str) -> None:
        for _ in range(12):
            at = self.u8(column_address)
            if at == column:
                return
            self.press("right" if at < column else "left", hold=4, after=16)
        raise ControlError(f"the {what} cursor did not reach column {column}")

    def attack(self, column: int = 0, target: int | None = None) -> None:
        """Attack with the player's monster in field() row 2, `column`, the
        opponent's monster in field() row 1, column `target` (as the player
        sees the field), or directly when `target` is None. The game asks
        for a card from the hand every turn first, so this comes after
        play_card or fuse. The cursors: the field's column at 0x800E9F57
        (its row at 0x800E9F58), the target's at 0x800E9F73; their
        records' col and row bytes are duel_grid.h's DuelFieldCursor."""
        self.wait_turn()
        if self.phase() != DUEL_PHASES["field"]:
            raise ControlError("attack from the field: play a card first (the game asks for one every turn)")
        if not self.field()[2][column]:
            raise ControlError(f"no monster of the player's in column {column}")
        for _ in range(4):   # to the monsters' row
            row = self.u8(FIELD_CURSOR + 1)
            if row == 2:
                break
            self.press("down" if row < 2 else "up", hold=4, after=16)
        self._cursor_to(FIELD_CURSOR, column, "field")
        self.press("cross", hold=4, after=40)   # the monster: its targets are offered
        if target is not None:
            if not self.field()[1][target]:
                raise ControlError(f"no monster of the opponent's in column {target}")
            self._cursor_to(TARGET_CURSOR, target, "target")
        self.press("cross", hold=4, after=30)
        self._settle()

    def end_turn(self, timeout: int = 30000) -> None:
        """Start ends the player's turn; back when it is theirs again (the
        opponent has played) or the duel is over."""
        self.wait_turn()
        self.press_until(lambda g: g.turn() != 0 or g.duel_over(), "start", every=60, timeout=1200,
                         what="the end of the turn")
        self.wait_turn(("hand",), timeout)

    def shot(self, path: Path | str) -> Path:
        """The presented picture, as PNG (PPM for a .ppm path); relative
        paths go in the run's folder. It is the game's frame (VRAM), not the
        window: the port's own overlays (the menu bar, notices, the save
        slot menu) are not in it, and hash() hashes the same VRAM."""
        path = self.out / path
        path.parent.mkdir(parents=True, exist_ok=True)
        self.command(f"shot {path}")
        return path

    def hash(self) -> int:
        """FNV-1a of VRAM, as MEMORIES_FRAME_HASHES writes it."""
        return int(self.command("hash"), 16)

    def locate(self, where: int | str) -> int | str:
        """What peek and poke send for `where`: a guest address as it is, and
        a name as "@name" when the game keeps the variable itself (what the
        build defines: the small data, a module's, native code's, which is
        not at its retail address), as the retail guest address the game gives
        when the build pins it, and as the retail address of the table
        (config/pc/guest_addresses.txt) when the build has the name in neither
        way (no code of the game's refers to it: guest RAM is its only copy).
        A name nobody has is a KeyError, one that is not a name a ValueError,
        and a game with no variable table (a build without
        symbols/<id>.vars.txt) a ControlError: neither falls back to a
        guess."""
        if not isinstance(where, str):
            return where
        if where not in self._located:
            if not VARIABLE_NAME.fullmatch(where):
                raise ValueError(f"{where!r} is not a variable name (letters, digits, _, . and # only; "
                                 "use an address, or peek's offset argument, for an offset)")
            try:
                self.command(f"peek @{where} 1")
                self._located[where] = f"@{where}"
            except ControlError as error:
                reason = str(error)
                pinned = re.search(r"is pinned at retail address ([0-9a-f]{8})", reason)
                if pinned:   # guest memory: the build's own table says where
                    self._located[where] = int(pinned.group(1), 16)
                elif "no variable table" in reason:
                    raise ControlError(f"{where}: {reason}; its retail address would be a guess") from None
                elif "usage: peek" in reason:   # an executable from before @NAME
                    print(f"yfm_control: WARNING: this game cannot look up {where} (older than @NAME): using its "
                          "retail address, which is wrong for small data and a module's variables", file=sys.stderr)
                    if where not in self.symbols:
                        raise KeyError(f"no variable named {where!r}: not in config/pc/guest_addresses.txt nor "
                                       "yfm_control.FALLBACK") from None
                    self._located[where] = self.symbols[where]
                elif f"no variable {where} in this build" in reason:
                    # The build's table is whole: a name in it is neither
                    # defined nor pinned when no code of the game's refers to
                    # it, and its only copy is the retail one in guest RAM.
                    if where not in self.symbols:
                        raise KeyError(f"no variable named {where!r}: this build neither defines nor pins it "
                                       "(symbols/<id>.vars.txt) and the retail table "
                                       "(config/pc/guest_addresses.txt) does not have it") from None
                    self._located[where] = self.symbols[where]
                else:
                    raise
        return self._located[where]

    def address(self, where: int | str) -> int:
        """The guest address of `where`; a ControlError for a variable the game
        keeps itself (peek and poke reach it, and it has no guest address)."""
        located = self.locate(where)
        if isinstance(located, str):
            raise ControlError(f"{where} is a variable of the game's own memory: it has no guest address; "
                               "use peek and poke")
        return located

    def _spell(self, located: int | str, at: int) -> str:
        return f"@{located[1:]}+{at:x}" if isinstance(located, str) else f"{located + at:08x}"

    def peek(self, where: int | str, length: int, offset: int = 0) -> bytes:
        """Guest memory (KSEG0, KSEG1 or physical; or a variable's name), from
        `offset` bytes on."""
        located, data = self.locate(where), b""
        while len(data) < length:
            count = min(16384, length - len(data))
            data += bytes.fromhex(self.command(f"peek {self._spell(located, offset + len(data))} {count}"))
        return data

    def poke(self, where: int | str, data: bytes, offset: int = 0) -> None:
        located = self.locate(where)
        for at in range(0, len(data), 16384):
            self.command(f"poke {self._spell(located, offset + at)} {data[at:at + 16384].hex()}")

    def u8(self, where: int | str, offset: int = 0) -> int:
        return self.peek(where, 1, offset)[0]

    def u16(self, where: int | str, offset: int = 0) -> int:
        return struct.unpack("<H", self.peek(where, 2, offset))[0]

    def u32(self, where: int | str, offset: int = 0) -> int:
        return struct.unpack("<I", self.peek(where, 4, offset))[0]

    def save(self, path: Path | str) -> Path:
        """A save state, here (relative paths go in the run's folder)."""
        path = self.out / path
        path.parent.mkdir(parents=True, exist_ok=True)
        self.command(f"save {path}")
        return path

    def load(self, path: Path | str) -> dict[str, int]:
        """Resume a save state; the game stops again one frame into it."""
        return self._stopped(self.command(f"load {self.out / path}"))

    # The game's state, as data.

    def state(self) -> dict[str, object]:
        """What the game holds now: the mode, the opponent, the save's deck,
        chest and starchips, and in a duel both sides (duel_side_state.h):
        LP, the cards in hand and on the field (by card id; None empty)."""
        mode = self.u8("D_8009B26C")
        deck = struct.unpack(f"<{DECK_SIZE}H", self.peek("gDuel_awPlayerDeck", 2 * DECK_SIZE))
        chest = self.peek("gLibrary_abCardChest", CARDS)
        result: dict[str, object] = {
            "frame": self.frame, "vblank": self.vblank, "mode": mode & 0x1F,
            "mode_name": MODE_NAMES.get(mode & 0x1F, "?"), "mode_flags": mode & 0xE0,
            "opponent": self.u8("gDuel_bOpponentID"), "starchips": self.u32("gLibrary_dwStarchips"),
            "deck": list(deck), "chest": {card + 1: count for card, count in enumerate(chest) if count},
        }
        if mode & 0x1F in (MODE["duel"], MODE["campaign"]):
            result["duel"] = self.duel()
            result["field"] = self.field()
            result["phase"] = self.phase()
            result["turn"] = self.turn()
        return result

    def field(self) -> list[list[int | None]]:
        """The field as the player sees it, four rows of five card ids (None
        empty): the opponent's spell and trap row, their monsters, the
        player's monsters, the player's spells and traps. Laid out by the
        game's own grid (D_800907D8[0], duel_grid.h: a record index a slot)."""
        grid = self.peek("D_800907D8", 20)
        records = self.peek("D_801A7AD8", 30 * RECORD_SIZE)
        def card(index: int) -> int | None:
            card_id, flags = struct.unpack_from("<h8xH", records, index * RECORD_SIZE + 0x0C)
            return card_id if card_id > 0 and flags & CARD_OCCUPIED else None
        return [[card(grid[row * 5 + column]) for column in range(5)] for row in range(4)]

    def duel(self) -> list[dict[str, object]]:
        """Both sides of the duel: 0 the player, 1 the opponent. A hand slot
        whose byte at +0x1A is negative is empty (its card record keeps the
        card it held).
        `flags` is the record's word (duel_card_layout.h: 0x8000 occupied,
        0x4000 used this turn, 0x1000 face down, 0x0800 defense position);
        a zone or slot without 0x8000 is empty."""
        sides = self.peek("D_800E9FF0", 2 * SIDE_SIZE)
        records = self.peek("D_801A7AD8", 30 * RECORD_SIZE)

        def card(index: int) -> dict[str, int] | None:
            at = index * RECORD_SIZE
            card_id, attack, defense, _, _, flags = struct.unpack_from("<hhhhhH", records, at + 0x0C)
            if card_id <= 0 or not flags & CARD_OCCUPIED:
                return None
            return {"id": card_id, "attack": attack, "defense": defense, "flags": flags,
                    "face_down": bool(flags & 0x1000), "defense_position": bool(flags & 0x0800),
                    "used_this_turn": bool(flags & 0x4000)}

        result = []
        for side in range(2):
            at = side * SIDE_SIZE
            displayed, life, maximum = struct.unpack_from("<hHh", sides, at + 0x12)
            base = 15 * side
            slots = list(struct.unpack_from("<5b", sides, at + 0x1A))
            result.append({
                "lp": life, "displayed_lp": displayed, "max_lp": maximum,
                "deck_cursor": struct.unpack_from("<b", sides, at + 0x18)[0],
                "hand_slots": slots,
                "hand": [card(base + i) if slots[i] >= 0 else None for i in HAND_RECORDS],
                "monsters": [card(base + i) for i in MONSTER_RECORDS],
                "spells": [card(base + i) for i in SPELL_RECORDS],
            })
        return result

    # The end.

    def quit(self) -> None:
        """Ask the game to end, and wait for it."""
        try:
            self.command("quit")
        except (ControlError, OSError):
            pass
        self.close()

    def close(self) -> None:
        """Close the connection; a game still running after a few seconds is
        ended (this process's own child only)."""
        connection = getattr(self, "connection", None)
        if connection:
            connection.close()
            self.connection = None
        if getattr(self, "process", None) and self.process.poll() is None:
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if getattr(self, "_log", None):
            self._log.close()
            self._log = None

    def __enter__(self) -> "Game":
        return self

    def __exit__(self, *exception) -> None:
        if getattr(self, "connection", None) and exception[0] is None:
            self.quit()
        else:
            if getattr(self, "process", None) and self.process.poll() is None:
                self.process.kill()
            self.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--frames", type=int, default=1200, help="VBlanks to run before looking")
    arguments = parser.parse_args()
    with Game(arguments.executable) as game:
        game.step(arguments.frames)
        print(json.dumps({key: value for key, value in game.state().items() if key != "chest"}, indent=1))
        print("picture:", game.shot("look.png"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
