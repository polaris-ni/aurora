#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>

#include "aurora/core/color.h"
#include "aurora/render/painter.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 骨架屏占位控件（叶控件，§6 新增）。
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
    explicit Skeleton(Size sz) : m_size(sz) {
        // 骨架屏持续动画：每帧需 tick 推进相位，须开启 gesture-tick 驱动
        // （Widget::tick 在 !m_needs_gesture_tick 时直接返回，否则 tick_gestures 永不运行）。
        m_needs_gesture_tick = true;
    }

    [[nodiscard]] auto size_hint() const -> Size { return m_size; }
    auto set_size(Size s) -> Skeleton & {
        m_size = s;
        return *this;
    }
    auto set_color(Color c) -> Skeleton & {
        m_base = c;
        return *this;
    }
    auto set_highlight(Color c) -> Skeleton & {
        m_highlight = c;
        return *this;
    }
    /// @brief shimmer 循环周期（秒），<=0 视为默认 1.5s。
    auto set_duration(double seconds) -> Skeleton & {
        m_duration = seconds > 0.0 ? seconds : 1.5;
        return *this;
    }
    /// @brief 当前 shimmer 相位 [0,1)。
    [[nodiscard]] auto phase() const -> double { return m_phase; }

    [[nodiscard]] auto type_name() const -> const char * override { return "Skeleton"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Skeleton",
            .properties = {
                { .name = "width", .type = "Length", .default_value = "auto", .required = false, .note = "占位宽（0=占满）" },
                { .name = "height", .type = "Length", .default_value = "16", .required = false, .note = "占位高" },
                { .name = "color", .type = "Color", .default_value = "{220,220,220,255}", .required = false, .note = "底色" },
                { .name = "highlight", .type = "Color", .default_value = "{255,255,255,255}", .required = false, .note = "高亮带色" },
                { .name = "duration", .type = "double", .default_value = "1.5", .required = false, .note = "shimmer 周期(秒)" },
            },
            .events = {},
            .children_policy = "none",
            .examples = { "au::Skeleton(au::Size{120,16})" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["width"] = m_size.width;
        props["height"] = m_size.height;
        props["color"] = color_to_json(m_base);
        props["highlight"] = color_to_json(m_highlight);
        props["duration"] = m_duration;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("width")) {
            m_size.width = props["width"].get<float>();
        }
        if (props.contains("height")) {
            m_size.height = props["height"].get<float>();
        }
        if (props.contains("color")) {
            m_base = json_to_color(props["color"]);
        }
        if (props.contains("highlight")) {
            m_highlight = json_to_color(props["highlight"]);
        }
        if (props.contains("duration")) {
            m_duration = props["duration"].get<double>();
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        const float w = m_size.width > 0.0f ? m_size.width : c.max.width;
        const float h = m_size.height > 0.0f ? m_size.height : 16.0f;
        return c.constrain(Size{ .width = w, .height = h });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        p.fill_rect(bounds, m_base);
        const float band_w = bounds.size.width * 0.3f;
        const float travel = bounds.size.width + band_w;
        float x = bounds.origin.x + (static_cast<float>(m_phase) * travel) - band_w;
        x = std::max(x, bounds.origin.x);
        const Rect band{ .origin = Point{ .x = x, .y = bounds.origin.y },
                         .size = Size{ .width = band_w, .height = bounds.size.height } };
        p.fill_rect(band, m_highlight);
    }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        if (m_start == std::chrono::steady_clock::time_point{}) {
            m_start = now;
        }
        const double elapsed = std::chrono::duration<double>(now - m_start).count();
        const double dur = m_duration > 0.0 ? m_duration : 1.5;
        double ph = std::fmod(elapsed / dur, 1.0);
        if (ph < 0.0) {
            ph += 1.0;
        }
        if (ph != m_phase) {
            m_phase = ph;
            mark_needs_paint();
        }
    }

  private:
    Size m_size{ .width = 0.0f, .height = 16.0f };
    Color m_base{ 220, 220, 220, 255 };
    Color m_highlight{ 255, 255, 255, 255 };
    double m_duration = 1.5;
    double m_phase = 0.0;
    std::chrono::steady_clock::time_point m_start;
};

} // namespace aurora
