# Native performance and independent compatibility: first package

This stage has started. The benchmark drives the actual `AppService` and JSON
dispatcher against 100,000 real files. It measures native scan/review operations
and serialized response size, not WebView2 rendering or creation peak memory.
The existing browser virtualization test remains separate.

## Implemented changes

- Unfiltered manifest pages access only their requested range.
- One cached case-folded filter index serves subsequent pages; switching
  filters replaces it, bounding retained index memory to one manifest.
- Frozen-manifest validation, layout estimates, preflight issues and review
  summary are cached. Draft changes invalidate caches; rescans create new
  cache owners. Source/name/format/piece/profile/private changes cannot retain
  stale results. Dynamic destination checks still run on each validation.
- `tc-native-proof` measures real dispatcher latency, peak working set/RSS
  and response size. Python creates and cleans up the actual filesystem tree.
  Windows and Linux CI save JSON reports; timing is evidence, not a flaky
  hardware-independent pass/fail threshold. File counts, accepted preflight
  and responses below 1 MiB are asserted.
- The Windows loopback fixture publishes its complete port file atomically,
  then the launcher validates the port before starting WebView2. This avoids
  treating an empty, partially written readiness file as port zero. Self-test
  failure reports retain the native error message as well as its code.

Local Linux release measurements on 2026-10-05 (p50, milliseconds):

| Operation | Before | After |
| --- | ---: | ---: |
| Unfiltered 200-row page | 1.09 | 0.71 |
| Repeated filtered 200-row page | 50.29 | 0.61 |
| Repeated native snapshot | 1438.35 | 0.04 |
| Validation | 2778.30 | 78.72 |

The first filter still took 40.75 ms and the first validation 1214.70 ms.
After-change scan plus native summary took 1789.87 ms. Maximum response was
55,124 bytes; peak scan/review RSS was 164,118,528 bytes. These are local
observations, not guarantees for other hardware, Windows or UNC sources.
Full raw reports are in `docs/evidence/native-performance/`.

Local validation: **135/135 CTest entries** passed. A regression warms caches,
then changes name, format, piece length, private policy and source contents;
it also creates/removes the destination without changing draft revision and
requires fresh output validation.

## Independent client audit

The development-only Go tool pins anacrolix/torrent **v1.61.0**, Go **1.25.5**
and module checksums. Trackers, DHT, PEX, TCP/uTP, WebTorrent, web seeds,
upload/download, port forwarding and HTTP dialing are disabled. It uses the
stock client parser, storage and `VerifyDataContext`; no fake infohash adapter
or rewritten metadata is used. Negative controls corrupt/remove a real file,
then require verification failure. Payload contents and file inventory must
remain unchanged.

The audit compares native/reference results and imported infohashes for six
shape/format combinations, including Unicode paths, empty files and piece
boundaries. It found concrete open compatibility gates:

| Shape / format | anacrolix result |
| --- | --- |
| Multifile v1 | Payload verified; corrupt/missing controls rejected |
| Single-file v1 | Payload verified; corrupt/missing controls rejected |
| Multifile hybrid | Imported, both infohashes matched; client payload check passed; negative controls rejected |
| Multifile / single-file pure v2 | Metainfo/piece layers parsed, but stock AddTorrentOpt requires a nonzero legacy v1 lookup ID; client payload verification is not established |
| Single-file hybrid | Import succeeds, but the client resolves `name/name` for the pinned libtorrent single-file file-tree shape and cannot find the selected payload |

Hybrid client verification follows this client's v1 piece-hash path; it is
not independent proof of all v2 payload roots. The Python reference checks
those separately. All six torrents pass native and Python reference checks.

The audit writes `allClientsVerified: false` and `releaseGate: open` when any
combination remains unverified. CI checks report generation and known supported
cases; it **does not** declare the full compatibility gate passed. Invoke
`--require-complete` to enforce the release gate (currently exits with 5).

## Reproduce

```sh
python3 tests/performance/native_bridge.py \
  --proof build/linux-release/tools/tc-proof/tc-native-proof \
  --report build/performance/native.json

cd tools/anacrolix-proof
CGO_ENABLED=0 go build -p 4 -buildvcs=false -mod=readonly -o ../../build/anacrolix-proof .
cd ../..
python3 tests/integration/client_compatibility.py \
  --proof build/linux-release/tools/tc-proof/tc-proof \
  --client build/anacrolix-proof --report build/performance/client.json
```

## Remaining work before closing this stage

1. Move first expensive preflight/filter/import work outside the native UI
   thread and measure actual WebView2 interaction/heartbeat during it.
2. Measure creation peak memory and I/O on large trees and stalled/UNC sources.
   Existing instrumented hybrid tests prove one payload read pass on their
   local fixtures, not stalled Windows I/O cancellation.
3. Resolve the second client's pure-v2 API and single-file hybrid path issues,
   cross-check with a mature independent v2 client such as BiglyBT and verify
   both hybrid hash families independently. Preserve every existing raw-info
   and canonical-layout guarantee while investigating.
4. Complete outgoing bridge bounds for large verification/job/import data and
   fuzz/sanitizer runs. Distribution/clean-machine release gates remain M4.
