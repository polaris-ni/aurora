// 系统通知 demo：Win32 气球 / XDG 桌面通知的统一入口（含紧急度、超时、tag 去重与激活回调）。
//
// 观测面：点击按钮后应在桌面的通知渠道看到一条通知（Windows 在通知区域短暂出现一个临时托盘
// 图标 + 气球，气球消失后图标会被撤掉，不长期占位；Linux 走 libnotify / libdbus / notify-send
// 三层降级链）。点击那条通知应把 tag 回传到窗口里的「last activated」一行。
//
// 为什么要有 pulsing 定时任务：Linux 下没有 GLib 主循环的宿主是拿不到点击回调的，
// `NotificationCenter::pump_events()` 就是那个「给通知层一次排空机会」的钩子；
// 这里用调度器每 50ms 打一次（Windows 上它是 no-op，那里的回调由宿主消息泵送达）。
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "aurora/app/notification.h"
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
    opts.size = au::Size{.width = 560.0F, .height = 400.0F};
    opts.title = "System notification · Aurora Demo";
    auto win_res = au::create_native_window(opts);
    if (!win_res) {
        AURORA_LOG_ERROR("demo", "[notification] window creation failed: ", win_res.error().message);
        return -1;
    }
    au::Application app{au::Scene{au::Column{}}, std::move(win_res.value()), opts};

    auto status = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"(nothing posted yet)"});
    auto activated = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"(no notification clicked yet)"});

    au::NotificationCenter::set_on_notification_activated([activated](std::string tag) -> void {
        // 空 tag 的通知在平台层被换成一个占位动作键，回传时已还原成调用方的语义（空串）。
        const std::string shown = tag.empty() ? "(untagged)" : tag;
        activated->set(au::LocalizedString{shown});
        AURORA_LOG_INFO("demo", "[notification] activated: ", shown);
    });

    // 发送一条通知并把结果（成功/失败原因）写回状态行。
    auto post = [&status](const std::string &tag, au::NotificationUrgency urgency, std::uint32_t timeout_ms) -> void {
        au::Notification n;
        n.title = "Aurora demo";
        n.body = "Notification posted at " + std::to_string(timeout_ms) + " ms expire policy";
        n.tag = tag;
        n.urgency = urgency;
        n.timeout_ms = timeout_ms;
        const auto result = au::NotificationCenter::notify(n);
        status->set(au::LocalizedString{result.ok() ? ("ok: " + tag) : ("failed: " + result.error().message)});
        AURORA_LOG_INFO("demo", "[notification] post ", tag, result.ok() ? " ok" : " failed");
    };

    // 每帧给通知层一次排空机会：Linux 上这是激活回调能否到达的前提。
    (void)app.scheduler().set_interval(std::chrono::milliseconds(50),
                                       []() -> void { au::NotificationCenter::pump_events(); });

    au::Node root = au::Column{au::ColumnProps{
        .children = {
            GradientTitle{"System notification"},
            gap(8),
            au::Text{au::TextProps{.content = au::Reactive{status}}},
            gap(8),
            au::Text{au::TextProps{.content = au::Reactive{activated}}},
            gap(12),
            action_button("Post low urgency", [&post]() -> void { post("demo-low", au::NotificationUrgency::Low, 0); }),
            gap(8),
            action_button("Post normal urgency",
                          [&post]() -> void { post("demo-normal", au::NotificationUrgency::Normal, 6000); }),
            gap(8),
            action_button("Post critical urgency",
                          [&post]() -> void { post("demo-critical", au::NotificationUrgency::Critical, 0); }),
            gap(8),
            action_button("Post again with same tag (replaces)",
                          [&post]() -> void { post("demo-normal", au::NotificationUrgency::Normal, 6000); }),
            gap(12),
            au::Text{"Click the notification itself: its tag shows up in the line above. "
                     "On Windows the temporary tray icon disappears once the balloon is gone; "
                     "on Linux it needs an XDG notification server."},
        }}};
    app.scene().root_node() = std::move(root);
    app.focus().set_root(&app.scene().root());

#ifdef AURORA_BUILD_INSPECTOR_SERVER
    // 无人值守取证通道：状态行文本走 `/api/tree` 读（根在 `run()` 前一次性回填，故闭包
    // 直接引用稳定根即可）。未设 AURORA_INSPECTOR_PORT 时不启动。
    auto inspector = start_demo_inspector(
        [&app]() -> au::Node { return au::Node{app.scene().root_node()}; },
        [&app]() -> au::Surface * { return app.window() != nullptr ? &app.window()->surface() : nullptr; });
#endif

    AURORA_LOG_INFO("demo", "[notification] window shown; press a button to post a desktop notification");
    app.run();
    return 0;
}
