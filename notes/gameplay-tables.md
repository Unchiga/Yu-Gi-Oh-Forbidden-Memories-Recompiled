# Gameplay tables: fusions, equips, rituals, drops, decks and more

A mod can change the duel's rule tables with no code at all: which cards
fuse and into what, what an equip card may equip, what a ritual needs and
makes, what each opponent drops, and what each opponent's deck is dealt
from. The rules sit in `mod.json` beside everything else, name cards the
way a person would, and combine with other mods' rules instead of
overwriting them. The worked example is
[`examples/mods/rule-tables`](../examples/mods/rule-tables/mod.json).
What each guardian star gets against each other, and the stars themselves,
are `guardian_stars` ([Mods](modding.md#guardian-stars-names-icons-new-stars-and-matchups)).

```json
{
    "id": "my-rules",
    "name": "My rules",
    "fusions": [
        {"with": ["Kuriboh", "Mystical Elf"], "result": "Celtic Guardian"},
        {"with": ["Baby Dragon", "Time Wizard"], "result": null},
        {"remove": "Gaia the Dragon Champion"}
    ],
    "equips": [
        {"card": "Legendary Sword", "add": ["Dragon"], "remove": ["Curse of Dragon"]}
    ],
    "rituals": [
        {"card": "Black Luster Ritual", "tributes": ["Celtic Guardian", "Dark Magician", "Mystical Elf"],
         "result": "Black Luster Soldier"}
    ],
    "drops": {
        "Simon Muran": {"pow": {"Blue-eyes White Dragon": 20}},
        "all": {"tec": {"Kuriboh": 0}}
    },
    "decks": {
        "Heishin": {"Dark Magician": 60, "Kuriboh": 0}
    }
}
```

Like the cards a mod adds, these are read once when the game starts, so a
mod with any of them needs a restart to apply or remove. Anything a rule
names that the game does not have (a misspelt card, an opponent that does
not exist) is reported in the Mods window and in `MEMORIES_TRACE=mods`, and
that one rule is left out.

## Naming cards

Anywhere a rule names a card it may use:

* the card's name as the disc spells it, `"Blue-eyes White Dragon"`; case,
  spaces and punctuation are ignored, so `"blue eyes white dragon"` finds it
  too (`notes/card-catalog.csv` lists every name);
* its number, `12` or `"12"`;
* a card a mod adds, by its stable identity, `"my-cards:moon-dragon:1"`
  ([More cards](more-cards.md)).

A card a mod `replace`s keeps its disc name here: `"Turtwig"` names nothing
(the Mods window says so) even when a mod renamed a card to it, so name such
a card by its number.

## Fusions

`"fusions"` is a list of rules:

| Rule | Effect |
|---|---|
| `{"with": [A, B], "result": C}` | A and B fuse into C, in either order. It adds a fusion the disc lacks, or changes one it has |
| `{"with": [A, B], "result": null}` | A and B do not fuse |
| `{"remove": C}` | no recipe from the disc makes C any more. Recipes from mods still do |

A rule for a retail card also holds for the copies of it a mod adds, as the
disc's table does; a rule that names a copy itself is surer, and comes
first: a rule for the two cards as they are, then one naming one of them
as it is and the other's base (the later of two such), then the bases'
rule. Mods' rules are asked before the recipes of an added card's own
`fusions` list, which come before the disc's table. A card a `replace`
made another kind (a monster made a magic card, an equip made a monster)
is out of the disc's table, as material and as result, since the table is of
the card it was ([More cards](more-cards.md)); the mods' rules still name it.
The AI fuses by the same rules as the player.

## Equips

`"equips"` is a list, one entry per equip card:

| Key | Meaning |
|---|---|
| `card` | the equip card |
| `add` | cards, or monster types (`"Dragon"`, `"Winged Beast"`), it may now equip |
| `remove` | cards or types it may no longer equip |
| `replace` | `true`: it equips only what `add` names, and nothing the disc listed |

Within one entry a named card is surer than a type and a type surer than
`replace`, so `"add": ["Dragon"], "remove": ["Curse of Dragon"]` equips every
dragon but one. What no entry mentions, the disc's table decides, except for
a card a `replace` made another kind, which the disc's table no longer
covers either way: an equip made from a monster equips only what `add`
names.

An entry may also set what the equip adds to the monster's ATK and DEF, in
place of the disc's +500 (+1000 for Megamorph):

| Key | Meaning |
|---|---|
| `bonus` | points, with a sign, for any monster it equips |
| `bonus_if` | an object of monster types (`"Dragon"`) or attributes (`"Light"`, `"Dark"`, `"Earth"`, `"Water"`, `"Fire"`, `"Wind"`) and their points |

```json
"equips": [
    {"card": "Legendary Sword", "bonus": 300, "bonus_if": {"Warrior": 800, "Light": 600}},
    {"card": "Megamorph", "bonus": 1500}
]
```

The first `bonus_if` that fits the monster decides, then `bonus`; an entry
with neither for this monster says nothing, and an earlier entry decides,
else `equip_bonus_default` (below) if a mod sets it, else the disc. The latest entry that says something wins, as for what an
equip may equip, and a copy of an equip a mod added has its base's bonus.
Values are whole points from -9999 to 9999; a negative bonus lowers the
monster. A bonus past Megamorph's +1000 climbs on screen in about the time
+1000 takes, instead of 31 points a frame.

`"equip_bonus_default": 700`, beside `equips` at the top of the manifest,
sets what every equip no entry gives a bonus for adds, in place of the
disc's +500 and Megamorph's +1000 alike (give Megamorph an entry to keep it
apart). Without it those keep the disc's values. The latest mod that sets it
wins. Equips whose effect is more than a bonus (a Wicked Gods or Remaster
equip that heals, or gives +400 ATK and -200 DEF) need a code mod: the
bonus here always goes to ATK and DEF alike. ATK and DEF still stop at 9999 and 0. Reverse Trap turns
the bonus the equip gave into as large a loss, as it does on the disc. The CPU chooses its equips as before: it
never counted the bonus.

## Rituals

`"rituals"` is a list, one entry per ritual card. `card` is one of the
disc's ritual cards, a mod's copy of one (`"copy"` of a ritual card,
[more-cards.md](more-cards.md)), or a card made a ritual (`"type": "Ritual"`
with `"effect"` naming a ritual card, whose effect it is played with). A
card only typed Ritual does nothing when played and takes no recipe. A
copy or "effect" card without an entry of its own is summoned by that
ritual card's recipe. `tributes` names the three monsters it takes, and
`result` what it summons. `"result": null` takes the ritual away. A tribute
may be a copy a mod added; a retail tribute is also met by a copy of it.
Every tribute takes a monster that is exactly it before any takes a copy,
so a recipe naming both a retail monster and a copy of it is met whatever
order they stand in on the field.

A tribute may also be an object of conditions, all of which the monster
must meet:

```json
{"card": "Curse of Millennium Shield", "result": "Millennium Shield", "tributes": [
    {"type": "Spellcaster"},
    {"min_defense": 2000, "max_level": 4},
    {"fusion_group": "Female", "defense_gt_attack": true}]}
```

| Key | The monster |
|---|---|
| `card` | is this card, or a copy of it |
| `type` | is of this monster type (`"Dragon"`, `"Rock"`...) |
| `fusion_group` | is in this group of the fusion guides: `AngelWinged`, `Bugrothian`, `Egg`, `Elf`, `FeatherFromBear`, `FeatherFromHarpie`, `FeatherFromMachine`, `Female`, `Jar`, `Koumorian`, `MercuryMagicUser`, `MercurySpellcaster`, `Mirror`, `MusKingian`, `MystElfian`, `Rainbow`, `Sheepian`, `Thronian`, `Turtle`, `UsableBeast` |
| `min_attack`, `max_attack`, `min_defense`, `max_defense` | has at least or at most this printed ATK or DEF, 0 to 9999 |
| `min_level`, `max_level` | has at least or at most this many stars, 0 to 12 |
| `defense_gt_attack` | `true`: has more DEF than ATK |

Printed means the card's own stats, a mod's `cards` edits included, not what
equips or the terrain add in the duel. `{"min_attack": 0}` is any monster.
The three tributes may be plain cards and objects mixed; they are three
different monsters of the side's field, and the ritual takes place when any
three of them meet the three tributes. When more monsters would do, it
spends the weakest: tribute by tribute, the narrowest first (a `card`
first), the lowest DEF when the result has more DEF than ATK, the lowest ATK
otherwise. A key it does not know is noted in the Mods window and left out,
and an object with no key it knows leaves the entry out.

A card's groups are the disc card's, as the fusion guides list them (Marcelo
Silvarolla's table). A card of a mod's
own has its base's, or its own with `"fusion_groups": ["Elf", "Female"]` in
its [`cards` entry](more-cards.md).

## Drops and decks

Each opponent draws its deck, and the card it gives when it loses, from a
weighted pool: a weight for each card, out of 2048. Four pools per
opponent:

| Key | Pool |
|---|---|
| `decks` | the cards its deck is dealt from (40 cards, at most 3 of each), or its fixed deck (below) |
| `drops` → `pow` (or `sa-pow`) | the prize for an S or A rank won on POW |
| `drops` → `bcd` (or `b-c-d`) | the prize for a B, C or D rank |
| `drops` → `tec` (or `sa-tec`) | the prize for an S or A rank won on TEC |

Opponents are named as in the table below, by their number, or `"all"` for
every one of them. A pool is an object of cards and weights:

* a listed card gets exactly its weight, out of 2048: `"Dark Magician": 60`
  is 60 chances in 2048, about 3%;
* `0` takes a card out of the pool;
* the cards not listed share whatever is left, in the proportions they had;
* `"replace": true` empties the pool first, so it holds only the listed
  cards;
* if the listed cards come to 2048 or more, or nothing else is left in the
  pool, the listed cards make up the whole pool, in proportion to their
  weights: `{"replace": true, "Kuriboh": 1, "Mystical Elf": 3}` is a
  quarter Kuriboh and three quarters Mystical Elf.

The weights are always brought back to exactly 2048, which the game's draw
needs. An edit that would leave a deck pool with fewer than 14 cards (a deck
of 40 at 3 copies each needs that many), or a drop pool with none, is
refused and the pool is left as it was.

Edits of the same pool from several mods add up: each applies to the pool
as the mods before it left it, so one mod making Blue-eyes likelier and
another taking Kuriboh out of the same pool both take effect. Edits apply on
top of what the game loaded, so a data mod's byte patch of a pool comes
first. `"all"` in a mod applies where it is written, before or after that
mod's edits of one opponent. Cards a mod adds may be in a pool too.

| # | Opponent | # | Opponent | # | Opponent |
|---|---|---|---|---|---|
| 1 | Simon Muran | 14 | Yami Bakura | 27 | Desert Mage |
| 2 | Teana | 15 | Pegasus | 28 | High Mage Martis |
| 3 | Jono | 16 | Isis | 29 | Meadow Mage |
| 4 | Villager 1 | 17 | Kaiba | 30 | High Mage Kepura |
| 5 | Villager 2 | 18 | Mage Soldier | 31 | Labyrinth Mage |
| 6 | Villager 3 | 19 | Jono 2nd | 32 | Seto 2nd |
| 7 | Seto | 20 | Teana 2nd | 33 | Guardian Sebek |
| 8 | Heishin | 21 | Ocean Mage | 34 | Guardian Neku |
| 9 | Rex Raptor | 22 | High Mage Secmeton | 35 | Heishin 2nd |
| 10 | Weevil Underwood | 23 | Forest Mage | 36 | Seto 3rd |
| 11 | Mai Valentine | 24 | High Mage Anubisius | 37 | DarkNite |
| 12 | Bandit Keith | 25 | Mountain Mage | 38 | Nitemare |
| 13 | Shadi | 26 | High Mage Atenza | 39 | Duel Master K |

Opponent 0 is an unused copy of Simon Muran. The disc's own pools are in
`notes/research/fusion-and-drop-tables/drops.csv`.

### Fixed decks

A deck may instead be written down card by card, as some community mods do:
`"fixed": true` makes the numbers **copies**, not weights, and they must add
up to exactly 40:

```json
"decks": {
    "Simon Muran": {"fixed": true, "Kuriboh": 4, "Mystical Elf": 6, "Celtic Guardian": 30}
}
```

The duel deals those forty cards and shuffles them as it shuffles any deck.
The limit of three copies of a card does **not** apply to a fixed deck: the
counts are the deck, so a limit could only refuse the list or change it
behind the author's back, and nothing in the duel needs it (the game
itself deals a two-player deck of whatever its save holds). A dealt,
weighted deck keeps the limit. A card named in a fixed deck is dealt as it
is: a copy a mod added is not traded for its base, nor a retail card for one
of its copies.

A fixed deck that does not come to 40 cards, or names a card the game does
not have, is reported and left out, and so is a deck whose `"fixed"` is not
`true` or `false` (`"fixed": "true"`, in quotes). The latest fixed deck of an opponent
wins, and a fixed deck wins over weighted edits of the same deck from any
mod, which are reported as left out the first time the deck is dealt.
`"all"` fixes every opponent's deck. `MEMORIES_TRACE=mods` logs each fixed
deck as it is dealt.

## Terrain bonuses

A terrain (the field a Forest, Wasteland, Mountain, Sogen, Umi or Yami card
sets, or an opponent's home field) gives some monster types +500 and a few
-500 on the disc. A mod may set the bonus of any terrain and monster type,
in points, with a sign:

```json
"terrain_bonus": {
    "Forest": {"Beast": 300, "Insect": 300, "Fairy": -200},
    "Umi":    {"Aqua": 400, "Machine": -400},
    "replace": true
}
```

Terrains are named as the field cards are, `Forest`, `Wasteland`,
`Mountain`, `Sogen`, `Umi` and `Yami` (also `Meadow`, `Sea` and `Dark`, or
1 to 6), and types as a card's are (`"Winged Beast"`, `"Beast-Warrior"`).
Only monster types have a terrain bonus. Values are whole points from -9999
to 9999; they need not be multiples of 500, nor of 10. A pair the mod lists
has its value; a pair it does not keeps the disc's, unless `"replace": true`,
which gives every pair the mods do not list no bonus at all. Several mods'
tables add up the same way, the later one winning for a pair it lists, and a
later `"replace"` clearing what earlier mods set. Everything the game works
out from a terrain asks the same function (`Duel_GetTerrainBoost`) and
follows: a monster's ATK and DEF when it is placed, their recount when a
field card is played, the CPU's weighing of its cards and the fusion
helper. A monster's attribute is
not a terrain's affair (a mod whose fields favour attributes needs code),
and there are still only the six terrains.

## Attack traps

Six traps spring when a monster attacks, each up to an attack of its own:
House of Adhesive Tape 500, Eatgaboon 1000, Bear Trap 1500, Invisible Wire
2000, Acid Trap Hole 3000, and Widespread Ruin whatever the attack (25500
on the disc). A mod may set those thresholds, in points of ATK:

```json
"trap_thresholds": {"House of Adhesive Tape": 800, "Acid Trap Hole": 3500}
```

A trap springs when the attacker's ATK is at or under its threshold. The
duel looks at the attacked side's set attack traps from Widespread Ruin
down and stops at the first whose threshold is under the attack; the last
trap it passed springs, which, with the thresholds in order, is the weakest
trap that stops the attacker. Keep the six in that order (each at least the
one before it): the Mods window warns when they are not, naming the two
thresholds and the mod that set each (or the disc), since a trap behind
a lower threshold would never spring. Values are whole points, 0 to 65535;
a trap no mod names keeps the disc's threshold. A copy of a trap springs as
its base. Which cards are attack traps, and what they do, is the disc's.

## A full chest pays starchips

The chest holds at most 250 copies of a card; on the disc a copy won past
that is lost. A mod may keep the chest smaller, and make each copy it has no
room for worth starchips instead:

```json
"chest_overflow": {"limit": 3, "starchips": 3}
```

| Key | Meaning |
|---|---|
| `limit` | the copies of a card the chest keeps, 1 to 255 (250 when left out); past 250 is the same as `"limits": {"chest": n}` below |
| `starchips` | what each card won past `limit` is worth, 0 to 999999 (0 when left out) |

The Wicked Gods keeps the 250 and pays 1 starchip (`{"starchips": 1}`); the
Remaster keeps 3 and pays 3. The balance stops at 999999, as the game's own
prize does (or at a mod's `"limits": {"starchips": n}`). It holds wherever the game gives the player a card
(`Duel_AwardCard`): a duel's drop, the extra drops of Game > Card drops, and
a card bought in the password shop. The shop sells no copy the chest has no
room for: EXCHANGE is red and only QUIT can be chosen, as when the starchips
fall short, and neither the price nor the password is spent. A chest that
already held more than `limit` of a card (a save from before the mod) keeps
them; only new copies are turned away. The latest mod that sets it wins.
Without the key the chest is the disc's in every case.

## Limits: ATK, DEF, LP, starchips and more

The game caps some numbers: a monster's ATK and DEF at 9999, whatever its
bonuses; a duel's life points at 8000 to start and, when healing, at what
they started with; the starchips at 999999; a card's copies in the chest at
250; a Free Duel record at 999 wins and losses. A mod may raise or lower
each of them:

```json
"limits": {
    "stats": 30000,
    "life_points": {"start": 16000, "max": 30000, "duelists": {"Heishin": 20000}},
    "starchips": 5000000
}
```

| Key | Meaning | The game's | Range |
|---|---|---|---|
| `stats` | the most ATK and DEF a monster has, with its equips, terrain and guardian star | 9999 | 0-32767 |
| `attack`, `defense` | the same for one of the two (after `stats`) | 9999 | 0-32767 |
| `life_points` | a number: both sides' LP at the start of a duel against the CPU; or an object of the keys below | 8000 | 1-32767 |
| `life_points` `start` | both sides' start | 8000 | 1-32767 |
| `life_points` `player`, `opponent` | one side's start (after `start`) | 8000 | 1-32767 |
| `life_points` `max` | how far healing (the recovery cards) takes LP; a side that starts past it keeps its LP and is not healed | the side's start | 1-32767 |
| `life_points` `duelists` | an object of duelists (their names as in `drops`, or `"all"`) and a number, that duelist's own LP, or `{"player": n, "opponent": n}`: the LP each side starts with against that duelist, before `player`, `opponent` and `start` | | 1-32767 |
| `two_player` | the two-player duel's LP choice: `start` (where both begin), `max` (the most to pick) and `step` (each press) | 8000, 8000, 500 | 1-32767 |
| `starchips` | the most starchips the save holds, from a duel's prize, a full chest or Game > Cheats | 999999 | 0-99999999 |
| `chest` | the copies of a card the chest keeps (the same as `chest_overflow`'s `limit`) | 250 | 1-255 |
| `free_duel_record` | the most wins, and losses, a Free Duel opponent's record counts | 999 | 1-32767 |
| `two_player_record` | the same for the wins and losses two-player duels add to a save | 9999 | 1-65535 |

Everything is optional; each key's latest mod wins, and two mods' entries
for different duelists add up. A save that holds more than 250 copies of a
card (from a mod's `chest` past 250) keeps them when played without it: a
copy won then is turned away, as at 250, rather than wrap the byte to none;
in the same way a starchip balance past the cap in force (a mod's `starchips`
that is off now, or lowered) is kept, and a prize adds nothing to it rather
than cut it back. Game > Cheats > Starting LP, when set to
anything but the console's 8000, still comes first.

**What the numbers are kept in.** ATK, DEF and LP are 16-bit numbers in the
duel's own records (`DuelCardRecord`, `AiActiveCard`, `DuelSideState`), so
32767 is as high as they go; the chest is a byte a card in the memory
card's save (255), and the records are 16-bit numbers in the save. A value
past that is noted in the Mods window and held at the most the game keeps,
never cut short without a word. Going further would mean widening those
records, which the whole duel, the AI and the save format read at fixed
offsets: not a table change. A card's own printed ATK and DEF are nine bits
of tens in the card table (0 to 5110; `cards` notes a value past that, or
one between tens): the limits raise what a monster reaches with bonuses,
equips, fusions into it and the terrain, not what it is printed with. Every
bonus a mod sets (`equips`, `equip_bonus_default`, `terrain_bonus`) may be
up to 32767 either way; the cap decides what the sum comes to.

**On screen.** Nothing moves while every number fits in the digits the game
gives it. Past them:

- the life-point panel is one digit wider on the left when either side may
  reach 10000 (its start, its healing `max`, or its LP), drawn from its own
  picture, with the labels (and the opponent's name for COM) moved with it;
- a card on the field or in the hand with ATK or DEF past 9999 shows five
  digits a row, seven pixels apart, the sword and shield two to the left;
- the card view (and the battle's close-ups) shows five digits five pixels
  apart where four go six apart, so they touch but stay clear of the ATK
  and DFD labels and the plate's edge;
- a number in the game's text (the card bar under the field, the results'
  REMAINING LP, the Password screen's starchips) keeps all its digits, drawn
  closer together in the field's own width, so nothing after it moves;
- the two-player setup's LP choice draws five digits in its box when its
  `max` has five (the box holds them, a pixel or two from each side), and its
  bar runs from 0 to that `max`.

**The AI.** Scripts that look for the weakest monster, or the weakest that
still wins, start from the cap of the stat they rank by rather than from
9999, so they go on finding one when monsters pass 9999; at the disc's cap
they are the disc's.

**Code mods** read the limits in force with the mod API's `limit` (API 8):
`host->limit(host, "attack")`.

## Passwords and prices on the Password screen

A mod may change what password gives each card on the Password screen and
what it costs, naming the card as it names cards everywhere else:

```json
"passwords": {
    "Blue-eyes White Dragon": {"password": "00000001", "starchips": 100000},
    "Mystical Elf": {"password": "00000002", "starchips": 16},
    "Dark Magic Ritual": {"password": ""}
}
```

Or every card at once, with `"all"`:

```json
"passwords": {
    "all": {"password": "card number", "starchips_percent": 10},
    "Blue-eyes White Dragon": {"starchips": 5000}
}
```

| Key | Meaning |
|---|---|
| `password` | up to eight digits, as a string (`"00000001"`) or a number (`1`); `"card number"` for the card's own number (Blue-eyes `00000001`, Magician of Black Chaos `00000722`); `""` or `null` for none, so the screen cannot give the card |
| `starchips` | what the card costs, 0 to 999999; 0 is free: EXCHANGE gives the card and takes nothing |
| `starchips_percent` | what it costs as a percent of the price the game loaded (the disc's, or a `data` patch's), 0 to 1000, rounded; 0 is free, and any other percent of a card that cost something still costs at least 1 |

An entry may leave out either key, and the card keeps the disc's (or an
earlier mod's). `"all"` applies to all loaded cards before the cards named beside
it, wherever it is written, so a named card keeps what its own entry says.
A card is a name, a number or a stable identity; cards a mod adds past 722
can be bought there too. Added cards default to 999999 starchips until a rule
sets their price, and can be bought repeatedly (the retail used flags cover
only cards 1–722). A `cards[].password` works for both added and replaced
cards; an explicit `passwords` table entry overrides it. Two cards with the same password (two
mods' cards, or a mod's password that is already another card's on the
disc): the screen gives the lower card number, and the Mods window notes
the pair beside the mod that set the password, once the Password screen
has loaded its table.
View > Card passwords shows the passwords the mods set. The latest mod that
sets a card's password or price wins.

For an added card, use `copy` and a stable ID (a `replace` keeps the original
card number). The same password is accepted by the shop and printed by
View > Card passwords:

```json
{
  "id": "aurora",
  "cards": [{"copy": 58, "id": "wing", "name": "Aurora Wing", "password": "00001723"}],
  "passwords": {"aurora:wing:1": {"starchips": 100}}
}
```

The optional [Card Number Passwords mod](../mods/card-number-passwords/README-EN.md)
uses numeric IDs for cards without an explicit password and keeps the viewer
in sync. Update older copies of that mod: versions that replace the entire
shop handler bypass the shared policy.

## Rules a setting switches

A `fusions`, `equips` or `rituals` entry may say `"setting": "key"`, which
names one of the mod's declared `settings`: the entry is read only while
that setting is not 0, or, with `"value": N` as well, only while it is
exactly N (one choice of a `choice` setting). An entry without `setting`
is always read. So one mod may let the player turn groups of its rules on
and off in the Mods window:

```json
"settings": [
    {"key": "thunder_fusions", "label": "Thunder + Fiend fusions", "type": "bool", "default": 1,
     "restart": true, "description": "Thunder and Fiend monsters fuse into King of Yamimakai."},
    {"key": "expanded_fusions", "label": "Expanded Fiend fusions", "type": "bool", "default": 1,
     "restart": true, "description": "Fiends fuse with Dragons, Beasts and Warriors."}
],
"fusions": [
    {"with": ["Kuriboh", "Thunder Dragon"], "result": "King of Yamimakai", "setting": "thunder_fusions"},
    {"with": ["Kuriboh", "Baby Dragon"], "result": "Darkfire Dragon", "setting": "expanded_fusions"}
]
```

The Mods window shows each setting's `label`, with its `description`
under it. The tables are read as the game starts, so such a setting wants
`"restart": true`. A `setting` the mod does not declare is noted in the
Mods window and the entry read. Each key is read once: a manifest with
two `"fusions"` lists reads the first and warns of the second, so the
groups go in one list, each entry with its setting. The FM Editor shows
the disc's table and keeps these entries as they are written.

## Where two mods disagree

Mods apply in load order (priority, then `after` and `requires`, then the
order they were found; [the API 3 guide](mod-api-3.md)), and a later mod's
fusion, equip or ritual rule wins over an earlier one's for the same cards.
Pools add up, as above. The Mods window lists each place two enabled mods
meet and how it comes out ([When mods overlap](modding.md#when-mods-overlap)).

## Code mods

A code mod can decide the same questions at run time with managed events
([API 3](mod-api-3.md)): `FUSION` for fusions and `EQUIP` for equips come
before these tables, and a handled event overrides them. `REWARD` sees the
card a drop pool gave.

## How the port does it

`src/pc/cards/tables.c` reads the rules once, after the cards
(`Cards_Build`), from the mods in the order they loaded (`Mods_Loaded`).
Nothing the game loaded is changed, except the Password screen's table
(its search is a loop over the loaded records); the functions that read
each table ask it first:

| Game function | Table | Asks |
|---|---|---|
| `Duel_CheckFusion` (`duel_card_checks.c`) | fusion table, `0x8017C2D8` | `Tables_Fusion`, then `Tables_FilterFusion` over the disc's answer |
| `Duel_CheckEquip` (`duel_card_checks.c`) | equip table, `0x8017A1D8` | `Tables_Equip` |
| `DuelScene_UpdateCardPlacement` (`duel_scene_card_placement.c`) | +500, +1000 for Megamorph | `Tables_EquipBonus` |
| `Duel_CheckRitual` (`duel_check_ritual.c`) | ritual table, `0x801799D8` | `Tables_RitualRequirements` for conditions, else `Tables_Ritual`, whose recipe is laid out like the disc's |
| `Duel_ShuffleDeck` (`duel_shuffle_deck.c`) | deck pool, `0x801781D8` | `Tables_FixedDeck`, then `Tables_Pool(TABLES_POOL_DECK)` |
| `Duel_SelectCardDrop` (`duel_result_runtime.c`) | drop pools, `0x8017878C` | `Tables_Pool(TABLES_POOL_POW + pool)` |
| `Duel_GetTerrainBoost` (`duel_card_record_lifecycle.c`) | terrain table, `0x800909D4` | `Tables_TerrainBonus` |
| `Duel_SelectAttackTrap` (`duel_trap_resolution.c`) | trap thresholds, `0x8009AF24` (bytes, x100) | `Tables_TrapThreshold` |
| `Duel_AwardCard` (`duel_result_runtime.c`) | chest, `0x801D0250`; starchips, `0x801D07E0` | `Tables_ChestOverflow` before the card is counted, `Tables_ChestFull` after |
| `Password_UpdateShopScreen` (`overlays/password/shop.c`) | chest, `0x801D0250`; price countdown | `Duel_ChestFull` (`Tables_ChestFull`) before EXCHANGE is offered; a price of 0 skips the countdown |
| `Main_RunPasswordMenu` (`main_run_password_menu.c`) | price and password table, `0x801A8000` | `Tables_PasswordShop` for each card once the table is loaded, written into it; then `Tables_CheckPasswords` once a run |
| `Duel_CalcCardStats`, `Duel_CalcBattleAttack`/`Defense`, `Duel_GetBaseCardStat` | the 9999 cap | `Tables_StatCap` |
| `AiScript_FindWeakest` (`ai_script_combo.c`), `AiScript_FindKiller` | the 9999 starting bar | `Tables_StatCap`, `Tables_StatCapEither` |
| `DuelScene_UpdateCardPlacement` | an equip's room, twice the cap | `Tables_StatCapEither`, at most 32767 |
| `Duel_InitSideStates` (`duel_state_init.c`) | 8000 LP, healing up to the start | `Tables_StartingLifePoints`, `Tables_MaxLifePoints` |
| `DuelEffect_ApplyLifePointRecovery` (`duel_card_effects.c`) | the heal, a 16-bit sum | added in 32 bits, never past the cap nor down to it |
| `Main_RunTwoPlayerDuelSetup`, the setup screen (`overlays/main_menu/value_setup.c`) | 8000, by 500 | `Tables_TwoPlayerLifePoints` |
| `func_800218F0` (the duel's end), `Mods_AwardStarchips`, `Cheats_SetStarchips` | 999999 starchips; 9999 two-player wins | `Tables_StarchipCap` (`Mods_Limit`), `Tables_TwoPlayerRecordCap` |
| the Free Duel screen (`overlays/free_duel/screen_runtime.c`) | 999 wins or losses | `Tables_FreeDuelRecordCap` |
| `Duel_AwardCard`, `BuildDeck_ReturnCardToChest`, the trade screen | 250 copies | `Tables_ChestRoom` |
| `Duel_DrawLifePointsAndDeckCounts`, `func_80016784`, `func_80028B08`, `func_80038148` | four digits | the layouts above (`pc/text/number_width.h` for the text) |

A pool is worked out from the opponent's loaded pool and every edit of it
when the game draws from it, and kept until the opponent or the loaded pool
changes. An edited pool is drawn from exactly as the disc's is, with one
random number per draw and the same threshold, over every card the run has;
an opponent or pool no mod edits takes the game's own path, so without such
mods the random sequence, and every recorded run, is unchanged. The console
build has none of this (`#ifdef MEMORIES_PC`).

`tests/pc/tables_test.c` (ctest `pc_tables`) covers the rules, their order
between mods, the weights, the limits and the refusals.
