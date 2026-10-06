#include "tc/bridge/async_dispatcher.hpp"

namespace tc::bridge {

AsyncDispatcher::AsyncDispatcher(Dispatcher const& dispatcher, std::function<void()> wake)
    : dispatcher_(dispatcher), wake_(std::move(wake)), worker_([this](std::stop_token stop) { run(stop); }) {}

AsyncDispatcher::~AsyncDispatcher() { stop(); }

bool AsyncDispatcher::submit(std::string message, std::string source,
    std::vector<std::filesystem::path> attached, std::uint64_t generation)
{
    if (message.size() > max_message_bytes || source.size() > 4096 || attached.size() > 10000) return false;
    std::size_t bytes = message.size() + source.size();
    for (auto const& path : attached) {
        auto const size = path.native().size();
        if (size > max_queued_bytes / sizeof(std::filesystem::path::value_type)) return false;
        auto const path_bytes = size * sizeof(std::filesystem::path::value_type);
        if (path_bytes > max_queued_bytes - bytes) return false;
        bytes += path_bytes;
    }
    std::lock_guard lock(mutex_);
    if (closed_ || generation != generation_ || pending_.size() + replies_.size() + (active_ ? 1 : 0) >= max_outstanding
        || bytes > max_queued_bytes - queued_bytes_) return false;
    pending_.push_back({std::move(message), std::move(source), std::move(attached), generation, bytes});
    queued_bytes_ += bytes;
    cv_.notify_one();
    return true;
}

std::vector<AsyncDispatcher::Reply> AsyncDispatcher::take_replies()
{
    std::lock_guard lock(mutex_);
    std::vector<Reply> out;
    out.swap(replies_);
    return out;
}

void AsyncDispatcher::reset_generation(std::uint64_t generation)
{
    std::lock_guard lock(mutex_);
    generation_ = generation;
    pending_.clear(); replies_.clear(); queued_bytes_ = 0;
}

void AsyncDispatcher::stop()
{
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        pending_.clear(); replies_.clear(); queued_bytes_ = 0;
    }
    worker_.request_stop();
    cv_.notify_all();
}

void AsyncDispatcher::run(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        Command command;
        {
            std::unique_lock lock(mutex_);
            if (!cv_.wait(lock, stop, [this] { return closed_ || !pending_.empty(); }) || closed_) return;
            command = std::move(pending_.front()); pending_.pop_front();
            queued_bytes_ -= command.bytes;
            active_ = true;
        }
        auto response = dispatcher_.handle(command.message, command.source, command.attached);
        bool notify = false;
        {
            std::lock_guard lock(mutex_);
            active_ = false;
            if (!closed_ && command.generation == generation_) {
                replies_.push_back({std::move(response), command.generation});
                notify = true;
            }
        }
        if (notify && wake_) wake_();
    }
}

} // namespace tc::bridge
