#pragma once

#include "tc/core/metainfo.hpp"
#include <span>
#include <string_view>

namespace tc::core {

// Keys are raw bytes. An absent descriptor means a typed expert extension,
// never an inferred text field. File entries are inspect/preserve only.
struct FieldDescriptor {
    std::string_view scope; // top, info, file
    std::string_view key;
    std::string_view type;
    std::string_view formats;
    std::string_view support; // form, expert, computed, preserve
    bool editable;
    bool affects_hash;
    std::string_view reference;
    std::string_view validation;
};

std::span<FieldDescriptor const> field_registry();
FieldDescriptor const* find_field(std::string_view scope, std::string_view key);
bool metadata_values_equal(bencode::Value const& a, bencode::Value const& b);

struct MetadataPreview {
    std::string bytes;
    InfoHashes old_hashes;
    InfoHashes new_hashes;
    bool info_changed = false;
    bool removed_signatures = false;
};

// Validate patches, discard semantic no-ops, preserve untouched raw values,
// then validate the complete candidate. No payload files are read or hashed.
MetadataPreview preview_metadata_edit(Metainfo const& original,
    OuterEdit const& outer, InfoEdit const& info, bool remove_signatures = false);

} // namespace tc::core
