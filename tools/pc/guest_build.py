"""Shared guest address tables and Darwin compiler flags for games and mods."""

from build_config import MACOS_MINIMUM, ROOT

HOST_LIBC = {
    "printf",
    "sprintf",
    "strcmp",
    "strcpy",
    "bzero",
    "qsort",
    "memcpy",
    "memset",
    "memmove",
    "strlen",
    "strcat",
    "strncmp",
    "strncpy",
    "memcmp",
    "snprintf",
    "vsnprintf",
    "strchr",
    "strrchr",
    "strnlen",
    "strcspn",
    "strspn",
    "strstr",
    "memchr",
    "strtol",
    "fgets",
    "fwrite",
    "fseek",
    "fclose",
    "isalnum",
    "isdigit",
    "isspace",
    "tolower",
}
CHECKED_LIBC = {
    "__memcpy_chk",
    "__memmove_chk",
    "__memset_chk",
    "__strcpy_chk",
    "__strncpy_chk",
    "__strcat_chk",
    "__strncat_chk",
    "__sprintf_chk",
    "__snprintf_chk",
    "__vsprintf_chk",
    "__vsnprintf_chk",
    "__strlcpy_chk",
    "__strlcat_chk",
}


def maps():
    result, active = {}, None
    for line in (ROOT / "config/pc/guest_addresses.txt").read_text().splitlines():
        if line.startswith("["):
            active = line[1:-1]
            result[active] = {}
        elif active and len(line.split()) == 2:
            name, address = line.split()
            result[active][name] = int(address, 16)
    return result


ANDROID_API = 24


def darwin_flags(sdk):
    """Target flags: Darwin with an SDK path, Android arm64 when sdk is None
    (build_arm64.py --target android-arm64-translated). -fsigned-char: MIPS and Darwin's char is
    signed, Linux AArch64's is not."""
    if sdk is None:
        # Code mods are ELF objects the game's own loader reads (an app
        # cannot load a library from its data folder, targetSdk 29+):
        # build_mod.py --target aarch64-translated.
        return [f"--target=aarch64-linux-android{ANDROID_API}", "-fPIC", "-fsigned-char"]
    return ["-isysroot", str(sdk), f"-mmacosx-version-min={MACOS_MINIMUM}"]


def guest_frontend_flags(sdk):
    """Preserve the PS1 layout until structured guest-memory lowering."""
    return [
        *darwin_flags(sdk),
        "-std=gnu11",
        "-fms-extensions",
        "-O0",
        "-fno-strict-aliasing",
        "-ffp-contract=off",
        "-fwrapv",
        "-fno-stack-protector",
        "-DMEMORIES_PC",
        "-DMEMORIES_TRANSLATED",
        "-D_LANGUAGE_C",
        "-DLANGUAGE_C",
        *(["-D_DARWIN_C_SOURCE"] if sdk is not None else [
            "-I" + str(ROOT / "src/pc/compat/android"),
            "-include", str(ROOT / "src/pc/compat/android/android_compat.h"),
        ]),
        "-I" + str(ROOT / "src"),
    ]
