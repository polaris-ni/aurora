// OS 级全局热键 demo：注册 / 注销 / 命中计数。
//
// 真实实现（Windows `RegisterHotKey` + 隐藏消息窗口接 `WM_HOTKEY`）仅在
// `AURORA_PLATFORM_WINDOWS` 下生效，其余平台 `OsHotkeyRegistry::enabled()` 为 false、
// `add()` 返回 OsHotkeyRegisterFailed，界面上回显这条失败原因——降级是可见的，不是静默 no-op。
//
// 命中不就地回调：`WM_HOTKEY` 到达时只把 ID 入队，帧循环里的 `drain_pending()` 才执行动作，
// 故回调里改状态（本 demo 即自增计数 + 回显）不会在消息泵中途打乱布局与绘制。
#include <functional>
#include <memory>
#include <string>

#include "aurora/app/os_hotkey.h"
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
    opts.title = "OS hotkey · Aurora Demo";
    auto win_res = au::create_native_window(opts);
    if (!win_res) {
        AURORA_LOG_ERROR("demo", "[hotkey] window creation failed: ", win_res.error().message);
        return -1;
    }
    au::Application app{au::Scene{au::Column{}}, std::move(win_res.value()), opts};

    // 三条回显：平台支持位 / 最近一次操作结果 / 命中次数。
    auto status = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"(idle)"});
    auto hits = std::make_shared<au::State<int>>(0);
    auto hits_text = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"hits = 0"});

    au::OsHotkeyRegistry hotkeys;
    au::OsHotkeyHandle registered{};

    auto log_status = [&status](const std::string &text) -> void {
        status->set(au::LocalizedString{text});
        AURORA_LOG_INFO("demo", "[hotkey] ", text);
    };
    auto refresh_hits = [&hits, &hits_text](int next) -> void {
        hits->set(next);
        hits_text->set(au::LocalizedString{"hits = " + std::to_string(next)});
    };

    log_status(hotkeys.enabled() ? "backend available; press Register to grab Ctrl+Shift+K"
                                 : "no OS global hotkey backend on this platform");

    // 帧循环里排空：命中在这里才转成状态变更，不在窗口过程内执行。
    app.set_on_frame([&hotkeys, &hits, &refresh_hits, &log_status]() -> void {
        const std::size_t fired = hotkeys.drain_pending();
        if (fired == 0U) {
            return;
        }
        refresh_hits(hits->get() + static_cast<int>(fired));
        log_status("hotkey triggered");
    });

    au::Node root = au::Column{au::ColumnProps{
        .children = {
            GradientTitle{"OS hotkey"},
            gap(8),
            au::Text{au::TextProps{.content = au::Reactive{status}}},
            gap(8),
            au::Text{au::TextProps{.content = au::Reactive{hits_text}}},
            gap(12),
            action_button("Register Ctrl+Shift+K",
                          [&hotkeys, &registered, &log_status]() -> void {
                              if (registered != au::OsHotkeyHandle{}) {
                                  log_status("already registered; unregister first");
                                  return;
                              }
                              auto res = hotkeys.add(
                                  au::KeyCombo{au::ModifierKey::Control | au::ModifierKey::Shift, au::KeyCode::K},
                                  []() -> void {});
                              if (!res) {
                                  log_status("register failed: " + res.error().message);
                                  return;
                              }
                              registered = res.value();
                              log_status("registered; switch focus away and press Ctrl+Shift+K");
                          }),
            gap(8),
            action_button("Unregister",
                          [&hotkeys, &registered, &log_status]() -> void {
                              if (!hotkeys.remove(registered)) {
                                  log_status("nothing to unregister");
                                  return;
                              }
                              registered = au::OsHotkeyHandle{};
                              log_status("unregistered");
                          }),
            gap(12),
            au::Text{"Focus another window and press the combination: the hit is queued by the "
                     "message pump and drained once per frame."},
            gap(8),
            au::Text{"Unregister on exit is automatic: the registry releases every OS hotkey in "
                     "its destructor."},
        }}};
    app.scene().root_node() = std::move(root);
    app.focus().set_root(&app.scene().root());

    AURORA_LOG_INFO("demo", "[hotkey] window shown; hotkey backend enabled=", hotkeys.enabled());
    app.run();
    return 0;
}
