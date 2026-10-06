# Native bridge responsiveness

Windows dispatches commands on one ordered worker instead of its window STA.
This includes first preflight/filter work, batch scanning, metainfo import,
project/settings I/O and serialization. Common item dialogs and shell actions
are marshalled to the STA through posted callbacks; their COM objects never
cross threads. Revision checks still run when each ordered command executes.

The queue accepts at most 64 outstanding commands and 8 MiB of waiting input,
including attached native paths. Saturation produces a correlated, retryable
`BRIDGE_BUSY` response. Individual requests retain the existing 1 MiB limit
and origin/schema checks. Navigation or renderer recovery drops commands which
have not started and replies belonging to the previous page. A running command
finishes once: its effects are recovered through the native snapshot rather
than replaying commands.

Shutdown closes the UI callback queue before joining the bridge worker. This
releases a worker waiting for an unprocessed STA callback; pending commands
are discarded. Existing scan/job/diagnostics workers retain their cancellation
and joining rules. A running filesystem call can still delay process exit;
this package does not claim bounded cancellation of stalled Windows/UNC I/O.

## Evidence

Local Linux validation passed 139/139 CTest entries and 30/30 Chromium UI
tests. The retained real-filesystem fixture benchmark also passed with
100,000 files and 1,000 filtered matches. Windows heartbeat evidence is
validated separately in CI. PR #6 passed Windows/Linux/UI CI. Its Windows
report measured 243 native timer ticks and 245 WebView2 frames during the
100,000-file workflow, with maximum gaps of 16 ms and 15.8 ms respectively.
Dialogs, metadata editing, network diagnostics, renderer recovery and settings
restart also passed. Measurements are observations of that hosted runner.

Portable regression checks cover worker-thread execution/FIFO ordering, page
generation changes without replaying queued mutations, saturation and busy
response correlation, STA callback ownership and shutdown of a waiting worker.

Windows CI retains the 100,000-file native benchmark fixture and passes its
path natively to the self-test host. The page cannot supply arbitrary paths.
The actual WebView2 flow runs native scanning, uncached hybrid preflight,
first filtering and snapshot on this fixture while measuring both window
timer ticks and WebView2 animation frames. It requires all 100,000 files,
1,000 matching filter rows, accepted preflight, at least two ticks/frames and
no heartbeat gap of one second. The report is included in `flow.log.json`
under `responsiveness` and survives the renderer recovery checkpoint.

This is a responsiveness regression gate on hosted Windows, not a guarantee
of a particular latency on every machine. Peak creation memory, stalled I/O,
independent v2 client verification and complete outgoing-message bounds remain
separate tasks.

```powershell
python tests/performance/native_bridge.py --proof build/windows-x64-release/tools/tc-proof/tc-native-proof.exe --report build/performance/windows-native.json --fixture-root build/performance/fixture
./tests/windows/run-native-flow.ps1 -PerformanceRoot build/performance/fixture
```

An explicitly retained fixture must be a new directory; the benchmark refuses
to overwrite existing files. The default temporary fixture still cleans up
after the benchmark. Without `-PerformanceRoot`, the standalone native flow
runs its existing dialogs/create/edit/diagnostics/recovery checks only.
