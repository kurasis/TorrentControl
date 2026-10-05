# M2 stabilization

This change closes the first implementation package from
[review-and-roadmap.md](review-and-roadmap.md). It fixes the creation-policy,
project-reproducibility, output-cleanup, profile-dialog and verification-worker
failures. M3 diagnostics/editor work and release packaging remain separate.

## Behavior and regression evidence

| Problem | Result | Automated evidence |
| --- | --- | --- |
| Batch bypassed private validation | All three batch modes check the same private settings as single creation before enqueue. Per-item source/output failures remain isolated. Validation and the single-create snapshot are taken under one lock. | Catch2 `single and every batch mode enforce the same private policy`, `authorized private batches use their validated settings snapshot` |
| Saved auto piece size was ignored | A resolved project decision is restored as a fixed piece size. Selecting **Automatic** explicitly requests a fresh decision. Invalid/future policies and contradictory sizes are rejected. Unresolved and legacy projects retain auto selection. | Catch2 `automatic project piece decisions survive reopening and resaving` checks emitted piece size, repeat saving and byte-identical recreated torrents; `project piece policies are validated before restoring`; `unresolved and legacy projects can still select automatic pieces` |
| Actual write/flush/close failures leaked temp files | Cleanup includes the write itself and only removes files successfully created by this process. A zero-length OS write fails instead of looping forever. | Catch2 `an actual partial write failure removes its temporary file`: child-process RLIMIT_FSIZE/EFBIG injection checks new/replaced output, temp cleanup and preservation of old bytes. POSIX-specific injection is skipped on Windows; other output tests run on both platforms. |
| WebView2 disabled the prompt used by Save Profile | A labelled HTML form supports Enter/Escape, trimmed nonempty names, the native 200-byte UTF-8 limit, inline native errors and retry. Pending draft edits are flushed before saving. | Playwright keyboard/Unicode, cancellation/reopening, blank/oversized names and native-error/retry tests; extended native `--self-test` checks profile dialog and reloads the actual settings file. |
| Clipboard rejection used a disabled prompt | A read-only, selected magnet text area supports manual Ctrl+C. Replacing an open dialog ignores the old queued close event. | Playwright `clipboard denial opens a selected read-only magnet without a script prompt`; native self-test checks the manual-copy dialog and selection. |
| Standard exceptions escaped verify workers | Standard and unknown exceptions produce an INTERNAL verification failure, release pause state and allow queued jobs to continue. Cancellation keeps its Cancelled state. | Catch2 `unexpected verification exceptions fail only their job and release the queue`: factory, adapter, allocation and nonstandard exceptions followed by successful verification. Existing cancellation tests remain enabled. |

## Validation

Local cloud validation uses `linux-release`, warnings-as-errors, and the pinned
vcpkg baseline. The suite has **115 CTest entries** (114 Catch2 cases plus the
Python integration proof) and **17 Chromium UI tests**. The integration proof
contains 24 tests; UNC is skipped on Linux. Playwright mocks the bridge and is
not native WebView2 evidence.

The existing [CI workflow](../.github/workflows/ci.yml) separately builds and
runs the C++ tests on Windows (pinned MSVC 14.44) and Linux, and runs the UI suite.
Its Windows `TorrentControl.exe --self-test LOGFILE` now uses a separate
`TorrentControl-self-test-<pid>` data directory beside LOGFILE. It exercises the
HTML profile form through the real bridge, reloads settings to check the saved
profile, removes the test profile and checks selectable manual magnet copying.
Successful logs include `profileDialog`, `profilePersisted` and `magnetDialog`
set to true. Normal application data is untouched by this self-test.

Check the commit's [GitHub Actions run](https://github.com/kurasis/TorrentControl/actions)
for remote results. A passing native self-test is an automated smoke check;
manual file-picker, accessibility, scaling, clean-machine and performance
acceptance remain part of the later Windows/release validation package.

## Remaining work

The review's responsiveness/pagination, untrusted-metainfo hardening, export
privacy, settings-error notification, minimum WebView2 version and licensing
items remain tracked. This package does not claim M3/M4 completion.
