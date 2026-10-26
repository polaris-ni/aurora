// ws_frame.cpp —— WebSocket 帧编解码与分片重组的实现（口径见 `ws_frame.h` 的文件头）。
#include "ws_frame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "aurora/core/result.h"
#include "aurora/net/detail/random_draw.h"

namespace aurora::net::detail {
namespace {

/// @brief 把一帧的字节追加成一枚 `Result` 失败用的十进制文本（本文件唯一的格式化需求，不引 `<sstream>`）。
/// @param value 待写的无符号数。
/// @return 十进制文本。
[[nodiscard]] auto decimal(std::uint64_t value) -> std::string { return std::to_string(value); }

/// @brief 是否为控制帧操作码：`RFC 6455` §5.2 把 `opcode` 的最高位为 1 者定义为控制帧。
/// @param opcode 操作码。
/// @return 是控制帧为 true。
[[nodiscard]] auto is_control_opcode(WsOpcode opcode) -> bool {
    return (static_cast<std::uint8_t>(opcode) & 0x8U) != 0;
}

/// @brief 线上 4 位操作码折成枚举。
///
/// 保留段（`0x3`~`0x7`、`0xB`~`0xF`）解出即协议违例，故这里交出空而不是硬造一枚枚举值：让调用方「拿到空即报错」
/// 比让非法取值活在枚举里更难漏判。
/// @param raw 帧头里的低 4 位。
/// @return 合法操作码；保留段为空。
[[nodiscard]] auto opcode_from_raw(std::uint8_t raw) -> std::optional<WsOpcode> {
    switch (raw) {
        case 0x0:
            return WsOpcode::Continuation;
        case 0x1:
            return WsOpcode::Text;
        case 0x2:
            return WsOpcode::Binary;
        case 0x8:
            return WsOpcode::Close;
        case 0x9:
            return WsOpcode::Ping;
        case 0xA:
            return WsOpcode::Pong;
        default:
            return std::nullopt;
    }
}

/// @brief 协议违例（终态）。
/// @param detail 点名违的是哪一条（ASCII，进 `ws-protocol-error` 的 `{detail}` 槽）。
/// @return 待记录的错误。
[[nodiscard]] auto protocol_error(std::string_view detail) -> Error {
    return make_error(ErrorCode::WsProtocolError, ErrorParams{{"detail", std::string{detail}}});
}

/// @brief 单枚续字节是否长成 `80..BF`。
/// @param unit 字节。
/// @return 是续字节为 true。
[[nodiscard]] auto is_utf8_continuation(std::uint8_t unit) -> bool { return unit >= 0x80 && unit <= 0xBF; }

/// @brief 把一个 `uint64` 按大端写进串尾（长度前缀的 16 位与 64 位档共用）。
/// @param out 目标串。
/// @param value 待写的值。
/// @param width 写出的字节数（2 或 8）。
auto append_be(std::string &out, std::uint64_t value, std::size_t width) -> void {
    for (std::size_t step = width; step > 0; --step) {
        const auto shift = static_cast<unsigned>((step - 1U) * 8U);
        out.push_back(static_cast<char>((value >> shift) & 0xFFU));
    }
}

}  // namespace

// UTF-8 的判定住在 `detail` 而不是匿名空间：发出侧（`net/websocket.cpp` 的文本闸）与读本侧共用同一份实现，
// 两处各写一遍迟早会在「代理对区」这种边角上分岔。定义放在匿名空间之外，`is_utf8_continuation` 仍可见。
auto first_invalid_utf8(std::string_view bytes) -> std::optional<std::size_t> {
    const auto at = [&bytes](std::size_t index) { return static_cast<std::uint8_t>(bytes[index]); };
    std::size_t index = 0;
    while (index < bytes.size()) {
        const std::uint8_t lead = at(index);
        if (lead <= 0x7FU) {
            ++index;
            continue;
        }
        if (lead >= 0xC2U && lead <= 0xDFU) {
            if (bytes.size() - index < 2 || !is_utf8_continuation(at(index + 1))) {
                return index;
            }
            index += 2;
            continue;
        }
        if (lead >= 0xE0U && lead <= 0xEFU) {
            if (bytes.size() - index < 3) {
                return index;
            }
            const std::uint8_t second = at(index + 1);
            const bool second_ok = (lead == 0xE0U)
                                       ? (second >= 0xA0U && second <= 0xBFU)
                                       : ((lead == 0xEDU) ? (second <= 0x9FU && is_utf8_continuation(second))
                                                          : is_utf8_continuation(second));
            if (!second_ok || !is_utf8_continuation(at(index + 2))) {
                return index;
            }
            index += 3;
            continue;
        }
        if (lead >= 0xF0U && lead <= 0xF4U) {
            if (bytes.size() - index < 4) {
                return index;
            }
            const std::uint8_t second = at(index + 1);
            const bool second_ok = (lead == 0xF0U)
                                       ? (second >= 0x90U && second <= 0xBFU)
                                       : ((lead == 0xF4U) ? (second <= 0x8FU && is_utf8_continuation(second))
                                                          : is_utf8_continuation(second));
            if (!second_ok || !is_utf8_continuation(at(index + 2)) || !is_utf8_continuation(at(index + 3))) {
                return index;
            }
            index += 4;
            continue;
        }
        return index;  // 落单续字节（80..BF）、越码 C0/C1、F5+ 与上面两档没接住的形态
    }
    return std::nullopt;
}

auto is_close_code_on_wire(std::uint16_t code) -> bool {
    if (code >= 3000 && code <= 4999) {
        return true;  // 3000~3999 留给扩展、4000~4999 留给私有使用：两段都算上线合法
    }
    if (code >= 1000 && code <= 1003) {
        return true;
    }
    return code >= 1007 && code <= 1014;
}

WsFrameReader::WsFrameReader(WsFrameSinks sinks, WsFrameLimits limits) : sinks_(std::move(sinks)), limits_(limits) {}

WsFrameReader::WsFrameReader(WsFrameReader &&other) noexcept
    : sinks_(std::move(other.sinks_)), limits_(other.limits_), pending_(std::move(other.pending_)),
      message_(std::move(other.message_)), fragment_opcode_(other.fragment_opcode_),
      failure_(std::move(other.failure_)), fragment_open_(other.fragment_open_), paused_(other.paused_) {
    other.fragment_open_ = false;
}

auto WsFrameReader::operator=(WsFrameReader &&other) noexcept -> WsFrameReader & {
    if (this == &other) {
        return *this;
    }
    sinks_ = std::move(other.sinks_);
    limits_ = other.limits_;
    pending_ = std::move(other.pending_);
    message_ = std::move(other.message_);
    fragment_opcode_ = other.fragment_opcode_;
    failure_ = std::move(other.failure_);
    fragment_open_ = other.fragment_open_;
    paused_ = other.paused_;
    other.fragment_open_ = false;
    return *this;
}

WsFrameReader::~WsFrameReader() = default;

auto WsFrameReader::push(std::string_view bytes) -> Result<void> {
    if (failure_.has_value()) {
        return Result<void>{*failure_};
    }
    pending_.append(bytes);
    std::size_t offset = 0;
    while (pending_.size() - offset >= 2) {
        const auto first = static_cast<std::uint8_t>(pending_[offset]);
        const auto second = static_cast<std::uint8_t>(pending_[offset + 1]);
        const bool final = (first & 0x80U) != 0;
        const auto raw_opcode = static_cast<std::uint8_t>(first & 0x0FU);
        // 判定顺序刻意是「先看完前两枚字节能不能定这条帧的生死，再等长度字节」：形态违例与越限都不需要载荷到场，
        // 而等字节到场再判意味着对端可以只用一枚声明就把本层的缓冲推到上限。
        if ((first & 0x70U) != 0) {
            return fail(protocol_error("frame header sets the reserved bits, but no extension was negotiated"));
        }
        const auto opcode = opcode_from_raw(raw_opcode);
        if (!opcode.has_value()) {
            return fail(protocol_error("frame header carries a reserved opcode " + decimal(raw_opcode) +
                                       " (RFC 6455 leaves it undefined)"));
        }
        if ((second & 0x80U) != 0) {
            return fail(protocol_error("the peer masked a frame: masking is this side's job, not the server's"));
        }
        const auto short_length = static_cast<std::uint8_t>(second & 0x7FU);
        const bool control = is_control_opcode(*opcode);
        if (control && !final) {
            return fail(
                protocol_error("a control frame is fragmented (RFC 6455 says control frames are never fragmented)"));
        }
        std::size_t header_bytes = 2;
        std::uint64_t payload_length = short_length;
        if (short_length == 126) {
            if (pending_.size() - offset < header_bytes + 2) {
                break;  // 16 位长度还没到齐
            }
            payload_length = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(pending_[offset + 2])) << 8U) |
                             static_cast<std::uint8_t>(pending_[offset + 3]);
            header_bytes += 2;
            if (payload_length < 126) {
                return fail(protocol_error("a 16-bit length field carries a value that fits the 7-bit form"));
            }
        } else if (short_length == 127) {
            if (pending_.size() - offset < header_bytes + 8) {
                break;  // 64 位长度还没到齐
            }
            if (static_cast<std::uint8_t>(pending_[offset + 2]) >= 0x80U) {
                return fail(protocol_error("the 64-bit length field has its most significant bit set"));
            }
            payload_length = 0;
            for (std::size_t step = 0; step < 8; ++step) {
                payload_length = (payload_length << 8U) | static_cast<std::uint8_t>(pending_[offset + 2 + step]);
            }
            header_bytes += 8;
            if (payload_length <= 0xFFFFULL) {
                return fail(protocol_error("a 64-bit length field carries a value that fits the 16-bit form"));
            }
        }
        if (control && payload_length > AURORA_WS_MAX_CONTROL_BYTES) {
            return fail(protocol_error("control frame payload of " + decimal(payload_length) +
                                       " bytes exceeds the protocol limit of 125"));
        }
        if (payload_length > limits_.max_frame_bytes) {
            return fail(protocol_error("frame size " + decimal(payload_length) +
                                       " bytes exceeds this connection's frame limit of " +
                                       decimal(limits_.max_frame_bytes)));
        }
        const std::size_t frame_bytes = header_bytes + static_cast<std::size_t>(payload_length);
        if (pending_.size() - offset < frame_bytes) {
            break;  // 载荷还没到齐：声明长度已核过，本层的等待因此有界
        }
        const std::string_view payload =
            std::string_view{pending_}.substr(offset + header_bytes, static_cast<std::size_t>(payload_length));
        offset += frame_bytes;
        if (auto consumed = consume_frame(final, *opcode, payload); !consumed) {
            pending_.erase(0, offset);
            return consumed;
        }
    }
    pending_.erase(0, offset);
    return Result<void>{};
}

auto WsFrameReader::finish() -> Result<void> {
    if (failure_.has_value()) {
        return Result<void>{*failure_};
    }
    pending_.clear();
    message_.clear();
    fragment_opcode_.reset();
    fragment_open_ = false;
    return Result<void>{};
}

auto WsFrameReader::buffered_bytes() const -> std::size_t { return pending_.size() + message_.size(); }

auto WsFrameReader::consume_frame(bool final, WsOpcode opcode, std::string_view payload) -> Result<void> {
    switch (opcode) {
        case WsOpcode::Continuation:
            if (!fragment_open_) {
                return fail(protocol_error("a continuation frame arrived with no fragmented message open"));
            }
            if (message_.size() + payload.size() > limits_.max_message_bytes) {
                return fail(protocol_error("reassembled message exceeds this connection's message limit of " +
                                           decimal(limits_.max_message_bytes) + " bytes"));
            }
            message_.append(payload);
            if (!final) {
                return Result<void>{};
            }
            {
                const auto kind = fragment_opcode_.value_or(WsOpcode::Binary);
                std::string joined = std::move(message_);
                message_.clear();
                fragment_open_ = false;
                fragment_opcode_.reset();
                return dispatch(kind, std::move(joined));
            }
        case WsOpcode::Text:
        case WsOpcode::Binary:
            if (fragment_open_) {
                return fail(protocol_error(
                    "a text or binary frame arrived while a fragmented message is still open on this connection"));
            }
            if (payload.size() > limits_.max_message_bytes) {
                return fail(protocol_error("reassembled message exceeds this connection's message limit of " +
                                           decimal(limits_.max_message_bytes) + " bytes"));
            }
            if (final) {
                return dispatch(opcode, std::string{payload});
            }
            fragment_open_ = true;
            fragment_opcode_ = opcode;
            message_.assign(payload);
            return Result<void>{};
        case WsOpcode::Ping:
            if (sinks_.on_ping) {
                sinks_.on_ping(payload);
            }
            return Result<void>{};
        case WsOpcode::Pong:
            if (sinks_.on_pong) {
                sinks_.on_pong(payload);
            }
            return Result<void>{};
        case WsOpcode::Close: {
            if (payload.size() == 1) {
                return fail(
                    protocol_error("a close frame carries one payload byte: a status code is two bytes, not one"));
            }
            WsParsedClose close;
            if (payload.size() >= 2) {
                const auto high = static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[0]));
                const auto low = static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[1]));
                close.code = static_cast<std::uint16_t>((high << 8U) | low);
                if (!is_close_code_on_wire(*close.code)) {
                    return fail(protocol_error("close status code " + decimal(*close.code) +
                                               " is not valid on the wire (it is reserved, or it names a "
                                               "closure that only an implementation can know)"));
                }
                close.reason.assign(payload.substr(2));
            }
            if (const auto invalid = first_invalid_utf8(close.reason); invalid.has_value()) {
                return fail(protocol_error("the close reason is not valid UTF-8 at byte " +
                                           decimal(static_cast<std::uint64_t>(*invalid))));
            }
            if (sinks_.on_close) {
                sinks_.on_close(std::move(close));
            }
            return Result<void>{};
        }
    }
    return Result<void>{};
}

auto WsFrameReader::dispatch(WsOpcode opcode, std::string payload) -> Result<void> {
    if (opcode == WsOpcode::Text) {
        // 文本的 UTF-8 判定在整条消息上做：一条分片消息可以把一个多字节序列劈在两片的交界处。
        if (const auto invalid = first_invalid_utf8(payload); invalid.has_value()) {
            return fail(protocol_error("a text message is not valid UTF-8 at byte " +
                                       decimal(static_cast<std::uint64_t>(*invalid)) + " of " +
                                       decimal(static_cast<std::uint64_t>(payload.size()))));
        }
    }
    if (!sinks_.on_message || paused_) {
        return Result<void>{};  // 宿主已收摊：形态照判（终态不受 Appetite 影响），只是不再派发消息
    }
    if (!sinks_.on_message(WsParsedMessage{.opcode = opcode, .payload = std::move(payload)})) {
        paused_ = true;
    }
    return Result<void>{};
}

auto WsFrameReader::fail(Error error) -> Result<void> {
    failure_ = std::move(error);
    return Result<void>{*failure_};
}

auto close_frame_payload(std::uint16_t code, std::string_view reason) -> std::string {
    std::string out;
    out.reserve(reason.size() + 2);
    append_be(out, code, 2);
    out.append(reason);
    return out;
}

auto write_ws_frame(std::string &out, WsOpcode opcode, bool final, std::string_view payload,
                    std::optional<std::array<std::byte, AURORA_WS_MASK_KEY_BYTES>> key) -> Result<void> {
    const bool control = is_control_opcode(opcode);
    if (control && payload.size() > AURORA_WS_MAX_CONTROL_BYTES) {
        // 这两条拒的是**我方**要写的帧：宁可这次发不出去，也不写出一枚自己不认的字节让对端去判违例。
        return protocol_error("refusing to write a control frame whose payload is " +
                              decimal(static_cast<std::uint64_t>(payload.size())) + " bytes (the protocol allows 125)");
    }
    if (control && !final) {
        return protocol_error("refusing to write a fragmented control frame");
    }
    std::array<std::byte, AURORA_WS_MASK_KEY_BYTES> drawn{};
    if (key.has_value()) {
        drawn = *key;
    } else {
        // 掩码密钥是协议要求的不可预测值，取不到熵即交出失败：回落伪随机等于把「这一帧看着掩了、其实可猜」
        // 留给对端，与本库「不提供假降级」那条纪律正相反（口径见 §8.8.27 的两档分界）。
        if (const auto filled = random_fill(drawn); !filled) {
            return filled.error();
        }
    }
    const auto length = static_cast<std::uint64_t>(payload.size());
    const auto lead = static_cast<std::uint8_t>(final ? (0x80U | static_cast<std::uint8_t>(opcode))
                                                      : static_cast<std::uint8_t>(opcode));
    out.push_back(static_cast<char>(lead));
    if (length < 126) {
        out.push_back(static_cast<char>(0x80U | static_cast<unsigned>(length)));
    } else if (length <= 0xFFFFULL) {
        out.push_back(static_cast<char>(0x80U | 126U));
        append_be(out, length, 2);
    } else {
        out.push_back(static_cast<char>(0x80U | 127U));
        append_be(out, length, 8);
    }
    // 密钥属于帧头而不属于载荷：零长载荷照样写这四枚（省掉它会让对端把载荷的头四枚读成密钥）。
    for (const std::byte unit : drawn) {
        out.push_back(static_cast<char>(unit));
    }
    for (std::size_t index = 0; index < payload.size(); ++index) {
        const auto data = static_cast<std::uint8_t>(payload[index]);
        // 取模后的下标恒小于密钥长度（那枚常量就是数组长度）：逐字节的掩码路径不做运行时判定。
        // NOLINTNEXTLINE(*-pro-bounds-constant-array-index)
        const auto mask = static_cast<std::uint8_t>(drawn[index % AURORA_WS_MASK_KEY_BYTES]);
        out.push_back(static_cast<char>(data ^ mask));
    }
    return Result<void>{};
}

}  // namespace aurora::net::detail
