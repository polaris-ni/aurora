#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include "aurora/state/effect.h"
#include "aurora/state/reactive.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 响应式构建原语：按当前布局约束动态构建子节点（Flutter `LayoutBuilder` 语义）。
///
/// 典型用法：在 `builder` 闭包内读取 `media_query_of(ctx)`，依据 `constraints` 选择不同子树，
/// 实现响应式布局。为避免每帧重建，仅在「约束显著变化」或「builder 闭包被替换」时才重建并
/// 重新 mount 子节点；约束不变则复用缓存的子树。
///
/// @note `builder` 为 `Reactive<std::function<...>>`：闭包本身变化会触发整棵子树重建。
/// 闭包内部读取的信号（如 `media_query_of`）不自动订阅——响应式驱动来自约束变化
/// （如窗口缩放），或外部显式替换 `builder`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class LayoutBuilder : public Widget {
  public:
    /// @brief 纯展示件默认不是 Tab 停点：只有挂上点击 / 手势 / 菜单 / 滚动 / 键盘认领时
    ///        才可聚焦（覆写基类 public virtual；分级默认见 specification/05 §4.2）。
    /// @return 具备输入语义时为 true，否则 false。
    [[nodiscard]] auto wants_focus() const -> bool override { return has_input_semantics(); }
    /// @brief 构建回调：给定 (BuildContext, 当前 Constraints) 返回一棵子树。
    /// @param BuildContext 回调入参类型：构建上下文引用。
    /// @param Constraints 回调入参类型：当前布局约束引用。
    using BuilderFn = std::function<Node(const BuildContext &, const Constraints &)>;

    /// @brief 构建回调的响应式信号：约束变化或闭包替换时重建子树。
    /// NOLINTNEXTLINE(*-non-private-member-variables-in-classes) 公开响应式字段：宿主需直读直改 builder 闭包。
    Reactive<BuilderFn> builder;  ///< 约束变化时重建子节点（可热替换）。

    /// @brief 默认构造：builder 为空闭包，首轮布局生成空子节点，外部替换闭包后重建。
    LayoutBuilder() = default;

    /// @brief 便捷构造：直接传入构建闭包。
    /// @param fn 构建闭包，移动存入 builder。
    explicit LayoutBuilder(BuilderFn fn) : builder(std::move(fn)) {}

    /// @brief 类型名，供序列化与运行时自描述使用。
    /// @return C 字符串 "LayoutBuilder"。
    [[nodiscard]] auto type_name() const -> const char * override { return "LayoutBuilder"; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return LayoutBuilder 的控件描述符（尺寸属性、single 子策略与示例）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "LayoutBuilder",
            .properties =
                {
                    {.name = "width", .type = "Length", .default_value = "auto", .required = false},
                    {.name = "height", .type = "Length", .default_value = "auto", .required = false},
                    {.name = "show", .type = "bool", .default_value = "true", .required = false},
                },
            .events = {},
            .children_policy = "single",
            .examples = {"au::LayoutBuilder([](const BuildContext& ctx, const Constraints& c){ return Node{ "
                         "au::Text(\"responsive\") }; })"},
        };
    }
    /// @brief 运行时自描述：转发静态描述符。
    /// @return 本控件的静态描述符。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集本控件的可订阅信号视图：收录 builder 响应式信号。
    /// @param out 输出参数，收录本控件持有的 SignalViewBase 指针。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&builder); }

    /// @brief 子节点视图：单孩子策略下返回仅含 child_ 的缓存向量（无子时为空）。
    /// @return 视图缓存引用。
    [[nodiscard]] auto child_nodes() const -> const std::vector<Node> & override {
        child_view_.clear();
        if (child_) {
            child_view_.push_back(child_);
        }
        return child_view_;
    }

    /// @brief 遍历子控件：存在唯一子节点时对其调用回调。
    /// @param fn 子控件访问回调。
    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        if (child_) {
            fn(child_.widget());
        }
    }

  protected:
    auto on_mount(const BuildContext &ctx) -> void override {
        Widget::on_mount(ctx);
        // builder 闭包替换 → 标记脏并请求重建（清除旧子节点引用）。
        builder_effect_ = std::make_shared<Effect>([this] {  // NOLINT(*-use-trailing-return-type)
            dirty_ = true;
            mark_needs_layout();
            mark_needs_paint();
        });
        builder.subscribe(*builder_effect_);
    }

    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        const BuilderFn &fn = builder.get();
        const bool constraints_changed = (last_.min.width != c.min.width) || (last_.min.height != c.min.height) ||
                                         (last_.max.width != c.max.width) || (last_.max.height != c.max.height);
        if (!child_ || dirty_ || constraints_changed) {
            if (fn) {
                child_ = fn(ctx, c);
            } else {
                child_ = Node{};
            }
            last_ = c;
            dirty_ = false;
            if (child_) {
                child_.widget().mount(ctx);
            }
        }
        if (!child_) {
            return Size{.width = 0.0F, .height = 0.0F};
        }
        child_.widget().set_layout_parent(this);
        return child_.widget().layout(c, ctx);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (child_) {
            child_.widget().paint(p, bounds, ctx);
        }
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        if (!child_) {
            return nullptr;
        }
        return child_.widget().hit_test(local, bounds, ctx);
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        if (!child_) {
            return {};
        }
        return child_.widget().hit_test_chain(local, bounds, ctx);
    }

    /// @brief 逐帧驱动：转发 tick 给子控件（builder 子树内的动画依赖此推进）。
    /// @param now 当前墙钟时刻。
    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Widget::tick_gestures(now);
        if (child_) {
            child_.widget().tick(now);
        }
    }

  private:
    Node child_;
    /// @brief child_nodes() 视图缓存（const 方法返回引用需持久存储）。
    mutable std::vector<Node> child_view_;
    Constraints last_{};
    bool dirty_ = true;  ///< 首次构建 / 闭包替换后置位。
    std::shared_ptr<Effect> builder_effect_;
};

}  // namespace aurora
