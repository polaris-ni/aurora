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

/// @brief 序列化与代码生成命名空间（需求 SPEC.FEAT.TOOLING.UI-TO-CODE.001 / specification/08-tooling.md
/// §2.5）：把控件树 JSON 快照反向生成为可编译的 Aurora C++ 构造表达式。
namespace aurora::serialization {

/// @brief 代码生成风格（需求 SPEC.FEAT.TOOLING.UI-TO-CODE.001 / specification/08-tooling.md §2.5）。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
enum class CodeStyle : std::uint8_t {
    Fluent,  ///< 扁平/链式：au::Column 直接罗列子项；无属性容器免 Props 包裹
    StepByStep,  ///< 分步赋值：先声明控件变量，再逐属性写入
    DesignatedInit,  ///< Fluent 别名：指定初始化器不再合法，产物逐字节相同；值为 CLI/MCP/Inspector 的 style 参数保留
};

/// @brief codegen 内部实现细节命名空间：类型能力判定与属性发射的辅助函数，不构成稳定 API。
///
/// 消费端有两处：本文件的公开入口（代码生成），以及 `tools/include/lsp_live_schema.h`（语言服务的
/// schema 桥）。后者同源取 `props_chain_writers` / `is_base_key` / `expressible` / `prop_target` /
/// `cpp_class` / `cpp_props`，使 LSP 补全插出的写件名与生成器发射的落点是同一枚字符串——两份名单
/// 手抄必然漂移，而漂移的表现是「补出来的代码编译不过」。改动这些判定时须同时想到那两个消费端。
namespace detail {

/// @brief 类名之外还需要带一段 `au::` 下的子命名空间时，这段前缀是什么。
///
/// 在册类型里只有 `WebView` 不在 `aurora` 直属（它是 `aurora::webview::WebView`），而属性行自描述的
/// 枚举类型名（`type_text()`）与聚合类名都在同一命名空间下声明，故类名、聚合名与枚举名共用这一枚
/// 前缀；`Color` 这类基座类型名是 `aurora` 直属的，不走这条通路（值发射只处理枚举）。
///
/// @param type 序列化 type 名。
/// @return `au::` 之后的子命名空间段（含尾随 `::`）；`aurora` 直属类型为空串。
[[nodiscard]] constexpr auto cpp_namespace(std::string_view type) noexcept -> std::string_view {
    return type == "WebView" ? std::string_view{"webview::"} : std::string_view{};
}

/// @brief 把序列化 type 名映射到 C++ 类名（部分控件类名与 type 名不同）。
/// @param type 序列化 type 名。
/// @return 对应 C++ 类名（子命名空间类型带 `webview::` 前缀）：仅 Image 映射为 ImageView，其余原样返回。
[[nodiscard]] inline auto cpp_class(std::string_view type) -> std::string {
    const std::string ns{cpp_namespace(type)};
    if (type == "Image") {
        return ns + "ImageView";
    }
    return ns + std::string{type};
}
/// @brief 把序列化 type 名映射到其 Props 聚合类型名。
/// @param type 序列化 type 名。
/// @return 对应 Props 聚合类型名（与类名同一段子命名空间）：Image 映射 ImageViewProps，其余拼接 Props 后缀。
[[nodiscard]] inline auto cpp_props(std::string_view type) -> std::string {
    const std::string ns{cpp_namespace(type)};
    if (type == "Image") {
        return ns + "ImageViewProps";
    }
    return ns + std::string{type} + std::string{"Props"};
}

/// @brief 属性写入面只由 `set_<键名>()` 链式写件承担的控件类型（属性单一声明形态）。
///
/// 这类 `*Props` 类的字段全部私有、公共面只剩逐属性的 `set_<词根>` 写件（词根与序列化键同名），
/// 因此 **指定初始化器不再合法**（`au::ButtonProps{ .label = "OK" }` 编译红：无可直写的公有成员），
/// 表达式风格统一改发链式调用形态 `au::Button(au::ButtonProps().set_label("OK"))`；每迁移一枚控件
/// 就把它的类型名加进这张表，同时删掉 `prop_target_table` 里为该类型登记的手写落点（链式形态下由
/// 词根推出）。
///
/// 这 18 型同时是**发射侧唯一认账的属性聚合**：其余 58 型里，`Stack`/`Spacer`/`TabBar`… 根本没有
/// 聚合（臆造 `au::StackProps` 直接编译失败），按「无聚合」档发 `au::T{…}`、其属性在表达式两档
/// 静默省略（那是该档的既定能力边界）、在 StepByStep 告警省略——少报不误报。表达式风格对这一档类型
/// 只产出 `au::T{…}` 构造形、属性留待 StepByStep 的构造后语句（见 specification/08-tooling.md §2.5
/// 的风格能力表）。「有可装入属性的聚合」与「聚合只可链式装入」在本形态下是同一件事：新聚合只能由
/// 权威表生成，生成即私有字段 + 链式写件，不再有「有聚合却仍可指定初始化」的中间态。这里曾经维护
/// 两份名单，而两份名单逐字相等：靠它们的差区分的分支因此恒不成立，判定面比机制面窄了一档，故收成
/// 一份。曾有的最后一个存量例外是 `WebView`：它有聚合（`WebViewProps`，公有字段 + 控件公开继承）却
/// 不在任何一份发射名单里，属性因此按「无聚合」档整批发不出去——少报的那条通路其实合法。它已迁到
/// 权威表（字段私有 + 逐行链式写件）并加进这张名单，存量例外清零。
///
/// @param type 序列化 type 名。
/// @return 该类型的属性是否只可经 `set_<键名>()` 链式写件装入（= 发射侧认账的属性聚合档）。
[[nodiscard]] constexpr auto props_chain_writers(std::string_view type) noexcept -> bool {
    constexpr std::array<std::string_view, 18> k_chain = {
        "BarChart",     "BottomNavBar", "Button",    "Column",   "Divider",       "Grid",
        "Image",        "LazyRow",      "LineChart", "PieChart", "PullToRefresh", "Row",
        "ScatterChart", "Scroll",       "Sparkline", "Text",     "TextInput",     "WebView"};
    return std::ranges::find(k_chain, type) != k_chain.end();
}

/// @brief 属性键在 C++ 侧的写入落点。
struct PropTarget {
    std::string lhs;  ///< 成员路径（可嵌套）或链式 setter 名
    bool via_setter = false;  ///< true = 生成 `w.lhs(值)`；false = 生成 `w.lhs = 值`
};

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
                    " has no constructible form to emit (needs template/positional arguments, or is not under au::); "
                    "the au::",
                    type, "{} in the output must be rewritten by hand before it compiles");
    return true;
}

/// @brief 基类属性键：由 `Widget::serialize_props` **无条件**写出，值存放在 `Widget` 侧。
///
/// 任何 `*Props` 聚合都没有这些同名成员（宽度/高度是 `width()`/`height()` 链式 setter +
/// protected 字段，可见性是公有 `State<bool> show`，a11y 三键是 `set_*()`），所以它们只能
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
/// @note `show` 自状态模型收口起是只读展示属性：基类只暴露 `show()` 读取与
///       `set_show(...)` 换源入口，生成代码统一走 setter，不再有可直接赋值的同名成员。
/// @param key 基类属性键名。
/// @param type 序列化 type 名（遮蔽消歧用）。
/// @return 该键在该类型上的发射落点（链式 setter 名或公开成员路径）。
[[nodiscard]] inline auto base_target(const std::string &key, const std::string &type) -> PropTarget {
    if (key == "show") {
        // 展示属性只读：统一经换源入口写入，弹层同名 show() 不再造成赋值歧义。
        return {.lhs = "set_show", .via_setter = true};
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
/// 逐条由 `aurora_cli describe <Type>` 的 `props` 表键集与控件头文件的成员声明比对得出，
/// 并以 `g++ -fsyntax-only` 编译产物验证（未登记又非链式形态的键没有公开写入通道，告警省略）。
/// 当前在册的只有 protected 字段 + 公有链式 setter 一类（Stack / Skeleton / TitleBar 等）：
/// 这类落点既不是 `*Props` 成员也不是词根写件，构造表达式无从承载，故只在 StepByStep 出现；
/// 属性面已迁到链式写件的类型不在此表登记——落点由词根 `set_<键>` 推出。
///
/// @return 函数级 static 只读表（键为 type.key 拼接串），首次调用构造、此后只读缓存。
[[nodiscard]] inline auto prop_target_table() -> const std::unordered_map<std::string, PropTarget> & {
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers) 函数级只读表，理由同 enum_type_for_key
    static const std::unordered_map<std::string, PropTarget> M = {
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

/// @brief 取 (类型, 键) 已登记的写入落点。
///
/// 链式形态的类型（`props_chain_writers`）不发赋值：词根与序列化键同名，落点直接由键名推出为
/// `set_<键>`，无需逐键登记（这是属性单一声明形态的读面词根规则在发射侧的兑现）。
/// 其余类型只认 `prop_target_table` 的登记项；表外回退值（同名直接赋值）只是查表默认，两条发射路径
/// 都在取落点之前把它滤掉（表达式问 `expressible`、语句问 `statement_assignable`），当前没有产物走到
/// 这一支——保留是给「登记表新增一档落点」留的默认，不是「同名公有成员可直写」的承诺。
///
/// @param type 序列化 type 名。
/// @param key 快照属性键名。
/// @return 登记的 PropTarget；链式类型为由词根推出的 setter 落点；其余未登记时为键名同名的查表默认。
[[nodiscard]] inline auto prop_target(const std::string &type, const std::string &key) -> PropTarget {
    if (props_chain_writers(type)) {
        return {.lhs = "set_" + key, .via_setter = true};
    }
    const auto it = prop_target_table().find(type + "." + key);
    return it != prop_target_table().end() ? it->second : PropTarget{.lhs = key, .via_setter = false};
}

/// @brief 无公开写入通道的属性键（构造后既不能赋值也没有登记 setter）。
/// @note 逐项由全量类型体检（73 类型 × 3 风格的产物编译矩阵）暴露后登记，非凭空猜测。
///       `Scroll.offset`/`LazyRow.offset` 是**运行态**（滚动位置由交互产生，构造期无内容高度可夹取，
///       控件也没有同名成员）；`Image.image_width`/`image_height` 是解码结果的派生值
///       （它们由位图尺寸现算，属性表上没有对应行，链式形态也就没有 `set_` 写件）。硬发赋值或
///       臆造写件只会产出编译不过的语句。
/// @param type 序列化 type 名。
/// @param key 快照属性键名。
/// @return 键是否属已确认无写入通道的集合（运行态/派生值，静默省略）。
[[nodiscard]] inline auto no_write_path(const std::string &type, const std::string &key) -> bool {
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers) 函数级只读表，理由同 enum_type_for_key
    static const std::array<std::string, 4> K_NONE = {"Scroll.offset", "LazyRow.offset", "Image.image_width",
                                                      "Image.image_height"};
    return std::ranges::find(K_NONE, type + "." + key) != K_NONE.end();
}

/// @brief 该键能否进**构造表达式**（Fluent 与 DesignatedInit 共用这一条判据，两风格同发射路径）。
///
/// 只有一条合法路径：链式形态类型由词根推出的 `set_<key>(值)` 调用——它不是设计符，而是挂在
/// `TProps()` 之后的成员调用。曾经并列的第二条路径「`*Props` 聚合的顶层可设计成员」随属性单一声明
/// 形态收口退出：能进表达式的类型全在 `props_chain_writers` 名单里，其字段私有，指定初始化器编译红。
/// 基类键不在此判（聚合没有同名成员，`emit_prop_targets` 先行分流到 `base_target`）。
/// 链式形态下词根规则会为**任何**键名推出 `set_<key>`，包括运行时键（`LazyRow.offset` 只有
/// `set_scroll_offset()`，没有 `set_offset()`），故发射前先问那一枚键有没有写入通道。
/// @param type 序列化 type 名。
/// @param key 快照属性键名。
/// @return 该键是否可在构造期装入（表达式风格不丢这条属性）。
[[nodiscard]] inline auto expressible(const std::string &type, const std::string &key) -> bool {
    return props_chain_writers(type) && !no_write_path(type, key);
}

/// @brief 该键能否以构造后语句写入。
///
/// 两条合法路径：① 链式形态类型（落点由词根推出为 `set_<key>()`）；② 在 `prop_target` 登记了落点
/// （改名成员 / 嵌套路径 / setter）。基类属性是第三条通道，由调用方先行分流到 `base_target`，不进本判据。
/// 其余类型（`Slider`/`Checkbox`/`Stack`…）的字段是 protected + 访问器，默认同名赋值必然编译失败，
/// 故**跳过并告警**——宁可少写一条属性，也不产出编译不过的语句，更不静默吞掉语义。
/// 已确认「无写入通道」的键（`no_write_path`：运行态/派生值）按文档口径静默省略，不告警。
/// @param type 序列化 type 名。
/// @param key 快照属性键名。
/// @return 能否以构造后语句写入；未登记且非链式形态类型时返回 false 并向 stderr 告警省略。
[[nodiscard]] inline auto statement_assignable(const std::string &type, const std::string &key) -> bool {
    if (no_write_path(type, key)) {
        return false;
    }
    if (props_chain_writers(type) || has_prop_target(type, key)) {
        return true;
    }
    AURORA_LOG_WARN("codegen", type, ".", key,
                    " has no registered public write path (this type keeps its fields protected behind accessors), "
                    "omitted");
    return false;
}

// ---------- emit_props 辅助：自描述属性表 + 枚举映射 + 值分派 ----------

/// @brief 一个属性在自描述里的事实：声明序、C++ 声明类型、（若为枚举）合法取值名。
struct PropDescriptor {
    std::size_t order{};  ///< 在 `props` 表里的下标 = `*Props` 成员声明序
    std::string cpp_type;  ///< C++ 声明类型名（`StackFit`/`BoxFit`/`float`…）
    std::vector<std::string> enum_values;  ///< 枚举取值名；非枚举属性为空
};

/// @brief 该类型只读属性描述表（键 → PropDescriptor），数据源是 `describe_component()` 的 `props` 表。
///
/// 用它而不是再造一张手写表，因为这三件事只能由控件自己说：
/// ① **表序**——`props` 数组下标就是权威表的点名序，发射按它排序，产物才能与 schema、描述符逐行对照
/// （链式写件之间没有顺序约束，这曾是指定初始化器的硬判据：乱序直接编译失败）；② **声明类型**——同名键
/// 在不同控件上是不同枚举（`fit` 在 `Stack` 是 `StackFit`、在 `Image` 是 `BoxFit`），只有描述符的 `type`
/// 字段能消歧；③ **取值名**——`enum` 字段列出合法取值，
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
    const auto *pd_val = schema.at("props");
    if (pd_val != nullptr && pd_val->is_array()) {
        std::size_t idx = 0;
        for (const auto &d : *pd_val) {
            const auto *name_val = d.at("name");
            if (name_val != nullptr && d.contains("name") && name_val->is_string()) {
                PropDescriptor pd{};
                pd.order = idx;
                const auto *type_val = d.at("type");
                if (type_val != nullptr && d.contains("type") && type_val->is_string()) {
                    pd.cpp_type = type_val->as_or<std::string>("");
                }
                const auto *enum_val = d.at("enum");
                if (enum_val != nullptr && d.contains("enum") && enum_val->is_array()) {
                    for (const auto &e : *enum_val) {
                        if (e.is_string()) {
                            pd.enum_values.push_back(e.as_or<std::string>(""));
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
        return std::ranges::equal(a, b, [](const char x, const char y) {
            return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
        });
    };
    // 取值名必须能当 C++ 标识符用：`FontWeight` 的快照取值是 `"400"` 这类 CSS 数值，
    // 落点 `Font::weight` 本来就是 int，发 `au::FontWeight::400` 是语法错误——交回后续分支处理。
    const auto identifier_shaped = [](const std::string &s) {
        const char first = s.empty() ? '\0' : s.front();
        return std::isalpha(static_cast<unsigned char>(first)) != 0 || first == '_';
    };
    const std::string ns{cpp_namespace(type)};
    // 单串拼接走 append：链式 `operator+` 会在每一步产生临时串分配（clang-tidy
    // performance-inefficient-string-concatenation），此处改为一次预留 + 追加。
    // 取值名须为合法 C++ 标识符才发射限定名，否则退回空串交由类型体检暴露。
    const auto emit_typed = [&](const std::string &value) -> std::string {
        if (!identifier_shaped(value)) {
            return std::string{};
        }
        std::string out;
        out.reserve(4 + ns.size() + it->second.cpp_type.size() + 2 + value.size());
        out += "au::";
        out += ns;
        out += it->second.cpp_type;
        out += "::";
        out += value;
        return out;
    };
    for (const std::string &candidate : it->second.enum_values) {
        if (same_ignore_case(candidate, json_val)) {
            return emit_typed(candidate);
        }
    }
    // 列表里没有该取值：保留原样输出（交由类型体检暴露），但同样不发射非标识符形态。
    return emit_typed(json_val);
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
        const auto s = value.as_or<std::string>("");
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
        for (const auto &item : value) {
            if (!item.is_string()) {
                continue;
            }
            const auto s = item.as_or<std::string>("");
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
        // Length 属性以裸数字承载（`describe` 的 `props` 表就这么写）：`240.0f` 撞不进
        // 不可隐式转换的 `Length` 构造，按 px 语义包装才成立。
        const std::string cpp_type = declared_type(type, key);
        if (cpp_type == "Length") {
            return std::string{"au::px("} + emit_float_literal(value.as_or<float>(0.0F)) + std::string{")"};
        }
        // 枚举属性遇数字取值（`fit: 2`）：自描述没给取值名，按序号猜枚举项等于臆造 API，
        // 只能告警省略——数字既不是该类型的字面量，也没有名可发。
        if (!cpp_type.empty() && !is_numeric_declared_type(cpp_type)) {
            AURORA_LOG_WARN("codegen", type, ".", key, " is an enum property (", cpp_type,
                            ") but was given a numeric value; it cannot be restored to an enum expression, omitted");
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
            const auto unit = value.at(0)->as_or<std::string>("");
            if (unit == "px") {
                return std::string{"au::px("} + emit_float_literal(value.at(1)->as_or<float>(0.0F)) + std::string{")"};
            }
            if (unit == "percent") {
                return std::string{"au::percent("} + emit_float_literal(value.at(1)->as_or<float>(0.0F)) +
                       std::string{")"};
            }
        }
        if (value.size() >= 4 && value.at(0)->is_number()) {
            return std::string{"au::Color{"} + std::to_string(value.at(0)->as_or<std::int32_t>(0)) + std::string{","} +
                   std::to_string(value.at(1)->as_or<std::int32_t>(0)) + std::string{","} +
                   std::to_string(value.at(2)->as_or<std::int32_t>(0)) + std::string{","} +
                   std::to_string(value.at(3)->as_or<std::int32_t>(0)) + std::string{"}"};
        }
        if (key == "text_decoration" || key == "decoration") {
            return emit_text_decoration(value);
        }
    }
    // --- object: EdgeInsets {top,right,bottom,left} | legacy Length {value,unit} ---
    if (value.is_object()) {
        if (value.contains("top") && value.contains("left") && value.contains("right") && value.contains("bottom")) {
            return std::string{"au::EdgeInsets{"} + emit_float_literal(value.at("top")->as_or<float>(0.0F)) +
                   std::string{","} + emit_float_literal(value.at("right")->as_or<float>(0.0F)) + std::string{","} +
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
    AURORA_LOG_WARN("codegen", type, ".", key,
                    " value form cannot be restored to a C++ expression, omitted: ", json::dump(value).unwrap());
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
/// 无法还原的取值（`/* unknown */`）静默跳过。链式写件本身没有顺序约束（曾经的指定初始化器有：乱序
/// GCC 直接报错），这里仍按权威表的点名序排序，为的是产物与 schema、描述符能逐行对照；快照 JSON 的键
/// 序无从保证，StepByStep 的语句序无此约束，但同一顺序让两种风格的键序可读且可比。
/// @param type 序列化 type 名。
/// @param props 快照属性对象。
/// @param use_statement true = 构造后语句口径（含基类属性）；false = 构造表达式口径（仅链式写件可装入的键）。
/// @return 按 *Props 成员声明序排序的 PropEmit 列表。
[[nodiscard]] inline auto emit_prop_targets(const std::string &type, const Json &props, bool use_statement)
    -> std::vector<PropEmit> {
    std::vector<PropEmit> out{};  // 发射序列累加容器（过滤后按成员声明序稳定排序）
    for (const auto &e : props.entries()) {
        const auto key = std::string(e.key);
        std::string expr = emit_prop_value(key, e.value, type);
        if (expr == "/* unknown */") {
            continue;
        }
        const bool base = is_base_key(key, type);
        if (base) {
            if (!use_statement) {
                continue;  // 基类属性没有 `*Props` 成员，构造表达式无从安放
            }
            const PropTarget target = base_target(key, type);
            out.push_back(
                {.key = key, .lhs = std::string{target.lhs}, .expr = std::move(expr), .via_setter = target.via_setter});
            continue;
        }
        if (use_statement ? !statement_assignable(type, key) : !expressible(type, key)) {
            continue;
        }
        const PropTarget target = prop_target(type, key);
        out.push_back(
            {.key = key, .lhs = std::string{target.lhs}, .expr = std::move(expr), .via_setter = target.via_setter});
    }
    std::ranges::stable_sort(
        out, [&](const PropEmit &a, const PropEmit &b) {  // 按 *Props 成员声明序稳定排序，未登记键保持 JSON 原序
            return prop_order(type, a.key) < prop_order(type, b.key);
        });
    return out;
}

/// @brief 把可用属性渲染为构造表达式的 Props 形片段：链式形态发 `.set_成员(值)`
///        （无分隔符，直接续在 `TProps()` 之后）。
///
/// 能走到这里的只有链式形态类型：`expressible` 已把其余类型的键全部滤掉，「`.成员 = 值` 进初始化列表」
/// 那一档随属性单一声明形态一并退出。真出现非 setter 落点，说明判定面与发射面又分叉了——按
/// 「宁缺不伪」省略并告警，不发射编译不过的形态。
/// @param props 快照属性对象。
/// @param prefix 每个落点的前导点（当前传 `.`，为嵌套落点预留；链式调用同样以 `.` 起头）。
/// @param type 序列化 type 名。
/// @return 片段串；无可用属性时为空串。
[[nodiscard]] inline auto emit_props(const Json &props, const char *prefix, const std::string &type) -> std::string {
    std::string out{};
    for (const auto &emit : emit_prop_targets(type, props, false)) {
        if (!emit.via_setter) {
            AURORA_LOG_WARN("codegen", type, ".", emit.key,
                            " resolves to a member assignment, which the construction expression cannot carry; "
                            "omitted");
            continue;
        }
        out += prefix + emit.lhs + "(" + emit.expr + ")";
    }
    return out;
}

/// @brief 把 Props 形片段包成构造表达式：`au::T(au::TProps() 片段)`（片段自身已带 `.set_x(值)` 前导点）。
/// @param cls C++ 类名（已去命名空间）。
/// @param pr Props 类名。
/// @param body Props 形片段。
/// @return 该节点的构造表达式。
[[nodiscard]] inline auto wrap_props_ctor(const std::string &cls, const std::string &pr, const std::string &body)
    -> std::string {
    return "au::" + cls + "(au::" + pr + "()" + body + ")";
}

/// @brief 子节点是**构造首参**（`T(Node, TProps = {})`）的链式单子容器。
///
/// 子节点已从属性聚合里移出（聚合只管可序列化属性），但容器仍须在构造期接管子节点，于是首参就是
/// `Node`；属性经第二参 `TProps` 整体移入、在构造尾部接线。发射形态
/// `au::PullToRefresh(子节点, au::PullToRefreshProps().set_threshold(48.0F))`、
/// `au::Scroll(子节点, au::ScrollProps().set_step(24.0f))`。
///
/// @param type 序列化 type 名。
/// @return 该类型是否以单个 `Node` 为首参构造。
[[nodiscard]] constexpr auto single_child_first_ctor(std::string_view type) noexcept -> bool {
    return type == "PullToRefresh" || type == "Scroll";
}

/// @brief 子节点是**构造首参**（`T(std::vector<Node>, TProps = {})`）的链式聚合容器。
///
/// 子节点已从属性聚合里移出（聚合只管可序列化属性），但容器仍须在构造期接管子项，于是首参就是
/// `std::vector<Node>`；属性经第二参 `TProps` 整体移入、在构造尾部接线。发射形态
/// `au::Column({ 子项 }, au::ColumnProps().set_gap(8.0F))`。
///
/// @param type 序列化 type 名。
/// @return 该类型是否以子节点列表为首参构造。
[[nodiscard]] constexpr auto kids_first_ctor(std::string_view type) noexcept -> bool {
    return type == "Column" || type == "Row" || type == "Grid";
}

/// @brief 该类型能否在构造期交出子节点：不能则告警并返回空列表。
/// @note 子节点没有「构造后统一入口」——`Container::children_` 是 protected，公开面上只有构造形参
/// （无聚合类型是 `T{ Node… }` 初始化列表，链式形态容器是 `T(std::vector<Node>, TProps)` 或
/// `T(Node, TProps)` 的首参；`*Props` 聚合已不再带 `children`/`child` 成员）。故 bespoke 构造的类型只能
/// 省略子节点，并**必须在 stderr 留话**：生成的树看着完整、实际少一层，是比编译失败更坏的故障。
/// @param type 序列化 type 名。
/// @param kids 已递归生成的子项表达式列表。
/// @return 可发射的子项列表；bespoke 构造类型返回空列表并告警。
[[nodiscard]] inline auto accept_children(const std::string &type, const std::vector<std::string> &kids)
    -> std::vector<std::string> {
    if (kids.empty() || !is_bespoke_ctor(type)) {
        return kids;
    }
    AURORA_LOG_WARN("codegen", type,
                    " has no generic construction entry (positional constructor or template arguments required), "
                    "omitted ",
                    kids.size(), " child node(s)");
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

/// @brief 生成一个节点的构造表达式（Fluent 与 DesignatedInit 共用，两风格同一条发射路径）。
///
/// 形态选择（每条都必须是可编译的真实构造）：
/// 1. 不在链式写件名单里的类型（`Stack`/`Spacer`/`TabBar`…）→ `au::T{}` 或初始化列表形
///    `au::T{ 子项 }`；属性无处安放（见 `props_chain_writers` 的说明与 specification/08-tooling.md §2.5）。
/// 2. 无子节点 → `au::T(au::TProps().set_x(…))`。
/// 3. 子节点是构造首参的容器（`kids_first_ctor`）→ `au::T({ 子项 }, au::TProps().set_x(…))`。
/// 3b. 子节点是构造首参的单子容器（`single_child_first_ctor`）→
///     `au::T(子节点, au::TProps().set_x(…))`（无属性时 `au::T(子节点)`）。
/// 4. 有子节点却两者皆非 → 告警丢弃子节点，只发 `au::T(au::TProps().set_x(…))`。
///    子节点没有「构造后统一入口」（`Container::children_` 是 protected），兜底分支曾按
///    `au::TProps{ …, .children = { 子项 } }` 发射，而链式形态的字段是私有的，那条产物编译红；
///    随「有聚合」与「只可链式」两份名单合一，它已不可达，按「宁缺不伪」改成留话的省略。
///    在册 76 型的实测分支落点只有 1/2/3/3b 四条，本条是给「新增容器却忘了登记构造槽」预留的守卫。
///
/// @param type          序列化 type 名。
/// @param props_str     emit_props 的标量属性串（形如 `.set_gap(8.0f).set_columns(2)`，直接续在
///                      `TProps()` 之后）。
/// @param kids_all      已递归生成的子节点表达式（bespoke 构造类型可能被告警丢弃）。
/// @param indent        缩进层级（一级 4 空格）。
/// @return 该节点的构造表达式。
[[nodiscard]] inline auto emit_node(const std::string &type, const std::string &props_str,
                                    const std::vector<std::string> &kids_all, int indent) -> std::string {
    const std::string cls = cpp_class(type);
    (void)note_construction(type);
    const std::vector<std::string> kids = accept_children(type, kids_all);
    if (!props_chain_writers(type)) {
        return "au::" + cls + (kids.empty() ? std::string{"{}"} : emit_child_list(kids, indent));
    }
    const std::string pr = cpp_props(type);
    if (kids.empty()) {
        return wrap_props_ctor(cls, pr, props_str);
    }
    if (kids_first_ctor(type)) {
        const std::string list = emit_child_list(kids, indent);
        return props_str.empty() ? "au::" + cls + list
                                 : "au::" + cls + "(" + list + ", au::" + pr + "()" + props_str + ")";
    }
    if (single_child_first_ctor(type)) {
        const std::string &kid = kids.front();
        return props_str.empty() ? "au::" + cls + "(" + kid + ")"
                                 : "au::" + cls + "(" + kid + ", au::" + pr + "()" + props_str + ")";
    }
    AURORA_LOG_WARN("codegen", type, " has ", kids.size(),
                    " child node(s) but neither a child vector nor a single Node is a constructor argument; omitted");
    return wrap_props_ctor(cls, pr, props_str);
}
}  // namespace detail

/// @brief 递归下降构造表达式生成器（Fluent 与 DesignatedInit 共用；StepByStep 另走 to_code_sb）。
/// @note `style` 只作递归透传：表达式两条风格走同一条发射路径，产物逐字节相同，不据它分叉。
/// @param node 子树快照 JSON。
/// @param style 代码生成风格（Fluent / DesignatedInit 同路径）。
/// @param indent 缩进层级（一级 4 空格）。
/// @return 该子树的构造表达式串。
[[nodiscard]] inline auto to_code_expr(const Json &node, CodeStyle style, int indent) -> std::string {
    const std::string type = node.contains("type") ? node.at("type")->as_or<std::string>("") : "Column";
    const Json &props = node.contains("props") ? *node.at("props") : Json::object();
    std::vector<std::string> kids{};
    const auto *children_val = node.at("children");
    if (children_val != nullptr && children_val->is_array()) {
        for (const auto &kid : *children_val) {
            if (kid.is_object()) {
                kids.push_back(to_code_expr(kid, style, indent + 2));
            }
        }
    }
    return detail::emit_node(type, detail::emit_props(props, ".", type), kids, indent);
}

/// @brief 生成 DesignatedInit 形式（indent 为缩进层级）：与 Fluent **逐字节相同**的别名入口。
///
/// 指定初始化器要求 `*Props` 的公有同名成员，而属性面全量迁到链式写件后，18 型聚合的字段一律私有
/// （`au::ButtonProps{ .label = "OK" }` 编译红）；其余 58 型不在发射名单内，表达式压根不给它们装属性
/// （按能力边界省略）。故没有一条产物再发设计符。保留这个入口只为 CLI `--style di`、MCP 与 Inspector
/// 的 `style=2` 不破约；itest_to_code 逐字节钉住两风格同产物，别名一旦悄悄分叉即红。
/// @param node 子树快照 JSON。
/// @param indent 缩进层级（一级 4 空格）。
/// @return 与 Fluent 相同的构造表达式串。
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
/// 嵌套成员路径与 setter-only 属性（TextInput 的样式组）都只能在构造后写入。
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
        for (const auto &kid : *children_val) {
            if (kid.is_object()) {
                all_kids.push_back("au::Node{std::move(" + to_code_sb(kid, indent, os, counter) + ")}");
            }
        }
    }

    os << pad << "auto " << var << " = ";
    (void)detail::note_construction(type);
    // bespoke 构造的类型无法在构造期交出子节点（见 accept_children 的告警），此处只落成空构造。
    const std::vector<std::string> kids = detail::accept_children(type, all_kids);
    if (kids.empty()) {
        os << "au::" << cls << "{}";
    } else {
        // 子节点只能在构造期传入：构造后再给继承来的 `children_` 赋值是静默 no-op。
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

/// @brief 把序列化的 widget 树 JSON 反向生成为 Aurora C++ 源码（需求 SPEC.FEAT.TOOLING.UI-TO-CODE.001 /
/// specification/08-tooling.md §2.5）。
///
/// 输入为 `to_json(widget)` 产生的结构快照。默认 Fluent 风格（容器把子项作构造首参、属性续在
/// `au::Type(au::TypeProps()` 之后的链式写件；无属性聚合的类型直接罗列子项）。
/// 可用 CodeStyle 选择 DesignatedInit（与 Fluent 同产物的别名，见 `to_code_di`）或 StepByStep（先声明后逐属性赋值）。
///
/// 属性覆盖度：**StepByStep 完整**；Fluent / DesignatedInit 只承载链式写件可装入的自有属性，
/// 基类属性与无写入通道的键会被省略（逐类型能力表见 specification/08-tooling.md §2.5）。
///
/// 用于「设计工具 → 代码」工作流，或把快照作为可编辑源码。
///
/// @param node to_json(widget) 的结构快照。
/// @param indent 缩进层级（一级 4 空格）。
/// @return Fluent 风格 C++ 源码串。
[[nodiscard]] inline auto to_code(const Json &node, int indent = 0) -> std::string {
    return to_code_expr(node, CodeStyle::Fluent, indent);
}

/// @brief 按指定风格生成代码（需求 SPEC.FEAT.TOOLING.UI-TO-CODE.001）。默认 Fluent，与 to_code(node, int) 行为一致。
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
