#pragma once

/// @brief 系统代理设置的平台无关投影与解析口（CHG-018 §11「系统代理读取」的纯逻辑侧）。
/// @file net/detail/system_proxy.h
///
/// 各平台把「操作系统里配了什么」压成同一枚 `SystemProxySnapshot` 的三个文本字段，本件负责把
/// 文本变成 `ProxyEnvironment`。**平台 API 一律不出现在这里**：WinHTTP 的 `INTERNET_PER_CONN_*`
/// 与 macOS 的 `SCDynamicStore` 键表差异只在各自的采集腿（`src/aurora/net/system_proxy.cpp`）里
/// 消化，本件的每条判据因此可在三平台与 wasm 上逐值断言。
///
/// 接缝（`set_system_proxy_hook`）刻意留在库内：公共 API 里放一枚「改变代理决策来源」的开关，
/// 等于让任何调用方能把企业网流量指到直连上去，与本库「绝不静默绕过代理」的纪律同源冲突。
///
/// @note Thread: thread-safe（纯函数；钩子槽与 `net::detail::lookup_hook` 同口径）
/// @note Side-effects: none（除显式调用 `set_system_proxy_hook` 外不产生副作用）

#include <functional>
#include <optional>
#include <string>

#include "aurora/core/result.h"
#include "aurora/net/proxy.h"

namespace aurora::net::detail {

/// @brief 系统代理设置的原始快照：字段文本一律按平台原样携带，本件不做二次规范化。
struct SystemProxySnapshot {
    /// @brief 系统里是否存在手工代理（PAC/WPAD 独占时为 false）。
    bool manual_enabled{false};
    /// @brief 代理服务器表：`;` 或 `,` 分隔，项可带 `scheme=host:port` 前缀。
    std::string server_list;
    /// @brief 例外清单：`;` 或 `,` 分隔，可含系统专属令牌 `<local>` 与 `<-loopback>`。
    std::string bypass_list;
    /// @brief 自动配置脚本 URL：非空即「本库不实现但确实配了」的形态。
    std::string pac_url;
    /// @brief WPAD 自动探测开关：与 `pac_url` 同属「意图是走代理」的形态。
    bool auto_detect{false};
};

/// @brief 把快照换算成决策集合（本件的唯一出口，平台腿与测试都走此处）。
///
/// 三条落码口径：① PAC / WPAD 一律报 `net-proxy-unsupported` 而不返回空集合——那两类配置的意图
/// 就是「流量须经代理」，静默当成「没配」会让办公网流量直连出去且无痕；② 系统里没配代理是正常
/// 状态，回空集合（一切直连）；③ 服务器表的项经 `ProxyConfig::parse` 与 `NoProxy::parse` 复用
/// 同一套主机文法，本件不新造第二套。
/// @param snapshot 平台采集到的快照。
/// @return 成功为集合；畸形项为 `net-proxy-config-invalid`（`detail=` 指到第几项），
///         PAC / WPAD / 只给出 SOCKS 等不实现形态为 `net-proxy-unsupported`。
[[nodiscard]] auto system_proxy_from_snapshot(const SystemProxySnapshot &snapshot) -> Result<ProxyEnvironment>;

/// @brief 系统代理采集函数：返回快照，本腿没有系统代理通道时返回空值。
using SystemProxyFn = std::function<Result<std::optional<SystemProxySnapshot>>()>;

/// @brief 装入 / 清除采集钩子（传空函数即清除，回落平台腿）。
/// @param hook 钩子本体。
auto set_system_proxy_hook(SystemProxyFn hook) -> void;

/// @brief 取当前钩子（空函数表示未装入）。
/// @return 钩子的只读引用。
[[nodiscard]] auto system_proxy_hook() -> const SystemProxyFn &;

/// @brief 平台采集腿：Win32 走 WinHTTP、macOS 走系统配置、其余腿返回空值（本腿的通道就是环境变量）。
/// @return 成功为快照或空值（空值 = 本腿没有系统代理通道）；通道存在而读取被拒为
///         `net-proxy-config-unreadable`（**绝不**折成空值——那等于把「读不到」说成「没配」）。
[[nodiscard]] auto platform_system_proxy() -> Result<std::optional<SystemProxySnapshot>>;

}  // namespace aurora::net::detail
