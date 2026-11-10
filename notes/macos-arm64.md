# macOS ARM64 runtime

The macOS target runs the game's existing 32-bit PS1 address model in a
64-bit ARM process. Guest pointers stored by the game remain 32-bit tokens;
native host pointers remain 64-bit. The runtime registers PS1 address ranges independently of their host
allocations and resolves guest data accesses into native memory. It also resolves
indirect guest calls through a function map, so a callback stored in guest
memory can enter translated game code or the MIPS interpreter for a loaded
overlay.

Target configuration records platform, CPU architecture, host ABI and guest
memory model separately. Upstream's existing Android ARM64 target retains its
fixed mapping and AArch64 assembly; macOS ARM64 selects translated guest
memory. Sharing an ARM64 CPU therefore does not select the Darwin backend.
Linux and Windows retain their existing mappings and build entry points.

The LLVM pass translates game units to ARM64 while preserving the guest
address and storage contracts. The current frontend marks guest pointers with
LLVM address space 271; the pass lowers them to host address space zero, routes
memory operations through the guest runtime, and encodes native pointers
before they are stored in 32-bit guest slots.
Direct accesses rooted in native globals or stack allocations bypass the
resolver, including native GEPs and memory intrinsics. Other accesses use an
inline fast path: full-width native pointers pass through, and physical,
KSEG0 and KSEG1 RAM spans are checked against the currently bound 2 MiB image.
Sign-extended guest addresses retain their low 32 bits. Unbound contexts,
scratchpad, external regions and invalid spans reach the checked runtime.
This avoids a resolver call and its register spills for common model accesses;
it does not widen a registered span or change guest pointer storage.
Unsupported address-space conversions and ABI shapes fail the build. The
existing fixed guest image addresses and console matching configuration remain
the source of truth; use the console matching targets below to verify the
resident and overlays byte for byte.

Retail-backed data keeps its PS1 addresses and initializers from the loaded
image. Definitions in `src/pc/game/` instead keep the upstream PC storage:
expanded card tables and widened Library cursor fields get independent native
allocations and registered guest tokens. Pinning them to their smaller retail
slots would overlap data. The IR regression checks this distinction. The MDEC
decoder also disables floating-point contraction so ARM64 movie pixels retain
the i386 reference rounding used by deterministic VRAM replays.

The game build pins LLVM 21.1.8 for guest IR translation and native game-code
generation; another Clang/LLVM version is not an equivalent build. Host
sanitizer test executables use Apple's `/usr/bin/clang`: the pinned LLVM 21
macOS ASan runtime hangs during shadow-memory initialization before `main`,
while Apple's sanitizer runtime runs these harnesses normally. This does not
change the compiler or pass used to translate game IR. The
`__ptr32` store canary checks 8-bit and 16-bit stores through guest pointers
before and after translation at `-O0` and `-O2`. It protects against a codegen
path widening a narrow guest store and corrupting an adjacent field.

## Native allocation boundary

Guest-visible allocations use the registered 32-bit token arena. A freed span
can be reused even when a larger temporary allocation moved the cursor above
it; exhaustion retries from the arena base before failing. The arena remains
256 MiB. Invalid accesses fail with the operation, requested size, region count,
live registered bytes, heap bytes and allocation cursor. These diagnostics are
prepared before abort, and the runtime writes them into the crash report itself,
beside the loaded Mach-O UUID and build identity.

External guest accesses cache a region index per 4 KiB page and validate the
complete requested span on every hit. Small allocations may share a page;
candidate misses use a guest-base-sorted index with logarithmic lookup. Registry
growth retains indices, and removal or reset invalidates page candidates with
an epoch. The page candidates occupy approximately 3.5 MiB of native storage,
outside guest memory and save payloads.
Encoding native pointers uses separate host-base-sorted indexes for functions
and data instead of scanning the entire registry. Function aliases keep their
first registered token; containing allocations take precedence over adjacent
one-past pointers. Registry growth and swap removal preserve both indexes.
External registrations reject spans that overlap RAM or scratchpad aliases,
and host spans that belong to the bound console image. Console map constants
are shared with the IR compiler, whose RAM-at-offset-zero assumption is
checked by the runtime.
RAM/scratchpad checks for explicit memory contexts share one inline implementation
with the translated runtime, avoiding a second runtime call. The fixed-address
resolver retains its mapping-specific checks. Endian helpers and resident-function
lookup also share their implementations across the backends. Indirect function lookups
use the generated map's address ordering to search logarithmically, then check
every equal-address candidate's current overlay residency. No callable target
is cached across a module switch; the interpreter fallback is unchanged.

Texture-pack tables, resampled pixels and decoded PNG caches are exclusively
host data with LP64 pointers. Their allocations retain native libc ownership
and consume no guest tokens. They are rebuilt by the renderer rather than
serialized as guest heaps. Game allocations, guest globals, mod storage and
callback bridges keep their registered memory contracts.

## Code and test entry points

The game driver separates source compilation, guest symbol/pin preparation,
IR lowering, function tables, mod exports and final linkage. `guest_build.py`
shares address tables and Darwin frontend flags with the mod builder; mods
retain their own ABI validation, registration/cleanup and dynamic linkage.
The LLVM pass keeps validation, pinning, instruction preparation/lowering,
hooks and global registration as ordered phases. Host renderer allocations
remain outside guest heaps. The translated mod flattens packet chains through
a native adapter and a resolver-independent helper. The fixed backend retains
its original loop; helper tests cover its packet ordering and length bits.

The isolated function runners compile their own current C sources through
`guest_test_ir.py`. Function selection uses LLVM definitions and preserves
removed functions as typed declarations, including internal dependencies.
`guest_test_compile.py` compiles guest objects with pinned LLVM, then links
host harnesses with the chosen sanitizer runtime. Guest objects do not mix
LLVM and Apple sanitizer ABIs; the IR/store canaries cover guest accesses.
Gameplay runners share process setup and controller timelines while keeping
scenario-specific assertions in their own runners.

### Test contracts and prerequisites

| Contract | Test entry | Inputs | Execution |
| --- | --- | --- | --- |
| PS1 sizes, offsets and narrow stores | `test_ps1_layouts.py`, `test_guest_storage_layout.py`, `test_guest_store_canary.py` | Pinned LLVM | CI/local, no disc |
| LLVM pointer/call lowering and typed extraction | `test_llvm_guest.py`, `test_translated_ir.py` | Pinned LLVM and macOS SDK | CI/local, no disc |
| Native regions, aliases, context and state payloads | `test_native.py` | macOS ARM64 and SDK | CI/local, normal and host sanitizers |
| Real game/SDK storage, callbacks and rules | `test_guest_contracts.py` | Current C sources, pinned LLVM and SDK | CI/local, 15 selectable contracts |
| Loader storage lifecycle and typed hooks | `test_arm64_mod_loader.py`, `test_arm64_mod_hooks.py` | Repository fixtures | CI/local, no disc or external mod |
| CoreText fonts and card-title plates | `test_macos_fonts.py` | Pinned host compiler, macOS SDK and bundled FreeType/PNG dependencies | CI/local, no disc |
| Linkage, paths, fonts and packaging | `test_macos_linkage.py`, `test_package_macos.py` | Current executable or package | CI/local, no disc |
| Gameplay, rewards, languages and saves | Gameplay/replay commands below | Current executable and private retail disc | Local; fixture modifications are stated by each runner |
| GPU presentation and normal HD duel | Window commands below | Window server, copied save and optional texture pack | Local, captures require visual inspection |
| Third-party mod behavior | `test_arm64_life_points.py --mod /path/to/mod` | Explicit external mod, current executable and retail disc | Optional local validation |

Tests at these levels establish different contracts. A layout assertion does
not replace an executing function test, and a synthetic fixture does not
replace gameplay. Keep focused regressions when they exercise a distinct
reader, writer or lifecycle, even if their setup is shared. Generated files,
logs and private inputs stay under ignored paths.

## Mods and save-state boundaries

Mod units disable implicit builtin synthesis, so optimized loops cannot
introduce unchecked libc calls. Character classification, including `isdigit`,
uses explicit runtime exports; the libSystem import guard stays restricted.

Data mods use the existing platform-independent data format. Code mods need a
separate native macOS ARM64 Mach-O `.dylib`, built from source with the game's
translated guest-memory contract. Existing i386 ELF `.o` code mods cannot be
loaded by the macOS process; Linux and Windows keep their i386 format. The
mod loader checks exports and ABI signatures, and rejects unsupported
constructors, destructors, and thread-local storage. See
[modding](modding.md#native-macos-arm64-code-mods) for the build details.

The bundled 3D Monsters mod reads battle models from the player's disc
(`DATA/MODEL.MRG`); no separate model pack is required. Its shared callback
and ordering tables use the game's canonical headers and guest-width pointer
declarations on the translated backend. Other ports retain their upstream
declarations and rendering path. A native-width declaration reads the wrong callback
slot and can silently prevent field rendering on ARM64.

Verify a packaged build with a real face-up fusion, model loading and a field
image comparison against the disabled mod:

```sh
python3 tools/pc/test_arm64_3d_monsters.py --binary "/path/to/YFM Re-Decomp.app/Contents/MacOS/memories-arm64" --mods "/path/to/YFM Re-Decomp.app/Contents/Resources/mods" --disc "/path/to/disc.bin"
```

Inspect the generated `tmp/3d-monsters-regression/on/terrain.png` for the
standing monster. This disc-backed test is separate from the ROM-free package
and loader checks.

Add `--stress --window --texture-mod /path/to/hd-mod --output tmp/3d-stress-new`
to measure 1, 3, 4, 5 and 10 different retail models in a 4x window with HD.
This deliberately changes only the isolated duel's field records. Inspect the
field captures alongside `on/timings.json`. Each stress scene checks that its
expected models loaded with nonempty geometry and that no packets were dropped.
The same scenes run with the mod disabled for image comparisons. After timing,
each of the ten models also appears alone in the central player zone; its image
must differ from the corresponding mod-off image. The timings exclude initial model
reads and client waits between frames, and are processing times rather than
normal-play FPS. Each scene measures five batches of 120 frames and reports
the median alongside every batch in `on/timings.json`; `--timing-batches`
changes the batch count. These are throughput measurements, not individual
frame percentiles. Use a fresh output directory when supplying a texture mod.

Optimized macOS builds keep the software GTE math and LIBGS ordering-table
adapter native, resolving guest spans at their entry points. The diagnostic
`--no-optimize` path retains full translation. Texture invalidation clears
contiguous wrapped spans, and the mod flattens its OT through a native adapter
which preserves packet order and command-length bits. These paths are guarded
for the translated backend; other ports retain the upstream texture-clear and
mod ordering-table loops.

Native `.state` files include machine registers and stack data. They cannot
cross 32-bit/64-bit or operating-system builds. The macOS loader requires the same build identity and Mach-O UUID; it
relocates host pointers when ASLR moves that executable in a new process. The
loader also checks the active mod set, mod state layouts, language, and text
layout. Use ordinary `.sav` files to move a game between builds. See the save-state compatibility section below.

Save request scheduling and path construction are shared across backends.
Decimal slots and frame settings reject overflow and invalid digits; autosave
uses elapsed unsigned frames across counter wrap. Oversized paths fail before
I/O, and autosaves inherit `MEMORIES_STATE_DIR` unless explicitly overridden.
Backend stack capture, restore and serialized pointer layouts stay separate.

## Validation matrix

Run commands from the repository root. These are reproducible checks to run;
this page does not claim that they have been executed for a particular build.
Replace the example input paths with local files. Keep retail files and
generated output under ignored paths.

| Check | Command | What it covers |
| --- | --- | --- |
| Build and Mach-O linkage | `python3 tools/pc/build.py --target macos`<br>`python3 tools/pc/test_macos_linkage.py --binary tmp/pc/macos/memories-arm64` | Build the ARM64 executable; inspect its architecture, linked libraries, and font rendering. |
| Guest storage and narrow stores | `python3 tools/pc/test_guest_storage_layout.py`<br>`python3 tools/pc/test_guest_store_canary.py` | Check guest storage layouts and 8/16-bit stores through `__ptr32` at `-O0` and `-O2`. |
| Real guest functions, without a disc | `python3 tools/pc/test_guest_contracts.py`<br>`python3 tools/pc/test_guest_contracts.py --sanitize` | Recompile selected current game/SDK functions with pinned LLVM, then execute storage, callback, ritual/trap and variadic contracts. No existing game IR is read. Use `--case` for one contract. |
| LLVM translation and call bridges | `python3 tools/pc/test_llvm_guest.py`<br>`python3 tools/pc/test_translated_ir.py --optimize`<br>`python3 tools/pc/test_native_call_marshalling.py`<br>`python3 tools/pc/test_direct_overlay_bridge.py` | Check IR contracts, native-call ABI handling, and direct overlay imports. |
| Exact console output | `make clean`<br>`MAKEFLAGS=-j"$(sysctl -n hw.ncpu)" make match`<br>`MAKEFLAGS=-j"$(sysctl -n hw.ncpu)" make match-overlays` | Compare the retail resident and configured overlay modules byte for byte. Requires all ignored retail inputs and the repository's matching toolchain. Run from the root, with the targets sequentially. |
| Deterministic menu replay | `python3 tools/pc/replay.py play tests/pc/replays/menus --check --executable tmp/pc/macos/memories-arm64` | Replay the existing virtual-clock route through title, options, deck, Library, Password, map, and Free Duel menus. |
| Local replay repeatability | `python3 tools/pc/test_replay_determinism.py --replay tests/pc/replays/menus --binary tmp/pc/macos/memories-arm64 --out tmp/pc/replay-determinism` | Play the same recorded input in two fresh processes and compare every H hash and the E end marker. |
| Gameplay and deck menus | `python3 tools/pc/test_arm64_gameplay.py --disc "/path/to/disc.bin" --binary tmp/pc/macos/memories-arm64` | Run all gameplay cases, including duel results, Free Duel, deck editing, traps, rituals, and animated battle. |
| Complete Simon duel, French | `python3 tools/pc/test_arm64_full_duel.py --disc "/path/to/disc.bin" --binary tmp/pc/macos/memories-arm64 --language 2` | Reuse the upstream adaptive session: controlled eight-card deck, 8000 LP, fusion, magic, 3D attacks, results, reward and Free Duel return, without mods. Use `--language 0` for English. |
| Campaign, English by default | `python3 tools/pc/test_arm64_campaign.py --disc "/path/to/disc.bin" --binary tmp/pc/macos/memories-arm64` | Exercise the village sequence, exit, return, and story flags. Add `--all-languages` for optional French coverage; use `--language 2` for French alone. |
| Result names, English and French | `python3 tools/pc/test_arm64_result_text.py --disc "/path/to/disc.bin" --binary tmp/pc/macos/memories-arm64` | Check all three result pages with opponent names on and off, navigation, rewards, and campaign return. |
| Save-state replay and control | `python3 tools/pc/test_arm64_states.py --disc "/path/to/disc.bin" --binary tmp/pc/macos/memories-arm64`<br>`python3 tools/pc/test_arm64_state_control.py --disc "/path/to/disc.bin" --binary tmp/pc/macos/memories-arm64` | Check fresh-process replay, matching pixels, F5/F7, invalid-state rejection, RAM restore, and autosaves. The control runner uses loopback. |
| macOS HD presentation | `python3 tools/pc/test_macos_hd_render.py --disc "/path/to/disc.bin" --binary tmp/pc/macos/memories-arm64 --texture-mod "/path/to/hd-mod" --save "/path/to/copied-save.sav" --cases 1x 2x 4x effects` | Exercise the real window and GPU presenter with isolated settings. Requires the macOS window server; use a copy of a normal save. |
| Normal Free Duel with HD | `python3 tools/pc/test_arm64_full_duel.py --binary "/path/to/extracted/app/Contents/MacOS/memories-arm64" --disc "/path/to/disc.bin" --save "/path/to/save.sav" --texture-mod "/path/to/hd-mod" --window` | Copy a save, load it from the title menu, select Simon in Free Duel, play through results/rewards and return; no debug jump or deck replacement. Optional `--code-mod` adds an isolated code mod; `--restore` resumes the hand in a fresh process and `--animated` selects 3D attacks. The normal route respects the opening-turn attack restriction. |
| macOS font rendering | `python3 tools/pc/test_macos_fonts.py --sanitize` | Check CoreText discovery, glyph rasterization and card-title plate bounds, accents and repeatability; no disc needed. |
| Code-mod loader and hooks | `python3 tools/pc/test_arm64_mod_loader.py --sanitize`<br>`python3 tools/pc/test_arm64_mod_hooks.py --sanitize` | Check Mach-O loading/cleanup and typed ARM64 hook lifecycle under sanitizers. The loader builds its own translated fixture and rejected ELF object; no external mod is required. |

The checks establish separate evidence: successful linkage does not prove
gameplay, synthetic ABI tests do not prove a mod's behavior, and a selected
campaign route does not establish completion of the full campaign. A loopback
bind failure is an environment failure, not a passing control test. The HD
runner captures a presented window; inspect its capture for visual content.

Local replay repeatability only shows that two fresh processes of the same
local build produce identical H hashes and E markers. It does not establish
conformity with the Windows recording used as the `menus` golden; use
`replay.py play ... --check` for that cross-platform comparison. The repeatability
runner writes its two process outputs under `--out` and never updates replay
fixtures or goldens.

The `x64-data-mods` replay builds a recolored texture mod with Pillow. Install
Pillow into the Python environment used to run replays (`python3 -m pip install
Pillow`) before running `python3 tools/pc/replay.py play
tests/pc/replays/x64-data-mods --check`; other replay fixtures do not need it.


## Save-state use and compatibility

Both backends share slot paths, startup load, scripted saves, autosave rotation
and manual save-failure notices. Stack capture and payload restoration remain
backend-specific. Invalid or overflowing autosave intervals disable autosave.

Native save states capture a running game at a frame boundary, including PS1
RAM and scratchpad, the translated CPU context and stack, game allocations,
and runtime subsystem state. They are different from memory-card saves
(`.sav`), which remain the portable way to continue a game across versions and
platforms.

### Use

- Press **F5** to save to the selected slot and **F7** to load it.
- The save/load menu actions and the local control channel use the same state
  backend.
- Set `MEMORIES_LOAD_STATE` to a state path to load at startup. A slot number
  selects that slot instead.
- Set `MEMORIES_AUTOSAVE=<seconds>` to enable the three rotating autosaves.
- `MEMORIES_STATE_DIR` and `MEMORIES_AUTOSAVE_DIR` select state directories.
  By default, states are stored under the application's user directory.

The writer stages a `.partial` file, adds an integrity check, then renames it
into place. Truncated states or states whose integrity check fails are rejected
before restoration.

### Compatibility

Save states contain native stack frames and registers, so they cannot move
between 32-bit and 64-bit builds or between operating systems. A state from a
different build is rejected: the macOS loader requires the same build
identity and Mach-O UUID. Within that executable, host addresses are remapped
across ASLR when restoring in a new process. Resume with the same ARM64
executable and mod profile that created the state.

The loader also checks active mods, mod state layouts, language, and compiled
text. Changing a code or data mod, translation, or game build can make a state
incompatible. Keep a normal `.sav` for transfer between builds. States from a
PS1 emulator are not converted. Audio resumes from the disc position and is
not guaranteed to reproduce identical samples.

### Local checks

From the repository root, build first, then run the state replay and control
checks with a legally obtained USA disc:

```sh
DISC="game/YGOFM Vanilla (Base).bin"
python3 tools/pc/build.py --target macos
python3 tools/pc/test_arm64_states.py --disc "$DISC" --binary tmp/pc/macos/memories-arm64
python3 tools/pc/test_arm64_state_control.py --disc "$DISC" --binary tmp/pc/macos/memories-arm64
```

The replay check resumes a saved game in a fresh process, compares gameplay
data and pixels, and exercises F5/F7 through SDL's dummy input path. The
control check covers repeated loads, RAM replay, rejected incompatible or
damaged files, and rotating autosaves. These checks do not validate visible
window presentation or audio output.
