#!/usr/bin/env python3
"""Test tools/pc/build_sdk_guest_ir.py without LLVM: the pin, the check of an
LLVM release archive (what it holds, what it lacks), the partial unpacking,
and the flags cleaned of the machine that built the archive. Nothing is
downloaded and no compiler starts."""
import io, json, os, shutil, subprocess, sys, tarfile, tempfile, unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_sdk_guest_ir as sdk
import package

CONFIG_H = '#define LLVM_VERSION_MAJOR 23\n#define LLVM_VERSION_STRING "23.1.2"\n'


class Archive(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.mkdtemp(prefix="sdk-guest-ir-")
        self.addCleanup(shutil.rmtree, self.folder, ignore_errors=True)

    def make(self, files, top="LLVM-23.1.2-Linux-X64"):
        """A tar.xz with `files` ({path below the top folder: bytes}) in it."""
        path = os.path.join(self.folder, top + ".tar.xz")
        with tarfile.open(path, "w:xz") as archive:
            folder = tarfile.TarInfo(top)
            folder.type = tarfile.DIRTYPE
            archive.addfile(folder)
            for name, data in files.items():
                info = tarfile.TarInfo(f"{top}/{name}")
                info.size = len(data)
                info.mode = 0o755 if name.startswith("bin/") else 0o644
                archive.addfile(info, io.BytesIO(data))
        return path

    GOOD = {"bin/llvm-config": b"#!/bin/sh\n", "bin/clang": b"clang", "include/llvm/Config/llvm-config.h": CONFIG_H.encode(),
            "include/llvm/IR/Module.h": b"//", "include/llvm-c/Core.h": b"//", "include/clang/x.h": b"//",
            "lib/libLLVMCore.a": b"!<arch>\n", "lib/libLLVMSupport.a": b"!<arch>\n", "lib/libclang.a": b"x",
            "share/doc/readme": b"x"}

    def test_only_the_parts_are_unpacked(self):
        target = os.path.join(self.folder, "out")
        top, seen = sdk.stream_parts(self.make(self.GOOD), sdk.RELEASE_PARTS, target)
        self.assertEqual(top, "LLVM-23.1.2-Linux-X64")
        self.assertEqual(seen, {"bin", "include", "lib", "share"})
        found = sorted(os.path.relpath(os.path.join(directory, name), target).replace(os.sep, "/")
                       for directory, _, files in os.walk(target) for name in files)
        self.assertEqual(found, ["bin/llvm-config", "include/llvm-c/Core.h", "include/llvm/Config/llvm-config.h",
                                 "include/llvm/IR/Module.h", "lib/libLLVMCore.a", "lib/libLLVMSupport.a"])
        sdk.check_release(target, "23.1.2")
        self.assertEqual(sdk.version_of(target), (23, "23.1.2"))

    def test_the_source_release_is_unpacked_without_its_tests(self):
        files = {"llvm/CMakeLists.txt": b"x", "llvm/include/llvm/IR/Module.h": b"x", "llvm/lib/IR/Core.cpp": b"x",
                 "llvm/test/CodeGen/x.ll": b"x", "llvm/unittests/y.cpp": b"x", "llvm/docs/z.rst": b"x",
                 "cmake/modules/a.cmake": b"x", "third-party/b/c.h": b"x", "clang/lib/d.cpp": b"x", "README.md": b"x",
                 # libc's headers APFloat.cpp includes (shared/math.h and what it includes), not its tests
                 "libc/shared/math.h": b"x", "libc/src/__support/FPUtil/FPBits.h": b"x", "libc/hdr/errno_macros.h": b"x",
                 "libc/include/llvm-libc-macros/math-macros.h": b"x", "libc/test/src/math/e.cpp": b"x",
                 "libc/docs/f.rst": b"x", "libc/CMakeLists.txt": b"x"}
        target = os.path.join(self.folder, "src")
        top, _ = sdk.stream_parts(self.make(files, "llvm-project-23.1.2.src"), sdk.SOURCE_PARTS, target, sdk.SOURCE_SKIP)
        found = sorted(os.path.relpath(os.path.join(directory, name), target).replace(os.sep, "/")
                       for directory, _, names in os.walk(target) for name in names)
        self.assertEqual(found, ["cmake/modules/a.cmake", "libc/hdr/errno_macros.h",
                                 "libc/include/llvm-libc-macros/math-macros.h", "libc/shared/math.h",
                                 "libc/src/__support/FPUtil/FPBits.h", "llvm/CMakeLists.txt",
                                 "llvm/include/llvm/IR/Module.h", "llvm/lib/IR/Core.cpp", "third-party/b/c.h"])

    def test_release_is_fetched_checked_and_dropped(self):
        archive = self.make(self.GOOD)
        entry = {"method": "release", "url": "https://example.invalid/LLVM-23.1.2-Linux-X64.tar.xz", "sha256": "ab" * 32}
        with mock.patch.object(sdk, "download", lambda entry, name: archive):
            folder = sdk.release_llvm(entry, {"version": "23.1.2"}, os.path.join(self.folder, "work"))
        self.assertTrue(os.path.isfile(os.path.join(folder, "lib", "libLLVMCore.a")))
        self.assertFalse(os.path.exists(archive))   # 2 GB of it: only the parts stay
        with mock.patch.object(sdk, "download", side_effect=AssertionError("fetched twice")):   # cached
            self.assertEqual(sdk.release_llvm(entry, {"version": "23.1.2"}, os.path.join(self.folder, "work")), folder)

    def lacks(self, files, version="23.1.2"):
        archive = self.make(files)
        entry = {"method": "release", "url": "https://example.invalid/LLVM-23.1.2-Linux-X64.tar.xz", "sha256": "ab" * 32}
        with mock.patch.object(sdk, "download", lambda entry, name: archive), self.assertRaises(SystemExit) as stopped:
            sdk.release_llvm(entry, {"version": version}, os.path.join(self.folder, "work"))
        return str(stopped.exception.code)

    def test_an_archive_without_the_headers_or_libraries_stops_the_build(self):
        # What a toolchain-only archive holds: programs, no development files.
        text = self.lacks({"bin/clang": b"x", "bin/llvm-config": b"x", "lib/libclang.a": b"x"})
        self.assertIn("lacks what the pass is built from", text)
        self.assertIn("llvm-config.h", text)
        self.assertIn("libLLVMCore.a", text)
        self.assertIn("It holds: bin, lib", text)
        self.assertIn('"method": "source"', text)
        without_headers = {name: data for name, data in self.GOOD.items() if not name.startswith("include/llvm/")}
        self.assertIn("llvm-config.h", self.lacks(without_headers))
        without_libraries = {name: data for name, data in self.GOOD.items() if not name.startswith("lib/libLLVM")}
        self.assertIn("libLLVMCore.a", self.lacks(without_libraries))
        without_tool = {name: data for name, data in self.GOOD.items() if name != "bin/llvm-config"}
        self.assertIn("bin/llvm-config", self.lacks(without_tool))
        # Nothing half-unpacked is left to be taken for the archive's parts.
        self.assertFalse(os.path.exists(os.path.join(self.folder, "work", "LLVM-23.1.2-Linux-X64")))

    def test_another_version_stops_the_build(self):
        self.assertIn("is LLVM 23.1.2, sdk_llvm.json pins 23.1.3", self.lacks(self.GOOD, "23.1.3"))

    def test_a_download_needs_its_pin(self):
        with self.assertRaises(SystemExit) as stopped:
            sdk.pinned({"url": "u", "sha256": None}, "the Linux release archive")
        self.assertIn("--pin", str(stopped.exception.code))
        self.assertEqual(sdk.pinned({"url": "u", "sha256": "ab" * 32}, "x"), "ab" * 32)

    def test_the_committed_pins(self):
        config = sdk.load()
        for name, entry in config["platforms"].items():
            # Until `--pin` has run, the build says so; afterwards the digest is a SHA-256.
            self.assertTrue(entry["sha256"] is None or len(entry["sha256"]) == 64, name)

    def test_pin_reads_the_published_digests(self):
        copy = os.path.join(self.folder, "sdk_llvm.json")
        shutil.copyfile(sdk.CONFIG, copy)
        config = sdk.load()
        assets = [{"name": entry["url"].rsplit("/", 1)[1], "digest": "sha256:" + format(index + 1, "x") * 64}
                  for index, entry in enumerate(config["platforms"].values())]
        response = io.BytesIO(json.dumps({"assets": assets}).encode())
        requested = []
        with mock.patch.object(sdk, "CONFIG", copy), \
                mock.patch.object(sdk.urllib.request, "urlopen", lambda request, timeout: requested.append(request.full_url) or response), \
                mock.patch("sys.stdout", io.StringIO()):
            sdk.pin()
        self.assertEqual(requested, [f"https://api.github.com/repos/llvm/llvm-project/releases/tags/llvmorg-{config['version']}"])
        with open(copy, encoding="utf-8") as handle:
            written = json.load(handle)
        self.assertEqual([entry["sha256"] for entry in written["platforms"].values()],
                         [format(index + 1, "x") * 64 for index in range(len(assets))])
        self.assertEqual({key: value for key, value in written.items() if key != "platforms"},
                         {key: value for key, value in config.items() if key != "platforms"})

    def test_pin_without_a_digest_stops(self):
        copy = os.path.join(self.folder, "sdk_llvm.json")
        shutil.copyfile(sdk.CONFIG, copy)
        response = io.BytesIO(json.dumps({"assets": [{"name": "other.tar.xz", "digest": None}]}).encode())
        with mock.patch.object(sdk, "CONFIG", copy), \
                mock.patch.object(sdk.urllib.request, "urlopen", lambda request, timeout: response), \
                self.assertRaises(SystemExit) as stopped:
            sdk.pin()
        self.assertIn("no published digest", str(stopped.exception.code))

    def test_flags_of_the_machine_that_built_the_archive(self):
        """llvm-config names the system's libraries by the path they had on the
        machine that built the archive; they are asked of this one by name."""
        flags = ["-L/opt/llvm/lib", "-lLLVMCore", "/usr/lib/x86_64-linux-gnu/libz.so", "/usr/lib/libzstd.a",
                 "/usr/lib/x86_64-linux-gnu/libxml2.so.2", "-lm"]
        # The same on every host: the flags are read as llvm-config wrote them.
        self.assertEqual(sdk.cleaned(flags), ["-L/opt/llvm/lib", "-lLLVMCore", "-lz", "-lzstd", "-lxml2", "-lm"])
        self.assertEqual(sdk.cleaned(["/opt/llvm/lib/libLLVMCore.a"]), ["/opt/llvm/lib/libLLVMCore.a"])
        self.assertEqual(sdk.cleaned(["/usr/lib/libtinfo.so.6.4", "C:/llvm/lib/libz.a", "/usr/lib/notalib.so"]),
                         ["-ltinfo", "-lz", "/usr/lib/notalib.so"])
        self.assertEqual(sdk.cleaned(["-lLLVMCore", "-lpsapi"]), ["-lLLVMCore", "-lpsapi"])
        self.assertEqual(sdk.cleaned(["C:/msys64/mingw64/lib/libz.dll.a", "/opt/homebrew/lib/libzstd.1.dylib"]),
                         ["-lz", "-lzstd"])

    def test_a_system_library_without_a_static_one_is_named(self):
        have = {"libz.a": "/usr/lib/x86_64-linux-gnu/libz.a", "libm.a": "/usr/lib/x86_64-linux-gnu/libm.a"}
        def driver(command, **options):
            name = command[1].split("=", 1)[1]
            return subprocess.CompletedProcess(command, 0, have.get(name, name) + "\n", "")
        with mock.patch.object(sdk.subprocess, "run", driver):
            self.assertEqual(sdk.missing_static("c++", ["-lLLVMCore", "-lz", "-ltinfo", "-lm", "-lxml2", "-pthread"]),
                             ["-ltinfo", "-lxml2"])
            self.assertEqual(sdk.missing_static("c++", ["-lz", "-lm"]), [])

    def test_a_path_the_console_cannot_spell_is_printed(self):
        """A Windows console in cp1252, a folder named in another script."""
        console = io.TextIOWrapper(io.BytesIO(), encoding="cp1252")
        with mock.patch("sys.stdout", console):
            sdk.say("+ windres /tmp/Jos\u00e9-e\u0301-\u042f-\U0001f600/x.rc")
        console.flush()
        self.assertEqual(console.buffer.getvalue().decode("cp1252").rstrip(),
                         "+ windres /tmp/Jos\u00e9-e\\u0301-\\u042f-\\U0001f600/x.rc")

    def test_a_static_pass_imports_only_the_systems_libraries(self):
        names = ["KERNEL32.dll", "ntdll.dll", "api-ms-win-crt-runtime-l1-1-0.dll", "ADVAPI32.dll", "SHELL32.dll",
                 "WS2_32.dll", "ole32.dll", "PSAPI.DLL"]
        self.assertEqual(sdk.foreign_imports(names), [])
        self.assertEqual(sdk.foreign_imports(names + ["libc++.dll", "libunwind.dll", "libc++.dll"]),
                         ["libc++.dll", "libunwind.dll"])

    def test_check_static_reads_the_imports(self):
        windows = "The Import Tables:\n  DLL Name: KERNEL32.dll\n  DLL Name: libc++.dll\n"
        elf = "Dynamic section:\n 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]\n"
        def tool(text):
            return lambda command, **options: subprocess.CompletedProcess(command, 0, text, "")
        with mock.patch.object(sdk.sys, "platform", "win32"), mock.patch.object(sdk.shutil, "which", lambda name: name):
            with mock.patch.object(sdk.subprocess, "run", tool(windows)), self.assertRaises(SystemExit) as stopped:
                sdk.check_static("pass.exe")
            self.assertIn("libc++.dll", str(stopped.exception.code))
            with mock.patch.object(sdk.subprocess, "run", tool("DLL Name: KERNEL32.dll\n")):
                sdk.check_static("pass.exe")
        with mock.patch.object(sdk.sys, "platform", "linux"), mock.patch.object(sdk.shutil, "which", lambda name: name):
            with mock.patch.object(sdk.subprocess, "run", tool(elf)), self.assertRaises(SystemExit) as stopped:
                sdk.check_static("pass")
            self.assertIn("libc.so.6", str(stopped.exception.code))
            with mock.patch.object(sdk.subprocess, "run", tool("There is no dynamic section in this file.\n")):
                sdk.check_static("pass")

    def test_the_windows_pass_has_version_information(self):
        """As memories-pc.exe's (build_game32.py exe_resources): virus
        scanners hold a program that says nothing about itself against it."""
        compiled = []
        def windres(command, **options):
            compiled.append(command)
            return subprocess.CompletedProcess(command, 0)
        for label, number, text in (("v1.2.3", "1,2,3,0", "v1.2.3"), ("", "0,0,0,0", "development build"),
                                    ("dev-abc123", "0,0,0,0", "dev-abc123")):
            with mock.patch.dict(os.environ, {"MEMORIES_VERSION": label}), \
                    mock.patch.object(sdk.shutil, "which", lambda name: name), \
                    mock.patch.object(sdk.subprocess, "run", windres):
                result = sdk.version_resource(self.folder)
            self.assertTrue(result.endswith(".o"))
            with open(os.path.join(self.folder, "guest-ir-resources.rc"), encoding="utf-8") as handle:
                rc = handle.read()
            self.assertIn(f"FILEVERSION {number}", rc)
            self.assertIn(f'VALUE "FileVersion", "{text}"', rc)
            self.assertIn('VALUE "InternalName", "memories-guest-ir"', rc)
            self.assertIn('VALUE "OriginalFilename", "memories-guest-ir.exe"', rc)
            self.assertIn('VALUE "ProductName", "Yu-Gi-Oh! Forbidden Memories Recompiled"', rc)
            self.assertEqual(compiled[-1][1:], [os.path.join(self.folder, "guest-ir-resources.rc"), "-O", "coff", "-o", result])
        with mock.patch.object(sdk.shutil, "which", lambda name: None), self.assertRaises(SystemExit):
            sdk.version_resource(self.folder)

class Packing(unittest.TestCase):
    """package.py puts the pass of the archive's system in its SDK."""
    def setUp(self):
        self.folder = tempfile.mkdtemp(prefix="sdk-guest-ir-pack-")
        self.addCleanup(shutil.rmtree, self.folder, ignore_errors=True)

    def program(self, name, magic):
        path = os.path.join(self.folder, name)
        with open(path, "wb") as handle:
            handle.write(magic + bytes(100))
        return path

    def test_each_system_takes_the_pass_of_its_kind(self):
        exe, elf = self.program("a.exe", b"MZ"), self.program("a", b"\x7fELF")
        for paths in ([exe, elf], [elf, exe]):
            self.assertEqual(package.guest_ir_for("windows", paths), os.path.abspath(exe))
            self.assertEqual(package.guest_ir_for("windows-x64", paths), os.path.abspath(exe))
            self.assertEqual(package.guest_ir_for("linux", paths), os.path.abspath(elf))
        self.assertIsNone(package.guest_ir_for("linux", [exe]))
        self.assertIsNone(package.guest_ir_for("windows", [os.path.join(self.folder, "missing")]))

    def test_packing_stops_without_a_pass(self):
        exe = self.program("a.exe", b"MZ")
        self.assertIsNone(package.guest_ir_unavailable(["windows"], [exe]))
        self.assertIsNone(package.guest_ir_unavailable(["android-arm64"], []))   # an APK has no SDK
        text = package.guest_ir_unavailable(["windows", "linux"], [exe])
        self.assertIn("the linux SDK needs the guest pass", text)
        self.assertIn("build_sdk_guest_ir.py", text)

    def test_the_pass_is_staged_executable_beside_build_mod(self):
        """stage() copies it to sdk/tools/ (.exe on Windows) with the mode a
        tar archive keeps, and the Linux tar marks it executable."""
        source = self.program("built", b"\x7fELF")
        build = os.path.join(self.folder, "build")
        for name in ("mods", "sdk", "languages", "symbols"):
            os.makedirs(os.path.join(build, name))
        for name in ("memories-pc", "buildid", "commit"):
            with open(os.path.join(build, name), "w") as handle:
                handle.write("0\n")
        with open(os.path.join(build, "symbols", "0.txt"), "w") as handle:
            handle.write("0\n")
        with mock.patch.dict(package.BUILDS, {"linux": (build, "memories-pc", [])}), \
                mock.patch.object(package, "DIST", os.path.join(self.folder, "dist")):
            staged = package.stage("linux", "test", {}, source)
            archive = package.pack("linux", staged)
        with tarfile.open(archive) as tar:
            member = tar.getmember("yfm-redecomp-test/sdk/tools/memories-guest-ir")
            self.assertTrue(member.mode & 0o111)
            self.assertFalse(tar.getmember("yfm-redecomp-test/buildid").mode & 0o111)


if __name__ == "__main__":
    unittest.main()
