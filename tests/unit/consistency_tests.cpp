// Engine pipeline, source consistency (section 9.2) and preflight limits.

#include "tc/core/error.hpp"
#include "tc/core/metainfo.hpp"
#include "tc/core/torrent_engine.hpp"

#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <optional>

using namespace tc::core;
namespace fs = std::filesystem;

namespace {

constexpr int kib = 1024;

std::optional<CoreError> create_failure(Manifest const& m, CreateOptions const& o, PayloadSource& src,
    ProgressCallback const& cb = {})
{
    try {
        create_torrent(m, o, src, {}, cb);
    } catch (CoreError const& e) {
        return e;
    }
    return std::nullopt;
}

// Reports a different last-write time once the file has been read to the end,
// as if another process wrote to it during hashing.
class ChangingReader final : public PayloadReader {
public:
    explicit ChangingReader(std::unique_ptr<PayloadReader> inner) : inner_(std::move(inner)) {}
    std::size_t read(std::span<std::byte> buffer) override
    {
        std::size_t const n = inner_->read(buffer);
        if (n == 0) finished_ = true;
        return n;
    }
    std::optional<native::FileObservation> observe() override
    {
        auto o = inner_->observe();
        if (o && finished_) o->last_write += 1;
        return o;
    }

private:
    std::unique_ptr<PayloadReader> inner_;
    bool finished_ = false;
};

class ChangingSource final : public PayloadSource {
public:
    explicit ChangingSource(PayloadSource& inner) : inner_(inner) {}
    std::unique_ptr<PayloadReader> open(ManifestEntry const& e) override
    {
        return std::make_unique<ChangingReader>(inner_.open(e));
    }

private:
    PayloadSource& inner_;
};

} // namespace

TEST_CASE("worker count and buffer budget do not change the output", "[engine][P01]")
{
    tc::test::TempDir dir;
    fs::path const root = dir.path() / "Set";
    for (int i = 0; i < 12; ++i)
        tc::test::write_file(root / ("f" + std::to_string(i)), static_cast<std::uint64_t>(i) * 37 * kib + 5,
            static_cast<std::uint32_t>(i));
    tc::test::write_file(root / "big.bin", 5 * 1024 * 1024 + 3, 77);
    Manifest const m = scan_source(root);
    auto files = make_file_payload_source();

    for (auto const format : {TorrentFormat::V1, TorrentFormat::V2, TorrentFormat::Hybrid}) {
        INFO(to_string(format));
        CreateOptions base;
        base.format = format;
        base.piece_length = 64 * kib;

        CreateOptions serial = base;
        serial.hash_threads = 1;
        serial.buffer_budget = 64 * kib; // one piece in flight
        serial.read_buffer_size = 16 * kib;

        CreateOptions parallel = base;
        parallel.hash_threads = 4;
        parallel.buffer_budget = 8 * 1024 * 1024;
        parallel.read_buffer_size = 100 * kib;

        auto const one = create_torrent(m, serial, *files);
        auto const four = create_torrent(m, parallel, *files);
        for (auto const* result : {&one, &four}) {
            auto const budget = result == &one ? serial.buffer_budget : parallel.buffer_budget;
            CHECK(result->hashing.peak_payload_buffer_bytes <= budget);
            CHECK(result->hashing.peak_payload_buffer_bytes <= result->preflight.estimate.payload_buffer_bytes);
            CHECK(result->hashing.max_read_request_bytes <= (result == &one ? serial.read_buffer_size : parallel.read_buffer_size));
            CHECK(result->hashing.hash_slot_bytes > 0);
        }
        std::string const& a = one.torrent_bytes;
        std::string const& b = four.torrent_bytes;
        CHECK(a == b);
        CHECK(validate_metainfo(Metainfo::parse(a)).empty());
    }
}

TEST_CASE("a piece larger than the hard buffer budget is refused before reading", "[engine][P02]")
{
    tc::test::TempDir dir;
    fs::path const file = dir.path() / "data.bin";
    tc::test::write_file(file, 3 * 1024 * 1024 + 1);
    Manifest const m = scan_source(file);
    auto files = make_file_payload_source();
    CreateOptions o;
    o.piece_length = 1024 * 1024;
    o.buffer_budget = 64 * kib;
    o.hash_threads = 2;
    tc::test::CountingSource counting(*files);
    auto const error = create_failure(m, o, counting);
    REQUIRE(error);
    CHECK(error->code() == ErrorCode::ResourceLimit);
    CHECK(error->phase() == Phase::Preflight);
    CHECK(counting.opens.empty());
    o.buffer_budget = 1024 * 1024;
    auto const result = create_torrent(m, o, counting);
    CHECK(result.num_pieces == 4);
    CHECK(result.hashing.peak_payload_buffer_bytes == o.buffer_budget);
    CHECK(result.hashing.hash_workers == 1);
}

TEST_CASE("a source replaced or modified after the manifest freeze is detected", "[engine][W03]")
{
    tc::test::TempDir dir;
    fs::path const root = dir.path() / "Data";
    tc::test::write_file(root / "a.bin", 100 * kib, 1);
    tc::test::write_file(root / "b.bin", 100 * kib, 2);
    Manifest const m = scan_source(root);
    auto files = make_file_payload_source();

    SECTION("replaced by another file of the same size")
    {
        tc::test::write_file(root / "b.new", 100 * kib, 3);
        fs::rename(root / "b.new", root / "b.bin");
        auto const e = create_failure(m, {}, *files);
        REQUIRE(e);
        CHECK(e->code() == ErrorCode::SourceChanged);
        CHECK(e->phase() == Phase::Hashing);
        CHECK(e->source_id() == m.entries[1].source_id);
        CHECK(e->retryable());
    }
    SECTION("modified in place without a size change")
    {
        tc::test::touch_later(root / "a.bin");
        auto const e = create_failure(m, {}, *files);
        REQUIRE(e);
        CHECK(e->code() == ErrorCode::SourceChanged);
    }
}

TEST_CASE("a source that changes while it is read is detected", "[engine][W03]")
{
    tc::test::TempDir dir;
    fs::path const file = dir.path() / "data.bin";
    tc::test::write_file(file, 200 * kib);
    Manifest const m = scan_source(file);
    auto files = make_file_payload_source();
    ChangingSource changing(*files);

    auto const e = create_failure(m, {}, changing);
    REQUIRE(e);
    CHECK(e->code() == ErrorCode::SourceChanged);
    CHECK(std::string(e->what()).find("while it was being read") != std::string::npos);
}

TEST_CASE("a source changed after it was hashed blocks the result", "[engine][W03]")
{
    tc::test::TempDir dir;
    fs::path const root = dir.path() / "Data";
    tc::test::write_file(root / "a.bin", 64 * kib, 1);
    tc::test::write_file(root / "b.bin", 64 * kib, 2);
    Manifest const m = scan_source(root);
    auto files = make_file_payload_source();

    bool touched = false;
    auto const e = create_failure(m, {}, *files, [&](CreateProgress const& p) {
        // a.bin is done; change it while b.bin is still pending.
        if (!touched && p.files_completed == 1) {
            tc::test::touch_later(root / "a.bin");
            touched = true;
        }
    });
    REQUIRE(touched);
    REQUIRE(e);
    CHECK(e->code() == ErrorCode::SourceChanged);
    CHECK(e->phase() == Phase::Validating);
}

TEST_CASE("absurd piece counts fail in preflight before allocation", "[engine][P02]")
{
    Manifest m;
    m.name = "huge.bin";
    m.mode = LayoutMode::SingleFile;
    m.entries.push_back(ManifestEntry{"src-0", "/nonexistent", {}, 1ull << 46}); // 64 TiB

    CreateOptions o;
    o.format = TorrentFormat::V1;
    o.piece_length = 16 * kib; // 2^32 pieces
    try {
        preflight(m, o);
        FAIL_CHECK("expected a resource-limit failure");
    } catch (CoreError const& e) {
        CHECK(e.code() == ErrorCode::ResourceLimit);
        CHECK(e.phase() == Phase::Preflight);
    }
}

TEST_CASE("large planned memory needs explicit acceptance", "[engine][P02]")
{
    tc::test::TempDir dir;
    fs::path const file = dir.path() / "data.bin";
    tc::test::write_file(file, 100);
    Manifest const m = scan_source(file);

    CreateOptions o;
    o.buffer_budget = 600ull * 1024 * 1024; // estimate exceeds the 512 MiB warning level
    try {
        preflight(m, o);
        FAIL_CHECK("expected a resource-limit failure");
    } catch (CoreError const& e) {
        CHECK(e.code() == ErrorCode::ResourceLimit);
    }
    o.accept_large_resource_use = true;
    PreflightReport const r = preflight(m, o);
    CHECK(r.estimate.memory_bytes > planned_memory_warning_bytes);
    CHECK_FALSE(r.warnings.empty());
}

TEST_CASE("preflight reports padding and estimates before hashing", "[engine][F11]")
{
    tc::test::TempDir dir;
    fs::path const root = dir.path() / "Tiny";
    for (int i = 0; i < 50; ++i) tc::test::write_file(root / ("t" + std::to_string(i)), 10, static_cast<std::uint32_t>(i));
    Manifest const m = scan_source(root);

    CreateOptions o;
    o.format = TorrentFormat::Hybrid;
    PreflightReport const r = preflight(m, o);
    CHECK(r.piece.piece_length == 256 * kib);
    CHECK(r.piece.padding_warning);
    CHECK(r.estimate.hash_bytes == r.piece.estimated_hash_bytes);
    CHECK(r.estimate.metainfo_bytes > r.estimate.hash_bytes);
    bool padding_warning = false;
    for (auto const& w : r.warnings) padding_warning |= w.message.find("padding") != std::string::npos;
    CHECK(padding_warning);
}

TEST_CASE("zero payload budget cannot open sources and explicit workers are bounded", "[engine][P02]")
{
    tc::test::TempDir dir;
    tc::test::write_file(dir.path() / "data.bin", 65537);
    auto m = scan_source(dir.path() / "data.bin");
    auto files = make_file_payload_source();
    tc::test::CountingSource counting(*files);
    CreateOptions o;
    o.piece_length = 16384; o.buffer_budget = 0;
    auto error = create_failure(m, o, counting);
    REQUIRE(error);
    CHECK(error->code() == ErrorCode::ResourceLimit);
    CHECK(counting.opens.empty());
    o.buffer_budget = 3 * 16384 + 1; o.hash_threads = 64;
    auto result = create_torrent(m, o, counting);
    CHECK(result.hashing.hash_workers == 3);
    CHECK(result.hashing.unit_bytes == 16384);
    CHECK(result.hashing.peak_payload_buffer_bytes <= o.buffer_budget);
    CHECK(result.preflight.estimate.payload_buffer_bytes == 3 * 16384);
}
