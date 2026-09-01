#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/state/undo_stack.h"
#include "aurora/widget/text_span.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 带样式的单个字符（RichTextEdit 文档模型基本单元）。
struct StyledChar {
    char ch = ' ';
    Font font{};
    Color color = Color::black();
    bool underline = false;
};

/**
 * @brief 可编辑富文本控件。
 *
 * 文档模型为 `vector<StyledChar>`，支持：
 * - 加粗 / 斜体 / 下划线切换（Ctrl+B / Ctrl+I / Ctrl+U）
 * - 字号 / 颜色设置
 * - 撤销 / 重做（Ctrl+Z / Ctrl+Y），集成 UndoStack
 * - 光标定位、选区操作（Shift+方向键、Ctrl+A、Home/End）
 * - 剪贴板（Ctrl+C/X/V）
 * - 序列化：导出为 TextSpan 序列（连续相同样式的字符合并为一个 span）
 *
 * 对标 Qt `QTextEdit`、WPF `RichTextBox`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class RichTextEdit : public LeafWidget {
  public:
    RichTextEdit() = default;

    // ---- 文档访问 ----

    /// @brief 纯文本内容（所有字符拼接）。
    [[nodiscard]] auto plain_text() const -> std::string {
        std::string out;
        out.reserve(m_doc.size());
        for (const auto &sc : m_doc) {
            out.push_back(sc.ch);
        }
        return out;
    }

    /// @brief 导出为 TextSpan 序列（连续同样式字符合并）。
    [[nodiscard]] auto to_spans() const -> std::vector<TextSpan> {
        std::vector<TextSpan> out;
        for (const auto &sc : m_doc) {
            if (!out.empty() && out.back().font == sc.font && out.back().color == sc.color) {
                out.back().text.text.push_back(sc.ch);
            } else {
                TextSpan span;
                span.text.text = std::string(1, sc.ch);
                span.font = sc.font;
                span.color = sc.color;
                out.push_back(std::move(span));
            }
        }
        return out;
    }

    /// @brief 从 TextSpan 序列加载文档。
    auto load_spans(const std::vector<TextSpan> &spans) -> void {
        m_doc.clear();
        for (const auto &s : spans) {
            for (const char ch : s.text.text) {
                m_doc.push_back(StyledChar{ .ch = ch, .font = s.font, .color = s.color, .underline = false });
            }
        }
        m_caret = 0;
        m_sel_start = m_sel_end = 0;
        mark_needs_paint();
    }

    // ---- 当前输入样式 ----

    [[nodiscard]] auto current_font() const -> Font { return m_cur_font; }
    auto set_current_font(Font f) -> RichTextEdit & {
        m_cur_font = std::move(f);
        return *this;
    }
    [[nodiscard]] auto current_color() const -> Color { return m_cur_color; }
    auto set_current_color(Color c) -> RichTextEdit & {
        m_cur_color = c;
        return *this;
    }
    [[nodiscard]] auto current_underline() const -> bool { return m_cur_underline; }
    auto set_current_underline(bool v) -> RichTextEdit & {
        m_cur_underline = v;
        return *this;
    }

    /// @brief 切换当前字体粗体（weight 400 ↔ 700）。
    auto toggle_bold() -> void { m_cur_font.weight = (m_cur_font.weight >= 700) ? 400 : 700; }
    /// @brief 切换当前字体斜体（通过 family 后缀 "*" 模拟，实际渲染依赖 FontEngine）。
    auto toggle_italic() -> void {
        // 简化：用 family 尾部标记；真实场景需 Font::italic 字段。
        if (m_cur_font.family.size() >= 2 && m_cur_font.family.back() == 'I' &&
            m_cur_font.family[m_cur_font.family.size() - 2] == '/') {
            m_cur_font.family = m_cur_font.family.substr(0, m_cur_font.family.size() - 2);
        } else {
            m_cur_font.family += "/I";
        }
    }
    /// @brief 切换当前下划线。
    auto toggle_underline() -> void { m_cur_underline = !m_cur_underline; }

    // ---- UndoStack 集成 ----

    auto set_undo_stack(UndoStack *stack) -> RichTextEdit & {
        m_undo = stack;
        return *this;
    }
    [[nodiscard]] auto undo_stack() const -> UndoStack * { return m_undo; }

    // ---- 光标 / 选区 ----

    [[nodiscard]] auto caret() const -> std::size_t { return m_caret; }
    [[nodiscard]] auto selection_start() const -> std::size_t { return m_sel_start; }
    [[nodiscard]] auto selection_end() const -> std::size_t { return m_sel_end; }
    [[nodiscard]] auto has_selection() const -> bool { return m_sel_start != m_sel_end; }

    [[nodiscard]] auto selected_text() const -> std::string {
        if (!has_selection()) {
            return {};
        }
        const auto a = std::min(m_sel_start, m_sel_end);
        const auto b = std::max(m_sel_start, m_sel_end);
        std::string out;
        for (auto i = a; i < b && i < m_doc.size(); ++i) {
            out.push_back(m_doc[i].ch);
        }
        return out;
    }

    // ---- Widget 接口 ----

    [[nodiscard]] auto type_name() const -> const char * override { return "RichTextEdit"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;

    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    // ---- 事件处理 ----

    auto on_pointer_event(MouseEvent &e) -> void override;

    auto on_key_event(KeyEvent &e) -> void override;

    auto on_text_input(TextInputEvent &e) -> void override {
        if (!e.text.empty()) {
            do_insert(e.text);
            mark_needs_paint();
            e.handled = true;
        }
    }

    // ---- 序列化 ----

    auto serialize_props(Json &props) const -> void override;

    auto deserialize_props(const Json &props) -> void override;

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override;

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override;

  private:
    // ---- 内部行结构（布局用）----
    struct Line {
        std::vector<StyledChar> chars;
    };

    auto relayout_lines(float max_width) -> void;

    auto pos_from_line_col(size_t line_idx, size_t col) const -> size_t;

    // ---- 编辑操作（带 UndoStack 集成）----

    struct UndoSnapshot {
        std::string description;
        std::vector<StyledChar> doc;
        std::size_t caret;
    };

    auto push_undo(const UndoSnapshot &snap) -> void;

    auto do_insert(const std::string &text) -> void;

    auto do_delete_selection() -> void;

    auto do_delete_selection_no_undo() -> void;

    /// @brief 处理 Ctrl/Cmd 组合快捷键（撤销/重做/样式/全选/剪贴板）。返回是否命中并处理。
    auto handle_control_shortcut(KeyEvent &e, bool shift, std::size_t n) -> bool;

    /// @brief 处理方向键 / Home / End（含 Shift 扩展选区）。返回是否命中。
    auto handle_move_key(KeyEvent &e, bool shift, std::size_t n) -> bool;

    /// @brief 处理 Backspace / Delete / Enter 编辑键。返回是否命中。
    auto handle_text_key(KeyEvent &e, std::size_t n) -> bool;

    // ---- 绘制辅助 ----

    auto paint_selection_highlight(Painter &p, const Rect &bounds) const -> void;

    auto paint_cursor(Painter &p, const Rect &bounds) const -> void;

    // ---- 数据成员 ----
    std::vector<StyledChar> m_doc;
    std::size_t m_caret = 0;
    std::size_t m_sel_start = 0;
    std::size_t m_sel_end = 0;
    Font m_cur_font{};
    Color m_cur_color = Color::black();
    bool m_cur_underline = false;
    UndoStack *m_undo = nullptr;
    float m_line_height = 20.0f;
    std::vector<Line> m_lines;
};

} // namespace aurora
