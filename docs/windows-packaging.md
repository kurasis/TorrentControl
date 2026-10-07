# Windows development installers and offline prerequisite

M4 now has an application-only CMake install component, a pinned packaging
script and current-user Inno Setup installers. These are **unsigned development
packages**; they do not close release acceptance or select the application license.

## Products and installation

`packaging/windows/build.ps1` produces:

- `TorrentControl-<version>-dev-unsigned-windows-x64-online-setup.exe`: includes
  Microsoft's small Evergreen bootstrapper; internet is needed if the Runtime
  must be installed or updated.
- `TorrentControl-<version>-dev-unsigned-windows-x64-offline-setup.exe`: includes
  Microsoft's complete x64 standalone Evergreen Runtime installer (about 203 MiB
  at this pin). The prerequisite payload is embedded, not fetched by Setup.
- `TorrentControl-<version>-dev-unsigned-windows-x64.zip`: app and app-local CRT,
  requiring an already installed compatible Runtime. It is not self-contained.
- `checksums.json`: exact SHA-256 and sizes for those products.

The application installs under `%LOCALAPPDATA%\Programs\TorrentControl`, with
current-user Start menu shortcuts and an HKCU uninstall entry. Setup requires
x64 Windows with build 19045 or newer. This is a packaging eligibility check,
not an OS certification. No file association is claimed or overwritten.

MSVC release redistributable DLLs are copied from the pinned toolchain's x64
`Microsoft.VC143.CRT` directory and their Microsoft signatures verified. No
Python, IDE, vcpkg or developer proof tool is included in the app payload.
The frontend and actual dependency copyright texts are bundled, alongside
third-party notices, development status and a source/file-hash manifest.
The manifest's inventory excludes itself. This inventory is not a complete SBOM.

A stable installation mutex is held by every native host instance. Setup and
Uninstall refuse while the application is alive in that Windows session;
they never ask Restart Manager to terminate or restart hashing jobs. Other
sessions, sudden power loss and arbitrary filesystem failures are not certified.
Uninstall removes application files/shortcuts/registration, preserves
`%LOCALAPPDATA%\TorrentControl` user data and leaves shared WebView2 alone.

## Prerequisite integrity and behavior

`packaging/windows/prerequisites.json` pins Inno Setup 6.4.3 against its official
GitHub release asset SHA-256. Bootstrapper and standalone pins were computed
over official Microsoft HTTPS downloads; Windows additionally requires valid
Microsoft Authenticode signatures before packaging. Immutable resolved Microsoft
URLs are recorded, so a changing fwlink cannot silently change build inputs.
Updating Evergreen payload pins requires a reviewed refresh of hashes and signatures.

Setup reads Microsoft's documented Runtime `pv` registry keys and checks the
minimum from `src/app/runtime_policy.hpp`. A compatible existing Runtime skips
prerequisite execution. Otherwise Setup extracts its embedded prerequisite,
checks its compiled SHA-256, runs `/silent /install` and checks registry
compatibility again **before copying the application**. Failed launch, bad hash,
vendor failure or an absent/old Runtime aborts installation. Vendor exit 3010
is recognized as requiring reboot; `/NORESTART` prevents automatic restart.
The installed host still uses the real WebView2 loader's version check.

Microsoft's installers choose per-user/per-machine behavior according to
their execution context and existing Edge Updater. In particular, an existing
machine Updater may promote a Runtime installation; current-user application
installation does not promise current-user-only Runtime deployment. Microsoft's
Runtime terms and distribution policy apply; links are included in the package.

For an explicit prerequisite repair, use `/INSTALLRUNTIME=1`. It always runs
the same pinned, embedded official prerequisite, never an arbitrary supplied
executable. Ordinary installs avoid unnecessary Runtime changes.

## Build and CI proof

From the pinned MSVC 14.44 developer environment with PowerShell 7:

```powershell
cmake --preset windows-x64-release -DTC_REQUIRE_PINNED_TOOLCHAIN=ON
cmake --build --preset windows-x64-release
./packaging/windows/build.ps1
```

Use a fresh output directory for each packaging run. The default output is
`build/windows-packages`; prerequisites/tools remain there and are excluded
from product archives, except the selected embedded Runtime installer.

`tests/windows/run-packaging.ps1 -DisposableRunner` is restricted to disposable
GitHub Windows runners. It verifies:

- Online installation into the real default path, staged-file hashes, HKCU
  registration and actual installed WebView2/bridge self-test.
- A sanitized app PATH without toolchain/Python/vcpkg paths and observed
  **app-local** MSVC CRT module paths.
- Setup and Uninstall refusal while a real GUI process remains alive.
- Offline reinstall/repair restoring a deliberately damaged frontend file,
  application uninstall and preservation of a user-data sentinel.
- Explicit execution and verification of the genuine embedded standalone
  Microsoft installer, then the installed app and uninstall again.
- Three disposable controls: corrupt prerequisite hash, a vendor failure,
  and a successful process that does not provide the required Runtime. Each
  must fail without installing/registering the app. Test-only probe executables
  and fixture installers are excluded from product artifacts.

CI uploads products, checksums, manifest and proof logs/summary as
`TorrentControl-development-packages-windows-x64`, with seven-day retention.
This proof uses Windows Server 2022 with an existing Runtime and installed
build tools. It does **not** establish a cold, disconnected desktop installation.

## Remaining release gates

Clean Windows 10 22H2/Windows 11 without IDE/Python/vcpkg, missing/old Runtime,
fully disconnected prerequisite installation, multi-session upgrades,
keyboard/accessibility/DPI checks, the known BiglyBT compatibility gate,
application license selection, final dependency/SBOM review, publisher signing
and an actual release acceptance report remain open. Microsoft Runtime/OS
support must be rechecked when releasing. No stable release/tag is published
by this packaging workflow.
