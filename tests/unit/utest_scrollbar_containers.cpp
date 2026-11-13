/// 测试类型: unit
/// 目标单元: include/aurora/widget/scroll.h / lazy_list.h / grid_view.h / lazy_row.h
/// 测试说明: 覆盖四容器滚动条接线——经典档溢出时预留带宽并绘制轨道/滑块、不溢出零观感变化、
///           浮层档不收缩且无悬停不绘制、横向列表从既有固有高扣除带宽而不自增高度、
///           拖拽映射与指针捕获、轨道翻页一次、浮层显隐只按注入时刻定夺、预设档粗细与模式生效。
///           判据以像素 + 偏移值的成对观测给出，时间一律注入，不依赖墙钟与帧计数。

#include <chrono>
#include <cmath>
#include <memory>
#include <utility>

#include "aurora/environment/build_context.h"
#include "aurora/environment/environment.h"
#include "aurora/event/dispatcher.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/grid_view.h"
#include "aurora/widget/lazy_list.h"
#include "aurora/widget/lazy_row.h"
#include "aurora/widget/scroll.h"
#include "aurora/widget/widget.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_scrollbar_containers {

namespace {

using aurora::BuildContext;
using aurora::Color;
using aurora::ColorScheme;
using aurora::Constraints;
using aurora::Environment;
using aurora::EventDispatcher;
using aurora::GridView;
using aurora::LazyList;
using aurora::LazyRow;
using aurora::MouseAction;
using aurora::MouseButton;
using aurora::MouseEvent;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::Scroll;
using aurora::ScrollbarMode;
using aurora::Size;
using aurora::Theme;
using aurora::ThemeStyle;
using aurora::Widget;

using Clock = std::chrono::steady_clock;

constexpr float AURORA_W = 200.0F;
constexpr float AURORA_H = 200.0F;
constexpr float AURORA_INSET = 2.0F;
constexpr float AURORA_THICKNESS = 4.0F;
constexpr float AURORA_BAND = AURORA_THICKNESS + (2.0F * AURORA_INSET);
constexpr float AURORA_TRACK_X = AURORA_W - AURORA_INSET - (AURORA_THICKNESS * 0.5F);
const Color AURORA_CONTENT{255, 0, 0, 255};  ///< 内容区专用识别色：与轨道灰、画布白都不混淆。

/// @brief 固定尺寸哑控件：绘制时把自身盒整个涂成识别色，供像素判据读出内容边界。
class FillBox final : public Widget {
  public:
    FillBox(float w, float h) : w_(w), h_(h) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "FillBox"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = w_, .height = h_});
    }
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        p.fill_rect(bounds, AURORA_CONTENT);
    }

  private:
    float w_;
    float h_;
};

auto fill_node(float w, float h) -> Node { return Node{std::make_shared<FillBox>(w, h)}; }

/// @brief 造一个注入指定主题的构建上下文（Environment 须活到绘制结束）。
auto themed_ctx(const Theme &t) -> std::pair<std::shared_ptr<Environment>, BuildContext> {
    auto env = std::make_shared<Environment>();
    env->set_local(t);
    BuildContext ctx;
    ctx.env = env.get();
    return {std::move(env), ctx};
}

template <typename W>
auto layout_root(W &root, const BuildContext &ctx) -> void {
    root.layout(Constraints{.min = Size{}, .max = Size{.width = AURORA_W, .height = AURORA_H}}, ctx);
}

/// @brief 铺白底后把控件画满整幅画布（原点为零，条几何的局部坐标即画布坐标）。
template <typename W>
auto paint_root(W &root, Painter &p, const BuildContext &ctx) -> void {
    p.begin(static_cast<int>(AURORA_W), static_cast<int>(AURORA_H));
    p.fill_rect(Rect{.origin = Point{}, .size = Size{.width = AURORA_W, .height = AURORA_H}}, Color::white());
    root.paint(p, Rect{.origin = Point{}, .size = Size{.width = AURORA_W, .height = AURORA_H}}, ctx);
}

auto is_content(Color c) -> bool { return c.r == 255 && c.g == 0 && c.b == 0 && c.a == 255; }

/// @brief 条带像素：中性灰（轨道半透明叠白或滑块实色），既非识别红也非画布白。
auto is_bar_pixel(Color c) -> bool {
    const int r = c.r;
    const int g = c.g;
    const int b = c.b;
    return !is_content(c) && r < 250 && std::abs(r - g) <= 3 && std::abs(g - b) <= 3;
}

/// @brief 从右向左找第一个识别色像素，返回内容区右边界（dp）。
auto content_right_edge(const Painter &p, int y) -> int {
    for (int x = static_cast<int>(AURORA_W) - 1; x >= 0; --x) {
        if (is_content(p.get_pixel(x, y))) {
            return x + 1;
        }
    }
    return 0;
}

/// @brief 从下向上找第一个识别色像素，返回内容区下边界（dp）。
auto content_bottom_edge(const Painter &p, int x) -> int {
    for (int y = static_cast<int>(AURORA_H) - 1; y >= 0; --y) {
        if (is_content(p.get_pixel(x, y))) {
            return y + 1;
        }
    }
    return 0;
}

auto classic_theme() -> Theme { return Theme::light(); }

auto overlay_theme() -> Theme {
    Theme t = Theme::light();
    t.conventions.scrollbar_mode = ScrollbarMode::Overlay;
    return t;
}

auto build_scroll(float content_h, const BuildContext &ctx) -> std::shared_ptr<Scroll> {
    auto sc = std::make_shared<Scroll>(fill_node(AURORA_W, content_h));
    layout_root(*sc, ctx);
    return sc;
}

auto mouse(MouseAction action, float x, float y) -> MouseEvent {
    MouseEvent e;
    e.action = action;
    e.button = MouseButton::Left;
    e.position = Point{.x = x, .y = y};
    return e;
}

}  // namespace

AURORA_TEST_CASE(classic_scroll_reserves_right_band_and_draws_track) {
    auto [env, ctx] = themed_ctx(classic_theme());
    auto sc = build_scroll(600.0F, ctx);  // 视口 200，内容 600 ⇒ 溢出 400
    Painter p;
    paint_root(*sc, p, ctx);
    AURORA_TEST_CHECK_NEAR(static_cast<float>(content_right_edge(p, 100)), AURORA_W - AURORA_BAND, 2.0F);
    AURORA_TEST_CHECK_TRUE(is_bar_pixel(p.get_pixel(static_cast<int>(AURORA_TRACK_X), 100)));
}

AURORA_TEST_CASE(overlay_scroll_keeps_full_width_and_hides_bar_without_hover) {
    auto [env, ctx] = themed_ctx(overlay_theme());
    auto sc = build_scroll(600.0F, ctx);
    Painter p;
    paint_root(*sc, p, ctx);
    AURORA_TEST_CHECK_NEAR(static_cast<float>(content_right_edge(p, 100)), AURORA_W, 2.0F);
    // 无悬停 / 无滚动：浮层恒不绘制，条带位置就是内容本身。
    AURORA_TEST_CHECK_TRUE(is_content(p.get_pixel(static_cast<int>(AURORA_TRACK_X), 100)));
}

AURORA_TEST_CASE(no_overflow_has_no_bar_and_no_reservation) {
    auto [env, ctx] = themed_ctx(classic_theme());
    auto sc = build_scroll(100.0F, ctx);
    Painter p;
    paint_root(*sc, p, ctx);
    AURORA_TEST_CHECK_NEAR(static_cast<float>(content_right_edge(p, 50)), AURORA_W, 2.0F);
    AURORA_TEST_CHECK_TRUE(is_content(p.get_pixel(static_cast<int>(AURORA_TRACK_X), 50)));
}

AURORA_TEST_CASE(classic_lazy_list_and_grid_view_shrink_content_column) {
    auto [env, ctx] = themed_ctx(classic_theme());
    auto list = std::make_shared<LazyList>(20, [](int) -> Node { return fill_node(AURORA_W, 48.0F); }, 48.0F);
    layout_root(*list, ctx);
    Painter lp;
    paint_root(*list, lp, ctx);
    AURORA_TEST_CHECK_NEAR(static_cast<float>(content_right_edge(lp, 10)), AURORA_W - AURORA_BAND, 2.0F);
    AURORA_TEST_CHECK_TRUE(is_bar_pixel(lp.get_pixel(static_cast<int>(AURORA_TRACK_X), 100)));

    auto grid = std::make_shared<GridView>(20, 2, [](int) -> Node { return fill_node(AURORA_W, 96.0F); }, 96.0F);
    layout_root(*grid, ctx);
    Painter gp;
    paint_root(*grid, gp, ctx);
    AURORA_TEST_CHECK_NEAR(static_cast<float>(content_right_edge(gp, 10)), AURORA_W - AURORA_BAND, 2.0F);
    AURORA_TEST_CHECK_TRUE(is_bar_pixel(gp.get_pixel(static_cast<int>(AURORA_TRACK_X), 100)));
}

AURORA_TEST_CASE(classic_lazy_row_reserves_bottom_band_without_growing_intrinsic_height) {
    auto [env, ctx] = themed_ctx(classic_theme());
    auto row = std::make_shared<LazyRow>(20, [](int) -> Node { return fill_node(40.0F, AURORA_H); }, 40.0F);
    layout_root(*row, ctx);
    // 既有固有高 = item_extent(40) + 上下 padding(0)：不因条带向父级多要高度。
    AURORA_TEST_CHECK_NEAR(row->size().height, 40.0F, 1e-4F);
    Painter p;
    paint_root(*row, p, ctx);
    AURORA_TEST_CHECK_NEAR(static_cast<float>(content_bottom_edge(p, 20)), AURORA_H - AURORA_BAND, 2.0F);
    AURORA_TEST_CHECK_TRUE(is_bar_pixel(p.get_pixel(20, static_cast<int>(AURORA_H - AURORA_INSET - 2.0F))));
}

AURORA_TEST_CASE(drag_maps_pointer_delta_to_offset_and_capture_survives_leaving_bounds) {
    auto [env, ctx] = themed_ctx(classic_theme());
    auto sc = build_scroll(600.0F, ctx);
    // 轨道长 196、滑块长 196×200/600≈65.33、可滚上限 400 ⇒ 24dp 位移对应约 73.47dp 偏移。
    constexpr float track_length = AURORA_H - (2.0F * AURORA_INSET);
    constexpr float thumb_length = track_length * AURORA_H / 600.0F;
    const float expected = 24.0F * 400.0F / (track_length - thumb_length);

    MouseEvent press = mouse(MouseAction::Press, AURORA_TRACK_X, 20.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*sc, press, nullptr));
    AURORA_TEST_CHECK_TRUE(press.is_handled);
    MouseEvent move = mouse(MouseAction::Move, AURORA_W + 50.0F, 44.0F);  // 移出控件仍继续
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*sc, move, nullptr));
    AURORA_TEST_CHECK_NEAR(sc->offset_y(), expected, 1.0F);
    MouseEvent release = mouse(MouseAction::Release, AURORA_W + 50.0F, 44.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*sc, release, nullptr));
    MouseEvent after = mouse(MouseAction::Move, AURORA_W + 50.0F, 80.0F);
    // 抬起即解除指针捕获：落在控件外的后续移动不再命中（不吞事件），偏移也不再跟随。
    AURORA_TEST_CHECK_FALSE(EventDispatcher::dispatch(*sc, after, nullptr));
    AURORA_TEST_CHECK_NEAR(sc->offset_y(), expected, 1.0F);  // 抬起后不再跟随
}

AURORA_TEST_CASE(track_press_pages_once_and_holding_does_not_repeat) {
    auto [env, ctx] = themed_ctx(classic_theme());
    auto sc = build_scroll(600.0F, ctx);
    MouseEvent press = mouse(MouseAction::Press, AURORA_TRACK_X, 150.0F);  // 滑块之后 ⇒ 向下一页
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*sc, press, nullptr));
    AURORA_TEST_CHECK_NEAR(sc->offset_y(), AURORA_H, 1e-4F);
    MouseEvent held = mouse(MouseAction::Move, AURORA_TRACK_X, 160.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*sc, held, nullptr));
    AURORA_TEST_CHECK_NEAR(sc->offset_y(), AURORA_H, 1e-4F);  // 按住不重复翻页
    MouseEvent release = mouse(MouseAction::Release, AURORA_TRACK_X, 160.0F);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*sc, release, nullptr));
}

AURORA_TEST_CASE(overlay_visibility_follows_injected_time_and_releases_frame_tick) {
    auto [env, ctx] = themed_ctx(overlay_theme());
    auto sc = build_scroll(600.0F, ctx);
    const auto start = Clock::time_point{};

    sc->on_hover_change(true);
    sc->tick(start);
    Painter entered;
    paint_root(*sc, entered, ctx);
    // 浮层轨道是半透明黑（alpha=20）叠在红内容上：像素不再是纯内容红即证明条已绘。
    AURORA_TEST_CHECK_TRUE(!is_content(entered.get_pixel(static_cast<int>(AURORA_TRACK_X), 100)));

    sc->on_hover_change(false);
    sc->tick(start + std::chrono::seconds{2});
    Painter left;
    paint_root(*sc, left, ctx);
    AURORA_TEST_CHECK_TRUE(is_content(left.get_pixel(static_cast<int>(AURORA_TRACK_X), 100)));

    // 滚轮后保持 1 秒：到期前仍显、到期后隐藏，全部由注入 now 定夺。
    (void)sc->scroll_by(-1.0F);
    sc->tick(start + std::chrono::seconds{2});
    Painter held;
    paint_root(*sc, held, ctx);
    AURORA_TEST_CHECK_TRUE(!is_content(held.get_pixel(static_cast<int>(AURORA_TRACK_X), 100)));
    sc->tick(start + std::chrono::seconds{2} + std::chrono::milliseconds{999});
    Painter before_expiry;
    paint_root(*sc, before_expiry, ctx);
    AURORA_TEST_CHECK_TRUE(!is_content(before_expiry.get_pixel(static_cast<int>(AURORA_TRACK_X), 100)));
    sc->tick(start + std::chrono::seconds{2} + std::chrono::milliseconds{1001});
    Painter expired;
    paint_root(*sc, expired, ctx);
    AURORA_TEST_CHECK_TRUE(is_content(expired.get_pixel(static_cast<int>(AURORA_TRACK_X), 100)));
}

AURORA_TEST_CASE(gnome_preset_uses_overlay_with_default_thickness) {
    const Theme gnome = Theme::from_style(ThemeStyle::Gnome, ColorScheme::Light);
    // GNOME 只把占位模式覆写为 Overlay，粗细继承默认 4dp（8dp 是 KDE 的 ScrollBar_SliderWidth，见下一例）。
    AURORA_TEST_CHECK_NEAR(gnome.controls.scrollbar_thickness, 4.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(gnome.conventions.scrollbar_mode == ScrollbarMode::Overlay);
    auto [env, ctx] = themed_ctx(gnome);
    auto sc = build_scroll(600.0F, ctx);
    Painter hidden;
    paint_root(*sc, hidden, ctx);
    // 浮层不收缩内容盒：满宽，未悬停时条带位置仍是内容识别色。
    AURORA_TEST_CHECK_NEAR(static_cast<float>(content_right_edge(hidden, 100)), AURORA_W, 2.0F);
    AURORA_TEST_CHECK_TRUE(is_content(hidden.get_pixel(static_cast<int>(AURORA_TRACK_X), 100)));

    sc->on_hover_change(true);
    sc->tick(Clock::time_point{});
    Painter shown;
    paint_root(*sc, shown, ctx);
    // 半透明轨道叠在红内容上：像素不再是纯内容红即证明条已绘（条中心 x = 200−2−2 = 196）。
    AURORA_TEST_CHECK_TRUE(!is_content(shown.get_pixel(static_cast<int>(AURORA_TRACK_X), 100)));
    // 条带盒外仍是内容识别色：4dp 宽条只占 x∈[194,198]，185 不在其中。
    AURORA_TEST_CHECK_TRUE(is_content(shown.get_pixel(185, 100)));
    sc->on_hover_change(false);
    sc->tick(Clock::time_point{} + std::chrono::seconds{2});
}

AURORA_TEST_CASE(kde_preset_widens_band_to_eight_and_uses_classic) {
    const Theme kde = Theme::from_style(ThemeStyle::Kde, ColorScheme::Light);
    // KDE 覆写粗细为 8dp（ScrollBar_SliderWidth）并保持经典占位模式。
    AURORA_TEST_CHECK_NEAR(kde.controls.scrollbar_thickness, 8.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(kde.conventions.scrollbar_mode == ScrollbarMode::Classic);
    auto [env, ctx] = themed_ctx(kde);
    auto sc = build_scroll(600.0F, ctx);
    Painter p;
    paint_root(*sc, p, ctx);
    // 经典档带宽 = 粗细 8 + 2×内缩 2 = 12 ⇒ 内容宽 200−12 = 188；轨道中心 x = 200−2−4 = 194。
    AURORA_TEST_CHECK_NEAR(static_cast<float>(content_right_edge(p, 100)), AURORA_W - 12.0F, 2.0F);
    AURORA_TEST_CHECK_TRUE(is_bar_pixel(p.get_pixel(194, 100)));
}

}  // namespace aurora::test_cases::utest_scrollbar_containers
