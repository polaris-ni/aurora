// http_auth_wire.h —— 认证字段的文法层与凭据编码（仅库实现可见，不属公共 API）。
//
// 为何单列：`WWW-Authenticate` 的「方案 + 认证参数」是一段带引号转义与逗号歧义的文法（`RFC 9110 §4.1`、
// `RFC 7235 §2.1`），而公共头要留给形态与判据。收敛成自由函数后，单元侧能在零 socket、零真实域控的条件
// 下逐值断言解析结果——含「quoted-string 里的逗号不是分隔符」「裸 token 参数」「同名头多条各成一枚方案」
// 这三处最容易写坏的地方。base64 也住在这里：它在本库只有两处用户，两处都是「把凭据写成一枚字段值」
// （`CONNECT` 的预防性 `Proxy-Authorization` 与 `Authorization: Basic`），而 `RFC 7617 §2.1` 的 UTF-8 组串
// 纪律必须由同一处执行，否则两条腿上的 Basic 会编出不一致的字节。
//
// 三条口径：
// - **解析失败即丢弃那一条方案，不报错**：畸形挑战的正确处置是「内置件没得选、答空、把对端说的那份
//   401 原样交出」，而不是把一次本可以如实交付的交换折成错误。调用方永远拿到一份结构合法的列表。
// - **同名头不折叠**：`values()` 保序返回，逐条各解析成一枚方案，次序即线上次序（多方案协商时它是
//   唯一的偏好信号）。
// - **参数键 ASCII 小写、值保留原文**：键按 HTTP 字段的不敏感口径归一（`qop` 与 `QOP` 是同一枚参数），
//   值一律不动——`realm` 是给人看的字符串，改一个字节就换了一个域。
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/net/http_auth.h"

namespace aurora::net::detail {

/// @brief 认证字段值的读取上限（字节）：一段超出此长度的挑战按不可解析处置。
///
/// 取值依据：一条挑战只需方案名加少数几枚参数，8 KiB 已容下现实里最啰嗦的域控；不设上限会让一个持续
/// 发字节的对端把解析产物撑大，那是拒绝服务而非兼容性（与代理头部块的上限同一口径）。
inline constexpr std::size_t AURORA_AUTH_FIELD_MAX_BYTES = std::size_t{8} * 1024;

/// @brief 解析一条认证字段值（`auth-scheme [ 1*( "," OWS auth-param ) ]`）。
///
/// 参数值同时接受 quoted-string（含 `\"` / `\\` 反转义）与裸 token；`realm` 单列到 `AuthMechanism::realm`
/// （同名多次出现时取首个）。整段不合文法（没有方案令牌、引号没闭合）时交回空，由调用方按「丢弃这一条」
/// 处置。
/// @param value 一条字段值的原文（不含字段名）。
/// @return 解析出的方案；不可解析时 `nullopt`。
[[nodiscard]] auto parse_auth_mechanism(std::string_view value) -> std::optional<aurora::net::http::AuthMechanism>;

/// @brief 按线上次序解析多条认证字段值。
/// @param values 同名头（`WWW-Authenticate` / `Proxy-Authenticate`）的全部值，保序且不折叠。
/// @return 逐条解析成功的方案列表；全不可解析时为空列表。
[[nodiscard]] auto parse_auth_mechanisms(const std::vector<std::string> &values)
    -> std::vector<aurora::net::http::AuthMechanism>;

/// @brief 标准 base64（`RFC 4648` §4，带填充，不换行）。
/// @param input 待编码字节。
/// @return 编码文本。
[[nodiscard]] auto base64_encode(std::string_view input) -> std::string;

}  // namespace aurora::net::detail
