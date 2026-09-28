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
#include "aurora/environment/build_context.h"
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

    /// @brief 以下至 `background_color` 为文本排版属性组（参考 Flutter TextStyle / Text）。
    TextAlign text_align = TextAlign::Left;  ///< 水平对齐
    /// @brief 书写方向：nullopt = 继承（`Environment`/进程级；无任何显式来源时
    ///        shaping 按内容自动 guess——默认行为与接入前逐位一致）。
    ///        RTL 时 `TextAlign::Start/End` 解析为 Right/Left，caret/命中做逻辑↔视觉镜像。
    std::optional<TextDirection> direction = std::nullopt;
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

/// @brief 文本控件（叶 widget）：测量文字自然尺寸，绘制文本。
///
/// 支持**双模 API**（specification/04-widget.md §2.5）：配置块 `Text{.content="Hi", .font={.sizePt=14}}`
/// 与链式 `au::Text("Hi").font_size(14).color(au::colors::Red)` 等价。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
class Text : public LeafWidget, public TextProps {
  public:
    /// @brief 纯展示件默认不是 Tab 停点：只有挂上点击 / 手势 / 菜单 / 滚动 / 键盘认领时
    ///        才可聚焦（覆写基类 public virtual；分级默认见 specification/05 §4.2）。
    /// @return `has_input_semantics()` 的判定结果。
    [[nodiscard]] auto wants_focus() const -> bool override { return has_input_semantics(); }
    /// @brief 默认构造：空内容、默认字体（14pt）、黑色文字、不限行、自动换行。
    Text() = default;
    /// @brief 配置块构造：整体接管 `TextProps` 各字段。
    /// @param props 属性聚合（指定初始化器填写），移入本对象基类子对象。
    explicit Text(TextProps props) : TextProps(std::move(props)) {}
    /// @brief 以本地化字符串构造（内容存为响应式值，随其变化刷新）。
    /// @param s 待显示的 `LocalizedString`（key + fallback 文本）。
    explicit Text(LocalizedString s) { content = std::move(s); }
    /// @brief 以 UTF-8 文本构造。
    /// @param s 待显示的字面文本（包成非本地化内容）。
    explicit Text(const std::string &s) { content = s; }
    /// @brief 以 C 字符串构造。
    /// @param s 待显示的字面文本（nullptr 语义由 `LocalizedString` 转换处理）。
    explicit Text(const char *s) { content = s; }

    /// @brief 替换文本内容（链式）。
    /// @param s 新的 UTF-8 文本，写入响应式 `content`。
    /// @return 自身引用，便于链式调用。
    auto set_content(const std::string &s) -> Text & {
        content = s;
        return *this;
    }

    /// @brief 设置字号 pt（链式）；非正数降级为 14pt 并产生诊断（需求 #21）。
    /// @param pt 字号（point）；<= 0 时取默认 14pt 并记 `Diagnostics::degraded`。
    /// @return 自身引用，便于链式调用。
    auto font_size(float pt) -> Text & {
        if (pt <= 0.0F) {
            Diagnostics::degraded("widget", "Text 字号 <= 0 已降级为 14pt");
            font.size_pt = 14.0F;
        } else {
            font.size_pt = pt;
        }
        return *this;
    }

    /// @brief 设置文字颜色（链式）。
    /// @param c 文字前景色。
    /// @return 自身引用，便于链式调用。
    auto color(Color c) -> Text & {
        text_color = c;
        return *this;
    }

    /// @brief 加粗（链式）：字重置为 CSS 700。
    /// @return 自身引用，便于链式调用。
    auto bold() -> Text & {
        font.weight = 700;
        return *this;
    }

    /// @brief 设置字体族名（链式）。
    /// @param f 字体 family（移入内部存储）；无匹配字体时由 FontEngine 走回退链。
    /// @return 自身引用，便于链式调用。
    auto family(std::string f) -> Text & {
        font.family = std::move(f);
        return *this;
    }

    /// @brief 设置字重（链式）：把 `FontWeight` 枚举映射到 `font.weight` 数值。
    /// @param w 字重档位（其底层值即 CSS 数值口径）。
    /// @return 自身引用，便于链式调用。
    auto font_weight(FontWeight w) -> Text & {
        font.weight = static_cast<int>(w);
        return *this;
    }

    /// @brief 逐控件覆写抗锯齿模式（链式）：彩色/动画/渐变背景上用 `Supersample` 可避免 ClearType 子像素白边。
    /// @param mode 文本抗锯齿模式；绘制期优先于进程级 `text_aa_mode()`。
    /// @return 自身引用，便于链式调用。
    auto text_aa(render::TextAAMode mode) -> Text & {
        text_aa_mode = mode;
        return *this;
    }

    /// @brief 设置水平对齐（链式）。
    /// @param a 对齐方式；`Justify` 且多行时对非末行做逐词均分（见 `justify_layout`）。
    /// @return 自身引用，便于链式调用。
    auto set_align(TextAlign a) -> Text & {
        text_align = a;
        return *this;
    }

    /// @brief 设置书写方向：RTL 时 Start/End 对齐翻转、caret/命中镜像映射。
    /// @param d 书写方向（覆盖 `Environment` 与进程级来源）。
    /// @return 自身引用，便于链式调用。
    auto set_direction(TextDirection d) -> Text & {
        direction = d;
        return *this;
    }

    /// @brief 设置最大行数（链式）。
    /// @param n 行数上限；0 = 不限，超出部分按 `overflow` 处理。
    /// @return 自身引用，便于链式调用。
    auto set_max_lines(int n) -> Text & {
        max_lines = n;
        return *this;
    }

    /// @brief 设置溢出策略（链式）。
    /// @param o 超出 `max_lines` 的处理方式（Clip / Ellipsis 等）。
    /// @return 自身引用，便于链式调用。
    auto set_overflow(TextOverflow o) -> Text & {
        overflow = o;
        return *this;
    }

    /// @brief 设置是否自动软换行（链式）。
    /// @param b true 按可用宽度折行，false 单行不换行。
    /// @return 自身引用，便于链式调用。
    auto set_soft_wrap(bool b) -> Text & {
        soft_wrap = b;
        return *this;
    }

    /// @brief 设置行高倍数（链式）。
    /// @param h 相对字高的行距倍数（反序列化时非正值被拒收并保留旧值）。
    /// @return 自身引用，便于链式调用。
    auto set_line_height(float h) -> Text & {
        line_height = h;
        return *this;
    }

    /// @brief 设置字形间距（链式）。
    /// @param s 字符间额外间距 dp；FontEngine 暂不支持时优雅降级（不生效）。
    /// @return 自身引用，便于链式调用。
    auto set_letter_spacing(float s) -> Text & {
        letter_spacing = s;
        return *this;
    }

    /// @brief 设置词间距（链式）。
    /// @param s 词间额外间距 dp；FontEngine 暂不支持时优雅降级（不生效）。
    /// @return 自身引用，便于链式调用。
    auto set_word_spacing(float s) -> Text & {
        word_spacing = s;
        return *this;
    }

    /// @brief 设置字形风格（链式）。
    /// @param s 风格枚举；`Italic` 当前降级为 `Normal`。
    /// @return 自身引用，便于链式调用。
    auto set_font_style(FontStyle s) -> Text & {
        font_style = s;
        return *this;
    }

    /// @brief 设置装饰线（链式）。
    /// @param d 装饰线位组合（下划线/删除线等，可按位组合）。
    /// @return 自身引用，便于链式调用。
    auto set_decoration(TextDecoration d) -> Text & {
        decoration = d;
        return *this;
    }

    /// @brief 设置装饰线颜色（链式）。
    /// @param c 装饰线颜色；未设置时默认与文字色一致。
    /// @return 自身引用，便于链式调用。
    auto set_decoration_color(Color c) -> Text & {
        decoration_color = c;
        return *this;
    }

    /// @brief 设置文本底色（链式）。
    /// @param c 底色；alpha = 0 表示不绘制底色。
    /// @return 自身引用，便于链式调用。
    auto set_background_color(Color c) -> Text & {
        background_color = c;
        return *this;
    }

    /// @brief 解析当前应显示的文本（i18n 求值）。
    /// @param ctx 构建上下文：提供 `Environment` 注入的 `Locale`；缺失时按默认 locale 解析。
    /// @return 解析后的 UTF-8 文本；内容带 key 时查默认字符串表，否则为原文。
    [[nodiscard]] auto resolved_text(const BuildContext &ctx) const -> std::string {
        const auto *lp = ctx.environment<Locale>();
        return content.get().resolve(&default_string_table(), (lp != nullptr) ? *lp : Locale{});
    }

    /// @brief 收集响应式信号：仅文本内容 `content`。
    /// @param out 输出向量：挂载时由基类登记依赖。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&content); }

    /// @brief widget 类型名（结构快照 JSON 用）。
    /// @return 字面量 `"Text"`。
    [[nodiscard]] auto type_name() const -> const char * override { return "Text"; }

    /// @brief 首行基线（内容盒顶 → 基线）：Text 的首行就从内容盒顶起排（`on_paint` 的 `y = bounds.origin.y`），
    ///        故即有效字体的 ascent，与 `draw_text` 的 pen_y 同源（`FontEngine::measure_ascent`）。
    ///        多行文本仍取首行；不做整像素 snap（见 `Widget::baseline_distance` 的说明）。
    /// @param ctx 构建上下文：用于取有效字体（含字号缩放）。
    /// @return 基线距离 dp；字体度量不可用时 nullopt（调用方按无基线对齐处理）。
    [[nodiscard]] auto baseline_distance(const BuildContext &ctx) const -> std::optional<float> override;

    /// @brief 无障碍名称：取文本内容。
    ///
    /// 优先复用绘制期缓存的解析结果 `display_text_`（= `cached_resolved_text_`，已解析 i18n）；
    /// 未经绘制时退回按默认 locale 现场解析 `content`，保证无绘制查询也有 name。
    /// @return 显示文本（UTF-8）。
    /// @note Side-effects: reads i18n table
    [[nodiscard]] auto accessibility_label() const -> std::string override {
        if (!display_text_.empty()) {
            return display_text_;
        }
        return content.get().resolve(&default_string_table(), Locale{});
    }

    /// @brief 运行时自描述：content/字体/排版/装饰等全部属性键与默认值。
    /// @return Text 的控件描述符（实现见 `src/aurora/widget/text.cpp`）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;

    /// @brief 运行时多态自描述入口。
    /// @return 转发 `describe_static()` 的结果。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 构建期属性约束校验（specification/04-widget.md §2.2）：当前校验字号必须为正。
    /// @return 通过返回成功；否则为 `WidgetInvalidProp` 错误（含属性名与原因）。
    [[nodiscard]] auto validate_props() const -> Result<void> override;

    /// @brief 序列化全部排版属性到 props JSON（content/字号/颜色/对齐/方向/行数/溢出/行高等）。
    /// @param props 输出对象，先由基类填通用属性；`direction` 仅在显式设置时输出。
    /// @note Rebuildable: yes, via from_json
    auto serialize_props(Json &props) const -> void override;
    /// @brief 从 props JSON 还原排版属性：逐键校验类型/取值域，非法值记诊断并保留旧值。
    /// @param props 属性 JSON；缺失键不改动对应字段。
    auto deserialize_props(const Json &props) -> void override;

    /// @brief 指针事件：建立/延伸选区、按下定位 caret（实现见 `src/aurora/widget/text.cpp`）。
    /// @param e 鼠标事件；命中本行文本时改写 `caret_`/`sel_start_`/`sel_end_`。
    auto on_pointer_event(MouseEvent &e) -> void override;
    /// @brief 键盘事件：方向键移 caret、Shift 扩选、Home/End 移行首行尾。
    /// @param e 按键事件（实现见 `src/aurora/widget/text.cpp`）。
    auto on_key_event(KeyEvent &e) -> void override;

    /// @brief 失焦时取消选区。
    /// @param focused false 时清除选区（`sel_end_` 回落哨兵）、把选区起点贴回 caret 并标脏重绘。
    auto on_focus_change(bool focused) -> void override {
        if (!focused) {
            sel_end_ = AURORA_NO_SEL;  // 标记无选区（含头含尾模型下，起点=终点表示 1 字符而非无选区）
            sel_start_ = caret_;
            selecting_ = false;
            mark_needs_paint();
        }
        Widget::on_focus_change(focused);
    }

    /// @brief 选区（码点下标，含头含尾），对外以 caret 区间 [a, b+1) 形式返回，
    /// 与命中测试/高亮/复制等下游逻辑兼容。无选区时返回 {0, 0}。
    /// @return `{起点, 终点}` 码点下标对；无选区时两端同为 `caret_`。
    [[nodiscard]] auto selection() const -> std::pair<size_t, size_t> {
        if (!has_selection()) {
            return {caret_, caret_};
        }
        const size_t a = std::min(sel_start_, sel_end_);
        const size_t b = std::max(sel_start_, sel_end_);
        return {a, b + 1};
    }
    /// @brief 当前是否存在选区。
    /// @return `sel_end_` 不等于无选区哨兵 `AURORA_NO_SEL` 时为 true。
    [[nodiscard]] auto has_selection() const -> bool { return sel_end_ != AURORA_NO_SEL; }
    /// @brief 最近一次绘制所用的显示文本（i18n 已解析）。
    /// @return 指向内部缓存串的引用；未绘制过为空串，命中测试/选区即以它为下标基准。
    [[nodiscard]] auto display_text() const -> const std::string & { return display_text_; }

  protected:
    /// @brief 绘制文本：底色 → 逐可视行字形 → 装饰线 → 选区高亮与 caret。
    /// @param p 软件光栅画笔。
    /// @param bounds 本控件的绘制矩形（全局坐标）。
    /// @param ctx 构建上下文：供 locale 解析与主题取色。
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;

    /// @brief 测量并按可用宽度折行：记录可视行、行首码点与行高供绘制/命中复用。
    /// @param c 上游约束：以 `max` 为可用宽度做软换行，无界约束时退化为自然尺寸。
    /// @param ctx 构建上下文：提供 locale（解析显示文本）与字号缩放设置。
    /// @return 文本在该宽度下所需的尺寸（dp，经约束夹取）。
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;

  private:
    /// @brief 无选区哨兵：sel_end_ 取此值时表示当前没有选区。
    /// （命名带 `AURORA_` 前缀：类内 `static constexpr` 走全局常量口径，见 `.clang-tidy`。）
    static constexpr size_t AURORA_NO_SEL = static_cast<size_t>(-1);

    size_t sel_start_ = 0;  ///< 选区起点（含入的码点下标：该字符被选中）
    size_t sel_end_ = AURORA_NO_SEL;  ///< 选区终点（含入的码点下标；= AURORA_NO_SEL 表示无选区）
    size_t caret_ = 0;  ///< 光标（caret 位置，0..码点数；用于键盘导航）
    bool selecting_ = false;  ///< 是否正在拖选
    std::string display_text_;  ///< 最近一次绘制所用显示文本（命中测试/选区使用）
    std::string cached_resolved_text_;  ///< 缓存的 resolved_text 结果（避免每帧重复解析）
    bool resolved_dirty_ = true;  ///< resolved 缓存是否需重新计算
    std::vector<std::string> lines_;  ///< 最近一次布局所得折行结果（绘制复用）
    std::vector<size_t> line_cp_start_;  ///< 每个可视行首字符在 display_text_ 中的码点下标
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
    /// @brief 生效字体：补齐非法字号并施加无障碍字号缩放（倍率取进程级设置）。
    static auto effective_font(const Font &base) -> Font;
    /// @brief 生效字体（带上下文）：字号倍率优先取 `Environment` 注入的 `AccessibilitySettings`，
    ///        未注入时回落进程级设置——即「树级覆盖 > 进程默认」。
    static auto effective_font(const Font &base, const BuildContext &ctx) -> Font;
    /// @brief 生效书写方向：控件显式属性 > `Environment` 注入 > 进程级（host_set）；
    ///        无任何显式来源返回 nullopt（shaping 按内容 guess，默认行为不变）。
    ///        命中测试路径无 `BuildContext`，用无 ctx 版本（回落进程级）。
    [[nodiscard]] auto effective_direction(const BuildContext &ctx) const -> std::optional<TextDirection>;
    [[nodiscard]] auto effective_direction() const -> std::optional<TextDirection>;
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
    /// @param line 可视行文本（UTF-8，空格按 1 码点计）。
    /// @param f 该行走的有效字体（已施加字号缩放）。
    /// @param opts 文本布局选项（与绘制同源，保证度量一致）。
    /// @param avail 该行可用宽度 dp。
    /// @return 逐词布局项（含行内码点区间与均分后的 x）；不足两词返回空向量。
    static auto justify_layout(const std::string &line, const Font &f, const render::TextLayoutOpts &opts, float avail)
        -> std::vector<JustifiedWord>;
    /// @brief 可视行 li 是否按两端对齐绘制（Justify 且多行且非末行，与 on_paint 判定一致）。
    /// @param li 可视行下标（0 基，对应 `lines_`）。
    /// @return 该行需要走 `justify_layout` 均分路径时为 true。
    [[nodiscard]] auto is_justified_line(size_t li) const -> bool;
    /// @brief 行内 caret x（相对行左缘）：Justify 行按均分布局取词位（行尾 = 行右缘 avail），
    ///        其余行走 FontEngine::display_caret_x（按 paint_scale_ 推导物理 DPI 的前缀
    ///        extent，逐字符与实绘字形对齐；scale=1 退化为 caret_x）；选区高亮与绘制像素一一对应。
    /// @param li 可视行下标。
    /// @param cp_in_line caret 在该行内的码点位置（0..行长度）。
    /// @param f 有效字体。
    /// @param opts 文本布局选项。
    /// @param avail 该行可用宽度 dp（Justify 行尾 caret 即取此值）。
    /// @return caret 相对行左缘的 x（dp）。
    [[nodiscard]] auto line_caret_x(size_t li, size_t cp_in_line, const Font &f, const render::TextLayoutOpts &opts,
                                    float avail) const -> float;
    /// @brief 行内命中测试：返回 {caret 位置, 含头含尾字符下标}（行内码点）；
    ///        Justify 行按均分布局反解（词间拉伸间隙整体归属其空格字符），
    ///        其余行走实显命中（FontEngine::display_hit_test_char*，与实绘字形逐字符对齐）。
    /// @param li 可视行下标。
    /// @param x 点击点相对行左缘的 x（dp）。
    /// @param f 有效字体。
    /// @param opts 文本布局选项。
    /// @param avail 该行可用宽度 dp。
    /// @return `{caret 位置, 选中字符下标}` 二元组（均为行内码点下标，含头含尾）。
    [[nodiscard]] auto line_hit_test(size_t li, float x, const Font &f, const render::TextLayoutOpts &opts,
                                     float avail) const -> std::pair<size_t, size_t>;
};

}  // namespace aurora
