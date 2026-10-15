#pragma once

/// @brief 主机名解析（CHG-018 双栈与竞速切片：把「名字 + 端口」变成可连接的端点集合）。
/// @file net/resolver.h
///
/// 本类型只有两枚入口，职责边界刻意窄：**数字字面量永远不查网**，**解析不排序**，**结果不缓存**。
/// - 字面量短路：`127.0.0.1` / `[::1]` / `fe80::1%2` 一类形态在构造期即由 `Endpoint::from_ip`
///   判定，产出一条端点就返回。把字面量交给 `getaddrinfo` 会让离线机器连本机都解析失败，且
///   平台对前导零、缩写形态的容忍度不一致（同一个写法在一台机器能连、另一台报错）。
/// - 不排序：Happy Eyeballs 的地址次序属**连接**策略而不是解析事实（同一列表在 IPv6-only
///   故障时应给出不同次序），且只有发起连接的一侧知道并发预算，故排序随连接竞速切片落地。
/// - 不缓存：库内缓存会与系统解析器自身的 TTL 形成两层过期语义，现场表现为「改了 DNS 记录但
///   本进程仍连旧地址」且无法解释；需要复用结果的调用方自行持有返回值。
///
/// 三条契约（规范性，落码与文档回写以此为准）：
/// - **产出恒已规范化且去重**：同族同地址同端口只保留首次出现的那条，次序即首次出现次序。
/// - **失败分两码**：空主机名是用法错误（`net-endpoint-invalid`），查不到是网络事实
///   （`net-dns-failed`）；把前者报成后者会让调用方以为重试有意义。
/// - **异步完成恰好一次**：`resolve` 立即返回，阻塞腿在引擎线程执行，完成回调经主线程投递器
///   投递（未装投递器时在当前线程就地执行，与 `Task<T>` 同口径）；引擎不可用时回调以
///   `net-platform-unavailable` 抵达，**而不是永不抵达**——静默悬挂的异步接口比失败的更难排查。
///
/// @code
/// aurora::net::Resolver::resolve("example.com", 443, [](auto &result) {
///     if (result) {
///         for (const auto &endpoint : result.value()) { /* 交连接竞速 */ }
///     }
/// });
/// @endcode
/// @note Thread: `lookup` 在调用线程执行阻塞查表（不要在 UI 线程调用）；`resolve` 可跨线程调用
/// @note Side-effects: `lookup` / `resolve` 触发 DNS 查询；`resolve` 可触发引擎线程惰性启动

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/address.h"

namespace aurora::net {

/// @brief 异步解析的完成回调（载荷为端点列表或结构化错误）。
using ResolveDone = std::function<void(Result<std::vector<Endpoint>>)>;

/// @brief 主机名解析入口（无状态，全静态方法）。
/// @note Thread: thread-safe（自身无共享状态；线程约束见各方法）
/// @note Side-effects: 见文件头
class Resolver {
  public:
    /// @brief 同步解析：在**调用线程**完成，可能阻塞至系统解析器返回。
    ///
    /// 数字字面量走短路径（不查网）；其余形态交平台解析器，失败时错误 message 携主机名与
    /// 平台回报码。本方法也是异步形态的实现体，故两口径的产出与诊断完全一致。
    /// @param host 主机名文本或数字字面量（IPv6 可带方括号与作用域号）。
    /// @param port 端口（主机序），逐条填入产出的端点。
    /// @return 成功为至少一条端点（规范化、去重、保持首次出现次序）；空主机名为
    ///         `net-endpoint-invalid`；查表失败或零条可用记录为 `net-dns-failed`。
    [[nodiscard]] static auto lookup(std::string_view host, std::uint16_t port) -> Result<std::vector<Endpoint>>;

    /// @brief 异步解析：阻塞腿在引擎线程执行，完成回投递一次。
    ///
    /// 调用线程不等待 DNS。引擎不可用（无引擎语义的腿，或无法重启）不静默丢弃请求，而是把
    /// `net-platform-unavailable` 作为结果交给回调。
    /// @param host 主机名文本或数字字面量（同 `lookup`）。
    /// @param port 端口（主机序）。
    /// @param on_done 完成回调；引擎线程完成查表后经主线程投递器执行，恰一次。
    static auto resolve(std::string_view host, std::uint16_t port, ResolveDone on_done) -> void;
};

}  // namespace aurora::net
