# M3: registry and metadata editor

This stage implements the metadata portion of M3. Network diagnostics and the
N/S fixture groups remain the next stage; no tracker, seed or DHT requests are
made by this editor.

Open a torrent, switch to the Expert tab and use the Metadata editor. The
`top` and `info` dictionaries have separate paged field lists. Known fields
can be added from the registry. Unknown extensions accept a text key or a
`0x` hex byte key and use the tagged bencode representation. Large or protected
values remain inspectable with bounded prefixes; prefixes cannot be saved.
The registry also documents fields inside file entries, which are preserve or
rebuild only in this release. Arbitrary nested dictionaries can be edited as
a complete bounded typed value, except protected file/layout structures.

## Registry and validation

`src/core/include/tc/core/field_registry.hpp` defines byte-key descriptors:
placement, type, format applicability, support level, hash impact, reference
and validation policy. `field_registry.cpp` is the shared native authority.
The bridge exposes it; the page cannot bypass protected fields by changing
its own controls.

Forms cover comment, creator, Unix creation date, announce, ordered tracker
tiers, BEP 19 web seeds, separate BEP 17 HTTP seeds, source and private. BEP 19
string/list shape is retained when editing an imported single URL. Private
is set to integer 1 or removed; an untouched imported 0 is preserved. Nodes
use explicit host/port pairs. Known values receive native type and range
checks. Registered computed/preserve fields require dedicated workflows,
including layout, hashes, aliases, symlinks, signatures and legacy Merkle data.
Binary values in usually textual fields stay available as tagged bytes and
are omitted from decoded summaries rather than breaking JSON.

The editor does not implement signing, alias conflict repair, symlink
creation, mtime serialization or private tracker authorization. It preserves
these imports. Changing a legacy comment alias requires a dedicated conflict
resolution workflow. Unknown fields are labelled expert extensions, without
claiming their client semantics or interoperability.

## Preview and commit

Patches affect only selected keys. Semantic no-ops preserve the entire input,
including noncanonical v1 ordering and signatures. Outer-only edits copy the
exact raw `info`; info edits reuse existing payload hashes and piece layers
and show old/new full identifiers. BEP 35 signatures are removed only with an
explicit choice when info actually changes. The complete candidate undergoes
structural, path, hybrid and piece-layer validation.

Native code owns one immutable candidate, its opaque token and its selected
output path. A newer preview invalidates the older token and output selection.
The page receives a bounded before/after field review plus identity changes.
The default Save As name is `<source stem>.edited.torrent`. Cancel leaves the
preview intact. Replacing an existing destination requires the user's explicit
checkbox after the native dialog, and changed source bytes refuse a save.
The commit uses an exclusive temporary file, flush, reopen, byte/hash checks
and atomic replacement where supported. Errors retain the candidate for retry.
The saved torrent becomes the opened native model.

The output writer now checks full metainfo validity, not just parseability.
Its narrow legacy-v1 exception requires a validated source model and the exact
original raw info slice; v2/hybrid canonical validation remains mandatory.
Malformed/ambiguous bencode, duplicate keys and future meta versions do not
enter the normal editable model. Repair is not an implicit save operation.

Snapshot recovery includes the selected torrent, reviewed changes and pending
token/output selection. Unsubmitted typing is page-local and is not promised
across renderer failure; the reviewed native candidate survives. Jobs are not
replayed during recovery.

## Verification

Local Linux release verification passed: **128/128 CTest entries** and
**26/26 Playwright/Chromium tests**. Windows results are reported by the
required CI workflow and its `windows-native-evidence` artifact.

- Catch2: registry uniqueness, known types, URL tiers/seeds/nodes, protected
  fields, no-op raw preservation, signature decision, binary keys and large
  integers, v1/v2/hybrid hash reuse without reading payload, stale tokens,
  cancelled dialogs, changed sources, overwrite conflicts and atomic commits.
- Playwright: forms, binary extensions, identity review, protected fields,
  cancellation, explicit replacement, retry, hostile text and preview recovery.
- Windows/WebView2: actual UI outer/comment and info/source edits, native Save
  As dialogs, byte-level raw info/payload hash inspection, renderer recovery.
  The independent Python verifier checks all six actual output torrents.

Run the commands in [building.md](building.md) and the native workflow in
[windows-native-validation.md](windows-native-validation.md). The latter still
lists manual DPI, accessibility, shell and clean installation release gates.
