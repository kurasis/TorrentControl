#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <string>
#include <string_view>

namespace tc::service::view {
using nlohmann::json;
inline constexpr std::size_t page_bytes = 32 * 1024;

// Cut only at a UTF-8 boundary; the original remains in the native model.
inline std::string preview(std::string_view s, std::size_t limit = 512)
{
    auto end = std::min(s.size(), limit);
    while (end < s.size() && end > 0 && (static_cast<unsigned char>(s[end]) & 0xc0) == 0x80) --end;
    return std::string(s.substr(0, end));
}
inline json compact(json const& value)
{
    if (value.is_string()) return preview(value.get_ref<std::string const&>());
    if (value.is_array()) {
        json result = json::array();
        for (std::size_t i = 0; i < std::min<std::size_t>(value.size(), 3); ++i) result.push_back(compact(value[i]));
        return result;
    }
    if (value.is_object()) {
        json result = json::object();
        for (auto const& [key, item] : value.items()) result[key] = compact(item);
        return result;
    }
    return value;
}
template<class Row>
json page(std::size_t total, std::size_t offset, std::size_t limit, std::string const& revision, Row row)
{
    json items = json::array();
    std::size_t bytes = 0;
    for (auto i = offset; i < total && items.size() < std::min<std::size_t>(limit, 50); ++i) {
        json value = row(i);
        auto size = value.dump(-1, ' ', false, json::error_handler_t::replace).size();
        if (size > page_bytes) {
            value = compact(value);
            if (value.is_object()) value["displayTruncated"] = true;
            size = value.dump(-1, ' ', false, json::error_handler_t::replace).size();
        }
        if (!items.empty() && size > page_bytes - std::min(bytes, page_bytes)) break;
        bytes += size;
        items.push_back(std::move(value));
    }
    auto next = offset + items.size();
    return {{"offset", offset}, {"total", total}, {"items", std::move(items)},
        {"nextOffset", next < total ? json(next) : json(nullptr)}, {"revision", revision}};
}
inline json profile(Profile const& p)
{
    return {{"id", p.id}, {"name", p.name}, {"builtin", p.builtin},
        {"format", std::string(core::to_string(p.format))}, {"private", p.private_flag}};
}
inline json source(SourceSpec const& s)
{
    json value{{"id", s.id}, {"path", preview(core::to_utf8(s.path), 1024)},
        {"name", preview(core::to_utf8(s.path.filename()), 256)}, {"isDirectory", s.is_directory},
        {"recursive", s.recursive}, {"followLinks", s.follow_links}, {"skipCloud", s.skip_cloud},
        {"exclusions", json::array()}, {"exclusionsTotal", s.exclusions.size()}, {"exclusionsPaged", false}};
    bool small = s.exclusions.size() <= 3;
    for (auto const& pattern : s.exclusions) if (pattern.size() > 1024) small = false;
    if (small) value["exclusions"] = s.exclusions;
    else value["exclusionsPaged"] = true;
    return value;
}
} // namespace tc::service::view
