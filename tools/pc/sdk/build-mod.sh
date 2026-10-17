#!/bin/sh
# build-mod.sh: build a Memories code mod (every .c in its folder) into its
# objects, with nothing installed but python3 and a shell.
#
#   ./build-mod.sh my-mod                  # my-mod/<library>.o, .x86_64-windows.o, .aarch64-translated.o
#   ./build-mod.sh my-mod --target i386    # anything after the folder goes to tools/build_mod.py
#   ./build-mod.sh --setup                 # only fetch the compiler (below), then stop
#
# The first run downloads the compiler (llvm-mingw, the toolchain the game is
# built with: clang, ld.lld and llvm-objcopy) into toolchain/ beside this file,
# checks its SHA-256 against the hash written below, and unpacks it. Later
# runs find it there and download nothing. Delete toolchain/ to start over.
# Nothing else is downloaded, installed or changed on your system.
#
# Your own tools instead (nothing is downloaded for what you name):
#   MEMORIES_SDK_PYTHON     the python to run tools/build_mod.py with (default python3)
#   MEMORIES_SDK_TOOLCHAIN  a folder with clang, ld.lld and llvm-objcopy in it,
#                           or in its bin/ (a llvm-mingw you unpacked yourself)
# A clang from your distribution (sudo apt install clang lld llvm) builds the
# i386 and x86_64-windows objects, not the Android one (aarch64-translated):
# that goes through the SDK's guest pass, which is built against LLVM 23, and
# the clang that writes the IR must be of the same LLVM major.
#
# The downloaded llvm-mingw is the ubuntu-22.04 build: its clang needs glibc 2.34
# and the libstdc++ of GCC 11 or newer (Ubuntu 22.04, Debian 12, Fedora 35...).
# On an older system (Debian 11, Ubuntu 20.04) it does not start, and the script
# says so; name a clang 23 of your own with MEMORIES_SDK_TOOLCHAIN then.

set -eu

# What is downloaded: the same values as toolchain-pins.json (the launchers
# cannot read it; tools/pc/test_sdk_launchers.py keeps them the same).
LLVM_RELEASE="20260922"
LLVM_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-ucrt-ubuntu-22.04-x86_64.tar.xz"
LLVM_SHA256="bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21"
LLVM_MEGABYTES="84"
# The only protocol curl and wget may use, also after a redirect (the tests of
# this file name another, for archives made on the spot).
ALLOWED="https"
# What tools/build_mod.py needs of python.
PYTHON_MINIMUM_MAJOR="3"
PYTHON_MINIMUM_MINOR="8"

here=$(cd "$(dirname "$0")" && pwd)
toolchain="$here/toolchain"

fail() {
    echo "build-mod: $*" >&2
    exit 1
}

usage() {
    cat <<EOF
Usage: build-mod.sh MOD_FOLDER [build_mod.py options]
       build-mod.sh --setup

Builds the code mod in MOD_FOLDER (its mod.json and .c files) for the 32-bit
games (<library>.o), the 64-bit Windows game (<library>.x86_64-windows.o) and
the Android app (<library>.aarch64-translated.o), next to the mod's files.
Options after the folder go to tools/build_mod.py (--target, --out, ...).
With no folder, the current one is used if it holds a mod.json.

First run: downloads llvm-mingw $LLVM_RELEASE (about $LLVM_MEGABYTES MB) into
$toolchain, checks its SHA-256 and unpacks it. Needs python3 ($PYTHON_MINIMUM_MAJOR.$PYTHON_MINIMUM_MINOR or newer), curl or wget, tar with xz, Linux x86-64 with
glibc 2.34 and libstdc++ from GCC 11 or newer (Ubuntu 22.04, Debian 12, Fedora 35...).

MEMORIES_SDK_PYTHON     python to use instead of python3
MEMORIES_SDK_TOOLCHAIN  folder with clang, ld.lld and llvm-objcopy (or its bin/)
                        to use instead of the download. A clang from your
                        distribution builds the i386 and x86_64-windows
                        objects but not the Android one: the SDK's guest pass
                        is LLVM 23, and the clang must be the same LLVM major.
EOF
}

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d ' ' -f 1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d ' ' -f 1
    else
        fail "needs sha256sum (coreutils) to check the download"
    fi
}

download_failed() {
    rm -f "$2.part"
    fail "could not download $1
Check the internet connection. Behind a company proxy or firewall that inspects HTTPS, the
certificate checks can stop the download (the message above may say 'certificate'): this script
does not switch them off. Ask whoever runs the network, or download the file from the address
above in a browser and put it in $toolchain/downloads/ under its own name; the next run checks
it and uses it."
}

download() {
    # $1 url, $2 file
    if command -v curl >/dev/null 2>&1; then
        curl --fail --location --retry 3 --proto "=$ALLOWED" --proto-redir "=$ALLOWED" --output "$2.part" "$1" ||
            download_failed "$1" "$2"
    elif command -v wget >/dev/null 2>&1; then
        if [ "$ALLOWED" = https ]; then
            wget --https-only --output-document "$2.part" "$1" || download_failed "$1" "$2"
        else
            wget --output-document "$2.part" "$1" || download_failed "$1" "$2"
        fi
    else
        fail "needs curl or wget to download $1 (or download it yourself to $2)"
    fi
    mv "$2.part" "$2"
}

fetch_llvm() {
    folder="$toolchain/llvm-mingw-$LLVM_RELEASE"
    if [ -x "$folder/bin/clang" ]; then
        return 0
    fi
    [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = x86_64 ] ||
        fail "the compiler download is for Linux x86-64; on this system set MEMORIES_SDK_TOOLCHAIN to a folder with clang, ld.lld and llvm-objcopy"
    archive="$toolchain/downloads/$(basename "$LLVM_URL")"
    mkdir -p "$toolchain/downloads"
    if [ -f "$archive" ] && [ "$(sha256_of "$archive")" != "$LLVM_SHA256" ]; then
        rm -f "$archive"   # a half download or another file: fetch it again
    fi
    if [ ! -f "$archive" ]; then
        echo "build-mod: downloading llvm-mingw $LLVM_RELEASE (about $LLVM_MEGABYTES MB)"
        echo "build-mod:   from $LLVM_URL"
        download "$LLVM_URL" "$archive"
    fi
    actual=$(sha256_of "$archive")
    if [ "$actual" != "$LLVM_SHA256" ]; then
        rm -f "$archive"
        fail "the SHA-256 of the download is wrong, so nothing was unpacked and the file was deleted.
  expected $LLVM_SHA256
  got      $actual
  from     $LLVM_URL
Try again. If it repeats, a proxy or a mirror may be changing the file (some companies' proxies do); do not use it. Download it from the address above in a browser and put it in $toolchain/downloads/, or set MEMORIES_SDK_TOOLCHAIN to a toolchain of your own."
    fi
    echo "build-mod: SHA-256 matches; unpacking"
    part="$toolchain/.unpack-llvm-mingw"
    rm -rf "$part"
    mkdir -p "$part"
    tar -xJf "$archive" -C "$part" || { rm -rf "$part"; fail "could not unpack $archive (tar needs xz: sudo apt install xz-utils)"; }
    top=""
    count=0
    for entry in "$part"/*; do
        top=$entry
        count=$((count + 1))
    done
    if [ "$count" -ne 1 ] || [ ! -x "$top/bin/clang" ]; then
        rm -rf "$part"
        fail "$archive has no single folder with bin/clang in it"
    fi
    # A folder of that name without clang in it (an earlier run cut short) would make mv put the
    # new one inside it.
    rm -rf "$folder"
    mv "$top" "$folder"
    rm -rf "$part"
    rm -f "$archive"
}

# The toolchain's clang has to start here: the download is the ubuntu-22.04 build.
check_clang() {
    if "$1/clang" --version >/dev/null 2>&1; then
        return 0
    fi
    if [ "$2" = downloaded ]; then
        fail "the downloaded clang ($1/clang) does not start on this system.
It is llvm-mingw's ubuntu-22.04 build: it needs glibc 2.34 and the libstdc++ of GCC 11 or newer
(Ubuntu 22.04, Debian 12, Fedora 35 or newer; its version strings were read, not run). On an
older system, point MEMORIES_SDK_TOOLCHAIN at a clang 23 with lld of your own: the Android object
needs the same LLVM major as the SDK's guest pass (23). A distribution's older clang still builds
the i386 and x86_64-windows objects."
    fi
    fail "the clang in MEMORIES_SDK_TOOLCHAIN ($1/clang) does not start"
}

case "${1:-}" in
    -h|--help) usage; exit 0 ;;
esac
setup_only=0
if [ "${1:-}" = "--setup" ]; then
    setup_only=1
    shift
fi

python=${MEMORIES_SDK_PYTHON:-python3}
command -v "$python" >/dev/null 2>&1 ||
    fail "python3 was not found. Install it (sudo apt install python3; version $PYTHON_MINIMUM_MAJOR.$PYTHON_MINIMUM_MINOR or newer) or set MEMORIES_SDK_PYTHON"
"$python" -c "import sys; sys.exit(0 if sys.version_info >= ($PYTHON_MINIMUM_MAJOR, $PYTHON_MINIMUM_MINOR) else 1)" ||
    fail "$python is older than python $PYTHON_MINIMUM_MAJOR.$PYTHON_MINIMUM_MINOR, which tools/build_mod.py needs"

if [ -n "${MEMORIES_SDK_TOOLCHAIN:-}" ]; then
    if [ -d "$MEMORIES_SDK_TOOLCHAIN/bin" ]; then
        bin="$MEMORIES_SDK_TOOLCHAIN/bin"
    else
        bin="$MEMORIES_SDK_TOOLCHAIN"
    fi
    [ -x "$bin/clang" ] || fail "MEMORIES_SDK_TOOLCHAIN is $MEMORIES_SDK_TOOLCHAIN, which has no clang"
    check_clang "$bin" named
else
    fetch_llvm
    bin="$toolchain/llvm-mingw-$LLVM_RELEASE/bin"
    check_clang "$bin" downloaded
fi
PATH="$bin:$PATH"
export PATH

if [ "$setup_only" -eq 1 ]; then
    echo "build-mod: the toolchain is ready in $bin"
    exit 0
fi

if [ "$#" -eq 0 ]; then
    if [ -f ./mod.json ]; then
        set -- .
    else
        usage >&2
        fail "no mod folder given, and the current folder has no mod.json"
    fi
fi
case "$1" in
    -*) ;;
    *) [ -d "$1" ] || fail "$1 is not a folder" ;;
esac

status=0
# -X utf8: its messages hold folder names, and a Python in a "C" locale writing to a pipe would
# stop at the first letter it lacks.
"$python" -X utf8 "$here/tools/build_mod.py" "$@" || status=$?
if [ "$status" -ne 0 ]; then
    echo "build-mod: tools/build_mod.py stopped (exit $status); its message is above" >&2
    exit "$status"
fi
echo "build-mod: build_mod.py finished. Its last line above says which objects were built and which"
echo "build-mod: were not, and why; they are next to the mod's files (or in --out)"
