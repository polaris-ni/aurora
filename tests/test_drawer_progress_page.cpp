// 验证三控件：Drawer（开合/遮罩关闭/永久模式）、ProgressDialog（进度/取消）、
// PageView（翻页/滑动手势/指示器）。
// ── API 覆盖映射 ─────────────────────────────
// widget/drawer.h(Drawer/ProgressDialog/PageView 三控件)。

#include <memory>

#include "aurora/widget/drawer.h"
#include "aurora/widget/text.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Constraints;
using aurora::Drawer;
using aurora::DrawerSide;
using aurora::LocalizedString;
using aurora::MouseAction;
using aurora::MouseEvent;
using aurora::Node;
using aurora::PageView;
using aurora::Point;
using aurora::ProgressDialog;
using aurora::Rect;
using aurora::Size;
using aurora::Text;

namespace {

auto make_text(const char *s) -> Node {
    auto t = Text();
    t.content = LocalizedString{ s };
    return Node(std::move(t));
}

template<typename W> auto layout_widget(std::shared_ptr<W> &w, float width, float height) -> void {
    BuildContext ctx;
    w->mount(ctx);
    Constraints c;
    c.min = Size{ .width = 0.0f, .height = 0.0f };
    c.max = Size{ .width = width, .height = height };
    w->layout(c, ctx);
}

} // namespace

AURORA_TEST() {
    // ==================== Drawer ====================

    // ---- 1. 开合与回调 ----
    {
        auto d = std::make_shared<Drawer>(make_text("main"), make_text("side"), DrawerSide::Left, 200.0f);
        AURORA_TEST_CHECK(!d->is_open());

        bool last = false;
        int calls = 0;
        d->set_on_toggle([&](bool v) -> void {
            last = v;
            ++calls;
        });

        d->toggle();
        AURORA_TEST_CHECK(d->is_open());
        AURORA_TEST_CHECK(last && calls == 1);

        d->set_open(true); // 相同不重复回调
        AURORA_TEST_CHECK(calls == 1);
        d->set_open(false);
        AURORA_TEST_CHECK(calls == 2);
    }

    // ---- 2. 面板矩形（左/右侧）----
    {
        auto dl = std::make_shared<Drawer>(make_text("m"), make_text("s"), DrawerSide::Left, 200.0f);
        layout_widget(dl, 640.0f, 480.0f);
        const Rect pl = dl->panel_rect();
        AURORA_TEST_CHECK(pl.origin.x == 0.0f);
        AURORA_TEST_CHECK(pl.size.width == 200.0f);

        auto dr = std::make_shared<Drawer>(make_text("m"), make_text("s"), DrawerSide::Right, 200.0f);
        layout_widget(dr, 640.0f, 480.0f);
        const Rect pr = dr->panel_rect();
        AURORA_TEST_CHECK(pr.origin.x == 440.0f);
    }

    // ---- 3. 模态打开时点击遮罩关闭 ----
    {
        auto d = std::make_shared<Drawer>(make_text("m"), make_text("s"), DrawerSide::Left, 200.0f);
        layout_widget(d, 640.0f, 480.0f);
        d->set_open(true);
        layout_widget(d, 640.0f, 480.0f);

        MouseEvent e;
        e.action = MouseAction::Press;
        e.local_position = Point{ .x = 500.0f, .y = 200.0f }; // 面板(0..200)外
        d->on_pointer_event(e);
        AURORA_TEST_CHECK(e.handled);
        AURORA_TEST_CHECK(!d->is_open());
    }

    // ---- 4. 永久模式：无开合、内容让位 ----
    {
        auto d = std::make_shared<Drawer>(make_text("m"), make_text("s"), DrawerSide::Left, 200.0f);
        d->set_permanent(true);
        AURORA_TEST_CHECK(d->is_permanent());

        d->toggle(); // 无效
        AURORA_TEST_CHECK(!d->is_open());

        layout_widget(d, 640.0f, 480.0f);
        const auto kids = d->child_nodes();
        // 内容让出面板宽度：宽 440，从 x=200 开始
        AURORA_TEST_CHECK(kids[0].bounds().origin.x == 200.0f);
        AURORA_TEST_CHECK(kids[0].bounds().size.width == 440.0f);
        // 面板占左侧
        AURORA_TEST_CHECK(kids[1].bounds().origin.x == 0.0f);
        AURORA_TEST_CHECK(kids[1].bounds().size.width == 200.0f);
    }

    // ---- 5. Drawer 序列化 ----
    {
        auto d = std::make_shared<Drawer>(make_text("m"), make_text("s"), DrawerSide::Right, 180.0f);
        d->set_open(true);
        aurora::Json props;
        d->serialize_props(props);
        AURORA_TEST_CHECK(props["open"].get<bool>());
        AURORA_TEST_CHECK(props["side"].get<std::string>() == "right");
        AURORA_TEST_CHECK(props["panel_width"].get<float>() == 180.0f);
    }

    // ==================== ProgressDialog ====================

    // ---- 6. show/close 与进度 ----
    {
        auto pd = std::make_shared<ProgressDialog>("Loading...", true);
        AURORA_TEST_CHECK(!pd->is_open());
        AURORA_TEST_CHECK(pd->progress() == -1.0f); // 不确定态

        pd->show();
        AURORA_TEST_CHECK(pd->is_open());

        pd->set_progress(0.5f);
        AURORA_TEST_CHECK(pd->progress() == 0.5f);
        pd->set_progress(2.0f); // 钳制
        AURORA_TEST_CHECK(pd->progress() == 1.0f);
        pd->set_progress(-0.5f); // 负 = 不确定
        AURORA_TEST_CHECK(pd->progress() == -1.0f);

        pd->close();
        AURORA_TEST_CHECK(!pd->is_open());
    }

    // ---- 7. cancel 回调与不可取消 ----
    {
        auto pd = std::make_shared<ProgressDialog>("Working", true);
        int cancelled = 0;
        pd->set_on_cancel([&cancelled]() -> void { ++cancelled; });

        pd->show();
        pd->cancel();
        AURORA_TEST_CHECK(cancelled == 1);
        AURORA_TEST_CHECK(!pd->is_open());

        // 不可取消
        auto pd2 = std::make_shared<ProgressDialog>("Locked", false);
        int c2 = 0;
        pd2->set_on_cancel([&c2]() -> void { ++c2; });
        pd2->show();
        pd2->cancel();
        AURORA_TEST_CHECK(c2 == 0);
        AURORA_TEST_CHECK(pd2->is_open());
    }

    // ---- 8. 打开时模态吞点击 + 渲染不崩溃 ----
    {
        auto pd = std::make_shared<ProgressDialog>("Modal", true);
        pd->show();
        layout_widget(pd, 400.0f, 300.0f);

        MouseEvent e;
        e.action = MouseAction::Press;
        e.local_position = Point{ .x = 10.0f, .y = 10.0f }; // 对话框外
        pd->on_pointer_event(e);
        AURORA_TEST_CHECK(e.handled); // 模态吞掉
        AURORA_TEST_CHECK(pd->is_open());

        aurora::Painter p;
        p.begin(400, 300);
        BuildContext ctx;
        pd->paint(p, Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 400.0f, .height = 300.0f } },
                  ctx);
        AURORA_TEST_CHECK(p.width() == 400);
    }

    // ==================== PageView ====================

    // ---- 9. 翻页与回调 ----
    {
        std::vector<Node> pages;
        pages.push_back(make_text("p0"));
        pages.push_back(make_text("p1"));
        pages.push_back(make_text("p2"));
        auto pv = std::make_shared<PageView>(std::move(pages));
        AURORA_TEST_CHECK(pv->page_count() == 3);
        AURORA_TEST_CHECK(pv->current_page() == 0);

        int changed = -1;
        pv->set_on_page_change([&changed](int i) -> void { changed = i; });

        pv->next();
        AURORA_TEST_CHECK(pv->current_page() == 1);
        AURORA_TEST_CHECK(changed == 1);

        pv->prev();
        AURORA_TEST_CHECK(pv->current_page() == 0);

        pv->prev(); // 到底忽略
        AURORA_TEST_CHECK(pv->current_page() == 0);

        pv->go_to(2);
        AURORA_TEST_CHECK(pv->current_page() == 2);
        pv->go_to(99); // 越界忽略
        AURORA_TEST_CHECK(pv->current_page() == 2);
    }

    // ---- 10. 滑动手势翻页 ----
    {
        std::vector<Node> pages;
        pages.push_back(make_text("a"));
        pages.push_back(make_text("b"));
        auto pv = std::make_shared<PageView>(std::move(pages));
        layout_widget(pv, 400.0f, 300.0f);

        // 左滑超过 1/4 宽（100dp）→ 下一页
        MouseEvent press;
        press.action = MouseAction::Press;
        press.local_position = Point{ .x = 300.0f, .y = 150.0f };
        pv->on_pointer_event(press);

        MouseEvent release;
        release.action = MouseAction::Release;
        release.local_position = Point{ .x = 150.0f, .y = 150.0f }; // dx = -150
        pv->on_pointer_event(release);
        AURORA_TEST_CHECK(pv->current_page() == 1);

        // 右滑回上一页
        MouseEvent press2;
        press2.action = MouseAction::Press;
        press2.local_position = Point{ .x = 100.0f, .y = 150.0f };
        pv->on_pointer_event(press2);
        MouseEvent release2;
        release2.action = MouseAction::Release;
        release2.local_position = Point{ .x = 260.0f, .y = 150.0f }; // dx = +160
        pv->on_pointer_event(release2);
        AURORA_TEST_CHECK(pv->current_page() == 0);

        // 小于阈值不翻页
        MouseEvent press3;
        press3.action = MouseAction::Press;
        press3.local_position = Point{ .x = 200.0f, .y = 150.0f };
        pv->on_pointer_event(press3);
        MouseEvent release3;
        release3.action = MouseAction::Release;
        release3.local_position = Point{ .x = 170.0f, .y = 150.0f }; // dx = -30 < 100
        pv->on_pointer_event(release3);
        AURORA_TEST_CHECK(pv->current_page() == 0);
    }

    // ---- 11. PageView 渲染（含指示器）与序列化 ----
    {
        std::vector<Node> pages;
        pages.push_back(make_text("x"));
        pages.push_back(make_text("y"));
        auto pv = std::make_shared<PageView>(std::move(pages), 1);
        layout_widget(pv, 320.0f, 240.0f);

        aurora::Painter p;
        p.begin(320, 240);
        BuildContext ctx;
        pv->paint(p, Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 320.0f, .height = 240.0f } },
                  ctx);
        AURORA_TEST_CHECK(p.width() == 320);

        aurora::Json props;
        pv->serialize_props(props);
        AURORA_TEST_CHECK(props["current"].get<int>() == 1);
        AURORA_TEST_CHECK(props["show_indicator"].get<bool>());
    }
}
