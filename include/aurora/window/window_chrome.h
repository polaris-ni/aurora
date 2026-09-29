#pragma once

#include "aurora/window/surface.h"

namespace aurora {

/// @brief 窗口 chrome 服务：经 Environment 注入，子树控件据此驱动窗口动作
/// （自绘标题栏的按钮/拖拽即消费方）。生命周期由 Application→Window→Surface 保证；
/// 控件只在事件派发栈内同步调用（Wayland serial 时效约束）。
class WindowChrome {
  public:
    /// @brief 绑定宿主 Surface（非拥有）以驱动其窗口动作。
    /// @param surface 目标窗口后端裸指针；允许为 nullptr，此时所有动作方法静默跳过。
    explicit WindowChrome(Surface *surface) : surface_(surface) {}

    /// @brief 是否已绑定宿主 Surface。
    /// @return surface_ 非空时为 true。
    [[nodiscard]] auto valid() const -> bool { return surface_ != nullptr; }
    /// @brief 发起系统级窗口拖动（转发 `Surface::begin_window_move`；未绑定时为空操作）。
    auto begin_move() const -> void {
        if (surface_ != nullptr) {
            surface_->begin_window_move();
        }
    }
    /// @brief 发起系统级边缘调整大小（转发 `Surface::begin_window_resize`；未绑定时为空操作）。
    /// @param e 要抓取的调整边。
    auto begin_resize(WindowResizeEdge e) const -> void {
        if (surface_ != nullptr) {
            surface_->begin_window_resize(e);
        }
    }
    /// @brief 最小化窗口（转发 `Surface::minimize`；未绑定时为空操作）。
    auto minimize() const -> void {
        if (surface_ != nullptr) {
            surface_->minimize();
        }
    }
    /// @brief 切换最大化/还原（转发 `Surface::toggle_maximize`；未绑定时为空操作）。
    auto toggle_maximize() const -> void {
        if (surface_ != nullptr) {
            surface_->toggle_maximize();
        }
    }
    /// @brief 进入/退出全屏（转发 `Surface::set_fullscreen`；未绑定时为空操作）。
    /// @param on true=进入全屏，false=退出全屏。
    auto set_fullscreen(bool on) const -> void {
        if (surface_ != nullptr) {
            surface_->set_fullscreen(on);
        }
    }
    /// @brief 关闭窗口（转发 `Surface::close`；未绑定时为空操作）。
    auto close() const -> void {
        if (surface_ != nullptr) {
            surface_->close();
        }
    }
    /// @brief CSD 装饰占用区（自绘标题栏场景用于布局避让）。
    /// @return 绑定时返回 `Surface::content_inset()`，未绑定时为空 EdgeInsets（零占用）。
    [[nodiscard]] auto content_inset() const -> EdgeInsets {
        return (surface_ != nullptr) ? surface_->content_inset() : EdgeInsets{};
    }

  private:
    Surface *surface_ = nullptr;
};

}  // namespace aurora
