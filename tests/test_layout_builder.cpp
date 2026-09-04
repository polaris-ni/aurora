// test_layout_builder.cpp — LayoutBuilder 响应式构建 1:1 测试：约束显著变化才重建子树。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

#include <functional>
#include <memory>
#include <string>

#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Constraints;
using aurora::LayoutBuilder;
using aurora::Node;
using aurora::Painter;
using aurora::Rect;
using aurora::SignalViewBase;
using aurora::Size;
using aurora::Widget;
using aurora::WidgetDescriptor;

namespace {
class FixedBox : public Widget {
  public:
    float w_ = 40.0F, h_ = 40.0F;
    explicit FixedBox(float w = 40.0F, float h = 40.0F) : w_(w), h_(h) {}
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

static void test_layout_builder_rebuild_on_constraint_change() {
    int builds = 0;
    const LayoutBuilder::BuilderFn fn = [&](const BuildContext &, const Constraints &c) -> Node {
        ++builds;
        // 约束宽度 >100 时返回大盒，否则返回小盒（用于验证重建触发）。
        if (c.max.width > 100.0F) {
            return Node{FixedBox{80.0F, 80.0F}};
        }
        return Node{FixedBox{40.0F, 40.0F}};
    };

    LayoutBuilder lb{fn};
    constexpr BuildContext ctx;
    lb.mount(ctx);

    constexpr Constraints wide{.min = Size{.width = 0, .height = 0},
                               .max = Size{.width = 200.0F, .height = Size::infinity().height}};
    const Size s1 = lb.layout(wide, ctx);
    AURORA_TEST_CHECK_MSG(near_f(s1.width, 80.0F) && near_f(s1.height, 80.0F),
                          "LayoutBuilder: wide constraint -> 80x80");
    AURORA_TEST_CHECK_MSG(builds == 1, "LayoutBuilder: built once initially");

    // 约束显著变化（宽 60）→ 触发重建，返回小盒。
    constexpr Constraints narrow{.min = Size{.width = 0, .height = 0},
                                 .max = Size{.width = 60.0F, .height = Size::infinity().height}};
    const Size s2 = lb.layout(narrow, ctx);
    AURORA_TEST_CHECK_MSG(near_f(s2.width, 40.0F) && near_f(s2.height, 40.0F),
                          "LayoutBuilder: narrow constraint -> 40x40");
    AURORA_TEST_CHECK_MSG(builds == 2, "LayoutBuilder: rebuilt on constraint change");

    // 相同约束再次 layout → 不重建（复用缓存子树）。
    lb.layout(narrow, ctx);
    AURORA_TEST_CHECK_MSG(builds == 2, "LayoutBuilder: no rebuild when constraints unchanged");

    // 空 builder（无闭包）→ 子树为空，尺寸 0。
    LayoutBuilder empty;
    empty.mount(ctx);
    const Size s3 = empty.layout(wide, ctx);
    AURORA_TEST_CHECK_MSG(near_f(s3.width, 0.0F) && near_f(s3.height, 0.0F),
                          "LayoutBuilder: null builder -> empty 0x0");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_layout_builder ===\n");
    test_layout_builder_rebuild_on_constraint_change();
}
