/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/ws_handshake.h + src/aurora/net/detail/ws_handshake.cpp
/// 测试说明: WebSocket 升级握手的离线回归（纯字节、零 socket、零时钟、零线程），写侧与判侧各一层。
///           **写侧**（`render_ws_handshake`）验只有真把字节写出来才成立的口径：四枚协议字段（`Upgrade` /
///           `Connection` / `Sec-WebSocket-Version` / `Sec-WebSocket-Key`）恒由库写定且**同名只有一枚**（调用方
///           抢先写过也要被覆写，并列会让对端拿到两枚 key 而无从校验）；体形态的三枚字段（`Content-Length` /
///           `Transfer-Encoding` / `Content-Encoding`）一律被剥掉——握手发出的字节里若混进体，对端会把它当第一帧；
///           请求行恒 `GET` 且 origin-form 与 absolute-form 两型各证一次（代理转发形态与直连形态的唯一分岔）；
///           `Host` 的补齐沿用 HTTP 那一条路（含非缺省端口）；字段不合文法时交出的是 `http-request-invalid`
///           而不是新造一枚握手码；`Sec-WebSocket-Key` 是 16 枚熵字节的 base64（长度 24、字符集、逐次互异三条
///           同时成立才算「看着像随机」之外真的不可预判）；子协议按提出次序写成一行逗号列表，一个都没提时
///           整枚字段不出现。
///           **判侧**（`validate_ws_handshake`）验只有跨两次实现才会漂的口径：`ws_accept_token` 用 `RFC 6455`
///           §1.3 那道算例逐字符钉住，另加三枚长度分别落在「填充仍在同块」「材料恰满一块（`0x80` 与位长被挤到
///           第二块）」「跨两块」的取样，把 `SHA-1` 的填充与分块两处都钉到场；确定性（同一枚 key 两次算得相同）与
///           key 敏感性（换一枚 key 即换一枚摘要）各证一次。六类拒因**各点名自己那一处**——非 101（并把状态码
///           写进文本，`426` 与 `401` 是两条完全不同的排查路）、缺 `Upgrade` 与它的值不是 `websocket`、
///           `Connection` 的逗号列表不含 `upgrade` token（含则算过，哪怕同一行还写着 `keep-alive`；只以空白
///           分隔的两个词不构成列表）、缺 `Sec-WebSocket-Accept` 与它对不上本次 key 的摘要、对端宣告了客户端
///           从没请求的扩展。比对前剥掉的那一切 ASCII 空白单独证一次：合法算例写成折行形态仍算对。
///           子协议三型结局各一次：回了且是提出过的一枚（取第一枚记进事实）、回了却没提过（擅自换协议即拒）、
///           没回——不要求必选时**算成功**且事实里为空，要求必选时即拒；这两型合起来才是 §18 那条「降级是合法
///           协商结局，但不由库替宿主决定」的可执行形态。最后一枚用例把六类拒因的文本并排比一次互异性：
///           一句笼统的「握手被拒」会让调用方只能重跑一遍才知道卡在哪一步。
///           一处刻意不作判据：取不到系统熵时写侧交出 `net-platform-unavailable` 那条分支——本用例跑到的每条腿
///           都有熵源，仓内也没有可断言那一行 WARN 的日志捕获面；它的形态与 §8.8.27 的协议必需档同口径。

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/headers.h"
#include "aurora/core/result.h"
#include "aurora/core/url.h"
#include "aurora/net/detail/http_wire.h"
#include "aurora/net/detail/ws_handshake.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_ws_handshake {
namespace {

using aurora::net::detail::AURORA_WS_KEY_BYTES;
using aurora::net::detail::render_ws_handshake;
using aurora::net::detail::validate_ws_handshake;
using aurora::net::detail::ws_accept_token;
using aurora::net::detail::WsHandshakeOffer;
using aurora::net::wire::ResponseHead;

/// @brief `RFC 6455` §1.3 那道算例的 key：它的 `Accept` 值是协议文档写死的答案。
constexpr std::string_view AURORA_SAMPLE_KEY = "dGhlIHNhbXBsZSBub25jZQ==";

/// @brief ASCII 小写化（比对字段名用；非 ASCII 字节原样留下）。
[[nodiscard]] auto lowered(std::string_view text) -> std::string {
    std::string out;
    out.reserve(text.size());
    for (const char unit : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(unit))));
    }
    return out;
}

/// @brief 一条「服务端愿意升级」的应答头部块：状态、两枚连接字段与对本次 key 的正确摘要。
/// @param key 本次请求写出的 key（决定 `Sec-WebSocket-Accept` 的应有值）。
/// @return 可直接送进 `validate_ws_handshake` 且应当判成功的头部块。
[[nodiscard]] auto switching_head(std::string_view key = AURORA_SAMPLE_KEY) -> ResponseHead {
    ResponseHead head;
    head.status = 101;
    head.headers.set("Upgrade", "websocket");
    head.headers.set("Connection", "Upgrade");
    head.headers.set("Sec-WebSocket-Accept", ws_accept_token(key));
    return head;
}

/// @brief 剥掉某枚字段之后的应答（`Headers` 不提供公共 erase，故按条目重建）。
/// @param base 原头部块。
/// @param name 要剥掉的字段名（ASCII 大小写不敏感）。
/// @return 其余条目按原次序、原拼写重建的头部块。
[[nodiscard]] auto minus_field(const ResponseHead &base, std::string_view name) -> ResponseHead {
    ResponseHead out;
    out.status = base.status;
    out.reason = base.reason;
    out.version_minor = base.version_minor;
    const auto needle = lowered(name);
    for (const auto &entry : base.headers.entries()) {
        if (lowered(entry.name) != needle) {
            out.headers.append(entry.name, entry.value);
        }
    }
    return out;
}

/// @brief 一个目标 URL（解析失败即测试自己的前提塌了，故用 REQUIRE 交给调用方判）。
/// @param text 绝对形态文本。
/// @return 解析好的 URL。
[[nodiscard]] auto url_of(std::string_view text) -> aurora::Url {
    const auto parsed = aurora::Url::parse(text);
    AURORA_TEST_REQUIRE(parsed);
    return parsed.value();
}

/// @brief 大小写不敏感地看渲染出的头部块里有没有那枚字段名（只认行首的 `name:` 形态）。
/// @param text 渲染出的请求字节。
/// @param name 字段名（大小写随意）。
/// @return 出现为真。
[[nodiscard]] auto carries_field(std::string_view text, std::string_view name) -> bool {
    const auto lower_text = lowered(text);
    const auto needle = lowered(name) + ":";
    return lower_text.find("\r\n" + needle) != std::string::npos ||
           lower_text.find("\n" + needle) != std::string::npos || lower_text.starts_with(needle);
}

/// @brief 某枚字段在文本里出现的次数（「只有一枚」那条口径的算法）。
/// @param text 渲染出的请求字节。
/// @param field 字段的行首形态（区分大小写，按库写出的拼写给）。
/// @return 出现次数。
[[nodiscard]] auto field_count(std::string_view text, std::string_view field) -> std::size_t {
    std::size_t count = 0;
    const std::string view{text};
    std::size_t at = 0;
    while ((at = view.find(field, at)) != std::string::npos) {
        ++count;
        at += field.size();
    }
    return count;
}

/// @brief 渲染一次握手请求头部块（把出参与结局一起交出，调用方按需要求其一）。
/// @param target 目标 URL。
/// @param fields 调用方字段（与被测入口同形，只借用）。
/// @param offer 子协议与必选旗位。
/// @param absolute_form 请求目标形态。
/// @param key_out 出参：本次写出的 key。
/// @return 渲染结局。
[[nodiscard]] auto render(const aurora::Url &target, const aurora::Headers &fields, const WsHandshakeOffer &offer,
                          bool absolute_form, std::string &key_out) -> aurora::Result<std::string> {
    return render_ws_handshake(target, fields, offer, absolute_form, key_out);
}

/// @brief 一次「必须落到 `ws-handshake-rejected` 且 detail 点名某处」的判定。
/// @param head 应答头部块。
/// @param key 本次请求写出的 key。
/// @param offer 子协议与必选旗位。
/// @param needle detail 里必须出现的片段（ASCII）。
auto expect_rejected(const ResponseHead &head, std::string_view key, const WsHandshakeOffer &offer,
                     std::string_view needle) -> void {
    const auto judged = validate_ws_handshake(head, key, offer);
    AURORA_TEST_REQUIRE(!judged);
    AURORA_TEST_CHECK_EQ(judged.error().code_enum, aurora::ErrorCode::WsHandshakeRejected);
    AURORA_TEST_CHECK(judged.error().message.find(needle) != std::string::npos);
}

/// @brief 一次「必须判成功」的判定，并交出成功事实供调用方继续看分量。
/// @param head 应答头部块。
/// @param key 本次请求写出的 key。
/// @param offer 子协议与必选旗位。
/// @return 成功事实（判定不通过时该用例已就地中止）。
[[nodiscard]] auto expect_accepted(const ResponseHead &head, std::string_view key, const WsHandshakeOffer &offer)
    -> aurora::net::detail::WsHandshakeAccept {
    const auto judged = validate_ws_handshake(head, key, offer);
    AURORA_TEST_REQUIRE(judged);
    return judged.value();
}

}  // namespace

AURORA_TEST_CASE(accept_token_matches_the_published_worked_example) {
    AURORA_TEST_CHECK_EQ(ws_accept_token(AURORA_SAMPLE_KEY), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

AURORA_TEST_CASE(accept_token_carries_the_padding_across_block_boundaries) {
    // 三枚取样把 `SHA-1` 的两处分岔钉到场：材料 60 字节时 `0x80` + 零 + 位长仍在同一块；材料恰满 64 字节时
    // 填充必须溢出到第二块；材料 136 字节时主循环要走三块。只测 60 字节那一枚，「把位长写进本块末尾」与
    // 「少走一块」这两型错误都不会露头。
    AURORA_TEST_CHECK_EQ(ws_accept_token("AAAAAAAAAAAAAAAAAAAAAAAAAAAA"), "z3ycwt5uBPzmknp00S5pVy2s9vQ=");
    AURORA_TEST_CHECK_EQ(ws_accept_token(std::string(100U, 'a')), "+taVhj8zzaJPYE6HHo1ugamK/Ug=");
    // 空 key 只剩 GUID 那 36 字节：这一枚顺带把「拼接次序是 key 在前、GUID 在后」钉住（写反了它必然变）。
    AURORA_TEST_CHECK_EQ(ws_accept_token(std::string_view{}), "Kfh9QIsMVZcl6xEPYxPHzW8SZ8w=");
}

AURORA_TEST_CASE(accept_token_is_deterministic_and_key_sensitive) {
    AURORA_TEST_CHECK_EQ(ws_accept_token(AURORA_SAMPLE_KEY), ws_accept_token(AURORA_SAMPLE_KEY));
    AURORA_TEST_CHECK(ws_accept_token(AURORA_SAMPLE_KEY) != ws_accept_token("dGhlIHNhbXBsZSBub25jZw=="));
    // 摘要文本恒 28 字符（20 字节 base64 带一枚填充）；长度漂了意味着 base64 那侧的分组或填充出错。
    AURORA_TEST_CHECK_EQ(ws_accept_token(AURORA_SAMPLE_KEY).size(), 28U);
}

AURORA_TEST_CASE(the_handshake_head_writes_the_four_protocol_fields_once) {
    std::string key;
    aurora::Headers fields;
    // 调用方抢先写这四枚：并列会让对端拿到两枚 key 而无从校验，库必须覆写而不是并列。
    fields.append("Upgrade", "h2c");
    fields.append("Connection", "close");
    fields.append("Sec-WebSocket-Version", "8");
    fields.append("Sec-WebSocket-Key", "not-from-the-library");
    const auto written = render(url_of("http://example.com:9000/chat?room=1"), fields, WsHandshakeOffer{}, false, key);
    AURORA_TEST_REQUIRE(written);
    const auto &text = written.value();  // 只借用：written 活到本用例收尾
    AURORA_TEST_CHECK(text.find("GET /chat?room=1 HTTP/1.1\r\n") != std::string::npos);
    AURORA_TEST_CHECK(text.find("Host: example.com:9000\r\n") != std::string::npos);
    AURORA_TEST_CHECK(text.find("Upgrade: websocket\r\n") != std::string::npos);
    AURORA_TEST_CHECK(text.find("Connection: Upgrade\r\n") != std::string::npos);
    AURORA_TEST_CHECK(text.find("Sec-WebSocket-Version: 13\r\n") != std::string::npos);
    AURORA_TEST_CHECK(text.find("Sec-WebSocket-Key: " + key + "\r\n") != std::string::npos);
    AURORA_TEST_CHECK_EQ(text.find("not-from-the-library"), std::string::npos);
    AURORA_TEST_CHECK_EQ(text.find("h2c"), std::string::npos);
    // 四枚各只出现一次：第二次出现就是并列。
    AURORA_TEST_CHECK_EQ(field_count(text, "Upgrade:"), 1U);
    AURORA_TEST_CHECK_EQ(field_count(text, "Connection:"), 1U);
    AURORA_TEST_CHECK_EQ(field_count(text, "Sec-WebSocket-Version:"), 1U);
    AURORA_TEST_CHECK_EQ(field_count(text, "Sec-WebSocket-Key:"), 1U);
    AURORA_TEST_CHECK(text.ends_with("\r\n\r\n"));
}

AURORA_TEST_CASE(the_handshake_head_strips_every_body_form) {
    std::string key;
    aurora::Headers fields;
    fields.set("Content-Length", "5");
    fields.set("Transfer-Encoding", "chunked");
    fields.set("Content-Encoding", "gzip");
    const auto written = render(url_of("http://example.com/chat"), fields, WsHandshakeOffer{}, false, key);
    AURORA_TEST_REQUIRE(written);
    const auto &text = written.value();  // 只借用：written 活到本用例收尾
    // 握手腿发出的字节里出现任何体形态，对端都会把那几枚字节读成第一帧的开头。
    AURORA_TEST_CHECK(!carries_field(text, "content-length"));
    AURORA_TEST_CHECK(!carries_field(text, "transfer-encoding"));
    AURORA_TEST_CHECK(!carries_field(text, "content-encoding"));
    AURORA_TEST_CHECK_EQ(text.size(), text.find("\r\n\r\n") + 4U);  // 头部块之后一个字节都没有
}

AURORA_TEST_CASE(the_handshake_head_keeps_the_callers_other_fields) {
    std::string key;
    aurora::Headers fields;
    fields.set("User-Agent", "aurora/1.0");
    fields.set("Origin", "http://example.com");
    const auto written = render(url_of("http://example.com/chat"), fields, WsHandshakeOffer{}, false, key);
    AURORA_TEST_REQUIRE(written);
    const auto &text = written.value();  // 只借用：written 活到本用例收尾
    AURORA_TEST_CHECK(carries_field(text, "user-agent"));
    AURORA_TEST_CHECK(carries_field(text, "origin"));
    AURORA_TEST_CHECK(text.find("Origin: http://example.com\r\n") != std::string::npos);
}

AURORA_TEST_CASE(the_handshake_target_form_follows_the_proxy_shape) {
    std::string key;
    const auto target = url_of("http://example.com:8080/live?token=abc");
    const auto origin = render(target, aurora::Headers{}, WsHandshakeOffer{}, false, key);
    AURORA_TEST_REQUIRE(origin);
    AURORA_TEST_CHECK(origin.value().starts_with("GET /live?token=abc HTTP/1.1\r\n"));
    const auto absolute = render(target, aurora::Headers{}, WsHandshakeOffer{}, true, key);
    AURORA_TEST_REQUIRE(absolute);
    // 转发型代理要的是绝对形态目标：它据此决定把这条请求交给谁，origin-form 只会让它按 `Host` 猜。
    AURORA_TEST_CHECK(absolute.value().starts_with("GET http://example.com:8080/live?token=abc HTTP/1.1\r\n"));
}

AURORA_TEST_CASE(the_handshake_key_is_sixteen_drawn_bytes_in_base64) {
    AURORA_TEST_CHECK_EQ(AURORA_WS_KEY_BYTES, 16U);
    std::set<std::string> seen;
    for (std::size_t round = 0; round < 50U; ++round) {
        std::string key;
        const auto written =
            render(url_of("http://example.com/chat"), aurora::Headers{}, WsHandshakeOffer{}, false, key);
        AURORA_TEST_REQUIRE(written);
        AURORA_TEST_REQUIRE_EQ(key.size(), 24U);  // 16 字节 → 22 枚字符加两枚填充
        AURORA_TEST_CHECK(key.ends_with("=="));
        AURORA_TEST_CHECK_EQ(key.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/="),
                             std::string::npos);
        // 写进请求头部的正是这一枚：出参与线上文本对不上时，判侧的比对就成了一次无意义的自证。
        AURORA_TEST_CHECK(written.value().find("Sec-WebSocket-Key: " + key + "\r\n") != std::string::npos);
        AURORA_TEST_CHECK_EQ(field_count(written.value(), "Sec-WebSocket-Key:"), 1U);
        seen.insert(key);
    }
    // 五十枚逐枚互异：16 字节空间里相撞一次的可能性本就可以忽略，真撞了说明这一枚压根不是每次新取的。
    AURORA_TEST_CHECK_EQ(seen.size(), 50U);
}

AURORA_TEST_CASE(the_handshake_head_reports_http_request_rejection_unchanged) {
    std::string key;
    aurora::Headers fields;
    fields.set("Bad Name", "value");  // 字段名带空格：不是 token，`wire` 那侧起手就拒
    const auto written = render(url_of("http://example.com/chat"), fields, WsHandshakeOffer{}, false, key);
    AURORA_TEST_REQUIRE(!written);
    // 这里不新造握手码：请求本身不合 HTTP 文法与「对端不肯升级」是两件事，混成一件会让排查方向整个反掉。
    AURORA_TEST_CHECK_EQ(written.error().code_enum, aurora::ErrorCode::HttpRequestInvalid);
}

AURORA_TEST_CASE(subprotocols_are_offered_in_order_as_one_list) {
    std::string key;
    WsHandshakeOffer offer;
    offer.subprotocols = {"chat", "superchat", "echo"};
    offer.require_subprotocol = true;
    const auto written = render(url_of("http://example.com/chat"), aurora::Headers{}, offer, false, key);
    AURORA_TEST_REQUIRE(written);
    AURORA_TEST_CHECK(written.value().find("Sec-WebSocket-Protocol: chat, superchat, echo\r\n") != std::string::npos);
    AURORA_TEST_CHECK_EQ(field_count(written.value(), "Sec-WebSocket-Protocol:"), 1U);
    std::string bare_key;
    const auto bare = render(url_of("http://example.com/chat"), aurora::Headers{}, WsHandshakeOffer{}, false, bare_key);
    AURORA_TEST_REQUIRE(bare);
    // 一枚都没提时整枚字段不出现：写一枚空值等于宣告「我接受一枚空名字的孩子协议」。
    AURORA_TEST_CHECK(!carries_field(bare.value(), "Sec-WebSocket-Protocol"));
}

AURORA_TEST_CASE(a_response_that_is_not_switching_protocols_is_refused_by_status) {
    const std::vector<std::uint16_t> statuses{200U, 426U, 401U, 503U};
    for (const std::uint16_t status : statuses) {
        auto head = switching_head();
        head.status = status;
        expect_rejected(head, AURORA_SAMPLE_KEY, WsHandshakeOffer{},
                        std::to_string(status) + " instead of switching protocols");
    }
}

AURORA_TEST_CASE(the_upgrade_field_is_required_and_read_case_insensitively) {
    expect_rejected(minus_field(switching_head(), "Upgrade"), AURORA_SAMPLE_KEY, WsHandshakeOffer{},
                    "no Upgrade field");
    auto wrong = switching_head();
    wrong.headers.set("Upgrade", "h2c");
    expect_rejected(wrong, AURORA_SAMPLE_KEY, WsHandshakeOffer{}, "not websocket");
    for (const std::string_view shape : {"WebSocket", "  websocket\t", "WEBSOCKET"}) {
        auto head = switching_head();
        head.headers.set("Upgrade", shape);
        // 大小写与首尾 OWS 都不该改变判定：字段值的大小写按 HTTP 是自由的。
        (void)expect_accepted(head, AURORA_SAMPLE_KEY, WsHandshakeOffer{});
    }
}

AURORA_TEST_CASE(connection_is_read_as_a_token_list_not_a_substring_or_a_gap) {
    for (const std::string_view shape : {"Upgrade", "upgrade", "keep-alive, Upgrade", "404-not-found, UPGRADE"}) {
        auto head = switching_head();
        head.headers.set("Connection", shape);
        (void)expect_accepted(head, AURORA_SAMPLE_KEY, WsHandshakeOffer{});  // 同一行还写着别的 token 不影响
    }
    expect_rejected(minus_field(switching_head(), "Connection"), AURORA_SAMPLE_KEY, WsHandshakeOffer{},
                    "does not carry the upgrade token");
    auto close_only = switching_head();
    close_only.headers.set("Connection", "close");
    expect_rejected(close_only, AURORA_SAMPLE_KEY, WsHandshakeOffer{}, "does not carry the upgrade token");
    // 只以空白分隔的两个词不是一条列表（`RFC 9110` 的 list 元素由逗号分隔）：按子串放行会把
    // 「keep-alive upgrade」这种根本不合文法的写法当成同意升级。
    auto glued = switching_head();
    glued.headers.set("Connection", "keep-alive upgrade");
    expect_rejected(glued, AURORA_SAMPLE_KEY, WsHandshakeOffer{}, "does not carry the upgrade token");
    // 分两行写同一条列表也照样成立：`Connection` 是多值字段。
    auto split = switching_head();
    split.headers.append("Connection", "keep-alive");
    split.headers.append("Connection", "upgrade");
    (void)expect_accepted(split, AURORA_SAMPLE_KEY, WsHandshakeOffer{});
}

AURORA_TEST_CASE(the_accept_field_must_match_this_requests_key) {
    expect_rejected(minus_field(switching_head(), "Sec-WebSocket-Accept"), AURORA_SAMPLE_KEY, WsHandshakeOffer{},
                    "no Sec-WebSocket-Accept field");
    auto wrong = switching_head();
    wrong.headers.set("Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOr=");
    expect_rejected(wrong, AURORA_SAMPLE_KEY, WsHandshakeOffer{}, "does not match the value");
    // 另一枚 key 算出的正确摘要：对**它自己**那枚 key 是对的，对本次请求是错的——本库必须拿本次 key 比。
    expect_rejected(switching_head("dGhlIHNhbXBsZSBub25jZw=="), AURORA_SAMPLE_KEY, WsHandshakeOffer{},
                    "does not match the value");
    for (const std::string_view shape :
         {" s3pPLMBiTxaQ9kYGzzhZRbK+xOo= ", "s3pPLMBiTxaQ9kYGzzhZRbK+\r\n xOo=", "\ts3pPLMBiTxaQ9kYGzzhZRbK+xOo=\t"}) {
        auto head = switching_head();
        head.headers.set("Sec-WebSocket-Accept", shape);
        (void)expect_accepted(head, AURORA_SAMPLE_KEY, WsHandshakeOffer{});  // 折行与空白是书写自由，不是另一枚摘要
    }
}

AURORA_TEST_CASE(an_unrequested_extension_is_refused_by_name) {
    auto head = switching_head();
    head.headers.set("Sec-WebSocket-Extensions", "permessage-deflate; client_max_window_bits");
    const auto judged = validate_ws_handshake(head, AURORA_SAMPLE_KEY, WsHandshakeOffer{});
    AURORA_TEST_REQUIRE(!judged);
    AURORA_TEST_CHECK_EQ(judged.error().code_enum, aurora::ErrorCode::WsHandshakeRejected);
    AURORA_TEST_CHECK(judged.error().message.find("announced an extension") != std::string::npos);
    // 那行文本里要带着对端宣告的名字：只说「有扩展」等于把「哪一枚扩展」这一条排查线索丢掉。
    AURORA_TEST_CHECK(judged.error().message.find("permessage-deflate") != std::string::npos);
}

AURORA_TEST_CASE(a_returned_subprotocol_must_be_one_of_the_offered) {
    WsHandshakeOffer offer;
    offer.subprotocols = {"chat", "echo"};
    auto head = switching_head();
    head.headers.set("Sec-WebSocket-Protocol", "echo");
    AURORA_TEST_CHECK_EQ(expect_accepted(head, AURORA_SAMPLE_KEY, offer).subprotocol, "echo");
    auto invented = switching_head();
    invented.headers.set("Sec-WebSocket-Protocol", "whois");
    expect_rejected(invented, AURORA_SAMPLE_KEY, offer, "never offered");
    // 多枚一起回：每一枚都得在提出过的列表里，事实记第一枚（那是对端的主选择）。
    auto several = switching_head();
    several.headers.set("Sec-WebSocket-Protocol", "chat, echo");
    AURORA_TEST_CHECK_EQ(expect_accepted(several, AURORA_SAMPLE_KEY, offer).subprotocol, "chat");
    auto half_ok = switching_head();
    half_ok.headers.set("Sec-WebSocket-Protocol", "chat, whois");
    expect_rejected(half_ok, AURORA_SAMPLE_KEY, offer, "never offered");
    // 一枚都没提却回了子协议：那枚值必然不在空列表里，属于「擅自换协议」那一型。
    auto unoffered = switching_head();
    unoffered.headers.set("Sec-WebSocket-Protocol", "chat");
    expect_rejected(unoffered, AURORA_SAMPLE_KEY, WsHandshakeOffer{}, "never offered");
}

AURORA_TEST_CASE(absent_subprotocol_is_success_only_when_not_required) {
    WsHandshakeOffer optional;
    optional.subprotocols = {"chat"};
    // 服务端没回子协议是合法的降级结局，不是失败；事实里那一枚为空。
    AURORA_TEST_CHECK_EQ(expect_accepted(switching_head(), AURORA_SAMPLE_KEY, optional).subprotocol, "");
    WsHandshakeOffer required;
    required.subprotocols = {"chat"};
    required.require_subprotocol = true;
    expect_rejected(switching_head(), AURORA_SAMPLE_KEY, required, "requires one of the offered subprotocols");
}

AURORA_TEST_CASE(every_rejection_names_its_own_cause) {
    WsHandshakeOffer required;
    required.require_subprotocol = true;
    auto extensions = switching_head();
    extensions.headers.set("Sec-WebSocket-Extensions", "permessage-deflate");
    auto by_status = switching_head();
    by_status.status = 503;
    const std::vector<ResponseHead> scenes{by_status,
                                           minus_field(switching_head(), "Upgrade"),
                                           minus_field(switching_head(), "Connection"),
                                           minus_field(switching_head(), "Sec-WebSocket-Accept"),
                                           extensions,
                                           switching_head()};
    std::vector<std::string> causes;
    for (std::size_t index = 0; index < scenes.size(); ++index) {
        const auto judged = validate_ws_handshake(scenes[index], AURORA_SAMPLE_KEY,
                                                  index + 1U == scenes.size() ? required : WsHandshakeOffer{});
        AURORA_TEST_REQUIRE(!judged);
        AURORA_TEST_CHECK_EQ(judged.error().code_enum, aurora::ErrorCode::WsHandshakeRejected);
        causes.push_back(judged.error().message);
    }
    AURORA_TEST_REQUIRE_EQ(causes.size(), 6U);
    for (std::size_t first = 0; first < causes.size(); ++first) {
        AURORA_TEST_CHECK(!causes[first].empty());
        for (std::size_t second = first + 1; second < causes.size(); ++second) {
            AURORA_TEST_CHECK(causes[first] != causes[second]);
        }
    }
}

}  // namespace aurora::test_cases::utest_net_ws_handshake
