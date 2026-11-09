#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/diagnostics.h"
#include "aurora/core/enums.h"
#include "aurora/core/font.h"
#include "aurora/environment/build_context.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/i18n/message_catalog.h"
#include "aurora/state/read_only_state.h"
#include "aurora/theming/color_roles.h"  // 活令牌回落点：roles.on_surface
#include "aurora/widget/props.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief Text 的属性权威表：一行一字段，键名、私有字段、脏档位、约束与回落点只在行里声明一次。
///
/// 三态语义与迁移前一致：
/// - **必填行**（`content`）：字段本身就是生效值，不可撤表态；
/// - **可空行**（其余）：未表态时取该行回落点，表态后不再跟随；撤表态走公共面上唯一的 `clear(row)`；
/// - **来源行**（`content` / `color`）：另有一只同名写件收 `ReadOnlyState`，即响应式换源。
///
/// 回落点三档在本表里同时在场：
/// - 活令牌档（`color`）：随作用域换肤而变，读件带 `BuildContext`；
/// - 常量档（`font_size` / `text_align` / `max_lines` / `overflow` / `soft_wrap` / `line_height` /
///   `letter_spacing` / `word_spacing` / `font_weight` / `font_style` / `decoration` /
///   `background_color`）：全库唯一基线，读件无参；
/// - 派生档（`decoration_color`）：回落点读同族 `color` 行的生效值再加工，读件带 `BuildContext`，
///   schema 里的默认值只能以显示串给出，本行写 `.shows_token<&ColorRoles::on_surface>()`（名由登记表反查）。
///
/// `direction` 的「继承」语义留在表外：本行回落基线是 `LTR`（读件无参），但控件判定方向时先问
/// 表态位 `present(direction_row())`——未表态即走 `Environment` / 进程级继承，与迁移前 `nullopt` 等价。
///
/// @note Thread: main-thread only
/// @note Side-effects: 写入经宿主链接位标脏并重建订阅；未挂载时是纯 DTO
class TextProps : public AbstractProps<TextProps> {
  public:
    // ---- 权威表：参数顺序即 schema 顺序 ----

    /// @brief 必填来源件：文本内容。变更同时影响度量与绘制 ⇒ 布局 + 绘制两档都打。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto content_row() {
        return prop_row<&TextProps::content_>("content").layout().paint().required().note("Text content");
    }

    /// @brief 可空值件：字号（pt）。参与度量 ⇒ 布局档；下界 0。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto font_size_row() {
        return prop_row<&TextProps::font_size_>("font_size")
            .layout()
            .paint()
            .bounds("0")
            .fallback(&TextProps::font_size_default)
            .note("Font size (pt)");
    }

    /// @brief 可空来源件：文字色。未表态时取主题 `roles.on_surface`，换肤即随动。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto color_row() {
        return prop_row<&TextProps::color_>("color").paint().fallback<&ColorRoles::on_surface>().note(
            "Text color (follows theme on_surface when unset)");
    }

    /// @brief 可空值件：水平对齐。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto text_align_row() {
        return prop_row<&TextProps::text_align_>("text_align")
            .paint()
            .fallback(&TextProps::text_align_default)
            .note("Horizontal alignment");
    }

    /// @brief 可空值件：书写方向。回落基线只是读件取值，控件是否继承另看不表态位（见类注释）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto direction_row() {
        return prop_row<&TextProps::direction_>("direction")
            .layout()
            .paint()
            .fallback(&TextProps::direction_default)
            .shows("auto")
            .note("Text direction (auto = inherit from environment)");
    }

    /// @brief 可空值件：最大行数（0 = 不限）；超出按 `overflow` 处理。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto max_lines_row() {
        return prop_row<&TextProps::max_lines_>("max_lines")
            .layout()
            .paint()
            .bounds("0")
            .fallback(&TextProps::max_lines_default)
            .note("Max line count (0 = unlimited)");
    }

    /// @brief 可空值件：超出 `max_lines` 时的处理方式。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto overflow_row() {
        return prop_row<&TextProps::overflow_>("overflow")
            .layout()
            .paint()
            .fallback(&TextProps::overflow_default)
            .note("Overflow handling");
    }

    /// @brief 可空值件：是否按宽度自动换行。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto soft_wrap_row() {
        return prop_row<&TextProps::soft_wrap_>("soft_wrap")
            .layout()
            .paint()
            .fallback(&TextProps::soft_wrap_default)
            .note("Auto line wrap");
    }

    /// @brief 可空值件：行高倍数（相对字高）；下界 0。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto line_height_row() {
        return prop_row<&TextProps::line_height_>("line_height")
            .layout()
            .paint()
            .bounds("0")
            .fallback(&TextProps::line_height_default)
            .note("Line height multiplier");
    }

    /// @brief 可空值件：字形间距（FontEngine 暂不支持 → 优雅降级）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto letter_spacing_row() {
        return prop_row<&TextProps::letter_spacing_>("letter_spacing")
            .layout()
            .paint()
            .fallback(&TextProps::letter_spacing_default)
            .note("Letter spacing");
    }

    /// @brief 可空值件：词间距（FontEngine 暂不支持 → 优雅降级）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto word_spacing_row() {
        return prop_row<&TextProps::word_spacing_>("word_spacing")
            .layout()
            .paint()
            .fallback(&TextProps::word_spacing_default)
            .note("Word spacing");
    }

    /// @brief 可空值件：字重档位（其底层值即 CSS 数值口径）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto font_weight_row() {
        return prop_row<&TextProps::font_weight_>("font_weight")
            .layout()
            .paint()
            .fallback(&TextProps::font_weight_default)
            .note("Font weight");
    }

    /// @brief 可空值件：字形风格（Italic 经 FontEngine 施加合成斜体）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto font_style_row() {
        return prop_row<&TextProps::font_style_>("font_style")
            .layout()
            .paint()
            .fallback(&TextProps::font_style_default)
            .note("Font style");
    }

    /// @brief 可空值件：装饰线（可按位组合）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto decoration_row() {
        return prop_row<&TextProps::decoration_>("decoration")
            .paint()
            .fallback(&TextProps::decoration_default)
            .note("Text decoration");
    }

    /// @brief 可空值件：装饰线颜色。派生档由生效文字色调得 ⇒ 默认值只能是显示串。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto decoration_color_row() {
        return prop_row<&TextProps::decoration_color_>("decoration_color")
            .paint()
            .fallback(&TextProps::decoration_color_default)
            .shows_token<&ColorRoles::on_surface>()
            .note("Decoration color (follows text color when unset)");
    }

    /// @brief 可空值件：文本底色（alpha = 0 表示不绘制）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto background_color_row() {
        return prop_row<&TextProps::background_color_>("background_color")
            .paint()
            .fallback(&TextProps::background_color_default)
            .note("Text background color");
    }

    /// @brief 权威表本体：本控件全部属性行的元组。
    /// @return 按声明顺序排列的行元组（发射面与序列化面都遍历它）。
    [[nodiscard]] static constexpr auto rows() {
        return make_prop_rows(content_row(), font_size_row(), color_row(), text_align_row(), direction_row(),
                              max_lines_row(), overflow_row(), soft_wrap_row(), line_height_row(), letter_spacing_row(),
                              word_spacing_row(), font_weight_row(), font_style_row(), decoration_row(),
                              decoration_color_row(), background_color_row());
    }

    // ---- 读面：每行恰一件，形态由该行回落点决定 ----

    /// @brief 当前文本内容（必填来源件：无参读件）。
    /// @return 内容的本地化值（显示串仍须经 `MessageCatalog` 解析）。
    [[nodiscard]] auto content() const -> LocalizedString { return get(content_row()); }

    /// @brief 当前生效字号（pt）：常量档回落点不随作用域变 ⇒ 无参读件。
    /// @return 字号（pt）。
    [[nodiscard]] auto font_size() const -> float { return stated(font_size_row()); }

    /// @brief 当前作用域下生效的文字色。
    /// @param ctx 构建上下文：未表态时据此解主题 `roles.on_surface`。
    /// @return 生效文字色。
    [[nodiscard]] auto color(const BuildContext &ctx) const -> Color { return resolve(color_row(), ctx); }

    /// @brief 当前生效水平对齐。
    /// @return 生效对齐方式。
    [[nodiscard]] auto text_align() const -> TextAlign { return stated(text_align_row()); }

    /// @brief 当前方向表态值；是否继承另由 `present(direction_row())` 判定（见类注释）。
    /// @return 表态的方向；未表态时为回落基线 `LTR`。
    [[nodiscard]] auto direction() const -> TextDirection { return stated(direction_row()); }

    /// @brief 当前生效最大行数。
    /// @return 行数上限；0 = 不限。
    [[nodiscard]] auto max_lines() const -> int { return stated(max_lines_row()); }

    /// @brief 当前生效溢出策略。
    /// @return 超出 `max_lines` 的处理方式。
    [[nodiscard]] auto overflow() const -> TextOverflow { return stated(overflow_row()); }

    /// @brief 当前是否自动软换行。
    /// @return 自动折行位。
    [[nodiscard]] auto soft_wrap() const -> bool { return stated(soft_wrap_row()); }

    /// @brief 当前生效行高倍数。
    /// @return 相对字高的行距倍数。
    [[nodiscard]] auto line_height() const -> float { return stated(line_height_row()); }

    /// @brief 当前生效字形间距。
    /// @return 字符间额外间距 dp。
    [[nodiscard]] auto letter_spacing() const -> float { return stated(letter_spacing_row()); }

    /// @brief 当前生效词间距。
    /// @return 词间额外间距 dp。
    [[nodiscard]] auto word_spacing() const -> float { return stated(word_spacing_row()); }

    /// @brief 当前生效字重档位。
    /// @return 字重枚举（底层值即 CSS 数值口径）。
    [[nodiscard]] auto font_weight() const -> FontWeight { return stated(font_weight_row()); }

    /// @brief 当前生效字形风格。
    /// @return 字形风格枚举。
    [[nodiscard]] auto font_style() const -> FontStyle { return stated(font_style_row()); }

    /// @brief 当前生效装饰线。
    /// @return 装饰线位组合。
    [[nodiscard]] auto decoration() const -> TextDecoration { return stated(decoration_row()); }

    /// @brief 当前作用域下生效的装饰线颜色（未表态时等于生效文字色）。
    /// @param ctx 构建上下文：未表态时据此读同族 `color` 行的生效值。
    /// @return 生效装饰线色。
    [[nodiscard]] auto decoration_color(const BuildContext &ctx) const -> Color {
        return resolve(decoration_color_row(), ctx);
    }

    /// @brief 当前生效文本底色。
    /// @return 底色；alpha = 0 表示不绘制。
    [[nodiscard]] auto background_color() const -> Color { return stated(background_color_row()); }

    // ---- 写面：同名重载区分「装值」与「换源」，档位仍由行决定 ----

    /// @brief 装入文本内容常量值（换入自有常量源，旧源退订）。
    /// @param value 新的内容值。
    /// @return 引用本属性对象，便于链式续写。
    auto set_content(const LocalizedString &value) -> TextProps & { return assign(content_row(), value); }

    /// @brief 装入文本内容（`std::string` 直写通道）。
    /// @param value 新的 UTF-8 文本。
    /// @return 引用本属性对象，便于链式续写。
    auto set_content(const std::string &value) -> TextProps & { return set_content(LocalizedString{value}); }

    /// @brief 装入文本内容（C 字符串直写通道：消除字面量在值件与来源件之间的重载歧义）。
    /// @param value 新的 UTF-8 文本；nullptr 语义由 `LocalizedString` 转换处理。
    /// @return 引用本属性对象，便于链式续写。
    auto set_content(const char *value) -> TextProps & { return set_content(LocalizedString{value}); }

    /// @brief 换绑内容来源（响应式换源：订阅重建，外部 `State` 继续驱动本属性）。
    /// @param source 新的只读句柄来源。
    /// @return 引用本属性对象，便于链式续写。
    auto set_content(ReadOnlyState<LocalizedString> source) -> TextProps & {
        return rebind(content_row(), std::move(source));
    }

    /// @brief 装入字号（pt）。
    /// @param value 新字号。
    /// @return 引用本属性对象，便于链式续写。
    auto set_font_size(float value) -> TextProps & { return assign(font_size_row(), value); }

    /// @brief 装入文字色常量值（表态即脱离主题 `roles.on_surface`）。
    /// @param value 新文字色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_color(Color value) -> TextProps & { return assign(color_row(), value); }

    /// @brief 换绑文字色来源（响应式换源）。
    /// @param source 新的只读句柄来源。
    /// @return 引用本属性对象，便于链式续写。
    auto set_color(ReadOnlyState<Color> source) -> TextProps & { return rebind(color_row(), std::move(source)); }

    /// @brief 装入水平对齐。
    /// @param value 新对齐方式；`Justify` 且多行时对非末行做逐词均分。
    /// @return 引用本属性对象，便于链式续写。
    auto set_text_align(TextAlign value) -> TextProps & { return assign(text_align_row(), value); }

    /// @brief 装入书写方向（表态即覆盖 `Environment` 与进程级来源）。
    /// @param value 新方向。
    /// @return 引用本属性对象，便于链式续写。
    auto set_direction(TextDirection value) -> TextProps & { return assign(direction_row(), value); }

    /// @brief 装入最大行数。
    /// @param value 行数上限；0 = 不限。
    /// @return 引用本属性对象，便于链式续写。
    auto set_max_lines(int value) -> TextProps & { return assign(max_lines_row(), value); }

    /// @brief 装入溢出策略。
    /// @param value 超出 `max_lines` 的处理方式。
    /// @return 引用本属性对象，便于链式续写。
    auto set_overflow(TextOverflow value) -> TextProps & { return assign(overflow_row(), value); }

    /// @brief 装入自动软换行位。
    /// @param value true 按可用宽度折行，false 单行不换行。
    /// @return 引用本属性对象，便于链式续写。
    auto set_soft_wrap(bool value) -> TextProps & { return assign(soft_wrap_row(), value); }

    /// @brief 装入行高倍数。
    /// @param value 相对字高的行距倍数。
    /// @return 引用本属性对象，便于链式续写。
    auto set_line_height(float value) -> TextProps & { return assign(line_height_row(), value); }

    /// @brief 装入字形间距。
    /// @param value 字符间额外间距 dp；FontEngine 暂不支持时优雅降级（不生效）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_letter_spacing(float value) -> TextProps & { return assign(letter_spacing_row(), value); }

    /// @brief 装入词间距。
    /// @param value 词间额外间距 dp；FontEngine 暂不支持时优雅降级（不生效）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_word_spacing(float value) -> TextProps & { return assign(word_spacing_row(), value); }

    /// @brief 装入字重档位。
    /// @param value 字重枚举。
    /// @return 引用本属性对象，便于链式续写。
    auto set_font_weight(FontWeight value) -> TextProps & { return assign(font_weight_row(), value); }

    /// @brief 装入字重（CSS 数值口径通道：代码生成器按数值字面量发射）。
    /// @param value CSS 字重数值（100..900），转为对应档位。
    /// @return 引用本属性对象，便于链式续写。
    auto set_font_weight(int value) -> TextProps & { return set_font_weight(static_cast<FontWeight>(value)); }

    /// @brief 装入字形风格。
    /// @param value 风格枚举。
    /// @return 引用本属性对象，便于链式续写。
    auto set_font_style(FontStyle value) -> TextProps & { return assign(font_style_row(), value); }

    /// @brief 装入装饰线。
    /// @param value 装饰线位组合。
    /// @return 引用本属性对象，便于链式续写。
    auto set_decoration(TextDecoration value) -> TextProps & { return assign(decoration_row(), value); }

    /// @brief 装入装饰线颜色（表态即脱离「同文字色」回落）。
    /// @param value 新装饰线色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_decoration_color(Color value) -> TextProps & { return assign(decoration_color_row(), value); }

    /// @brief 装入文本底色。
    /// @param value 底色；alpha = 0 表示不绘制底色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_background_color(Color value) -> TextProps & { return assign(background_color_row(), value); }

    // ---- 回落点提供者：常量档与派生档的唯一基线（同族互读只经读件） ----

    /// @brief 字号回落基线（pt）。
    /// @return 14.0F。
    [[nodiscard]] static auto font_size_default() -> float { return 14.0F; }

    /// @brief 水平对齐回落基线。
    /// @return `TextAlign::Left`。
    [[nodiscard]] static auto text_align_default() -> TextAlign { return TextAlign::Left; }

    /// @brief 方向回落基线：仅作读件取值，继承语义由表态位判定（见类注释）。
    /// @return `TextDirection::LTR`。
    [[nodiscard]] static auto direction_default() -> TextDirection { return TextDirection::LTR; }

    /// @brief 最大行数回落基线。
    /// @return 0（不限）。
    [[nodiscard]] static auto max_lines_default() -> int { return 0; }

    /// @brief 溢出策略回落基线。
    /// @return `TextOverflow::Clip`。
    [[nodiscard]] static auto overflow_default() -> TextOverflow { return TextOverflow::Clip; }

    /// @brief 软换行回落基线。
    /// @return true（默认可折行）。
    [[nodiscard]] static auto soft_wrap_default() -> bool { return true; }

    /// @brief 行高倍数回落基线。
    /// @return 1.0F。
    [[nodiscard]] static auto line_height_default() -> float { return 1.0F; }

    /// @brief 字形间距回落基线。
    /// @return 0.0F。
    [[nodiscard]] static auto letter_spacing_default() -> float { return 0.0F; }

    /// @brief 词间距回落基线。
    /// @return 0.0F。
    [[nodiscard]] static auto word_spacing_default() -> float { return 0.0F; }

    /// @brief 字重回落基线。
    /// @return `FontWeight::Normal`。
    [[nodiscard]] static auto font_weight_default() -> FontWeight { return FontWeight::Normal; }

    /// @brief 字形风格回落基线。
    /// @return `FontStyle::Normal`。
    [[nodiscard]] static auto font_style_default() -> FontStyle { return FontStyle::Normal; }

    /// @brief 装饰线回落基线。
    /// @return `TextDecoration::None`。
    [[nodiscard]] static auto decoration_default() -> TextDecoration { return TextDecoration::None; }

    /// @brief 文本底色回落基线：全透明（不绘制底色）。
    /// @return 透明色。
    [[nodiscard]] static auto background_color_default() -> Color { return Color::transparent(); }

    /// @brief 装饰线色回落点（派生档）：读同族 `color` 行的生效值。
    /// @param self 本属性对象（经它读 `color` 行，故显式设色时装饰线随该色）。
    /// @param ctx 构建上下文：未表态时 `color` 行本身还要解主题。
    /// @return 生效文字色。
    [[nodiscard]] static auto decoration_color_default(const TextProps &self, const BuildContext &ctx) -> Color {
        return self.color(ctx);
    }

  private:
    ReadOnlyState<LocalizedString> content_;  ///< 内容来源（必填 ⇒ 裸句柄）
    std::optional<float> font_size_;  ///< 字号表态位（pt）
    std::optional<ReadOnlyState<Color>> color_;  ///< 文字色来源（可空 ⇒ 未表态即无源可订）
    std::optional<TextAlign> text_align_;  ///< 水平对齐表态位
    std::optional<TextDirection> direction_;  ///< 书写方向表态位
    std::optional<int> max_lines_;  ///< 最大行数表态位
    std::optional<TextOverflow> overflow_;  ///< 溢出策略表态位
    std::optional<bool> soft_wrap_;  ///< 软换行表态位
    std::optional<float> line_height_;  ///< 行高倍数表态位
    std::optional<float> letter_spacing_;  ///< 字形间距表态位
    std::optional<float> word_spacing_;  ///< 词间距表态位
    std::optional<FontWeight> font_weight_;  ///< 字重表态位
    std::optional<FontStyle> font_style_;  ///< 字形风格表态位
    std::optional<TextDecoration> decoration_;  ///< 装饰线表态位
    std::optional<Color> decoration_color_;  ///< 装饰线色表态位
    std::optional<Color> background_color_;  ///< 文本底色表态位

    friend class AbstractProps<TextProps>;
};

/// @brief 文本控件（叶 widget）：测量文字自然尺寸，绘制文本。
///
/// 属性一律经 `TextProps` 的读写件访问（本类公有继承它，故 `txt.set_font_size(16.0F)` 与
/// `txt.font_size()` 都直接可用）；链式便捷写法 `au::Text("Hi").font_size(14).color(...)` 保留在
/// 本类上，`au::Text(au::TextProps().set_content("Hi").set_font_size(14.0F))` 亦等价。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
class Text : public LeafWidget, public TextProps {
  public:
    /// @brief 纯展示件默认不是 Tab 停点：只有挂上点击 / 手势 / 菜单 / 滚动 / 键盘认领时
    ///        才可聚焦（覆写基类 public virtual；分级默认见 specification/05 §4.2）。
    /// @return `has_input_semantics()` 的判定结果。
    [[nodiscard]] auto wants_focus() const -> bool override { return has_input_semantics(); }
    /// @brief 默认构造：空内容、默认字体（14pt）、跟随主题的文字色、不限行、自动换行。
    Text() { bind(*this); }
    /// @brief 配置块构造：整体接管 `TextProps` 各行表态位。
    /// @param props 属性聚合（链式写件产出的对象），移入本对象基类子对象。
    explicit Text(TextProps props) : TextProps(std::move(props)) { bind(*this); }
    /// @brief 以本地化字符串构造（内容为自有常量来源，随换源可控）。
    /// @param s 待显示的 `LocalizedString`（key + fallback 文本）。
    explicit Text(const LocalizedString &s) : Text() { set_content(s); }
    /// @brief 以 UTF-8 文本构造。
    /// @param s 待显示的字面文本（包成非本地化内容）。
    explicit Text(const std::string &s) : Text() { set_content(s); }
    /// @brief 以 C 字符串构造。
    /// @param s 待显示的字面文本（nullptr 语义由 `LocalizedString` 转换处理）。
    explicit Text(const char *s) : Text() { set_content(s); }

    /// @brief 把属性面的同名读件带回重载集。
    ///
    /// 便捷写件与同名读件共存：`color(Color)` / `font_size(float)` / `font_weight(FontWeight)` 会
    /// 遮蔽基类无参读件，显式 `using` 把读件带回重载集。
    using TextProps::color;
    /// @brief 把属性面的同名读件带回重载集（控件便捷写件 `font_size(float)` 会遮蔽它）。
    using TextProps::font_size;
    /// @brief 把属性面的同名读件带回重载集（控件便捷写件 `font_weight(FontWeight)` 会遮蔽它）。
    using TextProps::font_weight;

    /// @brief 运行时可查询的默认属性值（属性表的单一事实来源，需求 SPEC.API.SENSIBLE-DEFAULTS.001）。
    /// @return 一份缺省构造的 `TextProps`：各行未表态，生效值即该行回落点。
    [[nodiscard]] static auto defaults() -> TextProps { return {}; }

    /// @brief 替换文本内容来源（展示属性换源）：常量由隐式 `ReadOnlyState` 构造接入。
    /// @param next 新的内容来源；挂载后调用会退订旧源、订阅新源并标脏。
    /// @return 自身引用，便于链式调用。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_content(const LocalizedString &next) -> Text & {
        TextProps::set_content(next);
        return *this;
    }

    /// @brief 替换文本内容（链式常量写法）。
    /// @param s 新的 UTF-8 文本（换入自有常量来源，不反写任何上游）。
    /// @return 自身引用，便于链式调用。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_content(const std::string &s) -> Text & {
        TextProps::set_content(s);
        return *this;
    }

    /// @brief 替换文本内容（C 字符串常量写法；消除 `const char*` 在两个常量重载间的歧义）。
    /// @param s 新的 UTF-8 文本；nullptr 语义由 `LocalizedString` 转换处理。
    /// @return 自身引用，便于链式调用。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_content(const char *s) -> Text & {
        TextProps::set_content(s);
        return *this;
    }

    /// @brief 换绑文本内容来源（响应式换源）。
    /// @param next 新的只读句柄来源。
    /// @return 自身引用，便于链式调用。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_content(ReadOnlyState<LocalizedString> next) -> Text & {
        TextProps::set_content(std::move(next));
        return *this;
    }

    /// @brief 设置字号 pt（链式）；非正数降级为 14pt 并产生诊断（需求 SPEC.QUALITY.CORE.GRACEFUL-DEGRADATION.001）。
    /// @param pt 字号（point）；<= 0 时取默认 14pt 并记 `Diagnostics::degraded`。
    /// @return 自身引用，便于链式调用。
    auto font_size(float pt) -> Text & {
        if (pt <= 0.0F) {
            Diagnostics::degraded("widget", "Text font size is <= 0, degraded to 14pt");
            set_font_size(14.0F);
        } else {
            set_font_size(pt);
        }
        return *this;
    }

    /// @brief 设置文字颜色（链式）。
    /// @param c 文字前景色。
    /// @return 自身引用，便于链式调用。
    auto color(Color c) -> Text & {
        set_color(c);
        return *this;
    }

    /// @brief 加粗（链式）：字重置为 `FontWeight::Bold`（CSS 700）。
    /// @return 自身引用，便于链式调用。
    auto bold() -> Text & {
        set_font_weight(FontWeight::Bold);
        return *this;
    }

    /// @brief 设置字体族名（链式）。族名不入属性表（从不参与序列化）。
    /// @param f 字体 family（移入内部存储）；无匹配字体时由 FontEngine 走回退链。
    /// @return 自身引用，便于链式调用。
    auto family(std::string f) -> Text & {
        font_family_ = std::move(f);
        mark_needs_layout();
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置字重（链式）：装入 `font_weight` 行的枚举值。
    /// @param w 字重档位（其底层值即 CSS 数值口径）。
    /// @return 自身引用，便于链式调用。
    auto font_weight(FontWeight w) -> Text & {
        set_font_weight(w);
        return *this;
    }

    /// @brief 逐控件覆写抗锯齿模式（链式）：彩色/动画/渐变背景上用 `Supersample` 可避免 ClearType 子像素白边。
    ///        该设置不入属性表（从不参与序列化）。
    /// @param mode 文本抗锯齿模式；绘制期优先于进程级 `text_aa_mode()`。
    /// @return 自身引用，便于链式调用。
    auto text_aa(render::TextAAMode mode) -> Text & {
        text_aa_mode_ = mode;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置水平对齐（链式，`set_text_align` 的历史别名）。
    /// @param a 对齐方式；`Justify` 且多行时对非末行做逐词均分（见 `justify_layout`）。
    /// @return 自身引用，便于链式调用。
    auto set_align(TextAlign a) -> Text & {
        set_text_align(a);
        return *this;
    }

    /// @brief 解析当前应显示的文本（i18n 求值）。
    /// @param ctx 构建上下文：提供 `Environment` 注入的 `Locale`；缺失时按默认 locale 解析。
    /// @return 解析后的 UTF-8 文本；内容带 key 时查默认字符串表，否则为原文。
    [[nodiscard]] auto resolved_text(const BuildContext &ctx) const -> std::string {
        const auto *lp = ctx.environment<Locale>();
        return content().resolve(&default_catalog(), (lp != nullptr) ? *lp : Locale{});
    }

    /// @brief 解析生效文字色：未显式设色时经 ctx 取主题 `on_surface`（ThemeScope 换肤即生效），
    ///        显式设色优先。
    /// @param ctx 构建上下文：供主题取色；无注入时 `inherit_theme` 回落 light（与 OS 未表态同口径）。
    /// @return 生效文字色。
    [[nodiscard]] auto resolve_text_color(const BuildContext &ctx) const -> Color { return TextProps::color(ctx); }

    /// @brief 收集响应式信号：内容与文字色两条来源行的当前句柄。
    /// @param out 输出向量：挂载时由基类登记依赖。
    auto collect_signals(StateSink &out) -> void override { collect_prop_sources(out); }

    /// @brief widget 类型名（结构快照 JSON 用）。
    /// @return 字面量 `"Text"`。
    [[nodiscard]] auto type_name() const -> const char * override { return "Text"; }

    /// @brief 首行基线（内容盒顶 → 基线）：Text 的首行就从内容盒顶起排（`on_paint` 的 `y = bounds.origin.y`），
    ///        故即有效字体的 ascent，与 `draw_text` 的 pen_y 同源（`FontEngine::measure_ascent`）。
    ///        多行文本仍取首行；不做整像素 snap（见 `Widget::baseline_distance` 的说明）。
    /// @param ctx 构建上下文：用于取有效字体（含字号缩放）。
    /// @return 基线距离 dp；字体度量不可用时 nullopt（调用方按无基线对齐处理）。
    [[nodiscard]] auto baseline_distance(const BuildContext &ctx) const -> std::optional<float> override;

    /// @brief 无障碍名称：取文本内容。
    ///
    /// 优先复用绘制期缓存的解析结果 `display_text_`（= `cached_resolved_text_`，已解析 i18n）；
    /// 未经绘制时退回按默认 locale 现场解析 `content`，保证无绘制查询也有 name。
    /// @return 显示文本（UTF-8）。
    /// @note Side-effects: reads i18n table
    [[nodiscard]] auto accessibility_label() const -> std::string override {
        if (!display_text_.empty()) {
            return display_text_;
        }
        return content().resolve(&default_catalog(), Locale{});
    }

    /// @brief 运行时自描述：属性项全部由权威表发射，控件只补自己的事实。
    /// @return 名为 "Text" 的描述表：`properties` 按 `rows()` 点名顺序，无事件，无子节点。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        auto descriptor = props_descriptor("Text");
        descriptor.children_policy = "none";
        descriptor.examples = {"au::Text(\"Hello\")",
                               "au::Text(au::TextProps().set_content(\"Hi\").set_font_size(16.0F))"};
        return descriptor;
    }

    /// @brief 运行时多态自描述入口。
    /// @return 转发 `describe_static()` 的结果。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 构建期属性约束校验（specification/04-widget.md §2.2）：当前校验字号必须为正。
    /// @return 通过返回成功；否则为 `WidgetInvalidProp` 错误（含属性名与原因）。
    [[nodiscard]] auto validate_props() const -> Result<void> override;

    /// @brief 序列化排版属性到 props JSON：必填行恒写，可空行只在表态时写。
    /// @param props 输出对象，先由基类填通用属性；未表态的行不产生键。
    /// @note Rebuildable: yes, via from_json
    auto serialize_props(Json &props) const -> void override;
    /// @brief 从 props JSON 还原排版属性：缺失键保持未表态，非法值退回该行回落点并记诊断。
    /// @param props 属性 JSON。
    auto deserialize_props(const Json &props) -> void override;

    /// @brief 指针事件：建立/延伸选区、按下定位 caret（实现见 `src/aurora/widget/text.cpp`）。
    /// @param e 鼠标事件；命中本行文本时改写 `caret_`/`sel_start_`/`sel_end_`。
    auto on_pointer_event(MouseEvent &e) -> void override;
    /// @brief 键盘事件：方向键移 caret、Shift 扩选、Home/End 移行首行尾。
    /// @param e 按键事件（实现见 `src/aurora/widget/text.cpp`）。
    auto on_key_event(KeyEvent &e) -> void override;

    /// @brief 失焦时取消选区。
    /// @param focused false 时清除选区（`sel_end_` 回落哨兵）、把选区起点贴回 caret 并标脏重绘。
    auto on_focus_change(bool focused) -> void override {
        if (!focused) {
            sel_end_ = AURORA_NO_SEL;  // 标记无选区（含头含尾模型下，起点=终点表示 1 字符而非无选区）
            sel_start_ = caret_;
            selecting_ = false;
            mark_needs_paint();
        }
        Widget::on_focus_change(focused);
    }

    /// @brief 选区（码点下标，含头含尾），对外以 caret 区间 [a, b+1) 形式返回，
    /// 与命中测试/高亮/复制等下游逻辑兼容。无选区时返回 {0, 0}。
    /// @return `{起点, 终点}` 码点下标对；无选区时两端同为 `caret_`。
    [[nodiscard]] auto selection() const -> std::pair<size_t, size_t> {
        if (!has_selection()) {
            return {caret_, caret_};
        }
        const size_t a = std::min(sel_start_, sel_end_);
        const size_t b = std::max(sel_start_, sel_end_);
        return {a, b + 1};
    }
    /// @brief 当前是否存在选区。
    /// @return `sel_end_` 不等于无选区哨兵 `AURORA_NO_SEL` 时为 true。
    [[nodiscard]] auto has_selection() const -> bool { return sel_end_ != AURORA_NO_SEL; }
    /// @brief 最近一次绘制所用的显示文本（i18n 已解析）。
    /// @return 指向内部缓存串的引用；未绘制过为空串，命中测试/选区即以它为下标基准。
    [[nodiscard]] auto display_text() const -> const std::string & { return display_text_; }

  protected:
    /// @brief 绘制文本：底色 → 逐可视行字形 → 装饰线 → 选区高亮与 caret。
    /// @param p 软件光栅画笔。
    /// @param bounds 本控件的绘制矩形（全局坐标）。
    /// @param ctx 构建上下文：供 locale 解析与主题取色。
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;

    /// @brief 测量并按可用宽度折行：记录可视行、行首码点与行高供绘制/命中复用。
    /// @param c 上游约束：以 `max` 为可用宽度做软换行，无界约束时退化为自然尺寸。
    /// @param ctx 构建上下文：提供 locale（解析显示文本）与字号缩放设置。
    /// @return 文本在该宽度下所需的尺寸（dp，经约束夹取）。
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;

  private:
    /// @brief 无选区哨兵：sel_end_ 取此值时表示当前没有选区。
    /// （命名带 `AURORA_` 前缀：类内 `static constexpr` 走全局常量口径，见 `.clang-tidy`。）
    static constexpr size_t AURORA_NO_SEL = static_cast<size_t>(-1);

    /// @brief 生效字体：族名（本控件私有）＋ 字号行 ＋ 字重行拼装。
    /// @return 供度量、绘制与命中测试共用的 `Font`。
    [[nodiscard]] auto current_font() const -> Font {
        return Font{.family = font_family_, .size_pt = font_size(), .weight = static_cast<int>(font_weight())};
    }

    std::string font_family_ = Font{}.family;  ///< 字体族名（不入属性表，从不参与序列化）
    std::optional<render::TextAAMode> text_aa_mode_;  ///< 逐控件抗锯齿覆写（不入属性表）

    size_t sel_start_ = 0;  ///< 选区起点（含入的码点下标：该字符被选中）
    size_t sel_end_ = AURORA_NO_SEL;  ///< 选区终点（含入的码点下标；= AURORA_NO_SEL 表示无选区）
    size_t caret_ = 0;  ///< 光标（caret 位置，0..码点数；用于键盘导航）
    bool selecting_ = false;  ///< 是否正在拖选
    std::string display_text_;  ///< 最近一次绘制所用显示文本（命中测试/选区使用）
    std::string cached_resolved_text_;  ///< 缓存的 resolved_text 结果（避免每帧重复解析）
    bool resolved_dirty_ = true;  ///< resolved 缓存是否需重新计算
    std::vector<std::string> lines_;  ///< 最近一次布局所得折行结果（绘制复用）
    std::vector<size_t> line_cp_start_;  ///< 每个可视行首字符在 display_text_ 中的码点下标
    float line_h_ = 0.0F;  ///< 行高（含 line_height 倍数）
    float layout_w_ = 0.0F;  ///< 最近一次布局所得控件宽度（命中测试按对齐偏移需要）
    float paint_scale_ = 1.0F;  ///< 最近一次绘制的帧缓冲像素比（dp→物理；实显 caret 校正用）

    static auto split_words(const std::string &text) -> std::vector<std::string>;
    /// @brief 折行结果：lines=可视行文本，cp_start=各行首字符在原始 text 中的码点下标（一一对应）。
    using WrapResult = std::pair<std::vector<std::string>, std::vector<size_t>>;
    static auto finalize_lines(std::vector<std::string> lines, std::vector<size_t> cp_start, const Font &f, float max_w,
                               int max_lines, TextOverflow overflow, const render::TextLayoutOpts &opts) -> WrapResult;
    static auto wrap_lines(const std::string &text, const Font &f, float max_w, bool soft_wrap, int max_lines,
                           TextOverflow overflow, const render::TextLayoutOpts &opts) -> WrapResult;
    /// @brief 生效字体：补齐非法字号并施加无障碍字号缩放（倍率取进程级设置）。
    static auto effective_font(const Font &base) -> Font;
    /// @brief 生效字体（带上下文）：字号倍率优先取 `Environment` 注入的 `AccessibilitySettings`，
    ///        未注入时回落进程级设置——即「树级覆盖 > 进程默认」。
    static auto effective_font(const Font &base, const BuildContext &ctx) -> Font;
    /// @brief 生效书写方向：控件显式属性 > `Environment` 注入 > 进程级（host_set）；
    ///        无任何显式来源返回 nullopt（shaping 按内容 guess，默认行为不变）。
    ///        命中测试路径无 `BuildContext`，用无 ctx 版本（回落进程级）。
    [[nodiscard]] auto effective_direction(const BuildContext &ctx) const -> std::optional<TextDirection>;
    [[nodiscard]] auto effective_direction() const -> std::optional<TextDirection>;
    static auto cp_len(unsigned char c) -> size_t;
    static auto cp_count(const std::string &s) -> size_t;
    static auto cp_slice(const std::string &s, size_t start, size_t count) -> std::string;
    [[nodiscard]] auto selected_text() const -> std::string;

    /// @brief Justify 行逐词布局项（与 on_paint 两端对齐绘制严格一致）。
    struct JustifiedWord {
        std::string text;  ///< 词内容（不含空格）
        size_t cp_begin = 0;  ///< 词首字符在行内的码点下标
        size_t cp_end = 0;  ///< 词尾后一位置（行内码点下标，不含）
        float x = 0.0F;  ///< 词左缘相对行左缘的 x
        float w = 0.0F;  ///< 词自然宽度
    };
    /// @brief 计算 Justify 行的逐词均分布局（剩余宽度均分进词间隙）；
    ///        词数 < 2 时返回空（该行不做两端对齐，与 on_paint 一致）。
    /// @param line 可视行文本（UTF-8，空格按 1 码点计）。
    /// @param f 该行走的有效字体（已施加字号缩放）。
    /// @param opts 文本布局选项（与绘制同源，保证度量一致）。
    /// @param avail 该行可用宽度 dp。
    /// @return 逐词布局项（含行内码点区间与均分后的 x）；不足两词返回空向量。
    static auto justify_layout(const std::string &line, const Font &f, const render::TextLayoutOpts &opts, float avail)
        -> std::vector<JustifiedWord>;
    /// @brief 可视行 li 是否按两端对齐绘制（Justify 且多行且非末行，与 on_paint 判定一致）。
    /// @param li 可视行下标（0 基，对应 `lines_`）。
    /// @return 该行需要走 `justify_layout` 均分路径时为 true。
    [[nodiscard]] auto is_justified_line(size_t li) const -> bool;
    /// @brief 行内 caret x（相对行左缘）：Justify 行按均分布局取词位（行尾 = 行右缘 avail），
    ///        其余行走 FontEngine::display_caret_x（按 paint_scale_ 推导物理 DPI 的前缀
    ///        extent，逐字符与实绘字形对齐；scale=1 退化为 caret_x）；选区高亮与绘制像素一一对应。
    /// @param li 可视行下标。
    /// @param cp_in_line caret 在该行内的码点位置（0..行长度）。
    /// @param f 有效字体。
    /// @param opts 文本布局选项。
    /// @param avail 该行可用宽度 dp（Justify 行尾 caret 即取此值）。
    /// @return caret 相对行左缘的 x（dp）。
    [[nodiscard]] auto line_caret_x(size_t li, size_t cp_in_line, const Font &f, const render::TextLayoutOpts &opts,
                                    float avail) const -> float;
    /// @brief 行内命中测试：返回 {caret 位置, 含头含尾字符下标}（行内码点）；
    ///        Justify 行按均分布局反解（词间拉伸间隙整体归属其空格字符），
    ///        其余行走实显命中（FontEngine::display_hit_test_char*，与实绘字形逐字符对齐）。
    /// @param li 可视行下标。
    /// @param x 点击点相对行左缘的 x（dp）。
    /// @param f 有效字体。
    /// @param opts 文本布局选项。
    /// @param avail 该行可用宽度 dp。
    /// @return `{caret 位置, 选中字符下标}` 二元组（均为行内码点下标，含头含尾）。
    [[nodiscard]] auto line_hit_test(size_t li, float x, const Font &f, const render::TextLayoutOpts &opts,
                                     float avail) const -> std::pair<size_t, size_t>;
};

}  // namespace aurora
