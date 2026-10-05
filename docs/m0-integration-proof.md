# M0 integration proof

Specification section 3.1 asks to prove the following before building the full
GUI. Each item is covered by an automated test that runs in CI on Windows
(MSVC) and Linux (GCC). Every produced torrent is checked by
`tools/reference/verify_torrent.py`, which recomputes SHA-1 pieces, v2 file
roots, piece layers and identifiers using only the Python standard library.

| Requirement | Evidence |
| --- | --- |
| v1/v2/hybrid generation | `FormatTests.test_v1_v2_hybrid_with_explicit_and_auto_piece_sizes`, boundary-size tests (F01, F03), cross-file pieces (F02) |
| Canonical file ordering and padding | `FormatTests.test_hybrid_tiny_files_and_one_large_file` (F05), `test_canonical_order_is_independent_of_input_order`; verifier checks hybrid v1/v2 layouts match and files are piece aligned |
| Source path differing from torrent path | `MappingTests.test_virtual_paths_from_unrelated_directories` (F10), unit test `virtual collection maps unrelated sources without copying` |
| Unicode and long paths | `MappingTests.test_unicode_and_long_paths`, unit test `Unicode names and paths longer than MAX_PATH` (W01) |
| Progress and cancellation | `LifecycleTests.test_progress_and_cancellation`, unit test `cancellation stops reading and produces no result` (W08) |
| One-pass hybrid payload reading | Unit test `hybrid creation reads each payload byte exactly once` with an instrumented reader (P01); every integration run also checks bytes read equals payload size |
| Outer-only editing with identical raw `info` bytes | `LifecycleTests.test_outer_only_edit_keeps_raw_info`, unit test `outer-only edit preserves raw info and unknown fields` (E01) |
| Minimal native WebView2 shell | CI step "WebView2 shell self-test": the app loads the bundled page, calls `getEngineInfo` over the bridge, and exits 0 |
| Pinned toolchain | `vcpkg.json` baseline and libtorrent override, `TC_REQUIRE_PINNED_TOOLCHAIN` in CI |

Also covered early: reproducible bytes (E08), hybrid magnet topics (E09),
malformed bencode rejection (E03), unknown `meta version` (E07), empty-payload
error (F08), automatic piece size termination with many small files (F11), and
bridge message validation (U01).

Not part of M0: interoperability with a second independent client engine,
performance measurements, and the Windows-specific filesystem cases W02–W07.
