# More cards than the disc has

The PC port can have any number of cards past the disc's 722, up to card id
32766. A mod adds them with a `cards` list in its `mod.json`, and they work
wherever a card does: the Library, Build Deck, duels (the hand, the field, the
3D battle, fusions, equips, rituals and card effects), duel rewards, the card
viewer, saves, save states, trading and two-player duels.

Each new card starts as a **copy** of a retail card, its *base*. Model, effect
and fusion defaults come from that card, but entries can specify independent
`model`, `effect` and `fusions` values. Managed code hooks can implement new
behavior; see [Mod API 3](mod-api-3.md). Everything a player reads off the card
can be its own: name, artwork, card text, ATK, DEF, type, guardian stars,
level and attribute. What a mod leaves out is the base's. The disc has none
of these cards, so wherever the game goes to the disc, or to a table laid out
by the disc, it asks for the base instead, and lays the card's own art and
text over what comes back.

The release ships no card mod; the checks below were made with test mods
(a thousand made-up names, and a card with its own picture).

## The manifest

```json
{
    "id": "wacky",
    "name": "Wacky cards",
    "cards": [
        { "copy": "Kuriboh", "name": "Dingus Shmingus", "art": "images/dingus.png",
          "description": "A round and cheerful fellow who has never once been on time.",
          "type": "Beast", "attribute": "Fire", "level": 7, "stars": ["Moon", "Venus"],
          "attack": 2500, "defense": 2100 },
        { "copy": 1, "name": "Shmungus Mingus", "description": "Blue-eyes' cousin from out of town." },
        { "copy": "Kuriboh", "count": 100, "count_setting": "count", "name": "Kuriboh {n}",
          "drops": false, "opponents": true }
    ]
}
```

| Key | Meaning |
|---|---|
| `copy` | the base: a retail card id (1-722) or its name as the game spells it (`"Kuriboh"`, any case) |
| `count` | how many cards this entry adds (default 1) |
| `count_setting` | read `count` from one of the mod's settings instead, so `MEMORIES_MOD_<ID>_COUNT=5000` or `mod.<id>.count=5000` in the settings file changes it without editing the manifest |
| `name` | the cards' own name; `{n}` is the card's number within the entry and `{id}` its card id. Without one a card has its base's name. Letters, digits, spaces and ``!"#$%&'()*+,-./:<>?`` are what the game's font has; accented letters and others the port adds ([translations](translation.md)) work too |
| `description` | the card's own text (UTF-8: accented letters work, [translations](translation.md)), wrapped as the retail texts are (lines of up to twenty letters, broken at spaces; `\n` breaks a line where it stands). The codes the FM Editor shows work too: `{f8 0B NN}` an icon (two letters wide), `{f8 0A NN}` a color ([the tables](#card-text-codes)), `{g X}` a glyph by number. Eight lines is the most any retail text has. Without one a card has its base's text |
| `art` | a PNG in the mod (a path relative to its directory): the card's picture and, made from the same image, the small one the hand and field show. Any size: the middle of it at the card's shape is taken and scaled to 102x96 and 40x32, and its colors reduced to the 255 and 63 each has. An image bigger than that is also drawn at its own resolution when View > Console resolution is set above 1x (Internal 2x, 4x), as a texture pack's image is ([HD pictures](#hd-pictures)), so 408x384 (4x) or 816x768 (8x) looks best |
| `thumbnail` | a PNG for the small picture alone, when the scaled-down `art` does not read well at 40x32; bigger than 40x32, it is drawn at its own resolution too |
| `field_art` | a PNG (same sizing rule as `art`) for the 3D Monsters mod's Card art style cutout alone, on top of the card on the duel field: never patched into the card's own record, so the Library, hand, trade screen and detail panel keep showing `art` (or the base's own picture) untouched. Without one the cutout shows the same picture everything else does |
| `title` | a PNG for the name plate at the top of the card's picture (96x14; dark ink on white, or on a transparent background). Without one, a card with its own name gets a plate with that name set in Times at the retail plates' size (Times New Roman on Windows, fontconfig's match for `Times` elsewhere, Liberation Serif on most Linux systems), or a blank plate when there is none |
| `attack`, `defense` | 0 to 5110, in tens, as the game stores them |
| `type` | a number or a name (`"Dragon"`, `"Winged Beast"`). A copy may become Magic, Trap, Ritual or Equip when `effect` selects a retail behavior of that type. Without a matching effect it keeps its original kind; any copy may become a monster, as a replaced card may (below) |
| `trap_threshold` | the highest attacker ATK stopped by this card's attack-destruction trap effect, inclusive, 0–65535. Applies only to a Trap with an effect from House of Adhesive Tape through Widespread Ruin. Omitted: keep the earlier replacement's override, or inherit a copy's base override; `null`: clear it and use the effect's global `trap_thresholds` setting or retail default. Other cards sharing the effect keep their own thresholds |
| `attribute` | a number or a name (`"Light"` to `"Wind"`) |
| `level` | 0 to 12 |
| `stars` | the two guardian stars, as numbers or names (`"Mars"` to `"Venus"`, and a mod's own up to 15: [Guardian Stars](modding.md#guardian-stars-names-icons-new-stars-and-matchups)); a second of `0` (none) or the same as the first is a card with one star |
| `frame` | the color of the card's frame, whatever its type: `"Gold"` (the disc's `"Monster"`), `"Green"` (`"Magic"`), `"Pink"` (`"Trap"`), `"Blue"` (`"Ritual"`), `"Purple"` or `"Orange"`, or a number 0-5 in that order; `"Type"` goes back to its type's. Left out, a monster with `monster_effects` is orange ([below](#frame-color)) |
| `tags` | words for the card, a list such as `["god"]` (31 letters at most; 32 different in all). They mean nothing to the game: a [card layout](modding.md#card-layout-repositioning-or-hiding-the-big-card-display) may give each tag a frame of its own. A copy has its base's unless it says; `[]` clears them ([below](#tags)) |
| `fusion_groups` | the fusion guides' groups the card is in, for a ritual's `fusion_group` condition ([Gameplay tables](gameplay-tables.md#rituals)): a list such as `["Elf", "Female"]`, `[]` for none; without it, its base's |
| `drops` | whether the card can be won in its base's place (default `true`, below) |
| `opponents` | whether an opponent's deck can be dealt it in its base's place (default `false`) |
| `password` | the password used by the Password shop and shown on the card by View > Card passwords: up to eight digits as a string (`"08124921"`, leading zeros kept) or a number, `""` or `null` for none. The [gameplay tables'](gameplay-tables.md#passwords-and-prices-on-the-password-screen) `passwords` entry overrides it and sets the price, for added cards as well as replacements. Added cards default to 999999 starchips and allow repeat purchases. A copy without a password has none (its base's would give the base) |
| `monster_effects` | what the monster does on the field: on summon, on flip, at its owner's draw, before a battle, when destroyed, or while face up ([below](#monster-effects)). Left out, a copy has its base's and a replace keeps an earlier one's; `[]` takes them away |
| `notes` | text of the modder's own, which the game shows and plays by none of ([below](#notes-on-a-card)) |

What an entry leaves out is its base's. Give entries explicit stable `id` keys. Saves use these identities; runtime
IDs are remapped when mods change. Legacy numeric sidecars require explicit
migration as described in [Mod API 3](mod-api-3.md#card-definitions-and-identities).

The mods' `cards` are read in load order, as every other table is
(`priority`, then `after` and `requires`, then the order the mods were
found; the Mods window's Load order), and each mod's entries in the order
they are written. The runtime ids follow each other in that order, so
moving a mod in the load order renumbers the cards added after it for the
run: saves and deck slots keep them by identity (above), and a save state
made with another order is refused, as for any change of the mods' order
([Mod API 3](mod-api-3.md#save-states)). The stats a copy leaves out are its
base's as the entries before it left them: a copy of a card that an earlier
mod in load order replaces has the replacement's stats, a later mod's
replace does not reach them. A name, text or picture it leaves out is its
base's as the game ends up with it, later mods' replaces included. A mod
with `cards` needs a restart to apply or remove, like a data override: the
cards are counted once, when the game starts. The window and the log
(`MEMORIES_TRACE=mods`) say which ids each entry got.

## Changing a card of the disc

`"replace": <id or name>` in place of `copy` changes the retail card itself,
so a mod can rework the existing cards without adding any:

```json
{ "replace": 1, "name": "Bulbasaur", "art": "images/bulbasaur.png",
  "description": "A strange seed was planted on its back at birth.",
  "type": "Plant", "attribute": "Earth", "attack": 1180, "defense": 1150 }
```

It takes the keys above that change what a player reads off the card:
`name`, `description`, `art`, `thumbnail`, `title`, `field_art`, `attack`, `defense`,
`type`, `attribute`, `level`, `stars`, `frame` and `password` (without one it shows
the disc's). It gets no id of its own, so `id`,
`count`, `count_setting`, `drops`, `opponents` and `fusions` do not apply:
the card keeps its place in the disc's tables, and the
[gameplay tables](gameplay-tables.md) change its fusions, equips and rituals.

A replaced card may change sides with `type`: a magic, trap, ritual or
equip card can become a monster, and a monster a magic, trap, ritual or equip
card. A copy may become a monster the same way (a copy of Raigeki made a
Dragon); it becomes a non-monster only as the `type` row above says. The disc has 3D models for its monsters only, so a card
made a monster fights without one unless `model` names a monster whose model
it takes (`"model": "Kuriboh"`), and without `stars` it gets that monster's
guardian stars, or the Sun and the Moon. A monster made a magic, trap or
ritual card does nothing when played unless `effect` names the card whose
effect it takes (`"effect": "Raigeki"`). A monster made an equip needs no
`effect`: it adds +500 ATK and DEF, or what `equips` gives it (`bonus`,
`bonus_attack`, `bonus_defense`), to the monsters `equips` lets it equip,
and a copy may become one the same way. An equip whose `effect` is a disc equip fits
the monsters that one fits, with its bonus (Megamorph's +1000 too), and
`equips` in the [gameplay tables](gameplay-tables.md) may change that. `model`
and `effect` work the same on a card that stays on its side, and a copy's
`model` and `effect` may name the card as a replace's do. `model` may also
name a battle model a mod adds ([Battle models](modding.md#battle-models-monsters-the-disc-does-not-have),
API 21).

The CPU plays a magic card as the disc's card its `effect` names, when that is
a magic card too: its scripts look cards up by the disc's numbers, so a mod
card whose effect is Raigeki is played when Raigeki would be, and one whose
effect is Umi when Umi would be. An effect of another kind (a trap's on a
magic card) is never played by the CPU. The FM Editor's Cards tab offers only
the disc's cards of the card's own type in its **Effect** list.

A trap card springs as the trap its `effect` names (`"effect": "Bear Trap"`,
stopping the attacks `trap_thresholds` gives Bear Trap), else as its own. A
card made anything but a trap springs as no trap, even one that was a trap on
the disc.

A card that changes kind (monster, magic, trap, ritual or equip) leaves the
disc's fusion and equip tables, which describe the card it was: it fuses and
equips only by the mods' own rules, and no disc recipe makes it. Otherwise the
CPU would plan with the old card, fusing a monster with what is now a magic
card or taking a monster off the field for what is now an equip, and lose the
cards. An equip made from another kind equips nothing until `equips` says
what, or its `effect` names a disc equip. A magic, trap, ritual or equip card has no ATK or DEF, so a monster made
one loses its own, and `attack` or `defense` on it is noted and left out: the
CPU ranks the cards in its hand by them whatever their type, and would set it
face down turn after turn as its best monster.

A replaced piece of Exodia (cards 17 to 21) is an ordinary card: a deck may
hold three of it, and Exodia can no longer be assembled. `"exodia": true`
keeps both rules, for a mod that only changes how the pieces look. Nothing of it goes in the save, so the mod can be
removed at any time (after a restart). A card with a name of its own gets a
plate that says it, as an added card does (a `title` PNG replaces it), and
the HD text renderer sets its title from the same name. Its name and text
win over a [translation](translation.md)'s, and the Library and Build Deck
sort it by the new name. Copies of it that set no name, text or art of their
own show the replaced ones.

When two entries (or two mods) replace the same card, the later one goes over
the earlier: of two mods, the later in load order, whatever their folders
are called. Its stats, stars, level, attribute, type, frame, model and
effect go over the earlier one's where it gives them, and what it leaves out
of those stays as the earlier one set it. Its name (with the plate that says
it), text, password, art, `title`, `field_art` and `fusion_groups` take the
earlier one's place whether it gives them or not: left out, the card's own
come back. Notes add up ([below](#notes-on-a-card)). The Mods window names
the mod whose keys are used and the one whose are dropped
([When mods overlap](modding.md#when-mods-overlap)).

## Frame color

The game draws a card's frame through a palette its type picks: gold for a
monster, green for magic and equip, pink for a trap, blue for a ritual.
`"frame"` picks one of those for the card whatever its type, or one of the
two the disc has and never uses, purple and orange:

```json
{ "replace": "Blue-Eyes White Dragon", "frame": "Purple" }
```

Only the color changes: a monster keeps its ATK/DEF and a magic card its
MAGIC word. Left out, a replaced card keeps the frame an earlier mod gave
it and a copy takes its base's; `"Type"` goes back to the type's
(`Cards_FrameColor`, `src/pc/cards/cards.c`). A monster with
[monster effects](#monster-effects) and no frame of its own is drawn
orange, as an effect monster is in the card game; `"Monster"` keeps it
gold, and `"Type"` gives it its type's. The FM Editor's **Frame** list
names both (**Monster (gold)**...) and writes the disc's names, which builds
older than the color names read too. It shows everywhere the game
colors a card by its type:

- the card view (Library, Build Deck, Trade, Password, the duel's card view,
  fusions, rituals and cards being played): `func_800291E0`, palette rows
  8-13 of each package's card-frame sheet;
- the duel's hand and its 2D field cards (`func_80016784`) and the 3D field
  cards (`func_80024C1C`, drawn by `func_80015EF4`): rows 1-6 of the duel's
  hand-frame palette;
- the Library's grid (`library_runtime.c`), whose small cards have a
  palette per frame at x 0x160-0x1B0;
- the 16x16 card icon in Build Deck (`func_80031574`) and Trade
  (`MainMenu_DrawCardTypeIcon`): its package has orange (x 0x2A0) but no
  purple, so a purple card keeps its type's icon there.

The Forbidden Memories HD mod's frames cover purple and orange too
(`tools/pc/hd_assets_pack.py` recolors the monster frame into them).

### Named frame styles

`"frame_style"` picks a named style from an applied `card_layout` mod's
`frame_styles`.  This is how a card mod can use a newly added frame without
pretending it is one of the game’s six palette colors or adding a tag only to
select art:

```json
{ "replace": "Blue-Eyes White Dragon", "frame_style": "godly" }
```

Define the matching style with both images when the duel hand should use a
custom frame too:

```json
"godly": {
  "image": "frames/godly.png",
  "hand_image": "frames/godly_hand.png",
  "library_icon": "frames/godly_library.png",
  "hand_color": "purple"
}
```

The selected style controls the big-card image and its small-card palette
color (`hand_color`), plus its optional flat duel-hand image (`hand_image`). An
explicit card style wins; otherwise selection falls through printed type, kind,
a style named for its `frame` color, the layout's `frame_for` rules and
`default_style`, then retail.
A copy inherits its base's named style; a later
replacement keeps it when left out, and `null` clears it back to the layout
rules. The named style must exist in the active layout; otherwise the game
uses its retail frame and notes the missing style in the Mods window.

## Tags

`"tags": ["god"]` on a card entry labels it, so that a mod that draws card
frames (`card_layout`'s `frame_for`, [modding.md](modding.md#frame-styles-and-the-rules-that-pick-them))
can give it a frame of its own without the card naming a color: the cards mod
says *what the card is*, the layout mod says *how that looks*. Tags are free
words (letters, digits, `-`, `_`); a card may have any number of them. There is
no tag the game itself reads; the anime frame's own classes (`monster`,
`effect_monster`, `spell`, `equip`, `ritual_spell`, `trap`) are worked out, not
tagged.

The [FM Editor](../tools/pc/fm_editor/README.md)'s Cards tab edits them as the
card's **Tags**, comma-separated: blank leaves the key out (a copy has its
base's), `[]` writes none. Its checks warn of a tag the game would leave out.

## Card text codes

A card's `name` and `description` (and any text in a mod's text listing)
may hold two codes besides letters. The FM Editor's card text box inserts
both from its right-click menu, with the icons and colors shown, so there
is no need to look them up.

`{f8 0B XX}` inserts an icon. The card view draws it 16 pixels across, two
letters wide, and the port wraps the text with that width. `{f8 0B 00}` is the
Dragon icon:

| XX | Icon | XX | Icon | XX | Icon |
|---|---|---|---|---|---|
| `00` | Dragon | `01` | Spellcaster | `02` | Zombie |
| `03` | Warrior | `04` | Beast-Warrior | `05` | Beast |
| `06` | Winged Beast | `07` | Fiend | `08` | Fairy |
| `09` | Insect | `0A` | Dinosaur | `0B` | Reptile |
| `0C` | Fish | `0D` | Sea Serpent | `0E` | Machine |
| `0F` | Thunder | `10` | Aqua | `11` | Pyro |
| `12` | Rock | `13` | Plant | `14` | Magic |
| `15` | Trap | `16` | Ritual | `17` | Equip |
| `18` | Mars | `19` | Jupiter | `1A` | Saturn |
| `1B` | Uranus | `1C` | Pluto | `1D` | Neptune |
| `1E` | Mercury | `1F` | Sun | `20` | Moon |
| `21` | Venus | `22` | Cross | `23` | Triangle |
| `24` | Square | `25` | Circle | `26` | Star |
| `27` | Start (left half) | `28` | Start (right half) | | |

`{f8 0A XX}` changes the color of the text after it; `{f8 0A 00}` goes back
to white:

| XX | Color | XX | Color |
|---|---|---|---|
| `00` | White (the default) | `04` | Grey |
| `01` | Yellow | `05` | Orange |
| `02` | Blue | `06` | Red |
| `03` | Green | | |

```text
{f8 0A 05}<Effect>{f8 0A 00}
Cannot be destroyed by card effects.
This card can attack {f8 0A 06}2x{f8 0A 00} during the same turn.
```

The pictures come from the disc. Every icon is a 16x16 cell of the boot
package's second font page (`WA_MRG.MRG` sectors 0x16A0-0x16AF), and the
palettes are at sector 0x16C0 (`func_80035E20`). In card text, 14-17 are the
pictures the card lists show for magic, trap, ritual and equip cards. The
same function has another path for them that draws the 32x16 words MAGIC,
TRAP, RITUAL and EQUIP (an 8-bit page at sector 0x16E6), but card text does
not take it. The colors are the text ramps (`gText_abColorSlots`) at sector
0x16C2. `tools/pc/fm_editor/card_text.py` reads all of them.

## Spell and trap effects

`"card_effects"` gives a Magic or Trap card data-defined work when it is
played or springs.  Its entries use the same `do`, `card`, `attack`,
`defense`, `amount`, `target`, `type`, `attribute` and `for_each` values as
a [monster effect](#monster-effects), but have no `when`: the card itself is
the occasion.  A `heal` or `damage` targets `own` or `opponent` (whose LP
changes); a `boost` or `destroy` may also target `all`.  When `target` is
left out, `damage` and `destroy` take the `opponent`, `heal` and `boost` its
owner (`own`).  No spell or trap can target `self`, `others` or `battle`,
because it is not a monster on the field; an entry that does is left out
with a note.  `"card_effects"`, `"card_effects_mode"` and `"ai_effect"`
(below) work only on a Magic or Trap card: on any other card they are refused
with a note, and a copy made a monster drops the ones its base had.

```json
{ "replace": "Hinotama", "card_effects_mode": "replace",
  "card_effects": [ { "do": "damage", "amount": 800 } ] }
```

`"card_effects_mode"` is `"add"` by default, which keeps the normal card
effect (including one selected by `"effect"`) and then runs the listed
effects.  `"replace"` takes the normal spell effect away and runs only the
data-defined list.  Copies inherit both fields from their base unless they
give their own list.  `"effect"` remains the way to copy a retail spell or
trap behavior.

The scripted CPU cannot infer a new effect's strategy from its text. A
`"replace"` card is therefore not treated as its old retail behavior. Give
it `"ai_effect": "Hinotama"` (a retail card of the same type) when that
card is the appropriate tactical analogue; its scripts then find, value and
play the custom card as that effect. Without it, the CPU's card lookups and
valuation never take a replacement for a retail spell/trap effect, rather
than using it as the wrong one. Card types remain authoritative, so spells
and traps never enter monster fusion or attack selection.

## Monster effects

`"monster_effects"` gives a monster things it does on the field. Each entry
says when (`when`) and what (`do`):

```json
{ "replace": "Blue-eyes White Dragon",
  "description": "{f8 0A 05}<Summon>{f8 0A 00} Raigeki.",
  "monster_effects": [
    { "when": "summon", "do": "magic", "card": "Raigeki" },
    { "when": "face_up", "do": "boost", "target": "others", "type": "Dragon", "attack": 300, "defense": 300 },
    { "when": "combat", "do": "boost", "target": "battle", "attack": -500 },
    { "when": "destroyed", "do": "damage", "amount": 800 }
  ] }
```

Man-eater Bug as on the cards:

```json
{ "replace": "Man-eater Bug",
  "description": "{f8 0A 05}<Flip>{f8 0A 00} Destroys the monster that attacked it.",
  "monster_effects": [ { "when": "flip", "do": "destroy", "target": "battle" } ] }
```

| `when` | It happens |
|---|---|
| `summon` | the monster is put on the field face up: played from the hand, a fusion's result (onto a field monster too) or a ritual's monster. A face-down play is no summon. The CPU puts a monster with a `summon`, `draw` or `face_up` effect down face up (one with a `flip` and no `summon` face down), so its effects work for it too |
| `flip` | a face-down monster is attacked. As in Yu-Gi-Oh!, the flip does not stop the battle: the damage is worked out with the cards as they are, then the flip resolves right after the battle, even if the battle destroyed the flipped monster, and before any `destroyed` effect of the battle. Not when a trap stops the attack. Attacking while face down, or a reveal (Dark-piercing Light), is no flip; an equip leaves a face-down monster face down |
| `draw` | its owner's turn begins, once the hand is drawn, while it is face up |
| `combat` | it attacks or is attacked, before the damage, when no trap springs |
| `destroyed` | it leaves the field in a battle or to an effect. Not when it is used as fusion material, a ritual's tribute, or the field is cleared by an Exodia win |
| `destroy_opponent` | it won a battle that destroyed the other monster and is still on the field: after the battle, after that monster's `destroyed` |
| `face_up` | all the while it is face up on the field: a boost that goes as soon as the card does |

| `do` | It does | With |
|---|---|---|
| `magic` | what a magic card of the disc does, as the monster's owner would play it, on either side's turn. One of the 33 with an effect of their own: Stop Defense, Dragon Capture Jar, the six fields, Dark Hole, Raigeki, the five LP recovery and five damage cards, Swords of Revealing Light, Spellbinding Circle, Dark-piercing Light, the type removals (Warrior Elimination ... Eternal Draught), Cursebreaker, Shadow Spell and Harpie's Feather Duster. Not a ritual, an equip or a trap | `card`: its number or name |
| `boost` | ATK and DEF up (or down, below 0) | `attack`, `defense` (-9999 to 9999); `target`; `type` and `attribute` keep it to monsters of that type or attribute |
| `heal` | its owner gains LP, up to the duel's starting LP as the recovery cards do | `amount` (1 to 9999) |
| `damage` | the other side loses LP | `amount` (1 to 9999) |
| `destroy` | the other side's monsters are destroyed, shown as Crush Card shows it | `target`: `opponent` (the default) or, on `flip`, `battle` (the monster attacking it, the default there); `type` and `attribute` keep it to those |
| `change_field` | changes the active duel field and immediately recalculates every monster's terrain bonus | `field`: a retail or declared field id or name. It cannot run every frame (`face_up`) or during combat |

`target` is `self` (the default), `own` (its side's monsters, itself too),
`others` (its side's other monsters), `opponent`, `all`, or `battle` (the
monster it battles: on `combat`, and on `flip` the monster that attacked it, if it is still there);
`destroyed` defaults to `own`. A boost
on `summon`, `flip`, `draw`, `destroyed` or `destroy_opponent` lasts as an equip's does (on the
monsters on the field then), one on `combat` lasts the battle, and one on
`face_up` is worked out whenever the game asks a card's ATK and DEF, so the
field, the battle, traps and the CPU all see it: it reaches each monster it
names once, those put down later too, and goes with the card. `face_up` takes only
boosts; `combat` takes a boost of `self` or `battle`, `heal` and `damage`.
`for_each` makes a `boost`, `heal` or `damage` once for each face-up
monster it counts, on any `when` that takes the effect; the count is
taken when the effect is made, and a `face_up` boost follows it as it
changes (a monster summoned, flipped, fused, destroyed):

```json
{ "replace": "Baby Dragon",
  "description": "Gains 300 ATK and DEF for each face-up Dragon on your field.",
  "monster_effects": [
    { "when": "face_up", "do": "boost", "target": "self", "attack": 300, "defense": 300,
      "for_each": { "whose": "own", "type": "Dragon" } } ] }
```

`whose` is `own` (its owner's monsters, the card itself too when it
matches), `opponent` or `all` (the default); `type` and `attribute` keep
the count to monsters of that type or attribute (neither: every face-up
monster). A face-down monster is not counted (its type is hidden) until it
is turned face up; the two monsters of a battle count as face up from its
start. With none counted the effect does nothing; `magic` and `destroy`
take no `for_each`. The total is held within the stat cap as every boost is.

A card has up to eight effects, which resolve in the order written, after
whatever triggered them is over (a flip's and a destroyed monster's after the battle).
What the game cannot do is left out with a note in the log and the Mods
window. Write what the effects do in the card's `description`: the game
shows only the text. The card is drawn with the orange frame unless its
entry has a `frame` ([Frame color](#frame-color)).

Each fires once for each time it happens: a summon once, a draw once a
turn, a flip once (a card is put face down only from the hand). A chain is fine (a summon's Dark Hole
destroys a monster whose `destroyed` effect fires next), and the CPU's
monsters' effects fire as the player's do. Apart from putting such
monsters down face up, the CPU plays as it always has: it sees the boosts
in its view of the board, but it does not plan around a card's effects.

The [FM Editor](../tools/pc/fm_editor/README.md#monster-effects) edits
them in the Cards tab. A [code mod](modding.md#code-mods) hears every one of
these occasions, for every monster, from mod API 10 with
`MEMORIES_EVENT_MONSTER` (`a` the card, `b` its duel record, `c` what
happened, `MEMORIES_MONSTER_SUMMON` to `MEMORIES_MONSTER_DESTROY_OPPONENT`; before:
`handled` skips the card's own effects for it, and a mod may start a card
effect of its own there).

How it is done (`src/pc/cards/monster_effects_duel.c`): the duel has no
single place where a monster is summoned, flipped or destroyed, so the
monster zones are looked at as each hand or field phase begins, before its
first step, with no card effect running. Every way back to those phases
starts one. What changed since the last look is what happened: placement
marks the zones it put a card in (`func_8001B170`, the ritual), and a
ritual's start marks its tributes; the battle's start notes its two
monsters, so the next look sees which one won, and a face-down defender
(its saved flags, `D_8009B178`, when no trap sprang), whose flip the next
look fires before anything else. The CPU's face-up play is
its hand play's face-down choice (`D_800EAE88[8]`) cleared. A `destroy`
runs Crush Card's removal (`DuelEffect_ApplyMonsterRemoval`)
with the monsters chosen in place of its ATK test. A `magic` effect runs through the game's
card-effect dispatch (`DuelEffect_StartCardEffect`), first handler then
second, as playing the card does. On the other side's turn, the turn is
lent to the owner (`D_8009B1D5`, `D_8009B1C8`, `D_8009B22C`) while it runs.
The state is a game variable (`src/pc/game/trigger_state.c`), so save states
carry it.

## Notes on a card

`"notes"` is the modder's: what was changed and why, what is still planned.
The game draws none of it and plays by none of it. The
[FM Editor](../tools/pc/fm_editor/README.md) shows it as the card's **Notes**
box. A `replace` entry with nothing but `notes` (and an `id`) only notes the
card, so a mod can tag the disc's cards, or another mod's changes, without
changing them:

```json
{ "replace": "Dark Magician", "notes": "The main boss card. <burn: 300> <no-fusion>" }
```

A card's notes add up: every entry that gives the card `notes`, in every
applied mod, in load order, a line between two. A later entry that replaces
the card again does not take an earlier one's notes away.

A [code mod](modding.md#code-mods) reads them, from mod API 7, with
`host->card_notes(host, id)` (the whole text, or `NULL`) and
`host->card_tag(host, id, key, out, size)`, which reads tags the way RPG
Maker reads its note boxes:

| Written | `card_tag` of it |
|---|---|
| `<burn: 300>` | `"burn"` gives `"300"` (returns 3, its length) |
| `<no-fusion>` | `"no-fusion"` gives `""` (returns 0) |
| no such tag | returns -1, `out` is `""` |

A tag's name is anything but `<`, `>` and `:`, and any case matches; the
value runs from the first colon to the `>`. Spaces around either do not
count. Of two tags with one name the last counts. Everything outside the
brackets is comment. `out` is cut to fit `size` and always ended; the return
is the value's whole length, as `snprintf`'s, so a longer one can be asked
for again. What the value means (a number, a card name, an identity) is up
to the mod that reads it:

```c
char value[16];
if (host->api >= 7 && host->card_tag(host, card, "burn", value, sizeof(value)) > 0)
    burn = atoi(value);
```

The notes are known once the card tables are built, before the title screen.

## Where a card comes from

The disc's reward and deck tables are of retail cards, and their weights add
up to 2048, so they are not extended. A copy that asks to (`drops`) is won in
its base's place instead: when a duel's reward roll lands on the base, the
game picks evenly among the base and every such copy. The base's family is
won exactly as often as the base was, and without such copies the game's
random numbers go exactly as before. `opponents` does the same for the cards
an opponent's deck is dealt. Starter decks and passwords are the disc's, so a
new card is never in a starter deck and has no password.

`MEMORIES_DEBUG_CHEST=3` (every card, three copies, new ones included) and
`MEMORIES_DEBUG_DECK="723-762"` (the deck, as ids and ranges) are the
development shortcuts; see [pc-build.md](pc-build.md).

## How it is done

The console build is untouched: every change to shared game sources is under
`MEMORIES_PC` or is a named constant that expands to the same number there,
and `make match` and `make match-overlays` reproduce the retail hashes.

**The count.** `CARD_COUNT` stays 722 everywhere the disc is laid out by it (the
art sectors are `(id - 1) * 7 + 722`). `CARD_COUNT_LIVE` (`gCard_nCount`) is
how many cards this run has, and bounds the loops. `CARD_TABLE_COUNT` (32766)
sizes the tables and workspaces indexed by card id
([`card_constants.h`](../src/game/card_constants.h)).

**The tables.** `gDuel_adwCardStats`, `gCard_asNameSortKey` and
`gDuel_abCardLevelAttr` are packed back to back in the executable with room
for 722 cards. The port defines them in
[`src/pc/game/card_storage.c`](../src/pc/game/card_storage.c), a game-side
unit, so the build stops pinning them to the retail addresses, and
`Cards_Build` ([`src/pc/cards/cards.c`](../src/pc/cards/cards.c)) copies the
retail tables in and appends the mods' cards at startup. The same unit holds
everything else that outgrew its console storage: the Library's per-card
display records (`D_800EA1E8`), the Build Deck workspace (two 0x6344-byte
panes at `payload_bases[0]` on the console, `BUILD_DECK_WORKSPACE`), the
trade screen's rows (`D_801845FC`) and the Library cursor, widened from `s8`.
Being game variables, they sit at fixed addresses inside every save state,
which is why a state is now about 7.5 MB.

None of the MIPS code the port runs under its interpreter (the WA effect
bank, the MODEL control modules) addresses these tables: a scan of the WA
bank and all of MODEL.MRG for `lui`-formed addresses found none in
`0x800EA1E8`-`0x800EAD88` or `0x801A0000`-`0x801E0000`.

**The base.** `Cards_BaseId` stands in for the card wherever the disc or a
disc table is behind the id: the big artwork (`func_80029164`), the small
field and hand images (`Duel_RequestCombinedDeckData` reads one sector per
unique *base*, and `Duel_PopulateCombinedDeckData` finds each card's by its
base), the 3D model (`Model_LoadMonsterMerge` from the Library, the battle's
`D_800EF658`: a copy's id could otherwise be 777, `MODEL_SPECIAL_BATTLE_ID`,
Exodia's), the card text (`duel_effect_command.c`), the fusion and equip
tables (`duel_card_checks.c`; an equip answers with the card it was asked
about), rituals (`Duel_CheckRitual`), card effects
(`DuelEffect_StartCardEffect`, so the handlers that test
`gDuel_wEffectCardID` see the base), traps (`Duel_SelectTrapByCardId`,
`duel_trap_resolution.c`) and the AI's damage-card values.

**Ids beside flags.** Build Deck's box rows and the placement result keep a
flag in bit 15 and read the id back with `& 0xFFF`; `CARD_ID_FIELD_MASK` is
`0x7FFF` on the PC port. Card ids themselves are 16 bits everywhere else.

**The trunk and seen marks.** The save's trunk is 722 bytes at +0x50 with 18
bytes to spare before the duelist code, and the Library's seen marks are
campaign flags `0x120 + id`, which reach the password flags at 0x400. So the
new cards' quantities and seen marks are kept by the port: `Cards_ChestSlot`
is the trunk byte of a card in a given save (the running one, the two a
two-player screen loads, or the copies a trade is made on), and
`Cards_Seen`/`Cards_MarkSeen` the seen mark. Every place that reached the
trunk directly goes through them: Build Deck's setup and exit, the Library,
`Library_CheckCardOwned`, `Duel_AwardCard`, the recent-drop compaction,
the trade screen and its commit.

**Beside the save.** A memory card block has no room for them either, so
`cards/<duelist code>.txt` in the user directory holds them, a section per
save sequence (`save <n>`, then `chest2 <identity> <count>`, `seen2 <identity>`,
`deck2 <slot> <old-id> <base> <identity>` and `recent2 <index> <old-id> <identity>`
lines, then `end`), the newest eight kept. The
game's own save writes one (`SaveData_RequestWrite`, under the sequence the
payload gets), a load reads the one for the loaded sequence
(`SaveData_PollLoad`), a two-player load reads both saves', and a trade
rewrites both once the memory cards took it. NEW GAME is noticed by the
running save's duelist code changing. A save made while no card mod was
applied has no section, and gets the newest earlier one: what the player had
when they last played with the mod. A deck that holds a card the run does not
have (the mod was removed) gets each such slot's base back, from its `deck2`
line, so a duel never deals a card that is not there. Ownership and seen records
for missing mods are retained; returning mods recover them by stable identity.

**The recent drops.** The save's list of the last 16 cards won
(`gDuel_awRecentCardDrops`, save offset +0x5BC) gives Build Deck its "New!"
mark and its sort by new, and holds card ids. A section has a
`recent2 <index> <old-id> <identity>` line for each entry past the disc's
cards (`-` for a card the run did not have when it saved), and a load puts the
card of that identity back wherever the list holds the old id, or none when
its mod is missing; so after mods are added, removed or reordered, "New!" stays
on the card that was won. The old id alone is the key: a section's `recent2`
lines are one numbering, that of the run that last wrote the list (after a
trade its `deck2` lines may be a later run's), so an old id names one card
wherever it stands in the list. A run with no card mod has none of these
cards: its load clears their entries, and a save it makes keeps no "New!" on
them. A section from before `recent2` has no such lines, and its
list is left as it is: those ids are right as long as the mods are, and
zeroing them would wipe right marks on the first load. A trade writes the
save's first 0x400 bytes only, which do not hold the list, so its rewrite of the
section keeps the `recent2` lines as they were. The disc's ids (722 and below)
never change.

The other fields of the 0x680-byte save state hold no card ids that need
this. The deck has its `deck2` lines; the trunk (+0x50) and the Library's seen
marks (campaign flags `0x120 + id`) are indexed by id and cover the disc's
cards only, the added cards' being the `chest2` and `seen2` lines; the named
rest is the duelist code, the sequence, the name, the campaign flags, the win
and loss totals and the Free Duel records (wins and losses by opponent), the
scene index, the sound output and the starchips. The card a duel drops is kept
in the result record (`DuelResultDisplayState.dropped_card_id`), not in the
save. A save written in game after a deck of added cards won one more held
their ids at the deck (+0x00..+0x4F) and at +0x5BC only, nowhere else in the
0x680 bytes; the unnamed bytes (+0x322..+0x333, +0x338..+0x3FF, +0x5B8..+0x5BB
and the tail from +0x628) were all zero.

**Its own art and text.** The game still loads the base's art record
(`func_80029164`) and the base's thumbnail sector
(`Duel_RequestCombinedDeckData`); `Cards_PatchArtRecord`, called in
`func_800289BC` before the four uploads, and `Cards_PatchThumbnail`, called in
`Duel_PopulateCombinedDeckData` after each block is copied, lay the card's own
picture, plate and thumbnail over them
([`art.c`](../src/pc/cards/art.c) makes them from the PNGs: the record layout
is in its header comment and in
[modding-tutorial-evidence.md](modding-tutorial-evidence.md#card-image-editor-dimensions)).
The plate is drawn subtractively over the gold frame through its own fixed
palette: entry 1 takes the most away and is the darkest ink, 7 barely shows,
0 is clear. The retail plates put their stems at 1 with faint 6 and 7
fringes, and a plate that inks with 7 reads as a pale ghost. The generated
plates follow the settings the
[YuGiOhForbiddenMemoriesRecomp](https://github.com/yamyi/YuGiOhForbiddenMemoriesRecomp)
project measured against window captures (its `psx_card_packs.c`,
`render_title`): Times
regular at 13 pixels, the baseline under row 11, whole-pixel advances,
and a name wider than 90 pixels squeezed into columns 3 to 93 and brought
back up to full ink. Each texel takes the ink of the nearest tone, from
what each ink was measured to take from the gold in the game (`ink_of`);
a retail card a translation renames gets its plate the same way
([translations](translation.md#how-the-port-does-it)). The name is read as UTF-8, as its glyphs are, so an
accented letter is one character on the plate too; one the font lacks is
left out. An entry's cards share one plate unless the name has `{n}` or
`{id}` in it. A patched picture and thumbnail are reported written
(`TextureDump_Written`), so a texture pack's picture of the base card does
not show through on the copy in words that happen to match it; the plate
is not, since that write would drop the delivery of the sector that also
ends the base's palette. Card text goes in beside the name, at the text
engine's insert command (`duel_effect_command.c`, op 0x40).

<a id="hd-pictures"></a>**HD pictures.** A texture pack finds its images by where their
bytes came from on the disc, and a mod card's picture comes from no place on
the disc, so the port registers the PNG itself: for an `art` or `thumbnail`
bigger than the console's picture, `Cards_Build` hands the record's made
bytes (the picture, 51 words by 96 rows at 8 bits, and its 256-entry CLUT;
the thumbnail, 20 by 32, and its 64 entries) and the PNG's crop (the same
middle `resample` takes, `CardArt_Crop`) to `TexturePack_AddMade`. Each
distinct block of bytes gets a place of its own from `TEXTURE_MADE_BASE`
(0xC0000000, past any CD) up, and the pack's recall index knows it by its
bytes: the upload of a patched record has no disc provenance (the patch
reported it written), so `TextureDump_Loaded` asks `recall`, which checks
the whole block, and the words are tagged with the made place. From there
it is a pack entry like any other: painted into the shadow, resampled at
1x, sampled at its own resolution above it by both renderers, found again
after a state load (`rediscover` compares against the made bytes). A made
block's first 32 bytes may be one value (a plain sky); only a block of one
value throughout is left out, as a fill would match it anywhere. Made
entries are kept apart from the packs' and joined to them again whenever
the packs are unloaded or reloaded, so enabling or disabling a texture pack
in the Mods window keeps them. Entries with the same art share one entry;
two pictures with the same palette share the palette's place. Nothing in the
mod's manifest changes: a mod written for 0.1.2 gets HD pictures from the
PNGs it already has, and one at 102x96 draws as before.

**What spells out 722.** The Library's heading string (`"<seen/722>"`,
0x801B121D, text 0xF8) is replaced by the port's own for the real total, in a
box wide enough for it. It is found by its id, so a translation's heading is
rewritten the same way: its "722" becomes the total and its count's width
grows to match (`Cards_Text`). Nine 16-pixel letters fill the console's
box, and a heading that wraps would wait for a page press (the port now
leaves out what does not fit). Three-digit card numbers
(`F8 03` with width 3, Build Deck's list, the trade offers) grow to four or
five digits in the same room. Build Deck's list and the trade screen's scroll
end at the live count (`maximum = 715` was 722 - 7).

**The Library grid.** Its panels are sprite sheets (resource 0/3/n of the
Library package): a stone frame around two rows of big digits that spell
"001" over "100" to "701" over "722". The digit pieces only spell those
ranges (there is no 8 or 9 among them), so with more cards the sections from
701 on get a panel of the port's own
([`library_panels.c`](../src/pc/cards/library_panels.c)): the same frame from
the same texture page, with the plain stone band from between the retail
numbers where the numbers were, drawn through the game's sprite-sheet
renderer for the section rows on screen. The grid itself grows by section
rows of 200 (`CARD_GRID_SECTION_ROW_COUNT`).

## Limits

* Ids stop at 32766 (`CARD_ID_LIMIT`): card ids are signed 16-bit in the
  duel's records and 0x7FFF-masked beside their flags.
* A card defaults to its base's 3D model and effect, with independent `model`
  and `effect` borrowing available. Declarative recipes and code hooks extend
  fusion behavior; equips and rituals use the base's entries unless a mod's
  `equips` or `rituals` rules name the copy ([gameplay tables](gameplay-tables.md)).
  A ritual is still asked for by the retail ritual card whose effect it is.
* The generated name plate is set in a system font, not the retail plates'
  own lettering; a `title` PNG replaces it.
* Copies of Exodia's pieces do not complete Exodia, and Build Deck's one-copy
  rule for the pieces is by id: a copy is another card.
* Legacy numeric sidecars require the original card mods and order for an
  explicit migration. See [API 3 migration](mod-api-3.md); new sidecars use stable identities.
* The Library's panels past section 7 carry no range numbers; the card number
  under the cursor is always shown.

## Checking it

With a save on the memory card (`tmp/morecards` has the harness used): the
Library heading, the grid's last section row and the card view (art, text,
model) of a new card; Build Deck at the end of the chest with 4-digit ids,
adding a new card, saving, and loading it back in a fresh process with the
counts intact; a Free Duel with a deck of new cards (hand images, names,
placement, a guardian star, a direct attack, the opponent's attack, and the
3D battle with a copy's model); a trade of a new card between two memory
cards (`MEMORIES_INPUT2` drives the second pad) with both sidecars rewritten
only after the write; a two-player duel; a save state taken in Build Deck and
resumed in a new process; 5,722 cards; a save with a new card in its deck
loaded without the mod; a card with its own picture, plate, text, type,
level, attribute and stars in the card view and in the duel's hand; and the
Windows build under Wine. Without a card mod
the smoke screenshots are unchanged on both systems.
