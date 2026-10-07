# Code audit and focused maintenance — 2026-10-07

Baseline: `eb90d4c2ab82de0a439ddaa1fd8d12432b34ea6f` (`main`). The worktree was
clean before this audit. No existing user edits were overwritten.

## Scope and baseline

Reviewed repository/build instructions, CMake presets, pinned dependency and
CI configuration, the native/service/bridge/frontend boundaries, persistence
and JSON validation, command dispatch, frontend paging and dynamic self-test
loading. This is a focused maintenance audit, not a claim of exhaustive
security or release acceptance.

The project uses C++20 and libtorrent 2.1.2 with native Windows/WebView2 hosting;
Linux runs its headless core, service, bridge and developer tools. JavaScript
is bundled directly; Playwright runs the real frontend against a native mock.

Initial local checks could not find CMake in `PATH` or Playwright's downloaded
Chromium. Existing tools were reused via `/workspace/setup-tools/bin` and
`TC_CHROMIUM=/usr/bin/chromium`, without changing project dependencies. With
that setup, the untouched baseline passed **174 CTest entries** and **54 UI
tests**. A dependency configuration emitted a nonfatal CMake CMP0167 developer
warning; no baseline application/test failures were observed.

## Confirmed findings and changes

P2 denotes a reproducible correctness/resource-lifecycle defect; P3 denotes
maintenance or documentation work.

| Priority | File | Cause and change |
| --- | --- | --- |
| P2 | [storage.cpp](../src/service/src/storage.cpp), `apply_settings_patch` | Range checking after `json.get<int>()` allowed `4294967297`, `4294967304` and `-4294967295` to wrap into 1 or 8. Compare the original JSON integer against 1–8 before narrowing. Keep all-or-nothing updates and existing valid settings. |
| P2 | [storage.cpp](../src/service/src/storage.cpp), `read_small_file` | A stream's `badbit` was ignored, returning empty/partial data after an IO failure. Reject read errors with `SourceUnreadable`; retain empty-file, EOF, missing-file and size-limit behavior. Reading a directory reproduced the silent failure on Linux. |
| P2 | [bridge.js](../frontend/bridge.js), `send` | Registering before serialization and leaving entries after a native send exception retained failed requests. Serialize before registering; remove an outstanding entry when either native sending method throws and reject with the original error. Preserve exports, envelope, IDs and successful response handling. |
| P3 | [tests/CMakeLists.txt](../tests/CMakeLists.txt) | Two adjacent `TC_BUILD_TOOLS` conditionals duplicated the same guard. Keep both integration registrations in one block. |
| P3 | [architecture.md](architecture.md) | Diagnostics were still described as pending, and bridge service commands as running on the UI thread. Describe the implemented diagnostics, ordered worker and UI-only callback marshalling. |
| P3 | [model-pagination.md](model-pagination.md), [process-memory-smb.md](process-memory-smb.md), [long-session-memory.md](long-session-memory.md) | Current-limit statements still said three fuzz runs after the budget was reduced. Point to the current `--runs=1` contract; historical original-budget statements remain intact. |

Before the implementation changes, the new native regression cases failed
(seven assertions across two cases) and both ordinary/attached-file bridge
failure tests failed. These failures were reproduced independently of the
otherwise passing baseline suite.

## Deletion and dependency decisions

Removed only the redundant CMake guard and obsolete descriptions. No tracked
temporary archives, binaries, logs or backup files were found. Existing ignored
build/evidence outputs were retained.

No dependency or runtime module was removed. WIL is used by the native host's
COM/handle ownership; curl and c-ares are used directly by network diagnostics.
Catch2 and Playwright are developer test dependencies. The independent Go/Java
client adapters are referenced by integration tooling. Frontend self-test
modules are imported dynamically by `app.js` and used by Windows CI, so a
missing static import is not evidence that they are unused.

## Validation

| Check | Result |
| --- | --- |
| Linux release build, compiler warnings as errors | Passed |
| Linux release CTest, including existing reference/network integration | **176/176 passed** |
| Chromium UI suite, including three new bridge cases | **57/57 passed** |
| Linux ASan/UBSan CTest with leak detection and runtime negative controls | **177/177 passed** |
| Four fuzz targets × three seeds, one requested run per campaign | **12/12 passed**, 180 processed initial units, no crash artifacts |
| JavaScript syntax and `git diff --check` | Passed |

The repository has no configured standalone C++/JS linter or JavaScript type
checker. C++ compilation checks types and the configured warning policy;
JavaScript syntax checks are not described as type checking. Dependencies
remain prebuilt and outside project-code sanitizer instrumentation. The
one-run fuzz budget primarily replays the initial corpus and is not extensive
mutation coverage.

Native MSVC/ASan, installed packages, Windows 11 ARM64 x64 emulation and actual
WebView2 flows require the existing GitHub Actions jobs; local Linux/browser
checks do not substitute for them. The change's PR checks provide that evidence.

## Remaining issues

- **P3 — CMake dependency configuration:** CMP0167 remains an upstream/policy
  compatibility warning from the pinned libtorrent/Boost configuration. This
  audit leaves the global policy and minimum CMake version unchanged.
- The known [BiglyBT pure-v2 leading-empty-file gate](stock-client-imports.md)
  and [release acceptance gates](windows-packaging.md#remaining-release-gates)
  remain open. This audit does not modify torrent layout/business rules,
  choose the application's license, sign packages or certify clean/offline
  desktop installation.
