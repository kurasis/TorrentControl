#include "tc/core/bencode.hpp"
#include "tc/core/error.hpp"
#include "tc/core/field_registry.hpp"
#include "tc/core/metainfo.hpp"
#include "tc/bridge/bencode_json.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <optional>

namespace {
void require(bool condition) { if (!condition) std::abort(); }
}
extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    if (size > 65536) return 0;
    std::string bytes(reinterpret_cast<char const*>(data), size);
    tc::core::bencode::Limits limits{64, 4096};
    std::optional<tc::core::bencode::Value> parsed;
    try { parsed = tc::core::bencode::parse(bytes, limits); }
    catch (tc::core::CoreError const&) { }
    if (parsed) {
        auto const& value = *parsed;
        auto canonical = tc::core::bencode::encode(value);
        require(canonical == bytes);
        require(tc::core::bencode::encode(tc::core::bencode::parse(canonical, limits)) == canonical);
        tc::bridge::DisplayBudget complete{8192, 65536, 2 * 1024 * 1024};
        auto display = tc::bridge::bencode_to_json(value, complete);
        require(tc::core::metadata_values_equal(tc::bridge::bencode_from_json(display), value));
        tc::bridge::DisplayBudget small{16, 8, 2048};
        require(tc::bridge::bencode_to_json(value, small).dump().size() <= small.max_output_bytes);
    }
    std::optional<tc::core::Metainfo> parsed_metainfo;
    try { parsed_metainfo = tc::core::Metainfo::parse(bytes, {65536, limits}); }
    catch (tc::core::CoreError const&) { }
    if (parsed_metainfo) {
        auto const& original = *parsed_metainfo;
        (void)tc::core::validate_metainfo(original);
        (void)tc::core::make_magnet(original);
        auto edited = tc::core::Metainfo::parse(tc::core::apply_outer_edit(original,
            {{"comment", tc::core::bencode::Value::string(std::string("fuzz\0comment", 12))}}), {131072, {66, 8192}});
        require(edited.raw_info() == original.raw_info());
        require(edited.info_hashes().v1 == original.info_hashes().v1);
        require(edited.info_hashes().v2 == original.info_hashes().v2);
        auto info_edit = tc::core::apply_info_edit(original,
            {{"source", tc::core::bencode::Value::string("fuzz")}}, {true});
        auto changed = tc::core::Metainfo::parse(info_edit.bytes, {131072, {66, 8192}});
        require(info_edit.new_hashes.v1 == changed.info_hashes().v1);
        require(info_edit.new_hashes.v2 == changed.info_hashes().v2);
    }
    return 0;
}
