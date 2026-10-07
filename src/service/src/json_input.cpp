#include "tc/service/json_input.hpp"

#include "tc/core/error.hpp"

namespace tc::service {

nlohmann::json parse_json_input(std::string_view bytes)
{
    std::size_t depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (char c : bytes) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == '[' || c == '{') {
            if (++depth > max_json_input_depth)
                throw core::CoreError(core::ErrorCode::ResourceLimit, "JSON nesting exceeds 512 containers");
        } else if ((c == ']' || c == '}') && depth != 0) {
            --depth;
        }
    }
    return nlohmann::json::parse(bytes, nullptr, false);
}

} // namespace tc::service
