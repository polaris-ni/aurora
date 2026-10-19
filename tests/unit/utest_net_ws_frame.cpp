/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/ws_frame.h + src/aurora/net/detail/ws_frame.cpp
/// 测试说明: WebSocket 帧层的离线回归（纯字节、零 socket、零时钟、零线程），读侧与写侧各一层。
///           **读侧**验只有增量解帧才成立的口径：一条帧横跨任意多段也仍是一条（含逐字节喂入与「后半帧留在
///           缓冲里等下一段」）、分片按序拼接且类型取自头帧、控制帧可以插在两片之间且不 Join 进消息、
///           `continuation` 无头帧即拒、头帧未说完又来数据帧即拒、对端带掩码即拒（掩码是我方的义务）、
///           保留位与保留操作码即拒、控制帧被分片或载荷超过 125 字节即拒、非最少的长度前缀（16 位档装得下
///           却用 64 位档等）即拒、64 位档最高位置起即拒、**单帧越限在声明长度一到就判**（不等那 MiB 到场，
///           这条把「越限」与「内存先到上限」分开钉）、越总帧的 detail 点名的是重组量而越单帧点名帧长、
///           文本的 UTF-8 在整条消息上判（一个多字节序列被劈在两片的交界处仍算合法），二进制不判、
///           `close` 的三型载荷（0 字节 = 没带码、2 字节 = 只有码、≥3 = 码 + 原因）、1 字节载荷即拒、
///           非法状态码与非法 UTF-8 原因即拒、首枚违例之后重复交出同一枚错误（这一幕特意再走一条
///           「违例帧已被吃掉」的路径：缓冲清空后仍交出同一枚错误，才排除了「把原始字节再判一遍」那种假自证）、
///           宿主回「不再收」之后消息不再派发但协议判定照走（派发口之前的 UTF-8 与帧头那两处各验一次：
///           收摊只关掉交付出口，不改对端是否违例）、`finish()` 丢弃对端从没说完的那半条消息。
///           **UTF-8 判定本身**另有一份逐形核对（`first_invalid_utf8()` 直接被喂字节）：四种长度的良构序列各取
///           边界值，畸形那几族（落单续字节、C0/C1 越码、截在串尾的半个序列、overlong、代理区、超过
///           U+10FFFF、`F5+`）逐一交出自己的位置，且交出的下标是**该序列的首字节**——收发两腿共用这份判定，
///           报出去的字节位置要能被「从那一字节起的数据是坏的」这句话直接读。
///           **写侧**验线上形态：与 `RFC 6455` §5.7 那道算例逐字节相同（已知密钥下的 FIN / opcode / 掩码位 /
///           长度档 / 密钥 / 掩码后载荷六处同时比对）、长度档随载荷跨 7 / 16 / 64 位、零长载荷照样带四枚密钥、
///           同一份载荷逐帧新取密钥且解掩后逐值等于原文、写一枚不合法的 control 帧会被拒而不落下任何字节、
///           `close` 载荷的状态码是大端、线上合法码表恰好收在 `{1000..1003} ∪ {1007..1014} ∪ {3000..4999}`。
///           一处刻意不作判据：取不到系统熵时写侧交出 `net-platform-unavailable` 那条分支——本用例跑到的每条腿
///           都有熵源，仓内也没有可断言那一行 WARN 的日志捕获面；那条分支的形态与 §8.8.27 的协议必需档同口径。

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/net/detail/ws_frame.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_ws_frame {
namespace {

using aurora::net::detail::AURORA_WS_MASK_KEY_BYTES;
using aurora::net::detail::AURORA_WS_MAX_CONTROL_BYTES;
using aurora::net::detail::close_frame_payload;
using aurora::net::detail::first_invalid_utf8;
using aurora::net::detail::is_close_code_on_wire;
using aurora::net::detail::write_ws_frame;
using aurora::net::detail::WsFrameLimits;
using aurora::net::detail::WsFrameReader;
using aurora::net::detail::WsFrameSinks;
using aurora::net::detail::WsOpcode;
using aurora::net::detail::WsParsedClose;
using aurora::net::detail::WsParsedMessage;

/// @brief 读侧收到的事实（四路各存一份，顺序即派发顺序）。
struct Recorder {
    std::vector<WsParsedMessage> messages;  ///< 成形的整条消息
    std::vector<std::string> pings;  ///< 探活帧载荷
    std::vector<std::string> pongs;  ///< 探活应答载荷
    std::vector<std::optional<std::uint16_t>> codes;  ///< 关闭帧带来的码（空 = 没带）
    std::vector<std::string> reasons;  ///< 关闭帧的原因
};

/// @brief 把四路出口接到一枚 `Recorder` 上。
/// @param rec 事实的去处（生命周期由用例保证）。
/// @param accept 消息出口的返回值（宿主的收摊判定）。
/// @return 出口束。
[[nodiscard]] auto sinks_for(Recorder &rec, bool accept = true) -> WsFrameSinks {
    WsFrameSinks sinks;
    sinks.on_message = [&rec, accept](WsParsedMessage message) {
        rec.messages.push_back(std::move(message));
        return accept;
    };
    sinks.on_ping = [&rec](std::string_view payload) { rec.pings.emplace_back(payload); };
    sinks.on_pong = [&rec](std::string_view payload) { rec.pongs.emplace_back(payload); };
    sinks.on_close = [&rec](WsParsedClose close) {
        rec.codes.push_back(close.code);
        rec.reasons.push_back(std::move(close.reason));
    };
    return sinks;
}

/// @brief 一枚大端长度档。
enum class LengthForm : std::uint8_t {
    Minimal = 0,  ///< 按载荷长度取最小档（服务端该这么写）
    TwoBytes = 1,  ///< 强制 16 位档
    EightBytes = 2,  ///< 强制 64 位档
};

/// @brief 按**服务端形态**写一帧（不掩码），供读侧喂字节。
///
/// 掩码位、保留位与长度档都可强行改写：读侧那几条负向判据要的就是「对端写错的那一型」，
/// 而那些形态不可能由本库的写侧产出（它拒写）。
/// @param final FIN 位。
/// @param opcode 低 4 位操作码（原样写入，含本层视为保留的那些取值）。
/// @param payload 载荷原文。
/// @param rsv 保留位（1 = RSV1，0 = 全清）。
/// @param masked 是否强行带上掩码位与四字节密钥。
/// @param form 长度档；非 `Minimal` 时可写出「非最少」的形态。
/// @param declared 长度字段写这一枚而不是载荷实长（只为「声明一个大家伙」那条判据而存在）。
/// @return 帧的线上字节。
[[nodiscard]] auto peer_frame(bool final, std::uint8_t opcode, std::string_view payload, std::uint8_t rsv = 0,
                              bool masked = false, LengthForm form = LengthForm::Minimal,
                              std::optional<std::uint64_t> declared = std::nullopt) -> std::string {
    std::string out;
    const auto rsv_bits = static_cast<unsigned>(rsv) << 4U;  // 升为 unsigned 再移，不给有符号数留位移
    out.push_back(static_cast<char>((final ? 0x80U : 0U) | rsv_bits | opcode));
    const auto real = static_cast<std::uint64_t>(payload.size());
    const auto length = declared.value_or(real);
    if (form == LengthForm::EightBytes || length > 0xFFFFULL) {
        out.push_back(static_cast<char>((masked ? 0x80U : 0U) | 127U));
        for (std::size_t step = 0; step < 8; ++step) {
            out.push_back(static_cast<char>((length >> ((7U - step) * 8U)) & 0xFFU));
        }
    } else if (form == LengthForm::TwoBytes || length >= 126) {
        out.push_back(static_cast<char>((masked ? 0x80U : 0U) | 126U));
        out.push_back(static_cast<char>((length >> 8U) & 0xFFU));
        out.push_back(static_cast<char>(length & 0xFFU));
    } else {
        out.push_back(static_cast<char>((masked ? 0x80U : 0U) | static_cast<unsigned>(length)));
    }
    if (masked) {
        out.append("\x11\x22\x33\x44");
    }
    out.append(payload);
    return out;
}

/// @brief 从本库写出的一帧里取出掩码密钥并解出载荷。
/// @param frame 写侧产出的字节。
/// @param payload 解出的载荷（仅返回真时有意义）。
/// @return 帧形能读为 true。
[[nodiscard]] auto unmask_written_frame(const std::string &frame, std::string &payload) -> bool {
    if (frame.size() < 6 || (static_cast<std::uint8_t>(frame[1]) & 0x80U) == 0) {
        return false;
    }
    const auto short_length = static_cast<std::uint8_t>(frame[1] & 0x7F);
    std::size_t header_bytes = 2;
    std::size_t length = short_length;
    if (short_length == 126) {
        header_bytes += 2;
        length =
            (static_cast<std::size_t>(static_cast<std::uint8_t>(frame[2])) << 8U) | static_cast<std::uint8_t>(frame[3]);
    } else if (short_length == 127) {
        header_bytes += 8;
        length = 0;
        for (std::size_t step = 0; step < 8; ++step) {
            length = (length << 8U) | static_cast<std::uint8_t>(frame[2 + step]);
        }
    }
    if (frame.size() < header_bytes + AURORA_WS_MASK_KEY_BYTES + length) {
        return false;
    }
    payload.clear();
    for (std::size_t index = 0; index < length; ++index) {
        const auto data = static_cast<std::uint8_t>(frame[header_bytes + AURORA_WS_MASK_KEY_BYTES + index]);
        const auto mask = static_cast<std::uint8_t>(frame[header_bytes + (index % AURORA_WS_MASK_KEY_BYTES)]);
        payload.push_back(static_cast<char>(data ^ mask));
    }
    return true;
}

/// @brief 读侧某段字节应当落 `ws-protocol-error`，且 detail 点名违的是哪一条。
/// @param settled 本次 `push()` 的结局。
/// @param needle detail 里必须出现的片段（ASCII）。
auto expect_refused(const aurora::Result<void> &settled, std::string_view needle) -> void {
    AURORA_TEST_REQUIRE(!settled);
    AURORA_TEST_CHECK_EQ(settled.error().code_enum, aurora::ErrorCode::WsProtocolError);
    AURORA_TEST_CHECK(settled.error().message.find(needle) != std::string::npos);
}

/// @brief 用给定上限喂一段字节并交出结局。
/// @param rec 事实的去处。
/// @param bytes 本段字节。
/// @param limits 两枚上限。
/// @return 本次 `push()` 的结局。
[[nodiscard]] auto feed(Recorder &rec, std::string_view bytes, WsFrameLimits limits = {}) -> aurora::Result<void> {
    WsFrameReader reader{sinks_for(rec), limits};
    return reader.push(bytes);
}

}  // namespace

AURORA_TEST_CASE(a_single_text_frame_is_one_message) {
    Recorder rec;
    const auto settled = feed(rec, peer_frame(true, 0x1, "hello"));
    AURORA_TEST_REQUIRE(settled);
    AURORA_TEST_REQUIRE_EQ(rec.messages.size(), 1U);
    AURORA_TEST_CHECK(rec.messages[0].opcode == WsOpcode::Text);
    AURORA_TEST_CHECK_EQ(rec.messages[0].payload, "hello");
}

AURORA_TEST_CASE(three_fragments_join_into_one_message_in_order) {
    Recorder rec;
    std::string wire = peer_frame(false, 0x1, "ab");
    wire += peer_frame(false, 0x0, "cd");
    wire += peer_frame(true, 0x0, "ef");
    const auto settled = feed(rec, wire);
    AURORA_TEST_REQUIRE(settled);
    // 三片是一条消息而不是三条：FIN 才结算，续片的 opcode 恒为 0。
    AURORA_TEST_REQUIRE_EQ(rec.messages.size(), 1U);
    AURORA_TEST_CHECK_EQ(rec.messages[0].payload, "abcdef");
}

AURORA_TEST_CASE(the_head_frame_decides_the_message_type) {
    Recorder rec;
    std::string wire = peer_frame(false, 0x2, "bin");
    wire += peer_frame(true, 0x0, "ary");
    const auto settled = feed(rec, wire);
    AURORA_TEST_REQUIRE(settled);
    AURORA_TEST_REQUIRE_EQ(rec.messages.size(), 1U);
    AURORA_TEST_CHECK(rec.messages[0].opcode == WsOpcode::Binary);
    AURORA_TEST_CHECK_EQ(rec.messages[0].payload, "binary");
}

AURORA_TEST_CASE(a_frame_delivered_one_byte_at_a_time_still_forms_one_message) {
    Recorder rec;
    const auto wire = peer_frame(true, 0x1, "streamed");
    WsFrameReader reader{sinks_for(rec)};
    for (const char unit : wire) {
        const auto settled = reader.push(std::string_view{&unit, 1});
        AURORA_TEST_REQUIRE(settled);
    }
    AURORA_TEST_REQUIRE_EQ(rec.messages.size(), 1U);
    AURORA_TEST_CHECK_EQ(rec.messages[0].payload, "streamed");
    AURORA_TEST_CHECK_EQ(reader.buffered_bytes(), 0U);
}

AURORA_TEST_CASE(a_partial_trailing_frame_waits_for_its_rest) {
    Recorder rec;
    std::string head = peer_frame(true, 0x1, "first");
    const std::string tail = peer_frame(true, 0x1, "second");
    head += tail.substr(0, 4);
    WsFrameReader reader{sinks_for(rec)};
    AURORA_TEST_REQUIRE(reader.push(head));
    // 前一条照发、后半条留在缓冲里等：这里若把 pending 整段清掉，第二条就永远发不出来。
    AURORA_TEST_REQUIRE_EQ(rec.messages.size(), 1U);
    AURORA_TEST_CHECK_EQ(rec.messages[0].payload, "first");
    AURORA_TEST_CHECK_EQ(reader.buffered_bytes(), 4U);
    AURORA_TEST_REQUIRE(reader.push(tail.substr(4)));
    AURORA_TEST_REQUIRE_EQ(rec.messages.size(), 2U);
    AURORA_TEST_CHECK_EQ(rec.messages[1].payload, "second");
    AURORA_TEST_CHECK_EQ(reader.buffered_bytes(), 0U);
}

AURORA_TEST_CASE(a_control_frame_between_two_fragments_does_not_break_the_message) {
    Recorder rec;
    std::string wire = peer_frame(false, 0x1, "ab");
    wire += peer_frame(true, 0x9, "are you there");  // 控制帧插在两片之间：规范明许
    wire += peer_frame(true, 0x0, "cd");
    const auto settled = feed(rec, wire);
    AURORA_TEST_REQUIRE(settled);
    AURORA_TEST_REQUIRE_EQ(rec.pings.size(), 1U);
    AURORA_TEST_CHECK_EQ(rec.pings[0], "are you there");
    AURORA_TEST_REQUIRE_EQ(rec.messages.size(), 1U);
    AURORA_TEST_CHECK_EQ(rec.messages[0].payload, "abcd");
}

AURORA_TEST_CASE(a_continuation_without_a_head_frame_is_refused) {
    Recorder rec;
    expect_refused(feed(rec, peer_frame(true, 0x0, "orphan")), "no fragmented message open");
    AURORA_TEST_CHECK_EQ(rec.messages.size(), 0U);
}

AURORA_TEST_CASE(a_new_data_frame_while_a_message_is_open_is_refused) {
    Recorder rec;
    // 两条消息不许在一条连接上交叠：头帧还没说完就来一枚新 data 帧，后一条属于谁无从判定。
    expect_refused(feed(rec, peer_frame(false, 0x1, "ab") + peer_frame(true, 0x1, "cd")), "still open");
}

AURORA_TEST_CASE(a_masked_frame_from_the_peer_is_refused) {
    Recorder rec;
    expect_refused(feed(rec, peer_frame(true, 0x1, "hi", 0, true)), "the peer masked a frame");
    AURORA_TEST_CHECK_EQ(rec.messages.size(), 0U);
}

AURORA_TEST_CASE(reserved_bits_and_reserved_opcodes_are_refused) {
    Recorder rec;
    expect_refused(feed(rec, peer_frame(true, 0x1, "hi", 1)), "reserved bits");
    expect_refused(feed(rec, peer_frame(true, 0x3, "hi")), "reserved opcode");
    expect_refused(feed(rec, peer_frame(true, 0xB, "hi")), "reserved opcode");
}

AURORA_TEST_CASE(a_fragmented_or_oversized_control_frame_is_refused) {
    Recorder rec;
    expect_refused(feed(rec, peer_frame(false, 0x9, "hi")), "control frame is fragmented");
    expect_refused(feed(rec, peer_frame(true, 0x9, std::string(AURORA_WS_MAX_CONTROL_BYTES + 1, 'p'))),
                   "exceeds the protocol limit of 125");
    // pong 同一条上限，且 125 枚本身合法（边界那侧要有人看着）。
    AURORA_TEST_REQUIRE(feed(rec, peer_frame(true, 0xA, std::string(AURORA_WS_MAX_CONTROL_BYTES, 'p'))));
    AURORA_TEST_CHECK_EQ(rec.pongs.size(), 1U);
}

AURORA_TEST_CASE(non_minimal_length_forms_are_refused) {
    Recorder rec;
    expect_refused(feed(rec, peer_frame(true, 0x1, "hi", 0, false, LengthForm::TwoBytes)), "fits the 7-bit form");
    expect_refused(feed(rec, peer_frame(true, 0x1, "hi", 0, false, LengthForm::EightBytes)), "fits the 16-bit form");
    // 64 位档的最高位留着：那一型长度不是「大」，而是根本没意义。
    std::string huge = peer_frame(true, 0x1, "", 0, false, LengthForm::EightBytes, 0x8000'0000'0000'0000ULL);
    expect_refused(feed(rec, huge), "most significant bit set");
}

AURORA_TEST_CASE(an_oversized_frame_is_refused_before_its_bytes_arrive) {
    Recorder rec;
    // 只喂声明了 2 MiB 的十字节帧头：越限在长度一到就判，那 MiB 不该先分配出来等对端收手。
    const auto declared = peer_frame(true, 0x2, "", 0, false, LengthForm::EightBytes, std::size_t{2} * 1024 * 1024);
    AURORA_TEST_CHECK_EQ(declared.size(), 10U);
    expect_refused(feed(rec, declared), "frame size");
    AURORA_TEST_CHECK(rec.messages.empty());
}

AURORA_TEST_CASE(an_oversized_message_names_the_reassembled_amount) {
    Recorder rec;
    WsFrameLimits tight;
    tight.max_frame_bytes = 64;
    tight.max_message_bytes = 10;
    // 两片各 8 字节：单帧都不越限，越的是重组量——detail 必须点名这一点，否则日志里两件事同句。
    const auto settled = feed(rec, peer_frame(false, 0x1, "12345678") + peer_frame(true, 0x0, "12345678"), tight);
    expect_refused(settled, "reassembled message");
    AURORA_TEST_CHECK(settled.error().message.find("frame size") == std::string::npos);
}

AURORA_TEST_CASE(a_frame_over_the_tightened_frame_limit_names_the_frame_size) {
    Recorder rec;
    WsFrameLimits tight;
    tight.max_frame_bytes = 8;
    expect_refused(feed(rec, peer_frame(true, 0x2, "0123456789"), tight), "frame size");
    // 同一份载荷在上限放宽时是合法的：判据盯的是旋钮而不是这枚长度本身。
    AURORA_TEST_REQUIRE(feed(rec, peer_frame(true, 0x2, "0123456789")));
}

AURORA_TEST_CASE(a_single_frame_bigger_than_the_message_limit_names_the_message_too) {
    Recorder rec;
    WsFrameLimits tight;
    tight.max_frame_bytes = 64;
    tight.max_message_bytes = 10;
    // 一条不分片的帧也能越「整条消息」那枚：它同时是头帧与整条消息，两枚闸都得问一遍。
    expect_refused(feed(rec, peer_frame(true, 0x2, std::string(12, 'x')), tight), "reassembled message");
    AURORA_TEST_CHECK_EQ(tight.max_frame_bytes, 64U);
}

AURORA_TEST_CASE(text_is_checked_for_utf_8_on_the_whole_message) {
    Recorder rec;
    // 一个三字节序列被劈在两片的交界处：逐片判会把合法的续字节当成违例，整条判才不会。
    const auto joined = feed(rec, peer_frame(false, 0x1, "\xE4\xB8") + peer_frame(true, 0x0, "\x96"));
    AURORA_TEST_REQUIRE(joined);
    AURORA_TEST_REQUIRE_EQ(rec.messages.size(), 1U);
    AURORA_TEST_CHECK_EQ(rec.messages[0].payload, "\xE4\xB8\x96");
    // 截在串尾的半截序列同样不合格：本层不替对端猜它下一帧要不要续上。
    Recorder broken;
    expect_refused(feed(broken, peer_frame(true, 0x1, "ok\xE4\xB8")), "not valid UTF-8");
    Recorder truncated;
    expect_refused(feed(truncated, peer_frame(true, 0x1, "\xF0\x9F\x98")), "not valid UTF-8");
    // 越码（C0 / C1）、代理区（ED A0）与超过 U+10FFFF 的四字节形各有猎枪。
    expect_refused(feed(truncated, peer_frame(true, 0x1, "\xC0\x80")), "not valid UTF-8");
    expect_refused(feed(truncated, peer_frame(true, 0x1, "\xED\xA0\x80")), "not valid UTF-8");
    expect_refused(feed(truncated, peer_frame(true, 0x1, "\xF4\x90\x80\x80")), "not valid UTF-8");
    expect_refused(feed(truncated, peer_frame(true, 0x1, "\xFF")), "not valid UTF-8");
    expect_refused(feed(truncated, peer_frame(true, 0x1, "\x80")), "not valid UTF-8");
    // 二进制不判：字节属于载荷，本层没有权解读它。
    Recorder binary;
    AURORA_TEST_REQUIRE(feed(binary, peer_frame(true, 0x2, "\xFF\xFE\x00")));
    AURORA_TEST_CHECK_EQ(binary.messages.size(), 1U);
}

AURORA_TEST_CASE(a_close_frame_reports_code_and_reason_or_the_absence_of_one) {
    Recorder rec;
    AURORA_TEST_REQUIRE(feed(rec, peer_frame(true, 0x8, close_frame_payload(1000, "bye"))));
    AURORA_TEST_REQUIRE_EQ(rec.codes.size(), 1U);
    // 前序两枚 AURORA_TEST_REQUIRE 已断言状态码在场，而断言宏的展开对 tidy 的路径分析不透明，属误报；
    // 本机 tidy 22.1.2 遍不报、CI 的 Linux 22.1.8 遍把两处解包各自报在解包那行，故整段收进区间式豁免
    // （同族写法见本文件的 UTF-8 用例与 `tests/unit/utest_net_websocket.cpp`）。
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    AURORA_TEST_REQUIRE(rec.codes[0].has_value());
    AURORA_TEST_CHECK_EQ(*rec.codes[0], 1000U);
    AURORA_TEST_CHECK_EQ(rec.reasons[0], "bye");

    // 只有码、没有原因：两枚字节是码而不是「原因的编码」。
    Recorder only_code;
    AURORA_TEST_REQUIRE(feed(only_code, peer_frame(true, 0x8, close_frame_payload(1001, ""))));
    AURORA_TEST_REQUIRE(only_code.codes[0].has_value());
    AURORA_TEST_CHECK_EQ(*only_code.codes[0], 1001U);
    AURORA_TEST_CHECK(only_code.reasons[0].empty());
    // NOLINTEND(bugprone-unchecked-optional-access)

    // 零长载荷 = 对端没带码：折成 1000 会让一次没说清的收场冒充一次好端端的道别。
    Recorder silent;
    AURORA_TEST_REQUIRE(feed(silent, peer_frame(true, 0x8, "")));
    AURORA_TEST_CHECK_EQ(silent.codes.size(), 1U);
    AURORA_TEST_CHECK(!silent.codes[0].has_value());
    AURORA_TEST_CHECK(silent.reasons[0].empty());
}

AURORA_TEST_CASE(a_malformed_close_frame_is_refused) {
    Recorder rec;
    expect_refused(feed(rec, peer_frame(true, 0x8, "\x03")), "a status code is two bytes");
    expect_refused(feed(rec, peer_frame(true, 0x8, close_frame_payload(1005, ""))), "not valid on the wire");
    expect_refused(feed(rec, peer_frame(true, 0x8, close_frame_payload(1015, ""))), "not valid on the wire");
    expect_refused(feed(rec, peer_frame(true, 0x8, close_frame_payload(999, ""))), "not valid on the wire");
    expect_refused(feed(rec, peer_frame(true, 0x8, close_frame_payload(2000, ""))), "not valid on the wire");
    expect_refused(feed(rec, peer_frame(true, 0x8, close_frame_payload(5000, ""))), "not valid on the wire");
    expect_refused(feed(rec, peer_frame(true, 0x8, close_frame_payload(1000, "\xFF"))), "close reason is not valid");
}

AURORA_TEST_CASE(the_first_protocol_violation_is_the_only_verdict_afterwards) {
    Recorder rec;
    WsFrameReader reader{sinks_for(rec)};
    const auto first = reader.push(peer_frame(true, 0x1, "hi", 0, true));
    AURORA_TEST_REQUIRE(!first);
    const auto again = reader.push(peer_frame(true, 0x1, "again"));
    AURORA_TEST_REQUIRE(!again);
    // 终态重复交出同一枚错误、不做二次判定：那批字节属于谁已经不可知。
    AURORA_TEST_CHECK_EQ(again.error().message, first.error().message);
    AURORA_TEST_CHECK_EQ(rec.messages.size(), 0U);
    const auto finished = reader.finish();
    AURORA_TEST_REQUIRE(!finished);
    AURORA_TEST_CHECK_EQ(finished.error().code_enum, aurora::ErrorCode::WsProtocolError);
    // 第二幕刻意挑「违例帧已被吃掉」那一条路：孤儿续片在结算处判违例，本层的缓冲连同那几枚字节一起清空，
    // 于是「把原始字节再判一遍」这条路根本不存在。此时仍交出同一枚错误、且一条消息都不派发的，只有终态旗位。
    Recorder clean;
    WsFrameReader second{sinks_for(clean)};
    const auto orphan = second.push(peer_frame(true, 0x0, "no head"));
    AURORA_TEST_REQUIRE(!orphan);
    AURORA_TEST_CHECK_EQ(second.buffered_bytes(), 0U);
    const auto after = second.push(peer_frame(true, 0x1, "a perfectly legal frame"));
    AURORA_TEST_REQUIRE(!after);
    AURORA_TEST_CHECK_EQ(after.error().message, orphan.error().message);
    AURORA_TEST_CHECK_EQ(clean.messages.size(), 0U);
}

AURORA_TEST_CASE(host_stop_silences_messages_but_not_the_checks) {
    Recorder rec;
    WsFrameReader reader{sinks_for(rec, false)};
    AURORA_TEST_REQUIRE(reader.push(peer_frame(true, 0x1, "one")));
    AURORA_TEST_REQUIRE(reader.push(peer_frame(true, 0x9, "ping")));
    AURORA_TEST_REQUIRE(reader.push(peer_frame(true, 0x1, "two")));
    // 收摊之后：消息不再派发，但控制帧照报、协议判定照走（宿主胃口不改变对端是否违例）。
    AURORA_TEST_CHECK_EQ(rec.messages.size(), 1U);
    AURORA_TEST_CHECK_EQ(rec.pings.size(), 1U);
    AURORA_TEST_CHECK(reader.host_paused());
    // 整条消息的 UTF-8 判定挂在派发口**之前**：把它挪到派发口之后（或干脆随胃口短路）就等于让收摊中的
    // 连接免检，而对端是否写坏了字节与宿主还要不要听毫无关系。
    const auto not_utf8 = reader.push(peer_frame(true, 0x1, "still checked \xFF"));
    AURORA_TEST_REQUIRE(!not_utf8);
    AURORA_TEST_CHECK_EQ(not_utf8.error().code_enum, aurora::ErrorCode::WsProtocolError);
    AURORA_TEST_CHECK_EQ(rec.messages.size(), 1U);
    // 帧头那几条（保留位）同理，但要另起一条流：上一条已经判死，终态错误是粘的。
    Recorder framing;
    WsFrameReader paused{sinks_for(framing, false)};
    AURORA_TEST_REQUIRE(paused.push(peer_frame(true, 0x1, "one")));
    AURORA_TEST_CHECK(paused.host_paused());
    const auto rsv = paused.push(peer_frame(true, 0x1, "again", 1));
    AURORA_TEST_REQUIRE(!rsv);
    AURORA_TEST_CHECK_EQ(rsv.error().code_enum, aurora::ErrorCode::WsProtocolError);
    AURORA_TEST_CHECK_EQ(framing.messages.size(), 1U);
}

AURORA_TEST_CASE(finish_discards_what_the_peer_never_finished) {
    Recorder rec;
    WsFrameReader reader{sinks_for(rec)};
    AURORA_TEST_REQUIRE(reader.push(peer_frame(false, 0x1, "half a message")));
    AURORA_TEST_CHECK(reader.message_in_progress());
    AURORA_TEST_REQUIRE(reader.finish());
    // 补发半条消息 = 把对端从没说完的话当成一条交出去。
    AURORA_TEST_CHECK_EQ(rec.messages.size(), 0U);
    AURORA_TEST_CHECK(!reader.message_in_progress());
    AURORA_TEST_CHECK_EQ(reader.buffered_bytes(), 0U);
}

AURORA_TEST_CASE(a_written_text_frame_matches_the_wire_byte_for_byte) {
    // 与 `RFC 6455` §5.7 那道算例同载荷同密钥：六处形态（FIN、opcode、保留位、掩码位、长度档、掩码结果）
    // 一次比对全钉住——分开断言时「掩码位写成 0」和「载荷没掩」会互相抵消，逐字节比不会。
    const std::array<std::byte, AURORA_WS_MASK_KEY_BYTES> key{std::byte{0x37}, std::byte{0xFA}, std::byte{0x21},
                                                              std::byte{0x03}};
    std::string out;
    const auto written = write_ws_frame(out, WsOpcode::Text, true, "Hi", key);
    AURORA_TEST_REQUIRE(written);
    const std::string expected{"\x81\x82\x37\xfa\x21\x03\x7f\x93", 8};
    AURORA_TEST_CHECK_EQ(out.size(), 8U);
    AURORA_TEST_CHECK(out == expected);
    // 未定尾的同一份载荷只改 FIN 位。
    std::string open_frame;
    AURORA_TEST_REQUIRE(write_ws_frame(open_frame, WsOpcode::Text, false, "Hi", key));
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(open_frame[0]), 0x01U);
}

AURORA_TEST_CASE(the_length_form_grows_with_the_payload) {
    const std::array<std::byte, AURORA_WS_MASK_KEY_BYTES> key{std::byte{0xAA}, std::byte{0xAA}, std::byte{0xAA},
                                                              std::byte{0xAA}};
    std::string edge;
    AURORA_TEST_REQUIRE(write_ws_frame(edge, WsOpcode::Binary, true, std::string(125, 'x'), key));
    AURORA_TEST_CHECK_EQ(edge.size(), 2U + AURORA_WS_MASK_KEY_BYTES + 125U);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(edge[1]), 0x80U | 125U);

    std::string wide;
    AURORA_TEST_REQUIRE(write_ws_frame(wide, WsOpcode::Binary, true, std::string(200, 'x'), key));
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(wide[1]), 0x80U | 126U);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(wide[2]), 0x00U);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(wide[3]), 0xC8U);
    AURORA_TEST_CHECK_EQ(wide.size(), 4U + AURORA_WS_MASK_KEY_BYTES + 200U);
    // 密钥紧跟长度字段：那一处偏移写错时，掩码结果看着仍像「掩过的字节」，只有逐枚比对才现形。
    for (std::size_t step = 0; step < AURORA_WS_MASK_KEY_BYTES; ++step) {
        AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(wide[4 + step]), 0xAAU);
    }
    // 掩码是逐字节的：同一枚密钥下 200 枚全同的载荷写出后也全同（这里比的是掩码算法不是随机性）。
    // 载荷的第一枚在 `2 + 2` 枚头与 `4` 枚密钥之后，即下标 8；写成 `== a ^ b` 会被 `==` 抢先结合，故两侧各自加括号。
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(wide[8]), static_cast<std::uint8_t>(0x78U ^ 0xAAU));

    std::string widest;
    AURORA_TEST_REQUIRE(write_ws_frame(widest, WsOpcode::Binary, true, std::string(70000, 'x'), key));
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(widest[1]), 0x80U | 127U);
    AURORA_TEST_CHECK_EQ(widest.size(), 10U + AURORA_WS_MASK_KEY_BYTES + 70000U);
    std::string back;
    AURORA_TEST_REQUIRE(unmask_written_frame(widest, back));
    AURORA_TEST_CHECK_EQ(back.size(), 70000U);
}

AURORA_TEST_CASE(an_empty_payload_still_carries_four_key_bytes) {
    std::string out;
    AURORA_TEST_REQUIRE(write_ws_frame(out, WsOpcode::Text, true, ""));
    // 密钥属于帧头而不属于载荷：省掉那四枚会让对端把载荷的头四枚读成密钥。
    AURORA_TEST_CHECK_EQ(out.size(), 2U + AURORA_WS_MASK_KEY_BYTES);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(out[1]), 0x80U);
}

AURORA_TEST_CASE(each_frame_draws_its_own_key_and_still_round_trips) {
    std::string first;
    std::string second;
    AURORA_TEST_REQUIRE(write_ws_frame(first, WsOpcode::Text, true, "same payload"));
    AURORA_TEST_REQUIRE(write_ws_frame(second, WsOpcode::Text, true, "same payload"));
    // 同一份载荷逐帧新取密钥：写死一枚（或复用一枚）会让整条连接的掩码形同一个静态扰动。
    AURORA_TEST_CHECK_NE(first.substr(2, AURORA_WS_MASK_KEY_BYTES), second.substr(2, AURORA_WS_MASK_KEY_BYTES));
    AURORA_TEST_CHECK(first != second);
    std::string back;
    AURORA_TEST_REQUIRE(unmask_written_frame(first, back));
    AURORA_TEST_CHECK_EQ(back, "same payload");
}

AURORA_TEST_CASE(the_writer_refuses_a_control_frame_it_would_send_illegally) {
    const std::array<std::byte, AURORA_WS_MASK_KEY_BYTES> key{std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                                                              std::byte{0x04}};
    std::string out;
    const auto oversized = write_ws_frame(out, WsOpcode::Ping, true, std::string(200, 'p'), key);
    AURORA_TEST_REQUIRE(!oversized);
    AURORA_TEST_CHECK_EQ(oversized.error().code_enum, aurora::ErrorCode::WsProtocolError);
    const auto fragmented = write_ws_frame(out, WsOpcode::Pong, false, "hi", key);
    AURORA_TEST_REQUIRE(!fragmented);
    // 两处拒的都是「我方自己要写出去的违例」，且都不留半个帧在串里——半帧比没帧更难收拾。
    AURORA_TEST_CHECK(out.empty());
    std::string legal;
    AURORA_TEST_REQUIRE(
        write_ws_frame(legal, WsOpcode::Ping, true, std::string(AURORA_WS_MAX_CONTROL_BYTES, 'p'), key));
    AURORA_TEST_CHECK_EQ(legal.size(), 2U + AURORA_WS_MASK_KEY_BYTES + AURORA_WS_MAX_CONTROL_BYTES);
}

AURORA_TEST_CASE(close_frame_payload_is_big_endian_and_keeps_the_reason) {
    const auto payload = close_frame_payload(1000, "bye");
    AURORA_TEST_CHECK_EQ(payload.size(), 5U);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(payload[0]), 0x03U);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(payload[1]), 0xE8U);
    AURORA_TEST_CHECK_EQ(payload.substr(2), "bye");
    // 只有码时不留一枚空原因的位置：两枚字节就是全部。
    AURORA_TEST_CHECK_EQ(close_frame_payload(1001, "").size(), 2U);
}

AURORA_TEST_CASE(the_close_code_table_accepts_exactly_the_published_ranges) {
    constexpr std::array<std::uint16_t, 16> allowed{1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010,
                                                    1011, 1012, 1013, 1014, 3000, 3999, 4000, 4999};
    for (const std::uint16_t code : allowed) {
        AURORA_TEST_CHECK(is_close_code_on_wire(code));
    }
    constexpr std::array<std::uint16_t, 10> refused{0, 999, 1004, 1005, 1006, 1015, 1016, 2999, 5000, 65535};
    for (const std::uint16_t code : refused) {
        AURORA_TEST_CHECK(!is_close_code_on_wire(code));
    }
}

AURORA_TEST_CASE(the_connection_frame_limit_binds_control_frames_too) {
    Recorder rec;
    WsFrameLimits tight;
    tight.max_frame_bytes = 8;
    // 单帧上限管的是「帧」，不分控制帧：把上限调到 125 以下就是自愿接受一次普通心跳也能越限。
    // 缺省那枚（1 MiB）远在 125 之上，故这条分岔只在收窄旋钮时可见。
    expect_refused(feed(rec, peer_frame(true, 0x9, std::string(60, 'p')), tight), "frame size");
    Recorder relaxed;
    AURORA_TEST_REQUIRE(feed(relaxed, peer_frame(true, 0x9, std::string(60, 'p'))));
    AURORA_TEST_CHECK_EQ(relaxed.pings.size(), 1U);
}

AURORA_TEST_CASE(utf8_check_accepts_well_formed_shapes_and_points_at_the_lead_byte) {
    // 良构的四档边界：两字节档的最小值 `C2 80`、三字节档代理区之前的最后一枚 `ED 9F BF`、四字节档的上限
    // `F4 8F BF BF`（= `U+10FFFF`），加上 ASCII 与空段。
    constexpr std::array<std::string_view, 9> well_formed{
        "",         "plain ascii",  "\xC3\xA9",         "\xE4\xB8\xAD", "\xF0\x9F\x98\x80",
        "\xC2\x80", "\xED\x9F\xBF", "\xF4\x8F\xBF\xBF", "\xE0\xA0\x80"};
    for (const std::string_view text : well_formed) {
        AURORA_TEST_CHECK_MSG(!first_invalid_utf8(text).has_value(), std::string{text});
    }

    // 畸形七族各取一型，位置恒指到**序列首字节**：`E0 9F` 与 `ED A0` 坏在第二枚字节上，但那一枚属于谁由首字节
    // 决定，报续字节的位置只会让排查者去数错的那一行。
    struct Probe {
        std::string_view bytes;  ///< 待判的一段
        std::size_t first_bad;  ///< 期望交出的下标
    };
    constexpr std::array<Probe, 12> refused{
        Probe{.bytes = "\xFF", .first_bad = 0},  // 落单的高位字节
        Probe{.bytes = "\x80", .first_bad = 0},  // 落单续字节（没有引导它的起始字节）
        Probe{.bytes = "\xC0\x80", .first_bad = 0},  // C0：overlong 两字节档，协议整档禁用
        Probe{.bytes = "\xC1\xBF", .first_bad = 0},  // C1 同上
        Probe{.bytes = "ab\xC3", .first_bad = 2},  // 截在串尾的半个序列，位置按整段算
        Probe{.bytes = "\xC2\x2C", .first_bad = 0},  // 两字节档的后继不是续字节
        Probe{.bytes = "\xE4\xB8", .first_bad = 0},  // 三字节档截断
        Probe{.bytes = "\xE0\x9F\xBF", .first_bad = 0},  // 三字节档 overlong
        Probe{.bytes = "\xED\xA0\x80", .first_bad = 0},  // 代理区（UTF-8 不许携带码点）
        Probe{.bytes = "\xF0\x8F\xBF\xBF", .first_bad = 0},  // 四字节档 overlong
        Probe{.bytes = "\xF4\x90\x80\x80", .first_bad = 0},  // 超过 U+10FFFF
        Probe{.bytes = "a\xE6\xB8\x85\xFF", .first_bad = 4},  // 合法序列之后才坏：前四枚字节不得替它背书
    };
    for (const Probe &probe : refused) {
        const auto bad = first_invalid_utf8(probe.bytes);
        AURORA_TEST_REQUIRE_MSG(bad.has_value(), "a malformed sequence was accepted");
        // 紧邻式豁免：上一行的 AURORA_TEST_REQUIRE_MSG 已断言持有值，宏展开对 tidy 的路径分析不透明，
        // 属误报；本机 tidy 22.1.2 遍不报，CI 的 Linux 22.1.8 遍报在本行。
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        AURORA_TEST_CHECK_EQ(*bad, probe.first_bad);
    }
}

}  // namespace aurora::test_cases::utest_net_ws_frame
