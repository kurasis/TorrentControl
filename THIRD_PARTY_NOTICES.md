# Third-party notices

TorrentControl uses the following components. Versions are fixed by the vcpkg
baseline in `vcpkg.json`. Full license texts are installed by vcpkg under
`vcpkg_installed/<triplet>/share/<port>/copyright` and will be bundled with
release packages (milestone M4). This file is a summary, not a substitute for
those texts.

| Component | Version | License | Used in |
| --- | --- | --- | --- |
| libtorrent | 2.1.2 | BSD-3-Clause, with bundled parts under Zlib, BSL-1.0 and others (see its `LICENSE`) | core |
| try_signal (libtorrent dependency) | pinned by the libtorrent port | BSD-3-Clause | core |
| Boost (Asio, System and other libtorrent dependencies) | 1.92.0 | BSL-1.0 | core |
| OpenSSL (libtorrent crypto) | 3.6.5 | Apache-2.0 | core |
| curl | 8.22.0 | curl license (MIT-style) | diagnostics; SChannel on Windows, OpenSSL on Linux |
| c-ares | 1.34.8 | MIT | bounded asynchronous DNS for diagnostics |
| zlib (curl dependency) | 1.3.2 | Zlib | diagnostics |
| nlohmann/json | 3.12.0 | MIT | bridge, tools |
| Microsoft.Web.WebView2 SDK | 1.0.4191.47 | Microsoft WebView2 SDK license (BSD-style) | Windows host |
| Windows Implementation Libraries (WIL) | 1.0.260126.7 | MIT | Windows host |
| Catch2 | 3.16.0 | BSL-1.0 | tests only, not distributed |
| anacrolix/torrent | 1.61.0 | MPL-2.0 (with third-party dependencies recorded in go.mod/go.sum) | independent development audit only, not distributed |

The Microsoft Edge WebView2 Runtime is not redistributed by development
builds; it is installed separately (Evergreen).
