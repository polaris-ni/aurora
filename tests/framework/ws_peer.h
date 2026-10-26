#pragma once

// ============================================================
// 脚本化 WebSocket 对端（tests/framework/ws_peer.h）
// ============================================================
//
// 本设施把 `RFC 6455` 客户端那一侧的**对端**补齐：读请求里的 `Sec-WebSocket-Key`、算出摘要、答一份 101，
// 然后按剧本把服务端帧写上线——必要时反过来把我方（客户端）帧逐枚解码并记进交接缓冲。握手腿与会话腿的
// 全部判据都长在这份字节之上，而字节本身不属于被测对象，于是它住在这里而不是某个用例文件里。
//
// 与另两份对端设施的分工：
// - `scripted_peer.h` 接管**明文 HTTP** 对端字节（状态行、分帧、代理握手），它不知道 WebSocket 为何物。
// - `tls_peer.h` 接管 **TLS 层**对端（真实 `openssl` 服务端、证书、记录），它不碰应用层帧。
// - 这一份接管**升级之后**的帧流：它自己就用 `ScriptedPeer` 起监听口，只把脚本形态换成 WebSocket 剧本。
//
// 四条纪律（其中两条是被实测教出来的，不是设计偏好）：
// - **服务端方向一律不带掩码**：掩码是 `RFC 6455 §5.1` 给客户端方向的义务，对端帧带掩码在我方分帧器里
//   是协议违例而不是「更严格」。测试自己拼帧，故本头的操作码常量与库内那枚枚举各走各路。
// - **一个剧本一条监听口**：`ScriptedPeer` 的脚本线程**串行**服务连接，而握手成功的剧本必须把连接一直持有
//   到用例收摊（否则 `StreamSocket` 随脚本作用域销毁，会话当场断流）。让位另一条路是「答完就返回」，但那会在
//   本端接收缓冲还压着那枚应答时收掉连接——Winsock 可把它改成 RST，字节先丢（`scripted_peer.h` 文件头记的
//   正是这条实测口径）。
// - **长眠让位**：脚本退出前恒在 `stop` 位上合作，`ScriptedPeer` 析构的 join 因此有界。
// - **停写与停机分家**：`stop_writing_after_frames` 走 `shutdown_handle(..., Write)` 而非 `close()`，
//   那条连接仍可被我方继续用到收摊——EOF 一族判据要的就是「对端不说了但链路还在」。
//
// 框架属仓库私有设施，不进 include/、不进 aurora_api.json。

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/net/blocking.h"
#include "aurora/net/detail/async_io.h"
#include "aurora/net/detail/ws_handshake.h"
#include "aurora/net/transport.h"
#include "framework/aurora_test.h"
#include "framework/scripted_peer.h"

namespace aurora::test::ws {

using aurora::net::ShutdownMode;
using aurora::net::blocking::StreamSocket;
using aurora::net::detail::shutdown_handle;
using aurora::net::detail::ws_accept_token;
using aurora::test::peer_sleep;
using aurora::test::PeerScript;
using aurora::test::PeerStop;
using aurora::test::ScriptedPeer;

/// @brief 服务端→客户端方向的操作码（测试自己拼帧，故不用库内那枚枚举的去向）。
constexpr std::uint8_t AURORA_OP_CONTINUATION = 0x0U;
constexpr std::uint8_t AURORA_OP_TEXT = 0x1U;
constexpr std::uint8_t AURORA_OP_BINARY = 0x2U;
constexpr std::uint8_t AURORA_OP_CLOSE = 0x8U;
constexpr std::uint8_t AURORA_OP_PING = 0x9U;
constexpr std::uint8_t AURORA_OP_PONG = 0xAU;

/// @brief 成功升级的应答头部块：`@accept@` 由脚本换成对本次请求 key 算出的摘要。
constexpr char AURORA_UPGRADE_REPLY[] =
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Accept: @accept@\r\n"
    "\r\n";

/// @brief 观察阶段单次读的超时：够长以免把「对端还没说」读成错误，够短以保证 `ScriptedPeer` 的 join 不挂。
constexpr std::chrono::milliseconds AURORA_OBSERVE_IO_POLL{50};

/// @brief 拼一个指向回环端口的 `ws://` 目标。
/// @param port 对端端口。
/// @param path 路径。
/// @return 绝对 URL（数字字面量，零 DNS）。
[[nodiscard]] inline auto loopback_ws_url(std::uint16_t port, std::string_view path = "/chat") -> std::string {
    return "ws://127.0.0.1:" + std::to_string(port) + std::string{path};
}

/// @brief 整段转小写（字段名比较大小写不敏感，`RFC 9110 §5.1`）。
[[nodiscard]] inline auto to_lower(std::string_view text) -> std::string {
    std::string out{text};
    std::ranges::transform(out, out.begin(), [](unsigned char unit) { return static_cast<char>(std::tolower(unit)); });
    return out;
}

/// @brief 从头部块里取一枚字段的首个值（大小写不敏感；未命中交空串）。
[[nodiscard]] inline auto field_value(std::string_view head, std::string_view name) -> std::string {
    const std::string haystack = to_lower(head);
    const std::string needle = to_lower(name) + ":";
    const auto at = haystack.find(needle);
    if (at == std::string::npos) {
        return {};
    }
    // 位置取自小写副本，而 to_lower 逐字映射、两份等长，故可拿来切原文；substr 而非指针加减。
    const std::string_view rest = head.substr(at + needle.size());
    const auto line = rest.find("\r\n");
    std::string value{rest.substr(0, line == std::string_view::npos ? rest.size() : line)};
    const auto first = value.find_first_not_of(" \t");
    value = first == std::string::npos ? std::string{} : value.substr(first);
    const auto last = value.find_last_not_of(" \t");
    return last == std::string::npos ? value : value.substr(0, last + 1U);
}

/// @brief 头部块里某枚字段出现几次（「覆写而非并列」的判据就靠它）。
[[nodiscard]] inline auto field_count(std::string_view head, std::string_view name) -> std::size_t {
    const std::string haystack = to_lower(head);
    const std::string needle = to_lower(name) + ":";
    std::size_t hits = 0;
    for (auto at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + needle.size())) {
        ++hits;
    }
    return hits;
}

/// @brief 把一次握手请求里的 `Sec-WebSocket-Key` 取出来（应答那枚摘要由它算）。
[[nodiscard]] inline auto key_of(std::string_view head) -> std::string {
    return field_value(head, "Sec-WebSocket-Key");
}

/// @brief 替换文本里的第一处占位（找不到即原样交出）。
[[nodiscard]] inline auto substitute(std::string_view text, std::string_view needle, std::string_view value)
    -> std::string {
    const auto at = text.find(needle);
    if (at == std::string_view::npos) {
        return std::string{text};
    }
    return std::string{text.substr(0, at)} + std::string{value} + std::string{text.substr(at + needle.size())};
}

/// @brief 拼一枚服务端帧（**不带掩码**——`RFC 6455 §5.1` 把掩码定为客户端方向，带掩码的对端帧是违例）。
[[nodiscard]] inline auto server_frame(std::uint8_t opcode, std::string_view payload, bool final = true)
    -> std::string {
    std::string out;
    out.push_back(static_cast<char>((final ? 0x80U : 0x00U) | opcode));
    if (payload.size() < 126U) {
        out.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 65535U) {
        out.push_back(static_cast<char>(126U));
        out.push_back(static_cast<char>(payload.size() >> 8U));
        out.push_back(static_cast<char>(payload.size() & 0xFFU));
    } else {
        out.push_back(static_cast<char>(127U));
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> shift) & 0xFFU));
        }
    }
    out.append(payload);
    return out;
}

/// @brief 拼一枚 close 帧的载荷：两字节大端状态码（+ 可选原因）。
[[nodiscard]] inline auto close_payload(std::uint16_t code, std::string_view reason = {}) -> std::string {
    std::string out;
    out.push_back(static_cast<char>(code >> 8U));
    out.push_back(static_cast<char>(code & 0xFFU));
    out.append(reason);
    return out;
}

/// @brief 从 close 载荷里读状态码；不足两字节即「没带码」。
[[nodiscard]] inline auto close_code_of(std::string_view payload) -> int {
    if (payload.size() < 2U) {
        return -1;
    }
    const auto high = static_cast<unsigned char>(payload[0U]);
    const auto low = static_cast<unsigned char>(payload[1U]);
    return static_cast<int>((static_cast<std::uint16_t>(high) * 256U) + static_cast<std::uint16_t>(low));
}

/// @brief 我方（客户端）帧的解码结果：读侧观察的唯一形态。
struct SeenFrame {
    bool final{false};  ///< FIN 位
    std::uint8_t opcode{0};  ///< 操作码
    bool masked{false};  ///< 掩码位（我方帧必须为真）
    std::string payload;  ///< 已解掩的载荷
};

/// @brief 从累积缓冲头部取出一枚完整的客户端帧；凑不齐即留着等下一段。
/// @param buffer 累积的线上字节（取出一枚就把那一段擦掉）。
/// @return 解出的帧；字节还不够一枚帧即空。
[[nodiscard]] inline auto take_client_frame(std::string &buffer) -> std::optional<SeenFrame> {
    if (buffer.size() < 2U) {
        return std::nullopt;
    }
    const auto first = static_cast<unsigned char>(buffer[0U]);
    const auto second = static_cast<unsigned char>(buffer[1U]);
    std::size_t offset = 2U;
    std::size_t length = second & 0x7FU;
    if (length == 126U) {
        if (buffer.size() < offset + 2U) {
            return std::nullopt;
        }
        length = (static_cast<std::size_t>(static_cast<unsigned char>(buffer[offset])) * 256U) +
                 static_cast<std::size_t>(static_cast<unsigned char>(buffer[offset + 1U]));
        offset += 2U;
    } else if (length == 127U) {
        if (buffer.size() < offset + 8U) {
            return std::nullopt;
        }
        length = 0U;
        for (std::size_t index = 0U; index < 8U; ++index) {
            length = (length * 256U) + static_cast<std::size_t>(static_cast<unsigned char>(buffer[offset + index]));
        }
        offset += 8U;
    }
    const bool masked = (second & 0x80U) != 0U;
    std::array<unsigned char, 4> key{};
    if (masked) {
        if (buffer.size() < offset + 4U) {
            return std::nullopt;
        }
        for (std::size_t index = 0U; index < key.size(); ++index) {
            key.at(index) = static_cast<unsigned char>(buffer[offset + index]);
        }
        offset += 4U;
    }
    if (buffer.size() < offset + length) {
        return std::nullopt;
    }
    SeenFrame seen;
    seen.final = (first & 0x80U) != 0U;
    seen.opcode = static_cast<std::uint8_t>(first & 0x0FU);
    seen.masked = masked;
    seen.payload.reserve(length);
    for (std::size_t index = 0U; index < length; ++index) {
        const auto unit = static_cast<unsigned char>(buffer[offset + index]);
        seen.payload.push_back(static_cast<char>(masked ? unit ^ key.at(index % 4U) : unit));
    }
    buffer.erase(0U, offset + length);
    return seen;
}

/// @brief 对端与用例之间的交接缓冲：脚本线程写、用例线程读，故一把锁护住两份记录。
///
/// 成员公开会让断言少写一层，但那份锁不能被绕过——观测口全部按值交出副本，用例拿到的是快照。
class WsLog {
  public:
    /// @brief 记下一条连接的请求头部块。
    /// @param head 头部块原文。
    auto record_head(std::string head) -> void {
        const std::scoped_lock held(guard_);
        heads_.push_back(std::move(head));
    }

    /// @brief 记下从这条连接上解出的一枚我方帧。
    /// @param frame 解码结果。
    auto record_frame(SeenFrame frame) -> void {
        const std::scoped_lock held(guard_);
        frames_.push_back(std::move(frame));
    }

    /// @brief 第 index 条连接的请求头部块（不存在即空串）。
    [[nodiscard]] auto head(std::size_t index) const -> std::string {
        const std::scoped_lock held(guard_);
        return index < heads_.size() ? heads_[index] : std::string{};
    }

    /// @brief 迄今解出的我方帧（快照）。
    [[nodiscard]] auto frames() const -> std::vector<SeenFrame> {
        const std::scoped_lock held(guard_);
        return frames_;
    }

    /// @brief 第 index 枚我方帧。
    [[nodiscard]] auto frame(std::size_t index) const -> std::optional<SeenFrame> {
        const std::scoped_lock held(guard_);
        return index < frames_.size() ? std::optional<SeenFrame>{frames_[index]} : std::nullopt;
    }

    /// @brief 某种操作码的我方帧枚数。
    [[nodiscard]] auto count(std::uint8_t opcode) const -> std::size_t {
        const std::scoped_lock held(guard_);
        return static_cast<std::size_t>(
            std::ranges::count(frames_, opcode, [](const SeenFrame &frame) { return frame.opcode; }));
    }

  private:
    mutable std::mutex guard_;  ///< 保护下方两份记录
    std::vector<std::string> heads_;  ///< 逐条连接的请求头部块
    std::vector<SeenFrame> frames_;  ///< 逐枚解出的我方帧
};

/// @brief 一条回环服务的剧本：怎么答握手、答完之后说什么、要不要听我方说话。
struct WsPlan {
    std::string reply{AURORA_UPGRADE_REPLY};  ///< 应答头部块（`@accept@` 占位换成本次 key 的摘要）
    std::vector<std::string> frames;  ///< 应答之后交出的服务端帧
    bool glue_frames{false};  ///< 与应答同一次写出（验「头部块之后的字节属于帧流」）
    bool watch_client_frames{false};  ///< 持续读我方帧并记进交接缓冲
    bool answer_ping{false};  ///< 见到我方 ping 回一枚同载荷 pong
    bool answer_close{false};  ///< 见到我方 close 回一枚 1000
    bool release_frames_on_request{false};  ///< 等我方第一枚帧到场才交出 `frames`（握手已交出句柄的确定性时序）
    bool stop_writing_after_frames{false};  ///< 交出 frames 之后单向停写（EOF 判据）
};

/// @brief 按剧本服务一条连接。
/// @param plan 剧本。
/// @param log 交接缓冲（用例持有，生命周期覆盖 `ScriptedPeer`）。
/// @return 对端脚本。
[[nodiscard]] inline auto serve_ws(WsPlan plan, WsLog &log) -> PeerScript {
    return [plan = std::move(plan), &log](StreamSocket &socket, PeerStop &stop) -> void {
        // 单向停写：EOF 一族判据的字节来源。只关写向（不是 `close()`），那条连接仍可被我方继续用到收摊。
        const auto half_close = [&socket, &plan]() -> void {
            if (plan.stop_writing_after_frames) {
                (void)shutdown_handle(socket.native_handle(), ShutdownMode::Write);
            }
        };
        const auto hold = [&stop]() -> void {
            while (!stop.load()) {
                peer_sleep(stop, std::chrono::milliseconds{20});
            }
        };
        (void)socket.set_io_timeout(std::chrono::seconds{10});
        std::vector<char> scratch(4096);
        std::string request;
        while (request.find("\r\n\r\n") == std::string::npos) {
            const auto read = socket.read_some(std::span<char>{scratch});
            if (!read || read.value() == 0U) {
                break;  // 请求不会更完整了：按手上的字节应答
            }
            request.append(scratch.data(), read.value());
        }
        log.record_head(request);
        std::string reply = substitute(plan.reply, "@accept@", ws_accept_token(key_of(request)));
        std::string trailing;
        for (const std::string &frame : plan.frames) {
            if (plan.glue_frames) {
                reply += frame;
            } else {
                trailing += frame;
            }
        }
        const bool observes = plan.watch_client_frames;
        if (!observes) {
            reply += trailing;
            trailing.clear();
        }
        (void)socket.write_all(std::span<const char>{reply});
        if (!observes) {
            half_close();
            hold();
            return;
        }
        // 「等我方开口再说法」的闸门：交付回调里销毁句柄、`SinkFlow::Stop` 之后还会不会发那一族判据，
        // 要的是「句柄已经交到宿主手上」这条确定时序——应答之后立刻写的字节往往在同一次读里就到齐了。
        const bool deferred = plan.release_frames_on_request;
        if (!deferred && !trailing.empty()) {
            (void)socket.write_all(std::span<const char>{trailing});
            trailing.clear();
            half_close();
        }
        (void)socket.set_io_timeout(AURORA_OBSERVE_IO_POLL);
        std::string buffer;
        std::size_t seen_frames = 0;
        while (!stop.load()) {
            const auto read = socket.read_some(std::span<char>{scratch});
            if (!read) {
                continue;  // 纯超时：这条连接还活着，只是我方暂时没说话
            }
            if (read.value() == 0U) {
                break;  // 对端收摊：不再观察，但连接留到停机（回声要在它还在的时候写完）
            }
            buffer.append(scratch.data(), read.value());
            while (const auto seen = take_client_frame(buffer)) {
                log.record_frame(*seen);
                ++seen_frames;
                if (plan.answer_ping && seen->opcode == AURORA_OP_PING) {
                    (void)socket.write_all(std::span<const char>{server_frame(AURORA_OP_PONG, seen->payload)});
                }
                if (plan.answer_close && seen->opcode == AURORA_OP_CLOSE) {
                    (void)socket.write_all(std::span<const char>{server_frame(AURORA_OP_CLOSE, close_payload(1000U))});
                }
            }
            if (deferred && !trailing.empty() && seen_frames > 0U) {
                (void)socket.write_all(std::span<const char>{trailing});
                trailing.clear();
                half_close();
            }
        }
        hold();
    };
}

/// @brief 一次开若干条回环对端，逐条按剧本服务。
///
/// 为什么一个剧本一条监听口见文件头第二条纪律。
/// @param plans 每条连接的剧本（按给出次序各开一条监听口）。
/// @param log 交接缓冲（用例持有，生命周期覆盖返回的监听口集合）。
/// @return 监听口集合；每条都已确认可用。
[[nodiscard]] inline auto make_peers(std::initializer_list<WsPlan> plans, WsLog &log)
    -> std::vector<std::unique_ptr<ScriptedPeer>> {
    std::vector<std::unique_ptr<ScriptedPeer>> peers;
    for (const auto &plan : plans) {
        auto peer = std::make_unique<ScriptedPeer>(serve_ws(plan, log));
        AURORA_TEST_REQUIRE_MSG(peer->valid(), "the loopback listener could not be opened");
        peers.push_back(std::move(peer));
    }
    return peers;
}

}  // namespace aurora::test::ws
