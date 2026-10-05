# Windows / WebView2 validation

This stage extends M2 stabilization with a real native workflow. It does not
implement the M3 editor or diagnostics. `--self-test-flow` uses the production
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
5. Save and reopen a project through Windows dialogs. Confirm its resolved
   piece size and recreate a byte-for-byte identical hybrid torrent.
6. Save a magnet through the Windows dialog and compare the actual file.
7. Save Russian/dark/advanced settings, crash the renderer using CDP
   `Page.crash`, and require one actual `ProcessFailed` recovery. The new page
   restores draft, completed jobs and appearance from the native snapshot;
   it must not replay creation. A normal page reload cannot pass this check.
8. Start a fresh application process with the isolated settings directory and
   verify that the saved appearance survived the process restart.
9. Verify all four actual output torrents with the independent Python BEP
   3/47/52 implementation, against the files picked through Windows.
10. Select a nonexistent fixed WebView2 runtime using a process environment
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

## Running

After building the Windows release preset, from the repository root:

```powershell
./tests/windows/run-native-flow.ps1
```

The script checks exit codes, requires exactly one successful JSON report per
successful process, runs the independent verifier, and enforces timeouts.
The existing basic `TorrentControl.exe --self-test LOGFILE` remains available.

## Evidence and limits

Local Linux verification: **115/115 CTest entries** and **19/19 Chromium UI
tests** passed, including recovery checkpoint success and failure regressions.
Linux unit/integration tests and mocked Chromium tests provide additional
coverage; they do not execute the Windows host.

The native workflow, fresh-process settings test, four independent output
verifications and missing-runtime check run in the Windows job of
[PR #2](https://github.com/kurasis/TorrentControl/pull/2/checks).
The check details link the CI log and `windows-native-evidence` artifact. CI
reports the actual Runtime version and final successful scenario flags; those
checks must pass before this change is merged.

The automated Windows runner covers actual WebView2 and common item dialogs.
It does not establish manual accessibility, keyboard-only operation of Windows
dialogs, physical monitor DPI transitions, Explorer/client associations, drag
and drop of actual Explorer objects, Windows 10/11 clean-machine installation,
offline prerequisite installation or client interoperability. Those remain
explicit release/performance gates in the roadmap.
