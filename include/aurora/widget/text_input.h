#pragma once

#include <algorithm>
#include <optional>
#include <string>

#include "aurora/app/clipboard.h"
#include "aurora/core/types.h"
#include "aurora/core/utf8.h"
#include "aurora/event/event.h"
#include "aurora/event/keycode.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/state/reactive.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief TextInput 属性（聚合）。
struct TextInputProps {
    std::string value;  ///< 初始文本
    std::string placeholder;  ///< 空值占位提示
    float font_size = 14.0F;  ///< 字号（点）
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
    explicit TextInput(const TextInputProps &props) : placeholder_(props.placeholder), font_size_(props.font_size) {
        value_ = props.value;
    }

    /// @brief 设置初始值（链式）。
    auto set_value(const std::string &v) -> TextInput & {
        value_ = v;
        return *this;
    }
    /// @brief 设置占位提示（链式）。
    auto set_placeholder(const std::string &p) -> TextInput & {
        placeholder_ = p;
        return *this;
    }
    /// @brief 设置字号（链式）。
    auto font_size(float s) -> TextInput & {
        font_size_ = s;
        return *this;
    }
    /// @brief 设置圆角半径（链式）。
    auto set_corner_radius(float r) -> TextInput & {
        corner_radius_ = r;
        return *this;
    }
    /// @brief 设置内边距（链式）。
    auto set_padding(EdgeInsets e) -> TextInput & {
        padding_ = e;
        return *this;
    }
    /// @brief 设置光标颜色（链式）。
    auto set_cursor_color(Color c) -> TextInput & {
        cursor_color_ = c;
        return *this;
    }
    /// @brief 设置是否启用（链式）；禁用态降级绘制并忽略输入。
    auto set_enabled(bool v) -> TextInput & {
        enabled_ = v;
        return *this;
    }

    /// @brief 设置文本颜色（链式）。
    auto set_text_color(Color c) -> TextInput & {
        text_color_ = c;
        return *this;
    }
    /// @brief 设置占位提示颜色（链式）。
    auto set_placeholder_color(Color c) -> TextInput & {
        placeholder_color_ = c;
        return *this;
    }
    /// @brief 设置背景色（链式）。
    auto set_background(Color c) -> TextInput & {
        background_ = c;
        return *this;
    }
    /// @brief 设置聚焦态背景色（链式）。
    auto set_focused_background(Color c) -> TextInput & {
        focused_background_ = c;
        return *this;
    }
    /// @brief 设置边框色（链式）。
    auto set_border_color(Color c) -> TextInput & {
        border_color_ = c;
        return *this;
    }
    /// @brief 设置聚焦态边框色（链式）。不调用则跟随主题 `Theme::primary`。
    auto set_focused_border_color(Color c) -> TextInput & {
        focused_border_color_ = c;
        return *this;
    }
    /// @brief 设置边框线宽 dp（链式；0 = 不描边）。
    auto set_border_width(float w) -> TextInput & {
        border_width_ = w >= 0.0F ? w : 1.0F;
        return *this;
    }
    /// @brief 设置选区高亮色（链式）。
    auto set_selection_color(Color c) -> TextInput & {
        selection_color_ = c;
        return *this;
    }
    /// @brief 设置最大长度（链式；码点计数，0 = 不限）。超出部分的输入/粘贴被截断。
    auto set_max_length(std::size_t n) -> TextInput & {
        max_length_ = n;
        return *this;
    }
    /// @brief 设置只读（链式）；只读态可选择/复制但不可编辑（对标 Qt QLineEdit::readOnly）。
    auto set_read_only(bool v) -> TextInput & {
        read_only_ = v;
        return *this;
    }
    [[nodiscard]] auto read_only() const -> bool { return read_only_; }
    /// @brief 设置密码掩码显示（链式）；显示为 •，值不变（对标 Flutter obscureText）。
    auto set_obscure_text(bool v) -> TextInput & {
        obscure_ = v;
        return *this;
    }
    /// @brief 设置变化回调（链式）；每次用户编辑（输入/退格/剪切/粘贴）后触发。
    auto set_on_changed(std::function<void(const std::string &)> cb) -> TextInput & {
        on_changed_ = std::move(cb);
        return *this;
    }
    /// @brief 设置提交回调（链式）；Enter 键触发。
    auto set_on_submit(std::function<void(const std::string &)> cb) -> TextInput & {
        on_submit_ = std::move(cb);
        return *this;
    }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&value_); }
    [[nodiscard]] auto type_name() const -> const char * override { return "TextInput"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "TextInput",
            .properties =
                {
                    {.name = "value",
                     .type = "string",
                     .default_value = "\"\"",
                     .required = false,
                     .note = "初始文本",
                     .json_type = "string"},
                    {.name = "placeholder",
                     .type = "string",
                     .default_value = "\"\"",
                     .required = false,
                     .note = "空值占位提示",
                     .json_type = "string"},
                    {.name = "font_size",
                     .type = "float",
                     .default_value = "14.0",
                     .required = false,
                     .note = "字号(pt)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "corner_radius",
                     .type = "float",
                     .default_value = "0.0",
                     .required = false,
                     .note = "圆角半径(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "padding",
                     .type = "EdgeInsets",
                     .default_value = "{12,12,12,12}",
                     .required = false,
                     .note = "内边距",
                     .json_type = "object"},
                    {.name = "cursor_color",
                     .type = "Color",
                     .default_value = "Color::black()",
                     .required = false,
                     .note = "光标颜色",
                     .json_type = "array"},
                    {.name = "enabled",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "是否可编辑",
                     .json_type = "boolean"},
                    {.name = "text_color",
                     .type = "Color",
                     .default_value = "{20,20,20,255}",
                     .required = false,
                     .note = "文本颜色",
                     .json_type = "array"},
                    {.name = "placeholder_color",
                     .type = "Color",
                     .default_value = "{150,150,150,255}",
                     .required = false,
                     .note = "占位提示颜色",
                     .json_type = "array"},
                    {.name = "background",
                     .type = "Color",
                     .default_value = "{240,240,240,255}",
                     .required = false,
                     .note = "背景色",
                     .json_type = "array"},
                    {.name = "focused_background",
                     .type = "Color",
                     .default_value = "{245,248,255,255}",
                     .required = false,
                     .note = "聚焦态背景色",
                     .json_type = "array"},
                    {.name = "border_color",
                     .type = "Color",
                     .default_value = "{120,120,120,255}",
                     .required = false,
                     .note = "边框色",
                     .json_type = "array"},
                    {.name = "focused_border_color",
                     .type = "Color",
                     .default_value = "theme.primary",
                     .required = false,
                     .note = "聚焦态边框色（缺省跟随主题 primary）",
                     .json_type = "array"},
                    {.name = "border_width",
                     .type = "float",
                     .default_value = "1.0",
                     .required = false,
                     .note = "边框线宽(dp)；0=不描边",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "selection_color",
                     .type = "Color",
                     .default_value = "{80,120,220,90}",
                     .required = false,
                     .note = "选区高亮色",
                     .json_type = "array"},
                    {.name = "max_length",
                     .type = "int",
                     .default_value = "0",
                     .required = false,
                     .note = "最大长度（码点）；0=不限",
                     .json_type = "integer",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "read_only",
                     .type = "bool",
                     .default_value = "false",
                     .required = false,
                     .note = "只读（可选择复制不可编辑）",
                     .json_type = "boolean"},
                    {.name = "obscure_text",
                     .type = "bool",
                     .default_value = "false",
                     .required = false,
                     .note = "密码掩码显示",
                     .json_type = "boolean"},
                    {.name = "width",
                     .type = "Length",
                     .default_value = "auto",
                     .required = false,
                     .note = "",
                     .json_type = "array"},
                    {.name = "height",
                     .type = "Length",
                     .default_value = "auto",
                     .required = false,
                     .note = "",
                     .json_type = "array"},
                    {.name = "show",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "",
                     .json_type = "boolean"},
                },
            .events = {"on_focus_change", "on_changed", "on_submit"},
            .children_policy = "none",
            .examples = {"au::TextInput().set_placeholder(\"Enter name\")"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["value"] = value_.get();
        props["placeholder"] = placeholder_;
        props["font_size"] = font_size_;
        props["corner_radius"] = corner_radius_;
        props["padding"] = edge_insets_to_json(padding_);
        props["cursor_color"] = color_to_json(cursor_color_);
        props["enabled"] = enabled_;
        props["text_color"] = color_to_json(text_color_);
        props["placeholder_color"] = color_to_json(placeholder_color_);
        props["background"] = color_to_json(background_);
        props["focused_background"] = color_to_json(focused_background_);
        props["border_color"] = color_to_json(border_color_);
        if (focused_border_color_.has_value()) {
            props["focused_border_color"] =
                color_to_json(*focused_border_color_);  // 未设置不输出：保留「跟随主题」语义
        }
        props["border_width"] = border_width_;
        props["selection_color"] = color_to_json(selection_color_);
        if (max_length_ > 0) {
            props["max_length"] = max_length_;
        }
        if (read_only_) {
            props["read_only"] = read_only_;
        }
        if (obscure_) {
            props["obscure_text"] = obscure_;
        }
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("value")) {
            value_ = props["value"].get<std::string>();
        }
        if (props.contains("placeholder")) {
            placeholder_ = props["placeholder"].get<std::string>();
        }
        if (props.contains("font_size")) {
            font_size_ = props["font_size"].get<float>();
        }
        if (props.contains("corner_radius")) {
            corner_radius_ = props["corner_radius"].get<float>();
        }
        if (props.contains("padding")) {
            padding_ = json_to_edge_insets(props["padding"]);
        }
        if (props.contains("cursor_color")) {
            cursor_color_ = json_to_color(props["cursor_color"]);
        }
        if (props.contains("enabled")) {
            enabled_ = props["enabled"].get<bool>();
        }
        if (props.contains("text_color")) {
            text_color_ = json_to_color(props["text_color"]);
        }
        if (props.contains("placeholder_color")) {
            placeholder_color_ = json_to_color(props["placeholder_color"]);
        }
        if (props.contains("background")) {
            background_ = json_to_color(props["background"]);
        }
        if (props.contains("focused_background")) {
            focused_background_ = json_to_color(props["focused_background"]);
        }
        if (props.contains("border_color")) {
            border_color_ = json_to_color(props["border_color"]);
        }
        if (props.contains("focused_border_color")) {
            focused_border_color_ = json_to_color(props["focused_border_color"]);
        }
        if (props.contains("border_width")) {
            border_width_ = props["border_width"].get<float>();
        }
        if (props.contains("selection_color")) {
            selection_color_ = json_to_color(props["selection_color"]);
        }
        if (props.contains("max_length")) {
            max_length_ = props["max_length"].get<std::size_t>();
        }
        if (props.contains("read_only")) {
            read_only_ = props["read_only"].get<bool>();
        }
        if (props.contains("obscure_text")) {
            obscure_ = props["obscure_text"].get<bool>();
        }
    }

    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!enabled_) {
            return;  // 禁用态不响应任何指针交互
        }
        // 保留基类行为：activate / 长按计时（命中即消费）。
        Widget::on_pointer_event(e);

        // 命中测试与实绘同源：掩码态下按掩码串定位（逐字符一一对应，码点下标一致）。
        const std::string v = display_value();
        const Font f{.size_pt = font_size_ > 0.0F ? font_size_ : 14.0F};
        const float lx = e.local_position.x - padding_.left;  // 文本左内边距
        if (e.action == MouseAction::Press) {
            // 含头含尾：选区端点用 hit_test_char_inclusive（点击字符任意位置均计入该字符），
            // m_caret 仍用 hit_test_char（caret 模型）作为编辑光标位置。
            const size_t ch = render::FontEngine::hit_test_char_inclusive(v, lx, f);
            caret_ = render::FontEngine::hit_test_char(v, lx, f);
            sel_start_ = ch;  // 锚点（含入字符）
            sel_end_ = AURORA_NO_SEL;  // 尚未形成选区，待拖拽
            selecting_ = true;
            request_focus();
            mark_needs_paint();
            e.is_handled = true;
        } else if (e.action == MouseAction::Move && selecting_) {
            const size_t ch = render::FontEngine::hit_test_char_inclusive(v, lx, f);
            caret_ = render::FontEngine::hit_test_char(v, lx, f);
            sel_end_ = ch;  // 拖拽终点（含入字符）：按下与松开所在字符均计入选区
            mark_needs_paint();
            e.is_handled = true;
        } else if (e.action == MouseAction::Release) {
            selecting_ = false;
            e.is_handled = true;
        }
    }

    auto on_key_event(KeyEvent &e) -> void override {  // NOLINT
        if (!enabled_ || !is_focused() || e.action != KeyAction::Down) {
            return;
        }
        const std::string &v = value_.get();
        const Font f{.size_pt = font_size_ > 0.0F ? font_size_ : 14.0F};
        const bool shift = (e.modifiers & ModifierKey::Shift) != 0u;  // NOLINT
        const bool ctrl = (e.modifiers & ModifierKey::Control) != 0u;  // NOLINT
        const size_t n = cp_count(v);

        if (ctrl && e.key == static_cast<int>(KeyCode::A)) {
            if (n > 0) {
                sel_start_ = 0;
                sel_end_ = n - 1;  // 含尾：最后一个字符下标
            } else {
                sel_end_ = AURORA_NO_SEL;
            }
            caret_ = n;
            mark_needs_paint();
            e.is_handled = true;
            return;
        }
        if (ctrl && e.key == static_cast<int>(KeyCode::C)) {
            const std::string t = selected_text();
            Clipboard::set_text(t.empty() ? v : t);
            e.is_handled = true;
            return;
        }
        // Ctrl+X: 剪切（只读态降级为仅复制）
        if (ctrl && e.key == static_cast<int>(KeyCode::X)) {
            const std::string t = selected_text();
            if (!t.empty()) {
                Clipboard::set_text(t);
                if (!read_only_) {
                    delete_selection();
                    notify_changed();
                    mark_needs_paint();
                }
            }
            e.is_handled = true;
            return;
        }
        // Ctrl+V: 粘贴（只读态忽略；超出 max_length 部分截断）
        if (ctrl && e.key == static_cast<int>(KeyCode::V)) {
            if (read_only_) {
                e.is_handled = true;
                return;
            }
            std::string clip = Clipboard::get_text();
            if (!clip.empty()) {
                if (sel_end_ != AURORA_NO_SEL) {
                    delete_selection();
                }
                if (max_length_ > 0) {
                    const size_t room = max_length_ > cp_count(value_.get()) ? max_length_ - cp_count(value_.get()) : 0;
                    clip = cp_slice(clip, 0, room);  // 限长：仅插入剩余额度内的码点
                }

                std::string pv = value_.get();
                size_t pi = 0;
                size_t pcp = 0;
                while (pi < pv.size() && pcp < caret_) {
                    pi += cp_len(static_cast<unsigned char>(pv[pi]));
                    ++pcp;
                }
                pv.insert(pi, clip);
                value_ = pv;
                caret_ += cp_count(clip);
                sel_start_ = caret_;
                sel_end_ = AURORA_NO_SEL;
                notify_changed();
                mark_needs_paint();
            }
            e.is_handled = true;
            return;
        }
        // Enter: 提交回调（对标 Qt returnPressed / Flutter onSubmitted）
        if (e.key == static_cast<int>(KeyCode::Enter)) {
            if (on_submit_) {
                on_submit_(value_.get());
            }
            e.is_handled = true;
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
                if (sel_end_ == AURORA_NO_SEL) {
                    sel_start_ = caret_;
                    sel_end_ = caret_;  // 先建立 1-char 选区锚点
                }
                const auto nc = static_cast<long long>(caret_) + dir;
                caret_ = static_cast<size_t>(std::clamp(nc, 0LL, static_cast<long long>(n)));
                if (dir > 0) {
                    sel_end_ = (caret_ == 0) ? AURORA_NO_SEL : caret_ - 1;  // 含尾最后字符 = caret-1
                } else {
                    sel_end_ = caret_;  // 含尾 = 新 caret 处的字符
                }
            } else {
                const auto nc = static_cast<long long>(caret_) + dir;
                caret_ = static_cast<size_t>(std::clamp(nc, 0LL, static_cast<long long>(n)));
                sel_start_ = caret_;
                sel_end_ = AURORA_NO_SEL;  // 无选区
            }
            mark_needs_paint();
            e.is_handled = true;
            return;
        }

        if (e.key == static_cast<int>(KeyCode::Backspace)) {
            if (read_only_) {
                e.is_handled = true;
                return;
            }
            if (sel_end_ != AURORA_NO_SEL) {
                delete_selection();
            } else {
                delete_before_caret();
            }
            notify_changed();
            mark_needs_paint();
            e.is_handled = true;
        }
    }

    auto on_text_input(TextInputEvent &e) -> void override {
        if (!enabled_ || !is_focused()) {
            return;
        }
        if (read_only_) {
            e.is_handled = true;  // 只读态吞掉输入不落字
            return;
        }
        if (sel_end_ != AURORA_NO_SEL) {
            delete_selection();  // 选区替换
        }
        std::string ins = e.text;
        if (max_length_ > 0) {
            const size_t room = max_length_ > cp_count(value_.get()) ? max_length_ - cp_count(value_.get()) : 0;
            ins = cp_slice(ins, 0, room);  // 限长：仅插入剩余额度内的码点
            if (ins.empty()) {
                e.is_handled = true;
                return;
            }
        }
        std::string v = value_.get();
        size_t i = 0;
        size_t cp = 0;
        while (i < v.size() && cp < caret_) {
            i += cp_len(static_cast<unsigned char>(v[i]));
            ++cp;
        }
        v.insert(i, ins);
        value_ = v;
        caret_ += cp_count(ins);
        sel_start_ = caret_;
        sel_end_ = AURORA_NO_SEL;
        notify_changed();
        mark_needs_paint();
        e.is_handled = true;
    }

    /// @brief 当前文本值（只读，供测试 / 外部读取）。
    [[nodiscard]] auto value() const -> std::string { return value_.get(); }

    /// @brief 是否有活动选区（无选区时 m_sel_end == AURORA_NO_SEL）。
    [[nodiscard]] auto has_selection() const -> bool { return sel_end_ != AURORA_NO_SEL; }

    /// @brief 当前选中的文本（含头含尾：返回 [min, max] 区间内的全部字符）。无选区返回空。
    [[nodiscard]] auto selected_text() const -> std::string {
        if (!has_selection()) {
            return {};
        }
        const size_t a = std::min(sel_start_, sel_end_);
        const size_t b = std::max(sel_start_, sel_end_);
        return cp_slice(value_.get(), a, b - a + 1);  // 含尾计数
    }

  protected:
    // ---- 继承扩展点 ----

    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        const float fs = font_size_ > 0.0F ? font_size_ : 14.0F;
        const Font f{.size_pt = fs};
        const std::string shown = value_.get().empty() ? placeholder_ : display_value();
        const float tw = render::FontEngine::measure_width(shown, f) + padding_.left + padding_.right;
        const float h = render::FontEngine::measure_height(f) + padding_.top + padding_.bottom;
        const float w = (c.max.width != Size::infinity().width) ? c.max.width : tw;
        return c.constrain(Size{.width = w, .height = h});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        const float fs = font_size_ > 0.0F ? font_size_ : 14.0F;
        const Font f{.size_pt = fs};
        const bool disabled = !enabled_;

        paint_frame(p, bounds, ctx);

        const bool empty = value_.get().empty();
        const std::string shown = empty ? placeholder_ : display_value();
        const Color text_col = [&] {  // NOLINT
            if (disabled) {
                return Color{150, 150, 152, 255};
            }
            if (empty) {
                return placeholder_color_;
            }
            return text_color_;
        }();
        const float th = render::FontEngine::measure_height(f);
        const float tx = bounds.origin.x + padding_.left;
        const float ty =
            bounds.origin.y + padding_.top + ((bounds.size.height - padding_.top - padding_.bottom - th) * 0.5F);

        // 选区高亮（含头含尾模型）：无选区不画；端点字符（含行尾/行首）始终计入。
        if (!empty && sel_end_ != AURORA_NO_SEL) {
            const size_t a = std::min(sel_start_, sel_end_);
            const size_t b = std::max(sel_start_, sel_end_);
            const float x0 = tx + render::FontEngine::caret_x(shown, a, f);
            const float x1 = tx + render::FontEngine::caret_x(shown, b + 1, f);  // 含尾：+1
            p.fill_rect(Rect{.origin = Point{.x = x0, .y = ty}, .size = Size{.width = x1 - x0, .height = th}},
                        selection_color_);
        }

        p.draw_text(Rect{.origin = Point{.x = tx, .y = ty},
                         .size = Size{.width = bounds.size.width - padding_.left - padding_.right, .height = th}},
                    shown, f, text_col);

        // 光标（仅非占位文本、聚焦且未禁用时显示）
        if (is_focused() && !empty && !disabled) {
            const float cx = tx + render::FontEngine::caret_x(shown, caret_, f);
            p.fill_rect(Rect{.origin = Point{.x = cx, .y = ty}, .size = Size{.width = 1.5F, .height = th}},
                        cursor_color_);
        }
    }

    /// @brief 绘制背景 + 边框（聚焦/禁用态切换；聚焦边框缺省跟随主题 primary）。子类可覆盖。
    virtual auto paint_frame(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
        const bool disabled = !enabled_;
        const Color bg = [&] {  // NOLINT
            if (disabled) {
                return Color{225, 225, 225, 255};
            }
            if (is_focused()) {
                return focused_background_;
            }
            return background_;
        }();
        Color border = disabled ? Color{180, 180, 180, 255} : border_color_;
        float bw = border_width_;
        if (!disabled && is_focused()) {
            border = focused_border_color_.value_or(inherit_theme(ctx).primary);
            bw = std::max(bw, 1.5F);  // 聚焦态边框加粗（Fluent 式聚焦提示）
        }
        if (corner_radius_ > 0.0F) {
            p.fill_rounded_rect(bounds, corner_radius_, bg);
            if (bw > 0.0F) {
                p.draw_rounded_border(bounds, corner_radius_, bw, border);
            }
        } else {
            p.fill_rect(bounds, bg);
            if (bw > 0.0F) {
                p.draw_rect(bounds, border);
            }
        }
    }

    /// @brief 实显文本：掩码态返回等长 • 串（逐码点一一对应），否则返回原值。
    [[nodiscard]] auto display_value() const -> std::string {
        if (!obscure_) {
            return value_.get();
        }
        std::string out;
        const size_t n = cp_count(value_.get());
        out.reserve(n * 3);
        for (size_t i = 0; i < n; ++i) {
            out += "\u2022";
        }
        return out;
    }

    /// @brief 编辑后统一上报（on_changed 回调）。
    auto notify_changed() const -> void {
        if (on_changed_) {
            on_changed_(value_.get());
        }
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    Reactive<std::string> value_;
    std::string placeholder_;
    float font_size_ = 14.0F;
    float corner_radius_ = 0.0F;  ///< 圆角半径（dp），>0 时背景与圆角裁剪
    EdgeInsets padding_ =
        EdgeInsets{.left = 12.0F, .top = 12.0F, .right = 12.0F, .bottom = 12.0F};  ///< 文本与边框之间的内边距
    Color cursor_color_ = Color{0, 0, 0, 255};  ///< 光标颜色
    bool enabled_ = true;  ///< 是否可编辑（禁用态降级绘制并忽略输入）
    Color text_color_ = Color{20, 20, 20, 255};  ///< 文本颜色
    Color placeholder_color_ = Color{150, 150, 150, 255};  ///< 占位提示颜色
    Color background_ = Color{240, 240, 240, 255};  ///< 背景色
    Color focused_background_ = Color{245, 248, 255, 255};  ///< 聚焦态背景色
    Color border_color_ = Color{120, 120, 120, 255};  ///< 边框色
    std::optional<Color> focused_border_color_;  ///< 聚焦态边框色；空 = 跟随主题 primary
    float border_width_ = 1.0F;  ///< 边框线宽 dp；0 = 不描边
    Color selection_color_ = Color{80, 120, 220, 90};  ///< 选区高亮色
    std::size_t max_length_ = 0;  ///< 最大长度（码点）；0 = 不限
    bool read_only_ = false;  ///< 只读：可选择/复制不可编辑
    bool obscure_ = false;  ///< 密码掩码显示
    std::function<void(const std::string &)> on_changed_;  ///< 每次编辑后触发
    std::function<void(const std::string &)> on_submit_;  ///< Enter 提交触发

    // 文字选区状态（含头含尾模型，与 Text 一致：sel_start_/sel_end_ 为「被选中字符的码点下标」；
    // 无选区时 sel_end_ == AURORA_NO_SEL。m_caret 为编辑光标位置（caret 下标，介于字符之间）。UTF-8 安全）
    size_t sel_start_ = 0;
    size_t sel_end_ = AURORA_NO_SEL;
    size_t caret_ = 0;
    bool selecting_ = false;
    static constexpr size_t AURORA_NO_SEL = static_cast<size_t>(-1);
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
        const size_t a = std::min(sel_start_, sel_end_);
        const size_t b = std::max(sel_start_, sel_end_);
        std::string v = value_.get();
        size_t i = 0;
        size_t cp = 0;
        while (i < v.size() && cp < a) {
            i += cp_len(static_cast<unsigned char>(v[i]));
            ++cp;
        }
        const size_t begin = i;
        while (i < v.size() && cp <= b) {  // 含尾：删除 [a, b]
            i += cp_len(static_cast<unsigned char>(v[i]));
            ++cp;
        }
        v.erase(begin, i - begin);
        value_ = v;
        caret_ = a;
        sel_start_ = a;
        sel_end_ = AURORA_NO_SEL;  // 删除后无选区
    }
    auto delete_before_caret() -> void {
        if (caret_ == 0) {
            return;
        }
        std::string v = value_.get();
        size_t i = 0;
        size_t cp = 0;
        while (i < v.size() && cp < caret_) {
            i += cp_len(static_cast<unsigned char>(v[i]));
            ++cp;
        }
        const size_t end = i;
        size_t j = end;
        while (j > 0 && (static_cast<unsigned char>(v[j - 1]) & 0xC0U) == 0x80U) {
            --j;
        }
        const size_t start = (j > 0) ? j - 1U : 0U;
        v.erase(start, end - start);
        value_ = v;
        --caret_;
        sel_start_ = caret_;
        sel_end_ = AURORA_NO_SEL;  // 退格后无选区
    }
};

}  // namespace aurora
