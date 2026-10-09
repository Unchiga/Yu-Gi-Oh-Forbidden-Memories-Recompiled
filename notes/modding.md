# Mods

A mod is a directory, not part of the game executable. The release ships its
own mods that way, and anyone else's mod is installed the same way: drop the
directory in, restart, apply it in **Game > Mods**.

## Where mods live

| Directory | What is in it |
|---|---|
| `mods/` beside the executable | the mods the release ships (`3d-monsters`, `hand-camera`, `ai-hard-mode`, `yamyi-mods`, `drop-missing-cards`) |
| `mods/` in the user directory | mods the player installed |

The user directory is where everything the player owns lives: settings,
controls, memory cards, save states, screenshots, their mods and whatever a
mod stores. It is `Documents\My Games\YFM Re-Decomp` on Windows and
`$XDG_DATA_HOME/YFM Re-Decomp` (`~/.local/share/YFM Re-Decomp`) elsewhere; see
[`src/pc/platform/paths.h`](../src/pc/platform/paths.h). A mod in the user
directory replaces one the release ships with the same `id`.

| Variable | Effect |
|---|---|
| `MEMORIES_USER_DIR` | the user directory, instead of the platform's |
| `MEMORIES_MODS_DIR` | the only directory scanned for mods |
| `MEMORIES_MODS=0` | load no mods at all this run |
| `MEMORIES_MOD_<ID>=0/1` | settle one mod for this run; the id uppercased, everything that is not a letter or a digit an underscore (`MEMORIES_MOD_3D_MONSTERS=1`) |
| `MEMORIES_MOD_<ID>_<KEY>=n` | one of a code mod's settings for this run, over what the settings file says (`MEMORIES_MOD_3D_MONSTERS_SCALE=5000`) |
| `MEMORIES_TRACE=mods` | log what the mod system finds, loads and overrides |

## The manifest

Every mod has a `mod.json` (UTF-8; a byte order mark, as some Windows
editors write, is fine):

```json
{
    "id": "card-tweaks",
    "name": "Card tweaks",
    "version": "1.0",
    "author": "someone",
    "description": "What it does, in a sentence.",
    "library": "card-tweaks",
    "enabled": false,
    "restart": false,
    "data": []
}
```

| Key | Meaning |
|---|---|
| `id` | the name the settings and the user directory use; the directory's name when it is left out |
| `name` | what the Mods window shows |
| `library` | the mod's code: an object file relative to its directory (a subdirectory is fine), `.o` added when the name has no `.` anywhere in it (`rules` is `rules.o`, `rules.v2` stays `rules.v2`); `build_mod.py` writes it under the same name. Linux and 32-bit Windows share one `.o`; macOS ARM64 needs a separately built `.dylib` (see [Native macOS ARM64 code mods](#native-macos-arm64-code-mods)). Leave it out for a mod that is only data |
| `enabled` | whether the mod is applied the first time the game sees it |
| `restart` | whether changing it needs a fresh process. Data overrides default to `true`, because the game reads most of what they change while it starts; code mods and `audio` default to `false` |
| `legacy_setting` | an older settings key to read the player's choice from, once |
| `data` | what the mod changes on the disc, below |
| `assets` | images the mod replaces by name: an entry each, or the name of a directory holding them, below |
| `textures` | a directory inside the mod holding a texture pack, below |
| `cards` | cards the mod adds after the disc's 722, and changes to the disc's own cards, below |
| `audio` | songs, XA clips and sound effects the mod replaces with WAV or Ogg files, below |
| `fusions`, `equips`, `rituals`, `drops`, `decks` | changes to the duel's rule tables, below |
| `chest_overflow` | how many copies of a card the chest keeps, and the starchips each one past that is worth, below |
| `terrain_bonus` | what each terrain gives each monster type, in place of the disc's +500 and -500, below |
| `equip_bonus_default` | what an equip adds when no `equips` entry sets its bonus, below |
| `trap_thresholds` | the attack each of the six attack traps stops, below |
| `passwords` | each card's password and starchip price on the Password screen, below |
| `limits` | the numbers the game caps (ATK and DEF, life points, starchips, the chest, the records), below |
| `guardian_stars` | the stars' names and icons, stars 11 to 15, and what each star gets against each other, below |
| `card_text_colors` | the color a card's name, description and guardian star are written in on its details, below |
| `palette_ramps` | the game's eight text color ramps, and ramps of the mod's own for its code to draw with, below |
| `starter` | the forty cards a new game begins with, one deck or a list of them, below |
| `packs`, `pack_shop` | card packs sold for starchips on the Password screen, and the shop's rules, below |
| `title`, `menu` | the title screen, and its two menus: their entries, buttons of the mod's own and their background, below |
| `text`, `font` | a translation of the game's text, and fonts for letters it has none of, below |

`version`, `author` and `description` are displayed in the manager. Version
bounds in `requires` are checked before activation. API 3 also supports
`min_api`, `game`, `requires`, `after`, `conflicts`, `priority` and declarative
`settings`; see [the API 3 guide](mod-api-3.md) for schemas and examples.

Any other top-level key is a warning beside the mod in the Mods window, with
the key it most likely meant: a manifest that says `"libary"` or `"texture"`
would otherwise load a mod that does nothing, without a word
(`unknown key 'libary' (did you mean 'library'?)`). `enabled` and `restart`
that are not `true` or `false` are warned about too. A warning never stops
the mod loading; the manifest keeps its warnings for the whole session,
however often the mod is applied.

Numbers are JSON's, in base 10, and must be whole (`1e3` is fine, `1.5` is
an error, and so is `010`); arrays and objects nest at most 64 deep. A
number written as a string (`"at": "0x5D800"`) may be hexadecimal with its
`0x`, and is otherwise decimal.

Two folders in one mods directory with the same `id` do not replace each
other: the first by name is kept and says which one was left out. Across
directories the player's copy still replaces the shipped one.

### Which game a mod needs

A game that does not know a key, or a value of one, plays the mod without
it: an unknown top-level key is a warning, an unknown value inside one is at
most a note, and the rest of the mod loads. That is a half-working mod. So a
mod that uses something newer than the oldest game it should run on says so
with `min_api`, and a game whose mod API is lower refuses the whole mod,
data mods included, with a line in the Mods window: `needs a newer game: mod
API 11, this one has 10. Update the game`. A code mod's `api` (below) is
checked as well, when it starts.

| `min_api` | Game | What a mod needs it for |
|---|---|---|
| 9 or less | v0.2.0 | everything not listed below; added cards, duelists, starter pools, packs, the title's menus |
| 10 | v0.2.1-preview.1 | `card_text_colors`; a card's `monster_effects` or `trap_threshold`; an equip's `bonus_attack`/`bonus_defense`; a pack's `image_style`; an added card (`copy`) of another kind than its base (a monster made a magic card, a magic card a monster or a trap), or whose `model` or `effect` is a card's name; the `MEMORIES_EVENT_MONSTER` event |
| 11 | the release after v0.2.1-preview.1 | `assets`, `ui`, `card_layout`, `palette_ramps`; a card's `tags`, or a `frame` of `Gold`, `Green`, `Pink` or `Blue`; a monster effect's `for_each`; `{"remove": "all"}` in `fusions`; a ritual of other than three tributes, or with `tributes_from` `hand` or `both`; the title's `images`, a `scale` in `title` or `menu`; the `limits` `deck_copies`, `swords_turns`, `crush_card`, `spellbinding_circle`, `shadow_spell`, `rank_score`, `starchip_prize` and `new_game_starchips` |

The FM Editor writes `min_api` on save as the lowest one of these that has
everything the mod uses, never lowering one written higher, and its Mod
info tab says which game that is and why (`tools/pc/fm_editor/compat.py`).
External `packs` files are inspected too, without rewriting them. An unreadable
or malformed file is an error in Conflicts and leaves the compatibility
requirement explicitly incomplete until fixed.
A mod written by hand should do the same. A release that adds a mod.json
feature an older game would leave out raises `MEMORIES_MOD_API`
(`src/pc/mods/mod_types.h`), lists the feature here and in `compat.py`,
and keeps every lower `min_api` loading.
When tagging the first API 11 release, replace the unreleased description in
the table and add that tag to `compat.RELEASES`; do not assign an unchosen
release number in advance.

## Data mods: no code at all

`data` is a list of entries, each naming a file on the disc by its retail
path (`"\\DATA\\CARD.MRG;1"`, as the game asks for it) or a raw sector
(`"lba"`), and either replacing it or patching bytes in it:

```json
"data": [
    { "file": "\\DATA\\CARD.MRG;1", "replace": "card.mrg" },
    { "file": "\\DATA\\WA_MRG.MRG;1",
      "patch": [ { "at": "0x5D800", "bytes": "26 25" } ] }
]
```

* `replace` names a file the mod ships. Named data files may be larger than
  the original: the port assigns a virtual disc extent and file lookup
  returns its position and exact byte size. The final sector is zero-padded;
  shorter replacements retain zero-filled sectors through the original
  allocation. Named replacements always require a restart, including when
  a manifest says `"restart": false`, because game and mod code cache file
  positions. The last replacement in startup load order wins, then patches
  apply on top. Competing replacements are reported in the Mods window,
  for named files and for raw sectors alike, and so are two mods patching
  the same bytes (the later one's bytes are the ones read; [When mods
  overlap](#when-mods-overlap)).
  Replacing a raw `lba` region needs a `sectors` count and must fit that
  allocation; an oversized raw replacement is rejected.
* The streamed files, `MASTER.XA` and `MOVIE.STR`, cannot be replaced or
  patched, by name or by `lba`: their sectors hold 2304 bytes of sound
  (MODE2 Form 2) where an override writes the 2048 of a data sector, so
  each sector would keep the tail of its old sound after the new bytes. A
  mod that tries says so in the Mods window and is not applied. Replace
  the sounds with [`audio`](#audio-songs-voices-and-sounds-from-files)
  instead; the movie's pictures cannot be replaced.
* `patch` writes `bytes` (hexadecimal, spaces optional) at `at`, an offset
  into the file, or into the sector when the entry names an `lba`. A run that
  crosses a sector boundary is fine. This is the shape the community's
  hex-editor tutorials are written in, so their offsets carry over directly
  (`modding-tutorial-gameplay-patches.md`).

Most of what the game's overlays hold is compiled into the port, so a patch
of an overlay's bytes on the disc reaches the port only where the port reads
them from memory as the console does. The campaign map's table of places,
exits, marker positions and cameras is one: it is read where the overworld
package puts it (`src/overlays/overworld/README.md`), so a patch of
`WA_MRG.MRG` at `0xFEC800 + 0x11A8` and `0x103B800 + 0x11A8` (before and after
the coup, 16 records of 66 bytes, `notes/overlays/campaign-map-records.md`)
changes the map. The FM Editor's Map tab writes those patches
([tools/pc/fm_editor](../tools/pc/fm_editor/README.md)). A save state holds the
table as it was in memory, so a state made without the map mod shows the
disc's map after loading, even with the mod on, until the game enters the map
again (`Main_RunCampaignMap` reads the overworld package each time it does).

The duel-effect bank is the other way round: it runs as native C only while
its bytes are the disc's. A disc patch that changes it is seen when the
package is read, and the delivered MIPS then runs in the interpreter, patch
included (`notes/pc-build.md`). The check happens at that read only, so a
code mod that writes into the bank's memory itself, after it is loaded, is
not seen and the native C still runs; such a mod patches the disc instead,
or sets `MEMORIES_DUEL_EFFECTS=interpreter` while it is developed. The
credits follow the same rule: the ending's 16-sector credits module at
`0x80180000` runs as native C only while its bytes are the disc's, a disc
patch of it is seen when it is read and then interpreted, and a code mod
that writes into its memory after the load is not seen; such a mod patches
the disc or sets `MEMORIES_CREDITS=interpreter`.

Named patches may address the expanded tail. They are checked against the
final selected replacement; a shorter replacement still allows patches
within the original file's byte length. Reads and raw patches at original
LBAs continue to address the original file's replacement prefix. A raw
patch crossing into the next file still affects that next file, rather than
the expanded tail. Code mods should obtain the effective LBA through
`host->disc_file_start` and use `host->disc_read` to read larger files.

Virtual allocation is bounded by the SDK's CD position format (last LBA
449849), the physical image's size, and other replacements. The port
reserves space for the largest enabled candidate for each file plus a guard
sector, so a failed mod can fall back without changing cached addresses.
Insufficient space rejects the replacement with a diagnostic; it never
silently truncates it. Save-state compatibility includes the replacement
contents and layout. Restart after changing replacement files.

Larger files do not automatically increase the game's model buffers or
change an MRG member's compiled offsets and transfer phases. Existing
record layouts continue to work; larger individual records require a loader
that requests and safely consumes their new layout. Higher-resolution PNG
textures already use texture packs, below. The implementation sequence and
remaining resource-loader work are in [the larger-file plan](larger-disc-files-plan.md).

Overrides stand in for the disc for every reader in the port: the drive
model, the bulk reads a mod makes, and the file lookup itself. Nothing on the
real disc image is touched, and removing the mod puts the game back exactly
as it was.

## Named assets: an image and the name of what it replaces

`assets` replaces one of the game's images with one of yours, named, with
nothing else to write: no directory to lay out and no `manifest.json`. The
engine already knows where every name sits on the disc, how wide it is, how
deep and which palette it is read through, so the mod only says which name
and which file:

```json
{
    "id": "my-ui",
    "name": "My UI",
    "assets": {
        "card_frames/frame_monster/column-0": {"image": "frames/monster-left.png"},
        "card_frames/frame_monster/column-1": {"image": "frames/monster-right.png"},
        "card_frames/level_star": {"image": "star.png"},
        "build_deck/cursor_bar": {"image": "cursor.png"},
        "duel/life_points_you": {"image": "life-points.png"}
    }
}
```

```
my-ui/
├── mod.json
├── star.png
├── cursor.png
├── life-points.png
└── frames/
    ├── monster-left.png
    └── monster-right.png
```

Each entry takes an `image`, a path relative to the mod's own directory (a
subdirectory is fine; a path that leaves the mod is refused), and an
optional `setting`, the key of one of the mod's `settings` that switches
that one image off, exactly as a pack entry's does.

### Or a directory, and nothing to list

`"assets"` may instead name a directory, and then the mod ships the pictures
rather than listing them: every PNG under it whose path is a name replaces
that image.

```json
{
    "id": "my-ui",
    "name": "My UI",
    "assets": "assets"
}
```

```
my-ui/
├── mod.json
└── assets/
    ├── build_deck/type_dragon.png       -> build_deck/type_dragon
    ├── card_frames/level_star.png       -> card_frames/level_star
    └── card_art/001.png               -> card_art/001
```

So `assets/build_deck/type_dragon.png` is `build_deck/type_dragon`: the path under
the directory, without `.png`. Nothing else is read, and nothing has to be
kept in step with the files — adding a picture adds the replacement, removing
it gives the image back to the disc. A file whose path is not a name is left
alone and counted in the mod's line in the Mods window, so a misspelt name is
told rather than silently ignored, and anything that is not a `.png` is
passed over. A directory that is empty, or not there yet, replaces nothing
and is not an error.

#### A directory for each part

A folder of the catalog switches with the mod's own setting of that name:
declare `attributes` and the player's switch takes `assets/attributes/...`
out, with nothing to nest.

```
my-mod/
├── mod.json                      settings: attributes, monster_type
└── assets/
    ├── attributes/light.png          switched by "attributes"
    └── monster_type/dragon.png       switched by "monster_type"
```

The name is read from the whole path either way, so a setting named after a
folder cannot change which image a file stands for.

To put several folders behind one switch, name a directory after the setting
and keep the names under it, as an entry's `"setting"` does:

```
my-ui/
├── mod.json                              settings: hd_art, hd_ui
└── assets/
    ├── hd_art/card_frames/level_star.png     switched by hd_art
    ├── hd_ui/monster_type/dragon.png         switched by hd_ui
    └── card_art/001.png                     no setting, always on
```

The whole path is tried as a name first, so a directory that is also a
folder of the catalog (`assets/card_art/...`) keeps meaning that folder; only
when the rest of the path is a name as well does the first part count as a
setting. A player turning the part off in the Mods window takes those
pictures out without a restart, exactly as a pack's entries go.

The two forms do the same thing and a mod picks one: the object when it wants
a `setting` on one image or its files laid out its own way, the directory
when it would rather just drop pictures in and switch them by folder. The FM Editor reads both, and writes
into the directory a mod already ships rather than converting it. A name the catalog does
not hold is an error naming it, not a silent no-op, so a typo is found when
the mod is applied rather than looked for on screen.

Everything the next section says about a pack's images holds for these:
any size, resampled at the console's resolution and sampled at its own at
Internal 2x and up, the palette rule, the alpha rule, and the load order
when two mods replace the same image. A mod may carry both `assets` and
`textures`; its own `assets` are applied over its own pack, and a mod later
in the load order still wins over both.

### The names

`docs/asset-catalog.json` is the catalog: every name with the geometry the
engine reads it by and a line saying what it is. `tools/pc/asset_catalog.py`
generates it, and `src/pc/render/asset_catalog.inc` beside it, from the same
layouts `tools/pc/extract_images.py` extracts by, so the two can never
disagree about where an image lives; `make check-asset-catalog` verifies the
tracked pair is current.

The folders are the parts a mod thinks in, not the shapes the disc keeps
them in: a card's picture, its thumbnail and its name plate share one record
on the disc but are three things to replace, so they are three folders.

| Folder | What it holds |
|---|---|
| `card_art/NNN` | the picture on a card |
| `thumbnails/NNN` | the small picture in the duel hand and on the field |
| `nameplate/NNN` | the card's name, written |
| `card_frames/...` | the card panel: the six frames, the card back, the level star, the ten ATK/DEF digits and the five words |
| `attributes/...` | the eight attribute balls |
| `star_guardians/...` | the ten Guardian Star symbols |
| `monster_type/...` | all twenty-four monster type icons, `dragon` to `equip` |
| `build_deck/...` | the Build Deck and Trade screens: the ATK and DEF marks, the count digits in six colors, the eight sort icons, the CHEST and ORDER boxes and the cursor bar |
| `duel/...` | the duel's HUD: the FIELD box, both life points panels and the turn arrows |
| `UI/...` | the sprites nearly every screen draws from one shared sheet: the pad glyphs, the arrows, the starchip, the DECK box, the 1P/2P badges |

Every name is one picture: a sprite cropped to its own rectangle of the
sheet it sits in, read through the one palette it is drawn with. The
screens' sheets themselves are not named, nor the duellists' portraits or
the story's scenes -- a column of a sheet is a slab many sprites share, and
each of its palettes is the key to a different set of them, so there is no
one picture to replace. A mod that wants those uses a texture pack, which
can address any words at all (below). The crop is per texel, so repainting
`monster_type/zombie` leaves the icon beside it in the same word alone, and
a sprite can be replaced without touching the screen around it.

A sprite is named for what it is, not for where it sits: the monster type
icons are `monster_type/dragon` to `monster_type/equip`, the Guardian
Stars `star_guardians/mars` to `star_guardians/venus` in the game's own
numbering, the sort icons `build_deck/sort_card_number`, `sort_attack` and the
rest by the order each one sorts the list into. A few are numbered because
nothing else would be honest: the turn arrow's eight frames
(`duel/turn_arrow-00`) are one picture's animation, and `build_deck/panel-NN`
is the Build Deck screen's furniture, which has no names of its own beyond
`build_deck/chest_box` and `build_deck/order_box`.

One name can stand for several readings of the same picture. Every
`card_frames` name covers all ten card UI packages the disc repeats — the Build
Deck screen's, the Library's, the Password screen's and one per duel terrain
— so a single `card_frames/level_star` repaints the star everywhere it is drawn.
The count the Mods window reports for such a mod is the images it replaced,
which is larger than the entries it wrote.

The images the names stand for are the game's own, so a mod ships painted
images or a way to make them from the player's disc, never the originals.

## Texture packs: images by origin

A mod may carry a `textures` directory: PNGs named by where their images
come from on the disc, with a `manifest.json` describing each one, exactly
what `tools/pc/extract_images.py` writes (`notes/pc-build.md`, "Images from
the disc"). Extract the family you want to repaint (`cards`, `portraits`,
`sheets` for the screens, `scenes` for the story's pictures, or `--assets`
for what a capture drew), paint over the PNGs, and point a manifest at the
directory:

```json
{
    "id": "hd-portraits",
    "name": "HD portraits",
    "textures": "images"
}
```

While the mod is applied, every upload the game makes from the disc is
traced to its bytes (`src/pc/render/texture_dump.c`), and the words an image
of the pack covers get its pixels in a shadow of VRAM; a primitive that
samples them through the palette the image was extracted with takes them
from the shadow instead (`texture_pack.c`). A pack image may be any size:
at the console's resolution it is resampled to the texture's own size, and
at an internal resolution (View > Internal 2x, 4x; `notes/pc-build.md`) it
is sampled at its own, so a bigger image shows its detail there. The
palette rule is what keeps a sprite the game draws through several
palettes (a selection bar, a greyed icon) looking right: only the palette
the image was made for is replaced. A pack may carry the same words
several times, one entry per palette (the `sheets` family writes a screen's
sheet once per way the game reads it), and the entry whose palette the
primitive uses is the one drawn; at the console's resolution only the first
of them shows, the scaled picture shows all. The packs of every enabled
mod add up. The extracted images themselves are the game's, so a pack
ships painted images or a way to make them from the player's own disc,
never the originals. The FM Editor's Art tab
([tools/pc/fm_editor](../tools/pc/fm_editor/README.md)) writes such a pack
for card pictures and thumbnails, a PNG at a time, and its Map tab for the
campaign map's sprites (the marker, the arrows, the name panel) and the
textures of its terrain.

A pack image does not need the extracted image's shape either: it is
stretched to the texture's width and rows (the crop's width, below), so a
4x image of a 102x96 card art is 408x384, and a wider or taller one is
squeezed to fit rather than cropped. With the OpenGL renderer an image
wider or taller than the driver's largest texture (`GL_MAX_TEXTURE_SIZE`,
16384 or 32768 on most) is averaged down to that size, with a line on the
console, rather than drawn black.

In the scaled picture (Internal 2x and up) a pixel's alpha is how much it
covers: below 8 it is transparent, and anything less than opaque is mixed
over what lies beneath (after the game's own blending), so letters and
outlines can have smooth edges. At the console's resolution a pixel with
alpha below half is transparent and every other pixel is drawn, black
included; there a replaced texel keeps the
game's semi-transparency bit, as on the PS1, so opaque black is the word
0x8000 where the game's texel has that bit and the darkest red, 0x0001,
where it has not (0x0000 is the PS1's transparent color).

When two enabled packs replace the same image read the same way (the same
archive offset, size, depth and palette), the one later in the mods' load
order is drawn: the higher `priority` number, or the player's Order in the
Mods window, or the one that names the other in `after`. That holds however
the packs were applied, on Linux and on Windows alike, and the Mods window
lists each such image ([When mods overlap](#when-mods-overlap)). Entries of different
sizes at one offset are not the same image; which one a texel comes from
follows their size, not the packs.

### The pack's manifest.json

`manifest.json` is an array with one object per image. The texture pack
loader (`src/pc/render/texture_pack.c`) reads these keys:

| Key | Meaning |
|---|---|
| `file` | the PNG, relative to the pack's directory; `..` and absolute paths are refused |
| `archive` | the archive on the disc the offsets are relative to, as the extractor names it (`WA_MRG.MRG`, looked up as `\DATA\WA_MRG.MRG;1`) |
| `offset` | the image's first byte in the archive |
| `words` | its width in 16-bit VRAM words (1 to 1024) |
| `rows` | its height (1 to 512) |
| `bpp` | 4, 8 or 16: how the game reads the words |
| `clut_offset` | the palette's first byte in the archive; only used when `clut_entries` is not 0 |
| `clut_entries` | the palette's size (16, 256), or 0 for a 16-bit image without one |
| `stride` | words from one row to the next in the archive (default `words`) |
| `row_offsets` | instead of a stride, each row's byte offset from `offset`: a list of exactly `rows` numbers, or `null` |
| `crop_left` | the first texel of each row the image covers (default 0) |
| `width` | how many texels from there it covers (default: the rest of the row) |
| `setting` | the key of one of the mod's declared `settings`: the entry is used only while that setting is not 0 (below) |

The extractor also writes `alias` (what the image is), `height` (the rows
again), and `sheet` and `column` (where a sheet's column stands, for
`upscale_pack.py`); the game does not read them. Numbers are whole numbers,
as everywhere in a manifest.

A pack can come in parts the player switches on and off. Declare a `bool`
setting per part in `mod.json` and give each entry of a part its key:

```json
"settings": [
    {"key": "card_art", "label": "Card art", "type": "bool", "default": 1},
    {"key": "portraits", "label": "Free Duel portraits", "type": "bool", "default": 1,
     "description": "The opponents' pictures on the Free Duel screen."}
]
```

An entry with `"setting": "portraits"` is used only while that setting is
on; an entry without `setting` always is. The settings show in the Mods
window like any mod's, and changing one loads the packs again at once, with
no restart (unless the setting or the mod says `"restart": true`). A pack
with every part off is applied with no images. A `setting` the mod does not
declare is reported with the pack's other problems, and its entry used.

An entry the loader cannot use is left out and counted, and the Mods window
shows one line for the pack, for example `2 images could not be read
(first: cards/001.png); 1 image is outside the pack (first: ../x.png)`: a
file that is missing, a path outside the pack, measures out of
range, a `row_offsets` list whose length is not `rows` (the image is then
read with the stride), an entry without `file` or `archive`, a `setting` the
mod does not declare (the image is still used), or more than 65535 images in
all. Loading does not open the images (thousands of opens held the frame
for seconds on a cold disc), only checks that each is there. PNGs are
decoded the first time the game needs them; a file that is not a PNG, or
fails to decode, is reported on the console then (`cannot be read`) and the
original texture stays.

`tools/pc/upscale_pack.py` makes a pack of upscaled images from an extracted
set with Upscayl's command-line binary (Real-ESRGAN on the GPU): the same
files and manifest, enlarged (`--scale 4` by default, one to one with
Internal 4x; `--scale 25 --passes 2` is the Upscayl window's "5x, twice"),
with `mod.json` written beside them and, with `--zip`, the mod folder in a
zip that unpacks into a `mods` directory (the user directory's, for anyone
who downloads it). Several extracted sets make one pack (`--images` again
for each); entries whose pictures are identical share one file and one
upscale. A screen the `sheets` family knows but no dump has shown yet gets
its sheets at a guessed reading; to be sure of one, play it once with
`MEMORIES_DUMP_TEXTURES=<dir>` from a cold boot and pass
`--variants <dir>/assets.txt` to the extractor: every depth and palette the
run read a sheet with becomes a PNG of it. `--assets <dir>/assets.txt`
instead writes what such a run drew, as it cut it, for whatever no family
covers. Keep the scale in proportion: the game holds a pack's images in
memory at full size, and a 128x256 sheet at 4x is 2 MB. `--cuts
<dir>/assets.txt` makes the upscale treat each piece the game cuts from a
sheet (a box's slices, the field's tiles, glyphs) as its own picture, so
no line shows where pieces meet.

The first attempt at a full HD pack, the problems it met and why its
result was not published are in `notes/image-remaster.md`: read it before
making another.

## Cards: more than the disc has

A mod may add cards with a `cards` list, no code needed. Each new card is a
copy of a retail card, which lends it its 3D model, fusions and effect; its
name, picture, text, stats, type, level, attribute and guardian stars can be
its own:

```json
"cards": [
    { "copy": "Kuriboh", "name": "Dingus Shmingus", "art": "images/dingus.png",
      "description": "A round and cheerful fellow who has never once been on time.",
      "level": 7, "attribute": "Fire", "stars": ["Moon", "Venus"], "attack": 2500 },
    { "copy": "Kuriboh", "count": 100, "name": "Kuriboh {n}" }
]
```

The cards take the ids after 722, in the mods' load order, and work
in the Library, Build Deck, duels, rewards, trades and saves.
An `art` PNG bigger than the card's 102x96 picture (408x384 is 4x) is also
drawn at its own resolution when the internal resolution is above 1x, with
no texture pack needed.
[More cards](more-cards.md) has every key, how a new card is won, where what
the save holds of them is kept, and how the port does it. Like data
overrides, a mod with cards needs a restart.

An entry with `replace` instead of `copy` changes a card of the disc in
place: the same keys, without `count`, and no new id.

```json
"cards": [
    { "replace": 1, "name": "Bulbasaur", "art": "images/bulbasaur.png",
      "description": "A strange seed was planted on its back at birth.",
      "type": "Plant", "attribute": "Earth", "attack": 1180, "defense": 1150 }
]
```

The name plate on the card's picture is set from the new name, as for an
added card, unless the entry has a `title` PNG.

A monster's `monster_effects` make it do something on the field: on summon,
on flip, at its owner's draw, before a battle, when destroyed or while face
up, a magic card's effect, a boost, healing or damage, which `for_each`
makes once per face-up monster counted ("+300 for each Dragon on your
field") ([Monster effects](more-cards.md#monster-effects)). A `description` may
hold icons and colors ([Card text codes](more-cards.md#card-text-codes)).

## Audio: songs, voices and sounds from files

A mod may replace the game's music, its XA streams (the recorded voices and
jingles on the disc) and its sound effects with ordinary WAV or Ogg Vorbis
files, from `mod.json` alone:

```json
"audio": {
    "music": { "0x000": "title.ogg",
               "0x010": { "file": "menu.wav", "loop": true, "loop_start": 44100 } },
    "xa":    { "0x8020": "fanfare.ogg" },
    "sfx":   { "0x007": { "file": "click.wav", "volume": 80 } }
}
```

Each key is an id, in hexadecimal with `0x` or in decimal (`"0x2D0"` and
`"720"` are the same id; a bare `2D0` is refused). Each value is a file name
relative to the mod's directory, or an object:

| Key | Meaning |
|---|---|
| `file` | the WAV or Ogg Vorbis file, inside the mod (`..` and absolute paths are refused) |
| `loop` | play again from `loop_start` when the end is reached; `true` by default for music, `false` for `xa` and `sfx` |
| `loop_start` | where the loop starts, in sample frames at 44.1 kHz (44100 is one second in), whatever the file's own rate |
| `volume` | percent of the file's own level, 0-400, default 100 |

A mod with only `audio` applies and removes live in **Game > Mods**, with no
restart; a song that is playing when the mod is applied or removed switches
at once. When several applied mods replace the same id, the one applied last
wins, as data overrides do (load order: `priority`, `after`, `requires`, then
discovery order); the Mods window warns about each id two mods replace.

### Finding the ids

Play with `MEMORIES_TRACE=mods` and the log names every id as it starts:

```
audio: music 0x0 starts
audio: sfx 0x7 plays (replaced by audio-replace)
audio: music 0x0 stops
audio: music 0x10 starts
audio: xa 0x8020 starts
```

Songs known so far: `0x000` is the title screen and both its menus, `0x010`
name entry after NEW GAME (`NameEntry_Init`).
A sound effect the game starts every frame is logged every 60th time.

* **music** is the sound driver's song number, `SD_BGMPlay(0x2D0)` is song
  `0x2D0` (the number is a song package times 16 plus a track in it). A
  replaced song's own sequence keeps running with its voices silenced, so
  everything the game times on it is unchanged; the file starts when the
  sequence does and stops when it stops, is reset, or reaches its end. The
  game's fades and its music volume apply, and so does the port's
  **Music** volume.
* **xa** ids are `0x8xxx`, `0x9xxx` or `0xAxxx`, as the game asks for them
  (the duel's `0x8020`-`0x8022`, for instance). The disc still reads the
  original clip at its own pace and its sound is dropped, because the game
  waits for the clip, not for the file: **the original clip's length decides
  when the game carries on**. A longer file keeps playing over what follows
  until the game stops XA playback or starts another clip; a shorter one
  leaves silence. Match the original's length where it matters. XA
  replacements go through the game's CD volume and fades and the port's
  **Stream** volume. An XA clip a song plays through (`SD_BGMPlay` of an XA
  id) is an `xa` entry too.
* **sfx** ids are the sound bank's, the number the trace prints. A replaced
  effect takes no SPU voice; it plays once with the game's volume and pan
  for it (and `loop: true` repeats it until every effect is keyed off, which
  most looped effects are not built for). The port's **SFX** volume applies.

### Formats and limits

* WAV: PCM 8, 16, 24 or 32-bit, or 32/64-bit float, including
  `WAVE_FORMAT_EXTENSIBLE`; any sample rate and up to 32 channels.
* More than two channels fold to stereo as a downmix does: the front pair
  as they are, a centre on both sides and surround, back and height
  channels on their own side, each at -3 dB, the LFE left out, and the sum
  scaled so that nothing clips. The speakers are a WAV's channel mask when
  it has one, else the usual order for the count (`L R C LFE Ls Rs`, then
  the sides for 7.1), and Vorbis's own order for Ogg files (`L C R Ls Rs
  LFE`). Channels past a known layout count as centres.
* Ogg Vorbis, decoded by [stb_vorbis](../src/pc/third_party/README.md)
  (public domain). Opus, MP3 and FLAC are not read.
* Files are decoded when the mod is applied, resampled to 44.1 kHz stereo
  (linearly) and kept in memory: about 10 MB a minute. A clip is at most 12
  minutes (an Ogg file's length is read from its last page, so a longer one
  is refused before it is decoded) and a file at most 256 MB. The 32-bit game has little room to
  spare, so prefer Ogg files for the mod and keep long songs few.
* A file that will not decode is skipped, with the reason beside the mod in
  the Mods window (and on stderr); the rest of the mod still applies.
* At most eight replaced effects sound at once; a ninth takes the oldest's
  place.
* Save states do not hold replacement sounds. Loading one starts the loaded
  game's song replacement from its top; an XA clip or effect that was playing
  is not resumed.

The same mod plays the same on Linux and Windows and with every audio
output (SDL, ALSA, the headless dump). `examples/mods/audio-replace` is a
worked example: run its `make_tone.py`, copy the directory into the user
mods folder and apply it. To check a replacement without speakers:

```sh
MEMORIES_MOD_AUDIO_REPLACE=1 MEMORIES_TRACE=mods MEMORIES_HEADLESS=1 \
MEMORIES_DUMP_AUDIO=out.raw MEMORIES_DUMP_FRAME=1500 MEMORIES_DUMP_PATH=out.ppm \
MEMORIES_INPUT="700:0008,706:0000" tmp/pc/game32/memories-pc
```

`out.raw` is s16le stereo at 44.1 kHz. How it is done:
[`src/pc/audio/replace.h`](../src/pc/audio/replace.h).
## The title screen

A mod may change the title screen with a `"title"` object: its song, the
intro before it, its background, its pictures -- the logo, PUSH START
BUTTON and the copyright line, moved, colored or replaced with the mod's
own -- which menu entries it offers, and lines of text of the mod's own.

```json
"title": {
    "music": "0x010",
    "skip_intro": true,
    "idle_seconds": 0,
    "background": {"image": "art/background.png", "dim": 64},
    "logo": {"image": "art/logo.png"},
    "prompt": {"image": "art/press-start.png", "y": -4},
    "copyright": {"tint": "#FFD060"},
    "entries": {"duel": {"hide": true}, "trade": {"hide": true},
                "options": {"tint": "#FF8080"}},
    "text": [{"text": "My mod 1.2", "x": 316, "y": 232, "align": "right"},
             {"text": "Press START", "y": 12, "show": "press_start", "color": "#FFE040"}]
}
```

Nothing on the title is text: the logo, PUSH START BUTTON, the copyright
line and every menu entry are pictures from `SU.MRG`. The background and
the three pictures take an `"image"` of the mod's own (below); the menu
entries' words are changed by a texture pack of the `sheets/menu` images
(above). Places are in the game's 320 x 240 picture, which grows with the
window and stays centred in widescreen.

| Key | Meaning |
|---|---|
| `music` | the song the title plays (retail `0x000`); an `audio` entry replaces a song's sound, this picks another song |
| `skip_intro` | `true`: go past the intro movie to the title at start-up and after a title jump |
| `press_start` | `false`: open on the menu, without PUSH START BUTTON |
| `idle_seconds` | seconds at PUSH START BUTTON before the intro plays again, `0` never (retail a little under a minute) |
| `background` | `image` (a PNG drawn over the whole screen instead of the hieroglyph wall), `picture` and `shade` (`false` leaves out the wall, or the mod's image, and the dark-to-light shade over it), `tint` (the wall's color, `#FFFFFF` as it is), `color` (a solid color under them, seen where they are left out or see-through), `dim` (how far the menu darkens the screen, `0` to `128`, retail `128`), `wide` and `wide_image` (widescreen, [below](#widescreen)) |
| `logo`, `copyright`, `prompt` | the three pictures: the logo, the (c) 1996 line and PUSH START BUTTON. `image` a PNG drawn instead, `width` and `height` its size, `x`, `y` move one from its place (`wide_x`, `wide_y` in widescreen), `tint` colors it, `hide` leaves it out (hiding `prompt` is `"press_start": false`), `show` when: `always`, `press_start` (not while a menu is up) or, for the logo and the copyright line, `menu` (only while one is) |
| `entries` | the menu entries by name: `new_game`, `load`, `duel`, `trade`, `options` before a game is loaded; `campaign`, `free_duel`, `build_deck`, `library`, `password`, `save` after (or their numbers, 0 to 10). Each may have `hide`, `x` (moved from the middle), `y` (its place) and `tint`, and all the keys of [the menus](#the-titles-menus)' items |
| `spacing` | how far apart the entries stand (retail 32) |
| `text` | lines drawn over the title, each `{"text", "x", "y", "align", "color", "size", "show"}`: `x` and `y` its place (default 160, 220, `y` the line's middle; `wide_x`, `wide_y` in widescreen), `align` `left`, `center` or `right` of `x`, `size` 1 to 8 (1 about the game's own letters), `show` `always`, `press_start` or `menu`; at most 16 |
| `images` | pictures of the mod's own over the title, each `{"image", "x", "y", "width", "height", "tint", "show"}`: `x` and `y` its middle (default 160, 120; `wide_x`, `wide_y` in widescreen), its size as the pictures' below (at most 320 x 240), `tint` its colors, `show` as a line's; at most 8. They go over the background, the logo and the copyright line, and under the menu's dimming, PUSH START BUTTON and the menus; the first given lowest |

A hidden entry is left out of its menu and the cursor steps over it; the
others close up, `spacing` apart around the middle of the retail menu,
unless given their own `y`. A menu with every entry hidden shows them all.
Hiding `load` hides the way to the second menu too, since loading a save is
what opens it. The tints multiply the picture's colors, so `#FFFFFF` leaves
one as it is and a color can only darken what is there; on PUSH START
BUTTON the tint goes over its pulse.

Every applied mod's `title` is read in load order each time the title
opens, a later mod's value winning key by key and the `text` lines and
`images` of all of them shown, so applying or removing a mod shows the next time the title
opens, with no restart. A key the title does not know, a color that is not
`#RRGGBB` or an entry that does not exist is noted beside the mod in the
Mods window. How it is done: [`src/pc/platform/title_screen.c`](../src/pc/platform/title_screen.c),
the pictures in `title_images.c`; the manifest is read by `title_config.c`,
checked by `tests/pc/title_config_test.c`. A code mod that wants more hooks the game's
own functions (`MainMenu_InitFrontendMenu`, `MainMenu_UpdateFrontendMenu`,
`MainMenu_DrawFrontendBackground`, API 4).

### The title's pictures

An `image` is a PNG in the mod, named relative to its directory. The
background's is stretched over the whole 320 x 240 screen, so draw it at
that shape: 1280 x 960 is four times the console's size. A picture's is
drawn with its middle where the game's picture has its middle (the logo's
at 162, 90, PUSH START BUTTON's at 160, 185, the copyright line's at 163,
207), moved by `x` and `y`. Its size is `width` by `height`, or one of them
and the PNG's shape; without either, the PNG's own size divided by the
smallest whole number that makes it fit -- 320 x 240 for the logo, 320 x
120 for the other two. That guess is right for a picture drawn at the
console's size, or one as wide as the screen at any multiple of it; for
anything else, say the size: a PUSH START BUTTON drawn at 4x, 800 x 64,
wants `"width": 200`. A see-through part of the PNG shows what is under
it.

At the console's resolution the picture is made into the game's own kind
of texture, 256 colors with anything under half covered clear, at the size
it is drawn; at an internal resolution above it (View > Internal 2x, 4x)
the PNG itself is drawn, at its own resolution and with soft edges, as a
mod card's art is. It is drawn where the game draws its own, so the menu
still goes over the logo and dims it, and PUSH START BUTTON still pulses
(its color is the game's pulse times its `tint`) and goes when START is
pressed. A PNG that cannot be read is noted beside the mod, and the game's
own picture shows. The pictures take VRAM the intro movie uses and the
title does not, uploaded again each time the title opens.

## The title's menus

The two menus on the title -- the first, after PUSH START BUTTON (NEW GAME,
LOAD, 2P DUEL, TRADE, OPTION), and the second, once a game is loaded
(CAMPAIGN to SAVE) -- take a `"menu"` object: buttons of the mod's own, the
game's entries changed, the order of both, and a background of their own.

```json
"menu": {
    "background": {"image": "art/menu.png", "shade": false, "dim": 0},
    "entries": {"trade": {"hide": true},
                "options": {"label": "SETTINGS"},
                "new_game": {"image": "art/new.png", "selected_image": "art/new-on.png"}},
    "buttons": [
        {"id": "credits", "label": "CREDITS", "notice": {"title": "Credits", "text": "Made by me."}},
        {"id": "gallery", "image": "art/gallery.png", "selected_image": "art/gallery-on.png",
         "action": "event", "value": 1},
        {"id": "quick", "menu": "second", "label": "QUICK DUEL", "action": "free_duel"}
    ],
    "order": {"first": ["new_game", "load", "credits", "gallery", "duel", "options"]}
}
```

| Key | Meaning |
|---|---|
| `buttons` | buttons the mod adds, at most 16 in all: each an `id` of its own (letters, digits, `_`, `-`), `menu` `first` (the default) or `second`, and the item keys below. A button is known to others as `"<mod id>:<id>"`: a later mod changes or hides one with that as its `id` |
| `entries` | the game's entries by name, as the title's `entries`, with the item keys below |
| `order` | the items top to bottom, as the cursor goes: `{"first": [...], "second": [...]}`, or a list alone for the first. A name is an entry's, a button's id (the mod's own) or `"<mod id>:<id>"`; the items it leaves out follow in the game's order, then the buttons as the mods made them. A later mod's list replaces an earlier one's |
| `spacing` | how far apart the items stand at their size 100 (retail 32; the same as the title's `spacing`) |
| `scale` | every item's size in percent, 25 to 400, but those with a `scale` of their own (100 as the game has it) |
| `background` | the background while a menu is up, as the title's `background` (`image`, `picture`, `shade`, `tint`, `color`, `dim`); what it leaves out is the title's |

Each item -- button or entry -- may have:

| Key | Meaning |
|---|---|
| `label` | its words, drawn on a frame in the entries' own look: dark in an olive rim, and red and orange with green letters while the cursor is on it. At most 31 letters, in the serif the card names are set in (Times; the system's sans without it) |
| `image`, `selected_image` | a PNG drawn instead, and another while the cursor is on it. Without `selected_image` the one picture is drawn darker while the cursor is elsewhere. Sized as the title's pictures are: `width` and `height`, one of them and the PNG's shape, or the PNG's own size divided by the smallest whole number that brings it to 32 rows or fewer -- so a picture drawn at 4 times the entries' 28 rows comes out their size. At most 256 x 64 |
| `action` | what choosing it does: an entry's name (what that entry does), `back`, `notice`, `quit`, `debug_menu`, `event` or `none`. An entry does its own unless given one; a button without one does nothing, or shows its `notice` |
| `notice` | the words of a `notice` action's box: text, or `{"title", "text"}` |
| `value` | a number handed to a code mod with `event` |
| `hide`, `x`, `y`, `tint` | as the title's entries: left out, moved from the middle (160), its middle's place, its colors multiplied |
| `wide_x`, `wide_y` | its `x` and `y` in widescreen, [below](#widescreen) |
| `scale` | its size in percent, 25 to 400, about its middle: the menu's `scale` without one. Words, frame and the cursor's look alike; a picture at that much of its size above |

The actions:

| Action | What it does |
|---|---|
| `new_game`, `options` | leaves the title for that, from either menu |
| `load`, `duel`, `trade` | the game's own dialog for it, in the first menu only |
| `campaign`, `free_duel`, `build_deck`, `library`, `password`, `save` | need a game loaded: in the second menu only |
| `back` | the first menu back to PUSH START BUTTON (a buzz when the title has none), the second back to the first, as Circle does |
| `notice` | a box over the picture with the item's `notice` and OK |
| `quit` | asks, then quits the game |
| `debug_menu` | the game's own debug menu |
| `event` | nothing but a code mod's `MEMORIES_EVENT_MENU` (below); a buzz when no mod takes it |
| `none` | a buzz |

An action a menu cannot take -- `campaign` in the first, say -- is noted
beside the mod and does nothing. The items drawn by the port (the ones
with a `label` or an `image`) slide in and out as the game's entries do,
from alternate sides, leaving the same afterimages; the game's own entries
are the game's sprites still, and a texture pack of `sheets/menu` changes
their words. Places are in the game's 320 x 240, as on the title. The shown
items stand `spacing` apart around the middle of the retail menu, closer
together when they would not fit between y 16 and 204; a `y` of the mod's
own stands. Each takes room as its size: two items' middles are `spacing`
times the half of each one's `scale` apart (32 between two at 100, 48
between two at 150, 40 between one at 100 and one at 150), so bigger
buttons push the others apart rather than run into them -- until the menu
would not fit, when all stand closer alike: kept between y 2 and 218 as
the game's 28-row entries are, the top one's middle half of 28 rows at
its size below y 2 (16 at 100) and the bottom one's as far above 218.
Fresh from PUSH START BUTTON the first menu opens on its top row; back
from a screen a choice opened, on the item it was chosen from.

A label's frame is made once as a PNG four times its size (more for one
drawn bigger: four times its size drawn), under the user directory's
`cache/menu-labels/`, and like an `image` is drawn at the console's
resolution as the game's own kind of texture and above it (View > Internal
2x, 4x) from the PNG itself. A `scale` other than 100 draws an item at
that size: a picture or a label made at it (at most 256 x 64 texels at the
console's resolution, stretched past that), and an entry of the game's own
by the port from the game's sprites -- the one it has while the cursor is
on it too -- each stretched about the entry's middle, with its afterimages.
A `scale` out of 25 to 400 is noted beside the mod and left out. The items' pictures share the
VRAM the title's pictures leave: about a dozen items with two pictures
each, more when the title has no background or logo picture of the mod's
own; one that finds no room is noted beside the mod.

### Widescreen

With View > Aspect at 16:9 the picture is 4/3 as wide: 54 of the game's
pixels more on either side of its 320, from x -54 to 374. By default the
title and its menus stay the game's 4:3 between black sides. A mod that
wants the room says so:

```json
"title": {"background": {"wide": true}},
"menu": {
    "background": {"image": "art/menu.png", "wide_image": "art/menu-wide.png"},
    "buttons": [{"id": "credits", "label": "CREDITS", "x": 0, "wide_x": 150}]
}
```

- **`wide`** on a `background` fills the sides: the game's hieroglyph wall
  tiles on into them, and the shade, the solid color and the menu's
  dimming widen with it. A 4:3 `image` is never stretched: it stays in the
  middle with the background's `color` (or black) beside it.
- **`wide_image`** is the background's picture for widescreen, drawn over
  the whole 428 x 240 (draw it at 1712 x 960, four times that); giving one
  turns `wide` on. The menus' background without one of its own uses the
  title's, unless it has a 4:3 `image` of its own.
- **`wide_x`, `wide_y`** are a second place, used instead of `x` and `y`
  while widescreen is on: on the menus' items (an offset from the middle and
  a middle, as `x` and `y` are), the logo, copyright and PUSH START BUTTON,
  and the `text` lines. What is left out keeps its 4:3 place (an item
  without `wide_y` its stacked one), so a mod can move only what it wants.

Items drawn by the port (a `label` or an `image`) can go right into the
sides. The game's own entries are sprites, which the widened picture clips
to the middle 320, so move an entry into the sides by giving it a picture
or a label. Turning widescreen on or off with the menu up moves the items
at once; the logo, copyright and PUSH START BUTTON, when the title opens
next.

A code mod takes a choice with `MEMORIES_EVENT_MENU` (API 9, [the API 3
guide](mod-api-3.md)): `a` the item (0 to 10 the entries, as
`MainMenuSelection`; 11 on the buttons, `host->menu_item(host, a)` names
it, `"my-mod:gallery"`), `b` the menu, `c` its `value`. Before, it may
handle it instead of the item's action, and set `result` to a choice the
title then slides out with and returns -- one of the game's, or a number of
the mod's own that its `MEMORIES_EVENT_SCENE` hook acts on (with `a` that
number); unhandled, a number the game does not know opens the debug menu.
The title comes back to the menu the choice was made in, on its item.

Every applied mod's `menu` is read with its `title`, each time the title
opens. How it is done: [`src/pc/platform/title_menu.c`](../src/pc/platform/title_menu.c)
(the cursor, the actions, the slides), the pictures in `title_images.c`,
the labels in `menu_label.c`; the manifest is read by `title_config.c` and
checked by `tests/pc/title_config_test.c`.

## Card layout: repositioning or hiding the big-card display

A mod may change where the elements of the "big card" display -- the card
viewer, and every other screen that shows one full card -- sit, or leave
some of them out, with a `"card_layout"` object, no code needed:

```json
"card_layout": {
    "frame_styles": {
        "gold":   {"image": "anime_frame_monster.png", "hand_color": "gold"},
        "green":  {"image": "anime_frame_magic.png",   "hand_color": "green"},
        "pink":   {"image": "anime_frame_trap.png",    "hand_color": "pink"},
        "orange": {"image": "anime_frame_orange.png",  "hand_color": "orange"}
    },
    "frame_for": [
        {"class": "ritual_spell",   "style": "green"},
        {"class": "effect_monster", "style": "orange"},
        {"class": "spell",          "style": "green"},
        {"class": "equip",          "style": "green"},
        {"class": "trap",           "style": "pink"},
        {"class": "monster",        "style": "gold"}
    ],
    "default_style": "gold",
    "art": {"x": 3, "y": 3, "width": 134, "height": 139},
    "attribute": {"x": 114, "y": 149},
    "atk": {"x": 38, "y": 179},
    "def": {"x": 102, "y": 179},
    "stars": {"x": 59, "y": 153},
    "spell": {
        "art": {"x": 3, "y": 3, "width": 134, "height": 139},
        "icon": {"x": 62, "y": 163}
    }
}
```

`attribute`/`spell`'s `icon` give a position but no `width`/`height`: those
frames cut no hole for them, so they draw at native size. Leave `attribute`
out entirely, though, and it inherits retail's title-plate spot -- now
covered by full-bleed's bigger art -- so it ends up hidden; giving it a
position in the stat band keeps it clear of `art`'s own rect.

`digits` (optional) replaces the ATK/DFD numbers with the mod's own while the
layout is on: `{"image": "digits.png", "width": 10, "height": 12, "step": 10}`.
The image is a strip of 5 x 4 cells of 40 x 48 texels: digit d in column
`d % 5` and row `d / 5`, and the same ten greyed two rows further down
(200 x 192; `tools/pc/card_digits.py` draws one from a font). The greyed ones
are the stat the attack screen dims, which keeps the card's own box and the
mod's font. `width`/`height` are one digit's size on the card, `step` the
distance between two digits' left edges (default `width`). Each number is
centred on its `atk`/`def` point. Left out, the retail digits are drawn.
`hd_assets_pack.py` copies `tools/pc/hd_recipes/anime_digits.png` in for the HD
mod (`--digit-font <ttf>` draws a new one).

`stars`'s `x`/`y` is the row's *centre*, like `atk`/`def`'s box centre
above, not retail's right-anchored first-star position: a card can carry
up to 12 stars at a fixed 9px each, and `func_80028B08.c` centres however
many a card has around this point so a wide row never runs past the frame.

Retail draws a small frame with a title plate, the card's picture at its
own fixed size, and ATK/DEF stacked under a separate plaque -- none of
which `"card_layout"` can change on its own. What it does is answer a mod's
own `full_bleed` setting (a `bool` the mod declares, as every setting is):
while that setting is on, the frame and title plate are left out, and the
rest move to the place given, in the same pixel neighbourhood retail draws
them in (`x`/`y`, with `width`/`height` on `art` and `attribute` stretching
the picture there instead of drawing it at its own size). Any key left out
-- the whole object included -- keeps retail's own place; a mod that only
sets `full_bleed` and gives no positions gets retail's own layout with
nothing hidden, which is a no-op, not a half-finished look. The description
box is always shown -- its own backdrop panel is on screen either way, so
there is nothing to gain by leaving its text out.

### Frame styles and the rules that pick them

A card is drawn in a frame **style**: a picture and the color its small card
wears in the hand, together, so the card view and the hand can never
disagree. Three keys say it:

- `frame_styles`: name -> `{"image", "hand_color", "width", "height"}`.
  `image` is relative to the mod's directory, as a `title` mod's pictures
  are, drawn as one textured quad behind everything else, at `width`/`height`
  (140x196 with neither given). `hand_color` (the older spelling
  `hand_colour` is still read; `hand_color` wins when a style has both)
  is one of the disc's six frame
  colors, `gold`, `green`, `pink`, `blue`, `purple` or `orange` (the colors
  its palette rows have: a monster is gold, magic and equip green, a trap
  pink, a ritual blue; purple and orange the disc never uses). Name a style
  as you like; a style named for a color (`gold`, ...) is also what a card
  whose own `frame` (a cards mod's, [Frame color](more-cards.md#frame-color))
  is that color wears.
- `frame_for`: a list of rules, the first that applies to a card wins. A rule
  says `"style"` and, to limit it, any of `"class"` (what the card is, below),
  `"tag"` (one of the card's [`tags`](more-cards.md#tags)) and `"setting"` (a
  `bool` the mod declares: the rule counts only while it is on, which is how a
  player gets a sub-option in the Mods window). A rule that names none applies
  to every card.
- `default_style`: the style of a card no rule picks (`gold` if left out).

The class of a card is worked out by the engine from its type and its
`monster_effects`, never written by a mod: `monster`, `effect_monster`,
`spell`, `equip`, `ritual_spell` or `trap`. It also decides the layout, not
the style: a spell, equip, ritual spell or trap has no ATK, DEF or level, and
reads `spell`'s `art` and `icon` below; a monster of any style keeps its ATK
and DEF.

Forbidden Memories HD follows the anime: a ritual spell is **green**, in the card
view and in the hand. Give the build a ritual picture (`--anime-frame-ritual`)
and it adds the style `blue` and the sub-option "Ritual spells: own frame" (off).
To keep ritual spells blue by default, put that rule's `"setting"` on and
declare its `default` as `1`, or delete the rule above it. A mod that adds
god monsters tags them (`"tags": ["god"]`) and adds a style and a rule for the
tag, before the class rules:

```json
"frame_styles": { "god": {"image": "god_frame.png", "hand_color": "purple"} },
"frame_for": [ {"tag": "god", "style": "god"} ]
```

An older layout with a `frame` per kind (`monster`, `magic`, `trap`, `ritual`,
`purple`, `orange`) and no `frame_styles` is read as before: the card's kind is
its own frame color, else its class's, a kind with no picture falls back to
`monster`'s, and a ritual spell with no ritual picture of its own (or magic's)
wears magic's. With no frame at all, nothing is drawn where the retail frame
was, which is a deliberate, supported look (a mod may want the art and stats
floating with no backing). The PNG is stretched to the texture's own
resolution regardless of its native size, the same rule a `title` mod's
`image` follows.

Whatever size the source art is, it lands on a fixed 177x254 texture
(`src/pc/cards/card_layout_art.c`'s `FRAME_W`/`FRAME_H`, the largest size
POLY_GT4's own UV coordinates can address) -- 919x1319, the size
`tools/pc/hd_recipes/anime_frame_*.png` are drawn at, is already a ~27x
reduction in area by the time it's on screen. That's a plain box-filter
average, which is lossless of whatever contrast is actually there (not a
quantizer bug -- checked by reimplementing the exact algorithm and by
running a bold checkerboard through the same pipeline, which stayed
perfectly crisp), but low-amplitude texture -- fine marbling, a soft
gradient a few pixels wide -- is exactly what that average erases first,
no matter how good it looks at full size. Draw frame art bolder than
looks necessary up close; `anime_frame_*.png` needed a contrast pass
(HSV-space unsharp mask plus a touch of marbling) after the first version
read as a flat color once actually in the game.

Magic, trap, ritual and equip cards have no level, ATK or DEF to draw --
`art`/`atk`/`def`/`stars` keep the monster layout, and `CARD_LAYOUT_ART`/`CARD_LAYOUT_
ATTRIBUTE` instead answer from `spell`'s `art`/`icon` for those four classes:
a different place for the same elements, not different ones --
func_80028B08.c still draws retail's own art/attribute textures there,
unchanged, just stretched into `spell`'s box instead of `art`'s/
`attribute`'s. Retail's elemental-attribute texture is not meaningful for
a non-monster card, so what actually shows in `icon`'s box today is
whatever that read happens to be -- a mod wanting something deliberate
there (a card-kind badge, say) has nowhere resident to read one from yet;
a Build Deck badge sheet and a duel-resident word texture were both tried
and backed out (wrong VRAM residency and wrong shape, respectively).

Only one mod's `card_layout` is read at a time -- the last applied one that
declares it, the same "a later mod wins" rule other singular keys follow --
so two layout mods together is the last one's layout, not a merge of both.
Forbidden Memories HD carries this as its own `full_bleed` setting, no code
of its own needed: `tools/pc/hd_assets_pack.py`'s `--anime-frame-<kind>`
flags default to `tools/pc/hd_recipes/anime_frame_<kind>.png` if present,
so a normal build picks this up with nothing extra to pass. `--anime-frame-<kind>
none` gives a kind no picture even where `hd_recipes` has one (effect monsters
then wear the monster frame). A model for writing another: a pure data mod, no
`library`, with one `full_bleed` setting, frame styles and rules, and the
positions above.

**Known limitation**: the frame image itself does not render in the duel's
own card viewer (opened from the hand) -- `CardLayout_DrawFrame` submits
through a raw depth value that bypasses this codebase's usual depth-sorting,
so it silently draws at the wrong depth there. Everything else (art, stats,
attribute, description) renders correctly in every viewer; only the
decorative backdrop is affected, and only in that one screen.

How it is done: the three retail call sites each ask one place
(`src/pc/cards/card_layout.c`'s `CardLayout_Get`/`CardLayout_FullBleed`/
`CardLayout_IsSpell`) for an element's place instead of carrying a mod's
logic themselves -- `src/game/func_80028B08.c` (title, stats, stars,
attribute, art, frame), `src/game/func_800283F4.c` (the description box,
always left visible today) and `src/game/duel_effect_resource_setup.c`'s
`func_800291E0` (the frame's own list membership). Each of the first two
call `CardLayout_SetCard` with the card it is about to draw before asking
for anything else -- its frame kind is `Cards_FrameColor` if a mod gave the
card one (whatever its real type), else bucketed from `Cards_Type` (equip
takes magic's), monster with no card named yet. None of those three change
at all with no `card_layout` mod applied -- `CardLayout_Get` answers with
retail's own constants, byte for byte. The frame texture is decoded once
into a VRAM bank by `src/pc/cards/card_layout_art.c`, the same shape
`src/pc/cards/star_icons.c` and `src/pc/text/glyphs.c` use for theirs,
re-decoding whenever `CardLayout_FramePath` answers a different file --
which a kind change already does, nothing further to invalidate. The
manifest key itself is checked by `tests/pc/card_layout_test.c`.

## The duel's pictures

A mod may move, size, color, hide or replace the duel's pictures with a
`"ui"` object: each half of the life-point panel with its digits, the FIELD
box, the card bar and the two cursors, and rearrange what the card bar
shows -- the card's name, its ATK and DEF, its icons. The FM Editor's UI
tab draws them from your disc and drags them about.

```json
"ui": {
    "duel": {
        "lp_opponent": {"y": 24, "tint": "#FF9090", "digits": "#FFE040", "label": "RIVAL"},
        "lp_player": {"y": -28, "scale": 120, "image": "art/lp.png"},
        "field": {"y": 40, "scale": 110, "tint": "#80C0FF"},
        "card_bar": {"tint": "#C0C0FF",
                     "name": {"x": 40, "tint": "#FFE040", "spacing": 1},
                     "atk": {"x": 55}, "def": {"x": 55},
                     "type": {"x": -237}, "stars": {"x": -237}, "kind": {"x": -237}},
        "hand_cursor": {"tint": "#FFFF40", "scale": 150},
        "field_cursor": {"hide": true}
    }
}
```

| Picture | What it is |
|---|---|
| `lp_opponent` | the panel's top half: LP, COM and the opponent's life points and deck count; up and down only, at most 161 % (201 % drawn from an `image`) |
| `lp_player` | its bottom half: YOU and the player's; the same |
| `field` | the FIELD box, with the terrain's name; up and down only, at most 130 % |
| `card_bar` | the strip under the hand; the hand's cards slide in and out with it, so it stays where the game has it, at its size: only its colors, a picture of its own or none. What it shows are its parts, below |
| `hand_cursor` | the red arrow under the card the cursor is on |
| `field_cursor` | the frame round the zone a card is going to |

| Key | Meaning |
|---|---|
| `x`, `y` | moved by so many of the game's pixels, -400 to 400 across (the cursors only), -300 to 300 down |
| `scale` | its size in percent, 25 to 400, about its middle (100 as it is; less for the panel's halves and the box, below) |
| `tint` | its colors multiplied, `#RRGGBB` (`#FFFFFF` as they are; a color can only darken what is there) |
| `hide` | `true`: not drawn |
| `image` | a PNG in the mod drawn instead, over the picture's place (moved and sized with it), `width` and `height` its size in the game's pixels instead of the picture's |
| `digits` | the LP halves: the color of their digits, over the game's (lit on that side's turn, dimmed on the other's) |
| `label` | the LP halves: words in place of COM or YOU, at most 15 letters, set in the font the card names are (with View > Opponent's name for COM the opponent's name shows there; a label wins) |

### The card bar's parts

What the card bar shows is not part of its picture: the game writes it over
the bar as one line of text (the card's name, the sword and ATK, the shield
and DEF, the icons), each time the cursor moves. Its parts are keys of
`card_bar`, each moved, colored or hidden on its own:

| Part | What it is, where the game puts it |
|---|---|
| `name` | the card's name, 8 x 12 letters from (16, 212); a monster's at most 24 letters, another card's 28 |
| `atk` | the sword and the ATK digits, from (211, 210) |
| `def` | the shield and the DEF digits, a row down |
| `type` | the icon of the card's type (Dragon, Spellcaster, Magic...), at (253, 210) |
| `stars` | a monster's two guardian stars at (271, 210) and (289, 210); on the field, the one it has on at (279, 210), and only that for an opponent's face-down monster |
| `kind` | a magic, trap, equip or ritual card's word, at (271, 210) |

Each takes `x` and `y` (moved by so many of the game's pixels), `tint`,
`hide`, and the name `spacing`: pixels added between its letters, -3 to 2
(each letter's place counts the spaces before it). A part stays on the
bar's dark panel, the same under the hand and on the bar's higher look
(over the hand, while a card is placed, a target chosen or the field
looked at from the hand; its words 87 higher): under the hand from x 14 to
306 and y 208 to 228, the name at its longest (28 letters, spread by its
`spacing`). The letters' cells are dark, so off the panel they would show
as dark boxes; a part moved further is drawn at the nearest place on it,
noted. The parts move as the bar does when it slides in and out. What is
left where the game has it: the Swords of Revealing Light line and the
GUARDIAN STAR words the bar shows over the field when a card is aimed at,
and the bar's own place and size. The parts can overlap: a long name runs
into what is moved where it reaches (the editor says so). The bar's own
`tint`, `hide` and `image` are its picture's only; its parts are drawn over
whichever it is.

Everything is done as the game draws the pictures, never to the game's own
objects, so its slides, the turn's colors and the cursors' moves go on as
they do. The game hides the panel and the FIELD box by sliding them off the
side of the screen (the panel right, the box left, for each battle and the
duel's end, and for Exodia a shorter way), so those move up and down only:
moved so, a half or the box slides with the game's, pixel for pixel, and is
off the screen when the game's is. An `x` for them is left out, and a size
that would still reach onto the screen where the game slides them to is
brought down to the most that does not (an LP half reaches 8 further with
a fifth digit, LP over 9999; a picture by its own `width`), both noted. A
piece of a sized one goes off the edge as the game's own would, where it is
drawn. The
digits and the label go with their half; the label is not drawn over a
half's `image`,
which has words of its own if it wants them, and the digits are. A moved
picture keeps its place among the others: the panel is still under the hand.
The pictures are drawn as the game draws them at the console's resolution;
above it (View > Internal 2x, 4x) a sized picture is not blurred, and the
mod's PNG is drawn at up to four times the size it takes (as many times as
fit in 1536 x 256 texels), as a texture of its own (the software GPU's
texture bank 14: nothing of the duel's VRAM is used). A PNG with no more
detail than the size it takes is drawn at that size.

Every applied mod's `ui` is read when a duel starts, a later mod's value
winning key by key. A key the game does not know, a value out of range or
a PNG it cannot read is noted beside the mod in the Mods window. How it is
done: [`src/pc/cards/duel_ui.c`](../src/pc/cards/duel_ui.c) (the card bar's
parts as its text is laid out, `DuelUi_TagEntry`, and drawn, `DuelUi_BarEntry`),
read by `src/pc/platform/ui_config.c`, checked by `tests/pc/ui_config_test.c`;
in a duel by `tests/pc/editor_cardbar_runtime.py`.

## Rules: fusions, equips, rituals, drops, decks and more

A mod may change what fuses into what, what an equip card may equip, what a
ritual needs and makes, what each opponent drops and what its deck is dealt
from, with no code and naming cards by name:

```json
"fusions": [ {"with": ["Kuriboh", "Mystical Elf"], "result": "Celtic Guardian"},
             {"with": ["Baby Dragon", "Time Wizard"], "result": null} ],
"equips":  [ {"card": "Legendary Sword", "add": ["Dragon"]} ],
"rituals": [ {"card": "Black Luster Ritual", "tributes": ["Gaia the Fierce Knight"],
              "tributes_from": "hand", "result": "Black Luster Soldier"} ],
"drops":   { "Simon Muran": {"pow": {"Blue-eyes White Dragon": 20}} },
"decks":   { "Heishin": {"Dark Magician": 60, "Kuriboh": 0} }
```

Weights are out of 2048, as the game's are, and the pools are always brought
back to 2048. A deck may be fixed instead, its forty cards counted out by
copies with no limit of three (`{"fixed": true, "Kuriboh": 4, ...}`).
`"chest_overflow": {"limit": 3, "starchips": 3}` keeps 3 copies of a card in
the chest and makes each card past them worth 3 starchips instead of lost.
`"terrain_bonus": {"Forest": {"Beast": 300, "Fairy": -200}}` sets, in points,
what a terrain gives a monster type.
A ritual takes one to five tributes, from the field (as on the disc), the
hand (`"tributes_from": "hand"`) or both (`"both"`).
An equip entry's `"bonus": 800` and `"bonus_if": {"Dragon": 1000, "Light": 700}`
set what it adds, in place of the disc's +500 (`"bonus_attack"` and
`"bonus_defense"` set ATK and DEF apart), and a top-level
`"equip_bonus_default": 700` what every other equip adds.
`"trap_thresholds": {"House of Adhesive Tape": 800}` sets, in points of ATK,
the attack each attack trap springs on.
`"passwords": {"Blue-eyes White Dragon": {"password": "00000001", "starchips": 100}}`
sets what the Password screen takes for a card and what it costs;
`"all": {"password": "card number", "starchips_percent": 10}` does every card.
`"limits": {"stats": 30000, "life_points": 16000}` raises the ATK and DEF cap
and the life points a duel starts with; `limits` also sets how far healing
goes, each side's and each duelist's LP, the two-player LP choice, the most
starchips, the chest and the Free Duel record, up to what the game keeps
them in (32767 for ATK, DEF and LP), and the duel's numbers take a fifth
digit where they need one. These are the rules a community mod
such as The Wicked Gods changes in its code; with them it plays close to its
own rules without C. The Wicked Gods also makes a monster's attribute count on
a terrain and lets monsters be equips: those need a code mod or the port
itself. Several mods' edits of the same opponent add up rather than
replace each other. A fusion, equip or ritual entry with `"setting"` is
read only while that setting of the mod is on, so the player can switch
groups of rules in the Mods window. [Gameplay tables](gameplay-tables.md) has every key, the
opponents' names, and how the rules combine. Like cards, they need a restart.

The [FM Editor](../tools/pc/fm_editor/README.md) (`python tools/pc/fm_editor`)
reads these tables and the cards out of the player's own game files and
writes a mod folder whose `mod.json` holds only what was changed.

## Guardian Stars: names, icons, new stars and matchups

The disc has ten stars in two cycles (Mars, Jupiter, Saturn, Uranus, Pluto,
Neptune; Mercury, Sun, Moon, Venus), each strong against the next in its
cycle. A mod may rename them, redraw their icons, add stars 11 to 15 and say
what every star gets against every other, which makes Pokémon types,
Digimon's Vaccine, Virus and Data, a pantheon or anything else:

```json
"guardian_stars": {
    "stars": [
        {"id": 11, "name": "Fire", "icon": "icons/fire.png", "beats": ["Grass"]},
        {"id": 12, "name": {"en-us": "Water", "fr": "Eau"}, "icon": "icons/water.png", "beats": ["Fire"]},
        {"id": 13, "name": "Grass", "icon": "icons/grass.png", "palette": "own", "beats": ["Water"]},
        {"id": 1, "name": "Ares"}
    ],
    "matchups": [ {"attacker": "Water", "defender": "Fire", "bonus": 1000} ],
    "default_bonus": 500,
    "replace": false,
    "choice": "ask"
}
```

What a matchup is. For each ORDERED pair, the attacker's star and the
defender's, the game has one signed number: what `Duel_CalcGuardianStarMatchup`
returns, the disc's +500, -500 or 0. The battle adds it to the attacker's
side of the comparison (capped at the ATK/DEF cap, a mod's
[`limits`](gameplay-tables.md), 9999 without one; there is no lower clamp),
as the disc adds its 500 ([the game](research/the-game.md), §5.8). So
"super-effective" is a pair at +1000, "neutral" 0, and "immune" is 0 too:
nothing here invents a battle rule the game does not have. The pair
backwards is its own entry, so a matchup may be one-sided.

| Key | Meaning |
|---|---|
| `matchups` | pairs: `attacker` and `defender` (a star's number or any of its names), `bonus` (points, -32767 to 32767; the default bonus when left out), `"mirror": true` to set the reverse pair to the opposite as well |
| `default_bonus` | what the disc's two cycles give instead of 500 (both signs), and what `beats` and a matchup with no `bonus` give |
| `replace` | `true`: every pair starts at 0, the disc's cycles gone |
| `stars` | declares a star: `id` 1 to 15, `name` (a string, or one per language: `en-us`, `en-eu`, `fr`, `de`, `it`, `es`, and `default`), `icon` (a PNG in the mod), `palette` (`game`, the default: the disc's stars' own 16 colors, as the game draws them; `own`: the PNG's, up to 15), and `beats` (stars it is strong against: +default for it, -default for them) |
| `choice` | at a summon: `ask` (the disc's SELECT A GUARDIAN STAR box), `first` (no box: the first star), `best` (no box: the star that does better against the opponent's face-up monsters, what it gains attacking them less what they gain attacking it; the first on a tie or with none) |

A minimal mod is one matchup; everything left out is the disc's. Where two
mods set the same pair the later one wins. Stars 11 to 15 are neutral against
every star until something says otherwise. A new star without a `name` is
"Star 11" (a translation's `[8322]` stands over that, since the names bank
has star N's name at `0x8317 + N`), and without an `icon` a plain disc in
the stars' colors. A card names them in its `stars` as it names the disc's
(`"stars": ["Fire", "Sun"]`, or `[11, 8]`).

One star. A card whose second star is none (`"stars": ["Fire", 0]`) or the
same as its first has only that one: there is no choice when it is summoned,
even with `ask`, and the card view shows one star. A first star of none with
a second (`[0, "Sun"]`) is the same one-star card, `["Sun", 0]`: the duel
reads the first star unless the second is chosen, so the game puts it first.
None is `0`, `null`, `"none"` or the FM Editor's `"(none)"` (a star a mod
names "None" is that star instead).

No star. A monster with both none (`"stars": [0, 0]`) has no star at all:
no SELECT A GUARDIAN STAR box at a summon, no star bonus given or taken (it
meets every star at 0 both ways, the AI's sums too), and nothing where a star
would be drawn: the field bar and the lists show no icon and no name, the
card view (the duel's, the Library's) lays it out as it does a magic card,
with no GUARDIAN STAR heading, and its battles have no star effect. This is decided for star 0
only once a mod has made such a monster, since the disc's arithmetic gives
star 0 a bonus against some stars (+500 against Mars, -500 against Pluto)
and nothing of the disc has it on a monster. A `replace` entry that turns a
magic, trap, ritual or equip card into a monster and says no `stars` still
takes its model's (or the Sun and the Moon); one that says `[0, 0]` has none.
No card of the disc is any of these, so without a mod nothing changes. A
`stars` that is not a list of two, or names no star, is noted in the Mods
window and left out, and the card keeps the stars it had.

The on-screen modifier climbs to the pair's own value, by 16 an update as on
the disc up to 512 and faster past it, so it never takes longer than the
disc's 32 updates; the yellow and red label goes by the sign, as before.

Icons are made at the console's size (16x16, 4 bits) in a texture bank of the
port's (`src/pc/cards/star_icons.c`) and drawn wherever the game draws a
star: the SELECT A GUARDIAN STAR box, the card view, the field bar, the
lists and the battle's star effect. The disc's ten are in the boot sheet
(`sheets/boot/a-c1-4-pb60500.png` from `tools/pc/extract_images.py`), so a
texture pack can also repaint them there at any resolution, one entry a
star: `"archive": "WA_MRG.MRG"`, `"offset"` `0xB51840 + ((N-1) % 8) * 8 +
((N-1) / 8) * 0x800` for star N, `"words": 4`, `"rows": 16`, `"stride": 64`,
`"bpp": 4`, `"clut_offset": 0xB60500`, `"clut_entries": 16` (the stars have
that palette to themselves, so nothing else changes). A mod's `icon` is not
drawn above the console's resolution yet: texture packs follow what the game
uploads to VRAM, and the bank the icons are made in has no such shadow.

The Mods window notes a star no card has, a card whose star no mod declares,
a declared star with no matchup, and a bonus past the stat cap. The
[FM Editor](../tools/pc/fm_editor/README.md)'s Guardian Stars tab edits all of
it, with the grid, and sets many cards' stars by attribute or type.

Limits. The stars are 4-bit fields of every card's stat word
(`gDuel_adwCardStats`, bits 18-21 and 22-25), so 15 is the most there can
be; more would need a wider card record in every table that holds one.
Every place that turns a star into a bonus, a name or an icon reads it from
that word by the card's id (`Duel_CalcGuardianStarBonus`,
`func_80023144`, `func_80037DA4`, and the AI's `func_80027DF8`), with the
record's flag `0x200` choosing the second: a future card effect that changes
a monster's star in a duel ("Terastalization") would give the duel record a
star of its own (it has three spare bytes, `pad_19`) and have those reads
take it first.

## Duelists: more than the disc has

The Free Duel grid holds forty because the disc lays out forty. A mod may add
its own past that, replace one of the disc's, and give each a deck, drop pools,
a portrait, an AI and conditions for when it appears — as files in four folders
beside the manifest, with no code at all.

```
shadow-duelists/
├── mod.json                     switches the mod on; nothing about duelists
├── duelists/dark-simon.json     who it is
├── decks/dark-simon.json        what it plays
├── drops/dark-simon.json        what you win from it
└── portraits/dark-simon.png     its face
```

The filename is the duelist's id, and anything missing falls back to the
duelist it copies. The same folders are read from the player's own directory,
so a character can be added without touching a mod.

[More duelists](more-duelists.md) has every property, what each folder holds,
how weights, unlocks, the AI row and the rank score work, and the disc layouts
the whole thing rests on.
## Starter decks: what a new game begins with

A mod may write down the forty cards a new game starts with, in place of the
seven weighted pools the disc draws them from. `starter` is one deck, or a
list of them, and one is picked for each new game:

```json
"starter": [
    { "name": "Spellbinder", "weight": 3, "Mystical Elf": 3, "Time Wizard": 3, "Yami": 3 },
    { "name": "Stone Wall", "Battle Warrior": 3, "Hard Armor": 3 }
]
```

A deck is its cards and their copies, adding up to forty, with cards named as
`decks` and `drops` name them. `"name"` is the deck's own, for the Mods window
and the log; `"weight"` is how often it is the deck picked, against every other
offered deck (1 without one, and 0 for a deck kept in the manifest but never
picked). The decks of every applied mod add up.

Writing the cards down, rather than weighting a pool, is what lets a starting
deck hold a card a mod added: the disc's pools are a fixed 722 weights whose
generator reads only the first 720, so no weight can name one. The copies are
the deck, so the three-copy limit does not apply here any more than it does to
a fixed opponent deck — but a deck Build Deck would refuse to take back, with
more than three copies of a card or more than one Exodia piece, says so in the
Mods window and is dealt as written. A deck that is not forty cards is left
out. [The starter deck](starter-deck.md) has the rest, including what it costs
the game's random numbers; `examples/mods/starter-deck` is a working one.

A mod may weight pools of its own instead, with `starter_pools`: a list of
pools, each drawing its own number of cards from its own weights, whose draws
add up to the forty a deck holds.

```json
"starter_pools": [
    {"name": "Weak monsters", "draws": 16, "cards": {"Mystical Elf": 100, "Baby Dragon": 60}},
    {"draws": 24, "cards": {"Dark Magician": 1}}
]
```

They are the disc's seven rows made a mod's to write, and they lift what the
disc's cannot do: a pool here names cards as the rest of a manifest does, so a
card a mod added may be weighted like any other. A written `starter` deck
still wins -- the game asks for one first, then for these, and reads the
disc's rows only when neither is offered. A draw that keeps finding a card
already held three times is retried a bounded number of times, so a pool of
three cards or fewer cannot hang a new game.

## Card packs: booster packs for starchips

A mod may sell packs of cards. The smallest is one line of a list:

```json
"packs": [
    {"name": "Dragons", "price": 50, "cards": ["Blue-eyes White Dragon", "Baby Dragon", "Koumori Dragon"]}
]
```

The Password screen then says △PACKS beside ✕OK ○END; △ opens the packs, ←/→
go through them, ✕ buys one and the big card turns each card over, in the
game's own card, boxes, letters and sounds. The starchips count down as a
password's price does, and the cards go into the chest before the first turns
over. Every option has a default: tiers with odds, a rule per slot, a
guarantee, a pity count, no repeats in a pack, a limit on copies held (and
whether a pack with nothing left for the player is still sold), a stock,
unlock conditions, a secret password, cards from the chest as part of
the price, an image of the pack's own (as the card's art, or with
`"image_style": "full"` the whole picture where the card is drawn), the
reveal and the sounds; and
`pack_shop` sets whether the screen sells passwords, packs or both, several
shops, and whether a save can reroll a pack. The packs of every applied mod
add up; they need a restart, like the tables.

[Card packs](card-packs.md) has every key, how a pack is dealt (always four of
the game's random numbers a card), and what the save keeps.

## Translations

A mod may put the game's text in another language: dialogue, menus, card
names and texts, types and duelists, accented letters included.
`tools/pc/text_listing.py extract` writes the text out of the player's disc
as an editable UTF-8 listing; the mod ships the translated file:

```json
{ "id": "spanish", "name": "Español", "text": "text.txt" }
```

[Translations](translation.md) describes the listing, its codes, the
letters the port draws and how a font is added. Like cards, a translation
needs a restart.

A `text` or `font` entry may be `{"file": "names.txt", "setting": "card_names"}`
instead of a name, which lets the player switch that file off, as a pack
entry's `setting` does (above): it is read only while the mod's declared
setting of that key is not 0. With `"value": N` as well it is read only
while the setting is exactly N, which gives each choice of a `choice`
setting a file of its own ([Translations](translation.md) has an example).
The text is built once, as the game starts,
so a mod with `text` or `font` asks for a restart on its own, and so does a
change to any of its settings (the setting may still say `"restart": true`,
which shows "Requires a restart" beside it). A `setting` the mod does not
declare is noted beside the mod and the file read.

## Card text colors

Every mod may give card-detail text its own one of the game's eight color
ramps. Put `card_text_colors` at the top level of `mod.json`; it needs no
library, setting, or extra file. `cards` is a list of rules. `card` accepts a
card number, its displayed name, or an added card identity. Each of `name`,
`description`, and `guardian_star` is optional, so a rule can color any one
or all three parts. Colors are ramp numbers 0 through 7: white, yellow,
blue, green, grey, orange, red, and the unused eighth ramp respectively.

```json
"card_text_colors": {
  "cards": [
    {"card": "Blue-Eyes White Dragon", "name": 2, "description": 0, "guardian_star": 1},
    {"card": "my-cards:dragon:1", "name": 6}
  ],
  "guardian_stars": [
    {"star": "Mars", "color": 5}
  ]
}
```

`guardian_stars` is optional. It colors that star wherever it appears; a
card's own `guardian_star` rule takes precedence. When two enabled mods set
the same card part or guardian star, the later mod in load order wins. The
rules are read when cards are built, so changing them requires a restart.

Yamyi Mods has an optional **Rarity card-name colors** setting for compatibility
with its older `card_name_color.ini` system. It is off by default. When enabled,
that INI may name or define color slots and assign colors by rarity tier or
per-card override; those colors are applied after `card_text_colors` and
therefore override the manifest color for the card name only. Description and
guardian-star colors continue to use `card_text_colors`.

## When mods overlap

Two enabled mods may change the same thing. Nothing stops that, and nothing
is refused: the game reads every mod in load order and each key decides what
happens, usually the later mod winning. The Mods window says where it
happens. Its **Compatibility** tab lists, by kind and with counts,
everything the selected mod changes that another enabled mod changes too,
one line each: what it is, which mods, and how it comes out. The first four
lines of a kind show; **and N more** shows the rest (up to 200 at a time). The
header counts the overlaps between all enabled mods. A line is in the warning
color when only one mod's change is used, and dim when the changes add up,
agree, or follow an order the winning mod asked for:

```
Fusion 'Kuriboh' + 'Mystical Elf' (Alpha, Beta, Gamma): Gamma wins (later in load order)
Simon Muran's POW drops (Alpha, Beta): both apply and add up (each edits the pool as the mods before left it)
Card 'Blue-eyes White Dragon' (Alpha, Beta): the later mod's description is used; the rest combines
menu.spacing (Beta, Gamma): Gamma wins (it loads after Beta on purpose: after/requires)
```

Load order is the one in the window, before Apply: enabling, disabling or
moving a mod's **Load order**, or changing a setting that switches an entry
on or off, shows the new outcome at once. (What applies live, without a
restart -- sounds, code hooks, data a mod says needs no restart -- goes in
after the mods already in place until the game restarts, whatever the
order says; the restart puts it right.) The list is worked out when the
enabled mods, their order, their settings or the code mods' hooks change,
not every frame, so dozens of mods with thousands of entries each cost
nothing while the window is open (two mods of every pair of 400 cards'
fusions, 79,800 overlaps, take about a tenth of a second).
`MEMORIES_TRACE=mods` writes every line to the log (`overlap: warning: Cards:
...`), for the mods applied at startup and again whenever the window's set
changes.

What counts as the same thing, and how each comes out, as each key's reader
decides it:

| Kind | The same thing | How it comes out |
|---|---|---|
| `data` | a disc file, by name as the disc's lookup takes it (letter case counts; leading backslashes and the `;1` aside), or raw sectors, which meet where their runs of sectors do (raw sectors and a file by name never meet, though the game reads a raw replacement over that file's sectors: the window does not know where the disc's files lie) | replacements: the later is read; patches: the later's bytes where two patch the same bytes, else both apply; a patch over another mod's replacement is written into it, at the disc's offsets (a warning) |
| `audio` | a `music`, `xa` or `sfx` id | the later is heard; two files are never the same sound, whatever their names, each being its mod's own |
| `textures`, `assets` | an image read the same way: archive, offset, size, stride (none for one read row by row, `row_offsets`), depth and palette (an entry the loader leaves out, or whose part is switched off, is not counted); a named asset and a pack entry over the same reading are the same image | the later is drawn; one line per image |
| `cards` | one of the disc's 722 a `replace` names (an added card's identity cannot be replaced) | the later in load order; the line names whose keys are used and whose are dropped. Its stats, stars, frame, model and effect go over the earlier's; its name, text, password, art, plate (`title`), field art and fusion groups too, and when it leaves one out the earlier's is dropped all the same: every replace of a card starts those from the disc; notes add up |
| `fusions` | a pair, in either order | the later's result; `remove`s add up |
| `equips` | an equip card, and its copies (a rule for the card is one for each copy of it, as the game matches the card or its base) | for each monster, the latest entry that says something about it decides (a card before a type before `"replace"` within an entry), so a later rule for a type, or a plain `bonus`, goes over an earlier rule for a card of it, or an earlier `bonus_if`; entries about other monsters add up |
| `equip_bonus_default` | | the later |
| `rituals` | a ritual card | the later's recipe (its tributes, where they come from, and its result) |
| `drops`, `decks` | an opponent's pool, named by name, number, `"all"` (every opponent another mod names) or, in a mod's `drops/` and `decks/` folders, by a duelist of the mod's own (whom it replaces, and the line names him as the mod did, or itself); a pool that is not an object of cards is left out, as the game leaves it | edits add up, each on the pool as the mods before left it; a later `"replace": true` empties it first, and an earlier `"all"` with `"replace": true` empties every pool, so it meets a later mod's edit of any one; a fixed deck (exactly 40 cards) wins over every weighted edit, the later fixed deck over an earlier |
| `starter` | | the decks add up |
| `starter_pools` | every mod's pools, and any mod's written `starter` deck beside them (one the game would deal: forty cards it knows, a `weight` above 0) | the pools add up, and are dealt from only when their `draws` together make the 40 cards of a deck; at any other total they are dropped and the disc's starter decks are used (a warning, even for one mod's pools, when two mods or more are listed; the mod itself also carries a note, with one mod or many); a written deck in any mod wins over every mod's pools (a warning naming whose) |
| `passwords` | a card's password, or its price (`starchips` and `starchips_percent` are two different prices); `"all"` reaches every card another mod names | the later; added cards can be sold too, at a default price of 999999 until a price rule changes it |
| `packs`, `pack_shop` | the shop's rules; a shop by id (a pack is its mod's own, `<mod id>:<id>`); a password the Password screen takes, a pack's or a card's (`passwords`) | the rules are the later mod's, every rule it leaves out back at its default; a shop's name and unlock are the later's, the packs of both in it; of packs with one password, the first in the packs' list (by `order`, else as declared) is sold, and a card with it goes before them all (of several cards, the lowest number) |
| `guardian_stars` | an ordered pair of stars (from `matchups`, `beats`, `mirror`, and `default_bonus` for the disc's cycles; a star named as the mods read so far, its own `stars` first, have named it, as the game reads them); a star's name, icon or palette; `choice` | the later; a later `"replace": true` sets every pair an earlier mod set to 0, and so does the first declaration of a star 11-15 for that star's pairs |
| `limits`, `chest_overflow` | each key, where `stats` is `attack` and `defense`, a life-point start (a number, or `start`) is both `player` and `opponent`, a duelist's own LP is a side (written as a number, it is the opponent's side, as the game reads it; `"all"` is a duelist of its own, which a named one goes over), and `chest_overflow` is the chest and its starchips, 250 and 0 for what it leaves out (one the game leaves out, a `limit` past 1-255 or `starchips` past 0-999999, is no claim) | the later; different duelists add up |
| `terrain_bonus` | a terrain and a monster type | the later; a later `"replace": true` clears the earlier |
| `trap_thresholds` | one of the six attack traps (or a copy of one) | the later |
| duelists (`duelists/` folder and `"duelists"` list) | a duelist a valid `replace` names; a Free Duel `slot` (40-127, with a `copy`) | the later has the duelist; the earlier keeps the slot, the later takes the next free one |
| `text` | a string id of the listings (an item may name several, `[8001 8002]`) | the later's words |
| `font` | | the fonts add up |
| `title`, `menu` | each key, where the title's and the menus' `spacing` and `entries` are one setting (an entry by its name or its number); a menu button by `"<mod id>:<id>"` | the later; the title's `text` lines add up; a mod loading after another changes its button on purpose, one loading before it is left out (a warning) |
| code hooks | a game function two mods hook (`host->hook`) | the mod applied last is called first; the others run only if it calls its `original` |
| events | an event two mods subscribe to (`host->subscribe`) | all are called, by priority; a before-hook that handles it stops the rest |

Every key is read as the type its reader takes, a list or an object; a
manifest with anything else there (a list where an object belongs) is
read as the game reads it: that key as nothing, the rest as it is.

Information, not a warning:

* **They agree.** Two mods setting the same value (the same fusion result,
  the same threshold, the same words for a string) change nothing between
  them.
* **They add up.** Pool edits, `remove`s, starter decks, starter pools whose
  draws make forty, fonts, title text lines, and two card `replace`s that
  only set different stats.
* **On purpose.** When the winner lists every other mod of the line in its
  `after` or `requires`, it is layered over them by design, and the line says
  so instead of warning. A mod made to go over another should say so with
  `after` (or `requires`, when it needs it).

An entry a mod's setting switches off (`"setting"`, as on a text file, a
pack's image or a fusion, equip or ritual entry) is not counted while it is
off. A code mod's hooks and events are known once its code has run in this
session. A mod's own `settings` never meet another's: each is kept as
`mod.<id>.<key>`. An image whose PNG is missing still counts (the loader
leaves it out), and data a code mod changes by writing memory cannot be
seen. A name or text longer than a line has room for is cut short.

The [FM Editor](../tools/pc/fm_editor/README.md)'s Conflicts tab checks the
mod being edited against the other mods installed beside the game and in the
player's mods folder (or a folder chosen there), with the same lines:
`tools/pc/fm_editor/overlaps.py` is the Python twin of
`src/pc/mods/overlap.c`, and both are tested against the mods in
`tests/pc/mod_overlaps` (and the sets of `starter-pools` there), line for
line and in order (`pc_mods_overlap`, `test_overlaps.py`).

## Code mods

A code mod is **one object file**, `<library>.o`, that runs on both the
Linux and the 32-bit Windows game. macOS ARM64 needs a separate
`<library>.dylib` build; mod authors supporting all three platforms must
ship both files. See [Native macOS ARM64 code mods](#native-macos-arm64-code-mods)
for the build command and manifest selection. Both i386 games are 32-bit
x86 code with the same calling convention, so the machine code is the same;
the game reads the file with its own loader
([`src/pc/mods/object_loader.c`](../src/pc/mods/object_loader.c)) rather than
the system's, so the container is the same too.

The 64-bit Windows game (`-windows-x64.zip`) loads data mods only: a code
mod is 32-bit code, so it stays off there with "needs a 64-bit build of
this mod" in the Mods window, and the 32-bit game is the one to play it
with. A 64-bit Windows code-mod SDK is a later milestone (`notes/pc-build.md`,
"64-bit Windows"). macOS ARM64 code mods use the separate native build
described below.

The mod exports one function, described in
[`src/pc/mods/modapi.h`](../src/pc/mods/modapi.h):

```c
#include "pc/mods/modapi.h"

static const MemoriesModHost *host;

static void draw_frame(void) { /* once a frame, while the mod is applied */ }

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    host = from;
    mod->api = MEMORIES_MOD_API;
    mod->frame = draw_frame;
    return 1;   /* 0 refuses the load */
}
```

The legacy hooks are `frame` (after the game has queued its own drawing, which is
where an extra pass can draw over the finished picture), `applied` (the
player applied or removed the mod), `reset` (a save state was loaded, so
anything cached from the old game is stale) and `shutdown`.

The host table is what a mod is given: `log`/`log_enabled`, `open_asset` (a
file the mod ships), `open_data` (the mod's own file in the user directory,
the only place it may write), `setting`/`set_setting` (whole numbers kept in
the player's settings file as `mod.<id>.<key>`, and read from
`MEMORIES_MOD_<ID>_<KEY>` first when that is set; a key is letters, digits,
`_` and `-`, and `order` is the manager's), `disc_file_start`/
`disc_read`, `pad`, and from mod API 2 `now_us` (a clock) and `map_fixed`
(memory at an address the mod chooses, as 3D Monsters' model arenas need). API 4 adds `hook`/`unhook`/`symbol`, below; API 5 adds `duelist_id`, which resolves an added duelist's identity to the id it has this run as `card_id` does for a card. API 7 adds `card_notes` and `card_tag`, a card's [notes](more-cards.md#notes-on-a-card) and the `<tag: value>` tags in them. API 8 adds `limit`, the numbers the game caps and its other values as the mods' `limits` set them ([Gameplay tables](gameplay-tables.md#values-atk-def-lp-starchips-and-more)): `host->limit(host, "attack")` is 9999 without such a mod, `host->limit(host, "deck_copies")` 3. API 9 adds `menu_item`, the name of an item of the title's menus, and the event `MEMORIES_EVENT_MENU` ([The title's menus](#the-titles-menus)). API 10 adds the event `MEMORIES_EVENT_MONSTER`: a monster summoned, flipped, at its owner's draw, in a battle, destroyed or destroying the monster it battled, for every monster, with `handled` to skip a card's own `monster_effects` ([Monster effects](more-cards.md#monster-effects)). API 11 adds no entry or event; it marks the mod.json features a game of API 10 leaves out ([Which game a mod needs](#which-game-a-mod-needs)).
A mod that uses an entry newer than API 1 should refuse to start when
`host->api` is older.

API 3 adds managed gameplay events, named configuration profiles and registered
save-state buffers. The [API 3 guide](mod-api-3.md) describes damage, reward,
fusion, effect, AI, input and scene hooks, their ordering/cancellation rules,
and stable card identities. The [manager](mods-window.md) exposes descriptions,
settings, compatibility and staged batch changes.

### Replacing or wrapping a game function (API 4)

Every function of the game can be taken over by a mod, not only the ones
with an event. `host->hook` names the game function directly and gives the
replacement, which has the same signature:

```c
extern void DuelScene_UpdateResultRewards(void);   /* the duel's result screen */
static void *original;   /* static: the host keeps it up to date */

static void my_result_screen(void)
{
    /* ... before ... */
    ((void (*)(void))original)();   /* the game's own, or the mod hooked before this one */
    /* ... after: change or observe what it did ... */
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    if (from->api < 4) return 0;
    host = from;
    mod->api = 4;
    return host->hook(host, (void *)DuelScene_UpdateResultRewards, (void *)my_result_screen, &original) != 0;
}
```

While the mod is applied every call, from anywhere in the game, goes to the
replacement; `original` leads to what it displaced, so calling it wraps the
function and not calling it replaces it. Several mods may hook one
function: the one applied last is called first, and each one's `original`
leads to the one before. Removing a mod in the Mods window takes its hooks
out at once, and a function nobody hooks is the game's again, byte for
byte. `hook` returns 0 for anything that is not a game function (the port's
own code, the C library, a function of another mod). `host->unhook` removes
one hook, and `host->symbol("name")` looks a name up at run time, for a mod
that can do without it.

How it works: every game unit is compiled with
`-fpatchable-function-entry=8,6`, which leaves six bytes of `nop` before
each function and two at its entry. A hook turns the six into an indirect
jump through a pointer the host keeps and the two into a short jump back to
it; see [`src/pc/mods/hooks.c`](../src/pc/mods/hooks.c). The game runs the
same with no mod hooking anything (the smoke screenshots are unchanged).

### Sharing with other mods, drawing, saves (API 4)

* **Sharing.** `host->provide(host, "name", pointer)` offers a function or
  data to other mods; another mod gets it with
  `host->find(host, "<providing mod's id>:name")`, NULL when no loaded mod
  offers it. A mod that lists the provider under `requires` is initialized
  after it, so `find` works in its `MemoriesModInit`.
* **Drawing over the picture.** Set `mod->overlay` (and
  `mod->overlay_signature`, a number that changes whenever what you draw
  does; without it the overlay is drawn every frame). Inside it,
  `host->overlay_size` gives the window's size in pixels and the scale the
  port draws its own menus at, `host->draw_text` writes ASCII text (its `y`
  is the line's middle), `host->text_width` measures it and `host->fill`
  blends a rectangle in. It is drawn at the window's resolution over the
  game picture, under the port's save menu, and only while the mod is
  applied.
* **Save slots.** The events `MEMORIES_EVENT_SLOT_SAVE` and
  `MEMORIES_EVENT_SLOT_LOAD` (after only) say that the running game was
  saved to, or loaded from, slot `a` (from 0); `b` is the slot's token and
  `c` the save's sequence number. A token is drawn afresh at every save, so
  a mod that keeps something per save names its file after it
  (`open_data`), and each slot has its own.
* **More of the C library**: `strcpy`, `strcat`, `strncat`, `atoi`, `labs`,
  `strtod`, `bsearch`, `tan`, `asin`, `acos`, `atan`, `exp`, `log`, `log10`,
  `tanf`, `expf`, `logf`, `<ctype.h>` (ASCII, in the header) and `rand`/
  `srand`, which give the same numbers on Linux and Windows and leave the
  game's own random numbers (and so its duels) alone.

### Building one

```sh
python3 tools/pc/build_mod.py my-mod            # writes my-mod/<library>.o
```

`build_mod.py` compiles every `.c` in the directory and merges them into the
one object. Beside a released game the same script is
`sdk/tools/build_mod.py`, and it builds against `sdk/include` there. The
release's `sdk/` also carries `extract_images.py` and `upscale_pack.py` in
`sdk/tools`, the example mods in `sdk/examples/mods`, and this note with
`mod-api-3.md` and `more-cards.md` in `sdk/notes`. It needs
clang (on Windows, the llvm-mingw clang; it builds the Linux object format
there too) or, on Linux, gcc with 32-bit support. `./build-pc.sh` builds
every directory under `mods/` this way, once, and copies the same file into
both games' `mods/` directories. The script keeps what it builds in
`tmp/pc/mod-build` (beside `sdk/`, or in the repository), under a key of the
compiler, the flags and the sources with every header they include, and
builds again only when one of those changed; the folder can be deleted at
any time.

A mod reaches the game directly. Its undefined names are bound when it is
loaded, against a table compiled into the game (`mod_exports.c`, generated
by `tools/pc/build_game32.py`). The table holds every game function and
variable, every guest variable pinned to its retail address, and the
port's own globals. That is what makes something like 3D Monsters possible:
it borrows the model loader, the software GPU's texture banks and the duel's
ordering table. `build_mod.py` checks the names against the game builds it
can see and says which are missing, rather than leaving it to the game.

### What a mod is built without, and why

A mod is built with **no system headers at all**. glibc and the Windows C
runtime disagree about `FILE`, `errno`, `stdin` and more, so a mod built
against either would only work on one system. Instead:

* the compiler supplies `stddef.h`, `stdint.h`, `stdarg.h`, `stdbool.h`,
  `limits.h` and `float.h`;
* the SDK's own `stdio.h`, `stdlib.h`, `string.h` and `math.h`
  ([`src/pc/mods/sdk`](../src/pc/mods/sdk)) declare exactly the C library
  the game lends a mod (`src/pc/mods/mod_libc.c`): memory and string
  functions, `snprintf`/`vsnprintf`, `malloc` and friends, `strtol`,
  `qsort`, the usual maths, and `fread`/`fwrite`/`fseek`/`ftell`/`fgets`/
  `fclose` for the files the host opens. There is no `fopen`, `getenv`,
  `printf`, `time` or `exit`: files come from `open_asset`/`open_data`,
  knobs from `setting`, the time from `now_us`, and output goes to `log`.

The compiler flags close the gaps between the two ABIs, and each one is
covered by a test (`tools/pc/test_object_loader.py`):

| Flag | Why |
|---|---|
| `-fno-pic -fno-common` | plain relocations only; the loader has no GOT and no COMMON symbols |
| `-fno-stack-protector` | the canary is read from Linux thread storage (`%gs`), which Windows does not have |
| `-march=i686 -mno-sse` | x87 floating point, like the game's own code |
| `-mstackrealign` | Windows only promises a 4-byte-aligned stack on the way in, and the Linux game (SSE2) needs 16 on the way out |
| `-fstack-clash-protection` | a frame over 4 KiB touches each page, as Windows' stack guard page requires |
| `-ffreestanding -nostdinc` | no system C library, as above |
| `-mretpoline-external-thunk` (clang), `-mindirect-branch=thunk-extern -mindirect-branch-register` (GCC) | every indirect call goes through the game's `__x86_indirect_thunk_*`, which the C library list lends, so a call through a function pointer read from a game table (a MIPS address) reaches the native function without DEP, as in the game's own code. See below the table |

The thunk flags came with the change that lets the game run without DEP
(Windows' Data Execution Prevention). Two consequences for mods:

- A mod built with an earlier SDK has no thunks: it still loads and runs,
  but a call it makes through a function pointer that holds a MIPS address
  (one read from a game table) reaches the native function only where DEP is
  on, through the game's fault handler. With DEP off, that call runs the
  MIPS bytes and crashes. Rebuild the mod with the current SDK to lift this.
  Calls to the mod API and to game functions by name are not affected.
- A mod built with the current SDK needs `__x86_indirect_thunk_*` from the
  game, so it loads only in a game from that change on; an earlier game
  refuses it in the Mods window.

The loader refuses anything it does not handle, with the reason in the Mods
window: position-independent code, relocations other than plain absolute
and relative ones, COMMON symbols, thread-local storage, constructors and
destructors (do that work in `MemoriesModInit`), and a name the game does
not provide. A crash inside a mod names the function it was in
(`3d-monsters:draw_frame+0x40`).

### Later releases

A code mod built against one release keeps working in the later ones,
without being rebuilt. Every name that release's `sdk/exports.txt` lists
stays exported, with the type its SDK declared. Every structure those
names reach keeps its layout, and every enumerator (`SET_PGXP`,
`MENU_ITEM_OPPONENT_NAME`) keeps its value. New settings, menu items and
fields go at the end, and retired ones keep their place. Each pull request
is checked against the releases in `tools/pc/mod_compat.txt`, and those
releases' own mods are run in each new release before it goes out
([pc-release.md](pc-release.md)). A mod that uses something newer sets
`min_api`, and checks `host->api` as `modapi.h` describes.

## What a mod may and may not do

The host table has no network call in it, and no way to name a file outside
the mod's own directory and its data directory: relative paths only, and
`..`, absolute paths and drive letters are refused (`Paths_Contained`). Data
overrides only reach the disc image through the port's own reader and never
write to it.

A code mod, though, is native code in the game's process: nothing stops one
from reaching past the host table, since the whole game image is in reach
by design. The confinement above is what the mod system offers, not a
sandbox around the process, so installing a code mod is trusting its
author, as with any plugin. A data-only mod carries no code and is safe to
install on that ground alone. The Mods window shows every mod it found, and
the reason beside any that failed to load.

## The mods the release ships

| Mod | What it is |
|---|---|
| `mods/3d-monsters` | face-up monsters on the duel field stand on their cards, either as animated models (`notes/pc-build.md`) or, its `style` setting turned to "Card art", as an enlarged, glowing cutout of the card's own art instead |
| `mods/hand-camera` | L1/R1 turn and L3/R3 zoom the duel camera while the hand is up |
| `mods/ai-hard-mode` | optional stronger opponent decisions |
| `mods/yamyi-mods` | return-to-title confirmation and Library drop odds, with independent switches |
| `mods/drop-missing-cards` | off by default: gives the 82 cards no duelist drops a duelist to win them from, as the old static recomp's option did; a data-only `drops` table |

The first two were part of the executable until they became mods; they are the worked
examples of a code mod that reaches deep into the game. 3D Monsters' knobs
are its declared settings `style`, `scale`, `pixels`, `lift`, `pitch`, `depth`,
`battle`, `battle_pixels`, `battle_dim`, `attack`, `attack_speed`, `glow`, `glow_r`, `glow_g`, `glow_b`,
`glow_reach` and `glow_period`, in
the Mods window (`MEMORIES_MOD_3D_MONSTERS_SCALE=5000` for one run; they were
`MEMORIES_MODS_SCALE` and so on before it became one object for both systems).
`style` picks the presentation (0 the original 3D models, 1 Card art).
`pixels`, `lift`, `pitch` and `depth` are shared on purpose -- both styles
fit and place their own cutout or model by the same target height, lift,
field-pitch threshold and depth offset, so one setting means the same thing
either way, and `field_art.c` simply reads the settings `field_models.c`
already declares rather than repeating them. Everything else belongs to one
style alone: `scale`, `battle`, `battle_pixels`, `battle_dim`, `attack` and `attack_speed` are 3D
models only (down to their own labels saying so in the manifest); `glow` and
the rest are Card art only, the same way. One more, `test`, is read but not
declared, so the window does not show it: `MEMORIES_MOD_3D_MONSTERS_TEST=<card>` stands a
different monster in every zone from that card on, for measuring the cache
and the arenas. `attack_test` (1-4, forcing the attack's destroyed, guarded,
counter or tie case), `attack_drift` (the percent of a monster's shift the
attack keeps), and, for the attack on the field, `fight_zoom` (the percent of
the camera's distance it comes in to), `fight_turn` (the percent of the way to
a side-on view it swings) and `fight_reach` (the percent of the arena's gap
the attacker strikes from) are read the same way. `attack` is a choice (off,
the attack cards, the field, both); the 1 it stored as a bool is the cards.

**A mod with a "style"-like choice setting** (more than one whole presentation,
picked by one setting, the way 3D Monsters' two styles are): a setting or a
whole piece of behaviour that belongs to one style alone must have no effect
in the other, not just be unlikely to matter there -- the settings window has
no way to hide a setting only some styles use, so this cannot be enforced by
the window; it has to be true of the code itself, checked at the one place
that reads the style, not left to whichever function happens to read the
setting. 3D Monsters got this wrong once during review: `battle` (the attack-
card presentation) kept running under Card art, since it has no card-art
equivalent and so seemed harmless to leave alone, but the two models it stood
still showed while everything else on the field had switched to cutouts.
Fixed by moving the style check ahead of it in `draw_frame`, so a style-
exclusive function is never even called under the other style, the same way
Card art's own drawing is never reached under 3D models. Where a setting is
genuinely the same thing under either style (3D Monsters' `pixels`, `lift`,
`pitch` and `depth`), share it rather than adding a second copy -- but where
it belongs to one style, its label says so (`"(3D models)"`, `"(Card art)"`)
even though the window shows it regardless, since that is the only signal a
player has that it does nothing under the other choice.

Building `style` surfaced a Mods window bug, since fixed for every mod's
`choice` settings, not just this one: `adjust()` (`mods_window.c`) clamped a
`choice` at its first and last option the way a plain `int` clamps at its
`min`/`max`, so pressing the same arrow again at either end did nothing --
confusing for a two-option choice especially, since one arrow would appear to
stop working entirely and the player had to know to press the other one. A
`choice` is a closed, named set the way a `key` setting's pad buttons are, not
a range with a meaningful limit, so it now wraps around instead, the same way
`key` already did.

## Testing a mod

* `MEMORIES_TRACE=mods` logs discovery, loads, overrides and whatever the mod
  logs itself.
* `MEMORIES_HEADLESS=1 MEMORIES_DUMP_FRAME=N MEMORIES_DUMP_PATH=out.ppm`
  renders one frame without a window.
* `tests/pc/mods_test.c` (ctest `pc_mods`) covers discovery, manifests, the
  settings keys, the names a mod binds to and what data overrides do to a
  sector;
* `tools/pc/test_object_loader.py` (ctest `pc_object_loader` for Linux,
  `smoke.py --windows` for Windows) loads and runs one object on both
  systems and feeds the loader broken and damaged ones;
* `tools/pc/check_mod_exports.py` (run by `smoke.py`) holds each game's
  table of names against its link;
  `tests/pc/json_test.c` (`pc_json`) covers the manifest reader;
* `tests/pc/audio_replace_test.c` (`pc_audio_replace`) covers WAV and Ogg
  decoding, resampling, the `audio` object, which mod wins an id and what
  the mixer plays for the sound driver's calls.

### Yamyi Mods

Apply **Yamyi Mods** in **Game > Mods**. Its settings separately enable
return-to-title confirmation and Library drop odds.
The panel can hide its rarity-score column, choose a sort order, list up to
20 duelists and change position. It only describes cards visible in the Library.
Rows are reduced to fit the window; enlarge a very small window to see the panel.
Scores and odds respect other mods' drop-table edits and added cards.
When sorting by score, each duelist's best scoring rank is shown; other sorts
use its highest drop weight. A weight of `w/2048` is the chance per win at that rank.

The first Library display creates `mod-data/yamyi-mods/card_name_color.ini`
in the player's directory. Its rarity tiers and duelist/rank multipliers give
the panel's score; restart after editing it. Lower scores mean rarer cards; an
explicit zero multiplier is respected. By default, card-name colors come from
`card_text_colors` declarations (above), in this or any other mod. Turn on
**Rarity card-name colors** to also read the INI's color slots, tiers and
card overrides; while enabled, its assigned rarity color temporarily
overrides the manifest color for the card name only. Description and guardian
star colors still come from `card_text_colors`. The package is disabled by
default and does not alter actual drops or duel rules.

These features originate in yamyi's PRs #68, #70 and #77. Their overlapping
Library panels are combined into one panel here; do not also install the old
`menu-back-confirm`, `card-name-color` or `drop-odds` packages.

## Native macOS ARM64 code mods

The macOS port loads an ARM64 `.dylib` compiled with guest-memory
translation; Linux and 32-bit Windows retain the shared i386 ELF `.o`. In a source checkout, build the macOS game first,
then run:

```sh
python3 tools/pc/build_mod.py /path/to/mod --target macos
```

This preserves the existing `.o` and produces `<library>.dylib` alongside it.
A manifest with `"library": "life-points"` selects `life-points.dylib` on
macOS and `life-points.o` on Linux/Windows. Copy the whole mod folder into
`~/Library/Application Support/YFM Re-Decomp/mods/` on macOS and apply it in
the Mods menu.

The compiler uses the same structured LLVM guest-memory translation as the
game. Mods should include the game's annotated headers (`G32`, `PSXLONG`)
for guest records; a plain LP64 recompile is insufficient. Imported function
ABIs are checked against the current build's `mod_signatures.json`; missing
imports are rejected. Constructors/destructors, TLS and machine assembly
are unsupported. Initialize through `MemoriesModInit` and release through
`shutdown` instead.

ARM64 hooks use typed wrappers around translated game routines, including
native game overrides. They preserve the original call signature and chain
in mod order without patching executable memory. Variadic functions,
`returns_twice` routines and ordinary host helpers are not hook targets.
Existing x86 objects do not run on ARM64; other source mods still need
compilation and individual validation. The optional `test_arm64_life_points.py` runner checks separate
player/opponent LP, campaign/free-duel scope, both sound timings, sound
disabled and fresh-process save-state replay against a supplied source mod.

## Palette ramps

`palette_ramps` turns one RGB value into the game's 16-entry text brightness
ramp: the transparent entry stays transparent and the dark glyph outline
stays opaque. Put it at the top level of `mod.json`; it needs no library.
Each value is `"#RRGGBB"` or `[red, green, blue]`, 0 to 255 each.

```json
"palette_ramps": {
  "0": "#FFFFFF",
  "rare": "#54C8FF",
  "legendary": [255, 184, 56]
}
```

A key from `0` to `7` replaces that one of the game's eight text ramps (the
ones `card_text_colors` numbers: white, yellow, blue, green, grey, orange,
red, and the unused eighth). That changes every text drawn in it, menus and
dialogue as well as cards: `"0"` recolors all white text. When two enabled
mods set the same ramp, the later mod in load order wins. Yamyi Mods' rarity
colors write the ramps their INI defines too, after `palette_ramps`, and put
the disc's back when turned off.

Any other key is a named ramp, up to 256 in all, named with the declaring
mod's ID in front (`"your-mod:rare"`, 63 letters at most). The game keeps it
but puts it nowhere: no part of VRAM stays free on every screen, so code that
draws a 4-bit texture with it uploads it to a place it knows is unused on its
own screen, and uses the CLUT word that returns:

```c
#include "pc/mods/palette_ramps.h"
prim->clut = PaletteRamps_Load("your-mod:rare", 992, 400);   /* 0: not declared */
```

`x` is a multiple of 16 below 1024 and `y` below 512. Load it again whenever
the screen may have overwritten that place. `PaletteRamps_Ramp(name, ramp)`
gives the 16 BGR555 entries instead, for code that puts them somewhere itself.

The ramps are made as the game starts, from the disc's white ramp, so turning
a mod with `palette_ramps` on or off, or changing them, takes a restart.
