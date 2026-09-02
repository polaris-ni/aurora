// 验证 RadioGroup / SpinBox：互斥选择、点击命中、数值钳制/步进、序列化。
#include <memory>

#include "aurora/widget/radio_spin.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Constraints;
using aurora::MouseAction;
using aurora::MouseEvent;
using aurora::Point;
using aurora::RadioGroup;
using aurora::Rect;
using aurora::Size;
using aurora::SpinBox;

AURORA_TEST() {
    // ==================== RadioGroup ====================

    // ---- 1. 构造与选中 ----
    {
        auto rg = std::make_shared<RadioGroup>(std::vector<std::string>{ "Yes", "No", "Maybe" }, 1);
        AURORA_TEST_CHECK(rg->option_count() == 3);
        AURORA_TEST_CHECK(rg->selected_index() == 1);
    }

    // ---- 2. select 与回调、越界忽略 ----
    {
        auto rg = std::make_shared<RadioGroup>(std::vector<std::string>{ "A", "B" });
        int changed = -1;
        rg->set_on_change([&changed](int i) -> void { changed = i; });

        rg->select(1);
        AURORA_TEST_CHECK(rg->selected_index() == 1);
        AURORA_TEST_CHECK(changed == 1);

        changed = -1;
        rg->select(1);  // 相同不回调
        rg->select(99); // 越界忽略
        AURORA_TEST_CHECK(changed == -1);
        AURORA_TEST_CHECK(rg->selected_index() == 1);
    }

    // ---- 3. 垂直排列点击命中（行高 28）----
    {
        auto rg = std::make_shared<RadioGroup>(std::vector<std::string>{ "R0", "R1", "R2" });
        BuildContext ctx;
        rg->mount(ctx);
        Constraints c;
        c.min = Size{ .width = 0.0f, .height = 0.0f };
        c.max = Size{ .width = 320.0f, .height = 240.0f };
        rg->layout(c, ctx);

        MouseEvent e;
        e.action = MouseAction::Press;
        e.local_position = Point{ .x = 10.0f, .y = (28.0f * 2) + 14.0f }; // 第三行中心
        rg->on_pointer_event(e);
        AURORA_TEST_CHECK(e.handled);
        AURORA_TEST_CHECK(rg->selected_index() == 2);
    }

    // ---- 4. 水平排列布局与命中 ----
    {
        auto rg = std::make_shared<RadioGroup>(std::vector<std::string>{ "L", "R" }, 0, true);
        BuildContext ctx;
        rg->mount(ctx);
        Constraints c;
        c.min = Size{ .width = 0.0f, .height = 0.0f };
        c.max = Size{ .width = 320.0f, .height = 240.0f };
        const Size s = rg->layout(c, ctx);
        AURORA_TEST_CHECK(s.height == 28.0f); // 单行

        // 点击第二项（第一项宽 = 文本宽 + 36）
        MouseEvent e;
        e.action = MouseAction::Press;
        e.local_position = Point{ .x = s.width - 10.0f, .y = 14.0f };
        rg->on_pointer_event(e);
        AURORA_TEST_CHECK(rg->selected_index() == 1);
    }

    // ---- 5. RadioGroup 序列化往返 ----
    {
        auto rg = std::make_shared<RadioGroup>(std::vector<std::string>{ "X", "Y" }, 1, true);
        aurora::Json props;
        rg->serialize_props(props);
        AURORA_TEST_CHECK(props["options"].size() == 2);
        AURORA_TEST_CHECK(props["selected_index"].get<int>() == 1);
        AURORA_TEST_CHECK(props["horizontal"].get<bool>());

        auto rg2 = std::make_shared<RadioGroup>();
        rg2->deserialize_props(props);
        AURORA_TEST_CHECK(rg2->option_count() == 2);
        AURORA_TEST_CHECK(rg2->selected_index() == 1);
    }

    // ==================== SpinBox ====================

    // ---- 6. 构造与钳制 ----
    {
        auto sb = std::make_shared<SpinBox>(50.0, 0.0, 100.0, 5.0);
        AURORA_TEST_CHECK(sb->value_of() == 50.0);
        AURORA_TEST_CHECK(sb->min_value() == 0.0);
        AURORA_TEST_CHECK(sb->max_value() == 100.0);
        AURORA_TEST_CHECK(sb->step() == 5.0);

        // 初值越界钳制
        auto sb2 = std::make_shared<SpinBox>(999.0, 0.0, 10.0);
        AURORA_TEST_CHECK(sb2->value_of() == 10.0);

        // max < min 修正
        auto sb3 = std::make_shared<SpinBox>(5.0, 10.0, 3.0);
        AURORA_TEST_CHECK(sb3->max_value() == 10.0);

        // 非正步长降级为 1
        auto sb4 = std::make_shared<SpinBox>(0.0, 0.0, 10.0, -2.0);
        AURORA_TEST_CHECK(sb4->step() == 1.0);
    }

    // ---- 7. increment/decrement 与边界 ----
    {
        auto sb = std::make_shared<SpinBox>(95.0, 0.0, 100.0, 10.0);
        double last = -1.0;
        sb->set_on_change([&last](double v) -> void { last = v; });

        sb->increment(); // 95+10=105 → 钳到 100
        AURORA_TEST_CHECK(sb->value_of() == 100.0);
        AURORA_TEST_CHECK(last == 100.0);

        sb->increment(); // 已到顶不变、不回调
        last = -1.0;
        sb->increment();
        AURORA_TEST_CHECK(last == -1.0);

        sb->decrement();
        AURORA_TEST_CHECK(sb->value_of() == 90.0);
    }

    // ---- 8. display_text 前后缀与小数 ----
    {
        auto sb = std::make_shared<SpinBox>(42.0, 0.0, 100.0);
        AURORA_TEST_CHECK(sb->display_text() == "42");

        sb->set_prefix("$").set_suffix(" USD").set_decimals(2);
        AURORA_TEST_CHECK(sb->display_text() == "$42.00 USD");
    }

    // ---- 9. 点击箭头区调节 ----
    {
        auto sb = std::make_shared<SpinBox>(5.0, 0.0, 10.0, 1.0);
        BuildContext ctx;
        sb->mount(ctx);
        Constraints c;
        c.min = Size{ .width = 0.0f, .height = 0.0f };
        c.max = Size{ .width = 320.0f, .height = 240.0f };
        const Size s = sb->layout(c, ctx);

        // 点右上（上箭头 = increment）
        MouseEvent up;
        up.action = MouseAction::Press;
        up.local_position = Point{ .x = s.width - 5.0f, .y = 5.0f };
        sb->on_pointer_event(up);
        AURORA_TEST_CHECK(up.handled);
        AURORA_TEST_CHECK(sb->value_of() == 6.0);

        // 点右下（下箭头 = decrement）
        MouseEvent dn;
        dn.action = MouseAction::Press;
        dn.local_position = Point{ .x = s.width - 5.0f, .y = s.height - 5.0f };
        sb->on_pointer_event(dn);
        AURORA_TEST_CHECK(sb->value_of() == 5.0);

        // 点文本区不调节
        MouseEvent mid;
        mid.action = MouseAction::Press;
        mid.local_position = Point{ .x = 10.0f, .y = 15.0f };
        sb->on_pointer_event(mid);
        AURORA_TEST_CHECK(sb->value_of() == 5.0);
    }

    // ---- 10. SpinBox 序列化往返 ----
    {
        auto sb = std::make_shared<SpinBox>(7.5, 0.0, 20.0, 0.5);
        sb->set_prefix(">").set_decimals(1);
        aurora::Json props;
        sb->serialize_props(props);
        AURORA_TEST_CHECK(props["value"].get<double>() == 7.5);
        AURORA_TEST_CHECK(props["step"].get<double>() == 0.5);

        auto sb2 = std::make_shared<SpinBox>();
        sb2->deserialize_props(props);
        AURORA_TEST_CHECK(sb2->value_of() == 7.5);
        AURORA_TEST_CHECK(sb2->display_text() == ">7.5");
    }

    // ---- 11. 无头渲染不崩溃 ----
    {
        auto rg = std::make_shared<RadioGroup>(std::vector<std::string>{ "A", "B" });
        auto sb = std::make_shared<SpinBox>(1.0, 0.0, 9.0);
        BuildContext ctx;
        rg->mount(ctx);
        sb->mount(ctx);
        Constraints c;
        c.min = Size{ .width = 0.0f, .height = 0.0f };
        c.max = Size{ .width = 320.0f, .height = 240.0f };
        rg->layout(c, ctx);
        sb->layout(c, ctx);

        aurora::Painter p;
        p.begin(320, 240);
        rg->paint(p, Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 150.0f, .height = 60.0f } },
                  ctx);
        sb->paint(p,
                  Rect{ .origin = Point{ .x = 0.0f, .y = 100.0f }, .size = Size{ .width = 120.0f, .height = 30.0f } },
                  ctx);
        AURORA_TEST_CHECK(p.width() == 320);
    }

    // ---- 12. 现代化属性：禁用态 / 主题回退 / 新属性往返 ----
    {
        // RadioGroup 禁用：点击不切换
        auto rg = std::make_shared<RadioGroup>(std::vector<std::string>{ "A", "B" }, 0);
        rg->set_enabled(false);
        MouseEvent e;
        e.action = MouseAction::Press;
        e.local_position = Point{ .x = 10.0f, .y = 28.0f + 14.0f }; // 第二行
        rg->on_pointer_event(e);
        AURORA_TEST_CHECK(rg->selected_index() == 0);
        AURORA_TEST_CHECK(!rg->enabled());

        // active_color 未设置不序列化（跟随主题）；新属性往返
        aurora::Json j0;
        rg->serialize_props(j0);
        AURORA_TEST_CHECK(!j0.contains("active_color"));

        rg->set_active_color(aurora::Color::red())
            .set_text_color(aurora::Color{ 1, 2, 3, 255 })
            .set_dot_size(20.0f)
            .set_row_height(32.0f)
            .set_font_size(15.0f);
        aurora::Json j;
        rg->serialize_props(j);
        AURORA_TEST_CHECK(j["active_color"][0].get<int>() == 255);
        AURORA_TEST_CHECK(j["dot_size"].get<double>() == 20.0);
        AURORA_TEST_CHECK(j["row_height"].get<double>() == 32.0);

        auto rg2 = std::make_shared<RadioGroup>();
        rg2->deserialize_props(j);
        aurora::Json k;
        rg2->serialize_props(k);
        AURORA_TEST_CHECK(k["font_size"].get<double>() == 15.0);
        AURORA_TEST_CHECK(k["enabled"].get<bool>() == false);
    }

    // ---- 13. SpinBox 现代化：箭头键调节 / 禁用 / 样式属性往返 ----
    {
        auto sb = std::make_shared<SpinBox>(5.0, 0.0, 10.0, 1.0);
        aurora::KeyEvent up;
        up.action = aurora::KeyAction::Down;
        up.key = static_cast<int>(aurora::KeyCode::ArrowUp);
        sb->on_key_event(up);
        AURORA_TEST_CHECK(sb->value_of() == 6.0);
        aurora::KeyEvent dn;
        dn.action = aurora::KeyAction::Down;
        dn.key = static_cast<int>(aurora::KeyCode::ArrowDown);
        sb->on_key_event(dn);
        AURORA_TEST_CHECK(sb->value_of() == 5.0);

        // 禁用：箭头键/点击均无效
        sb->set_enabled(false);
        sb->on_key_event(up);
        AURORA_TEST_CHECK(sb->value_of() == 5.0);

        // 样式属性往返
        sb->set_background(aurora::Color{ 9, 9, 9, 255 })
            .set_border_color(aurora::Color{ 8, 8, 8, 255 })
            .set_text_color(aurora::Color{ 7, 7, 7, 255 })
            .set_arrow_color(aurora::Color{ 6, 6, 6, 255 })
            .set_corner_radius(2.0f)
            .set_font_size(11.0f);
        aurora::Json j;
        sb->serialize_props(j);
        AURORA_TEST_CHECK(j["background"][0].get<int>() == 9);
        AURORA_TEST_CHECK(j["corner_radius"].get<double>() == 2.0);
        AURORA_TEST_CHECK(j["enabled"].get<bool>() == false);

        auto sb2 = std::make_shared<SpinBox>();
        sb2->deserialize_props(j);
        AURORA_TEST_CHECK(!sb2->enabled());
        aurora::Json k;
        sb2->serialize_props(k);
        AURORA_TEST_CHECK(k["font_size"].get<double>() == 11.0);
    }
}
