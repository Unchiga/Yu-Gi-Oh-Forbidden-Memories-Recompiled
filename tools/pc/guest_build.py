"""Shared guest address tables and host compiler flags for games and mods."""

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


def darwin_flags(sdk):
    return ["-isysroot", str(sdk), f"-mmacosx-version-min={MACOS_MINIMUM}"]


def guest_frontend_flags(sdk, host=None):
    """Preserve the PS1 layout until structured guest-memory lowering.

    host replaces the Darwin flags for another translated target (the
    Switch: switch_deps.compile_flags())."""
    return [
        *(darwin_flags(sdk) if host is None else host),
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
        "-D_DARWIN_C_SOURCE" if host is None else "-D_DEFAULT_SOURCE",
        "-I" + str(ROOT / "src"),
    ]
