#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/types.h"
#include "aurora/environment/build_context.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/i18n/string_table.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/state/reactive.h"
#include "aurora/state/signal_view.h"
#include "aurora/widget/text_span.h"
#include "aurora/widget/widget.h"

namespace aurora {

namespace detail {

/// @brief 富文本布局后的单词（已绑定样式与测量宽度）。
struct RichWord {
    std::string text;  ///< 单词文本（已完成 i18n 解析）
    Font font;  ///< 所属片段的样式字体
    Color color;  ///< 所属片段的渲染色
    float width = 0.0F;  ///< 该单词按 font 实测的宽度（dp）
};

/// @brief 一行（含若干单词），已计算 y 偏移与行高。
struct RichLine {
    float y = 0.0F;  ///< 行顶相对内容区顶部的偏移（dp，按行高累加）
    float height = 0.0F;  ///< 行高：该行各片段字体高度的最大值
    std::vector<RichWord> words;  ///< 行内单词序列（按放置顺序）
};

/// @brief 按空格拆分单词（保留空行安全）。
/// @param s 待切分文本；连续空格视作一个分隔，行首/尾空格丢弃
/// @return 非空单词序列；全空白输入返回空向量
inline auto split_words(const std::string &s) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::string cur;
    for (const char ch : s) {
        if (ch == ' ') {
            if (!cur.empty()) {
                out.push_back(std::move(cur));
                cur.clear();
            }
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) {
        out.push_back(std::move(cur));
    }
    return out;
}

}  // namespace detail

/// @brief 把片段序列按**单词贪心换行**，返回行集合（已计算每行的 y 与 height）。
///
/// 在 `maxWidth` 约束内逐词放置；行内非首词前补一个空格宽（按该词所属片段字号）。
/// 行高取该行各片段高度最大值。算法纯函数、无随机性 → 确定性（满足 specification/03-layout-render.md §2.3
/// 两阶段布局）。
/// @param spans     文本片段序列；每片自带文本/字体/颜色
/// @param max_width 换行约束宽度（dp），行宽（含空格间隙）超出即另起一行
/// @param loc       i18n locale，片段文本经 `default_string_table()` 解析时用
/// @return 布局完成的行集合：每行含单词序列（带实测宽度）与 y/height；空输入至少返回一行
inline auto layout_rich_text(const std::vector<TextSpan> &spans, float max_width, const Locale &loc)
    -> std::vector<detail::RichLine> {
    std::vector<detail::RichLine> lines;  // 已封行的结果集
    detail::RichLine cur;  // 正在填充的当前行
    float cur_w = 0.0F;  // 当前行已占用宽度（含空格间隙）

    auto space_w = [](const Font &f) -> float {
        return render::FontEngine::measure_width(" ", f);
    };  // 该字号下空格宽度

    for (const auto &span : spans) {
        const std::string s = span.text.resolve(&default_string_table(), loc);
        const std::vector<std::string> words = detail::split_words(s);
        for (const auto &word : words) {
            const float w = render::FontEngine::measure_width(word, span.font);
            const bool need_space = !cur.words.empty();
            const float gap = need_space ? space_w(span.font) : 0.0F;
            if (need_space && (cur_w + gap + w > max_width)) {
                lines.push_back(std::move(cur));
                cur = detail::RichLine{};
                const float w2 = render::FontEngine::measure_width(word, span.font);
                cur.words.push_back(
                    detail::RichWord{.text = word, .font = span.font, .color = span.color, .width = w2});
                cur.height = render::FontEngine::measure_height(span.font);
                cur_w = w2;
            } else {
                if (need_space) {
                    cur_w += gap;
                }
                cur.words.push_back(detail::RichWord{.text = word, .font = span.font, .color = span.color, .width = w});
                cur.height = std::max(cur.height, render::FontEngine::measure_height(span.font));
                cur_w += w;
            }
        }
    }
    if (!cur.words.empty() || lines.empty()) {
        lines.push_back(std::move(cur));
    }

    float y = 0.0F;  // 行顶累计偏移游标
    for (auto &line : lines) {
        line.y = y;
        y += line.height;
    }
    return lines;
}

/// @brief 富文本测量：返回在 `maxWidth` 约束下所需的内容尺寸。
/// @param spans     文本片段序列（同 `layout_rich_text`）
/// @param max_width 换行约束宽度（dp）
/// @param loc       i18n locale，缺省 `Locale{}`
/// @return 宽度=最宽行（含空格间隙），高度=各行高之和
inline auto measure_rich_text(const std::vector<TextSpan> &spans, float max_width, const Locale &loc = Locale{})
    -> Size {
    const std::vector<detail::RichLine> lines = layout_rich_text(spans, max_width, loc);
    float w = 0.0F;  // 最宽行的实测宽度
    float h = 0.0F;  // 行高累加的总高度
    for (const auto &line : lines) {
        float lw = 0.0F;
        for (std::size_t i = 0; i < line.words.size(); ++i) {
            lw += line.words[i].width;
            if (i + 1 < line.words.size()) {
                lw += render::FontEngine::measure_width(" ", line.words[i].font);
            }
        }
        w = std::max(w, lw);
        h += line.height;
    }
    return Size{.width = w, .height = h};
}

/// @brief 富文本控件（叶控件）：按 `TextSpan` 序列渲染带样式的文本。
///
/// 文本经 `default_string_table()` + 当前 `Locale` 解析（支持 i18n）。布局采用确定性贪心换行，
/// 整串宽度/高度由 `measureRichText` 决定；值来源支持 `Reactive<std::vector<TextSpan>>`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
class RichText : public LeafWidget {
  public:
    /// @brief 纯展示件默认不是 Tab 停点：只有挂上点击 / 手势 / 菜单 / 滚动 / 键盘认领时
    ///        才可聚焦（覆写基类 public virtual；分级默认见 specification/05 §4.2）。
    /// @return 修饰链携带交互语义时为 true
    [[nodiscard]] auto wants_focus() const -> bool override { return has_input_semantics(); }
    /// @brief 默认构造：空片段序列，渲染空白行。
    RichText() = default;
    /// @brief 构造富文本：接管响应式片段序列。
    /// @param spans 片段序列的响应式值（文本/字体/颜色），移入 `spans_`
    explicit RichText(Reactive<std::vector<TextSpan>> spans) : spans_(std::move(spans)) {}

    /// @brief 登记需订阅的信号视图：片段序列 `spans_`。
    /// @param out 末尾追加 `&spans_`（内容变更驱动重排重绘）
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&spans_); }

    /// @brief 类型名：供注册表/日志/序列化按名分派。
    /// @return 静态字符串 `"RichText"`
    [[nodiscard]] auto type_name() const -> const char * override { return "RichText"; }

    /// @brief 无障碍名称：拼接全部 span 的解析文本（无障碍视图不区分富文本样式）。
    /// @note Side-effects: reads i18n table
    /// @return 各片段按默认表与缺省 Locale 解析后的顺序拼接串
    [[nodiscard]] auto accessibility_label() const -> std::string override {
        std::string out;  // 拼接结果缓冲
        for (const auto &s : spans_.get()) {
            out += s.text.resolve(&default_string_table(), Locale{});
        }
        return out;
    }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return `RichText` 的 `WidgetDescriptor`（text/width/height/show，无事件、无子节点）
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "RichText",
            .properties =
                {
                    {.name = "text",
                     .type = "string",
                     .default_value = "\"\"",
                     .required = false,
                     .note = "纯文本内容（序列化用）",
                     .json_type = "string"},
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
            .events = {},
            .children_policy = "none",
            .examples = {"au::RichText(au::Reactive<std::vector<au::TextSpan>>{ ... })"},
        };
    }
    /// @brief 实例级自描述：无实例差异描述，直接转发 `describe_static()`。
    /// @return 与 `describe_static()` 相同的静态描述表
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 序列化：将全部片段文本拼接为单个 `text` 字符串输出（样式不序列化）。
    /// @param props 输出目标 JSON 对象
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);  // 先由基类写入通用属性
        std::string all;  // 各片段原文拼接缓冲
        for (const auto &s : spans_.get()) {
            all += s.text.text;
        }
        props["text"] = all;
    }

    /// @brief 反序列化：`text` 键存在时重建为单一片段序列（富文本样式无法由 JSON 恢复）。
    /// @param props 序列化属性对象
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("text")) {
            spans_ = Reactive{std::vector{TextSpan{.text = LocalizedString{props["text"].get<std::string>()}}}};
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        const Locale loc = (ctx.environment<Locale>() != nullptr) ? *ctx.environment<Locale>() : Locale{};
        lines_ = layout_rich_text(spans_.get(), c.max.width, loc);
        float w = 0.0F;
        float h = 0.0F;
        for (const auto &line : lines_) {
            float lw = 0.0F;
            for (std::size_t i = 0; i < line.words.size(); ++i) {
                lw += line.words[i].width;
                if (i + 1 < line.words.size()) {
                    lw += render::FontEngine::measure_width(" ", line.words[i].font);
                }
            }
            w = std::max(w, lw);
            h += line.height;
        }
        return c.constrain(Size{.width = w, .height = h});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (lines_.empty()) {
            const Locale loc = (ctx.environment<Locale>() != nullptr) ? *ctx.environment<Locale>() : Locale{};
            lines_ = layout_rich_text(spans_.get(), bounds.size.width, loc);
        }
        float y = bounds.origin.y;
        for (const auto &line : lines_) {
            float x = bounds.origin.x;
            for (const auto &wd : line.words) {
                if (!wd.text.empty()) {
                    p.draw_text(
                        Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = wd.width, .height = line.height}},
                        wd.text, wd.font, wd.color);
                }
                x += wd.width + render::FontEngine::measure_width(" ", wd.font);
            }
            y += line.height;
        }
    }

  private:
    Reactive<std::vector<TextSpan>> spans_;
    std::vector<detail::RichLine> lines_;
};

}  // namespace aurora
