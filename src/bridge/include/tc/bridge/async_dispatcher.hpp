#pragma once

#include "tc/bridge/protocol.hpp"
#include <condition_variable>
#include <deque>
#include <thread>

namespace tc::bridge {

// One ordered command worker. Navigation discards commands which have not
// started; an in-flight mutation finishes, but its reply belongs to its page.
class AsyncDispatcher {
public:
    struct Reply { std::string message; std::uint64_t generation; };
    static constexpr std::size_t max_outstanding = 64;
    static constexpr std::size_t max_queued_bytes = 8 * max_message_bytes;
    AsyncDispatcher(Dispatcher const& dispatcher, std::function<void()> wake);
    ~AsyncDispatcher();
    bool submit(std::string message, std::string source,
        std::vector<std::filesystem::path> attached, std::uint64_t generation);
    std::vector<Reply> take_replies();
    void reset_generation(std::uint64_t generation);
    // Does not join: the UI must first cancel any pending UI-only callbacks.
    void stop();
private:
    struct Command {
        std::string message, source;
        std::vector<std::filesystem::path> attached;
        std::uint64_t generation;
        std::size_t bytes;
    };
    void run(std::stop_token stop);
    Dispatcher const& dispatcher_;
    std::function<void()> wake_;
    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::deque<Command> pending_;
    std::vector<Reply> replies_;
    std::size_t queued_bytes_ = 0;
    std::uint64_t generation_ = 0;
    bool active_ = false, closed_ = false;
    std::jthread worker_;
};

} // namespace tc::bridge
