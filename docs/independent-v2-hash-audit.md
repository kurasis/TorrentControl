# Independent v2 and hybrid hash audit

`tests/integration/biglybt_compatibility.py` exercises BiglyBT 4.1.0.0's
metainfo parser, SHA1Hasher, v2 file-tree builder, payload root hasher and
piece-layer Merkle validator. It uses Java 21 source-file mode, without
starting a BiglyBT application Core, download manager, GUI or swarm.
Configuration goes into disposable directories outside the payload. BiglyBT
is a GPL developer dependency downloaded during the audit; it is not included
in TorrentControl's application artifacts.

The six cases cover single/multiple files in v1, v2 and hybrid, Unicode
paths, empty files and 16 KiB/64 KiB boundaries. Both imported identifiers
must match the native creator's `result.infohash_*` fields. The older
anacrolix audit previously inspected the top-level JSON instead, silently
skipping those comparisons; it now checks the actual result object.

For every case, healthy payload passes and changed/missing payload fails.
Hybrid controls independently damage v1 pieces, a v2 root, or a v2 piece
layer in disposable metadata. The other hash family must still pass. These
controls demonstrate that a successful v1 check cannot hide a v2 failure.
All client runs, including negative controls, must leave the payload's
inventory and bytes unchanged. Healthy torrent bytes are never rewritten.

## What this establishes

BiglyBT's algorithms independently validate both payload hash families and
v2 piece layers for the generated layout. The adapter assembles v1 pieces
from the imported client file list, including synthetic zero padding, and
uses the client's hash routines. It reads the original v2 tree through the
client's internal builder rather than changing metadata or copying files.
The two internal methods (`lashUpV2Files`, `addPieceLayer`) are pinned to the
audited release and accessed reflectively; API changes should fail the audit.

This is **not** a stock download-manager import/recheck test. BiglyBT's stock
hybrid root fixup assumes identical v1/v2 file counts. The generated multifile
hybrid can contain a trailing v1 padding entry that its v2 builder omits,
causing `Inconsistent v1/v2 files`. The independent tree audit bypasses that
fixup; it does not resolve or conceal the import limitation. The report states
`downloadManagerImportVerified: false`, `releaseGate: open` even when all six
hash audits pass. Anacrolix's pure-v2 lookup and single-file hybrid path
limitations also remain open; neither client nor native metainfo is patched.

## Reproduce

```sh
python3 tools/biglybt-proof/fetch.py build/biglybt/BiglyBT.jar
python3 tests/integration/biglybt_compatibility.py \
  --proof build/linux-release/tools/tc-proof/tc-proof \
  --jar build/biglybt/BiglyBT.jar \
  --report build/performance/biglybt-hash-audit.json
```

The fetcher checks SHA-256
`ff2a3d1cbcf8d9b816ffd022cde33db1c610fd38857f75a10148dc4a21e83572`
of the official release jar. This pin was computed from the published jar;
the upstream installer checksum manifest does not list this jar. No automatic
version or checksum upgrades occur. CI retains both client reports alongside
the Linux performance evidence. Supported native builds remain Windows/Linux;
this Java audit currently runs on Linux only.

A further client package must exercise the actual download manager's path
resolution, disk checks and completion state without these internal adapters,
or establish a documented upstream resolution before closing the import gate.
