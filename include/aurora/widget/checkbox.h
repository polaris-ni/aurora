#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/render/painter.h"
#include "aurora/state/binding.h"
#include "aurora/state/reactive.h"
#include "aurora/state/signal_view.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 复选框（叶控件）：勾选状态 `bool`，点击切换。
 *
 * 支持两种值来源（与 TextInput 一致的响应式模式）：
 * - `Reactive<bool>`：内部持有状态，变化触发重绘；
 * - `Binding<bool>`：双向绑定到上游 `State<bool>`，写回上游并随其变化刷新。
 * 变化通过 `onChanged` 回调上报。
 *
 * 视觉（对标 Material 3 Checkbox / Fluent CheckBox）：
 * - 未勾选：圆角方框描边（`border_color`），悬停时描边转激活色并铺淡色底；
 * - 勾选：激活色填充圆角方框 + 抗锯齿白色勾号 ✓（`check_color`），悬停/按下加深填充；
 * - 禁用（`set_enabled(false)`）：灰化并忽略点击。
 * `active_color` 未显式设置时自动跟随主题 `Theme::primary`（ThemeScope 换肤即生效）。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Checkbox : public LeafWidget {
  public:
    Checkbox() = default;
    explicit Checkbox(Reactive<bool> checked, std::function<void(bool)> on_changed = {})
        : m_value(std::move(checked)), m_on_changed(std::move(on_changed)) {}
    explicit Checkbox(Binding<bool> binding, std::function<void(bool)> on_changed = {})
        : m_binding(std::move(binding)), m_value(m_binding.get()), m_on_changed(std::move(on_changed)) {}

    auto set_on_changed(std::function<void(bool)> cb) -> Checkbox & {
        m_on_changed = std::move(cb);
        return *this;
    }

    /// @brief 设置勾选态填充色（链式）。不调用则跟随主题 `Theme::primary`。
    auto set_active_color(Color c) -> Checkbox & {
        m_active_color = c;
        return *this;
    }

    /// @brief 设置未勾选边框色（链式）。
    auto set_border_color(Color c) -> Checkbox & {
        m_border_color = c;
        return *this;
    }

    /// @brief 设置勾号颜色（链式；默认白色）。
    auto set_check_color(Color c) -> Checkbox & {
        m_check_color = c;
        return *this;
    }

    /// @brief 设置方框边长 dp（链式）。
    auto set_size(float s) -> Checkbox & {
        m_size = s;
        return *this;
    }

    /// @brief 设置圆角半径 dp（链式；< 0 表示自动 = 边长 × 0.2）。
    auto set_corner_radius(float r) -> Checkbox & {
        m_corner_radius = r;
        return *this;
    }

    /// @brief 设置未勾选描边宽 dp（链式）。
    auto set_border_width(float w) -> Checkbox & {
        m_border_width = w;
        return *this;
    }

    /// @brief 设置是否启用（链式）；禁用态灰化绘制并忽略点击。
    auto set_enabled(bool v) -> Checkbox & {
        m_enabled = v;
        mark_needs_paint();
        return *this;
    }
    [[nodiscard]] auto enabled() const -> bool { return m_enabled; }

    [[nodiscard]] auto value() const -> bool { return m_binding.bound() ? m_binding.get() : m_value.get(); }

    auto set_value(bool v) -> void {
        if (m_binding.bound()) {
            m_binding.set(v);
        }
        m_value = v;
        if (m_on_changed) {
            m_on_changed(v);
        }
        mark_needs_paint();
    }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        out.push_back(&m_value);
        if (m_binding.bound()) {
            out.push_back(m_binding.target());
        }
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Checkbox"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Checkbox",
            .properties = {
                { .name="checked", .type="bool", .default_value="false", .required=false, .note="勾选状态", .json_type="boolean" },
                { .name="active_color", .type="Color", .default_value="theme.primary", .required=false, .note="勾选态填充色（缺省跟随主题 primary）", .json_type="array" },
                { .name="border_color", .type="Color", .default_value="{140,140,146,255}", .required=false, .note="未勾选边框色", .json_type="array" },
                { .name="check_color", .type="Color", .default_value="Color::white()", .required=false, .note="勾号颜色", .json_type="array" },
                { .name="size", .type="float", .default_value="20.0", .required=false, .note="方框边长(dp)", .json_type="number", .enum_values={}, .min_value="0" },
                { .name="corner_radius", .type="float", .default_value="-1.0", .required=false, .note="圆角半径(dp)；<0 自动=边长×0.2", .json_type="number" },
                { .name="border_width", .type="float", .default_value="1.5", .required=false, .note="未勾选描边宽(dp)", .json_type="number", .enum_values={}, .min_value="0" },
                { .name="enabled", .type="bool", .default_value="true", .required=false, .note="是否可交互（禁用灰化）", .json_type="boolean" },
                { .name="width", .type="Length", .default_value="auto", .required=false, .note="", .json_type="array" },
                { .name="height", .type="Length", .default_value="auto", .required=false, .note="", .json_type="array" },
                { .name="show", .type="bool", .default_value="true", .required=false, .note="", .json_type="boolean" },
            },
            .events = { "on_changed" },
            .children_policy = "none",
            .examples = { "au::Checkbox()" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!m_enabled) {
            e.handled = true; // 禁用态吞掉点击（不冒泡触发父级点击），但不切换
            return;
        }
        if (e.action == MouseAction::Press) {
            m_pressed = true;
            mark_needs_paint(); // 按下态视觉反馈
            e.handled = true;
        } else if (e.action == MouseAction::Release) {
            if (m_pressed) {
                set_value(!value());
            }
            m_pressed = false;
            mark_needs_paint();
            e.handled = true;
        }
    }

    /// @brief 悬停反馈：未勾选描边高亮 + 淡色底；勾选填充加深。
    auto on_hover_change(bool entered) -> void override {
        Widget::on_hover_change(entered);
        mark_needs_paint();
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["checked"] = value();
        if (m_active_color.has_value()) {
            props["active_color"] = color_to_json(*m_active_color); // 未设置不输出：保留「跟随主题」语义
        }
        props["border_color"] = color_to_json(m_border_color);
        props["check_color"] = color_to_json(m_check_color);
        props["size"] = m_size;
        props["corner_radius"] = m_corner_radius;
        props["border_width"] = m_border_width;
        props["enabled"] = m_enabled;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("checked")) {
            set_value(props["checked"].get<bool>());
        }
        if (props.contains("active_color")) {
            m_active_color = json_to_color(props["active_color"]);
        }
        if (props.contains("border_color")) {
            m_border_color = json_to_color(props["border_color"]);
        }
        if (props.contains("check_color")) {
            m_check_color = json_to_color(props["check_color"]);
        }
        if (props.contains("size")) {
            m_size = props["size"].get<float>();
        }
        if (props.contains("corner_radius")) {
            m_corner_radius = props["corner_radius"].get<float>();
        }
        if (props.contains("border_width")) {
            m_border_width = props["border_width"].get<float>();
        }
        if (props.contains("enabled")) {
            m_enabled = props["enabled"].get<bool>();
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{ .width = m_size, .height = m_size });
    }

    /// @brief 三态配色（勾选填充 / 未勾选描边 / 勾号），由 `resolve_palette` 一次性算出。
    struct Palette {
        Color active; ///< 勾选态填充色（显式设置优先，否则跟随主题 primary）
        Color border; ///< 未勾选描边色
        Color check;  ///< 勾号颜色
    };

    /// @brief 解析当前状态配色：禁用时统一灰化。子类可只覆盖配色而不改绘制几何。
    /// @note Side-effects: none
    [[nodiscard]] virtual auto resolve_palette(const BuildContext &ctx) -> Palette {
        Palette pal{ .active = m_active_color.value_or(inherit_theme(ctx).primary),
                     .border = m_border_color,
                     .check = m_check_color };
        if (!m_enabled) {
            // 禁用灰化：统一降饱和（对标 Material disabled 38% 透明语义的软件光栅近似）。
            pal.active = Color{ 176, 176, 180, 255 };
            pal.border = Color{ 200, 200, 204, 255 };
            pal.check = Color{ 245, 245, 247, 255 };
        }
        return pal;
    }

    /// @brief 圆角半径：`corner_radius < 0` 表示自动 = 边长 ×0.2。
    [[nodiscard]] virtual auto corner_radius(const Rect &bounds) -> float {
        return m_corner_radius >= 0.0f ? m_corner_radius : std::min(bounds.size.width, bounds.size.height) * 0.2f;
    }

    /// @brief 绘制勾选态盒体：激活色填充，悬停 / 按下乘性加深（保留色相）。
    /// @note Side-effects: paints
    virtual auto paint_checked_box(Painter &p, const Rect &bounds, float radius, Color active, bool hot) -> void {
        if (hot) {
            active = active.shaded(m_pressed ? 0.72f : 0.86f);
        }
        p.fill_rounded_rect(bounds, radius, active);
    }

    /// @brief 绘制勾号 ✓：两段抗锯齿圆帽线段（Material 折点几何），线宽随边长缩放。
    /// @note Side-effects: paints
    virtual auto paint_check_mark(Painter &p, const Rect &bounds, Color check) -> void {
        const float w = bounds.size.width;
        const float h = bounds.size.height;
        const float lw = std::max(1.6f, w * 0.11f);
        const Point p0{ .x = bounds.origin.x + (w * 0.24f), .y = bounds.origin.y + (h * 0.54f) };
        const Point p1{ .x = bounds.origin.x + (w * 0.42f), .y = bounds.origin.y + (h * 0.72f) };
        const Point p2{ .x = bounds.origin.x + (w * 0.78f), .y = bounds.origin.y + (h * 0.32f) };
        p.draw_line(p0, p1, lw, check);
        p.draw_line(p1, p2, lw, check);
    }

    /// @brief 绘制未勾选态盒体：悬停铺激活色淡底（Fluent 式渐进反馈），描边在 hot 时高亮为激活色。
    /// @note Side-effects: paints
    virtual auto paint_idle_box(Painter &p, const Rect &bounds, float radius, const Palette &pal, bool hot) -> void {
        if (hot) {
            Color tint = pal.active;
            tint.m_a = m_pressed ? 56 : 28;
            p.fill_rounded_rect(bounds, radius, tint);
        }
        p.draw_rounded_border(bounds, radius, m_border_width, hot ? pal.active : pal.border);
    }

    /// @brief 绘制编排：取状态色 → 按勾选态分派到各虚钩子（子类一般只需覆盖钩子，无需重写本函数）。
    /// @note Side-effects: paints
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        const Palette pal = resolve_palette(ctx);
        const float radius = corner_radius(bounds);
        const bool hot = m_enabled && (hovered() || m_pressed);
        if (value()) {
            paint_checked_box(p, bounds, radius, pal.active, hot);
            paint_check_mark(p, bounds, pal.check);
        } else {
            paint_idle_box(p, bounds, radius, pal, hot);
        }
    }

  private:
    Binding<bool> m_binding; // 声明须在 m_value 之前：Binding 构造器用 m_binding.get() 初始化 m_value，
                             // 成员按声明顺序初始化，故 m_binding 须先就位，否则 m_binding.get() 空指针解引用。
    Reactive<bool> m_value;
    std::function<void(bool)> m_on_changed;
    std::optional<Color> m_active_color;                ///< 勾选态填充色；空 = 跟随主题 primary
    Color m_border_color = Color{ 140, 140, 146, 255 }; ///< 未勾选边框色
    Color m_check_color = colors::AURORA_WHITE;         ///< 勾号颜色
    float m_size = 20.0f;                               ///< 方框边长 dp
    float m_corner_radius = -1.0f;                      ///< 圆角半径 dp；< 0 自动 = 边长 × 0.2
    float m_border_width = 1.5f;                        ///< 未勾选描边宽 dp
    bool m_enabled = true;                              ///< 禁用态灰化并忽略点击
};

} // namespace aurora
