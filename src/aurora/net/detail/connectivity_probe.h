#pragma once

/// @brief 链路态采集接缝：把三平台原语压成同一枚无平台类型的读数。
/// @file net/detail/connectivity_probe.h
///
/// 本件只含三样：`ProbeReading`（内部读数形态）、`compiled_source()`（编译期腿令牌）、
/// Windows 查询 `probe_default_route_win32()` 的**声明**与 `probe_default_route()`（分派）。
/// 平台头一律不出现在这里：Windows 的 `<winsock2.h>` / `<iphlpapi.h>`、macOS 的
/// `SystemConfiguration`、Linux 的路由伪文件与 sysfs 全在各自采集腿里消化。
///
/// 两条不变量（本件对它负责）：
/// - **平台 socket 头唯一落点**仍是 `src/aurora/net/socket.cpp`。Windows 查询的实现写在那个
///   文件里，对外只经 `probe_default_route_win32()` 交出一枚 `optional<bool>`。
/// - **零出站 I/O**：采集只读 OS 表 / 伪文件 / 动态存储，不得 `connect` / `sendto` / `getaddrinfo`。
///
/// @note Thread: `probe_default_route()` 只读 OS 状态，可在引擎线程调用；不作跨线程同步保证。
/// @note Side-effects: 读 OS 路由与接口状态。
#include <optional>

#include "aurora/core/platform.h"  // 编译期平台宏（`AURORA_PLATFORM_*`）：`compiled_source()` 的分派依赖它
#include "aurora/net/connectivity.h"

namespace aurora::net::detail {

/// @brief 一次同步采集的内部读数：只含「有 / 没有 / 读不到」三种之一。
///
/// 刻意**不含**接口名、网关地址、SSID、链路本地地址——那些是拓扑事实，不进任何导出面，
/// 连内部快照都不留（采集 TU 内部用于拼路径的接口名，函数返回即丢弃）。
struct ProbeReading {
    std::optional<bool> online;  ///< 有可用缺省路由 / 没有 / 读不到（空值）
};

/// @brief 本构建编译进来的采集腿令牌（编译期常量，构造后不随读数成败变化）。
/// @return Windows 为 `IpHelper`；Linux 为 `LinkFiles`；其余（含 wasm 与 macOS）为 `None`。
[[nodiscard]] auto compiled_source() -> ConnectivitySource;

/// @brief Windows 采集腿：查路由表与接口表，只交一枚无平台类型的读数。
///
/// 实现在 `src/aurora/net/socket.cpp` 内（平台 socket 头的既有唯一落点），故本函数
/// 的签名不含任何平台类型。
/// @return 存在经非回环、operational 接口的缺省路由时为 true；明确读到无可用缺省
///         路由为 false；API 失败或接口状态读不到为空值。
[[nodiscard]] auto probe_default_route_win32() -> std::optional<bool>;

/// @brief 本构建是否存在「建连旁证通路」：连接结局能否成为链路态的证据。
///
/// wasm 的 fetch 腿不经全库唯一的建连出口，故它对链路态**恒无贡献**——这是如实记录的缺口，
/// 不假装补上。其余平台至少有一条能产生旁证的通路。
/// @return wasm 为 false；其余为 true。
[[nodiscard]] auto has_evidence_path() -> bool;

/// @brief 采集腿分派：按编译期平台宏选腿，其余平台恒回「读不到」。
/// @return 本腿读数；无采集腿时 `online` 为空值。
[[nodiscard]] auto probe_default_route() -> ProbeReading;

}  // namespace aurora::net::detail
