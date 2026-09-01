#include "aurora/navigation/navigator.h"

#include "aurora/core/diagnostics.h"
#include "aurora/navigation/route.h"

namespace aurora {

Navigator::Navigator(Route initial) { push(std::move(initial)); }

auto Navigator::push(Route route) -> void {
    if (depth() + 1 > m_max_depth) {
        Diagnostics::degraded("Navigator::push refused: route stack depth " + std::to_string(depth() + 1) +
                                  " would exceed max_depth " + std::to_string(m_max_depth),
                              "navigator.cpp:push");
        return;
    }
    m_stack.push_back(std::move(route));
    notify();
}

auto Navigator::push_replacement(Route route) -> void {
    if (m_stack.empty()) {
        if (depth() + 1 > m_max_depth) {
            Diagnostics::degraded("Navigator::push_replacement refused: route stack depth " +
                                      std::to_string(depth() + 1) + " would exceed max_depth " +
                                      std::to_string(m_max_depth),
                                  "navigator.cpp:push_replacement");
            return;
        }
        m_stack.push_back(std::move(route));
    } else {
        m_stack.back() = std::move(route);
    }
    notify();
}

auto Navigator::pop() -> bool {
    if (m_stack.size() <= 1) {
        return false;
    }
    m_stack.pop_back();
    notify();
    return true;
}

auto Navigator::pop_to_root() -> void {
    while (m_stack.size() > 1) {
        m_stack.pop_back();
    }
    notify();
}

auto Navigator::current() -> Route & { return m_stack.back(); }

auto Navigator::current() const -> const Route & { return m_stack.back(); }

auto Navigator::current_root() -> Node { return m_stack.empty() ? Node{} : m_stack.back().root(); }

auto Navigator::depth() const -> std::size_t { return m_stack.size(); }

auto Navigator::can_pop() const -> bool { return m_stack.size() > 1; }

auto Navigator::stack() const -> const std::vector<Route> & { return m_stack; }

auto Navigator::path() const -> std::vector<std::string> {
    std::vector<std::string> out;
    out.reserve(m_stack.size());
    for (const Route &r : m_stack) {
        out.push_back(r.name());
    }
    return out;
}

auto Navigator::restore(const std::vector<std::string> &names, const std::function<Route(std::string)> &build) -> void {
    if (names.empty()) {
        return;
    }
    if (names.size() > m_max_depth) {
        Diagnostics::degraded("Navigator::restore refused: route stack depth " + std::to_string(names.size()) +
                                  " exceeds max_depth " + std::to_string(m_max_depth),
                              "navigator.cpp:restore");
        return;
    }
    m_stack.clear();
    m_stack.reserve(names.size());
    for (const std::string &n : names) {
        m_stack.push_back(build(n));
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
            resolved.push_back(n); // 表中缺失的名称段跳过，不构造空路由。
        }
    }
    restore(resolved, [&registry](const std::string &name) -> aurora::Route { return registry.at(name)(name); });
}

auto Navigator::set_on_route_changed(std::function<void()> cb) -> void { m_on_changed = std::move(cb); }

auto Navigator::max_depth() const -> std::size_t { return m_max_depth; }

auto Navigator::set_max_depth(std::size_t d) -> void { m_max_depth = d; }

auto Navigator::notify() const -> void {
    if (m_on_changed) {
        m_on_changed();
    }
}

} // namespace aurora
