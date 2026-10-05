#pragma once

// Binary-safe bencode representation for the JavaScript boundary
// (specification section 7.4).
//
// JSON has no byte strings and JavaScript numbers are doubles, so every value
// is tagged and integers travel as decimal text:
//   integer     {"t":"int","v":"-12345678901234567890123"}
//   byte string {"t":"str","utf8":"text","len":4}         valid UTF-8
//               {"t":"bytes","hex":"00ff","len":2}        anything else
//   list        {"t":"list","items":[...]}
//   dictionary  {"t":"dict","entries":[{"key":<string>,"value":<value>},...]}
// Keys are tagged byte strings too. Large structures are shown lazily: once
// the node budget is used up, values become {"t":"elided","nodes":N}, and long
// strings carry only a prefix with "truncated":true. Elided or truncated
// values cannot be converted back; the original bytes stay in the native
// model and are never rebuilt from a display copy.

#include "tc/core/bencode.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>

namespace tc::bridge {

struct DisplayBudget {
    std::size_t max_nodes = 10'000;
    std::size_t max_string_bytes = 4'096;
};

nlohmann::json bencode_to_json(core::bencode::Value const& value, DisplayBudget const& budget = {});

// Strict inverse for complete (not elided or truncated) values. Throws
// core::CoreError(InvalidArgument) on malformed input.
core::bencode::Value bencode_from_json(nlohmann::json const& j);

} // namespace tc::bridge
