#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/types.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 分段控件。
 *
 * `SegmentedControl<T>{segments, selected, on_change}` — 互斥分段选择。
 *
 * 可定制性（对标 SwiftUI `Picker(.segmented)`、Flutter `SegmentedButton`）：
 * - `active_color`（选中段填充）未显式设置时跟随主题 `Theme::primary`；
 * - 文本色 / 选中文本色 / 边框色 / 字号 / 圆角均可配；
 * - 悬停段淡色反馈；禁用（`set_enabled(false)`）灰化并忽略点击。
 *
 * 继承扩展点（protected 虚函数）：`paint_segment`（单个分段）。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class SegmentedControl : public LeafWidget {
  public:
    SegmentedControl() = default;
    explicit SegmentedControl(std::vector<std::string> segments, int selected = 0)
        : segments_(std::move(segments)), selected_(selected) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "SegmentedControl"; }
    [[nodiscard]] auto segments() const -> const std::vector<std::string> & { return segments_; }
    [[nodiscard]] auto selected() const -> int { return selected_; }
    auto set_selected(int i) -> SegmentedControl & {
        selected_ = i;
        mark_needs_paint();
        return *this;
    }
    auto set_on_change(std::function<void(int)> cb) -> SegmentedControl & {
        on_change_ = std::move(cb);
        return *this;
    }

    /// @brief 设置选中段填充色（链式）。不调用则跟随主题 `Theme::primary`。
    auto set_active_color(Color c) -> SegmentedControl & {
        active_color_ = c;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置未选中段文本色（链式）。
    auto set_text_color(Color c) -> SegmentedControl & {
        text_color_ = c;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置选中段文本色（链式）。
    auto set_selected_text_color(Color c) -> SegmentedControl & {
        selected_text_color_ = c;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置外框边框色（链式）。
    auto set_border_color(Color c) -> SegmentedControl & {
        border_color_ = c;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置字号 pt（链式）。
    auto set_font_size(float s) -> SegmentedControl & {
        font_size_ = s > 0.0F ? s : 14.0F;
        mark_needs_layout();
        return *this;
    }
    /// @brief 设置外框圆角半径 dp（链式；0 = 直角）。
    auto set_corner_radius(float r) -> SegmentedControl & {
        corner_radius_ = r >= 0.0F ? r : 0.0F;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置是否启用（链式）；禁用态灰化绘制并忽略点击。
    auto set_enabled(bool v) -> SegmentedControl & {
        enabled_ = v;
        mark_needs_paint();
        return *this;
    }
    [[nodiscard]] auto enabled() const -> bool { return enabled_; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "SegmentedControl",
            .properties =
                {
                    {.name = "selected",
                     .type = "int",
                     .default_value = "0",
                     .required = false,
                     .note = "选中序号",
                     .json_type = "integer",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "active_color",
                     .type = "Color",
                     .default_value = "theme.primary",
                     .required = false,
                     .note = "选中段填充色（缺省跟随主题 primary）",
                     .json_type = "array"},
                    {.name = "text_color",
                     .type = "Color",
                     .default_value = "Color::black()",
                     .required = false,
                     .note = "未选中段文本色",
                     .json_type = "array"},
                    {.name = "selected_text_color",
                     .type = "Color",
                     .default_value = "Color::white()",
                     .required = false,
                     .note = "选中段文本色",
                     .json_type = "array"},
                    {.name = "border_color",
                     .type = "Color",
                     .default_value = "{200,200,200,255}",
                     .required = false,
                     .note = "外框边框色",
                     .json_type = "array"},
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
                     .default_value = "6.0",
                     .required = false,
                     .note = "外框圆角半径(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "enabled",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "是否可交互（禁用灰化）",
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
            .events = {"on_change"},
            .children_policy = "none",
            .examples = {R"(au::SegmentedControl({"Day", "Week", "Month"}, 0))"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!enabled_) {
            e.is_handled = true;  // 禁用态吞掉点击（不冒泡），不切换
            return;
        }
        if (e.action == MouseAction::Press && e.button == MouseButton::Left) {
            const Font f{.size_pt = font_size_};
            float x = 0.0F;
            for (size_t i = 0; i < segments_.size(); ++i) {
                const float sw = render::FontEngine::measure_width(segments_[i], f) + 24.0F;
                if (e.local_position.x >= x && e.local_position.x < x + sw) {
                    if (std::cmp_not_equal(selected_, i)) {
                        selected_ = static_cast<int>(i);
                        if (on_change_) {
                            on_change_(selected_);
                        }
                        mark_needs_paint();
                    }
                    break;
                }
                x += sw;
            }
            e.is_handled = true;
        }
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["selected"] = selected_;
        Json segs = Json::array();
        for (const auto &s : segments_) {
            segs.push_back(s);
        }
        props["segments"] = segs;
        if (active_color_.has_value()) {
            props["active_color"] = color_to_json(*active_color_);  // 未设置不输出：保留「跟随主题」语义
        }
        props["text_color"] = color_to_json(text_color_);
        props["selected_text_color"] = color_to_json(selected_text_color_);
        props["border_color"] = color_to_json(border_color_);
        props["font_size"] = font_size_;
        props["corner_radius"] = corner_radius_;
        props["enabled"] = enabled_;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("selected")) {
            selected_ = props["selected"].get<int>();
        }
        if (props.contains("segments") && props["segments"].is_array()) {
            segments_.clear();
            for (const auto &s : props["segments"]) {
                segments_.push_back(s.get<std::string>());
            }
        }
        if (props.contains("active_color")) {
            active_color_ = json_to_color(props["active_color"]);
        }
        if (props.contains("text_color")) {
            text_color_ = json_to_color(props["text_color"]);
        }
        if (props.contains("selected_text_color")) {
            selected_text_color_ = json_to_color(props["selected_text_color"]);
        }
        if (props.contains("border_color")) {
            border_color_ = json_to_color(props["border_color"]);
        }
        if (props.contains("font_size")) {
            font_size_ = props["font_size"].get<float>();
        }
        if (props.contains("corner_radius")) {
            corner_radius_ = props["corner_radius"].get<float>();
        }
        if (props.contains("enabled")) {
            enabled_ = props["enabled"].get<bool>();
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        const Font f{.size_pt = font_size_};
        const float seg_h = render::FontEngine::measure_height(f) + 12.0F;
        float total_w = 0.0F;
        for (const auto &s : segments_) {
            total_w += render::FontEngine::measure_width(s, f) + 24.0F;
        }
        return c.constrain(Size{.width = total_w, .height = seg_h});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        const Font f{.size_pt = font_size_};
        // 状态色解析：显式设置优先，否则跟随主题 primary；禁用态统一灰化。
        Color accent = active_color_.value_or(inherit_theme(ctx).primary);
        Color text = text_color_;
        Color sel_text = selected_text_color_;
        Color border = border_color_;
        if (!enabled_) {
            accent = Color{176, 176, 180, 255};
            text = Color{168, 168, 172, 255};
            sel_text = Color{240, 240, 242, 255};
            border = Color{215, 215, 219, 255};
        }
        // 外框（圆角裁剪保证选中段填充不溢出圆角）
        if (corner_radius_ > 0.0F) {
            p.push_clip_rounded(bounds, corner_radius_);
        }
        float x = bounds.origin.x;
        for (size_t i = 0; i < segments_.size(); ++i) {
            const float sw = render::FontEngine::measure_width(segments_[i], f) + 24.0F;
            const Rect seg{.origin = Point{.x = x, .y = bounds.origin.y},
                           .size = Size{.width = sw, .height = bounds.size.height}};
            paint_segment(p, i, seg, std::cmp_equal(i, selected_), f, accent, text, sel_text);
            x += sw;
        }
        if (corner_radius_ > 0.0F) {
            p.pop_clip();
        }
        if (corner_radius_ > 0.0F) {
            p.draw_rounded_border(bounds, corner_radius_, 1.0F, border);
        } else {
            p.draw_rect(bounds, border);
        }
    }

    /// @brief 继承扩展点：绘制单个分段（选中填充强调色，未选中透明）。
    virtual auto paint_segment(Painter &p, size_t index, const Rect &seg, bool selected, const Font &f, Color accent,
                               Color text, Color sel_text) -> void {
        if (selected) {
            p.fill_rect(seg, accent);
        }
        p.draw_text(Rect{.origin = Point{.x = seg.origin.x + 12.0F, .y = seg.origin.y + 6.0F},
                         .size = Size{.width = seg.size.width - 24.0F, .height = seg.size.height}},
                    segments_[index], f, selected ? sel_text : text);
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    std::vector<std::string> segments_;
    int selected_ = 0;
    std::function<void(int)> on_change_;
    std::optional<Color> active_color_;  ///< 选中段填充色；空 = 跟随主题 primary
    Color text_color_ = Color::black();  ///< 未选中段文本色
    Color selected_text_color_ = Color::white();  ///< 选中段文本色
    Color border_color_ = Color{200, 200, 200, 255};  ///< 外框边框色
    float font_size_ = 14.0F;  ///< 字号 pt
    float corner_radius_ = 6.0F;  ///< 外框圆角半径 dp；0 = 直角
    bool enabled_ = true;  ///< 禁用态灰化并忽略点击
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

}  // namespace aurora
