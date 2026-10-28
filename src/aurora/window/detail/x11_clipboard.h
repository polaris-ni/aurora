#pragma once

// X11 CLIPBOARD 剪贴板桥：**内部头**（与 `x11_drag.h` / `atspi_bridge.h` 同列于 src/）。
//
// 为何单列：X11 剪贴板是**基于 Selection 的异步协议**——写入只是「声明本进程持有 CLIPBOARD 选择
// 权」，真正的数据在别的进程经 `XConvertSelection` 请求时才经 `SelectionRequest` 送达；读入则是
// 本进程向当前 owner 发 `XConvertSelection`、等 `SelectionNotify` 回属性。把这套协议态写进
// `X11Surface` 的事件泵，会把「Selection 时序」混进「窗口事件映射」，两边都不可读、也不可单测。
// 故协议态全在本桥内，表层只做「转发条目」（`x11_surface.cpp`）。
//
// 为何本头**不引 Xlib 头**（与 `x11_drag.h` 同款纪律）：Xlib 的 `<X11/X.h>` 无条件 `#define None 0`
// 与 `#define CursorShape 0`，一旦进入头文件就会污染所有下游 TU 的 `ModifierKey::None` 与
// `aurora::CursorShape`。把它关在 `x11_clipboard.cpp` 单个 TU 内，污染面即被限制到该 TU。本头因此
// 只用 `void *` 表达 `Display *`、用 `unsigned long` 表达 XID / Atom（X 协议下两者同宽）。
//
// 门控：Linux 且非 Android 且 X11 后端（与 `x11_surface.cpp` 同款析取）。

#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_X11)

#include <functional>
#include <memory>

namespace aurora::detail {

/// @brief X11 CLIPBOARD 剪贴板桥：把 X11 Selection 协议翻译成 `clipboard::detail::PlatformBackend`
/// 的九枚入口。
///
/// 生命周期由**窗口宿主**持有（`X11Surface::Impl` 唯一持有）：`install()` 缓存 atom、按 XFIXES
/// 能力登记变更监听，并注册进程级剪贴板后端；`uninstall()` 撤后端并放弃所有权。所有权本身在
/// `set_payload` / `set_offer` 时才经 `XSetSelectionOwner` 声明——`install()` 不抢占别人的剪贴板。
/// @note Thread: main-thread only（X 事件泵与控件回调同线程）
class X11ClipboardBridge final {
  public:
    /// @brief 上层通道：变更通知（XFIXES selection owner notify 触达时回调）。
    struct Hooks {
        std::function<void()> on_change;  ///< 系统剪贴板内容变化回调（空则无机制）。
    };

    /// @brief 构造：不触碰 X（全部 X 调用集中在 `install()`，便于宿主按返回值降级）。
    /// @param display `Display *`（以 `void *` 传入：本头不引 Xlib，见文件头）。
    /// @param window 客户窗口 XID（CLIPBOARD 选择权的 owner 窗口）。
    /// @param hooks 上层通道。
    X11ClipboardBridge(void *display, unsigned long window, Hooks hooks);
    ~X11ClipboardBridge();

    X11ClipboardBridge(const X11ClipboardBridge &) = delete;
    auto operator=(const X11ClipboardBridge &) -> X11ClipboardBridge & = delete;
    X11ClipboardBridge(X11ClipboardBridge &&) = delete;
    auto operator=(X11ClipboardBridge &&) -> X11ClipboardBridge & = delete;

    /// @brief 缓存 atom、按 XFIXES 能力登记变更监听、注册进程级剪贴板后端。
    ///
    /// 失败只有一种常见来源：display 已断开 / 窗口已销毁。此时如实返回 false，由宿主按
    /// 「该窗口不参与 X11 剪贴板」处理——**不谎报已有通路**。
    /// @return 是否成功；重复调用返回 false（不重复注册）。
    [[nodiscard]] auto install() -> bool;

    /// @brief 撤后端、放弃 CLIPBOARD 所有权、注销 XFIXES 监听并清全部会话态。
    auto uninstall() -> void;

    /// @brief 投一条 `SelectionRequest`（本进程是 owner，外来进程索取数据）。
    /// @param requestor 请求方窗口 XID。
    /// @param selection selection atom（非 `CLIPBOARD` 一律忽略）。
    /// @param target 被请求的目标 atom。
    /// @param property 请求方指定的结果属性名（拒绝时回 0）。
    /// @param time 请求时间戳（原样回给请求方）。
    auto on_selection_request(unsigned long requestor, unsigned long selection, unsigned long target,
                              unsigned long property, unsigned long time) -> void;

    /// @brief 投一条 `SelectionClear`（本进程的所有权被别的进程夺走）。
    /// @param selection selection atom（非 `CLIPBOARD` 一律忽略）。
    /// @param time 事件时间戳。
    auto on_selection_clear(unsigned long selection, unsigned long time) -> void;

    /// @brief 投一条 `SelectionNotify`（读入侧 `XConvertSelection` 的到达面）。
    /// @param selection selection atom（非 `CLIPBOARD` 一律忽略）。
    /// @param target 被请求的目标 atom。
    /// @param property 结果属性名；**0（atom `None`）表示该目标无数据**。
    auto on_selection_notify(unsigned long selection, unsigned long target, unsigned long property) -> void;

    /// @brief 投一条 `PropertyNotify`（INCR 分片的推进面：读入收块、写出发块）。
    /// @param window 属性所在窗口（读入侧是本窗口；写出侧是外来请求方窗口）。
    /// @param atom 属性名。
    /// @param state `PropertyNewValue`（0）/ `PropertyDelete`（1）。
    auto on_property_notify(unsigned long window, unsigned long atom, int state) -> void;

    /// @brief 统一事件入口：XFIXES 的 selection owner 通知。
    ///
    /// 由宿主对**每条** X 事件调用（先于自己的 switch）；非本桥关心的类型返回 false，宿主照常
    /// 处理。把 XFIXES 的事件基址与结构体关在本 TU 内，表层只做一次「是否被消费」的判断。
    /// @param xevent `XEvent *`（void 形态：本头不引 Xlib）。
    /// @return 是否已被本桥消费。
    [[nodiscard]] auto on_x_event(const void *xevent) -> bool;

    /// @brief 本桥是否持有 CLIPBOARD 所有权（宿主据此判断「清空」是否要放弃所有权）。
    [[nodiscard]] auto owns() const -> bool;

  private:
    struct Impl;  ///< 协议状态机（定义在 `x11_clipboard.cpp`：含全部 atom 与会话态）
    std::unique_ptr<Impl> impl_;  ///< 状态机（唯一所有权）

    /// @brief 进程内当前持有 CLIPBOARD 选择权的桥（无则空）。
    ///
    /// 多窗口下读本地内容必须落在 owner 桥：同步等待期间没人泵 owner 窗口的 SelectionRequest。
    static auto owner_bridge(Impl *self) -> Impl *;
    /// @brief 把 `self` 登记为进程级后端表的当前捕获者（最近登记者接管）。
    static auto install_backend_for(Impl *self) -> void;
    /// @brief 存活清单登记：多窗口下后端表捕获最近存活桥，先关的窗口不拆仍在世窗口的通路。
    static auto register_backend(Impl *self) -> void;
    /// @brief 存活清单注销：从清单移除并改挂最近的存活桥（空则撤表）。
    ///
    /// 后端表捕获桥的自指针，桥销毁前不撤即悬垂——本函数是那唯一的撤点。
    static auto unregister_backend(Impl *self) -> void;
};

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_LINUX && !ANDROID && AURORA_BACKEND_X11
