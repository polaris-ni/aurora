#pragma once
// ============================================================================
// json.h — Aurora 自研 JSON 编解码库（公共单头，框架正式组成）
// ----------------------------------------------------------------------------
// 分层约定：
//   * 本头为「轻头」：只含类型定义、声明与极少量 inline 构造（算术构造）。
//     其余实现全部位于 src/aurora/core/json/*.cpp；头内不放大段逻辑或模板实现，
//     故封闭模板成员 as_or / as<T> / get<T> 在此仅留声明，实现与显式实例化在
//     value.cpp。
//   * 层边界（tools/check/check_core_layer_boundary.py）：只可 include "aurora/core/*"
//     与标准库；禁引 aurora/aurora.h 与 aurora/aurora_pch.h。
//   * Type 的序数与 Value 内部 variant 的 alternative index 强制同构（内部契约）。
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "aurora/core/result.h"

namespace aurora::json {

/// @brief JSON 值类型判别（9 种）。
/// @note 枚举量序数与 `Value` 内部 `std::variant` 的 alternative index **强制同构**。
///       两者任一重排都会使 tests/unit/utest_json.cpp 的逐枚举量断言失败。本枚举经
///       tools/include/known_enums.h 登记为 "JsonType"，从而进入 aurora_api.json，
///       使 AI 侧可枚举 JSON 类型判别值。
enum class Type : std::uint8_t {
    Null = 0,  ///< null
    Bool = 1,  ///< true / false
    Int = 2,  ///< 有符号整数（int64_t 域内）
    UInt = 3,  ///< 无符号整数（> INT64_MAX 的正数）
    Double = 4,  ///< 双精度浮点（shortest round-trip 文本化）
    RawNumber = 5,  ///< 保真数字（超 int64/uint64 域或 double 往返失真的原字面量）
    String = 6,  ///< UTF-8 字符串
    Array = 7,  ///< 数组
    Object = 8,  ///< 对象（插入序）
};

class Value;

/// @brief JSON 数组（插入序）。
using Array = std::vector<Value>;
/// @brief JSON 对象（插入序；小文档线性查找，对缓存友好且零节点分配）。
using Object = std::vector<std::pair<std::string, Value>>;

/// @brief 保真数字：超 int64/uint64 域，或 double 往返失真的原始字面量文本。
/// @note 独立类型，与 `String` 不同型，保证 `Type::RawNumber` 可判别。
struct RawNumber {
    std::string text;

    /// @brief 按文本比较（支撑 Value 的 variant 相等）。
    [[nodiscard]] friend auto operator==(const RawNumber &a, const RawNumber &b) -> bool = default;
};

/// @brief 算术域约束：隐式入向构造的合法 T。`bool` 走 `explicit` 构造，排除。
template <typename T>
concept json_arith = std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>;

/// @brief 读出口封闭类型集：`as<T>` / `as_or<T>` / `get<T>` 仅对这些 T 可见，
///        无用户特化点；域外类型被 `static_assert` 拒绝。
template <typename T>
concept json_readable =
    std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t> || std::is_same_v<T, int> ||
    std::is_same_v<T, std::uint64_t> || std::is_same_v<T, std::size_t> || std::is_same_v<T, float> ||
    std::is_same_v<T, double> || std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>;

/// @brief Object 条目只读视图（零拷贝遍历）。
/// @warning `key` 与 `value` 均**别名** Value 内部存储；该 Value 被移动 / 修改后即失效。
struct Entry {
    std::string_view key;
    const Value &value;
};

/// @brief `Value::entries()` 的只读 range（非拥有）。非 Object 上产出空 range。
/// @note 成员体在 `Value` 定义之后（此处先声明），因其触及 `std::pair<std::string, Value>`，
///       需要 `Value` 为完整类型。
class EntryRange;

/// @brief JSON 值。全值语义（拷贝即深拷贝），节点无逐节点堆分配。
/// @note Thread: 非线程安全（与容器一致）
class Value {
  public:
    // ---- 构造：空 / 布尔 ----
    Value() noexcept;
    Value(std::nullptr_t) noexcept;
    explicit Value(bool b) noexcept;

    // ---- 构造：算术（隐式入向：只进不出；按 T 的精确类别落域，绝不跨类别）----
    /// @brief 浮点 T → Double；有符号整型 T → Int；无符号整型 T → Int（值 ≤ INT64_MAX）
    ///        或 UInt。域外 T（指针 / 自定义类型）被概念约束拒绝。
    template <json_arith T>
    Value(T v) noexcept {  // NOLINT(google-explicit-constructor)：隐式入向是本类设计意图
        if constexpr (std::is_floating_point_v<T>) {
            data_.emplace<double>(static_cast<double>(v));
        } else if constexpr (std::is_signed_v<T>) {
            data_.emplace<std::int64_t>(static_cast<std::int64_t>(v));
        } else if (static_cast<std::uint64_t>(v) <=
                   static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            data_.emplace<std::int64_t>(static_cast<std::int64_t>(v));
        } else {
            data_.emplace<std::uint64_t>(static_cast<std::uint64_t>(v));
        }
    }

    // ---- 构造：字符串（隐式入向）----
    Value(const char *s);  // NOLINT(google-explicit-constructor)：String（nullptr → Null）
    Value(std::string_view s);  // NOLINT(google-explicit-constructor)：String（拷贝）
    Value(std::string s) noexcept;  // String（移动）

    // ---- 工厂 ----
    [[nodiscard]] static auto raw_number(std::string_view digits) -> Value;  ///< RawNumber 保真入口
    [[nodiscard]] static auto array() -> Value;  ///< 空 Array
    [[nodiscard]] static auto object() -> Value;  ///< 空 Object

    // ---- 类型查询 ----
    [[nodiscard]] auto type() const noexcept -> Type;  ///< = static_cast<Type>(data_.index())
    [[nodiscard]] auto is_null() const noexcept -> bool;
    [[nodiscard]] auto is_bool() const noexcept -> bool;
    [[nodiscard]] auto is_int() const noexcept -> bool;
    [[nodiscard]] auto is_uint() const noexcept -> bool;
    [[nodiscard]] auto is_double() const noexcept -> bool;
    [[nodiscard]] auto is_raw_number() const noexcept -> bool;
    [[nodiscard]] auto is_string() const noexcept -> bool;
    [[nodiscard]] auto is_array() const noexcept -> bool;
    [[nodiscard]] auto is_object() const noexcept -> bool;
    [[nodiscard]] auto is_number() const noexcept -> bool;  ///< Int|UInt|Double|RawNumber
    [[nodiscard]] auto is_integer() const noexcept -> bool;  ///< Int|UInt

    // ---- 读：宽容路径（空安全、零 UB；「非法值回退默认」的官方入口）----
    /// @note 缺失 / 类型不符 / 容器类型不符 → 返回 fallback，不返回指针，无解引用风险。
    template <json_readable T>
    [[nodiscard]] auto as_or(T fallback) const noexcept -> T;  // 自身
    template <json_readable T>
    [[nodiscard]] auto as_or(std::string_view key, T fallback) const noexcept -> T;  // Object 子键
    template <json_readable T>
    [[nodiscard]] auto as_or_at(std::size_t index, T fallback) const noexcept -> T;  // Array 元素

    // ---- 读：指针路径（调用方自判空；可变重载供就地写）----
    /// @note `at` 的键族与索引族是同名重载，两者**语义一致**：命中返回指针，
    ///       键缺失 / 索引越界 / 容器类型不符一律返回 `nullptr`，**绝不抛异常**
    ///       （刻意区别于 STL `at` 的 `out_of_range` 语义，故调用方必须自判空）。
    ///       `find(key)` / `contains(key)` 与 `at(key)` 等价，保留供键语义自述。
    [[nodiscard]] auto find(std::string_view key) noexcept -> Value *;
    [[nodiscard]] auto find(std::string_view key) const noexcept -> const Value *;
    [[nodiscard]] auto at(std::string_view key) noexcept -> Value *;
    [[nodiscard]] auto at(std::string_view key) const noexcept -> const Value *;
    [[nodiscard]] auto at(std::size_t index) noexcept -> Value *;
    [[nodiscard]] auto at(std::size_t index) const noexcept -> const Value *;
    [[nodiscard]] auto contains(std::string_view key) const noexcept -> bool;
    [[nodiscard]] auto size() const noexcept -> std::size_t;
    [[nodiscard]] auto empty() const noexcept -> bool;

    // ---- 读：严格路径（结构化错误）----
    template <json_readable T>
    [[nodiscard]] auto as() const -> Result<T>;  // 自身
    template <json_readable T>
    [[nodiscard]] auto get(std::string_view key) const -> Result<T>;  // Object 子键

    // ---- 读：无模板族（免实例化，供热路径与宽容读取）----
    [[nodiscard]] auto as_bool() const noexcept -> std::optional<bool>;
    [[nodiscard]] auto as_int() const noexcept -> std::optional<std::int64_t>;
    [[nodiscard]] auto as_double() const noexcept -> std::optional<double>;
    [[nodiscard]] auto as_string() const noexcept -> std::optional<std::string_view>;
    [[nodiscard]] auto as_raw_number() const noexcept -> std::optional<std::string_view>;

    // ---- 写（仅 Object/Array 有效；其他类型上调用是未定义行为，debug 断言拦截）----
    auto set(std::string_view key, Value v) -> void;  ///< Object：插入或覆盖（保持首次插入位置）
    auto push_back(Value v) -> void;  ///< Array：追加（不提供 emplace_back）
    auto reserve(std::size_t n) -> void;  ///< Array/Object：容量预留（批量构造热路径）
    auto clear() noexcept -> void;  ///< 清空为对应空容器
    auto erase(std::string_view key) -> bool;  ///< Object：删键，返回是否命中
    auto erase_at(std::size_t index) -> bool;  ///< Array：删元素，返回是否命中

    // ---- 迭代 ----
    [[nodiscard]] auto entries() const noexcept -> EntryRange;  ///< Object → Entry 只读视图（非 Object → 空 range）
    [[nodiscard]] auto begin() const noexcept -> const Value *;  ///< Array → 元素指针（非 Array → end()）
    [[nodiscard]] auto end() const noexcept -> const Value *;

    // ---- 相等（同 Type 严格比较；跨数值类型不等，以保 diff 语义）----
    [[nodiscard]] friend auto operator==(const Value &a, const Value &b) -> bool { return a.data_ == b.data_; }
    [[nodiscard]] friend auto operator!=(const Value &a, const Value &b) -> bool { return !(a == b); }

  private:
    /// @brief 封闭读类型集的统一读取（宽容、无错误对象）；实现见 value.cpp。
    ///        供 as_or / as / get 共用，避免逐类型重复逻辑。
    template <json_readable T>
    [[nodiscard]] auto read_impl() const noexcept -> std::optional<T>;

    /// @brief 内部存储。alternative index **必须**与 Type 枚举量同构：
    ///        0=Null 1=Bool 2=Int 3=UInt 4=Double 5=RawNumber 6=String 7=Array 8=Object。
    std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, RawNumber, std::string, Array, Object>
        data_;
};

class EntryRange {
  public:
    class Iterator {
      public:
        Iterator() noexcept = default;
        explicit Iterator(const std::pair<std::string, Value> *p) noexcept : p_(p) {}

        [[nodiscard]] auto operator*() const noexcept -> Entry;
        auto operator++() noexcept -> Iterator &;
        auto operator++(int) noexcept -> Iterator;
        [[nodiscard]] auto operator==(const Iterator &o) const noexcept -> bool { return p_ == o.p_; }
        [[nodiscard]] auto operator!=(const Iterator &o) const noexcept -> bool { return p_ != o.p_; }

      private:
        const std::pair<std::string, Value> *p_ = nullptr;
    };

    explicit EntryRange(const Object *obj) noexcept : obj_(obj) {}
    [[nodiscard]] auto begin() const noexcept -> Iterator;
    [[nodiscard]] auto end() const noexcept -> Iterator;

  private:
    const Object *obj_ = nullptr;
};

inline auto EntryRange::Iterator::operator*() const noexcept -> Entry { return Entry{p_->first, p_->second}; }

inline auto EntryRange::Iterator::operator++() noexcept -> Iterator & {
    ++p_;
    return *this;
}

inline auto EntryRange::Iterator::operator++(int) noexcept -> Iterator {
    Iterator prev = *this;
    ++p_;
    return prev;
}

inline auto EntryRange::begin() const noexcept -> Iterator {
    return obj_ == nullptr ? Iterator{} : Iterator{obj_->data()};
}

inline auto EntryRange::end() const noexcept -> Iterator {
    return obj_ == nullptr ? Iterator{} : Iterator{obj_->data() + obj_->size()};
}

// ============================================================================
// 解析
// ============================================================================

/// @brief 解析选项。
struct ParseOptions {
    std::size_t max_depth = 512;  ///< 嵌套深度上限，超限 → json-depth-exceeded
    bool validate_utf8 = true;  ///< 字符串 / 文档级 UTF-8 校验（无效序列 → 解析失败）
};

/// @brief 解析完整 JSON 文档（RFC 8259）。
/// @return 成功持 Value；失败持结构化 Error（json-parse-error，含 line/column/offset）。
[[nodiscard]] auto parse(std::string_view input, ParseOptions opts = {}) -> Result<Value>;

// ============================================================================
// 序列化
// ============================================================================

/// @brief 序列化选项。
struct DumpOptions {
    int indent = -1;  ///< <0 紧凑单行；≥0 每层缩进空格数（dump(2) 对应 indent=2）
    bool ensure_ascii = false;  ///< true 时非 ASCII 转 \uXXXX（含代理对编码）
};

/// @brief 序列化为 UTF-8 JSON 文本。
/// @return 失败：值内含 NaN/Inf Double（RFC 8259 无对应文本，json-value-not-serializable）。
[[nodiscard]] auto dump(const Value &v, DumpOptions opts = {}) -> Result<std::string>;

/// @brief 追加式序列化（响应帧热路径复用缓冲，避免每帧分配）。
auto dump_into(const Value &v, std::string &out, DumpOptions opts = {}) -> Result<void>;

}  // namespace aurora::json
