#pragma once

#include <functional>
#include <memory>

#include "aurora/environment/media_query.h"
#include "aurora/state/effect.h"
#include "aurora/state/reactive.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 断点档位（F2 响应式原语；参考 Material 窗口尺寸等级的常用简化）。
 *
 * 由可用宽度解析：`width < medium_max_width` → `Compact`；
 * `medium_max_width <= width < expanded_min_width` → `Medium`；`width >= expanded_min_width` → `Expanded`。
 */
enum class Breakpoint : std::uint8_t { Compact, Medium, Expanded };

/// @brief 按宽度与阈值解析断点档位（纯函数，供控件与测试复用）。
[[nodiscard]] constexpr auto resolve_breakpoint(float width, float medium_max, float expanded_min) -> Breakpoint {
    if (width < medium_max) {
        return Breakpoint::Compact;
    }
    if (width < expanded_min) {
        return Breakpoint::Medium;
    }
    return Breakpoint::Expanded;
}

/**
 * @brief 断点感知容器（F2 响应式原语）：按当前可用宽度档位构建不同子树。
 *
 * 宽度来源：优先读最近祖先 `Provider<MediaQuery>` 注入的窗口/子树逻辑宽度
 * （`media_query_of(ctx)`）；无注入时退化为父约束最大宽度（仍有限则取之，无限视为 0 = Compact）。
 * 阈值可调（`medium_max_width` / `expanded_min_width`，默认 600 / 840，对应 Material 简化档）。
 *
 * 重建时机与 `LayoutBuilder` 同口径：仅在「断点档位变化」或「builder 闭包被替换」时重建并
 * 重新 mount 子节点；同档位内的约束变化只重新布局、不重建子树（避免每帧重建）。
 * @note Thread: main-thread only
 * @note Rebuildable: no（builder 为运行时回调，工厂注册仅收录自描述元数据）
 */
class BreakpointBuilder : public Widget {
  public:
    /// @brief 构建回调：给定解析出的断点档位返回一棵子树。
    using BuilderFn = std::function<Node(Breakpoint)>;

    // NOLINTNEXTLINE(*-non-private-member-variables-classes) 同 LayoutBuilder：props 直读直改
    Reactive<BuilderFn> builder;  ///< 档位变化 / 闭包替换时重建子节点。

    /// @brief Compact 上界（width < 该值 → Compact）。
    float medium_max_width = 600.0F;
    /// @brief Expanded 下界（width >= 该值 → Expanded；其间为 Medium）。
    float expanded_min_width = 840.0F;

    BreakpointBuilder() = default;

    /// @brief 便捷构造：直接传入构建闭包。
    explicit BreakpointBuilder(BuilderFn fn) : builder(std::move(fn)) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "BreakpointBuilder"; }

    /// @brief 运行时自描述（规格附录 B）。
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
                     .note = "Compact 上界(dp)"},
                    {.name = "expanded_min_width",
                     .type = "float",
                     .default_value = "840.0",
                     .required = false,
                     .note = "Expanded 下界(dp)"},
                },
            .events = {},
            .children_policy = "single",
            .examples = {"au::BreakpointBuilder([](au::Breakpoint bp){ return au::Text(bp == au::Breakpoint::Compact "
                         "? \"phone\" : \"wide\"); })"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&builder); }

    /// @brief 当前生效档位（布局后有效；供测试与外部读取）。
    [[nodiscard]] auto current_breakpoint() const -> Breakpoint { return last_bp_; }

    /// @brief 关闭布局缓存：on_layout 依赖 MediaQuery 注入宽度（窗口宽度可变而子树约束不变），
    ///        缓存命中会跳过 on_layout 导致断点档位冻结（与 Skeleton 同理由）。
    [[nodiscard]] auto can_cache_layout() const -> bool override { return false; }

    [[nodiscard]] auto child_nodes() const -> const std::vector<Node> & override {
        child_view_.clear();
        if (child_) {
            child_view_.push_back(child_);
        }
        return child_view_;
    }

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
        // 档位宽度来源：MediaQuery 注入优先（真窗口/子树宽度），退化取父约束最大宽（无限视 0）。
        const MediaQuery *mq = media_query_of(ctx);
        const float width = mq != nullptr ? mq->size.width
                                          : (c.max.width != Size::infinity().width ? c.max.width : 0.0F);
        const Breakpoint bp = resolve_breakpoint(width, medium_max_width, expanded_min_width);

        const BuilderFn &fn = builder.get();
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
    Breakpoint last_bp_ = Breakpoint::Compact;  ///< 最近一次解析的档位（重建判定依据）。
    bool dirty_ = true;  ///< 首次构建 / 闭包替换后置位。
    std::shared_ptr<Effect> builder_effect_;
};

}  // namespace aurora
