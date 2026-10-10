# Native PC foundation

The port is not playable. Two native builds exist:

- The CMake target: portable adapters and their tests (guest-layout RAM,
  retail GPU packet collection, LIBGS ordering tables, software GTE, game RNG),
  plus the optional PSY-Z renderer probe. Builds as 64-bit or 32-bit.
- `tools/pc/build_game32.py` (shortcut: `./build-pc.sh [run|trace]`): **all 514
  resident game C units plus the main-menu and password/name-entry modules,
  linked as a 32-bit Linux executable that boots to the title screen and main
  menu in a window, and runs NEW GAME through name entry into the opening
  story scene.**
  Missing SDK/assembly routines are generated stubs that name themselves.

All 61 handwritten assembly routines (the model polygon drivers) have native
forms in `src/pc/overrides/model_polygon_drivers.c`; see the table below. Shared game sources
carry four small `#ifdef MEMORIES_PC` guards (listed below); `make match` and
`make match-overlays` still reproduce the retail hashes with them.

## Play from a checkout

On Linux and on Windows alike: put the `.bin` of your USA disc in `game/`
(any name), then run `./play.sh` or `play.bat`. The first run builds the game
and fetches what that needs into `tmp/` (about a minute on Linux, longer on
Windows); later runs rebuild what changed and start at once. `trace` and
`load [slot]` work with both.

What each needs installed: on Linux, `gcc` (with binutils) and `python3`, which
`play.sh` checks for and names the package to install; not their 32-bit
(multilib) parts, CMake or Ninja. On Windows, nothing: `play.bat` uses an
installed Python or fetches the official embeddable one into
`tmp\pc\tools\python`, and `tools/pc/fetch_tools.py` fetches llvm-mingw, CMake
and Ninja when they are not on PATH. Every download is pinned to a release and
its SHA-256.

The matching build (`make match`, which needs the MIPS toolchain) is not
needed: the retail addresses the link pins game variables to are in
`config/pc/guest_addresses.txt`, which `build_game32.py` rewrites from the
matching build's ELFs whenever they are present (commit it when it changes;
a build from the file and one from the ELFs are byte-identical). The game's
executable, SLUS_014.11, is read out of the disc image
(`src/pc/platform/game_files.c`); a path on the command line still names a
separate copy.

### Folder names

Native paths, environment values and command-line arguments use UTF-8.
On Windows, `src/pc/compat/fs.h` converts file operations to the wide Windows
APIs, independent of the user's system code page. Include it (or `posix.h`)
in native units that use filenames or environment variables. Library calls
need the same care: PNGs are read from an already-open file, and FreeType
faces from memory through `compat/font.h`. There is no wide `freopen` or
`_mkdir`: close and `fopen`, and `mkdir(path, 0777)`. A unit that misses the
header still builds and runs, and only fails under such a folder (mod text
and fonts once did), so the `pc_utf8_paths` test
(`tools/pc/check_utf8_paths.py`) checks every unit in `src/pc` reaches it.

The PC test workflow runs with Unicode temporary directories and relocates
an executable into a folder containing accents, combining characters, CJK,
emoji, spaces and shell metacharacters. Its Windows check also runs
`play.bat` through the embedded-Python fallback with delayed expansion
initially enabled. Run it with `python tools/pc/test_path_layout.py --build
<cmake-build-directory>` after building the tests. Existing path-length
limits and the operating system's filename restrictions still apply.

### Build folders shared by worktrees

Worktrees share `tmp/` through a junction, and with it the default build
folders (`tmp/pc/game32`, and `tmp/pc/win32` where `package.py` builds a
release). A game object is kept while it is newer than its source and the
headers, which says nothing about whose source it was: an object another
worktree compiled is newer than every file this checkout wrote before it.
So `<build>/checkout.txt` names the checkout a folder was last built from
(written when a build ends, removed when one from another checkout starts),
and a build from any other checkout, or after one from another checkout that
was stopped, compiles everything again. What `build_game32.py` copies into
the folder (the SDK's headers and tools, the mods' data, the languages) is
copied when its bytes differ, not when it is newer. A build folder of its own
per worktree (`--build tmp/pc/game32-<name>`) still saves the full rebuilds.
Two builds into one folder at the same time are still not supported (there
is no lock): they write the same objects, and the one that ends last names
its checkout in `checkout.txt` whatever the other compiled after it started.

### Mod objects (`tmp/pc/mod-build`)

Every build compiles the code mods in `mods/` (`build_mods` in
`tools/pc/build_game32.py`, through `tools/pc/build_mod.py`) and copies each
object into `<build>/mods/<mod>/` when the copy there differs. Worktrees share
`tmp/` through a junction, so the objects are kept by what goes into them,
never by time: `tmp/pc/mod-build/<mod>-<key>/<library>`, where the key is a
SHA-256 of the compiler and linker files (name, size, time, as ccache's
`compiler_check=mtime`), the flags with the checkout's root spelled
`<root>`, the environment variables the compiler takes include directories
or options from (`CPATH`, `C_INCLUDE_PATH`, `CCC_OVERRIDE_OPTIONS`, ...),
`build_mod.py` (line ends normalized) and each source after the
preprocessor with the file names taken out of its line markers. So every
header a source includes is in it, from wherever it comes, and two checkouts
share an object exactly when they would build the same one (it then carries
the debug paths of the checkout that built it). The object is compiled from
that preprocessed text (`-x cpp-output`), not from the sources again, so it
is made of exactly what its key was taken from even when a header is edited
while the mod builds (`tools/pc/test_mod_cache.py`, ctest `pc_mod_cache`).
It is built in a staging folder (`.<mod>-XXXX`), checked and renamed into
place, so a folder there is always a whole object that passed, with the
names it leaves undefined in `<library>.undefined`. It is checked against the game's exports
on every build, reused or not, from that list: what a game lends comes from
its own sources, which the key does not cover. A failed check removes the
copy beside the game, not the shared object.

Running the preprocessor costs a process start per source, which a virus
scanner makes slow on Windows. `tmp/pc/mod-build/.memo/<mod>-<digest>/`
holds memos (as ccache's direct mode): the key the preprocessor gave, with
the SHA-256 of every file it read. The folder is named by the settings, the
sources and the names of every file under `src/` (or the SDK's `include/`)
and the mod's directory, so a new file that would be included first also
misses; a memo whose files are all unchanged gives
the key with no process started. A memo is not written when a file it read
changed while the preprocessor ran, nor when it named a file that cannot be
found. It leads only to a key that has an object: when that object is
missing, the key is taken from the preprocessor again before building.

Since the key covers the compiler, builds with different compilers (llvm-mingw
on Windows and gcc under WSL, say) keep an object each, and either runs on
both systems; `./build-pc.sh` builds both games with one compiler and so
copies one object beside both. Each build removes staging folders a day old
(from a build that was stopped) and memo folders no build has used for 30
days; objects are a few hundred KB and stay. `tmp/pc/mod-build` can be
deleted at any time, and so can the
folders from before the key (`<mod>/`, `e1e2eded/`) and
`tmp/pc/mod-objects`, which nothing uses any more.

## 32-bit game executable (bring-up)

The user chose a 32-bit (ILP32) host build as the bring-up memory model on
2026-09-20; 64-bit is deferred.

```sh
python3 tools/pc/build_game32.py
tmp/pc/game32/memories-pc                       # exits 70 at the first stub
MEMORIES_STUB_TRACE=1 tmp/pc/game32/memories-pc # survey: stubs log and return
```

How it works:

- `src/pc/guest/image.c` maps 2 MiB at `0x80000000`, mirrors the same pages at
  `0xA0000000` and at physical `0x10000..0x200000` (hosts reserve the first
  64 KiB; 24-bit packet links use these addresses), maps the scratchpad at
  `0x9F800000`, and copies the user's PS-X EXE image to its load address.
  `0x9F800000` is the console's own second view of the scratchpad (KSEG0);
  the retail code uses `0x1F800000`, where an Android app has its Java heap.
  Game C writes a scratchpad address as `SCRATCHPAD_ADDR(0x1F8003C0)`
  (`src/types.h`): the literal itself for the console build (every unit
  preprocesses to the same tokens, so matching is unchanged), the `0x9F80xxxx`
  view natively. No game unit leaves a scratchpad variable undefined, so
  neither symbol script pins one; `build_game32.py`'s `host_address` would
  pin such a variable at the view. Every native path that takes a retail
  address translates it (`MEMORIES_SCRATCHPAD_VIEW`, `image.h`): the
  interpreter (`mips.c`) for its own loads and stores, the GTE loads and
  stores and the string routines; the GPU's address resolver (`resolve.c`),
  save states and the control channel's peek and poke. The words the
  interpreter hands to native code (arguments and results) are translated
  only where the retail view is not mapped (`Memories_ScratchpadRetailView`
  0, an Android app): which of them are pointers is not known, so an integer
  in that 1 KiB would be changed too. On the desktops, where both views
  reach the same page, they go over unchanged, as on master. `0x1F800000` is also mapped as a second
  view of the same page where the host allows it (Windows, Linux). Where it
  cannot be mapped and nothing holds it, a native access through it faults
  and takes the null-page register rebase onto `0x9F800000`, reported once
  per site. That redirect needs a fault: where something readable holds
  the range (an Android app's Java heap) an untranslated access would reach
  the holder, which is why the translation is explicit everywhere.
  `MEMORIES_TEST_HOLD_SCRATCHPAD=rw` or `none` (not in a release) holds a
  page there before the guest is mapped, read-write or with no access, and
  at exit says whether anything wrote the read-write page (smoke case
  `options-hold-scratchpad`; a Windows build has no such hook and skips
  it, as Windows maps the retail view itself). On Windows each
  view is a 64 KiB section view of which only the first page is accessible,
  so the I/O registers from `0x1F801000` still fault.
  Guest pointers are therefore host pointers, and structure layouts, packet
  words and the `ygo_types.h` size assertions hold unchanged. Linux only so
  far; Windows needs the equivalent `VirtualAlloc`/`MapViewOfFileEx` calls.
- The driver reads symbol addresses from `tmp/project-build/SLUS_014.11.elf`
  and writes a linker script pinning every data symbol that game C leaves
  undefined or tentative (`-fcommon`) to its retail address: 612 symbols.
  The pins are `HIDDEN(name = address)`: in a shared object (the Android
  build's `libmain.so`) bionic adds the load bias to a default-visibility
  absolute symbol; a hidden one is resolved at link time.
  Initialized data defined in C stays in host `.data`, which keeps function
  pointers in C tables native.
- Guest-image tables do hold MIPS function addresses (the text opcode
  handlers `D_80090F18[]`/`D_80090E64[]`, callbacks an overlay installs), and
  native code calls through them. Every unit the driver compiles (game,
  overlays, `src/pc`, the generated units, and the mods through
  `build_mod.py`) is built with `-mretpoline-external-thunk` (clang, Windows)
  or `-mindirect-branch=thunk-extern -mindirect-branch-register` (GCC,
  Linux), so each indirect call or jump loads its target into a register and
  goes through `__x86_indirect_thunk_<reg>` (`src/pc/guest/branch_thunks.c`).
  A target in guest memory (`0x80000000`, `0xA0000000`, the physical mirror)
  goes to the resolver in `image.c`: the native function of that address from
  the generated `Memories_FunctionMap`, or `Memories_MipsThunk` for an
  interpreted overlay (a KSEG1 or physical address is looked up as its KSEG0
  alias, as the console runs the same code there); one with no native
  function ends the game with a report instead of being jumped to. On
  Windows, a piece of the physical mirror that Windows holds is not guest
  RAM, and a target there is jumped to as it is. Any other target is jumped to after one
  `test`. The thunks keep every register (the target's too) and the stack as
  the caller left them. A module function that C calls by name
  (`func_8016AA6C`, name entry's entry point in the shared `0x80168000` bank,
  and 12 more) would be pinned to its guest address like the data, and a
  direct call is no indirect branch: the driver finds those names in the
  objects' pc-relative relocations and gives each a host stub instead
  (`guest_branches.c` in the build directory), which pushes the address and
  enters the same resolver (`Memories_GuestBranchDirect`), so the resident
  module's function is picked as before. A name that is also used as an
  address (whose value must stay the guest's) stops the build. This is the
  primary mechanism, and it needs nothing from the system: the game works
  without DEP. The fault of executing guest RAM (mapped without execute
  permission) is a second safety net, which works only where DEP is on; the
  game leaves the system's DEP policy as it is (see "Faults" in the Windows part). Before the
  thunks it was the only one, and a player with DEP off (Windows `AlwaysOff`)
  crashed on the title's Options: the handler at `0x80038b4c` ran its MIPS
  bytes as x86. `MEMORIES_TEST_EXEC_GUEST=1` maps guest RAM executable, as it
  is without DEP, on any machine: with it, only the thunks stand between a
  guest call and the MIPS bytes (builds that are not releases only; the smoke
  case `options-exec-guest` uses it). `pc_branch_thunks` (CTest) checks all seven
  thunks' register and stack contract. clang also turns switch jump tables
  into compare trees; GCC sends them through a thunk too.
- Undefined functions become stubs calling `Memories_Unimplemented`. Current
  link report (`tmp/pc/game32/link-report.json`): 80 SDK, 61 handwritten
  assembly, 8 renamed host-colliding names (file I/O, `exit`, `ccos`/`csin`).
  A definition under `src/pc/sdk/` or `src/pc/guest/` replaces its stub.
- `config/pc/host_symbol_renames.txt` renames game references that would
  otherwise bind to host libc with a different contract: `rand`/`srand` go to
  the exact game RNG, `setjmp`/`longjmp` to a six-word i386 version that fits
  the game's 48-byte Psy-Q `jmp_buf` at `0x800E9DC0`, and `open`/`read`/... to
  `Psx_*` so host libc internals never reach a game stub.
- GTE inline assembly: `src/psyq/inline_c.h` selects
  `src/pc/compat/inline_c_native.h` natively, backed by the software GTE in
  `src/pc/compat/gte.c` (all COP2 commands found in the retail executable:
  RTPS, RTPT, MVMVA, SQR, NCDS, NCCS, NCCT, NCLIP, AVSZ3/4, GPF, plus the
  remaining color commands). `pc_gte` holds hand-computed known answers;
  it has **not** been compared against hardware or an emulator yet.

### Guest-width pointers (G32, CALL32, PSXLONG)

Game declarations carry upstream's annotations for a native 64-bit build
(`src/port_ptr.h`, cherry-picked from memories-decomp #6623 and #6637, since
the port does not merge upstream): `T *G32 p` for a pointer the game stores
in memory (a structure member or a global pinned to a retail address),
`T *G32 *p` for a local that walks such storage, `CALL32(type, f)(args)` for
a call through a stored function pointer, and `PSXLONG` for the Psy-Q
32-bit `long`. On the console and in this 32-bit build all three expand to
what they replace, so the objects do not change. `make check-g32`
(`tools/project/check_g32.py`, run by the metadata workflow) rejects game code
under `src/` without them; `src/pc`, the port's own host code, is exempt.

### What runs (2026-09-20)

`./build-pc.sh run` opens a 960x720 window and boots from the user's disc
image: Konami logo, company screen, the intro FMV with its soundtrack, title
screen with music, main menu and the Options screen. Menu input and sound
effects work; Start skips the movie. NEW GAME runs name entry (a name, END,
YES, "Your duelist code has been recorded") and continues into the opening
story scene with its dialogue, Simon's "Run away" branch, and the 3D town map:
moving between locations and entering them works (checked: the palace, the
shrine to its right, the location below), the card shop menu, SAVE/LOAD and
BUILD DECK (both panes, moving cards, paging, leaving, and the card viewer on
Triangle: turn animation, art, name, level, attribute, type, guardian stars,
text, ATK/DEF, from either pane), and the post-LOAD menu with LIBRARY: the
722-card grid, crosshair and scrolling, the card view, and the 3D model view
(Square or Cross in the card view): the monster stands on the wireframe floor
and animates. Checked with Ryu-Kishin only; its colors have not been compared
with hardware. The Free Duel `0x80168000` module is integrated; its opponent
grid initializes from the save's unlock flags and accepts cursor input.
The duel's 3D battle presentation loads and renders both monster models, the
arena and camera sequence, then returns to the field. Per-monster MODEL
control modules are retail MIPS overlays with no C source, and by default
(2026-09-21) they run as they are; the shared WA effect bank runs as the
decomp's native C when the disc delivered the retail bytes, else as MIPS: see
[MIPS-only effects](#mips-only-effects) below. Every duel effect id and every
monster's own attack choreography therefore plays with its retail timing,
colors and particles.
Music tempo was checked by measurement: the retail `SetRCnt` (read from the
resident assembly) programs counter 2 as mode `0x248`, sysclk/8, so target
`0xE000` is 73.83 Hz; `MEMORIES_TRACE_FRAMES=1` reports 73.7-74.0 delivered
sequencer ticks/s and 59.9-60.0 VBlanks/s, and each tick runs eight sequence
steps with no other time source (`GetRCnt`'s value is discarded).
Audio has otherwise only been checked by signal level and waveform statistics from
`MEMORIES_DUMP_AUDIO`, not by ear or against hardware. Nothing here has been compared frame-by-frame against an
emulator yet; the screens were checked by eye from headless frame dumps.

The native mixer bus split was verified with `MEMORIES_TRACE_SPU=1`: title
music key-ons used only voices 0 through 19, while scripted main-menu cursor
moves keyed masks `0x100000`, `0x200000` and `0x400000` (voices 20 through
22). This agrees with `SD_VOICE_SLOT_FIRST_VOICE == 20` and the four-slot
allocator in `sound_effect_voices.c`; voice 23 is the fourth slot in the same
mask range. CD/XA remains a separate streamed input.

Input. Keyboard, mouse and controllers are all live at once and OR together
on port 1.

**Game > Controls...** opens the keyboard/controller editor: a resizable window
with a grouped binding table, the PlayStation pad picture, live input preview,
rebinding, saved profiles and explicit controller selection for either port. See [Controls](controls-menu.md). The table below lists factory defaults;
custom bindings are stored separately in `controls.txt`.

| PlayStation | Keyboard | Xbox controller | Mouse |
|---|---|---|---|
| D-pad | Arrow keys | D-pad, left stick | wheel = up/down tap |
| Cross | X | A | |
| Circle | S | B | right button |
| Square | Z | X | |
| Triangle | A | Y | middle button |
| L1 / R1 | Q / W | LB / RB | |
| L2 / R2 | E / R | LT / RT (past a third of travel) | |
| L3 / R3 | T / Y | stick clicks | |
| Start | Enter | Menu/Start | |
| Select | Right Shift | View/Back | |

**View > Japanese buttons (Circle confirms)** (`jp_buttons`,
`MEMORIES_JP_BUTTONS=1`; off by default) gives the Japanese release's layout:
Circle confirms and Cross cancels (Square confirms in both). Every button
check matched in both releases is the USA one with Cross and Circle
exchanged (the card viewer's close button is the one that is not), so the
port exchanges those two bits of the pad state the game reads, in
`run_vblank` (`libetc.c`, `platform/button_layout.h`), for both pads, and
changes no game code. It
applies from the next VBlank. What the player's keys and controllers press is
exchanged; the mouse (right button stays "back") and scripted input
(`MEMORIES_INPUT`, `MEMORIES_INPUT2`, written in the USA layout, so a
check's input means the same whatever the setting) are not. The memory card
slot menu reads the game's pad state, so it follows and its hints name the
buttons it takes. The deck slot screen (F6) reads the pad itself but
exchanges the same two bits, so it follows too, hints included. The Controls
and Mods windows read the keys and controllers themselves and keep their own
buttons. Mods: an
`INPUT` before-hook sees the controller's bits, as `host->pad` does; the
after-hook sees what the game gets ([mod API](mod-api-3.md)).

**View > Touch controls** (`touch_controls`, `MEMORIES_TOUCH_CONTROLS`: 0
Automatic, the default; 1 Show; 2 Hide) is an on-screen controller for touch
screens (phones, tablets, touch laptops): a D-pad and the four face buttons
at the middle of the left and right sides, L2/L1 and R1/R2 above them,
SELECT and START below (`platform/touch_pad.c`). It is drawn with the game's
own pictures, cut off the disc from the boot package the game keeps in VRAM
(`touch_pad_art.c`, `cards/disc_art.h`): the round Cross, Circle, Triangle and
Square, START, the L1/L2/R1/R2 tabs and the boxed arrows (Left is Right's,
turned round); SELECT is set in the game's text font. A button let go is
see-through; held, it is solid and a little smaller. Automatic shows it once
the screen is touched (that touch presses nothing) and hides it when a key or
a controller button is pressed. Several fingers work at once; a finger that
lands on the pad stays the pad's while it slides (the D-pad goes by the angle
from its middle, so between two arrows presses both), and one that lands
elsewhere is the mouse SDL makes of it (the menu bar). It presses the button
it shows: its bits join the pad state beside the mouse's, which View >
Japanese buttons does not exchange, and a tap shorter than a frame counts
once. Hidden, it draws nothing and takes no touch.
**MENU**, in the game's font under L2/L1 (the D-pad below it), opens the
menu bar's first menu (`TouchPad_TakeMenu`, taken in the event pump): the
way to the menus where the bar is hidden, as in fullscreen or on a phone.
While a menu, a notice or a bar drawn over the picture is up, the pad
steps aside (`TouchPad_Block`: it draws nothing, takes no touch and lets
go of what it held), so their rows and buttons are the finger's; it is
decided once per event pump, after all of its events, since SDL delivers a
tap as finger events and mouse events made of them. The mouse SDL makes of
a finger does not hover: its moves reach the menu only while the finger is
down (a drag), so its last place never keeps the bar shown, and a tap at
the top of the screen shows a hidden bar under it. Where the window is
always the whole screen (`Platform_HasWindowModes` 0, an Android app) the
bar is drawn over the picture instead of pushing it down. Where the system
gives the density (Android), a button is 48 to 80 dp wide (13% of the
short side otherwise, and never above 16% of it), and SELECT, START and
MENU take a touch a button tall. The save slot and deck slot menus, which
the pad plays, keep between its columns while it shows
(`TouchPad_FreeSpan`, `Menu_SetOverlayArea`); narrower than their one-row
layout, a slot's details go on lines under its name and the hints wrap
(`saves/flow_text.h`); the deck slot menu's messages break at spaces where
the box is narrower than them (`centred_words`, `deck_menu.c`); the text
gets smaller only when even that does not fit.

Esc quits (it closes an open menu first): it is the default key of Game >
Controls' **Exit game**. With **File > Confirm before quitting** on (the
default) it asks "Quit the game?" first: Quit or Keep playing, Keep playing
focused and last, so Enter and Escape stay in the game. Closing the window
(its button, Alt+F4) and File > Exit ask the same (`quit_prompt.c`); with the
setting off all three quit at once. Every shortcut below is a row of Game > Controls' **Game** list, like
Exit game: the keys named are the defaults, each can be rebound, cleared or
given a controller button too, and the File, Debug and Game menus show the
key bound now ([Controls](controls-menu.md), `host_actions.c`). F1, F2 and F4
select those state slots, F5 saves, and F7 loads; State slot 3 has no key by
default (F3 cycles the debug HUD) and is selectable from File. F6 opens the
deck slots. F11 or Alt+Enter (the main Enter or the keypad's) switches between
the window and desktop fullscreen and saves `fullscreen`; in fullscreen Esc
first returns to the window. Alt+Enter, F10 and Esc's own uses stay fixed
(the Controls window lists them under Fixed keys). The Enter pressed under
Alt stops there, so it never presses Start, and Alt is a reserved modifier
no binding can use. While a menu or a notice is open it takes the keys
first, Alt+Enter included. Fullscreen is SDL only: the X11 backend has a
fixed window (`Platform_HasWindowModes` is 0 there).

The keypad's + and - (Volume up and Volume down) raise and lower the master
volume by 5 (0-100, repeating while held), which is the Audio menu's Master
slider and the saved `master_volume`; M's mute stays on or off as it was, so
a change while muted is heard when M unmutes. There is no on-screen notice:
an open Audio menu shows the slider move, and `MEMORIES_TRACE=menu` logs each
step. Binding a keypad key to a pad button takes it off the volume, as any
move of a key between rows does. Both backends (SDL and X11). Controllers
(`platform/gamepad_evdev.c`) are read through evdev, which names controls by
meaning, so the one table covers Xbox pads on xpad, xone and xpadneo and most
other pads. `/dev/input` is rescanned about once a second while a port is
free: plugging in mid-game works, and unplugging frees the port. The first
controller shares port 1 with the keyboard; a second becomes port 2, which
otherwise reports "no pad" as before. `MEMORIES_NO_GAMEPAD=1` turns the scan
off. Checked with a virtual Xbox 360 pad made through uinput (hat, stick,
A, RT, removal); not yet with physical hardware. The account needs read access
to the pad's `/dev/input/event*` node, which desktop logins get by default.

Environment switches: `MEMORIES_DISC` (the raw MODE2/2352 image; without it
the first `.bin` of the USA disc is taken from `game/` beside the executable,
the executable's own folder, `game/` in the user directory, then `game/` in
the current directory), `MEMORIES_SCALE` (1-8, default 4, which puts the 320x240
picture on screen at 1280x960), `MEMORIES_VOLUME` (0-100, overrides the
stored volume and applies headless too), `MEMORIES_SETTINGS` (another
settings file), `MEMORIES_HEADLESS=1`,
`MEMORIES_DUMP_FRAME=N` with `MEMORIES_DUMP_PATH` and optional
`MEMORIES_DUMP_VRAM=1` (write frame N as PPM and exit),
`MEMORIES_DETERMINISTIC=1` (the virtual clock for the whole run, headless
or in a window, with no frame dump needed: two runs with the same input
give the same frames; in a window the frames are shown at the game speed
and pause and frame step work, headless runs flat out; see
[Agent control](agent-control.md). `MEMORIES_HEADLESS` with
`MEMORIES_DUMP_FRAME` and `MEMORIES_SPEED=-1` still selects it too),
`MEMORIES_RECORD=path` (the pad bits of every VBlank, a frame hash at
every frame and, with `MEMORIES_RECORD_STATES=N`, a state every N VBlanks,
as text; `src/pc/debug/recorder.h`), `MEMORIES_PLAY=path` (play such a
file back instead of the live pads, ending the game at its end; replays
are made and checked with `tools/pc/replay.py`, and `python3
tools/pc/replay.py run` checks those in `tests/pc/replays/` before a PR,
like `smoke.py`: a local gate, since CI has no disc),
`MEMORIES_CONTROL=<port>` (a control channel on 127.0.0.1:<port>: the
game waits at its first frame for a client, which steps it, holds pad
buttons, reads and writes guest memory, takes pictures and save states;
it waits `MEMORIES_CONTROL_WAIT` seconds for one, 10 unset, -1 for good;
`tools/pc/yfm_control.py` is the client, the commands are in
[Agent control](agent-control.md)),
`MEMORIES_WINDOW_SHOT=N` (save the window as shown at frame N, as the
screenshot key does, into `MEMORIES_SCREENSHOT_DIR` or the user folder),
`MEMORIES_SCALE_AT=N:S` (change the internal resolution to S at frame N, as
the View menu would), `MEMORIES_RESTART_ENV="NAME=value;NAME=;..."` (the
environment a restart of the game, `Platform_RestartGame`, starts the next
one with, an empty value removing the name, so a check can drive the game
past the restart instead of replaying its `MEMORIES_INPUT`; applied by the
game itself, so on Windows only with `MEMORIES_NO_MONITOR=1`),
`MEMORIES_MODE_AT=N:M[,N:M...]` (from frame N on, the
next main mode a screen publishes becomes M, `main_modes.h`; M 0 is the
game's debug menu, reached with the options case's input; see
`notes/image-remaster.md`),
`MEMORIES_INPUT="700:0008,706:0000"` (scripted pad bits from a frame on;
`MEMORIES_INPUT2` the same for the second pad, which then counts as
connected: two-player trades and duels),
`MEMORIES_DEBUG_CHEST=N` (N of every card in the trunk),
`MEMORIES_DEBUG_DECK="723-762"` (the deck, as ids and ranges repeated to
forty) and `MEMORIES_DEBUG_STARCHIPS=N` (the balance, as Set StarChips puts
it, capped at 999999 or a mod's `limits`), all once a save is live
([More cards](more-cards.md)),
`MEMORIES_NO_AUDIO=1`, `MEMORIES_DUMP_AUDIO=path` (raw s16le stereo 44.1 kHz
instead of a device),
`MEMORIES_TEST_EXEC_GUEST=1` (guest RAM mapped executable, as without DEP,
to check that the branch thunks carry every guest call; see "How it works";
not in release builds),
and `MEMORIES_STUB_TRACE=1`. Traces on stderr: `MEMORIES_TRACE_SPU=1` (every
`SpuSetKeyOnWithAttr`), `MEMORIES_TRACE_INPUT=1` (scripted pad changes with
frame and VBlank numbers; script frames are presented frames, which run
behind VBlanks during loads and the movie), `MEMORIES_TRACE_FRAMES=1`
(per-frame game/present/rasterizer time and missed VBlanks) and
`MEMORIES_TRACE_DISC=1` (each sector's destination),
`MEMORIES_TRACE_MEMCARD=1` (card checks, completed commands and directory
entries), and
`MEMORIES_TRACE_MENU=1` (menu-bar clicks) and
`MEMORIES_TRACE_MODEL_MODULES=1` (MODEL bridge commands) and
`MEMORIES_TRACE_DUEL_EFFECTS=1` (WA effect requests and payloads). Without an audio device
(or headless) a silent sink still runs the SPU: the game polls envelopes and
the disc service waits for CD-input room, so a stopped SPU hangs the game
after the movie.

Key-on and key-off requests are applied by the mixer at its next 256-frame
period (5.8 ms), but the hardware answered `SpuGetKeyStatus` and
`SpuGetVoiceEnvelope` at once, and the sound driver relies on that: it picks
a free SFX voice by envelope 0 and spins on status after a key-off. So both
reads answer from the request (`keyed_mask`; a pending key-on reads as full
attack), not from the mixer. Before that, two sounds keyed within one mix
period could land on the same voice and the first never played, which
showed as missing sound effects at 200% and above; `MEMORIES_TRACE=spu`
logs "replaces a key-on not yet mixed" whenever it still happens.

### MIPS-only effects

Two kinds of duel code were loaded as MIPS bytes from the archives:

- the shared WA effect bank (native C since 2026-09-30, below) that every duel package copies to `0x80146000`,
  entered at `0x801462B0` with an effect id: fusion (1), battle damage (2),
  destruction (3), the magic, trap, ritual, terrain and field effects up to
  id 23;
- the per-monster MODEL control modules the loader swaps into
  `0x8013A000`/`0x8013B000` (slot A) and `0x8017A000`/`0x8017B000` (slot B)
  with each monster: a primary module (`return 2` in every record seen) and
  a variant module holding that monster's attack choreography and impact
  particles. MODEL.MRG has 621 records and 1,181 distinct variant modules.

Translating those one by one was the earlier plan; the default now runs the
loaded bytes through the interpreter in `src/pc/guest/mips.c` (integer MIPS I
plus COP2 through the software GTE), which bridges every call that leaves an
overlay to the native function of that address. A code-following scan of the
WA bank and all 1,181 variant modules (`tmp/`, 2026-09-21) found only
instructions the interpreter has, no GTE opcodes, and 26 SDK routines the
resident C never references (`GetTPage`, `SetPolyFT4`, `RotMatrixX`,
`gteMIMefunc`, `catan`, `GsGetLs`, ...); those are ported in
`src/pc/sdk/libgte_extra.c` from the resident assembly so they enter the
function map. Calls the other way (a native routine reaching a callback an
effect installed) go through the branch thunks (or, as the second net, the
guest-call fault handler) into the interpreter. Interpreted code runs on its
own stack mapped at `0x9FF00000`, negative like every console address, and
nests through native calls.

A bridged call forwards a0-a3 and eight stack words (sp+16 to sp+44). The
one native callee that takes more is `Model_QueueTintRequestForParts`
(0x80058838), whose part list ends at the first negative word: 140 call
sites in the variant modules, 90 of them passing parts 1 to 18 (24 words,
up to sp+92) and 10 a list read from a table (13 words). `call_native`
hands the list over where the module stored it on the guest stack
(`Model_QueueTintRequestForPartList`); before, the walk ran past the
forwarded words into the host frame: Master & Expert's attack (command 9 of
the shared variant module 0x2F0) asked for parts 1-18 and got 1-7. A scan
of MODEL, WA and SU found no other variadic callee: `printf` is handled in
`call_native` itself, and `FntPrint`, `sprintf` and
`Model_SetSlotProperties` are never called from them.
`tests/pc/mips_varargs_test.c` runs such a call through the interpreter.

Checked from `slot1.state`: the AI's attack (card clash, damage lettering,
burn destruction: ids 2 and 3), the player's attack committed with Square
(the 3D battle: arena, both monsters, the variant module's setup command and
70 update frames with its sparkle burst, then the return to the field) and
effect 11, with no interpreter failures. The pad word the game reads is
byte-swapped relative to the raw pad, so in `duel_scene_field_actions.c`
`0xC0` is Cross or Square: either starts an attack, Square commits the
target with the 3D presentation (`D_8009B229 = 1`), Cross without it.

**The effect bank in native C.** Upstream matched the whole North American
bank (85/85 functions, #6691), and its C is linked in as the `duel_effects`
module (`src/overlays/duel_effects/`, bank `0x80146000`, identifier word
`0x18`; `tools/pc/build_game32.py`). `Memories_DuelEffectControl`
(`src/pc/overlays/duel_effects.c`, the function map's entry for
`0x801462B0`) calls the decomp's dispatcher instead of interpreting, under
one rule: **the native C runs only for the retail bytes.** A disc delivery
that writes the bank's first word (`0x80146000`, a new copy of it) marks it
pending; the next effect call
hashes all `0x16000` bytes (SHA-256, before the bank has written its own
variables) and compares them with the image `config/slus_01411/overlays.json`
records (`baa203b9...`, the same at all seven WA_MRG copies). A mod or disc
patch that changed any byte, code or tables, gets the delivered bank
interpreted as before (`src/pc/guest/retail_image.c`). `MEMORIES_TRACE=mods`
logs each verdict (`duel_effects: retail bytes ... native C` or `changed
bytes ... interpreter`); save states carry it (chunk `retail-images`), and a
state from before the check, whose bank can no longer be hashed, gets the
interpreter until the next duel package. The bank's functions are kept out of
`Memories_FunctionMap` (`GATED_MODULES`), so nothing else can reach the native
C for a modded image. Only the NA executable runs here, so the regional
banks (French matched 85/85, Spanish 84/85, European and Japanese 80/85
upstream) are never loaded; the language packs take only text from PAL discs.

Switches: `MEMORIES_DUEL_EFFECTS=interpreter` interprets the bank even when it
is retail (the reference for comparisons); `MEMORIES_DUEL_EFFECTS=skip`
(called `native` before the bank ran as native C; `native` now means the
default) restores the bring-up behaviour
(ids 1-3 interpreted, others complete at once); `MEMORIES_FRAME_HASHES=<file>`
writes a hash of VRAM per presented frame for comparing two runs; `MEMORIES_MODEL_MODULES=native`
uses the resident spark burst instead of the monster's module; an effect or
module the interpreter cannot run is reported once on stderr and falls back
the same way. `MEMORIES_TRACE_MIPS_PRINTF=1` prints the modules' own
`printf` format strings.

### Credits

The ending's credits (`Main_RunCredits` phase 2, `func_800507D0`) load 16
SU sectors from `0x4C7` into `0x80180000`, where the main-menu overlay is
otherwise linked natively, and call the loaded MIPS directly
(`func_801807B0`, `func_80181C4C`, ...). `Memories_MipsInOverlay` therefore
counts `0x80180000-0x80188000` as interpreted whenever no native module is
resident in that bank (the bank's identifier word decides), and the
outermost interpreted frame starts 64 bytes below the stack's top because
the module stores its arguments at `sp+0` on entry. The module's external
calls are `LoadImage2`, `IsIdleGPU`, `GsSortPoly`, `strlen` and
`Krom2RawAdd2`; the retail string routines are bridged by address in
`call_native` next to `memset`, and `Krom2RawAdd`/`Krom2RawAdd2`
(`sdk/libapi_krom.c`) stand in for the BIOS kanji ROM the port has no copy
of: each Shift-JIS character is rendered once with FreeType from the face
fontconfig names for Japanese (Noto Sans CJK here) into the ROM's 16x15,
30-byte, one-bit pattern, which the module reads through the returned host
address. Checked from a state at the ending's last dialogue, mashing Cross
(`MEMORIES_INPUT`) at 400%: names and the wireframe monsters through
"Created by Konami Computer Entertainment Japan" with no interpreter failure.

Since 2026-09-30 the module runs as native C when the delivered bytes are the
retail module: upstream matched it 6/6 (#6692), and `src/overlays/credits` is
linked in as a gated module (identifier `0x10`, bank `0x80180000`, symbols
prefixed `credits__`). The function map sends its three entries
(`0x801807B0` set-up, `0x80180A24` update, `0x80181C4C` a group of lines) to
`src/pc/overlays/credits.c`, which applies the duel-effect bank's rule (see
[MIPS-only effects](#mips-only-effects)): SHA-256 of the `0x8000` bytes at the
first call after a delivery of the module's first sector against
`f125a2a6...`, native C if equal, the interpreter above otherwise. Only a new
first sector starts a new check because the credits then stream 20 more
sectors into the module's tail (`0x80185CD4-0x8018FCD4`) while they run. `MEMORIES_CREDITS=interpreter` forces the
interpreter. Gated modules are kept out of the module registry, so the
interpreter still owns `0x80180000-0x80188000` whenever it runs a modded image.

The retail game never leaves the credits: `Main_RunCredits` runs the scene in
its last phase, after the save and the secret number, and drops the answer of
`Model_IsCreditsPresentationComplete`, so the screen stays on the last credit
until the console is reset. The port resets it three seconds after the
presentation is complete (`platform/credits.c`, `Platform_RestartGame`):
the game starts again with the logos, as the console would; the game's code
is unchanged. (It was a setting, `return_after_credits`, on by default,
until the Game menu was trimmed; the key is ignored now.) It used to publish
the main menu's mode instead, which still happens if the restart fails. That
went back to the title with the credits' VRAM in place: the presentation
uploads over palettes the boot uploads once (y 240-248 from x 512, y 248
from x 0, x 480-511 from y 256), and the Build Deck trunk before a Free
Duel came out green. Game > Restart game in the credits restarts the game
too (see "Back to the title screen"). Checked by entering the ending with
`MEMORIES_MODE_AT=1000:15`, declining the save and letting it end (the
presentation is complete at frame 7308 with the options-case input): the
game boots again and a loaded save's Build Deck trunk before a Free Duel is
blue, as without the credits.

### Where the player's files go

Nothing the player owns lives beside the game any more. `platform/paths.c`
resolves one user directory -- `Documents\My Games\YFM Re-Decomp` on Windows,
`$XDG_DATA_HOME/YFM Re-Decomp` (`~/.local/share/YFM Re-Decomp`) elsewhere,
`MEMORIES_USER_DIR` instead of either -- and everything the port writes goes
under it:

| File | What it is |
|---|---|
| `settings.txt` | the settings (`MEMORIES_SETTINGS` names another file) |
| `controls.txt` | the control bindings (`MEMORIES_CONTROLS`) |
| `saves/slot01.sav` ... `slot10.sav` | the game saves, one per slot (see "Saves" below) |
| `memcard1.mcd`, `memcard2.mcd` | memory cards of older builds, only read now (`MEMORIES_MEMCARD1/2`) |
| `saves/*.mcr`, `*.mcd`, `*.gme`, `*.mcs`, ... | memory card files to import, renamed `*.imported` once imported (see "Saves") |
| `states/slot1.state` ... | save states (`MEMORIES_STATE_DIR`) |
| `screenshots/` | F12 and **File > Screenshot** (`MEMORIES_SCREENSHOT_DIR`) |
| `mods/` | mods the player installed (`notes/modding.md`) |
| `mod-data/<id>/` | whatever a mod stores, the only place one may write |

Portable mode: when a file named `portable.txt` sits beside the executable
(its contents are ignored), the user directory is `user/` beside the
executable instead, with the same layout -- settings, controls, saves,
states, screenshots, the player's mods and mod data all go there, and the
port says so on stderr. It is a folder of its own because `mods/` beside the
executable is the release's. `MEMORIES_USER_DIR` still wins over it, and
`MEMORIES_SETTINGS`, `MEMORIES_CONTROLS`, `MEMORIES_STATE_DIR` and
`MEMORIES_SCREENSHOT_DIR` still name their own places. The folder must be
writable (not under `Program Files`); if `user/` cannot be made the port
falls back to `./saves` as for any user directory. Without the file nothing
changes.

The program directory (the release's `mods/`, `languages/`, the build's
`buildid` and `symbols/` for save states, `game/`) is the executable's
folder; `MEMORIES_PROGRAM_DIR` names another, for a process that is not the
port's own executable (an Android app, which unpacks those files).

What an older build left in `./saves` is carried over on the first launch
that finds the destination missing (`Paths_MigrateLegacySaves`), so an
existing card, settings and bindings survive the move. The game's own files
(the disc image, `mods/` as shipped) stay where the release put them and are
only read.

When a write there fails, the player is told where and why, never only that
it failed (`Paths_WriteError`, `paths.h`): the full path as Explorer shows it
and the system's own reason (FormatMessage, in the user's language, on
Windows; strerror elsewhere), e.g. `Could not save settings to
C:\Users\...\Documents\My Games\YFM Re-Decomp\settings.txt: Access is
denied.` When Windows denies access inside Documents, one sentence follows:
an antivirus's "ransomware protection" or Windows' "Controlled folder access"
may be blocking the Documents folder; allow `memories-pc.exe` there (Avast's
Ransomware Shield did this to a player of v0.1.3-preview.1: every setting
reverted at the next launch, with nothing saying why). Where it is said:

- settings (`Settings_Save`, `Settings_LastError`): the Mods window's status
  line (it grows to more lines for it), Game > Language's notice, and for
  every other save (menu items, sliders, hotkeys, a moved window, which do
  not look at the result) a "Settings not saved" notice, once until the
  reason changes or a save succeeds (`Settings_TakeNewError`, `menu.c`);
- game saves: the save slot menu's message, with the path and reason under
  it (`SaveSlots_LastError`); deck slots, the mods' card and duelist records
  and memory card images on stderr;
- save states (F5), screenshots (F12) and a mod profile: a notice, or the
  Mods window's status; Controls: its window's status; Help > System info
  when `system-info.txt` cannot be written; the ROM location
  (`disc-path.txt`): the setup error.

Nothing about where files go changes. The crash reports' facts (and Help >
System info) carry `user dir: <folder>; writable: not tried yet` until the
game writes something there, then the last write's outcome: `writable: yes`
or `writable: no: <reason>` (`Paths_WatchUserDir`; failures are seen in
`Paths_WriteError`, successes said with `Paths_WriteDone` by the settings,
save slots, states, screenshots and System info). Nothing is written only to
find out: a test file made and removed at every start would raise Controlled
folder access's notification on every launch and looks like ransomware to
antivirus heuristics. Help > System info opens `system-info.txt` before it
reads the facts, so what it shows answers the question when nothing had been
saved yet.

### Window and menu bar

Two window backends exist under `src/pc/platform`, chosen at build time
(`tools/pc/build_game32.py --backend sdl|x11`, or `MEMORIES_BACKEND`; SDL by
default: SDL 3.4.16, the release the Windows build ships, built as a 32-bit
static library by `tools/pc/build_linux_sysroot.py`). Both share the menu (`menu.c`), the interrupt clock and the
scripted input (`platform_common.c`).

**SDL3/OpenGL** (`sdl.c`, the default) is the portable one: window, keyboard,
mouse, controllers (`SDL_Gamepad`, so any pad SDL knows, hot-plugged, two
ports) and audio (`SDL_AudioStream`, 256-frame periods) through the one
library that exists for Linux, Windows and macOS. An explicit OpenGL presenter
is preferred, with SDL_Render as a fallback when a GL context is unavailable.
The picture is a 320x240 streaming texture the GPU scales with nearest filtering, and the menu is a
transparent ARGB texture blended over it, uploaded only where it changed; so
the CPU never scales a frame. Window layout, the menu/HUD and pointer input
use SDL logical coordinates; SDL scales the complete composition to the
physical render target on high-DPI displays. SDL selects the native desktop
backend, avoiding XWayland cursor and DPI mismatches on Wayland; set
`SDL_VIDEODRIVER` only to override that choice for diagnostics. On this
machine (renderer `opengl` under X11)
the present path is about 0.25 ms a frame, down from 1.6 ms for the software
scaling below. Signals are blocked while SDL creates threads so the SIGALRM
clock stays on the main thread; Windows will still need the clock replaced
(`platform_common.c`). `MEMORIES_SDL_SCRIPT="200:click:20:13,260:move:60:69,
420:key:escape"` pushes pointer and key events at presented frames for
tests: synthetic X input does not reach SDL correctly (XI2, and XTest is
refused by this compositor), so the menu was verified that way.

**X11** (`x11.c`, `audio_alsa.c`, `gamepad_evdev.c`) is the Linux-only
fallback with no dependency beyond Xlib: it is plain Xlib. The frame it shows is one ARGB32 buffer the port
composes in software: the 320x240 picture scaled by an integer (Video menu,
`MEMORIES_SCALE`, 1-8) under a 26-pixel menu bar. The buffer lives in MIT-SHM
memory the X server reads directly (`XShmPutImage`), so a frame costs a
request rather than a copy of the whole picture down the socket, and the
menu's own activity (hover, an open menu, a slider drag) repaints and shows
only the rectangle the menu covers (`Menu_Bounds`), never a frame. With
`MEMORIES_TRACE_FRAMES=1` on this machine the whole present path, scaling
included, is about 1.6 ms a frame at 4x with no missed VBlanks.
`MEMORIES_NO_SHM=1` forces the `XPutImage` path.

Menu text (both backends) is FreeType through fontconfig's `sans-serif` face at 13 px, cached
once as coverage bitmaps and blended into the frame; a machine without a face
falls back to a built-in 5x7 font at double size. Host UI text (menus, notices, Mods, Controls,
HUD) is UTF-8: ASCII is rendered at start, any other character the first time it is drawn, and
one the face lacks (or a byte that is not UTF-8) shows as "?"; code that cuts such text to fit
uses `Menu_TextBack`/`Menu_TextFit`/`Menu_TextTrim` (menu.h) so no character is split. The bar is dark with an
accent highlight; menus have hover rows, separators, shortcut hints, check
and radio marks, and a shadow. Keyboard: F10 opens the first menu, arrows
move, Enter activates, Esc closes (with no menu open it is Exit game, asking first).

| Menu | Items |
|---|---|
| File | Save/load state, slots 1-4, screenshot, reload settings, confirm before quitting, exit |
| Audio | Master/music/SFX sliders, mute and focus-loss mute, Gaussian (console) or cubic (sharper) voice interpolation (`audio_interpolation`) |
| Video | Window scale and Menu size submenus, window mode, scaling/aspect/filter/VSync choices |
| View | Fusion helper (`fusion_helper`, [notes/fusion-helper.md](fusion-helper.md)), Card passwords, Library: show every card, Free Duel progress (see [View > Free Duel progress](#view--free-duel-progress)); Duel rank submenu: Off, Rank, Rank and score (`rank_meter` 0/1/2, see [Duel rank](#duel-rank)); Opponent's name for COM; Japanese buttons |
| Game | Game speed, Frame rate and Cheats submenus (see [Cheats](#cheats)), Card drops, Deck slots, card browsing, Restart game |
| Mods | opens the mods window, which lists every mod found in `mods/` beside the executable and in the user directory (`notes/modding.md`) |
| Debug | HUD levels, pause/step, frame and VRAM dumps |
| Trace | Live frames, disc, SPU, input and state log-channel switches |

`MEMORIES_TRACE_MENU=1` logs menu clicks and keys. The menu never reaches the pad:
a click on the bar or in an open menu, and the wheel there, are the menu's.
Menu changes from events (hover, clicks, keys, resizes) mark the menu dirty
and it is repainted once with the next game frame, or at most 120 times a
second while paused; a 1000 Hz mouse sweeping the bar used to present a
frame per motion event. Events are pumped before a frame is composed, so the
frame shows the input and menu state of that moment. The overlay is only
recomposed when the menu is dirty or the HUD's text changes (`Hud_Signature`;
the full-statistics level changes every frame), and a dropdown's shadow
blends only the strips outside the box: composing every frame with twelve
alpha passes over a 4K dropdown made the game crawl whenever a menu was
open. `MEMORIES_TRACE=window` reports compositions per 120 frames.
A row with a triangle opens a submenu beside it (one level: `ITEM_SUBMENU`,
`submenus[]`), on hover, click, Enter or Right; Left or Esc closes it.
A submenu that would run off the bottom of the window moves up, and a menu
still taller than the window (a small window; a phone's finger-sized rows)
is cut to it and scrolls: arrow bands at its ends move it a row, the wheel
or a drag scrolls it, and the arrow keys keep the lit row in view. A press
on a row of such a menu acts when it comes up without having dragged, so a
finger can scroll it without choosing what it lands on; a slider's row
takes the pointer at once. One geometry (`drop_box` in `menu.c`) serves the
drawing, the hit-testing and the dirty rectangle. Menus that fit are drawn
and behave as before.

The menu draws at a size multiple (`menu_scale`, `MEMORIES_MENU_SCALE`, View >
Menu size): bar, rows, marks, font and the HUD all scale together, and the
window is sized for the bar it gets. Automatic (0) follows the window height
(`Menu_AutoScale`): 1 below 1200 rows (including a 4x window), 2 at 1200 rows
and above (including a 4K display). This is one step smaller than the original
automatic size, with a minimum of 1; explicit 1x–4x choices are unchanged.
`MEMORIES_SDL_SCRIPT` accepts `frame:shot` to save the composed
window, which is how the menus are checked, `frame:key:alt+<name>` to
send a key with Left Alt held (`alt+return` is Alt+Enter; `kp_plus` and
`kp_minus` are the keypad's + and -, `f5` saves a state), and
`frame:keydown:<name>` / `frame:keyup:<name>` (or `down` / `up`) to hold a
key across frames (a `key` is pressed and released in one pump, before the
game reads the pad; `s` is Circle and `x` Cross by default), which is how
Japanese buttons was checked through the keyboard; `f8` is one of the names.

### Rewind (removed)

Rewind (hold F8, `rewind=1`) was taken out on 2026-09-28. It wrote the
whole 7.5 MB save state to memory every 10 presented frames and XOR-encoded
it into a ring, about 2 ms on the game's thread each time; at 200% speed a
frame has 8.3 ms, and those frames were the ones that ran over (a duel at
4x with 3D Monsters showed 104-118 frames/s instead of 120). `SET_REWIND`
stays in settings.h, retired, so mods keep their setting numbers; a
`rewind=` line left in `settings.txt` does nothing (it is kept in the file as
an unknown line). `serialize` still writes the stack chunk last.

### Cheats

**Game > Cheats** (`src/pc/debug/cheats.c`). Every row is off, or at the
console's value, until the player picks it; nothing changes on screen before.

The rows that change the save (Give, Unlock and Set StarChips) do nothing
until a game is started or loaded, and say **Load a save first** instead
(`Cheats_SaveLoaded`: the save workspace holds a deck, the same test as
Deck slots and `MEMORIES_DEBUG_CHEST`). Before that the workspace is scratch,
and on a new game it becomes the save when the name entry closes.
Give also covers the cards mods added (their trunk is kept beside the save,
[More cards](more-cards.md)). It refuses while Build Deck, or its screen
before a duel, is open, and says **Leave Build Deck first**
(`Cheats_ChestOnScreen`): that screen lists its own copy of the chest and
writes it back over the save as it closes, so cards given meanwhile were
neither listed nor kept. `MEMORIES_DEBUG_CHEST` waits for the same. The settings
rows (LP, free spending, the CPU's hand) change nothing in the save and work
at any time. Starting LP is 1 to 32767 (`cheat_life_points`,
`MEMORIES_CHEAT_LIFE_POINTS`; the menu offers 1000, 4000, 8000 and 9999); at
the console's 8000 a mod's `limits` decide the start instead, and any other
value comes first. Set StarChips and `MEMORIES_DEBUG_STARCHIPS` stop at the
game's 999999, or at a mod's `limits` (notes/gameplay-tables.md).

### Back to the title screen

**Game > Restart game...** is the player's way back to the title without
closing the program, like a console's soft reset. It first asks "Restart the
game? Unsaved progress is lost." with Yes and No. No is focused and last, so
Enter and Escape both keep playing; the game keeps running behind the
question. Every notice also answers a controller: the D-pad moves the focus,
Cross presses the focused button and Circle the last one (the other way round
with View > Japanese buttons), and while one is up the game gets no pad input,
getting it back once everything is released (`Menu_NoticePad`,
`ControlsRuntime_Hold`). Yes makes the same request as Debug > Jump to > Title Screen
(`TitleJump_Confirm` in `title_jump.c`), and both items are enabled and
dimmed together. A Yes given after the game reached the title by itself is
dropped. It does not boot the game again (logos and intro): that would mean
resetting the whole guest (RAM, VRAM, SPU, disc) under the running C code,
while the title is what a player restarting wants.

The credits are the exception: from them both items restart the program
(`Platform_RestartGame`, the language change's restart), and the game boots
again. Retail never leaves the credits but by a reset, and their
presentation takes over what only the boot before `Main_Init`'s `setjmp`
sets up. Its first step moves the music buffer into its own area
(`SD_SetMusicTrackBuffer(D_80010034)`, `model_intro_controller.c`) and only
its last step moves it back, so a jump during the credits left the sound
driver's VAB header (`D_8009B458` + 0x4A8, the buffer + 0x50) in memory
that the next screens load over. The first duel's music then read a program
table out of it: a player's Free Duel after Restart game in the credits
crashed in `func_8004ADE8` reading 0x80549544, and the minidump holds the
header pointer 0x80185D24, `D_80010034` (0x80185CD4) + 0x50. The
presentation also uploads over resident VRAM palettes, the Build Deck
trunk's among them (green instead of blue), which only the boot puts there.
A restart keeps the settings file and the saves; what is not saved is lost,
as with a reset, and the credits come after their save.

Debug > Jump to > Title Screen leaves whatever is running for the title, the
way the retail game leaves a campaign loss. `Main_RunGameOver` fades the
music and the screen out, asks for the title menu (`D_8009B268 = 1`,
`D_8009B26D = 0`, mode 8), and longjmps to the point `Main_Init` set up after
the boot sequence. From there it resets the frontend runtime, loads the
main-menu package and runs the title again. The platform's `title_jump.c`
dispatches that sequence through `src/pc/overrides/title_jump.c`, after
`DebugMenu_Exit`'s `DisplayObject_Reset` and `func_80035A64`, with the
disc left idle first. That way, no transfer the old screen asked for lands on
the title's package. While the save slot menu is open, the request waits
for it to close, so the menu is never left drawn over the title.

Debug > Jump to's other items (Debug Menu, Free Duel, Build Deck, Library,
Password, Map, Options, Credits) and the control channel's `jump` take the
same way out, then the title gives way at once to the game's own debug
menu, whose entry for the screen is taken as Cross would take it; a duel
(channel only: it needs an opponent and a deck) is armed as the Free Duel
screen arms one. See [Agent control](agent-control.md), step 6.

The request is only taken between two mode runners. A `MEMORIES_PC` hook in
`Main_Loop` polls after each frame. That is the one point where no runner is
half way through a step, and where no nested frame loop is on the stack
(a fade, a disc wait).
The item is off while the title's own loop (`Main_RunFrontendLoop`) runs,
before `Main_Loop` starts and after every return, including a normal game
over or debug-menu exit. It is also disabled during the jump's disc wait and
fade, so another click cannot reset the following game. At the title, a
request would otherwise wait for the next screen. Progress not saved is
lost, as with a reset.

Save states carry the item's enabled state and discard pending UI requests
on load. `pc_title_jump` tests title entry, save-menu deferral, repeated
requests during a jump, state restoration and Restart game's Yes and No.

`MEMORIES_TITLE_AT=N[,N...]` makes the request at presented frames N, for
checks. Requests scheduled while the item is disabled are consumed and
ignored at that frame. Checked headless: the jump works from a campaign duel,
and so do two jumps, each followed by New Game played to the duel again. It also works from
eleven screens reached with `MEMORIES_MODE_AT` from the options case: debug
menu, campaign, Library, map, Free Duel, Build Deck, name entry, Password,
Options, game over and Trade. After each one, the title and then the main
menu on Start come back pixel-identical. In the credits, a request made while
their save slot menu was open waited, and the jump (now the restart) came
once Cross had saved to a slot. The restart from the credits was checked
headless with the options-case input, `MEMORIES_MODE_AT=1000:15`, the save
declined, `MEMORIES_TITLE_AT=2000` in the credits scene and
`MEMORIES_RESTART_ENV` driving the new game: Load, Free Duel, Simon Muran,
the Build Deck trunk and the duel. Before, the trunk was green and the duel
crashed as the player's did (`func_8004ADE8` reading 0x80549544, with and
without the player's mods); after, the restarted game's frames at the trunk
and in the duel are byte-identical to a fresh launch's, and so are they
after the credits' own end. By mouse (`MEMORIES_SDL_SCRIPT`), the item jumps from a duel and
is disabled at the title. Restart game was checked the same way in a
campaign conversation reached with the `duel-hand-camera` smoke input: No
closed the question and the game went on, Yes brought the title back with
the item dimmed there. 2P Duel setup forced by `MEMORIES_MODE_AT` stops
presenting frames right after the switch, with or without this change.
Reached from the menu with no saves, it stays in the title's loop.

### Deck slots

Game > Deck slots (F6) keeps up to ten decks beside a save. Switching to one
takes one step, for example between a farming deck and the campaign deck.
The game has one deck: the forty ids at the start of the save state. Its
trunk counts only the cards outside that deck, and Build Deck moves a card
from one to the other. Using a kept deck therefore puts the current forty
back in the trunk and takes the kept forty out (`src/pc/saves/deck_slots.c`).
It is refused, with the reason, in three cases:

- a kept card is in neither the trunk nor the deck ("Missing 3 x Mystical
  Elf");
- a returned card would take the trunk past the 250 a byte of it holds. The
  game's own return to a full trunk drops the card;
- the slot is not forty cards this game has, at most three of each.

One slot is **active**: the one holding the save's forty cards. It is not
written anywhere; each time the decks are read, the active slot is the one
whose deck the save has (the one the screen last made active, if it still
does). A deck in no slot takes the first empty one.

The decks are a draft kept with the save. Using a deck, making one or
clearing one changes the draft, and so does leaving Build Deck: the active
slot takes the deck Build Deck wrote, when it is forty cards (the not-ready
way out can leave fewer, and then the slot stays as it was). The file is
written only when the game is saved through the save slot menu, and read
again when a save is loaded (`SaveMenu_SaveCount`, `SaveMenu_LoadCount`).
So the decks go with the save: a game left unsaved loses its deck changes
with the rest. A save state holds the draft (the `deck-slots` chunk); a
state from before it reads the file. A restored draft is written on the next
game save even if it was clean in the snapshot, since the file may have
changed afterwards. Loading a state also closes any old deck picker.
The screen shows "saved with the game"
while the draft differs from the file.

The screen (`deck_menu.c`) is drawn in the overlay like the save slot menu.
It is read from the pad, or the keys mapped to it: Cross uses a deck (on an
empty slot, it makes a new one, a copy of the current deck), Triangle clears
a slot other than the active one, Circle or Esc closes. While it is open,
the game gets no buttons, and it gets them back once the button that closed
the screen is released, so the game never takes that press as its own.

Build Deck opens with it. `Main_RunBuildDeckMenu` asks
`DeckMenu_BuildDeckEntry` before it copies the deck (`func_800323F8`):
while the list is up, the mode is not set up yet (0x40 clear), so Build Deck
has no copy yet. An incomplete deck bypasses the picker so it can be repaired. Cross picks the deck to edit, which becomes the active one,
and Build Deck is then set up from the save. Circle goes back where Build
Deck was entered from (`D_8009B269`), as its own way out does. As Build Deck
leaves, `DeckMenu_BuildDeckLeft` puts the deck it wrote in the active slot.

The chest a campaign duel opens first (`Main_RunDuel`'s step 0, the same
Build Deck screen) takes F6 the same way: it is left through step 4, whose
confirm there has no EXIT, so a short deck cannot reach the duel; then
`DeckMenu_DuelChestLeft` sends the duel back to step 0, where
`DeckMenu_DuelChestEntry` shows the list (Circle keeps the deck) before the
chest again. The duel's own way in is unchanged: the list shows only after
that F6.

In Build Deck, waiting on a pane, F6 (or the menu item) goes back to the
list: Build Deck is left the way Circle leaves it (step 4,
`func_800339D0`, which writes the deck back) and entered again to the list
instead of returning. With fewer than forty cards, that step asks first, as
the game does. Its BUILD DECK goes back to editing and drops the list. The
shop's EXIT leaves with the deck short, and then the game itself refuses the
campaign ("YOU MUST PREPARE A DECK..."). The list is not shown for a deck
short of forty: Build Deck opens straight on it to be finished, since no
deck can be used in its place. Nothing here makes a deck short of forty:
slots hold forty cards, and a deck is used only when it checks out.

The screen opens with a game loaded, on screens that keep no copy of the
deck:

- the main menu with Campaign. It runs in the title's own loop until the
  player first leaves it, so `Main_RunFrontendLoop` polls there too. The
  title's menu, or a save left in the workspace by a jump to the title, does
  not count: the entry byte `gMain_bMenuID` is 5 or more only on the loaded
  one;
- the campaign map;
- a card shop's menu (Save / Build Deck / Return to Title / Leave Shop)
  while it waits for a choice: the only way to Build Deck in the present,
  which has no map (`Script_OpSavePrompt`, scene-script command 13);
- Free Duel's opponent select;
- Build Deck, as it is entered, and from a pane (above).

Changes are made where `Main_Loop` or that loop is between two steps.
Elsewhere, the screen says where it opens. The setting `deck_slots`
(Game > Use deck slots) turns all of it off: Build Deck, the shop's menu and
the file are then the game's alone.

The card shop's own menu also offers it: DECK SLOTS under BUILD DECK.
`Script_OpSavePrompt` asks `DeckMenu_ShopMenu` as it makes the menu (a row
taller) and hands the choice to the screen; the rest of its code keeps
numbering its four entries (`DeckMenu_ShopChoice`). The menu's text,
string 0x11, is the retail listing with the line added, compiled as a
mod's text is (`Text_CompileOwn`) so its `{choose}` jumps land. The game
keeps four entries' worth of enabled bits (`Text_HandleChoiceCommand`), so
wherever it sets the menu's choices up (after its text, the memory card and
the title confirm) `DeckMenu_ShopRestore` moves the cursor and enables all
five entries. It does not reuse a nested prompt's enabled mask. The
`deck-shop` save-state chunk keeps whether the menu has the extra entry
and rebases pointers into its compiled text before restoring game memory,
so loading a shop state also works in a fresh process. With the setting off the menu is the game's. A
translation's string 0x11 gets the entry too, in the translation's words
(string `FE10`, [translations](translation.md#the-ports-own-strings)), when
its menu is four plain lines and the five fit the box's 44 letters (79 with
a PAL language on, whose text entries are the PAL game's 800,
[translations](translation.md#the-official-languages)); else it
keeps the translation's four. Checked on a save in the tournament's
shop: the setting off is pixel-identical to master (the menu, and the cursor
on LEAVE SHOP); DECK SLOTS opens the screen, and Circle brings the menu back
with the cursor on it; LEAVE SHOP has its normal highlight and leaves;
RETURN TO TITLE's No returns to it; SAVE cancelled, then LEAVE SHOP;
BUILD DECK opens.

The slots are kept in `decks/<duelist code>.txt` in the user folder, one
file per game, as text. Each line is `slot: forty cards`, a retail id or, for
a card a mod adds, its identity, because those ids change with the mods
applied. A line that does not read as forty cards leaves its slot empty, and
the log says so. Card names come from the game's own text lookup and glyph
table.

`MEMORIES_DECKS_AT=N[,N...]` opens it at frames, for checks. `tests/pc/deck_slots_test.c`
covers the rules and the file. These were checked by driving the pad on a
save loaded through the save slot menu:

- a deck kept in slot 1 and another used from slot 2. The saved game then
  holds slot 2's deck, and each of the 722 trunk counts is exactly what the
  swap should leave;
- the missing-card refusal with the card's name, and an invalid slot;
- the screen on Free Duel's opponent select;
- F6 and the menu item, which are off on the title's menu and with the
  setting off, and Esc closing the screen without quitting;
- Build Deck from the main menu: the list first, slot 2 picked, Build Deck
  set up with its deck; left, the decks file unchanged until SAVE;
- an empty slot picked (a copy), left, then SAVE and Overwrite: the file
  gains the slot; the same without SAVE: the file is unchanged;
- the active deck picked, a trunk card swapped for a deck card in Build
  Deck, left, SAVE: the active slot has the swap;
- a new slot left unsaved, F5, then a new session loading that state and
  saving: the file gains the slot (the draft came with the state);
- Build Deck from the card shop: Circle on the list goes back to the shop,
  a deck picked opens Build Deck, which returns to the shop;
- the setting off: Build Deck from the main menu and from the shop
  pixel-identical to master.

### Language (Game > Language)

Game > Language puts the game's own European translations in the US game:
English (US), the default, English (Europe), Français, Deutsch, Italiano and
Español. Their text ships with the port, one listing a language in
`languages/` (`en-eu.txt`, `fr.txt`, `de.txt`, `it.txt`, `es.txt`), which
the build copies beside the program and the release packs; no PAL disc is
needed. The packs are the port's own reading of the PAL discs, written by
`tools/pc/export_languages.py` (below); they are the one exception to
"nothing of the game in the repository", text only, as the team agreed.
The port reads `languages/<name>.txt` beside the program, else in the
current directory. When a pack is not there, a PAL disc is read instead:
any `.bin`, or the `.bin` a `.cue` names, in `game/languages` or
`game/pal` beside the US disc (also beside the program or in the current
directory), found by what it is, never by its name (SYSTEM.CNF's
SLES-03947 to 03951 and the language pack's three file ids in WA_MRG);
each language from its own country's disc first. `MEMORIES_LANGUAGES_DIR`
names one folder to look in for both packs and discs, instead of all
those. A language with neither is greyed in the menu and named in the log
(`MEMORIES_TRACE=menu`).

The choice is the `language` setting (0-5, `MEMORIES_LANGUAGE`), saved at
once and taken up at the next launch, as a translation mod is: the menu
offers Restart now, Later or Cancel. A mod's translation stands over it
string by string. What the text is and how it is read and spaced is in
notes/translation.md, "The official languages" (src/pc/text/language.c,
pal_text.c; the spacing hook in text_box_build_step.c).

**The packs.** `python3 tools/pc/export_languages.py --discs game/pal`
writes them again from the five PAL discs, with a built game (it runs the
game with `MEMORIES_EXPORT_LANGUAGES=<folder>`, which writes each
language's listing from the first source that has it and exits, so the
packs are byte for byte what the disc source gives). `--check` compares
instead of writing: `--discs game/pal --check` says the committed packs are
what the discs give, and `--discs languages --check` that the port reads
the packs back unchanged. They are UTF-8 with LF line ends
(`.gitattributes`); a carriage return an editor adds is dropped on reading.

Tested: CTest `pc_pal_text` (a made-up pack); both `--check`s; with only
the packs beside the program (`MEMORIES_LANGUAGES_DIR` on an empty folder
for the discs' side, and the default run, whose log names
`languages/es.txt`), Spanish, French and German in the game: the name
entry, Simon's talk and his choice, the duel's hand, the Library's card
view, RESULTS (whose pages stay English in phase 1) and the card drops'
list (`11 CARTAS MÁS`, `PÁGINA 1 DE 2`, `NUEVA`, the accented capitals of
the small letters); HD text (the Forbidden Memories HD mod's setting, at
Internal 2x) against it off, with Spanish in the Library's card view: the
smooth letters, `ú` among them. In a window: every language enabled with the packs,
all greyed with an empty `MEMORIES_LANGUAGES_DIR`; choosing Español and
Restart now started the game again in Spanish from the pack. The six
smoke cases are unchanged with English (US).

### Present pass (Video > Color)

The OpenGL presenter can draw the game picture through one fragment program
(`src/pc/render/present_pass.c`), under the menu and the HUD, which are not
filtered. The program runs where the picture's quad is drawn in `show()`
(`sdl.c`), both for the OpenGL picture and for the one the CPU uploads, so
it works at every internal resolution and in widescreen. The effects are
settings that are off at their defaults:

- Brightness, Contrast, Saturation and Gamma (percent, 100 leaves the
  picture alone). They are applied in that order in the shader: gamma,
  contrast about mid grey, brightness, then saturation against Rec. 601
  luma. The sliders are in Video > Color, with Reset.
- CRT scanlines (Video > Effects, `crt`). There is one scanline per line of
  the console's picture: 240, the source height over its nearest multiple
  of 240, so the lines match at every internal resolution. Each line is
  darkened towards its edges, and an aperture grille is drawn over window
  pixels, with the lost brightness given back.
- Reduce flashes (Video > Effects, `reduce_flashes`). The picture's average
  brightness may rise by at most 2.0 a second (black to white is 1.0). A
  frame that brightens faster is darkened as a whole to that rise, and
  darkening is never held back. Small moving things barely change the
  average, and the game's own fades are slower than the limit. The rate is
  timed in real time, so it holds at every speed. Without framebuffer
  objects, only this effect is unavailable.
- Sharp bilinear (Video > Filtering, `filter=2`; Nearest is 0, Smooth 1).
  Each texel is drawn as a flat block of whole window pixels. Only the
  window pixel a texel's edge falls inside blends the two neighbouring
  texels. So an uneven scale, such as 4:3 at 4.5x on 1080 lines, shows
  texels of equal width without bilinear's blur. At a whole scale every
  window pixel samples its texel's centre, so it is identical to Nearest.
  At a scale ending in .5 some edges fall exactly on a pixel's centre, and
  rounding decides whether that column blends or not. xBR takes precedence
  over it.
- xBR pixel smoothing (Video > Effects, `xbr`). The picture is read through
  xBR level 2 (written from its published rules, in the same program). A
  texel's corner is cut along a 45, 30 or 60 degree edge found in its
  neighbours and filled with the nearer neighbour's color. The cut is
  antialiased over one window pixel, so it works at any window size.
  - At internal resolution 1x it works on the finished picture's texels,
    and replaces Smooth and Sharp bilinear filtering while it is on.
  - At 2x and up (the OpenGL picture) it works on the textures instead, in
    `gl_picture.c`: each textured primitive's texels go through the same
    rules as it is drawn, so sprites, fonts and 3D textures are smoothed
    at the internal resolution, over whatever lies under them, and the
    picture is not smoothed again (Filtering still applies to it).
    Transparent texels count as one color
    far from all others, so a sprite's outline rounds too, and where a
    corner is filled with transparency nothing is drawn. A primitive only
    sees its own rectangle of texture (the edge repeats past it), so a
    picture made of several rectangles shows no seams. HD text and texture
    pack images are left as they are. A texel like the four beside it is
    drawn after five reads, so at 4x the duel's replay time stays within
    its noise (about 2 to 3.5 ms here, on or off).

While every effect is at its default, the pass is not used, and the picture
is drawn by the fixed-function quad exactly as before. The SDL_Render
fallback (no OpenGL) has no pass.

### Speed, frame rate and vsync

Three independent controls (`platform.h`, `platform_common.c`):

- **Game speed** (`speed`, `MEMORIES_SPEED`, Game menu, 25-400 or -1) scales
  the game clock: VBlanks, disc timing and SFX run that much faster, so 200%
  is 119.88 game frames a second. Music sequencing runs on real time and
  keeps its tempo. Tab holds 400% while pressed; P pauses; `.` steps a frame.
  Uncapped (-1) fires a VBlank whenever the game waits for one.
- **Frame rate** (`fps`, `MEMORIES_FPS`, Game menu) is how many of those game frames
  reach the window: 0 (default) follows the display's refresh rate, -1 shows
  every game frame, or a number. Presentation is paced on a fixed grid apart
  from the game clock, so a cap of 60 at 400% shows every fourth frame and
  the game never waits for the window. Frames that are not shown still poll
  input and the menu.
- **VSync** (`vsync`) blocks the present on the display. That may only pace
  the game while game frames come no faster than the display refreshes;
  above that (200% on a 60 Hz display, or uncapped) the backend presents
  unsynced and the frame-rate cap alone limits presents. At 100% on a 60 Hz
  display the swap paces the game: the game's VBlank comes when a swap that
  waited for the display returns, and the next a refresh later (less a 64th,
  so a driver that queues swaps fills its queue and starts to wait), and a
  cap at the refresh or above drops nothing. It used to come a refresh less
  1.5 ms after every swap, which added whatever the game and the present
  took past 1.5 ms to every frame: 45 fps with widescreen, the CRT pass and
  a model mod on an older PC.

`VSync(0)` presents, then waits for the next VBlank after entry. It must
not return at once because a VBlank passed since the previous call:
`Graphics_SyncFrame` resets the game's own VBlank counter to -1 just before
calling, and `Input_UpdatePads` takes a counter still at -1 as a lag frame
and publishes every press a second time on the next frame. A catch-up
variant was tried and doubled inputs at 300%. A frame that overruns its
VBlank therefore costs a whole slot, as on the console; 200% and 400% hold
their rates because presents are cheap on the accelerated path (below).

The counter must equal exactly 0 at `Input_UpdatePads`, and a value above 0
doubles presses as well (retail defers that frame's pressed and repeat bits
and publishes them again on the next frame: one tap of Down moved the title
menu's cursor two places, with keyboard, gamepad or wheel alike). On the
console exactly one VBlank callback runs between that reset and the return
of `VSync(0)`: VBlanks a slow frame runs past interrupt it while it computes
and draws, so `Graphics_SyncFrame` counts them (`D_8009B0D8 = 2`) before the
reset. The cooperative clock used to take them only at `VSync(0)`'s entry,
after the reset, and several at once after a present or a host hiccup longer
than two VBlank periods. Two service points restore the console's order: `DrawSync` runs
what the clock owes (`Platform_ServiceClock`), which `Graphics_SyncFrame`
calls just before its read, and `VSync(0)` holds the clock to one VBlank
until it returns (`Platform_LimitVBlanks`); the rest wait for the next
service point. Neither steps time, so deterministic runs are unchanged. When
the host cannot keep up, a frame now shows the overrun the console's way
(the next frame advances two VBlanks' worth, `D_8009B0D8 = 2`) instead of
catching up with extra frames; 400% on a loaded machine keeps 240 VBlanks a
second at fewer game frames.

The HUD (F3) shows game frames a second and shown frames a second; with
`MEMORIES_TRACE=frames` the same appears every 120 frames with the clock
rate, VBlank rate and sequencer rate.

**Software OpenGL on Wayland.** The 32-bit build on an NVIDIA Wayland
desktop gets Mesa's `llvmpipe` through EGL (there is no 32-bit NVIDIA EGL
Wayland path), and a software renderer takes 6 ms and more to present a
frame, which alone breaks 200%. `Platform_Open` therefore retries with the
`x11` (XWayland) driver when the GL renderer is software, where the NVIDIA
driver presents in about 0.2 ms. `SDL_VIDEODRIVER` pins a driver and skips
the retry; `MEMORIES_TRACE=window` logs the renderer, the refresh rate and
every vsync change. XWayland reports no refresh rate, so the rate the
Wayland driver reported before the retry is kept. The pointer over an XWayland
window is Xlib's core font cursor (tiny, unthemed) unless the 32-bit Xcursor
library is installed: Xlib loads `libXcursor.so.1` itself to substitute the
desktop theme at the size KDE publishes for Xwayland (`xrdb -query`:
`Xcursor.size`). On Arch that is `lib32-libxcursor`; without it there is no
fix from inside the game short of drawing its own pointer.

### Cooperative clock

The cooperative clock is the default (since 2026-09-24, after a 29-minute
session played on Windows with it and no problem). It runs the game with no
interrupt at all. `MEMORIES_CLOCK=interrupt` brings back the old clock, a
timer that interrupts the game as the console's VBlank would. The sampling
profiler takes its samples from that timer, so `MEMORIES_PROFILE` without
`MEMORIES_CLOCK` chooses it too, and the log says so. The console's interrupt
work (the VBlank callback, the sequencer's root counter, disc delivery,
MDEC, memory card) is run on the main thread where the game calls in to
wait or to read the time: every `VSync`, `Platform_WaitVBlank` (which wakes
every 0.5 ms and runs what is due) and `Platform_PollTime`. The game's
interrupt code therefore never runs in the middle of anything, which is what
the SIGALRM holds and, on Windows, the suspend-and-redirect clock thread and
its repairs exist for; in this mode that thread only watches for stalls.
It is the model the deterministic runs (the smoke fixtures) have always used,
with real time instead of synthetic steps.

Why it holds: every loop in which the game waits for something its interrupt
code sets goes through one of those calls (`Main_AdvanceFrame`, the VBlank
waits; `File_WaitForTransfers` calls `Main_AdvanceFrame` each turn). In a
deterministic run the fallback timer reports any second the game spins
without a wait, with the address (`clock: the game spun a second without a
wait`; checked to fire with `MEMORIES_CRASH_TEST=spin`); it never fired
through boot, the movie, title, menu, options, the story, both duel cases,
and the debug menu, Library, campaign map, Free Duel, Build Deck, name
entry, password, game over, trade and credits entered with
`MEMORIES_MODE_AT`.

Measured on Windows at 100% (`MEMORIES_TRACE=frames` reports the
sequencer's lateness against when each tick was due, every 1000 ticks, and
each stretch of more than 20 ms between two `VSync` calls):

| run | clock | sequencer lateness, mean / worst | ticks later than 5 ms | clock repairs |
|---|---|---|---|---|
| 3D duel case, first | interrupt | 818 us / 135.6 ms | 18 | 293 deferred ticks |
| 3D duel case, first | cooperative | 75 us / 4.1 ms | 0 | none |
| 3D duel case, second | interrupt | 724 us / 4.5 ms | 0 | 288 deferred ticks |
| 3D duel case, second | cooperative | 74 us / 4.5 ms | 0 | none |
| options case | interrupt | 740 us / 1.7 ms | 0 | 10 deferred ticks |
| options case | cooperative | 27 us / 4.0 ms | 0 | none |

VBlanks ran at 59.9-60.0 a second and the sequencer at 73.4-74.0 in both.
The options case ends on the same frame, pixel for pixel, with either clock;
the duel cases do not, with either clock, from one real-time run to the
next: the deck is shuffled from time-dependent state, so the scripted
presses meet a different hand (checked on the hand camera case: three
clocks, three hands).

Not yet: Linux has only been compiled with it (its stall reports come from
the crash monitor process, since no tick runs); the sampling profiler
(`MEMORIES_PROFILE`) takes its samples from the interrupt and gets none;
save states made in one mode have not been loaded in the other. Removing the
interrupt clock is a later step, once this has been played on both systems.

### Mods > Hand camera

On by default (`mod.hand-camera=0` in `settings.txt` in the user directory
turns it off; older files' `hand_camera=0` is still read). While
the hand is up (duel scene state 4, the human's hand actions), where the
console ignores the shoulder buttons, L1 and R1 turn the camera around the
mat and L3 and R3 (T and Y, or the stick clicks) zoom it in and out; L2 and
R2 stay the game's own top-down look at the opponent's field: `mods/hand-camera/hand_camera.c` moves the
view state's own heading and distance (`D_800F2848.angle`, 20 units of a
0x1000 turn a frame; `field_00`, 6 units a frame between 200 and 1400, the
duel's own view being 600 away) and re-applies `ViewState_ApplyOrbit`, which
the game itself only does while one of its own camera tweens runs (the first
version moved the heading alone and only the 3D Monsters turned); the mat,
the card sprites and the monsters all follow. The view stays where it is put:
nothing eases it back on release and nothing resets it when the hand closes,
because every camera move the game makes from there (placing a card, the turn
switch, an attack) is a tween from the current view to an absolute target,
so the game's own moves carry the camera back smoothly. Checked from
`slot1.state` (2026-09-21) with `MEMORIES_INPUT="60:0400,150:0000,210:0002,
270:0000,280:0004,320:0000"`: turned at 120, still turned at 200, zoomed in
at 260, out again at 330.

### Mods > 3D Monsters

A window of extras the console could not run, one directory each under
`mods/` (`notes/modding.md`), off by default. The first is **3D Monsters**: every face-up monster on the duel
field stands on its card as the model the battle presentation uses, animating
on the spot, the near side turned to face the opponent. Ticking it in
**Game > Mods** takes effect on the next frame and is kept in `settings.txt`
in the user directory as `mod.3d-monsters` (`MEMORIES_MOD_3D_MONSTERS=1` sets
it for one run).

What it costs the console, and why the port does not pay it: a monster's
`MODEL.MRG` record is 276 sectors -- 96 of them model data, a quarter of the
machine's RAM -- and its textures are a 256x256 block of VRAM, and the duel is
already using both. So each monster here gets

- a private 256 KiB arena, mapped at `0x90000000` upwards, outside guest RAM
  and still at a negative address, because model code tells a pointer from a
  small number by its sign;
- a private texture bank in the software GPU: VRAM-shaped memory that a
  primitive selects through bits 11-14 of its texture-page word, which the
  hardware ignores and retail always leaves zero (`SoftGpu_Bank`,
  `src/pc/render/soft_gpu.h`). Nothing else in VRAM moves, and a bank holds
  the palettes at their own coordinates too, so the model's own primitives
  need no other change.

Loading is synchronous and beside the game's streaming
(`Memories_DiscReadSectors` reads the whole record in one call, about 1 ms),
so a monster appears without the duel's own transfers noticing. The record's
seventeen phases are replayed exactly as `func_80056D7C` programs them, into
the arena instead of the two fixed duel arenas; the phases that upload to VRAM
upload for real, because the setup's palette phase reads them back through the
GPU, and the duel's pixels are put back afterwards. `func_80056828` then runs
the game's own eleven setup phases. The slot is flagged `0x80`, which is what
the game itself uses for a quiet load: no sequence bank, no voice block, and
the three control-module command words left at -1, so no per-monster module is
ever called.

Drawing is `func_800540B4` and `func_800556E8`, the pair the Library's card
model view drives, with the slot placed by `func_8005A4C4` at the card's own
field coordinates (`D_800908A0`). It is sorted into the game's own model
ordering table before the frame is sent, so the game's layering applies:
monsters stand over the field and under the hand and the interface. Two
measurements make them stand right:

- **where the body is.** A duel model is built around the point between the
  two duellists, so its parts sit some 250 units up the field from the origin
  `func_8005A4C4` places and about 200 above it. Walking the parts' world
  matrices once gives the offset, and the feet land on the card.
- **how big it is.** A model's parts say where its joints are, not how far its
  skin reaches -- Korogashi is one big ball around a single joint -- so the
  monster is sorted into a scratch table that is never drawn and its packets
  are read for the height they cover. Drawing it at the square root of that
  against a middling monster keeps the order, a dragon still towering over
  Sangan, while bringing a sevenfold range down to about two and a half.

Which way a monster faces is fixed to the side that owns its zone: the
player's monsters face up the mat, the opponent's face down it, as the
battle presentation stands its two slots. It is not fixed to whose turn it
is, which was the first version's mistake: the turn switch
(`DuelScene_UpdateTurnSwitch`) swings the camera a half turn round the mat
over 48 frames and flips `D_8009B1D5`, the acting side, a third of the way
through, so choosing by that snapped every monster round mid-swing and left
the player's showing their backs for the opponent's turn. The monsters ride
round with their cards because they are placed in field coordinates and the
pass draws through the duel's own `GsRVIEW2`. Each also floats `LIFT_PIXELS`
(8) above its card, so the card shows beneath it.

Two more things the field taught it: the cards sort at a sixteenth of their
distance and a model's primitives at a quarter of theirs, so a monster is
sorted at its own scale into a table of its own (the other frame's packet
area), and that run of packets goes into the game's table whole, at a
quarter of its nearest entry and three entries nearer, which draws it on the
card rather than under it. The first version quartered the GTE's Z factors
instead, which put four of a model's depths in one entry; parts sharing an
entry draw in the order they were sorted, so a skirt's inside or a limb
showed through the body and flickered as the animation moved it, which the
internal resolution made plain; and the duel flies
its camera down to eye level for the zone picker, using a card and the
guardian-star presentation, where a monster standing on a card has nothing to
stand on, so the pass runs only while the camera is above the mat (pitch below
512 of a 4096-unit turn).

Switches: `MEMORIES_MOD_3D_MONSTERS=0/1`, and the mod's settings (the
`mod.3d-monsters.<key>` lines in the settings file, or
`MEMORIES_MOD_3D_MONSTERS_<KEY>` for one run): `SCALE` (4096 = as
measured), `PIXELS`, `DEPTH`, `PITCH`, `LIFT` (field units above the card, 2
per pixel from the duel's view), `BATTLE`, `BATTLE_PIXELS` and `BATTLE_DIM`
(the attack cards, above), and `TEST=<card id>`, which stands a
different monster in all ten zones; that is how the cache, the arenas and the
banks were measured together. `MEMORIES_TRACE=mods` logs it.

Checked from the duel in `tmp/pc/states/slot1.state`: the monster loads in
about 1 ms and animates, ten of them at once keep the frame rate, the duel's
own graphics are unchanged with the mod on or off, the menu item takes effect
live and persists, and picking up a card, choosing a zone, the guardian star
and the return to the field all behave as they did. The turn switch was
watched frame by frame with `tmp/pc/mods/turn/cap.sh` (plays the first hand
card from slot 1, ends the turn with Start, dumps the frames it is given and
tiles them): the monster keeps its facing through the swing and ends facing
the camera on the opponent's turn.

**Fading a model.** The console's semi-transparency cannot fade a
textured model: it only blends texels whose own semi-transparency bit is set,
and only by fixed amounts. So both renderers take one more unused field, for
a polygon that samples a texture bank (which retail never does): the upper
half of its third texture-coordinate word, `SOFT_GPU_FADE | amount`
(`soft_gpu.h`), mixes the polygon with what is under it by `amount`/255, in
the software GPU's two plotters and in the OpenGL picture's shader through the
same blend function its semi-transparency uses. The mod writes that half on
every polygon it stamps with a bank, zero when it does not fade, so nothing
a model's own code leaves there can fade it. An older game ignores the field
and the monsters simply vanish at the end.

**On the attack cards.** When one monster attacks another without going to
the arena, the battle presentation (`DuelScene_UpdateBattle`) lays the two
cards side by side, big, over the faded field. With the `battle` setting on
(off by default) each monster stands on its card there too, the attacker on
the left turned right and the defender turned left, both a little towards
the camera, and the cards are drawn darker under them. The pass projects
through a camera of its own, looking straight at the cards, and fits each
monster to its card from its packets: into a box 150 pixels wide and
`battle_pixels` (160) high, whichever it meets first, so a dragon's wings
count as much as its height. The box grows and shrinks with the monster's
size on the field (the square root of it against a middling monster, from
70% to 160%), so Blue-Eyes stands taller than Mystical Elf, and no monster
is taller than the space between the card's foot and the top of the screen.
The first version capped every monster at the middling size, which drew
Blue-Eyes no bigger than an elf. It is placed by its outline, middle over
the card's middle and lowest point on a line across the card's print, not by its body,
which put winged and armed monsters off to one side.
`MEMORIES_MOD_3D_MONSTERS_BATTLE_TEST=<card>` (undeclared, like `test`)
puts that card and the next on the two cards, for measuring. Three things it has to
respect:

- the big cards are sprites in ordering table 1, the interface's, and once
  the field has faded out the model table is not drawn at all
  (`Fade_StartOutKeepOverlayAndHideSecondaryTables`), so each monster goes
  into table 1, two entries nearer than its card: over the picture and the
  print, under the hit flash, the glow and the damage numbers;
- the battle sets its projection up once and draws those numbers and glows
  through it for the rest of the presentation, so the pass puts the GTE
  registers and the world-screen matrices back as it found them (without
  that the numbers were never seen);
- the dimming is the card's own color word, which the presentation's last
  step turns down to fade the cards out: everything printed on the card goes
  darker with it (`battle_dim`, 50%), the fade starts from there, and the
  color goes back to retail's whenever the pass lets go of a card that is
  still up.

Each stands 8 pixels back from the middle of its card, away from the
other. The monsters appear once the cards have faded in, the loser's goes
as its card starts to burn (the card stays dimmed until the game releases
it, or it lit up for the frames before the flames covered it), when the attack is over the monsters fade out over 16 frames while
the surviving cards light up to their full color again, and only then does
the game's own fade take the cards away (the pass sets each card's color to
what the step will take 8 off the next frame, which holds the cards up that
long), and none appear when the attack goes on to the 3D arena
(the cards are up only a few frames before the fade there). Checked with the
opponent's first attack and the player's quick and arena attacks after the
`duel-3d-monsters` smoke input, frame by frame against the same frames with
`MEMORIES_MOD_3D_MONSTERS_BATTLE=0`.

**The attack.** The setting `attack` is a choice: off (the default), on the
attack cards (the 1 the setting stored when it was a bool on or off), on the
field, or both (one bit each). On the attack cards, with `battle` on, the
two monsters fight on the big cards instead of standing there. The
fight uses the rows of animation the 3D arena plays, which every model
carries: the attack row is `field_DFE + 3` (row 3 from attack position), and
the reactions are 5 (withstands the blow), 6 (hit) and 8 (guards). The arena's
controller (`func_8004EB00`, mode 15) starts the defender's reaction from the
attacker's control module. For a monster with no module, which is how the mod
loads every one, `func_800559D4` starts it at the attack row's midpoint, and
so does the mod. When the battle reaches step 7, before any damage number, the
mod starts the attack and holds the battle there. A hook on
`DuelScene_UpdateBattle` skips its update until the last blow lands (and at
most 1,200 frames). Then the game shows its flash, numbers and flames as
ever, over the reactions. The case comes from the exchange step 3 already
resolved (`D_8009B1B0[side]`, -1 for a card destroyed and 1 for one hit
that stays):

- the defender is destroyed: it is hit (6);
- its defence holds: it guards (8);
- the attacker is the weaker: the defender withstands the blow (5), strikes
  back with its own attack row as soon as that row ends (the arena waits for
  the attacker's whole row), and the attacker is hit;
- a tie: both are hit;
- no defender: the attack alone.

A trap that negates the attack skips step 7, and nothing is played. A monster
that is hit holds the row's last frame until its card burns. Any other row
ends back at rest (`Model_ControlSlotAnimation(0, 0, 0)`, then
`func_800597C8(0, 1, 0)`).

The arena's rows carry a monster across a wide floor, and a small monster
stands on its card several times enlarged (Shadow Specter at 5.7 times), so
the first version threw the defender off the screen with its recoil. Every
frame the mod takes the mean world translation of the model's parts against
the same mean at rest. It keeps 35% of the shift (`attack_drift`, not
declared) and no more than half a card's width of it, and takes the rest off
the placement. The attacker still lunges at the other card (Man-eating Plant's
head reaches the skull), and the one hit recoils by its own.

The two monsters are cache entries of their own while `attack` is on (tag
`side + 1`), so the rows never reach the monsters on the field, and they are
dropped when the presentation ends. `attack_speed` (default 200%) scales the
slot's speed byte while a row plays. The arena plays rows at 8 units a VBlank,
the same as here, so 100% is the arena's own pace: its blows land 1.3 to 4.5
seconds in, and a counter-attack up to 10. The particles and sounds of the
arena's attacks are not there: they come from each monster's MIPS control
module and its sound bank, which the mod does not load.
`MEMORIES_MOD_3D_MONSTERS_ATTACK_TEST=1..4` (not declared) forces the
destroyed, guarded, counter or tie case for any exchange, which only changes
what is shown. Checked frame by frame on the quick battle of the
`animated_battle_inputs` timeline with its last Square turned into Cross
(Man-eating Plant against Shadow Specter in defence, held 76 frames at 200%),
on every case with `BATTLE_TEST` pairs, and against the previous build:
`attack` off, and the arena attack, give the same frames. The smoke case
`duel-3d-monsters-attack` is that battle at the bite (frame 20164).

**The attack on the field.** With `attack` on the field (or both), the two
fight on the field first, before the big cards come up, and need no `battle`.
The quick battle lifts both field cards in its first call: their records
lose `DUEL_CARD_FLAG_OCCUPIED`, and each card's display object becomes
`D_800E9EF0[side]`, its `field_6A` the record. Through step 1 the cards fly
up to the screen while the game's own camera comes back from overhead (pitch
1022) to the view the duel is played from (600 away, pitch 256), with the
panels gone. Step 1 ends by setting `D_8009B174` to 2 (frame 20045 of the
timeline), and the next call makes the big cards. The same hook holds the
battle at that call (`D_8009B174 == 2` exactly), so nothing of the game's
camera or interface has to be touched, while:

- the cards in the air are hidden (`DISPLAY_OBJECT_FLAG_RENDERABLE`, which
  step 2 clears itself a little later), and both monsters grow on their own
  zones (`D_800908A0`) over 12 frames, turned to face each other, a face-down
  one shown;
- the duel camera comes in over 24 frames, to 60% of its distance
  (`fight_zoom`), aimed between the attacker, wherever it is, and the
  defender, 48 units over the mat, and swung 60% of the way to seeing the two
  side on (`fight_turn`; the camera looks along `(cos angle, sin angle)` on the
  mat in `ViewState_ApplyOrbit`). The game re-applies the orbit only while one
  of its own camera moves runs, and the last one ended with step 1. The mat is
  drawn before the mod's pass, so each new camera is set for the next frame;
- the attacker glides over 20 frames from its zone to where it strikes from.
  The arena stands both models on one point facing each other, each body its
  own way from it (the 1:1 body offset `raw_z`), and an attack row is made to
  reach across that gap, so the attacker stops as far from the defender as the
  two bodies would be in the arena at its own scale (`fight_reach` percent of
  it; Man-eating Plant against Shadow Specter: 208 units, so it crosses 75
  of the 283 between their zones). While a monster plays an attack row all of its body's shift shows,
  up to that gap; any other row keeps 35% of it, at most 35 units;
- the blows are the attack cards', from the outcome worked out ahead: it is
  only stored at step 3, but it is `func_8001EFD4`'s (above 0 destroyed, 0
  guarded, -1 a tie, below that a counter, or guarded when the defender is in
  defence), which only reads;
- once the last blow has landed and its row has ended, the attacker glides
  home unless it was destroyed, and the camera goes back the same way; the
  battle is let go when both monsters' rows are over (back at rest, or a
  destroyed one stopped on its hit row).

The hit row stops itself a step short of its end (`func_800556E8` calls
`func_80059700(index, 0)` for row 6, which sets `field_E16` to 0x23), so a
row that has stopped counts as over. The two are cache entries of their own
(tag `side + 3`), dropped when the big cards' pass takes over or the battle
ends; anything that lets go early (the 1,200-call limit, a setting changed,
the mod turned off) puts the camera back, and a loaded state keeps its own.
Both on the field and on the cards plays the fight twice. Checked frame by
frame on the same timeline for every case with `attack_test`: the fight takes
270 frames there (355 for the counter). The smoke case
`duel-3d-monsters-attack-field` is the bite on the field (frame 20155).

**On the field alone.** With `attack` on the field and not both, the big
cards never come up: the fight ends the battle itself.

- Each blow shows what the card it struck would have shown, as it lands.
  Step 3's `D_8009B1B0` and `D_8009B1A4` are worked out ahead from the same
  `func_8001EFD4` result. A side left untouched shows nothing; any other gets
  step 8's damage effect (request id 2: `field_12` the number, `field_1A`
  its size by thousands, at most 2) and its sound (0x10 plus the size, 0xD
  plus it on the attacker's side). One struck for nothing gets the flash
  alone, as the effect itself does with 0.
- A direct attack's number is step 9's in its plain form: size 0-2, not the
  burst of 3-5.
- The effect adds its colour to whatever is under it, so over a pale monster
  the digits all but vanished. Each number goes above the head of the monster
  it is for, against the dark behind the fight; a direct attack's goes over
  the middle of the screen, above the attacker. The head is the top of the
  outline its packets covered when it was last drawn (`packet_height`), and
  the number is kept whole on the 320-pixel screen.
- A destroyed monster goes over 16 frames once its hit row has stopped: the
  summon run backwards, with the burning card's sound (0x1B). Until it has
  gone, the camera stays in and the attacker stays where it struck. While the
  camera goes back, both turn to the way their zones face.
- The battle is let go once every number has run (its request no longer
  `DUEL_EFFECT_REQUEST_FLAG_ACTIVE`) and every destroyed monster has gone.
  At the next call, still at step 2, the hook ends it:
  - Step 3 runs as the game's own, with `D_8009B174 = 3 | 0x80` so its
    fade-out counts as begun and the field stays up. It takes the life points
    (`Mods_DamageLife`) and counts the ranks. Like step 3, this waits out any
    screen fade (`D_800E9ECE[0]` bit 7), or no life points would be taken.
  - What step 11 does first follows: each card that was not destroyed goes
    back on its zone (`func_80024D34`, the saved 0xA00 flags, 0x4000 on the
    attacker, the saved modifiers, `Duel_ApplyCardObjectFlags`), and its
    summon is marked done, so its monster stands at once where the fighter
    stood.
  - The two lifted cards are released, the panels slide back to 0xC and
    0x118 (`func_8001ED20`) and count to the new life points, and the scene
    goes to state 5.
  - A destroyed card's record stays empty, as after its card burns.

Checked on the timeline with each real outcome, forced by a test mod at step
1 that changes the cards' modifiers and positions. Destroyed with 2300
damage, counter (-1700 to the player), tie, guarded with 1400 to the player,
guarded for nothing and a direct attack for 800 all end on the field with
the life points step 3 takes. The smoke case
`duel-3d-monsters-attack-field-end` is the turn after the battle (frame
20400): the attacker back on its zone, face up.

**The attacker's effects.** With `effects` on (the default), the field fight
plays the attacker's own control module, the per-monster MIPS overlay the
arena runs for its beams, flashes and particles. The defender's module
(slot B) is not loaded: its code is linked for 0x8017A000, where the duel
keeps its equip and fusion tables.

- **Loading the module.** Each cached monster keeps its record's three
  command words (sector 275, +0x110). These are the two stances' variant
  commands and the primary's; the quiet load leaves the slot's own copies at
  -1. Its stance's variant and its primary are already in its arena. When
  its stance's command is not negative, the fight:
  - copies the primary to 0x8013A000 and the variant to 0x8013B000, keeping
    what was there and putting it back when the fight is over (a loaded state
    keeps its own);
  - clears the attacker's contexts in its arena;
  - puts the arena's effect sheet into the attacker's soft-GPU bank: SU.MRG
    stage 0 record (sector 0x88), palette sector 83 to (0x200,0xF4), 32
    sectors of texels from (0x380,0). In VRAM the duel's card thumbnails sit
    there.
- **Running it.** The module runs through `func_800559D4(0)` inside
  `sort_monster`'s scratch window, right after the attacker's model, so its
  packets are chained and stamped with the attacker's bank. For the call it
  is told the arena's world:
  - **Time.** A module adds `Model_GetFrameStep` (`D_8009AFA3`, the VBlanks
    the frame took) on each frame its animation index moves. That is 2 in
    the 30 fps arena and 1 on the 60 fps field, where its clock ran at half
    speed. Blue-Eyes, which charges from 80 to 316, fires at 324-360 and
    fades at 500-520 (its command-0 table at module +0x38A4), never fired
    within its 270-frame attack row. It is handed 2.
  - **Space.** A module works at the arena's scale, around slot 0's arena
    pose. It aims at the other slot's body centre (`field_DD0`, through
    `Model_CopySlotU16Values`) and draws camera-facing billboards with
    view-space sizes (`GsGetLs`, then `ReadRotMatrix`, `RotMatrix`,
    `ScaleMatrix`, `SetRotMatrix`). So for the call:
    - the attacker stands at the arena pose (`func_8005A4C4(slot, 0, 0, 0,
      0)`, scale 1);
    - both body centres go through the inverse of its field root matrix T
      (`field_D18`: rotation times its scale s, then its position);
    - `GsWSMATRIX` (`D_800FE148`, which the port's `GsGetLs` reads too) is
      WS T scaled by 1/s. That keeps the field's screen positions at the
      arena's depth, so a billboard comes out s times its arena size, as the
      monster does. With WS T alone the billboards came out at arena size,
      seven times too big for a monster drawn at 552/4096.
  - **Camera.** Its camera requests are refused (`D_8009B07B`/`D_8009B07C`).
  - **Reactions.** The reactions it would start in slot 1 go with the copy
    of the defender put there for the call. For a direct attack, the copy's
    body centre is the empty zone across.
- **Timing.** The module's answer times the fight:
  - its first 4, 3 or 1 lands the attacker's blow (or the end of the attack
    row, if it never says);
  - until it answers 2, or has run 900 frames, the attacker stays where it
    struck, the camera stays in and the battle is held.
  - Blue-Eyes lands 165 frames into the module and is over at 336. Man-eating
    Plant lands at 74 and is over at 118.
- **The counter.** A defender that strikes back does it as before.

Checked on the timeline with 60 attackers (`battle_test` 1, 75, and every
twelfth card from 3 to 699). Each of the 52 monsters has a module; each
lands its blow 21 to 266 frames in and is over by frame 397, and every
battle ends. The 8 cards that are not monsters fight no fight. The smoke
case `duel-3d-monsters-attack-effects` is Blue-Eyes' White Lightning
mid-beam (frame 20320). The two field fight cases set `effects` off, so
their frames are as before.

### Images from the disc

`python tools/pc/extract_images.py [family ...]` writes the game's images
from `game/DATA/*.MRG` as PNG under `tmp/pc/images/`, named by where they
come from, with `manifest.json` giving each one's provenance (archive, byte
offset, size in VRAM words and rows, depth, palette offset): the identity a
texture pack goes by, with names as aliases on top. The archives hold no
image files, so the tool replays the game's own loaders, one family at a
time; so far `cards`, the 722 cards' artwork from `func_800289BC` (WA
sector `(n-1)*7 + 722`, seven sectors: the 102x96 8-bit picture, its
256-entry palette, the name strip under it and the strip beside it), 2,166
files, and `portraits`, the 48x48 dialogue portraits through their 64-entry
palettes (the campaign's 25 and Free Duel's 40, `0x980`-byte records).
`--names cards.tsv` (card_number, name) puts the card's name in the file
name. `sheets` replays every screen loader that streams its images through
the GPU path (`file_transfer_runtime.c`: a sector is a 64x16-word tile,
sixteen stack into a 64-word column of 256 rows, contiguous in the archive,
the next column 64 words to the right): the main menu (`SU.MRG`), the boot
UI and the title, the story dialogue UI, the campaign, Free Duel, name
entry, password, options, game over, the duel results and rewards, the
Library, the seven duel terrains, the campaign map's strip and the 65
display-effect records of the duel. A sheet is one PNG per column and per
way the game reads it, depth and palette (`notes/mrg-files.md` for the
loaders; the readings come from the draw code and from dumps, `--variants
<assets.txt>` adds a dump's). `scenes` is the story's pictures
(`ScriptImage_RequestTransfer`: 33-, 81- and 113-sector records from WA
sector `0x21D5`, the card shop among them). Records laid out alike (the
terrains, a mode's story pictures) share the readings a dump shows for one
of them, and entries whose pixels come out identical (the duel-hand block
in all seven terrains and the Library) share one file. `--assets
<assets.txt>` extracts whatever a texture-dump run drew (below), under
`assets/`, named by archive, offset, size, depth and palette, skipping
what a sheet covers: the way to cover what no family describes yet. Every
dump so far (title, main menu, options, the story, both duel cases) comes
out pixel-identical to the sheets' crops, and a pack of the sheets as they
are draws the same frame as no pack at 1x, 2x, in software and in GL. A
mod with `"textures"` in its manifest replaces the images at draw time
from such a directory (`notes/modding.md`, "Texture packs"). Not yet: the
monster textures (`MODEL.MRG`). The campaign map's own pictures (the
terrain model's textures, uploaded from its 134-sector block) have no
extractor family and a dump's `assets.txt` leaves them out: their palettes
reach VRAM from memory with the semi-transparency bit set on every entry but
the first, and an asset wants every entry traced. A pack replaces them all
the same, since its palette rule keys on the first entry; the FM Editor's
Map tab writes such entries (`tools/pc/fm_editor/map_art.py`).

### Texture dump (what is on screen)

`MEMORIES_DUMP_TEXTURES=<directory>` writes every texture the software GPU
draws as a PNG named by its hash (`src/pc/render/texture_dump.c`), a
discovery tool: what a screen is made of, at what size and depth, through
which palette. A texture is the rectangle of texels one textured primitive
covers, decoded through its palette, so a sprite comes out at its own size
and colors and the same one drawn again is the same file; the hash covers
the texel indices and the palette entries, so a palette swap is another
image. `textures.txt` in the directory lists each hash with its size, depth,
page and palette coordinates. Dumping costs a hash per primitive, so it is
for a capture session, not play; a duel dumps about 1,900 images. The hash
is not what a texture pack goes by: images are named by where they come
from on the disc (the archives stream raw VRAM blocks, `notes/mrg-files.md`;
the loader call sites are the asset table), the way a decompiled port can
and an emulator cannot. So the same run also traces provenance: the disc
layer reports every copy of sector data into game memory
(`TextureDump_Delivered`), an upload looks its pixels up in those and tags
each VRAM word with its disc byte offset, moves carry the tags, fills and
drawing clear them, and a primitive whose texels and palette are all tagged
adds a line to `assets.txt`: offset, row layout (a stride, or each row's
offset when the streamer laid the blocks side by side), size, depth,
palette offset, the pixel crop within the first word, and the hash of the
PNG it was drawn as. `extract_images.py --assets <dir>/assets.txt` then
writes those images from the archives, and every one comes out identical
to the PNG the game drew (76 of 76 through the title and main menu), which
is the proof of the provenance. `MEMORIES_DUMP_TEXTURES_FROM=<frame>` starts
both lists over at that frame, so a dump holds everything one screen draws,
also what an earlier screen drew first (the duel's digits after the Build
Deck's). A state load restores VRAM without
deliveries, so it clears the tags: the textures traced after it are the
ones loaded after it.

### Internal resolution

View > Console resolution / Internal 2x, 4x (the `internal_scale`
setting, `MEMORIES_INTERNAL_SCALE=N`, 1, 2, 4 or 8) shows a picture of the
whole of VRAM at N x N pixels per VRAM word in 24-bit color instead of
VRAM. VRAM itself stays exactly what the console's would be: the game reads
it back and states hold it, and the 1x frame the smoke fixtures hash is
byte-identical at any scale. No dithering in the picture; the mask bits
are VRAM's; a texture pack's image is sampled at its own resolution there,
VRAM's own texels otherwise. One exception: when a card burns after a
battle, `DuelCard_CaptureRoundedTexture` reads the card back from the screen
(StoreImage), marks it semi-transparent, rounds its corners and loads it at
320,256, where the burn draws it from. VRAM only has that copy at the
console's resolution, so the burn showed the card without its pack image or
HD text. The function now also calls `Memories_PictureCapture` (under
`MEMORIES_PC`): the OpenGL picture copies that part of its scaled picture
when the record reaches it, and 16-bit texels in the rect take their color
from the copy while their word still decides transparency and
semi-transparency. A load, copy or fill over the rect, a resync or a new
scale drops the copy. The software picture and 1x keep VRAM's texels there. A line is the console's one-pixel line made N
times thicker, one quad covering each picture pixel once (`line_quad` in
both renderers), so a semi-transparent line blends once per pixel as on the
console. Drawing it as an N x N block per picture step blended the Library's
grid up to N times, too bright, and at 4x cost the OpenGL replay about
880,000 vertices a frame (1200 frames of the Library took 30 s, now 2.3 s).

Two renderers draw it. With the SDL backend on OpenGL 3.0 or later
(`gl_picture.c`; OpenGL ES 3.0 on Android, "OpenGL ES 3" below) the
software GPU only records what it does to VRAM (every
GP0 batch, every transfer, `SoftGpuRecorder` in `soft_gpu.h`) and the
record is replayed at present into a framebuffer: VRAM is an integer
texture the fragment shader decodes (4, 8 and 16 bits per texel through
the palette) as the software GPU samples it, a mod's texture banks are an
array texture uploaded when a replay finds them changed, and a pack's
images are textures of their own with the pack's word-to-entry maps beside
them. Opaque pixels and blending modes 0, 1 and 3 share one draw (the
color comes out pre-multiplied, the destination's factor in alpha); mode
2 draws its opaque texels, then its semi-transparent ones subtracted.
What a primitive draws is not sampled by a later one of the same frame
(the game never renders to a texture); across frames VRAM is uploaded
whole after each replay. A duel with 3D Monsters replays in 1.5-2.5 ms a
frame at 2x and 2.5-4 ms at 4x on the development machine.

Without that (the X11 backend, `MEMORIES_GL_PICTURE=0`, an older GL) the
software GPU draws every primitive a second time into the picture
(`soft_gpu.c`, `picture_*`), the picture's pass before the word's so both
see the same mask bits; uploads, fills and moves keep it in step; a state
load redraws it from VRAM. That costs about 9 ms a frame at 2x and 30 ms
at 4x in the same duel. It is the OpenGL pass's oracle: `MEMORIES_DUMP_FRAME`
with `MEMORIES_DUMP_PICTURE=1` writes the picture (from either renderer)
instead of the frame, and `MEMORIES_DETERMINISTIC=1` makes a windowed run
as deterministic as a headless one, so the two can be
compared on one frame of the same build; the title, the main menu, a 2D
duel frame and a 3D one come out identical pixel for pixel at 2x and 4x
(three edge pixels differ on the 3D monster). The X11 backend shows VRAM
as before (`Platform_PresentPicture` returns 0).

Widescreen draws the game's full-screen drawing areas into wider targets
(`soft_gpu.c`, `SoftGpu_WideMargin`). Each target is its area widened by a
margin on each side. Every primitive drawn to the area is drawn a second
time into the target, shifted right by the margin: polygons and lines are
unclipped at the sides, sprites keep the 4:3 clip. Transfers into the area
are copied into the target's centre. VRAM itself is never widened.

The software GPU keeps the 1x targets. At 2x and up the OpenGL pass draws
its own at the scale (`gl_picture.c`, "widescreen"). The window then shows
those, so the software GPU keeps its targets' bookkeeping (which areas have
one, whether anything was drawn) but no longer draws the primitives into
them a second time (`SoftGpu_WideRastered`): widescreen's software drawing
takes what 4:3's does (3D Monsters duel, 2x and 4x: about 2.4 ms per frame
instead of 4.5 ms), and the window is identical. A 1x frame dump then reads
the OpenGL target; if the backend has no room for one, the window shows the
4:3 picture between black sides. Changing the scale makes the targets again
from VRAM. Its targets follow the
software GPU's rule and are laid out the same way. Each is a texture of
the widened area alone. Its primitives go through the same runs as the
picture's, in the same order, drawn with the viewport moved. Since each
primitive is gathered for the picture and then for its target, the runs
alternate between the two; `flush_runs` draws the picture's runs first and
then each target's, each in its own order and with neighbours in one state
joined (`group_runs`). Nothing in one flush reads the picture or a target
back, so the picture is the same, and the 3D Monsters duel takes 17-22
draws and 2-4 framebuffer switches a frame instead of about 400 of each
(4x, NVIDIA: 1.6-1.9 ms per replay instead of 5-6.7 ms). Before this,
the software GPU drew the scaled targets on the CPU next to the OpenGL
pass. Unthrottled, the 3D Monsters duel case to its frame now takes 40 s
instead of 128 s at 2x, and 41 s instead of 350 s at 4x. Against the
software picture, the widened duel comes out identical save 3 edge pixels
at 2x and 20 scattered ones at 4x (4:3 has 3 and 14: polygon edges and
texture rounding), and the main menu save the corner below.
After a resync (an overflowed record, a state load) a target's sides stay
black until the next frame draws them.

A target made in the middle of a frame starts from what is already there:
the software GPU seeds its scaled centre from VRAM's 1x words, the OpenGL
pass copies the scaled picture, which still holds what was drawn there at
the scale (in this frame or an earlier one). So a corner the frame does not
draw again shows at 1x in the software picture and at the scale here: two
texels of the main menu's top row. Whatever the game draws every frame
comes out the same. As in the software GPU, a target's sides are made
black when it is shown with nothing drawn into it since it was last shown
(a movie, a still loaded into VRAM).

Video > Anti-aliasing (`msaa`, `MEMORIES_MSAA`: 0 off, 2, 4 or 8 samples)
draws the OpenGL picture and widescreen's targets into multisampled
renderbuffers, which smooths the edges of polygons: the 3D monsters, the
duel table, the cards laid on it. Sprites and textures are drawn as before.
Each buffer is resolved into its texture wherever the texture is read: a
move's source, a target's centre, and the end of each replay, for
presenting and frame dumps. A GPU cannot copy into a multisampled buffer,
so the pass draws what it copies in as a quad.

Without anti-aliasing a polygon's vertices move half a picture pixel
right and down, so GL, which tests pixel centres, draws the pixels whose
corners the software pass tests. With it, a pixel is drawn as much as its
area is covered, and that move left half of every pixel along an edge on a
word boundary uncovered: a dark column at x 192 of the duel's stone bar,
where the left half's rectangles meet its mirrored right half, a quad (and
a line along the quad's top and right). Into a multisampled buffer the
vertices stay where they are and the fragment shader takes the attributes
half a pixel up and left, at the pixel's corner, the same values
(gl_picture.c, triangle()). In the bar the picture is then identical to
anti-aliasing off, but for the field's slanted edges at its corners.

The buffers cover all of VRAM at the scale. With 8 samples that is about
256 MB of video memory at 4x and 1 GB at 8x. Where the driver has no room,
it says so once, and the picture is drawn without until the setting or the
scale changes. (With the allocation made to fail, the duel comes out
identical to off, in 4:3 and widescreen.) A driver with fewer samples
gives what it has, and says so.

Turning anti-aliasing on or off mid-game carries the picture over. After
a switch, the duel case's frame is identical to one run with the new
setting from the start. The widescreen targets are made again, so their
sides are black for a frame, as after a resync. With anti-aliasing off,
the picture is identical to before. In widescreen the centre of a target
is identical to the 4:3 picture, with it on or off.

It changes nothing at 1x, and nothing in the software picture.

### OpenGL ES 3 (Android)

On Android the picture pass runs on OpenGL ES 3.0 or later, so internal
2x and 4x are drawn by the GPU (the software picture costs a phone far more
than a desktop) and HD text, HD numbers and labels, the opponent's name and
PGXP are there. `gl_picture.c` uses nothing past ES 3.0 (integer textures
and framebuffers, `texelFetch`, array textures, multisampled
renderbuffers, blits). Its shaders are one source: on ES (`GL_VERSION`
starting "OpenGL ES ") `es_source` replaces the `#version 130` line with
`#version 300 es` and high precision for float, int and the samplers, and
drops `noperspective`, which GLSL ES does not have. Every vertex's w is 1,
so the perspective-correct interpolation ES does instead is the same up to
rounding. The desktop strings are untouched.

There is no context of the game's own there. SDL's renderer (opengles2)
presents, as it did, but `create_window` asks for an ES 3.0 context
(`SDL_WINDOW_OPENGL` and the context attributes before
`SDL_CreateRenderer(window, "opengles2")`), and the pass draws in that
context between the renderer's batches. `es_enter` flushes the renderer
(`SDL_FlushRenderer`, which also makes SDL set its own state again) and
makes the context current; `es_leave` puts back what SDL sets once and
relies on: the framebuffer it draws into and its pixel alignments of 1. The
pass's frame reaches the window through a texture of SDL's own, the size of
the shown area: `GlPicture_CopyInto` blits the picture's area (or the
widened picture) into it, and the renderer draws it with the menu over it.
SDL owns that texture and the pass only draws into it, so either side can
make its textures again without the other. (Wrapping the pass's own
texture as an SDL texture does not work: SDL's `GLES2_CreateTexture`
specifies the storage of a texture it is handed, which wipes it.) A
repaint of the menu over a still frame shows the same texture again.
`use_gl` stays 0 on this path: the desktop presenter and Video > Color
(`present_pass.c`, fixed function) are desktop only. A device without
ES 3 (or a failed context) gets the renderer SDL picks and the software
picture, as before, and Video > HD text says "needs OpenGL ES 3".

Limits on ES: internal resolution up to 4x (8x is refused: 128 MiB per
picture texture), anti-aliasing up to 4 samples (8x is given as 4x). The
GL version, renderer, GLSL version and any shader compile log go to
standard error, which the app forwards to logcat (`adb logcat -s
memories`: "OpenGL picture: OpenGL ES 3.2 ... on Adreno ..., OpenGL ES
GLSL ES 3.20", then "OpenGL picture pass on").

**A lost context.** Android may lose the GL context while the app is in
the background; SDL then makes a new one and sends
`SDL_EVENT_RENDER_DEVICE_RESET`. SDL's GLES2 renderer cannot go on (its
context is the lost one), so `reset_renderer` forgets the pass's GL names
(`GlPicture_Lost`, nothing deleted), destroys and makes the renderer again,
starts the pass in the new context and lets the next frame make the
textures again; the pass's first replay draws the picture again from VRAM
(as a resync does). Where the pass does not start again, or its frame
cannot be copied into the renderer's texture, `GlPicture_Stop` takes its
recorder out of the software GPU, which draws the scaled picture again
from VRAM, as on a device without ES 3. `MEMORIES_TEST_GL_RESET=<frame>`
sends that event at a frame, with nothing lost, to try the path anywhere;
`<frame>fail` also keeps the pass from starting again.

**On a desktop: `MEMORIES_GLES=1`** takes the same path in a desktop
window, to test it where frame dumps and the desktop renderer can be
compared on one machine. On Windows the ES context must be the driver's
own (`WGL_EXT_create_context_es2_profile`): where the driver has none, or
one older than 3.0, SDL would fall back to an EGL library found on the
search path (ANGLE's copy from any other program), so the window first
makes its desktop context, checks the profile as SDL does (the WGL
extension and `GL_ARB_ES3_compatibility` or later), and stays on desktop
GL if it is missing. Off Windows it is GLX's ES profile or the system's
EGL (Mesa).

Checked (2026-10-05), `MEMORIES_GLES=1` against desktop GL on the same
build, frame dumps of the scaled picture (`MEMORIES_DETERMINISTIC=1`,
`MEMORIES_DUMP_PICTURE=1`):

- Mesa 21.2.6's llvmpipe (32-bit Linux build, Xvfb; OpenGL ES 3.2 against
  3.1): the title and the first duel's frame (mods off) at 2x and 4x, and
  at 2x with HD text, the opponent's name, PGXP and 4x anti-aliasing on,
  that duel with and without the 3D Monsters mod: identical pixel for
  pixel. Dropping `noperspective` changed no pixel there. The window (the
  desktop presenter against the renderer's copy) is identical at the
  title, also after `MEMORIES_TEST_GL_RESET`.
- The NVIDIA driver (RTX 3080, its WGL ES 3.2 profile), 32-bit and x64:
  title, Options, name entry, a story scene, the first duel (hand, field,
  the 3D field, the 3D Monsters mod on), the map, Library, Free Duel, the
  credits, a fusion result and Spellbinding, in 4:3 and widescreen, and
  with HD text, the opponent's name, PGXP, anti-aliasing and xBR on:
  identical between this build on desktop GL, through the ES path and the
  base build, apart from a clock shown on one screen and anti-aliasing's
  one-unit noise that two runs of the base also show. With the present
  pass's effects on (desktop GL only) the base and this build agree. A widened picture was compared in the window
  (`MEMORIES_WINDOW_SHOT`, `MEMORIES_ASPECT=2`, half speed so that every
  frame is presented) as well: the title, the main menu, Options and the
  duel at 2x and 4x. Forced resets (`MEMORIES_TEST_GL_RESET`) leave the
  picture as without one; with `<frame>fail` the dumps equal those of the
  software picture (`MEMORIES_GL_PICTURE=0`).
- The Android emulator (api35x64, SwiftShader, OpenGL ES 3.0) runs the
  pass at 2x and 4x with HD text and carries on after the app goes to the
  background and back (the context was kept there); a phone (Adreno 660)
  plays at 4x with HD text.

### HD text

Video > HD text (`hd_text`, `MEMORIES_HD_TEXT=1`, off by default) sets the
text's letters in a font at the internal resolution instead of drawing the
retail 8x12 and 16x16 cells texel by texel (`src/pc/text/hd_text.c`). It is
a change to the OpenGL picture above, so it shows at internal 2x and up,
in widescreen too. The software picture (`MEMORIES_GL_PICTURE=0`) and 1x
are as before.

The retail font is anti-aliased in its indices, which the text palettes run
from black up to the text's color. The dark outline is the lowest index
(1, and 2-3 in the large font). Above it, an index is how bright the texel
is, and the letters are shaded brightest at the top of the cell. An HD
letter is made the same way at N pixels per texel:

- the character is set in the font glyphs.c sets added letters in (a mod's
  `font`, else the system's sans-serif);
- the retail font is measured once from its letters and digits: its
  baseline, x-height, capital, ascender and descender lines, its stem and
  bar weights, its outline and each row's shading. A texel at 70% of its
  row's brightest counts as fully covered, because the large font is shaded
  across its strokes as well as down;
- a letter or digit is set to those lines, so every small letter is the
  same height, and so is every capital; anything else takes its cell's
  height. Across, it stands where its cell's letter does, as wide as it
  (at most a quarter wider than the font's proportions; a bare stem like
  an l keeps them);
- stems and bars are thickened or thinned to the retail weights. Where the
  font's I is a bare stem and the retail one has serifs, it gets serifs,
  so it is not taken for an l. With a PAL language (Game > Language), the
  i and l and the letters made of them (í, ì, î, ï) get the serifs their
  cells have (`serif.c`; notes/translation.md, "Widths"): the font's stem
  (its dotless i's, or the l's) stands where the cell's stem is, at the
  font's proportions, and gets a foot across its last rows and a serif
  left of its top, which stops a texel short of a mark over it. Those
  cells are in texture bank 15, which a texture pack does not reach, so a
  pack that replaces the font leaves the i and l the port's then;
- each pixel's coverage goes onto the run from the outline's index to the
  row's shading (half-way from its average stroke to its brightest, so the
  color is the cell's), and the outline's index goes in a band a texel
  wide round it.

The shader samples the HD indices in place of the cell's, and the glyph's
palette does the rest. So colors, fades, flashes, semi-transparency and
the order the game draws in are the game's, including turned and leaning
letters and the letters translations add (texture bank 15).

func_80035E20 marks its glyph primitives with bit 15 of the texture-page
word, which the hardware leaves unused (bits 11-14 are the bank). Only
marked primitives are drawn this way. The mark changes nothing else: the
smoke cases give the same hashes with `hd_text=1`.

Glyphs that are not letters, digits or ASCII punctuation (the card-type
icons, the arrows) keep their texels. So does any letter no font sets. The
pictures are made the first time a letter is drawn, and made again if its
cell changes; the atlas holds 896 (its last four rows of cells are the
titles').

Card titles go the same way. The plate on a card (the 96x14 4-bit strip
func_800289BC uploads under the art) is set anew from the card's name as it
reads now: the base cards', a mod's added cards' and a translation's alike
(`Cards_NameUtf8`). It is the port's own plate renderer (`src/pc/cards/art.c`,
the one that makes the plates of added cards: Times at 13 pixels, the
baseline under row 11, squeezed past 90 columns) at N times the size, and
its coverage goes onto the plate's inks, 1 the darkest to 7 the faintest.
The card view subtracts those inks from the frame, so through the plate's
palette the letters come out dark with smooth edges on every frame color.
func_800289BC notes where each plate went (`HdText_TitleUploaded`); a 4-bit
primitive sampling those words while they still hold the plate (the flat
strip, or a piece of the turning card) samples the picture instead. Twenty
titles are kept, the least recently drawn made over. So no pack needs to
carry names, and a card a mod adds reads like the rest.

### HD numbers and labels

HD numbers and labels come with HD text (the same item; they had one of
their own, `hd_hud`, until v0.1.2). They do for the duel's numbers and
labels what HD text does for the text. They are sprites from sheets of their own, not the font's cells, so
HD text never reached them. It works in the OpenGL picture at 2x and up; 1x
and the software picture never change.

- **Digits.** Covered: the life points, the deck counts, the hand's and
  field's ATK and DEF (8x8, 8-bit, beside the duel's terrain at (896, 256)),
  the field cards' 12x16 numbers and a second set of small ones, and the
  menus' 8x8 digits (the deck builder's list and the card bar, at (704, 0)).
  - Each sheet is measured from its own ten digits: the outline (the index
    next to nothing), the fill (the commonest inside), the ramp of indices
    between them, their feet, heads and stroke weight.
  - A digit is then set in the text's font like an HD glyph and colored
    through the ramp, so the game's palettes still decide the colors (the
    inactive side's dimming too).
  - A color off the way from outline to fill, like the purple the duel's
    digits have in a few corners, is left out.
- **Labels.**
  - The life-point panel's LP, COM and YOU are set anew in the font, over
    the panel's own texels made larger, in the box's colors. Only where
    the retail panel is (a hash of its words); a mod's own panel is left as
    it is.
  - The card kinds (Magic, Equip, Trap, Ritual), in the hand and on the
    card bar, are set in the plates' serif face (Times) with their outline.
  - The FIELD box's word (with its shadow) and the terrains' names. A name
    the game draws in two sprites (MEAD + OW) is set whole and cut where
    the sprites meet.
  - The card view's ATK and DFD and its digits are set in the plates' serif
    face, in the plates' subtracted inks.
  - The labels' rectangles are those #47's HD pack recipe lists.
- **Texture packs come first.** A sprite a pack paints is drawn from the
  pack, so an HD pack's art for these is never overridden: with a pack that
  covers them the picture is the same, pixel for pixel, with this on or off.

The pictures share HD text's atlas (four rows of cells above the titles).

### Opponent's name for COM

View > Opponent's name for COM (`opponent_name`, `MEMORIES_OPPONENT_NAME=1`,
off by default) shows the opponent's name in the life-point panel's COM
box, in the OpenGL picture at 2x and up and at the console's resolution. A
pack can't do this, because the panel is one texture for every opponent.
At 1x the software GPU draws the same box right after the panel sprite
(`name_over_panel` in `soft_gpu.c`, through the `SoftGpu_PanelName` hook
that `libgpu.c` sets while the option is on): its palette indices, made
at 1x (`HdText_NamePixels`), through the panel's CLUT, so the inactive
side's dimming applies and whatever the game draws over the panel stays
over it. With the option off nothing is drawn.

Video > HD text is dimmed when it could not show: "needs OpenGL 3" when
the picture pass is off (no OpenGL 3, the SDL renderer fallback,
`MEMORIES_GL_PICTURE=0`, the X11 backend; on Android "needs OpenGL ES 3",
a phone without it) and "needs Internal 2x" at
console resolution (`Menu_SetHdPicture`, `menu.c`). The opponent's name
works at 1x with or without OpenGL, so its View item is dimmed only at 2x
and up without the picture pass ("needs OpenGL 3 or 1x").

- The name comes from the opponent id (`gDuel_bOpponentID`, 1-39) through
  `Tables_DuelistShortName`. A name of up to 11 letters is shown whole.
  Longer ones show the part that tells the duelist apart, a High Mage or a
  Guardian with the title shortened: Weevil, Mai, Keith, Soldier,
  H.M. Secmeton, H.M. Anubisius, Mountain, H.M. Atenza, H.M. Martis,
  H.M. Kepura, Labyrinth, G. Sebek, G. Neku, Master K. A translation can
  give its own (strings `FE41`-`FE67`, or its names for the duelists:
  `Text_OpponentName`, notes/translation.md).
- The box is COM's, made from the panel's own texels: its left end, then its
  rows' border and background, as long as the name needs, growing leftwards
  from where it meets the panel. The name is set in the text's font in COM's
  colors, through the panel's palette, so the inactive side's dimming still
  applies.
- It is drawn over the panel whatever drew the panel: the retail panel, HD
  numbers and labels, or a texture pack's image.
- A 2P duel (no opponent id) and a panel other than the retail one keep COM.
- YOU's box shows the name the player gave at name entry (the save's,
  `SaveSlots_StateName`), made the same way; You when the save has none.
- The result screens name the sides too (the player over YOU's column,
  the opponent over COM's). They call the dialogue bank's YOU and COM
  labels by their place, which `Text_Retarget` points at the names. Their
  small font has no full stop and odd digits, so there the name is the
  panel's when it is letters only and at most 9 of them, else its longest
  word of letters (G. Sebek: Sebek, Teana 2nd: Teana, Simon Muran: Simon).
  A copy of the result strings steps less before COM, so the name ends
  where COM did. The player's name is YOU's box's when it is letters,
  digits, spaces and `:` alone, else You (the font has no `. ! ? $ & * %
  @` and draws `- / +` in the blue of its own dots); the copy steps less
  before each YOU (more for a name shorter than three), so it ends where
  YOU did, over its numbers. `WINNER ···` names the winner
  (`gDuel_bWinnerSide`): the player's name, or the opponent's when the
  computer won, ending where the game's YOU or COM would; a name that
  cannot stay You, or the game's own COM. 2P duels keep 1P and 2P.

Not covered yet: the sword and shield icons (pictures, not lettering).

### View > Free Duel progress

View > Free Duel progress (`free_duel_progress`, `MEMORIES_FREE_DUEL_PROGRESS=1`,
off by default) shows `owned/obtainable` right of the FREE DUEL title for
the opponent under the grid cursor: `12/157` for Duel Master K, in the
game's own 8x12 text font, white, and yellow (the game's own yellow text
ramp) once every card is owned. The host draws it over the picture as it
draws the fusion helper (`src/pc/cards/free_duel_progress_view.c`, from
`Hud_Draw`), laid on the game picture in its own pixels, so it keeps its
place in widescreen and at any internal resolution and is as sharp as the
game's text. With the option off nothing is drawn and the disc is not read;
a window capture of the grid with the cursor on an opponent is identical to
master's.

- **The font** comes off the player's disc the first time the count is
  drawn (`src/pc/cards/font_art.c`), never from the console's VRAM, and
  nothing of the game is kept in the repository. It is the boot package,
  WA sector `0x1690` (`Main_RunBootSequence`, `Main_LoadBootPackageStage`):
  not a TIM but raw VRAM words, one sector a 64 x 16 block placed down a
  column from 0x280, 0 (`File_StepActiveTransfer`), so the font's page is
  its first 16 sectors; the text color ramps are the first 0x100 bytes of
  its sector 50, a 16 x 8 `LoadImage` at 0x280, 0xE8, one row a color in
  the order of `gText_abColorSlots`' values (0 white, 1 yellow, 2 blue, 3
  green, 4 grey, 5 orange, 6 red). The glyphs are 4-bit 8 x 12 cells where
  `func_80035E20` finds them (`retail_cell` in `glyphs.c`): '0' at 120, 0,
  '1'-'9' from 0, 12, '/' at 112, 0. `src/pc/cards/disc_art.c` unpacks the
  package into a private VRAM the way the loader does, cuts the glyphs and
  draws them (each window pixel takes the texel under it). If the disc
  cannot give them nothing is drawn and the log says so once.
- **The shared module** `disc_art.c`/`disc_art.h` is added, byte for byte
  the same, by View > Duel rank too (for the result screen's pictures), so
  each change stands alone and whichever lands second merges it unchanged.
  A change to one copy belongs in the other.

- **Obtainable** is every card with a weight in any of the opponent's three
  drop pools (S/A-POW, B/C/D, S/A-TEC). The disc's rows are read once, as
  the yamyi-mods Library panel reads them (WA_MRG `0xE9B000 + 0x1800 * (id -
  1)`, three sectors an opponent, rows 1-3 of four 1460-byte rows), and each
  goes through `Tables_PoolFor`, the call the drop roll makes: a mod's
  `"drops"` counts, Drop missing cards included (Simon Muran 58 -> 61). The
  deck pool is not counted. Checked against the rows read with Python:
  Simon Muran 58, Teana 28, Seto 102, Duel Master K 157. The rows and the
  mods' tables are fixed for a session (data mods and `Tables_Build` are
  applied at startup), so each opponent's cards are worked out once, the
  first time the cursor rests on it.
- **Owned** is how many of those the deck and trunk hold now
  (`Cards_ChestSlot` and `gDuel_awPlayerDeck`, as `owned()` in `drops.c`
  counts). The game keeps no record of who gave a card, so this is the
  collection, not where it came from.
- The cell is the pending one the pad moves (`gFreeDuel_bTargetColumn/Row`),
  so the count changes as soon as the cursor starts to glide. Its index is
  the opponent's id (`func_80024DC8`); Build Deck (cell 0) and empty cells
  show nothing. It also shows nothing while the screen's text box is up or
  it is leaving (`gFreeDuel_bScreenFlags` 0x20/0x40), during a fade, and
  while `Main_InitFreeDuelMenu` is still loading the module: until
  `FreeDuel_Init` runs, the module's state is the last screen's.

`pc_free_duel_progress` (`tests/pc/free_duel_progress_test.c`) counts over a
made-up WA_MRG with the real `tables.c`: the union of the three pools, a
zero weight and the deck pool left out, deck and trunk counted once per
card, a mod's added and removed cards (a mod card among them), an edit of
`all`, and a disc without the file. `pc_font_art` (`tests/pc/font_art_test.c`)
reads the real `game/DATA/WA_MRG.MRG` (skipped without it), checks the
glyphs against the hand decode above in all seven colors and draws counts
through `FontArt_Draw`; `MEMORIES_FONT_ART_SHEET=<file.ppm>` saves them.
Not done: a frame on the portraits of opponents whose cards are all owned.

### Duel rank

View > Duel rank (`rank_meter` in `settings.txt`, `MEMORIES_RANK_METER`,
0 by default) shows, during a duel against the computer, the rank the duel
would end with: **Rank** (1) the letter and axis, S-POW to S-TEC, and
**Rank and score** (2) also the score, 0-99 as the rank uses it (below 50
is TEC). It is drawn with the result screen's own pictures, right of the
FIELD box and as tall as it: the stone plate with the rank letter on it (D
blue, C green, B yellow, A red, S magenta, the colors the result screen
gives them) and the POW or TEC badge behind the letter's top left; the
score follows in the cards' ATK/DEF digits. Off, nothing is drawn, nothing
is computed and the disc is not read: `update()` returns before reading
anything.

- **The sum** is `Rank_Score` (`src/pc/cards/rank.c`): 50, the ending's
  adjustment, and `Duel_CalcRankScoreChange` of the ten counters in the
  player's side record (`D_800E9FF0[0]`), in the order and with the reads of
  `Duel_CalcRankScore`. That matching function is left as it is (it also
  fills the result display and the statistics pages); `Rank_Score` writes
  nothing. Before calling the game's lookup it checks that the walk ends
  inside the ten rows of `gDuel_awRankScoreChange`, so a table that is not
  loaded cannot hang it (the plate is then hidden). The table comes with the
  opponent's block and stays unchanged for the whole duel (checked each
  frame over three duels).
- **The letter** is `Rank_Grade`, `DuelScene_UpdateResultRewards`'s rule:
  below 50 is TEC and mirrored as 99 - score (from 0), 100 and up count as
  99, and (score - 50) / 10 is the tier.
- **The ending** is only known at the end: until then the record's
  adjustment is 0 and the plate counts the +2 of an LP win. A deck-out win
  (-40) or Exodia (+40) moves the final rank from what was shown.
- **When:** the player on side 0 against a CPU opponent (`D_8009B360 < 0`,
  `gDuel_bOpponentID >= 0`), scene phases 2 to 11 except 6 (the used card
  shown across the screen), and not while the card viewer, a card effect
  or the quit dialog is up. It follows the FIELD box sprite (`D_8009B214`)
  and is hidden while the box is not all on screen: the box slides off for
  battles and some camera views, and for the outro.
- **The pictures** come off the player's disc the first time the rank is
  drawn (`src/pc/cards/rank_art.c`), never from the console's VRAM, so they
  are there before any result screen has been seen, and nothing of the
  game is kept in the repository. They are not TIM files: the packages are
  raw VRAM words, one sector a 64 x 16 block placed down a column and on to
  the next 64 words every 256 rows (`File_StepActiveTransfer`), with the
  palettes as separate `LoadImage` rectangles. `src/pc/cards/disc_art.c`
  unpacks them into a private VRAM the way the loaders do and cuts sprites
  from it:
  - the result screen's package, WA sector `0x1DAB`
    (`FILE_WA_DUEL_RESULTS_START_SECTOR`; `func_80020BE4`): 32 sectors of
    image to VRAM 0, 256, the palette sector (+32) to 0, 248 (256 x 4), and
    the display resource (+33, `D_801AF000`). The pieces are what that
    resource's sprite sheets say for the two objects `func_800218F0` makes,
    indices 0, 5, `is_tec_rank` and 0, 6, `rank_tier`, walked as
    `DisplayObject_UpdateCommandStream` and drawn as
    `DisplayObject_RenderSpriteSheet` does (the objects set flag 0x20, so
    the sheet's own palette step is added). That gives, on page 0, 256 in
    4-bit color: the badge 24 x 24 at 184, 312 (POW, palette 112, 248) or
    184, 288 (TEC, 96, 248), the plate 56 x 48 at 128, 288 (16, 248), and
    the letter 40 x 40 at 0/40/80, 128/168 with a palette for each (S 144
    ... D 208). `pc_rank_art` checks each against this hand decode.
  - the card digits from a terrain package, WA sector `0x16C6`
    (`Duel_LoadPackageStage`, all seven are the same here): 64 sectors of
    image to 0x300, 0x100 and four of palette to 0x100, 0xF0 (256 x 16);
    `Duel_DrawCardFrame` draws them from page 0x1E, v 0x58, 8 x 8 in
    8-bit color, palette 0x100, 0xF1.
  If the disc cannot give them the rank is not drawn and the log says so
  once; there is no fallback to other lettering.
- **Drawing** is the host overlay, like the fusion helper's (`hud.c`,
  `Hud_Signature`), laid on the game picture in its own pixels: the plate at
  half size (28 x 24, the box's height), the letter at 24 x 24, the badge at
  12 x 12, the digits 8 x 8. Each window pixel takes the texel under it when
  a piece is enlarged, so it is as sharp as the game's own 2D at any window
  size, internal resolution or in widescreen, and the average of the texels
  it covers when shrunk (a 1x window). Nothing goes through the GPU or the
  present pass. With Video > xBR texture filtering on, the game's own 2D is
  smoothed and these pieces are not.
- **The shared module** `disc_art.c`/`disc_art.h` is added, byte for byte
  the same, by View > Free Duel progress too (for the game's text font), so
  each change stands alone and whichever lands second merges it unchanged.
  A change to one copy belongs in the other.
- **Check:** when the result screen opens, with the option on, the log gets
  `memories-pc: duel rank: ours N, the game's N (winner side 0, same rank)`:
  the same sum with the adjustment the duel ended with, against
  `side_scores[0]` and the game's own rank. `pc_rank`
  (`tests/pc/rank_test.c`) links the unchanged `duel_result_runtime.c` and
  compares both sides over 20,000 random records and tables, including the
  retail rows. `pc_rank_art` (`tests/pc/rank_art_test.c`) reads the real
  `game/DATA/WA_MRG.MRG` (skipped without it) and draws the ten ranks
  through `RankArt_Draw`; `MEMORIES_RANK_ART_SHEET=<file.ppm>` saves them.

### Library: show every card

View > Library: show every card (`library_all_cards`,
`MEMORIES_LIBRARY_ALL_CARDS=1`, off by default) makes the Library show every
card the player has never seen. Each one has its art, name, stats, Guardian
Stars and text. Nothing is given and nothing is saved, and it lasts only
while the Library is open.

The Library works out what it shows once, as it opens (`func_8002BFCC`).
It first marks every trunk and deck card as seen (`Library_MarkOwnedCards`,
unchanged). It then fills one byte per card in its screen state at
`D_800EA1E8`: 0x80 for a seen card, plus 1 when the player owns none. On
the port the seen test was already `Cards_Seen` inside a `MEMORIES_PC`
block. One `else if` under that block, still in PC-only code, gives a card
that fails the test the same byte a seen card nobody owns gets
(`Cards_LibraryPlaceholder`, `src/pc/cards/cards.c`). The grid, the name
line and the card view read only that byte, so they show the card as seen.
The seen flags (`0x120 + id`, `Cards_MarkSeen`), the trunk and the rest of
the save are never written. The heading's "seen/total" still counts only the
cards really seen. With the option off, the added branch never runs and the
Library is the retail one. Matched code is untouched: the retail
`Campaign_TestStoryFlag` arm is as it was.

Checked in a window (`MEMORIES_DETERMINISTIC=1`, `MEMORIES_SDL_SCRIPT`
clicking View at x 242, y 13 and the item at x 310, y 70). The route was a
New Game (the duel-hand-camera case's input up to the name),
`MEMORIES_MODE_AT=1940:4,3000:0` to open the Library in place of the story
and, once it closes, the debug menu. There, TITLE with the value 10 opens the
loaded menu on SAVE, and the game is saved to slot 1.

- Option off, the Library showed 35/722 (the starter deck). Option on,
  every cell was filled, the heading still read 35/722, and card 001's view
  (Blue-eyes White Dragon, never seen) showed art, stats, stars and text.
- The two saves, one from each run, are identical over `[0, 0xF00)`: the
  header, both state copies, the deck, the trunk and the seen flags. They
  differ only in the 4 bytes of the slot token at 0xF08, which is drawn
  afresh on every save (`save_slots.h`).
- In the second run the option was then switched off and the Library
  opened again from the loaded menu. It showed the same 35 cards as the
  first run.

The option is read when the Library opens: switching it while the Library
is open takes effect the next time it opens.
### Card passwords (View)

View > Card passwords (`card_passwords`, `MEMORIES_CARD_PASSWORDS=1`, off by
default) shows the card's eight-digit password in the card view: the viewer
the duel, Build Deck (deck and trunk) and Trade share, and the Library's
card page. The digits go on the second Guardian Star's row, flush right; a
magic, trap, ritual or equip card has the same row at the bottom of its
empty middle panel. Leading zeros stay (Right Leg of the Forbidden One is
08124921). The 24 cards the Password screen cannot give (`N/A` in
`notes/card-catalog.csv`) show nothing; the ones that cost 999999
starchips show theirs.

- **The table** is the Password screen's (`src/overlays/password/shop.h`):
  `Password_LoadPackageStage` reads the package from sector
  `FILE_WA_PASSWORD_START_SECTOR` of `WA_MRG.MRG`, 64 + 4 sectors of
  pictures and then 3 to `0x801A8000`, one record per card id from 0: the
  price and the password, eight BCD digits, as little-endian words (Blue-eyes
  is `3F 42 0F 00 39 11 63 89`: 999999 and 89631139; no password is
  `0xFFFFFFFE`). `Cards_Password(id)` (the shared policy in `src/game/card_password.c`,
  backed by `src/pc/cards/password_data.c`) reads
  those 3 sectors from the disc once, the first time it is asked, checks
  every value is BCD or `0xFFFFFFFE`, and logs card 1's and how many have
  none (89631139 and 24 on the retail disc). A mod card can have one with
  `"password"` ([More cards](more-cards.md)), and a mod's `passwords`
  ([gameplay tables](gameplay-tables.md#passwords-and-prices-on-the-password-screen))
  changes any card's, for both the viewer and Password shop. Added cards
  can be purchased at a default price of 999999 starchips or the price set
  in `passwords`; they allow repeat purchases. Code mods can hook the shared
  `Cards_Password` and `Cards_PasswordPrice` policies so the display and shop
  stay consistent.
- **The drawing** is the game's text. The viewer's text box is laid out by
  string 3 (a monster) or 4 (the rest), whose `{f8 00 40}` inserts the
  card's text 80 pixels down in both. Once the face is up, the box is made
  again in place, on its channel, with its position and settings, from a
  copy of that layout (a translation's, through `Text_LookupString`) with
  the digits before `{f8 00 40}`: up 24 to the star row, to x 96, the
  digits, down 24. `Text_Resolve` gives that copy for string 0xFFFE
  (`CardPassword_Text`). HD text and a mod's fonts draw it like the rest,
  and the card's own text is where it was: dumps with the option on and off
  differ only in the digits' rectangle.
- **When:** the hooks are the handler tables, not the matched functions.
  Under `MEMORIES_PC`, `gDuelEffect_apfnStateHandler`'s card viewer is
  `CardPassword_UpdateViewer` and `gMain_apfnModeRunner`'s Library
  `CardPassword_RunLibraryMenu`; each runs the game's function, then looks
  at its state. The viewer shows the digits once `0x20` (face up) is set in
  its flags and neither `0x40` (slides) nor `0x10` (closing); the Library
  on its card page's resting step 5 (not the 3D model, step 4, nor the way
  back, 6). Either way only once the game's box has all its text and every
  letter has settled (`TEXT_BOX_FLAG_DONE`, no `DuelEffect_HasActiveEntry`):
  the Library types its text in, and a box made again at once would skip
  the last letters' appearance. The box goes back to the retail layout on the frame Circle is
  read, so the closing's first picture is the retail one: in a
  deterministic duel dump the flip's last frame is identical with the option
  on and off, the next shows the digits, and the first closing frame is
  identical again. The same holds in the Library.
- Nothing is kept that a loaded state could contradict: which layout the
  box has is its string id, and the retail one comes from the card's type.
  Off, nothing is made and nothing changes.
- The box's channel has a slice of the text entries (255, 160, 160 or 45;
  with a PAL language 280, 220, 220 or 80).
  If the card's text and the digits do not fit together, the retail box is
  made again and that card shows no password: in the duel the viewer's box
  is on a 160-entry channel, where Right Leg, Left Leg and Right Arm of the
  Forbidden One (17-19, eight lines of text) do not fit; in Build Deck and
  the Library (255) they do.

Checked in a window (deterministic, 1x and 2x with HD text): Dancing Elf
59983499 in a duel, Blue-eyes 89631139, Right Leg 08124921, Tenderness,
Eternal Rest (a magic card) and Super War-lion (none) in the Library, and
Blue-eyes and Mushroom Man in Build Deck's trunk and deck. Trade shares the
viewer and was not reached.

### Precise geometry (PGXP)

Precise geometry (PGXP) has a Video menu option, *Off*/*Textures*
(`MENU_ITEM_PGXP`, `menu.c`), disabled where it cannot draw: below Internal
2x, or without the OpenGL picture pass. It affects the OpenGL picture only,
at 2x and up.

- **Affine textures** (`pgxp=1`, *Textures*). The GTE keeps no depth with a
  vertex, so textures on 3D polygons bend. Textured polygons are drawn in
  perspective, at the console's whole-pixel vertices.
- **Rounded vertices** (`pgxp=2`, *Textures and positions*). The GTE's
  perspective transform rounds each vertex to a whole console pixel, which
  makes 3D polygons wobble as they move. Polygons are also drawn at the
  vertices' precise positions. A model's parts are projected each with its
  own matrix, and where they meet, the vertices lie up to about a console
  pixel apart; the console's rounding closes those seams. So a frame word
  that carries two different precise positions keeps its whole-pixel one
  (`snap_seams` in `libgpu.c`; its depth stays precise).

  The `pgxp` setting is clamped to 1 for now: level 2 is not offered from
  the menu yet, pending a fix to `snap_seams` (a small model or part that
  happens to land on the same screen word as an unrelated one can be
  wrongly treated as a seam) and independent testing across more than one
  OS/GPU driver combination. Level 1 has none of that seam logic and is not
  affected.

**How it works** (`src/pc/compat/pgxp.c`):

1. `rtp()` in `gte.c` works each vertex's position out from the view
   position before the shift and the division in full, not from the GTE's
   rounded quotient. It keeps it with its depth beside the vertex's SXY
   FIFO entry (`Memories_GtePrecise`); anything else writing the entry
   drops it. A vertex whose precise position does not round near its screen
   word (clamped off the screen, say) has none.
2. Drawing that is ours or the game's C carries it to the packet, keyed by
   the packet word's physical address (`Pgxp_StoreAt`):
   - the HMD polygon drivers (`model_polygon_drivers.c`, the map) tag each
     vertex word they write, and the pre-pass tags its results for them;
   - the game units' `gte_stsxy` stores go through `Memories_GteStore`
     (`Pgxp_Stored`), and their `addPrim` (redefined in `pgxp_game.h`,
     which `build_game32.py` puts before every game unit and no mod
     includes) tags the words of the primitive that hold those vertices
     (`Pgxp_AddPrim`). This is how the duel's models get there
     (`func_80033DB0`, `func_80034830`).
   A vertex stored with no precise value is tagged as such and stays
   rounded.
3. Every other road (`GsSortPoly`, the scratchpad, the interpreter) falls
   back to the word's value, as in DuckStation's vertex cache: `rtp()` also
   records each vertex keyed by its screen word (x | y << 16), and a word
   two vertices of one frame round to with different precise values is not
   matched.
4. `DrawOTag` collects every word of the frame with its address
   (`Memories_GpuCollectAt`) and looks each up by address, then by value.
   Projections up to that point are the frame's, while the actual drawing
   happens later, after the next frame has begun projecting.
5. The matches go with the batch to the OpenGL pass (`SoftGpu_SetPrecise`,
   the recorder's `precise`, arena op `OP_PRECISE`). There `polygon()`
   places each vertex at its precise position. Below level 2, `DrawOTag`
   puts the word's own whole-pixel position there first, so only the depth
   is new.
6. A textured triangle whose three vertices all have their depths
   interpolates uv / w and 1 / w and divides back per pixel (flag 32). Every
   other triangle takes the path it took before, so with PGXP off the
   picture is identical.

**Effects:**

- The software GPU, VRAM and the game see nothing of it. The smoke cases
  give their hashes with `pgxp=1`, and nothing changes at 1x.
- In the 3D Monsters duel about 300 of a frame's 1,400 words are precise
  vertices, which is most of the 3D, nearly all of them by address. The
  rest is 2D drawn without the GTE. `MEMORIES_TRACE=frames` logs the counts
  (by address, by value). The main menu and Options come out identical
  with PGXP on: their 2D is not projected.
- One limit: vertices `GsSortPoly` moves by its offsets no longer match
  their word and stay rounded.

### Deterministic PC checks

`make check-pc` rebuilds the native game and portable C tests, runs every
`pc_*` CTest, then boots the game headless once per case in
`tests/pc/smoke/`. It compares PPM hashes for the title at frame 900, the
main menu after one cursor move (4:3 and widescreen), Options, and the first
campaign duel with both code mods on (a 3D Monsters model standing on a
face-up card at frame 6760; the field turned by the hand camera's L1 at 6560).
Options is played again with guest RAM executable (`options-exec-guest`,
`MEMORIES_TEST_EXEC_GUEST=1`, as a machine without DEP): the same frame, so
the branch thunks carried every guest call. A case's `environment` sets
variables for it alone, and its `expect_output` must appear in the game's
output. That variable is read only by builds that are not releases
(`MEMORIES_TEST_HOOKS`, which `build_game32.py` defines without
`--release`): the runner skips the case, saying so, for an executable that
does not contain the variable's name, such as the release `package.py` smokes.
Each run works in a folder of its own, `tmp/pc/smoke/run-XXXXXXXX` (printed
at the start), removed when every case passes and kept with the differing
image when one fails: worktrees share `tmp/` through a junction, and two runs
at once used to overwrite each other's `tmp/pc/smoke/<case>.ppm`. After an
intentional rendering change, inspect those images and update the fixtures
with `python3 tools/pc/smoke.py --record`; immediately run the normal command
twice before committing new hashes.

The smoke runner clears other `MEMORIES_*` switches (except a caller-supplied
`MEMORIES_DISC`) and gives each case its own settings file and a new user
folder (`<case>.user` in the run's folder, so the player's memory cards take no part),
no gamepad and no audio device. A case's `settings` fill that file
(`"aspect": 2`, `"mod.3d-monsters": 1`). It still requires the private disc
image and the native build prerequisites described above. Each boot gets
120 s; `MEMORIES_SMOKE_TIMEOUT=<seconds>` raises it where the headless game
runs slower.

A run that is headless, uncapped (`MEMORIES_SPEED=-1`) and dumps a frame is
deterministic: the same input gives the same frame on Linux, on the Windows
build (under Wine), and with the machine under load. Its game time moves only
where the game waits: `Platform_WaitVBlank` steps it 1 ms at a time to the next
VBlank, and a VSync that only reads the count steps it 1 ms (movie playback
polls that way). The host timer steps it only after a second without a wait,
so a spinning loop cannot hang. Driven by the host timer, as it used to be, a
frame that took longer to compute got more ticks, so more disc sectors, and the
Linux and Windows builds dealt different hands in the first duel. It is also
fast: frame 1100 in about 3 s on Linux.

Tests that need files make them with `scratch_dir()` (`tests/pc/scratch.h`):
`memories-<kind>-p<pid>-XXXXXX` in TMPDIR/TEMP, removed at exit and on a
failed `assert` (and on SIGINT/SIGTERM off Windows). What a test cannot remove
itself (killed by a timeout, or a file it still holds open, which Windows will
not delete: the disc image, the log) is removed by the next `scratch_dir()` of
the same kind and by `pc_scratch_sweep`, a CTest cleanup fixture every test
requires, so it runs after them even under `-R`; it lists what it removed.
The sweep takes only a kind listed in `scratch_kinds()` (a new kind goes
there) whose process has exited (a PID that does not fit or cannot be queried
counts as alive), and the untagged `memories-<kind>-XXXXXX` of older builds
once they are a day old. It never goes through a link: a symbolic link or
junction is unlinked, or left if that fails (a read-only junction), and never
entered; `pc_scratch_rules` checks all of this on decoys, junctions included.
Off Windows it only sweeps entries the current user owns. PIDs are only
meaningful on one host, and the checks and the removal are not atomic: use a
temporary folder of your own (the default TEMP on Windows, a private TMPDIR
rather than a shared /tmp elsewhere), and do not share one between
containers, or between Wine and Windows, while tests run. The disc
test's 1 GB capacity image is sparse on NTFS as it is on Linux. The game
itself makes nothing in the temporary folder.

### AI thinking time

The opponent's interpreter (`AiScript_Run`) yields to the next frame once
`VSync(1)` reports 240 lines. On the console a heavy decision therefore spans
several frames. Natively it rarely spans more than one, and in a deterministic
run the 1 ms step per `VSync(1)` makes the interpreter yield after about 17
commands. The decision would change only if other code drew from `rand()` in
those frames, between two of the AI's own draws.

`MEMORIES_AI_TRACE=<file>` logs every frame, every `VSync(1)` made by
`AiScript_Run` (the command's script offset and the opponent id) and the
caller of every `rand()`. `MEMORIES_AI_YIELD=N` makes every Nth of those
queries report a full frame, so the interpreter yields there as a slow machine
would. `python3 tools/pc/ai_trace_check.py <traces> --listing <dir>` groups
the trace into decisions and lists the `rand()` callers. It also lists the
ones that drew while a decision was in progress. With `--listing` (the output
of upstream's
`ai_script_disasm.py`, from
[krystalgamer/memories-decomp#6006](https://github.com/krystalgamer/memories-decomp/pull/6006)),
it checks every logged offset against the disassembly.

Measured on 2026-09-24: 16 deterministic sessions of 60,000 frames of the
`duel-hand-camera` case, driven by random button presses. They covered 943
decisions against Simon Muran and Teana and 156,234 commands. Four sessions
left the interpreter alone. Nine forced a yield after every command (up to
281 frames per decision), three of them with the code mods off. Three forced
one after every eighth command. Results:

- No code but the AI's own `AiScript_JumpRandom` drew from `rand()` while a
  decision was in progress. The AI's rolls are the same consecutive draws
  whatever the frame count, so a faster machine does not change them.
- The other duel-time callers (`DuelScene_UpdateBattle`'s shake, the result
  screen, the shuffle) run between decisions, for a fixed number of frames.
- Every logged offset is an instruction start of the disassembly: 489 of the
  hand script's 1314 and 296 of the field script's 795 were reached.

### Save states

F1, F2 and F4 pick those slots (shown in the File menu), while slot 3 is
available from File; **F5 saves and F7 loads**. Slots
are `states/slot<N>.state` in the user directory (`MEMORIES_STATE_DIR` moves them), about
4 MiB each. `./build-pc.sh load [slot]` or `MEMORIES_LOAD_STATE=<slot or
path>` starts from a state: the process boots for 30 frames so every
subsystem is initialized, then resumes the state (under a second).
`MEMORIES_SAVE_STATE="<frame>:<path>"` saves from a script, for headless work.
On this machine `slot1.state` is the "Run away / Keep listening" choice at the
end of the opening scene and `slot2.state` is the town map with the cursor on
the palace. Both were regenerated after the game-source guards below, which
moved code; older copies no longer resume.

**States survive a rebuild of the native side**, which is the point: reach a
stub, implement it, rebuild, load. Checked by shifting every native address
with a rebuild and loading an older state: the frame 420 frames later was
bit-identical. How (details in `src/pc/guest/state.h`):

- A state is only taken or resumed at a `VSync(0)` called from game code.
  `VSync` is a small assembly entry (`guest/state_i386.S`) that records the
  caller's callee-saved registers and return-address slot; resuming is
  returning from that call.
- The build collects every game object's code and variables into
  `game_text/rodata/data/bss` (and the `ovl_<module>_*` sections) and links
  them at fixed addresses (`FIXED_SECTIONS` in `tools/pc/build_game32.py`);
  the game runs on a stack mapped at `0xB0000000` on every system (Linux
  used `0x70000000` up to v0.2.0; an Android app has ART's boot image
  there). Return addresses and pointers inside a state therefore mean the
  same in the next build. A `system` chunk names the system whose build
  made the state (the code's layout is that compiler's), and a state from
  another system is refused; states from before the chunk tell by their
  stack, and an old Linux state (stack at `0x70000000`) is refused with its
  own message. No format version bump: a Windows state from v0.2.0 still
  loads (its stack was already there) with v0.2.0's `symbols/<build id>.txt`
  beside the new executable, as any state from another build needs, and an
  older Linux one is refused.
- Stored: guest RAM, scratchpad, those sections, the game stack above the
  call, and one self-described chunk per native subsystem (`*_State`
  functions: soft GPU, SPU, LIBSPU, LIBDS including buffered movie frames,
  LIBETC, LIBGPU, LIBGTE, MDEC, VBlank count), and the game's random seed
  (chunk `rng`: `rand` is the native `Memories_Rand`, whose seed sits in no
  game section; without it a state loaded in a running game dealt other
  cards, shuffles and CPU choices than the game that saved it: the same
  pack bought twice from one state, loaded in place in between, dealt
  other cards; a state without the chunk loads as before, with the seed
  left as it is), and the mods' own rand seed (chunk `mod-rng`, the one
  sequence `mod_libc.c` gives every mod). A chunk whose layout changed is
  reported and skipped, leaving that subsystem as it is. Nothing native is
  stored by address; timers, the disc file, the window and the audio device
  belong to the process. The exception is text the port compiles (a
  language, a translation mod, the shop's menu): the game's text boxes keep
  pointers into it in guest RAM, so it lives at a fixed address, `0x9C000000`
  (`translation.c`, `arena_take`), laid out in compile order, and the same
  language and mods lay it out the same at every launch. On the heap it
  moved with anything that changed the heap (the length of a folder name),
  and a state saved in a dialogue crashed in `TextBox_BuildStep` on some
  loads (FR→FR 3/10, pt-BR 3/10); 10/10 now. States saved before that with
  a language or translation on keep the old heap addresses and may still
  crash; English (US) states never held any.
- **A loaded state goes on frame for frame as the game that saved it**
  (`tools/pc/test_state_resume.py`: a second process loads the state and
  plays the same input; 2000 frames on the campaign map, twice, and through
  the opening movie, identical). Three things were missing until then:
  - the clock's phase, in chunks of their own so that older states keep
    loading the old ones: `platform-clock` (how far off the next VBlank is),
    `libetc-clock` (the sound driver's next tick from the last clock tick)
    and `libds-clock` (a stream's next sector). Kept as distances, since
    the clock's microseconds are the process's. Under the virtual clock a
    frame gets the 1 ms ticks up to the next VBlank, 16 or 17 as the phase
    falls, and each reads disc sectors: with the loading process's phase a
    file on the campaign map finished loading a frame apart and the map
    parted from the saving game some 180 or 350 frames on, depending on
    where it was saved, in the same process as in a new one;
  - the end of the `VSync(0)` the state resumes in (`LibEtc_StateResumed`):
    states are taken before `last_vsync` moves on to the VBlank just waited
    for, so after a load `VSync(1)` reported a frame already spent, and a
    CPU duelist thinking when the state was taken (`AiScript_Run` thinks
    until `VSync(1)` reaches 240 lines) stopped after one step and played a
    frame late (the replay `state-load-cpu`). `VSync(0)` also returns the
    fields that passed, not one;
  - on Windows, the game's small-data variables: `section(".sdata")` and
    `(".sbss")` in the sources (the movie's decode slot `D_8009B066` and 90
    others) stayed in the host's own sections, outside `game_data`, so no
    state held them; a state loaded in the opening movie decoded into the
    wrong one of its two buffers from then on. `build_game32.py` now puts
    them in `game_data` after everything else (`$n`), where no variable
    moves, and a variables chunk shorter than the section (a state from
    before) restores the part it has. The other way round, an older build says the
    game's variables "do not match this build" and leaves them out: the
    state loads, but play does not go on as saved.
  Not reproduced bit for bit: the SPU mixer's state (`spu`, `libspu`) and the
  sound driver's work area, which the mixer thread updates in real time (as
  between any two runs).
- **A state loads only in the language it was made in** (Game > Language),
  **and only with the same compiled text at the same place.** The
  `language` chunk (32 bytes, `LanguageChunk` in `state.c`) holds the
  language's code (`en-us`, `fr`...), a version (1), and the text's layout
  as `Text_Layout` gives it after `Text_Build`: the region's address (0
  when the text stayed on the heap), the bytes the startup text takes, and
  a CRC-32 of it unit by unit (the shop's menu, compiled later from that
  text, is left out). Another language, other text (a pack, a mod's `.txt`
  or the port's own words edited, none of which the `mod-set` signature
  covers: 23 letters added to one pt-BR string made a state reopen on
  another line) or text that was on the heap is refused. A later version
  appends to the chunk. No chunk is English (US); the language is not in
  the `mod-set` signature, which would refuse every state saved before it.
  A state with no layout (no chunk, or a chunk of the code alone: 16
  bytes, version 0, from the build just before the layout) loads only
  while no text is compiled now (English US with no translation mod), so
  English states from older versions load as before. With text compiled
  it is refused: a 0.1.2 state made with a translation mod points into
  that version's heap, and French, Spanish and Italian version-0 states
  from a build whose PAL text differed reopened on the wrong lines.
- **A refused load says why on screen**: every refusal in `load()` (other
  language or text, other mods, another build, another system, not a state,
  no such slot) goes to stderr and to a notice over the picture
  (`Menu_ShowNotice`, "Save state not loaded"); F7 used to do nothing
  visible on Windows.
- Game data words the linker relocated (pointers to native functions) are
  taken from the running build when the game never changed them; the file
  keeps the startup image of that data to tell.
- **States are carried between builds by relocation.** A change to game
  sources moves game code, and *any* rebuild can move native routines the
  game holds pointers to (the town map keeps the addresses of the HMD drivers
  `GsU_*` in its model data; a state taken there broke as soon as `libgs.c`
  grew). Every build files all of its functions and the game objects'
  variables in `tmp/pc/game32/symbols/<build id>.txt`, the id being the
  table's hash (also written to `tmp/pc/game32/buildid`, which the runtime
  reads); the state header carries the id (format version 3; version 1
  carried the game-source fingerprint, version 2 the build id of a game
  compiled without the indirect-branch thunks).
  A state from another build is
  rewritten by name when loaded: function starts wherever callbacks live
  (guest RAM, game variables, the callback part of the LIBDS/LIBETC/MDEC
  chunks) and any address inside a function on the stack. The load is refused,
  with the reason, if a function that was running has changed size, if a game
  variable moved, or if either symbol table is missing (keep the `symbols/`
  directory; a table for a lost build can be regenerated by building those
  sources with `--build <other dir>`). First use: the three states saved
  before the pointer-sign fix below load in the fixed build (about 425
  addresses moved each). A new native static that the game depends on still
  needs a field in its subsystem's `*_State`.
- **States from before the indirect-branch thunks are refused** ("made by an
  older version of the game; save states don't carry over across this
  update", on screen). The thunk flags changed the code of every game
  function (a call through a pointer became a call to a thunk, clang's
  switch tables compare trees), so the return addresses on a saved stack
  point into code that is no longer there. The relocation above could not
  see it: PE symbols carry no sizes (a function runs to the next symbol), a
  function whose extent stayed the same passed for unchanged, and a state
  from the build before resumed a byte off, in the middle of an instruction
  (`Graphics_SyncFrame`'s call to `VSync` returns at `+0xd2` now, not
  `+0xd1`). Such states carry format version 1 or 2; this build writes 3.
  Memory card saves are not affected.

### Saves

The port does not use memory cards for saving. Every load and save the game
asks for goes through `MemCardDialog_Request` / `MemCardDialog_Poll`
(`src/game/mem_card_dialog_runtime.c`); under `MEMORIES_PC` those hand the
request to the save slot menu (`src/pc/saves/save_menu.c`) instead of the
card dialog. The menu is drawn in the overlay (`debug/hud.c` calls
`SaveMenu_Draw`), is driven by the game pad (Cross picks, Circle backs out,
Up/Down move, Left/Right in the overwrite prompt), and returns the outcome
the card dialog would have (1 done, 2 failed, 3 cancelled), so every caller
is unchanged. What it covers:

| Dialog step | Caller | Slot menu |
|---|---|---|
| `0` load | title LOAD | pick a slot with a save; empty and damaged slots cannot be picked |
| `1` load, unprompted | 2P DUEL and TRADE, once per player | "Player 1/2: choose a save"; player 2 cannot pick player 1's slot |
| `2` save | SAVE in the main menu, the campaign's save prompt, the completion save after the credits | pick any slot; an occupied one asks "Overwrite it?", defaulting to Overwrite only when it holds the game being saved as it was last loaded or saved (same duelist, the previous sequence number) and to Cancel otherwise, with a line saying it is another duelist's, an earlier save of this game or further along |
| `4` trade write-back | TRADE | no menu: writes both traded saves back to the slots they were loaded from, after checking the duelist code as the card dialog did |

The side effects the card dialog kept on success are kept too:
`gSaveDataSequence` goes up after a save and `D_8009B3D4` is cleared after a
save or a prompted load.

A slot file (`src/pc/saves/save_slots.c`) is exactly the 8 KiB block the
game writes to a card: the 0x200-byte "SC" header with icon and title, the
0x680-byte state and its duplicate, zeros after. A slot can therefore be put
back onto a card image with any memory card manager. In the padding after
the duplicate (0xF00) the port keeps a token, `YFMSLOT\1` and a number drawn
afresh at each save; what the save holds of the cards mods add
(`cards/<duelist>.txt`, notes/more-cards.md) is filed under that token, so
two slots of one duelist never share it. A slot is written to
`slotNN.sav.partial`, flushed to the disk and renamed over the old file, so
a failed write or a power cut keeps the previous save (on Windows the
rename writes through, and is retried for a second while another program,
a cloud sync say, holds the file). The list is read from the files again
before a pick, so a list brought back by a save state cannot save over a
slot without asking. When the first copy fails `SaveData_ValidateIntegrity`
the duplicate is used; when both fail the slot shows as damaged. The menu
lists the player name, starchips, cards owned (chest plus deck), wins and
losses, and the file's modification time.

Trade write-back checks both destination duelists before writing either
slot. It updates both copies in each slot together, preserving the valid
copy's progress outside the traded card data. Each slot replacement is
atomic, and when the second write fails the first slot is put back as it
was, so the trade happens to both saves or to neither.

The first time the saves are used, the save on each old memory card image
(`memcard1.mcd`, `memcard2.mcd`) is copied into slot 1 and 2, never over a
slot that is already there. `saves/.cards-imported` marks that done; a card
that could not be read is tried again at the next load or save.

The first time the `saves` folder is created, the game's save
(`BASLUS-01411-YUGIOH`) on `memcard1.mcd` and `memcard2.mcd` is copied into
slots 1 and 2. The card images are only read. `pc_save_slots` tests the slot
files and the import. `pc_save_menu` covers save/load/cancel, overwrite
defaults, pair selection, trade validation and backup consistency. The
menu's own state is a save-state chunk (`save-menu`), so a state taken with
the menu open resumes in it; each poll supplies the current build's
integrity callback, including after loading a state in a fresh process.

**Saves from an emulator or a PS1 memory card** (issue #299): a memory card
file put in the `saves` folder is imported each time the slot menu opens
(`SaveSlots_ImportFolder`, called from the menu's `start`). Taken by
extension, in any case: raw card images `.mcr` `.mcd` `.mc` `.srm` `.mem`
`.ddf` `.vm1` (ePSXe, PCSX, mednafen, DuckStation, RetroArch, PCSX2), and
`.gme` (DexDrive), `.vgs`, `.vmp` (PSP), and the single saves `.mcs` `.psx`
`.ps1` `.mcb` `.psv` (MemcardRex, Action Replay, PS3). The format is not
parsed: each keeps the game's one 8 KiB block whole after a header of its
own, so `SaveSlots_FindSave` looks for a block that opens with `SC` and
holds a copy that passes `SaveData_ValidateIntegrity` and has a deck (an
all-zero state passes the check, and other games' blocks may hold zeros
there). Each save found goes into the first empty slot through
`SaveSlots_WriteFile`, as the game's own save would, so loading it is the
ordinary slot load with the game's check. A save already in a slot (the
same state, byte for byte) is not copied again. When every save in a file
is in a slot the file is renamed `<name>.imported` (`<name>.2.imported`
...), so it is imported once; with no empty slot it stays and the menu says
how many did not fit. The menu shows one message for the lot ("Imported the
save in epsxe000.mcr into slot 2.", "No save of this game found in
blank.mcd.", ...), Cross closes it. Emulator save states (`.sstate`,
`.ss0`...) are not memory cards and are not read. Only the USA release's
saves were checked; another region's loads if it passes the same check.
`pc_save_slots` tests the finder on each container shape and the folder
import; `pc_save_menu` the menu's message and load. Checked in game
(Linux, and the Windows build under Wine): a 128 KiB `.mcr` with another
game's save in block 1 and the game's in block 2, a `.gme`, a `.mcs` and a
`.psv` each, put in a fresh user folder; LOAD on the title imported it to
slot 1 and loaded it with the save's deck, 711 kinds of cards and 25
starchips.

Checked on Linux and under Wine: LOAD on the title imports the card's save
and loads it, SAVE writes an empty slot at once and asks before overwriting
slot 1, and an overwrite replaces the file on the Windows build.

### Memory cards

Nothing on the normal paths reaches LIBMCRD any more (see "Saves" above).
`sdk/libmcrd.c` implements LIBMCRD over standard 128 KiB raw card images
(`.mcd`/`.mcr`, the format emulators use, so saves can be exchanged with them).
Slot 1 is `memcard1.mcd` in the user directory, created formatted when
missing; slot 2 is `memcard2.mcd` beside it and exists only if the file does; `MEMORIES_MEMCARD1/2`
name other files. The image is re-read on every command and written through a
temporary file. Commands complete after the time the hardware would take
(about one 128-byte frame per VBlank), so the game's "accessing memory card"
messages stay up. The first `MemCardAccept` of a card reports `McErrNewCard`,
as after an insertion. Checked: SAVE at the card shop writes
`BASLUS-01411-YUGIOH` (one block, valid "SC" header, icon and title,
directory checksums), a second SAVE asks to overwrite, and LOAD from the title
menu returns to the shop. The low-level `_card_*`/`InitCARD` path in
`mem_card_driver.c` is still stubbed; nothing reached so far uses it.

Native pieces (all under `src/pc/`):

| Area | File | Notes |
|---|---|---|
| GPU | `render/soft_gpu.c` | Software rasterizer: flat/Gouraud/textured polygons, sprites, lines, fills, VRAM transfers, 4/8/15-bit textures, texture window, four blend modes, mask bits, dithering. `pc_soft_gpu` checks the fill rule, CLUT path, clipping and wraparound. Replaces PSY-Z for the 32-bit build (no 32-bit SDL installed here) |
| Mods | `src/pc/mods/mods.c`, `mods/3d-monsters/field_models.c` | The mod system (`notes/modding.md`) and **3D Monsters**, described above: the duel field's face-up monsters as animated models, on their own arenas and software-GPU texture banks. Off by default; nothing in it runs while it is off |
| Guardian Stars | `cards/stars.c`, `cards/star_icons.c`, `cards/stars_duel.c` | A mod's `guardian_stars` (`notes/modding.md`): the matchup table `Duel_CalcGuardianStarMatchup` asks first (nothing decided without a mod, so the disc's arithmetic runs), the names Text_Resolve gives at `0x8318`-`0x8326`, stars 11-15, the summon choice and one-star cards, and the icons, made from the mod's PNGs in software-GPU texture bank 14 (3D Monsters uses 1-12, the added glyphs 15) and drawn by func_80035E20 for the icon entries func_80037DA4 marks, and by the battle's star effect (0xE) through `Memories_DuelEffectControl` |
| Menu bar | `platform/menu_x11.c` | **File > Exit**, **Audio > Volume** and **Mods**, one checked item per extra (a 0-100 slider: drag it, click the track, or use the wheel over it). Drawn with plain Xlib, since the port has no toolkit; the window is `Menu_Height()` (22 px) taller than the picture and the picture sits below it. Labels use an X core font, falling back to a small built-in glyph table because a server started under Wayland often has no core fonts. The volume is kept in `settings.txt` in the user directory (see `MEMORIES_SETTINGS`) and applied through `Spu_SetOutputVolume`, which is the port's own control and deliberately outside save states. While a menu is open it owns every mouse event, including the wheel: otherwise the wheel stepped the game's cursor behind the menu and played its sound |
| Window/input | `platform/x11.c` | Plain Xlib. The 59.94 Hz VBlank is a `SIGALRM` tick on the main thread, standing in for the interrupt, so the game's busy-waits on VBlank counters work unchanged. Game units are built `-O0` so those non-volatile polls are not hoisted. The frame and the menu bar are composed in an offscreen pixmap and reach the window in one `XCopyArea`: an open menu hangs over the picture, so drawing both straight to the window made the menu flash once a frame |
| LIBETC/pads | `sdk/libetc.c` | Callbacks, `VSync` (presents, then waits), critical sections that defer the tick, BIOS pad buffers |
| LIBGPU | `sdk/libgpu.c` | Environments, `DrawOTag` through `Memories_GpuCollect`, image transfers. `DrawOTag` snapshots the list and it is rasterized at `DrawSync` or before the next VRAM access, where the hardware would have finished it. Drawing inside `DrawOTag` put ~8 ms between `VSync` and `Input_UpdatePads`; whenever a second VBlank got in there the pad code published each press twice (two cursor steps, two sounds). `DrawSync` also runs what the cooperative clock owes, so a frame's overrun is counted before `Graphics_SyncFrame` reads the game's VBlank counter (see "VSync(0)" under the clock) |
| LIBGS | `sdk/libgs.c` | Ported from the resident assembly against the library's guest globals (`GsDRAWENV` `0x800FE048`, `GsDISPENV` `0x800FE0A8`, ...): graph init, display-buffer swap, OT clear/sort, `GsSortSprite`/`FastSprite`/`FlipSprite`/`Poly`/`BoxFill` |
| LIBDS/LIBCD | `sdk/libds.c` | ISO9660 lookup and sector delivery from the disc image on the VBlank tick (8 sectors per tick; faster than hardware). Resolved LBAs equal `disc_layout.json`. XA "play" only advances the head |
| SPU | `audio/spu.c`, `sdk/libspu.c`, `platform/audio_alsa.c` | 24 ADPCM voices, hardware ADSR, pitch, volumes, Gaussian interpolation, CD/XA input, mixed on an ALSA thread at 44.1 kHz. The menu's output volume is a separate gain the mixer walks to its target over about 36 ms, because stepping it mid-waveform is an audible click and dragging the slider made a burst of them. No reverb, noise, sweeps or pitch modulation. Key on/off cross threads as atomic bit sets (a key-off after a still-pending key-on is applied after it); no locks where a signal handler runs. `SpuSetVoiceAttr` follows the decompiled library (`tmp/port-research/psyz/decomp/src/libspu/sr_sv.c`): pitch, then sample note, then note, so a note overrides a pitch in the same call, with the library's integer note-to-pitch; ADSR modes are written only with their rates. The sound-effect voice sends mask `0xFFFF` with pitch `0x1000` and note `0x2400` against sample note `0x3C00`; applying the pitch last played every effect two octaves high |
| Modules | `guest/modules.c`, `MODULES` in `tools/pc/build_game32.py` | Modules built for the shared `0x80168000` bank are all linked in. Clashing symbols become `<module>__<name>`, their variables move to `ovl_<module>_data/bss` and are restored from a startup copy when the module's first sector lands on the bank (`CdGetSector` reports loader writes), and guest-address calls are resolved against the identifier word at the start of the loaded image. Data the module's C leaves undefined is pinned to the guest addresses the loader fills. Linked so far: `password` (`0x15`, also name entry) |
| Interrupts | `sdk/libetc.c`, `platform/x11.c` | A 1 kHz `SIGALRM` drives VBlank (59.94 Hz), root counter 2 (the sequencer: `SetRCnt` without the system-clock flag selects sysclk/8, so `0xE000` is 73.8 Hz), disc delivery and MDEC completion. Critical sections defer them |
| XA/STR/MDEC | `sdk/libds.c`, `sdk/libpress.c` | XA ADPCM sectors decode into the SPU's CD input; streaming reads run at the drive's real rate (75/150 sectors per second). `St*` assembles STR frames in host slots; `DecDCTvlc2` emits genuine MDEC run-level words (bitstream v2), and the software MDEC fills each `DecDCTout` strip and then runs the game's callback. Floating-point IDCT; 15- and 24-bit output; 24-bit display mode is presented |
| LIBGTE | `sdk/libgte.c` | Register setters, and ports of the resident routines: `rsin`/`rcos` and the matrix builders read the library's own tables from the resident image (`0x80094938` quarter sine, `0x80095638` sin/cos pairs, `0x800951A8` square roots); `RotMatrix`, `RotMatrix_gte`, `RotMatrixZYX_gte`, `RotMatrixYXZ_gte` (each keeps the retail rounding: some negate before the shift, the GTE ones after), `MulMatrix`/`MulMatrix2`/`ApplyMatrixLV` issued as the same MVMVA commands, `TransposeMatrix`, `SquareRoot0`, `ratan2` (table at `0x80099638`), `RotAverage3/4`, `RotAverageNclip3/4` and `_nom`, `AverageZ3`, `NormalClip`, `RotTrans`, `RotTransSV`, `RotTransPersN`, `RotColorDpq`, `NormalColorCol`, and `DivideFT4` with its recursive subdivider and packet emitter (`RCpolyFT4A`, `func_80089910`) over the caller's `DIVPOLYGON4` work area: the card viewer draws the large card with it. The earlier `RotMatrix` was the Z-Y-X formula under the wrong name |
| LIBGS units | `sdk/libgs_unit.c` | The HMD path the town map uses: `GsMapUnit`, `GsMapCoordUnit`, `GsScanUnit`, `GsSortUnit` (primitive drivers are function pointers the game installs), the library's null and image-upload drivers, `GsGetLwUnit`/`GsGetLsUnit`/`GsGetLwsUnit` with their per-frame coordinate cache, `GsMulCoord2/3`, `GsSetRefView2`, `GsSetLightMatrix`, `GsSetFlatLight`, `GsLinkAnim`, `GsScanAnim`; `GsSortLine`/`GsSortGLine` are in `libgs.c`. `tests`: a scratch harness checked that the reference point lands on the view axis at the right distance and `ApplyMatrixLV` against 64-bit math; not yet a CTest |
| Model drivers | `overrides/model_polygon_drivers.c` | The 61 hand-written GTE routines at `0x800612C0`-`0x8006ADE8` are HMD primitive drivers, and one algorithm under switches: triangle/quad x flat/Gouraud, back-face culled (`0x0020xxxx`) or both-sided (`0x0030xxxx`), plain or tiled (`0x02xx`: each polygon wrapped in its texture-window word and a reset), a second bank that forces semi-transparency, a shared-vertex bank (`0x012x/0x013x`) fed by a pre-calculation pass at `0x80067220`, and twelve outline drivers. Record layouts, the lighting modes of `D_8009AFE4`, the color cache and the translucent second pass are described at the top of the file. Read in full for each shape and by diff for each variant; the outline drivers and most variants have not been seen running yet |
| Null page | `guest/image.c` | Retail code dereferences null pointers that land in kernel RAM on the console (`CardList_CreateSlotTextBox` clears a flag through `box->field_28` one call before that object is created; it made BUILD DECK fault). A faulting access below 64 KiB is redirected: the handler decodes the instruction's base register, points it at a mapping of guest RAM's first 64 KiB, single-steps (trap flag) and restores the register. Each site is reported once on stderr, which makes these bugs visible instead of fatal |
| Guest calls | `guest/branch_thunks.c`, `guest/image.c` | Every unit's indirect calls and jumps go through `__x86_indirect_thunk_<reg>`; a MIPS address stored in the data image resolves to the native function via the generated `Memories_FunctionMap` (or to the MIPS interpreter). Guest RAM is also non-executable, so a call that escaped the thunks faults and the handler redirects it the same way where DEP is on |
| Overrides | `overlays/boot_check.c`, `sdk/deferred.c` | The boot package's console-modification check has no source and is passed. `GsSetFlatLight` and the debug font log once and do nothing |

A native definition with a game function's name replaces it (the driver
weakens the game's symbol). Sources that still use address-based names
(`func_800434F4`) are aliased to the renamed C definition, as the PS1 link
does. `src/overlays/main_menu` is linked in because its load address
(`0x80180000`) is private.

Software GTE fix found through the map camera: MVMVA and the light-color
stage take IR1-IR3 as input and write them row by row; the inputs are now
latched first (`pc_gte` has the regression case). Before it, every
`ApplyMatrixLV` translation and every lit color was wrong in rows 2 and 3.

Pointer-sign tests: retail tells a callback from a small number by the sign,
every console address being negative. `func_80041F90`
(`display_object_projection_checks.c`) did `if (cb < 0) cb(obj, otz)` on
`DisplayObject.field_10`; host code addresses are positive, so the callback
that swaps a card to its back texture never ran and the card viewer showed the
blank front frame through the first half of its turn. Under `MEMORIES_PC` the
test is `(u32)cb >= 0x10000`. A scan for sign and top-bit tests next to
indirect calls found only this one; tests on data pointers may still exist.

Calls that rely on MIPS argument registers passing through: the matching
sources write three calls without arguments because retail leaves the
caller's `$a0`-`$a3` in place (`func_80056250` -> `func_8004CB0C`,
`func_80023D08` -> `func_8002348C`, `FreeDuel_UpdateSparkle` ->
`DisplayObject_ReleaseIfPresent`; the last confirmed from the matched
disassembly). Under `MEMORIES_PC` they pass the arguments; `make match` and
`make match-overlays` still reproduce the retail hashes. A scan for calls
with empty parentheses to functions defined with parameters found only these.

The overworld module references six resident addresses that fall inside
functions (`func_800158C8`, ...): its `alternate_*` code was linked against
another build of the executable and is dead in retail. They stay stubs.

Symbols identified while porting are recorded with their evidence in
`notes/pc-port-symbol-evidence.csv`, in the schema of
`notes/semantic-symbol-map.csv`. It is a staging file: the main map is
enforced by `tools/project/apply_semantic_names.py --check` and applying it
renames sources, so rows move there (and get applied, followed by `make
match`) as a separate step. Add a row whenever a port establishes what an
address is; say what was read and what was observed working, not just the
name.

Shared-source guards added for the host compiler (GCC 16):
`src/game/graphics_frame.h` / `graphics_frame_buffer.h` / `main_init.c` (an
array of incomplete type is declared after the layout instead),
`src/psyq/stdarg.h` (compiler builtins), `src/psyq/inline_c.h` (native GTE
macros). The build also defines `_LANGUAGE_C`/`LANGUAGE_C`, which the MIPS
front end predefined, and uses `-fpermissive` for GCC 2.8.1-era pointer
conversions.

## Sharing a build

```sh
python3 tools/pc/package.py        # dist/yfm-redecomp-<date>-<commit>-{windows.zip,windows-x64.zip,linux.tar.gz}
```

Builds all three (32-bit Windows, 64-bit Windows, Linux), smoke tests each,
and packs each as a folder a player unpacks
and runs: the executable, the shipped mods, the mod SDK, this build's symbol
table, an empty `game/` for their own `.bin`, and `tools/pc/release/README.txt`.
Nothing from the disc goes in. A missing disc image is reported in a message
box naming the folder to put it in. Crash and hang reports, minidumps and
menu frame dumps go to `reports/` in the user directory when the game is not
run from a checkout (`Crash_ReportDir`; `tmp/pc` in one).

The 64-bit Windows archive has the data mods only and no mod SDK ("64-bit
Windows" below). Where its compiler is missing (x86_64-w64-mingw32-clang 21
or later), `package.py` with no arguments skips it with a message and packs
the other two; `package.py windows-x64` stops instead.

### Crash reports

The executable runs the game as its own child and stays as its monitor
(`src/pc/debug/monitor.c`), so whatever ends or freezes the game, a report
is written: `crash-<pid>.txt` or `hang-<pid>.txt` in `Crash_ReportDir`,
with a message box naming it (not when headless or scripted;
`MEMORIES_CRASH_DIALOG=0/1` decides). The two share a block of memory the
game writes and the monitor reads, so what the game knew survives however
it ended: facts (build and commit, OS or Wine version, CPU, memory, GPU and
driver, SDL video and audio drivers, every setting but the retired ones, the
applied mods, the user folder and whether it can be written to, with the
reason when not), the
runtime module last loaded, the frame and VBlank counts, and the last 128
lines of the log. The mods, state, memory card and duel model channels are
kept there even when not traced (`Log_Wanted`). The game's console output
passes through the monitor into `last-session.log` (the run before in
`previous-session.log`) and its last 200 lines into the report. The home
folder is written `~` in the monitor's part of a report.

Help > System info for bug reports shows the same facts with the build's
version (all but the settings line), puts them all on the clipboard (the X11
backend has none) and writes them to `system-info.txt` in the user folder,
for a report that has no crash behind it (`Monitor_Facts`, `menu.c`).

- A crash the game's handlers see (`crash.c`) is reported by them first,
  in the same file; the monitor adds its section after it. What they cannot
  see is the monitor's alone: a fail-fast, the out-of-memory killer, a
  stack Windows could not deliver an exception on, an unexplained exit.
  On Linux it adds `coredumpctl info` (a stack per thread) when
  systemd-coredump kept the core.
- A freeze: no VSync for `MEMORIES_WATCHDOG` + 1 seconds (5 + 1 by
  default; 30 before the first frame; never while paused, in a message
  box, or quitting). The monitor lists every thread with its registers and
  frames, taken from outside (ptrace and `/proc` on Linux, where the
  game's own watchdog in the tick handler cannot see a freeze of the tick
  itself; `GetThreadContext` on Windows) and writes a minidump on
  Windows. If frames come again the report says so; if the player closes
  the game, that too.
- Errors the game cannot go on from (an unimplemented routine, a bad
  ordering table, a MIPS overlay failure, a lost disc image) go through
  `Crash_ReportFatal` to `crash-<pid>.txt` before the exit. Windows gets
  `abort()` and the C runtime's invalid-parameter handler (logged and
  ignored, as MinGW's own did) too, and the exception code for the exit
  status of a crash.
- `MEMORIES_NO_MONITOR=1`, or a debugger, runs the game alone. On Windows a
  restart (mods window) is the monitor starting the game again; on Linux
  `execv` keeps the same process.

`MEMORIES_CRASH_TEST=<kind>@<frame>` fails on purpose
(`src/pc/debug/crash_test.c`: segv, thread, overflow, abort, fatal, kill,
hang, spin, deadlock, tickhang, slow, restart, null; and present, the segv
write inside the next present, which `crash_check.py` does not run), and
`tools/pc/crash_check.py [--windows]` runs every kind headless and checks
its report; all 13 pass on Linux and under Wine. Not yet seen on real
Windows: the message box, the minidump's size (Wine's is small), and the
job that ends the game with the monitor.

Every Linux build, the everyday one included, is the one that ships. A
program built against this machine's libraries would ask for its glibc (2.43
on Arch), so `build_game32.py` builds against Debian 11's i386 libraries, which
`tools/pc/build_linux_sysroot.py` fetches into `tmp/pc/linux-sysroot` (no root,
no container; each package is checked against the archive's SHA-256), with
SDL3 built against them into `tmp/pc/sdl-m32-portable`. Debian's 32-bit
start-up objects and libgcc are linked by name (`startfiles()`/`endfiles()`,
`-nostartfiles`), so the host's multilib files are never used or needed. The
executable asks for glibc 2.29 at most, links FreeType, fontconfig and libpng in, and needs only libc and the
32-bit GL driver from the system (and a 32-bit PulseAudio or ALSA library for
sound; SDL loads those at run time). Its SDL has X11 but not Wayland (Debian
11's is too old); it runs through XWayland, which is also the path that reaches
the real GPU (llvmpipe otherwise). The smoke frames match the old host build.

Only this build's symbol table ships, so a save state from an earlier release
will not carry over to a later one unless that release's table is added to
`symbols/` as well.

## Windows

The same driver builds `tmp/pc/game32/memories-pc.exe` on Windows 10/11 with
the SDL backend. Status (2026-09-22, Windows 11, i686 on x64): headless and
in the SDL/OpenGL window at 59.94 fps, with the menu bar, through the Konami
logo, movie, title, main menu, name entry and the opening story into the
deck (CHEST) screen. Audio output and the rest of the game are not checked
yet on Windows.

`play.bat` is the way in ("Play from a checkout" above). By hand, from any
shell with Python 3:

```sh
python tools/pc/build_game32.py       # fetches llvm-mingw, CMake, Ninja and the libraries the first time
tmp/pc/game32/memories-pc.exe
```

### From Linux

`./build-pc.sh` builds the Windows executable as well as the Linux one, so
every change is compiled for both (`MEMORIES_SKIP_WINDOWS=1` skips it). The
first run of `tools/pc/build_win32_deps.py` fetches the pinned llvm-mingw
release for Linux into `tmp/pc/llvm-mingw` (the same toolchain as on
Windows) and builds the libraries with it; `build_game32.py --target windows`
then writes `tmp/pc/win32/memories-pc.exe`, its mods and `SDL3.dll`,
beside the Linux build rather than over it. The Windows units are compiled
with `-gcodeview` and lld writes `memories-pc.pdb` beside the executable
(the release zip ships it): Visual Studio, WinDbg and profilers such as
Superluminal read symbols from a PDB, not from DWARF. Needs cmake, ninja and Wine to
run it:

```sh
./build-pc.sh run-windows               # play it under Wine (prefix tmp/pc/wine-prefix)
python3 tools/pc/smoke.py --windows     # the smoke frames, under Wine
```

The smoke frames under Wine match the Linux hashes. Wine is not Windows: it
presents through SDL's Direct3D renderer (its 32-bit OpenGL is not there),
and its clock reads leave the executable, which is how the tick came to be
serviced from `Platform_VSyncHeartbeat` (a `VSync(-1)` poll, as the movie's
wait for sectors, otherwise took ticks only by chance: 3.5 frames/s). Check
anything timing- or driver-sensitive on Windows itself. `SDL3.dll` is copied
beside the executable. Everything Windows-specific is behind `_WIN32`; the
Linux build is unchanged.

For play-testing, `tools/pc/run_debug_windows.bat` runs the game with
problem reporting on. It keeps a rolling state every 30 s
(`MEMORIES_AUTOSAVE=<seconds>`, slots `auto1`..`auto3` in
`tmp/pc/debug/states` through `MEMORIES_AUTOSAVE_DIR`; F5 states stay in the
user directory), traces in `tmp/pc/debug/trace.log` and the console in
`tmp/pc/debug/console.txt`. `tmp/pc/hang-*.txt` / `crash-*.txt` hold named
backtraces (see "Crash reports"): PE symbols have no sizes, so the build
sizes each function up to the next symbol, and addresses in a DLL are named
by module. The game's own hang watchdog runs on the clock thread
(`Win32_SetStallReporter`), so it also reports a main thread stuck in a
driver or a lock, which the tick cannot reach; a pause counts as alive.

What differs from Linux, and why:

- **Clock.** No signals: `platform/win32.c` runs a 1 kHz timer thread that
  suspends the main thread and, if it is executing code of the executable
  and does not hold SIGALRM, redirects it through an assembly trampoline
  that saves every register and the FPU/SSE state, runs the tick and returns
  to the interrupted instruction. Code outside the executable (C runtime,
  SDL, drivers) is never interrupted; a tick missed there is taken by
  `Win32_ServiceInterrupt` from `Platform_WaitVBlank`. `pc/compat/signal.h`
  maps `sigprocmask`/`pthread_sigmask` on SIGALRM to that hold flag.
- **Faults.** A vectored exception handler in `image.c` does what the
  SIGSEGV/SIGTRAP handlers do (guest-call redirect, low-address fixup); the
  guest-call redirect is the second net behind the branch thunks, since it
  needs DEP. The game works without DEP; where DEP is on it is a second
  safety net. A 32-bit process follows the system's DEP policy (only 64-bit
  processes always have DEP), so under `OptIn`, the default, `--nxcompat`
  turns it on. The game does not call `SetProcessDEPPolicy` for `OptOut`
  with the game excepted: a program switching its own DEP policy is what
  virus scanners' heuristics look for (0.1.4 was flagged), and the thunks
  make it unnecessary;
  32-bit processes on 64-bit Windows can report the single step as
  `STATUS_WX86_SINGLE_STEP`. Fatal exceptions raised in the executable are
  reported by `crash.c` through `Win32_SetCrashReporter`.
- **Physical RAM mirror.** Windows already occupies `0x10000..0x200000` when
  the program starts (process parameters, locale tables, the WoW64 stack),
  so the mirror cannot be mapped; `Memories_Resolve` returns the
  `0x80000000` alias for physical RAM addresses, and the fault handler sends
  any other access there through guest RAM (each site reported once).
- **Stacks.** The game stack is at `0xB0000000`, as on every system (32-bit
  Windows loads system DLLs around `0x70000000`; the mods keep `0x90000000`). `state.c` switches stacks with
  `Memories_ContextSwitch` (`state_i386.S`; Android uses it too, since bionic
  has no ucontext functions, while Linux keeps `swapcontext`) and moves the
  TEB's stack bounds,
  `DeallocationStack` and exception chain with it, as fibers do. A guard
  page 64 KiB above the game stack's bottom, below `DeallocationStack` so
  that Windows and Wine take it for a plain guard page and not stack growth,
  turns an overflow into a report; running off the end left no stack to
  deliver the exception on, and the process just ended. A context is the
  stack pointer of a suspended switch, with the registers kept on that
  stack; unlike a ucontext, it is gone once that stack is used again. So a
  state load enters the game through a switch too (`apply`, `resume_game`),
  which takes the service context again each time: resuming the startup
  one after `apply` had run over it crashed every second load of a session
  (EBP 0 at `Memories_StateRunGame`).
- **Icon.** The window and the taskbar show the game's memory card icon,
  the one its saves carry (Yugi, frame 0 of three), decoded from the save
  header template `gSaveData_aHeaderTemplate` of the game being played
  (`src/pc/platform/save_icon.c`) and pixel-doubled to 32, 48 and 64. On
  Windows the executable carries it too: `build_game32.py` (`exe_icon`)
  makes `icon.ico` from `game/SLUS_014.11` and links it in as a resource
  through windres. Nothing of the game's is in the repository; without the
  game files or windres, the executable has no icon.
- **Link (lld, PE).** C symbols carry a leading underscore; `asm("name")`
  labels are renamed to match. No GNU linker script: pins are absolute
  symbols from `guest_symbols.s`, and the game units' COMMON symbols for
  pinned names are turned into references, since lld would prefer the
  COMMON. Section renames edit the COFF headers (`rename_coff_sections`) and
  `__start_`/`__stop_` come from `$a`/`$z` marker sections. Overrides win by
  link order (`--allow-multiple-definition`, native objects first); any
  other duplicate definition is still an error. PE cannot place sections at
  chosen addresses, so the fixed game sections are not there: save states
  work within one build but are not carried across rebuilds.
- **Bitfield layout.** MinGW compilers lay bitfields out as MSVC does by
  default, where fields of different declared types do not share a unit:
  `GsOT_TAG` (`unsigned p:24; unsigned char num:8`) became 8 bytes and every
  LIBGS ordering table was walked with the wrong stride, so semi-transparent
  and shaded primitives were overwritten or lost (the title's lower
  gradient, the yellow selection bar). The build passes `-mno-ms-bitfields`
  to game and native units, and `libgte_extra.c` asserts the size.
- **Clock races.** Besides the fault race above, a redirect made while an
  exception is being delivered can be dropped by Windows; the tick then never
  runs. It is released wherever it shows: by the exception handler, which
  sees the redirect still marked while the registers it got are not at the
  tick's entry; by the clock thread, when the main thread's stack pointer is
  back above the slot it pushed; and by `Win32_ServiceInterrupt`, since a
  wait on the main thread is not the tick. Before this, the game froze in
  `Platform_WaitVBlank` after a few minutes of duelling. The game stack runs
  with an empty SEH chain, so the crash reporter also takes any fatal
  exception raised while the chain is empty, in a DLL too; a stack overflow
  is reported from a thread of its own, the faulting one having too little
  stack left. Crash and hang reports come with a minidump (`tmp/pc/*.dmp`,
  `lldb -c` reads it). Other threads' unhandled exceptions go through
  `SetUnhandledExceptionFilter`, and every exception nothing claimed is
  logged once per address.
- **Low accesses without a trap.** A plain 32-bit `mov` to or from a low
  address is carried out by the fault handler through guest RAM
  (`emulate_low_mov`) instead of rebase and single step: WoW64 mishandled
  the trap for `movl 0x4c(%eax),%eax` (destination = base register) in
  `func_800540B4` under the 3D Monsters mod, and the process died in the
  exception dispatcher.
- **Mods.** A code mod is the same ELF object on Windows as on Linux, loaded
  by the game's own loader (`notes/modding.md`), so there is no DLL and no
  export table. `mods.c` reads a replacement file into memory instead of
  mapping it. Both mods load under Wine; they have not been run in a duel
  on Windows yet.
- **Tests.** On MinGW every CMake test links `-static`: otherwise a 32-bit
  test loads whichever `libwinpthread-1.dll` PATH finds first, often a
  64-bit one, and fails to start with 0xc000007b. Run them with a native
  Windows `ctest`; an MSYS one mangles the test paths.
- **rename.** Windows' `rename` does not replace an existing file; states,
  settings, controls and memory cards save through `MoveFileEx`
  (`pc/compat/posix.h`).
- **Narrow returns.** clang leaves the upper bits of a `char`/`short` return
  undefined, which GCC happens to fill. Two matching definitions are read
  wider by callers (`Ai_GetHandSize`, `MemCard_FindLoadedEntry`; an IR
  comparison of declared and defined return types over all game units found
  only these), and `src/pc/overrides/narrow_returns.c` returns full words for
  both. Before it, the opponent's turn in a duel ran off guest RAM.
- **Libraries.** fontconfig is replaced by fonts from `%WINDIR%\Fonts`
  (`Win32_FontPath`), iconv by code page 932, and the few POSIX calls by
  `pc/compat/posix.h` and `pc/compat/mman.h`.

### 64-bit Windows (milestones X1-X3)

```sh
python tools/pc/build_win32_deps.py --arch x86_64   # once; build_game32.py does it too
python tools/pc/build_game32.py --target windows-x64  # tmp/pc/win64/memories-pc.exe
```

A native x86-64 executable from the same sources, with the guest at its
fixed addresses and 32-bit guest pointers. It boots through the logos and the movie
to the title, the main menu and Options, and the smoke fixtures `title`,
`main-menu-cursor`, `main-menu-widescreen` and `options` give the 32-bit
hashes (PGXP and HD off, as the fixtures run). What makes it work:

- **Guest pointers stay 4 bytes.** The game's stored pointers are clang's
  `__ptr32 __uptr` through `G32` (`src/port_ptr.h`, "Guest-width pointers"
  above), with `-fms-extensions`; `CALL32` casts before a call through one.
  Casts that make a pointer from a signed int or walk a guest table carry
  `G32` as well; `python tools/pc/check_x64_casts.py` finds the ones that
  do not (two minutes; `--fix` edits them). It also reports `(T *)(i +
  sizeof(x))` with a signed 32-bit `i` ("widened"): the sum is 64-bit
  unsigned, so it is not a signed cast, but `i` is sign-extended into it.
  It found two, fixed by hand with the offset kept 32 bits wide. The bolt
  effect's vertex step (`func_8014FABC`, `bolt_vertices.c`) took its buffer,
  guest RAM at 0x80103800, out to 0xFFFFFFFF801xxxxx: this build crashed as
  Dark Hole (effect 17) or Spellbinding Circle (effect 13) resolved (the
  `dark-hole` and `spellbinding` replays; v0.2.0 shipped no 64-bit build).
  Build Deck's `BuildDeck_AddCard` did it to its record, which is port data
  in the image: harmless at 0x40000000, but arm64's game library is at
  0xC0000000, and there the first card added to the deck crashed the game
  (the `build-deck-add` replay keeps the path in step on both widths).
- **Toolchain.** `x86_64-w64-mingw32-clang`, clang 21 or later: the build
  first runs `tools/pc/x64_compiler_gate.c`, which refuses clang 12's
  silent miscompile of `__ptr32` function-pointer arrays. `-fno-jump-tables`.
  `src/pc/compat/ptr32.h` is included first into every unit: mingw's
  `_mingw.h` defines `__ptr32` as nothing, and Psy-Q's `size_t` must be
  the host's.
- **Everything the game can reach is below 4 GB.** The image is linked at
  0x40000000 without ASLR (`--disable-dynamicbase
  --disable-high-entropy-va`): native function addresses go into 4-byte
  guest slots, and game code reaches the pinned variables RIP-relative
  within 2 GB. Guest RAM, its mirrors, the scratchpad, the game stack
  (0xB0000000) and the arenas keep their 32-bit addresses. The game stops
  with a message if Windows ever loads it above 4 GB. The regions mapped
  after guest RAM (arenas, the interpreter's and the game's stacks) are
  reserved right after it: a 64-bit GL driver and audio also load below
  4 GB, and once took the game stack's range. `MEMORIES_X64_MAP_REPORT=1`
  prints the image base; 320 headless launches all mapped, and windowed runs
  with GL and audio give the fixture frames (2026-09-30).
- **Host memory the game is handed is low too.** A block the port
  allocates and gives the game, or that native code returns to it as a
  pointer, comes from `Memories_LowAlloc`/`Memories_LowFree`
  (`src/pc/guest/low_memory.h`): a first-fit allocator over 16 MiB at the
  fixed address `0x9E000000` (after the compiled text's region at
  `0x9C000000`, below the interpreter's stack at `0x9FF00000`), held from
  `Memories_GuestMap` like the other regions and mapped on first use. Its
  users: the kanji ROM glyphs `Krom2RawAdd2` returns to the credits, the
  compiled text when its own region cannot be had or is full (on 32-bit it
  stays on the heap as before), and a mod's card names and texts, guardian
  star names and duelist names. On 32-bit the two are `malloc` and `free`.
  New code that hands the game a host block uses them; host-only memory
  stays on the heap.
- **Arch code.** One branch thunk (`__x86_indirect_thunk_r11`, the only
  one clang's x86-64 retpoline uses), `state_x86_64.S` (VSync entry and
  the game-stack switch, TEB bounds through `%gs`), `setjmp_x86_64.S`
  (the Win64 state does not fit the game's 48-byte `jmp_buf`: it goes in a
  host slot keyed by the buffer), and the fault handler's `Rip`/REX decoding
  for the null-page fix-up, which also takes clang's load through a G32
  pointer, `disp32(,%reg)` with no base register. A fatal fault in the
  game's code, or a call to where nothing is (a non-canonical target faults
  as a #GP in the branch thunk, reported at `0xFFFFFFFF`), is noted on
  stderr ("fault at rip ... (r11 ...)", r11 being where a call through the
  thunk went). x86-64 code keeps no frame chain, so the 64-bit crash and
  hang reports list the callers from the unwind tables, named as on 32-bit
  (`Win32_UnwindCallers` in win32.c), with whole 64-bit addresses and a
  `RIP RSP RBP` registers line; their `executable` fact says
  `64-bit (x86-64)`.
- **What G32 on declarations does not cover.** A local that receives a
  guest table whole (`func_8004EB00` copies four model handlers from
  `D_800114E8` as one 16-byte block) is `T (*G32 name[N])(...)`, called
  through `CALL32`; the port's own `extern` declarations of pinned guest
  globals (`libgs.c`'s `D_800E9D98`) carry G32 as the game's headers do; a
  guest struct the port reads through a cast (`GsDrawOt`'s GsOT tag) reads
  `T *G32`. `check_x64_casts.py` scans `src/pc/sdk` and `src/pc/platform`
  for the last kind as well as the game.
- **Truncation check.** `MEMORIES_X64_HIGH_HEAP=1` reserves every free range
  below 4 GB and fills the process heap's low segments, so host memory comes
  from above 4 GB; a host pointer stored into a guest slot then faults, and
  is reported as "truncated host pointer". The title, the menus and a duel
  run with it clean.

**X2 (2026-10-03): the game plays as on 32-bit.** Replays recorded on the
32-bit build with every mod off (`tests/pc/replays`: `python
tools/pc/replay.py run tests/pc/replays --executable
tmp/pc/win64/memories-pc.exe`) play on the 64-bit build with every frame
hash the same, plainly and with `MEMORIES_X64_HIGH_HEAP=1` (the same command
with `--env MEMORIES_X64_HIGH_HEAP=1`: a replay's game never sees the
caller's own MEMORIES_* variables): boot to the first story duel; a whole duel
against Simon Muran with a fusion, Raigeki, Forest, Red Medicine and 3D
battles (each attacker's MODEL variant module run by the MIPS interpreter,
`MEMORIES_TRACE=model`) through the result to Free Duel, also with
`MEMORIES_DUEL_EFFECTS=interpreter`; the credits from the save prompt to the
end of the roll, also with `MEMORIES_CREDITS=interpreter`; the title menu,
Options, Build Deck, the Library, Password, the map and Free Duel. On the
way, `func_80051350` (the 3D camera's nearness test) stopped reading two
locals through pointers that only the console's stack frame lines up: both
port builds had read other words there, and a credits scene's camera
drifted.

**X3 (2026-10-03): states, the crash monitor, data mods, a package.**

- **Save states.** The 64-bit game saves and loads its own states: the
  32-bit format, with an entry of nine 64-bit registers and xmm6-xmm15
  (Win64 keeps them across a call), and a `jump-slots` chunk for the host
  slot that holds the game's `jmp_buf` (on 32-bit it lies in guest RAM).
  A state saved, then loaded in a new process and played with the same
  presses, draws the same 2000 frames on the campaign map (twice) and in
  the opening movie (`tools/pc/test_state_resume.py --executable
  tmp/pc/win64/memories-pc.exe`), as on 32-bit; the replays
  `state-load-rng` and `state-load-cpu` pass on 64-bit. The VSync a loaded
  state resumes in returns what it returned in the game that saved it
  (`resume_value`) on both widths.
- **No state crosses the widths.** A state holds the game's native stack
  (the frames of `run_game`, `Main_Init`, `Main_Loop`, `Main_AdvanceFrame`,
  `Graphics_SyncFrame` and, during a fade or a load, more: compiled for one
  width), the game objects' variables as one build lays them out
  (`game_dat` is 0x2B44 bytes at 0x023A3000 on 32-bit and 0x3144 at
  0x421A5000 on 64-bit, in the builds of 2026-10-03) and its subsystems' native fields. Either game
  refuses the other's states by name ("saved by the 32-bit game, and this
  one is 64-bit"), told by the size of the `entry` chunk (20 bytes on
  32-bit). Carrying a state across needs a format without native frames
  or layouts: a milestone of its own. (The game's own saves,
  `saves/slotNN.sav`, are the game's data; that the other width loads them
  is expected but not yet checked.) The width check comes before the mods check, so
  the mods refusal ("uses different mods...") is only ever between two
  states of one width, whose code mods are the same (none on 64-bit).
- **Mods.** The 64-bit build carries the data mods (`build_mods(code=False)`
  copies them and leaves the code mods out), and the game is built with
  `MEMORIES_NO_CODE_MODS`. The data mod loader is the 32-bit one, and what
  was run on 64-bit draws as there: cards and a texture pack (the gate
  replay below), a booster pack (state-load-rng's test mod), and the
  baseline release's data examples load without a note (`check_mod_abi`
  below). Fusions and the other tables, guardian stars, duelists, limits,
  audio, translations and disc patches load the same way but have no
  64-bit frame check yet; star and duelist names come from the low memory
  region (X2), which no mod has exercised there. A mod with a `library` (3d-monsters,
  hand-camera, ai-hard-mode, yamyi-mods, the gameplay-rules example) is a
  32-bit object: none ships with the 64-bit game, and one the player
  installs is not loaded: the Mods window shows "needs a 64-bit build of
  this mod" beside it. It is not a broken mod, so
  the other mods' Apply goes on, and it is not in a state's mod set. What
  stays off with it: `object_loader.c` maps an object wherever the system
  puts it and its relocations are i386 (`ObjectLoader_Load` refuses too),
  `mod_libc.c` lends 32-bit helpers and the heap's `malloc` (which may be
  above 4 GB), and `hooks.c` writes an i386 `jmp *[abs32]` (inert off
  i386 already). No mod SDK goes beside the 64-bit game: it builds 32-bit
  objects. `check_mod_abi.py --run --build <64-bit build>` plays a
  baseline release's mods there and requires that note for each code mod.
  A replay recorded on 32-bit with two data mods on (the card-pack example
  with its two cards in the deck, and a texture pack of the screens'
  sheets with every color turned, made from the disc at play time) plays
  on 64-bit with all 1145 frame hashes the same, also with
  `MEMORIES_X64_HIGH_HEAP=1`: `tests/pc/replays/x64-data-mods`, whose
  `mods.py` makes the two mods in the play's folder (`tools/pc/replay.py`
  gives the game `MEMORIES_MODS_DIR`).
- **Crash monitor.** On, as on 32-bit: the second copy of the game with the
  shared block. A thread's callers come from the unwind tables through
  dbghelp's `StackWalk64` on the game's process. `crash_check.py --windows
  --executable tmp/pc/win64/memories-pc.exe`: all 13 kinds end in their
  reports.
- **Package.** `python tools/pc/package.py windows-x64` makes
  `dist/yfm-redecomp-<version>-windows-x64.zip` (its folder
  `yfm-redecomp-<version>-x64`, with the data mods and a README that says
  so) beside the 32-bit
  `-windows.zip` (the default now packs all three); `smoke.py` skips the
  cases that turn on a code mod for it, and `test_package.py` checks the
  x86-64 executable and that there is no SDK. The release workflow
  (pc-release.yml) does not build it yet: its Windows job's later steps
  (the FM Editor, VirusTotal) key on the runner being Windows, so a second
  Windows entry would need them keyed on the system, and the 64-bit
  libraries (`tmp/pc/win64-deps`) a cache of their own.

Still off in the 64-bit build: code mods, whose 64-bit SDK is a later
milestone, after arm64, covering both 64-bit targets (the loader for
x86-64 and AArch64 objects, the mod C library, function hooks); the
interrupt clock (the cooperative one is the default anyway); a state from
the other width.

## Android

**Status (2026-10-04):** the app is **arm64-v8a** ("Android arm64" below:
the target, its memory layout and the phone tests). `android-x86`, the
target M1-M4 were made on with the emulator images, still builds as a
development check of the shared code, but no APK is made for it and it is
neither run nor shipped. The 32-bit ARM target (`armeabi-v7a`, M2) was
removed, and with it the 32-bit-kernel and 32-bit-userspace limits below;
`--target android-armeabi-v7a` is refused. What follows is the record of
M1-M4: the shared pieces (loader, disc picker, touch controls, lifecycle,
dimmed rows, APK packaging) are the arm64 app's too.

Milestone M1 (2026-09-28): the same code as Windows and Linux, built for
Android x86 (32-bit), boots on the API 30 x86 emulator through the intro
(Konami logo, the opening movie) to the title screen, and on to the main menu
and New Game's name entry. Milestone M2 (2026-09-29), 32-bit ARM, is not kept
(Status above). Milestone M3 (2026-09-29): playable. The disc image comes in through the
system's file picker, touch controls are drawn with the game's own button
pictures, taps shorter than a frame count, the app pauses in the
background, Back asks before quitting, save states survive a relaunch (the
game is loaded at its link address), and the desktop-only menu rows are
dimmed. M4 (2026-09-29) was started and then paused: **the Android port
waits for the 64-bit (relocatable guest) work**, and picks up from the state
in "M4, where it stopped" below. Android TV is out of scope. The design and
the probe behind it are in
`tmp/research/android_feasibility.md` (not in the repository); this section
is what exists.

**Supported (M1-M4, 32-bit):** a 64-bit kernel that ran 32-bit apps; not
the arm64 app's limit, which needs a 64-bit kernel and runs on arm64-only
phones too ("Android arm64").

### Build, install, run

Needs the Android SDK with the NDK (r29 tested), a platform (android-35) and
build-tools (35), a JDK (17 or later: `javac`, `keytool`), cmake and ninja.
Nothing else: no Gradle, no Android Studio.

```sh
export ANDROID_SDK_ROOT=/path/to/sdk          # the NDK: newest under sdk/ndk, or ANDROID_NDK_ROOT
python3 tools/pc/build_game32.py --target android-x86
# -> tmp/pc/android-x86/libgame.so, libmain.so and tmp/pc/android-x86/memories-x86.apk
adb install -r tmp/pc/android-x86/memories-x86.apk
```

- `tools/pc/build_android_deps.py <abi>` (run by the build the first time)
  builds SDL3 (shared), libpng and FreeType (static) with the NDK from the
  same pinned archives as the Linux and Windows builds, into
  `tmp/pc/android-deps/<abi>`, and keeps SDL's Java shell (`org.libsdl.app`)
  from the same SDL release. zlib is the system's.
- `build_game32.py --target android-<abi>` compiles every unit with NDK clang
  for the ABI at API 24, `-fPIC`, and links the game as the shared object
  `libgame.so` (`-Bsymbolic`, `--no-undefined`; pins `HIDDEN`, no fixed
  sections, as on Windows) at a fixed base, `0x08000000` (`--image-base`;
  the build checks that its load span fits the `0x04000000` the loader
  reserves; it was `0x02000000` until the image outgrew it). `libmain.so`, the library SDL's Java shell loads, is only a
  loader (`src/pc/platform/android_loader.c`): it reserves that range and
  loads `libgame.so` into it with
  `android_dlopen_ext(ANDROID_DLEXT_RESERVED_ADDRESS)`, so the load bias is
  0 on every launch, then runs `Memories_AndroidMain` (android.c); when the
  port's `main` returns, the process ends with it. If the range cannot be
  had, the game loads where the system puts it and keeps save states for
  that launch only. `tools/pc/package_android.py` then compiles SDL's Java
  with `javac` against the SDK's `android.jar`, dexes it with `d8`, links
  the manifest with `aapt2`, adds `lib/<abi>/libmain.so`, `libgame.so` and
  `libSDL3.so` and the build's `buildid`, `commit` and symbol table as
  assets (`assets/build/`), and aligns and signs the APK (`zipalign`,
  `apksigner`) with a debug key it creates under
  `tmp/pc/android-deps/debug.keystore` (never in the repository), or, with
  `MEMORIES_ANDROID_KEYSTORE` and its password set, with that release key.
  It prints the signer's certificate (DN, SHA-256) and verifies the APK
  (`apksigner verify`).
  The variables, the release key (held by Unchiga, in the repository's
  secrets; releases are signed only by CI) and `package.py android-arm64`
  (`dist/yfm-redecomp-<version>-android-arm64.apk`, release key only) are
  in [PC release](pc-release.md), "Android signing". `versionCode` and
  `versionName` come from the build's version (pc-release.md, "Version":
  `v0.3.0-preview.1` is 30041, `0.3.0-preview.1`). The release APK does not
  install over a debug-signed one (`INSTALL_FAILED_UPDATE_INCOMPATIBLE`):
  uninstall the test app once first, which deletes its files (disc copy,
  saves). The
  package is `org.yfmredecomp.game`; the activity is SDL's own
  `SDLActivity`, with no Java of ours.
- `--target android-armeabi-v7a` is refused: 32-bit ARM was removed.

**The disc image.** On the first run the game finds no image and shows its
welcome box; "Choose disc image..." opens the system's document picker
(SDL's file dialog: the Storage Access Framework), so the image can be
anywhere the phone reaches (Downloads, an SD card, a cloud drive), with no
adb and no root. Any document is offered (the system knows no type for a
`.bin`); the chosen one is checked where it is (`SLUS_014.11` on a raw
image, as `game_files.c` checks) before anything is copied, and a file that
is not the disc gets the desktop's "Unable to use this ROM" and the welcome
box again. A good image is copied (about 25 s for 494 MB on the emulator)
into the app's external files folder, `game/rpg-yfm.bin` under
`/sdcard/Android/data/org.yfmredecomp.game/files/`, since the right to read
a picked document does not outlast the app, and is remembered as on the
desktop. Only raw `.bin` images: `game_files.c` reads nothing else (no
`.cue`, `.iso` or `.chd`). The Android TV images have no document picker
(the intent resolves to a stub that returns at once, and the welcome box
comes back): there the image has to be put in place by other means.

Testing aids: `environment.txt`, `NAME=value` per line, in the external
files folder (or, for a debuggable build, the internal one, which `run-as`
reaches on an image without root) sets environment variables before the
port starts (the app has no environment of its own): `MEMORIES_TRACE`,
`MEMORIES_INPUT` (scripted pad), `MEMORIES_DUMP_FRAME`, and so on. The
game's load address and load bias are logged at start (`adb logcat -s
memories`); with bias 0 a crash's addresses are the build's own, for
`llvm-symbolizer --obj=libgame.so`. On an emulator image with root the disc
can also be pushed straight into the game folder:

```sh
adb root
F=/data/media/0/Android/data/org.yfmredecomp.game/files
adb shell mkdir -p $F/game && adb push rpg-yfm.bin $F/game/
adb shell chown -R "$(adb shell stat -c %U $F)":ext_data_rw $F/game
adb shell chcon -R "$(adb shell ls -dZ $F/reports | cut -d' ' -f1)" $F/game   # the app's SELinux categories
```

(without the `chcon` the file keeps `storage_file` and the app is denied).
On an image without root, a debuggable build's `run-as` can copy it into
the program directory's `game/` (`files/program/game/` in the internal
files folder), which `game_files.c` searches first.
Screenshots of the device, never the host: `adb exec-out screencap -p`.

### How it differs (and what is shared)

- `src/pc/platform/android.c` is the whole platform layer:
  `Memories_AndroidMain` (called by the loader) sets the player's folder
  (`MEMORIES_USER_DIR` = the external files folder), forwards stdout/stderr
  to logcat, reads `environment.txt`, unpacks the build's files into the
  internal files folder's `program/` and names it the program directory
  (`MEMORIES_PROGRAM_DIR`, `paths.h`: save states and crash reports read
  `buildid` and `symbols/` there; the symbol tables of earlier builds stay,
  so a state from an earlier APK is carried over by name), turns off what
  re-executes the program (the crash monitor, `Platform_RestartGame`) and
  the update check, asks SDL for landscape, a fullscreen (immersive) window
  and Back for the game, and runs the port's `main`. It has the disc picker
  (`Platform_SelectDisc`) and says what a failed guest mapping means
  (`Platform_GuestMemoryHelp`). `Platform_HasDesktopGL` answers 0: the
  window takes the SDL renderer path (opengles2), in an OpenGL ES 3.0
  context with the OpenGL picture pass in it where the device has ES 3,
  else showing the software GPU's picture ("OpenGL ES 3 (Android)"). It
  also stands in for `bzero` and, below API 30,
  `memfd_create` (the system call), with the ashmem device where the kernel
  has no memfd.
- `src/pc/compat/android/`: `android_compat.h`, force-included in every
  native unit (below-API fallbacks, no system headers), and a header-only
  `fontconfig/fontconfig.h` answering the port's few fontconfig calls with
  `/system/fonts`. `src/pc/render/gl_desktop_none.c` replaces
  `present_pass.c`: desktop GL's fixed function does not exist in GLES and
  is never reached there.
- In the SDL backend (`sdl.c`), under `SDL_PLATFORM_ANDROID`: Back closes
  a menu or answers a notice as Esc does, closes the deck slot screen, and
  otherwise asks "Quit the game?" with Quit, Menu (opens the first menu: the
  way to the menus with a controller in hand) and Keep playing
  (`QuitPrompt_Back`); the rows for second
  windows (Controls..., Mods), the window's size and mode, and the update
  check are dimmed (`Menu_SetPlatformItems`), `Platform_HasWindowModes`
  answers 0 (F11, Alt+Enter and Esc keep the whole screen). Sizes come from
  the display's density (SDL's content scale, densityDpi / 160), not the
  window: Automatic menu size is the density rounded (13 px text at 1 dp,
  about the system's 14 sp), the bar, the rows and a notice's buttons are at
  least 48 dp tall (`Menu_SetTouchTarget`), and the touch controls 48 to 80
  dp. The bar is hidden, as in a desktop's fullscreen, and drawn over the
  picture when it shows (where the window is always the whole screen,
  `bar_overlays`), so the picture and the touch controls never move: MENU on
  the touch controls opens it, as does a tap at the top of the screen or
  Back's Menu. The mouse SDL makes of a finger does not hover: before, its
  last place (0,0 at start, the top after a tap on the bar) kept the bar
  shown for good, and its move to a tap opened the menu that the press then
  closed. Everything else there is the desktop's.
- Shared changes this needed, one commit each: the game stack at
  `0xB0000000` on every system; the scratchpad at `0x9F800000`
  (`SCRATCHPAD_ADDR`, "How it works" above); `HIDDEN` pins; the asm stack
  switch for Android (bionic has no ucontext) and its 16-byte-aligned entry
  frames; position-independent paths in the i386 assembly; the desktop-GL
  capability (M1); the ARM pieces (M2); and for M3: a key tapped between
  two pad updates still reaches the game (`controls_runtime.c`; `adb shell
  input keyevent` sends down and up in one millisecond); `MEMORIES_PROGRAM_DIR`;
  the clock stopping while an app is in the background (SDL's application
  events, which reach only event watchers); touch controls (View > Touch
  controls); `Platform_GuestMemoryHelp`; `Menu_SetPlatformItems`.

### What works on the emulator (API 30 x86)

- Boot, intro logos, the opening movie (MDEC), title, main menu, New Game to
  the name entry, Options; the menu bar draws and is tapped. Sound through
  SDL (AAudio).
- **Input:** the touch controls (View > Touch controls, several fingers at
  once), a keyboard (a hardware one, or `adb shell input keyevent`: a tap
  counts once), and SDL's gamepads. The emulators' own keyboards show up as
  gamepads too (on the TV image "qwerty2" and "virtual-search"); they press
  nothing and take nothing from the keyboard.
- **Lifecycle:** to the background (Home), the game clock stops (SDL holds
  its event loop and the audio); back, it resumes where it was with no rush
  to catch up (the VBlank count is the same before and after 30 s away),
  and the picture comes back. Rotation is landscape only. Back asks "Quit
  the game?" (a second Back keeps playing); Quit ends the process, and the
  next launch starts afresh.
- **Save states:** `libgame.so` sits at `0x08000000` with load bias 0 on
  every launch; F5 on the Options screen, the app force-stopped and started
  again, F7 at the title brings the Options screen back, live.
- The app process's layout: `0x1F800000` is taken (ART's heap), the port
  says so once and uses `0x9F800000`; guest RAM, its mirrors, the game stack
  and the interpreter stack map where they do on the desktop.
- Presents take 20-40 ms at 2280x1080 with the emulator's host GPU
  (`-gpu host`), 40-80 ms with SwiftShader; the game clock keeps time and
  presents drop frames.

### M4, where it stopped

Paused on 2026-09-29 until the 64-bit (relocatable guest) work is done.

- **Done:** the history of the Android branches is folded for review (the
  mod SDK's `signal_context.h` fix is part of the M2 commit that added the
  header). The APK carries what the desktop games have beside them: the
  shipped mods (`mods/`, their objects checked against the Android build's
  own export table) and the language packs (`languages/`), as assets under
  `build/files/` listed in `build/files.txt` (`package_android.py`);
  `android.c` unpacks them into the program directory with the rest of the
  build's files, removing an earlier build's `mods/` and `languages/` first.
  Checked on the API 30 x86 emulator: 15 files unpacked
  (`program/mods/<mod>/`, `program/languages/*.txt`).
- **Found:** the i386 mod objects are the same code for Android x86: the
  objects built for the Android build differ from the Windows build's only
  in their debugging information (source paths); stripped of it they are
  byte-identical. Every record under `src/` has the same layout for
  `i686-linux-android` as for `i386-pc-linux-gnu` (`check_layouts.py` with
  the Android triple in place of the Windows one: 667 headers, 0 differ), so
  one `.o` serves Linux, Windows and Android x86. The player's mods folder
  is `mods/` in the external files folder (the user directory); on Android
  11 and later `adb shell` without root cannot list
  `/sdcard/Android/data/<package>`, so tests there need root (the M3
  `chcon` recipe above) or a debuggable build's `run-as`.
- **Not started / half-done:**
  - Applying mods in the app: Game > Mods is still dimmed, so only mods
    whose manifest says `"enabled": true` are applied; not yet checked in a
    game on Android (code mods that hook game functions write to
    `libgame.so`'s text: watch for SELinux denials on the first apply).
  - A mod `.zip` through the system's file picker; Mods and Controls as
    panels inside the game window (the game's font, as the other overlays);
    the menu bar hiding in play: done on feat/android-arm64 (the mouse SDL
    makes of a touch kept it shown; see "How it differs").

### Not yet

- **Mods:** see "M4, where it stopped". Game > Mods and Controls... are
  dimmed (second windows; an in-window version of both is for later).
- **Restart:** an app cannot re-execute itself; where the port restarts
  (the end of the credits, Game > Language), it falls back as when a
  restart fails (back to the title; "start the game again").
- Video > Color (the present pass, desktop GL's fixed function) and no
  update check. Internal 2x and 4x, HD text and PGXP are drawn by the GPU
  on OpenGL ES 3 ("OpenGL ES 3 (Android)"); a phone without ES 3 keeps the
  software picture, which is slow above 1x.
- Android TV: out of scope (no document picker there; M3's notes on the TV
  image stay as test findings).

### Next milestones

| M | Goal |
|---|---|
| M2 | Done, then removed (2026-10-04): the app is arm64-v8a |
| M3 | Done: disc import through the file picker, the input latch, touch controls with the game's art, lifecycle (background, Back, landscape), the fixed-base loader (save states, crash symbols), desktop-only menu rows dimmed, the 32-bit-kernel message. Left: Mods/Controls as in-window overlays, performance (internal scale above 1), a real restart, the disc on Android TV |
| M4 | Paused (above), waits for M6's 64-bit work. Mods on Android: content-only mods first; then per-ABI objects for code mods (bundled mods built by `build_game32.py`, third-party ones by the SDK's `build_mod.py` per target), ARM relocations in the object loader, `__aeabi_*` helpers, hook trampolines for armv7 |
| M5 | Release: signing, CI for both ABIs, emulator smoke; before it, a duel played on a real arm64 phone that runs 32-bit apps (the TV translator mishandles the fault paths) |
| M6 | The relocatable guest / 64-bit everywhere: closes both gaps left, arm64-only phones (no 32-bit apps) and 32-bit kernels (3 GB, the top taken), since the guest then needs no fixed addresses; Windows and Linux move with it |

## Launch the local graphics preview

```sh
./launch-pc-preview.sh
```

The installed Linux executable is `tmp/pc-preview/memories-pc-preview` (about
4.7 MiB stripped). Both entrypoints open a persistent preview window with the
texture test and a port-in-progress label. Close the window to exit. The game
does not boot from this executable. No game assets are required by the preview.

Rebuild/install it after configuring the optional PSY-Z build below:

```sh
cmake --build tmp/pc-psyz --target memories-pc-preview
cmake --install tmp/pc-psyz --prefix "$PWD/tmp/pc-preview" --strip
```

`./launch-pc-preview.sh --frames 3` was tested successfully with the local
Vulkan window backend. `--help` lists preview/capture options; `--headless`
performs the render checks and exits without entering the presentation loop.
This binary targets the local Linux x86-64 environment, not Windows or an
arbitrary older Linux distribution.

## Build without assets or an SDK

From the repository root, with CMake 3.21+, a C11 compiler and optionally Python:

```sh
cmake -S . -B tmp/pc -DCMAKE_BUILD_TYPE=Debug
cmake --build tmp/pc --config Debug
ctest --test-dir tmp/pc -C Debug --output-on-failure
cmake --build tmp/pc --target pc_audit
```

The audit writes `tmp/pc/port-audit.json`. It inventories all 61 resident ASM
targets, the five configured overlay modules, SDK references and source hazards.
Initial results: 1,071 scanned C/header files, 215 distinct resident SDK symbols,
120 scratchpad-literal lines, and 192 address-to-32-bit-cast candidates. This is
a lexical review aid, not a complete call graph, data-flow analysis or proof that
all executable overlay packages have been found.

For GCC/Clang sanitizers, configure a separate build with
`-DMEMORIES_SANITIZERS=ON`. The core test covers original game RNG integration,
overflow, signed comparison boundaries, guest address aliases, scratchpad ranges,
endianness, alignment and invalid/overflowing spans. Release builds retain checks.
The new GitHub workflow defines Linux and Windows core builds and Linux sanitizer
checks; remote workflow execution has not been observed in this workspace.

Guest storage is currently used by adapter tests only. It does not yet supply
the game's linker globals, relocations or overlay data, and cannot execute guest
code. Unsupported RAM mirrors, BIOS and MMIO accesses are rejected explicitly.

## Host compile census

```sh
python3 tools/pc/host_census.py
```

This syntax-checks all 546 game/overlay C units with the host GCC and writes
`tmp/pc/host-census.json`. First result on 2026-09-20: **465 pass as ILP32 (`-m32`),
80 pass as LP64**; with the guards and flags above, **546 / 546 pass as ILP32**
(83 as LP64). Nearly all LP64 failures are the size/offset assertions in
`src/ygo_types.h` firing on pointer-bearing structures. The 81 ILP32 failures
come from a few headers: the incomplete `GraphicsFrameBuffer` array declaration
(54 errors), `jmp_buf`, `struct DIRENTRY`, and `DisplayObject_*` pointer-type
mismatches. It is a front-end check only; it says nothing about linking,
fixed addresses, linker-placed globals or behavior.

## Optional pinned PSY-Z probe

```sh
git clone https://github.com/Xeeynamo/psyz.git tmp/port-research/psyz
git -C tmp/port-research/psyz checkout e2d3a84eb4432c0a80b193eb844edc2bdde90c62
git -C tmp/port-research/psyz submodule update --init --depth 1 external/SDL
cmake -S . -B tmp/pc-psyz \
  -DMEMORIES_PSYZ_ROOT="$PWD/tmp/port-research/psyz" \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build tmp/pc-psyz --config Debug
ctest --test-dir tmp/pc-psyz -C Debug --output-on-failure
```

If that checkout already exists, skip cloning. On PowerShell pass an absolute
Windows path for `MEMORIES_PSYZ_ROOT` and use one line for the configure command.
The SDK needs a C++ compiler and SDL platform build dependencies. CMake refuses
an incorrect SDK revision or a missing SDL checkout. It does not fetch or build
the original proprietary SDK. The SDL commit recorded by this PSY-Z revision is
`147a8ee32dbf9ac02f3794964490687b6bbda1bc`.

The probe checks GTE signed triangle area and native ordering-table pointers.
It does not render a frame or boot the game. It must call `ResetGraph` to install
the SDK's native callbacks before using `ClearOTagR`. PSY-Z logs its unimplemented
`ResetCallback` during initialization; this remains a known integration gap.
See [SDK evaluation](pc-sdk-evaluation.md) for the remaining coverage gaps.

## Retail GPU packet render test

After building the optional SDK targets:

```sh
tmp/pc-psyz/memories_render_smoke tmp/pc-psyz/retail-packets.ppm
```

On this Linux machine it also passes without a window:

```sh
SDL_VIDEODRIVER=offscreen tmp/pc-psyz/memories_render_smoke tmp/pc-psyz/retail-packets.ppm
```

For multi-configuration generators use the configuration subdirectory, e.g.
`tmp/pc-psyz/Debug/memories_render_smoke.exe`. The optional argument writes a PPM
of the checked 320x240 VRAM region. It is captured before the queue stress test.
Configure `-DMEMORIES_RENDER_TESTS=ON` to register the render executable in CTest;
the default tests do not require a graphics driver.

The test submits original-width DMA packet links with draw-state commands, a
16bpp sprite, a 4bpp palette-indexed sprite, and terminal NOPs. All 76,800 pixels
are checked against a synthetic expected RGB555 image, excluding the mask bit.
It then submits 18,003 words, exceeding PSY-Z's 16K queue, and checks the final
draw arrived. A malformed stream must fail without partially changing the image.
The render log is `tmp/render-smoke.log`. Offscreen Vulkan reports that swapchain
presentation is unavailable; GPU rendering and VRAM readback still pass. Window
presentation, retail visual fidelity, transparency and all primitive variants
have not been validated by this test.

Packet traversal also has asset-independent sanitizer tests covering empty
entries, split commands, cyclic/unaligned/out-of-range links, insufficient buffer
space, terminal payloads and unsupported/truncated commands. See
[frontend contracts](pc-frontend-contracts.md) for the recovered LIBGS/LIBDS and
frame-service requirements.

## Reference inputs and build

The user's BIN/CUE in `/home/codyj/Documents/YFM` were read without modification.
An ignored local BIN copy, canonical CUE, executable and seven DATA files are
under `game/`; all ten tracked hashes pass `make verify-inputs`.

This machine's default Python 3.14 and GCC 16 do not directly build the pinned
reference toolchain: the Python lock requires 3.10, and binutils 2.42 uses a
`static_assert` identifier that conflicts with GCC's default C23 dialect.
The local workaround uses a standalone Python 3.10.21 under
`tools/environments/bootstrap-python` and `CFLAGS='-O2 -std=gnu17'` when building
binutils. These are local toolchain selections, not changes to the game's
reference compiler profiles. The pinned prebuilt MIPS GCC 2.8.1 installs normally.

Validation completed on Linux x86-64:

- `make verify-inputs`: all ten tracked hashes pass.
- `make -j8 match`: resident executable matches SHA-256
  `84a54ed74f3d0edd6d81380839f7e4ef5bfb21ecea18be9a062bd6bfa5a45c88`.
- `make -j8 match-overlays`: all five configured modules match.
- Core and packet CTests with address/undefined-behavior sanitizers: pass.
- Optional SDK build and CTest: core, packets and PSY-Z probe pass.
- Offscreen Vulkan render smoke: full texture readback, queue stress and atomic
  malformed-stream rejection pass.
- Audit spot checks: comment/string masking, manifest counts, and known SDK/
  scratchpad sites pass.

Reference logs are under `tmp/reference-match.log` and
`tmp/reference-overlays.log`; these generated files are ignored. Windows CI is
configured but has not been run here. No native game boot, retail-frame fidelity,
audio, or assembly-replacement equivalence claim is made by these checks.

## Android arm64 (A1/A2, 2026-10-03)

`--target android-arm64-v8a` builds libgame.so with G32
pointers (`WIDE` in build_game32.py: the windows-x64 flags), clang's
`-mharden-sls=blr` thunks in AArch64 form (`branch_thunks.c`; the build
checks that no `br`/`blr` escapes them), `setjmp_aarch64.S` and
`state_aarch64.S`, and the AArch64 fault handler in `image.c` (a low or
retail-scratchpad access runs once from a stub page mapped within a `b`'s
reach of the faulting code, with the addressing register moved up by
0x80000000; no free register exists at an arbitrary load, so the return is
a direct branch).

Host function addresses reach the game's 4-byte slots, so the game library
is linked at 0xC0000000 (`ANDROID_GAME_BASE` on arm64) and the loader
(`android_loader.c`, `ANDROID_DLEXT_RESERVED_ADDRESS`) puts it there, in a
range it asks for with `MAP_FIXED_NOREPLACE` (a plain address is only a
hint, which the kernel passed over on the phone). That base is below 4 GB
(slots are zero-extended: G32 is `__uptr`), has bit 30 set (the thunks'
fast path), and lies outside every guest range the port tests. It is above
the guest because ART fills a 64-bit app process's low 4 GB from the
bottom up. On a Xiaomi 11T Pro (Android 14, `dalvik.vm.heapsize` 512m) the
app process held:

- the heap at 0x02000000-0x22000000;
- a free list at 0x42000000;
- the JIT caches at 0x62000000-0x6A000000;
- the boot image and its spaces at 0x6FCFC000-0x76000000;
- nothing from there to 4 GB, apart from a sentinel page at 0xEBAD6000.

A bigger heap moves all of it up. The first base, 0x60000000, ran as a
plain process but not in the app: the JIT caches took it. When the range
is taken, the loader logs every mapping in it ("occupied by"). The image
is about 35 MiB, most of it `.bss`, inside a 64 MiB reservation. No
trampoline table. A function of another library (libc, SDL) whose address the
game stored would not fit; none is known, and the plain-process runs below
are where it would show (a fault at a truncated address).

**The guest's own ranges have the same limit:** guest RAM at 0x80000000
(and its mirror at 0xA0000000), the scratchpad view at 0x9F800000 and the
game stack at 0xB0000000 must be free, and with Android 10-13's concurrent
copying collector a `dalvik.vm.heapsize` of about 870m or more reaches
0x80000000. The game then stops with "cannot map guest memory at ...", the
reason, and the mappings that hold the range ("occupied by", in logcat);
it does not run elsewhere.

**The retail scratchpad is never used in the app.** 0x1F800000 lies inside
ART's heap there (the port says "0x1f800000 is taken here; the scratchpad
is reached at 0x9f800000"), where an access would not fault but reach the
Java heap. Every native path translates a retail scratchpad address to the
view at 0x9F800000 ("How it works" above), the words the interpreter hands
to native code included, since the retail view is not mapped there
(`Memories_ScratchpadRetailView` 0). `MEMORIES_TEST_HOLD_SCRATCHPAD` holds
the page as ART does, in a test build (Linux or Android). Code mods, which
could write a retail address of their own, are not loaded on arm64.

**SDL's calls into Java run on the thread's own stack.** SDL reaches Java
(JNI) for events and joysticks (`Android_JNI_PollInputDevices`, every 3 s
from the event pump), the window's mode, the cursor, the clipboard,
message boxes, URLs and audio devices, and ART refuses a call from a
native stack it does not know: below the thread's stack end it throws
`StackOverflowError` or skips the method, and with CheckJNI the next call
aborts. The game stack at 0xB0000000 is such a stack. So every platform
entry point the game thread calls that may get there runs through
`Memories_OnHostStack` (`state.c`): on the game stack,
`Memories_CallOnStack` (`state_aarch64.S`) runs it below the frame of the
process side waiting in `Memories_ContextSwitch`, which is the thread's
own stack. They are the presents, which pump the events and apply the
display settings every frame (`Platform_Present`,
`Platform_PresentPicture`, `Platform_PresentWidePicture`),
`Platform_Frame`, `Platform_PumpEvents` (a frame that is not shown),
`Platform_StartAudio` (`SpuInit`), `Platform_ShowError`,
`Platform_OpenUrl`, `Platform_OpenFolder`, `Platform_CopyText` and the
second windows (`sdl.c`'s `HERE`). Only `sdl.c` and `android.c` call SDL
functions that may reach Java (`gl_picture.c` and `present_pass.c` call
`SDL_GL_GetProcAddress` and `SDL_GetTicksNS`, which do not);
`platform/jni_guard.h`, included last in both, checks each
SDL call that may reach Java against the game stack and logs one that
runs there ("... which may call Java, ran on the game stack"), so a new
path that misses the wrapper shows in logcat (arm64 only: the x86
development build has no stack switch, so the check would flag every call). `MEMORIES_TEST_JAVA=<frame>`
(or `<frame>box`, with a message box) makes such calls at that frame in
`Platform_Frame` and in the present's pump, and logs the stack they ran on
and how many guarded calls ran on the game stack so far; `<frame>guard`
also calls the guard once on the game stack (no Java call), to show it
logs and counts. The emulator does
not show the refusal (its translator runs Java on a stack of its own), so
a phone is the real check. A crash inside such a call keeps the game's
callers in its report: `Memories_CallOnStack` leaves its frame record on
the game stack, and `crash.c`'s walk follows the chain down to it once
(`MEMORIES_CRASH_TEST=present` faults inside the next present). These
calls use the SDL thread's own stack, about 1 MiB for a Java thread;
`MEMORIES_TEST_HOST_STACK=1` paints up to 512 KiB below the switch point
at the first switch and logs every 1000 frames how deep they went.

Three things differ from windows-x64 beyond the pointer width:

- **`long` is 8 bytes (LP64).** The Psy-Q `long` is `PSXLONG` (`int` here)
  in the psyq headers and in the SDK stand-ins, and each stand-in unit
  includes the psyq header it implements (`libgte.c`, `libgs.c`,
  `libmcrd.c`, `libetc.c`, `libpress.c`; `libapi_krom.c` through
  `pc/sdk/krom.h`), so a definition that disagrees with the game's
  declaration does not compile. `_Static_assert`s pin the records they
  touch (MATRIX, VECTOR, SVECTOR, GsSPRITE, GsOT, DIRENTRY).
- **A workaround for an LLVM AArch64 bug: stores through `__ptr32`
  pointers lose their width.** Affected: NDK r29's clang 21 (21.0.0,
  r563880c) and upstream clang 22.1.8; x86-64 is not (the windows-x64 build
  needs no workaround), and loads are selected correctly. `p->u16 |= x`
  through a G32 pointer became `ldrh w8, [x9]; orr; str w8, [x9]`, at -O2 a
  plain `p->u8 = v` a 4-byte `str`, and a 6-byte structure copy ended in a
  4-byte store, overwriting what follows (it cleared DisplayObject.field_0A
  beside the flags, which relinked the display lists: the duel drew cards on
  the wrong side of the field and the portraits out of place, while the
  game's state stayed in step). The front end's IR is right, so on arm64
  every game and port C unit (all that `compile_unit` builds; the generated
  stubs, `guest_branches.c` and `mod_exports.c` hold no G32 store) is
  compiled to IR first and `tools/pc/ptr32_stores.py` sends
  each store and memcpy/memmove/memset through a G32 pointer via an
  addrspacecast to an ordinary pointer, then the IR is compiled; it fails
  the build if such a write is left. Reported upstream, with a minimal
  repro: [llvm/llvm-project#228782](https://github.com/llvm/llvm-project/issues/228782).

  Retiring it: every arm64 build first compiles a canary
  (`ptr32_stores.CANARY`: `p->u16 |= 1` at -O0, `p->u8 = v` at -O2, through
  a G32 pointer, without the pass) and prints either `ptr32: compiler bug
  still present (...): pass needed` or `ptr32: the compiler keeps store
  widths through __ptr32: the pass can be retired`. With a new NDK, build
  with `MEMORIES_PTR32_PASS=0`: while the bug is present the build stops
  ("MEMORIES_PTR32_PASS=0 refused"); once the canary is clean it builds
  without the pass, and if first-duel, full-duel, credits and menus all
  play as recorded on arm64, drop the IR step from `compile_unit`, the
  canary and `ptr32_stores.py`.
- **No fused multiply-add.** clang contracts `a*b+c` into AArch64's
  `fmadd` by default and x86 has none without `-mfma`, so float code
  (LIBPRESS's IDCT holds 83 of them) rounded differently from the other
  builds: the menus replay had two VRAM pixels off in 36 of its 1039
  frames. arm64 builds with `-ffp-contract=off`.

Off the phone, the x86_64 emulator image (`api35x64`, Google APIs, which
runs arm64 code through `libndk_translation`) runs `device_run.py` the same
way (`ANDROID_SERIAL=emulator-5554`) and reproduced the phone's replay
divergence exactly; a process there ends with SIGSEGV (exit 139) after the
game exits, which replay.py reports as a failure even when every frame
matches.

Build and test (Git Bash, `ANDROID_SDK_ROOT=D:/Android/sdk`):

```sh
python tools/pc/build_android_deps.py arm64-v8a          # SDL3, libpng, FreeType (once)
python tools/pc/build_game32.py --target android-arm64-v8a --build tmp/pc/android-arm64-v8a
python tools/pc/android/device_check.py                   # the phone's ABIs, page size
python tools/pc/android/device_run.py setup tmp/pc/android-arm64-v8a   # runner, libs, disc
python tools/pc/replay.py play tests/pc/replays/first-duel --check \
    --executable tmp/pc/android-arm64-v8a/device.cmd      # plain process, headless
adb install -r tmp/pc/android-arm64-v8a/memories-arm64-v8a.apk   # Xiaomi asks on the phone
```

qemu-aarch64 (WSL, 4.2) is not the test bed: the Android build is a
bionic shared library, which needs the phone's linker and libc; the phone
over adb is a real kernel (PROT_EXEC honoured, the guest-call fault path
testable) and runs at full speed.
