// =============================================================================
// lsp_schema.h — LSP 分析器的 Schema 模型层（header-only，与库解耦）。
// -----------------------------------------------------------------------------
// 声明式 UI 的组件 / 枚举 / 属性 Schema，以及查找与 Props 后缀剥离等纯函数。
// 由 lsp_document.h（解析）与 lsp_features.h（补全 / 悬停 / 诊断 / 代码动作）消费。
// =============================================================================

#pragma once

#include <string>
#include <vector>

namespace aurora::tools { // 工具命名空间，避免与 aurora 主命名空间冲突

// ----------------------------- Schema 模型 -----------------------------------
struct PropSchema {
    std::string name;
    std::string type;
    std::string default_value; // 字符串化默认值（可能为空）
    bool required = false;
    std::string note;
};

struct EnumSchema {
    std::string name;
    std::vector<std::string> values;
};

struct ComponentSchema {
    std::string type; // 控件类型名（不含 Props 后缀）
    std::string category;
    std::string children_policy;
    std::vector<PropSchema> props;
    std::vector<std::string> events;
    std::vector<std::string> examples;
};

struct Schema {
    std::vector<ComponentSchema> components;
    std::vector<EnumSchema> enums;

    [[nodiscard]] auto find_component(const std::string &t) const -> const ComponentSchema * {
        for (const auto &c : components) {
            if (c.type == t) {
                return &c;
            }
        }
        return nullptr;
    }
    [[nodiscard]] auto find_enum(const std::string &t) const -> const EnumSchema * {
        for (const auto &e : enums) {
            if (e.name == t) {
                return &e;
            }
        }
        return nullptr;
    }
    // 去掉 Props 后缀，解析为控件类型；返回空表示非控件（可能是枚举等）。
    [[nodiscard]] static auto strip_props(const std::string &t) -> std::string {
        if (t.size() > 5 && t.ends_with("Props")) {
            return t.substr(0, t.size() - 5);
        }
        return t;
    }
    // 是否是已知的控件类型（含 Props 形态）。
    [[nodiscard]] auto is_known_component(const std::string &t) const -> bool {
        return find_component(strip_props(t)) != nullptr;
    }
    [[nodiscard]] auto is_known_enum(const std::string &t) const -> bool { return find_enum(t) != nullptr; }
};

} // namespace aurora::tools
