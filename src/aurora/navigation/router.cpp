#include "aurora/navigation/router.h"

#include "aurora/navigation/route.h"

namespace aurora {

auto Router::register_route(std::string name, RouteBuilder builder) -> void {
    m_routes[std::move(name)] = std::move(builder);
}

auto Router::has(const std::string &name) const -> bool { return m_routes.contains(name); }

auto Router::build(const std::string &name) const -> std::optional<Route> {
    const auto it = m_routes.find(name);
    if (it == m_routes.end()) {
        return std::nullopt;
    }
    return it->second();
}

auto Router::build_root(const std::string &name) const -> Node {
    auto r = build(name);
    if (r) {
        return r->root();
    }
    return Node{};
}

} // namespace aurora
