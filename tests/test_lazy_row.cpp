// test_lazy_row.cpp — 校验框架横向虚拟列表 LazyRow：构造/属性、横向滚动、命中点击、
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

#include <memory>

#include "aurora/aurora.h"
#include "aurora/widget/lazy_row.h"
#include "aurora/window/window.h"

#include "test_harness.h"

namespace au = aurora;

AURORA_TEST() {
    // 构造与属性。
    {
        auto lr = std::make_shared<au::LazyRow>(
            10,
            [](int) -> au::Node {
                return au::Node{ std::make_shared<au::Canvas>(96.0f, 96.0f,
                                                              [](au::Painter &, const au::Rect &) -> void {}) };
            },
            96.0f);
        lr->set_item_count(20);
        lr->set_item_extent(120.0f);
        lr->set_cache_extent(300.0f);
    }

    // on_item_click：在 extent=96、offset=0 时，local x=100 命中 index 1。
    {
        auto lr = std::make_shared<au::LazyRow>(
            10,
            [](int) -> au::Node {
                return au::Node{ std::make_shared<au::Canvas>(96.0f, 96.0f,
                                                              [](au::Painter &, const au::Rect &) -> void {}) };
            },
            96.0f);
        int clicked = -1;
        lr->set_on_item_click([&clicked](int i) -> void { clicked = i; });
        au::Node node{ lr };
        // 先渲染以获得布局与 widget bounds，再派发点击。
        au::HeadlessOptions opts;
        opts.size = au::Size{ .width = 400.0f, .height = 120.0f };
        opts.png_path = "build/test_lazy_row_click.png";
        auto res = au::create_window(opts);
        AURORA_TEST_CHECK(static_cast<bool>(res));
        if (res) {
            auto win = std::move(res.value());
            auto r = win->present_root(node);
            AURORA_TEST_CHECK(static_cast<bool>(r));
            au::MouseEvent down;
            down.local_position = au::Point{ .x = 100.0f, .y = 10.0f };
            down.action = au::MouseAction::Press;
            lr->on_pointer_event(down);
            au::MouseEvent up;
            up.local_position = au::Point{ .x = 100.0f, .y = 10.0f };
            up.action = au::MouseAction::Release;
            lr->on_pointer_event(up);
            AURORA_TEST_CHECK(clicked == 1);
        }
    }

    // Headless 渲染不崩溃（含圆角裁剪父容器，回归 LazyList 同类越界问题）。
    {
        auto lr = std::make_shared<au::LazyRow>(
            30,
            [](int i) -> au::Node {
                const auto c =
                    std::make_shared<au::Canvas>(96.0f, 96.0f, [i](au::Painter &p, const au::Rect &b) -> void {
                        p.fill_rect(b, au::Color{ static_cast<uint8_t>(i * 8 % 256), 0x80, 0xC0, 0xFF });
                    });
                return au::Node{ c };
            },
            96.0f);
        auto wrap = std::make_shared<au::Row>();
        wrap->add(au::Node{ lr });
        wrap->modifier = au::Modifier{}.clip_rounded(24.0f).background(au::Color{ 0xFF, 0xFF, 0xFF, 0xFF });

        au::HeadlessOptions opts;
        opts.size = au::Size{ .width = 420.0f, .height = 140.0f };
        opts.png_path = "build/test_lazy_row.png";
        auto res = au::create_window(opts);
        AURORA_TEST_CHECK(static_cast<bool>(res));
        if (res) {
            auto win = std::move(res.value());
            au::Node root_node{ wrap };
            auto r = win->present_root(root_node);
            AURORA_TEST_CHECK(static_cast<bool>(r));
            AURORA_TEST_CHECK(win->surface().frame_count() == 1);
        }
    }
}
