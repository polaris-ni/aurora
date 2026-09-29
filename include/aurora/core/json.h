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
#include <memory>
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
    std::string text;  ///< 原始数字字面量文本（逐字节保真）

    /// @brief 按文本比较（支撑 Value 的 variant 相等）。
    /// @param a 左操作数。
    /// @param b 右操作数。
    /// @return 两侧 `text` 逐字节相等时为 true。
    [[nodiscard]] friend auto operator==(const RawNumber &a, const RawNumber &b) -> bool = default;
};

/// @brief 算术域约束：隐式入向构造的合法 T。`bool` 走 `explicit` 构造，排除。
/// @tparam T 待判定的算术类型。
template <typename T>
concept json_arith = std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>;

/// @brief 读出口封闭类型集：`as<T>` / `as_or<T>` / `get<T>` 仅对这些 T 可见，
///        无用户特化点；域外类型被 `static_assert` 拒绝。
/// @tparam T 待判定的读取目标类型。
template <typename T>
concept json_readable =
    std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t> || std::is_same_v<T, int> ||
    std::is_same_v<T, std::uint64_t> || std::is_same_v<T, std::size_t> || std::is_same_v<T, float> ||
    std::is_same_v<T, double> || std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>;

/// @brief Object 条目只读视图（零拷贝遍历）。
/// @warning `key` 与 `value` 均**别名** Value 内部存储；该 Value 被移动 / 修改后即失效。
struct Entry {
    std::string_view key;  ///< 键（别名 Object 内部存储）
    /// @brief 子值只读引用（别名该 Value 内部存储）；零拷贝视图为设计意图。
    const Value &value;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
};

/// @brief `Value::entries()` 的只读 range（非拥有）。非 Object 上产出空 range。
/// @note 成员体在 `Value` 定义之后（此处先声明），因其触及 `std::pair<std::string, Value>`，
///       需要 `Value` 为完整类型。
class EntryRange;

/// @brief JSON 值。全值语义（拷贝即深拷贝），节点无逐节点堆分配。
/// @note Thread: 非线程安全（与容器一致）
class Value {
  public:
    /// @brief 默认构造：Null（空值）。
    Value() noexcept;
    /// @brief nullptr 构造：Null（字面空指针语义）。
    Value(std::nullptr_t) noexcept;
    /// @brief Bool 构造；`explicit` 防 0/1 隐式混入。
    /// @param b 初始布尔值。
    explicit Value(bool b) noexcept;

    // ---- 构造：算术（隐式入向：只进不出；按 T 的精确类别落域，绝不跨类别）----
    // 隐式入向为设计意图；variant::emplace 无 noexcept 规格（可能置 valueless）但各分支的
    // 替代项构造均无抛，实际无异常路径——检查器无法建模。
    /// @brief 浮点 T → Double；有符号整型 T → Int；无符号整型 T → Int（值 ≤ INT64_MAX）
    ///        或 UInt。域外 T（指针 / 自定义类型）被概念约束拒绝。
    /// @tparam T 算术类型：浮点落 Double，有符号落 Int，无符号按值域落 Int / UInt。
    /// @param v 参与构造的算术值。
    template <json_arith T>
    Value(T v) noexcept {  // NOLINT(google-explicit-constructor,bugprone-exception-escape)
        if constexpr (std::is_floating_point_v<T>) {
            data_.emplace<double>(static_cast<double>(v));
        } else if (std::is_signed_v<T> || static_cast<std::uint64_t>(v) <=
                                              static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            data_.emplace<std::int64_t>(static_cast<std::int64_t>(v));
        } else {
            data_.emplace<std::uint64_t>(static_cast<std::uint64_t>(v));
        }
    }

    /// @brief C 字符串构造：非空指针 → String（拷贝内容）；`nullptr` → Null。
    /// @param s 以 NUL 结尾的字节串。
    Value(const char *s);  // NOLINT(google-explicit-constructor)：String（nullptr → Null）
    /// @brief string_view 构造：String（拷贝内容）；隐式入向是本类设计意图。
    /// @param s 待拷贝的字符串内容。
    /// NOLINTNEXTLINE(google-explicit-constructor)
    Value(std::string_view s);
    /// @brief string 构造：String（移动内容入存储，免二次拷贝）。
    /// @param s 待移入的字符串。
    Value(std::string s) noexcept;

    /// @brief 工厂：RawNumber 保真入口，按原始数字字面量文本建值。
    /// @param digits 数字字面量文本（逐字节保真）。
    /// @return Type::RawNumber 的 Value。
    [[nodiscard]] static auto raw_number(std::string_view digits) -> Value;
    /// @brief 工厂：空 Array。
    /// @return Type::Array 且 size() == 0 的 Value。
    [[nodiscard]] static auto array() -> Value;
    /// @brief 工厂：空 Object。
    /// @return Type::Object 且 size() == 0 的 Value。
    [[nodiscard]] static auto object() -> Value;

    /// @brief 类型判别 = static_cast\<Type\>(data_.index())。
    /// @return 与当前存储 alternative 同序数的 Type 枚举量。
    [[nodiscard]] auto type() const noexcept -> Type;
    /// @brief 判别是否 Type::Null。
    /// @return 值为 null 时 true。
    [[nodiscard]] auto is_null() const noexcept -> bool;
    /// @brief 判别是否 Type::Bool。
    /// @return 值为布尔字面量时 true。
    [[nodiscard]] auto is_bool() const noexcept -> bool;
    /// @brief 判别是否 Type::Int。
    /// @return 值为 int64 域内整数时 true。
    [[nodiscard]] auto is_int() const noexcept -> bool;
    /// @brief 判别是否 Type::UInt。
    /// @return 值为超 INT64_MAX 的无符号整数时 true。
    [[nodiscard]] auto is_uint() const noexcept -> bool;
    /// @brief 判别是否 Type::Double。
    /// @return 值为双精度浮点时 true。
    [[nodiscard]] auto is_double() const noexcept -> bool;
    /// @brief 判别是否 Type::RawNumber。
    /// @return 值为保真数字原文时 true。
    [[nodiscard]] auto is_raw_number() const noexcept -> bool;
    /// @brief 判别是否 Type::String。
    /// @return 值为 UTF-8 字符串时 true。
    [[nodiscard]] auto is_string() const noexcept -> bool;
    /// @brief 判别是否 Type::Array。
    /// @return 值为数组时 true。
    [[nodiscard]] auto is_array() const noexcept -> bool;
    /// @brief 判别是否 Type::Object。
    /// @return 值为对象时 true。
    [[nodiscard]] auto is_object() const noexcept -> bool;
    /// @brief 数值族判别：Int、UInt、Double 或 RawNumber 任一。
    /// @return 属于上述四类型之一时 true。
    [[nodiscard]] auto is_number() const noexcept -> bool;
    /// @brief 整数族判别：Int 或 UInt。
    /// @return 属于二者之一时 true。
    [[nodiscard]] auto is_integer() const noexcept -> bool;

    // ---- 读：宽容路径（空安全、零 UB；「非法值回退默认」的官方入口）----
    /// @brief 宽容读自身值：缺失 / 类型不符 / 容器类型不符 → 返回 fallback，不返回指针，无解引用风险。
    /// @tparam T 读取目标类型（限于 json_readable 封闭集）。
    /// @param fallback 读失败时的回退值。
    /// @return 命中域内时按 T 转换的取值，否则 fallback。
    template <json_readable T>
    [[nodiscard]] auto as_or(T fallback) const noexcept -> T;  // 自身
    /// @brief 宽容读 Object 子键：键缺失 / 自身非 Object / 子值类型不符 → 返回 fallback。
    /// @tparam T 读取目标类型（限于 json_readable 封闭集）。
    /// @param key 对象键。
    /// @param fallback 读失败时的回退值。
    /// @return 命中时按 T 转换的子值，否则 fallback。
    template <json_readable T>
    [[nodiscard]] auto as_or(std::string_view key, const T &fallback) const noexcept -> T;  // Object 子键
    /// @brief 宽容读 Array 元素：索引越界 / 自身非 Array / 元素类型不符 → 返回 fallback。
    /// @tparam T 读取目标类型（限于 json_readable 封闭集）。
    /// @param index 元素下标。
    /// @param fallback 读失败时的回退值。
    /// @return 命中时按 T 转换的元素值，否则 fallback。
    template <json_readable T>
    [[nodiscard]] auto as_or_at(std::size_t index, const T &fallback) const noexcept -> T;  // Array 元素

    // ---- 读：指针路径（调用方自判空；可变重载供就地写）----
    /// @brief 指针路径读：命中返回指针，键缺失 / 索引越界 / 容器类型不符一律返回 `nullptr`，
    ///        **绝不抛异常**（刻意区别于 STL `at` 的 `out_of_range` 语义，调用方必须自判空）。
    /// @param key 对象键。
    /// @return 命中子值的指针，未命中为 `nullptr`。
    [[nodiscard]] auto find(std::string_view key) noexcept -> Value *;
    /// @brief find(key) 的常量视图重载：语义与非 const 版一致。
    /// @param key 对象键。
    /// @return 命中子值的常量指针，未命中为 `nullptr`。
    [[nodiscard]] auto find(std::string_view key) const noexcept -> const Value *;
    /// @brief 按键访问：等价 find(key)。
    /// @param key 对象键。
    /// @return 命中子值的指针，未命中为 `nullptr`。
    [[nodiscard]] auto at(std::string_view key) noexcept -> Value *;
    /// @brief at(key) 的常量视图重载。
    /// @param key 对象键。
    /// @return 命中子值的常量指针，未命中为 `nullptr`。
    [[nodiscard]] auto at(std::string_view key) const noexcept -> const Value *;
    /// @brief Array 索引访问，越界返回 `nullptr`。
    /// @param index 元素下标。
    /// @return 命中元素的指针，越界或非 Array 为 `nullptr`。
    [[nodiscard]] auto at(std::size_t index) noexcept -> Value *;
    /// @brief at(index) 的常量视图重载。
    /// @param index 元素下标。
    /// @return 命中元素的常量指针，越界或非 Array 为 `nullptr`。
    [[nodiscard]] auto at(std::size_t index) const noexcept -> const Value *;
    /// @brief 键存在性判别：等价 at(key) != nullptr。
    /// @param key 对象键。
    /// @return 自身为 Object 且含该键时 true。
    [[nodiscard]] auto contains(std::string_view key) const noexcept -> bool;
    /// @brief Array 元素数 / Object 键值对数 / String·RawNumber 字节数；其余类型 0。
    /// @return 容器与字符串的尺寸，其余标量类型恒 0。
    [[nodiscard]] auto size() const noexcept -> std::size_t;
    /// @brief 判空：Null 恒 true；容器与字符串按 size() == 0；其余标量恒 false。
    /// @return 值为空（Null 或空容器 / 空串）时 true。
    [[nodiscard]] auto empty() const noexcept -> bool;

    // ---- 读：严格路径（结构化错误）----
    /// @brief 严格读自身值：类型不符返回结构化错误（json-type-mismatch，携 expected/actual）。
    /// @tparam T 严格读取的目标类型（限于 json_readable 封闭集）。
    /// @return Result：成功持按 T 转换的值，失败持 Error。
    template <json_readable T>
    [[nodiscard]] auto as() const -> Result<T>;  // 自身
    /// @brief 严格读 Object 子键：键缺失 / 自身非 Object / 类型不符均返回结构化错误。
    /// @tparam T 严格读取的目标类型（限于 json_readable 封闭集）。
    /// @param key 对象键。
    /// @return Result：命中持按 T 转换的子值，否则持 Error。
    template <json_readable T>
    [[nodiscard]] auto get(std::string_view key) const -> Result<T>;  // Object 子键

    /// @brief 无模板族宽容读 Bool（免实例化，供热路径）：类型不符返回 nullopt。
    /// @return Type::Bool 时持值，否则 nullopt。
    [[nodiscard]] auto as_bool() const noexcept -> std::optional<bool>;
    /// @brief 宽容读 Int：Int、UInt（≤INT64_MAX）或整值 Double（int64 域内）时返回值。
    /// @return 命中类型时持值，否则 nullopt。
    [[nodiscard]] auto as_int() const noexcept -> std::optional<std::int64_t>;
    /// @brief 宽容读 Double：Double 直取，Int/UInt 宽化为 double。
    /// @return 命中类型时持值，否则 nullopt。
    [[nodiscard]] auto as_double() const noexcept -> std::optional<double>;
    /// @brief 宽容读 String：String 时返回内容视图（别名存储）。
    /// @return 命中类型时持内容视图，否则 nullopt。
    [[nodiscard]] auto as_string() const noexcept -> std::optional<std::string_view>;
    /// @brief 宽容读 RawNumber：RawNumber 时返回原始字面量文本。
    /// @return 命中类型时持原始字面量，否则 nullopt。
    [[nodiscard]] auto as_raw_number() const noexcept -> std::optional<std::string_view>;

    /// @brief 写操作（仅 Object/Array 有效；其他类型上调用是未定义行为，debug 断言拦截）。
    /// @param key 对象键。
    /// @param v 新值：键存在则覆盖，保持首次插入位置。
    auto set(std::string_view key, Value v) -> void;
    /// @brief Array 追加元素（不提供 emplace_back；非 Array 上调用为 UB，debug 断言拦截）。
    /// @param v 待追加的值（移入）。
    auto push_back(Value v) -> void;
    /// @brief Array/Object 容量预留（批量构造热路径避免反复扩容）；其余类型无操作。
    /// @param n 预留的条目数。
    auto reserve(std::size_t n) -> void;
    /// @brief 清空：Array/Object 清为空容器；其余类型无操作。
    auto clear() noexcept -> void;
    /// @brief Object 删键。
    /// @param key 待删除的对象键。
    /// @return 命中并删除时 true；键不存在或非 Object 时 false。
    auto erase(std::string_view key) -> bool;
    /// @brief Array 删元素。
    /// @param index 待删除的元素下标。
    /// @return 命中并删除时 true；越界或非 Array 时 false。
    auto erase_at(std::size_t index) -> bool;

    /// @brief 迭代接口：Object 条目 range。
    /// @return 只读 EntryRange（非 Object → 空 range）。
    [[nodiscard]] auto entries() const noexcept -> EntryRange;
    /// @brief 迭代接口：Array 首元素指针。
    /// @return Array 时首元素指针；非 Array 为 `nullptr`（与 end() 相同，成空区间）。
    [[nodiscard]] auto begin() const noexcept -> const Value *;
    /// @brief 迭代接口：Array 尾后指针。
    /// @return Array 时 data() + size()；非 Array 为 `nullptr`。
    [[nodiscard]] auto end() const noexcept -> const Value *;

    /// @brief 相等比较：variant 同 Type 严格比较（跨数值类型不等，以保 diff 语义）。
    /// @param a 左操作数。
    /// @param b 右操作数。
    /// @return 类型与值均相等时为 true。
    [[nodiscard]] friend auto operator==(const Value &a, const Value &b) -> bool { return a.data_ == b.data_; }
    /// @brief 不等比较（operator== 取反）。
    /// @param a 左操作数。
    /// @param b 右操作数。
    /// @return 类型或值不等时为 true。
    [[nodiscard]] friend auto operator!=(const Value &a, const Value &b) -> bool { return !(a == b); }

  private:
    // 封闭读类型集的统一读取（宽容、无错误对象）；实现见 value.cpp。
    // 供 as_or / as / get 共用，避免逐类型重复逻辑。
    template <json_readable T>
    [[nodiscard]] auto read_impl() const noexcept -> std::optional<T>;

    // 内部存储。alternative index **必须**与 Type 枚举量同构：
    // 0=Null 1=Bool 2=Int 3=UInt 4=Double 5=RawNumber 6=String 7=Array 8=Object。
    std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, RawNumber, std::string, Array, Object>
        data_;
};

/// @brief Object 条目只读 range（非拥有）：从 Object 指针产出 Entry 迭代区间；空指针 → 空区间。
class EntryRange {
  public:
    /// @brief Object 条目正向迭代器（裸指针包装，平凡可拷贝）。
    class Iterator {
      public:
        // 空态迭代器（p_ = nullptr），作 range 端点哨兵（§13.5.1 豁免平凡 = default 升格为文档）。
        Iterator() noexcept = default;
        /// @brief 以条目存储指针构造。
        /// @param p 指向 `Object` 底层 pair 数组的指针。
        explicit Iterator(const std::pair<std::string, Value> *p) noexcept : p_(p) {}

        /// @brief 解引用当前条目。
        /// @return Entry 只读视图（键与子值均别名内部存储）。
        [[nodiscard]] auto operator*() const noexcept -> Entry;
        /// @brief 前置自增：前进到下一条目。
        /// @return 自引用。
        auto operator++() noexcept -> Iterator &;
        /// @brief 后置自增：返回自增前副本。
        /// @param tag 后置哑元标记，仅用于重载决议（C++ 惯例）。
        /// @return 自增前的迭代器副本。
        auto operator++(int tag) noexcept -> Iterator;
        /// @brief 指针相等比较。
        /// @param o 右操作数迭代器。
        /// @return 指向同一条目时为 true。
        [[nodiscard]] auto operator==(const Iterator &o) const noexcept -> bool { return p_ == o.p_; }
        /// @brief 指针不等比较。
        /// @param o 右操作数迭代器。
        /// @return 指向不同条目时为 true。
        [[nodiscard]] auto operator!=(const Iterator &o) const noexcept -> bool { return p_ != o.p_; }

      private:
        const std::pair<std::string, Value> *p_ = nullptr;
    };

    /// @brief 以对象存储构造 range。
    /// @param obj 非拥有的 Object 指针；nullptr 表示空 range。
    explicit EntryRange(const Object *obj) noexcept : obj_(obj) {}
    /// @brief 首条目迭代器。
    /// @return obj_ 非空时指向首条目，空 range 返回默认态迭代器。
    [[nodiscard]] auto begin() const noexcept -> Iterator;
    /// @brief 尾后迭代器。
    /// @return 越过末元素的迭代器（空 range 时与默认态相等）。
    [[nodiscard]] auto end() const noexcept -> Iterator;

  private:
    const Object *obj_ = nullptr;
};

/// @brief 解引用当前条目。
/// @return Entry 只读视图（键与子值均别名内部存储）。
inline auto EntryRange::Iterator::operator*() const noexcept -> Entry {
    return Entry{.key = p_->first, .value = p_->second};
}

/// @brief 前置自增（类外定义）：前进到下一条目。
/// @return 自引用。
inline auto EntryRange::Iterator::operator++() noexcept -> Iterator & {
    ++p_;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) 裸指针步进是零开销视图迭代器的设计本体
    return *this;
}

/// @brief 后置自增（类外定义）。
/// @param tag 后置哑元标记，仅用于重载决议（C++ 惯例）。
/// @return 自增前的迭代器副本。
inline auto EntryRange::Iterator::operator++([[maybe_unused]] int tag) noexcept -> Iterator {
    Iterator prev = *this;
    ++p_;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) 同前置自增
    return prev;
}

/// @brief 构造首条目迭代器（类外定义）。
/// @return obj_ 为空时返回默认态迭代器，否则指向首条目。
inline auto EntryRange::begin() const noexcept -> Iterator {
    return obj_ == nullptr ? Iterator{} : Iterator{obj_->data()};
}

/// @brief 构造尾后迭代器（类外定义）。
/// @return obj_ 为空时返回默认态迭代器，否则指向末元素之后。
inline auto EntryRange::end() const noexcept -> Iterator {
    return obj_ == nullptr ? Iterator{} : Iterator{std::to_address(obj_->end())};
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
/// @param input 文档全文（UTF-8 文本）。
/// @param opts 解析选项：超 `max_depth` → json-depth-exceeded；`validate_utf8` 开时无效序列判失败。
/// @return 成功持 Value；失败持结构化 Error（json-parse-error，含 line/column/offset）。
[[nodiscard]] auto parse(std::string_view input, ParseOptions opts = {}) -> Result<Value>;

// ============================================================================
// SAX 解析
// ============================================================================

/// @brief SAX 事件消费者（虚接口）。
/// @note 回调返回 `false` 表示消费者要求**提前终止**：`parse_sax` 随即停止解析并返回**成功**
///       ——这是消费者的主动决定而非错误。DOM 解析路径永不返回 `false`。
/// @warning 回调收到的 `string_view`（字符串解码结果 / 对象键 / 保真数字文本）**仅在本次回调
///          期间有效**，底层是可复用的临时缓冲；需要留存请自行拷贝。
/// @note Thread: 回调在调用 `parse_sax` 的线程上同步执行
class SaxHandler {
  public:
    SaxHandler() = default;  // 默认构造：消费者仅需实现回调
    virtual ~SaxHandler() = default;  // 虚析构：支持经基类引用派发

    // 拷贝与移动特殊成员全部删除：parse_sax 全程以基类引用持有 handler，复制/搬移无意义
    SaxHandler(const SaxHandler &) = delete;
    SaxHandler &operator=(const SaxHandler &) = delete;
    SaxHandler(SaxHandler &&) = delete;
    SaxHandler &operator=(SaxHandler &&) = delete;

    /// @brief 回调：读到 null 字面量。
    /// @return true 继续解析；false 提前终止（见类注释）。
    virtual auto on_null() -> bool = 0;
    /// @brief 回调：读到 true/false 字面量。
    /// @param value 读到的布尔值。
    /// @return true 继续解析；false 提前终止（见类注释）。
    virtual auto on_bool(bool value) -> bool = 0;
    /// @brief 回调：读到落在 int64 域的整数。
    /// @param value 整数值。
    /// @return true 继续解析；false 提前终止（见类注释）。
    virtual auto on_int(std::int64_t value) -> bool = 0;
    /// @brief 回调：读到超 int64、落 uint64 域的正整数。
    /// @param value 无符号整数值。
    /// @return true 继续解析；false 提前终止（见类注释）。
    virtual auto on_uint(std::uint64_t value) -> bool = 0;
    /// @brief 回调：读到含小数/指数且 double 往返保真的数字。
    /// @param value 浮点值。
    /// @return true 继续解析；false 提前终止（见类注释）。
    virtual auto on_double(double value) -> bool = 0;
    /// @brief 回调：读到域外或往返失真的保真数字。
    /// @param digits 原始数字字面量文本（仅回调期有效）。
    /// @return true 继续解析；false 提前终止（见类注释）。
    virtual auto on_raw_number(std::string_view digits) -> bool = 0;
    /// @brief 回调：读到字符串（解码结果）。
    /// @param decoded 解码后的 UTF-8 文本（仅回调期有效）。
    /// @return true 继续解析；false 提前终止（见类注释）。
    virtual auto on_string(std::string_view decoded) -> bool = 0;
    /// @brief 回调：进入数组（左方括号）。
    /// @return true 继续解析；false 提前终止（见类注释）。
    virtual auto on_array_start() -> bool = 0;
    /// @brief 回调：退出数组（右方括号）。
    /// @param count 已产出的元素数。
    /// @return true 继续解析；false 提前终止（见类注释）。
    virtual auto on_array_end(std::size_t count) -> bool = 0;
    /// @brief 回调：进入对象（左花括号）。
    /// @return true 继续解析；false 提前终止（见类注释）。
    virtual auto on_object_start() -> bool = 0;
    /// @brief 回调：读到对象键（解码后）。
    /// @param key 解码后的键文本（仅回调期有效）。
    /// @return true 继续解析；false 提前终止（见类注释）。
    virtual auto on_object_key(std::string_view key) -> bool = 0;
    /// @brief 回调：退出对象（右花括号）。
    /// @param count 已产出的键值对数。
    /// @return true 继续解析；false 提前终止（见类注释）。
    virtual auto on_object_end(std::size_t count) -> bool = 0;
};

/// @brief SAX 模式解析（不构建 DOM）。
/// @note 与 `parse` 共用同一字符级引擎（DOM 出口即「本引擎 + 内置 builder」），行为不分叉。
/// @param input 文档全文（UTF-8 文本）。
/// @param handler 事件消费者；各回调返回 false 即提前终止（见类注释）。
/// @param opts 解析选项（同 `parse`）。
/// @return 失败持结构化 Error（与 `parse` 同口径）；成功涵盖「文档解析完毕」与「消费者提前终止」。
[[nodiscard]] auto parse_sax(std::string_view input, SaxHandler &handler, ParseOptions opts = {}) -> Result<void>;

// ============================================================================
// 序列化
// ============================================================================

/// @brief 序列化选项。
struct DumpOptions {
    int indent = -1;  ///< <0 紧凑单行；≥0 每层缩进空格数（dump(2) 对应 indent=2）
    bool ensure_ascii = false;  ///< true 时非 ASCII 转 \\uXXXX（含代理对编码）
};

/// @brief 序列化为 UTF-8 JSON 文本。
/// @param v 待序列化的值。
/// @param opts 序列化选项（缩进 / ensure_ascii 转义）。
/// @return 失败：值内含 NaN/Inf Double（RFC 8259 无对应文本，json-value-not-serializable）。
[[nodiscard]] auto dump(const Value &v, DumpOptions opts = {}) -> Result<std::string>;

/// @brief 追加式序列化（响应帧热路径复用缓冲，避免每帧分配）。
/// @param v 待序列化的值。
/// @param out 输出缓冲，新文本追加到其末尾。
/// @param opts 序列化选项（同 `dump`）。
/// @return Result<void>：失败条件与 `dump` 相同（NaN/Inf → json-value-not-serializable）。
auto dump_into(const Value &v, std::string &out, DumpOptions opts = {}) -> Result<void>;

// ============================================================================
// JSON Pointer（RFC 6901 最小集）
// ============================================================================

/// @brief 按 RFC 6901 Pointer 寻址（只读）。
/// @param root 被寻址的文档根。
/// @param pointer 形如 `"/a/0/b"`；**空串指向 `root` 自身**。段内 `~1` 还原为 `/`、`~0` 还原为 `~`。
/// @return 命中返回子值指针；未命中（段不存在 / 数组索引越界 / 段与值的类型不符）或 `pointer`
///         语法非法（非空且不以 `/` 开头）一律返回 `nullptr`。与 `Value::at` 同风格：**不抛异常**，
///         调用方必须自判空。
[[nodiscard]] auto find_pointer(const Value &root, std::string_view pointer) -> const Value *;
/// @brief 按 RFC 6901 Pointer 寻址（可变视图，供就地写）。
/// @param root 被寻址的文档根。
/// @param pointer 语法与只读重载一致。
/// @return 命中返回可写子值指针；未命中 / 语法非法返回 `nullptr`（不抛异常）。
[[nodiscard]] auto find_pointer(Value &root, std::string_view pointer) -> Value *;

/// @brief 写路径寻址：自动补齐缺失的中间容器（按**下一段**形态决定建 Object 还是 Array）。
/// @note 末段落在数组上时，段为 `-`（RFC 6901 追加记号）或数字等于当前长度均按**追加**处理。
/// @param root 被寻址的文档根（缺失中间容器会写入它）。
/// @param pointer 段路径，语法同 `find_pointer`。
/// @return 成功返回可写槽位指针；失败持结构化 Error——`pointer` 语法非法落 `json-parse-error`，
///         段与值的类型不符（对非容器取子项 / 对数组用非数字段）或数组索引越界落 `json-type-mismatch`。
[[nodiscard]] auto resolve_for_write(Value &root, std::string_view pointer) -> Result<Value *>;

/// @brief 按 Pointer 删除末段所指成员（对应 `Value::erase` / `Value::erase_at` 的路径化形式）。
/// @param root 被删除操作作用的文档根。
/// @param pointer 段路径；空串（指向根自身）恒失败——根不可删除。
/// @return 成功时 bool 表示**是否命中**（未命中不是错误，返回 `false`）；失败条件与
///         `resolve_for_write` 相同。
[[nodiscard]] auto erase_pointer(Value &root, std::string_view pointer) -> Result<bool>;

}  // namespace aurora::json
