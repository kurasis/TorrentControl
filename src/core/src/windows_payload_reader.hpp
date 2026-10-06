#pragma once
#ifdef _WIN32
#include "tc/core/payload_source.hpp"
#include <string_view>

namespace tc::core::detail {
// Internal adapter for an already opened overlapped read handle. Takes
// ownership, including on allocation failure; used by the ordinary source
// and the real-kernel pending-I/O regression fixture.
std::unique_ptr<PayloadReader> take_windows_payload_reader(void* handle, std::string_view label);
}
#endif
