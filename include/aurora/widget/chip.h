#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/types.h"
#include "aurora/icon/icon.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme_scope.h"  // inherit_theme：未显式设色时取主题角色
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 标签/标记控件。
///
/// `Chip{label, avatar?, on_delete?}` — 带可选头像图标与删除按钮的紧凑标签。
/// 胶囊圆角背景（`corner_radius < 0` 自动 = 高度一半）；悬停调暗反馈；
/// 文本色 / 字号 / 删除钮颜色均可配。
///
/// 继承扩展点（protected 虚函数）：`paint_background` / `paint_foreground`。
/// 对标 Flutter `Chip`、SwiftUI `Label`、WPF 徽章模式。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Chip : public LeafWidget {
  public:
    /// @brief 默认构造：空标签，背景/文字取类内默认（浅灰底、黑字）。
    Chip() = default;
    /// @brief 构造标签：直接给定文字与可选背景色。
    /// @param label 标签文字（移入 `label_`）
    /// @param bg    背景色；缺省 `std::nullopt` ⇒ 未显式设色，跟随主题 `roles.surface_variant`
    ///             （ThemeScope 换肤即生效）。传入具体色值则显式覆盖主题。
    explicit Chip(std::string label, std::optional<Color> bg = std::nullopt)
        : label_(std::move(label)), bg_(bg) {}  // optional 平凡可拷贝，move 无收益

    /// @brief 类型名：供注册表/日志/序列化按名分派。
    /// @return 静态字符串 `"Chip"`
    [[nodiscard]] auto type_name() const -> const char * override { return "Chip"; }
    /// @brief 当前标签文字。
    /// @return `label_` 的常引用
    [[nodiscard]] auto label() const -> const std::string & { return label_; }
    /// @brief 设置标签文字（链式）。
    /// @param l 新文字（移入 `label_` 并标记重绘）
    /// @return 引用 `*this` 以继续链式配置
    auto set_label(std::string l) -> Chip & {
        label_ = std::move(l);
        mark_needs_paint();  // 文字变更需重绘前景
        return *this;
    }
    /// @brief 当前背景色（可选）。
    /// @return `bg_` 的常引用；`!has_value()` 表示未显式设色（绘制时回落主题 `surface_variant`）
    [[nodiscard]] auto background() const -> const std::optional<Color> & { return bg_; }
    /// @brief 设置背景色（链式）。
    /// @param c 新背景色（写入 `bg_` 并标记重绘）
    /// @return 引用 `*this` 以继续链式配置
    auto set_background(Color c) -> Chip & {
        bg_ = c;
        mark_needs_paint();  // 背景变更需重绘胶囊
        return *this;
    }

    /// @brief 设置头像文本（显示在标签左侧，如 emoji 或首字母）。
    /// @param av 头像文本（移入 `avatar_`，空串=不显示）；与 `avatar_icon` 互斥，本调用清空图标入口
    /// @return 引用 `*this` 以继续链式配置
    auto set_avatar(std::string av) -> Chip & {
        avatar_ = std::move(av);
        avatar_icon_.reset();  // 两入口互斥：字符串头像优先时清空图标入口
        mark_needs_paint();  // 前导内容变更需重绘
        return *this;
    }
    /// @brief 当前头像文本。
    /// @return `avatar_` 的常引用（空串=未设置）
    [[nodiscard]] auto avatar() const -> const std::string & { return avatar_; }

    /// @brief 设置图标头像（显示在标签左侧，`IconData` 字形或 SVG）。
    /// @param data 图标数据；与 `avatar` 字符串互斥，本调用清空文本入口
    /// @return 引用 `*this` 以继续链式配置
    auto set_avatar_icon(icon::IconData data) -> Chip & {
        avatar_icon_ = std::move(data);
        avatar_.clear();  // 两入口互斥：图标入口优先时清空文本头像
        mark_needs_layout();
        mark_needs_paint();
        return *this;
    }
    /// @brief 当前图标头像（可空）。
    /// @return `avatar_icon_` 的常引用；`!has_value()` 表示未设置
    [[nodiscard]] auto avatar_icon() const -> const std::optional<icon::IconData> & { return avatar_icon_; }

    /// @brief 设置删除回调（非空时显示 × 按钮）。
    /// @param cb 删除回调；点击右 30% 区域触发
    /// @return 引用 `*this` 以继续链式配置
    auto set_on_delete(std::function<void()> cb) -> Chip & {
        on_delete_ = std::move(cb);
        mark_needs_paint();  // 增删 × 钮需重绘（宽度也随重排变化）
        return *this;
    }

    /// @brief 设置文本色（链式）。
    /// @param c 标签/头像文字色
    /// @return 引用 `*this` 以继续链式配置
    auto set_text_color(Color c) -> Chip & {
        text_color_ = c;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置删除钮颜色（链式）。
    /// @param c × 钮文字色
    /// @return 引用 `*this` 以继续链式配置
    auto set_delete_color(Color c) -> Chip & {
        delete_color_ = c;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置字号 pt（链式）。
    /// @param s 字号；<=0 时回落默认 13pt，变更触发重排
    /// @return 引用 `*this` 以继续链式配置
    auto set_font_size(float s) -> Chip & {
        font_size_ = s > 0.0F ? s : 13.0F;
        mark_needs_layout();
        return *this;
    }
    /// @brief 设置圆角半径 dp（链式；< 0 自动 = 高度一半胶囊，0 = 直角）。
    /// @param r 圆角半径，直接写入 `corner_radius_`
    /// @return 引用 `*this` 以继续链式配置
    auto set_corner_radius(float r) -> Chip & {
        corner_radius_ = r;
        mark_needs_paint();
        return *this;
    }

    /// @brief 解析标签背景色：未显式设色时经 ctx 取主题 `surface_variant`（ThemeScope 换肤即生效），显式设色优先。
    /// @param ctx 构建上下文：供主题取色；无注入时 `inherit_theme` 回落 light（与 OS 未表态同口径）。
    /// @return 生效背景色。
    [[nodiscard]] auto resolve_background(const BuildContext &ctx) const -> Color {
        return bg_.value_or(inherit_theme(ctx).roles.surface_variant);
    }

    /// @brief 解析标签/头像文字色：未显式设色时经 ctx 取主题 `on_surface`，显式设色优先。
    /// @param ctx 构建上下文：供主题取色；无注入时 `inherit_theme` 回落 light。
    /// @return 生效文字色。
    [[nodiscard]] auto resolve_text_color(const BuildContext &ctx) const -> Color {
        return text_color_.value_or(inherit_theme(ctx).roles.on_surface);
    }

    /// @brief 解析删除钮颜色：未显式设色时经 ctx 取主题 `on_surface_variant`，显式设色优先。
    /// @param ctx 构建上下文：供主题取色；无注入时 `inherit_theme` 回落 light。
    /// @return 生效删除钮颜色。
    [[nodiscard]] auto resolve_delete_color(const BuildContext &ctx) const -> Color {
        return delete_color_.value_or(inherit_theme(ctx).roles.on_surface_variant);
    }

    /// @brief 静态自描述（规格附录 B）：可序列化属性/事件/子策略的运行时描述表。
    /// @return `Chip` 的 `WidgetDescriptor`（label/avatar/颜色/字号/圆角等，事件 on_delete，无子节点）
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Chip",
            .properties =
                {
                    {.name = "label",
                     .type = "string",
                     .default_value = "\"\"",
                     .required = false,
                     .note = "Label text",
                     .json_type = "string"},
                    {.name = "avatar",
                     .type = "string",
                     .default_value = "\"\"",
                     .required = false,
                     .note = "Leading icon/text",
                     .json_type = "string"},
                    {.name = "background",
                     .type = "Color",
                     .default_value = R"("theme.surface_variant")",
                     .required = false,
                     .note = "Background color (follows theme surface_variant when unset)",
                     .json_type = "array"},
                    {.name = "text_color",
                     .type = "Color",
                     .default_value = R"("theme.on_surface")",
                     .required = false,
                     .note = "Text color (follows theme on_surface when unset)",
                     .json_type = "array"},
                    {.name = "delete_color",
                     .type = "Color",
                     .default_value = R"("theme.on_surface_variant")",
                     .required = false,
                     .note = "Delete button color (follows theme on_surface_variant when unset)",
                     .json_type = "array"},
                    {.name = "font_size",
                     .type = "float",
                     .default_value = "13.0",
                     .required = false,
                     .note = "Font size (pt)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "corner_radius",
                     .type = "float",
                     .default_value = "-1.0",
                     .required = false,
                     .note = "Corner radius (dp); <0 = half the height",
                     .json_type = "number"},
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
            .events = {"on_delete"},
            .children_policy = "none",
            .examples = {"au::Chip(\"Tag\").set_avatar(\"★\").set_on_delete([]{ })"},
        };
    }
    /// @brief 实例级自描述：Chip 无实例差异描述，直接转发 `describe_static()`。
    /// @return 与 `describe_static()` 相同的静态描述表
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 信号采集：Chip 的可订阅面（on_delete 回调）以数据成员形态直挂，无独立 SignalView 节点可收集。
    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 指针事件：左键 Press 落在右 30% 区域（删除热区）时触发 `on_delete_` 并标记事件已处理。
    /// @param e 鼠标事件；命中删除热区时置 `is_handled`，其余情形不消费
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (on_delete_ && e.action == MouseAction::Press && e.button == MouseButton::Left) {
            // 简化：点击右半区触发删除
            if (e.local_position.x > size().width * 0.7F) {
                on_delete_();
                e.is_handled = true;
            }
        }
    }

    /// @brief 悬停反馈：背景调暗。
    /// @param entered true=指针进入，false=离开；两种跃迁都标记重绘以切换明暗
    auto on_hover_change(bool entered) -> void override {
        Widget::on_hover_change(entered);
        mark_needs_paint();
    }

    /// @brief 序列化 Chip 自有属性（avatar 为空不输出）。
    /// @param props 输出目标 JSON 对象
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);  // 先由基类写入通用属性
        props.set("label", label_);
        if (!avatar_.empty()) {
            props.set("avatar", avatar_);
        }
        // 背景/文字/删除色仅在显式设置时输出：保留「跟随主题」语义（与 Button/Text 同口径）
        if (bg_.has_value()) {
            props.set("background", color_to_json(*bg_));
        }
        if (text_color_.has_value()) {
            props.set("text_color", color_to_json(*text_color_));
        }
        if (delete_color_.has_value()) {
            props.set("delete_color", color_to_json(*delete_color_));
        }
        props.set("font_size", font_size_);
        props.set("corner_radius", corner_radius_);
    }

    /// @brief 从 JSON 重建 Chip 自有属性（与 `serialize_props` 对偶；键缺失保留现值）。
    /// @param props 序列化属性对象
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("label")) {
            label_ = props.at("label")->as_or<std::string>("");
        }
        if (props.contains("avatar")) {
            avatar_ = props.at("avatar")->as_or<std::string>("");
        }
        if (props.contains("background")) {
            bg_ = json_to_color(*props.at("background"));
        }
        if (props.contains("text_color")) {
            text_color_ = json_to_color(*props.at("text_color"));
        }
        if (props.contains("delete_color")) {
            delete_color_ = json_to_color(*props.at("delete_color"));
        }
        if (props.contains("font_size")) {
            font_size_ = props.at("font_size")->as_or<float>(0.0F);
        }
        if (props.contains("corner_radius")) {
            corner_radius_ = props.at("corner_radius")->as_or<float>(0.0F);
        }
    }

  protected:
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        paint_background(p, bounds, ctx);
        paint_foreground(p, bounds, ctx);
    }

    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        const Font f{.size_pt = font_size_};
        constexpr float h_pad = 10.0F;
        constexpr float v_pad = 4.0F;
        float w = h_pad * 2.0F;
        if (!avatar_.empty()) {
            w += render::FontEngine::measure_width(avatar_, f) + 4.0F;
        }
        if (avatar_icon_.has_value()) {
            w += font_size_ + 4.0F;  // 图标盒占一行字高
        }
        w += render::FontEngine::measure_width(label_, f);
        if (on_delete_) {
            w += render::FontEngine::measure_width(" ×", f);
        }
        const float h = render::FontEngine::measure_height(f) + (v_pad * 2.0F);
        return c.constrain(Size{.width = w, .height = h});
    }

    /// @brief 继承扩展点：绘制胶囊背景（悬停调暗）。
    /// @param p 绘制器。
    /// @param bounds 本控件布局矩形（圆角未设时取其半高为半径）。
    /// @param ctx 构建上下文：未显式设色时解析主题 `surface_variant` 作背景。
    virtual auto paint_background(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
        const float radius = corner_radius_ >= 0.0F ? corner_radius_ : bounds.size.height * 0.5F;
        const Color bg = hovered() ? resolve_background(ctx).shaded(0.92F) : resolve_background(ctx);
        if (radius > 0.0F) {
            p.fill_rounded_rect(bounds, radius, bg);
        } else {
            p.fill_rect(bounds, bg);
        }
    }

    /// @brief 继承扩展点：绘制头像 + 标签 + 删除钮。
    /// @param p 绘制器。
    /// @param bounds 本控件布局矩形（文本基线与删除钮均在其内定位）。
    /// @param ctx 构建上下文：供主题取色（标签/头像/删除钮未显式设色时回落对应角色）。
    /// @note 名称不可用 `paint_content`：该名在 `Widget` 中是 `virtual` 的绘制管线核心
    ///       （`paint()` → `render_into()` → `paint_content()`），签名完全一致会被**意外覆盖**，
    ///       导致管线绕过 `on_paint`，胶囊背景永不绘制（历史缺陷）。
    virtual auto paint_foreground(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
        const Font f{.size_pt = font_size_};
        const Color tc = resolve_text_color(ctx);
        const Color dc = resolve_delete_color(ctx);
        float x = bounds.origin.x + 10.0F;
        const float y = bounds.origin.y + 4.0F;
        if (!avatar_.empty()) {
            p.draw_text(Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = 100.0F, .height = 20.0F}}, avatar_,
                        f, tc);
            x += render::FontEngine::measure_width(avatar_, f) + 4.0F;
        }
        if (avatar_icon_.has_value()) {
            const Rect icon_box{
                .origin = Point{.x = x, .y = bounds.origin.y + ((bounds.size.height - font_size_) / 2.0F)},
                .size = Size{.width = font_size_, .height = font_size_}};
            paint_icon_data(p, icon_box, *avatar_icon_, tc, 0.0F);
            x += font_size_ + 4.0F;
        }
        p.draw_text(Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = 200.0F, .height = 20.0F}}, label_, f,
                    tc);
        x += render::FontEngine::measure_width(label_, f);
        if (on_delete_) {
            p.draw_text(Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = 30.0F, .height = 20.0F}}, " ×", f,
                        dc);
        }
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    std::string label_;
    std::string avatar_;
    std::optional<icon::IconData> avatar_icon_;  ///< 图标头像（`IconData` 字形 / SVG）；与 `avatar_` 互斥
    std::optional<Color> bg_;  ///< 标签背景色；空 = 跟随主题 surface_variant
    std::function<void()> on_delete_;
    std::optional<Color> text_color_;  ///< 标签/头像文字色；空 = 跟随主题 on_surface
    std::optional<Color> delete_color_;  ///< 删除钮颜色；空 = 跟随主题 on_surface_variant
    float font_size_ = 13.0F;  ///< 字号 pt
    float corner_radius_ = -1.0F;  ///< 圆角半径 dp；< 0 自动 = 高度一半
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

/// @brief 角标/计数徽章。
///
/// `Badge{count, child}` — 在子控件右上角叠加数字徽章。
/// 对标 Flutter `Badge`、SwiftUI `.badge()`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Badge : public SingleChild {
  public:
    /// @brief 默认构造：计数 0（不绘制徽章），无子节点。
    Badge() = default;
    /// @brief 构造角标：给定计数与可选宿主子节点。
    /// @param count 徽章数字；>0 才绘制
    /// @param child 被叠加角标的子控件节点（缺省空）
    explicit Badge(int count, Node child = Node{}) : SingleChild(std::move(child)), count_(count) {}

    /// @brief 类型名：供注册表/日志/序列化按名分派。
    /// @return 静态字符串 `"Badge"`
    [[nodiscard]] auto type_name() const -> const char * override { return "Badge"; }
    /// @brief 当前计数。
    /// @return `count_` 值
    [[nodiscard]] auto count() const -> int { return count_; }
    /// @brief 设置计数（链式）。
    /// @param c 新计数，>0 时绘制角标
    /// @return 引用 `*this` 以继续链式配置
    auto set_count(int c) -> Badge & {
        count_ = c;
        mark_needs_paint();  // 角标显隐/数字变化需重绘
        return *this;
    }
    /// @brief 设置徽章背景色（链式）。
    /// @param c 角标底色
    /// @return 引用 `*this` 以继续链式配置
    auto set_badge_color(Color c) -> Badge & {
        badge_color_ = c;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置徽章文字色（链式；默认白色）。
    /// @param c 角标数字颜色
    /// @return 引用 `*this` 以继续链式配置
    auto set_text_color(Color c) -> Badge & {
        text_color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 解析徽章背景色：未显式设色时经 ctx 取主题 `error`（ThemeScope 换肤即生效），显式设色优先。
    /// @param ctx 构建上下文：供主题取色；无注入时 `inherit_theme` 回落 light。
    /// @return 生效徽章底色。
    [[nodiscard]] auto resolve_badge_color(const BuildContext &ctx) const -> Color {
        return badge_color_.value_or(inherit_theme(ctx).roles.error);
    }

    /// @brief 解析徽章文字色：未显式设色时经 ctx 取主题 `on_error`，显式设色优先。
    /// @param ctx 构建上下文：供主题取色；无注入时 `inherit_theme` 回落 light。
    /// @return 生效徽章文字色。
    [[nodiscard]] auto resolve_text_color(const BuildContext &ctx) const -> Color {
        return text_color_.value_or(inherit_theme(ctx).roles.on_error);
    }

    /// @brief 静态自描述（规格附录 B）：可序列化属性/事件/子策略的运行时描述表。
    /// @return `Badge` 的 `WidgetDescriptor`（count/背景色/文字色，单子节点策略）
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Badge",
            .properties =
                {
                    {.name = "count",
                     .type = "int",
                     .default_value = "0",
                     .required = false,
                     .note = "Badge number",
                     .json_type = "integer",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "badge_color",
                     .type = "Color",
                     .default_value = R"("theme.error")",
                     .required = false,
                     .note = "Badge background color (follows theme error when unset)",
                     .json_type = "array"},
                    {.name = "text_color",
                     .type = "Color",
                     .default_value = R"("theme.on_error")",
                     .required = false,
                     .note = "Badge text color (follows theme on_error when unset)",
                     .json_type = "array"},
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
            .children_policy = "single",
            .examples = {"au::Badge(5, au::Text{\"Inbox\"})"},
        };
    }
    /// @brief 实例级自描述：Badge 无实例差异描述，直接转发 `describe_static()`。
    /// @return 与 `describe_static()` 相同的静态描述表
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 信号采集：Badge 纯展示、无可订阅事件，空实现。
    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 序列化 Badge 自有属性（计数与两色）。
    /// @param props 输出目标 JSON 对象
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);  // 先由基类写入通用属性
        props.set("count", count_);
        // 徽章底/文字色仅在显式设置时输出：保留「跟随主题」语义（与 Button/Text/Chip 同口径）
        if (badge_color_.has_value()) {
            props.set("badge_color", color_to_json(*badge_color_));
        }
        if (text_color_.has_value()) {
            props.set("text_color", color_to_json(*text_color_));
        }
    }

    /// @brief 从 JSON 重建 Badge 自有属性（与 `serialize_props` 对偶；键缺失保留现值）。
    /// @param props 序列化属性对象
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("count")) {
            count_ = props.at("count")->as_or<std::int32_t>(0);
        }
        if (props.contains("badge_color")) {
            badge_color_ = json_to_color(*props.at("badge_color"));
        }
        if (props.contains("text_color")) {
            text_color_ = json_to_color(*props.at("text_color"));
        }
    }

  protected:
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (child_) {
            child_.widget().paint(p, bounds, ctx);
        }
        if (count_ > 0) {
            constexpr float bw = 20.0F;
            constexpr float bh = 18.0F;
            const Rect badge_rect{.origin = Point{.x = bounds.origin.x + bounds.size.width - bw, .y = bounds.origin.y},
                                  .size = Size{.width = bw, .height = bh}};
            p.fill_rounded_rect(badge_rect, bh * 0.5F, resolve_badge_color(ctx));  // 胶囊徽章（对标 Material Badge）
            const Font f{.size_pt = 11.0F, .weight = 700};
            const std::string txt = (count_ > 99) ? "99+" : std::to_string(count_);
            p.draw_text(badge_rect, txt, f, resolve_text_color(ctx));
        }
    }

    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        Size child_size{.width = 0, .height = 0};
        if (child_) {
            child_size = child_.widget().layout(c, ctx);
        }
        // 徽章额外空间
        constexpr float badge_w = 20.0F;
        constexpr float badge_h = 18.0F;
        return c.constrain(Size{.width = std::max(child_size.width, badge_w),
                                .height = child_size.height + (count_ > 0 ? badge_h * 0.5F : 0.0F)});
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    int count_ = 0;
    std::optional<Color> badge_color_;  ///< 徽章底色；空 = 跟随主题 error
    std::optional<Color> text_color_;  ///< 徽章文字色；空 = 跟随主题 on_error
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

}  // namespace aurora
