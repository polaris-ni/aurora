#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 下拉选择器：点击展开选项列表，选择后收起。
 *
 * 选项为字符串列表（序列化友好、AI 可枚举）；泛型选项经 `on_change(index)`
 * 回调在调用侧映射。选中序号存于响应式 `selected()`。
 *
 * 可定制性（对标 Qt QComboBox / Flutter DropdownButton）：
 * - 颜色：主框背景/边框/文本/箭头；选中高亮 `accent_color` 未显式设置时跟随主题 `Theme::primary`；
 * - 尺寸：主框高（`set_box_height`）、选项行高（`set_item_height`）、字号、圆角；
 * - 空选项时显示 `placeholder`；禁用（`set_enabled(false)`）灰化并忽略点击。
 *
 * 继承扩展点（protected 虚函数）：`paint_box`（主框）与 `paint_item`（单个下拉选项行）。
 *
 * 对标 Qt `QComboBox`、WPF `ComboBox`、Flutter `DropdownButton`、SwiftUI `Picker`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Dropdown : public Widget {
  public:
    Dropdown() = default;
    explicit Dropdown(std::vector<std::string> options, int initial = 0) : options_(std::move(options)) {
        const int max_idx = static_cast<int>(options_.size()) - 1;
        selected_.set(std::clamp(initial, 0, std::max(0, max_idx)));
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Dropdown"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&selected_); }

    [[nodiscard]] auto option_count() const -> std::size_t { return options_.size(); }
    [[nodiscard]] auto selected() -> State<int> & { return selected_; }
    [[nodiscard]] auto selected_index() const -> int { return selected_.get(); }
    [[nodiscard]] auto selected_text() const -> std::string {
        const int i = selected_.get();
        if (i < 0 || std::cmp_greater_equal(i, options_.size())) {
            return {};
        }
        return options_[static_cast<std::size_t>(i)];
    }
    [[nodiscard]] auto is_open() const -> bool { return is_open_; }

    /// @brief 选择指定序号（越界忽略；触发 on_change）。
    auto select(int index) -> void {
        if (index >= 0 && std::cmp_less(index, options_.size()) && index != selected_.get()) {
            selected_.set(index);
            mark_needs_paint();
            if (on_change_) {
                on_change_(index);
            }
        }
    }

    /// @brief 展开/收起下拉。
    auto set_open(bool open) -> void {
        is_open_ = open;
        mark_needs_paint();
    }

    /// @brief 设置选择回调（链式）。
    auto set_on_change(std::function<void(int)> cb) -> Dropdown & {
        on_change_ = std::move(cb);
        return *this;
    }

    /// @brief 设置空选项占位文本（链式）。
    auto set_placeholder(std::string s) -> Dropdown & {
        placeholder_ = std::move(s);
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置选中高亮色（链式）。不调用则跟随主题 `Theme::primary`。
    auto set_accent_color(Color c) -> Dropdown & {
        accent_color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置主框背景色（链式）。
    auto set_box_color(Color c) -> Dropdown & {
        box_color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置主框/面板边框色（链式）。
    auto set_border_color(Color c) -> Dropdown & {
        border_color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置选项文本色（链式）。
    auto set_text_color(Color c) -> Dropdown & {
        text_color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置箭头颜色（链式）。
    auto set_arrow_color(Color c) -> Dropdown & {
        arrow_color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置主框高度 dp（链式）。
    auto set_box_height(float h) -> Dropdown & {
        box_height_ = h > 0.0F ? h : 30.0F;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置选项行高 dp（链式）。
    auto set_item_height(float h) -> Dropdown & {
        item_height_ = h > 0.0F ? h : 26.0F;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置字号 pt（链式）。
    auto set_font_size(float s) -> Dropdown & {
        font_size_ = s > 0.0F ? s : 13.0F;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置主框圆角半径 dp（链式；0 = 直角）。
    auto set_corner_radius(float r) -> Dropdown & {
        corner_radius_ = r >= 0.0F ? r : 0.0F;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置是否启用（链式）；禁用态灰化绘制并忽略点击。
    auto set_enabled(bool v) -> Dropdown & {
        enabled_ = v;
        mark_needs_paint();
        return *this;
    }
    [[nodiscard]] auto enabled() const -> bool { return enabled_; }

    /// @brief 点击交互：框内点击开合；展开时点击选项选中并收起。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!enabled_) {
            e.is_handled = true;  // 禁用态吞掉点击（不冒泡），不开合
            return;
        }
        if (e.action != MouseAction::Press) {
            Widget::on_pointer_event(e);
            return;
        }
        // 主框区域：开合切换
        if (e.local_position.y < box_height_) {
            is_open_ = !is_open_;
            mark_needs_paint();
            e.is_handled = true;
            return;
        }
        // 展开中的选项列表
        if (is_open_) {
            const int idx = static_cast<int>((e.local_position.y - box_height_) / item_height_);
            if (idx >= 0 && std::cmp_less(idx, options_.size())) {
                select(idx);
            }
            is_open_ = false;
            mark_needs_paint();
            e.is_handled = true;
            return;
        }
        Widget::on_pointer_event(e);
    }

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    /// @brief 悬停反馈：主框边框高亮为强调色。
    auto on_hover_change(bool entered) -> void override {
        Widget::on_hover_change(entered);
        if (enabled_) {
            mark_needs_paint();
        }
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        Json opts = Json::array();
        for (const auto &o : options_) {
            opts.push_back(o);
        }
        props["options"] = opts;
        props["selected_index"] = selected_.get();
        if (!placeholder_.empty()) {
            props["placeholder"] = placeholder_;
        }
        if (accent_color_.has_value()) {
            props["accent_color"] = color_to_json(*accent_color_);  // 未设置不输出：保留「跟随主题」语义
        }
        props["box_color"] = color_to_json(box_color_);
        props["border_color"] = color_to_json(border_color_);
        props["text_color"] = color_to_json(text_color_);
        props["arrow_color"] = color_to_json(arrow_color_);
        props["box_height"] = box_height_;
        props["item_height"] = item_height_;
        props["font_size"] = font_size_;
        props["corner_radius"] = corner_radius_;
        props["enabled"] = enabled_;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("options") && props["options"].is_array()) {
            options_.clear();
            for (const auto &o : props["options"]) {
                options_.push_back(o.get<std::string>());
            }
        }
        if (props.contains("selected_index")) {
            selected_.set(props["selected_index"].get<int>());
        }
        if (props.contains("placeholder")) {
            placeholder_ = props["placeholder"].get<std::string>();
        }
        if (props.contains("accent_color")) {
            accent_color_ = json_to_color(props["accent_color"]);
        }
        if (props.contains("box_color")) {
            box_color_ = json_to_color(props["box_color"]);
        }
        if (props.contains("border_color")) {
            border_color_ = json_to_color(props["border_color"]);
        }
        if (props.contains("text_color")) {
            text_color_ = json_to_color(props["text_color"]);
        }
        if (props.contains("arrow_color")) {
            arrow_color_ = json_to_color(props["arrow_color"]);
        }
        if (props.contains("box_height")) {
            box_height_ = props["box_height"].get<float>();
        }
        if (props.contains("item_height")) {
            item_height_ = props["item_height"].get<float>();
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
        // 主框宽度 = 最长选项宽 + 箭头区；下拉为覆盖绘制不占布局
        Font f;
        f.size_pt = font_size_;
        float w = 80.0F;
        for (const auto &o : options_) {
            w = std::max(w, render::FontEngine::measure_width(o, f) + (AURORA_PAD * 2.0F) + AURORA_ARROW_ZONE);
        }
        if (options_.empty() && !placeholder_.empty()) {
            w = std::max(w,
                         render::FontEngine::measure_width(placeholder_, f) + (AURORA_PAD * 2.0F) + AURORA_ARROW_ZONE);
        }
        return c.constrain(Size{.width = w, .height = box_height_});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        Font f;
        f.size_pt = font_size_;
        // 状态色解析：显式设置优先，否则跟随主题 primary；禁用态统一灰化。
        Color accent = accent_color_.value_or(inherit_theme(ctx).primary);
        Color box = box_color_;
        Color border = border_color_;
        Color text = text_color_;
        Color arrow = arrow_color_;
        if (!enabled_) {
            accent = Color{176, 176, 180, 255};
            box = Color{235, 235, 237, 255};
            border = Color{215, 215, 219, 255};
            text = Color{168, 168, 172, 255};
            arrow = Color{190, 190, 194, 255};
        } else if (hovered() || is_open_) {
            border = accent;  // 悬停/展开时边框高亮
        }

        const Rect box_rect{.origin = bounds.origin, .size = Size{.width = bounds.size.width, .height = box_height_}};
        paint_box(p, box_rect, f, box, border, text, arrow);

        // 下拉选项面板
        if (is_open_) {
            const float h = static_cast<float>(options_.size()) * item_height_;
            const Rect drop{.origin = Point{.x = box_rect.origin.x, .y = box_rect.origin.y + box_height_},
                            .size = Size{.width = box_rect.size.width, .height = h}};
            p.draw_shadow(drop, 0.0F, 2.0F, 8.0F, Color(0, 0, 0, 48));
            p.fill_rect(drop, box_color_);
            p.draw_rect(drop, border_color_);
            float y = drop.origin.y;
            for (std::size_t i = 0; i < options_.size(); ++i) {
                const Rect item{.origin = Point{.x = drop.origin.x, .y = y},
                                .size = Size{.width = drop.size.width, .height = item_height_}};
                paint_item(p, i, item, std::cmp_equal(i, selected_.get()), f, accent, text);
                y += item_height_;
            }
        }
    }

    /// @brief 继承扩展点：绘制主框（背景/边框/选中文本/箭头）。
    virtual auto paint_box(Painter &p, const Rect &box, const Font &f, Color bg, Color border, Color text, Color arrow)
        -> void {
        if (corner_radius_ > 0.0F) {
            p.fill_rounded_rect(box, corner_radius_, bg);
            p.draw_rounded_border(box, corner_radius_, 1.0F, border);
        } else {
            p.fill_rect(box, bg);
            p.draw_rect(box, border);
        }
        const std::string shown = options_.empty() ? placeholder_ : selected_text();
        const Rect text_box{
            .origin = Point{.x = box.origin.x + AURORA_PAD, .y = box.origin.y + ((box.size.height - 14.0F) * 0.5F)},
            .size = Size{.width = box.size.width - (AURORA_PAD * 2.0F) - AURORA_ARROW_ZONE, .height = 14.0F}};
        p.draw_text(text_box, shown, f, text);
        // 箭头（简化为 "v"/"^"）
        const Rect arrow_box{.origin = Point{.x = box.origin.x + box.size.width - AURORA_ARROW_ZONE,
                                             .y = box.origin.y + ((box.size.height - 14.0F) * 0.5F)},
                             .size = Size{.width = AURORA_ARROW_ZONE - 4.0F, .height = 14.0F}};
        p.draw_text(arrow_box, is_open_ ? "^" : "v", f, arrow);
    }

    /// @brief 继承扩展点：绘制单个下拉选项行（选中项铺强调色淡底 + 强调色文本）。
    virtual auto paint_item(Painter &p, std::size_t index, const Rect &item, bool selected, const Font &f, Color accent,
                            Color text) -> void {
        if (selected) {
            p.fill_rect(item, accent.with_alpha(30));
        }
        const Rect item_box{
            .origin = Point{.x = item.origin.x + AURORA_PAD, .y = item.origin.y + 5.0F},
            .size = Size{.width = item.size.width - (AURORA_PAD * 2.0F), .height = item.size.height - 10.0F}};
        p.draw_text(item_box, options_[index], f, selected ? accent : text);
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        // 主框
        if (local.y < box_height_ &&
            Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = bounds.size.width, .height = box_height_}}
                .contains(local)) {
            return this;
        }
        // 展开的下拉区
        if (is_open_) {
            const float h = static_cast<float>(options_.size()) * item_height_;
            const Rect drop{.origin = Point{.x = 0.0F, .y = box_height_},
                            .size = Size{.width = bounds.size.width, .height = h}};
            if (drop.contains(local)) {
                return this;
            }
        }
        return nullptr;
    }

    static constexpr float AURORA_PAD = 10.0F;  ///< 文本内边距(dp)
    static constexpr float AURORA_ARROW_ZONE = 24.0F;  ///< 箭头区宽度(dp)

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    std::vector<std::string> options_;
    State<int> selected_{0};
    bool is_open_ = false;
    std::function<void(int)> on_change_;
    std::string placeholder_;  ///< 空选项时的占位文本
    std::optional<Color> accent_color_;  ///< 选中高亮色；空 = 跟随主题 primary
    Color box_color_ = Color{255, 255, 255, 255};  ///< 主框背景色
    Color border_color_ = Color{200, 200, 205, 255};  ///< 主框/面板边框色
    Color text_color_ = Color{30, 30, 30, 255};  ///< 选项文本色
    Color arrow_color_ = Color{120, 120, 125, 255};  ///< 箭头颜色
    float box_height_ = 30.0F;  ///< 主框高度 dp
    float item_height_ = 26.0F;  ///< 选项行高 dp
    float font_size_ = 13.0F;  ///< 字号 pt
    float corner_radius_ = 4.0F;  ///< 主框圆角半径 dp；0 = 直角
    bool enabled_ = true;  ///< 禁用态灰化并忽略点击
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

inline auto Dropdown::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "Dropdown",
        .properties =
            {
                {.name = "options",
                 .type = "vector<string>",
                 .default_value = "[]",
                 .required = true,
                 .note = "选项列表",
                 .json_type = "array"},
                {.name = "selected_index",
                 .type = "int",
                 .default_value = "0",
                 .required = false,
                 .note = "当前选中序号",
                 .json_type = "integer",
                 .enum_values = {},
                 .min_value = "0"},
                {.name = "placeholder",
                 .type = "string",
                 .default_value = "\"\"",
                 .required = false,
                 .note = "空选项时的占位文本",
                 .json_type = "string"},
                {.name = "accent_color",
                 .type = "Color",
                 .default_value = "theme.primary",
                 .required = false,
                 .note = "选中高亮色（缺省跟随主题 primary）",
                 .json_type = "array"},
                {.name = "box_color",
                 .type = "Color",
                 .default_value = "Color::white()",
                 .required = false,
                 .note = "主框背景色",
                 .json_type = "array"},
                {.name = "border_color",
                 .type = "Color",
                 .default_value = "{200,200,205,255}",
                 .required = false,
                 .note = "主框/面板边框色",
                 .json_type = "array"},
                {.name = "text_color",
                 .type = "Color",
                 .default_value = "{30,30,30,255}",
                 .required = false,
                 .note = "选项文本色",
                 .json_type = "array"},
                {.name = "arrow_color",
                 .type = "Color",
                 .default_value = "{120,120,125,255}",
                 .required = false,
                 .note = "箭头颜色",
                 .json_type = "array"},
                {.name = "box_height",
                 .type = "float",
                 .default_value = "30.0",
                 .required = false,
                 .note = "主框高度(dp)",
                 .json_type = "number",
                 .enum_values = {},
                 .min_value = "0"},
                {.name = "item_height",
                 .type = "float",
                 .default_value = "26.0",
                 .required = false,
                 .note = "选项行高(dp)",
                 .json_type = "number",
                 .enum_values = {},
                 .min_value = "0"},
                {.name = "font_size",
                 .type = "float",
                 .default_value = "13.0",
                 .required = false,
                 .note = "字号(pt)",
                 .json_type = "number",
                 .enum_values = {},
                 .min_value = "0"},
                {.name = "corner_radius",
                 .type = "float",
                 .default_value = "4.0",
                 .required = false,
                 .note = "主框圆角半径(dp)",
                 .json_type = "number",
                 .enum_values = {},
                 .min_value = "0"},
                {.name = "enabled",
                 .type = "bool",
                 .default_value = "true",
                 .required = false,
                 .note = "是否可交互（禁用灰化）",
                 .json_type = "boolean"},
            },
        .events = {"on_change"},
        .children_policy = "none",
        .invariants = {"selected_index >= 0", "selected_index < options.size()"},
        .examples = {R"(au::Dropdown({"Small", "Medium", "Large"}, 1))"},
    };
}
}  // namespace aurora
