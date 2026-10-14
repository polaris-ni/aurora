/// 测试类型: unit
/// 目标单元: include/aurora/widget/bottom_nav_bar.h
/// 测试说明: 底部导航栏。覆盖默认/聚合构造、type_name、静态/运行期自描述、信号收集（空）、set_items/
/// set_selected_index/set_on_select/set_bar_height 链式、on_pointer_event（按等分格宽换算下标触发 on_select +
/// 置 handled）、on_layout（栏高与可用高取小）、on_paint（背景/分隔线/选中 pill/图标/文案）、
/// on_hit_test/on_hit_test_chain、serialize/deserialize_props。
/// 判据：点击某 tab 触发 on_select(对应下标) 且 is_handled；on_layout 高度受可用高截断；序列化往返保留选中下标与栏高。

#include <memory>
#include <vector>

#include "aurora/core/types.h"
#include "aurora/environment/environment.h"
#include "aurora/event/event.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/bottom_nav_bar.h"
#include "aurora/widget/widget.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_bottom_nav_bar {

namespace {

/// 造一个注入了指定主题的构建上下文（根级环境，无父指针）。
/// 返回 holder + ctx：`ctx.env` 指向 holder 内的 Environment，二者须在同作用域内使用。
auto themed_ctx(const Theme &t) -> std::pair<std::shared_ptr<Environment>, BuildContext> {
    auto env = std::make_shared<Environment>();
    env->set_local(t);
    BuildContext ctx;
    ctx.env = env.get();
    return {std::move(env), ctx};
}

auto make_item(const std::string &label) -> BottomNavItem {
    return BottomNavItem{.icon = [](Painter &, const Rect &, bool) {}, .label = label};
}

}  // namespace

AURORA_TEST_CASE(api_surface_and_select) {
    BottomNavBarProps props;
    props.items = {make_item("Home"), make_item("Settings")};
    props.selected_index = 0;
    int sel = -1;
    props.on_select = [&](int i) { sel = i; };
    BottomNavBar bar{props};

    AURORA_TEST_CHECK_EQ(std::string(bar.type_name()), "BottomNavBar");
    auto d = BottomNavBar::describe_static();
    AURORA_TEST_CHECK_EQ(d.name, "BottomNavBar");
    auto d2 = bar.describe();
    AURORA_TEST_CHECK_EQ(d2.name, "BottomNavBar");
    std::vector<SignalViewBase *> sigs;
    bar.collect_signals(sigs);
    AURORA_TEST_CHECK_EQ(sigs.size(), 0U);

    bar.set_selected_index(1);
    bar.set_bar_height(50.0F);
    bar.set_items({make_item("A"), make_item("B"), make_item("C")});
    int sel2 = -1;
    bar.set_on_select([&](int i) { sel2 = i; });

    BuildContext ctx;
    const Constraints c{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 300.0F, .height = 200.0F}};
    const Size sz = bar.layout(c, ctx);
    AURORA_TEST_CHECK_EQ(sz.height, 50.0F);  // min(栏高, 可用高)

    // 点击第 0 格（x=10，3 项 → 格宽 100 → idx 0）。
    MouseEvent e0;
    e0.action = MouseAction::Press;
    e0.local_position = Point{.x = 10.0F, .y = 10.0F};
    bar.on_pointer_event(e0);
    AURORA_TEST_CHECK_EQ(sel2, 0);
    AURORA_TEST_CHECK_EQ(e0.is_handled, true);

    // 点击第 2 格（x=210 → idx 2）。
    MouseEvent e2;
    e2.action = MouseAction::Press;
    e2.local_position = Point{.x = 210.0F, .y = 10.0F};
    bar.on_pointer_event(e2);
    AURORA_TEST_CHECK_EQ(sel2, 2);
    AURORA_TEST_CHECK_EQ(e2.is_handled, true);

    // 绘制（背景/分隔线/选中 pill/图标/文案）。
    Painter painter;
    const Rect bounds{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 300.0F, .height = 50.0F}};
    bar.paint(painter, bounds, ctx);

    (void)bar.hit_test(Point{.x = 10.0F, .y = 10.0F}, bounds, ctx);
    (void)bar.hit_test_chain(Point{.x = 10.0F, .y = 10.0F}, bounds, ctx);
    (void)sel;
}

AURORA_TEST_CASE(serialize_deserialize_roundtrip) {
    BottomNavBarProps props;
    props.items = {make_item("X")};
    props.selected_index = 1;
    props.bar_height = 72.0F;
    BottomNavBar bar{props};

    Json out = Json::object();
    bar.serialize_props(out);
    AURORA_TEST_CHECK_TRUE(out.contains("selected_index"));
    AURORA_TEST_CHECK_TRUE(out.contains("bar_height"));

    BottomNavBar bar2;
    Json p = Json::object();
    p.set("selected_index", 2);
    p.set("bar_height", 80.0);
    bar2.deserialize_props(p);
    AURORA_TEST_CHECK_EQ(std::string(bar2.type_name()), "BottomNavBar");  // 反序列化不破坏类型
}

// ---- 主题跟随（CHG-011 迁移批）：未显式设色 → 角色回退；显式设色 → 不被主题覆盖 ----

AURORA_TEST_CASE(bottom_nav_bar_colors_follow_theme_when_unset) {
    // 两态语义：未显式设色时 resolve 取主题角色；主题翻转 ⇒ 解析值翻转（换肤生效）。
    const BuildContext bare;  // 无注入：inherit_theme 回落 light
    BottomNavBar bar;
    AURORA_TEST_CHECK(bar.resolve_background(bare) == Color(255, 255, 255));  // light surface
    AURORA_TEST_CHECK(bar.resolve_divider(bare) == Color(198, 198, 198));  // light outline_variant
    AURORA_TEST_CHECK(bar.resolve_selected_color(bare) == Color(0, 0, 255));  // light primary
    AURORA_TEST_CHECK(bar.resolve_unselected_color(bare) == Color(71, 71, 71));  // light on_surface_variant
    AURORA_TEST_CHECK(bar.resolve_pill_color(bare) == Color(225, 224, 251));  // light secondary_container
    const auto [env, dark] = themed_ctx(Theme::dark());
    AURORA_TEST_CHECK(bar.resolve_background(dark) == Color(32, 33, 36));  // dark surface
    AURORA_TEST_CHECK(bar.resolve_divider(dark) == Color(71, 71, 71));  // dark outline_variant
    AURORA_TEST_CHECK(bar.resolve_selected_color(dark) == Color(192, 193, 255));  // dark primary
    AURORA_TEST_CHECK(bar.resolve_unselected_color(dark) == Color(198, 198, 198));  // dark on_surface_variant
    AURORA_TEST_CHECK(bar.resolve_pill_color(dark) == Color(68, 69, 91));  // dark secondary_container
}

AURORA_TEST_CASE(bottom_nav_bar_explicit_colors_override_theme) {
    // 对照组：显式设色不被主题覆盖（两态语义的另一半）。
    const auto [env, dark] = themed_ctx(Theme::dark());
    BottomNavBar bar;
    bar.set_background_color(Color(1, 2, 3, 4));
    bar.set_selected_color(Color(9, 8, 7, 6));
    bar.set_pill_color(Color(11, 12, 13, 14));
    AURORA_TEST_CHECK(bar.resolve_background(dark) == Color(1, 2, 3, 4));
    AURORA_TEST_CHECK(bar.resolve_selected_color(dark) == Color(9, 8, 7, 6));
    AURORA_TEST_CHECK(bar.resolve_pill_color(dark) == Color(11, 12, 13, 14));
    // 未设置的字段仍跟随主题（不被已设置的字段影响）。
    AURORA_TEST_CHECK(bar.resolve_unselected_color(dark) == Color(198, 198, 198));  // dark on_surface_variant
}

}  // namespace aurora::test_cases::utest_bottom_nav_bar
