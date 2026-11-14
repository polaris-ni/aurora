/// 测试类型: unit
/// 目标单元: include/aurora/widget/window_resize_grip.h
/// 测试说明: 覆盖边缘带的八档判定与四档光标、带内按下发起 begin_resize、带外透传子件、
/// band=0 关闭手势、非 Normal 窗口模式不命中、以及「命中测试定档」带来的光标无粘滞

#include <memory>
#include <optional>
#include <vector>

#include "aurora/environment/build_context.h"
#include "aurora/event/dispatcher.h"
#include "aurora/widget/window_resize_grip.h"
#include "aurora/window/surface.h"
#include "aurora/window/window_chrome.h"
#include "aurora/window/window_state.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_window_resize_grip {

namespace {

/// 记录开始缩放的顺序（无头表面：只落记录，不碰平台）。
class RecordingSurface final : public HeadlessSurface {
  public:
    auto begin_window_resize(WindowResizeEdge edge) -> void override { edges.push_back(edge); }

    std::vector<WindowResizeEdge> edges;
};

/// 记录自身被按下的叶控件（验证带外按下是否透传到内容）。
class PressProbe final : public LeafWidget {
  public:
    [[nodiscard]] auto type_name() const -> const char * override { return "PressProbe"; }

    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action == MouseAction::Press) {
            ++presses;
            e.is_handled = true;
        }
    }

    int presses = 0;

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { return c.constrain(c.max); }

    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
};

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

auto full_rect(float w, float h) -> Rect {
    return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = w, .height = h}};
}

/// 造一个已布局的 grip（200×120，默认带宽 6）。`env` 非空时随布局一起交给控件。
auto make_grip(float band = AURORA_WINDOW_RESIZE_BAND, const Environment *env = nullptr)
    -> std::shared_ptr<WindowResizeGrip> {
    auto grip = std::make_shared<WindowResizeGrip>(Node{std::make_shared<PressProbe>()}, band);
    BuildContext ctx;
    ctx.env = env;
    grip->layout(bounded(200.0F, 120.0F), ctx);
    return grip;
}

auto press_at(float x, float y) -> MouseEvent {
    MouseEvent e;
    e.position = Point{.x = x, .y = y};
    e.local_position = Point{.x = x, .y = y};
    e.action = MouseAction::Press;
    e.button = MouseButton::Left;
    return e;
}

}  // namespace

AURORA_TEST_CASE(grip_reports_edge_and_cursor_per_position) {
    const auto grip = make_grip();
    const Rect box = full_rect(200.0F, 120.0F);
    BuildContext const ctx;

    struct Probe {
        float x;
        float y;
        WindowResizeEdge edge;
        CursorShape cursor;
    };
    // 带宽 6、角隅 12：四边取中线，四角取 (2,2) 这类落在 12×12 内的点。
    const Probe probes[] = {
        {.x = 100.0F, .y = 2.0F, .edge = WindowResizeEdge::Top, .cursor = CursorShape::ResizeNS},
        {.x = 100.0F, .y = 118.0F, .edge = WindowResizeEdge::Bottom, .cursor = CursorShape::ResizeNS},
        {.x = 2.0F, .y = 60.0F, .edge = WindowResizeEdge::Left, .cursor = CursorShape::ResizeEW},
        {.x = 198.0F, .y = 60.0F, .edge = WindowResizeEdge::Right, .cursor = CursorShape::ResizeEW},
        {.x = 2.0F, .y = 2.0F, .edge = WindowResizeEdge::TopLeft, .cursor = CursorShape::ResizeNWSE},
        {.x = 198.0F, .y = 2.0F, .edge = WindowResizeEdge::TopRight, .cursor = CursorShape::ResizeNESW},
        {.x = 2.0F, .y = 118.0F, .edge = WindowResizeEdge::BottomLeft, .cursor = CursorShape::ResizeNESW},
        {.x = 198.0F, .y = 118.0F, .edge = WindowResizeEdge::BottomRight, .cursor = CursorShape::ResizeNWSE},
    };
    for (const Probe &p : probes) {
        const Point at{.x = p.x, .y = p.y};
        const auto chain = grip->hit_test_chain(at, box, ctx);
        AURORA_TEST_REQUIRE_EQ(chain.size(), std::size_t{1});
        AURORA_TEST_CHECK_EQ(chain.front().ptr, static_cast<Widget *>(grip.get()));
        const auto cursor = grip->cursor_shape();
        AURORA_TEST_REQUIRE_TRUE(cursor.has_value());
        AURORA_TEST_CHECK_TRUE(*cursor == p.cursor);

        // 同一位置按下即发起对应边档的缩放（每档独立表面与宿主，记录不串档）。
        RecordingSurface surface;
        const Environment env = Environment{}.with<WindowChrome>(WindowChrome{&surface});
        const auto armed = make_grip(AURORA_WINDOW_RESIZE_BAND, &env);
        MouseEvent e = press_at(p.x, p.y);
        armed->on_pointer_event(e);
        AURORA_TEST_CHECK_TRUE(e.is_handled);
        AURORA_TEST_REQUIRE_EQ(surface.edges.size(), std::size_t{1});
        AURORA_TEST_CHECK_TRUE(surface.edges.front() == p.edge);
    }
}

AURORA_TEST_CASE(grip_center_falls_through_to_content) {
    const auto grip = make_grip();
    BuildContext const ctx;
    const auto chain = grip->hit_test_chain(Point{.x = 100.0F, .y = 60.0F}, full_rect(200.0F, 120.0F), ctx);

    AURORA_TEST_REQUIRE_EQ(chain.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(chain.front().ptr, static_cast<Widget *>(grip.get()));
    AURORA_TEST_CHECK_STREQ(chain.back().ptr->type_name(), "PressProbe");
    AURORA_TEST_CHECK_FALSE(grip->cursor_shape().has_value());

    // 中心按下不发起缩放：命中链把事件交给内容，内容消费之。
    RecordingSurface surface;
    const Environment env = Environment{}.with<WindowChrome>(WindowChrome{&surface});
    BuildContext live;
    live.env = &env;
    grip->layout(bounded(200.0F, 120.0F), live);
    MouseEvent e = press_at(100.0F, 60.0F);
    grip->on_pointer_event(e);
    AURORA_TEST_CHECK_TRUE(surface.edges.empty());
}

AURORA_TEST_CASE(grip_zero_band_disables_the_gesture) {
    const auto grip = make_grip(0.0F);
    BuildContext const ctx;
    for (const Point at : {Point{.x = 2.0F, .y = 2.0F}, Point{.x = 2.0F, .y = 60.0F}, Point{.x = 100.0F, .y = 2.0F}}) {
        const auto chain = grip->hit_test_chain(at, full_rect(200.0F, 120.0F), ctx);
        AURORA_TEST_REQUIRE_EQ(chain.size(), std::size_t{2});
        AURORA_TEST_CHECK_STREQ(chain.back().ptr->type_name(), "PressProbe");
        AURORA_TEST_CHECK_FALSE(grip->cursor_shape().has_value());
    }

    RecordingSurface surface;
    const Environment env = Environment{}.with<WindowChrome>(WindowChrome{&surface});
    BuildContext live;
    live.env = &env;
    grip->layout(bounded(200.0F, 120.0F), live);
    MouseEvent e = press_at(2.0F, 60.0F);
    grip->on_pointer_event(e);
    AURORA_TEST_CHECK_TRUE(surface.edges.empty());
}

AURORA_TEST_CASE(grip_ignores_edges_when_window_not_normal) {
    auto grip = make_grip();
    RecordingSurface surface;
    const Environment maximized =
        Environment{}.with<WindowMode>(WindowMode::Maximized).with<WindowChrome>(WindowChrome{&surface});
    BuildContext ctx;
    ctx.env = &maximized;
    grip->layout(bounded(200.0F, 120.0F), ctx);

    // 最大化下边缘带不命中：内容拿到事件、光标不给缩放形状。
    const auto chain = grip->hit_test_chain(Point{.x = 2.0F, .y = 60.0F}, full_rect(200.0F, 120.0F), ctx);
    AURORA_TEST_REQUIRE_EQ(chain.size(), std::size_t{2});
    AURORA_TEST_CHECK_STREQ(chain.back().ptr->type_name(), "PressProbe");
    AURORA_TEST_CHECK_FALSE(grip->cursor_shape().has_value());

    MouseEvent e = press_at(2.0F, 60.0F);
    grip->on_pointer_event(e);
    AURORA_TEST_CHECK_TRUE(surface.edges.empty());

    // 切回 Normal 后同一坐标恢复命中——判据读的是环境值而非一次性缓存。
    const Environment normal =
        Environment{}.with<WindowMode>(WindowMode::Normal).with<WindowChrome>(WindowChrome{&surface});
    ctx.env = &normal;
    grip->layout(bounded(200.0F, 120.0F), ctx);
    const auto restored = grip->hit_test_chain(Point{.x = 2.0F, .y = 60.0F}, full_rect(200.0F, 120.0F), ctx);
    AURORA_TEST_REQUIRE_EQ(restored.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(restored.front().ptr, static_cast<Widget *>(grip.get()));
}

AURORA_TEST_CASE(grip_cursor_follows_edge_without_sticking) {
    const auto grip = make_grip();
    EventDispatcher dispatcher;
    std::vector<CursorShape> emitted;
    dispatcher.set_cursor_handler([&emitted](CursorShape shape) { emitted.push_back(shape); });

    const auto move_to = [&](float x, float y) -> void {
        MouseEvent m;
        m.position = Point{.x = x, .y = y};
        m.action = MouseAction::Move;
        dispatcher.dispatch_mouse(*grip, m);
    };

    move_to(2.0F, 60.0F);  // 左边缘
    AURORA_TEST_REQUIRE_TRUE(!emitted.empty());
    AURORA_TEST_CHECK_TRUE(emitted.back() == CursorShape::ResizeEW);

    move_to(100.0F, 60.0F);  // 移入内容：必须回到默认箭头，不得粘住缩放光标
    AURORA_TEST_CHECK_TRUE(emitted.back() == CursorShape::Arrow);

    move_to(198.0F, 60.0F);  // 右边缘
    AURORA_TEST_CHECK_TRUE(emitted.back() == CursorShape::ResizeEW);

    move_to(198.0F, 2.0F);  // 右上角：同一条命中链内换档，光标须立即跟随
    AURORA_TEST_CHECK_TRUE(emitted.back() == CursorShape::ResizeNESW);
}

AURORA_TEST_CASE(grip_large_band_is_clamped_to_a_quarter_of_the_short_side) {
    // 窄窗防护：band 超过最短边四分之一时按四分之一钳制（40 高的窗 ⇒ 带宽 10）。
    // 不钳制则 band = 100 > 窗高，整窗都成了边缘带、内容再也点不到。
    auto grip = std::make_shared<WindowResizeGrip>(Node{std::make_shared<PressProbe>()}, 100.0F);
    BuildContext ctx;
    grip->layout(bounded(200.0F, 40.0F), ctx);
    const Rect box = full_rect(200.0F, 40.0F);

    // 中心与「离上边 12dp」都在钳制后的带外：若未钳制，这两点都会被判成边缘。
    for (const Point at : {Point{.x = 100.0F, .y = 20.0F}, Point{.x = 100.0F, .y = 12.0F}}) {
        const auto chain = grip->hit_test_chain(at, box, ctx);
        AURORA_TEST_REQUIRE_EQ(chain.size(), std::size_t{2});
        AURORA_TEST_CHECK_STREQ(chain.back().ptr->type_name(), "PressProbe");
    }

    // 离上边 5dp 仍在带内（5 < 10）：边缘仍可命中。
    const auto edge = grip->hit_test_chain(Point{.x = 100.0F, .y = 5.0F}, box, ctx);
    AURORA_TEST_REQUIRE_EQ(edge.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(edge.front().ptr, static_cast<Widget *>(grip.get()));
}

AURORA_TEST_CASE(grip_descriptor_declares_band_and_single_child) {
    const WindowResizeGrip grip{Node{std::make_shared<PressProbe>()}};
    const WidgetDescriptor desc = grip.describe();
    AURORA_TEST_CHECK_STREQ(desc.name, "WindowResizeGrip");
    AURORA_TEST_CHECK_STREQ(desc.children_policy, "single");
    AURORA_TEST_REQUIRE_EQ(desc.properties.size(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(desc.properties.front().name, "band");
    AURORA_TEST_CHECK_STREQ(desc.properties.front().default_value, "6.0");
    AURORA_TEST_CHECK_NEAR(grip.band(), AURORA_WINDOW_RESIZE_BAND, 1e-6F);
}

}  // namespace aurora::test_cases::utest_window_resize_grip
