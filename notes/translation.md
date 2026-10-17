# Translations

A mod can put the game in another language: every line of dialogue, every
menu string, every card's name and text, the monster types, the guardian
stars and the duelists' names. Accented letters work (é, ñ, ç, ü, ø, ß,
¿, ¡ and the rest of the Latin alphabets), and a mod can bring a font for
anything else. A renamed card's name plate, the name drawn into the top of
its art, is set anew from the new name at every scale (below). Other text
drawn as pictures (the main menu's words, the results screen's headings) is
not text to the game; a [texture pack](modding.md) repaints those.

## Making one

1. Write the game's text out of your own disc:

   ```sh
   python3 tools/pc/text_listing.py extract -o text.txt
   ```

   It finds the disc image in `game/` (or name it with `--exe`). The
   listing is plain UTF-8, about 10,000 lines.

2. Translate it in any text editor, keeping every `{...}` code and every
   `[ID]` and `{:L...}` line (see below).

3. Make a mod of it:

   ```json
   {
       "id": "spanish",
       "name": "Español",
       "description": "The whole game in Spanish.",
       "text": "text.txt"
   }
   ```

   `text` may be a list of files; they are read in order, and a later
   string with the same id replaces an earlier one. A jump to a place a
   file does not define lands in the latest file read before it (this
   mod's or an earlier mod's) that does. So may `font` (below).

   An entry may also be an object naming a setting of the mod, so the
   player can leave that file out, for example to keep the cards' English
   names:

   ```json
   "text": ["text.txt", {"file": "card_names.txt", "setting": "card_names"}],
   "settings": [
       {"key": "card_names", "label": "Translated card names", "type": "bool",
        "default": 1, "restart": true}
   ]
   ```

   The file is read only while the setting is not 0 (`mod.<id>.card_names`
   in the settings, or `MEMORIES_MOD_<ID>_CARD_NAMES=0` for one run). The
   text is built once, as the game starts, so changing it takes a restart;
   `"restart": true` is what says so beside the setting in the Mods window.
   A setting the mod does not declare is noted there, and the file read.

   With `"value"` the file is read only while the setting is exactly that
   value, so each choice of a `choice` setting can have its own file:

   ```json
   "text": [
       "text.txt",
       {"file": "people_us.txt", "setting": "people", "value": 0},
       {"file": "people_jp.txt", "setting": "people", "value": 1},
       {"file": "people_br.txt", "setting": "people", "value": 2}
   ],
   "settings": [
       {"key": "people", "label": "Character names", "type": "choice",
        "default": 0, "choices": ["Original (US)", "Romanized (JP)", "Localized"],
        "restart": true}
   ]
   ```

   `font` entries take `setting` and `value` the same way.
   A build older than this form reads the object as a file with no name:
   it notes `"text":  is not a file in the mod`, leaves that entry out and
   reads the rest.

   Save the files as UTF-8. A file in another encoding (Windows-1252, or
   what Notepad calls "Unicode") is reported, with the line where it goes
   wrong, rather than read as boxes.

4. Put the mod's folder in `mods` in the user directory, apply it in
   **Game > Mods** and restart. What the game could not read, and letters it
   has no way to draw, are listed beside the mod in the Mods window and in
   `MEMORIES_TRACE=mods`; everything else is used.

A translation may be partial: strings it leaves out stay as they are, and a
jump to a place no file has lands in the game's own text. The
listing itself must not be shared as it comes out of the tool: it is the
game's text. Share your translated file.

## The listing

```text
@bank dialog

[0501]
My dear prince!
Are you going to the city
to play cards again!?{page}You are of royal blood!
...{choice 02}«Run away»
«Keep listening»
{choose 80 L15DE L140F}

{:L140F}
The Pharaoh has gotten
wind of your activities...
```

* `@bank dialog`, `@bank descriptions` and `@bank names` start the three
  banks of the game's text: dialogue and menus; card texts; and card
  names, monster types, guardian stars, duelists and places.
* `[ID]` starts a string: its number, in hexadecimal. Its text starts on
  the next line. Card `n`'s name is `8000 + n` and its text `D100 + n`
  (`[8001]` and `[D101]` are Blue-eyes White Dragon's); the descriptions
  carry the card's name as a comment.
* A line break is a line break in the game's text box. Break the lines
  where they fit: a line wider than its box goes on at the start of the
  next row, which pushes the rest of the text down a row. Card texts have
  lines of 20 letters and room for 8 lines. A letter is 8 pixels wide, so
  a box has as many columns as its width in pixels over 8, and the width is
  the box's, not the string's: `[0021]`, the guardian star choice, has 22
  columns, and `[0022]`, the two-player duel's quit box, 10.
* In a menu with choices (`{choice ...}` then `{choose ...}`) that row is
  worse: when it pushes the choices past the bottom of the box, the console
  stops for good, with no frame after (the menu is laid out in one go and
  waits for a button nothing reads). The port cuts that line at the box's
  edge instead, and says so in `MEMORIES_TRACE=mods`; a menu that fits is
  drawn as the console draws it. Keep menu lines within their box. A line
  break of your own in a menu does not stop the game, but it counts as one
  of the choices' lines, so the last choice is lost.
* A menu with more lines than its box has rows under where it starts (four
  choices in the three rows left in the name box, or a menu that goes on
  from the rows of an earlier one) stops the console the same way. The port
  leaves the lines past the box out and offers only the choices in it
  (`{choose}` then takes the first targets), and says so in
  `MEMORIES_TRACE=mods`. Start a menu on a fresh box (after a `{page}`, or
  `{f8 1A}` as the name screen's own text does) and give it no more lines
  than the box has rows.
* Every menu's last choice line is followed by its `{choose 80 ...}`: once
  the player answers, the text goes on there, and the jump ends it (a `0`
  target lands on an `{end}`). Keep it. Without it the text runs on into
  whatever follows, and the screen that waits for the text to end waits
  forever: listings extracted before the fix of 2026-09-29 lacked
  `[00E3]`'s `{choose 80 0 0}` after QUIT (the password shop's
  EXCHANGE/QUIT: `text_listing.py` lost it, and `pal_text.c` the same jump
  in the European packs), and a translation written from one stops the game
  there once the player answers. The
  port ends such a menu's text where its jump should be, and says so in
  `MEMORIES_TRACE=mods`.
* A text box has room for so many letters at once: 254 in the dialogue
  box and some menus, 159 in most menus. What is past that on a page is
  left out, and a page with more than 254 is reported. A menu writes its
  text in one go: what is past its box's last line, or past a `{page}`, is
  left out too (the console would wait for a button there forever).
* A string ends at `{end}`, or at a code that jumps away (`{jump}`,
  `{choose}`, `{f8 17}`, `{f8 18}`, `{f8 28}`). What follows it up to the
  next `[ID]` or `{:L...}` is ignored, so blank lines and `# comments` can
  go there. A string that reaches the next item without one is reported:
  its blank lines and comments would be text.
* `{:LXXXX}` on a line of its own is a place something jumps to: a
  choice's answer, a branch, a shared ending. Its text belongs with it; keep
  the line. The listing names each after its place in the game's own text.
* `{cont}` marks a string that runs on into the next item without ending;
  keep the two in that order. Blank lines and `# comments` between it and
  the next item are ignored, as after `{end}`.

The codes:

| Code | What it is |
|---|---|
| `{page}` | wait for the button, then a fresh box |
| `{nl}` | a line break where the listing cannot write one (before a new item) |
| `{sp}` | a space at the end of a line, which an editor would strip |
| `{choice NN}` | the choice that follows: the next lines are its answers, one per line |
| `{choose 80 La Lb ...}` | where each answer goes, in order |
| `{jump L}`, `{call L}` | go on at `L`; insert the text at `L` (`{call L125A}` is the player's name) |
| `{if FFFF L}`, `{set FFFF}` | go to `L` if a story flag is set; set one |
| `{state ...}`, `{fx ...}` | pictures, pauses and effects of the story |
| `{f8 ...}` | the text's formatting and inserts: `{f8 00 20}` the card's name, `{f8 00 40}` its text, `{f8 03 ...}` a number, `{f8 0A NN}` a color, `{f8 01 NN}`/`{f8 02 NN}`/`{f8 06 ...}` positions, `{f8 0E ...}`/`{f8 10 ...}` music and sound |
| `{g NN}` | a glyph by its number: the few symbols with no character to type |

Move a code with the words it belongs to; do not change its numbers. The
retail glyphs can be typed as themselves: letters, digits, `! " # $ % & '
( ) * + , - . / : ; < = > ?`, `«` `»`, `·`, `α β γ`, `← →`, `♂ ♀`; typographic
quotes and dashes are taken as their plain ones.

## The port's own strings

A few words the port adds to the game's screens are drawn in the game's
letters, inside the game's picture, and a translation gives them in the
same listing, in the dialogue bank, with ids no retail string has
(`FE00`-`FEFF`, `TEXT_OWN_*` in `src/pc/text/text.h`). They are not in the
extracted listing; add them:

```text
@bank dialog

[FE00]
NEW{end}

[FE01]
%d MORE CARD{end}

[FE02]
%d MORE CARDS{end}

[FE03]
PAGE %d OF %d{end}

[FE10]
DECK SLOTS{end}

[FE11]
PAGE %d/%d{end}

[FE41]
Simon Muran{end}
```

| Id | Where | Room |
|---|---|---|
| `FE00` | Card drops' added result pages: after a card the player had none of | ends at the plate's end; each letter past 3 takes one from the card's name |
| `FE01`, `FE02` | the same pages' heading, left: one card past the first, or more | with `FE03` right-aligned on the same line: 33 letters for both, numbers and spaces included |
| `FE03` | the heading, right, when there is more than one page | as above |
| `FE10` | the card shop's menu (string `0011`): the entry under BUILD DECK | the menu's box shows 44 letters in all (spaces are none; 79 with a PAL language on, whose text entries are the PAL game's); retail's four lines have 35, so 9; a line is 15 wide |
| `FE11` | the Free Duel grid's page line, between the L1 and R1 hints, when a duelist mod gives the grid more than one page (`notes/more-duelists.md`) | centred on the picture between the two hints: 30 letters, numbers and spaces included, before it reaches them |
| `FE20`-`FE39` | the card packs on the Password screen ([card packs](card-packs.md)): `FE20` PACKS (after OK END), `FE21` BUY, `FE22` QUIT, `FE23` BACK, `FE24` INFO, `FE25` NEXT, `FE26` SKIP, `FE27` OK, `FE28` END, `FE29` SOLD OUT, `FE2A` LEFT %d, `FE2B` %d CARDS (`FE38` %d CARD for one), `FE2C` LOCKED; what opens a locked pack: `FE2D` BEAT %s, `FE2E` BEAT %s %d TIMES, `FE2F` WIN %d DUELS, `FE30` GO ON IN THE STORY, `FE31` HOLD %s, `FE32` HOLD %d %s, `FE33` SPEND %d MORE, `FE34` OPEN %d MORE PACKS, `FE35` OPEN %s x%d; the details: `FE36` AT LEAST %d %s, `FE37` %s IN %d PACKS; `FE39` ALL OWNED (why BUY is refused: every card of the pack held `max_copies` times) | the message box's line: 20 letters, a button's icon two; `%s` is a name (a duelist's, a card's, a pack's or a tier's), put in as the game writes it. Only shown when a mod sells packs |
| `FE70`-`FE7F` | the [tower](tower-mode.md)'s screen: `FE70` FLOOR %d OF %d: %s, `FE71` DUEL, `FE72` LEAVE, `FE73` NO FUSIONS, `FE74` NO EQUIPS, `FE75` LP YOU %d COM %d, `FE76` CLEARED x%d, `FE77` FLOOR %d CLEARED!, `FE78` %d STARCHIPS, `FE79` %s UNLOCKED, `FE7A` CHECKPOINT, `FE7B` THE TOWER IS CLEARED!, `FE7C` YOU LOST. BACK TO FLOOR %d., `FE7D` SAVE YOUR PROGRESS?, `FE7E` YES, `FE7F` NO | a box's line: 32 letters; `%s` is a duelist's name. Only shown when a mod has a tower |
| `FE41`-`FE67` | the opponent's name in place of COM (View > Opponent's name for COM): `FE40` + the duelist's id, 1-39 (the names bank's `8328` + id is the same duelist) | 14 letters, spaces and full stops (H.M. Anubisius, the longest English one); past that, the first 14 |

Letters and spaces only: a string with other codes is not used (the port's
English is). `%d` is where the port puts a number, in the order above
(`FE03` and `FE11`: the page, then how many). The headings are in the small letters,
which have no accents: an accented letter is drawn as its plain one.

The shop's menu with the entry is rebuilt from the translation's string
`0011` when it has one: its four lines as they are, `FE10` (or the English)
added under the second, centred as the others are, and its `{choice}` given
the fifth entry. A `0011` that is not four lines of letters, spaces,
`{f8 02}` steps and `{f8 0A}` colors, or five lines past the box's 44
letters, leaves the menu the translation's four entries, and the log
(`MEMORIES_TRACE=mods`) says so. A translation whose own four lines have
more letters than retail's has less room for `FE10`: the pt-BR menu has 38,
so 6.

The opponent's name in place of COM (`FE41`-`FE67`) is set in the text's
font, not the game's letters, so it may have accents (Simão): letters,
spaces and full stops of Latin-1; a string with anything else is not used
(the log says so). Without it, a translation that renames the duelist in
the names bank (`8329`-`834F`, as pt-BR's does) has that name shown, made
short as the English ones are (`Tables_ShortenName`): up to its first
character that is not a letter, a space or a full stop (Jono 2º Duelo:
Jono; an elided article before an apostrophe goes too, Sekmeton
l'Archimage: Sekmeton), whole up to 14; longer, its first words as initials when every word
starts with a capital (Sumo Mago Martis: S.M. Martis), else its last word
(Mago da Montanha: Montanha). A name the translation gives as the English
(Weevil Underwood) keeps the English short one (Weevil). Without either,
the English. The result screens show the same name over COM's column,
an accented letter as its plain one (their small letters have none).

A translation that gives the result screens words of its own (strings
`40`-`45`) gets the names there too (View > Opponent's name for COM), when
it calls its YOU, COM and WINNER the way the game's strings do: with `{call}`
to the strings it defines at `3E` (YOU, or 1P), `3F` (COM, or 2P) and `3D`
(`{f8 18}`: the winner's), by the labels `L050E`, `L051C` and `L0504`
(retail's places), or to the game's own where it defines none. The port
knows a call by where it lands, whatever the label is called, and measures
the word the translation has there, the string a 1P duel takes: VOCÊ is four
letters, so a player's name ends where it did, not where YOU did. The
`{f8 02 NN}` right before each call (or, failing it, four spaces) is the
step that moves; a string that builds YOU or COM in a way it cannot measure
(a call or a flag first) keeps its word. The translation's own word also
stays for what the small font cannot show, and when the player gave no name.
Strings `40`-`45` should be listed together: the copy takes up to 16 KB
of the file, from `40`'s first to the string after the last of them; a
longer span is said once (the log and the mod's notes) and has no names.
No shipped language (`languages/`) defines `40`-`45`, so with them the
results are the game's own strings, named as before; only a mod (the pt-BR
one) gives them words of its own.

| Id | Duelist | Id | Duelist | Id | Duelist |
|---|---|---|---|---|---|
| `FE41` | Simon Muran | `FE4E` | Yami Bakura | `FE5B` | Desert Mage |
| `FE42` | Teana | `FE4F` | Pegasus | `FE5C` | High Mage Martis |
| `FE43` | Jono | `FE50` | Isis | `FE5D` | Meadow Mage |
| `FE44` | Villager 1 | `FE51` | Kaiba | `FE5E` | High Mage Kepura |
| `FE45` | Villager 2 | `FE52` | Mage Soldier | `FE5F` | Labyrinth Mage |
| `FE46` | Villager 3 | `FE53` | Jono 2nd | `FE60` | Seto 2nd |
| `FE47` | Seto | `FE54` | Teana 2nd | `FE61` | Guardian Sebek |
| `FE48` | Heishin | `FE55` | Ocean Mage | `FE62` | Guardian Neku |
| `FE49` | Rex Raptor | `FE56` | High Mage Secmeton | `FE63` | Heishin 2nd |
| `FE4A` | Weevil Underwood | `FE57` | Forest Mage | `FE64` | Seto 3rd |
| `FE4B` | Mai Valentine | `FE58` | High Mage Anubisius | `FE65` | DarkNite |
| `FE4C` | Bandit Keith | `FE59` | Mountain Mage | `FE66` | Nitemare |
| `FE4D` | Shadi | `FE5A` | High Mage Atenza | `FE67` | Duel Master K |

The port's other words (the save slot and deck slot menus, the host
window's menus) are drawn in the host's font over the game, not in the
game's letters, and are not part of a translation's text.

## Letters

The game's font has 91 letters, none accented. The port draws more,
the first time a text uses them:

* **An accented letter** is the retail letter with its mark drawn on, in
  all three text sizes (16x16, 8x12 and the 8x8 of the duel results'
  headings): acute, grave, circumflex, diaeresis, tilde, ring, cedilla,
  caron, macron, breve, dot, double acute and ogonek, on any letter Unicode
  combines them with (251 of them: á, Ž, ő, ę, ǎ, ẽ...; Vietnamese below). Also
  ¿ ¡ ı ø Ø ł Ł đ Đ ħ Ħ, and `:` in the 8x8 font, which has none (two of its
  `·`, in the letters' colors).
* **How a mark fits** a cell with no room above the letter. In the 16x16
  font a capital is squeezed down to leave the mark its rows. The small
  fonts have none to spare: an 8x12 capital's outline is on the cell's top
  row (body 9 rows, a small letter's 7), an 8x8 capital's too (body 6, small
  4), so squeezing would leave a small letter. There the capital gives up
  **one** row of its body, the inner row most like a neighbour, nearer the
  middle on a tie (a thick stroke thins, a thin one stays), and the mark's
  two rows go on the cell's top two, its lower row where the letter's top
  outline was, touching the letter: É, Ê, Ã, Õ read as capitals, a row
  shorter than the others. A small letter whose outline row is where the
  mark's outline goes below it (é, ã, ô in 8x12, all of them in 8x8)
  shares that row instead of being squeezed. The 8x8 font's cedilla is one
  pixel on its bottom row, under the letter.
* **Vietnamese** (all 134 letters past ASCII: ả ạ ơ ư ế ặ ự...) is composed
  the same way, with three more marks and letters of two:
  * the **hook above** (ả) is drawn like the other marks; the **dot
    below** (ạ) goes on the row under the letter, the pixel of the letter
    above it turned to outline so it stays a dot (the small fonts' last row
    is the letters' foot); under a tail (ỵ) it goes beside it, under the
    right arm; the **horn** (ơ ư) is a stroke of two pixels (three in
    16x16) out and up from the letter's top right. Any letter Unicode gives
    one of these marks is drawn too (ḍ, ṣ, ṭ...).
  * **Two marks** (^, the breve or the horn, and a tone): in the 8x12 and
    8x8 fonts, which have no rows to stack them, the tone goes **beside**
    the ^ or breve, on its right, in the same two rows, as compact
    Vietnamese fonts do (Ấ is ^´ over A; the breve beside a tone is drawn
    as a v, so that the two fit), so a capital gives up only the row it
    gives up for one mark. In 16x16 the tone is **stacked** over the ^ or
    breve, to its right. The 8x8 font leaves the tone out (keeping the ^ or
    breve) if the two do not fit its cell; none of the Vietnamese ones needs
    to. Horn and dot below take no rows above, so ớ, ợ, ậ are a mark above
    and one of these.
  * The table (`src/pc/text/accents.inc`) is written by
    `tools/pc/glyph_accents.py` from Python's `unicodedata`.
  * HD text sets these from the font as it does é, fitted to the composed
    cell; the dot below touches the letter in ọ ộ ợ ụ ự ệ ỵ and their capitals
    (Ộ's circumflex too; readable, a drop under the letter, about 2% of the
    letters of a Vietnamese text) and ỵ reads poorly. At 1x they stand apart. A renamed card's
    title plate (Times, `cards/art.c`) fits a name whose marks reach past
    its top (two-mark capitals) into its rows instead of cutting them.
* **Text is composed (NFC)** when a listing is compiled: a letter followed
  by combining marks (U+0300-U+036F), as some editors and macOS save them,
  is one letter (e + U+0302 + U+0301 is ế), whatever order the marks are in;
  a mark with nothing to compose with stays a glyph of its own after it.
  Only the listing does this; a mod's card names and host menus read the
  UTF-8 as it is, so save those composed.
* `MEMORIES_GLYPH_SHEET=file.ppm` writes the pictures of every letter the
  text added (8x12, 16x16 and 8x8, one column each, outline black) the first
  time one is drawn, and `file.ppm.txt` with their code points: a way to see
  all of a translation's letters without finding each on screen.
* **ß ẞ æ Æ œ Œ ð Ð þ Þ º ª ° €** are built in, drawn from Noto Sans Bold
  (SIL Open Font License) and given the retail letters' outline and
  shading.
* **Anything else** (Greek, Cyrillic...) is set in a font: first the
  mod's `"font"` files (`.ttf`, `.otf`), then the system's sans-serif.
  Ship a font with the mod if it needs one: the system's differs between
  machines, and a machine may have none.

The added letters live in the software GPU's texture bank 15, not in the
console's VRAM, and take their colors from the text's own palettes, so they
fade, flash and change color as the retail ones do. Up to 672 of them.

## How the port does it

The text is data in the game's executable: three banks at `0x801B0000`
(menus and dialogue), `0x801C0000` (card texts) and `0x801D0000` (names),
each string found by its id through a table. Their bytecode is described in
[the text control codes](text-control-bytecode.md); `text_listing.py`
decodes all of it, following every jump from every string, and `check`
assembles the listing again and compares it with the retail bytes (all
128,172 of them match). Each item is compared from its start, so an item
cut short by an op the decoder never read would still match: `check` also
reports a text written to run on (`{cont}`) where the next item does not
begin, which is how `[00E3]`'s lost jump would have shown.

At startup `src/pc/text/translation.c` compiles each mod's listing
(`listing.c`) into a buffer of its own. The game turns a string id into
text in four places (`TextBox_BuildStep`, `Text_LookupString`, the
card-name and string inserts in `duel_effect_command.c`), which ask
`Text_Resolve` first. A jump in the game's text replaces the low 16 bits of
the text pointer, which only works inside a 64 KB bank; the compiled text
is anywhere and any size, so its jumps are indices into a table of its own
places, and the five jump handlers (`{jump}`, `{call}`, `{if}`, `{choose}`,
`{f8 17/18}`) ask `Text_Retarget`. A place the listing does not define is
the retail address, which is how `{call L125A}` still reaches the name the
game writes there. Each compiled file keeps its places, so a later file's
jump to one it does not define lands in the latest earlier file that does,
before it falls back to the retail text.

The game keeps a text box's letters in a slice of the entry table
`D_800EB288` (620 entries: 255, 160, 160 and 45 for the four text
channels; with a PAL language the PAL game's 800, see the official
languages below). The console's text always fits; the port's
`DuelEffect_AppendEntry` stops adding letters when the channel's slice is
full, rather than writing into the next channel's (or past the table), and
`func_80039A14`/`func_80039A60`, which build a menu's text in one go, stop
at a page that waits for a button (state 4) instead of looping forever.
A menu with choices has the same loop in `func_80039794`, which steps the
text unbounded while `flags_34 & 0x1000` (the choices' layout) is up; there
`TextBox_BuildStep` drops a letter past the box's right edge when its wrap
would leave the choices' last line below the box (`Text_CutsMenuGlyph`).
The other way into state 4 inside that loop is `Text_NewLine`: a new line
whose row is past the box before the menu's last line (`field_56 <
gDialog_bChoiceCount`, so `Text_TryCompleteChoiceLayout` does not take
over). There the port skips the wait and cuts the menu
(`src/pc/text/menu_cut.c`, by text channel `index_57`): the letters of the
lines after it are dropped in `TextBox_BuildStep`, and once the layout
completes `gDialog_bChoiceCount` is cut to the lines that were down less the
heading rows (`D_8009B34C & 0x30`), at least one. A menu that fits never
takes either path, so the console's frames are unchanged
(`tests/pc/menu_cut_test.c`). When the layout completes, `Text_NewLine` also
notes whether the text goes on with the menu's `{choose}` (`FB` with bit 7),
as all 127 of the game's menus do; if not, the first op `TextBox_BuildStep`
reads there after the answer ends the stream as `{end}` would
(`TextMenu_Unanswered`), rather than running on into the next string's menu
forever (a translation missing `[00E3]`'s jump). The Library's heading (string `F8`, "<seen/722>") is rewritten for the
number of cards there are, by its id, whether the text is the disc's or a
translation's (`Cards_Text`).

Glyph codes above the retail ones (`0x100` on, written `F1`-`F5` and a low
byte, which the game already reads as a glyph) have words of their own
(`Glyphs_Word`), and `func_80035E20` draws them from bank 15
(`Glyphs_Cell`). The 8x8 font is another path: `DuelEffect_AppendEntry`
keeps only the glyph's index in that font (bits 20-27 of its word, the
retail letter's for an accented one, 0 for `:`, which the retail game then
drops), so on the port it also writes the glyph's Shift-JIS into the entry
(`code_00`, which the retail 8x8 path leaves stale) and gives `:` a stand-in
index (`Glyphs_TinyIndex`); the draw asks `Glyphs_TinyCell`, which makes the
8x8 picture from the font at (704, 0) and puts it on page 4 of the bank,
with the 8x8 palettes (row `0xFA`, from x 656) copied beside the others.
Retail text never has an accented letter or `:` in the 8x8 font (its only
8x8 strings are the results' headings, YOU/COM and the ♂/♀ marks), so its
pictures are the same as before, byte for byte. The built-in letters (ß,
æ...) and characters set in a font (Greek...) still have no 8x8 picture and
are left out there, as before.
If a translation renames cards, their alphabetical order
(`gCard_asNameSortKey`) is worked out again from the new names, accents
sorting as their plain letters. A name listed as the disc has it, letter
for letter, renames nothing: when every card keeps its US name (a mod that
puts the English names back over a PAL language, say), the disc's own order
stays, which passes over hyphens (M-warrior #1 after Mushroom Man) where the
worked-out one would not. English (EU) is such a case: its pack renames no
card, and the PAL executables carry the US order table byte for byte. The names and texts of cards a mod adds
([more cards](more-cards.md)) are UTF-8 too and take the same letters.

The name on the top of a card's big picture (Triangle in a duel, the
Library, Build Deck) is not text either: it is a 96x14 4-bit plate in the
card's art record on the disc (`+0x2840`, [art.c](../src/pc/cards/art.c)),
drawn subtractively over the gold frame, and `func_800289BC` uploads it with
the picture. So when a translation rewrites a card's name (string
`0x8000 + id`, `Text_Overridden`), `Cards_PatchArtRecord`, which that loader
already calls before its uploads, puts a plate set from the new name in
place of the English one (`translated_plate` in `cards.c`, made once per
card by `CardArt_TitleFromName`, as the plate of a mod card with a name of
its own is). There is no background to keep: index 0 is clear and is the
whole of every retail plate's border, the gold showing through. The name is
set in the same serif face and layout HD text uses for titles (Times at 13
pixels, baseline under row 11, squeezed into columns 3 to 93 when longer
than 90 pixels), and each texel takes the plate ink of the nearest tone:
what inks 1 to 7 take from the gold was measured on a retail plate in the
game (1 all, 7 about a fifth), so stems land at 1 and edges at 6 and 7, as
the retail plates have them. This is the plate at 1x and at any internal
scale without HD text; with HD text at scale 2 and up, the title is set
from the name at that size over it, as before (`HdText_Title`). A card
whose translated name is the retail one, a card a mod's `cards[]` names
(its own plate wins), and a system with no serif face keep the plate they
had; with no mod renaming cards the art is the disc's, byte for byte.
`tests/pc/card_plate_test.c` (ctest `pc_card_plate`, where FreeType is
found) checks the plates: inks 0 to 7 only, clear edges, a long name
squeezed inside, accents inside the plate.

Running the recorded smoke cases with the untranslated listing installed
as a mod gives the same pictures as without it, byte for byte, jumps into
the listing's own text and to the player's name included.
`tests/pc/text_listing_test.c` (ctest `pc_text_listing`) covers the
compiler.

## The official languages

Game > Language (notes/pc-build.md, "Language") puts in the game the
European releases' own translations, English (Europe), French, German,
Italian and Spanish, handed at startup to the same machinery as a mod's
text. Their text ships with the port: `languages/en-eu.txt`, `fr.txt`,
`de.txt`, `it.txt` and `es.txt`, one listing a language in this file's
format, which the build copies beside the program. This is the one
exception to "never commit game data", agreed by the team: the text
only, as a transcription; the pictures with words and everything else on
the discs stay out. The packs are not written by hand: they are the port's
own reading of the PAL discs (`tools/pc/export_languages.py --discs
game/pal` writes them again; `--check` compares), so a fix to the reading
below is a fix to the packs once they are exported again. A PAL disc is
still read when a pack is not there.

**Where the text is.** Unlike the US disc, the PAL discs keep no text in
the executable. DATA/WA_MRG.MRG has a pack per language (English, French,
German, Italian, Spanish) from sector 6498, 110 sectors apart; after the
language's font and interface textures come three files, each a u16 id
(0x0F, 0x10 and 0x11, plus 3 a language) and a u16 0:

- A, 0xF000 bytes: a table of u16 offsets at +4, the menus (0x000-0x0FF),
  the card texts (0x100-0x3FF, the US `D100`+) and the story's lines
  (0x400-0x4F9, the US `0500`-`05F9`, into file B), then the menus and the
  card texts;
- B, 0x10000 bytes: the story's lines;
- C, 0x7180 bytes: the names (0x360, the US `8000`+), offsets as the US
  names bank's.

The string ids are the US ones. A disc of France, Germany, Italy or Spain
has all four languages (the Italian and Spanish ones the final text of
all four); the English one only English.

**How it is read** (src/pc/text/pal_text.c, src/pc/text/language.c). The
codes are the US text's, decoded as tools/pc/text_listing.py does, into a
listing in memory with the US ids that `TextListing_Compile` compiles; the
listing is, byte for byte, the one the research extractor wrote for each
of the five discs, but for the two points marked below. What differs from
the US text:

- `F8 1B` (no operand) is the player's name: `{call L125A}`;
- the name buffers are at A+`F800`, `F814` and `F848`: the US `122B`,
  `1238` and `125A`;
- `F8 03` reads its number 0x15D7C lower in RAM: moved to the US address;
- menus `06`, `10` (the debug menu with ENDING and LANGUAGE), `18`, `19`,
  `50`, `EE`-`F1` (the PAL name keyboard), `F3` and `F7` mean something
  else, or nothing, on PAL: they keep the US string; so do the result
  pages, `40`-`45` (below; the extractor swapped `40` and `44`), and `05`
  and `0E`, which have no words (the Library's card number and name, Build
  Deck's counts) and are laid out for the PAL's frames, not the US ones the
  port draws (below, "Frames");
- the menus' labels are renamed from `LF000` up, as the story's text in the
  same bank uses the offsets;
- `F8 1C`, which only the two-player results use, is left out: the US
  engine reads it as a glyph (phase 2; the extractor kept it);
- the glyph codes are the PAL executable's (its Shift-JIS table, read off
  the same disc), and the accented letters sit on placeholder codes that
  each language's font draws its own way (the tables in pal_text.c).

**Sources.** Where the text comes from is one table in language.c
(`sources`: whether a source has the language, and its listing): the pack
first, then the disc. `MEMORIES_EXPORT_LANGUAGES=<folder>` makes the game
write each language's listing from the first source that has it and exit;
the exporter runs it with `MEMORIES_LANGUAGES_DIR` on the discs, so a pack
is byte for byte the disc's listing, and pointed at the packs it shows they
read back unchanged. Each language is read off its own country's disc
(SLES-03947 to 03951); English (Europe) has one glyph with no character,
`{g 9C}` in the debug menu `51`. The port's own strings (below) are not in
the packs: they are added after either source. Images with words stay
disc-only, and English.

**Under the mods.** The language is compiled first and a mod's string
stands over it, string by string, so pt-BR over Spanish is pt-BR where it
has a string. Its labels are the PAL banks' offsets, so a mod's undefined
label still means the US text there, never the language's. With a language
on, the port's features that look for a translated string (the shop's menu
with DECK SLOTS, the opponent's name over COM on the results) behave as
with a full translation mod.

**The port's own strings** (FE00-FE10 above) come with the language
(`own_words` in `src/pc/text/language.c`, added after the pack or disc):

| | FE00 | FE01 / FE02 | FE03 | FE10 |
|---|---|---|---|---|
| French | NOUV. | `%d CARTE(S) DE PLUS` | `PAGE %d SUR %d` | JEUX |
| German | NEU | `%d WEITERE KARTE(N)` | `SEITE %d VON %d` | STAPEL |
| Italian | NUOVA | `%d CARTA/CARTE IN PIÙ` | `PAGINA %d DI %d` | MAZZI |
| Spanish | NUEVA | `%d CARTA(S) MÁS` | `PÁGINA %d DE %d` | MAZOS |

The card packs' words (FE20-FE38) come in all four, the QUIT and END the
Password screen's own (QUITTER and FIN, BEENDEN, ESCI, SALIR); the list is
`own_words` in `language.c`.

English (EU) says what the port's English says. FE10 is the game's own
word for the deck (CONSTRUIRE JEU, STAPEL ZUSAMMENSTELLEN, CREA MAZZO,
CREAR MAZO); the French NEW is cut short as the PAL text cuts words
(ESCI DAL NEGO.), since past three letters it takes room from the card's
name. The small letters of the headings draw the accented
capitals, so MÁS, PÁGINA and PIÙ show their accents. The shop's menu
fits the entry in every language because a PAL language also has the PAL
game's text entries (below). The opponent's name over COM is the names
bank's, shortened as a translation's is, except where that loses who it
is: French puts the name before the title, so the High Mages and the
Guardians have their names alone (Sekmeton, Anubis, Atenza, Marthis,
Képurah, Sébek, Néku, where shortening gave "Sekmeton l" and "Gardien"
twice), and Duel Master K is Maître K / Maestro K in French, Italian and
Spanish ("Duels", "K"); the Italian Ocean Mage is Oceano, as its other
mages are their places. These are `FE55`-`FE67` strings in language.c's
`own_names`, added after `own_words`.

**Text entries.** The PAL executables keep a text box's letters in 800
entries, sliced 280, 220, 220 and 80 for the four text channels (the
boundaries at file offset 0x82650 of SLES_039.47, 0x82A64 of the other
four), where the US has 620 (255, 160, 160, 45): the PAL text needs
them. The French card shop's menu alone has 54 letters, past the US
channel 3's 44, and its last line (QUITTER MAGASIN) was cut to QUIT. With
a PAL language on, the port lays the entries out as the PAL game does
(`src/pc/text/entry_layout.c`): the boundaries become the PAL ones and the
table is 800 entries at 0x801F8000-0x801FD780, guest RAM the US game
leaves free (the console's stack; the port's game runs on a stack of its
own, and the sound driver's music package at 0x801EA800 ends by
0x801F4800). The game reaches the table through `D_800EB288`, which
`duel_effect.h` makes the one in use for the port, and its scans of every
entry take the table's size; the listing compiler's page warning takes
the layout's pages (279 letters, menus 219). Which table is in use
follows the boundaries, and they travel with a save state: the port
writes the PAL ones as the game starts (`TextEntries_Start`, the entry
`main.c` gives `Memories_StateRunGame`), after its startup picture of
the game data, which is therefore the US one in every launch. A state
with the PAL boundaries keeps them when loaded; one with the US ones
takes this build's US ones (the state's rule for words the game never
changed). So a state goes on with the entries its boxes point into,
whichever language the game was launched in (checked: French to US,
saved again, and US to French). With English (US) nothing changes: the
table, its place and its slices are retail's.

**Save states** keep pointers into the language's compiled text, so a
state loads only in the language it was made in, and only while that text
(with the mods' text over it) is byte for byte what it was (pc-build.md,
"Save states"); the text sits at a fixed address, so the same language
finds it again at the next launch. Editing a mod's text or a pack makes
the states made before refuse to load, with a notice saying so.

**Widths.** The PAL text is longer than the US (French has about a hundred
dialogue lines past the US 36 columns) and still fits on the console:
the PAL font is spaced by letter. The EU executable's glyph routine
(func_80036A78, the US func_80036C14) returns an adjustment that
TextBox_BuildStep adds to the 8 pixels of a cell (0x5A): in the dialogue
boxes' mode a space takes 7 pixels, `f`, `i`, `l`, `.` and `,` take 6
(the PAL draws them a pixel left), and the apostrophe takes 2, drawn 3
left (the same code is in the French/German/Italian/Spanish executable).
The port draws the US letters where the PAL font has its own: `f`, `i`
and `l` a pixel left, as the PAL, the apostrophe 3, but `.` and `,` where
they are, since the PAL's sit a pixel further right in their cell. The
PAL's `i` and `l` have serifs that fill their 6 pixels; the US ones are a
2-pixel stem, so two of them side by side ("li", "il") left a wider gap
than other letters. With a PAL language on, the port draws its `i` and
`l` with the PAL's serifs (`src/pc/text/serif.c`, made from the US
letters when first drawn, as the accented letters are; nothing of either
font is in the source): a foot a pixel past the stem either side and a
serif a pixel left of its top (two in the 16x16 font, as in the PAL's
12x16), and so the accented letters made of them (`í`, `ì`, `î`, `ï`);
the acute over them sits a column left, on the stem, where the PAL has
it, clear of the stem by a row. The 8x8 font keeps its letters (the PAL's
has only digits), `f` keeps its own (its crossbar already fills the 6
pixels; the PAL's is another shape, with a tail), and `j`, `t` and `I`,
which the PAL narrows only in a mode the port does not use, stay as
they are. HD text sets the same serifs on the font's letters. English
(US), and a translation mod over it, draw nothing differently. Two
places keep the US `i` and `l`: the name entry's own sprites (the letter
under the cursor, the ones flying to and standing in the name box, which
`name_entry_runtime.c` draws straight from the font's page, not through
func_80035E20), and a texture pack that replaces the font: the serifed
letters are drawn from texture bank 15, as the accented ones are, and a
pack does not reach the bank, so with a PAL language a font pack's `i`
and `l` are the port's. The PAL
cuts a name by pixels (its `F8 07` counts 8 per unit,
the US one letters); with a PAL language on, the port reads the
limits the same way (`Language_PastWidth`): the card lists' 16 are 128
pixels, the duel bar's 24 are 192 (a German name like Doppelköpfiger
Donnerdrache stops after "Donnerdra", as on the console) and a magic
card's bar, `[0051]`, 28 are 224. The PAL text's own `F8 07` has one
operand byte; `pal_text.c` writes it as the US u16 (`{f8 07 1C 00}`),
and the PAL's `F8 00 03` (a card's type as a label) as the US `F8 00 01`. The boxes
are as wide as the US ones and wrap the same way (TextBox_WrapLineIfNeeded,
unchanged), so the text was written for that spacing: many PAL lines lean
on the box's edge to wrap. With a PAL language on, the port spaces
letters the same (`Language_Advance`, a hook in TextBox_BuildStep; not the
small letters of flag 0x100, nor 0x80), and a line breaks where the
console breaks it; with English (US) nothing changes. The PAL also has
line heights of 16 (US 12) in boxes 0x40 high (US 0x30), four lines
either way: the port keeps the US heights. A menu line that would still
pass its box is cut there (Text_CutsMenuGlyph), as a mod's.

**Frames.** The port draws the US screens, so the PAL text has to sit in
the US frames; `pal_text.c` writes the few codes that lay it out for the
PAL's own (checked screen by screen against English (US), the sweep in the
pull request that brought this). The PAL's `F8 04` has four sizes (EU
0x80038018: 0 the letters of its 8x16 cell, 1 the small ones, 2 a 12x16
cell, 3 a 16x16 one), the US one two (1 the small letters, 2 back to the
8x12 cell): the PAL's back-to-normal `0` is the US `2`, and its `2`,
which the card view's type and guardian stars are in, is the US letters
in cells of 8 with lines of 16, which puts the stars at +40 and +56 and
the card text at +80, where the US view has them (before, the text was 8
pixels higher, on the stone above it). The card text's own `F8 05 08 0D`
(13-pixel lines: the PAL's panel, box 0xD0 at y 6, is 16 pixels taller)
is the US 12, which fills the US panel with 8 lines. Six German card texts
have more than 8 (D10C, D111-D114 with 10, D1DA with 9; the PAL's panel
shows 9 and its tenth line is on a page the view never turns to): they
are laid out again in the US box's width, every word kept and each
letter's cell inside the box, a word the PAL cut with a hyphen at a line's
end made whole (Hochspannungs-/blitze), so D10C and D1DA take 8 lines;
the Exodia limbs still take 9, which are 11 pixels apart from 3 higher
(10 would draw over the accents of the line below), ending where the US
eighth line does. The Password screen's star chips are in a box the US
code sizes to big letters in cells of 16 (shop.c) and the EU code leaves
at its 8x16: a heading wider than the box in those cells, the French ÉCLAT
D'ÉTOILE, has its letters as close as it needs (11 pixels) instead of
wrapping onto the count. The duel's card bar on the field (`52`-`55`: a
line to the Swords' turns or GUARDIAN STAR, then the bar) goes 35 pixels
up in the PAL (`F8 01 DD`) and never back, its box being lower, so in the
US box the name, ATK/DEF and icons sat on the stone above the bar: it gets
the US lines there and back (`F8 01 E4` ... `F8 01 1C`) around the PAL's
words. Free Duel's record (`0C`) is, in every PAL language, the duelist's
name and, a line below, the wins and losses (Victoria/Derrota, Sieg/
Niederlage...) in colors at their own x, where the US has WIN and LOSS on
the name's line at x 0xA0: in the US box, one line high, they never showed.
They do not fit on the US line: the longest names take 144 to 162 pixels
at the PAL's spacing (Magier des Labyrinths, K le Maître des Duels) and
the words with three digits 123 to 199 of the box's 288. So with a PAL
language on, the box is the PAL's height, 32 from the same corner (the
PAL's own is 16, 204, 320x32; free_duel screen_runtime.c), and the second
line goes 4 pixels further down (`F8 01 04` after the break, `lower_lines`),
16 under the name as the PAL's lines are: at the US 12 it would sit on the
name plate's lower edge, at 16 it is on the stone under it, inside the
picture (ink y 222-234). The PAL's number format (`03`, the digits alone)
stays: on its own line nothing comes after it but the second word, at its
fixed x. English (US) keeps its box and line, pixel for pixel. Menus with
no words that the PAL lays out for its frames keep the US string (above).

Known in phase 1: RESULTS keeps the US pages (strings `40`-`45`, in
English, beside the language's YOU and COM columns). The PAL pages are laid
out by code of their own (func_80020EAC and the pages in reverse order):
their `40` begins with DEFENSE STATISTICS where the US has the win
condition, no PAL string says TOTAL ANNIHILATION, and `41`/`42` jump into
`40`'s tail, so under the US screens an attrition or Exodia win would show
one page twice and never the statistics. The images with words (main menu,
game over, the results' headings) stay English. The German Exodia limbs
(D111-D114), laid out again (above, "Frames"), read "Glied Wer das Siegel":
the PAL text has no full stop there, which its line break hid.

**Phase 2.** The PAL result pages (func_80020EAC's layout, then their
strings) and the other modes of the
spacing (2 and 3: the Library's title and a few menus); the images per
language, from SU.MRG (the main menu, sector 136 per language) and WA_MRG
(game over, sector 10135 + 41 per language) as a texture pack made at
startup; `F8 1C` in the two-player results; and the port's own strings in
French, German and Italian, reviewed by speakers.
