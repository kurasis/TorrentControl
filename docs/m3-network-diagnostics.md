# M3 network diagnostics

The tracker, web-seed and opened-torrent tabs now offer an explicit check.
Opening a torrent, rendering a tab or restoring a renderer starts no traffic.
Native targets come from a draft or opened torrent ID; the bridge does not
accept a page-selected diagnostic URL. URLs in result pages are origin-only:
credentials, passkey paths, query tokens and server failure bodies stay out.

## Protocol behavior

- UDP trackers: BEP 15 connect with random transaction ID; validate sender,
  action and transaction, accept compatible trailing bytes. One address per
  available family. Quick response window is 15 seconds, with an explicit
  optional second request/window of 30 seconds. DNS is bounded separately.
  No connection ID cache, real announce, peer registration or UDP scrape.
  A connect response leaves torrent acceptance and authorization unknown.
- HTTP trackers: transform the last `announce…` path segment to `scrape…`
  when possible, use a random 20-byte hash and strip swarm query parameters.
  Otherwise probe the endpoint without swarm parameters. Bounded bencode
  validation distinguishes zero peers, access restriction, unsupported scrape
  and HTML with status 200. Failure text is never exported.
- HTTP requests: IPv4 and IPv6 each receive half of a 15-second total budget.
  Default platform certificate and hostname verification remain enabled;
  Windows uses SChannel. Redirects are limited to three within the same origin
  and credentials. Cross-origin redirects are intentionally blocked.
- BEP 19: resolve each path component once, retain the signed base query,
  include the root name once and skip virtual hybrid padding. Direct single-file
  and directory bases are distinct; multifile bases require a trailing slash.
  Sample first, nested (when present) and last real file, at most three per base.
  `Range: bytes=0-0` must return a matching total length and one byte. Empty files,
  ignored Range, bad Content-Range and content encoding have separate results.
  HEAD is not needed; a server rejecting HEAD can still pass Range.
- BEP 17: transport reachability only, clearly labelled. Ordinary pure-v2
  httpseeds edits are rejected; unsupported imports remain preserved.

Seed observations do not claim data integrity or full-file coverage. Results
show the checked time, cache status, sampled path and total real-file count.
Each HTTP tracker body is capped at 64 KiB, seed response at 4 KiB, headers at
32 KiB and catalog at 1 MiB. No decompression is requested. Even ignored Range
responses remain below the specified 1 MiB per-base download budget.

## Connection policy and catalog

Choose direct or an explicit HTTP proxy origin. HTTPS uses CONNECT through that
proxy. Environment proxy variables are disabled; a failed proxy never falls
back to direct. UDP through this policy is unsupported without DNS or packets.
Four workers, native deduplication and one active run bound concurrent traffic.
Cancel aborts outstanding operations. Observation cache expires after ten
minutes; a checkbox bypasses it. Native pages retain eight runs across renderer
recovery. Proxy credentials must be reentered after renderer recovery, while
the selected proxy mode remains selected.

Catalog fetch is another explicit action against the built-in HTTPS source.
A valid list has at most 512 supported, unauthenticated URLs. Its fetched time
and SHA-256 checksum are separate from the upstream source date, which is
unknown for downloaded lists. The bundled provenance remains unchanged until
a fetch succeeds. Failure preserves the last valid native catalog.
Review displays additions/removals before applying with both checksum and
draft revision. Custom endpoints, disabled rows and existing tiers survive;
public catalogs cannot be applied to private drafts. Catalog-managed additions
are tracked during this application process. There is no periodic updater;
catalog/cache/history persistence across full process restart is not provided.

## Evidence

Local release suite: 134 CTest entries, including 15 deterministic Python
network fixture cases, and 30 Chromium UI cases. All network fixtures use
loopback; they do not contact public trackers or the catalog source.

| Fixtures | Checks |
| --- | --- |
| N01/N02 | UDP sender/transaction, trailing bytes, restriction, deadline and finite retry |
| N03/N04 | HTML 200, zero peers, failure reason, unsupported scrape, IPv4/IPv6 observations |
| N05 | Explicit HTTP proxy without destination DNS; UDP unsupported; invalid proxy policy |
| N06 | Opening/planning sends nothing; no actual hash or peer ID in HTTP probes |
| N07 | Valid catalog provenance, failed refresh preservation, reviewed application and private/stale guards |
| S01/S04 | Unicode/space/hash paths, query preservation, single-file v2, hybrid pad exclusion |
| S02/S03/S05 | Range/length, empty file, bounded ignored Range, at most three real samples, no integrity claim |
| TLS / resources | Untrusted certificate rejected; four-worker bound; cancellation; dedup/cache/refresh |
| U03/U05 | Native history ordering and redaction; UI snapshot recovery without request replay |

`tests/windows/run-native-flow.ps1` additionally drives actual WebView2 tracker
and seed buttons against a local fixture. It requires a zero-peer tracker
response, two Unicode payload paths with correct Content-Range, no exported
URL token and restored diagnostic results after a real renderer crash.
The required Windows CI job reports the native result in its evidence artifact.

The fixture certificate and private key under `tests/integration/fixtures` are
public, untrusted test data, used only to verify certificate rejection.
