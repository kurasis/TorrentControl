# Acceptance evidence (specification section 17)

This file maps the fixture IDs of the specification to the tests that prove
them. Unit tests live in `tests/unit` (Catch2; the tags in square brackets
carry the IDs). Integration tests live in `tests/integration/test_proof.py`.
They drive the `tc-proof` tool and check every produced torrent with the
independent Python verifier in `tools/reference/verify_torrent.py`, which is
the golden reference: it shares no code with the production encoder.

Milestone M1 exit gate: every F, E and W fixture below passes on Windows and
Linux CI. M2 adds the U fixtures. N and S fixtures belong to M3. UI tests live
in `tests/ui` (Playwright, Chromium) and run the bundled page against a mocked
native bridge.

## Format fixtures (F)

| ID | Evidence |
| --- | --- |
| F01 | `FormatTests.test_v1_single_file_boundary_sizes` |
| F02 | `FormatTests.test_v1_pieces_cross_file_boundaries` |
| F03 | `FormatTests.test_v2_and_hybrid_boundary_sizes`; unit `pure v2 output has no padding and no v1 pieces` |
| F04 | `FormatTests.test_v2_and_hybrid_boundary_sizes`: a 5×16 KiB+3 file has 6 blocks in a 64 KiB piece, and P+1 gives a non-power-of-two piece count; roots are checked against the reference verifier |
| F05 | `FormatTests.test_hybrid_tiny_files_and_one_large_file`; unit `hybrid creation reads each payload byte exactly once` |
| F06 | `LayoutFixtureTests.test_identical_files_share_layers`; unit `generated torrents validate in every format` (one shared layer entry) |
| F07 | `LayoutFixtureTests.test_empty_files_are_kept` |
| F08 | `MappingTests.test_empty_dataset_is_a_specific_error`; unit `empty payloads are rejected with a specific error`, `empty selections and all-empty payloads are rejected` |
| F09 | `LayoutFixtureTests.test_folder_with_one_file_versus_single_file`; unit `a selected file produces single-file mode`. Web-seed paths follow in M3 |
| F10 | `MappingTests.test_virtual_paths_from_unrelated_directories`; unit `virtual collection maps unrelated sources without copying`, `the payload may be verified from a differently named folder` |
| F11 | `LayoutFixtureTests.test_auto_piece_size_on_many_small_files`; unit `automatic piece size follows policy version 1`, `preflight reports padding and estimates before hashing` |

## Metadata fixtures (E)

| ID | Evidence |
| --- | --- |
| E01 | `LifecycleTests.test_outer_only_edit_keeps_raw_info`; unit `outer-only edit preserves raw info and unknown fields` |
| E02 | unit `bencode round-trips binary keys and strings and huge integers`; bridge `integers travel as decimal text without rounding`, `byte strings and keys keep their exact bytes`, `large structures are displayed lazily and cannot be saved back` |
| E03 | unit `bencode rejects malformed input without repair`, `... duplicate keys ...`, `... nesting and node limits`, `structural problems are invalid metainfo`, `inconsistent or unsafe layouts are invalid` |
| E04 | `MetainfoFixtureTests.test_info_edit_keeps_payload_hashes`; unit `info edits change identifiers but reuse payload hashes` |
| E05 | `MetainfoFixtureTests.test_mutated_piece_layer_is_rejected`; unit `a mutated piece layer is rejected although the infohash is unchanged`, `invalid metainfo is reported before any payload is read` |
| E06 | unit `signatures and vendor fields are preserved or explicitly removed` |
| E07 | unit `future and legacy formats are labelled, not misreported`, `unknown meta version is reported as unsupported rather than corrupt` |
| E08 | `LifecycleTests.test_reproducible_output`; unit `reproducible configuration yields identical bytes` |
| E09 | `LifecycleTests.test_hybrid_magnet_has_both_topics`; unit `magnet encodes parameters once and lists v1 topic` |

## Windows and filesystem fixtures (W)

| ID | Evidence |
| --- | --- |
| W01 | `MappingTests.test_unicode_and_long_paths`, `FilesystemFixtureTests.test_unc_path` (Windows; skipped when the administrative share is unreachable); unit `Unicode names and paths longer than MAX_PATH`, `Unicode case-insensitive collisions are reported`, `Windows-invalid names are reported` |
| W02 | `FilesystemFixtureTests.test_directory_links_are_not_traversed_by_default` (a junction on Windows, a symlink elsewhere); unit `links are not followed by default and followed traversal stays bounded`, `Windows reparse points and cloud placeholders are classified` |
| W03 | unit `a source replaced or modified after the manifest freeze is detected`, `a source that changes while it is read is detected`, `a source changed after it was hashed blocks the result`, `recheck detects modified, replaced and missing sources` |
| W04 | Documented: consistency checks are best effort, not a snapshot (see [architecture](architecture.md#source-consistency)). Neither the engine nor the tool claims a snapshot |
| W05 | unit `Windows reparse points and cloud placeholders are classified`, `cloud files that need hydration require explicit consent` |
| W06 | `FilesystemFixtureTests.test_output_may_not_overwrite_a_source`; unit `the output may not be a source file or its hard-link alias` |
| W07 | `FilesystemFixtureTests.test_existing_output_needs_explicit_replace`; unit `an existing output is replaced only by explicit choice`, `an output created by another process during the job is not overwritten`, `a write failure keeps the previous output and removes the temporary file`, `access denied is a write failure, not a success`, `a missing destination folder fails cleanly`, `unreadable selected folders block creation until excluded` |
| W08 | `LifecycleTests.test_progress_and_cancellation`; unit `cancellation stops reading and produces no result`, `verification can be cancelled`. UI responsiveness is M2 |

## Performance and resource fixtures (P)

| ID | Evidence |
| --- | --- |
| P01 | unit `hybrid creation reads each payload byte exactly once`, `worker count and buffer budget do not change the output` |
| P02 | unit `absurd piece counts fail in preflight before allocation`, `large planned memory needs explicit acceptance`, `manifest size limits warn and block` |

## User interface fixtures (U)

| ID | Evidence |
| --- | --- |
| U01 | ui `U01 hostile names render as text`; unit `origin checks accept only the bundled application origin`, `invalid messages are rejected with stable codes`, `payload strings round-trip as data`, `malformed and hostile bridge messages are rejected`, `drag and drop uses natively attached files only` |
| U02 | ui `100 000 entries keep the DOM small and scroll to the end`; unit `manifest pages are bounded and filterable`, `large structures are displayed lazily and cannot be saved back` |
| U03 | unit `a snapshot restores the view without replaying work`; the host reloads a crashed renderer and the page rebuilds from `getSnapshot` |
| U04 | Runtime detection before any web UI and the download prompt (`src/app/windows/main.cpp`); offline installation is documented with the installer in M4 |
| U05 | unit `settings protect passkeys and exports are redacted`, `credentials are masked in URLs`, `profile export is redacted by default` |
| U06 | ui `keyboard alone reaches Create and starts a job`, `tabs move with the arrow keys`, `switching modes keeps the values typed in either mode`, `U06 display scaling 100%/150%/200%` |
| U07 | unit `queued work keeps its settings snapshot and runs in order`, `batch plans per file and per child folder with conflict handling`, `one failed batch item does not affect the others` |

## Notes on running

- POSIX permission tests skip when the process can bypass permissions (root).
  CI runs them as an ordinary user.
- The hard-link and directory-link tests skip, with a reason, on file systems
  that cannot create links.
- UI tests: `cd tests/ui && npm ci && npx playwright test`. Set
  `TC_CHROMIUM` to use an already installed Chromium.
