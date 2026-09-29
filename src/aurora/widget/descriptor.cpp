#include "aurora/widget/descriptor.h"

namespace aurora {

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

auto descriptor_to_json(const PropDescriptor &p) -> Json {
    Json j = Json::object();

    j.set("name", p.name);
    j.set("type", p.type);
    j.set("default", p.default_value);
    j.set("required", Json{p.required});
    if (!p.note.empty()) {
        j.set("note", p.note);
    }
    // ---- JSON Schema 约束字段（仅非空时输出，向后兼容） ----
    if (!p.json_type.empty()) {
        j.set("json_type", p.json_type);
    }
    if (!p.enum_values.empty()) {
        Json ev = Json::array();
        for (const auto &v : p.enum_values) {
            ev.push_back(v);
        }
        j.set("enum", ev);
    }
    if (!p.min_value.empty()) {
        j.set("minimum", p.min_value);
    }
    if (!p.max_value.empty()) {
        j.set("maximum", p.max_value);
    }
    if (!p.pattern.empty()) {
        j.set("pattern", p.pattern);
    }
    if (!p.constraint.empty()) {
        j.set("constraint", p.constraint);
    }
    if (!p.requires_props.empty()) {
        Json rp = Json::array();
        for (const auto &r : p.requires_props) {
            rp.push_back(r);
        }
        j.set("requires_props", rp);
    }
    if (!p.conflicts_with.empty()) {
        Json cw = Json::array();
        for (const auto &c : p.conflicts_with) {
            cw.push_back(c);
        }
        j.set("conflicts_with", cw);
    }
    return j;
}

auto descriptor_to_json(const WidgetDescriptor &d) -> Json {
    Json j = Json::object();
    j.set("name", d.name);
    j.set("namespace", d.ns);

    Json props = Json::array();
    for (const auto &p : d.properties) {
        props.push_back(descriptor_to_json(p));
    }
    j.set("properties", props);

    Json events = Json::array();
    for (const auto &e : d.events) {
        events.push_back(e);
    }
    j.set("events", events);

    j.set("children_policy", d.children_policy);

    Json examples = Json::array();
    for (const auto &ex : d.examples) {
        examples.push_back(ex);
    }
    j.set("examples", examples);

    // ---- Schema 扩展字段（仅非空时输出，向后兼容） ----
    if (!d.allowed_child_types.empty()) {
        Json act = Json::array();
        for (const auto &t : d.allowed_child_types) {
            act.push_back(t);
        }
        j.set("allowed_child_types", act);
    }
    if (!d.invariants.empty()) {
        Json inv = Json::array();
        for (const auto &inv_item : d.invariants) {
            inv.push_back(inv_item);
        }
        j.set("invariants", inv);
    }

    return j;
}

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

}  // namespace aurora
