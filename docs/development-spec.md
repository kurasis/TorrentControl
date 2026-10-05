# Windows Torrent Creator — Development Specification

Version: 1.0 — reviewed handoff  
Review date: 2026-10-03  
Document language: English  
Target: Windows desktop application, initially x64  
Approved architecture: C++ + libtorrent + Microsoft Edge WebView2

This document supersedes the earlier Qt proposal and resolves ambiguities in the initial design. It is the implementation brief for the development chat. Build the application described here; do not restart technology selection. Requirements marked MUST are release requirements. SHOULD identifies a preferred implementation with a documented alternative. LATER identifies work outside the first release. Numeric performance values below are engineering targets, not benchmark results.

## 1. Product and release scope

Create, inspect, edit, and validate BitTorrent metainfo files through a responsive GUI. Support very large files and large file collections without loading payloads into memory. Offer a simple workflow while retaining access to advanced metadata.

The application MUST work offline for creation, inspection, editing, and local verification. Network activity is limited to user-requested diagnostics and catalog updates. Creating a torrent does not upload its contents, register it with a tracker, publish it in DHT, or start seeding.

### 1.1 Required first release

- Creation of v1, v2, and hybrid torrents from one file, a folder, or multiple selected sources.
- Recursive and non-recursive folder selection, exclusions, virtual destination paths, and a final manifest preview.
- Basic and advanced forms, a binary-safe bencode inspector/editor, and preservation of unknown fields.
- Trackers with tiers, ten built-in public addresses, catalog import/update, and protocol diagnostics.
- BEP 19 web seeds and BEP 17 HTTP seed metadata; diagnostics with clearly stated scope.
- Public/private profiles, configurable piece size, comments, timestamps, and optional extension fields.
- Batch creation, queued jobs, in-process pause/resume, cancellation, and progress.
- Editing outer metadata while preserving the original infohash; controlled editing that intentionally changes it.
- Local payload verification, magnet export, project/profile save/load, and an external-client handoff.
- Installer, offline installation option, source/build instructions, dependency notices, and tests.

### 1.2 Explicitly later

Persistent hash-cache reuse and restartable hashing after application exit; VSS snapshot orchestration; a dedicated strict locking mode; signing and certificate-management workflows; a built-in torrent client/seeder; WebSocket/I2P/Yggdrasil tracker probes; automatic publishing; Windows Explorer extensions; ARM64 packages; a public CLI.

Rare fields remain inspectable, preservable, and generically editable in release 1. That does not promise implementation of every protocol described by those fields. Public CLI packaging is later, but the core MUST be independently testable without WebView2.

## 2. Corrections established during review

| Earlier ambiguity | Binding decision |
| --- | --- |
| Qt versus WebView2 | Use a Win32 C++ host and WebView2. No Qt dependency. |
| “All torrent fields” | There is no finite universal schema. Implement a known-field registry plus a generic bencode editor with explicit support levels. |
| Ping means tracker works | ICMP is not a health criterion. Report transport, protocol, and authorization results separately. |
| Ten working trackers | The ten addresses were present in an externally checked list dated 2026-10-03. They were not independently probed from the preparation environment. Initial local status is Unchecked. |
| Hybrid is v1 plus a flag | Hybrid requires consistent v1/v2 layouts, hashes, and virtual padding. |
| One hash identifies all variants | Hybrid has two identifiers. A separately generated v1 file need not have the hybrid torrent's v1 infohash. |
| Every external field is harmless to edit | Outer fields can affect signatures, privacy, discovery, and v2 validity even when infohash stays unchanged. |
| Bencode is ordinary JSON | Keys and strings can be arbitrary bytes; integers are not JavaScript floating-point numbers. |
| File size/time checks prove consistency | They are best-effort detection, not an atomic filesystem snapshot. |
| “One-pass” means constant total memory | Payload buffers can be bounded; file manifests and hash metadata still grow with file/piece count. |
| Arbitrary source roots work with the default hasher | A virtual-to-physical source adapter is required; do not silently copy the dataset to a staging folder. |
| WebView2 is guaranteed to be installed | Detect a compatible Runtime and handle absence with a native bootstrap screen. |
| Saving a torrent starts distribution | External-client launch is a separate explicit action. |

## 3. Technology and module boundaries

Use C++20, CMake, a supported Windows SDK, and a pinned MSVC toolchain. The native host owns the window, Windows dialogs, jobs, filesystem access, network diagnostics, and persistence. WebView2 hosts bundled HTML/CSS/TypeScript. Node.js, if used by frontend tooling, is a build-time dependency only.

Use a pinned stable libtorrent release. At review time the upstream release page identifies **v2.1.2** as the latest release; treat it as the initial integration baseline, not a floating dependency. Confirm APIs against its headers and tests. The online reference identifies itself as 2.1.0 and is not sufficient evidence that a signature exists in every 2.x version. Do not copy a 2.0-era wrapper without testing it against the selected release. [L1, L2]

| Module | Responsibility |
| --- | --- |
| NativeHost | Win32 window, WebView2 lifecycle, file/folder dialogs, OS integration |
| Frontend | Form state, virtualized views, accessibility, localization, display-only validation |
| CommandBridge | Versioned asynchronous messages, origin checks, validation, cancellation routing |
| ProjectService | Draft configuration, profiles, revisions, local source mappings |
| ManifestService | Enumeration, exclusions, conflicts, final source manifest |
| TorrentEngine | Layout, hashing, v1/v2/hybrid generation, local data verification |
| MetainfoService | Bencode parsing, raw-byte preservation, registry validation, editing, identifiers |
| DiagnosticsService | DNS, UDP tracker, HTTP tracker, and web-seed checks |
| JobScheduler | Queue, resource budgets, pause/cancel, progress, terminal status |
| PersistenceService | Atomic output, settings, project files, redacted diagnostics |

The engine MUST NOT depend on WebView2 types. Do not create a networking libtorrent session merely to calculate hashes. If any integration path creates a session, disable DHT, discovery, announces, listening, port mapping, and other unsolicited network activity; verify offline behavior with a network test.

Libtorrent is the preferred protocol implementation. The application still needs a lossless metadata layer: high-level library objects are not assumed to preserve every vendor field or the exact imported bytes.

### 3.1 Early integration proof

Before building the full GUI, prove: v1/v2/hybrid generation; canonical file ordering/padding; a source path differing from its torrent path; Unicode and long paths; progress and cancellation; one-pass hybrid payload reading; and outer-only editing with identical raw `info` bytes.

The ordinary `set_piece_hashes` convenience API expects files under a common base path. For virtual mappings, use a tested custom disk/source adapter or feed hashes through supported builder APIs. Own all asynchronous lifetimes. Do not treat a progress callback as proof of cancellation support, nor rely on exceptions escaping worker callbacks unless the pinned implementation explicitly supports that behavior. [L2, L3]

## 4. GUI and user workflow

### 4.1 Main workflow

1. Add sources or open an existing `.torrent`.
2. Review the actual included file tree and destination paths.
3. Select a profile and inspect any changed settings.
4. Configure trackers, web seeds, and metadata as needed.
5. Resolve validation errors and choose an output path.
6. Create or save; inspect the result, identifiers, and warnings.

Creation MUST remain available when every tracker check times out. Metadata validity and tracker availability are separate concerns.

| Tab | Required controls |
| --- | --- |
| Files | Native add dialogs, drop support, recursive toggle per folder, virtual tree, source/destination columns, filters, exclusion reason, totals |
| General | Torrent name, format, profile, piece size, private flag, output path, creation-date policy |
| Trackers | Enabled rows, exact URL, tier, source, protocol result, latency, last check; import, paste, reorder, check, update catalog |
| Web Seeds | Type, URL, resolved-path preview, checked scope, range support, errors, last check |
| Metadata | Comment, creator, source, optional fields, extension support labels |
| Expert | Lazy bencode tree, text/hex view, typed values, field placement, old/new identifiers, validation |
| Jobs | State, phase, bytes read, speed, ETA, current file, pause/resume/cancel, redacted log |

Simple mode shows Sources, Name, Profile, Destination, and Create. Advanced mode exposes all tabs. Switching modes MUST NOT discard hidden values. Before creation show active advanced settings in a compact review summary.

Display payload bytes, logical/padded bytes, real file count, synthetic padding count, piece length, piece count, and estimated metainfo size separately. Size units are binary KiB/MiB/GiB/TiB; retain exact byte values in details.

Use virtualized file and tracker lists. No DOM node per file for a 100,000-file dataset. Keyboard navigation, visible focus, DPI scaling, accessible labels, and light/dark/system themes are required. Keep text in localization resources; ship English and Russian UI, with unchanged protocol field names and URLs.

### 4.2 Default profiles

| Profile | Format | Discovery and other defaults |
| --- | --- | --- |
| Public | Hybrid | Ten built-in trackers, one tier per independent endpoint; private omitted; nodes empty |
| Maximum compatibility | v1 | Same public tracker set; no optional file attributes |
| Private tracker | v1 initially | `private=1`; only user-configured authorized trackers; source configurable; web seeds disabled until explicitly allowed |
| Trackerless | Hybrid | No announce fields; optional user-provided DHT nodes; private omitted |
| Custom | User-selected | Explicit values, saved as a named profile |

All profiles use automatic piece size. Source and comment are empty. Optional timestamps/attributes are off, except the outer creation date which defaults to current UTC. Imported torrents do not acquire a default tracker list or other generated fields merely by opening them.

Switching profiles is an undoable draft change. Show the affected fields before applying it; never silently overwrite a user's private tracker URL or passkey.

### 4.3 Batch creation and external handoff

Offer three explicit modes: one torrent for the complete selection, one torrent per selected file, and one torrent per immediate child folder. Recursion inside each child folder follows its own selection setting; do not recursively create a torrent for every nested directory unless a later option explicitly requests it.

Preview the planned job names, source roots, output paths, and format for every batch item. Snapshot the selected profile into each queued job so a later profile edit does not change work already queued. Resolve duplicate output names before starting; skip/replace/rename is an explicit per-item or batch-wide choice. One job failure does not corrupt or overwrite other outputs. Show a final per-item report.

The result screen offers Save magnet, Show in Explorer, and Open in default torrent client. Client launch is opt-in. Use the saved `.torrent` through the Windows file association with safely separated arguments; do not construct a shell command from torrent names. Launching a client is not proof that it found the payload or started seeding. Show the required local directory layout, particularly for virtual collections.

## 5. Sources, manifest, and Windows behavior

### 5.1 Source mapping

Maintain an explicit mapping from native source handles/paths to torrent-relative path components. Paths in `.torrent` MUST NOT contain drive letters, UNC prefixes, absolute local paths, or local user-profile directories.

- A selected file normally produces single-file mode with its basename.
- A selected folder normally uses that folder name as the torrent root and preserves relative descendants.
- Non-recursive mode includes only directly contained files.
- Multiple unrelated sources use a user-visible virtual root. Show and resolve destination collisions before hashing.
- Destination renaming changes torrent metadata only; never rename or move source files automatically.
- Serialize the library's canonical v2/hybrid layout rather than manually prepending `name` twice. V2 `name` is advisory and its actual file tree also matters.
- A virtual collection may not correspond to one existing seed directory. Explain the mapping in the final result; “Open in client” cannot promise automatic seeding from unrelated directories.

Freeze a manifest revision before hashing. Each entry records: source ID, canonical native path, torrent-relative components, expected length, file identity when available, last-write/change observations, flags, and inclusion reason. Treat missing identity information on a filesystem as a capability limitation.

### 5.2 Filesystem policy

Use Unicode Windows APIs and opt into long-path handling. Support ordinary local files and UNC sources. Validate lengths and offsets with checked 64-bit arithmetic and also check narrower limits in the pinned engine.

- Include hidden/system ordinary files unless an explicit visible exclusion rule says otherwise.
- Default exclusions for application outputs, temporary files, and project files MUST be visible in the preview. Do not use a broad rule that silently excludes every existing `.torrent` payload file.
- Exclude the exact current output/temp paths and detect output-versus-source identity conflicts, including hard-link aliases.
- Do not follow directory junctions or symbolic links by default. List skipped reparse points with reasons. Advanced traversal, if enabled, needs identity-based cycle detection, root-boundary checks, and a depth limit.
- OneDrive and other cloud placeholders are not simply synonymous with symlinks. Classify their reparse tags; identify files that require hydration. Do not trigger a large cloud download during a harmless preview. Hydration requires the user's selected policy.
- Hard links remain separate torrent entries if their destination paths differ. Do not deduplicate away user-visible files.
- Sparse/compressed files contribute their logical bytes. Do not serialize NTFS storage layout, ACLs, or alternate data streams as ordinary torrent content.
- Empty files are supported inside a non-empty dataset. Empty directories are not promised to survive torrent transfer. Show this limitation.
- For the initial libtorrent integration, reject a dataset with no files or total payload length zero with a specific message. This is a product/engine limitation, not a blanket claim about every possible BitTorrent implementation. [L2]
- Detect Windows destination conflicts: case-insensitive collisions, reserved device names, trailing dots/spaces, invalid characters, and file-versus-directory collisions.
- Do not normalize Unicode or “fix” names silently. Offer an explicit rename in the draft and show the resulting torrent path.

Unreadable selected files block creation until the user excludes them or resolves the error. Do not silently omit them and report complete success.

## 6. Torrent formats, layout, and piece size

### 6.1 Protocol requirements

Implement BEP 3, BEP 52, and hybrid compatibility using the pinned engine and independent verification. [B3, B52]

- V1 uses SHA-1 hashes over the ordered logical byte stream; a piece may cross real-file boundaries. The final piece may be short.
- V2 uses SHA-256 Merkle trees based on 16 KiB data blocks. A file's `pieces root` is not generally `SHA256(whole_file)`. Empty files omit that root. Missing leaves used to balance a tree are zero hashes, not hashes of imaginary zero-filled payload blocks.
- Hybrid must describe matching real files and order in both representations, with BEP 47 padding where required. Virtual padding feeds the v1 logical stream but is not read from disk or created beside user files.
- Keep payload progress distinct from logical padding processed. Honor the engine's tested tail-padding policy consistently; do not add “helpful” padding after serialization.
- Validate v2 `piece layers` against the corresponding roots, even though layers are outside `info`. Generate the required dictionary; an empty dictionary is acceptable when no layer entries are needed. Shared roots must not create duplicate dictionary keys.
- Do not offer a generic v1-to-v2 conversion without payload data or a trustworthy existing v2 hash source.

### 6.2 Automatic piece-size policy

This is an application policy, not a BitTorrent requirement. Use powers of two. The normal manual range is 16 KiB through 16 MiB; an expert range may extend to the pinned engine's tested limit. The reviewed API documents a 128 MiB upper limit; do not assume arbitrary powers of two are accepted. [L2]

Auto policy version 1:

1. Evaluate candidates from 256 KiB through 16 MiB, in ascending order.
2. Build or simulate the actual format-specific layout, including hybrid padding, for each candidate.
3. Prefer the smallest candidate with at most 32,768 logical pieces and at most 8 MiB of estimated hash payload. These are soft targets.
4. If none qualifies, select the largest normal candidate and show the exceeded targets. Never loop indefinitely trying to satisfy a target made impossible by huge numbers of small files.
5. If virtual padding exceeds 10% of payload size, show a prominent estimate and offer smaller pieces or v1. Never silently change the chosen format.
6. Validate memory, file count, index, and total-length limits before reading payloads. Allow larger supported jobs only after the user explicitly accepts the estimated resource use.

For pure v1 without padding, hash-array bytes equal `20 * ceil(payload_bytes / piece_length)`. For hybrid use the actual padded logical stream. V2 estimates must count file-specific layers and path metadata; do not reuse the v1 formula.

Example: 1 TiB of pure-v1 payload at 1 MiB pieces requires 20 MiB for `pieces` alone. A soft estimate is not the final file size. Save the resolved numeric piece size and policy version in the project; regeneration does not silently change it after an application update.

## 7. Metadata registry and expert editor

### 7.1 Support levels

- **Form:** normal or advanced typed control with application validation.
- **Computed:** generated from sources; read-only in ordinary forms.
- **Expert:** typed bencode editing, preservation, and a compatibility label; no implied downstream client support.
- **Preserve:** retain on import, inspect, and explicitly remove if requested; no first-release workflow to produce or use the extension.

Maintain descriptors with byte key, permitted placement, bencode type, known format applicability, support level, hash-impact rule, source reference, and validation behavior. Unknown keys remain representable.

### 7.2 Core and common fields

`top` denotes the outer dictionary. `file` denotes a v1 file entry or the appropriate v2 file-property dictionary; placement must follow the relevant format.

| Key | Placement / type | Support and rules |
| --- | --- | --- |
| `info` | top / dictionary | Computed or raw-preserved; never replace during an outer-only edit |
| `announce` | top / byte string URL | Form; absent in trackerless mode |
| `announce-list` | top / list of lists of URLs | Form; non-empty tiers; preserve imported grouping |
| `url-list` | top / string or list of strings | Form; BEP 19; preserve legal imported shape; generate a list |
| `httpseeds` | top / list of strings | Form; BEP 17; legacy/client-dependent |
| `nodes` | top / list of `[host, port]` lists | Advanced form; not trackers; no hardcoded automatic DHT-router insertion |
| `comment` | top / UTF-8 string | Form; optional |
| `created by` | top / UTF-8 string | Form; optional application/version string |
| `creation date` | top / integer | Form; Unix seconds, shown in local time; current/fixed/omit modes |
| `name` | info / UTF-8 string | Form at creation; may affect paths and identifiers |
| `piece length` | info / integer | Form setting, resolved before hashing |
| `pieces` | info / binary string | Computed v1 SHA-1 sequence |
| `length` | info or file / integer | Computed; non-negative; v1 single-file or per-file length |
| `files` | info / list of dictionaries | Computed v1 multifile layout; exclusive with v1 top-level-in-info `length` |
| `path` | v1 file / list of strings | Computed from the approved destination mapping |
| `private` | info / integer | Form; create `1` for private and omit for public; preserve imported `0` in outer-only edits |
| `meta version` | info / integer | Computed; `2` for v2/hybrid; not `1` for v1 |
| `file tree` | info / nested dictionaries | Computed v2 tree; empty byte-string key denotes file properties |
| `pieces root` | v2 file properties / 32 bytes | Computed for non-empty files |
| `piece layers` | top / dictionary of binary keys and values | Computed/preserved and validated against file roots |

References: [B3, B5, B12, B17, B19, B27, B52].

### 7.3 Optional and non-universal fields

| Key or family | Placement | First-release behavior |
| --- | --- | --- |
| `source` | info by explicit profile convention | Advanced form; nonstandard; changes infohash; do not invent tracker-specific values |
| `attr` | file, or single-file info where applicable | Advanced attributes; BEP 47; synthetic `p` belongs to the engine; do not derive Unix executable status merely from `.exe` |
| `sha1` | file, or single-file info where applicable | Optional computed whole-file SHA-1 hint; not a replacement for piece validation |
| `symlink path` with `attr=l` | format-specific file entry | Preserve/Expert; dedicated link-creation workflow is later |
| `mtime` | engine/client-specific file entry | Advanced opt-in only after testing exact serialization; default omitted |
| `collections`, `similar` | top and/or info under BEP 38 | Advanced form; top placement for new entries by default; preserve imported placement; binary infohashes are not UTF-8 |
| `signatures` | top / nested dictionaries | Preserve/Expert; includes signer `certificate`, `info`, `signature`; signing workflow later |
| `update-url`, `originator` | info under BEP 39 | Expert/Preserve; no automatic network fetch or updater |
| `ssl-cert` | libtorrent-specific info extension | Expert/Preserve; client-dependent SSL torrent certificate, not the HTTPS tracker certificate |
| `root hash` | info under BEP 30 | Legacy Preserve; not BEP 52 `pieces root`; no new BEP 30 generation |
| `encoding`, `publisher`, `publisher-url` | imported/vendor-specific, commonly top | Expert; preserve exact placement; no universal behavior claim |
| `md5sum`, extra checksum fields | imported/vendor-specific file locations | Expert; no substitution for mandatory piece hashes |
| `name.utf-8`, `path.utf-8`, `comment.utf-8`, related variants | imported/vendor-specific | Preserve; do not silently reconcile conflicting duplicate naming schemes |
| `azureus_properties`, other vendor dictionaries | wherever imported | Preserve/Expert; semantics may be unknown |
| Arbitrary keys | selected dictionary | Expert typed values; clearly marked unrecognized |

References: [B30, B35, B38, B39, B47, L2]. This table catalogs supported handling, not a claim that all listed extensions are universally standardized or supported by clients. New known-key descriptors require source evidence and fixtures.

### 7.4 Binary-safe representation

Bencode has byte strings, integers, lists, and dictionaries. It has no JSON boolean, null, float, or native timestamp type. Treat dictionary keys as byte strings too. Use tagged byte values and decimal integer strings when crossing the JavaScript boundary. Display text only after a successful decode; keep original bytes independently.

Unknown integers may exceed 64-bit range: retain their canonical decimal representation without narrowing. Protocol values used for sizes and counts must separately pass range checks. Lists preserve order; generated dictionary keys sort lexicographically by raw bytes.

Reject duplicate keys, invalid integers, excessive nesting, truncated strings, overflow, and ambiguous structures for normal save. Malformed input may be inspected in a bounded read-only view; repair is an explicit operation, never an automatic normalize-and-save. Unknown future `meta version` values must produce “unsupported format,” not a misleading generic corruption error.

Structural hash/layout fields are not freely replaceable through a generic “Save valid torrent” action. Changes require a coherent rebuild or a supported, fully validated import of that structure. Do not provide an easy path to exporting deliberately inconsistent metainfo as a successful torrent.

## 8. Identity, editing, and reproducibility

Compute v1 infohash as SHA-1 of the exact bencoded `info` byte sequence, and v2 infohash as SHA-256 of that sequence for v2/hybrid. For hybrid, both algorithms cover the same complete `info` dictionary, including its v2 keys. Never strip keys to synthesize a supposed v1 identity. Show the full 32-byte v2 value; a 20-byte protocol truncation is not the full v2 identifier. [B3, B52]

| Edit | Identity / processing policy |
| --- | --- |
| Outer tracker URLs, web seeds, comment, creator, date, nodes | Preserve raw `info`; no payload hashing |
| `source`, `private`, another non-layout info key | New identifiers; piece hashes may be reused; no payload reread is inherently required |
| Name/path/order/format/piece-size/file selection | Treat as a rebuild; determine exactly which hashes/layout are invalidated; first release may conservatively require sources |
| `piece layers` | Identifier can remain unchanged but v2 validity can break; validate and protect as a computed field |
| Signature-related metadata | Check signed scope; remove or regenerate affected signatures explicitly; never label stale signatures valid |

For an outer-only edit, preserve the exact raw `info` slice, unknown values, binary keys, layer data, and unaffected signatures. Reopen the saved file and compare both raw `info` and applicable identifiers. Do not rely solely on rebuilding a high-level torrent object and hoping it retains all metadata.

Before an identity-changing save, show old/new identifiers and save a new file by default. Existing source `.torrent` files are not overwritten unless selected explicitly.

Reproducible mode fixes/omits creation date and optional mtimes, fixes creator text, ordering, padding policy, profile values, and piece size. Record the engine version. Same payload, same configuration, and the same pinned implementation must yield identical bytes; cross-version byte identity is not promised.

Public magnet export supports v1 `xt=urn:btih:<40 hex>`, v2 `xt=urn:btmh:1220<64 hex>`, and both exact topics for the same hybrid torrent. Encode URL parameters once. Magnets cannot preserve all metainfo, tier grouping, or outer extensions. Private-profile sharing defaults to `.torrent`; magnet export requires an explicit choice and must not expose passkeys unnoticed. [B9]

## 9. Hashing, consistency, and resource control

### 9.1 Processing pipeline

`Enumerate -> Validate manifest -> Resolve layout -> Read/hash -> Build metainfo -> Validate output -> Commit`

All payload bytes must be processed for initial creation. Sampling, file size, and filename equality cannot replace torrent hashes. Use bounded native buffers and a worker pool. Stream real data once through the required hash computations for hybrid; generate padding in memory. Report physical payload reading separately from synthetic work.

Default execution policy:

- One active creation job, with additional jobs queued. An advanced setting may allow two when they read independent devices.
- Conservative sequential access for rotational or unknown storage. Tune SSD concurrency by measurement, not by launching a thread per file.
- Initial payload-buffer budget: 128 MiB per active job; configurable with validation. This is not a cap on total process memory.
- Hash metadata and manifests are separately estimated. Stream/spool where supported; do not promise constant memory when the engine retains a complete hash table.
- Start with at most four hash workers, bounded by available processors and the memory budget. Keep the scheduling policy configurable.
- Coalesce UI progress to at most five messages per second per job. Send only deltas and aggregates; use pages for file details.
- Local regular-file cancellation should normally finish within two seconds. Network/storage stalls can exceed that; display Cancelling and keep the UI responsive.
- Use cancellable I/O where supported. Cancellation is a request: wait for completion before freeing an outstanding buffer or handle. Never kill an arbitrary hashing thread. [W6]

Track bytes actually processed, current phase, per-phase timing, throughput, estimated remaining time, and read failures. ETA remains unknown until enough progress exists. Avoid presenting virtual-padding throughput as disk speed.

### 9.2 Source consistency policy

The first release provides **best-effort change detection**, not an atomic snapshot guarantee. Document that users should create torrents from stable, completed files.

1. Capture each manifest entry's identity, size, and available timestamps.
2. Before reading, reopen/validate the expected file. Where feasible, acquire a read handle that does not share write/delete access. A sharing violation is an actionable error, not permission to silently hash concurrently changing data.
3. Read from the verified object; validate again after reading.
4. Recheck the complete selected manifest before committing. Changed/replaced/missing entries block a normal successful result and require a retry or an explicit new draft.
5. Files newly created after manifest freeze are not automatically added; report detected directory changes without mutating the active job.
6. Release handles reliably on completion, cancellation, and failure.

A per-file lock released after hashing does not freeze the whole dataset. Matching metadata does not prove identical bytes. Ordinary share restrictions also must not be advertised as a defense against every possible memory-mapped or privileged write. Windows sharing behavior and filesystem capabilities must be tested. [W5]

A later strict mode may require an immutable input snapshot, or retain appropriate locks for the entire selected set and job lifetime with documented filesystem limits. Never label a sequential per-file lock strategy as a complete snapshot. Do not introduce an administrator requirement or silently create VSS snapshots in release 1.

### 9.3 Pause, restart, cache, and local verification

Pause stops scheduling new reads and drains in-flight operations to a safe boundary. Resume revalidates any source whose protective handle was released. Avoid serializing opaque hash-library state across application versions.

Saving a project is not saving trusted completed hashes. Interrupted jobs can restore configuration but restart hashing in release 1. Future cache keys must include content assurance and the relevant layout/piece-size/algorithm parameters; path, length, and mtime alone are insufficient.

Local verification has two distinct levels:

- **Metainfo validation:** structure, counts, identifiers, and v2 layer/root consistency. Does not reread payloads.
- **Full payload verification:** read all mapped source bytes and compare required torrent hashes. For hybrid validate both representations. Padding is synthetic, not a required on-disk file.

Post-creation validation always includes reopening the small output file. A second full payload pass is optional and labelled with its I/O cost; it is not silently performed on every creation.

## 10. Tracker catalog and private-mode policy

### 10.1 Built-in seed list

These exact addresses were present in the upstream `trackers_best.txt` list consulted on **2026-10-03**. The repository states that its bot checks trackers and updates the lists daily. This handoff does not claim an independent successful announce or reachability from the user's network. Store source provenance separately from local observations. [T1, T2]

```text
udp://tracker.opentrackr.org:1337/announce
udp://open.stealth.si:80/announce
udp://tracker.torrent.eu.org:451/announce
udp://open.demonii.com:1337/announce
udp://exodus.desync.com:6969/announce
http://tracker.qu.ax:6969/announce
udp://tracker.skynetcloud.site:6969/announce
udp://tracker.tryhackx.org:6969/announce
udp://tracker.gmi.gd:6969/announce
udp://explodie.org:6969/announce
```

Initial `locallyCheckedAt` is absent and state is Unchecked. An imported source timestamp is never a fabricated local test timestamp. Reachability changes over time and between networks.

### 10.2 Tiers and URL identity

For the built-in public preset, create one tier per independent endpoint and set `announce` to the first enabled URL. Within a tier, order is not a guaranteed fixed priority. Different clients can have additional announce policies; adding ten tiers does not command all clients to contact ten services simultaneously. Preserve existing grouping when importing. [B12]

Deduplicate exact equivalent endpoints conservatively. Never merge merely because DNS resolves to the same IP: multiple services, ports, paths, and authenticated URLs can share an address. Preserve path case, query parameters, passkeys, and deliberate protocol variants. Do not append `/announce` to an arbitrary user URL.

Support UDP, HTTP, and HTTPS for active checks. Preserve unfamiliar schemes as unsupported diagnostic entries rather than pretending they are healthy. Distinguish a tracker URL from a BEP 5 DHT node; do not automatically insert a popular DHT bootstrap hostname. [B5]

### 10.3 Catalog updates

Update only on an explicit action, or a separately enabled periodic preference. Retrieve the configured HTTPS text source, with response-size/line-count limits and URL validation. Keep the last valid catalog on failure. A reasonable default is at most one automatic update attempt per 24 hours while the application is running.

Show additions/removals before applying them to a draft. Preserve custom addresses, disabled entries, and tiers. Existing saved torrents never change during catalog refresh. Keep source date, fetch time, source URL, and a checksum of the fetched catalog. Do not silently replace domain names with resolved IPs.

The upstream repository identifies a GPL-2.0 license. Track catalog provenance and assess notices/redistribution terms before shipping copied catalog material; do not assume every fetched repository asset is license-free. Do not copy its tooling into the application unnecessarily. [T1]

### 10.4 Private torrents

Generate `info.private=1`, require at least one explicitly configured tracker, suppress public presets and DHT nodes, and disable automatic addition of public trackers. Private means tracker-restricted peer discovery, not encryption or a password. It is not access control over the `.torrent` file itself. [B27]

When switching a draft to private mode, clearly show the incompatible fields to be removed. Do not silently rewrite a private imported torrent on open. Imported incompatible fields are validation issues for the user to resolve.

Do not fetch tracker-specific rules or invent a `source` value. A profile can constrain format, pieces, web seeds, and extra fields. Web seeds in private profiles require an explicit profile allowance, not an assumption that BEP 27 universally forbids all HTTP data sources.

DHT/PEX/LSD switches, peer limits, and upload/download speed limits are not general standalone metainfo fields. Do not serialize invented keys that claim to control another client's settings.

## 11. Tracker diagnostics

### 11.1 Goals and result model

Check from the user's machine, through the selected network/proxy policy. The result says what was observed at that time; it does not guarantee global uptime, acceptance of a particular torrent, or available seeds.

Store separate observations for DNS, address family, transport, protocol operation, authorization, latency, and last-check time. Aggregate without discarding per-family results. Suggested visible states:

| State | Meaning |
| --- | --- |
| Unchecked | No local observation |
| Protocol responding | Valid response to the specific implemented probe |
| Responding, restricted | Valid protocol error indicating authorization/policy rejection |
| Scrape unsupported | Cannot obtain statistics by this method; not proof of tracker outage |
| No response | Deadline elapsed; cause unknown |
| DNS / TLS / transport error | Concrete failure category with redacted details |
| Invalid response | Wrong transaction, malformed bencode, HTML, or other protocol mismatch |
| Probe unsupported | Scheme/proxy/network combination not implemented |

Do not translate a zero peer count into Down. Do not rank expected torrent download speed by tracker RTT. Keep “accepts this torrent” Unknown unless an actual authorized announce was performed.

### 11.2 UDP

Use the BEP 15 connect handshake with a random transaction ID. Check the sender endpoint, minimum response size, action, and matching transaction ID. Preserve allowance for protocol-compatible extra bytes. A connect response tests the UDP service, not arbitrary URL-path/passkey semantics. [B15]

Default quick probe: one connect request with a 15-second observation window. A full retry mode may retry once at 15 seconds and wait a further 30 seconds. These finite diagnostic budgets do not claim to run the full tracker client's retry schedule. No rapid-fire retries.

Optional scrape uses the acquired connection ID and a random diagnostic 20-byte hash, not the draft's real hash. Respect connection-ID lifetime and correlate the reply. Never send announce as a side effect of the Check button. BEP 41 path/query behavior, where applicable, requires explicit implementation; a bare connect response is not evidence that authenticated URL options were accepted. [B15, B41]

BEP 41 URLData is an announce-request extension; do not append it to connect or scrape packets. For a private UDP endpoint requiring it, release-1 connect/scrape diagnostics can only report the narrower operation they actually performed.

### 11.3 HTTP/HTTPS

Check DNS/TCP/TLS and use bounded responses. A 200 response alone is not success. Parse the expected bencoded structure. A valid tracker error means the service responded; it may still reject the supplied credentials or operation.

When an announce path permits the BEP 48 transformation, a scrape probe may use a random diagnostic hash. Never fetch a full tracker scrape accidentally. Keep existing URL authentication/query semantics intact. If scrape is unsupported, an explicitly labelled endpoint probe can request the announce endpoint without identifying a real swarm; a protocol error in response establishes liveness only. Some servers/WAFs will not allow this, which is inconclusive. [B3, B48]

An actual announce test is outside the default first-release diagnostic path. Do not invent a fake peer or register the user with the real torrent to obtain a green icon. Future implementation must clearly describe that it contacts the tracker about that torrent, and must implement the required lifecycle.

### 11.4 Common network behavior

- At most four endpoint probes concurrently by default; deduplicate simultaneous requests to the same endpoint.
- HTTP total deadline defaults to 15 seconds; allow cancellation. Apply limits to decompressed as well as compressed responses.
- Cache observations briefly, initially ten minutes, while retaining their timestamp and network/proxy context. Manual refresh can bypass the cache.
- Test IPv4/IPv6 when available; “IPv6 unavailable locally” is not a tracker failure. DNS ordering must not permanently exclude the other family.
- Support direct connections and an explicit HTTP proxy for HTTP/HTTPS in release 1. If a proxy policy is selected but UDP proxying is unavailable, mark UDP probes unsupported. Never fall back to direct traffic or local DNS contrary to the configured policy. SOCKS5 UDP can be added later.
- Validate HTTPS certificates with the platform trust model. Do not globally disable verification to make diagnostics green.
- Do not forward credentials to a different origin after a redirect. Limit redirects and revalidate destination policy on each hop.
- Do not contact URLs just because a torrent was opened. Explicit LAN endpoints can be checked, but imported/public endpoints must not silently redirect into unrelated loopback/private services.
- Redact secrets in URLs, including path-based passkeys. Safe logging uses endpoint labels/hostnames and error categories, not raw authenticated URLs.

## 12. Web seeds

### 12.1 Metadata and path resolution

Treat BEP 19 `url-list` and BEP 17 `httpseeds` as different types. A BEP 19 seed serves ordinary files; a BEP 17 endpoint understands torrent-piece requests. Do not silently move a URL from one field to the other. [B19, B17]

For a v1/hybrid multifile torrent named `Release`, a BEP 19 base `https://example.org/downloads/` and relative file `bin/app.exe` resolve to `https://example.org/downloads/Release/bin/app.exe`. Show this preview. In single-file mode distinguish a direct file URL from a directory URL ending in `/`.

For pure v2, derive the effective paths from the engine's tested file-tree interpretation. Do not assume `info.name` is always another path component. Integration fixtures must catch doubled or missing roots.

Use standards-aware URL construction with each path component encoded exactly once. Preserve query strings and signed-URL semantics; a direct signed URL is not automatically a usable multi-file base. Reject fragments for generated network endpoints. Never lowercase URL paths.

First-release diagnostics actively support HTTP/HTTPS BEP 19. Preserve FTP and other schemes with an Unsupported probe label. BEP 17 metadata is supported for v1/hybrid; pure-v2 creation with BEP 17 is rejected in ordinary forms unless a tested client-specific profile is added. Generic preservation does not claim interoperable pure-v2 support.

### 12.2 Probe levels

1. **Address and transport:** exact resolved file URL, DNS, TLS, redirections, status.
2. **Length/range behavior:** use HEAD as an optional hint, then a small GET with `Range` and `Accept-Encoding: identity`. Actual 206 plus coherent `Content-Range` establishes range behavior. A ranged response's `Content-Length` is the returned slice size, not the entire file size.
3. **Sample comparison:** optional bounded downloaded bytes compared with local bytes; label this as partial evidence.
4. **Integrity verification:** fetch and verify complete applicable torrent pieces, or complete files/valid Merkle proofs for v2. Random byte samples cannot establish full-file or full-torrent integrity.

Handle HEAD rejection, zero-length files, 416 range responses, servers ignoring Range, compressed content, authorization failures, redirects, and expiring signed URLs. If a server returns the entire file to a tiny range request, stop after the configured byte cap; do not accidentally download a multi-terabyte payload.

Default checks sample at most three representative files per base URL, including a nested path where available, with a 1 MiB total body cap per base. Display “3 of N files checked.” Full coverage requires an explicit choice and shows the estimated traffic first. Virtual padding files must never be requested from a BEP 19 server.

BEP 17 semantic checking requires known piece layout and potentially the real infohash. It must be a separate explicit action, clearly marked as contacting the seed about that torrent. For first release, a generic transport check alone must not be called a successful BEP 17 data-integrity test.

## 13. WebView2 host and bridge

Bundle all frontend assets locally; no CDN dependencies, remote scripts, login, or development server at runtime. Use a fixed application origin mapped only to the installed frontend asset directory. Do not expose the source-file directory as the web asset root. [W1, W3]

Create WebView2 on the required STA UI thread with its message loop. Marshal UI interactions onto that thread; never block it waiting for hashing, network results, or synchronous frontend round trips. [W4]

### 13.1 Message contract

Messages have protocol version, request ID, operation, draft/job revision, and a schema-validated payload. Responses correlate by request ID; job events include a monotonic sequence number. Ignore stale results after an edit, cancelled probe, or renderer reload.

Representative commands: `selectSources`, `scanSources`, `getManifestPage`, `updateDraft`, `validateDraft`, `startCreate`, `pauseJob`, `resumeJob`, `cancelJob`, `checkTrackers`, `checkWebSeeds`, `openTorrent`, `saveTorrent`, `verifyPayload`, `exportMagnet`.

```json
{
  "protocolVersion": 1,
  "requestId": "req-42",
  "operation": "getManifestPage",
  "draftRevision": "7",
  "payload": { "sourceSetId": "set-3", "offset": 0, "limit": 250 }
}
```

Use native-owned source IDs for privileged operations, not arbitrary path strings supplied by a web page. File lengths, byte offsets, timestamps with large range, and unknown bencode integers cross the bridge as decimal strings when outside the safe numeric domain. Binary values use a tagged hex/base64 representation. Limit ordinary messages to 1 MiB and page large values.

Use a JSON serializer for messages. Never interpolate filenames, comments, or error strings into executable JavaScript. Validate the sender origin, frame context, operation name, payload shape, bounds, and project state. No generic execute-shell, arbitrary file-read, or unrestricted host-object bridge. [W2]

### 13.2 Content and recovery

Allow navigation only to bundled application content. Open user-selected external links through the system browser after scheme validation. Render torrent strings as text, not HTML; apply a restrictive content policy. Block unsolicited new windows, permissions, and downloads.

The native job state is authoritative. If a renderer fails, hashing can continue when the native host is healthy. Recreate the view and request a fresh snapshot with current revisions; do not replay a previous Create command. Closing the host during a job offers cancel-and-exit or keep-running behavior. Never detach an invisible job without a visible control path.

## 14. Persistence, output, and errors

Store local configuration under the user's application-data location. Project files are versioned JSON containing configuration, source mappings, resolved piece-size policy, profile references, and UI-independent draft state. They are not `.torrent` files and must never be included in the metainfo dictionary.

Secrets saved locally should use Windows-protected storage where appropriate. Exported profiles are redacted by default. A final private `.torrent` may intentionally contain a tracker passkey: masking it in the UI does not remove it from the file. Show that fact in the export summary without logging the value.

### 14.1 Output commit

1. Validate output destination and reject any source-file identity collision.
2. Reserve the intended output name; require an explicit replace choice for an existing file.
3. Write to a unique temporary file on the same destination volume/directory when possible.
4. Flush and close it; reopen and validate its metainfo and applicable identifiers.
5. Commit using the appropriate Windows same-volume rename/replace operation. Handle a destination created by another process during the job.
6. Publish Succeeded only after commit. Keep the previous output intact on failure wherever the filesystem permits.

Do not claim universal atomicity or power-loss durability on every remote filesystem. Report a weaker destination guarantee when applicable. Clean up only application-owned temporary files; never delete source files during recovery.

### 14.2 States and errors

Job states: Draft, Scanning, Ready, Queued, Hashing, Pausing, Paused, Cancelling, Validating, Committing, Succeeded, SucceededWithWarnings, Failed, Cancelled. Only valid transitions are accepted by the native service. Disallow cancellation once an atomic replacement is in its non-interruptible commit step and explain that short phase.

Errors include a stable code, phase, source/endpoint ID, safe message, retryability, and optional OS code. Examples: `SOURCE_CHANGED`, `SOURCE_MISSING`, `SOURCE_UNREADABLE`, `PATH_COLLISION`, `UNSUPPORTED_FORMAT`, `INVALID_METAINFO`, `RESOURCE_LIMIT`, `TRACKER_TIMEOUT`, `PROXY_UNSUPPORTED`, `WEBSEED_RANGE_UNSUPPORTED`, `OUTPUT_CONFLICT`, `OUTPUT_WRITE_FAILED`, `RUNTIME_UNAVAILABLE`.

Warn-only outcomes must be distinct from failed content generation. Tracker timeout can coexist with successful creation; a missing selected payload file cannot.

## 15. Parser limits and release performance targets

All imported `.torrent`, project, profile, and catalog files are untrusted input. Bound allocations before reading declared lengths; use checked arithmetic. Do not follow local paths embedded in metainfo without the user mapping them to a selected source root.

Initial application limits, adjustable after evidence:

| Resource | Default policy |
| --- | --- |
| Imported metainfo bytes | 64 MiB normal limit; explicit advanced override subject to estimated memory |
| Bencode nesting | 128 levels; fail with a clear limit error |
| Decoded nodes | 2,000,000 initial safety limit; include model overhead in estimates |
| Creation manifest | Warn above 100,000 real files; initially block above 1,000,000 unless a documented tested limit is raised |
| Tracker catalog response | 1 MiB and 5,000 entries maximum |
| HTTP tracker response | 1 MiB maximum after decompression |
| Ordinary bridge message | 1 MiB maximum; pagination for larger data |
| Planned metainfo/engine memory | Warn above 512 MiB estimated; block before allocation if the checked budget cannot support it |

These are application safeguards, not claimed protocol maxima. A cap change must be tested against the engine's internal indices and parser limits. Do not allocate one GUI object for every binary hash.

Performance acceptance must record hardware, filesystem, dataset, engine version, warm/cold cache state, total process-tree memory, and payload throughput. First-release targets:

- Responsive controls during enumeration and hashing; ordinary UI actions target under 100 ms and never wait for a disk operation.
- Comfortable browsing of 100,000-file manifests through pagination/virtualization.
- Hybrid reads each ordinary source byte once during the main hash pass under stable-input, no-retry conditions; verify using an instrumented reader.
- No payload-sized allocation; buffer use remains within its configured budget.
- Compare native engine throughput with and without GUI attached. Investigate sustained regression greater than 10% under the same controlled workload.
- Report whole process-tree memory, including WebView2; do not advertise host-process RSS as total application usage.

No fixed MB/s, startup time, or total-memory promise is made before measurements.

## 16. Packaging, licensing, and updates

Ship a native x64 installer for Windows 11 and Windows 10 22H2 as the initial tested OS matrix. Recheck Microsoft Runtime support and toolchain support when releasing; compatibility with an OS does not imply the OS itself remains generally supported.

Use WebView2 Evergreen by default. Detect presence and the minimum compatible Runtime before constructing the web UI. If missing/too old, show a native explanation with installation options. Do not assume Microsoft Edge browser installation is an interchangeable deployment strategy. [W1]

Provide a normal small installer that can obtain the Runtime and an offline bundle containing its official standalone installer. Document per-user installation behavior. A lightweight portable package can require an already installed Runtime; do not label it fully self-contained. A Fixed Version distribution is later unless explicitly needed, and must include its size and security-update responsibilities.

WebView2 SDK/Runtime require no paid application subscription or end-user account for this design, but their redistribution terms still apply. Libtorrent and all transitive/native/frontend dependencies require notices according to their actual pinned licenses. Maintain `THIRD_PARTY_NOTICES`, an SBOM/dependency manifest, and source/build provenance. Do not claim that an entire dependency tree is covered by one library's license. [W7]

Pin and lock native/frontend packages. Include CMake presets, the frontend production build, clean-machine build instructions, Windows test instructions, and release packaging scripts. Code signing is recommended for distributed releases; do not generate a fake publisher identity. Unsigned development builds must be labelled accurately.

Do not automatically restart the app for a Runtime update during a hashing job. Defer reinitialization until jobs finish or are cancelled. Check API availability rather than assuming every machine has the newest Evergreen version.

## 17. Acceptance tests and evidence

Use deterministic fixtures and mocked tracker/web-seed servers for CI. Public tracker outages must not make CI fail. Public checks belong to optional integration runs with timestamps. Golden results must come from an independent implementation or published vector, not solely the same production encoder used twice.

| ID | Scenario | Required result |
| --- | --- | --- |
| F01 | v1 single file at sizes 1, P-1, P, P+1 | Correct pieces and short-tail handling |
| F02 | v1 files whose boundaries split a piece | Correct concatenated-stream hash |
| F03 | v2 sizes 0, 1, 16383, 16384, 16385, P-1, P, P+1 | Correct roots, absent empty-file root, valid layers |
| F04 | v2 non-power-of-two block count | Correct zero-hash tree completion |
| F05 | hybrid with many tiny files and one large file | Matching layouts, synthetic padding, both hash families valid |
| F06 | repeated identical files in v2 | Valid shared layer keys; no duplicate bencode keys |
| F07 | empty files mixed with non-empty files | Paths retained and verified |
| F08 | empty directory / all-empty dataset | Documented omission or explicit engine-limit error |
| F09 | one file in a folder versus true single-file mode | Intended destination structure and web-seed path |
| F10 | virtual paths from two source volumes | Correct content without copying or renaming sources |
| F11 | auto-piece calculation on many small files | Finite selection, correct padding warning and estimates |
| E01 | edit trackers/comment/web seeds only | Exact `info` bytes and all applicable hashes unchanged |
| E02 | unknown binary keys, byte strings, huge integers | Lossless preservation; lazy display; no JS rounding |
| E03 | duplicate keys, malformed integers, truncated strings, deep input | Bounded rejection; no automatic repair |
| E04 | change source/private inside info | Identifiers change; existing valid payload hashes can be reused |
| E05 | mutate v2 layer outside info | Rejected despite unchanged infohash |
| E06 | imported vendor naming fields/signatures | Preserved, or explicit conflict/invalidation handling |
| E07 | future meta version / BEP 30 input | Unsupported/legacy label; no mistaken v2 generation |
| E08 | deterministic profile, repeated creation | Identical output with pinned implementation |
| E09 | hybrid magnet | Both correct topics; no stripped-info hash; correct escaping |
| W01 | Unicode, long paths, UNC, case collisions | Correct paths or specific actionable error |
| W02 | junction cycle and external-target symlink | No default traversal; explicit exclusion reason |
| W03 | source replaced, truncated, or changed during read | Detected changes block normal success |
| W04 | same metadata after undetectable external mutation | No claim that best-effort mode guarantees a snapshot |
| W05 | cloud placeholder | No surprise hydration during preview |
| W06 | output equals source or its hard-link alias | Creation blocked before writing |
| W07 | disk full, output race, access denied | Previous output retained where possible; no false success |
| W08 | pause/cancel, including stalled UNC I/O | Responsive UI and safe resource cleanup |
| N01 | UDP correct/wrong transaction and sender | Accept only a matching valid response |
| N02 | UDP timeout, retry, expired connection ID | Correct finite state and retry timing |
| N03 | HTTP HTML 200, protocol failure, zero peers | Distinct invalid/restricted/responding outcomes |
| N04 | scrape unsupported / inaccessible IPv6 | Not incorrectly labelled globally down |
| N05 | proxy lacks UDP support | No direct fallback or unintended DNS leak |
| N06 | opening a torrent / clicking ordinary Check | No real-swarm announce, DHT publish, or peer connection |
| N07 | catalog update failure or changed list | Last valid catalog retained; custom profiles untouched |
| S01 | BEP 19 nested paths, spaces, Unicode, signed query | Exact tested URLs, no double encoding/root |
| S02 | HEAD 405, valid 206, ignored Range, 416/empty file | Accurate diagnosis, bounded download |
| S03 | sampled web seed check | Scope clearly stated; not “all files verified” |
| S04 | padding in hybrid | No HTTP request for virtual padding files |
| S05 | BEP 17 versus BEP 19 / pure v2 | No false interchangeability or unsupported integrity claim |
| U01 | malicious comment, filename, or bridge message | Text-only rendering; no script/native-command execution |
| U02 | 100,000 files | Virtualized interaction without massive DOM allocation |
| U03 | renderer crash during hashing | Native state retained; recovery without duplicate job |
| U04 | missing/old Runtime and offline installation | Native bootstrap works and recovery is documented |
| U05 | private profile and exported diagnostics | No public additions and no unredacted passkey |
| U06 | keyboard and 100/150/200% display scaling | All primary actions usable |
| U07 | batch job naming, profile edit, one failed item | Stable queued settings, collision handling, isolated outcomes |
| P01 | stable hybrid creation with instrumented I/O | Single main payload read pass |
| P02 | oversized manifest/piece count/metainfo | Checked preflight failure, not allocation crash |

For interoperability, test v1, v2, and hybrid outputs in at least two independently implemented engines supporting the relevant format. A qBittorrent test plus another libtorrent wrapper is not two independent engines. Record actual versions and features tested; verify content, not just successful loading. Include one older/v1-only compatibility check for a representative hybrid fixture if maximum backward compatibility is claimed.

## 18. Implementation milestones and handoff instructions

| Milestone | Deliverable and exit gate |
| --- | --- |
| M0 — Integration proof | Pinned toolchain; minimal native WebView2 shell; headless format/mapping/cancellation proof from section 3.1 |
| M1 — Correct core | Manifest, engine, parser, identifiers, save/verify; F/E/W core fixtures pass |
| M2 — Usable application | Simple/advanced UI, bridge, queue, pause/cancel, profiles, projects, batch creation |
| M3 — Metadata and diagnostics | Registry, expert preservation/editing, trackers/tiers, catalog, web seeds, private rules; N/S tests pass |
| M4 — Release | Clean-machine packaging, offline Runtime installation, security/parser tests, accessibility, independent interoperability, performance report |

The developer may choose routine implementation details without asking the user to reconfirm WebView2, C++, or libtorrent. Preserve the scope and behavioral requirements. Record any engine limitation that prevents a MUST requirement before implementing a workaround; do not silently replace binary-preserving editing with lossy reconstruction or remove v2/hybrid support.

Expected repository outputs: buildable sources; pinned dependency manifests; production frontend assets/build; Windows packaging; tests and fixtures; `README` with build/run instructions; architecture notes; dependency notices; an acceptance report with measured results and remaining limitations.

The final application must not claim “all trackers working,” “all clients supported,” “snapshot-safe,” or “all metadata preserved” without the corresponding scoped evidence. The preparation of this specification involved documentation review, not an application build or successful live tracker protocol tests.

## 19. Primary references

Links below are the normative/provenance references used for the design review. Reviewed on 2026-10-03. BEP status and client adoption vary; a published BEP is not proof of universal client support. Use the pinned source tree to resolve API differences.

| ID | Source |
| --- | --- |
| B3 | [BEP 3 — BitTorrent protocol and v1 metainfo](https://www.bittorrent.org/beps/bep_0003.html) |
| B5 | [BEP 5 — DHT, trackerless files, and nodes](https://www.bittorrent.org/beps/bep_0005.html) |
| B9 | [BEP 9 — Metadata exchange and v1/v2 magnet identifiers](https://www.bittorrent.org/beps/bep_0009.html) |
| B12 | [BEP 12 — Multitracker metadata and tiers](https://www.bittorrent.org/beps/bep_0012.html) |
| B15 | [BEP 15 — UDP tracker protocol](https://www.bittorrent.org/beps/bep_0015.html) |
| B17 | [BEP 17 — HTTP seeds](https://www.bittorrent.org/beps/bep_0017.html) |
| B19 | [BEP 19 — GetRight-style web seeds](https://www.bittorrent.org/beps/bep_0019.html) |
| B27 | [BEP 27 — Private torrents](https://www.bittorrent.org/beps/bep_0027.html) |
| B30 | [BEP 30 — Legacy Merkle torrent extension](https://www.bittorrent.org/beps/bep_0030.html) |
| B35 | [BEP 35 — Torrent signing](https://www.bittorrent.org/beps/bep_0035.html) |
| B38 | [BEP 38 — Similar torrents and collections](https://www.bittorrent.org/beps/bep_0038.html) |
| B39 | [BEP 39 — Update URL and originator](https://www.bittorrent.org/beps/bep_0039.html) |
| B41 | [BEP 41 — UDP tracker URL extensions](https://www.bittorrent.org/beps/bep_0041.html) |
| B47 | [BEP 47 — Padding and extended file attributes](https://www.bittorrent.org/beps/bep_0047.html) |
| B48 | [BEP 48 — HTTP scrape](https://www.bittorrent.org/beps/bep_0048.html) |
| B52 | [BEP 52 — v2 and hybrid requirements](https://www.bittorrent.org/beps/bep_0052.html) |
| L1 | [libtorrent v2.1.2 release](https://github.com/arvidn/libtorrent/releases/tag/v2.1.2) |
| L2 | [libtorrent creation API](https://libtorrent.org/reference-Create_Torrents.html) |
| L3 | [libtorrent storage API](https://libtorrent.org/reference-Storage.html) |
| W1 | [Microsoft — WebView2 deployment](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution) |
| W2 | [Microsoft — WebView2 security](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/security) |
| W3 | [Microsoft — Local content in WebView2](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/working-with-local-content) |
| W4 | [Microsoft — WebView2 threading model](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/threading-model) |
| W5 | [Microsoft — CreateFileW and sharing semantics](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew) |
| W6 | [Microsoft — CancelIoEx](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex) |
| W7 | [Microsoft WebView2 SDK package and license link](https://www.nuget.org/packages/Microsoft.Web.WebView2) |
| T1 | [ngosang trackerslist — update method, date, and license](https://github.com/ngosang/trackerslist) |
| T2 | [Tracker seed-list source](https://raw.githubusercontent.com/ngosang/trackerslist/master/trackers_best.txt) |

Application defaults, resource budgets, UI choices, milestones, and acceptance thresholds in this document are design decisions. They are not represented as requirements imposed by the cited protocols.
