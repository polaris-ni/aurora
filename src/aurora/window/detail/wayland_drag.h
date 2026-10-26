#pragma once

// Wayland 系统拖放桥：**内部头**（与 `x11_drag.h` / `atspi_bridge.h` 同列于 src/）。
//
// 为何单列：Wayland 的拖放是**对象协议**而不是一组 API——拖入侧要接 `wl_data_device` 的
// `enter` / `motion` / `leave` / `drop` 与 `wl_data_offer` 的 `offer` / `source_actions` /
// `action`，落放后经**管道**（`wl_data_offer.receive` 给一个 fd，对端写、本端读）把字节取回；
// 拖出侧要建 `wl_data_source`、广告 MIME 名、用 `wl_data_device.start_drag` 起手，并在
// `send` 事件里把字节写回对端 fd。把这套时序写进 `WaylandSurface` 的事件泵，会把「协议时序」
// 混进「窗口事件映射」，两边都不可读、也不可单测。故协议态与时序全在本桥内，表层只做「转发
// 条目」（`wayland_surface.cpp`）。
//
// 为何本头**不引 `<wayland-client.h>`**（与 `x11_drag.h` 不引 Xlib 同款纪律）：wayland 头
// 虽不像 Xlib 那样无条件 `#define None`，但把平台协议头关进单个 TU 仍是更小的污染面与更小的
// 依赖闭包——公共头零平台依赖的既有约束由此延伸到内部头。故协议对象一律以 `void *` +
// 版本号传入，在 `.cpp` 内还原类型。
//
// 执行模型（事件驱动，与 X11 桥同款）：Wayland 的拖放会话**同样不阻塞事件泵**——全部通知
// 经 `wl_display_dispatch_pending` 在帧内到达，故拖出会话能在 UI 线程上跑完，不需要库自管
// 线程（Win32 `DoDragDrop` 的嵌套模态循环才是要搬走的东西）。终态一律经
// `detail::complete_drag_out_session` 登记、由 `system_drag::pump_reads()` 在帧内投递。
//
// 与 X11 的关键差异（本桥独有、不容偷懒的两点）：
//  ① **数据经管道而非 selection round-trip**：`receive` 只能给一个写端，读取必须自己做。
//     本桥用「读端非阻塞 + `tick()` 逐帧推进」——同步 `read()` 一整个大载荷即是一帧的整段
//     停顿（本库是单线程 UI + 帧循环），非阻塞 + 墙钟超时兑现则不悬挂、不谎报。
//  ② **终态信号依赖协议版本**：`dnd_drop_performed` / `dnd_finished` 是 v3 才有的；绑到
//     v1/v2 的 manager 上，源侧只收得到 `cancelled`（该事件在 v1/v2 语义里同时承担「用户取消」
//     与「会话结束」两种含义）。故终态折算按版本分叉，判据见 `wayland_drag.cpp` 的
//     `on_source_cancelled` 注释。
//
// 门控：Linux 且非 Android 且 Wayland 后端（与 `wayland_surface.cpp` 同款析取）。

#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_WAYLAND)

#include <array>
#include <functional>
#include <memory>

#include "aurora/event/event.h"

namespace aurora::detail {

/// @brief Wayland 系统拖放桥：把 `wl_data_device` / `wl_data_source` 双向翻译成
/// `SystemDropEvent` 四态与 `DragOffer` 会话。
///
/// 生命周期由**窗口宿主**持有（`WaylandSurface::Impl` 唯一持有）：`install()` 取数据设备并
/// 注册进程级拖出后端，`uninstall()` 销毁设备对象并收摊在途会话。两侧会话态互不重叠：
///   - 拖入（本窗口是 target）：`wl_data_device` 四回调驱动，桥应答 `accept` / `set_actions`，
///     落放后逐 MIME 走管道取数；
///   - 拖出（本窗口是 source）：`system_drag::begin` 经后端槽进入，桥建 `wl_data_source` 并用
///     `start_drag` 起手（serial 取宿主在 `wl_pointer.button` 按下时刷新的那一个）。
///
/// 同一窗口同时作为源与目标（自拖放）天然成立：`send` 事件在 UI 线程到达，桥直调
/// `DragOffer::provide` 应答（单线程 UI 纪律下该回调不跨线程）。
/// @note Thread: main-thread only（Wayland 事件泵与控件回调同线程）
class WaylandDragBridge final {
  public:
    /// @brief 上层通道：事件投递、缩放查询与修饰键查询，全部由宿主提供。
    struct Hooks {
        std::function<void(Event &)> emit;  ///< 投给宿主派发器（`allowed` 由派发器在调用内写定）
        std::function<float()> scale_factor;  ///< 当前 DPI 缩放（物理像素 → 逻辑 dp 的除数）
        /// @brief 当前修饰键位集（键盘路径缓存）。
        ///
        /// Wayland 的 `wl_data_device` 与 `wl_pointer` 事件**都不携带修饰态**（不像 Win32 的
        /// `grfKeyState`、也不像 X11 的 `state` 掩码），故拖放路径的修饰键只能取自键盘路径缓存。
        /// 无此钩子时按 `ModifierKey::None`（如实：不知道就是不知道）。
        std::function<ModifierKey()> modifiers;
    };

    /// @brief 构造：不触碰 Wayland（全部协议调用集中在 `install()`，便于宿主按返回值降级）。
    /// @param display `wl_display *`（以 `void *` 传入：本头不引 wayland 头，见文件头）。
    /// @param manager `wl_data_device_manager *`（`void *` 形态；可为空 ⇒ install 失败）。
    /// @param manager_version 绑 manager 时采用的协议版本（决定 v3 动作协商是否可用）。
    /// @param seat `wl_seat *`（`void *` 形态）。
    /// @param surface 发起拖出时的 origin surface（`wl_surface *` 的 `void *` 形态）。
    /// @param hooks 上层通道。
    WaylandDragBridge(void *display, void *manager, std::uint32_t manager_version, void *seat, void *surface,
                      Hooks hooks);
    ~WaylandDragBridge();

    WaylandDragBridge(const WaylandDragBridge &) = delete;
    auto operator=(const WaylandDragBridge &) -> WaylandDragBridge & = delete;
    WaylandDragBridge(WaylandDragBridge &&) = delete;
    auto operator=(WaylandDragBridge &&) -> WaylandDragBridge & = delete;

    /// @brief 取数据设备、挂监听器并注册进程级拖出后端。
    ///
    /// 失败来源只有三类：manager / seat 缺失、`get_data_device` 返回空、连接已断开。此时如实
    /// 返回 false，由宿主记日志并按「该窗口不参与系统拖放」处理——**不谎报已有通路**。
    /// @return 是否成功；重复调用返回 false（不重复注册）。
    [[nodiscard]] auto install() -> bool;

    /// @brief 销毁数据设备与在途数据源、收摊在途会话（在途拖出按 `Cancelled` 兑现）并清全部会话态。
    auto uninstall() -> void;

    /// @brief 刷新「最近一次指针按键按下」的 serial（`start_drag` 的协议必需实参）。
    ///
    /// 宿主在 `wl_pointer.button` 的按下分支调用：拖放由控件在**该次按键的派发期内**发起
    /// （`Widget::begin_system_drag`），故此刻记录的 serial 正是发起窗口自己的 implicit grab
    /// serial——Wayland 协议要求 `start_drag` 用它，用别的 serial 即 protocol error。
    /// @param serial `wl_pointer.button` 的 serial。
    auto note_press_serial(std::uint32_t serial) -> void;

    /// @brief 帧循环看守：管道读取的逐帧推进与「落放后迟迟不给数据」的墙钟超时在此兑现。
    auto tick() -> void;

    /// @brief 本桥是否正在发起拖出（宿主据此屏蔽拖出期内的普通指针派发）。
    [[nodiscard]] auto drag_out_active() const -> bool;

    /// @brief 本桥是否处于拖入会话中（宿主据此屏蔽拖入期内的重复悬停派发）。
    [[nodiscard]] auto drag_in_active() const -> bool;

    /// @brief 登记「当前发起窗口」槽（进程级；由宿主在事件泵入口刷新，供拖出后端解析发起桥）。
    /// @param bridge 发起桥（可为空：窗口销毁时清槽）。
    static auto note_initiator(WaylandDragBridge *bridge) -> void;

    /// @brief 协议状态机（display / data_device / 两侧会话态），定义在 `wayland_drag.cpp`。
    ///
    /// public 而非 private：Wayland 的 listener 是**自由函数指针表**（`wl_data_device_listener` /
    /// `wl_data_source_listener`），C 回调只能经 `void *user data` 还原对象，故同 TU 内的 thunk
    /// 必须在类外以 `Impl *` 命名本类型（`WaylandSurface::Impl` 同款先例）。
    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;  ///< 状态机（唯一所有权）
    void *display_ = nullptr;  ///< `wl_display *`（void 形态：本头不引 wayland 头）
    void *manager_ = nullptr;  ///< `wl_data_device_manager *`（void 形态）
    std::uint32_t manager_version_ = 0;  ///< 绑 manager 时采用的版本（≥3 才有 v3 动作协商）
    void *seat_ = nullptr;  ///< `wl_seat *`（void 形态）
    void *surface_ = nullptr;  ///< 发起拖出的 origin surface（`wl_surface *` 的 void 形态）
    Hooks hooks_;  ///< 上层通道

    /// @brief 发起一次拖出会话（进程级后端槽的唯一落点；由 `system_drag::begin` 经后端进入）。
    /// @param offer 控件要约（事件层已校验 `formats` 与 `provide` 非空）。
    /// @return 会话是否成功发起（manager 缺失 / serial 无效 / 无格式可广告时为 false）。
    auto begin_drag_out(const DragOffer &offer) -> bool;
    /// @brief 取消在途拖出会话并兑现 `Cancelled`（`system_drag::cancel` 唯一入口）。
    auto cancel_drag_out() -> void;
    /// @brief 取发起槽内的桥（多窗口下由宿主在事件泵入口刷新）。
    [[nodiscard]] static auto initiator() -> WaylandDragBridge *;
    /// @brief 注册进程级拖出后端（先到者优先；由 `install()` 调用一次）。
    static auto register_backend() -> void;
};

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_LINUX && !ANDROID && AURORA_BACKEND_WAYLAND
