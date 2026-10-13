// remote_connect.h —— 「名字 → 已建立的流」的编排面（仅库实现可见，不属公共 API）。
//
// 为何单列：本文件的三件事都不新增系统调用，只把已有的三件按次序与判据编排——`Resolver` 的端点
// 列表、`StreamTransport` 的连接出口、`wire` 的握手字节。塞进 `transport.cpp` 会让那层的并发契约
// 与两族连接策略混在一处；分开之后，竞速次序与隧道分岔能在零 DNS、零代理进程的条件下逐值断言
// （注入接缝是 `net::detail::set_lookup_hook` 与进程内假代理，都不进公共头）。
//
// 四条口径（规范性，落码与文档回写以此为准）：
// - **地址次序是连接策略而不是解析事实**：偏好族（IPv6）居首，其余按族交替（RFC 8305 §5.4 形态），
//   本机 IPC 形态在此被剔除（随本机 IPC 切片接入，不混进 TCP 竞速）。
// - **竞速是串行交错而不是并发齐发**：首条立即发起，其后每条在「上条发起后 `race_stagger`」与
//   「上条已出结果」两者先到时发起；最先建成者独占那次完成，落败者关闭。窗口置 0 即只发偏好族首条。
// - **代理隧道只在握手成功时成立**：不可达、被拒、不会说话一律以失败收尾，**绝不回退直连**；读过头
//   部块之后的字节经 `detail::seed_read` 接回该流的读队列前端，不会因为「一次读多了」而丢字节。
// - **完成恰一次，且恒回到发起线程**：解析回调、交错定时器、隧道时限都可能在引擎线程落地，故每次
//   进入编排状态前先归队到发起线程（与 `transport.cpp` 的 `deliver` 同一条通道、同一处局限）。
//   归队之后本文件的状态转换全部串行，因此不设互斥量——这不是省锁，而是「所有入口都在同一条线程
//   上」的直接推论，去掉归队即失效。
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "aurora/net/address.h"
#include "aurora/net/proxy.h"
#include "aurora/net/transport.h"

namespace aurora::net::detail {

/// @brief 按连接策略给候选端点排序：IPv6 首条居首，其后两族交替，剩余按原次序补齐。
///
/// 只改次序、不增不减（除剔除本机 IPC 形态），故调用方能拿同一份列表证明「一条都没丢」。
/// @param endpoints 解析产出（已规范化、已去重）。
/// @return 竞速次序的候选列表；无可连接项时为空。
[[nodiscard]] auto race_preference(const std::vector<Endpoint> &endpoints) -> std::vector<Endpoint>;

/// @brief 按主机名发起异步连接：解析 → 竞速 → 交出最先建成的那条流。
///
/// 入参不合用（空主机名、端口 0）与没有回调都在 `on_done` 之前判掉并以回调送达原因，不存在「既不
/// 成功也不失败」的第三种结局；解析失败的原因原样转达（`net-dns-failed` / `net-endpoint-invalid`），
/// 不在这里另造一套分岔。
/// @param host 目标主机文本（IPv6 字面量不带方括号，与 `Endpoint::from_ip` 同口径）。
/// @param port 目标端口（主机序）。
/// @param options 缓冲、超时与竞速口径。
/// @param cancellation 本次操作的取消令牌；逐条传给每个候选。
/// @param on_done 完成回调；恰一次抵达。
auto connect_by_name(std::string_view host, std::uint16_t port, const StreamOptions &options,
                     const Cancellation &cancellation, ConnectDone on_done) -> void;

/// @brief 经代理建立到 `host:port` 的隧道：连代理 → 握手 → 交出隧道里的流。
///
/// `ProxyKind::Direct` 转交直连入口；其余形态的代理自身也走「解析 + 竞速」（代理常常是双栈的）。
/// 握手的每个失败分支都经 `wire` 的换算口落成 `net-proxy-*` 系列原因，**任何分支都不回退直连**。
/// @param proxy 代理配置。
/// @param host 目标主机文本。
/// @param port 目标端口（主机序）。
/// @param options 缓冲、超时与竞速口径；`connect_timeout` 在此覆盖「直到隧道可用」的整体时限。
/// @param cancellation 本次操作的取消令牌。
/// @param on_done 完成回调；恰一次抵达。
auto connect_via_proxy(const ProxyConfig &proxy, std::string_view host, std::uint16_t port,
                       const StreamOptions &options, const Cancellation &cancellation, ConnectDone on_done) -> void;

}  // namespace aurora::net::detail
