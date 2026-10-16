/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/http_retry.h + src/aurora/net/detail/http_retry.cpp
/// 测试说明: HTTP 重试判定与退避算式的逐值回归。这套判据刻意从发起侧
///           的协程体里拆出来，就是为了让每一条都能在零 socket、零时钟推进的场合断言：策略形态闸只挡越界
///           字段并点名是哪一枚（`base` 为负 / `max_backoff` 小于 `base` / `jitter` 不在 `[0,1]`，NaN 也算
///           越界，而 `budget` 非正值与不设等价故不属越界）；幂等白名单按 `RFC 9110` 的方法律取死集合
///           （`GET` / `HEAD` / `PUT` / `DELETE`）；一枚失败的重试资格是 `Error::retryable` 与归因档的
///           **交集**（`net-cancelled` 标着可重试但落在 `Cancelled` 档，故被挡住——重放用户主动取消的请求
///           等于违抗那次取消；代理档与缓冲上限同理），状态码那族只收 408 / 429 / 500 / 502 / 503 / 504；
///           指数退避逐次翻倍并在 `max_backoff` 封顶（移位越界只可能落到上限，不会绕回小值）；抖动取库内置
///           的非密码学序列，故 `jitter=0` 恒等于算得值、`jitter=1` 落在 `[raw, 2*raw]` 且总和仍受封顶；
///           `Retry-After` 两形态都解（延迟秒数位数不限、到一年封顶而非退化成「没给」，IMF-fixdate 走纯日历
///           算术且只认规范星期 / 月份拼写与 `" GMT"` 收尾，过期日期折成零等待），有它就以它为准、不加抖动、
///           仍受 `max_backoff` 封顶。日期与时钟全部由参数注入，故三平台同值、wasm 腿同样可跑。

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/net/detail/http_retry.h"
#include "aurora/net/http.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_http_retry {
namespace {

using aurora::net::detail::backoff_delay;
using aurora::net::detail::is_idempotent_method;
using aurora::net::detail::parse_http_date;
using aurora::net::detail::parse_retry_after;
using aurora::net::detail::retry_enabled;
using aurora::net::detail::should_retry_error;
using aurora::net::detail::should_retry_status;
using aurora::net::detail::validate_retry_policy;
using aurora::net::detail::wait_delay;
using aurora::net::http::Method;
using aurora::net::http::RetryPolicy;
using aurora::testing::require_value;

/// @brief 毫秒的字面量写法（不用 `chrono_literals`：本仓的测试与实现一律写全称，别在这里开第二个口径）。
/// @param value 毫秒数。
/// @return 对应时长。
[[nodiscard]] constexpr auto ms(std::int64_t value) -> std::chrono::milliseconds {
    return std::chrono::milliseconds{value};
}

/// @brief 把可选等待时长折成毫秒数；空值折成 `-1`，使「解不出」与「解出零」在断言里可分。
/// @param value 解析结果。
/// @return 毫秒数，或 `-1` 表示没有解析出任何事实。
[[nodiscard]] auto millis(const std::optional<std::chrono::milliseconds> &value) -> std::int64_t {
    return value.has_value() ? value.value().count() : std::int64_t{-1};
}

/// @brief 断言文本里出现某子串（越界判定必须点名是哪一枚字段，否则调用方只能读源码）。
/// @param haystack 错误载荷的 message。
/// @param needle 期望点名的片段。
/// @return 出现为真。
[[nodiscard]] auto names(std::string_view haystack, std::string_view needle) -> bool {
    return haystack.find(needle) != std::string_view::npos;
}

/// @brief 一枚码的期望重试资格（交集判据的真值表：新增码不该静默改变重放面）。
struct RetryExpectation {
    aurora::ErrorCode code;  ///< 表驱动的错误码
    bool admitted;  ///< `Error::retryable` 与归因档取交集后的期望
};

/// @brief 由一枚码造出失败载荷（元数据全取自生成表，用例不手填 `retryable`）。
/// @param code 错误码。
/// @return 可直接喂给判定函数的载荷。
[[nodiscard]] auto failure_of(aurora::ErrorCode code) -> aurora::Error {
    return aurora::make_error(code, "retry fixture");
}

/// @brief 把时刻折成「自 epoch 起的毫秒」（断言日历算术时不比 `time_point` 的内部刻度）。
/// @param when 待折算的时刻。
/// @return 毫秒计数。
[[nodiscard]] auto since_epoch(std::chrono::system_clock::time_point when) -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count();
}

// `RFC 9110 §5.6.7` 的规范样例时刻：1994-11-06 08:49:37 GMT = epoch 秒 784111777。
constexpr std::int64_t AURORA_FIXDATE_SECONDS = 784111777;
constexpr std::string_view AURORA_FIXDATE = "Sun, 06 Nov 1994 08:49:37 GMT";

}  // namespace

AURORA_TEST_CASE(policy_defaults_to_no_retry) {
    const RetryPolicy policy;
    // 缺省关闭：一次都不重放。其余字段即便有值也无从生效，故只要求形态自洽。
    AURORA_TEST_CHECK_EQ(policy.attempts, std::uint32_t{1});
    AURORA_TEST_CHECK_FALSE(retry_enabled(policy));
    AURORA_TEST_CHECK_FALSE(validate_retry_policy(policy).has_value());
    AURORA_TEST_CHECK_EQ(policy.base, ms(200));
    AURORA_TEST_CHECK_EQ(policy.max_backoff, ms(30000));
    AURORA_TEST_CHECK(policy.jitter >= 0.0 && policy.jitter <= 1.0);
    AURORA_TEST_CHECK_FALSE(policy.budget.has_value());

    // 开关只看 `attempts`：两枚及以上才算「有一次重放」。
    AURORA_TEST_CHECK_FALSE(retry_enabled(RetryPolicy{.attempts = 0}));
    AURORA_TEST_CHECK(retry_enabled(RetryPolicy{.attempts = 2}));
    AURORA_TEST_CHECK(retry_enabled(RetryPolicy{.attempts = std::numeric_limits<std::uint32_t>::max()}));
}

AURORA_TEST_CASE(validate_names_the_out_of_range_knob) {
    // 起手即拒、detail 点名是哪一枚：静默折算会让「以为开了抖动其实没开」等到线上重试风暴才现形。
    const auto negative_base = require_value(validate_retry_policy(RetryPolicy{.base = ms(-1)}));
    AURORA_TEST_CHECK_EQ(negative_base.code_enum, aurora::ErrorCode::HttpRequestInvalid);
    AURORA_TEST_CHECK(names(negative_base.message, "base"));

    const auto negative_cap = require_value(validate_retry_policy(RetryPolicy{.base = ms(100), .max_backoff = ms(-1)}));
    AURORA_TEST_CHECK(names(negative_cap.message, "max_backoff"));

    const auto cap_below_base =
        require_value(validate_retry_policy(RetryPolicy{.base = ms(500), .max_backoff = ms(100)}));
    AURORA_TEST_CHECK(names(cap_below_base.message, "smaller"));

    const auto too_small = require_value(validate_retry_policy(RetryPolicy{.jitter = -0.01}));
    AURORA_TEST_CHECK(names(too_small.message, "jitter"));
    const auto too_large = require_value(validate_retry_policy(RetryPolicy{.jitter = 1.5}));
    AURORA_TEST_CHECK(names(too_large.message, "jitter"));
    // NaN 与 [0,1] 的比较式恒假，故同样落进这一档：它不是「比较漏网」而是「越界」。
    const auto not_a_number =
        require_value(validate_retry_policy(RetryPolicy{.jitter = std::numeric_limits<double>::quiet_NaN()}));
    AURORA_TEST_CHECK(names(not_a_number.message, "jitter"));

    // 合法域的边界本身可用：0 与 1 都是可设定值。
    AURORA_TEST_CHECK_FALSE(validate_retry_policy(RetryPolicy{.jitter = 0.0}).has_value());
    AURORA_TEST_CHECK_FALSE(validate_retry_policy(RetryPolicy{.jitter = 1.0}).has_value());
    // `budget` 非正值与不设等价（与 `Timeouts` 各档同口径），故不属越界形态。
    AURORA_TEST_CHECK_FALSE(validate_retry_policy(RetryPolicy{.budget = ms(0)}).has_value());
    AURORA_TEST_CHECK_FALSE(validate_retry_policy(RetryPolicy{.budget = ms(-5)}).has_value());
}

AURORA_TEST_CASE(idempotent_whitelist_is_the_method_law) {
    // 白名单按 `RFC 9110` 的幂等定义取死集合，不做成可调旋钮：重放非幂等请求的代价在失败现场无从判定。
    AURORA_TEST_CHECK(is_idempotent_method(Method::Get));
    AURORA_TEST_CHECK(is_idempotent_method(Method::Head));
    AURORA_TEST_CHECK(is_idempotent_method(Method::Put));
    AURORA_TEST_CHECK(is_idempotent_method(Method::Delete));
    AURORA_TEST_CHECK_FALSE(is_idempotent_method(Method::Post));
    AURORA_TEST_CHECK_FALSE(is_idempotent_method(Method::Patch));
}

AURORA_TEST_CASE(retry_admission_is_the_intersection_of_flag_and_class) {
    const std::vector<RetryExpectation> table{
        // 三档之内且表标可重试：允许重放。
        {.code = aurora::ErrorCode::NetDnsFailed, .admitted = true},
        {.code = aurora::ErrorCode::NetConnectFailed, .admitted = true},
        {.code = aurora::ErrorCode::NetSocketFailed, .admitted = true},
        {.code = aurora::ErrorCode::NetRuntimeFailed, .admitted = true},
        {.code = aurora::ErrorCode::NetWaitFailed, .admitted = true},
        {.code = aurora::ErrorCode::NetConnectTimeout, .admitted = true},
        {.code = aurora::ErrorCode::NetRecvTimeout, .admitted = true},
        {.code = aurora::ErrorCode::HttpTimeout, .admitted = true},
        // 服务端面的三枚同样落在 `Unreachable` 且标可重试：发起侧不产出它们，交集口径不为它们开口子。
        {.code = aurora::ErrorCode::NetBindFailed, .admitted = true},
        {.code = aurora::ErrorCode::NetListenFailed, .admitted = true},
        {.code = aurora::ErrorCode::NetAcceptFailed, .admitted = true},
        // 可重试但不在三档：`net-cancelled` 是本条裁决的头号目标——重放用户主动取消的请求等于违抗那次取消。
        {.code = aurora::ErrorCode::NetCancelled, .admitted = false},
        {.code = aurora::ErrorCode::NetProxyUnreachable, .admitted = false},
        {.code = aurora::ErrorCode::NetProxyConfigUnreadable, .admitted = false},
        {.code = aurora::ErrorCode::NetBufferLimit, .admitted = false},
        {.code = aurora::ErrorCode::RuntimeAsyncTimeout, .admitted = false},  // 非网络族码：兜底档不重放
        // 档内但表标不可重试：写侧与收侧的既有失败不靠「再打一遍」修复。
        {.code = aurora::ErrorCode::NetSendFailed, .admitted = false},
        {.code = aurora::ErrorCode::NetRecvFailed, .admitted = false},
        // 用法、协议与本库上限：重放一万次也是同一个错。
        {.code = aurora::ErrorCode::HttpRequestInvalid, .admitted = false},
        {.code = aurora::ErrorCode::HttpProtocolError, .admitted = false},
        {.code = aurora::ErrorCode::HttpBodyTooLarge, .admitted = false},
        {.code = aurora::ErrorCode::HttpContentEncodingUnsupported, .admitted = false},
        {.code = aurora::ErrorCode::NetTlsHandshakeFailed, .admitted = false},
        {.code = aurora::ErrorCode::NetPlatformUnavailable, .admitted = false},
    };
    for (const auto &[code, admitted] : table) {
        AURORA_TEST_CHECK_EQ(should_retry_error(failure_of(code)), admitted);
    }
}

AURORA_TEST_CASE(status_admits_only_the_transient_family) {
    constexpr std::array<std::uint16_t, 6> transient{408, 429, 500, 502, 503, 504};
    for (const std::uint16_t status : transient) {
        AURORA_TEST_CHECK(should_retry_status(status));
    }
    // 其余码不是「服务器明说现在不行」：501 是能力缺失，重放只换回同一句「不支持」；3xx 由重定向环处理，
    // 与重试环是两件事；4xx 的修复动作在调用方而不在时间上。
    constexpr std::array<std::uint16_t, 17> others{200, 204, 301, 302, 400, 401, 403, 404, 405,
                                                   409, 413, 425, 451, 501, 505, 507, 599};
    for (const std::uint16_t status : others) {
        AURORA_TEST_CHECK_FALSE(should_retry_status(status));
    }
}

AURORA_TEST_CASE(backoff_doubles_then_clamps) {
    const RetryPolicy policy{.base = ms(100), .max_backoff = ms(1000)};
    // 第 1 次失败后等 `base`，其后逐次翻倍，直到封顶。
    AURORA_TEST_CHECK_EQ(backoff_delay(policy, 1), ms(100));
    AURORA_TEST_CHECK_EQ(backoff_delay(policy, 2), ms(200));
    AURORA_TEST_CHECK_EQ(backoff_delay(policy, 3), ms(400));
    AURORA_TEST_CHECK_EQ(backoff_delay(policy, 4), ms(800));
    AURORA_TEST_CHECK_EQ(backoff_delay(policy, 5), ms(1000));
    AURORA_TEST_CHECK_EQ(backoff_delay(policy, 64), ms(1000));
    // 移位越界只可能落到上限：再夸张的尝试序号也不会绕回小值或折成负数。
    AURORA_TEST_CHECK_EQ(backoff_delay(policy, 4096), ms(1000));
    const RetryPolicy uncapped{.base = ms(100), .max_backoff = ms(std::numeric_limits<std::int64_t>::max())};
    AURORA_TEST_CHECK_EQ(backoff_delay(uncapped, std::numeric_limits<std::uint32_t>::max()),
                         ms(std::numeric_limits<std::int64_t>::max()));
    // `base` 为零即「立刻再打」：不因为翻倍而凭空长出等待。
    AURORA_TEST_CHECK_EQ(backoff_delay(RetryPolicy{.base = ms(0), .max_backoff = ms(5000)}, 3), ms(0));
}

AURORA_TEST_CASE(jitter_is_a_spread_not_a_random_number) {
    const RetryPolicy off{.base = ms(100), .max_backoff = ms(100000), .jitter = 0.0};
    // 抖动关掉时恒等于算得值：这条只有在序列可复现时才成立，故本库不用 `std::random_device`。
    for (int draw = 0; draw < 32; ++draw) {
        AURORA_TEST_CHECK_EQ(wait_delay(off, 2, std::nullopt), ms(200));
    }

    const RetryPolicy full{.base = ms(100), .max_backoff = ms(100000), .jitter = 1.0};
    bool spread = false;
    for (int draw = 0; draw < 32; ++draw) {
        const auto delay = wait_delay(full, 2, std::nullopt);
        // 抖动只加不减：等得比基准更早是缺陷，不是「打散」。
        AURORA_TEST_CHECK_GE(delay, ms(200));
        AURORA_TEST_CHECK_LE(delay, ms(400));
        spread = spread || delay > ms(200);
    }
    AURORA_TEST_CHECK(spread);

    // 加抖之后的和仍受封顶约束：抖动不会把等待推过 `max_backoff`。
    const RetryPolicy tight{.base = ms(1000), .max_backoff = ms(1500), .jitter = 1.0};
    for (int draw = 0; draw < 32; ++draw) {
        AURORA_TEST_CHECK_LE(wait_delay(tight, 1, std::nullopt), ms(1500));
    }
}

AURORA_TEST_CASE(retry_after_delay_seconds_saturates_instead_of_failing) {
    const auto now = std::chrono::system_clock::from_time_t(0);
    AURORA_TEST_CHECK_EQ(millis(parse_retry_after("5", now)), std::int64_t{5000});
    // 零秒是对端给的事实，与「没给」可分（后者为 -1）。
    AURORA_TEST_CHECK_EQ(millis(parse_retry_after("0", now)), std::int64_t{0});
    AURORA_TEST_CHECK_EQ(millis(parse_retry_after(" 12 ", now)), std::int64_t{12000});
    // 一年封顶：更大的数没有可等的意义，但也不能因为「位数太长」退化成「对端没给」。
    const std::string absurd(30, '9');
    AURORA_TEST_CHECK_EQ(millis(parse_retry_after(absurd, now)), std::int64_t{365LL * 86400 * 1000});
    // 非纯数字（负号、小数点、单位、空串）一律按「没给」处理，交给指数退避。
    for (const std::string_view junk : {std::string_view{"-1"}, std::string_view{"5.5"}, std::string_view{"5s"},
                                        std::string_view{""}, std::string_view{"   "}, std::string_view{"abc"}}) {
        AURORA_TEST_CHECK_EQ(millis(parse_retry_after(junk, now)), std::int64_t{-1});
    }
}

AURORA_TEST_CASE(http_date_is_pure_calendar_math) {
    const auto canonical = parse_http_date(AURORA_FIXDATE);
    AURORA_TEST_CHECK_EQ(since_epoch(require_value(canonical)), AURORA_FIXDATE_SECONDS * 1000);
    // 闰日与纪元边界同样只靠日序数算，不碰 `<ctime>` 的平台分支与本地化月份表。
    const auto leap = parse_http_date("Thu, 29 Feb 2024 12:00:00 GMT");
    AURORA_TEST_CHECK_EQ(since_epoch(require_value(leap)), std::int64_t{1709208000} * 1000);
    const auto epoch = parse_http_date("Thu, 01 Jan 1970 00:00:00 GMT");
    AURORA_TEST_CHECK_EQ(since_epoch(require_value(epoch)), std::int64_t{0});

    // 文法不合即「不是 IMF-fixdate」：已弃用的两型不做时区猜测，缺星期 / 缺 GMT / 定宽不满足都按没给处理。
    for (const std::string_view junk :
         {std::string_view{"Xyz, 06 Nov 1994 08:49:37 GMT"}, std::string_view{"Sun, 06 Nov 1994 08:49:37 UTC"},
          std::string_view{"Sun, 06 Nov 1994 08:49:37 +0000"}, std::string_view{"Sun, 06 Nov 1994 08:49:37"},
          std::string_view{"Sun, 06 Foo 1994 08:49:37 GMT"}, std::string_view{"Sun, 32 Nov 1994 08:49:37 GMT"},
          std::string_view{"Sun, 00 Nov 1994 08:49:37 GMT"}, std::string_view{"Fri, 16 Dec 1969 23:59:59 GMT"},
          std::string_view{"Sun, 06 Nov 10000 08:49:37 GMT"}, std::string_view{"Sun, 06 Nov 1994 24:00:00 GMT"},
          std::string_view{"sun, 06 nov 1994 08:49:37 gmt"}, std::string_view{"Sun, 6 Nov 1994 08:49:37 GMT"},
          std::string_view{"Sunday, 06 Nov 1994 08:49:37 GMT"}, std::string_view{""}}) {
        AURORA_TEST_CHECK_FALSE(parse_http_date(junk).has_value());
    }
}

AURORA_TEST_CASE(retry_after_date_is_measured_against_the_given_now) {
    const auto when = std::chrono::system_clock::from_time_t(AURORA_FIXDATE_SECONDS);
    AURORA_TEST_CHECK_EQ(millis(parse_retry_after(AURORA_FIXDATE, when - std::chrono::seconds{60})),
                         std::int64_t{60000});
    // 过期日期折成零等待（立刻再打一次），而不是一个负数。
    AURORA_TEST_CHECK_EQ(millis(parse_retry_after(AURORA_FIXDATE, when + std::chrono::seconds{1})), std::int64_t{0});
    // 已经过去的整点同样是「解得出、值为零」的形态：它与解不出是两件事。
    AURORA_TEST_CHECK_EQ(millis(parse_retry_after("Sun, 06 Nov 1994 07:49:37 GMT", when)), std::int64_t{0});
    // 两形态混在一枚字段里仍是两形态：先试秒数，不成才走日期。
    AURORA_TEST_CHECK_EQ(millis(parse_retry_after("Sun, 06 Nov 1994 09:49:37 GMT", when)), std::int64_t{3600000});
}

AURORA_TEST_CASE(retry_after_beats_the_backoff_and_skips_the_jitter) {
    const RetryPolicy policy{.base = ms(100), .max_backoff = ms(30000), .jitter = 0.5};
    // 服务器要的是一个时刻，不是「大概这个数量级」：这条等待不加抖动，抖只会更晚。
    for (int draw = 0; draw < 16; ++draw) {
        AURORA_TEST_CHECK_EQ(wait_delay(policy, 1, ms(7000)), ms(7000));
    }
    // 但仍受 `max_backoff` 封顶：对端写一枚一小时的 `Retry-After` 不该让本库等一小时。
    AURORA_TEST_CHECK_EQ(wait_delay(policy, 1, ms(3600000)), ms(30000));
    // 没给 `Retry-After` 才回到指数退避（抖动已由另一用例覆盖，这里关掉）。
    const RetryPolicy quiet{.base = ms(100), .max_backoff = ms(30000), .jitter = 0.0};
    AURORA_TEST_CHECK_EQ(wait_delay(quiet, 3, std::nullopt), ms(400));
    AURORA_TEST_CHECK_EQ(wait_delay(quiet, 3, ms(1)), ms(1));  // 零等待也是对端给的事实
    AURORA_TEST_CHECK_EQ(wait_delay(quiet, 3, ms(0)), ms(0));
}

}  // namespace aurora::test_cases::utest_net_http_retry
