#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string_view>

namespace tc::service {

// Includes the root container. Enough for 128 bencode levels represented as
// tagged dictionaries (three JSON containers per level) plus request wrappers.
inline constexpr std::size_t max_json_input_depth = 512;

// Bounds nesting before the JSON library can allocate/copy a recursive DOM.
// Syntax and UTF-8 validation remain the JSON parser's responsibility.
// Throws CoreError(ResourceLimit) for excessive nesting; malformed JSON is
// returned as a discarded value. Callers must also apply their byte limits.
nlohmann::json parse_json_input(std::string_view bytes);

} // namespace tc::service
