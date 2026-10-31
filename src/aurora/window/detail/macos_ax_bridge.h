#pragma once

/// @brief macOS NSAccessibility 桥：语义树快照 ↔ `NSAccessibilityElement` 元素树。
/// @file window/detail/macos_ax_bridge.h
///
/// 与 Win32 UIA / Linux AT-SPI 两座桥同列：内部头（src/，非公共 API），由 `MacOSSurface` 持有。
/// 门控与 `macos_surface.h` 同款：平台宏 ∧ 后端宏。

#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

#include <cstdint>
#include <memory>
#include <string>

#include "aurora/core/a11y_provider.h"

namespace aurora {

class Widget;  // 前向声明：语义树根（完整定义见 widget/widget.h）

namespace detail {

/// @brief macOS 无障碍桥：AppKit 查询（`accessibilityChildren` / `accessibilityHitTest:`）驱动的惰性桥。
///
/// 形态与另两桥一致：
/// - **惰性激活**：无读屏在线时 `MacOSSurface` 连桥对象都不建；首个 AppKit 无障碍查询
///   （内容视图的 `accessibilityChildren`）到达时构造并 `activate()`，同时回填
///   `AccessibilitySettings::screen_reader_active`；
/// - **拉取式**：语义事件只置脏，真正重建发生在下一次查询的 `sync_if_dirty()`；
/// - **快照按值**：元素只持有字段副本与节点 id，控件树销毁不会留下悬垂指针；根销毁经
///   `on_widget_destroying` 立即切断。
///
/// 元素帧坐标：`AccessibilityNode::bounds` 是窗口本地逻辑 dp（y 向下），桥经内容视图
/// `convertRect:toView:` + `convertRectToScreen:` 换成屏幕点（y 向上）——AppKit 的唯一口径。
/// @note Thread: main-thread only（AppKit 无障碍查询本就只在主线程）。
class MacOSAxBridge final : public a11y::Provider {
  public:
    /// @brief 构造桥（不激活）；`view` 是内容视图（`NSView *`，以 `void *` 承载）。
    /// @param view 语义树几何换算的锚点视图（非拥有；生命周期随宿主窗口）。
    /// @return 桥实例；`view` 为空时返回 nullptr。
    [[nodiscard]] static auto create(void *view) -> std::unique_ptr<MacOSAxBridge>;

    ~MacOSAxBridge() override;

    MacOSAxBridge(const MacOSAxBridge &) = delete;
    auto operator=(const MacOSAxBridge &) -> MacOSAxBridge & = delete;
    MacOSAxBridge(MacOSAxBridge &&) = delete;
    auto operator=(MacOSAxBridge &&) -> MacOSAxBridge & = delete;

    // ---- a11y::Provider ----
    auto activate() -> void override;
    auto deactivate() -> void override;
    auto sync_if_dirty() -> void override;
    auto mark_dirty() -> void override;
    [[nodiscard]] auto is_active() const -> bool override;
    [[nodiscard]] auto name() const -> std::string override;
    auto set_root(Widget *root) -> void override;
    auto on_announcement(const std::string &text, const Widget *target) -> void override;
    auto on_widget_destroying(const Widget *w) -> void override;

    // ---- AppKit 侧（内容视图转发；返回值为 `NSArray *` / `NSAccessibilityElement *`）----
    /// @brief 内容视图的无障碍子元素（触发惰性激活 + 拉取式同步）。
    /// @return 自动释放的 `NSArray<NSAccessibilityElement *> *`；无根时为空数组。
    [[nodiscard]] auto children() -> void *;
    /// @brief 屏幕点命中测试：返回最深命中元素（无命中回内容视图自身）。
    /// @param x 屏幕点 x（点，非像素）。
    /// @param y 屏幕点 y（原点主屏左下）。
    /// @return 元素或视图（`id`，以 `void *` 承载）。
    [[nodiscard]] auto hit_test(double x, double y) -> void *;
    /// @brief 当前焦点元素（`NSAccessibilityElement *`；无焦点为 nullptr）。
    /// @return 元素指针（非拥有）。
    [[nodiscard]] auto focused_element() -> void *;

    // ---- 元素回调（`AuroraAxElement` 转发）----
    /// @brief 执行「按下」语义动作（Invoke 优先，回落 Click）。
    /// @param id 节点 id。
    /// @return 控件接受动作时为 true。
    auto perform_press(std::uint64_t id) -> bool;
    /// @brief 执行「增大 / 减小」语义动作（Slider / Progress 的 Value 动作）。
    /// @param id 节点 id。
    /// @param step +1 增大、-1 减小。
    /// @return 控件接受动作时为 true。
    auto perform_step(std::uint64_t id, int step) -> bool;
    /// @brief 读屏请求把焦点移到某节点（`Focus` 动作）。
    /// @param id 节点 id。
    /// @param on true 请求聚焦。
    auto set_focused(std::uint64_t id, bool on) -> void;

  private:
    struct Impl;
    explicit MacOSAxBridge(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace detail
}  // namespace aurora

#endif  // AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS
