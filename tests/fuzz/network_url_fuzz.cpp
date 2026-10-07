#include "tc/service/diagnostics.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>

namespace {
void require(bool condition) { if (!condition) std::abort(); }
}

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    if (size > 65536) return 0;
    std::string const bytes(reinterpret_cast<char const*>(data), size);
    std::optional<tc::service::ParsedUrl> parsed;
    try { parsed = tc::service::parse_probe_url(bytes); }
    catch (tc::service::ServiceError const& error) { require(error.code() == "INVALID_URL"); }
    if (!parsed) return 0;
    require(!bytes.empty() && bytes.size() <= 8192 && bytes.find('#') == std::string::npos);
    for (unsigned char c : bytes) require(c > 32 && c != 127 && c != '\\');
    // Normalization inserts a default port. Round-trip parsing is only within
    // the parser's raw 8192-byte input budget, including that added text.
    auto const canonical = parsed->str();
    if (canonical.size() > 8192) return 0;
    auto restored = tc::service::parse_probe_url(canonical);
    require(restored.scheme == parsed->scheme && restored.host == parsed->host);
    require(restored.port == parsed->port && restored.path == parsed->path);
    require(restored.query == parsed->query && restored.userinfo == parsed->userinfo);
    require(restored.origin() == parsed->origin());
    // Display origins must not expose credentials, path or query strings.
    auto origin = tc::service::parse_probe_url(parsed->origin());
    require(origin.userinfo.empty() && origin.query.empty());
    require(origin.scheme == parsed->scheme && origin.host == parsed->host && origin.port == parsed->port);
    return 0;
}
