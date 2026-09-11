/// 测试类型: integration
/// 目标单元: include/aurora/core/enums.h + include/aurora/widget/widget.h（overflow 属性）
/// 测试说明: OverflowStrategy 枚举 JSON 序列化与未知值回退、Widget overflow 链式属性、
/// serialize_props/deserialize_props 往返、四种策略下溢出内容布局+绘制完整走通
/// （裁剪栈平衡，显式 Modifier 尺寸语义生效）

#include <memory>
#include <string>
#include <utility>

#include "aurora/core/color.h"
#include "aurora/core/enums.h"
#include "aurora/event/dispatcher.h"
#include "aurora/modifier/modifier.h"
#include "aurora/render/painter.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/scroll.h"
#include "aurora/widget/scroll_viewport.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_overflow_strategy {

AURORA_TEST_CASE(overflow_strategy_json_roundtrip_and_fallback) {
    // to_json：每个枚举值输出同名标准字符串。
    AURORA_TEST_CHECK_EQ(overflow_strategy_to_json(OverflowStrategy::Visible).get<std::string>(),
                         std::string{"Visible"});
    AURORA_TEST_CHECK_EQ(overflow_strategy_to_json(OverflowStrategy::Hidden).get<std::string>(), std::string{"Hidden"});
    AURORA_TEST_CHECK_EQ(overflow_strategy_to_json(OverflowStrategy::Clip).get<std::string>(), std::string{"Clip"});
    AURORA_TEST_CHECK_EQ(overflow_strategy_to_json(OverflowStrategy::Scroll).get<std::string>(), std::string{"Scroll"});

    // from_json：标准字符串还原。
    AURORA_TEST_CHECK_TRUE(json_to_overflow_strategy(Json("Visible")) == OverflowStrategy::Visible);
    AURORA_TEST_CHECK_TRUE(json_to_overflow_strategy(Json("Hidden")) == OverflowStrategy::Hidden);
    AURORA_TEST_CHECK_TRUE(json_to_overflow_strategy(Json("Clip")) == OverflowStrategy::Clip);
    AURORA_TEST_CHECK_TRUE(json_to_overflow_strategy(Json("Scroll")) == OverflowStrategy::Scroll);

    // 未知值 / 非字符串回退 Visible。
    AURORA_TEST_CHECK_TRUE(json_to_overflow_strategy(Json("unknown")) == OverflowStrategy::Visible);
    AURORA_TEST_CHECK_TRUE(json_to_overflow_strategy(Json(42)) == OverflowStrategy::Visible);
}

AURORA_TEST_CASE(widget_overflow_strategy_chainable) {
    Column col;
    AURORA_TEST_CHECK_TRUE(col.overflow_strategy() == OverflowStrategy::Visible);  // 默认值

    col.overflow_strategy(OverflowStrategy::Hidden);
    AURORA_TEST_CHECK_TRUE(col.overflow_strategy() == OverflowStrategy::Hidden);

    col.overflow_strategy(OverflowStrategy::Clip);
    AURORA_TEST_CHECK_TRUE(col.overflow_strategy() == OverflowStrategy::Clip);

    col.overflow_strategy(OverflowStrategy::Scroll);
    AURORA_TEST_CHECK_TRUE(col.overflow_strategy() == OverflowStrategy::Scroll);

    col.overflow_strategy(OverflowStrategy::Visible);
    AURORA_TEST_CHECK_TRUE(col.overflow_strategy() == OverflowStrategy::Visible);
}

AURORA_TEST_CASE(overflow_props_serialize_deserialize_roundtrip) {
    Column col;
    col.overflow_strategy(OverflowStrategy::Hidden);

    Json props;
    col.serialize_props(props);
    AURORA_TEST_CHECK_TRUE(props.contains("overflow"));
    AURORA_TEST_CHECK_EQ(props["overflow"].get<std::string>(), std::string{"Hidden"});

    // 反序列化到另一个 widget。
    Column col2;
    col2.deserialize_props(props);
    AURORA_TEST_CHECK_TRUE(col2.overflow_strategy() == OverflowStrategy::Hidden);
}

namespace {

/// 构造「固定 100x50 绿底容器 + 200x100 红底溢出子 Text」并走完 mount→layout→paint。
/// 返回容器最终布局尺寸（显式 Modifier 尺寸应无条件覆写为 100x50）。
auto paint_overflow_scenario(OverflowStrategy strategy) -> Size {
    auto txt = std::make_shared<Text>("XXXXXXXXXXXXXXXXXXXX");
    txt->modifier.set(Modifier{}.size(200.0F, 100.0F).background(Color{255, 0, 0, 255}));

    Column col{Node{std::move(txt)}};
    col.modifier.set(Modifier{}.size(100.0F, 50.0F).background(Color{0, 255, 0, 255}));
    col.overflow_strategy(strategy);

    const BuildContext ctx;
    col.mount(ctx);
    const Constraints cc{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 100.0F, .height = 50.0F}};
    col.layout(cc, ctx);

    Painter p;
    p.begin(100, 50);
    col.paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 100.0F, .height = 50.0F}}, ctx);
    return col.size();
}

}  // namespace

AURORA_TEST_CASE(paint_completes_for_every_overflow_strategy) {
    // Visible / Hidden / Clip / Scroll 四种策略：布局 + 绘制完整走通不崩溃（裁剪栈平衡），
    // 且显式 Modifier 尺寸在所有策略下都无条件覆写最终尺寸（100x50）。
    const Size sv = paint_overflow_scenario(OverflowStrategy::Visible);
    AURORA_TEST_CHECK_NEAR(sv.width, 100.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(sv.height, 50.0F, 1e-3F);

    const Size sh = paint_overflow_scenario(OverflowStrategy::Hidden);
    AURORA_TEST_CHECK_NEAR(sh.width, 100.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(sh.height, 50.0F, 1e-3F);

    const Size sc = paint_overflow_scenario(OverflowStrategy::Clip);
    AURORA_TEST_CHECK_NEAR(sc.width, 100.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(sc.height, 50.0F, 1e-3F);

    const Size ss = paint_overflow_scenario(OverflowStrategy::Scroll);
    AURORA_TEST_CHECK_NEAR(ss.width, 100.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(ss.height, 50.0F, 1e-3F);
}

namespace {

/// 构造「100x50 绿底 Scroll 容器 + 100x100 红底溢出子 Text」并走完 mount→layout。
auto make_scroll_column() -> Column {
    auto txt = std::make_shared<Text>("XXXXXXXXXXXXXXXXXXXX");
    txt->modifier.set(Modifier{}.size(100.0F, 100.0F).background(Color{255, 0, 0, 255}));

    Column col{Node{std::move(txt)}};
    col.modifier.set(Modifier{}.size(100.0F, 50.0F).background(Color{0, 255, 0, 255}));
    col.overflow_strategy(OverflowStrategy::Scroll);

    const BuildContext ctx;
    col.mount(ctx);
    const Constraints cc{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 100.0F, .height = 50.0F}};
    col.layout(cc, ctx);
    return col;
}

}  // namespace

AURORA_TEST_CASE(overflow_scroll_wheel_clamps_to_content) {
    // D0b：OverflowStrategy::Scroll 获得真实滚动。夹取上限 = 内容高(100) − 视口高(50) = 50，
    // 符号约定与全库一致（delta_y 正方向为向上滚动 → offset 减小）。
    Column col = make_scroll_column();

    // 顶部向上滚：offset 已在 0，无变化。
    AURORA_TEST_CHECK_TRUE(col.scroll_by(3.0F) == false);
    AURORA_TEST_CHECK_NEAR(col.scroll_offset_y(), 0.0F, 1e-3F);

    // 向下滚 2 格 = 32px。
    AURORA_TEST_CHECK_TRUE(col.scroll_by(-2.0F) == true);
    AURORA_TEST_CHECK_NEAR(col.scroll_offset_y(), 32.0F, 1e-3F);

    // 大步向下滚：夹到最大 50；端点后再滚无变化。
    AURORA_TEST_CHECK_TRUE(col.scroll_by(-100.0F) == true);
    AURORA_TEST_CHECK_NEAR(col.scroll_offset_y(), 50.0F, 1e-3F);
    AURORA_TEST_CHECK_TRUE(col.scroll_by(-1.0F) == false);
    AURORA_TEST_CHECK_NEAR(col.scroll_offset_y(), 50.0F, 1e-3F);

    // 向上滚回顶部：夹到 0。
    AURORA_TEST_CHECK_TRUE(col.scroll_by(100.0F) == true);
    AURORA_TEST_CHECK_NEAR(col.scroll_offset_y(), 0.0F, 1e-3F);
}

AURORA_TEST_CASE(overflow_scroll_non_scroll_strategies_do_not_scroll) {
    // Hidden/Clip/Visible 不获得滚动能力：scroll_by 无变化（滚动是 Scroll 独有语义）。
    for (const OverflowStrategy s : {OverflowStrategy::Visible, OverflowStrategy::Hidden, OverflowStrategy::Clip}) {
        Column col = make_scroll_column();
        col.overflow_strategy(s);
        AURORA_TEST_CHECK_TRUE(col.wants_scroll() == false);
        AURORA_TEST_CHECK_TRUE(col.scroll_by(-2.0F) == false);
        AURORA_TEST_CHECK_NEAR(col.scroll_offset_y(), 0.0F, 1e-3F);
    }
}

AURORA_TEST_CASE(overflow_scroll_paint_translates_content) {
    // 滚动后内容上移平移：红(0..100)+绿(100..200)两段内容，视口 50 高。
    // 未滚动：视口内全红；滚 128px（clamp 150）后绿段平移到 -28..72 → 视口内变绿。
    auto red_txt = std::make_shared<Text>("red");
    red_txt->modifier.set(Modifier{}.size(100.0F, 100.0F).background(Color{255, 0, 0, 255}));
    auto green_txt = std::make_shared<Text>("green");
    green_txt->modifier.set(Modifier{}.size(100.0F, 100.0F).background(Color{0, 255, 0, 255}));

    Column col{Node{std::move(red_txt)}, Node{std::move(green_txt)}};
    col.modifier.set(Modifier{}.size(100.0F, 50.0F).background(Color{0, 0, 255, 255}));
    col.overflow_strategy(OverflowStrategy::Scroll);

    const BuildContext ctx;
    col.mount(ctx);
    const Constraints cc{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 100.0F, .height = 50.0F}};
    col.layout(cc, ctx);

    Painter p0;
    p0.begin(100, 50);
    col.paint(p0, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 100.0F, .height = 50.0F}}, ctx);
    AURORA_TEST_CHECK_EQ(p0.get_pixel(50, 25), Color{255, 0, 0, 255});  // 未滚动：视口内是红段

    AURORA_TEST_CHECK_TRUE(col.scroll_by(-8.0F) == true);  // 128 → 夹到 150（内容 200 − 视口 50）
    AURORA_TEST_CHECK_NEAR(col.scroll_offset_y(), 128.0F, 1e-3F);
    Painter p1;
    p1.begin(100, 50);
    col.paint(p1, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 100.0F, .height = 50.0F}}, ctx);
    AURORA_TEST_CHECK_EQ(p1.get_pixel(50, 25), Color{0, 255, 0, 255});  // 绿段平移进视口
}

AURORA_TEST_CASE(scroll_dispatch_nearest_scrollable_ancestor_wins) {
    // 滚轮路由（D0b）：沿命中链自最深向根找最近 wants_scroll 者。
    // 结构：外层 Column(overflow=Scroll, 100x120，内容 170 → 可滚 50)
    //       ├─ 内层真实 Scroll(100x100, 内容 300 高)
    //       ├─ 可点击 Text(100x40)（y∈[100,140]，非滚动目标，不拦截滚轮）
    //       └─ 撑高用 Text(100x30)（y∈[140,170]，超出视口供外层滚动）
    auto inner_txt = std::make_shared<Text>("inner");
    inner_txt->modifier.set(Modifier{}.size(100.0F, 300.0F));
    auto inner = std::make_shared<Scroll>(ScrollProps{.child = Node{std::move(inner_txt)}});
    inner->modifier.set(Modifier{}.size(100.0F, 100.0F));

    auto btn_txt = std::make_shared<Text>("btn");
    btn_txt->modifier.set(Modifier{}.size(100.0F, 40.0F).clickable([] {}));

    auto pad_txt = std::make_shared<Text>("pad");
    pad_txt->modifier.set(Modifier{}.size(100.0F, 30.0F));

    Column outer{Node{std::move(inner)}, Node{std::move(btn_txt)}, Node{std::move(pad_txt)}};
    outer.modifier.set(Modifier{}.size(100.0F, 120.0F));
    outer.overflow_strategy(OverflowStrategy::Scroll);

    const BuildContext ctx;
    outer.mount(ctx);
    const Constraints cc{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 100.0F, .height = 120.0F}};
    outer.layout(cc, ctx);

    // ① 滚轮落在内层真实 Scroll 上：内层滚动（最深滚动者优先），外层不动。
    ScrollEvent e1;
    e1.position = Point{.x = 50.0F, .y = 50.0F};
    e1.delta_y = -2.0F;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(outer, e1) == true);
    AURORA_TEST_CHECK_NEAR(outer.scroll_offset_y(), 0.0F, 1e-3F);
    // 内层 offset = 2 格 × step16 = 32（clamp 上限 300−100=200，未触及）。
    const Scroll &inner_ref = *static_cast<const Scroll *>(&outer.child(0).widget());
    AURORA_TEST_CHECK_NEAR(inner_ref.offset_y(), 32.0F, 1e-3F);

    // ② 滚轮落在可点击子项上（y∈[100,120] 视口内）：子项不拦截，滚轮归最近可滚动祖先（外层）。
    ScrollEvent e2;
    e2.position = Point{.x = 50.0F, .y = 110.0F};
    e2.delta_y = -1.0F;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(outer, e2) == true);
    AURORA_TEST_CHECK_NEAR(outer.scroll_offset_y(), 16.0F, 1e-3F);

    // ③ 再次滚轮（同一位置）：外层继续滚动（可滚动状态跨事件保持）。
    ScrollEvent e3;
    e3.position = Point{.x = 50.0F, .y = 110.0F};
    e3.delta_y = -1.0F;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(outer, e3) == true);
    AURORA_TEST_CHECK_NEAR(outer.scroll_offset_y(), 32.0F, 1e-3F);
}

AURORA_TEST_CASE(scroll_viewport_kernel_math) {
    // 内核单测：clamp 数学与符号约定（Scroll 组件与 Overflow::Scroll 共用）。
    ScrollViewport vp;
    vp.content_h = 100.0F;
    vp.viewport_h = 50.0F;
    vp.step = 16.0F;
    AURORA_TEST_CHECK_NEAR(vp.max_offset(), 50.0F, 1e-3F);
    AURORA_TEST_CHECK_TRUE(vp.apply_scroll(3.0F) == false);  // 顶部向上滚不动
    AURORA_TEST_CHECK_TRUE(vp.apply_scroll(-2.0F) == true);
    AURORA_TEST_CHECK_NEAR(vp.offset_y, 32.0F, 1e-3F);
    // 内容不足视口：max_offset=0，任何方向都不可滚。
    ScrollViewport short_vp;
    short_vp.content_h = 30.0F;
    short_vp.viewport_h = 50.0F;
    AURORA_TEST_CHECK_NEAR(short_vp.max_offset(), 0.0F, 1e-3F);
    AURORA_TEST_CHECK_TRUE(short_vp.apply_scroll(-10.0F) == false);
    // 静态夹取数学（Scroll::on_scroll 复用路径）。
    AURORA_TEST_CHECK_NEAR(ScrollViewport::clamp_offset(0.0F, -10.0F, 16.0F, 100.0F, 50.0F), 50.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(ScrollViewport::clamp_offset(50.0F, 10.0F, 16.0F, 100.0F, 50.0F), 0.0F, 1e-3F);
}

}  // namespace aurora::test_cases::itest_overflow_strategy
