# Bounded outgoing bridge and paged results

Every serialized response and event is limited to **1 MiB**, including JSON
escaping and invalid UTF-8 replacement. The serializer writes through a
bounded stream rather than first allocating an arbitrarily large JSON dump.
An oversized response becomes a correlated `RESPONSE_TOO_LARGE` error. It is
not retryable: a command may already have completed and must not be replayed.
An oversized event becomes a monotonic `resyncRequired` event; the page reads
the authoritative native snapshot.

## Paged data

- Snapshot job history contains at most 50 compact jobs and at most 256 KiB
  of job rows. `getJobsPage` supplies the next actual offset and a collection
  revision. The page retries snapshot collection if jobs are added/removed
  between pages. Existing per-job versions reject older events.
- Job events contain aggregates and small previews, rather than all payload
  verification rows. The complete per-file report remains native-owned.
  `getVerifyFilesPage` reads up to 250 rows, either all files or non-OK files.
  Error indices are built once, so error paging does not repeatedly scan every
  verified file. Each row has its original native index.
- The job view virtualizes verification rows, supports switching the problem
  filter, and obtains original per-file text with `getVerifyFile`. Long path
  and message previews carry an explicit truncation flag; they are never
  substituted for native file paths.
- `getJobLayoutPage`/`getJobLayoutRow` expose retained result mappings.
  The existing native mapping sample still retains at most 1,000 rows; UI
  reports retained rows and real payload file count rather than claiming the
  sample is complete. Job logs retain their existing 200-line ring.
- `getJobTextPage`/`getJobText` expose retained logs and creation warnings.
  Native versions protect full text lookup from a shifted live log ring.

Virtual lists retain at most **8 pages** (2,000 rows) and allow at most four
page requests in flight. Visiting earlier evicted pages reads them again.
Detached lists disconnect their resize observer instead of retaining caches.

## Metadata display

Tagged bencode display applies an aggregate 256 KiB byte budget by default,
in addition to its node and per-string budgets. Budget accounting reserves
space for JSON wrappers and worst-case escaping. An omitted container reports
its direct child count without traversing its entire subtree. Elided and
truncated values remain unsaveable; the native raw bytes remain intact.

Field pages also have a 256 KiB row budget. Their actual `nextOffset` may be
earlier than the row-count limit when keys are large. The editor follows these
cursors and retains previous page boundaries, preserving access to complete
binary keys up to its existing 4,096-byte edit limit.

This package bounds transport and display data, not every native allocation.
Native verification retains complete reports. Very large unpaged draft,
profile, batch or torrent-overview objects can still receive a size error;
the guard does not claim all those collections have paginated user flows.
Creation peak memory, stalled Windows/UNC I/O, independent v2 client proof and
fuzz/sanitizer gates remain open.

## Verification

Regression checks cover escaped-size overflow, correlation without mutation
replay, UTF-8 replacement, monotonic resync events, aggregate metadata budgets,
large binary-key cursors preserving raw info, a real 261-file create/verify
workflow with a missing-file control, native retention of 100,000 results,
73-job history recovery, browser paging to the last of 100,000 rows, bounded
page caching, complete warning/log navigation and cursor back navigation.

The real Windows WebView2 flow additionally reads verification pages and full
file details for v1/v2/hybrid payloads; evidence records `verificationPaging`.
It retains the 100,000-file Win32/WebView2 heartbeat regression gate.

Local validation: **149/149 CTest** cases and **35/35 Chromium UI** tests
passed. The real 100,000-file native benchmark recorded a maximum response
of 55,124 bytes, 1,000 filtered files and successful create validation.
Windows evidence is checked separately in GitHub CI before merging.
