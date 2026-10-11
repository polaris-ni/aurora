/// 测试类型: unit
/// 目标单元: include/aurora/widget/breakpoint_builder.h
/// 测试说明: 断点感知容器。覆盖纯函数 resolve_breakpoint 的三档分支、静态/运行期自描述、默认与便捷构造、
/// 挂载订阅、on_layout 按宽度解析档位并构建/重建子节点（Compact/Medium/Expanded 三档 + 退化无限宽 → Compact）、
/// 卸载退订、paint/hit_test/hit_test_chain/tick_gestures 转发、child_nodes/for_each_child 视图。
/// 判据：三档断点均被解析；宽/窄/中/无限四种约束走通 on_layout 的全部分支；挂载后子节点视图非空。

#include <chrono>
#include <memory>
#include <vector>

#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/widget/breakpoint_builder.h"
#include "aurora/widget/widget.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_breakpoint_builder {

namespace {

/// @brief 固定尺寸叶控件，避免引入字体/绘制后端依赖。
class LeafProbe final : public Widget {
  public:
    int mounts = 0;
    int unmounts = 0;
    [[nodiscard]] auto type_name() const -> const char * override { return "LeafProbe"; }
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { (void)out; }
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

AURORA_TEST_CASE(resolve_breakpoint_three_bands) {
    AURORA_TEST_CHECK_EQ(resolve_breakpoint(100.0F, 600.0F, 840.0F), Breakpoint::Compact);
    AURORA_TEST_CHECK_EQ(resolve_breakpoint(700.0F, 600.0F, 840.0F), Breakpoint::Medium);
    AURORA_TEST_CHECK_EQ(resolve_breakpoint(900.0F, 600.0F, 840.0F), Breakpoint::Expanded);
}

AURORA_TEST_CASE(default_ctor_api_surface) {
    BreakpointBuilder def;
    AURORA_TEST_CHECK_EQ(std::string(def.type_name()), "BreakpointBuilder");
    AURORA_TEST_CHECK_EQ(def.current_breakpoint(), Breakpoint::Compact);
    AURORA_TEST_CHECK_EQ(def.can_cache_layout(), false);
    AURORA_TEST_CHECK_EQ(def.child_nodes().size(), 0U);
    def.for_each_child([](const Widget &) {});
    auto d = BreakpointBuilder::describe_static();
    AURORA_TEST_CHECK_EQ(d.name, "BreakpointBuilder");
    auto d2 = def.describe();
    AURORA_TEST_CHECK_EQ(d2.name, "BreakpointBuilder");
    std::vector<SignalViewBase *> sigs;
    def.collect_signals(sigs);
    AURORA_TEST_CHECK_EQ(sigs.size(), 1U);
}

AURORA_TEST_CASE(mount_layout_rebuild_unmount_covers_all_breakpoints) {
    auto bp = std::make_shared<BreakpointBuilder>(
        [](Breakpoint /*b*/) -> Node { return Node{std::make_shared<LeafProbe>()}; });
    AURORA_TEST_CHECK_EQ(bp->wants_focus(), false);

    BuildContext ctx;
    bp->mount(ctx);  // on_mount：订阅 builder 信号

    // 宽 → Expanded，并构建子节点。
    bp->layout(Constraints{.min = Size{.width = 0.0F, .height = 0.0F},
                           .max = Size{.width = 1000.0F, .height = 800.0F}},
                ctx);
    AURORA_TEST_CHECK_EQ(bp->current_breakpoint(), Breakpoint::Expanded);
    AURORA_TEST_CHECK_EQ(bp->child_nodes().size(), 1U);

    // 窄 → Compact（档位变化触发重建）。
    bp->layout(Constraints{.min = Size{.width = 0.0F, .height = 0.0F},
                           .max = Size{.width = 100.0F, .height = 200.0F}},
                ctx);
    AURORA_TEST_CHECK_EQ(bp->current_breakpoint(), Breakpoint::Compact);

    // 中 → Medium。
    bp->layout(Constraints{.min = Size{.width = 0.0F, .height = 0.0F},
                           .max = Size{.width = 700.0F, .height = 200.0F}},
                ctx);
    AURORA_TEST_CHECK_EQ(bp->current_breakpoint(), Breakpoint::Medium);

    // 同档位内再布局：不重建（复用子树），covering 约束不变分支。
    bp->layout(Constraints{.min = Size{.width = 0.0F, .height = 0.0F},
                           .max = Size{.width = 700.0F, .height = 200.0F}},
                ctx);
    AURORA_TEST_CHECK_EQ(bp->current_breakpoint(), Breakpoint::Medium);

    // 退化：父约束无限宽 → width 视为 0 → Compact。
    bp->layout(Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size::infinity()}, ctx);
    AURORA_TEST_CHECK_EQ(bp->current_breakpoint(), Breakpoint::Compact);

    // paint / hit_test / hit_test_chain / tick 转发。
    Painter painter;
    const Rect bounds{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 1000.0F, .height = 800.0F}};
    bp->paint(painter, bounds, ctx);
    (void)bp->hit_test(Point{.x = 5.0F, .y = 5.0F}, bounds, ctx);
    (void)bp->hit_test_chain(Point{.x = 5.0F, .y = 5.0F}, bounds, ctx);
    bp->tick(std::chrono::steady_clock::now());

    bp->unmount();  // on_unmount：退订 + 卸载子节点
    AURORA_TEST_CHECK_EQ(bp->child_nodes().size(), 1U);
}

}  // namespace aurora::test_cases::utest_breakpoint_builder
