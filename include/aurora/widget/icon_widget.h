#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "aurora/core/color.h"
#include "aurora/icon/icon.h"
#include "aurora/icon/icon_svg.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 图标尺寸基准档（dp）：与 `TypeScale` 的常用档位对齐。
enum class IconSize : std::uint8_t {
    S16 = 16,  ///< 紧凑嵌入（列表项尾标、输入内嵌）。
    S20 = 20,  ///< 标准小标（工具栏、芯片）。
    S24 = 24,  ///< 默认基准（按钮、导航）。
    S32 = 32,  ///< 强调（空状态、首屏插画级）。
};

/// @brief 图标控件（叶控件）：按语义名或 `IconData` 直绘一个字形 / SVG 图标。
///
/// 字体与 SVG 资产由应用经 `FontEngine` / 字节注入提供，库不内置任何图标资产；
/// 未注册资产时 `IconRegistry::lookup` 返回结构化错误，控件据此跳过绘制（不崩溃、不画占位）。
/// @note Thread: main-thread only
/// @note Rebuildable: via from_json
class Icon : public LeafWidget {
  public:
    /// @brief 默认构造：24 dp 基准、未设色（跟随主题 `on_surface_variant`）、无语义标注。
    Icon() = default;

    /// @brief 以语义名构造（须已注册到 `IconRegistry`）。
    /// @param icon_name 语义名（ASCII snake_case）。
    explicit Icon(std::string icon_name) : icon_name_(std::move(icon_name)) {}

    /// @brief 以 `IconData` 字面构造（代码侧使用，不走注册表）。
    /// @param data 图标数据（字形或 SVG）。
    explicit Icon(icon::IconData data) : icon_data_(std::move(data)) {}

    /// @brief 设置语义名（链式）；与 `IconData` 字面互斥，后者优先。
    /// @param name 语义名（ASCII snake_case）。
    /// @return 自身引用。
    auto set_icon(std::string name) -> Icon & {
        icon_name_ = std::move(name);
        icon_data_.reset();
        return *this;
    }

    /// @brief 设置 `IconData` 字面（链式）；覆盖语义名。
    /// @param data 图标数据（字形或 SVG）。
    /// @return 自身引用。
    auto set_icon(icon::IconData data) -> Icon & {
        icon_data_ = std::move(data);
        icon_name_.clear();
        return *this;
    }

    /// @brief 设置盒尺寸（dp，正方形）；变更经 `mark_size_intent_changed` 触发父侧重排。
    /// @param dp 目标边长（dp）；跨两档以上跨度时绘制引入光学中心垂直补偿。
    /// @return 自身引用。
    auto set_size(float dp) -> Icon & {
        if (size_pt_ == dp) {
            return *this;
        }
        size_pt_ = dp;
        mark_size_intent_changed();
        return *this;
    }

    /// @brief 设置盒尺寸（按基准档，链式）。
    /// @param s 基准档（`IconSize`：16/20/24/32 dp）。
    /// @return 自身引用。
    auto set_size(IconSize s) -> Icon & { return set_size(static_cast<float>(s)); }

    /// @brief 设置着色（链式）；空 = 跟随主题 `on_surface_variant`。
    /// @param c 显式着色；`std::nullopt` 回落主题角色。
    /// @return 自身引用。
    auto set_color(std::optional<Color> c) -> Icon & {
        color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置无障碍语义标注（链式）；非空时才进语义树。
    /// @param label 语义标签（UTF-8）；空 = 纯装饰。
    /// @return 自身引用。
    auto set_semantic_label(std::string label) -> Icon & {
        semantic_label_ = std::move(label);
        return *this;
    }

    /// @brief 类型名。
    /// @return C 字符串 "Icon"。
    [[nodiscard]] auto type_name() const -> const char * override { return "Icon"; }

    /// @brief 无障碍语义标签：返回 `semantic_label_`。
    /// @return 语义标签；空串时基类约定不进语义树。
    [[nodiscard]] auto accessibility_label() const -> std::string override { return semantic_label_; }

    /// @brief 仅当设置了语义标注时才进语义树（纯装饰图标排除）。
    /// @return `!semantic_label_.empty()`。
    [[nodiscard]] auto accessibility_is_semantic() const -> bool override { return !semantic_label_.empty(); }

    /// @brief 运行时自描述。
    /// @return Icon 的控件描述符（icon/size/color/semantic_label 属性）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Icon",
            .properties =
                {
                    {.name = "icon",
                     .type = "string",
                     .default_value = "",
                     .required = false,
                     .note = "Semantic name registered in IconRegistry",
                     .json_type = "string"},
                    {.name = "size",
                     .type = "float",
                     .default_value = "24",
                     .required = false,
                     .note = "Box size (dp, square)",
                     .json_type = "number",
                     .min_value = "0"},
                    {.name = "color",
                     .type = "Color",
                     .default_value = "theme.on_surface_variant",
                     .required = false,
                     .note = "Icon color (follows theme on_surface_variant when unset)",
                     .json_type = "array"},
                    {.name = "semantic_label",
                     .type = "string",
                     .default_value = "",
                     .required = false,
                     .note = "Accessibility label; non-empty = semantic",
                     .json_type = "string"},
                },
            .events = {},
            .children_policy = "none",
            .examples = {"au::Icon(\"settings\")", "au::Icon(icon::IconData::glyph(\"MaterialSymbols\", 0xE8B8))"},
        };
    }
    /// @brief 运行时自描述：转发静态描述符。
    /// @return Icon 的控件描述符（icon/size/color/semantic_label 属性，无事件、无子项）。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集本控件的可订阅信号视图。
    /// @param out 输出参数；`Icon` 无信号，恒不写入。
    auto collect_signals(StateSink &out) -> void override { (void)out; }

    /// @brief 序列化 Icon 自有属性：语义名（未设不输出）、盒尺寸、显式着色（未设不输出）与语义标注。
    /// @param props 目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        if (!icon_name_.empty()) {
            props.set("icon", icon_name_);
        }
        props.set("size", size_pt_);
        if (color_.has_value()) {
            props.set("color", color_to_json(*color_));
        }
        if (!semantic_label_.empty()) {
            props.set("semantic_label", semantic_label_);
        }
    }

    /// @brief 反序列化 Icon 自有属性：按存在的键逐一还原语义名/尺寸/着色/语义标注，缺键保持当前值。
    /// @param props 源 JSON 对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("icon")) {
            icon_name_ = props.at("icon")->as_or<std::string>("");
            icon_data_.reset();
        }
        if (props.contains("size")) {
            size_pt_ = props.at("size")->as_or<float>(24.0F);
        }
        if (props.contains("color")) {
            color_ = json_to_color(*props.at("color"));
        }
        if (props.contains("semantic_label")) {
            semantic_label_ = props.at("semantic_label")->as_or<std::string>("");
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = size_pt_, .height = size_pt_});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;

  private:
    friend class IconButton;
    std::string icon_name_;  ///< 语义名（与 `icon_data_` 互斥，后者优先）。
    std::optional<icon::IconData> icon_data_;  ///< 代码侧 `IconData` 字面。
    float size_pt_ = 24.0F;  ///< 盒边长（dp）。
    std::optional<Color> color_;  ///< 显式着色；空 = 跟随主题。
    std::string semantic_label_;  ///< 无障碍语义标注；空 = 纯装饰。
    float optical_bias_ = 0.0F;  ///< 光学中心垂直补偿量（参数化，单位 dp）。
};

/// @brief 图标按钮（叶控件）：在方形点击区内绘制一个图标，含悬停 / 按下 / 禁用 / 焦点环四态取色。
///
/// 交互语义复用 `LeafWidget` 的输入框架；取色两态语义在全部四态下均须成立。
/// @note Thread: main-thread only
/// @note Rebuildable: via from_json
class IconButton : public LeafWidget {
  public:
    /// @brief 默认构造：24 dp 图标 + 8 dp 内边距、启用、未设色。
    IconButton() = default;

    /// @brief 以语义名构造。
    /// @param icon_name 语义名（ASCII snake_case）。
    explicit IconButton(std::string icon_name) { set_icon(std::move(icon_name)); }

    /// @brief 以 `IconData` 字面构造。
    /// @param data 图标数据。
    explicit IconButton(icon::IconData data) { set_icon(std::move(data)); }

    /// @brief 设置图标语义名（链式）；与 `IconData` 字面互斥，后者优先。
    /// @param name 语义名（ASCII snake_case，须已注册到 `IconRegistry`）。
    /// @return 自身引用。
    auto set_icon(std::string name) -> IconButton & {
        icon_name_ = std::move(name);
        icon_data_.reset();
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置 `IconData` 字面（链式）；覆盖语义名。
    /// @param data 图标数据（字形或 SVG）。
    /// @return 自身引用。
    auto set_icon(icon::IconData data) -> IconButton & {
        icon_data_ = std::move(data);
        icon_name_.clear();
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置图标盒尺寸（dp，链式）；变更经 `mark_size_intent_changed` 触发父侧重排。
    /// @param dp 目标边长（dp）。
    /// @return 自身引用。
    auto set_size(float dp) -> IconButton & {
        if (size_pt_ == dp) {
            return *this;
        }
        size_pt_ = dp;
        mark_size_intent_changed();
        return *this;
    }

    /// @brief 设置图标盒尺寸（按基准档，链式）。
    /// @param s 基准档（`IconSize`：16/20/24/32 dp）。
    /// @return 自身引用。
    auto set_size(IconSize s) -> IconButton & { return set_size(static_cast<float>(s)); }

    /// @brief 设置内边距（dp，链式）；点击区 = 图标盒 + 两侧内边距。
    /// @param dp 内边距（dp，非负）。
    /// @return 自身引用。
    auto set_padding(float dp) -> IconButton & {
        padding_ = dp;
        mark_size_intent_changed();
        return *this;
    }

    /// @brief 设置显式着色（链式）；空 = 跟随主题角色按启用/悬停/按下/禁用四态取色。
    /// @param c 显式着色；`std::nullopt` 回落四态主题角色。
    /// @return 自身引用。
    auto set_color(std::optional<Color> c) -> IconButton & {
        color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置无障碍语义标注（链式）。
    /// @param label 语义标签（UTF-8）；经 `accessibility_label()` 进语义树。
    /// @return 自身引用。
    auto set_semantic_label(std::string label) -> IconButton & {
        semantic_label_ = std::move(label);
        return *this;
    }

    /// @brief 设置是否启用（链式）；禁用态降级绘制、吞掉点击且不作为点击目标。
    /// @param v 启用为 true。
    /// @return 自身引用。
    auto set_enabled(bool v) -> IconButton & {
        enabled_ = v;
        mark_needs_paint();
        return *this;
    }

    /// @brief 点击回调（同步派发）。
    std::function<void()> on_click;  ///< 点击回调。 NOLINT

    /// @brief 设置点击回调（链式）；经 `activate()` 在 Release 且未拖拽/未长按时同步派发。
    /// @param fn 回调（`std::function` 空即可撤销）。
    /// @return 自身引用。
    auto set_on_click(std::function<void()> fn) -> IconButton & {
        on_click = std::move(fn);
        return *this;
    }

    /// @brief 类型名。
    /// @return C 字符串 "IconButton"。
    [[nodiscard]] auto type_name() const -> const char * override { return "IconButton"; }

    /// @brief 无障碍语义标签：返回 `semantic_label_`。
    /// @return 语义标签（UTF-8）；空串时由语义树按回退链继续推断。
    [[nodiscard]] auto accessibility_label() const -> std::string override { return semantic_label_; }

    /// @brief 运行时自描述。
    /// @return IconButton 的控件描述符（icon/size/padding/color/semantic_label/enabled 属性与 on_click 事件）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "IconButton",
            .properties =
                {
                    {.name = "icon",
                     .type = "string",
                     .default_value = "",
                     .required = false,
                     .note = "Semantic name registered in IconRegistry",
                     .json_type = "string"},
                    {.name = "size",
                     .type = "float",
                     .default_value = "24",
                     .required = false,
                     .note = "Icon box size (dp, square)",
                     .json_type = "number",
                     .min_value = "0"},
                    {.name = "padding",
                     .type = "float",
                     .default_value = "8",
                     .required = false,
                     .note = "Padding around icon (dp)",
                     .json_type = "number",
                     .min_value = "0"},
                    {.name = "color",
                     .type = "Color",
                     .default_value = "theme.on_surface_variant",
                     .required = false,
                     .note = "Icon color (theme role per state when unset)",
                     .json_type = "array"},
                    {.name = "semantic_label",
                     .type = "string",
                     .default_value = "",
                     .required = false,
                     .note = "Accessibility label",
                     .json_type = "string"},
                    {.name = "enabled",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "Interactive when true",
                     .json_type = "boolean"},
                },
            .events = {"on_click"},
            .children_policy = "none",
            .examples = {"au::IconButton(\"close\").set_on_click([]{ })"},
        };
    }
    /// @brief 运行时自描述：转发静态描述符。
    /// @return IconButton 的控件描述符（icon/size/padding/color/semantic_label/enabled 属性与 on_click 事件）。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集本控件的可订阅信号视图。
    /// @param out 输出参数；`IconButton` 无信号，恒不写入。
    auto collect_signals(StateSink &out) -> void override { (void)out; }

    /// @brief 序列化 IconButton 自有属性：语义名、图标盒尺寸、内边距、显式着色与语义标注、启用位。
    /// @param props 目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        if (!icon_name_.empty()) {
            props.set("icon", icon_name_);
        }
        props.set("size", size_pt_);
        props.set("padding", padding_);
        if (color_.has_value()) {
            props.set("color", color_to_json(*color_));
        }
        if (!semantic_label_.empty()) {
            props.set("semantic_label", semantic_label_);
        }
        props.set("enabled", Json{enabled_});
    }

    /// @brief 反序列化 IconButton 自有属性：按存在的键逐一还原，缺键保持当前值。
    /// @param props 源 JSON 对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("icon")) {
            icon_name_ = props.at("icon")->as_or<std::string>("");
            icon_data_.reset();
        }
        if (props.contains("size")) {
            size_pt_ = props.at("size")->as_or<float>(24.0F);
        }
        if (props.contains("padding")) {
            padding_ = props.at("padding")->as_or<float>(8.0F);
        }
        if (props.contains("color")) {
            color_ = json_to_color(*props.at("color"));
        }
        if (props.contains("semantic_label")) {
            semantic_label_ = props.at("semantic_label")->as_or<std::string>("");
        }
        if (props.contains("enabled")) {
            enabled_ = props.at("enabled")->as_or<bool>(true);
        }
    }

    /// @brief 触发点击：仅当启用且已设置 `on_click` 时同步调用回调。
    /// @note Side-effects: invokes user callback
    auto activate() -> void override {
        if (enabled_ && on_click) {
            on_click();
        }
    }

    /// @brief IconButton 自带 on_click，作为点击目标消费事件（即便无 Clickable 修饰）。
    /// @return `enabled_` 且已设置 `on_click` 时为 true。
    [[nodiscard]] auto wants_click() const -> bool override { return enabled_ && on_click != nullptr; }

    /// @brief 指针事件：禁用态吞掉点击不冒泡；启用态交基类维护按下/悬停态并标记重绘。
    /// @param e 鼠标事件（可标记已处理）。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!enabled_) {
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
        const float total = size_pt_ + (2.0F * padding_);
        return c.constrain(Size{.width = total, .height = total});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;

  private:
    std::string icon_name_;  ///< 语义名。
    std::optional<icon::IconData> icon_data_;  ///< 代码侧 `IconData` 字面。
    float size_pt_ = 24.0F;  ///< 图标盒边长（dp）。
    float padding_ = 8.0F;  ///< 图标与点击区边界的内边距（dp）。
    std::optional<Color> color_;  ///< 显式着色；空 = 四态取色。
    std::string semantic_label_;  ///< 无障碍语义标注。
    bool enabled_ = true;  ///< 是否可交互。
};

}  // namespace aurora
