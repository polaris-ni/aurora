// =============================================================================
// lsp_schema.h — Schema model layer of the LSP analyzer (header-only, decoupled from the library).
// -----------------------------------------------------------------------------
// Component / enum / property schemas for declarative UI, plus pure functions such as lookup and
// Props-suffix stripping. Consumed by lsp_document.h (parsing) and lsp_features.h
// (completion / hover / diagnostics / code actions).
//
// The three analyzer headers never link the library: every field below is filled by the caller.
// tools/include/lsp_live_schema.h is the in-repo bridge that fills them from the live API
// (describe_component + known_enums) and from the codegen capability bits, so the LSP and the
// code generator agree on which types load properties through set_<key>() chain writers.
// =============================================================================

#pragma once

#include <string>
#include <vector>

namespace aurora::tools {  // tool namespace, avoids clashing with the main aurora namespace

// ----------------------------- schema model -----------------------------------
struct PropSchema {
    std::string name;
    std::string type;
    std::string default_value;  // stringified default value (may be empty)
    bool required = false;
    std::string note;
    // Name of the set_<key>() writer on the Props class; empty means this key has no Props-side
    // writer. The schema's props table is the union of the authority-table rows and the Widget-side
    // layout keys (width / height / show / overflow), and the latter live on Widget, not on the
    // Props aggregate — completing them inside a XxxProps() chain would emit code that does not
    // compile. The bridge derives this from the same capability bits the code generator uses.
    std::string writer;
};

struct EnumSchema {
    std::string name;
    std::vector<std::string> values;
};

struct ComponentSchema {
    std::string type;  // widget type name (without the Props suffix)
    std::string category;
    std::string children_policy;
    // C++ spelling of the widget class and of its Props aggregate, relative to au:: (a few controls
    // differ from the serialization type name: Image -> ImageView / ImageViewProps, and WebView
    // lives in au::webview::). Empty props_type means the type has no aggregate at all, so no
    // property-loading form can be synthesized for it.
    std::string cpp_type;
    std::string props_type;
    // True when the type's properties may only be loaded through set_<key>() chain writers, i.e.
    // designated initializers are not a legal form for it. Same source as codegen's capability bit.
    bool chain_writers = false;
    std::vector<PropSchema> props;
    std::vector<std::string> events;
    std::vector<std::string> examples;
};

struct Schema {
    std::vector<ComponentSchema> components;
    std::vector<EnumSchema> enums;

    // Strip a trailing namespace segment and the Props suffix, resolving to the widget type name;
    // returning empty means not a widget (it may be an enum, etc.). Namespace segments are dropped
    // because au::webview::WebViewProps names the same component as WebView.
    [[nodiscard]] static auto strip_props(const std::string &t) -> std::string {
        const auto sep = t.rfind("::");
        std::string seg = sep == std::string::npos ? t : t.substr(sep + 2);
        if (seg.size() > 5 && seg.ends_with("Props")) {
            return seg.substr(0, seg.size() - 5);
        }
        return seg;
    }
    // Resolve a written type name (widget, Props, C++ class spelling, or namespace-qualified) to its
    // component. The serialization type name is tried first; the C++ spellings are the fallback for
    // the controls whose class name differs from it.
    [[nodiscard]] auto find_component(const std::string &t) const -> const ComponentSchema * {
        const std::string bare = strip_props(t);
        for (const auto &c : components) {
            if (c.type == t || c.type == bare) {
                return &c;
            }
        }
        for (const auto &c : components) {
            if (!c.cpp_type.empty() && (c.cpp_type == t || c.cpp_type == bare)) {
                return &c;
            }
            if (!c.props_type.empty() && c.props_type == t) {
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
    // First namespace segment of a qualified name. The parser reads au::Alignment::Center as one
    // reference named "Alignment::Center", whose enum part is the head segment.
    [[nodiscard]] static auto head_segment(const std::string &t) -> std::string {
        const auto sep = t.find("::");
        return sep == std::string::npos ? t : t.substr(0, sep);
    }
    // Enum lookup that also accepts the qualified spelling au::Enum::Value.
    [[nodiscard]] auto find_enum_qualified(const std::string &t) const -> const EnumSchema * {
        if (const auto *e = find_enum(t); e != nullptr) {
            return e;
        }
        return find_enum(head_segment(t));
    }
    // Whether this is a known widget type (including the Props and namespace-qualified forms).
    [[nodiscard]] auto is_known_component(const std::string &t) const -> bool { return find_component(t) != nullptr; }
    [[nodiscard]] auto is_known_enum(const std::string &t) const -> bool { return find_enum(t) != nullptr; }
    // Whether a written au:: reference names anything known. The head-segment fallback keeps
    // au::Alignment::Center and au::webview::WebView from being reported as unknown types.
    [[nodiscard]] auto is_known_ref(const std::string &t) const -> bool {
        return is_known_component(t) || is_known_enum(t) || is_known_enum(head_segment(t));
    }
};

}  // namespace aurora::tools
