#!/usr/bin/env python3
"""Package the Android build (libmain.so, the loader, and libgame.so, the
game) as an installable APK.

Called by build_game32.py --target android-<abi> after the link; it can also
be run on its own: package_android.py <build dir> <abi>. Uses only the JDK
(javac) and the Android SDK's own tools, no Gradle: SDL's Java shell
(org.libsdl.app, from the same SDL release as libSDL3.so; build_android_deps.py)
and the game's restart activity (src/pc/platform/android/Restart.java, in a
process of its own) are compiled against the SDK's android.jar and dexed with d8, aapt2 links the
manifest, the native libraries go in lib/<abi>/, and the APK is aligned
(zipalign) and signed (apksigner). The key is the release key named by the
environment (signing_key(): MEMORIES_ANDROID_KEYSTORE and its passwords,
which are never printed or put on a command line), else a debug key kept
under tmp/, never in the repository; either way the signer's certificate
(DN, SHA-256) is printed and checked with apksigner verify. The activity is SDL's own SDLActivity: it loads libSDL3.so
and libmain.so and calls SDL_main (src/pc/platform/android_loader.c), which
loads libgame.so at its link address. The build's id, commit and symbol
table go in assets/build/, and the shipped mods and language packs under
assets/build/files/ with their list in build/files.txt
(src/pc/platform/android.c unpacks them).

Output: <build>/memories-<abi>.apk. notes/pc-build.md, "Android"."""
import glob, os, re, shutil, subprocess, sys, zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_android_deps

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PACKAGE = "org.yfmredecomp.game"
LABEL = "YFM Re-Decomp"
KEYSTORE = os.path.join(ROOT, "tmp", "pc", "android-deps", "debug.keystore")
# The port's own Java, beside SDL's, one class per file in this folder (all
# in PACKAGE): Restart (Platform_RestartGame), HdDownload (the HD pack) and
# ReportProvider, which hands the crash report to the app the player shares
# it with (android_report.c).
JAVA = os.path.join(ROOT, "src", "pc", "platform", "android")
DEBUG_DN = "CN=Android Debug, O=Android, C=US"
# The alias of the release key when MEMORIES_ANDROID_KEY_ALIAS is unset.
RELEASE_ALIAS = "yfm"
# Where signing_key() hands apksigner the passwords (env:<name>): the
# child's environment only, never its command line nor a file.
STORE_PASS_VAR, KEY_PASS_VAR = "MEMORIES_APKSIGNER_STORE_PASS", "MEMORIES_APKSIGNER_KEY_PASS"
TARGET_SDK = 35

# The app's version (app_version): versionCode orders updates (Android installs
# an APK only over one with the same or a lower code) and versionName is what
# the player sees. A release vMAJOR.MINOR.PATCH[-LABEL.N] gets
#   MAJOR*1000000 + MINOR*10000 + PATCH*100 + STAGE
# with STAGE 99 for the release itself and PRERELEASE[LABEL] + N (N 0-19)
# before it, so codes rise in the order versions compare (notes/updates.md):
# v0.2.0-preview.1 = 20041 < v0.2.0-rc.1 = 20061 < v0.2.0 = 20099 <
# v0.2.1-preview.1 = 20141. MINOR and PATCH are 0-99 and MAJOR at most 2099:
# Google Play takes codes up to 2100000000 (MAX_CODE), below the 32-bit
# int's 2147483647. LABEL is lowercase, as the update check compares labels
# byte by byte (update.c, compare_pre: "RC" would sort before "alpha").
# A development build takes the highest code of the v* tags it descends from
# (an equal code installs over that release), at least VERSION_FLOOR, above
# the 1 every earlier test APK had.
VERSION = re.compile(r"v(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?")
PRERELEASE = {"alpha": 0, "beta": 20, "preview": 40, "rc": 60}
RELEASE_STAGE = 99
VERSION_FLOOR = 2
MAX_CODE = 2100000000
# Changes the build itself makes to the checkout, which do not make a
# development build's name "-dirty" (build_game32.py refreshes the
# addresses from the matching build's ELFs before the link).
BUILD_WRITES = ("config/pc/guest_addresses.txt",)

# Every configuration change the activity takes itself instead of being
# destroyed and created again: a destroyed activity ends the game
# (SDLActivity.onDestroy sends a quit), which, at its fixed addresses,
# cannot start over in the same process.
# Display size and font size in the system settings (density, fontScale,
# fontWeightAdjustment), a SIM swap (mcc, mnc), a wide-gamut or HDR display
# mode (colorMode), Android 14's grammatical gender, and a touchscreen coming
# or going (touchscreen) are the ones that were missing; the picture and the
# touch controls follow a density change (sdl.c, "density"). Unknown names on
# an older Android are ignored (the manifest holds a bit mask); aapt2 checks
# each against android.jar, which needs API 34 or later for grammaticalGender
# (MIN_PLATFORM, which platform_dir checks; the build's android-35 has it).
CONFIG_CHANGES = "|".join((
    "mcc", "mnc", "locale", "touchscreen", "keyboard", "keyboardHidden", "navigation", "orientation",
    "screenLayout", "uiMode", "screenSize", "smallestScreenSize", "layoutDirection", "fontScale", "colorMode",
    "density", "fontWeightAdjustment", "grammaticalGender"))
MIN_PLATFORM = 34
MANIFEST = f"""<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="{PACKAGE}" android:versionCode="@VERSION_CODE@" android:versionName="@VERSION_NAME@">
    <!-- The Mods panel's HD pack... (hd_pack.h): granted at install, no
         prompt; nothing is contacted until the player taps it. -->
    <uses-permission android:name="android.permission.INTERNET" />
    <uses-feature android:glEsVersion="0x00020000" />
    <uses-feature android:name="android.hardware.touchscreen" android:required="false" />
    <uses-feature android:name="android.hardware.gamepad" android:required="false" />
    <application android:label="{LABEL}" android:hasCode="true" android:allowBackup="false"
        android:extractNativeLibs="true" android:hardwareAccelerated="true"
        android:theme="@android:style/Theme.NoTitleBar.Fullscreen">
        <activity android:name="org.libsdl.app.SDLActivity" android:exported="true"
            android:configChanges="@CONFIG_CHANGES@"
            android:screenOrientation="sensorLandscape" android:launchMode="singleInstance"
            android:preferMinimalPostProcessing="true">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
        <activity android:name="org.yfmredecomp.game.Restart" android:exported="false"
            android:process=":restart" android:excludeFromRecents="true" android:noHistory="true"
            android:configChanges="layoutDirection|locale|orientation|uiMode|screenLayout|screenSize|smallestScreenSize|keyboard|keyboardHidden|navigation"
            android:theme="@android:style/Theme.Translucent.NoTitleBar" />
        <provider android:name="org.yfmredecomp.game.ReportProvider"
            android:authorities="{PACKAGE}.reports"
            android:exported="false" android:grantUriPermissions="true" />
    </application>
</manifest>
"""


def sdk():
    path = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
    if not path or not os.path.isdir(path):
        sys.exit("ANDROID_SDK_ROOT is not set (notes/pc-build.md, \"Android\")")
    return path


def newest(folder, prefix=""):
    """The subfolder of `folder` named prefix + a plain version (35.0.0,
    android-35, android-36.1) with the highest version, compared as numbers.
    Other names are passed over: previews and extensions (36.0.0-rc1,
    android-36-ext19, android-CANARY), whose parts are not numbers."""
    versions = []
    for path in glob.glob(os.path.join(folder, prefix + "*")):
        rest = os.path.basename(path)[len(prefix):]
        if os.path.isdir(path) and re.fullmatch(r"\d+(\.\d+)*", rest):
            versions.append((tuple(int(part) for part in rest.split(".")), path))
    if not versions:
        sys.exit(f"no {prefix}<version> folder in {folder}")
    return max(versions)[1]


def build_tools_dir():
    """The SDK's build-tools: ANDROID_BUILD_TOOLS (a version, e.g. 35.0.0;
    the release workflow pins it) when set, else the newest installed."""
    pinned = os.environ.get("ANDROID_BUILD_TOOLS")
    if pinned:
        path = os.path.join(sdk(), "build-tools", pinned)
        if not os.path.isdir(path):
            sys.exit(f"ANDROID_BUILD_TOOLS is {pinned}, and {path} is not there")
        return path
    return newest(os.path.join(sdk(), "build-tools"))


def platform_dir():
    """The SDK platform compiled against: android-TARGET_SDK when installed,
    else the newest installed; android-MIN_PLATFORM at the least."""
    path = os.path.join(sdk(), "platforms", f"android-{TARGET_SDK}")
    if not os.path.isdir(path):
        path = newest(os.path.join(sdk(), "platforms"), "android-")
    if int(os.path.basename(path)[len("android-"):].split(".")[0]) < MIN_PLATFORM:
        sys.exit(f"{path}: android-{MIN_PLATFORM} or later is needed (the manifest's configChanges names "
                 f"grammaticalGender); install it: sdkmanager \"platforms;android-{TARGET_SDK}\"")
    return path


def tool(build_tools, name):
    """A build-tools program: name.exe or name.bat on Windows, name elsewhere."""
    for suffix in ((".exe", ".bat") if sys.platform == "win32" else ("",)):
        if os.path.exists(os.path.join(build_tools, name + suffix)):
            return os.path.join(build_tools, name + suffix)
    sys.exit(f"{name} is not in {build_tools}")


def run(command, env=None):
    result = subprocess.run(command, capture_output=True, text=True, env=env)
    if result.returncode:
        sys.exit(f"{' '.join(command[:4])} ...\n{result.stdout}\n{result.stderr}")
    return result.stdout


def secret(name):
    """The value of $name, else the first line of the file $name_FILE (its
    line ending dropped), else None. The value is never printed."""
    if os.environ.get(name):
        return os.environ[name]
    path = os.environ.get(name + "_FILE")
    if not path:
        return None
    try:
        with open(path, encoding="utf-8") as handle:
            value = handle.readline().rstrip("\r\n")
    except OSError as error:
        sys.exit(f"{name}_FILE: cannot read {path}: {error.strerror}")
    if not value:
        sys.exit(f"{name}_FILE: the first line of {path} is empty")
    return value


def signing_key():
    """The key the APK is signed with: (apksigner sign's key options, the
    environment to run it in, a description without any secret).

    MEMORIES_ANDROID_KEYSTORE set: the release key. The keystore (PKCS12 when
    it ends in .p12 or .pfx) and its password, MEMORIES_ANDROID_KEYSTORE_
    PASSWORD or the first line of MEMORIES_ANDROID_KEYSTORE_PASSWORD_FILE;
    the key's alias, MEMORIES_ANDROID_KEY_ALIAS (default yfm), and
    its password, MEMORIES_ANDROID_KEY_PASSWORD(_FILE) (default the
    keystore's). Anything missing stops the build: it never falls back to
    the debug key. The passwords reach apksigner as env:<name> in its own
    environment, so they are on no command line and in no file.

    Unset: the debug key, created under tmp/ the first time."""
    keystore = os.environ.get("MEMORIES_ANDROID_KEYSTORE")
    if not keystore:
        if not os.path.exists(KEYSTORE):
            home = os.environ.get("JAVA_HOME")
            keytool = (shutil.which("keytool") or (home and shutil.which("keytool", path=os.path.join(home, "bin"))) or
                       sys.exit("keytool is not on PATH or in $JAVA_HOME/bin (a JDK)"))
            run([keytool, "-genkeypair", "-keystore", KEYSTORE, "-alias", "androiddebugkey", "-storepass", "android",
                 "-keypass", "android", "-dname", "CN=Android Debug,O=Android,C=US", "-keyalg", "RSA",
                 "-keysize", "2048", "-validity", "10000"])
        return (["--ks", KEYSTORE, "--ks-pass", "pass:android", "--key-pass", "pass:android"], None,
                f"the debug key ({os.path.relpath(KEYSTORE, ROOT)}; MEMORIES_ANDROID_KEYSTORE is not set)")
    if not os.path.isfile(keystore):
        sys.exit(f"MEMORIES_ANDROID_KEYSTORE: {keystore} is not a file")
    store = secret("MEMORIES_ANDROID_KEYSTORE_PASSWORD") or sys.exit(
        "MEMORIES_ANDROID_KEYSTORE is set but its password is not: set MEMORIES_ANDROID_KEYSTORE_PASSWORD_FILE "
        "(a file whose first line is the password) or MEMORIES_ANDROID_KEYSTORE_PASSWORD")
    key = secret("MEMORIES_ANDROID_KEY_PASSWORD") or store
    # Out of this process's environment once read: javac, d8, aapt2 and
    # zipalign do not inherit them; only apksigner's own environment has them.
    for name in ("MEMORIES_ANDROID_KEYSTORE_PASSWORD", "MEMORIES_ANDROID_KEY_PASSWORD"):
        os.environ.pop(name, None)
    alias = os.environ.get("MEMORIES_ANDROID_KEY_ALIAS") or RELEASE_ALIAS
    options = ["--ks", keystore, "--ks-key-alias", alias, "--ks-pass", f"env:{STORE_PASS_VAR}",
               "--key-pass", f"env:{KEY_PASS_VAR}"]
    if keystore.lower().endswith((".p12", ".pfx")):
        options[2:2] = ["--ks-type", "PKCS12"]
    return options, dict(os.environ, **{STORE_PASS_VAR: store, KEY_PASS_VAR: key}), \
        f"the release key (alias {alias} in {keystore})"


def signer(apk, build_tools=None):
    """apksigner verify's verdict on an APK, which must have one signer:
    {"dn": ..., "sha256": ...} (64 lowercase hex digits). Stops the build when
    the APK does not verify."""
    build_tools = build_tools or build_tools_dir()
    output = run([tool(build_tools, "apksigner"), "verify", "--verbose", "--print-certs", apk])
    fields = {}
    for line in output.splitlines():
        for key, label in (("dn", "certificate DN: "), ("sha256", "certificate SHA-256 digest: ")):
            if line.startswith("Signer #") and label in line:
                fields.setdefault(key, []).append(line.split(label, 1)[1].strip())
    if len(fields.get("dn", [])) != 1 or len(fields.get("sha256", [])) != 1:
        sys.exit(f"{apk}: apksigner verify found {len(fields.get('sha256', []))} signers, not one:\n{output}")
    return {"dn": fields["dn"][0], "sha256": fields["sha256"][0].lower()}


def manifest_version(apk, build_tools=None):
    """The APK's (versionCode, versionName), from aapt2 dump badging."""
    build_tools = build_tools or build_tools_dir()
    first = run([tool(build_tools, "aapt2"), "dump", "badging", apk]).splitlines()[0]
    match = re.search(r"versionCode='(\d+)' versionName='([^']*)'", first)
    if not match:
        sys.exit(f"{apk}: no versionCode/versionName in aapt2's badging: {first}")
    return int(match.group(1)), match.group(2)


def colons(digest):
    """A SHA-256 digest as keytool and the Play Console print it."""
    return ":".join(digest[i:i + 2] for i in range(0, len(digest), 2)).upper()


def fingerprint_parts(expected):
    """MEMORIES_ANDROID_CERT_SHA256's value as [full] or [prefix, suffix]
    (lowercase hex): the full digest (64 hex digits, colons and case
    ignored), or a prefix and a suffix joined by "...", e.g.
    C8DA2D97...4B8B. Stops the build when it is neither."""
    parts = [part.replace(":", "").replace(" ", "").lower() for part in expected.split("...")]
    if len(parts) > 2 or not all(all(c in "0123456789abcdef" for c in part) for part in parts) or \
            (len(parts) == 1 and len(parts[0]) != 64) or (len(parts) == 2 and not (parts[0] or parts[1])):
        sys.exit(f"MEMORIES_ANDROID_CERT_SHA256 is {expected!r}: 64 hex digits, or PREFIX...SUFFIX")
    return parts


def check_fingerprint(digest, expected):
    """None when the certificate's SHA-256 `digest` matches `expected`
    (fingerprint_parts), else why not."""
    parts = fingerprint_parts(expected)
    if len(parts) == 1 and digest == parts[0]:
        return None
    if len(parts) == 2 and digest.startswith(parts[0]) and digest.endswith(parts[1]):
        return None
    return f"the signer's SHA-256 {colons(digest)} is not the expected {expected}"


def version_code(version):
    """versionCode for a release version vX.Y.Z[-LABEL[.N]] (VERSION above).
    Stops the build for one the scheme has no room for."""
    major, minor, patch, pre = VERSION.fullmatch(version).groups()
    major, minor, patch = int(major), int(minor), int(patch)
    if minor > 99 or patch > 99 or major > 2099:
        sys.exit(f"{version}: versionCode has room for minor and patch 0-99 and major up to 2099 "
                 f"(package_android.py; notes/pc-release.md, \"Version\")")
    stage = RELEASE_STAGE
    if pre:
        match = re.fullmatch(r"([a-z]+)(?:\.(\d+))?", pre)
        if not match or match.group(1) not in PRERELEASE or int(match.group(2) or 0) > 19:
            sys.exit(f"{version}: a pre-release for the APK is -LABEL.N with LABEL one of {', '.join(PRERELEASE)} "
                     f"(lowercase) and N 0-19, for its versionCode (package_android.py; notes/pc-release.md, "
                     f"\"Version\")")
        stage = PRERELEASE[match.group(1)] + int(match.group(2) or 0)
    code = major * 1000000 + minor * 10000 + patch * 100 + stage
    assert code <= MAX_CODE
    return code


def git(*arguments):
    """git's output in the checkout, or None when it fails (no git, no repository)."""
    try:
        result = subprocess.run(["git", *arguments], cwd=ROOT, capture_output=True, text=True)
    except OSError:
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def app_version():
    """(versionCode, versionName). The version is MEMORIES_VERSION when set
    (package.py's --version: the tag in CI), else the v* tag the checkout is
    exactly at, as for the desktop games (build_game32.release_version). A
    release version gives version_code() and its name without the v
    (0.2.0-preview.1). Anything else is a development build: the highest code
    of the v* tags it descends from (tags outside the scheme left out), and
    git describe's name (0.2.0-12-g<sha>), "-dirty" when the checkout has
    changes other than BUILD_WRITES; with no tag in reach (a shallow clone),
    VERSION_FLOOR and 0.0.0-dev."""
    version = os.environ.get("MEMORIES_VERSION")
    if version is None:
        version = git("describe", "--tags", "--exact-match", "--match", "v[0-9]*") or ""
    if VERSION.fullmatch(version):
        return version_code(version), version[1:]
    codes = []
    for tag in (git("tag", "--merged", "HEAD", "--list", "v[0-9]*") or "").split():
        if VERSION.fullmatch(tag):
            try:
                codes.append(version_code(tag))
            except SystemExit:   # an old tag outside the scheme does not stop a development build
                pass
    described = git("describe", "--tags", "--match", "v[0-9]*", "--abbrev=10")
    if not codes or not described:
        return VERSION_FLOOR, "0.0.0-dev"
    if git("diff", "--quiet", "HEAD", "--", ".", *(f":(exclude){path}" for path in BUILD_WRITES)) is None:
        described += "-dirty"
    return max(max(codes), VERSION_FLOOR), described[1:]


def program_files(build, folders):
    """The files under <build>/<folder> for each of `folders`, as assets
    under build/files/, and build/files.txt listing them (one relative path
    a line): an app cannot list its own assets, and android.c unpacks each
    one listed into the program directory."""
    assets, names = {}, []
    for folder in folders:
        for path in sorted(glob.glob(os.path.join(build, folder, "**", "*"), recursive=True)):
            if os.path.isfile(path):
                relative = os.path.relpath(path, build).replace(os.sep, "/")
                assets[f"build/files/{relative}"] = path
                names.append(relative)
    index = os.path.join(build, "files.txt")
    with open(index, "w", encoding="utf-8", newline="\n") as handle:
        handle.writelines(name + "\n" for name in names)
    assets["build/files.txt"] = index
    return assets


def check_report_authority():
    """The crash report's content:// authority is spelled in three places:
    the manifest here, ReportProvider.java and android_report.c (the URI
    Share hands out). A rename that missed one would only show as a share
    with nothing attached, so the build stops instead."""
    authority = f"{PACKAGE}.reports"
    for path in (os.path.join(JAVA, "ReportProvider.java"),
                 os.path.join(ROOT, "src", "pc", "platform", "android_report.c")):
        with open(path, encoding="utf-8") as source:
            if f'"{authority}"' not in source.read():
                sys.exit(f"{path} does not name the provider's authority {authority!r} (the manifest's)")


def package(build, abi, library, game, assets):
    apk_path = os.path.join(build, f"memories-{abi}.apk")
    if os.path.exists(apk_path):
        os.remove(apk_path)   # a failed build leaves no APK of an earlier build behind
    # The version, the key and the expected fingerprint first: a mistake in
    # any stops the build before anything is made.
    code, version_name = app_version()
    options, environment, description = signing_key()
    expected = os.environ.get("MEMORIES_ANDROID_CERT_SHA256")
    if expected:
        fingerprint_parts(expected)
    build_tools = build_tools_dir()
    android_jar = os.path.join(platform_dir(), "android.jar")
    work = os.path.join(build, "apk")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(os.path.join(work, "classes"))
    os.makedirs(os.path.join(work, "dex"))
    # SDL's Java shell, from the SDL release libSDL3.so was built from, and
    # the game's own (JAVA): the activity that restarts it (Restart.java), the
    # HD pack's download (HdDownload.java) and the crash report's
    # ReportProvider.
    sources = sorted(glob.glob(os.path.join(build_android_deps.OUT, "java", "**", "*.java"), recursive=True))
    sources += sorted(glob.glob(os.path.join(JAVA, "*.java")))
    check_report_authority()
    javac = shutil.which("javac") or sys.exit("javac is not on PATH (a JDK, 17 or later)")
    run([javac, "--release", "11", "-nowarn", "-encoding", "UTF-8", "-classpath", android_jar,
         "-d", os.path.join(work, "classes"), *sources])
    classes = sorted(glob.glob(os.path.join(work, "classes", "**", "*.class"), recursive=True))
    run([tool(build_tools, "d8"), "--release", "--min-api", str(build_android_deps.API), "--lib", android_jar,
         "--output", os.path.join(work, "dex"), *classes])
    with open(os.path.join(work, "AndroidManifest.xml"), "w", encoding="utf-8") as handle:
        handle.write(MANIFEST.replace("@VERSION_CODE@", str(code)).replace("@VERSION_NAME@", version_name)
                     .replace("@CONFIG_CHANGES@", CONFIG_CHANGES))
    unaligned = os.path.join(work, "unaligned.apk")
    run([tool(build_tools, "aapt2"), "link", "-o", unaligned, "-I", android_jar, "--manifest",
         os.path.join(work, "AndroidManifest.xml"), "--min-sdk-version", str(build_android_deps.API),
         "--target-sdk-version", str(TARGET_SDK)])
    deps_lib = os.path.join(build_android_deps.OUT, abi, "lib")
    with zipfile.ZipFile(unaligned, "a", zipfile.ZIP_DEFLATED) as apk:
        apk.write(os.path.join(work, "dex", "classes.dex"), "classes.dex")
        apk.write(library, f"lib/{abi}/libmain.so")
        apk.write(game, f"lib/{abi}/libgame.so")
        for name, path in sorted(assets.items()):
            apk.write(path, f"assets/{name}")
        apk.write(os.path.join(deps_lib, "libSDL3.so"), f"lib/{abi}/libSDL3.so")
    aligned = os.path.join(work, "aligned.apk")
    run([tool(build_tools, "zipalign"), "-p", "-f", "4", unaligned, aligned])
    run([tool(build_tools, "apksigner"), "sign", *options, "--out", apk_path, aligned], env=environment)
    certificate = signer(apk_path, build_tools)
    print(f"{apk_path}: {PACKAGE} {version_name} (versionCode {code}), "
          f"lib/{abi}/libmain.so + libgame.so + libSDL3.so, {len(assets)} assets")
    print(f"{apk_path}: signed with {description}, verified (apksigner verify)\n"
          f"  certificate: {certificate['dn']}\n  SHA-256: {colons(certificate['sha256'])}")
    if expected:
        mismatch = check_fingerprint(certificate["sha256"], expected)
        if mismatch:
            os.remove(apk_path)
            sys.exit(f"{apk_path}: removed: {mismatch} (MEMORIES_ANDROID_CERT_SHA256)")
        print(f"  matches MEMORIES_ANDROID_CERT_SHA256 ({expected})")
    return apk_path


if __name__ == "__main__":
    build = sys.argv[1]
    with open(os.path.join(build, "buildid")) as handle:
        build_id = handle.read().strip()
    assets = {"build/buildid": os.path.join(build, "buildid"), "build/commit": os.path.join(build, "commit"),
              f"build/symbols/{build_id}.txt": os.path.join(build, "symbols", f"{build_id}.txt"),
              f"build/symbols/{build_id}.vars.txt": os.path.join(build, "symbols", f"{build_id}.vars.txt")}
    assets.update(program_files(build, ("mods", "languages")))
    package(build, sys.argv[2], os.path.join(build, "libmain.so"), os.path.join(build, "libgame.so"), assets)
