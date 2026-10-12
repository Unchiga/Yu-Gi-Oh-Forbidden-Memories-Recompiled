# Nintendo Switch (homebrew)

The Switch build is a homebrew `.nro` for a console running custom firmware
(Atmosphère), started from the Homebrew Menu. It is the macOS ARM64 port's
translated memory model (`translated-32`, [macOS ARM64](macos-arm64.md)) on
devkitPro's newlib and libnx, with SDL2 standing in for SDL3.

## Playing

Unpack `yfm-redecomp-<version>-switch.zip` from a release onto the SD card's
root, or copy `yfm-redecomp-<version>-switch.nro` alone into `sdmc:/switch/`:

```
sdmc:/switch/yfm-redecomp.nro
sdmc:/switch/yfm-redecomp/game/       your USA disc's .bin (SLUS-01411)
sdmc:/switch/yfm-redecomp/mods/       one folder per mod
sdmc:/switch/yfm-redecomp/languages/  the European translations (from the ZIP)
sdmc:/switch/yfm-redecomp/user/       saves, save states and settings
sdmc:/switch/yfm-redecomp/log.txt     the last start's log
```

The `.nro` makes `game/`, `mods/` and `user/` on its first start
(`src/pc/platform/switch.c`, `make_folders`). The disc is always read from
`game/`: there is no file picker.

Start it **in place of a game**: hold **R** while starting any game, then
choose it in the Homebrew Menu. Started from the album, homebrew runs as an
applet with a few hundred MiB, and the game says so instead of starting.

### Controls

| Button | Does |
|---|---|
| **R3** (press the right stick) | Shows the menu bar, and hides it again. The bar starts hidden. |
| **L** / **R** (bar shown) | Previous / next menu. The bar shows **L** and **R** badges at its ends. |
| **D-pad** (bar shown) | Moves in the open menu; left and right change a slider or open a submenu. |
| Confirm / back (bar shown) | Choose an item / close the menu. With no menu open, back hides the bar. |
| **L3** (press the left stick) | Fast forward on, and off again on the next press (the PC's held Tab). |
| Touch screen | Taps on the bar choose its menus and items. |

While the bar is shown, the controller drives the menus and the game gets no
buttons. Every game button is rebindable in **Game > Controls**; R3, L3, L
and R as above are the Switch build's defaults (`src/pc/platform/sdl.c`,
`src/pc/platform/controls.c`).

Save states are in **File > Save state / Load state**, with **File > State
slot** choosing the slot. A state is tied to the exact `.nro` that made it
(its GNU build ID): a state from another build is refused.

### What differs from the PC builds

- **Data mods only.** Horizon gives homebrew no executable memory of its own,
  so code mods do not load (`MEMORIES_NO_CODE_MODS`). Texture packs (the HD
  pack too), card, fusion, drop and text mods work (**Game > Mods**);
  3D Monsters, Hand Camera, AI Hard Mode and Yamyi Mods do not. The release
  ZIP ships Drop Missing Cards only.
- **HD pack:** put `assets-hd` in `sdmc:/switch/yfm-redecomp/mods/`. At
  internal 1x it is resampled to the console's size and hardly shows; pick
  **Video > Resolution > Internal 2x** or higher. 4x is heavy for the
  Switch's GPU.
- No update check and no file picker.
- The log goes to `log.txt` in the game's folder, or to the PC's terminal
  when started with `nxlink -s`.

## Building

The build runs on macOS (Apple Silicon), where the pinned LLVM 21 and its
guest IR pass come from ([macOS build](macos-build.md)), plus devkitPro for
the Switch's C library, libnx and the link:

1. Install devkitPro's pacman
   ([devkitPro's guide](https://devkitpro.org/wiki/Getting_Started)), then:

   ```sh
   sudo dkp-pacman -S switch-dev switch-sdl2 switch-mesa switch-freetype \
       switch-libpng switch-zlib switch-harfbuzz
   ```

   `DEVKITPRO` defaults to `/opt/devkitpro`. `pkg-config` must be on the
   `PATH` (Homebrew's `pkgconf`).

2. Build:

   ```sh
   python3 tools/pc/build.py --target switch
   ```

   The `.nro` is `tmp/pc/switch/memories.nro`, beside its ELF and the build's
   logs. Building needs no disc.

3. Send it to the console with `nxlink -s tmp/pc/switch/memories.nro` (the
   Homebrew Menu's netloader, Y), or copy it onto the SD card.

`tools/pc/switch_deps.py` holds the toolchain: clang compiles for
`aarch64-none-elf` against devkitA64's newlib and libnx headers, devkitA64's
GCC links with libnx's `switch.specs`, and `elf2nro` packs the result with
`tools/pc/switch/icon.jpg` (its source is `icon.svg`).

### Toolchain details

- `-mtp=el0 -femulated-tls`: libnx keeps the thread pointer behind
  `TPIDRRO_EL0`, which clang cannot read inline; thread locals go through
  libgcc's emulation.
- `-Wl,--wrap=rename`: the SD card's `rename` does not replace an existing
  file, and the port saves by writing aside and renaming over
  (`src/pc/platform/switch_posix.c`).
- `-Wl,--build-id=sha1`: names the build for save states.
- `-DMEMORIES_NO_CODE_MODS`: see above.
- `src/pc/compat/switch/` stands in for what newlib and libnx lack:
  `sys/mman.h` (anonymous mappings from the heap), fontconfig (the system's
  shared font) and SDL3 (`SDL3/sdl3_shim.h` over SDL2,
  `src/pc/platform/switch_sdl3.c`). It stays out of the mod SDK.
- libnx's `switch.ld` defines `__start__` as an absolute 0, not moved with
  the image: the image's start is crt0's `_start`.

### Memory

Horizon places a process's stacks, the loader and thread-local pages below
4 GiB, where the translated runtime's guest tokens are. On the Switch the
token space is cut in two: `[0, 0x20000000)` and `[0x80000000, 4 GiB)` are the
game's, and addresses in `[0x20000000, 0x80000000)` are the host's
(`MEMORIES_HOST_LOW_START`/`END` in `src/pc/memory_map.h`;
`GuestRuntime_IsHostAddress` in `src/pc/guest/translated_runtime.h`; the IR
pass's resolver in `tools/pc/llvm/guest_ir.cpp`, for `aarch64` with an unknown
OS only). At start (`src/pc/platform/switch.c`):

- `userAppInit` reserves both token ranges in libnx's virtual memory manager,
  so no thread stack or mapping libnx makes lands there.
- `Switch_Main` moves `main` onto a stack of its own when the loader started
  it on one in a token range.
- `check_layout` refuses to run when anything else of the process (other than
  the loader's code) was placed in a token range before the reservation; a
  new start (a new layout) resolves it.

### Packaging and releases

```sh
MEMORIES_VERSION=v0.3.0 python3 tools/pc/build.py --target switch
python3 tools/pc/package_switch.py --version v0.3.0
```

writes to `dist/`:

- `yfm-redecomp-<version>-switch.nro`: the program alone;
- `yfm-redecomp-<version>-switch.zip`: unpacked onto the SD card's root,
  `switch/yfm-redecomp.nro` and `switch/yfm-redecomp/` with a README, the
  license, `languages/`, `game/` and the shipped data mods;
- a `.sha256` beside each.

The `package-switch` job of `.github/workflows/pc-release.yml` does the same
on a `macos-15` runner, installing devkitPro there, and a `v*` tag's draft
release carries both files. A manual run can build the Switch alone
(**Run workflow**, platform `switch`).
