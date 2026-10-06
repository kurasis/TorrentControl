#pragma once
#include <cstddef>
#include <cstdint>

namespace tc::core {
// Payload allocations only. Manifests, hash slots and engine/parse copies
// are separate; these counters are not total process memory.
struct HashMetrics {
    std::size_t unit_bytes = 0;
    std::size_t allocated_buffers = 0;
    std::uint64_t peak_payload_buffer_bytes = 0;
    int hash_workers = 0;
    std::size_t max_read_request_bytes = 0;
    std::uint64_t hash_slot_bytes = 0;
};
} // namespace tc::core
