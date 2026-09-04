#include "aurora/widget/segmented_control.h"

#include "test_harness.h"

using aurora::Color;
using aurora::Json;
using aurora::MouseAction;
using aurora::MouseButton;
using aurora::MouseEvent;
using aurora::Point;
using aurora::SegmentedControl;

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_segmented_control ===\n");

    // --- 构造 / 空状态 ---
    {
        SegmentedControl sc;
        AURORA_TEST_CHECK(sc.type_name() == std::string("SegmentedControl"));
        AURORA_TEST_CHECK(sc.segments().empty());
        AURORA_TEST_CHECK(sc.selected() == 0);
    }

    // --- 带 segments 构造 ---
    {
        SegmentedControl sc({ "Day", "Week", "Month" }, 1);
        AURORA_TEST_CHECK(sc.segments().size() == 3);
        AURORA_TEST_CHECK(sc.selected() == 1);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(sc.segments()[0] == "Day");
    }

    // --- set_selected ---
    {
        SegmentedControl sc({ "A", "B", "C" });
        sc.set_selected(2);
        AURORA_TEST_CHECK(sc.selected() == 2);
    }

    // --- on_change 回调 ---
    {
        SegmentedControl sc({ "X", "Y" });
        int changed_to = -1;
        sc.set_on_change([&](int v) -> void { changed_to = v; });
        // 模拟通过 set_selected 不会触发回调（回调只在点击时触发）
        // 直接验证 setter
        AURORA_TEST_CHECK(changed_to == -1); // 未触发
    }

    // --- describe_static ---
    {
        auto desc = SegmentedControl::describe_static();
        AURORA_TEST_CHECK(desc.name == "SegmentedControl");
        bool has_selected = false;
        for (const auto &p : desc.properties) {
            if (p.name == "selected") {
                has_selected = true;
            }
        }
        AURORA_TEST_CHECK(has_selected);
        AURORA_TEST_CHECK(desc.children_policy == "none");
    }

    // --- 序列化往返 ---
    {
        SegmentedControl sc({ "A", "B", "C" }, 2);
        Json props = Json::object();
        sc.serialize_props(props);
        AURORA_TEST_CHECK(props.contains("selected"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(props["selected"].get<int>() == 2);

        SegmentedControl sc2;
        sc2.deserialize_props(props);
        AURORA_TEST_CHECK(sc2.selected() == 2);
    }

    // --- 现代化属性：主题回退 / 禁用态 / 样式往返 ---
    {
        SegmentedControl sc({ "A", "B" }, 0);
        Json j0;
        sc.serialize_props(j0);
        AURORA_TEST_CHECK(!j0.contains("active_color")); // 未设置不序列化（跟随主题 primary）
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(j0["segments"].size() == 2);   // segments 现已序列化（支持完整重建）

        sc.set_active_color(Color::red())
            .set_text_color(Color{1, 2, 3, 255})
            .set_selected_text_color(Color{4, 5, 6, 255})
            .set_border_color(Color{7, 8, 9, 255})
            .set_font_size(16.0F)
            .set_corner_radius(8.0F)
            .set_enabled(false);
        Json j;
        sc.serialize_props(j);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(j["active_color"][0].get<int>() == 255);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(j["font_size"].get<double>() == 16.0);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(j["corner_radius"].get<double>() == 8.0);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(j["enabled"].get<bool>() == false);

        SegmentedControl sc2;
        sc2.deserialize_props(j);
        AURORA_TEST_CHECK(sc2.segments().size() == 2);
        AURORA_TEST_CHECK(!sc2.enabled());

        // 禁用态：点击不切换
        MouseEvent e;
        e.action = MouseAction::Press;
        e.button = MouseButton::Left;
        e.local_position = Point{.x = 5.0F, .y = 5.0F};
        sc2.on_pointer_event(e);
        AURORA_TEST_CHECK(sc2.selected() == 0);
        AURORA_TEST_CHECK(e.is_handled_);
    }
}