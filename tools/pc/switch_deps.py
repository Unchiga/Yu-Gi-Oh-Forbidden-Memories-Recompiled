"""devkitPro toolchain and portlibs for the Nintendo Switch homebrew build.

The game is compiled by the pinned LLVM (the guest IR pass needs it) for
aarch64-none-elf against devkitA64's newlib and libnx headers, then linked
by devkitA64's GCC with libnx's switch.specs and packed by elf2nro.
SDL3 has no public Switch port, so the platform layer runs on devkitPro's
switch-sdl2 through src/pc/compat/switch/sdl3_shim.h.
"""

import os
import subprocess
from pathlib import Path

DEVKITPRO = Path(os.environ.get("DEVKITPRO", "/opt/devkitpro"))
A64 = DEVKITPRO / "devkitA64"
LIBNX = DEVKITPRO / "libnx"
PORTLIBS = DEVKITPRO / "portlibs/switch"
TOOLS = DEVKITPRO / "tools/bin"
ROOT = Path(__file__).resolve().parents[2]

ARCH = ["-march=armv8-a+crc+crypto", "-mtune=cortex-a57"]
REQUIRED = {
    "libSDL2.a": "switch-sdl2",
    "libz.a": "switch-zlib",
    "libpng16.a": "switch-libpng",
    "libfreetype.a": "switch-freetype",
    "libharfbuzz.a": "switch-harfbuzz",   # freetype2.pc requires it
    "libEGL.a": "switch-mesa",
}


def ensure(link=True):
    """Return the portlibs prefix, or stop with the packages to install
    (compiling needs their headers; linking, harfbuzz as well)."""
    if not (LIBNX / "switch.specs").is_file() or not (A64 / "bin/aarch64-none-elf-gcc").is_file():
        raise SystemExit(f"devkitA64 and libnx not found under {DEVKITPRO} (set DEVKITPRO)")
    missing = sorted({package for name, package in REQUIRED.items()
                      if not (PORTLIBS / "lib" / name).is_file() and (link or name != "libharfbuzz.a")})
    if missing:
        raise SystemExit("Missing Switch portlibs; run: sudo dkp-pacman -S " + " ".join(missing))
    return PORTLIBS


def compile_flags():
    """Host flags for every unit: clang for the Switch's newlib/libnx ABI."""
    return [
        "--target=aarch64-none-elf",
        *ARCH,
        "-mtp=el0",
        # libnx keeps the thread pointer behind TPIDRRO_EL0 (__aarch64_read_tp),
        # which clang cannot read inline: thread locals through libgcc instead.
        "-femulated-tls",
        "-fPIE",
        "-ffunction-sections",
        "-fdata-sections",
        "-D__SWITCH__",
        "-DMEMORIES_NO_CODE_MODS",   # no executable memory of its own on Horizon: data mods only
        # Stand-ins for what newlib/libnx lack: sys/mman.h, fontconfig, SDL3.
        "-I" + str(ROOT / "src/pc/compat/switch"),
        "-isystem", str(LIBNX / "include"),
        "-isystem", str(PORTLIBS / "include"),
        "-isystem", str(A64 / "aarch64-none-elf/include"),
    ]


def nm():
    return str(A64 / "bin/aarch64-none-elf-nm")


def link(objects, output):
    """The devkitA64 link command for an ELF that elf2nro accepts."""
    # The host's pkg-config, confined to the portlibs: devkitPro's own
    # wrapper is a separate package (switch-pkg-config).
    environment = dict(os.environ, PKG_CONFIG_LIBDIR=str(PORTLIBS / "lib/pkgconfig"), PKG_CONFIG_PATH="")
    libs = subprocess.check_output(
        ["pkg-config", "--static", "--libs", "sdl2", "glesv2", "freetype2", "libpng", "zlib"],
        text=True, env=environment,
    ).split()
    return [
        str(A64 / "bin/aarch64-none-elf-gcc"),
        "-specs=" + str(LIBNX / "switch.specs"),
        *ARCH,
        "-mtp=soft",
        "-fPIE",
        "-Wl,--gc-sections",
        "-Wl,--build-id=sha1",   # names the build for save states (translated_state_backend.c)
        "-Wl,--wrap=rename",     # one that replaces, as on POSIX (switch_posix.c)
        *objects,
        "-L" + str(PORTLIBS / "lib"),
        "-L" + str(LIBNX / "lib"),
        *libs,
        "-lnx",
        "-lm",
        "-o",
        str(output),
    ]


def package(elf, nro, version, icon=None):
    """Wrap the ELF as a homebrew .nro with its title metadata."""
    nacp = nro.with_suffix(".nacp")
    subprocess.check_call(
        [str(TOOLS / "nacptool"), "--create", "YFM Re-Decomp", "Unchiga", version, str(nacp)]
    )
    # 256x256 JPEG; tools/pc/switch/icon.svg is its source.
    icon = icon or (ROOT / "tools/pc/switch/icon.jpg")
    subprocess.check_call(
        [str(TOOLS / "elf2nro"), str(elf), str(nro), "--icon=" + str(icon), "--nacp=" + str(nacp)]
    )
    return nro
