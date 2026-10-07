# JSON input limits and Windows AddressSanitizer

Bridge messages, saved projects and settings now reject JSON containing more
than **512 simultaneously open arrays/objects**, including the root. A linear,
nonrecursive scan runs before constructing the JSON DOM, and respects quoted
strings and backslash escapes. The JSON library still validates syntax, numbers
and UTF-8. The existing byte limits remain 1 MiB, 64 MiB and 16 MiB respectively.

The bridge returns `MESSAGE_TOO_DEEP` without dispatching a handler. Its request
ID is null because extracting an ID must not require parsing rejected input.
A queue refusal likewise preserves its original error code and uses a null ID
for over-deep input. Origin checks run first. Project/settings readers raise
`CoreError(ResourceLimit)`, consistent with their existing byte-limit failures;
ordinary malformed settings still fall back to defaults.

Regression tests exercise the exact depth boundary, 100,000 nested containers
within the byte limit, escaped strings, malformed syntax/UTF-8/numeric overflow,
queue refusal, and a subsequent healthy request. A 128-level tagged bencode
dictionary round trip verifies that the limit accommodates the metadata
editor's existing maximum depth and JSON wrappers. This closes an unbounded
recursive-DOM input path; it is not a claim of exhaustive input security.

[Local evidence](evidence/json-input-security/linux-local.json) records
174/174 Linux release tests and 175/175 Clang 19.1.7 ASan/UBSan tests, both
runtime controls, and the stock fixture's 12 imports/48 rechecks with exact
mount verification and cleanup. The known BiglyBT gate remains open. Windows
results are established separately by CI, not by this Linux report.

## Windows instrumentation

`windows-asan` builds native project core/service/bridge code, developer tools
and tests using pinned MSVC 14.44 with `/fsanitize=address`, RelWithDebInfo and
`/INCREMENTAL:NO`. It reuses release dependencies through `TC_DEPS_PREFIX` and
the pinned vcpkg toolchain's package wrappers, with manifest installation
disabled. This preserves static-library discovery (including zlib's `zs.lib`)
without rebuilding or instrumenting dependencies.
The prebuilt dependencies and WebView2 host are **not instrumented**. Windows
UBSan and leak checking are not claimed. Regular Windows GUI, DPAPI, dialog,
SMB and process-memory tests continue using the ordinary release build.
MSVC STL vector/string annotations are disabled to match the prebuilt libraries
and avoid incompatible inline definitions (`LNK2038`). ASan still checks heap
allocation boundaries and freed memory, but cannot detect access past a
container's logical size when it stays within allocated capacity. See Microsoft's
[container annotation requirements](https://learn.microsoft.com/en-us/cpp/sanitizers/error-container-overflow).

```powershell
# After building windows-x64-release, in the same MSVC developer prompt:
$env:TC_DEPS_PREFIX = "$PWD/build/windows-x64-release/vcpkg_installed/x64-windows-static-md"
cmake --preset windows-asan -DTC_REQUIRE_PINNED_TOOLCHAIN=ON
cmake --build --preset windows-asan --parallel 4
$env:ASAN_OPTIONS = "halt_on_error=1"
ctest --preset windows-asan --parallel 4
```

Both Windows ASan and Linux ASan/UBSan suites include two negative runtime
controls. A developer-only executable intentionally performs a heap overrun
and a read after free in separate subprocesses. The harness requires a nonzero
exit and the corresponding AddressSanitizer diagnosis, preventing a missing
runtime or uninstrumented probe from passing. Intentional diagnoses belong to
these controls; the application tests must complete without sanitizer errors.
CTest results, control reports and logs are retained in CI artifacts
`windows-asan-evidence` and `linux-fuzz-sanitizer-evidence`.

## Campaign pause and fixture isolation

Fuzz campaigns are currently paused at the user's request. CI still builds
the existing drivers and runs deterministic sanitizer tests, but skips the
campaign step by default. An explicit manual workflow dispatch input
`run_fuzz_campaigns=true` re-enables it; the runner remains at three requested
executions per campaign. No new fuzz target or campaign is part of this package.

The stock-client Docker fixture now mounts only its two adapter files instead
of the complete repository. It verifies the exact bind mounts with Docker
inspect and records `repositoryMounted: false`. The repository's Git metadata
and checkout credentials are therefore outside the client mounts. Existing
network isolation, payload integrity and known BiglyBT compatibility gate
checks remain required.
