#pragma once

#include <functional>
#include <memory>

#include "aurora/environment/media_query.h"
#include "aurora/state/read_only_state.h"
#include "aurora/state/subscription.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 断点档位（响应式原语；参考 Material 窗口尺寸等级的常用简化）。
///
/// 由可用宽度解析：`width < medium_max_width` → `Compact`；
/// `medium_max_width <= width < expanded_min_width` → `Medium`；`width >= expanded_min_width` → `Expanded`。
///
enum class Breakpoint : std::uint8_t { Compact, Medium, Expanded };

/// @brief 按宽度与阈值解析断点档位（纯函数，供控件与测试复用）。
/// @param width 当前可用宽度（dp）。
/// @param medium_max Compact 上界：小于该值判为 Compact。
/// @param expanded_min Expanded 下界：不小于该值判为 Expanded；其间为 Medium。
/// @return 宽度落入区间对应的断点档位。
[[nodiscard]] constexpr auto resolve_breakpoint(float width, float medium_max, float expanded_min) -> Breakpoint {
    if (width < medium_max) {
        return Breakpoint::Compact;
    }
    if (width < expanded_min) {
        return Breakpoint::Medium;
    }
    return Breakpoint::Expanded;
}

/// @brief 断点感知容器（响应式原语）：按当前可用宽度档位构建不同子树。
///
/// 宽度来源：优先读最近祖先 `Provider<MediaQuery>` 注入的窗口/子树逻辑宽度
/// （`media_query_of(ctx)`）；无注入时退化为父约束最大宽度（仍有限则取之，无限视为 0 = Compact）。
/// 阈值可调（`medium_max_width` / `expanded_min_width`，默认 600 / 840，对应 Material 简化档）。
///
/// 重建时机与 `LayoutBuilder` 同口径：仅在「断点档位变化」或「builder 闭包被替换」时重建并
/// 重新 mount 子节点；同档位内的约束变化只重新布局、不重建子树（避免每帧重建）。
/// @note Thread: main-thread only
/// @note Rebuildable: no（builder 为运行时回调，工厂注册仅收录自描述元数据）
///
class BreakpointBuilder : public Widget {
  public:
    /// @brief 纯展示件默认不是 Tab 停点：只有挂上点击 / 手势 / 菜单 / 滚动 / 键盘认领时
    ///        才可聚焦（覆写基类 public virtual；分级默认见 specification/05 §4.2）。
    /// @return 具备输入语义时为 true，否则 false。
    [[nodiscard]] auto wants_focus() const -> bool override { return has_input_semantics(); }
    /// @brief 构建回调：给定解析出的断点档位返回一棵子树。
    /// @param Breakpoint 回调入参类型：当前断点档位值。
    using BuilderFn = std::function<Node(Breakpoint)>;

    /// @brief Compact 上界（width < 该值 → Compact）。
    float medium_max_width = 600.0F;
    /// @brief Expanded 下界（width >= 该值 → Expanded；其间为 Medium）。
    float expanded_min_width = 840.0F;

    /// @brief 默认构造：构建闭包为空，首轮布局构建空子节点，外部换源后重建。
    BreakpointBuilder() = default;

    /// @brief 便捷构造：直接传入构建闭包（包成自有常量来源安装）。
    /// @param fn 构建闭包，移入安装来源。
    explicit BreakpointBuilder(BuilderFn fn) { install_builder(ReadOnlyState<BuilderFn>{std::move(fn)}); }

    /// @brief 构建闭包来源（只读句柄）：供宿主接走或另行订阅。
    /// @return 当前安装的构建闭包读句柄。
    [[nodiscard]] auto builder_source() const -> ReadOnlyState<BuilderFn> { return builder_; }

    /// @brief 替换构建闭包来源（展示属性换源）：置脏并请求整棵子树重建。
    /// @param next 新的构建闭包读句柄（可由 `BuilderFn` 字面量隐式转换）。
    /// @return 自身引用，便于链式调用。
    auto set_builder_source(ReadOnlyState<BuilderFn> next) -> BreakpointBuilder & {
        install_builder(std::move(next));
        return *this;
    }

    /// @brief 以字面量替换构建闭包（换到一份自有常量来源）。
    /// @param fn 新的构建闭包。
    /// @return 自身引用，便于链式调用。
    auto set_builder(BuilderFn fn) -> BreakpointBuilder & {
        install_builder(ReadOnlyState<BuilderFn>{std::move(fn)});
        return *this;
    }

    /// @brief 类型名，供序列化与运行时自描述使用。
    /// @return C 字符串 "BreakpointBuilder"。
    [[nodiscard]] auto type_name() const -> const char * override { return "BreakpointBuilder"; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return BreakpointBuilder 的控件描述符（尺寸/阈值属性、single 子策略与示例）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "BreakpointBuilder",
            .properties =
                {
                    {.name = "width", .type = "Length", .default_value = "auto", .required = false},
                    {.name = "height", .type = "Length", .default_value = "auto", .required = false},
                    {.name = "show", .type = "bool", .default_value = "true", .required = false},
                    {.name = "medium_max_width",
                     .type = "float",
                     .default_value = "600.0",
                     .required = false,
                     .note = "Compact upper bound (dp)"},
                    {.name = "expanded_min_width",
                     .type = "float",
                     .default_value = "840.0",
                     .required = false,
                     .note = "Expanded lower bound (dp)"},
                },
            .events = {},
            .children_policy = "single",
            .examples = {"au::BreakpointBuilder([](au::Breakpoint bp){ return au::Text(bp == au::Breakpoint::Compact "
                         "? \"phone\" : \"wide\"); })"},
        };
    }
    /// @brief 运行时自描述：转发静态描述符。
    /// @return 本控件的静态描述符。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集本控件的可订阅信号视图：收录构建闭包来源。
    /// @param out 输出参数，收录本控件持有的构建闭包读句柄。
    auto collect_signals(StateSink &out) -> void override { out.push(builder_); }

    /// @brief 当前生效档位（布局后有效；供测试与外部读取）。
    /// @return 最近一次 on_layout 解析出的断点档位。
    [[nodiscard]] auto current_breakpoint() const -> Breakpoint { return last_bp_; }

    /// @brief 关闭布局缓存：on_layout 依赖 MediaQuery 注入宽度（窗口宽度可变而子树约束不变），
    ///        缓存命中会跳过 on_layout 导致断点档位冻结（与 Skeleton 同理由）。
    /// @return 恒为 false（本控件不参与布局缓存）。
    [[nodiscard]] auto can_cache_layout() const -> bool override { return false; }

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
    /// 子节点由 `on_layout` 在断点重算后挂载（父侧 ctx），不经 `on_mount`，故这里显式回收。
    /// @param ctx 本控件挂载时记录的那份上下文。
    auto on_unmount(const BuildContext &ctx) -> void override {
        (void)ctx;
        builder_subscription_.reset();  // 退订：之后替换闭包不再驱动重建
        if (child_) {
            child_.widget().unmount();
        }
    }

    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        // 档位宽度来源：MediaQuery 注入优先（真窗口/子树宽度），退化取父约束最大宽（无限视 0）。
        const MediaQuery *mq = media_query_of(ctx);
        const float width =
            mq != nullptr ? mq->size.width : (c.max.width != Size::infinity().width ? c.max.width : 0.0F);
        const Breakpoint bp = resolve_breakpoint(width, medium_max_width, expanded_min_width);

        const BuilderFn &fn = builder_.get();
        if (!child_ || dirty_ || bp != last_bp_) {
            if (fn) {
                child_ = fn(bp);
            } else {
                child_ = Node{};
            }
            last_bp_ = bp;
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
    Breakpoint last_bp_ = Breakpoint::Compact;  ///< 最近一次解析的档位（重建判定依据）。
    bool dirty_ = true;  ///< 首次构建 / 闭包换源后置位。
    ReadOnlyState<BuilderFn> builder_;  ///< 安装的构建闭包来源（只读句柄）
    Subscription builder_subscription_;  ///< 挂载期建立的构建闭包订阅（卸载即取消）
};

}  // namespace aurora
