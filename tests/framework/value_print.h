#pragma once

// ============================================================
// 测试框架（tests/framework/）—— 值打印内核
// ------------------------------------------------------------
// 断言失败时把实际值渲染成人类可读文本。判定次序（先专后泛，避免语义误判）：
//   bool → 字符 → 整数 → 浮点 → 枚举 → 字符串样（含 C 数组）→ 指针 →
//   optional 形 → pair/tuple → 容器（字节序列特化）→ operator<< → <unprintable>
//
// 枚举：走工具链单一来源 `tools/include/known_enums.h` 反查值名。该表按声明顺序登记，
// 故仅当「该型确已登记且底层值落在 [0, 登记数)」时才给名字，否则退回 `Type(值)`——
// 稀疏枚举（如 `FontWeight{100..900}`）因此走数值分支，不会误报。
//
// 定制点：`aurora::testing::ValuePrinter<T>` 可被测试 TU 显式特化，为自有类型给出
// 更友好的输出（框架正文不依赖任何 aurora 头；唯一例外是零成本纯宏头
// `aurora/core/platform.h`——平台判定 SSOT，零 `#include`、零运行时成本，用以取代裸平台宏）。
// ============================================================

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

#include "aurora/core/platform.h"
#include "known_enums.h"

namespace aurora::testing {

namespace detail {

/// @brief 类型是否可 `operator<<`（不可打印类型输出 `<unprintable 类型名>` 与表达式原文）。
template <typename T, typename = void>
struct IsStreamable : std::false_type {};

template <typename T>
struct IsStreamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

/// @brief 类型是否可迭代（容器分支的准入条件；C 数组另走专属分支）。
template <typename T, typename = void>
struct IsIterable : std::false_type {};

template <typename T>
struct IsIterable<
    T, std::void_t<decltype(std::begin(std::declval<const T&>())), decltype(std::end(std::declval<const T&>()))>>
    : std::true_type {};

template <typename T>
concept ByteLike =
    std::is_same_v<std::remove_cv_t<T>, std::uint8_t> || std::is_same_v<std::remove_cv_t<T>, std::int8_t> ||
    std::is_same_v<std::remove_cv_t<T>, char> || std::is_same_v<std::remove_cv_t<T>, std::byte>;

template <typename T>
concept Iterable = IsIterable<T>::value && !std::is_array_v<T>;

/// @brief 字符串样：`std::string` / `std::string_view` / `const char*`（C 数组另判）。
template <typename T>
concept StringLike =
    std::is_same_v<std::remove_cv_t<T>, std::string> || std::is_same_v<std::remove_cv_t<T>, std::string_view> ||
    (std::is_pointer_v<std::remove_cv_t<T>> &&
     std::is_same_v<std::remove_cv_t<std::remove_pointer_t<std::remove_cv_t<T>>>, char>);

/// @brief 把字符串渲染为带引号、转义控制字符的形式。
[[nodiscard]] inline auto quote(std::string_view text) -> std::string;

/// @brief 去掉类型名里的 `enum ` / `class ` 等修饰噪声。
[[nodiscard]] inline auto clean_type_name(std::string_view raw) -> std::string;

/// @brief 类型名（用于枚举标识与 `<unprintable>` 提示）。
template <typename T>
[[nodiscard]] auto type_name_str() -> std::string;

/// @brief 渲染枚举值：命中 `known_enums` 登记表给 `Type::Name`，否则给 `Type(底层值)`。
template <typename E>
[[nodiscard]] auto enum_text(E value) -> std::string;

/// @brief 字节序列渲染：十六进制 + 总长，超出阈值截断（整段像素数据会淹没报告）。
template <typename C>
[[nodiscard]] auto byte_dump(const C& bytes) -> std::string;

/// @brief 流式渲染（`operator<<` 可用时）。
template <typename T>
[[nodiscard]] auto via_stream(const T& value) -> std::string;

/// @brief 浮点渲染：给到可精确回读的位数，避免「诊断信息自身丢精度」。
template <typename T>
    requires std::is_floating_point_v<T>
[[nodiscard]] auto float_text(T value) -> std::string;

/// @brief 通用分支：所有未被显式特化的类型走这里。
template <typename T>
[[nodiscard]] auto generic_print(const T& value) -> std::string;

/// @brief 异常诊断：解释 `std::exception` 派生类的运行时类型与 `what()`。
[[nodiscard]] auto exception_text(const std::exception& error) -> std::string;

}  // namespace detail

/// @brief 值打印定制点：显式特化本模板即可为自有类型接管失败信息里的实际值。
template <typename T>
struct ValuePrinter {
    static auto print(const T& value) -> std::string { return detail::generic_print(value); }
};

/// @brief 主入口：把任意值渲染为诊断文本（经 `ValuePrinter`，故特化对元素级递归同样生效）。
template <typename T>
[[nodiscard]] auto print_value(const T& value) -> std::string {
    return ValuePrinter<std::remove_cv_t<T>>::print(value);
}

namespace detail {

/// @brief 两个操作数的对比诊断尾注。
template <typename A, typename B>
[[nodiscard]] auto compare_detail(const A& lhs, const B& rhs) -> std::string {
    return "\n    Which is: " + print_value(lhs) + " vs " + print_value(rhs);
}

template <typename T>
[[nodiscard]] auto via_stream(const T& value) -> std::string {
    std::ostringstream out;
    out << value;
    return out.str();
}

template <typename T>
    requires std::is_floating_point_v<T>
[[nodiscard]] auto float_text(T value) -> std::string {
    std::ostringstream out;
    out.precision(std::numeric_limits<T>::max_digits10);
    out << value;
    return out.str();
}

template <typename T>
[[nodiscard]] auto type_name_str() -> std::string {
#if defined(AURORA_COMPILER_GCC) || defined(AURORA_COMPILER_CLANG)
    // GCC/Clang：`... [with T = aurora::Alignment; ...]` / `... [T = aurora::Alignment]`
    return clean_type_name(__PRETTY_FUNCTION__);
#else
    // MSVC：`typeid` 名自带「enum/class/struct + 限定名」的可读形式。
    return clean_type_name(typeid(T).name());
#endif
}

template <typename E>
[[nodiscard]] auto enum_text(E value) -> std::string {
    static_assert(std::is_enum_v<E>, "enum_text 仅用于枚举类型");
    const auto numeric = static_cast<long long>(static_cast<std::underlying_type_t<E>>(value));
    const std::string qualified = type_name_str<E>();
    // 登记表的键是「属性描述符里的类型名」——限定名取最后一段。
    const auto last = qualified.find_last_of(": ");
    const std::string short_name = (last == std::string::npos) ? qualified : qualified.substr(last + 1);
    const auto registry = tools::known_enums();
    if (const auto entry = registry.find(short_name);
        entry != registry.end() && numeric >= 0 && static_cast<std::size_t>(numeric) < entry->second.size()) {
        return short_name + "::" + entry->second[static_cast<std::size_t>(numeric)];
    }
    return short_name + "(" + std::to_string(numeric) + ")";
}

template <typename C>
[[nodiscard]] auto byte_dump(const C& bytes) -> std::string {
    constexpr std::size_t max_bytes = 32;
    std::ostringstream out;
    out << "<" << std::distance(std::begin(bytes), std::end(bytes)) << " bytes:";
    std::size_t index = 0;
    for (const auto& byte : bytes) {
        if (index++ == max_bytes) {
            out << " ...";
            break;
        }
        out << ' ' << std::hex << std::uppercase << std::setw(2) << std::setfill('0');
        out << (static_cast<unsigned int>(static_cast<std::uint8_t>(byte)) & 0xFFU);
    }
    out << std::setfill(' ') << std::dec << '>';
    return out.str();
}

template <typename T>
[[nodiscard]] auto generic_print(const T& value) -> std::string {
    using Plain = std::remove_cv_t<T>;

    if constexpr (std::is_same_v<Plain, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_array_v<Plain>) {
        using Element = std::remove_cv_t<std::remove_extent_t<Plain>>;
        if constexpr (std::is_same_v<Element, char>) {
            return quote(std::string_view{static_cast<const char*>(value)});
        } else {
            return byte_dump(value);
        }
    } else if constexpr (std::is_same_v<Plain, char>) {
        return std::string{"'"} + (value == '\0' ? std::string{"\\0"} : std::string(1, value)) + "'";
    } else if constexpr (std::is_integral_v<Plain>) {
        // signed/unsigned char 不按字符处理：它们通常是字节或计数值。
        using Wide = std::conditional_t<std::is_signed_v<Plain>, long long, unsigned long long>;
        return std::to_string(static_cast<Wide>(value));
    } else if constexpr (std::is_floating_point_v<Plain>) {
        return float_text(value);
    } else if constexpr (std::is_enum_v<Plain>) {
        return enum_text(value);
    } else if constexpr (StringLike<Plain>) {
        if constexpr (std::is_pointer_v<Plain>) {
            if (value == nullptr) {
                return "nullptr";  // 构造 string_view(nullptr) 是未定义行为，先挡住。
            }
        }
        return quote(std::string_view{value});
    } else if constexpr (std::is_null_pointer_v<Plain>) {
        return "nullptr";
    } else if constexpr (std::is_pointer_v<Plain>) {
        if (value == nullptr) {
            return "nullptr";
        }
        std::ostringstream out;
        out << static_cast<const void*>(value);
        return out.str();
    } else if constexpr (requires {
                             value.has_value();
                             value.value();
                         }) {
        // std::optional 及同形类型（如 `Result`）：空态只报 «empty»，不触碰错误负载。
        if (!value.has_value()) {
            return std::string{"«empty»"};
        }
        return print_value(value.value());
    } else if constexpr (requires { std::get<0>(value); }) {
        std::string out = "(";
        std::size_t index = 0;
        std::apply([&out, &index](
                       const auto&... item) -> auto { ((out += (index++ == 0 ? "" : ", ") + print_value(item)), ...); },
                   value);
        out += ')';
        return out;
    } else if constexpr (Iterable<Plain>) {
        using Element = std::remove_cv_t<std::remove_reference_t<decltype(*std::begin(value))>>;
        if constexpr (ByteLike<Element>) {
            return byte_dump(value);
        } else if constexpr (std::is_same_v<Element, Plain>) {
            // 自引用退化容器：迭代产物与自身同型（如 nlohmann::json 标量——遍历恒产生自身引用），
            // 递归打印将无限展开直至栈溢出；退回流式输出（json 有 operator<<）。
            return via_stream(value);
        } else {
            std::string out = "{ ";
            std::size_t index = 0;
            for (const auto& item : value) {
                out += (index++ == 0 ? "" : ", ") + print_value(item);
            }
            out += (index == 0 ? "}" : " }");
            return out;
        }
    } else if constexpr (IsStreamable<Plain>::value) {
        return via_stream(value);
    } else {
        return "<unprintable " + type_name_str<Plain>() + ">";
    }
}

}  // namespace detail

inline auto detail::quote(std::string_view text) -> std::string {
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    for (const char c : text) {
        switch (c) {
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            default:
                out += (c >= '\x20' && c != '\x7f') ? c : '?';
        }
    }
    out += '"';
    return out;
}

inline auto detail::clean_type_name(std::string_view raw) -> std::string {
    // 匿名命名空间标记（GCC 16: {anonymous}；旧 GCC/Clang: (anonymous namespace)；
    // MSVC: `anonymous namespace'）——从类型名中移除，使测试 TU 内部链接类型的失败诊断
    // 与具名类型格式一致。
    auto strip = [](std::string text) -> std::string {
        for (const std::string_view noise : {"enum ", "class ", "struct ", "unsigned ",
                                             "{anonymous}::", "(anonymous namespace)::", "`anonymous namespace'::"}) {
            std::string::size_type pos = 0;
            while ((pos = text.find(noise, pos)) != std::string::npos) {
                text.erase(pos, noise.size());
            }
        }
        return text;
    };
    // GCC/Clang 的 `__PRETTY_FUNCTION__` 里先切出 `T = ` 与 `;` / `]` 之间的片段。
    if (const auto begin = raw.find("T = "); begin != std::string_view::npos) {
        auto rest = raw.substr(begin + 4);
        if (const auto end = rest.find_first_of(";]"); end != std::string_view::npos) {
            rest = rest.substr(0, end);
        }
        return strip(std::string{rest});
    }
    return strip(std::string{raw});
}

}  // namespace aurora::testing
