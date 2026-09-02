// 脏区裁剪绘制：验证「仅绘制脏区（保留上帧缓冲 + clear_rect + push_clip）」
// 与「整帧重绘」逐位一致，且脏区外像素不被改写（golden 零差异约束）。
// 用 HeadlessSurface::data() 直接读取设备像素缓冲比对；本测试不含计时断言。
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::Chip;
using aurora::Color;
using aurora::Column;
using aurora::ColumnProps;
using aurora::HeadlessSurface;
using aurora::Node;
using aurora::Rect;
using aurora::Text;
using aurora::Window;

namespace {

// 创建已定尺寸的 Headless 窗口（HeadlessSurface 尺寸由首次 begin_frame 确立）。
auto make_window(int w, int h) -> Window {
    auto surface = std::make_unique<HeadlessSurface>();
    (void)surface->begin_frame(w, h);
    return Window{ std::move(surface) };
}

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
        if (a[i] != b[i]) {
            ++d;
        }
    }
    return d;
}

// 场景 1：根控件整体变脏（脏区 = 全窗）——裁剪分支在 clip == 全窗时仍须与整帧一致。
auto scenario_root_dirty() -> void {
    const auto chip = std::make_shared<Chip>();
    chip->set_label("hi");
    chip->set_background(Color{ 0, 0, 255 });
    Node root{ chip };

    Window win = make_window(256, 192);
    AURORA_TEST_CHECK(win.present_root(root).ok()); // 帧 1：首帧全绘
    auto const &hs = dynamic_cast<HeadlessSurface &>(win.surface());
    AURORA_TEST_CHECK(hs.data() != nullptr);
    constexpr size_t n = static_cast<size_t>(256) * static_cast<size_t>(192) * 4u;
    const auto before = copy_pixels(hs.data(), n);

    chip->set_background(Color{ 255, 0, 0 });       // paint-only 脏（根 → 全窗几何）
    AURORA_TEST_CHECK(win.present_root(root).ok()); // 帧 2：脏区裁剪绘制
    const auto dirty = copy_pixels(hs.data(), n);

    win.force_full_redraw();
    AURORA_TEST_CHECK(win.present_root(root).ok()); // 帧 3：整帧重绘基准
    const auto full = copy_pixels(hs.data(), n);

    AURORA_TEST_CHECK(count_diff(dirty, full) == 0);  // 脏区重绘 == 整帧重绘（逐位）
    AURORA_TEST_CHECK(count_diff(dirty, before) > 0); // 颜色变更确实生效
}

// 场景 2：嵌套小控件变脏（脏区 < 全窗）——真正锻炼部分裁剪路径：
// 裁剪内与整帧重绘逐位一致；裁剪外（含同帧其它控件的文本）逐字节保持上帧内容。
auto scenario_nested_partial_dirty() -> void {
    constexpr int w = 320;
    constexpr int h = 240;
    const auto text = std::make_shared<Text>("stable reference line");
    const auto chip = std::make_shared<Chip>();
    chip->set_label("dirty");
    chip->set_background(Color{ 0, 0, 255 });
    Node root{ std::make_shared<Column>(ColumnProps{ .children = { Node{ text }, Node{ chip } } }) };

    Window win = make_window(w, h);
    AURORA_TEST_CHECK(win.present_root(root).ok()); // 帧 1：首帧全绘
    auto const &hs = dynamic_cast<HeadlessSurface &>(win.surface());
    constexpr size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    const auto before = copy_pixels(hs.data(), n);

    // 脏源必须是「小于全窗」的子控件几何（部分裁剪路径，而非退化为全窗裁剪）。
    const Rect cb = chip->paint_bounds();
    AURORA_TEST_CHECK(cb.size.width > 0.0f && cb.size.height > 0.0f);
    AURORA_TEST_CHECK(cb.size.width < static_cast<float>(w) - 1.0f);
    AURORA_TEST_CHECK(cb.size.height < static_cast<float>(h) - 1.0f);

    chip->set_background(Color{ 255, 0, 0 });       // 仅 Chip 变脏
    AURORA_TEST_CHECK(win.present_root(root).ok()); // 帧 2：部分脏区裁剪绘制
    const auto dirty = copy_pixels(hs.data(), n);

    win.force_full_redraw();
    AURORA_TEST_CHECK(win.present_root(root).ok()); // 帧 3：整帧重绘基准
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
                continue; // 脏区（含容差）内跳过
            }
            const size_t i = ((static_cast<size_t>(y) * static_cast<size_t>(w)) + static_cast<size_t>(x)) * 4u;
            for (size_t k = 0; k < 4; ++k) {
                if (dirty[i + k] != before[i + k]) {
                    ++outside_diff;
                }
            }
        }
    }
    AURORA_TEST_CHECK(outside_diff == 0); // 脏区外零改写（文本行等上帧内容原样保留）

    // 脏区内确实重绘出了新颜色。
    AURORA_TEST_CHECK(count_diff(dirty, before) > 0);
}

} // namespace

AURORA_TEST() {
    scenario_root_dirty();
    scenario_nested_partial_dirty();
}
