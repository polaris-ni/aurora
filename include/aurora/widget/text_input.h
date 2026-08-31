#pragma once

#include <algorithm>
#include <optional>
#include <string>

#include "aurora/app/clipboard.h"
#include "aurora/core/types.h"
#include "aurora/core/utf8.h" // aurora::utf8_cp_len / utf8_cp_count / utf8_cp_slice（收口 dup-1）
#include "aurora/event/event.h"
#include "aurora/event/keycode.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/state/reactive.h"
#include "aurora/theming/theme_scope.h" // inherit_theme：聚焦边框色未显式设置时跟随主题 primary
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief TextInput 属性（聚合）。
struct TextInputProps {
    std::string value;       ///< 初始文本
    std::string placeholder; ///< 空值占位提示
    float font_size = 14.0f; ///< 字号（点）
};

/**
 * @brief 文本输入框（叶控件）：绘制边框 + 文本（值或占位）+ 光标。
 *
 * - 点击经 `FocusManager::request_focus` 获焦（触发 `on_focus_change` 重绘聚焦态）。
 * - 聚焦后接收 `onTextInput` 追加字符、`onKeyEvent` 退格（Delete/Backspace）。
 * - 值为 `Reactive<std::string>`，变化触发重绘；headless 下静态渲染当前文本/占位。
 * - 焦点全局协调由 `FocusManager` 负责（Tab 序、焦点派发），见 `aurora/event/focus.h`。
 *
 * 可定制性（对标 Qt QLineEdit / Flutter TextField）：
 * - 颜色：文本/占位/背景/聚焦背景/边框/聚焦边框（缺省跟随主题 primary）/选区高亮；
 * - 行为：`max_length` 限长（码点计数）、`read_only` 只读（可选可复制不可编辑）、
 *   `obscure_text` 密码掩码显示；
 * - 回调：`on_changed`（每次编辑）与 `on_submit`（Enter 提交）。
 *
 * 继承扩展点（protected 虚函数）：`paint_frame`（背景+边框）；内部状态（选区/光标）
 * 为 protected，子类可直接复用。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class TextInput : public LeafWidget {
  public:
    TextInput() = default;
    explicit TextInput(const TextInputProps &props) : m_placeholder(props.placeholder), m_font_size(props.font_size) {
        m_value = props.value;
    }

    /// @brief 设置初始值（链式）。
    auto set_value(const std::string &v) -> TextInput & {
        m_value = v;
        return *this;
    }
    /// @brief 设置占位提示（链式）。
    auto set_placeholder(const std::string &p) -> TextInput & {
        m_placeholder = p;
        return *this;
    }
    /// @brief 设置字号（链式）。
    auto font_size(float s) -> TextInput & {
        m_font_size = s;
        return *this;
    }
    /// @brief 设置圆角半径（链式）。
    auto set_corner_radius(float r) -> TextInput & {
        m_corner_radius = r;
        return *this;
    }
    /// @brief 设置内边距（链式）。
    auto set_padding(EdgeInsets e) -> TextInput & {
        m_padding = e;
        return *this;
    }
    /// @brief 设置光标颜色（链式）。
    auto set_cursor_color(Color c) -> TextInput & {
        m_cursor_color = c;
        return *this;
    }
    /// @brief 设置是否启用（链式）；禁用态降级绘制并忽略输入。
    auto set_enabled(bool v) -> TextInput & {
        m_enabled = v;
        return *this;
    }

    /// @brief 设置文本颜色（链式）。
    auto set_text_color(Color c) -> TextInput & {
        m_text_color = c;
        return *this;
    }
    /// @brief 设置占位提示颜色（链式）。
    auto set_placeholder_color(Color c) -> TextInput & {
        m_placeholder_color = c;
        return *this;
    }
    /// @brief 设置背景色（链式）。
    auto set_background(Color c) -> TextInput & {
        m_background = c;
        return *this;
    }
    /// @brief 设置聚焦态背景色（链式）。
    auto set_focused_background(Color c) -> TextInput & {
        m_focused_background = c;
        return *this;
    }
    /// @brief 设置边框色（链式）。
    auto set_border_color(Color c) -> TextInput & {
        m_border_color = c;
        return *this;
    }
    /// @brief 设置聚焦态边框色（链式）。不调用则跟随主题 `Theme::primary`。
    auto set_focused_border_color(Color c) -> TextInput & {
        m_focused_border_color = c;
        return *this;
    }
    /// @brief 设置边框线宽 dp（链式；0 = 不描边）。
    auto set_border_width(float w) -> TextInput & {
        m_border_width = w >= 0.0f ? w : 1.0f;
        return *this;
    }
    /// @brief 设置选区高亮色（链式）。
    auto set_selection_color(Color c) -> TextInput & {
        m_selection_color = c;
        return *this;
    }
    /// @brief 设置最大长度（链式；码点计数，0 = 不限）。超出部分的输入/粘贴被截断。
    auto set_max_length(std::size_t n) -> TextInput & {
        m_max_length = n;
        return *this;
    }
    /// @brief 设置只读（链式）；只读态可选择/复制但不可编辑（对标 Qt QLineEdit::readOnly）。
    auto set_read_only(bool v) -> TextInput & {
        m_read_only = v;
        return *this;
    }
    [[nodiscard]] auto read_only() const -> bool { return m_read_only; }
    /// @brief 设置密码掩码显示（链式）；显示为 •，值不变（对标 Flutter obscureText）。
    auto set_obscure_text(bool v) -> TextInput & {
        m_obscure = v;
        return *this;
    }
    /// @brief 设置变化回调（链式）；每次用户编辑（输入/退格/剪切/粘贴）后触发。
    auto set_on_changed(std::function<void(const std::string &)> cb) -> TextInput & {
        m_on_changed = std::move(cb);
        return *this;
    }
    /// @brief 设置提交回调（链式）；Enter 键触发。
    auto set_on_submit(std::function<void(const std::string &)> cb) -> TextInput & {
        m_on_submit = std::move(cb);
        return *this;
    }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&m_value); }
    [[nodiscard]] auto type_name() const -> const char * override { return "TextInput"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "TextInput",
            .properties = {
                { .name = "value", .type = "string", .default_value = "\"\"", .required = false, .note = "初始文本", .json_type = "string" },
                { .name = "placeholder", .type = "string", .default_value = "\"\"", .required = false, .note = "空值占位提示", .json_type = "string" },
                { .name = "font_size", .type = "float", .default_value = "14.0", .required = false, .note = "字号(pt)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "corner_radius", .type = "float", .default_value = "0.0", .required = false, .note = "圆角半径(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "padding", .type = "EdgeInsets", .default_value = "{12,12,12,12}", .required = false, .note = "内边距", .json_type = "object" },
                { .name = "cursor_color", .type = "Color", .default_value = "Color::black()", .required = false, .note = "光标颜色", .json_type = "array" },
                { .name = "enabled", .type = "bool", .default_value = "true", .required = false, .note = "是否可编辑", .json_type = "boolean" },
                { .name = "text_color", .type = "Color", .default_value = "{20,20,20,255}", .required = false, .note = "文本颜色", .json_type = "array" },
                { .name = "placeholder_color", .type = "Color", .default_value = "{150,150,150,255}", .required = false, .note = "占位提示颜色", .json_type = "array" },
                { .name = "background", .type = "Color", .default_value = "{240,240,240,255}", .required = false, .note = "背景色", .json_type = "array" },
                { .name = "focused_background", .type = "Color", .default_value = "{245,248,255,255}", .required = false, .note = "聚焦态背景色", .json_type = "array" },
                { .name = "border_color", .type = "Color", .default_value = "{120,120,120,255}", .required = false, .note = "边框色", .json_type = "array" },
                { .name = "focused_border_color", .type = "Color", .default_value = "theme.primary", .required = false, .note = "聚焦态边框色（缺省跟随主题 primary）", .json_type = "array" },
                { .name = "border_width", .type = "float", .default_value = "1.0", .required = false, .note = "边框线宽(dp)；0=不描边", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "selection_color", .type = "Color", .default_value = "{80,120,220,90}", .required = false, .note = "选区高亮色", .json_type = "array" },
                { .name = "max_length", .type = "int", .default_value = "0", .required = false, .note = "最大长度（码点）；0=不限", .json_type = "integer", .enum_values = {}, .min_value = "0" },
                { .name = "read_only", .type = "bool", .default_value = "false", .required = false, .note = "只读（可选择复制不可编辑）", .json_type = "boolean" },
                { .name = "obscure_text", .type = "bool", .default_value = "false", .required = false, .note = "密码掩码显示", .json_type = "boolean" },
                { .name = "width", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
                { .name = "height", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
                { .name = "show", .type = "bool", .default_value = "true", .required = false, .note = "", .json_type = "boolean" },
            },
            .events = { "on_focus_change", "on_changed", "on_submit" },
            .children_policy = "none",
            .examples = { "au::TextInput().set_placeholder(\"Enter name\")" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["value"] = m_value.get();
        props["placeholder"] = m_placeholder;
        props["font_size"] = m_font_size;
        props["corner_radius"] = m_corner_radius;
        props["padding"] = edge_insets_to_json(m_padding);
        props["cursor_color"] = color_to_json(m_cursor_color);
        props["enabled"] = m_enabled;
        props["text_color"] = color_to_json(m_text_color);
        props["placeholder_color"] = color_to_json(m_placeholder_color);
        props["background"] = color_to_json(m_background);
        props["focused_background"] = color_to_json(m_focused_background);
        props["border_color"] = color_to_json(m_border_color);
        if (m_focused_border_color.has_value()) {
            props["focused_border_color"] =
                color_to_json(*m_focused_border_color); // 未设置不输出：保留「跟随主题」语义
        }
        props["border_width"] = m_border_width;
        props["selection_color"] = color_to_json(m_selection_color);
        if (m_max_length > 0) {
            props["max_length"] = m_max_length;
        }
        if (m_read_only) {
            props["read_only"] = m_read_only;
        }
        if (m_obscure) {
            props["obscure_text"] = m_obscure;
        }
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("value")) {
            m_value = props["value"].get<std::string>();
        }
        if (props.contains("placeholder")) {
            m_placeholder = props["placeholder"].get<std::string>();
        }
        if (props.contains("font_size")) {
            m_font_size = props["font_size"].get<float>();
        }
        if (props.contains("corner_radius")) {
            m_corner_radius = props["corner_radius"].get<float>();
        }
        if (props.contains("padding")) {
            m_padding = json_to_edge_insets(props["padding"]);
        }
        if (props.contains("cursor_color")) {
            m_cursor_color = json_to_color(props["cursor_color"]);
        }
        if (props.contains("enabled")) {
            m_enabled = props["enabled"].get<bool>();
        }
        if (props.contains("text_color")) {
            m_text_color = json_to_color(props["text_color"]);
        }
        if (props.contains("placeholder_color")) {
            m_placeholder_color = json_to_color(props["placeholder_color"]);
        }
        if (props.contains("background")) {
            m_background = json_to_color(props["background"]);
        }
        if (props.contains("focused_background")) {
            m_focused_background = json_to_color(props["focused_background"]);
        }
        if (props.contains("border_color")) {
            m_border_color = json_to_color(props["border_color"]);
        }
        if (props.contains("focused_border_color")) {
            m_focused_border_color = json_to_color(props["focused_border_color"]);
        }
        if (props.contains("border_width")) {
            m_border_width = props["border_width"].get<float>();
        }
        if (props.contains("selection_color")) {
            m_selection_color = json_to_color(props["selection_color"]);
        }
        if (props.contains("max_length")) {
            m_max_length = props["max_length"].get<std::size_t>();
        }
        if (props.contains("read_only")) {
            m_read_only = props["read_only"].get<bool>();
        }
        if (props.contains("obscure_text")) {
            m_obscure = props["obscure_text"].get<bool>();
        }
    }

    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!m_enabled) {
            return; // 禁用态不响应任何指针交互
        }
        // 保留基类行为：activate / 长按计时（命中即消费）。
        Widget::on_pointer_event(e);

        // 命中测试与实绘同源：掩码态下按掩码串定位（逐字符一一对应，码点下标一致）。
        const std::string v = display_value();
        const Font f{ .size_pt = m_font_size > 0.0f ? m_font_size : 14.0f };
        const float lx = e.local_position.x - m_padding.left; // 文本左内边距
        if (e.action == MouseAction::Press) {
            // 含头含尾：选区端点用 hit_test_char_inclusive（点击字符任意位置均计入该字符），
            // m_caret 仍用 hit_test_char（caret 模型）作为编辑光标位置。
            const size_t ch = render::FontEngine::hit_test_char_inclusive(v, lx, f);
            m_caret = render::FontEngine::hit_test_char(v, lx, f);
            m_sel_start = ch;     // 锚点（含入字符）
            m_sel_end = m_no_sel; // 尚未形成选区，待拖拽
            m_selecting = true;
            request_focus();
            mark_needs_paint();
            e.handled = true;
        } else if (e.action == MouseAction::Move && m_selecting) {
            const size_t ch = render::FontEngine::hit_test_char_inclusive(v, lx, f);
            m_caret = render::FontEngine::hit_test_char(v, lx, f);
            m_sel_end = ch; // 拖拽终点（含入字符）：按下与松开所在字符均计入选区
            mark_needs_paint();
            e.handled = true;
        } else if (e.action == MouseAction::Release) {
            m_selecting = false;
            e.handled = true;
        }
    }

    auto on_key_event(KeyEvent &e) -> void override { // NOLINT
        if (!m_enabled || !is_focused() || e.action != KeyAction::Down) {
            return;
        }
        const std::string &v = m_value.get();
        const Font f{ .size_pt = m_font_size > 0.0f ? m_font_size : 14.0f };
        const bool shift = (e.modifiers & ModifierKey::Shift) != 0u;  // NOLINT
        const bool ctrl = (e.modifiers & ModifierKey::Control) != 0u; // NOLINT
        const size_t n = cp_count(v);

        if (ctrl && e.key == static_cast<int>(KeyCode::A)) {
            if (n > 0) {
                m_sel_start = 0;
                m_sel_end = n - 1; // 含尾：最后一个字符下标
            } else {
                m_sel_end = m_no_sel;
            }
            m_caret = n;
            mark_needs_paint();
            e.handled = true;
            return;
        }
        if (ctrl && e.key == static_cast<int>(KeyCode::C)) {
            const std::string t = selected_text();
            Clipboard::set_text(t.empty() ? v : t);
            e.handled = true;
            return;
        }
        // Ctrl+X: 剪切（只读态降级为仅复制）
        if (ctrl && e.key == static_cast<int>(KeyCode::X)) {
            const std::string t = selected_text();
            if (!t.empty()) {
                Clipboard::set_text(t);
                if (!m_read_only) {
                    delete_selection();
                    notify_changed();
                    mark_needs_paint();
                }
            }
            e.handled = true;
            return;
        }
        // Ctrl+V: 粘贴（只读态忽略；超出 max_length 部分截断）
        if (ctrl && e.key == static_cast<int>(KeyCode::V)) {
            if (m_read_only) {
                e.handled = true;
                return;
            }
            std::string clip = Clipboard::get_text();
            if (!clip.empty()) {
                if (m_sel_end != m_no_sel) {
                    delete_selection();
                }
                if (m_max_length > 0) {
                    const size_t room =
                        m_max_length > cp_count(m_value.get()) ? m_max_length - cp_count(m_value.get()) : 0;
                    clip = cp_slice(clip, 0, room); // 限长：仅插入剩余额度内的码点
                }

                std::string pv = m_value.get();
                size_t pi = 0;
                size_t pcp = 0;
                while (pi < pv.size() && pcp < m_caret) {
                    pi += cp_len(static_cast<unsigned char>(pv[pi]));
                    ++pcp;
                }
                pv.insert(pi, clip);
                m_value = pv;
                m_caret += cp_count(clip);
                m_sel_start = m_caret;
                m_sel_end = m_no_sel;
                notify_changed();
                mark_needs_paint();
            }
            e.handled = true;
            return;
        }
        // Enter: 提交回调（对标 Qt returnPressed / Flutter onSubmitted）
        if (e.key == static_cast<int>(KeyCode::Enter)) {
            if (m_on_submit) {
                m_on_submit(m_value.get());
            }
            e.handled = true;
            return;
        }

        int dir = 0;
        if (e.key == static_cast<int>(KeyCode::ArrowLeft)) {
            dir = -1;
        } else if (e.key == static_cast<int>(KeyCode::ArrowRight)) {
            dir = 1;
        }
        if (dir != 0) {
            if (shift) {
                // 含头含尾：以当前 caret 作为含入锚点，按方向扩展选区。
                if (m_sel_end == m_no_sel) {
                    m_sel_start = m_caret;
                    m_sel_end = m_caret; // 先建立 1-char 选区锚点
                }
                const auto nc = static_cast<long long>(m_caret) + dir;
                m_caret = static_cast<size_t>(std::clamp(nc, 0LL, static_cast<long long>(n)));
                if (dir > 0) {
                    m_sel_end = (m_caret == 0) ? m_no_sel : m_caret - 1; // 含尾最后字符 = caret-1
                } else {
                    m_sel_end = m_caret; // 含尾 = 新 caret 处的字符
                }
            } else {
                const auto nc = static_cast<long long>(m_caret) + dir;
                m_caret = static_cast<size_t>(std::clamp(nc, 0LL, static_cast<long long>(n)));
                m_sel_start = m_caret;
                m_sel_end = m_no_sel; // 无选区
            }
            mark_needs_paint();
            e.handled = true;
            return;
        }

        if (e.key == static_cast<int>(KeyCode::Backspace)) {
            if (m_read_only) {
                e.handled = true;
                return;
            }
            if (m_sel_end != m_no_sel) {
                delete_selection();
            } else {
                delete_before_caret();
            }
            notify_changed();
            mark_needs_paint();
            e.handled = true;
        }
    }

    auto on_text_input(TextInputEvent &e) -> void override {
        if (!m_enabled || !is_focused()) {
            return;
        }
        if (m_read_only) {
            e.handled = true; // 只读态吞掉输入不落字
            return;
        }
        if (m_sel_end != m_no_sel) {
            delete_selection(); // 选区替换
        }
        std::string ins = e.text;
        if (m_max_length > 0) {
            const size_t room = m_max_length > cp_count(m_value.get()) ? m_max_length - cp_count(m_value.get()) : 0;
            ins = cp_slice(ins, 0, room); // 限长：仅插入剩余额度内的码点
            if (ins.empty()) {
                e.handled = true;
                return;
            }
        }
        std::string v = m_value.get();
        size_t i = 0;
        size_t cp = 0;
        while (i < v.size() && cp < m_caret) {
            i += cp_len(static_cast<unsigned char>(v[i]));
            ++cp;
        }
        v.insert(i, ins);
        m_value = v;
        m_caret += cp_count(ins);
        m_sel_start = m_caret;
        m_sel_end = m_no_sel;
        notify_changed();
        mark_needs_paint();
        e.handled = true;
    }

    /// @brief 当前文本值（只读，供测试 / 外部读取）。
    [[nodiscard]] auto value() const -> std::string { return m_value.get(); }

    /// @brief 是否有活动选区（无选区时 m_sel_end == m_no_sel）。
    [[nodiscard]] auto has_selection() const -> bool { return m_sel_end != m_no_sel; }

    /// @brief 当前选中的文本（含头含尾：返回 [min, max] 区间内的全部字符）。无选区返回空。
    [[nodiscard]] auto selected_text() const -> std::string {
        if (!has_selection()) {
            return {};
        }
        const size_t a = std::min(m_sel_start, m_sel_end);
        const size_t b = std::max(m_sel_start, m_sel_end);
        return cp_slice(m_value.get(), a, b - a + 1); // 含尾计数
    }

  protected:
    // ---- 继承扩展点 ----

    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        const float fs = m_font_size > 0.0f ? m_font_size : 14.0f;
        const Font f{ .size_pt = fs };
        const std::string shown = m_value.get().empty() ? m_placeholder : display_value();
        const float tw = render::FontEngine::measure_width(shown, f) + m_padding.left + m_padding.right;
        const float h = render::FontEngine::measure_height(f) + m_padding.top + m_padding.bottom;
        const float w = (c.max.width != Size::infinity().width) ? c.max.width : tw;
        return c.constrain(Size{ .width = w, .height = h });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        const float fs = m_font_size > 0.0f ? m_font_size : 14.0f;
        const Font f{ .size_pt = fs };
        const bool disabled = !m_enabled;

        paint_frame(p, bounds, ctx);

        const bool empty = m_value.get().empty();
        const std::string shown = empty ? m_placeholder : display_value();
        const Color text_col = [&] { // NOLINT
            if (disabled) {
                return Color{ 150, 150, 152, 255 };
            }
            if (empty) {
                return m_placeholder_color;
            }
            return m_text_color;
        }();
        const float th = render::FontEngine::measure_height(f);
        const float tx = bounds.origin.x + m_padding.left;
        const float ty =
            bounds.origin.y + m_padding.top + ((bounds.size.height - m_padding.top - m_padding.bottom - th) * 0.5f);

        // 选区高亮（含头含尾模型）：无选区不画；端点字符（含行尾/行首）始终计入。
        if (!empty && m_sel_end != m_no_sel) {
            const size_t a = std::min(m_sel_start, m_sel_end);
            const size_t b = std::max(m_sel_start, m_sel_end);
            const float x0 = tx + render::FontEngine::caret_x(shown, a, f);
            const float x1 = tx + render::FontEngine::caret_x(shown, b + 1, f); // 含尾：+1
            p.fill_rect(Rect{ .origin = Point{ .x = x0, .y = ty }, .size = Size{ .width = x1 - x0, .height = th } },
                        m_selection_color);
        }

        p.draw_text(Rect{ .origin = Point{ .x = tx, .y = ty },
                          .size = Size{ .width = bounds.size.width - m_padding.left - m_padding.right, .height = th } },
                    shown, f, text_col);

        // 光标（仅非占位文本、聚焦且未禁用时显示）
        if (is_focused() && !empty && !disabled) {
            const float cx = tx + render::FontEngine::caret_x(shown, m_caret, f);
            p.fill_rect(Rect{ .origin = Point{ .x = cx, .y = ty }, .size = Size{ .width = 1.5f, .height = th } },
                        m_cursor_color);
        }
    }

    /// @brief 绘制背景 + 边框（聚焦/禁用态切换；聚焦边框缺省跟随主题 primary）。子类可覆盖。
    virtual auto paint_frame(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
        const bool disabled = !m_enabled;
        const Color bg = [&] { // NOLINT
            if (disabled) {
                return Color{ 225, 225, 225, 255 };
            }
            if (is_focused()) {
                return m_focused_background;
            }
            return m_background;
        }();
        Color border = disabled ? Color{ 180, 180, 180, 255 } : m_border_color;
        float bw = m_border_width;
        if (!disabled && is_focused()) {
            border = m_focused_border_color.value_or(inherit_theme(ctx).primary);
            bw = std::max(bw, 1.5f); // 聚焦态边框加粗（Fluent 式聚焦提示）
        }
        if (m_corner_radius > 0.0f) {
            p.fill_rounded_rect(bounds, m_corner_radius, bg);
            if (bw > 0.0f) {
                p.draw_rounded_border(bounds, m_corner_radius, bw, border);
            }
        } else {
            p.fill_rect(bounds, bg);
            if (bw > 0.0f) {
                p.draw_rect(bounds, border);
            }
        }
    }

    /// @brief 实显文本：掩码态返回等长 • 串（逐码点一一对应），否则返回原值。
    [[nodiscard]] auto display_value() const -> std::string {
        if (!m_obscure) {
            return m_value.get();
        }
        std::string out;
        const size_t n = cp_count(m_value.get());
        out.reserve(n * 3);
        for (size_t i = 0; i < n; ++i) {
            out += "\u2022";
        }
        return out;
    }

    /// @brief 编辑后统一上报（on_changed 回调）。
    auto notify_changed() const -> void {
        if (m_on_changed) {
            m_on_changed(m_value.get());
        }
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    Reactive<std::string> m_value;
    std::string m_placeholder;
    float m_font_size = 14.0f;
    float m_corner_radius = 0.0f; ///< 圆角半径（dp），>0 时背景与圆角裁剪
    EdgeInsets m_padding =
        EdgeInsets{ .left = 12.0f, .top = 12.0f, .right = 12.0f, .bottom = 12.0f }; ///< 文本与边框之间的内边距
    Color m_cursor_color = Color{ 0, 0, 0, 255 };                                   ///< 光标颜色
    bool m_enabled = true;                                    ///< 是否可编辑（禁用态降级绘制并忽略输入）
    Color m_text_color = Color{ 20, 20, 20, 255 };            ///< 文本颜色
    Color m_placeholder_color = Color{ 150, 150, 150, 255 };  ///< 占位提示颜色
    Color m_background = Color{ 240, 240, 240, 255 };         ///< 背景色
    Color m_focused_background = Color{ 245, 248, 255, 255 }; ///< 聚焦态背景色
    Color m_border_color = Color{ 120, 120, 120, 255 };       ///< 边框色
    std::optional<Color> m_focused_border_color;              ///< 聚焦态边框色；空 = 跟随主题 primary
    float m_border_width = 1.0f;                              ///< 边框线宽 dp；0 = 不描边
    Color m_selection_color = Color{ 80, 120, 220, 90 };      ///< 选区高亮色
    std::size_t m_max_length = 0;                             ///< 最大长度（码点）；0 = 不限
    bool m_read_only = false;                                 ///< 只读：可选择/复制不可编辑
    bool m_obscure = false;                                   ///< 密码掩码显示
    std::function<void(const std::string &)> m_on_changed;    ///< 每次编辑后触发
    std::function<void(const std::string &)> m_on_submit;     ///< Enter 提交触发

    // 文字选区状态（含头含尾模型，与 Text 一致：m_sel_start/m_sel_end 为「被选中字符的码点下标」；
    // 无选区时 m_sel_end == m_no_sel。m_caret 为编辑光标位置（caret 下标，介于字符之间）。UTF-8 安全）
    size_t m_sel_start = 0;
    size_t m_sel_end = m_no_sel;
    size_t m_caret = 0;
    bool m_selecting = false;
    static constexpr size_t m_no_sel = static_cast<size_t>(-1);
    // NOLINTEND(*-non-private-member-variables-in-classes)

    // UTF-8 码点原语已收口到 aurora::utf8_cp_*（见 core/utf8.h，dup-1）；此处委托，避免重复实现。
    static auto cp_len(unsigned char c) -> size_t { return static_cast<size_t>(utf8_cp_len(c)); }
    static auto cp_count(const std::string &s) -> size_t { return utf8_cp_count(s); }
    static auto cp_slice(const std::string &s, size_t start, size_t count) -> std::string {
        return utf8_cp_slice(s, start, count);
    }
    auto delete_selection() -> void {
        if (!has_selection()) {
            return;
        }
        const size_t a = std::min(m_sel_start, m_sel_end);
        const size_t b = std::max(m_sel_start, m_sel_end);
        std::string v = m_value.get();
        size_t i = 0;
        size_t cp = 0;
        while (i < v.size() && cp < a) {
            i += cp_len(static_cast<unsigned char>(v[i]));
            ++cp;
        }
        const size_t begin = i;
        while (i < v.size() && cp <= b) { // 含尾：删除 [a, b]
            i += cp_len(static_cast<unsigned char>(v[i]));
            ++cp;
        }
        v.erase(begin, i - begin);
        m_value = v;
        m_caret = a;
        m_sel_start = a;
        m_sel_end = m_no_sel; // 删除后无选区
    }
    auto delete_before_caret() -> void {
        if (m_caret == 0) {
            return;
        }
        std::string v = m_value.get();
        size_t i = 0;
        size_t cp = 0;
        while (i < v.size() && cp < m_caret) {
            i += cp_len(static_cast<unsigned char>(v[i]));
            ++cp;
        }
        const size_t end = i;
        size_t j = end;
        while (j > 0 && (static_cast<unsigned char>(v[j - 1]) & 0xC0u) == 0x80u) {
            --j;
        }
        const size_t start = (j > 0) ? j - 1u : 0u;
        v.erase(start, end - start);
        m_value = v;
        --m_caret;
        m_sel_start = m_caret;
        m_sel_end = m_no_sel; // 退格后无选区
    }
};

} // namespace aurora
