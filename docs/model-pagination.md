# Native model pagination

Large draft, profile, batch, review and imported-torrent views now expose
bounded summaries and native pages. Original source paths, exclusion patterns,
tracker URLs, profile definitions and metainfo remain native-owned. Project
saving and torrent hashing use those originals, never display previews.

`getModelPage` returns `items`, `total`, `offset`, the actual `nextOffset` and
`revision`. Pages contain at most 50 rows with a 32 KiB serialized row budget,
including escaping. Oversized individual rows become display previews;
`getModelText` reads original text in UTF-8 boundary-safe chunks of at most
8,192 bytes. The existing 1 MiB transport guard still includes the envelope.

Supported collections:

| Model | Collections | Identity |
| --- | --- | --- |
| draft | sources, one selected source, trackers, webSeeds, source exclusions | draft revision; source ID for exclusions |
| profiles | compact profile metadata | profile collection revision |
| profilePlan | before/after arrays for a changed field | profile ID and draft revision |
| batch | items, notes, source roots of one item | batch revision and item ID |
| batchJobs | IDs of jobs started by a batch | immutable batch ID |
| torrent | trackers, webSeeds, problems | immutable opened-torrent ID |
| review | issues and active settings | draft revision |

Snapshots include totals and the selected profile's compact metadata, rather
than every profile's trackers. Settings summaries skip constructing the
custom-profile collection. Draft summaries serialize scalar fields and only
the initial collection pages. Imported overview summaries keep long text and
collection tails accessible through native readers; raw info bytes are intact.

The UI follows native byte-budget cursors for Next and records visited offsets
for Previous. First and Last allow direct access without fetching intermediate
pages. Each new collection pane retains one page, at most 50 rendered rows;
the profile select retains at most 50 initial profiles plus the current choice.
Normal draft scalar text is loaded completely only within its existing 64 KiB
field bound before editing. Larger legacy values and clipped tracker rows stay
read-only until the user explicitly supplies a replacement. Page failures show
an error and retry the failed read; they do not replay a mutation.

`editDraftRow` modifies one tracker, seed or exclusion pattern, preserving all
unloaded rows. Draft mutations use the revision of the rendered controls.
Batch updates carry the batch revision and update one item's native override;
earlier exclusions survive changes on other pages. Starting a batch returns
its ID, total and only the first job-ID page. The UI obtains aggregate completion
counts with `getBatchStatus`, including when jobs finished before the start
response arrived, without collecting every batch job ID in the renderer.
Counters are maintained at enqueue/terminal transitions and survive clearing
completed job rows; polling does not scan or copy the entire history.

Project read/write limits are both **64 MiB**. The former 16 MiB read limit
could reject the application's own saved large source mappings. Oversized
saves now fail before replacing the previous file. Settings retain their
**16 MiB** limit, now enforced on both reading and writing; a rejected settings
candidate does not change live preferences/profiles.

Verification includes a native 100,000-source project larger than 16 MiB,
cursor traversal of 1,000 escaped tracker URLs, edits preserving unseen rows,
stale page/write rejection, 75 profiles with large tracker collections,
123-item batch planning and aggregate results, original hybrid metainfo/raw-info
preservation, UTF-8 text chunk boundaries and oversized persistence failures.
Browser cases navigate 100,000 sources, edit final tracker/seed/exclusion rows,
select profiles outside the summary, preserve batch exclusions across pages,
retry read failures and display hostile imported text without markup.
Local validation passed **166 Linux CTest cases** and **45 Chromium UI cases**.

This bounds the new transport and view paths, not the entire process tree.
The native model still retains complete source mappings, profiles, imported
overview data and reports; profile/review calculations can still build full
native intermediate data. Existing frontend job history and process-tree
memory measurements remain separate performance work. Real SMB fault tests,
stock independent client import, expanded fuzzing and clean-machine release
acceptance remain open. Fuzz campaigns keep the user's three-run smoke limit.
