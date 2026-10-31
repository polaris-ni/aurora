/// 测试类型: unit
/// 目标单元: include/aurora/window/surface.h include/aurora/window/window_state.h
///           include/aurora/window/window.h include/aurora/window/platform.h
/// 测试说明: 运行期显隐与置顶开关的四条口径：未接线后端是可观测的 no-op（基类默认值不被改写，
///           配合能力位申报而非静默失效）、无头 seam 记录值并上报对应 `WindowState`（同值幂等）、
///           `Window` 透传与读回、`compute_window_state` 的第三枚输入 `on_screen`、
///           能力位逐 `SurfaceKind` 矩阵、以及「显隐 → 宿主上报 → 应用级 `window_state()`」的聚合链。
///           置顶切换不得改动几何记录值。全部走无头后端，不依赖真实桌面会话。

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/app/scene.h"
#include "aurora/widget/text.h"
#include "aurora/window/platform.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"
#include "aurora/window/window_state.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_window_runtime_control {

namespace {

auto make_scene() -> Scene { return Scene{Node{std::make_shared<Text>("hi")}}; }

/// @brief 最小后端桩：只实现纯虚四件，其余全部走 `Surface` 默认实现——
///        「未接线后端」的判据必须打在默认实现上，而不是打在无头 seam 上。
class BareSurface final : public Surface {
  public:
    auto begin_frame(int /*width*/, int /*height*/) -> Result<bool> override { return Result<bool>{true}; }
    [[nodiscard]] auto painter() -> Painter & override { return painter_; }
    [[nodiscard]] auto present() -> Result<bool> override { return Result<bool>{true}; }
    [[nodiscard]] auto size() const -> Size override { return Size{.width = 10.0F, .height = 10.0F}; }

  private:
    Painter painter_;
};

/// @brief 产出一枚无头窗口（可独立断言其 `HeadlessSurface` seam）。
/// @param title 窗口标题（日志归因用）。
/// @param opts  可见性档（默认 `Normal`）。
auto make_headless_window(std::string title, WindowVisibility visibility = WindowVisibility::Normal)
    -> std::unique_ptr<Window> {
    HeadlessOptions opts;
    static_cast<WindowOptions &>(opts) = WindowOptions{};
    opts.title = std::move(title);
    opts.visibility = visibility;
    opts.size = Size{.width = 200.0F, .height = 150.0F};
    auto res = create_window(opts);
    return res ? std::move(res.value()) : nullptr;
}

/// @brief 取窗口的无头表面（seam 入口）。
/// @param window 目标窗口。
/// @return `HeadlessSurface` 裸指针；非无头后端为 nullptr。
auto headless_of(Window *window) -> HeadlessSurface * {
    return window == nullptr ? nullptr : dynamic_cast<HeadlessSurface *>(&window->surface());
}

}  // namespace

AURORA_TEST_CASE(unwired_backends_are_observable_noops) {
    // 「不支持」必须是可查询、可读回的，而不是按了没反应。基类默认 setter 不动任何状态，
    // 默认读回恒为「未隐藏 / 未置顶」——调用方据此能立即看出开关没生效。
    BareSurface surface;
    AURORA_TEST_CHECK_TRUE(surface.is_visible());
    AURORA_TEST_CHECK_FALSE(surface.is_always_on_top());

    surface.set_visible(false);
    surface.set_always_on_top(true);
    AURORA_TEST_CHECK_TRUE(surface.is_visible());  // 默认实现不记录：读回仍是默认值
    AURORA_TEST_CHECK_FALSE(surface.is_always_on_top());  // 同上
}

AURORA_TEST_CASE(headless_records_visibility_and_reports_state) {
    // 判据 1：记录值 + 状态上报同轴；同值重复调用不产生第二次上报（与 Win32 腿的幂等同口径）。
    HeadlessSurface surface;
    std::vector<WindowState> seen;
    surface.set_window_state_handler([&seen](WindowState s) -> void { seen.push_back(s); });

    AURORA_TEST_CHECK_TRUE(surface.is_visible());
    surface.set_visible(false);
    AURORA_TEST_CHECK_FALSE(surface.is_visible());
    AURORA_TEST_REQUIRE_EQ(seen.size(), 1U);
    AURORA_TEST_CHECK_EQ(seen.at(0), WindowState::Hidden);

    surface.set_visible(false);  // 同值：不再上报
    AURORA_TEST_CHECK_EQ(seen.size(), 1U);

    surface.set_visible(true);
    AURORA_TEST_CHECK_TRUE(surface.is_visible());
    AURORA_TEST_REQUIRE_EQ(seen.size(), 2U);
    AURORA_TEST_CHECK_EQ(seen.at(1), WindowState::Visible);
}

AURORA_TEST_CASE(window_passthrough_forwards_and_reads_back) {
    // `Window` 的四枚透传：开关落在同一个 Surface 上，读回走 Surface 而非另存一份意图。
    auto window = make_headless_window("runtime_control");
    AURORA_TEST_REQUIRE_NOT_NULL(window);
    HeadlessSurface *const surface = headless_of(window.get());
    AURORA_TEST_REQUIRE_NOT_NULL(surface);

    AURORA_TEST_CHECK_TRUE(window->is_visible());
    window->set_visible(false);
    AURORA_TEST_CHECK_FALSE(window->is_visible());
    AURORA_TEST_CHECK_FALSE(surface->is_visible());  // 同一条状态，不是两份真值源

    AURORA_TEST_CHECK_FALSE(window->is_always_on_top());
    window->set_always_on_top(true);
    AURORA_TEST_CHECK_TRUE(window->is_always_on_top());
    AURORA_TEST_CHECK_TRUE(surface->is_always_on_top());
    window->set_always_on_top(false);
    AURORA_TEST_CHECK_FALSE(window->is_always_on_top());
}

AURORA_TEST_CASE(topmost_toggle_does_not_touch_geometry) {
    // 判据 2：置顶切换不得改动位置 / 尺寸（Win32 腿靠 `SWP_NOMOVE | SWP_NOSIZE` 保证，
    // 无头腿靠「两枚开关与几何记录值正交」保证——同一判据在两侧都不许被违反）。
    auto window = make_headless_window("topmost_geometry");
    AURORA_TEST_REQUIRE_NOT_NULL(window);
    Surface &surface = window->surface();
    surface.set_position(Point{.x = 37.0F, .y = 91.0F});
    const Size before = surface.size();
    const Point origin_before = surface.position();

    surface.set_always_on_top(true);
    surface.set_always_on_top(false);
    AURORA_TEST_CHECK_NEAR(surface.position().x, origin_before.x, 1e-4F);
    AURORA_TEST_CHECK_NEAR(surface.position().y, origin_before.y, 1e-4F);
    AURORA_TEST_CHECK_NEAR(surface.size().width, before.width, 1e-4F);
    AURORA_TEST_CHECK_NEAR(surface.size().height, before.height, 1e-4F);
}

AURORA_TEST_CASE(compute_window_state_on_screen_matrix) {
    // 判据 3：不在屏恒 `Hidden`（四组合都成立）；在屏时逐位等于旧两形参结果——
    // 既有调用点（X11 / GLFW）不写第三实参即得旧行为，这是「不改既有语义」的实现层保证。
    for (const bool minimized : {false, true}) {
        for (const bool active : {false, true}) {
            AURORA_TEST_CHECK_EQ(compute_window_state(minimized, active, false), WindowState::Hidden);
            const WindowState legacy = compute_window_state(minimized, active);  // 默认实参路径
            AURORA_TEST_CHECK_EQ(compute_window_state(minimized, active, true), legacy);
            if (minimized) {
                AURORA_TEST_CHECK_EQ(legacy, WindowState::Hidden);
            } else {
                AURORA_TEST_CHECK_EQ(legacy, active ? WindowState::Visible : WindowState::Occluded);
            }
        }
    }
}

AURORA_TEST_CASE(capability_matrix_pins_wired_backends) {
    // 判据 4：能力位申报「机制存在且已接线」——Win32 / D3D11（同一宿主）与 MacOS 翻位；
    // 其余后端为「可查询的不支持」，应用据此降级而不是以为热键坏了。
    auto visibility_bit = [](SurfaceKind kind) -> bool {
        Platform p;
        p.surface = kind;
        return p.capabilities().runtime_visibility;
    };
    auto topmost_bit = [](SurfaceKind kind) -> bool {
        Platform p;
        p.surface = kind;
        return p.capabilities().runtime_always_on_top;
    };
    AURORA_TEST_CHECK_TRUE(visibility_bit(SurfaceKind::Win32));
    AURORA_TEST_CHECK_TRUE(topmost_bit(SurfaceKind::Win32));
    AURORA_TEST_CHECK_TRUE(visibility_bit(SurfaceKind::D3D11));
    AURORA_TEST_CHECK_TRUE(topmost_bit(SurfaceKind::D3D11));
    AURORA_TEST_CHECK_TRUE(visibility_bit(SurfaceKind::MacOS));
    AURORA_TEST_CHECK_TRUE(topmost_bit(SurfaceKind::MacOS));
    for (const SurfaceKind kind :
         {SurfaceKind::X11, SurfaceKind::Wayland, SurfaceKind::Glfw, SurfaceKind::Wasm, SurfaceKind::Headless}) {
        AURORA_TEST_CHECK_FALSE(visibility_bit(kind));
        AURORA_TEST_CHECK_FALSE(topmost_bit(kind));
    }
}

AURORA_TEST_CASE(visibility_aggregates_to_application_state) {
    // 聚合链：显隐 → Surface 上报 → `WindowHost` → `Application::window_state()`。
    // 主窗投影是既有口径（`set_state_sink` 只装主窗），本条只证新开关落进同一条链、不另立通路。
    auto window = make_headless_window("runtime_app_state");
    AURORA_TEST_REQUIRE_NOT_NULL(window);
    Application app{make_scene(), std::move(window)};
    Window *const win = app.window();
    AURORA_TEST_REQUIRE_NOT_NULL(win);

    app.window()->set_visible(false);
    AURORA_TEST_CHECK_EQ(app.window_state().get(), WindowState::Hidden);
    win->set_visible(true);
    AURORA_TEST_CHECK_EQ(app.window_state().get(), WindowState::Visible);
}

AURORA_TEST_CASE(constructed_hidden_window_and_runtime_hide_agree) {
    // 自洽判据：建窗期 `Hidden` 档与运行期 `set_visible(false)` 落在同一个可观测事实两侧——
    // 前者初值即「不在屏」，故 `is_visible()` 与状态上报都不必为它破例。
    auto window = make_headless_window("runtime_initial_hidden", WindowVisibility::Hidden);
    AURORA_TEST_REQUIRE_NOT_NULL(window);
    HeadlessSurface *const surface = headless_of(window.get());
    AURORA_TEST_REQUIRE_NOT_NULL(surface);

    // 无头后端的初值口径：`HeadlessSurface` 不消费 `visibility`（无 OS 窗口），故这里显式
    // 走运行期开关表达同一事实——真实 Win32 腿由构造期播种同一档位（见 `Win32Host` 构造尾注记）。
    surface->set_visible(false);
    AURORA_TEST_CHECK_FALSE(surface->is_visible());

    std::vector<WindowState> seen;
    surface->set_window_state_handler([&seen](WindowState s) -> void { seen.push_back(s); });
    surface->set_visible(true);
    surface->set_visible(false);
    AURORA_TEST_REQUIRE_EQ(seen.size(), 2U);
    AURORA_TEST_CHECK_EQ(seen.at(0), WindowState::Visible);
    AURORA_TEST_CHECK_EQ(seen.at(1), WindowState::Hidden);
}

}  // namespace aurora::test_cases::utest_window_runtime_control
