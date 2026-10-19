// HTTP Cookie Jar 内核（§12「Cookie」条、提案 §16 第 36 项第⑥款）。
//
// `RFC 6265` 的最小集：name / value / domain / path / `Secure` / `HttpOnly` 加过期（`Expires` 与
// `Max-Age` 并给时 `Max-Age` 优先）。内存、不落盘、不做 `SameSite`——那是浏览器跨站模型的属性，本库
// 没有第一方 / 第三方之分，留空比猜一个语义诚实。
//
// 三条纪律写死在这里：
// - **`Secure` cookie 遇明文信道不发送也不存储**：存侧看「这次交换走的是不是 TLS」，取侧看目标 scheme。
//   这一条独立于 `NetPolicy` 的明文闸（策略管「这次交换允不允许发生」，Jar 管「这份凭据上不上这条信道」）。
// - **畸形 `Set-Cookie` 丢弃、不给调用方造一次失败**：一枚坏 cookie 不该让一次本可成功的交换失败
//   （与认证切片对畸形挑战的处置同一条哲学）。
// - **`Set-Cookie` 恒不折叠**：那是 `core::Headers` 的既有纪律（同名条目保序不折叠），取侧逐条处理。
//
// 只做接收侧的选择与回放（§5.3 存储 + §5.4 选择），不做写侧的域名校验强对称（公共同意后缀由取侧
// domain-match 兜住）；线程口径与连接池同格局：互斥量在本类内，出口只交值。

#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/headers.h"
#include "aurora/core/url.h"

namespace aurora::net::detail {

/// @brief 一枚已存的 cookie（`RFC 6265 §5.3` 的存储形态，最小属性集）。
struct HttpCookie {
    std::string name;  ///< 名（token，存原样）
    std::string value;  ///< 值（存原样；`RFC 6265` 不解释内容）
    std::string domain;  ///< 域（归 ASCII 小写、无前导点；host-only 时即请求主机本身）
    bool host_only{true};  ///< 纯主机 cookie（未带 `Domain` 属性；取侧只与注册名全等比对）
    std::string path;  ///< 路径（缺省走 `§5.1.4` 的 default-path 算法）
    bool secure{false};  ///< `Secure`：只上 TLS 信道
    bool http_only{false};  ///< `HttpOnly`：本库没有非 HTTP 信道，此位只如实存储
    std::optional<std::chrono::system_clock::time_point> expiry;  ///< 过期时刻；`nullopt` = 会话 cookie
    std::chrono::system_clock::time_point created;  ///< 创建时刻（§5.4 取侧排序的次级键；替换时保留旧值）
};

/// @brief Cookie Jar 本体：解析 `Set-Cookie`、存储（去重替换 + 过期清除 + 上限逐出）、按目标选择回放。
class HttpCookieJar {
  public:
    /// @brief 把一份应答里的全部 `Set-Cookie` 收进 Jar。
    /// @param target 本跳请求的目标 URL（域与缺省路径的基准；`is_secure()` 不在此判，用下一枚参数）。
    /// @param response_fields 应答头字段表（`Set-Cookie` 逐条取，不折叠）。
    /// @param secure_channel 本次交换是否走了 TLS（明文信道上的 `Secure` cookie 不存储）。
    /// @param now 现在的时刻（`Expires` 的比较基准与创建时刻的来源）。
    auto store_from_response(const aurora::Url &target, const aurora::Headers &response_fields, bool secure_channel,
                             std::chrono::system_clock::time_point now) -> void;

    /// @brief 按 `§5.4` 的选择算法为一个请求目标算出 `Cookie` 头的值。
    /// @param target 请求目标 URL（域匹配、路径前缀、安全位都对着它算）。
    /// @param now 现在的时刻（过期条目就地清除）。
    /// @return `name=value` 以 `"; "` 相连的字符串；没有可发的 cookie 时为空串（调用方据此不附头）。
    [[nodiscard]] auto cookies_for(const aurora::Url &target, std::chrono::system_clock::time_point now) const
        -> std::string;

    /// @brief 当前存量条数（含未过期的会话 cookie；判据用）。
    [[nodiscard]] auto size() const -> std::size_t;

    /// @brief 清空（判据用；正式面没有清空口——身份的终结是销毁 `Session`）。
    auto clear() -> void;

  private:
    /// @brief Jar 的条数上限；超出时先清过期、再按创建时刻逐出最老的。
    static constexpr std::size_t AURORA_COOKIE_JAR_MAX = 512;

    mutable std::mutex mutex_;  ///< 护表体；`Client` 拷贝可并发发起，取侧恒只交值
    std::vector<HttpCookie> cookies_;  ///< 存量（量级 = 上限，线性扫描足矣）
};

/// @brief 解析一枚 `Set-Cookie` 值（`RFC 6265 §5.2` 文法 + 属性集；畸形即 `nullopt` 整条丢弃）。
///
/// 独立成自由函数是为了让文法判据（`Max-Age` 优先序、域校验、default-path、畸形丢弃）在零网络下逐值
/// 断言；`HttpCookieJar::store_from_response` 是它唯一的库内消费者。
/// @param set_cookie_value 单枚字段值（`Set-Cookie` 恒不折叠，逐条给）。
/// @param target 本跳目标 URL（域校验与缺省路径的基准）。
/// @param now 现在的时刻（`Expires` 的比较基准）。
/// @return 解析产物；`nullopt` = 整条丢弃。
[[nodiscard]] auto parse_set_cookie(std::string_view set_cookie_value, const aurora::Url &target,
                                    std::chrono::system_clock::time_point now) -> std::optional<HttpCookie>;

}  // namespace aurora::net::detail
