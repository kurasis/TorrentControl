# Windows / WebView2 validation

This workflow validates M2 stabilization and the
[M3 metadata editor](m3-metadata-editor.md). `--self-test-flow` uses the production
Windows host, bundled frontend, bridge, service, filesystem and hashing engine.
The only dialog automation is a timer which presses the real common item
dialog's OK button after setting a native-owned fixture name and folder.
It checks that the window was shown and that Windows returned the expected
path; it does not substitute a `HostServices` mock or bypass `GetResult`.

## Scenarios

1. HTML profile dialog, settings reload and selected manual magnet text (the
   existing M2 native checks).
2. Cancel an actual file picker and confirm that the draft revision is intact.
3. Select a Unicode file, then a Unicode directory containing two real files.
4. Choose an output, create, reopen and verify each of v1, v2 and hybrid. Jobs
   reach the page via actual native events. Reopened infohashes must match.
   Selecting the parent folder instead of the payload directory must produce
   `PAYLOAD_MISMATCH` and two missing-file results; the correct folder must pass.
5. Save and reopen a project through Windows dialogs. Confirm its resolved
   piece size and recreate a byte-for-byte identical hybrid torrent.
6. Save a magnet through the Windows dialog and compare the actual file.
7. Save Russian/dark/advanced settings, crash the renderer using CDP
   `Page.crash`, and require one actual `ProcessFailed` recovery. The new page
   restores draft, completed jobs, appearance, opened torrent and reviewed editor
   candidate from the native snapshot;
   it must not replay creation. A normal page reload cannot pass this check.
8. Start a fresh application process with the isolated settings directory and
   verify that the saved appearance survived the process restart.
9. Edit hybrid comment and source through actual HTML controls and native
   Save As dialogs. Check raw info preservation for the outer edit, both new
   identifiers for the info edit, and unchanged payload hashes and piece layers.
10. Verify all six actual output torrents with the independent Python BEP
   3/47/52 implementation, against the files picked through Windows.
11. Select a nonexistent fixed WebView2 runtime using a process environment
    override and require the pre-UI missing-runtime exit code 3.

Normal launches register none of the self-test bridge operations. Self-tests
use a separate data directory; `--self-test-data` is ignored without a
self-test switch. CI saves logs, JSON results, payload fixtures, output files
and isolated settings as `windows-native-evidence`; browser caches are omitted.

## Recovery fix

A native dialog runs a nested message loop. A navigation or renderer failure
can occur while an old page's request is being dispatched. A recovered page
starts numbering requests again, so an old response could satisfy a new
request with the same ID. The host now records a page generation on each
request, discards pending requests when the page changes and sends a response
only to the generation that requested it. A failed recovery reload is also
reported as a fatal host error.

The native test also exposed a Runtime selection mismatch: asking the loader
for the version with a null folder reported the installed Evergreen Runtime
even when a missing fixed Runtime folder was configured in the environment.
The host now reads `WEBVIEW2_BROWSER_EXECUTABLE_FOLDER` explicitly and passes
the same folder to both version detection and environment creation. An
unavailable configured Runtime is detected before creating any web UI.

## Running

After building the Windows release preset, from the repository root:

```powershell
./tests/windows/run-native-flow.ps1
```

The script checks exit codes, requires exactly one successful JSON report per
successful process, runs the independent verifier, and enforces timeouts.
The existing basic `TorrentControl.exe --self-test LOGFILE` remains available.

## Evidence and limits

Historical M2 local verification: **115/115 CTest entries** and **19/19 Chromium UI
tests** passed, including recovery checkpoint success and failure regressions.
Linux unit/integration tests and mocked Chromium tests provide additional
coverage; they do not execute the Windows host.

The original native workflow, fresh-process settings test, four independent output
verifications and missing-runtime check run in the Windows job of
[PR #2](https://github.com/kurasis/TorrentControl/pull/2/checks).
The check details link the CI log and `windows-native-evidence` artifact. CI
reports the actual Runtime version and final successful scenario flags; those
checks passed before that change was merged. M3 adds two edited output
verifications and editor snapshot recovery to the same required CI workflow.

The automated Windows runner covers actual WebView2 and common item dialogs.
It does not establish manual accessibility, keyboard-only operation of Windows
dialogs, physical monitor DPI transitions, Explorer/client associations, drag
and drop of actual Explorer objects, Windows 10/11 clean-machine installation,
offline prerequisite installation or client interoperability. Those remain
explicit release/performance gates in the roadmap.
