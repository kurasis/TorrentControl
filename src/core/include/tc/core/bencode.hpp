#pragma once

// Binary-safe bencode model (specification section 7.4).
//
// - Byte strings and dictionary keys are arbitrary bytes held in std::string.
// - Integers keep their canonical decimal text so values beyond 64-bit range
//   survive a round trip without narrowing.
// - Every parsed value remembers the byte range it occupied in the input, so
//   callers can copy original bytes verbatim (for example the raw `info`
//   dictionary during an outer-only edit).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tc::core::bencode {

struct Limits {
    std::size_t max_depth = 128;
    std::size_t max_nodes = 2'000'000;
};

enum class Type : std::uint8_t { Integer, String, List, Dictionary };

struct ByteRange {
    std::size_t offset = 0;
    std::size_t size = 0;
};

struct DictEntry;

class Value {
public:
    using List = std::vector<Value>;
    using Dictionary = std::vector<DictEntry>;

    Value();
    ~Value();
    Value(Value const&);
    Value(Value&&) noexcept;
    Value& operator=(Value const&);
    Value& operator=(Value&&) noexcept;

    static Value integer(std::int64_t v);
    // `decimal` must be canonical bencode integer text ("0", "-5", "123...").
    static Value integer_text(std::string decimal);
    static Value string(std::string bytes);
    static Value list(List items = {});
    // Sorts entries by raw key bytes; throws on duplicate keys.
    static Value dictionary();
    static Value dictionary(Dictionary entries);

    Type type() const noexcept { return type_; }
    bool is_integer() const noexcept { return type_ == Type::Integer; }
    bool is_string() const noexcept { return type_ == Type::String; }
    bool is_list() const noexcept { return type_ == Type::List; }
    bool is_dictionary() const noexcept { return type_ == Type::Dictionary; }

    // Integer: canonical decimal text. String: raw bytes.
    std::string const& text() const noexcept { return text_; }
    std::optional<std::int64_t> as_int64() const;

    List const& items() const noexcept { return list_; }
    List& items() noexcept { return list_; }

    Dictionary const& entries() const noexcept { return dict_; }
    Value const* find(std::string_view key) const;
    // Mutable access to a value; replacing it does not change the key order.
    Value* find(std::string_view key);
    // Inserts or replaces, keeping keys sorted by raw bytes.
    void set(std::string key, Value v);
    bool erase(std::string_view key);

    // Position of this value in the parsed input. Size 0 for constructed values.
    ByteRange raw() const noexcept { return raw_; }
    // False when a parsed dictionary's keys were not in canonical sorted order.
    bool canonical_order() const noexcept { return canonical_order_; }

private:
    friend class Parser;

    Type type_ = Type::String;
    std::string text_;
    List list_;
    Dictionary dict_;
    ByteRange raw_;
    bool canonical_order_ = true;
};

struct DictEntry {
    std::string key;
    Value value;
};

// Parses exactly one value spanning the whole input. Throws CoreError with
// InvalidMetainfo (malformed, duplicate keys, trailing data) or ResourceLimit.
Value parse(std::string_view input, Limits const& limits = {});

std::string encode(Value const& v);
void encode_to(Value const& v, std::string& out);

} // namespace tc::core::bencode
