# Creation payload memory and real I/O evidence

Creation and verification now enforce their configured payload-buffer budget.
A budget smaller than one complete piece, including zero, is rejected before
opening payload sources. Choose a smaller piece or increase the budget. The
former implicit one-piece exception has been removed. Default 128 MiB budgets
still support the maximum 128 MiB piece size.

One shared planner computes aligned unit size, maximum buffer count and hash
workers for preflight and execution. Workers are limited by available piece
buffers. Recycled payload allocations use exact-sized owned arrays; queued,
active and reader buffers together cannot exceed the budget. Digest scratch,
file manifests, hash tables, libtorrent and metainfo parsing are separate
allocations, so this is not a limit on total application memory.

`CreateResult::hashing` records allocated buffer count, exact peak allocated
payload bytes, unit size, workers, maximum main read request and digest-slot
bytes. The intermediate layout mapping and hash-result vectors are freed after
feeding libtorrent, before serializing and validating the result.

The developer-only `tc-proof` additionally instruments actual PayloadSource
open/read calls and reports peak host RSS/working set and creation time.
`tests/performance/creation_memory.py` runs each case in a new process: 10,000
real files and a 96 MiB file with a tail, three formats and 1 MiB/8 MiB budgets
with one/four requested workers. It asserts exact single-pass payload reads,
budget compliance, identical torrent bytes across scheduling choices and
independent Python payload verification. CI saves reports for Linux and Windows.

Local validation on 2026-10-06: **150/150 CTest** cases and all **12 creation
benchmark cases** passed. For the large-file cases, buffers allocated exactly
1 MiB or 7.5 MiB under the respective budgets. Headless peak RSS observations
were about 30–68 MiB across this dataset matrix. This is an observation of this
Linux environment, not a hardware-independent pass/fail threshold.

Reports record OS, processor count, engine, datasets, cache policy, estimated
memory, measured RSS and I/O. OS caches are not forcibly flushed; repeats are
warm. Peak RSS includes the CLI scan/create/commit/reopen workflow. On POSIX,
the high-water counter can retain the subprocess's pre-exec RSS baseline.
These results exclude WebView2, driver and other process-tree memory. Resource
estimates remain rough planning estimates, not upper bounds on native metadata
allocations; the measurements show overhead beyond those estimates. Whole
application process-tree profiling and tighter metadata estimates remain open.

The [process-tree evidence package](process-memory-smb.md) adds fresh-process
AppService measurements and paired real WebView2/headless workflows, including
all discovered child processes. Sampling and native-model limits are documented
separately; tighter metadata estimates remain open.

```sh
python3 tests/performance/creation_memory.py \
  --proof build/linux-release/tools/tc-proof/tc-proof \
  --report build/performance/linux-creation-memory.json
```

The [payload I/O package](cancellable-payload-io.md) adds stop propagation
and safe cancellation of supported pending Windows reads. Delayed source
opens and uncooperative storage/remote SMB operations remain open gates.
