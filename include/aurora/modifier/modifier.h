#pragma once

/// @file modifier.h
/// @brief 修饰系统聚合入口：包含基类 + 全部功能切片 + Modifier 工厂链。
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

/// @brief 修饰链：有序的修饰节点集合，挂在 widget 的 `modifier` 属性上。
///
/// 工厂方法返回副本（基于 shared_ptr），支持链式 `Modifier{}.padding(8).background(c)`。
class Modifier {
  public:
    Modifier() = default;

    /// @brief 追加一个修饰节点（就地链式）。
    /// @tparam N 具体 ModifierNode 派生类型；节点以 shared_ptr 副本入链，链拷贝间共享。
    /// @param node [in] 待追加的修饰节点，按值移入链尾。
    /// @return *this 引用，便于继续链式追加。
    template <typename N>
    auto then(N node) -> Modifier & {
        nodes_.push_back(std::make_shared<N>(std::move(node)));
        return *this;
    }

    /// @brief 只读访问修饰链的节点集合。
    /// @return 按压入顺序排列的节点引用；元素由 shared_ptr 持有，链拷贝间共享同一节点。
    [[nodiscard]] auto nodes() const -> const std::vector<std::shared_ptr<ModifierNode>> & { return nodes_; }

    /// @brief 等距内边距：四边各加 p px，子约束收缩测量后尺寸加回 2p。
    /// @param p [in] 内边距像素；负值在入链前降级为 0 并记录 layout 降级诊断。
    /// @return 追加 Padding 节点后的新链副本（本链不变）。
    [[nodiscard]] auto padding(float p) const -> Modifier {
        const float clamped = p < 0.0F ? (Diagnostics::degraded("layout", "Modifier::padding degrade to 0"), 0.0F) : p;
        Modifier c = *this;
        c.nodes_.push_back(std::make_shared<Padding>(clamped));
        return c;
    }
    /// @brief 非对称内边距（对应 Flutter EdgeInsets）。
    /// @param insets [in] 四边独立内边距；负值边在节点构造时降级为 0 并记录诊断。
    /// @return 追加 PaddingEdges 节点后的新链副本。
    [[nodiscard]] auto padding(EdgeInsets insets) const -> Modifier {
        Modifier c = *this;
        c.nodes_.push_back(std::make_shared<PaddingEdges>(insets));
        return c;
    }
    /// @brief 背景填充：不影响尺寸，绘制时在内容之下铺满矩形（含内边距区）。
    /// @param c [in] 背景色（含 alpha 通道）。
    /// @param radius [in] 圆角半径 px，默认 0（直角矩形）。
    /// @return 追加 Background 节点后的新链副本。
    [[nodiscard]] auto background(Color c, float radius = 0.0F) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<Background>(c, radius));
        return cc;
    }
    /// @brief 线性渐变背景（双色，angle_deg 为渐变方向角度，0=从左到右，90=从上到下）。
    /// @param from [in] 渐变起点色。
    /// @param to [in] 渐变终点色。
    /// @param angle_deg [in] 渐变方向角度（度），默认 0。
    /// @return 追加 GradientBackground 节点（stops 固定 0..1）后的新链副本。
    [[nodiscard]] auto gradient_linear(Color from, Color to, float angle_deg = 0.0F) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(
            std::make_shared<GradientBackground>(std::vector{from, to}, std::vector{0.0F, 1.0F}, angle_deg));
        return cc;
    }
    /// @brief 线性渐变背景（多色标）。
    /// @param colors [in] 渐变色标序列。
    /// @param stops [in] 与 colors 逐一对齐的停靠位序列（0..1）。
    /// @param angle_deg [in] 渐变方向角度（度），默认 0。
    /// @return 追加 GradientBackground 节点后的新链副本。
    [[nodiscard]] auto gradient_linear(std::vector<Color> colors, std::vector<float> stops,
                                       float angle_deg = 0.0F) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<GradientBackground>(std::move(colors), std::move(stops), angle_deg));
        return cc;
    }
    /// @brief 径向渐变背景（双色，center→edge）。
    /// @param center_color [in] 圆心色。
    /// @param edge_color [in] 边缘色。
    /// @return 追加 GradientBackground 节点（径向，stops 固定 0..1）后的新链副本。
    [[nodiscard]] auto gradient_radial(Color center_color, Color edge_color) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(
            std::make_shared<GradientBackground>(std::vector{center_color, edge_color}, std::vector{0.0F, 1.0F}));
        return cc;
    }
    /// @brief 投影阴影（绘制于内容之下）。offset_x/y 偏移，blur 模糊半径，color 阴影色。
    /// @param offset_x [in] 阴影水平偏移 px，默认 0。
    /// @param offset_y [in] 阴影垂直偏移 px，默认 2。
    /// @param blur [in] 模糊半径 px，默认 4。
    /// @param color [in] 阴影颜色，默认半透明黑（alpha 64）。
    /// @return 追加 ShadowNode 节点后的新链副本。
    [[nodiscard]] auto shadow(float offset_x = 0.0F, float offset_y = 2.0F, float blur = 4.0F,
                              Color color = Color(0, 0, 0, 64)) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<ShadowNode>(offset_x, offset_y, blur, color));
        return cc;
    }
    /// @brief 可点击：命中本控件盒时拦截输入事件并触发回调（不改布局）。
    /// @param fn [in] 点击回调，移入 Clickable 节点；链上任一 Clickable 命中即执行。
    /// @return 追加 Clickable 节点后的新链副本。
    [[nodiscard]] auto clickable(std::function<void()> fn) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<Clickable>(std::move(fn)));
        return cc;
    }

    /// @brief 固定尺寸（宽高都强制）。-1 表示不约束该轴。
    /// @param w [in] 目标宽度 px；-1 表示宽轴沿用子节点尺寸。
    /// @param h [in] 目标高度 px；-1 表示高轴沿用子节点尺寸。
    /// @return 追加 SizeModifier 节点后的新链副本。
    [[nodiscard]] auto size(float w, float h) const -> Modifier {
        Modifier c = *this;
        const auto n = std::make_shared<SizeModifier>();
        n->set_width(w);
        n->set_height(h);
        c.nodes_.push_back(n);
        return c;
    }
    /// @brief 固定宽度（-1 不约束）。
    /// @param w [in] 目标宽度 px；-1 表示宽轴沿用子节点尺寸。
    /// @return 追加 SizeModifier（仅约束宽轴）后的新链副本。
    [[nodiscard]] auto width(float w) const -> Modifier {
        Modifier c = *this;
        const auto n = std::make_shared<SizeModifier>();
        n->set_width(w);
        c.nodes_.push_back(n);
        return c;
    }
    /// @brief 固定高度（-1 不约束）。
    /// @param h [in] 目标高度 px；-1 表示高轴沿用子节点尺寸。
    /// @return 追加 SizeModifier（仅约束高轴）后的新链副本。
    [[nodiscard]] auto height(float h) const -> Modifier {
        Modifier c = *this;
        const auto n = std::make_shared<SizeModifier>();
        n->set_height(h);
        c.nodes_.push_back(n);
        return c;
    }
    /// @brief 填充父级可用宽度。
    /// @return 追加 SizeModifier（宽轴 min=max=约束上限）后的新链副本。
    [[nodiscard]] auto fill_max_width() const -> Modifier {
        Modifier c = *this;
        const auto n = std::make_shared<SizeModifier>();
        n->set_fill_w(true);
        c.nodes_.push_back(n);
        return c;
    }
    /// @brief 填充父级可用高度。
    /// @return 追加 SizeModifier（高轴 min=max=约束上限）后的新链副本。
    [[nodiscard]] auto fill_max_height() const -> Modifier {
        Modifier c = *this;
        const auto n = std::make_shared<SizeModifier>();
        n->set_fill_h(true);
        c.nodes_.push_back(n);
        return c;
    }
    /// @brief 填充父级可用宽高。
    /// @return 追加 SizeModifier（两轴 min=max=约束上限）后的新链副本。
    [[nodiscard]] auto fill_max_size() const -> Modifier {
        Modifier c = *this;
        const auto n = std::make_shared<SizeModifier>();
        n->set_fill_w(true);
        n->set_fill_h(true);
        c.nodes_.push_back(n);
        return c;
    }
    /// @brief 边框：`width` px 描边，`c` 颜色（绘制于内容之上）。
    /// @param width [in] 描边线宽 px。
    /// @param c [in] 描边颜色。
    /// @return 追加 Border 节点后的新链副本。
    [[nodiscard]] auto border(float width, Color c) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<Border>(width, c));
        return cc;
    }
    /// @brief 矩形裁剪：内容裁到本控件盒子内（overflow 隐藏）。
    /// @return 追加 Clip 节点后的新链副本。
    [[nodiscard]] auto clip() const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<Clip>());
        return cc;
    }

    /// @brief 圆角裁剪：内容裁到圆角矩形内（硬遮罩，无抗锯齿）。
    /// @param radius [in] 圆角半径 px。
    /// @return 追加 ClipRounded 节点后的新链副本。
    [[nodiscard]] auto clip_rounded(float radius) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<ClipRounded>(radius));
        return cc;
    }

    /// @brief 对齐：在父级额外空间内把子项按 `a` 定位（占满可用空间）。
    /// @param a [in] 目标对齐方位。
    /// @return 追加 AlignNode 节点后的新链副本。
    [[nodiscard]] auto align(Alignment a) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<AlignNode>(a));
        return cc;
    }

    /// @brief 视觉偏移：把内容按 (dx,dy) 平移，不改变布局尺寸；命中测试的平移量与绘制保持一致（命中区随 offset 移动）。
    /// @param dx [in] 水平平移 px。
    /// @param dy [in] 垂直平移 px。
    /// @return 追加 OffsetNode 节点后的新链副本。
    [[nodiscard]] auto offset(float dx, float dy) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<OffsetNode>(dx, dy));
        return cc;
    }

    /// @brief 不透明度：整体乘以 alpha（0=全透明，1=不透明）。可叠加多个。
    /// @param alpha [in] 0..1 系数（越界由 OpacityNode 构造钳制）；链上多节点累乘。
    /// @return 追加 OpacityNode 节点后的新链副本。
    [[nodiscard]] auto opacity(float alpha) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<OpacityNode>(alpha));
        return cc;
    }

    /// @brief 旋转：绕内容盒中心旋转 `degrees` 度（顺时针为正，屏幕 y 轴向下）。
    /// @param degrees [in] 旋转角度（度）。
    /// @return 追加 TransformNode（Rotate）节点后的新链副本。
    [[nodiscard]] auto rotate(float degrees) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<TransformNode>(degrees));
        return cc;
    }

    /// @brief 缩放：绕内容盒中心按 (sx,sy) 非均匀缩放。
    /// @param sx [in] 横向倍率。
    /// @param sy [in] 纵向倍率。
    /// @return 追加 TransformNode（ScaleXY）节点后的新链副本。
    [[nodiscard]] auto scale(float sx, float sy) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<TransformNode>(sx, sy));
        return cc;
    }

    /// @brief 等比缩放（sx==sy==s）。
    /// @param s [in] 等比倍率，等价于 `scale(s, s)`。
    /// @return 追加 TransformNode（ScaleXY）节点后的新链副本。
    [[nodiscard]] auto scale(float s) const -> Modifier { return scale(s, s); }

    /// @brief 任意仿射变换：用户提供矩阵（关于原点；如需绕中心请自行 `from_*_about`）。
    /// @param m [in] 用户仿射矩阵，按原样关于原点应用。
    /// @return 追加 TransformNode（Raw）节点后的新链副本。
    [[nodiscard]] auto transform(const Matrix2D &m) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<TransformNode>(m));
        return cc;
    }

    /// @brief 可拖拽：按下并移动时回调上报位移增量与绝对坐标（不改布局）。
    /// @param on_drag [in] 移动回调，参数为（相对上次位置的位移增量, 指针绝对坐标）。
    /// @param on_start [in] 拖拽开始回调，默认空。
    /// @param on_end [in] 拖拽结束回调，默认空。
    /// @return 追加 Draggable 节点后的新链副本。
    [[nodiscard]] auto draggable(Draggable::DragCallback on_drag, std::function<void()> on_start = {},
                                 std::function<void()> on_end = {}) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<Draggable>(std::move(on_drag), std::move(on_start), std::move(on_end)));
        return cc;
    }

    /// @brief 长按：按下保持超过阈值（默认 500ms）触发回调（需 tickGestures 驱动）。
    /// @param on_long_press [in] 到达阈值触发一次的回调。
    /// @param threshold_ms [in] 按住阈值毫秒，默认 500。
    /// @return 追加 LongPress 节点后的新链副本。
    [[nodiscard]] auto long_press(std::function<void()> on_long_press, float threshold_ms = 500.0F) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<LongPress>(std::move(on_long_press), threshold_ms));
        return cc;
    }

    /// @brief 原始多点触摸流：每次 `TouchEvent` 派发到该 widget 时回调完整事件。
    /// 当前实现中 Input 类节点一经命中即返回自身，会拦截向子节点下探（含 TouchListener / Tooltip / Cursor）。
    /// 用于上层自定义并发交互（多指手势、自定义转场等）。
    /// @param on_touch [in] 完整事件回调，每次 TouchEvent 派发到本 widget 时收到该事件。
    /// @return 追加 TouchListener 节点后的新链副本。
    [[nodiscard]] auto touch(std::function<void(const TouchEvent &)> on_touch) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<TouchListener>(std::move(on_touch)));
        return cc;
    }

    /// @brief 内容模糊：子树绘制完成后对整个内容盒做高斯近似模糊。
    /// @param radius [in] 高斯模糊半径 px。
    /// @return 追加 BlurNode（backdrop=false）后的新链副本。
    [[nodiscard]] auto blur(float radius) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<BlurNode>(radius, false));
        return cc;
    }

    /// @brief 背景滤镜（毛玻璃）：绘制内容前先模糊内容盒背后的已绘像素，
    /// 配合半透明 background 形成毛玻璃效果。
    /// @param radius [in] 背后已绘像素的模糊半径 px。
    /// @return 追加 BlurNode（backdrop=true）节点后的新链副本。
    [[nodiscard]] auto backdrop_filter(float radius) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<BlurNode>(radius, true));
        return cc;
    }

    /// @brief 像素混合：内容绘制完成后，把内容盒像素与 `tint` 按 `mode` 混合。
    /// 对标 CSS `mix-blend-mode` 的常用子集；`strength`（0..1）控制强度。
    /// @param mode [in] 混合模式。
    /// @param tint [in] 参与混合的颜色。
    /// @param strength [in] 混合强度 0..1，默认 1（完全混合）。
    /// @return 追加 BlendNode 节点后的新链副本。
    [[nodiscard]] auto blend_mode(BlendMode mode, Color tint, float strength = 1.0F) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<BlendNode>(mode, tint, strength));
        return cc;
    }

    /// @brief 着色器遮罩：内容绘制完成后按 `kind` 渐变淡出内容盒像素（0..1 强度）。
    /// 常用作图片 / 容器顶部或边缘的淡出聚焦效果。
    /// @param kind [in] 遮罩渐变形态。
    /// @param strength [in] 淡出强度 0..1，默认 1。
    /// @return 追加 ShaderMaskNode 节点后的新链副本。
    [[nodiscard]] auto shader_mask(ShaderMaskKind kind, float strength = 1.0F) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<ShaderMaskNode>(kind, strength));
        return cc;
    }

    /// @brief 离屏缓存：把子树渲染结果缓存到离屏位图，尺寸不变且未失效时直接复用，
    /// 避免重复绘制昂贵子树（类比 Flutter `RepaintBoundary`）。
    /// 失效请调用 `Widget::invalidate_paint_cache()`。
    /// @return 追加 CacheLayerNode 节点后的新链副本。
    [[nodiscard]] auto cache_layer() const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<CacheLayerNode>());
        return cc;
    }

    /// @brief 工具提示：鼠标悬停延迟（默认 500ms）后显示提示气泡。
    /// 对标 Qt `QToolTip`、WPF `ToolTip`、Flutter `Tooltip`、SwiftUI `.help()`。
    /// @param text [in] 提示气泡文本，移入节点。
    /// @param delay_ms [in] 悬停显示延迟毫秒，默认 500（负值由节点钳制为 0）。
    /// @return 追加 TooltipNode 节点后的新链副本。
    [[nodiscard]] auto tooltip(std::string text, float delay_ms = 500.0F) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<TooltipNode>(std::move(text), delay_ms));
        return cc;
    }

    /// @brief 上下文菜单：右键点击时弹出浮动菜单。
    /// 对标 Qt `QMenu::exec()`、SwiftUI `.contextMenu{}`、WPF `ContextMenu`。
    /// @param items [in] 菜单项列表，移入 ContextMenuNode。
    /// @return 追加 ContextMenuNode 节点后的新链副本。
    [[nodiscard]] auto context_menu(std::vector<MenuItem> items) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<ContextMenuNode>(std::move(items)));
        return cc;
    }

    /// @brief 声明本 widget 在父级 Row/Column 中的 flex 权重（瓜分主轴剩余空间）。
    /// 例：`Text{...}.modifier.set(Modifier{}.expand(2.0F))` 占 2 份。
    /// @param weight [in] flex 权重（>0 按份数瓜分主轴剩余空间），默认 1。
    /// @return 追加 FlexWeight 节点后的新链副本。
    [[nodiscard]] auto expand(float weight = 1.0F) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<FlexWeight>(weight));
        return cc;
    }

    /// @brief 读取 Flex 权重：遍历修饰链取首个 `FlexWeight`；无则返回 0（不扩展）。
    /// @return 首个正权重值；链上无 FlexWeight（或仅非 FlexWeight 节点）时为 0。
    [[nodiscard]] auto flex_weight() const -> float {
        float w = 0.0F;
        for (const auto &n : nodes_) {
            if (n) {
                w = n->flex_weight();  // 非 FlexWeight 返回 0，取首个正权重即返回
                if (w > 0.0F) {
                    return w;
                }
            }
        }
        return w;
    }

    /// @brief 声明悬停光标形状：鼠标悬停本控件时把光标切到 `shape`（不影响布局/命中）。
    /// 例：`Modifier{}.clickable(fn).cursor(CursorShape::PointingHand)`。
    /// @param shape [in] 悬停时生效的光标形状。
    /// @return 追加 CursorNode 节点后的新链副本。
    [[nodiscard]] auto cursor(CursorShape shape) const -> Modifier {
        Modifier cc = *this;
        cc.nodes_.push_back(std::make_shared<CursorNode>(shape));
        return cc;
    }

    /// @brief 读取悬停光标声明：遍历修饰链取**最后一个** `CursorNode`（后写覆盖先写）；
    /// 无则返回空（交由 `Widget::cursor_shape()` 虚钩子 / Clickable 缺省策略兜底）。
    /// @return 链上最后一个 `CursorNode` 的形状；链上无声明时为空 optional。
    [[nodiscard]] auto cursor_shape() const -> std::optional<CursorShape> {
        std::optional<CursorShape> found;
        for (const auto &n : nodes_) {
            if (n) {
                if (const auto s = n->cursor_shape()) {
                    found = s;
                }
            }
        }
        return found;
    }

    /// @brief 触发修饰链中所有 `Clickable` 的点击回调（事件派发器调用）。
    auto invoke_click() const -> void;

    /// @brief 计算修饰链对绘制内容的几何与透明度影响。
    ///
    /// - `translation`：Align/Offset/Padding 带来的内容平移量（绘制期生效，命中盒随之同步平移）。
    /// - `content_size`：实际内容盒尺寸（Align 时小于布局盒）。
    /// - `hit_size`：命中盒尺寸，**只**被 Align 收缩（Padding 不收缩，与 `paint_boxes` 的
    /// 内边距豁免同源，见 `specification/07-environment-modifier.md` §7.4）。
    /// - `matrix`：Transform 切片（TransformNode）累积的绕内容盒中心的仿射矩阵
    /// （旋转/缩放/任意矩阵），用于离屏合成（见 `Widget::paint`）。
    /// - `opacity`：OpacityNode 透明度累乘（0~1）。
    ///
    /// 多个 Transform 节点叠加时矩阵按链序组合；多个 OpacityNode 透明度相乘。
    struct TransformInfo {
        Point translation{.x = 0.0F, .y = 0.0F};  ///< 绘制内容相对布局盒的平移量
        Size content_size;  ///< 实际内容盒尺寸（Align 时小于布局盒）
        Size hit_size;  ///< 命中盒尺寸（仅 Align 收缩；链上无 Align 时等于布局盒尺寸）
        Matrix2D matrix;  ///< 绕内容盒中心的仿射变换（恒等=无变换）
        float opacity = 1.0F;  ///< 整体不透明度（1=不透明）
    };

    /// @brief 按链序合成 Transform 切片对绘制的影响：Align/Offset/Padding 的平移与收缩、TransformNode 矩阵组合、Opacity
    /// 累乘。
    /// @param self_size [in] 本控件的布局盒尺寸，作为内容盒与命中盒的起点。
    /// @return 合成后的 TransformInfo（平移量、内容盒、命中盒、累积矩阵与累乘不透明度）。
    [[nodiscard]] auto transform(const Size &self_size) const -> TransformInfo;

    /// @brief 逐个节点的绘制盒（与 `nodes()` 同序），供 Paint 类修饰按链上位置取盒。
    ///
    /// 链序约定：`Widget::layout` 逆序包裹节点，故 **先压入者靠外**
    /// （`Modifier{}.a().b()` 里 a 是 b 的外层）。盒规则：
    /// - 外侧的 `AlignNode` 展开出的那部分空间不属于本控件自身，其**内侧**的 Paint 节点
    /// 只取对齐后的子盒：`align(Center).size(120,40).background(c)` → 背景画在居中的
    /// 120×40，而不是展开后的整行。
    /// - 外侧的 `OffsetNode` 平移其内侧的 Paint 节点，与内容/命中保持一致。
    /// - `Padding` / `PaddingEdges` **不**分段：背景、边框、裁剪一律连内边距一起覆盖
    /// （规格 §7.4 把「把 Paint 修饰限制在 content_box 导致 padding 区域露白」列为历史错误形态）。
    ///
    /// @param widget_box [in] 本控件的布局盒（最外层节点所占的盒）。
    ///
    /// @return 与 `nodes()` 同序的盒子数组；链上无盒改变节点（Align/Offset）时返回**空数组**
    /// （调用方沿用 `widget_box`，与历史行为逐位一致，且免掉每帧走链开销）。
    [[nodiscard]] auto paint_boxes(const Rect &widget_box) const -> std::vector<Rect>;

    /// @brief 触发所有 `Draggable` 的拖拽开始回调（指针按下时调用）。
    /// 每个 `Draggable` 按下时绑定 pointer id，仅匹配指针才会 fire（并发触控互不干扰）。
    /// @param pid [in] 按下的指针 id；nullopt（鼠标）视为任意指针，绑定后仅同指针拖拽响应。
    auto invoke_drag_start(std::optional<int> pid) const -> void;
    /// @brief 触发所有 `Draggable` 的拖拽移动回调（仅匹配 pointer id 的拖拽响应）。
    /// @param delta [in] 相对上一帧的位移增量。
    /// @param pos [in] 指针当前绝对坐标。
    /// @param pid [in] 待匹配的指针 id。
    auto invoke_drag(const Point &delta, const Point &pos, std::optional<int> pid) const -> void;
    /// @brief 触发所有 `Draggable` 的拖拽结束回调（指针抬起时调用，仅匹配指针解绑）。
    /// @param pid [in] 抬起的指针 id；仅匹配指针触发结束回调并解绑。
    auto invoke_drag_end(std::optional<int> pid) const -> void;
    /// @brief 检查修饰链是否含任何 `Draggable`/`LongPress`（用于决定是否记录按下位置）。
    /// @return 链上存在 Draggable 或 LongPress 节点时为 true。
    [[nodiscard]] auto has_gesture() const -> bool;
    /// @brief 检查修饰链是否含任何 `Clickable`（用于决定是否消费指针事件、触发点击）。
    /// @return 链上存在 Clickable 节点时为 true。
    [[nodiscard]] auto has_clickable() const -> bool;
    /// @brief 修饰链中是否有任意 `LongPress` 已触发（用于点击/长按互斥）。
    /// @return 任一 LongPress 本次按住已触发回调时为 true（此时应抑制点击）。
    [[nodiscard]] auto long_press_fired() const -> bool;
    /// @brief 标记所有 `LongPress` 节点本次按下起点（用于阈值计时）；按下时绑定 pointer id，
    ///        仅匹配指针的 `LongPress` 进入计时（并发触控互不干扰）。
    /// @param t [in] 按下时刻（steady_clock），作为阈值计时起点。
    /// @param pid [in] 按下的指针 id；绑定后仅同指针进入计时。
    auto press_long_press(std::chrono::steady_clock::time_point t, std::optional<int> pid) const -> void;
    /// @brief 取消所有 `LongPress` 计时（指针抬起/移出时，仅匹配指针解绑）。
    /// @param pid [in] 抬起/移出的指针 id；仅匹配指针取消计时并解绑。
    auto cancel_long_press(std::optional<int> pid) const -> void;
    /// @brief 驱动所有 `LongPress` 节点计时检查（由 `Widget::tickGestures` 调用）。
    /// @param now [in] 当前时刻（steady_clock）；按下起点到 now 达到阈值即触发一次回调。
    auto tick_long_press(std::chrono::steady_clock::time_point now) const -> void;
    /// @brief 将原始 `TouchEvent` 交给修饰链上所有节点（仅 `TouchListener` 等消费）。
    /// @param e [in] 派发到本 widget 的原始触摸事件，逐节点传入 on_touch。
    auto on_pointer_event(const TouchEvent &e) const -> void;

    /// @brief 驱动所有 `TooltipNode` 计时检查（由 `Widget::tickGestures` 调用）。
    /// @param now [in] 当前时刻（steady_clock）；悬停起点到 now 超过 delay_ms 即置为可见。
    auto tick_tooltip(std::chrono::steady_clock::time_point now) const -> void;

    /// @brief 鼠标进入时通知所有 `TooltipNode` 开始计时。
    /// @param t [in] 鼠标进入时刻（steady_clock），作为显示延迟计时起点。
    auto tooltip_hover_start(std::chrono::steady_clock::time_point t) const -> void;

    /// @brief 鼠标离开时通知所有 `TooltipNode` 重置。
    auto tooltip_hover_end() const -> void;

    /// @brief 获取当前应显示的 Tooltip 文本（无则返回空字符串）。
    /// @return 链上首个已可见 TooltipNode 的文本；无可见提示时为空字符串。
    [[nodiscard]] auto active_tooltip() const -> std::string;
    /// @brief 检查修饰链是否含任意 `TooltipNode`（用于决定是否需每帧 tick 驱动延迟计时）。
    /// @return 链上存在 TooltipNode 时为 true。
    [[nodiscard]] auto has_tooltip() const -> bool;

    /// @brief 检查修饰链是否含任何 `ContextMenuNode`。
    /// @return 链上存在 ContextMenuNode 时为 true。
    [[nodiscard]] auto has_context_menu() const -> bool;

    /// @brief 右键按下时打开上下文菜单（记录弹出位置）。
    /// @param pos [in] 弹出位置（全局坐标），记入链上所有 ContextMenuNode。
    auto open_context_menu(Point pos) const -> void;

    /// @brief 关闭所有上下文菜单。
    auto close_context_menu() const -> void;

    /// @brief 获取当前打开的上下文菜单项列表（无则返回空）。
    /// @return 首个处于打开状态的 ContextMenuNode 的菜单项；无打开菜单时为空数组。
    [[nodiscard]] auto active_context_menu_items() const -> std::vector<MenuItem>;

    /// @brief 获取当前打开的上下文菜单弹出位置。
    /// @return 打开菜单记录的弹出坐标（全局）；无打开菜单时为 (0, 0)。
    [[nodiscard]] auto active_context_menu_position() const -> Point;

  private:
    std::vector<std::shared_ptr<ModifierNode>> nodes_;
};

}  // namespace aurora
