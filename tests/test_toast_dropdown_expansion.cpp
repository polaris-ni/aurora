// 验证三控件：ToastHost（队列/过期/位置）、Dropdown（开合/选择）、ExpansionPanel（折叠）。
// ── API 覆盖映射 ─────────────────────────────
// widget/dropdown.h(Dropdown)、widget/toast.h(ToastHost 队列)。

#include <chrono>
#include <memory>

#include "aurora/widget/dropdown.h"
#include "aurora/widget/expansion_panel.h"
#include "aurora/widget/text.h"
#include "aurora/widget/toast.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Constraints;
using aurora::Dropdown;
using aurora::ExpansionPanel;
using aurora::LocalizedString;
using aurora::MouseAction;
using aurora::MouseEvent;
using aurora::Node;
using aurora::Point;
using aurora::Rect;
using aurora::Size;
using aurora::Text;
using aurora::ToastHost;
using aurora::ToastPosition;

namespace {

auto make_text(const char *s) -> Node {
    auto t = Text();
    t.content = LocalizedString{ s };
    return Node(std::move(t));
}

} // namespace

AURORA_TEST() {
    // ==================== ToastHost ====================

    // ---- 1. show 入队与可见列表 ----
    {
        auto host = std::make_shared<ToastHost>(make_text("base"));
        AURORA_TEST_CHECK(host->pending_count() == 0);

        host->show("first", 1000.0f);
        host->show("second", 1000.0f);
        AURORA_TEST_CHECK(host->pending_count() == 2);
        auto vis = host->visible_toasts();
        AURORA_TEST_CHECK(vis.size() == 2);
        AURORA_TEST_CHECK(vis[0] == "first");
    }

    // ---- 2. 同屏上限 3 条（第 4 条排队）----
    {
        auto host = std::make_shared<ToastHost>(make_text("base"));
        for (int i = 0; i < 5; ++i) {
            host->show("t" + std::to_string(i), 1000.0f);
        }
        AURORA_TEST_CHECK(host->pending_count() == 5);
        AURORA_TEST_CHECK(host->visible_toasts().size() == 3);
    }

    // ---- 3. tick 驱动过期出队 ----
    {
        auto host = std::make_shared<ToastHost>(make_text("base"));
        host->show("ephemeral", 100.0f);

        const auto t0 = std::chrono::steady_clock::now();
        host->tick(t0); // 开始计时
        AURORA_TEST_CHECK(host->pending_count() == 1);

        host->tick(t0 + std::chrono::milliseconds(50)); // 未过期
        AURORA_TEST_CHECK(host->pending_count() == 1);

        host->tick(t0 + std::chrono::milliseconds(150)); // 过期
        AURORA_TEST_CHECK(host->pending_count() == 0);
    }

    // ---- 4. 排队候补顶上并重新计时 ----
    {
        auto host = std::make_shared<ToastHost>(make_text("base"));
        for (int i = 0; i < 4; ++i) {
            host->show("q" + std::to_string(i), 100.0f);
        }

        const auto t0 = std::chrono::steady_clock::now();
        host->tick(t0);
        host->tick(t0 + std::chrono::milliseconds(150)); // 前 3 条过期
        AURORA_TEST_CHECK(host->pending_count() == 1);
        AURORA_TEST_CHECK(host->visible_toasts()[0] == "q3");

        // 候补此刻才开始计时：再过 150ms 才过期
        host->tick(t0 + std::chrono::milliseconds(200));
        AURORA_TEST_CHECK(host->pending_count() == 1);
        host->tick(t0 + std::chrono::milliseconds(400));
        AURORA_TEST_CHECK(host->pending_count() == 0);
    }

    // ---- 5. clear 与位置设置、无头渲染 ----
    {
        auto host = std::make_shared<ToastHost>(make_text("base"));
        host->set_position(ToastPosition::Top);
        host->show("visible", 5000.0f);

        BuildContext ctx;
        host->mount(ctx);
        Constraints c;
        c.min = Size{ .width = 0.0f, .height = 0.0f };
        c.max = Size{ .width = 320.0f, .height = 240.0f };
        host->layout(c, ctx);

        aurora::Painter p;
        p.begin(320, 240);
        host->paint(
            p, Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 320.0f, .height = 240.0f } }, ctx);
        AURORA_TEST_CHECK(p.width() == 320);

        host->clear();
        AURORA_TEST_CHECK(host->pending_count() == 0);
    }

    // ==================== Dropdown ====================

    // ---- 6. 构造与选中 ----
    {
        auto dd = std::make_shared<Dropdown>(std::vector<std::string>{ "Small", "Medium", "Large" }, 1);
        AURORA_TEST_CHECK(dd->option_count() == 3);
        AURORA_TEST_CHECK(dd->selected_index() == 1);
        AURORA_TEST_CHECK(dd->selected_text() == "Medium");
        AURORA_TEST_CHECK(!dd->is_open());
    }

    // ---- 7. select 与回调 ----
    {
        auto dd = std::make_shared<Dropdown>(std::vector<std::string>{ "A", "B" });
        int changed = -1;
        dd->set_on_change([&changed](int i) -> void { changed = i; });

        dd->select(1);
        AURORA_TEST_CHECK(dd->selected_index() == 1);
        AURORA_TEST_CHECK(changed == 1);

        // 相同/越界不回调
        changed = -1;
        dd->select(1);
        dd->select(99);
        AURORA_TEST_CHECK(changed == -1);
    }

    // ---- 8. 点击开合与选项选择 ----
    {
        auto dd = std::make_shared<Dropdown>(std::vector<std::string>{ "One", "Two", "Three" });
        BuildContext ctx;
        dd->mount(ctx);
        Constraints c;
        c.min = Size{ .width = 0.0f, .height = 0.0f };
        c.max = Size{ .width = 320.0f, .height = 240.0f };
        dd->layout(c, ctx);

        // 点主框展开
        MouseEvent e1;
        e1.action = MouseAction::Press;
        e1.local_position = Point{ .x = 20.0f, .y = 15.0f };
        dd->on_pointer_event(e1);
        AURORA_TEST_CHECK(e1.handled);
        AURORA_TEST_CHECK(dd->is_open());

        // 点第三个选项（主框 30 + 2*26 + 13）
        MouseEvent e2;
        e2.action = MouseAction::Press;
        e2.local_position = Point{ .x = 20.0f, .y = 30.0f + (26.0f * 2) + 13.0f };
        dd->on_pointer_event(e2);
        AURORA_TEST_CHECK(dd->selected_index() == 2);
        AURORA_TEST_CHECK(!dd->is_open()); // 选择后收起
    }

    // ---- 9. 序列化往返 ----
    {
        auto dd = std::make_shared<Dropdown>(std::vector<std::string>{ "X", "Y" }, 1);
        aurora::Json props;
        dd->serialize_props(props);
        AURORA_TEST_CHECK(props["options"].size() == 2);
        AURORA_TEST_CHECK(props["selected_index"].get<int>() == 1);

        auto dd2 = std::make_shared<Dropdown>();
        dd2->deserialize_props(props);
        AURORA_TEST_CHECK(dd2->option_count() == 2);
        AURORA_TEST_CHECK(dd2->selected_text() == "Y");
    }

    // ---- 9b. 现代化属性：禁用 / 主题回退 / 样式往返 ----
    {
        auto dd = std::make_shared<Dropdown>(std::vector<std::string>{ "A", "B" });
        // 禁用：点击不开合
        dd->set_enabled(false);
        MouseEvent e;
        e.action = MouseAction::Press;
        e.local_position = Point{ .x = 20.0f, .y = 15.0f };
        dd->on_pointer_event(e);
        AURORA_TEST_CHECK(e.handled);
        AURORA_TEST_CHECK(!dd->is_open());

        // accent_color 未设置不序列化（跟随主题 primary）
        aurora::Json j0;
        dd->serialize_props(j0);
        AURORA_TEST_CHECK(!j0.contains("accent_color"));

        dd->set_accent_color(aurora::Color::red())
            .set_box_color(aurora::Color{ 1, 2, 3, 255 })
            .set_border_color(aurora::Color{ 4, 5, 6, 255 })
            .set_text_color(aurora::Color{ 7, 8, 9, 255 })
            .set_box_height(36.0f)
            .set_item_height(30.0f)
            .set_font_size(15.0f)
            .set_corner_radius(6.0f)
            .set_placeholder("pick");
        aurora::Json j;
        dd->serialize_props(j);
        AURORA_TEST_CHECK(j["accent_color"][0].get<int>() == 255);
        AURORA_TEST_CHECK(j["box_height"].get<double>() == 36.0);
        AURORA_TEST_CHECK(j["item_height"].get<double>() == 30.0);
        AURORA_TEST_CHECK(j["placeholder"].get<std::string>() == "pick");
        AURORA_TEST_CHECK(j["enabled"].get<bool>() == false);

        auto dd2 = std::make_shared<Dropdown>();
        dd2->deserialize_props(j);
        AURORA_TEST_CHECK(!dd2->enabled());
        aurora::Json k;
        dd2->serialize_props(k);
        AURORA_TEST_CHECK(k["font_size"].get<double>() == 15.0);
        AURORA_TEST_CHECK(k["corner_radius"].get<double>() == 6.0);

        // box_height 变更后，开合命中区随之变化：y=32 仍在主框内（36 高）
        auto dd3 = std::make_shared<Dropdown>(std::vector<std::string>{ "A", "B" });
        dd3->set_box_height(36.0f);
        BuildContext ctx;
        dd3->mount(ctx);
        Constraints c;
        c.min = Size{ .width = 0.0f, .height = 0.0f };
        c.max = Size{ .width = 320.0f, .height = 240.0f };
        dd3->layout(c, ctx);
        MouseEvent e2;
        e2.action = MouseAction::Press;
        e2.local_position = Point{ .x = 20.0f, .y = 32.0f };
        dd3->on_pointer_event(e2);
        AURORA_TEST_CHECK(dd3->is_open()); // 若仍按旧 30 高判定，此点会落入选项区
    }

    // ==================== ExpansionPanel ====================

    // ---- 10. 构造与折叠布局 ----
    {
        auto ep = std::make_shared<ExpansionPanel>("Details", make_text("hidden content"), false);
        AURORA_TEST_CHECK(!ep->is_expanded());
        AURORA_TEST_CHECK(ep->header() == "Details");

        BuildContext ctx;
        ep->mount(ctx);
        Constraints c;
        c.min = Size{ .width = 0.0f, .height = 0.0f };
        c.max = Size{ .width = 320.0f, .height = 480.0f };
        const Size collapsed = ep->layout(c, ctx);
        AURORA_TEST_CHECK(collapsed.height == 36.0f); // 仅头部

        // 展开后高度增加
        ep->set_expanded(true);
        const Size expanded = ep->layout(c, ctx);
        AURORA_TEST_CHECK(expanded.height > 36.0f);
    }

    // ---- 11. toggle 与回调 ----
    {
        auto ep = std::make_shared<ExpansionPanel>("P", make_text("c"), false);
        bool last = false;
        int calls = 0;
        ep->set_on_toggle([&](bool v) -> void {
            last = v;
            ++calls;
        });

        ep->toggle();
        AURORA_TEST_CHECK(ep->is_expanded());
        AURORA_TEST_CHECK(last && calls == 1);

        // 相同值不重复回调
        ep->set_expanded(true);
        AURORA_TEST_CHECK(calls == 1);

        ep->toggle();
        AURORA_TEST_CHECK(!ep->is_expanded());
        AURORA_TEST_CHECK(calls == 2);
    }

    // ---- 12. 点击标题头切换 ----
    {
        auto ep = std::make_shared<ExpansionPanel>("Click me", make_text("c"), false);
        BuildContext ctx;
        ep->mount(ctx);
        Constraints c;
        c.min = Size{ .width = 0.0f, .height = 0.0f };
        c.max = Size{ .width = 320.0f, .height = 480.0f };
        ep->layout(c, ctx);

        MouseEvent e;
        e.action = MouseAction::Press;
        e.local_position = Point{ .x = 50.0f, .y = 18.0f };
        ep->on_pointer_event(e);
        AURORA_TEST_CHECK(e.handled);
        AURORA_TEST_CHECK(ep->is_expanded());
    }

    // ---- 13. 序列化往返 ----
    {
        auto ep = std::make_shared<ExpansionPanel>("Ser", make_text("c"), true);
        aurora::Json props;
        ep->serialize_props(props);
        AURORA_TEST_CHECK(props["header"].get<std::string>() == "Ser");
        AURORA_TEST_CHECK(props["expanded"].get<bool>());

        auto ep2 = std::make_shared<ExpansionPanel>();
        ep2->deserialize_props(props);
        AURORA_TEST_CHECK(ep2->header() == "Ser");
        AURORA_TEST_CHECK(ep2->is_expanded());
    }
}
