# Bounded fuzzing and sanitizer checks

`linux-sanitizers` instruments project code with AddressSanitizer and
UndefinedBehaviorSanitizer. `linux-fuzz` additionally builds two Clang
libFuzzer targets, with coverage instrumentation in the core, service and
bridge libraries. Ordinary tests and developer tools use the same instrumented
libraries. CI uses Clang 18 on Ubuntu 24.04, and runs the entire Linux native
CTest suite with leak detection and sanitizer errors configured to stop the
process. The initial local campaign used Clang 19.1.7 on Debian 13.

The dependency prefix reuses the pinned release build's libtorrent 2.1.2,
OpenSSL, Catch2 and other libraries. Their binaries are **not instrumented**;
this package does not establish sanitizer coverage of those implementations,
the Windows/WebView2 host, network parsers or full application operation.
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

Each input is bounded to 64 KiB. Bencode/metainfo use depth 64 and 4,096 nodes;
edit reparsing allows the small added fields. Each input has a five-second
deadline and each campaign a 1 GiB RSS limit plus a 600-second process deadline.
These are fuzz-driver limits; production defaults are unchanged. No payload
I/O is performed by the drivers. The seed generator creates real single/multi
v1/v2/hybrid torrents with empty files, Unicode paths and piece boundaries,
plus binary keys, unsorted/duplicate dictionaries, oversized integers, unsafe
paths and malformed tagged values.

At the user's request, the runner starts each target from a fresh seed corpus
for RNG seeds 1, 7 and 42 with `-runs=3`: six short smoke campaigns. LibFuzzer
replays the initial corpus before applying the execution limit, so actual
executions can exceed three and this budget may produce no new mutations.
Reports explicitly mark smoke mode and include the actual execution statistics,
commands, initial seed counts, elapsed times,
libFuzzer statistics and exit codes. Logs, mutated corpora and crash inputs
are retained even on failure. `campaignPassed` requires successful exits and
at least the requested execution counts; it is not a release security certification.
Longer continuous campaigns, production-sized inputs, additional subsystems
and dependency/Windows instrumentation remain further work.

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
tagged-JSON defect. The committed default and CI use three as requested.
Increase `--runs` for a longer campaign. `linux-sanitizers` can also use GCC,
while `linux-fuzz` rejects incompatible compilers or disabled sanitizers at
configuration time.
