#pragma once

// API floor, not a recommendation to install an old Runtime. Use current Evergreen.
// SDK 1.0.1774.30 introduced the DOM File/AdditionalObjects API used by Explorer drops.
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace tc::app {

inline constexpr std::string_view minimum_webview_runtime = "113.0.1774.30";

inline std::optional<std::array<std::uint32_t, 4>> runtime_version(std::string_view text)
{
    std::array<std::uint32_t, 4> parts{};
    std::size_t at = 0;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        std::size_t const start = at;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            auto const digit = static_cast<std::uint32_t>(text[at++] - '0');
            if (parts[i] > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) return std::nullopt;
            parts[i] = parts[i] * 10 + digit;
        }
        if (at == start) return std::nullopt;
        if (i + 1 < parts.size() && (at >= text.size() || text[at++] != '.')) return std::nullopt;
    }
    auto const suffix = text.substr(at);
    if (!suffix.empty() && suffix != " beta" && suffix != " dev" && suffix != " canary") return std::nullopt;
    return parts;
}

inline bool supports_webview_runtime(std::string_view actual, std::string_view minimum = minimum_webview_runtime)
{
    auto const current = runtime_version(actual);
    auto const required = runtime_version(minimum);
    return current && required && *current >= *required;
}

} // namespace tc::app
