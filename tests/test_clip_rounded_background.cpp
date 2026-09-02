// test_clip_rounded_background.cpp
// 回归测试：clip_rounded 必须同时裁剪控件自身背景，且 Paint 修饰应作用于完整视觉盒子。
// 旧实现 1：background 在 clip_rounded 之前被当作直角矩形填色，子节点才被圆角裁剪，
//          导致圆角容器（搜索框等）出现「直角背景 + 圆角内容」，四角直角背景溢出。
// 旧实现 2：background/clip_rounded 只填充/裁剪 content_box，padding 区域透明露白。
#include <cstdint>

#include "aurora/aurora.h"
#include "aurora/widget/containers.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Color;
using aurora::Constraints;
using aurora::EdgeInsets;
using aurora::Modifier;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::Row;
using aurora::Size;
using aurora::Widget;

static auto region_has_bg(const Painter &p, int x0, int y0, int x1, int y1, const Color &bg) -> bool {
    const int w = p.width();
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::uint8_t *c = &p.data()[((static_cast<std::size_t>(y) * w) + x) * 4];
            // 容忍抗锯齿羽化：背景色 ±36 内视为背景泄漏到圆角区。
            if (std::abs(c[0] - static_cast<int>(bg.m_r)) < 36 && std::abs(c[1] - static_cast<int>(bg.m_g)) < 36 &&
                std::abs(c[2] - static_cast<int>(bg.m_b)) < 36) {
                return true;
            }
        }
    }
    return false;
}

static auto make_search_like() -> std::shared_ptr<Widget> {
    auto row = std::make_shared<Row>(std::initializer_list<Node>{});
    row->modifier = Modifier{}.background(Color{ 180, 180, 180, 255 }).clip_rounded(20.0f).width(200.0f).height(80.0f);
    return row;
}

static auto test_rounded_background_no_padding() -> void {
    const auto w = make_search_like();
    constexpr BuildContext ctx;
    w->mount(ctx);
    Constraints cc;
    cc.min = Size{ .width = 0, .height = 0 };
    cc.max = Size{ .width = 400, .height = 200 };
    const Size sz = w->layout(cc, ctx);

    Painter p;
    p.begin(static_cast<int>(sz.width), static_cast<int>(sz.height));
    p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = sz.width, .height = sz.height } },
                Color::white());
    w->paint(p, Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = sz.width, .height = sz.height } }, ctx);

    const int img_w = static_cast<int>(sz.width);
    const int img_h = static_cast<int>(sz.height);
    // 四角 5x5 区域必须被圆角裁剪（不含背景色）。clip_rounded(20) 下该区域
    // 完全落在圆角弧线之外，旧实现的直角矩形背景会在此泄漏。
    constexpr Color bg{ 180, 180, 180, 255 };
    const bool tl = region_has_bg(p, 0, 0, 5, 5, bg);
    const bool tr = region_has_bg(p, img_w - 5, 0, img_w, 5, bg);
    const bool bl = region_has_bg(p, 0, img_h - 5, 5, img_h, bg);
    const bool br = region_has_bg(p, img_w - 5, img_h - 5, img_w, img_h, bg);

    AURORA_TEST_PRINTF("corner bg leak: TL=%d TR=%d BL=%d BR=%d\n", tl, tr, bl, br);
    AURORA_TEST_CHECK_MSG(!tl && !tr && !bl && !br,
                          "clip_rounded must round the widget's own background (no sharp corner leak)");

    // 同时验证对称性：左/右、上/下角行为一致。
    AURORA_TEST_CHECK_MSG(tl == tr && bl == br, "rounded background must be symmetric L/R and T/B");
}

// 带 Padding 的场景：Background + ClipRounded 应填满整个视觉盒子（含 padding 区域），
// 而不仅仅是 content_box。旧实现会让 padding 区域透明，搜索框等圆角输入框因此露白。
static auto test_padded_rounded_background() -> void {
    constexpr Color bg{ 180, 180, 180, 255 };
    constexpr Color page{ 255, 255, 255, 255 };
    constexpr float radius = 20.0f;
    constexpr float pad = 12.0f;
    constexpr float w = 160.0f;
    constexpr float h = 64.0f;

    // 空 Row + background + clip_rounded + padding，模拟搜索框外壳。
    const auto root = std::make_shared<Row>(std::initializer_list<Node>{});
    root->modifier = Modifier{}
                         .background(bg)
                         .clip_rounded(radius)
                         .padding(EdgeInsets{ .left = pad, .top = pad, .right = pad, .bottom = pad })
                         .width(w)
                         .height(h);

    constexpr BuildContext ctx;
    root->mount(ctx);
    Constraints cc;
    cc.min = Size{ .width = 0, .height = 0 };
    cc.max = Size{ .width = w, .height = h };
    root->layout(cc, ctx);

    Painter p;
    p.begin(static_cast<int>(w), static_cast<int>(h));
    p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = w, .height = h } }, page);
    root->paint(p, Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = w, .height = h } }, ctx);

    constexpr int iw = static_cast<int>(w);
    constexpr int ih = static_cast<int>(h);

    auto sample = [&](int x, int y) -> Color {
        const uint8_t *px = p.data() + (((static_cast<std::size_t>(y) * iw) + x) * 4);
        return Color{ px[0], px[1], px[2], px[3] };
    };

    // 检查区域是否全为背景色。
    auto region_filled = [&](int x0, int y0, int x1, int y1) -> bool {
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                if (sample(x, y) != bg) {
                    return false;
                }
            }
        }
        return true;
    };

    // 四边中部（padding 区域内）应被背景填满，不应露白。
    AURORA_TEST_CHECK(region_filled((iw / 2) - 2, 2, (iw / 2) + 2, 5));           // 顶边
    AURORA_TEST_CHECK(region_filled((iw / 2) - 2, ih - 5, (iw / 2) + 2, ih - 2)); // 底边
    AURORA_TEST_CHECK(region_filled(2, (ih / 2) - 2, 5, (ih / 2) + 2));           // 左边
    AURORA_TEST_CHECK(region_filled(iw - 5, (ih / 2) - 2, iw - 2, (ih / 2) + 2)); // 右边

    // padding 区域接近圆角但仍位于圆角弧线内侧（圆心 (20,20)，半径 20）的点应被填满。
    // 取 (8,8) 附近，距离圆心约 17 < 20，处于圆角裁剪区内且属于 padding 区域。
    AURORA_TEST_CHECK(region_filled(8, 8, 11, 11));                     // 左上内侧 padding
    AURORA_TEST_CHECK(region_filled(iw - 11, 8, iw - 8, 11));           // 右上内侧 padding
    AURORA_TEST_CHECK(region_filled(8, ih - 11, 11, ih - 8));           // 左下内侧 padding
    AURORA_TEST_CHECK(region_filled(iw - 11, ih - 11, iw - 8, ih - 8)); // 右下内侧 padding

    // 最外层角点本身因圆角裁剪应为 page 白色，验证圆角仍然生效。
    AURORA_TEST_CHECK_EQ(sample(0, 0), page);
    AURORA_TEST_CHECK_EQ(sample(iw - 1, 0), page);
    AURORA_TEST_CHECK_EQ(sample(0, ih - 1), page);
    AURORA_TEST_CHECK_EQ(sample(iw - 1, ih - 1), page);
}

AURORA_TEST() {
    test_rounded_background_no_padding();
    test_padded_rounded_background();
}
