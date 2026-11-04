#pragma once

#include <concepts>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/enums.h"
#include "aurora/core/json.h"
#include "aurora/core/types.h"
#include "aurora/environment/build_context.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/state/read_only_state.h"
#include "aurora/theming/theme.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/widget.h"

/// @brief 属性单一声明面：权威表行（`PropRow` 与 `prop_row` / `make_prop_rows`）和控件属性基类
/// （`AbstractProps`）。
///
/// 一个属性在本库里只声明一次：`XxxProps` 类里的一个私有字段 + 一行 `x_row()` + 在 `rows()`
/// 点名。写件、读件、脏标记档位、序列化、反序列化、响应式订阅与 `aurora_api.json` 的条目全部由
/// 那一行派生；控件侧只剩「公有继承 + 虚函数一行转调 + 构造尾部一次 `bind`」。
///
/// 三条支撑这个面成立的类型层事实：
///
///   - **可空 ⇔ 非必填**。字段是 `std::optional<T>` 即可撤表态、必有回落点；字段是裸 `T`（含不可
///     空的 `ReadOnlyState<T>`）即必填、回落点就是它自身。`required` 位与「能不能 `clear`」都由字段
///     形状推出，没有旁注可声明、也就没有旁注可以标错。
///   - **回落点强制**。每行要么给 `.fallback(...)`（活令牌成员指针 / 库内常量提供者 / 派生提供者三
///     形态之一），要么 `.required()`。没有第三种形状，于是「未表态时取到什么」在编译期就是确定的，
///     取值点不再需要逐属性手写的 `value_or` 口径。
///   - **读面每行恰一件**。回落点不随作用域变的行发无参 `x()`；回落点跟随主题（活令牌档）或由他行
///     派生（派生档）的行发 `x(const BuildContext &)`。两者互斥，所以「在渲染路径里选错读件」在类型
///     上不可表达 —— 没有上下文时根本调不到需要上下文的那一件，反过来多发的那一件则被覆盖门禁判红。
///
/// 公共面上的通用件只有 `clear(row)` 与各 `Props` 类自己的 `rows()`；`assign` / `resolve` / `bind`
/// 一律 `protected`。撤表态是这一件通用件而非逐属性 `reset_x()`；表态位与来源位也不发公共名字
/// （来源位由字段形状推导，序列化与 Inspector 经库内取值点消费）。
///
/// @note Thread: main-thread only（与控件属性读写一致）
/// @note Side-effects: none（行对象是纯值，接线与写入的副作用在 `AbstractProps` 上）
namespace aurora {

// ============================================================================
// 1. 字段形状判读：可空性、来源形态、成员指针的宿主与裸值类型
// ============================================================================

namespace detail {

/// @brief 逐层剥掉可空外壳与句柄外壳，得到该属性对外的裸值类型。
/// @tparam T 字段类型（`T` / `std::optional<T>` / `ReadOnlyState<T>` / `std::optional<ReadOnlyState<T>>`）。
template <typename T>
struct PropBare {
    /// @brief 裸值类型（已是裸值，原样给出）。
    /// 成员名 `X::type` 是标准库类型萃取的既定形态（与 std traits 组合时按此名查找），非本库命名自由度。
    /// NOLINTNEXTLINE(readability-identifier-naming)
    using type = T;
};
/// @brief 可空外壳的去壳规则（可嵌套：`std::optional<ReadOnlyState<T>>` 逐层剥到 `T`）。
template <typename T>
struct PropBare<std::optional<T>> {
    /// @brief 内层的裸值类型。
    /// 同上：`trait::type` 由惯例固定。
    /// NOLINTNEXTLINE(readability-identifier-naming)
    using type = PropBare<T>::type;
};
/// @brief 句柄外壳的去壳规则。
template <typename T>
struct PropBare<ReadOnlyState<T>> {
    /// @brief 句柄承载的裸值类型。
    /// 同上：`trait::type` 由惯例固定。
    /// NOLINTNEXTLINE(readability-identifier-naming)
    using type = T;
};
/// @brief `PropBare` 的别名写法。
template <typename T>
using PropBareT = PropBare<T>::type;

/// @brief 只剥最外层可空外壳、保留句柄外壳（用于判定「槽里有没有源」）。
template <typename T>
struct PropUnoptional {
    /// @brief 非可空字段原样返回。
    /// 同上：`trait::type` 由惯例固定。
    /// NOLINTNEXTLINE(readability-identifier-naming)
    using type = T;
};
/// @brief 可空字段的剥壳。
template <typename T>
struct PropUnoptional<std::optional<T>> {
    /// @brief 剥掉外层可空壳后的字段类型。
    /// 同上：`trait::type` 由惯例固定。
    /// NOLINTNEXTLINE(readability-identifier-naming)
    using type = T;
};
/// @brief `PropUnoptional` 的别名写法。
template <typename T>
using PropUnoptionalT = PropUnoptional<T>::type;

/// @brief 字段是否为可空形态（可空 ⇔ 非必填 ⇔ 可撤表态）。
template <typename T>
struct PropIsOptional : std::false_type {};
/// @brief `std::optional` 字段特化为真。
template <typename T>
struct PropIsOptional<std::optional<T>> : std::true_type {};

/// @brief 字段（剥掉可空外壳后）是否为响应式句柄，即该行是否「来源型」。
template <typename T>
struct PropIsHandle : std::false_type {};
/// @brief `ReadOnlyState` 字段特化为真。
template <typename T>
struct PropIsHandle<ReadOnlyState<T>> : std::true_type {};

/// @brief 从数据成员指针取出字段类型。
template <typename M>
struct PropFieldValue;
/// @brief 数据成员指针特化。
template <typename C, typename M>
struct PropFieldValue<M C::*> {
    /// @brief 字段类型（含可空 / 句柄外壳）。
    /// 同上：`trait::type` 由惯例固定。
    /// NOLINTNEXTLINE(readability-identifier-naming)
    using type = M;
};
/// @brief `PropFieldValue` 的别名写法。
template <typename M>
using PropFieldValueT = PropFieldValue<M>::type;

/// @brief 从数据成员指针取出宿主类型（本行所属的 `Props` 类）。
template <typename M>
struct PropFieldOwner;
/// @brief 数据成员指针特化。
template <typename C, typename M>
struct PropFieldOwner<M C::*> {
    /// @brief 宿主类型。
    /// 同上：`trait::type` 由惯例固定。
    /// NOLINTNEXTLINE(readability-identifier-naming)
    using type = C;
};
/// @brief `PropFieldOwner` 的别名写法。
template <typename M>
using PropFieldOwnerT = PropFieldOwner<M>::type;

/// @brief 属性写入后要打的脏档位（由表行给定；控件侧不再逐 setter 手写 `mark_needs_*`）。
struct PropDirtyBits {
    bool layout = false;  ///< 需要重排（几何随该属性变）
    bool paint = false;  ///< 需要重绘（画面随该属性变）
};

}  // namespace detail

// ============================================================================
// 2. 编解码登记的前置声明：行对象只需要「该类型能不能转 JSON / 怎么显示」
// ============================================================================

/// @brief 属性裸值类型的编解码登记：类型名、JSON 类型与双向转换。
///
/// 主模板故意不定义任何成员：未登记类型一经使用即编译期报错，报错点就落在那个类型的行上。新类型
/// 在自己那对 JSON 转换器旁特化本模板即可，机制层不需要任何逐属性代码。
///
/// 必需成员：`type_text()`（描述符的 `type`）、`json_text()`（描述符的 `json_type` 缺省值）、
/// `to_json` / `from_json`。可选成员：`display(const T &)`（缺省用 JSON 转义文本作显示串）、
/// `enum_names()`（缺省不发 `enum`）、`validate(const Json &, const PropDescriptor &)`（缺省即反序列化
/// 走宽容 `from_json`；有 `descriptor.h` 里那枚 `validate_prop<T>` 特化的类型才登记它）。
/// 类型名写成函数而不是常量：`static constexpr` 成员在 clang-tidy 的命名面里归「全局常量」档，与
/// 文件作用域那些 `AURORA_` 全大写常量同判，那对类型元数据是错的形式；成员函数走函数命名惯例，零豁免。
/// `pattern` / `constraint` / `conflicts_with` 三个描述符字段**不由本面派生** —— 全库现状对它们是零
/// 真实使用（只有两处空串初始化），由类型自动发它们等于给 92 个 `Color` 属性凭空加键，故留在手写面。
/// @tparam T 属性裸值类型。
template <typename T>
struct PropCodec {
    static_assert(sizeof(T) == 0, "property type needs a PropCodec registration beside its JSON converters");
};

/// @brief 该裸值类型已登记编解码（可发射、可序列化）。
/// @tparam T 属性裸值类型。
template <typename T>
concept prop_codec_registered = requires {
    { PropCodec<T>::type_text() } -> std::convertible_to<std::string_view>;
    { PropCodec<T>::to_json(std::declval<const T &>()) } -> std::same_as<Json>;
};

// ============================================================================
// 3. 权威表的一行：键名与私有字段钉在同一实参位，其余都是链式旁注
// ============================================================================

/// @brief 活令牌回落点的取值体：按当前作用域主题读出该令牌字段的值。
///
/// 令牌必须以**模板实参**给出（`.fallback<&ShapeScale::small>()`）：短名反查与取值体都要在编译期
/// 认出这根成员指针的身份，而普通实参位置的成员指针不是常量表达式，也不是合法的非类类型模板参数。
/// @tparam Scale 令牌组类型（`ColorRoles` / `ShapeScale` / `SpacingScale` / `ControlMetrics` /
///         `MotionScale` / `CsdRoles`）。
/// @tparam Value 令牌字段类型（必须等于本行裸值类型）。
/// @tparam Member 令牌成员指针。
/// @param ctx 构建上下文。
/// @return 当前作用域主题里该字段的值。
template <typename Scale, typename Value, Value Scale::*Member>
[[nodiscard]] auto prop_token_value(const BuildContext &ctx) -> Value {
    return detail::theme_section<Scale>(inherit_theme(ctx)).*Member;
}

/// @brief 令牌组是否登记了「成员指针 → 短名」对照表；未登记的组作回落点即编译期报错，因为
/// schema 里 `theme.<短名>` 的反查没有来源。
/// @tparam Scale 令牌组类型。
/// @tparam MemberPtr 令牌成员指针。
template <typename Scale, auto MemberPtr>
concept prop_token_registered = requires {
    { Scale::token_name(MemberPtr) } -> std::convertible_to<std::string_view>;
};

/// @brief 权威表的一行：属性键名、私有字段、脏档位、约束与回落点的唯一声明处。
///
/// 行对象是字面类型（成员只有 `std::string_view`、标志位与函数指针），整张表因此可在常量求值里
/// 成形；`AbstractProps::row_table()` 只物化一次，逐属性读写路径上不产生任何构造成本。旁注一律
/// 返回新行（本行不变），所以 `x_row()` 里的链式写法是纯值运算。
///
/// 回落点三形态共用同一个旁注名，由实参形态区分：
///
///   - `.fallback<&ColorRoles::primary>()` —— 活令牌档：随作用域换肤而变，本行发 `x(ctx)`。
///   - `.fallback(&XxxProps::some_default)` —— 常量档（`static T()`）：全库唯一基线，本行发 `x()`。
///   - `.fallback(&XxxProps::some_derived)` —— 派生档（`static T(const Derived &, const BuildContext &)`）：
///     读同族他行的生效值再加工（如 hover 由当前背景调暗），本行发 `x(ctx)`，且必须配
///     `.shows("...")` 给出静态可显示的默认串（没有实例就求不出值，发射面只能取显示串）。
///
/// @tparam FieldPtr 本行对应的私有数据成员指针（`&XxxProps::field_`）。
/// @note Thread: 行不可变，可跨帧持有
/// @note Side-effects: none
template <auto FieldPtr>
class PropRow {
  public:
    using Field = detail::PropFieldValueT<decltype(FieldPtr)>;  ///< 字段类型（含可空 / 句柄外壳）
    using Owner = detail::PropFieldOwnerT<decltype(FieldPtr)>;  ///< 宿主 `Props` 类
    using Bare = detail::PropBareT<Field>;  ///< 裸值类型（属性对外呈现的类型）

    /// @brief 本行的字段（写面与读面都经它落到宿主实例上）。
    /// 写成 `static constexpr` 函数而不是常量，理由同 `PropCodec::type_text()`：类内常量在命名门禁里
    /// 归「全局常量」档，与属性元数据的形式不符。
    /// @return 本行的私有数据成员指针。
    [[nodiscard]] static constexpr auto field() -> decltype(FieldPtr) { return FieldPtr; }
    /// @brief 必填位：字段不可空即必填，回落点就是字段自身。
    /// @return 必填为 true。
    [[nodiscard]] static constexpr auto is_required() -> bool { return !detail::PropIsOptional<Field>::value; }
    /// @brief 来源型位：字段是响应式句柄即有外部源可绑、也即有「写当前源」这条不断绑通道。
    /// @return 来源型为 true。
    [[nodiscard]] static constexpr auto is_source() -> bool {
        return detail::PropIsHandle<detail::PropUnoptionalT<Field>>::value;
    }

    std::string_view key;  ///< 序列化键名（同 schema 的属性名）
    detail::PropDirtyBits dirty;  ///< 脏档位
    std::string_view note_text;  ///< 说明（描述符的 `note`）
    std::string_view json_type_text;  ///< JSON 类型覆写（空则取该类型的默认表示）
    std::string_view min_text;  ///< 数值下界（描述符的 `minimum`）
    std::string_view max_text;  ///< 数值上界（描述符的 `maximum`）
    std::string_view requires_text;  ///< 属性依赖（描述符的 `requires_props`）
    std::string_view shows_text;  ///< 默认值显示串（派生档必需，其余档为可选覆写）
    std::string_view token_name;  ///< 活令牌短名（非空即回落点跟随主题）
    Bare (*token_fn)(const BuildContext &) = nullptr;  ///< 活令牌档取值体
    Bare (*const_fn)() = nullptr;  ///< 常量档提供者
    Bare (*derived_fn)(const Owner &, const BuildContext &) = nullptr;  ///< 派生档提供者

    /// @brief 打布局档：该属性变更需要重排。
    /// @return 带布局脏位的新行。
    [[nodiscard]] constexpr auto layout() const -> PropRow {
        auto next = *this;
        next.dirty.layout = true;
        return next;
    }

    /// @brief 打绘制档：该属性变更需要重绘。
    /// @return 带绘制脏位的新行。
    [[nodiscard]] constexpr auto paint() const -> PropRow {
        auto next = *this;
        next.dirty.paint = true;
        return next;
    }

    /// @brief 把「本行必填、回落点即自身」这句话显式写出来（必填位仍由字段形状推出）。
    /// @return 本行的副本。
    [[nodiscard]] constexpr auto required() const -> PropRow {
        static_assert(is_required(), "required() only applies to non-optional fields");
        return *this;
    }

    /// @brief 回落点形态一：主题令牌成员指针（随作用域换肤而变）。
    /// @tparam TokenPtr 令牌成员指针，必须作模板实参给出。
    /// @return 带活令牌回落点的新行。
    template <auto TokenPtr>
    [[nodiscard]] constexpr auto fallback() const -> PropRow {
        using Scale = detail::PropFieldOwnerT<decltype(TokenPtr)>;
        using Value = detail::PropFieldValueT<decltype(TokenPtr)>;
        static_assert(std::is_same_v<Value, Bare>, "fallback token type must equal the row's bare type");
        static_assert(prop_token_registered<Scale, TokenPtr>,
                      "fallback token group must register a member-pointer to short-name table");
        auto next = *this;
        next.token_fn = &prop_token_value<Scale, Value, TokenPtr>;
        next.token_name = Scale::token_name(TokenPtr);
        return next;
    }

    /// @brief 回落点形态二：库内常量提供者（全库唯一基线，不随作用域变）。
    /// @param provider 与该行裸值同型的静态函数（`static T XxxProps::x_default()`）。
    /// @return 带常量回落点的新行。
    [[nodiscard]] constexpr auto fallback(Bare (*provider)()) const -> PropRow {
        auto next = *this;
        next.const_fn = provider;
        return next;
    }

    /// @brief 回落点形态三：派生提供者（读同族他行的生效值再加工）。
    /// @param provider 形如 `static T(const Derived &, const BuildContext &)` 的静态函数。
    /// @return 带派生回落点的新行。
    [[nodiscard]] constexpr auto fallback(Bare (*provider)(const Owner &, const BuildContext &)) const -> PropRow {
        auto next = *this;
        next.derived_fn = provider;
        return next;
    }

    /// @brief 属性说明（进描述符与 schema 的 `note`）。
    /// @param text 说明文本（静态存储期字面量）。
    /// @return 带说明的新行。
    [[nodiscard]] constexpr auto note(std::string_view text) const -> PropRow {
        auto next = *this;
        next.note_text = text;
        return next;
    }

    /// @brief JSON 类型覆写（仅当该类型的默认表示不足以表达本行时使用）。
    /// @param text JSON Schema 类型串。
    /// @return 带 JSON 类型旁注的新行。
    [[nodiscard]] constexpr auto json_type(std::string_view text) const -> PropRow {
        auto next = *this;
        next.json_type_text = text;
        return next;
    }

    /// @brief 数值下界（无上界）。
    /// @param low 下界显示串（如 `"0"`）。
    /// @return 带下界的新行。
    [[nodiscard]] constexpr auto bounds(std::string_view low) const -> PropRow {
        auto next = *this;
        next.min_text = low;
        return next;
    }

    /// @brief 数值上下界。
    /// @param low 下界显示串。
    /// @param high 上界显示串。
    /// @return 带上下界的新行。
    [[nodiscard]] constexpr auto bounds(std::string_view low, std::string_view high) const -> PropRow {
        auto next = *this;
        next.min_text = low;
        next.max_text = high;
        return next;
    }

    /// @brief 属性依赖（如 `border_color` 要求 `border_width` 大于 0）。
    ///
    /// 名字取 `requires_prop`：`requires` 是 C++20 关键字，作成员名不合法。
    /// @param other_key 依赖的另一属性键名。
    /// @return 带依赖旁注的新行。
    [[nodiscard]] constexpr auto requires_prop(std::string_view other_key) const -> PropRow {
        auto next = *this;
        next.requires_text = other_key;
        return next;
    }

    /// @brief 默认值显示串覆写（派生档必需：没有实例就求不出值，静态发射只能取显示串）。
    /// @param alias 显示串（如 `"auto"` / `"none"`）。
    /// @return 带显示串旁注的新行。
    [[nodiscard]] constexpr auto shows(std::string_view alias) const -> PropRow {
        auto next = *this;
        next.shows_text = alias;
        return next;
    }

    /// @brief 本行回落点是否依赖上下文（活令牌档与派生档都是）—— 决定本行发 `x(ctx)` 而非 `x()`。
    /// @return 作用域依赖档为 true。
    [[nodiscard]] constexpr auto scope_dependent() const -> bool {
        return token_fn != nullptr || derived_fn != nullptr;
    }

    /// @brief 生效回落值：给定上下文下本行未表态时取到的值。
    /// @param self 宿主 `Props` 实例（派生档要读它的他行生效值）。
    /// @param ctx 构建上下文。
    /// @return 回落裸值。
    [[nodiscard]] auto effective(const Owner &self, const BuildContext &ctx) const -> Bare {
        if (derived_fn != nullptr) {
            return derived_fn(self, ctx);
        }
        if (token_fn != nullptr) {
            return token_fn(ctx);
        }
        return const_fn();
    }

    /// @brief 回落基线：裸上下文（没有注入作用域）下本行未表态时取到的值。
    /// @param self 宿主 `Props` 实例。
    /// @return 基线裸值。
    [[nodiscard]] auto baseline(const Owner &self) const -> Bare { return effective(self, BuildContext{}); }
};

/// @brief 该行必填：字段不可空 ⇔ 生效值就是字段自身，因此没有「撤表态」这一位。
/// @tparam Row 表行类型。
template <typename Row>
concept prop_row_required = Row::is_required();

/// @brief 该行有「表态 / 撤表态」两位：字段可空 ⇔ 非必填 ⇔ 可由用户显式退回回落点。
///
/// 命名成概念而不是就地写 `!Row::is_required()`：必填行的不可空是类型层事实，凡依赖它的重载集都写
/// `requires prop_row_optional<Row>`，编译期拒绝因此落在重载选择上（`no matching function`），
/// 不是运行期判空。写成带括号的 `requires(!...)` 时，本库注释门禁会把约束子句误当签名体（实测两条
/// 误报），连 `requires Row::is_required()` 这样带调用的约束也同理 —— 所以正反两侧各一枚概念名。
/// @tparam Row 表行类型。
template <typename Row>
concept prop_row_optional = !prop_row_required<Row>;

/// @brief 造一行：把私有字段与属性键名钉在同一处。
/// @tparam FieldPtr 私有数据成员指针（`&XxxProps::field_`）。
/// @param key 序列化键名。
/// @return 只带键名的新行（档位、约束与回落点由后续链式旁注补齐）。
template <auto FieldPtr>
[[nodiscard]] constexpr auto prop_row(std::string_view key) -> PropRow<FieldPtr> {
    PropRow<FieldPtr> row;
    row.key = key;
    return row;
}

/// @brief 把若干行组成权威表元组；**参数顺序即 schema 顺序**（替代旧的「字段声明序」承诺）。
/// @tparam Rows 行类型包。
/// @param rows 各行（顺序即对外属性表顺序）。
/// @return 行元组。
template <typename... Rows>
[[nodiscard]] constexpr auto make_prop_rows(Rows... rows) -> std::tuple<Rows...> {
    return std::make_tuple(std::move(rows)...);
}

namespace detail {

/// @brief 逐行遍历表元组：编译期展开，运行期无间接调用、无堆分配。
///
/// 回调取 `const &` 而不是转发引用：本函数按定义不接管可调用体的所有权（要接管就得逐行 `std::forward`，
/// 而展开里 `fn` 被调用 N 次，`forward` 反而错），写成 `Fn &&` 只会撞上「转发引用从未 forward」的判据。
/// @tparam Tuple 行元组类型。
/// @tparam Fn 可调用体，形参接受每行的 `const &`。
/// @param rows 行元组。
/// @param fn 逐行回调。
template <typename Tuple, typename Fn>
constexpr auto for_each_prop_row(const Tuple &rows, const Fn &fn) -> void {
    std::apply([&](const auto &...row) { (std::invoke(fn, row), ...); }, rows);
}

/// @brief 属性值的显示串：该类型自带 `display` 时用它，否则用 JSON 原生值的转义文本。
/// @tparam T 已登记的属性裸值类型。
/// @param v 待格式化的值。
/// @return 显示串；序列化失败时为空串。
template <prop_codec_registered T>
[[nodiscard]] auto prop_bare_display(const T &v) -> std::string {
    if constexpr (requires { PropCodec<T>::display(v); }) {
        return std::string{PropCodec<T>::display(v)};
    } else {
        auto dumped = json::dump(PropCodec<T>::to_json(v));
        return dumped ? std::move(dumped.value()) : std::string{};
    }
}

/// @brief 该裸值类型是否自带枚举合法值表（自带即发射 `enum` 字段）。
///
/// 写成概念而不是 `constexpr bool` 变量模板：变量模板在命名门禁里归「全局常量」档，与这里的形式不符。
/// @tparam T 属性裸值类型。
template <typename T>
concept prop_codec_has_enum_names = requires { PropCodec<T>::enum_names(); };

/// @brief 该裸值类型是否自带约束校验（自带即反序列化对非法值「退到该行回落基线并保持未表态」，其余按
///        `from_json` 宽容解析）。
///
///        判据取「登记里没有 `validate`」而不是 `requires { validate_prop<T>(...) }`：后者对任意 T
///        都成立（主模板只声明未定义），会在实例化时才报无匹配定义 —— 登记位才能区分「这个类型真有
///        校验特化」。形态同样取概念，理由见上一行。
/// @tparam T 属性裸值类型。
template <typename T>
concept prop_codec_has_validate = requires { PropCodec<T>::validate(Json{}, PropDescriptor{}); };

/// @brief 校验一个 JSON 值并按该类型约束判定合法与否；非法即经 `Diagnostics` 上报退化。
/// @tparam T 属性裸值类型。
/// @param raw 源 JSON 值。
/// @param entry 该行的描述符（含属性名与约束）。
/// @return 合法时为该值，非法时为 `nullopt`（已上报，调用方据此不写入）。
template <typename T>
[[nodiscard]] auto prop_bare_checked(const Json &raw, const PropDescriptor &entry) -> std::optional<T> {
    if constexpr (prop_codec_has_validate<T>) {
        auto checked = PropCodec<T>::validate(raw, entry);
        if (checked) {
            return checked.value();
        }
        Diagnostics::degraded("Property '" + entry.name + "' is invalid: " + checked.error().message, entry.name,
                              checked.error().code);
        return std::nullopt;
    } else {
        (void)entry;
        return PropCodec<T>::from_json(raw);
    }
}

/// @brief 取该类型的枚举合法值表（未自带则为空表，发射时不输出 `enum` 键）。
/// @tparam T 已登记的属性裸值类型。
/// @return 合法值列表。
template <prop_codec_registered T>
[[nodiscard]] auto prop_codec_enum_names() -> std::vector<std::string> {
    if constexpr (prop_codec_has_enum_names<T>) {
        std::vector<std::string> names;
        for (const auto &name : PropCodec<T>::enum_names()) {
            names.emplace_back(name);
        }
        return names;
    } else {
        return {};
    }
}

}  // namespace detail

// ============================================================================
// 4. 编解码登记：机制层用到的类型在此一次登记完，其余类型在自己的转换器旁特化
// ============================================================================

/// @brief 非布尔算术类型的编解码体（`float` 落 number，整型落 integer），供登记与特化共用。
/// @tparam T 算术类型（排除 `bool`：它有独立的登记）。
namespace detail {
template <typename T>
    requires std::is_arithmetic_v<T> && (!std::is_same_v<T, bool>)
struct ArithPropCodec {
    /// @brief schema 的 C++ 类型名：浮点落 `float`、整型落 `int`。
    /// @return 按该类型形态给出的串。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view {
        return std::is_floating_point_v<T> ? "float" : "int";
    }

    /// @brief schema 的 JSON 类型名：浮点落 `number`、整型落 `integer`。
    /// @return 按该类型形态给出的串。
    [[nodiscard]] static constexpr auto json_text() -> std::string_view {
        return std::is_floating_point_v<T> ? "number" : "integer";
    }

    /// @brief 值 → JSON 数字。
    /// @param v 数值。
    /// @return 数字 JSON 值。
    [[nodiscard]] static auto to_json(const T &v) -> Json { return Json{v}; }

    /// @brief JSON → 值（类型不符取零值）。
    /// @param j JSON 值。
    /// @return 解析出的数值。
    [[nodiscard]] static auto from_json(const Json &j) -> T { return j.as_or<T>(T{}); }
};
}  // namespace detail

/// @brief 算术属性登记：整型族里除 `float` / `int` 之外没有约束校验特化，故只取公共体。
/// @tparam T 算术类型（排除 `bool`）。
template <typename T>
    requires std::is_arithmetic_v<T> && (!std::is_same_v<T, bool>)
struct PropCodec<T> : detail::ArithPropCodec<T> {};

/// @brief 单精度属性：在公共体之外登记约束校验（`minimum` / `maximum` 经描述符生效）。
template <>
struct PropCodec<float> : detail::ArithPropCodec<float> {
    /// @brief JSON → 值并按该行的约束校验；非法即返回错误（由调用侧降级并上报）。
    /// @param j JSON 值。
    /// @param desc 该行描述符（含 `minimum` / `maximum`）。
    /// @return 校验结果。
    [[nodiscard]] static auto validate(const Json &j, const PropDescriptor &desc) -> Result<float> {
        return validate_prop<float>(j, desc);
    }
};

/// @brief 整数属性：同 `float`，整数族里只有本档登记了约束校验。
template <>
struct PropCodec<int> : detail::ArithPropCodec<int> {
    /// @brief JSON → 值并按该行的约束校验；非法即返回错误（由调用侧降级并上报）。
    /// @param j JSON 值。
    /// @param desc 该行描述符（含 `minimum` / `maximum`）。
    /// @return 校验结果。
    [[nodiscard]] static auto validate(const Json &j, const PropDescriptor &desc) -> Result<int> {
        return validate_prop<int>(j, desc);
    }
};

/// @brief 双精度属性：`type` 与 `float` 区分（现状描述符里两者是两种类型名）。
template <>
struct PropCodec<double> {
    /// @brief schema 的 C++ 类型名。
    /// @return `double`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "double"; }

    /// @brief schema 的 JSON 类型名。
    /// @return `number`。
    [[nodiscard]] static constexpr auto json_text() -> std::string_view { return "number"; }

    /// @brief 值 → JSON 数字。
    /// @param v 双精度数值。
    /// @return 数字 JSON 值。
    [[nodiscard]] static auto to_json(const double &v) -> Json { return Json{v}; }

    /// @brief JSON → 值。
    /// @param j JSON 值。
    /// @return 解析出的双精度值。
    [[nodiscard]] static auto from_json(const Json &j) -> double { return j.as_or<double>(0.0); }
};

/// @brief 布尔属性：JSON `boolean`，显示串 `true` / `false`。
template <>
struct PropCodec<bool> {
    /// @brief schema 的 C++ 类型名。
    /// @return `bool`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "bool"; }

    /// @brief schema 的 JSON 类型名。
    /// @return `boolean`。
    [[nodiscard]] static constexpr auto json_text() -> std::string_view { return "boolean"; }

    /// @brief 值 → JSON。
    /// @param v 布尔值。
    /// @return 布尔 JSON 值。
    [[nodiscard]] static auto to_json(const bool &v) -> Json { return Json{v}; }

    /// @brief JSON → 值。
    /// @param j JSON 值。
    /// @return 解析出的布尔值（非布尔取 false）。
    [[nodiscard]] static auto from_json(const Json &j) -> bool { return j.as_or<bool>(false); }

    /// @brief 显示串。
    /// @param v 布尔值。
    /// @return `"true"` 或 `"false"`。
    [[nodiscard]] static auto display(const bool &v) -> std::string_view { return v ? "true" : "false"; }

    /// @brief JSON → 值并按该行的 JSON 类型约束校验（非布尔即非法）。
    /// @param j JSON 值。
    /// @param desc 该行描述符。
    /// @return 校验结果。
    [[nodiscard]] static auto validate(const Json &j, const PropDescriptor &desc) -> Result<bool> {
        return validate_prop<bool>(j, desc);
    }
};

/// @brief 字符串属性：JSON `string`（显示串走默认的转义文本，即 `""`）。
template <>
struct PropCodec<std::string> {
    /// @brief schema 的 C++ 类型名。
    /// @return `string`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "string"; }

    /// @brief schema 的 JSON 类型名。
    /// @return `string`。
    [[nodiscard]] static constexpr auto json_text() -> std::string_view { return "string"; }

    /// @brief 值 → JSON。
    /// @param v 字符串。
    /// @return 字符串 JSON 值。
    [[nodiscard]] static auto to_json(const std::string &v) -> Json { return Json{v}; }

    /// @brief JSON → 值。
    /// @param j JSON 值。
    /// @return 解析出的字符串（非字符串取空串）。
    [[nodiscard]] static auto from_json(const Json &j) -> std::string { return j.as_or<std::string>(""); }
};

/// @brief 自由 JSON 属性（值本身即 JSON 片段）：原样进出，默认显示串即其序列化结果。
template <>
struct PropCodec<Json> {
    /// @brief schema 的 C++ 类型名。
    /// @return `Json`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "Json"; }

    /// @brief schema 的 JSON 类型名。
    /// @return `object`。
    [[nodiscard]] static constexpr auto json_text() -> std::string_view { return "object"; }

    /// @brief 值 → JSON（深拷贝，与其余属性一致的全值语义）。
    /// @param v JSON 值。
    /// @return 同值的 JSON 值。
    [[nodiscard]] static auto to_json(const Json &v) -> Json { return v; }

    /// @brief JSON → 值。
    /// @param j JSON 值。
    /// @return 原值。
    [[nodiscard]] static auto from_json(const Json &j) -> Json { return j; }
};

/// @brief 颜色属性：JSON `[r,g,b,a]`。
template <>
struct PropCodec<Color> {
    /// @brief schema 的 C++ 类型名。
    /// @return `Color`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "Color"; }

    /// @brief schema 的 JSON 类型名。
    /// @return `array`。
    [[nodiscard]] static constexpr auto json_text() -> std::string_view { return "array"; }

    /// @brief 值 → JSON。
    /// @param v 颜色。
    /// @return 四元素整数数组。
    [[nodiscard]] static auto to_json(const Color &v) -> Json { return color_to_json(v); }

    /// @brief JSON → 值。
    /// @param j 四元整数数组。
    /// @return 解析出的颜色（形态不符取黑色）。
    [[nodiscard]] static auto from_json(const Json &j) -> Color { return json_to_color(j); }

    /// @brief JSON → 值并按颜色形态校验（须为 `[r,g,b,a]` 且分量在 0-255）。
    /// @param j JSON 值。
    /// @param desc 该行描述符（颜色校验不看它）。
    /// @return 校验结果。
    [[nodiscard]] static auto validate(const Json &j, const PropDescriptor &desc) -> Result<Color> {
        return validate_prop<Color>(j, desc);
    }
};

/// @brief 四边内边距属性：JSON `{left,top,right,bottom}` 对象。
template <>
struct PropCodec<EdgeInsets> {
    /// @brief schema 的 C++ 类型名。
    /// @return `EdgeInsets`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "EdgeInsets"; }

    /// @brief schema 的 JSON 类型名。
    /// @return `object`。
    [[nodiscard]] static constexpr auto json_text() -> std::string_view { return "object"; }

    /// @brief 值 → JSON。
    /// @param v 内边距。
    /// @return 四键对象。
    [[nodiscard]] static auto to_json(const EdgeInsets &v) -> Json { return edge_insets_to_json(v); }

    /// @brief JSON → 值。
    /// @param j 四键对象。
    /// @return 解析出的内边距（缺字段取 0）。
    [[nodiscard]] static auto from_json(const Json &j) -> EdgeInsets { return json_to_edge_insets(j); }

    /// @brief JSON → 值并按对象形态校验（须为对象且各字段为数字）。
    /// @param j JSON 值。
    /// @param desc 该行描述符（内边距校验不看它）。
    /// @return 校验结果。
    [[nodiscard]] static auto validate(const Json &j, const PropDescriptor &desc) -> Result<EdgeInsets> {
        return validate_prop<EdgeInsets>(j, desc);
    }
};

/// @brief 强类型尺寸意图属性：`"auto"` / `"fill"` / `["px"|"percent", v]`。
template <>
struct PropCodec<Length> {
    /// @brief schema 的 C++ 类型名。
    /// @return `Length`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "Length"; }

    /// @brief schema 的 JSON 类型名。
    /// @return `array`。
    [[nodiscard]] static constexpr auto json_text() -> std::string_view { return "array"; }

    /// @brief 值 → JSON。
    /// @param v 尺寸意图。
    /// @return 编码后的 JSON 值。
    [[nodiscard]] static auto to_json(const Length &v) -> Json { return length_to_json(v); }

    /// @brief JSON → 值。
    /// @param j 编码值。
    /// @return 解析出的尺寸意图（无法识别一律按内容自适应）。
    [[nodiscard]] static auto from_json(const Json &j) -> Length { return json_to_length(j); }

    /// @brief 显示串：字符串形态直接给内容（`auto` / `fill`），数组形态给序列化文本。
    /// @param v 尺寸意图。
    /// @return 显示串。
    [[nodiscard]] static auto display(const Length &v) -> std::string {
        const Json encoded = length_to_json(v);
        if (encoded.is_string()) {
            return encoded.as_or<std::string>("auto");
        }
        auto dumped = json::dump(encoded);
        return dumped ? std::move(dumped.value()) : std::string{"auto"};
    }

    /// @brief JSON → 值并按尺寸意图形态校验（须为已知串或二元数组）。
    /// @param j JSON 值。
    /// @param desc 该行描述符（长度校验不看它）。
    /// @return 校验结果。
    [[nodiscard]] static auto validate(const Json &j, const PropDescriptor &desc) -> Result<Length> {
        return validate_prop<Length>(j, desc);
    }
};

/// @brief 可本地化文本属性：JSON 为纯串或 `$msg` 对象。
template <>
struct PropCodec<LocalizedString> {
    /// @brief schema 的 C++ 类型名。
    /// @return `LocalizedString`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "LocalizedString"; }

    /// @brief schema 的 JSON 类型名。
    /// @return `string|$msg`。
    [[nodiscard]] static constexpr auto json_text() -> std::string_view { return "string|$msg"; }

    /// @brief 值 → JSON。
    /// @param v 可本地化文本。
    /// @return 编码后的 JSON 值。
    [[nodiscard]] static auto to_json(const LocalizedString &v) -> Json { return localizable_to_json(v); }

    /// @brief JSON → 值。
    /// @param j 编码值。
    /// @return 解析出的文本（形态不符取空文本）。
    [[nodiscard]] static auto from_json(const Json &j) -> LocalizedString { return localizable_from_json(j); }

    /// @brief JSON → 值并按形态校验（须为纯串或 `$msg` 对象）。
    /// @param j JSON 值。
    /// @param desc 该行描述符。
    /// @return 校验结果。
    [[nodiscard]] static auto validate(const Json &j, const PropDescriptor &desc) -> Result<LocalizedString> {
        return validate_prop<LocalizedString>(j, desc);
    }
};

namespace detail {

/// @brief 共享枚举属性的编解码公共体（`props_io.h` 里那对 `<name>_to_json` / `json_to_<name>`）。
/// @tparam T 枚举类型。
/// @tparam ToJsonFn 该枚举转 JSON 的函数。
/// @tparam FromJsonFn 该枚举读 JSON 的函数。
template <typename T, auto ToJsonFn, auto FromJsonFn>
struct EnumPropCodec {
    /// @brief schema 的 JSON 类型名。
    /// @return `string`。
    [[nodiscard]] static constexpr auto json_text() -> std::string_view { return "string"; }

    /// @brief 值 → JSON（与枚举同名的串）。
    /// @param v 枚举值。
    /// @return 字符串 JSON 值。
    [[nodiscard]] static auto to_json(const T &v) -> Json { return ToJsonFn(v); }

    /// @brief JSON → 值。
    /// @param j 字符串 JSON 值。
    /// @return 解析出的枚举值（无法识别取该枚举的默认档）。
    [[nodiscard]] static auto from_json(const Json &j) -> T { return FromJsonFn(j); }
};

}  // namespace detail

// ---- 共享枚举：逐类型登记一次；新增枚举在自己的转换器旁登记，机制层零改动 ----

/// @brief 文本对齐方式属性。
template <>
struct PropCodec<TextAlign> : detail::EnumPropCodec<TextAlign, &text_align_to_json, &json_to_text_align> {
    /// @brief schema 的 C++ 类型名。
    /// @return `TextAlign`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "TextAlign"; }
};

/// @brief 文本方向属性。
template <>
struct PropCodec<TextDirection>
    : detail::EnumPropCodec<TextDirection, &text_direction_to_json, &json_to_text_direction> {
    /// @brief schema 的 C++ 类型名。
    /// @return `TextDirection`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "TextDirection"; }
};

/// @brief 文本溢出策略属性。
template <>
struct PropCodec<TextOverflow> : detail::EnumPropCodec<TextOverflow, &text_overflow_to_json, &json_to_text_overflow> {
    /// @brief schema 的 C++ 类型名。
    /// @return `TextOverflow`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "TextOverflow"; }
};

/// @brief 字重属性。
template <>
struct PropCodec<FontWeight> : detail::EnumPropCodec<FontWeight, &font_weight_to_json, &json_to_font_weight> {
    /// @brief schema 的 C++ 类型名。
    /// @return `FontWeight`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "FontWeight"; }
};

/// @brief 字形属性。
template <>
struct PropCodec<FontStyle> : detail::EnumPropCodec<FontStyle, &font_style_to_json, &json_to_font_style> {
    /// @brief schema 的 C++ 类型名。
    /// @return `FontStyle`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "FontStyle"; }
};

/// @brief 文本装饰属性。
template <>
struct PropCodec<TextDecoration>
    : detail::EnumPropCodec<TextDecoration, &text_decoration_to_json, &json_to_text_decoration> {
    /// @brief schema 的 C++ 类型名。
    /// @return `TextDecoration`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "TextDecoration"; }
};

/// @brief 主轴尺寸属性。
template <>
struct PropCodec<MainAxisSize> : detail::EnumPropCodec<MainAxisSize, &main_axis_size_to_json, &json_to_main_axis_size> {
    /// @brief schema 的 C++ 类型名。
    /// @return `MainAxisSize`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "MainAxisSize"; }
};

/// @brief 主轴对齐属性。
template <>
struct PropCodec<MainAxisAlignment>
    : detail::EnumPropCodec<MainAxisAlignment, &main_axis_alignment_to_json, &json_to_main_axis_alignment> {
    /// @brief schema 的 C++ 类型名。
    /// @return `MainAxisAlignment`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "MainAxisAlignment"; }
};

/// @brief 交叉轴对齐属性。
template <>
struct PropCodec<CrossAxisAlignment>
    : detail::EnumPropCodec<CrossAxisAlignment, &cross_axis_alignment_to_json, &json_to_cross_axis_alignment> {
    /// @brief schema 的 C++ 类型名。
    /// @return `CrossAxisAlignment`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "CrossAxisAlignment"; }
};

/// @brief 滚动吸附对齐属性。
template <>
struct PropCodec<ScrollSnapAlignment>
    : detail::EnumPropCodec<ScrollSnapAlignment, &snap_alignment_to_json, &json_to_snap_alignment> {
    /// @brief schema 的 C++ 类型名。
    /// @return `ScrollSnapAlignment`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "ScrollSnapAlignment"; }
};

/// @brief 堆叠适配属性。
template <>
struct PropCodec<StackFit> : detail::EnumPropCodec<StackFit, &stack_fit_to_json, &json_to_stack_fit> {
    /// @brief schema 的 C++ 类型名。
    /// @return `StackFit`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "StackFit"; }
};

/// @brief 图片填充方式属性。
template <>
struct PropCodec<BoxFit> : detail::EnumPropCodec<BoxFit, &box_fit_to_json, &json_to_box_fit> {
    /// @brief schema 的 C++ 类型名。
    /// @return `BoxFit`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "BoxFit"; }
};

/// @brief 溢出策略属性。
template <>
struct PropCodec<OverflowStrategy>
    : detail::EnumPropCodec<OverflowStrategy, &overflow_strategy_to_json, &json_to_overflow_strategy> {
    /// @brief schema 的 C++ 类型名。
    /// @return `OverflowStrategy`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "OverflowStrategy"; }
};

// ============================================================================
// 5. 宿主链接位：标脏与重订阅的唯一通道，只存在于库内
// ============================================================================

namespace detail {

/// @brief 属性面与宿主控件之间的链接位。
///
/// 两处依赖：标脏要经宿主的 `mark_needs_layout()` / `mark_needs_paint()`（公开入口）；换源后的重订阅
/// 要经宿主的 `resubscribe_signals()`（受保护）。后者是本结构存在的全部理由 —— 用 `std::function`
/// 转调会给每只控件加一次堆分配，把那个受保护成员放宽给派生类又会让任何子类绕过换源契约，所以取
/// friend：不扩公共面、不放大可见性、零分配。
///
/// `host == nullptr` 即脱离宿主的构造期 DTO：写入只落字段，不产生任何标脏（与现状链式 DTO 一致）。
///
/// 宿主侧另有一枚对称的槽（`Widget` 的基类 `detail::PropsHost::props_link_`），两者**互指**才叫接上，
/// 故下面四件接线件各管一次互指或解指、且两端析构时互解：整控件移动由 `PropsHost` 的移动面先把
/// `host` 指向新控件，属性子对象的移动面再用 `adopt()` 把自己登记为新宿主的槽值。
struct PropsLink {
    Widget *host = nullptr;  ///< 宿主控件（未接线为空）

    /// @brief 接线：把本链接位与宿主互指（控件构造尾部 `bind` 的唯一调用点）。
    /// @param new_host 承载本属性对象的控件（须与其生命周期一致）。
    auto attach(Widget &new_host) -> void {
        host = &new_host;
        new_host.props_link_ = this;
    }

    /// @brief 整控件移动 / 移动赋值后自登记：宿主的槽认回本链接位。
    ///
    /// 调用点是属性子对象的移动面，此刻宿主指针已由 `PropsHost::take_over` 指到目标控件（基类的移动
    /// 面先于兄弟基类执行），所以这里只补另一半：把目标控件的槽从「来源的链接位」换成「本链接位」。
    auto adopt() -> void {
        if (host != nullptr) {
            host->props_link_ = this;
        }
    }

    /// @brief 脱钩（属性侧）：属性对象析构或被移出宿主时调用；宿主的槽只在确实指向本链接位时才清零。
    ///
    /// 「确实指向」这一判据挡住的是切出宿主搬移属性对象之后的旧宿主：那时它的槽已被本链接位认走，
    /// 若这里再清一次就会清掉别人（本链接位的新位置）的接线。
    /// @note 宿主槽的清零走 `PropsHost::take_over` 一侧的判据是「链接位仍绑在让出的那只控件上」；
    ///       两枚判据分别站在链的两端，合起来保证一条链接位同时只被一只控件引用。
    auto detach() -> void {
        if (host != nullptr) {
            if (host->props_link_ == this) {
                host->props_link_ = nullptr;
            }
            host = nullptr;
        }
    }

    /// @brief 脱钩（宿主侧）：宿主析构或移动赋值覆盖旧接线时调用，只清本链接位的宿主指针。
    ///
    /// 与 `detach()` 的分工：本件在链接位这一侧，故不需要「槽是否还指着我」的判据 —— 指向本链接位的
    /// 那只控件正是把它让出来的对象；宿主的槽随后由 `PropsHost::drop()` 自己清零。
    auto drop_host() -> void { host = nullptr; }

    /// @brief 按表行档位标脏；未接线时是空操作。
    /// @param bits 该行的脏档位。
    auto mark(const PropDirtyBits &bits) const -> void {
        if (host == nullptr) {
            return;
        }
        if (bits.layout) {
            host->mark_needs_layout();
        }
        if (bits.paint) {
            host->mark_needs_paint();
        }
    }

    /// @brief 重建宿主的响应式订阅（来源型属性换源后调用；未挂载时框架侧本就是空操作）。
    auto resubscribe() const -> void {
        if (host != nullptr) {
            host->resubscribe_signals();
        }
    }
};

}  // namespace detail

// ============================================================================
// 6. 控件属性基类：CRTP、零虚函数；公共面只有 `clear`
// ============================================================================

/// @brief 控件属性对象的基类：把权威表行落到字段读写、序列化、订阅采集与 schema 发射上。
///
/// 用法（控件侧唯一形态）：
/// @code
///   class ButtonProps : public AbstractProps<ButtonProps> {
///     public:
///       static constexpr auto label_row() {
///           return prop_row<&ButtonProps::label_>("label").paint().required().note("Button text");
///       }
///       static constexpr auto rows() { return make_prop_rows(label_row(), color_row()); }
///       auto set_label(LocalizedString v) -> ButtonProps & { return assign(label_row(), v); }
///       [[nodiscard]] auto label() const -> LocalizedString { return get(label_row()); }
///     private:
///       ReadOnlyState<LocalizedString> label_;  // 必填 ⇒ 裸句柄
///       friend class AbstractProps<ButtonProps>;
///   };
/// @endcode
/// 控件本体只需公有继承该 `Props` 类、把 `collect_signals` / `describe` / `serialize_props` /
/// `deserialize_props` 各一行转调过来，并在构造尾部 `bind(*this)` 一次。
///
/// @tparam Derived 具体属性类（CRTP）。
/// @note Thread: main-thread only
/// @note Side-effects: 写入经宿主链接位标脏并重建订阅
template <typename Derived>
class AbstractProps {
  public:
    /// @brief 撤表态：把该行退回回落点。公共面上唯一的通用件，逐属性零新增名字。
    ///
    /// 必填行的字段不可空 ⇒ `is_required()` 是类型层事实 ⇒ 本重载集里根本没有它，传入即编译期拒绝
    /// （不是运行期判空）。撤表态后序列化不再写该键，生效值重新跟随该行回落点。
    /// @tparam Row 表行类型。
    /// @param row 要撤表态的行。
    /// @return 派生属性对象引用，便于继续配置。
    template <typename Row>
        requires prop_row_optional<Row>
    auto clear(const Row &row) -> Derived & {
        auto &self = static_cast<Derived &>(*this);
        self.*(Row::field()) = std::nullopt;
        if constexpr (Row::is_source()) {
            link_.resubscribe();
        }
        link_.mark(row.dirty);
        return self;
    }

  private:
    // 特殊成员函数一律私有，且只把派生类登记为朋友：本类只作为 CRTP 基类被 `XxxProps` 派生。
    // 公共构造/析构会被判成「能把 `AbstractProps<Xxx>` 当普通模板类直接实例化」；受保护也不过 ——
    // 受保护构造仍允许它被继续当成普通模板类继承下去，所以唯一的干净形态是私有 + 朋友，而派生类
    // 自己的公共构造照常能调用私有的基类构造，析构同理。
    //
    // 移动面的分工见 `detail::PropsHost`：宿主侧先移动（把 `link_.host` 指向新控件），本侧随后
    // `adopt()` 把新控件的槽认回自己，并把来源的 `host` 置空 ⇒ 整控件移动后标脏命中新宿主、来源脱钩。

    /// @brief 默认构造：未接线（构造期 DTO）。
    AbstractProps() = default;
    /// @brief 拷贝构造：基类只带链接位，而拷贝出来的是另一只尚未挂载的对象，故链接不随拷贝走。
    /// @param other 被拷贝的属性对象（其链接位被有意忽略）。
    AbstractProps(const AbstractProps &other) noexcept { (void)other; }
    /// @brief 拷贝赋值：只换派生侧字段值，本对象的宿主链接保持不变。
    /// @param other 右值属性对象（其链接位被有意忽略）。
    /// @return 引用自身。
    auto operator=(const AbstractProps &other) noexcept -> AbstractProps & {
        (void)other;
        return *this;
    }
    /// @brief 移动构造：搬过链接位并自登记为新宿主的槽值，来源此后不再命中任何宿主。
    /// @param other 被移动的属性对象。
    AbstractProps(AbstractProps &&other) noexcept : link_(other.link_) {
        other.link_.host = nullptr;
        link_.adopt();
    }
    /// @brief 移动赋值：先与本对象原宿主解链，再同移动构造（先摘来源、后落位自登记）。
    /// @param other 右值属性对象。
    /// @return 引用自身。
    auto operator=(AbstractProps &&other) noexcept -> AbstractProps & {
        if (this != &other) {
            link_.detach();
            link_.host = other.link_.host;
            other.link_.host = nullptr;
            link_.adopt();
        }
        return *this;
    }
    /// @brief 析构：与宿主解链（属性对象比宿主活得久时，宿主的槽不留悬垂）。
    ~AbstractProps() { link_.detach(); }

    // 唯一获准构造/析构本基类的类型：CRTP 派生类（构造期 DTO 与挂载后的对象都经它）。
    // 写 `friend Derived;` 而不是 `friend class Derived;`：后者在 `Derived` 是模板参数时是非法的
    // （elaborated-type-specifier 不许指模板参数，gcc 报 "using template type parameter after 'class'"）。
    friend Derived;

  protected:
    /// @brief 本控件的权威表：行元组只在此物化一次（常量初始化，落在静态存储），遍历零构造成本。
    ///
    /// 返回类型用推导而不是 `const decltype(Derived::rows()) &`：后者在基类隐式实例化时（即派生类定义
    /// 那一点）就要对 `Derived::rows()` 求类型，而那时 `Derived` 还不完整。推导返回类型把这次求值推迟
    /// 到调用点 —— CRTP 基类里凡触及 `Derived` 成员的类型都必须这样写。
    /// @return 全部表行的只引用。
    [[nodiscard]] static auto row_table() {
        static constexpr auto ROWS = Derived::rows();
        return static_cast<const decltype(ROWS) &>(ROWS);
    }

    // ---- 写面：实参一律裸值；来源型另有句柄重载 ----

    /// @brief 表态写入：判等短路 → 落字段 → 按该行档位标脏（来源型另需重建订阅）。
    /// @tparam Row 表行类型。
    /// @param row 目标行。
    /// @param value 新的裸值。
    /// @return 派生属性对象引用。
    template <typename Row>
    auto assign(const Row &row, const Row::Bare &value) -> Derived & {
        auto &self = static_cast<Derived &>(*this);
        auto &slot = self.*(Row::field());
        using Bare = Row::Bare;
        if constexpr (Row::is_required() && Row::is_source()) {  // 必填来源件：换入自有常量源
            if constexpr (std::equality_comparable<Bare>) {
                if (slot.get() == value) {
                    return self;
                }
            }
            slot = ReadOnlyState<Bare>{value};
            link_.resubscribe();
        } else if constexpr (Row::is_required()) {  // 必填值件：回落点即字段自身
            if constexpr (std::equality_comparable<Bare>) {
                if (slot == value) {
                    return self;
                }
            }
            slot = value;
        } else if constexpr (Row::is_source()) {  // 来源件：值写入装入自有常量源（整只句柄可空）
            if constexpr (std::equality_comparable<Bare>) {
                if (slot.has_value() && slot->get() == value) {
                    return self;
                }
            }
            slot.emplace(value);
            link_.resubscribe();
        } else {  // 值件：可空槽落值
            if constexpr (std::equality_comparable<Bare>) {
                if (slot.has_value() && *slot == value) {
                    return self;
                }
            }
            slot = value;
        }
        link_.mark(row.dirty);
        return self;
    }

    /// @brief 换绑来源：整只句柄替换并重建订阅（响应式换源的那条路，与值写入共用同一档位）。
    /// @tparam Row 表行类型（必须来源型）。
    /// @param row 目标行。
    /// @param source 新的只读句柄来源。
    /// @return 派生属性对象引用。
    template <typename Row>
    auto rebind(const Row &row, ReadOnlyState<typename Row::Bare> source) -> Derived & {
        static_assert(Row::is_source(), "rebind() only applies to handle-shaped rows");
        auto &self = static_cast<Derived &>(*this);
        if constexpr (Row::is_required()) {
            self.*(Row::field()) = std::move(source);
        } else {
            self.*(Row::field()) = std::optional<ReadOnlyState<typename Row::Bare>>{std::move(source)};
        }
        link_.resubscribe();
        link_.mark(row.dirty);
        return self;
    }

    // ---- 读面：每行恰一件，形态由该行回落点决定 ----

    /// @brief 无参读件（必填行）：字段自身就是生效值，没有第二个名字。
    /// @tparam Row 表行类型。
    /// @param row 目标行。
    /// @return 该属性的当前值。
    template <typename Row>
        requires prop_row_required<Row>
    [[nodiscard]] auto get(const Row &row) const -> Row::Bare {
        (void)row;
        const auto &slot = static_cast<const Derived &>(*this).*(Row::field());
        if constexpr (Row::is_source()) {
            return slot.get();
        } else {
            return slot;
        }
    }

    /// @brief 无参读件（回落点不随作用域变的行）：表态值优先，未表态时取该行回落基线。
    /// @tparam Row 表行类型。
    /// @param row 目标行。
    /// @return 该属性的生效值（与任何作用域无关）。
    template <typename Row>
        requires prop_row_optional<Row>
    [[nodiscard]] auto stated(const Row &row) const -> Row::Bare {
        const auto &self = static_cast<const Derived &>(*this);
        const auto &slot = self.*(Row::field());
        if (slot.has_value()) {
            if constexpr (Row::is_source()) {
                return slot->get();
            } else {
                return *slot;
            }
        }
        return row.baseline(self);
    }

    /// @brief 带上下文读件（活令牌档 / 派生档）：当前作用域下的生效值。
    ///
    /// 签名要求上下文，所以「没有上下文却要作用域依赖的生效值」根本写不出来，也不可能把解析结果
    /// 烘进存储 —— 这正是「渲染路径选错读件」在类型上不可表达的依据。
    /// @tparam Row 表行类型。
    /// @param row 目标行。
    /// @param ctx 构建上下文（解析主题与同族他行）。
    /// @return 该属性在当前作用域下的生效值。
    template <typename Row>
    [[nodiscard]] auto resolve(const Row &row, const BuildContext &ctx) const -> Row::Bare {
        const auto &self = static_cast<const Derived &>(*this);
        const auto &slot = self.*(Row::field());
        if constexpr (Row::is_required()) {
            (void)ctx;
            if constexpr (Row::is_source()) {
                return slot.get();
            } else {
                return slot;
            }
        } else if (slot.has_value()) {
            if constexpr (Row::is_source()) {
                return slot->get();
            } else {
                return *slot;
            }
        }
        return row.effective(self, ctx);
    }

    /// @brief 来源行的句柄读件：当前句柄；未表态时给一份承载回落基线的常量源。
    /// @tparam Row 表行类型（必须来源型）。
    /// @param row 目标行。
    /// @return 该属性的来源句柄。
    template <typename Row>
    [[nodiscard]] auto read(const Row &row) const -> ReadOnlyState<typename Row::Bare> {
        static_assert(Row::is_source(), "read() only applies to handle-shaped rows");
        const auto &self = static_cast<const Derived &>(*this);
        const auto &slot = self.*(Row::field());
        if constexpr (Row::is_required()) {
            return slot;
        } else if (slot.has_value()) {
            return *slot;
        }
        return ReadOnlyState<typename Row::Bare>{row.baseline(self)};
    }

    /// @brief 表态位（只在库内消费：序列化与 Inspector 用它区分「用户显式」与「系统默认」）。
    /// @tparam Row 表行类型。
    /// @param row 目标行。
    /// @return 必填行恒为 true；其余行为「槽里有值」。
    template <typename Row>
    [[nodiscard]] auto present(const Row &row) const -> bool {
        (void)row;
        if constexpr (Row::is_required()) {
            return true;
        } else {
            return (static_cast<const Derived &>(*this).*(Row::field())).has_value();
        }
    }

    // ---- 接线面 ----

    /// @brief 接上宿主控件：构造尾部调用一次，此后本属性对象的写入才落标脏与重订阅。
    /// @param host 承载本属性对象的控件（须与其生命周期一致）。
    /// @note 这是双向互指：宿主侧的槽（`detail::PropsHost`）同时记下本链接位，于是控件整体移动时
    ///       两端各自重绑，不需要任何控件手写（判据见验收条款「移动与绑定」）。
    auto bind(Widget &host) -> void { link_.attach(host); }

    // ---- 发射面：条目与顺序全部由表派生 ----

    /// @brief 由权威表发射属性描述符（不含事件 / 子节点策略 / 示例，那些是控件自身的事实）。
    /// @param type_name 控件类型名（如 `"Button"`）。
    /// @return 描述符：`properties` 按 `rows()` 点名顺序排列。
    [[nodiscard]] static auto props_descriptor(std::string_view type_name) -> WidgetDescriptor {
        WidgetDescriptor descriptor;
        descriptor.name = std::string{type_name};
        const auto &rows = row_table();
        detail::for_each_prop_row(rows,
                                  [&descriptor](const auto &row) { descriptor.properties.push_back(prop_entry(row)); });
        return descriptor;
    }

    /// @brief 单行的描述符条目（发射与反序列化约束校验共用同一形状）。
    /// @tparam Row 表行类型。
    /// @param row 目标行。
    /// @return 填好约束与默认值表示的属性条目。
    template <typename Row>
    [[nodiscard]] static auto prop_entry(const Row &row) -> PropDescriptor {
        using Bare = Row::Bare;
        static_assert(prop_codec_registered<Bare>, "property type needs a PropCodec registration");
        PropDescriptor entry;
        entry.name = std::string{row.key};
        entry.type = std::string{PropCodec<Bare>::type_text()};
        entry.json_type =
            row.json_type_text.empty() ? std::string{PropCodec<Bare>::json_text()} : std::string{row.json_type_text};
        entry.required = Row::is_required();
        entry.note = std::string{row.note_text};
        entry.min_value = std::string{row.min_text};
        entry.max_value = std::string{row.max_text};
        entry.enum_values = detail::prop_codec_enum_names<Bare>();
        if (!row.requires_text.empty()) {
            entry.requires_props.push_back(std::string{row.requires_text});
        }
        // 默认值表示：派生档只能给显示串（无实例不可求值），活令牌档给令牌短名（登记表里已带
        // `theme.` 前缀），常量档同时给格式化串与原生值 —— 后者让 schema 里的 `default` 保持原生类型
        // 而不是字符串。
        if (!row.shows_text.empty()) {
            entry.default_value = std::string{row.shows_text};
        } else if (!row.token_name.empty()) {
            entry.default_value = std::string{"\""} + std::string{row.token_name} + "\"";
        } else if (row.const_fn != nullptr) {
            entry.default_value = detail::prop_bare_display<Bare>(row.const_fn());
            entry.default_native = PropCodec<Bare>::to_json(row.const_fn());
        }
        return entry;
    }

    // ---- 序列化面 ----

    /// @brief 序列化：必填行恒写，非必填行只在表态时写（未表态不写键，回落语义不外泄）。
    /// @param out 目标 JSON 对象。
    auto props_serialize(Json &out) const -> void {
        const auto &self = static_cast<const Derived &>(*this);
        const auto &rows = row_table();
        detail::for_each_prop_row(rows, [&self, &out](const auto &row) {
            using Row = std::remove_cvref_t<decltype(row)>;
            if constexpr (Row::is_required()) {
                out.set(row.key, PropCodec<typename Row::Bare>::to_json(self.get(row)));
            } else if (self.present(row)) {
                const auto &slot = self.*(Row::field());
                if constexpr (Row::is_source()) {
                    out.set(row.key, PropCodec<typename Row::Bare>::to_json(slot->get()));
                } else {
                    out.set(row.key, PropCodec<typename Row::Bare>::to_json(*slot));
                }
            }
        });
    }

    /// @brief 反序列化：来源型属性写进**当前源**（不换绑、外部绑定与订阅数都不变），其余形态按值装入。
    ///
    /// 键缺失即保留现值。非法值经该类型的约束校验后**一律不写入**，同时经 `Diagnostics` 上报：可空行若
    /// 已表态则撤表态（生效值因此退回该行回落基线），必填行没有可撤的表态位、保留现值。这里刻意不把
    /// 基线值当作用户意图写进字段 —— 写进去即把系统默认伪装成用户默认，且对活令牌行等于把「随时随档而变」
    /// 钉成「当前档的一枚值」，正是要消灭的那类分叉；必填行也不退到类型默认值，那会让一枚坏键把已经合法
    /// 的现值擦成一枚无意义初值。派生来源不可写，此时退回装入常量源 —— 与现状「换绑常量源」的行为一致。
    /// @param in 源 JSON 对象。
    auto props_deserialize(const Json &in) -> void {
        auto &self = static_cast<Derived &>(*this);
        const auto &rows = row_table();
        detail::for_each_prop_row(rows, [&self, &in](const auto &row) {
            if (!in.contains(row.key)) {
                return;
            }
            using Row = std::remove_cvref_t<decltype(row)>;
            using Bare = Row::Bare;
            const Json *raw = in.at(row.key);
            if (raw == nullptr) {
                return;
            }
            const auto value = detail::prop_bare_checked<Bare>(*raw, prop_entry(row));
            if (!value) {
                // 非法值不写入：可空行若已表态则撤表态（生效值退回该行回落基线），必填行保留现值。
                if constexpr (prop_row_optional<Row>) {
                    if (self.present(row)) {
                        self.clear(row);
                    }
                }
                return;
            }
            if constexpr (Row::is_source()) {
                const auto &slot = self.*(Row::field());
                const bool written = [&slot, &value]() {
                    if constexpr (Row::is_required()) {
                        return detail::HandleAccess::write_through(slot, *value);
                    } else {
                        return slot.has_value() && detail::HandleAccess::write_through(*slot, *value);
                    }
                }();
                if (!written) {
                    self.assign(row, *value);
                }
            } else {
                self.assign(row, *value);
            }
        });
    }

    /// @brief 采集来源型字段进响应式出口（控件的 `collect_signals` 一行转调即可）。
    /// @param out 信号采集出口。
    auto collect_prop_sources(StateSink &out) const -> void {
        const auto &self = static_cast<const Derived &>(*this);
        const auto &rows = row_table();
        detail::for_each_prop_row(rows, [&self, &out](const auto &row) {
            (void)row;
            using Row = std::remove_cvref_t<decltype(row)>;
            if constexpr (Row::is_source()) {
                const auto &slot = self.*(Row::field());
                if constexpr (Row::is_required()) {
                    out.push(slot);
                } else if (slot.has_value()) {
                    out.push(*slot);
                }
            }
        });
    }

  private:
    detail::PropsLink link_;  ///< 宿主链接位：标脏与重订阅的唯一通道（未接线即构造期 DTO）
};

}  // namespace aurora
