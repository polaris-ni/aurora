// 验证 Popup/OverlayHost：打开/关闭、锚定布局、外部点击关闭、z-order 命中。

#include <memory>

#include "aurora/widget/button.h"
#include "aurora/widget/popup.h"
#include "aurora/widget/text.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Button;
using aurora::ButtonProps;
using aurora::Constraints;
using aurora::LocalizedString;
using aurora::Node;
using aurora::OverlayHost;
using aurora::Point;
using aurora::Popup;
using aurora::Rect;
using aurora::Size;
using aurora::Text;

namespace {

auto make_text(const char *s) -> Node {
    auto t = Text();
    t.content = LocalizedString{ s };
    return Node(std::move(t));
}

} // namespace

AURORA_TEST() {
    // ---- 1. Popup 初始关闭，常规流零尺寸 ----
    {
        auto popup = std::make_shared<Popup>(make_text("menu"));
        AURORA_TEST_CHECK(!popup->is_open());

        BuildContext ctx;
        popup->mount(ctx);
        Constraints c;
        c.min = Size{ .width = 0.0f, .height = 0.0f };
        c.max = Size{ .width = 640.0f, .height = 480.0f };
        const Size s = popup->layout(c, ctx);
        AURORA_TEST_CHECK(s.width == 0.0f);
        AURORA_TEST_CHECK(s.height == 0.0f);
    }

    // ---- 2. open_at 打开并布局内容 ----
    {
        auto popup = std::make_shared<Popup>(make_text("hello popup"));
        popup->open_at(Point{ .x = 100.0f, .y = 50.0f });
        AURORA_TEST_CHECK(popup->is_open());
        AURORA_TEST_CHECK(popup->anchor().x == 100.0f);
        AURORA_TEST_CHECK(popup->anchor().y == 50.0f);

        BuildContext ctx;
        popup->mount(ctx);
        Constraints c;
        c.min = Size{ .width = 0.0f, .height = 0.0f };
        c.max = Size{ .width = 640.0f, .height = 480.0f };
        popup->layout(c, ctx);

        // 内容盒有实际尺寸
        const Rect cb = popup->content_bounds();
        AURORA_TEST_CHECK(cb.origin.x == 100.0f);
        AURORA_TEST_CHECK(cb.size.width > 0.0f);
        AURORA_TEST_CHECK(cb.size.height > 0.0f);
    }

    // ---- 3. close 触发回调 ----
    {
        auto popup = std::make_shared<Popup>(make_text("x"));
        int closed = 0;
        popup->set_on_close([&closed]() -> void { ++closed; });

        popup->open_at(Point{ .x = 0.0f, .y = 0.0f });
        popup->close();
        AURORA_TEST_CHECK(closed == 1);
        AURORA_TEST_CHECK(!popup->is_open());

        // 已关闭再 close 不重复触发
        popup->close();
        AURORA_TEST_CHECK(closed == 1);
    }

    // ---- 4. handle_outside_click：外部点击关闭 ----
    {
        auto popup = std::make_shared<Popup>(make_text("dismiss me"));
        popup->open_at(Point{ .x = 100.0f, .y = 100.0f });

        BuildContext ctx;
        popup->mount(ctx);
        Constraints c;
        c.min = Size{ .width = 0.0f, .height = 0.0f };
        c.max = Size{ .width = 640.0f, .height = 480.0f };
        popup->layout(c, ctx);

        // 点击内容内部：不关闭
        const Rect cb = popup->content_bounds();
        const Point inside{ .x = cb.origin.x + 1.0f, .y = cb.origin.y + 1.0f };
        AURORA_TEST_CHECK(!popup->handle_outside_click(inside));
        AURORA_TEST_CHECK(popup->is_open());

        // 点击外部：关闭
        AURORA_TEST_CHECK(popup->handle_outside_click(Point{ 500.0f, 400.0f }));
        AURORA_TEST_CHECK(!popup->is_open());
    }

    // ---- 5. dismiss_on_outside_click=false 时点击外部不关闭 ----
    {
        auto popup = std::make_shared<Popup>(make_text("sticky"));
        popup->set_dismiss_on_outside_click(false);
        popup->open_at(Point{ .x = 10.0f, .y = 10.0f });

        BuildContext ctx;
        popup->mount(ctx);
        Constraints c;
        c.min = Size{ .width = 0.0f, .height = 0.0f };
        c.max = Size{ .width = 640.0f, .height = 480.0f };
        popup->layout(c, ctx);

        AURORA_TEST_CHECK(!popup->handle_outside_click(Point{ 600.0f, 400.0f }));
        AURORA_TEST_CHECK(popup->is_open());
    }

    // ---- 6. OverlayHost 基础结构 ----
    {
        auto host = std::make_shared<OverlayHost>(make_text("base content"));
        AURORA_TEST_CHECK(host->overlay_count() == 0);

        auto popup = Popup(make_text("overlay 1"));
        popup.open_at(Point{ .x = 50.0f, .y = 50.0f });
        const std::size_t idx = host->add_overlay(Node(std::move(popup)));
        AURORA_TEST_CHECK(idx == 1);
        AURORA_TEST_CHECK(host->overlay_count() == 1);

        host->remove_overlay(idx);
        AURORA_TEST_CHECK(host->overlay_count() == 0);

        // 序号 0（基础内容）不可移除
        host->remove_overlay(0);
        AURORA_TEST_CHECK(host->overlay_count() == 0);
    }

    // ---- 7. OverlayHost 布局与外部点击分发 ----
    {
        auto host = std::make_shared<OverlayHost>(make_text("base"));

        auto popup = Popup(make_text("floating"));
        popup.open_at(Point{ .x = 200.0f, .y = 100.0f });
        host->add_overlay(Node(std::move(popup)));

        BuildContext ctx;
        host->mount(ctx);
        Constraints c;
        c.min = Size{ .width = 0.0f, .height = 0.0f };
        c.max = Size{ .width = 640.0f, .height = 480.0f };
        const Size s = host->layout(c, ctx);
        AURORA_TEST_CHECK(s.width == 640.0f);
        AURORA_TEST_CHECK(s.height == 480.0f);

        // 外部点击关闭浮层
        AURORA_TEST_CHECK(host->handle_outside_click(Point{ 600.0f, 400.0f }));
        // 再次点击无浮层可关
        AURORA_TEST_CHECK(!host->handle_outside_click(Point{ 600.0f, 400.0f }));
    }

    // ---- 8. Popup 命中测试（打开时命中内容按钮）----
    {
        int clicked = 0;
        auto btn = Button(ButtonProps{ .label = "hit me" });
        btn.set_on_click([&clicked]() -> void { ++clicked; });

        auto popup = std::make_shared<Popup>(Node(std::move(btn)));
        popup->open_at(Point{ .x = 100.0f, .y = 100.0f });

        BuildContext ctx;
        popup->mount(ctx);
        Constraints c;
        c.min = Size{ .width = 0.0f, .height = 0.0f };
        c.max = Size{ .width = 640.0f, .height = 480.0f };
        popup->layout(c, ctx);

        // Popup 布局盒在原点（零尺寸），命中需将全局坐标映射到内容
        constexpr Rect popup_bounds{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                                     .size = Size{ .width = 0.0f, .height = 0.0f } };
        const Rect content = popup->content_bounds();
        const Point hit_global{ .x = content.origin.x + (content.size.width / 2.0f),
                                .y = content.origin.y + (content.size.height / 2.0f) };
        // local == global（popup 布局盒原点在 0,0）
        aurora::Widget *hit = popup->hit_test(hit_global, popup_bounds, ctx);
        AURORA_TEST_CHECK(hit != nullptr);

        // 关闭后不命中
        popup->close();
        aurora::Widget *hit2 = popup->hit_test(hit_global, popup_bounds, ctx);
        AURORA_TEST_CHECK(hit2 == nullptr);
    }

    // ---- 9. 序列化往返 ----
    {
        auto popup = std::make_shared<Popup>(make_text("ser"));
        popup->open_at(Point{ .x = 30.0f, .y = 40.0f });
        popup->set_dismiss_on_outside_click(false);

        aurora::Json props;
        popup->serialize_props(props);
        AURORA_TEST_CHECK(props["open"].get<bool>());
        AURORA_TEST_CHECK(props["anchor_x"].get<float>() == 30.0f);
        AURORA_TEST_CHECK(props["anchor_y"].get<float>() == 40.0f);
        AURORA_TEST_CHECK(!props["dismiss_on_outside_click"].get<bool>());

        auto popup2 = std::make_shared<Popup>(make_text("de"));
        popup2->deserialize_props(props);
        AURORA_TEST_CHECK(popup2->is_open());
        AURORA_TEST_CHECK(popup2->anchor().x == 30.0f);
        AURORA_TEST_CHECK(!popup2->dismiss_on_outside_click());
    }
}
