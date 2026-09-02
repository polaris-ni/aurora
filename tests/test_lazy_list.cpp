// 验证 LazyList 虚拟滚动：可见范围计算、实例回收、滚动、滚轮事件、命中。

#include <cstdio>
#include <memory>
#include <string>

#include "aurora/widget/lazy_list.h"
#include "aurora/widget/text.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Constraints;
using aurora::LazyList;
using aurora::LocalizedString;
using aurora::Node;
using aurora::Point;
using aurora::Rect;
using aurora::ScrollEvent;
using aurora::Size;
using aurora::Text;
using aurora::Widget;

namespace {

auto make_builder() -> LazyList::ItemBuilder {
    return [](int i) -> Node {
        auto t = Text();
        t.content = LocalizedString{ "item " + std::to_string(i) };
        return Node(std::move(t));
    };
}

auto layout_list(std::shared_ptr<LazyList> const &list, const float w, float h) -> void {
    constexpr BuildContext ctx;
    list->mount(ctx);
    Constraints c;
    c.min = Size{ .width = 0.0f, .height = 0.0f };
    c.max = Size{ .width = w, .height = h };
    list->layout(c, ctx);
}

} // namespace

AURORA_TEST() {
    // ---- 1. 基本构造 ----
    {
        auto list = std::make_shared<LazyList>(10000, make_builder(), 48.0f);
        AURORA_TEST_CHECK(list->count() == 10000);
        AURORA_TEST_CHECK(list->content_height() == 480000.0f);
        AURORA_TEST_CHECK(list->scroll_offset() == 0.0f);
    }

    // ---- 2. 仅实例化可见窗口（关键虚拟化断言）----
    {
        auto list = std::make_shared<LazyList>(10000, make_builder(), 48.0f);
        layout_list(list, 320.0f, 480.0f); // 视口 480dp = 10 行可见

        // 存活实例 = 可见 10 行 + 缓冲 200dp/48 ≈ 5 行 ≈ 15，远小于 10000
        AURORA_TEST_CHECK(list->live_item_count() > 0);
        AURORA_TEST_CHECK(list->live_item_count() < 30);
        AURORA_TEST_PRINTF("  live items: %zu / 10000\n", list->live_item_count());
    }

    // ---- 3. 可见范围计算 ----
    {
        auto list = std::make_shared<LazyList>(1000, make_builder(), 50.0f);
        list->set_cache_extent(0.0f);      // 无缓冲便于精确断言
        layout_list(list, 320.0f, 500.0f); // 恰好 10 行

        auto [first, last] = list->visible_range();
        AURORA_TEST_CHECK(first == 0);
        AURORA_TEST_CHECK(last == 10);
    }

    // ---- 4. 滚动后窗口移动 + 旧实例回收 ----
    {
        auto list = std::make_shared<LazyList>(1000, make_builder(), 50.0f);
        list->set_cache_extent(0.0f);
        layout_list(list, 320.0f, 500.0f);

        // 滚动到 item 100（offset = 5000）
        list->scroll_to_item(100);
        AURORA_TEST_CHECK(list->scroll_offset() == 5000.0f);

        BuildContext ctx;
        Constraints c;
        c.min = Size{ .width = 0.0f, .height = 0.0f };
        c.max = Size{ .width = 320.0f, .height = 500.0f };
        list->layout(c, ctx);

        auto [first, last] = list->visible_range();
        AURORA_TEST_CHECK(first == 100);
        AURORA_TEST_CHECK(last == 110);
        AURORA_TEST_CHECK(list->live_item_count() == 10); // 旧实例已回收
    }

    // ---- 5. 滚动钳制 ----
    {
        auto list = std::make_shared<LazyList>(100, make_builder(), 50.0f);
        layout_list(list, 320.0f, 500.0f); // 内容 5000，视口 500，最大偏移 4500

        AURORA_TEST_CHECK(list->max_scroll_offset() == 4500.0f);

        list->set_scroll_offset(-100.0f);
        AURORA_TEST_CHECK(list->scroll_offset() == 0.0f);

        list->set_scroll_offset(99999.0f);
        AURORA_TEST_CHECK(list->scroll_offset() == 4500.0f);
    }

    // ---- 6. 内容不足时不可滚动 ----
    {
        auto list = std::make_shared<LazyList>(3, make_builder(), 50.0f);
        layout_list(list, 320.0f, 500.0f); // 内容 150 < 视口 500

        AURORA_TEST_CHECK(list->max_scroll_offset() == 0.0f);
        list->set_scroll_offset(100.0f);
        AURORA_TEST_CHECK(list->scroll_offset() == 0.0f);
        AURORA_TEST_CHECK(list->live_item_count() == 3);
    }

    // ---- 7. 滚轮事件 ----
    {
        auto list = std::make_shared<LazyList>(1000, make_builder(), 50.0f);
        layout_list(list, 320.0f, 500.0f);

        ScrollEvent e;
        e.delta_y = -2.0f; // 向下滚动（delta 负 = 内容上移）
        list->on_scroll(e);
        AURORA_TEST_CHECK(e.handled);
        AURORA_TEST_CHECK(list->scroll_offset() == 80.0f); // 2 * 40dp
    }

    // ---- 8. count=0 空列表 ----
    {
        auto list = std::make_shared<LazyList>(0, make_builder(), 50.0f);
        layout_list(list, 320.0f, 500.0f);
        AURORA_TEST_CHECK(list->live_item_count() == 0);
        auto [first, last] = list->visible_range();
        AURORA_TEST_CHECK(first == 0);
        AURORA_TEST_CHECK(last == 0);
    }

    // ---- 9. 负 count / 非正 item_extent 降级 ----
    {
        auto list = std::make_shared<LazyList>(-5, make_builder(), -10.0f);
        AURORA_TEST_CHECK(list->count() == 0);
        // item_extent 降级为 48
        layout_list(list, 320.0f, 480.0f);
        AURORA_TEST_CHECK(list->live_item_count() == 0);
    }

    // ---- 10. 无头渲染不崩溃 ----
    {
        auto list = std::make_shared<LazyList>(500, make_builder(), 48.0f);
        layout_list(list, 320.0f, 240.0f);

        aurora::Painter p;
        p.begin(320, 240);
        BuildContext ctx;
        list->paint(
            p, Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 320.0f, .height = 240.0f } }, ctx);
        AURORA_TEST_CHECK(p.width() == 320);
    }

    // ---- 11. 序列化 ----
    {
        auto list = std::make_shared<LazyList>(200, make_builder(), 32.0f);
        layout_list(list, 320.0f, 480.0f);
        list->set_scroll_offset(64.0f);

        aurora::Json props;
        list->serialize_props(props);
        AURORA_TEST_CHECK(props["count"].get<int>() == 200);
        AURORA_TEST_CHECK(props["item_extent"].get<float>() == 32.0f);
        AURORA_TEST_CHECK(props["scroll_offset"].get<float>() == 64.0f);
    }

    // ---- 12. 命中测试返回自身（确保滚轮事件派发到列表而非子项） ----
    {
        auto list = std::make_shared<LazyList>(1000, make_builder(), 50.0f);
        layout_list(list, 320.0f, 500.0f); // 视口 500dp，可见 item 0..9/10

        constexpr Rect bounds{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                               .size = Size{ .width = 320.0f, .height = 500.0f } };
        BuildContext ctx;
        // 落在子项 item 2（y≈100~150）区域内：滚轮命中必须解析为列表自身，
        // 否则 ScrollEvent 派发到叶控件后为空操作，列表无法滚动。
        Widget *hit_item = list->hit_test(Point{ .x = 100.0f, .y = 120.0f }, bounds, ctx);
        AURORA_TEST_CHECK(hit_item == list.get());
        // 视口外不应命中
        Widget *hit_out = list->hit_test(Point{ .x = 100.0f, .y = 600.0f }, bounds, ctx);
        AURORA_TEST_CHECK(hit_out == nullptr);
    }
}
