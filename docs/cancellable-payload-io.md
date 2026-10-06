# Stop-aware payload I/O and safe Windows cancellation

The hashing pipeline passes the job stop token to source opens, all payload
reads and the EOF probe. Verification propagates `Cancelled` as cancellation;
it does not turn it into an unreadable-file report and hash the remaining
bytes as zeros. Stop wakes queue waits and workers check it between pieces.
Synthetic padding/missing-file processing also checks stop between units.
Worker-start exceptions wake and join any workers already started.

Ordinary Windows sources open overlapped read handles with the existing
read-only sharing restrictions. Each read owns its event and OVERLAPPED and
uses an explicit 64-bit file offset. A stop callback requests `CancelIoEx` for
that specific operation. Registration precedes submission; a second check
covers stop during submission. The reader always waits for completion before
releasing the event, OVERLAPPED, buffer or handle. After success the offset
advances by the actual byte count; EOF still returns zero.

Pause continues to stop scheduling new reads and waits for an outstanding read
and hash work to finish before reporting Paused. Cancel can interrupt supported
pending reads while Pausing. The atomic output replacement remains a short
non-interruptible Committing phase.

## Evidence

Portable regressions cover stop before a legacy open/read, pending-read and
EOF-probe cancellation for both creation and verification, a subsequent
successful job, safe reader destruction and scheduler shutdown. An explicitly
uncancellable adapter remains in Cancelling, retains its reader and exposes
native snapshots until the blocked read completes. No output is published.

The Windows-only kernel fixture opens a local named pipe with an overlapped
client handle and uses the production reader. `GetThreadIOPendingFlag` confirms
that the worker has real pending I/O while the server deliberately withholds
data. The test requests stop, requires completion within two seconds, joins
the worker and performs a successful new read through the same handle. A
bounded fallback releases the fixture so a regression fails without leaving
the test stuck. This exercises kernel I/O cancellation, not a sleeping reader.

`tests/performance/payload_cancellation.py` adds six local-file scenarios:
v1/v2/hybrid with two budgets on a real 64 MiB payload. It records stop-to-join
latency, bounds actual read bytes, requires cancellation within two seconds,
preserves a pre-existing valid torrent byte for byte, leaves no temporary
files and checks unchanged payload data. Windows and Linux CI retain JSON
reports and rerun the 12 memory/reference scenarios from the previous package.

```sh
python3 tests/performance/payload_cancellation.py \
  --proof build/linux-release/tools/tc-proof/tc-proof \
  --report build/performance/linux-payload-cancellation.json
```

Local Linux validation passed **154/154 CTest** entries, all six cancellation
cases and the repeated 12 creation/reference cases. Observed local stop-to-join
latency was 0.16–0.78 ms. The kernel fixture is Windows-only and is validated
separately in CI before merging. These timings describe this machine.

## Remaining I/O limits

Opening a source, querying file attributes, scanning directories and output
filesystem calls remain synchronous. POSIX regular-file and third-party
legacy adapters check cancellation before and after a read; they cannot
interrupt an already blocked operation. Windows drivers may delay or decline
cancellation. Those cases retain Cancelling and owned resources until the OS
operation settles; shutdown can wait for them. No arbitrary worker is killed.

The kernel fixture is local IPC. Existing Windows integration exercises UNC
creation when the runner exposes its administrative share; a missing share
is explicitly skipped. Real remote SMB fault injection, delayed opens and
storage-driver stalls remain release gates. These tests do not establish a
universal two-second deadline for UNC or every filesystem operation.

The [process-tree and SMB package](process-memory-smb.md) adds real TCP/SMB
fault injection through Linux kernel CIFS and an isolated Samba peer. It
checks stalled reads/opens, cancellation with retained resources, disconnect
failure, output preservation and a healthy retry. Remote Windows SMB and
arbitrary driver stalls remain open; the Linux fixture does not close them.
