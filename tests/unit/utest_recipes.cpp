/// 测试类型: unit
/// 目标单元: include/aurora/widget/recipes.h
/// 测试说明: 覆盖组合配方 form_layout/toolbar/sidebar/menu_bar/list_view/tab_view
/// 的返回原语类型、树结构与标签定宽、布局行为（填满宽/定宽）与 tab_view 点击换页

#include <string>
#include <utility>
#include <vector>

#include "aurora/core/dimension.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/recipes.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_recipes {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(recipes_compose_basic_primitives) {
    // 每个配方只是原语的约定排版：不新增核心 Widget 类（需求 #3 设计哲学）
    auto form = form_layout(
        {FormRow{.label = "Name", .field = Node{Text{"Ada"}}}, FormRow{.label = "Age", .field = Node{Text{"36"}}}});
    AURORA_TEST_CHECK_EQ(std::string{form.widget().type_name()}, "Column");

    auto bar = toolbar({Node{Button{"A"}}, Node{Button{"B"}}});
    AURORA_TEST_CHECK_EQ(std::string{bar.widget().type_name()}, "Row");

    auto side = sidebar({Node{Text{"Home"}}, Node{Text{"Settings"}}});
    AURORA_TEST_CHECK_EQ(std::string{side.widget().type_name()}, "Column");

    auto menu = menu_bar({Node{Button{"File"}}, Node{Button{"Edit"}}});
    AURORA_TEST_CHECK_EQ(std::string{menu.widget().type_name()}, "Row");

    auto list = list_view({Node{Text{"1"}}, Node{Text{"2"}}});
    AURORA_TEST_CHECK_EQ(std::string{list.widget().type_name()}, "Scroll");

    auto tabs = tab_view({TabPage{.title = "A", .content = Node{Text{"Page A"}}},
                          TabPage{.title = "B", .content = Node{Text{"Page B"}}}});
    AURORA_TEST_CHECK_EQ(std::string{tabs.widget().type_name()}, "Column");
}

AURORA_TEST_CASE(form_layout_rows_and_fixed_label_width) {
    auto form = form_layout(
        {FormRow{.label = "Name", .field = Node{Text{"Ada"}}}, FormRow{.label = "Age", .field = Node{Text{"36"}}}});
    // 下行目标类型已由配方树结构（Column）锁定，dynamic_cast 徒增 RTTI 依赖且掩盖测试意图。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto& col = static_cast<Column&>(form.widget());
    AURORA_TEST_REQUIRE_EQ(col.child_count(), 2U);  // 每个表单行一个 Row

    // 行子节点类型为 Row（form_layout 逐行构建 Row）。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto& row0 = static_cast<Row&>(col.child(0).widget());
    AURORA_TEST_REQUIRE_EQ(row0.child_count(), 2U);  // 标签 + 字段
    AURORA_TEST_CHECK_EQ(std::string{row0.child(0).widget().type_name()}, "Text");
    AURORA_TEST_CHECK_EQ(std::string{row0.child(1).widget().type_name()}, "Text");
    // 同上：第二行仍是 Row。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto& row1 = static_cast<Row&>(col.child(1).widget());
    AURORA_TEST_CHECK_EQ(row1.child_count(), 2U);

    // 标签列为固定宽（Widget::width 强类型意图），默认 label_width = 120
    // Row 首子节点已由上文 type_name 断言锁定为 Text。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto& label0 = static_cast<Text&>(row0.child(0).widget());
    AURORA_TEST_CHECK_EQ(label0.width_spec().kind, LengthKind::Fixed);
    AURORA_TEST_CHECK_NEAR(label0.width_spec().value, 120.0F, 1e-3F);
    // 行带内边距修饰（Modifier{}.padding(4)）
    AURORA_TEST_CHECK_EQ(row0.modifier.get().nodes().size(), 1U);

    // 自定义 label_width 逐行传递
    auto form2 = form_layout({FormRow{.label = "Name", .field = Node{Text{"Ada"}}}}, 90.0F);
    // 下行链 Column→Row→Text 由 form_layout 树结构锁定，dynamic_cast 徒增 RTTI 依赖。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast)
    auto& label2 =
        static_cast<Text&>(static_cast<Row&>(static_cast<Column&>(form2.widget()).child(0).widget()).child(0).widget());
    // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
    AURORA_TEST_CHECK_NEAR(label2.width_spec().value, 90.0F, 1e-3F);
    LayoutEngine::layout(form2.widget(), bounded(400.0F, 600.0F));
    AURORA_TEST_CHECK_NEAR(label2.size().width, 90.0F, 1e-3F);  // 布局后实际宽度即 label_width
}

AURORA_TEST_CASE(toolbar_fills_width_sidebar_fixed_width) {
    auto bar = toolbar({Node{Button{"A"}}, Node{Button{"B"}}});
    // toolbar 返回 Row（type_name 断言见上一用例），无需 dynamic_cast。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto& bar_row = static_cast<Row&>(bar.widget());
    // 工具栏修饰链：fill_max_width + background + padding
    AURORA_TEST_CHECK_EQ(bar_row.modifier.get().nodes().size(), 3U);
    LayoutEngine::layout(bar.widget(), bounded(480.0F, 600.0F));
    AURORA_TEST_CHECK_NEAR(bar.widget().size().width, 480.0F, 1e-3F);  // 填满可用宽度

    auto side = sidebar({Node{Text{"Home"}}}, 160.0F);
    LayoutEngine::layout(side.widget(), bounded(480.0F, 600.0F));
    AURORA_TEST_CHECK_NEAR(side.widget().size().width, 160.0F, 1e-3F);  // 固定宽

    auto side_default = sidebar({Node{Text{"Home"}}});
    LayoutEngine::layout(side_default.widget(), bounded(480.0F, 600.0F));
    AURORA_TEST_CHECK_NEAR(side_default.widget().size().width, 200.0F, 1e-3F);  // 默认 200

    auto menu = menu_bar({Node{Button{"File"}}});
    LayoutEngine::layout(menu.widget(), bounded(480.0F, 600.0F));
    AURORA_TEST_CHECK_NEAR(menu.widget().size().width, 480.0F, 1e-3F);  // 同工具栏填满宽
}

AURORA_TEST_CASE(list_view_wraps_column_in_scroll) {
    auto list = list_view({Node{Text{"1"}}, Node{Text{"2"}}, Node{Text{"3"}}});
    // list_view 契约返回 Scroll（type_name 断言见 compose 用例），下行类型已由结构锁定。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto& scroll = static_cast<Scroll&>(list.widget());
    AURORA_TEST_REQUIRE_EQ(scroll.child_count(), 1U);  // Scroll 包裹 Column
    AURORA_TEST_CHECK_NEAR(scroll.step, 16.0F, 1e-3F);  // 配方约定的滚轮步长

    // ScrollProps::child 成员与 Container::child(i) 同名，取容器访问须限定基类；内层为 Column。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto& inner = static_cast<Column&>(scroll.Container::child(0).widget());
    AURORA_TEST_CHECK_EQ(inner.child_count(), 3U);
    AURORA_TEST_CHECK_EQ(std::string{inner.child(0).widget().type_name()}, "Text");
}

AURORA_TEST_CASE(tab_view_switches_page_on_tab_activate) {
    Text page_a{"Alpha"};
    page_a.width(px(200.0F)).height(px(40.0F));
    Text page_b{"Beta"};
    page_b.width(px(200.0F)).height(px(80.0F));

    auto tabs = tab_view({TabPage{.title = "A", .content = Node{std::move(page_a)}},
                          TabPage{.title = "B", .content = Node{std::move(page_b)}}});
    // tab_view 契约：Column[Row(标签按钮), TabBody]，下行类型由结构锁定（type_name 断言在下）。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto& col = static_cast<Column&>(tabs.widget());
    AURORA_TEST_REQUIRE_EQ(col.child_count(), 2U);  // 标签按钮行 + 内容体
    AURORA_TEST_CHECK_EQ(std::string{col.child(0).widget().type_name()}, "Row");
    AURORA_TEST_CHECK_EQ(std::string{col.child(1).widget().type_name()}, "TabBody");

    // 标签按钮标题与页序一致，on_click 已接线到内部 State
    // 首子节点 type_name 已断言为 Row。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto& btn_row = static_cast<Row&>(col.child(0).widget());
    AURORA_TEST_REQUIRE_EQ(btn_row.child_count(), 2U);
    // 标签行子节点由 tab_view 构建为 Button。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto& btn_a = static_cast<Button&>(btn_row.child(0).widget());
    // 同上，第二枚标签按钮。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto& btn_b = static_cast<Button&>(btn_row.child(1).widget());
    AURORA_TEST_CHECK_EQ(btn_a.label.get().text, std::string{"A"});
    AURORA_TEST_CHECK_EQ(btn_b.label.get().text, std::string{"B"});

    const BuildContext ctx;
    col.mount(ctx);  // 挂载后 TabBody 订阅内部 selected 状态（变化即标脏）
    const Constraints c = bounded(400.0F, 600.0F);
    LayoutEngine::layout(col, c);
    const float initial_h = col.size().height;  // 默认显示页 A（固定高 40）

    btn_b.activate();  // 点击标签 B → 内部 State 置 1 → 内容体换页
    LayoutEngine::layout(col, c);
    AURORA_TEST_CHECK_GT(col.size().height, initial_h);
    AURORA_TEST_CHECK_NEAR(col.size().height - initial_h, 40.0F, 1e-3F);  // 页 B 高 80

    btn_a.activate();  // 切回页 A
    LayoutEngine::layout(col, c);
    AURORA_TEST_CHECK_NEAR(col.size().height, initial_h, 1e-3F);
}

}  // namespace aurora::test_cases::utest_recipes
