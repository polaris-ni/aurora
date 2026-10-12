/// 测试类型: unit
/// 目标单元: include/aurora/app/event_source_registry.h
/// 测试说明: aurora::app::EventSourceRegistry 的平台无关就绪契约（CHG-008 底座切片）：
///           有效句柄登记成功、-1 与「本腿不支持」回 net-platform-unavailable（不假降级）；
///           就绪回调「抵达次数 == 就绪次数」；注销 = 停止产生新回调但已入队的一次尾批仍抵达
///           （§18-1），`cancel_pending()` 才作废尾批；移动后源句柄析构不双重注销；同句柄
///           注销后重登记得新令牌。Poller/Dispatcher 经接缝脚本化，无需真 socket，
///           从而在无宿主事件循环下逐值断言；注册口是进程级单例，每例用作用域 guard 复位接缝。

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "aurora/app/event_source_registry.h"
#include "aurora/core/result.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_app_event_source_registry {

using aurora::app::EventSourceRegistration;
using aurora::app::EventSourceRegistry;
using aurora::app::ReadySource;
using aurora::app::SourceReady;
using aurora::app::WaitInterest;
using aurora::app::WaitSource;

namespace {

/// @brief 每例复位进程级接缝（Poller/Dispatcher/支持位），隔离用例间的全局状态。
struct SeamsGuard {
    SeamsGuard() { reset(); }
    SeamsGuard(const SeamsGuard &) = delete;
    auto operator=(const SeamsGuard &) -> SeamsGuard & = delete;
    SeamsGuard(SeamsGuard &&) = delete;
    auto operator=(SeamsGuard &&) -> SeamsGuard & = delete;
    ~SeamsGuard() { reset(); }
    static auto reset() -> void {
        auto &r = EventSourceRegistry::instance();
        r.set_poller({});
        r.set_dispatcher({});
        r.set_event_source_supported(true);
    }
};

/// @brief 脚本化 Poller：每次调用回报给定的一批就绪令牌（忽略真实句柄）。
auto scripted_poller(std::vector<ReadySource> batch) -> EventSourceRegistry::Poller {
    auto shared = std::make_shared<std::vector<ReadySource>>(std::move(batch));
    return [shared](const std::vector<WaitSource> &,
                    std::chrono::milliseconds) -> aurora::Result<std::vector<ReadySource>> { return *shared; };
}

}  // namespace

// 有效句柄登记成功并计入存活数；-1 句柄被拒。
AURORA_TEST_CASE(register_accepts_valid_handle_rejects_invalid) {
    SeamsGuard guard;
    auto &r = EventSourceRegistry::instance();
    AURORA_TEST_REQUIRE_EQ(r.live_count(), 0U);

    auto bad = r.register_source(-1, WaitInterest{.read = true}, [](SourceReady) {});
    AURORA_TEST_REQUIRE_FALSE(bool(bad));
    AURORA_TEST_CHECK(bad.error().code_enum == aurora::ErrorCode::NetPlatformUnavailable);

    auto reg = r.register_source(7, WaitInterest{.read = true}, [](SourceReady) {});
    AURORA_TEST_REQUIRE(bool(reg));
    AURORA_TEST_CHECK_TRUE(reg.value().is_registered());
    AURORA_TEST_CHECK_TRUE(reg.value().token() != 0U);
    AURORA_TEST_CHECK_EQ(r.live_count(), 1U);
}

// 无 fd 语义的腿（wasm 形态）登记一律回不可用，不默默收下永不被触发的条目。
AURORA_TEST_CASE(unsupported_leg_rejects_registration) {
    SeamsGuard guard;
    auto &r = EventSourceRegistry::instance();
    r.set_event_source_supported(false);

    auto reg = r.register_source(7, WaitInterest{.read = true}, [](SourceReady) {});
    AURORA_TEST_REQUIRE_FALSE(bool(reg));
    AURORA_TEST_CHECK(reg.error().code_enum == aurora::ErrorCode::NetPlatformUnavailable);
    AURORA_TEST_CHECK_EQ(r.live_count(), 0U);
}

// 就绪回调「抵达次数 == 就绪次数」：两次 wait_and_dispatch 各回报一次可读，计数应为 2。
AURORA_TEST_CASE(ready_callback_arrives_once_per_ready) {
    SeamsGuard guard;
    auto &r = EventSourceRegistry::instance();
    int hits = 0;
    auto reg = r.register_source(11, WaitInterest{.read = true}, [&hits](SourceReady ready) {
        if (ready.readable) {
            ++hits;
        }
    });
    AURORA_TEST_REQUIRE(bool(reg));
    const auto token = reg.value().token();
    r.set_poller(scripted_poller({ReadySource{.token = token, .ready = SourceReady{.readable = true}}}));

    AURORA_TEST_CHECK(bool(r.wait_and_dispatch(std::chrono::milliseconds{0})));
    AURORA_TEST_CHECK(bool(r.wait_and_dispatch(std::chrono::milliseconds{0})));
    AURORA_TEST_CHECK_EQ(hits, 2);
}

// 缺 Poller 时 wait_and_dispatch 报不可用（不忙等、不假降级），但登记本身仍可用（原生宿主自带等待集）。
AURORA_TEST_CASE(native_host_dispatch_without_poller) {
    SeamsGuard guard;
    auto &r = EventSourceRegistry::instance();
    int hits = 0;
    auto reg = r.register_source(13, WaitInterest{.read = true}, [&hits](SourceReady) { ++hits; });
    AURORA_TEST_REQUIRE(bool(reg));
    const auto token = reg.value().token();

    // 无 Poller：便捷循环拒绝……
    auto loop = r.wait_and_dispatch(std::chrono::milliseconds{1});
    AURORA_TEST_REQUIRE_FALSE(bool(loop));
    AURORA_TEST_CHECK(loop.error().code_enum == aurora::ErrorCode::NetPlatformUnavailable);

    // ……但宿主用 sources()/dispatch() 直连仍能收就绪（X11/Wayland/Win32 路径）。
    auto sources = r.sources();
    AURORA_TEST_REQUIRE_EQ(sources.size(), 1U);
    AURORA_TEST_CHECK_EQ(sources[0].token, token);
    r.dispatch({ReadySource{.token = token, .ready = SourceReady{.readable = true}}});
    AURORA_TEST_CHECK_EQ(hits, 1);
}

// 注销 = 停止产生新回调，但已入队的一次尾批仍抵达（用排程 Dispatcher 制造「入队后、执行前注销」）。
AURORA_TEST_CASE(unregister_lets_one_tail_batch_arrive) {
    SeamsGuard guard;
    auto &r = EventSourceRegistry::instance();
    std::vector<std::function<void()>> queue;
    r.set_dispatcher([&queue](std::function<void()> fn) { queue.push_back(std::move(fn)); });

    int hits = 0;
    std::uint64_t token = 0;
    {
        auto reg = r.register_source(17, WaitInterest{.read = true}, [&hits](SourceReady) { ++hits; });
        AURORA_TEST_REQUIRE(bool(reg));
        token = reg.value().token();
        r.dispatch({ReadySource{.token = token, .ready = SourceReady{.readable = true}}});  // 仅入队，未执行
        AURORA_TEST_CHECK_EQ(queue.size(), 1U);
        AURORA_TEST_CHECK_EQ(hits, 0);  // 尚未执行
    }  // reg 析构 → 注销（从快照移除，但不作废已入队尾批）

    for (auto &fn : queue) {
        fn();  // 尾批仍抵达一次
    }
    AURORA_TEST_CHECK_EQ(hits, 1);

    // 注销后新的 dispatch 不再为该令牌产生回调。
    queue.clear();
    r.dispatch({ReadySource{.token = token, .ready = SourceReady{.readable = true}}});
    AURORA_TEST_CHECK_EQ(queue.size(), 0U);
}

// cancel_pending 作废已入队的尾批（与析构注销互不隐含）：入队后 cancel，再执行队列为零抵达。
AURORA_TEST_CASE(cancel_pending_voids_tail_batch) {
    SeamsGuard guard;
    auto &r = EventSourceRegistry::instance();
    std::vector<std::function<void()>> queue;
    r.set_dispatcher([&queue](std::function<void()> fn) { queue.push_back(std::move(fn)); });

    int hits = 0;
    auto reg = r.register_source(19, WaitInterest{.read = true}, [&hits](SourceReady) { ++hits; });
    AURORA_TEST_REQUIRE(bool(reg));
    r.dispatch({ReadySource{.token = reg.value().token(), .ready = SourceReady{.readable = true}}});  // 入队
    reg.value().cancel_pending();  // 作废尾批
    for (auto &fn : queue) {
        fn();
    }
    AURORA_TEST_CHECK_EQ(hits, 0);
}

// 移动后源句柄转为空态、其析构不触发注销；存活句柄析构只注销一次，全程无二次注销。
AURORA_TEST_CASE(move_does_not_double_unregister) {
    SeamsGuard guard;
    auto &r = EventSourceRegistry::instance();
    {
        auto reg = r.register_source(23, WaitInterest{.read = true}, [](SourceReady) {});
        AURORA_TEST_REQUIRE(bool(reg));
        const auto token = reg.value().token();
        AURORA_TEST_CHECK_EQ(r.live_count(), 1U);

        EventSourceRegistration moved = std::move(reg.value());  // 源转空态
        AURORA_TEST_CHECK_TRUE(moved.is_registered());
        AURORA_TEST_CHECK_FALSE(reg.value().is_registered());
        AURORA_TEST_CHECK_EQ(moved.token(), token);
        AURORA_TEST_CHECK_EQ(r.live_count(), 1U);  // 同一条目，未因移动而增减
    }  // moved 与已空的 reg 各自析构：仅注销一次
    AURORA_TEST_CHECK_EQ(r.live_count(), 0U);
}

// 同句柄注销后重登记取得不同的新令牌；对旧令牌的迟到就绪不触发新条目回调。
AURORA_TEST_CASE(re_register_same_handle_gets_new_token) {
    SeamsGuard guard;
    auto &r = EventSourceRegistry::instance();
    std::uint64_t token1 = 0;
    {
        auto first = r.register_source(29, WaitInterest{.read = true}, [](SourceReady) {});
        AURORA_TEST_REQUIRE(bool(first));
        token1 = first.value().token();
    }  // first 析构 → 注销

    auto second = r.register_source(29, WaitInterest{.read = true}, [](SourceReady) {});
    AURORA_TEST_REQUIRE(bool(second));
    const auto token2 = second.value().token();
    AURORA_TEST_CHECK_NE(token1, token2);

    int hits = 0;
    auto third = r.register_source(31, WaitInterest{.read = true}, [&hits](SourceReady) { ++hits; });
    AURORA_TEST_REQUIRE(bool(third));
    r.dispatch({ReadySource{.token = token1, .ready = SourceReady{.readable = true}}});  // 表里已无旧令牌
    AURORA_TEST_CHECK_EQ(hits, 0);
}

}  // namespace aurora::test_cases::utest_app_event_source_registry
