#pragma once

#include <utility>

#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 吸顶头部包装器（specification/04-widget.md）：包裹任意子树，声明其滚动经过视口
 * 顶部时**钉驻**在原位置，后续头部依次向下堆叠（iOS/Android 列表分组头语义）。
 *
 * 实现为**纯绘制层覆盖**：控件正常参与布局（占据自身高度、随内容滚动被录入内容缓冲），
 * 滚动宿主（`Scroll` / `LazyList`）在 blit 合成之后把「已滚过头顶」的本控件按 pin 位
 * 重绘于顶部——内容缓冲不因此逐帧重录（`is_sticky_header()` 是宿主识别本控件的虚钩子）。
 *
 * 对标 Flutter `SliverPersistentHeader(pinned: true)`、CSS `position: sticky`、
 * Android `RecyclerView` sticky headers。
 *
 * @note Thread: main-thread only
 * @note Rebuildable: yes（无自有属性；子节点经通用 children 路径还原）
 */
class StickyHeader : public SingleChild {
  public:
    StickyHeader() = default;
    explicit StickyHeader(Node child) : SingleChild(std::move(child)) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "StickyHeader"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "StickyHeader",
            .properties = {},
            .events = {},
            .children_policy = "single",
            .examples = {"au::StickyHeader(au::Text(\"分组标题\")) /* Scroll 内容中滚动时钉驻顶部 */"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 宿主识别钩子：滚动控件据此在覆盖层按 pin 位重绘本控件。
    [[nodiscard]] auto is_sticky_header() const -> bool override { return true; }

  protected:
    /// @brief 交叉轴撑满、主轴取子项自然高（分组头典型形态：整行高亮条）。
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        Size self = c.max;
        if (!c.max.is_finite()) {
            self = Size{.width = 320.0F, .height = 48.0F};
        }
        if (child_) {
            Constraints inner{.min = Size{.width = self.width, .height = 0.0F},
                              .max = Size{.width = self.width, .height = Size::infinity().height}};
            const Size kid = child_.widget().layout(inner, ctx);
            self = Size{.width = self.width, .height = kid.height};
        }
        child_.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = self});
        return c.constrain(self);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (child_) {
            child_.widget().paint(p, bounds, ctx);
        }
    }
};

}  // namespace aurora
