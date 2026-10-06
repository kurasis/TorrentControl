# Long sessions and bounded job views

`tests/performance/session_memory.py` runs **3000 real jobs in one process**:
three cycles of 1000, using v1, v2 and hybrid respectively. Each cycle uses
ten waves of 100 over the same 32 real files. Waves alternate single creation,
payload verification and single-item batch creation: 700 create jobs including
300 batches, 300 verify jobs, plus one separate warm-up creation per format.
Every job uses the production AppService, reader, hashing and output writer.

Linux CI runs one headless session. Windows CI runs one headless and one actual
Win32/WebView2 session over identical input: **9000 measured jobs** across CI
plus nine warm-ups. The GUI uses a developer-only bulk enqueue hook; ordinary
events, snapshots, history navigation, result pages and Clear update the real
renderer. The hook requires `--self-test LOG --self-test-session CONFIG` and
is absent in ordinary use.

## Findings and changes

A local pre-fix 1000-job run retained 1000 joinable per-job threads,
700 create specifications, 300 verification specifications, 22,400 manifest
entries and 338,400 bytes of verification input. Clear released job rows but
retained all 300 batch aggregates. AppService also retained batch job-ID
collections. The old renderer fetched and displayed the whole history.

The scheduler now uses a reusable pool, grown lazily to configured concurrency,
clamped to 1..8. Lowering concurrency limits newly started work; running jobs
finish normally. Workers created at a higher setting remain until shutdown,
so the pool stays at most eight. A pending-ID queue avoids rescanning completed
history. Cancellation and pause controls still belong to each job, including
the existing Windows SMB cancellation and shutdown paths.

Frozen create/verify inputs are released after the worker returns from the
engine, including failure and cancellation. Queued cancellation releases them
immediately. Results, file mappings, verification reports and logs remain
native-owned until Clear. Shared ownership keeps a terminal row alive while
a listener or another thread clears it. Clear no longer unlocks while holding
iterators into a mutable history vector.

Clear removes batch job-ID collections. A bounded archive of the latest
**64 aggregate reports** survives Clear for pending completion notifications.
Older archived batch IDs return an empty status. Reports for batches with
remaining history rows are retained.

The renderer keeps one page of **at most 50 summaries**, plus one selected
off-page summary. Only the page renders job rows. First/Previous/Next/Latest
load native pages; Next follows the byte-budget cursor and Previous stores at
most 64 visited offsets. Collection revisions prevent mixing changed pages,
with three read attempts and no replay of mutations. Newer events win over
older page replies. Off-page terminal events still resolve completion watches.
Latest falls back to the final row when the native byte budget shortens its
50-row window, keeping the newest job reachable without loading all history.

## Checks and evidence

Each cycle checks 1000 successful states and the expected kind mix, zero
retained frozen inputs, a bounded pool, history at offsets 0, 450 and 950, and
readable logs/layout/verification details after input release. Clear must leave
zero job rows, active batch records and AppService batch job IDs, with at most
64 archived aggregates. Actual WebView2 checks page navigation, pinned
selection, native result pages, at most 51 cached summaries and 50 rendered job
rows. Native and renderer heartbeats must stay below two seconds in this fixture.

The driver samples the task-owned process tree every 50 ms, retaining process
birth identities, RSS/working-set sums, Linux PSS, Windows private commit, and
native root thread and handle/fd counts. It reports each cycle before and after
Clear, including cleared-memory drift. Normal exit must leave no sampled
WebView2 descendants. The independent Python verifier checks six archived
create/batch outputs per mode; GUI/headless bytes must match. Source hashes
must be unchanged and temporary output files absent.
Root resource probes are enabled specifically for these sessions. A permission
denial is deferred to the next sweep/end to distinguish process exit from a
live inaccessible process; a live denial still fails the measurement. Controls
exercise both cases, as well as real child/grandchild residency and reparenting.

RSS sums can count shared pages multiple times. Sampling sweeps are not atomic
and can miss peaks; phase boundaries can include a sweep begun in the preceding
phase. Private commit differs from resident RAM. OS caches are not flushed;
elapsed time includes checkpoint holds, bridge and render overhead. There is
no hardware-independent RSS threshold. This covers three cycles of small real
inputs; complete native history deliberately grows until Clear. Large histories
of 100,000-file reports, many opened torrents, days-long sessions and physical
remote NAS workloads remain broader gates.

Run after building, with `tests/performance/requirements.txt` installed:

```sh
python tests/performance/session_memory.py \
  --proof build/linux-release/tools/tc-proof/tc-workflow-proof \
  --report build/performance/linux-long-session.json
```

On Windows, add `--gui build/windows-x64-release/src/app/windows/TorrentControl.exe`
and use the `.exe` proof. CI uploads `linux-long-session` and
`windows-long-session`: reports, samples, checkpoints, GUI logs and torrents.
The checked-in [local Linux report](evidence/long-session/linux-local.json)
documents this fixture; Windows evidence comes from CI, not Linux emulation.
The [pre-fix diagnostics](evidence/long-session/linux-before.json) cover one
1000-job v1 cycle, without a pre-fix GUI or OS-memory claim. Local validation
passed 170 CTest cases, 52 Chromium UI cases, both sampler controls and both
workflow fault controls.
The final local cleared-phase peaks were 118,603,776 / 127,549,440 / 127,582,208
resident bytes; root file-descriptor peaks were 3 / 3 / 3. These are observations
on this container, not process-wide memory limits.

Regressions cover reentrant terminal clearing, concurrent Clear/enqueue,
queued cancellation with other work running, bounded batch archives, retained
verification details, 3000-row UI recovery and 3000 additional events. Fuzz
campaigns keep the user's **three runs per campaign**.
