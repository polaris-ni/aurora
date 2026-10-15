// http_retry.cpp —— 重试判定与退避算式的实现。
//
// 本文件不含 socket、不含时钟推进、不含协程：它只回答四个问题（这枚失败要不要重放、这个状态码要不要重放、
// 这个方法能不能重放、该等多久），因此每一条判据都能在单测里逐值断言。时钟只从参数进来（`parse_retry_after`
// 的 `now`），日期算术是纯日历数学，故跨腿同值。
#include "aurora/net/detail/http_retry.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string_view>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/net/detail/noncrypto_random.h"
#include "aurora/net/http.h"

namespace aurora::net::detail {
namespace {

/// @brief 状态码里属于「服务器明说现在不行」的那一族（瞬时形态，语义见头文件的第三条口径）。
constexpr std::array<std::uint16_t, 6> AURORA_TRANSIENT_STATUS{408, 429, 500, 502, 503, 504};

/// @brief 允许重放的归因档（`Dns` / `Unreachable` / `Timeout`）；其余档一律不重放。
[[nodiscard]] auto class_admits_retry(aurora::net::http::HttpErrorClass error_class) -> bool {
    return error_class == aurora::net::http::HttpErrorClass::Dns ||
           error_class == aurora::net::http::HttpErrorClass::Unreachable ||
           error_class == aurora::net::http::HttpErrorClass::Timeout;
}

/// @brief 把「毫秒数」折成 64 位计数并夹到上限；任何溢出形态都只可能落到上限而不是绕回小值。
[[nodiscard]] auto saturate_milliseconds(std::int64_t value, std::int64_t cap) -> std::int64_t {
    if (value < 0) {
        return 0;
    }
    return std::min(value, cap);
}

/// @brief 抖动序列的推进体：转发到库内置的非密码学序列（口径与选型理由见 `detail/noncrypto_random.h`）。
///
/// 本文件不再自带状态与参数：抖动与链路标识要的只是同一件事——「不必安全，但必须散得开」，故共用一份
/// 进程内序列。这里保留一层转发的名字，是让下方 `wait_delay` 的读法仍指向「抖动的一个单位」这一语义。
[[nodiscard]] auto jitter_unit() -> double { return noncrypto_unit(); }

/// @brief 跳过首尾的空白（`OWS` = 空格与水平制表）。
[[nodiscard]] auto trim_ows(std::string_view text) -> std::string_view {
    constexpr std::string_view spaces = " \t";
    const auto begin = text.find_first_not_of(spaces);
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(spaces);
    return text.substr(begin, end - begin + 1);
}

/// @brief 把 `[first, last)` 的恰好 `width` 位数字读进 `out`；非数字即失败。
[[nodiscard]] auto parse_fixed_digits(std::string_view text, std::size_t &cursor, std::size_t width, int &out) -> bool {
    if (cursor + width > text.size()) {
        return false;
    }
    const auto field = text.substr(cursor, width);
    int value = 0;
    const char *first = field.data();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): from_chars 需要 [first, last)
    const char *last = first + field.size();
    const auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return false;
    }
    cursor += width;
    out = value;
    return true;
}

/// @brief 把整段文本读成「延迟秒数」：全为数字才成立，值到 `cap` 即饱和。
///
/// 刻意不走 `parse_fixed_digits()` 的 `int` 通道：`delay-seconds` 的位数不限，一位越不出界的长数字若被判为
/// 「不是秒数」，就会掉进日期文法再落回「对端没给」——那等于让服务器越强的限速声明越容易被忽略。
/// @param text 去掉首尾空白的字段原文。
/// @param cap 饱和上限（秒）。
/// @return 成功为不超过 `cap` 的秒数；含任何非数字字符为 `nullopt`。
[[nodiscard]] auto parse_delay_seconds(std::string_view text, std::int64_t cap) -> std::optional<std::int64_t> {
    std::int64_t value = 0;
    for (const char symbol : text) {
        if (symbol < '0' || symbol > '9') {
            return std::nullopt;
        }
        // 已达上限就不再进位：位数再长也只可能停在封顶，不可能溢出成负数或小值。
        if (value < cap) {
            value = std::min((value * 10) + (symbol - '0'), cap);
        }
    }
    return value;
}

/// @brief 三字母星期名是否合法（`RFC 9110 §5.6.7` 的固定拼写，大小写敏感）。
///
/// 星期位不参与日历算术，但它是文法的一部分：`"Xyz, 06 Nov 1994 ..."` 不是任何一个对端该写出的形态，
/// 放过它只会让「字段其实是别的日期格式」这一事实被静默吞下。
/// @param text 待判定的三字母片段。
/// @return 属于七枚规范拼写之一为 true。
[[nodiscard]] auto is_weekday(std::string_view text) -> bool {
    constexpr std::array<std::string_view, 7> names{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    return std::find(names.begin(), names.end(), text) != names.end();
}

/// @brief 由月份缩写查月序（1..12）；表是本库自有的三字母表，不依赖本地化的月份名。
[[nodiscard]] auto month_of(std::string_view name) -> int {
    constexpr std::array<std::string_view, 12> months{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    // 以迭代器距离而非下标取月序：本表是编译期定长的 `std::array`，裸下标在 tidy 眼里是「非常量下标」。
    const auto found = std::find(months.begin(), months.end(), name);
    if (found == months.end()) {
        return 0;
    }
    return static_cast<int>(std::distance(months.begin(), found)) + 1;
}

/// @brief 公历日序数（自 1970-01-01 起，可为负）。`RFC 5322` 的日历换算法，纯整数、无平台分支。
[[nodiscard]] auto days_from_civil(int year, int month, int day) -> std::int64_t {
    const int shifted_year = year - (month <= 2 ? 1 : 0);
    const int era = (shifted_year >= 0 ? shifted_year : shifted_year - 399) / 400;
    const int year_of_era = shifted_year - (era * 400);
    const int month_index = month + (month > 2 ? -3 : 9);
    const int day_of_year = (((153 * month_index) + 2) / 5) + day - 1;
    const int day_of_era = (year_of_era * 365) + (year_of_era / 4) - (year_of_era / 100) + day_of_year;
    return (static_cast<std::int64_t>(era) * 146097) + day_of_era - 719468;
}

}  // namespace

auto validate_retry_policy(const aurora::net::http::RetryPolicy &policy) -> std::optional<aurora::Error> {
    // 越界值在起手即拒（`net/datagram.h` 队列上限的同一条口径）：静默折算会让「以为开了抖动其实没开」等到
    // 线上重试风暴才现形。detail 点名是哪一枚，使修复动作不依赖读源码。
    if (policy.base.count() < 0) {
        return aurora::make_error(ErrorCode::HttpRequestInvalid, "retry policy: base must not be negative");
    }
    if (policy.max_backoff.count() < 0) {
        return aurora::make_error(ErrorCode::HttpRequestInvalid, "retry policy: max_backoff must not be negative");
    }
    if (policy.max_backoff < policy.base) {
        return aurora::make_error(ErrorCode::HttpRequestInvalid, "retry policy: max_backoff is smaller than base");
    }
    // 与 [0, 1] 的比较式对 NaN 恒假，故 NaN 也落进这里（不是漏网）。
    // NOLINTNEXTLINE(readability-simplify-boolean-expr): 取反式对 NaN 判真，DeMorgan 改写后判假，不等价
    if (!(policy.jitter >= 0.0 && policy.jitter <= 1.0)) {
        return aurora::make_error(ErrorCode::HttpRequestInvalid, "retry policy: jitter must be within 0..1");
    }
    return std::nullopt;
}

auto retry_enabled(const aurora::net::http::RetryPolicy &policy) -> bool { return policy.attempts > 1; }

auto is_idempotent_method(aurora::net::http::Method method) -> bool {
    return method == aurora::net::http::Method::Get || method == aurora::net::http::Method::Head ||
           method == aurora::net::http::Method::Put || method == aurora::net::http::Method::Delete;
}

auto should_retry_error(const aurora::Error &error) -> bool {
    if (!error.retryable) {
        return false;
    }
    return class_admits_retry(aurora::net::http::error_class_of(error.code_enum));
}

auto should_retry_status(std::uint16_t status) -> bool {
    return std::find(AURORA_TRANSIENT_STATUS.begin(), AURORA_TRANSIENT_STATUS.end(), status) !=
           AURORA_TRANSIENT_STATUS.end();
}

auto backoff_delay(const aurora::net::http::RetryPolicy &policy, std::uint32_t attempt) -> std::chrono::milliseconds {
    const std::int64_t cap = policy.max_backoff.count() < 0 ? 0 : policy.max_backoff.count();
    const std::uint64_t base = policy.base.count() < 0 ? 0 : static_cast<std::uint64_t>(policy.base.count());
    const std::uint32_t doublings = attempt > 1 ? attempt - 1 : 0;
    // 移位到 64 位边界即视为封顶：再往下算只可能溢出，不可能得到更小的等待。
    if (doublings >= 64 || base > (static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) >> doublings)) {
        return std::chrono::milliseconds{cap};
    }
    const std::uint64_t shifted = base << doublings;
    // 上面的界内闸已保证 `shifted` 落在 `int64` 可表示范围内，故这里与其余各档同走一枚封顶算子。
    return std::chrono::milliseconds{saturate_milliseconds(static_cast<std::int64_t>(shifted), cap)};
}

auto parse_http_date(std::string_view value) -> std::optional<std::chrono::system_clock::time_point> {
    // `IMF-fixdate = "Sun, 06 Nov 1994 08:49:37 GMT"`：定宽、定序、恒 GMT，故不需要时区数学。
    const std::string_view text = trim_ows(value);
    std::size_t cursor = 0;
    if (!is_weekday(text.substr(0, 3))) {
        return std::nullopt;
    }
    cursor += 3;
    if (cursor + 2 > text.size() || text[cursor] != ',' || text[cursor + 1] != ' ') {
        return std::nullopt;
    }
    cursor += 2;
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!parse_fixed_digits(text, cursor, 2, day)) {
        return std::nullopt;
    }
    if (cursor + 1 > text.size() || text[cursor] != ' ') {
        return std::nullopt;
    }
    ++cursor;
    const std::string_view month_name = text.substr(cursor, 3);
    const int month = month_of(month_name);
    if (month == 0) {
        return std::nullopt;
    }
    cursor += 3;
    if (cursor + 1 > text.size() || text[cursor] != ' ') {
        return std::nullopt;
    }
    ++cursor;
    if (!parse_fixed_digits(text, cursor, 4, year)) {
        return std::nullopt;
    }
    if (cursor + 1 > text.size() || text[cursor] != ' ') {
        return std::nullopt;
    }
    ++cursor;
    if (!parse_fixed_digits(text, cursor, 2, hour)) {
        return std::nullopt;
    }
    if (cursor + 1 > text.size() || text[cursor] != ':') {
        return std::nullopt;
    }
    ++cursor;
    if (!parse_fixed_digits(text, cursor, 2, minute)) {
        return std::nullopt;
    }
    if (cursor + 1 > text.size() || text[cursor] != ':') {
        return std::nullopt;
    }
    ++cursor;
    if (!parse_fixed_digits(text, cursor, 2, second)) {
        return std::nullopt;
    }
    // 只认 GMT：其余时区后缀（含 `UTC` / `+0000`）按不合本文法处理，不做「大概也是零偏移」的猜测。
    if (text.substr(cursor) != " GMT") {
        return std::nullopt;
    }
    // 界内性由日历算术自己判：月内日与时分秒越界都会落进回不了 1970..9999 的形态，这里显式挡在算式之前。
    if (year < 1970 || year > 9999 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
        return std::nullopt;
    }
    const std::int64_t days = days_from_civil(year, month, day);
    const std::int64_t seconds = (days * 86400LL) + (hour * 3600LL) + (minute * 60LL) + second;
    return std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
}

auto parse_retry_after(std::string_view value, std::chrono::system_clock::time_point now)
    -> std::optional<std::chrono::milliseconds> {
    const std::string_view text = trim_ows(value);
    if (text.empty()) {
        return std::nullopt;
    }
    // 形态一：延迟秒数（`RFC 9110 §10.2.3` 的 `delay-seconds`，非负整数、位数不限）。
    const std::int64_t year_cap = 365LL * 86400;  // 一年封顶：更大的数没有可等的意义
    if (const auto seconds = parse_delay_seconds(text, year_cap)) {
        return std::chrono::milliseconds{seconds.value() * 1000};
    }
    // 形态二：HTTP-date；过期的日期折成「零等待」，即立刻再打一次。
    const auto when = parse_http_date(text);
    if (!when.has_value()) {
        return std::nullopt;
    }
    const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(when.value() - now);
    return std::chrono::milliseconds{delta.count() < 0 ? 0 : delta.count()};
}

auto wait_delay(const aurora::net::http::RetryPolicy &policy, std::uint32_t attempt,
                const std::optional<std::chrono::milliseconds> &retry_after) -> std::chrono::milliseconds {
    const std::int64_t cap = policy.max_backoff.count() < 0 ? 0 : policy.max_backoff.count();
    if (retry_after.has_value()) {
        // 服务器要的是一个时刻，不是「大概这个数量级」，故这条等待不加抖动——抖动只会更晚。
        return std::chrono::milliseconds{saturate_milliseconds(retry_after->count(), cap)};
    }
    const auto raw = backoff_delay(policy, attempt);
    if (policy.jitter <= 0.0 || raw.count() == 0) {
        return raw;
    }
    const auto extra = static_cast<std::int64_t>(static_cast<double>(raw.count()) * policy.jitter * jitter_unit());
    return std::chrono::milliseconds{saturate_milliseconds(raw.count() + extra, cap)};
}

}  // namespace aurora::net::detail
