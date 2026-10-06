#pragma once

#include "tc/bridge/protocol.hpp"
#include <deque>
#include <future>
#include <thread>
#include <type_traits>

namespace tc::bridge {

// The owner drains callbacks on its UI thread, outside browser callbacks.
// Closing destroys pending packaged tasks, waking their waiting worker.
class UiTasks {
public:
    explicit UiTasks(std::function<bool()> post) : post_(std::move(post)), owner_(std::this_thread::get_id()) {}
    bool is_owner_thread() const { return std::this_thread::get_id() == owner_; }
    template<class F> auto invoke(F&& callback) -> std::invoke_result_t<F>
    {
        if (is_owner_thread()) return std::forward<F>(callback)();
        using Result = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<F>(callback));
        auto future = task->get_future();
        {
            std::lock_guard lock(mutex_);
            if (closed_) throw BridgeError("HOST_CLOSED", "The application window has closed");
            pending_.push_back([task] { (*task)(); });
        }
        // Only the queue should own the task while the worker waits, so
        // clearing the queue completes the future with broken_promise.
        task.reset();
        if (!post_()) close();
        return future.get();
    }
    void drain()
    {
        if (!is_owner_thread()) throw std::logic_error("UI callbacks must run on their owner thread");
        while (true) {
            std::function<void()> task;
            {
                std::lock_guard lock(mutex_);
                if (pending_.empty()) return;
                task = std::move(pending_.front()); pending_.pop_front();
            }
            task();
        }
    }
    void close()
    {
        std::lock_guard lock(mutex_);
        closed_ = true; pending_.clear();
    }
private:
    std::function<bool()> post_;
    std::thread::id owner_;
    std::mutex mutex_;
    std::deque<std::function<void()>> pending_;
    bool closed_ = false;
};

} // namespace tc::bridge
