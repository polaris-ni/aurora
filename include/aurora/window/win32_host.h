#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "aurora/core/a11y_provider.h"
#include "aurora/core/types.h"
#include "aurora/event/event.h"
#include "aurora/window/surface.h"
#include "aurora/window/window_state.h"

// 共享 Win32 窗口宿主：仅依赖 Windows SDK（user32/gdi32），零三方依赖。
// 把「窗口创建 / 消息泵 / 事件翻译 / DPI / WM_PAINT·WM_SIZE 同步重渲染 / 白闪修复刷」
// 从 Win32Surface 抽取出来，供 Win32Surface(GDI) 与 D3D11Surface(GPU) 共用，
// 避免复制消息泵带来的行为分歧。
//
// pimpl 封装：公共头不再包含 <windows.h> / <windowsx.h>，所有 Win32/GDI 细节（HWND/HINSTANCE/
// 消息分发 / 键码映射 / UTF-8 转换等）移入 src/aurora/window/win32_host.cpp 的 Impl，
// 仅暴露 `std::unique_ptr<Impl> pimpl_`；跨平台消费者（如 D3D11Surface、Headless 测试）
// 无需拉入重型平台头。原生句柄以 `void*` 暴露（避免公共头引入 <windows.h>），
// 调用方如需真实 `HWND` 显式 `static_cast` 即可。整文件被 #ifdef AURORA_BACKEND_WIN32 包裹。
#ifdef AURORA_BACKEND_WIN32

namespace aurora {

/// @brief 共享 Win32 窗口宿主：窗口创建 + 消息泵 + 事件翻译 + DPI + 同步重渲染。
///
/// 不含「像素如何上屏」：present 由 Win32Surface(GDI SetDIBitsToDevice) /
/// D3D11Surface(纹理上传) 各自实现，宿主仅在 WM_SIZE/WM_PAINT 时调用
/// `present_request_` 触发 Window 的同步重渲染（消除最大化白闪）。
class Win32Host {
  public:
    using EventHandler =
        std::function<void(Event &)>;  ///< 事件处理器：接收翻译后的 Event 引用，由 Application 统一派发
    using WindowStateHandler = std::function<void(WindowState)>;  ///< 可见性状态上报句柄（最小化/遮挡/前台）
    using WindowModeHandler =
        std::function<void(WindowMode)>;  ///< 几何态上报句柄（Normal/Maximized/Minimized/FullScreen）
    using PresentRequest = std::function<void()>;  ///< 同步重渲染请求：WM_SIZE/WM_PAINT 时触发 Window 立即重呈现

    /// @brief 创建原生窗口（带高级样式与可见性策略）。
    /// @param w 窗口宽度（逻辑像素，内部按 DPI 换算物理尺寸）。
    /// @param h 窗口高度（逻辑像素）。
    /// @param title 窗口标题（UTF-8，经 ACP 转换喂给 Win32）。
    /// @param style 高级样式选项（标题栏/边框/透明等，见 WindowStyleOptions）。
    /// @param visibility 初始可见性策略；Normal 之外经首显时机控制避免闪烁。
    Win32Host(int w, int h, const std::string &title, const WindowStyleOptions &style,
              WindowVisibility visibility = WindowVisibility::Normal);
    /// @brief 销毁原生窗口、解除消息泵回调并释放宿主持有的 GDI/UIA 资源。
    ~Win32Host();

    /// @brief 禁止拷贝：HWND 消息泵与 pimpl 所有权唯一。
    Win32Host(const Win32Host &) = delete;
    /// @brief 禁止拷贝赋值：窗口所有权唯一，任何使用均为编译期错误。
    /// @return 删除声明无运行期返回值。
    auto operator=(const Win32Host &) -> Win32Host & = delete;
    /// @brief 禁止移动：窗口过程 user-data 绑定本对象，移动会悬垂。
    Win32Host(Win32Host &&) = delete;
    /// @brief 禁止移动赋值：窗口过程 user-data 绑定本对象，任何使用均为编译期错误。
    /// @return 删除声明无运行期返回值。
    auto operator=(Win32Host &&) -> Win32Host & = delete;

    /// @brief 注册事件处理器：WndProc 翻译后的 aurora Event 经此上抛。
    /// @param h 事件接收器；为空时仅做窗口状态维护、不上抛事件。
    auto set_event_handler(EventHandler h) const -> void;
    /// @brief 注册可见性状态上报句柄。
    /// @param h 状态回调；由 ShowWindow/焦点/遮挡探测触发。
    auto set_window_state_handler(WindowStateHandler h) const -> void;
    /// @brief 注册窗口几何态上报句柄。
    /// @param h 几何态回调；由 SIZE/MOVE/最小化等消息翻译产生。
    auto set_window_mode_handler(WindowModeHandler h) const -> void;
    /// @brief 注册同步重渲染请求句柄：WM_SIZE/WM_PAINT 内调用以消除白闪窗口期。
    /// @param h 请求回调；为空时仅 invalidate，不主动重呈现。
    auto set_present_request(PresentRequest h) const -> void;

    /// @brief 运行时更新窗口标题（SetWindowText + UTF-8→ACP，支持非 ASCII 字符）。
    /// @param title 新标题（UTF-8）。
    auto set_title(const std::string &title) const -> void;

    /// @brief 原生窗口句柄（`HWND`），以 `void*` 暴露以避免公共头引入 <windows.h>。
    [[nodiscard]] auto hwnd() const -> void *;
    /// @brief 窗口客户区逻辑尺寸（物理尺寸 ÷ 当前缩放）。
    /// @return 客户区 Size。
    [[nodiscard]] auto size() const -> Size;
    /// @brief 当前 DPI 缩放因子（GetDpiForWindow / 96）。
    /// @return 缩放倍数；查询失败时为 1.0。
    [[nodiscard]] auto scale_factor() const -> float;
    /// @brief 用户是否请求关闭窗口（WM_CLOSE 后置位）。
    /// @return 收到关闭请求时 true。
    [[nodiscard]] auto should_close() const -> bool;
    /// @brief 已呈现次数（测试/自检用）：验证 WM_SIZE/WM_PAINT 触发了同步重渲染。
    /// @return present_request 生效以来的呈现计数。
    [[nodiscard]] auto present_count() const -> int;
    /// @brief 类背景擦除刷（测试/自检用）：非空（HBRUSH 句柄值，以 `void*` 暴露）表示已消除最大化黑屏；未设置时
    /// nullptr。
    [[nodiscard]] static auto background_brush() -> void *;

    /// @brief 轮询平台原生事件（PeekMessage 抽出消息泵，翻译后上抛）。
    auto poll_platform_events() const -> void;

    /// @brief 阻塞等待消息或超时：`MsgWaitForMultipleObjectsEx`
    /// 带 `QS_ALLINPUT|MWMO_INPUTAVAILABLE`，队列已有消息时立即返回。
    /// `timeout_ms < 0` 为无限等待，按 1000ms 分段兜底（防丢唤醒死等）。
    /// @param timeout_ms 等待上限毫秒；0 立即返回，负值按无限（分段 1s）。
    auto wait_events(double timeout_ms) const -> void;

    /// @brief 跨线程唤醒：`PostMessage(WM_NULL)` 使阻塞在 `wait_events` 的主循环立即返回。
    /// PostMessage 本身线程安全，可由后台线程（async 回投）直接调用。
    auto request_wake() const -> void;

    // ---- 父子窗口与模态（多窗口）----

    /// @brief 设置 owner 窗口（HWND，以 `void*` 传入；nullptr = 解除从属）。
    /// 走 `GWLP_HWNDPARENT`：子窗恒浮于 owner 之上、随 owner 最小化，且不产生独立任务栏条目。
    /// @param owner_hwnd owner 窗口原生句柄（HWND 的 void* 形态）；nullptr 解除从属关系。
    auto set_owner(void *owner_hwnd) const -> void;
    /// @brief 启用/禁用窗口输入（`EnableWindow`）——模态窗口屏蔽其 owner 时使用。
    /// @param on true 启用输入，false 禁用（置灰）。
    auto set_enabled(bool on) const -> void;

    // ---- z 序、显示器与 DPI（多窗口）----

    /// @brief 提升 z 序到同组顶部（不激活）：`BringWindowToTop`。
    auto raise() const -> void;
    /// @brief 激活窗口（置顶 + 键盘焦点）：`SetForegroundWindow` + `SetFocus`。
    auto focus_window() const -> void;
    /// @brief 当前所在显示器的稳定 id（`MonitorFromWindow` 的 HMONITOR 句柄值，
    /// 与 `app::Display::id` 同源）；未知返回 -1。
    /// @return 显示器稳定 id；窗口句柄或显示器不可用时 -1。
    [[nodiscard]] auto display_id() const -> int;
    /// @brief 窗口在屏幕上的位置（**物理像素**，`GetWindowRect` 左上角）。
    /// @return 窗口左上角屏幕坐标；句柄失效时为 (0,0)。
    [[nodiscard]] auto position() const -> Point;
    /// @brief 程序化移动窗口（`SetWindowPos`，物理像素）。
    /// @param p 目标左上角位置（物理像素）。
    auto set_position(Point p) const -> void;
    /// @brief 程序化设置窗口**外框**尺寸（`SetWindowPos`，物理像素）。
    /// @param s 目标外框尺寸（物理像素）。
    auto set_size(Size s) const -> void;
    /// @brief 注册 DPI 缩放变化回调：`WM_DPICHANGED` 处理后上报新的 `scale_factor`。
    /// @param h 缩放变化处理器，参数为新的缩放因子。
    auto set_scale_change_handler(std::function<void(float)> h) const -> void;

    // ---- 无障碍桥 ----

    /// @brief 本窗口的无障碍桥（`a11y::Provider`）；未激活（尚无读屏查询）返回 nullptr。
    ///
    /// 桥实例**由窗口宿主持有**（而非任一 Surface）：`Win32Surface`(GDI) 与 `D3D11Surface`(GPU)
    /// 共用本宿主，二者 `Surface::accessibility_provider()` 都返回同一实例，避免两份
    /// id→Widget* 映射分裂。惰性：首个 `WM_GETOBJECT(UiaRootObjectId)` 到达时才构造并激活。
    /// @return 无障碍桥裸指针，所有权归宿主；未激活时 nullptr。
    [[nodiscard]] auto accessibility_provider() const -> a11y::Provider *;

    /// @brief 注入语义树根（每帧调用；桥尚未构造时由宿主记下，构造后补喂）。
    /// @param root 当前窗口语义树根 Widget；nullptr 表示清除。
    auto set_accessibility_root(Widget *root) const -> void;

    /// @brief 覆盖 `WM_GETOBJECT` 处理（宿主可抢先接管；默认空 → 走内置 UIA 桥）。
    ///
    /// 以指针宽度整数传递 `WPARAM`/`LPARAM`、返回 `LRESULT`，避免在公共头引入 `<windows.h>`；
    /// 宿主侧（`.cpp`）在调用前 `static_cast` 回原生类型。
    /// @param h WM_GETOBJECT 覆盖钩子：入参为指针宽度整数的 `WPARAM`/`LPARAM`，返回 optional `LRESULT`；
    /// 返回 `std::nullopt` 表示「未处理，交内置桥 / DefWindowProc」。
    /// @return 本设置器无返回值（hook 注册即生效）。
    auto set_accessibility_hook(std::function<std::optional<std::intptr_t>(std::uintptr_t, std::intptr_t)> h) const
        -> void;

    // ---- 输入法桥（IMM32 组合输入）----

    /// @brief 注入 IME 候选窗定位查询（返回**窗口逻辑 dp** 零宽竖盒；默认空 → 系统默认位置）。
    ///
    /// 与 `Surface::set_composition_caret_provider` 同一契约：`Win32Surface`(GDI) 与
    /// `D3D11Surface`(GPU) 共用本宿主，宿主在 `WM_IME_STARTCOMPOSITION`/`WM_IME_COMPOSITION`
    /// 时调用它取当前焦点控件的插入点，再 `× scale` + `ClientToScreen` 喂 `ImmSetCandidateWindow`。
    /// @param provider 候选窗定位查询（返回逻辑坐标零宽竖盒）；默认空 → 系统默认位置。
    auto set_composition_caret_provider(std::function<Rect()> provider) const -> void;

    /// @brief 是否处于 IME 组合中（真机验收探针 / 自检观测器用）。
    /// @return 组合输入进行中时 true。
    [[nodiscard]] auto ime_composing() const -> bool;

  private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;  ///< Win32/GDI 窗口状态与消息分发的 pimpl 实现体（公共头不含 <windows.h>）
};

}  // namespace aurora

#endif  // AURORA_BACKEND_WIN32
