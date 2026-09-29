#pragma once

#include <utility>

#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 吸顶头部包装器（specification/04-widget.md）：包裹任意子树，声明其滚动经过视口
/// 顶部时**钉驻**在原位置，后续头部依次向下堆叠（iOS/Android 列表分组头语义）。
///
/// 实现为**纯绘制层覆盖**：控件正常参与布局（占据自身高度、随内容滚动被录入内容缓冲），
/// 滚动宿主（`Scroll` / `LazyList`）在 blit 合成之后把「已滚过头顶」的本控件按 pin 位
/// 重绘于顶部——内容缓冲不因此逐帧重录（`is_sticky_header()` 是宿主识别本控件的虚钩子）。
///
/// 对标 Flutter `SliverPersistentHeader(pinned: true)`、CSS `position: sticky`、
/// Android `RecyclerView` sticky headers。
///
/// @note Thread: main-thread only
/// @note Rebuildable: yes（无自有属性；子节点经通用 children 路径还原）
///
class StickyHeader : public SingleChild {
  public:
    /// @brief 默认构造：无子节点（可后续经 adopt_children 挂接）。
    StickyHeader() = default;
    /// @brief 以单一子树构造吸顶头。
    /// @param child 被包裹的子节点，移动接管。
    explicit StickyHeader(Node child) : SingleChild(std::move(child)) {}

    /// @brief 类型名，供序列化与运行时自描述使用。
    /// @return C 字符串 "StickyHeader"。
    [[nodiscard]] auto type_name() const -> const char * override { return "StickyHeader"; }

    /// @brief 静态运行时自描述（规格附录 B）。
    /// @return StickyHeader 的控件描述符（无自有属性，single 子策略与示例）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "StickyHeader",
            .properties = {},
            .events = {},
            .children_policy = "single",
            .examples = {"au::StickyHeader(au::Text(\"Section title\")) /* pinned to the top while scrolling */"},
        };
    }
    /// @brief 运行时自描述：转发静态描述符。
    /// @return StickyHeader 的控件描述符（无自有属性，single 子策略与示例）。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集本控件的可订阅信号视图。
    /// @param out 输出参数，收集 SignalViewBase 指针；StickyHeader 无信号，恒不写入。
    auto collect_signals([[maybe_unused]] std::vector<SignalViewBase *> &out) -> void override {}

    /// @brief 宿主识别钩子：滚动控件据此在覆盖层按 pin 位重绘本控件。
    /// @return 恒为 true（本控件即吸顶头）。
    [[nodiscard]] auto is_sticky_header() const -> bool override { return true; }

  protected:
    /// @brief 交叉轴撑满、主轴取子项自然高（分组头典型形态：整行高亮条）。
    /// @param c 父约束：有限时取 max 为自身尺寸，非有限时回落 320×48 缺省；结果经 constrain 夹取。
    /// @param ctx 构建上下文，透传给子项 layout。
    /// @return 夹取后的自身尺寸（高度为子项自然高）。
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

    /// @brief 透传绘制：把子控件画在本控件 bounds 内；钉驻覆盖由滚动宿主完成。
    /// @param p 画布绘制器。
    /// @param bounds 本控件布局矩形。
    /// @param ctx 构建上下文。
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (child_) {
            child_.widget().paint(p, bounds, ctx);
        }
    }
};

}  // namespace aurora
