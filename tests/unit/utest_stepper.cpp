/// 测试类型: unit
/// 目标单元: include/aurora/widget/stepper.h
/// 测试说明: 覆盖 Stepper——构造状态与 is_last_step、next/prev 线性推进与首末边界、
/// 步骤 validate 拦截、末步触发 on_complete、越界 current 容错、底部按钮区指针命中
/// （Cancel/Next 区域划分）、current 序列化往返与 describe 元数据

#include <string>
#include <vector>

#include "aurora/widget/stepper.h"
#include "aurora/layout/layout_engine.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_stepper {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

auto three_steps() -> std::vector<StepperStep> {
    return {StepperStep{"Alpha"}, StepperStep{"Beta"}, StepperStep{"Gamma"}};
}

auto press_at(float x, float y) -> MouseEvent {
    MouseEvent e;
    e.action = MouseAction::Press;
    e.button = MouseButton::Left;
    e.local_position = Point{.x = x, .y = y};
    return e;
}

}  // namespace

AURORA_TEST_CASE(default_and_construction_state) {
    const Stepper empty;
    AURORA_TEST_CHECK_EQ(std::string{empty.type_name()}, "Stepper");
    AURORA_TEST_CHECK_TRUE(empty.steps().empty());
    AURORA_TEST_CHECK_EQ(empty.current(), 0);

    Stepper s{three_steps(), 1};
    AURORA_TEST_CHECK_EQ(s.steps().size(), 3U);
    AURORA_TEST_CHECK_EQ(s.steps()[1].label, "Beta");
    AURORA_TEST_CHECK_EQ(s.current(), 1);
    AURORA_TEST_CHECK_FALSE(s.is_last_step());
    AURORA_TEST_CHECK_TRUE(Stepper{three_steps(), 2}.is_last_step());
}

AURORA_TEST_CASE(next_and_prev_navigate_linearly) {
    Stepper s{three_steps(), 0};
    AURORA_TEST_CHECK_TRUE(s.next());
    AURORA_TEST_CHECK_EQ(s.current(), 1);
    AURORA_TEST_CHECK_TRUE(s.next());
    AURORA_TEST_CHECK_EQ(s.current(), 2);
    AURORA_TEST_CHECK_TRUE(s.is_last_step());

    AURORA_TEST_CHECK_FALSE(s.next());  // 末步不再推进
    AURORA_TEST_CHECK_EQ(s.current(), 2);

    AURORA_TEST_CHECK_TRUE(s.prev());
    AURORA_TEST_CHECK_EQ(s.current(), 1);
    AURORA_TEST_CHECK_TRUE(s.prev());
    AURORA_TEST_CHECK_EQ(s.current(), 0);
    AURORA_TEST_CHECK_FALSE(s.prev());  // 首步不再后退
    AURORA_TEST_CHECK_EQ(s.current(), 0);
}

AURORA_TEST_CASE(last_step_next_fires_complete) {
    Stepper s{three_steps(), 0};
    int completes = 0;
    s.set_on_complete([&completes] { ++completes; });

    AURORA_TEST_CHECK_TRUE(s.next());
    AURORA_TEST_CHECK_TRUE(s.next());
    AURORA_TEST_CHECK_EQ(completes, 0);   // 中途不触发
    AURORA_TEST_CHECK_FALSE(s.next());    // 末步再点 → 触发 complete 且返回 false
    AURORA_TEST_CHECK_EQ(completes, 1);
    AURORA_TEST_CHECK_EQ(s.current(), 2); // 仍停在末步
}

AURORA_TEST_CASE(validate_gate_blocks_advance) {
    std::vector<StepperStep> steps;
    steps.push_back(StepperStep{"locked", [] { return false; }});
    steps.push_back(StepperStep{"open"});

    Stepper s{steps, 0};
    AURORA_TEST_CHECK_FALSE(s.next());  // validate 失败：拦截推进
    AURORA_TEST_CHECK_EQ(s.current(), 0);

    Stepper pass{std::vector<StepperStep>{StepperStep{"ok", [] { return true; }}, StepperStep{"next"}}, 0};
    AURORA_TEST_CHECK_TRUE(pass.next());
    AURORA_TEST_CHECK_EQ(pass.current(), 1);
}

AURORA_TEST_CASE(out_of_range_current_is_inert) {
    Stepper s{three_steps(), 5};  // 越界起点：next 直接失败
    AURORA_TEST_CHECK_FALSE(s.next());
    AURORA_TEST_CHECK_EQ(s.current(), 5);

    Stepper neg{three_steps(), -1};  // 负起点：next/prev 均失败
    AURORA_TEST_CHECK_FALSE(neg.next());
    AURORA_TEST_CHECK_FALSE(neg.prev());
    AURORA_TEST_CHECK_EQ(neg.current(), -1);
}

AURORA_TEST_CASE(pointer_regions_trigger_cancel_and_next) {
    Stepper s{three_steps(), 0};
    int cancels = 0;
    s.set_on_cancel([&cancels] { ++cancels; });
    LayoutEngine::layout(s, bounded(400.0F, 600.0F));
    // 自然高度 = 步数 * 40 + 内容 120 + 按钮 44。
    AURORA_TEST_CHECK_NEAR(s.size().width, 400.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.size().height, 3.0F * 40.0F + 120.0F + 44.0F, 1e-4F);
    const float btn_y = s.size().height - 44.0F + 10.0F;  // 按钮区中段（> height - 44）

    MouseEvent cancel = press_at(50.0F, btn_y);  // 左下角 Cancel 区（x < 100）
    s.on_pointer_event(cancel);
    AURORA_TEST_CHECK_EQ(cancels, 1);
    AURORA_TEST_CHECK_TRUE(cancel.is_handled);
    AURORA_TEST_CHECK_EQ(s.current(), 0);  // Cancel 不推进

    MouseEvent next = press_at(350.0F, btn_y);  // 右下角 Next 区（x > width - 110）
    s.on_pointer_event(next);
    AURORA_TEST_CHECK_EQ(s.current(), 1);
    AURORA_TEST_CHECK_TRUE(next.is_handled);

    MouseEvent middle = press_at(200.0F, btn_y);  // 中部无按钮
    s.on_pointer_event(middle);
    AURORA_TEST_CHECK_EQ(s.current(), 1);
    AURORA_TEST_CHECK_FALSE(middle.is_handled);
}

AURORA_TEST_CASE(serialize_deserialize_roundtrip) {
    Stepper src{three_steps(), 2};
    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["current"].get<int>(), 2);
    AURORA_TEST_CHECK_EQ(props["step_count"].get<int>(), 3);  // 只读描述字段

    Stepper dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(dst.current(), 2);
    AURORA_TEST_CHECK_TRUE(dst.steps().empty());  // 步骤列表不随 JSON 重建，仅恢复 current
}

AURORA_TEST_CASE(describe_reports_metadata) {
    const auto d = Stepper::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Stepper");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    bool has_complete = false;
    bool has_cancel = false;
    for (const auto &e : d.events) {
        if (std::string{e} == "on_complete") {
            has_complete = true;
        }
        if (std::string{e} == "on_cancel") {
            has_cancel = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_complete);
    AURORA_TEST_CHECK_TRUE(has_cancel);

    bool has_step_count = false;
    for (const auto &p : d.properties) {
        if (std::string{p.name} == "step_count") {
            has_step_count = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_step_count);
}

}  // namespace aurora::test_cases::utest_stepper
