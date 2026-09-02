// ============================================================================
// au_lint_core.h — aurora_lint structural-check core (unit-testable)
// ----------------------------------------------------------------------------
// The lint logic of aurora_lint is extracted from the anonymous namespace in aurora_lint.cpp and
// exposed as an inline free function, shared by the aurora_lint main program and
// tests/test_au_lint.cpp, avoiding two duplicate and drifting copies of "tool implementation /
// test". The behavior is verbatim equivalent to the original anonymous-namespace implementation.
//
// Consumers must ensure components are registered first (tests call
// aurora::serialization::register_core_widgets(); the aurora_lint main program relies on global
// static registration), otherwise list_all_components() returns empty and every type is judged
// unknown.
// ============================================================================
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include <aurora/aurora.h>

namespace aurora::tools {

/// @brief A single lint finding (structured, machine-readable, aligned with au::ErrorSeverity).
struct LintFinding {
    ErrorSeverity severity = ErrorSeverity::Warning; // aligned with Diagnostic/Error
    std::string code;
    std::string message;
    std::string path;
};

constexpr int AURORA_MAX_LINT_DEPTH = 64;

/// @brief Collect the set of "known component types" and the "type -> known property keys" mapping.
/// Modifier nodes (header-only Modifier<T>, not in WidgetRegistry) are explicitly allowed to avoid false positives.
inline auto load_known_types() -> std::pair<std::set<std::string>, std::map<std::string, std::set<std::string>>> {
    auto types = list_all_components();
    std::set known(types.begin(), types.end());

    for (const char *m : { "Padding", "FlexWeight", "Background", "Border", "Clip", "SizeModifier", "Clickable",
                           "IgnorePointer", "Opacity", "Transform", "Align", "Center", "Expanded", "Flexible" }) {
        known.insert(m);
    }

    std::map<std::string, std::set<std::string>> props;
    for (const auto &t : types) {
        const Json schema = describe_component(t);
        // component_schema() returns property keys as an "array" (see serialization::component_schema),
        // so it is parsed as an array here (it was once misread as an object, making the unknown-prop guard useless).
        if (schema.contains("props") && schema["props"].is_array()) {
            std::set<std::string> keys;
            for (const auto &k : schema["props"]) {
                if (k.is_string()) {
                    keys.insert(k.get<std::string>());
                }
            }
            props[t] = std::move(keys);
        }
    }
    return { known, props };
}

inline void lint_node_impl(const Json &node, const std::string &path, int depth, const std::set<std::string> &known,
                           const std::map<std::string, std::set<std::string>> &props, std::vector<LintFinding> &out) {
    if (!node.is_object()) {
        out.push_back({ .severity = ErrorSeverity::Error,
                        .code = "node-not-object",
                        .message = "UI tree node must be an object",
                        .path = path });
        return;
    }
    if (!node.contains("type") || !node["type"].is_string()) {
        out.push_back({ .severity = ErrorSeverity::Error,
                        .code = "node-no-type",
                        .message = "Node missing string \"type\" field",
                        .path = path });
        return;
    }

    const std::string type = node["type"].get<std::string>();
    if (!known.contains(type)) {
        out.push_back({ .severity = ErrorSeverity::Warning,
                        .code = "unknown-type",
                        .message = "Unknown component/modifier type: " + type,
                        .path = path });
    }

    if (node.contains("props") && node["props"].is_object()) {
        if (auto pit = props.find(type); pit != props.end()) {
            for (auto it = node["props"].begin(); it != node["props"].end(); ++it) {
                if (!pit->second.contains(it.key())) {
                    out.push_back({ .severity = ErrorSeverity::Warning,
                                    .code = "unknown-prop",
                                    .message = "Type " + type + " has unknown property: " + it.key(),
                                    .path = path });
                }
            }
        }
    }

    if (node.contains("children")) {
        if (!node["children"].is_array()) {
            out.push_back({ .severity = ErrorSeverity::Error,
                            .code = "children-not-array",
                            .message = "\"children\" must be an array",
                            .path = path });
            return;
        }
        const auto &ch = node["children"];
        if (ch.empty()) {
            out.push_back({ .severity = ErrorSeverity::Info,
                            .code = "empty-container",
                            .message = type + " container has no children",
                            .path = path });
        }
        for (std::size_t i = 0; i < ch.size(); ++i) {
            lint_node_impl(ch.at(i), path + "/children/" + std::to_string(i), depth + 1, known, props, out);
        }
    }

    if (depth > AURORA_MAX_LINT_DEPTH) {
        out.push_back({ .severity = ErrorSeverity::Warning,
                        .code = "depth-exceeded",
                        .message = "UI tree depth exceeds " + std::to_string(AURORA_MAX_LINT_DEPTH) +
                                   " (possible infinite recursion)",
                        .path = path });
    }
}

/// @brief Run a structural check over the whole UI tree and return all findings
/// (an error-level finding makes aurora_lint exit with code 1).
inline auto lint_ui_tree(const Json &root) -> std::vector<LintFinding> {
    const auto [known, props] = load_known_types();
    std::vector<LintFinding> out;
    lint_node_impl(root, "$", 0, known, props, out);
    return out;
}

} // namespace aurora::tools
