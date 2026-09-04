#pragma once

#include "aurora/window/surface.h"

namespace aurora {

/// @brief 窗口 chrome 服务：经 Environment 注入，子树控件据此驱动窗口动作
/// （自绘标题栏的按钮/拖拽即消费方）。生命周期由 Application→Window→Surface 保证；
/// 控件只在事件派发栈内同步调用（Wayland serial 时效约束）。
class WindowChrome {
  public:
    explicit WindowChrome(Surface *surface) : surface_(surface) {}

    [[nodiscard]] auto valid() const -> bool { return surface_ != nullptr; }
    auto begin_move() const -> void {
        if (surface_ != nullptr) {
            surface_->begin_window_move();
        }
    }
    auto begin_resize(WindowResizeEdge e) const -> void {
        if (surface_ != nullptr) {
            surface_->begin_window_resize(e);
        }
    }
    auto minimize() const -> void {
        if (surface_ != nullptr) {
            surface_->minimize();
        }
    }
    auto toggle_maximize() const -> void {
        if (surface_ != nullptr) {
            surface_->toggle_maximize();
        }
    }
    auto set_fullscreen(bool on) const -> void {
        if (surface_ != nullptr) {
            surface_->set_fullscreen(on);
        }
    }
    auto close() const -> void {
        if (surface_ != nullptr) {
            surface_->close();
        }
    }
    /// @brief CSD 装饰占用区（自绘标题栏场景用于布局避让）。
    [[nodiscard]] auto content_inset() const -> EdgeInsets {
        return (surface_ != nullptr) ? surface_->content_inset() : EdgeInsets{};
    }

  private:
    Surface *surface_ = nullptr;
};

}  // namespace aurora
