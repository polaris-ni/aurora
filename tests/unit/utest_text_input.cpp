/// 测试类型: unit
/// 目标单元: include/aurora/widget/text_input.h
/// 测试说明: 覆盖 TextInput——Props 构造与链式 setter、只读/限长/禁用状态、布局尺寸与字号关系、
/// 经公开文本输入入口验证 on_changed 回调与截断/吞输入行为、IME 组合输入（preedit 显示 /
/// 上屏落字 / 限长 / 失焦取消 / 参与测量）、序列化往返与默认键省略

#include <cstddef>
#include <string>

#include "aurora/layout/layout_engine.h"
#include "aurora/widget/text_input.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_text_input {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(props_constructor_sets_initial_state) {
    const TextInputProps props{.value = "init", .placeholder = "ph", .font_size = 16.0F};
    TextInput ti{props};
    AURORA_TEST_CHECK_EQ(ti.value(), std::string{"init"});

    Json out;
    ti.serialize_props(out);
    AURORA_TEST_CHECK_EQ(out["value"].get<std::string>(), "init");
    AURORA_TEST_CHECK_EQ(out["placeholder"].get<std::string>(), "ph");
    AURORA_TEST_CHECK_NEAR(out["font_size"].get<float>(), 16.0F, 1e-4F);
}

AURORA_TEST_CASE(chained_setters_update_serialized_props) {
    TextInput ti;
    ti.set_value("v").set_placeholder("p").font_size(20.0F);
    AURORA_TEST_CHECK_EQ(ti.value(), std::string{"v"});

    Json out;
    ti.serialize_props(out);
    AURORA_TEST_CHECK_EQ(out["value"].get<std::string>(), "v");
    AURORA_TEST_CHECK_EQ(out["placeholder"].get<std::string>(), "p");
    AURORA_TEST_CHECK_NEAR(out["font_size"].get<float>(), 20.0F, 1e-4F);

    // 再次 setter 覆盖旧值。
    ti.set_value("w");
    AURORA_TEST_CHECK_EQ(ti.value(), std::string{"w"});
}

AURORA_TEST_CASE(read_only_flag_state_and_serialization) {
    TextInput ti;
    AURORA_TEST_CHECK_FALSE(ti.read_only());
    ti.set_read_only(true);
    AURORA_TEST_CHECK_TRUE(ti.read_only());

    Json out;
    ti.serialize_props(out);
    AURORA_TEST_CHECK_TRUE(out.contains("read_only"));
    AURORA_TEST_CHECK_EQ(out["read_only"].get<bool>(), true);

    // 默认只读=false 时不落盘（键省略语义）。
    Json defaults;
    TextInput def;
    def.serialize_props(defaults);
    AURORA_TEST_CHECK_FALSE(defaults.contains("read_only"));
}

AURORA_TEST_CASE(layout_uses_finite_width_and_pads_height) {
    // 有限约束宽度直接采用；高度至少含默认 24px 垂直内边距。
    TextInput ti;
    LayoutEngine::layout(ti, bounded(300.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(ti.size().width, 300.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(ti.size().height > 24.0F);

    // 字号更大 → 文本度量更高（相对断言，不依赖具体字形宽度）。
    TextInput small;
    small.font_size(10.0F);
    LayoutEngine::layout(small, bounded(300.0F, 300.0F));
    TextInput big;
    big.font_size(30.0F);
    LayoutEngine::layout(big, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_TRUE(big.size().height > small.size().height);
}

AURORA_TEST_CASE(text_input_entry_fires_on_changed) {
    // 经公开文本输入入口（on_text_input）直接驱动：焦点经公开通知入口 on_focus_change
    // 置位，不经 FocusManager/事件派发器，纯状态验证。
    TextInput ti;
    int fired = 0;
    std::string last;
    ti.set_on_changed([&fired, &last](const std::string& v) -> void {
        ++fired;
        last = v;
    });
    ti.on_focus_change(true);

    TextInputEvent first;
    first.text = "ab";
    ti.on_text_input(first);
    AURORA_TEST_CHECK_TRUE(first.is_handled);
    AURORA_TEST_CHECK_EQ(ti.value(), std::string{"ab"});
    AURORA_TEST_CHECK_EQ(fired, 1);
    AURORA_TEST_CHECK_EQ(last, std::string{"ab"});

    TextInputEvent second;
    second.text = "cd";
    ti.on_text_input(second);
    AURORA_TEST_CHECK_EQ(ti.value(), std::string{"abcd"});
    AURORA_TEST_CHECK_EQ(fired, 2);
    AURORA_TEST_CHECK_EQ(last, std::string{"abcd"});
}

AURORA_TEST_CASE(max_length_truncates_and_swallows_overflow) {
    // max_length 按码点计数：超额部分截断；无额度时吞输入且不触发回调。
    TextInput ti;
    ti.set_max_length(3);
    ti.on_focus_change(true);
    int fired = 0;
    ti.set_on_changed([&fired](const std::string&) -> void { ++fired; });

    TextInputEvent overflowed;
    overflowed.text = "abcd";
    ti.on_text_input(overflowed);
    AURORA_TEST_CHECK_TRUE(overflowed.is_handled);
    AURORA_TEST_CHECK_EQ(ti.value(), std::string{"abc"});
    AURORA_TEST_CHECK_EQ(fired, 1);

    TextInputEvent no_room;
    no_room.text = "x";
    ti.on_text_input(no_room);
    AURORA_TEST_CHECK_TRUE(no_room.is_handled);
    AURORA_TEST_CHECK_EQ(ti.value(), std::string{"abc"});
    AURORA_TEST_CHECK_EQ(fired, 1);  // 无额度：值与回调均不变
}

AURORA_TEST_CASE(read_only_and_disabled_swallow_text_input) {
    // 只读：吞输入并消费事件，不落字、不回调。
    TextInput ro;
    ro.set_read_only(true);
    ro.on_focus_change(true);
    int ro_fired = 0;
    ro.set_on_changed([&ro_fired](const std::string&) -> void { ++ro_fired; });
    TextInputEvent ro_event;
    ro_event.text = "x";
    ro.on_text_input(ro_event);
    AURORA_TEST_CHECK_TRUE(ro_event.is_handled);
    AURORA_TEST_CHECK_EQ(ro.value(), std::string{});
    AURORA_TEST_CHECK_EQ(ro_fired, 0);

    // 禁用：入口直接返回，连事件都不消费。
    TextInput disabled;
    disabled.set_enabled(false);
    disabled.on_focus_change(true);
    TextInputEvent disabled_event;
    disabled_event.text = "x";
    disabled.on_text_input(disabled_event);
    AURORA_TEST_CHECK_FALSE(disabled_event.is_handled);
    AURORA_TEST_CHECK_EQ(disabled.value(), std::string{});
}

AURORA_TEST_CASE(composition_shows_preedit_without_touching_value) {
    // 拼音输入的完整三段序列：preedit → 候选替换 → 上屏。
    // 关键不变量：preedit 期间 value() 保持干净（golden 可复现），仅上屏时落字。
    TextInput ti;
    ti.on_focus_change(true);

    TextCompositionEvent typing;
    typing.preedit = "nihao";
    typing.cursor_index = 5;
    ti.on_text_composition(typing);
    AURORA_TEST_CHECK_TRUE(typing.is_handled);
    AURORA_TEST_CHECK_TRUE(ti.is_composing());
    AURORA_TEST_CHECK_EQ(ti.preedit(), std::string{"nihao"});
    AURORA_TEST_CHECK_EQ(ti.value(), std::string{""});  // 未上屏，值不变
    AURORA_TEST_CHECK_EQ(ti.composition_cursor(), static_cast<std::size_t>(5));
    AURORA_TEST_CHECK_EQ(ti.accessibility_value(), std::string{"nihao"});  // 读屏需播报组合态

    TextCompositionEvent candidate;
    candidate.preedit = "你好";
    candidate.cursor_index = 2;
    ti.on_text_composition(candidate);
    AURORA_TEST_CHECK_EQ(ti.preedit(), std::string{"你好"});
    AURORA_TEST_CHECK_EQ(ti.value(), std::string{""});

    TextCompositionEvent commit;
    commit.committed = "你好";
    ti.on_text_composition(commit);
    AURORA_TEST_CHECK_FALSE(ti.is_composing());
    AURORA_TEST_CHECK_EQ(ti.preedit(), std::string{""});
    AURORA_TEST_CHECK_EQ(ti.value(), std::string{"你好"});
    AURORA_TEST_CHECK_EQ(ti.accessibility_value(), std::string{"你好"});
}

AURORA_TEST_CASE(composition_commit_respects_max_length_read_only_and_disabled) {
    // 上屏文本走与普通输入同一条 `insert_at_caret`，故限长语义自动一致。
    TextInput limited;
    limited.set_max_length(2);
    limited.on_focus_change(true);
    TextCompositionEvent over;
    over.committed = "abcdef";
    limited.on_text_composition(over);
    AURORA_TEST_CHECK_EQ(limited.value(), std::string{"ab"});

    // 只读：吞事件、不上屏、不显示 preedit
    TextInput ro;
    ro.set_value("keep").set_read_only(true);
    ro.on_focus_change(true);
    TextCompositionEvent blocked;
    blocked.preedit = "ni";
    blocked.committed = "x";
    ro.on_text_composition(blocked);
    AURORA_TEST_CHECK_TRUE(blocked.is_handled);
    AURORA_TEST_CHECK_FALSE(ro.is_composing());
    AURORA_TEST_CHECK_EQ(ro.value(), std::string{"keep"});

    // 禁用：与 TextInputEvent 一致，连事件都不消费
    TextInput disabled;
    disabled.set_enabled(false);
    disabled.on_focus_change(true);
    TextCompositionEvent ignored;
    ignored.preedit = "ni";
    disabled.on_text_composition(ignored);
    AURORA_TEST_CHECK_FALSE(ignored.is_handled);
    AURORA_TEST_CHECK_FALSE(disabled.is_composing());
}

AURORA_TEST_CASE(blur_cancels_pending_composition) {
    // 失焦即取消未上屏的组合（平台 IME 惯例），且取消不落字。
    TextInput ti;
    ti.on_focus_change(true);
    TextCompositionEvent composing;
    composing.preedit = "zhong";
    composing.sel_start = 0;
    composing.sel_end = 5;
    ti.on_text_composition(composing);
    AURORA_TEST_CHECK_TRUE(ti.is_composing());

    ti.on_focus_change(false);
    AURORA_TEST_CHECK_FALSE(ti.is_composing());
    AURORA_TEST_CHECK_EQ(ti.preedit(), std::string{""});
    AURORA_TEST_CHECK_EQ(ti.value(), std::string{""});
}

AURORA_TEST_CASE(preedit_participates_in_layout_measurement) {
    // preedit 参与宽度测量：组合中的中文应把输入框撑开（否则文字被裁切）。
    // 注意用无界约束测内在宽度——有界约束下 on_layout 恒取 c.max.width，观察不到内容差异。
    const Constraints loose{};  // max = 无限
    TextInput base;
    base.set_value("ab");
    LayoutEngine::layout(base, loose);

    TextInput composing;
    composing.set_value("ab");
    composing.on_focus_change(true);
    TextCompositionEvent e;
    e.preedit = "你好吗";
    composing.on_text_composition(e);
    LayoutEngine::layout(composing, loose);

    AURORA_TEST_CHECK_TRUE(composing.size().width > base.size().width);
    AURORA_TEST_CHECK_NEAR(composing.size().height, base.size().height, 1e-4F);  // 单行：高度不变
}

AURORA_TEST_CASE(serialize_deserialize_roundtrip_and_defaults) {
    TextInput src;
    src.set_value("user")
        .set_placeholder("type here")
        .font_size(18.0F)
        .set_background(Color(10, 20, 30, 40))
        .set_max_length(5)
        .set_read_only(true)
        .set_obscure_text(true)
        .set_focused_border_color(Color(1, 2, 3, 4));

    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["value"].get<std::string>(), "user");
    AURORA_TEST_CHECK_EQ(props["max_length"].get<int>(), 5);
    AURORA_TEST_CHECK_EQ(props["read_only"].get<bool>(), true);
    AURORA_TEST_CHECK_EQ(props["obscure_text"].get<bool>(), true);
    AURORA_TEST_CHECK_EQ(props["background"][0].get<int>(), 10);
    AURORA_TEST_CHECK_EQ(props["focused_border_color"][2].get<int>(), 3);

    TextInput dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(dst.value(), std::string{"user"});
    AURORA_TEST_CHECK_TRUE(dst.read_only());
    Json back;
    dst.serialize_props(back);
    AURORA_TEST_CHECK_EQ(back["placeholder"].get<std::string>(), "type here");
    AURORA_TEST_CHECK_NEAR(back["font_size"].get<float>(), 18.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(back["background"][3].get<int>(), 40);
    AURORA_TEST_CHECK_EQ(back["focused_border_color"][0].get<int>(), 1);

    // 负边框线宽被钳制为 1.0。
    TextInput clamped;
    clamped.set_border_width(-5.0F);
    Json clamp_props;
    clamped.serialize_props(clamp_props);
    AURORA_TEST_CHECK_NEAR(clamp_props["border_width"].get<float>(), 1.0F, 1e-4F);

    // 默认值省略语义：未设置的键不落盘。
    Json defaults;
    TextInput def;
    def.serialize_props(defaults);
    AURORA_TEST_CHECK_FALSE(defaults.contains("focused_border_color"));  // 保留「跟随主题」语义
    AURORA_TEST_CHECK_FALSE(defaults.contains("max_length"));
    AURORA_TEST_CHECK_FALSE(defaults.contains("obscure_text"));
}

}  // namespace aurora::test_cases::utest_text_input
