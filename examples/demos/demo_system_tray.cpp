// 系统托盘 demo：图标 / 悬浮提示 / 气泡通知 / 右键菜单 / 激活回调。
//
// 真实 Win32 实现（Shell_NotifyIcon + 隐藏消息窗口）仅在 `AURORA_PLATFORM_WINDOWS` 下生效，
// 其余平台全部方法退化为 no-op，故观测量集中在 Windows 桌面：通知区域是否出现图标、
// 气泡是否弹出、右键菜单是否可点、左键是否触发 on_activate。
// 主窗提供同一批托盘 API 的按钮，便于在不动鼠标找图标的情况下对照同一路径。
//
// 托盘消息由隐藏消息窗口接收，与主窗同线程；`Application` 的帧循环用
// `PeekMessage(nullptr, ...)` 抽取本线程全部消息，故托盘回调无需额外的消息循环。
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "demo_common.h"

namespace {

/// @brief 生成一个点击即执行 fn 的按钮节点（免写 Node{} 包裹）。
auto action_button(const char *label, std::function<void()> fn) -> au::Node {
    au::Button button{au::ButtonProps{.label = label}};
    button.on_click = std::move(fn);
    return au::Node{std::move(button)};
}

}  // namespace

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    au::enable_dpi_awareness();  // 必须在建窗前，否则 DPI 感知失败（scale=1.0，高分屏发虚）
    au::init_console();

    au::WindowOptions opts;
    opts.size = au::Size{.width = 520.0F, .height = 340.0F};
    opts.title = "System tray · Aurora Demo";
    auto win_res = au::create_native_window(opts);
    if (!win_res) {
        AURORA_LOG_ERROR("demo", "[tray] window creation failed: ", win_res.error().message);
        return -1;
    }
    au::Application app{au::Scene{au::Column{}}, std::move(win_res.value()), opts};

    // 主窗回显最近一次气泡正文（托盘点击后无需切回窗口即可看到结果）。
    auto last_msg = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"(no balloon yet)"});
    auto show_balloon = [&app, &last_msg](au::SystemTray &tray, const char *msg) -> void {
        tray.show_balloon("Aurora", msg);
        last_msg->set(au::LocalizedString{tray.last_balloon_message()});
        AURORA_LOG_INFO("demo", "[tray] balloon shown");
    };

    au::SystemTray tray{"Aurora tray demo"};
    tray.set_context_menu({
        au::MenuItem{"Show balloon",
                     [&tray, &show_balloon]() -> void { show_balloon(tray, "Balloon from the tray context menu"); }},
        au::MenuItem::separator_item(),
        au::MenuItem{"Hide icon",
                     [&tray]() -> void {
                         tray.hide();
                         AURORA_LOG_INFO("demo", "[tray] icon hidden");
                     }},
        au::MenuItem{"Show icon",
                     [&tray]() -> void {
                         tray.show();
                         AURORA_LOG_INFO("demo", "[tray] icon shown");
                     }},
        au::MenuItem::separator_item(),
        au::MenuItem{"Quit",
                     [&app]() -> void {
                         AURORA_LOG_INFO("demo", "[tray] quit requested from tray menu");
                         app.quit();
                     }},
    });
    tray.on_activate([&tray, &show_balloon]() -> void {
        show_balloon(tray, "Icon activated");
        AURORA_LOG_INFO("demo", "[tray] activated");
    });

    au::Node root = au::Column{au::ColumnProps{
        .children = {
            GradientTitle{"System tray"},
            gap(8),
            au::Text{au::TextProps{.content = au::Reactive{last_msg}}},
            gap(8),
            action_button("Show balloon",
                          [&tray, &show_balloon]() -> void { show_balloon(tray, "Balloon from the window"); }),
            gap(8),
            action_button("Hide tray icon", [&tray]() -> void { tray.hide(); }),
            gap(8),
            action_button("Show tray icon", [&tray]() -> void { tray.show(); }),
            gap(12),
            au::Text{"Left-click the tray icon to fire on_activate; "
                     "right-click it for the context menu."},
        }}};
    app.scene().root_node() = std::move(root);
    app.focus().set_root(&app.scene().root());

    AURORA_LOG_INFO("demo", "[tray] window shown; look for the icon in the notification area");
    app.run();
    return 0;
}
