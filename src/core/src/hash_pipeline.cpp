#include "hash_pipeline.hpp"

#include "tc/core/error.hpp"

#include <libtorrent/hasher.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

namespace tc::core::detail {

namespace {

constexpr std::size_t max_unit_bytes = 16u * 1024 * 1024;

Sha256Digest hash_pair(Sha256Digest const& a, Sha256Digest const& b)
{
    lt::hasher256 h;
    h.update(reinterpret_cast<char const*>(a.data()), 32);
    h.update(reinterpret_cast<char const*>(b.data()), 32);
    auto const d = h.final();
    Sha256Digest out{};
    std::memcpy(out.data(), d.data(), 32);
    return out;
}

Sha256Digest sha256_of(std::byte const* data, std::size_t n)
{
    lt::hasher256 h;
    h.update(reinterpret_cast<char const*>(data), static_cast<int>(n));
    auto const d = h.final();
    Sha256Digest out{};
    std::memcpy(out.data(), d.data(), 32);
    return out;
}

Sha1Digest sha1_of(std::byte const* data, std::size_t n)
{
    lt::hasher h;
    h.update(reinterpret_cast<char const*>(data), static_cast<int>(n));
    auto const d = h.final();
    Sha1Digest out{};
    std::memcpy(out.data(), d.data(), 20);
    return out;
}

// Root of a subtree whose leaves are all zero hashes.
Sha256Digest zero_subtree_root(std::size_t leaves)
{
    Sha256Digest h{};
    for (std::size_t width = 1; width < leaves; width *= 2) h = hash_pair(h, h);
    return h;
}

Sha256Digest fold_layer(std::vector<Sha256Digest> layer, std::size_t width, Sha256Digest const& pad)
{
    layer.resize(width, pad);
    while (layer.size() > 1) {
        for (std::size_t i = 0; i < layer.size() / 2; ++i) layer[i] = hash_pair(layer[2 * i], layer[2 * i + 1]);
        layer.resize(layer.size() / 2);
    }
    return layer.front();
}

std::uint64_t ceil_div(std::uint64_t a, std::uint64_t b) { return (a + b - 1) / b; }

struct Unit {
    std::unique_ptr<std::byte[]> data;
    std::size_t size = 0;
    std::uint64_t v1_first_piece = 0;
    // v2: real bytes of one file at the start of `data` (padding may follow).
    int v2_file = -1;
    std::uint64_t v2_first_piece = 0;
    std::size_t v2_bytes = 0;
    std::uint64_t v2_file_length = 0;
};

// Bounded hand-off between the reading thread and the hash workers. Buffers
// are recycled; at most `max_buffers` exist at once.
class WorkQueue {
public:
    WorkQueue(std::size_t buffer_size, std::size_t max_buffers) : buffer_size_(buffer_size), max_buffers_(max_buffers) {}

    std::unique_ptr<std::byte[]> acquire()
    {
        std::unique_lock lock(mutex_);
        space_.wait(lock, [&] { return failed_ || !free_.empty() || allocated_ < max_buffers_; });
        rethrow_locked();
        if (!free_.empty()) {
            auto b = std::move(free_.back());
            free_.pop_back();
            return b;
        }
        ++allocated_;
        lock.unlock();
        return std::make_unique<std::byte[]>(buffer_size_);
    }

    void release(std::unique_ptr<std::byte[]> buffer)
    {
        {
            std::lock_guard lock(mutex_);
            free_.push_back(std::move(buffer));
        }
        space_.notify_one();
    }

    void push(Unit unit)
    {
        {
            std::lock_guard lock(mutex_);
            rethrow_locked();
            items_.push_back(std::move(unit));
        }
        ready_.notify_one();
    }

    // Returns false once the queue is closed and drained, or after a failure.
    bool pop(Unit& out)
    {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return failed_ || closed_ || !items_.empty(); });
        if (failed_ || items_.empty()) return false;
        out = std::move(items_.front());
        items_.pop_front();
        return true;
    }

    void close()
    {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        ready_.notify_all();
    }

    void fail(std::exception_ptr e)
    {
        {
            std::lock_guard lock(mutex_);
            if (!error_) error_ = e;
            failed_ = true;
        }
        ready_.notify_all();
        space_.notify_all();
    }

    void rethrow()
    {
        std::lock_guard lock(mutex_);
        rethrow_locked();
    }

    std::size_t allocated_buffers()
    {
        std::lock_guard lock(mutex_);
        return allocated_;
    }

    // Waits until every queued unit has been hashed and its buffer returned,
    // except the one buffer the reader is filling.
    void wait_drained()
    {
        std::unique_lock lock(mutex_);
        space_.wait(lock, [&] { return failed_ || (items_.empty() && free_.size() + 1 >= allocated_); });
    }

private:
    void rethrow_locked()
    {
        if (error_) std::rethrow_exception(error_);
    }

    std::size_t const buffer_size_;
    std::size_t const max_buffers_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable space_;
    std::deque<Unit> items_;
    std::vector<std::unique_ptr<std::byte[]>> free_;
    std::size_t allocated_ = 0;
    bool closed_ = false;
    bool failed_ = false;
    std::exception_ptr error_;
};

class Pipeline {
public:
    Pipeline(HashJob const& job, PayloadSource& source, std::stop_token stop,
        std::function<void(HashProgress const&)> const& progress)
        : job_(job), source_(source), stop_(std::move(stop)), progress_(progress),
          piece_(static_cast<std::uint64_t>(job.piece_length))
    {
        auto const plan = plan_buffers(job.piece_length, job.buffer_budget, job.threads);
        threads_ = plan.workers;
        unit_target_ = plan.unit_bytes;
        out_.metrics.unit_bytes = plan.unit_bytes;
        out_.metrics.hash_workers = plan.workers;
        queue_ = std::make_unique<WorkQueue>(unit_target_, plan.max_buffers);

        std::uint64_t stream = 0;
        out_.v2_piece_roots.resize(job.files.size());
        out_.outcomes.resize(job.files.size());
        for (std::size_t i = 0; i < job.files.size(); ++i) {
            auto const& f = job.files[i];
            if (f.pad) {
                if (job.v1) {
                    stream += f.length;
                    out_.progress.padding_bytes_total += f.length;
                }
                continue;
            }
            stream += f.length;
            out_.progress.payload_bytes_total += f.length;
            ++out_.progress.files_total;
            if (job.v2 && f.length > 0) out_.v2_piece_roots[i].resize(static_cast<std::size_t>(ceil_div(f.length, piece_)));
        }
        if (job.v1) out_.v1_pieces.resize(static_cast<std::size_t>(ceil_div(stream, piece_)));
        out_.metrics.hash_slot_bytes = out_.v1_pieces.size() * sizeof(Sha1Digest);
        for (auto const& roots : out_.v2_piece_roots) out_.metrics.hash_slot_bytes += roots.size() * sizeof(Sha256Digest);
    }

    HashOutput run()
    {
        std::vector<std::jthread> workers;
        auto const cancellation = std::make_exception_ptr(CoreError(ErrorCode::Cancelled, "Hashing was cancelled"));
        std::stop_callback on_stop(stop_, [this, cancellation] { queue_->fail(cancellation); });
        workers.reserve(static_cast<std::size_t>(threads_));
        try {
            // A thread-start failure must also wake and join already started workers.
            for (int i = 0; i < threads_; ++i) workers.emplace_back([this] { work(); });
            current_.data = queue_->acquire();
            report();
            for (std::size_t i = 0; i < job_.files.size(); ++i) {
                check_stop();
                auto const& f = job_.files[i];
                if (f.pad) {
                    if (job_.v1) {
                        append_zeros(i, f.length, false);
                        out_.progress.padding_bytes_processed += f.length;
                    }
                    continue;
                }
                if (job_.v2) flush(); // v2 work units never mix files
                file_done_ = 0;
                out_.progress.current_file = f.torrent_path;
                read_file(i);
                ++out_.progress.files_completed;
                report();
            }
            flush();
            report();
        } catch (...) {
            queue_->fail(std::current_exception());
            workers.clear(); // joins
            queue_->rethrow();
        }
        queue_->close();
        workers.clear(); // joins after the queue drains
        queue_->rethrow();
        out_.metrics.allocated_buffers = queue_->allocated_buffers();
        out_.metrics.peak_payload_buffer_bytes = out_.metrics.allocated_buffers * unit_target_;
        return std::move(out_);
    }

private:
    void check_stop()
    {
        if (stop_.stop_requested())
            throw CoreError(ErrorCode::Cancelled, "Hashing was cancelled");
        queue_->rethrow();
        if (job_.pause != nullptr) {
            job_.pause->checkpoint(stop_, [this] { queue_->wait_drained(); });
            if (stop_.stop_requested())
                throw CoreError(ErrorCode::Cancelled, "Hashing was cancelled");
            queue_->rethrow();
        }
    }

    void report()
    {
        if (progress_) progress_(out_.progress);
    }

    // Prepares `current_` to receive bytes of layout file `index`.
    void begin_bytes(std::size_t index, bool real)
    {
        if (current_.size == unit_target_) flush();
        if (current_.size == 0) {
            if (job_.v1) {
                if (stream_pos_ % piece_ != 0)
                    throw CoreError(ErrorCode::EngineError, "Internal error: work unit not aligned to a piece");
                current_.v1_first_piece = stream_pos_ / piece_;
            }
            if (real && job_.v2) {
                current_.v2_file = static_cast<int>(index);
                current_.v2_first_piece = file_done_ / piece_;
                current_.v2_file_length = job_.files[index].length;
            }
        }
    }

    void committed(std::size_t n, bool real)
    {
        current_.size += n;
        if (job_.v1) stream_pos_ += n;
        if (real) {
            file_done_ += n;
            if (job_.v2) current_.v2_bytes += n;
        }
    }

    void append_zeros(std::size_t index, std::uint64_t n, bool real)
    {
        while (n > 0) {
            check_stop();
            begin_bytes(index, real);
            auto const take = static_cast<std::size_t>(std::min<std::uint64_t>(n, unit_target_ - current_.size));
            std::memset(current_.data.get() + current_.size, 0, take);
            committed(take, real);
            n -= take;
        }
    }

    void flush()
    {
        if (current_.size == 0) return;
        // Hand over first: with a budget of one buffer, acquiring before
        // pushing would wait for a buffer that only this unit can free.
        queue_->push(std::move(current_));
        current_ = Unit{};
        current_.data = queue_->acquire();
    }

    // Records a problem under the Record policy; rethrows under Strict.
    void problem(std::size_t index, FileStatus status, std::string message, CoreError const* cause)
    {
        // Verification must never convert a cancellation into an unreadable
        // row and synthesize the rest of a large file.
        if (cause != nullptr && cause->code() == ErrorCode::Cancelled) throw *cause;
        if (job_.policy == ReadPolicy::Strict) {
            if (cause != nullptr) {
                CoreError e = *cause;
                e.with_phase(Phase::Hashing);
                if (auto const* src = job_.files[index].source) e.with_source(src->source_id);
                throw e;
            }
            CoreError e(ErrorCode::SourceChanged, std::move(message));
            e.with_phase(Phase::Hashing);
            if (auto const* src = job_.files[index].source) e.with_source(src->source_id);
            throw e;
        }
        auto& o = out_.outcomes[index];
        if (o.status == FileStatus::Ok) o = FileOutcome{status, std::move(message)};
    }

    void read_file(std::size_t index)
    {
        LayoutFile const& f = job_.files[index];
        std::uint64_t remaining = f.length;
        if (f.source == nullptr) {
            if (job_.policy == ReadPolicy::Strict)
                throw CoreError(ErrorCode::EngineError, "Engine layout contains an unmapped file: " + f.torrent_path);
            problem(index, FileStatus::Missing, "no source is mapped to this file", nullptr);
            append_zeros(index, remaining, true);
            return;
        }

        std::unique_ptr<PayloadReader> reader;
        try {
            reader = source_.open(*f.source, stop_);
        } catch (CoreError const& e) {
            FileStatus const s = e.code() == ErrorCode::SourceMissing ? FileStatus::Missing : FileStatus::Unreadable;
            problem(index, s, e.what(), &e);
            append_zeros(index, remaining, true);
            return;
        }

        std::optional<native::FileObservation> const at_open = reader->observe();
        if (at_open) {
            if (job_.policy == ReadPolicy::Strict && f.source->observed.identity.valid) {
                auto const changes = native::describe_changes(f.source->observed, *at_open);
                if (!changes.empty())
                    problem(index, FileStatus::Changed,
                        "Source changed after the manifest was frozen (" + changes.front() + "): " + f.torrent_path, nullptr);
            }
            if (job_.policy == ReadPolicy::Record && at_open->size != f.length)
                problem(index, FileStatus::SizeMismatch,
                    "size is " + std::to_string(at_open->size) + " bytes, expected " + std::to_string(f.length), nullptr);
        }

        while (remaining > 0) {
            check_stop();
            begin_bytes(index, true);
            auto const want = static_cast<std::size_t>(
                std::min<std::uint64_t>({remaining, job_.read_size, unit_target_ - current_.size}));
            out_.metrics.max_read_request_bytes = std::max(out_.metrics.max_read_request_bytes, want);
            std::size_t n = 0;
            try {
                n = reader->read(std::span<std::byte>(current_.data.get() + current_.size, want), stop_);
            } catch (CoreError const& e) {
                problem(index, FileStatus::Unreadable, e.what(), &e);
                append_zeros(index, remaining, true);
                return;
            }
            if (n == 0) {
                problem(index, FileStatus::SizeMismatch, "Source file shrank after the manifest was frozen: " + f.torrent_path,
                    nullptr);
                append_zeros(index, remaining, true);
                return;
            }
            n = std::min(n, want);
            committed(n, true);
            remaining -= n;
            out_.progress.payload_bytes_read += n;
            report();
        }

        check_stop();
        // The file must end exactly at the expected length.
        std::array<std::byte, 1> probe{};
        std::size_t extra = 0;
        try {
            extra = reader->read(probe, stop_);
        } catch (CoreError const& e) {
            problem(index, FileStatus::Unreadable, e.what(), &e);
            return;
        }
        if (extra != 0) {
            problem(index, FileStatus::SizeMismatch, "Source file grew after the manifest was frozen: " + f.torrent_path, nullptr);
            return;
        }

        if (job_.policy == ReadPolicy::Strict && at_open) {
            if (auto const at_end = reader->observe()) {
                auto const changes = native::describe_changes(*at_open, *at_end);
                if (!changes.empty())
                    problem(index, FileStatus::Changed,
                        "Source changed while it was being read (" + changes.front() + "): " + f.torrent_path, nullptr);
            }
        }
    }

    void work()
    {
        try {
            Unit unit;
            while (queue_->pop(unit)) {
                process(unit);
                queue_->release(std::move(unit.data));
            }
        } catch (...) {
            queue_->fail(std::current_exception());
        }
    }

    void process(Unit const& u)
    {
        auto stop_check = [this] {
            if (stop_.stop_requested()) throw CoreError(ErrorCode::Cancelled, "Hashing was cancelled");
        };
        stop_check();
        std::byte const* const data = u.data.get();
        if (job_.v1) {
            std::uint64_t piece = u.v1_first_piece;
            for (std::size_t off = 0; off < u.size; off += static_cast<std::size_t>(piece_), ++piece) {
                stop_check();
                std::size_t const n = std::min<std::size_t>(static_cast<std::size_t>(piece_), u.size - off);
                out_.v1_pieces[static_cast<std::size_t>(piece)] = sha1_of(data + off, n);
            }
        }
        if (job_.v2 && u.v2_file >= 0 && u.v2_bytes > 0) {
            auto& roots = out_.v2_piece_roots[static_cast<std::size_t>(u.v2_file)];
            std::size_t const blocks_per_piece = static_cast<std::size_t>(piece_) / block_size;
            std::size_t const width = u.v2_file_length < piece_
                ? next_power_of_two(static_cast<std::size_t>(ceil_div(u.v2_file_length, block_size)))
                : blocks_per_piece;
            std::uint64_t piece = u.v2_first_piece;
            std::vector<Sha256Digest> leaves;
            for (std::size_t off = 0; off < u.v2_bytes; off += static_cast<std::size_t>(piece_), ++piece) {
                std::size_t const end = std::min<std::size_t>(u.v2_bytes, off + static_cast<std::size_t>(piece_));
                stop_check();
                leaves.clear();
                for (std::size_t b = off; b < end; b += block_size)
                    leaves.push_back(sha256_of(data + b, std::min<std::size_t>(block_size, end - b)));
                roots[static_cast<std::size_t>(piece)] = merkle_root(leaves, width);
            }
        }
    }

    HashJob const& job_;
    PayloadSource& source_;
    std::stop_token stop_;
    std::function<void(HashProgress const&)> const& progress_;
    std::uint64_t const piece_;
    int threads_ = 1;
    std::size_t unit_target_ = 0;
    std::unique_ptr<WorkQueue> queue_;
    Unit current_;
    std::uint64_t stream_pos_ = 0;
    std::uint64_t file_done_ = 0;
    HashOutput out_;
};

} // namespace

std::string_view to_string(FileStatus s) noexcept
{
    switch (s) {
    case FileStatus::Ok: return "ok";
    case FileStatus::Missing: return "missing";
    case FileStatus::Unreadable: return "unreadable";
    case FileStatus::SizeMismatch: return "size-mismatch";
    case FileStatus::Changed: return "changed";
    }
    return "unknown";
}

std::size_t next_power_of_two(std::size_t n)
{
    std::size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

Sha256Digest merkle_root(std::vector<Sha256Digest> leaves, std::size_t width)
{
    return fold_layer(std::move(leaves), width, Sha256Digest{});
}

Sha256Digest file_root(std::vector<Sha256Digest> const& piece_roots, int piece_length)
{
    if (piece_roots.size() == 1) return piece_roots.front();
    Sha256Digest const pad = zero_subtree_root(static_cast<std::size_t>(piece_length / block_size));
    return fold_layer(piece_roots, next_power_of_two(piece_roots.size()), pad);
}

BufferPlan plan_buffers(int piece_length, std::size_t budget, int threads)
{
    if (piece_length < block_size || (piece_length & (piece_length - 1)) != 0)
        throw CoreError(ErrorCode::InvalidArgument, "Piece length must be a power of two of at least 16 KiB");
    if (threads < 0 || threads > 64)
        throw CoreError(ErrorCode::InvalidArgument, "Hash thread count must be between 0 and 64");
    auto const piece = static_cast<std::size_t>(piece_length);
    if (budget < piece)
        throw CoreError(ErrorCode::ResourceLimit, "Payload buffer budget must hold at least one piece; increase the budget or reduce piece length");
    int const hw = static_cast<int>(std::min(64u, std::max(1u, std::thread::hardware_concurrency())));
    int const requested = threads > 0 ? threads : std::min(4, hw);
    int const workers = static_cast<int>(std::min(static_cast<std::size_t>(requested), budget / piece));
    auto const per_buffer = budget / (static_cast<std::size_t>(workers) + 1);
    auto const unit = std::max(std::size_t(1), std::min(per_buffer, max_unit_bytes) / piece) * piece;
    return {unit, budget / unit, workers};
}

HashOutput hash_payload(HashJob const& job, PayloadSource& source, std::stop_token stop,
    std::function<void(HashProgress const&)> const& progress)
{
    if (job.piece_length < block_size || (job.piece_length & (job.piece_length - 1)) != 0)
        throw CoreError(ErrorCode::InvalidArgument, "Piece length must be a power of two of at least 16 KiB");
    if (job.read_size < block_size) throw CoreError(ErrorCode::InvalidArgument, "Read buffer must be at least 16 KiB");
    return Pipeline(job, source, std::move(stop), progress).run();
}

} // namespace tc::core::detail
