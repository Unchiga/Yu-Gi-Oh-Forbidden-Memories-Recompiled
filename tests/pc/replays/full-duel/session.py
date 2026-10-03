"""The session this replay was recorded from (tools/pc/replay.py record
tests/pc/replays/full-duel --session tests/pc/replays/full-duel/session.py
--hash-every 4 --settings mod.3d-monsters=0 mod.hand-camera=0): boot, the
title, a duel against Simon Muran through Debug > Jump to's path with a deck
whose opening hand is arranged (two Thunder Dragons, Raigeki, Forest, Red
Medicine), played to its end: the fusion (Twin-headed Thunder Dragon),
Raigeki, the terrain, the heal, attacks with their 3D battles (the MODEL
variant modules, interpreted), the result and the rewards, back to the Free
Duel screen. Every native duel effect it calls is the matched C of the WA
bank; the attacks run each monster's choreography in the MIPS interpreter.
Kept to record it again; playing the replay does not run it."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "tools/pc"))
from yfm_control import FIELD_CURSOR, TARGET_CURSOR  # noqa: E402

ANIMATED_BATTLE, DUEL = 1, 3
SIMON = 1
THUNDER_DRAGON, RAIGEKI, FOREST, RED_MEDICINE, BLUE_EYES = 425, 337, 330, 339, 1
DECK = f"{THUNDER_DRAGON},{THUNDER_DRAGON},{RAIGEKI},{FOREST},{RED_MEDICINE},{BLUE_EYES},{BLUE_EYES},{BLUE_EYES}"
MAGIC = (RAIGEKI, FOREST, RED_MEDICINE)


def hand_ids(game):
    return [card and card["id"] for card in game.duel()[0]["hand"]]


def attack_all(game):
    """Every monster of the player's that has not attacked: at the
    opponent's weakest monster, or directly when they have none."""
    for column in range(5):
        if over(game) or game.turn() != 0:
            return
        mine = game.field()[2][column]
        if not mine:
            continue
        sides = game.duel()
        monster = next((m for m in sides[0]["monsters"] if m and m["id"] == mine), None)
        if monster and (monster["used_this_turn"] or monster["defense_position"]):
            continue
        theirs = [(card, column_) for column_, card in enumerate(game.field()[1]) if card]
        target = None
        if theirs:
            target = theirs[0][1]
        attack_3d(game, column, target)


def attack_3d(game, column, target):
    """Game.attack, with a monster target committed by Square: the 3D battle,
    where the attacker's MODEL variant module runs (notes/pc-build.md, MIPS-only
    effects); Cross commits it without the presentation."""
    game.wait_turn()
    for _ in range(4):   # to the monsters' row
        row = game.u8(FIELD_CURSOR + 1)
        if row == 2:
            break
        game.press("down" if row < 2 else "up", hold=4, after=16)
    game._cursor_to(FIELD_CURSOR, column, "field")
    # Cross on the monster turns the camera to the opponent's side, where the
    # target cursor moves; after a magic card the first Cross can go to the
    # card's panel instead. A test move tells which: back and Cross again.
    for _ in range(4):
        game.press("cross", hold=4, after=80)
        field, aim = game.u8(FIELD_CURSOR), game.u8(TARGET_CURSOR)
        game.press("left" if aim else "right", hold=4, after=16)
        if game.u8(TARGET_CURSOR) != aim:
            break
        game._cursor_to(FIELD_CURSOR, column, "field")
    else:
        raise RuntimeError("the attack's target screen did not come up")
    if target is not None:
        game._cursor_to(TARGET_CURSOR, target, "target")
    # Until the battle starts: the target screen takes a press only once its
    # camera has come round. A direct attack takes Cross only.
    game.press_until(lambda g: g.mode() != DUEL or g.phase() != 5,
                     "square" if target is not None else "cross", every=40, timeout=1200, what="the battle")
    settle(game)


def over(game):
    """The duel's end: Game.duel_over(), except that the 3D battle is a mode
    of its own (1) in the middle of the duel."""
    return game.mode() != ANIMATED_BATTLE and game.duel_over()


def settle(game, timeout=8000):
    """Until the player can act again, their turn is over or the duel is,
    through the 3D battle (Game._settle takes its mode for the duel's end)."""
    start = game.vblank
    while game.mode() == ANIMATED_BATTLE or not (over(game) or game.turn() != 0 or game.phase() in (4, 5)):
        if game.vblank - start >= timeout:
            raise TimeoutError(f"the attack's end not reached (frame {game.frame}, mode {game.mode()})")
        game.step(2)


def run(game, out):
    game.wait_until(lambda g: g.resident("main_menu"), 3000, what="the title")
    game.goto("duel", opponent=SIMON, deck=DECK)
    game.duel_ready(before_deal=lambda g: g.arrange_deck(0, [THUNDER_DRAGON, THUNDER_DRAGON, RAIGEKI, FOREST,
                                                             RED_MEDICINE]))
    game.shot("1-hand.png")
    # Turn 1: the fusion, face up.
    game.fuse([0, 1], face_up=True)
    game.shot("2-fusion.png")
    game.end_turn()
    used = set()
    turns = 0
    while not over(game) and turns < 30:
        turns += 1
        hand = hand_ids(game)
        magic = next((slot for slot, card in enumerate(hand) if card in MAGIC and card not in used), None)
        if magic is not None:
            used.add(hand[magic])
            game.play_card(magic)   # as the card comes up: a magic card so put down is used at once
        else:
            slot = next((slot for slot, card in enumerate(hand) if card == BLUE_EYES), None)
            if slot is None:
                slot = next(slot for slot, card in enumerate(hand) if card)
            game.play_card(slot, face_up=True)
        game.shot(f"3-turn{turns:02d}-played.png")
        if not over(game) and game.turn() == 0:
            attack_all(game)
        game.shot(f"4-turn{turns:02d}-attacked.png")
        if not over(game):
            game.end_turn()
    game.shot("5-over.png")
    # The result and the rewards: Cross through them until the duel's mode ends.
    game.press_until(lambda g: g.mode() != 3, "cross", every=40, timeout=6000, what="the end of the duel")
    game.step(240)
    game.shot("6-after.png")
