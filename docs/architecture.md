# Architecture

TorrentControl is a Win32 C++ host with a WebView2 user interface. The native
side owns windows, dialogs, files, jobs, network diagnostics and persistence;
the web side renders state and sends validated commands. The engine has no
dependency on WebView2 or Windows UI types and is tested headlessly.

```
frontend (HTML/CSS/JS, bundled)            src/app/windows (NativeHost)
  bridge.js  ── JSON string messages ──▶    WebViewHost ── Dispatcher (src/bridge)
                ◀── responses, events ──      │  ▲            │   WindowsHostServices
                                              │  └ EventQueue │   (dialogs, Explorer)
                                              ▼               ▼
                                         src/service: AppService (draft, scan,
                                                   JobScheduler, profiles, batch,
                                                   projects, settings)
                                                              │
                                                              ▼
                                         src/core: ManifestService, TorrentEngine,
                                                   MetainfoService (bencode)
                                                              │
                                                              ▼
                                         libtorrent 2.1.2 (layout + serialization)
```

## Modules (specification section 3)

| Module | Location | Current state |
| --- | --- | --- |
| NativeHost | `src/app/windows` | Window, Runtime detection, WebView2 lifecycle, virtual-host asset mapping, navigation/new-window/permission/download blocking, renderer-failure reload, common item dialogs, Explorer and file-association actions, dropped-file paths, event marshaling, close prompt while jobs run |
| Frontend | `frontend` | Simple and advanced modes, metadata editor with field/hash preview, review summary, job queue, result/profile/batch dialogs, virtualized lists, light/dark theme, English and Russian strings |
| CommandBridge | `src/bridge` | Protocol v1 envelope, origin check, schema validation, stable error codes, `getEngineInfo`, application operations (`app_operations.*`), event envelope, tagged bencode JSON (`bencode_json.*`) |
| ManifestService | `src/core/manifest.*`, `src/core/native_fs.*` | Native enumeration with reparse-point and cloud classification, exclusions, link policy, file identity, frozen observations, recheck, Unicode collision checks, limits |
| TorrentEngine | `src/core/torrent_engine.*`, `src/core/hash_pipeline.*` | Preflight and resource estimates, parallel single-pass hashing, v1/v2/hybrid creation, output validation |
| MetainfoService | `src/core/bencode.*`, `src/core/metainfo.*`, `src/core/field_registry.*`, `src/service/torrent_editor.cpp` | Field registry, immutable edit previews, lossless bencode, metainfo validation, outer and info edits, signatures, legacy labels, magnet export |
| Verification | `src/core/verify.*` | Payload verification against any metainfo, per-file results |
| PersistenceService | `src/core/output.*`, `src/service/storage.*` | Atomic output commit, atomic settings and project files, DPAPI-protected passkeys |
| DiagnosticsService | — | M3 |
| JobScheduler | `src/service/jobs.*` | Queue with a concurrency limit, the job state machine of section 14.2, pause/resume/cancel, settings snapshot per job, progress throttling, bounded logs |
| ProjectService | `src/service/draft.*`, `src/service/profiles.*`, `src/service/batch.*`, `src/service/app_service.*` | Revisioned draft, profiles with change preview and undo, batch planning with conflict policies, projects |

## Torrent creation pipeline

`Scan → Validate manifest → Preflight → Resolve layout → Read/hash → Build metainfo → Validate → Commit`

1. **Scan.** `scan_source` enumerates with native APIs and an explicit stack.
   Every entry is classified (regular, directory, symbolic link, junction,
   other reparse point, special file) and every exclusion is recorded with a
   reason. Links are not followed by default. In follow mode, targets must stay
   inside the selected root and cycles are detected by file identity.
   Unreadable folders and files block creation until they are excluded. Cloud
   placeholders are either included (hydration then needs consent) or skipped.
   Default exclusions cover TorrentControl temporary and project files.
2. **Manifest.** Each entry maps a native source path to UTF-8 torrent path
   components and stores the observation made at scan time (size, last write
   time, change time, file identity, link count). Sources never have to share
   a base directory, and nothing is copied or renamed. Edits bump `revision`.
3. **Preflight.** Checks hydration consent, chooses or validates the piece
   size, bounds the layout (hash bytes and piece count) before libtorrent
   allocates anything, and estimates memory. Plans above 512 MiB need explicit
   acceptance.
4. **Layout.** `lt::create_torrent` is constructed from the manifest. For v2
   and hybrid it produces the canonical order and BEP 47 padding (including
   tail padding of the last file, libtorrent's default policy).
5. **Hashing.** One reader thread walks the canonical file list once through a
   `PayloadSource` and fills buffers from a fixed budget; a worker pool hashes
   them. The same bytes feed SHA-1 (v1 stream, pieces may cross files) and
   SHA-256 (16 KiB leaves folded into per-piece Merkle roots; v2 work units
   never mix files). Padding is generated in memory and never touches the disk.
   Output does not depend on the thread count or the budget.
6. **Build and validate.** Hashes go to libtorrent via `set_hash()` and
   `set_hash2()`. The generated bytes are reparsed by the lossless parser and
   by libtorrent, checked by `validate_metainfo` (layout, safe paths, hybrid
   consistency, piece layers against roots), and the sources are rechecked.
7. **Commit.** `commit_output` refuses an output that is a source file or a
   hard-link alias of one, writes a temporary file beside the target, flushes
   it, reopens and validates it, and moves it into place. An existing file is
   replaced only by explicit choice, and an output created by another program
   during the job is never overwritten.

## Source consistency

Consistency is best effort, not a snapshot. A file is compared with its
frozen observation when it is opened, again after it has been read, and once
more before the result is accepted. A replaced file, a size or timestamp
change, or growth beyond the frozen length fails with `SOURCE_CHANGED`. On
Windows the read handle also denies write and delete sharing while it is open.
A change that keeps the size and restores the timestamps between checks
cannot be detected; TorrentControl never claims a snapshot (W04). Verifying
the payload after creation is the way to prove a finished torrent.

No libtorrent session is ever created, so creation performs no network
activity.

## Lossless metadata

`bencode::Value` keeps byte strings and keys as raw bytes, integers as
canonical decimal text, and the input byte range of every value. Identifiers
are SHA-1/SHA-256 of the exact `info` slice. An outer-only edit writes a new
top-level dictionary, copying the `info` slice and every untouched value
byte-for-byte.

The [metadata editor](m3-metadata-editor.md) validates native patches against
the field registry, owns one immutable preview token and native output path,
and saves through `commit_output`. Snapshots restore the selected torrent and
reviewed candidate. Semantic no-ops preserve imported raw info and signatures.
Only a validated legacy v1 import with the exact original info slice may retain
noncanonical key ordering during an outer-only commit.

## Bridge security

- Frontend assets are served from `https://torrentcontrol.example/` mapped to
  the installed `frontend` folder only; navigation elsewhere is cancelled.
- Every message is a JSON string parsed by a JSON parser. The dispatcher checks
  the sender's document URI, size (1 MiB), protocol version, request ID,
  operation name, revision and payload shape before calling a handler.
- There is no host object, no script injection and no generic shell or file
  operation.

## Application service (M2)

`AppService` (`src/service`) is the single owner of application state; the
page mirrors it and can always be rebuilt from `getSnapshot`.

- **Draft.** Every mutation carries the revision the page last saw. A
  mismatch fails with `STALE_REVISION` (retryable) and the page refreshes, so
  an edit is never applied to a draft the user has not seen. Adding or
  changing sources starts a background scan; its result is tagged with the
  source revision and stale results are dropped.
- **Jobs.** `JobScheduler` copies the full creation settings into each job
  when it is queued, so later draft or profile edits never change queued work.
  States follow section 14.2; `Committing` cannot be cancelled. Pause parks
  the reader after in-flight buffers drain; file handles stay open.
- **Profiles.** Applying a profile first returns a plan listing every field
  it changes and whether a value the user entered is removed; the page asks
  when more than the profile name changes. The previous draft can be restored
  with undo.
- **Batch.** Per-file and per-child-folder plans inherit the draft settings;
  output conflicts are resolved per item (skip, replace, rename), and
  duplicates inside one batch are always renamed.

### Events and threading

Service events (`job`, `scan`) are raised on worker threads. The host queues
them, wakes the UI thread with a window message, coalesces progress updates
of the same job to the newest, and sends each as
`{"protocolVersion":1,"event":type,"sequence":"N","payload":{...}}`.
Sequence numbers are monotonic; the page ignores anything older than what it
has applied, and job snapshots carry their own version.

Bridge requests are answered on the UI thread outside the WebView2 callback,
because handlers may open modal dialogs. Paths for privileged operations
never come from the page: files are chosen in native dialogs, dropped files
are read from the `ICoreWebView2File` objects WebView2 attaches to the
message, and later commands refer to native-owned IDs.
