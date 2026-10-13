/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/remote_connect.h + src/aurora/net/detail/remote_connect.cpp
/// 测试说明: 「名字 → 已建立的流」的编排回归，两半各自的可证性：竞速半在**零 DNS** 条件下断言（查表
///           经库内钩子注入），地址次序是 IPv6 首条居首、两族交替、本机 IPC 形态剔除且一条不增不减；
///           失败快跟让第二条候选不等交错窗口立即发起，先到先得让成功之后的那条**根本不发起**（第二
///           个监听口的 accept 数恒为 0），全败也只交付一次；一次名字连接**只查一次表**，而数字字面量
///           与入参不合用两类形态**一次都不查**（钩子计数即证据），空回调则连查表都不发起。完成恰一次
///           且携的是建成那条（`peer_endpoint` 可证）。隧道半用**进程内假代理**（回环监听口 + 脚本化
///           握手）逐值断言：CONNECT 请求行与 `Host` 字段、407 → `net-proxy-auth-required`、非 2xx →
///           `net-proxy-unreachable`、不会说话 → `net-proxy-protocol`；SOCKS5 的三阶段字节（方法协商 /
///           子协商 / CONNECT 应答）与同一非零字节按阶段分岔，且**阶段之间前移读偏移**（否则下一段会把
///           上一段当自己的开头重读）；两条成功用例都验证「读过头部块之后的字节属于隧道」——握手多读
///           的那段经接回队列先于内核字节送达且次序不失真。三条禁令各有负向断言：代理被拒 / 代理不可达
///           时，目标监听口的 accept 数恒为 0（**绝不回退直连**），`Direct` 形态则必须直连命中。全部走
///           宿主驱动形态以便把送达时刻钉死，wasm 腿无 fd 等待语义，用例体退化为 AURORA_TEST_SKIP。
/// ⚠️ 平台分支写在用例体内而非包裹 `AURORA_TEST_CASE` 声明（registry_integrity 静态扫源码）。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/app/event_source_registry.h"
#include "aurora/core/platform.h"
#include "aurora/core/result.h"
#include "aurora/net/address.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/detail/lookup.h"
#include "aurora/net/detail/remote_connect.h"
#include "aurora/net/engine.h"
#include "aurora/net/poll.h"
#include "aurora/net/proxy.h"
#include "aurora/net/transport.h"
#include "framework/async_wait.h"
#include "framework/aurora_test.h"
#include "framework/net_wait.h"

namespace aurora::test_cases::utest_net_remote_connect {
namespace {

using aurora::net::Cancellation;
using aurora::net::Endpoint;
using aurora::net::ProxyConfig;
using aurora::net::ProxyCredentials;
using aurora::net::StreamListener;
using aurora::net::StreamOptions;
using aurora::net::StreamTransport;
using aurora::test::AURORA_WAIT_BUDGET;
using aurora::test::net::EngineGuard;
using aurora::test::net::HostDriven;

/// @brief 「第二条候选根本不该发起」的观察窗：明显大于交错窗口，又不把挂死伪装成慢。
constexpr std::chrono::milliseconds AURORA_GRACE_WINDOW{600};

/// @brief 隧道成功时随头部块一起写出的标记字节（握手多读的那段必须原样回到应用）。
constexpr std::string_view AURORA_TUNNEL_MARKER{"TUNNELDATA"};

/// @brief 「这条还没让调用方出结果」的观察窗：握手中间的每一轮都以「继续等下一段」为正常结局，
///        用全预算去确认这件事会把每轮都拖成一次超时。
constexpr std::chrono::milliseconds AURORA_STEP_WINDOW{300};

/// @brief 本文件宿主等待的主题名：超时文本据此指明是竞速腿、握手段还是隧道没出结果。
constexpr std::string_view AURORA_PUMP_SUBJECT = "net-remote-connect-host-pump";

/// @brief 查表钩子守卫：退出即清除，钩子是进程级状态，不得留给同 runner 的其它用例。
///
/// 自带调用计数：入参不合用的形态**连查表都不该发起**，而「没发起」只能靠这里数出来——注入接缝本身
/// 不回报是否被走过。
struct LookupHook {
    /// @brief 装入固定回报。
    /// @param endpoints 钩子产出的端点列表。
    explicit LookupHook(std::vector<Endpoint> endpoints) : value(std::move(endpoints)) {
        aurora::net::detail::set_lookup_hook([this](std::string_view, std::uint16_t) {
            ++calls;
            return aurora::Result<std::vector<Endpoint>>{value};
        });
    }
    ~LookupHook() { aurora::net::detail::set_lookup_hook({}); }
    LookupHook(const LookupHook &) = delete;
    auto operator=(const LookupHook &) -> LookupHook & = delete;
    LookupHook(LookupHook &&) = delete;
    auto operator=(LookupHook &&) -> LookupHook & = delete;

    /// @brief 迄今被走到的次数。
    /// @return 次数。
    [[nodiscard]] auto lookup_calls() const -> int { return calls.load(); }

    std::vector<Endpoint> value;  ///< 固定回报
    std::atomic<int> calls{0};  ///< 钩子被走到的次数
};

/// @brief 本层用例统一取「不设连接时限」的口径：时限由引擎定时器触发，会把送达时刻挪到别的线程。
/// @param stagger 交错窗口。
/// @return 竞速与握手口径。
auto host_options(std::chrono::milliseconds stagger = std::chrono::milliseconds{0}) -> StreamOptions {
    StreamOptions options;
    options.connect_timeout = std::chrono::milliseconds{0};
    options.race_stagger = stagger;
    return options;
}

/// @brief 轮询宿主等待直到谓词成立或预算耗尽（转发到共用口径）。
///
/// 返回值是**静默** bool：本文件有把它当条件用的调用点（「这一步出结果就走另一条」），在转发里自带
/// 断言会把正常的分支判成红灯。超时现场由共用辅助记录，红时看 `AURORA_PUMP_SUBJECT` 指认阶段。
/// @param registry 进程级事件源注册口（形参为维持调用点形态而留，泵自取进程级单例）。
/// @param pred 目标条件。
/// @param budget 预算；缺省是「这一步必须出结果」的全预算，中间轮次应显式改短。
/// @return 预算内成立为 true。
auto pump_until(aurora::app::EventSourceRegistry &registry, const std::function<bool()> &pred,
                std::chrono::milliseconds budget = AURORA_WAIT_BUDGET) -> bool {
    (void)registry;  // NOLINT(bugprone-unused-parameter) 形参为维持调用点形态而留
    return aurora::test::net::drive_until(AURORA_PUMP_SUBJECT, pred, budget).satisfied;
}

/// @brief 让宿主空转一段有界时间（「不该发生的事」只能靠窗口观察，不能靠谓词等）。
/// @param registry 进程级事件源注册口（同上，泵自取进程级单例）。
/// @param window 窗口长度。
auto spin(aurora::app::EventSourceRegistry &registry, std::chrono::milliseconds window) -> void {
    (void)registry;  // NOLINT(bugprone-unused-parameter) 形参为维持调用点形态而留
    aurora::test::net::pump_idle(window);
}

/// @brief 一次连接完成的观测记录（回调可能落在引擎线程，故字段一律加锁读写）。
struct ConnectLog {
    std::mutex mutex;
    int calls = 0;
    bool failed = false;
    std::string code;
    std::string detail;  ///< 失败原因的 message（含现场上下文，红时靠它定位阶段）
    StreamTransport stream;  ///< 成功时接管
};

/// @brief 取已记录的回调用次数。
/// @param log 目标日志。
/// @return 次数。
auto calls_of(ConnectLog &log) -> int {
    std::scoped_lock lock(log.mutex);
    return log.calls;
}

/// @brief 取失败原因 slug（成功或未收尾时为空）。
/// @param log 目标日志。
/// @return slug 文本副本。
auto code_of(ConnectLog &log) -> std::string {
    std::scoped_lock lock(log.mutex);
    return log.code;
}

/// @brief 取失败原因 slug + 现场细节（红时用它指认「哪一阶段、为什么」）。
/// @param log 目标日志。
/// @return `slug / message` 文本；成功或未收尾时为空。
auto reason_of(ConnectLog &log) -> std::string {
    std::scoped_lock lock(log.mutex);
    return log.code.empty() ? std::string{} : log.code + " / " + log.detail;
}

/// @brief 把一次连接的结果落到日志里。
/// @param log 目标日志（须存活到回调抵达）。
/// @return 可直接交给 `connect_via` 或名字形态 `connect` 的回调。
auto connect_into(ConnectLog &log) -> aurora::net::ConnectDone {
    return [&log](auto result) {
        std::scoped_lock lock(log.mutex);
        if (!result) {
            log.failed = true;
            log.code = std::string{result.error().code};
            log.detail = result.error().message;
        } else {
            log.stream = std::move(result.value());
        }
        ++log.calls;
    };
}

/// @brief 一次握手腿读写的观测记录。
struct LegLog {
    std::atomic<int> calls{0};
    std::atomic<std::size_t> bytes{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> end_of_file{false};
};

/// @brief 把一次读的结果落到日志里。
/// @param log 目标日志。
/// @return 读回调。
auto read_into(LegLog &log) -> aurora::net::ReadDone {
    return [&log](auto result) {
        if (!result) {
            log.failed.store(true);
        } else if (result.value() == 0) {
            log.end_of_file.store(true);
        } else {
            log.bytes.store(result.value());
        }
        ++log.calls;
    };
}

/// @brief 把一次写的结果落到日志里。
/// @param log 目标日志。
/// @return 写回调。
auto write_into(LegLog &log) -> aurora::net::WriteDone {
    return [&log](auto result) {
        if (!result) {
            log.failed.store(true);
        } else {
            log.bytes.store(result.value());
        }
        ++log.calls;
    };
}

/// @brief 绑定一个回环临时端口监听口。
/// @return 就绪监听器（端口非 0）。
auto bind_loopback() -> StreamListener {
    auto bound = StreamListener::bind(Endpoint::loopback(0), 4, host_options());
    AURORA_TEST_REQUIRE(bound.ok());
    auto listener = std::move(bound.value());
    AURORA_TEST_REQUIRE_NE(listener.local_port(), std::uint16_t{0});
    return listener;
}

/// @brief 登记一枚 accept（只登记不等待：连接常在发起之后才可能交付）。
/// @param listener 目标监听口。
/// @param sink 存放交付的流。
/// @param accepted 计数（回调自增）。
auto arm_accept(StreamListener &listener, StreamTransport &sink, std::atomic<int> &accepted) -> void {
    listener.accept([&sink, &accepted](auto result) {
        // 致命断言会抛 CaseAbort，而本回调可能落在 `~StreamListener` 的派发路径上（析构边界内的抛出直接
        // `std::terminate`，整条测试进程连带消失），故这里只用非致命断言记录失败。
        AURORA_TEST_CHECK(result.ok());
        if (!result) {
            return;
        }
        sink = std::move(result.value());
        ++accepted;
    });
}

/// @brief 等到假代理侧的流交付到手：握手脚本只能对着已成立的代理连接读写字节，空句柄上的读会立刻落
///        `net-socket-failed`，把「代理不会说话」误记成「测试自己不会说话」。
/// @param registry 进程级事件源注册口。
/// @param accepted `arm_accept` 自增的计数。
auto await_proxy_side(aurora::app::EventSourceRegistry &registry, const std::atomic<int> &accepted) -> void {
    AURORA_TEST_REQUIRE(pump_until(registry, [&accepted]() { return accepted.load() >= 1; }));
}

/// @brief 从握手对端读字节直到谓词成立。
/// @param registry 进程级事件源注册口。
/// @param peer 握手对端（假代理侧或隧道内侧）。
/// @param pred 以「迄今读到的全部字节」为入参的谓词。
/// @return 迄今读到的字节文本。
auto read_until(aurora::app::EventSourceRegistry &registry, StreamTransport &peer,
                const std::function<bool(const std::string &)> &pred) -> std::string {
    std::string seen;
    std::vector<char> scratch(4096);
    for (std::size_t round = 0; round < 8; ++round) {
        if (pred(seen)) {
            return seen;
        }
        LegLog log;
        peer.read(std::span<char>(scratch), read_into(log));
        AURORA_TEST_REQUIRE(pump_until(registry, [&log]() { return log.calls.load() >= 1; }));
        AURORA_TEST_REQUIRE_FALSE(log.failed.load());
        if (log.end_of_file.load()) {
            break;
        }
        seen.append(scratch.data(), log.bytes.load());
    }
    AURORA_TEST_REQUIRE(pred(seen));  // 前提造不出来即转红，不带残缺字节继续跑
    return seen;
}

/// @brief 写出握手字节并轮询直到写完。
/// @param registry 进程级事件源注册口。
/// @param peer 握手对端。
/// @param payload 待写文本。
auto write_payload(aurora::app::EventSourceRegistry &registry, StreamTransport &peer, std::string_view payload)
    -> void {
    LegLog log;
    peer.write(std::span<const char>(payload.data(), payload.size()), write_into(log));
    AURORA_TEST_REQUIRE(pump_until(registry, [&log]() { return log.calls.load() >= 1; }));
    AURORA_TEST_REQUIRE_FALSE(log.failed.load());
}

/// @brief 把 SOCKS5 的八位组序列换成一串可直接写出的字节。
/// @param bytes 各八位组。
/// @return 文本形态的原始字节。
[[nodiscard]] auto raw(std::initializer_list<std::uint8_t> bytes) -> std::string {
    std::string out;
    out.reserve(bytes.size());
    for (const auto byte : bytes) {
        out.push_back(static_cast<char>(byte));
    }
    return out;
}

/// @brief 读满恰好 `count` 个八位组（SOCKS5 各阶段的定长报文）。
/// @param registry 进程级事件源注册口。
/// @param peer 握手对端。
/// @param count 目标字节数。
/// @return 读到的字节文本。
auto read_exact(aurora::app::EventSourceRegistry &registry, StreamTransport &peer, std::size_t count) -> std::string {
    return read_until(registry, peer, [count](const std::string &seen) { return seen.size() >= count; });
}

/// @brief 目标监听口 + accept 计数：用它证「有没有回退直连」。
struct TargetWatch {
    StreamListener listener;
    std::atomic<int> accepted{0};
    StreamTransport stream;
};

/// @brief 起一个目标监听口并登记 accept 计数（不回写任何字节，纯粹观察）。
/// @param watch 目标观察体（生命周期由用例负责）。
auto watch_target(TargetWatch &watch) -> void {
    watch.listener = bind_loopback();
    watch.listener.accept([&watch](auto result) {
        if (result) {
            watch.stream = std::move(result.value());
        }
        ++watch.accepted;
    });
}

/// @brief 一轮假代理脚本：先读够多少个八位组，再写出什么字节。
struct Step {
    std::size_t read_at_least;
    std::string reply;
};

/// @brief 一条「没有端口语义」的本机 IPC 地址字面量：只用于装配 `Endpoint`，本层从不触碰文件系统。
/// TEST_TEMP_EXEMPT: 地址字面量而非临时文件写入——用例断言的正是这种形态在发起系统调用前即被拒。
constexpr std::string_view AURORA_LOCAL_IPC_PATH = "/tmp/aurora-not-a-tcp-target";

}  // namespace

AURORA_TEST_CASE(race_preference_orders_families_and_drops_local_paths) {
    const auto v6_first = Endpoint::from_ip("2001:db8::1", 80);
    const auto v6_second = Endpoint::from_ip("2001:db8::2", 80);
    const auto v4_first = Endpoint::from_ip("192.0.2.1", 80);
    const auto v4_second = Endpoint::from_ip("192.0.2.2", 80);
    const auto local = Endpoint::local(AURORA_LOCAL_IPC_PATH);
    AURORA_TEST_REQUIRE(v6_first.ok());
    AURORA_TEST_REQUIRE(v6_second.ok());
    AURORA_TEST_REQUIRE(v4_first.ok());
    AURORA_TEST_REQUIRE(v4_second.ok());
    AURORA_TEST_REQUIRE(local.ok());
    const std::vector<Endpoint> resolved{v4_first.value(), v6_first.value(), local.value(), v4_second.value(),
                                         v6_second.value()};

    const auto ordered = aurora::net::detail::race_preference(resolved);

    // 本机 IPC 形态被剔除，其余一条不增不减；次序是「v6 首条 → v4 首条 → v6 次条 → v4 次条」。
    AURORA_TEST_REQUIRE_EQ(ordered.size(), std::size_t{4});
    AURORA_TEST_CHECK_TRUE(ordered[0] == v6_first.value());
    AURORA_TEST_CHECK_TRUE(ordered[1] == v4_first.value());
    AURORA_TEST_CHECK_TRUE(ordered[2] == v6_second.value());
    AURORA_TEST_CHECK_TRUE(ordered[3] == v4_second.value());
}

AURORA_TEST_CASE(racing_fast_follows_a_failed_candidate_without_waiting_for_the_window) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no socket to fail and recover from");
#else
    EngineGuard engine;
    HostDriven driven;
    auto &registry = aurora::app::EventSourceRegistry::instance();

    auto live = bind_loopback();
    auto dying = bind_loopback();
    const auto dead_port = dying.local_port();
    dying.close();  // 端口上再没有服务，连它必然立即被拒
    std::atomic<int> live_accepted{0};
    StreamTransport server;
    arm_accept(live, server, live_accepted);

    const auto dead = Endpoint::loopback(dead_port);
    const auto target = Endpoint::from_ip("127.0.0.1", live.local_port());
    AURORA_TEST_REQUIRE(target.ok());
    LookupHook hook{{dead, target.value()}};  // 死地址在前，快跟才有条可跳

    ConnectLog log;
    // 交错窗口置 0 即「不靠定时器前进」：第二条只能由第一条的失败发出来。
    StreamTransport::connect("fast-follow.example", 80, host_options(), Cancellation{}, connect_into(log));
    // 等到「连接出结果」且「监听口交付了服务端流」两件事都发生：Linux 上 connect 成功只是把连接放进内核
    // backlog，应用侧 accept 的投递要再晚一轮泵，只等前一件就会让后面的交付计数断言变成竞态。
    AURORA_TEST_REQUIRE(
        pump_until(registry, [&log, &live_accepted]() { return calls_of(log) >= 1 && live_accepted.load() >= 1; }));
    AURORA_TEST_CHECK_EQ(calls_of(log), 1);
    {
        std::scoped_lock lock(log.mutex);
        AURORA_TEST_REQUIRE_FALSE(log.failed);
        AURORA_TEST_REQUIRE(log.stream.is_open());
        const auto peer = log.stream.peer_endpoint();
        AURORA_TEST_REQUIRE(peer.ok());
        // 交付的是建成那条而不是列表首条：端口即活监听口。
        AURORA_TEST_CHECK_EQ(peer.value().port(), live.local_port());
    }
    AURORA_TEST_CHECK_EQ(live_accepted.load(), 1);
    // 一次名字连接只查一次表：竞速是在既有列表上推进，不是每条候选各解析一遍。
    AURORA_TEST_CHECK_EQ(hook.lookup_calls(), 1);
#endif
}

AURORA_TEST_CASE(racing_settles_once_with_the_earliest_failure_when_no_candidate_connects) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no socket to refuse");
#else
    EngineGuard engine;
    HostDriven driven;
    auto &registry = aurora::app::EventSourceRegistry::instance();

    auto first = bind_loopback();
    auto second = bind_loopback();
    const auto first_dead = first.local_port();
    const auto second_dead = second.local_port();
    first.close();
    second.close();  // 两个端口上都没有服务，连上去必然被拒

    const auto a = Endpoint::from_ip("127.0.0.1", first_dead);
    const auto b = Endpoint::from_ip("127.0.0.1", second_dead);
    AURORA_TEST_REQUIRE(a.ok());
    AURORA_TEST_REQUIRE(b.ok());
    LookupHook hook{std::vector<Endpoint>{a.value(), b.value()}};

    // 时限设短是为了让两条尝试都在本用例的栈还活着时收完：一台机器上「立即被拒」要等 2 秒（Windows
    // 的回环 SYN 重试），两条串起来就越过观察窗，而未落地的回调会在用例结束后踩到已销毁的日志。
    StreamOptions impatient = host_options();
    impatient.connect_timeout = std::chrono::milliseconds{300};

    ConnectLog log;
    StreamTransport::connect("nobody-listens.example", 80, impatient, Cancellation{}, connect_into(log));
    AURORA_TEST_REQUIRE(pump_until(registry, [&log]() { return calls_of(log) >= 1; }));
    spin(registry, std::chrono::milliseconds{400});
    // 全败也只交付一次，携的是连接类失败原因（不是「没有候选」这种无法排查的兜底码）。被拒还是到点
    // 因机器而异（回环立即 RST vs 路由黑洞），但两者都是连接类事实，且**只此一次**。
    AURORA_TEST_CHECK_EQ(calls_of(log), 1);
    const auto code = code_of(log);
    AURORA_TEST_CHECK_TRUE(code == "net-connect-failed" || code == "net-connect-timeout");
#endif
}

AURORA_TEST_CASE(name_connect_rejects_usage_errors_without_querying_the_resolver) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no resolver leg to bypass");
#else
    EngineGuard engine;
    HostDriven driven;
    auto &registry = aurora::app::EventSourceRegistry::instance();

    // 空主机名与 0 端口是用法错误：原因当场经回调送达，且**连查表都不发起**——让调用方去重试一个永远
    // 不合格的入参，比报个错更糟。
    const std::vector<std::pair<std::string, std::uint16_t>> unusable{
        {"", std::uint16_t{443}},
        {"usage-error.example", std::uint16_t{0}},
    };
    for (const auto &[host, port] : unusable) {
        LookupHook hook{std::vector<Endpoint>{Endpoint::loopback(1)}};
        ConnectLog log;
        StreamTransport::connect(host, port, host_options(), Cancellation{}, connect_into(log));
        AURORA_TEST_CHECK_EQ(calls_of(log), 1);
        AURORA_TEST_CHECK_EQ(code_of(log), "net-endpoint-invalid");
        AURORA_TEST_CHECK_EQ(hook.lookup_calls(), 0);
    }

    // 数字字面量在解析层短路：钩子计数为 0 就是「这一步没查网」的直接证据（离线机器连本机也得能连）。
    auto live = bind_loopback();
    std::atomic<int> accepted{0};
    StreamTransport server;
    arm_accept(live, server, accepted);
    LookupHook literal_hook{std::vector<Endpoint>{Endpoint::loopback(1)}};
    ConnectLog literal;
    StreamTransport::connect("127.0.0.1", live.local_port(), host_options(), Cancellation{}, connect_into(literal));
    AURORA_TEST_REQUIRE(pump_until(registry, [&literal]() { return calls_of(literal) >= 1; }));
    AURORA_TEST_CHECK_EQ(literal_hook.lookup_calls(), 0);
    {
        std::scoped_lock lock(literal.mutex);
        AURORA_TEST_REQUIRE_FALSE(literal.failed);
        AURORA_TEST_CHECK(literal.stream.is_open());
    }

    // 解析回报里一条可连的 TCP 端点都没有（全是本机 IPC 形态）：按「查不到可用地址」收尾，不去连一个
    // 没有端口语义的地址，也不静默成功。
    const auto local = Endpoint::local(AURORA_LOCAL_IPC_PATH);
    AURORA_TEST_REQUIRE(local.ok());
    LookupHook local_hook{std::vector<Endpoint>{local.value()}};
    ConnectLog local_log;
    StreamTransport::connect("ipc-only.example", 80, host_options(), Cancellation{}, connect_into(local_log));
    AURORA_TEST_REQUIRE(pump_until(registry, [&local_log]() { return calls_of(local_log) >= 1; }));
    AURORA_TEST_CHECK_EQ(calls_of(local_log), 1);
    AURORA_TEST_CHECK_EQ(code_of(local_log), "net-dns-failed");
    AURORA_TEST_CHECK_EQ(local_hook.lookup_calls(), 1);

    // 没有回调就没有承诺：不解析、不建句柄，事件源数量一分不增。
    const std::size_t before = registry.live_count();
    StreamTransport::connect("silent.example", 80, host_options(), Cancellation{}, {});
    AURORA_TEST_CHECK_EQ(registry.live_count(), before);
#endif
}

AURORA_TEST_CASE(racing_first_success_wins_and_the_later_candidate_is_never_started) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no socket to race over");
#else
    EngineGuard engine;
    HostDriven driven;
    auto &registry = aurora::app::EventSourceRegistry::instance();

    auto first = bind_loopback();
    auto second = bind_loopback();
    std::atomic<int> first_accepted{0};
    std::atomic<int> second_accepted{0};
    StreamTransport first_server;
    arm_accept(first, first_server, first_accepted);
    second.accept([&second_accepted](auto) { ++second_accepted; });

    const auto a = Endpoint::from_ip("127.0.0.1", first.local_port());
    const auto b = Endpoint::from_ip("127.0.0.1", second.local_port());
    AURORA_TEST_REQUIRE(a.ok());
    AURORA_TEST_REQUIRE(b.ok());
    LookupHook hook{{a.value(), b.value()}};

    ConnectLog log;
    StreamTransport::connect("first-wins.example", 80, host_options(std::chrono::milliseconds{200}), Cancellation{},
                             connect_into(log));
    // 两件事都等到才算「第一条赢」：connect 结果先把流交付到应用侧，监听口那一轮的 accept 还在后面。
    AURORA_TEST_REQUIRE(
        pump_until(registry, [&log, &first_accepted]() { return calls_of(log) >= 1 && first_accepted.load() >= 1; }));
    AURORA_TEST_CHECK_EQ(calls_of(log), 1);
    AURORA_TEST_CHECK_EQ(first_accepted.load(), 1);

    // 交错窗口到点时这条已经建成了：第二条候选根本不该发起，第二个监听口不该有任何 accept。
    spin(registry, AURORA_GRACE_WINDOW);
    AURORA_TEST_CHECK_EQ(calls_of(log), 1);
    AURORA_TEST_CHECK_EQ(second_accepted.load(), 0);
#endif
}

AURORA_TEST_CASE(connect_via_http_connect_tunnel_delivers_the_leftover_bytes) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no socket to tunnel over");
#else
    EngineGuard engine;
    HostDriven driven;
    auto &registry = aurora::app::EventSourceRegistry::instance();

    auto proxy = bind_loopback();
    TargetWatch watch;
    watch_target(watch);
    std::atomic<int> proxy_accepted{0};
    StreamTransport proxy_side;
    arm_accept(proxy, proxy_side, proxy_accepted);

    const auto config = ProxyConfig::http_connect("127.0.0.1", proxy.local_port(), {});
    AURORA_TEST_REQUIRE(config.ok());
    ConnectLog log;
    StreamTransport::connect_via(config.value(), "127.0.0.1", watch.listener.local_port(), host_options(),
                                 Cancellation{}, connect_into(log));
    await_proxy_side(registry, proxy_accepted);

    const auto request = read_until(registry, proxy_side,
                                    [](const std::string &seen) { return seen.find("\r\n\r\n") != std::string::npos; });
    AURORA_TEST_CHECK_TRUE(request.find("CONNECT 127.0.0.1:") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(request.find("\r\nHost: 127.0.0.1:") != std::string::npos);
    // 应答尾部粘着隧道的首字节：解析只吃掉自身长度，多读的那段必须回到读队列前端。
    write_payload(registry, proxy_side,
                  "HTTP/1.1 200 Connection Established\r\n\r\n" + std::string{AURORA_TUNNEL_MARKER});

    AURORA_TEST_REQUIRE(pump_until(registry, [&log]() { return calls_of(log) >= 1; }));
    AURORA_TEST_CHECK_EQ(calls_of(log), 1);
    StreamTransport client;
    {
        std::scoped_lock lock(log.mutex);
        AURORA_TEST_REQUIRE_FALSE(log.failed);
        client = std::move(log.stream);
    }
    const auto seen =
        read_until(registry, client, [](const std::string &got) { return got.size() >= AURORA_TUNNEL_MARKER.size(); });
    AURORA_TEST_CHECK(seen == std::string{AURORA_TUNNEL_MARKER});
    AURORA_TEST_CHECK_EQ(watch.accepted.load(), 0);  // 隧道成立，目标侧不该出现直连
    watch.listener.close();
#endif
}

AURORA_TEST_CASE(connect_via_http_connect_refusal_splits_by_status) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no socket to refuse on");
#else
    struct Script {
        std::string reply;
        std::string expected_code;
    };
    EngineGuard engine;
    HostDriven driven;
    auto &registry = aurora::app::EventSourceRegistry::instance();

    // 拒绝建隧道 / 要凭据 / 不会说话是三回事，排查方向各不相同，共用一码会让一半判断出错。
    const std::vector<Script> scripts{
        Script{.reply = "HTTP/1.1 407 Proxy Authentication Required\r\n\r\n",
               .expected_code = "net-proxy-auth-required"},
        Script{.reply = "HTTP/1.1 403 Forbidden\r\n\r\n", .expected_code = "net-proxy-unreachable"},
        Script{.reply = "NOT-A-STATUS-LINE\r\n\r\n", .expected_code = "net-proxy-protocol"},
    };

    for (const auto &script : scripts) {
        auto proxy = bind_loopback();
        TargetWatch watch;
        watch_target(watch);
        std::atomic<int> proxy_accepted{0};
        StreamTransport proxy_side;
        arm_accept(proxy, proxy_side, proxy_accepted);

        const auto config = ProxyConfig::http_connect("127.0.0.1", proxy.local_port(), {});
        AURORA_TEST_REQUIRE(config.ok());
        ConnectLog log;
        StreamTransport::connect_via(config.value(), "127.0.0.1", watch.listener.local_port(), host_options(),
                                     Cancellation{}, connect_into(log));
        await_proxy_side(registry, proxy_accepted);
        (void)read_until(registry, proxy_side,
                         [](const std::string &seen) { return seen.find("\r\n\r\n") != std::string::npos; });
        write_payload(registry, proxy_side, script.reply);

        AURORA_TEST_REQUIRE(pump_until(registry, [&log]() { return calls_of(log) >= 1; }));
        AURORA_TEST_CHECK_EQ(calls_of(log), 1);
        AURORA_TEST_CHECK_EQ(code_of(log), script.expected_code);
        AURORA_TEST_CHECK_EQ(watch.accepted.load(), 0);  // 三条分支一律不回退直连
        watch.listener.close();
    }
#endif
}

AURORA_TEST_CASE(connect_via_socks5_tunnel_walks_three_stages_and_keeps_leftovers) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no socket to tunnel over");
#else
    EngineGuard engine;
    HostDriven driven;
    auto &registry = aurora::app::EventSourceRegistry::instance();

    auto proxy = bind_loopback();
    TargetWatch watch;
    watch_target(watch);
    std::atomic<int> proxy_accepted{0};
    StreamTransport proxy_side;
    arm_accept(proxy, proxy_side, proxy_accepted);

    const auto config = ProxyConfig::socks5("127.0.0.1", proxy.local_port(), false, {});
    AURORA_TEST_REQUIRE(config.ok());
    ConnectLog log;
    StreamTransport::connect_via(config.value(), "127.0.0.1", watch.listener.local_port(), host_options(),
                                 Cancellation{}, connect_into(log));
    await_proxy_side(registry, proxy_accepted);

    const auto greeting = read_exact(registry, proxy_side, 3);
    AURORA_TEST_REQUIRE_EQ(greeting.size(), std::size_t{3});
    AURORA_TEST_CHECK_EQ(static_cast<unsigned>(static_cast<std::uint8_t>(greeting[0])), 5U);
    AURORA_TEST_CHECK_EQ(static_cast<unsigned>(static_cast<std::uint8_t>(greeting[1])), 1U);  // 只 offer 一种方法
    AURORA_TEST_CHECK_EQ(static_cast<unsigned>(static_cast<std::uint8_t>(greeting[2])), 0U);  // 不认证
    write_payload(registry, proxy_side, raw({0x05, 0x00}));

    // VER CMD RESV ATYPE ADDR(4) PORT(2) = 10 字节，目标走 ATYPE=1 的数字形态
    const auto request = read_exact(registry, proxy_side, 10);
    AURORA_TEST_REQUIRE_EQ(request.size(), std::size_t{10});
    AURORA_TEST_CHECK_EQ(static_cast<unsigned>(static_cast<std::uint8_t>(request[0])), 5U);
    AURORA_TEST_CHECK_EQ(static_cast<unsigned>(static_cast<std::uint8_t>(request[1])), 1U);  // CMD=CONNECT
    AURORA_TEST_CHECK_EQ(static_cast<unsigned>(static_cast<std::uint8_t>(request[3])), 1U);  // ATYPE=IPv4
    const auto port = watch.listener.local_port();
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(request[8]), static_cast<std::uint8_t>(port >> 8U));
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(request[9]), static_cast<std::uint8_t>(port & 0xFFU));
    // 应答恒 10 字节，其后字节全部属于隧道：一段写出即可验证「解析只吃掉自身长度」。
    write_payload(
        registry, proxy_side,
        raw({0x05, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}) + std::string{AURORA_TUNNEL_MARKER});

    AURORA_TEST_REQUIRE(pump_until(registry, [&log]() { return calls_of(log) >= 1; }));
    AURORA_TEST_CHECK_EQ(calls_of(log), 1);
    AURORA_TEST_CHECK_EQ(reason_of(log), std::string{});
    StreamTransport client;
    {
        std::scoped_lock lock(log.mutex);
        AURORA_TEST_REQUIRE(log.stream.is_open());
        client = std::move(log.stream);
    }
    const auto seen =
        read_until(registry, client, [](const std::string &got) { return got.size() >= AURORA_TUNNEL_MARKER.size(); });
    AURORA_TEST_CHECK(seen == std::string{AURORA_TUNNEL_MARKER});
    AURORA_TEST_CHECK_EQ(watch.accepted.load(), 0);
    watch.listener.close();
#endif
}

AURORA_TEST_CASE(connect_via_socks5_status_bytes_split_by_stage) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no socket to refuse on");
#else
    struct Script {
        std::vector<Step> steps;
        std::string expected_code;
    };
    EngineGuard engine;
    HostDriven driven;
    auto &registry = aurora::app::EventSourceRegistry::instance();

    // 同一个非零字节在方法协商 / 子协商 / CONNECT 应答三个阶段含义不同，故换算入口带阶段入参。
    const std::vector<Script> scripts{
        // 方法协商 0xFF = 「一种都没接」：本构建只 offer 免认证与用户名/密码，被全拒就是缺凭据。
        Script{.steps = {Step{.read_at_least = 3, .reply = raw({0x05, 0xFF})}},
               .expected_code = "net-proxy-auth-required"},
        // 子协商 = VER ULEN UNAME ULEN PASSWD = 1+1+10+1+10（凭据各 10 字节），非零状态即认证被拒。
        Script{.steps = {Step{.read_at_least = 3, .reply = raw({0x05, 0x02})},
                         Step{.read_at_least = 23, .reply = raw({0x01, 0x01})}},
               .expected_code = "net-proxy-auth-required"},
        // CONNECT 应答 = VER REP RESV ATYPE ADDR(4) PORT(2) = 10 字节，此处 REP=1 是「不可达」。
        Script{.steps = {Step{.read_at_least = 3, .reply = raw({0x05, 0x00})},
                         Step{.read_at_least = 10, .reply = raw({0x05, 0x01, 0x00, 0x01, 0, 0, 0, 0, 0, 0})}},
               .expected_code = "net-proxy-unreachable"},
        // 同一个位置的 `REP=7`（命令不支持）换成能力缺口而非网络事实，两码不可互换。
        Script{.steps = {Step{.read_at_least = 3, .reply = raw({0x05, 0x00})},
                         Step{.read_at_least = 10, .reply = raw({0x05, 0x07, 0x00, 0x01, 0, 0, 0, 0, 0, 0})}},
               .expected_code = "net-proxy-unsupported"},
    };

    for (const auto &script : scripts) {
        auto proxy = bind_loopback();
        TargetWatch watch;
        watch_target(watch);
        std::atomic<int> proxy_accepted{0};
        StreamTransport proxy_side;
        arm_accept(proxy, proxy_side, proxy_accepted);

        const auto config = ProxyConfig::socks5("127.0.0.1", proxy.local_port(), false,
                                                ProxyCredentials{.username = "proxy-user", .password = "proxy-pass"});
        AURORA_TEST_REQUIRE(config.ok());
        ConnectLog log;
        StreamTransport::connect_via(config.value(), "127.0.0.1", watch.listener.local_port(), host_options(),
                                     Cancellation{}, connect_into(log));
        await_proxy_side(registry, proxy_accepted);

        for (const auto &step : script.steps) {
            if (calls_of(log) >= 1) {
                break;  // 已收尾（例如方法被拒即不再发后续报文）
            }
            (void)read_exact(registry, proxy_side, step.read_at_least);
            write_payload(registry, proxy_side, step.reply);
            // 只看「这一步是否已让调用方出结果」，出不了就喂下一段——握手中间轮的正常结局是继续等。
            if (pump_until(registry, [&log]() { return calls_of(log) >= 1; }, AURORA_STEP_WINDOW)) {
                break;
            }
        }

        AURORA_TEST_REQUIRE(pump_until(registry, [&log]() { return calls_of(log) >= 1; }));
        AURORA_TEST_CHECK_EQ(calls_of(log), 1);
        AURORA_TEST_CHECK_EQ(code_of(log), script.expected_code);
        AURORA_TEST_CHECK_EQ(watch.accepted.load(), 0);  // 每个阶段都绝不回退直连
        watch.listener.close();
    }
#endif
}

AURORA_TEST_CASE(direct_proxy_config_connects_the_target_without_a_tunnel) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no socket to connect");
#else
    EngineGuard engine;
    HostDriven driven;
    auto &registry = aurora::app::EventSourceRegistry::instance();

    auto target = bind_loopback();
    std::atomic<int> accepted{0};
    StreamTransport target_side;
    arm_accept(target, target_side, accepted);

    ConnectLog log;
    StreamTransport::connect_via(ProxyConfig::direct(), "127.0.0.1", target.local_port(), host_options(),
                                 Cancellation{}, connect_into(log));
    AURORA_TEST_REQUIRE(
        pump_until(registry, [&log, &accepted]() { return calls_of(log) >= 1 && accepted.load() >= 1; }));
    AURORA_TEST_CHECK_EQ(calls_of(log), 1);
    {
        std::scoped_lock lock(log.mutex);
        AURORA_TEST_REQUIRE_FALSE(log.failed);
        AURORA_TEST_REQUIRE(log.stream.is_open());
    }
    AURORA_TEST_CHECK_EQ(accepted.load(), 1);  // 显式关闭代理就是直连，不是「还没决定」
#endif
}

AURORA_TEST_CASE(proxy_that_is_not_listening_fails_without_falling_back) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no socket to refuse");
#else
    EngineGuard engine;
    HostDriven driven;
    auto &registry = aurora::app::EventSourceRegistry::instance();

    auto dying = bind_loopback();
    const auto absent_port = dying.local_port();
    dying.close();
    TargetWatch watch;
    watch_target(watch);

    ConnectLog log;
    const auto absent = ProxyConfig::http_connect("127.0.0.1", absent_port, {});
    AURORA_TEST_REQUIRE(absent.ok());
    StreamTransport::connect_via(absent.value(), "127.0.0.1", watch.listener.local_port(), host_options(),
                                 Cancellation{}, connect_into(log));
    AURORA_TEST_REQUIRE(pump_until(registry, [&log]() { return calls_of(log) >= 1; }));
    AURORA_TEST_CHECK_EQ(calls_of(log), 1);
    // 代理连不上是连接类失败（不是代理类码），且**绝不改道直连目标**。
    AURORA_TEST_CHECK_TRUE(code_of(log) == "net-connect-failed" || code_of(log) == "net-socket-failed" ||
                           code_of(log) == "net-bind-failed");
    AURORA_TEST_CHECK_EQ(watch.accepted.load(), 0);

    ConnectLog bogus;
    StreamTransport::connect_via(absent.value(), "127.0.0.1", 0, host_options(), Cancellation{}, connect_into(bogus));
    AURORA_TEST_REQUIRE(pump_until(registry, [&bogus]() { return calls_of(bogus) >= 1; }));
    AURORA_TEST_CHECK_EQ(code_of(bogus), "net-endpoint-invalid");
    watch.listener.close();
#endif
}

}  // namespace aurora::test_cases::utest_net_remote_connect
