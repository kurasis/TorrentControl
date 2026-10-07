# Production bridge and diagnostic URL input checks

The next security package adds two project-code libFuzzer harnesses alongside
the existing metainfo and tagged-bencode targets. It does not change production
request or networking behavior or claim that a smoke run establishes complete security.

## Bridge input

`tc-fuzz-bridge_input` calls the actual `parse_json_input`, with its production
512-container depth limit, and the actual `Dispatcher::handle` and
`reject_request`. It checks:

- Accepted JSON round trips through the production parser.
- Only requests satisfying the protocol, ID, operation, payload and decimal
  uint64 revision contracts reach a harmless registered handler. An independent
  `from_chars` oracle checks revision values; production uses `stoull`.
- A lookalike origin never dispatches; responses remain bounded, valid JSON.
- Queue refusal preserves the correlated ID, BUSY code and retryability.
- A healthy request succeeds after any rejected input.

Seeds include the depth boundary and its immediate successor, 10,000 nested
arrays, uint64 maximum/overflow and negative revisions, malformed JSON,
invalid UTF-8, embedded NUL, incorrect field types and Unicode strings. The
handler returns a small synthetic result and has no application/file side effects.

## Diagnostic URLs

`tc-fuzz-network_url` calls `parse_probe_url` and the `ParsedUrl` serialization
and display-origin methods. Accepted raw URLs must satisfy the length/control
character policy; normalized URLs must preserve their parsed components when
round-trip parsing stays inside the 8192-byte raw input budget. Normalization
can add a default port, so an originally maximum-sized URL can exceed that
budget; the round-trip invariant does not apply to that expanded representation.
Display origins must have no userinfo or query.

Seeds cover HTTP/HTTPS/UDP, unsupported FTP, IPv6 (including scoped input),
synthetic credentials and signed queries, misleading host suffixes, invalid
ports, fragments, control bytes, backslashes and length boundaries. This
harness never calls `probe_endpoint`: no DNS lookup, socket, tracker or public
endpoint request occurs. It does not validate HTTP/UDP response handling,
redirect transport policy or preservation of an IPv6 scope identifier.

## Resumed budget and evidence

The first committed runner/CI budget was **three requested executions per
campaign**. The user requested one third, so both runner default and CI now
use **`--runs=1`**. Four targets with RNG seeds 1, 7 and 42 produce twelve
campaigns, each from a fresh corpus. Ordinary push/PR CI runs them; manual
dispatch defaults to enabled and can opt out with `run_fuzz_campaigns=false`.

LibFuzzer replays the initial corpus before honoring its execution limit.
Actual processed-unit counts can exceed one; this budget commonly yields
**zero new mutations**. Reports retain those counts, corpus sizes, exact
commands, elapsed times, exit codes and any crash inputs. Input size remains
64 KiB, per-input timeout 5 seconds, RSS limit 1 GiB and process deadline
600 seconds. The reduced requested execution count is not a strict total
input count or a one-third wall-clock guarantee.

The instrumented code uses ASan/UBSan with leak detection. Prebuilt dependencies
(including libcurl) and the WebView2 host remain outside this instrumentation.
The deterministic sanitizer suite and runtime negative controls still run.
CI uploads all results as `linux-fuzz-sanitizer-evidence`, including
`campaign/campaign.json`. [Local evidence](evidence/security-inputs/linux-local.json)
records the requested and observed budgets; Windows behavior is established
separately by CI.
