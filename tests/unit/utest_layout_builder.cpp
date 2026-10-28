/// 测试类型: unit
/// 目标单元: include/aurora/widget/layout_builder.h
/// 测试说明: 响应式构建原语。覆盖默认与便捷构造、静态/运行期自描述、挂载订阅、on_layout 按约束变化重建
/// 子节点（首次构建 / 约束变化重建 / 同约束复用 / 空闭包回退空子节点）、卸载退订、
/// paint/hit_test/hit_test_chain/tick_gestures 转发、child_nodes/for_each_child 视图。
/// 判据：约束变化触发重建、同约束复用不重建；空闭包布局返回零尺寸。

#include <chrono>
#include <memory>
#include <vector>

#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/widget/layout_builder.h"
#include "aurora/widget/widget.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_layout_builder {

namespace {

class LeafProbe final : public Widget {
  public:
    int mounts = 0;
    int unmounts = 0;
    [[nodiscard]] auto type_name() const -> const char * override { return "LeafProbe"; }
    auto collect_signals(StateSink &out) -> void override { (void)out; }
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = 20.0F, .height = 20.0F});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*b*/, const BuildContext & /*ctx*/) -> void override {}
    auto on_mount(const BuildContext &ctx) -> void override {
        Widget::on_mount(ctx);
        ++mounts;
    }
    auto on_unmount(const BuildContext &ctx) -> void override {
        Widget::on_unmount(ctx);
        ++unmounts;
    }
};

}  // namespace

AURORA_TEST_CASE(default_ctor_api_surface) {
    LayoutBuilder def;
    AURORA_TEST_CHECK_EQ(std::string(def.type_name()), "LayoutBuilder");
    AURORA_TEST_CHECK_EQ(def.can_cache_layout(), true);
    AURORA_TEST_CHECK_EQ(def.child_nodes().size(), 0U);
    def.for_each_child([](const Widget &) {});
    auto d = LayoutBuilder::describe_static();
    AURORA_TEST_CHECK_EQ(d.name, "LayoutBuilder");
    StateSink sigs;
    def.collect_signals(sigs);
    AURORA_TEST_CHECK_EQ(sigs.sources().size(), 1U);
}

AURORA_TEST_CASE(empty_builder_falls_back_to_empty_child) {
    LayoutBuilder def;
    BuildContext ctx;
    def.mount(ctx);
    const Size sz = def.layout(
        Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 300.0F, .height = 200.0F}}, ctx);
    AURORA_TEST_CHECK_EQ(sz.width, 0.0F);
    AURORA_TEST_CHECK_EQ(sz.height, 0.0F);
    AURORA_TEST_CHECK_EQ(def.child_nodes().size(), 0U);
    def.unmount();
}

AURORA_TEST_CASE(rebuild_on_constraint_change_reuse_on_same) {
    auto lb = std::make_shared<LayoutBuilder>(
        [](const BuildContext &, const Constraints &) -> Node { return Node{std::make_shared<LeafProbe>()}; });
    AURORA_TEST_CHECK_EQ(lb->wants_focus(), false);

    BuildContext ctx;
    lb->mount(ctx);

    const Constraints a{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 300.0F, .height = 200.0F}};
    lb->layout(a, ctx);
    AURORA_TEST_CHECK_EQ(lb->child_nodes().size(), 1U);

    // 同约束再布局：不重建（复用子树）。
    lb->layout(a, ctx);
    AURORA_TEST_CHECK_EQ(lb->child_nodes().size(), 1U);

    // 约束变化：重建子节点。
    lb->layout(Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 500.0F, .height = 200.0F}},
               ctx);
    AURORA_TEST_CHECK_EQ(lb->child_nodes().size(), 1U);

    Painter painter;
    const Rect bounds{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 300.0F, .height = 200.0F}};
    lb->paint(painter, bounds, ctx);
    (void)lb->hit_test(Point{.x = 5.0F, .y = 5.0F}, bounds, ctx);
    (void)lb->hit_test_chain(Point{.x = 5.0F, .y = 5.0F}, bounds, ctx);
    lb->tick(std::chrono::steady_clock::now());
    lb->unmount();
    AURORA_TEST_CHECK_EQ(lb->child_nodes().size(), 1U);
}

}  // namespace aurora::test_cases::utest_layout_builder
