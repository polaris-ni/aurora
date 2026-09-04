// test_grid.cpp — Grid 布局控件 1:1 测试：列数/间距/有界均分/序列化往返。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Constraints;
using aurora::Grid;
using aurora::Json;
using aurora::Painter;
using aurora::Rect;
using aurora::SignalViewBase;
using aurora::Size;
using aurora::Widget;
using aurora::WidgetDescriptor;

namespace {
// 确定性固定尺寸控件（不依赖字体/渲染），用于布局断言。
class FixedBox : public Widget {
  public:
    float w_ = 50.0F, h_ = 50.0F;
    FixedBox() = default;
    FixedBox(float w, float h) : w_(w), h_(h) {}
    void collect_signals(std::vector<SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "FixedBox"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "FixedBox", .children_policy = "none"};
    }

  protected:
    void on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) override {}

    auto on_layout(const Constraints & /*c*/, const BuildContext & /*ctx*/) -> Size override {
        return Size{.width = w_, .height = h_};
    }
};
}  // namespace

static void test_grid_columns() {
    Grid g = {};
    AURORA_TEST_CHECK_MSG(g.columns == 1, "Grid: default columns == 1");
    AURORA_TEST_CHECK_MSG(near_f(g.gap, 4.0F), "Grid: default gap == 4");

    g.set_columns(0);
    AURORA_TEST_CHECK_MSG(g.columns == 1, "Grid: set_columns(0) clamps to 1");
    g.set_columns(3);
    AURORA_TEST_CHECK_MSG(g.columns == 3, "Grid: set_columns(3) applied");
    g.set_gap(8.0F);
    AURORA_TEST_CHECK_MSG(near_f(g.gap, 8.0F), "Grid: set_gap applied");

    // 便捷构造：负值列数回退 1
    const Grid g2({FixedBox{}, FixedBox{}}, -2);
    AURORA_TEST_CHECK_MSG(g2.columns == 1, "Grid: negative columns ctor clamps to 1");
}

static void test_grid_layout_bounded() {
    // 4 个 50x50 子项，2 列，间距 8，父约束有界宽 200。
    Grid g({FixedBox{}, FixedBox{}, FixedBox{}, FixedBox{}}, 2, 8.0F);
    constexpr BuildContext ctx;
    g.mount(ctx);
    constexpr Constraints c{.min = Size{.width = 0, .height = 0},
                            .max = Size{.width = 200.0F, .height = Size::infinity().height}};
    const Size s = g.layout(c, ctx);
    // cell_w = (200 - 8) / 2 = 96；子项自然宽 50 → col_w=[50,50]；
    // total_w = 50 + 50 + 8 = 108；两行 total_h = 50 + 50 + 8 = 108。
    AURORA_TEST_CHECK_MSG(near_f(s.width, 108.0F), "Grid: bounded total width == 108");
    AURORA_TEST_CHECK_MSG(near_f(s.height, 108.0F), "Grid: bounded total height == 108");

    const auto kids = g.child_nodes();
    AURORA_TEST_CHECK_MSG(kids.size() == 4, "Grid: four children retained");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(near_f(kids[0].bounds().origin.x, 0.0F) && near_f(kids[0].bounds().origin.y, 0.0F),
                          "Grid: child0 at (0,0)");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(near_f(kids[1].bounds().origin.x, 58.0F) && near_f(kids[1].bounds().origin.y, 0.0F),
                          "Grid: child1 at (58,0)");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(near_f(kids[2].bounds().origin.x, 0.0F) && near_f(kids[2].bounds().origin.y, 58.0F),
                          "Grid: child2 at (0,58)");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(near_f(kids[3].bounds().origin.x, 58.0F) && near_f(kids[3].bounds().origin.y, 58.0F),
                          "Grid: child3 at (58,58)");
}

static void test_grid_serialize_roundtrip() {
    Grid g;
    g.columns = 3;
    g.gap = 10.0F;
    Json props;
    g.serialize_props(props);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(props["columns"].get<int>() == 3, "Grid: serialize_props writes columns");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(near_f(props["gap"].get<float>(), 10.0F), "Grid: serialize_props writes gap");

    Grid g2;
    g2.deserialize_props(props);
    AURORA_TEST_CHECK_MSG(g2.columns == 3, "Grid: deserialize_props restores columns");
    AURORA_TEST_CHECK_MSG(near_f(g2.gap, 10.0F), "Grid: deserialize_props restores gap");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_grid ===\n");
    test_grid_columns();
    test_grid_layout_bounded();
    test_grid_serialize_roundtrip();
}