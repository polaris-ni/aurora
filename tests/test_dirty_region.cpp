// 验证脏区域渲染：DirtyRegionTracker 合并逻辑 + Window 跳帧集成。

#include <memory>

#include "aurora/render/dirty_region.h"
#include "aurora/state/state.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"

#include "test_harness.h"

using aurora::Column;
using aurora::ColumnProps;
using aurora::DirtyRegionTracker;
using aurora::HeadlessSurface;
using aurora::LocalizedString;
using aurora::Node;
using aurora::Point;
using aurora::Rect;
using aurora::Size;
using aurora::State;
using aurora::Text;
using aurora::Window;

namespace {

/// 创建已定尺寸的 Headless 窗口（HeadlessSurface 尺寸由首次 begin_frame 确立）。
auto make_window(int w, const int h) -> Window {
    auto surface = std::make_unique<HeadlessSurface>();
    (void)surface->begin_frame(w, h);
    return Window{ std::move(surface) };
}

} // namespace

AURORA_TEST() {
    // ---- 1. Tracker 基本状态 ----
    {
        DirtyRegionTracker t;
        AURORA_TEST_CHECK(t.is_empty());
        AURORA_TEST_CHECK(!t.is_full());

        t.mark(Rect{ .origin = Point{ .x = 10.0f, .y = 10.0f }, .size = Size{ .width = 50.0f, .height = 50.0f } });
        AURORA_TEST_CHECK(!t.is_empty());
        AURORA_TEST_CHECK(t.rects().size() == 1);

        t.clear();
        AURORA_TEST_CHECK(t.is_empty());
    }

    // ---- 2. 零尺寸矩形忽略 ----
    {
        DirtyRegionTracker t;
        t.mark(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 0.0f, .height = 100.0f } });
        t.mark(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 100.0f, .height = -5.0f } });
        AURORA_TEST_CHECK(t.is_empty());
    }

    // ---- 3. 重叠矩形合并为并集 ----
    {
        DirtyRegionTracker t;
        t.mark(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 50.0f, .height = 50.0f } });
        t.mark(Rect{ .origin = Point{ .x = 25.0f, .y = 25.0f },
                     .size = Size{ .width = 50.0f, .height = 50.0f } }); // 与前者重叠
        AURORA_TEST_CHECK(t.rects().size() == 1);
        const Rect m = t.rects()[0];
        AURORA_TEST_CHECK(m.origin.x == 0.0f && m.origin.y == 0.0f);
        AURORA_TEST_CHECK(m.size.width == 75.0f && m.size.height == 75.0f);
    }

    // ---- 4. 不相交矩形独立保留 ----
    {
        DirtyRegionTracker t;
        t.mark(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 10.0f, .height = 10.0f } });
        t.mark(Rect{ .origin = Point{ .x = 100.0f, .y = 100.0f }, .size = Size{ .width = 10.0f, .height = 10.0f } });
        AURORA_TEST_CHECK(t.rects().size() == 2);

        // merged_bounds 是两者包围盒
        const Rect mb = t.merged_bounds();
        AURORA_TEST_CHECK(mb.origin.x == 0.0f);
        AURORA_TEST_CHECK(mb.size.width == 110.0f);
    }

    // ---- 5. 连锁合并（第三个矩形桥接前两个）----
    {
        DirtyRegionTracker t;
        t.mark(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 10.0f, .height = 10.0f } });
        t.mark(Rect{ .origin = Point{ .x = 20.0f, .y = 0.0f }, .size = Size{ .width = 10.0f, .height = 10.0f } });
        AURORA_TEST_CHECK(t.rects().size() == 2);
        // 桥接两者
        t.mark(Rect{ .origin = Point{ .x = 5.0f, .y = 0.0f }, .size = Size{ .width = 20.0f, .height = 10.0f } });
        AURORA_TEST_CHECK(t.rects().size() == 1);
        AURORA_TEST_CHECK(t.rects()[0].size.width == 30.0f);
    }

    // ---- 6. mark_all 与超限退化 ----
    {
        DirtyRegionTracker t;
        t.mark_all();
        AURORA_TEST_CHECK(t.is_full());
        AURORA_TEST_CHECK(!t.is_empty());
        // 整帧脏后 mark 无操作
        t.mark(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 10.0f, .height = 10.0f } });
        AURORA_TEST_CHECK(t.rects().empty());

        // 超过 AURORA_MAX_RECTS 个离散矩形退化为整帧脏
        DirtyRegionTracker t2;
        for (int i = 0; i < 20; ++i) {
            t2.mark(Rect{ .origin = Point{ .x = static_cast<float>(i) * 100.0f, .y = 0.0f },
                          .size = Size{ .width = 10.0f, .height = 10.0f } });
        }
        AURORA_TEST_CHECK(t2.is_full());
    }

    // ---- 7. Window 集成：默认开启（idle 跳帧）----
    {
        Window win = make_window(320, 240);
        AURORA_TEST_CHECK(win.dirty_tracking_enabled());

        auto text = Text();
        text.content = LocalizedString{ "hello" };
        Node root{ std::move(text) };
        root.widget().mount(aurora::BuildContext{});

        // 首帧全绘；静态树第二帧跳帧（idle 零开销，仍返回 ok）
        auto r1 = win.present_root(root);
        AURORA_TEST_CHECK(r1.ok());
        auto r2 = win.present_root(root);
        AURORA_TEST_CHECK(r2.ok());
    }

    // ---- 8. Window 集成：启用后静态树第二帧跳帧 ----
    {
        Window win = make_window(320, 240);
        win.enable_dirty_tracking(true);
        AURORA_TEST_CHECK(win.dirty_tracking_enabled());

        auto text = Text();
        text.content = LocalizedString{ "static" };
        Node root{ std::move(text) };
        root.widget().mount(aurora::BuildContext{});

        // 首帧全绘
        auto r1 = win.present_root(root);
        AURORA_TEST_CHECK(r1.ok());
        // 静态树：第二帧跳帧（跳帧路径返回 true 且不 begin_frame）
        AURORA_TEST_CHECK(win.dirty_tracker().is_empty());
        auto r2 = win.present_root(root);
        AURORA_TEST_CHECK(r2.ok());
    }

    // ---- 9. Window 集成：State 变更触发重绘 ----
    {
        Window win = make_window(320, 240);
        win.enable_dirty_tracking(true);

        auto text = std::make_shared<Text>();
        text->content = LocalizedString{ "dynamic" };
        Node root{ std::shared_ptr<aurora::Widget>(text) };
        root.widget().mount(aurora::BuildContext{});

        auto r1 = win.present_root(root); // 首帧全绘 + 接线 on_dirty
        AURORA_TEST_CHECK(r1.ok());
        AURORA_TEST_CHECK(win.dirty_tracker().is_empty());

        // 控件标脏 → tracker 变脏 → 下一帧重绘
        text->mark_needs_paint();
        AURORA_TEST_CHECK(!win.dirty_tracker().is_empty());
        auto r2 = win.present_root(root);
        AURORA_TEST_CHECK(r2.ok());
        AURORA_TEST_CHECK(win.dirty_tracker().is_empty()); // 渲染后清空
    }

    // ---- 10. mark_dirty / force_full_redraw seam ----
    {
        Window win = make_window(320, 240);
        win.enable_dirty_tracking(true);

        win.mark_dirty(
            Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 10.0f, .height = 10.0f } });
        AURORA_TEST_CHECK(!win.dirty_tracker().is_empty());

        win.force_full_redraw();
        AURORA_TEST_CHECK(win.dirty_tracker().is_full());
    }
}
