# Building and testing

## Pinned toolchain and dependencies

| Item | Pin | Where |
| --- | --- | --- |
| C++ standard | C++20 | `CMakeLists.txt` |
| Windows compiler | MSVC 14.44 (cl 19.44, Visual Studio 2022 17.14) | `TC_PINNED_MSVC_VERSION` in `CMakeLists.txt`, CI `toolset` |
| CMake | 3.25 or newer | `CMakePresets.json` |
| vcpkg registry | commit `3cbc1db4d867ec83c89fba4c461321c11f78b5e3` | `builtin-baseline` in `vcpkg.json`, `VCPKG_COMMIT` in CI |
| libtorrent | 2.1.2 exactly (`default-features: false`) | `overrides` in `vcpkg.json`, `find_package(... 2.1.2 EXACT)` |
| Windows triplet | `x64-windows-static-md` (static libraries, dynamic CRT) | `CMakePresets.json` |

Other dependency versions (Boost, OpenSSL, nlohmann-json, Catch2, WebView2 SDK,
WIL) are fixed by the vcpkg baseline commit. Configure with
`-DTC_REQUIRE_PINNED_TOOLCHAIN=ON` to turn a compiler mismatch into an error
(CI does this).

## Windows

Prerequisites: Visual Studio 2022 17.14 with the "Desktop development with
C++" workload (MSVC v14.44 and a Windows 10/11 SDK), Git, Python 3.9+ (for the
integration tests), and Ninja (bundled with Visual Studio's CMake tools, or
`pip install ninja`).

```powershell
git clone https://github.com/microsoft/vcpkg C:\src\vcpkg
git -C C:\src\vcpkg checkout 3cbc1db4d867ec83c89fba4c461321c11f78b5e3
C:\src\vcpkg\bootstrap-vcpkg.bat -disableMetrics
$env:VCPKG_ROOT = "C:\src\vcpkg"

# In a developer prompt for the pinned toolset:
#   "C:\Program Files\Microsoft Visual Studio\2022\<Edition>\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release
```

The application is `build\windows-x64-release\src\app\windows\TorrentControl.exe`,
with its `frontend` folder next to it. It needs the Microsoft Edge WebView2
Runtime (Evergreen), version 113.0.1774.30 or newer. A missing, unrecognized or
older Runtime shows a native message offering the official download page.
Use current Evergreen; see [compatibility and settings persistence](settings-runtime-reliability.md).

`TorrentControl.exe --self-test <log>` loads the UI, checks the real bridge,
HTML profile dialog, saved-profile reload and manual magnet-copy selection,
writes the log, and exits with code 0 on success. It uses an isolated data
folder beside the log and leaves normal user settings untouched. CI runs it
on every build; see [M2 stabilization](m2-stabilization.md).

CI now additionally runs `tests/windows/run-native-flow.ps1`: real Windows
file/folder/save dialogs, v1/v2/hybrid creation and verification, project
round-trip, renderer crash recovery, settings process restart, missing Runtime
and independent verification of the native outputs. Logs and fixtures are
uploaded as `windows-native-evidence`. See
[Windows / WebView2 validation](windows-native-validation.md) for scope and limits.

## Linux (headless core)

The core, bridge, tools and tests are platform independent and are built and
tested on Linux in CI. The Windows host is skipped automatically.

```sh
git clone https://github.com/microsoft/vcpkg ~/vcpkg
git -C ~/vcpkg checkout 3cbc1db4d867ec83c89fba4c461321c11f78b5e3
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_ROOT=~/vcpkg
cmake --preset linux-release && cmake --build --preset linux-release && ctest --preset linux-release
```

If vcpkg downloads are not available, build libtorrent 2.1.2 yourself (CMake
options `-DBUILD_SHARED_LIBS=OFF -Dwebtorrent=OFF -Ddeprecated-functions=OFF`),
install it with Boost, OpenSSL, nlohmann-json and Catch2 3 under one prefix,
and use the `linux-prebuilt-deps` preset with `TC_DEPS_PREFIX` set to that
prefix.

## Tests

- `tc_unit_tests` (Catch2): bencode, metainfo, manifest validation, engine,
  bridge. Test names carry the acceptance IDs from the specification
  (`[F05]`, `[E01]`, `[W03]`, ...).
- `integration_proof`: drives `tc-proof` on generated datasets and checks every
  output with `tools/reference/verify_torrent.py`, an independent
  standard-library Python implementation of BEP 3/47/52 hashing.

Run one group with `ctest --preset <preset> -R integration` or pass Catch2 tags
directly: `tc_unit_tests "[engine]"`.

The `windows-asan` preset instruments native project code with MSVC ASan and
uses the release build's dependencies through `TC_DEPS_PREFIX`. Linux presets
use ASan and UBSan. See [JSON input and Windows ASan checks](json-input-windows-asan.md)
for commands, runtime controls and scope. Fuzz smoke campaigns now use one
requested run per campaign; see [additional input checks](security-input-harnesses.md).

## Windows development packages

After the release build, run `./packaging/windows/build.ps1` from PowerShell 7
in the same MSVC developer environment. It creates unsigned online/offline
installers and an app ZIP under `build/windows-packages`. See
[Windows packaging](windows-packaging.md) for prerequisite integrity, installed
app proof, uninstall behavior and remaining clean-machine release checks.
