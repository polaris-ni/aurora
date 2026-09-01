#include "aurora/app/scene.h"

#include "aurora/core/string_util.h"

namespace aurora {

auto Scene::serialize() const -> std::string {
    std::string out;
    serialize_widget(m_root.widget(), out);
    return out;
}

auto Scene::serialize_widget(const Widget &w, std::string &out) -> void {
    out += R"({"type":")";
    out += w.type_name();
    out += R"(","size":[)";
    out += internal::string_format("%.1f,%.1f", w.size().width, w.size().height);
    out += ']';

    bool has_children = false;
    w.for_each_child([&](const Widget & /*c*/) -> void { has_children = true; });
    if (has_children) {
        out += ",\"children\":[";
        bool first = true;
        w.for_each_child([&](const Widget &c) -> void {
            if (!first) {
                out += ',';
            }
            first = false;
            serialize_widget(c, out);
        });
        out += ']';
    }
    out += '}';
}

} // namespace aurora
