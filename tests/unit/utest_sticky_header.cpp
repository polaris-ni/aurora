/// 测试类型: unit
/// 目标单元: include/aurora/widget/sticky_header.h
/// 测试说明: 吸顶头部——is_sticky_header 虚钩子（默认 false / StickyHeader 覆写 true）、自描述与
/// 无属性序列化、布局语义（交叉轴撑满 + 主轴取子项自然高）、Scroll 覆盖层钉驻像素观测
/// （滚过头顶仍驻顶部、后到头部把前一个顶出视口、离开视口后不再压顶）、
/// LazyList 虚拟化窗口内的同序钉驻

#include <memory>
#include <string>
#include <vector>

#include "aurora/layout/layout_engine.h"
#include "aurora/widget/lazy_list.h"
#include "aurora/widget/scroll.h"
#include "aurora/widget/sticky_header.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_sticky_header {

namespace {

constexpr Color AURORA_GREEN{0, 255, 0, 255};
constexpr Color AURORA_RED{255, 0, 0, 255};
constexpr Color AURORA_BLUE{0, 0, 255, 255};
constexpr Color AURORA_YELLOW{255, 255, 0, 255};

/// 纯色哑控件：整盒填充给定颜色，自然高由构造给定、宽取约束上限。
class ColorBox final : public Widget {
  public:
    ColorBox(float h, Color color) : h_(h), color_(color) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "ColorBox"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = c.max.width, .height = h_});
    }
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        p.fill_rect(bounds, color_);
    }

  private:
    float h_;
    Color color_;
};

/// 纵向堆叠容器：子项按自然高依次下排，宽度撑满（仅提供测试所需的确定布局语义）。
class ColumnBox final : public Container {
  public:
    [[nodiscard]] auto type_name() const -> const char * override { return "ColumnBox"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        float y = 0.0F;
        for (Node &ch : children_) {
            const Constraints inner{.min = Size{.width = c.max.width, .height = 0.0F},
                                    .max = Size{.width = c.max.width, .height = Size::infinity().height}};
            const Size cs = ch.widget().layout(inner, ctx);
            ch.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = y}, .size = cs});
            y += cs.height;
        }
        size_ = c.constrain(Size{.width = c.max.width, .height = y});
        return size_;
    }
};

auto box(float h, Color color) -> Node { return Node{std::make_shared<ColorBox>(h, color)}; }

auto header(float h, Color color) -> Node { return Node{std::make_shared<StickyHeader>(box(h, color))}; }

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

constexpr BuildContext AURORA_CTX;

auto viewport(float h) -> Rect { return Rect{.origin = Point{}, .size = Size{.width = 200.0F, .height = h}}; }

/// 用 Painter 起一帧并绘制 root，返回取色器（闭包内持有 Painter 生命期）。
struct Canvas {
    Painter p;
    Canvas(int w, int h) { p.begin(w, h); }

    [[nodiscard]] auto pixel(int x, int y) const -> Color { return p.get_pixel(x, y); }
};

/// 是否命中目标色（容 1dp AA 边界的宽差：色带内插值不影响整数填充）。
auto same_color(const Color &a, const Color &b) -> bool { return a.r == b.r && a.g == b.g && a.b == b.b; }

}  // namespace

AURORA_TEST_CASE(sticky_hook_defaults_to_false_and_header_overrides) {
    ColorBox plain(30.0F, AURORA_RED);
    AURORA_TEST_CHECK_FALSE(plain.is_sticky_header());

    StickyHeader h{box(30.0F, AURORA_GREEN)};
    AURORA_TEST_CHECK_TRUE(h.is_sticky_header());
    AURORA_TEST_CHECK_EQ(std::string{h.type_name()}, std::string{"StickyHeader"});

    const auto desc = StickyHeader::describe_static();
    AURORA_TEST_CHECK_EQ(desc.name, std::string{"StickyHeader"});
    AURORA_TEST_CHECK_EQ(desc.children_policy, std::string{"single"});
    AURORA_TEST_CHECK_TRUE(desc.properties.empty());
    AURORA_TEST_CHECK_EQ(h.describe().name, std::string{"StickyHeader"});

    // 无自有属性：序列化产物不含吸顶相关键，反序列化亦不引入状态
    Json props;
    h.serialize_props(props);
    AURORA_TEST_CHECK_FALSE(props.contains("sticky"));
    StickyHeader dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_TRUE(dst.is_sticky_header());
}

AURORA_TEST_CASE(sticky_header_layout_spans_cross_axis_and_adopts_child_height) {
    auto kid = std::make_shared<ColorBox>(30.0F, AURORA_GREEN);
    StickyHeader h{Node{kid}};
    const Size s = h.layout(bounded(200.0F, 100.0F), AURORA_CTX);
    AURORA_TEST_CHECK_NEAR(s.width, 200.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.height, 30.0F, 1e-4F);  // 主轴取子项自然高，不被父约束撑满

    // 无子项：退化为主轴缺省高（48）× 交叉轴撑满
    StickyHeader empty;
    const Size e = empty.layout(Constraints{}, AURORA_CTX);
    AURORA_TEST_CHECK_NEAR(e.width, 320.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(e.height, 48.0F, 1e-4F);
}

AURORA_TEST_CASE(scroll_pins_scrolled_past_header_at_top) {
    auto column = std::make_shared<ColumnBox>();
    column->add(header(40.0F, AURORA_GREEN));  // 内容 0..40
    column->add(box(200.0F, AURORA_RED));  // 40..240
    column->add(header(40.0F, AURORA_BLUE));  // 240..280
    column->add(box(200.0F, AURORA_YELLOW));  // 280..480
    Scroll s;
    s.add(Node{column});
    LayoutEngine::layout(s, bounded(200.0F, 120.0F));  // 视口 120 → 可滚 360

    {
        Canvas c{200, 120};
        s.paint(c.p, viewport(120.0F), AURORA_CTX);
        AURORA_TEST_CHECK_TRUE(same_color(c.pixel(10, 10), AURORA_GREEN));  // 未滚动：头部在自然位
        AURORA_TEST_CHECK_TRUE(same_color(c.pixel(10, 80), AURORA_RED));
    }

    AURORA_TEST_CHECK_TRUE(s.set_offset(100.0F));
    {
        Canvas c{200, 120};
        s.paint(c.p, viewport(120.0F), AURORA_CTX);
        AURORA_TEST_CHECK_TRUE(same_color(c.pixel(10, 10), AURORA_GREEN));  // 滚过头顶：钉驻顶部
        AURORA_TEST_CHECK_TRUE(same_color(c.pixel(10, 80), AURORA_RED));  // 覆盖层不改其余内容
    }

    AURORA_TEST_CHECK_TRUE(s.set_offset(260.0F));
    {
        Canvas c{200, 120};
        s.paint(c.p, viewport(120.0F), AURORA_CTX);
        AURORA_TEST_CHECK_TRUE(same_color(c.pixel(10, 10), AURORA_BLUE));  // 第二个头部接管顶部
        AURORA_TEST_CHECK_TRUE(same_color(c.pixel(10, 80), AURORA_YELLOW));  // 首个头部已整条顶出
    }

    AURORA_TEST_CHECK_TRUE(s.set_offset(360.0F));  // 末端：最后一个头部延伸到底、无对手
    {
        Canvas c{200, 120};
        s.paint(c.p, viewport(120.0F), AURORA_CTX);
        AURORA_TEST_CHECK_TRUE(same_color(c.pixel(10, 10), AURORA_BLUE));
        AURORA_TEST_CHECK_TRUE(same_color(c.pixel(10, 100), AURORA_YELLOW));
    }
}

AURORA_TEST_CASE(scroll_pushes_previous_header_out_while_next_arrives) {
    auto column = std::make_shared<ColumnBox>();
    column->add(header(40.0F, AURORA_GREEN));
    column->add(box(200.0F, AURORA_RED));
    column->add(header(40.0F, AURORA_BLUE));
    column->add(box(200.0F, AURORA_YELLOW));
    Scroll s;
    s.add(Node{column});
    LayoutEngine::layout(s, bounded(200.0F, 120.0F));

    // 第二个头部视口顶部 = 240-220 = 20 < 首个头部高 40 → 首个被顶到 pin=-20，仅露出下沿 0..20
    AURORA_TEST_CHECK_TRUE(s.set_offset(220.0F));
    Canvas c{200, 120};
    s.paint(c.p, viewport(120.0F), AURORA_CTX);
    AURORA_TEST_CHECK_TRUE(same_color(c.pixel(10, 10), AURORA_GREEN));  // 顶出途中：绿仍占上部
    AURORA_TEST_CHECK_TRUE(same_color(c.pixel(10, 30), AURORA_BLUE));  // 蓝在自身自然位（20..60）
    AURORA_TEST_CHECK_TRUE(same_color(c.pixel(10, 90), AURORA_YELLOW));
}

AURORA_TEST_CASE(scroll_without_header_has_no_overlay_cost) {
    auto column = std::make_shared<ColumnBox>();
    column->add(box(40.0F, AURORA_GREEN));
    column->add(box(400.0F, AURORA_RED));
    Scroll s;
    s.add(Node{column});
    LayoutEngine::layout(s, bounded(200.0F, 120.0F));
    AURORA_TEST_CHECK_TRUE(s.set_offset(60.0F));

    Canvas c{200, 120};
    s.paint(c.p, viewport(120.0F), AURORA_CTX);
    AURORA_TEST_CHECK_TRUE(same_color(c.pixel(10, 10), AURORA_RED));  // 无 sticky → 顶部即滚动内容
}

AURORA_TEST_CASE(lazy_list_pins_sticky_rows_over_scrolled_content) {
    auto list = std::make_shared<LazyList>(
        10,
        [](int index) -> Node {
            if (index == 0) {
                return header(40.0F, AURORA_BLUE);
            }
            if (index == 5) {
                return header(40.0F, AURORA_GREEN);
            }
            return box(40.0F, AURORA_YELLOW);
        },
        40.0F);
    LayoutEngine::layout(*list, bounded(200.0F, 120.0F));  // 内容 400 → 可滚 280

    list->set_scroll_offset(60.0F);  // 条目 0 视口 y=-60 → 钉驻
    LayoutEngine::layout(*list, bounded(200.0F, 120.0F));
    {
        Canvas c{200, 120};
        list->paint(c.p, viewport(120.0F), AURORA_CTX);
        AURORA_TEST_CHECK_TRUE(same_color(c.pixel(10, 10), AURORA_BLUE));  // 首个 sticky 压顶
        AURORA_TEST_CHECK_TRUE(same_color(c.pixel(10, 80), AURORA_YELLOW));  // 覆盖层之外为普通行
    }

    list->set_scroll_offset(250.0F);  // 条目 5（y=-50）接管顶部，条目 0 整条被顶出
    LayoutEngine::layout(*list, bounded(200.0F, 120.0F));
    {
        Canvas c{200, 120};
        list->paint(c.p, viewport(120.0F), AURORA_CTX);
        AURORA_TEST_CHECK_TRUE(same_color(c.pixel(10, 10), AURORA_GREEN));
        AURORA_TEST_CHECK_TRUE(same_color(c.pixel(10, 80), AURORA_YELLOW));
    }
}

}  // namespace aurora::test_cases::utest_sticky_header
