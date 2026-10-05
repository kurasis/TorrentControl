#pragma once

#include "tc/core/payload_source.hpp"
#include "tc/core/torrent_engine.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace tc::test {

// A unique empty directory, removed when the object goes out of scope.
class TempDir {
public:
    TempDir();
    ~TempDir();
    TempDir(TempDir const&) = delete;
    TempDir& operator=(TempDir const&) = delete;

    std::filesystem::path const& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// Writes `size` deterministic pseudo-random bytes derived from `seed`.
void write_file(std::filesystem::path const& p, std::uint64_t size, std::uint32_t seed = 1);
std::string read_all(std::filesystem::path const& p);
void write_bytes(std::filesystem::path const& p, std::string_view bytes);

// Moves a file's last-write time forward without changing its size.
void touch_later(std::filesystem::path const& p);

// True when the process can bypass file permissions (POSIX root), which makes
// permission-based failure tests meaningless.
bool permissions_are_bypassed();

// Creates a torrent from `source` with fixed metadata (no creation date).
std::string make_torrent(std::filesystem::path const& source, core::TorrentFormat format, int piece_length);

// Wraps a real PayloadSource and records how many bytes were read from each
// source and how many times each source was opened.
class CountingSource final : public core::PayloadSource {
public:
    explicit CountingSource(core::PayloadSource& inner) : inner_(inner) {}
    std::unique_ptr<core::PayloadReader> open(core::ManifestEntry const& entry) override;

    std::map<std::string, std::uint64_t> bytes_read;
    std::map<std::string, int> opens;

private:
    core::PayloadSource& inner_;
};

} // namespace tc::test
