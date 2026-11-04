/// 测试类型: integration
/// 目标单元: include/aurora/window/window.h（prepare_context 根主题注入）+ theming/theme_scope.h
/// 测试说明: 集成——Window::present_root 自动注入根 Theme（接线批）。验证三级优先链：
/// 显式 set_theme > set_os_color_scheme 推入的 OS 档（经稳定 seam 模拟，不依赖真机外观）> 明色；
/// 探针 widget 经 inherit_theme(ctx) 读到注入主题，scheme 携带解析后最终档（非 Unspecified）。
/// 另覆盖 OS 档位变化的生产链路：HeadlessSurface 的 emit_os_color_scheme_change seam
/// （生产侧等价于 Win32 WM_SETTINGCHANGE → Application 再推送）经 WindowHost 转发后，
/// 默认根注入下一帧翻转，且宿主 teardown 后 seam 不再触达 Window（解绑回归）。
///
/// 还覆盖**常驻树**上的换档落地，分两格各钉一半：换档帧连带整子树缓存失效（重排重录读到新档，
/// 关掉脏追踪才观察得到）、换档同时整窗标脏（增量脏区推不出「整幅配色变了」，开着脏追踪才观察
/// 得到）；同值重推两半都不做（稳态帧的缓存收益与不重绘，宿主 attach 会原样再推一次当前档）。
/// 本文件的其余用例每次新建一棵树，观察不到这两段。
///
/// 两轴解算面另三格：未显式设主题时注入主题的**风格轴跟随探测档**（`Theme::default_style()`，
/// 真机读数即真机观感，故本格的判据是「与探测结果自洽」而非某个具体档）、显式 `set_theme`
/// 整份携带两轴、`set_theme_style` 只钉风格轴而明暗仍随 OS 翻转。做绝对色值断言的用例一律先
/// `set_theme_style(Material)` 钉住风格档——这是 specification/07-environment-modifier.md §5.2
/// 测试约定的既有要求，否则判据随 runner 的 OS 家族漂移（Windows 构建与 Linux 构建的默认观感
/// 本就应当不同）。

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

/// @brief 取一枚**与本机探测档不同**的风格档。
///
/// 钉档类断言必须钉「与默认解算结果不同」的那一枚：若钉住的档恰与 `Theme::default_style()` 同值，
/// 「显式钉风格轴」与「完全忽略风格轴」的退化实现会给出同样的注入结果，判据当场空转。
/// Windows 构建上探测档就是 `Windows`，故这里从 KDE / GNOME 起挑。
[[nodiscard]] auto style_distinct_from_probed() -> aurora::ThemeStyle {
    const aurora::ThemeStyle probed = aurora::Theme::default_style();
    for (const aurora::ThemeStyle candidate :
         {aurora::ThemeStyle::Kde, aurora::ThemeStyle::Gnome, aurora::ThemeStyle::MacOs, aurora::ThemeStyle::Windows}) {
        if (candidate != probed) {
            return candidate;
        }
    }
    return aurora::ThemeStyle::Material;  // 五档全同值不可达（枚举只有五档且候选已覆盖四档）。
}

/// @brief 常驻树探针：逐帧记录**布局期**与**绘制期**读到的根主题底色。
///
/// 与 `probe_theme` 的分工：后者每次新建一棵树，`present_root` 必走重挂载，观察不到「同一棵常驻树
/// 换档」——而那正是根注入缓存失效要负责的场景（脏标记只向上溯达祖先，后代不主动失效即停在旧档）。
/// 本探针**不禁**布局缓存：缓存短路下一帧就是缺陷现象，换档帧整子树失效生效才是判据。
class ThemeObserver final : public aurora::Widget {
  public:
    std::vector<aurora::Color> layout_surfaces;  ///< 每帧一条：`on_layout` 期读到的 `roles.surface`。
    std::vector<aurora::Color> paint_surfaces;  ///< 每帧一条：`on_paint` 期读到的 `roles.surface`。

    [[nodiscard]] auto type_name() const -> const char * override { return "ThemeObserver"; }

  protected:
    auto on_layout(const aurora::Constraints &c, const aurora::BuildContext &ctx) -> aurora::Size override {
        layout_surfaces.push_back(aurora::inherit_theme(ctx).roles.surface);
        return c.max;  // 撑满父约束（无子节点，尺寸不参与断言）
    }

    auto on_paint(aurora::Painter & /*p*/, const aurora::Rect & /*bounds*/, const aurora::BuildContext &ctx)
        -> void override {
        paint_surfaces.push_back(aurora::inherit_theme(ctx).roles.surface);
    }
};

}  // namespace

AURORA_TEST_CASE(default_injection_is_light_and_concrete_scheme) {
    auto win = make_window(headless_opts("theme_default"));
    // 钉住风格档再做绝对色值断言（§5.2 测试约定）：本格要证的是「明暗轴的回落与具体档」，
    // 不是「哪套设计语言」——后者由 default_injection_style_follows_the_probed_value 单独证。
    win->set_theme_style(aurora::ThemeStyle::Material);
    const aurora::Theme t = probe_theme(*win);
    // 未 set_theme、未推 OS 档 → 明色回落；scheme 为具体 Light（非 Unspecified）。
    AURORA_TEST_CHECK(t.scheme == ColorScheme::Light);
    AURORA_TEST_CHECK(t.roles.surface == Color::white());
    AURORA_TEST_CHECK(t.roles.primary == Color{0x00, 0x00, 0xFF});
}

AURORA_TEST_CASE(default_injection_style_follows_the_probed_value) {
    // 风格轴的唯一生产入口就是这里：应用什么都不设时，注入主题的 `style` 必须等于探测档，
    // 且整套令牌就是该档的标定值。判据取「与探测结果自洽」而非某个具体档——真机读数本就随
    // OS 家族不同（Windows 构建得 Windows 档、无头构建得 Material 档），写死档名会让同一份用例
    // 在两种 runner 上一红一绿。
    auto win = make_window(headless_opts("theme_default_style"));
    const aurora::Theme t = probe_theme(*win);
    AURORA_TEST_CHECK_EQ(t.style, aurora::Theme::default_style());
    // 明暗轴与风格轴各自独立解算：OS 未表态时落具体 Light（不为 Unspecified），取值即该档明色列。
    AURORA_TEST_CHECK_EQ(t.scheme, ColorScheme::Light);
    const aurora::Theme expect = aurora::Theme::from_style(t.style, ColorScheme::Light);
    AURORA_TEST_CHECK_TRUE(t.roles == expect.roles);
    AURORA_TEST_CHECK_TRUE(t.type_scale == expect.type_scale);
    AURORA_TEST_CHECK_TRUE(t.shapes == expect.shapes);
    AURORA_TEST_CHECK_TRUE(t.conventions == expect.conventions);
    // 兜底三点同源的第一点：挂 `Window` 且未显式设主题 → 得探测档（另两点在无 Provider 与裸
    // ctx 处，由 utest_theme_probe / utest_theme 钉 Material 基线不变）。
    AURORA_TEST_CHECK_FALSE(win->theme_style().has_value());
}

AURORA_TEST_CASE(set_theme_style_pins_the_style_axis_and_leaves_the_scheme_to_os) {
    // 两轴正交在应用侧的表达：`set_theme_style` 只钉风格轴，明暗仍随 OS 档翻转。
    // 钉的是「与本机探测档不同」的那一枚，否则忽略钉档的退化实现同样能通过（判据空转）。
    auto win = make_window(headless_opts("theme_style_only"));
    const aurora::ThemeStyle pinned = style_distinct_from_probed();
    win->set_theme_style(pinned);
    // optional 与值直接比较：同时钉「已钉档」与「钉的就是这一枚」，且不触发可选值的无检查解引用。
    AURORA_TEST_CHECK_TRUE(win->theme_style() == pinned);
    AURORA_TEST_CHECK_TRUE(pinned != aurora::Theme::default_style());

    win->set_os_color_scheme(ColorScheme::Dark);
    const aurora::Theme dark = probe_theme(*win);
    AURORA_TEST_CHECK_EQ(dark.style, pinned);
    AURORA_TEST_CHECK_EQ(dark.scheme, ColorScheme::Dark);
    AURORA_TEST_CHECK_TRUE(dark.roles == aurora::Theme::from_style(pinned, ColorScheme::Dark).roles);

    win->set_os_color_scheme(ColorScheme::Light);
    const aurora::Theme light = probe_theme(*win);
    AURORA_TEST_CHECK_EQ(light.style, pinned);
    AURORA_TEST_CHECK_EQ(light.scheme, ColorScheme::Light);
    // 同一风格档的明暗两档只在配色面分叉：风格身份位与度量面不随明暗变化（正交的可观测面）。
    AURORA_TEST_CHECK_EQ(light.style, dark.style);
    AURORA_TEST_CHECK_TRUE(light.shapes == dark.shapes);
    AURORA_TEST_CHECK_TRUE(light.conventions == dark.conventions);
    AURORA_TEST_CHECK(!(light.roles == dark.roles));
}

AURORA_TEST_CASE(explicit_theme_overrides_both_axes) {
    // 显式 `set_theme` 携带完整两轴取值，`set_theme_style` 被它整份覆盖（短路在风格解算之前）。
    auto win = make_window(headless_opts("theme_explicit_both_axes"));
    win->set_theme_style(aurora::ThemeStyle::Windows);
    win->set_os_color_scheme(ColorScheme::Dark);
    win->set_theme(aurora::Theme::from_style(aurora::ThemeStyle::Kde, ColorScheme::Light));
    const aurora::Theme t = probe_theme(*win);
    AURORA_TEST_CHECK_EQ(t.style, aurora::ThemeStyle::Kde);
    AURORA_TEST_CHECK_EQ(t.scheme, ColorScheme::Light);
    AURORA_TEST_CHECK_TRUE(t.roles == aurora::Theme::from_style(aurora::ThemeStyle::Kde, ColorScheme::Light).roles);
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
    // 钉风格档：下面两枚 `surface == white` 是绝对色值断言（§5.2 测试约定）。
    win->set_theme_style(aurora::ThemeStyle::Material);
    win->set_os_color_scheme(ColorScheme::Dark);
    const aurora::Theme dark = probe_theme(*win);
    AURORA_TEST_CHECK(dark.scheme == ColorScheme::Dark);
    AURORA_TEST_CHECK(dark.roles.surface != aurora::Color::white());  // background 由浅变深

    win->set_os_color_scheme(ColorScheme::Light);
    const aurora::Theme light = probe_theme(*win);
    AURORA_TEST_CHECK(light.scheme == ColorScheme::Light);
    AURORA_TEST_CHECK(light.roles.surface == aurora::Color::white());
}

AURORA_TEST_CASE(theme_axis_change_reaches_a_persistent_tree) {
    // 常驻树换肤：三枚主题 setter 的「下一帧生效」必须连带一次整子树缓存失效，否则后代沿用旧档的
    // 布局 / Display List 解算结果。真机复现于 examples/demos/demo_theme_styles.cpp：切风格档后
    // 按钮与滑杆仍按旧 primary 绘制——根注入值与脏标记都对，只有后代缓存是旧的。
    auto win = make_window(headless_opts("theme_live_switch"));
    win->enable_dirty_tracking(false);  // 无脏的下一帧会被整帧跳过，注入复评无从发生
    win->set_theme_style(aurora::ThemeStyle::Material);

    auto probe = std::make_shared<ThemeObserver>();
    aurora::Node node{probe};
    (void)win->present_root(node);
    AURORA_TEST_REQUIRE_EQ(probe->layout_surfaces.size(), 1U);
    AURORA_TEST_REQUIRE_EQ(probe->paint_surfaces.size(), 1U);
    const aurora::Theme material_light = aurora::Theme::from_style(aurora::ThemeStyle::Material, ColorScheme::Light);
    AURORA_TEST_CHECK(probe->layout_surfaces[0] == material_light.roles.surface);
    AURORA_TEST_CHECK(probe->paint_surfaces[0] == material_light.roles.surface);

    // 明暗轴换档：同一棵树、探针自身没有任何脏标记，只靠换档帧的向下失效读到新档。
    win->set_os_color_scheme(ColorScheme::Dark);
    (void)win->present_root(node);
    AURORA_TEST_REQUIRE_EQ(probe->layout_surfaces.size(), 2U);
    const aurora::Theme material_dark = aurora::Theme::from_style(aurora::ThemeStyle::Material, ColorScheme::Dark);
    AURORA_TEST_CHECK(probe->layout_surfaces[1] == material_dark.roles.surface);
    AURORA_TEST_REQUIRE_EQ(probe->paint_surfaces.size(), 2U);
    AURORA_TEST_CHECK(probe->paint_surfaces[1] == material_dark.roles.surface);

    // 风格轴换档：钉一枚与当前档不同的（同档则判据空转，与 style_distinct_from_probed 同一口径）。
    const aurora::ThemeStyle other = aurora::ThemeStyle::Windows;
    win->set_theme_style(other);
    (void)win->present_root(node);
    AURORA_TEST_REQUIRE_EQ(probe->layout_surfaces.size(), 3U);
    const aurora::Theme other_dark = aurora::Theme::from_style(other, ColorScheme::Dark);
    AURORA_TEST_CHECK(probe->layout_surfaces[2] == other_dark.roles.surface);
    AURORA_TEST_CHECK(!(other_dark.roles.surface == material_dark.roles.surface));

    // 同值重推不置挂起位：宿主 attach 会原样再推一次当前 OS 档，稳态帧的布局缓存必须仍然有效
    // （否则每次 attach / 每次重复推送都白重排整子树）。
    const std::size_t settled = probe->layout_surfaces.size();
    win->set_os_color_scheme(ColorScheme::Dark);
    win->set_theme_style(other);
    (void)win->present_root(node);
    AURORA_TEST_CHECK_EQ(probe->layout_surfaces.size(), settled);
}

AURORA_TEST_CASE(theme_axis_change_marks_the_whole_window_dirty) {
    // 换肤的第二半：像素面。上一格证「后代缓存失效 ⇒ 重排读到新档」，本格证「这一帧真的被重绘」——
    // 只失效不标脏时逻辑全对、屏幕不动（增量脏区不会自己推断出「整窗配色变了」）。
    // 本格刻意**不关**脏追踪：关了就没有增量脏区可言，判据当场空转。
    auto win = make_window(headless_opts("theme_dirty_full_redraw"));
    win->set_theme_style(aurora::ThemeStyle::Material);
    auto probe = std::make_shared<ThemeObserver>();
    aurora::Node node{probe};
    (void)win->present_root(node);  // 消费首帧脏区，回到无脏稳态
    AURORA_TEST_REQUIRE_FALSE(win->has_pending_dirty());

    // 换风格档 ⇒ 整窗标脏（变异自证：删 setter 里的 force_full_redraw 本格必红）。
    win->set_theme_style(aurora::ThemeStyle::Gnome);
    AURORA_TEST_CHECK_TRUE(win->has_pending_dirty());
    (void)win->present_root(node);
    AURORA_TEST_CHECK_FALSE(win->has_pending_dirty());  // 生效帧消费掉脏区，不留残脏

    // 明暗轴同理：Unspecified → Light 是一次真换档。
    win->set_os_color_scheme(ColorScheme::Light);
    AURORA_TEST_CHECK_TRUE(win->has_pending_dirty());
    (void)win->present_root(node);
    const std::size_t settled = probe->paint_surfaces.size();
    AURORA_TEST_CHECK_FALSE(win->has_pending_dirty());

    // 同值重推既不标脏也不重绘：宿主 attach 会原样再推一次当前档，稳态帧不该被整帧重绘。
    win->set_os_color_scheme(ColorScheme::Light);
    win->set_theme_style(aurora::ThemeStyle::Gnome);
    AURORA_TEST_CHECK_FALSE(win->has_pending_dirty());
    (void)win->present_root(node);
    AURORA_TEST_CHECK_EQ(probe->paint_surfaces.size(), settled);
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
    // 转发 → Window::set_os_color_scheme（换档两半：整窗标脏 + 生效帧向下失效）→ 下一帧根注入翻转。
    WindowOptions opts;
    opts.title = "theme_os_change";
    opts.size = Size{.width = 320.0F, .height = 240.0F};
    Application app{make_scene("os-change"), make_window(headless_opts("theme_os_change")), opts};
    WindowHost *const host = app.windows().front();
    HeadlessSurface *const surface = surface_of(host);
    AURORA_TEST_REQUIRE_NOT_NULL(surface);

    // 先固定明色基线（不依赖真机 OS 档位），再经 seam 推暗——判据才与运行环境无关。
    // 风格档同批钉住：本用例尾上的 `surface == white` 是绝对色值断言（§5.2 测试约定）。
    host->window()->set_theme_style(aurora::ThemeStyle::Material);
    host->window()->set_os_color_scheme(ColorScheme::Light);
    AURORA_TEST_CHECK(probe_theme(*host->window()).scheme == ColorScheme::Light);
    // 基线自证：探针与 render_frame 后须无残脏，否则「emit 后置脏」无法区分「seam 置脏」
    // 与「本就未清的残脏」，判据会空转（变异自证：删 set_os_color_scheme 的 force_full_redraw 必红）。
    AURORA_TEST_CHECK_FALSE(host->window()->has_pending_dirty());
    (void)host->render_frame(0.016);
    AURORA_TEST_CHECK_FALSE(host->window()->has_pending_dirty());

    surface->emit_os_color_scheme_change(ColorScheme::Dark);
    // seam 置脏（换档 setter 内 force_full_redraw 的可观测面）：翻转必须伴随整帧重排重绘，
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
