#pragma once

/// @file modifier.h
/// @brief 修饰系统聚合入口：包含基类 + 全部功能切片 + Modifier 工厂链。
///
/// 消费者通常只需 `#include "aurora/modifier/modifier.h"` 即可获得全部修饰能力。
/// 需要单独引用的场景可按切片引入：
/// - `modifier_base.h`：ModifierNode 基类 + Kind/PaintKind 枚举
/// - `modifier_layout.h`：Padding / PaddingEdges / FlexWeight / SizeModifier
/// - `modifier_transform.h`：AlignNode / OffsetNode / TransformNode
/// - `modifier_paint.h`：Background / GradientBackground / ShadowNode / BlendNode /
///   ShaderMaskNode / CacheLayerNode / Border / Clip / ClipRounded / OpacityNode / BlurNode
/// - `modifier_input.h`：Clickable / Draggable / LongPress / TouchListener / TooltipNode / ContextMenuNode

// clang-format off
// 注意：blend.h 必须在 modifier_paint.h 之前引入——modifier_paint.h 中的 BlendNode /
// ShaderMaskNode 直接使用 BlendMode / ShaderMaskKind，否则会因类型不完整导致编译失败。
// 禁止 clang-format 对该组 include 重新排序。
#include "aurora/core/color.h"
#include "aurora/core/diagnostics.h"
#include "aurora/core/transform.h"
#include "aurora/render/blend.h"
#include "aurora/modifier/modifier_base.h"
#include "aurora/modifier/modifier_input.h"
#include "aurora/modifier/modifier_layout.h"
#include "aurora/modifier/modifier_paint.h"
#include "aurora/modifier/modifier_transform.h"
// clang-format on

namespace aurora {

/**
 * @brief 修饰链：有序的修饰节点集合，挂在 widget 的 `modifier` 属性上。
 *
 * 工厂方法返回副本（基于 shared_ptr），支持链式 `Modifier{}.padding(8).background(c)`。
 */
class Modifier {
  public:
    Modifier() = default;

    /// @brief 追加一个修饰节点（就地链式）。
    template<typename N> auto then(N node) -> Modifier & {
        m_nodes.push_back(std::make_shared<N>(std::move(node)));
        return *this;
    }

    [[nodiscard]] auto nodes() const -> const std::vector<std::shared_ptr<ModifierNode>> & { return m_nodes; }

    [[nodiscard]] auto padding(float p) const -> Modifier {
        const float clamped =
            p < 0.0f ? (Diagnostics::degraded("layout", "Modifier::padding 负值已降级为 0"), 0.0f) : p;
        Modifier c = *this;
        c.m_nodes.push_back(std::make_shared<Padding>(clamped));
        return c;
    }
    /// @brief 非对称内边距（对应 Flutter EdgeInsets）。
    [[nodiscard]] auto padding(EdgeInsets insets) const -> Modifier {
        Modifier c = *this;
        c.m_nodes.push_back(std::make_shared<PaddingEdges>(insets));
        return c;
    }
    [[nodiscard]] auto background(Color c, float radius = 0.0f) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<Background>(c, radius));
        return cc;
    }
    /// @brief 线性渐变背景（双色，angle_deg 为渐变方向角度，0=从左到右，90=从上到下）。
    [[nodiscard]] auto gradient_linear(Color from, Color to, float angle_deg = 0.0f) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(
            std::make_shared<GradientBackground>(std::vector{ from, to }, std::vector{ 0.0f, 1.0f }, angle_deg));
        return cc;
    }
    /// @brief 线性渐变背景（多色标）。
    [[nodiscard]] auto gradient_linear(std::vector<Color> colors, std::vector<float> stops,
                                       float angle_deg = 0.0f) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<GradientBackground>(std::move(colors), std::move(stops), angle_deg));
        return cc;
    }
    /// @brief 径向渐变背景（双色，center→edge）。
    [[nodiscard]] auto gradient_radial(Color center_color, Color edge_color) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(
            std::make_shared<GradientBackground>(std::vector{ center_color, edge_color }, std::vector{ 0.0f, 1.0f }));
        return cc;
    }
    /// @brief 投影阴影（绘制于内容之下）。offset_x/y 偏移，blur 模糊半径，color 阴影色。
    [[nodiscard]] auto shadow(float offset_x = 0.0f, float offset_y = 2.0f, float blur = 4.0f,
                              Color color = Color(0, 0, 0, 64)) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<ShadowNode>(offset_x, offset_y, blur, color));
        return cc;
    }
    [[nodiscard]] auto clickable(std::function<void()> fn) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<Clickable>(std::move(fn)));
        return cc;
    }

    /// @brief 固定尺寸（宽高都强制）。-1 表示不约束该轴。
    [[nodiscard]] auto size(float w, float h) const -> Modifier {
        Modifier c = *this;
        const auto n = std::make_shared<SizeModifier>();
        n->set_width(w);
        n->set_height(h);
        c.m_nodes.push_back(n);
        return c;
    }
    /// @brief 固定宽度（-1 不约束）。
    [[nodiscard]] auto width(float w) const -> Modifier {
        Modifier c = *this;
        const auto n = std::make_shared<SizeModifier>();
        n->set_width(w);
        c.m_nodes.push_back(n);
        return c;
    }
    /// @brief 固定高度（-1 不约束）。
    [[nodiscard]] auto height(float h) const -> Modifier {
        Modifier c = *this;
        const auto n = std::make_shared<SizeModifier>();
        n->set_height(h);
        c.m_nodes.push_back(n);
        return c;
    }
    /// @brief 填充父级可用宽度。
    [[nodiscard]] auto fill_max_width() const -> Modifier {
        Modifier c = *this;
        const auto n = std::make_shared<SizeModifier>();
        n->set_fill_w(true);
        c.m_nodes.push_back(n);
        return c;
    }
    /// @brief 填充父级可用高度。
    [[nodiscard]] auto fill_max_height() const -> Modifier {
        Modifier c = *this;
        const auto n = std::make_shared<SizeModifier>();
        n->set_fill_h(true);
        c.m_nodes.push_back(n);
        return c;
    }
    /// @brief 填充父级可用宽高。
    [[nodiscard]] auto fill_max_size() const -> Modifier {
        Modifier c = *this;
        const auto n = std::make_shared<SizeModifier>();
        n->set_fill_w(true);
        n->set_fill_h(true);
        c.m_nodes.push_back(n);
        return c;
    }
    /// @brief 边框：`width` px 描边，`c` 颜色（绘制于内容之上）。
    [[nodiscard]] auto border(float width, Color c) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<Border>(width, c));
        return cc;
    }
    /// @brief 矩形裁剪：内容裁到本控件盒子内（overflow 隐藏）。
    [[nodiscard]] auto clip() const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<Clip>());
        return cc;
    }

    /// @brief 圆角裁剪：内容裁到圆角矩形内（硬遮罩，无抗锯齿）。
    [[nodiscard]] auto clip_rounded(float radius) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<ClipRounded>(radius));
        return cc;
    }

    /// @brief 对齐：在父级额外空间内把子项按 `a` 定位（占满可用空间）。
    [[nodiscard]] auto align(Alignment a) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<AlignNode>(a));
        return cc;
    }

    /// @brief 视觉偏移：把内容按 (dx,dy) 平移，不改变布局/命中。
    [[nodiscard]] auto offset(float dx, float dy) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<OffsetNode>(dx, dy));
        return cc;
    }

    /// @brief 不透明度：整体乘以 alpha（0=全透明，1=不透明）。可叠加多个。
    [[nodiscard]] auto opacity(float alpha) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<OpacityNode>(alpha));
        return cc;
    }

    /// @brief 旋转：绕内容盒中心旋转 `degrees` 度（顺时针为正，屏幕 y 轴向下）。
    [[nodiscard]] auto rotate(float degrees) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<TransformNode>(degrees));
        return cc;
    }

    /// @brief 缩放：绕内容盒中心按 (sx,sy) 非均匀缩放。
    [[nodiscard]] auto scale(float sx, float sy) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<TransformNode>(sx, sy));
        return cc;
    }

    /// @brief 等比缩放（sx==sy==s）。
    [[nodiscard]] auto scale(float s) const -> Modifier { return scale(s, s); }

    /// @brief 任意仿射变换：用户提供矩阵（关于原点；如需绕中心请自行 `from_*_about`）。
    [[nodiscard]] auto transform(const Matrix2D &m) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<TransformNode>(m));
        return cc;
    }

    /// @brief 可拖拽：按下并移动时回调上报位移增量与绝对坐标（不改布局）。
    [[nodiscard]] auto draggable(Draggable::DragCallback on_drag, std::function<void()> on_start = {},
                                 std::function<void()> on_end = {}) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<Draggable>(std::move(on_drag), std::move(on_start), std::move(on_end)));
        return cc;
    }

    /// @brief 长按：按下保持超过阈值（默认 500ms）触发回调（需 tickGestures 驱动）。
    [[nodiscard]] auto long_press(std::function<void()> on_long_press, float threshold_ms = 500.0f) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<LongPress>(std::move(on_long_press), threshold_ms));
        return cc;
    }

    /// @brief 原始多点触摸流：每次 `TouchEvent` 派发到该 widget 时回调完整事件（不消费命中）。
    /// 用于上层自定义并发交互（多指手势、自定义转场等）。
    [[nodiscard]] auto touch(std::function<void(const TouchEvent &)> on_touch) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<TouchListener>(std::move(on_touch)));
        return cc;
    }

    /// @brief 内容模糊：子树绘制完成后对整个内容盒做高斯近似模糊。
    [[nodiscard]] auto blur(float radius) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<BlurNode>(radius, false));
        return cc;
    }

    /// @brief 背景滤镜（毛玻璃）：绘制内容前先模糊内容盒背后的已绘像素，
    /// 配合半透明 background 形成毛玻璃效果。
    [[nodiscard]] auto backdrop_filter(float radius) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<BlurNode>(radius, true));
        return cc;
    }

    /// @brief 像素混合：内容绘制完成后，把内容盒像素与 `tint` 按 `mode` 混合。
    /// 对标 CSS `mix-blend-mode` 的常用子集；`strength`（0..1）控制强度。
    [[nodiscard]] auto blend_mode(BlendMode mode, Color tint, float strength = 1.0f) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<BlendNode>(mode, tint, strength));
        return cc;
    }

    /// @brief 着色器遮罩：内容绘制完成后按 `kind` 渐变淡出内容盒像素（0..1 强度）。
    /// 常用作图片 / 容器顶部或边缘的淡出聚焦效果。
    [[nodiscard]] auto shader_mask(ShaderMaskKind kind, float strength = 1.0f) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<ShaderMaskNode>(kind, strength));
        return cc;
    }

    /// @brief 离屏缓存：把子树渲染结果缓存到离屏位图，尺寸不变且未失效时直接复用，
    /// 避免重复绘制昂贵子树（类比 Flutter `RepaintBoundary`）。
    /// 失效请调用 `Widget::invalidate_paint_cache()`。
    [[nodiscard]] auto cache_layer() const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<CacheLayerNode>());
        return cc;
    }

    /// @brief 工具提示：鼠标悬停延迟（默认 500ms）后显示提示气泡。
    /// 对标 Qt `QToolTip`、WPF `ToolTip`、Flutter `Tooltip`、SwiftUI `.help()`。
    [[nodiscard]] auto tooltip(std::string text, float delay_ms = 500.0f) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<TooltipNode>(std::move(text), delay_ms));
        return cc;
    }

    /// @brief 上下文菜单：右键点击时弹出浮动菜单。
    /// 对标 Qt `QMenu::exec()`、SwiftUI `.contextMenu{}`、WPF `ContextMenu`。
    [[nodiscard]] auto context_menu(std::vector<MenuItem> items) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<ContextMenuNode>(std::move(items)));
        return cc;
    }

    /// @brief 声明本 widget 在父级 Row/Column 中的 flex 权重（瓜分主轴剩余空间）。
    /// 例：`Text{...}.modifier.set(Modifier{}.expand(2.0f))` 占 2 份。
    [[nodiscard]] auto expand(float weight = 1.0f) const -> Modifier {
        Modifier cc = *this;
        cc.m_nodes.push_back(std::make_shared<FlexWeight>(weight));
        return cc;
    }

    /// @brief 读取 Flex 权重：遍历修饰链取首个 `FlexWeight`；无则返回 0（不扩展）。
    [[nodiscard]] auto flex_weight() const -> float {
        float w = 0.0f;
        for (const auto &n : m_nodes) {
            if (n) {
                w = n->flex_weight(); // 非 FlexWeight 返回 0，覆盖式取最后一个
                if (w > 0.0f) {
                    return w;
                }
            }
        }
        return w;
    }

    /// @brief 触发修饰链中所有 `Clickable` 的点击回调（事件派发器调用）。
    auto invoke_click() const -> void;

    /**
     * @brief 计算修饰链对绘制内容的几何与透明度影响。
     *
     * - `translation`：Align/Offset/Padding 带来的内容平移量（绘制期生效，命中测试同步）。
     * - `content_size`：实际内容盒尺寸（Align 时小于布局盒）。
     * - `matrix`：Transform 切片（TransformNode）累积的绕内容盒中心的仿射矩阵
     *   （旋转/缩放/任意矩阵），用于离屏合成（见 `Widget::paint`）。
     * - `opacity`：OpacityNode 透明度累乘（0~1）。
     *
     * 多个 Transform 节点叠加时矩阵按链序组合；多个 OpacityNode 透明度相乘。
     */
    struct TransformInfo {
        Point translation{ .x = 0.0f, .y = 0.0f }; ///< 绘制内容相对布局盒的平移量
        Size content_size;                         ///< 实际内容盒尺寸（Align 时小于布局盒）
        Matrix2D matrix;                           ///< 绕内容盒中心的仿射变换（恒等=无变换）
        float opacity = 1.0f;                      ///< 整体不透明度（1=不透明）
    };

    [[nodiscard]] auto transform(const Size &self_size) const -> TransformInfo;

    /// @brief 触发所有 `Draggable` 的拖拽开始回调（指针按下时调用）。
    /// 每个 `Draggable` 按下时绑定 pointer id，仅匹配指针才会 fire（并发触控互不干扰）。
    auto invoke_drag_start(std::optional<int> pid) const -> void;
    /// @brief 触发所有 `Draggable` 的拖拽移动回调（仅匹配 pointer id 的拖拽响应）。
    auto invoke_drag(const Point &delta, const Point &pos, std::optional<int> pid) const -> void;
    /// @brief 触发所有 `Draggable` 的拖拽结束回调（指针抬起时调用，仅匹配指针解绑）。
    auto invoke_drag_end(std::optional<int> pid) const -> void;
    /// @brief 检查修饰链是否含任何 `Draggable`/`LongPress`（用于决定是否记录按下位置）。
    [[nodiscard]] auto has_gesture() const -> bool;
    /// @brief 检查修饰链是否含任何 `Clickable`（用于决定是否消费指针事件、触发点击）。
    [[nodiscard]] auto has_clickable() const -> bool;
    /// @brief 修饰链中是否有任意 `LongPress` 已触发（用于点击/长按互斥）。
    [[nodiscard]] auto long_press_fired() const -> bool;
    /// @brief 标记所有 `LongPress` 节点本次按下起点（用于阈值计时）；按下时绑定 pointer id，
    ///        仅匹配指针的 `LongPress` 进入计时（并发触控互不干扰）。
    auto press_long_press(std::chrono::steady_clock::time_point t, std::optional<int> pid) const -> void;
    /// @brief 取消所有 `LongPress` 计时（指针抬起/移出时，仅匹配指针解绑）。
    auto cancel_long_press(std::optional<int> pid) const -> void;
    /// @brief 驱动所有 `LongPress` 节点计时检查（由 `Widget::tickGestures` 调用）。
    auto tick_long_press(std::chrono::steady_clock::time_point now) const -> void;
    /// @brief 将原始 `TouchEvent` 交给修饰链上所有节点（仅 `TouchListener` 等消费）。
    auto on_pointer_event(const TouchEvent &e) const -> void;

    /// @brief 驱动所有 `TooltipNode` 计时检查（由 `Widget::tickGestures` 调用）。
    auto tick_tooltip(std::chrono::steady_clock::time_point now) const -> void;

    /// @brief 鼠标进入时通知所有 `TooltipNode` 开始计时。
    auto tooltip_hover_start(std::chrono::steady_clock::time_point t) const -> void;

    /// @brief 鼠标离开时通知所有 `TooltipNode` 重置。
    auto tooltip_hover_end() const -> void;

    /// @brief 获取当前应显示的 Tooltip 文本（无则返回空字符串）。
    [[nodiscard]] auto active_tooltip() const -> std::string;
    /// @brief 检查修饰链是否含任意 `TooltipNode`（用于决定是否需每帧 tick 驱动延迟计时）。
    [[nodiscard]] auto has_tooltip() const -> bool;

    /// @brief 检查修饰链是否含任何 `ContextMenuNode`。
    [[nodiscard]] auto has_context_menu() const -> bool;

    /// @brief 右键按下时打开上下文菜单（记录弹出位置）。
    auto open_context_menu(Point pos) const -> void;

    /// @brief 关闭所有上下文菜单。
    auto close_context_menu() const -> void;

    /// @brief 获取当前打开的上下文菜单项列表（无则返回空）。
    [[nodiscard]] auto active_context_menu_items() const -> std::vector<MenuItem>;

    /// @brief 获取当前打开的上下文菜单弹出位置。
    [[nodiscard]] auto active_context_menu_position() const -> Point;

  private:
    std::vector<std::shared_ptr<ModifierNode>> m_nodes;
};

} // namespace aurora
