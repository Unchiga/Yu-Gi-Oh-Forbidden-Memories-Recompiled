"""Save states and the stack of the interpreted model modules (fixed in
"Port: save states carry the interpreter's stack"). The animated battle of
MODEL.MRG record 330 (card 381) with its second guardian star runs variant
module 128 in slot A and 258 in slot B, which read locals they never wrote:
the fourth byte of the two colours handed to Model_QueueTintRequestForParts
(the start one is the tint pass's draw mode) and the corners of a POLY_FT4.
The battle is run from a state, then record 320's battle (modules 443 and
593, whose frames leave other bytes there), then 330's again from the same
state: every frame must be the same. Before the fix the stack was not in
the state, so the second run drew other sparks from 679 frames in. The
battles start the way the duel starts one (duel_scene_battle.c, phase 5):
the two model records into D_800EF658, the mode to return to into
D_8009B269, the animated battle (1) into D_8009B26C."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "tools/pc"))
from yfm_control import Game  # noqa: E402

SETTINGS = {"mod.3d-monsters": 0, "mod.hand-camera": 0, "mod.ai-hard-mode": 0}
ANIMATED_BATTLE = 1
# The frames compared, counted from the battle's start: the difference was at 679.
FIRST, LAST = 640, 720


def properties(card, alternative):
    """D_800EF658's two records (main_modes.h): the card on both sides, a tie."""
    record = struct.pack("<HhhBB", card, 0, 0, 0, alternative)
    return record + record


def battle(game, card, alternative, frames=None):
    """One battle from the state: the picture's hash at each frame asked for,
    or (frames None) run to its end."""
    game.load("base.state")
    game.poke("D_800EF658", properties(card, alternative))
    game.poke("D_8009B269", bytes([0]))   # back to the debug menu after
    game.poke("D_8009B26C", bytes([ANIMATED_BATTLE]))
    start = game.frame
    if frames is None:
        while game.u8("D_8009B26C") & 0x1F == ANIMATED_BATTLE:
            assert game.frame - start < 12000, f"card {card}'s battle did not end"
            game.step(16)
        return None
    hashes = []
    for offset in frames:
        while game.frame < start + offset:
            game.step(1)
        hashes.append(game.hash())
    return hashes


def run(executable, out):
    (out / "mods").mkdir(parents=True, exist_ok=True)
    with Game(executable, out=out / "game", settings=SETTINGS, mods_dir=out / "mods") as game:
        game.goto("debug")
        game.save("base.state")
        frames = range(FIRST, LAST)
        first = battle(game, 381, 1, frames)
        battle(game, 371, 1)
        second = battle(game, 381, 1, frames)
        differing = [FIRST + i for i, (a, b) in enumerate(zip(first, second)) if a != b]
        assert not differing, (f"record 330's battle from the same state drew other frames after record 320's "
                               f"battle, from +{differing[0]} ({len(differing)} of {len(first)} frames)")
