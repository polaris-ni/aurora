#pragma once

// ============================================================
// 脚本化回环对端（tests/framework/scripted_peer.h）
// ============================================================
//
// 传输层的故障现场有一半在「对端怎么说话」：什么时候写、写成几段、什么时候停写、什么时候直接收摊。
// 这些事实无法靠真实远端脚本化（CI 无出站，且时序不可复现），也不该靠伪造（伪造的对端测不到内核
// 的那一份行为）。本设施的做法是：**连接是真的**（回环或本机 IPC 上的真实 socket），**对端的说法是
// 脚本**——一条后台线程按脚本收发，用例只决定脚本内容与推进时机。
//
// 与 `framework/fake_engine.h` 的分工：那一份接管**时间**（时限、到期次序），这一份接管**对端字节**。
// 两者合起来才是七类故障的脚本化底座：慢连接 = 真连接 + 时限由用例推进；半关闭 = 真连接 + 对端停写；
// 乱序 = 真连接 + 边界被打乱后重排；超大帧 = 真连接 + 一次写出越限字节。
//
// 三条纪律：
// - **只绑回环 / 本机 IPC**：`Endpoint` 由本设施内部构造（缺省 `127.0.0.1` 端口 0），不存在把流量
//   发出局外的路径——这是「全部网络用例在禁网环境下跑通」的可证形态。
// - **脚本必须在停止位上合作**：脚本收到 `stop` 引用，长睡眠请调 `peer_sleep()`（按片检查停止位），
//   否则析构的 join 会挂在脚本里。
// - **一条连接一个脚本体**：脚本来回只在后台线程执行，不碰用例线程的状态；要回报事实给用例，请写进
//   `accepts()` 计数或用例外部的 `Watch<T>`。
//
// 框架属仓库私有设施，不进 include/、不进 aurora_api.json。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/blocking.h"
#include "aurora/net/detail/async_io.h"

namespace aurora::test {

/// @brief 后台脚本里对「还要不要继续」的合作式停止位。
using PeerStop = std::atomic<bool>;

/// @brief 片长上限：`peer_sleep()` 每片之间检查停止位，故析构的 join 最多等一片。
inline constexpr std::chrono::milliseconds AURORA_PEER_SLEEP_SLICE{10};

/// @brief 分片之间的小间隔：把「一次写完」与「分几次写」在时序上真正区分开。
inline constexpr std::chrono::milliseconds AURORA_PEER_FRAGMENT_GAP{2};

/// @brief 脚本内部的有界睡眠：按片检查停止位，停机请求不会撞上一条长睡眠。
/// @param stop 合作式停止位。
/// @param total 计划睡眠总长。
/// @return 是否睡满了全程（返回 false 表示中途看到停机请求）。
inline auto peer_sleep(const PeerStop &stop, std::chrono::milliseconds total) -> bool {
    for (std::int64_t left = total.count(); left > 0; left -= AURORA_PEER_SLEEP_SLICE.count()) {
        if (stop.load()) {
            return false;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds{std::min<std::int64_t>(left, AURORA_PEER_SLEEP_SLICE.count())});
    }
    return !stop.load();
}

/// @brief 对端脚本：在已接受的连接上按剧本收发。
///
/// 形参是这条连接的只借用语句柄与停止位；脚本可以调 `write_all()` / `read_some()` / `peer_sleep()`，
/// 也可以经 `net::detail::shutdown_handle()` 做单向关停（半关闭）。脚本抛出即视为对端进程崩了，
/// 后台线程吞掉异常并继续接受下一条连接——用例的失败应当由断言给出，而不是由线程异常带走整个 runner。
using PeerScript = std::function<void(aurora::net::blocking::StreamSocket &, PeerStop &)>;

namespace scripts {

/// @brief 接受但不说任何话：无应答代理、卡住的 TLS 握手、连上不回数据的服务器都归这一类。
/// @return 只等停机、永不写字节的脚本。
inline auto silent() -> PeerScript {
    return [](aurora::net::blocking::StreamSocket &, PeerStop &stop) -> void {
        while (!stop.load()) {
            peer_sleep(stop, std::chrono::milliseconds{20});
        }
    };
}

/// @brief 写出给定字节后保持连接（不断开、不再写）：观察「收到一段应答之后的时限」。
/// @param payload 要写出的字节。
/// @return 脚本。
inline auto write_then_hold(std::string payload) -> PeerScript {
    return [payload = std::move(payload)](aurora::net::blocking::StreamSocket &socket, PeerStop &stop) -> void {
        (void)socket.write_all(std::span<const char>{payload});
        while (!stop.load()) {
            peer_sleep(stop, std::chrono::milliseconds{20});
        }
    };
}

/// @brief 按给定片段依次写出（片段之间留一个小间隔）：边界与消息边界错位的「乱序」形态。
/// @param fragments 依次写出的片段。
/// @return 脚本。
inline auto scatter(std::vector<std::string> fragments) -> PeerScript {
    return [fragments = std::move(fragments)](aurora::net::blocking::StreamSocket &socket, PeerStop &) -> void {
        for (const auto &fragment : fragments) {
            if (!socket.is_valid()) {
                return;
            }
            (void)socket.write_all(std::span<const char>{fragment});
            std::this_thread::sleep_for(AURORA_PEER_FRAGMENT_GAP);
        }
    };
}

/// @brief 先读掉若干字节再写出：请求-应答式的假服务器（代理握手、HTTP 应答都走这个形状）。
/// @param read_bytes 计划读取的下限字节数（读到该数或对方收摊即停）。
/// @param payload 随后写出的字节。
/// @return 脚本。
inline auto reply_after_reading(std::size_t read_bytes, std::string payload) -> PeerScript {
    return [read_bytes, payload = std::move(payload)](aurora::net::blocking::StreamSocket &socket,
                                                      PeerStop &stop) -> void {
        (void)socket.set_io_timeout(std::chrono::milliseconds{200});
        std::vector<char> buffer(4096);
        std::size_t seen = 0;
        while (seen < read_bytes && !stop.load()) {
            const auto read = socket.read_some(std::span<char>{buffer});
            if (!read) {
                break;
            }
            if (read.value() == 0) {
                break;  // 对端已停写
            }
            seen += read.value();
        }
        (void)socket.write_all(std::span<const char>{payload});
    };
}

/// @brief 按连接次序换剧本：第 n 条连接（从 0 起）跑 `steps` 的第 n 份，越界后恒用最后一份。
///
/// 重试、重定向到第二个对端、以及「第一次失败第二次成功」这一类判据都需要**同一条监听口给出不同的
/// 说法**；用两份 `ScriptedPeer` 会把端口换成第二个，用例就分不清「重播放到了同一个目标」还是「换目标」。
/// 连接数请读 `ScriptedPeer::accepts()`——本工厂只负责「这一条连接该说什么」。
/// @param steps 依次使用的脚本（为空则一律不说任何话）。
/// @return 脚本。
inline auto per_connection(std::vector<PeerScript> steps) -> PeerScript {
    // 序号是**这条监听口**的状态而非脚本的状态：`PeerScript` 会被复制进后台线程，故计数只能挂在共享位上。
    auto cursor = std::make_shared<std::atomic<std::size_t>>(0);
    return [steps = std::move(steps), cursor](aurora::net::blocking::StreamSocket &socket, PeerStop &stop) -> void {
        if (steps.empty()) {
            return;
        }
        const std::size_t index = cursor->fetch_add(1);
        steps.at(index < steps.size() ? index : steps.size() - 1)(socket, stop);
    };
}

/// @brief 写出给定字节后**单向停写**（连接保持、只关写向）：半关闭判据的对端那一半。
///
/// 用 `detail::shutdown_handle()` 而不是 `close()`：后者带来的是「双向收摊」，测不到「本端读得 0 字节
/// 但仍可继续写」这条被提案 §18-2 点名的判据。
/// @param payload 停写之前最后写出的字节。
/// @return 脚本。
inline auto write_then_half_close(std::string payload) -> PeerScript {
    return [payload = std::move(payload)](aurora::net::blocking::StreamSocket &socket, PeerStop &stop) -> void {
        (void)socket.write_all(std::span<const char>{payload});
        (void)aurora::net::detail::shutdown_handle(socket.native_handle(), aurora::net::ShutdownMode::Write);
        while (!stop.load()) {
            peer_sleep(stop, std::chrono::milliseconds{20});
        }
    };
}

/// @brief 写出给定字节后以 **RST** 收摊（`SO_LINGER` 时限 0 再关）：读侧拿到一次真实的套接口错误。
///
/// 为什么不是「写完直接 `close()`」：后者的结局**不唯一**——FIN 还是 RST 取决于关闭那一刻内核队列的形态，
/// Winsock 在「本端接收缓冲里还压着没被读走的字节」时可把关闭改成异常断开，于是同一份剧本在负载不同的两台
/// 机器上能落到两种结局（实测：同一用例在全绿与 `http-protocol-error` 之间翻脸，故本设施不设那枚剧本）。
/// 要钉住「读侧报一次套接口错误」这条判据——`net-recv-failed` / `net-send-failed` 那一档的唯一来源——
/// 就得把关闭形态本身钉死：时限 0 的 linger 令 `close()` 丢弃待发字节并直接发 RST，两腿同形
/// （`transport.h` 的 `linger` 口径）。双向收摊但走优雅 FIN 的形态由 `write_then_half_close()` 承担。
/// @param payload 断开之前写出的字节。
/// @return 脚本。
inline auto write_then_reset(std::string payload) -> PeerScript {
    return [payload = std::move(payload)](aurora::net::blocking::StreamSocket &socket, PeerStop &) -> void {
        aurora::net::NativeOptions reset{};
        reset.linger = std::chrono::milliseconds{0};  // 0 = 关闭即 RST
        (void)aurora::net::detail::apply_native_options(socket.native_handle(), reset);
        (void)socket.write_all(std::span<const char>{payload});
        socket.close();
    };
}

/// @brief 一次写出越限长度的字节（超大帧 / 越界载荷判据的字节来源）。
/// @param total 写出总字节数（按内核能接受的块大小分次写出，但仍是一次性灌满读侧）。
/// @param filler 填充字节。
/// @return 脚本。
inline auto flood(std::size_t total, char filler = 'x') -> PeerScript {
    return [total, filler](aurora::net::blocking::StreamSocket &socket, PeerStop &stop) -> void {
        const std::size_t chunk = 65536;
        std::vector<char> block(chunk, filler);
        std::size_t sent = 0;
        while (sent < total && !stop.load()) {
            const std::size_t want = std::min(chunk, total - sent);
            const auto written = socket.write_all(std::span<char>{block.data(), want});
            if (!written) {
                return;  // 对端已收摊：本端读侧的越限判定由用例断言
            }
            sent += want;
        }
    };
}

}  // namespace scripts

/// @brief 脚本化回环对端：在回环（或本机 IPC）上监听并按脚本服务连接，析构时停机并 join。
///
/// 一个对象 = 一个「对端」，可服务多条连接（脚本每条各跑一次）。`accepts()` 是这条对端被真正连上的
/// 次数——「代理不可达时绝不回退直连」这类判据要靠它证明**目标侧一条连接都没来**，比「没收到数据」强。
/// @note Thread: 后台一条脚本线程；全部观测口线程安全
/// @note Side-effects: 构造即 bind + listen，析构即 close + join
class ScriptedPeer {
  public:
    /// @brief 在回环上开一个监听口并按脚本服务连接。
    ///
    /// 失败（绑定不上来）时本对象进入无效态：`valid()` 为 false、`port()` 为 0，用例据此先 REQUIRE
    /// 再往下走——环境缺项要报成缺项，不能伪装成一次逻辑失败。
    /// @param script 对端脚本。
    /// @param backlog 挂起连接队列深度。
    explicit ScriptedPeer(PeerScript script, int backlog = 4) : script_(std::move(script)) {
        auto listener = aurora::net::blocking::Listener::listen_loopback(0, backlog);
        if (!listener) {
            return;
        }
        listener_ = std::move(listener.value());
        port_ = listener_->port();
        valid_ = true;
        thread_ = std::thread([this]() -> void { serve(); });
    }

    ScriptedPeer(const ScriptedPeer &) = delete;
    auto operator=(const ScriptedPeer &) -> ScriptedPeer & = delete;
    ScriptedPeer(ScriptedPeer &&) = delete;
    auto operator=(ScriptedPeer &&) -> ScriptedPeer & = delete;

    /// @brief 停机并 join：先请脚本合作退出，再收回监听口。
    ~ScriptedPeer() {
        stop_.store(true);
        if (listener_) {
            listener_->close();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    /// @brief 监听口是否真的开起来了。
    /// @return 绑定成功为 true。
    [[nodiscard]] auto valid() const -> bool { return valid_; }

    /// @brief 实际绑定端口（端口 0 由系统分配后的真值）。
    /// @return 端口；无效态为 0。
    [[nodiscard]] auto port() const -> std::uint16_t { return port_; }

    /// @brief 迄今被接受并跑过脚本的连接数。
    /// @return 计数（脚本仍在执行的连接也已计入）。
    [[nodiscard]] auto accepts() const -> int { return accepts_.load(); }

  private:
    /// @brief 后台线程主体：接受一条、跑一条，直到停机请求。
    auto serve() -> void {
        while (!stop_.load()) {
            if (!listener_) {
                return;
            }
            const auto ready = listener_->wait_readable(std::chrono::milliseconds{20});
            if (!ready) {
                return;  // 监听口已关闭：停机路径
            }
            if (!ready.value()) {
                continue;
            }
            auto peer = listener_->accept();
            if (!peer) {
                continue;
            }
            ++accepts_;
            if (script_) {
                try {
                    script_(peer.value(), stop_);
                }
                // 脚本抛出 = 对端「崩了」。这里刻意吞掉：用例的失败必须由断言给出，让异常穿线程边界
                // 只会把整个 runner 带走，现场反而没了。
                // NOLINTNEXTLINE(bugprone-empty-catch) 见上行说明：静默是本体的设计意图，不是漏处理
                catch (...) {
                }
            }
        }
    }

    PeerScript script_;  ///< 每条连接执行一次的脚本
    std::optional<aurora::net::blocking::Listener> listener_;  ///< 监听口
    std::thread thread_;  ///< 脚本线程
    PeerStop stop_{false};  ///< 合作式停机位
    std::atomic<int> accepts_{0};  ///< 已接受连接数
    std::uint16_t port_ = 0;  ///< 实际端口
    bool valid_ = false;  ///< 监听口是否开起来
};

/// @brief 取一个「端口上根本没有服务」的地址：先真绑一次再立刻关掉。
///
/// 这是代理不可达判据的诚实形态——不编造端口号，而是让内核发过一个端口后收回它。收回之后端口可能
/// 被别的进程抢占（本机极不可能，且抢占者若不监听同样给出「拒连」这一类事实），故判据只断言到
/// 「失败且不回退直连」，不断言具体 OS 错误码。
/// @return 端口号；取不到回环监听口时为 0（用例据此先 REQUIRE 再走）。
inline auto dead_port() -> std::uint16_t {
    auto listener = aurora::net::blocking::Listener::listen_loopback(0, 1);
    if (!listener) {
        return 0;
    }
    const std::uint16_t port = listener.value().port();
    listener.value().close();
    return port;
}

}  // namespace aurora::test
