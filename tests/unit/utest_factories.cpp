/// 测试类型: unit
/// 目标单元: include/aurora/ui/factories.h
/// 测试说明: 覆盖 ui 工厂函数——label/button/input/checkbox/slider 的主文案覆盖与回调接线、
/// 双模重载（Reactive 与裸值）、vbox/hbox/stack/grid/scroll 容器糖、
/// lazy_row/lazy_list/bottom_nav_bar 的构造转发与父子所有权

#include "aurora/ui/factories.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_factories {

namespace ui = aurora::ui;

AURORA_TEST_CASE(label_adds_text_and_overrides_content) {
    aurora::Column root;
    aurora::Text* t = ui::label(root, "hello");
    AURORA_TEST_REQUIRE_NOT_NULL(t);
    AURORA_TEST_CHECK_EQ(root.child_nodes().size(), 1U);
    AURORA_TEST_CHECK_EQ(t->content.get().text, "hello");
    // props.content 被主文案覆盖。
    aurora::Text* t2 = ui::label(root, "world", aurora::TextProps{.content = aurora::LocalizedString{"ignored"}});
    AURORA_TEST_CHECK_EQ(t2->content.get().text, "world");
    AURORA_TEST_CHECK_EQ(root.child_nodes().size(), 2U);
}

AURORA_TEST_CASE(button_adds_with_label_and_click) {
    aurora::Column root;
    int clicks = 0;
    aurora::Button* b = ui::button(root, "OK", aurora::ButtonProps{}, [&clicks]() -> void { ++clicks; });
    AURORA_TEST_REQUIRE_NOT_NULL(b);
    AURORA_TEST_CHECK_EQ(b->label.get().text, "OK");
    AURORA_TEST_REQUIRE_NOT_NULL(b->on_click);
    b->on_click();
    AURORA_TEST_CHECK_EQ(clicks, 1);
    // 无回调重载：on_click 为空不崩溃。
    aurora::Button* plain = ui::button(root, "Noop");
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(plain->on_click));
}

AURORA_TEST_CASE(input_adds_text_input_with_initial_value) {
    aurora::Column root;
    aurora::TextInput* ti = ui::input(root, "abc");
    AURORA_TEST_REQUIRE_NOT_NULL(ti);
    AURORA_TEST_CHECK_EQ(ti->value(), "abc");
    // 默认空值。
    aurora::TextInput* empty = ui::input(root);
    AURORA_TEST_CHECK_EQ(empty->value(), "");
}

AURORA_TEST_CASE(checkbox_reactive_and_bool_overloads) {
    aurora::Column root;
    aurora::Reactive<bool> state{false};
    aurora::Checkbox* a = ui::checkbox(root, state);
    AURORA_TEST_REQUIRE_NOT_NULL(a);
    // 裸值重载 → 内部转 Reactive。
    aurora::Checkbox* b = ui::checkbox(root, true);
    AURORA_TEST_REQUIRE_NOT_NULL(b);
    AURORA_TEST_CHECK_EQ(root.child_nodes().size(), 2U);
}

AURORA_TEST_CASE(slider_reactive_and_scalar_overloads) {
    aurora::Column root;
    aurora::Reactive<double> value{0.25};
    aurora::Slider* a = ui::slider(root, value);
    AURORA_TEST_REQUIRE_NOT_NULL(a);
    aurora::Slider* b = ui::slider(root, 0.75);
    AURORA_TEST_REQUIRE_NOT_NULL(b);
    AURORA_TEST_CHECK_EQ(root.child_nodes().size(), 2U);
}

AURORA_TEST_CASE(vbox_hbox_return_typed_containers) {
    aurora::Column root;
    aurora::Column* v = ui::vbox(root);
    aurora::Row* h = ui::hbox(root);
    AURORA_TEST_REQUIRE_NOT_NULL(v);
    AURORA_TEST_REQUIRE_NOT_NULL(h);
    AURORA_TEST_CHECK_EQ(std::string{v->type_name()}, "Column");
    AURORA_TEST_CHECK_EQ(std::string{h->type_name()}, "Row");
    // 工厂返回的容器可直接续接子项。
    ui::label(*v, "in-vbox");
    AURORA_TEST_CHECK_EQ(v->child_nodes().size(), 1U);
}

AURORA_TEST_CASE(stack_grid_scroll_forward_props) {
    aurora::Column root;
    aurora::Stack* s = ui::stack(root, aurora::Alignment::Center);
    AURORA_TEST_REQUIRE_NOT_NULL(s);
    aurora::Grid* g = ui::grid(root, aurora::GridProps{.columns = 2});
    AURORA_TEST_REQUIRE_NOT_NULL(g);
    aurora::Scroll* sc = ui::scroll(
        root, aurora::ScrollProps{.child = aurora::Node(std::make_shared<aurora::Text>("page")), .step = 32.0F});
    AURORA_TEST_REQUIRE_NOT_NULL(sc);
    AURORA_TEST_CHECK_EQ(root.child_nodes().size(), 3U);
}

AURORA_TEST_CASE(lazy_row_and_list_forward_builder) {
    aurora::Column root;
    auto* lr = ui::lazy_row(
        root, 10,
        // 花括号返回列表会走列表初始化重载决议，存在被 initializer_list 构造函数劫持的陷阱，刻意保留显式类型名。
        // NOLINTNEXTLINE(modernize-return-braced-init-list)
        [](int i) -> aurora::Node { return aurora::Node(std::make_shared<aurora::Text>(std::to_string(i))); }, 64.0F);
    AURORA_TEST_REQUIRE_NOT_NULL(lr);
    auto* ll = ui::lazy_list(root, 20, [](int i) -> aurora::Node {
        // 同上：显式 aurora::Node(...) 构造意图清晰，改花括号列表存在重载决议劫持陷阱。
        // NOLINTNEXTLINE(modernize-return-braced-init-list)
        return aurora::Node(std::make_shared<aurora::Text>(std::to_string(i)));
    });
    AURORA_TEST_REQUIRE_NOT_NULL(ll);
    AURORA_TEST_CHECK_EQ(std::string{lr->type_name()}, "LazyRow");
    AURORA_TEST_CHECK_EQ(std::string{ll->type_name()}, "LazyList");
}

AURORA_TEST_CASE(bottom_nav_bar_forward_props) {
    aurora::Column root;
    aurora::BottomNavBarProps props;
    props.items.push_back(aurora::BottomNavItem{.icon = {}, .label = "Home"});
    props.items.push_back(aurora::BottomNavItem{.icon = {}, .label = "Settings"});
    props.selected_index = 1;
    aurora::BottomNavBar* bar = ui::bottom_nav_bar(root, std::move(props));
    AURORA_TEST_REQUIRE_NOT_NULL(bar);
    AURORA_TEST_CHECK_EQ(std::string{bar->type_name()}, "BottomNavBar");
}

AURORA_TEST_CASE(factories_take_ownership_via_parent_tree) {
    // 返回的裸指针由父树 shared_ptr 持有：父销毁后指针失效（此处仅验证树内互指一致）。
    aurora::Column root;
    aurora::Text* t = ui::label(root, "owned");
    AURORA_TEST_CHECK_EQ(&root.child_nodes()[0].widget(), t);
}

}  // namespace aurora::test_cases::utest_factories
