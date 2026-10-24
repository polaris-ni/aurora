#pragma once

// macOS 系统拖放桥：**内部头**（与 `x11_drag.h` / `wayland_drag.h` 同列于 src/）。
//
// 为何单列（与 X11 / Wayland 同判据）：AppKit 的拖放是**协议对象 + 委托方法**的完整双向
// 状态机——拖入侧要实现 `NSDraggingDestination` 的五个方法（`draggingEntered` /
// `draggingUpdated` / `draggingExited` / `prepareForDragOperation` / `performDragOperation`），
// 拖出侧要实现 `NSDraggingSource` 的两个委托方法与 `beginDraggingSessionWithItems:event:source:`
// 的调用时序，外加 `NSPasteboard` 的编解码。把这套时序写进 `MacOSSurface` 的事件泵，会把
// 「会话时序」混进「窗口事件映射」，两边都不可读、也不可单测。故会话态与时序全在本桥内，
// 表层只做「转发条目」（`macos_surface.cpp`）。
//
// 与另两平台的关键差异（本桥独有、不容偷懒的三点）：
//  ① **accept 闸门是 `prepareForDragOperation` 的返回值**：AppKit 只在它返回 YES 后才调
//     `performDragOperation`。把闸门放在后者，「拒绝」路径无处实现——目标侧从未收到过数据，
//     却也无法告诉 OS「不接受」。故本桥在 Enter / Over 派发后即持守 `allowed`，闸门读它。
//  ② **拖入取数是同步的**：`performDragOperation` 里直接读 `draggingPasteboard`，AppKit 的
//     读取 API 自身阻塞到数据就绪（与 X11 selection round-trip、Wayland 管道的「非阻塞 +
//     帧内推进」都不同）。故本桥**没有** tick 级的读取泵——`tick()` 是空转（保留只为表层
//     帧循环调用点与另两桥同形），载荷读取全部发生在 `performDragOperation` 内。
//  ③ **终态来自 `draggingSession(_:endedAt:operation:)`**：operation 为 none ⇒ 取消；否则
//     按 `NSDragOperation` 折算（Move 时先走 `on_move_confirmed` 确认，失败改写为
//     `Failed` + `Move`，与 Win32 侧同纪律）。macOS 没有「目标回传执行动作」的协议事件，
//     源侧只知道「谈成了哪个 operation」——与 Wayland 的 `dnd_finished` 同形态。
//
// 为何本头**不引 `<AppKit/AppKit.h>`**（与 `x11_drag.h` 不引 Xlib 同款纪律）：把平台头关进
// 单个 TU 是更小的污染面与更小的依赖闭包——公共头零平台依赖的既有约束由此延伸到内部头。
// 故视图 / 事件 / 会话一律以 `void *` 传入，在 `.cpp` 内还原类型。
//
// 执行模型（事件驱动，与 X11 / Wayland 桥同款）：AppKit 的拖放会话**不阻塞事件泵**——全部
// 委托方法都在 UI 线程的 runloop 上到达（`beginDraggingSession` 自身不进嵌套模态循环，
// 会话期间普通事件继续派发），故拖出会话能在 UI 线程上跑完，不需要库自管线程（Win32
// `DoDragDrop` 的嵌套模态循环才是要搬走的东西）。终态一律经
// `detail::complete_drag_out_session` 登记、由 `system_drag::pump_reads()` 在帧内投递。
//
// 修饰态：AppKit 的 `NSDraggingInfo` 不携带修饰键位集（与 Wayland 同），且拖动期间鼠标事件
// 被会话吞掉，故拖放路径的修饰键只能由宿主从其键盘路径缓存经 hooks 提供；无钩子时按
// `ModifierKey::None`（如实：不知道就是不知道）。`NSDraggingSource` 的
// `ignoreModifierKeysForDraggingSession:` 返回 YES，AppKit 不自行按修饰键改写 operation——
// 修饰键 → 动作的翻译全库只有 `system_drag::proposed_action_for` 一处（跨平台一致口径）。
//
// 内存纪律：本实现按 **MRC**（无 `-fobjc-arc`，与 `macos_surface.cpp` 同口径）书写，ObjC
// 对象一律显式 retain / release；会话对象（拖拽会话 / 粘贴板条目）随会话创建与销毁成对。
//
// 门控：macOS 平台且 macOS 后端（与 `macos_surface.cpp` 同款析取）。

#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

#include <cstdint>
#include <functional>
#include <memory>

#include "aurora/event/event.h"

namespace aurora::detail {

/// @brief macOS 系统拖放桥：把 AppKit 拖放会话（`NSDraggingDestination` / `NSDraggingSource`）
/// 翻译成 `SystemDropEvent` 四态与 `DragOffer` 会话。
///
/// 生命周期由**窗口宿主**持有（`MacOSSurface::Impl` 唯一持有）：`install()` 在内容视图上
/// 登记拖放类型并注册进程级拖出后端，`uninstall()` 收摊在途会话。两侧会话态互不重叠：
///   - 拖入（本窗口是 target）：视图的 `NSDraggingDestination` 方法转发到
///     `on_dragging_entered` 一族 C++ 落点，`prepare_for_drag_operation` 是 accept 闸门；
///   - 拖出（本窗口是 source）：`system_drag::begin` 经后端槽进入，桥用宿主在指针按下时
///     经 `note_press_event` 暂存的 `NSEvent` 调 `beginDraggingSessionWithItems:event:source:`。
///
/// 同一窗口同时作为源与目标（自拖放）天然成立：全部委托方法都在 UI 线程到达，桥直调
/// `DragOffer::provide` 应答（单线程 UI 纪律下该回调不跨线程）。
/// @note Thread: main-thread only（AppKit 会话回调与控件回调同线程）
class MacOSDragBridge final {
  public:
    /// @brief 上层通道：事件投递、缩放查询与修饰键查询，全部由宿主提供。
    struct Hooks {
        std::function<void(Event &)> emit;  ///< 投给宿主派发器（`allowed` 由派发器在调用内写定）
        std::function<float()> scale_factor;  ///< 当前 DPI 缩放（物理像素 → 逻辑 pt 的除数）
        /// @brief 当前修饰键位集（键盘路径缓存）。
        ///
        /// AppKit 的 `NSDraggingInfo` 不携带修饰态（拖动会话吞掉鼠标事件），故拖放路径的
        /// 修饰键只能取自宿主键盘路径缓存（与键盘 / 鼠标事件同一份缓存、同一折算入口）。
        /// 无此钩子时按 `ModifierKey::None`（如实：不知道就是不知道）。
        std::function<ModifierKey()> modifiers;
    };

    /// @brief 构造：不触碰 AppKit（全部平台调用集中在 `install()`，便于宿主按返回值降级）。
    /// @param view 内容视图（`NSView *` 的 `void *` 形态；拖入登记与拖出发起都经它）。
    /// @param hooks 上层通道。
    MacOSDragBridge(void *view, Hooks hooks);
    ~MacOSDragBridge();

    MacOSDragBridge(const MacOSDragBridge &) = delete;
    auto operator=(const MacOSDragBridge &) -> MacOSDragBridge & = delete;
    MacOSDragBridge(MacOSDragBridge &&) = delete;
    auto operator=(MacOSDragBridge &&) -> MacOSDragBridge & = delete;

    /// @brief 装填内容视图（供「视图由桥自己产出」的宿主，构造期拿不到视图时用）。
    ///
    /// `make_drag_destination_view` 产出的视图需要桥先存在（视图转发要落进 `Impl`），故这类宿主
    /// 只能先建桥（视图位为空）、再建视图、再回装。登记与拖出发起都以回装后的视图为落点。
    /// @param view 内容视图（`NSView *` 的 `void *` 形态；为空则 `install()` 如实失败）。
    auto attach_view(void *view) -> void;

    /// @brief 在内容视图上登记拖放类型并注册进程级拖出后端。
    ///
    /// 失败来源只有两类：视图缺失、登记调用无效（视图已释放等）。此时如实返回 false，由宿主
    /// 记日志并按「该窗口不参与系统拖放」处理——**不谎报已有通路**。
    /// @return 是否成功；重复调用返回 false（不重复注册）。
    [[nodiscard]] auto install() -> bool;

    /// @brief 注销拖放类型登记、收摊在途会话（在途拖出按 `Cancelled` 兑现）并清全部会话态。
    auto uninstall() -> void;

    // ---- 拖入侧落点（由内容视图的 `NSDraggingDestination` 方法转发进来）----

    /// @brief `draggingEntered:` 落点：建会话、派发 `Enter`，返回目标侧受理的 operation 位。
    /// @param sender `id<NSDraggingInfo>`（`void *` 形态）。
    /// @return `NSDragOperation` 位（`None` = 不接受；由视图方法原样返回给 AppKit）。
    [[nodiscard]] auto on_dragging_entered(void *sender) -> std::uint32_t;

    /// @brief `draggingUpdated:` 落点：更新位置与源能力、派发 `Over`，返回受理的 operation 位。
    /// @param sender `id<NSDraggingInfo>`（`void *` 形态）。
    /// @return `NSDragOperation` 位（`None` = 不接受）。
    [[nodiscard]] auto on_dragging_updated(void *sender) -> std::uint32_t;

    /// @brief `draggingExited:` 落点：未落放即离开，补派 `Leave` 并收清会话态。
    auto on_dragging_exited() -> void;

    /// @brief `prepareForDragOperation:` 落点：**accept 闸门**（AppKit 只在 YES 后调 perform）。
    /// @param sender `id<NSDraggingInfo>`（`void *` 形态；不用其内容，闸门读会话态）。
    /// @return 是否接受本次落放（Enter / Over 派发后写定的 `allowed` 非 None）。
    [[nodiscard]] auto on_prepare_for_drag_operation(void *sender) -> bool;

    /// @brief `performDragOperation:` 落点：同步读 `draggingPasteboard`、派发 `Drop`。
    ///
    /// 读取在**本调用内**同步完成（AppKit 读取 API 自身阻塞到数据就绪），大小载荷同一条路径
    /// ——macOS 没有 X11 `INCR` 分块或 Wayland 管道那样的分叉，`AURORA_DRAG_SYNC_PAYLOAD_LIMIT`
    /// 在 macOS 侧不产生第二种传输形态。
    /// @param sender `id<NSDraggingInfo>`（`void *` 形态）。
    auto on_perform_drag_operation(void *sender) -> void;

    // ---- 拖出侧 ----

    /// @brief 暂存「最近一次指针按键按下」的事件（`beginDraggingSession` 的协议必需实参）。
    ///
    /// 宿主在 `mouseDown:` 派发期内调用：拖放由控件在**该次按键的派发期内**发起
    /// （`Widget::begin_system_drag`），故此刻暂存的事件正是发起窗口自己的按下事件。
    /// 一次性：发起消费后即清空。
    /// @param event `NSEvent *`（`void *` 形态；桥在暂存期持一次引用）。
    auto note_press_event(void *event) -> void;

    /// @brief 帧循环看守：macOS 侧**无在途读取可泵**（取数同步发生在 perform 内），故为空转。
    ///
    /// 保留只为表层帧循环调用点与另两桥同形（`x11_surface.cpp` / `wayland_surface.cpp` 的
    /// `tick()` 调用点不按平台分叉）。
    auto tick() -> void;

    /// @brief 本桥是否正在发起拖出（宿主据此屏蔽拖出期内的普通指针派发）。
    [[nodiscard]] auto drag_out_active() const -> bool;

    /// @brief 本桥是否处于拖入会话中（宿主据此屏蔽拖入期内的重复悬停派发）。
    [[nodiscard]] auto drag_in_active() const -> bool;

    /// @brief 登记「当前发起窗口」槽（进程级；由宿主在事件泵入口刷新，供拖出后端解析发起桥）。
    /// @param bridge 发起桥（可为空：窗口销毁时清槽）。
    static auto note_initiator(MacOSDragBridge *bridge) -> void;

    /// @brief 建一枚「系统拖放内容视图」（`NSView` 子类；`void *` 形态）。
    ///
    /// 该类实现 `NSDraggingDestination` 五方法（全部转发到桥的 C++ 落点）、
    /// `isFlipped = YES`（视图坐标 y 向下，与 X11 / Win32 客户区同向）与
    /// `acceptsFirstResponder`。宿主的真实实现把它作为内容视图即可获得完整拖入行为；
    /// 探针亦复用它（真机验收与开发同一条代码路径）。
    /// @param bridge 转发目标（桥的生命周期必须长于视图）。
    /// @return `NSView *`（`void *` 形态；调用方持所有权，MRC 下须自行 release）。
    [[nodiscard]] static auto make_drag_destination_view(void *bridge) -> void *;

    /// @brief 会话状态机（视图 / 事件 / 粘贴板 / 两侧会话态），定义在 `macos_drag.cpp`。
    ///
    /// public 而非 private：ObjC 委托对象（`NSDraggingSource` 实现与目标视图类）要经
    /// `void *user data` 还原 `Impl *` 再转发，同 TU 内的 ObjC 方法必须在类外以
    /// `Impl *` 命名本类型（`WaylandSurface::Impl` 同款先例）。
    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;  ///< 状态机（唯一所有权）
    void *view_ = nullptr;  ///< 内容视图（`NSView *` 的 void 形态）
    Hooks hooks_;  ///< 上层通道

    /// @brief 发起一次拖出会话（进程级后端槽的唯一落点；由 `system_drag::begin` 经后端进入）。
    /// @param offer 控件要约（事件层已校验 `formats` 与 `provide` 非空）。
    /// @return 会话是否成功发起（视图缺失 / 无按下事件 / 无格式可广告时为 false）。
    auto begin_drag_out(const DragOffer &offer) -> bool;
    /// @brief 取消在途拖出会话并兑现 `Cancelled`（`system_drag::cancel` 唯一入口）。
    auto cancel_drag_out() -> void;
    /// @brief 取发起槽内的桥（多窗口下由宿主在事件泵入口刷新）。
    [[nodiscard]] static auto initiator() -> MacOSDragBridge *;
    /// @brief 注册进程级拖出后端（先到者优先；由 `install()` 调用一次）。
    static auto register_backend() -> void;
};

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS
