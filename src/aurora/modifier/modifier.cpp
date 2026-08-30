#include "aurora/modifier/modifier.h"

#include <algorithm>

namespace aurora {

auto Modifier::invoke_click() const -> void {
    for (const auto &n : m_nodes) {
        if (n) {
            n->fire_click();
        }
    }
}

auto Modifier::transform(const Size &self_size) const -> TransformInfo {
    TransformInfo info;
    info.content_size = self_size;
    for (const auto &n : m_nodes) {
        if (const auto *a = dynamic_cast<const AlignNode *>(n.get())) {
            const Size child = a->child_size();
            info.translation = info.translation + align_origin(a->align(), child, self_size);
            info.content_size = child;
        } else if (const auto *o = dynamic_cast<const OffsetNode *>(n.get())) {
            info.translation = info.translation + Point{ .x = o->dx(), .y = o->dy() };
        } else if (const auto *p = dynamic_cast<const Padding *>(n.get())) {
            // 内边距：内容盒整体平移 (pad,pad)，并收缩尺寸，使子节点绘制在内边距以内。
            info.translation = info.translation + Point{ .x = p->padding(), .y = p->padding() };
            info.content_size = Size{ .width = std::max(0.0f, info.content_size.width - 2.0f * p->padding()),
                                      .height = std::max(0.0f, info.content_size.height - 2.0f * p->padding()) };
        } else if (const auto *pe = dynamic_cast<const PaddingEdges *>(n.get())) {
            // 非对称内边距：内容盒平移 (left, top)，收缩尺寸。
            const EdgeInsets ins = pe->insets();
            info.translation = info.translation + Point{ .x = ins.left, .y = ins.top };
            info.content_size = Size{ .width = std::max(0.0f, info.content_size.width - ins.horizontal()),
                                      .height = std::max(0.0f, info.content_size.height - ins.vertical()) };
        } else if (const auto *t = dynamic_cast<const TransformNode *>(n.get())) {
            // 绕当前内容盒中心的仿射矩阵，按链序组合。
            info.matrix = info.matrix.compose(t->matrix(info.content_size));
        } else if (const auto *op = dynamic_cast<const OpacityNode *>(n.get())) {
            info.opacity *= op->alpha();
        }
    }
    return info;
}

auto Modifier::invoke_drag_start(std::optional<int> pid) const -> void {
    for (const auto &n : m_nodes) {
        if (const auto *d = dynamic_cast<const Draggable *>(n.get())) {
            d->bind(pid);
            if (d->matches(pid)) {
                d->fire_start();
            }
        }
    }
}

auto Modifier::invoke_drag(const Point &delta, const Point &pos, std::optional<int> pid) const -> void {
    for (const auto &n : m_nodes) {
        if (const auto *d = dynamic_cast<const Draggable *>(n.get())) {
            if (d->matches(pid)) {
                d->fire_drag(delta, pos);
            }
        }
    }
}

auto Modifier::invoke_drag_end(std::optional<int> pid) const -> void {
    for (const auto &n : m_nodes) {
        if (const auto *d = dynamic_cast<const Draggable *>(n.get())) {
            if (d->matches(pid)) {
                d->fire_end();
                d->release();
            }
        }
    }
}

auto Modifier::has_gesture() const -> bool {
    return std::ranges::any_of(m_nodes, [](const auto &n) -> auto {
        return dynamic_cast<const Draggable *>(n.get()) != nullptr ||
               dynamic_cast<const LongPress *>(n.get()) != nullptr;
    });
}

auto Modifier::has_clickable() const -> bool {
    return std::ranges::any_of(
        m_nodes, [](const auto &n) -> auto { return dynamic_cast<const Clickable *>(n.get()) != nullptr; });
}

auto Modifier::long_press_fired() const -> bool {
    return std::ranges::any_of(m_nodes, [](const auto &n) -> auto {
        const auto *lp = dynamic_cast<const LongPress *>(n.get());
        return lp != nullptr && lp->long_press_fired();
    });
}

auto Modifier::press_long_press(std::chrono::steady_clock::time_point t, std::optional<int> pid) const -> void {
    for (const auto &n : m_nodes) {
        if (auto *lp = dynamic_cast<LongPress *>(n.get())) {
            lp->bind(pid);
            if (lp->matches(pid)) {
                lp->press_at(t);
            }
        }
    }
}

auto Modifier::cancel_long_press(std::optional<int> pid) const -> void {
    for (const auto &n : m_nodes) {
        if (auto *lp = dynamic_cast<LongPress *>(n.get())) {
            if (lp->matches(pid)) {
                lp->cancel();
                lp->release();
            }
        }
    }
}

auto Modifier::tick_long_press(std::chrono::steady_clock::time_point now) const -> void {
    for (const auto &n : m_nodes) {
        if (auto *lp = dynamic_cast<LongPress *>(n.get())) {
            lp->tick(now);
        }
    }
}

auto Modifier::on_pointer_event(const TouchEvent &e) const -> void {
    for (const auto &n : m_nodes) {
        n->on_touch(e);
    }
}

auto Modifier::tick_tooltip(std::chrono::steady_clock::time_point now) const -> void {
    for (const auto &n : m_nodes) {
        if (auto *tt = dynamic_cast<TooltipNode *>(n.get())) {
            tt->tick(now);
        }
    }
}

auto Modifier::tooltip_hover_start(std::chrono::steady_clock::time_point t) const -> void {
    for (const auto &n : m_nodes) {
        if (const auto *tt = dynamic_cast<TooltipNode *>(n.get())) {
            tt->hover_start(t);
        }
    }
}

auto Modifier::tooltip_hover_end() const -> void {
    for (const auto &n : m_nodes) {
        if (const auto *tt = dynamic_cast<TooltipNode *>(n.get())) {
            tt->hover_end();
        }
    }
}

auto Modifier::active_tooltip() const -> std::string {
    for (const auto &n : m_nodes) {
        if (const auto *tt = dynamic_cast<const TooltipNode *>(n.get())) {
            if (tt->is_visible()) {
                return tt->text();
            }
        }
    }
    return {};
}

auto Modifier::has_tooltip() const -> bool {
    return std::ranges::any_of(
        m_nodes, [](const auto &n) -> auto { return dynamic_cast<const TooltipNode *>(n.get()) != nullptr; });
}

auto Modifier::has_context_menu() const -> bool {
    return std::ranges::any_of(
        m_nodes, [](const auto &n) -> auto { return dynamic_cast<const ContextMenuNode *>(n.get()) != nullptr; });
}

auto Modifier::open_context_menu(Point pos) const -> void {
    for (const auto &n : m_nodes) {
        if (const auto *cm = dynamic_cast<ContextMenuNode *>(n.get())) {
            cm->open_at(pos);
        }
    }
}

auto Modifier::close_context_menu() const -> void {
    for (const auto &n : m_nodes) {
        if (const auto *cm = dynamic_cast<ContextMenuNode *>(n.get())) {
            cm->close();
        }
    }
}

auto Modifier::active_context_menu_items() const -> std::vector<MenuItem> {
    for (const auto &n : m_nodes) {
        if (const auto *cm = dynamic_cast<const ContextMenuNode *>(n.get())) {
            if (cm->is_open()) {
                return cm->items();
            }
        }
    }
    return {};
}

auto Modifier::active_context_menu_position() const -> Point {
    for (const auto &n : m_nodes) {
        if (const auto *cm = dynamic_cast<const ContextMenuNode *>(n.get())) {
            if (cm->is_open()) {
                return cm->position();
            }
        }
    }
    return Point{ .x = 0.0f, .y = 0.0f };
}

} // namespace aurora
