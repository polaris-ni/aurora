// http_retry.h —— 重试策略的判定与算式。
//
// 为何单列：一次交换该不该再打一遍、隔多久打，是一套有独立判据的纯算式（幂等白名单、归因档交集、状态码
// 那一族、指数 + 抖动 + 封顶、`Retry-After` 的两形态）。它一旦写进 `Client::send` 的协程体，就只能靠真连接
// 与真对端才能测——那既测不出「取消的那一枚不该被重放」，也测不出退避有没有封顶。落在这里，零 socket、
// 时钟由参数注入即可逐值断言；发起侧只在三处各叫一声：起手校验形态、拿到结局问要不要重试、决定重试前问该
// 等多久。
//
// 五条口径（规范性，落码与文档回写以此为准）：
// - **判据不新造**：能不能重试由两份既有事实共同决定——`Error::retryable`（`codespec/errors.toml` 表驱动）
//   与 `error_class_of()` 的归因档，取交集而非另立第三份真值源。只读 `retryable` 会把 `net-cancelled`
//   也重放（它确实标着可重试），而重放用户主动取消的请求等于违抗那次取消。
// - **档位只收三档**：`Dns` / `Unreachable` / `Timeout`。`Protocol`（报文不合语法）与 `Invalid`（用法、
//   能力、本库上限）重放一万次也是同一个错；`Tls` 与 `Proxy` 的失败在「对端身份 / 配置」上，重放只会
//   再撞一次同一道墙；`Cancelled` 如上；`Status` 档没有码可映射（非 2xx 是成功的 `Result`），走状态码
//   那条独立分岔。
// - **状态码那一族只收瞬时的**：408 / 429 / 500 / 502 / 503 / 504。`Retry-After` 是服务器亲口给的等待
//   指令，故有它就以它为准（仍受 `max_backoff` 封顶），没有才用指数退避。
// - **`Retry-After` 两形态都解，解不出即当没给**：延迟秒数与 IMF-fixdate 都是 `RFC 9110 §10.2.3` 承认的
//   写法；过期的日期折成「零等待」而非「负数」。已弃用的两型（RFC 850 / asctime）不解——本库自产的日期
//   恒是 IMF-fixdate 形态，对端给弃用形态时按「没给」走指数退避，比猜一个时区数学更诚实。
// - **抖动不是随机数**：它只负责打散同一时刻整批发起的节奏，不需要也不可预测，故取库内置的非密码学序列
//   （进程内播种、逐次推进）。密码学质量的熵在这里没有消费者，却会让单测只能退化成区间断言。
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

#include "aurora/core/result.h"
#include "aurora/net/http.h"

namespace aurora::net::detail {

/// @brief 策略形态闸：把越界字段在起手时挡下（`RetryPolicy` 的头注释即此条裁决的出处）。
/// @param policy 待校验的策略。
/// @return 形态合法为空 `nullopt`；越界为 `http-request-invalid`，detail 点名是哪一枚字段。
[[nodiscard]] auto validate_retry_policy(const aurora::net::http::RetryPolicy &policy) -> std::optional<aurora::Error>;

/// @brief 策略是否开着（`attempts > 1`）。
/// @param policy 待判定的策略。
/// @return 开着为 true；关闭时上面几枚判定一律不必询问。
[[nodiscard]] auto retry_enabled(const aurora::net::http::RetryPolicy &policy) -> bool;

/// @brief 方法是否在幂等白名单内（`GET` / `HEAD` / `PUT` / `DELETE`）。
///
/// 白名单按 `RFC 9110` 的幂等定义直接取方法律，不做成可调集合：重放非幂等请求的代价（把一次提交变成两次）
/// 在失败现场无从判定，而它恰是「响应没收到」这一事实本身所暗示的风险。
/// @param method 请求方法。
/// @return 可安全重放为 true。
[[nodiscard]] auto is_idempotent_method(aurora::net::http::Method method) -> bool;

/// @brief 一枚失败是否值得重放：`retryable` 与归因档的交集。
/// @param error 失败载荷（取其 `retryable` 字段与 `code_enum` 的档位）。
/// @return 值得重放为 true。
[[nodiscard]] auto should_retry_error(const aurora::Error &error) -> bool;

/// @brief 一个应答状态码是否属于「服务器明说现在不行」那一族。
/// @param status 三位状态码。
/// @return 属于 408 / 429 / 500 / 502 / 503 / 504 为 true。
[[nodiscard]] auto should_retry_status(std::uint16_t status) -> bool;

/// @brief 第 `attempt` 次尝试失败后的指数退避基准（不含抖动与封顶）。
///
/// 第 1 次失败后等 `base`，其后逐次翻倍，直到 `max_backoff` 封顶。乘法在 64 位上饱和，故 `base` 与
/// `attempts` 的组合再怎么离谱也只会落到上限，不会绕回小值或溢出成负数。
/// @param policy 重试策略。
/// @param attempt 已完成的尝试序号（从 1 起）。
/// @return 抖动前的等待时长。
[[nodiscard]] auto backoff_delay(const aurora::net::http::RetryPolicy &policy, std::uint32_t attempt)
    -> std::chrono::milliseconds;

/// @brief 解析 `Retry-After` 字段（延迟秒数或 IMF-fixdate 两形态）。
/// @param value 字段原文。
/// @param now 判定基准时刻（由调用方给出，使日期形态可复现地断言）。
/// @return 该等的时长；字段不可解析为 `nullopt`（视为对端没给）。
[[nodiscard]] auto parse_retry_after(std::string_view value, std::chrono::system_clock::time_point now)
    -> std::optional<std::chrono::milliseconds>;

/// @brief 解析 IMF-fixdate（`RFC 9110 §5.6.7` 的唯一规范性日期形态）为时刻。
///
/// 纯日历算术（日序数差），不碰 `<ctime>` 的平台分支与本地化月份表，故三条腿同值、可单测。
/// @param value 字段原文（须含收尾的 `" GMT"`）。
/// @return 成功为对应时刻；文法不合或字段超出可表示范围为 `nullopt`。
[[nodiscard]] auto parse_http_date(std::string_view value) -> std::optional<std::chrono::system_clock::time_point>;

/// @brief 下一次尝试前该等多久：有 `Retry-After` 以它为准，没有则用指数退避；两者同受 `max_backoff` 封顶。
/// @param policy 重试策略。
/// @param attempt 已完成的尝试序号（从 1 起）。
/// @param retry_after `parse_retry_after()` 的结果（`nullopt` = 对端没给或解不出）。
/// @return 等待时长（恒非负）。
[[nodiscard]] auto wait_delay(const aurora::net::http::RetryPolicy &policy, std::uint32_t attempt,
                              const std::optional<std::chrono::milliseconds> &retry_after) -> std::chrono::milliseconds;

}  // namespace aurora::net::detail
