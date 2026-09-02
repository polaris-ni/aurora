// TitleBar 控件 demo：Borderless 窗口 + 声明式标题栏（标题/副标题/动作区 + 内置
// 窗口控制钮）。窗口动作（拖拽移动/双击最大化/最小化/关闭/Snap 弹窗）经 Environment
// 注入的 WindowChrome 服务下发给平台 Surface（headless 回退渲染时安全 no-op）。
// 交互点：
//   ① 标题栏空白区单击拖拽移动窗口   ② 双击空白区切换最大化
//   ③ 右侧控制钮：最小化 / 最大化还原 / 关闭
//   ④ 悬停最大化钮 ≥400ms 弹出 Snap 动作菜单（含自定义「左半屏」项）
//   ⑤ 窗口失焦时标题栏自动变暗
// ⚠️ 本 demo 使用 DecorationPolicy::Borderless：在 GNOME 等 SSD 缺失的合成器上，
//    标题栏完全由本控件接管（KDE 上则与系统装饰并存，仅作演示）。
// 运行：cmake --build build --target demo_title_bar && ./build/demo_title_bar
#include "aurora/widget/title_bar.h"

#include "demo_common.h"

auto main() -> int {
    au::enable_dpi_awareness();
    au::init_console();

    constexpr float w = 720.0f;
    constexpr float h = 480.0f;

    // 配置声明式标题栏（经 WindowChrome 驱动窗口；headless 无 chrome 时交互安全跳过）。
    au::TitleBar titlebar;
    titlebar.set_title("Aurora TitleBar Demo")
        .set_subtitle("Borderless + WindowChrome")
        .add_action({ .label = "Settings", .on_click = []() -> void { AURORA_LOG_INFO("demo", "settings clicked"); } })
        .add_snap_action(
            { .label = "Left half screen (custom)", .on_click = []() -> void { AURORA_LOG_INFO("demo", "custom snap action"); } });

    au::Node root = au::Column{
        au::Node{ std::move(titlebar) },
        gap(12),
        GradientTitle{ "TitleBar" },
        au::Text{ "Window actions dispatched via WindowChrome; hover maximize button to try Snap popup" },
    };

    au::FocusManager fm;
    fm.set_root(&root.widget());

    au::WindowOptions wopts;
    wopts.size = au::Size{ .width = w, .height = h };
    wopts.title = "TitleBar · Aurora Demo";
    wopts.style.decoration = au::DecorationPolicy::Borderless;

    auto win_res = au::create_native_window(wopts);
    if (!win_res) {
        AURORA_LOG_ERROR("demo", "[demo_title_bar] window creation failed: ", win_res.error().message, ", falling back to headless render");
        std::error_code ec;
        std::filesystem::create_directories("build", ec);
        au::Scene scene{ root };
        auto r = scene.render_to_png("build/demo_title_bar.png", static_cast<int>(w), static_cast<int>(h));
        if (r) {
            AURORA_LOG_INFO("demo", "[demo_title_bar] rendered build/demo_title_bar.png");
        } else {
            AURORA_LOG_ERROR("demo", "[demo_title_bar] headless render failed: ", r.error().message);
        }
        return 0;
    }

    auto win = std::move(win_res.value());
    win->surface().set_event_handler([&](au::Event &e) -> void {
        auto &wd = root.widget();
        if (auto *me = dynamic_cast<au::MouseEvent *>(&e)) {
            au::EventDispatcher::dispatch(wd, *me, &fm);
        } else if (auto *ke = dynamic_cast<au::KeyEvent *>(&e)) {
            au::EventDispatcher::dispatch(wd, *ke, fm);
        } else if (auto *se = dynamic_cast<au::ScrollEvent *>(&e)) {
            au::EventDispatcher::dispatch(wd, *se);
        } else if (auto *te = dynamic_cast<au::TextInputEvent *>(&e)) {
            au::EventDispatcher::dispatch(wd, *te, fm);
        }
    });

    AURORA_LOG_INFO("demo", "[demo_title_bar] window shown (close window to exit)");
    // 与 run_demo 同款事件驱动帧循环（present_root 内部按需 begin，勿手调 begin_frame）。
    win->run([&]() -> void { (void)win->present_root(root); });
    return 0;
}
