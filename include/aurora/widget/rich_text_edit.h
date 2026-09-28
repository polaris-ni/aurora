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
    char ch = ' ';  ///< 字符（按 char 存储：UTF-8 多字节序列逐字节展开为多个 StyledChar，共享同一样式）。
    Font font{};  ///< 该字符字体（`to_spans` 合并与行内 run 切分均以 font 相等为键）。
    Color color = Color::black();  ///< 该字符颜色（同为合并/切分键）。
    bool underline = false;  ///< 下划线开关（逐字绘制；不参与 span/run 的相等判据）。
};

/// @brief 可编辑富文本控件。
///
/// 文档模型为 `vector<StyledChar>`，支持：
/// - 加粗 / 斜体 / 下划线切换（Ctrl+B / Ctrl+I / Ctrl+U）
/// - 字号 / 颜色设置
/// - 撤销 / 重做（Ctrl+Z / Ctrl+Y），集成 UndoStack
/// - 光标定位、选区操作（Shift+方向键、Ctrl+A、Home/End）
/// - 剪贴板（Ctrl+C/X/V）
/// - 序列化：导出为 TextSpan 序列（连续相同样式的字符合并为一个 span）
///
/// 对标 Qt `QTextEdit`、WPF `RichTextBox`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class RichTextEdit : public LeafWidget {
  public:
    /// @brief 默认构造：空文档、光标与选区收敛于 0，输入样式取默认字体 / 黑色 / 无下划线。
    RichTextEdit() = default;

    // ---- 文档访问 ----

    /// @brief 纯文本内容（所有字符拼接）。
    /// @return 文档各字符按序拼接的字符串（空文档为空串）。
    [[nodiscard]] auto plain_text() const -> std::string {
        std::string out;
        out.reserve(doc_.size());
        for (const auto &sc : doc_) {
            out.push_back(sc.ch);
        }
        return out;
    }

    /// @brief 导出为 TextSpan 序列（连续同样式字符合并）。
    /// @return 相邻同 font 同 color 字符合并后的 span 序列（判据不含 underline；空文档空序列）。
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
    /// @param spans 源 span 序列：先清空现文档，逐字符展开为 StyledChar（underline 一律重置为
    ///        false），完成后光标与选区复位到 0。
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

    /// @brief 当前输入字体（后续插入字符采用的样式）。
    /// @return 当前输入字体值。
    [[nodiscard]] auto current_font() const -> Font { return cur_font_; }
    /// @brief 设置当前输入字体（链式）。
    /// @param f 新输入字体。
    /// @return 本控件引用（`*this`），支持链式调用。
    auto set_current_font(Font f) -> RichTextEdit & {
        cur_font_ = std::move(f);
        return *this;
    }
    /// @brief 当前输入颜色。
    /// @return 当前输入颜色（缺省黑色）。
    [[nodiscard]] auto current_color() const -> Color { return cur_color_; }
    /// @brief 设置当前输入颜色（链式）。
    /// @param c 新输入颜色。
    /// @return 本控件引用（`*this`），支持链式调用。
    auto set_current_color(Color c) -> RichTextEdit & {
        cur_color_ = c;
        return *this;
    }
    /// @brief 当前输入下划线开关。
    /// @return true = 新插入字符带下划线。
    [[nodiscard]] auto current_underline() const -> bool { return cur_underline_; }
    /// @brief 设置当前输入下划线（链式）。
    /// @param v 下划线开关。
    /// @return 本控件引用（`*this`），支持链式调用。
    auto set_current_underline(bool v) -> RichTextEdit & {
        cur_underline_ = v;
        return *this;
    }

    /// @brief 设置书写方向（链式）。nullopt = 继承环境（`Directionality` 注入/进程级），
    ///        与 Text/TextInput 语义一致。RTL 时光标/选区/命中走逻辑↔视觉镜像（逻辑首字符在右缘）、
    ///        段落整体右对齐。
    /// @param d 显式书写方向；布局期解析生效（见 `cached_direction_`）。
    /// @return 本控件引用（`*this`），支持链式调用。
    auto set_direction(std::optional<TextDirection> d) -> RichTextEdit & {
        direction_ = d;
        return *this;
    }
    /// @brief 显式书写方向设定。
    /// @return nullopt = 继承环境；否则为显式 LTR/RTL（序列化仅显式值落盘）。
    [[nodiscard]] auto direction() const -> std::optional<TextDirection> { return direction_; }

    /// @brief 切换当前字体粗体（weight 400 ↔ 700）。
    auto toggle_bold() -> void { cur_font_.weight = (cur_font_.weight >= 700) ? 400 : 700; }
    /// @brief 切换当前字体斜体（通过 family 后缀 "*" 模拟，实际渲染依赖 FontEngine）。
    auto toggle_italic() -> void {
        // 简化：用 family 尾部标记；真实场景需 Font::italic 字段。
        if (cur_font_.family.size() >= 2 && cur_font_.family.back() == 'I' &&
            cur_font_.family[cur_font_.family.size() - 2] == '/') {  // 与前判联合：末尾恰为斜体后缀 "/I"。
            cur_font_.family = cur_font_.family.substr(0, cur_font_.family.size() - 2);
        } else {
            cur_font_.family += "/I";
        }
    }
    /// @brief 切换当前下划线。
    auto toggle_underline() -> void { cur_underline_ = !cur_underline_; }

    // ---- UndoStack 集成 ----

    /// @brief 绑定撤销栈（链式）；此后编辑操作经它登记撤销/重做命令。
    /// @param stack 外部 UndoStack 指针（控件不持有所有权；nullptr = 不记录撤销）。
    /// @return 本控件引用（`*this`），支持链式调用。
    auto set_undo_stack(UndoStack *stack) -> RichTextEdit & {
        undo_ = stack;
        return *this;
    }
    /// @brief 当前绑定的撤销栈。
    /// @return 非拥有指针；nullptr = 未绑定。
    [[nodiscard]] auto undo_stack() const -> UndoStack * { return undo_; }

    // ---- 光标 / 选区 ----

    /// @brief 光标插入点位置。
    /// @return `doc_` 下标（UTF-8 字节口径，可等于文档长度）。
    [[nodiscard]] auto caret() const -> std::size_t { return caret_; }
    /// @brief 选区锚点一。
    /// @return 字节偏移；可能大于锚点二（消费侧按 min/max 归一）。
    [[nodiscard]] auto selection_start() const -> std::size_t { return sel_start_; }
    /// @brief 选区锚点二。
    /// @return 字节偏移；与锚点一相等即无选区。
    [[nodiscard]] auto selection_end() const -> std::size_t { return sel_end_; }
    /// @brief 是否存在非空选区。
    /// @return true = 两锚点不相等。
    [[nodiscard]] auto has_selection() const -> bool { return sel_start_ != sel_end_; }

    /// @brief 选区文本（取 [min, max) 半开区间，越界截到文档尾）。
    /// @return 选中字符的纯文本拼接；无选区为空串。
    [[nodiscard]] auto selected_text() const -> std::string {
        if (!has_selection()) {
            return {};
        }
        const auto a = std::min(sel_start_, sel_end_);
        const auto b = std::max(sel_start_, sel_end_);
        std::string out;  // 逐字符累积 [a, b) 区间的选中文本。
        for (auto i = a; i < b && i < doc_.size(); ++i) {
            out.push_back(doc_[i].ch);
        }
        return out;
    }

    // ---- Widget 接口 ----

    /// @brief 类型名（注册 / 诊断 / 序列化通道）。
    /// @return "RichTextEdit"。
    [[nodiscard]] auto type_name() const -> const char * override { return "RichTextEdit"; }

    /// @brief 无障碍值：文档纯文本，组合期间含光标处的 preedit（读屏应播报未上屏内容）。
    /// @return 全文纯文本；组合态时在光标下标处插入 preedit（字节偏移夹紧到文末）。
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_value() const -> std::string override {
        std::string out = plain_text();  // 以全文纯文本为底，组合态再把 preedit 插入光标处。
        if (is_composing()) {
            out.insert(std::min(caret_, out.size()), preedit_);
        }
        return out;
    }

    // ---- 无障碍语义（切片 1/2）：多行文本 + TextPattern 支撑 ----

    /// @brief 无障碍状态：多行文本（UIA 侧映射 `Document` 而非 `Edit`）。
    /// @return 基类通用状态叠加 `multiline = true` 后的结果。
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_state() const -> AccessibilityState override {
        AccessibilityState s = Widget::accessibility_state();
        s.multiline = true;
        return s;
    }

    /// @brief 无障碍文本全文（UTF-8 字节口径，与 `doc_` 的 char 序列一一对应）。
    /// @return 经 `mutable` 缓存中转的全文视图（内容等同 `plain_text()`）。
    ///
    /// @note 文档以 `StyledChar` 序列存储、`plain_text()` 按值拼接，无法给出持久视图，故经
    ///       `mutable` 缓存中转：**调用方须在本控件下一次文本操作前消费该视图**（桥侧立即复制）。
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_text() const -> std::string_view override {
        a11y_text_cache_ = plain_text();
        return a11y_text_cache_;
    }

    /// @brief 无障碍选区：内部 `sel_start_/sel_end_` 即 UTF-8 字节偏移（半开区间）。
    /// @return 两锚点 min/max 归一后的字节区间（本控件恒有值——光标态为零宽区间，不返回 nullopt）。
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_selection() const -> std::optional<AccessibilityTextSelection> override {
        return AccessibilityTextSelection{.start = std::min(sel_start_, sel_end_),
                                          .end = std::max(sel_start_, sel_end_)};
    }

    /// @brief 设置选区（UTF-8 字节半开区间），光标置于选区终点。
    /// @param start 半开区间起点（越界夹紧到文档长度）。
    /// @param end 半开区间终点（先收敛到 [start, 文档长度]，光标落位处）。
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
    /// @param start 替换区间起点（字节偏移，越界夹紧）。
    /// @param end 替换区间终点（夹紧到 [start, 文档长度]）。
    /// @param utf8 写入文本（UTF-8）；重建走单 span 的 `load_spans`，完成后光标置于插入串尾。
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
    /// @param req 动作请求；`Value` 时以 `req.text` 重建全文档。
    /// @return true = 本控件已消费；false = 转交基类处理。
    /// @note Side-effects: mutates document
    auto perform_accessibility_action(const AccessibilityActionRequest &req) -> bool override {
        if (req.action == AccessibilityAction::Value) {
            accessibility_replace_text(0, plain_text().size(), req.text);
            return true;
        }
        return Widget::perform_accessibility_action(req);
    }

    /// @brief 悬停默认文本光标：文本编辑区悬停 IBeam；修饰链显式 `cursor(...)` 声明优先。
    /// @return 恒为 `CursorShape::IBeam`。
    /// @note Side-effects: pure
    [[nodiscard]] auto cursor_shape() const -> std::optional<CursorShape> override { return CursorShape::IBeam; }

    /// @brief Enter 优先经 `on_key_event` 投递（换行插入依赖它，而非激活语义）。
    /// @return 恒为 true。
    /// @note Side-effects: pure
    [[nodiscard]] auto wants_activation_keys() const -> bool override { return true; }

    /// @brief 方向键优先经 `on_key_event` 投递（←/→ 光标与选区扩展依赖它，而非几何焦点导航）。
    ///
    /// 未认领的方向（↑/↓）仍回落焦点导航，见 `Widget::wants_navigation_keys()`。
    /// @return 恒为 true。
    /// @note Side-effects: pure
    [[nodiscard]] auto wants_navigation_keys() const -> bool override { return true; }

    /// @brief 运行时自描述（规格附录 B；实现在同名 `.cpp`）。
    /// @return 含 text/direction 等属性表、输入事件表与示例的描述符。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;

    /// @brief 实例侧自描述：转发静态 `describe_static`。
    /// @return 与 `describe_static()` 相同的描述符。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 信号登记：文档模型为普通成员（非 reactive），无信号需登记，空实现。
    /// @param out 输出参数（本控件不使用）。
    auto collect_signals([[maybe_unused]] std::vector<SignalViewBase *> &out) -> void override {}

    // ---- 事件处理 ----

    /// @brief 指针事件（实现在同名 `.cpp`）：左键按下获焦并按行高定位行、逐 run 命中取最近
    ///        字符（含头含尾）落光标，选区收敛为光标；点击超出行数时落到文档尾。
    /// @param e 鼠标事件（本地坐标；左键按下消费该事件）。
    auto on_pointer_event(MouseEvent &e) -> void override;

    /// @brief 键盘派发（实现在同名 `.cpp`）：Ctrl 组合键 → 撤销/重做/样式/全选/剪贴板，
    ///        方向键与 Home/End → 移动/扩选，Backspace/Delete/Enter → 编辑；非 Down 直接放行。
    /// @param e 键盘事件（命中任一处理组即置 `is_handled`）。
    auto on_key_event(KeyEvent &e) -> void override;

    /// @brief 字符输入：非空文本走统一插入路径 `do_insert`（带撤销记录）。
    /// @param e 输入事件（落字时消费）。
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
    /// @param e 组合事件：`committed` 上屏串（转 `do_insert`）、`preedit` 及其光标/选区码点下标
    ///        （夹取到 preedit 长度；端点倒置退化为单点）。无论是否落地都消费。
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
    /// @return preedit 的 UTF-8 副本（不进 `doc_`，仅绘制期可见）。
    [[nodiscard]] auto preedit() const -> std::string { return preedit_; }

    /// @brief 是否处于组合态（preedit 非空）。
    /// @return true = 组合中（有未上屏文本）。
    [[nodiscard]] auto is_composing() const -> bool { return !preedit_.empty(); }

    /// @brief 组合光标在 preedit 内的码点下标（候选插入点）。
    /// @return 下标（更新时已夹紧到 preedit 码点数）。
    [[nodiscard]] auto composition_cursor() const -> std::size_t { return preedit_cursor_; }

    /// @brief IME 候选窗定位盒：文档光标处叠加 preedit 前缀宽度（组合中即候选插入点）。
    ///        未经绘制遍历（无有效光标行）时回退基类的控件盒。
    /// @return 零宽盒：x = 光标视觉位置 + preedit 已编码部分宽度，y/高度取光标行。
    [[nodiscard]] auto composition_caret_bounds() const -> Rect override;

    /// @brief 焦点变更：失焦即取消未上屏的组合（平台 IME 惯例）。
    /// @param focused true = 获焦；false = 失焦（触发组合取消）。
    auto on_focus_change(bool focused) -> void override {
        Widget::on_focus_change(focused);
        if (!focused) {
            cancel_composition();
        }
    }

    // ---- 序列化 ----

    /// @brief 序列化属性（实现在同名 `.cpp`）：导出纯文本 `text`；仅显式设置时导出方向
    ///        （继承环境语义不落盘）。
    /// @param props 目标 JSON 对象（基类先写通用字段）。
    auto serialize_props(Json &props) const -> void override;

    /// @brief 反序列化属性（实现在同名 `.cpp`）：`text` 按键存在且为字符串时以当前输入样式
    ///        逐字符重建文档（类型不符记降级诊断）；`direction` 仅认显式字符串值。
    /// @param props 外部传入的 JSON 对象。
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
        std::vector<StyledChar> chars;  ///< 该行换行产物（不含 '\n' 本身；行距以 chars.size()+1 计）。
    };

    /// @brief 按可用宽度重排换行：遇 '\n' 断行，逐字测宽超出 max_width 强制折行（空行不折）；
    ///        空文档也产出一个空行。结果写入 `lines_`。
    /// @param max_width 换行可用宽度（布局约束上界 / 绘制区宽）。
    auto relayout_lines(float max_width) -> void;

    /// @brief (行号, 行内列) → `doc_` 全局下标：每行占 chars.size()+1（含换行符），结果夹紧到
    ///        文档尾；行号越界返回文档尾。
    /// @param line_idx 行下标（`lines_` 序）。
    /// @param col 行内列（不含 '\n' 的行内字符数）。
    /// @return `doc_` 全局下标（UTF-8 字节口径）。
    auto pos_from_line_col(size_t line_idx, size_t col) const -> size_t;

    /// @brief 单行「生效方向 + 各样式/层级 run」的视觉布局（RTL + 完整 UBA）：连续同样式
    ///        字符合并为样式 run，再逐 run 按完整 UBA（UAX #9）嵌入层级细分为层级 run（层级
    ///        单一，hb 方向取层级奇偶）；跨 run 顺序按 `uba_visual_order`（L2 层叠反转）重排；
    ///        RTL 段落整体右对齐。返回每个 run 的视觉起点 x、宽度、行内下标区间与整形方向。
    struct RunLayout {
        std::string text;  ///< 该 run 文本（UTF-8；样式 run 先按同 font 同 color 合并，再按层级细分）。
        Font font = {};  ///< run 统一字体（样式合并判据）。
        Color color = Color::black();  ///< run 统一颜色（样式合并判据）。
        std::vector<char> underline;  ///< 与 text 逐字对应的下划线标记（0/1）
        float x = 0.0F;  ///< run 视觉起点 x（L2 重排后写入；RTL 段整体右对齐后再排布）。
        float w = 0.0F;  ///< run 视觉宽度（按其整形方向测得）。
        std::size_t begin = 0;  ///< 该 run 首字符在行内的逻辑下标（含 '\n' 计 1）
        std::size_t end = 0;  ///< 该 run 末字符逻辑下标 +1
        TextDirection dir = TextDirection::LTR;  ///< 该 run 的整形方向（UBA 层级奇偶）
    };

    /// @brief 一行的视觉 run 布局：先按同 font 同 color 合并样式 run，再按完整 UBA（UAX #9）
    ///        嵌入层级细分为层级 run（方向取层级奇偶），逐 run 测宽后按 L2 层叠反转重排视觉
    ///        起点 x（RTL 段落整体右对齐）。
    /// @param line 待布局行（不含 '\n' 的 styled 字符序列）。
    /// @param base 该行的段落基准方向（显式方向优先，否则按内容 guess）。
    /// @param bounds 布局参照盒（LTR 自左缘起排，RTL 以其右缘为锚）。
    /// @return 按视觉顺序排列的层级 run（含 x/w/begin/end/dir）。
    auto compute_line_runs(const Line &line, TextDirection base, const Rect &bounds) const -> std::vector<RunLayout>;

    /// @brief 计算给定行内、逻辑下标 `caret_local` 处光标的视觉 x（相对 bounds 左缘）。
    ///        RTL 下 `FontEngine::caret_x` 返回逻辑 0 在右缘的视觉偏移，调用方无需再镜像。
    /// @param line 光标所在行。
    /// @param base 该行的段落基准方向。
    /// @param caret_local 行内逻辑下标（0..chars.size()，即光标插入点列）。
    /// @param bounds run 布局参照盒（RTL 右对齐基准）。
    /// @return 光标视觉 x；没有任何 run 覆盖该下标时返回 -1.0F（调用侧视为无效）。
    auto caret_visual_x(const Line &line, TextDirection base, std::size_t caret_local, const Rect &bounds) const
        -> float;

    // ---- 编辑操作（带 UndoStack 集成）----

    /// @brief 编辑前快照：整文档克隆 + 光标位 + 动作标签（撤销/重做的恢复依据）。
    struct UndoSnapshot {
        std::string description;  ///< 动作标签（如 "insert" / "backspace" / "delete selection"）。
        std::vector<StyledChar> doc;  ///< 快照时的文档内容。
        std::size_t caret;  ///< 快照时的光标下标。
    };

    /// @brief 压入一条撤销命令：闭包快照（旧 doc/旧 caret）与当前状态（新 doc/新 caret），
    ///        undo/redo 各自整体恢复 doc_ 并把选区收敛到光标；未绑定撤销栈时静默跳过。
    /// @param snap 编辑前快照。
    auto push_undo(const UndoSnapshot &snap) -> void;

    /// @brief 统一插入路径：有选区先替换式删除（不另记 undo），按当前输入样式
    ///        （font/color/underline）逐字符插入光标处，最后以 "insert" 记一条撤销。
    /// @param text 待插入文本（UTF-8，逐字节成 StyledChar）。
    auto do_insert(const std::string &text) -> void;

    /// @brief 删除选区（[min, max) 区间），以 "delete selection" 记撤销；无选区时 no-op。
    auto do_delete_selection() -> void;

    /// @brief 删除选区但不记 undo（供插入替换等复合操作内部复用），光标落到区间起点。
    auto do_delete_selection_no_undo() -> void;

    /// @brief 处理 Ctrl/Cmd 组合快捷键（撤销/重做/样式/全选/剪贴板）。返回是否命中并处理。
    /// @param e 键盘事件（认 Z/Y/B/I/U/A/C/X/V 键码，其余不命中）。
    /// @param shift true = 带 Shift（Ctrl+Shift+Z 走重做而非撤销）。
    /// @param n 文档长度（Ctrl+A 全选时据此选中整篇并把光标置尾）。
    /// @return true = 快捷键命中（调用方标记已处理）；false = 非识别组合，继续后续派发。
    auto handle_control_shortcut(KeyEvent &e, bool shift, std::size_t n) -> bool;

    /// @brief 处理方向键 / Home / End（含 Shift 扩展选区）。返回是否命中。
    /// @param e 键盘事件（←/→/Home/End；RTL 下方向键按逻辑↔视觉反转）。
    /// @param shift true = 扩选（移动光标并更新 sel_end_）；false = 移光标并塌缩选区。
    /// @param n 文档长度（光标移动与扩选的钳制上界）。
    /// @return true = 移动类键命中。
    auto handle_move_key(KeyEvent &e, bool shift, std::size_t n) -> bool;

    /// @brief 处理 Backspace / Delete / Enter 编辑键。返回是否命中。
    /// @param e 键盘事件（三键之一）。
    /// @param n 文档长度（Delete 判断右侧是否还有字符）。
    /// @return true = 编辑键命中（Enter 走 `do_insert("\n")` 统一插入路径）。
    auto handle_text_key(KeyEvent &e, std::size_t n) -> bool;

    // ---- 绘制辅助 ----

    /// @brief 绘制选区高亮：逐排版行扫描，对 [min, max) 内的每个字符按 run 视觉边界填
    ///        半透明蓝底矩形（行与选区无交集则整行跳过）。
    /// @param p 目标绘制器。
    /// @param bounds 绘制区（行自顶向下按 line_height_ 排布）。
    auto paint_selection_highlight(Painter &p, const Rect &bounds) const -> void;

    /// @brief 绘制文档光标：`caret_box_at` 给出有效盒时以黑色细条填充（无效则跳过）。
    /// @param p 目标绘制器。
    /// @param bounds 绘制区参照（透传给 `caret_box_at`）。
    auto paint_cursor(Painter &p, const Rect &bounds) const -> void;

    /// @brief 文档光标定位盒（绝对窗口逻辑 dp，`width` 为光标笔宽）；caret 不在任何已排版行内
    ///        （未绘制 / 空文档未换行）或视觉 x 无效时返回 `nullopt`。
    ///        `paint_cursor` 与 IME 候选窗定位（`composition_caret_bounds`）共用同一算式。
    /// @param bounds 行扫描参照盒（绘制区 / focus_bounds）。
    /// @return 宽 1dp、高一行高的光标盒；无法定位时为 nullopt。
    [[nodiscard]] auto caret_box_at(const Rect &bounds) const -> std::optional<Rect>;

    /// @brief 组合态绘制：preedit 内选区高亮 + preedit 文本 + 下划线 + 候选插入点光标。
    ///        在 `(x, y)` 处绘制，返回占用宽度（供调用方推进游标，实现「预编辑串挤开后续文本」）。
    /// @param p 目标绘制器。
    /// @param x preedit 起点 x（文档光标的视觉位置，由调用方经 `caret_visual_x` 求得）。
    /// @param y 所绘行顶部 y。
    /// @return preedit 的测量宽度。
    auto paint_preedit(Painter &p, float x, float y) const -> float;

    /// @brief 取消组合：清空 preedit 与其选区（失焦 / 平台侧取消时调用）。
    auto cancel_composition() -> void;

    // ---- 数据成员 ----
    /// @brief 文档本体：带样式字符序列（char 级，UTF-8 多字节逐字节展开），编辑即对其插/删。
    std::vector<StyledChar> doc_;
    std::size_t caret_ = 0;  ///< 光标插入点（`doc_` 下标，可等于文档长度）。
    std::size_t sel_start_ = 0;  ///< 选区锚点一（字节偏移；可大于锚点二，消费侧 min/max 归一）。
    std::size_t sel_end_ = 0;  ///< 选区锚点二（字节偏移；与一相等 = 无选区）。
    /// @brief `accessibility_text()` 的视图中转缓存（见该方法注释：调用方须即时消费）。
    mutable std::string a11y_text_cache_;
    Font cur_font_{};  ///< 当前输入字体（新插入字符的样式模板）。
    Color cur_color_ = Color::black();  ///< 当前输入颜色。
    bool cur_underline_ = false;  ///< 当前输入下划线开关。
    UndoStack *undo_ = nullptr;  ///< 外部撤销栈（不持有所有权；nullptr = 编辑不记撤销）。
    float line_height_ = 20.0F;  ///< 生效行高（dp；on_layout 以当前字体测量，异常回退 20）。
    std::vector<Line> lines_;  ///< `relayout_lines` 的换行产物（绘制/命中/光标定位同源）。

    // IME 组合态：preedit 不进 doc_，仅在绘制期插到 caret_ 处。
    /// @brief preedit 内「无选区」哨兵。
    static constexpr std::size_t AURORA_NO_PREEDIT_SELECTION = TextCompositionEvent::AURORA_NO_SELECTION;
    std::string preedit_;  ///< 预编辑串（UTF-8）；空 = 无组合
    std::size_t preedit_cursor_ = 0;  ///< 组合光标在 preedit 内的码点下标
    std::size_t preedit_sel_start_ = 0;  ///< preedit 内选区起点（码点下标）
    std::size_t preedit_sel_end_ = AURORA_NO_PREEDIT_SELECTION;  ///< 选区终点（含尾）；AURORA_NO_PREEDIT_SELECTION = 无
};

}  // namespace aurora
