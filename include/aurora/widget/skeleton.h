#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>

#include "aurora/core/color.h"
#include "aurora/render/painter.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 骨架屏占位控件（叶控件）。
 *
 * 常见加载占位 UX：以底色矩形 + 横向移动的高亮带模拟「内容正在加载」。
 * - `tick(now)` 推进 shimmer 相位（0..1 循环），变化时请求重绘；
 * - `phase()` 暴露当前相位，便于测试与外部驱动；
 * - `size` 宽度为 0 时占满约束最大宽度（适配列表行占位）。
 *
 * headless 渲染：底色 `fill_rect` + 高亮带 `fill_rect`，无 GUI 依赖。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Skeleton : public LeafWidget {
  public:
    Skeleton() = default;
    explicit Skeleton(Size sz) : size_(sz) {
        // 骨架屏持续动画：每帧需 tick 推进相位，须开启 gesture-tick 驱动
        // （Widget::tick 在 !m_needs_gesture_tick 时直接返回，否则 tick_gestures 永不运行）。
        needs_gesture_tick_ = true;
    }

    [[nodiscard]] auto size_hint() const -> Size { return size_; }
    auto set_size(Size s) -> Skeleton & {
        size_ = s;
        return *this;
    }
    auto set_color(Color c) -> Skeleton & {
        base_ = c;
        return *this;
    }
    auto set_highlight(Color c) -> Skeleton & {
        highlight_ = c;
        return *this;
    }
    /// @brief shimmer 循环周期（秒），<=0 视为默认 1.5s。
    auto set_duration(double seconds) -> Skeleton & {
        duration_ = seconds > 0.0 ? seconds : 1.5;
        return *this;
    }
    /// @brief 当前 shimmer 相位 [0,1)。
    [[nodiscard]] auto phase() const -> double { return phase_; }

    [[nodiscard]] auto type_name() const -> const char * override { return "Skeleton"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Skeleton",
            .properties =
                {
                    {.name = "width",
                     .type = "Length",
                     .default_value = "auto",
                     .required = false,
                     .note = "占位宽（0=占满）"},
                    {.name = "height", .type = "Length", .default_value = "16", .required = false, .note = "占位高"},
                    {.name = "color",
                     .type = "Color",
                     .default_value = "{220,220,220,255}",
                     .required = false,
                     .note = "底色"},
                    {.name = "highlight",
                     .type = "Color",
                     .default_value = "{255,255,255,255}",
                     .required = false,
                     .note = "高亮带色"},
                    {.name = "duration",
                     .type = "double",
                     .default_value = "1.5",
                     .required = false,
                     .note = "shimmer 周期(秒)"},
                },
            .events = {},
            .children_policy = "none",
            .examples = {"au::Skeleton(au::Size{120,16})"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["width"] = size_.width;
        props["height"] = size_.height;
        props["color"] = color_to_json(base_);
        props["highlight"] = color_to_json(highlight_);
        props["duration"] = duration_;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("width")) {
            size_.width = props["width"].get<float>();
        }
        if (props.contains("height")) {
            size_.height = props["height"].get<float>();
        }
        if (props.contains("color")) {
            base_ = json_to_color(props["color"]);
        }
        if (props.contains("highlight")) {
            highlight_ = json_to_color(props["highlight"]);
        }
        if (props.contains("duration")) {
            duration_ = props["duration"].get<double>();
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        const float w = size_.width > 0.0F ? size_.width : c.max.width;
        const float h = size_.height > 0.0F ? size_.height : 16.0F;
        return c.constrain(Size{.width = w, .height = h});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        p.fill_rect(bounds, base_);
        const float band_w = bounds.size.width * 0.3F;
        const float travel = bounds.size.width + band_w;
        float x = bounds.origin.x + (static_cast<float>(phase_) * travel) - band_w;
        x = std::max(x, bounds.origin.x);
        const Rect band{.origin = Point{.x = x, .y = bounds.origin.y},
                        .size = Size{.width = band_w, .height = bounds.size.height}};
        p.fill_rect(band, highlight_);
    }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        if (start_ == std::chrono::steady_clock::time_point{}) {
            start_ = now;
        }
        const double elapsed = std::chrono::duration<double>(now - start_).count();
        const double dur = duration_ > 0.0 ? duration_ : 1.5;
        double ph = std::fmod(elapsed / dur, 1.0);
        if (ph < 0.0) {
            ph += 1.0;
        }
        if (ph != phase_) {
            phase_ = ph;
            mark_needs_paint();
        }
    }

  private:
    Size size_{.width = 0.0F, .height = 16.0F};
    Color base_{220, 220, 220, 255};
    Color highlight_{255, 255, 255, 255};
    double duration_ = 1.5;
    double phase_ = 0.0;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace aurora
