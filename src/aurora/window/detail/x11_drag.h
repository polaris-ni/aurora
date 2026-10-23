#pragma once

// X11 Xdnd 拖放桥：**内部头**（与 `x11_modifiers.h` / `atspi_bridge.h` 同列于 src/）。
//
// 为何单列：Xdnd 是**消息协议**而不是一组 API——拖入侧要接 ClientMessage（`XdndEnter` /
// `XdndPosition` / `XdndLeave` / `XdndDrop`）、自己驱动 selection round-trip
// （`XConvertSelection` → `SelectionNotify` → 可能的 `INCR` 分片）；拖出侧要 grab 指针、向
// **外来窗口**发 ClientMessage 并以 `SelectionRequest` 应答取数。把这套状态机写进 `X11Surface`
// 的事件泵，会把「协议时序」混进「窗口事件映射」，两边都不可读、也不可单测。故协议态与时序
// 全在本桥内，表层只做「转发条目」（`x11_surface.cpp`）。
//
// 为何本头**不引 Xlib 头**（与 `win32_drag.h` 直引 `<windows.h>` 不同）：Xlib 的 `<X11/X.h>`
// 无条件 `#define None 0` 与 `#define CursorShape 0`，一旦进入头文件就会污染所有下游 TU 的
// `ModifierKey::None` 与 `aurora::CursorShape`。把它关在 `x11_drag.cpp` 单个 TU 内（取值后
// `#undef` 的既有纪律，见 `x11_surface.cpp`），污染面即被限制到该 TU。本头因此只用 `void *`
// 表达 `Display *`、用 `unsigned long` 表达 XID / Atom（X 协议下两者同宽）。
//
// 执行模型（事件驱动，与 Win32 桥同款、与 Win32 的线程形态不同）：X11 的指针 grab **不阻塞**
// 事件泵——motion / button / ClientMessage 照旧从本窗口队列到达，故拖出会话可以在 UI 线程上跑完，
// 不需要库自管线程（Win32 `DoDragDrop` 的嵌套模态循环才是要搬走的东西）。终态一律经
// `detail::complete_drag_out_session` 登记、由 `system_drag::pump_reads()` 在帧内投递。
//
// 门控：Linux 且非 Android 且 X11 后端（与 `x11_surface.cpp` 同款析取）。

#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_X11)

#include <array>
#include <functional>
#include <memory>

#include "aurora/event/event.h"

namespace aurora::detail {

/// @brief X11 Xdnd 拖放桥：把 Xdnd 消息协议双向翻译成 `SystemDropEvent` 四态与 `DragOffer` 会话。
///
/// 生命周期由**窗口宿主**持有（`X11Surface::Impl` 唯一持有）：`install()` 广告 `XdndAware` 并
/// 注册进程级拖出后端，`uninstall()` 撤广告并收摊在途会话。两侧会话态互不重叠：
///   - 拖入（本窗口是 target）：外部源驱动，桥应答 `XdndStatus` / `XdndFinished`；
///   - 拖出（本窗口是 source）：`system_drag::begin` 经后端槽进入，桥驱动 grab 与消息序列。
///
/// 同一窗口同时作为源与目标（自拖放）天然成立：消息经 X 服务器回到自己的队列，`SelectionRequest`
/// 由本桥在 UI 线程直调 `DragOffer::provide` 应答（单线程 UI 纪律下该回调不跨线程）。
/// @note Thread: main-thread only（X 事件泵与控件回调同线程）
class X11DragBridge final {
  public:
    /// @brief 上层通道：事件投递与缩放查询，全部由宿主提供。
    struct Hooks {
        std::function<void(Event &)> emit;  ///< 投给宿主派发器（`allowed` 由派发器在调用内写定）
        std::function<float()> scale_factor;  ///< 当前 DPI 缩放（物理像素 → 逻辑 dp 的除数）
    };

    /// @brief 构造：不触碰 X（全部 X 调用集中在 `install()`，便于宿主按返回值降级）。
    /// @param display `Display *`（以 `void *` 传入：本头不引 Xlib，见文件头）。
    /// @param window 客户窗口 XID。
    /// @param hooks 上层通道。
    X11DragBridge(void *display, unsigned long window, Hooks hooks);
    ~X11DragBridge();

    X11DragBridge(const X11DragBridge &) = delete;
    auto operator=(const X11DragBridge &) -> X11DragBridge & = delete;
    X11DragBridge(X11DragBridge &&) = delete;
    auto operator=(X11DragBridge &&) -> X11DragBridge & = delete;

    /// @brief 广告 `XdndAware`、intern 协议 atom、装 X 错误处理器并注册进程级拖出后端。
    ///
    /// 失败只有一种常见来源：display 已断开 / 窗口已销毁。此时如实返回 false，由宿主记日志并按
    /// 「该窗口不参与 Xdnd」处理——**不谎报已有通路**。
    /// @return 是否成功；重复调用返回 false（不重复注册）。
    [[nodiscard]] auto install() -> bool;

    /// @brief 撤广告、收摊在途会话（在途拖出按 `Cancelled` 兑现）并清全部会话态。
    auto uninstall() -> void;

    /// @brief 投一条 ClientMessage（Xdnd 全族与 `XdndStatus` / `XdndFinished` 回程都经此）。
    /// @param message_type 消息类型 atom（`ev.xclient.message_type`）。
    /// @param data 五个 32 位载荷字（X 协议下 ClientMessage 恒为 5 个 long）。
    auto on_client_message(unsigned long message_type, const std::array<long, 5> &data) -> void;

    /// @brief 投一条 `SelectionNotify`（拖入侧 selection round-trip 的到达面）。
    /// @param selection selection atom（非 `XdndSelection` 一律忽略）。
    /// @param target 被请求的目标 atom。
    /// @param property 结果属性名；**0（atom `None`）表示该目标无数据**。
    auto on_selection_notify(unsigned long selection, unsigned long target, unsigned long property) -> void;

    /// @brief 投一条 `SelectionRequest`（拖出侧应答取数；外来目标在 drop 后索取数据）。
    /// @param requestor 请求方窗口 XID。
    /// @param selection selection atom（非 `XdndSelection` 一律拒绝）。
    /// @param target 请求的目标 atom（未广告 / 未登记时拒绝）。
    /// @param property 请求方指定的结果属性名（拒绝时回 0）。
    /// @param time 请求时间戳（原样回给请求方）。
    auto on_selection_request(unsigned long requestor, unsigned long selection, unsigned long target,
                              unsigned long property, unsigned long time) -> void;

    /// @brief 投一条 `PropertyNotify`（`INCR` 分片的推进面：拖入收块、拖出发块）。
    /// @param window 属性所在窗口（拖入侧是本窗口；拖出侧是外来请求方窗口）。
    /// @param atom 属性名。
    /// @param state `PropertyNewValue`（0）/ `PropertyDelete`（1）。
    auto on_property_notify(unsigned long window, unsigned long atom, int state) -> void;

    /// @brief 投一次指针移动（拖出侧据此推进 `XdndPosition` 与目标切换）。
    /// @param state X 修饰态掩码（与 `XMotionEvent::state` 同语义）。
    /// @param x_root 指针根坐标 X（物理像素）。
    /// @param y_root 指针根坐标 Y（物理像素）。
    /// @param time 事件时间戳（原样回给目标，供其 selection 请求使用）。
    auto on_motion(unsigned int state, int x_root, int y_root, unsigned long time) -> void;

    /// @brief 投一次按键释放（拖出侧的落放触发点：按下即发起、释放即落放）。
    /// @param time 事件时间戳。
    auto on_button_release(unsigned long time) -> void;

    /// @brief 投一次按键按下（拖出侧：`Escape` 取消会话，其余键在会话期内吞噬）。
    /// @param key_code 平台无关键码（`KeyCode` 枚举值）。
    /// @return 是否被拖出会话消费（true 时宿主不得再派发该按键）。
    [[nodiscard]] auto on_key_press(int key_code) -> bool;

    /// @brief 帧循环看守：在途读取 / 等待 `XdndFinished` 的墙钟超时在此兑现（不悬挂）。
    auto tick() -> void;

    /// @brief 本桥是否正在发起拖出（宿主据此屏蔽拖出期内的普通指针派发）。
    [[nodiscard]] auto drag_out_active() const -> bool;

    /// @brief 本桥是否处于拖入会话中（宿主据此屏蔽拖入期内的重复悬停派发）。
    [[nodiscard]] auto drag_in_active() const -> bool;

    /// @brief 登记「当前发起窗口」槽（进程级；由宿主在事件泵入口刷新，供拖出后端解析发起桥）。
    /// @param bridge 发起桥（可为空：窗口销毁时清槽）。
    static auto note_initiator(X11DragBridge *bridge) -> void;

  private:
    struct Impl;  ///< 协议状态机（定义在 `x11_drag.cpp`：含全部 atom 与会话态）
    std::unique_ptr<Impl> impl_;  ///< 状态机（唯一所有权）
    void *display_ = nullptr;  ///< `Display *`（void 形态：本头不引 Xlib）
    unsigned long window_ = 0;  ///< 客户窗口 XID
    Hooks hooks_;  ///< 上层通道

    /// @brief 发起一次拖出会话（进程级后端槽的唯一落点；由 `system_drag::begin` 经后端进入）。
    /// @param offer 控件要约（事件层已校验 `formats` 与 `provide` 非空）。
    /// @return 会话是否成功发起（grab / 取得 selection 所有权失败时为 false）。
    auto begin_drag_out(const DragOffer &offer) -> bool;
    /// @brief 取消在途拖出会话并兑现 `Cancelled`（`system_drag::cancel` 与 `Escape` 共用）。
    auto cancel_drag_out() -> void;
    /// @brief 取发起槽内的桥（多窗口下由宿主在事件泵入口刷新）。
    [[nodiscard]] static auto initiator() -> X11DragBridge *;
    /// @brief 注册进程级拖出后端（先到者优先；由 `install()` 调用一次）。
    static auto register_backend() -> void;
};

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_LINUX && !ANDROID && AURORA_BACKEND_X11
