// =============================================================================
// lsp_schema.h — Schema model layer of the LSP analyzer (header-only, decoupled from the library).
// -----------------------------------------------------------------------------
// Component / enum / property schemas for declarative UI, plus pure functions such as lookup and
// Props-suffix stripping. Consumed by lsp_document.h (parsing) and lsp_features.h
// (completion / hover / diagnostics / code actions).
// =============================================================================

#pragma once

#include <string>
#include <vector>

namespace aurora::tools { // tool namespace, avoids clashing with the main aurora namespace

// ----------------------------- schema model -----------------------------------
struct PropSchema {
    std::string name;
    std::string type;
    std::string default_value; // stringified default value (may be empty)
    bool required = false;
    std::string note;
};

struct EnumSchema {
    std::string name;
    std::vector<std::string> values;
};

struct ComponentSchema {
    std::string type; // widget type name (without the Props suffix)
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
    // Strip the Props suffix and resolve to the widget type; returning empty means not a widget
    // (it may be an enum, etc.).
    [[nodiscard]] static auto strip_props(const std::string &t) -> std::string {
        if (t.size() > 5 && t.ends_with("Props")) {
            return t.substr(0, t.size() - 5);
        }
        return t;
    }
    // Whether this is a known widget type (including the Props form).
    [[nodiscard]] auto is_known_component(const std::string &t) const -> bool {
        return find_component(strip_props(t)) != nullptr;
    }
    [[nodiscard]] auto is_known_enum(const std::string &t) const -> bool { return find_enum(t) != nullptr; }
};

} // namespace aurora::tools
