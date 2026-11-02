#!/bin/sh
# Renders the Mods and Controls windows, and with PREVIEW_TOUCH=1 their
# panels inside the game's window on phones and tablets, to PPM files
# (tests/pc/panels_preview.c). PREVIEW_OUT names the folder (default
# tmp/pc/panels). Needs a C compiler, FreeType, fontconfig and libpng.
set -eu
cd -- "$(dirname -- "$0")/../.."
out=${PREVIEW_OUT:-tmp/pc/panels}
mkdir -p "$out"
cc -std=gnu11 -O2 -DPREVIEW_PANELS -ffunction-sections -fdata-sections -Wl,--gc-sections -Isrc \
    $(pkg-config --cflags freetype2 fontconfig) tests/pc/panels_preview.c src/pc/platform/panel.c \
    src/pc/platform/mods_window.c src/pc/mods/mods.c src/pc/mods/manager.c src/pc/mods/overlap.c src/pc/mods/events.c \
    src/pc/mods/hooks.c src/pc/mods/import.c src/pc/mods/hd_pack.c src/pc/mods/import_job.c src/pc/platform/update.c src/pc/mods/mod_libc.c src/pc/mods/object_loader.c src/pc/mods/json.c src/pc/guest/branch_thunks.c \
    src/pc/platform/paths.c src/pc/platform/settings.c src/pc/text/utf8.c \
    src/pc/platform/controls.c src/pc/platform/controls_config.c src/pc/platform/controls_runtime.c \
    src/pc/platform/controls_art.c src/pc/platform/controls_window.c \
    -lpng16 -lz -lm -lpthread $(pkg-config --libs freetype2 fontconfig) -o tmp/pc/panels-preview
MEMORIES_MODS_DIR="$PWD/mods" MEMORIES_SETTINGS="$out/settings.txt" MEMORIES_USER_DIR="$out" \
    MEMORIES_CONTROLS="$out/controls.txt" PREVIEW_OUT="$out" tmp/pc/panels-preview
