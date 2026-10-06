#include "tc/bridge/bencode_json.hpp"
#include "tc/core/error.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    if (size > 65536) return 0;
    // Limit JSON depth before constructing a recursive DOM. Syntax errors remain parser input.
    std::size_t depth = 0;
    bool string = false, escape = false;
    for (std::size_t i = 0; i < size; ++i) {
        auto c = data[i];
        if (string) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') string = false;
        } else if (c == '"') string = true;
        else if (c == '{' || c == '[') { if (++depth > 64) return 0; }
        else if ((c == '}' || c == ']') && depth != 0) --depth;
    }
    auto json = nlohmann::json::parse(data, data + size, nullptr, false);
    if (json.is_discarded()) return 0;
    try {
        auto value = tc::bridge::bencode_from_json(json);
        auto encoded = tc::core::bencode::encode(value);
        auto parsed = tc::core::bencode::parse(encoded, {128, 65536});
        if (tc::core::bencode::encode(parsed) != encoded) std::abort();
    } catch (tc::core::CoreError const&) { }
    return 0;
}
