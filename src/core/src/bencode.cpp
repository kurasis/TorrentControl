#include "tc/core/bencode.hpp"

#include <utility>

#include "tc/core/error.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <unordered_set>

namespace tc::core::bencode {

namespace {

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// Canonical bencode integer text: optional '-', no leading zeros, no "-0".
bool is_canonical_integer(std::string_view s) noexcept
{
    if (s.empty()) return false;
    std::size_t i = 0;
    if (s[0] == '-') {
        if (s.size() == 1) return false;
        i = 1;
        if (s[1] == '0') return false;
    }
    if (s[i] == '0' && s.size() > i + 1) return false;
    for (; i < s.size(); ++i)
        if (!is_digit(s[i])) return false;
    return true;
}

// Bencode orders keys by unsigned raw bytes, not by (possibly signed) char.
bool bytes_less(std::string_view a, std::string_view b) noexcept
{
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
        [](char x, char y) { return static_cast<unsigned char>(x) < static_cast<unsigned char>(y); });
}

bool key_less(DictEntry const& a, DictEntry const& b) noexcept { return bytes_less(a.key, b.key); }

[[noreturn]] void fail(std::size_t offset, std::string_view what)
{
    throw CoreError(ErrorCode::InvalidMetainfo,
        "Malformed bencode at byte " + std::to_string(offset) + ": " + std::string(what));
}

} // namespace

class Parser {
public:
    Parser(std::string_view in, Limits const& limits) : in_(in), limits_(limits) {}

    Value parse_root()
    {
        Value v = parse_value(0);
        if (pos_ != in_.size()) fail(pos_, "trailing data after the root value");
        return v;
    }

private:
    Value parse_value(std::size_t depth)
    {
        if (depth >= limits_.max_depth)
            throw CoreError(ErrorCode::ResourceLimit,
                "Bencode nesting exceeds the limit of " + std::to_string(limits_.max_depth) + " levels");
        if (++nodes_ > limits_.max_nodes)
            throw CoreError(ErrorCode::ResourceLimit,
                "Bencode node count exceeds the limit of " + std::to_string(limits_.max_nodes));
        if (pos_ >= in_.size()) fail(pos_, "unexpected end of input");

        std::size_t const start = pos_;
        Value v;
        char const c = in_[pos_];
        if (c == 'i') {
            v.type_ = Type::Integer;
            v.text_ = parse_integer();
        } else if (is_digit(c)) {
            v.type_ = Type::String;
            v.text_ = std::string(parse_string());
        } else if (c == 'l') {
            v.type_ = Type::List;
            ++pos_;
            while (true) {
                if (pos_ >= in_.size()) fail(pos_, "unterminated list");
                if (in_[pos_] == 'e') break;
                v.list_.push_back(parse_value(depth + 1));
            }
            ++pos_;
        } else if (c == 'd') {
            v.type_ = Type::Dictionary;
            ++pos_;
            // Keys of an unsorted dictionary are tracked in a hash set so
            // duplicate detection stays linear for adversarial input.
            // Views point into the input buffer, which outlives this parse.
            std::vector<std::string_view> keys;
            std::unordered_set<std::string_view> seen;
            while (true) {
                if (pos_ >= in_.size()) fail(pos_, "unterminated dictionary");
                if (in_[pos_] == 'e') break;
                std::size_t const key_offset = pos_;
                if (!is_digit(in_[pos_])) fail(pos_, "dictionary key is not a byte string");
                std::string_view const key = parse_string();
                if (!keys.empty()) {
                    if (key == keys.back()) fail(key_offset, "duplicate dictionary key");
                    if (v.canonical_order_ && bytes_less(key, keys.back())) {
                        v.canonical_order_ = false;
                        seen.insert(keys.begin(), keys.end());
                    }
                }
                if (!v.canonical_order_ && !seen.insert(key).second) fail(key_offset, "duplicate dictionary key");
                keys.push_back(key);
                Value child = parse_value(depth + 1);
                v.dict_.push_back(DictEntry{std::string(key), std::move(child)});
            }
            ++pos_;
        } else {
            fail(pos_, "unexpected character");
        }
        v.raw_ = ByteRange{start, pos_ - start};
        return v;
    }

    std::string parse_integer()
    {
        std::size_t const start = pos_;
        ++pos_; // 'i'
        std::size_t const end = in_.find('e', pos_);
        if (end == std::string_view::npos) fail(start, "unterminated integer");
        std::string_view digits = in_.substr(pos_, end - pos_);
        if (!is_canonical_integer(digits)) fail(start, "invalid integer");
        pos_ = end + 1;
        return std::string(digits);
    }

    std::string_view parse_string()
    {
        std::size_t const start = pos_;
        std::size_t const colon = in_.find(':', pos_);
        if (colon == std::string_view::npos) fail(start, "unterminated string length");
        std::string_view len_text = in_.substr(pos_, colon - pos_);
        if (len_text.empty() || (len_text.size() > 1 && len_text[0] == '0'))
            fail(start, "invalid string length");
        std::uint64_t len = 0;
        auto const [p, ec] = std::from_chars(len_text.data(), len_text.data() + len_text.size(), len);
        if (ec != std::errc{} || p != len_text.data() + len_text.size())
            fail(start, "invalid string length");
        pos_ = colon + 1;
        if (len > in_.size() - pos_) fail(start, "string extends beyond end of input");
        std::string_view s = in_.substr(pos_, static_cast<std::size_t>(len));
        pos_ += static_cast<std::size_t>(len);
        return s;
    }

    std::string_view in_;
    Limits limits_;
    std::size_t pos_ = 0;
    std::size_t nodes_ = 0;
};

Value::Value() = default;
// DictEntry is complete here. Clang eagerly instantiates vector special members
// at default arguments in Value's declaration when these remain implicit.
Value::~Value() = default;
Value::Value(Value const&) = default;
Value::Value(Value&&) noexcept = default;
Value& Value::operator=(Value const&) = default;
Value& Value::operator=(Value&&) noexcept = default;

Value Value::dictionary() { return dictionary({}); }

Value Value::integer(std::int64_t v)
{
    Value r;
    r.type_ = Type::Integer;
    r.text_ = std::to_string(v);
    return r;
}

Value Value::integer_text(std::string decimal)
{
    if (!is_canonical_integer(decimal))
        throw CoreError(ErrorCode::InvalidArgument, "Not a canonical bencode integer");
    Value r;
    r.type_ = Type::Integer;
    r.text_ = std::move(decimal);
    return r;
}

Value Value::string(std::string bytes)
{
    Value r;
    r.type_ = Type::String;
    r.text_ = std::move(bytes);
    return r;
}

Value Value::list(List items)
{
    Value r;
    r.type_ = Type::List;
    r.list_ = std::move(items);
    return r;
}

Value Value::dictionary(Dictionary entries)
{
    std::stable_sort(entries.begin(), entries.end(), key_less);
    for (std::size_t i = 1; i < entries.size(); ++i)
        if (entries[i].key == entries[i - 1].key)
            throw CoreError(ErrorCode::InvalidArgument, "Duplicate dictionary key");
    Value r;
    r.type_ = Type::Dictionary;
    r.dict_ = std::move(entries);
    return r;
}

std::optional<std::int64_t> Value::as_int64() const
{
    if (type_ != Type::Integer) return std::nullopt;
    std::int64_t v = 0;
    auto const [p, ec] = std::from_chars(text_.data(), text_.data() + text_.size(), v);
    if (ec != std::errc{} || p != text_.data() + text_.size()) return std::nullopt;
    return v;
}

Value const* Value::find(std::string_view key) const
{
    if (type_ != Type::Dictionary) return nullptr;
    for (auto const& e : dict_)
        if (e.key == key) return &e.value;
    return nullptr;
}

Value* Value::find(std::string_view key)
{
    return const_cast<Value*>(std::as_const(*this).find(key));
}

void Value::set(std::string key, Value v)
{
    if (type_ != Type::Dictionary)
        throw CoreError(ErrorCode::InvalidArgument, "set() on a non-dictionary value");
    for (auto& e : dict_) {
        if (e.key == key) {
            e.value = std::move(v);
            return;
        }
    }
    DictEntry entry{std::move(key), std::move(v)};
    auto it = std::upper_bound(dict_.begin(), dict_.end(), entry, key_less);
    dict_.insert(it, std::move(entry));
}

bool Value::erase(std::string_view key)
{
    auto it = std::find_if(dict_.begin(), dict_.end(), [&](DictEntry const& e) { return e.key == key; });
    if (it == dict_.end()) return false;
    dict_.erase(it);
    return true;
}

Value parse(std::string_view input, Limits const& limits)
{
    return Parser(input, limits).parse_root();
}

void encode_to(Value const& v, std::string& out)
{
    switch (v.type()) {
    case Type::Integer:
        out += 'i';
        out += v.text();
        out += 'e';
        break;
    case Type::String:
        out += std::to_string(v.text().size());
        out += ':';
        out += v.text();
        break;
    case Type::List:
        out += 'l';
        for (auto const& item : v.items()) encode_to(item, out);
        out += 'e';
        break;
    case Type::Dictionary:
        out += 'd';
        for (auto const& e : v.entries()) {
            out += std::to_string(e.key.size());
            out += ':';
            out += e.key;
            encode_to(e.value, out);
        }
        out += 'e';
        break;
    }
}

std::string encode(Value const& v)
{
    std::string out;
    encode_to(v, out);
    return out;
}

} // namespace tc::core::bencode
