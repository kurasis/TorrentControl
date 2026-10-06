#pragma once

// Developer-only coordination around the ordinary payload adapter. A held
// gate is never evidence of pending OS I/O: release it before checking flags.
#include "tc/core/payload_source.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace tc::proof {
struct WorkflowProbe {
    std::mutex mutex;
    std::condition_variable wake;
    std::string gate;
    bool held = false, released = false;
    std::atomic<bool> native_open{false}, native_read{false};
    std::atomic<std::size_t> readers{0}, reads{0};
    std::atomic<std::uint64_t> bytes{0};
    void arm(std::string const& site) {
        if (site != "open" && site != "read") throw std::runtime_error("Invalid probe site");
        std::lock_guard lock(mutex);
        gate = site;
    }
    void release() {
        { std::lock_guard lock(mutex); released = true; }
        wake.notify_all();
    }
    void checkpoint(std::string const& site) {
        std::unique_lock lock(mutex);
        if (gate != site) return;
        gate.clear(); held = true; released = false;
        wake.wait(lock, [&] { return released; });
        held = false;
    }
    nlohmann::json snapshot() {
        std::lock_guard lock(mutex);
        return {{"held", held}, {"nativeOpen", native_open.load()}, {"nativeRead", native_read.load()},
            {"readers", readers.load()}, {"readCalls", reads.load()}, {"bytesRead", bytes.load()}};
    }
};
struct ProbeActive {
    std::atomic<bool>& flag;
    explicit ProbeActive(std::atomic<bool>& f) : flag(f) { flag = true; }
    ~ProbeActive() { flag = false; }
};
class ProbeReader final : public core::PayloadReader {
public:
    ProbeReader(std::unique_ptr<core::PayloadReader> inner, WorkflowProbe& p) : inner_(std::move(inner)), p_(p) { ++p_.readers; }
    ~ProbeReader() override { inner_.reset(); --p_.readers; }
    std::size_t read(std::span<std::byte> buffer) override { return read(buffer, {}); }
    std::size_t read(std::span<std::byte> buffer, std::stop_token stop) override {
        p_.checkpoint("read");
        ProbeActive active(p_.native_read); ++p_.reads;
        auto const n = inner_->read(buffer, stop); p_.bytes += n; return n;
    }
    std::optional<core::native::FileObservation> observe() override { return inner_->observe(); }
private:
    std::unique_ptr<core::PayloadReader> inner_;
    WorkflowProbe& p_;
};
class ProbeSource final : public core::PayloadSource {
public:
    explicit ProbeSource(WorkflowProbe& p) : p_(p), inner_(core::make_file_payload_source()) {}
    std::unique_ptr<core::PayloadReader> open(core::ManifestEntry const& e) override { return open(e, {}); }
    std::unique_ptr<core::PayloadReader> open(core::ManifestEntry const& e, std::stop_token stop) override {
        p_.checkpoint("open"); ProbeActive active(p_.native_open);
        return std::make_unique<ProbeReader>(inner_->open(e, stop), p_);
    }
private:
    WorkflowProbe& p_;
    std::unique_ptr<core::PayloadSource> inner_;
};
} // namespace tc::proof
