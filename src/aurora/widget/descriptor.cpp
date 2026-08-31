#include "aurora/widget/descriptor.h"

namespace aurora {

auto descriptor_to_json(const PropDescriptor &p) -> Json {
    Json j = Json::object();
    j["name"] = p.name;
    j["type"] = p.type;
    j["default"] = p.default_value;
    j["required"] = p.required;
    if (!p.note.empty()) {
        j["note"] = p.note;
    }
    // ---- JSON Schema 约束字段（仅非空时输出，向后兼容） ----
    if (!p.json_type.empty()) {
        j["json_type"] = p.json_type;
    }
    if (!p.enum_values.empty()) {
        Json ev = Json::array();
        for (const auto &v : p.enum_values) {
            ev.push_back(v);
        }
        j["enum"] = ev;
    }
    if (!p.min_value.empty()) {
        j["minimum"] = p.min_value;
    }
    if (!p.max_value.empty()) {
        j["maximum"] = p.max_value;
    }
    if (!p.pattern.empty()) {
        j["pattern"] = p.pattern;
    }
    if (!p.constraint.empty()) {
        j["constraint"] = p.constraint;
    }
    if (!p.requires_props.empty()) {
        Json rp = Json::array();
        for (const auto &r : p.requires_props) {
            rp.push_back(r);
        }
        j["requires_props"] = rp;
    }
    if (!p.conflicts_with.empty()) {
        Json cw = Json::array();
        for (const auto &c : p.conflicts_with) {
            cw.push_back(c);
        }
        j["conflicts_with"] = cw;
    }
    return j;
}

auto descriptor_to_json(const WidgetDescriptor &d) -> Json {
    Json j = Json::object();
    j["name"] = d.name;
    j["namespace"] = d.ns;

    Json props = Json::array();
    for (const auto &p : d.properties) {
        props.push_back(descriptor_to_json(p));
    }
    j["properties"] = props;

    Json events = Json::array();
    for (const auto &e : d.events) {
        events.push_back(e);
    }
    j["events"] = events;

    j["children_policy"] = d.children_policy;

    Json examples = Json::array();
    for (const auto &ex : d.examples) {
        examples.push_back(ex);
    }
    j["examples"] = examples;

    // ---- Schema 扩展字段（仅非空时输出，向后兼容） ----
    if (!d.allowed_child_types.empty()) {
        Json act = Json::array();
        for (const auto &t : d.allowed_child_types) {
            act.push_back(t);
        }
        j["allowed_child_types"] = act;
    }
    if (!d.invariants.empty()) {
        Json inv = Json::array();
        for (const auto &inv_item : d.invariants) {
            inv.push_back(inv_item);
        }
        j["invariants"] = inv;
    }

    return j;
}

} // namespace aurora
