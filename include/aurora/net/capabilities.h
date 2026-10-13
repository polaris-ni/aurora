#pragma once

/// @brief 传输引擎的运行期能力声明（CHG-010 底座切片：能力差异经查询表达，公共 API 面零宏）。
/// @file net/capabilities.h
///
/// 使用方据本结构**运行期**降级，而不是靠平台宏猜（`SPEC.PLATFORM.ZERO-IFDEF.001` 的同构要求）。
/// 唯一真值源是 `Engine::instance().capabilities()`：引擎未落地某形态时该位恒为 false，随对应切片
/// 置真——先立一个无人填写的真相源比不立更糟，因为它会诱导调用方去分支一条今天不存在的能力。
///
/// 语义分工（提案 §3/§9/§18-6，规范性）：
/// - `tcp` / `udp` / `local_ipc` / `dns` / `listens` 表达**本引擎腿是否承载该传输形态**（句柄、
///   等待与建立语义齐备）；`tls` 表达安全通道是否可用（§5 的 provider 探测结果，Linux 无 provider
///   即为 false）。
/// - `event_source` 表达「可并入宿主事件源」，落点是 `app::EventSourceRegistry` 的运行期开关：
///   wasm 腿无 fd 语义，注册即返回 `net-platform-unavailable`，**不提供假降级**。
/// - `streaming` / `custom_headers` 表达**平台限制**而非实现进度：浏览器 `fetch` 受 CORS 预检与
///   `ReadableStream` 支持度约束，native 腿不受这些约束故恒 true。
///
/// 具名 bool 而非位标志（§18 次要建议裁决）：它是运行期查询而非位掩码协议，直读对 AI 更友好；
/// 将来加字段即可，无膨胀压力。
///
/// @code
/// const auto caps = aurora::net::Engine::instance().capabilities();
/// if (!caps.udp) { /* 本腿不承载数据报：改走 TCP 或报 net-platform-unavailable */ }
/// @endcode
/// @note Thread: thread-safe（值类型；由 `Engine::capabilities()` 每次按当前运行期状态组装）
/// @note Side-effects: none

#include <cstdint>

namespace aurora::net {

/// @brief 一条引擎腿的能力位集合（缺省全 false：未声明即不可用）。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct Capabilities {
    bool tcp = false;  ///< 任意主机 TCP 字节流（非仅回环）
    bool udp = false;  ///< UDP 数据报（含组播）
    bool local_ipc = false;  ///< 本机 IPC：UnixSocket / NamedPipe
    bool dns = false;  ///< 名称解析（`Resolver`）
    bool tls = false;  ///< TLS 安全通道（provider 探测结果）
    bool listens = false;  ///< 服务端监听（浏览器侧无此语义）
    bool event_source = false;  ///< 可并入宿主事件源（wasm 为 false）
    bool streaming = false;  ///< 流式响应体（wasm 无 ReadableStream 时为 false）
    bool custom_headers = false;  ///< 可自由设非安全请求头（wasm 受 CORS 预检约束）
};

}  // namespace aurora::net
