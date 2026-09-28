#include "aurora/modifier/modifier.h"

#include <algorithm>

namespace aurora {

namespace {

/// @brief 盒收缩节点给内侧节点留下的盒子（仅 Transform 切片的 Align / Offset 会改变盒子；
///        其余节点原样返回）。
/// @note Padding/PaddingEdges **不**参与：Paint 修饰覆盖内边距是既定语义（规格 §7.4
///       「把 Paint 修饰限制在 content_box 导致 padding 区域露白」被列为历史错误形态）。
[[nodiscard]] auto shrink_for_inner(const ModifierNode &n, const Rect &cur) -> Rect {
    if (const auto *a = dynamic_cast<const AlignNode *>(&n)) {
        const Size child = a->child_size();
        return Rect{.origin = cur.origin + align_origin(a->align(), child, cur.size), .size = child};
    }
    if (const auto *o = dynamic_cast<const OffsetNode *>(&n)) {
        return Rect{.origin = Point{.x = cur.origin.x + o->dx(), .y = cur.origin.y + o->dy()}, .size = cur.size};
    }
    return cur;
}

/// @brief 该节点是否改变内侧节点的盒子。
[[nodiscard]] auto shrinks_box(const ModifierNode &n) -> bool {
    return n.kind() == ModifierNode::Kind::Transform &&
           (dynamic_cast<const AlignNode *>(&n) != nullptr || dynamic_cast<const OffsetNode *>(&n) != nullptr);
}

}  // namespace

auto Modifier::invoke_click() const -> void {
    for (const auto &n : nodes_) {
        if (n) {
            n->fire_click();
        }
    }
}

auto Modifier::transform(const Size &self_size) const -> TransformInfo {
    TransformInfo info;
    info.content_size = self_size;
    info.hit_size = self_size;
    for (const auto &n : nodes_) {
        if (const auto *a = dynamic_cast<const AlignNode *>(n.get())) {
            const Size child = a->child_size();
            info.translation = info.translation + align_origin(a->align(), child, self_size);
            info.content_size = child;
            // 命中盒与被对齐收缩后的自身盒一致：外侧 Align 展开出的那段空间不属于本控件，
            // 若仍按布局盒尺寸判定，展开行会在平移方向上多吞一块什么都没画的点击。
            info.hit_size = child;
        } else if (const auto *o = dynamic_cast<const OffsetNode *>(n.get())) {
            info.translation = info.translation + Point{.x = o->dx(), .y = o->dy()};
        } else if (const auto *p = dynamic_cast<const Padding *>(n.get())) {
            // 内边距：内容盒整体平移 (pad,pad)，并收缩尺寸，使子节点绘制在内边距以内。
            info.translation = info.translation + Point{.x = p->padding(), .y = p->padding()};
            info.content_size = Size{.width = std::max(0.0F, info.content_size.width - 2.0F * p->padding()),
                                     .height = std::max(0.0F, info.content_size.height - 2.0F * p->padding())};
        } else if (const auto *pe = dynamic_cast<const PaddingEdges *>(n.get())) {
            // 非对称内边距：内容盒平移 (left, top)，收缩尺寸。
            const EdgeInsets ins = pe->insets();
            info.translation = info.translation + Point{.x = ins.left, .y = ins.top};
            info.content_size = Size{.width = std::max(0.0F, info.content_size.width - ins.horizontal()),
                                     .height = std::max(0.0F, info.content_size.height - ins.vertical())};
        } else if (const auto *t = dynamic_cast<const TransformNode *>(n.get())) {
            // 绕当前内容盒中心的仿射矩阵，按链序组合。
            info.matrix = info.matrix.compose(t->matrix(info.content_size));
        } else if (const auto *op = dynamic_cast<const OpacityNode *>(n.get())) {
            info.opacity *= op->alpha();
        }
    }
    return info;
}

auto Modifier::paint_boxes(const Rect &widget_box) const -> std::vector<Rect> {
    bool any_shrink = false;
    for (const auto &n : nodes_) {
        if (n && shrinks_box(*n)) {
            any_shrink = true;
            break;
        }
    }
    if (!any_shrink) {
        return {};  // 无盒改变节点：各 Paint 节点都作用于控件自身盒（历史行为）
    }
    std::vector<Rect> boxes;
    boxes.reserve(nodes_.size());
    Rect cur = widget_box;
    // 正序走链 = 由外向外内（`Widget::layout` 逆序包裹，故先压入者靠外）：
    // 节点 j 的盒子是它外侧盒改变节点作用完剩下的那个。
    for (const auto &n : nodes_) {
        boxes.push_back(cur);
        if (n) {
            cur = shrink_for_inner(*n, cur);
        }
    }
    return boxes;
}

auto Modifier::invoke_drag_start(std::optional<int> pid) const -> void {
    for (const auto &n : nodes_) {
        if (const auto *d = dynamic_cast<const Draggable *>(n.get())) {
            d->bind(pid);
            if (d->matches(pid)) {
                d->fire_start();
            }
        }
    }
}

auto Modifier::invoke_drag(const Point &delta, const Point &pos, std::optional<int> pid) const -> void {
    for (const auto &n : nodes_) {
        if (const auto *d = dynamic_cast<const Draggable *>(n.get())) {
            if (d->matches(pid)) {
                d->fire_drag(delta, pos);
            }
        }
    }
}

auto Modifier::invoke_drag_end(std::optional<int> pid) const -> void {
    for (const auto &n : nodes_) {
        if (const auto *d = dynamic_cast<const Draggable *>(n.get())) {
            if (d->matches(pid)) {
                d->fire_end();
                d->release();
            }
        }
    }
}

auto Modifier::has_gesture() const -> bool {
    return std::ranges::any_of(nodes_, [](const auto &n) -> auto {
        return dynamic_cast<const Draggable *>(n.get()) != nullptr ||
               dynamic_cast<const LongPress *>(n.get()) != nullptr;
    });
}

auto Modifier::has_clickable() const -> bool {
    return std::ranges::any_of(
        nodes_, [](const auto &n) -> auto { return dynamic_cast<const Clickable *>(n.get()) != nullptr; });
}

auto Modifier::long_press_fired() const -> bool {
    return std::ranges::any_of(nodes_, [](const auto &n) -> auto {
        const auto *lp = dynamic_cast<const LongPress *>(n.get());
        return lp != nullptr && lp->long_press_fired();
    });
}

auto Modifier::press_long_press(std::chrono::steady_clock::time_point t, std::optional<int> pid) const -> void {
    for (const auto &n : nodes_) {
        if (auto *lp = dynamic_cast<LongPress *>(n.get())) {
            lp->bind(pid);
            if (lp->matches(pid)) {
                lp->press_at(t);
            }
        }
    }
}

auto Modifier::cancel_long_press(std::optional<int> pid) const -> void {
    for (const auto &n : nodes_) {
        if (auto *lp = dynamic_cast<LongPress *>(n.get())) {
            if (lp->matches(pid)) {
                lp->cancel();
                lp->release();
            }
        }
    }
}

auto Modifier::tick_long_press(std::chrono::steady_clock::time_point now) const -> void {
    for (const auto &n : nodes_) {
        if (auto *lp = dynamic_cast<LongPress *>(n.get())) {
            lp->tick(now);
        }
    }
}

auto Modifier::on_pointer_event(const TouchEvent &e) const -> void {
    for (const auto &n : nodes_) {
        n->on_touch(e);
    }
}

auto Modifier::tick_tooltip(std::chrono::steady_clock::time_point now) const -> void {
    for (const auto &n : nodes_) {
        if (auto *tt = dynamic_cast<TooltipNode *>(n.get())) {
            tt->tick(now);
        }
    }
}

auto Modifier::tooltip_hover_start(std::chrono::steady_clock::time_point t) const -> void {
    for (const auto &n : nodes_) {
        if (const auto *tt = dynamic_cast<TooltipNode *>(n.get())) {
            tt->hover_start(t);
        }
    }
}

auto Modifier::tooltip_hover_end() const -> void {
    for (const auto &n : nodes_) {
        if (const auto *tt = dynamic_cast<TooltipNode *>(n.get())) {
            tt->hover_end();
        }
    }
}

auto Modifier::active_tooltip() const -> std::string {
    for (const auto &n : nodes_) {
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
        nodes_, [](const auto &n) -> auto { return dynamic_cast<const TooltipNode *>(n.get()) != nullptr; });
}

auto Modifier::has_context_menu() const -> bool {
    return std::ranges::any_of(
        nodes_, [](const auto &n) -> auto { return dynamic_cast<const ContextMenuNode *>(n.get()) != nullptr; });
}

auto Modifier::open_context_menu(Point pos) const -> void {
    for (const auto &n : nodes_) {
        if (const auto *cm = dynamic_cast<ContextMenuNode *>(n.get())) {
            cm->open_at(pos);
        }
    }
}

auto Modifier::close_context_menu() const -> void {
    for (const auto &n : nodes_) {
        if (const auto *cm = dynamic_cast<ContextMenuNode *>(n.get())) {
            cm->close();
        }
    }
}

auto Modifier::active_context_menu_items() const -> std::vector<MenuItem> {
    for (const auto &n : nodes_) {
        if (const auto *cm = dynamic_cast<const ContextMenuNode *>(n.get())) {
            if (cm->is_open()) {
                return cm->items();
            }
        }
    }
    return {};
}

auto Modifier::active_context_menu_position() const -> Point {
    for (const auto &n : nodes_) {
        if (const auto *cm = dynamic_cast<const ContextMenuNode *>(n.get())) {
            if (cm->is_open()) {
                return cm->position();
            }
        }
    }
    return Point{.x = 0.0F, .y = 0.0F};
}

}  // namespace aurora
