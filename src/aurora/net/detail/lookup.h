// lookup.h —— DNS 查表的平台腿与注入接缝（仅库实现可见，不属公共 API）。
//
// 为何单列：`getaddrinfo` 属平台 socket 头，仓内唯一落点是 `src/aurora/net/socket.cpp`
// （CHG-006 不变量，grep 判据），故公共面 `net::Resolver` 的实现只能经本头调用它。同时
// 「查表回报如何变成端点列表」必须能在无网络、无 DNS 的环境里逐值断言（离线 CI 与沙箱构建），
// 注入接缝因此只声明在此处而不进公共头——公共 API 不该存在「改变解析结果」的开关，
// 那样任何人都能让库连到他自己想去的机器上。
#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/address.h"

namespace aurora::net::detail {

/// @brief 平台查表腿：把 `getaddrinfo` 的地址记录映射为端点列表（族 + 字节 + 入参端口）。
///
/// 只取 `AF_INET` / `AF_INET6` 两族，其余族（如链路层记录）跳过；记录长度不足该族地址结构的
/// 同样跳过（平台回报形态属外部输入，宁可少一条也不读出未初始化字节）。产出保持平台给出的
/// 相对次序（排序属连接竞速，不在本层）。DNS 回报不带作用域号，故产出的 `zone()` 恒为空。
/// @param host 主机名文本；调用方（`Resolver::lookup`）已先把数字字面量短路掉。
/// @param port 端口（主机序），逐条填入产出的端点。
/// @return 成功为至少一条端点；运行时不可用为 `net-platform-unavailable`；查表失败或零条可用
///         记录为 `net-dns-failed`（message 携主机名与平台回报码）。
[[nodiscard]] auto platform_lookup(std::string_view host, std::uint16_t port) -> Result<std::vector<Endpoint>>;

/// @brief 查表钩子的函数形态（与平台腿同签名，故可整体替换）。
using LookupFn = std::function<Result<std::vector<Endpoint>>(std::string_view, std::uint16_t)>;

/// @brief 装入或清除查表钩子（仅测试与实现侧装配使用；传入空函数即清除，回落平台腿）。
/// @param hook 钩子本体。
/// @note 钩子是进程级状态：装入与清除须在没有并发解析时进行，否则读到的半条 `std::function`
///       是未定义行为。数字字面量的短路路径**不经**本接缝，故测试可同时证明「字面量不查网」。
auto set_lookup_hook(LookupFn hook) -> void;

/// @brief 取当前钩子（未装入时为空函数）。
/// @return 钩子的只读引用。
[[nodiscard]] auto lookup_hook() -> const LookupFn &;

}  // namespace aurora::net::detail
