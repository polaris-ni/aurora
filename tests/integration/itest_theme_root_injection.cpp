/// 测试类型: integration
/// 目标单元: include/aurora/window/window.h（prepare_context 根主题注入）+ theming/theme_scope.h
/// 测试说明: 集成——Window::present_root 自动注入根 Theme（CHG-011 接线批）。验证三级优先链：
/// 显式 set_theme > set_os_color_scheme 推入的 OS 档（经稳定 seam 模拟，不依赖真机外观）> 明色；
/// 探针 widget 经 inherit_theme(ctx) 读到注入主题，scheme 携带解析后最终档（非 Unspecified）。
/// 另覆盖 OS 档位变化的生产链路：HeadlessSurface 的 emit_os_color_scheme_change seam
/// （生产侧等价于 Win32 WM_SETTINGCHANGE → Application 再推送）经 WindowHost 转发后，
/// 默认根注入下一帧翻转，且宿主 teardown 后 seam 不再触达 Window（解绑回归）。

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "aurora/app/application.h"
#include "aurora/aurora.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/layout_builder.h"
#include "aurora/widget/node.h"
#include "aurora/widget/text.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_theme_root_injection {

namespace {

auto headless_opts(const std::string &name) -> aurora::HeadlessOptions {
    aurora::HeadlessOptions opts;
    opts.size = aurora::Size{.width = 800.0F, .height = 600.0F};
    opts.title = name;
    opts.png_path = (std::filesystem::path(aurora::testing::isolation::temp_dir()) / (name + ".png")).string();
    return opts;
}

auto make_window(const aurora::HeadlessOptions &opts) -> std::unique_ptr<aurora::Window> {
    auto created = aurora::create_window(opts);
    AURORA_TEST_REQUIRE_MSG(created.ok(), "create_window(HeadlessOptions) failed");
    return std::move(created.value());
}

/// 呈现一次并回报根 widget 经 inherit_theme 读到的主题（seen=false 表示无注入也无回落不可达，不应发生）。
auto probe_theme(aurora::Window &win) -> aurora::Theme {
    aurora::Theme out{};
    auto host = aurora::LayoutBuilder{[&](const aurora::BuildContext &c, const aurora::Constraints &) -> aurora::Node {
        out = aurora::inherit_theme(c);
        return aurora::Node{aurora::Text{"probe"}};
    }};
    aurora::Node node{std::move(host)};
    (void)win.present_root(node);
    return out;
}

auto make_scene(const std::string &label) -> aurora::Scene {
    return aurora::Scene{aurora::Node{std::make_shared<aurora::Text>(label)}};
}

auto surface_of(WindowHost *host) -> HeadlessSurface * {
    if (host == nullptr || host->window() == nullptr) {
        return nullptr;
    }
    return dynamic_cast<HeadlessSurface *>(&host->window()->surface());
}

}  // namespace

AURORA_TEST_CASE(default_injection_is_light_and_concrete_scheme) {
    auto win = make_window(headless_opts("theme_default"));
    const aurora::Theme t = probe_theme(*win);
    // 未 set_theme、未推 OS 档 → 明色回落；scheme 为具体 Light（非 Unspecified）。
    AURORA_TEST_CHECK(t.scheme == ColorScheme::Light);
    AURORA_TEST_CHECK(t.roles.surface == Color::white());
    AURORA_TEST_CHECK(t.roles.primary == Color{0x00, 0x00, 0xFF});
}

AURORA_TEST_CASE(explicit_set_theme_is_highest_priority) {
    auto win = make_window(headless_opts("theme_explicit"));
    win->set_theme(aurora::Theme::dark());
    const aurora::Theme t = probe_theme(*win);
    AURORA_TEST_CHECK(t.scheme == ColorScheme::Dark);
    AURORA_TEST_CHECK(t.roles.surface == aurora::Color{0x20, 0x21, 0x24});
}

AURORA_TEST_CASE(os_scheme_flips_default_injection) {
    // 变异自证：经稳定 seam 推入 OS 档（等价于 Win32 WM_SETTINGCHANGE 后 Application 的再推送），
    // 无显式 set_theme 时下一帧注入必须翻转档位——否则判据结构性不可观测。
    auto win = make_window(headless_opts("theme_os_dark"));
    win->set_os_color_scheme(ColorScheme::Dark);
    const aurora::Theme dark = probe_theme(*win);
    AURORA_TEST_CHECK(dark.scheme == ColorScheme::Dark);
    AURORA_TEST_CHECK(dark.roles.surface != aurora::Color::white());  // background 由浅变深

    win->set_os_color_scheme(ColorScheme::Light);
    const aurora::Theme light = probe_theme(*win);
    AURORA_TEST_CHECK(light.scheme == ColorScheme::Light);
    AURORA_TEST_CHECK(light.roles.surface == aurora::Color::white());
}

AURORA_TEST_CASE(explicit_theme_overrides_os_scheme) {
    // 三级链：显式 set_theme 高于 OS 档——即便 OS 推 Dark，显式 Light 主题原样注入。
    auto win = make_window(headless_opts("theme_explicit_vs_os"));
    win->set_os_color_scheme(ColorScheme::Dark);
    win->set_theme(aurora::Theme::light());
    const aurora::Theme t = probe_theme(*win);
    AURORA_TEST_CHECK(t.scheme == ColorScheme::Light);
    AURORA_TEST_CHECK(t.roles.surface == aurora::Color::white());
}

AURORA_TEST_CASE(os_scheme_change_via_surface_seam_flips_injection) {
    // OS 档位变化的生产链路（不含 Win32 WndProc 本体，那一段只能在真机验证）：
    // Surface seam（生产侧 = WM_SETTINGCHANGE 到达后 Application 的再推送）→ WindowHost
    // 转发 → Window::set_os_color_scheme + force_full_redraw → 下一帧根注入翻转。
    WindowOptions opts;
    opts.title = "theme_os_change";
    opts.size = Size{.width = 320.0F, .height = 240.0F};
    Application app{make_scene("os-change"), make_window(headless_opts("theme_os_change")), opts};
    WindowHost *const host = app.windows().front();
    HeadlessSurface *const surface = surface_of(host);
    AURORA_TEST_REQUIRE_NOT_NULL(surface);

    // 先固定明色基线（不依赖真机 OS 档位），再经 seam 推暗——判据才与运行环境无关。
    host->window()->set_os_color_scheme(ColorScheme::Light);
    AURORA_TEST_CHECK(probe_theme(*host->window()).scheme == ColorScheme::Light);
    // 基线自证：探针与 render_frame 后须无残脏，否则「emit 后置脏」无法区分「 seam 置脏」
    // 与「本就未清的残脏」，判据会空转（变异自证：删 on_os_color_scheme 的 force_full_redraw 必红）。
    AURORA_TEST_CHECK_FALSE(host->window()->has_pending_dirty());
    (void)host->render_frame(0.016);
    AURORA_TEST_CHECK_FALSE(host->window()->has_pending_dirty());

    surface->emit_os_color_scheme_change(ColorScheme::Dark);
    // seam 置脏（on_os_color_scheme 里 force_full_redraw 的可观测面）：翻转必须伴随整帧重排重绘，
    // 否则脏区优化会让旧明色像素留在帧缓冲里，探针读到新主题但屏幕是旧观感。
    // 须先于 probe_theme 断言——probe 的 present_root 会消费脏区。
    AURORA_TEST_CHECK_TRUE(host->window()->has_pending_dirty());
    const aurora::Theme dark = probe_theme(*host->window());
    AURORA_TEST_CHECK(dark.scheme == ColorScheme::Dark);
    AURORA_TEST_CHECK(dark.roles.surface != aurora::Color::white());

    (void)host->render_frame(0.016);  // 消费脏区，回到稳态

    surface->emit_os_color_scheme_change(ColorScheme::Light);
    const aurora::Theme light = probe_theme(*host->window());
    AURORA_TEST_CHECK(light.scheme == ColorScheme::Light);
    AURORA_TEST_CHECK(light.roles.surface == aurora::Color::white());
}

AURORA_TEST_CASE(teardown_unbinds_os_color_scheme_handler) {
    // 宿主回收路径（`Application::reap_closed`）先 teardown 再释放宿主。WM_SETTINGCHANGE
    // 在窗口销毁后仍可能到达消息泵（系统外观切换与窗口生命周期无关），回调捕获 `this`：
    // 未解绑即 use-after-free。判据先自证「接线态置脏」，再验证「解绑后无脏」，
    // 否则「解绑后无脏」无法区分「已解绑」与「从未接线」。
    WindowOptions opts;
    opts.title = "theme_os_unbind";
    opts.size = Size{.width = 320.0F, .height = 240.0F};
    Application app{make_scene("os-unbind"), make_window(headless_opts("theme_os_unbind")), opts};
    WindowHost *const host = app.windows().front();
    HeadlessSurface *const surface = surface_of(host);
    AURORA_TEST_REQUIRE_NOT_NULL(surface);

    host->window()->set_os_color_scheme(ColorScheme::Light);
    (void)host->render_frame(0.016);
    surface->emit_os_color_scheme_change(ColorScheme::Dark);
    AURORA_TEST_REQUIRE_TRUE(host->window()->has_pending_dirty());
    (void)host->render_frame(0.016);  // 消费脏区，回到无脏稳态

    host->teardown();
    surface->emit_os_color_scheme_change(ColorScheme::Light);
    AURORA_TEST_CHECK_FALSE(host->window()->has_pending_dirty());
}

}  // namespace aurora::test_cases::itest_theme_root_injection
