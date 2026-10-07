#pragma once

namespace tc::app {
// Stable across development upgrades; Setup/Uninstall checks the same object.
inline constexpr wchar_t install_mutex[] = L"Local\\TorrentControl.Installation.75613C0D-60D8-4B53-B7F7-01DE3D5E37FE";
} // namespace tc::app
