#include "aurora/render/painter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

#include "aurora/core/math.h" // saturate / saturate_u8：像素/覆盖度钳制收口
#include "aurora/perf/counters.h"
#include "aurora/render/detail/gamma_lut.h"
#include "aurora/render/detail/paint_timing.h" // [性能排查] detail::PaintTiming / PaintTimer / paint_timing()
#include "aurora/render/detail/painter_simd.h"
#include "aurora/render/detail/painter_simd.inl"
#include "aurora/render/display_list.h"
#include "aurora/render/font_engine.h"

// 【性能豁免说明】本 TU 整体抑制以下检查，理由与 painter_simd.inl 头部一致：逐像素越界访问
// 由裁剪交集（shrink_to_clips 与 set_pixel 矩形裁剪逐字一致）在区域边界保证、指针步进与
// 对齐暂存为混合实现惯用法、窄化转换须与 SIMD 路径逐位一致（golden 测试逐位比对）。
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic, cppcoreguidelines-narrowing-conversions,
// bugprone-narrowing-conversions, readability-math-missing-parentheses, cppcoreguidelines-avoid-c-arrays,
// modernize-avoid-c-arrays, cppcoreguidelines-pro-type-reinterpret-cast,
// cppcoreguidelines-pro-bounds-constant-array-index, cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,
// readability-isolate-declaration, readability-avoid-nested-conditional-operator, modernize-use-auto)

namespace aurora {

namespace {
// 把逻辑 dp 矩形按 device pixel ratio 放大为物理像素矩形（几何绘制用）。
auto scale_rect(const Rect &r, float s) -> Rect {
    return Rect{ .origin = Point{ .x = r.origin.x * s, .y = r.origin.y * s },
                 .size = Size{ .width = r.size.width * s, .height = r.size.height * s } };
}

} // namespace

// SIMD 双实现：标量参考与 SIMD 路径逐位一致。
// gamma LUT / 标量 golden 混合实现见 aurora/render/detail/gamma_lut.{h,cpp} 与 painter_simd.inl。
using aurora::detail::blend_linear_region;
using aurora::detail::blend_srgb_over;
using aurora::detail::blend_srgb_over_region;
using aurora::detail::blur_region;
using aurora::detail::g_gamma_tables;
using aurora::detail::gradient_linear_fill;
using aurora::detail::gradient_radial_fill;
using aurora::detail::init_gamma_tables;
using aurora::detail::linear_to_srgb;

// [性能排查] 各光栅热点耗时累加器（结构/计时器/访问器见 detail/paint_timing.h）。
// 性能排查累加器为跨函数共享的可变模块状态，设计为有意的命名空间级全局；见 paint_timing()/paint_timing_last()。
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
detail::PaintTiming g_pt; // 当前帧累加器（aurora::g_pt）
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
detail::PaintTiming g_pt_last; // 上一完成绘制帧快照

auto detail::paint_timing() -> detail::PaintTiming & { return g_pt; }
auto detail::paint_timing_last() -> const detail::PaintTiming & { return g_pt_last; }
auto detail::commit_paint_frame(bool drew) -> void {
    // 仅绘制帧覆盖快照；idle 帧（未进 paint 域）保留上一绘制帧快照，避免清零成零值污染。
    if (drew) {
        g_pt_last = g_pt;
    }
    // 保留滚动缓冲重录/合成累积计数（不随帧清零，用于归因占比）。
    const std::uint64_t r0 = g_pt.scroll_r_whole;
    const std::uint64_t r1 = g_pt.scroll_r_band;
    const std::uint64_t r2 = g_pt.scroll_r_reanchor;
    const std::uint64_t r3 = g_pt.scroll_r_blit;
    g_pt = detail::PaintTiming{}; // 当前帧清零，下帧从零计
    g_pt.scroll_r_whole = r0;
    g_pt.scroll_r_band = r1;
    g_pt.scroll_r_reanchor = r2;
    g_pt.scroll_r_blit = r3;
}

// [性能排查] 构建 JSON：把给定 PaintTiming 按 divisor 折算为每帧均值（divisor=1 即原始值）。
// scene 是「整段 widget 绘制」超集，故 total 仅累加非 scene 原子类别，避免重复计数。
static auto paint_timing_to_json(const detail::PaintTiming &t, double divisor) -> std::string {
    const double d = divisor > 0.0 ? divisor : 1.0;
    const double total = t.fill + t.text + t.shadow + t.gradient + t.image + t.blur + t.border + t.line + t.composite +
                         t.clear + t.shift + t.region + t.other;
    // glue = scene 内「非栅格」胶水耗时（树遍历 / DL 回放调度 / 修饰遍历 / 命令分发等）。
    // 各栅格原语已被逐位计入 total，故 glue = scene - total 即未归类的残留；夹紧到 ≥0
    // （HUD composite 在 scene 计时域外但已计入 composite，可能略减 residue，不影响归因）。
    const double glue = t.scene > total ? (t.scene - total) : 0.0;
    const auto f = [d](double v) -> std::string { return std::to_string(v / d); };
    std::string s = "{";
    s += "\"fill\":" + f(t.fill);
    s += ",\"text\":" + f(t.text);
    s += ",\"shadow\":" + f(t.shadow);
    s += ",\"gradient\":" + f(t.gradient);
    s += ",\"image\":" + f(t.image);
    s += ",\"blur\":" + f(t.blur);
    s += ",\"border\":" + f(t.border);
    s += ",\"line\":" + f(t.line);
    s += ",\"composite\":" + f(t.composite);
    s += ",\"clear\":" + f(t.clear);
    s += ",\"shift\":" + f(t.shift);
    s += ",\"region\":" + f(t.region);
    s += ",\"scene\":" + f(t.scene);
    s += ",\"glue\":" + f(glue);
    s += ",\"other\":" + f(t.other);
    s += ",\"total\":" + f(total);
    s += ",\"paint_nodes\":" + std::to_string(t.paint_nodes);
    s += ",\"dl_replays\":" + std::to_string(t.dl_replays);
    s += ",\"dl_records\":" + std::to_string(t.dl_records);
    s += ",\"scroll_r_whole\":" + std::to_string(t.scroll_r_whole);
    s += ",\"scroll_r_band\":" + std::to_string(t.scroll_r_band);
    s += ",\"scroll_r_reanchor\":" + std::to_string(t.scroll_r_reanchor);
    s += ",\"scroll_r_blit\":" + std::to_string(t.scroll_r_blit);
    s += "}";
    return s;
}

// [性能排查] 整个累计窗口的逐原语耗时（按 per_frame_divisor 折算每帧均值），读取后清零。
// 仅供 DEBUG 排查，不用于生产。
auto paint_primitive_timing_json(double per_frame_divisor) -> std::string {
    const std::string s = paint_timing_to_json(g_pt, per_frame_divisor);
    g_pt = detail::PaintTiming{}; // 读取后清零
    return s;
}

// [性能排查] 上一完成绘制帧的逐原语耗时（divisor=1，即该帧真实耗时），不重置。
// 用于每秒打印「代表性绘制帧」分解，避免 idle 帧零值污染与整秒/FPS 折算误差。
auto paint_primitive_timing_last_json() -> std::string { return paint_timing_to_json(g_pt_last, 1.0); }

auto paint_timing_scene_last() -> double { return g_pt.scene; }

// 给定扫描线 y，计算圆角矩形 SDF ≤ t 的 x 范围（半开区间 [x0, x1)）。
// 在此范围内 coverage 恒为 1.0（全覆写），可走快速路径跳过逐像素 sqrt。
// t = -0.5（anti_alias）或 0（硬边）对应 coverage == 1.0 的精确边界。
auto Painter::rounded_full_x_range(const ClipRegion &cr, int y, float t) -> std::pair<int, int> {
    const Rect &r = cr.rect;
    const float rad = std::min(cr.radius, std::min(r.size.width, r.size.height) * 0.5f);
    const float left = r.origin.x;
    const float right = r.right();
    const float top = r.origin.y;
    const float bottom = r.bottom();
    const float cx = (left + right) * 0.5f;
    const float cy = (top + bottom) * 0.5f;
    const float hw = (right - left) * 0.5f;
    const float hh = (bottom - top) * 0.5f;
    const float py = std::fabs(static_cast<float>(y) - cy) - hh + rad;
    if (py > -t) {
        return { 0, 0 };
    }
    float half_range = NAN;
    if (py <= 0.0f) {
        half_range = hw;
    } else {
        const float disc = (t * t) - (py * py);
        if (disc < 0.0f) {
            return { 0, 0 };
        }
        half_range = hw - rad + std::sqrt(disc);
    }
    int x0 = static_cast<int>(std::ceil(cx - half_range));
    int x1 = static_cast<int>(std::ceil(cx + half_range)) + 1;
    x0 = std::max(x0, static_cast<int>(std::ceil(left)));
    x1 = std::min(x1, static_cast<int>(std::floor(right)) + 1);
    return { x0, x1 };
}

// 逐像素原语的迭代范围收缩：与裁剪栈各矩形求交（物理像素，取整与 fill_rect 快路径/
// coverage 的 contains 含右/下边界语义像素级等价）。coverage 对任何裁剪区（含圆角）
// 先做 rect.contains 硬门槛，裁剪矩形外的像素必然写不进去——收缩只剔除必被丢弃的
// 迭代，结果逐位不变；部分脏区重绘（push_clip 脏矩形）下避免全屏逐像素白扫
// （全屏圆角背景/渐变/阴影在拖选帧扫全窗，大窗口下数十 ms/帧）。
auto Painter::shrink_to_clips(int &x0, int &y0, int &x1, int &y1) const -> bool {
    for (const ClipRegion &cr : m_clip_stack) {
        x0 = std::max(x0, static_cast<int>(std::ceil(cr.rect.origin.x)));
        y0 = std::max(y0, static_cast<int>(std::ceil(cr.rect.origin.y)));
        x1 = std::min(x1, static_cast<int>(std::floor(cr.rect.right())) + 1);
        y1 = std::min(y1, static_cast<int>(std::floor(cr.rect.bottom())) + 1);
    }
    return x0 < x1 && y0 < y1;
}

auto Painter::begin(int width, int height) -> void {
    // 逻辑 dp 尺寸 → 物理像素缓冲（高 DPI 清晰、1:1 贴窗口）。
    m_width = static_cast<int>(std::lround(width * m_scale));
    m_height = static_cast<int>(std::lround(height * m_scale));
    if (m_width <= 0) {
        m_width = 1;
    }
    if (m_height <= 0) {
        m_height = 1;
    }
    m_pixels.assign(static_cast<size_t>(m_width) * static_cast<size_t>(m_height) * 4u, 0);
    // 新帧裁剪栈归零兜底：push/pop 由调用方配对，但若某处失衡（历史上曾因
    // push_clip_rounded 双压泄漏造成整窗白屏），不得跨帧扩散。
    m_clip_stack.clear();
    m_has_rounded_clip = false;
}

auto Painter::width() const -> int { return m_width; }

auto Painter::height() const -> int { return m_height; }

auto Painter::data() const -> const std::uint8_t * { return m_pixels.data(); }

auto Painter::fill_rect(const Rect &r, Color c) -> void {
    if (is_recording()) {
        DrawCmd cmd;
        cmd.kind = CmdKind::FillRect;
        cmd.bounds = r;
        cmd.color = c;
        m_recording_stack.back()->push_cmd(cmd);
        return;
    }
    const Rect pr = scale_rect(r, m_scale);
    int x0 = static_cast<int>(std::floor(std::max(0.0f, pr.origin.x)));
    int y0 = static_cast<int>(std::floor(std::max(0.0f, pr.origin.y)));
    int x1 = static_cast<int>(std::ceil(std::min(static_cast<float>(m_width), pr.origin.x + pr.size.width)));
    int y1 = static_cast<int>(std::ceil(std::min(static_cast<float>(m_height), pr.origin.y + pr.size.height)));
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    AURORA_PROFILE_COUNT(draw_calls, 1);
    AURORA_PROFILE_COUNT(fill_rects, 1);
    AURORA_PROFILE_COUNT(pixels_filled, static_cast<std::uint64_t>(x1 - x0) * static_cast<std::uint64_t>(y1 - y0));
    detail::PaintTimer guard{ &g_pt.fill };
    // 快速路径命中即绘制并返回；否则 x0..y1 保持未裁剪交集，交由慢路径处理。
    if (fill_rect_fast_path(x0, y0, x1, y1, c)) {
        return;
    }
    fill_rect_slow_path(x0, y0, x1, y1, c);
}

auto Painter::fill_rect_fast_path(int &x0, int &y0, int &x1, int &y1, Color c) -> bool {
    // 行级快速路径：全局透明度为 1 且裁剪栈全为非圆角矩形时，把裁剪收缩进边界后
    // 整行写入，避免逐像素的裁剪栈遍历与浮点混合——全屏背景/容器底色/选区高亮
    // 都走此路径；最大化窗口（如 2560×1440 物理）下逐像素路径单次全屏填充 >10ms，
    // 每帧两次全屏填充（清屏 + 容器背景）即是拖选时选区高亮卡顿的主因。
    bool rect_clips_only = (m_global_alpha == 1.0);
    if (rect_clips_only) {
        for (const ClipRegion &cr : m_clip_stack) {
            if (cr.rounded && cr.radius > 0.0f) {
                rect_clips_only = false;
                break;
            }
        }
    }
    if (!rect_clips_only) {
        return false;
    }

    // 与 coverage 的 contains 语义像素级等价：保留 x ∈ [ceil(origin.x), floor(right())]
    // （contains 含右/下边界），故上界为 floor(...)+1（半开）。
    for (const ClipRegion &cr : m_clip_stack) {
        x0 = std::max(x0, static_cast<int>(std::ceil(cr.rect.origin.x)));
        y0 = std::max(y0, static_cast<int>(std::ceil(cr.rect.origin.y)));
        x1 = std::min(x1, static_cast<int>(std::floor(cr.rect.right())) + 1);
        y1 = std::min(y1, static_cast<int>(std::floor(cr.rect.bottom())) + 1);
    }
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, m_width);
    y1 = std::min(y1, m_height);
    if (x0 >= x1 || y0 >= y1) {
        return true;
    }
    const std::size_t row_bytes = static_cast<std::size_t>(x1 - x0) * 4u;
    std::uint8_t *first =
        m_pixels.data() +
        (((static_cast<std::size_t>(y0) * static_cast<std::size_t>(m_width)) + static_cast<std::size_t>(x0)) * 4u);
    if (c.m_a == 255) {
        // 不透明覆写：构造首行后其余行整行 memcpy（与 set_pixel 在 a=255 时结果一致）。
        for (int x = 0; x < x1 - x0; ++x) {
            first[(static_cast<std::size_t>(x) * 4u) + 0u] = c.m_r;
            first[(static_cast<std::size_t>(x) * 4u) + 1u] = c.m_g;
            first[(static_cast<std::size_t>(x) * 4u) + 2u] = c.m_b;
            first[(static_cast<std::size_t>(x) * 4u) + 3u] = 255;
        }
        for (int y = y0 + 1; y < y1; ++y) {
            std::memcpy(m_pixels.data() + (((static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width)) +
                                            static_cast<std::size_t>(x0)) *
                                           4u),
                        first, row_bytes);
        }
    } else {
        // 半透明：内联 source-over（与 set_pixel 同一浮点公式，位级一致），
        // 省去逐像素的越界/裁剪/全局透明度开销。
        // 注意：必须乘以全局透明度 m_global_alpha（转场淡入淡出依赖），
        // 与 set_pixel 的 c.a *= m_global_alpha 语义一致。
        const float a = (static_cast<float>(c.m_a) * static_cast<float>(m_global_alpha)) / 255.0f;
        const float inv = 1.0f - a;
        for (int y = y0; y < y1; ++y) {
            std::uint8_t *row =
                m_pixels.data() +
                (((static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width)) + static_cast<std::size_t>(x0)) *
                 4u);
            for (int x = 0; x < x1 - x0; ++x) {
                std::uint8_t *px = row + (static_cast<std::size_t>(x) * 4u);
                px[0] = static_cast<std::uint8_t>((px[0] * inv) + (c.m_r * a));
                px[1] = static_cast<std::uint8_t>((px[1] * inv) + (c.m_g * a));
                px[2] = static_cast<std::uint8_t>((px[2] * inv) + (c.m_b * a));
                px[3] = 255;
            }
        }
    }
    return true;
}

auto Painter::fill_rect_slow_path(int x0, int y0, int x1, int y1, Color c) -> void {
    // 慢路径：圆角裁剪（SDF 覆盖度）/ 全局透明度 <1 时逐像素处理；
    // 迭代范围仍收缩进裁剪交集（裁剪外像素 coverage 恒为 0，扫到也是白扫）。
    // 行级优化：每行计算圆角全覆写 x 范围（rounded_full_x_range），范围内直接
    // source-over 混合（与 set_pixel coverage=1 时位级一致），仅圆弧过渡行走
    // 逐像素 coverage（含 sqrt）。大窗口 Button 场景下 >95% 像素走快速路径。
    if (!shrink_to_clips(x0, y0, x1, y1)) {
        return;
    }
    // 与快速路径一致：圆角裁剪交集可能越过缓冲边界（如卡片贴边时 clip 右沿 == 宽），
    // 必须夹回缓冲，否则下方逐像素内联快写会越界写（0xC0000005）。
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, m_width);
    y1 = std::min(y1, m_height);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    // 预计算各圆角裁剪的 coverage=1 阈值：anti_alias 时 SDF≤-0.5 → cov=1，
    // 硬边时 SDF≤0 → cov=1。
    struct RoundedInfo {
        const ClipRegion *cr;
        float threshold;
    };
    std::vector<RoundedInfo> rounded_clips;
    for (const ClipRegion &cr : m_clip_stack) {
        if (cr.rounded && cr.radius > 0.0f) {
            rounded_clips.push_back({ .cr = &cr, .threshold = cr.anti_alias ? -0.5f : 0.0f });
        }
    }
    // 慢路径逐像素 source-over 仍须乘以全局透明度 m_global_alpha（与 set_pixel 一致），
    // 否则 set_alpha(<1) 淡入淡出对矩形填充无效（转场/全局淡变）。
    const float fa = (static_cast<float>(c.m_a) * static_cast<float>(m_global_alpha)) / 255.0f;
    const float finv = 1.0f - fa;
    for (int y = y0; y < y1; ++y) {
        // 全覆写 x 范围 = 所有圆角裁剪各行全覆写范围的交集
        int safe_x0 = x0;
        int safe_x1 = x1;
        for (const auto &ri : rounded_clips) {
            const auto [fx0, fx1] = rounded_full_x_range(*ri.cr, y, ri.threshold);
            safe_x0 = std::max(safe_x0, fx0);
            safe_x1 = std::min(safe_x1, fx1);
        }
        if (safe_x0 > safe_x1) {
            safe_x0 = safe_x1 = x0; // 退化：全行走逐像素
        }
        // 左过渡区（圆弧）：逐像素 coverage
        for (int x = x0; x < safe_x0; ++x) {
            set_pixel(x, y, c);
        }
        // 行内快速路径：coverage 恒为 1，直接 source-over（与 set_pixel 位级一致）
        if (safe_x0 < safe_x1) {
            std::uint8_t *row = m_pixels.data() + (((static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width)) +
                                                    static_cast<std::size_t>(safe_x0)) *
                                                   4u);
            const int count = safe_x1 - safe_x0;
            blend_linear_region(row, c.m_r, c.m_g, c.m_b, fa, finv, count);
        }
        // 右过渡区（圆弧）：逐像素 coverage
        for (int x = safe_x1; x < x1; ++x) {
            set_pixel(x, y, c);
        }
    }
}

auto Painter::draw_rect(const Rect &r, Color c) -> void {
    if (is_recording()) {
        DrawCmd cmd;
        cmd.kind = CmdKind::DrawRect;
        cmd.bounds = r;
        cmd.color = c;
        m_recording_stack.back()->push_cmd(cmd);
        return;
    }
    const float x0 = r.origin.x;
    const float y0 = r.origin.y;
    const float w = r.size.width;
    const float h = r.size.height;
    fill_rect(Rect{ .origin = Point{ .x = x0, .y = y0 }, .size = Size{ .width = w, .height = 1.0f } }, c);
    fill_rect(Rect{ .origin = Point{ .x = x0, .y = y0 + std::max(0.0f, h - 1.0f) },
                    .size = Size{ .width = w, .height = 1.0f } },
              c);
    fill_rect(Rect{ .origin = Point{ .x = x0, .y = y0 }, .size = Size{ .width = 1.0f, .height = h } }, c);
    fill_rect(Rect{ .origin = Point{ .x = x0 + std::max(0.0f, w - 1.0f), .y = y0 },
                    .size = Size{ .width = 1.0f, .height = h } },
              c);
}

auto Painter::draw_line(Point a, Point b, float width, Color c) -> void {
    if (is_recording()) {
        DrawCmd cmd;
        cmd.kind = CmdKind::DrawLine;
        cmd.pt0 = a;
        cmd.pt1 = b;
        cmd.f0 = width;
        cmd.color = c;
        m_recording_stack.back()->push_cmd(cmd);
        return;
    }
    if (width <= 0.0f || c.m_a == 0) {
        return;
    }
    AURORA_PROFILE_COUNT(draw_calls, 1);
    detail::PaintTimer guard{ &g_pt.line };
    // 逻辑 dp → 物理像素；半宽 + 1px 羽化带决定包围盒。
    const float ax = a.x * m_scale;
    const float ay = a.y * m_scale;
    const float bx = b.x * m_scale;
    const float by = b.y * m_scale;
    const float hw = width * m_scale * 0.5f;
    const float pad = hw + 1.0f;
    int x0 = static_cast<int>(std::floor(std::min(ax, bx) - pad));
    int y0 = static_cast<int>(std::floor(std::min(ay, by) - pad));
    int x1 = static_cast<int>(std::ceil(std::max(ax, bx) + pad)) + 1;
    int y1 = static_cast<int>(std::ceil(std::max(ay, by) + pad)) + 1;
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, m_width);
    y1 = std::min(y1, m_height);
    if (!shrink_to_clips(x0, y0, x1, y1)) {
        return;
    }
    const float dx = bx - ax;
    const float dy = by - ay;
    const float len_sq = (dx * dx) + (dy * dy);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            // 点到线段距离（像素中心采样）：投影参数 t 钳到 [0,1] 得圆帽。
            const float px = static_cast<float>(x) + 0.5f - ax;
            const float py = static_cast<float>(y) + 0.5f - ay;
            const float t = len_sq > 0.0f ? aurora::saturate(((px * dx) + (py * dy)) / len_sq) : 0.0f;
            const float ex = px - (t * dx);
            const float ey = py - (t * dy);
            const float dist = std::sqrt((ex * ex) + (ey * ey));
            const float cov = aurora::saturate(hw + 0.5f - dist); // 1px 羽化
            if (cov <= 0.0f) {
                continue;
            }
            Color pc = c;
            pc.m_a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.m_a) * cov));
            set_pixel(x, y, pc); // set_pixel 另行应用裁剪 coverage 与全局透明度
        }
    }
}

auto Painter::fill_rounded_rect(const Rect &r, float radius, Color c) -> void {
    // 组合实现：录制模式下三条子命令均可录制，无需新增命令类型。
    if (radius <= 0.0f) {
        fill_rect(r, c);
        return;
    }
    push_clip_rounded(r, radius);
    fill_rect(r, c);
    pop_clip();
}

auto Painter::draw_rounded_border(const Rect &r, float radius, float thickness, Color c) -> void {
    if (is_recording()) {
        DrawCmd cmd;
        cmd.kind = CmdKind::RoundedBorder;
        cmd.bounds = r;
        cmd.f0 = radius;
        cmd.f1 = thickness;
        cmd.color = c;
        m_recording_stack.back()->push_cmd(cmd);
        return;
    }
    if (thickness <= 0.0f || c.m_a == 0 || r.size.width <= 0.0f || r.size.height <= 0.0f) {
        return;
    }
    AURORA_PROFILE_COUNT(draw_calls, 1);
    detail::PaintTimer guard{ &g_pt.border };
    // 圆角矩形 SDF 带状覆盖度：外缘 d=0、内缘 d=-t（向内描边），两侧各 0.5px 羽化。
    const Rect pr = scale_rect(r, m_scale);
    const float t = thickness * m_scale;
    const float rad = std::min(radius * m_scale, std::min(pr.size.width, pr.size.height) * 0.5f);
    const float cx = pr.origin.x + (pr.size.width * 0.5f);
    const float cy = pr.origin.y + (pr.size.height * 0.5f);
    const float hw = pr.size.width * 0.5f;
    const float hh = pr.size.height * 0.5f;
    int x0 = static_cast<int>(std::floor(pr.origin.x - 1.0f));
    int y0 = static_cast<int>(std::floor(pr.origin.y - 1.0f));
    int x1 = static_cast<int>(std::ceil(pr.right() + 1.0f)) + 1;
    int y1 = static_cast<int>(std::ceil(pr.bottom() + 1.0f)) + 1;
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, m_width);
    y1 = std::min(y1, m_height);
    if (!shrink_to_clips(x0, y0, x1, y1)) {
        return;
    }
    // 内部安全区（距边框带 > 1px 的矩形）整行跳过，避免大矩形内部白扫。
    const float inset = t + rad + 1.0f;
    const int sx0 = static_cast<int>(std::ceil(pr.origin.x + inset));
    const int sx1 = static_cast<int>(std::floor(pr.right() - inset));
    const int sy0 = static_cast<int>(std::ceil(pr.origin.y + inset));
    const int sy1 = static_cast<int>(std::floor(pr.bottom() - inset));
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (y >= sy0 && y <= sy1 && x >= sx0 && x <= sx1) {
                x = sx1; // 内部安全区：跳到右缘（循环自增后继续扫右侧边框）
                continue;
            }
            // 圆角矩形 SDF（像素中心采样）。
            const float qx = std::fabs(static_cast<float>(x) + 0.5f - cx) - (hw - rad);
            const float qy = std::fabs(static_cast<float>(y) + 0.5f - cy) - (hh - rad);
            const float ox = std::max(qx, 0.0f);
            const float oy = std::max(qy, 0.0f);
            const float d = std::sqrt((ox * ox) + (oy * oy)) + std::min(std::max(qx, qy), 0.0f) - rad;
            // 带状覆盖：|d + t/2| <= t/2 为实体，两侧 0.5px 羽化。
            const float band = std::fabs(d + (t * 0.5f));
            const float cov = aurora::saturate((t * 0.5f) + 0.5f - band);
            if (cov <= 0.0f) {
                continue;
            }
            Color pc = c;
            pc.m_a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.m_a) * cov));
            set_pixel(x, y, pc);
        }
    }
}

auto Painter::clear_rect(const Rect &r) -> void {
    if (is_recording()) {
        DrawCmd cmd;
        cmd.kind = CmdKind::ClearRect;
        cmd.bounds = r;
        m_recording_stack.back()->push_cmd(cmd);
        return;
    }
    // 新帧零基底重置：直接写零，不走混合/裁剪/全局透明度。取整与 fill_rect 快路径的
    // 裁剪收缩语义一致（保留 x ∈ [ceil(l), floor(r)]，含右/下边界），保证被后续裁剪绘制
    // 触及的每个像素都先回到与 begin 后一致的零基底。
    detail::PaintTimer guard{ &g_pt.clear };
    const Rect pr = scale_rect(r, m_scale);
    const int x0 = std::max(0, static_cast<int>(std::ceil(pr.origin.x)));
    const int y0 = std::max(0, static_cast<int>(std::ceil(pr.origin.y)));
    const int x1 = std::min(m_width, static_cast<int>(std::floor(pr.right())) + 1);
    const int y1 = std::min(m_height, static_cast<int>(std::floor(pr.bottom())) + 1);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    AURORA_PROFILE_COUNT(draw_calls, 1);
    AURORA_PROFILE_COUNT(pixels_filled, static_cast<std::uint64_t>(x1 - x0) * static_cast<std::uint64_t>(y1 - y0));
    const std::size_t row_bytes = static_cast<std::size_t>(x1 - x0) * 4u;
    for (int y = y0; y < y1; ++y) {
        std::memset(m_pixels.data() + (((static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width)) +
                                        static_cast<std::size_t>(x0)) *
                                       4u),
                    0, row_bytes);
    }
}

auto Painter::shift_pixels(float dy) -> void {
    if (is_recording()) {
        // Display List 无「像素搬移」命令：本原语只服务直绘的离屏缓冲（滚动 reanchor），
        // 录制态静默忽略，避免录出与回放不一致的半成品。
        return;
    }
    detail::PaintTimer guard{ &g_pt.shift };
    if (m_pixels.empty() || m_width <= 0 || m_height <= 0) {
        return;
    }
    // 逻辑 dp → 物理行数（与 begin 的 lround 取整一致）。
    const int shift = static_cast<int>(std::lround(dy * m_scale));
    if (shift == 0) {
        return;
    }
    const int abs_shift = shift > 0 ? shift : -shift;
    const std::size_t row_bytes = static_cast<std::size_t>(m_width) * 4u;
    std::uint8_t *base = m_pixels.data();
    AURORA_PROFILE_COUNT(draw_calls, 1);
    if (abs_shift >= m_height) {
        // 位移超过缓冲高度：无像素可复用，整块回到零基底。
        AURORA_PROFILE_COUNT(pixels_filled, static_cast<std::uint64_t>(m_width) * static_cast<std::uint64_t>(m_height));
        std::memset(base, 0, m_pixels.size());
        return;
    }
    const auto keep_rows = static_cast<std::size_t>(m_height - abs_shift);
    const std::size_t keep_bytes = keep_rows * row_bytes;
    const std::size_t gap_bytes = static_cast<std::size_t>(abs_shift) * row_bytes;
    AURORA_PROFILE_COUNT(pixels_filled, static_cast<std::uint64_t>(m_width) * static_cast<std::uint64_t>(m_height));
    if (shift > 0) {
        // 内容下移：行 [0, keep_rows) → [abs_shift, m_height)，顶部 abs_shift 行让出。
        std::memmove(base + gap_bytes, base, keep_bytes);
        std::memset(base, 0, gap_bytes);
    } else {
        // 内容上移：行 [abs_shift, m_height) → [0, keep_rows)，底部 abs_shift 行让出。
        std::memmove(base, base + gap_bytes, keep_bytes);
        std::memset(base + keep_bytes, 0, gap_bytes);
    }
}

auto Painter::draw_text(const Rect &r, const std::string &s, const Font &f, Color c) -> void {
    if (is_recording()) {
        record_text_cmd(r, s, f, c, render::FontEngine::text_aa_mode(), render::TextLayoutOpts{});
        return;
    }
    AURORA_PROFILE_COUNT(draw_calls, 1);
    AURORA_PROFILE_COUNT(draw_texts, 1);
    // 委托给 FontEngine：真实字体渲染（Windows/GDI）或回退内置位图字体。
    // 原点换算为物理像素（FontEngine 以物理分辨率光栅字形并逐物理像素写入，不再乘 scale）。
    const float sc = m_scale;
    const Rect pr{ .origin = Point{ .x = r.origin.x * sc, .y = r.origin.y * sc }, .size = r.size };
    {
        detail::PaintTimer guard{ &g_pt.text };
        render::FontEngine::draw_text(*this, pr, s, f, c);
    }
}

auto Painter::draw_text(const Rect &r, const std::string &s, const Font &f, Color c, const render::TextLayoutOpts &opts)
    -> void {
    if (is_recording()) {
        record_text_cmd(r, s, f, c, render::FontEngine::text_aa_mode(), opts);
        return;
    }
    AURORA_PROFILE_COUNT(draw_calls, 1);
    AURORA_PROFILE_COUNT(draw_texts, 1);
    const float sc = m_scale;
    const Rect pr{ .origin = Point{ .x = r.origin.x * sc, .y = r.origin.y * sc }, .size = r.size };
    {
        detail::PaintTimer guard{ &g_pt.text };
        render::FontEngine::draw_text(*this, pr, s, f, c, opts);
    }
}

auto Painter::draw_text(const Rect &r, const std::string &s, const Font &f, Color c, render::TextAAMode aa_mode,
                        const render::TextLayoutOpts &opts) -> void {
    if (is_recording()) {
        record_text_cmd(r, s, f, c, aa_mode, opts);
        return;
    }
    AURORA_PROFILE_COUNT(draw_calls, 1);
    AURORA_PROFILE_COUNT(draw_texts, 1);
    const float sc = m_scale;
    const Rect pr{ .origin = Point{ .x = r.origin.x * sc, .y = r.origin.y * sc }, .size = r.size };
    {
        detail::PaintTimer guard{ &g_pt.text };
        render::FontEngine::draw_text(*this, pr, s, f, c, aa_mode, opts);
    }
}

auto Painter::blend_pixel(int x, int y, Color c) -> void { set_pixel(x, y, c); }

auto Painter::blend_rect(const Rect &r, Color c) -> void { fill_rect(r, c); }

auto Painter::draw_image(const Image &img, const Rect &dest) -> void {
    if (is_recording()) {
        DrawCmd cmd;
        cmd.kind = CmdKind::DrawImage;
        cmd.bounds = dest;
        cmd.image_idx = m_recording_stack.back()->add_image(img);
        m_recording_stack.back()->push_cmd(cmd);
        return;
    }
    detail::PaintTimer guard{ &g_pt.image };
    if (img.pixels.empty() || img.width <= 0 || img.height <= 0) {
        return;
    }
    // 校验 Image 的核心不变量：pixels 至少覆盖 width*height*4（RGBA8）。
    // 该不变量此前无人强制：Image 的字段是公开可写的，且解码器/用户代码可能构造出
    // 「声明尺寸大于实际缓冲」的图（如解码中途失败仍保留了 width/height），
    // 此时下方 sample() 只按 width/height 钳制下标，会越界读堆内存并把它画到屏幕上。
    if (static_cast<std::uint64_t>(img.pixels.size()) <
        static_cast<std::uint64_t>(img.width) * static_cast<std::uint64_t>(img.height) * 4u) {
        return;
    }
    const Rect pd = scale_rect(dest, m_scale);
    int dx0 = static_cast<int>(std::floor(std::max(0.0f, pd.origin.x)));
    int dy0 = static_cast<int>(std::floor(std::max(0.0f, pd.origin.y)));
    int dx1 = static_cast<int>(std::ceil(std::min(static_cast<float>(m_width), pd.origin.x + pd.size.width)));
    int dy1 = static_cast<int>(std::ceil(std::min(static_cast<float>(m_height), pd.origin.y + pd.size.height)));
    if (!shrink_to_clips(dx0, dy0, dx1, dy1)) {
        return; // 裁剪外纯白扫（set_pixel 恒丢弃），直接早退
    }
    AURORA_PROFILE_COUNT(draw_calls, 1);
    AURORA_PROFILE_COUNT(pixels_filled, static_cast<std::uint64_t>(std::max(0, dx1 - dx0)) *
                                            static_cast<std::uint64_t>(std::max(0, dy1 - dy0)));
    const float sx = pd.size.width > 0.0f ? img.width / pd.size.width : 1.0f;
    const float sy = pd.size.height > 0.0f ? img.height / pd.size.height : 1.0f;
    const auto img_w = static_cast<std::size_t>(img.width);
    const auto img_h = static_cast<std::size_t>(img.height);
    const std::size_t row4 = img_w * 4u;
    // 双线性采样：在 premultiplied-alpha 空间插值，避免半透明边缘出现暗边/光晕；
    // 同时消除最近邻缩放引入的阶梯锯齿（图标 96px→显示 64px 及设备像素比二次缩放）。
    auto sample = [&](int ix, int iy) -> const std::uint8_t * {
        if (ix < 0) {
            ix = 0;
        } else if (std::cmp_greater_equal(ix, img_w)) {
            ix = static_cast<int>(img_w) - 1;
        }
        if (iy < 0) {
            iy = 0;
        } else if (std::cmp_greater_equal(iy, img_h)) {
            iy = static_cast<int>(img_h) - 1;
        }
        return &img.pixels[(static_cast<std::size_t>(iy) * row4) + (static_cast<std::size_t>(ix) * 4u)];
    };
    for (int y = dy0; y < dy1; ++y) {
        const float fy = ((static_cast<float>(y) + 0.5f - pd.origin.y) * sy) - 0.5f;
        const int y0 = static_cast<int>(std::floor(fy));
        const float ty = fy - static_cast<float>(y0);
        for (int x = dx0; x < dx1; ++x) {
            const float fx = ((static_cast<float>(x) + 0.5f - pd.origin.x) * sx) - 0.5f;
            const int x0 = static_cast<int>(std::floor(fx));
            const float tx = fx - static_cast<float>(x0);
            const std::uint8_t *p00 = sample(x0, y0);
            const std::uint8_t *p10 = sample(x0 + 1, y0);
            const std::uint8_t *p01 = sample(x0, y0 + 1);
            const std::uint8_t *p11 = sample(x0 + 1, y0 + 1);
            const float a00 = p00[3] / 255.0f;
            const float a10 = p10[3] / 255.0f;
            const float a01 = p01[3] / 255.0f;
            const float a11 = p11[3] / 255.0f;
            const float w00 = (1.0f - tx) * (1.0f - ty);
            const float w10 = tx * (1.0f - ty);
            const float w01 = (1.0f - tx) * ty;
            const float w11 = tx * ty;
            const float pa = (a00 * w00) + (a10 * w10) + (a01 * w01) + (a11 * w11);
            std::uint8_t r = 0;
            std::uint8_t g = 0;
            std::uint8_t b = 0;
            if (pa > 1e-6f) {
                const auto ch = [&](int c) -> std::uint8_t {
                    const float v = ((p00[c] * a00) * w00) + ((p10[c] * a10) * w10) + ((p01[c] * a01) * w01) +
                                    ((p11[c] * a11) * w11);
                    return static_cast<std::uint8_t>(v / pa);
                };
                r = ch(0);
                g = ch(1);
                b = ch(2);
            }
            const std::uint8_t a = pa >= 1.0f ? 0xFF : static_cast<std::uint8_t>(pa * 255.0f);
            set_pixel(x, y, Color{ r, g, b, a });
        }
    }
}

auto Painter::push_clip(const Rect &r) -> void {
    if (is_recording()) {
        DrawCmd cmd;
        cmd.kind = CmdKind::PushClip;
        cmd.bounds = r;
        m_recording_stack.back()->push_cmd(cmd);
        // 录制模式下同步维护裁剪栈，使 clip_bounds() 在录制期间返回正确区域
    }
    if (m_clip_stack.empty()) {
        m_clip_stack.push_back(
            ClipRegion{ .rect = scale_rect(r, m_scale), .rounded = false, .radius = 0.0f, .anti_alias = false });
        return;
    }
    const Rect &top = m_clip_stack.back().rect;
    const Rect pr = scale_rect(r, m_scale);
    const float x = std::max(top.origin.x, pr.origin.x);
    const float y = std::max(top.origin.y, pr.origin.y);
    const float right = std::min(top.right(), pr.right());
    const float bottom = std::min(top.bottom(), pr.bottom());
    m_clip_stack.push_back(ClipRegion{
        .rect = Rect{ .origin = Point{ .x = x, .y = y },
                      .size = Size{ .width = std::max(0.0f, right - x), .height = std::max(0.0f, bottom - y) } },
        .rounded = false,
        .radius = 0.0f });
    // push_clip 只压矩形，m_has_rounded_clip 不变
}

auto Painter::push_clip_rounded(const Rect &r, float radius, bool anti_alias) -> void {
    if (is_recording()) {
        DrawCmd cmd;
        cmd.kind = CmdKind::PushClipRounded;
        cmd.bounds = r;
        cmd.f0 = radius;
        cmd.rounded_aa = anti_alias;
        m_recording_stack.back()->push_cmd(cmd);
        // 录制模式下同步维护裁剪栈，使 clip_bounds() 在录制期间返回正确区域
    }
    // 每次调用只压入一个 ClipRegion（与 pop_clip 一对一）：旧版误压两个（先压自身再压
    // 「与自身的交集」）导致裁剪栈泄漏——后续所有绘制被永久裁剪到首个圆角控件区域
    // （整窗白屏只剩最后一个圆角控件）。保留原始圆角矩形几何（SDF 形状正确）；
    // 与既有裁剪的交集由 coverage/快路径遍历所有栈层天然实现（AND 语义），无需显式求交。
    // anti_alias 始终透传，首个圆角裁剪也不退化。
    m_clip_stack.push_back(
        ClipRegion{ .rect = scale_rect(r, m_scale), .rounded = true, .radius = radius, .anti_alias = anti_alias });
    m_has_rounded_clip = true;
}

auto Painter::pop_clip() -> void {
    if (is_recording()) {
        DrawCmd cmd;
        cmd.kind = CmdKind::PopClip;
        m_recording_stack.back()->push_cmd(cmd);
        // 录制模式下同步维护裁剪栈
    }
    if (!m_clip_stack.empty()) {
        m_clip_stack.pop_back();
        // 弹出后重新计算圆角标志（可能弹出了唯一的圆角裁剪）
        m_has_rounded_clip = false;
        for (const ClipRegion &cr : m_clip_stack) {
            if (cr.rounded && cr.radius > 0.0f) {
                m_has_rounded_clip = true;
                break;
            }
        }
    }
}

auto Painter::has_clip() const -> bool { return !m_clip_stack.empty(); }

auto Painter::clip_bounds() const -> Rect {
    if (m_clip_stack.empty()) {
        // 无裁剪：返回整块画布（逻辑 dp）
        return Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                     .size = Size{ .width = static_cast<float>(m_width), .height = static_cast<float>(m_height) } };
    }
    // m_clip_stack.back().rect 已是各层矩形裁剪的交集（物理像素，pushClip 时与上一层取交），
    // 圆角裁剪退化为其外接矩形（保守，保证不误剔除）。转回逻辑 dp 以匹配控件全局坐标。
    const Rect &pr = m_clip_stack.back().rect;
    const float inv = m_scale > 0.0f ? 1.0f / m_scale : 1.0f;
    return Rect{ .origin = Point{ .x = pr.origin.x * inv, .y = pr.origin.y * inv },
                 .size = Size{ .width = pr.size.width * inv, .height = pr.size.height * inv } };
}

auto Painter::record(DisplayList &dl) -> void {
    dl.clear();
    m_recording_stack.push_back(&dl);
    m_rec_dynamic.push_back(0);
}

auto Painter::stop() -> void {
    if (!m_recording_stack.empty()) {
        m_recording_stack.pop_back();
        m_rec_dynamic.pop_back();
    }
}

auto Painter::mark_recording_dynamic() -> void {
    // 置全部层级：任何录制了动态内容的祖先都不应缓存其 Display List。
    for (auto &d : m_rec_dynamic) {
        d = 1;
    }
}

auto Painter::record_text_cmd(const Rect &r, const std::string &s, const Font &f, Color c, render::TextAAMode aa,
                              const render::TextLayoutOpts &opts) -> void {
    DrawCmd cmd;
    cmd.kind = CmdKind::DrawText;
    cmd.bounds = r;
    cmd.color = c;
    cmd.font_idx = m_recording_stack.back()->add_font(f);
    cmd.aa_mode = aa;
    cmd.text_ls = opts.letter_spacing;
    cmd.text_ws = opts.word_spacing;
    cmd.text_italic = opts.italic;
    cmd.str_idx = m_recording_stack.back()->add_string(s);
    m_recording_stack.back()->push_cmd(cmd);
}

namespace {
// 圆角矩形有符号距离场（iq 圆角盒 SDF）：负值在内部，正值在外部。
auto sd_round_rect(float x, float y, const Rect &r, float rad) -> float {
    const float left = r.origin.x;
    const float top = r.origin.y;
    const float right = r.right();
    const float bottom = r.bottom();
    const float half_w = (right - left) * 0.5f;
    const float half_h = (bottom - top) * 0.5f;
    rad = std::min({ rad, half_w, half_h });
    const float px = x - ((left + right) * 0.5f);
    const float py = y - ((top + bottom) * 0.5f);
    const float qx = std::fabs(px) - half_w + rad;
    const float qy = std::fabs(py) - half_h + rad;
    const float outside =
        std::sqrt((std::max(qx, 0.0f) * std::max(qx, 0.0f)) + (std::max(qy, 0.0f) * std::max(qy, 0.0f)));
    return std::min(std::max(qx, qy), 0.0f) + outside - rad;
}
} // namespace

auto Painter::ClipRegion::coverage(float x, float y) const -> float {
    if (!rect.contains(Point{ .x = x, .y = y })) {
        return 0.0f;
    }
    if (!rounded || radius <= 0.0f) {
        return 1.0f;
    }
    const float d = sd_round_rect(x, y, rect, radius);
    if (!anti_alias) {
        return d <= 0.0f ? 1.0f : 0.0f; // 硬遮罩（无羽化）
    }
    return std::max(0.0f, std::min(1.0f, 0.5f - d)); // 1px 抗锯齿羽化
}

auto Painter::get_pixel(int x, int y) const -> Color {
    if (x < 0 || y < 0 || x >= m_width || y >= m_height) {
        return Color{ 0, 0, 0, 0 }; // 越界返回透明色（而非默认 a=255 的不透明黑）
    }
    const std::size_t i =
        ((static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width)) + static_cast<std::size_t>(x)) * 4u;
    return Color{
        m_pixels[i + 0],
        m_pixels[i + 1],
        m_pixels[i + 2],
        m_pixels[i + 3],
    };
}

auto Painter::set_pixel(int x, int y, Color c) -> void {
    init_gamma_tables();
    if (x < 0 || y < 0 || x >= m_width || y >= m_height) {
        return;
    }
    // 应用全局透明度（转场淡入淡出）。
    c.m_a = static_cast<std::uint8_t>(std::max(0.0, std::min(255.0, static_cast<double>(c.m_a) * m_global_alpha)));
    // 快速路径：无圆角裁剪时跳过逐 clip 的 coverage 浮点计算——文本渲染
    // （blend_pixel/blend_subpixel）每像素都走此路径，裁剪栈遍历 + SDF coverage
    // 在大窗口下逐像素开销显著。纯矩形裁剪只需边界检查（coverage 恒为 1）。
    if (!m_has_rounded_clip) {
        // 纯矩形裁剪：coverage 恒为 1（已在矩形内）或 0（在矩形外），
        // 用整数边界检查替代逐 clip 浮点 coverage 遍历。
        for (const ClipRegion &cr : m_clip_stack) {
            const int cx0 = static_cast<int>(std::ceil(cr.rect.origin.x));
            const int cy0 = static_cast<int>(std::ceil(cr.rect.origin.y));
            const int cx1 = static_cast<int>(std::floor(cr.rect.right())); // 含右/下边界
            const int cy1 = static_cast<int>(std::floor(cr.rect.bottom()));
            if (x < cx0 || x > cx1 || y < cy0 || y > cy1) {
                return; // 被某层矩形裁剪裁掉
            }
        }
        const std::size_t i =
            ((static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width)) + static_cast<std::size_t>(x)) * 4u;
        // 不透明快路径：当 global_alpha×c.a 仍为全不透明（a==1）时，
        // sRGB↔线性往返是恒等映射（g_gamma_tables.srgb_to_linear / g_gamma_tables.linear_to_srgb
        // 逐值互逆，已验证 0..255 全位级一致），故结果数学上等价于直接写入 sRGB 颜色 c，
        // 跳过 9 次 gamma LUT 往返。
        // 与旧实现位级一致，且对渐变/不透明填充/文字等 opaque 绘制普遍提速。
        if (c.m_a == 255) {
            m_pixels[i + 0] = c.m_r;
            m_pixels[i + 1] = c.m_g;
            m_pixels[i + 2] = c.m_b;
            m_pixels[i + 3] = 255;
            return;
        }
        // coverage == 1.0，直接 source-over 混合（在线性光空间进行，避免 sRGB 空间
        // 线性 alpha 造成的半透明边缘发暗/文字发虚）。
        const float a = c.m_a / 255.0f;
        blend_srgb_over_region(&m_pixels[i], c.m_r, c.m_g, c.m_b, a, a, a, 1);
        return;
    }
    // 慢路径：圆角裁剪（SDF 覆盖度 0..1 抗锯齿）
    if (!m_clip_stack.empty()) {
        float cov = 1.0f;
        for (const ClipRegion &cr : m_clip_stack) {
            cov *= cr.coverage(static_cast<float>(x), static_cast<float>(y));
        }
        if (cov <= 0.0f) {
            return; // 被裁剪区域外（含圆角硬边）
        }
        c.m_a = static_cast<std::uint8_t>(std::max(0.0, std::min(255.0, static_cast<double>(c.m_a) * cov)));
    }
    // 边界检查：圆角/SDF 裁剪路径下 coverage 不直接限制整数坐标范围（如 clip 落在窗口
    // 角落或调用方传入负坐标），务必防止 m_pixels 越界写导致访问违规（0xC0000005）。
    if (x < 0 || y < 0 || x >= m_width || y >= m_height) {
        return;
    }
    const std::size_t i =
        ((static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width)) + static_cast<std::size_t>(x)) * 4u;
    const float a = c.m_a / 255.0f;
    m_pixels[i + 0] = blend_srgb_over(m_pixels[i + 0], c.m_r, a);
    m_pixels[i + 1] = blend_srgb_over(m_pixels[i + 1], c.m_g, a);
    m_pixels[i + 2] = blend_srgb_over(m_pixels[i + 2], c.m_b, a);
    m_pixels[i + 3] = 255;
}

auto Painter::blend_subpixel(int x, int y, Color c, std::uint8_t cr, std::uint8_t cg, std::uint8_t cb) -> void {
    init_gamma_tables();
    if (x < 0 || y < 0 || x >= m_width || y >= m_height) {
        return;
    }
    float cov = 1.0f;
    if (!m_has_rounded_clip) {
        // 快速路径：纯矩形裁剪，coverage 恒为 1 或 0（整数边界检查）
        for (const ClipRegion &cr2 : m_clip_stack) {
            const int cx0 = static_cast<int>(std::ceil(cr2.rect.origin.x));
            const int cy0 = static_cast<int>(std::ceil(cr2.rect.origin.y));
            const int cx1 = static_cast<int>(std::floor(cr2.rect.right())); // 含右/下边界
            const int cy1 = static_cast<int>(std::floor(cr2.rect.bottom()));
            if (x < cx0 || x > cx1 || y < cy0 || y > cy1) {
                return;
            }
        }
        // cov 保持 1.0
    } else if (!m_clip_stack.empty()) {
        // 慢路径：圆角裁剪 SDF 覆盖度
        for (const ClipRegion &cr2 : m_clip_stack) {
            cov *= cr2.coverage(static_cast<float>(x), static_cast<float>(y));
        }
        if (cov <= 0.0f) {
            return;
        }
    }
    cov *= m_global_alpha;
    // 边界检查：同 set_pixel 慢路径，防止圆角/SDF 裁剪下越界写（0xC0000005）。
    if (x < 0 || y < 0 || x >= m_width || y >= m_height) {
        return;
    }
    const float fr = (cr / 255.0f) * cov;
    const float fg = (cg / 255.0f) * cov;
    const float fb = (cb / 255.0f) * cov;
    const std::size_t i =
        ((static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width)) + static_cast<std::size_t>(x)) * 4u;
    blend_srgb_over_region(&m_pixels[i], c.m_r, c.m_g, c.m_b, fr, fg, fb, 1);
}

auto Painter::blend_subpixel_span(int x0, int y, Color c, const std::uint8_t *src, int n, bool lcd,
                                  float src_alpha) // NOLINT(readability-function-cognitive-complexity)
    -> void {
    if (n <= 0) {
        return;
    }
    if (is_recording() || m_has_rounded_clip) {
        // 录制态 / 圆角 SDF 裁剪：逐像素回退（语义不同，不能批量）
        if (lcd) {
            for (int x = 0; x < n; ++x) {
                blend_subpixel(x0 + x, y, c, src[(x * 3) + 0], src[(x * 3) + 1], src[(x * 3) + 2]);
            }
        } else {
            for (int x = 0; x < n; ++x) {
                blend_subpixel(x0 + x, y, c, src[x], src[x], src[x]);
            }
        }
        return;
    }
    init_gamma_tables();
    if (y < 0 || y >= m_height) {
        return;
    }
    // 纯矩形裁剪栈：求 x 区间与全部裁剪矩形 + 屏幕边界的交集（只算一次，不必逐像素遍历）。
    int x_lo = x0;
    int x_hi = x0 + n - 1;
    for (const ClipRegion &cr2 : m_clip_stack) {
        const int cx0 = static_cast<int>(std::ceil(cr2.rect.origin.x));
        const int cx1 = static_cast<int>(std::floor(cr2.rect.right()));
        x_lo = std::max(cx0, x_lo);
        x_hi = std::min(cx1, x_hi);
    }
    x_lo = std::max(x_lo, 0);
    x_hi = std::min(x_hi, m_width - 1);
    if (x_lo > x_hi) {
        return;
    }
    // 源色线性光（整段恒定，逐像素仅查一次，省去每像素 3 次 srgb_to_linear 查表）。
    const float sr_lin[3] = { g_gamma_tables.srgb_to_linear[c.m_r], g_gamma_tables.srgb_to_linear[c.m_g],
                              g_gamma_tables.srgb_to_linear[c.m_b] };
    const float cov = static_cast<float>(m_global_alpha) * src_alpha;
    const std::size_t row_base = static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width);
    for (int x = x_lo; x <= x_hi; ++x) {
        const int k = x - x0;
        const std::uint8_t cr = lcd ? src[(k * 3) + 0] : src[k];
        const std::uint8_t cg = lcd ? src[(k * 3) + 1] : src[k];
        const std::uint8_t cb = lcd ? src[(k * 3) + 2] : src[k];
        const float fr = (cr / 255.0f) * cov;
        const float fg = (cg / 255.0f) * cov;
        const float fb = (cb / 255.0f) * cov;
        if (fr <= 0.0f && fg <= 0.0f && fb <= 0.0f) {
            continue; // 零覆盖像素跳过（等同原 blend_subpixel 的 continue）
        }
        const std::size_t i = (row_base + static_cast<std::size_t>(x)) * 4u;
        std::uint8_t *p = &m_pixels[i];
        // gamma-correct source-over（逐位等价于 blend_srgb_over），已验证与旧实现位级一致。
        p[0] = linear_to_srgb((sr_lin[0] * fr) + (g_gamma_tables.srgb_to_linear[p[0]] * (1.0f - fr)));
        p[1] = linear_to_srgb((sr_lin[1] * fg) + (g_gamma_tables.srgb_to_linear[p[1]] * (1.0f - fg)));
        p[2] = linear_to_srgb((sr_lin[2] * fb) + (g_gamma_tables.srgb_to_linear[p[2]] * (1.0f - fb)));
        p[3] = 255;
    }
}

auto Painter::set_alpha(double a) -> void {
    if (is_recording()) {
        DrawCmd cmd;
        cmd.kind = CmdKind::SetAlpha;
        cmd.alpha = (a < 0.0) ? 0.0 : (a > 1.0 ? 1.0 : a);
        m_recording_stack.back()->push_cmd(cmd);
        return;
    }
    m_global_alpha = (a < 0.0) ? 0.0 : (a > 1.0 ? 1.0 : a);
}

auto Painter::composite(const Painter &src, const Matrix2D &matrix) -> void {
    if (is_recording()) {
        DrawCmd cmd;
        cmd.kind = CmdKind::Composite;
        cmd.matrix_idx = m_recording_stack.back()->add_matrix(matrix);
        cmd.composite_scale = src.m_scale;
        cmd.image_idx = m_recording_stack.back()->add_image(src.to_image());
        m_recording_stack.back()->push_cmd(cmd);
        return;
    }
    if (m_width <= 0 || m_height <= 0 || src.width() <= 0 || src.height() <= 0) {
        return;
    }
    composite_pixels(src.m_pixels.data(), src.width(), src.height(), src.m_scale, matrix);
}

auto Painter::composite(const Image &src, const Matrix2D &matrix, float src_scale) -> void {
    // 录制态：缓存 DL（含 cache_layer 位图 / 像素缓存）回放进祖先录制时，必须把该合成
    // 重新录为 Composite 命令而非就地执行——否则祖先 DL 不含本合成，回放时子树像素缺失
    // （GridView 的 AppCell 像素缓存表现为单元格整体透明）。
    if (is_recording()) {
        DrawCmd cmd;
        cmd.kind = CmdKind::Composite;
        cmd.matrix_idx = m_recording_stack.back()->add_matrix(matrix);
        cmd.composite_scale = src_scale;
        cmd.image_idx = m_recording_stack.back()->add_image(src);
        m_recording_stack.back()->push_cmd(cmd);
        return;
    }
    if (m_width <= 0 || m_height <= 0 || src.width <= 0 || src.height <= 0) {
        return;
    }
    // 同 draw_image：composite_pixels 按 width/height 索引裸指针，
    // 故须先确认缓冲确实覆盖 width*height*4，否则声明尺寸虚高的 Image 会导致越界读。
    if (static_cast<std::uint64_t>(src.pixels.size()) <
        static_cast<std::uint64_t>(src.width) * static_cast<std::uint64_t>(src.height) * 4u) {
        return;
    }
    composite_pixels(src.pixels.data(), src.width, src.height, src_scale, matrix);
}

auto Painter::to_image() const -> Image {
    Image img;
    img.width = m_width;
    img.height = m_height;
    img.pixels = m_pixels; // 拷贝设备像素缓冲
    return img;
}

auto Painter::composite_pixels(const std::uint8_t *spix, int sw, int sh, float sscale, const Matrix2D &matrix)
    -> void { // NOLINT(readability-function-cognitive-complexity)
    detail::PaintTimer guard{ &g_pt.composite };
    const float lw = static_cast<float>(sw) / sscale;
    const float lh = static_cast<float>(sh) / sscale;

    // 源内容四角在逻辑空间的包围盒（物理像素），用于限定遍历范围。
    const Point corners[4] = { Point{ .x = 0, .y = 0 }, Point{ .x = lw, .y = 0 }, Point{ .x = 0, .y = lh },
                               Point{ .x = lw, .y = lh } };
    int minx = m_width;
    int miny = m_height;
    int maxx = 0;
    int maxy = 0;
    for (auto corner : corners) {
        Point p = matrix.apply_to_point(corner);
        p.x *= m_scale;
        p.y *= m_scale;
        minx = std::min(minx, static_cast<int>(std::floor(p.x)));
        miny = std::min(miny, static_cast<int>(std::floor(p.y)));
        maxx = std::max(maxx, static_cast<int>(std::ceil(p.x)));
        maxy = std::max(maxy, static_cast<int>(std::ceil(p.y)));
    }
    minx = std::max(0, minx);
    miny = std::max(0, miny);
    maxx = std::min(m_width - 1, maxx);
    maxy = std::min(m_height - 1, maxy);
    {
        // 收缩进裁剪交集（半开→闭区间适配）：裁剪外像素 set_pixel 恒丢弃。
        int cx1 = maxx + 1;
        int cy1 = maxy + 1;
        if (!shrink_to_clips(minx, miny, cx1, cy1)) {
            return;
        }
        maxx = cx1 - 1;
        maxy = cy1 - 1;
    }
    if (maxx < minx || maxy < miny) {
        return;
    }

    const Matrix2D inv = matrix.inverse();

    // ---- 纯平移 + 同 scale 快路径 ----
    // 纯平移下逆变换可分离（lp.x 只依赖 x、lp.y 只依赖 y），故把慢路径的逐像素矩阵求逆
    // 预计算为两张一维映射表——表项用与慢路径**逐字相同**的浮点表达式求得，故结果逐位一致
    // （无需假设 (x+0.5)/s*s 往返无舍入）。内层随之退化为「查表 + 连续内存扫描」。
    // 滚动容器每帧把离屏缓冲 blit 到窗口即走此路径（视口级 composite 是 60fps 预算大头）。
    // 前置条件：范围已收缩进裁剪交集（shrink_to_clips 与 set_pixel 的矩形裁剪判据逐字一致，
    // 故区间内像素必然通过裁剪）；圆角裁剪按 SDF 覆盖度加权、global_alpha<1 需按像素乘 alpha，
    // 二者 set_pixel 另有语义，保守回退慢路径。
    if (matrix.m11 == 1.0f && matrix.m12 == 0.0f && matrix.m21 == 0.0f && matrix.m22 == 1.0f && sscale == m_scale &&
        !m_has_rounded_clip && m_global_alpha == 1.0) {
        init_gamma_tables(); // set_pixel 每次调用前置；快路径直接混合，须自行确保 LUT 就绪
        // 单线程 UI 每帧复用，避免逐帧分配（表长 = 目标遍历宽/高，量级为视口尺寸）。
        static thread_local std::vector<int> sx_map;
        static thread_local std::vector<int> sy_map;
        sx_map.resize(static_cast<std::size_t>(maxx) - static_cast<std::size_t>(minx) + 1u);
        sy_map.resize(static_cast<std::size_t>(maxy) - static_cast<std::size_t>(miny) + 1u);
        for (int x = minx; x <= maxx; ++x) {
            const Point lp = inv.apply_to_point(Point{ .x = (x + 0.5f) / m_scale, .y = 0.0f });
            sx_map[static_cast<std::size_t>(x - minx)] =
                (lp.x >= 0.0f && lp.x < lw) ? static_cast<int>(std::floor(lp.x * sscale)) : -1;
        }
        for (int y = miny; y <= maxy; ++y) {
            const Point lp = inv.apply_to_point(Point{ .x = 0.0f, .y = (y + 0.5f) / m_scale });
            sy_map[static_cast<std::size_t>(y - miny)] =
                (lp.y >= 0.0f && lp.y < lh) ? static_cast<int>(std::floor(lp.y * sscale)) : -1;
        }
        // 有效列为连续区间：先收缩掉两端越界列，省去内层的逐像素判断。
        int vx0 = minx;
        int vx1 = maxx;
        while (vx0 <= vx1 && sx_map[static_cast<std::size_t>(vx0 - minx)] < 0) {
            ++vx0;
        }
        while (vx1 >= vx0 && sx_map[static_cast<std::size_t>(vx1 - minx)] < 0) {
            --vx1;
        }
        const std::size_t dst_stride = static_cast<std::size_t>(m_width) * 4u;
        const std::size_t src_stride = static_cast<std::size_t>(sw) * 4u;
        for (int y = miny; y <= maxy; ++y) {
            const int sy = sy_map[static_cast<std::size_t>(y - miny)];
            if (sy < 0) {
                continue;
            }
            const std::uint8_t *srow = spix + (static_cast<std::size_t>(sy) * src_stride);
            std::uint8_t *drow = m_pixels.data() + (static_cast<std::size_t>(y) * dst_stride);
            for (int x = vx0; x <= vx1; ++x) {
                const int sx = sx_map[static_cast<std::size_t>(x - minx)];
                if (sx < 0) {
                    continue;
                }
                const std::uint8_t *sp = srow + (static_cast<std::size_t>(sx) * 4u);
                const std::uint8_t sa = sp[3];
                if (sa == 0) {
                    continue; // 与慢路径的 c.a > 0 判据一致（全透明源不写目标）
                }
                std::uint8_t *dp = drow + (static_cast<std::size_t>(x) * 4u);
                if (sa == 255) {
                    // 与 set_pixel 的不透明快路径一致：sRGB↔线性往返恒等，直接覆写。
                    dp[0] = sp[0];
                    dp[1] = sp[1];
                    dp[2] = sp[2];
                    dp[3] = 255;
                    continue;
                }
                const float a = static_cast<float>(sa) / 255.0f;
                blend_srgb_over_region(dp, sp[0], sp[1], sp[2], a, a, a, 1);
            }
        }
        return;
    }

    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            const Point phys{ .x = (x + 0.5f) / m_scale, .y = (y + 0.5f) / m_scale };
            const Point lp = inv.apply_to_point(phys);
            if (lp.x >= 0.0f && lp.x < lw && lp.y >= 0.0f && lp.y < lh) {
                const int sx = static_cast<int>(std::floor(lp.x * sscale));
                const int sy = static_cast<int>(std::floor(lp.y * sscale));
                const std::size_t si = ((static_cast<std::size_t>(sy) * sw) + sx) * 4u;
                const Color c{ spix[si + 0], spix[si + 1], spix[si + 2], spix[si + 3] };
                if (c.m_a > 0) {
                    set_pixel(x, y, c); // 经裁剪栈 + global_alpha（透明度统一生效）
                }
            }
        }
    }
}

// ---------- 渐变绘制 ----------

namespace {
/// 在色标数组中按 t∈[0,1] 线性插值 RGBA。
inline auto sample_gradient(const std::vector<Color> &colors, const std::vector<float> &stops, float t) -> Color {
    if (colors.empty()) {
        return Color{};
    }
    if (colors.size() == 1 || t <= 0.0f) {
        return colors.front();
    }
    if (t >= 1.0f) {
        return colors.back();
    }
    // 找到 t 所在的区间。colors/stops 由调用方传入，长度可能不一致（如反序列化的
    // DisplayList 或用户直接调用），故索引上界取两者较小值，避免 colors[i+1] 越界读。
    const std::size_t n = std::min(colors.size(), stops.size());
    for (std::size_t i = 0; i + 1 < n; ++i) {
        if (t >= stops[i] && t <= stops[i + 1]) {
            const float range = stops[i + 1] - stops[i];
            const float frac = (range > 0.0f) ? (t - stops[i]) / range : 0.0f;
            const Color &a = colors[i];
            const Color &b = colors[i + 1];
            return Color{
                static_cast<std::uint8_t>(a.m_r + ((b.m_r - a.m_r) * frac)),
                static_cast<std::uint8_t>(a.m_g + ((b.m_g - a.m_g) * frac)),
                static_cast<std::uint8_t>(a.m_b + ((b.m_b - a.m_b) * frac)),
                static_cast<std::uint8_t>(a.m_a + ((b.m_a - a.m_a) * frac)),
            };
        }
    }
    return colors.back();
}
} // namespace

auto Painter::draw_linear_gradient(
    const Rect &area, Point start, Point end,
    const std::vector<Color> &colors, // NOLINT(readability-function-cognitive-complexity)
    const std::vector<float> &stops) -> void {
    if (is_recording()) {
        DrawCmd cmd;
        cmd.kind = CmdKind::LinearGradient;
        cmd.bounds = area;
        cmd.pt0 = start;
        cmd.pt1 = end;
        cmd.col_idx = m_recording_stack.back()->add_colors(colors);
        cmd.flt_idx = m_recording_stack.back()->add_floats(stops);
        m_recording_stack.back()->push_cmd(cmd);
        return;
    }
    detail::PaintTimer guard{ &g_pt.gradient };
    if (colors.empty() || stops.empty()) {
        return;
    }
    const Rect pr = scale_rect(area, m_scale);
    int x0 = static_cast<int>(std::floor(std::max(0.0f, pr.origin.x)));
    int y0 = static_cast<int>(std::floor(std::max(0.0f, pr.origin.y)));
    int x1 = static_cast<int>(std::ceil(std::min(static_cast<float>(m_width), pr.origin.x + pr.size.width)));
    int y1 = static_cast<int>(std::ceil(std::min(static_cast<float>(m_height), pr.origin.y + pr.size.height)));
    if (!shrink_to_clips(x0, y0, x1, y1)) {
        return; // 裁剪外纯白扫
    }
    AURORA_PROFILE_COUNT(draw_calls, 1);
    AURORA_PROFILE_COUNT(pixels_filled, static_cast<std::uint64_t>(std::max(0, x1 - x0)) *
                                            static_cast<std::uint64_t>(std::max(0, y1 - y0)));

    // 渐变方向向量（物理像素空间）
    const float sx = start.x * m_scale;
    const float sy = start.y * m_scale;
    const float ex = end.x * m_scale;
    const float ey = end.y * m_scale;
    float dx = ex - sx;
    float dy = ey - sy;
    const float len_sq = (dx * dx) + (dy * dy);
    if (len_sq < 0.001f) {
        // 退化：方向为零，用首色填充
        fill_rect(area, colors.front());
        return;
    }
    const float inv_len_sq = 1.0f / len_sq;

    const bool fast = !m_has_rounded_clip && m_clip_stack.empty();
    // SIMD 扫描线快路径：仅双色标 + 全不透明（a=255）且无任何裁剪时启用，与标量黄金逐位一致；
    // 其余回落「圆角/矩形裁剪感知」优化路径（见下），再不行才逐像素 set_pixel。
    const bool simd_grad =
        fast && colors.size() == 2 && stops.size() == 2 && colors[0].m_a == 255 && colors[1].m_a == 255;
    if (simd_grad) {
        for (int y = y0; y < y1; ++y) {
            std::uint8_t *row =
                m_pixels.data() +
                (((static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width)) + static_cast<std::size_t>(x0)) *
                 4u);
            const int w = x1 - x0;
            const std::uint8_t g0[4] = { colors[0].m_r, colors[0].m_g, colors[0].m_b, 255 };
            const std::uint8_t g1[4] = { colors[1].m_r, colors[1].m_g, colors[1].m_b, 255 };
            const float py = (static_cast<float>(y) - sy) * dy;
            gradient_linear_fill(row, x0, w, sx, py, dx, dy, inv_len_sq, g0, g1, stops[0], stops[1] - stops[0]);
        }
        return;
    }

    // 圆角/矩形裁剪感知优化路径（仿 fill_rect_slow_path）：预收集全部圆角裁剪的 coverage=1 阈值；
    // 逐行用 rounded_full_x_range 求全覆写 x 区间（多圆角裁剪取交集），区间内 coverage 恒为 1，
    // 直接 source-over（与 set_pixel 位级一致，跳过逐像素裁剪栈遍历 + SDF）；仅四角圆弧过渡带
    // 走 set_pixel 逐像素 coverage。banner 卡片渐变（圆角裁剪 + 每帧重绘）此前全走慢路径，此优化为主因。
    struct GradRoundedInfo {
        const ClipRegion *cr;
        float threshold;
    };
    std::vector<GradRoundedInfo> rounded_clips;
    for (const ClipRegion &cr : m_clip_stack) {
        if (cr.rounded && cr.radius > 0.0f) {
            rounded_clips.push_back({ .cr = &cr, .threshold = cr.anti_alias ? -0.5f : 0.0f });
        }
    }
    const auto ga = static_cast<float>(m_global_alpha); // 与 set_pixel 一致的全局透明度
    for (int y = y0; y < y1; ++y) {
        std::uint8_t *row =
            m_pixels.data() +
            (((static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width)) + static_cast<std::size_t>(x0)) * 4u);
        // 本行全覆写 x 区间 = 所有圆角裁剪各行全覆写范围的交集（无圆角裁剪时即 [x0,x1)）。
        int safe_x0 = x0;
        int safe_x1 = x1;
        for (const auto &ri : rounded_clips) {
            const auto [fx0, fx1] = rounded_full_x_range(*ri.cr, y, ri.threshold);
            safe_x0 = std::max(safe_x0, fx0);
            safe_x1 = std::min(safe_x1, fx1);
        }
        if (safe_x0 >= safe_x1) {
            safe_x0 = safe_x1 = x0; // 退化：整行走逐像素 set_pixel
        }
        // 左过渡区（圆弧）：逐像素 coverage（set_pixel 含裁剪 + gamma）
        for (int x = x0; x < safe_x0; ++x) {
            float t = (((static_cast<float>(x) - sx) * dx) + ((static_cast<float>(y) - sy) * dy)) * inv_len_sq;
            t = std::max(0.0f, std::min(1.0f, t));
            set_pixel(x, y, sample_gradient(colors, stops, t));
        }
        // 行内全覆写区：coverage 恒为 1，直接 source-over（与 set_pixel 位级一致）
        if (safe_x0 < safe_x1) {
            float t = (((static_cast<float>(safe_x0) - sx) * dx) + ((static_cast<float>(y) - sy) * dy)) * inv_len_sq;
            const float t_step = dx * inv_len_sq; // 沿 +x 每像素 t 增量（t 沿 x 线性）
            for (int x = safe_x0; x < safe_x1; ++x) {
                t = std::max(0.0f, std::min(1.0f, t));
                const Color c = sample_gradient(colors, stops, t);
                t += t_step;
                std::uint8_t *p = row + (static_cast<std::size_t>(x - x0) * 4u);
                // 与 set_pixel 一致：先按全局透明度折算 alpha，再判不透明快路径 / gamma 混合。
                const int ca = static_cast<int>(std::lround(static_cast<double>(c.m_a) * ga));
                const int ca_c = ca > 255 ? 255 : ca;
                if (ca_c >= 255) {
                    p[0] = c.m_r;
                    p[1] = c.m_g;
                    p[2] = c.m_b;
                    p[3] = 255;
                } else if (ca_c <= 0) {
                    continue;
                } else {
                    const float a = static_cast<float>(ca_c) / 255.0f;
                    blend_srgb_over_region(p, c.m_r, c.m_g, c.m_b, a, a, a, 1);
                }
            }
        }
        // 右过渡区（圆弧）：逐像素 coverage
        for (int x = safe_x1; x < x1; ++x) {
            float t = (((static_cast<float>(x) - sx) * dx) + ((static_cast<float>(y) - sy) * dy)) * inv_len_sq;
            t = std::max(0.0f, std::min(1.0f, t));
            set_pixel(x, y, sample_gradient(colors, stops, t));
        }
    }
}

auto Painter::draw_radial_gradient(
    const Rect &area, Point center, float radius,
    const std::vector<Color> &colors, // NOLINT(readability-function-cognitive-complexity)
    const std::vector<float> &stops) -> void {
    if (is_recording()) {
        DrawCmd cmd;
        cmd.kind = CmdKind::RadialGradient;
        cmd.bounds = area;
        cmd.pt0 = center;
        cmd.f0 = radius;
        cmd.col_idx = m_recording_stack.back()->add_colors(colors);
        cmd.flt_idx = m_recording_stack.back()->add_floats(stops);
        m_recording_stack.back()->push_cmd(cmd);
        return;
    }
    detail::PaintTimer guard{ &g_pt.gradient };
    if (colors.empty() || stops.empty() || radius <= 0.0f) {
        return;
    }
    const Rect pr = scale_rect(area, m_scale);
    int x0 = static_cast<int>(std::floor(std::max(0.0f, pr.origin.x)));
    int y0 = static_cast<int>(std::floor(std::max(0.0f, pr.origin.y)));
    int x1 = static_cast<int>(std::ceil(std::min(static_cast<float>(m_width), pr.origin.x + pr.size.width)));
    int y1 = static_cast<int>(std::ceil(std::min(static_cast<float>(m_height), pr.origin.y + pr.size.height)));
    if (!shrink_to_clips(x0, y0, x1, y1)) {
        return; // 裁剪外纯白扫
    }
    AURORA_PROFILE_COUNT(draw_calls, 1);
    AURORA_PROFILE_COUNT(pixels_filled, static_cast<std::uint64_t>(std::max(0, x1 - x0)) *
                                            static_cast<std::uint64_t>(std::max(0, y1 - y0)));

    const float cx = center.x * m_scale;
    const float cy = center.y * m_scale;
    const float r = radius * m_scale;
    const float inv_r = 1.0f / r;

    const bool fast = !m_has_rounded_clip && m_clip_stack.empty();
    // SIMD 扫描线快路径：仅双色标 + 全不透明（a=255）且无任何裁剪时启用，与标量黄金逐位一致。
    const bool simd_grad =
        fast && colors.size() == 2 && stops.size() == 2 && colors[0].m_a == 255 && colors[1].m_a == 255;
    if (simd_grad) {
        for (int y = y0; y < y1; ++y) {
            std::uint8_t *row =
                m_pixels.data() +
                (((static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width)) + static_cast<std::size_t>(x0)) *
                 4u);
            const int w = x1 - x0;
            const std::uint8_t g0[4] = { colors[0].m_r, colors[0].m_g, colors[0].m_b, 255 };
            const std::uint8_t g1[4] = { colors[1].m_r, colors[1].m_g, colors[1].m_b, 255 };
            const float py = (static_cast<float>(y) - cy) * (static_cast<float>(y) - cy); // (y - cy)^2
            gradient_radial_fill(row, x0, w, cx, py, inv_r, g0, g1, stops[0], stops[1] - stops[0]);
        }
        return;
    }

    // 圆角/矩形裁剪感知优化路径（同 draw_linear_gradient）：逐行求全覆写 x 区间，区间内直接
    // source-over，仅四角圆弧过渡带走 set_pixel。圆角裁剪下的径向渐变（如圆形光晕）同样受益。
    struct GradRoundedInfo {
        const ClipRegion *cr;
        float threshold;
    };
    std::vector<GradRoundedInfo> rounded_clips;
    for (const ClipRegion &cr : m_clip_stack) {
        if (cr.rounded && cr.radius > 0.0f) {
            rounded_clips.push_back({ .cr = &cr, .threshold = cr.anti_alias ? -0.5f : 0.0f });
        }
    }
    const auto ga = static_cast<float>(m_global_alpha); // 与 set_pixel 一致的全局透明度
    for (int y = y0; y < y1; ++y) {
        std::uint8_t *row =
            m_pixels.data() +
            (((static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width)) + static_cast<std::size_t>(x0)) * 4u);
        int safe_x0 = x0;
        int safe_x1 = x1;
        for (const auto &ri : rounded_clips) {
            const auto [fx0, fx1] = rounded_full_x_range(*ri.cr, y, ri.threshold);
            safe_x0 = std::max(safe_x0, fx0);
            safe_x1 = std::min(safe_x1, fx1);
        }
        if (safe_x0 >= safe_x1) {
            safe_x0 = safe_x1 = x0;
        }
        // 左过渡区（圆弧）
        for (int x = x0; x < safe_x0; ++x) {
            const float px = static_cast<float>(x) - cx;
            const float py = static_cast<float>(y) - cy;
            const float dist = std::sqrt((px * px) + (py * py));
            float t = std::max(0.0f, std::min(1.0f, dist * inv_r));
            set_pixel(x, y, sample_gradient(colors, stops, t));
        }
        // 行内全覆写区
        if (safe_x0 < safe_x1) {
            const float py = static_cast<float>(y) - cy;
            for (int x = safe_x0; x < safe_x1; ++x) {
                const float px = static_cast<float>(x) - cx;
                const float dist = std::sqrt((px * px) + (py * py));
                float t = std::max(0.0f, std::min(1.0f, dist * inv_r));
                const Color c = sample_gradient(colors, stops, t);
                std::uint8_t *p = row + (static_cast<std::size_t>(x - x0) * 4u);
                const int ca = static_cast<int>(std::lround(static_cast<double>(c.m_a) * ga));
                const int ca_c = ca > 255 ? 255 : ca;
                if (ca_c >= 255) {
                    p[0] = c.m_r;
                    p[1] = c.m_g;
                    p[2] = c.m_b;
                    p[3] = 255;
                } else if (ca_c <= 0) {
                    continue;
                } else {
                    const float a = static_cast<float>(ca_c) / 255.0f;
                    blend_srgb_over_region(p, c.m_r, c.m_g, c.m_b, a, a, a, 1);
                }
            }
        }
        // 右过渡区（圆弧）
        for (int x = safe_x1; x < x1; ++x) {
            const float px = static_cast<float>(x) - cx;
            const float py = static_cast<float>(y) - cy;
            const float dist = std::sqrt((px * px) + (py * py));
            float t = std::max(0.0f, std::min(1.0f, dist * inv_r));
            set_pixel(x, y, sample_gradient(colors, stops, t));
        }
    }
}

auto Painter::draw_shadow(const Rect &shape, float offset_x, float offset_y, float blur_radius, Color color)
    -> void { // NOLINT(readability-function-cognitive-complexity)
    if (is_recording()) {
        DrawCmd cmd;
        cmd.kind = CmdKind::Shadow;
        cmd.bounds = shape;
        cmd.f0 = offset_x;
        cmd.f1 = offset_y;
        cmd.f2 = blur_radius;
        cmd.color = color;
        m_recording_stack.back()->push_cmd(cmd);
        return;
    }
    // 阴影矩形（偏移后）
    const Rect shadow_rect{ .origin = Point{ .x = shape.origin.x + offset_x, .y = shape.origin.y + offset_y },
                            .size = shape.size };

    if (blur_radius <= 0.0f) {
        // 硬边阴影
        fill_rect(shadow_rect, color);
        return;
    }

    // 模糊阴影：扩展区域并逐像素计算覆盖度（简化高斯：距离衰减）
    const float expand = blur_radius * 2.0f;
    const Rect expanded{ .origin = Point{ .x = shadow_rect.origin.x - expand, .y = shadow_rect.origin.y - expand },
                         .size = Size{ .width = shadow_rect.size.width + (expand * 2.0f),
                                       .height = shadow_rect.size.height + (expand * 2.0f) } };

    const Rect pr = scale_rect(expanded, m_scale);
    int x0 = static_cast<int>(std::floor(std::max(0.0f, pr.origin.x)));
    int y0 = static_cast<int>(std::floor(std::max(0.0f, pr.origin.y)));
    int x1 = static_cast<int>(std::ceil(std::min(static_cast<float>(m_width), pr.origin.x + pr.size.width)));
    int y1 = static_cast<int>(std::ceil(std::min(static_cast<float>(m_height), pr.origin.y + pr.size.height)));
    if (!shrink_to_clips(x0, y0, x1, y1)) {
        return; // 裁剪外纯白扫（blend_pixel 经 set_pixel 恒丢弃）
    }
    AURORA_PROFILE_COUNT(draw_calls, 1);

    // 阴影矩形（物理像素）
    const Rect spr = scale_rect(shadow_rect, m_scale);
    const float blur_px = blur_radius * m_scale;
    const float inv_blur = 1.0f / (blur_px > 0.0f ? blur_px : 1.0f);

    // 性能优化：阴影内部（矩形内）衰减因子恒为 1，等效于整块 fill_rect——直接走半透明
    // source-over 快路径（无逐像素 sqrt / 裁剪栈遍历），大幅降低大面积阴影（GridView
    // 多单元格 / banner 卡片）的重栅开销。仅对「扩展区 − 内部矩形」的边缘环做距离衰减羽化。
    fill_rect(shadow_rect, color);

    detail::PaintTimer guard{ &g_pt.shadow };
    // 内部矩形的物理像素边界取与 fill_rect 一致的 ceil/floor（含右/下边界），
    // 使边缘环恰好从 fill 之外开始，不重不漏。
    const int si0 = static_cast<int>(std::ceil(spr.origin.x));
    const int sj0 = static_cast<int>(std::ceil(spr.origin.y));
    const int si1 = static_cast<int>(std::floor(spr.origin.x + spr.size.width));
    const int sj1 = static_cast<int>(std::floor(spr.origin.y + spr.size.height));

    for (int y = y0; y < y1; ++y) {
        const auto fy = static_cast<float>(y);
        const bool y_in = (y >= sj0 && y <= sj1);
        for (int x = x0; x < x1; ++x) {
            // 内部已由 fill_rect 填充，跳过（避免对每像素重复 sqrt + 混合）
            if (y_in && x >= si0 && x <= si1) {
                continue;
            }
            // 计算到阴影矩形边缘的距离（内部为负，外部为正）
            const auto fx = static_cast<float>(x);
            float dx = 0.0f;
            float dy = 0.0f;
            if (fx < spr.origin.x) {
                dx = spr.origin.x - fx;
            } else if (fx > spr.origin.x + spr.size.width) {
                dx = fx - (spr.origin.x + spr.size.width);
            }
            if (fy < spr.origin.y) {
                dy = spr.origin.y - fy;
            } else if (fy > spr.origin.y + spr.size.height) {
                dy = fy - (spr.origin.y + spr.size.height);
            }

            const float dist = std::sqrt((dx * dx) + (dy * dy));
            // 衰减因子：内部=1，外部按距离线性衰减到0
            float alpha_factor = 1.0f;
            if (dist > 0.0f) {
                alpha_factor = std::max(0.0f, 1.0f - (dist * inv_blur));
            }

            if (alpha_factor > 0.0f) {
                Color c = color;
                c.m_a = static_cast<std::uint8_t>(color.m_a * alpha_factor);
                if (c.m_a > 0) {
                    blend_pixel(x, y, c);
                }
            }
        }
    }
}

auto Painter::blur_region(const Rect &region, float radius) -> void {
    if (is_recording()) {
        DrawCmd cmd;
        cmd.kind = CmdKind::BlurRegion;
        cmd.bounds = region;
        cmd.f0 = radius;
        m_recording_stack.back()->push_cmd(cmd);
        return;
    }
    detail::PaintTimer guard{ &g_pt.blur };
    if (radius <= 0.0f || m_width <= 0 || m_height <= 0) {
        return;
    }
    // 物理像素区域与模糊半径
    const Rect pr = scale_rect(region, m_scale);
    const int x0 = std::max(0, static_cast<int>(std::floor(pr.origin.x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(pr.origin.y)));
    const int x1 = std::min(m_width, static_cast<int>(std::ceil(pr.origin.x + pr.size.width)));
    const int y1 = std::min(m_height, static_cast<int>(std::ceil(pr.origin.y + pr.size.height)));
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    AURORA_PROFILE_COUNT(draw_calls, 1);
    AURORA_PROFILE_COUNT(pixels_filled, static_cast<std::uint64_t>(x1 - x0) * static_cast<std::uint64_t>(y1 - y0));
    const int r = std::max(1, static_cast<int>(radius * m_scale));
    const int rw = x1 - x0;
    const int rh = y1 - y0;

    // 分离式两遍 box blur（水平 → 垂直），边缘采样钳制到区域内（不漏采区外像素，
    // 毛玻璃语义下区域外背景不参与）。临时缓冲避免读写串扰。
    // SIMD 双实现：标量黄金 / SSE2 / AVX2 逐位一致，分发由 g_simd_level 决定。
    aurora::detail::blur_region(m_pixels.data(), m_width, x0, y0, rw, rh, r);
}

auto Painter::blend_region(const Rect &region, BlendMode mode, Color tint, float strength) -> void {
    if (is_recording()) {
        DrawCmd cmd;
        cmd.kind = CmdKind::BlendRegion;
        cmd.bounds = region;
        cmd.blend_mode = mode;
        cmd.color = tint;
        cmd.f0 = strength;
        m_recording_stack.back()->push_cmd(cmd);
        return;
    }
    detail::PaintTimer guard{ &g_pt.region };
    if (m_width <= 0 || m_height <= 0) {
        return;
    }
    strength = aurora::saturate(strength);
    if (strength <= 0.0f) {
        return;
    }
    const Rect pr = scale_rect(region, m_scale);
    const int x0 = std::max(0, static_cast<int>(std::floor(pr.origin.x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(pr.origin.y)));
    const int x1 = std::min(m_width, static_cast<int>(std::ceil(pr.origin.x + pr.size.width)));
    const int y1 = std::min(m_height, static_cast<int>(std::ceil(pr.origin.y + pr.size.height)));
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    AURORA_PROFILE_COUNT(draw_calls, 1);
    AURORA_PROFILE_COUNT(pixels_filled, static_cast<std::uint64_t>(x1 - x0) * static_cast<std::uint64_t>(y1 - y0));
    const int tr = tint.m_r;
    const int tg = tint.m_g;
    const int tb = tint.m_b;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t i = ((static_cast<std::size_t>(y) * m_width) + x) * 4;
            const int sr = m_pixels[i];
            const int sg = m_pixels[i + 1];
            const int sb = m_pixels[i + 2];
            int r = 0;
            int g = 0;
            int b = 0;
            switch (mode) {
            case BlendMode::Normal:
                r = tr;
                g = tg;
                b = tb;
                break;
            case BlendMode::Multiply:
                r = sr * tr / 255;
                g = sg * tg / 255;
                b = sb * tb / 255;
                break;
            case BlendMode::Screen:
                r = 255 - ((255 - sr) * (255 - tr) / 255);
                g = 255 - ((255 - sg) * (255 - tg) / 255);
                b = 255 - ((255 - sb) * (255 - tb) / 255);
                break;
            case BlendMode::Overlay:
                r = sr < 128 ? 2 * sr * tr / 255 : 255 - (2 * (255 - sr) * (255 - tr) / 255);
                g = sg < 128 ? 2 * sg * tg / 255 : 255 - (2 * (255 - sg) * (255 - tg) / 255);
                b = sb < 128 ? 2 * sb * tb / 255 : 255 - (2 * (255 - sb) * (255 - tb) / 255);
                break;
            case BlendMode::Darken:
                r = std::min(sr, tr);
                g = std::min(sg, tg);
                b = std::min(sb, tb);
                break;
            case BlendMode::Lighten:
                r = std::max(sr, tr);
                g = std::max(sg, tg);
                b = std::max(sb, tb);
                break;
            case BlendMode::Difference:
                r = std::abs(sr - tr);
                g = std::abs(sg - tg);
                b = std::abs(sb - tb);
                break;
            case BlendMode::Exclusion:
                r = sr + tr - (2 * sr * tr / 255);
                g = sg + tg - (2 * sg * tg / 255);
                b = sb + tb - (2 * sb * tb / 255);
                break;
            default:
                r = sr;
                g = sg;
                b = sb;
                break;
            }
            const int fr = sr + static_cast<int>(strength * (r - sr));
            const int fg = sg + static_cast<int>(strength * (g - sg));
            const int fb = sb + static_cast<int>(strength * (b - sb));
            m_pixels[i] = aurora::saturate_u8(fr);
            m_pixels[i + 1] = aurora::saturate_u8(fg);
            m_pixels[i + 2] = aurora::saturate_u8(fb);
        }
    }
}

auto Painter::mask_region(const Rect &region, ShaderMaskKind kind, float strength) -> void {
    if (is_recording()) {
        DrawCmd cmd;
        cmd.kind = CmdKind::MaskRegion;
        cmd.bounds = region;
        cmd.mask_kind = kind;
        cmd.f0 = strength;
        m_recording_stack.back()->push_cmd(cmd);
        return;
    }
    detail::PaintTimer guard{ &g_pt.region };
    if (m_width <= 0 || m_height <= 0) {
        return;
    }
    strength = aurora::saturate(strength);
    if (strength <= 0.0f) {
        return;
    }
    const Rect pr = scale_rect(region, m_scale);
    const int x0 = std::max(0, static_cast<int>(std::floor(pr.origin.x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(pr.origin.y)));
    const int x1 = std::min(m_width, static_cast<int>(std::ceil(pr.origin.x + pr.size.width)));
    const int y1 = std::min(m_height, static_cast<int>(std::ceil(pr.origin.y + pr.size.height)));
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    AURORA_PROFILE_COUNT(draw_calls, 1);
    AURORA_PROFILE_COUNT(pixels_filled, static_cast<std::uint64_t>(x1 - x0) * static_cast<std::uint64_t>(y1 - y0));
    const int rw = x1 - x0;
    const int rh = y1 - y0;
    const float cx = rw * 0.5f;
    const float cy = rh * 0.5f;
    const float max_r = std::sqrt((cx * cx) + (cy * cy)) + 1e-3f;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float fx = static_cast<float>(x - x0);
            const float fy = static_cast<float>(y - y0);
            float base = NAN;
            switch (kind) {
            case ShaderMaskKind::LinearFade: base = 1.0f - (fy / rh); break;
            case ShaderMaskKind::LinearRise: base = fy / rh; break;
            case ShaderMaskKind::RadialFade: {
                const float dx = fx - cx;
                const float dy = fy - cy;
                base = 1.0f - (std::sqrt((dx * dx) + (dy * dy)) / max_r);
                break;
            }
            default: base = 1.0f; break;
            }
            base = aurora::saturate(base);
            const float factor = 1.0f - (strength * (1.0f - base));
            const std::size_t i = ((static_cast<std::size_t>(y) * m_width) + x) * 4;
            m_pixels[i] = aurora::saturate_u8(static_cast<int>(m_pixels[i] * factor));
            m_pixels[i + 1] = aurora::saturate_u8(static_cast<int>(m_pixels[i + 1] * factor));
            m_pixels[i + 2] = aurora::saturate_u8(static_cast<int>(m_pixels[i + 2] * factor));
        }
    }
}

} // namespace aurora
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic, cppcoreguidelines-narrowing-conversions,
// bugprone-narrowing-conversions, readability-math-missing-parentheses, cppcoreguidelines-avoid-c-arrays,
// modernize-avoid-c-arrays, cppcoreguidelines-pro-type-reinterpret-cast,
// cppcoreguidelines-pro-bounds-constant-array-index, cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,
// readability-isolate-declaration, readability-avoid-nested-conditional-operator, modernize-use-auto)
