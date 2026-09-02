// test_bottom_nav_bar.cpp — 校验框架底部导航栏 BottomNavBar：选中态、on_select 点击回调、
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

#include <memory>

#include "aurora/aurora.h"
#include "aurora/widget/bottom_nav_bar.h"
#include "aurora/window/window.h"

#include "test_harness.h"

namespace au = aurora;

static auto make_nav() -> std::shared_ptr<au::BottomNavBar> {
    std::vector<au::BottomNavItem> items;
    for (int i = 0; i < 4; ++i) {
        const int idx = i;
        items.push_back(au::BottomNavItem{
            .icon = [](au::Painter &p, const au::Rect &b, bool) -> void {
                p.fill_rect(b, au::Color{ 0x1A, 0x73, 0xE8, 0xFF });
            },
            .label = "Tab" + std::to_string(idx),
        });
    }
    return std::make_shared<au::BottomNavBar>(au::BottomNavBarProps{
        .items = items,
        .selected_index = 0,
        .on_select = [](int) -> void {},
    });
}

AURORA_TEST() {
    // 选中态属性。
    {
        auto nav = make_nav();
        nav->set_selected_index(2);
        au::Node node{ nav };
        node.set_bounds(au::Rect{ .origin = au::Point{ .x = 0.0f, .y = 0.0f },
                                  .size = au::Size{ .width = 400.0f, .height = 64.0f } });
        // 布局以获得尺寸。
        au::HeadlessOptions opts;
        opts.size = au::Size{ .width = 400.0f, .height = 64.0f };
        auto res = au::create_window(opts);
        if (res) {
            auto win = std::move(res.value());
            auto r = win->present_root(node);
            AURORA_TEST_CHECK(static_cast<bool>(r));
            AURORA_TEST_CHECK(win->surface().frame_count() == 1);
        }
    }

    // on_select：4 等分中点击第 4 项（index 3）中心。
    {
        auto nav = make_nav();
        int sel = -1;
        nav->set_on_select([&sel](int i) -> void { sel = i; });
        au::Node node{ nav };
        au::HeadlessOptions opts;
        opts.size = au::Size{ .width = 400.0f, .height = 64.0f };
        opts.png_path = "build/test_bottom_nav_bar_click.png";
        auto res = au::create_window(opts);
        AURORA_TEST_CHECK(static_cast<bool>(res));
        if (res) {
            auto win = std::move(res.value());
            auto r = win->present_root(node);
            AURORA_TEST_CHECK(static_cast<bool>(r));
            // present_root 会按窗口根重新布局并重置 widget 几何；派发点击前重新手动布局，
            // 使 on_pointer_event 能依据 size().width 正确计算选中项索引。
            au::BuildContext ctx;
            nav->layout(au::Constraints{ .min = au::Size{ .width = 0.0f, .height = 0.0f },
                                         .max = au::Size{ .width = 400.0f, .height = 64.0f } },
                        ctx);
            AURORA_TEST_CHECK(nav->size().width == 400.0f);
            const float item_w = nav->size().width / static_cast<float>(nav->items.size());
            au::MouseEvent down;
            down.local_position = au::Point{ .x = (3.0f * item_w) + (item_w * 0.5f), .y = 32.0f };
            down.action = au::MouseAction::Press;
            nav->on_pointer_event(down);
            au::MouseEvent up;
            up.local_position = au::Point{ .x = (3.0f * item_w) + (item_w * 0.5f), .y = 32.0f };
            up.action = au::MouseAction::Release;
            nav->on_pointer_event(up);
            AURORA_TEST_CHECK(sel == 3);
        }
    }

    // Headless 渲染不崩溃。
    {
        auto nav = make_nav();
        au::HeadlessOptions opts;
        opts.size = au::Size{ .width = 400.0f, .height = 64.0f };
        opts.png_path = "build/test_bottom_nav_bar.png";
        auto res = au::create_window(opts);
        AURORA_TEST_CHECK(static_cast<bool>(res));
        if (res) {
            auto win = std::move(res.value());
            au::Node nav_node{ nav };
            auto r = win->present_root(nav_node);
            AURORA_TEST_CHECK(static_cast<bool>(r));
            AURORA_TEST_CHECK(win->surface().frame_count() == 1);
        }
    }
}
