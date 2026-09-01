#pragma once

#include <algorithm>
#include <optional>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/types.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/render/font_engine.h"
#include "aurora/state/reactive.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief Button 属性（聚合）。未显式设置的 optional 颜色按状态自动派生（hover/pressed
/// 由背景色调暗得到，禁用态用统一灰化），与 Checkbox 「缺省跟随主题」语义一致。
struct ButtonProps {
    Reactive<LocalizedString> label;       ///< 按钮文字
    Reactive<Color> color = Color::blue(); ///< 背景色
    Color on_color = Color::white();       ///< 文字色
    Font font = Font{};                    ///< 字体
    float corner_radius = 6.0f;            ///< 圆角半径（dp），>0 时背景与圆角裁剪
    EdgeInsets padding =
        EdgeInsets{ .left = 12.0f, .top = 6.0f, .right = 12.0f, .bottom = 6.0f }; ///< 文字与背景之间的内边距
    bool enabled = true; ///< 是否可点击（禁用态降级绘制并忽略点击）
    // ---- 状态/样式扩展（对标 Flutter ButtonStyle / Qt QPushButton）----
    std::optional<Color> hover_color;         ///< 悬停背景色；缺省 = 背景色 ×0.92 调暗
    std::optional<Color> pressed_color;       ///< 按下背景色；缺省 = 背景色 ×0.80 调暗
    std::optional<Color> border_color;        ///< 边框色；缺省 = 无边框（填充按钮）；搭配 border_width 可做描边按钮
    float border_width = 0.0f;                ///< 边框线宽（dp）；0 = 不描边
    std::optional<Color> disabled_color;      ///< 禁用态背景色；缺省 = {200,200,200}
    std::optional<Color> disabled_text_color; ///< 禁用态文字色；缺省 = {130,130,130}
    float min_width = 0.0f;                   ///< 最小宽度（dp）；文字+内边距不足时擑到此宽
    float min_height = 0.0f;                  ///< 最小高度（dp）
};

/**
 * @brief 按钮控件（叶 widget）：填充背景 + 居中文字；点击激活 on_click。
 *
 * 视觉（对标 Material Filled/Outlined Button / Fluent Button）：
 * - 填充态：背景色 + 圆角；悬停/按下自动调暗（或用 hover_color/pressed_color 显式指定）；
 * - 描边态：设置 border_color + border_width（配合透明背景可做 Outlined 风格）；
 * - 禁用态：灰化并忽略点击。
 *
 * 继承扩展点（protected 虚函数，子类可单独覆盖某个绘制阶段）：
 * `resolve_background()` → `paint_background()` → `paint_border()` → `paint_label()`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Button : public LeafWidget, public ButtonProps {
  public:
    Button() = default;
    explicit Button(ButtonProps props) : ButtonProps(std::move(props)) {}
    explicit Button(const std::string &label) { this->label = label; }
    explicit Button(const char *label) { this->label = label; }

    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::function<void()> on_click; ///< 点击回调（同步派发，见 specification/05-event-navigation.md §3）

    /// @brief 运行时可查询的默认属性值（属性默认值的单一事实来源，需求 #5；经实例 `btn.defaults()` 调用亦可）。
    [[nodiscard]] static auto defaults() -> ButtonProps { return ButtonProps{}; }

    /// @brief 设置文字（链式）。
    auto set_label(const std::string &s) -> Button & {
        label = s;
        return *this;
    }

    /// @brief 设置点击回调（链式）。
    auto set_on_click(std::function<void()> fn) -> Button & {
        on_click = std::move(fn);
        return *this;
    }

    /// @brief 设置背景色（链式）。
    auto background(Color c) -> Button & {
        color = c;
        return *this;
    }

    /// @brief 设置文字色（链式）。
    auto text_color(Color c) -> Button & {
        on_color = c;
        return *this;
    }

    /// @brief 设置圆角半径（链式）。
    auto set_corner_radius(float r) -> Button & {
        corner_radius = r;
        return *this;
    }

    /// @brief 设置内边距（链式）。
    auto set_padding(EdgeInsets e) -> Button & {
        padding = e;
        return *this;
    }

    /// @brief 设置是否启用（链式）；禁用态降级绘制并忽略点击。
    auto set_enabled(bool v) -> Button & {
        enabled = v;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置悬停背景色（链式）。不调用则由背景色自动调暗。
    auto set_hover_color(Color c) -> Button & {
        hover_color = c;
        return *this;
    }

    /// @brief 设置按下背景色（链式）。不调用则由背景色自动调暗。
    auto set_pressed_color(Color c) -> Button & {
        pressed_color = c;
        return *this;
    }

    /// @brief 设置边框（链式）；width<=0 不描边。搭配透明背景可做 Outlined 风格。
    auto set_border(Color c, float width = 1.5f) -> Button & {
        border_color = c;
        border_width = width;
        return *this;
    }

    /// @brief 设置禁用态颜色（链式）。不调用则用统一灰化默认值。
    auto set_disabled_colors(Color background, Color text) -> Button & {
        disabled_color = background;
        disabled_text_color = text;
        return *this;
    }

    /// @brief 设置最小尺寸（链式）；文字+内边距不足时擑到此尺寸。
    auto set_min_size(float w, float h) -> Button & {
        min_width = w;
        min_height = h;
        return *this;
    }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        out.push_back(&label);
        out.push_back(&color);
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Button"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Button",
            .properties = {
                { .name="label", .type="LocalizedString", .default_value="\"\"", .required=true, .note="按钮文字", .json_type="string" },
                { .name="color", .type="Color", .default_value="Color::blue()", .required=false, .note="背景色", .json_type="array" },
                { .name="on_color", .type="Color", .default_value="Color::white()", .required=false, .note="文字色", .json_type="array" },
                { .name="font_size", .type="float", .default_value="14.0", .required=false, .note="字号(pt)", .json_type="number", .enum_values={}, .min_value="0" },
                { .name="corner_radius", .type="float", .default_value="6.0", .required=false, .note="圆角半径(dp)", .json_type="number", .enum_values={}, .min_value="0" },
                { .name="padding", .type="EdgeInsets", .default_value="{12,6,12,6}", .required=false, .note="内边距", .json_type="object" },
                { .name="enabled", .type="bool", .default_value="true", .required=false, .note="是否可点击", .json_type="boolean" },
                { .name="hover_color", .type="Color", .default_value="auto", .required=false, .note="悬停背景色（缺省由背景色调暗）", .json_type="array" },
                { .name="pressed_color", .type="Color", .default_value="auto", .required=false, .note="按下背景色（缺省由背景色调暗）", .json_type="array" },
                { .name="border_color", .type="Color", .default_value="none", .required=false, .note="边框色（缺省不描边）", .json_type="array", .enum_values={}, .min_value="", .max_value="", .pattern="", .constraint="", .requires_props={"border_width"} },
                { .name="border_width", .type="float", .default_value="0.0", .required=false, .note="边框线宽(dp)；0=不描边", .json_type="number", .enum_values={}, .min_value="0" },
                { .name="disabled_color", .type="Color", .default_value="{200,200,200}", .required=false, .note="禁用态背景色", .json_type="array" },
                { .name="disabled_text_color", .type="Color", .default_value="{130,130,130}", .required=false, .note="禁用态文字色", .json_type="array" },
                { .name="min_width", .type="float", .default_value="0.0", .required=false, .note="最小宽度(dp)", .json_type="number", .enum_values={}, .min_value="0" },
                { .name="min_height", .type="float", .default_value="0.0", .required=false, .note="最小高度(dp)", .json_type="number", .enum_values={}, .min_value="0" },
                { .name="width", .type="Length", .default_value="auto", .required=false, .note="", .json_type="array" },
                { .name="height", .type="Length", .default_value="auto", .required=false, .note="", .json_type="array" },
                { .name="show", .type="bool", .default_value="true", .required=false, .note="", .json_type="boolean" },
            },
            .events = { "on_click" },
            .children_policy = "none",
            .examples = { "au::Button(au::ButtonProps{ .label = \"OK\" })" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["label"] = label.get().text;
        props["color"] = color_to_json(color.get());
        props["on_color"] = color_to_json(on_color);
        props["font_size"] = font.size_pt;
        props["corner_radius"] = corner_radius;
        props["padding"] = edge_insets_to_json(padding);
        props["enabled"] = enabled;
        // optional 颜色未显式设置不输出：保留「自动派生」语义
        if (hover_color.has_value()) {
            props["hover_color"] = color_to_json(*hover_color);
        }
        if (pressed_color.has_value()) {
            props["pressed_color"] = color_to_json(*pressed_color);
        }
        if (border_color.has_value()) {
            props["border_color"] = color_to_json(*border_color);
        }
        if (border_width > 0.0f) {
            props["border_width"] = border_width;
        }
        if (disabled_color.has_value()) {
            props["disabled_color"] = color_to_json(*disabled_color);
        }
        if (disabled_text_color.has_value()) {
            props["disabled_text_color"] = color_to_json(*disabled_text_color);
        }
        if (min_width > 0.0f) {
            props["min_width"] = min_width;
        }
        if (min_height > 0.0f) {
            props["min_height"] = min_height;
        }
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("label")) {
            label.set(LocalizedString{ props["label"].get<std::string>() });
        }
        if (props.contains("color")) {
            color.set(json_to_color(props["color"]));
        }
        if (props.contains("on_color")) {
            on_color = json_to_color(props["on_color"]);
        }
        if (props.contains("font_size")) {
            font.size_pt = props["font_size"].get<float>();
        }
        if (props.contains("corner_radius")) {
            corner_radius = props["corner_radius"].get<float>();
        }
        if (props.contains("padding")) {
            padding = json_to_edge_insets(props["padding"]);
        }
        if (props.contains("enabled")) {
            enabled = props["enabled"].get<bool>();
        }
        if (props.contains("hover_color")) {
            hover_color = json_to_color(props["hover_color"]);
        }
        if (props.contains("pressed_color")) {
            pressed_color = json_to_color(props["pressed_color"]);
        }
        if (props.contains("border_color")) {
            border_color = json_to_color(props["border_color"]);
        }
        if (props.contains("border_width")) {
            border_width = props["border_width"].get<float>();
        }
        if (props.contains("disabled_color")) {
            disabled_color = json_to_color(props["disabled_color"]);
        }
        if (props.contains("disabled_text_color")) {
            disabled_text_color = json_to_color(props["disabled_text_color"]);
        }
        if (props.contains("min_width")) {
            min_width = props["min_width"].get<float>();
        }
        if (props.contains("min_height")) {
            min_height = props["min_height"].get<float>();
        }
    }

    auto activate() -> void override {
        if (enabled && on_click) {
            on_click();
        }
    }

    /// @brief Button 自带 on_click，作为点击目标消费事件（即便无 Clickable 修饰）。
    [[nodiscard]] auto wants_click() const -> bool override { return enabled && on_click != nullptr; }

    /// @brief 悬停反馈：背景色随悬停态变化（自动调暗或 hover_color）。
    auto on_hover_change(bool entered) -> void override {
        Widget::on_hover_change(entered);
        if (enabled) {
            mark_needs_paint();
        }
    }

    /// @brief 按下/松开时重绘以呈现 pressed 态（基类维护 m_pressed 与点击识别）。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!enabled) {
            e.handled = true; // 禁用态吞掉点击（不冒泡触发父级点击）
            return;
        }
        Widget::on_pointer_event(e);
        if (e.action == MouseAction::Press || e.action == MouseAction::Release) {
            mark_needs_paint();
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        Font f = font;
        if (f.size_pt <= 0.0f) {
            f.size_pt = 14.0f;
        }
        m_cached_text_width = render::FontEngine::measure_width(label.get().text, f);
        m_cached_text_height = render::FontEngine::measure_height(f);
        const float w = std::max(m_cached_text_width + padding.left + padding.right, min_width);
        const float h = std::max(m_cached_text_height + padding.top + padding.bottom, min_height);
        return c.constrain(Size{ .width = w, .height = h });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override;

    // ---- 继承扩展点：子类可单独覆盖某个绘制阶段，无需重写整个 on_paint ----

    /// @brief 解析当前状态（enabled/pressed/hover）下的背景色。
    [[nodiscard]] virtual auto resolve_background() const -> Color {
        if (!enabled) {
            return disabled_color.value_or(Color{ 200, 200, 200, 255 });
        }
        const Color bg = color.get();
        if (m_pressed) {
            return pressed_color.value_or(bg.shaded(0.80f));
        }
        if (hovered()) {
            return hover_color.value_or(bg.shaded(0.92f));
        }
        return bg;
    }

    /// @brief 解析当前状态下的文字色。
    [[nodiscard]] virtual auto resolve_text_color() const -> Color {
        return enabled ? on_color : disabled_text_color.value_or(Color{ 130, 130, 130, 255 });
    }

    /// @brief 绘制背景（圆角填充）。
    virtual auto paint_background(Painter &p, const Rect &bounds, Color bg) -> void;
    /// @brief 绘制边框（border_width>0 且 border_color 已设置时）。
    virtual auto paint_border(Painter &p, const Rect &bounds) -> void;
    /// @brief 绘制居中文字。
    virtual auto paint_label(Painter &p, const Rect &bounds, Color text_color) -> void;

    // 缓存供 on_layout / paint_label 使用；受 protected 扩展点约束，有意非 private。
    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    float m_cached_text_width = 0.0f; ///< on_layout 缓存的文字宽度（dp）
    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    float m_cached_text_height = 0.0f; ///< on_layout 缓存的文字高度（dp）
};

} // namespace aurora
