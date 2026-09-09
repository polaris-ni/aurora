/// 测试类型: unit
/// 目标单元: include/aurora/widget/pickers.h
/// 测试说明: 覆盖 Date/TimeOfDay 值语义（合法性/闰年/字符串化）、DatePicker（选中与回调、
/// 翻月回卷、网格映射、指针交互）、TimePicker（回卷调节、四象限点击）、ColorPicker（默认/自定义
/// 色板、点击选色）的构造契约、属性读写、自描述、序列化往返与无头布局/绘制

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "aurora/environment/build_context.h"
#include "aurora/event/event.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/pickers.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_pickers {

namespace {

/// 挂载并按给定上限布局，返回测得尺寸（无头环境：BuildContext + 约束）。
auto laid_out(Widget &w, float max_w, float max_h) -> Size {
    BuildContext ctx;
    w.mount(ctx);
    const Constraints c{
        .min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = max_w, .height = max_h}};
    return w.layout(c, ctx);
}

/// 合成一次按下事件并派发给控件。
auto press(Widget &w, float x, float y) -> MouseEvent {
    MouseEvent e;
    e.action = MouseAction::Press;
    e.local_position = Point{.x = x, .y = y};
    w.on_pointer_event(e);
    return e;
}

}  // namespace

AURORA_TEST_CASE(date_time_value_semantics) {
    // Date 合法性与闰年规则
    AURORA_TEST_CHECK_TRUE((Date{.year = 2026, .month = 7, .day = 25}).is_valid());
    AURORA_TEST_CHECK_FALSE((Date{.year = 2026, .month = 13, .day = 1}).is_valid());
    AURORA_TEST_CHECK_FALSE((Date{.year = 2026, .month = 2, .day = 29}).is_valid());  // 平年
    AURORA_TEST_CHECK_TRUE((Date{.year = 2024, .month = 2, .day = 29}).is_valid());  // 4 年闰
    AURORA_TEST_CHECK_TRUE((Date{.year = 2000, .month = 2, .day = 29}).is_valid());  // 400 年闰
    AURORA_TEST_CHECK_FALSE((Date{.year = 2100, .month = 2, .day = 29}).is_valid());  // 百年非闰

    AURORA_TEST_CHECK_EQ(Date::days_in_month(2026, 4), 30);
    AURORA_TEST_CHECK_EQ(Date::days_in_month(2026, 2), 28);
    AURORA_TEST_CHECK_EQ(Date::days_in_month(2024, 2), 29);
    AURORA_TEST_CHECK_EQ(Date::days_in_month(2026, 0), 0);  // 越界月份
    AURORA_TEST_CHECK_EQ(Date::days_in_month(2026, 13), 0);

    AURORA_TEST_CHECK_EQ((Date{.year = 2026, .month = 7, .day = 25}).to_string(), "2026-07-25");
    AURORA_TEST_CHECK_EQ((Date{.year = 2026, .month = 3, .day = 9}).to_string(), "2026-03-09");  // 补零

    // TimeOfDay 值语义
    AURORA_TEST_CHECK_TRUE((TimeOfDay{.hour = 23, .minute = 59}).is_valid());
    AURORA_TEST_CHECK_FALSE((TimeOfDay{.hour = 24, .minute = 0}).is_valid());
    AURORA_TEST_CHECK_FALSE((TimeOfDay{.hour = 12, .minute = 60}).is_valid());
    AURORA_TEST_CHECK_FALSE((TimeOfDay{.hour = -1, .minute = 0}).is_valid());
    AURORA_TEST_CHECK_EQ((TimeOfDay{.hour = 23, .minute = 58}).to_string(), "23:58");
    AURORA_TEST_CHECK_EQ((TimeOfDay{.hour = 8, .minute = 5}).to_string(), "08:05");
}

AURORA_TEST_CASE(pickers_type_contract) {
    // 三个选择器均为 Widget 派生、不可复制（Widget 拷贝已删除）。
    static_assert(std::is_base_of_v<aurora::Widget, aurora::DatePicker>);
    static_assert(std::is_base_of_v<aurora::Widget, aurora::TimePicker>);
    static_assert(std::is_base_of_v<aurora::Widget, aurora::ColorPicker>);
    static_assert(!std::is_copy_constructible_v<aurora::DatePicker>);
    static_assert(!std::is_copy_constructible_v<aurora::TimePicker>);
    static_assert(!std::is_copy_constructible_v<aurora::ColorPicker>);

    DatePicker dp;
    TimePicker tp;
    ColorPicker cp;
    AURORA_TEST_CHECK_EQ(std::string{dp.type_name()}, "DatePicker");
    AURORA_TEST_CHECK_EQ(std::string{tp.type_name()}, "TimePicker");
    AURORA_TEST_CHECK_EQ(std::string{cp.type_name()}, "ColorPicker");

    // 三者均可点击、且各暴露一个信号。
    AURORA_TEST_CHECK_TRUE(dp.wants_click());
    AURORA_TEST_CHECK_TRUE(tp.wants_click());
    AURORA_TEST_CHECK_TRUE(cp.wants_click());

    std::vector<SignalViewBase *> out;
    dp.collect_signals(out);
    tp.collect_signals(out);
    cp.collect_signals(out);
    AURORA_TEST_CHECK_EQ(out.size(), 3U);
}

AURORA_TEST_CASE(date_picker_constructor_and_select) {
    // 合法初始值：选中与视图同步。
    DatePicker dp{Date{2026, 7, 25}};
    AURORA_TEST_CHECK_EQ(dp.selected_date(), (Date{2026, 7, 25}));
    AURORA_TEST_CHECK_EQ(dp.view_year(), 2026);
    AURORA_TEST_CHECK_EQ(dp.view_month(), 7);

    // 非法初始值：回退默认 {2026-01-01}。
    DatePicker bad{Date{2026, 2, 30}};
    AURORA_TEST_CHECK_EQ(bad.selected_date(), (Date{2026, 1, 1}));
    AURORA_TEST_CHECK_EQ(bad.view_year(), 2026);
    AURORA_TEST_CHECK_EQ(bad.view_month(), 1);

    // 选中合法日期：回调触发、视图跟随。
    int calls = 0;
    Date last{};
    dp.set_on_change([&calls, &last](Date d) { ++calls; last = d; });
    dp.select(Date{2026, 8, 1});
    AURORA_TEST_CHECK_EQ(calls, 1);
    AURORA_TEST_CHECK_EQ(last, (Date{2026, 8, 1}));
    AURORA_TEST_CHECK_EQ(dp.selected_date(), (Date{2026, 8, 1}));
    AURORA_TEST_CHECK_EQ(dp.view_month(), 8);

    // 同值重复选中：不回调。
    dp.select(Date{2026, 8, 1});
    AURORA_TEST_CHECK_EQ(calls, 1);

    // 非法日期：忽略。
    dp.select(Date{2026, 2, 30});
    AURORA_TEST_CHECK_EQ(calls, 1);
    AURORA_TEST_CHECK_EQ(dp.selected_date(), (Date{2026, 8, 1}));
}

AURORA_TEST_CASE(date_picker_month_navigation_wraps_year) {
    DatePicker dp{Date{2026, 12, 15}};
    dp.next_month();
    AURORA_TEST_CHECK_EQ(dp.view_year(), 2027);
    AURORA_TEST_CHECK_EQ(dp.view_month(), 1);
    dp.prev_month();
    AURORA_TEST_CHECK_EQ(dp.view_year(), 2026);
    AURORA_TEST_CHECK_EQ(dp.view_month(), 12);

    // 1 月前翻回卷到上一年 12 月。
    DatePicker jan{Date{2026, 1, 15}};
    jan.prev_month();
    AURORA_TEST_CHECK_EQ(jan.view_year(), 2025);
    AURORA_TEST_CHECK_EQ(jan.view_month(), 12);

    // 翻月只动视图：不改选中、不触发 on_change。
    int calls = 0;
    jan.set_on_change([&calls](Date) { ++calls; });
    jan.next_month();
    jan.next_month();
    AURORA_TEST_CHECK_EQ(calls, 0);
    AURORA_TEST_CHECK_EQ(jan.selected_date(), (Date{2026, 1, 15}));
}

AURORA_TEST_CASE(date_picker_grid_day_mapping) {
    // 2026-07-01 是周三（first_wd=3，周日为第 0 列）。
    DatePicker dp{Date{2026, 7, 25}};
    AURORA_TEST_CHECK_EQ(dp.grid_day(0, 3), 1);  // 第一行周三 = 1 号
    AURORA_TEST_CHECK_EQ(dp.grid_day(0, 2), 0);  // 周二为空格
    AURORA_TEST_CHECK_EQ(dp.grid_day(0, 0), 0);
    AURORA_TEST_CHECK_EQ(dp.grid_day(4, 5), 31);  // 31 号 = (3-1+31)=33 → 第 4 行第 5 列
    AURORA_TEST_CHECK_EQ(dp.grid_day(5, 6), 0);  // 超出 31 天的尾格为空

    // 闰年 2024-02：1 号是周四，共 29 天。
    DatePicker feb{Date{2024, 2, 29}};
    AURORA_TEST_CHECK_EQ(feb.grid_day(0, 4), 1);
    AURORA_TEST_CHECK_EQ(feb.grid_day(0, 3), 0);
    AURORA_TEST_CHECK_EQ(feb.grid_day(4, 4), 29);
    AURORA_TEST_CHECK_EQ(feb.grid_day(4, 5), 0);  // 平年多出的格子为空
}

AURORA_TEST_CASE(date_picker_layout_and_describe) {
    DatePicker dp{Date{2026, 7, 25}};
    const Size s = laid_out(dp, 400.0F, 400.0F);
    // 月历期望尺寸 224 x (32 头部 + 6*28 网格)。
    AURORA_TEST_CHECK_NEAR(s.width, 224.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(s.height, 200.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(dp.size().width, 224.0F, 1e-3F);

    const auto d = DatePicker::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "DatePicker");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    AURORA_TEST_REQUIRE_EQ(d.properties.size(), 3U);
    AURORA_TEST_CHECK_EQ(std::string{d.properties[0].name}, "year");
    AURORA_TEST_CHECK_EQ(std::string{d.properties[2].name}, "day");
    AURORA_TEST_REQUIRE_EQ(d.events.size(), 1U);
    AURORA_TEST_CHECK_EQ(std::string{d.events[0]}, "on_change");
    AURORA_TEST_CHECK_EQ(d.invariants.size(), 2U);
}

AURORA_TEST_CASE(date_picker_pointer_navigation_and_pick) {
    DatePicker dp{Date{2026, 7, 25}};
    laid_out(dp, 400.0F, 400.0F);  // 224 x 200

    // 头部左箭头（x<28, y<32）：上一月。
    MouseEvent e = press(dp, 10.0F, 10.0F);
    AURORA_TEST_CHECK_TRUE(e.is_handled);
    AURORA_TEST_CHECK_EQ(dp.view_month(), 6);

    // 头部右箭头（x>196）：下一月。
    e = press(dp, 210.0F, 10.0F);
    AURORA_TEST_CHECK_TRUE(e.is_handled);
    AURORA_TEST_CHECK_EQ(dp.view_month(), 7);

    // 网格点击 (col=3,row=0) → 1 号（2026-07-01 周三）。
    int calls = 0;
    dp.set_on_change([&calls](Date) { ++calls; });
    e = press(dp, 100.0F, 40.0F);
    AURORA_TEST_CHECK_TRUE(e.is_handled);
    AURORA_TEST_CHECK_EQ(calls, 1);
    AURORA_TEST_CHECK_EQ(dp.selected_date(), (Date{2026, 7, 1}));

    // 空格点击（7 月首格前三列）：不选中、不回调，但事件已消费。
    e = press(dp, 20.0F, 40.0F);
    AURORA_TEST_CHECK_TRUE(e.is_handled);
    AURORA_TEST_CHECK_EQ(calls, 1);
    AURORA_TEST_CHECK_EQ(dp.selected_date(), (Date{2026, 7, 1}));
}

AURORA_TEST_CASE(date_picker_json_roundtrip_and_invalid_guard) {
    DatePicker src{Date{2025, 3, 9}};
    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["year"].get<int>(), 2025);
    AURORA_TEST_CHECK_EQ(props["month"].get<int>(), 3);
    AURORA_TEST_CHECK_EQ(props["day"].get<int>(), 9);

    DatePicker dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(dst.selected_date(), (Date{2025, 3, 9}));
    AURORA_TEST_CHECK_EQ(dst.view_year(), 2025);
    AURORA_TEST_CHECK_EQ(dst.view_month(), 3);

    // 反序列化非法日期（month=13）：保持原选中不变。
    props["month"] = 13;
    DatePicker guard;
    guard.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(guard.selected_date(), (Date{2026, 1, 1}));
}

AURORA_TEST_CASE(time_picker_adjust_wraps) {
    TimePicker tp{TimeOfDay{23, 58}};
    AURORA_TEST_CHECK_EQ(tp.selected_time(), (TimeOfDay{23, 58}));

    tp.add_minutes(3);  // 23:58 + 3min = 00:01（跨天回卷）
    AURORA_TEST_CHECK_EQ(tp.selected_time(), (TimeOfDay{0, 1}));

    tp.add_hours(-1);  // 00:01 - 1h = 23:01
    AURORA_TEST_CHECK_EQ(tp.selected_time(), (TimeOfDay{23, 1}));

    tp.add_minutes(-2);  // 23:01 - 2min = 22:59
    AURORA_TEST_CHECK_EQ(tp.selected_time(), (TimeOfDay{22, 59}));

    tp.add_hours(2);  // 22:59 + 2h = 00:59
    AURORA_TEST_CHECK_EQ(tp.selected_time(), (TimeOfDay{0, 59}));

    // 默认构造 00:00，负小时回卷到 23。
    TimePicker fresh;
    AURORA_TEST_CHECK_EQ(fresh.selected_time(), (TimeOfDay{0, 0}));
    fresh.add_hours(-1);
    AURORA_TEST_CHECK_EQ(fresh.selected_time(), (TimeOfDay{23, 0}));
}

AURORA_TEST_CASE(time_picker_select_validates_and_notifies) {
    TimePicker bad{TimeOfDay{24, 0}};  // 非法初始 → 默认 {0,0}
    AURORA_TEST_CHECK_EQ(bad.selected_time(), (TimeOfDay{0, 0}));

    TimePicker tp{TimeOfDay{8, 15}};
    int calls = 0;
    TimeOfDay last{};
    tp.set_on_change([&calls, &last](TimeOfDay t) { ++calls; last = t; });

    tp.select(TimeOfDay{9, 45});
    AURORA_TEST_CHECK_EQ(calls, 1);
    AURORA_TEST_CHECK_EQ(last, (TimeOfDay{9, 45}));
    AURORA_TEST_CHECK_EQ(tp.selected_time(), (TimeOfDay{9, 45}));

    tp.select(TimeOfDay{25, 0});  // 非法忽略
    tp.select(TimeOfDay{9, 45});  // 同值忽略
    AURORA_TEST_CHECK_EQ(calls, 1);
}

AURORA_TEST_CASE(time_picker_pointer_quadrants) {
    TimePicker tp{TimeOfDay{10, 30}};
    const Size s = laid_out(tp, 400.0F, 400.0F);  // 期望 120 x 72
    AURORA_TEST_CHECK_NEAR(s.width, 120.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(s.height, 72.0F, 1e-3F);

    // 左上 = 时 +1。
    MouseEvent e = press(tp, 20.0F, 10.0F);
    AURORA_TEST_CHECK_TRUE(e.is_handled);
    AURORA_TEST_CHECK_EQ(tp.selected_time().hour, 11);

    // 右下 = 分 -1。
    e = press(tp, 100.0F, 60.0F);
    AURORA_TEST_CHECK_TRUE(e.is_handled);
    AURORA_TEST_CHECK_EQ(tp.selected_time().minute, 29);

    // 左下 = 时 -1（11 时回卷）。
    e = press(tp, 20.0F, 60.0F);
    AURORA_TEST_CHECK_TRUE(e.is_handled);
    AURORA_TEST_CHECK_EQ(tp.selected_time().hour, 10);

    // 右上 = 分 +1。
    e = press(tp, 100.0F, 10.0F);
    AURORA_TEST_CHECK_TRUE(e.is_handled);
    AURORA_TEST_CHECK_EQ(tp.selected_time().minute, 30);
}

AURORA_TEST_CASE(time_picker_describe_and_json_roundtrip) {
    const auto d = TimePicker::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "TimePicker");
    AURORA_TEST_REQUIRE_EQ(d.properties.size(), 2U);
    AURORA_TEST_CHECK_EQ(std::string{d.properties[0].name}, "hour");
    AURORA_TEST_CHECK_EQ(std::string{d.properties[1].name}, "minute");
    AURORA_TEST_REQUIRE_EQ(d.events.size(), 1U);
    AURORA_TEST_CHECK_EQ(std::string{d.events[0]}, "on_change");

    TimePicker src{TimeOfDay{8, 15}};
    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["hour"].get<int>(), 8);
    AURORA_TEST_CHECK_EQ(props["minute"].get<int>(), 15);

    TimePicker dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(dst.selected_time(), (TimeOfDay{8, 15}));
}

AURORA_TEST_CASE(color_picker_palette_and_select) {
    ColorPicker cp;
    // 默认 16 色板，默认选中第 0 格（黑）。
    AURORA_TEST_CHECK_EQ(cp.palette().size(), 16U);
    AURORA_TEST_CHECK(cp.selected_color() == ColorPicker::default_palette()[0]);

    int calls = 0;
    Color last{};
    cp.set_on_change([&calls, &last](Color c) { ++calls; last = c; });
    cp.select(Color(220, 53, 69, 255));
    AURORA_TEST_CHECK_EQ(calls, 1);
    AURORA_TEST_CHECK_EQ(last, (Color(220, 53, 69, 255)));
    AURORA_TEST_CHECK(cp.selected_color() == (Color(220, 53, 69, 255)));

    // 同色重复选中不回调。
    cp.select(Color(220, 53, 69, 255));
    AURORA_TEST_CHECK_EQ(calls, 1);

    // 显式构造初始色。
    ColorPicker red{Color(255, 0, 0, 255)};
    AURORA_TEST_CHECK(red.selected_color() == (Color(255, 0, 0, 255)));
}

AURORA_TEST_CASE(color_picker_custom_palette) {
    ColorPicker cp;
    cp.set_palette({Color(1, 2, 3, 255), Color(4, 5, 6, 255)});
    AURORA_TEST_CHECK_EQ(cp.palette().size(), 2U);

    // 空色板被忽略：沿用当前自定义色板。
    cp.set_palette({});
    AURORA_TEST_CHECK_EQ(cp.palette().size(), 2U);

    // 链式：set_palette 返回引用可继续 select。
    cp.set_palette({Color(9, 9, 9, 255)}).select(Color(9, 9, 9, 255));
    AURORA_TEST_CHECK(cp.selected_color() == (Color(9, 9, 9, 255)));
    AURORA_TEST_CHECK_EQ(cp.palette().size(), 1U);
}

AURORA_TEST_CASE(color_picker_pointer_grid_select) {
    ColorPicker cp;
    const Size s = laid_out(cp, 400.0F, 400.0F);  // 16 色 8 列 → 224 x 56
    AURORA_TEST_CHECK_NEAR(s.width, 224.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(s.height, 56.0F, 1e-3F);

    // 第 2 行第 1 格（index 8 = Color(0,190,190)）。
    MouseEvent e = press(cp, 10.0F, 40.0F);
    AURORA_TEST_CHECK_TRUE(e.is_handled);
    AURORA_TEST_CHECK(cp.selected_color() == (Color(0, 190, 190, 255)));

    // 越界格（row=3 → idx 24 ≥ 16）：不选中，事件仍消费。
    e = press(cp, 10.0F, 100.0F);
    AURORA_TEST_CHECK_TRUE(e.is_handled);
    AURORA_TEST_CHECK(cp.selected_color() == (Color(0, 190, 190, 255)));
}

AURORA_TEST_CASE(color_picker_describe_and_json_roundtrip) {
    const auto d = ColorPicker::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "ColorPicker");
    AURORA_TEST_REQUIRE_EQ(d.properties.size(), 1U);
    AURORA_TEST_CHECK_EQ(std::string{d.properties[0].name}, "color");
    AURORA_TEST_REQUIRE_EQ(d.events.size(), 1U);
    AURORA_TEST_CHECK_EQ(std::string{d.events[0]}, "on_change");

    ColorPicker src{Color(0, 122, 255, 255)};
    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_TRUE(props["color"].is_array());  // color 以 [r,g,b,a] 数组落盘

    ColorPicker dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK(dst.selected_color() == (Color(0, 122, 255, 255)));
}

AURORA_TEST_CASE(pickers_headless_paint_smoke) {
    DatePicker dp{Date{2026, 7, 25}};
    ColorPicker cp{Color(0, 122, 255, 255)};
    laid_out(dp, 400.0F, 400.0F);
    laid_out(cp, 400.0F, 400.0F);

    Painter p;
    p.begin(400, 400);
    BuildContext ctx;
    dp.paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 224.0F, .height = 200.0F}}, ctx);
    cp.paint(p, Rect{.origin = Point{.x = 0.0F, .y = 220.0F}, .size = Size{.width = 224.0F, .height = 56.0F}}, ctx);
    AURORA_TEST_CHECK_EQ(p.width(), 400);
    AURORA_TEST_CHECK_EQ(p.height(), 400);
}

}  // namespace aurora::test_cases::utest_pickers
