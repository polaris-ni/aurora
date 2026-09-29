#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "aurora/core/json.h"
#include "aurora/core/platform.h"

namespace aurora::json {

namespace {

/// @brief 错误消息用的类型名（与 Type 枚举量同名，便于 AI / 日志识别）。
[[nodiscard]] auto type_name(Type t) noexcept -> const char * {
    switch (t) {
        case Type::Null:
            return "null";
        case Type::Bool:
            return "bool";
        case Type::Int:
            return "int";
        case Type::UInt:
            return "uint";
        case Type::Double:
            return "double";
        case Type::RawNumber:
            return "raw_number";
        case Type::String:
            return "string";
        case Type::Array:
            return "array";
        case Type::Object:
            return "object";
    }
    return "unknown";
}

/// @brief 封闭读类型集的期望类型名（供 json-type-mismatch 的 {expected} 键）。
template <typename T>
[[nodiscard]] constexpr auto expected_name() noexcept -> const char * {
    if constexpr (std::is_same_v<T, bool>) {
        return "bool";
    } else if constexpr (std::is_same_v<T, std::int64_t>) {
        return "int64";
    } else if constexpr (std::is_same_v<T, int>) {
        return "int32";
    } else if constexpr (std::is_same_v<T, std::uint64_t>) {
        return "uint64";
    } else if constexpr (std::is_same_v<T, std::size_t>) {
        return "size";
    } else if constexpr (std::is_same_v<T, float>) {
        return "float";
    } else if constexpr (std::is_same_v<T, double>) {
        return "double";
    } else if constexpr (std::is_same_v<T, std::string>) {
        return "string";
    } else if constexpr (std::is_same_v<T, std::string_view>) {
        return "string_view";
    } else {
        return "unknown";
    }
}

}  // namespace

// ============================================================================
// 构造
// ============================================================================

Value::Value() noexcept : data_(std::monostate{}) {}

Value::Value(std::nullptr_t) noexcept : data_(std::monostate{}) {}

// NOLINTNEXTLINE(bugprone-exception-escape) variant::emplace 实际无抛，检查器无法建模
Value::Value(bool b) noexcept : data_(std::monostate{}) { data_.emplace<bool>(b); }

Value::Value(const char *s) : data_(std::monostate{}) {
    if (s != nullptr) {
        data_.emplace<std::string>(s);
    }
}

Value::Value(std::string_view s) : data_(std::monostate{}) { data_.emplace<std::string>(std::string(s)); }

// variant::emplace 无 noexcept 规格（可能置 valueless），但替代项构造均无抛；检查器无法建模。
// NOLINTNEXTLINE(bugprone-exception-escape)
Value::Value(std::string s) noexcept : data_(std::monostate{}) { data_.emplace<std::string>(std::move(s)); }

auto Value::raw_number(std::string_view digits) -> Value {
    Value v;
    v.data_.emplace<RawNumber>(RawNumber{std::string(digits)});
    return v;
}

auto Value::array() -> Value {
    Value v;
    v.data_.emplace<Array>(Array{});
    return v;
}

auto Value::object() -> Value {
    Value v;
    v.data_.emplace<Object>(Object{});
    return v;
}

// ============================================================================
// 类型查询
// ============================================================================

auto Value::type() const noexcept -> Type { return static_cast<Type>(data_.index()); }

auto Value::is_null() const noexcept -> bool { return std::holds_alternative<std::monostate>(data_); }
auto Value::is_bool() const noexcept -> bool { return std::holds_alternative<bool>(data_); }
auto Value::is_int() const noexcept -> bool { return std::holds_alternative<std::int64_t>(data_); }
auto Value::is_uint() const noexcept -> bool { return std::holds_alternative<std::uint64_t>(data_); }
auto Value::is_double() const noexcept -> bool { return std::holds_alternative<double>(data_); }
auto Value::is_raw_number() const noexcept -> bool { return std::holds_alternative<RawNumber>(data_); }
auto Value::is_string() const noexcept -> bool { return std::holds_alternative<std::string>(data_); }
auto Value::is_array() const noexcept -> bool { return std::holds_alternative<Array>(data_); }
auto Value::is_object() const noexcept -> bool { return std::holds_alternative<Object>(data_); }

auto Value::is_number() const noexcept -> bool { return is_int() || is_uint() || is_double() || is_raw_number(); }
auto Value::is_integer() const noexcept -> bool { return is_int() || is_uint(); }

// ============================================================================
// 读：无模板族
// ============================================================================

auto Value::as_bool() const noexcept -> std::optional<bool> {  // NOLINT(bugprone-exception-escape)
    if (std::holds_alternative<bool>(data_)) {
        return std::get<bool>(data_);
    }
    return std::nullopt;
}

auto Value::as_int() const noexcept -> std::optional<std::int64_t> {  // NOLINT(bugprone-exception-escape)
    if (std::holds_alternative<std::int64_t>(data_)) {
        return std::get<std::int64_t>(data_);
    }
    if (std::holds_alternative<std::uint64_t>(data_)) {
        const auto u = std::get<std::uint64_t>(data_);
        if (u <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(u);
        }
        return std::nullopt;
    }
    if (std::holds_alternative<double>(data_)) {
        const double d = std::get<double>(data_);
        // int64 域 [-2^63, 2^63) 且为整数（2^63 精确可表示，故用半开区间避免溢出）
        if (d >= -9223372036854775808.0 && d < 9223372036854775808.0 && d == std::trunc(d)) {
            return static_cast<std::int64_t>(d);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

auto Value::as_double() const noexcept -> std::optional<double> {  // NOLINT(bugprone-exception-escape)
    if (std::holds_alternative<double>(data_)) {
        return std::get<double>(data_);
    }
    if (std::holds_alternative<std::int64_t>(data_)) {
        return static_cast<double>(std::get<std::int64_t>(data_));
    }
    if (std::holds_alternative<std::uint64_t>(data_)) {
        return static_cast<double>(std::get<std::uint64_t>(data_));
    }
    return std::nullopt;
}

auto Value::as_string() const noexcept -> std::optional<std::string_view> {  // NOLINT(bugprone-exception-escape)
    if (std::holds_alternative<std::string>(data_)) {
        return std::string_view{std::get<std::string>(data_)};
    }
    return std::nullopt;
}

auto Value::as_raw_number() const noexcept -> std::optional<std::string_view> {  // NOLINT(bugprone-exception-escape)
    if (std::holds_alternative<RawNumber>(data_)) {
        return std::string_view{std::get<RawNumber>(data_).text};
    }
    return std::nullopt;
}

// ============================================================================
// 读：封闭模板成员共用实现
// ============================================================================

template <json_readable T>
auto Value::read_impl() const noexcept -> std::optional<T> {  // NOLINT(bugprone-exception-escape)
    if constexpr (std::is_same_v<T, bool>) {
        if (std::holds_alternative<bool>(data_)) {
            return std::get<bool>(data_);
        }
        return std::nullopt;
    } else if constexpr (std::is_same_v<T, std::int64_t>) {
        return as_int();
    } else if constexpr (std::is_same_v<T, int>) {
        if (const auto r = as_int(); r.has_value() && *r >= -2147483648LL && *r <= 2147483647LL) {
            return static_cast<int>(*r);
        }
        return std::nullopt;
    } else if constexpr (std::is_same_v<T, std::uint64_t>) {
        if (std::holds_alternative<std::uint64_t>(data_)) {
            return std::get<std::uint64_t>(data_);
        }
        if (std::holds_alternative<std::int64_t>(data_)) {
            const auto i = std::get<std::int64_t>(data_);
            if (i >= 0) {
                return static_cast<std::uint64_t>(i);
            }
            return std::nullopt;
        }
        if (std::holds_alternative<double>(data_)) {
            const double d = std::get<double>(data_);
            // uint64 域 [0, 2^64) 且为整数
            if (d >= 0.0 && d < 18446744073709551616.0 && d == std::trunc(d)) {
                return static_cast<std::uint64_t>(d);
            }
        }
        return std::nullopt;
    } else if constexpr (std::is_same_v<T, std::size_t>) {
        // 64 位目标上 size_t 与 uint64_t 同型，上面的 uint64_t 分支先行命中，本分支不可达；
        // 仅在 32 位 size_t 目标（独立于 uint64_t 的类型）上需要此窄化转换。
        const auto u = read_impl<std::uint64_t>();
        if (u.has_value() && *u <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            return static_cast<std::size_t>(*u);
        }
        return std::nullopt;
    } else if constexpr (std::is_same_v<T, float>) {
        if (const auto d = as_double(); d.has_value()) {
            return static_cast<float>(*d);
        }
        return std::nullopt;
    } else if constexpr (std::is_same_v<T, double>) {
        return as_double();
    } else if constexpr (std::is_same_v<T, std::string_view>) {
        return as_string();
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (const auto s = as_string(); s.has_value()) {
            return std::string(*s);
        }
        return std::nullopt;
    } else {
        static_assert(!std::is_same_v<T, T>, "unhandled json_readable type");
    }
}

template <json_readable T>
auto Value::as_or(T fallback) const noexcept -> T {
    if (const auto v = read_impl<T>(); v.has_value()) {
        return *v;
    }
    return fallback;
}

template <json_readable T>
auto Value::as_or(std::string_view key, const T &fallback) const noexcept -> T {
    const Value *sub = find(key);
    return sub != nullptr ? sub->as_or<T>(fallback) : fallback;
}

template <json_readable T>
auto Value::as_or_at(std::size_t index, const T &fallback) const noexcept -> T {
    const Value *sub = at(index);
    return sub != nullptr ? sub->as_or<T>(fallback) : fallback;
}

template <json_readable T>
auto Value::as() const -> Result<T> {
    if (const auto v = read_impl<T>(); v.has_value()) {
        return Result<T>(*v);
    }
    ErrorParams params;
    params["expected"] = expected_name<T>();
    params["actual"] = type_name(type());
    return Result<T>(make_error(ErrorCode::JsonTypeMismatch, params));
}

template <json_readable T>
auto Value::get(std::string_view key) const -> Result<T> {
    const Value *sub = find(key);
    if (sub != nullptr) {
        return sub->as<T>();
    }
    ErrorParams params;
    params["expected"] = expected_name<T>();
    params["actual"] = "missing";
    return Result<T>(make_error(ErrorCode::JsonTypeMismatch, params));
}

// ============================================================================
// 读：指针路径
// ============================================================================

auto Value::find(std::string_view key) noexcept -> Value * {  // NOLINT(bugprone-exception-escape)
    if (!std::holds_alternative<Object>(data_)) {
        return nullptr;
    }
    auto &obj = std::get<Object>(data_);
    for (auto &kv : obj) {
        if (std::string_view{kv.first} == key) {
            return &kv.second;
        }
    }
    return nullptr;
}

auto Value::find(std::string_view key) const noexcept -> const Value * {  // NOLINT(bugprone-exception-escape)
    if (!std::holds_alternative<Object>(data_)) {
        return nullptr;
    }
    const auto &obj = std::get<Object>(data_);
    for (const auto &kv : obj) {
        if (std::string_view{kv.first} == key) {
            return &kv.second;
        }
    }
    return nullptr;
}

auto Value::at(std::size_t index) noexcept -> Value * {  // NOLINT(bugprone-exception-escape)
    if (!std::holds_alternative<Array>(data_)) {
        return nullptr;
    }
    auto &arr = std::get<Array>(data_);
    return index < arr.size() ? &arr[index] : nullptr;
}

auto Value::at(std::size_t index) const noexcept -> const Value * {  // NOLINT(bugprone-exception-escape)
    if (!std::holds_alternative<Array>(data_)) {
        return nullptr;
    }
    const auto &arr = std::get<Array>(data_);
    return index < arr.size() ? &arr[index] : nullptr;
}

// 键族 at 与索引族 at 同名重载、语义一致（缺失 / 越界一律 nullptr），实现直接复用 find。
auto Value::at(std::string_view key) noexcept -> Value * { return find(key); }

auto Value::at(std::string_view key) const noexcept -> const Value * { return find(key); }

auto Value::contains(std::string_view key) const noexcept -> bool { return find(key) != nullptr; }

auto Value::size() const noexcept -> std::size_t {  // NOLINT(bugprone-exception-escape)
    if (std::holds_alternative<Array>(data_)) {
        return std::get<Array>(data_).size();
    }
    if (std::holds_alternative<Object>(data_)) {
        return std::get<Object>(data_).size();
    }
    if (std::holds_alternative<std::string>(data_)) {
        return std::get<std::string>(data_).size();
    }
    if (std::holds_alternative<RawNumber>(data_)) {
        return std::get<RawNumber>(data_).text.size();
    }
    return 0;
}

auto Value::empty() const noexcept -> bool {
    if (std::holds_alternative<std::monostate>(data_)) {
        return true;
    }
    if (std::holds_alternative<Array>(data_) || std::holds_alternative<Object>(data_) ||
        std::holds_alternative<std::string>(data_) || std::holds_alternative<RawNumber>(data_)) {
        return size() == 0;
    }
    return false;
}

// ============================================================================
// 写
// ============================================================================

auto Value::set(std::string_view key, Value v) -> void {
    assert(std::holds_alternative<Object>(data_) && "set() requires an Object value");
    auto &obj = std::get<Object>(data_);
    for (auto &kv : obj) {
        if (std::string_view{kv.first} == key) {
            kv.second = std::move(v);  // 覆盖：保持首次插入位置
            return;
        }
    }
    obj.emplace_back(std::string(key), std::move(v));
}

auto Value::push_back(Value v) -> void {
    assert(std::holds_alternative<Array>(data_) && "push_back() requires an Array value");
    std::get<Array>(data_).push_back(std::move(v));
}

auto Value::reserve(std::size_t n) -> void {
    if (std::holds_alternative<Array>(data_)) {
        std::get<Array>(data_).reserve(n);
        return;
    }
    if (std::holds_alternative<Object>(data_)) {
        std::get<Object>(data_).reserve(n);
    }
}

auto Value::clear() noexcept -> void {  // NOLINT(bugprone-exception-escape)
    if (std::holds_alternative<Array>(data_)) {
        std::get<Array>(data_).clear();
        return;
    }
    if (std::holds_alternative<Object>(data_)) {
        std::get<Object>(data_).clear();
    }
}

auto Value::erase(std::string_view key) -> bool {
    if (!std::holds_alternative<Object>(data_)) {
        return false;
    }
    auto &obj = std::get<Object>(data_);
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (std::string_view{it->first} == key) {
            obj.erase(it);
            return true;
        }
    }
    return false;
}

auto Value::erase_at(std::size_t index) -> bool {
    if (!std::holds_alternative<Array>(data_)) {
        return false;
    }
    auto &arr = std::get<Array>(data_);
    if (index >= arr.size()) {
        return false;
    }
    arr.erase(arr.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

// ============================================================================
// 迭代
// ============================================================================

auto Value::entries() const noexcept -> EntryRange {  // NOLINT(bugprone-exception-escape)
    if (std::holds_alternative<Object>(data_)) {
        return EntryRange{&std::get<Object>(data_)};
    }
    return EntryRange{nullptr};
}

auto Value::begin() const noexcept -> const Value * {  // NOLINT(bugprone-exception-escape)
    if (std::holds_alternative<Array>(data_)) {
        return std::get<Array>(data_).data();
    }
    return nullptr;
}

auto Value::end() const noexcept -> const Value * {  // NOLINT(bugprone-exception-escape)
    if (std::holds_alternative<Array>(data_)) {
        const auto &arr = std::get<Array>(data_);
        return std::to_address(arr.end());
    }
    return nullptr;
}

// ============================================================================
// 封闭读类型集的显式实例化
// ----------------------------------------------------------------------------
// 头内不留模板实现，故此处为全部封闭特化提供唯一实例化点。std::size_t 与 std::uint64_t 在 64 位
// Linux/Windows 目标上同型（重复显式实例化非法，故跳过）；但在 macOS（libc++，AURORA_PLATFORM_MACOS）
// 上 std::size_t == unsigned long 而 std::uint64_t == unsigned long long，是两种不同类型，必须独立
// 实例化，否则 TextInput::deserialize_props 等引用 as_or<unsigned long> 会链接失败。32 位目标
// （wasm32/x86/arm32，AURORA_BIT_32）同样为异型，需独立实例化。其余 64 位目标二者同型，跳过以避免
// 重复显式实例化错误。
// ============================================================================

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) 显式实例化清单无法用模板函数表达
#define AURORA_JSON_INSTANTIATE_STRICT(T)    \
    template Result<T> Value::as<T>() const; \
    template Result<T> Value::get<T>(std::string_view key) const;

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) 同上
#define AURORA_JSON_INSTANTIATE_READ(T)                                                 \
    template T Value::as_or<T>(T fallback) const noexcept;                              \
    template T Value::as_or<T>(std::string_view key, const T &fallback) const noexcept; \
    template T Value::as_or_at<T>(std::size_t index, const T &fallback) const noexcept; \
    AURORA_JSON_INSTANTIATE_STRICT(T)

AURORA_JSON_INSTANTIATE_READ(bool)
AURORA_JSON_INSTANTIATE_READ(int)
AURORA_JSON_INSTANTIATE_READ(std::int64_t)
AURORA_JSON_INSTANTIATE_READ(std::uint64_t)
#if defined(AURORA_BIT_32) || defined(AURORA_PLATFORM_MACOS)
AURORA_JSON_INSTANTIATE_READ(std::size_t)
#endif
AURORA_JSON_INSTANTIATE_READ(float)
AURORA_JSON_INSTANTIATE_READ(double)
AURORA_JSON_INSTANTIATE_READ(std::string)
AURORA_JSON_INSTANTIATE_READ(std::string_view)

#undef AURORA_JSON_INSTANTIATE_READ
#undef AURORA_JSON_INSTANTIATE_STRICT

}  // namespace aurora::json
