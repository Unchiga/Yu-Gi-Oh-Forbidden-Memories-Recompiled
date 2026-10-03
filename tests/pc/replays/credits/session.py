"""The session this replay was recorded from (tools/pc/replay.py record
tests/pc/replays/credits --session tests/pc/replays/credits/session.py
--hash-every 8 --settings mod.3d-monsters=0 mod.hand-camera=0): boot, the
title, the credits through Debug > Jump to's path (their mode, as
MEMORIES_MODE_AT sets it), the save prompt answered with Cross, SECRET NO.,
and the whole roll with its 3D scenes, until the presentation is complete.
The credits module's matched C draws every name through LIBAPI's
Krom2RawAdd2, whose glyphs the port renders on the host: the 64-bit build
must hand the game their addresses below 4 GB. The session ends before the
port restarts the game three seconds later (src/pc/platform/credits.c): a
recording is one process. Kept to record it again; playing the replay does
not run it."""
STILL = 150   # VBlanks of an unchanged picture: the roll is over (the restart comes 180 frames later)


def run(game, out):
    game.wait_until(lambda g: g.resident("main_menu"), 3000, what="the title")
    game.goto("credits")
    # The save prompt (the port's slot menu over the retail one), then SECRET NO.
    game.step(450)
    game.press("cross")
    game.step(300)
    game.shot("1-secret.png")
    game.press("cross")
    game.wait_until(lambda g: g.resident("credits"), 3000, what="the credits module")
    game.step(1500)
    game.shot("2-names.png")
    # The presentation's end (Model_IsCreditsPresentationComplete) is a
    # variable of the port's executable, not in guest RAM where the client
    # looks: the picture standing still tells it. No scene of the roll holds
    # one picture this long.
    still, last = 0, game.hash()
    while still < STILL:
        if game.vblank > 30000:
            raise TimeoutError("the end of the credits not reached")
        game.step(10)
        now = game.hash()
        still = still + 10 if now == last else 0
        last = now
    game.shot("3-end.png")
