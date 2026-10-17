"""Shared PC target metadata, game inventory, and native source selection."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Declared deployment baseline; runtime compatibility is validated separately.
MACOS_MINIMUM = "11.0"

# Keep host platform, machine architecture, native calling ABI, and guest
# memory strategy explicit.  In particular, Windows x64 is still the fixed
# address G32 build; only macOS ARM64 uses translated guest memory.
TARGETS = {
    "linux": {
        "platform": "linux",
        "architecture": "i386",
        "abi": "sysv-ilp32",
        "memory_model": "fixed-32",
        "guest_assembly": "i386",
        "driver": "build_game32.py",
        "build": "tmp/pc/game32",
        "executable": "memories-pc",
    },
    "windows": {
        "platform": "windows",
        "architecture": "i686",
        "abi": "windows-ilp32",
        "memory_model": "fixed-32",
        "guest_assembly": "i386",
        "driver": "build_game32.py",
        "build": "tmp/pc/win32",
        "executable": "memories-pc.exe",
    },
    "windows-x64": {
        "platform": "windows",
        "architecture": "x86_64",
        "abi": "windows-llp64",
        "memory_model": "fixed-32",
        "guest_assembly": "x86_64",
        "driver": "build_game32.py",
        "build": "tmp/pc/win64",
        "executable": "memories-pc.exe",
    },
    "android-arm64-v8a": {
        "platform": "android",
        "architecture": "arm64",
        "abi": "android-lp64",
        "memory_model": "fixed-32",
        "guest_assembly": "aarch64",
        "driver": "build_game32.py",
        "build": "tmp/pc/android-arm64-v8a",
        "executable": "libgame.so",
    },
    "android-x86": {
        "platform": "android",
        "architecture": "i386",
        "abi": "android-ilp32",
        "memory_model": "fixed-32",
        "guest_assembly": "i386",
        "driver": "build_game32.py",
        "build": "tmp/pc/android-x86",
        "executable": "libgame.so",
    },
    # Android arm64 on the translated backend (build_arm64.py --target
    # android-arm64-translated): no fixed host address anywhere
    # (notes/pc-build.md, "Android arm64").
    "android-arm64-translated": {
        "platform": "android",
        "architecture": "arm64",
        "abi": "android-lp64",
        "memory_model": "translated-32",
        "guest_assembly": "translated-arm64",
        "driver": "build_arm64.py",
        "build": "tmp/pc/android-arm64-translated",
        "executable": "libgame.so",
    },
    "macos": {
        "platform": "macos",
        "architecture": "arm64",
        "abi": "darwin-lp64",
        "memory_model": "translated-32",
        "guest_assembly": "translated-arm64",
        "driver": "build_arm64.py",
        "build": "tmp/pc/macos",
        "executable": "memories-arm64",
    },
}

BACKENDS = {
    "sdl": [
        "src/pc/platform/sdl.c",
        "src/pc/render/gl_picture.c",
        "src/pc/render/present_pass.c",
    ],
    "x11": [
        "src/pc/platform/x11.c",
        "src/pc/platform/audio_alsa.c",
        "src/pc/platform/gamepad_evdev.c",
    ],
}

MODULES = [
    ("main_menu", "src/overlays/main_menu/*.c", 0x0F, 0),
    ("password", "src/overlays/password/*.c", 0x15, 0x80168000),
    ("overworld", "src/overlays/overworld/*.c", 0x14, 0x80168000),
    ("free_duel", "src/overlays/free_duel/*.c", 0x13, 0x80168000),
    ("duel_effects", "src/overlays/duel_effects/*.c", 0x18, 0x80146000),
    ("credits", "src/overlays/credits/*.c", 0x10, 0x80180000),
]
MODULE_CONFIG = {"overworld": "overworld_before_coup"}
GATED_MODULES = {"duel_effects", "credits"}

COMMON_FILES = [
    "rng.c",
    "compat/fs.c",
    "compat/gte.c",
    "compat/pgxp.c",
    "compat/libgs_ot.c",
    "render/packets.c",
    "render/soft_gpu.c",
    "render/texture_dump.c",
    "render/texture_pack.c",
]
FIXED_MEMORY = {
    "src/pc/guest/resolve.c",
    "src/pc/guest/image.c",
    "src/pc/guest/branch_thunks.c",
    "src/pc/guest/state.c",
}


def game_sources():
    """Return the shared resident and overlay inventory for every target."""
    groups = {
        "resident": sorted(
            path.relative_to(ROOT).as_posix()
            for directory in ("src/game", "src/pc/game")
            for path in (ROOT / directory).glob("*.c")
        )
    }
    groups.update(
        (name, sorted(path.relative_to(ROOT).as_posix() for path in ROOT.glob(pattern)))
        for name, pattern, _, _ in MODULES
    )
    return groups


def native_sources(target, backend="sdl"):
    """Select native C and assembly from a complete target descriptor."""
    if not isinstance(target, dict):
        raise ValueError("A complete PC target descriptor is required")
    architecture = target.get("guest_assembly")
    memory_model = target.get("memory_model")
    if architecture not in {"i386", "x86_64", "aarch64", "translated-arm64"}:
        raise ValueError("Unsupported guest assembly target: " + str(architecture))
    if memory_model not in {"fixed-32", "translated-32"}:
        raise ValueError("Unsupported guest memory model: " + str(memory_model))
    if (architecture == "translated-arm64") != (memory_model == "translated-32"):
        raise ValueError(
            "Translated ARM64 assembly and memory model must be selected together"
        )
    if backend is not None and backend not in BACKENDS:
        raise ValueError("Unsupported PC backend: " + str(backend))

    patterns = [
        "guest/*.c",
        "sdk/*.c",
        "platform/*.c",
        "overlays/*.c",
        "overrides/*.c",
        "audio/*.c",
        "mods/*.c",
        "debug/*.c",
        "cards/*.c",
        "free_duel/*.c",
        "saves/*.c",
        "text/*.c",
        "tower/*.c",
    ]
    sources = {
        path.relative_to(ROOT).as_posix()
        for pattern in patterns
        for path in (ROOT / "src/pc").glob(pattern)
    }
    sources.update("src/pc/" + path for path in COMMON_FILES)
    backend_sources = {source for files in BACKENDS.values() for source in files}
    sources.discard("src/pc/platform/android_loader.c")
    sources.difference_update(backend_sources)
    if backend:
        sources.update(BACKENDS[backend])

    assembly = sorted(
        path.relative_to(ROOT).as_posix()
        for path in (ROOT / "src/pc/guest").glob("*.S")
        if path.name.endswith(
            {
                "i386": "_i386.S",
                "x86_64": "_x86_64.S",
                "aarch64": "_aarch64.S",
                "translated-arm64": "_arm64.S",
            }[architecture]
        )
    )
    sources.difference_update(FIXED_MEMORY)
    if memory_model == "fixed-32":
        # The fixed-address engine and branch thunks are part of the memory
        # contract shared by i386, i686, Windows x64, and Android ARM64.
        sources.update(FIXED_MEMORY)
        sources.update(assembly)
        sources = {
            source
            for source in sources
            if not source.startswith("src/pc/guest/translated")
            and source != "src/pc/guest/state_translated.c"
        }
        sources.discard("src/pc/memory.c")
        sources.discard("src/pc/guest/state_translated.c")
    else:
        sources.add("src/pc/memory.c")
        sources.add("src/pc/guest/state_translated.c")
        sources.update(
            path.relative_to(ROOT).as_posix()
            for path in (ROOT / "src/pc/guest").glob("translated*.S")
        )
    return sorted(sources)
