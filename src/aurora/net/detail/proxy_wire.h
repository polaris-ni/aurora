// proxy_wire.h —— 代理握手的字节层（仅库实现可见，不属公共 API）。
//
// 为何单列：CONNECT 与 SOCKS5 的「写什么字节 / 读回什么字节」是纯编码与纯解析，把隧道建到
// 一半再回退直连是禁止的（`net-proxy-unreachable` 的立身理由），而这条禁止要能被逐值验证，
// 前提就是它不依赖真实代理进程。收敛为自由函数后：单元侧可在零 socket 的条件下断言字节
// 逐值（含畸形回报的三态分岔），传输切片只需把字节喂进套接字并把本层的产出翻成错误码。
//
// 三条口径：
// - 解析产出一律是 `Result<std::optional<T>>` 三态：成功且有值 = 报文完整、成功且空 = 还没读够
//   （「没读到」不是错误，调用方据此继续喂字节）、失败 = 报文不合文法或超出头部上限。
// - 状态码到错误码的换算集中在本文件末两枚函数里，分岔口径固定：407 与 SOCKS5 方法 / 子协商
//   的认证失败落 `net-proxy-auth-required`；代理明确拒绝建隧道落 `net-proxy-unreachable`；
//   代理选用了本库不实现的形态落 `net-proxy-unsupported`。调用方不自行猜。
// - 密码只出现在写出的字节里，不进任何可打印形态（诊断文本只带代理与目标的 `host:port`）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "aurora/core/headers.h"
#include "aurora/core/result.h"
#include "aurora/net/address.h"
#include "aurora/net/proxy.h"

namespace aurora::net::wire {

/// @brief 头部块解析上限的推荐值（字节）：代理回报的头部块远超此长度即按畸形处理。
///
/// 取值依据：CONNECT 应答只需状态行与少量字段，16 KiB 已容下现实里最啰嗦的代理；不设上限
/// 会让一个持续不发空行的对端把读缓冲无界撑大，那是拒绝服务而非兼容性。
inline constexpr std::size_t AURORA_PROXY_MAX_HEAD_BYTES = std::size_t{16} * 1024;

/// @brief HTTP CONNECT 应答的头部块（状态行 + 字段），其后的字节属于隧道。
struct ConnectHead {
    std::uint16_t status{0};  ///< 三位状态码；只在头部块完整时给出
    std::string reason;  ///< 状态行的原因短语（原样保留，仅用于诊断文本）
    Headers headers;  ///< 应答字段（同名保序、ASCII 大小写不敏感存取）
    std::size_t header_bytes{0};  ///< 状态行 + 头部块占用字节数，其后即隧道首字节
};

/// @brief SOCKS5 CONNECT 应答（`RFC 1928 §6`）的解析产出。
struct Socks5Reply {
    std::uint8_t status{0};  ///< REP 字段原值，0 才表示隧道建成
    std::optional<Endpoint> bound;  ///< BND.ADDR + BND.PORT；域名型回报（ATYPE=3）时为缺失
    std::size_t consumed{0};  ///< 本应答占用字节数，其后即隧道首字节
};

/// @brief SOCKS5 握手的三个阶段，决定同一个非零状态字节该翻成哪条错误。
enum class Socks5Phase : std::uint8_t {
    MethodSelection,  ///< 方法协商回报（0xFF = 本库提供的方法一个都没被选中）
    Authentication,  ///< 用户名 / 密码子协商回报（`RFC 1928 §3`，非 0 = 凭据被拒）
    Tunnel,  ///< CONNECT 应答的 REP 字段
};

/// @brief 产出 HTTP `CONNECT` 请求字节（请求行 + `Host` + 可选 `Proxy-Authorization` + 空行）。
/// @param host 目标主机文本（IPv6 不带括号，本函数按需补括号）。
/// @param port 目标端口（主机序）。
/// @param credentials 代理凭据；`empty()` 时不发 `Proxy-Authorization`。
/// @return 可直接写入套接字的 UTF-8/Latin-1 字节串（以 CRLF 结尾的空行收束）。
[[nodiscard]] auto make_connect_request(std::string_view host, std::uint16_t port, const ProxyCredentials &credentials)
    -> std::string;

/// @brief 解析代理就 CONNECT 回报的头部块。
///
/// 分行同时接受 CRLF 与裸 LF（现实代理皆发 CRLF，而这里只读不回写，为一条换行差异把可用代理
/// 判成畸形不值得）；状态行要求 `HTTP/<major>.<minor> <NNN>`，缺原因短语允许。
/// @param bytes 已读到的字节（可含隧道首字节，只解析头部块）。
/// @param max_head_bytes 头部块上限，超出即按畸形回报失败（见 `AURORA_PROXY_MAX_HEAD_BYTES`）。
/// @return 三态见本文件头注：空 = 未出现终止空行、失败 = 不合文法或超上限。
[[nodiscard]] auto parse_connect_response(std::string_view bytes,
                                          std::size_t max_head_bytes = AURORA_PROXY_MAX_HEAD_BYTES)
    -> Result<std::optional<ConnectHead>>;

/// @brief 产出 SOCKS5 方法协商请求：无凭据只发 `0`（不认证），有凭据发 `0` 与 `2`（用户名 / 密码）。
/// @param with_credentials 是否提供用户名 / 密码子协商。
/// @return 五个字节以内的请求（版本、方法数、方法列表）。
[[nodiscard]] auto make_socks5_greeting(bool with_credentials) -> std::vector<std::uint8_t>;

/// @brief 解析代理选定的方法字节（`VER METHOD`）。
/// @param bytes 已读到的字节。
/// @return 三态；成功且有值时为选中的方法号（`0` / `2` / `0xFF`，其它值由调用方判为不支持）。
[[nodiscard]] auto parse_socks5_method(std::string_view bytes) -> Result<std::optional<std::uint8_t>>;

/// @brief 产出 SOCKS5 用户名 / 密码子协商请求（`RFC 1928 §3` 的 `VER=1` 形态）。
/// @param credentials 代理凭据；用户名或密码超过 255 字节即不合该子协商文法。
/// @return 成功为请求字节；失败为 `net-proxy-config-invalid`（长度越界，凭据本身没处安放）。
[[nodiscard]] auto make_socks5_auth_request(const ProxyCredentials &credentials) -> Result<std::vector<std::uint8_t>>;

/// @brief 解析子协商回报（`VER STATUS`）。
/// @param bytes 已读到的字节。
/// @return 三态；成功且有值时为状态字节原值（`0` 才通过）。
[[nodiscard]] auto parse_socks5_auth(std::string_view bytes) -> Result<std::optional<std::uint8_t>>;

/// @brief 产出 SOCKS5 CONNECT 请求（`VER CMD RESV ATYPE ADDR PORT`）。
///
/// ATYPE 只由「域名交给代理解析与否」与目标主机形态决定：`resolve_via_proxy` 为真走 `ATYPE=3`
/// 的域名形态（长度 1..255）；为假时本层不解析域名，目标必须是数字字面量，按族走 `ATYPE=1` /
/// `ATYPE=4`——把名字交给一个未被授权解析的代理是不可见的行为差异，宁可当场报错。
/// @param host 目标主机文本（IPv6 不带括号）。
/// @param port 目标端口（主机序）。
/// @param resolve_via_proxy 目标名是否交给代理解析（`socks5h` 语义）。
/// @return 成功为请求字节；失败为 `net-endpoint-invalid`（非 `h` 形态却给了名字、域名长度越界）。
[[nodiscard]] auto make_socks5_connect(std::string_view host, std::uint16_t port, bool resolve_via_proxy)
    -> Result<std::vector<std::uint8_t>>;

/// @brief 解析 SOCKS5 CONNECT 应答。
/// @param bytes 已读到的字节（可含隧道首字节）。
/// @param max_head_bytes 应答上限（域名型 ATYPE 单条最长 262 字节，正常应答远小于此）。
/// @return 三态；成功且有值时给出 REP 原值与可还原的绑定端点。
[[nodiscard]] auto parse_socks5_reply(std::string_view bytes, std::size_t max_head_bytes = AURORA_PROXY_MAX_HEAD_BYTES)
    -> Result<std::optional<Socks5Reply>>;

/// @brief 把 CONNECT 的非 2xx 状态码换算成隧道失败错误（2xx 由调用方判成成功、不来此处）。
/// @param status 应答状态码。
/// @param proxy 代理的 `host:port` 文本（诊断用，绝不含凭据）。
/// @param target 目标的 `host:port` 文本（诊断用）。
/// @return 407 → `net-proxy-auth-required`；其余非 2xx → `net-proxy-unreachable`（均不回退直连）。
[[nodiscard]] auto connect_tunnel_error(std::uint16_t status, std::string_view proxy, std::string_view target) -> Error;

/// @brief 把 SOCKS5 的非零状态字节换算成握手失败错误（按阶段分岔，同字节不同义）。
/// @param status 该阶段回报的状态字节原值。
/// @param phase 握手阶段。
/// @param proxy 代理的 `host:port` 文本（诊断用，绝不含凭据）。
/// @return 方法协商的 `0xFF` 与子协商非零 → `net-proxy-auth-required`；被选用不实现的形态 →
///         `net-proxy-unsupported`；其余（`REP` 非零的可达性类失败）→ `net-proxy-unreachable`。
[[nodiscard]] auto socks5_error_for(std::uint8_t status, Socks5Phase phase, std::string_view proxy) -> Error;

}  // namespace aurora::net::wire
