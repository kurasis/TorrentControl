#pragma once

// Virtual-to-physical source adapter (specification section 3.1). The engine
// never assumes that torrent paths exist under a common base directory; it
// asks a PayloadSource to open each manifest entry by its native path.

#include "tc/core/manifest.hpp"
#include "tc/core/native_fs.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>

namespace tc::core {

class PayloadReader {
public:
    virtual ~PayloadReader() = default;
    // Reads up to buffer.size() bytes. Returns 0 only at end of file. Throws
    // CoreError(SourceUnreadable) on I/O failure.
    virtual std::size_t read(std::span<std::byte> buffer) = 0;
    // Observation of the opened object (identity, size, last-write time), or
    // std::nullopt when the source cannot provide one.
    virtual std::optional<native::FileObservation> observe() { return std::nullopt; }
};

class PayloadSource {
public:
    virtual ~PayloadSource() = default;
    // Throws CoreError(SourceMissing / SourceUnreadable).
    virtual std::unique_ptr<PayloadReader> open(ManifestEntry const& entry) = 0;
};

// Reads ordinary files. On Windows the handle denies write and delete sharing
// while it is open, so concurrent modification during hashing surfaces as a
// sharing-violation error instead of silently hashing changing data.
std::unique_ptr<PayloadSource> make_file_payload_source();

} // namespace tc::core
