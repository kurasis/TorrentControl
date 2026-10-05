#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace tc::test {

TempDir::TempDir()
{
    static std::atomic<int> counter{0};
    auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = fs::temp_directory_path() / ("tc-test-" + std::to_string(stamp) + "-" + std::to_string(counter++));
    fs::create_directories(path_);
}

TempDir::~TempDir()
{
    std::error_code ec;
    fs::remove_all(path_, ec);
}

void write_file(fs::path const& p, std::uint64_t size, std::uint32_t seed)
{
    fs::create_directories(p.parent_path());
    std::mt19937 rng(seed);
    std::vector<char> buf(64 * 1024);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    while (size > 0) {
        auto const n = static_cast<std::size_t>(std::min<std::uint64_t>(size, buf.size()));
        for (std::size_t i = 0; i < n; ++i) buf[i] = static_cast<char>(rng() & 0xff);
        out.write(buf.data(), static_cast<std::streamsize>(n));
        size -= n;
    }
}

std::string read_all(fs::path const& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void write_bytes(fs::path const& p, std::string_view bytes)
{
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void touch_later(fs::path const& p)
{
    fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(5));
}

bool permissions_are_bypassed()
{
#ifdef _WIN32
    return true; // POSIX permission bits do not apply; tests use other means
#else
    return ::geteuid() == 0;
#endif
}

std::string make_torrent(fs::path const& source, core::TorrentFormat format, int piece_length)
{
    core::Manifest const m = core::scan_source(source);
    core::CreateOptions o;
    o.format = format;
    o.piece_length = piece_length;
    auto files = core::make_file_payload_source();
    return core::create_torrent(m, o, *files).torrent_bytes;
}

namespace {

class CountingReader final : public core::PayloadReader {
public:
    CountingReader(std::unique_ptr<core::PayloadReader> inner, std::uint64_t& counter)
        : inner_(std::move(inner)), counter_(counter)
    {
    }
    std::size_t read(std::span<std::byte> buffer) override
    {
        std::size_t const n = inner_->read(buffer);
        counter_ += n;
        return n;
    }
    std::optional<core::native::FileObservation> observe() override { return inner_->observe(); }

private:
    std::unique_ptr<core::PayloadReader> inner_;
    std::uint64_t& counter_;
};

} // namespace

std::unique_ptr<core::PayloadReader> CountingSource::open(core::ManifestEntry const& entry)
{
    ++opens[entry.source_id];
    return std::make_unique<CountingReader>(inner_.open(entry), bytes_read[entry.source_id]);
}

} // namespace tc::test
