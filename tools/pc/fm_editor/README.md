# FM Editor

A standalone editor for mods of the PC port: cards and their art, fusions, equips, rituals,
the opponents' deck and drop pools, and the campaign map. It is a program of its own, not part
of the game, and needs nothing but Python 3 and Tkinter (part of Python on
Windows and macOS; on Linux maybe a package of its own: `python3-tk`, or `tk`
on Arch).

**The mod is the diff.** The editor reads the retail tables from your own
game files, lets you change them, and on save writes a mod folder whose
`mod.json` holds only what differs from retail, in the schema the port
reads ([modding](../../../notes/modding.md), [more cards](../../../notes/more-cards.md),
[gameplay tables](../../../notes/gameplay-tables.md)). Opening a mod folder
lays its `mod.json` over retail, so a saved mod can be opened and edited
again. It writes mod folders only: never the disc, never `game/`.

## Running it

    python tools/pc/fm_editor [--game <folder or .bin>] [--mod <mod folder>]

The Cards tab's right-hand options and the Limits tab scroll vertically:
use the scrollbar or mouse wheel when the window is too short. Tabbing to
a field brings it into view; text boxes keep their own scrolling.

**Starchips**, below **Password** in Cards, edits any card's price
on the Password screen (0–999999; **0 is free**). The field shows the price
after this mod's `passwords` rules, with the disc's price beside it. Leave
it empty to remove the card's price override and use the mod's `all` rule,
or the disc's price if there is none. **Apply**, then **File > Save**.
Unedited percentage prices stay as percentages. Added cards use a default
price of 999999 and stable identities in the `passwords` table.

The window has a tab per table:

| Tab | What you edit |
|---|---|
| Cards | search and filter the 722 cards; name, card text (with the game's 20-letter, 8-line wrapping counted, and **Tools > Card text preview** to see it as the card view draws it, below), ATK/DEF, type, attribute, level, guardian stars, password; the retail value beside each field. A guardian star may be **(none)**, written `0`: both none is a monster with no star at all (no SELECT A GUARDIAN STAR box, no star bonus given or taken, no star drawn), the second none a monster with one star; a first star of none with a second is warned about, because the game takes the second as the card's one star ([no star](../../../notes/modding.md#guardian-stars-names-icons-new-stars-and-matchups)). **Frame**: the colour of the card's frame (by type, or monster, magic, trap, ritual, purple or orange whatever its type), with a swatch of it; the card view, the Library and the duel draw it ([frame colour](../../../notes/more-cards.md#frame-colour)). **Notes**: text of your own on the card (what you changed, what you plan), saved as its `"notes"`; the game shows none of it, and a code mod can read `<tag: value>` tags from it ([notes on a card](../../../notes/more-cards.md#notes-on-a-card)). **Revert to retail** keeps them; the **With notes** filter lists the cards that have some, and the search finds words of them too. **Add a card** copies the selected one as a new card with a stable id; a new card starts in nobody's chest (it is won in its base's place, dealt in a starter deck, or given by Game > Cheats), and its password works in the Password shop and is shown in the card view. Added cards default to 999999 starchips; edit **Starchips** to change the price |
| Art | a card's picture (102x96), thumbnail (40x32, the hand and the field) and name plate (96x14) as the disc has them, beside what the game will draw at the console's resolution and at Internal 2x/4x; **Import PNG**, **Export** the disc's or the mod's (to paint over), **Revert** |
| Fusions | every pair and its result (search by a card, or show the changed ones); add, change, remove (the pair no longer fuses) or revert; **Remove recipes of...** takes away every disc recipe of a card in one `remove` rule; a pair a card's own `fusions` list makes (no rule of the mod deciding it first) shows that list's result, marked "own list"; **Bulk...** adds or takes away the fusions of every card of one filtered set with every card of another (below) |
| Equips | per equip card, the monsters it may equip; add one, add or remove a whole type, remove, revert |
| Rituals | per ritual card, its three tributes and the monster it summons |
| Duelists | per opponent, the deck pool and the S/A-POW, B/C/D and S/A-TEC drop pools: weights, their chance, the retail weight, and the total against 2048 (**Normalize** scales a pool back to 2048 the way the port does). The deck is either the **Weighted deck (retail)** or a **Fixed deck (40 cards)**: forty specific cards by their copies, counted against 40, each beside its weighted chance; **Copy the weighted deck's most likely 40**, **Clear**, **Revert to retail** |
| Starter decks | the decks a new game may be dealt in place of the disc's weighted pools: a deck's name, its weight against the other decks offered, and its cards by their copies, counted against the forty a deck holds |
| Map | the campaign map's sixteen places (below): each exit's destination, direction, story-flag condition, length and arrow on the screen, the Millennium Puzzle marker's place in the town, Confirm's destination and each place's camera, over pictures of the map drawn from your disc; **Reset place**, **Reset all**; **Pictures...**: the marker, arrows and name panel, and the terrain's textures |
| Limits | the numbers the game caps (`limits`, [gameplay tables](../../../notes/gameplay-tables.md#limits-atk-def-lp-starchips-and-more)). The simple part: the ATK and DEF cap, the LP a duel starts with, and how far healing goes. **Show advanced**: ATK and DEF apart, each side's starting LP, the two-player LP choice (start, most, step), the most starchips, the chest's copies, the Free Duel and two-player records, and a table of duelists with the LP each side starts with against them. An empty field is the game's own number (beside it, with the range the game keeps); a value past that range is warned about and held at the most the game keeps |
| Guardian Stars | the stars (`guardian_stars`, [Guardian Stars](../../../notes/modding.md#guardian-stars-names-icons-new-stars-and-matchups)): the list of stars with a name and an icon each (**Import icon (PNG)...**, with a preview; the game makes it 16x16 in the disc's stars' colours), **Add star** for 11 to 15 (a card holds its stars in 4 bits, so fifteen at most), and the full grid of matchups: a row is the attacker's star, a column the defender's, a cell the bonus the attacker's side gets, green above 0 and red below; click a cell, type a bonus or use **+ default**, **- default** or **0** (with **Reverse pair gets the opposite** on, the reverse cell takes the opposite sign). **Default bonus** moves the disc's 500 in both cycles, **Retail cycles** and **Clear all** are presets, **Revert to retail** takes the whole key away. **Set stars by rule...** sets many cards' first or second star from their attribute or type through a table you fill in (a Fire monster's first star is Fire), or one star for all, **(none)** included (a first star of none leaves the second as the card's one star, as the game reads it; both none, no star), over a filter of cards like Bulk fusions', with a preview and **Undo last batch**. **Show advanced**: a name per language (`fr=Feu, de=Feuer`), an icon's colours (`game` or its own), and what happens at a summon (`ask`, `first`, `best`). The Cards tab's star lists show the mod's stars as they are named here |
| Packs | the card packs the mod sells for starchips on the Password screen: each pack's name, description, price, cards a pack and picture, its cards with their tier, weight and chance; an **Advanced** part for everything else; **Shop settings...** and **Simulate...** (below) |
| Mod info | id, name, version, author, description, `settings`, and the other `mod.json` keys, kept as written (`limits` is the Limits tab's, `guardian_stars` the Guardian Stars tab's) |
| Conflicts | the loader's checks; double-click a line to go to it. Below them, where this mod meets the **other mods installed** (beside the game and in the player's mods folder, or a folder chosen with **Other mods folder...**): the same lines as the game's Mods window, a warning where only one mod's change is used and a note where the changes add up or agree ([When mods overlap](../../../notes/modding.md#when-mods-overlap)) |

On **Cards**, **Equips**, and **Duelists**, click a column heading to sort
ascending; click it again to reverse the order. An arrow marks the active
column and direction. Card lists support **#**, **Name** (or **Card/Monster**),
**Type**, **ATK**, and **DEF**; the equip and opponent lists also sort by their
own headings, as do weights, chances and fixed-deck copies. Numbers sort by
value, names and types alphabetically. Each list keeps its chosen order while
filtering, editing, or switching opponents and pools. Sorting keeps the current
selection and changes only the view, so it does not mark the mod as edited.
Wide lists have a horizontal scrollbar.

**File > Save** writes the mod folder (Ctrl+S); the first save asks where
(an empty folder, or a parent where a folder named after the mod id is
made; the port's player mods are in `Documents\My Games\YFM Re-Decomp\mods`).
Enable the mod in the game under **Game > Mods** and restart. **File > Open
mod folder** opens a mod over retail. Save refuses nothing, but lists what
the loader would refuse first. **Save as** copies the mod's assets too,
replacing matching files when overwriting another mod. Its destination may
be inside the source mod; the destination itself is excluded from the copy.

**View > Dark mode** switches the window, its dialogs and the text boxes,
lists and menus to a dark look at once, and back (no restart); the editor
remembers it in its own settings file, `%APPDATA%\FM Editor\settings.json`
on Windows (`~/.config/fm-editor/settings.json`, or under
`XDG_CONFIG_HOME`, elsewhere), never in the mod or the game's folders. Off,
the editor looks as it always has (the desktop's "vista" theme on Windows,
"clam" elsewhere). The dark look is a clam theme recolored (`theme.py`;
text at least 4.5:1 against its background); the Art tab's pictures keep
their pixels. On Windows the title bar turns dark too (Windows 10 1809 and
later), and since Windows draws a menu bar light whatever it is asked, a
strip of menu buttons with the same menus stands in for it (Alt+letter and
F10 open them). Left light: the thin frame Windows draws around an open
menu, and the system's own dialogs (message boxes, choosing a file or
folder).

On Windows the editor tells the system it knows the screen's dpi (per
monitor, `theme.dpi_awareness`, before the first window), so at 125% or
150% it is drawn at that size itself instead of stretched and blurred: the
fonts, in points, follow Tk's scaling, and so do the sizes the editor gives
in pixels (`widgets.px`) and the dark theme's arrows and check boxes. Its
face is Windows 11's Segoe UI Variable where there is one (Segoe UI before),
Consolas for the fixed one. None of this runs elsewhere.

### Bulk fusions

**Bulk...** in the Fusions tab (`bulk_dialog.py`, the rules in
`bulk_fusions.py`) pairs every card of **Material A** with every card of
**Material B**. Each side is chosen by filters that must all hold, empty
meaning any card: kind (monster, magic, trap, ritual, equip), monster type,
attribute, guardian star (either of the two), ATK, DEF and level ranges,
words of the name or of the card text as the editor shows them (any case),
a list of cards (numbers, ranges such as `10-20`, names) and "only cards a
fusion makes". **A → B** copies one side's filters to the other.

* **Result**: one card, or "the weakest of these that beats both materials"
  (a list of monsters; each pair gets the one with the least ATK above both
  materials', the way the disc's type fusions climb). "Only when the
  result's ATK beats both materials'" skips the pairs it would not; a card
  with itself is left out unless allowed.
* **A pair that already fuses** keeps its result, or has it replaced. What
  "already fuses" means is the port's reading: the pair's rule, or for a
  card the mod adds, its base's (`Tables_Fusion`); a pair a rule forbids
  (`null`) fuses with nothing and is free. A pair already making the chosen
  card is left alone.
* **Take fusions away** uses the same filters, optionally only the fusions
  that make one card.
* A+B and B+A are one pair, as in the game: a pair both sets make twice is
  counted once.

The preview says, as the filters change, how many pairs are added,
replaced, kept or skipped and why, lists the first 300, and counts the
fusion rules the mod would carry. **Apply** asks first; **Undo last batch**
puts back the pairs the batch changed (not those edited since). The mod
writes rules, not the disc's 64 KB table, so neither that table's size nor
its count byte per card limits a batch: every pair of the 722 cards is
261,003 rules, a 24 MB `mod.json` the port reads in about 3 s. A batch that
would leave the mod past 300,000 rules is refused. Ports built before the
bulk fusions read a long `fusions` list in quadratic time (20,000 rules took
about a minute); use a current build.

### The Map tab

The campaign map (`campaign_map.py`) is the overworld module's table of
sixteen places: 0-9 the world map's sites, 10-15 the town's, named as the
game names them (strings `0x8350` + place; two town places read "before /
after" when their label changes once the tournament is over). A place is
what the game shows while the player stands there, so it is edited as a
screen:

* **Screen**: the place at 2x, over the map as its camera sees it (drawn
  from the disc's own 3D map, `map_view.py`: the terrain model, its
  textures, the camera of `ViewState_ApplyOrbit`, the game's fog and, on the
  world map, its spotlight; close to the game's frame, not exact, because
  the light is a fit). The name panel, each used exit's arrow (the game's
  own sprite from the map's strip, `field_08`) and, in the town, the
  Millennium Puzzle marker are drawn where the game draws them; drag an
  arrow or the marker to move it. Exits at the same spot share one label,
  with the flag each needs.
* **Overview**: the world map from straight above (turned as its cameras
  mostly look: -x up) with each world site where its camera looks, the town
  (place 10's camera) with its places where the marker stands, and every
  exit as an arrow to its destination: green always, amber while a flag is
  set, blue while it is clear, dashed for Confirm; thicker for the selected
  place. Dragging a world site moves its camera's target; dragging a town
  place moves its marker. **Map** chooses the model before or after the
  coup (the terrain changes; the table is one for both). A **Reference
  picture...** (a screenshot of the game at this place) replaces the drawn
  map for this camera while the editor is open.
* The form: the camera (distance, heading, pitch in 4096ths of a turn, and
  the x and z it looks at); the marker (the town only: the world map draws
  none, so a world site's are kept as they are); Confirm's destination
  ("enter the place's own scene" is the disc's 0) and whether it waits for
  exit 1's condition (the record's gate); and four exits, each **Used** or
  not (destination 16 on the disc), its destination by name, the direction
  held as a D-pad (any of the four), **When**: always, while a story flag
  is set, or while it is clear (the flag number is the one the game tests,
  `0x8000` set in the record for "clear"), **Frames** (the move's length:
  the camera and the marker take that many frames), the arrow's picture
  (one of the eight the map has) and its x, y on the screen. A new exit
  starts at 16 frames, the disc's usual length.

The game takes the first exit whose direction is held and whose condition
holds, so two exits may share a direction under opposite conditions (the
disc does that); Cancel in the town, once the tournament is over
(flag `0x47`), always leads back to the world map at Metropolis.

**Pictures...** (`map_art.py`) replaces the map's own pictures, as
texture pack entries in the mod's pack (the Art tab's pack, `textures/`,
the map's PNGs under `textures/map/`); the Screen and Overview draw them
at once:

* **Sprites**: the map's one strip of sprites (WA sector `+141` of each
  package, 256x256 at four bits, drawn through four 16-colour palettes:
  0 the name panel, 2 the marker, 3 the arrows; a dump shows palette 1
  read too). **Import picture...**
  for one sprite (the marker, the name panel, an arrow) pastes it into every
  cell of that sprite's animation (the marker turns through 16 frames, an
  arrow pulses through 10; the name panel is one frame), so the new picture
  keeps the sprite's motion but not the differences between its frames; an
  arrow and its mirror share their cells (right and left, the diagonals).
  The picture is the sprite's first frame as the preview shows it (the
  marker 32x32, an arrow 16x24, 24x16 or, on a diagonal, 16x16, the panel
  256x32); a bigger one is
  kept at up to 4x. **Export sprites...** writes the strip through each
  palette (`sprites-p0.png` to `p3.png`) to paint every frame yourself, and
  **Import sprites...** takes those files back, any size up to 4x. The
  mod's entry names the strip as the game reads it through one palette, in
  both packages (the strips are the same, and share the PNG).
* **Terrain textures**: the map is a 3D model whose textures are tiles
  (86 in 100 of the texels of its upward faces are drawn more than once,
  one up to 51 times), so there is no single picture of the map to swap:
  its textures are replaced one by one. **Export textures...** writes each
  texture of the chosen map (before or after the coup: two models, 60 and
  61 textures, 4 or 8 bits) as the terrain draws it, one PNG per palette it
  is drawn with (`textureNN-PPPP.png`: the upload's number and the palette
  word), and **Import textures...** takes the files of a folder with those
  names; a texture whose file is not there stays the disc's. A picture of
  another shape is stretched to the texture's, and kept at up to 4x.

At the console's resolution a bigger picture is averaged down to the
texture (the game's 4 or 8 bits are gone: any colour goes); Internal 2x
and 4x draw it at its own resolution. The game reads a pack at start, like
the table, so the mod needs a restart.

## Game files

The editor looks for the game where the port does: `MEMORIES_DISC`, the disc
the port was last pointed at (`disc-path.txt` in the user directory),
`game/` beside the program, the program's folder, `game/` in the user
directory, and `./game`. It takes a raw `.bin` image (the one the port runs
from), an ISO, or a folder holding `SLUS_014.11` and `DATA/WA_MRG.MRG`.

What it reads (layouts in `gamedata.py`):

| Table | Where |
|---|---|
| card stats, level and attribute | `SLUS_014.11`, `0x801D4244` and `0x801D5332` |
| card names and texts | the executable's text banks, through `tools/pc/text_listing.py` |
| equips, fusions, rituals | `WA_MRG.MRG`, the duel package at `0xB63000` (+0x22000, +0x24800, +0x34800) |
| deck and drop pools | `WA_MRG.MRG` `0xE99800 + 0x1800 * opponent` |
| the Password screen's passwords | `WA_MRG.MRG` `0xFB9800 + 8 * card`: price, then the password as BCD digits (`0xFFFFFFFE` for none) |
| the text font and its colours (the card-text preview only) | `WA_MRG.MRG` sector `0x1690` (16 sectors, the 8x12 font's page) and the first 32 bytes of sector `0x16C2`, as `src/pc/cards/font_art.c` reads them |
| the campaign map (the Map tab) | `WA_MRG.MRG`, the two overworld packages at sectors 8153 (before the coup) and 8311 (after): the module's first word `0x14`, the table at `+0x11A8` (16 x 66 bytes), the display resource bank at sector `+140`, the sprite strip `+141` (16 sectors, 256x256 at four bits) and its palettes `+157`; the terrain model at `+6` (134 sectors, an HMD) |

The 15 "glitch" fusions the game's table reader makes by reading past an odd
record are shown as retail fusions and marked.

## What it writes

* `cards`: a `replace` entry per changed retail card with only the changed
  keys, and a `copy` entry per added card with a stable `id`. An added
  card's password is its entry's `password` (8 digits, used in the Password shop and shown by View > Card
  passwords). Keys the editor does not show (`model`, `count`...) are kept as
  written; `art`,
  `thumbnail` and `title` are the Art tab's (below). A copy with no `name` shows its base's name from the disc.
  `stars` is written as the disc's names and `0` for none (`[0, 0]`: no
  star); read back, `0`, `null`, `"none"` and `"(none)"` are all none, as the
  game reads them, and `[none, X]` stays as written.
* `passwords`: a retail card whose password changed gets `{"password": "…"}`
  (`""` for none) under its name, merged into the mod's own entries, whose
  `all` and `"card number"` stay as written. Editing **Starchips** for an original or added card writes
  `"starchips": n` in the card's entry, replacing its previous absolute or
  percentage price; other cards' prices stay as written. A password is up to
  8 digits, and no other card's: the Conflicts tab says when two cards share
  one, since the Password screen then gives the lower card number.
* `fusions`: one rule per pair whose result changed (`"result": null` for a
  fusion taken away). An added card fuses as its base until a rule names it,
  so taking away its pair's fusion writes a `null` rule for it. A mod's
  `{"remove": C}` rules are kept as written, first: the disc recipes of C
  they take away write nothing, and one the mod keeps or changes (reverted
  in the tab, say) is written as a rule of its own, which the remove would
  otherwise take away too. When every disc recipe of C is back, the remove
  is dropped; one for a card no disc recipe makes stays as it was. A
  pair rule the mod wrote is kept even where the result alone needs none
  (the disc's result, or a `null` on a recipe a remove takes away): the
  game asks it before a card's own `fusions` list. So is one for a pair
  such a list names once the modder edits it, so the game plays what the
  tab shows. A remove leaves such a pair to the list, as in the game (mods'
  recipes still make the card): the tab's row says "own list" and shows
  what the list makes (`own_fusion`). Deleting an added card turns a kept
  rule that made it into a null rule, shown as "removed"; one on a pair
  with no disc fusion that no own list names is dropped instead.
* `equips`: per equip card, `add` and `remove` (a whole monster type as its
  name), or `replace` when that is shorter. An added card is equipped (and
  equips) as its base, so what differs for it is written in later entries,
  which the game's reading of the rules confirms before saving. `bonus` and
  `bonus_if` are kept as written.
* `rituals`: a changed recipe, or `"result": null`.
* `guardian_stars`: the stars the mod declares (`id`, and `name`, `icon`,
  `palette` when set), `default_bonus`, `replace` and `choice` when not the
  disc's, and a `matchups` entry for each pair whose bonus differs from what
  the rest of the key already gives; a mod's `beats` and `mirror` are read
  into the grid and written back as those pairs. Icons are written to
  `icons/star-<id>.png` in the mod folder.
* `limits`: what the Limits tab sets, a key per field that is not empty
  (`"life_points": 16000` when only both sides' start is set); a key the tab
  does not show is kept as written.
* `drops` and `decks`: per opponent and pool, the fewest listed weights that
  make the port's arithmetic (`tables.c`, mirrored in `pools.py`) come out
  at exactly the edited pool; an edit every opponent shares is written once
  as `"all"`.
* Fixed decks (`"decks": {"Simon Muran": {"fixed": true, "Kuriboh": 4, ...}}`,
  `fixed_decks.py`, read by `tables.c` `read_fixed_deck`): an opponent's deck
  as forty specific cards, in place of its weighted pool, shuffled for each
  duel. A card has 0 to 40 copies (no limit of three: the weighted deal's
  limit does not apply to cards written down), and they add up to exactly 40;
  a card the port cannot name is left out uncounted, so a deck naming one, or
  one that is not 40, is left out and the weighted deck is dealt. The
  Conflicts tab says so before saving, and warns when one duelist has two
  entries (the port deals the later) or weighted edits a fixed deck hides.
  Choosing **Fixed deck** starts from the forty the weighted deck most likely
  deals: its weights apportioned to 40 cards (largest remainder, ties to the
  heavier card and then the lower id), at most three of a card as the retail
  deal allows. Choosing **Weighted deck** again keeps the fixed one aside
  until the mod is closed; **Revert to retail** takes it out and puts the
  weighted deck back to the disc's. The weighted pool's own edits stay in the
  project while a deck is fixed, but a fixed deck written under the same key
  takes their place. A deck the editor read is written back exactly as it
  was while it is untouched (and still names the same cards: a card named by
  an added card's identity follows a new mod id); a changed or new one is
  written as `"fixed": true`, the cards in id order, then any name it could
  not place. An entry for `"all"`, or for a duelist a mod adds, is kept as
  written. Drops and the other duelist data are not touched by any of this.
* `starter`: the decks a new game may be dealt
  ([the starter deck](../../../notes/starter-deck.md)), each written down as
  its cards and their copies rather than as weights — which is what lets one
  hold a card the mod adds. One deck is written as the object itself, several
  as a list. A deck is exactly 40 cards, and the Conflicts tab says so while it
  is not; more than three copies of a card, or more than one Exodia piece, is
  dealt as written but is a warning, because Build Deck will not take it back.
  A card the editor cannot place keeps its row and its copies, under the name
  it was written with, and `"starter"` given as the name of a file stays that
  filename.
* `packs` and `pack_shop`: the card packs and the shop's rules
  ([card packs](../../../notes/card-packs.md)). Each pack is kept as the
  object the mod wrote, so a key the editor has no field for stays as
  written, and is written back with only what differs from the game's
  defaults: no `"count": 5`, `"price": 100`, `"duplicates": "allow"`,
  `"reveal": "flip"`..., a pool of weights of 1 as a list, `"cost":
  {"starchips": n}` alone as `"price"`. A pack's picture goes in the mod's
  `packs/` folder. `"packs"` given as the name of a file stays that filename
  (the tab then edits nothing).
* The duelists the editor knows are the forty the disc lays out, since it
  reads the game's own files. A mod may add its own
  ([more duelists](../../../notes/more-duelists.md)), and which of those exist
  depends on the mods applied at run time, so an entry of `drops` or `decks`
  naming one is kept as written rather than resolved — as is either table
  given as the name of a file (`"decks": "tables/decks.json"`), which the
  editor does not read. A roster's `duelists/`, `decks/`, `drops/` and
  `portraits/` folders are copied with the mod's other files.
* `data` (the Map tab): one entry patching `\DATA\WA_MRG.MRG;1` where the
  map differs from the disc, the same bytes in both overworld packages'
  tables (`0xFEC800 + 0x11A8` and `0x103B800 + 0x11A8`, each 1056 bytes),
  as runs of changed bytes (`{"at": "0xFED9CF", "bytes": "01"}`). The PC
  port reads the table from the package it loads
  (`src/overlays/overworld/location_table.c`), as the console does, so the
  patch works on either; the game reads it when the map loads, and data
  mods need a restart. The module's alternate copy of the table
  (`+0x1E54`) has no reader found on the map's paths and is not written. The mod's other
  `data` entries are kept as written, before the map's; opening a mod
  takes its patches of the two tables back into the map (a run across a
  table's edge stays as written, with a note), and a mod whose two tables
  differ opens with the one before the coup and saves both alike.
* The map's pictures (the Map tab's **Pictures...**, `map_art.py`): texture
  pack entries in the same `textures/manifest.json`, after the Art tab's,
  PNGs under `textures/map/`: the sprite strip as one entry per palette and
  package (`archive` `WA_MRG.MRG`, `offset` the strip's sector `+141`,
  64 words x 256 rows at 4 bits, `clut_offset` the palette in sector
  `+157`), and a terrain texture as one entry per palette it is drawn with
  (the image's place in the model's image section, its words and rows, 4
  or 8 bits, and the palette the polygons name, as the last upload to that
  place in VRAM leaves it). Opening a mod takes such entries back into the
  map; the pack's other entries are kept as written.
* Art (the Art tab, `art.py`): a retail card's picture and thumbnail go in
  a texture pack, `textures/manifest.json` with PNGs under
  `textures/cards/`, one entry each addressed as `extract_images.py` and
  `hd_assets_pack.py` address them (the picture at the art record, WA
  sector `(n-1)*7 + 722`, 8-bit through its 256-entry palette; the
  thumbnail at sector `n-1`, through its 64 entries), and mod.json gets
  `"textures"`. A pack image may be up to 4x: the console's resolution
  averages it down, Internal 2x and 4x draw its detail. A card the mod
  adds has its base's place on the disc, so a pack cannot tell the two
  apart: its picture and thumbnail are the entry's `art` and `thumbnail`
  PNGs (under `art/`), made into 102x96 and 40x32 at 255 and 63 colours when
  the game starts, the same at every resolution. The name plate of any
  card is the entry's `title` PNG (dark ink on white; a retail card gets a
  `replace` entry for it). An imported PNG is cut to the part's shape from
  the middle (a warning says so), made opaque over black, and kept at most
  4x; a new picture for a retail card makes its thumbnail too, cut where the
  game cuts its own (`tools/pc/hd_recipes/thumb_crops.json`) until one is
  imported. A retail card's own `art` in mod.json would hide the pack's
  picture, so importing one moves it to the pack. An opened mod's pack is
  kept entry by entry: only a card part's plain entry (no `setting`) is
  the editor's. One a setting switches (assets-hd's) is shown until a PNG
  is imported, which goes before it in the pack so the game draws the
  import. A PNG another entry or card shares is left to it, and the PNGs
  are written on save. A card's `art` that can't be read stops the import
  that would move it, instead of losing it.
* Every other key of an opened mod (`data`, `text`, `textures`, `audio`,
  `requires`, `duelists`...) is kept as written, and the folder's other files
  are copied when the mod is saved somewhere new.

Cards are named by their retail name when that finds the card again in the
port (`retail_by_name`), by number otherwise, and added cards by their
stable identity `<mod id>:<id>:1`.

## Importing a modified game

The PS1 scene's mods (Mod 13, FM 2023, rebalances...) ship patched copies of
the game's files. **File > Import a modified game** (or the `import`
command) compares a modified `.bin`, or its `SLUS_014.11` and `WA_MRG.MRG`,
with your retail files and makes a port mod of the difference, which then
opens and saves like any other:

| Changed in the modified game | Becomes |
|---|---|
| card stats, names, texts; fusions, equips, rituals; deck and drop pools | `cards`, `fusions`, `equips`, `rituals`, `decks`, `drops`. A name or text that differs only by spaces at line ends stays retail's. Drop pools a mod stores encoded (the TeaOnline drop tool writes `bias + 8 * weight + noise` and makes the draw at `0x80021860` jump to code that undoes it) are decoded as `max(0, (raw - bias) >> shift)`, with the bias and shift read from that code's `addiu` and `sra`, or, when the code is not recognized, the values that make every such pool add up to 2048. Any other pool that does not add up to 2048 is scaled to 2048 keeping each card's share. The report says which |
| other text: dialogue, menus, types, stars, duelists, places | `text.txt`, a partial [text listing](../../../notes/translation.md) (a bank whose changed strings jump is written whole; a bank that is the mod's code is left out and reported). Card names and texts with colour or icon codes, and texts the mod left empty, go there too (as `{f8 0A 05}...` or a bare `{end}`), since `cards[]` cannot carry them; the editor shows them and writes an edited one to `cards[]`. The name entry's strings (`0xF0`-`0xFF`) stay retail's |
| other bytes of `WA_MRG.MRG` (pictures, passwords and costs, portraits...) | `data` patches; a run longer than 4 KB becomes whole sectors in `data/`, replaced at the retail disc's LBA. Past 256 patches or 16 sector runs (the port holds 1024 and 64 for all mods together), or when the file's size differs, the whole file is replaced. The tables `mod.json` carries (fusions, equips, rituals, pools) are always retail's in what `data` carries, and so are starter decks written as counts of 40 and the programs the port runs its own code for (Free Duel, name entry, password, overworld), all reported |
| code and tables of the executable (AI parameters, field bonuses, equip bonuses, the draw...) | nothing, except the rules below: the port runs the executable's code natively. Listed in the report by RAM address, with the `j`/`jal` instructions that reach each place; changed bytes of the text banks that the text listing does not read (a mod's code or tables in the banks' free space, text left over) are listed too |

The report is shown and saved with the mod as `import-report.txt`.

**Mods made with a MIPS patch kit.** A family of mods
changes the duel in MIPS with one kit. `kit.py` recognizes its code by the
place it hooks and the shape of the code, and reads the values from the
mod's own code and tables (the addresses of its code move from mod to mod):

| Signature | Read as |
|---|---|
| `Duel_CheckEquip` steps 94 bytes and tests a bit per monster (A6) | every equip card's monsters from the bitmaps, with the records per monster some add and the monsters no equip takes. Records of monsters or magic used as equips are reported: the port's equips take equip cards only |
| a ritual table that is mostly not recipes (A13) | rituals removed |
| `Duel_ShuffleDeck` deals counts of copies up to 40 (A4) | `decks` with `"fixed": true`: what that shuffle deals from each pool |
| the prize draw jumps to a loop that calls `addiu -bias; sra shift` (A1, A2) | the drop pools decoded; the number of prizes goes to the report and the mod's `README.txt` (set Game > Card drops to it; `mod.json` has no key) |
| `Duel_AwardCard` caps the chest and pays starchips (A3) | `chest_overflow` |
| the terrain table, or `Duel_GetTerrainBoost` reading a table of its own (A9) | `terrain_bonus` with `"replace"` |
| `Duel_SelectAttackTrap` rewritten, thresholds as u16 (A8) | `trap_thresholds` |
| `DuelScene_UpdateCardPlacement` walks a table of bonuses (A7) | `"bonus"` on the equips' entries |

What has no key (the 30000 cap, terrains past six or by attribute, each
opponent's home field, the AI's commands, the frame colour, monster effects,
equips with an effect of their own) is listed in the report and in the
imported mod's `README.txt`, in the game's terms.

    python tools/pc/fm_editor import <modified .bin, folder or SLUS_014.11> -o <mod folder>
        [--wa <modified WA_MRG.MRG>] [--game <retail>] [--id <mod id>]

## Converting an old recomp's .ygomods package (one way)

The old static recompilation's in-game editor exported `.ygomods` packages
(a ZIP of INI and text files and PNGs). The port does not read them, and
they are not a mod format of the port: its mods are folders with a
`mod.json`. The editor can only convert one, once, into such a folder, so
that a mod made for the old recomp has a starting point here. The
conversion is best effort: what the port has no key for is left out and
listed in the report, so check the result in the game before sharing it.
**File > Convert an old recomp's .ygomods package (one way)** (or
`import <file>.ygomods -o <mod folder>`) reads one over retail:

| In the package | Becomes |
|---|---|
| `cards/<id>/card.ini`: name, description (`\|` breaks a line), ATK/DEF, level, type, attribute, stars | `cards` replace entries |
| `cards/<id>/card.ini`: `equips`, `ritual` | `equips` (the complete list), `rituals` |
| `cards/<id>/card.ini`: `price`, `password` | a `data` patch of the password table in `WA_MRG.MRG` (`price` taken as the starchip cost) |
| `cards/<id>/art.png`, `thumb.png`, `title.png` | the card's `art`, `thumbnail`, `title` |
| `fusion-edits.txt` | `fusions` (a `clear` line removes every retail fusion first) |
| `drop_table_edits.ini`, `drop_missing_cards.ini` | `drops` (the listed weights, the rest sharing the remainder, as the port does) |
| `cpu-duelists.ini` deck weights; `name =` | `decks` (the whole pool); a renamed opponent in `text.txt` |
| `duelists/<n>/portrait.png` | a texture pack image of the Free Duel portrait |

Listed in the report and left out, as the port has no data key for them:
scripted monster and magic effects (`on_flip`, `battle`, `effect`...),
card and name colours, the nine AI bytes, scripted rewards and StarChip
rules, the recomp's card shop and its settings, and `dialogue.txt`, whose
code numbering is the recomp's own (translate with `text_listing.py`).
The importer was written from the packages' own file layout; no code of the
recomp is used.

## Checks

Before saving, the editor runs the loader's checks (`validate.py`): the mod
id, settings, ATK/DEF in tens up to 5110, levels, a copy staying on its
base's side, equip and ritual cards of the right type, a deck pool of at
least 14 cards, a drop pool with a card left, pools adding up to 2048, and a
fixed deck of exactly 40 cards the editor can name.
For the art, the texture pack loader's (`texture_pack.c`): `textures` inside
the mod and its `manifest.json` an array; each entry's `file` and `archive`,
the file inside the pack and there, its measures (offset, words 1-1024,
rows 1-512, depth 4/8/16, stride, `crop_left` and `width` within the row),
`row_offsets` as long as `rows`, and a `setting` the mod declares; and the
cards' `art`, `thumbnail` and `title`: inside the mod, there, and PNGs.
For the map: a destination past 15 (16 is "no exit") or Confirm past 15,
and a move of 0 frames (the game divides by it) are errors; an exit that
leads back to its own place, needs no direction or other buttons, is
shadowed by an earlier exit in the same direction under the same condition,
has a flag past `0x7FF` or its arrow off the screen, a marker off the
screen, and a place no exit, Confirm or Cancel leads to any more are
warnings.

## Card text preview

**Tools > Card text preview** opens a window of its own that follows the
Cards tab: the selected card's text as the card view lays it out and draws
it, redrawn as you type. The tab itself is unchanged, and nothing of it goes
into the mod. `card_text.py` does the work:

* **Layout**, in the two steps the port and the game take: the port's
  wrapping (`cards.c` `encode_description`: lines of up to 20 letters,
  broken at spaces, `
` where it stands, a longer word left whole), then the
  text box (`TextBox_WrapLineIfNeeded`): 8 pixels a glyph and 21 to the
  box, so a word past 21 letters is cut where the box ends, and the rest of
  its line takes a row of its own. The card view shows 8 rows clear of its
  panel's frame, draws a 9th over the frame, and stops before a 10th (seen
  in the game with a test text, in Build Deck's card view, whose box has
  255 glyph sprites; the duel's viewer has 160, so a long text may stop
  sooner there). The preview marks each: the 9th row on the
  frame's colour, the rows the game never shows dimmed on grey, a red tick
  right of a row the box cut mid-word, and a red box for a character with
  no retail letter (the port sets those from a font); an accented letter is
  drawn plain (the port draws its mark on).
* **Font**, at 1x to 4x:
  * *Retail font*: the game's own 8x12 font and text colours, read off the
    player's disc each time (nothing of it is kept or saved), each texel
    made `scale` pixels square. At 1x the letters are the game's pixel for
    pixel (the panel behind them is a flat colour, not the game's stone).
  * *HD text: the port's face*: what Video > HD text draws at Internal 2x-4x,
    set in the face the port uses when no mod gives one (on Windows the
    first of Segoe UI, Arial and Tahoma in the Fonts folder; elsewhere
    fontconfig's `sans-serif:bold`). A mod's own `"font"` comes first in
    the game; choose that file in the next mode to see it.
  * *HD text: a font file*: any TrueType file you have (**Font file...**,
    which opens in the system's fonts folder), such as your own copy of Matrix,
    the face of the paper cards' names, if you have a licence for it. The file is only read: the
    editor never copies it into the mod. OpenType fonts with PostScript
    (CFF) outlines, most `.otf` files, are refused with a message; their
    `.ttf` version works.

  HD text is drawn as `src/pc/text/hd_text.c` sets it: each glyph stays in
  its retail cell, the face's baseline, x-height, capitals, ascenders and
  descenders are set onto the retail font's lines, the glyph is made as
  wide as the cell's letter (so a small-caps face's l stays as narrow as the
  retail l), its stems as heavy, with the dark outline and each row's
  shading through the text's palette. `ttf.py` reads the TrueType outlines
  and fills them in plain Python (non-zero winding, 4 sub-rows a pixel),
  in place of FreeType, so the result is close to the game's, not identical:
  against the game's own 4x picture about half the text's pixels are the
  same colour and 96-97% within one or two steps of the palette. A face
  whose lines cannot be measured (no x-height, no descenders) is not used by
  the port, which keeps the retail letters; the preview says so and does too.

  The first HD picture of a face at a scale takes a second or so (the
  window shows a busy cursor); glyphs are kept, so typing redraws at once.

## Command line

    python tools/pc/fm_editor check <mod folder> [--game <folder or .bin>] [--print]

opens a mod over retail, lists what the loader would complain about, and
with `--print` shows the `mod.json` the editor would write for it.

## A standalone executable

    python -m pip install pyinstaller
    python tools/pc/fm_editor/build_exe.py [--dist tmp/pc/fm-editor]

builds the editor with no Python needed to run it. On Windows that is
`tmp/pc/fm-editor/fm-editor/`: `fm-editor.exe`, `fm-editor.pkg` and the
`fm-editor-files/` folder, which stay together. Elsewhere it is one file,
`tmp/pc/fm-editor/fm-editor`. Put them beside `memories-pc.exe` and the
editor finds the game's `game/` folder there. Build outputs never go in git.
Windows gets no one-file build because virus scanners took it for a dropper.

Each release carries it as `fm-editor-<version>-windows.zip` and
`fm-editor-<version>-linux.tar.gz` (`.github/workflows/pc-release.yml`):
unpack it where the game's archive was unpacked, and `fm-editor.exe` with
its files (or `fm-editor`) lands beside the game's program. The Linux one is built on
Debian 11, like the game, and brings its own Python and Tk. Running it from
the source as above works too.

### Packs

The left list is the mod's packs in their order (`#`, name, price, cards a
pack, stock): **Add pack**, **Duplicate**, **Remove**, **Up**/**Down** (the
list's order is the order the game sells them in; `"order"` is written only
when set under Advanced). A pack the game would leave out is red, one with a
note amber; the Conflicts tab has the reader's words (packs.py says what the
game's Mods window would).

The simple view has what most packs need: **Name** (16 letters show; the
identity `mod-id:id` beside it stays when the name changes, since a save's
progress is kept by it), **Description**, **Price** in starchips and **Cards
a pack**. The picture is what the big card on the Password screen shows:
**Import PNG** (cut to 102:96 from the middle, kept at up to 4x; the console's
resolution makes it 102x96, Internal 2x and 4x draw its own detail), **Export**,
**Revert** (back to the cover card's art), with the name plate the game sets
in its serif font under it, at 1x, 2x and 4x. The cards: `#`, card, tier,
weight and **Chance**, the share of a slot dealt by the tiers' odds that is
this card, before any is taken out. **Add a card...** (the mod's own cards
too), **Add filtered...** (the Bulk fusions filters: every Dragon under 1500
ATK, say) into the chosen tier at the weight typed, **Tier**/**Set** moves the
selected rows, **Weight**/**Set**, **Remove selected**.

**Advanced** (closed at first):

* **Tiers**: name, odds (and their share), label (`"ULTRA RARE!"`), colour
  (the game's text colours, 0-15), sound, reveal and how many cards; **Add
  tier**, **Edit...**, **Remove**, **Up**/**Down**: the order is the rarity,
  commonest first. A one-pool pack becomes a pack of tiers with its pool the
  tier `cards`. A renamed tier is renamed in the slots, guarantee and pity.
* **Slots**: every slot by the tiers' odds, or a rule for each: a tier, tiers
  by weight, its own cards (`card=weight`), or always one card.
* **Dealing**: guarantee and pity (`tier=n`), max copies, stock, cost in
  cards (`card=copies` from the chest), order, shops, cover, duplicates
  (`unique_in_pack`), reveal (`flip`, `quick`, `list`), include the cards mods
  add, and **All owned** (`when_nothing_left`): with max copies, when the
  player holds that many of every card, `refuse` the pack (ALL OWNED on the
  screen, nothing paid) or `sell` it anyway, or `(shop's)` for Shop
  settings' rule.
* **Unlock**: beat (a duelist), wins, story flag (`0x6E0` + n is the n-th
  campaign duelist beaten), card and copies, starchips spent on packs, packs
  opened, opened (`pack=n`), and whether a locked pack is hidden or shown.
* **Password and sounds**: a password (said when a card has it too: the card
  comes first), once a save, in the list, and the five sound ids (empty for
  the Password screen's own).

**Shop settings...** edits `pack_shop`: what the Password screen sells (both,
packs only, passwords only), the random numbers (`game`, or `save` so a
reloaded save deals the same pack), the music, **All owned** (the rule for
the packs that do not say, `refuse` by default), and the shops, one a line
(`id | name | unlock as JSON`, sixteen at most); a shop's other keys (`where`,
and any the editor has no field for) stay as written. Not yet in the game, and so not offered (the keys
are kept free for them): a PACKS entry in the campaign's shop
(`campaign_shop`), the main menu, saving after each purchase (`autosave`),
selling mods' cards by their passwords (`sell_added_cards`), a currency of the
mod's own (`currency`) and a stock that comes back (`restock`).

**Simulate...** opens N packs (1000 at first) from a seed with the game's own
dealer (`packs.py` is `src/pc/cards/packs.c` in Python: four of the game's
random numbers a card, the guarantee and the pity redealing the last slots,
`unique_in_pack` and `max_copies` shrinking the pools), the pity counted from
one pack to the next as a save counts it, and lists the cards dealt by tier
and by card, how often a tier with a pity came on average and how often the
pity dealt it. The packs are opened a slice at a time, so the editor stays
free meanwhile: a bar shows how far it is and **Stop** shows what came so far
(at most a million packs, and five million cards in all, about a minute).
The tests hold the Python dealer to the C one's deals
(`tests/pc/packs_golden.txt`), line for line.

A field of the pack left as the tab showed it keeps the key as the mod wrote
it: opening a mod and moving through its packs changes nothing of it (a
`"cover": 2` stays a number), and **Apply** writes only the fields changed.
**Duplicate** gives the copy a picture of its own, so importing one for
either pack leaves the other's. When `packs` names a file of the mod, the
tab keeps it as written: a pack's fields and buttons are grey, and only
**Shop settings...** (the manifest's `pack_shop`) is offered.

## Tests

    python -m unittest discover -s tools/pc/fm_editor/tests -t tools/pc

(ctest `pc_fm_editor`). The tests build synthetic game files at the retail
offsets (`tests/fixtures.py`), art records included, and their PNGs in code
(`tests/map_fixture.py` adds the two overworld packages: a made-up table,
resource bank, strip and a one-quad HMD);
they need no game data (the bulk fusion tests time a 722 x 722 preview). PNGs are
read and written by `pngio.py`, in plain Python like the rest; the card-text
preview's tests build their font page and a TrueType file in code as well
(`tests/test_card_text.py`).

## Building another front end

The window is one front end over an engine that has no Tk in it. Another
front end (Qt, a web page, a script) reuses the engine as it is and replaces
only the window; it does not rewrite the engine, whose rules are the port's
(`tables.c`, `cards.c`, the loaders) and are pinned by the tests below.

**The engine** (no `tkinter` import; plain Python 3, nothing to install):

| Module | What it does |
|---|---|
| `disc.py` | finds and reads the game files (`load`, `find_game`, `user_dir`) into `GameFiles(slus, wa, source)` |
| `gamedata.py` | the retail tables from them: `load_game(files)` → `GameData`; the names of types, attributes, stars, duelists, pools |
| `model.py` | `Project`: the retail tables with the edits on top, and the edits themselves (below) |
| `manifest.py` | reading a mod folder (`open_mod`, `apply`) and writing one (`build`, `dumps`, `save_mod`) |
| `validate.py` | the loader's checks: `validate(project)` → `Issue` list |
| `pools.py`, `fixed_decks.py`, `bulk_fusions.py` | the port's pool arithmetic, fixed decks, bulk fusions |
| `art.py`, `campaign_map.py`, `map_art.py`, `map_view.py` | card art, the campaign map's table and pictures, the map drawn from the disc's 3D model (`map_view.py` has no Tk despite its name) |
| `guardian_stars.py`, `star_rules.py` | a mod's `guardian_stars` (the stars, the matchup grid, presets, checks) and setting many cards' stars by a rule |
| `card_text.py`, `ttf.py`, `pngio.py` | the card-text layout and picture, TrueType outlines, PNGs and the `Image` type every picture is |
| `importer.py`, `kit.py`, `ygomods.py` | importing a modified game, and converting a `.ygomods` package |
| `cli.py` | `check` and `import` (the window only through a lazy import) |

It needs `tools/pc/text_listing.py` beside the package (`gamedata.py`
finds it). **The Tk front end** is `app.py`, `tabs.py`, `widgets.py`,
`theme.py`, `art_tab.py`, `map_tab.py`, `fixed_deck_view.py`,
`bulk_dialog.py`, `guardian_stars_tab.py`, `star_rules_dialog.py`,
`preview.py`, `importers.py` (the File menu's import
dialogs) and `settings.py` (the window's own settings, no Tk).

**The whole round trip, with no Tk** (run from the source tree's root; it
prints `{"replace": 1, "attack": 3500}` in the mod.json for Blue-Eyes):

```python
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, "tools/pc")        # the folder holding fm_editor/ and text_listing.py
from fm_editor import disc, gamedata, manifest, validate
from fm_editor.model import Project

files = disc.load("game")             # a folder, a .bin or an ISO; disc.find_game() looks where the port does
retail = gamedata.load_game(files)    # the disc's tables, read once
project = Project(retail)             # or: project, notes = manifest.open_mod(retail, "path/to/mod")
project.info.id, project.info.name = "stronger-blue-eyes", "Stronger Blue-Eyes"

card = project.cards[1]               # Blue-Eyes White Dragon
project.cards[1] = card.copy(attack=card.attack + 500)

issues = validate.validate(project)   # the loader's checks: Issue(level, area, where, message, target)
for issue in issues:
    print(issue)
if not validate.errors(issues):
    path = manifest.save_mod(project, Path(tempfile.mkdtemp()) / project.info.id)
    print(path.read_text(encoding="utf-8"))

assert "tkinter" not in sys.modules
```

**Opening.** `gamedata.load_game(files)` once per game; then either
`Project(retail)` (a new mod, everything as retail) or
`manifest.open_mod(retail, folder)` → `(project, messages)`: the folder's
`mod.json` laid over retail, its texts, art and map taken back, and what
could not be read said in `messages` (show them). `open_mod` raises
`ValueError` or `OSError` for a `mod.json` that is not JSON or not there.
`project.source_dir` is where it came from; `project.retail` stays the
disc's, and every "changed" mark compares with it.

**Reading and changing.** Change the project, then redraw what shows it;
the engine keeps no undo (a front end may keep `project.clone()`s).

* Cards: `project.cards[id]` is a `gamedata.Card` (name, description,
  attack, defense, type, attribute, level, star1, star2, frame); replace it
  with `card.copy(field=value)`; a function's `card` argument below is the
  number, not the `Card`. `card_changed`, `revert_card`,
  `add_card(base, key)` (an added card, id 723 and up, in
  `project.added`), `remove_card`, `password`/`set_password`,
  `set_notes`, `card_label`, `model.card_matches` (the search).
* Fusions: `project.fusions[(low, high)] = result`, through
  `set_fusion(a, b, result or None)`; `fusion_status`, `revert_fusion`.
  `project.fusion_removes` lists the `{"remove": C}` cards in the mod's
  order (`remove_recipes`, `retail_recipes`, `active_removes`,
  `fusion_rule`: whether a pair is written, which the bulk count shares);
  `project.fusion_explicit` the pairs written whatever their result
  (`own_fusion_pairs`, `explicit_after_edit`).
  Bulk: `bulk_fusions.plan(project, BulkSpec(...))`, then `apply` and `undo`.
* Equips: `project.equips[equip]` is a set of monsters; `equip_baseline`
  is what the disc gives it. Rituals: `project.rituals[ritual] = (t1, t2,
  t3, result)`; `ritual_status`, `revert_ritual`.
* Duelists: `project.pools[duelist][pool]` is `{card: weight}` for the
  pools `gamedata.POOLS` (`"deck"`, `"pow"`, `"bcd"`, `"tec"`), out of
  2048; `pools.normalize`, `revert_pool`. A fixed deck:
  `fixed_decks.deck_of`, `set_deck(project, d, {card: copies})`,
  `most_likely`, `remove`.
* Starter decks: `project.starter`, a list of `model.StarterDeck`.
* Guardian Stars: `guardian_stars.read(project.other.get("guardian_stars"))`
  → a `Stars` to edit (`add_star`, `remove_star`, `set_default`,
  `preset_retail`, `preset_clear`), `.build()` back into
  `project.other["guardian_stars"]` (`None` when it would change nothing);
  `guardian_stars.choices(section)` names the stars a card may have.
  Many cards' stars by a rule: `star_rules.plan(project, spec)`, `apply`,
  `undo`.
* Mod info: `project.info` (`ModInfo`); other `mod.json` keys, kept as
  written, in `project.other`.
* Art: `art.set_image(project, card, part, image)` (part `"art"`,
  `"thumbnail"` or `"title"`; returns notes), `art.revert`,
  `art.changed_cards`.
* The map: `campaign_map.state(project).locations`, a list of 16
  `Location`s: store an edited `loc.copy()` back at its index;
  `campaign_map.reset`, `reset_all`. Its pictures: `map_art.set_sprite`,
  `set_texture`, `import_sprites`, `import_textures`, `revert_*`.
* Importing: `importer.import_modded(retail_files, modded_files, id,
  name)` → a result with `.project` and `.report` (saved with
  `importer.save`); `ygomods.import_package(retail, files.wa, path, id,
  name)` → `(project, report)`.

**Checking.** `validate.validate(project)` is every check the window's
Conflicts tab lists, `validate.errors(issues)` the ones the loader refuses,
`validate.validate_card(project, card)` one card's. An `Issue` has `level`
(`"error"`/`"warning"`), `area` (the tab: `"Cards"`, `"Fusions"`, `"Map"`...),
`where`, `message`, and `target`, what to select to show it (a card id, a
fusion pair, `(duelist, pool)`, a map place).

**Against the other mods.** `validate.cross_mod(project, folders=None,
settings_path=None)` is `(issues, summary)`: where the mod meets each mod
installed in `folders`, in the order the game would load them with the
player's Load order from the port's settings file (`MEMORIES_SETTINGS`, else
the user directory's `settings.txt`). `validate.mod_folders(project)` is
where the game finds mods: `MEMORIES_MODS_DIR` when set, else the mods
beside the game (`validate.shipped_folder()`: beside the editor's program,
beside the game files the port was last pointed at, or the source tree's
`mods/`) and the user directory's `mods`; and the folder the mod was opened
from, which is this mod whatever its id now. Every installed mod counts,
applied or not; the summary names the ones off in the game now
(`validate.applied`: `MEMORIES_MODS`, `MEMORIES_MOD_<ID>`, the player's
choice, `legacy_setting`, `enabled`) and those the game would not load (a
broken `mod.json`, which the game lists in its folder's place, over a
shipped copy of its id, but never loads; a missing requirement, or one left
out itself; a cycle). A card is named as the game will name it: the last
replace's name in load order, else the disc's. Its issues have `area` `"Other mods"` and
`level` `"warning"` (only one mod's change is used) or `"note"` (they add
up, agree, or follow an `after` the winner declared); a problem reading the
other mods is said beside them and never stops a mod opening.
`overlaps.py` is the check itself, the Python twin of the game's
`src/pc/mods/overlap.c`: `overlaps.check(mods, source, involving=None)` over
`overlaps.Mod`s in load order (with `involving`, only that mod's overlaps
are worked out), `overlaps.installed(folders)`, `overlaps.load_order(mods,
settings)`; it reads each file again only when it changed.
`overlaps.parse` reads a manifest as the game's json.c does (a `\u`
escape is one byte, a member named twice is met twice, a comma may close a
list), and a mod's name is cut to 95 bytes as the game keeps it, so the
lines match byte for byte. `tests/pc/mod_overlaps` holds four mods and the lines both must find, in
order and with their texts (`tests/test_overlaps.py`,
`tests/pc/mods_overlap_test.c`), so a rule changed in one and not the other
fails a test.

**Saving.** `manifest.save_mod(project, folder)` writes the art's PNGs and
texture pack first (that sets `"textures"`), then `mod.json`, holding only
what differs from retail, and on a save somewhere new copies the source
mod's other files. It refuses a folder that holds game files.
`manifest.dumps(manifest.build(project))` is the text it would write, for a
preview. **A front end never writes `mod.json`, the texture pack or the
mod's PNGs itself**: only `manifest.save_mod` (or `importer.save`), so the
diff, the order of the writes and what is kept as written stay the
engine's.

**Pictures.** Every picture is a `pngio.Image` (`width`, `height`,
`rgba` bytes); `pngio.encode(image)` makes PNG bytes any toolkit reads.

* Card text: `card_text.Renderer(card_text.RetailFont(files.wa), face).render(text,
  scale)` → `(image, layout)`, with `face` `None` for the retail font or a
  `ttf.Font(path)` (`card_text.port_face_path()` is the port's own);
  `layout` has the rows, the cut rows and the glyphs to mark.
* Art: `art.disc_image(files.wa, card, part)`, and `art.in_game(project,
  files.wa, card, part, scale)` as the game draws the mod's at 1x, 2x, 4x.
* The map: `map_view.render(map_view.model(files.wa, sector), camera,
  spotlight=place < campaign_map.TOWN_FIRST,
  overrides=map_art.texture_overrides(project, package))`, with `camera`
  `(distance, heading, pitch, target_x, target_z)` of the place and
  `(package, sector)` one of `campaign_map.PACKAGES`; `map_view.render_top`
  the world from above. The sprites, as `(image, left, top)`:
  `campaign_map.sprite_image(data, *campaign_map.PANEL, strips)` (or
  `MARKER`) and `arrow_image(data, arrow, strips)`, with `data`
  `campaign_map.state(project).retail` and `strips` the mod's strips,
  `{p: map_art.strip_override(project, p)}` for the palettes that have one.

**Still in the Tk layer** (a new front end redoes these, or they move to the
engine first):

* `App.save`: where a first save goes (an empty folder, or a folder named
  after the mod id inside the chosen one; the id must be letters, digits,
  `-` and `_`), and asking before replacing another mod's `mod.json`;
  `App.load_mod` wants a `mod.json` in the folder.
* Reading the forms: a password is up to 8 digits, padded with zeros
  (`CardsTab.apply`); an added card's key is `model.KEY_RE` and unique; a
  new card copies the selected card (not its base) and is named "... II";
  a pool weight is 0-65535, a deck's copies 0-40, a starter deck's weight
  0-`STARTER_WEIGHT_LIMIT`, and a card left at 0 is taken out of the pool
  or deck; "Add every"/"Remove every" of a monster type (`EquipsTab.by_type`);
  a card named by typing (`widgets.CardField.get`: a number, "7 Name",
  `Project.resolve`, then the exact name).
* `FixedDeckView.switch`: switching a duelist back to the weighted deck
  keeps its fixed deck aside until the mod is closed.
* `ModInfoTab.commit`: `settings` and the other keys parsed as JSON, and
  the keys the tabs own refused there.
* `MapTab`: the fields' ranges (-32768 to 32767, a flag up to `0x7FFF`,
  frames up to 255), a new exit's 16 frames; the Screen put together from
  the map picture, the name panel at `campaign_map.PANEL_AT`, the arrows
  and the marker at their sprite offsets; the Overview's geometry, and
  turning a drag into coordinates.
* `importers.ask_modded_files`: a modified `SLUS_014.11`'s `WA_MRG.MRG`
  looked for in `DATA/` beside it, then beside it.
* `preview.describe`: the card-text layout's marks in words.

**The tests a front end keeps passing** (the command above): `test_data`
(tables, the diff to `mod.json` and back, the pools' arithmetic, the
checks), `test_family` and `test_importer` (imports), `test_ygomods`,
`test_art`, `test_card_text`, `test_campaign_map` and `test_map_art` need no
Tk. `test_bulk_fusions`, `test_fixed_decks` and `test_starter` test the
engine and then the Tk dialogs, and `test_gui` and `test_map_gui` drive the
window; those Tk parts skip where Tk cannot start. A new front end adds its
own tests beside them and leaves the engine's as they are.
