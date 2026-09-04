// 验证 TabBar：构造/选中/切换、关闭标签、点击命中切换、序列化。

#include <memory>

#include "aurora/widget/tab_bar.h"
#include "aurora/widget/text.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Constraints;
using aurora::LocalizedString;
using aurora::MouseAction;
using aurora::MouseButton;
using aurora::MouseEvent;
using aurora::Node;
using aurora::Point;
using aurora::Size;
using aurora::Tab;
using aurora::TabBar;
using aurora::Text;

namespace {

auto make_text(const char *s) -> Node {
    auto t = Text();
    t.content = LocalizedString{s};
    return Node{std::move(t)};
}

auto make_tabs() -> std::vector<Tab> {
    std::vector<Tab> tabs;
    tabs.push_back(Tab{.label = "Home", .content = make_text("home content"), .closable = false});
    tabs.push_back(Tab{.label = "Edit", .content = make_text("edit content"), .closable = false});
    tabs.push_back(Tab{.label = "View", .content = make_text("view content"), .closable = true});
    return tabs;
}

}  // namespace

AURORA_TEST() {
    // ---- 1. 构造与初始选中 ----
    {
        auto tb = std::make_shared<TabBar>(make_tabs());
        AURORA_TEST_CHECK(tb->tab_count() == 3);
        AURORA_TEST_CHECK(tb->selected() == 0);
        AURORA_TEST_CHECK(tb->tab_label(0) == "Home");
        AURORA_TEST_CHECK(tb->tab_label(2) == "View");
        AURORA_TEST_CHECK(tb->tab_label(99).empty());
    }

    // ---- 2. 初始序号越界钳制 ----
    {
        auto tb = std::make_shared<TabBar>(make_tabs(), 99);
        AURORA_TEST_CHECK(tb->selected() == 2);

        auto tb2 = std::make_shared<TabBar>(make_tabs(), -5);
        AURORA_TEST_CHECK(tb2->selected() == 0);
    }

    // ---- 3. select 切换与回调 ----
    {
        auto tb = std::make_shared<TabBar>(make_tabs());
        int changed_to = -1;
        tb->set_on_change([&changed_to](int i) -> void { changed_to = i; });

        tb->select(1);
        AURORA_TEST_CHECK(tb->selected() == 1);
        AURORA_TEST_CHECK(changed_to == 1);

        // 相同序号不重复回调
        changed_to = -1;
        tb->select(1);
        AURORA_TEST_CHECK(changed_to == -1);

        // 越界忽略
        tb->select(99);
        AURORA_TEST_CHECK(tb->selected() == 1);
    }

    // ---- 4. close_tab 移除与选中修正 ----
    {
        auto tb = std::make_shared<TabBar>(make_tabs());
        int closed = -1;
        tb->set_on_close([&closed](int i) -> void { closed = i; });

        tb->select(2);
        tb->close_tab(2);  // 关闭当前选中的最后一个
        AURORA_TEST_CHECK(closed == 2);
        AURORA_TEST_CHECK(tb->tab_count() == 2);
        AURORA_TEST_CHECK(tb->selected() == 1);  // 前移

        // 关闭选中之前的标签
        tb->close_tab(0);
        AURORA_TEST_CHECK(tb->tab_count() == 1);
        AURORA_TEST_CHECK(tb->selected() == 0);
    }

    // ---- 5. add_tab 追加 ----
    {
        auto tb = std::make_shared<TabBar>(make_tabs());
        tb->add_tab(Tab{.label = "New", .content = make_text("new"), .closable = false});
        AURORA_TEST_CHECK(tb->tab_count() == 4);
        AURORA_TEST_CHECK(tb->tab_label(3) == "New");
    }

    // ---- 6. 布局：内容区在标签栏下方 ----
    {
        auto tb = std::make_shared<TabBar>(make_tabs());
        BuildContext ctx;
        tb->mount(ctx);
        Constraints c;
        c.min = Size{.width = 0.0F, .height = 0.0F};
        c.max = Size{.width = 640.0F, .height = 480.0F};
        const Size s = tb->layout(c, ctx);
        AURORA_TEST_CHECK(s.width == 640.0F);
        AURORA_TEST_CHECK(s.height == 480.0F);
    }

    // ---- 7. 点击标签切换 ----
    {
        auto tb = std::make_shared<TabBar>(make_tabs());
        BuildContext ctx;
        tb->mount(ctx);
        Constraints c;
        c.min = Size{.width = 0.0F, .height = 0.0F};
        c.max = Size{.width = 640.0F, .height = 480.0F};
        tb->layout(c, ctx);

        // 点击第二个标签（第一个标签宽约 60-70dp，点击 x=90 落在第二个）
        MouseEvent e;
        e.action = MouseAction::Press;
        e.button = MouseButton::Left;
        e.local_position = Point{.x = 90.0F, .y = 18.0F};
        tb->on_pointer_event(e);
        AURORA_TEST_CHECK(e.is_handled_);
        AURORA_TEST_CHECK(tb->selected() == 1);

        // 点击第一个标签切回
        MouseEvent e2;
        e2.action = MouseAction::Press;
        e2.button = MouseButton::Left;
        e2.local_position = Point{.x = 10.0F, .y = 18.0F};
        tb->on_pointer_event(e2);
        AURORA_TEST_CHECK(tb->selected() == 0);
    }

    // ---- 8. 内容区点击不切换标签 ----
    {
        auto tb = std::make_shared<TabBar>(make_tabs());
        BuildContext ctx;
        tb->mount(ctx);
        Constraints c;
        c.min = Size{.width = 0.0F, .height = 0.0F};
        c.max = Size{.width = 640.0F, .height = 480.0F};
        tb->layout(c, ctx);

        MouseEvent e;
        e.action = MouseAction::Press;
        e.button = MouseButton::Left;
        e.local_position = Point{.x = 90.0F, .y = 200.0F};  // 内容区
        tb->on_pointer_event(e);
        AURORA_TEST_CHECK(tb->selected() == 0);  // 未切换
    }

    // ---- 9. 序列化 ----
    {
        auto tb = std::make_shared<TabBar>(make_tabs());
        tb->select(1);
        tb->set_tab_height(42.0F);

        aurora::Json props;
        tb->serialize_props(props);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(props["selected_index"].get<int>() == 1);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(props["tab_height"].get<float>() == 42.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(props["tab_labels"].size() == 3);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(props["tab_labels"][0].get<std::string>() == "Home");
    }

    // ---- 10. 无头渲染不崩溃 ----
    {
        auto tb = std::make_shared<TabBar>(make_tabs());
        BuildContext ctx;
        tb->mount(ctx);
        Constraints c;
        c.min = Size{.width = 0.0F, .height = 0.0F};
        c.max = Size{.width = 320.0F, .height = 240.0F};
        tb->layout(c, ctx);

        aurora::Painter p;
        p.begin(320, 240);
        tb->paint(p,
                  aurora::Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 320.0F, .height = 240.0F}},
                  ctx);
        AURORA_TEST_CHECK(p.width() == 320);
    }

    // ---- 11. 现代化属性：主题回退 / 样式往返 ----
    {
        auto tb = std::make_shared<TabBar>(make_tabs());
        aurora::Json j0;
        tb->serialize_props(j0);
        AURORA_TEST_CHECK(!j0.contains("active_color"));  // 未设置不序列化（跟随主题 primary）

        tb->set_active_color(aurora::Color::red())
            .set_bar_background(aurora::Color{1, 2, 3, 255})
            .set_tab_background(aurora::Color{4, 5, 6, 255})
            .set_text_color(aurora::Color{7, 8, 9, 255})
            .set_indicator_thickness(3.0F)
            .set_font_size(15.0F)
            .set_tab_padding(16.0F);
        aurora::Json j;
        tb->serialize_props(j);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(j["active_color"][0].get<int>() == 255);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(j["bar_background"][2].get<int>() == 3);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(j["indicator_thickness"].get<float>() == 3.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(j["font_size"].get<float>() == 15.0F);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(j["tab_padding"].get<float>() == 16.0F);

        auto tb2 = std::make_shared<TabBar>(make_tabs());
        tb2->deserialize_props(j);
        aurora::Json k;
        tb2->serialize_props(k);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(k["active_color"][0].get<int>() == 255);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(k["tab_padding"].get<float>() == 16.0F);
    }
}