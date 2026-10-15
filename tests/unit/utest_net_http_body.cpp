/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/http_body.h + src/aurora/net/detail/http_body.cpp
/// 测试说明: 请求体线上形态计划与写出泵的逐值回归（CHG-018 §12「大文件流式上传」的判定与产字节面）。
///           判定面覆盖无体与内存体的形态推定（零长内存体判「无体」、两枚字段都不补）、流式源按自报总长
///           在 `Length` 与 `Chunked` 之间分流（挂了源就算有体，段还没被问过不改变形态）、调用方声明优先
///           且被标成 `declared`（流式体配自报长度是正当写法）、两枚字段并存或同名各写多条判歧义、
///           非 `chunked` 的传输编码与非纯十进制的长度各落一次 `http-request-invalid`、`has_body` 独立于
///           形态供 `Expect:` 判定。写出泵面覆盖内存体按上限切段且字节序列与原体同值、分块形态的逐段框架
///           与只发一次的终止块、源自报的超大段由泵切开、正文计数不含框架、源失败原样上抛不被吞。
/// 全用例零 socket、零文件、零时钟：本件是纯判定与纯产字节函数，「头部说什么」与「体泵写什么」必须来自
/// 同一份计划，而这条一致性只能在这里逐值断言——放到发起侧就只能靠真连接反推。三平台同值，wasm 腿同样可跑。

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/net/detail/http_body.h"
#include "aurora/net/detail/http_wire.h"
#include "aurora/net/http.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_http_body {
namespace {

using aurora::net::detail::AURORA_HTTP_WRITE_CHUNK;
using aurora::net::detail::plan_request_body;
using aurora::net::detail::RequestBodyFraming;
using aurora::net::detail::RequestBodyPump;
using aurora::net::http::BodySource;
using aurora::net::http::Request;
using aurora::net::wire::render_request_chunk;

/// @brief 一只只回答三枚虚方法的推送源：形态判定不需要它真的被泵。
class StubSource final : public BodySource {
  public:
    /// @brief 以自报总长与可回退性构造。
    /// @param total `total_size()` 的回答。
    /// @param rewindable `restart()` 的回答。
    StubSource(std::optional<std::uint64_t> total, bool rewindable) : total_(total), rewindable_(rewindable) {}

    [[nodiscard]] auto next_chunk() -> Result<std::vector<char>> override {
        return Result<std::vector<char>>{std::vector<char>{}};  // 一问就结束：判定面不关心段
    }
    [[nodiscard]] auto total_size() const -> std::optional<std::uint64_t> override { return total_; }
    auto restart() -> bool override { return rewindable_; }

  private:
    std::optional<std::uint64_t> total_;  ///< 自报总长
    bool rewindable_;  ///< 可否回退
};

/// @brief 按脚本交段的推送源：段列交完即交空段，可指定从第几段起改为失败。
class ScriptedSource final : public BodySource {
  public:
    /// @brief 以自报总长、脚本段列与失败起点构造。
    /// @param total `total_size()` 的回答。
    /// @param pieces 逐段交出的正文（文本形态，构造时转字节）。
    /// @param fail_from 从第几段起改为失败（0 = 第一段就失败；`pieces.size()` 及以后 = 不失败）。
    ScriptedSource(std::optional<std::uint64_t> total, std::vector<std::string> pieces, std::size_t fail_from)
        : total_(total), pieces_(std::move(pieces)), fail_from_(fail_from) {}

    /// @brief 简化构造：不失败。
    /// @param total 自报总长。
    /// @param pieces 脚本段列。
    ScriptedSource(std::optional<std::uint64_t> total, std::vector<std::string> pieces)
        : ScriptedSource(total, std::move(pieces), std::numeric_limits<std::size_t>::max()) {}

    [[nodiscard]] auto next_chunk() -> Result<std::vector<char>> override {
        ++pulls_;
        if (cursor_ >= fail_from_) {
            return Result<std::vector<char>>{
                make_error(ErrorCode::IOFileNotFound, std::string{"scripted read failed"})};
        }
        if (cursor_ >= pieces_.size()) {
            return Result<std::vector<char>>{std::vector<char>{}};  // 段列交完 = 体已发完
        }
        const auto &piece = pieces_.at(cursor_++);
        return Result<std::vector<char>>{std::vector<char>{piece.begin(), piece.end()}};
    }
    [[nodiscard]] auto total_size() const -> std::optional<std::uint64_t> override { return total_; }
    auto restart() -> bool override {
        cursor_ = 0;
        return true;
    }

    /// @brief 被问过几次（「一次一问」这条纪律的观测口）。
    /// @return 累计询问次数。
    [[nodiscard]] auto pulls() const -> std::size_t { return pulls_; }

  private:
    std::optional<std::uint64_t> total_;  ///< 自报总长
    std::vector<std::string> pieces_;  ///< 脚本段列
    std::size_t fail_from_;  ///< 失败起点（段下标）
    std::size_t cursor_{0};  ///< 已交出的段数
    std::size_t pulls_{0};  ///< 被询问次数
};

/// @brief 断言判定成功并交出计划（判失败即致命失败，消息带进断言文本）。
/// @param request 待判定的请求。
/// @return 体形态计划。
[[nodiscard]] auto planned(const Request &request) -> aurora::net::detail::RequestBodyPlan {
    const auto plan = plan_request_body(request);
    AURORA_TEST_REQUIRE_MSG(bool(plan), plan ? std::string{} : plan.error().message);
    return plan.value();
}

/// @brief 断言判定失败并回读错误码。
/// @param request 待判定的请求。
/// @return 失败码。
[[nodiscard]] auto planned_error(const Request &request) -> aurora::ErrorCode {
    const auto plan = plan_request_body(request);
    AURORA_TEST_REQUIRE_MSG(!bool(plan), plan ? std::string{"expected a refusal"} : std::string{});
    return plan.error().code_enum;
}

/// @brief 把一只泵抽到底。
/// @param pump 待抽取的泵。
/// @param out_bytes 拼接出的线上字节。
/// @return 交出的非空段数（失败即致命失败，故本函数只在成功路径上返回）。
[[nodiscard]] auto pump_all(RequestBodyPump &pump, std::string &out_bytes) -> std::size_t {
    std::size_t segments = 0;
    while (true) {
        auto segment = pump.next_segment();
        AURORA_TEST_REQUIRE_MSG(bool(segment), segment ? std::string{} : segment.error().message);
        if (segment.value().empty()) {
            return segments;
        }
        out_bytes.append(segment.value().data(), segment.value().size());
        ++segments;
    }
}

/// @brief 把文本按单段上限切成若干段（构造「超过一段的大块」用）。
/// @param text 原文。
/// @param length 每段长度。
/// @return 段列。
[[nodiscard]] auto slice(std::string_view text, std::size_t length) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (std::size_t offset = 0; offset < text.size(); offset += length) {
        out.emplace_back(text.substr(offset, length));
    }
    return out;
}

}  // namespace

AURORA_TEST_CASE(in_memory_bodies_plan_a_length_or_nothing) {
    // 无体：两枚字段都不补，也没有待写出的体。
    const auto bare = Request::post("https://example.com/");
    const auto bare_plan = planned(bare);
    AURORA_TEST_CHECK_TRUE(bare_plan.framing == RequestBodyFraming::None);
    AURORA_TEST_CHECK_TRUE(!bare_plan.has_body);
    AURORA_TEST_CHECK_TRUE(!bare_plan.declared);
    AURORA_TEST_CHECK_TRUE(!bare_plan.length.has_value());

    // 零长内存体等价于无体：不为「POST 一个零长资源」多出一句对端可能另有解读的声明。
    const auto zero = Request::put("https://example.com/").body(std::vector<char>{});
    AURORA_TEST_CHECK_TRUE(planned(zero).framing == RequestBodyFraming::None);
    AURORA_TEST_CHECK_TRUE(!planned(zero).has_body);

    // 有体：确切长度就是字节数，且这长度由本件现地算出（不是从字段里读回来的）。
    const auto some = Request::post("https://example.com/").body(std::vector<char>(7, 'a'));
    const auto some_plan = planned(some);
    AURORA_TEST_CHECK_TRUE(some_plan.framing == RequestBodyFraming::Length);
    AURORA_TEST_CHECK_EQ(some_plan.length.value_or(0), std::uint64_t{7});
    AURORA_TEST_CHECK_TRUE(!some_plan.declared);
    AURORA_TEST_CHECK_TRUE(some_plan.has_body);

    // `HEAD` 在 setter 处就丢弃了体，故这里仍是无体。
    const auto head = Request::head("https://example.com/").body(std::vector<char>(9, 'a'));
    AURORA_TEST_CHECK_TRUE(planned(head).framing == RequestBodyFraming::None);
}

AURORA_TEST_CASE(streaming_source_decides_between_length_and_chunked) {
    // 源自报总长：按长度走，与内存体同一条形态。
    StubSource measured{std::uint64_t{3}, true};
    const auto with_total = Request::post("https://example.com/upload").body(measured);
    const auto total_plan = planned(with_total);
    AURORA_TEST_CHECK_TRUE(total_plan.framing == RequestBodyFraming::Length);
    AURORA_TEST_CHECK_EQ(total_plan.length.value_or(0), std::uint64_t{3});
    AURORA_TEST_CHECK_TRUE(total_plan.has_body);

    // 总长未知：只能分块——这是本库唯一会主动选 `Transfer-Encoding: chunked` 的入口。
    StubSource unknown{std::nullopt, true};
    const auto streaming = Request::post("https://example.com/upload").body(unknown);
    const auto streaming_plan = planned(streaming);
    AURORA_TEST_CHECK_TRUE(streaming_plan.framing == RequestBodyFraming::Chunked);
    AURORA_TEST_CHECK_TRUE(!streaming_plan.length.has_value());
    AURORA_TEST_CHECK_TRUE(streaming_plan.has_body);

    // 挂了源就算有体：段还没被问过、乃至一只一段也不吐的源，都不该让形态摇摆。
    StubSource empty_source{std::uint64_t{0}, false};
    const auto empty_stream = Request::post("https://example.com/upload").body(empty_source);
    const auto empty_plan = planned(empty_stream);
    AURORA_TEST_CHECK_TRUE(empty_plan.framing == RequestBodyFraming::Length);
    AURORA_TEST_CHECK_TRUE(empty_plan.has_body);
}

AURORA_TEST_CASE(caller_declaration_wins_and_is_marked_declared) {
    // 流式体 + 自报长度：「我知道这文件多大但不想整份进内存」是那条声明的用意，长度以声明为准。
    StubSource unknown{std::nullopt, true};
    const auto declared_length =
        Request::post("https://example.com/upload").body(unknown).set_header("Content-Length", "1048576");
    const auto length_plan = planned(declared_length);
    AURORA_TEST_CHECK_TRUE(length_plan.framing == RequestBodyFraming::Length);
    AURORA_TEST_CHECK_EQ(length_plan.length.value_or(0), std::uint64_t{1048576});
    AURORA_TEST_CHECK_TRUE(length_plan.declared);

    // 内存体 + 声明分块：写出侧照声明走分块，本件不改回长度。
    const auto declared_chunked = Request::post("https://example.com/")
                                      .body(std::vector<char>(5, 'a'))
                                      .set_header("Transfer-Encoding", "chunked");
    const auto chunked_plan = planned(declared_chunked);
    AURORA_TEST_CHECK_TRUE(chunked_plan.framing == RequestBodyFraming::Chunked);
    AURORA_TEST_CHECK_TRUE(chunked_plan.declared);
    AURORA_TEST_CHECK_TRUE(chunked_plan.has_body);

    // 传输编码的大小写与首尾空白都不影响判定（字段值按令牌比对）。
    const auto padded = Request::post("https://example.com/")
                            .body(std::vector<char>(1, 'a'))
                            .set_header("Transfer-Encoding", " ChUnKeD ");
    AURORA_TEST_CHECK_TRUE(planned(padded).framing == RequestBodyFraming::Chunked);

    // 声明了却暂无体：形态照声明，`has_body` 说的是「有没有待写出的字节」，两件事各归各。
    const auto declared_only = Request::get("https://example.com/").set_header("Content-Length", "12");
    const auto only_plan = planned(declared_only);
    AURORA_TEST_CHECK_TRUE(only_plan.framing == RequestBodyFraming::Length);
    AURORA_TEST_CHECK_TRUE(!only_plan.has_body);
    AURORA_TEST_CHECK_TRUE(only_plan.declared);
}

AURORA_TEST_CASE(ambiguous_or_unparsable_declarations_are_refused) {
    // 两枚字段并存：线上只有一句该作数，本库不替调用方挑（转发方的清理动作不属发起侧）。
    const auto both = Request::post("https://example.com/")
                          .body(std::vector<char>(1, 'a'))
                          .set_header("Content-Length", "1")
                          .set_header("Transfer-Encoding", "chunked");
    AURORA_TEST_CHECK_EQ(planned_error(both), aurora::ErrorCode::HttpRequestInvalid);

    // 同名各写两条同样是歧义：`get()` 只取首个，第二条没人负责。
    const auto twice = Request::post("https://example.com/")
                           .body(std::vector<char>(1, 'a'))
                           .header("Content-Length", "1")
                           .header("Content-Length", "2");
    AURORA_TEST_CHECK_EQ(planned_error(twice), aurora::ErrorCode::HttpRequestInvalid);

    // 本库只按 chunked 发请求体：其余传输编码写上线等于声明一段没人负责编码的字节。
    const auto gzip =
        Request::post("https://example.com/").body(std::vector<char>(1, 'a')).set_header("Transfer-Encoding", "gzip");
    AURORA_TEST_CHECK_EQ(planned_error(gzip), aurora::ErrorCode::HttpRequestInvalid);

    // 长度值必须是纯十进制：带单位、带符号、空白塞中间都判不可解析。
    for (const std::string_view bogus : {std::string_view{"12x"}, std::string_view{"+12"}, std::string_view{"1 2"},
                                         std::string_view{"12KB"}, std::string_view{""}}) {
        const auto request =
            Request::post("https://example.com/").body(std::vector<char>(1, 'a')).set_header("Content-Length", bogus);
        AURORA_TEST_CHECK_EQ(planned_error(request), aurora::ErrorCode::HttpRequestInvalid);
    }
}

AURORA_TEST_CASE(framing_is_a_pure_function_of_the_request) {
    // 同一份请求判两次同值：形态里没有任何「第一次判完就改掉」的状态（源的 `total_size()` 只读）。
    StubSource unknown{std::nullopt, true};
    const auto request = Request::post("https://example.com/upload").body(unknown);
    AURORA_TEST_CHECK_TRUE(planned(request).framing == planned(request).framing);
    AURORA_TEST_CHECK_EQ(planned(request).has_body, planned(request).has_body);

    // 判定不碰字段表：判完请求的字段仍与判前逐条同序同值。
    const auto sized = Request::post("https://example.com/").body(std::vector<char>(3, 'a'));
    AURORA_TEST_CHECK_EQ(sized.headers().size(), std::size_t{0});
    (void)planned(sized);
    AURORA_TEST_CHECK_EQ(sized.headers().size(), std::size_t{0});
}

AURORA_TEST_CASE(a_memory_body_pumps_in_slices_that_reassemble_the_original) {
    const auto text = std::string((AURORA_HTTP_WRITE_CHUNK * 2U) + 5U, 'z');
    const auto request = Request::post("https://example.com/put").body(std::vector<char>{text.begin(), text.end()});
    RequestBodyPump pump{request, planned(request)};

    std::string flowed;
    AURORA_TEST_CHECK_EQ(pump_all(pump, flowed), std::size_t{3});  // 上限、上限、余 5 字节
    AURORA_TEST_CHECK_TRUE(pump.has_body());
    AURORA_TEST_CHECK_TRUE(pump.done());
    // 定长形态的框架就是没有框架：交出的字节逐字节等于原体，长度行一个也不插。
    AURORA_TEST_CHECK_EQ(flowed, text);
    AURORA_TEST_CHECK_EQ(pump.payload_bytes(), static_cast<std::uint64_t>(text.size()));
    // 交完之后每问一次都是空段（幂等），发起侧可以循环到 `done()` 为止。
    const auto extra = pump.next_segment();
    AURORA_TEST_REQUIRE_TRUE(bool(extra));
    AURORA_TEST_CHECK_TRUE(extra.value().empty());
}

AURORA_TEST_CASE(a_chunked_pump_wraps_each_segment_and_terminates_once) {
    ScriptedSource source{std::nullopt, std::vector<std::string>{std::string{"ab"}, std::string{"cde"}}};
    const auto request = Request::post("https://example.com/stream").body(source);
    RequestBodyPump pump{request, planned(request)};

    std::string flowed;
    AURORA_TEST_CHECK_EQ(pump_all(pump, flowed), std::size_t{3});
    const auto expected =
        render_request_chunk("ab") + render_request_chunk("cde") + render_request_chunk(std::string_view{});
    AURORA_TEST_CHECK_EQ(flowed, expected);
    // 终止块只发一次，且发完即收摊；再问只剩空段。
    AURORA_TEST_CHECK_TRUE(pump.done());
    AURORA_TEST_CHECK_TRUE(flowed.find("0\r\n\r\n") != std::string::npos);
    AURORA_TEST_CHECK_EQ(flowed.find("0\r\n\r\n"), flowed.size() - std::size_t{5});
    // 正文计数只数 `abcde`：长度行与 CRLF 不算「我的数据送到哪儿了」。
    AURORA_TEST_CHECK_EQ(pump.payload_bytes(), std::uint64_t{5});
    AURORA_TEST_CHECK_TRUE(flowed.size() > std::size_t{5});
    // 一次一问：两段正文各问一次，最后再问一次才知道「交完了」。
    AURORA_TEST_CHECK_EQ(source.pulls(), std::size_t{3});
}

AURORA_TEST_CASE(an_oversized_source_piece_is_split_by_the_pump) {
    const auto big = std::string(AURORA_HTTP_WRITE_CHUNK + 10U, 'q');
    ScriptedSource source{std::nullopt, std::vector<std::string>{big}};
    const auto request = Request::post("https://example.com/stream").body(source);
    RequestBodyPump pump{request, planned(request)};

    std::string flowed;
    // 源自报的一段超上限：泵替它切成两段（分块形态切开只是多一枚长度行），再加终止块共三段。
    AURORA_TEST_CHECK_EQ(pump_all(pump, flowed), std::size_t{3});
    const auto halves = slice(big, AURORA_HTTP_WRITE_CHUNK);
    const auto expected =
        render_request_chunk(halves[0]) + render_request_chunk(halves[1]) + render_request_chunk(std::string_view{});
    AURORA_TEST_CHECK_EQ(flowed, expected);
    AURORA_TEST_CHECK_EQ(pump.payload_bytes(), static_cast<std::uint64_t>(big.size()));
    // 切开的那半段留在泵里，不必回头再问源；多出来的一次问话是「还有下一段吗」。
    AURORA_TEST_CHECK_EQ(source.pulls(), std::size_t{2});
}

AURORA_TEST_CASE(a_source_failure_is_handed_up_verbatim) {
    // 源在第一段就失败：泵不解释「为什么读不到」，码与消息都原样交给发起侧。
    // 临时量都先具名再进断言宏：宏按单表达式展开，括号里的逗号会被当参数分隔。
    auto first_failure = ScriptedSource{std::nullopt, std::vector<std::string>{std::string{"ab"}}, std::size_t{0}};
    const auto failed_request = Request::post("https://example.com/stream").body(first_failure);
    RequestBodyPump pump{failed_request, planned(failed_request)};
    const auto failed = pump.next_segment();
    AURORA_TEST_REQUIRE_TRUE(!bool(failed));
    AURORA_TEST_CHECK_EQ(failed.error().code_enum, aurora::ErrorCode::IOFileNotFound);
    AURORA_TEST_CHECK_EQ(failed.error().message, std::string{"scripted read failed"});
    AURORA_TEST_CHECK_TRUE(!pump.done());  // 失败不是「交完了」：发起侧据此中止本次交换而非收尾

    // 交出一段之后再失败：已计数的正文不回滚，进度说的是「送到哪儿了」。
    auto halfway_source = ScriptedSource{std::nullopt, std::vector<std::string>{std::string{"ab"}}, std::size_t{1}};
    const auto midway = Request::post("https://example.com/stream").body(halfway_source);
    RequestBodyPump second{midway, planned(midway)};
    const auto first = second.next_segment();
    AURORA_TEST_REQUIRE_TRUE(bool(first));
    AURORA_TEST_CHECK_EQ(second.payload_bytes(), std::uint64_t{2});
    const auto again = second.next_segment();
    AURORA_TEST_REQUIRE_TRUE(!bool(again));
    AURORA_TEST_CHECK_EQ(again.error().code_enum, aurora::ErrorCode::IOFileNotFound);
    AURORA_TEST_CHECK_EQ(second.payload_bytes(), std::uint64_t{2});
}

AURORA_TEST_CASE(a_request_without_a_body_pumps_nothing) {
    const auto request = Request::get("https://example.com/");
    RequestBodyPump pump{request, planned(request)};
    AURORA_TEST_CHECK_TRUE(!pump.has_body());
    AURORA_TEST_CHECK_TRUE(pump.done());
    const auto segment = pump.next_segment();
    AURORA_TEST_REQUIRE_TRUE(bool(segment));
    AURORA_TEST_CHECK_TRUE(segment.value().empty());
    AURORA_TEST_CHECK_EQ(pump.payload_bytes(), std::uint64_t{0});

    // 零长内存体同样无段可交（判定把它算成「无体」，泵照同一份计划走）。
    const auto zero = Request::put("https://example.com/").body(std::vector<char>{});
    RequestBodyPump zero_pump{zero, planned(zero)};
    AURORA_TEST_CHECK_TRUE(zero_pump.done());
    AURORA_TEST_CHECK_TRUE(!zero_pump.has_body());
}

}  // namespace aurora::test_cases::utest_net_http_body
