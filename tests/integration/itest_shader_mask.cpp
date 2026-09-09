/// 测试类型: integration
/// 目标单元: include/aurora/render/blend.h
/// 测试说明: 验证 Modifier::shader_mask——内容绘制完成后按渐变遮罩淡出内容盒像素：
///           LinearFade（顶亮底淡出）、LinearRise（顶淡出底亮）、RadialFade（中心亮边缘暗）
///           与 strength=0 不改变像素；无头 layout+paint 像素断言，不依赖 GUI 后端

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "framework/aurora_test.h"

using au::BuildContext;
using au::Color;
using au::Constraints;
using au::LeafWidget;
using au::Modifier;
using au::Node;
using au::Painter;
using au::Point;
using au::Rect;
using au::ShaderMaskKind;
using au::Size;
using au::Stack;
using au::Widget;

namespace aurora::test_cases::itest_shader_mask {

namespace {

class SolidBox : public LeafWidget {
  public:
    Size sz{.width = 60.0F, .height = 60.0F};
    Color color{255, 0, 0, 255};

    [[nodiscard]] auto type_name() const -> const char * override { return "SolidBox"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { return c.constrain(sz); }
    void on_paint(Painter &p, const Rect &b, const BuildContext & /*ctx*/) override { p.fill_rect(b, color); }
};

struct RenderResult {
    std::vector<std::uint8_t> pixels;
    int w = 0;
    int h = 0;
    [[nodiscard]] auto at(int x, int y, const int ch) const -> std::uint8_t {
        const std::size_t off = ((static_cast<std::size_t>(y) * w) + x) * 4;
        return pixels.at(off + ch);
    }
};

auto render_in_root(std::shared_ptr<Widget> w, const int ww, const int hh) -> RenderResult {
    auto const root = std::make_shared<Stack>(std::vector{Node{std::move(w)}});
    constexpr BuildContext ctx;
    root->mount(ctx);
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = static_cast<float>(ww), .height = static_cast<float>(hh)};
    root->layout(c, ctx);
    Painter p;
    p.begin(ww, hh);
    root->paint(p,
                Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                     .size = Size{.width = static_cast<float>(ww), .height = static_cast<float>(hh)}},
                ctx);
    const std::uint8_t *d = p.data();
    RenderResult r;
    r.w = ww;
    r.h = hh;
    // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
    r.pixels.assign(d, d + (static_cast<std::size_t>(ww) * hh * 4));  // NOLINT
    return r;
}

}  // namespace

AURORA_TEST_CASE(linear_fade_top_bright_bottom_faded) {
    const auto sb = std::make_shared<SolidBox>();
    sb->modifier.set(Modifier{}.shader_mask(ShaderMaskKind::LinearFade, 1.0F));
    const auto r = render_in_root(sb, 200, 200);
    const std::uint8_t top_red = r.at(30, 0, 0);
    const std::uint8_t bottom_red = r.at(30, 59, 0);
    AURORA_TEST_CHECK_MSG(top_red >= 250, "LinearFade top row stays bright red");
    AURORA_TEST_CHECK_MSG(bottom_red < top_red, "LinearFade bottom row darker than top");
    AURORA_TEST_CHECK_MSG(bottom_red < 30, "LinearFade bottom row strongly faded");
}

AURORA_TEST_CASE(linear_rise_top_faded_bottom_bright) {
    const auto sb = std::make_shared<SolidBox>();
    sb->modifier.set(Modifier{}.shader_mask(ShaderMaskKind::LinearRise, 1.0F));
    const auto r = render_in_root(sb, 200, 200);
    const std::uint8_t top_red = r.at(30, 0, 0);
    const std::uint8_t bottom_red = r.at(30, 59, 0);
    AURORA_TEST_CHECK_MSG(bottom_red >= 250, "LinearRise bottom row stays bright red");
    AURORA_TEST_CHECK_MSG(top_red < bottom_red, "LinearRise top row darker than bottom");
    AURORA_TEST_CHECK_MSG(top_red < 30, "LinearRise top row strongly faded");
}

AURORA_TEST_CASE(radial_fade_center_bright_corner_faded) {
    const auto sb = std::make_shared<SolidBox>();
    sb->modifier.set(Modifier{}.shader_mask(ShaderMaskKind::RadialFade, 1.0F));
    const auto r = render_in_root(sb, 200, 200);
    const std::uint8_t center_red = r.at(30, 30, 0);
    const std::uint8_t corner_red = r.at(0, 0, 0);
    AURORA_TEST_CHECK_MSG(center_red > corner_red, "RadialFade center brighter than corner");
    AURORA_TEST_CHECK_MSG(center_red >= 200, "RadialFade center stays bright");
}

AURORA_TEST_CASE(shader_mask_strength_zero_leaves_pixels_unchanged) {
    const auto sb = std::make_shared<SolidBox>();
    sb->modifier.set(Modifier{}.shader_mask(ShaderMaskKind::LinearFade, 0.0F));
    const auto r = render_in_root(sb, 200, 200);
    AURORA_TEST_CHECK_MSG(r.at(30, 59, 0) == 255, "strength 0 leaves bottom row unchanged");
}

}  // namespace aurora::test_cases::itest_shader_mask
