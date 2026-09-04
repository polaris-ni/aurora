// Modifier::shader_mask 验证：内容绘制完成后按渐变淡出内容盒像素。
// 无头 layout + paint，像素断言，不依赖 GUI 后端。
#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Color;
using aurora::Constraints;
using aurora::LeafWidget;
using aurora::Modifier;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::ShaderMaskKind;
using aurora::SignalViewBase;
using aurora::Size;
using aurora::Stack;
using aurora::Widget;
using aurora::WidgetDescriptor;

namespace {

class SolidBox : public LeafWidget {
  public:
    Size sz_{.width = 60.0F, .height = 60.0F};
    Color color_{255, 0, 0, 255};
    void collect_signals(std::vector<SignalViewBase*>& /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char* override { return "SolidBox"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "SolidBox", .children_policy = "none"};
    }

  protected:
    auto on_layout(const Constraints& c, const BuildContext& /*ctx*/) -> Size override { return c.constrain(sz_); }
    void on_paint(Painter& p, const Rect& b, const BuildContext& /*ctx*/) override { p.fill_rect(b, color_); }
};

struct RenderResult {
    std::vector<std::uint8_t> pixels_;
    int w_ = 0, h_ = 0;
    [[nodiscard]] auto at(int x, int y, const int ch) const -> std::uint8_t {
        const std::size_t off = ((static_cast<std::size_t>(y) * w_) + x) * 4;
        return pixels_.at(off + ch);
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
    const std::uint8_t* d = p.data();
    RenderResult r;
    r.w_ = ww;
    r.h_ = hh;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
    r.pixels_.assign(d, d + (static_cast<std::size_t>(ww) * hh * 4));
    return r;
}
}  // namespace

AURORA_TEST() {
    // LinearFade（顶不透明 → 底淡出）：顶部行红 ~255，底部行明显变暗
    {
        const auto sb = std::make_shared<SolidBox>();
        sb->modifier.set(Modifier{}.shader_mask(ShaderMaskKind::LinearFade, 1.0F));
        const auto r = render_in_root(sb, 200, 200);
        const std::uint8_t top_red = r.at(30, 0, 0);
        const std::uint8_t bottom_red = r.at(30, 59, 0);
        AURORA_TEST_CHECK_MSG(top_red >= 250, "LinearFade top row stays bright red");
        AURORA_TEST_CHECK_MSG(bottom_red < top_red, "LinearFade bottom row darker than top");
        AURORA_TEST_CHECK_MSG(bottom_red < 30, "LinearFade bottom row strongly faded");
    }
    // LinearRise（顶透明 → 底不透明）：与 LinearFade 相反
    {
        const auto sb = std::make_shared<SolidBox>();
        sb->modifier.set(Modifier{}.shader_mask(ShaderMaskKind::LinearRise, 1.0F));
        const auto r = render_in_root(sb, 200, 200);
        const std::uint8_t top_red = r.at(30, 0, 0);
        const std::uint8_t bottom_red = r.at(30, 59, 0);
        AURORA_TEST_CHECK_MSG(bottom_red >= 250, "LinearRise bottom row stays bright red");
        AURORA_TEST_CHECK_MSG(top_red < bottom_red, "LinearRise top row darker than bottom");
        AURORA_TEST_CHECK_MSG(top_red < 30, "LinearRise top row strongly faded");
    }
    // RadialFade：中心亮，边缘暗
    {
        const auto sb = std::make_shared<SolidBox>();
        sb->modifier.set(Modifier{}.shader_mask(ShaderMaskKind::RadialFade, 1.0F));
        const auto r = render_in_root(sb, 200, 200);
        const std::uint8_t center_red = r.at(30, 30, 0);
        const std::uint8_t corner_red = r.at(0, 0, 0);
        AURORA_TEST_CHECK_MSG(center_red > corner_red, "RadialFade center brighter than corner");
        AURORA_TEST_CHECK_MSG(center_red >= 200, "RadialFade center stays bright");
    }
    // strength=0：不改变
    {
        const auto sb = std::make_shared<SolidBox>();
        sb->modifier.set(Modifier{}.shader_mask(ShaderMaskKind::LinearFade, 0.0F));
        const auto r = render_in_root(sb, 200, 200);
        AURORA_TEST_CHECK_MSG(r.at(30, 59, 0) == 255, "strength 0 leaves bottom row unchanged");
    }
}