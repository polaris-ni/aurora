/// 测试类型: integration
/// 目标单元: include/aurora/window/window.h（脏区 sink）、include/aurora/widget/scroll.h（离屏缓冲脏带）
/// 测试说明: 验证 Window::present_root 脏区裁剪绘制——「仅绘制脏区（保留上帧缓冲 + clear_rect +
///           push_clip）」与「整帧重绘」逐位一致，且脏区外像素不被改写；用 HeadlessSurface::data()
///           直接读取设备像素缓冲比对（Headless 部分受 AURORA_BACKEND_HEADLESS 门控）；
///           本测试不含计时断言。另覆盖「绘制落在自身盒外」的一类脏区缺陷：基类统一焦点环
///           外扩于控件盒，获焦帧须入裁剪才能上屏、失焦帧须入裁剪才能重绘掉上一帧的环，
///           普通 sink 与 Scroll 离屏缓冲脏带两条标脏路径都要覆盖。

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "aurora/aurora.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_dirty_clip_paint {

namespace {

#ifdef AURORA_BACKEND_HEADLESS
// 创建已定尺寸的 Headless 窗口（HeadlessSurface 尺寸由首次 begin_frame 确立）。
// HeadlessSurface 仅在 AURORA_BACKEND_HEADLESS 下编译；宏关闭时本文件走 SKIP 桩。
auto make_window(int w, int h) -> au::Window {
    auto surface = std::make_unique<au::HeadlessSurface>();
    (void)surface->begin_frame(w, h);
    return au::Window{std::move(surface)};
}
#endif

auto copy_pixels(const std::uint8_t *src, size_t n) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> out(n);
    if (src != nullptr) {
        std::memcpy(out.data(), src, n);
    }
    return out;
}

auto count_diff(const std::vector<std::uint8_t> &a, const std::vector<std::uint8_t> &b) -> size_t {
    size_t d = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        if (a.at(i) != b.at(i)) {
            ++d;
        }
    }
    return d;
}

/// @brief 统计盒外环带（`box` 外扩 5 px 一圈、`box` 自身除外）内相对参考帧发生变化的像素数。
///
/// 该带完全落在控件自身盒之外，故命中只可能是画在盒外的装饰（本用例针对基类统一焦点环，
/// 环带位于外扩 2–4 dp）；控件自身外观不参与计数。判据取「与未获焦参考帧的差」而非某个具体
/// 色相，因为环色由主题命名令牌 `focus.ring` 决定（浅色主题为黑、深色主题为白，见 `Theme::light()`），
/// 写死色相会让这条脏区回归随主题配色漂移。
auto ring_pixels_outside(const std::vector<std::uint8_t> &px, const std::vector<std::uint8_t> &ref, int img_w,
                         int img_h, const au::Rect &box) -> size_t {
    const int x0 = std::max(0, static_cast<int>(std::floor(box.origin.x)) - 5);
    const int y0 = std::max(0, static_cast<int>(std::floor(box.origin.y)) - 5);
    const int x1 = std::min(img_w, static_cast<int>(std::ceil(box.right())) + 5);
    const int y1 = std::min(img_h, static_cast<int>(std::ceil(box.bottom())) + 5);
    size_t n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (x >= static_cast<int>(box.origin.x) && x < static_cast<int>(std::ceil(box.right())) &&
                y >= static_cast<int>(box.origin.y) && y < static_cast<int>(std::ceil(box.bottom()))) {
                continue;  // 盒内像素属控件自身外观，不计
            }
            const size_t i = ((static_cast<size_t>(y) * static_cast<size_t>(img_w)) + static_cast<size_t>(x)) * 4U;
            // 比 RGBA 四通道而非仅 RGB：Headless 帧底色是全透明黑 (0,0,0,0)，浅色主题的环色恰为
            // 纯黑 (0,0,0,255)——RGB 逐位相同，只有 alpha 把它显影出来。
            if (px.at(i) != ref.at(i) || px.at(i + 1U) != ref.at(i + 1U) || px.at(i + 2U) != ref.at(i + 2U) ||
                px.at(i + 3U) != ref.at(i + 3U)) {
                ++n;  // 与参考帧该处不同 = 盒外装饰落到了这里
            }
        }
    }
    return n;
}

}  // namespace

AURORA_TEST_CASE(root_dirty_clip_matches_full_redraw) {
#ifdef AURORA_BACKEND_HEADLESS
    // 场景 1：根控件整体变脏（脏区 = 全窗）——裁剪分支在 clip == 全窗时仍须与整帧一致。
    const auto chip = std::make_shared<au::Chip>();
    chip->set_label("hi");
    chip->set_background(Color{0, 0, 255});
    au::Node root{chip};

    au::Window win = make_window(256, 192);
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 1：首帧全绘
    auto const &hs = dynamic_cast<au::HeadlessSurface &>(win.surface());
    AURORA_TEST_CHECK(hs.data() != nullptr);
    constexpr size_t n = static_cast<size_t>(256) * static_cast<size_t>(192) * 4U;
    const auto before = copy_pixels(hs.data(), n);

    chip->set_background(Color{255, 0, 0});  // paint-only 脏（根 → 全窗几何）
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 2：脏区裁剪绘制
    const auto dirty = copy_pixels(hs.data(), n);

    win.force_full_redraw();
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 3：整帧重绘基准
    const auto full = copy_pixels(hs.data(), n);

    AURORA_TEST_CHECK(count_diff(dirty, full) == 0);  // 脏区重绘 == 整帧重绘（逐位）
    AURORA_TEST_CHECK(count_diff(dirty, before) > 0);  // 颜色变更确实生效
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS not enabled, HeadlessSurface is not compiled");
#endif
}

AURORA_TEST_CASE(nested_partial_dirty_clip_matches_full_redraw) {
#ifdef AURORA_BACKEND_HEADLESS
    // 场景 2：嵌套小控件变脏（脏区 < 全窗）——真正锻炼部分裁剪路径：
    // 裁剪内与整帧重绘逐位一致；裁剪外（含同帧其它控件的文本）逐字节保持上帧内容。
    constexpr int w = 320;
    constexpr int h = 240;
    const auto text = std::make_shared<au::Text>("stable reference line");
    const auto chip = std::make_shared<au::Chip>();
    chip->set_label("dirty");
    chip->set_background(Color{0, 0, 255});
    au::Node root{au::Column{au::Node{text}, au::Node{chip}}};

    au::Window win = make_window(w, h);
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 1：首帧全绘
    auto const &hs = dynamic_cast<au::HeadlessSurface &>(win.surface());
    constexpr size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4U;
    const auto before = copy_pixels(hs.data(), n);

    // 脏源必须是「小于全窗」的子控件几何（部分裁剪路径，而非退化为全窗裁剪）。
    const au::Rect cb = chip->paint_bounds();
    AURORA_TEST_CHECK(cb.size.width > 0.0F && cb.size.height > 0.0F);
    AURORA_TEST_CHECK(cb.size.width < static_cast<float>(w) - 1.0F);
    AURORA_TEST_CHECK(cb.size.height < static_cast<float>(h) - 1.0F);

    chip->set_background(Color{255, 0, 0});  // 仅 Chip 变脏
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 2：部分脏区裁剪绘制
    const auto dirty = copy_pixels(hs.data(), n);

    win.force_full_redraw();
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 3：整帧重绘基准
    const auto full = copy_pixels(hs.data(), n);

    // 核心不变量：部分脏区重绘与整帧重绘逐位一致（不漏绘、无残留、无双重混合）。
    AURORA_TEST_CHECK(count_diff(dirty, full) == 0);

    // 脏区外像素不被改写：chip 几何外扩 2px 容差之外，帧 2 与帧 1 逐字节相同。
    const int x0 = static_cast<int>(std::floor(cb.origin.x)) - 2;
    const int y0 = static_cast<int>(std::floor(cb.origin.y)) - 2;
    const int x1 = static_cast<int>(std::ceil(cb.origin.x + cb.size.width)) + 2;
    const int y1 = static_cast<int>(std::ceil(cb.origin.y + cb.size.height)) + 2;
    size_t outside_diff = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (x >= x0 && x < x1 && y >= y0 && y < y1) {
                continue;  // 脏区（含容差）内跳过
            }
            const size_t i = ((static_cast<size_t>(y) * static_cast<size_t>(w)) + static_cast<size_t>(x)) * 4U;
            for (size_t k = 0; k < 4; ++k) {
                if (dirty[i + k] != before[i + k]) {  // NOLINT 容器类型无法本地确证为顺序容器
                    ++outside_diff;
                }
            }
        }
    }
    AURORA_TEST_CHECK(outside_diff == 0);  // 脏区外零改写（文本行等上帧内容原样保留）

    // 脏区内确实重绘出了新颜色。
    AURORA_TEST_CHECK(count_diff(dirty, before) > 0);
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS not enabled, HeadlessSurface is not compiled");
#endif
}

AURORA_TEST_CASE(focus_ring_band_repainted_when_focus_changes) {
#ifdef AURORA_BACKEND_HEADLESS
    // 回归：基类统一焦点环画在控件盒**外** 2–4 dp（src/aurora/widget/widget.cpp 的 paint_content），
    // 而脏区曾只按控件自身 paint_bounds() 标记 → 环带落在脏区裁剪之外，两个方向同时出错：
    //  ① 获焦帧裁剪不含环带 → 环根本画不上屏；
    //  ② 失焦帧裁剪不含环带 → 上一帧的环像素留在帧缓冲上，屏幕表现即「控件外面多一圈主题色、
    //     看起来比自身盒大」。
    // 标脏现走 Widget::dirty_bounds()（自身盒 ∪ 盒外装饰），两个方向都须与整帧重绘逐位一致。
    constexpr int w = 320;
    constexpr int h = 240;
    const auto label = std::make_shared<au::Text>("row above keeps the button off the window edge");
    const auto btn = std::make_shared<au::Button>("tap");
    btn->set_focusable(true);
    au::Node root{au::Column{au::Node{label}, au::Node{btn}}};

    au::Window win = make_window(w, h);
    au::FocusManager fm;
    fm.set_root(&root.widget());
    auto const &hs = dynamic_cast<au::HeadlessSurface &>(win.surface());
    constexpr size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4U;

    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 1：未获焦
    const au::Rect box = btn->paint_bounds();
    AURORA_TEST_REQUIRE(box.size.width > 0.0F && box.size.height > 0.0F);
    AURORA_TEST_REQUIRE(box.origin.y > 6.0F);  // 上方留空，否则环带被窗口上边缘切掉无从观测
    const auto unfocused = copy_pixels(hs.data(), n);

    fm.set_focus(btn.get());
    AURORA_TEST_REQUIRE(btn->is_focused());
    AURORA_TEST_CHECK(btn->dirty_bounds().size.width > box.size.width);  // 标脏盒须含盒外环带
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 2：获焦，部分脏区裁剪绘制
    const auto focused = copy_pixels(hs.data(), n);
    AURORA_TEST_CHECK_MSG(ring_pixels_outside(focused, unfocused, w, h, box) > 0,
                          "focused control must paint the base ring in the band outside its box");

    win.force_full_redraw();
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 3：获焦态的整帧重绘基准
    AURORA_TEST_CHECK(count_diff(focused, copy_pixels(hs.data(), n)) == 0);  // 方向①：获焦帧 == 整帧

    fm.set_focus(nullptr);
    AURORA_TEST_REQUIRE_FALSE(btn->is_focused());
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 4：失焦，部分脏区裁剪绘制
    const auto after_blur = copy_pixels(hs.data(), n);

    win.force_full_redraw();
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 5：整帧重绘基准
    const auto full = copy_pixels(hs.data(), n);

    // 核心不变量（方向②）：失焦帧与整帧重绘逐位一致（无修复时差出环带那一圈残留）。
    AURORA_TEST_CHECK(count_diff(after_blur, full) == 0);
    AURORA_TEST_CHECK_EQ(ring_pixels_outside(after_blur, unfocused, w, h, box), 0U);
    // 回到未获焦外观：与帧 1 逐位一致（环带既无残留也无二次混合）。
    AURORA_TEST_CHECK(count_diff(after_blur, unfocused) == 0);
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS not enabled, HeadlessSurface is not compiled");
#endif
}

AURORA_TEST_CASE(focus_ring_band_inside_scroll_repainted) {
#ifdef AURORA_BACKEND_HEADLESS
    // 同一缺陷的第二条汇聚路径：后代落在 Scroll 的离屏内容缓冲内时，标脏经
    // `Scroll::on_descendant_dirty` 合并成缓冲局部脏带（内容坐标系）并只重录该带。脏带若不含
    // 后代的盒外环带，残留会固化在离屏缓冲里、随视口反复上屏（普通脏区 sink 修好也不覆盖此路）。
    constexpr int w = 320;
    constexpr int h = 240;
    const auto btn = std::make_shared<au::Button>("tap");
    btn->set_focusable(true);
    // 内容高于视口才成其为滚动容器；按钮放第二行，视口 offset=0 时其环带完整可见。
    au::Node content{au::Column{au::Node{au::Text("first row")}, au::Node{btn}, au::Node{au::Text("row 3")},
                                au::Node{au::Text("row 4")}, au::Node{au::Text("row 5")}, au::Node{au::Text("row 6")}}};
    const auto scroller = std::make_shared<au::Scroll>(au::ScrollProps{.child = std::move(content)});
    scroller->modifier.set(au::Modifier{}.size(static_cast<float>(w), 100.0F));  // 视口高 100 < 内容高
    au::Node root{au::Column{au::Node{au::Text("header")}, au::Node{scroller}}};

    au::Window win = make_window(w, h);
    au::FocusManager fm;
    fm.set_root(&root.widget());
    auto const &hs = dynamic_cast<au::HeadlessSurface &>(win.surface());
    constexpr size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4U;

    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 1：未获焦
    const auto unfocused = copy_pixels(hs.data(), n);

    fm.set_focus(btn.get());
    AURORA_TEST_REQUIRE(btn->is_focused());
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 2：获焦，脏带局部重录
    const auto focused = copy_pixels(hs.data(), n);
    AURORA_TEST_CHECK_MSG(count_diff(focused, unfocused) > 0,
                          "focus gain must actually reach the screen through the scroll band (ring never committed "
                          "when the band excludes the out-of-box decoration)");

    win.force_full_redraw();
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 3：获焦态整帧重绘基准
    AURORA_TEST_CHECK(count_diff(focused, copy_pixels(hs.data(), n)) == 0);

    fm.set_focus(nullptr);
    AURORA_TEST_REQUIRE_FALSE(btn->is_focused());
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 4：失焦，脏带局部重录
    const auto after_blur = copy_pixels(hs.data(), n);

    win.force_full_redraw();
    AURORA_TEST_CHECK(win.present_root(root).ok());  // 帧 5：整帧重绘基准
    AURORA_TEST_CHECK(count_diff(after_blur, copy_pixels(hs.data(), n)) == 0);  // 无环带残留
    AURORA_TEST_CHECK(count_diff(after_blur, unfocused) == 0);  // 回到帧 1 外观
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS not enabled, HeadlessSurface is not compiled");
#endif
}

}  // namespace aurora::test_cases::itest_dirty_clip_paint
