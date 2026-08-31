#include "aurora/widget/widget.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <ranges>

#include "aurora/core/debug.h"
#include "aurora/event/focus.h"
#include "aurora/modifier/modifier.h"
#include "aurora/perf/counters.h"
#include "aurora/render/detail/paint_timing.h" // [鎬ц兘鎺掓煡] g_pt.paint_nodes / g_pt.dl_replays 闀滃儚
#include "aurora/render/painter.h"

namespace aurora {

// Node 析构：子节点销毁时清空其缓存的布局父指针，避免向上失效传播解引用悬垂指针
// （见 node.h 中 Node 类注释）。需完整 Widget，故定义于此而非头文件。
Node::~Node() {
    if (m_widget) {
        m_widget->set_layout_parent(nullptr);
    }
}

/// @brief 把 widget 局部坐标 `local` 经修饰链的平移与仿射矩阵映射回「内容局部」坐标。
/// 恒等矩阵时退化为 `local - translation`；否则用绕内容中心的逆矩阵映射，使
/// 旋转/缩放内容的命中测试与绘制（离屏合成）一致。
namespace {
/// @brief 当前正在 `layout()` 中的控件——布局调用天然嵌套（父在栈上、子在其内），
/// 故它恰是下一层子节点的**布局父**。由 `Widget::layout` 以 RAII 维护。
///
/// 意义：布局父链由**构造**保证完整，不再依赖各容器手工 `set_layout_parent`（历史上
/// `NavigatorHost` 就漏过一次，导致其子树的脏标记无法上溯）。父链完整是脏标记上溯
/// （`Widget::request_frame`）与布局/DL 缓存向上失效的共同前提。
/// @note Thread: main-thread only（`thread_local` 仅为多渲染线程隔离，不做跨线程同步）
thread_local Widget *t_layout_parent = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
} // namespace

static auto adjust_for_transform(const Modifier::TransformInfo &tf, Point local) -> Point {
    // tf.matrix 已绕「内容盒中心」构造（TransformNode::matrix），只需再平移到 widget
    // 局部原点（content 位于 tf.translation）。注意：不可再次叠加内容中心，否则双重居中。
    const Point t = tf.translation;
    const Matrix2D local_mat =
        Matrix2D::from_translate(t.x, t.y).compose(tf.matrix).compose(Matrix2D::from_translate(-t.x, -t.y));
    return local_mat.inverse().apply_to_point(local) - tf.translation;
}

auto Widget::layout(const Constraints &c, const BuildContext &ctx) -> Size {
    // 布局父链自动登记（见 t_layout_parent）：必须在任何提前返回之前完成，使 show=false
    // 与命中布局缓存的节点也持有正确父指针（其后代仍可能标脏并需要上溯）。链顶（根控件由
    // 渲染器直接 layout）时 t_layout_parent 为空，不覆写既有父指针。
    if (t_layout_parent != nullptr) {
        m_layout_parent = t_layout_parent;
    }
    struct LayoutParentScope {
        Widget *saved;
        explicit LayoutParentScope(Widget *self) : saved(t_layout_parent) { t_layout_parent = self; }
        LayoutParentScope(const LayoutParentScope &) = delete;
        auto operator=(const LayoutParentScope &) -> LayoutParentScope & = delete;
        LayoutParentScope(LayoutParentScope &&) = delete;
        auto operator=(LayoutParentScope &&) -> LayoutParentScope & = delete;
        ~LayoutParentScope() { t_layout_parent = saved; }
    } parent_scope{ this };

    if (!show.get()) {
        m_size = Size{ .width = 0.0f, .height = 0.0f };
        return m_size;
    }

#ifdef AURORA_LAYOUT_CACHE
    // 缓存命中：约束未变、缓存有效、且本控件允许布局缓存（上层已保证“缓存有效 ⇒ 无脏后代”）。
    // 直接复用缓存尺寸，完全跳过修饰链构建 + on_layout + 子树递归。
    // `can_cache_layout()` 为 false 的控件（on_layout 含时间/状态依赖副作用，如骨架→内容切换）
    // 永不命中缓存，保证其 on_layout 在每个布局 pass 被真正执行，杜绝“约束不变 ⇒ 内容冻结”类白屏。
    if (m_layout_cache_valid && m_cached_constraints == c && can_cache_layout()) {
        m_size = m_cached_size;
        return m_size;
    }
#endif
    // 缓存未命中：本节点将真正重新测量（含修饰链构建 + on_layout + 子树递归）。
    // 计数只统计「真实布局工作量」，缓存命中不计——G-6 复杂度门槛即基于此语义。
    AURORA_PROFILE_COUNT(layout_nodes, 1);

    // 显式尺寸意图（规格 §4/§20）：固定宽度/高度构成"显式盒"，把对应轴约束
    // 夹成 [v, v]，使子节点在固定盒内布局；其余意图（auto/fill）保持内容/弹性。
    Constraints cc = c;
    if (m_width.kind == LengthKind::Fixed) {
        cc.min.width = m_width.value;
        cc.max.width = m_width.value;
    } else if (m_width.kind == LengthKind::Fraction) {
        // 百分比：占父约束 max 的比例
        const float w = c.max.width * m_width.value;
        cc.min.width = w;
        cc.max.width = w;
    } else if (m_width.kind == LengthKind::Expand) {
        // 填充：撑满父约束
        cc.min.width = c.max.width;
        cc.max.width = c.max.width;
    }
    if (m_height.kind == LengthKind::Fixed) {
        cc.min.height = m_height.value;
        cc.max.height = m_height.value;
    } else if (m_height.kind == LengthKind::Fraction) {
        const float h = c.max.height * m_height.value;
        cc.min.height = h;
        cc.max.height = h;
    } else if (m_height.kind == LengthKind::Expand) {
        cc.min.height = c.max.height;
        cc.max.height = c.max.height;
    }

    std::function measure = [this, &ctx](const Constraints &inner) -> Size { return this->on_layout(inner, ctx); };

    const Modifier &mod = modifier.get();
    for (const auto &it : std::views::reverse(mod.nodes())) {
        const ModifierNode *node = it.get();
        auto inner = std::move(measure);
        measure = [node, inner](const Constraints &child_c) -> Size { return node->layout(child_c, inner); };
    }

    m_size = measure(cc);
    // 显式盒最终尺寸严格等于设定值（内容溢出不撑大盒子，符合 CSS box 语义）。
    if (m_width.kind == LengthKind::Fixed) {
        m_size.width = m_width.value;
    } else if (m_width.kind == LengthKind::Fraction) {
        m_size.width = c.max.width * m_width.value;
    } else if (m_width.kind == LengthKind::Expand) {
        m_size.width = c.max.width;
    }
    if (m_height.kind == LengthKind::Fixed) {
        m_size.height = m_height.value;
    } else if (m_height.kind == LengthKind::Fraction) {
        m_size.height = c.max.height * m_height.value;
    } else if (m_height.kind == LengthKind::Expand) {
        m_size.height = c.max.height;
    }
    // 几何权威完全收敛到 Node::m_bounds：布局只确定自身尺寸，位置由父节点经
    // Node::set_bounds 写入（含真实 origin）。Widget 不再持有任何几何缓存。
#ifdef AURORA_LAYOUT_CACHE
    m_cached_constraints = c;
    m_cached_size = m_size;
    // 布局缓存决策：仅基于本控件自身的 can_cache_layout()。
    // 含时间/状态依赖副作用的控件（骨架→内容切换、入场动画等）覆写 can_cache_layout()=false，
    // 使其自身永不命中缓存——on_layout 在每个布局 pass 被真正执行，杜绝内容冻结/白屏。
    //
    // 注意：不向上传播「子树含不可缓存后代」到祖先。传播会导致 grid_rows 等测试回归
    // （祖先 on_layout 含副作用，额外执行会推进动画状态）。
    //
    // 对于「父控件缓存会跳过调用子控件 layout()」的场景（如 Scroll），由该父控件自行在
    // 其 can_cache_layout() 覆写中检查直接子控件（见 Scroll::can_cache_layout）。
    m_layout_cache_valid = can_cache_layout();
#endif
    return m_size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Widget::paint_content(Painter &p, const Rect &visual_box, const Rect &content_box, const BuildContext &ctx)
    -> void {
    const Modifier &mod = modifier.get();
    // 内容前修饰（Paint 切片）分三遍，关键顺序：
    //   第 1 遍：阴影 / 背景毛玻璃(backdrop)——必须在圆角裁剪【之前】绘制，
    //            否则阴影的外扩羽化、毛玻璃采样会被圆角裁剪切掉。
    //   第 2 遍：压裁剪（Clip / ClipRounded），与既有裁剪栈取交集。
    //   第 3 遍：背景填充 / 渐变背景——此时圆角裁剪已生效，背景随圆角裁剪呈圆角。
    // 注意：Paint 类修饰（背景、边框、阴影、裁剪）作用于控件完整视觉盒子 visual_box；
    // 子节点绘制与内容后效则限定在 content_box（已扣除 Padding/Align 等布局内边距）。
    // Pass 1：阴影 + 背景毛玻璃（无裁剪，外扩可见）。
    for (const auto &mn : mod.nodes()) {
        switch (mn->paint_kind()) {
        case ModifierNode::PaintKind::Blur: {
            const auto *bl = dynamic_cast<BlurNode *>(mn.get());
            if (bl->is_backdrop()) {
                // 毛玻璃：绘内容前先模糊视觉盒背后的已绘像素。
                p.blur_region(visual_box, bl->radius());
            }
            break;
        }
        case ModifierNode::PaintKind::Shadow: {
            const auto *sh = dynamic_cast<ShadowNode *>(mn.get());
            p.draw_shadow(visual_box, sh->offset_x(), sh->offset_y(), sh->blur(), sh->color());
            break;
        }
        default: break;
        }
    }
    // Pass 2：压裁剪（与既有裁剪栈取交集）。
    for (const auto &mn : mod.nodes()) {
        switch (mn->paint_kind()) {
        case ModifierNode::PaintKind::ClipRounded: {
            const auto *cr = dynamic_cast<ClipRounded *>(mn.get());
            p.push_clip_rounded(visual_box, cr->radius());
            break;
        }
        case ModifierNode::PaintKind::Clip: p.push_clip(visual_box); break;
        default: break;
        }
    }
    // Pass 3：背景填充 / 渐变背景（圆角裁剪已生效）。
    for (const auto &mn : mod.nodes()) {
        switch (mn->paint_kind()) {
        case ModifierNode::PaintKind::Background: {
            const auto *bg = dynamic_cast<Background *>(mn.get());
            if (bg->corner_radius() > 0.0f) {
                p.push_clip_rounded(visual_box, bg->corner_radius());
                p.fill_rect(visual_box, bg->color());
                p.pop_clip();
            } else {
                p.fill_rect(visual_box, bg->color());
            }
            break;
        }
        case ModifierNode::PaintKind::GradientBackground: {
            const auto *grad = dynamic_cast<GradientBackground *>(mn.get());
            const auto &colors = grad->colors();
            const auto &stops = grad->stops();
            if (grad->type() == GradientBackground::Type::Linear) {
                // 按角度计算 start/end（角度 0=左→右，90=上→下）
                const float rad = grad->angle() * std::numbers::pi_v<float> / 180.0f;
                const float cx = visual_box.origin.x + (visual_box.size.width * 0.5f);
                const float cy = visual_box.origin.y + (visual_box.size.height * 0.5f);
                const float half_diag = (visual_box.size.width + visual_box.size.height) * 0.5f;
                const float dx = std::cos(rad) * half_diag;
                const float dy = std::sin(rad) * half_diag;
                p.draw_linear_gradient(visual_box, Point{ .x = cx - dx, .y = cy - dy },
                                       Point{ .x = cx + dx, .y = cy + dy }, colors, stops);
            } else {
                // 径向：center 为盒中心，radius 为半对角线
                const float cx = visual_box.origin.x + (visual_box.size.width * 0.5f);
                const float cy = visual_box.origin.y + (visual_box.size.height * 0.5f);
                const float r = std::sqrt((visual_box.size.width * visual_box.size.width) +
                                          (visual_box.size.height * visual_box.size.height)) *
                                0.5f;
                p.draw_radial_gradient(visual_box, Point{ .x = cx, .y = cy }, r, colors, stops);
            }
            break;
        }
        default: break;
        }
    }

    // 溢出策略裁剪：Hidden/Clip/Scroll 时把内容裁剪到本控件视觉盒子内（与 Clip 修饰语义一致）。
    // Clip 与 Hidden 当前行为相同（均裁剪视觉）；Scroll 预留，当前等同 Hidden。
    const bool overflow_clip = m_overflow != OverflowStrategy::Visible;
    if (overflow_clip) {
        p.push_clip(visual_box);
    }

    on_paint(p, content_box, ctx);

    // 内容后修饰（Paint 切片）：边框绘制于内容之上 + 内容后效 + 弹出裁剪。
    for (const auto &mn : mod.nodes()) {
        switch (mn->paint_kind()) {
        case ModifierNode::PaintKind::Blur: {
            const auto *bl = dynamic_cast<BlurNode *>(mn.get());
            if (!bl->is_backdrop()) {
                // 内容模糊：内容（含背景/边框）绘制完成后模糊整个视觉盒。
                p.blur_region(visual_box, bl->radius());
            }
            break;
        }
        case ModifierNode::PaintKind::Border: {
            const auto *br = dynamic_cast<Border *>(mn.get());
            const float bw = br->border_width();
            const Color bc = br->border_color();
            for (int i = 0; i < static_cast<int>(bw); ++i) {
                const auto inset = static_cast<float>(i);
                const float x0 = visual_box.origin.x + inset;
                const float y0 = visual_box.origin.y + inset;
                const float x1 = visual_box.right() - inset;
                const float y1 = visual_box.bottom() - inset;
                if (x1 <= x0 || y1 <= y0) {
                    break;
                }
                const float w = x1 - x0;
                const float h = y1 - y0;
                p.fill_rect(Rect{ .origin = Point{ .x = x0, .y = y0 }, .size = Size{ .width = w, .height = 1.0f } },
                            bc); // 上边
                p.fill_rect(
                    Rect{ .origin = Point{ .x = x0, .y = y1 - 1.0f }, .size = Size{ .width = w, .height = 1.0f } },
                    bc); // 下边
                p.fill_rect(Rect{ .origin = Point{ .x = x0, .y = y0 }, .size = Size{ .width = 1.0f, .height = h } },
                            bc); // 左边
                p.fill_rect(
                    Rect{ .origin = Point{ .x = x1 - 1.0f, .y = y0 }, .size = Size{ .width = 1.0f, .height = h } },
                    bc); // 右边
            }
            break;
        }
        case ModifierNode::PaintKind::ClipRounded:
        case ModifierNode::PaintKind::Clip: p.pop_clip(); break;
        case ModifierNode::PaintKind::Blend: {
            const auto *bn = dynamic_cast<BlendNode *>(mn.get());
            // 内容混合：内容绘制完成后把视觉盒像素与 tint 按模式混合。
            p.blend_region(visual_box, bn->mode(), bn->tint(), bn->strength());
            break;
        }
        case ModifierNode::PaintKind::ShaderMask: {
            const auto *sm = dynamic_cast<ShaderMaskNode *>(mn.get());
            // 着色器遮罩：内容绘制完成后按渐变淡出视觉盒像素。
            p.mask_region(visual_box, sm->mask_kind(), sm->strength());
            break;
        }
        default: break;
        }
    }

    // 弹出溢出策略裁剪（与上方 push_clip 配对）。
    if (overflow_clip) {
        p.pop_clip();
    }
}

auto Widget::render_into(Painter &dst, const Rect &local, const BuildContext &ctx) -> void {
#ifdef AURORA_ENABLE_DEBUG
    // 标记本控件本帧实际重绘（仅 render_into 入口，DL 缓存 replay / 缓存命中路径不进此处），
    // 供 repaint_highlight 判定「本帧是否重绘」。与 present_root 的调试帧计数器比较。
    m_debug_paint_frame = debug::current_debug_frame();
#endif
    const Modifier &mod = modifier.get();
    const Modifier::TransformInfo tf = mod.transform(local.size);

    const double saved_alpha = dst.global_alpha();
    if (tf.opacity < 1.0f) {
        dst.set_alpha(saved_alpha * static_cast<double>(tf.opacity));
    }

    if (tf.matrix.is_identity()) {
        // 恒等快速路径：与旧行为完全一致（零回归）。
        paint_content(dst, local, Rect{ .origin = local.origin + tf.translation, .size = tf.content_size }, ctx);
    } else {
        // 离屏合成：把整棵子树渲染到离屏缓冲，再按仿射矩阵合成回目标缓冲，
        // 旋转/缩放内容（含文本经重采样）正确；命中测试用逆矩阵映射配套。
        const Point t = tf.translation;
        const Matrix2D mtx =
            Matrix2D::from_translate(t.x, t.y).compose(tf.matrix).compose(Matrix2D::from_translate(-t.x, -t.y));

        Painter sub;
        sub.set_scale(dst.scale());
        sub.begin(static_cast<int>(local.size.width), static_cast<int>(local.size.height));
        paint_content(sub, Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = local.size },
                      Rect{ .origin = t, .size = tf.content_size }, ctx);

        const Matrix2D abs = Matrix2D::from_translate(local.origin.x, local.origin.y).compose(mtx);
        dst.composite(sub, abs);
    }

    dst.set_alpha(saved_alpha);
}

auto Widget::paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
#ifdef AURORA_ENABLE_DEBUG
    // 渲染纯度守卫接线（规格 §2.2）：进入绘制上下文即递增 g_paint_depth，
    // 配合 current_timestamp() 的 !g_paint_depth 断言捕获「绘制中读全局时钟」的反模式。
    debug::PaintPurityGuard aurora_paint_purity_guard;
    debug::check_render_purity();
#endif
    if (!show.get()) {
        return;
    }
    AURORA_PROFILE_COUNT(paint_nodes, 1); // 本帧真正参与 paint 遍历的节点数（DL 命中的子树不计）
    detail::paint_timing().paint_nodes++; // [性能排查] 镜像到光栅计时累加器，供 glue 归因
    m_paint_bounds = bounds;              // 记录绝对（窗口逻辑 dp）盒，供脏区裁剪绘制精确标记几何

    const Modifier &mod = modifier.get();
    const bool cache = std::ranges::any_of(mod.nodes(), [](const std::shared_ptr<ModifierNode> &n) -> bool {
        return n->paint_kind() == ModifierNode::PaintKind::CacheLayer;
    });

    if (cache) {
        if (m_paint_cache && m_paint_cache_valid && m_paint_cache_size.width == bounds.size.width &&
            m_paint_cache_size.height == bounds.size.height) {
            p.composite(*m_paint_cache, Matrix2D::from_translate(bounds.origin.x, bounds.origin.y));
            return;
        }
        m_paint_cache = std::make_unique<Painter>();
        m_paint_cache->set_scale(p.scale());
        m_paint_cache->begin(static_cast<int>(bounds.size.width), static_cast<int>(bounds.size.height));
        render_into(*m_paint_cache, Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }, ctx);
        m_paint_cache_size = bounds.size;
        m_paint_cache_valid = true;
        p.composite(*m_paint_cache, Matrix2D::from_translate(bounds.origin.x, bounds.origin.y));
        return;
    }

#ifdef AURORA_DISPLAY_LIST
    // Display List 快路径：仅恒等变换（无非离屏合成）参与录制/回放；
    // 旋转/缩放（非恒等 Transform）走 render_into 的离屏合成，保持原样不录制。
    // 含绘制副作用 / 每帧变动内容的控件（Hero / 转场层 / 导航宿主）不可缓存。
    // 脏区裁剪绘制（partial clip）期间跳过 DL：partial clip 下子树 paint 只画 clip 内子节点，
    // 此时录制 DL 会丢失 clip 外子节点命令，后续 full 帧 replay 该 DL 时永久丢失这些子节点，
    // 导致 partial 重绘与 full 重绘在同一像素产生不同结果。partial clip 帧 fall through 到
    // render_into 直绘（painter clip 栈保证越界裁剪），下帧 full 再重录完整 DL。
    if (!p.skip_dl_record() && mod.transform(bounds.size).matrix.is_identity() && can_cache_display_list()) {
        // 允许在外部裁剪（脏区裁剪）下命中/建立缓存：DL 本身不含外部裁剪，且所有光栅原语
        // （set_pixel / composite / fill_rect 等）在绘制时都会查裁剪栈——外部矩形裁剪会被
        // 逐像素尊重，故「在裁剪下回放缓存 DL」与「在裁剪下直绘」逐位一致，不会越界绘制。
        // 仅此一项即可消除「连续动画每帧产生脏区 → 根裁剪使全树 DL 缓存永久失效 → 每帧重录重放」
        // 的 11ms 级 glue 浪费（Google Play demo 稳态 banner/shimmer 动画持续标脏即此场景）。
        if (m_dl_valid && bounds == m_last_paint_bounds) {
            const double saved_alpha = p.global_alpha();
            AURORA_PROFILE_COUNT(dl_replays, 1);
            detail::paint_timing().dl_replays++; // [性能排查] 镜像到光栅计时累加器，供 glue 归因
            m_display_list.replay(p);            // 命中：整棵子树命令直接回放，跳过 paint 遍历
            p.set_alpha(saved_alpha);
            return;
        }
        const double saved_alpha = p.global_alpha();
        AURORA_PROFILE_COUNT(dl_records, 1);
        detail::paint_timing().dl_records++;               // [性能排查] 镜像到光栅计时累加器，供 glue 归因
        p.record(m_display_list);                          // 进入录制（清空并压栈）
        render_into(p, bounds, ctx);                       // 全部绘制录入 m_display_list（含子树）
        const bool was_dynamic = p.recording_is_dynamic(); // 在 stop() 前捕获
        p.stop();                                          // 退出录制（恢复 Direct）
        if (!was_dynamic) {
            m_dl_valid = true;
            m_last_paint_bounds = bounds;
        }
        m_display_list.replay(p); // 立即回放（Direct）上屏
        p.set_alpha(saved_alpha);
        return;
    }
    // 不可缓存（或不可录制）的控件每帧必重绘：若正处于祖先录制中，标记该录制含动态内容，
    // 使祖先不缓存本控件的易变输出（见 Painter::mark_recording_dynamic）。
    if (p.is_recording()) {
        p.mark_recording_dynamic();
    }
#endif

    render_into(p, bounds, ctx);
}

auto Widget::invalidate_paint_cache() const -> void { m_paint_cache_valid = false; }

auto Widget::hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * {
    if (!show.get()) {
        return nullptr;
    }

    const Modifier &mod = modifier.get();
    // Transform 切片：命中区随内容盒平移/仿射变换（与绘制一致）。
    const Modifier::TransformInfo tf = mod.transform(bounds.size);
    const Point local_adj = adjust_for_transform(tf, local);
    const Rect content_box{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = tf.content_size };

    for (const auto &mn : mod.nodes()) {
        if (mn->kind() == ModifierNode::Kind::Input) {
            // local 是相对本 widget 原点的局部坐标，需与“内容矩形”（原点 0）比较；
            // 否则非原点控件（如纵向布局中的按钮）永远命中失败。
            if (content_box.contains(local_adj)) {
                return this; // Clickable 拦截
            }
        }
    }

    return on_hit_test(local_adj, bounds, ctx);
}

auto Widget::hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx) -> std::vector<HitNode> {
    if (!show.get()) {
        return {};
    }

    const Modifier &mod = modifier.get();
    // Transform 切片：命中区随内容盒平移/仿射变换（与 hit_test / paint 一致）。
    const Modifier::TransformInfo tf = mod.transform(bounds.size);
    const Point local_adj = adjust_for_transform(tf, local);
    const Rect content_box{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = tf.content_size };

    // 收集子树（后代）命中链（不含自身）。后代链中各节点 origin 已由递归下降填入（相对根）。
    std::vector<HitNode> descendants = on_hit_test_chain(local_adj, bounds, ctx);

    // 本节点是否进入命中链：自身可点击（Button 的 on_click 或 Clickable 修饰）、
    // 自身有 Input 修饰（Draggable/LongPress）覆盖该点，或存在命中的后代（作为祖先）。
    // 注意：on_hit_test_chain 仅返回后代链（不含自身），自身是否入链统一在此决定，
    // 避免叶控件在 on_hit_test_chain 返回 [this] 时与基类前缀自身重复入链。
    bool self_hit = !descendants.empty() || wants_click();
    if (!self_hit) {
        for (const auto &mn : mod.nodes()) {
            if (mn->kind() == ModifierNode::Kind::Input && content_box.contains(local_adj)) {
                self_hit = true;
                break;
            }
        }
    }

    if (self_hit) {
        // 避免与 on_hit_test_chain 已返回自身（叶控件在命中点返回 [this]）重复入链。
        bool self_in_chain = false;
        for (const auto &d : descendants) {
            if (d.ptr == this) {
                self_in_chain = true;
                break;
            }
        }
        std::vector<HitNode> result;
        result.reserve(descendants.size() + 1);
        if (!self_in_chain) {
            result.emplace_back(this, weak_from_this(), bounds.origin);
        }
        result.insert(result.end(), descendants.begin(), descendants.end());
        return result;
    }
    return descendants;
}

auto Widget::mount(const BuildContext &ctx) -> void {
    if (m_mounted) {
        return; // 幂等：已挂载则跳过，避免转场切换复用同一 widget 实例时重复订阅信号
    }
    m_mounted = true;
    std::vector<SignalViewBase *> sigs;
    collect_signals(sigs);
    sigs.push_back(&modifier);
    sigs.push_back(&show);

    for (SignalViewBase *s : sigs) {
        track(*s, m_effects);
    }

    // 含需每帧计时的手势（LongPress/Draggable）或 Tooltip 延迟检测时，开启 tick 驱动，
    // 使 Widget::tick 不提前返回，从而正确推进 Tooltip 延迟计时（架构 §4.5/§5.4）。
    const Modifier &mod = modifier.get();
    if (mod.has_gesture() || mod.has_tooltip()) {
        m_needs_gesture_tick = true;
    }

    on_mount(ctx);
}

auto Widget::request_focus() -> void {
    if (FocusManager *fm = current_focus_manager()) {
        fm->request_focus(this);
    }
}

auto Widget::collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void {}

auto Widget::describe() const -> WidgetDescriptor { return WidgetDescriptor{ .name = type_name() }; }

auto Container::collect_signals(std::vector<SignalViewBase *> &out) -> void {
    for (Node &child : m_children) {
        child.widget().collect_signals(out);
    }
}

} // namespace aurora
