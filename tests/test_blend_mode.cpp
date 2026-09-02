// Modifier::blend_mode 验证：内容绘制完成后与 tint 按模式逐通道混合。
// 无头 layout + paint，像素断言，不依赖 GUI 后端。
// ── API 覆盖映射 ─────────────────────────────
// render/blend.h(BlendMode/ShaderMaskKind 枚举语义；像素混合一致性另见 test_simd_parity)。

#include <cstdint>
#include <memory>
#include <vector>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::BlendMode;
using aurora::BuildContext;
using aurora::Color;
using aurora::Constraints;
using aurora::LeafWidget;
using aurora::Modifier;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::SignalViewBase;
using aurora::Size;
using aurora::Stack;
using aurora::Widget;
using aurora::WidgetDescriptor;

namespace {

class SolidBox : public LeafWidget {
  public:
    Size sz{ .width = 100.0f, .height = 20.0f };
    Color color{ 255, 0, 0, 255 };
    void collect_signals(std::vector<SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "SolidBox"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{ .name = "SolidBox", .children_policy = "none" };
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { return c.constrain(sz); }
    void on_paint(Painter &p, const Rect &b, const BuildContext & /*ctx*/) override { p.fill_rect(b, color); }
};

struct RenderResult {
    std::vector<std::uint8_t> pixels;
    int w = 0, h = 0;
    [[nodiscard]] auto at(int x, const int y, int ch) const -> std::uint8_t {
        const std::size_t off = ((static_cast<std::size_t>(y) * w) + x) * 4;
        return pixels[off + ch];
    }
};

auto render_in_root(const std::shared_ptr<Widget> &w, const int ww, const int hh) -> RenderResult {
    auto const root = std::make_shared<Stack>(std::vector{ Node{ w } });
    constexpr BuildContext ctx;
    root->mount(ctx);
    Constraints c;
    c.min = Size{ .width = 0.0f, .height = 0.0f };
    c.max = Size{ .width = static_cast<float>(ww), .height = static_cast<float>(hh) };
    root->layout(c, ctx);
    Painter p;
    p.begin(ww, hh);
    root->paint(p,
                Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                      .size = Size{ .width = static_cast<float>(ww), .height = static_cast<float>(hh) } },
                ctx);
    const std::uint8_t *d = p.data();
    RenderResult r;
    r.w = ww;
    r.h = hh;
    r.pixels.assign(d, d + (static_cast<std::size_t>(ww) * hh * 4));
    return r;
}
} // namespace

AURORA_TEST() {
    // Multiply + 黑色 tint：src * 0 / 255 = 0（红变黑）
    {
        const auto sb = std::make_shared<SolidBox>();
        sb->modifier.set(Modifier{}.blend_mode(BlendMode::Multiply, Color::black()));
        const auto r = render_in_root(sb, 200, 200);
        AURORA_TEST_CHECK_MSG(r.at(50, 10, 0) == 0, "Multiply with black -> red channel 0");
        AURORA_TEST_CHECK_MSG(r.at(50, 10, 1) == 0 && r.at(50, 10, 2) == 0, "Multiply with black -> green/blue 0");
    }
    // Multiply + 白色 tint：src * 255 / 255 = src（不变）
    {
        const auto sb = std::make_shared<SolidBox>();
        sb->modifier.set(Modifier{}.blend_mode(BlendMode::Multiply, Color::white()));
        const auto r = render_in_root(sb, 200, 200);
        AURORA_TEST_CHECK_MSG(r.at(50, 10, 0) == 255, "Multiply with white leaves red 255");
    }
    // Normal + 绿色 tint：直接覆盖为绿
    {
        const auto sb = std::make_shared<SolidBox>();
        sb->modifier.set(Modifier{}.blend_mode(BlendMode::Normal, Color{ 0, 255, 0 }));
        const auto r = render_in_root(sb, 200, 200);
        AURORA_TEST_CHECK_MSG(r.at(50, 10, 1) == 255, "Normal with green -> green 255");
        AURORA_TEST_CHECK_MSG(r.at(50, 10, 0) == 0, "Normal with green -> red 0");
    }
    // strength=0：不改变
    {
        const auto sb = std::make_shared<SolidBox>();
        sb->modifier.set(Modifier{}.blend_mode(BlendMode::Multiply, Color::black(), 0.0f));
        const auto r = render_in_root(sb, 200, 200);
        AURORA_TEST_CHECK_MSG(r.at(50, 10, 0) == 255, "strength 0 leaves red unchanged");
    }
    // Darken：min(src, tint)；src=红(255,0,0), tint=蓝(0,0,255) -> (0,0,0)
    {
        const auto sb = std::make_shared<SolidBox>();
        sb->modifier.set(Modifier{}.blend_mode(BlendMode::Darken, Color::blue()));
        const auto r = render_in_root(sb, 200, 200);
        AURORA_TEST_CHECK_MSG(r.at(50, 10, 0) == 0, "Darken red vs blue tint -> red 0");
        AURORA_TEST_CHECK_MSG(r.at(50, 10, 2) == 0, "Darken red vs blue tint -> blue 0");
    }
}
