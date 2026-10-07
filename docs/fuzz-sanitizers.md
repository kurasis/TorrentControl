# Bounded fuzzing and sanitizer checks

**Campaigns are enabled at one third of their original requested budget:**
`--runs=1` instead of `--runs=3`, per target and RNG seed. Push/PR CI includes
them; a manual dispatch can opt out with `run_fuzz_campaigns=false`.
See [JSON limits and Windows ASan](json-input-windows-asan.md) for production
input hardening and Windows native instrumentation, and
[additional input harnesses](security-input-harnesses.md) for bridge and URL coverage.

`linux-sanitizers` instruments project code with AddressSanitizer and
UndefinedBehaviorSanitizer. `linux-fuzz` additionally builds four Clang
libFuzzer targets, with coverage instrumentation in the core, service and
bridge libraries. Ordinary tests and developer tools use the same instrumented
libraries. CI uses Clang 18 on Ubuntu 24.04, and runs the entire Linux native
CTest suite with leak detection and sanitizer errors configured to stop the
process. The initial local campaign used Clang 19.1.7 on Debian 13.

The dependency prefix reuses the pinned release build's libtorrent 2.1.2,
OpenSSL, Catch2 and other libraries. Their binaries are **not instrumented**;
this package does not establish sanitizer coverage of those implementations,
the Windows/WebView2 host, network response parsing or full application operation.
URL parsing is exercised without network I/O. Subsequent Windows native ASan coverage is documented
separately in the link above.
Regular Windows/Linux/UI jobs remain required alongside this Linux campaign.

## Harnesses and limits

`tc-fuzz-metainfo` parses binary bencode and metainfo, validates layouts and
piece layers, exports magnets, and performs outer/info edits. It checks exact
bencode byte round trips (including accepted unsorted dictionaries), semantic
tagged-JSON round trips, bounded display output, and preservation of raw info
and both identifiers across outer-only edits. Expected parse rejection is
handled separately from subsequent invariants so a failed round trip cannot
silently become a rejected input.

`tc-fuzz-tagged_json` parses JSON and invokes the strict tagged bencode inverse,
checking accepted values can survive bencode encoding/parsing. Malformed
tagged values must raise the documented domain error; unexpected exceptions
fail the run. Before constructing the JSON DOM the driver bounds nesting to
64; deeper JSON is outside this campaign, rather than tested and declared safe.

`tc-fuzz-bridge_input` exercises the production depth-512 JSON parser, request
validation, revision parsing, origin refusal, queue refusal and recovery after
rejection. `tc-fuzz-network_url` exercises diagnostic URL normalization and
credential-free display origins. Neither harness sends network requests or
opens payload files. Their contracts and limits are in the report linked above.

Each input is bounded to 64 KiB. Bencode/metainfo use depth 64 and 4,096 nodes;
edit reparsing allows the small added fields. Each input has a five-second
deadline and each campaign a 1 GiB RSS limit plus a 600-second process deadline.
These are fuzz-driver limits; this initial package did not alter production
defaults. The subsequent JSON input limit is 512 containers. No payload
I/O is performed by the drivers. The seed generator creates real single/multi
v1/v2/hybrid torrents with empty files, Unicode paths and piece boundaries,
plus binary keys, unsorted/duplicate dictionaries, oversized integers, unsafe
paths and malformed tagged values.

At the user's request, the runner starts each target from a fresh seed corpus
for RNG seeds 1, 7 and 42 with `-runs=1`: twelve short smoke campaigns across
the four targets. The original two targets used `-runs=3`. LibFuzzer
replays the initial corpus before applying the execution limit, so actual
executions can exceed one and this budget may produce no new mutations.
Reports explicitly mark smoke mode and include the actual execution statistics,
commands, initial seed counts, elapsed times,
libFuzzer statistics and exit codes. Logs, mutated corpora and crash inputs
are retained even on failure. `campaignPassed` requires successful exits and
at least the requested execution counts; it is not a release security certification.
Longer continuous campaigns, production-sized inputs, additional subsystems
and dependency instrumentation remain further work. Windows project code is
now covered by a separate ASan preset; its GUI host remains outside that preset.

## Findings included in this package

Clang rejected `Value`'s recursive dictionary model because its declarations
instantiated vector special members/default arguments while `DictEntry` was
incomplete. Special members and the empty-dictionary overload now have
definitions after the type is complete, retaining copy/move behavior.

A malformed `truncated` field such as `"wrong"` raised a nlohmann `type_error`
instead of `CoreError(InvalidArgument)`. Both tagged values and dictionary keys
now validate its boolean type before reading it. A regression case checks
string, number, null, array and object flags at both locations. This was an
exception-contract defect; the audit does not label it a demonstrated memory
safety vulnerability.

## Reproduce

First build `linux-release` to install the pinned dependencies. With Clang
and its sanitizer runtime installed:

```sh
export TC_DEPS_PREFIX="$PWD/build/linux-release/vcpkg_installed/x64-linux"
CXX=clang++-18 cmake --preset linux-fuzz
cmake --build --preset linux-fuzz --parallel 4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --preset linux-fuzz --parallel 4
python3 tests/fuzz/seed_corpus.py \
  --proof build/linux-fuzz/tools/tc-proof/tc-proof --output build/fuzz-seeds
python3 tests/fuzz/run_campaign.py \
  --build build/linux-fuzz --corpus build/fuzz-seeds --output build/fuzz-evidence/campaign
```

Use a fresh output directory for every rerun; evidence is never overwritten.
An exploratory local run used 10,000 executions per target after fixing the
tagged-JSON defect. The committed default and CI now use one as requested;
that exploratory 10,000-run experiment is not the original CI budget.
Increase `--runs` for a longer campaign. `linux-sanitizers` can also use GCC,
while `linux-fuzz` rejects incompatible compilers or disabled sanitizers at
configuration time.
