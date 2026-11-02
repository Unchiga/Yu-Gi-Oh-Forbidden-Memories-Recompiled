# Mod manager

**Game > Mods** opens a resizable library. Search names, IDs or authors; cycle
All / Enabled / Disabled / Issues to filter the list. The list and the detail
pane scroll independently. The window fits the desktop, and adapts its local
text scale when resized. SDL and X11 use the same drawing and input code.

Select a mod to see its description, author, version, source directory, current
runtime status and native-code/content classification. The Settings tab renders
the mod's declared integer sliders, toggles, choices and pad-button bindings,
one block each: the label beside `-` value `+`, the description wrapped under
them, then an integer's slider. The value box is sized for the widest value the
setting can take, so choices are never cut off; clicking it steps forward.
Restore defaults (above the list, which scrolls under it) resets the selected
mod's staged settings. Compatibility lists requirements, ordering constraints
and declared conflicts, then everything the mod changes that another enabled
mod changes too, by kind with counts: the first four lines of each, **and N
more** to show the rest (up to 200), a warning where only one mod's change is
used and a dim line where they add up, agree or follow an `after`; a line
about the mod alone (its starter pools that the game leaves out with these
mods enabled) is counted apart, as left out rather than changed by another
mod; the header counts the overlaps between all enabled mods ([When mods
overlap](modding.md#when-mods-overlap)). They follow the enabled set, the
order and the settings staged in the window; a setting that needs a restart
counts there as staged (or as saved), so after **Apply changes** the list
shows the next launch, not the session still running with the old value. Lower Order values load first, subject to
dependencies.

Changes are staged until **Apply changes**. Restart-only changes share one
confirmation; restarting discards unsaved game progress. Close/Escape asks before
discarding staged edits; the title-bar close button asks once, and a second
click discards. Only settings edited in the window are written; an untouched
mod's order keeps following its manifest `priority`. A failed restart keeps the saved preferences for the
next launch. Active, inactive, pending, warning and error states are distinct.
Changing a load-order value requires a restart. New directories are discovered
on the next launch; native objects stay resident until exit.
A live mod that requires a restart-only mod which is not in place yet waits for
the same restart, and Apply offers it. Game > Reload settings follows the same
rule: it records restart-only mods without putting them in place or taking them
out. A mod that failed to go in place (a missing replacement file, say) is
tried again after it is removed and applied again.

A warning is text beside a mod that is still applied: manifest keys the game
does not know (with the key it most likely meant), `enabled`/`restart` that are
not booleans, another folder with the same id, a replacement larger than the
file it replaces, and texture pack entries that were left out, counted, with
the first file named.
**Open mods folder** shows the folder new mods go in (`MEMORIES_MODS_DIR`, else
the user `mods` folder, created if missing) in the system file manager.

The profile field accepts a name (letters, digits, spaces, `_`, `-`). Save writes
the currently applied preferences to `mod-profiles/<name>.txt` in the user
directory. Load stages that profile for inspection; it does not change the
running game. Profiles include declared option values and load-order settings.
Apply edits before saving a profile. Profiles do not copy the mods themselves.

Keyboard: Up/Down select a mod; Left/Right disable/enable; Enter toggles; Tab
focuses search, then the profile name, then the list. Escape leaves text entry,
cancels a confirmation, or closes the window. The mouse wheel scrolls whichever
pane is under the pointer. Both panes show a scrollbar when their contents do
not fit: drag its thumb, or press the track to jump there. The details scroll by
pixels, so every tab (About, Settings, Compatibility) reaches its last line;
integer sliders support dragging.

**On a phone or tablet** (Android) the window is a panel inside the game's
window (`panel.h`; [Android](pc-build.md#mods-and-controls-as-panels-inside-the-window)):
the same code at the screen's density, every row and button at least 48 dp
tall, no title or keyboard hints (the counts take the message line while
there is no message), and, where the list and the details do not fit side
by side, two pages: the list, and a mod's page (Back, load order, Enabled,
the tabs) that a tap on its row opens; the [x] at a row's left switches the
mod without opening it. A drag scrolls the list or the details; a slider
follows the finger. Search and the profile name show the on-screen keyboard
(its Enter ends the typing); Back is Escape. There is no Open mods folder
(an app has no folder window to open; `Platform_OpenFolder` fails on
Android): the player's mods go in `mods/` in the app's files folder
(`Android/data/org.yfmredecomp.game/files`), and **Import mod...** (in the
footer left of Close, on the list's page) puts one there from its `.zip`,
below. Apply & restart starts the app again in a new process. The game is
paused while the panel shows.

**Import mod...** (Android; `ModsWindow_SetImport`, which no desktop
calls, so the desktop window and the desktop's test panels have no such
button) opens the system's file picker; the chosen document is copied into
the mods folder (`mods/.incoming.zip`; a document over the importer's 4 GB,
or one whose size the provider gives and the free space cannot hold, is
refused before the copy), read
(`src/pc/mods/import.c`), and its mods are put in place and added to the
list, off, without a restart (`Mods_Discover`): the first one is selected,
and the message line says "Imported <name>." (or "Imported 3 mods: A, B and
C."). Turning one on is then like any other mod's change (Apply, a restart
when the mod needs one). The copy, the reading and the unpacking run on the
game's thread, so the panel waits through them without a progress bar: a
few seconds for a usual mod, minutes for a pack of gigabytes.

- **What counts as a mod in it**: every folder with a `mod.json` at the
  `.zip`'s root (the whole `.zip` is then the mod, in a folder named by the
  manifest's `id`), in its top folders, or one wrapper folder down (a
  `Downloads/` or `mods/` the mod was zipped in; files beside the mods there
  are left out). A `mod.json` deeper inside a mod is that mod's file;
  `__MACOSX/` is skipped. A `.zip` with none says "This .zip has no mod in
  it."
- **Already installed** (the folder of that name holds a mod with the same
  `id`, or an installed mod has that `id`): "<name> is already installed.
  Replace it with the one in this .zip?" with Replace and Cancel (Back
  cancels too). Replace moves the old folder aside, puts the new one in and
  then deletes the old one. A folder of that name that holds another mod, or
  no mod, is never replaced: the new mod goes in `<folder>-2` (`-3`, ...),
  a name no other mod of the same `.zip` has either (letter case aside). A
  mod that is (or was) in place this launch is not replaced at all, since
  its data, pictures and code are read from its folder while it runs: "<name>
  is in use, so its files cannot be replaced now. Turn it off, Apply &
  restart, then import it again." A new mod with the `id` of one the release
  ships goes in beside it and replaces it from the next launch, as on a
  desktop ("It is used after a restart.").
- **Code** (the manifest names a `library`, or a `libraries` object with
  any entry, as the loader reads it: `Manifest_HasCode`, which `mods.c`
  `read_manifest` and the import share): imported, off. A game with the
  loader (every build target since Mod SDK M2, the arm64 game among them)
  says nothing more: whether the mod has an object for it is found at
  Apply, and said beside the mod (`mods.c`). A game built without the
  loader (`MEMORIES_NO_CODE_MODS`, which no build target sets now; the
  `pc_mods_window_no_code` test builds one) says, whatever objects the mod
  ships, "This mod has code, which the game cannot run in this build yet.
  It stays off and changes nothing in the game." ("on Android" there).
- **Refused, with nothing written**: a name that would land outside the
  folder (`..`, `.` or empty parts, an absolute path, a drive letter or any
  `:`; backslashes count as slashes), more than 32 folders deep, a link or
  other special file, a password, a compression other than stored and
  deflate, two entries for one file (ASCII letter case aside; two names
  the storage itself takes for one, such as `É.png` and `é.png` on
  Android's shared storage, are found as the second is written: "Two files
  in the .zip are one file on this storage..."), two mods with
  one folder or one `id`, names that are not UTF-8, ZIP64 or split
  archives (the plain form holds 65,534 entries and 4 GB, which is what
  the importer takes), more than 4 GB unpacked or more than the free space
  holds, a `.zip` over 4 GB (2 GB where a `long` has 32 bits, where `ftell`
  stops: Windows and the 32-bit builds; Android's arm64 game takes 4 GB),
  a file that is not a `.zip`, or more mods than the game takes (256, in
  one `.zip` or with those installed). A mod at the `.zip`'s root without an `id` goes in `imported-mod`.
  Unpacking goes into a hidden `mods/.import-XXXXXX` folder (the scan skips
  names starting with a dot), checking each file's size and CRC; only then
  is each mod renamed into place, and any failure puts back what was moved
  and removes the hidden folder (an old folder that cannot be put back is
  kept in `mods/.recovered-XXXXXX` and named in the message; a new mod
  already placed that can be neither moved back nor removed is named too,
  "...could not be taken out of the mods folder again (delete that
  folder)"). What an
  import cut short leaves (a crash) is removed when the panel opens again.
- **Testing**: `MEMORIES_IMPORT_ZIP=<file>` (in `environment.txt`) makes
  Import mod... take that file instead of opening the picker.
  `pc_mod_import` checks the reader and the installer on `.zip` files it
  writes; `pc_mods_window` drives the panel's import with a fake picker.

**Remove** (Android; it comes with `ModsWindow_SetImport`, so no desktop
panel has it either): a mod that is off, or does not load (an old code mod
this build refuses shows "Inactive" or "Error" and nothing else could take
it away), is deleted from the app's mods folder, which no file manager can
open on Android 11 and later. The button is in the footer beside Import
mod... (left of it); on a phone, where Import is on the list's page, Remove
is on the page of the selected mod. The first tap asks, in the message line
and with the buttons as for Replace ("Remove <name>? Its folder is deleted
from the mods folder, with everything in it. This cannot be undone."; Apply
reads Remove, Close reads Cancel; Enter says yes and Escape no); the second
deletes.
- **What it deletes**: `Mods_ImportRemoveInstalled` (`import.c`) removes the
  mod's folder only when it is `<mods folder>/<one plain name>`: not the
  mods folder itself, not a path with `..`, a separator, a `:` or a leading
  `.` (the import's own hidden folders), and not a link (`lstat`: a link is
  refused, never followed; Windows has no `lstat` here and `stat` follows a
  junction, but nothing runs this there except the tests, since the button
  exists only on Android). The folder is renamed in one step to
  `mods/.import-removing-<name>` (hidden: the mod scan skips it) and then
  deleted, so a delete that fails or is cut short leaves no half mod in the
  list but a hidden folder that `Mods_ImportCleanup` finishes off at the
  next start (or the next import). The mods that came with the game (origin "shipped") are
  refused: "<name> came with the game, so it cannot be
  removed."
- **A mod in use stays**: if anything was loaded from the mod since the game
  started (`Mods_InUse`: it is on, or was turned off since) its code, data
  and pictures are files the game holds, so Remove says so and deletes
  nothing: "<name> is on, so the game is using its files. Turn it off,
  apply, restart the game, then remove it." (or, for one that is off but was
  tried since the game started, which includes one that is turned on and
  failed to load: "...was tried since the game started, so the game may
  still hold its files. Turn it off, apply, restart the game, then remove
  it."). After that restart it was never tried, and goes. The same rule keeps
  Replace from changing it.
- **The list**: the mod leaves it at once, with no restart, and the next one
  is shown. `Mods_Forget` keeps its slot in the loader's table (events and
  saved state are keyed by the index, which a loaded mod behind it holds),
  marks it removed (`Mods_Removed`: not in the list and not in the counts;
  what was staged for it, its On, load order and settings, is dropped, so it
  is no "Unsaved changes"; a profile that names it is loaded without it, and
  a profile saved after writes it as off, `mod.<id>=0`) and broken (nothing
  can apply it). The slot stays until the next launch (it counts toward the
  game's 256 mods until then). Importing a mod with that `id` later takes the slot again.
- **The saved settings**: `mod.<id>` (on or off) is set off, so a mod
  imported later with that id starts off like any new one, and the load order
  and the mod's own settings are kept: an updated version imported after
  Remove comes back with the player's choices. (There is no call to delete a
  named setting; the leftover keys do nothing.)
- **A folder that cannot be renamed** keeps the mod in the list ("Could not
  delete <folder>: try Remove again. It stays in the list.").
- **While the question is up** the left Remove button steps aside: the
  answer is Apply, which reads Remove, and Cancel.
- **A copy that replaced a mod of the game's** (same id as a shipped one): once removed, the game's own mod is back at the next launch, off.

- **Testing**: `pc_mod_import` checks `Mods_ImportRemoveInstalled` (names
  that leave the folder, `..`, hidden folders, links, files, a trailing
  slash, the hidden rename and the sweep of what it leaves); `pc_mods_window`
  removes imported mods through the panel (the question, Cancel, the list
  and the count, what was staged for the mod and a profile that names it,
  an updated version imported again, a mod that is on, the phone's two
  pages, no button without the picker). `MEMORIES_TRACE=window` lists the button as `remove`.

**HD pack...** (Android; `HdPack_SetNet` in `src/pc/mods/hd_pack.h`, which
only `sdl.c` under `SDL_PLATFORM_ANDROID` calls, so no desktop has the
button) is in the top bar, left of Save (the footer is Import's and the
message's). It downloads the HD pack of the latest release
(`yfm-redecomp-hd-mod-<tag>.zip` on GitHub) and installs it as Import mod...
installs a `.zip`. Nothing is contacted until it is tapped.

- **Asking first**: a tap asks GitHub's API for `/releases/latest` (drafts
  and pre-releases are never "latest", so a preview's pack is not offered)
  and takes its asset `yfm-redecomp-hd-mod-<tag>.zip` (else the first
  `yfm-redecomp-hd-mod-*.zip`), its size and its `digest` (an asset without
  one is refused: the download could not be checked; one over the
  importer's 4 GB unpacked is refused before anything is downloaded, since
  pictures barely compress: "The HD pack of <tag> is too large: more than
  4096 MB once unpacked."). Then:
  - the same release installed (its tag in the mod folder's `.hd-release`,
    since the pack's `mod.json` says `"version": "1.0"` in every release):
    "The HD pack is already installed (v0.2.0)." and nothing is downloaded;
  - a newer one installed: "The installed HD pack (...) is newer than the
    latest release (...)";
  - the pack in use (on this launch): "...cannot be replaced now. Turn it
    off, Apply & restart, then tap HD pack... again.";
  - less free space than about 2.25 times the `.zip` plus 32 MiB (the
    `.zip` and its files are on the disk at once, and the files may be a
    quarter bigger than the `.zip`): "Not enough free space for the HD pack:
    it needs about N MB, and M MB are free.";
  - else the question, with Download and Cancel: "Download the HD pack
    (v0.2.0)? 111 MB; Wi-Fi recommended. Needs ~284 MB free." (decimal
    megabytes, as the phone's storage settings count them; the importer's
    own room check counts MiB; "Update the HD pack from v0.1.2 to
    v0.2.0? ..." over an older one, "... over the installed one?" over a
    copy of unknown release).
- **Downloading**: by the system's download manager (Android's
  `DownloadManager`, with its notification while it runs), into
  `downloads/` in the app's files folder (beside `mods/`, on the same file
  system) as `<asset>.part`. It goes on while the app is in the background
  (Android 15 closes an app's own connections a few seconds after it leaves
  the screen) and waits when there is no network. The message shows the
  share ("Downloading the HD pack (v0.2.0): 45% of 111 MB.", or "Waiting for
  the network to go on ..." / "Waiting for Wi-Fi ..."), and the button reads
  Stop. Once done, the file is renamed to the `.zip` (before the download
  manager forgets the download, which would remove it) and then checked:
  its size and SHA-256 must match the API's. Then the `.zip` is read by the
  importer (it must hold one mod, `forbidden-memories-hd`), placed as
  Import places a mod (a copy there of the same id is replaced) and
  unpacked ("Unpacking ...: N%"), then the tag is written to
  `.hd-release`. "HD pack installed (v0.2.0). Tick it and apply to use it."
  The pack is listed **off**, selected, as any import
  (`mod.forbidden-memories-hd=0` saved): on a desktop the pack's own
  `"enabled": true` turns it on because unpacking it into the game's folder
  is the only step and the next launch is its apply; here the panel
  installs it while the game runs, and the switch-on (and the restart it
  asks for) stays the player's, one tick away.
- **Errors**, in plain words, with nothing left behind (the `.part`, the
  `.zip` and the importer's hidden folder are removed, the system's download
  forgotten): "No internet connection. Connect to Wi-Fi or mobile data and
  try again.", "GitHub did not answer in time.", "GitHub is limiting
  requests from this network right now. Try again later." (HTTP 403 or 429),
  "Not enough free space for the HD pack." (the download manager's 1006),
  "The download from GitHub failed. Try again.", "The download was
  cancelled outside the game." (from the system's notification or
  Downloads), "The download was damaged. Try again." (size or SHA-256 not as
  the API says), "That download is not the HD pack", "This phone's download
  manager is turned off ...", the importer's own refusals. Stop says "The HD
  pack's download was stopped. Nothing was installed."; a Stop that comes
  after the last piece (while the file is checked, in the frames between the
  download's end and the unpacking's start, or with the importer's last
  file) still installs nothing: `HdPack_Install` refuses once a Stop came.
- **Threads**: each step (the question to GitHub, the download, the
  unpacking, the clean-up) runs on a thread of its own (`hd_pack.c`); the
  network is `HdDownload.java` (`HttpURLConnection` for the API,
  `DownloadManager` for the pack) called through JNI on that thread, never
  on the game's, which makes no JNI call for it and reads the step's atomic
  counters each tick. Import mod..., Apply and HD pack... wait for each
  other, and the window's own question (Discard your changes?) keeps the
  message line until it is answered. Closing the panel lets a download go
  on; the step after it runs when the panel opens again. What a job the app
  ended in left (the system's download, which a killed app's goes on
  without it, and the files in `downloads/`) is removed when the panel
  opens, on a thread; the importer's hidden folder is left alone while a
  job is under way.
- **Testing**: `MEMORIES_HD_TEST_SHA256=<hex>` (in `environment.txt`, honoured
  only by a development build, one without a release version) makes a
  download need that SHA-256 instead of the release's, to see the damaged
  path. `pc_mods_window` drives HD pack... with a fake network (no internet,
  403, no pack, the question and Cancel, a damaged download, the download
  manager's failures (1004, 1006, 403, cancelled outside), waiting for the
  network, Stop halfway and during the check, the window's own question
  during a job, a `.zip` that is not the pack, the whole way with the file
  saved under a name of the system's, already installed, newer installed,
  Update, in use, the clean-up at panel open); `pc_mod_import` checks the
  importer's progress and cancel (`Mods_ImportWatch`).

Implementation: `src/pc/platform/mods_window.c`, `src/pc/mods/manager.c`
(`src/pc/mods/overlap.c` for the overlaps, `src/pc/mods/import.c` for Import
mod..., `src/pc/mods/hd_pack.c` for HD pack...), and
window ownership in `sdl.c` / `x11.c`. `pc_mods_window` exercises real settings
and manifests with a fake renderer/restart. `tools/pc/test_mods_context.sh`
checks actual SDL/OpenGL context ownership during secondary-window operations.
`tools/pc/preview_mods.sh` renders the window with the real font and the
repository's `mods/` to `tmp/pc/mods-*.ppm` for review (`PREVIEW_SCALE=2` for
the doubled UI); `tools/pc/preview_panels.sh` renders the window at both
scales and, with `PREVIEW_TOUCH=1`, the panel on six phones and tablets.
