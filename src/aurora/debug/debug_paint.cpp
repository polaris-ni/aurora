// 可视化调试叠层 + 控件拾取实现（specification/06-app-platform.md §11.1）。
// 见 include/aurora/debug/debug_paint.h 的设计说明：体按 AURORA_ENABLE_DEBUG 裁切，Release 零开销。

#include "aurora/debug/debug_paint.h"

#include <algorithm>
#include <array>
#include <functional>
#include <vector>

#include "aurora/modifier/modifier_base.h"
#include "aurora/render/painter.h"
#include "aurora/widget/widget.h"

namespace aurora::debug {

namespace {
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
DebugPaintFlags g_flags;  ///< 全局叠层开关（DEBUG 下由 set_flags 写入）。
DebugOverlayStats g_stats;  ///< 叠层绘制统计（测试断言用）。
std::uint64_t g_debug_frame = 0;  ///< 调试帧计数（repaint_highlight 时序）。
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

/// @brief 控件修饰链是否含离屏缓存层（对应 `Modifier::cache_layer()`）。
[[nodiscard]] auto has_cache_layer(const Widget &w) -> bool {
    return std::ranges::any_of(w.modifier.get().nodes(), [](const auto &n) -> bool {
        return n && n->paint_kind() == ModifierNode::PaintKind::CacheLayer;
    });
}

/// @brief 递归遍历整棵 widget 树（先根后子树），对每个控件调用 `fn`。
auto traverse(const Widget &w, const std::function<void(const Widget &)> &fn) -> void {
    fn(w);
    w.for_each_child([&](const Widget &c) -> void { traverse(c, fn); });
}

/// @brief repaint_highlight 循环调色板（rainbow）：每帧推进色相。
[[nodiscard]] auto rainbow(std::uint64_t frame) -> Color {
    static constexpr std::array AURORA_RAINBOW = {
        Color(255, 60, 60, 120),  Color(255, 160, 40, 120), Color(255, 230, 40, 120), Color(60, 220, 80, 120),
        Color(40, 200, 220, 120), Color(80, 120, 255, 120), Color(170, 90, 230, 120),
    };
    constexpr std::size_t n = AURORA_RAINBOW.size();
    return AURORA_RAINBOW.at(frame % n);
}
}  // namespace

auto set_flags(const DebugPaintFlags &f) -> void {
#ifdef AURORA_ENABLE_DEBUG
    g_flags = f;
#else
    (void)f;
#endif
}

auto flags() -> DebugPaintFlags { return g_flags; }

auto any_flag_enabled() -> bool {
    return g_flags.layout_guides || g_flags.relayout_boundaries || g_flags.layer_borders || g_flags.repaint_highlight ||
           g_flags.overdraw;
}

auto current_debug_frame() -> std::uint64_t { return g_debug_frame; }

auto bump_debug_frame() -> void { ++g_debug_frame; }

auto overlay_stats() -> DebugOverlayStats { return g_stats; }

auto reset_overlay_stats() -> void { g_stats = DebugOverlayStats{}; }

auto paint_debug_overlays(Painter &p, const Widget &root, const Rect &root_bounds, const BuildContext &ctx) -> void {
#ifdef AURORA_ENABLE_DEBUG
    (void)root_bounds;
    (void)ctx;  // 叠层绘制不直接消费 ctx（仅 paint 内部使用）；保留形参以维持与 picker 对称签名。
    reset_overlay_stats();
    const DebugPaintFlags f = g_flags;
    const std::uint64_t frame = g_debug_frame;

    const bool need_per_widget = f.layout_guides || f.relayout_boundaries || f.layer_borders || f.repaint_highlight;
    if (!need_per_widget && !f.overdraw) {
        return;  // 无任何需要遍历的叠层
    }

    // per-widget 叠层：直接读各控件 paint_bounds / is_relayout_boundary / 修饰链 / 本帧重绘标记。
    if (need_per_widget) {
        traverse(root, [&](const Widget &w) -> void {
            const Rect box = w.paint_bounds();
            if (box.size.width <= 0.0F || box.size.height <= 0.0F) {
                return;  // 未布局 / 无尺寸控件跳过
            }
            if (f.layout_guides) {
                p.draw_rect(box, Color(0, 200, 255, 220));  // 青色 render box 边框
                ++g_stats.layout_guides_drawn;
            }
            if (f.relayout_boundaries && w.is_relayout_boundary()) {
                p.draw_rect(box, Color(255, 0, 200, 235));  // 品红边界框（与 layout_guides 区分）
                ++g_stats.relayout_boundaries_drawn;
            }
            if (f.layer_borders && has_cache_layer(w)) {
                p.draw_rect(box, Color(255, 150, 0, 235));  // 橙色离屏缓存层边框
                ++g_stats.layer_borders_drawn;
            }
            if (f.repaint_highlight && w.debug_paint_frame() == frame) {
                p.fill_rect(box, rainbow(frame));  // 本帧重绘控件循环色填充（rainbow）
                ++g_stats.repaint_highlight_drawn;
            }
        });
    }

    // overdraw 热力图（控件粒度）：跳过根（背景画布），每个控件 paint_bounds 半透明叠加，
    // 重叠区域累积 alpha 形成热力（叠得越多越饱和）。零额外光栅开销（仅 N 次 fill_rect）。
    // 这是「叠加控件层数」近似，不区分透明/局部重绘的逐像素精度（见 specification/06-app-platform.md §11.1）。
    if (f.overdraw) {
        traverse(root, [&](const Widget &w) -> void {
            if (&w == &root) {
                return;  // 根背景不计入 overdraw
            }
            const Rect box = w.paint_bounds();
            if (box.size.width <= 0.0F || box.size.height <= 0.0F) {
                return;
            }
            p.fill_rect(box, Color(255, 80, 0, 60));  // 暖色半透明，重叠累积
            ++g_stats.overdraw_regions_drawn;
        });
    }
#else
    (void)p;
    (void)root;
    (void)root_bounds;
    (void)ctx;
#endif
}

auto widget_picker(Widget &root, const Rect &root_bounds, const BuildContext &ctx, Point screen) -> DebugPickResult {
#ifdef AURORA_ENABLE_DEBUG
    DebugPickResult res;
    // hit_test_chain 接收相对 root 的局部坐标与 root 全局盒；screen 为窗口逻辑 dp（相对根原点，
    // 且 root_bounds.origin 通常为 (0,0)），直接作为局部坐标传入即可。
    const std::vector<HitNode> chain = root.hit_test_chain(screen, root_bounds, ctx);
    res.hit = !chain.empty();
    for (const HitNode &hn : chain) {
        const Widget *w = hn.ptr;  // 同步命中、主线程内，生命周期安全
        if (w == nullptr) {
            continue;
        }
        DebugPickNode node;
        node.type_name = w->type_name();
        node.bounds = Rect{.origin = hn.origin, .size = w->size()};
        res.chain.push_back(std::move(node));
    }
    return res;
#else
    (void)root;
    (void)root_bounds;
    (void)ctx;
    (void)screen;
    return DebugPickResult{};
#endif
}

}  // namespace aurora::debug
