# Stock download-manager imports

The Linux fixture imports original TorrentControl metainfo into the official
qBittorrent **5.2.4 libtorrent-2.0 AppImage** and the unmodified BiglyBT
**4.1.0.0 Core/GlobalManager**. It uses qBittorrent's authenticated loopback
Web API and BiglyBT's public download-manager APIs. Neither adapter repairs
metadata, changes hashes, remaps files, or assumes that seeding data is complete.
The earlier [BiglyBT parser/hash audit](independent-v2-hash-audit.md) remains a
separate check of both hybrid hash families.

## Matrix and remaining gate

There are 12 imports: two clients × v1/v2/hybrid × single-file/multifile.
The folder includes an empty file, a one-byte file, Unicode paths and sizes
around 16 KiB and 64 KiB boundaries. Each imported download manager remains
alive for four forced checks: intact data, a changed byte, a removed file,
and restored data. Both full native infohashes, natural file paths and lengths
must match. Original torrent SHA-256 and payload inventories are checked around
each operation; network data counters must remain zero.

| Client | v1 | Pure v2 | Hybrid |
| --- | --- | --- | --- |
| qBittorrent 5.2.4 | Single-file and folder verified | Single-file and folder verified | Single-file and folder verified |
| BiglyBT 4.1.0.0 | Single-file and folder verified | Single-file verified; leading-empty folder gate open | Single-file and folder verified |

BiglyBT's folder pure-v2 recheck reports **909/1000**, leaving `file-1.bin`
(one byte) unverified when `file-0.bin` is empty. All other real files are
complete. This reproduces after restoration and is preserved as an exact
regression expectation. Its stock
[DMCheckerImpl](https://github.com/BiglySoftware/BiglyBT/blob/v4.1.0.0/core/src/com/biglybt/core/disk/impl/access/impl/DMCheckerImpl.java)
uses the first piece-map entry's file length in its v2 hasher setup. A leading
zero-length entry violates the checker's assumption that a v2 piece contains
only its data file and optional padding. Independent reference and BiglyBT
root/layer hash audits pass the same producer layout. The fixture does not
remove the empty file or patch this client.

CI checks the precise known failure and rejects any unexpected hash/path,
healthy-data, negative-control, recovery or cleanup failure. Its report keeps
`passed: false`, `allClientsVerified: false`, and `releaseGate: "open"` while
`regressionChecksPassed: true` means the fixture behaved as documented.
`--require-complete` exits **5** while the compatibility gate remains open.
Green development CI therefore does not approve distribution.
The [local evidence report](evidence/stock-client-imports/linux-local.json)
records all 12 imports, 48 rechecks and 11 fully verified combinations. The
same complete matrix was also run with strict acceptance, which exited 5
after writing its report and removing the container.

## Isolation and lifecycle

Clients run under an unprivileged UID in a disposable Docker container with
`--network none`, only loopback, a read-only root, dropped capabilities and
`no-new-privileges`. Payload, original torrents, adapters and vendor binaries
are mounted read-only; only private client profiles and `/tmp` are writable.
The driver changes/removes/restores the test payload from the host side.
Missing data may yield a native storage error because the client cannot
recreate a file on the read-only mount; this proves refusal of incomplete
data, rather than a completed hash check of an absent file.

qBittorrent runs its actual Qt GUI under Xvfb. Its API uses random disposable
credentials and never receives host credentials or proxy settings. Forced
recheck results allow three seconds for the GUI's asynchronous libtorrent
status cache; the corruption/restoration controls verify that results change
within the same client process. BiglyBT runs the actual application Core
without its desktop UI, disables plugins and peer sources, and waits on the
public force-rechecking state. Both exit normally between cases. The owned
container is removed before temporary AppImage extraction is deleted.

## Reproduce

Requires Linux x86-64, Docker with BuildKit, Python 3.11+ and a built `tc-proof`.

```sh
python3 tools/stock-client-proof/fetch.py build/stock-clients
docker build --secret id=host_ca,src=/etc/ssl/certs/ca-certificates.crt \
  --build-arg HTTPS_PROXY --build-arg https_proxy --build-arg HTTP_PROXY --build-arg http_proxy \
  --tag tc-stock-clients:local --file tools/stock-client-proof/Dockerfile tools/stock-client-proof
python3 tests/integration/stock_client_imports.py \
  --proof build/linux-release/tools/tc-proof/tc-proof --clients build/stock-clients \
  --report build/performance/stock-client-imports.json
```

Use a new report basename for another run: the fixture refuses to reuse its
evidence directory. Add `--require-complete` for strict release acceptance.
The Docker build uses the host's public CA bundle through an ephemeral
BuildKit secret; HTTPS and package signature verification stay enabled.

Vendor provenance and SHA-256 pins are in
[fetch.py](../tools/stock-client-proof/fetch.py). Every run verifies raw vendor
artifacts and freshly extracts the verified AppImage. These GPL developer
dependencies are not bundled with TorrentControl. CI uploads the report,
logs and original fixture torrents/payload as `linux-stock-client-imports`,
without credentials, profiles or client binaries.

This proves local import/recheck behavior of these pinned Linux clients.
Peer downloads, tracker connectivity, Windows client UI, arbitrary releases,
and universal compatibility remain outside this fixture. A hybrid recheck
alone does not prove both hash families; the independent hash audit remains
mandatory. The anacrolix audit's previously documented gates are unchanged.
