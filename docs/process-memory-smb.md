# Process-tree memory and real SMB fault evidence

`tc-workflow-proof` and the native `--self-test-memory` path run the same
`AppService` fixture. They use the ordinary scheduler, payload reader, engine,
validation and atomic commit. The console driver accepts JSON lines so a
pending filesystem operation does not block the control channel. These are
developer tests; there is no new public CLI or Python runtime dependency.

## Process-tree memory

`tests/performance/workflow_memory.py` starts a fresh process for every case:
100,000 real small files and a 96 MiB file with a tail, each in v1, v2 and hybrid.
Windows pairs the console workflow with the actual Win32/WebView2 application.
Both modes use identical source mappings, piece lengths and metadata with the
creation date omitted. Their torrent bytes must match, and every output is
checked by the independent Python payload verifier.

The driver records idle, scan, review, create, completed-job and cleared-history
phases. A 50 ms sampling sweep sums RSS/working set of the host and discovered
descendants, including WebView2 browser/renderer/GPU/utility processes.
PID plus creation time prevents PID reuse from including unrelated processes;
discovered children remain tracked after reparenting. Missing resident-memory
access fails the evidence run. A real child/grandchild control verifies summed
memory and tracking after the original parent exits. Normal GUI shutdown must
leave no discovered live child processes.

RSS sums can count shared pages more than once and process queries occur
sequentially within each sweep. Linux PSS, when accessible, and Windows private
commit are reported as separate metrics. The driver, OS page cache, kernel and
driver allocations are excluded. Sampling can miss transient children and
inter-sample peaks. Paired GUI-minus-headless observations are differences
between separate runs, not an additive component-memory bound. There is no
hardware-independent RSS threshold or claim that the whole process uses no
more than the payload-buffer budget. This exercises one completed job per
fresh process; a long session with thousands of jobs remains separate work.

The first full local run found a real 100,000-file hybrid creation failure:
libtorrent's default 3,000,000 decode tokens rejected output already accepted
by the application's bounded parser. Generated-output validation now allows
6,000,000 engine tokens: each application value can contribute its token, a
dictionary key and a container-end token. The application still first enforces
its existing 64 MiB and 2,000,000-value bounds. Imported-file parsing and fuzz
input limits are unchanged. The real-file memory matrix is also the regression
for this failure.

```sh
python3 -m venv build/performance-python
build/performance-python/bin/pip install -r tests/performance/requirements.txt
build/performance-python/bin/python tests/performance/workflow_memory.py \
  --proof build/linux-release/tools/tc-proof/tc-workflow-proof \
  --report build/performance/linux-process-memory.json
```

On Windows, use the `.exe` proof and add
`--gui build/windows-x64-release/src/app/windows/TorrentControl.exe`.
Reports retain per-phase peaks, metric definitions, original sample sweeps,
process identities, workflow results and native GUI logs as CI artifacts.

Local Linux validation passed 166 CTest cases both normally and under
ASan/UBSan, three sampler/fault-driver controls and all six full-size memory
cases. Observed headless creation peaks were 410 / 423 / 950 MiB for the
100,000-file v1 / v2 / hybrid tree, and 54–65 MiB for the 96 MiB file.
These are machine-specific observations. The
[local report](evidence/process-memory/linux-local.json) retains all phase
measurements; CI publishes the Windows GUI/headless pairs and Linux SMB cases.

## Real SMB faults

`tests/performance/smb_faults.py` requires a disposable root-capable Linux
runner, Samba, cifs-utils, iproute2 and the CIFS kernel module. It creates its
own Samba configuration, read-only guest share, veth pair and network namespace.
The client reaches the isolated peer through TCP/SMB 3.1.1 and a real kernel
CIFS mount with `cache=none`, `actimeo=0` and a soft reconnect policy. There are
no public SMB endpoints or user credentials. Existing machine shares and SMB
services are untouched. Root privileges are for this disposable fixture only.

For each format, a real 64 MiB payload first produces a baseline torrent,
independently verified over the mounted share. Fault cases then exercise:

- A production read stalled while the Samba process group is suspended;
  cancellation keeps the reader owned until the native syscall settles.
- A delayed production source open; cancellation leaves `Cancelling` and the
  control channel available until the server is resumed.
- A server disconnect during the stalled read; a filesystem error must end
  the job as Failed, not publish a successful replacement.

A one-shot test gate coordinates injection before the native open/read.
That gate is released before any pending-I/O assertion. The fixture requires
the production operation to remain pending for 250 ms with no read progress,
records kernel wait channels, checks responsive cancel/snapshot replies, then
restores the server. It requires a terminal state, an actual scheduler join,
zero live readers, unchanged previous output and no temporary files. Every
case performs a healthy retry with identical baseline torrent bytes; the
original payload must remain unchanged. Normal cleanup must unmount the share
and remove the task-owned namespace, interfaces and server.

The real GUI run also exposed retained renderer job history: snapshot refresh
merged jobs but never removed rows already cleared in native state. Complete
snapshot reconciliation now removes unchanged missing rows while retaining job
events received during the read. A background redraw likewise keeps an active
paged row input until blur commits it. Browser regressions cover both cases.

```sh
sudo apt-get install -y samba cifs-utils iproute2
sudo modprobe cifs
sudo build/performance-python/bin/python tests/performance/smb_faults.py \
  --proof build/linux-release/tools/tc-proof/tc-workflow-proof \
  --report build/performance/linux-smb-faults.json
```

This is one physical Linux runner with an isolated peer network stack. It
establishes real TCP/SMB and Linux CIFS fault behavior, not remote Windows SMB
driver behavior, Windows delayed-open cancellation or a universal two-second
deadline. Linux synchronous calls intentionally retain Cancelling until the
OS operation returns. Remote Windows SMB, arbitrary storage-driver stalls
and shutdown against an indefinitely unavailable server remain release gates.
The cloud workspace kernel lacks CIFS; that fixture is validated on the Linux
CI runner, and this prerequisite is not reported as a passing local SMB test.
Fuzz campaigns retain the user's three-run smoke limit.
