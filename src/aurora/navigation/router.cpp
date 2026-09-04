#include "aurora/navigation/router.h"

#include "aurora/navigation/route.h"

namespace aurora {

auto Router::register_route(std::string name, RouteBuilder builder) -> void {
    routes_[std::move(name)] = std::move(builder);
}

auto Router::has(const std::string &name) const -> bool { return routes_.contains(name); }

auto Router::build(const std::string &name) const -> std::optional<Route> {
    const auto it = routes_.find(name);
    if (it == routes_.end()) {
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

}  // namespace aurora
