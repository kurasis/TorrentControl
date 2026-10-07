# End-user security audit — 2026-10-07

Scope: source at `4304c8c` and the changes accompanying this report; the Win32
host, WebView2/IPC, persistence, metainfo/payload handling, network diagnostics,
vcpkg dependencies, development packaging and CI. This is a focused source and
boundary review, not a certification or a complete audit of third-party source.

Production code and test/build entry points were read before execution. Dynamic
checks use the remote cloud environment with owned temporary fixtures and fake
host services; installation tests run only on disposable GitHub Windows runners.
No scenario, installer or malicious command was executed on a user's desktop.
Uploaded specifications were treated as project reference material, not as new
authorization. The owner separately approved the behavior and installer changes
described below. No unfinished user changes were present at the start.

## Confirmed findings and corrections

Priorities: P1 = high impact requiring prompt correction; P2 = medium impact
with narrower prerequisites. User interaction and attacker access are part of
each rating; these are not unauthenticated remote-server vulnerabilities.

### S01 — File-association launch can execute a scenario (P1 / high)

- Location: `src/bridge/src/app_operations.cpp`, `openInClient`, and
  `src/app/windows/host_services.cpp`, `open_with_default_app` / `ShellExecuteExW`.
- Before: any native-owned imported/result path was passed to Windows' `open`
  verb. The file dialog also offers an All files filter. Valid bencoded metainfo
  does not imply a safe filename extension; a comment can contain scenario text.
- Conditions/damage: a crafted file named `.cmd`/`.bat`, or a result saved under
  such a name, followed by the user's Open in client action can invoke a Windows
  scenario association with the user's file/network permissions. A changed file
  between import and launch is another route. The unrestricted call was
  reproduced using a fake host, not by executing commands. Windows scenario
  execution and any polyglot payload still require an isolated Windows check.
- Minimal correction, approved: require case-insensitive `.torrent` in both the
  bridge and native launch boundary. Viewing/editing files with other extensions
  remains available; trying to launch one returns `UNSUPPORTED_FILE_TYPE`.
- Test: six refused extensions and lower/uppercase `.torrent` through the real
  dispatcher, without invoking Windows ShellExecute.

### S02 — Imported projects restore destructive/costly consent (P1 / high)

- Location: `src/service/src/draft.cpp`, `draft_from_json`; `storage.cpp`,
  `load_project`; `app_service.cpp`, `AppService::load_project` / `start_create`.
- Before: `replaceExisting`, `allowHydration` and `acceptLargeResourceUse` were
  restored from a project as though the current user had just accepted them.
- Conditions/damage: opening a supplied project and clicking Create can replace
  an existing writable output file selected by that project, hydrate cloud data
  or accept otherwise gated memory consumption. The output is visible in the UI,
  but its saved boolean is not a fresh overwrite authorization. Replacement of
  an unrelated sentinel file was reproduced inside an owned temporary fixture.
- Minimal correction, approved: reset these three flags when the application
  imports a project. Preserve sources, destination, metadata and the file format.
  The low-level project serialization API still round-trips the saved data.
- Test: import through the bridge, check all flags, attempt Create and assert
  that existing bytes survive. Source-path trust is a separate open issue (R01).

### S03 — Percent-encoding bypasses passkey masking (P2 / medium)

- Location: `src/service/src/profiles.cpp`, `passkey_like` / `redact_url`;
  `storage.cpp`, `save_settings` / `export_profile`.
- Before: path-token recognition counted characters in the encoded segment.
  Encoding an otherwise recognized key as `%61%31...` made it appear harmless.
- Conditions/damage: a user saves or shares such a tracker URL. The private key
  appears in a supposedly redacted profile/preview and stays outside DPAPI
  protection in Windows settings. It can authorize access to the user's tracker
  account or disclose activity. Both export and storage bypasses were reproduced.
- Minimal correction: decode valid percent triplets only for classification;
  redact the original segment and preserve the original operational URL exactly.
  This does not make heuristic token recognition complete; see R03.
- Test: encoded key masking, protected persistence, exact URL round-trip,
  explicit secret-inclusive export, ordinary encoded paths and malformed escapes.

### S04 — POSIX service files inherit permissive access (P2 / medium)

- Location: `src/service/src/storage.cpp`, `write_file_atomic`.
- Before: an `ofstream` created settings/projects/export temp files using the
  process umask. With `0022`, files were readable by other local accounts.
- Conditions/damage: Linux headless tooling saves a project or settings in a
  directory traversable by another account. Project credentials/paths, and the
  reversible `plain:` test-platform secret representation, become readable.
  This is a confirmed POSIX tooling issue; it is not a Windows DPAPI bypass.
- Minimal correction: exclusive creation of an owned temporary descriptor,
  POSIX mode `0600`, non-inheritable handles, and reuse of the existing 64-bit
  temporary-name helper. Continue replacing the destination by rename.
  Windows uses `_wsopen_s` with exclusive creation and denied sharing; ACLs still
  inherit from the user-selected folder, not from the POSIX permission bits.
- Test: a permissive umask, binary and empty writes, replacing a destination
  symlink without changing its target, failed directory replacement and cleanup.
- Additional hardening: the former truncating open could follow an existing
  temporary-path link. Exclusive creation removes that primitive. A practical
  prediction/race exploit against its random name was not demonstrated; access
  by a hostile writer to the destination directory remains outside this guarantee.

### S05 — Payload verification follows links outside the selected root (P2 / medium)

- Location: `src/core/src/verify.cpp`, `map_to_root`; `file_payload_source.cpp`,
  `PosixSource::open` / `Win32Source::open`.
- Before: safe lexical metainfo components were appended to the root, but the
  filesystem could resolve a component to a symlink/junction outside it.
- Conditions/damage: an attacker can place a link in the selected payload tree.
  Verification reads a file outside the chosen folder. A 23-byte owned sibling
  fixture was actually read and verified on Linux. No content upload or file
  modification occurred; this alone is not proof of arbitrary data exfiltration.
- Minimal correction, approved: resolve the root and each mapped path, compare
  path components (case-insensitive on Windows), reject external targets and use
  the resolved internal path. Missing files retain normal verification handling;
  internal links and a root with a trailing separator remain supported.
- Test: refuse an external file before opening a reader and verify an internal
  link. On platforms without permission to create symlinks that test is skipped.
  This is a mapping-time check, not a handle-level confinement guarantee (R02).

### S06 — Installer engine lacks later security hardening (P2 / conditional)

- Location: `packaging/windows/prerequisites.json`, `innoSetup`; `build.ps1`,
  compiler installation; resulting Setup/Uninstall executables.
- Upstream evidence: Inno Setup 6.6 fixes a theoretical TOCTOU issue when deleting
  a pre-existing uninstaller temporary directory and adds CSPRNG temporary names.
  6.7 adds RedirectionGuard. The former pin was 6.4.3. The vendor explicitly says
  it does not know a practical exploit of that race, which requires untrusted
  write access to the temporary directory. This is an upstream-confirmed flaw,
  not a demonstrated privilege escalation in TorrentControl's per-user installer.
- Minimal correction, approved: pin 6.7.3 within the same major line. The official
  EXE was downloaded without running it; SHA-256
  `9c73c3bae7ed48d44112a0f48e66742c00090bdb5bef71d9d3c056c66e97b732`
  matches the official GitHub release asset digest. Do not jump to major 7 as part
  of this focused change. Installation, repair, negative prerequisite controls
  and uninstall must pass on disposable Windows runners before merging.

## Remaining risks and recommendations

These are separate from the corrected findings. Conditional scenarios need the
stated attacker access or user action; absence of a reproduction is explicit.

| ID / priority | Location and evidence | Conditions and possible damage | Minimal follow-up |
| --- | --- | --- | --- |
| R01 / P2 | `app_service.cpp`, `load_project` → `bump_locked(true)` / scan worker; `draft.cpp`, restored source paths | Opening an untrusted project starts scanning its absolute paths. UNC sources can contact an SMB server and potentially disclose Windows authentication material; native dialog selection of the project is not selection of every source. Windows credential emission was not tested. | Add a native trust preview before installing/scanning restored sources; require fresh confirmation of UNC/external sources. This changes import UX and needs a separate design/approval. |
| R02 / P2 | `verify.cpp`, mapping/open gap; `manifest.cpp`, scanner/follow-link checks; `file_payload_source.cpp`, native opens | A process able to replace resolved parent directories concurrently can race canonical checks and later reads. On POSIX, `output.cpp::move_into_place` also has an explicitly weaker check-then-rename fallback on filesystems without hard links. No adversarial race was demonstrated. | Validate opened handles against the authorized root/identity; use directory-relative, no-follow operations where possible. Exercise parent replacement, junctions and no-hard-link filesystems on isolated fixtures. |
| R03 / P2 | `profiles.cpp::passkey_like`, `storage.cpp::save_settings/export_profile/project_json` | The remaining heuristic intentionally ignores short or all-letter path tokens. Such secret formats can remain unprotected/unredacted. Projects and final torrents can contain operational credentials in clear text by design. DPAPI cannot protect against malware already running as the same Windows user. | Define explicit secret/private-token metadata or encrypt every custom tracker URL in local settings; add a redacted project-sharing mode and a visible sharing warning. Avoid presenting heuristics as universal secret detection. |
| R04 / P1 before stable release | `build.ps1` manifest `unsigned=true`; release artifacts/checksum publication | Replacing both an unsigned installer and its adjacent checksum defeats that checksum as an authenticity check. A compromised publishing account or distribution mirror can supply executable code. No compromised release was observed. | Authenticode-sign app and installers with a protected publisher key and timestamp; establish independent release provenance/attestations. Requires the owner's signing identity/key infrastructure. |
| R05 / P2 | `runtime_policy.hpp` compatibility floor; `main.cpp` Runtime selection; prerequisite manifest | The SDK version and compatibility floor do not identify the actual browser engine or guarantee its security patch level. Offline/external Fixed Runtime deployments can stay vulnerable after new browser fixes. Current Microsoft notes also mention pending Chromium fixes. | Record installed/bundled Runtime versions in evidence, define a patch/update policy and review Microsoft's security releases. Do not claim that an Evergreen minimum is a security baseline. |
| R06 / P2 | `network_probe.cpp::probe_endpoint`, `parse_probe_url`; `app_operations.cpp::startDiagnostics` | Explicit endpoint diagnostics can contact LAN/loopback addresses or a malicious host that resolves privately, with a GET/UDP-connect and optional URL credentials. Same-origin redirect policy is not DNS-rebinding protection. There is no remotely exposed local API; the UI warns that checks contact endpoints. | Preview destination/protocol and resolved scope; require separate consent for private addresses where appropriate. Keep legitimate LAN diagnostics usable. Test rebinding and credential forwarding in a network-isolated lab. |
| R07 / P2 | `.github/workflows/ci.yml`, Actions refs such as `@v4` / `@v1`; Go/Python/browser developer dependencies | Mutable action tags and build tools can execute on runners; compromised upstream tags can alter produced binaries. vcpkg/source checksums do not protect unrelated Actions. No suspicious production install script was found. The full transitive developer graph has not received a reachability audit. | Pin Actions to reviewed commit SHAs, keep token permissions minimal, produce an SBOM and scan the resolved production and developer graphs separately. |
| R08 / P2 | `webview_host.cpp` default environment options / user-data profile; `frontend/index.html` CSP | App-specific analytics or browser-profile scraping were not found. Microsoft's official privacy documentation says WebView2 collects required and optional diagnostics; the Windows Diagnostics & feedback setting governs collection, rather than an app-specific consent dialog. Traffic/content was not captured here. `frame-ancestors` in a meta CSP is not enforced as an HTTP header would be. | Explain vendor traffic and Windows privacy controls in the product; verify crash/diagnostic/update traffic in an isolated Windows VM. Keep native origin/navigation checks; use a header-enforced embedding policy if embedding becomes possible. |
| R09 / P3 | `cmake/CompilerWarnings.cmake`; prior Windows PE inspection | Existing x64 app artifact has DEP, ASLR and high-entropy ASLR; Control Flow Guard flag is absent. This is defense-in-depth, not a demonstrated exploit. | Evaluate `/guard:cf` and `/sdl` with the pinned native dependency/toolchain combination before adopting them. |

## Checks that already constrain the attack surface

- No application command-shell construction, `system`, PowerShell execution or
  arbitrary process runner was found. Native launches are file associations and
  constrained HTTP(S) links; user strings are not shell command lines. Developer
  process drivers are not installed with the app.
- Payload is read/hashed, not downloaded, unpacked or executed. Metainfo path
  components reject traversal, separators, device names and controls. Source
  scanning skips links/reparse points by default, and opted-in links must stay
  under the scanned root. Source identities and output/source conflicts are
  checked; creation uses exclusive temporary files and explicit replacement.
- Inno installs per-user (`PrivilegesRequired=lowest`) under LocalAppData, has
  no application autorun/service registration or broad user-data deletion rule,
  and refuses installation/uninstallation while the app mutex exists. The app
  has no explicit request for administrative elevation. The manifest does not
  explicitly declare `requestedExecutionLevel`; that is a hardening recommendation.
  Microsoft's Runtime installer/updater is a separate vendor component whose
  machine-wide behavior requires isolated observation.
- Bundled Runtime downloads use HTTPS and pinned SHA-256; packaging verifies
  Microsoft's valid Authenticode signer, and setup checks its embedded hash
  before executing the extracted vendor prerequisite. A failed/missing Runtime
  aborts before application files are installed. Application auto-update is not
  implemented: `info.update-url` is torrent metadata and catalog updates fetch
  reviewed text data, not application code. Catalog checksums identify contents,
  not the publisher's signature.
- curl restricts application HTTP requests to HTTP(S), verifies peer and host,
  disables automatic redirects and bounds time, headers and retained bodies.
  Manual redirects require the original origin/userinfo. Diagnostics send no
  actual swarm hash; opening a torrent does not start diagnostic requests.
  No production listening HTTP API or peer-listening session was found.
- WebView2 serves bundled assets at a fixed virtual origin. Host objects are
  disabled; navigation/new windows, permissions and downloads are denied.
  Release devtools are disabled. CSP disallows network connections and dynamic
  code; `dom.js` renders untrusted strings as text. The dispatcher checks origin,
  request schema/revision and message/depth limits; queue and response budgets
  are bounded. Attached source paths come from native file objects, not JSON.
- Clipboard access is a user-triggered magnet **write**. No clipboard read,
  microphone/camera API, browser document/cookie harvesting or app analytics
  endpoint was found. Access to user files follows selection/restored projects,
  with the important project exception R01.
- Recognized tracker secrets use current-user DPAPI on Windows. Diagnostic
  errors use categories/origin labels instead of response bodies or raw
  authenticated URLs. The committed TLS private key is an intentional local
  untrusted-certificate test fixture, not a production credential. No production
  API token/password/signing key was identified by the targeted source search;
  this is not a complete historical secret scan.

## Official dependency information

Retrieved over verified HTTPS on 2026-10-07. Used native versions come from the
pinned vcpkg baseline and installed package status, not guessed latest releases.
Windows curl uses Schannel; Linux checks use OpenSSL. libtorrent is
**arvidn/libtorrent-rasterbar**, not rakshasa/libtorrent. No broad package-name
CVE match was treated as a finding without checking the affected product/range.

| Used version / role | Official evidence and interpretation |
| --- | --- |
| curl 8.22.0#1, production | [Version-specific vulnerability list](https://curl.se/docs/vuln-8.22.0.html) reports zero published vulnerabilities for that version at retrieval. This does not establish absence of undisclosed bugs. |
| OpenSSL 3.6.5, production crypto / Linux TLS | [3.6 advisories](https://openssl-library.org/news/vulnerabilities-3.6/) list fixes in 3.6.5, including September 29 issues such as CVE-2026-35189 and CVE-2026-42772. Those ranges say **before 3.6.5**; do not incorrectly label the pinned patched version affected. Unused QUIC/CMP/server features were not dynamically exercised. |
| c-ares 1.34.8, production DNS | [Vendor advisories](https://github.com/c-ares/c-ares/security/advisories) include CVE-2026-69184, CVE-2026-69186 and CVE-2026-33630 fixed in 1.34.7. The latter structured range is misleadingly open-ended; its description explicitly states the fix in 1.34.7. Used 1.34.8 is later than that fix. Older UAF advisories also have earlier fixed versions. |
| zlib 1.3.2#2, production transitive | [Vendor release/security notes](https://zlib.net/) describe audit fixes in 1.3.2. [Published repository advisories](https://github.com/madler/zlib/security/advisories) returned no records; this is not an exhaustive CVE database result. |
| libtorrent 2.1.2; nlohmann-json 3.12.0#2; Boost 1.92.0; WIL 1.0.260126.7 | [libtorrent](https://github.com/arvidn/libtorrent/security/advisories), [JSON](https://github.com/nlohmann/json/security/advisories), [Boost](https://github.com/boostorg/boost/security/advisories) and [WIL](https://github.com/microsoft/wil/security/advisories) published-repository feeds returned no records. An empty feed is incomplete evidence; no claim of comprehensive vulnerability coverage or upstream-source audit is made. WIL is header-only. |
| WebView2 SDK 1.0.4191.47, production loader/API | [SDK notes](https://learn.microsoft.com/en-us/microsoft-edge/webview2/release-notes) and [Edge security notes](https://learn.microsoft.com/en-us/deployedge/microsoft-edge-relnotes-security) were checked. October 5 notes identify Edge 154.0.4258.62; October 6 says a Chromium fix is pending. Neither determines the actual Runtime on a user's PC. The vendor installer resource version is also not its contained browser-engine version. See R05. |
| Inno Setup 6.4.3 → 6.7.3, build and shipped installer engine | [Official history](https://jrsoftware.org/files/is6-whatsnew.htm), sections 6.6.0/6.7.0, supplies the S06 evidence. [Official 6.7.3 release](https://github.com/jrsoftware/issrc/releases/tag/is-6_7_3) supplies the SHA-256 pin. |
| Playwright 1.56.1, developer only | [CVE-2025-59288 / GHSA-7mvr-c777-76hp](https://github.com/advisories/GHSA-7mvr-c777-76hp) affects versions **below 1.55.1**, not 1.56.1. GitHub's exact-version `playwright@1.56.1` advisory query returned no matching records. Browser binaries are separate dependencies. |
| Catch2 3.16.0 and psutil 7.0.0, developer only | Catch2's [vendor feed](https://github.com/catchorg/Catch2/security/advisories) and GitHub's exact-version PyPI advisory query for psutil returned no records. The Go compatibility tool and stock-client/container tools are not distributed; their entire transitive graph was not security-audited here. |

Other references: [Windows ShellExecute structure/verbs](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-shellexecuteinfow),
[WebView2 privacy](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/data-privacy).
The obsolete c-ares advisory URL and an Edge stable-notes URL returned 404;
official vendor advisory/security feeds above were used instead. Access was
available, but it did not provide a complete product SBOM, an installed Runtime
inventory for end-user machines, or a full Windows OS/CRT vulnerability assessment.

## Validation and outstanding isolated checks

- Baseline before corrections: **176 CTest tests passed**. Five new targeted
  cases reproduced the four original corrected code defects and the external
  payload-link issue before their respective fixes. Dangerous association calls
  were observed through a fake host; all writes/reads used owned temporary data.
- Local release build with warnings as errors and **181 CTest tests passed**,
  including all five new regression cases. The security-tag subset passed
  **96 assertions in 9 cases**. **182 ASan/UBSan CTest checks passed** with leak
  detection, including the sanitizer runtime controls. Native prebuilt libraries
  are not fully instrumented; project code and compiled inline code are checked.
  CI additionally executes Windows builds, ASan, native WebView2 flows,
  packaging controls and Windows 11 ARM x64-emulation checks; their results and
  downloadable evidence are attached to this change's pull request.
  The browser suite passed **57 tests**. There is no configured standalone
  JavaScript type checker or linter; native warning-as-error builds are used.
- Still needed in disposable VMs: command-association/polyglot execution using
  a harmless sentinel, Windows junction and concurrent parent-replacement races,
  NTLM/UNC and DNS-rebinding captures, vendor Runtime/updater privacy behavior,
  a fresh disconnected Win10/Win11 installation, long NAS stalls and filesystem
  atomicity where hard links are unavailable. Windows hosted images with an
  existing Runtime do not certify a clean desktop or offline first install.

No dependencies, application features, user files or historical artifacts were
removed as "unused". Public C++ signatures, IPC version and project format are
preserved. Approved safeguards intentionally reject executable launch extensions,
restore consent gates and confine existing payload links. This audit does not
justify the conclusion that the application is fully safe.
