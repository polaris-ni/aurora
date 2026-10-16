#pragma once

/// @brief wasm fetch 引擎桥（CHG-018 §16 第 37 项：wasm 腿的 HTTP 交换由浏览器 `fetch` 交付）。
/// @file net/detail/http_fetch.h
///
/// wasm 腿没有 fd 语义（`Engine` 维持 `NullEngine`），HTTP 腿是唯一例外：整次交换打包给浏览器
/// `fetch`，结局以回调抵达。本层只回答「一次交换的规格进、结局出」，协议语义（重定向链、超时
/// 改道、认证环、进度记账）全部留在发起侧既有环里——fetch 腿的接入点只有一处：`Client::send`
/// 的「建连并统一读写口」分支换挂 fetch 适配器。
///
/// 五条口径（规范性，落码与文档回写以此为准）：
/// - **重定向永不自动跟随**：fetch 以 `redirect: 'manual'` 发出，3xx 交回发起侧既有重定向链。
///   node 腿的 3xx 头可读、链照走；浏览器的 manual 重定向是**不透明应答**（状态 0、头体皆空），
///   本层如实落 `net-platform-unavailable` 并点名限制，绝不编造一条重定向。
/// - **Cookie 只经库内 Jar**：`credentials: 'omit'`，浏览器自带的 cookie 存储不参与（确定性与
///   D5b「Jar 只住 Session」同一条纪律）；`Set-Cookie` 经 `getSetCookie()` 尽收（平台不给就收不到，
///   见 §18-6 的「不可读」注记）。
/// - **受限项以能力位表达**：`Capabilities::custom_headers` 为 true（自定义头可传，预检失败以
///   浏览器错误形态抵达）；`Capabilities::streaming` 恒 false（本切片不消费 `ReadableStream`，
///   `open_stream` 的既有闸直接落 `net-platform-unavailable`，不做悄悄一次性物化）。
/// - **超时以浏览器侧闸承担**：`Timeouts::connect` 档折算成 `AbortController` 的截止时刻（整次
///   交换就是一次「建连 + 读写」）；`total` / `read_idle` 两档依赖引擎 `post_after`，本腿引擎
///   不可用故不计时——如实记录，不伪装已设限。
/// - **代理为空操作**：浏览器代管连接策略，发起侧以空代理环境跑同一条 `plan_route()`（策略闸
///   照常判定，代理决策恒 `Direct`）。
///
/// @note Thread: single-threaded（wasm 主线程；JS 回调经 `post_to_main` 恢复协程）
/// @note Side-effects: `fetch_exchange` 在 JS 全局登记一枚待决交换并可能启动引擎无关的网络请求

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "aurora/net/cancellation.h"
#include "aurora/net/detail/http_await.h"

namespace aurora::net::detail {

/// @brief fetch 腿是否在本构建里承接 HTTP 交换。
/// @return wasm 腿为 true；native 腿恒 false（调用方据此让交换走原生传输路径）。
[[nodiscard]] auto fetch_leg_active() -> bool;

/// @brief 一次 fetch 交换的规格（库内形态，调用方逐字段装配）。
/// @note Thread: single-threaded（随一次协程帧使用）
/// @note Side-effects: none
struct FetchSpec {
    std::string url;  ///< 绝对形态目标（规范化的 `Url::to_string()`）
    std::string method_text;  ///< 大写线上方法形态（`method_text()` 的产物）
    std::vector<std::pair<std::string, std::string>> headers;  ///< 请求头（连接管理字段已由调用方剥除）
    std::vector<char> body;  ///< 已物化的请求体（无体为空；流式体本腿不支持，发起侧前置拒）
    std::chrono::milliseconds deadline{0};  ///< 交换截止时刻；非正值 = 不设限
};

/// @brief 一次 fetch 交换的结局。
/// @note Thread: single-threaded（随一次协程帧使用）
/// @note Side-effects: none
struct FetchOutcome {
    /// @brief 结局形态。
    enum class Ending : std::uint8_t {
        Completed,  ///< 拿到应答（状态、头、体有效；状态 0 = 浏览器不透明应答，专属 manual 重定向）
        NetworkError,  ///< fetch 本身拒绝（网络错误 / CORS 预检被拒 / DNS 失败；浏览器不披露成因）
        Aborted,  ///< 被 `AbortController` 打断（截止到点或调用方取消，由调用方按令牌分辨）
    };

    Ending ending{Ending::NetworkError};  ///< 结局形态
    std::uint16_t status = 0;  ///< 应答状态码；`Completed` 时有效，0 = 不透明应答
    std::string reason;  ///< 应答短语（平台可不给，空即如实记空）
    std::string headers_text;  ///< 应答头合并文本（`"name: value\n"` 逐行；`Set-Cookie` 逐枚占一行）
    std::vector<char> body;  ///< 应答体（浏览器已透明解码，与声明头之间的口径由合成侧收口）
};

/// @brief 发起一次 fetch 交换并等它的结局。
///
/// 挂起直到 JS 侧结算抵达；调用方的取消令牌接了 JS 侧的 `AbortController`（取消即打断在途
/// fetch），结算后钩子自动撤登记。结局的语义分辨（取消 vs 超时 vs 网络错）留给调用方——
/// 本层只交「发生了什么」，不替上层编错误码。
/// @param spec 交换规格；引用须活过整个挂起期（协程帧上的局部对象正满足这条）。
/// @param token 交换的取消令牌。
/// @return 可等待体，求得一次结局（`CallbackAwaiter<T>` 口径：co_await 求得 `Result<T>`）。
[[nodiscard]] auto fetch_exchange(const FetchSpec &spec, const Cancellation &token)
    -> CallbackAwaiter<FetchOutcome>;

}  // namespace aurora::net::detail
