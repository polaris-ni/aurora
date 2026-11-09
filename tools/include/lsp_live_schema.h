// =============================================================================
// lsp_live_schema.h — Bridge from the library's live API to the LSP analyzer's Schema.
// -----------------------------------------------------------------------------
// Unlike the three analyzer headers (lsp_schema.h / lsp_document.h / lsp_features.h), this one DOES
// link the library. It is the single place where the running server's schema is filled, so the
// analyzer stays testable against a synthetic Schema while the server always sees the live code.
//
// Two sources, both live (specification/08-tooling.md §7.3: never read aurora_api.json):
//   describe_component + known_enums -> the component / property / enum tables;
//   codegen's capability bits        -> which types load properties only through set_<key>() chain
//                                       writers, which keys have no Props-side writer at all, and
//                                       the C++ spellings that differ from the serialization names.
// Sharing the codegen bits is what keeps the two emission surfaces from drifting: the writer name a
// completion inserts is the same PropTarget::lhs the code generator emits, and a key the generator
// refuses to put in a construction expression is a key the LSP refuses to complete inside one.
// =============================================================================

#pragma once

#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "known_enums.h"
#include "lsp_schema.h"

namespace aurora::tools {

/// @brief Build the analyzer schema from the library's live self-description and codegen capability bits.
///
/// Components whose self-description throws are skipped with an error log rather than aborting the
/// whole schema: one malformed descriptor must not take the language service down.
///
/// @return Schema with every registered component, its property rows (writer name included), and the
///         known-enum registry.
[[nodiscard]] inline auto live_schema() -> Schema {
    Schema result;
    for (const auto &kv : known_enums()) {
        EnumSchema e;
        e.name = kv.first;
        e.values = kv.second;
        result.enums.push_back(std::move(e));
    }
    for (const std::string &t : au::list_all_components()) {
        try {
            au::Json j = au::describe_component(t);
            if (j.is_null() || j.empty()) {
                continue;
            }
            ComponentSchema c;
            c.type = j.as_or<std::string>("type", t);
            if (const auto *v = j.at("category"); v != nullptr && v->is_string()) {
                c.category = v->as_or<std::string>("");
            }
            if (const auto *v = j.at("children_policy"); v != nullptr && v->is_string()) {
                c.children_policy = v->as_or<std::string>("");
            } else if (const auto *v = j.at("container"); v != nullptr && v->is_string()) {
                c.children_policy = v->as_or<std::string>("");
            }
            // Same capability bits the code generator asks, so the two surfaces cannot disagree.
            c.chain_writers = au::serialization::detail::props_chain_writers(c.type);
            c.cpp_type = au::serialization::detail::cpp_class(c.type);
            c.props_type = c.chain_writers ? au::serialization::detail::cpp_props(c.type) : std::string{};
            if (const auto *descriptors = j.at("props"); descriptors != nullptr && descriptors->is_array()) {
                for (const auto &descriptor : *descriptors) {
                    PropSchema ps;
                    if (const auto *v = descriptor.at("name"); v != nullptr && v->is_string()) {
                        ps.name = v->as_or<std::string>("");
                    }
                    if (const auto *v = descriptor.at("type"); v != nullptr && v->is_string()) {
                        ps.type = v->as_or<std::string>("");
                    }
                    if (const auto *d = descriptor.at("default"); d != nullptr) {
                        if (d->is_string()) {
                            ps.default_value = d->as_or<std::string>("");
                        } else {
                            const auto dumped = au::json::dump(*d);
                            ps.default_value = dumped.ok() ? dumped.value() : std::string{};
                        }
                    } else if (const auto *e = descriptor.at("default_expr"); e != nullptr && e->is_string()) {
                        // Rows whose fallback is a live theme token have no native default to
                        // serialize; the display expression is the only default information they
                        // carry, and dropping it would leave hover / completion silent about them.
                        ps.default_value = e->as_or<std::string>("");
                    }
                    ps.required = descriptor.as_or<bool>("required", false);
                    if (const auto *v = descriptor.at("note"); v != nullptr && v->is_string()) {
                        ps.note = v->as_or<std::string>("");
                    }
                    // The props table is a union: authority-table rows plus the Widget-side layout
                    // keys appended by serialize_props. The latter have no member on the Props
                    // aggregate, so they get no writer and are never completed inside a chain.
                    // expressible additionally drops runtime / derived keys (LazyRow.offset,
                    // Image.image_width) that the generator also refuses to emit.
                    const bool has_writer = c.chain_writers &&
                                            !au::serialization::detail::is_base_key(ps.name, c.type) &&
                                            au::serialization::detail::expressible(c.type, ps.name);
                    ps.writer =
                        has_writer ? au::serialization::detail::prop_target(c.type, ps.name).lhs : std::string{};
                    c.props.push_back(std::move(ps));
                }
            }
            if (const auto *events = j.at("events"); events != nullptr && events->is_array()) {
                for (const auto &event : *events) {
                    if (event.is_string()) {
                        c.events.push_back(event.as_or<std::string>(""));
                    }
                }
            }
            if (const auto *examples = j.at("examples"); examples != nullptr && examples->is_array()) {
                for (const auto &example : *examples) {
                    if (example.is_string()) {
                        c.examples.push_back(example.as_or<std::string>(""));
                    }
                }
            }
            result.components.push_back(std::move(c));
        } catch (const std::exception &e) {
            AURORA_LOG_ERROR("lsp", "[aurora-lsp] skip component ", t, ": ", e.what());
        }
    }
    return result;
}

}  // namespace aurora::tools
