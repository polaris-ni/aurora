#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "aurora/app/clipboard.h"
#include "aurora/core/accessibility.h"
#include "aurora/core/color.h"
#include "aurora/core/diagnostics.h"
#include "aurora/core/directionality.h"
#include "aurora/core/types.h"
#include "aurora/core/utf8.h"
#include "aurora/environment/build_context.h"  // 带上下文的读件需完整类型以解主题作用域
#include "aurora/event/event.h"
#include "aurora/event/keycode.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/theming/color_roles.h"  // 活令牌回落点：roles.on_surface / on_surface_variant / outline ...
#include "aurora/theming/control_metrics.h"  // 活令牌回落点：controls.border_thickness
#include "aurora/theming/theme_scope.h"  // inherit_theme
#include "aurora/widget/props.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief TextInput 的属性权威表：19 行，键名、私有字段、脏档位、约束与回落点只在行里声明一次。
///
/// 本型是属性单一声明面里唯一带**可写来源行**的控件：`value` 的字段是 `State<std::string>` 而不是
/// `ReadOnlyState<std::string>`，因为输入框本体就是这段文本的编辑者——逐字符写回落进**当前来源的
/// 现值**（源不换、句柄不重绑），调用方那只 `State<std::string>` 上的其它订阅照常收到每一轮。换源
/// 仍只有同名写件的句柄重载一条通道，其余来源行「装值即换入自有常量源」的口径在这里不适用。
///
/// 三态语义与其余已迁型一致：
/// - **必填行**（`value`）：字段本身就是生效值，不可撤表态；
/// - **可空行**（其余 18）：未表态时取该行回落点，表态后不再跟随；撤表态走公共面上唯一的 `clear(row)`；
/// - **来源行**（`value`）：公共面上没有 `value_source()` 这类句柄读件，来源位由字段形状推导。
///
/// 回落点三档在本表里同时在场：
/// - 活令牌档（`cursor_color` / `text_color` / `placeholder_color` / `background` /
///   `focused_background` / `border_color` / `focused_border_color` / `selection_color` 之外的
///   `border_width`）：随作用域换肤或切风格档而变，读件带 `BuildContext`；
/// - 常量档（`placeholder` / `font_size` / `corner_radius` / `padding` / `enabled` / `max_length` /
///   `read_only` / `obscure_text` / `direction`）：全库唯一基线，读件无参；
/// - 派生档（`selection_color`）：回落点由当前作用域的 `roles.primary` 调出半透明高亮，读件带
///   `BuildContext`，schema 里的默认值只能是以 `.shows()` 给出的显示串。
///
/// `direction` 的「继承」语义留在表外（与 `Text` 同口径）：本行回落基线是 `LTR`（读件无参），控件
/// 判定生效方向时先问表态位 `present(direction_row())`——未表态即走 `Environment` / 进程级继承，
/// 与迁移前的 `std::optional<TextDirection>` 空态等价。
///
/// @note Thread: main-thread only
/// @note Side-effects: 写入经宿主链接位标脏并重建订阅；未挂载时是纯 DTO
class TextInputProps : public AbstractProps<TextInputProps> {
  public:
    // ---- 权威表：参数顺序即 schema 顺序 ----

    /// @brief 必填可写来源件：输入框文本。变更同时影响度量与绘制 ⇒ 布局 + 绘制两档都打。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto value_row() {
        return prop_row<&TextInputProps::value_>("value").layout().paint().required().note("Text value");
    }

    /// @brief 可空值件：空值时的占位提示（同时充当无障碍名称）。参与度量 ⇒ 布局档。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto placeholder_row() {
        return prop_row<&TextInputProps::placeholder_>("placeholder")
            .layout()
            .paint()
            .fallback(&TextInputProps::placeholder_default)
            .note("Placeholder shown when empty");
    }

    /// @brief 可空值件：字号（pt）。参与度量 ⇒ 布局档；下界 0。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto font_size_row() {
        return prop_row<&TextInputProps::font_size_>("font_size")
            .layout()
            .paint()
            .bounds("0")
            .fallback(&TextInputProps::font_size_default)
            .note("Font size (pt)");
    }

    /// @brief 可空值件：圆角半径（dp）；0 = 直角。只改背景与裁剪形状，不改度量 ⇒ 绘制档。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto corner_radius_row() {
        return prop_row<&TextInputProps::corner_radius_>("corner_radius")
            .paint()
            .bounds("0")
            .fallback(&TextInputProps::corner_radius_default)
            .note("Corner radius (dp)");
    }

    /// @brief 可空值件：文本与边框之间的内边距。参与度量 ⇒ 布局档。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto padding_row() {
        return prop_row<&TextInputProps::padding_>("padding")
            .layout()
            .paint()
            .bounds("0")
            .fallback(&TextInputProps::padding_default)
            .note("Padding");
    }

    /// @brief 可空值件：光标色。未表态时取主题 `roles.on_surface`，换肤即随动。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto cursor_color_row() {
        return prop_row<&TextInputProps::cursor_color_>("cursor_color")
            .paint()
            .fallback<&ColorRoles::on_surface>()
            .note("Cursor color (follows theme on_surface when unset)");
    }

    /// @brief 可空值件：是否可编辑。禁用态只降级绘制并忽略输入，不改度量 ⇒ 绘制档。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto enabled_row() {
        return prop_row<&TextInputProps::enabled_>("enabled")
            .paint()
            .fallback(&TextInputProps::enabled_default)
            .note("Editable");
    }

    /// @brief 可空值件：文本色。未表态时取主题 `roles.on_surface`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto text_color_row() {
        return prop_row<&TextInputProps::text_color_>("text_color")
            .paint()
            .fallback<&ColorRoles::on_surface>()
            .note("Text color (follows theme on_surface when unset)");
    }

    /// @brief 可空值件：占位提示色。未表态时取主题 `roles.on_surface_variant`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto placeholder_color_row() {
        return prop_row<&TextInputProps::placeholder_color_>("placeholder_color")
            .paint()
            .fallback<&ColorRoles::on_surface_variant>()
            .note("Placeholder text color (follows theme on_surface_variant when unset)");
    }

    /// @brief 可空值件：常态背景色。未表态时取主题 `roles.surface_container`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto background_row() {
        return prop_row<&TextInputProps::background_>("background")
            .paint()
            .fallback<&ColorRoles::surface_container>()
            .note("Background color (follows theme surface_container when unset)");
    }

    /// @brief 可空值件：聚焦态背景色。未表态时取主题 `roles.surface_bright`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto focused_background_row() {
        return prop_row<&TextInputProps::focused_background_>("focused_background")
            .paint()
            .fallback<&ColorRoles::surface_bright>()
            .note("Focused background color (follows theme surface_bright when unset)");
    }

    /// @brief 可空值件：常态边框色。未表态时取主题 `roles.outline`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto border_color_row() {
        return prop_row<&TextInputProps::border_color_>("border_color")
            .paint()
            .fallback<&ColorRoles::outline>()
            .note("Border color (follows theme outline when unset)");
    }

    /// @brief 可空值件：聚焦态边框色。未表态时取主题 `roles.primary`（Fluent 式聚焦提示）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto focused_border_color_row() {
        return prop_row<&TextInputProps::focused_border_color_>("focused_border_color")
            .paint()
            .fallback<&ColorRoles::primary>()
            .note("Focused border color (follows theme primary when unset)");
    }

    /// @brief 可空值件：边框线宽（dp）；0 = 不描边。未表态时取主题 `controls.border_thickness`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto border_width_row() {
        return prop_row<&TextInputProps::border_width_>("border_width")
            .paint()
            .bounds("0")
            .fallback<&ControlMetrics::border_thickness>()
            .note("Border width (dp); 0 = no border");
    }

    /// @brief 可空值件：选区高亮色。派生档由当前作用域主色调得半透明高亮 ⇒ 默认值只能是显示串。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto selection_color_row() {
        return prop_row<&TextInputProps::selection_color_>("selection_color")
            .paint()
            .fallback(&TextInputProps::selection_color_default)
            .shows("auto")
            .note("Selection highlight color (follows theme primary when unset)");
    }

    /// @brief 可空值件：最大长度（码点，0 = 不限）。只约束后续编辑，不改变已有画面 ⇒ 无脏档。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto max_length_row() {
        return prop_row<&TextInputProps::max_length_>("max_length")
            .bounds("0")
            .fallback(&TextInputProps::max_length_default)
            .note("Max length (code points); 0 = unlimited");
    }

    /// @brief 可空值件：只读位（可选择/复制、不可编辑）。纯行为门，不改画面 ⇒ 无脏档。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto read_only_row() {
        return prop_row<&TextInputProps::read_only_>("read_only")
            .fallback(&TextInputProps::read_only_default)
            .note("Read-only (selectable and copyable, not editable)");
    }

    /// @brief 可空值件：密码掩码显示。实显串随之改变 ⇒ 布局 + 绘制两档。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto obscure_text_row() {
        return prop_row<&TextInputProps::obscure_text_>("obscure_text")
            .layout()
            .paint()
            .fallback(&TextInputProps::obscure_text_default)
            .note("Password masked display");
    }

    /// @brief 可空值件：书写方向。回落基线只是读件取值，是否继承另看表态位（见类注释）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto direction_row() {
        return prop_row<&TextInputProps::direction_>("direction")
            .layout()
            .paint()
            .fallback(&TextInputProps::direction_default)
            .shows("auto")
            .note("Text direction (auto = inherit from environment)");
    }

    /// @brief 权威表本体：本控件全部属性行的元组。
    /// @return 按声明顺序排列的行元组（发射面与序列化面都遍历它）。
    [[nodiscard]] static constexpr auto rows() {
        return make_prop_rows(value_row(), placeholder_row(), font_size_row(), corner_radius_row(), padding_row(),
                              cursor_color_row(), enabled_row(), text_color_row(), placeholder_color_row(),
                              background_row(), focused_background_row(), border_color_row(),
                              focused_border_color_row(), border_width_row(), selection_color_row(), max_length_row(),
                              read_only_row(), obscure_text_row(), direction_row());
    }

    // ---- 读面：每行恰一件，形态由该行回落点决定 ----

    /// @brief 当前文本值（必填可写来源件：无参读件）。
    /// @return 文本的 UTF-8 副本（不含组合期 preedit）。
    [[nodiscard]] auto value() const -> std::string { return get(value_row()); }

    /// @brief 当前生效占位提示。
    /// @return 占位文本；未表态时为空串。
    [[nodiscard]] auto placeholder() const -> std::string { return stated(placeholder_row()); }

    /// @brief 当前生效字号（pt）：常量档回落点不随作用域变 ⇒ 无参读件。
    /// @return 字号（pt）。
    [[nodiscard]] auto font_size() const -> float { return stated(font_size_row()); }

    /// @brief 当前生效圆角半径（dp）。
    /// @return 半径；0 = 直角。
    [[nodiscard]] auto corner_radius() const -> float { return stated(corner_radius_row()); }

    /// @brief 当前生效内边距。
    /// @return 四边内边距。
    [[nodiscard]] auto padding() const -> EdgeInsets { return stated(padding_row()); }

    /// @brief 当前作用域下生效的光标色。
    /// @param ctx 构建上下文：未表态时据此解主题 `roles.on_surface`。
    /// @return 生效光标色。
    [[nodiscard]] auto cursor_color(const BuildContext &ctx) const -> Color { return resolve(cursor_color_row(), ctx); }

    /// @brief 当前是否可编辑。
    /// @return 生效的启用位。
    [[nodiscard]] auto enabled() const -> bool { return stated(enabled_row()); }

    /// @brief 当前作用域下生效的文本色。
    /// @param ctx 构建上下文：未表态时据此解主题 `roles.on_surface`。
    /// @return 生效文本色。
    [[nodiscard]] auto text_color(const BuildContext &ctx) const -> Color { return resolve(text_color_row(), ctx); }

    /// @brief 当前作用域下生效的占位提示色。
    /// @param ctx 构建上下文：未表态时据此解主题 `roles.on_surface_variant`。
    /// @return 生效占位色。
    [[nodiscard]] auto placeholder_color(const BuildContext &ctx) const -> Color {
        return resolve(placeholder_color_row(), ctx);
    }

    /// @brief 当前作用域下生效的常态背景色。
    /// @param ctx 构建上下文：未表态时据此解主题 `roles.surface_container`。
    /// @return 生效背景色。
    [[nodiscard]] auto background(const BuildContext &ctx) const -> Color { return resolve(background_row(), ctx); }

    /// @brief 当前作用域下生效的聚焦态背景色。
    /// @param ctx 构建上下文：未表态时据此解主题 `roles.surface_bright`。
    /// @return 生效聚焦背景色。
    [[nodiscard]] auto focused_background(const BuildContext &ctx) const -> Color {
        return resolve(focused_background_row(), ctx);
    }

    /// @brief 当前作用域下生效的常态边框色。
    /// @param ctx 构建上下文：未表态时据此解主题 `roles.outline`。
    /// @return 生效边框色。
    [[nodiscard]] auto border_color(const BuildContext &ctx) const -> Color { return resolve(border_color_row(), ctx); }

    /// @brief 当前作用域下生效的聚焦态边框色。
    /// @param ctx 构建上下文：未表态时据此解主题 `roles.primary`。
    /// @return 生效聚焦边框色。
    [[nodiscard]] auto focused_border_color(const BuildContext &ctx) const -> Color {
        return resolve(focused_border_color_row(), ctx);
    }

    /// @brief 当前作用域下生效的边框线宽（dp）。
    /// @param ctx 构建上下文：未表态时据此解主题 `controls.border_thickness`。
    /// @return 生效线宽；0 = 不描边。
    [[nodiscard]] auto border_width(const BuildContext &ctx) const -> float { return resolve(border_width_row(), ctx); }

    /// @brief 当前作用域下生效的选区高亮色。
    /// @param ctx 构建上下文：未表态时据当前作用域主色调出半透明高亮。
    /// @return 生效选区色。
    [[nodiscard]] auto selection_color(const BuildContext &ctx) const -> Color {
        return resolve(selection_color_row(), ctx);
    }

    /// @brief 当前生效长度上限（码点计数）。
    /// @return 上限；0 = 不限。
    [[nodiscard]] auto max_length() const -> std::size_t { return stated(max_length_row()); }

    /// @brief 当前是否处于只读态。
    /// @return true = 只读（可选择/复制，不可编辑）。
    [[nodiscard]] auto read_only() const -> bool { return stated(read_only_row()); }

    /// @brief 当前是否掩码显示。
    /// @return true = 密码掩码（实显为等长 • 串，值不变）。
    [[nodiscard]] auto obscure_text() const -> bool { return stated(obscure_text_row()); }

    /// @brief 当前方向表态值；是否继承环境另由 `present(direction_row())` 判定（见类注释）。
    /// @return 表态的方向；未表态时为回落基线 `LTR`。
    [[nodiscard]] auto direction() const -> TextDirection { return stated(direction_row()); }

    // ---- 写面：同名重载区分「装入值」与「换源」，档位仍由行决定 ----

    /// @brief 写入文本值（可写来源件：落进**当前来源的现值**，不换源、不重绑）。
    /// @param value 新的 UTF-8 文本；同值写入短路（不标脏、不发轮次）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_value(const std::string &value) -> TextInputProps & { return assign(value_row(), value); }

    /// @brief 写入文本值（C 字符串通道：消除字面量在值形参与句柄形参之间的重载歧义）。
    /// @param value 新的 UTF-8 文本；nullptr 按空串处理。
    /// @return 引用本属性对象，便于链式续写。
    auto set_value(const char *value) -> TextInputProps & {
        return set_value(value != nullptr ? std::string{value} : std::string{});
    }

    /// @brief 换绑文本来源（响应式换源：本控件与外部 `State` 从此共用同一来源）。
    ///
    /// 本库第一枚双向绑定：控件本体的逐字符编辑落进换入的那只来源，外部同一只句柄上的其它订阅
    /// 照常收到每一轮 —— 可写来源行只接受 `State`（不接受 `ReadOnlyState`），因为本控件要写回去。
    /// @param source 新的可写句柄来源。
    /// @return 引用本属性对象，便于链式续写。
    auto set_value(State<std::string> source) -> TextInputProps & { return rebind(value_row(), std::move(source)); }

    /// @brief 装入占位提示。
    /// @param value 空值时显示的文本（也充当无障碍名称）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_placeholder(const std::string &value) -> TextInputProps & { return assign(placeholder_row(), value); }

    /// @brief 装入字号（pt）。
    /// @param value 新字号；<= 0 时绘制/度量按 14pt 兜底（见 `TextInput::effective_font_size`）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_font_size(float value) -> TextInputProps & { return assign(font_size_row(), value); }

    /// @brief 装入圆角半径（dp）；0 是「显式要直角」，与「未表态」不是同一件事。
    /// @param value 新半径。
    /// @return 引用本属性对象，便于链式续写。
    auto set_corner_radius(float value) -> TextInputProps & { return assign(corner_radius_row(), value); }

    /// @brief 装入内边距。
    /// @param value 新的四边内边距。
    /// @return 引用本属性对象，便于链式续写。
    auto set_padding(EdgeInsets value) -> TextInputProps & { return assign(padding_row(), value); }

    /// @brief 装入光标色常量值（表态即脱离主题 `roles.on_surface`）。
    /// @param value 新光标色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_cursor_color(Color value) -> TextInputProps & { return assign(cursor_color_row(), value); }

    /// @brief 装入启用位；false = 禁用（忽略指针/键盘输入，无障碍状态并入只读）。
    /// @param value 新的启用位。
    /// @return 引用本属性对象，便于链式续写。
    auto set_enabled(bool value) -> TextInputProps & { return assign(enabled_row(), value); }

    /// @brief 装入文本色（表态即脱离主题 `roles.on_surface`）。
    /// @param value 新文本色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_text_color(Color value) -> TextInputProps & { return assign(text_color_row(), value); }

    /// @brief 装入占位提示色（表态即脱离主题 `roles.on_surface_variant`）。
    /// @param value 新占位色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_placeholder_color(Color value) -> TextInputProps & { return assign(placeholder_color_row(), value); }

    /// @brief 装入常态背景色（表态即脱离主题 `roles.surface_container`）。
    /// @param value 新背景色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_background(Color value) -> TextInputProps & { return assign(background_row(), value); }

    /// @brief 装入聚焦态背景色（表态即脱离主题 `roles.surface_bright`）。
    /// @param value 新聚焦背景色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_focused_background(Color value) -> TextInputProps & { return assign(focused_background_row(), value); }

    /// @brief 装入常态边框色（表态即脱离主题 `roles.outline`）。
    /// @param value 新边框色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_border_color(Color value) -> TextInputProps & { return assign(border_color_row(), value); }

    /// @brief 装入聚焦态边框色（表态即脱离主题 `roles.primary`）。
    /// @param value 新聚焦边框色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_focused_border_color(Color value) -> TextInputProps & { return assign(focused_border_color_row(), value); }

    /// @brief 装入边框线宽（dp）；0 = 不描边；表态即脱离主题 `controls.border_thickness`。
    /// @param value 新线宽；不再夹取负值（负线宽按「不描边」绘制处理）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_border_width(float value) -> TextInputProps & { return assign(border_width_row(), value); }

    /// @brief 装入选区高亮色（表态即脱离「主色半透明」派生回落）。
    /// @param value 新选区色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_selection_color(Color value) -> TextInputProps & { return assign(selection_color_row(), value); }

    /// @brief 装入长度上限（码点计数，0 = 不限）。键盘输入、粘贴与无障碍替换共用同一截断口径。
    /// @param value 码点数上限。
    /// @return 引用本属性对象，便于链式续写。
    auto set_max_length(std::size_t value) -> TextInputProps & { return assign(max_length_row(), value); }

    /// @brief 装入只读位；只读态可选择/复制但不可编辑（对标 Qt QLineEdit::readOnly）。
    /// @param value true = 只读（光标与选区仍可移动）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_read_only(bool value) -> TextInputProps & { return assign(read_only_row(), value); }

    /// @brief 装入密码掩码位；显示为 •，值不变（对标 Flutter obscureText）。
    /// @param value true = 掩码显示（命中/选区按逐字符对应的掩码串定位）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_obscure_text(bool value) -> TextInputProps & { return assign(obscure_text_row(), value); }

    /// @brief 装入书写方向（表态即覆盖 `Environment` 与进程级来源）。
    /// @param value 新方向；RTL 时光标/命中走逻辑↔视觉镜像、方向键反转。
    /// @return 引用本属性对象，便于链式续写。
    auto set_direction(TextDirection value) -> TextInputProps & { return assign(direction_row(), value); }

    // ---- 回落点提供者：常量档与派生档的唯一基线（同族互读只经读件） ----

    /// @brief 占位提示回落基线。
    /// @return 空串（无占位）。
    [[nodiscard]] static auto placeholder_default() -> std::string { return {}; }

    /// @brief 字号回落基线（pt）。
    /// @return 14.0F。
    [[nodiscard]] static auto font_size_default() -> float { return 14.0F; }

    /// @brief 圆角回落基线。
    /// @return 0.0F（直角）。
    [[nodiscard]] static auto corner_radius_default() -> float { return 0.0F; }

    /// @brief 内边距回落基线：四边同为 12dp（复现令牌层落地前的字段初值）。
    /// @return 四边 12dp 的内边距。
    [[nodiscard]] static auto padding_default() -> EdgeInsets {
        return EdgeInsets{.left = 12.0F, .top = 12.0F, .right = 12.0F, .bottom = 12.0F};
    }

    /// @brief 启用位回落基线。
    /// @return true（可编辑）。
    [[nodiscard]] static auto enabled_default() -> bool { return true; }

    /// @brief 长度上限回落基线。
    /// @return 0（不限）。
    [[nodiscard]] static auto max_length_default() -> std::size_t { return 0U; }

    /// @brief 只读位回落基线。
    /// @return false（可编辑）。
    [[nodiscard]] static auto read_only_default() -> bool { return false; }

    /// @brief 掩码位回落基线。
    /// @return false（明文显示）。
    [[nodiscard]] static auto obscure_text_default() -> bool { return false; }

    /// @brief 方向回落基线：仅作读件取值，继承语义由表态位判定（见类注释）。
    /// @return `TextDirection::LTR`。
    [[nodiscard]] static auto direction_default() -> TextDirection { return TextDirection::LTR; }

    /// @brief 选区高亮色回落点（派生档）：取当前作用域主色、压到迁移前那枚字面量的透明度档。
    /// @param self 本属性对象（本回落点不读同族他行，形参由派生档签名固定）。
    /// @param ctx 构建上下文：提供主题作用域。
    /// @return 主色的 alpha = 90/255 版本（与迁移前 `{80,120,220,90}` 同透明度档、同「随主色」语义）。
    [[nodiscard]] static auto selection_color_default(const TextInputProps &self, const BuildContext &ctx) -> Color {
        (void)self;
        return inherit_theme(ctx).roles.primary.with_alpha(90);
    }

  private:
    State<std::string> value_;  ///< 文本来源（必填 ⇒ 裸句柄；可写 ⇒ 编辑落进当前源）
    std::optional<std::string> placeholder_;  ///< 占位提示表态位
    std::optional<float> font_size_;  ///< 字号表态位（pt）
    std::optional<float> corner_radius_;  ///< 圆角半径表态位（dp）
    std::optional<EdgeInsets> padding_;  ///< 内边距表态位
    std::optional<Color> cursor_color_;  ///< 光标色表态位
    std::optional<bool> enabled_;  ///< 启用位表态位
    std::optional<Color> text_color_;  ///< 文本色表态位
    std::optional<Color> placeholder_color_;  ///< 占位色表态位
    std::optional<Color> background_;  ///< 常态背景色表态位
    std::optional<Color> focused_background_;  ///< 聚焦背景色表态位
    std::optional<Color> border_color_;  ///< 常态边框色表态位
    std::optional<Color> focused_border_color_;  ///< 聚焦边框色表态位
    std::optional<float> border_width_;  ///< 边框线宽表态位（dp）
    std::optional<Color> selection_color_;  ///< 选区高亮色表态位
    std::optional<std::size_t> max_length_;  ///< 长度上限表态位（码点）
    std::optional<bool> read_only_;  ///< 只读位表态位
    std::optional<bool> obscure_text_;  ///< 掩码位表态位
    std::optional<TextDirection> direction_;  ///< 书写方向表态位

    friend class AbstractProps<TextInputProps>;
};

/// @brief 文本输入框（叶控件）：绘制边框 + 文本（值或占位）+ 光标。
///
/// - 点击经 `FocusManager::request_focus` 获焦（触发 `on_focus_change` 重绘聚焦态）。
/// - 聚焦后接收 `onTextInput` 追加字符、`onKeyEvent` 退格（Delete/Backspace）。
/// - 值是 `State<std::string>`（可写来源行），变化触发重绘；headless 下静态渲染当前文本/占位。
/// - 焦点全局协调由 `FocusManager` 负责（Tab 序、焦点派发），见 `aurora/event/focus.h`。
///
/// 属性一律经 `TextInputProps` 的读写件访问（本类公有继承它，故 `box.set_max_length(8U)` 与
/// `box.max_length()` 都直接可用）；`au::TextInput(au::TextInputProps().set_placeholder("Name"))`
/// 与逐个 `set_*` 等价。
///
/// 可定制性（对标 Qt QLineEdit / Flutter TextField）：
/// - 颜色：文本/占位/背景/聚焦背景/边框/聚焦边框（缺省跟随主题对应角色）/选区高亮；
/// - 行为：`max_length` 限长（码点计数）、`read_only` 只读（可选可复制不可编辑）、
/// `obscure_text` 密码掩码显示；
/// - 回调：`on_changed`（每次编辑）与 `on_submit`（Enter 提交）。
/// - 窄盒长文本：内容（选区/组合串/正文/光标）裁进自身盒，水平视口随光标自动滚动
///   （`ensure_caret_visible`），不会画到相邻控件上。
///
/// 继承扩展点（protected 虚函数）：`paint_frame`（背景+边框）；内部状态（选区/光标/组合串）
/// 为 protected，子类可直接复用。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class TextInput : public LeafWidget, public TextInputProps {
  public:
    /// @brief 默认构造：各行未表态，生效值即该行回落点（空文本、无占位、14pt、四边 12dp、跟随主题）。
    TextInput() { bind(*this); }

    /// @brief 配置块构造：整体接管 `TextInputProps` 各行表态位。
    /// @param props 属性对象（链式写件产出的对象），移入本对象基类子对象。
    explicit TextInput(TextInputProps props) : TextInputProps(std::move(props)) { bind(*this); }

    /// @brief 把属性面的同名读件带回重载集。
    ///
    /// 控件侧 `set_value(...)` 覆写要发无障碍事件，与属性面同名写件共存，显式 `using` 把属性面
    /// 的句柄重载带回本类重载集（否则 `set_value(State<std::string>)` 在控件上不可见）。
    using TextInputProps::set_value;

    /// @brief 写入文本值（链式）：属性面写入之后补一次 ValueChanged 无障碍事件。
    ///
    /// 外部整体赋值就是「值变了」这一语义事件（迁移前行为，读屏与单测都依赖它）；控件内部的编辑
    /// 路径（键入 / 粘贴 / 删除 / 无障碍替换）不经本件，改由 `notify_changed()` 统一上报，故不双发。
    /// @param v 新的 UTF-8 文本。
    /// @return 本控件引用（`*this`），支持链式调用。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_value(const std::string &v) -> TextInput & {
        TextInputProps::set_value(v);
        notify_accessibility_event(AccessibilityEvent{.kind = AccessibilityEventKind::ValueChanged, .target = this});
        return *this;
    }

    /// @brief 运行时可查询的默认属性值（属性表的单一事实来源）。
    /// @return 一份缺省构造的 `TextInputProps`：各行未表态，生效值即该行回落点。
    [[nodiscard]] static auto defaults() -> TextInputProps { return {}; }

    /// @brief 设置变化回调（链式）；每次用户编辑（输入/退格/剪切/粘贴）后触发。
    /// @param cb 回调，收参为编辑后的最新文本（UTF-8）。
    /// @return 本控件引用（`*this`），支持链式调用。
    auto set_on_changed(std::function<void(const std::string &)> cb) -> TextInput & {
        on_changed_ = std::move(cb);
        return *this;
    }

    /// @brief 设置提交回调（链式）；Enter 键触发。
    /// @param cb 回调，收参为提交时的当前文本（UTF-8）。
    /// @return 本控件引用（`*this`），支持链式调用。
    auto set_on_submit(std::function<void(const std::string &)> cb) -> TextInput & {
        on_submit_ = std::move(cb);
        return *this;
    }

    /// @brief 登记响应式信号：全部来源行经机制层采集（本表即 `value` 一枚可写来源行）。
    /// @param out 输出参数：追加本控件依赖的信号视图指针。
    auto collect_signals(StateSink &out) -> void override { collect_prop_sources(out); }

    /// @brief 类型名（注册 / 诊断 / Inspector 通道）。
    /// @return "TextInput"。
    [[nodiscard]] auto type_name() const -> const char * override { return "TextInput"; }

    /// @brief 首行基线（内容盒顶 → 基线）：与 `on_paint` 的文本原点 `ty` 同源——
    ///        上内边距 + 文本在左右内边距之内的垂直居中偏移 + 有效字体 ascent。
    /// @param ctx 构建上下文（未使用；签名与协议保持一致）。
    /// @return 按当前盒尺寸与字体度量算出的基线距离（恒有值）。
    [[nodiscard]] auto baseline_distance([[maybe_unused]] const BuildContext &ctx) const
        -> std::optional<float> override {
        const Font f{.size_pt = effective_font_size()};
        const float th = render::FontEngine::measure_height(f);
        const EdgeInsets pad = padding();
        return pad.top + ((size().height - pad.top - pad.bottom - th) * 0.5F) +
               render::FontEngine::measure_ascent(f);  // 末项加真实字体 ascent，得到内容盒顶 → 基线距离。
    }

    /// @brief 无障碍名称：无宿主覆写时以占位提示充当（编辑框的屏幕阅读器惯例）。
    /// @return 占位提示文本（UTF-8；未设置占位时为空串）。
    /// @note Side-effects: pure
    [[nodiscard]] auto accessibility_label() const -> std::string override { return placeholder(); }

    /// @brief 无障碍值：取当前编辑内容（`value()`）。
    /// @return 当前文本；组合态含光标处的 preedit（与绘制显示一致）。
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_value() const -> std::string override { return composed_text(value()); }

    // ---- 无障碍语义：状态 + TextPattern 支撑的四组文本钩子 + Value 动作 ----

    /// @brief 无障碍状态：只读（含禁用）、密码掩码（读屏不得逐字朗出）、单行。
    /// @return 基类通用状态叠加输入框特有字段后的结果（multiline 恒 false）。
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_state() const -> AccessibilityState override {
        AccessibilityState s = Widget::accessibility_state();  // 先取基类通用状态，再覆写输入框特有字段。
        s.read_only = read_only() || !enabled();
        s.disabled = !enabled();
        s.password = obscure_text();
        s.multiline = false;  // 单行输入框（UIA Edit；多行控件覆写为 true → Document）
        return s;
    }

    /// @brief 无障碍文本全文（UTF-8）：文本来源的现值，**不含**组合期 preedit
    ///        （组合中文本属未确定态，读屏读 value 即可）。
    /// @return 全文的 UTF-8 视图（空文本时为空串）。
    /// @note 返回视图指向来源的存量字符串：控件存活期间有效。
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_text() const -> std::string_view override { return value_ref(); }

    /// @brief 无障碍选区：内部「含头含尾码点下标」模型 → UTF-8 字节半开区间 `[start, end)`。
    /// @return 选区字节区间；无选区时 start == end（光标位置），恒有值。
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_selection() const -> std::optional<AccessibilityTextSelection> override {
        const std::string &v = value_ref();
        AccessibilityTextSelection sel;  // 待填的 UTF-8 字节半开区间；无选区时两端重合于光标。
        if (has_selection()) {
            const std::size_t a = std::min(sel_start_, sel_end_);
            const std::size_t b = std::max(sel_start_, sel_end_) + 1U;  // 含尾 → 半开
            sel.start = cp_to_byte(v, a);
            sel.end = cp_to_byte(v, b);
        } else {
            const std::size_t at = cp_to_byte(v, caret_);
            sel.start = at;
            sel.end = at;  // 空选区 = 光标位置
        }
        return sel;
    }

    /// @brief 设置选区（UTF-8 字节半开区间 → 内部码点模型）；只读态仍允许移动光标/选区。
    /// @param start 半开区间起点（UTF-8 字节偏移，越界夹紧到文本尾）。
    /// @param end 半开区间终点（等于 start = 清空选区、光标落位）。
    /// @note Side-effects: mutates selection state
    auto accessibility_set_selection(std::size_t start, std::size_t end) -> void override {
        const std::string &v = value_ref();
        const std::size_t b0 = byte_to_cp(v, std::min(start, v.size()));
        const std::size_t b1 = byte_to_cp(v, std::min(end, v.size()));
        const std::size_t a = std::min(b0, b1);
        const std::size_t b = std::max(b0, b1);
        sel_start_ = a;
        if (b <= a) {
            sel_end_ = AURORA_NO_SEL;
            caret_ = a;
        } else {
            sel_end_ = b - 1U;  // 含尾模型
            caret_ = b;
        }
        selecting_ = false;
        ensure_caret_visible();
        mark_needs_paint();
    }

    /// @brief 单字符盒（**窗口本地 DIP**）：经 `FontEngine::caret_x` 取该字符左右边界。
    ///
    /// @param utf8_index 字符起点的 UTF-8 字节偏移（越界返回 nullopt）。
    /// @return 该字符的矩形盒；无字体度量（Headless）或未布局（零尺寸盒）时为 nullopt。
    /// @note 无字体度量时（Headless 不加载字体）返回 nullopt —— 契约是「不崩溃」，
    ///       几何断言只能靠真机探针（`tools/verify/win32_ua_live_probe`）。
    /// @note Side-effects: reads layout/state
    [[nodiscard]] auto accessibility_char_bounds(std::size_t utf8_index) const -> std::optional<Rect> override {
        const std::string &v = value_ref();
        if (utf8_index >= v.size()) {
            return std::nullopt;
        }
        const Rect box = paint_bounds();
        if (box.size.width <= 0.0F || box.size.height <= 0.0F) {
            return std::nullopt;
        }
        const Font f{.size_pt = effective_font_size()};
        if (!v.empty() && render::FontEngine::measure_width(v, f) <= 0.0F) {
            return std::nullopt;  // 无可用字体：度量不可用，交由真机验证
        }
        const std::size_t cp = byte_to_cp(v, utf8_index);
        const std::string shown = composed_text(display_value());
        const float text_w = render::FontEngine::measure_width(shown, f, layout_opts());
        const float area_w = text_area_width(box.size.width);
        const float tx = box.origin.x + text_origin_offset(text_w, area_w);
        const float x0 = render::FontEngine::caret_x(v, cp, f, layout_opts());
        const float x1 = render::FontEngine::caret_x(v, cp + 1, f, layout_opts());
        const float th = render::FontEngine::measure_height(f);
        return Rect{.origin = Point{.x = tx + x0, .y = box.origin.y + padding().top},
                    .size = Size{.width = std::max(0.0F, x1 - x0), .height = th}};
    }

    /// @brief 替换文本（UTF-8 字节半开区间）：读屏编辑动作（`ITextRangeProvider` / AT-SPI2
    ///        `EditableText`）的落点；只读 / 禁用时 no-op。
    /// @param start 替换区间起点（UTF-8 字节偏移，越界夹紧）。
    /// @param end 替换区间终点（半开；小于 start 时按 start 收敛）。
    /// @param utf8 新文本（UTF-8）；超过 `max_length` 剩余额度按码点截断。
    /// @note Side-effects: mutates state（写回文本来源，触发 `on_changed` 与 ValueChanged 事件）
    auto accessibility_replace_text(std::size_t start, std::size_t end, std::string_view utf8) -> void override {
        if (read_only() || !enabled()) {
            return;
        }
        const std::string v = value();
        const std::size_t a = std::min(start, v.size());
        const std::size_t b = std::clamp(end, a, v.size());
        std::string next = v.substr(0, a);
        next.append(utf8);
        next.append(v, b, std::string::npos);
        const std::size_t limit = max_length();
        if (limit > 0 && cp_count(next) > limit) {
            next = cp_slice(next, 0, limit);  // 限长按码点截断（与键盘输入同口径）
        }
        TextInputProps::set_value(next);  // 值件形参按 const 引用（无移动可发生）
        const std::size_t caret_cp = byte_to_cp(value_ref(), std::min(a + utf8.size(), value_ref().size()));
        caret_ = caret_cp;
        sel_start_ = caret_cp;
        sel_end_ = AURORA_NO_SEL;
        ensure_caret_visible();
        mark_needs_layout();
        mark_needs_paint();
        if (on_changed_) {
            on_changed_(value_ref());
        }
        notify_accessibility_event(AccessibilityEvent{.kind = AccessibilityEventKind::ValueChanged, .target = this});
    }

    /// @brief 读屏 Value 动作：整值替换（与 `IValueProvider::SetValue` 同语义）；
    ///        Focus 走基类（另把光标置于文末）。
    /// @param req 动作请求；`Value` 时取 `req.text` 作为新值。
    /// @return true = 动作已被本控件消费；false = 转交基类处理。
    /// @note Side-effects: mutates state
    auto perform_accessibility_action(const AccessibilityActionRequest &req) -> bool override {
        if (req.action == AccessibilityAction::Value) {
            accessibility_replace_text(0, value_ref().size(), req.text);
            return true;
        }
        return Widget::perform_accessibility_action(req);
    }

    /// @brief 悬停默认文本光标：输入框悬停 IBeam；修饰链显式 `cursor(...)` 声明优先。
    /// @return 恒为 `CursorShape::IBeam`。
    /// @note Side-effects: pure
    [[nodiscard]] auto cursor_shape() const -> std::optional<CursorShape> override { return CursorShape::IBeam; }

    /// @brief Enter 优先经 `on_key_event` 投递（提交回调 `on_submit` 依赖它，而非激活语义）。
    /// @return 恒为 true。
    /// @note Side-effects: pure
    [[nodiscard]] auto wants_activation_keys() const -> bool override { return true; }

    /// @brief 方向键优先经 `on_key_event` 投递（←/→ 光标移动与 Shift 扩选依赖它，而非几何焦点导航）。
    ///
    /// 单行输入框不认领 ↑/↓，二者仍回落焦点导航（Tab 序同样可用），见 `Widget::wants_navigation_keys()`。
    /// @return 恒为 true（仅 ←/→ 被认领）。
    /// @note Side-effects: pure
    [[nodiscard]] auto wants_navigation_keys() const -> bool override { return true; }

    /// @brief 运行时自描述（规格附录 B）：属性项全部由权威表发射，控件只补自己的事实。
    /// @return 名为 "TextInput" 的描述表：`properties` 按 `rows()` 点名顺序，事件含三枚回调，无子节点。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        auto descriptor = props_descriptor("TextInput");
        descriptor.events = {"on_focus_change", "on_changed", "on_submit"};
        descriptor.children_policy = "none";
        descriptor.examples = {"au::TextInput(au::TextInputProps().set_placeholder(\"Enter name\"))",
                               "au::TextInput(au::TextInputProps().set_max_length(11U).set_obscure_text(true))"};
        return descriptor;
    }

    /// @brief 实例侧自描述：直接转发静态 `describe_static()`（属性表与实例状态无关）。
    /// @return 与 `describe_static()` 相同的描述符。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 序列化属性：先由基类写通用布局字段，再按权威表逐行写出（未表态的行不产生键）。
    /// @param props 目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props_serialize(props);
    }

    /// @brief 反序列化属性（Factory `from_json` 重建通道）：键与 `serialize_props` 同名，
    ///        缺键保持现值；`value` 是可写来源行，故写进**当前源**而不换绑，外部绑定照常保住。
    /// @note 非法值一律不写入：可空行撤表态（生效值退回该行回落点）并经 Diagnostics 上报。
    /// @param props 源 JSON 对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        props_deserialize(props);
    }

    /// @brief 指针交互：Press 定位光标/选区锚点并请求焦点，Move 拖拽扩选（含头含尾），
    ///        Release 结束拖拽；命中测试与实绘同源（掩码态按逐字符对应的掩码串定位）。
    ///        禁用态不响应任何指针交互；基类的 activate/长按计时行为保留。
    /// @param e 鼠标事件（本地坐标；消费时置 `is_handled`）。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!enabled()) {
            return;  // 禁用态不响应任何指针交互
        }
        // 保留基类行为：activate / 长按计时（命中即消费）。
        Widget::on_pointer_event(e);

        // 命中测试与实绘同源：掩码态下按掩码串定位（逐字符一一对应，码点下标一致）。
        const std::string v = display_value();
        const Font f{.size_pt = effective_font_size()};
        const render::TextLayoutOpts opts = layout_opts();
        // 文本空间横坐标：本地 x 先扣掉「自然原点 + 视口」偏移，命中与实绘同源。
        const float text_w = render::FontEngine::measure_width(v, f, opts);
        const float lx = e.local_position.x - text_origin_offset(text_w, text_area_width(size().width));
        if (e.action == MouseAction::Press) {
            // 含头含尾：选区端点用 hit_test_char_inclusive（点击字符任意位置均计入该字符），
            // caret_ 仍用 hit_test_char（caret 模型）作为编辑光标位置。
            const size_t ch = render::FontEngine::hit_test_char_inclusive(v, lx, f, opts);
            caret_ = render::FontEngine::hit_test_char(v, lx, f, opts);
            sel_start_ = ch;  // 锚点（含入字符）
            sel_end_ = AURORA_NO_SEL;  // 尚未形成选区，待拖拽
            selecting_ = true;
            request_focus();
            ensure_caret_visible();
            mark_needs_paint();
            e.is_handled = true;
        } else if (e.action == MouseAction::Move && selecting_) {
            const size_t ch = render::FontEngine::hit_test_char_inclusive(v, lx, f, opts);
            caret_ = render::FontEngine::hit_test_char(v, lx, f, opts);
            sel_end_ = ch;  // 拖拽终点（含入字符）：按下与松开所在字符均计入选区
            ensure_caret_visible();
            mark_needs_paint();
            e.is_handled = true;
        } else if (e.action == MouseAction::Release) {
            selecting_ = false;
            e.is_handled = true;
        }
    }

    /// @brief 键盘编辑派发：Ctrl+A/C/X/V 全选/复制/剪切/粘贴（只读降级），Enter 提交，
    ///        ←/→/Home/End 移动与 Shift 扩选（RTL 下方向键按视觉方向反转），Backspace 删除；
    ///        非 Down、未聚焦或禁用态直接放行。
    /// @param e 键盘事件（消费时置 `is_handled`）。
    auto on_key_event(KeyEvent &e) -> void override {  // NOLINT
        if (!enabled() || !is_focused() || e.action != KeyAction::Down) {
            return;
        }
        const std::string &v = value_ref();
        const Font f{.size_pt = effective_font_size()};
        const bool shift = (e.modifiers & ModifierKey::Shift) != 0u;  // NOLINT
        const bool ctrl = (e.modifiers & ModifierKey::Control) != 0u;  // NOLINT
        const size_t n = cp_count(v);

        if (ctrl && e.key == static_cast<int>(KeyCode::A)) {
            if (n > 0) {
                sel_start_ = 0;
                sel_end_ = n - 1;  // 含尾：最后一个字符下标
            } else {
                sel_end_ = AURORA_NO_SEL;
            }
            caret_ = n;
            ensure_caret_visible();
            mark_needs_paint();
            e.is_handled = true;
            return;
        }
        if (ctrl && e.key == static_cast<int>(KeyCode::C)) {
            const std::string t = selected_text();
            // 复制失败是「用户按了复制却没复制上」，控件层无法补救，但必须留痕：Result 只到调用方，
            // 而这条路径的调用方就是控件自己。
            if (const auto copied = Clipboard::set_text(t.empty() ? v : t); !copied) {
                Diagnostics::warn("TextInput Ctrl+C copy failed: " + copied.error().message, "TextInput::on_key_event",
                                  copied.error().code);
            }
            e.is_handled = true;
            return;
        }
        // Ctrl+X: 剪切（只读态降级为仅复制）
        if (ctrl && e.key == static_cast<int>(KeyCode::X)) {
            const std::string t = selected_text();
            if (!t.empty()) {
                const auto copied = Clipboard::set_text(t);
                if (!copied) {
                    Diagnostics::warn("TextInput Ctrl+X copy failed: " + copied.error().message,
                                      "TextInput::on_key_event", copied.error().code);
                }
                // 复制没成就不删：剪切删了便无处可粘，宁可让选区留在原地等用户重试。
                if (!read_only() && copied) {
                    delete_selection();
                    notify_changed();
                    mark_needs_paint();
                }
            }
            e.is_handled = true;
            return;
        }
        // Ctrl+V: 粘贴（只读态忽略；超出 max_length 部分截断）
        if (ctrl && e.key == static_cast<int>(KeyCode::V)) {
            if (read_only()) {
                e.is_handled = true;
                return;
            }
            // 「剪贴板里没有文本」与「剪贴板读不出来」从此可分：前者 Ok("") 静默不插入，后者留痕。
            std::string clip;
            if (const auto fetched = Clipboard::get_text(); fetched) {
                clip = fetched.value();
            } else {
                Diagnostics::warn("TextInput Ctrl+V read failed: " + fetched.error().message, "TextInput::on_key_event",
                                  fetched.error().code);
            }
            if (!clip.empty()) {
                if (sel_end_ != AURORA_NO_SEL) {
                    delete_selection();
                }
                if (max_length() > 0) {
                    const size_t room = max_length() > cp_count(value_ref()) ? max_length() - cp_count(value_ref()) : 0;
                    clip = cp_slice(clip, 0, room);  // 限长：仅插入剩余额度内的码点
                }

                std::string pv = value_ref();
                size_t pi = 0;
                size_t pcp = 0;
                while (pi < pv.size() && pcp < caret_) {
                    pi += cp_len(static_cast<unsigned char>(pv[pi]));
                    ++pcp;
                }
                pv.insert(pi, clip);
                TextInputProps::set_value(pv);
                caret_ += cp_count(clip);
                sel_start_ = caret_;
                sel_end_ = AURORA_NO_SEL;
                ensure_caret_visible();
                notify_changed();
                mark_needs_paint();
            }
            e.is_handled = true;
            return;
        }
        // Enter: 提交回调（对标 Qt returnPressed / Flutter onSubmitted）
        if (e.key == static_cast<int>(KeyCode::Enter)) {
            if (on_submit_) {
                on_submit_(value_ref());
            }
            e.is_handled = true;
            return;
        }

        int dir = 0;
        if (e.key == static_cast<int>(KeyCode::ArrowLeft)) {
            dir = -1;
        } else if (e.key == static_cast<int>(KeyCode::ArrowRight)) {
            dir = 1;
        }
        // RTL 下方向键按视觉方向语义反转（ArrowLeft = 逻辑前进）。
        if (dir != 0 && cached_direction_ == TextDirection::RTL) {
            dir = -dir;
        }
        if (dir != 0) {
            if (shift) {
                // 含头含尾：锚点取「本次新纳入的那个字符」——右扩是 caret 处的字符，左扩是 caret
                // 前一个（caret 是插入点而非字符下标，直接拿它当左扩锚点会多选一个字符）。
                if (sel_end_ == AURORA_NO_SEL) {
                    sel_start_ = (dir > 0) ? caret_ : (caret_ == 0 ? 0 : caret_ - 1);
                    sel_end_ = sel_start_;  // 先建立 1-char 选区锚点
                }
                const auto nc = static_cast<long long>(caret_) + dir;
                caret_ = static_cast<size_t>(std::clamp(nc, 0LL, static_cast<long long>(n)));
                if (dir > 0) {
                    sel_end_ = (caret_ == 0) ? AURORA_NO_SEL : caret_ - 1;  // 含尾最后字符 = caret-1
                } else {
                    sel_end_ = caret_;  // 含尾 = 新 caret 处的字符
                }
            } else {
                const auto nc = static_cast<long long>(caret_) + dir;
                caret_ = static_cast<size_t>(std::clamp(nc, 0LL, static_cast<long long>(n)));
                sel_start_ = caret_;
                sel_end_ = AURORA_NO_SEL;  // 无选区
            }
            ensure_caret_visible();
            mark_needs_paint();
            e.is_handled = true;
            return;
        }

        if (e.key == static_cast<int>(KeyCode::Home) || e.key == static_cast<int>(KeyCode::End)) {
            // 单行框无「行首/行尾」之别 → Home/End 即文本两端；与方向键共用含头含尾选区模型。
            const bool to_head = e.key == static_cast<int>(KeyCode::Home);
            const size_t prev = caret_;
            const size_t target = to_head ? 0U : n;
            if (!shift) {
                caret_ = target;
                sel_start_ = caret_;
                sel_end_ = AURORA_NO_SEL;
            } else {
                const bool had_sel = has_selection();
                if (!had_sel) {
                    // 首次扩选的锚点 = 本次新纳入的第一个字符：Home 向左跨过 caret-1，End 向右跨过 caret
                    sel_start_ = to_head ? (prev == 0U ? 0U : prev - 1U) : prev;
                }
                caret_ = target;
                // 空文本、或光标本已在端点且原无选区 → 没有字符被纳入，留空选区而非 1 字符假选区
                const bool empty_extend = (n == 0U) || (!had_sel && prev == target);
                sel_end_ = empty_extend ? AURORA_NO_SEL : (to_head ? 0U : n - 1U);
            }
            ensure_caret_visible();
            mark_needs_paint();
            e.is_handled = true;
            return;
        }

        if (e.key == static_cast<int>(KeyCode::Backspace)) {
            if (read_only()) {
                e.is_handled = true;
                return;
            }
            if (sel_end_ != AURORA_NO_SEL) {
                delete_selection();
            } else {
                delete_before_caret();
            }
            notify_changed();
            mark_needs_paint();
            e.is_handled = true;
        }
    }

    /// @brief 焦点变更：失焦即取消未上屏的组合（平台 IME 惯例，避免 preedit 残留在失焦控件里）。
    /// @param focused true = 获焦；false = 失焦（触发组合取消）。
    auto on_focus_change(bool focused) -> void override {
        Widget::on_focus_change(focused);
        if (!focused) {
            cancel_composition();
        }
    }

    /// @brief 关闭基类统一焦点环：本控件已自带 Fluent 式聚焦边框（`paint_frame` 的主题色加粗
    ///        边框），再叠一环会出现双环。见 specification/05-event-navigation.md §4.4。
    /// @return 恒为 false。
    [[nodiscard]] auto wants_focus_ring() const -> bool override { return false; }

    /// @brief 字符输入：启用且聚焦时走统一插入路径 `insert_at_caret`（选区替换 + 限长）。
    /// @param e 输入事件（禁用/未聚焦放行；只读态吞掉不落字；落字即消费）。
    auto on_text_input(TextInputEvent &e) -> void override {
        if (!enabled() || !is_focused()) {
            return;
        }
        if (read_only()) {
            e.is_handled = true;  // 只读态吞掉输入不落字
            return;
        }
        insert_at_caret(e.text);  // 与 IME 上屏共用插入路径：选区替换 + 限长截断。
        e.is_handled = true;
    }

    /// @brief IME 组合输入（CJK 攻坚）：先落 `committed` 上屏，再更新 preedit 显示态。
    ///
    /// 组合期间 `value()` **不含** preedit（数据模型保持干净、golden 可复现），preedit 仅在
    /// 绘制/测量期插入到光标处（见 `composed_text()`）。`preedit` 为空即组合结束或取消。
    ///
    /// 典型序列（拼音输入法）：`preedit="nihao"` → `preedit="你好",cursor=2` →
    /// `preedit="",committed="你好"`（落字）。
    /// @param e 组合事件：`committed` 上屏文本、`preedit` 未上屏串及其光标/选区下标。
    auto on_text_composition(TextCompositionEvent &e) -> void override {
        // 与 `on_text_input` 完全同构：禁用/未聚焦 → 不消费；只读 → 消费但不落地。
        if (!enabled() || !is_focused()) {
            return;
        }
        e.is_handled = true;
        if (read_only()) {
            return;  // 只读：不落字也不显示 preedit
        }
        if (!e.committed.empty()) {
            insert_at_caret(e.committed);
        }
        preedit_ = e.preedit;
        const size_t pn = cp_count(preedit_);
        preedit_cursor_ = std::min(e.cursor_index, pn);
        preedit_sel_start_ = std::min(e.sel_start, pn);
        preedit_sel_end_ = e.has_preedit_selection() ? std::min(e.sel_end, pn) : AURORA_NO_SEL;
        if (preedit_sel_end_ != AURORA_NO_SEL && preedit_sel_end_ < preedit_sel_start_) {
            preedit_sel_end_ = preedit_sel_start_;  // 端点倒置时退化为单点选区
        }
        ensure_caret_visible();
        mark_needs_paint();
        notify_accessibility_event(AccessibilityEvent{.kind = AccessibilityEventKind::ValueChanged, .target = this});
    }

    /// @brief IME 候选窗定位盒：preedit 光标处的零宽竖盒（绝对窗口逻辑 dp）。
    ///
    /// 与 `on_paint` 用同一套算式（字体解析、RTL 右对齐锚、垂直居中、`composed_index` 映射），
    /// 使候选窗落在带下划线的组合串末端而非控件角落。未经绘制遍历（`focus_bounds` 为零盒）时
    /// 原样返回零盒，平台侧退化为系统默认位置。
    /// @return 候选窗锚点盒：preedit 光标处零宽、高度为文本行高的绝对窗口 DIP 矩形。
    [[nodiscard]] auto composition_caret_bounds() const -> Rect override {
        const Rect box = focus_bounds();
        if (box.size.width <= 0.0F || box.size.height <= 0.0F) {
            return box;
        }
        const float fs = effective_font_size();
        const Font f{.size_pt = fs};
        const bool empty = value_ref().empty() && !is_composing();
        const std::string shown = composed_text(empty ? placeholder() : display_value());
        const float th = render::FontEngine::measure_height(f);
        const render::TextLayoutOpts opts = layout_opts();
        const float text_w = render::FontEngine::measure_width(shown, f, opts);
        const float area_w = text_area_width(box.size.width);
        const float tx = box.origin.x + text_origin_offset(text_w, area_w);
        const float ty =
            box.origin.y + padding().top + ((box.size.height - padding().top - padding().bottom - th) * 0.5F);
        const size_t ci = composed_index(caret_) + preedit_cursor_;
        const float cx = tx + render::FontEngine::caret_x(shown, ci, f, opts);
        return Rect{.origin = Point{.x = cx, .y = ty}, .size = Size{.width = 0.0F, .height = th}};
    }

    /// @brief 当前预编辑串（组合中的未上屏文本）；无组合时为空串。
    /// @return preedit 的 UTF-8 副本（不进 `value` 行，仅绘制/测量期可见）。
    [[nodiscard]] auto preedit() const -> std::string { return preedit_; }

    /// @brief 是否处于组合态（preedit 非空）。
    /// @return true = 组合中（有未上屏文本）。
    [[nodiscard]] auto is_composing() const -> bool { return !preedit_.empty(); }

    /// @brief 组合光标在 preedit 内的码点下标（候选插入点）。
    /// @return 下标（更新时已夹紧到 preedit 码点数）。
    [[nodiscard]] auto composition_cursor() const -> std::size_t { return preedit_cursor_; }

    /// @brief 是否有活动选区（无选区时 sel_end_ == AURORA_NO_SEL）。
    /// @return true = 存在含头含尾选区；false = 仅光标（无字符被选中）。
    [[nodiscard]] auto has_selection() const -> bool { return sel_end_ != AURORA_NO_SEL; }

    /// @brief 当前选中的文本（含头含尾：返回 [min, max] 区间内的全部字符）。无选区返回空。
    /// @return 选区内字符拼接的 UTF-8 串；无选区为空串。
    [[nodiscard]] auto selected_text() const -> std::string {
        if (!has_selection()) {
            return {};
        }
        const size_t a = std::min(sel_start_, sel_end_);
        const size_t b = std::max(sel_start_, sel_end_);
        return cp_slice(value_ref(), a, b - a + 1);  // 含尾计数
    }

  protected:
    // ---- 内部读件：属性面上的行读件在控件侧的取用形态 ----

    /// @brief 文本来源的现值引用（可写来源行的读视图，避开逐处拷贝）。
    ///
    /// 引用由来源节点持有、与 `value` 行同源，故在控件存活期间有效；编辑路径写回的正是这一份存储，
    /// 外部那只 `State<std::string>` 读到的也是它（双向绑定的物理依据）。
    /// @return 当前文本的常引用（不含组合期 preedit）。
    [[nodiscard]] auto value_ref() const -> const std::string & { return read(value_row()).get(); }

    /// @brief 有效字号（pt）：非正值降级为该行回落基线，供绘制/度量/命中共用。
    /// @return 严格为正的字号（pt）。
    [[nodiscard]] auto effective_font_size() const -> float {
        const float fs = font_size();
        return fs > 0.0F ? fs : TextInputProps::font_size_default();
    }

    // ---- 继承扩展点 ----

    /// @brief 生效布局选项：direction 取缓存方向（布局期解析），nullopt 时保持默认
    ///        （FontEngine 按内容 guess）——默认行为与接入前逐位一致。
    /// @return 布局选项聚合：`direction` 置为 `cached_direction_`（nullopt = 按内容猜测），其余字段保持默认。
    [[nodiscard]] auto layout_opts() const -> render::TextLayoutOpts {
        render::TextLayoutOpts opts;
        opts.direction = cached_direction_;
        return opts;
    }

    /// @brief 文本区可视宽度：盒宽扣除左右内边距，负值归零。
    /// @param box_w 控件盒宽（dp）。
    /// @return 可用于排版文本的宽度（≥ 0）。
    [[nodiscard]] auto text_area_width(float box_w) const -> float {
        return std::max(0.0F, box_w - padding().left - padding().right);
    }

    /// @brief 文本自然原点的盒内偏移（左内边距 + RTL 右对齐 − 水平视口）。
    ///
    /// 绘制、命中测试、IME 候选窗与无障碍字符盒共用此式，保证四者读同一个视量。
    /// @param text_w 当前显示串的全量排版宽度（dp）。
    /// @param area_w 文本区可视宽度（`text_area_width` 的产物）。
    /// @return 相对控件盒左沿的偏移；RTL 未超宽时额外右对齐，超宽时由视口夹取。
    [[nodiscard]] auto text_origin_offset(float text_w, float area_w) const -> float {
        const float align = (cached_direction_ == TextDirection::RTL) ? std::max(0.0F, area_w - text_w) : 0.0F;
        return padding().left + align - std::clamp(view_x_, 0.0F, std::max(0.0F, text_w - area_w));
    }

    /// @brief 把水平视口推到「光标可见」：光标左缘越出左界则回卷到光标处，
    ///        右缘越出右界则推到光标贴右内边距；视口不越过文本两端。
    ///
    /// 所有改光标的路径（键入 / 粘贴 / 删除 / 方向键 / Home / End / 点选 / IME / 无障碍选区）
    /// 都收口到此，绘制侧只读 `view_x_` 的生效值，不再各自推算。
    /// @note Side-effects: 可能写 `view_x_` 并标脏
    auto ensure_caret_visible() -> void { ensure_caret_visible_for(size().width); }

    /// @brief 按指定盒宽执行「确保光标可见」（`on_layout` 在 `size_` 落定前调用）。
    /// @param box_w 即将生效的控件盒宽（dp）。
    /// @note Side-effects: 可能写 `view_x_` 并标脏
    auto ensure_caret_visible_for(float box_w) -> void {
        const Font f{.size_pt = effective_font_size()};
        const std::string shown = composed_text(display_value());
        const float text_w = render::FontEngine::measure_width(shown, f, layout_opts());
        const float area_w = text_area_width(box_w);
        if (area_w <= 0.0F) {
            return;  // 未布局/零宽盒：无视口可言，绘制侧按当前宽度夹取
        }
        const size_t ci = composed_index(caret_) + preedit_cursor_;
        const float caret_rel = render::FontEngine::caret_x(shown, ci, f, layout_opts());
        const float max_view = std::max(0.0F, text_w - area_w);
        float v = view_x_;
        if (caret_rel - v > area_w) {
            v = caret_rel - area_w;  // 光标越右界：把它贴到右内边距
        }
        if (caret_rel - v < 0.0F) {
            v = caret_rel;  // 光标越左界：回卷到光标处
        }
        v = std::clamp(v, 0.0F, max_view);
        if (v != view_x_) {
            view_x_ = v;
            mark_needs_paint();
        }
    }

    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        // 缓存生效方向：事件路径（on_pointer_event/on_key_event）拿不到 BuildContext，
        // 与 Text 缓存 resolved_text 同模式——布局期解析一次，事件/绘制共用。
        cached_direction_ =
            present(direction_row()) ? std::optional<TextDirection>{direction()} : explicit_text_direction(ctx);
        const float fs = effective_font_size();
        const Font f{.size_pt = fs};
        const std::string shown = composed_text(value_ref().empty() ? placeholder() : display_value());
        const float tw = render::FontEngine::measure_width(shown, f) + padding().left + padding().right;
        const float h = render::FontEngine::measure_height(f) + padding().top + padding().bottom;
        const float w = (c.max.width != Size::infinity().width) ? c.max.width : tw;
        const Size sz = c.constrain(Size{.width = w, .height = h});
        // 盒宽变化（含收窄）后光标可能落到视口外：布局期即回卷，绘制不会出现「光标失踪」。
        ensure_caret_visible_for(sz.width);
        return sz;
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        const float fs = effective_font_size();
        const Font f{.size_pt = fs};
        const bool disabled = !enabled();

        paint_frame(p, bounds, ctx);
        // 内容（选区 / 组合串 / 正文 / 光标）一律裁进自身盒：窄盒里的长文本不得画到邻居身上。
        // 圆角盒走圆角裁剪，与 `paint_frame` 的圆角外观一致。
        const bool rounded_clip = corner_radius() > 0.0F;
        if (rounded_clip) {
            p.push_clip_rounded(bounds, corner_radius());
        } else {
            p.push_clip(bounds);
        }

        const bool empty = value_ref().empty() && !is_composing();
        const std::string shown = composed_text(empty ? placeholder() : display_value());
        const Color text_col = [&] {  // NOLINT
            if (disabled) {
                return Color{150, 150, 152, 255};
            }
            if (empty) {
                return placeholder_color(ctx);
            }
            return text_color(ctx);
        }();
        // 文本原点 = 自然原点（左内边距；RTL 未超宽时右对齐到右内边距）+ 生效水平视口。
        // 视口只由 `ensure_caret_visible()` 推进；绘制侧仅按当前文本宽度夹取，不写状态，
        // 故显示列表录制与回放读到同一偏移。
        const float text_w = render::FontEngine::measure_width(shown, f, layout_opts());
        const float area_w = text_area_width(bounds.size.width);
        const float tx = bounds.origin.x + text_origin_offset(text_w, area_w);
        const float th = render::FontEngine::measure_height(f);
        const float ty =
            bounds.origin.y + padding().top + ((bounds.size.height - padding().top - padding().bottom - th) * 0.5F);

        // 选区高亮（含头含尾模型）：无选区不画；端点字符（含行尾/行首）始终计入。
        // 组合期间 `shown` 含 preedit，故下标须经 `composed_index` 映射，否则高亮整体错位。
        if (!empty && sel_end_ != AURORA_NO_SEL) {
            const size_t a = composed_index(std::min(sel_start_, sel_end_));
            const size_t b = composed_index(std::max(sel_start_, sel_end_));
            const render::TextLayoutOpts opts = layout_opts();
            const float x0 = tx + render::FontEngine::caret_x(shown, a, f, opts);
            const float x1 = tx + render::FontEngine::caret_x(shown, b + 1, f, opts);  // 含尾：+1
            p.fill_rect(Rect{.origin = Point{.x = x0, .y = ty}, .size = Size{.width = x1 - x0, .height = th}},
                        selection_color(ctx));
        }

        // 组合态：preedit 内选区高亮 + 整段下划线
        if (is_composing()) {
            paint_composition(p, shown, tx, ty, th, f, layout_opts());
        }

        p.draw_text(Rect{.origin = Point{.x = tx, .y = ty},
                         .size = Size{.width = bounds.size.width - padding().left - padding().right, .height = th}},
                    shown, f, text_col);

        // 光标（仅非占位文本、聚焦且未禁用时显示）；组合时光标落在 preedit 内的候选插入点并加粗。
        if (is_focused() && !empty && !disabled) {
            const size_t ci = composed_index(caret_) + preedit_cursor_;
            const float cx = tx + render::FontEngine::caret_x(shown, ci, f, layout_opts());
            p.fill_rect(Rect{.origin = Point{.x = cx, .y = ty},
                             .size = Size{.width = is_composing() ? 2.0F : 1.5F, .height = th}},
                        cursor_color(ctx));
        }
        p.pop_clip();
    }

    /// @brief 绘制背景 + 边框（聚焦/禁用态切换；聚焦边框缺省跟随主题 primary）。子类可覆盖。
    /// @param p 绘制器：发出背景填充与边框描边。
    /// @param bounds 控件盒（窗口逻辑 dp）：填充与描边均画在此盒上。
    /// @param ctx 构建上下文：聚焦且未显式设置边框色时，从中取 `Theme::primary` 作缺省色。
    virtual auto paint_frame(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
        const bool disabled = !enabled();
        const Color bg = [&] {  // NOLINT
            if (disabled) {
                return Color{225, 225, 225, 255};
            }
            if (is_focused()) {
                return focused_background(ctx);
            }
            return background(ctx);
        }();
        Color border = disabled ? Color{180, 180, 180, 255} : border_color(ctx);
        float bw = border_width(ctx);
        if (!disabled && is_focused()) {
            border = focused_border_color(ctx);
            bw = std::max(bw, 1.5F);  // 聚焦态边框加粗（Fluent 式聚焦提示）
        }
        if (corner_radius() > 0.0F) {
            p.fill_rounded_rect(bounds, corner_radius(), bg);
            if (bw > 0.0F) {
                p.draw_rounded_border(bounds, corner_radius(), bw, border);
            }
        } else {
            p.fill_rect(bounds, bg);
            if (bw > 0.0F) {
                p.draw_rect(bounds, border);
            }
        }
    }

    /// @brief 实显文本：掩码态返回等长 • 串（逐码点一一对应），否则返回原值。
    /// @return 掩码态为逐码点一个 •（每字符 3 字节）的显示串；非掩码态为文本现值的副本。
    [[nodiscard]] auto display_value() const -> std::string {
        if (!obscure_text()) {
            return value_ref();
        }
        std::string out;
        const size_t n = cp_count(value_ref());
        out.reserve(n * 3);
        for (size_t i = 0; i < n; ++i) {
            out += "\u2022";
        }
        return out;
    }

    /// @brief 在光标处插入文本（选区替换 + `max_length` 限长 + 光标前进）。
    ///
    /// 普通文本输入与 IME 上屏（`TextCompositionEvent::committed`）共用此路径，保证限长/选区
    /// 语义一致。`text` 为空时为空操作。
    /// @param text 待插入文本（UTF-8）：先替换现有选区；超出 `max_length` 行剩余额度时按码点截断，
    ///        截断后为空则不落字。
    auto insert_at_caret(const std::string &text) -> void {
        if (text.empty()) {
            return;
        }
        if (sel_end_ != AURORA_NO_SEL) {
            delete_selection();  // 选区替换
        }
        std::string ins = text;
        if (max_length() > 0) {
            const size_t room = max_length() > cp_count(value_ref()) ? max_length() - cp_count(value_ref()) : 0;
            ins = cp_slice(ins, 0, room);  // 限长：仅插入剩余额度内的码点
            if (ins.empty()) {
                return;
            }
        }
        std::string v = value_ref();
        size_t i = 0;
        size_t cp = 0;
        while (i < v.size() && cp < caret_) {
            i += cp_len(static_cast<unsigned char>(v[i]));
            ++cp;
        }
        v.insert(i, ins);
        TextInputProps::set_value(v);
        caret_ += cp_count(ins);
        sel_start_ = caret_;
        sel_end_ = AURORA_NO_SEL;
        ensure_caret_visible();
        notify_changed();
        mark_needs_paint();
    }

    /// @brief 组合显示串：把 preedit 插到 `base` 的光标处（`base` 可为掩码后的显示值）。
    ///
    /// 绘制与测量一律走此串，故 preedit 与既有文本共用一套 `caret_x` 度量，天然支持中文混排。
    /// @param base 底衬显示文本（可为文本现值、掩码后的显示值或占位串）。
    /// @return 把 preedit 插入 `base` 第 `caret_` 个码点处得到的串（`caret_` 越界夹紧到串尾）；
    ///         `preedit_` 为空时原样返回 `base`。
    [[nodiscard]] auto composed_text(const std::string &base) const -> std::string {
        if (preedit_.empty()) {
            return base;
        }
        const size_t n = cp_count(base);
        const size_t at = std::min(caret_, n);
        return cp_slice(base, 0, at) + preedit_ + cp_slice(base, at, n - at);
    }

    /// @brief `base` 的码点下标 `i` 在 `composed_text(base)` 中的对应下标（preedit 插在 caret_ 处）。
    /// @param i `base` 内的码点下标。
    /// @return 组合显示串中的对应下标：组合中且 `i >= caret_` 时后移 preedit 的码点数，否则原样返回。
    [[nodiscard]] auto composed_index(size_t i) const -> size_t {
        return preedit_.empty() ? i : i + (i >= caret_ ? cp_count(preedit_) : 0U);
    }

    /// @brief 组合态绘制：preedit 选区高亮 + 下划线（候选串底衬）。仅 `is_composing()` 时调用。
    ///
    /// 高亮与下划线均以 `caret_x(shown, caret_)` 定 preedit 起点，与主文本共用度量口径。
    /// @param p 绘制器：填充 preedit 内选区高亮与整段下划线。
    /// @param shown 含 preedit 的显示串（`composed_text` 的产物），用于光标定位度量。
    /// @param x0 文本原点 x（绝对坐标，已含内边距与 RTL 右对齐偏移）。
    /// @param ty 文本原点 y，高亮矩形以此为顶。
    /// @param th 文本行高：高亮高度与下划线纵位（`ty + th - 1`）的计算基准。
    /// @param f 当前字体。
    /// @param opts 布局选项（含生效方向）。
    /// @return preedit 的宽度（即所画下划线段的宽度）。
    auto paint_composition(Painter &p, const std::string &shown, float x0, float ty, float th, const Font &f,
                           const render::TextLayoutOpts &opts) const -> float {
        const float px0 = x0 + render::FontEngine::caret_x(shown, caret_, f, opts);
        const float pw = render::FontEngine::measure_width(preedit_, f);

        // ① preedit 内选区（输入法高亮「待转换片段」）
        if (preedit_sel_end_ != AURORA_NO_SEL) {
            const size_t a = std::min(preedit_sel_start_, preedit_sel_end_);
            const size_t b = std::max(preedit_sel_start_, preedit_sel_end_);
            const float sx0 = px0 + render::FontEngine::caret_x(preedit_, a, f, opts);
            const float sx1 = px0 + render::FontEngine::caret_x(preedit_, b + 1, f, opts);  // 含尾
            p.fill_rect(Rect{.origin = Point{.x = sx0, .y = ty}, .size = Size{.width = sx1 - sx0, .height = th}},
                        preedit_selection_color_);
        }
        // ② 组合下划线（整段 preedit，区别于正式文本）
        p.fill_rect(Rect{.origin = Point{.x = px0, .y = ty + th - 1.0F}, .size = Size{.width = pw, .height = 2.0F}},
                    preedit_underline_color_);
        return pw;
    }

    /// @brief 取消组合：清空 preedit 与其选区（失焦 / 只读 / 平台侧取消时调用）。
    auto cancel_composition() -> void {
        if (preedit_.empty()) {
            return;
        }
        preedit_.clear();
        preedit_cursor_ = 0;
        preedit_sel_start_ = 0;
        preedit_sel_end_ = AURORA_NO_SEL;
        ensure_caret_visible();
        mark_needs_paint();
    }

    /// @brief 编辑后统一上报（on_changed 回调 + 无障碍 ValueChanged 事件）。
    ///
    /// 所有编辑路径（插入 / 粘贴 / 删除 / 退格）都收口于此，故无障碍上报放在这里而非逐处补。
    auto notify_changed() const -> void {
        if (on_changed_) {
            on_changed_(value_ref());
        }
        notify_accessibility_event(AccessibilityEvent{.kind = AccessibilityEventKind::ValueChanged, .target = this});
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    // 19 行属性已全数收进 `TextInputProps` 权威表；此处只留表外字段：组合串配色（从不参与序列化，
    // 未成行）与控件自己的运行态（方向缓存、两枚回调、选区/光标/视口、IME 组合态）。
    Color preedit_underline_color_ = Color{30, 110, 220, 255};  ///< 组合串（preedit）下划线色
    Color preedit_selection_color_ = Color{255, 200, 80, 120};  ///< preedit 内选区（待转换片段）高亮色
    /// @brief 生效方向缓存：on_layout 解析（显式属性 > Environment > 进程级 > 无来源），
    ///        供无 BuildContext 的事件路径与绘制路径共用。
    std::optional<TextDirection> cached_direction_;
    std::function<void(const std::string &)> on_changed_;  ///< 每次编辑后触发
    std::function<void(const std::string &)> on_submit_;  ///< Enter 提交触发

    // 文字选区状态（含头含尾模型，与 Text 一致：sel_start_/sel_end_ 为「被选中字符的码点下标」；
    // 无选区时 sel_end_ == AURORA_NO_SEL。caret_ 为编辑光标位置（caret 下标，介于字符之间）。UTF-8 安全）
    size_t sel_start_ = 0;
    size_t sel_end_ = AURORA_NO_SEL;
    size_t caret_ = 0;
    /// @brief 水平视口：文本空间左缘已滚出的距离（dp，0 = 首字符贴左内边距）。
    ///        只由 `ensure_caret_visible()` 推进；绘制/命中读生效值（按当前文本宽度与盒宽夹取）。
    float view_x_ = 0.0F;
    bool selecting_ = false;
    static constexpr size_t AURORA_NO_SEL = static_cast<size_t>(-1);

    // IME 组合态：preedit 不进文本来源（数据模型保持纯净），仅在绘制/测量期插入 caret_ 处。
    std::string preedit_;  ///< 预编辑串（UTF-8）；空 = 无组合
    size_t preedit_cursor_ = 0;  ///< 组合光标在 preedit 内的码点下标
    size_t preedit_sel_start_ = 0;  ///< preedit 内选区起点（码点下标）
    size_t preedit_sel_end_ = AURORA_NO_SEL;  ///< preedit 内选区终点（含尾）；AURORA_NO_SEL = 无
    // NOLINTEND(*-non-private-member-variables-in-classes)

    // UTF-8 码点原语已收口到 aurora::utf8_cp_*（见 core/utf8.h，dup-1）；此处委托，避免重复实现。
    static auto cp_len(unsigned char c) -> size_t { return static_cast<size_t>(utf8_cp_len(c)); }
    static auto cp_count(const std::string &s) -> size_t { return utf8_cp_count(s); }
    static auto cp_slice(const std::string &s, size_t start, size_t count) -> std::string {
        return utf8_cp_slice(s, start, count);
    }
    /// @brief 码点下标 → UTF-8 字节偏移（越界夹紧到串尾；无障碍文本钩子的换算原语）。
    /// @param s UTF-8 串。
    /// @param cp_index 目标码点下标（从 0 计；超出码点总数时停在串尾）。
    /// @return 该码点首字节的字节偏移；`cp_index` 为 0 或 `s` 为空时为 0，越界时为 `s.size()`。
    [[nodiscard]] static auto cp_to_byte(const std::string &s, size_t cp_index) -> size_t {
        size_t i = 0;
        size_t cp = 0;
        while (i < s.size() && cp < cp_index) {
            i += cp_len(static_cast<unsigned char>(s[i]));
            ++cp;
        }
        return i;
    }
    /// @brief UTF-8 字节偏移 → 码点下标（越界夹紧到串尾）。
    /// @param s UTF-8 串。
    /// @param byte_index 目标字节偏移；落在某码点内部时归属到其后首个码点边界，越界时按串尾计。
    /// @return 对应的码点下标（串尾偏移返回码点总数）。
    [[nodiscard]] static auto byte_to_cp(const std::string &s, size_t byte_index) -> size_t {
        size_t i = 0;
        size_t cp = 0;
        while (i < s.size() && i < byte_index) {
            i += cp_len(static_cast<unsigned char>(s[i]));
            ++cp;
        }
        return cp;
    }
    auto delete_selection() -> void {
        if (!has_selection()) {
            return;
        }
        const size_t a = std::min(sel_start_, sel_end_);
        const size_t b = std::max(sel_start_, sel_end_);
        std::string v = value_ref();
        size_t i = 0;
        size_t cp = 0;
        while (i < v.size() && cp < a) {
            i += cp_len(static_cast<unsigned char>(v[i]));
            ++cp;
        }
        const size_t begin = i;
        while (i < v.size() && cp <= b) {  // 含尾：删除 [a, b]
            i += cp_len(static_cast<unsigned char>(v[i]));
            ++cp;
        }
        v.erase(begin, i - begin);
        TextInputProps::set_value(v);
        caret_ = a;
        sel_start_ = a;
        sel_end_ = AURORA_NO_SEL;  // 删除后无选区
        ensure_caret_visible();
    }
    auto delete_before_caret() -> void {
        if (caret_ == 0) {
            return;
        }
        std::string v = value_ref();
        size_t i = 0;
        size_t cp = 0;
        while (i < v.size() && cp < caret_) {
            i += cp_len(static_cast<unsigned char>(v[i]));
            ++cp;
        }
        const size_t end = i;
        size_t j = end;
        while (j > 0 && (static_cast<unsigned char>(v[j - 1]) & 0xC0U) == 0x80U) {
            --j;
        }
        const size_t start = (j > 0) ? j - 1U : 0U;
        v.erase(start, end - start);
        TextInputProps::set_value(v);
        --caret_;
        sel_start_ = caret_;
        sel_end_ = AURORA_NO_SEL;  // 退格后无选区
        ensure_caret_visible();
    }
};

}  // namespace aurora
