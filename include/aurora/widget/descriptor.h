#pragma once

#include <array>
#include <string>
#include <vector>

#include "aurora/core/diagnostics.h"
#include "aurora/core/result.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/widget/props_io.h" // Json

namespace aurora {

/// @brief 单个属性的元数据描述（规格附录 B）。
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
/// @note Rebuildable: no
struct PropDescriptor {
    std::string name;          ///< 属性键名（序列化键），如 "label"
    std::string type;          ///< C++ 类型名，如 "LocalizedString"
    std::string default_value; ///< 字符串化默认值，如 "\"\""
    bool required = false;     ///< 是否必填
    std::string note;          ///< 可选说明（人类/AI 可读）

    // ---- JSON Schema 约束字段（编译期零开销，仅 Schema 生成时读取） ----
    std::string json_type;                   ///< JSON Schema 类型（"string"/"number"/"boolean"/"array"/"object"）
    std::vector<std::string> enum_values;    ///< 枚举合法值列表（如 ["Left","Right","Center"]）
    std::string min_value;                   ///< 数值最小（如 "0"）
    std::string max_value;                   ///< 数值最大（如 "100"）
    std::string pattern;                     ///< 字符串格式（如 "color-rgba"）
    std::string constraint;                  ///< 自由约束描述（如 "corner_radius >= 0"）
    std::vector<std::string> requires_props; ///< 属性依赖（如 border_color 要求 border_width > 0）
    std::vector<std::string> conflicts_with; ///< 属性互斥
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
    std::string name;                       ///< 控件类型名，如 "Button"
    std::string ns = "aurora";              ///< 命名空间
    std::vector<PropDescriptor> properties; ///< 属性列表
    std::vector<std::string> events;        ///< 事件/回调名列表，如 ["on_click"]
    std::string children_policy;            ///< 子节点策略："none" | "single" | "multiple"

    // ---- Schema 扩展字段（可选，空时不输出） ----
    std::vector<std::string> allowed_child_types; ///< 合法子类型（空 = 任意）
    std::vector<std::string> invariants;          ///< 控件级不变量描述
    std::vector<std::string> examples;            ///< 构造示例代码
};

/// @brief 把 PropDescriptor 序列化为 JSON 对象。
[[nodiscard]] auto descriptor_to_json(const PropDescriptor &p) -> Json;

/// @brief 把 WidgetDescriptor 序列化为 JSON 对象（供 describe_component / gen_api 消费）。
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
template<typename T> auto validate_prop(const Json &j, const PropDescriptor &desc) -> Result<T>;

// ---- Color 特化：数组长度 >= 4，值在 0-255 ----
template<> inline auto validate_prop<Color>(const Json &j, const PropDescriptor & /*desc*/) -> Result<Color> {
    if (!j.is_array() || j.size() < 4) {
        return make_error(ErrorCode::WidgetInvalidProp, "Color expects array of >= 4 numbers [r,g,b,a]");
    }
    for (std::size_t i = 0; i < 4; ++i) {
        if (!j[i].is_number()) {
            return make_error(ErrorCode::WidgetInvalidProp,
                              "Color component[" + std::to_string(i) + "] must be a number");
        }
        const int v = j[i].get<int>();
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
inline auto parse_constraint_float(const std::string &s, float &out) -> bool {
    try {
        out = std::stof(s);
        return true;
    } catch (...) {
        return false;
    }
}

inline auto parse_constraint_int(const std::string &s, int &out) -> bool {
    try {
        out = std::stoi(s);
        return true;
    } catch (...) {
        return false;
    }
}

template<> inline auto validate_prop<float>(const Json &j, const PropDescriptor &desc) -> Result<float> {
    if (!j.is_number()) {
        return make_error(ErrorCode::WidgetInvalidProp,
                          "Property '" + desc.name + "' expects number, got " + std::string(j.type_name()));
    }
    const float v = j.get<float>();
    if (!desc.min_value.empty()) {
        float lo = 0.0f;
        if (parse_constraint_float(desc.min_value, lo) && v < lo) {
            return make_error(ErrorCode::WidgetPropConstraintViolated, "Property '" + desc.name +
                                                                           "' must be >= " + desc.min_value + ", got " +
                                                                           std::to_string(v));
        }
    }
    if (!desc.max_value.empty()) {
        float hi = 0.0f;
        if (parse_constraint_float(desc.max_value, hi) && v > hi) {
            return make_error(ErrorCode::WidgetPropConstraintViolated, "Property '" + desc.name +
                                                                           "' must be <= " + desc.max_value + ", got " +
                                                                           std::to_string(v));
        }
    }
    return v;
}

// ---- int 特化：根据 desc.min_value / desc.max_value 检查范围 ----
template<> inline auto validate_prop<int>(const Json &j, const PropDescriptor &desc) -> Result<int> {
    if (!j.is_number()) {
        return make_error(ErrorCode::WidgetInvalidProp,
                          "Property '" + desc.name + "' expects integer, got " + std::string(j.type_name()));
    }
    const int v = j.get<int>();
    if (!desc.min_value.empty()) {
        int lo = 0;
        if (parse_constraint_int(desc.min_value, lo) && v < lo) {
            return make_error(ErrorCode::WidgetPropConstraintViolated, "Property '" + desc.name +
                                                                           "' must be >= " + desc.min_value + ", got " +
                                                                           std::to_string(v));
        }
    }
    if (!desc.max_value.empty()) {
        int hi = 0;
        if (parse_constraint_int(desc.max_value, hi) && v > hi) {
            return make_error(ErrorCode::WidgetPropConstraintViolated, "Property '" + desc.name +
                                                                           "' must be <= " + desc.max_value + ", got " +
                                                                           std::to_string(v));
        }
    }
    return v;
}

// ---- bool 特化：布尔类型检查 ----
template<> inline auto validate_prop<bool>(const Json &j, const PropDescriptor &desc) -> Result<bool> {
    if (!j.is_boolean()) {
        return make_error(ErrorCode::WidgetInvalidProp,
                          "Property '" + desc.name + "' expects boolean, got " + std::string(j.type_name()));
    }
    return j.get<bool>();
}

// ---- LocalizedString 特化：字符串类型检查 ----
template<>
inline auto validate_prop<LocalizedString>(const Json &j, const PropDescriptor &desc) -> Result<LocalizedString> {
    if (!j.is_string()) {
        return make_error(ErrorCode::WidgetInvalidProp,
                          "Property '" + desc.name + "' expects string, got " + std::string(j.type_name()));
    }
    return LocalizedString{ j.get<std::string>() };
}

// ---- Length 特化：value >= 0（当 kind 为 Fixed 或 Fraction 时） ----
template<> inline auto validate_prop<Length>(const Json &j, const PropDescriptor & /*desc*/) -> Result<Length> {
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (s == "auto" || s == "fill") {
            return json_to_length(j);
        }
        return make_error(ErrorCode::WidgetInvalidProp, "Length string must be 'auto' or 'fill', got '" + s + "'");
    }
    if (j.is_array() && j.size() == 2) {
        if (!j[1].is_number()) {
            return make_error(ErrorCode::WidgetInvalidProp, "Length value must be a number");
        }
        const float v = j[1].get<float>();
        if (v < 0.0f) {
            return make_error(ErrorCode::WidgetPropConstraintViolated,
                              "Length value must be >= 0, got " + std::to_string(v));
        }
        return json_to_length(j);
    }
    return make_error(ErrorCode::WidgetInvalidProp, "Length expects 'auto'/'fill' string or [kind, value] array");
}

// ---- EdgeInsets 特化：对象类型检查，各字段 >= 0 ----
template<> inline auto validate_prop<EdgeInsets>(const Json &j, const PropDescriptor & /*desc*/) -> Result<EdgeInsets> {
    if (!j.is_object()) {
        return make_error(ErrorCode::WidgetInvalidProp, "EdgeInsets expects an object {left,top,right,bottom}");
    }
    constexpr std::array<const char *, 4> fields = { "left", "top", "right", "bottom" };
    for (const char *f : fields) {
        if (j.contains(f)) {
            if (!j[f].is_number()) {
                return make_error(ErrorCode::WidgetInvalidProp, std::string("EdgeInsets.") + f + " must be a number");
            }
            if (j[f].get<float>() < 0.0f) {
                return make_error(ErrorCode::WidgetPropConstraintViolated,
                                  std::string("EdgeInsets.") + f + " must be >= 0");
            }
        }
    }
    return json_to_edge_insets(j);
}

// ---- validate_or_default<T>：反序列化约束助手 ----
/// @brief 用 validate_prop 校验 JSON 值；合法返回解析值，非法回退默认值并经 Diagnostics::degraded 上报。
/// @note 适用于 deserialize_props 中「非法输入安全降级」场景；严格模式下 degraded 升级为硬失败。
template<typename T> auto validate_or_default(const Json &j, const PropDescriptor &desc, T fallback) -> T {
    auto r = validate_prop<T>(j, desc);
    if (r) {
        return r.value();
    }
    Diagnostics::degraded("属性 '" + desc.name + "' 非法: " + r.error().message, desc.name, r.error().code);
    return fallback;
}

// ---- 枚举类型验证辅助 ----
/// @brief 验证字符串是否在 desc.enum_values 集合内。
/// @return 成功返回 true，失败填充 err_out。
inline auto validate_enum_string(const Json &j, const PropDescriptor &desc, Error &err_out) -> bool {
    if (!j.is_string()) {
        err_out = make_error(ErrorCode::WidgetInvalidProp,
                             "Enum property '" + desc.name + "' expects string, got " + std::string(j.type_name()));
        return false;
    }
    if (!desc.enum_values.empty()) {
        const std::string s = j.get<std::string>();
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

} // namespace aurora
