#pragma once

// Win32 UIA 无障碍桥：**内部头**（与 `win32_cursor.h` / `win32_capture.h` 同列于 src/）。
//
// 为何不进公共头：实现要 `<windows.h>` / `<uiautomationcore.h>` 与 HWND，而
// `Win32Window` 刻意 pimpl 隔离、公共头零平台污染；本文件仅供 `win32_window.cpp` /
// `win32_surface.cpp` / `d3d11_surface.cpp` 引入。
//
// 平台宏来源必须在守卫**之前**引入：`AURORA_PLATFORM_WINDOWS` 由本头导出，若依赖调用方
// 先包含则「首 include 即本头」的 TU（如 win32_ua.cpp 自身）会把整文件判为假 → 空 TU、
// 链接期 undefined symbol。
#include "aurora/core/platform.h"

// 门控与 `win32_cursor.h` 同款：平台宏 ∧ 后端宏析取（Win32 GDI 与 D3D11 共用宿主）。
#if defined(AURORA_PLATFORM_WINDOWS) && (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11))

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

// clang-format off
#include <windows.h>
// 引**总头** <uiautomation.h>（= core + client 的正确包含顺序），而非分别引
// <uiautomationcore.h> + <uiautomationclient.h>：
//  1. 常量（UIA_*ControlTypeId / UIA_*PropertyId / UIA_*EventId …）在 MinGW SDK 下
//     只存在于 client 段，单引 core 拿不到；
//  2. 单引 client 会因 TreeTraversalOptions / ConnectionRecoveryBehaviorOptions /
//     CoalesceEventsOptions 未前置声明而编译失败（总头内部顺序才正确）。
#include <uiautomation.h>
#include <uiautomationcoreapi.h>
// clang-format on

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "aurora/core/a11y_diff.h"
#include "aurora/core/a11y_provider.h"
#include "aurora/core/a11y_text.h"

namespace aurora::detail {

class Win32UiaBridge;
class UiaNodeProvider;
class UiaTextRangeProvider;

/// @brief 动态加载的 UIA 扁平 API（D15：无链接期依赖，缺库降级 no-op）。
///
/// `UIAutomationCore.dll` 只在桥首次激活时 `LoadLibraryA`；任一**核心**函数缺失即整桥降级
/// （`Diagnostics::warn` 一次），`UiaRaiseNotificationEvent` 是**可选**项——较老系统上不存在，
/// 缺失时播报回退 `UIA_LiveRegionChangedEventId`（G20/G30）。
struct UiaApi {
    bool loaded = false;
    LRESULT(WINAPI *return_raw_element_provider)(HWND, WPARAM, LPARAM, IRawElementProviderSimple *) = nullptr;
    HRESULT(WINAPI *raise_automation_event)(IRawElementProviderSimple *, EVENTID) = nullptr;
    HRESULT(WINAPI *raise_property_changed)(IRawElementProviderSimple *, PROPERTYID, VARIANT, VARIANT) = nullptr;
    HRESULT(WINAPI *raise_structure_changed)(IRawElementProviderSimple *, enum StructureChangeType, int *, int) = nullptr;
    HRESULT(WINAPI *disconnect_provider)(IRawElementProviderSimple *) = nullptr;
    HRESULT(WINAPI *host_provider_from_hwnd)(HWND, IRawElementProviderSimple **) = nullptr;
    HRESULT(WINAPI *get_reserved_not_supported)(IUnknown **) = nullptr;
    HRESULT(WINAPI *raise_notification_event)
    (IRawElementProviderSimple *, enum NotificationKind, enum NotificationProcessing, BSTR, BSTR) = nullptr;

    /// @brief 进程级加载（幂等；失败时 `loaded == false`）。
    [[nodiscard]] static auto instance() -> const UiaApi &;
};

/// @brief Win32 UIA 桥：快照 / diff / provider 缓存 / 事件映射 / 坐标换算。
///
/// 所有权（G14）：由 `Win32Window::Impl` 持有；`Win32Surface` 与 `D3D11Surface` 的
/// `accessibility_provider()` 都返回同一实例，避免两份 id→Widget* 映射分裂。
///
/// 同步模型（D9/G2）：事件只置 dirty；平台查询（Navigate / 属性拉取）到达时
/// `sync_if_dirty()` 才重建快照 + diff + **批量**发事件（G28）。
class Win32UiaBridge final : public a11y::Provider {
  public:
    explicit Win32UiaBridge(HWND hwnd);
    ~Win32UiaBridge() override;

    Win32UiaBridge(const Win32UiaBridge &) = delete;
    auto operator=(const Win32UiaBridge &) -> Win32UiaBridge & = delete;
    Win32UiaBridge(Win32UiaBridge &&) = delete;
    auto operator=(Win32UiaBridge &&) -> Win32UiaBridge & = delete;

    // ---- a11y::Provider ----
    auto activate() -> void override;
    auto deactivate() -> void override;
    auto sync_if_dirty() -> void override;
    auto mark_dirty() -> void override;
    [[nodiscard]] auto is_active() const -> bool override;
    [[nodiscard]] auto name() const -> std::string override;
    auto set_root(Widget *root) -> void override;
    auto on_event(const AccessibilityEvent &e) -> void override;
    auto on_announcement(const std::string &text, const Widget *target) -> void override;
    auto on_widget_destroying(const Widget *w) -> void override;

    // ---- 宿主接线（Win32Window 的 WM_GETOBJECT 分支调用）----
    /// @brief 应答 `WM_GETOBJECT`：`lParam == UiaRootObjectId` 时返回根 provider 的 LRESULT。
    /// @return 已处理返回其 LRESULT；非 UIA 请求返回 `std::nullopt`（交由 DefWindowProc）。
    [[nodiscard]] auto handle_get_object(WPARAM wp, LPARAM lp) -> std::optional<LRESULT>;
    /// @brief 窗口销毁前调用：全量 `UiaDisconnectProvider` 防 UIA 侧悬垂访问（R1）。
    /// 等价于 `deactivate()`（幂等、可重入）；窗口关闭与桥析构都走此口。
    auto disconnect_all() -> void;
    /// @brief 切断根与缓存并把平台侧对象全部断连/释放（不清 `active_`，不断注册表）。
    /// 用于「根控件已销毁但窗口仍在」：桥之后可接受新根继续投影。
    auto release_platform_providers() -> void;

    /// @brief 解除「拆除门闩」（#8：`tearing_down_` 受控复位）。
    ///
    /// 仅用于**换根重建**场景：窗口 UI 树整体重建、旧根销毁广播已发生、即将经 `set_root`
    /// 注入新根之前调用。复位后桥恢复可投影状态。
    /// @warning 不得在窗口已销毁 / `disconnect_all()` 已调用的状态下调用——那会令「已销毁窗口复活」。
    ///          正常窗口生命周期无需调用本方法；`set_root` 在收到非空新根时会自动复位本门闩。
    auto reset_teardown() -> void;

    // ---- RTL（#5：应用侧经 Window 推送，桥内只做遍历/几何适配）----
    /// @brief 设置 RTL 标志（由 `Window::set_accessibility_rtl` 转发）。
    auto set_rtl(bool rtl) -> void override { rtl_ = rtl; }
    /// @brief 当前是否 RTL（影响 Navigate 遍历顺序，使读屏按从右到左阅读顺序遍历）。
    [[nodiscard]] auto is_rtl() const -> bool { return rtl_; }
    /// @brief 窗口可见盒（根节点几何，窗口本地 DIP）：裁剪离屏几何用（#3）。
    [[nodiscard]] auto visible_box() const -> Rect;

    // ---- 供 provider 查询 ----
    [[nodiscard]] auto hwnd() const -> HWND { return hwnd_; }
    [[nodiscard]] auto scale_factor() const -> float;
    [[nodiscard]] auto origin() const -> Point;
    /// @brief 窗口本地 DIP → 物理屏幕像素（D12：`物理 = bounds × scale + position`）。
    [[nodiscard]] auto to_physical(const Rect &dip) const -> UiaRect;
    /// @brief 物理屏幕像素 → 窗口本地 DIP（`ElementProviderFromPoint` 反换算）。
    [[nodiscard]] auto to_local(double phys_x, double phys_y) const -> Point;

    [[nodiscard]] auto root_widget() const -> Widget * { return root_; }
    [[nodiscard]] auto root_provider() -> IRawElementProviderSimple *;
    /// @brief 窗口宿主 provider（`UiaHostProviderFromHwnd` 结果；G19）。
    [[nodiscard]] auto host_provider() -> IRawElementProviderSimple *;
    /// @brief 当前快照（provider 的 Navigate / 属性查询来源）。
    [[nodiscard]] auto snapshot() const -> const a11y::TreeSnapshot & { return snap_; }
    /// @brief 是否存在「可滚动祖先」（`IScrollItemProvider` 暴露判据）。
    [[nodiscard]] auto has_scrollable_ancestor(std::uint64_t id) const -> bool;
    [[nodiscard]] auto find_node(std::uint64_t id) const -> const a11y::NodeSnapshot *;
    [[nodiscard]] auto id_of(const Widget *w) const -> std::uint64_t;
    /// @brief 取（或惰性创建）节点 provider；**保对象同一性**（Qt/Chromium 实践）。
    [[nodiscard]] auto provider_for(std::uint64_t id) -> IRawElementProviderSimple *;
    /// @brief 命中测试（与派发器同口径）→ 节点 id（供 `ElementProviderFromPoint` / `RangeFromPoint`）。
    [[nodiscard]] auto hit_test_id(const Point &local_dip) const -> std::uint64_t;

    /// @brief 当前聚焦节点 id（0 = 无）。
    [[nodiscard]] auto focused_id() const -> std::uint64_t;

    // ---- 事件（供 provider / 内部使用）----
    auto queue_property_changed(std::uint64_t id, PROPERTYID prop, const VARIANT &old_v, const VARIANT &new_v) -> void;
    auto queue_structure_changed(std::uint64_t parent_id, bool child_added, std::uint64_t child_id) -> void;
    auto queue_focus_changed(std::uint64_t id) -> void;
    auto queue_announcement(const std::string &text) -> void;
    /// @brief 是否有客户端在监听（G31：无监听者时整批丢弃）。
    [[nodiscard]] auto has_listeners() const -> bool { return listener_count_ > 0; }
    auto note_listener_added() -> void { ++listener_count_; }
    auto note_listener_removed() -> void {
        if (listener_count_ > 0) {
            --listener_count_;
        }
    }

  private:
    auto rebuild() -> void;
    auto emit_pending() -> void;
    auto recycle_removed(const std::vector<std::uint64_t> &removed) -> void;
    [[nodiscard]] auto provider_object(std::uint64_t id) -> UiaNodeProvider *;

    struct PendingEvent {
        enum class Kind : std::uint8_t { Structure, Focus, Property, Announcement } kind = Kind::Property;
        std::uint64_t id = 0;
        std::uint64_t child_id = 0;
        bool child_added = true;
        PROPERTYID property = 0;
        VARIANT old_value{};
        VARIANT new_value{};
        std::string text;
    };

    HWND hwnd_ = nullptr;
    Widget *root_ = nullptr;
    bool active_ = false;
    bool dirty_ = true;
    bool warned_ = false;
    /// @brief 拆除门闩（单向，仅由 `disconnect_all()` 立起；`reset_teardown()` / `set_root` 可复位）。
    ///
    /// 为何必需：`UiaDisconnectProvider` 会**同步重入**本 provider 取属性值（UIA 需为被
    /// 丢弃的侦听者补发属性变更事件）。而窗口销毁是**后于**宿主拆 UI 树的常规顺序，此刻
    /// `root_` 已悬垂 —— 若重入路径仍走拉取式重建，就会解引用已释放内存（实机 SIGSEGV，
    /// 栈：`UiaDisconnectProvider → GetPropertyValue → node() → rebuild() → build_accessibility_node`）。
    /// 立闩后重入只读旧快照、永不重建。
    bool tearing_down_ = false;
    /// @brief RTL 标志（#5）：影响 Navigate 遍历顺序；由应用侧 `Window::set_accessibility_rtl` 推送。
    bool rtl_ = false;
    int listener_count_ = 0;
    a11y::TreeSnapshot snap_;
    std::unordered_map<const Widget *, std::uint64_t> id_by_widget_;
    std::unordered_map<std::uint64_t, UiaNodeProvider *> providers_;
    IRawElementProviderSimple *host_provider_ = nullptr;  ///< `UiaHostProviderFromHwnd`（G19）
    UiaNodeProvider *root_provider_ = nullptr;
    std::vector<PendingEvent> pending_;
};

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS && (AURORA_BACKEND_WIN32 || AURORA_BACKEND_D3D11)
