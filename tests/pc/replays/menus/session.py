"""The session this replay was recorded from (tools/pc/replay.py record
tests/pc/replays/menus --session tests/pc/replays/menus/session.py
--hash-every 4 --settings mod.3d-monsters=0 mod.hand-camera=0): boot, the
title and its menu, then each screen of the main menu through Debug > Jump
to's path, moved around in: Options, Build Deck (with the deck a duel's
jump put in the save), the Library, Password, the campaign map and Free
Duel's list of duelists. Kept to record it again;
playing the replay does not run it."""


def walk(game, keys, every=20):
    for key in keys:
        game.press(key, hold=4, after=every)


def run(game, out):
    game.wait_until(lambda g: g.resident("main_menu"), 3000, what="the title")
    game.press("start")   # the title's menu: NEW GAME, CONTINUE
    game.step(120)
    walk(game, ["down", "up", "down"], 30)
    game.shot("0-title-menu.png")
    # A deck in the save for Build Deck to show: a duel's deck replaces it.
    game.goto("duel", opponent=1, deck="1-40")
    game.step(60)
    screens = [
        ("options", ["down", "down", "right", "left", "down", "down", "up"]),
        ("build_deck", ["down", "down", "down", "right", "right", "down", "cross", "circle", "up", "triangle"]),
        ("library", ["down", "down", "right", "down", "cross", "circle", "down", "down"]),
        ("password", ["up", "right", "up", "up", "right", "down", "left"]),
        ("map", ["right", "right", "down", "left", "up", "up"]),
        ("free_duel", ["down", "down", "right", "down", "up", "left"]),
    ]
    for number, (screen, keys) in enumerate(screens, 1):
        game.goto(screen)
        game.step(120)
        game.shot(f"{number}-{screen}-in.png")
        walk(game, keys)
        game.step(60)
        game.shot(f"{number}-{screen}-moved.png")
