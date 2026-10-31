/// 测试类型: unit
/// 目标单元: include/aurora/net/datagram.h
/// 测试说明: aurora::net 事件驱动数据报（传输族 UDP 切片）的行为契约回归。四条形态裁决各有用例：
///           **一次 `receive()` 交付恰一条数据报**，且 `Datagram::source` 如实回报来源端点；
///           **零字节是一条消息**（空载荷 OK 送达，不是 EOF 也不是错误）；**截断显式回报**——预投两枚
///           接收槽再发两条数据报，短缓冲那一槽拿到第一条的前缀并置 `truncated`，下一槽拿到的必须是
///           **第二条**而不是第一条的尾巴；**拉模型**——没有待交付槽就不等待可读，收发失败**不关闭
///           套接字**（向广播地址发送却没置允许位只作废那一条请求，随后单播仍照常送达），这与流式传输
///           「写失败即关流」是刻意的差别。connect 模式过滤非对端数据报、`send_to()` 在固定对端上报
///           `net-option-conflict`（Linux/Windows 对带地址 `sendto` 的处理相反，不能有两种结局）；组播
///           与 connect 互斥同样报 `net-option-conflict`（提案 §18-3，不静默失效），非组播地址与族不符
///           报 `net-endpoint-invalid`，未加入过的组离开时报 `net-option-failed`；口径非法（单项上限为
///           0 或越过 65535、待发上限为 0）在 `bind()` 即拒，超发的单条数据报与被发队列高水位落
///           `net-buffer-limit`；`cancel()` 让挂起的接收恰一次以 `net-cancelled` 收尾并释放事件源登记；
///           空态句柄与空回调不崩不悬挂。除引擎那条外全部走宿主驱动形态（`set_host_driven(true)` +
///           `sources()` / `wait_ready` / `dispatch()`，共用口径见 `framework/net_wait.h`），据此把送达
///           时刻钉在用例线程上。
/// ⚠️ 平台分支写在**用例体内**而非包裹 `AURORA_TEST_CASE` 声明（registry_integrity 静态扫源码，
///    被 #ifdef 摘掉的声明在另一种构建里必然单边失踪）；wasm 无 fd 等待语义，用例体退化为
///    AURORA_TEST_SKIP。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/app/event_source_registry.h"
#include "aurora/core/platform.h"
#include "aurora/core/result.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/datagram.h"
#include "aurora/net/engine.h"
#include "framework/async_wait.h"
#include "framework/aurora_test.h"
#include "framework/net_wait.h"

namespace aurora::test_cases::utest_net_datagram {
namespace {

using aurora::net::Cancellation;
using aurora::net::Datagram;
using aurora::net::DatagramOptions;
using aurora::net::DatagramTransport;
using aurora::net::Endpoint;
using aurora::test::net::EngineGuard;
using aurora::test::net::HostDriven;

/// @brief 组播用例用的组地址（RFC 4312 管理范围，不会与真实路由组冲突）。
[[maybe_unused]] constexpr std::string_view AURORA_GROUP_V4 = "239.255.41.255";

/// @brief 本文件宿主等待的主题名：超时文本据此指明是收发轮次还是引擎驱动那一腿没出结果。
constexpr std::string_view AURORA_PUMP_SUBJECT = "net-datagram-host-pump";

/// @brief 本文件引擎驱动等待的主题名（本线程不碰 `dispatch()`）。
constexpr std::string_view AURORA_ENGINE_SUBJECT = "net-datagram-engine-driven";

/// @brief 轮询宿主等待直到谓词成立或预算耗尽（被测工作在回调里发生，同线程无法同步断言）。
///
/// 转发到共用口径，返回值是**静默** bool：调用点既有 `REQUIRE` 也有 `CHECK`，转发里自带断言会把
/// 后者的软判据变成硬红灯。
/// @param registry 进程级事件源注册口（形参为维持调用点形态而留，泵自取进程级单例）。
/// @param pred 目标条件。
/// @return 预算内成立为 true。
auto pump_until(aurora::app::EventSourceRegistry &registry, const std::function<bool()> &pred) -> bool {
    (void)registry;  // NOLINT(bugprone-unused-parameter) 形参为维持调用点形态而留
    return aurora::test::net::drive_until(AURORA_PUMP_SUBJECT, pred).satisfied;
}

/// @brief 有界等待谓词成立（引擎驱动形态：本线程完全不碰 `dispatch()`）。
/// @param pred 目标条件。
/// @return 预算内成立为 true。
[[maybe_unused]] auto wait_until(const std::function<bool()> &pred) -> bool {
    return aurora::test::net::engine_driven_wait(AURORA_ENGINE_SUBJECT, pred).satisfied;
}

/// @brief 绑出一枚回环数据报套接字（端口交给系统分配）。
/// @param options 缓冲口径。
/// @return 已就绪的套接字；绑不出来即 REQUIRE 转红（制造不出被测前提时不该继续跑）。
[[maybe_unused]] auto bind_socket(const DatagramOptions &options) -> DatagramTransport {
    auto bound = DatagramTransport::bind(Endpoint::loopback(0), options);
    AURORA_TEST_REQUIRE(bound.ok());
    auto socket = std::move(bound.value());
    AURORA_TEST_REQUIRE_NE(socket.local_port(), std::uint16_t{0});
    AURORA_TEST_REQUIRE_TRUE(socket.is_open());
    return socket;
}

/// @brief 绑出一枚回环套接字并按其端口组成目标端点，得到可互发的两台「主机内进程」。
/// @return 套接字与其自身端口组成的端点。
[[maybe_unused]] auto bind_pair_endpoint(DatagramTransport &socket) -> Endpoint {
    return Endpoint::loopback(socket.local_port());
}

/// @brief 一次发送的观测记录。
struct SendLog {
    std::atomic<int> calls{0};
    std::atomic<std::size_t> bytes{0};
    bool failed = false;  ///< 宿主轮询后同线程判读，无需原子
    std::string code;
};

/// @brief 一次接收的观测记录（载荷副本用于断言「哪一条数据报」）。
struct ReceiveLog {
    std::atomic<int> calls{0};
    std::atomic<std::size_t> bytes{0};
    std::string payload;
    std::uint16_t source_port = 0;
    bool truncated = false;
    bool failed = false;
    std::string code;
};

/// @brief 引擎驱动用例的最小记录：回调可能落在后台线程，本用例只数「恰几次」。
struct CountLog {
    std::atomic<int> calls{0};
    std::atomic<std::size_t> bytes{0};
};

/// @brief 把一次发送的结果落到日志里。
/// @param log 目标日志（须存活到回调抵达）。
/// @return 可直接交给 `send_to()` / `send()` 的回调。
auto send_into(SendLog &log) -> aurora::net::DatagramSendDone {
    return [&log](auto result) {
        if (!result) {
            log.failed = true;
            log.code = std::string{result.error().code};
        } else {
            log.bytes.store(result.value());
        }
        ++log.calls;
    };
}

/// @brief 把一次接收的结果落到日志里。
/// @param log 目标日志（须存活到回调抵达）。
/// @return 可直接交给 `receive()` 的回调。
[[maybe_unused]] auto receive_into(ReceiveLog &log) -> aurora::net::DatagramDone {
    return [&log](auto result) {
        if (!result) {
            log.failed = true;
            log.code = std::string{result.error().code};
        } else {
            const Datagram &message = result.value();
            log.bytes.store(message.bytes.size());
            log.payload.assign(message.bytes.begin(), message.bytes.end());
            log.source_port = message.source.port();
            log.truncated = message.truncated;
        }
        ++log.calls;
    };
}

/// @brief 把一次接收的结果只记次数（回调线程不确定时用）。
/// @param log 目标日志（须存活到回调抵达）。
/// @return 可直接交给 `receive()` 的回调。
[[maybe_unused]] auto receive_counted(CountLog &log) -> aurora::net::DatagramDone {
    return [&log](auto result) {
        if (result) {
            log.bytes.store(result.value().bytes.size());
        }
        ++log.calls;
    };
}

/// @brief 把一次发送的结果只记次数。
/// @param log 目标日志（须存活到回调抵达）。
/// @return 可直接交给 `send_to()` 的回调。
[[maybe_unused]] auto send_counted(CountLog &log) -> aurora::net::DatagramSendDone {
    return [&log](auto result) {
        if (result) {
            log.bytes.store(result.value());
        }
        ++log.calls;
    };
}

/// @brief 发一条数据报并轮询直到写完（制造「对端有一条数据报可读」这一前提）。
/// @param registry 进程级事件源注册口。
/// @param socket 发送方。
/// @param target 目标端点。
/// @param payload 载荷。
[[maybe_unused]] auto send_and_pump(aurora::app::EventSourceRegistry &registry, DatagramTransport &socket,
                                    const Endpoint &target, std::string_view payload) -> void {
    SendLog log;
    socket.send_to(std::span<const char>(payload.data(), payload.size()), target, send_into(log));
    AURORA_TEST_REQUIRE(pump_until(registry, [&log]() { return log.calls.load() >= 1; }));
    AURORA_TEST_REQUIRE_EQ(log.calls.load(), 1);
    AURORA_TEST_REQUIRE_FALSE(log.failed);
    AURORA_TEST_REQUIRE_EQ(log.bytes.load(), payload.size());
}

}  // namespace

// 数据报往返：一次 `receive()` 恰交付一条，来源端点如实回报，双向都成立。
AURORA_TEST_CASE(round_trip_delivers_exactly_one_datagram_with_source) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no file-descriptor wait semantics; a UDP socket cannot be bound");
#else
    HostDriven host;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    auto left = bind_socket(DatagramOptions{});
    auto right = bind_socket(DatagramOptions{});
    AURORA_TEST_CHECK_NE(left.native_handle(), -1);
    AURORA_TEST_CHECK_FALSE(left.peer().is_valid());  // 未固定对端不是错误，是「收任何来源」的形态

    std::vector<char> inbox(64);
    ReceiveLog arrived;
    right.receive(std::span<char>(inbox), receive_into(arrived));
    SendLog sent;
    left.send_to(std::span<const char>("ping", 4), bind_pair_endpoint(right), send_into(sent));

    AURORA_TEST_REQUIRE(pump_until(registry, [&]() { return arrived.calls.load() == 1 && sent.calls.load() == 1; }));
    AURORA_TEST_CHECK_EQ(sent.calls.load(), 1);
    AURORA_TEST_CHECK_FALSE(sent.failed);
    AURORA_TEST_CHECK_EQ(sent.bytes.load(), 4U);
    AURORA_TEST_CHECK_EQ(arrived.calls.load(), 1);
    AURORA_TEST_CHECK_FALSE(arrived.failed);
    AURORA_TEST_CHECK_EQ(arrived.payload, "ping");
    AURORA_TEST_CHECK_EQ(arrived.bytes.load(), 4U);
    AURORA_TEST_CHECK_FALSE(arrived.truncated);
    AURORA_TEST_CHECK_EQ(arrived.source_port, left.local_port());
    AURORA_TEST_CHECK_TRUE(arrived.source_port != right.local_port());

    // 反向同一条通道：换方向后来源端口跟着换成新的发送方。
    std::vector<char> back(64);
    ReceiveLog returned;
    left.receive(std::span<char>(back), receive_into(returned));
    SendLog replied;
    right.send_to(std::span<const char>("pong", 4), bind_pair_endpoint(left), send_into(replied));
    AURORA_TEST_REQUIRE(
        pump_until(registry, [&]() { return returned.calls.load() == 1 && replied.calls.load() == 1; }));
    AURORA_TEST_CHECK_FALSE(replied.failed);
    AURORA_TEST_CHECK_EQ(returned.payload, "pong");
    AURORA_TEST_CHECK_EQ(returned.source_port, right.local_port());
    AURORA_TEST_CHECK_TRUE(left.is_open());
    AURORA_TEST_CHECK_TRUE(right.is_open());
#endif
}

// 零字节是一条消息：空载荷以 OK 形态送达且 `truncated == false`，既不是 EOF 也不是错误。
AURORA_TEST_CASE(zero_length_datagram_is_a_message_not_eof) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no UDP socket to carry an empty message");
#else
    HostDriven host;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    auto sender = bind_socket(DatagramOptions{});
    auto receiver = bind_socket(DatagramOptions{});

    std::vector<char> inbox(32);
    ReceiveLog arrived;
    receiver.receive(std::span<char>(inbox), receive_into(arrived));
    const std::vector<char> no_bytes;
    SendLog sent;
    sender.send_to(std::span<const char>(no_bytes), bind_pair_endpoint(receiver), send_into(sent));

    AURORA_TEST_REQUIRE(pump_until(registry, [&]() { return arrived.calls.load() == 1 && sent.calls.load() == 1; }));
    AURORA_TEST_CHECK_EQ(sent.calls.load(), 1);
    AURORA_TEST_CHECK_FALSE(sent.failed);
    AURORA_TEST_CHECK_EQ(sent.bytes.load(), 0U);
    AURORA_TEST_CHECK_EQ(arrived.calls.load(), 1);
    AURORA_TEST_CHECK_FALSE(arrived.failed);  // 空载荷若被当成终止信号，上层永远读不到合法的空调用
    AURORA_TEST_CHECK_EQ(arrived.bytes.load(), 0U);
    AURORA_TEST_CHECK_TRUE(arrived.payload.empty());
    AURORA_TEST_CHECK_FALSE(arrived.truncated);

    // 空消息之后同一条通道仍能送真数据报：空槽没有被误当作「收尾」。
    std::vector<char> next(32);
    ReceiveLog followed;
    receiver.receive(std::span<char>(next), receive_into(followed));
    send_and_pump(registry, sender, bind_pair_endpoint(receiver), "after");
    AURORA_TEST_REQUIRE(pump_until(registry, [&]() { return followed.calls.load() == 1; }));
    AURORA_TEST_CHECK_EQ(followed.payload, "after");
#endif
}

// 截断显式回报且**不回头补投**：预投两枚槽，短缓冲那槽拿第一条的前缀并置 `truncated`，另一槽必须拿
// 第二条数据报（Windows 的 `recvfrom` 缓冲不足会整条丢弃、POSIX 静默截断，库内先整条读进 65535 缓冲
// 再裁剪，于是两条腿都只裁一次）。
AURORA_TEST_CASE(truncation_is_flagged_and_next_slot_gets_next_datagram) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no UDP socket to truncate");
#else
    HostDriven host;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    auto sender = bind_socket(DatagramOptions{});
    auto receiver = bind_socket(DatagramOptions{});
    const auto target = bind_pair_endpoint(receiver);

    std::vector<char> small(4);
    std::vector<char> roomy(64);
    ReceiveLog first_slot;
    ReceiveLog second_slot;
    receiver.receive(std::span<char>(small), receive_into(first_slot));
    receiver.receive(std::span<char>(roomy), receive_into(second_slot));

    send_and_pump(registry, sender, target, "abcdefghijkl");
    send_and_pump(registry, sender, target, "second");

    AURORA_TEST_REQUIRE(pump_until(registry, [&]() { return second_slot.calls.load() == 1; }));
    AURORA_TEST_CHECK_EQ(first_slot.calls.load(), 1);
    AURORA_TEST_CHECK_FALSE(first_slot.failed);
    AURORA_TEST_CHECK_EQ(first_slot.bytes.load(), 4U);
    AURORA_TEST_CHECK_EQ(first_slot.payload, "abcd");
    AURORA_TEST_CHECK_TRUE(first_slot.truncated);
    AURORA_TEST_CHECK_EQ(second_slot.calls.load(), 1);
    AURORA_TEST_CHECK_EQ(second_slot.payload, "second");  // 是下一条消息，不是上一条的尾巴
    AURORA_TEST_CHECK_FALSE(second_slot.truncated);
    AURORA_TEST_CHECK_EQ(second_slot.bytes.load(), 6U);

    // 合同口径也能收紧单次交付：`max_datagram_bytes` 与调用方缓冲取小者。
    DatagramOptions tight;
    tight.max_datagram_bytes = 3U;
    auto capped = bind_socket(tight);
    std::vector<char> wide(64);
    ReceiveLog clipped;
    capped.receive(std::span<char>(wide), receive_into(clipped));
    send_and_pump(registry, sender, bind_pair_endpoint(capped), "abcdefg");
    AURORA_TEST_REQUIRE(pump_until(registry, [&]() { return clipped.calls.load() == 1; }));
    AURORA_TEST_CHECK_EQ(clipped.bytes.load(), 3U);
    AURORA_TEST_CHECK_TRUE(clipped.truncated);
#endif
}

// connect 模式过滤非对端数据报，并改掉两个入口：固定对端上 `send_to()` 报 `net-option-conflict`
// （Linux 让带地址的 `sendto` 直接失败、Windows 忽略该地址，同一调用不能有两种结局），`send()` 走
// `send(2)` 通道。
AURORA_TEST_CASE(connected_socket_receives_only_from_its_peer) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no UDP socket to connect");
#else
    HostDriven host;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    auto peer = bind_socket(DatagramOptions{});
    auto stranger = bind_socket(DatagramOptions{});

    DatagramOptions options;
    auto connected = DatagramTransport::connect(bind_pair_endpoint(peer), options);
    AURORA_TEST_REQUIRE(connected.ok());
    auto fixed = std::move(connected.value());
    AURORA_TEST_CHECK_TRUE(fixed.peer().is_valid());
    AURORA_TEST_CHECK_EQ(fixed.peer().port(), peer.local_port());
    AURORA_TEST_CHECK_NE(fixed.local_port(), std::uint16_t{0});

    // 固定对端的一方发的数据报正常送达（来源端点即该对端）。
    std::vector<char> inbox(32);
    ReceiveLog from_peer;
    fixed.receive(std::span<char>(inbox), receive_into(from_peer));
    send_and_pump(registry, peer, bind_pair_endpoint(fixed), "hello");
    AURORA_TEST_REQUIRE(pump_until(registry, [&]() { return from_peer.calls.load() == 1; }));
    AURORA_TEST_CHECK_EQ(from_peer.payload, "hello");
    AURORA_TEST_CHECK_EQ(from_peer.source_port, peer.local_port());

    // 第三方朝这个端口发：内核不交付，槽位仍空着。
    std::vector<char> other(32);
    ReceiveLog from_stranger;
    fixed.receive(std::span<char>(other), receive_into(from_stranger));
    SendLog ignored;
    stranger.send_to(std::span<const char>("nope", 4), Endpoint::loopback(fixed.local_port()), send_into(ignored));
    AURORA_TEST_REQUIRE(pump_until(registry, [&]() { return ignored.calls.load() == 1; }));
    AURORA_TEST_CHECK_FALSE(ignored.failed);  // 发出去了：UDP 没有投递回执，过滤发生在接收侧
    AURORA_TEST_CHECK_EQ(from_stranger.calls.load(), 0);

    // 固定对端上 `send_to()` 是直接拒绝的用法错误，`send()` 才是该形态的出口。
    SendLog conflicting;
    fixed.send_to(std::span<const char>("x", 1), bind_pair_endpoint(peer), send_into(conflicting));
    AURORA_TEST_CHECK_EQ(conflicting.calls.load(), 1);
    AURORA_TEST_CHECK_TRUE(conflicting.failed);
    AURORA_TEST_CHECK_EQ(conflicting.code, "net-option-conflict");

    SendLog via_send;
    fixed.send(std::span<const char>("back", 4), send_into(via_send));
    AURORA_TEST_REQUIRE(pump_until(registry, [&]() { return via_send.calls.load() == 1; }));
    AURORA_TEST_CHECK_FALSE(via_send.failed);
    std::vector<char> peer_box(32);
    ReceiveLog peer_arrived;
    peer.receive(std::span<char>(peer_box), receive_into(peer_arrived));
    AURORA_TEST_CHECK(pump_until(registry, [&]() { return peer_arrived.calls.load() == 1; }));
    AURORA_TEST_CHECK_EQ(peer_arrived.payload, "back");
    AURORA_TEST_CHECK_TRUE(fixed.is_open());
#endif
}

// 组播与 connect 互斥（提案 §18-3：静默接受等于造出永远收不到包的配置），校验分支在系统调用前即落；
// 加入 / 离开的记录与内核一致。内核层面的加入依赖本机有可组播的接口，缺它时只认失败码不改判据。
AURORA_TEST_CASE(multicast_membership_conflicts_with_connect_and_validates_group) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no UDP socket to join a group");
#else
    HostDriven host;
    auto group = Endpoint::from_ip(AURORA_GROUP_V4, 51111);
    AURORA_TEST_REQUIRE(group.ok());

    auto peer = bind_socket(DatagramOptions{});
    DatagramOptions options;
    auto connected = DatagramTransport::connect(bind_pair_endpoint(peer), options);
    AURORA_TEST_REQUIRE(connected.ok());
    auto fixed = std::move(connected.value());
    auto conflicting = fixed.join_multicast_group(group.value());
    AURORA_TEST_CHECK_FALSE(conflicting.ok());
    if (!conflicting) {
        AURORA_TEST_CHECK_EQ(std::string{conflicting.error().code}, "net-option-conflict");
    }
    auto leaving = fixed.leave_multicast_group(group.value());
    AURORA_TEST_CHECK_FALSE(leaving.ok());

    auto socket = bind_socket(DatagramOptions{});
    auto not_a_group = socket.join_multicast_group(Endpoint::loopback(51111));
    AURORA_TEST_CHECK_FALSE(not_a_group.ok());
    if (!not_a_group) {
        AURORA_TEST_CHECK_EQ(std::string{not_a_group.error().code}, "net-endpoint-invalid");
    }
    auto other_family = Endpoint::from_ip("ff12::8", 51111);
    AURORA_TEST_REQUIRE(other_family.ok());
    auto mismatched = socket.join_multicast_group(other_family.value());
    AURORA_TEST_CHECK_FALSE(mismatched.ok());
    if (!mismatched) {
        AURORA_TEST_CHECK_EQ(std::string{mismatched.error().code}, "net-endpoint-invalid");
    }

    auto joined = socket.join_multicast_group(group.value());
    if (!joined) {
        // 加入组播要本机有可组播的路由 / 接口：缺它时内核拒绝加入，这是环境项而不是契约违例。
        // 悄悄放行会让下面这批强断言在某台机器上永不执行，故整条用例判为跳过。
        AURORA_TEST_CHECK_EQ(std::string{joined.error().code}, "net-option-failed");
        AURORA_TEST_SKIP("the kernel refused the multicast join: this host has no multicast-capable route");
    }
    AURORA_TEST_CHECK_TRUE(socket.multicast_loopback());  // 内核缺省：本端发出的组播报回投本端
    auto looped = socket.set_multicast_loopback(false);
    AURORA_TEST_CHECK_TRUE(looped.ok());
    AURORA_TEST_CHECK_FALSE(socket.multicast_loopback());
    AURORA_TEST_CHECK_TRUE(socket.set_multicast_loopback(true).ok());
    AURORA_TEST_CHECK_TRUE(socket.multicast_loopback());

    auto left = socket.leave_multicast_group(group.value());
    AURORA_TEST_CHECK_TRUE(left.ok());
    auto again = socket.leave_multicast_group(group.value());
    AURORA_TEST_CHECK_FALSE(again.ok());  // 没加入过的组不去问内核，否则报出与现场无关的原生码
    if (!again) {
        AURORA_TEST_CHECK_EQ(std::string{again.error().code}, "net-option-failed");
    }

    auto second_group = Endpoint::from_ip("239.255.42.42", 51111);
    AURORA_TEST_REQUIRE(second_group.ok());
    AURORA_TEST_CHECK_TRUE(socket.join_multicast_group(second_group.value()).ok());
    auto wrong_record = socket.leave_multicast_group(group.value());
    AURORA_TEST_CHECK_FALSE(wrong_record.ok());  // 记录里只剩第二个组
    AURORA_TEST_CHECK_TRUE(socket.leave_multicast_group(second_group.value()).ok());
#endif
}

// 广播允许位如实记录，且**缺它时那条发送失败不关闭套接字**：数据报没有连接状态可破坏，一条消息的失败
// 只作废该条请求，队列继续推进（口径与流式传输「写失败即关流」相反）。
AURORA_TEST_CASE(broadcast_without_allow_bit_fails_only_that_send) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no UDP socket to broadcast");
#else
    HostDriven host;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    auto sender = bind_socket(DatagramOptions{});
    auto receiver = bind_socket(DatagramOptions{});

    AURORA_TEST_CHECK_FALSE(sender.broadcast_allowed());  // 内核缺省：不允许
    AURORA_TEST_CHECK_TRUE(sender.set_broadcast_allowed(true).ok());
    AURORA_TEST_CHECK_TRUE(sender.broadcast_allowed());
    AURORA_TEST_CHECK_TRUE(sender.set_broadcast_allowed(false).ok());
    AURORA_TEST_CHECK_FALSE(sender.broadcast_allowed());

    auto broadcast = Endpoint::from_ip("255.255.255.255", receiver.local_port());
    AURORA_TEST_REQUIRE(broadcast.ok());

    std::vector<char> inbox(32);
    ReceiveLog arrived;
    receiver.receive(std::span<char>(inbox), receive_into(arrived));
    SendLog refused;
    sender.send_to(std::span<const char>("bcast", 5), broadcast.value(), send_into(refused));
    AURORA_TEST_REQUIRE(pump_until(registry, [&]() { return refused.calls.load() == 1; }));
    AURORA_TEST_CHECK_EQ(refused.calls.load(), 1);  // 恰一次落到这一条请求上
    AURORA_TEST_CHECK_TRUE(refused.failed);  // 未置允许位时被内核拒绝（EPERM / WSAEACCES）
    AURORA_TEST_CHECK_TRUE(sender.is_open());  // 套接字不因此退役
    AURORA_TEST_CHECK_EQ(sender.pending_send_bytes(), 0U);

    // 同一条套接字随后照常完成一次单播：证明失败只作废那一条，没有把队列或句柄带下去。
    send_and_pump(registry, sender, bind_pair_endpoint(receiver), "unicast");
    AURORA_TEST_REQUIRE(pump_until(registry, [&]() { return arrived.calls.load() == 1; }));
    AURORA_TEST_CHECK_EQ(arrived.payload, "unicast");
    AURORA_TEST_CHECK_EQ(arrived.source_port, sender.local_port());
#endif
}

// 缓冲口径：单条越过 `max_datagram_bytes` 与被发队列越过高水位都落 `net-buffer-limit`，且都在发起
// 系统调用前定形（回调就地抵达）；非法口径在 `bind()` 即拒。
AURORA_TEST_CASE(size_caps_reject_before_the_syscall) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no UDP socket to size-check");
#else
    HostDriven host;
    auto &registry = aurora::app::EventSourceRegistry::instance();

    DatagramOptions tight;
    tight.max_datagram_bytes = 8U;
    auto sender = bind_socket(tight);
    const std::string oversize(9, 'x');
    SendLog too_big;
    sender.send_to(std::span<const char>(oversize), Endpoint::loopback(sender.local_port()), send_into(too_big));
    AURORA_TEST_CHECK_EQ(too_big.calls.load(), 1);
    AURORA_TEST_CHECK_TRUE(too_big.failed);
    AURORA_TEST_CHECK_EQ(too_big.code, "net-buffer-limit");
    AURORA_TEST_CHECK_EQ(sender.pending_send_bytes(), 0U);

    // 内核上界之外的长度连副本都不建：65535 字节是 UDP 载荷的物理上限，不是平台差异。
    const std::string absurd(65536, 'y');
    SendLog absurd_log;
    sender.send_to(std::span<const char>(absurd), Endpoint::loopback(sender.local_port()), send_into(absurd_log));
    AURORA_TEST_CHECK_EQ(absurd_log.calls.load(), 1);
    AURORA_TEST_CHECK_TRUE(absurd_log.failed);
    AURORA_TEST_CHECK_EQ(absurd_log.code, "net-buffer-limit");

    // 被发队列高水位：宿主驱动且本轮不 dispatch 时请求留在库里，字节数如实可查，越过上限即拒。
    DatagramOptions queue_bound;
    queue_bound.max_pending_bytes = 16U;
    auto queued = bind_socket(queue_bound);
    const auto target = bind_pair_endpoint(sender);
    SendLog first;
    queued.send_to(std::span<const char>("12345678", 8), target, send_into(first));
    AURORA_TEST_CHECK_EQ(queued.pending_send_bytes(), 8U);
    SendLog second;
    queued.send_to(std::span<const char>("12345678", 8), target, send_into(second));
    AURORA_TEST_CHECK_EQ(queued.pending_send_bytes(), 16U);
    SendLog third;
    queued.send_to(std::span<const char>("12345678", 8), target, send_into(third));
    AURORA_TEST_CHECK_EQ(third.calls.load(), 1);
    AURORA_TEST_CHECK_TRUE(third.failed);
    AURORA_TEST_CHECK_EQ(third.code, "net-buffer-limit");
    AURORA_TEST_CHECK_EQ(queued.pending_send_bytes(), 16U);
    AURORA_TEST_CHECK_EQ(first.calls.load(), 0);  // 前两条仍排着队，没被误收尾
    AURORA_TEST_REQUIRE(pump_until(registry, [&]() { return first.calls.load() == 1 && second.calls.load() == 1; }));
    AURORA_TEST_CHECK_FALSE(first.failed);
    AURORA_TEST_CHECK_EQ(first.bytes.load(), 8U);
    AURORA_TEST_CHECK_EQ(queued.pending_send_bytes(), 0U);

    // 口径非法的组合都在绑定时即拒，不留一枚「看似能用」的套接字。
    DatagramOptions zero_cap;
    zero_cap.max_datagram_bytes = 0U;
    auto zero = DatagramTransport::bind(Endpoint::loopback(0), zero_cap);
    AURORA_TEST_CHECK_FALSE(zero.ok());
    DatagramOptions huge_cap;
    huge_cap.max_datagram_bytes = 65536U;
    AURORA_TEST_CHECK_FALSE(DatagramTransport::bind(Endpoint::loopback(0), huge_cap).ok());
    DatagramOptions zero_pending;
    zero_pending.max_pending_bytes = 0U;
    AURORA_TEST_CHECK_FALSE(DatagramTransport::bind(Endpoint::loopback(0), zero_pending).ok());
    auto local = Endpoint::local("aurora-datagram.sock");
    AURORA_TEST_REQUIRE(local.ok());
    auto ipc = DatagramTransport::bind(local.value(), DatagramOptions{});
    AURORA_TEST_CHECK_FALSE(ipc.ok());  // Windows 命名管道没有数据报语义，本机 IPC 形态不进本层
    if (!ipc) {
        AURORA_TEST_CHECK_EQ(std::string{ipc.error().code}, "net-endpoint-invalid");
    }
    auto no_port = DatagramTransport::connect(Endpoint::loopback(0), DatagramOptions{});
    AURORA_TEST_CHECK_FALSE(no_port.ok());
    if (!no_port) {
        AURORA_TEST_CHECK_EQ(std::string{no_port.error().code}, "net-endpoint-invalid");
    }
    auto ipc_peer = DatagramTransport::connect(local.value(), DatagramOptions{});
    AURORA_TEST_CHECK_FALSE(ipc_peer.ok());
#endif
}

// 取消与关闭：挂起的收发各恰一次以 `net-cancelled` 收尾，事件源登记随之释放；空态句柄与空回调不崩
// 不悬挂，`close()` 幂等。
AURORA_TEST_CASE(cancel_and_close_finish_pending_requests_once) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no UDP socket to cancel");
#else
    HostDriven host;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    auto socket = bind_socket(DatagramOptions{});
    const std::size_t baseline = registry.live_count();

    std::vector<char> inbox(32);
    ReceiveLog hung;
    socket.receive(std::span<char>(inbox), receive_into(hung));
    AURORA_TEST_CHECK_EQ(registry.live_count(), baseline + 1);  // 有槽才等待可读（拉模型）
    AURORA_TEST_CHECK_EQ(hung.calls.load(), 0);

    auto token = socket.cancellation();
    token.cancel();
    AURORA_TEST_CHECK_EQ(hung.calls.load(), 1);
    AURORA_TEST_CHECK_TRUE(hung.failed);
    AURORA_TEST_CHECK_EQ(hung.code, "net-cancelled");
    AURORA_TEST_CHECK_FALSE(socket.is_open());
    AURORA_TEST_CHECK_EQ(socket.native_handle(), -1);
    AURORA_TEST_CHECK_EQ(registry.live_count(), baseline);  // §18-4：先释放 fd，不留悬挂等待项
    token.cancel();
    AURORA_TEST_CHECK_EQ(hung.calls.load(), 1);

    SendLog after_close;
    socket.send_to(std::span<const char>("x", 1), Endpoint::loopback(51111), send_into(after_close));
    AURORA_TEST_CHECK_EQ(after_close.calls.load(), 1);
    AURORA_TEST_CHECK_TRUE(after_close.failed);
    AURORA_TEST_CHECK_EQ(after_close.code, "net-cancelled");
    socket.close();  // 幂等

    auto other = bind_socket(DatagramOptions{});
    ReceiveLog queued;
    other.receive(std::span<char>(inbox), receive_into(queued));
    other.close();
    AURORA_TEST_CHECK_EQ(queued.calls.load(), 1);
    AURORA_TEST_CHECK_EQ(queued.code, "net-cancelled");
    AURORA_TEST_CHECK_EQ(registry.live_count(), baseline);

    DatagramTransport orphan;
    AURORA_TEST_CHECK_FALSE(orphan.is_open());
    AURORA_TEST_CHECK_EQ(orphan.native_handle(), -1);
    AURORA_TEST_CHECK_EQ(orphan.local_port(), std::uint16_t{0});
    AURORA_TEST_CHECK_FALSE(orphan.peer().is_valid());
    AURORA_TEST_CHECK_EQ(orphan.pending_send_bytes(), 0U);
    AURORA_TEST_CHECK_TRUE(orphan.multicast_loopback());
    AURORA_TEST_CHECK_FALSE(orphan.broadcast_allowed());
    AURORA_TEST_CHECK_FALSE(orphan.join_multicast_group(Endpoint::loopback(51111)).ok());
    AURORA_TEST_CHECK_FALSE(orphan.leave_multicast_group(Endpoint::loopback(51111)).ok());
    AURORA_TEST_CHECK_FALSE(orphan.set_multicast_loopback(false).ok());
    AURORA_TEST_CHECK_FALSE(orphan.set_broadcast_allowed(true).ok());
    auto orphan_token = orphan.cancellation();
    orphan_token.cancel();  // 空态令牌的 cancel 不产生任何效果，也不得崩溃

    ReceiveLog orphan_receive;
    orphan.receive(std::span<char>(inbox), receive_into(orphan_receive));
    AURORA_TEST_CHECK_EQ(orphan_receive.calls.load(), 1);
    AURORA_TEST_CHECK_TRUE(orphan_receive.failed);
    SendLog orphan_send;
    orphan.send(std::span<const char>("x", 1), send_into(orphan_send));
    AURORA_TEST_CHECK_EQ(orphan_send.calls.load(), 1);
    AURORA_TEST_CHECK_TRUE(orphan_send.failed);

    // 空回调不构成任何承诺：不建副本、不登记事件源。
    const std::size_t before_null = registry.live_count();
    auto live = bind_socket(DatagramOptions{});
    live.send_to(std::span<const char>("x", 1), Endpoint::loopback(51111), {});
    std::vector<char> box(8);
    live.receive(std::span<char>(box), {});
    AURORA_TEST_CHECK_EQ(registry.live_count(), before_null);
    auto empty_box = std::span<char>{};
    ReceiveLog empty_buffer;
    live.receive(empty_box, receive_into(empty_buffer));
    AURORA_TEST_CHECK_EQ(empty_buffer.calls.load(), 1);  // 空缓冲是用法错误，不是「等一条空消息」
    AURORA_TEST_CHECK_EQ(empty_buffer.code, "net-buffer-limit");
#endif
}

// 引擎驱动形态（本库各平台腿的统一形态）：用例线程不做任何 `dispatch()`，一条数据报照样送达且恰一次
// 出结果。
AURORA_TEST_CASE(engine_driven_delivery_without_host_pump) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no engine thread to drive delivery");
#else
    EngineGuard guard;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    registry.set_host_driven(false);
    AURORA_TEST_CHECK_EQ(aurora::net::Engine::instance().name(), "poll");

    auto sender = bind_socket(DatagramOptions{});
    auto receiver = bind_socket(DatagramOptions{});
    CountLog sent;
    CountLog arrived;
    std::vector<char> inbox(64);
    receiver.receive(std::span<char>(inbox), receive_counted(arrived));
    sender.send_to(std::span<const char>("engine", 6), bind_pair_endpoint(receiver), send_counted(sent));

    AURORA_TEST_CHECK(wait_until([&]() { return arrived.calls.load() == 1 && sent.calls.load() == 1; }));
    AURORA_TEST_CHECK_EQ(arrived.calls.load(), 1);
    AURORA_TEST_CHECK_EQ(arrived.bytes.load(), 6U);
    AURORA_TEST_CHECK_EQ(sent.bytes.load(), 6U);

    // 无人认领的接收槽不会让引擎空转消费就绪：没有槽就不登记可读关注。
    const std::size_t before = registry.live_count();
    auto idle = bind_socket(DatagramOptions{});
    AURORA_TEST_CHECK_EQ(registry.live_count(), before);
    std::vector<char> box(8);
    idle.receive(std::span<char>(box), receive_counted(arrived));
    AURORA_TEST_CHECK_EQ(registry.live_count(), before + 1);
#endif
}

}  // namespace aurora::test_cases::utest_net_datagram
