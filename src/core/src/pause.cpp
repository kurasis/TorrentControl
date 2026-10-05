#include "tc/core/pause.hpp"

namespace tc::core {

void PauseControl::set_listener(Listener listener)
{
    std::lock_guard lock(mutex_);
    listener_ = std::move(listener);
}

void PauseControl::request_pause()
{
    std::lock_guard lock(mutex_);
    requested_ = true;
}

void PauseControl::resume()
{
    {
        std::lock_guard lock(mutex_);
        requested_ = false;
    }
    cv_.notify_all();
}

bool PauseControl::pause_requested() const
{
    std::lock_guard lock(mutex_);
    return requested_;
}

bool PauseControl::parked() const
{
    std::lock_guard lock(mutex_);
    return parked_;
}

void PauseControl::checkpoint(std::stop_token const& stop, std::function<void()> const& drain)
{
    {
        std::lock_guard lock(mutex_);
        if (!requested_) return;
    }
    if (drain) drain();

    Listener listener;
    {
        std::unique_lock lock(mutex_);
        if (!requested_ || stop.stop_requested()) return;
        parked_ = true;
        listener = listener_;
    }
    if (listener) listener(true);
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, stop, [&] { return !requested_; });
        parked_ = false;
        listener = listener_;
    }
    if (listener) listener(false);
}

} // namespace tc::core
