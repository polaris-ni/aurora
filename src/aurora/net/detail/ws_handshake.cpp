// ws_handshake.cpp —— WebSocket 升级握手的字节层实现（口径全文见 ws_handshake.h）。
#include "aurora/net/detail/ws_handshake.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/net/detail/http_auth_wire.h"
#include "aurora/net/detail/http_wire.h"
#include "aurora/net/detail/random_draw.h"

namespace aurora::net::detail {
namespace {

/// @brief 剥掉首尾的 OWS（SP / HTAB；`RFC 9110 §5.6.3`）。
[[nodiscard]] auto trim_ows(std::string_view text) -> std::string_view {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

/// @brief ASCII 小写化（字段值比对用；非 ASCII 字节原样留下）。
[[nodiscard]] auto to_lower(std::string_view text) -> std::string {
    std::string out;
    out.reserve(text.size());
    for (const char unit : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(unit))));
    }
    return out;
}

/// @brief 剥掉文本里的**全部** ASCII 空白（不只是首尾）。
///
/// `RFC 6455 §1.3` 明说比对前要把首部空白、尾部空白与折行都剥掉，而 base64 的字母表本身不含空白，
/// 故「含空白的两处剥法」与「剥掉一切空白的这一处剥法」在同一枚合法值上给出同一结果；后者不需要
/// 再判「空白出现在中间算不算折行」。
[[nodiscard]] auto strip_blanks(std::string_view text) -> std::string {
    std::string out;
    out.reserve(text.size());
    for (const char unit : text) {
        if (unit != ' ' && unit != '\t' && unit != '\r' && unit != '\n') {
            out.push_back(unit);
        }
    }
    return out;
}

/// @brief 把逗号分隔的字段值切成 token 列表（各段先剥 OWS，空段丢掉）。
[[nodiscard]] auto split_tokens(std::string_view value) -> std::vector<std::string_view> {
    std::vector<std::string_view> out;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto comma = value.find(',', begin);
        const auto stop = comma == std::string_view::npos ? value.size() : comma;
        const auto piece = trim_ows(value.substr(begin, stop - begin));
        if (!piece.empty()) {
            out.push_back(piece);
        }
        if (stop == value.size()) {
            break;
        }
        begin = stop + 1;
    }
    return out;
}

/// @brief 十进制文本（错误 detail 里点名数量与状态码用；避开 `std::format` 在本腿的可用性分岔）。
[[nodiscard]] auto decimal(std::uint64_t value) -> std::string { return std::to_string(value); }

/// @brief 握手被拒的成形口：`ws-handshake-rejected` + 点名哪一处。
[[nodiscard]] auto rejected(std::string_view detail) -> Error {
    return make_error(ErrorCode::WsHandshakeRejected, ErrorParams{{"detail", std::string{detail}}});
}

/// @brief `SHA-1`（`RFC 3174`）：本库唯一需要它的地方是握手的 `Accept` 摘要，故不出本文件。
///
/// 分组、填充（`0x80` + 零 + 64 位大端位长）与四轮函数都按标准写法，不做任何「看着等价」的简化：
/// 这一处的输入是**网络对端给的文本**，摘要算错不会崩，只会让一条本来合法的升级被自己判成对端伪造。
/// @param input 待摘要字节。
/// @return 20 字节摘要（大端次序，可直接喂 base64）。
[[nodiscard]] auto sha1(std::string_view input) -> std::array<std::uint8_t, 20> {
    std::string message(input);
    const auto bit_length = static_cast<std::uint64_t>(message.size()) * 8ULL;
    message.push_back(static_cast<char>(0x80U));
    while (message.size() % 64U != 56U) {
        message.push_back('\0');
    }
    for (std::size_t step = 0; step < 8U; ++step) {
        message.push_back(static_cast<char>((bit_length >> ((7U - step) * 8U)) & 0xFFULL));
    }
    std::array<std::uint32_t, 5U> state{0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};
    // 三段循环的下标恒在数组长度之内（分组 16 字、扩展与四轮共 80 字、摘要 20 字节，全部取自 `RFC 3174`
    // 的定长结构），改 `.at()` 只是给一条纯内存摘要逐字加一次运行时判定。按本仓定长表同族写法就地豁免。
    // NOLINTBEGIN(*-pro-bounds-constant-array-index)
    for (std::size_t block = 0; block < message.size(); block += 64U) {
        std::array<std::uint32_t, 80U> words{};
        for (std::size_t index = 0; index < 16U; ++index) {
            const auto at = block + (index * 4U);
            words[index] = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(message[at])) << 24U) |
                           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(message[at + 1U])) << 16U) |
                           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(message[at + 2U])) << 8U) |
                           static_cast<std::uint32_t>(static_cast<std::uint8_t>(message[at + 3U]));
        }
        for (std::size_t index = 16U; index < 80U; ++index) {
            words[index] =
                std::rotl(words[index - 3U] ^ words[index - 8U] ^ words[index - 14U] ^ words[index - 16U], 1U);
        }
        auto a = state[0U];
        auto b = state[1U];
        auto c = state[2U];
        auto d = state[3U];
        auto e = state[4U];
        for (std::size_t index = 0; index < 80U; ++index) {
            std::uint32_t mixed = 0;
            std::uint32_t constant = 0;
            if (index < 20U) {
                mixed = (b & c) | (~b & d);
                constant = 0x5A827999U;
            } else if (index < 40U) {
                mixed = b ^ c ^ d;
                constant = 0x6ED9EBA1U;
            } else if (index < 60U) {
                mixed = (b & c) | (b & d) | (c & d);
                constant = 0x8F1BBCDCU;
            } else {
                mixed = b ^ c ^ d;
                constant = 0xCA62C1D6U;
            }
            const auto next = std::rotl(a, 5U) + mixed + e + constant + words[index];
            e = d;
            d = c;
            c = std::rotl(b, 30U);
            b = a;
            a = next;
        }
        state[0U] += a;
        state[1U] += b;
        state[2U] += c;
        state[3U] += d;
        state[4U] += e;
    }
    std::array<std::uint8_t, 20U> digest{};
    for (std::size_t index = 0; index < 5U; ++index) {
        digest[index * 4U] = static_cast<std::uint8_t>(state[index] >> 24U);
        digest[(index * 4U) + 1U] = static_cast<std::uint8_t>(state[index] >> 16U);
        digest[(index * 4U) + 2U] = static_cast<std::uint8_t>(state[index] >> 8U);
        digest[(index * 4U) + 3U] = static_cast<std::uint8_t>(state[index] & 0xFFU);
    }
    // NOLINTEND(*-pro-bounds-constant-array-index)
    return digest;
}

/// @brief 剥掉调用方那三枚「体形态」字段：握手不带体，留着会让对端把体字节读成第一帧。
/// @param fields 调用方的请求字段（只借用）。
/// @return 重建过的字段表（同名保序、原始拼写保留）。
[[nodiscard]] auto without_body_fields(const Headers &fields) -> Headers {
    Headers out;
    for (const auto &entry : fields.entries()) {
        const auto name = to_lower(entry.name);
        if (name == "content-length" || name == "transfer-encoding" || name == "content-encoding") {
            continue;
        }
        out.append(entry.name, entry.value);
    }
    return out;
}

}  // namespace

auto ws_accept_token(std::string_view key) -> std::string {
    std::string material(key);
    material.append(AURORA_WS_HANDSHAKE_GUID);
    const auto digest = sha1(material);
    std::string raw;
    raw.reserve(digest.size());
    for (const auto unit : digest) {
        raw.push_back(static_cast<char>(unit));
    }
    return base64_encode(raw);
}

auto render_ws_handshake(const Url &target, const Headers &fields, const WsHandshakeOffer &offer, bool absolute_form,
                         std::string &key_out) -> Result<std::string> {
    std::array<std::byte, AURORA_WS_KEY_BYTES> drawn{};
    // 这一枚 key 是协议要求的不可预测值：取不到熵就交出失败。回落伪随机等于「看着掩了、其实可猜」，
    // 与本库「不提供假降级」那条纪律正相反（分界见 §8.8.27 的协议必需档）。
    if (const auto filled = random_fill(drawn); !filled) {
        return Result<std::string>{filled.error()};
    }
    std::string raw;
    raw.reserve(drawn.size());
    for (const auto unit : drawn) {
        raw.push_back(static_cast<char>(static_cast<std::uint8_t>(unit)));
    }
    key_out = base64_encode(raw);

    Headers merged = without_body_fields(fields);
    merged.set("Upgrade", "websocket");
    merged.set("Connection", "Upgrade");
    merged.set("Sec-WebSocket-Version", AURORA_WS_VERSION);
    merged.set("Sec-WebSocket-Key", key_out);
    if (!offer.subprotocols.empty()) {
        std::string list;
        for (const auto &item : offer.subprotocols) {
            if (!list.empty()) {
                list += ", ";
            }
            list += item;
        }
        merged.set("Sec-WebSocket-Protocol", list);
    }
    // 方法写死 GET：这不是「调用方没选」，而是 `RFC 6455 §5.1` 只把 GET 列为合法起手（分岔在发起侧
    // 已经判掉，这里再写一遍是保证本函数**任何**一条来路都发不出别的形态）。
    return wire::render_request_head("GET", target, merged, absolute_form);
}

auto validate_ws_handshake(const wire::ResponseHead &head, std::string_view key, const WsHandshakeOffer &offer)
    -> Result<WsHandshakeAccept> {
    if (head.status != 101U) {
        return Result<WsHandshakeAccept>{
            rejected("the peer answered " + decimal(head.status) + " instead of switching protocols")};
    }
    const auto upgrade = head.headers.get("Upgrade");
    if (!upgrade.has_value()) {
        return Result<WsHandshakeAccept>{rejected("the response carries no Upgrade field")};
    }
    if (to_lower(trim_ows(*upgrade)) != "websocket") {
        return Result<WsHandshakeAccept>{rejected("the Upgrade field names \"" + *upgrade + "\", not websocket")};
    }
    bool connection_upgrades = false;
    for (const auto &value : head.headers.values("Connection")) {
        for (const auto token : split_tokens(value)) {
            if (to_lower(token) == "upgrade") {
                connection_upgrades = true;
                break;
            }
        }
    }
    if (!connection_upgrades) {
        return Result<WsHandshakeAccept>{
            rejected("the Connection field list does not carry the upgrade token (it names how this "
                     "connection ends after the switch)")};
    }
    if (head.headers.contains("Sec-WebSocket-Extensions")) {
        const auto announced = head.headers.get("Sec-WebSocket-Extensions").value_or(std::string{});
        return Result<WsHandshakeAccept>{rejected("the peer announced an extension (\"" + announced +
                                                  "\") that this client never requested; frame bytes read "
                                                  "under it would look like broken frames, not like a "
                                                  "missing decoder")};
    }
    const auto accept = head.headers.get("Sec-WebSocket-Accept");
    if (!accept.has_value()) {
        return Result<WsHandshakeAccept>{
            rejected("the response carries no Sec-WebSocket-Accept field: nothing proves the peer read "
                     "this upgrade request")};
    }
    if (strip_blanks(*accept) != ws_accept_token(key)) {
        return Result<WsHandshakeAccept>{
            rejected("the Sec-WebSocket-Accept value does not match the value "
                     "this request's key implies")};
    }
    WsHandshakeAccept accepted;
    if (const auto protocol = head.headers.get("Sec-WebSocket-Protocol"); protocol.has_value()) {
        const auto pieces = split_tokens(*protocol);
        for (const auto piece : pieces) {
            const bool offered = std::ranges::find(offer.subprotocols, std::string{piece}) != offer.subprotocols.end();
            if (!offered) {
                return Result<WsHandshakeAccept>{rejected("the peer selected subprotocol \"" + std::string{piece} +
                                                          "\", which this client never offered")};
            }
        }
        if (!pieces.empty()) {
            accepted.subprotocol.assign(pieces.front());
        }
    } else if (offer.require_subprotocol) {
        return Result<WsHandshakeAccept>{
            rejected("the caller requires one of the offered subprotocols but the peer returned none; "
                     "falling back would mean running business data under the wrong protocol")};
    }
    return Result<WsHandshakeAccept>{std::move(accepted)};
}

}  // namespace aurora::net::detail
