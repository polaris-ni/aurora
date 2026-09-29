#pragma once

#include <array>
#include <string>
#include <vector>

#include "aurora/core/diagnostics.h"
#include "aurora/core/result.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/widget/props_io.h"

/// @brief 控件自描述元数据：描述符结构（PropDescriptor / WidgetDescriptor）、JSON 序列化与属性约束验证。
namespace aurora {

/// @brief 单个属性的元数据描述（规格附录 B）。
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
/// @note Rebuildable: no
struct PropDescriptor {
    std::string name;  ///< 属性键名（序列化键），如 "label"
    std::string type;  ///< C++ 类型名，如 "LocalizedString"
    std::string default_value;  ///< 字符串化默认值，如 "\"\""
    bool required = false;  ///< 是否必填
    std::string note;  ///< 可选说明（人类/AI 可读）

    /// @brief 组：JSON Schema 约束字段（编译期零开销，仅 Schema 生成时读取）。
    std::string json_type;  ///< JSON Schema 类型（"string"/"number"/"boolean"/"array"/"object"）
    std::vector<std::string> enum_values;  ///< 枚举合法值列表（如 ["Left","Right","Center"]）
    std::string min_value;  ///< 数值最小（如 "0"）
    std::string max_value;  ///< 数值最大（如 "100"）
    std::string pattern;  ///< 字符串格式（如 "color-rgba"）
    std::string constraint;  ///< 自由约束描述（如 "corner_radius >= 0"）
    std::vector<std::string> requires_props;  ///< 属性依赖（如 border_color 要求 border_width > 0）
    std::vector<std::string> conflicts_with;  ///< 属性互斥
};

/// @brief 控件完整自描述元数据（规格附录 B：运行时自描述能力）。
///
/// 任何 Aurora 组件都能在运行时描述自己的完整 API：
/// @code
///   auto info = au::Button::describe_static();
///   // info.name == "Button"
///   // info.properties[0].name == "label", .type == "LocalizedString", .required == true
///   // info.events == ["on_click"]
///   // info.children_policy == "none"
/// @endcode
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
/// @note Rebuildable: no
struct WidgetDescriptor {
    std::string name;  ///< 控件类型名，如 "Button"
    std::string ns = "aurora";  ///< 命名空间
    std::vector<PropDescriptor> properties;  ///< 属性列表
    std::vector<std::string> events;  ///< 事件/回调名列表，如 ["on_click"]
    std::string children_policy;  ///< 子节点策略："none" | "single" | "multiple"

    /// @brief 组：Schema 扩展字段（可选，空时不输出）。
    std::vector<std::string> allowed_child_types;  ///< 合法子类型（空 = 任意）
    std::vector<std::string> invariants;  ///< 控件级不变量描述
    std::vector<std::string> examples;  ///< 构造示例代码
};

/// @brief 取 JSON 值的类型名（迁移自 nlohmann 的 `Value::type_name()`；`aurora::json::Value` 无该成员）。
/// @param j 待判别的 JSON 值。
/// @return 类型名字符串："null" / "boolean" / "number" / "string" / "array" / "object"。
inline auto json_type_name(const Json &j) -> std::string {
    if (j.is_null()) {
        return "null";
    }
    if (j.is_bool()) {
        return "boolean";
    }
    if (j.is_number()) {
        return "number";
    }
    if (j.is_string()) {
        return "string";
    }
    if (j.is_array()) {
        return "array";
    }
    if (j.is_object()) {
        return "object";
    }
    return "unknown";
}

/// @brief 把 PropDescriptor 序列化为 JSON 对象。
/// @param p 待序列化的属性描述符。
/// @return 含 name/type/default_value/required/note 及全部 Schema 约束字段的 JSON 对象。
[[nodiscard]] auto descriptor_to_json(const PropDescriptor &p) -> Json;

/// @brief 把 WidgetDescriptor 序列化为 JSON 对象（供 describe_component / gen_api 消费）。
/// @param d 待序列化的控件描述符。
/// @return 含 name/ns/properties/events/children_policy 及扩展字段的 JSON 对象。
[[nodiscard]] auto descriptor_to_json(const WidgetDescriptor &d) -> Json;

// ============================================================
// validate_prop<T>：属性值约束验证（specification/04-widget.md §2.2）
// ============================================================
// 定义在此而非 props_io.h，因需要 PropDescriptor 完整定义（props_io.h 被本头文件包含，
// 反向包含会造成循环依赖）。

/// @brief 属性值验证：根据 PropDescriptor 约束验证 JSON 值合法性。
/// @tparam T 目标 C++ 类型
/// @param j JSON 值
/// @param desc 属性描述符（含约束元数据）
/// @return 成功返回解析后的 T 值，失败返回 Error（含 ErrorCode）
/// @note Thread: main-thread only
/// @note Side-effects: none
template <typename T>
auto validate_prop(const Json &j, const PropDescriptor &desc) -> Result<T>;

// ---- Color 特化：数组长度 >= 4，值在 0-255 ----
/// @brief Color 属性验证：j 须为 [r,g,b,a] 数值数组且各分量在 [0,255]（忽略 desc 约束）。
/// @return 成功返回解析后的 Color，失败返回 WidgetInvalidProp 错误。
template <>
inline auto validate_prop<Color>(const Json &j, const PropDescriptor & /*desc*/) -> Result<Color> {
    if (!j.is_array() || j.size() < 4) {
        return make_error(ErrorCode::WidgetInvalidProp, "Color expects array of >= 4 numbers [r,g,b,a]");
    }
    for (std::size_t i = 0; i < 4; ++i) {
        if (!j.at(i)->is_number()) {
            return make_error(ErrorCode::WidgetInvalidProp,
                              "Color component[" + std::to_string(i) + "] must be a number");
        }
        const int v = j.at(i)->as_or<std::int32_t>(0);
        if (v < 0 || v > 255) {
            return make_error(ErrorCode::WidgetInvalidProp, "Color component[" + std::to_string(i) +
                                                                "] out of range [0,255], got " + std::to_string(v));
        }
    }
    return json_to_color(j);
}

// ---- float 特化：根据 desc.min_value / desc.max_value 检查范围 ----

/// @brief 解析描述符约束串。非法串返回 false 并视为「无此约束」：约束串由开发者注册，
///        但运行期裸调 stof/stoi 一旦遇到坏串即抛异常逃逸（经 Inspector/JSON 加载路径
///        可达），此处降级为跳过该约束，绝不终止进程。
/// @param s 约束原文（如 "0" / "100.5"）。
/// @param out 解析成功时写入的浮点值。
/// @return 解析成功为 true；s 非法（空串/含非数值字符/越界）为 false。
inline auto parse_constraint_float(const std::string &s, float &out) -> bool {
    try {
        out = std::stof(s);
        return true;
    } catch (...) {
        return false;
    }
}

/// @brief 解析描述符整数约束串（同 parse_constraint_float，改用 stoi）。
///        非法串返回 false 并视为「无此约束」，异常绝不逃逸出本函数。
/// @param s 约束原文（如 "0" / "255"）。
/// @param out 解析成功时写入的整数值。
/// @return 解析成功为 true；s 非法为 false。
inline auto parse_constraint_int(const std::string &s, int &out) -> bool {
    try {
        out = std::stoi(s);
        return true;
    } catch (...) {
        return false;
    }
}

/// @brief float 属性验证：j 须为数值，并按 desc.min_value / desc.max_value 检查范围。
/// @return 成功返回该数值，类型不符返回 WidgetInvalidProp，越界返回 WidgetPropConstraintViolated。
template <>
inline auto validate_prop<float>(const Json &j, const PropDescriptor &desc) -> Result<float> {
    if (!j.is_number()) {
        return make_error(ErrorCode::WidgetInvalidProp,
                          "Property '" + desc.name + "' expects number, got " + json_type_name(j));
    }
    const auto v = j.as_or<float>(0.0F);
    if (!desc.min_value.empty()) {
        float lo = 0.0F;
        if (parse_constraint_float(desc.min_value, lo) && v < lo) {
            return make_error(
                ErrorCode::WidgetPropConstraintViolated,
                "Property '" + desc.name + "' must be >= " + desc.min_value + ", got " + std::to_string(v));
        }
    }
    if (!desc.max_value.empty()) {
        float hi = 0.0F;
        if (parse_constraint_float(desc.max_value, hi) && v > hi) {
            return make_error(
                ErrorCode::WidgetPropConstraintViolated,
                "Property '" + desc.name + "' must be <= " + desc.max_value + ", got " + std::to_string(v));
        }
    }
    return v;
}

// ---- int 特化：根据 desc.min_value / desc.max_value 检查范围 ----
/// @brief int 属性验证：j 须为数值，并按 desc.min_value / desc.max_value 检查范围。
/// @return 成功返回该整数，类型不符返回 WidgetInvalidProp，越界返回 WidgetPropConstraintViolated。
template <>
inline auto validate_prop<int>(const Json &j, const PropDescriptor &desc) -> Result<int> {
    if (!j.is_number()) {
        return make_error(ErrorCode::WidgetInvalidProp,
                          "Property '" + desc.name + "' expects integer, got " + json_type_name(j));
    }
    const int v = j.as_or<std::int32_t>(0);
    if (!desc.min_value.empty()) {
        int lo = 0;
        if (parse_constraint_int(desc.min_value, lo) && v < lo) {
            return make_error(
                ErrorCode::WidgetPropConstraintViolated,
                "Property '" + desc.name + "' must be >= " + desc.min_value + ", got " + std::to_string(v));
        }
    }
    if (!desc.max_value.empty()) {
        int hi = 0;
        if (parse_constraint_int(desc.max_value, hi) && v > hi) {
            return make_error(
                ErrorCode::WidgetPropConstraintViolated,
                "Property '" + desc.name + "' must be <= " + desc.max_value + ", got " + std::to_string(v));
        }
    }
    return v;
}

// ---- bool 特化：布尔类型检查 ----
/// @brief bool 属性验证：j 须为 JSON 布尔值（desc 仅用于错误消息中的属性名）。
/// @return 成功返回该布尔值，类型不符返回 WidgetInvalidProp 错误。
template <>
inline auto validate_prop<bool>(const Json &j, const PropDescriptor &desc) -> Result<bool> {
    if (!j.is_bool()) {
        return make_error(ErrorCode::WidgetInvalidProp,
                          "Property '" + desc.name + "' expects boolean, got " + json_type_name(j));
    }
    return j.as_or<bool>(false);
}

// ---- LocalizedString 特化：字符串类型检查 ----
/// @brief LocalizedString 属性验证：j 须为字符串，命中后包装为 LocalizedString（desc 仅用于错误消息）。
/// @return 成功包装后的值，类型不符返回 WidgetInvalidProp 错误。
template <>
inline auto validate_prop<LocalizedString>(const Json &j, const PropDescriptor &desc) -> Result<LocalizedString> {
    if (!j.is_string()) {
        return make_error(ErrorCode::WidgetInvalidProp,
                          "Property '" + desc.name + "' expects string, got " + json_type_name(j));
    }
    return LocalizedString{j.as_or<std::string>("")};
}

// ---- Length 特化：value >= 0（当 kind 为 Fixed 或 Fraction 时） ----
/// @brief Length 属性验证：j 接受 "auto"/"fill" 字符串或 [kind, value] 数组（value 须 >= 0；忽略 desc）。
/// @return 成功返回解析后的 Length，形态不符返回 WidgetInvalidProp，负值返回 WidgetPropConstraintViolated。
template <>
inline auto validate_prop<Length>(const Json &j, const PropDescriptor & /*desc*/) -> Result<Length> {
    if (j.is_string()) {
        const auto s = j.as_or<std::string>("");
        if (s == "auto" || s == "fill") {
            return json_to_length(j);
        }
        return make_error(ErrorCode::WidgetInvalidProp, "Length string must be 'auto' or 'fill', got '" + s + "'");
    }
    if (j.is_array() && j.size() == 2) {
        if (!j.at(1)->is_number()) {
            return make_error(ErrorCode::WidgetInvalidProp, "Length value must be a number");
        }
        const auto v = j.at(1)->as_or<float>(0.0F);
        if (v < 0.0F) {
            return make_error(ErrorCode::WidgetPropConstraintViolated,
                              "Length value must be >= 0, got " + std::to_string(v));
        }
        return json_to_length(j);
    }
    return make_error(ErrorCode::WidgetInvalidProp, "Length expects 'auto'/'fill' string or [kind, value] array");
}

// ---- EdgeInsets 特化：对象类型检查，各字段 >= 0 ----
/// @brief EdgeInsets 属性验证：j 须为 {left,top,right,bottom} 对象，出现的字段须为非负数值（忽略 desc）。
/// @return 成功返回解析后的 EdgeInsets，非对象/非数值返回 WidgetInvalidProp，负值返回 WidgetPropConstraintViolated。
template <>
inline auto validate_prop<EdgeInsets>(const Json &j, const PropDescriptor & /*desc*/) -> Result<EdgeInsets> {
    if (!j.is_object()) {
        return make_error(ErrorCode::WidgetInvalidProp, "EdgeInsets expects an object {left,top,right,bottom}");
    }
    constexpr std::array<const char *, 4> fields = {"left", "top", "right", "bottom"};
    for (const char *f : fields) {
        if (j.contains(f)) {
            if (!j.at(f)->is_number()) {
                return make_error(ErrorCode::WidgetInvalidProp, std::string("EdgeInsets.") + f + " must be a number");
            }
            if (j.at(f)->as_or<float>(0.0F) < 0.0F) {
                return make_error(ErrorCode::WidgetPropConstraintViolated,
                                  std::string("EdgeInsets.") + f + " must be >= 0");
            }
        }
    }
    return json_to_edge_insets(j);
}

// ---- validate_or_default<T>：反序列化约束助手 ----
/// @brief 用 validate_prop 校验 JSON 值；合法返回解析值，非法回退默认值并经 Diagnostics::degraded 上报。
/// @tparam T 目标 C++ 类型（与 validate_prop 的模板实参一致）。
/// @param j 待校验的 JSON 值。
/// @param desc 属性描述符（含约束元数据与属性名）。
/// @param fallback 校验失败时回退的默认值。
/// @return 校验通过为解析出的 T 值；否则为 fallback。
/// @note 适用于 deserialize_props 中「非法输入安全降级」场景；严格模式下 degraded 升级为硬失败。
template <typename T>
auto validate_or_default(const Json &j, const PropDescriptor &desc, T fallback) -> T {
    auto r = validate_prop<T>(j, desc);
    if (r) {
        return r.value();
    }
    Diagnostics::degraded("Property '" + desc.name + "' is invalid: " + r.error().message, desc.name, r.error().code);
    return fallback;
}

// ---- 枚举类型验证辅助 ----
/// @brief 验证字符串是否在 desc.enum_values 集合内。
/// @param j JSON 值（须为字符串）。
/// @param desc 属性描述符（enum_values 为合法值集合；空集合时任意字符串均通过）。
/// @param err_out 失败时写入对应 Error（WidgetInvalidProp，消息含合法值列表）。
/// @return 值合法为 true；类型不符或不在集合内为 false。
inline auto validate_enum_string(const Json &j, const PropDescriptor &desc, Error &err_out) -> bool {
    if (!j.is_string()) {
        err_out = make_error(ErrorCode::WidgetInvalidProp,
                             "Enum property '" + desc.name + "' expects string, got " + json_type_name(j));
        return false;
    }
    if (!desc.enum_values.empty()) {
        const auto s = j.as_or<std::string>("");
        for (const auto &allowed : desc.enum_values) {
            if (s == allowed) {
                return true;
            }
        }
        std::string expected;
        for (std::size_t i = 0; i < desc.enum_values.size(); ++i) {
            if (i > 0) {
                expected += ", ";
            }
            expected += desc.enum_values[i];
        }
        err_out = make_error(ErrorCode::WidgetInvalidProp,
                             "Enum property '" + desc.name + "' got '" + s + "', expected one of: " + expected);
        return false;
    }
    return true;
}

}  // namespace aurora
