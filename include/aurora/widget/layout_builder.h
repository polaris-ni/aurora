#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include "aurora/state/effect.h"
#include "aurora/state/reactive.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 响应式构建原语：按当前布局约束动态构建子节点（Flutter `LayoutBuilder` 语义）。
 *
 * 典型用法：在 `builder` 闭包内读取 `media_query_of(ctx)`，依据 `constraints` 选择不同子树，
 * 实现响应式布局。为避免每帧重建，仅在「约束显著变化」或「builder 闭包被替换」时才重建并
 * 重新 mount 子节点；约束不变则复用缓存的子树。
 *
 * @note `builder` 为 `Reactive<std::function<...>>`：闭包本身变化会触发整棵子树重建。
 *       闭包内部读取的信号（如 `media_query_of`）不自动订阅——响应式驱动来自约束变化
 *       （如窗口缩放），或外部显式替换 `builder`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class LayoutBuilder : public Widget {
  public:
    /// @brief 构建回调：给定 (BuildContext, 当前 Constraints) 返回一棵子树。
    using BuilderFn = std::function<Node(const BuildContext &, const Constraints &)>;

    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    Reactive<BuilderFn> builder; ///< 约束变化时重建子节点（可热替换）。

    LayoutBuilder() = default;

    /// @brief 便捷构造：直接传入构建闭包。
    explicit LayoutBuilder(BuilderFn fn) : builder(std::move(fn)) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "LayoutBuilder"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "LayoutBuilder",
            .properties = {
                { .name="width", .type="Length", .default_value="auto", .required=false },
                { .name="height", .type="Length", .default_value="auto", .required=false },
                { .name="show", .type="bool", .default_value="true", .required=false },
            },
            .events = {},
            .children_policy = "single",
            .examples = { "au::LayoutBuilder([](const BuildContext& ctx, const Constraints& c){ return Node{ au::Text(\"responsive\") }; })" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&builder); }

    [[nodiscard]] auto child_nodes() const -> const std::vector<Node> & override {
        m_child_view.clear();
        if (m_child) {
            m_child_view.push_back(m_child);
        }
        return m_child_view;
    }

    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        if (m_child) {
            fn(m_child.widget());
        }
    }

  protected:
    auto on_mount(const BuildContext &ctx) -> void override {
        Widget::on_mount(ctx);
        // builder 闭包替换 → 标记脏并请求重建（清除旧子节点引用）。
        m_builder_effect = std::make_shared<Effect>([this] { // NOLINT(*-use-trailing-return-type)
            m_dirty = true;
            mark_needs_layout();
            mark_needs_paint();
        });
        builder.subscribe(*m_builder_effect);
    }

    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        const BuilderFn &fn = builder.get();
        const bool constraints_changed = (m_last.min.width != c.min.width) || (m_last.min.height != c.min.height) ||
                                         (m_last.max.width != c.max.width) || (m_last.max.height != c.max.height);
        if (!m_child || m_dirty || constraints_changed) {
            if (fn) {
                m_child = fn(ctx, c);
            } else {
                m_child = Node{};
            }
            m_last = c;
            m_dirty = false;
            if (m_child) {
                m_child.widget().mount(ctx);
            }
        }
        if (!m_child) {
            return Size{ .width = 0.0f, .height = 0.0f };
        }
        m_child.widget().set_layout_parent(this);
        return m_child.widget().layout(c, ctx);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (m_child) {
            m_child.widget().paint(p, bounds, ctx);
        }
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        if (!m_child) {
            return nullptr;
        }
        return m_child.widget().hit_test(local, bounds, ctx);
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        if (!m_child) {
            return {};
        }
        return m_child.widget().hit_test_chain(local, bounds, ctx);
    }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Widget::tick_gestures(now);
        if (m_child) {
            m_child.widget().tick(now);
        }
    }

  private:
    Node m_child;
    /// @brief child_nodes() 视图缓存（const 方法返回引用需持久存储）。
    mutable std::vector<Node> m_child_view;
    Constraints m_last{};
    bool m_dirty = true; ///< 首次构建 / 闭包替换后置位。
    std::shared_ptr<Effect> m_builder_effect;
};

} // namespace aurora
