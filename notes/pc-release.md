# PC release packaging

The release is an unpack-and-run folder. Windows uses a GUI executable
(no console window); Linux uses the SDL executable, built against the
existing Debian 11 i386 sysroot. Both remain 32-bit builds. macOS ships a
native Apple Silicon application bundle inside a ZIP.

```text
yfm-redecomp-<version>/
  memories-pc.exe + SDL3.dll    # Windows ZIP
  memories-pc.pdb              # Windows symbols (debuggers, profilers)
  memories-pc                  # Linux tar.gz, executable permission retained
  README.txt
  LICENSE
  buildid
  commit
  game/README.txt              # optional auto-detected ROM location
  mods/                       # bundled mods, from tracked project files
  sdk/                        # headers, tools, examples, modding notes
  symbols/                    # this build's crash/save-state symbol tables
```

The macOS ZIP contains the app bundle at its root, alongside its short
launch/readme and license files:

```text
YFM Re-Decomp.app/             # ad-hoc signed, native Apple Silicon
  Contents/MacOS/memories-arm64
  Contents/Resources/          # languages, build metadata, dependency licenses
README.txt
LICENSE.txt
```

GitHub shows each release asset's SHA-256. The macOS ZIP also ships a
`.zip.sha256` sidecar for independent verification. No ROM, extracted game
data, user settings, saves, reports or personal HD packs belong in the
archive. Release builds omit the optional executable icon extracted from a
local disc.

The macOS app is signed ad hoc for bundle integrity, without an Apple
Developer ID signature or notarization. Gatekeeper may block a downloaded
copy; the bundled README explains how to open it through Privacy & Security
without disabling Gatekeeper globally.

The Windows executable is what virus scanners' heuristics judge, so the
release keeps it plain: `package.py` strips the symbol table and debug
sections (`memories-pc.pdb` has them) and fills in the PE checksum the linker
leaves 0; the build gives it version information, names its PDB without the
builder's path, and leaves test-only paths out (`MEMORIES_TEST_HOOKS`,
[PC build](pc-build.md)); the game does not call `SetProcessDEPPolicy`.
v0.1.4-preview.1 was flagged by 11 engines with generic machine-learning
labels (a false positive): Bitdefender's engines over the GitHub runner's PDB
path, the rest with no reason given, in the release that had gained the
DEP-policy imports. `test_package.py` checks all of these on every release.

## Launch experience

If a remembered or auto-detected disc is available, launch goes straight
into the game. Otherwise a normal information dialog welcomes the player,
explains the USA SLUS-01411 raw `.bin` requirement, and offers **Choose ROM...**
and **Quit**. Choose ROM opens the system file picker. Cancel exits with
status zero. Invalid/unreadable selections show an error and allow another
attempt. Failure to save the location is also reported.

Only the path is saved, as UTF-8 in `disc-path.txt` in the existing user
folder. The ROM is never copied or modified. A moved/missing selection brings
setup back on the next launch. The existing `game/` discovery and explicit
`MEMORIES_DISC` override still work. Headless launches and a broken explicit
override fail without opening a picker. The developer X11 backend retains
folder/environment-based discovery; shipped builds explicitly use SDL.

SDL's asynchronous picker is awaited while pumping events, with the crash
monitor paused for user interaction. Linux uses the desktop portal or Zenity;
a failed picker explains the `game/` folder fallback. See the
[SDL dialog contract](https://wiki.libsdl.org/SDL3/SDL_ShowOpenFileDialog).

## GitHub build flow

`.github/workflows/pc-release.yml` builds on pull requests, pushes to `master`,
`v*` tags and manual dispatch. Native Ubuntu, Windows and Apple Silicon
runners use the existing dependency/toolchain fetchers and cache only
dependencies. Linux selects GCC 14 because the game source build uses C
`-fpermissive`. The macOS job builds the ARM64 game without a disc, wraps it
in an ad-hoc signed `.app`, and checks the ZIP contents; it needs no signing
secret and also runs on fork pull requests or manual dispatch.

Standard runs upload the Windows ZIP and Linux tar.gz as Actions artifacts.
The macOS ARM64 job uploads its ZIP and SHA-256 sidecar as an artifact on every
run. Artifacts are retained for 14 days; downloading one requires a GitHub
sign-in and read access to the repository. A manual run offers `all` (the
default) or `macos-arm64`; selecting the latter skips Windows, Linux and
Android packaging, so a fork can make the Mac artifact without Android release
secrets. Manual runs only make Actions artifacts. The workflow must already exist on
the default branch with `workflow_dispatch`; select the support branch in the
Branch dropdown. See [GitHub manual workflow instructions](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow). The `android` job builds the
arm64 APK (the translated game: it fetches the official LLVM 21.1.8 Linux
archive once, cached as `tmp/pc/llvm-linux`, for the guest pass; notes/pc-build.md,
"The app's game is the translated one") and, with the release key (pushes, tags and manual runs; "Android
signing" below), uploads it signed as
`yfm-redecomp-<version>-android-arm64.apk`. On a version tag, all jobs must
succeed before the final job creates a **draft** GitHub release and attaches
the packages. Each SDK carries the guest pass of its system
(`sdk/tools/memories-guest-ir[.exe]`, built against the LLVM of the SDK's
llvm-mingw clang by the `sdk-guest-ir` job: `tools/pc/sdk_llvm.json` pins it,
notes/pc-build.md "The SDK's guest pass"), and the `package` job builds a
bundled code mod for Android from the packed SDK to prove it
(`sdk-mod-ir-*` artifacts).

*The guest pass's LLVM in CI.* Its libraries are the costly part (a Windows
build is up to an hour, the Linux archive 2 GB), so `sdk-guest-ir` caches
them under a key of the pin, `fetch_tools.py` and the script, saving the
cache in a step of its own as soon as they are built (a failure while
linking the pass keeps the work). Caches saved by master are readable by
pull requests, which therefore link the pass in a minute; a pull request
builds the LLVM itself only when it touches what the pass is made of
(`tools/pc/llvm/`, `build_sdk_guest_ir.py`, `sdk_llvm.json`,
`fetch_tools.py`, `src/pc/memory_map.h`). A pull request that touches
none of them while master has not yet saved the cache (right after the pin
changed) builds no pass: its SDKs are packed with `package.py
--no-guest-ir` and tested with `test_package.py --no-guest-ir`. Pushes,
tags and manual runs always have the pass, or fail.

*Changing the LLVM pin.* When llvm-mingw's clang changes major (the toolchain
pinned in `tools/pc/fetch_tools.py` and `build_win32_deps.py`), the SDK's pass
has to follow: edit `llvm_major`, `version` and both URLs in
`tools/pc/sdk_llvm.json` (the version llvm-mingw's release notes name), clear
the two `sha256` values, run `python tools/pc/build_sdk_guest_ir.py --pin`
(it reads the digests GitHub publishes for the release; nothing is
downloaded) and commit; change `LLVM_MINGW_CLANG` in `build_mod.py` and the
numbers `notes/modding.md` names to match (`test_build_mod_toolchain.py`
fails when the two disagree). The cache key follows the file.
Reruns can update a draft but refuse to replace an already published release.
Draft assets are not public until the draft is published; published release
assets are the persistent download, subject to the repository's visibility.
A fork without the Android release key can still make the manual Mac artifact,
but cannot complete the tag-triggered draft release, which requires every job
to succeed.

Two kinds of tag:

- `v0.2.0-preview.1` (any hyphen, also `-rc.1`, `-beta.2`): a **preview**.
  The draft is marked as a pre-release, so GitHub labels it and "Latest
  release" keeps pointing at the last real one.
- `v0.2.0`: a **release**, a normal draft.

To make one: `git tag v0.2.0-preview.1 origin/master && git push origin
v0.2.0-preview.1`, wait for the workflow, test the attached archives, then
edit the draft on the Releases page and press Publish. Nothing is public
until then. A bad draft can be deleted along with its tag
(`gh release delete v0.2.0-preview.1 --cleanup-tag`).
The normal PC foundation workflow continues running the full adapter tests
on both platforms; it also includes the new ROM setup tests.

Public runners do not receive a disc or need a ROM secret. They compile the
full game and check archive structure; they skip ROM-dependent gameplay smoke
tests explicitly. Run those locally before publishing a draft. Workflow events
follow [GitHub's trigger documentation](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/trigger-a-workflow).

### Mods made for earlier releases

A mod made for one release has to keep working in the later ones.
`tools/pc/mod_compat.txt` lists the releases that promise covers, and
`tools/pc/check_mod_abi.py` enforces it:

- In CI, on every pull request (Linux job, no disc), it compares the SDK
  just built with each listed release's SDK. Every name that release lent
  a code mod must still be exported. Every exported function and variable
  its headers declare must keep its type. Every structure they reach must
  keep its layout, and every enumerator its value.
- Locally, `smoke.py` (and so `package.py`) runs it with `--run`. This
  loads each listed release's own mods, and its SDK's examples built with
  its own `build_mod.py`, into the new build. Every code mod must load.
  Each smoke case that turns mods on must draw the same frame with the old
  release's copies of those mods as with this build's. Each run plays in a
  folder of its own, `tmp/pc/mod-compat/run/<tag>-XXXXXXXX`, kept when it
  found a difference not accepted in `mod_compat.txt`: worktrees share `tmp/`, and two checks at once used to
  take each other's frames.

Both run what a release package holds (its SDK's headers and tools, its
mods' objects), so the package is pinned: `mod_compat.txt` has a
`sha256 TAG SYSTEM DIGEST` line for each listed release's Windows `.zip`
and Linux `.tar.gz`, the digest GitHub's release API gives the asset. A
package is downloaded once into `tmp/pc/mod-compat/<tag>/<system>/` and
unpacked only if its sha256 is the pinned one, and only if every name in
it is under its `yfm-redecomp-<tag>/` folder (`yfm-redecomp-<tag>-x64/` in
the 64-bit Windows one). It is unpacked into
`verified/` there, where a check from before the pinning (which unpacks
beside the package, unchecked) never writes. The package stays beside it
and is hashed again whenever the folder is used. A folder without its
package (one copied in) is fetched again rather than trusted; a package
that differs is refused, with the folder to delete. `--baseline <folder>`
runs an unpacked folder as it is, unchecked.

A difference that fails the check is fixed, not waved through. Keep the old
number, argument list or layout, and add beside it. Only a difference
that provably breaks no mod goes in `mod_compat.txt` as `accept`, with the
reason. After publishing a release, add `baseline <tag>` for it to
`mod_compat.txt`, with its two `sha256` lines from
`gh api repos/Unchiga/Yu-Gi-Oh-Forbidden-Memories-Recompiled/releases/tags/<tag> --jq '.assets[] | .name + " " + (.digest | ltrimstr("sha256:"))'`
(`test_mod_compat_fetch.py`, a CTest, fails while a baseline lacks one).
The first release whose 64-bit Windows package carries x86_64-windows mods
also gets `baseline <tag> x86_64-windows`, with a `sha256 <tag> windows-x64`
line for its `-windows-x64.zip`: the 64-bit game's SDK is compared with
those (none is listed yet; until then that game is only held to refusing
the 32-bit releases' code mods by name).

## Android signing

> **WARNING: back up the release key, in two places.** Android installs an
> update only when it is signed with the same key as the app already on the
> phone. If the release keystore or its password is lost, no later version
> can ever be installed over the published ones: every player would have to
> uninstall the app (and lose its files) to get the next one. Keep the
> keystore file *and* its password backed up in at least two separate places
> (for example a password manager and an encrypted offline drive), and never
> put either in the repository, an issue, a chat, a build folder or a log.

**The release key is the one in this repository's Actions secrets, held by
Unchiga.** Only the `android` job of `pc-release.yml` signs release APKs with
it. A key kept on a developer's machine (such as
`%USERPROFILE%\.config\yfm\android-release.p12`) is a test key, for trying
the mechanism below; an APK signed with it does not install over the
released app and is never published.

`tools/pc/package_android.py` signs the APK the Android build makes
(`build_game32.py --target android-arm64-v8a`, and so `package.py
android-arm64`) with the key the environment names, else with the debug key
under `tmp/pc/android-deps/debug.keystore`, as before:

| Variable | Meaning |
| --- | --- |
| `MEMORIES_ANDROID_KEYSTORE` | The keystore's path. Set: the release key signs; unset: the debug key. A `.p12`/`.pfx` file is read as PKCS12. |
| `MEMORIES_ANDROID_KEYSTORE_PASSWORD_FILE` | A file whose first line is the keystore's password (local builds). |
| `MEMORIES_ANDROID_KEYSTORE_PASSWORD` | The keystore's password itself (CI, from a secret). Used before the `_FILE` one. |
| `MEMORIES_ANDROID_KEY_ALIAS` | The key's alias; default `yfm`. The CI key's is in the `ANDROID_KEY_ALIAS` secret. |
| `MEMORIES_ANDROID_KEY_PASSWORD`, `MEMORIES_ANDROID_KEY_PASSWORD_FILE` | The key's password, as above; default the keystore's (a PKCS12 file has one password). |
| `MEMORIES_ANDROID_CERT_SHA256` | Optional: the signer's expected certificate SHA-256, the full 64 hex digits (colons and case do not matter) or `PREFIX...SUFFIX`. A mismatch deletes the APK and fails the build. |

With `MEMORIES_ANDROID_KEYSTORE` set, anything missing or wrong (no password,
a wrong alias or password, a file that is not a keystore) fails the build:
it never falls back to the debug key. The passwords are never printed. They
reach `apksigner` as `env:<name>` in its own environment, so they are on no
command line and in no file the build writes. Once read, the two password
variables are removed from the build's environment, so `javac`, `d8`,
`aapt2` and `zipalign` do not inherit them. Do **not** point both password
variables at one `_FILE`. That is safe here, but `apksigner`'s own
`--ks-pass file:X --key-pass file:X` would read the key password from the
second line. After signing, the build runs `apksigner verify --print-certs`
and prints the signer:

```text
tmp/pc/android-arm64-v8a/memories-arm64-v8a.apk: signed with the release key (alias yfm in ...), verified (apksigner verify)
  certificate: CN=..., O=...
  SHA-256: C8:DA:2D:97:...:4B:8B
```

`package.py android-arm64` packs `dist/yfm-redecomp-<version>-android-arm64.apk`
only with `MEMORIES_ANDROID_KEYSTORE` set. It refuses an APK whose signer is
the debug certificate (`CN=Android Debug`), it checks
`MEMORIES_ANDROID_CERT_SHA256` when set, and it refuses an APK whose
`versionCode`/`versionName` are not what `--version` makes ("Version" below):
with `--no-build`, an earlier build's APK is not packed under a new version.
So a debug-signed or stale APK never reaches `dist/` and never becomes a
release asset.

The SDK tools are the ones `ANDROID_BUILD_TOOLS` names (a build-tools
version, `35.0.0` in the workflow) and the platform `android-35` (the target
SDK). Without them, the newest installed ones are taken, compared by version
number, passing over previews and extensions (`36.0.0-rc1`,
`android-36-ext19`, `android-37.2-beta3`).

```sh
# A test of the mechanism with a local key (bash; the key and its password
# file live outside the repository):
export MEMORIES_ANDROID_KEYSTORE=~/.config/yfm/android-release.p12
export MEMORIES_ANDROID_KEYSTORE_PASSWORD_FILE=~/.config/yfm/android-release.password
export MEMORIES_ANDROID_KEY_ALIAS=yfm
python3 tools/pc/package.py android-arm64 --version dev-signing-test
```

### The secrets (CI)

**Done (2026-10):** Unchiga added these four secrets, with his release key,
to `Unchiga/Yu-Gi-Oh-Forbidden-Memories-Recompiled`. The steps below are kept
for reference: to replace the key's copy there, or to set up a fork.

In the GitHub repository: **Settings > Secrets and variables > Actions**,
**Secrets** tab, **New repository secret**, one for each of:

| Secret | Value |
| --- | --- |
| `ANDROID_KEYSTORE_BASE64` | The PKCS12 keystore file, base64-encoded on one line (below). |
| `ANDROID_KEYSTORE_PASSWORD` | The keystore's password. |
| `ANDROID_KEY_ALIAS` | The key's alias in it. |
| `ANDROID_KEY_PASSWORD` | The key's password (for a PKCS12 file, the keystore's). |

Optionally, on the **Variables** tab, `ANDROID_CERT_SHA256`: the release
certificate's full SHA-256. The `android` job checks the signer against it.
Without it, the job checks the release certificate's full SHA-256, written
in the workflow
(`C8DA2D972E42522E9F83139F5D54092F69FB3EE4C0B074FA53F8245DA45A4B8B`). The
variable is public information, not a secret.

The base64 text of the keystore, on one line, with no header:

```powershell
# Windows PowerShell: copies it to the clipboard, to paste in the secret's box
[Convert]::ToBase64String([IO.File]::ReadAllBytes("C:\path\to\release.p12")) | Set-Clipboard
```

```sh
# Linux (GNU coreutils): -w0 writes one line
base64 -w0 /path/to/release.p12 > release.p12.b64   # paste its content, then delete the file
# macOS: base64 -i /path/to/release.p12 | pbcopy
```

Do not use `certutil -encode`: its `-----BEGIN CERTIFICATE-----` lines would
be decoded with the key. With the GitHub CLI, the secrets can be set without
the clipboard. `gh secret set NAME` with no value asks for it without showing
it:

```sh
base64 -w0 /path/to/release.p12 | gh secret set ANDROID_KEYSTORE_BASE64 -R Unchiga/Yu-Gi-Oh-Forbidden-Memories-Recompiled
gh secret set ANDROID_KEYSTORE_PASSWORD -R Unchiga/Yu-Gi-Oh-Forbidden-Memories-Recompiled   # prompts
gh secret set ANDROID_KEY_ALIAS -R Unchiga/Yu-Gi-Oh-Forbidden-Memories-Recompiled
gh secret set ANDROID_KEY_PASSWORD -R Unchiga/Yu-Gi-Oh-Forbidden-Memories-Recompiled
```

```powershell
[Convert]::ToBase64String([IO.File]::ReadAllBytes("C:\path\to\release.p12")) | gh secret set ANDROID_KEYSTORE_BASE64 -R Unchiga/Yu-Gi-Oh-Forbidden-Memories-Recompiled
```

The certificate's SHA-256, for `ANDROID_CERT_SHA256` or to compare with the
job's summary: `keytool -list -v -storetype PKCS12 -keystore release.p12
-alias <alias>` (it asks for the password; the `SHA256:` line), or `apksigner
verify --print-certs <apk>` on an APK it signed.

In the job, the build (the NDK, cmake, the dependencies' downloaded sources)
runs with no secret in its environment and makes a debug-signed APK. Only
then does one step, "Sign the APK with the release key", get the secrets. It
decodes the keystore into the runner's temporary folder (`umask 077`), with
its path in that step only, never in `GITHUB_ENV`. It re-packages and signs
the built APK with `package_android.py <build> arm64-v8a`. Then it packs it
with `package.py android-arm64 --no-build`, without the passwords in its
environment. The keystore is deleted when the step ends, and again by a
last `if: always()` step. The job prints the signer's DN, SHA-256 and the
APK's version to its summary. Only a push to `master` or of a `v*` tag, or a
manual run (workflow_dispatch) on `master` or a `v*` tag, gets the release
key. The repository is public and anyone can download a run's artifacts, so
unmerged code is never signed. Pull requests, manual runs on other branches,
and forks without the secrets build the APK as a check, debug-signed, and
upload nothing. A version tag without the secrets fails the job, and with
it the draft release.

GitHub hides every secret's value in the logs. `ANDROID_KEY_ALIAS` is
`yfm`, so the job's log shows `***` wherever that word appears
(`dist/***-redecomp-v0.3.0-android-arm64.apk`). This is only in the log; the
files and the release assets are named correctly. An alias is not secret: it
could become a repository variable (`ANDROID_KEY_ALIAS` on the **Variables**
tab, read as `vars.ANDROID_KEY_ALIAS` in the workflow), and the logs would
show it plainly.

### Version

`package_android.py` writes the APK's `versionCode` and `versionName` from
the build's version: `MEMORIES_VERSION` (`package.py --version`, the tag in
CI), else the `v*` tag the checkout is exactly at, as for the desktop
games' update check ([updates](updates.md)). Android installs an APK only
over one with the same or a lower `versionCode`, so codes must rise from
release to release. For `vMAJOR.MINOR.PATCH[-LABEL.N]`:

```text
versionCode = MAJOR*1000000 + MINOR*10000 + PATCH*100 + STAGE
STAGE       = 99 for the release itself
              alpha+N: 0+N, beta+N: 20+N, preview+N: 40+N, rc+N: 60+N   (N 0-19)
```

| Tag | versionCode | versionName |
| --- | --- | --- |
| `v0.2.0` | 20099 | `0.2.0` |
| `v0.3.0-preview.1` | 30041 | `0.3.0-preview.1` |
| `v0.3.0-rc.2` | 30062 | `0.3.0-rc.2` |
| `v0.3.0` | 30099 | `0.3.0` |
| `v1.0.0` | 1000099 | `1.0.0` |

The codes rise in the order the versions compare (semver:
alpha < beta < preview < rc < release). MINOR and PATCH are 0-99, and N is
0-19, LABEL is lowercase (the update check compares labels byte by byte, so
`RC` would sort before `alpha`), and MAJOR is at most 2099: Google Play
takes codes up to 2100000000. Change the scheme here only upward: a code
lower than a published one can never update it.

**A tag outside the scheme fails the release.** A version tag like
`v0.3.0-nightly.1`, `v0.3.0-rc.20`, `v0.3.0-RC.1` or `v0.100.0` stops the
Android build with a message naming the rule. `draft-release` needs the
`android` job, so no draft is made for that tag, with no desktop archives
either. This is on purpose: a release is never published without its APK.
The `package` jobs need it too (and `sdk-exports-x64`): their SDKs carry
the arm64 game's export list, which that job builds (notes/pc-build.md,
"Sharing a build"). So when the Android build fails, on a tag or a pull
request, the desktop archives and the checks after them (`test_package.py`,
`check_mod_abi.py`, the FM Editor's) do not run either, and they start only
once it is done.
Delete the tag and tag again within the scheme
(`git push --delete origin <tag>`, then a new tag).

A development build (`dev-<sha>` in CI, an untagged checkout) takes the
highest code of the `v*` tags it descends from (`git tag --merged HEAD`;
tags outside the scheme are left out): an equal code installs over that
release, and the next release is higher. Its name is `git describe`'s
(`0.2.0-12-g2dffe92641`), with `-dirty` when the checkout has changes. The
build's own rewrite of `config/pc/guest_addresses.txt` does not count. With no tag in reach (a shallow clone), the code is
2 and the name `0.0.0-dev`; the `android` job checks out the whole history
(`fetch-depth: 0`), so it always has the tags. Every code is above the
`versionCode` 1 of the APKs made before this (`versionName` `m1`).
`aapt2 dump badging <apk>` (build-tools) shows both:

```text
package: name='org.yfmredecomp.game' versionCode='30041' versionName='0.3.0-preview.1' ...
```

### Test APKs signed with the debug key

An APK built without the release key (every local
`build_game32.py --target android-arm64-v8a`, earlier test builds) is signed
with a debug key. Android refuses to install the release APK over it:

```text
adb: failed to install yfm-redecomp-v0.3.0-android-arm64.apk: Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE: Existing package org.yfmredecomp.game signatures do not match newer version; ignoring!]
```

Uninstall the test app once before the first release APK (`adb uninstall
org.yfmredecomp.game`, or long-press the icon, then Uninstall). Uninstalling
deletes the app's folder, `Android/data/org.yfmredecomp.game/files/`. It
holds the copied disc image, the saves and the settings. Copy it out first if
they matter (`adb pull /sdcard/Android/data/org.yfmredecomp.game/files`).
From then on each release installs over the last.

## Local commands

```sh
# Build the CTests used by the Linux smoke runner first:
cmake -S . -B tmp/pc/cmake-test -DCMAKE_BUILD_TYPE=Release
cmake --build tmp/pc/cmake-test --parallel

# Build, smoke-test with your own disc, then package all three archives
# (32-bit Windows, 64-bit Windows, Linux; the 64-bit one is skipped with a
# message where x86_64-w64-mingw32-clang 21 or later is missing, and then its
# export list must come from a build of the same tree, --exports-from). Every
# SDK carries the arm64 game's export list too: build it at this commit first.
python tools/pc/build_game32.py --target android-arm64-v8a --backend sdl --release --build tmp/pc/android-arm64-v8a
python tools/pc/package.py --version v0.1.0

# Compile/package without a disc (the CI path). Each SDK takes the other
# targets' export lists from their builds of the same tree (notes/pc-build.md,
# "Sharing a build"), so build those first, or name them with --exports-from:
python tools/pc/build_game32.py --target android-arm64-v8a --backend sdl --release --build tmp/pc/android-arm64-v8a
python tools/pc/build_game32.py --target windows-x64 --backend sdl --release --build tmp/pc/win64
python tools/pc/package.py linux --skip-smoke --version dev-preview
python tools/pc/package.py windows --skip-smoke --version dev-preview

# Inspect the artifacts without a disc:
python tools/pc/test_package.py --archives dist
```

`--no-build` packages existing outputs; use it only after explicitly building
with `--release`, since it cannot change a previously built executable.
Windows builds use `tmp/pc/win32` during packaging on both host platforms.

Before publishing: play-test the packaged builds on Linux and real Windows,
including first-run selection, invalid selection, cancellation, moving the
ROM, restart with the remembered path, sound, controller input, and an in-game
save/load. Wine checks help but do not replace native Windows validation.
Upgrade by extracting into a fresh folder. A build packaged with a `vX.Y.Z`
version checks the releases at start (Help menu turns it off) and tells the player
a newer one is out; it never installs it itself ([updates](updates.md)). `--version` is the
version the build compares with (`MEMORIES_VERSION`); CI passes the tag, so
tag builds check and `dev-*` builds do not. User settings and memory-card
saves persist; cross-build save-state compatibility is not guaranteed.

## VirusTotal

Generic machine-learning and heuristic engines keep flagging the Windows
`memories-pc.exe` (v0.1.4-preview.1: 11 of 71). `tools/pc/vt_check.py`
measures that without uploading by hand on the website: it looks each file
up by SHA-256 (API v3), uploads the ones VirusTotal has not seen, waits for
the analysis and prints detections/total, the label of every engine that
flagged the file, an engine × file matrix when there are several files, and
each file's page (`https://www.virustotal.com/gui/file/<sha256>`). The
count is the engines that said "malicious", out of those that gave any
verdict, as the website counts; "suspicious" verdicts are listed apart.

**Key.** Sign up at virustotal.com (a free community account); the key is
under the profile menu, *API key*. Keep it out of the repository: the script
reads `VT_API_KEY`, else `~/.config/yfm/vt_api_key` (on Windows
`%USERPROFILE%\.config\yfm\vt_api_key`; make it readable only by you). It
never prints the key, only where it came from.

**Terms and limits.** The free public API is for non-commercial use only
(this project qualifies; a commercial product or service needs VirusTotal's
premium API). It allows 4 requests a minute, 500 a day and 15.5 thousand a
month. The script spaces its requests 15 s apart (`--rate`), backs off on
HTTP 429, and stops at `--daily` requests (500) in one run. A lookup is one
request; an upload is one or two more, then one per 15 s until the analysis
is done, usually a few minutes.

**Uploads are public.** Every uploaded file is shared with the antivirus
vendors and downloadable by VirusTotal's premium users. Upload only builds
that are, or will be, public anyway: release candidates of our own
executable. Never anything with game data (a disc image, extracted files,
HD packs), nor builds nobody will ship. `--lookup-only` never uploads.

**Bisecting locally.** Build the variants exactly like the release (as
`package.py windows` does: `build_game32.py --target windows --backend sdl
--release --build tmp/pc/<name>` with `MEMORIES_VERSION` set, then that
commit's `package.strip()` on a copy, since the shipped exe is stripped; a
build dir of its own, because worktrees share `tmp/`). Then:

```sh
python tools/pc/vt_check.py --lookup-only old-release.exe      # already public: no upload
python tools/pc/vt_check.py --label master --label no-dep a.exe b.exe --json vt.json
python tools/pc/vt_check.py --reanalyze --label v0.1.4 v0.1.4.exe   # rescan with today's engines
```

The same engine names down one column tell which change moved which
engine. Engines' verdicts drift over days (a rescan can flag a file that
was clean when it shipped), so compare variants scanned the same day.
`--max N` exits 1 when a file has more than N detections; 2 means the
check itself failed (no key, network, quota, `--timeout`).

**In CI.** The release workflow's *VirusTotal* step (Windows job) runs the
script on the shipped `memories-pc.exe`, the SDK's `memories-guest-ir.exe`
(`sdk/tools/`, a program of its own that is built, stripped and given
version information as the game is) and `fm-editor.exe`, taken from
the archives, and writes the table, the matrix and the links into the job
summary. It warns above `VT_MAX_DETECTIONS` (2) and never fails the build;
when VirusTotal is down or slow it warns too, after at most 10 minutes an
analysis (`--timeout 600`) and 30 for the step. McAfeeD's reputation verdict (`ti!<hash>`) marks every file VirusTotal has
just met, so 0 would warn on every release. It runs only on version tags and
manual runs (*Run workflow*), so only those builds are uploaded; never on
pull requests nor on pushes to `master`, and it is skipped when the
repository has no `VT_API_KEY` secret (forks). A repository admin adds the secret with
`gh secret set VT_API_KEY` (it prompts for the value, so it stays out of
the shell history); removing it turns the step off.
