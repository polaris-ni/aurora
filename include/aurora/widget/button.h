#pragma once

#include <algorithm>
#include <optional>
#include <string>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/types.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/i18n/string_table.h"
#include "aurora/render/font_engine.h"
#include "aurora/state/reactive.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief Button 属性（聚合）。未显式设置的 optional 颜色按状态自动派生（hover/pressed
/// 由背景色调暗得到，禁用态用统一灰化），与 Checkbox 「缺省跟随主题」语义一致。
struct ButtonProps {
    Reactive<LocalizedString> label;  ///< 按钮文字
    Reactive<Color> color = Color::blue();  ///< 背景色
    Color on_color = Color::white();  ///< 文字色
    Font font = Font{};  ///< 字体
    float corner_radius = 6.0F;  ///< 圆角半径（dp），>0 时背景与圆角裁剪
    EdgeInsets padding =
        EdgeInsets{.left = 12.0F, .top = 6.0F, .right = 12.0F, .bottom = 6.0F};  ///< 文字与背景之间的内边距
    bool enabled = true;  ///< 是否可点击（禁用态降级绘制并忽略点击）
    /// @brief 状态/样式扩展（对标 Flutter ButtonStyle / Qt QPushButton）：以下悬停/按下/边框/
    ///        禁用/最小尺寸字段均有默认派生值，未显式设置时按背景与状态计算。
    std::optional<Color> hover_color;  ///< 悬停背景色；缺省 = 背景色 ×0.92 调暗
    std::optional<Color> pressed_color;  ///< 按下背景色；缺省 = 背景色 ×0.80 调暗
    std::optional<Color> border_color;  ///< 边框色；缺省 = 无边框（填充按钮）；搭配 border_width 可做描边按钮
    float border_width = 0.0F;  ///< 边框线宽（dp）；0 = 不描边
    std::optional<Color> disabled_color;  ///< 禁用态背景色；缺省 = {200,200,200}
    std::optional<Color> disabled_text_color;  ///< 禁用态文字色；缺省 = {130,130,130}
    float min_width = 0.0F;  ///< 最小宽度（dp）；文字+内边距不足时擑到此宽
    float min_height = 0.0F;  ///< 最小高度（dp）
};

/// @brief 按钮控件（叶 widget）：填充背景 + 居中文字；点击激活 on_click。
///
/// 视觉（对标 Material Filled/Outlined Button / Fluent Button）：
/// - 填充态：背景色 + 圆角；悬停/按下自动调暗（或用 hover_color/pressed_color 显式指定）；
/// - 描边态：设置 border_color + border_width（配合透明背景可做 Outlined 风格）；
/// - 禁用态：灰化并忽略点击。
///
/// 继承扩展点（protected 虚函数，子类可单独覆盖某个绘制阶段）：
/// `resolve_background()` → `paint_background()` → `paint_border()` → `paint_label()`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Button : public LeafWidget, public ButtonProps {
  public:
    /// @brief 默认构造：全部属性取 `ButtonProps` 的聚合默认值（蓝底白字、圆角 6、内边距 12/6）。
    Button() = default;
    /// @brief 以一份完整属性聚合构造按钮（属性整体接管，避免逐字段赋值）。
    /// @param props 按钮属性快照，移动进本实例的 `ButtonProps` 基类子对象
    explicit Button(ButtonProps props) : ButtonProps(std::move(props)) {}
    /// @brief 以文字构造按钮，其余属性取默认值。
    /// @param label 按钮文字（存入 `label` 属性）
    explicit Button(const std::string &label) { this->label = label; }
    /// @brief 以字符串字面量构造按钮，其余属性取默认值。
    /// @param label 按钮文字（隐式转为 `std::string` 后存入 `label`）
    explicit Button(const char *label) { this->label = label; }

    /// @brief 点击回调槽位：装配链/用户可替换，`activate()` 触发时同步调用。
    /// NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::function<void()> on_click;  ///< 点击回调（同步派发，见 specification/05-event-navigation.md §3）

    /// @brief 运行时可查询的默认属性值（属性默认值的单一事实来源，需求 #5；经实例 `btn.defaults()` 调用亦可）。
    /// @return 一份缺省构造的 `ButtonProps`，与 `ButtonProps{}` 完全一致
    [[nodiscard]] static auto defaults() -> ButtonProps { return ButtonProps{}; }

    /// @brief 设置文字（链式）。
    /// @param s 新按钮文本，覆盖 `label` 属性
    /// @return 引用 `*this`，便于链式续写
    auto set_label(const std::string &s) -> Button & {
        label = s;
        return *this;
    }

    /// @brief 设置点击回调（链式）。
    /// @param fn 新回调；经 `std::move` 存入 `on_click`，点击时同步派发
    /// @return 引用 `*this` 以继续链式配置
    auto set_on_click(std::function<void()> fn) -> Button & {
        on_click = std::move(fn);
        return *this;
    }

    /// @brief 设置背景色（链式）。
    /// @param c 背景色，写入响应式属性 `color`
    /// @return 引用 `*this` 以继续链式配置
    auto background(Color c) -> Button & {
        color = c;
        return *this;
    }

    /// @brief 设置文字色（链式）。
    /// @param c 文字色，写入 `on_color`
    /// @return 引用 `*this` 以继续链式配置
    auto text_color(Color c) -> Button & {
        on_color = c;
        return *this;
    }

    /// @brief 设置圆角半径（链式）。
    /// @param r 圆角半径（dp）；>0 时背景与裁剪按圆角绘制
    /// @return 引用 `*this` 以继续链式配置
    auto set_corner_radius(float r) -> Button & {
        corner_radius = r;
        return *this;
    }

    /// @brief 设置内边距（链式）。
    /// @param e 文字与背景之间的四边内边距
    /// @return 引用 `*this` 以继续链式配置
    auto set_padding(const EdgeInsets &e) -> Button & {
        padding = e;
        return *this;
    }

    /// @brief 设置是否启用（链式）；禁用态降级绘制并忽略点击。
    /// @param v true 可交互；变更后立即 `mark_needs_paint()` 重绘灰化/正常态
    /// @return 引用 `*this` 以继续链式配置
    auto set_enabled(bool v) -> Button & {
        enabled = v;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置悬停背景色（链式）。不调用则由背景色自动调暗。
    /// @param c 显式悬停色；`resolve_background` 在 hover 态直接取该值
    /// @return 引用 `*this` 以继续链式配置
    auto set_hover_color(Color c) -> Button & {
        hover_color = c;
        return *this;
    }

    /// @brief 设置按下背景色（链式）。不调用则由背景色自动调暗。
    /// @param c 显式按下色；`resolve_background` 在 pressed 态直接取该值
    /// @return 引用 `*this` 以继续链式配置
    auto set_pressed_color(Color c) -> Button & {
        pressed_color = c;
        return *this;
    }

    /// @brief 设置边框（链式）；width<=0 不描边。搭配透明背景可做 Outlined 风格。
    /// @param c     边框色，写入 `border_color`
    /// @param width 边框线宽（dp），写入 `border_width`；缺省 1.5
    /// @return 引用 `*this` 以继续链式配置
    auto set_border(Color c, float width = 1.5F) -> Button & {
        border_color = c;
        border_width = width;
        return *this;
    }

    /// @brief 设置禁用态颜色（链式）。不调用则用统一灰化默认值。
    /// @param background 禁用态背景色（`disabled_color`）
    /// @param text       禁用态文字色（`disabled_text_color`）
    /// @return 引用 `*this` 以继续链式配置
    auto set_disabled_colors(Color background, Color text) -> Button & {
        disabled_color = background;
        disabled_text_color = text;
        return *this;
    }

    /// @brief 设置最小尺寸（链式）；文字+内边距不足时擑到此尺寸。
    /// @param w 最小宽度（dp），参与 `on_layout` 的宽度取大
    /// @param h 最小高度（dp），参与 `on_layout` 的高度取大
    /// @return 引用 `*this` 以继续链式配置
    auto set_min_size(float w, float h) -> Button & {
        min_width = w;
        min_height = h;
        return *this;
    }

    /// @brief 登记需订阅的信号视图：影响外观的两个响应式属性 `label` 与 `color`。
    /// @param out 末尾追加 `&label`、`&color`（框架据此订阅变更并触发重建）
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        out.push_back(&label);
        out.push_back(&color);
    }

    /// @brief 类型名：供注册表/日志/序列化按名分派。
    /// @return 静态字符串 `"Button"`
    [[nodiscard]] auto type_name() const -> const char * override { return "Button"; }

    /// @brief 首行基线（内容盒顶 → 基线）：与 `paint_label` 同源——标签在内容盒内垂直居中
    ///        （`ty = top + (h - th) / 2`），故基线 = 居中偏移 + 有效字体 ascent。
    /// @param ctx 构建上下文；形参保留自基类钩子签名，本实现未使用
    /// @return `(size().height - th) / 2 + ascent`：th/ascent 按有效字号（`size_pt`，非正时兜底 14）实测
    [[nodiscard]] auto baseline_distance(const BuildContext &ctx) const -> std::optional<float> override;

    /// @brief 无障碍名称：取按钮文字（经 i18n 表解析后的最终显示串）。
    /// @note Side-effects: reads i18n table
    /// @return `label` 解析后的显示文本（与 `paint_label` 所绘一致）
    [[nodiscard]] auto accessibility_label() const -> std::string override {
        return label.get().resolve(&default_string_table(), Locale{});
    }

    /// @brief 悬停默认手型光标：按钮悬停 PointingHand；修饰链显式 `cursor(...)` 声明优先。
    /// @note Side-effects: pure
    /// @return 恒为 `CursorShape::PointingHand`
    [[nodiscard]] auto cursor_shape() const -> std::optional<CursorShape> override { return CursorShape::PointingHand; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return 名为 "Button" 的描述表：属性项与 `ButtonProps` 字段一一对应，事件含 `on_click`，无子节点
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Button",
            .properties =
                {
                    {.name = "label",
                     .type = "LocalizedString",
                     .default_value = "\"\"",
                     .required = true,
                     .note = "Button text",
                     .json_type = "string"},
                    {.name = "color",
                     .type = "Color",
                     .default_value = "Color::blue()",
                     .required = false,
                     .note = "Background color",
                     .json_type = "array"},
                    {.name = "on_color",
                     .type = "Color",
                     .default_value = "Color::white()",
                     .required = false,
                     .note = "Text color",
                     .json_type = "array"},
                    {.name = "font_size",
                     .type = "float",
                     .default_value = "14.0",
                     .required = false,
                     .note = "Font size (pt)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "corner_radius",
                     .type = "float",
                     .default_value = "6.0",
                     .required = false,
                     .note = "Corner radius (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "padding",
                     .type = "EdgeInsets",
                     .default_value = "{12,6,12,6}",
                     .required = false,
                     .note = "Padding",
                     .json_type = "object"},
                    {.name = "enabled",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "Clickable",
                     .json_type = "boolean"},
                    {.name = "hover_color",
                     .type = "Color",
                     .default_value = "auto",
                     .required = false,
                     .note = "Hover background color (defaults to a darkened background)",
                     .json_type = "array"},
                    {.name = "pressed_color",
                     .type = "Color",
                     .default_value = "auto",
                     .required = false,
                     .note = "Pressed background color (defaults to a darkened background)",
                     .json_type = "array"},
                    {.name = "border_color",
                     .type = "Color",
                     .default_value = "none",
                     .required = false,
                     .note = "Border color (no border by default)",
                     .json_type = "array",
                     .enum_values = {},
                     .min_value = "",
                     .max_value = "",
                     .pattern = "",
                     .constraint = "",
                     .requires_props = {"border_width"}},
                    {.name = "border_width",
                     .type = "float",
                     .default_value = "0.0",
                     .required = false,
                     .note = "Border width (dp); 0 = no border",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "disabled_color",
                     .type = "Color",
                     .default_value = "{200,200,200}",
                     .required = false,
                     .note = "Disabled background color",
                     .json_type = "array"},
                    {.name = "disabled_text_color",
                     .type = "Color",
                     .default_value = "{130,130,130}",
                     .required = false,
                     .note = "Disabled text color",
                     .json_type = "array"},
                    {.name = "min_width",
                     .type = "float",
                     .default_value = "0.0",
                     .required = false,
                     .note = "Min width (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "min_height",
                     .type = "float",
                     .default_value = "0.0",
                     .required = false,
                     .note = "Min height (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
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
            .events = {"on_click"},
            .children_policy = "none",
            .examples = {"au::Button(au::ButtonProps{ .label = \"OK\" })"},
        };
    }
    /// @brief 实例级自描述：Button 无实例差异描述，直接转发 `describe_static()`。
    /// @return 与 `describe_static()` 相同的静态描述表
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 序列化 Button 自有属性到 `props`（optional 颜色仅在显式设置时输出，保留派生语义）。
    /// @param props 输出目标 JSON 对象，写入 label/color/on_color/font_size 等本类键
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);  // 先由基类写入 width/height/show 等通用属性
        props.set("label", label.get().text);
        props.set("color", color_to_json(color.get()));
        props.set("on_color", color_to_json(on_color));
        props.set("font_size", font.size_pt);
        props.set("corner_radius", corner_radius);
        props.set("padding", edge_insets_to_json(padding));
        props.set("enabled", Json{enabled});
        // optional 颜色未显式设置不输出：保留「自动派生」语义
        if (hover_color.has_value()) {
            props.set("hover_color", color_to_json(*hover_color));
        }
        if (pressed_color.has_value()) {
            props.set("pressed_color", color_to_json(*pressed_color));
        }
        if (border_color.has_value()) {
            props.set("border_color", color_to_json(*border_color));
        }
        if (border_width > 0.0F) {
            props.set("border_width", border_width);
        }
        if (disabled_color.has_value()) {
            props.set("disabled_color", color_to_json(*disabled_color));
        }
        if (disabled_text_color.has_value()) {
            props.set("disabled_text_color", color_to_json(*disabled_text_color));
        }
        if (min_width > 0.0F) {
            props.set("min_width", min_width);
        }
        if (min_height > 0.0F) {
            props.set("min_height", min_height);
        }
    }

    /// @brief 从 JSON 重建自有属性（与 `serialize_props` 对偶；键缺失则保留现值）。
    /// @param props 序列化属性对象，逐项 `contains` 检查后写回对应属性
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("label")) {
            label.set(LocalizedString{props.at("label")->as_or<std::string>("")});
        }
        if (props.contains("color")) {
            color.set(json_to_color(*props.at("color")));
        }
        if (props.contains("on_color")) {
            on_color = json_to_color(*props.at("on_color"));
        }
        if (props.contains("font_size")) {
            font.size_pt = props.at("font_size")->as_or<float>(0.0F);
        }
        if (props.contains("corner_radius")) {
            corner_radius = props.at("corner_radius")->as_or<float>(0.0F);
        }
        if (props.contains("padding")) {
            padding = json_to_edge_insets(*props.at("padding"));
        }
        if (props.contains("enabled")) {
            enabled = props.at("enabled")->as_or<bool>(false);
        }
        if (props.contains("hover_color")) {
            hover_color = json_to_color(*props.at("hover_color"));
        }
        if (props.contains("pressed_color")) {
            pressed_color = json_to_color(*props.at("pressed_color"));
        }
        if (props.contains("border_color")) {
            border_color = json_to_color(*props.at("border_color"));
        }
        if (props.contains("border_width")) {
            border_width = props.at("border_width")->as_or<float>(0.0F);
        }
        if (props.contains("disabled_color")) {
            disabled_color = json_to_color(*props.at("disabled_color"));
        }
        if (props.contains("disabled_text_color")) {
            disabled_text_color = json_to_color(*props.at("disabled_text_color"));
        }
        if (props.contains("min_width")) {
            min_width = props.at("min_width")->as_or<float>(0.0F);
        }
        if (props.contains("min_height")) {
            min_height = props.at("min_height")->as_or<float>(0.0F);
        }
    }

    /// @brief 触发点击：仅当 `enabled` 且已设置 `on_click` 时同步调用回调。
    auto activate() -> void override {
        if (enabled && on_click) {
            on_click();
        }
    }

    /// @brief Button 自带 on_click，作为点击目标消费事件（即便无 Clickable 修饰）。
    /// @return `enabled` 且已设置 `on_click` 时为 true（禁用或无回调时不消费点击）
    [[nodiscard]] auto wants_click() const -> bool override { return enabled && on_click != nullptr; }

    /// @brief 悬停反馈：背景色随悬停态变化（自动调暗或 hover_color）。
    /// @param entered true=指针进入，false=离开；启用态下据此标记重绘
    auto on_hover_change(bool entered) -> void override {
        Widget::on_hover_change(entered);  // 基类维护 hovered 状态并派发无障碍悬停事件
        if (enabled) {
            mark_needs_paint();
        }
    }

    /// @brief 按下/松开时重绘以呈现 pressed 态（基类维护 pressed_ 与点击识别）。
    /// @param e 鼠标事件；禁用态直接标记 handled 吞掉点击
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!enabled) {
            e.is_handled = true;  // 禁用态吞掉点击（不冒泡触发父级点击）
            return;
        }
        Widget::on_pointer_event(e);
        if (e.action == MouseAction::Press || e.action == MouseAction::Release) {
            mark_needs_paint();
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        Font f = font;
        if (f.size_pt <= 0.0F) {
            f.size_pt = 14.0F;
        }
        cached_text_width_ = render::FontEngine::measure_width(label.get().text, f);
        cached_text_height_ = render::FontEngine::measure_height(f);
        const float w = std::max(cached_text_width_ + padding.left + padding.right, min_width);
        const float h = std::max(cached_text_height_ + padding.top + padding.bottom, min_height);
        return c.constrain(Size{.width = w, .height = h});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override;

    // ---- 继承扩展点：子类可单独覆盖某个绘制阶段，无需重写整个 on_paint ----

    /// @brief 解析当前状态（enabled/pressed/hover）下的背景色。
    /// @return 启用态按 pressed/hover 取按下色/悬停色（缺省为基色加深），禁用态取禁用色（缺省浅灰）。
    [[nodiscard]] virtual auto resolve_background() const -> Color {
        if (!enabled) {
            return disabled_color.value_or(Color{200, 200, 200, 255});
        }
        const Color bg = color.get();
        if (pressed_) {
            return pressed_color.value_or(bg.shaded(0.80F));
        }
        if (hovered()) {
            return hover_color.value_or(bg.shaded(0.92F));
        }
        return bg;
    }

    /// @brief 解析当前状态下的文字色。
    /// @return 启用态取 on_color；禁用态取禁用文字色（缺省中灰）。
    [[nodiscard]] virtual auto resolve_text_color() const -> Color {
        return enabled ? on_color : disabled_text_color.value_or(Color{130, 130, 130, 255});
    }

    /// @brief 绘制背景（圆角填充）。
    /// @param p 绘制器。
    /// @param bounds 按钮区域。
    /// @param bg 当前状态解析出的背景色。
    virtual auto paint_background(Painter &p, const Rect &bounds, Color bg) -> void;
    /// @brief 绘制边框（border_width>0 且 border_color 已设置时）。
    /// @param p 绘制器。
    /// @param bounds 按钮区域。
    virtual auto paint_border(Painter &p, const Rect &bounds) -> void;
    /// @brief 绘制居中文字。
    /// @param p 绘制器。
    /// @param bounds 按钮区域。
    /// @param text_color 当前状态解析出的文字色。
    virtual auto paint_label(Painter &p, const Rect &bounds, Color text_color) -> void;

    // 缓存供 on_layout / paint_label 使用；受 protected 扩展点约束，有意非 private。
    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    float cached_text_width_ = 0.0F;  ///< on_layout 缓存的文字宽度（dp）
    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    float cached_text_height_ = 0.0F;  ///< on_layout 缓存的文字高度（dp）
};

}  // namespace aurora
