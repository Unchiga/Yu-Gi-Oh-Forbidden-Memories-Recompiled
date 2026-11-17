#!/usr/bin/env python3
"""Monster effects "for each" (notes/more-cards.md, "Monster effects"):
boosts and LP made once per face-up monster counted, played in the built
game (needs the player's disc).

The mod is written for the run with the FM Editor's model and manifest
(in --out, never in the repository). It gives disc monsters effects:

  Baby Dragon       while face up, +300/+300 for each face-up Dragon on its
                    owner's field (itself one of them)
  Hitotsu-me Giant  while face up, +200 ATK for each face-up Dragon on the
                    whole field
  Mystical Elf      while face up, +400/+400 for each face-up Dragon on the
                    opponent's field
  Mushroom Man      while face up, +100/+100 for each face-up Light monster
                    on the field
  Feral Imp         on summon, its owner gains 100 LP for each face-up
                    Dragon on the field

and fixes Simon Muran's deck to Red Medicine, so that he plays nothing on
the field unless a scenario deals him monsters. What each scenario checks
(the ATK and DEF the game shows -- the field's info panel for the card
under the cursor, Duel_CalcCardStats -- against the cards' own stats and
modifiers and the count):

  count      0, 1 and 3 Dragons: the Giant before any, Baby Dragon counting
             itself, a face-down Winged Dragon not counted, then counted
             once its attack flips it; Mushroom Man counting Blue-eyes (Light)
  opponent   the CPU's Baby Dragons (put down face up for their effect) are
             counted by the Elf and the Giant, not by the player's Baby
             Dragon; the CPU's own count; one destroyed in battle takes its
             count away, the battle's damage matching the shown ATK
  fusion     a fusion's Dragon (B. Dragon Jungle King) counted as it lands;
             Dragon Treasure on Baby Dragon: its +500 and the count add up
  heal       Feral Imp's summon heals 100 per Dragon on the field
  duel       a whole duel against a CPU with Dragons, played turn by turn to
             its end: after each turn every face-up monster's shown stats
             match, the records' modifiers stay as they were, and each
             battle's LP change matches the stats shown before it

Each pass writes pictures to --out/<pass>/shots (look at them). With
--interpreter the scenarios run again with the duel effects interpreted
(MEMORIES_DUEL_EFFECTS=interpreter, on the reference build: the game with the
MIPS interpreter, tools/pc/reference_build.py, built when it is missing, or
--reference-executable).

    python3 tests/pc/monster_for_each_runtime.py [--executable PATH] [--out DIR]
        [--interpreter [--reference-executable PATH]] [scenarios...]
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/pc"))
from yfm_control import Game, DUEL_PHASES, EXECUTABLE, FIELD_CURSOR  # noqa: E402
from reference_build import reference_executable  # noqa: E402

MOD = "for-each-test"
SIMON = 1
BLUE_EYES, ELF, GIANT, BABY, IMP, WINGED, MUSHROOM = 1, 2, 3, 4, 6, 7, 8
MEDICINE = 339              # Red Medicine: the CPU's filler, and the player's
TREASURE = 315              # Dragon Treasure: a Dragon's equip, +500/+500
JUNGLE_KING = 571           # B. Dragon Jungle King: Winged Dragon #1 + Mushroom Man
DRAGON, LIGHT = 0, 0        # gamedata TYPE_NAMES, ATTRIBUTE_NAMES
EFFECTS = {
    BABY: [{"when": "face_up", "do": "boost", "target": "self", "attack": 300, "defense": 300,
            "for_each": {"whose": "own", "type": "Dragon"}}],
    GIANT: [{"when": "face_up", "do": "boost", "target": "self", "attack": 200,
             "for_each": {"whose": "all", "type": "Dragon"}}],
    ELF: [{"when": "face_up", "do": "boost", "target": "self", "attack": 400, "defense": 400,
           "for_each": {"whose": "opponent", "type": "Dragon"}}],
    MUSHROOM: [{"when": "face_up", "do": "boost", "target": "self", "attack": 100, "defense": 100,
                "for_each": {"whose": "all", "attribute": "Light"}}],
    IMP: [{"when": "summon", "do": "heal", "amount": 100, "for_each": {"whose": "all", "type": "Dragon"}}],
}
RECORD_SIZE = 0x1C
INFO_STATS = 0x801D5608     # D_801D5608[0].card_stats: the info panel's ATK and DEF (s32 each)


def write_mod(mods: Path) -> dict:
    """The test's mod, made as the FM Editor makes one; its manifest."""
    from fm_editor import disc, fixed_decks, gamedata, manifest, monster_effects as fx
    from fm_editor.model import Project
    folder = mods / MOD
    shutil.rmtree(folder, ignore_errors=True)
    retail = gamedata.load_game(disc.find_game([ROOT / "game"]))
    project = Project(retail)
    project.info.id = MOD
    project.info.name = "For each test"
    for card, effects in EFFECTS.items():
        assert all(fx.normalize(e) == e for e in effects), effects
        project.set_monster_effects(card, effects)
    messages = []
    fixed_decks.read_entry(project, gamedata.DUELIST_NAMES[SIMON], {"fixed": True, str(MEDICINE): 40}, messages)
    assert not messages, messages
    manifest.save_mod(project, folder)
    reopened, messages = manifest.open_mod(retail, folder)
    assert not messages, messages
    assert manifest.build(reopened) == manifest.build(project), "save/reopen changed the mod"
    assert {c: reopened.monster_effects_of(c)[0] for c in EFFECTS} == EFFECTS
    return json.loads((folder / "mod.json").read_text(encoding="utf-8"))


def matchup(a: int, b: int) -> int:
    """Duel_CalcGuardianStarMatchup without a mod's stars: +500, -500 or 0."""
    if a - 7 >= 0:
        a, b, n = a - 7, b - 7, 4
        if b < 0:
            return 0
    else:
        a, b, n = a - 1, b - 1, 6
        if b >= n:
            return 0
    a += 1
    if a >= n:
        a = 0
    if a == b:
        return 500
    a -= 2
    if a < 0:
        a += n
    return -500 if a == b else 0


class Run:
    """One game, the scenarios' checks and pictures."""

    def __init__(self, game: Game, log: Path, label: str, retail):
        self.game, self.log, self.label, self.retail = game, log, label, retail
        self.failures: list[str] = []
        self.passed = 0
        self.seen: set[str] = set()     # what the duel's turns have covered

    def check(self, what: str, ok: bool, detail: object = "") -> bool:
        if ok:
            self.passed += 1
        else:
            self.failures.append(f"{what}: {detail}")
        print(f"  {'ok' if ok else 'FAILED'}: {what}" + ("" if ok else f" ({detail})"), flush=True)
        return ok

    def shot(self, name: str) -> Path:
        return self.game.shot(f"shots/{self.label}-{name}.png")

    # The field, as data.

    def records(self, lifted: dict[int, int] | None = None) -> list[dict | None]:
        """The 30 duel records (duel_card.h DuelCardRecord), None empty;
        `lifted` gives records their flags while a battle holds them."""
        data = self.game.peek("D_801A7AD8", 30 * RECORD_SIZE)
        out = []
        for index in range(30):
            card, attack, defense, modifier, terrain, flags, _, _, defense_modifier = struct.unpack_from(
                "<hhhhhHBBh", data, index * RECORD_SIZE + 0x0C)
            flags = (lifted or {}).get(index, flags)
            out.append({"record": index, "side": index // 15, "id": card, "attack": attack, "defense": defense,
                        "modifier": modifier, "terrain": terrain, "defense_modifier": defense_modifier,
                        "flags": flags, "up": not flags & 0x1000}
                       if card > 0 and flags & 0x8000 else None)
        return out

    def monsters(self, side: int = 0) -> list[dict]:
        return [r for r in self.records()[15 * side + 5:15 * side + 10] if r]

    def counted(self, effect: dict, source: dict, records: list) -> int:
        """The face-up monsters `effect` of `source` counts (the spec, from the field)."""
        each = effect["for_each"]
        n = 0
        for r in records:
            if not r or r["record"] % 15 < 5 or r["record"] % 15 >= 10 or not r["up"]:
                continue
            if each["whose"] == "own" and r["side"] != source["side"]:
                continue
            if each["whose"] == "opponent" and r["side"] == source["side"]:
                continue
            card = self.retail.cards[r["id"]]
            if "type" in each and card.type != DRAGON:
                continue
            if "attribute" in each and card.attribute != LIGHT:
                continue
            n += 1
        return n

    def expected(self, record: dict, records: list | None = None) -> tuple[int, int]:
        """The ATK and DEF the game must show (Duel_CalcCardStats): the
        card's, its modifiers, and its face_up boosts times their count."""
        records = records or self.records()
        attack = record["attack"] + record["modifier"] + record["terrain"]
        defense = record["defense"] + record["modifier"] + record["terrain"] + record["defense_modifier"]
        if record["up"]:
            for effect in EFFECTS.get(record["id"], []):
                if effect["when"] == "face_up":
                    n = self.counted(effect, record, records)
                    attack += effect.get("attack", 0) * n
                    defense += effect.get("defense", 0) * n
        return max(0, min(9999, attack)), max(0, min(9999, defense))

    def where(self, record: int) -> tuple[int, int]:
        """The field cursor's row and column for a record, as the player sees it."""
        grid = self.game.peek("D_800907D8", 20)
        at = grid.index(record)
        return at // 5, at % 5

    def shown(self, record: int) -> tuple[int, int]:
        """The ATK and DEF the field's info panel shows for the card at
        `record`, the cursor put on it (the player's field phase)."""
        g = self.game
        row, column = self.where(record)
        # The panel follows the cursor's moves: one already there may show
        # the card the zone held before, so it steps off and back.
        if (g.u8(FIELD_CURSOR + 1), g.u8(FIELD_CURSOR)) == (row, column):
            g.press("left" if column else "right", hold=4, after=10)
        for _ in range(16):
            at = (g.u8(FIELD_CURSOR + 1), g.u8(FIELD_CURSOR))
            if at == (row, column):
                break
            key = "down" if at[0] < row else "up" if at[0] > row else "right" if at[1] < column else "left"
            g.press(key, hold=4, after=10)
        g.step(6)
        card = self.records()[record]["id"]
        assert (g.u8(FIELD_CURSOR + 1), g.u8(FIELD_CURSOR)) == (row, column), "the cursor did not reach the card"
        assert g.u16("gDuel_wSelectedCardID") == card, ("the panel shows another card", g.u16("gDuel_wSelectedCardID"))
        return struct.unpack("<ii", g.peek(INFO_STATS, 8))

    def verify(self, what: str, equipped: dict[int, int] | None = None) -> dict[int, tuple[int, int]]:
        """Every face-up monster's shown ATK and DEF against the spec, and no
        modifier anywhere but an equip's (`equipped`, by card): what was
        shown, by record."""
        records = self.records()
        shown, wrong = {}, []
        for r in records:
            if not r or r["record"] % 15 < 5 or r["record"] % 15 >= 10 or not r["up"]:
                continue
            shown[r["record"]] = self.shown(r["record"])
            if shown[r["record"]] != self.expected(r, records):
                wrong.append((r["id"], r["record"], shown[r["record"]], self.expected(r, records)))
        modified = [(r["id"], r["modifier"], r["defense_modifier"]) for r in records
                    if r and (r["modifier"] != (equipped or {}).get(r["id"], 0) or r["defense_modifier"])]
        self.check(f"{what}: shown stats as counted ({len(shown)} monsters)", not wrong, wrong)
        self.check(f"{what}: no record's modifiers touched", not modified, modified)
        return shown

    def show(self, card: int, side: int = 0, nth: int = 0) -> tuple[int, int]:
        """The shown stats of the `nth` `card` on `side`'s field."""
        record = [r for r in self.monsters(side) if r["id"] == card][nth]
        return self.shown(record["record"])

    # The duel.

    def duel(self, top: list[int], opponent: list[int] | None = None, opponent_top: list[int] | None = None,
             life: int | None = None) -> None:
        g = self.game

        def deal(game):
            game.arrange_deck(0, top)
            if opponent:
                base = game.address("gDuel_aDeckCardRecords") + 6 * 40
                records = [game.peek(base + 6 * i, 6) for i in range(40)]
                game.poke(base, b"".join(struct.pack("<h", opponent[i % len(opponent)]) + r[2:]
                                         for i, r in enumerate(records)))
            if opponent_top:
                game.arrange_deck(1, opponent_top)
        deck = top + [MEDICINE] * (40 - len(top))
        g.goto("duel", opponent=SIMON, deck=deck)
        g.duel_ready(before_deal=deal)
        if life:
            for side in range(2):
                g.poke(g.address("D_800E9FF0") + 0x20 * side + 0x12, struct.pack("<hH", life, life))

    def hand(self, side: int = 0) -> list[int | None]:
        return [c and c["id"] for c in self.game.duel()[side]["hand"]]

    def lp(self) -> list[int]:
        return [d["lp"] for d in self.game.duel()]

    def play(self, card: int, face_up: bool = True) -> None:
        self.game.play_card(self.hand().index(card), face_up=face_up)
        self.game.step(30)

    def column(self, card: int, side: int = 0) -> int:
        """The field column of the player's (or the CPU's) `card`, as attack() takes it."""
        return self.game.field()[2 if side == 0 else 1].index(card)

    def fighting(self, records: list, a: dict, d: dict | None) -> tuple:
        """The two monsters' ATK and DEF in their battle: both face up from
        its start (MonsterEffects_Battle), so counted and boosted."""
        records = [dict(r, up=True) if r and r["record"] in (a["record"], d and d["record"]) else r
                   for r in records]
        return (self.expected(records[a["record"]], records),
                self.expected(records[d["record"]], records) if d else None)

    def predict(self, a: dict, d: dict | None, a_shown, d_shown, lp: list[int]):
        """A battle as the game works it out from the stats it shows
        (duel_scene_battle.c, the guardian stars' 500): the LP after, whether
        the target goes, and "attacker" when the attacker goes too."""
        def star(r):
            card = self.retail.cards[r["id"]]
            return card.star2 if r["flags"] & 0x200 else card.star1
        loser = 1 - a["side"]
        if d is None:
            return [x - a_shown[0] if side == loser else x for side, x in enumerate(lp)], True, None
        # Only the star with the advantage gets its 500 (duel_scene_battle.c,
        # case 4: the modifier climbs on that card alone).
        power = min(9999, a_shown[0] + max(0, matchup(star(a), star(d))))
        guard = d_shown[1] if d["flags"] & 0x800 else d_shown[0]
        guard = min(9999, guard + max(0, matchup(star(d), star(a))))
        lp = list(lp)
        if d["flags"] & 0x800:
            lp[a["side"]] -= max(0, guard - power)
            return lp, power > guard, None
        if power > guard:
            lp[loser] -= power - guard
            return lp, True, None
        if power < guard:
            lp[a["side"]] -= guard - power
            return lp, False, "attacker"
        return lp, True, "attacker"

    def battle(self, attacker: int, target: int | None) -> dict:
        """The player's monster at record `attacker` attacks the CPU's at
        record `target` (None: directly). The shown stats of both first,
        the outcome checked against them as the game's battle works it
        out (duel_battle_stats.c: the guardian stars' 500)."""
        g = self.game
        records = self.records()
        a = records[attacker]
        d = records[target] if target is not None else None
        a_shown = self.shown(attacker)
        d_shown = self.shown(target) if d and d["up"] else None
        fight = self.fighting(records, a, d)
        self.check(f"battle {a['id']} vs {d and d['id']}: the shown stats are the counted ones",
                   a_shown == fight[0] or not a["up"], (a_shown, fight))
        if d_shown is not None:
            self.check(f"battle {a['id']} vs {d['id']}: the target's shown stats are the counted ones",
                       d_shown == fight[1] or not d["up"], (d_shown, fight))
        # A face-down card is turned up by the battle: counted from then on.
        a_shown, d_shown = fight
        lp = self.lp()
        g.attack(self.where(attacker)[1], self.where(target)[1] if d else None)
        g.step(60)
        after, now = self.lp(), self.records()

        want = self.predict(a, d, a_shown, d_shown, lp)
        outcome = {"attacker": (a["id"], a_shown), "target": d and (d["id"], d_shown), "lp": (lp, after),
                   "target_gone": d is not None and (not now[target] or now[target]["id"] != d["id"]),
                   "attacker_gone": not now[attacker] or now[attacker]["id"] != a["id"]}
        lp_ok = after == [max(0, x) for x in want[0]]
        gone_ok = d is None or outcome["target_gone"] == want[1]
        if d is not None and want[2] == "attacker":
            gone_ok = gone_ok and outcome["attacker_gone"]
        self.check(f"battle {a['id']} ({a_shown[0]}) vs {d and d['id']} ({d_shown}): LP and losses as the shown "
                   f"stats say", lp_ok and gone_ok, (outcome, want))
        return outcome

    def end_turn(self) -> None:
        self.game.end_turn()
        self.game.step(20)

    def cpu_turn(self) -> int:
        """End the player's turn and watch the CPU's: each battle it starts
        (DUEL_PHASES battle, its two cards' records in D_800E9EF0's objects
        at +0x6A) is worked out from the counted stats at its start and
        checked against the LP after it. The battles seen."""
        g = self.game
        g.wait_turn()
        g.press_until(lambda g: g.turn() != 0 or g.duel_over(), "start", every=60, timeout=1200,
                      what="the end of the turn")
        start, pending, seen = g.vblank, None, 0
        objects = g.address("D_800E9EF0")
        while not (g.duel_over() or (g.turn() == 0 and g.phase() == DUEL_PHASES["hand"])):
            if g.vblank - start > 30000:
                raise TimeoutError("the CPU's turn did not end")
            if g.phase() == DUEL_PHASES["battle"] and pending is None:
                # The two are lifted off the field for the battle: their
                # flags wait in D_8009B178 (the defender's, the attacker's).
                attacker, defender = g.u32(objects), g.u32(objects + 4)
                saved = struct.unpack("<HH", g.peek("D_8009B178", 4))
                lifted = {g.u8(attacker + 0x6A): saved[0]} if attacker else {}
                if defender:
                    lifted[g.u8(defender + 0x6A)] = saved[1]
                records = self.records(lifted)
                # Their stat modifiers wait in D_8009B170 (the stars' 500 climbs on the card).
                for (record, _), modifier in zip(lifted.items(), struct.unpack("<hh", g.peek("D_8009B170", 4))):
                    if records[record]:
                        records[record]["modifier"] = modifier
                a = records[g.u8(attacker + 0x6A)] if attacker else None
                d = records[g.u8(defender + 0x6A)] if defender else None
                if a:
                    pending = (a, d, self.fighting(records, a, d), self.lp())
            elif g.phase() != DUEL_PHASES["battle"] and pending:
                a, d, fight, lp = pending
                want = self.predict(a, d, fight[0], fight[1], lp)
                self.check(f"CPU battle {a['id']} ({fight[0][0]}) vs {d and d['id']} ({fight[1]}): LP as the "
                           f"counted stats say", self.lp() == [max(0, x) for x in want[0]], (lp, self.lp(), want))
                pending, seen = None, seen + 1
            g.step(2)
        return seen


# --- the scenarios ----------------------------------------------------------

def count(run: Run) -> None:
    """0, 1 and 3 Dragons on the player's field; face down not counted."""
    run.duel([GIANT, BABY, WINGED, BLUE_EYES, MUSHROOM])
    run.play(GIANT)
    run.check("count: no Dragon, the Giant as printed (1200/1000)", run.show(GIANT) == (1200, 1000),
              run.show(GIANT))
    run.shot("count-0-dragons")
    run.end_turn()
    run.play(BABY)
    run.check("count: Baby Dragon counts itself (1200+300 / 700+300)", run.show(BABY) == (1500, 1000),
              run.show(BABY))
    run.check("count: one Dragon, the Giant 1400", run.show(GIANT) == (1400, 1000), run.show(GIANT))
    run.shot("count-1-dragon")
    run.verify("count, one Dragon")
    run.end_turn()
    run.play(WINGED, face_up=False)
    run.check("count: a face-down Winged Dragon is not counted (Baby 1500/1000)", run.show(BABY) == (1500, 1000),
              run.show(BABY))
    run.check("count: nor by the Giant (1400)", run.show(GIANT) == (1400, 1000), run.show(GIANT))
    run.shot("count-face-down")
    run.end_turn()
    run.play(BLUE_EYES)
    run.check("count: Blue-eyes summoned: Baby 1800/1300", run.show(BABY) == (1800, 1300), run.show(BABY))
    run.check("count: Blue-eyes summoned: Giant 1600", run.show(GIANT) == (1600, 1000), run.show(GIANT))
    run.shot("count-2-dragons")
    run.end_turn()
    run.play(MUSHROOM)
    run.check("count: Mushroom Man counts the one Light monster (Blue-eyes): 900/700",
              run.show(MUSHROOM) == (900, 700), run.show(MUSHROOM))
    winged = next(r for r in run.monsters() if r["id"] == WINGED)
    run.check("count: Winged Dragon still face down", not winged["up"], winged)
    run.battle(winged["record"], None)
    winged = next(r for r in run.monsters() if r["id"] == WINGED)
    run.check("count: its attack turned Winged Dragon face up", winged["up"], winged)
    run.check("count: three Dragons: Baby 2100/1600", run.show(BABY) == (2100, 1600), run.show(BABY))
    run.check("count: three Dragons: Giant 1800", run.show(GIANT) == (1800, 1000), run.show(GIANT))
    run.show(BABY)
    run.shot("count-3-dragons")
    run.verify("count, three Dragons")


def opponent(run: Run) -> None:
    """The CPU's Dragons: counted by the Elf ("opponent") and the Giant
    ("all"), not by the player's Baby Dragon ("own"); the CPU's Baby
    Dragons count their own side. One destroyed in battle."""
    run.duel([BLUE_EYES, ELF, GIANT, BABY, MUSHROOM], opponent=[BABY, BABY] + [MEDICINE] * 38,
             opponent_top=[BABY, BABY, MEDICINE, MEDICINE, MEDICINE])
    run.play(BLUE_EYES)
    run.end_turn()
    cpu = [r for r in run.monsters(1) if r["id"] == BABY]
    run.check("opponent: the CPU put its Baby Dragon down face up", len(cpu) == 1 and cpu[0]["up"], run.monsters(1))
    run.play(ELF)
    run.check("opponent: the Elf counts the CPU's Dragon: 1200/2400", run.show(ELF) == (1200, 2400), run.show(ELF))
    run.check("opponent: the CPU's Baby Dragon counts its own side only (1500/1000)",
              run.show(BABY, 1) == (1500, 1000), run.show(BABY, 1))
    run.show(ELF)
    run.shot("opponent-elf")
    run.verify("opponent, the Elf")
    # Blue-eyes destroys the CPU's Dragon: the Elf's count goes with it.
    blue = next(r for r in run.monsters() if r["id"] == BLUE_EYES)
    outcome = run.battle(blue["record"], cpu[0]["record"])
    run.check("opponent: Blue-eyes destroyed the CPU's Baby Dragon", outcome["target_gone"], outcome)
    run.check("opponent: no Dragon of the CPU's left: the Elf 800/2000", run.show(ELF) == (800, 2000),
              run.show(ELF))
    run.shot("opponent-after-battle")
    run.verify("opponent, after the battle")
    run.end_turn()
    # The CPU's second Baby Dragon; the Giant counts both sides.
    run.play(GIANT)
    dragons = sum(1 for side in range(2) for r in run.monsters(side) if r["up"] and r["id"] in (BABY, BLUE_EYES))
    them = sum(1 for r in run.monsters(1) if r["up"] and r["id"] == BABY)
    run.check(f"opponent: the CPU has a Baby Dragon again ({them})", them == 1, run.monsters(1))
    run.check(f"opponent: the Giant counts both sides' {dragons} Dragons: {1200 + 200 * dragons}",
              run.show(GIANT) == (1200 + 200 * dragons, 1000), (dragons, run.show(GIANT)))
    run.show(GIANT)
    run.shot("opponent-giant")
    run.verify("opponent, the Giant")
    run.end_turn()
    run.play(BABY)
    own = sum(1 for r in run.monsters() if r["up"] and r["id"] in (BABY, BLUE_EYES))
    run.check(f"opponent: the player's Baby Dragon counts only the player's {own} Dragons",
              run.show(BABY) == (1200 + 300 * own, 700 + 300 * own), (own, run.show(BABY)))
    run.show(BABY)
    run.shot("opponent-both-babies")
    run.verify("opponent, both sides' Baby Dragons")


def fusion(run: Run) -> None:
    """A fusion's Dragon is counted as it lands; an equip's modifier and
    the count add up."""
    run.duel([GIANT, WINGED, MUSHROOM, BABY, TREASURE])
    run.play(GIANT)
    run.check("fusion: no Dragon, the Giant 1200", run.show(GIANT) == (1200, 1000), run.show(GIANT))
    run.end_turn()
    hand = run.hand()
    run.game.fuse([hand.index(WINGED), hand.index(MUSHROOM)], face_up=True)
    run.game.step(30)
    run.check("fusion: Winged Dragon #1 and Mushroom Man made B. Dragon Jungle King",
              [r["id"] for r in run.monsters()] == [GIANT, JUNGLE_KING], run.monsters())
    run.check("fusion: the Giant counts the fusion's Dragon: 1400", run.show(GIANT) == (1400, 1000), run.show(GIANT))
    run.show(GIANT)
    run.shot("fusion")
    run.verify("fusion")
    run.end_turn()
    hand = run.hand()
    run.game.fuse([hand.index(BABY), hand.index(TREASURE)], face_up=True)
    run.game.step(30)
    baby = next((r for r in run.monsters() if r["id"] == BABY), None)
    run.check("fusion: Dragon Treasure equipped Baby Dragon (+500 modifier)", baby and baby["modifier"] == 500,
              run.monsters())
    # The zone the game offered may have been the Jungle King's: whatever
    # is left is counted.
    dragons = sum(1 for r in run.monsters() if r["id"] in (BABY, JUNGLE_KING))
    run.check(f"fusion: equipped and counting {dragons} Dragon(s): 1200+500+{300 * dragons} / "
              f"700+500+{300 * dragons}", run.show(BABY) == (1700 + 300 * dragons, 1200 + 300 * dragons),
              (dragons, run.show(BABY), run.monsters()))
    run.check(f"fusion: the Giant {1200 + 200 * dragons}", run.show(GIANT) == (1200 + 200 * dragons, 1000),
              run.show(GIANT))
    run.show(BABY)
    run.shot("equip")
    run.verify("equip", {BABY: 500})


def heal(run: Run) -> None:
    """Feral Imp's summon: 100 LP for each face-up Dragon on the field."""
    run.duel([BLUE_EYES, BABY, IMP, ELF, GIANT], life=5000)
    run.play(BLUE_EYES)
    run.end_turn()
    run.play(BABY)
    run.end_turn()
    before = run.lp()[0]
    run.play(IMP)
    run.game.step(60)
    run.shot("heal")
    run.check("heal: Feral Imp healed 200 (two Dragons)", run.lp()[0] == before + 200, (before, run.lp()))
    run.end_turn()
    before = run.lp()[0]
    run.play(ELF)
    run.check("heal: an Elf's summon heals nothing", run.lp()[0] == before, (before, run.lp()))


def duel(run: Run) -> None:
    """A whole duel: each of the player's turns plays a monster face up (or a
    card), attacks with everything that wins, and checks every face-up
    monster's shown stats; the CPU plays its own deck of Dragons and others."""
    g = run.game
    mine = [BABY, GIANT, ELF, MUSHROOM, WINGED, IMP, BABY, BLUE_EYES, GIANT, BABY]
    theirs = [BABY, WINGED, BLUE_EYES, ELF, GIANT, MUSHROOM, IMP, MEDICINE]
    run.duel(mine, opponent=theirs, opponent_top=[BABY, WINGED, BABY, GIANT, ELF])
    battles = cpu_battles = 0
    for turn in range(40):
        if g.duel_over():
            break
        hand = [c for c in run.hand() if c]
        monsters = [c for c in hand if c != MEDICINE]
        full = len(run.monsters()) >= 5
        card = (MEDICINE if MEDICINE in hand and (full or not monsters) else
                max(monsters, key=lambda c: run.retail.cards[c].attack) if monsters else hand[0])
        print(f"  turn {turn}: hand {hand}, plays {card}; field {[r['id'] for r in run.monsters()]} vs "
              f"{[r['id'] for r in run.monsters(1)]}, LP {run.lp()}", flush=True)
        run.play(card, face_up=card != WINGED or turn % 2 == 0)
        if g.duel_over():
            break
        shown = run.verify(f"duel turn {turn}")
        if turn in (1, 4, 8):
            run.shot(f"duel-turn{turn}")
        # Attack with each face-up attack-position monster that beats a
        # target (not on the duel's first turn, which has no battle).
        for r in [] if turn == 0 else sorted(run.monsters(), key=lambda r: -shown.get(r["record"], (0, 0))[0]):
            if g.duel_over() or g.turn() != 0 or not r["up"] or r["flags"] & (0x800 | 0x4000):
                continue
            now = run.records()
            if not now[r["record"]] or now[r["record"]]["id"] != r["id"]:
                continue
            power = run.shown(r["record"])[0]
            targets = [t for t in run.monsters(1)]
            if not targets:
                run.battle(r["record"], None)
                battles += 1
                continue

            def guard(t):
                stats = run.expected(t, now)
                return stats[1] if t["flags"] & 0x800 else stats[0]
            weak = min(targets, key=guard)
            if power - 500 > guard(weak):    # wins whatever the stars say
                run.battle(r["record"], weak["record"])
                battles += 1
                if not g.duel_over():
                    run.verify(f"duel turn {turn}, after a battle")
        if g.duel_over():
            break
        cpu_battles += run.cpu_turn()
        run.seen.update(f"cpu{r['id']}" for r in run.monsters(1) if r["up"])
    run.shot("duel-end")
    print(f"  duel: {turn + 1} turns, {battles} battles by the player, {cpu_battles} by the CPU, LP {run.lp()}, "
          f"over {g.duel_over()}")
    run.check("duel: played to its end", g.duel_over(), (turn, run.lp()))
    run.check("duel: battles were fought on both sides", battles >= 3 and cpu_battles >= 3, (battles, cpu_battles))


SCENARIOS = {"count": count, "opponent": opponent, "fusion": fusion, "heal": heal, "duel": duel}


def run_pass(executable: Path | None, out: Path, label: str, env: dict, names: list[str], retail) -> Run:
    folder = out / label
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    log = folder / "log.txt"
    env = {"MEMORIES_TRACE": "duel_effects,mods", "MEMORIES_LOG": str(log), "MEMORIES_NO_MONITOR": "1", **env}
    print(f"{label}:", flush=True)
    with Game(executable, out=folder, mods_dir=out / "mods", env=env,
              settings={f"mod.{MOD}": 1, "mod.3d-monsters": 0, "mod.hand-camera": 0, "mod.ai-hard-mode": 0}) as game:
        run = Run(game, log, label, retail)
        for name in names:
            print(f" {name}", flush=True)
            try:
                SCENARIOS[name](run)
            except Exception as failure:   # one scenario's trouble is reported, the rest still run
                run.check(f"{name}: ran to the end", False, f"{type(failure).__name__}: {failure}")
                run.shot(f"{name}-error")
        notes = [line for line in (folder / "game.log").read_text(errors="replace").splitlines()
                 if line.startswith(f"memories-pc: mod {MOD}:")]
        run.check("the game noted nothing against the mod", not notes, notes)
        counted = re.findall(r"for each: (\d+) counted", log.read_text(errors="replace"))
        if "heal" in names:
            run.check("the log has the heal's count", "2" in counted, counted)
    return run


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--out", type=Path, default=ROOT / "tmp/pc/monster-for-each")
    parser.add_argument("--interpreter", action="store_true", help="also run with the duel effects interpreted")
    parser.add_argument("--reference-executable", type=Path,
                        help="--interpreter's game, one with the MIPS interpreter (default the reference build)")
    parser.add_argument("scenarios", nargs="*", choices=[[], *SCENARIOS], default=[])
    arguments = parser.parse_args()
    out = arguments.out.resolve()
    names = arguments.scenarios or list(SCENARIOS)
    out.mkdir(parents=True, exist_ok=True)
    manifest = write_mod(out / "mods")
    print("cards as the game reads them:", json.dumps(manifest.get("cards"))[:300], flush=True)
    from fm_editor import disc, gamedata
    retail = gamedata.load_game(disc.find_game([ROOT / "game"]))
    passes = [("native", {}, arguments.executable)]
    if arguments.interpreter:   # the interpreter is in no shipped game: the reference build's
        passes.append(("interpreter", {"MEMORIES_DUEL_EFFECTS": "interpreter"},
                       reference_executable(arguments.reference_executable,
                                            subject=arguments.executable or EXECUTABLE)))
    runs = [run_pass(executable, out, label, env, names, retail) for label, env, executable in passes]
    failed = sum(len(r.failures) for r in runs)
    for r in runs:
        print(f"{r.label}: {r.passed} passed, {len(r.failures)} failed" +
              "".join(f"\n  {f}" for f in r.failures))
    print(f"pictures in {out}/*/shots")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
