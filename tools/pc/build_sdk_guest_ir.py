#!/usr/bin/env python3
"""Build the guest pass (memories-guest-ir) an SDK ships, against the LLVM of
the SDK's own clang (release workflow: tools/pc/package.py --guest-ir).

The pass reads the IR a mod's clang writes and the clang compiles the IR the
pass writes back (tools/pc/build_mod.py, aarch64-translated), and LLVM reads
the IR of its own version or an older one, never a newer: only a clang of the
pass's own LLVM is certain to do both. The SDK's toolchain
is llvm-mingw (clang 23), so its pass is built against LLVM 23, where the game
itself is translated with LLVM 21 (tools/pc/llvm_guest.py, untouched: the
two meet through the game's tables, mod_translation.json, not through a
compiler version). tools/pc/sdk_llvm.json pins the LLVM, per host:

  linux    "release": the official release archive (LLVM-<v>-Linux-X64.tar.xz),
           of which only the headers, llvm-config and the static libraries are
           unpacked; built with the system's c++, linked statically
  windows  "source": the official llvm-project source release, whose three
           libraries the pass needs are built with the SDK's own llvm-mingw
           (clang++, ninja, cmake as fetch_tools.py fetches them), linked
           statically, so the program is self-contained. The official
           Windows archive is built for the MSVC C++ ABI, which llvm-mingw
           cannot link.

Every download is checked against the SHA-256 in sdk_llvm.json, which
`--pin` fills in from the release's published digests (one GitHub API call,
no download). An archive without the headers or the libraries stops the
build with what it does hold. The result is checked by running it
(--llvm-major, and the three operations on a small unit) and by its imports
(only the system's libraries on Windows, no NEEDED entry on Linux). It ships
as memories-pc.exe does (notes/pc-release.md, "VirusTotal"): without
symbols, and on Windows with version information.

  python tools/pc/build_sdk_guest_ir.py [--out DIR]       build for this host
  python tools/pc/build_sdk_guest_ir.py --llvm-only       just the LLVM libraries
        (the release workflow caches them before the pass is linked)
  python tools/pc/build_sdk_guest_ir.py --pin             fill in sha256
  python tools/pc/build_sdk_guest_ir.py --llvm DIR        use an LLVM build or
        install folder (bin/llvm-config) instead of the pinned one, for a
        local check; --source-dir DIR the same with llvm-project's sources;
        --expect-major N then names the version it holds
"""
import argparse, hashlib, json, os, re, shlex, shutil, subprocess, sys, tarfile, tempfile, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
CONFIG = os.path.join(HERE, "sdk_llvm.json")
SOURCE = os.path.join(HERE, "llvm", "guest_ir.cpp")
WORK = os.path.join(ROOT, "tmp", "pc", "sdk-llvm")
OUT = os.path.join(ROOT, "tmp", "pc", "sdk-guest-ir")
EXE = ".exe" if sys.platform == "win32" else ""
# The libraries the pass links (llvm-config components): the IR, its reader
# and the support library they stand on.
COMPONENTS = ("core", "irreader", "support")
# What the Linux release archive is unpacked to: llvm-config, the headers and
# the static libraries, about 440 MB of its 2 GB.
RELEASE_PARTS = ("bin/llvm-config", "include/llvm/", "include/llvm-c/", "lib/libLLVM")
# What of the source release the build needs: not the tests, documentation
# or examples (nine tenths of it, and the deepest paths, which a Windows
# extraction may not manage), none of which the build includes. LLVM 23's
# configure stops without libc/ (cmake/Modules/FindLibcCommonUtils.cmake),
# and APFloat.cpp (LLVMSupport) includes libc's shared/math.h, whose headers
# are all under these four of libc's folders (none of its tests or tools).
SOURCE_PARTS = ("llvm/", "cmake/", "third-party/", "libc/shared/", "libc/src/", "libc/include/", "libc/hdr/")
SOURCE_SKIP = ("llvm/test/", "llvm/unittests/", "llvm/docs/", "llvm/benchmarks/", "llvm/examples/")
# The targets of the source build (llvm-config tells the flags of these).
BUILD_TARGETS = ("LLVMCore", "LLVMIRReader", "LLVMSupport", "llvm-config")
# A unit exercising each operation: an address-space-271 (guest) load, which
# the pass lowers through its resolver.
FIXTURE = """target triple = "aarch64-unknown-linux-android24"
define i32 @fixture(ptr addrspace(271) %p) {
  %v = load i32, ptr addrspace(271) %p
  ret i32 %v
}
"""


def host():
    if sys.platform == "win32":
        return "windows"
    if sys.platform.startswith("linux"):
        return "linux"
    sys.exit(f"build_sdk_guest_ir: no SDK is shipped for {sys.platform} (the macOS app builds its own, "
             "tools/pc/llvm_guest.py)")


def load():
    with open(CONFIG, encoding="utf-8") as handle:
        return json.load(handle)


def pinned(entry, name):
    if not entry.get("sha256"):
        sys.exit(f"build_sdk_guest_ir: {name} has no sha256 in {os.path.relpath(CONFIG, ROOT)}; "
                 "run `python tools/pc/build_sdk_guest_ir.py --pin` (it reads the release's published digests, "
                 "downloads nothing) and commit the result")
    return entry["sha256"]


def pin():
    """Fill in sha256 of every download in sdk_llvm.json from the digests GitHub
    publishes for the release's assets."""
    config = load()
    request = urllib.request.Request(
        f"https://api.github.com/repos/llvm/llvm-project/releases/tags/llvmorg-{config['version']}",
        headers={"Accept": "application/vnd.github+json", "User-Agent": "build_sdk_guest_ir"})
    if os.environ.get("GITHUB_TOKEN"):
        request.add_header("Authorization", "Bearer " + os.environ["GITHUB_TOKEN"])
    with urllib.request.urlopen(request, timeout=60) as response:
        assets = {asset["name"]: asset.get("digest") or "" for asset in json.load(response)["assets"]}
    for name, entry in config["platforms"].items():
        asset = entry["url"].rsplit("/", 1)[1]
        digest = assets.get(asset, "")
        if not digest.startswith("sha256:"):
            sys.exit(f"build_sdk_guest_ir: the release llvmorg-{config['version']} has no published digest "
                     f"for {asset} (it has: {', '.join(sorted(assets)) or 'no assets'})")
        entry["sha256"] = digest[len("sha256:"):]
        print(f"{name}: {asset} {entry['sha256']}")
    with open(CONFIG, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(config, handle, indent=2)
        handle.write("\n")


def download(entry, name):
    import fetch_tools
    return fetch_tools.download(entry["url"], pinned(entry, name))


def stream_parts(archive, parts, destination, skip=()):
    """Unpack the members of the tar.xz `archive` (one pass through it) whose
    path, below its top folder, starts with one of `parts` and none of `skip`,
    into `destination`.
    Returns the top folder's name and every directory name the archive has
    one level below it, for what an archive that lacks them can say."""
    seen = set()
    with tarfile.open(archive, "r|xz") as bundle:
        top = None
        for member in bundle:
            head, _, relative = member.name.partition("/")
            top = top or head
            if head != top or not relative:
                continue
            seen.add(relative.split("/")[0])
            if not relative.startswith(parts) or relative.startswith(skip):
                continue
            if not (member.isfile() or member.issym() or member.isdir()):
                continue
            if member.issym() and sys.platform == "win32":
                continue   # nothing here needs a link, and Windows may not make one
            member.name = relative
            if hasattr(tarfile, "data_filter"):
                bundle.extract(member, destination, filter="data")
            else:
                bundle.extract(member, destination)
    return top, seen


def version_of(folder):
    """(major, "x.y.z") from the llvm-config.h of an LLVM tree."""
    path = os.path.join(folder, "include", "llvm", "Config", "llvm-config.h")
    with open(path, encoding="utf-8") as handle:
        text = handle.read()
    values = {}
    for line in text.splitlines():
        words = line.split()
        if len(words) >= 3 and words[0] == "#define" and words[1] in ("LLVM_VERSION_MAJOR", "LLVM_VERSION_STRING"):
            values[words[1]] = words[2].strip('"')
    if "LLVM_VERSION_MAJOR" not in values:
        sys.exit(f"build_sdk_guest_ir: {path} does not say which LLVM it is")
    return int(values["LLVM_VERSION_MAJOR"]), values.get("LLVM_VERSION_STRING", "")


def check_release(folder, version, seen=()):
    """Stop, saying what is missing, unless `folder` holds what the pass is
    built from: the headers (with their llvm-config.h), llvm-config and the
    static libraries of `version`."""
    missing = []
    if not os.path.isfile(os.path.join(folder, "include", "llvm", "Config", "llvm-config.h")):
        missing.append("include/llvm/Config/llvm-config.h (the headers)")
    libraries = [name for name in os.listdir(os.path.join(folder, "lib"))
                 if name.startswith(("libLLVM", "LLVM")) and name.endswith((".a", ".lib"))] \
        if os.path.isdir(os.path.join(folder, "lib")) else []
    if "libLLVMCore.a" not in libraries and "LLVMCore.lib" not in libraries:
        missing.append("lib/libLLVMCore.a and the other static libraries (the archive holds "
                       f"{len(libraries)} of them)")
    if not any(os.path.isfile(os.path.join(folder, "bin", "llvm-config" + suffix)) for suffix in ("", ".exe")):
        missing.append("bin/llvm-config")
    if missing:
        sys.exit("build_sdk_guest_ir: the LLVM release archive lacks what the pass is built from: "
                 + "; ".join(missing) + (f". It holds: {', '.join(sorted(seen))}" if seen else "")
                 + ". Its entry in tools/pc/sdk_llvm.json can name \"method\": \"source\" instead (the libraries "
                 "are built from the source release, as for Windows).")
    found, string = version_of(folder)
    if string != version:
        sys.exit(f"build_sdk_guest_ir: {folder} is LLVM {string}, sdk_llvm.json pins {version}")


def release_llvm(entry, config, work):
    """The unpacked parts of the release archive (Linux), downloaded once."""
    top = entry["url"].rsplit("/", 1)[1][: -len(".tar.xz")]
    folder = os.path.join(work, top)
    if not os.path.isfile(os.path.join(folder, "bin", "llvm-config")):
        archive = download(entry, "the Linux release archive")
        staging = folder + ".part"
        shutil.rmtree(staging, ignore_errors=True)
        _, seen = stream_parts(archive, RELEASE_PARTS, staging)
        try:
            check_release(staging, config["version"], seen)
        except SystemExit:
            shutil.rmtree(staging, ignore_errors=True)
            raise
        shutil.rmtree(folder, ignore_errors=True)
        os.replace(staging, folder)
        os.remove(archive)   # 2 GB, and the unpacked parts are all that is used
    else:
        check_release(folder, config["version"])
    return folder


def say(text):
    """Print `text`, with what the console's encoding cannot spell escaped (a
    Windows console in cp1252 and a folder named in another script), never
    an error."""
    encoding = getattr(sys.stdout, "encoding", None) or "utf-8"
    print(text.encode(encoding, "backslashreplace").decode(encoding), flush=True)


def run(command, **options):
    say("+ " + " ".join(shlex.quote(str(word)) for word in command))
    subprocess.run(command, check=True, **options)


def source_llvm(entry, config, work, source=None, jobs=None):
    """The libraries of the source release, built with the compiler on PATH
    (clang++, the SDK's llvm-mingw): returns the build folder, which has
    bin/llvm-config and works as an LLVM install for llvm-config."""
    external = source is not None   # the caller's tree: never pruned
    top = os.path.basename(source.rstrip("/\\")) if source else f"llvm-project-{config['version']}.src"
    build = os.path.join(work, top + "-build")
    if os.path.isfile(os.path.join(build, "bin", "llvm-config" + EXE)) and os.path.isfile(
            os.path.join(build, "lib", "libLLVMCore.a")):
        return build
    if source is None:
        source = os.path.join(work, top)
        if not os.path.isfile(os.path.join(source, "llvm", "CMakeLists.txt")):
            archive = download(entry, "the LLVM source release")
            staging = source + ".part"
            shutil.rmtree(staging, ignore_errors=True)
            stream_parts(archive, SOURCE_PARTS, staging, SOURCE_SKIP)
            if not os.path.isfile(os.path.join(staging, "llvm", "CMakeLists.txt")):
                sys.exit(f"build_sdk_guest_ir: {entry['url']} has no llvm/CMakeLists.txt")
            shutil.rmtree(source, ignore_errors=True)
            os.replace(staging, source)
            os.remove(archive)
    for tool in ("cmake", "ninja"):   # the runner's own, else the pinned release
        if not shutil.which(tool):
            import fetch_tools
            fetch_tools.ensure(tool)
    cxx = shutil.which("clang++")
    if not cxx:
        sys.exit("build_sdk_guest_ir: clang++ is not on PATH (llvm-mingw)")
    shutil.rmtree(build, ignore_errors=True)
    run(["cmake", "-G", "Ninja", "-S", os.path.join(source, "llvm"), "-B", build, "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_C_COMPILER=" + shutil.which("clang"), "-DCMAKE_CXX_COMPILER=" + cxx,
         "-DLLVM_TARGETS_TO_BUILD=X86", "-DLLVM_ENABLE_PROJECTS=", "-DLLVM_ENABLE_ASSERTIONS=OFF",
         "-DLLVM_ENABLE_ZLIB=OFF", "-DLLVM_ENABLE_ZSTD=OFF", "-DLLVM_ENABLE_LIBXML2=OFF",
         "-DLLVM_ENABLE_TERMINFO=OFF", "-DLLVM_INCLUDE_TESTS=OFF", "-DLLVM_INCLUDE_EXAMPLES=OFF",
         "-DLLVM_INCLUDE_BENCHMARKS=OFF", "-DLLVM_INCLUDE_DOCS=OFF"])
    run(["cmake", "--build", build, "--target", *BUILD_TARGETS] + (["-j", str(jobs)] if jobs else []))
    # What is kept (the workflow caches it): llvm-config and the libraries
    # with the generated headers, and the source's headers they point to.
    for name in os.listdir(build):
        if name not in ("bin", "lib", "include"):
            path = os.path.join(build, name)
            shutil.rmtree(path, ignore_errors=True) if os.path.isdir(path) else os.remove(path)
    if not external:
        for name in os.listdir(source):
            if name != "llvm":
                shutil.rmtree(os.path.join(source, name), ignore_errors=True)
        for name in os.listdir(os.path.join(source, "llvm")):
            if name != "include":
                path = os.path.join(source, "llvm", name)
                shutil.rmtree(path, ignore_errors=True) if os.path.isdir(path) else os.remove(path)
    return build


def cleaned(flags):
    """llvm-config's flags with the absolute paths it gives to the system's
    libraries (the machine that built the archive: /usr/lib/.../libz.so,
    libzstd.a, ...) spelled -l<name>, for this machine's linker to find the
    static library (missing_static says when there is none)."""
    result = []
    for flag in flags:
        # A shared library may be named by its versioned file (libxml2.so.2,
        # libzstd.1.dylib), an import library by libz.dll.a.
        # Read as written, whatever the host's own idea of an absolute path.
        library = re.fullmatch(r"(?:[A-Za-z]:)?/(?:.*/)?lib([^/]+?)\.(?:so(?:\.\d+)*|dll\.a|a|(?:\d+\.)*dylib)", flag)
        if library and not library.group(1).startswith("LLVM"):
            result.append("-l" + library.group(1))
        else:
            result.append(flag)
    return result


def missing_static(cxx, flags):
    """The -l<name> of `flags` that have no static library (lib<name>.a) for
    `cxx` to link: a -static link cannot use the shared one."""
    missing = []
    for flag in flags:
        if flag.startswith("-l") and not flag.startswith("-lLLVM"):
            name = f"lib{flag[2:]}.a"
            found = subprocess.run([cxx, f"-print-file-name={name}"], capture_output=True, text=True).stdout.strip()
            if found in ("", name):   # the driver gives the bare name back when it finds nothing
                missing.append(flag)
    return missing


def version_resource(folder):
    """The Windows program's version information, as memories-pc.exe's
    (build_game32.py exe_resources): virus scanners hold a program that says
    nothing about itself against it. Returns the resource object."""
    windres = next((found for found in map(shutil.which, ("x86_64-w64-mingw32-windres", "llvm-windres", "windres"))
                    if found), None)
    if not windres:
        sys.exit("build_sdk_guest_ir: no windres (llvm-mingw's) to give memories-guest-ir.exe version information")
    label = os.environ.get("MEMORIES_VERSION", "")
    match = re.match(r"v(\d+)\.(\d+)\.(\d+)", label)
    number = ",".join([*match.groups(), "0"] if match else ["0", "0", "0", "0"])
    text = label or "development build"
    strings = {"CompanyName": "Yu-Gi-Oh! Forbidden Memories Recompiled",
               "FileDescription": "Yu-Gi-Oh! Forbidden Memories Recompiled mod build tool (guest pass)",
               "FileVersion": text, "InternalName": "memories-guest-ir", "OriginalFilename": "memories-guest-ir.exe",
               "ProductName": "Yu-Gi-Oh! Forbidden Memories Recompiled", "ProductVersion": text,
               "Comments": "https://github.com/Unchiga/Yu-Gi-Oh-Forbidden-Memories-Recompiled"}
    rc = ["1 VERSIONINFO", f"FILEVERSION {number}", f"PRODUCTVERSION {number}", "FILEOS 0x40004", "FILETYPE 0x1",
          "BEGIN", '  BLOCK "StringFileInfo"', "  BEGIN", '    BLOCK "040904B0"', "    BEGIN",
          *[f'      VALUE "{key}", "{value}"' for key, value in strings.items()],
          "    END", "  END", '  BLOCK "VarFileInfo"', "  BEGIN", '    VALUE "Translation", 0x409, 1200', "  END",
          "END"]
    source = os.path.join(folder, "guest-ir-resources.rc")
    with open(source, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(rc) + "\n")
    output = os.path.join(folder, "guest-ir-resources.o")
    run([windres, source, "-O", "coff", "-o", output])
    return output


def compile_pass(llvm, cxx, output, resources=()):
    """memories-guest-ir from guest_ir.cpp, linked statically (and without
    symbols) against the LLVM in `llvm` (a folder with bin/llvm-config)."""
    config_tool = os.path.join(llvm, "bin", "llvm-config" + EXE)
    flags = subprocess.check_output([config_tool, "--cxxflags", "--ldflags", "--libs", *COMPONENTS,
                                     "--system-libs"], text=True)
    flags = cleaned(shlex.split(flags.replace("\\", "/")))
    if sys.platform.startswith("linux"):
        missing = missing_static(cxx, flags)
        if missing:
            sys.exit(f"build_sdk_guest_ir: llvm-config asks the link for {' '.join(missing)}, and {cxx} finds no "
                     "static library (lib<name>.a) for it, which the static pass needs: install the package that "
                     "has it (Debian: zlib1g-dev, libzstd-dev, libncurses-dev for -ltinfo), or take it out of "
                     "the link if the pass does not use it")
    command = [cxx, SOURCE, *flags, *resources, "-O2", "-static", "-s", "-o", output]
    os.makedirs(os.path.dirname(output), exist_ok=True)
    run(command)


# What a program built with llvm-mingw may import and still run on a clean
# Windows: the system's libraries (the C runtime comes as api-ms-win-crt-*).
WINDOWS_DLLS = {"kernel32.dll", "ntdll.dll", "advapi32.dll", "shell32.dll", "ole32.dll", "oleaut32.dll", "ws2_32.dll",
                "psapi.dll", "user32.dll", "bcrypt.dll", "version.dll", "shlwapi.dll", "ucrtbase.dll", "msvcrt.dll"}


def foreign_imports(names):
    """The DLLs among `names` a clean Windows does not have."""
    return sorted({name for name in names
                   if name.lower() not in WINDOWS_DLLS and not name.lower().startswith("api-ms-win-")})


def check_static(program):
    """Stop unless `program` stands alone: on Windows imports only the system's
    libraries (not libc++.dll, libunwind.dll ...), on Linux has no NEEDED
    entry (a libstdc++ or glibc of the build machine's)."""
    if sys.platform == "win32":
        tool = shutil.which("llvm-objdump")
        if not tool:
            sys.exit("build_sdk_guest_ir: llvm-objdump (llvm-mingw's) is not on PATH to read the imports of " + program)
        text = subprocess.run([tool, "-p", program], capture_output=True, text=True).stdout
        found = foreign_imports(re.findall(r"DLL Name: (\S+)", text))
        if found:
            sys.exit(f"build_sdk_guest_ir: {program} imports {', '.join(found)}, which a clean Windows has not: "
                     "it was not linked statically")
    else:
        tool = shutil.which("readelf") or shutil.which("llvm-readelf")
        if not tool:
            sys.exit("build_sdk_guest_ir: readelf is not on PATH to read the dynamic section of " + program)
        text = subprocess.run([tool, "-d", program], capture_output=True, text=True).stdout
        found = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", text)
        if found:
            sys.exit(f"build_sdk_guest_ir: {program} needs {', '.join(found)} at run time: it was not linked statically")


def smoke(program, major):
    """Run what was built: it must start (no missing library), say the LLVM
    it was built against, and do its three operations on a unit."""
    said = subprocess.run([program, "--llvm-major"], capture_output=True, text=True)
    if said.returncode or said.stdout.strip() != str(major):
        sys.exit(f"build_sdk_guest_ir: {program} --llvm-major gave {said.stdout.strip()!r} "
                 f"(exit {said.returncode}), expected {major}: {said.stderr.strip()}")
    with tempfile.TemporaryDirectory() as folder:
        source = os.path.join(folder, "unit.ll")
        options = os.path.join(folder, "options.json")
        with open(source, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(FIXTURE)
        with open(options, "w", encoding="utf-8") as handle:
            handle.write("{}")
        for operation in ("inspect", "normalize", "translate"):
            output = os.path.join(folder, operation + ".out")
            result = subprocess.run([program, operation, source, output, options], capture_output=True, text=True)
            if result.returncode:
                sys.exit(f"build_sdk_guest_ir: {program} {operation} failed on the check unit: {result.stderr.strip()}")
            with open(output, encoding="utf-8") as handle:
                text = handle.read()
            if operation == "inspect" and '"fixture"' not in text:
                sys.exit(f"build_sdk_guest_ir: {program} inspect does not list the check unit's function")
            if operation == "translate" and "Memories_ResolveDataFast" not in text:
                sys.exit(f"build_sdk_guest_ir: {program} translate did not route the guest load through the resolver")
    check_static(program)
    say(f"{program}: LLVM {major}, inspect/normalize/translate ran, stands alone")


def clang_major(path):
    import re
    text = subprocess.run([path, "--version"], capture_output=True, text=True).stdout
    match = re.search(r"clang version (\d+)", text)
    return int(match.group(1)) if match else None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=OUT, help=f"where memories-guest-ir{EXE} goes (default: tmp/pc/sdk-guest-ir)")
    parser.add_argument("--work", default=WORK, help="where the LLVM is unpacked or built (default: tmp/pc/sdk-llvm)")
    parser.add_argument("--pin", action="store_true", help="fill in sdk_llvm.json's sha256 from the release's digests")
    parser.add_argument("--llvm", help="an LLVM build or install folder (bin/llvm-config) to use instead of the pinned one")
    parser.add_argument("--source-dir", help="an unpacked llvm-project to build the libraries from, instead of the pinned one")
    parser.add_argument("--expect-major", type=int, help="the LLVM major --llvm or --source-dir hold (default: the pinned)")
    parser.add_argument("--cxx", help="the C++ compiler (default: MEMORIES_GUEST_CXX; Windows: llvm-mingw's clang++; else c++)")
    parser.add_argument("--jobs", type=int, help="build jobs for the source build")
    parser.add_argument("--llvm-only", action="store_true",
                        help="only get the LLVM libraries (unpack or build them), for the workflow to cache them")
    options = parser.parse_args()
    if options.pin:
        pin()
        return
    config = load()
    system = host()
    entry = config["platforms"][system]
    major = options.expect_major or config["llvm_major"]
    if system == "windows" and not options.cxx and not os.environ.get("MEMORIES_GUEST_CXX"):
        import fetch_tools
        fetch_tools.ensure("llvm-mingw")
    cxx = options.cxx or os.environ.get("MEMORIES_GUEST_CXX") or ("clang++" if system == "windows" else "c++")
    cxx = shutil.which(cxx) or cxx
    if system == "windows":
        mingw = clang_major(cxx)
        # The SDK's modders compile with this clang: the pass is built for its LLVM.
        if not options.expect_major and mingw != config["llvm_major"]:
            sys.exit(f"build_sdk_guest_ir: {cxx} is clang {mingw}, and sdk_llvm.json pins LLVM "
                     f"{config['llvm_major']} for the SDK's toolchain (llvm-mingw); change the pin or the toolchain")
    os.makedirs(options.work, exist_ok=True)
    if options.llvm:
        llvm = options.llvm
    elif options.source_dir:
        llvm = source_llvm(entry, config, options.work, options.source_dir, options.jobs)
    elif entry["method"] == "release":
        llvm = release_llvm(entry, config, options.work)
    else:
        llvm = source_llvm(entry, config, options.work, jobs=options.jobs)
    found, _ = version_of(llvm) if os.path.isfile(os.path.join(llvm, "include", "llvm", "Config", "llvm-config.h")) \
        else (None, None)
    if found is not None and found != major:
        sys.exit(f"build_sdk_guest_ir: {llvm} is LLVM {found}, expected {major}")
    if options.llvm_only:
        say(llvm)
        return
    output = os.path.join(options.out, "memories-guest-ir" + EXE)
    os.makedirs(options.out, exist_ok=True)
    with tempfile.TemporaryDirectory() as resources:
        compile_pass(llvm, cxx, output, [version_resource(resources)] if system == "windows" else [])
    smoke(output, major)
    say(output)


if __name__ == "__main__":
    main()
