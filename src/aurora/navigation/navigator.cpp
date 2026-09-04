#include "aurora/navigation/navigator.h"

#include "aurora/core/diagnostics.h"
#include "aurora/navigation/route.h"

namespace aurora {

Navigator::Navigator(Route initial) { push(std::move(initial)); }

auto Navigator::push(Route route) -> void {
    if (depth() + 1 > max_depth_) {
        Diagnostics::degraded("Navigator::push refused: route stack depth " + std::to_string(depth() + 1) +
                                  " would exceed max_depth " + std::to_string(max_depth_),
                              "navigator.cpp:push");
        return;
    }
    stack_.push_back(std::move(route));
    notify();
}

auto Navigator::push_replacement(Route route) -> void {
    if (stack_.empty()) {
        if (depth() + 1 > max_depth_) {
            Diagnostics::degraded("Navigator::push_replacement refused: route stack depth " +
                                      std::to_string(depth() + 1) + " would exceed max_depth " +
                                      std::to_string(max_depth_),
                                  "navigator.cpp:push_replacement");
            return;
        }
        stack_.push_back(std::move(route));
    } else {
        stack_.back() = std::move(route);
    }
    notify();
}

auto Navigator::pop() -> bool {
    if (stack_.size() <= 1) {
        return false;
    }
    stack_.pop_back();
    notify();
    return true;
}

auto Navigator::pop_to_root() -> void {
    while (stack_.size() > 1) {
        stack_.pop_back();
    }
    notify();
}

auto Navigator::current() -> Route & { return stack_.back(); }

auto Navigator::current() const -> const Route & { return stack_.back(); }

auto Navigator::current_root() -> Node { return stack_.empty() ? Node{} : stack_.back().root(); }

auto Navigator::depth() const -> std::size_t { return stack_.size(); }

auto Navigator::can_pop() const -> bool { return stack_.size() > 1; }

auto Navigator::stack() const -> const std::vector<Route> & { return stack_; }

auto Navigator::path() const -> std::vector<std::string> {
    std::vector<std::string> out;
    out.reserve(stack_.size());
    for (const Route &r : stack_) {
        out.push_back(r.name());
    }
    return out;
}

auto Navigator::restore(const std::vector<std::string> &names, const std::function<Route(std::string)> &build) -> void {
    if (names.empty()) {
        return;
    }
    if (names.size() > max_depth_) {
        Diagnostics::degraded("Navigator::restore refused: route stack depth " + std::to_string(names.size()) +
                                  " exceeds max_depth " + std::to_string(max_depth_),
                              "navigator.cpp:restore");
        return;
    }
    stack_.clear();
    stack_.reserve(names.size());
    for (const std::string &n : names) {
        stack_.push_back(build(n));
    }
    notify();
}

auto Navigator::split_uri(const std::string &uri) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::string seg;
    seg.reserve(16);
    for (const char c : uri) {
        if (c == '/') {
            if (!seg.empty()) {
                out.push_back(seg);
                seg.clear();
            }
        } else {
            seg.push_back(c);
        }
    }
    if (!seg.empty()) {
        out.push_back(seg);
    }
    return out;
}

auto Navigator::open_uri(const std::string &uri, const std::function<Route(const std::string &)> &build) -> void {
    restore(split_uri(uri), build);
}

auto Navigator::open_uri(const std::string &uri, const RouteRegistry &registry) -> void {
    const std::vector<std::string> names = split_uri(uri);
    std::vector<std::string> resolved;
    resolved.reserve(names.size());
    for (const std::string &n : names) {
        if (registry.contains(n)) {
            resolved.push_back(n);  // 表中缺失的名称段跳过，不构造空路由。
        }
    }
    restore(resolved, [&registry](const std::string &name) -> aurora::Route { return registry.at(name)(name); });
}

auto Navigator::set_on_route_changed(std::function<void()> cb) -> void { on_changed_ = std::move(cb); }

auto Navigator::max_depth() const -> std::size_t { return max_depth_; }

auto Navigator::set_max_depth(std::size_t d) -> void { max_depth_ = d; }

auto Navigator::notify() const -> void {
    if (on_changed_) {
        on_changed_();
    }
}

}  // namespace aurora
