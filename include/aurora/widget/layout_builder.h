#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include "aurora/state/read_only_state.h"
#include "aurora/state/subscription.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 响应式构建原语：按当前布局约束动态构建子节点（Flutter `LayoutBuilder` 语义）。
///
/// 典型用法：在 `builder` 闭包内读取 `media_query_of(ctx)`，依据 `constraints` 选择不同子树，
/// 实现响应式布局。为避免每帧重建，仅在「约束显著变化」或「builder 闭包被替换」时才重建并
/// 重新 mount 子节点；约束不变则复用缓存的子树。
///
/// @note 构建闭包以只读句柄 `ReadOnlyState<BuilderFn>` 安装：换源（`set_builder`）会触发整棵子树重建。
/// 闭包内部读取的信号（如 `media_query_of`）不自动订阅——响应式驱动来自约束变化
/// （如窗口缩放），或外部显式替换构建闭包。
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

    /// @brief 默认构造：构建闭包为空，首轮布局生成空子节点，外部换源后重建。
    LayoutBuilder() = default;

    /// @brief 便捷构造：直接传入构建闭包（包成自有常量来源安装）。
    /// @param fn 构建闭包，移入安装来源。
    explicit LayoutBuilder(BuilderFn fn) { install_builder(ReadOnlyState<BuilderFn>{std::move(fn)}); }

    /// @brief 构建闭包来源（只读句柄）：供宿主接走或另行订阅。
    /// @return 当前安装的构建闭包读句柄。
    [[nodiscard]] auto builder_source() const -> ReadOnlyState<BuilderFn> { return builder_; }

    /// @brief 替换构建闭包来源（展示属性换源）：置脏并请求整棵子树重建。
    /// @param next 新的构建闭包读句柄（可由 `BuilderFn` 字面量隐式转换）。
    /// @return 自身引用，便于链式调用。
    auto set_builder_source(ReadOnlyState<BuilderFn> next) -> LayoutBuilder & {
        install_builder(std::move(next));
        return *this;
    }

    /// @brief 以字面量替换构建闭包（换到一份自有常量来源）。
    /// @param fn 新的构建闭包。
    /// @return 自身引用，便于链式调用。
    auto set_builder(BuilderFn fn) -> LayoutBuilder & {
        install_builder(ReadOnlyState<BuilderFn>{std::move(fn)});
        return *this;
    }

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

    /// @brief 收集本控件的可订阅信号视图：收录构建闭包来源。
    /// @param out 输出参数，收录本控件持有的构建闭包读句柄。
    auto collect_signals(StateSink &out) -> void override { out.push(builder_); }

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
        // 构建闭包来源变化 → 标记脏并请求重建（下一帧 on_layout 重建子树）。
        builder_subscription_ = builder_.subscribe([this](const BuilderFn &) {
            dirty_ = true;
            mark_needs_layout();
            mark_needs_paint();
        });
    }

    /// @brief 释放挂载期建的 builder 订阅并卸载当前子节点（与 `on_mount` 对称）。
    ///
    /// 子节点由 `on_layout` 在重建时挂载（父侧 ctx），不经 `on_mount`，故这里显式回收：换宿主重挂后
    /// `child_` 会被下一次重建替换，在替换前它仍持着旧宿主派生的订阅。
    /// @param ctx 本控件挂载时记录的那份上下文。
    auto on_unmount(const BuildContext &ctx) -> void override {
        (void)ctx;
        builder_subscription_.reset();  // 退订：之后替换闭包不再驱动重建
        if (child_) {
            child_.widget().unmount();
        }
    }

    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        const BuilderFn &fn = builder_.get();
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
    /// @brief 安装构建闭包来源：置脏、标记重排重绘，并重建挂载期订阅（旧订阅固定旧源）。
    /// @param next 新的构建闭包读句柄。
    auto install_builder(ReadOnlyState<BuilderFn> next) -> void {
        const bool subscribed = builder_subscription_.active();
        builder_subscription_.reset();  // 旧来源的局部订阅先解除，避免固定到换下的来源
        builder_ = std::move(next);
        dirty_ = true;  // 下一帧 on_layout 无条件重建子树
        mark_needs_layout();
        mark_needs_paint();
        if (subscribed) {
            builder_subscription_ = builder_.subscribe([this](const BuilderFn &) {
                dirty_ = true;
                mark_needs_layout();
                mark_needs_paint();
            });
        }
        resubscribe_signals();  // 未挂载时为空操作；挂载后重建采集订阅
    }

    Node child_;
    /// @brief child_nodes() 视图缓存（const 方法返回引用需持久存储）。
    mutable std::vector<Node> child_view_;
    Constraints last_{};
    bool dirty_ = true;  ///< 首次构建 / 闭包换源后置位。
    ReadOnlyState<BuilderFn> builder_;  ///< 安装的构建闭包来源（只读句柄）
    Subscription builder_subscription_;  ///< 挂载期建立的构建闭包订阅（卸载即取消）
};

}  // namespace aurora
