#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/event/keycode.h"
#include "aurora/core/string_util.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 单选按钮组：互斥选项垂直/水平排列。
 *
 * 选项为字符串列表，选中序号存于响应式 `selected()`。
 * 单控件承载整组（组内互斥天然保证），比分散 `Radio` + 手动 group 更 AI 友好。
 *
 * 视觉（对标 Material RadioButton / Qt `QRadioButton`+`QButtonGroup`）：
 * - 外圈圆环 + 选中内实心圆点；`active_color` 未显式设置时跟随主题 `Theme::primary`；
 * - 圆点尺寸 / 行高 / 字号 / 文本色均可配置；禁用（`set_enabled(false)`）灰化并忽略点击。
 *
 * 继承扩展点（protected 虚函数）：`paint_option`（逐选项行绘制，子类可整行自定义）。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class RadioGroup : public Widget {
  public:
    RadioGroup() = default;
    explicit RadioGroup(std::vector<std::string> options, int initial = 0, bool horizontal = false)
        : m_options(std::move(options)), m_horizontal(horizontal) {
        const int max_idx = static_cast<int>(m_options.size()) - 1;
        m_selected.set(std::clamp(initial, 0, std::max(0, max_idx)));
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "RadioGroup"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "RadioGroup",
            .properties = {
                { .name = "options", .type = "vector<string>", .default_value = "[]", .required = true, .note = "选项列表", .json_type = "array" },
                { .name = "selected_index", .type = "int", .default_value = "0", .required = false, .note = "选中序号", .json_type = "integer", .enum_values = {}, .min_value = "0" },
                { .name = "horizontal", .type = "bool", .default_value = "false", .required = false, .note = "水平排列", .json_type = "boolean" },
                { .name = "active_color", .type = "Color", .default_value = "theme.primary", .required = false, .note = "选中态圆环与圆点色（缺省跟随主题 primary）", .json_type = "array" },
                { .name = "border_color", .type = "Color", .default_value = "{140,140,146,255}", .required = false, .note = "未选中圆环色", .json_type = "array" },
                { .name = "text_color", .type = "Color", .default_value = "{30,30,30,255}", .required = false, .note = "选项文本色", .json_type = "array" },
                { .name = "dot_size", .type = "float", .default_value = "16.0", .required = false, .note = "圆点外径(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "row_height", .type = "float", .default_value = "28.0", .required = false, .note = "行高(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "font_size", .type = "float", .default_value = "13.0", .required = false, .note = "选项字号(pt)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "enabled", .type = "bool", .default_value = "true", .required = false, .note = "是否可交互（禁用灰化）", .json_type = "boolean" },
            },
            .events = { "on_change" },
            .children_policy = "none",
            .invariants = { "selected_index >= 0", "selected_index < options.size()" },
            .examples = { R"(au::RadioGroup({"Yes", "No"}, 0))" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&m_selected); }

    [[nodiscard]] auto option_count() const -> std::size_t { return m_options.size(); }
    [[nodiscard]] auto selected() -> State<int> & { return m_selected; }
    [[nodiscard]] auto selected_index() const -> int { return m_selected.get(); }

    /// @brief 选中指定序号（越界忽略；触发 on_change）。
    auto select(int index) -> void {
        if (index >= 0 && std::cmp_less(index, m_options.size()) && index != m_selected.get()) {
            m_selected.set(index);
            mark_needs_paint();
            if (m_on_change) {
                m_on_change(index);
            }
        }
    }

    /// @brief 设置选择回调（链式）。
    auto set_on_change(std::function<void(int)> cb) -> RadioGroup & {
        m_on_change = std::move(cb);
        return *this;
    }

    /// @brief 设置选中态圆环与圆点色（链式）。不调用则跟随主题 `Theme::primary`。
    auto set_active_color(Color c) -> RadioGroup & {
        m_active_color = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置未选中圆环色（链式）。
    auto set_border_color(Color c) -> RadioGroup & {
        m_border_color = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置选项文本色（链式）。
    auto set_text_color(Color c) -> RadioGroup & {
        m_text_color = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置圆点外径 dp（链式）。
    auto set_dot_size(float s) -> RadioGroup & {
        m_dot_size = s > 0.0f ? s : 16.0f;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置行高 dp（链式；决定选项间距与命中区）。
    auto set_row_height(float h) -> RadioGroup & {
        m_row_height = h > 0.0f ? h : 28.0f;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置选项字号 pt（链式）。
    auto set_font_size(float s) -> RadioGroup & {
        m_font_size = s > 0.0f ? s : 13.0f;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置是否启用（链式）；禁用态灰化绘制并忽略点击。
    auto set_enabled(bool v) -> RadioGroup & {
        m_enabled = v;
        mark_needs_paint();
        return *this;
    }
    [[nodiscard]] auto enabled() const -> bool { return m_enabled; }

    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!m_enabled) {
            e.handled = true; // 禁用态吞掉点击（不冒泡），但不切换
            return;
        }
        if (e.action == MouseAction::Press) {
            const int idx = m_horizontal ? hit_index_horizontal(e.local_position.x)
                                         : static_cast<int>(e.local_position.y / m_row_height);
            if (idx >= 0 && std::cmp_less(idx, m_options.size())) {
                select(idx);
                e.handled = true;
                return;
            }
        }
        Widget::on_pointer_event(e);
    }

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        Json opts = Json::array();
        for (const auto &o : m_options) {
            opts.push_back(o);
        }
        props["options"] = opts;
        props["selected_index"] = m_selected.get();
        props["horizontal"] = m_horizontal;
        if (m_active_color.has_value()) {
            props["active_color"] = color_to_json(*m_active_color); // 未设置不输出：保留「跟随主题」语义
        }
        props["border_color"] = color_to_json(m_border_color);
        props["text_color"] = color_to_json(m_text_color);
        props["dot_size"] = m_dot_size;
        props["row_height"] = m_row_height;
        props["font_size"] = m_font_size;
        props["enabled"] = m_enabled;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("options") && props["options"].is_array()) {
            m_options.clear();
            for (const auto &o : props["options"]) {
                m_options.push_back(o.get<std::string>());
            }
        }
        if (props.contains("selected_index")) {
            m_selected.set(props["selected_index"].get<int>());
        }
        if (props.contains("horizontal")) {
            m_horizontal = props["horizontal"].get<bool>();
        }
        if (props.contains("active_color")) {
            m_active_color = json_to_color(props["active_color"]);
        }
        if (props.contains("border_color")) {
            m_border_color = json_to_color(props["border_color"]);
        }
        if (props.contains("text_color")) {
            m_text_color = json_to_color(props["text_color"]);
        }
        if (props.contains("dot_size")) {
            m_dot_size = props["dot_size"].get<float>();
        }
        if (props.contains("row_height")) {
            m_row_height = props["row_height"].get<float>();
        }
        if (props.contains("font_size")) {
            m_font_size = props["font_size"].get<float>();
        }
        if (props.contains("enabled")) {
            m_enabled = props["enabled"].get<bool>();
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        Font f;
        f.size_pt = m_font_size;
        if (m_horizontal) {
            float w = 0.0f;
            for (const auto &o : m_options) {
                w += item_width(render::FontEngine::measure_width(o, f));
            }
            return c.constrain(Size{ .width = w, .height = m_row_height });
        }
        float max_w = 0.0f;
        for (const auto &o : m_options) {
            max_w = std::max(max_w, item_width(render::FontEngine::measure_width(o, f)));
        }
        return c.constrain(Size{ .width = max_w, .height = static_cast<float>(m_options.size()) * m_row_height });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        Font f;
        f.size_pt = m_font_size;
        // 状态色解析：显式设置优先，否则跟随主题 primary；禁用态统一灰化。
        Color accent = m_active_color.value_or(inherit_theme(ctx).primary);
        Color idle = m_border_color;
        Color text = m_text_color;
        if (!m_enabled) {
            accent = Color{ 176, 176, 180, 255 };
            idle = Color{ 200, 200, 204, 255 };
            text = Color{ 168, 168, 172, 255 };
        }
        float x = bounds.origin.x;
        float y = bounds.origin.y;
        for (std::size_t i = 0; i < m_options.size(); ++i) {
            const bool sel = std::cmp_equal(i, m_selected.get());
            const Rect row{ .origin = Point{ .x = x, .y = y },
                            .size = Size{ .width = m_horizontal
                                                       ? item_width(render::FontEngine::measure_width(m_options[i], f))
                                                       : bounds.size.width,
                                          .height = m_row_height } };
            paint_option(p, i, row, sel, accent, idle, text, f);
            if (m_horizontal) {
                x += row.size.width;
            } else {
                y += m_row_height;
            }
        }
    }

    /// @brief 继承扩展点：绘制单个选项行（圆点 + 文本）。子类可覆盖以自定义整行外观。
    virtual auto paint_option(Painter &p, std::size_t index, const Rect &row, bool selected, Color accent, Color idle,
                              Color text, const Font &f) -> void {
        // 圆点：外圈真圆环（draw_rounded_border，radius = 半径即圆）+ 选中内实心圆点，
        // 对标 Material RadioButton。
        const Rect outer{ .origin = Point{ .x = row.origin.x + 4.0f,
                                           .y = row.origin.y + ((m_row_height - m_dot_size) * 0.5f) },
                          .size = Size{ .width = m_dot_size, .height = m_dot_size } };
        p.draw_rounded_border(outer, outer.size.width * 0.5f, selected ? 2.0f : 1.5f, selected ? accent : idle);
        if (selected) {
            const float d = outer.size.width * 0.5f; // 内点直径 = 外圈一半
            const Rect inner{ .origin = Point{ .x = outer.origin.x + ((outer.size.width - d) * 0.5f),
                                               .y = outer.origin.y + ((outer.size.height - d) * 0.5f) },
                              .size = Size{ .width = d, .height = d } };
            p.fill_rounded_rect(inner, d * 0.5f, accent);
        }
        const Rect text_box{ .origin = Point{ .x = row.origin.x + m_dot_size + 10.0f, .y = row.origin.y + 6.0f },
                             .size = Size{ .width = 400.0f, .height = m_row_height - 12.0f } };
        p.draw_text(text_box, m_options[index], f, text);
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        return Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }.contains(local) ? this : nullptr;
    }

    [[nodiscard]] auto item_width(float text_w) const -> float { return text_w + m_dot_size + 20.0f; }

    [[nodiscard]] auto hit_index_horizontal(float x) const -> int {
        Font f;
        f.size_pt = m_font_size;
        float acc = 0.0f;
        for (std::size_t i = 0; i < m_options.size(); ++i) {
            const float w = item_width(render::FontEngine::measure_width(m_options[i], f));
            if (x >= acc && x < acc + w) {
                return static_cast<int>(i);
            }
            acc += w;
        }
        return -1;
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    std::vector<std::string> m_options;
    State<int> m_selected{ 0 };
    bool m_horizontal = false;
    std::function<void(int)> m_on_change;
    std::optional<Color> m_active_color;                ///< 选中态圆环/圆点色；空 = 跟随主题 primary
    Color m_border_color = Color{ 140, 140, 146, 255 }; ///< 未选中圆环色
    Color m_text_color = Color{ 30, 30, 30, 255 };      ///< 选项文本色
    float m_dot_size = 16.0f;                           ///< 圆点外径 dp
    float m_row_height = 28.0f;                         ///< 行高 dp
    float m_font_size = 13.0f;                          ///< 选项字号 pt
    bool m_enabled = true;                              ///< 禁用态灰化并忽略点击
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

/**
 * @brief 数字输入框：带上下箭头的数值调节。
 *
 * 值域 [min, max]，按 `step` 递增/递减；`value()` 为响应式状态可订阅。
 * 支持前缀/后缀文本（如 "$"、"px"）；上下方向键可调节（获焦后）。
 *
 * 视觉：背景 / 边框 / 文本 / 箭头颜色与圆角、字号均可配置；
 * 禁用（`set_enabled(false)`）灰化并忽略交互。
 *
 * 继承扩展点（protected 虚函数）：`paint_box` / `paint_value` / `paint_arrows`。
 *
 * 对标 Qt `QSpinBox`/`QDoubleSpinBox`、WPF `NumericUpDown`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class SpinBox : public Widget {
  public:
    SpinBox() = default;
    SpinBox(double initial, double min_v, double max_v, double step = 1.0)
        : m_min(min_v), m_max(max_v < min_v ? min_v : max_v), m_step(step <= 0.0 ? 1.0 : step) {
        m_value.set(std::clamp(initial, m_min, m_max));
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "SpinBox"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "SpinBox",
            .properties = {
                { .name = "value", .type = "double", .default_value = "0", .required = false, .note = "当前值", .json_type = "number" },
                { .name = "min", .type = "double", .default_value = "0", .required = false, .note = "最小值", .json_type = "number" },
                { .name = "max", .type = "double", .default_value = "100", .required = false, .note = "最大值", .json_type = "number" },
                { .name = "step", .type = "double", .default_value = "1", .required = false, .note = "步长", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "prefix", .type = "string", .default_value = "\"\"", .required = false, .note = "前缀文本", .json_type = "string" },
                { .name = "suffix", .type = "string", .default_value = "\"\"", .required = false, .note = "后缀文本", .json_type = "string" },
                { .name = "decimals", .type = "int", .default_value = "0", .required = false, .note = "显示小数位数", .json_type = "integer", .enum_values = {}, .min_value = "0" },
                { .name = "background", .type = "Color", .default_value = "Color::white()", .required = false, .note = "背景色", .json_type = "array" },
                { .name = "border_color", .type = "Color", .default_value = "{200,200,205,255}", .required = false, .note = "边框色", .json_type = "array" },
                { .name = "text_color", .type = "Color", .default_value = "{30,30,30,255}", .required = false, .note = "数值文本色", .json_type = "array" },
                { .name = "arrow_color", .type = "Color", .default_value = "{100,100,105,255}", .required = false, .note = "箭头颜色", .json_type = "array" },
                { .name = "corner_radius", .type = "float", .default_value = "4.0", .required = false, .note = "圆角半径(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "font_size", .type = "float", .default_value = "13.0", .required = false, .note = "数值字号(pt)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "enabled", .type = "bool", .default_value = "true", .required = false, .note = "是否可交互（禁用灰化）", .json_type = "boolean" },
            },
            .events = { "on_change" },
            .children_policy = "none",
            .invariants = { "min <= max", "step >= 0", "value >= min", "value <= max" },
            .examples = { "au::SpinBox(50, 0, 100, 5)" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&m_value); }

    [[nodiscard]] auto value() -> State<double> & { return m_value; }
    [[nodiscard]] auto value_of() const -> double { return m_value.get(); }
    [[nodiscard]] auto min_value() const -> double { return m_min; }
    [[nodiscard]] auto max_value() const -> double { return m_max; }
    [[nodiscard]] auto step() const -> double { return m_step; }

    /// @brief 设置值（钳制到 [min,max]；触发 on_change）。
    auto set_value(double v) -> void {
        const double clamped = std::clamp(v, m_min, m_max);
        if (clamped != m_value.get()) {
            m_value.set(clamped);
            mark_needs_paint();
            if (m_on_change) {
                m_on_change(clamped);
            }
        }
    }

    /// @brief 递增一步。
    auto increment() -> void { set_value(m_value.get() + m_step); }
    /// @brief 递减一步。
    auto decrement() -> void { set_value(m_value.get() - m_step); }

    /// @brief 设置前后缀（链式）。
    auto set_prefix(std::string s) -> SpinBox & {
        m_prefix = std::move(s);
        return *this;
    }
    auto set_suffix(std::string s) -> SpinBox & {
        m_suffix = std::move(s);
        return *this;
    }
    /// @brief 设置小数位数（链式，0=整数显示）。
    auto set_decimals(int n) -> SpinBox & {
        m_decimals = std::clamp(n, 0, 9);
        return *this;
    }

    /// @brief 设置回调（链式）。
    auto set_on_change(std::function<void(double)> cb) -> SpinBox & {
        m_on_change = std::move(cb);
        return *this;
    }

    /// @brief 设置背景色（链式）。
    auto set_background(Color c) -> SpinBox & {
        m_background = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置边框色（链式）。
    auto set_border_color(Color c) -> SpinBox & {
        m_border_color = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置数值文本色（链式）。
    auto set_text_color(Color c) -> SpinBox & {
        m_text_color = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置箭头颜色（链式）。
    auto set_arrow_color(Color c) -> SpinBox & {
        m_arrow_color = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置圆角半径 dp（链式；0 = 直角）。
    auto set_corner_radius(float r) -> SpinBox & {
        m_corner_radius = r >= 0.0f ? r : 0.0f;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置数值字号 pt（链式）。
    auto set_font_size(float s) -> SpinBox & {
        m_font_size = s > 0.0f ? s : 13.0f;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置是否启用（链式）；禁用态灰化绘制并忽略交互。
    auto set_enabled(bool v) -> SpinBox & {
        m_enabled = v;
        mark_needs_paint();
        return *this;
    }
    [[nodiscard]] auto enabled() const -> bool { return m_enabled; }

    /// @brief 当前显示文本（前缀 + 值 + 后缀）。
    [[nodiscard]] auto display_text() const -> std::string {
        return m_prefix + internal::string_format("%.*f", m_decimals, m_value.get()) + m_suffix;
    }

    /// @brief 点击箭头区调节（右侧上下两半）。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!m_enabled) {
            e.handled = true; // 禁用态吞掉点击（不冒泡），但不调节
            return;
        }
        if (e.action == MouseAction::Press) {
            const float arrow_x = m_size.width - m_aurora_arrow_zone;
            if (e.local_position.x >= arrow_x) {
                if (e.local_position.y < m_size.height * 0.5f) {
                    increment();
                } else {
                    decrement();
                }
                e.handled = true;
                return;
            }
            request_focus(); // 值区点击获焦，随后可用上下方向键调节
        }
        Widget::on_pointer_event(e);
    }

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    /// @brief 上下方向键调节（获焦后）。
    auto on_key_event(KeyEvent &e) -> void override {
        if (m_enabled && e.action == KeyAction::Down) {
            if (e.key == static_cast<int>(KeyCode::ArrowUp)) {
                increment();
                e.handled = true;
                return;
            }
            if (e.key == static_cast<int>(KeyCode::ArrowDown)) {
                decrement();
                e.handled = true;
                return;
            }
        }
        Widget::on_key_event(e);
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["value"] = m_value.get();
        props["min"] = m_min;
        props["max"] = m_max;
        props["step"] = m_step;
        props["prefix"] = m_prefix;
        props["suffix"] = m_suffix;
        props["decimals"] = m_decimals;
        props["background"] = color_to_json(m_background);
        props["border_color"] = color_to_json(m_border_color);
        props["text_color"] = color_to_json(m_text_color);
        props["arrow_color"] = color_to_json(m_arrow_color);
        props["corner_radius"] = m_corner_radius;
        props["font_size"] = m_font_size;
        props["enabled"] = m_enabled;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("min")) {
            m_min = props["min"].get<double>();
        }
        if (props.contains("max")) {
            m_max = props["max"].get<double>();
        }
        if (props.contains("step")) {
            m_step = props["step"].get<double>();
        }
        if (props.contains("prefix")) {
            m_prefix = props["prefix"].get<std::string>();
        }
        if (props.contains("suffix")) {
            m_suffix = props["suffix"].get<std::string>();
        }
        if (props.contains("decimals")) {
            m_decimals = props["decimals"].get<int>();
        }
        if (props.contains("background")) {
            m_background = json_to_color(props["background"]);
        }
        if (props.contains("border_color")) {
            m_border_color = json_to_color(props["border_color"]);
        }
        if (props.contains("text_color")) {
            m_text_color = json_to_color(props["text_color"]);
        }
        if (props.contains("arrow_color")) {
            m_arrow_color = json_to_color(props["arrow_color"]);
        }
        if (props.contains("corner_radius")) {
            m_corner_radius = props["corner_radius"].get<float>();
        }
        if (props.contains("font_size")) {
            m_font_size = props["font_size"].get<float>();
        }
        if (props.contains("enabled")) {
            m_enabled = props["enabled"].get<bool>();
        }
        if (props.contains("value")) {
            m_value.set(std::clamp(props["value"].get<double>(), m_min, m_max));
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{ .width = 120.0f, .height = m_aurora_box_height });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        // 状态色解析：禁用态统一灰化。
        Color bg = m_background;
        Color border = m_border_color;
        Color text = m_text_color;
        Color arrow = m_arrow_color;
        if (!m_enabled) {
            bg = Color{ 235, 235, 237, 255 };
            border = Color{ 215, 215, 219, 255 };
            text = Color{ 168, 168, 172, 255 };
            arrow = Color{ 190, 190, 194, 255 };
        }
        paint_box(p, bounds, bg, border);
        paint_value(p, bounds, text);
        paint_arrows(p, bounds, arrow);
    }

    /// @brief 继承扩展点：绘制值框（背景 + 边框，圆角可配）。
    virtual auto paint_box(Painter &p, const Rect &bounds, Color bg, Color border) -> void {
        if (m_corner_radius > 0.0f) {
            p.fill_rounded_rect(bounds, m_corner_radius, bg);
            p.draw_rounded_border(bounds, m_corner_radius, 1.0f, border);
        } else {
            p.fill_rect(bounds, bg);
            p.draw_rect(bounds, border);
        }
    }

    /// @brief 继承扩展点：绘制数值文本（前缀 + 值 + 后缀）。
    virtual auto paint_value(Painter &p, const Rect &bounds, Color text) -> void {
        Font f;
        f.size_pt = m_font_size;
        const Rect text_box{ .origin = Point{ .x = bounds.origin.x + 8.0f, .y = bounds.origin.y + 8.0f },
                             .size = Size{ .width = bounds.size.width - m_aurora_arrow_zone - 12.0f,
                                           .height = bounds.size.height - 16.0f } };
        p.draw_text(text_box, display_text(), f, text);
    }

    /// @brief 继承扩展点：绘制上下箭头区。
    virtual auto paint_arrows(Painter &p, const Rect &bounds, Color arrow) -> void {
        const float ax = bounds.origin.x + bounds.size.width - m_aurora_arrow_zone;
        Font sf;
        sf.size_pt = 9.0f;
        const Rect up_box{ .origin = Point{ .x = ax + 6.0f, .y = bounds.origin.y + 2.0f },
                           .size = Size{ .width = m_aurora_arrow_zone - 8.0f,
                                         .height = (bounds.size.height * 0.5f) - 2.0f } };
        const Rect dn_box{ .origin = Point{ .x = ax + 6.0f, .y = bounds.origin.y + (bounds.size.height * 0.5f) },
                           .size = Size{ .width = m_aurora_arrow_zone - 8.0f,
                                         .height = (bounds.size.height * 0.5f) - 2.0f } };
        p.draw_text(up_box, "^", sf, arrow);
        p.draw_text(dn_box, "v", sf, arrow);
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        return Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }.contains(local) ? this : nullptr;
    }

    static constexpr float m_aurora_box_height = 30.0f; ///< 输入框高度(dp)
    static constexpr float m_aurora_arrow_zone = 22.0f; ///< 箭头区宽度(dp)

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    State<double> m_value{ 0.0 };
    double m_min = 0.0;
    double m_max = 100.0;
    double m_step = 1.0;
    std::string m_prefix;
    std::string m_suffix;
    int m_decimals = 0;
    std::function<void(double)> m_on_change;
    Color m_background = Color{ 255, 255, 255, 255 };   ///< 背景色
    Color m_border_color = Color{ 200, 200, 205, 255 }; ///< 边框色
    Color m_text_color = Color{ 30, 30, 30, 255 };      ///< 数值文本色
    Color m_arrow_color = Color{ 100, 100, 105, 255 };  ///< 箭头颜色
    float m_corner_radius = 4.0f;                       ///< 圆角半径 dp；0 = 直角
    float m_font_size = 13.0f;                          ///< 数值字号 pt
    bool m_enabled = true;                              ///< 禁用态灰化并忽略交互
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

} // namespace aurora
