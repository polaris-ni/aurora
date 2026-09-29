#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aurora/core/log.h"
#include "aurora/widget/serialization.h"

/// @brief 序列化与代码生成命名空间（需求 #22 / specification/08-tooling.md §2.5）：把控件树 JSON 快照反向生成为可编译的
/// Aurora C++ 构造表达式。
namespace aurora::serialization {

/// @brief 代码生成风格（需求 #22 / specification/08-tooling.md §2.5）。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
enum class CodeStyle : std::uint8_t {
    Fluent,  ///< 扁平/链式：au::Column 直接罗列子项；无属性容器免 Props 包裹
    StepByStep,  ///< 分步赋值：先声明控件变量，再逐属性写入
    DesignatedInit,  ///< 指定初始化器：一律 au::T 配 au::TProps 的成员初始化列表形态
};

/// @brief codegen 内部实现细节命名空间：类型能力判定与属性发射的辅助函数，仅由本文件公开入口调用，不构成稳定 API。
namespace detail {

/// @brief 是否为多子扁平容器（Fluent 风格直接罗列子项，免 Props 包裹）。
/// @param type 序列化 type 名。
/// @return type 为 Column/Row/Stack/Grid/Scroll/Card 之一时为 true。
[[nodiscard]] constexpr auto is_flat_container(std::string_view type) noexcept -> bool {
    constexpr std::array<std::string_view, 6> k_flat = {"Column", "Row", "Stack", "Grid", "Scroll", "Card"};
    return std::ranges::find(k_flat, type) != k_flat.end();
}

/// @brief 把序列化 type 名映射到 C++ 类名（部分控件类名与 type 名不同）。
/// @param type 序列化 type 名。
/// @return 对应 C++ 类名：仅 Image 映射为 ImageView，其余原样返回。
[[nodiscard]] inline auto cpp_class(std::string_view type) -> std::string {
    if (type == "Image") {
        return "ImageView";
    }
    return std::string{type};
}
/// @brief 把序列化 type 名映射到其 Props 聚合类型名。
/// @param type 序列化 type 名。
/// @return 对应 Props 聚合类型名：Image 映射 ImageViewProps，其余拼接 Props 后缀。
[[nodiscard]] inline auto cpp_props(std::string_view type) -> std::string {
    if (type == "Image") {
        return "ImageViewProps";
    }
    return std::string{type} + std::string{"Props"};
}

/// @brief 拥有 `*Props` 属性聚合的控件类型。
///
/// 只有这 17 个类型存在属性聚合（逐个 `struct *Props` 声明核对），指定初始化器也只对它们的
/// **顶层**成员合法；其余类型（`Stack`/`Spacer`/`TabBar`…）没有聚合，臆造 `au::StackProps` 会直接
/// 编译失败，故表达式风格（Fluent / DesignatedInit）对它们只产出 `au::T{…}` 构造形、属性留待
/// StepByStep 的构造后语句（见 specification/08-tooling.md §2.5 的风格能力表）。
///
/// @param type 序列化 type 名。
/// @return 是否声明了对应的 struct *Props 属性聚合。
[[nodiscard]] constexpr auto has_props_aggregate(std::string_view type) noexcept -> bool {
    constexpr std::array<std::string_view, 17> k_aggregates = {
        "BarChart", "BottomNavBar",  "Button", "Column",       "Divider", "Grid",      "Image", "LazyRow",  "LineChart",
        "PieChart", "PullToRefresh", "Row",    "ScatterChart", "Scroll",  "Sparkline", "Text",  "TextInput"};
    return std::ranges::find(k_aggregates, type) != k_aggregates.end();
}

/// @brief 属性键在 C++ 侧的写入落点。
struct PropTarget {
    std::string_view lhs;  ///< 成员路径（可嵌套，如 `font.size_pt`）或链式 setter 名
    bool via_setter = false;  ///< true = 生成 `w.lhs(值)`；false = 生成 `w.lhs = 值`
};

/// @brief 公开继承自身 `*Props` 聚合的类型（16 个，即 `has_props_aggregate` 去掉 `TextInput`）。
///
/// 公开继承意味着「序列化键 == 公有同名成员」这条默认成立，`w.key = value;` 与 `.key = value` 都合法；
/// `TextInput` 是唯一例外：它以 `TextInputProps` 为**构造参数**、把值拷进 protected 字段，对外只给
/// `set_*()` / 链式 setter，故它的每个属性都必须走 `prop_target` 登记的写入路径。
///
/// @param type 序列化 type 名。
/// @return 类型是否公开继承自身 *Props（序列化键名即公开成员名）；TextInput 除外。
[[nodiscard]] constexpr auto publicly_inherits_props(std::string_view type) noexcept -> bool {
    return has_props_aggregate(type) && type != "TextInput";
}

/// @brief 构造签名 bespoke（既不接受通用 `T{ Node… }` 初始化列表，也没有 `*Props` 子节点槽）的类型。
///
/// 这些类型按位置参数构造（`Badge(count, child)`、`Drawer(content, panel, side, width)`、
/// `Splitter(orientation, …)`；`Stack` 的 `alignment` 同样只能进构造形参），或本身是类模板
/// （`Provider<T>`/`Repeater<T>`/`ReorderableList<T>`——`au::Provider{}` 推不出模板实参），
/// `BreakpointBuilder` 甚至不在 `aurora` 命名空间下；`LazyRow` 是虚拟化列表（`children_policy = "virtual"`），
/// 子项按需由 `item_builder` 生成、`LazyRowProps` 里没有 `children` 成员。**没有通用构造入口**，故对它们只生成空构造并
/// **告警丢弃子节点**——硬编一个位置参数实参等于臆造 API。名单逐项由「73 类型 × 3 风格」产物编译
/// 体检暴露（`error: no matching function for call to`），非凭空猜测；能力口径见
/// specification/08-tooling.md §2.5。
///
/// @param type 序列化 type 名。
/// @return 类型是否只有位置参数构造或需模板实参、无通用构造形可发射。
[[nodiscard]] constexpr auto is_bespoke_ctor(std::string_view type) noexcept -> bool {
    constexpr std::array<std::string_view, 21> k_bespoke = {"Badge",
                                                            "BreakpointBuilder",
                                                            "Drawer",
                                                            "ExpansionPanel",
                                                            "Form",
                                                            "FormField",
                                                            "Hero",
                                                            "LazyRow",
                                                            "LocaleProvider",
                                                            "MediaQueryProvider",
                                                            "PageView",
                                                            "Provider",
                                                            "ReorderableList",
                                                            "Repeater",
                                                            "Show",
                                                            "Splitter",
                                                            "TabBar",
                                                            "ThemeProvider",
                                                            "Timer",
                                                            "VideoControls",
                                                            "VideoPlayer"};
    return std::ranges::find(k_bespoke, type) != k_bespoke.end();
}

/// @brief 连「空构造」都不存在的类型：`au::T{}` 本身就是编译错误。
///
/// 三类成因：类模板无默认模板实参（`Provider`/`Repeater`/`ReorderableList`，三个 `*Provider` 是它们的
/// 别名，构造要写出 `Provider<Theme>`）；只有位置参数构造且无默认构造（`Hero`）；不在 `aurora` 命名空间下
/// （`BreakpointBuilder`，它根本不经 `au::` 限定）。生成器无从猜出模板实参或构造实参，**照原类型名发射**
/// （比悄悄替换成别的控件更诚实）并在 stderr 点名——产物须手工改这一行才能编译。
/// 名单逐项由「73 类型 × 3 风格」产物编译体检暴露，非凭空猜测。
///
/// @param type 序列化 type 名。
/// @return 类型是否连默认构造都不存在（au::T 大括号形态编译不过）。
[[nodiscard]] constexpr auto is_unconstructible(std::string_view type) noexcept -> bool {
    constexpr std::array<std::string_view, 8> k_raw = {"BreakpointBuilder",  "Hero",         "LocaleProvider",
                                                       "MediaQueryProvider", "Provider",     "ReorderableList",
                                                       "Repeater",           "ThemeProvider"};
    return std::ranges::find(k_raw, type) != k_raw.end();
}

/// @brief 发射该类型的构造形前的一次性告知：不可通用构造者须留话，勿让读者以为产物可直接编译。
/// @param type 序列化 type 名。
/// @return 类型是否属无通用构造名单；为真时顺带向 stderr 发一条告警。
[[nodiscard]] inline auto note_construction(const std::string &type) -> bool {
    if (!is_unconstructible(type)) {
        return false;
    }
    AURORA_LOG_WARN("codegen", type,
                    " 没有可生成的构造形式（需模板实参/位置参数，或不在 au:: 下），产物中的 au::", type,
                    "{} 须手工改写才可编译");
    return true;
}

/// @brief 基类属性键：由 `Widget::serialize_props` **无条件**写出，值存放在 `Widget` 侧。
///
/// 任何 `*Props` 聚合都没有这些同名成员（宽度/高度是 `width()`/`height()` 链式 setter +
/// protected 字段，可见性是公有 `Reactive<bool> show`，a11y 三键是 `set_*()`），所以它们只能
/// 在构造后写入。`overflow` 是双关键：Text 用同名键承载自己的 `TextOverflow` 聚合成员
/// （`Text::serialize_props` 覆盖了基类写入），其余类型才是基类 `OverflowStrategy`。
/// 同理 `Skeleton` 的 `width`/`height` 是自己的占位尺寸（float，经 `set_size` 一次给出）、
/// `TitleBar` 的 `height` 是自己的标题栏高度（float，经 `set_height`），都不是基类 `Length` 约束。
///
/// @param key 快照属性键名。
/// @param type 序列化 type 名（用于同名键消歧）。
/// @return 是否为只能构造后写入的基类属性键；Text 的 overflow、Skeleton/TitleBar 自有
///         width/height 等同名例外返回 false。
[[nodiscard]] inline auto is_base_key(const std::string &key, const std::string &type) -> bool {
    if (key == "overflow") {
        return type != "Text";
    }
    if (key == "width") {
        return type != "Skeleton";
    }
    if (key == "height") {
        return type != "Skeleton" && type != "TitleBar";
    }
    return key == "show" || key == "accessibility_label" || key == "stable_key" || key == "labelled_by";
}

/// @brief 基类属性的构造后写法（键 → setter 名 / 直接成员）。
/// @note `show` 在 `Dialog`/`Drawer`/`ToastHost`/`ProgressDialog` 上被同名成员函数遮蔽（弹层自己的
///       `show()`），裸写 `w.show = true;` 是「取成员函数地址后赋值」，编译失败；用限定名
///       `w.Widget::show` 指名基类成员才成立。
/// @param key 基类属性键名。
/// @param type 序列化 type 名（遮蔽消歧用）。
/// @return 该键在该类型上的发射落点（链式 setter 名或公开成员路径）。
[[nodiscard]] inline auto base_target(const std::string &key, const std::string &type) -> PropTarget {
    if (key == "show") {
        // 公有 `Reactive<bool> show`，`w.show = false;` 靠隐式转换成立；遮蔽类型改走限定名。
        constexpr std::array<std::string_view, 4> k_show_shadowed = {"Dialog", "Drawer", "ProgressDialog", "ToastHost"};
        const bool shadowed = std::ranges::find(k_show_shadowed, std::string_view{type}) != k_show_shadowed.end();
        return {.lhs = shadowed ? "Widget::show" : "show", .via_setter = false};
    }
    if (key == "width") {
        return {.lhs = "width", .via_setter = true};
    }
    if (key == "height") {
        return {.lhs = "height", .via_setter = true};
    }
    if (key == "overflow") {
        return {.lhs = "overflow_strategy", .via_setter = true};
    }
    if (key == "accessibility_label") {
        return {.lhs = "set_accessibility_label", .via_setter = true};
    }
    if (key == "stable_key") {
        return {.lhs = "set_stable_key", .via_setter = true};
    }
    return {.lhs = "set_labelled_by", .via_setter = true};
}

/// @brief 键名 ≠ 公开成员名、路径嵌套、或只能经 setter 写入的属性差集表。
///
/// 逐条由 `aurora_cli describe <Type>` 的 `default_props` 键集与控件头文件的成员声明比对得出，
/// 并以 `g++ -fsyntax-only` 编译产物验证（未登记者按同名公开成员处理）。典型三类：
/// ① 嵌套聚合（Text 的 `font_size` 落在 `font.size_pt`、Column 的 `main_axis_alignment` 落在
/// `flex.main_axis`）；② 改名（Text 的 `color` 落在 `text_color`）；③ protected 字段 + 公有
/// 链式 setter（TextInput 的一整组样式键）。②可进指定初始化器，①③不可（GCC 的 C++ 指定初始化器
/// 不支持嵌套设计符，protected 字段不可在类外赋值），故只在 StepByStep 出现。
///
/// @return 函数级 static 只读表（键为 type.key 拼接串），首次调用构造、此后只读缓存。
[[nodiscard]] inline auto prop_target_table() -> const std::unordered_map<std::string, PropTarget> & {
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers) 函数级只读表，理由同 enum_type_for_key
    static const std::unordered_map<std::string, PropTarget> M = {
        {"Text.color", {.lhs = "text_color", .via_setter = false}},
        {"Text.font_size", {.lhs = "font.size_pt", .via_setter = false}},
        {"Text.font_weight", {.lhs = "font.weight", .via_setter = false}},
        {"Button.font_size", {.lhs = "font.size_pt", .via_setter = false}},
        {"Column.main_axis_alignment", {.lhs = "flex.main_axis", .via_setter = false}},
        {"Column.cross_axis_alignment", {.lhs = "flex.cross_axis", .via_setter = false}},
        {"Column.main_axis_size", {.lhs = "flex.main_axis_size", .via_setter = false}},
        {"Row.main_axis_alignment", {.lhs = "flex.main_axis", .via_setter = false}},
        {"Row.cross_axis_alignment", {.lhs = "flex.cross_axis", .via_setter = false}},
        {"Row.main_axis_size", {.lhs = "flex.main_axis_size", .via_setter = false}},
        {"Scroll.snap_alignment", {.lhs = "snap.alignment", .via_setter = false}},
        {"Scroll.snap_extent", {.lhs = "snap.extent", .via_setter = false}},
        {"Scroll.snap_paging", {.lhs = "snap.paging", .via_setter = false}},
        {"TextInput.background", {.lhs = "set_background", .via_setter = true}},
        {"TextInput.border_color", {.lhs = "set_border_color", .via_setter = true}},
        {"TextInput.border_width", {.lhs = "set_border_width", .via_setter = true}},
        {"TextInput.corner_radius", {.lhs = "set_corner_radius", .via_setter = true}},
        {"TextInput.cursor_color", {.lhs = "set_cursor_color", .via_setter = true}},
        {"TextInput.enabled", {.lhs = "set_enabled", .via_setter = true}},
        {"TextInput.focused_background", {.lhs = "set_focused_background", .via_setter = true}},
        {"TextInput.focused_border_color", {.lhs = "set_focused_border_color", .via_setter = true}},
        {"TextInput.padding", {.lhs = "set_padding", .via_setter = true}},
        {"TextInput.placeholder_color", {.lhs = "set_placeholder_color", .via_setter = true}},
        {"TextInput.selection_color", {.lhs = "set_selection_color", .via_setter = true}},
        {"TextInput.text_color", {.lhs = "set_text_color", .via_setter = true}},
        // TextInput 不公开继承聚合，其余键同样只有 setter 形公开入口（`font_size` 是无前缀的链式 setter）。
        {"TextInput.value", {.lhs = "set_value", .via_setter = true}},
        {"TextInput.placeholder", {.lhs = "set_placeholder", .via_setter = true}},
        {"TextInput.font_size", {.lhs = "font_size", .via_setter = true}},
        {"TextInput.max_length", {.lhs = "set_max_length", .via_setter = true}},
        {"TextInput.read_only", {.lhs = "set_read_only", .via_setter = true}},
        {"TextInput.obscure_text", {.lhs = "set_obscure_text", .via_setter = true}},
        // Stack 的 `fit` 有公开 setter；`alignment` 只能进构造形参，故落到「未登记 → 告警省略」。
        {"Stack.fit", {.lhs = "set_fit", .via_setter = true}},
        // Skeleton / TitleBar 的样式组：字段是 protected，公开面只有链式 `set_*()`。
        // 宽度/高度两键各自被自己的 float 尺寸占用（见 is_base_key），`Skeleton` 只有成对的
        // `set_size(Size)`，两键无从分别写入，故不登记（告警省略）。
        {"Skeleton.color", {.lhs = "set_color", .via_setter = true}},
        {"Skeleton.highlight", {.lhs = "set_highlight", .via_setter = true}},
        {"Skeleton.duration", {.lhs = "set_duration", .via_setter = true}},
        {"TitleBar.title", {.lhs = "set_title", .via_setter = true}},
        {"TitleBar.subtitle", {.lhs = "set_subtitle", .via_setter = true}},
        {"TitleBar.window_controls", {.lhs = "set_window_controls", .via_setter = true}},
        {"TitleBar.height", {.lhs = "set_height", .via_setter = true}},
    };
    return M;
}

/// @brief 该 (类型, 键) 是否登记了明确的写入路径。
/// @param type 序列化 type 名。
/// @param key 快照属性键名。
/// @return 该 (type, key) 是否在 prop_target_table 登记过。
[[nodiscard]] inline auto has_prop_target(const std::string &type, const std::string &key) -> bool {
    return prop_target_table().contains(type + "." + key);
}

/// @brief 取 (类型, 键) 已登记的写入落点，未登记时回退同名赋值形态。
/// @param type 序列化 type 名。
/// @param key 快照属性键名。
/// @return 登记的 PropTarget；未登记时为键名同名、直接赋值的落点。
[[nodiscard]] inline auto prop_target(const std::string &type, const std::string &key) -> PropTarget {
    const auto it = prop_target_table().find(type + "." + key);
    return it != prop_target_table().end() ? it->second : PropTarget{.lhs = key, .via_setter = false};
}

/// @brief 无公开写入通道的属性键（构造后既不能赋值也没有登记 setter）。
/// @note 逐项由全量类型体检（73 类型 × 3 风格的产物编译矩阵）暴露后登记，非凭空猜测。
///       `Scroll.offset`/`LazyRow.offset` 是**运行态**（滚动位置由交互产生，构造期无内容高度可夹取，
///       控件也没有同名成员）；`Image.image_width`/`image_height` 是解码结果的派生值
///       （`ImageViewProps` 只有 `bitmap`/`source` 两个成员）。硬发赋值只会产出编译不过的语句。
/// @param type 序列化 type 名。
/// @param key 快照属性键名。
/// @return 键是否属已确认无写入通道的集合（运行态/派生值，静默省略）。
[[nodiscard]] inline auto no_write_path(const std::string &type, const std::string &key) -> bool {
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers) 函数级只读表，理由同 enum_type_for_key
    static const std::array<std::string, 4> K_NONE = {"Scroll.offset", "LazyRow.offset", "Image.image_width",
                                                      "Image.image_height"};
    return std::ranges::find(K_NONE, type + "." + key) != K_NONE.end();
}

/// @brief 该键能否出现在指定初始化器里：类型有属性聚合、非基类键、无「已确认无写入通道」的键，
///        且落点是聚合的顶层成员（而非嵌套路径/setter）。
/// @param type 序列化 type 名。
/// @param key 快照属性键名。
/// @return 该键能否进入指定初始化器（TProps 成员初始化列表形态）。
[[nodiscard]] inline auto designatable(const std::string &type, const std::string &key) -> bool {
    if (!has_props_aggregate(type) || is_base_key(key, type) || no_write_path(type, key)) {
        return false;
    }
    const PropTarget target = prop_target(type, key);
    return !target.via_setter && target.lhs.find('.') == std::string_view::npos;
}

/// @brief 该键能否以构造后语句写入。
///
/// 三条合法路径：① 类型公开继承自己的 `*Props`（序列化键即公有同名成员）；② 在 `prop_target`
/// 登记了落点（改名成员 / 嵌套路径 / setter）；③ 基类属性（走 `base_target`，由调用方处理）。
/// 其余类型（`Slider`/`Checkbox`/`Stack`…）的字段是 protected + 访问器，默认同名赋值必然编译失败，
/// 故**跳过并告警**——宁可少写一条属性，也不产出编译不过的语句，更不静默吞掉语义。
/// 已确认「无写入通道」的键（`no_write_path`：运行态/派生值）按文档口径静默省略，不告警。
/// @param type 序列化 type 名。
/// @param key 快照属性键名。
/// @return 能否以构造后语句写入；未登记且非公开继承聚合时返回 false 并向 stderr 告警省略。
[[nodiscard]] inline auto statement_assignable(const std::string &type, const std::string &key) -> bool {
    if (no_write_path(type, key)) {
        return false;
    }
    if (publicly_inherits_props(type) || has_prop_target(type, key)) {
        return true;
    }
    AURORA_LOG_WARN("codegen", type, ".", key,
                    " 没有已登记的公开写入路径（该类型的字段为 protected + 访问器），已省略");
    return false;
}

// ---------- emit_props 辅助：自描述属性表 + 枚举映射 + 值分派 ----------

/// @brief 一个属性在自描述里的事实：声明序、C++ 声明类型、（若为枚举）合法取值名。
struct PropDescriptor {
    std::size_t order{};  ///< 在 `prop_descriptors` 里的下标 = `*Props` 成员声明序
    std::string cpp_type;  ///< C++ 声明类型名（`StackFit`/`BoxFit`/`float`…）
    std::vector<std::string> enum_values;  ///< 枚举取值名；非枚举属性为空
};

/// @brief 该类型只读属性描述表（键 → PropDescriptor），数据源是 `describe_component()` 的 `prop_descriptors`。
///
/// 用它而不是再造一张手写表，因为这三件事只能由控件自己说：
/// ① **声明序**——C++ 指定初始化器必须按聚合成员声明序书写，乱序直接编译失败，而快照 JSON 的键是
/// 字典序；② **声明类型**——同名键在不同控件上是不同枚举（`fit` 在 `Stack` 是 `StackFit`、在
/// `Image` 是 `BoxFit`），只有描述符的 `type` 字段能消歧；③ **取值名**——`enum` 字段列出合法取值，
/// 用于把快照里的大小写变体（`"left"`）规范化回 `DrawerSide::Left`。
/// 未注册类型返回空表，调用方回落到按键名的静态表。
///
/// @param type 序列化 type 名。
/// @return 该类型只读属性描述表；未注册类型为空表。
[[nodiscard]] inline auto descriptors_for(const std::string &type)
    -> const std::unordered_map<std::string, PropDescriptor> & {
    // 运行期只读缓存：类型 → 属性描述表（单线程 UI，见 CodeStyle 的 Thread 注记）。
    // 首次访问某类型时才查注册表；函数级 static 的首建时刻与跨 TU 静态初始化顺序无关
    // （CODING_STANDARDS.md §5.2 的口径差异）。
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static std::unordered_map<std::string, std::unordered_map<std::string, PropDescriptor>> cache{};
    if (const auto found = cache.find(type); found != cache.end()) {
        return found->second;
    }
    std::unordered_map<std::string, PropDescriptor> table{};  // 未命中缓存时新建的描述表（未知类型为空表）
    const Json schema = describe_component(type);  // 未知类型为空对象
    const auto *pd_val = schema.at("prop_descriptors");
    if (pd_val != nullptr && pd_val->is_array()) {
        std::size_t idx = 0;
        for (const auto *d = pd_val->begin(); d != pd_val->end(); ++d) {
            const auto *name_val = d->at("name");
            if (name_val != nullptr && d->contains("name") && name_val->is_string()) {
                PropDescriptor pd{};
                pd.order = idx;
                const auto *type_val = d->at("type");
                if (type_val != nullptr && d->contains("type") && type_val->is_string()) {
                    pd.cpp_type = type_val->as_or<std::string>("");
                }
                const auto *enum_val = d->at("enum");
                if (enum_val != nullptr && d->contains("enum") && enum_val->is_array()) {
                    for (const auto *e = enum_val->begin(); e != enum_val->end(); ++e) {
                        if (e->is_string()) {
                            pd.enum_values.push_back(e->as_or<std::string>(""));
                        }
                    }
                }
                table.emplace(name_val->as_or<std::string>(""), std::move(pd));
            }
            ++idx;
        }
    }
    return cache.emplace(type, std::move(table)).first->second;
}

/// @brief 属性声明序（未登记者排最后，由稳定排序保持 JSON 原序）。
/// @param type 序列化 type 名。
/// @param key 快照属性键名。
/// @return 成员声明序下标；未登记者返回 std::size_t 最大值。
[[nodiscard]] inline auto prop_order(const std::string &type, const std::string &key) -> std::size_t {
    const auto &table = descriptors_for(type);
    const auto it = table.find(key);
    return it != table.end() ? it->second.order : std::numeric_limits<std::size_t>::max();
}

/// @brief 属性的 C++ 声明类型名（`Length`/`StackFit`/`float`…）；自描述未收录该键时返回空串。
/// @param type 序列化 type 名。
/// @param key 快照属性键名。
/// @return C++ 声明类型名；自描述未收录该键时返回空串。
[[nodiscard]] inline auto declared_type(const std::string &type, const std::string &key) -> std::string {
    const auto &table = descriptors_for(type);
    const auto it = table.find(key);
    return it != table.end() ? it->second.cpp_type : std::string{};
}

/// @brief 声明类型是否「数字即字面量」（int/float/double/bool 一族），决定数字取值能否原样发射。
/// @param cpp_type 属性的 C++ 声明类型名。
/// @return 是否属数字类型一族（数字可原样发射）。
[[nodiscard]] inline auto is_numeric_declared_type(const std::string &cpp_type) -> bool {
    constexpr std::array<std::string_view, 8> k_numeric = {"int",  "float",        "double",        "bool",
                                                           "long", "std::int32_t", "std::uint32_t", "std::size_t"};
    return std::ranges::find(k_numeric, std::string_view{cpp_type}) != k_numeric.end();
}

/// @brief 按自描述把字符串取值还原为 `au::枚举类型::取值`；该键不是枚举属性时返回空串。
/// @note 取值名按 `enum` 列表**忽略大小写**规范化（快照可能写 `"left"`，声明是 `Left`）；列表里没有
/// 该取值时保留原样输出，交由类型体检暴露——不猜别的枚举类型，也不静默丢属性。
/// @param type 序列化 type 名。
/// @param key 快照属性键名。
/// @param json_val 快照里的字符串取值。
/// @return au::类型::取值 表达式；该键非枚举属性、或取值名不是合法 C++ 标识符时返回空串。
[[nodiscard]] inline auto descriptor_enum_expression(const std::string &type, const std::string &key,
                                                     const std::string &json_val) -> std::string {
    const auto &table = descriptors_for(type);
    const auto it = table.find(key);
    if (it == table.end() || it->second.enum_values.empty() || it->second.cpp_type.empty()) {
        return {};
    }
    const auto same_ignore_case = [](const std::string &a, const std::string &b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const char x, const char y) {
            return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
        });
    };
    // 取值名必须能当 C++ 标识符用：`FontWeight` 的快照取值是 `"400"` 这类 CSS 数值，
    // 落点 `Font::weight` 本来就是 int，发 `au::FontWeight::400` 是语法错误——交回后续分支处理。
    const auto identifier_shaped = [](const std::string &s) {
        const char first = s.empty() ? '\0' : s.front();
        return std::isalpha(static_cast<unsigned char>(first)) != 0 || first == '_';
    };
    for (const std::string &candidate : it->second.enum_values) {
        if (same_ignore_case(candidate, json_val)) {
            return identifier_shaped(candidate) ? "au::" + it->second.cpp_type + "::" + candidate : std::string{};
        }
    }
    // 列表里没有该取值：保留原样输出（交由类型体检暴露），但同样不发射非标识符形态。
    return identifier_shaped(json_val) ? "au::" + it->second.cpp_type + "::" + json_val : std::string{};
}

/// @brief JSON 属性名 → C++ 枚举类型名（用于 codegen 输出 `au::EnumType::Value`）。
/// @note 键必须同时覆盖「库自描述里真实出现的属性名」（`to_json` 产出即此名，如 Stack 的
/// `alignment`、Text 的 `overflow`、Drawer 的 `side`、ToastHost 的 `position`）与历史
/// 合成名（`text_overflow` / `stack_fit` / `box_fit` / `overflow_strategy`，供手工 JSON 使用）：
/// 缺前者会让真实 UI 树的枚举属性退化成裸字符串，生成的代码编译不过。
/// @note 同名键在不同控件上属于不同枚举（`fit` 在 Stack 上是 `StackFit`、在 Image/VideoPlayer 上是
/// `BoxFit`；`orientation` 在 Divider 上是 `Orientation`、在 Splitter 上是
/// `SplitterOrientation`）。生成时**已知类型**，故按 (type, key) 消歧；类型未知或未登记时
/// 返回空串（保持字符串输出），避免猜错类型发出臆造枚举。
/// @param key 快照属性键名。
/// @param type 序列化 type 名（同名键消歧用）。
/// @return 枚举类型名；未登记返回空串（保持字符串输出）。
[[nodiscard]] inline auto enum_type_for_key(const std::string &key, const std::string &type) -> std::string {
    // 同名键按类型消歧的那几组先走；其余键的映射与类型无关。
    if (key == "fit") {
        if (type == "Stack") {
            return "StackFit";
        }
        if (type == "Image" || type == "VideoPlayer") {
            return "BoxFit";
        }
        return {};
    }
    if (key == "orientation") {
        if (type == "Divider") {
            return "Orientation";
        }
        if (type == "Splitter") {
            return "SplitterOrientation";
        }
        return {};
    }
    if (key == "overflow") {
        // Text 的同名键是自己的 `TextOverflow`，其余类型是基类 `OverflowStrategy`。
        return type == "Text" ? "TextOverflow" : "OverflowStrategy";
    }
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers) 惰性构造的函数内 static 表，跨 TU 初始化顺序无关
    static const std::unordered_map<std::string, std::string> M = {
        {"text_align", "TextAlign"},
        {"text_overflow", "TextOverflow"},
        {"direction", "TextDirection"},  // Text 的真实属性名
        {"decoration", "TextDecoration"},  // Text 的真实属性名（字符串形态；数组形态另处理）
        {"main_axis_alignment", "MainAxisAlignment"},
        {"cross_axis_alignment", "CrossAxisAlignment"},
        {"main_axis_size", "MainAxisSize"},
        {"stack_fit", "StackFit"},
        {"alignment", "Alignment"},  // Stack 的真实属性名
        {"box_fit", "BoxFit"},
        {"overflow_strategy", "OverflowStrategy"},
        {"font_style", "FontStyle"},
        {"snap_alignment", "ScrollSnapAlignment"},  // Scroll 的真实属性名
        {"side", "DrawerSide"},  // Drawer 的真实属性名
        {"position", "ToastPosition"},  // ToastHost 的真实属性名
    };
    const auto it = M.find(key);
    return it != M.end() ? it->second : std::string{};
}

/// @brief 把 PascalCase 枚举值字符串转为 codegen 用的 `au::EnumType::Value` 表达式。
/// @param enum_type 枚举类型名（不含 au:: 前缀）。
/// @param json_val 快照里的取值名。
/// @return au::EnumType::Value 形式的限定枚举表达式。
[[nodiscard]] inline auto emit_enum_value(std::string_view enum_type, std::string_view json_val) -> std::string {
    return "au::" + std::string{enum_type} + std::string{"::"} + std::string{json_val};
}

/// @brief FontWeight 特殊处理：JSON 存为数值字符串 "100".."900"，而落点 `Font::weight` 就是 int。
/// @param value JSON 取值：数字字符串或数字。
/// @return 解析后字重的十进制字符串，无法解析时为 400。
[[nodiscard]] inline auto emit_font_weight(const Json &value) -> std::string {
    int w = 400;
    if (value.is_string()) {
        const std::string s = value.as_or<std::string>("");
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) std::from_chars 需要首尾指针
        std::from_chars(s.data(), s.data() + s.size(), w);
    } else if (value.is_number()) {
        w = value.as_or<std::int32_t>(0);
    }
    return std::to_string(w);
}

/// @brief float → C++ 字面量：带 `f` 后缀且**保证有小数点**。
/// @note `std::ostream << 14.0f` 输出 `14`，直接拼 `f` 会得到 `14f`——那是用户字面量
///       `operator""f`，编译器一律报错；整数值属性（`step`/`threshold`/`font_size` 等）在快照里很常见。
/// @param v 待发射的浮点值。
/// @return 带 f 后缀且含小数点的 C++ 浮点字面量。
[[nodiscard]] inline auto emit_float_literal(float v) -> std::string {
    std::ostringstream ss{};
    ss << v;
    std::string s = ss.str();
    if (s.find_first_of(".eE") == std::string::npos) {
        s += ".0";
    }
    return s + "f";
}

/// @brief TextDecoration 特殊处理：JSON 存储为字符串数组 ["Underline", ...]，按位或组合。
/// @param value JSON 取值：装饰名字符串或字符串数组。
/// @return 按位或组合的 au::TextDecoration 表达式；空数组或非字符串取值为 None。
[[nodiscard]] inline auto emit_text_decoration(const Json &value) -> std::string {
    const auto one = [](const std::string &s) -> std::string {
        if (s == "Underline") {
            return "au::TextDecoration::Underline";
        }
        if (s == "Overline") {
            return "au::TextDecoration::Overline";
        }
        if (s == "LineThrough") {
            return "au::TextDecoration::LineThrough";
        }
        return "au::TextDecoration::None";
    };
    if (value.is_array()) {
        std::string result{};
        for (const auto *item = value.begin(); item != value.end(); ++item) {
            if (!item->is_string()) {
                continue;
            }
            const std::string s = item->as_or<std::string>("");
            if (s == "None") {
                return "au::TextDecoration::None";
            }
            if (!result.empty()) {
                result += " | ";
            }
            result += one(s);
        }
        return result.empty() ? "au::TextDecoration::None" : result;
    }
    if (value.is_string()) {
        return one(value.as_or<std::string>(""));
    }
    return "au::TextDecoration::None";
}

/// @brief 转义字符串中的双引号、反斜杠、换行、回车与制表符等 C++ 特殊字符。
/// @param s 原始字符串。
/// @return 转义后的新字符串。
[[nodiscard]] inline auto escape_cpp_string(std::string_view s) -> std::string {
    std::string out{};
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

/// @brief 智能分派：根据 (类型, 键名) 与 value 的 JSON 类型生成 C++ 表达式。
/// @note 产物一律 `au::` 限定（与 examples/demos 的书写口径一致），使生成的代码在任何命名空间都能编译。
/// @param key 快照属性键名。
/// @param value 快照属性取值。
/// @param type 序列化 type 名。
/// @return C++ 值表达式；无法还原时告警并返回 /* unknown */ 占位（调用方据此省略发射）。
[[nodiscard]] inline auto emit_prop_value(const std::string &key, const Json &value, const std::string &type)
    -> std::string {
    // --- 枚举属性（string → au::EnumType::Value）---
    if (value.is_string()) {
        // 自描述优先：描述符带枚举取值集时，类型名与取值名都是控件自己声明的，无需按键名猜。
        std::string declared = descriptor_enum_expression(type, key, value.as_or<std::string>(""));
        if (!declared.empty()) {
            return declared;
        }
        const std::string enum_type = enum_type_for_key(key, type);
        if (!enum_type.empty()) {
            return emit_enum_value(enum_type, value.as_or<std::string>(""));
        }
        // FontWeight: 数值字符串 → 数值字面量（落点 `Font::weight` 是 int）
        if (key == "font_weight") {
            return emit_font_weight(value);
        }
        // Length 特殊字符串
        if (value.as_or<std::string>("") == "auto") {
            return "au::auto_length()";
        }
        if (value.as_or<std::string>("") == "fill") {
            return "au::fill()";
        }
        // 普通字符串
        return "\"" + escape_cpp_string(value.as_or<std::string>("")) + "\"";
    }
    // --- bool ---
    if (value.is_bool()) {
        return value.as_or<bool>(false) ? "true" : "false";
    }
    // --- number ---
    if (value.is_number()) {
        // Length 属性以裸数字承载（`describe` 的 `default_props` 就这么写）：`240.0f` 撞不进
        // 不可隐式转换的 `Length` 构造，按 px 语义包装才成立。
        const std::string cpp_type = declared_type(type, key);
        if (cpp_type == "Length") {
            return std::string{"au::px("} + emit_float_literal(value.as_or<float>(0.0F)) + std::string{")"};
        }
        // 枚举属性遇数字取值（`fit: 2`）：自描述没给取值名，按序号猜枚举项等于臆造 API，
        // 只能告警省略——数字既不是该类型的字面量，也没有名可发。
        if (!cpp_type.empty() && !is_numeric_declared_type(cpp_type)) {
            AURORA_LOG_WARN("codegen", type, ".", key, " 是枚举属性（", cpp_type,
                            "）却给了数字取值，无法还原为枚举表达式，已省略");
            return "/* unknown */";
        }
        if (value.is_double()) {
            return emit_float_literal(value.as_or<float>(0.0F));
        }
        return std::to_string(value.as_or<std::int32_t>(0));
    }
    // --- array: Length ["px"/"percent", N] | Color [r,g,b,a] | TextDecoration [...] ---
    if (value.is_array()) {
        if (value.size() == 2 && value.at(0)->is_string()) {
            const std::string unit = value.at(0)->as_or<std::string>("");
            if (unit == "px") {
                return std::string{"au::px("} + emit_float_literal(value.at(1)->as_or<float>(0.0F)) + std::string{")"};
            }
            if (unit == "percent") {
                return std::string{"au::percent("} + emit_float_literal(value.at(1)->as_or<float>(0.0F)) + std::string{")"};
            }
        }
        if (value.size() >= 4 && value.at(0)->is_number()) {
            return std::string{"au::Color{"} + std::to_string(value.at(0)->as_or<std::int32_t>(0)) + std::string{","} +
                   std::to_string(value.at(1)->as_or<std::int32_t>(0)) + std::string{","} + std::to_string(value.at(2)->as_or<std::int32_t>(0)) +
                   std::string{","} + std::to_string(value.at(3)->as_or<std::int32_t>(0)) + std::string{"}"};
        }
        if (key == "text_decoration" || key == "decoration") {
            return emit_text_decoration(value);
        }
    }
    // --- object: EdgeInsets {top,right,bottom,left} | legacy Length {value,unit} ---
    if (value.is_object()) {
        if (value.contains("top") && value.contains("left") && value.contains("right") && value.contains("bottom")) {
            return std::string{"au::EdgeInsets{"} + emit_float_literal(value.at("top")->as_or<float>(0.0F)) + std::string{","} +
                   emit_float_literal(value.at("right")->as_or<float>(0.0F)) + std::string{","} +
                   emit_float_literal(value.at("bottom")->as_or<float>(0.0F)) + std::string{","} +
                   emit_float_literal(value.at("left")->as_or<float>(0.0F)) + std::string{"}"};
        }
        if (value.contains("value") && value.contains("unit")) {
            const auto unit = value.at("unit")->as_or<std::string>("");
            const auto v = value.at("value")->as_or<float>(0.0F);
            if (unit == "pct") {
                return std::string{"au::percent("} + emit_float_literal(v) + std::string{")"};
            }
            return std::string{"au::px("} + emit_float_literal(v) + std::string{")"};
        }
    }
    // --- fallback ---
    // 静默丢属性是「生成的树看着完整、实际少一项」的故障，必须在 stderr 留话（调用方据此跳过发射）。
    AURORA_LOG_WARN("codegen", type, ".", key, " 的取值形态无法还原为 C++ 表达式，已省略: ", json::dump(value).unwrap());
    return "/* unknown */";
}

/// @brief 一条已决定的属性发射：落点 + 值表达式 + 写入方式。
struct PropEmit {
    std::string key;  ///< 快照里的属性键（排序用）
    std::string lhs;  ///< 成员路径或 setter 名（已按类型换算，不含前缀）
    std::string expr;  ///< 值表达式
    bool via_setter{};  ///< true = `lhs(expr)`；false = `lhs = expr`
};

/// @brief 属性键 → 发射序列（按 `*Props` 的**成员声明序**，见 `descriptors_for`）。
/// 无法还原的取值（`/* unknown */`）静默跳过。指定初始化器必须按成员声明序书写（乱序 GCC
/// 直接报错），而快照 JSON 的键是字典序；StepByStep 的语句顺序无此约束，
/// 但同一顺序让两种风格的键序可读且可比。
/// @param type 序列化 type 名。
/// @param props 快照属性对象。
/// @param use_statement true = 构造后语句口径（含基类属性）；false = 指定初始化器口径（仅可设计成员）。
/// @return 按 *Props 成员声明序排序的 PropEmit 列表。
[[nodiscard]] inline auto emit_prop_targets(const std::string &type, const Json &props, bool use_statement)
    -> std::vector<PropEmit> {
    std::vector<PropEmit> out{};  // 发射序列累加容器（过滤后按成员声明序稳定排序）
    for (const auto &e : props.entries()) {
        const std::string key = std::string(e.key);
        std::string expr = emit_prop_value(key, e.value, type);
        if (expr == "/* unknown */") {
            continue;
        }
        const bool base = is_base_key(key, type);
        if (base) {
            if (!use_statement) {
                continue;  // 基类属性没有 `*Props` 成员，指定初始化器无从安放
            }
            const PropTarget target = base_target(key, type);
            out.push_back(
                {.key = key, .lhs = std::string{target.lhs}, .expr = std::move(expr), .via_setter = target.via_setter});
            continue;
        }
        if (use_statement ? !statement_assignable(type, key) : !designatable(type, key)) {
            continue;
        }
        const PropTarget target = prop_target(type, key);
        out.push_back(
            {.key = key, .lhs = std::string{target.lhs}, .expr = std::move(expr), .via_setter = target.via_setter});
    }
    std::stable_sort(
        out.begin(), out.end(),
        [&](const PropEmit &a, const PropEmit &b) {  // 按 *Props 成员声明序稳定排序，未登记键保持 JSON 原序
            return prop_order(type, a.key) < prop_order(type, b.key);
        });
    return out;
}

/// @brief 把可用属性渲染为 `.成员 = 值` 的逗号分隔片段（指定初始化器口径）。
/// @param props 快照属性对象。
/// @param prefix 每个成员路径的前缀（当前传 `.`，为嵌套设计符预留）。
/// @param type 序列化 type 名。
/// @return 片段串；无可用属性时为空串。
[[nodiscard]] inline auto emit_props(const Json &props, const char *prefix, const std::string &type) -> std::string {
    std::string out{};
    for (const auto &emit : emit_prop_targets(type, props, false)) {
        if (!out.empty()) {
            out += ", ";
        }
        out += prefix;
        out += emit.lhs;
        out += " = ";
        out += emit.expr;
    }
    return out;
}

/// @brief 子节点在其 `*Props` 聚合里的成员名：`Column`/`Row`/`Grid` 为 `children`、`Scroll`/
///        `PullToRefresh` 为 `child`；其余类型返回空串（无此成员，调用方改走初始化列表形态）。
/// @note 子节点只能在**构造期**交出去：`Column(ColumnProps)` 把 `props.children` 搬进
///       `Container::children_`，构造后再给继承来的 `children` 成员赋值是静默 no-op（树看着对、画不出来）。
/// @param type 序列化 type 名。
/// @return 子节点成员名（children/child）；其余类型返回空串（无此成员，调用方改走初始化列表形态）。
[[nodiscard]] constexpr auto props_child_slot(std::string_view type) noexcept -> std::string_view {
    if (type == "Column" || type == "Row" || type == "Grid") {
        return "children";
    }
    if (type == "Scroll" || type == "PullToRefresh") {
        return "child";
    }
    return {};
}

/// @brief 该类型能否在构造期交出子节点：不能则告警并返回空列表。
/// @note 子节点没有「构造后统一入口」——`Container::children_` 是 protected，公开面上只有构造形参
/// （`T{ Node… }` 初始化列表或 `*Props` 的 `children`/`child` 槽）。故 bespoke 构造的类型只能
/// 省略子节点，并**必须在 stderr 留话**：生成的树看着完整、实际少一层，是比编译失败更坏的故障。
/// @param type 序列化 type 名。
/// @param kids 已递归生成的子项表达式列表。
/// @return 可发射的子项列表；bespoke 构造类型返回空列表并告警。
[[nodiscard]] inline auto accept_children(const std::string &type, const std::vector<std::string> &kids)
    -> std::vector<std::string> {
    if (kids.empty() || !is_bespoke_ctor(type)) {
        return kids;
    }
    AURORA_LOG_WARN("codegen", type, " 没有通用构造入口（位置参数构造或需模板实参），已省略 ", kids.size(),
                    " 个子节点");
    return {};
}

/// @brief 子表达式列表排版：`{\n<内层一级>e1,\n<内层一级>e2\n<本层>}`。
/// @param kids 已生成的子项表达式列表。
/// @param indent 缩进层级（一级 4 空格）。
/// @return 多行大括号包裹的子项列表片段。
[[nodiscard]] inline auto emit_child_list(const std::vector<std::string> &kids, int indent) -> std::string {
    const std::string pad(static_cast<std::size_t>(indent) * 4, ' ');
    const std::string inner = pad + std::string{"    "};
    std::ostringstream os{};
    os << "{\n";
    for (std::size_t i = 0; i < kids.size(); ++i) {
        os << inner << kids[i];
        if (i + 1 < kids.size()) {
            os << ',';
        }
        os << '\n';
    }
    return os.str() + pad + "}";
}

/// @brief 生成一个节点的构造表达式（Fluent / DesignatedInit 共用）。
///
/// 形态选择（每条都必须是可编译的真实构造）：
/// 1. 无属性聚合的类型（`Stack`/`Spacer`…）→ `au::T{}` 或初始化列表形 `au::T{ 子项 }`；属性无处安放
/// （见 `has_props_aggregate` 的说明与 specification/08-tooling.md §2.5）。
/// 2. 无子节点 → `au::T(au::TProps{ 属性 })`。
/// 3. 子节点成员已登记（`props_child_slot`）且需要带属性，或 DesignatedInit → Props 形：子节点成员排在
/// 前（聚合声明序如此），标量属性随后。
/// 4. 其余扁平容器 → 初始化列表形 `au::T{ 子项 }`（无标量属性时）。
/// 5. 非扁平容器兜底 → `au::T(au::TProps{ 属性, .children = { 子项 } })`。
///
/// @param type          序列化 type 名。
/// @param props_str     emit_props 的标量属性串（形如 `.gap = 8.0f, .columns = 2`）。
/// @param kids_all      已递归生成的子节点表达式（bespoke 构造类型可能被告警丢弃）。
/// @param indent        缩进层级（一级 4 空格）。
/// @param unified_props DesignatedInit 的「容器也一律走 Props 形」开关。
/// @return 该节点的构造表达式。
[[nodiscard]] inline auto emit_node(const std::string &type, const std::string &props_str,
                                    const std::vector<std::string> &kids_all, int indent, bool unified_props)
    -> std::string {
    const std::string cls = cpp_class(type);
    (void)note_construction(type);
    const std::vector<std::string> kids = accept_children(type, kids_all);
    if (!has_props_aggregate(type)) {
        return "au::" + cls + (kids.empty() ? std::string{"{}"} : emit_child_list(kids, indent));
    }
    const std::string pr = cpp_props(type);
    if (kids.empty()) {
        return "au::" + cls + "(au::" + pr + "{" + props_str + "})";
    }
    const std::string_view slot = props_child_slot(type);
    if (!slot.empty() && (unified_props || !props_str.empty())) {
        std::string body{std::string{"."} + std::string{slot}};
        body += (slot == std::string_view{"child"}) ? (" = " + kids.front()) : (" = " + emit_child_list(kids, indent));
        if (!props_str.empty()) {
            body += ", " + props_str;
        }
        return "au::" + cls + "(au::" + pr + "{" + body + "})";
    }
    if (is_flat_container(type)) {
        return "au::" + cls + emit_child_list(kids, indent);
    }
    std::string body = props_str;  // 兜底形态的 Props 成员片段累加器（先标量属性后 .children）
    if (!body.empty()) {
        body += ", ";
    }
    body += ".children = " + emit_child_list(kids, indent);
    return "au::" + cls + "(au::" + pr + "{" + body + "})";
}
}  // namespace detail

/// @brief 递归下降构造表达式生成器（Fluent 与 DesignatedInit 共用；StepByStep 另走 to_code_sb）。
/// @param node 子树快照 JSON。
/// @param style 代码生成风格。
/// @param indent 缩进层级（一级 4 空格）。
/// @return 该子树的构造表达式串。
[[nodiscard]] inline auto to_code_expr(const Json &node, CodeStyle style, int indent) -> std::string {
    const std::string type = node.contains("type") ? node.at("type")->as_or<std::string>("") : "Column";
    const Json &props = node.contains("props") ? *node.at("props") : Json::object();
    std::vector<std::string> kids{};
    const auto *children_val = node.at("children");
    if (children_val != nullptr && children_val->is_array()) {
        for (const auto *kid = children_val->begin(); kid != children_val->end(); ++kid) {
            if (kid->is_object()) {
                kids.push_back(to_code_expr(*kid, style, indent + 2));
            }
        }
    }
    return detail::emit_node(type, detail::emit_props(props, ".", type), kids, indent,
                             style == CodeStyle::DesignatedInit);
}

/// @brief 生成 DesignatedInit 形式（indent 为缩进层级）：容器与叶一律
/// `au::T(au::TProps{ .子节点成员, .属性 = 值 })`——指定初始化器只有 `*Props` 聚合支持，控件类本身是非聚合类型。
/// @param node 子树快照 JSON。
/// @param indent 缩进层级（一级 4 空格）。
/// @return DesignatedInit 风格的构造表达式串。
[[nodiscard]] inline auto to_code_di(const Json &node, int indent) -> std::string {
    return to_code_expr(node, CodeStyle::DesignatedInit, indent);
}

/// @brief 分步赋值形式：`auto __wN = au::T{…};` 后逐属性 `__wN.key = value;`，返回本节点变量名。
///
/// 语句按**后序**落盘：子节点先声明并逐属性配好，再以 `au::Node{std::move(__wN)}` 交进父节点的构造实参
/// （控件不可拷贝，`Widget(const&) = delete`，只能移动交出；交完即不再复用该变量）。
/// 变量编号仍按前序分配（根 = `__w0`）。
///
/// 这是三种风格里唯一**完整**的一种：基类属性（`width`/`height`/`show`/`overflow`/a11y 三键）、
/// 嵌套路径（`font.size_pt`）与 setter-only 属性（TextInput 的样式组）都只能在构造后写入。
///
/// @param node 子树快照 JSON。
/// @param indent 缩进层级（一级 4 空格）。
/// @param os 语句流（后序落盘）。
/// @param counter 变量编号计数器（根分配 `__w0`）。
/// @return 本节点对应的变量名。
[[nodiscard]] inline auto to_code_sb(const Json &node, int indent, std::ostringstream &os, int &counter)
    -> std::string {
    const std::string pad(static_cast<std::size_t>(indent) * 4, ' ');  // 本层行首缩进串（层级 × 4 空格）
    const std::string type = node.contains("type") ? node.at("type")->as_or<std::string>("") : "Column";
    const Json &props = node.contains("props") ? *node.at("props") : Json::object();
    const std::string var = std::string{"__w"} + std::to_string(counter++);
    const std::string cls = detail::cpp_class(type);

    std::vector<std::string> all_kids{};  // 已递归生成的子节点移动表达式（交构造实参用）
    const auto *children_val = node.at("children");
    if (children_val != nullptr && children_val->is_array()) {
        for (const auto *kid = children_val->begin(); kid != children_val->end(); ++kid) {
            if (kid->is_object()) {
                all_kids.push_back("au::Node{std::move(" + to_code_sb(*kid, indent, os, counter) + ")}");
            }
        }
    }

    os << pad << "auto " << var << " = ";
    (void)detail::note_construction(type);
    // bespoke 构造的类型无法在构造期交出子节点（见 accept_children 的告警），此处只落成空构造。
    const std::vector<std::string> kids = detail::accept_children(type, all_kids);
    if (kids.empty()) {
        os << "au::" << cls << "{}";
    } else if (const std::string_view slot = detail::props_child_slot(type);
               detail::has_props_aggregate(type) && !slot.empty()) {  // 有属性聚合且子槽名已登记：走 Props 子槽构造形
        // 子节点只能在构造期传入：构造后再给继承来的 `children` 成员赋值是静默 no-op。
        // 单槽类型（`child`）只取第一个子项，与 `Scroll`/`PullToRefresh` 的聚合声明一致。
        const std::string list =
            slot == std::string_view{"child"} ? kids.front() : detail::emit_child_list(kids, indent);
        os << "au::" << cls << "(au::" << detail::cpp_props(type) << "{." << slot << " = " << list << "})";
    } else {
        os << "au::" << cls << detail::emit_child_list(kids, indent);
    }
    os << ";\n";
    for (const auto &emit : detail::emit_prop_targets(type, props, true)) {
        os << pad << var << '.' << emit.lhs;
        if (emit.via_setter) {
            os << '(' << emit.expr << ");\n";
        } else {
            os << " = " << emit.expr << ";\n";
        }
    }
    return var;
}

/// @brief 把序列化的 widget 树 JSON 反向生成为 Aurora C++ 源码（需求 #22 / specification/08-tooling.md §2.5）。
///
/// 输入为 `to_json(widget)` 产生的结构快照。默认 Fluent 风格（扁平容器直接罗列子项 +
/// `au::Type(au::TypeProps{...})` 叶/带属性容器形式）。
/// 可用 CodeStyle 选择 DesignatedInit（容器也统一走 Props 指定初始化器）或 StepByStep（先声明后逐属性赋值）。
///
/// 属性覆盖度：**StepByStep 完整**；Fluent / DesignatedInit 只能承载 `*Props` 聚合的顶层成员，
/// 基类属性与嵌套/setter-only 属性会被省略（逐类型能力表见 specification/08-tooling.md §2.5）。
///
/// 用于「设计工具 → 代码」工作流，或把快照作为可编辑源码。
///
/// @param node to_json(widget) 的结构快照。
/// @param indent 缩进层级（一级 4 空格）。
/// @return Fluent 风格 C++ 源码串。
[[nodiscard]] inline auto to_code(const Json &node, int indent = 0) -> std::string {
    return to_code_expr(node, CodeStyle::Fluent, indent);
}

/// @brief 按指定风格生成代码（需求 #22）。默认 Fluent，与 to_code(node, int) 行为一致。
/// @param node to_json(widget) 的结构快照。
/// @param style 目标代码风格。
/// @param indent 缩进层级（一级 4 空格）。
/// @return 按风格生成的 C++ 源码串。
[[nodiscard]] inline auto to_code(const Json &node, CodeStyle style, int indent = 0) -> std::string {
    switch (style) {
        case CodeStyle::DesignatedInit:
            return to_code_di(node, indent);
        case CodeStyle::StepByStep: {
            std::ostringstream os{};
            int counter = 0;
            (void)to_code_sb(node, indent, os, counter);
            return os.str();
        }
        case CodeStyle::Fluent:
        default:
            return to_code(node, indent);
    }
}

/// @brief 便捷：直接对 widget 取快照再生成代码。
/// @param w 待生成的控件。
/// @return 控件快照的 C++ 源码串。
[[nodiscard]] inline auto to_code(const Widget &w) -> std::string { return to_code(to_json(w)); }

}  // namespace aurora::serialization
