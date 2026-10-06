# TorrentControl

A Windows desktop application for creating, inspecting, editing, and validating
BitTorrent metainfo (`.torrent`) files: v1, v2, and hybrid torrents, very large
files and file collections, lossless metadata editing, and tracker/web-seed
diagnostics. Built with C++20, [libtorrent](https://libtorrent.org) 2.1.2, and
Microsoft Edge WebView2.

> **Status:** M2 complete, M3 metadata editor and network diagnostics implemented. On top of the M1 core, the
> app has simple and advanced modes, a job queue with pause, resume and
> cancel, profiles with a change preview and undo, projects, and batch
> creation. The [M3 editor](docs/m3-metadata-editor.md) adds a native field registry,
> binary-safe metadata forms, identity preview and safe Save As. Explicit
> [network checks](docs/m3-network-diagnostics.md) cover trackers and sampled web seeds.
> [Performance and independent client compatibility](docs/native-performance-compatibility.md)
> are in progress. Development builds are unsigned.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/core` | Platform-independent engine: manifest, single-pass hasher, lossless bencode/metainfo layer, field registry |
| `src/service` | Application service: draft, job queue, profiles, batch, projects, settings, metadata previews |
| `src/bridge` | Versioned JSON command bridge between the frontend and the native host |
| `src/app/windows` | Win32 + WebView2 native host |
| `frontend` | Bundled HTML/CSS/JS served from a fixed virtual origin |
| `tools/tc-proof` | Internal developer tool that drives the core headlessly (not shipped) |
| `tools/reference` | Independent Python verifier used as the golden reference in tests |
| `tests` | Catch2 unit tests, the Python integration proof and Playwright UI tests (`tests/ui`) |
| `docs` | Building, architecture, engine notes, milestone reports |

## Building

See [docs/building.md](docs/building.md). In short, with
[vcpkg](https://github.com/microsoft/vcpkg) checked out at the commit pinned in
`vcpkg.json` and `VCPKG_ROOT` pointing at it:

```powershell
# Windows, from a "x64 Native Tools" prompt for MSVC 14.44
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release
```

```sh
# Linux (headless core and tests only)
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
```

## Developer tool

`tc-proof` drives the core from the command line and prints JSON. It is used
by the integration tests and is not shipped.

```sh
tc-proof scan PATH [--exclude GLOB] [--follow-links] [--skip-cloud] [--non-recursive]
tc-proof create --source PATH --format v1|v2|hybrid [--piece-length N] -o OUT [--replace]
tc-proof validate FILE
tc-proof verify FILE --root PAYLOAD_FOLDER
tc-proof edit-outer IN OUT [--set-comment TEXT] [--remove KEY]
tc-proof edit-info IN OUT [--set-source TEXT] [--set-private] [--remove-signatures]
```

`validate` and `verify` exit with 5 when they find a problem; any other error
prints `{"status": "failed", "code": ...}` and exits with 2.

## Documentation

- [Building and testing](docs/building.md)
- [Architecture](docs/architecture.md)
- [Engine notes and known limitations](docs/engine-notes.md)
- [Acceptance evidence](docs/acceptance.md)
- [M0 integration proof report](docs/m0-integration-proof.md)
- [M2 stabilization and regression evidence](docs/m2-stabilization.md)
- [Settings persistence and Runtime compatibility](docs/settings-runtime-reliability.md)
- [Native model pagination](docs/model-pagination.md)
- [Third-party notices](THIRD_PARTY_NOTICES.md)
