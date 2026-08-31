#pragma once

#include <memory>
#include <vector>

#include "aurora/core/diagnostics.h"
#include "aurora/core/types.h"
#include "aurora/state/state.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 动态列表（REQUIREMENTS §4.1 / 规格 §4.1.1）：把 `State<std::vector<T>>` 的每一项
 * 经 `itemBuilder` 展开为一个子 widget。容器尺寸随数据项变化，依赖变化时触发定点刷新。
 *
 * @code
 *   auto items = std::make_shared<State<std::vector<std::string>>>(std::vector<std::string>{"A","B","C"});
 *   Repeater<std::string>{items, [](const std::string& s, int i){
 *       return Node{ au::Text(s) };
 *   }};
 * @endcode
 *
 * @tparam T 列表项类型（须可拷贝）。
 */
template<typename T> class Repeater : public Container {
  public:
    using ItemBuilder = std::function<Node(const T &, int)>;

    Repeater(std::shared_ptr<State<std::vector<T>>> items, ItemBuilder builder)
        : m_items(std::move(items)), m_builder(std::move(builder)) {
        rebuild_if_needed();
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Repeater"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Repeater",
            .properties = {
                { .name = "width", .type = "Length", .default_value = "auto", .required = false },
                { .name = "height", .type = "Length", .default_value = "auto", .required = false },
                { .name = "show", .type = "bool", .default_value = "true", .required = false },
            },
            .events = {},
            .children_policy = "multiple",
            .examples = { "au::Repeater<std::string>{items, [](const std::string& s, int i){ return Node{ au::Text(s) }; }}" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 当前允许的子树最大深度（默认 `AURORA_DEFAULT_MAX_WIDGET_DEPTH`）。
    [[nodiscard]] auto max_depth() const -> std::size_t { return m_max_depth; }
    /// @brief 设置子树最大深度；超限展开经 `Diagnostics` 截断（规格 §2.4）。
    auto set_max_depth(std::size_t d) -> void { m_max_depth = d; }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        if (m_items) {
            out.push_back(static_cast<SignalViewBase *>(m_items.get()));
        }
    }
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["note"] = "Repeater items are runtime-state driven, not serialized";
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        rebuild_if_needed();
        for (Node &child : m_children) {
            child.widget().set_layout_parent(this);
        }
        float max_w = 0.0f;
        float total_h = 0.0f;
        for (Node &child : m_children) {
            const Size s = child.widget().layout(c, ctx);
            max_w = std::max(max_w, s.width);
            total_h += s.height;
        }
        max_w = std::max(c.min.width, std::min(max_w, c.max.width));
        total_h = std::max(c.min.height, std::min(total_h, c.max.height));
        return c.constrain(Size{ .width = max_w, .height = total_h });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        float y = bounds.origin.y;
        for (Node &child : m_children) {
            const Size s = child.widget().size();
            Rect child_bounds{ .origin = Point{ .x = bounds.origin.x, .y = y }, .size = s };
            child.widget().paint(p, child_bounds, ctx);
            y += s.height;
        }
    }

  private:
    // 仅在数据源 size 变化（或尚未构建）时重建子节点，避免每次布局都重新构造全部 widget。
    auto rebuild_if_needed() -> void {
        if (!m_items || !m_builder) {
            return;
        }
        const std::size_t n = m_items->get().size();
        if (m_built && n == m_child_count) {
            return;
        }
        const std::vector<T> &data = m_items->get();
        std::vector<Node> kids;
        kids.reserve(data.size());
        for (std::size_t i = 0; i < data.size(); ++i) {
            kids.push_back(m_builder(data[i], static_cast<int>(i)));
        }
        m_children = std::move(kids);
        m_child_count = data.size();
        m_built = true;

        const std::size_t d = widget_tree_depth(*this);
        if (d > m_max_depth) {
            Diagnostics::degraded("repeater",
                                  "Repeater subtree depth " + std::to_string(d) + " exceeds max_depth " +
                                      std::to_string(m_max_depth) + "; truncating expansion",
                                  "repeater.h:rebuild_if_needed");
            m_children.clear();
        }
    }

    std::shared_ptr<State<std::vector<T>>> m_items;
    ItemBuilder m_builder;
    std::size_t m_child_count = 0;
    bool m_built = false;
    std::size_t m_max_depth = AURORA_DEFAULT_MAX_WIDGET_DEPTH;

    /// @brief 递归统计 widget 子树深度（含自身），用于有界层深度守卫（§2.4）。
    [[nodiscard]] static auto widget_tree_depth(const Widget &w) -> std::size_t {
        std::size_t best = 0;
        for (const Node &c : w.child_nodes()) {
            best = std::max(best, widget_tree_depth(c.widget()));
        }
        return 1 + best;
    }
};

} // namespace aurora
