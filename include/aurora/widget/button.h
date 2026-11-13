#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/types.h"
#include "aurora/environment/build_context.h"  // 带上下文的读件需完整类型以解主题作用域
#include "aurora/i18n/localized_string.h"
#include "aurora/i18n/message_catalog.h"
#include "aurora/render/font_engine.h"
#include "aurora/state/read_only_state.h"
#include "aurora/theming/color_roles.h"  // 活令牌回落点：roles.primary / roles.on_primary
#include "aurora/theming/shape_scale.h"  // 活令牌回落点：shapes.small
#include "aurora/theming/spacing_scale.h"  // 派生回落点读 spacing.large / spacing.small
#include "aurora/theming/theme_scope.h"  // inherit_theme
#include "aurora/theming/type_scale.h"  // 非正字号兜底取字阶声明默认值，不另立常量
#include "aurora/widget/props.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief Button 的属性权威表：一行一字段，键名、私有字段、脏档位、约束与回落点只在行里声明一次。
///
/// 读件、写件、序列化、订阅采集与 schema 描述全部由这张表派生，因此本类不再有可直读的公有字段，
/// 也不再有「字段名 / setter 名 / JSON 键名 / 描述符名」四份需要同步的拼写。三态语义与迁移前一致：
/// - **必填行**（`label`）：字段本身就是生效值，不可撤表态；
/// - **可空行**（其余）：未表态时取该行回落点，表态后不再跟随；撤表态走公共面上唯一的 `clear(row)`；
/// - **来源行**（`label` / `color`）：另有一只同名写件收 `ReadOnlyState`，即响应式换源。
///
/// 回落点三档在本表里同时在场：
/// - 活令牌档（`color` / `on_color` / `corner_radius` / `hover_color` / `pressed_color` /
///   `disabled_color` / `disabled_text_color`）：随作用域换肤而变，读件带 `BuildContext`；
/// - 常量档（`enabled` / `border_*` / `min_*`）：全库唯一基线，读件无参；
/// - 派生档（`font_size` / `padding`）：回落点读同族他行或主题档再加工，读件带 `BuildContext`，
///   schema 里的默认值只能以显示串给出：只读已登记令牌的行写
///   `.shows_token<&SpacingScale::large, &SpacingScale::small>()`（名由登记表反查），其余写 `.shows("auto")`。
///
/// `padding` 落派生档而非活令牌档：四边结构与单一标量档不同型（`SpacingScale` 的档位说明同口径），
/// 故未表态时按「水平 `spacing.large`、垂直 `spacing.small`」现场拼装——与令牌层落地前的字面量
/// 12/6/12/6 逐值相同，切风格档只换档、不换拼装规则。
///
/// `font_size` 也落派生档：回落点是主题字阶正文档，而 `TypeScale` 的值是 `Font` 而非标量档、
/// 不入 `theme_tokens` 登记表（理由见 specification/07-environment-modifier.md §5.1），故只能就地
/// 写 `.shows("type_scale.body_medium")`。
///
/// @note Thread: main-thread only
/// @note Side-effects: 写入经宿主链接位标脏并重建订阅；未挂载时是纯 DTO
class ButtonProps : public AbstractProps<ButtonProps> {
  public:
    // ---- 权威表：参数顺序即 schema 顺序 ----

    /// @brief 必填来源件：按钮文字。变更同时影响度量与绘制 ⇒ 布局 + 绘制两档都打。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto label_row() {
        return prop_row<&ButtonProps::label_>("label").layout().paint().required().note("Button text");
    }

    /// @brief 可空来源件：背景色。未表态时取主题 `roles.primary`，换肤即随动。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto color_row() {
        return prop_row<&ButtonProps::color_>("color").paint().fallback<&ColorRoles::primary>().note(
            "Background color (follows theme primary when unset)");
    }

    /// @brief 可空值件：文字色。未表态时取主题 `roles.on_primary`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto on_color_row() {
        return prop_row<&ButtonProps::on_color_>("on_color")
            .paint()
            .fallback<&ColorRoles::on_primary>()
            .note("Text color (follows theme on_primary when unset)");
    }

    /// @brief 可空值件：字号（pt）。参与度量 ⇒ 布局档；下界 0。
    ///
    /// 未表态时取主题**正文档**（`type_scale.body_medium`）：风格档的正文基准与无障碍字号缩放都经
    /// 同一条字阶通道到达本控件，与 `Text` 读同一档逐位同值。表态过即以表态值为准 —— 缩放不二次
    /// 作用于显式设值（见 specification/07-environment-modifier.md §5.1）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto font_size_row() {
        return prop_row<&ButtonProps::font_size_>("font_size")
            .layout()
            .paint()
            .bounds("0")
            .fallback(&ButtonProps::font_size_default)
            .shows("type_scale.body_medium")
            .note("Font size (pt; follows the theme body type scale when unset)");
    }

    /// @brief 可空值件：圆角半径（dp）。未表态时取主题 `shapes.small`；下界 0。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto corner_radius_row() {
        return prop_row<&ButtonProps::corner_radius_>("corner_radius")
            .paint()
            .bounds("0")
            .fallback<&ShapeScale::small>()
            .note("Corner radius (dp; follows theme shapes.small when unset)");
    }

    /// @brief 可空值件：内边距。派生档 ⇒ schema 只能给出显示串（没有实例求不出值）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto padding_row() {
        return prop_row<&ButtonProps::padding_>("padding")
            .layout()
            .paint()
            .fallback(&ButtonProps::padding_default)
            .shows_token<&SpacingScale::large, &SpacingScale::small>()
            .note("Padding (follows theme spacing when unset)");
    }

    /// @brief 可空值件：是否可点击。禁用态只降级绘制，不改度量。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto enabled_row() {
        return prop_row<&ButtonProps::enabled_>("enabled")
            .paint()
            .fallback(&ButtonProps::enabled_default)
            .note("Clickable");
    }

    /// @brief 可空值件：悬停叠色。未表态时取主题 `roles.hover`（半透明，绘制时叠在基色之上）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto hover_color_row() {
        return prop_row<&ButtonProps::hover_color_>("hover_color")
            .paint()
            .fallback<&ColorRoles::hover>()
            .note("Hover overlay tint (follows theme hover when unset)");
    }

    /// @brief 可空值件：按下叠色。未表态时取主题 `roles.pressed`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto pressed_color_row() {
        return prop_row<&ButtonProps::pressed_color_>("pressed_color")
            .paint()
            .fallback<&ColorRoles::pressed>()
            .note("Pressed overlay tint (follows theme pressed when unset)");
    }

    /// @brief 可空值件：边框色。缺省「无边框」是语义事实，故给显示串而非透明色。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto border_color_row() {
        return prop_row<&ButtonProps::border_color_>("border_color")
            .paint()
            .fallback(&ButtonProps::border_color_default)
            .shows("none")
            .requires_prop("border_width")
            .note("Border color (no border by default)");
    }

    /// @brief 可空值件：边框线宽（dp）；0 即不描边。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto border_width_row() {
        return prop_row<&ButtonProps::border_width_>("border_width")
            .paint()
            .bounds("0")
            .fallback(&ButtonProps::border_width_default)
            .note("Border width (dp); 0 = no border");
    }

    /// @brief 可空值件：禁用态背景色。未表态时取主题 `roles.surface_container_high`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto disabled_color_row() {
        return prop_row<&ButtonProps::disabled_color_>("disabled_color")
            .paint()
            .fallback<&ColorRoles::surface_container_high>()
            .note("Disabled background color (follows theme surface_container_high when unset)");
    }

    /// @brief 可空值件：禁用态文字色。未表态时取主题 `roles.outline`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto disabled_text_color_row() {
        return prop_row<&ButtonProps::disabled_text_color_>("disabled_text_color")
            .paint()
            .fallback<&ColorRoles::outline>()
            .note("Disabled text color (follows theme outline when unset)");
    }

    /// @brief 可空值件：最小宽度（dp）；只参与度量。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto min_width_row() {
        return prop_row<&ButtonProps::min_width_>("min_width")
            .layout()
            .bounds("0")
            .fallback(&ButtonProps::min_width_default)
            .note("Min width (dp)");
    }

    /// @brief 可空值件：最小高度（dp）；只参与度量。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto min_height_row() {
        return prop_row<&ButtonProps::min_height_>("min_height")
            .layout()
            .bounds("0")
            .fallback(&ButtonProps::min_height_default)
            .note("Min height (dp)");
    }

    /// @brief 权威表本体：本控件全部属性行的元组。
    /// @return 按声明顺序排列的行元组（发射面与序列化面都遍历它）。
    [[nodiscard]] static constexpr auto rows() {
        return make_prop_rows(label_row(), color_row(), on_color_row(), font_size_row(), corner_radius_row(),
                              padding_row(), enabled_row(), hover_color_row(), pressed_color_row(), border_color_row(),
                              border_width_row(), disabled_color_row(), disabled_text_color_row(), min_width_row(),
                              min_height_row());
    }

    // ---- 读面：每行恰一件，形态由该行回落点决定 ----

    /// @brief 当前标签的生效值（必填来源件：无参读件）。
    /// @return 标签的本地化值（显示串仍须经 `MessageCatalog` 解析）。
    [[nodiscard]] auto label() const -> LocalizedString { return get(label_row()); }

    /// @brief 当前作用域下生效的背景色。
    /// @param ctx 构建上下文：未表态时据此解主题 `roles.primary`。
    /// @return 生效背景色。
    [[nodiscard]] auto color(const BuildContext &ctx) const -> Color { return resolve(color_row(), ctx); }

    /// @brief 当前作用域下生效的文字色。
    /// @param ctx 构建上下文：未表态时据此解主题 `roles.on_primary`。
    /// @return 生效文字色。
    [[nodiscard]] auto on_color(const BuildContext &ctx) const -> Color { return resolve(on_color_row(), ctx); }

    /// @brief 当前作用域下生效的字号（pt）：回落点是主题正文档 ⇒ 带上下文读件。
    /// @param ctx 构建上下文：未表态时据此解主题 `type_scale.body_medium`。
    /// @return 生效字号（pt）；表态过即表态值，缩放不二次作用于它。
    [[nodiscard]] auto font_size(const BuildContext &ctx) const -> float { return resolve(font_size_row(), ctx); }

    /// @brief 当前作用域下生效的圆角半径（dp）。
    /// @param ctx 构建上下文：未表态时据此解主题 `shapes.small`。
    /// @return 生效圆角半径。
    [[nodiscard]] auto corner_radius(const BuildContext &ctx) const -> float {
        return resolve(corner_radius_row(), ctx);
    }

    /// @brief 当前作用域下生效的四边内边距。
    /// @param ctx 构建上下文：未表态时按主题间距档现场拼装（见 `padding_default`）。
    /// @return 生效内边距。
    [[nodiscard]] auto padding(const BuildContext &ctx) const -> EdgeInsets { return resolve(padding_row(), ctx); }

    /// @brief 当前是否可点击。
    /// @return 生效的启用位。
    [[nodiscard]] auto enabled() const -> bool { return stated(enabled_row()); }

    /// @brief 当前作用域下生效的悬停叠色（未表态时取主题 `roles.hover`）。
    /// @param ctx 构建上下文。
    /// @return 生效悬停叠色（半透明，绘制时叠在基色之上）。
    [[nodiscard]] auto hover_color(const BuildContext &ctx) const -> Color { return resolve(hover_color_row(), ctx); }

    /// @brief 当前作用域下生效的按下叠色（未表态时取主题 `roles.pressed`）。
    /// @param ctx 构建上下文。
    /// @return 生效按下叠色（半透明，绘制时叠在基色之上）。
    [[nodiscard]] auto pressed_color(const BuildContext &ctx) const -> Color {
        return resolve(pressed_color_row(), ctx);
    }

    /// @brief 当前生效的边框色（未表态时取「无边框」回落基线；画不画由表态位决定）。
    /// @return 生效边框色。
    [[nodiscard]] auto border_color() const -> Color { return stated(border_color_row()); }

    /// @brief 当前生效的边框线宽（dp）。
    /// @return 生效线宽；0 即不描边。
    [[nodiscard]] auto border_width() const -> float { return stated(border_width_row()); }

    /// @brief 当前作用域下生效的禁用态背景色（未表态时取主题 `roles.surface_container_high`）。
    /// @param ctx 构建上下文。
    /// @return 生效禁用背景色。
    [[nodiscard]] auto disabled_color(const BuildContext &ctx) const -> Color {
        return resolve(disabled_color_row(), ctx);
    }

    /// @brief 当前作用域下生效的禁用态文字色（未表态时取主题 `roles.outline`）。
    /// @param ctx 构建上下文。
    /// @return 生效禁用文字色。
    [[nodiscard]] auto disabled_text_color(const BuildContext &ctx) const -> Color {
        return resolve(disabled_text_color_row(), ctx);
    }

    /// @brief 当前生效的最小宽度（dp）。
    /// @return 生效最小宽度。
    [[nodiscard]] auto min_width() const -> float { return stated(min_width_row()); }

    /// @brief 当前生效的最小高度（dp）。
    /// @return 生效最小高度。
    [[nodiscard]] auto min_height() const -> float { return stated(min_height_row()); }

    // ---- 写面：同名重载区分「装值」与「换源」，档位仍由行决定 ----

    /// @brief 装入标签常量值（换入自有常量源，旧源退订）。
    /// @param value 新标签值。
    /// @return 引用本属性对象，便于链式续写。
    auto set_label(const LocalizedString &value) -> ButtonProps & { return assign(label_row(), value); }

    /// @brief 装入标签常量值（`std::string` 直写通道）。
    /// @param value 新标签文本。
    /// @return 引用本属性对象，便于链式续写。
    auto set_label(const std::string &value) -> ButtonProps & { return set_label(LocalizedString{value}); }

    /// @brief 装入标签常量值（C 字符串直写通道：消除字面量在值件与来源件之间的重载歧义）。
    /// @param value 新标签文本。
    /// @return 引用本属性对象，便于链式续写。
    auto set_label(const char *value) -> ButtonProps & { return set_label(LocalizedString{value}); }

    /// @brief 换绑标签来源（响应式换源：订阅重建，外部 `State` 继续驱动本属性）。
    /// @param source 新的只读句柄来源。
    /// @return 引用本属性对象，便于链式续写。
    auto set_label(ReadOnlyState<LocalizedString> source) -> ButtonProps & {
        return rebind(label_row(), std::move(source));
    }

    /// @brief 装入背景色常量值（表态即脱离主题 `roles.primary`）。
    /// @param value 新背景色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_color(Color value) -> ButtonProps & { return assign(color_row(), value); }

    /// @brief 换绑背景色来源（响应式换源）。
    /// @param source 新的只读句柄来源。
    /// @return 引用本属性对象，便于链式续写。
    auto set_color(ReadOnlyState<Color> source) -> ButtonProps & { return rebind(color_row(), std::move(source)); }

    /// @brief 装入文字色（表态即脱离主题 `roles.on_primary`）。
    /// @param value 新文字色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_on_color(Color value) -> ButtonProps & { return assign(on_color_row(), value); }

    /// @brief 装入字号（pt）；表态即脱离主题正文档，且不再随无障碍字号缩放二次变化。
    /// @param value 新字号。
    /// @return 引用本属性对象，便于链式续写。
    auto set_font_size(float value) -> ButtonProps & { return assign(font_size_row(), value); }

    /// @brief 装入圆角半径（dp）；表态即脱离主题形状档。
    /// @param value 新半径；0 是「显式要直角」，与「未表态」不是同一件事。
    /// @return 引用本属性对象，便于链式续写。
    auto set_corner_radius(float value) -> ButtonProps & { return assign(corner_radius_row(), value); }

    /// @brief 装入四边内边距；表态即脱离主题间距档。
    /// @param value 新内边距。
    /// @return 引用本属性对象，便于链式续写。
    auto set_padding(EdgeInsets value) -> ButtonProps & { return assign(padding_row(), value); }

    /// @brief 装入启用位。
    /// @param value true 可点击；禁用态降级绘制并忽略点击。
    /// @return 引用本属性对象，便于链式续写。
    auto set_enabled(bool value) -> ButtonProps & { return assign(enabled_row(), value); }

    /// @brief 装入悬停叠色；不调用则取主题 `roles.hover`。
    /// @param value 显式悬停叠色（半透明色叠在基色之上）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_hover_color(Color value) -> ButtonProps & { return assign(hover_color_row(), value); }

    /// @brief 装入按下叠色；不调用则取主题 `roles.pressed`。
    /// @param value 显式按下叠色（半透明色叠在基色之上）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_pressed_color(Color value) -> ButtonProps & { return assign(pressed_color_row(), value); }

    /// @brief 装入边框色；画不画仍看本行表态位（见 `Button::paint_border`）。
    /// @param value 边框色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_border_color(Color value) -> ButtonProps & { return assign(border_color_row(), value); }

    /// @brief 装入边框线宽（dp）；0 即不描边。
    /// @param value 线宽。
    /// @return 引用本属性对象，便于链式续写。
    auto set_border_width(float value) -> ButtonProps & { return assign(border_width_row(), value); }

    /// @brief 装入禁用态背景色。
    /// @param value 禁用背景色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_disabled_color(Color value) -> ButtonProps & { return assign(disabled_color_row(), value); }

    /// @brief 装入禁用态文字色。
    /// @param value 禁用文字色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_disabled_text_color(Color value) -> ButtonProps & { return assign(disabled_text_color_row(), value); }

    /// @brief 装入最小宽度（dp）。
    /// @param value 最小宽度。
    /// @return 引用本属性对象，便于链式续写。
    auto set_min_width(float value) -> ButtonProps & { return assign(min_width_row(), value); }

    /// @brief 装入最小高度（dp）。
    /// @param value 最小高度。
    /// @return 引用本属性对象，便于链式续写。
    auto set_min_height(float value) -> ButtonProps & { return assign(min_height_row(), value); }

    // ---- 回落点提供者：常量档与派生档的唯一基线（同族互读只经读件） ----

    /// @brief 字号回落点（派生档）：读当前作用域主题的字阶正文档。
    ///
    /// 非正读数的兜底直接取 `TypeScale` 的声明默认值，不另立一份磅值常量——那枚数值的真源只有字阶本身。
    /// @param self 本属性对象（本回落点不读同族他行，形参由派生档签名固定）。
    /// @param ctx 构建上下文：提供主题作用域。
    /// @return 该作用域下 `theme.type_scale.body_medium` 的磅值。
    [[nodiscard]] static auto font_size_default(const ButtonProps &self, const BuildContext &ctx) -> float {
        (void)self;
        const float pt = inherit_theme(ctx).type_scale.body_medium.size_pt;
        return pt > 0.0F ? pt : TypeScale{}.body_medium.size_pt;  // 主题被写成非正字号时不退化成零高度文本
    }

    /// @brief 启用位回落基线。
    /// @return true（默认可点击）。
    [[nodiscard]] static auto enabled_default() -> bool { return true; }

    /// @brief 边框色回落基线：全透明（填充按钮）。
    /// @return 透明色。
    [[nodiscard]] static auto border_color_default() -> Color { return Color::transparent(); }

    /// @brief 边框线宽回落基线。
    /// @return 0.0F（不描边）。
    [[nodiscard]] static auto border_width_default() -> float { return 0.0F; }

    /// @brief 最小宽度回落基线。
    /// @return 0.0F（不擑宽）。
    [[nodiscard]] static auto min_width_default() -> float { return 0.0F; }

    /// @brief 最小高度回落基线。
    /// @return 0.0F（不擑高）。
    [[nodiscard]] static auto min_height_default() -> float { return 0.0F; }

    /// @brief 内边距回落点（派生档）：按当前作用域的间距档拼装四边。
    /// @param self 本属性对象（本回落点不读同族他行，形参由派生档签名固定）。
    /// @param ctx 构建上下文：提供主题作用域。
    /// @return 水平 `spacing.large`、垂直 `spacing.small` 的四边内边距。
    [[nodiscard]] static auto padding_default(const ButtonProps &self, const BuildContext &ctx) -> EdgeInsets {
        (void)self;
        const auto &s = inherit_theme(ctx).spacing;
        return EdgeInsets{.left = s.large, .top = s.small, .right = s.large, .bottom = s.small};
    }

  private:
    ReadOnlyState<LocalizedString> label_;  ///< 标签来源（必填 ⇒ 裸句柄）
    std::optional<ReadOnlyState<Color>> color_;  ///< 背景色来源（可空 ⇒ 未表态即无源可订）
    std::optional<Color> on_color_;  ///< 文字色表态位
    std::optional<float> font_size_;  ///< 字号表态位（pt）
    std::optional<float> corner_radius_;  ///< 圆角表态位（dp）
    std::optional<EdgeInsets> padding_;  ///< 内边距表态位
    std::optional<bool> enabled_;  ///< 启用位表态位
    std::optional<Color> hover_color_;  ///< 悬停色表态位
    std::optional<Color> pressed_color_;  ///< 按下色表态位
    std::optional<Color> border_color_;  ///< 边框色表态位
    std::optional<float> border_width_;  ///< 边框线宽表态位（dp）
    std::optional<Color> disabled_color_;  ///< 禁用背景色表态位
    std::optional<Color> disabled_text_color_;  ///< 禁用文字色表态位
    std::optional<float> min_width_;  ///< 最小宽度表态位（dp）
    std::optional<float> min_height_;  ///< 最小高度表态位（dp）

    friend class AbstractProps<ButtonProps>;
};

/// @brief 按钮控件（叶 widget）：填充背景 + 居中文字；点击激活 on_click。
///
/// 视觉（对标 Material Filled/Outlined Button / Fluent Button）：
/// - 填充态：背景色 + 圆角；悬停/按下在基色上叠加状态层（默认取主题 `roles.hover` /
///   `roles.pressed`，也可用 `set_hover_color()` / `set_pressed_color()` 显式指定叠色）；
/// - 描边态：`set_border_color()` + `set_border_width()` 同时表态即描边（配合透明背景可做 Outlined）；
/// - 禁用态：灰化并忽略点击。
///
/// 属性一律经 `ButtonProps` 的读写件访问（本类公有继承它，故 `btn.set_label(...)` 与
/// `btn.label()` 都直接可用）；继承扩展点为 protected 虚函数：
/// `resolve_background()` → `paint_background()` → `paint_border()` → `paint_label()`。
///
/// 标签的 i18n 契约：`label` 是 `LocalizedString`，绘制与布局一律经 `resolved_label()` 查
/// `MessageCatalog`（`Environment` 注入 `Locale` 时按它解析，否则回落缺省档）——不直读
/// `label().text`，否则 `tr()` 构造的键会画成空白。`accessibility_label()` 与绘制同源。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Button : public LeafWidget, public ButtonProps {
  public:
    /// @brief 默认构造：接线宿主并取全表回落点——颜色跟随主题（`roles.primary` 底 +
    ///        `roles.on_primary` 字），圆角与内边距同样跟随主题（`shapes.small` 与
    ///        `spacing.large` / `spacing.small`）。
    Button() { bind(*this); }

    /// @brief 以一份完整属性对象构造按钮（属性整体接管，避免逐字段赋值）。
    /// @param props 属性快照；移动进本实例的 `ButtonProps` 基类子对象后接线宿主
    explicit Button(ButtonProps props) : ButtonProps(std::move(props)) { bind(*this); }

    /// @brief 以文字构造按钮，其余属性取回落点。
    /// @param text 按钮文字（装入 `label` 行）
    explicit Button(const std::string &text) : Button() { set_label(text); }

    /// @brief 以字符串字面量构造按钮，其余属性取回落点。
    /// @param text 按钮文字（装入 `label` 行）
    explicit Button(const char *text) : Button() { set_label(text); }

    /// @brief 点击回调槽位：装配链/用户可替换，`activate()` 触发时同步调用。
    ///        非属性行——它是回调而非可序列化状态，故留在控件本体。
    /// NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::function<void()> on_click;  ///< 点击回调（同步派发，见 specification/05-event-navigation.md §3）

    /// @brief 运行时可查询的默认属性值（属性表的单一事实来源，需求 SPEC.API.SENSIBLE-DEFAULTS.001）。
    /// @return 一份缺省构造的 `ButtonProps`：各行未表态，生效值即该行回落点
    [[nodiscard]] static auto defaults() -> ButtonProps { return {}; }

    /// @brief 设置点击回调（链式）。
    /// @param fn 新回调；经 `std::move` 存入 `on_click`，点击时同步派发
    /// @return 引用 `*this` 以继续链式配置
    auto set_on_click(std::function<void()> fn) -> Button & {
        on_click = std::move(fn);
        return *this;
    }

    /// @brief 登记需订阅的信号视图：表里来源型行的当前句柄（未表态的行没有源可订）。
    /// @param out 末尾追加已安装的标签与背景色来源（框架据此订阅变更并触发重建）
    auto collect_signals(StateSink &out) -> void override { collect_prop_sources(out); }

    /// @brief 类型名：供注册表/日志/序列化按名分派。
    /// @return 静态字符串 `"Button"`
    [[nodiscard]] auto type_name() const -> const char * override { return "Button"; }

    /// @brief 首行基线（内容盒顶 → 基线）：与 `paint_label` 同源——标签在内容盒内垂直居中
    ///        （`ty = top + (h - th) / 2`），故基线 = 居中偏移 + 有效字体 ascent。
    /// @param ctx 构建上下文：有效字体经它解作用域主题的正文档（与 `paint_label` 同一份回落口径）
    /// @return `(size().height - th) / 2 + ascent`：th/ascent 按有效字号（`font_size(ctx)`，非正时兜底字阶正文档）实测
    [[nodiscard]] auto baseline_distance(const BuildContext &ctx) const -> std::optional<float> override;

    /// @brief 无障碍名称：取按钮文字（经 i18n 表解析后的最终显示串）。
    /// @note 与 `paint_label` 所绘同源：二者共用 `resolved_label()`——布局/绘制期的解析结果落在
    ///       `cached_display_text_`，本钩子优先复用它；尚未布局或绘制时按默认 locale 现场解析。
    /// @note Side-effects: reads i18n table
    /// @return `label` 解析后的显示文本（与 `paint_label` 所绘一致）
    [[nodiscard]] auto accessibility_label() const -> std::string override {
        if (!cached_display_text_.empty()) {
            return cached_display_text_;
        }
        return label().resolve(&default_catalog(), Locale{});
    }

    /// @brief 悬停默认手型光标：按钮悬停 PointingHand；修饰链显式 `cursor(...)` 声明优先。
    /// @note Side-effects: pure
    /// @return 恒为 `CursorShape::PointingHand`
    [[nodiscard]] auto cursor_shape() const -> std::optional<CursorShape> override { return CursorShape::PointingHand; }

    /// @brief 运行时自描述（规格附录 B）：属性项全部由权威表发射，控件只补自己的事实。
    /// @return 名为 "Button" 的描述表：`properties` 按 `rows()` 点名顺序，事件含 `on_click`，无子节点
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        auto descriptor = props_descriptor("Button");
        descriptor.events = {"on_click"};
        descriptor.children_policy = "none";
        descriptor.examples = {"au::Button(au::ButtonProps().set_label(\"OK\"))"};
        return descriptor;
    }

    /// @brief 实例级自描述：Button 无实例差异描述，直接转发 `describe_static()`。
    /// @return 与 `describe_static()` 相同的静态描述表
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 序列化自有属性到 `props`：必填行恒写，其余行只在表态时写（未表态不写键，回落语义不外泄）。
    /// @param props 输出目标 JSON 对象；先由基类写 width/height/show，再叠本表行
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props_serialize(props);
    }

    /// @brief 从 JSON 重建自有属性（与 `serialize_props` 对偶；键缺失则保留现值）。
    /// @note 来源行写进**当前源**而不换绑：外部 `State` 的绑定与订阅数在一次回写后保持不变。
    /// @note 非法值一律不写入：可空行撤表态（生效值退回该行回落点）并经 Diagnostics 上报。
    /// @param props 序列化属性对象，按权威表逐行取键
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        props_deserialize(props);
    }

    /// @brief 触发点击：仅当启用且已设置 `on_click` 时同步调用回调。
    auto activate() -> void override {
        if (enabled() && on_click) {
            on_click();
        }
    }

    /// @brief Button 自带 on_click，作为点击目标消费事件（即便无 Clickable 修饰）。
    /// @return 启用且已设置 `on_click` 时为 true（禁用或无回调时不消费点击）
    [[nodiscard]] auto wants_click() const -> bool override { return enabled() && on_click != nullptr; }

    /// @brief 悬停反馈：状态层随悬停态叠加（默认 `roles.hover`）。
    /// @param entered true=指针进入，false=离开；启用态下据此标记重绘
    auto on_hover_change(bool entered) -> void override {
        Widget::on_hover_change(entered);  // 基类维护 hovered 状态并派发无障碍悬停事件
        if (enabled()) {
            mark_needs_paint();
        }
    }

    /// @brief 按下/松开时重绘以呈现 pressed 态（基类维护 pressed_ 与点击识别）。
    /// @param e 鼠标事件；禁用态直接标记 handled 吞掉点击
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!enabled()) {
            e.is_handled = true;  // 禁用态吞掉点击（不冒泡触发父级点击）
            return;
        }
        Widget::on_pointer_event(e);
        if (e.action == MouseAction::Press || e.action == MouseAction::Release) {
            mark_needs_paint();
        }
    }

  protected:
    /// @brief 度量：文字实测 + 四边内边距，再与最小尺寸取大。
    /// @param c 父级约束
    /// @param ctx 构建上下文：字号、内边距与标签解析都取当前作用域
    /// @return 约束后的按钮尺寸
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        const Font f = effective_font(ctx);
        layout_font_ = f;  // 绘制与命中路径按这份缓存取值，与本轮度量同源（见 layout_font_）
        // 显示串唯一来源：与绘制共用 resolved_label()，不直读 label().text。
        const std::string display = resolved_label(ctx);
        // 内边距与绘制同源：表态过用表态值，未表态随主题间距档（见 padding_default）。
        const EdgeInsets pad = padding(ctx);
        cached_display_text_ = display;
        cached_text_width_ = render::FontEngine::measure_width(display, f);
        cached_text_height_ = render::FontEngine::measure_height(f);
        const float w = std::max(cached_text_width_ + pad.left + pad.right, min_width());
        const float h = std::max(cached_text_height_ + pad.top + pad.bottom, min_height());
        return c.constrain(Size{.width = w, .height = h});
    }

    /// @brief 绘制本帧：解算一次背景/文字色与圆角，再交给三个绘制阶段。
    /// @param p 绘制器
    /// @param bounds 按钮区域
    /// @param ctx 构建上下文：主题作用域与 locale 都经它
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;

    // ---- 继承扩展点：子类可单独覆盖某个绘制阶段，无需重写整个 on_paint ----

    /// @brief 解析当前状态（enabled）下的背景色。
    /// @param ctx 构建上下文：提供主题作用域，未表态时取 `roles.primary`。
    /// @return 启用态取基色（`color` 行），禁用态取禁用色（缺省 `roles.surface_container_high`）。
    [[nodiscard]] virtual auto resolve_background(const BuildContext &ctx) const -> Color {
        return enabled() ? color(ctx) : disabled_color(ctx);
    }

    /// @brief 解析当前状态下的交互叠色（悬停 / 按下）。
    /// @param ctx 构建上下文：未表态时悬停取 `roles.hover`、按下取 `roles.pressed`。
    /// @return 需要叠加的状态层色；无状态层（无指针交互或禁用态）时为空。
    /// @note 叠色是半透明层，绘制路径把它画在基色之上（源覆盖混合），不改基色本身。
    [[nodiscard]] virtual auto resolve_state_overlay(const BuildContext &ctx) const -> std::optional<Color> {
        if (!enabled()) {
            return std::nullopt;
        }
        if (pressed_) {
            return pressed_color(ctx);
        }
        if (hovered()) {
            return hover_color(ctx);
        }
        return std::nullopt;
    }

    /// @brief 解析当前状态下的文字色。
    /// @param ctx 构建上下文：提供主题作用域，未表态时取 `roles.on_primary`。
    /// @return 启用态取 on_color（或其主题回退）；禁用态取禁用文字色（缺省 `roles.outline`）。
    [[nodiscard]] virtual auto resolve_text_color(const BuildContext &ctx) const -> Color {
        return enabled() ? on_color(ctx) : disabled_text_color(ctx);
    }

    /// @brief 绘制背景（圆角填充）。
    /// @param p 绘制器。
    /// @param bounds 按钮区域。
    /// @param bg 当前状态解析出的背景色。
    /// @param radius 本帧生效圆角半径（= `corner_radius(ctx)`，由 `on_paint` 解算一次后
    ///        传给背景与边框两阶段，故二者不会因同帧内主题换档而分叉）。
    virtual auto paint_background(Painter &p, const Rect &bounds, Color bg, float radius) -> void;

    /// @brief 绘制边框（`border_color` 表态且 `border_width` > 0 时）。
    /// @param p 绘制器。
    /// @param bounds 按钮区域。
    /// @param radius 本帧生效圆角半径（与 `paint_background` 同一份，理由同彼）。
    virtual auto paint_border(Painter &p, const Rect &bounds, float radius) -> void;

    /// @brief 解析当前应显示的标签串（i18n 求值）：绘制与布局的**唯一显示串来源**。
    ///
    /// 与 `Text::resolved_text` 同口径：`Environment` 注入了 `Locale` 就按它解析，否则回落
    /// `Locale{}` 走 `default_catalog()` 的缺省档。`on_layout` 与 `on_paint` 各自解析一次
    /// （布局与绘制是两个阶段，无法共享局部量），但**同一阶段内**只解析一次：
    /// `paint_label` 收到的是该串的引用，量宽与落笔必然同值。
    /// @param ctx 构建上下文：提供 `Environment` 注入的 `Locale`。
    /// @return 解析后的 UTF-8 标签；`label` 为字面值时即其原文本。
    /// @note Side-effects: reads i18n table
    [[nodiscard]] auto resolved_label(const BuildContext &ctx) const -> std::string {
        const auto *lp = ctx.environment<Locale>();
        return label().resolve(&default_catalog(), (lp != nullptr) ? *lp : Locale{});
    }

    /// @brief 绘制居中文字。
    /// @param p 绘制器。
    /// @param bounds 按钮区域。
    /// @param text_color 当前状态解析出的文字色。
    /// @param display 本帧应绘制的显示串（= `resolved_label(ctx)` 的同一份副本）：量宽与
    ///        `draw_text` 都取它，故二者不会因同帧内 locale 变化而分叉。子类覆写时应直接
    ///        用该串，不要再自行解析 `label`。
    virtual auto paint_label(Painter &p, const Rect &bounds, Color text_color, const std::string &display) -> void;

    // 缓存供 on_layout / paint_label 使用；受 protected 扩展点约束，有意非 private。
    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    float cached_text_width_ = 0.0F;  ///< 缓存的显示串宽度（dp）
    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    float cached_text_height_ = 0.0F;  ///< 缓存的显示串高度（dp）
    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::string cached_display_text_;  ///< 上次测量所用的显示串：与缓存宽度配套，串变即重测
    // 生效字体缓存供没有 BuildContext 的 `paint_label` 扩展点复用——与 `cached_text_width_` 同源，
    // 否则「按 A 字号度量、按 B 字号落笔」会让文字溢出按钮盒。
    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    Font layout_font_{};  ///< 最近一次布局所得的生效字体（族名 / 字号 / 字重）

  private:
    /// @brief 有效字体：字号取 `font_size` 行在该作用域下的生效值，非正时兜底字阶正文档。
    ///
    /// 这里**不**有任何字号缩放乘法：无障碍 `font_scale` 已在主题物化点折进字阶（见
    /// `Theme::with_accessibility`），未表态字号经 `font_size` 行的派生回落点取那一份折好的快照；
    /// 控件侧再乘一次就是绕过 `TypeScale` 的第二条通道。
    /// @param ctx 构建上下文：字号未表态时据此解主题正文档。
    /// @return 供度量与绘制共用的 `Font`（字号是本控件唯一的字体属性，族名/字重另有 `Text` 一族承载）
    [[nodiscard]] auto effective_font(const BuildContext &ctx) const -> Font {
        const float fs = font_size(ctx);
        return Font{.size_pt = fs > 0.0F ? fs : TypeScale{}.body_medium.size_pt};
    }
};

}  // namespace aurora
