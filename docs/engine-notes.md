# Engine notes and known limitations

Findings from integrating the pinned libtorrent 2.1.2 (built with
`deprecated-functions=OFF`, `webtorrent=OFF`, so `TORRENT_ABI_VERSION=100`).
The specification asks for engine limitations to be recorded before a
workaround is implemented.

## libtorrent 2.1.2

| Topic | Observation | Consequence |
| --- | --- | --- |
| `set_piece_hashes()` | Reads files relative to one base directory | Not used. The engine hashes through its own `PayloadSource` and calls `set_hash()` / `set_hash2()`, which supports virtual mappings and one-pass hybrid reading. |
| `create_torrent::add_http_seed()` | Deprecated and a no-op in 2.1 | The M3 editor writes BEP 17 `httpseeds` through the lossless metainfo layer. |
| `source` and other custom info keys | No API on `create_torrent` | Creation and the M3 editor write these keys through the metainfo layer; edits change the infohash by design. |
| Empty file list or total length 0 | `create_torrent` throws | Rejected earlier with `EMPTY_PAYLOAD`. |
| Piece length | Must be a power of two, 16 KiB to 128 MiB | Validated before layout. |
| Default v2/hybrid layout | Canonical order with BEP 47 padding after every file, including tail padding after the last file | The engine uses this policy consistently and never adds padding afterwards. `canonical_files_no_tail_padding` is not used. |
| `creation date` | Omitted when set to 0 | Used for reproducible output. |
| `created by` | Omitted when empty | Defaults to empty in the engine; the application chooses the text. |

## Current limitations

- Pausing a job stops reading after the buffers already in flight are
  hashed. Source files stay open while paused, so they cannot be deleted or
  renamed on Windows until the job resumes or is cancelled.
- Tracker and web-seed rows show "Unchecked"; network checks arrive with
  diagnostics in M3.
- Consistency checks are best effort, not a snapshot (see
  [architecture](architecture.md#source-consistency)).
- On file systems without hard links, a non-replacing POSIX commit falls back
  to check-then-rename, which has a short race window. Windows uses
  `MoveFileExW` without `MOVEFILE_REPLACE_EXISTING`, which is atomic.
- Durability after commit depends on the file system. The temporary file is
  flushed before the move; Windows moves it with `MOVEFILE_WRITE_THROUGH`,
  and POSIX flushes the folder after the rename.
- Case folding outside Windows uses a built-in table for Latin, Greek,
  Cyrillic, Armenian and fullwidth letters. Windows uses the invariant locale.
- The M3 editor supports typed non-layout extensions and BEP 17 `httpseeds`;
  signing, alias repair, layout rebuilding and network diagnostics remain
  separate workflows. See [metadata editor](m3-metadata-editor.md).
- Pure v2 single-file torrents and a folder containing one file with the same
  name as the torrent have the same `file tree` shape; the reference verifier
  treats a single top-level file named `name` as single-file mode.
