#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/types.h"
#include "aurora/environment/environment.h"
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
    std::string text;
    Font font;
    Color color;
    float width = 0.0F;
};

/// @brief 一行（含若干单词），已计算 y 偏移与行高。
struct RichLine {
    float y = 0.0F;
    float height = 0.0F;
    std::vector<RichWord> words;
};

/// @brief 按空格拆分单词（保留空行安全）。
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

/**
 * @brief 把片段序列按**单词贪心换行**，返回行集合（已计算每行的 y 与 height）。
 *
 * 在 `maxWidth` 约束内逐词放置；行内非首词前补一个空格宽（按该词所属片段字号）。
 * 行高取该行各片段高度最大值。算法纯函数、无随机性 → 确定性（满足 specification/03-layout-render.md §2.3 两阶段布局）。
 */
inline auto layout_rich_text(const std::vector<TextSpan> &spans, float max_width, const Locale &loc)
    -> std::vector<detail::RichLine> {
    std::vector<detail::RichLine> lines;
    detail::RichLine cur;
    float cur_w = 0.0F;

    auto space_w = [](const Font &f) -> float { return render::FontEngine::measure_width(" ", f); };

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

    float y = 0.0F;
    for (auto &line : lines) {
        line.y = y;
        y += line.height;
    }
    return lines;
}

/// @brief 富文本测量：返回在 `maxWidth` 约束下所需的内容尺寸。
inline auto measure_rich_text(const std::vector<TextSpan> &spans, float max_width, const Locale &loc = Locale{})
    -> Size {
    const std::vector<detail::RichLine> lines = layout_rich_text(spans, max_width, loc);
    float w = 0.0F;
    float h = 0.0F;
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

/**
 * @brief 富文本控件（叶控件）：按 `TextSpan` 序列渲染带样式的文本。
 *
 * 文本经 `defaultStringTable` + 当前 `Locale` 解析（支持 i18n）。布局采用确定性贪心换行，
 * 整串宽度/高度由 `measureRichText` 决定；值来源支持 `Reactive<std::vector<TextSpan>>`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class RichText : public LeafWidget {
  public:
    RichText() = default;
    explicit RichText(Reactive<std::vector<TextSpan>> spans) : spans_(std::move(spans)) {}

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&spans_); }

    [[nodiscard]] auto type_name() const -> const char * override { return "RichText"; }

    /// @brief 运行时自描述（规格附录 B）。
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
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        std::string all;
        for (const auto &s : spans_.get()) {
            all += s.text.text;
        }
        props["text"] = all;
    }

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
