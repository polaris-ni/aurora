/// 测试类型: unit
/// 目标单元: include/aurora/net/poll.h
/// 测试说明: aurora::net 事件驱动多句柄就绪基元（CHG-010 底座切片）的行为契约回归，并把
///           `net::wait_ready` 与 `app::EventSourceRegistry` 串成一条端到端事件源通路：
///           空集合与纯超时都返回 OK（不是错误）、无效句柄槽位全 false 且不毁掉整批等待、
///           新建回环流先可写、写入后可读、排空后回到「什么都没等到」；集成腿把基于
///           `wait_ready` 的 Poller 闭包装进进程级注册口，登记真实 socket 句柄后由对端写入触发
///           一次就绪回调（缺省 Dispatcher 为就地同步执行），句柄析构注销后 `sources()` 清空、
///           后续 `wait_and_dispatch` 空转且回调不再抵达（§18-1「停止产生新回调」在真实 fd 上落地）。
///           全程同线程即可（TCP 握手由内核 backlog 完成，无需 accept 线程）；断言刻意避开两条
///           跨平台不稳路径（WSAPoll 对 listener 待 accept 与对端 FD_CLOSE 的呈现），只用「连接就绪
///           即可写 / 写入后可读」这两条两平台同色的判据。端口一律 0 = 系统分配，只连 127.0.0.1。
/// ⚠️ 平台分支写在**用例体内**而非包裹 `AURORA_TEST_CASE` 声明（registry_integrity 静态扫源码，
///    被 #ifdef 摘掉的声明在另一种构建里必然单边失踪）；wasm 无 BSD socket / poll 语义，用例体退化
///    为 AURORA_TEST_SKIP。

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/app/event_source_registry.h"
#include "aurora/core/platform.h"
#include "aurora/core/result.h"
#include "aurora/net/blocking.h"
#include "aurora/net/poll.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_poll {
namespace {

/// @brief 等待类用例的统一轮询窗口（覆盖内核回环往返，又不至于拖慢失败路径）。
constexpr std::chrono::milliseconds AURORA_NET_POLL_TIMEOUT{80};

/// @brief 纯超时用例的短窗口（无待就绪项时确保尽快返回而非长挂）。
constexpr std::chrono::milliseconds AURORA_NET_EMPTY_TIMEOUT{20};

/// @brief 一对已建立的回环 TCP 连接（client 发起、经 backlog 由同线程 accept 取得 server）。
/// @note 两个 StreamSocket 均 move-only；作为返回值靠移动/RVO 落地。
struct LoopbackPair {
    aurora::net::blocking::StreamSocket client;
    aurora::net::blocking::StreamSocket server;
};

/// @brief 在临时端口起监听、连接、接受，得到一对连通的回环流套接字后关闭监听口。
/// @return 连通的 client/server 对（listener 已在函数返回时析构关闭）。
auto make_connected_pair() -> LoopbackPair {
    auto bound = aurora::net::blocking::Listener::listen_loopback(0, 4);
    AURORA_TEST_REQUIRE(bound.ok());
    auto listener = std::move(bound.value());
    AURORA_TEST_REQUIRE_NE(listener.port(), std::uint16_t{0});

    auto connected = aurora::net::blocking::StreamSocket::connect_loopback(listener.port());
    AURORA_TEST_REQUIRE(connected.ok());
    auto client = std::move(connected.value());

    auto accepted = listener.accept();
    AURORA_TEST_REQUIRE(accepted.ok());
    auto server = std::move(accepted.value());
    return LoopbackPair{.client = std::move(client), .server = std::move(server)};
}

/// @brief 写入全部字节并在断言失败时立即转红的测试内助记（write_all 的 Result 样板收一次）。
/// @param socket 目标流。
/// @param payload 待发送文本。
auto write_payload(aurora::net::blocking::StreamSocket &socket, std::string_view payload) -> void {
    auto sent = socket.write_all(std::span<const char>(payload.data(), payload.size()));
    AURORA_TEST_REQUIRE(sent.ok());
}

/// @brief 进程级注册口的接缝守卫：构造装 Poller、析构清空 Poller 与 Dispatcher。
/// @note 用 RAII 而非末尾手工清零——用例中途 REQUIRE 转红也会经析构复原，不把 Poller 泄漏给同 runner 的其它用例。
struct SeamGuard {
    explicit SeamGuard(aurora::app::EventSourceRegistry::Poller poller) {
        aurora::app::EventSourceRegistry::instance().set_poller(std::move(poller));
    }
    ~SeamGuard() {
        auto &registry = aurora::app::EventSourceRegistry::instance();
        registry.set_poller({});
        registry.set_dispatcher({});
    }
    SeamGuard(const SeamGuard &) = delete;
    auto operator=(const SeamGuard &) -> SeamGuard & = delete;
    SeamGuard(SeamGuard &&) = delete;
    auto operator=(SeamGuard &&) -> SeamGuard & = delete;
};

/// @brief 由 `net::wait_ready` 组装注册口 Poller：WaitSource→PollDescriptor 摊平、就绪项按令牌回填。
/// @return 就绪等待接缝实现。
auto poller_over_wait_ready() -> aurora::app::EventSourceRegistry::Poller {
    return [](const std::vector<aurora::app::WaitSource> &sources,
              std::chrono::milliseconds timeout) -> aurora::Result<std::vector<aurora::app::ReadySource>> {
        std::vector<aurora::net::PollDescriptor> want;
        want.reserve(sources.size());
        for (const auto &source : sources) {
            want.push_back(aurora::net::PollDescriptor{
                .handle = source.handle, .want_read = source.interest.read, .want_write = source.interest.write});
        }
        auto polled = aurora::net::wait_ready(want, timeout);
        if (!polled) {
            return polled.error();
        }
        std::vector<aurora::app::ReadySource> ready;
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const auto &outcome = polled.value()[i];
            if (outcome.readable || outcome.writable || outcome.error || outcome.hangup) {
                ready.push_back(aurora::app::ReadySource{.token = sources[i].token,
                                                         .ready = aurora::app::SourceReady{.readable = outcome.readable,
                                                                                           .writable = outcome.writable,
                                                                                           .error = outcome.error,
                                                                                           .hangup = outcome.hangup}});
            }
        }
        return ready;
    };
}

}  // namespace

// 空句柄集合的等待是合法态：返回 OK 且结果向量亦为空（不是错误、不阻塞）。
AURORA_TEST_CASE(wait_ready_empty_set_is_ok) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no poll/WSAPoll semantics; aurora::net is not built for wasm");
#else
    const auto polled = aurora::net::wait_ready({}, AURORA_NET_POLL_TIMEOUT);
    AURORA_TEST_REQUIRE(polled.ok());
    AURORA_TEST_CHECK_TRUE(polled.value().empty());
#endif
}

// 单个 -1 无效槽位：整批等待仍成功，该槽全 false，且不影响同批其它有效槽的回报
// （无效句柄不进系统调用，避免一个坏句柄毁掉全批）。
AURORA_TEST_CASE(wait_ready_invalid_slot_does_not_fail_the_batch) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no poll/WSAPoll semantics; aurora::net is not built for wasm");
#else
    auto pair = make_connected_pair();
    const std::vector<aurora::net::PollDescriptor> want{
        {.handle = static_cast<std::intptr_t>(-1), .want_read = true, .want_write = true},
        {.handle = pair.server.native_handle(), .want_read = false, .want_write = true},
    };
    const auto polled = aurora::net::wait_ready(want, AURORA_NET_POLL_TIMEOUT);
    AURORA_TEST_REQUIRE(polled.ok());
    AURORA_TEST_REQUIRE_EQ(polled.value().size(), std::size_t{2});
    const auto &invalid = polled.value()[0];
    AURORA_TEST_CHECK_FALSE(invalid.readable);
    AURORA_TEST_CHECK_FALSE(invalid.writable);
    AURORA_TEST_CHECK_FALSE(invalid.error);
    AURORA_TEST_CHECK_FALSE(invalid.hangup);
    AURORA_TEST_CHECK_TRUE(polled.value()[1].writable);  // 有效槽不受无效槽牵连
#endif
}

// 连通回环流的生命周期就绪态：新建即可写（POLLOUT）而不可读；对端写入后可读；
// 排空后纯超时（OK、全 false，非错误）。三条都是 POSIX poll 与 Winsock WSAPoll 同色判据。
AURORA_TEST_CASE(wait_ready_writable_then_readable_then_idle) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no poll/WSAPoll semantics; aurora::net is not built for wasm");
#else
    auto pair = make_connected_pair();
    const std::vector<aurora::net::PollDescriptor> both{
        {.handle = pair.server.native_handle(), .want_read = true, .want_write = true}};

    const auto before = aurora::net::wait_ready(both, AURORA_NET_POLL_TIMEOUT);
    AURORA_TEST_REQUIRE(before.ok());
    AURORA_TEST_REQUIRE_EQ(before.value().size(), std::size_t{1});
    AURORA_TEST_CHECK_FALSE(before.value()[0].readable);
    AURORA_TEST_CHECK_TRUE(before.value()[0].writable);

    // 后两段只关注可读：Linux 回环上数据落地可以晚于发送返回，而可写位当场就成立，混在一起会让
    // `wait_ready` 立刻带著「不可读」返回，把内核往返时间从等待窗口里挤掉（Windows 同步投递掩盖了这点）。
    const std::vector<aurora::net::PollDescriptor> read_only{
        {.handle = pair.server.native_handle(), .want_read = true, .want_write = false}};

    write_payload(pair.client, "hello");
    const auto arrived = aurora::net::wait_ready(read_only, AURORA_NET_POLL_TIMEOUT);
    AURORA_TEST_REQUIRE(arrived.ok());
    AURORA_TEST_CHECK_TRUE(arrived.value()[0].readable);

    std::array<char, 16> buffer{};
    const auto got = pair.server.read_some(std::span(buffer));
    AURORA_TEST_REQUIRE(got.ok());
    AURORA_TEST_CHECK_EQ(got.value(), std::size_t{5});

    const auto idle = aurora::net::wait_ready(read_only, AURORA_NET_EMPTY_TIMEOUT);
    AURORA_TEST_REQUIRE(idle.ok());
    AURORA_TEST_CHECK_FALSE(idle.value()[0].readable);
#endif
}

// 端到端底座：注册口装上 `wait_ready` 版 Poller，登记真实 server 句柄（关注可读）→ 对端写入
// → `wait_and_dispatch` 触发一次 readable 回调（缺省 Dispatcher 就地同步）；句柄析构注销后
// `live_count` 归零、`wait_and_dispatch` 空转且回调不再抵达（§18-1 在真实 fd 上的落地）。
AURORA_TEST_CASE(event_source_registry_drives_real_fd_ready) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no poll/WSAPoll semantics; aurora::net is not built for wasm");
#else
    auto pair = make_connected_pair();
    auto &registry = aurora::app::EventSourceRegistry::instance();

    int hits = 0;
    aurora::app::SourceReady last_ready{};
    SeamGuard guard(poller_over_wait_ready());

    {
        auto reg = registry.register_source(pair.server.native_handle(), aurora::app::WaitInterest{.read = true},
                                            [&](const aurora::app::SourceReady &ready) {
                                                ++hits;
                                                last_ready = ready;
                                            });
        AURORA_TEST_REQUIRE(reg.ok());
        AURORA_TEST_CHECK_EQ(registry.live_count(), std::size_t{1});

        write_payload(pair.client, "x");
        const auto waited = registry.wait_and_dispatch(AURORA_NET_POLL_TIMEOUT);
        AURORA_TEST_REQUIRE(waited.ok());
        AURORA_TEST_CHECK_EQ(hits, 1);
        AURORA_TEST_CHECK_TRUE(last_ready.readable);

        const std::array<char, 8> drained = [&] {
            std::array<char, 8> buffer{};
            (void)pair.server.read_some(std::span(buffer));
            return buffer;
        }();
        (void)drained;

        auto moved_out = std::move(reg.value());
        AURORA_TEST_CHECK_TRUE(moved_out.is_registered());
    }  // moved_out 与 reg（已空态）在此析构：注销自有条目且空态句柄不二次注销

    AURORA_TEST_CHECK_EQ(registry.live_count(), std::size_t{0});
    AURORA_TEST_CHECK_TRUE(registry.sources().empty());

    write_payload(pair.client, "y");
    const auto after = registry.wait_and_dispatch(AURORA_NET_EMPTY_TIMEOUT);
    AURORA_TEST_REQUIRE(after.ok());
    AURORA_TEST_CHECK_EQ(hits, 1);  // 注销后不再产生新回调
#endif
}

}  // namespace aurora::test_cases::utest_net_poll
