#pragma once

// 系统**拖入**入口：**内部头**（与 `window/detail/system_drag_translate.h` 同列于 src/）。
//
// 为何单列：拖入与拖出不对称——拖出的发起方是本进程（`system_drag::begin`），拖入的发起方是
// **外部进程**（OS 会话回调平台桥）。CHG-023 冻结的 inert 三件套只覆盖拖出（`begin` /
// `emit_session_end`），拖入没有注入口，于是「合成 OS 侧四态序列」这条判据无从落地。本头补上
// 对称的一面：
//
//  * `dispatch` 是**宿主投事件的唯一入口**——生产路径也走它（平台桥把折算好的事件交给它、拿回
//    派发器写定的 `allowed` 去回写 OS 光标），故它不是纯测试设施；
//  * `install_test_drag_in` 一族是 inert 拖入后端：`active` 期间 `dispatch` 抑制真实派发，于是
//    测试可以完全接管拖入链而不被真机后端干扰（判据「安装期间真实后端不接管」即此）。
//
// 为何不在公共头：本提案不新增公共 API 符号（`aurora_api.json` 预期零差异），且使用方只有平台桥
// 与 `tests/`——二者都能 include src 内部头（`utest_ime_composition` 覆盖 `ime_composition` 的
// 先例即如此）。测试三件套受 `AURORA_ENABLE_DEBUG && AURORA_ENABLE_TEST_HOOKS` 双宏裁切，Release
// 下恒为 no-op，`dispatch` 本身不变（生产路径不受宏影响）。
//
// 门控：无。本头在**所有**平台编译（含 wasm）。

#include <functional>

#include "aurora/event/event.h"

namespace aurora::system_drag::ingress {

/// @brief 拖入派发槽：宿主把「投给派发链并返回写定的 `allowed`」包成这一枚回调。
///
/// 平台桥侧形如「调 `Win32Host::Impl::handler` 后返回 `e.allowed`」；测试侧形如「调
/// `EventDispatcher` 后返回 `e.allowed`」。槽由**调用方**持有而非全局登记：多窗口场景下进程级
/// 单槽会让后登记者覆盖先登记者，把槽作为入参就没有这份共享状态。
using DragInSink = std::function<DragAction(SystemDropEvent &)>;

/// @brief 把一次拖入回调折算好的事件投进派发链，返回派发器写定的 `allowed`。
///
/// 契约三点：
///  1. inert 拖入后端 `active` 期间**抑制派发**（返回 `None`、槽不被调用）——真实后端与测试注入
///     互斥，不叠加；
///  2. 槽为空 ⇒ 返回 `None`（无派发链可投，不谎报已派发）；
///  3. 只做「投事件 + 取回 `allowed`」，不读写任何会话态——会话态归平台桥与派发器。
/// @param event 待派发的事件（派发器会在调用内写定 `allowed`）。
/// @param sink 宿主 / 测试提供的派发槽。
/// @return 派发后写定的允许动作（抑制或槽为空时为 `DragAction::None`）。
[[nodiscard]] auto dispatch(SystemDropEvent &event, const DragInSink &sink) -> DragAction;

/// @brief 安装 inert 拖入后端：此后 `dispatch` 抑制真实派发（测试完全接管拖入链）。
/// @return 注入是否生效（双宏 `AURORA_ENABLE_DEBUG && AURORA_ENABLE_TEST_HOOKS` 未齐备时为 false）。
[[nodiscard]] auto install_test_drag_in() -> bool;

/// @brief 复位 inert 拖入后端（恢复真实派发）；未安装时为 no-op。
auto reset_test_drag_in() -> void;

/// @brief 卸载 inert 拖入后端并清态；未安装时返回 false。
/// @return 是否确有后端被卸载（双宏未齐备时为 false）。
[[nodiscard]] auto remove_test_drag_in() -> bool;

/// @brief inert 拖入后端是否处于接管态（真机探针与自检观测器用；Release 下恒 false）。
[[nodiscard]] auto test_drag_in_active() -> bool;

}  // namespace aurora::system_drag::ingress
