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
#include "aurora/modifier/modifier.h"
#include "aurora/render/painter.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_overflow_strategy {

AURORA_TEST_CASE(overflow_strategy_json_roundtrip_and_fallback) {
    // to_json：每个枚举值输出同名标准字符串。
    AURORA_TEST_CHECK_EQ(overflow_strategy_to_json(OverflowStrategy::Visible).get<std::string>(), std::string{"Visible"});
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
    const Constraints cc{.min = Size{.width = 0.0F, .height = 0.0F},
                         .max = Size{.width = 100.0F, .height = 50.0F}};
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

}  // namespace aurora::test_cases::itest_overflow_strategy
