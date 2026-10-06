#include "tc/bridge/async_dispatcher.hpp"
#include "tc/bridge/ui_tasks.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <future>

using namespace std::chrono_literals;
using nlohmann::json;

namespace {
std::string command(int id)
{
    return json{{"protocolVersion", 1}, {"requestId", "req-" + std::to_string(id)},
        {"operation", "work"}, {"payload", {{"id", id}}}}.dump();
}
constexpr char const* source = "https://torrentcontrol.example/index.html";

std::vector<tc::bridge::AsyncDispatcher::Reply> collect(tc::bridge::AsyncDispatcher& worker, std::size_t count)
{
    std::vector<tc::bridge::AsyncDispatcher::Reply> out;
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (out.size() < count && std::chrono::steady_clock::now() < deadline) {
        auto replies = worker.take_replies();
        for (auto& reply : replies) out.push_back(std::move(reply));
        std::this_thread::sleep_for(1ms);
    }
    return out;
}
}

TEST_CASE("async bridge runs ordered commands off the caller thread", "[bridge][async]")
{
    tc::bridge::Dispatcher dispatcher;
    auto const caller = std::this_thread::get_id();
    int next = 1;
    dispatcher.register_operation("work", [&](json const& payload) {
        bool ordered = payload.at("id") == next++;
        return json{{"ordered", ordered}, {"background", std::this_thread::get_id() != caller}};
    });
    tc::bridge::AsyncDispatcher worker(dispatcher, [] {});
    for (int i = 1; i <= 3; ++i) REQUIRE(worker.submit(command(i), source, {}, 0));
    auto replies = collect(worker, 3);
    REQUIRE(replies.size() == 3);
    for (std::size_t i = 0; i < replies.size(); ++i) {
        auto r = json::parse(replies[i].message);
        CHECK(r["requestId"] == "req-" + std::to_string(i + 1));
        CHECK(r["result"]["ordered"] == true);
        CHECK(r["result"]["background"] == true);
    }
}

TEST_CASE("navigation drops queued mutations and replies from the old page", "[bridge][async]")
{
    tc::bridge::Dispatcher dispatcher;
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    std::vector<int> applied;
    dispatcher.register_operation("work", [&](json const& payload) {
        int id = payload.at("id");
        if (id == 1) { entered.set_value(); gate.wait_for(5s); }
        applied.push_back(id);
        return payload;
    });
    tc::bridge::AsyncDispatcher worker(dispatcher, [] {});
    REQUIRE(worker.submit(command(1), source, {}, 0));
    REQUIRE(entered.get_future().wait_for(2s) == std::future_status::ready);
    REQUIRE(worker.submit(command(2), source, {}, 0));
    worker.reset_generation(1);
    CHECK_FALSE(worker.submit(command(3), source, {}, 0));
    REQUIRE(worker.submit(command(4), source, {}, 1));
    release.set_value();
    auto replies = collect(worker, 1);
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].generation == 1);
    CHECK(json::parse(replies[0].message)["requestId"] == "req-4");
    CHECK(applied == std::vector<int>{1, 4});
}

TEST_CASE("async bridge bounds queued commands and correlates a busy refusal", "[bridge][async]")
{
    tc::bridge::Dispatcher dispatcher;
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    dispatcher.register_operation("work", [&](json const& payload) {
        if (payload.at("id") == 0) { entered.set_value(); gate.wait_for(5s); }
        return payload;
    });
    tc::bridge::AsyncDispatcher worker(dispatcher, [] {});
    REQUIRE(worker.submit(command(0), source, {}, 0));
    REQUIRE(entered.get_future().wait_for(2s) == std::future_status::ready);
    for (std::size_t i = 1; i < worker.max_outstanding; ++i)
        REQUIRE(worker.submit(command(static_cast<int>(i)), source, {}, 0));
    CHECK_FALSE(worker.submit(command(100), source, {}, 0));
    CHECK_FALSE(worker.submit(std::string(tc::bridge::max_message_bytes + 1, 'x'), source, {}, 0));
    auto refusal = json::parse(tc::bridge::reject_request(command(100), "BRIDGE_BUSY", "Try again", true));
    CHECK(refusal["requestId"] == "req-100");
    CHECK(refusal["error"]["retryable"] == true);
    worker.stop();
    CHECK_FALSE(worker.submit(command(101), source, {}, 0));
    release.set_value();
}

TEST_CASE("UI callbacks run on the owner and close releases a waiting worker", "[bridge][async]")
{
    std::promise<void> posted;
    tc::bridge::UiTasks ui([&] { posted.set_value(); return true; });
    auto const owner = std::this_thread::get_id();
    auto result = std::async(std::launch::async, [&] { return ui.invoke([&] { return std::this_thread::get_id() == owner; }); });
    REQUIRE(posted.get_future().wait_for(2s) == std::future_status::ready);
    ui.drain();
    CHECK(result.get());

    std::promise<void> pending;
    tc::bridge::UiTasks closing([&] { pending.set_value(); return true; });
    auto abandoned = std::async(std::launch::async, [&] { return closing.invoke([] { return 1; }); });
    REQUIRE(pending.get_future().wait_for(2s) == std::future_status::ready);
    closing.close();
    REQUIRE(abandoned.wait_for(2s) == std::future_status::ready);
    CHECK_THROWS_AS(abandoned.get(), std::future_error);
    auto late = std::async(std::launch::async, [&] { return closing.invoke([] { return 2; }); });
    CHECK_THROWS_AS(late.get(), tc::bridge::BridgeError);
}
