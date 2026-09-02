// 脏区裁剪重绘底色回归：验证「跳过 begin_frame 的部分脏路径」在铺浅色底的后端上，
// 会以 Surface::clear_color() 重铺脏区底色，与整帧 begin_frame 逐位一致。
//
// 背景（bug: LazyList 点击后子项变黑）：真实窗口后端（Win32/GLFW/X11/Wayland）在 begin_frame
// 铺浅色底 (245,245,247)，但部分脏路径跳过 begin_frame，仅 clear_rect 归零。脏区内若无不透明
// 背景的控件（裸 Text / 无背景 LazyList 子项），归零后只画字形 → 露出零基底（黑）。
// HeadlessSurface 的 begin_frame 不铺底色（零基底），故无法复现该 bug——本测试用一个「铺浅色底 +
// 覆盖 clear_color」的自定义 Surface 精确复现真实后端，并断言部分脏路径与整帧逐位一致。
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"

#include "test_harness.h"

using aurora::Color;
using aurora::Column;
using aurora::ColumnProps;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::Result;
using aurora::Size;
using aurora::Surface;
using aurora::Text;
using aurora::Window;

namespace {

// 模拟真实窗口后端：begin_frame 铺浅色底色，并跨帧保留 Painter 缓冲（部分脏帧不重分配）。
class BgSurface final : public Surface {
  public:
    static constexpr Color m_k_bg{ 245, 245, 247, 255 };

    [[nodiscard]] auto begin_frame(int width, int height) -> Result<bool> override {
        if (!m_begun || static_cast<int>(m_size.width) != width || static_cast<int>(m_size.height) != height) {
            m_painter.begin(width, height);
            m_begun = true;
        }
        m_size = Size{ .width = static_cast<float>(width), .height = static_cast<float>(height) };
        // 与 Win32/GLFW/X11/Wayland 一致：整帧铺浅色底色。
        m_painter.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = m_size }, m_k_bg);
        return Result{ true };
    }
    [[nodiscard]] auto painter() -> Painter & override { return m_painter; }
    [[nodiscard]] auto present() -> Result<bool> override { return Result{ true }; }
    [[nodiscard]] auto size() const -> Size override { return m_size; }
    [[nodiscard]] auto clear_color() const -> Color override { return m_k_bg; }
    [[nodiscard]] auto data() const -> const std::uint8_t * override { return m_painter.data(); }

  private:
    Painter m_painter;
    Size m_size{ .width = 0.0f, .height = 0.0f };
    bool m_begun = false;
};

auto copy_pixels(const std::uint8_t *src, const size_t n) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> out(n);
    if (src != nullptr) {
        std::memcpy(out.data(), src, n);
    }
    return out;
}

// 裸 Text（无不透明背景）作为脏源：点击/失焦触发 paint-only 脏，走部分裁剪路径。
// 若部分路径不重铺底色，脏区归零后只画字形，Text 行背景会变黑（与整帧不一致）。
auto scenario_bare_text_partial_repaint() -> void {
    constexpr int w = 320;
    constexpr int h = 240;

    const auto stable = std::make_shared<Text>("stable reference line");
    const auto target = std::make_shared<Text>("dirty target line");
    Node root{ std::make_shared<Column>(ColumnProps{ .children = { Node{ stable }, Node{ target } } }) };

    // 先定尺寸（HeadlessSurface 同款约定：begin_frame 确立尺寸后再交给 Window）。
    auto surface = std::make_unique<BgSurface>();
    (void)surface->begin_frame(w, h);
    Window win{ std::move(surface) };

    AURORA_TEST_CHECK(win.present_root(root).ok()); // 帧 1：首帧整帧铺底 + 绘树
    auto const &s = dynamic_cast<BgSurface &>(win.surface());
    constexpr size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;

    // 脏源须为「小于全窗」的子控件几何，走部分裁剪路径而非退化为全窗裁剪。
    const Rect cb = target->paint_bounds();
    AURORA_TEST_CHECK(cb.size.width > 0.0f && cb.size.height > 0.0f);
    AURORA_TEST_CHECK(cb.size.width < static_cast<float>(w) - 1.0f);
    AURORA_TEST_CHECK(cb.size.height < static_cast<float>(h) - 1.0f);

    target->mark_needs_paint();                     // 仅绘制脏（模拟点击/失焦重绘），内容不变
    AURORA_TEST_CHECK(win.present_root(root).ok()); // 帧 2：部分脏区裁剪绘制
    const auto dirty = copy_pixels(s.data(), n);

    win.force_full_redraw();
    AURORA_TEST_CHECK(win.present_root(root).ok()); // 帧 3：整帧重绘基准
    const auto full = copy_pixels(s.data(), n);

    // 核心不变量：在 target 行内部（内缩 3px 以排除字形 AA 描边/降部溢出 paint_bounds 后被裁剪
    // 的边界像素——该溢出是「裁剪到 paint_bounds」的既有性质，与本 bug 无关），部分脏区重绘与
    // 整帧重绘逐位一致——证明脏区底色被以 clear_color 正确重铺（不再露黑底），且字形绘制一致。
    const int rx0 = std::max(0, static_cast<int>(cb.origin.x) + 3);
    const int ry0 = std::max(0, static_cast<int>(cb.origin.y) + 3);
    const int rx1 = std::min(w, static_cast<int>(cb.origin.x + cb.size.width) - 3);
    const int ry1 = std::min(h, static_cast<int>(cb.origin.y + cb.size.height) - 3);
    size_t inside_diff = 0;
    for (int y = ry0; y < ry1; ++y) {
        for (int x = rx0; x < rx1; ++x) {
            const size_t k = ((static_cast<size_t>(y) * static_cast<size_t>(w)) + static_cast<size_t>(x)) * 4u;
            for (size_t c = 0; c < 4; ++c) {
                if (dirty[k + c] != full[k + c]) {
                    ++inside_diff;
                }
            }
        }
    }
    AURORA_TEST_CHECK(inside_diff == 0);

    // 直接采样 target 行内一处背景像素（右下角，远离左对齐字形），应为底色而非黑。
    // 这是本 bug 的直接判据：修复前部分脏路径归零后不重铺底色，此处会是 (0,0,0)。
    const int px = std::min(w - 1, static_cast<int>(cb.origin.x + cb.size.width) - 2);
    const int py = std::min(h - 1, static_cast<int>(cb.origin.y + cb.size.height) - 2);
    const size_t i = ((static_cast<size_t>(py) * static_cast<size_t>(w)) + static_cast<size_t>(px)) * 4u;
    AURORA_TEST_CHECK(dirty[i + 0] == BgSurface::m_k_bg.m_r);
    AURORA_TEST_CHECK(dirty[i + 1] == BgSurface::m_k_bg.m_g);
    AURORA_TEST_CHECK(dirty[i + 2] == BgSurface::m_k_bg.m_b);
}

} // namespace

AURORA_TEST() { scenario_bare_text_partial_repaint(); }
