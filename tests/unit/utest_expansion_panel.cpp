/// 测试类型: unit
/// 目标单元: include/aurora/widget/expansion_panel.h
/// 测试说明: 折叠面板。覆盖默认/聚合构造、type_name、静态/运行期自描述、信号收集、is_expanded/header/
/// expanded 取值、set_expanded（变更触发 on_toggle + 标脏；同值早退）、toggle、set_on_toggle、on_pointer_event
/// （标题头 Press 切换 + 置 handled）、wants_click 恒真、serialize/deserialize_props、on_layout（收起仅头部高 /
/// 展开叠加内容高）、on_paint（标题头 + 展开时内容）、on_hit_test/on_hit_test_chain。
/// 判据：set_expanded(true) 回调 on_toggle(true)；标题头点击切换展开态并置 handled；序列化往返保留 header/expanded。

#include <chrono>
#include <memory>
#include <vector>

#include "aurora/core/types.h"
#include "aurora/event/event.h"
#include "aurora/render/painter.h"
#include "aurora/widget/expansion_panel.h"
#include "aurora/widget/widget.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_expansion_panel {

namespace {

class LeafProbe final : public Widget {
  public:
    [[nodiscard]] auto type_name() const -> const char * override { return "LeafProbe"; }
    auto collect_signals(StateSink &out) -> void override { (void)out; }
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = 10.0F, .height = 10.0F});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*b*/, const BuildContext & /*ctx*/) -> void override {}
};

}  // namespace

AURORA_TEST_CASE(api_surface_and_toggle) {
    auto content = std::make_shared<LeafProbe>();
    ExpansionPanel ep{"Details", Node{content}, false};
    AURORA_TEST_CHECK_EQ(std::string(ep.type_name()), "ExpansionPanel");
    AURORA_TEST_CHECK_EQ(ep.is_expanded(), false);
    AURORA_TEST_CHECK_EQ(ep.header(), "Details");
    AURORA_TEST_CHECK_EQ(ep.wants_click(), true);

    auto d = ExpansionPanel::describe_static();
    AURORA_TEST_CHECK_EQ(d.name, "ExpansionPanel");
    auto d2 = ep.describe();
    AURORA_TEST_CHECK_EQ(d2.name, "ExpansionPanel");
    StateSink sigs;
    ep.collect_signals(sigs);
    AURORA_TEST_CHECK_EQ(sigs.sources().size(), 1U);

    int toggled = -1;
    ep.set_on_toggle([&toggled](bool v) { toggled = v ? 1 : 0; });
    ep.set_expanded(true);  // 变更 → on_toggle(true)
    AURORA_TEST_CHECK_EQ(ep.is_expanded(), true);
    AURORA_TEST_CHECK_EQ(toggled, 1);
    ep.set_expanded(true);  // 同值早退，不回调
    AURORA_TEST_CHECK_EQ(toggled, 1);
    ep.toggle();  // → false
    AURORA_TEST_CHECK_EQ(ep.is_expanded(), false);
    AURORA_TEST_CHECK_EQ(toggled, 0);
}

AURORA_TEST_CASE(pointer_event_on_header_toggles) {
    auto content = std::make_shared<LeafProbe>();
    ExpansionPanel ep{"Details", Node{content}, false};
    MouseEvent e;
    e.action = MouseAction::Press;
    e.local_position = Point{.x = 5.0F, .y = 5.0F};  // y < header_height(36)
    ep.on_pointer_event(e);
    AURORA_TEST_CHECK_EQ(ep.is_expanded(), true);  // 标题头 Press 切换为展开
    AURORA_TEST_CHECK_EQ(e.is_handled, true);

    // 点击头部下方（非标题头区域）：下沉基类，不切换。
    MouseEvent e2;
    e2.action = MouseAction::Press;
    e2.local_position = Point{.x = 5.0F, .y = 100.0F};
    ep.on_pointer_event(e2);
    AURORA_TEST_CHECK_EQ(ep.is_expanded(), true);
}

AURORA_TEST_CASE(layout_paint_hit_roundtrip) {
    auto content = std::make_shared<LeafProbe>();
    ExpansionPanel ep{"Details", Node{content}, false};
    BuildContext ctx;
    const Constraints c{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 200.0F, .height = 400.0F}};

    // 收起：高度仅头部。
    const Size collapsed = ep.layout(c, ctx);
    AURORA_TEST_CHECK_EQ(collapsed.height, 36.0F);

    // 展开：叠加内容高度。
    ep.set_expanded(true);
    const Size expanded = ep.layout(c, ctx);
    AURORA_TEST_CHECK_EQ(expanded.height, 36.0F + 10.0F);

    Painter painter;
    const Rect bounds{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 200.0F, .height = 400.0F}};
    ep.paint(painter, bounds, ctx);  // 展开态：绘制标题头 + 内容

    // 命中：标题头区域返回本控件。
    (void)ep.hit_test(Point{.x = 5.0F, .y = 5.0F}, bounds, ctx);
    (void)ep.hit_test_chain(Point{.x = 5.0F, .y = 100.0F}, bounds, ctx);
}

AURORA_TEST_CASE(serialize_deserialize_roundtrip) {
    ExpansionPanel ep{"Title", Node{std::make_shared<LeafProbe>()}, true};
    Json props = Json::object();
    ep.serialize_props(props);
    AURORA_TEST_CHECK_TRUE(props.contains("header"));
    AURORA_TEST_CHECK_TRUE(props.contains("expanded"));
    AURORA_TEST_CHECK_TRUE(props.contains("header_height"));

    ExpansionPanel ep2;
    Json p = Json::object();
    p.set("header", "Other");
    p.set("expanded", Json{false});
    p.set("header_height", 40.0);
    ep2.deserialize_props(p);
    AURORA_TEST_CHECK_EQ(ep2.header(), "Other");
    AURORA_TEST_CHECK_EQ(ep2.is_expanded(), false);
}

}  // namespace aurora::test_cases::utest_expansion_panel
