#pragma once

// ============================================================================
// args.h — argv token → 强类型值的解析与取值（需求规格：specification/09-cli.md §4–§6）
// ----------------------------------------------------------------------------
// 本头只管「一条已声明命令的 token 序列怎么变成值」；命令/选项的声明与派生文本在
// `aurora/cli/command.h`。设计内核与库内其他模块一致：声明式 + 概念可枚举 + 强类型 +
// 零异常。
//
//   * 零异常：一切失败经 `Result<T>` + `codespec/errors.toml` 的 `cli-*` 码上报。
//   * 展示请求是一等结果而非错误：任何旗标（内建 `--help`/`--version`，或调用方自己声明并标了
//     `OptionSchema::early_view` 的旗标，如 `--dump-schema`）都只把 `Invocation::view` 置为对应的
//     `EarlyView` 并在 `display_text` 给出渲染好的文本，调用方 `AURORA_LOG_RAW` 落 stdout 即可。
//   * 语法取 GNU/POSIX 全集：`--k=v` `--k v` `-k v` `-kv` `-k=v` `-abc` 聚组、`--` 终止、
//     负数消歧、重复选项按 arity 累积。不做前缀缩写匹配（歧义不可枚举）。
//
// 生命周期：`Arguments` 内以指针引用解析时传入的命令声明，故该声明必须比任何由它解析出
// 的 `Invocation` / `Arguments` 活得更久（同 clap 借用 App 的约定）。
//
// 退出码约定（交由调用方实施，库本身不 exit、不打印）：
//   0 = view 为 None/Help/Version/Schema 且业务成功；2 = Err(Error)（用法错误）；1 = 业务失败。
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/dimension.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"

namespace aurora::cli {

struct CommandSpec;  // 声明在 command.h；此处仅按指针引用
struct OptionSchema;  // 声明在 command.h

namespace detail {
/// @brief 字面量转换的唯一出口（实现见 src/aurora/cli/args.cpp，内部头 literals.h 声明）。
///        以友元类形式存在，避免把 `Value` 的构造器开放为公共 API。
class LiteralFactory;
}  // namespace detail

// ---------------------------------------------------------------- 值类型

/**
 * @brief 选项/位置参数的值类型（需求 #9：类型集合封闭可枚举，无开放扩展点）。
 *
 * 决定 token → 内部表示的转换规则，字面量语法见 specification/09-cli.md §5。
 */
enum class ValueKind : std::uint8_t {
    Bool = 0,  ///< flag：出现即 true，不消耗后续 token
    Int,  ///< 十进制整数（可带正负号），内部存 std::int64_t
    Double,  ///< 十进制浮点，内部存 double
    String,  ///< 原样字符串
    Enum,  ///< 词表字符串（须填 `OptionSchema::choices`），内部存 std::string
    Length,  ///< 尺寸意图：`123` / `123px` / `25%` / `fill` / `auto`
    Color,  ///< 颜色：`#rgb` / `#rrggbb` / `#rrggbbaa` / `rgb(r,g,b)` / `rgba(r,g,b,a)`
    LogLevel,  ///< 日志级别：trace|debug|info|warn|error|fatal（大小写不敏感，另收 TRC/DBG/…）
    Duration,  ///< 时长：`500`（缺省毫秒）/ `250ms` / `5s` / `2m` / `1h` / `1d`，内部存毫秒
};

/// @brief 全部取值类型（顺序即 `ValueKind` 枚举序），供 schema 导出与帮助渲染。
[[nodiscard]] auto all_value_kinds() -> std::vector<ValueKind>;

/// @brief 值类型的线名（小写，与 `schema_json` 的 `"type"` 字段一致）。
[[nodiscard]] auto to_string(ValueKind kind) noexcept -> std::string_view;

/// @brief 线名 → 值类型；未知返回 std::nullopt。
[[nodiscard]] auto value_kind_from_name(std::string_view name) noexcept -> std::optional<ValueKind>;

// ----------------------------------------------------------------  arity

/**
 * @brief 值个数区间 `[min, max]`；`max == AURORA_UNBOUNDED` 表示无上界。
 *
 * Bool/flag 用 `{0,0}`；单值选项用 `{1,1}`；列表选项用 `{0,AURORA_UNBOUNDED}`。
 * 多数场景直接用下方命名工厂，只有非标准的区间（如 `{2,4}`）才手写聚合。
 */
struct Arity {
    static constexpr int AURORA_UNBOUNDED = -1;  ///< 无上界哨兵

    int min = 1;
    int max = 1;

    [[nodiscard]] static constexpr auto exactly_one() noexcept -> Arity { return {.min = 1, .max = 1}; }
    [[nodiscard]] static constexpr auto optional_one() noexcept -> Arity { return {.min = 0, .max = 1}; }
    [[nodiscard]] static constexpr auto flag() noexcept -> Arity { return {.min = 0, .max = 0}; }
    [[nodiscard]] static constexpr auto at_least_one() noexcept -> Arity { return {.min = 1, .max = AURORA_UNBOUNDED}; }
    [[nodiscard]] static constexpr auto zero_or_more() noexcept -> Arity { return {.min = 0, .max = AURORA_UNBOUNDED}; }

    /// @brief 是否允许「不消耗 token」（即作为无值 flag 出现）。
    [[nodiscard]] constexpr auto allows_no_value() const noexcept -> bool { return min == 0; }

    /// @brief 单条选项出现时消耗多少个 token：定长则返回该值，变长返回 std::nullopt。
    [[nodiscard]] constexpr auto fixed_span() const noexcept -> std::optional<int> {
        return (min == max) ? std::optional<int>{min} : std::optional<int>{};
    }

    /// @brief 上界的可读文本（无界渲染为 ∞），用于错误参数与 usage。
    [[nodiscard]] auto max_text() const -> std::string;

    /// @brief 渲染为帮助/usage 里的占位片段（如 `<VALUE>` / `<VALUE>...` / 空串）。
    [[nodiscard]] auto help_placeholder(std::string_view value_hint) const -> std::string;
};

// ---------------------------------------------------------------- 取值

/**
 * @brief 已按声明类型转换完成的单个参数值。
 *
 * 内部是 `std::variant` + 记录来源 `ValueKind`。公共出口只有一个泛型 `as<T>()`（加 `kind()` 与
 * `raw_text()`）：数值访问器允许「无损」跨读，`Int`/`Duration` → `double` 加宽、`double` → 整型
 * 仅当值恰为整且可表示，否则 `cli-invalid-value`；类型不符一律 `cli-invalid-value`，窄化越界一律
 * `cli-range-violated`。`ValueKind::Duration` 没有对应 C++ 类型，用 `as<std::int64_t>()` 取毫秒。
 *
 * @note Thread: thread-safe（纯值类型）
 * @note Side-effects: none
 */
class Value {
  public:
    /// @brief 变体分支由 `ValueKind` 决定（Duration 存毫秒整数，Enum 存字符串）。
    using Raw = std::variant<std::monostate, bool, std::int64_t, double, std::string, Length, Color, LogLevel>;

    Value() = default;

    [[nodiscard]] constexpr auto kind() const noexcept -> ValueKind { return kind_; }

    /// @brief 用户输入的 token 原文（未经转换）。始终可得，供错误回显与脚本消费。
    [[nodiscard]] auto raw_text() const -> const std::string & { return raw_text_; }

    /**
     * @brief 按 C++ 类型取值（`get<T>` 风格的唯一强类型出口）。
     * @tparam T 支持集合可枚举：bool / int / std::int64_t / double / std::string /
     *           Length / Color / LogLevel。其余类型编译期即拒绝。
     */
    template <typename T>
    [[nodiscard]] auto as() const -> Result<T> {
        if constexpr (std::is_same_v<T, bool>) {
            return read_bool();
        } else if constexpr (std::is_same_v<T, int>) {
            return read_int();
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            return read_int64();
        } else if constexpr (std::is_same_v<T, double>) {
            return read_double();
        } else if constexpr (std::is_same_v<T, std::string>) {
            return read_string();
        } else if constexpr (std::is_same_v<T, Length>) {
            return read_length();
        } else if constexpr (std::is_same_v<T, Color>) {
            return read_color();
        } else if constexpr (std::is_same_v<T, LogLevel>) {
            return read_log_level();
        } else {
            static_assert(sizeof(T) == 0,
                          "aurora::cli::Value::as<T>: unsupported T; the supported set is "
                          "bool, int, int64_t, double, std::string, Length, Color, LogLevel");
        }
    }

  private:
    friend class Parser;
    friend class Arguments;
    friend class detail::LiteralFactory;

    Value(ValueKind kind, Raw raw, std::string literal)
        : kind_(kind), raw_(std::move(raw)), raw_text_(std::move(literal)) {}

    /// @brief 变体只读访问（跨类型无损加宽由 `read_int64` / `read_double` 使用）。
    [[nodiscard]] const Raw &raw() const noexcept { return raw_; }

    // as<T>() 的具型别实现；留在 args.cpp，公共头只保留一个泛型出口。
    [[nodiscard]] auto read_bool() const -> Result<bool>;
    [[nodiscard]] auto read_int() const -> Result<int>;
    [[nodiscard]] auto read_int64() const -> Result<std::int64_t>;
    [[nodiscard]] auto read_double() const -> Result<double>;
    [[nodiscard]] auto read_string() const -> Result<std::string>;
    [[nodiscard]] auto read_length() const -> Result<Length>;
    [[nodiscard]] auto read_color() const -> Result<Color>;
    [[nodiscard]] auto read_log_level() const -> Result<LogLevel>;

    ValueKind kind_ = ValueKind::String;
    Raw raw_;
    std::string raw_text_;
};

/**
 * @brief 一次成功解析的产物：按长名/下标取值，不持有用户变量。
 *
 * 默认值已在解析结束时物化进槽位，故即使用户没给，`get<int>("width")` 也成功；要区分
 * 「用户显式给出」与「回落默认」用 `explicitly_given()`。
 */
class Arguments {
  public:
    /// @brief 空结果（未解析）：取值一律为空/失败，仅用于默认构造 `Invocation`。
    Arguments() = default;

    /// @brief 长名对应的所有出现值（未出现且无默认 → 空表，不算错误）。
    [[nodiscard]] auto values(std::string_view long_name) const -> std::vector<Value>;

    /// @brief 强类型取单值：缺失返回 `cli-missing-required`，多值返回 `cli-arity-violated`。
    template <typename T>
    [[nodiscard]] auto get(std::string_view long_name) const -> Result<T> {
        auto single = one(long_name);
        if (!single) {
            return single.error();
        }
        return single.value().template as<T>();
    }

    /// @brief Bool/flag 是否生效（含 `[--flag=false]` 显式关闭）；未声明的长名返回 false。
    [[nodiscard]] auto flag(std::string_view long_name) const -> bool;

    /// @brief 该长名出现的次数（`-v -v -v` → 3）；默认值不计。
    [[nodiscard]] auto count(std::string_view long_name) const -> int;

    /// @brief 是否由用户显式给出（区别于默认值物化）。
    [[nodiscard]] auto explicitly_given(std::string_view long_name) const -> bool;

    /// @brief 第 index 个位置参数值（按声明顺序展开后的扁平序列）。
    [[nodiscard]] auto positional(std::size_t index) const -> Result<Value>;

    /// @brief 全部位置参数值（含默认值物化）。
    [[nodiscard]] auto positionals() const -> const std::vector<Value> & { return positionals_; }

    /// @brief `--` 之后的原始 token（未经任何类型转换）。
    [[nodiscard]] auto rest() const -> const std::vector<std::string> & { return rest_; }

    /// @brief 命中的命令链，根在前（如 {"aurora_cli", "render"}）。
    [[nodiscard]] auto command_chain() const -> const std::vector<std::string> & { return chain_; }

    /// @brief 命令链拼成的可执行调用名（空格分隔），用于错误回显。
    [[nodiscard]] auto command_display() const -> std::string;

    /// @brief 最终生效的命令声明（子命令叶节点）；未设置时返回 nullptr。
    [[nodiscard]] auto matched_command() const -> const CommandSpec * { return matched_; }

  private:
    friend class Parser;

    /// @brief 单值读出（`get<T>()` 的实现）：缺失 `cli-missing-required`，多值 `cli-arity-violated`。
    [[nodiscard]] auto one(std::string_view long_name) const -> Result<Value>;

    /// @brief 一个选项槽：声明 + 累积值 + 是否显式给出。
    struct Slot {
        const OptionSchema *spec = nullptr;
        std::vector<Value> values;
        bool given = false;
    };

    [[nodiscard]] Arguments(std::vector<Slot> slots, std::vector<Value> positionals, std::vector<std::string> rest,
                            std::vector<std::string> chain, const CommandSpec *matched)
        : slots_(std::move(slots)), positionals_(std::move(positionals)), rest_(std::move(rest)),
          chain_(std::move(chain)), matched_(matched) {}

    [[nodiscard]] auto find_slot(std::string_view long_name) const -> const Slot *;

    std::vector<Slot> slots_;
    std::vector<Value> positionals_;
    std::vector<std::string> rest_;
    std::vector<std::string> chain_;
    const CommandSpec *matched_ = nullptr;
};

/// @brief 提前展示的视图：封闭词表，新增一项即多一种可声明的「短路出口」（需求 #9）。
///
/// `Help`/`Version` 由库内建旗标使用（见 `CommandSpec::builtins`），`Schema` 既可作内建亦可由
/// 调用方标在自有旗标上（如 `--dump-schema`）；`None` 表示没有展示请求，正常走业务。
enum class EarlyView : std::uint8_t {
    None = 0,
    Help,  ///< `--help` / `-h`（任意层级）
    Version,  ///< 声明了 `version` 的那一层命令的 `--version` / `-V`
    Schema,  ///< 整棵声明树的 `schema_json`（缩进 2，尾随换行）
};

/// @brief 解析产物：展示视图 + 取值 + 已渲染的展示文本（`view != None` 时非空）。
struct Invocation {
    EarlyView view = EarlyView::None;
    Arguments arguments;
    std::string display_text;

    /// @brief 是否命中了某个提前展示旗标：为真时调用方只需打印 `display_text`，不必进业务。
    [[nodiscard]] constexpr auto shows_display() const noexcept -> bool { return view != EarlyView::None; }
};

/// @brief 展示视图的线名（小写，`None` 为 "ok"），供 schema / 日志消费。
[[nodiscard]] auto early_view_to_string(EarlyView view) noexcept -> std::string_view;

// ---------------------------------------------------------------- 解析入口

/**
 * @brief 解析命令行 token 序列。
 * @param root          命令声明；必须比返回的 Invocation 活得久。
 * @param tokens        不含 argv[0] 的参数序列（调用方自行去掉程序名）。
 * @param program_name  usage/help 里显示的程序名；空则回落 `root.name`，再空则 "program"。
 * @return 用法错误返回 `cli-*` 结构化 Error；展示请求返回成功的 `Invocation`
 *         （`view` 非 `None`，`display_text` 已渲染）。
 */
[[nodiscard]] auto parse(const CommandSpec &root, const std::vector<std::string> &tokens,
                         std::string_view program_name = {}) -> Result<Invocation>;

/**
 * @brief C 入口便利重载：把 `main(argc, argv)` 直接交给库，自动跳过程序名，
 *        并从 argv[0] 取 basename 作为程序名。
 * @note argc <= 0 按「无参数」处理，不视为错误。
 */
[[nodiscard]] auto parse(const CommandSpec &root, int argc, const char *const *argv) -> Result<Invocation>;

}  // namespace aurora::cli
