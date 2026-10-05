#pragma once

// Cooperative pause for hashing jobs (specification section 9.3).
//
// Pause stops scheduling new reads: the reader parks between two bounded
// reads, in-flight buffers finish hashing, and only then is the job reported
// as parked. Source handles stay open while parked, so the protection taken
// when a file was opened (Windows denies write/delete sharing) still holds
// and nothing needs to be revalidated on resume. Cancellation wakes a parked
// reader.

#include <condition_variable>
#include <functional>
#include <mutex>
#include <stop_token>

namespace tc::core {

class PauseControl {
public:
    // Called with true once the job is parked and drained, and with false when
    // it continues. Runs on the hashing thread; keep it short.
    using Listener = std::function<void(bool parked)>;

    void set_listener(Listener listener);

    void request_pause();
    void resume();
    bool pause_requested() const;
    bool parked() const;

    // Called by the reader between reads. Returns immediately unless a pause
    // is requested; otherwise runs `drain`, reports parked, and blocks until
    // resume() or until `stop` is requested.
    void checkpoint(std::stop_token const& stop, std::function<void()> const& drain = {});

private:
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    Listener listener_;
    bool requested_ = false;
    bool parked_ = false;
};

} // namespace tc::core
