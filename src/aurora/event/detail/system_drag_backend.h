#pragma once

// 系统**拖出**真实后端注册面：**内部头**（与 `system_drag_ingress.h` 同列于 src/）。
//
// 为何单列：拖出的发起方是本进程，但 OS 会话的运行形态逐平台不同——X11 / Wayland / macOS 的
// 会话回调都在**UI 线程**的事件循环上到达（与拖入同侧），唯独 Win32 的 `DoDragDrop` 一旦调用
// 就在**调用线程**上跑 OS 固有的嵌套模态循环。规格已明文「不引入嵌套消息泵」（事件驱动执行
// 模型：`system_drag::begin` 发起即返回，终态经帧循环排空兑现），故 Win32 侧把会话搬到库自管
// 的拖拽线程上跑，OS 嵌套循环随之离开 UI 线程。无论哪种形态，后端与事件层之间只经本头的通道
// 交互，平台桥不自行决定投递时机——帧循环排空通则因此对四个平台一致：
//
//  ① 终态通道：`complete_drag_out_session(outcome, action)`（线程安全；登记终态，经
//     `system_drag::pump_reads()` 在帧内投递 `on_end`，含「Move 确认早于 on_end」的次序）；
//  ② 数据通道：`post_provide_request(request)`（跨线程要约索取：拖拽线程向 UI 线程要某个 MIME
//     的字节，UI 线程在 `pump_reads()` 排空内调 `DragOffer::provide` 应答——单线程 UI 纪律下，
//     控件数据只经 UI 线程读取，拖拽线程不得直接调用应用回调）。
//
// 为何不在公共头：不新增公共 API 符号（`aurora_api.json` 预期零差异），使用方只有平台桥与
// `tests/`——二者都能 include src 内部头（`utest_ime_composition` 的先例即如此）。
//
// 门控：无。本头在**所有**平台编译（含 wasm）。

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/event/event.h"

namespace aurora::detail {

/// @brief 一次跨线程要约索取：拖拽线程向 UI 线程要某个 MIME 的字节（Win32 `IDataObject::GetData`）。
///
/// 存在的理由：Win32 拖出会话跑在库自管的拖拽线程上，而 `DragOffer::provide` 是控件侧回调
/// （读控件数据），单线程 UI 纪律下只允许在 UI 线程调用。于是取数拆成两半——拖拽线程登记
/// 索取并阻塞等待，UI 线程在帧循环排空内应答。UI 线程不排空（宿主不调 `pump_reads`）时由
/// **拖拽线程侧按墙钟超时**自行失败兑现：不悬挂、不谎报。
struct ProvideRequest {
    std::string mime;  ///< 待索取的 MIME
    /// @brief 应答回调（UI 线程在排空阶段调用一次；携 `provide` 的返回原样交付）。
    std::function<void(Result<std::vector<std::uint8_t>>)> fulfill;
};

/// @brief 登记一次要约索取（跨线程安全）：结果由 UI 线程在 `system_drag::pump_reads()` 排空内应答。
/// @param request 索取请求（`fulfill` 只被调用一次）。
auto post_provide_request(ProvideRequest request) -> void;

/// @brief 取走全部待应答索取（`pump_reads` 与收口路径用；取出后登记表即空）。
/// @return 待应答清单（按登记序；无则为空）。
auto take_provide_requests() -> std::vector<ProvideRequest>;

/// @brief 在途读取 / 要约索取的墙钟超时（生产路径固定；测试经 `test_set_read_timeout` 调整）。
/// @return 当前超时时长。
[[nodiscard]] auto system_drag_read_timeout() -> std::chrono::milliseconds;

/// @brief 登记拖出会话终态（线程安全；无在途会话时 no-op）。
///
/// 平台桥在 OS 会话结束时调用；终态经帧循环投递（`Move` 时先调 `DragOffer::on_move_confirmed`
/// 确认，失败则终态改写为 `Failed` 并上报 `system-drag-move-rollback`）。
/// @param outcome 会话结局。
/// @param action 协商出的动作（`Succeeded` 时有效；其余结局传 `DragAction::None`）。
auto complete_drag_out_session(SystemDragOutcome outcome, DragAction action) -> void;

/// @brief 真实拖出后端接口（平台桥实现并注册；判优序见 `system_drag::begin`：inert 测试后端
/// 优先，真实 backend 次之，都无则 `system-drag-not-supported`）。
struct SystemDragOutBackend {
    /// @brief 发起一次拖出会话（发起即返回；OS 会话在平台侧运行）。
    /// @param offer 控件要约（`begin` 已校验合法性：非空 `formats` 与非空 `provide`）。
    /// @return 会话是否成功发起；false 时由 `system_drag::begin` 落 `system-drag-session-failed`
    ///         并回滚会话态（不悬挂）。
    /// @note 终结回调不经本入口传递：终态一律经 `complete_drag_out_session` 登记、帧循环投递。
    std::function<bool(const DragOffer &offer)> begin;
    /// @brief 取消在途会话（程序化取消；无在途会话为 no-op）。
    std::function<void()> cancel;
};

/// @brief 注册真实拖出后端（进程级单槽；重复注册以先到者为准，不覆盖）。
///
/// 不提供成对注销：拖出机制（OS 会话 + 库自管拖拽线程）与具体窗口句柄无关，窗口全关后机制
/// 依旧成立，故注册面进程级常驻；进程退出由 OS 回收。
auto register_system_drag_out_backend(SystemDragOutBackend backend) -> void;

/// @brief 查询已注册的真实拖出后端（未注册时为 nullptr）。
[[nodiscard]] auto system_drag_out_backend() -> const SystemDragOutBackend *;

}  // namespace aurora::detail
