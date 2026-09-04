#include "aurora/widget/descriptor.h"

namespace aurora {

auto descriptor_to_json(const PropDescriptor &p) -> Json {
    Json j = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["name"] = p.name;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["type"] = p.type;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["default"] = p.default_value;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["required"] = p.required;
    if (!p.note.empty()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        j["note"] = p.note;
    }
    // ---- JSON Schema 约束字段（仅非空时输出，向后兼容） ----
    if (!p.json_type.empty()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        j["json_type"] = p.json_type;
    }
    if (!p.enum_values.empty()) {
        Json ev = Json::array();
        for (const auto &v : p.enum_values) {
            ev.push_back(v);
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        j["enum"] = ev;
    }
    if (!p.min_value.empty()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        j["minimum"] = p.min_value;
    }
    if (!p.max_value.empty()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        j["maximum"] = p.max_value;
    }
    if (!p.pattern.empty()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        j["pattern"] = p.pattern;
    }
    if (!p.constraint.empty()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        j["constraint"] = p.constraint;
    }
    if (!p.requires_props.empty()) {
        Json rp = Json::array();
        for (const auto &r : p.requires_props) {
            rp.push_back(r);
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        j["requires_props"] = rp;
    }
    if (!p.conflicts_with.empty()) {
        Json cw = Json::array();
        for (const auto &c : p.conflicts_with) {
            cw.push_back(c);
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        j["conflicts_with"] = cw;
    }
    return j;
}

auto descriptor_to_json(const WidgetDescriptor &d) -> Json {
    Json j = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["name"] = d.name;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["namespace"] = d.ns;

    Json props = Json::array();
    for (const auto &p : d.properties) {
        props.push_back(descriptor_to_json(p));
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["properties"] = props;

    Json events = Json::array();
    for (const auto &e : d.events) {
        events.push_back(e);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["events"] = events;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["children_policy"] = d.children_policy;

    Json examples = Json::array();
    for (const auto &ex : d.examples) {
        examples.push_back(ex);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["examples"] = examples;

    // ---- Schema 扩展字段（仅非空时输出，向后兼容） ----
    if (!d.allowed_child_types.empty()) {
        Json act = Json::array();
        for (const auto &t : d.allowed_child_types) {
            act.push_back(t);
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        j["allowed_child_types"] = act;
    }
    if (!d.invariants.empty()) {
        Json inv = Json::array();
        for (const auto &inv_item : d.invariants) {
            inv.push_back(inv_item);
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        j["invariants"] = inv;
    }

    return j;
}

}  // namespace aurora