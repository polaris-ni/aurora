#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/diagnostics.h"
#include "aurora/core/enums.h"
#include "aurora/core/font.h"
#include "aurora/environment/environment.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/i18n/string_table.h"
#include "aurora/state/reactive.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief Text 属性（聚合，AI 用指定初始化器填写）。
struct TextProps {
    Reactive<LocalizedString> content;  ///< 文本内容（可由字符串隐式构造）
    Font font = Font{};  ///< 字体（默认 14pt）
    Color text_color = Color::black();  ///< 文字色（默认黑）

    // ---- 文本排版（参考 Flutter TextStyle / Text） ----
    TextAlign text_align = TextAlign::Left;  ///< 水平对齐
    int max_lines = 0;  ///< 最大行数（0=不限）；超出按 overflow 处理
    TextOverflow overflow = TextOverflow::Clip;  ///< 超出 max_lines 时的处理
    bool soft_wrap = true;  ///< 是否按宽度自动换行
    float line_height = 1.0F;  ///< 行高倍数（相对字高）
    float letter_spacing = 0.0F;  ///< 字形间距（FontEngine 暂不支持 → 优雅降级）
    float word_spacing = 0.0F;  ///< 词间距（FontEngine 暂不支持 → 优雅降级）
    FontStyle font_style = FontStyle::Normal;  ///< 字形风格（Italic 暂降级为 Normal）
    TextDecoration decoration = TextDecoration::None;  ///< 装饰线（可按位组合）
    Color decoration_color = Color::black();  ///< 装饰线颜色（默认同文字色）
    Color background_color = Color{0, 0, 0, 0};  ///< 文本底色（alpha=0 表示无）

    /// @brief 逐控件抗锯齿覆写：nullopt = 用进程级 `text_aa_mode()`（默认 ClearType）；
    ///        设为 `Supersample` 可让本控件文字在彩色/动画/渐变背景上避免 ClearType 子像素白边
    ///        （例：`examples/demos/common.h:GradientTitle` 与 demo_animation 的「color pulse」呼吸盒）。
    std::optional<render::TextAAMode> text_aa_mode = std::nullopt;
};

/**
 * @brief 文本控件（叶 widget）：测量文字自然尺寸，绘制文本。
 *
 * 支持**双模 API**（specification/04-widget.md §2.5）：配置块 `Text{.content="Hi", .font={.sizePt=14}}`
 * 与链式 `au::Text("Hi").font_size(14).color(au::colors::Red)` 等价。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Text : public LeafWidget, public TextProps {
  public:
    Text() = default;
    /// @brief 配置块构造。
    explicit Text(TextProps props) : TextProps(std::move(props)) {}
    explicit Text(LocalizedString s) { content = std::move(s); }
    explicit Text(const std::string &s) { content = s; }
    explicit Text(const char *s) { content = s; }

    auto set_content(const std::string &s) -> Text & {
        content = s;
        return *this;
    }

    // 非正数降级为 14pt 并产生诊断（需求 #21）。
    auto font_size(float pt) -> Text & {
        if (pt <= 0.0F) {
            Diagnostics::degraded("widget", "Text 字号 <= 0 已降级为 14pt");
            font.size_pt = 14.0F;
        } else {
            font.size_pt = pt;
        }
        return *this;
    }

    auto color(Color c) -> Text & {
        text_color = c;
        return *this;
    }

    // 字重 CSS 700。
    auto bold() -> Text & {
        font.weight = 700;
        return *this;
    }

    auto family(std::string f) -> Text & {
        font.family = std::move(f);
        return *this;
    }

    // 映射到 font.weight 数值。
    auto font_weight(FontWeight w) -> Text & {
        font.weight = static_cast<int>(w);
        return *this;
    }

    // 彩色/动画/渐变背景上用 `Supersample` 可避免 ClearType 子像素白边。
    auto text_aa(render::TextAAMode mode) -> Text & {
        text_aa_mode = mode;
        return *this;
    }

    auto set_align(TextAlign a) -> Text & {
        text_align = a;
        return *this;
    }

    auto set_max_lines(int n) -> Text & {
        max_lines = n;
        return *this;
    }

    auto set_overflow(TextOverflow o) -> Text & {
        overflow = o;
        return *this;
    }

    auto set_soft_wrap(bool b) -> Text & {
        soft_wrap = b;
        return *this;
    }

    auto set_line_height(float h) -> Text & {
        line_height = h;
        return *this;
    }

    auto set_letter_spacing(float s) -> Text & {
        letter_spacing = s;
        return *this;
    }

    auto set_word_spacing(float s) -> Text & {
        word_spacing = s;
        return *this;
    }

    auto set_font_style(FontStyle s) -> Text & {
        font_style = s;
        return *this;
    }

    auto set_decoration(TextDecoration d) -> Text & {
        decoration = d;
        return *this;
    }

    auto set_decoration_color(Color c) -> Text & {
        decoration_color = c;
        return *this;
    }

    auto set_background_color(Color c) -> Text & {
        background_color = c;
        return *this;
    }

    [[nodiscard]] auto resolved_text(const BuildContext &ctx) const -> std::string {
        const auto *lp = ctx.environment<Locale>();
        return content.get().resolve(&default_string_table(), (lp != nullptr) ? *lp : Locale{});
    }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&content); }

    [[nodiscard]] auto type_name() const -> const char * override { return "Text"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;

    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 构建期属性约束校验（specification/04-widget.md §2.2）：当前校验字号必须为正。
    [[nodiscard]] auto validate_props() const -> Result<void> override;

    auto serialize_props(Json &props) const -> void override;
    auto deserialize_props(const Json &props) -> void override;

    auto on_pointer_event(MouseEvent &e) -> void override;
    auto on_key_event(KeyEvent &e) -> void override;

    /// @brief 失焦时取消选区。
    auto on_focus_change(bool focused) -> void override {
        if (!focused) {
            sel_end_ = NO_SEL;  // 标记无选区（含头含尾模型下，起点=终点表示 1 字符而非无选区）
            sel_start_ = caret_;
            selecting_ = false;
            mark_needs_paint();
        }
        Widget::on_focus_change(focused);
    }

    /// @brief 选区（码点下标，含头含尾），对外以 caret 区间 [a, b+1) 形式返回，
    /// 与命中测试/高亮/复制等下游逻辑兼容。无选区时返回 {0, 0}。
    [[nodiscard]] auto selection() const -> std::pair<size_t, size_t> {
        if (!has_selection()) {
            return {caret_, caret_};
        }
        const size_t a = std::min(sel_start_, sel_end_);
        const size_t b = std::max(sel_start_, sel_end_);
        return {a, b + 1};
    }
    [[nodiscard]] auto has_selection() const -> bool { return sel_end_ != NO_SEL; }
    [[nodiscard]] auto display_text() const -> const std::string & { return display_text_; }

  protected:
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;

    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;

  private:
    /// @brief 无选区哨兵：m_sel_end 取此值时表示当前没有选区。
    static constexpr size_t NO_SEL = static_cast<size_t>(-1);

    size_t sel_start_ = 0;  ///< 选区起点（含入的码点下标：该字符被选中）
    size_t sel_end_ = NO_SEL;  ///< 选区终点（含入的码点下标；= AURORA_NO_SEL 表示无选区）
    size_t caret_ = 0;  ///< 光标（caret 位置，0..码点数；用于键盘导航）
    bool selecting_ = false;  ///< 是否正在拖选
    std::string display_text_;  ///< 最近一次绘制所用显示文本（命中测试/选区使用）
    std::string cached_resolved_text_;  ///< 缓存的 resolved_text 结果（避免每帧重复解析）
    bool resolved_dirty_ = true;  ///< resolved 缓存是否需重新计算
    std::vector<std::string> lines_;  ///< 最近一次布局所得折行结果（绘制复用）
    std::vector<size_t> line_cp_start_;  ///< 每个可视行首字符在 m_display_text 中的码点下标
    float line_h_ = 0.0F;  ///< 行高（含 line_height 倍数）
    float layout_w_ = 0.0F;  ///< 最近一次布局所得控件宽度（命中测试按对齐偏移需要）
    float paint_scale_ = 1.0F;  ///< 最近一次绘制的帧缓冲像素比（dp→物理；实显 caret 校正用）

    static auto split_words(const std::string &text) -> std::vector<std::string>;
    /// @brief 折行结果：lines=可视行文本，cp_start=各行首字符在原始 text 中的码点下标（一一对应）。
    using WrapResult = std::pair<std::vector<std::string>, std::vector<size_t>>;
    static auto finalize_lines(std::vector<std::string> lines, std::vector<size_t> cp_start, const Font &f, float max_w,
                               int max_lines, TextOverflow overflow, const render::TextLayoutOpts &opts) -> WrapResult;
    static auto wrap_lines(const std::string &text, const Font &f, float max_w, bool soft_wrap, int max_lines,
                           TextOverflow overflow, const render::TextLayoutOpts &opts) -> WrapResult;
    static auto effective_font(const Font &base) -> Font;
    static auto cp_len(unsigned char c) -> size_t;
    static auto cp_count(const std::string &s) -> size_t;
    static auto cp_slice(const std::string &s, size_t start, size_t count) -> std::string;
    [[nodiscard]] auto selected_text() const -> std::string;

    /// @brief Justify 行逐词布局项（与 on_paint 两端对齐绘制严格一致）。
    struct JustifiedWord {
        std::string text;  ///< 词内容（不含空格）
        size_t cp_begin = 0;  ///< 词首字符在行内的码点下标
        size_t cp_end = 0;  ///< 词尾后一位置（行内码点下标，不含）
        float x = 0.0F;  ///< 词左缘相对行左缘的 x
        float w = 0.0F;  ///< 词自然宽度
    };
    /// @brief 计算 Justify 行的逐词均分布局（剩余宽度均分进词间隙）；
    ///        词数 < 2 时返回空（该行不做两端对齐，与 on_paint 一致）。
    static auto justify_layout(const std::string &line, const Font &f, const render::TextLayoutOpts &opts, float avail)
        -> std::vector<JustifiedWord>;
    /// @brief 可视行 li 是否按两端对齐绘制（Justify 且多行且非末行，与 on_paint 判定一致）。
    [[nodiscard]] auto is_justified_line(size_t li) const -> bool;
    /// @brief 行内 caret x（相对行左缘）：Justify 行按均分布局取词位（行尾 = 行右缘 avail），
    ///        其余行走 FontEngine::display_caret_x（按 m_paint_scale 推导物理 DPI 的前缀
    ///        extent，逐字符与实绘字形对齐；scale=1 退化为 caret_x）；选区高亮与绘制像素一一对应。
    [[nodiscard]] auto line_caret_x(size_t li, size_t cp_in_line, const Font &f, const render::TextLayoutOpts &opts,
                                    float avail) const -> float;
    /// @brief 行内命中测试：返回 {caret 位置, 含头含尾字符下标}（行内码点）；
    ///        Justify 行按均分布局反解（词间拉伸间隙整体归属其空格字符），
    ///        其余行走实显命中（FontEngine::display_hit_test_char*，与实绘字形逐字符对齐）。
    [[nodiscard]] auto line_hit_test(size_t li, float x, const Font &f, const render::TextLayoutOpts &opts,
                                     float avail) const -> std::pair<size_t, size_t>;
};

}  // namespace aurora
