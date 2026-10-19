/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/http_sse.*（解析器）+ include/aurora/net/http_sse.h 与 Client::open_sse
///           （src/aurora/net/http_sse.cpp）
/// 测试说明: `text/event-stream` 的离线回归，分两层。
///           **解析器层**（纯字节、零 socket、零时钟）验九条只有增量解析才成立的口径：三型行终止（CR / CRLF /
///           LF，含一枚终止符横跨两段）、流首 BOM（含 BOM 横跨两段）、多行 `data` 以 LF 连接、冒号后只剥一枚
///           空格、未知字段忽略而非报错、数据缓冲为空不派发且事件类型清回去、含 NUL 的 `id` 整条忽略、
///           `retry` 只认纯 ASCII 数字、未以空行收尾的那条事件随收摊丢弃。两枚内置上限各证一次，并验终态
///           重复交出同一枚错误。非法 UTF-8 字节按原文留在字段值里（本层不改写载荷）。
///           **订阅层**（ScriptedPeer 在回环上按脚本应答真实字节）验只有跨交换会话才成立的事实：一次调用交出
///           的完成事实带着流条数 / 事件数 / 线上字节 / 续传标识；`SinkFlow::Stop` 是成功结局且恰一次 `on_end`；
///           非 GET 起手即 `http-request-invalid` 且对端一条连接都没连上；非 2xx 与内容类型不合格是**终止失败**
///           （不重连，`accepts()` 恒 1）；干净收摊之后带 `Last-Event-ID` 再接一条，且第一条不带；额度用尽
///           交出的是最后一次交换那枚真实失败并在文本里点名额度，而干净收摊那种仍算成功（`reconnect_exhausted`）；
///           重连窗口取服务端写过的 `retry:`（对照组：服务端没写时用本地初值，那条窗口在预算内不到期）；
///           取消掐在途那条流、已交付的事件不回滚且不再重连；本腿无流式能力落 `net-platform-unavailable`；
///           缓存既不查也不写（表里有新鲜副本也照样触网，订阅的结果也不进表）；`Accept` 的缺省宣告与
///           「调用方写过就不动」。
///           全用例零真实远端、零 DNS（目标恒为数字字面量），故禁网 CI 无需豁免；浏览器腿没有 BSD socket
///           语义，订阅层按 `AURORA_TEST_SKIP` 登记。

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/net/blocking.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/capabilities.h"
#include "aurora/net/detail/http_sse.h"
#include "aurora/net/http.h"
#include "aurora/net/http_client.h"
#include "aurora/net/http_sse.h"
#include "aurora/net/http_stream.h"
#include "aurora/net/policy.h"
#include "aurora/state/coroutine.h"
#include "framework/async_wait.h"
#include "framework/aurora_test.h"
#include "framework/fake_engine.h"
#include "framework/net_wait.h"
#include "framework/scripted_peer.h"

namespace aurora::test_cases::utest_net_http_sse {
namespace {

using aurora::CoroTask;
using aurora::Result;
using aurora::net::Cancellation;
using aurora::net::Capabilities;
using aurora::net::NetPolicy;
using aurora::net::detail::AURORA_SSE_MAX_EVENT_BYTES;
using aurora::net::detail::AURORA_SSE_MAX_LINE_BYTES;
using aurora::net::detail::SseParsedEvent;
using aurora::net::detail::SseParserSinks;
using aurora::net::detail::SseStreamParser;
using aurora::net::http::CacheOptions;
using aurora::net::http::Client;
using aurora::net::http::Request;
using aurora::net::http::Response;
using aurora::net::http::SinkFlow;
using aurora::net::http::SseEvent;
using aurora::net::http::SseOptions;
using aurora::net::http::SseOutcome;
using aurora::net::http::SseSink;
using aurora::test::FakeEngine;
using aurora::test::FakeEngineScope;
using aurora::test::PeerScript;
using aurora::test::PeerStop;
using aurora::test::ScriptedPeer;
using aurora::test::scripts::per_connection;
using aurora::test::scripts::read_head_then_reset;
using aurora::test::scripts::reply_after_reading;
using aurora::test::scripts::write_then_hold;

/// @brief 拼一个指向回环端口的 GET 目标。
[[nodiscard]] auto loopback_url(std::uint16_t port, std::string_view path = "/") -> std::string {
    return "http://127.0.0.1:" + std::to_string(port) + std::string{path};
}

/// @brief 取协程结果的外层（协程本身不抛，外层恒成功）。
[[nodiscard]] auto inner(CoroTask<Result<SseOutcome>> &task) -> Result<SseOutcome> {
    const auto outer = task.result();
    AURORA_TEST_REQUIRE_MSG(bool(outer), outer ? std::string{} : outer.error().message);
    return outer.value();
}

/// @brief 关掉重连的订阅口径：单流用例要的正是「一条流就是全部」，缺省那枚开关会把它变成两次交换。
[[nodiscard]] auto single_stream() -> SseOptions {
    SseOptions options;
    options.reconnect = false;
    return options;
}

/// @brief 立刻重连的订阅口径：窗口判据另有专责用例，其余重连用例不该为一次真实睡眠买单。
[[nodiscard]] auto immediate_reconnect(std::uint32_t max_reconnects) -> SseOptions {
    SseOptions options;
    options.max_reconnects = max_reconnects;
    options.retry_default = std::chrono::milliseconds{0};
    return options;
}

/// @brief 把一份事件流载荷按分块上线：块边界与消息边界错位才是「增量」的形态。
/// @param fields 头部块里除帧型与内容类型之外的字段（逐条以 CRLF 收尾，可为空）。
/// @param body 事件流正文（自带 CR / LF 形态）。
/// @return 完整应答字节（含零块收尾：对端说完这件事有确定形态，不靠关闭套接口）。
[[nodiscard]] auto event_stream(std::string_view fields, std::string_view body) -> std::string {
    std::string size_text;
    std::size_t remaining = body.size();
    do {
        const int digit = static_cast<int>(remaining % 16);
        const char glyph = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
        size_text.insert(size_text.begin(), glyph);
        remaining /= 16;
    } while (remaining != 0);
    return "HTTP/1.1 200 OK\r\n" + std::string{fields} +
           "Content-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n" + size_text + "\r\n" +
           std::string{body} + "\r\n0\r\n\r\n";
}

/// @brief 解析器的观测面：四个出口各记一份，宿主的收摊答复可由用例打开。
///
/// 成员公开是为了让断言直读；本类型只在用例线程被驱动（解析器同步调用出口），无并发面。
struct Recorder {
    std::vector<std::string> types;  ///< 每条派发事件的事件类型
    std::vector<std::string> datas;  ///< 每条派发事件的消息体
    std::vector<std::string> comments;  ///< 每条注释行的正文
    std::vector<std::string> ids;  ///< 每次 `id` 字段效应交出的值（含空值）
    std::optional<std::chrono::milliseconds> retry;  ///< 最后一次 `retry` 字段效应
    int retry_reports{0};  ///< `retry` 出口被叫到的次数
    bool accept_events{true};  ///< 假 = 宿主从第一条起就收摊
    int decline_at{0};  ///< 从第几条事件起收摊（1 基：1 = 第一条就收摊，0 = 从不收摊）

    /// @brief 绑定到本记录器的四个出口。
    /// @return 出口束。
    [[nodiscard]] auto sinks() -> SseParserSinks {
        return SseParserSinks{
            .on_event = [this](const SseParsedEvent &event) -> bool {
                const std::size_t index = types.size();
                types.push_back(event.type);
                datas.push_back(event.data);
                // `decline_at` 是 1 基的「第几条起收摊」，正数才有意义；换成无符号比较口径后，判据与它同源。
                return decline_at > 0 && index + 1 >= static_cast<std::size_t>(decline_at) ? false : accept_events;
            },
            .on_comment = [this](std::string_view text) -> void { comments.emplace_back(text); },
            .on_last_id = [this](const std::string &value) -> void { ids.push_back(value); },
            .on_retry = [this](std::chrono::milliseconds window) -> void {
                ++retry_reports;
                retry = window;
            },
        };
    }
};

/// @brief 把若干段字节喂进一枚新解析器，必要时宣告输入结束。
/// @param recorder 观测面。
/// @param segments 依次喂入的段（段与段的边界由用例决定，跨段形态正是本层要证的）。
/// @param close_input 是否在喂完后叫 `finish()`。
/// @return 第一个非成功的结局；全程顺利则为成功。
[[nodiscard]] auto feed(Recorder &recorder, const std::vector<std::string> &segments, bool close_input = true)
    -> Result<void> {
    SseStreamParser parser{recorder.sinks()};
    for (const std::string &segment : segments) {
        auto pushed = parser.push(std::string_view{segment});
        if (!pushed) {
            return pushed;
        }
    }
    if (close_input) {
        return parser.finish();
    }
    return Result<void>{};
}

/// @brief 记录逐条交付事实的订阅侧宿主替身：四个入口各自的次数、拼出的事件、以及在哪一条收摊。
class RecordingSink final : public SseSink {
  public:
    /// @brief 记一次建流（重连之后每条流各一次）。
    /// @param head 头部块。
    /// @return 按 `stop_at_open` 决定收摊或继续。
    auto on_open(const Response &head) -> SinkFlow override {
        ++opens;
        status = head.status();
        head_body_empty = head.body().empty();
        return stop_at_open ? SinkFlow::Stop : SinkFlow::Continue;
    }

    /// @brief 记一条事件，按 `stop_after` 决定何时收摊。
    /// @param event 本条事件（视图只在本次调用内有效，故全部拷贝）。
    /// @return 继续或收摊。
    auto on_event(const SseEvent &event) -> SinkFlow override {
        ++events;
        types.emplace_back(event.type);
        datas.emplace_back(event.data);
        ids.emplace_back(event.id);
        if (events >= stop_after) {
            stopped = true;
            return SinkFlow::Stop;
        }
        return SinkFlow::Continue;
    }

    /// @brief 记一行注释。
    /// @param text 注释正文。
    auto on_comment(std::string_view text) -> void override {
        ++comments;
        texts.emplace_back(text);
    }

    /// @brief 记一次订阅收尾宣告。
    auto on_end() -> void override { ++ends; }

    int opens{0};  ///< `on_open` 次数
    int events{0};  ///< `on_event` 次数
    int ends{0};  ///< `on_end` 次数
    int comments{0};  ///< `on_comment` 次数
    std::vector<std::string> types;  ///< 按交付次序的事件类型
    std::vector<std::string> datas;  ///< 按交付次序的消息体
    std::vector<std::string> ids;  ///< 每条事件派发时刻的续传标识
    std::vector<std::string> texts;  ///< 按到达次序的注释正文
    std::uint16_t status{0};  ///< 最后一次建流的状态码
    bool head_body_empty{false};  ///< 建流交付头部时 `body()` 是否为空
    bool stopped{false};  ///< 本宿主是否回出过 `Stop`
    bool stop_at_open{false};  ///< 是否在第一条流建立时就收摊
    int stop_after{std::numeric_limits<int>::max()};  ///< 第几条事件起收摊
};

/// @brief 为测试放行明文：缺省安全策略拒绝一切未列入白名单的 `http://` 目标，而本套件的对端恒在回环地址上。
/// @param client 待配置的客户端。
auto allow_test_cleartext(Client &client) -> void {
    NetPolicy permissive;
    permissive.allow_cleartext_hosts = {"127.0.0.1"};
    client.policy(std::move(permissive));
}

/// @brief 发起一次订阅并泵到收尾。
/// @param client 客户端。
/// @param request 请求。
/// @param sink 宿主替身。
/// @param token 取消令牌。
/// @param budget 收尾预算（重连与窗口用例各要各的：默认 3 s 足够覆盖两次回环交换）。
/// @return 订阅的出口结果（协程未收尾时由 REQUIRE 收场）。
[[nodiscard]] auto subscribe_and_drive(const Client &client, Request request, SseSink &sink, Cancellation token = {},
                                       std::chrono::milliseconds budget = std::chrono::milliseconds{3000})
    -> Result<SseOutcome> {
    const aurora::test::net::HostDriven host;
    auto task = client.open_sse(std::move(request), sink, std::move(token));
    AURORA_TEST_REQUIRE_MSG(aurora::test::net::drive_until(
                                "open_sse done", [&task] { return task.is_done(); }, budget)
                                .satisfied,
                            "the subscription never settled inside the wait budget");
    return inner(task);
}

/// @brief 读满请求头、把请求记进交接缓冲、随后按给定应答收摊的对端脚本。
/// @param recorded 每条连接记下的一份请求头部块（脚本线程写入）。
/// @param guard 交接缓冲的锁。
/// @param payload 要写出的应答字节。
/// @return 对端脚本。
[[nodiscard]] auto record_head_then_reply(std::vector<std::string> &recorded, std::mutex &guard, std::string payload)
    -> PeerScript {
    return [&recorded, &guard, payload = std::move(payload)](aurora::net::blocking::StreamSocket &socket,
                                                             PeerStop &) -> void {
        (void)socket.set_io_timeout(std::chrono::seconds{10});
        std::vector<char> buffer(4096);
        std::string seen;
        while (seen.find("\r\n\r\n") == std::string::npos) {
            const auto read = socket.read_some(std::span<char>{buffer});
            if (!read || read.value() == 0) {
                break;  // 请求不会更完整了：按手上的字节应答
            }
            seen.append(buffer.data(), read.value());
        }
        {
            const std::scoped_lock held(guard);
            recorded.push_back(seen);
        }
        (void)socket.write_all(std::span<const char>{payload});
    };
}

/// @brief 请求头部块里是否出现某段子串（大小写敏感：字段名与取值都由发起侧写法决定）。
/// @param recorded 全部已记录的请求。
/// @param guard 交接缓冲的锁。
/// @param index 第几条连接（从 0 起）。
/// @param needle 要找的子串。
/// @return 命中为 true；那条请求还不存在时同样为 false。
[[nodiscard]] auto request_contains(const std::vector<std::string> &recorded, std::mutex &guard, std::size_t index,
                                    std::string_view needle) -> bool {
    const std::scoped_lock held(guard);
    return index < recorded.size() && recorded[index].find(needle) != std::string::npos;
}

}  // namespace

AURORA_TEST_CASE(sse_parser_delivers_a_single_event_with_the_default_type) {
    Recorder recorder;
    const auto parsed = feed(recorder, {"data: hello\n\n"});
    AURORA_TEST_REQUIRE_MSG(bool(parsed), parsed ? std::string{} : parsed.error().message);
    AURORA_TEST_CHECK_EQ(recorder.types.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(recorder.types.at(0), "message");  // 服务端没写 `event:` 时的规范折值
    AURORA_TEST_CHECK_EQ(recorder.datas.at(0), "hello");
}

AURORA_TEST_CASE(sse_parser_joins_multiple_data_lines_with_lf) {
    Recorder recorder;
    const auto parsed = feed(recorder, {"data: first\ndata: second\ndata:\n\n"});
    AURORA_TEST_REQUIRE_MSG(bool(parsed), parsed ? std::string{} : parsed.error().message);
    AURORA_TEST_CHECK_EQ(recorder.datas.size(), std::size_t{1});
    // 三行 `data` 折成一条消息体：LF 是连接符而不是第二条事件；空的那行也参与（尾部多一枚 LF）。
    AURORA_TEST_CHECK_EQ(recorder.datas.at(0), "first\nsecond\n");
}

AURORA_TEST_CASE(sse_parser_accepts_all_three_line_terminators) {
    Recorder recorder;
    // 裸 CR、CRLF、LF 三型同时在场，且最后那条空行用 CR 收尾：`Framer` 的行型不认第一型，本层必须认。
    const auto parsed = feed(recorder, {"event: tick\rdata: a\r\ndata: b\n\n"});
    AURORA_TEST_REQUIRE_MSG(bool(parsed), parsed ? std::string{} : parsed.error().message);
    AURORA_TEST_CHECK_EQ(recorder.types.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(recorder.types.at(0), "tick");
    AURORA_TEST_CHECK_EQ(recorder.datas.at(0), "a\nb");
}

AURORA_TEST_CASE(sse_parser_pairs_a_crlf_split_across_two_segments_into_one_terminator) {
    Recorder recorder;
    // CR 落在第一段尾、LF 落在第二段首：这是同一枚终止符的两半，不是一条空行。判据若松，宿主会多收到
    // 一条凭空出现的派发（甚至两条事件）。
    const auto first = feed(recorder, {"data: a\r", "\ndata: b\n\n"});
    AURORA_TEST_REQUIRE_MSG(bool(first), first ? std::string{} : first.error().message);
    AURORA_TEST_CHECK_EQ(recorder.types.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(recorder.datas.at(0), "a\nb");
}

AURORA_TEST_CASE(sse_parser_dispatches_only_when_the_data_buffer_has_bytes) {
    Recorder recorder;
    // 注释之后的一条空行不派发；只有 `event:` / `id:` 的那一段空行也不派发（那是心跳的常见形态）。
    const auto parsed = feed(recorder, {": ping\n\n", "event: keepalive\nid: 3\n\n", "data: real\n\n"});
    AURORA_TEST_REQUIRE_MSG(bool(parsed), parsed ? std::string{} : parsed.error().message);
    AURORA_TEST_CHECK_EQ(recorder.types.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(recorder.datas.at(0), "real");
    AURORA_TEST_CHECK_EQ(recorder.comments.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(recorder.comments.at(0), "ping");
    // 没派发的那一段把事件类型缓冲清了回去：否则下一条会继承一个服务端从没打算发的类型名。
    AURORA_TEST_CHECK_EQ(recorder.types.at(0), "message");
    AURORA_TEST_CHECK_EQ(recorder.ids.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(recorder.ids.at(0), "3");  // 字段效应即时上报，不跟着派发
}

AURORA_TEST_CASE(sse_parser_strips_one_leading_space_after_the_colon) {
    Recorder recorder;
    const auto parsed = feed(recorder, {"data:  x\n\n"});  // 冒号后两枚空格：第一枚是分隔符，第二枚是正文
    AURORA_TEST_REQUIRE_MSG(bool(parsed), parsed ? std::string{} : parsed.error().message);
    AURORA_TEST_CHECK_EQ(recorder.datas.at(0), " x");

    Recorder valueless;
    const auto bare = feed(valueless, {"data\n\n"});  // 没带冒号的字段行：字段名整行、值为空
    AURORA_TEST_REQUIRE_MSG(bool(bare), bare ? std::string{} : bare.error().message);
    AURORA_TEST_CHECK_EQ(valueless.datas.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(valueless.datas.at(0), "");  // 空值也参与连接：那条事件是「有一行 data」
}

AURORA_TEST_CASE(sse_parser_ignores_unknown_fields_instead_of_refusing_the_event) {
    Recorder recorder;
    // 未知字段是 `text/event-stream` 的向后兼容机制：服务端将来加什么字段，本层无权替它作废整条事件。
    const auto parsed = feed(recorder, {"trace-id: 42\ndata: x\n\n"});
    AURORA_TEST_REQUIRE_MSG(bool(parsed), parsed ? std::string{} : parsed.error().message);
    AURORA_TEST_CHECK_EQ(recorder.types.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(recorder.datas.at(0), "x");
    AURORA_TEST_CHECK_EQ(recorder.ids.size(), std::size_t{0});
}

AURORA_TEST_CASE(sse_parser_drops_an_id_field_containing_a_nul) {
    Recorder recorder;
    // 含 NUL 的 `id` 整条忽略（规范原文是 ignore the field）：续传标识里塞进 NUL 之后线上没法表达。
    // 字面量里那枚 `\0` 会把 C 字符串截断，故按数组长度构造——否则这条用例喂给解析器的根本不是它想测的字节。
    static constexpr char POLLUTED_RAW[] = "id: a\0b\ndata: x\n\n";
    const std::string polluted{POLLUTED_RAW, std::size(POLLUTED_RAW) - std::size_t{1}};
    const auto parsed = feed(recorder, {polluted, "id: 9\ndata: y\n\n"});
    AURORA_TEST_REQUIRE_MSG(bool(parsed), parsed ? std::string{} : parsed.error().message);
    AURORA_TEST_CHECK_EQ(recorder.ids.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(recorder.ids.at(0), "9");
}

AURORA_TEST_CASE(sse_parser_adopts_a_retry_field_only_when_it_is_all_digits) {
    Recorder recorder;
    const auto parsed = feed(recorder, {"retry: 1500\ndata: x\n\n", "retry: later\ndata: y\n\n"});
    AURORA_TEST_REQUIRE_MSG(bool(parsed), parsed ? std::string{} : parsed.error().message);
    AURORA_TEST_CHECK_EQ(recorder.retry_reports, 1);  // 非数字那行「当没给」：不报错也不上报
    AURORA_TEST_CHECK(recorder.retry.has_value());
    AURORA_TEST_CHECK_EQ(recorder.retry.value_or(std::chrono::milliseconds{-1}).count(), 1500);

    Recorder empty;
    const auto blank = feed(empty, {"retry: \ndata: x\n\n"});
    AURORA_TEST_REQUIRE_MSG(bool(blank), blank ? std::string{} : blank.error().message);
    AURORA_TEST_CHECK_EQ(empty.retry_reports, 0);
}

AURORA_TEST_CASE(sse_parser_strips_a_byte_order_mark_even_across_segments) {
    Recorder direct;
    const auto whole = feed(direct, {"\xEF\xBB\xBF"
                                     "data: x\n\n"});
    AURORA_TEST_REQUIRE_MSG(bool(whole), whole ? std::string{} : whole.error().message);
    AURORA_TEST_CHECK_EQ(direct.datas.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(direct.datas.at(0), "x");

    Recorder split;
    // BOM 横跨两段：判不出来的那两枚字节必须挂住，否则第一段会被切成一行假字段，宿主收到一条假事件。
    const auto halves = feed(split, {"\xEF\xBB",
                                     "\xBF"
                                     "data: y\n\n"});
    AURORA_TEST_REQUIRE_MSG(bool(halves), halves ? std::string{} : halves.error().message);
    AURORA_TEST_CHECK_EQ(split.datas.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(split.datas.at(0), "y");
}

AURORA_TEST_CASE(sse_parser_discards_an_event_the_peer_never_finished) {
    Recorder recorder;
    // 服务端写完 `data:` 那行就断了：那条事件没有空行收尾，库不补发半条消息。
    const auto parsed = feed(recorder, {"data: half\n"});
    AURORA_TEST_REQUIRE_MSG(bool(parsed), parsed ? std::string{} : parsed.error().message);
    AURORA_TEST_CHECK_EQ(recorder.types.size(), std::size_t{0});

    Recorder no_terminator;
    const auto tailless = feed(no_terminator, {"data: x"});  // 末行连终止符都没有：结算成一行，仍不派发
    AURORA_TEST_REQUIRE_MSG(bool(tailless), tailless ? std::string{} : tailless.error().message);
    AURORA_TEST_CHECK_EQ(no_terminator.types.size(), std::size_t{0});
}

AURORA_TEST_CASE(sse_parser_reports_the_builtin_line_limit_once_and_keeps_reporting_it) {
    Recorder recorder;
    const std::string too_long(AURORA_SSE_MAX_LINE_BYTES + std::size_t{2}, 'a');
    const auto first = feed(recorder, {too_long + "\n"}, false);
    AURORA_TEST_REQUIRE_FALSE(bool(first));
    AURORA_TEST_CHECK_EQ(first.error().code_enum, aurora::ErrorCode::NetFramerOverflow);
    AURORA_TEST_CHECK_NE(first.error().code_enum, aurora::ErrorCode::NetBufferLimit);  // 越的是哪一枚要说得清
    AURORA_TEST_CHECK(first.error().message.find("1 MiB") != std::string::npos);

    // 终态之后重复喂交出同一枚错误：那批字节属于谁已经不可知，继续切行只会造出更多假事件。
    SseStreamParser parser{recorder.sinks()};
    const auto again = parser.push(std::string_view{too_long});
    AURORA_TEST_REQUIRE_FALSE(bool(again));
    const auto second = parser.push("data: x\n\n");
    AURORA_TEST_REQUIRE_FALSE(bool(second));
    AURORA_TEST_CHECK_EQ(second.error().code_enum, again.error().code_enum);
    AURORA_TEST_CHECK_EQ(recorder.types.size(), std::size_t{0});
}

AURORA_TEST_CASE(sse_parser_reports_the_builtin_record_limit) {
    Recorder recorder;
    // 每条 `data` 行都在单行上限之内，之和越过单条记录的缓冲：越的是 `net-buffer-limit` 而不是行上限。
    const std::string line = "data: " + std::string(AURORA_SSE_MAX_LINE_BYTES - std::size_t{32}, 'x') + "\n";
    std::vector<std::string> segments;
    segments.reserve(7);  // 六行 `data` 加一条空行收尾：一次留够，环里不反复扩容
    for (int index = 0; index < 6; ++index) {
        segments.emplace_back(line);
    }
    segments.emplace_back("\n");
    const auto parsed = feed(recorder, segments, false);
    AURORA_TEST_REQUIRE_FALSE(bool(parsed));
    AURORA_TEST_CHECK_EQ(parsed.error().code_enum, aurora::ErrorCode::NetBufferLimit);
    AURORA_TEST_CHECK(parsed.error().message.find("4 MiB") != std::string::npos);
    AURORA_TEST_CHECK_EQ(recorder.types.size(), std::size_t{0});
}

AURORA_TEST_CASE(sse_parser_hands_over_bytes_it_cannot_decode) {
    Recorder recorder;
    // 事件流的字节属于载荷：本层不替换非法序列、也不报错（charset 解码是物化路径 `Response::text()` 的事）。
    const std::string raw{"data: \xFF\xFE\n\n"};
    const auto parsed = feed(recorder, {raw});
    AURORA_TEST_REQUIRE_MSG(bool(parsed), parsed ? std::string{} : parsed.error().message);
    AURORA_TEST_CHECK_EQ(recorder.datas.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(recorder.datas.at(0), "\xFF\xFE");
}

AURORA_TEST_CASE(sse_parser_stops_dispatching_after_the_host_declines) {
    Recorder recorder;
    recorder.decline_at = 1;  // 第一条起宿主收摊
    const auto parsed = feed(recorder, {"data: a\n\ndata: b\n\n"});
    AURORA_TEST_REQUIRE_MSG(bool(parsed), parsed ? std::string{} : parsed.error().message);
    AURORA_TEST_CHECK_EQ(recorder.types.size(), std::size_t{1});  // 收摊之后一条都不再派发
}

AURORA_TEST_CASE(sse_subscription_delivers_events_and_reports_one_exchange) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    Client client;
    allow_test_cleartext(client);
    ScriptedPeer peer(reply_after_reading(1, event_stream("", "id: 1\nevent: tick\ndata: a\n\ndata: b\n\n")));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    const auto outcome =
        subscribe_and_drive(client, Request::get(loopback_url(peer.port())).sse(single_stream()), sink);
    AURORA_TEST_REQUIRE_MSG(bool(outcome), outcome ? std::string{} : outcome.error().message);

    AURORA_TEST_CHECK_EQ(sink.opens, 1);
    AURORA_TEST_CHECK_TRUE(sink.head_body_empty);  // 建流交出的是头部块，体的字节属于事件
    AURORA_TEST_CHECK_EQ(sink.events, 2);
    AURORA_TEST_CHECK_EQ(sink.types, std::vector<std::string>{"tick", "message"});
    AURORA_TEST_CHECK_EQ(sink.datas, std::vector<std::string>{"a", "b"});
    AURORA_TEST_CHECK_EQ(sink.ends, 1);  // 不再重连且流已收完：正常结局宣告一次收尾
    const auto &done = outcome.value();
    AURORA_TEST_CHECK_EQ(done.exchanges, std::uint32_t{1});
    AURORA_TEST_CHECK_EQ(done.events, std::uint64_t{2});
    AURORA_TEST_CHECK_EQ(done.response.status(), std::uint16_t{200});
    AURORA_TEST_CHECK_TRUE(done.response.body().empty());
    AURORA_TEST_CHECK_EQ(done.last_event_id, "1");
    AURORA_TEST_CHECK_EQ(done.wire_bytes, std::string{"id: 1\nevent: tick\ndata: a\n\ndata: b\n\n"}.size());
    AURORA_TEST_CHECK_FALSE(done.stopped_by_sink);
    AURORA_TEST_CHECK_FALSE(done.reconnect_exhausted);
    AURORA_TEST_CHECK_EQ(sink.ids, std::vector<std::string>{"1", "1"});  // 派发时刻的续传标识，跨条留存
}

AURORA_TEST_CASE(sse_subscription_refuses_a_non_get_request_without_touching_the_peer) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    Client client;
    allow_test_cleartext(client);
    ScriptedPeer peer(reply_after_reading(1, event_stream("", "data: x\n\n")));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    const auto outcome =
        subscribe_and_drive(client, Request::post(loopback_url(peer.port())).sse(single_stream()), sink);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::HttpRequestInvalid);
    AURORA_TEST_CHECK(outcome.error().message.find("GET") != std::string::npos);
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);  // 一条连接都没发起：方法闸在建连之前
    AURORA_TEST_CHECK_EQ(sink.opens, 0);
    AURORA_TEST_CHECK_EQ(sink.ends, 0);
}

AURORA_TEST_CASE(sse_subscription_refuses_a_plain_text_response_and_does_not_reconnect) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 内容类型严判：`text/plain` 上一条「看着像事件流」的文本会让宿主收到一堆服务端从没打算推的记录。
    Client client;
    allow_test_cleartext(client);
    ScriptedPeer peer(
        reply_after_reading(1, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 10\r\n\r\ndata: x\n\n"));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    const auto outcome =
        subscribe_and_drive(client, Request::get(loopback_url(peer.port())).sse(immediate_reconnect(3)), sink);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::HttpProtocolError);
    AURORA_TEST_CHECK_EQ(sink.events, 0);
    AURORA_TEST_CHECK_EQ(sink.opens, 0);  // 没过闸的流不叫 `on_open`
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);  // 重连一万次拿到的也是同一个答案，故一条都不试
}

AURORA_TEST_CASE(sse_subscription_accepts_an_event_stream_with_a_charset_parameter) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 参数是同一份媒体的合法表达；大小写拼写同样不绑判定（字段值按不敏感比较）。
    Client client;
    allow_test_cleartext(client);
    ScriptedPeer peer(reply_after_reading(
        1,
        "HTTP/1.1 200 OK\r\nContent-Type: Text/Event-Stream; charset=utf-8\r\nContent-Length: 9\r\n\r\n"
        "data: x\n\n"));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    const auto outcome =
        subscribe_and_drive(client, Request::get(loopback_url(peer.port())).sse(single_stream()), sink);
    AURORA_TEST_REQUIRE_MSG(bool(outcome), outcome ? std::string{} : outcome.error().message);
    AURORA_TEST_CHECK_EQ(sink.events, 1);
    AURORA_TEST_CHECK_EQ(sink.datas.at(0), "x");
}

AURORA_TEST_CASE(sse_subscription_refuses_a_non_success_status) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    Client client;
    allow_test_cleartext(client);
    ScriptedPeer peer(reply_after_reading(
        1, "HTTP/1.1 503 Busy\r\nRetry-After: 1\r\nContent-Type: text/event-stream\r\nContent-Length: 2\r\n\r\nok"));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    const auto outcome =
        subscribe_and_drive(client, Request::get(loopback_url(peer.port())).sse(immediate_reconnect(2)), sink);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::HttpProtocolError);
    AURORA_TEST_CHECK(outcome.error().message.find("503") != std::string::npos);  // detail 点名状态码
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);
    AURORA_TEST_CHECK_EQ(sink.opens, 0);
}

AURORA_TEST_CASE(sse_subscription_reconnects_with_the_last_event_id) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    Client client;
    allow_test_cleartext(client);
    std::vector<std::string> recorded;
    std::mutex guard;
    ScriptedPeer peer(per_connection({record_head_then_reply(recorded, guard, event_stream("", "id: 7\ndata: a\n\n")),
                                      record_head_then_reply(recorded, guard, event_stream("", "data: b\n\n"))}));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    const auto outcome =
        subscribe_and_drive(client, Request::get(loopback_url(peer.port())).sse(immediate_reconnect(1)), sink);
    AURORA_TEST_REQUIRE_MSG(bool(outcome), outcome ? std::string{} : outcome.error().message);

    AURORA_TEST_CHECK_EQ(sink.opens, 2);  // 每条流各叫一次：那是宿主唯一能知道「流断过」的出口
    AURORA_TEST_CHECK_EQ(sink.events, 2);
    AURORA_TEST_CHECK_EQ(sink.ends, 1);  // 一次订阅至多一次收尾宣告，不是每条流一次
    const auto &done = outcome.value();
    AURORA_TEST_CHECK_EQ(done.exchanges, std::uint32_t{2});
    AURORA_TEST_CHECK_EQ(done.last_event_id, "7");
    AURORA_TEST_CHECK_TRUE(done.reconnect_exhausted);  // 停是因为额度用尽，不是因为服务端说完
    AURORA_TEST_CHECK_TRUE(request_contains(recorded, guard, 0, "GET "));
    AURORA_TEST_CHECK_FALSE(request_contains(recorded, guard, 0, "Last-Event-ID"));  // 第一条不带
    AURORA_TEST_CHECK_TRUE(request_contains(recorded, guard, 1, "Last-Event-ID: 7"));  // 第二条替宿主接力
}

AURORA_TEST_CASE(sse_subscription_clears_the_resume_token_when_the_peer_writes_an_empty_id) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 空值 `id:` 是「清空续传标识」：它的线上形态与从没带过这一枚头一样，故重连时不带。
    Client client;
    allow_test_cleartext(client);
    std::vector<std::string> recorded;
    std::mutex guard;
    ScriptedPeer peer(per_connection({record_head_then_reply(recorded, guard, event_stream("", "id: 7\ndata: a\n\n")),
                                      record_head_then_reply(recorded, guard, event_stream("", "id:\ndata: b\n\n")),
                                      record_head_then_reply(recorded, guard, event_stream("", "data: c\n\n"))}));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    const auto outcome =
        subscribe_and_drive(client, Request::get(loopback_url(peer.port())).sse(immediate_reconnect(2)), sink);
    AURORA_TEST_REQUIRE_MSG(bool(outcome), outcome ? std::string{} : outcome.error().message);
    AURORA_TEST_CHECK_EQ(outcome.value().last_event_id, "");
    AURORA_TEST_CHECK_TRUE(request_contains(recorded, guard, 1, "Last-Event-ID: 7"));
    AURORA_TEST_CHECK_FALSE(request_contains(recorded, guard, 2, "Last-Event-ID"));
}

AURORA_TEST_CASE(sse_subscription_adopts_the_retry_window_the_peer_wrote) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 判据是差分的：本地初值给 4 s，服务端写了 `retry: 20` → 重连在等待预算内落地；对照组里服务端没写，
    // 那条窗口就没到期（本用例因此显式取消它，不留跨用例复活的订阅）。
    Client client;
    allow_test_cleartext(client);
    const SseOptions slow{.reconnect = true, .max_reconnects = 1, .retry_default = std::chrono::milliseconds{4000}};
    ScriptedPeer fast(per_connection({reply_after_reading(1, event_stream("", "retry: 20\ndata: a\n\n")),
                                      reply_after_reading(1, event_stream("", "data: b\n\n"))}));
    AURORA_TEST_REQUIRE(fast.valid());

    RecordingSink sink;
    const auto outcome = subscribe_and_drive(client, Request::get(loopback_url(fast.port())).sse(slow), sink);
    AURORA_TEST_REQUIRE_MSG(bool(outcome), outcome ? std::string{} : outcome.error().message);
    AURORA_TEST_CHECK_EQ(outcome.value().exchanges, std::uint32_t{2});
    AURORA_TEST_CHECK_EQ(sink.events, 2);

    Client stubborn;
    allow_test_cleartext(stubborn);
    ScriptedPeer slow_peer(reply_after_reading(1, event_stream("", "data: a\n\n")));
    AURORA_TEST_REQUIRE(slow_peer.valid());
    const aurora::test::net::HostDriven host;
    RecordingSink other;
    Cancellation token;
    auto task = stubborn.open_sse(Request::get(loopback_url(slow_peer.port())).sse(slow), other, token);
    AURORA_TEST_CHECK_TRUE(
        aurora::test::net::drive_until("first stream settled", [&other] { return other.events >= 1; }).satisfied);
    // 4 s 的初值在一秒半内不会到期：这条判据钉的是「服务端没写才用本地初值」的后半句。
    AURORA_TEST_CHECK_FALSE(
        aurora::test::net::drive_until(
            "second exchange settles early", [&task] { return task.is_done(); }, std::chrono::milliseconds{1500})
            .satisfied);
    token.cancel();
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "open_sse done", [&task] { return task.is_done(); }, std::chrono::milliseconds{3000})
                          .satisfied);
    // 取消落在退避等待里：结局是 `net-cancelled` 而不是「等到 4 s 到期后重连上」。
    const auto settled = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(settled));
    AURORA_TEST_CHECK_EQ(settled.error().code_enum, aurora::ErrorCode::NetCancelled);
    // 对照组里那条窗口从未到期：对端只被连上一次，第二次连接压根没发起。
    AURORA_TEST_CHECK_EQ(slow_peer.accepts(), 1);
}

AURORA_TEST_CASE(sse_subscription_stops_reading_when_the_sink_stops) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 宿主收摊是成功结局：`stopped_by_sink` 为真、后续事件不再交、`on_end` 仍恰一次，且不重连。
    Client client;
    allow_test_cleartext(client);
    ScriptedPeer peer(reply_after_reading(1, event_stream("", "data: a\n\ndata: b\n\ndata: c\n\n")));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    sink.stop_after = 2;
    const auto outcome =
        subscribe_and_drive(client, Request::get(loopback_url(peer.port())).sse(immediate_reconnect(8)), sink);
    AURORA_TEST_REQUIRE_MSG(bool(outcome), outcome ? std::string{} : outcome.error().message);
    AURORA_TEST_CHECK_EQ(sink.events, 2);  // 停在收摊那一条，之后一条都不交
    AURORA_TEST_CHECK_EQ(sink.ends, 1);
    const auto &done = outcome.value();
    AURORA_TEST_CHECK_TRUE(done.stopped_by_sink);
    AURORA_TEST_CHECK_EQ(done.exchanges, std::uint32_t{1});
    AURORA_TEST_CHECK_FALSE(done.reconnect_exhausted);  // 收摊是宿主的主意，不是额度用尽
}

AURORA_TEST_CASE(sse_subscription_stops_at_the_head_when_the_sink_stops_there) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 建流就收摊：一条事件都不交，仍是成功结局且 `on_end` 恰一次。
    Client client;
    allow_test_cleartext(client);
    ScriptedPeer peer(reply_after_reading(1, event_stream("", "data: a\n\n")));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    sink.stop_at_open = true;
    const auto outcome = subscribe_and_drive(client, Request::get(loopback_url(peer.port())), sink);
    AURORA_TEST_REQUIRE_MSG(bool(outcome), outcome ? std::string{} : outcome.error().message);
    AURORA_TEST_CHECK_EQ(sink.opens, 1);
    AURORA_TEST_CHECK_EQ(sink.events, 0);
    AURORA_TEST_CHECK_EQ(sink.ends, 1);
    AURORA_TEST_CHECK_TRUE(outcome.value().stopped_by_sink);
}

AURORA_TEST_CASE(sse_subscription_surfaces_the_last_failure_when_the_budget_runs_out) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 额度用尽不新造码：交回的还是最后一次交换那枚真实失败（这里是读侧套接口错误），只是把「不再试了」
    // 补在文本里。`max_reconnects` 不含首次，故三条连接之后停手。
    //
    // 这枚剧本同时钉住重连资格的口径：`net-recv-failed` 在错误表里标的是**不可重试**，那句说的是「同一条请求
    // 别原地重放」；订阅要问的是「这条流断得值不值得再订一次」，答值得。故本件按错误码清单（而非表的旗位）
    // 判定，并在结局里原样交出那枚旗位——本件不改写元数据。
    Client client;
    allow_test_cleartext(client);
    const std::string head_only{
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n"};
    ScriptedPeer peer(per_connection({read_head_then_reset(head_only), read_head_then_reset(head_only),
                                      read_head_then_reset(head_only), read_head_then_reset(head_only)}));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    const auto outcome =
        subscribe_and_drive(client, Request::get(loopback_url(peer.port())).sse(immediate_reconnect(2)), sink);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::NetRecvFailed);
    AURORA_TEST_CHECK(outcome.error().message.find("reconnect budget of 2") != std::string::npos);
    AURORA_TEST_CHECK_EQ(outcome.error().retryable, false);  // 旗位来自错误表，本件没改写它
    AURORA_TEST_CHECK_EQ(peer.accepts(), 3);
    AURORA_TEST_CHECK_EQ(sink.ends, 0);  // 失败结局不收尾
}

AURORA_TEST_CASE(sse_subscription_cancellation_ends_the_stream_without_reconnecting) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 取消掐在途那条流：已交付的事件不回滚，`on_end` 不被调用（失败结局不收尾），且一条新流都不再接。
    Client client;
    allow_test_cleartext(client);
    const std::string one_event{
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n"
        "9\r\ndata: a\n\n\r\n"};
    ScriptedPeer peer(write_then_hold(one_event));
    AURORA_TEST_REQUIRE(peer.valid());

    const aurora::test::net::HostDriven host;
    RecordingSink sink;
    Cancellation token;
    auto task = client.open_sse(Request::get(loopback_url(peer.port())).sse(immediate_reconnect(8)), sink, token);
    AURORA_TEST_CHECK_TRUE(
        aurora::test::net::drive_until("first event delivered", [&sink] { return sink.events >= 1; }).satisfied);
    token.cancel();
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "open_sse done", [&task] { return task.is_done(); }, std::chrono::milliseconds{3000})
                          .satisfied);

    const auto outcome = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::NetCancelled);
    AURORA_TEST_CHECK_EQ(sink.events, 1);  // 取消之后不再交付
    AURORA_TEST_CHECK_EQ(sink.ends, 0);
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);  // 不再重连
}

AURORA_TEST_CASE(sse_subscription_refuses_a_leg_without_streaming_semantics) {
    FakeEngine engine;
    Capabilities caps;
    caps.streaming = false;
    engine.set_capabilities(caps);
    const FakeEngineScope scope(engine);

    RecordingSink sink;
    auto task = Client{}.open_sse(Request::get("http://127.0.0.1:1/"), sink);
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "open_sse done", [&task] { return task.is_done(); }, std::chrono::milliseconds{1000})
                          .satisfied);
    const auto outcome = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::NetPlatformUnavailable);
    AURORA_TEST_CHECK_EQ(sink.opens, 0);  // 绝不退化成「物化一份应答再慢慢切行」
    AURORA_TEST_CHECK_EQ(sink.ends, 0);
}

AURORA_TEST_CASE(sse_subscription_neither_reads_nor_writes_the_cache) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 「不查」只有先喂出一条**真能命中**的新鲜副本才算判据：表空时查与不查逐字节相同，变异掉绕过也无人变红。
    // 故同一 URL 先走一次普通交换把带 `max-age` 的副本存进表，再订阅同一目标。写侧另说：流式交换本就没有
    // `settle_cache` 那一步，`no_cache(true)` 买到的是读侧那一刀，本用例把两侧各留一枚断言。
    static constexpr char SEEDED_BODY[] =
        "HTTP/1.1 200 OK\r\n"
        "Cache-Control: max-age=600\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 3\r\n\r\n"
        "old";
    Client client;
    allow_test_cleartext(client);
    client.cache(CacheOptions{.enabled = true});
    ScriptedPeer peer(per_connection({
        reply_after_reading(1, std::string{SEEDED_BODY}),
        reply_after_reading(1, event_stream("", "data: a\n\n")),
    }));
    AURORA_TEST_REQUIRE(peer.valid());

    const auto url = loopback_url(peer.port());
    auto seed = client.send(Request::get(url));
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "seed send done", [&seed] { return seed.is_done(); }, std::chrono::milliseconds{3000})
                          .satisfied);
    const auto seeded = seed.result();
    AURORA_TEST_REQUIRE(bool(seeded));
    AURORA_TEST_REQUIRE(bool(seeded.value()));
    AURORA_TEST_CHECK_FALSE(seeded.value().value().from_cache());  // 表还空着：这份是从对端取回来的
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);

    RecordingSink sink;
    const auto subscribed = subscribe_and_drive(client, Request::get(url).sse(single_stream()), sink);
    AURORA_TEST_REQUIRE_MSG(bool(subscribed), subscribed ? std::string{} : subscribed.error().message);
    AURORA_TEST_CHECK_FALSE(subscribed.value().response.from_cache());
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);  // 表里那条新鲜副本没被拿来充当订阅的答案
    AURORA_TEST_CHECK_EQ(sink.events, 1);  // 收到的是流里那一条，不是副本的载荷

    auto probe = client.send(Request::get(url));
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "probe send done", [&probe] { return probe.is_done(); }, std::chrono::milliseconds{3000})
                          .satisfied);
    const auto materialized = probe.result();
    AURORA_TEST_REQUIRE(bool(materialized));
    AURORA_TEST_REQUIRE(bool(materialized.value()));
    const auto &copied = materialized.value().value();
    AURORA_TEST_CHECK_TRUE(copied.from_cache());
    // 表里仍是那条副本：一次订阅没有往表里写过任何东西
    const std::string reused{copied.body().begin(), copied.body().end()};
    AURORA_TEST_CHECK_EQ(reused, std::string{"old"});
}

AURORA_TEST_CASE(sse_subscription_declares_accept_and_leaves_a_written_one_alone) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 缺省宣告与 `Accept-Encoding` 同一纪律：调用方自己写过就不动，内容协商是调用方的表达。
    Client client;
    allow_test_cleartext(client);
    std::vector<std::string> recorded;
    std::mutex guard;
    ScriptedPeer peer(per_connection({record_head_then_reply(recorded, guard, event_stream("", "data: a\n\n")),
                                      record_head_then_reply(recorded, guard, event_stream("", "data: b\n\n"))}));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    const auto plain = subscribe_and_drive(client, Request::get(loopback_url(peer.port())).sse(single_stream()), sink);
    AURORA_TEST_REQUIRE_MSG(bool(plain), plain ? std::string{} : plain.error().message);
    AURORA_TEST_CHECK_TRUE(request_contains(recorded, guard, 0, "Accept: text/event-stream"));

    RecordingSink other;
    const auto written = subscribe_and_drive(
        client, Request::get(loopback_url(peer.port())).header("Accept", "*/*").sse(single_stream()), other);
    AURORA_TEST_REQUIRE_MSG(bool(written), written ? std::string{} : written.error().message);
    AURORA_TEST_CHECK_TRUE(request_contains(recorded, guard, 1, "Accept: */*"));
    AURORA_TEST_CHECK_EQ(std::ranges::count(recorded[1], '\0'), std::string::size_type{0});  // 没重复追加第二枚
}

}  // namespace aurora::test_cases::utest_net_http_sse
