#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/accessibility.h"
#include "aurora/core/color.h"
#include "aurora/core/enums.h"
#include "aurora/core/font.h"
#include "aurora/core/types.h"
#include "aurora/core/utf8.h"
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
        out.reserve(doc_.size());
        for (const auto &sc : doc_) {
            out.push_back(sc.ch);
        }
        return out;
    }

    /// @brief 导出为 TextSpan 序列（连续同样式字符合并）。
    [[nodiscard]] auto to_spans() const -> std::vector<TextSpan> {
        std::vector<TextSpan> out;
        for (const auto &sc : doc_) {
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
        doc_.clear();
        for (const auto &s : spans) {
            for (const char ch : s.text.text) {
                doc_.push_back(StyledChar{.ch = ch, .font = s.font, .color = s.color, .underline = false});
            }
        }
        caret_ = 0;
        sel_start_ = sel_end_ = 0;
        mark_needs_paint();
    }

    // ---- 当前输入样式 ----

    [[nodiscard]] auto current_font() const -> Font { return cur_font_; }
    auto set_current_font(Font f) -> RichTextEdit & {
        cur_font_ = std::move(f);
        return *this;
    }
    [[nodiscard]] auto current_color() const -> Color { return cur_color_; }
    auto set_current_color(Color c) -> RichTextEdit & {
        cur_color_ = c;
        return *this;
    }
    [[nodiscard]] auto current_underline() const -> bool { return cur_underline_; }
    auto set_current_underline(bool v) -> RichTextEdit & {
        cur_underline_ = v;
        return *this;
    }

    /// @brief 设置书写方向（链式）。nullopt = 继承环境（`Directionality` 注入/进程级），
    ///        与 Text/TextInput 语义一致。RTL 时光标/选区/命中走逻辑↔视觉镜像（逻辑首字符在右缘）、
    ///        段落整体右对齐。
    auto set_direction(std::optional<TextDirection> d) -> RichTextEdit & {
        direction_ = d;
        return *this;
    }
    [[nodiscard]] auto direction() const -> std::optional<TextDirection> { return direction_; }

    /// @brief 切换当前字体粗体（weight 400 ↔ 700）。
    auto toggle_bold() -> void { cur_font_.weight = (cur_font_.weight >= 700) ? 400 : 700; }
    /// @brief 切换当前字体斜体（通过 family 后缀 "*" 模拟，实际渲染依赖 FontEngine）。
    auto toggle_italic() -> void {
        // 简化：用 family 尾部标记；真实场景需 Font::italic 字段。
        if (cur_font_.family.size() >= 2 && cur_font_.family.back() == 'I' &&
            cur_font_.family[cur_font_.family.size() - 2] == '/') {
            cur_font_.family = cur_font_.family.substr(0, cur_font_.family.size() - 2);
        } else {
            cur_font_.family += "/I";
        }
    }
    /// @brief 切换当前下划线。
    auto toggle_underline() -> void { cur_underline_ = !cur_underline_; }

    // ---- UndoStack 集成 ----

    auto set_undo_stack(UndoStack *stack) -> RichTextEdit & {
        undo_ = stack;
        return *this;
    }
    [[nodiscard]] auto undo_stack() const -> UndoStack * { return undo_; }

    // ---- 光标 / 选区 ----

    [[nodiscard]] auto caret() const -> std::size_t { return caret_; }
    [[nodiscard]] auto selection_start() const -> std::size_t { return sel_start_; }
    [[nodiscard]] auto selection_end() const -> std::size_t { return sel_end_; }
    [[nodiscard]] auto has_selection() const -> bool { return sel_start_ != sel_end_; }

    [[nodiscard]] auto selected_text() const -> std::string {
        if (!has_selection()) {
            return {};
        }
        const auto a = std::min(sel_start_, sel_end_);
        const auto b = std::max(sel_start_, sel_end_);
        std::string out;
        for (auto i = a; i < b && i < doc_.size(); ++i) {
            out.push_back(doc_[i].ch);
        }
        return out;
    }

    // ---- Widget 接口 ----

    [[nodiscard]] auto type_name() const -> const char * override { return "RichTextEdit"; }

    /// @brief 无障碍值：文档纯文本，组合期间含光标处的 preedit（读屏应播报未上屏内容）。
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_value() const -> std::string override {
        std::string out = plain_text();
        if (is_composing()) {
            out.insert(std::min(caret_, out.size()), preedit_);
        }
        return out;
    }

    // ---- 无障碍语义（切片 1/2）：多行文本 + TextPattern 支撑 ----

    /// @brief 无障碍状态：多行文本（UIA 侧映射 `Document` 而非 `Edit`）。
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_state() const -> AccessibilityState override {
        AccessibilityState s = Widget::accessibility_state();
        s.multiline = true;
        return s;
    }

    /// @brief 无障碍文本全文（UTF-8 字节口径，与 `doc_` 的 char 序列一一对应）。
    ///
    /// @note 文档以 `StyledChar` 序列存储、`plain_text()` 按值拼接，无法给出持久视图，故经
    ///       `mutable` 缓存中转：**调用方须在本控件下一次文本操作前消费该视图**（桥侧立即复制）。
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_text() const -> std::string_view override {
        a11y_text_cache_ = plain_text();
        return a11y_text_cache_;
    }

    /// @brief 无障碍选区：内部 `sel_start_/sel_end_` 即 UTF-8 字节偏移（半开区间）。
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_selection() const -> std::optional<AccessibilityTextSelection> override {
        return AccessibilityTextSelection{.start = std::min(sel_start_, sel_end_),
                                          .end = std::max(sel_start_, sel_end_)};
    }

    /// @brief 设置选区（UTF-8 字节半开区间），光标置于选区终点。
    /// @note Side-effects: mutates selection state
    auto accessibility_set_selection(std::size_t start, std::size_t end) -> void override {
        const std::string t = plain_text();
        const std::size_t a = std::min(start, t.size());
        const std::size_t b = std::min(std::max(end, a), t.size());
        sel_start_ = a;
        sel_end_ = b;
        caret_ = b;
        mark_needs_paint();
    }

    /// @brief 替换文本（UTF-8 字节半开区间）：按纯文本重建文档（样式以当前输入样式统一）。
    ///
    /// @note 富文本样式在替换区间内退化为当前样式——这是「读屏改写文本」语义下的可接受代价
    ///       （三桥的文本编辑通道都只传纯文本，无样式信息）。
    /// @note Side-effects: mutates document
    auto accessibility_replace_text(std::size_t start, std::size_t end, std::string_view utf8) -> void override {
        std::string next = plain_text();
        const std::size_t a = std::min(start, next.size());
        const std::size_t b = std::clamp(end, a, next.size());
        std::string rebuilt = next.substr(0, a);
        rebuilt.append(utf8);
        rebuilt.append(next, b, std::string::npos);
        TextSpan span;
        span.text.text = std::move(rebuilt);
        span.font = cur_font_;
        span.color = cur_color_;
        load_spans({span});
        caret_ = std::min(a + utf8.size(), plain_text().size());
        sel_start_ = caret_;
        sel_end_ = caret_;
        mark_needs_paint();
    }

    /// @brief 读屏 Value 动作：整值替换。
    /// @note Side-effects: mutates document
    auto perform_accessibility_action(const AccessibilityActionRequest &req) -> bool override {
        if (req.action == AccessibilityAction::Value) {
            accessibility_replace_text(0, plain_text().size(), req.text);
            return true;
        }
        return Widget::perform_accessibility_action(req);
    }

    /// @brief 悬停默认文本光标：文本编辑区悬停 IBeam；修饰链显式 `cursor(...)` 声明优先。
    /// @note Side-effects: pure
    [[nodiscard]] auto cursor_shape() const -> std::optional<CursorShape> override { return CursorShape::IBeam; }

    /// @brief Enter 优先经 `on_key_event` 投递（换行插入依赖它，而非激活语义）。
    /// @note Side-effects: pure
    [[nodiscard]] auto wants_activation_keys() const -> bool override { return true; }

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
            e.is_handled = true;
        }
    }

    /// @brief IME 组合输入（CJK 攻坚）：先落 `committed` 上屏（走 `do_insert`，带 undo），
    ///        再更新 preedit 显示态。preedit 不进 `doc_`（未上屏文本不参与撤销栈 / 序列化），
    ///        仅在绘制期插到光标处（见 `paint_preedit`）。
    ///
    /// 典型序列（拼音输入法）：`preedit="nihao"` → `preedit="你好",cursor_index=2` →
    /// `preedit="",committed="你好"`（落字）。
    auto on_text_composition(TextCompositionEvent &e) -> void override {
        e.is_handled = true;  // 无论是否落地都吞掉，避免平台侧因「无人处理」重复上屏
        if (!e.committed.empty()) {
            do_insert(e.committed);
        }
        preedit_ = e.preedit;
        const std::size_t pn = utf8_cp_count(preedit_);
        preedit_cursor_ = std::min(e.cursor_index, pn);
        preedit_sel_start_ = std::min(e.sel_start, pn);
        preedit_sel_end_ = e.has_preedit_selection() ? std::min(e.sel_end, pn) : AURORA_NO_PREEDIT_SELECTION;
        if (preedit_sel_end_ != AURORA_NO_PREEDIT_SELECTION && preedit_sel_end_ < preedit_sel_start_) {
            preedit_sel_end_ = preedit_sel_start_;  // 端点倒置退化为单点选区
        }
        mark_needs_paint();
    }

    /// @brief 当前预编辑串（组合中的未上屏文本）；无组合时为空串。
    [[nodiscard]] auto preedit() const -> std::string { return preedit_; }

    /// @brief 是否处于组合态（preedit 非空）。
    [[nodiscard]] auto is_composing() const -> bool { return !preedit_.empty(); }

    /// @brief 组合光标在 preedit 内的码点下标（候选插入点）。
    [[nodiscard]] auto composition_cursor() const -> std::size_t { return preedit_cursor_; }

    /// @brief 焦点变更：失焦即取消未上屏的组合（平台 IME 惯例）。
    auto on_focus_change(bool focused) -> void override {
        Widget::on_focus_change(focused);
        if (!focused) {
            cancel_composition();
        }
    }

    // ---- 序列化 ----

    auto serialize_props(Json &props) const -> void override;

    auto deserialize_props(const Json &props) -> void override;

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;

    /// @brief 生效书写方向：布局期解析并缓存。nullopt = 无显式来源，shaping 保持按内容 guess
    ///        （默认行为与接入前一致，golden 逐位不变）；进程级默认 LTR 不强制覆盖 guess。
    std::optional<TextDirection> cached_direction_;

    /// @brief 显式书写方向：nullopt = 继承环境（`Directionality` 注入/进程级）。
    std::optional<TextDirection> direction_;

  private:
    // ---- 内部行结构（布局用）----
    struct Line {
        std::vector<StyledChar> chars;
    };

    auto relayout_lines(float max_width) -> void;

    auto pos_from_line_col(size_t line_idx, size_t col) const -> size_t;

    /// @brief 单行「生效方向 + 各样式/层级 run」的视觉布局（RTL + 完整 UBA）：连续同样式
    ///        字符合并为样式 run，再逐 run 按完整 UBA（UAX #9）嵌入层级细分为层级 run（层级
    ///        单一，hb 方向取层级奇偶）；跨 run 顺序按 `uba_visual_order`（L2 层叠反转）重排；
    ///        RTL 段落整体右对齐。返回每个 run 的视觉起点 x、宽度、行内下标区间与整形方向。
    struct RunLayout {
        std::string text;
        Font font = {};
        Color color = Color::black();
        std::vector<char> underline;  ///< 与 text 逐字对应的下划线标记（0/1）
        float x = 0.0F;
        float w = 0.0F;
        std::size_t begin = 0;  ///< 该 run 首字符在行内的逻辑下标（含 '\n' 计 1）
        std::size_t end = 0;  ///< 该 run 末字符逻辑下标 +1
        TextDirection dir = TextDirection::LTR;  ///< 该 run 的整形方向（UBA 层级奇偶）
    };

    auto compute_line_runs(const Line &line, TextDirection base, const Rect &bounds) const -> std::vector<RunLayout>;

    /// @brief 计算给定行内、逻辑下标 `caret_local` 处光标的视觉 x（相对 bounds 左缘）。
    ///        RTL 下 `FontEngine::caret_x` 返回逻辑 0 在右缘的视觉偏移，调用方无需再镜像。
    auto caret_visual_x(const Line &line, TextDirection base, std::size_t caret_local, const Rect &bounds) const
        -> float;

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

    /// @brief 组合态绘制：preedit 内选区高亮 + preedit 文本 + 下划线 + 候选插入点光标。
    ///        在 `(x, y)` 处绘制，返回占用宽度（供调用方推进游标，实现「预编辑串挤开后续文本」）。
    auto paint_preedit(Painter &p, float x, float y) const -> float;

    /// @brief 取消组合：清空 preedit 与其选区（失焦 / 平台侧取消时调用）。
    auto cancel_composition() -> void;

    // ---- 数据成员 ----
    std::vector<StyledChar> doc_;
    std::size_t caret_ = 0;
    std::size_t sel_start_ = 0;
    std::size_t sel_end_ = 0;
    /// @brief `accessibility_text()` 的视图中转缓存（见该方法注释：调用方须即时消费）。
    mutable std::string a11y_text_cache_;
    Font cur_font_{};
    Color cur_color_ = Color::black();
    bool cur_underline_ = false;
    UndoStack *undo_ = nullptr;
    float line_height_ = 20.0F;
    std::vector<Line> lines_;

    // IME 组合态：preedit 不进 doc_，仅在绘制期插到 caret_ 处。
    /// @brief preedit 内「无选区」哨兵。
    static constexpr std::size_t AURORA_NO_PREEDIT_SELECTION = TextCompositionEvent::AURORA_NO_SELECTION;
    std::string preedit_;  ///< 预编辑串（UTF-8）；空 = 无组合
    std::size_t preedit_cursor_ = 0;  ///< 组合光标在 preedit 内的码点下标
    std::size_t preedit_sel_start_ = 0;  ///< preedit 内选区起点（码点下标）
    std::size_t preedit_sel_end_ = AURORA_NO_PREEDIT_SELECTION;  ///< 选区终点（含尾）；AURORA_NO_PREEDIT_SELECTION = 无
};

}  // namespace aurora
