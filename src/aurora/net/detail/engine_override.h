#pragma once

/// @brief 引擎替身安装口（CHG-018 测试替身切片的库内接缝）。
/// @file net/detail/engine_override.h
///
/// **仅供测试设施使用，不占公共 API 预算。**存在理由：`Engine` 把「什么时候执行、什么时候到期」
/// 藏进接口之后，而自带引擎的到期时刻由真实单调时钟决定——超时、取消、竞速交错这三类判据因此
/// 只能靠「睡够再断言」来测，慢且抖。本接缝让测试把一个脚本化引擎交给 `Engine::instance()`，
/// 时限改由用例推进，时序变成逐值可断言的事实。
///
/// 三条纪律：
/// - **只在用例线程安装，且装/拆成对**（用 `framework/fake_engine.h` 的 RAII 包装，不要手写这对调用）；
///   替身期间常驻 `PollEngine` 不会被本层的任何路径拉起。
/// - **安装期不并发**：本接缝是进程级单点，替身生效期间同进程的其它线程若也走引擎路径，它们看到的就是
///   替身。测试 runner 用 `--run <suite>` 进程隔离，正是为了满足这一条。
/// - **拆除即恢复原样**：拆掉后 `Engine::instance()` 回到本腿的真实引擎，不留下任何状态。
///
/// 与 `detail::set_lookup_hook()`、`detail::set_system_proxy_hook()` 同一条思路：能改变运行期事实的
/// 开关一律留在库内，不进公共面。
///
/// @note Thread: 安装与读取跨线程可见（原子指针），安装动作本身约定在单线程完成
/// @note Side-effects: 改写进程级引擎入口的指向

#include "aurora/net/engine.h"

namespace aurora::net::detail {

/// @brief 装入引擎替身。
/// @param engine 替身对象指针；`nullptr` 等价于 `clear_engine_override()`。生命周期由调用方负责，
///               必须严格长于替身生效期。
auto set_engine_override(Engine *engine) -> void;

/// @brief 拆除引擎替身，恢复本腿的真实引擎。
///
/// 幂等：未装时为空操作。
auto clear_engine_override() -> void;

/// @brief 当前生效的替身（未装为空）。
/// @return 替身指针；供 `Engine::instance()` 与测试侧断言「此刻确实在替身之下」共用一处真值。
[[nodiscard]] auto engine_override() -> Engine *;

}  // namespace aurora::net::detail
