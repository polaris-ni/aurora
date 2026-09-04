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

/// @brief 单个选项卡描述：标签文本 + 内容子树 + 可选可关闭标记。
struct Tab {
    std::string label;  ///< 标签文本
    Node content;  ///< 选中时显示的内容
    bool closable = false;  ///< 是否显示关闭按钮
};

/**
 * @brief 选项卡容器：标签栏 + 内容区。
 *
 * 顶部渲染标签栏（选中项高亮下划线），下方渲染当前选中标签的内容。
 * 点击标签切换选中；`selected_index()` 为响应式状态可订阅。
 *
 * 可定制性（对标 Qt QTabWidget / Flutter TabBar）：
 * - `active_color`（选中文本 + 指示器）未显式设置时跟随主题 `Theme::primary`；
 * - 标签栏背景 / 选中标签背景 / 未选中文本色 / 指示器厚度 / 字号 / 标签内边距均可配。
 *
 * 继承扩展点（protected 虚函数）：`paint_tab`（单个标签，含选中背景/指示器/文本/关闭钮）。
 *
 * 对标 Qt `QTabWidget`、WPF `TabControl`、Flutter `TabBar`+`TabBarView`、SwiftUI `TabView`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class TabBar : public Widget {
  public:
    TabBar() = default;
    explicit TabBar(std::vector<Tab> tabs, int initial = 0) : tabs_(std::move(tabs)) {
        const int max_idx = static_cast<int>(tabs_.size()) - 1;
        selected_.set(std::clamp(initial, 0, std::max(0, max_idx)));
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "TabBar"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "TabBar",
            .properties =
                {
                    {.name = "selected_index",
                     .type = "int",
                     .default_value = "0",
                     .required = false,
                     .note = "当前选中标签序号",
                     .json_type = "integer",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "tab_height",
                     .type = "float",
                     .default_value = "36.0",
                     .required = false,
                     .note = "标签栏高度(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "active_color",
                     .type = "Color",
                     .default_value = "theme.primary",
                     .required = false,
                     .note = "选中文本与指示器色（缺省跟随主题 primary）",
                     .json_type = "array"},
                    {.name = "bar_background",
                     .type = "Color",
                     .default_value = "{245,245,247,255}",
                     .required = false,
                     .note = "标签栏背景色",
                     .json_type = "array"},
                    {.name = "tab_background",
                     .type = "Color",
                     .default_value = "Color::white()",
                     .required = false,
                     .note = "选中标签背景色",
                     .json_type = "array"},
                    {.name = "text_color",
                     .type = "Color",
                     .default_value = "{96,96,96,255}",
                     .required = false,
                     .note = "未选中标签文本色",
                     .json_type = "array"},
                    {.name = "indicator_thickness",
                     .type = "float",
                     .default_value = "2.0",
                     .required = false,
                     .note = "选中指示器厚度(dp)；0=不画",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "font_size",
                     .type = "float",
                     .default_value = "13.0",
                     .required = false,
                     .note = "标签字号(pt)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "tab_padding",
                     .type = "float",
                     .default_value = "12.0",
                     .required = false,
                     .note = "标签左右内边距(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                },
            .events = {"on_change", "on_close"},
            .children_policy = "multiple",
            .invariants = {"selected_index >= 0", "tab_height > 0"},
            .examples = {R"(au::TabBar({ {"Tab A", au::Text("A")}, {"Tab B", au::Text("B")} }))"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&selected_); }

    /// @brief 当前选中序号（响应式状态）。
    [[nodiscard]] auto selected_index() -> State<int> & { return selected_; }
    [[nodiscard]] auto selected() const -> int { return selected_.get(); }

    /// @brief 切换选中标签（越界忽略）。
    auto select(int index) -> void {
        if (index >= 0 && std::cmp_less(index, tabs_.size()) && index != selected_.get()) {
            selected_.set(index);
            mark_needs_layout();
            mark_needs_paint();
            if (on_change_) {
                on_change_(index);
            }
        }
    }

    /// @brief 关闭标签（移除并回调；选中项自动修正）。
    auto close_tab(int index) -> void {
        if (index < 0 || std::cmp_greater_equal(index, tabs_.size())) {
            return;
        }
        tabs_.erase(tabs_.begin() + index);
        if (on_close_) {
            on_close_(index);
        }
        // 修正选中：若删除的在选中之前或就是选中项，前移
        const int sel = selected_.get();
        if (index <= sel && sel > 0) {
            selected_.set(sel - 1);
        } else if (std::cmp_greater_equal(sel, tabs_.size()) && !tabs_.empty()) {
            selected_.set(static_cast<int>(tabs_.size()) - 1);
        }
        mark_needs_layout();
        mark_needs_paint();
    }

    /// @brief 追加标签。
    auto add_tab(Tab tab) -> void {
        tabs_.push_back(std::move(tab));
        mark_needs_layout();
    }

    [[nodiscard]] auto tab_count() const -> std::size_t { return tabs_.size(); }
    [[nodiscard]] auto tab_label(int index) const -> std::string {
        if (index < 0 || std::cmp_greater_equal(index, tabs_.size())) {
            return {};
        }
        return tabs_[static_cast<std::size_t>(index)].label;
    }

    /// @brief 设置切换回调（链式）。
    auto set_on_change(std::function<void(int)> cb) -> TabBar & {
        on_change_ = std::move(cb);
        return *this;
    }
    /// @brief 设置关闭回调（链式）。
    auto set_on_close(std::function<void(int)> cb) -> TabBar & {
        on_close_ = std::move(cb);
        return *this;
    }
    /// @brief 设置标签栏高度（链式）。
    auto set_tab_height(float h) -> TabBar & {
        tab_height_ = h > 0.0F ? h : 36.0F;
        return *this;
    }

    /// @brief 设置选中文本与指示器色（链式）。不调用则跟随主题 `Theme::primary`。
    auto set_active_color(Color c) -> TabBar & {
        active_color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置标签栏背景色（链式）。
    auto set_bar_background(Color c) -> TabBar & {
        bar_background_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置选中标签背景色（链式）。
    auto set_tab_background(Color c) -> TabBar & {
        tab_background_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置未选中标签文本色（链式）。
    auto set_text_color(Color c) -> TabBar & {
        text_color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置选中指示器厚度 dp（链式；0 = 不画指示器）。
    auto set_indicator_thickness(float t) -> TabBar & {
        indicator_thickness_ = t >= 0.0F ? t : 2.0F;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置标签字号 pt（链式）。
    auto set_font_size(float s) -> TabBar & {
        font_size_ = s > 0.0F ? s : 13.0F;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置标签左右内边距 dp（链式）。
    auto set_tab_padding(float pad) -> TabBar & {
        tab_padding_ = pad >= 0.0F ? pad : 12.0F;
        mark_needs_layout();
        return *this;
    }

    /// @brief 命中标签栏时的点击切换（含关闭按钮命中）。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action == MouseAction::Press && e.local_position.y < tab_height_) {
            // 查找命中的标签
            float x = 0.0F;
            for (std::size_t i = 0; i < tabs_.size(); ++i) {
                const float w = tab_width(i);
                if (e.local_position.x >= x && e.local_position.x < x + w) {
                    // 命中关闭按钮区域（标签右侧 16dp）
                    if (tabs_[i].closable && e.local_position.x > x + w - AURORA_CLOSE_ZONE) {
                        close_tab(static_cast<int>(i));
                    } else {
                        select(static_cast<int>(i));
                    }
                    e.is_handled = true;
                    return;
                }
                x += w;
            }
        }
        Widget::on_pointer_event(e);
    }

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["selected_index"] = selected_.get();
        props["tab_height"] = tab_height_;
        Json labels = Json::array();
        for (const auto &t : tabs_) {
            labels.push_back(t.label);
        }
        props["tab_labels"] = labels;
        if (active_color_.has_value()) {
            props["active_color"] = color_to_json(*active_color_);  // 未设置不输出：保留「跟随主题」语义
        }
        props["bar_background"] = color_to_json(bar_background_);
        props["tab_background"] = color_to_json(tab_background_);
        props["text_color"] = color_to_json(text_color_);
        props["indicator_thickness"] = indicator_thickness_;
        props["font_size"] = font_size_;
        props["tab_padding"] = tab_padding_;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("selected_index")) {
            selected_.set(props["selected_index"].get<int>());
        }
        if (props.contains("tab_height")) {
            tab_height_ = props["tab_height"].get<float>();
        }
        if (props.contains("active_color")) {
            active_color_ = json_to_color(props["active_color"]);
        }
        if (props.contains("bar_background")) {
            bar_background_ = json_to_color(props["bar_background"]);
        }
        if (props.contains("tab_background")) {
            tab_background_ = json_to_color(props["tab_background"]);
        }
        if (props.contains("text_color")) {
            text_color_ = json_to_color(props["text_color"]);
        }
        if (props.contains("indicator_thickness")) {
            indicator_thickness_ = props["indicator_thickness"].get<float>();
        }
        if (props.contains("font_size")) {
            font_size_ = props["font_size"].get<float>();
        }
        if (props.contains("tab_padding")) {
            tab_padding_ = props["tab_padding"].get<float>();
        }
    }

    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        for (const auto &t : tabs_) {
            if (t.content) {
                fn(t.content.widget());
            }
        }
    }

    [[nodiscard]] auto child_nodes() const -> const std::vector<Node> & override {
        child_view_.clear();
        child_view_.reserve(tabs_.size());
        for (const auto &t : tabs_) {
            if (t.content) {
                child_view_.push_back(t.content);
            }
        }
        return child_view_;
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        Size self = c.max;
        if (!c.max.is_finite()) {
            self = Size{.width = 480.0F, .height = 320.0F};
        }
        // 当前内容区布局
        const int sel = selected_.get();
        if (sel >= 0 && std::cmp_less(sel, tabs_.size()) && tabs_[static_cast<std::size_t>(sel)].content) {
            Node &content = tabs_[static_cast<std::size_t>(sel)].content;
            Constraints inner;
            inner.min = Size{.width = 0.0F, .height = 0.0F};
            inner.max = Size{.width = self.width, .height = std::max(0.0F, self.height - tab_height_)};
            const Size cs = content.widget().layout(inner, ctx);
            content.widget().set_layout_parent(this);
            content.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = tab_height_}, .size = cs});
        }
        return c.constrain(self);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 标签栏背景
        const Rect bar{.origin = bounds.origin, .size = Size{.width = bounds.size.width, .height = tab_height_}};
        p.fill_rect(bar, bar_background_);

        // 选中色：显式设置优先，否则跟随主题 primary（ThemeScope 换肤即生效）。
        const Color accent = active_color_.value_or(inherit_theme(ctx).primary);
        Font label_font;
        label_font.size_pt = font_size_;

        float x = bounds.origin.x;
        for (std::size_t i = 0; i < tabs_.size(); ++i) {
            const float w = tab_width(i);
            const Rect tab_box{.origin = Point{.x = x, .y = bounds.origin.y},
                               .size = Size{.width = w, .height = tab_height_}};
            paint_tab(p, i, tab_box, std::cmp_equal(i, selected_.get()), accent, label_font);
            x += w;
        }

        // 当前内容
        const int sel = selected_.get();
        if (sel >= 0 && std::cmp_less(sel, tabs_.size()) && tabs_[static_cast<std::size_t>(sel)].content) {
            Node &content = tabs_[static_cast<std::size_t>(sel)].content;
            const Rect cb = content.bounds();
            const Rect global{.origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                              .size = cb.size};
            content.widget().paint(p, global, ctx);
        }
    }

    /// @brief 继承扩展点：绘制单个标签（选中背景 + 指示器 + 文本 + 可选关闭钮）。
    virtual auto paint_tab(Painter &p, std::size_t index, const Rect &tab_box, bool selected, Color accent,
                           const Font &label_font) -> void {
        if (selected) {
            p.fill_rect(tab_box, tab_background_);
            if (indicator_thickness_ > 0.0F) {
                const Rect underline{
                    .origin = Point{.x = tab_box.origin.x, .y = tab_box.origin.y + tab_height_ - indicator_thickness_},
                    .size = Size{.width = tab_box.size.width, .height = indicator_thickness_}};
                p.fill_rect(underline, accent);
            }
        }
        const Color text_color = selected ? accent : text_color_;
        const Rect text_box{
            .origin = Point{.x = tab_box.origin.x + tab_padding_, .y = tab_box.origin.y + 8.0F},
            .size = Size{.width = tab_box.size.width - (tab_padding_ * 2.0F), .height = tab_height_ - 16.0F}};
        p.draw_text(text_box, tabs_[index].label, label_font, text_color);
        // 关闭按钮
        if (tabs_[index].closable) {
            const Rect close_box{
                .origin = Point{.x = tab_box.origin.x + tab_box.size.width - AURORA_CLOSE_ZONE + 2.0F,
                                .y = tab_box.origin.y + 10.0F},
                .size = Size{.width = 12.0F, .height = tab_height_ - 20.0F}};
            p.draw_text(close_box, "x", label_font, Color{140, 140, 140, 255});
        }
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        // 标签栏区域命中自身
        if (local.y < tab_height_) {
            return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
        }
        // 内容区命中当前选中子树
        const int sel = selected_.get();
        if (sel >= 0 && std::cmp_less(sel, tabs_.size()) && tabs_[static_cast<std::size_t>(sel)].content) {
            Node &content = tabs_[static_cast<std::size_t>(sel)].content;
            const Rect cb = content.bounds();
            if (cb.contains(local)) {
                const Rect global{
                    .origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                    .size = cb.size};
                return content.widget().hit_test(local - cb.origin, global, ctx);
            }
        }
        return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        if (local.y >= tab_height_) {
            const int sel = selected_.get();
            if (sel >= 0 && std::cmp_less(sel, tabs_.size()) && tabs_[static_cast<std::size_t>(sel)].content) {
                Node &content = tabs_[static_cast<std::size_t>(sel)].content;
                const Rect cb = content.bounds();
                if (cb.contains(local)) {
                    const Rect global{
                        .origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                        .size = cb.size};
                    return content.widget().hit_test_chain(local - cb.origin, global, ctx);
                }
            }
        }
        return {};
    }

    auto on_mount(const BuildContext &ctx) -> void override {
        for (auto &t : tabs_) {
            if (t.content) {
                t.content.widget().mount(ctx);
            }
        }
    }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Widget::tick_gestures(now);
        for (auto &t : tabs_) {
            if (t.content) {
                t.content.widget().tick(now);
            }
        }
    }

    static constexpr float AURORA_TAB_PADDING = 12.0F;  ///< 标签左右内边距默认值(dp)
    static constexpr float AURORA_CLOSE_ZONE = 20.0F;  ///< 关闭按钮命中区宽度(dp)

    /// @brief 单个标签宽度：文本宽 + 内边距（closable 额外加关闭区）。
    [[nodiscard]] auto tab_width(std::size_t i) const -> float {
        Font f;
        f.size_pt = font_size_;
        float w = render::FontEngine::measure_width(tabs_[i].label, f) + (tab_padding_ * 2.0F);
        if (tabs_[i].closable) {
            w += AURORA_CLOSE_ZONE;
        }
        return std::max(w, 60.0F);
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    std::vector<Tab> tabs_;
    /// @brief child_nodes() 视图缓存（const 方法返回引用需持久存储）。
    mutable std::vector<Node> child_view_;
    State<int> selected_{0};
    float tab_height_ = 36.0F;
    std::function<void(int)> on_change_;
    std::function<void(int)> on_close_;
    std::optional<Color> active_color_;  ///< 选中文本/指示器色；空 = 跟随主题 primary
    Color bar_background_ = Color{245, 245, 247, 255};  ///< 标签栏背景色
    Color tab_background_ = Color{255, 255, 255, 255};  ///< 选中标签背景色
    Color text_color_ = Color{96, 96, 96, 255};  ///< 未选中标签文本色
    float indicator_thickness_ = 2.0F;  ///< 选中指示器厚度 dp；0 = 不画
    float font_size_ = 13.0F;  ///< 标签字号 pt
    float tab_padding_ = 12.0F;  ///< 标签左右内边距 dp
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

}  // namespace aurora
