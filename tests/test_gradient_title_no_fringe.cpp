// 决定性验证：多色渐变背景（蓝→紫 24-band，仿 GradientTitle）上画白色文字。
// ClearType 用单点 bg 决定子像素着色 → 字形像素 R/G/B 通道差异大、红/蓝羽化。
// Supersample 灰度 AA → 字形像素近似灰度，R/G/B 通道差异小。
//
// 指标：固定布局，分别用 ClearType / Supersample 渲染。
// 扫描所有像素，对比两种 AA 下"同一坐标"的 R/G/B 各通道差：
// - 像素 (x,y) 在 ClearType 比 Supersample 通道差大 → ClearType 特有的「红/蓝羽化」。
// - 统计 ClearType 比 Supersample 通道差超过阈值的像素数与平均通道差。
#include <cmath>
#include <cstdint>
#include <iostream>

#include "aurora/aurora.h"
#include "aurora/render/text_aa_mode.h"

#include "test_harness.h"

namespace ar = aurora::render;
using aurora::Color;
using aurora::Font;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::Size;

auto paint_gradient_bg(Painter &p, int w, int h) -> void {
    constexpr int bands = 24;
    const float bw = static_cast<float>(w) / bands;
    for (int i = 0; i < bands; ++i) {
        const float t = static_cast<float>(i) / (bands - 1);
        const Color c{ static_cast<std::uint8_t>(37 + ((236 - 37) * t)),
                       static_cast<std::uint8_t>(99 + ((72 - 99) * t)),
                       static_cast<std::uint8_t>(235 + ((153 - 235) * t)) };
        p.fill_rect(Rect{ .origin = Point{ .x = static_cast<float>(i) * bw, .y = 0 },
                          .size = Size{ .width = bw + 1.0f, .height = static_cast<float>(h) } },
                    c);
    }
}

auto render_one(ar::TextAAMode mode, int w, int h) -> std::vector<std::uint8_t> {
    Painter p;
    p.begin(w, h);
    paint_gradient_bg(p, w, h);
    p.draw_text(Rect{ .origin = Point{ .x = 20.0f, .y = (static_cast<float>(h) / 2) - 20.0f },
                      .size = Size{ .width = static_cast<float>(w - 40), .height = 40.0f } },
                std::string{ "Animation" }, Font{ .size_pt = 34.0f }, Color{ 255, 255, 255 }, mode,
                ar::TextLayoutOpts{});
    const std::uint8_t *buf = p.data();
    return std::vector(buf, buf + (static_cast<std::size_t>(w) * h * 4u));
}

AURORA_TEST() {
    constexpr int W = 600;
    constexpr int H = 80;
    const auto ct = render_one(ar::TextAAMode::ClearType, W, H);
    const auto ss = render_one(ar::TextAAMode::Supersample, W, H);

    // 逐像素对比两种 AA 路径在同一位置的颜色差。
    long n_diff_total = 0;    // 颜色不同的像素数（任一通道差 >= 8）
    long n_fringe = 0;        // 通道不平衡像素（R-B 或 R-G 或 G-B 差 >= 32）
    double sum_ch_diff = 0.0; // 像素三个通道差的最大值的累计
    double sum_ch_diff_fringe = 0.0;
    constexpr std::size_t n_px = static_cast<std::size_t>(W) * H;
    for (std::size_t i = 0; i < n_px; ++i) {
        const std::size_t off = i * 4u;
        const int cr = ct[off];
        const int cg = ct[off + 1];
        const int cb = ct[off + 2];
        const int sr = ss[off];
        const int sg = ss[off + 1];
        const int sb = ss[off + 2];
        const int dr = std::abs(cr - sr);
        const int dg = std::abs(cg - sg);
        const int db = std::abs(cb - sb);
        const int mch = std::max({ dr, dg, db });
        if (mch >= 8) {
            ++n_diff_total;
            sum_ch_diff += mch;
        }
        // ClearType 羽化像素：与 Supersample 同一位置的某通道差 >> 其它通道差。
        // 例：ClearType 红羽化 → R 通道更亮，B 通道更暗 → dr, db 都大，dg 小。
        // 但更直接的检测：在 ClearType 下 R 与 B 通道差与在 Supersample 下差异巨大。
        const int ct_rb = std::abs(cr - cb);
        const int ss_rb = std::abs(sr - sb);
        const int ct_rg = std::abs(cr - cg);
        const int ss_rg = std::abs(sr - sg);
        const int ct_gb = std::abs(cg - cb);
        const int ss_gb = std::abs(sg - sb);
        const int rb_imbal = std::abs(ct_rb - ss_rb);
        const int rg_imbal = std::abs(ct_rg - ss_rg);
        const int gb_imbal = std::abs(ct_gb - ss_gb);
        if (rb_imbal >= 24 || rg_imbal >= 24 || gb_imbal >= 24) {
            ++n_fringe;
            sum_ch_diff_fringe += std::max({ rb_imbal, rg_imbal, gb_imbal });
        }
    }
    AURORA_LOG_INFO("test", "====== AA mode comparison on multi-color gradient (white text) ======");
    AURORA_LOG_INFO("test", "total_px=", n_px);
    AURORA_LOG_INFO("test", "[mch>=8 ] differing pixels  : ", n_diff_total, " (ClearType vs Supersample, same coord)");
    AURORA_LOG_INFO("test", "[avg ] avg max-ch-diff     : ", (n_diff_total > 0 ? sum_ch_diff / n_diff_total : 0.0));
    AURORA_LOG_INFO("test", "[frng] channel-imbalance px: ", n_fringe, " (R-B/R-G/G-B diff >= 24 between modes)");
    AURORA_LOG_INFO("test", "[frng] avg imbalance       : ", (n_fringe > 0 ? sum_ch_diff_fringe / n_fringe : 0.0));

    // 演示型测试：仅输出指标，不 fail。这是 ClearType 红/蓝羽化在多变背景上的"留证"——
    // 对比 Supersample 时 n_fringe 必然 > 0。`examples/demos/common.h:GradientTitle` 已经显式
    // 选 Supersample 让 demo 不出现该羽化。
}
