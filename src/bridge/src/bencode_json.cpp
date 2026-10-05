#include "tc/bridge/bencode_json.hpp"

#include "tc/core/error.hpp"

#include <string>

namespace tc::bridge {

namespace {

using core::CoreError;
using core::ErrorCode;
using core::bencode::Value;

bool valid_utf8(std::string_view s)
{
    std::size_t i = 0;
    while (i < s.size()) {
        auto const c = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        std::uint32_t cp = 0;
        if (c < 0x80) { ++i; continue; }
        if ((c & 0xe0) == 0xc0) { len = 2; cp = c & 0x1f; }
        else if ((c & 0xf0) == 0xe0) { len = 3; cp = c & 0x0f; }
        else if ((c & 0xf8) == 0xf0) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > s.size()) return false;
        for (std::size_t k = 1; k < len; ++k) {
            auto const cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3f);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return false;
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        i += len;
    }
    return true;
}

std::string to_hex(std::string_view s)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out += digits[c >> 4];
        out += digits[c & 0xf];
    }
    return out;
}

[[noreturn]] void bad(std::string const& what)
{
    throw CoreError(ErrorCode::InvalidArgument, "Invalid tagged bencode value: " + what);
}

std::string from_hex(std::string const& hex)
{
    if (hex.size() % 2 != 0) bad("odd hex length");
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        bad("invalid hex digit");
    };
    std::string out(hex.size() / 2, '\0');
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
    return out;
}

// Number of nodes in a subtree, for the elision marker.
std::size_t count_nodes(Value const& v)
{
    std::size_t n = 1;
    if (v.is_list())
        for (auto const& item : v.items()) n += count_nodes(item);
    if (v.is_dictionary())
        for (auto const& e : v.entries()) n += count_nodes(e.value);
    return n;
}

class Converter {
public:
    explicit Converter(DisplayBudget const& budget) : budget_(budget) {}

    nlohmann::json string(std::string_view bytes)
    {
        bool const truncated = bytes.size() > budget_.max_string_bytes;
        std::string_view shown = truncated ? bytes.substr(0, budget_.max_string_bytes) : bytes;
        nlohmann::json j;
        // A UTF-8 sequence cut by truncation is shown as hex rather than mangled.
        if (valid_utf8(shown)) {
            j = {{"t", "str"}, {"utf8", std::string(shown)}};
        } else {
            j = {{"t", "bytes"}, {"hex", to_hex(shown)}};
        }
        j["len"] = bytes.size();
        if (truncated) j["truncated"] = true;
        return j;
    }

    nlohmann::json value(Value const& v)
    {
        if (used_ >= budget_.max_nodes) return {{"t", "elided"}, {"nodes", count_nodes(v)}};
        ++used_;
        switch (v.type()) {
        case core::bencode::Type::Integer:
            if (v.text().size() > budget_.max_string_bytes)
                return {{"t", "int"}, {"v", v.text().substr(0, budget_.max_string_bytes)}, {"truncated", true}};
            return {{"t", "int"}, {"v", v.text()}};
        case core::bencode::Type::String: return string(v.text());
        case core::bencode::Type::List: {
            nlohmann::json items = nlohmann::json::array();
            for (auto const& item : v.items()) {
                bool const last = used_ >= budget_.max_nodes;
                items.push_back(value(item));
                if (last) break;
            }
            return {{"t", "list"}, {"items", std::move(items)}};
        }
        case core::bencode::Type::Dictionary: {
            nlohmann::json entries = nlohmann::json::array();
            for (auto const& e : v.entries()) {
                if (used_ >= budget_.max_nodes) {
                    entries.push_back({{"key", {{"t", "elided"}}}, {"value", {{"t", "elided"}}}});
                    break;
                }
                ++used_; // keys also consume the display budget
                entries.push_back({{"key", string(e.key)}, {"value", value(e.value)}});
            }
            return {{"t", "dict"}, {"entries", std::move(entries)}};
        }
        }
        return nullptr;
    }

private:
    DisplayBudget const& budget_;
    std::size_t used_ = 0;
};

std::string bytes_from_json(nlohmann::json const& j)
{
    if (!j.is_object() || !j.contains("t") || !j["t"].is_string()) bad("missing tag");
    if (j.value("truncated", false)) bad("a truncated display value cannot be saved");
    std::string const t = j["t"];
    if (t == "str") {
        if (!j.contains("utf8") || !j["utf8"].is_string()) bad("str without utf8");
        auto bytes = j["utf8"].get<std::string>();
        if (!valid_utf8(bytes)) bad("str contains invalid UTF-8");
        if (j.contains("len") && (!j["len"].is_number_unsigned() || j["len"].get<std::size_t>() != bytes.size())) bad("byte length does not match");
        return bytes;
    }
    if (t == "bytes") {
        if (!j.contains("hex") || !j["hex"].is_string()) bad("bytes without hex");
        auto bytes = from_hex(j["hex"].get<std::string>());
        if (j.contains("len") && (!j["len"].is_number_unsigned() || j["len"].get<std::size_t>() != bytes.size())) bad("byte length does not match");
        return bytes;
    }
    bad("expected a byte string");
}

Value value_from_json(nlohmann::json const& j, int depth)
{
    if (depth > 128) bad("nesting too deep");
    if (!j.is_object() || !j.contains("t") || !j["t"].is_string()) bad("missing tag");
    if (j.value("truncated", false)) bad("a truncated display value cannot be saved");
    std::string const t = j["t"];
    if (t == "int") {
        if (!j.contains("v") || !j["v"].is_string()) bad("int without decimal text");
        std::string const text = j["v"];
        // Canonical bencode integer text: no leading zeros, no "-0".
        bool ok = !text.empty();
        std::size_t start = (ok && text[0] == '-') ? 1 : 0;
        if (start == text.size()) ok = false;
        for (std::size_t i = start; ok && i < text.size(); ++i) ok = text[i] >= '0' && text[i] <= '9';
        if (ok && text[start] == '0' && (text.size() > start + 1 || start == 1)) ok = false;
        if (!ok) bad("non-canonical integer '" + text + "'");
        return Value::integer_text(text);
    }
    if (t == "str" || t == "bytes") return Value::string(bytes_from_json(j));
    if (t == "list") {
        if (!j.contains("items") || !j["items"].is_array()) bad("list without items");
        Value::List items;
        for (auto const& item : j["items"]) items.push_back(value_from_json(item, depth + 1));
        return Value::list(std::move(items));
    }
    if (t == "dict") {
        if (!j.contains("entries") || !j["entries"].is_array()) bad("dict without entries");
        Value::Dictionary entries;
        for (auto const& e : j["entries"]) {
            if (!e.is_object() || !e.contains("key") || !e.contains("value")) bad("dict entry without key or value");
            entries.push_back({bytes_from_json(e["key"]), value_from_json(e["value"], depth + 1)});
        }
        return Value::dictionary(std::move(entries)); // sorts; rejects duplicates
    }
    bad("unknown tag '" + t + "'");
}

} // namespace

nlohmann::json bencode_to_json(Value const& value, DisplayBudget const& budget)
{
    return Converter(budget).value(value);
}

Value bencode_from_json(nlohmann::json const& j)
{
    return value_from_json(j, 0);
}

} // namespace tc::bridge
