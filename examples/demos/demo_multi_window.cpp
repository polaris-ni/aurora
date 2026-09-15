// 多窗口 demo（specification/06-app-platform.md §2.4）：一个 Application 同时驱动多个独立窗口。
//
// 每个窗口是一个 `WindowHost`：拥有自己的 Scene（UI 树）、FocusManager 与帧统计，
// 由 `Application` 的单一帧循环统一驱动——关掉一个窗口不影响其他窗口继续渲染。
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "aurora/app/application.h"
#include "aurora/app/display.h"
#include "demo_common.h"

namespace {

/// @brief 跨窗广播载荷：演示 `Application::bus()` 的「一次性通知」通道。
/// （共享**状态**应直接用共享 `Store`/`State`，bus 只承载动作类通知。）
struct PingMessage {
    std::string text;
};

auto count_label(int n) -> au::LocalizedString { return au::LocalizedString{"windows = " + std::to_string(n)}; }

/// @brief 辅助窗口的 UI：自带关闭按钮（经 `WindowHost` 的 id 程序化关闭）+ 跨窗广播回显。
auto aux_ui(int index, std::function<void()> close_self,
            const std::shared_ptr<au::State<au::LocalizedString>> &last_ping) -> au::Node {
    au::Button close_btn{au::ButtonProps{.label = "Close this window"}};
    close_btn.on_click = std::move(close_self);
    return au::Column{
        au::ColumnProps{
            .children =
                {
                    GradientTitle{"Auxiliary #" + std::to_string(index)},
                    gap(12),
                    au::Text{"Each window owns its own Scene + FocusManager."},
                    gap(8),
                    au::Text{au::TextProps{.content = au::Reactive{last_ping}}},
                    gap(8),
                    std::move(close_btn),
                },
        },
    };
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    au::enable_dpi_awareness();  // 必须在建窗前，否则 DPI 感知失败（scale=1.0，高分屏发虚）
    au::init_console();

    // 按钮回调在 UI 构建时尚无 Application 实例，故先留槽位，构造后回填。
    auto spawn = std::make_shared<std::function<void()>>([]() -> void {});
    auto close_last = std::make_shared<std::function<void()>>([]() -> void {});
    auto quit_app = std::make_shared<std::function<void()>>([]() -> void {});
    auto open_modal = std::make_shared<std::function<void()>>([]() -> void {});
    auto move_display = std::make_shared<std::function<void()>>([]() -> void {});
    auto broadcast = std::make_shared<std::function<void()>>([]() -> void {});

    auto count = std::make_shared<au::State<int>>(1);
    auto label = std::make_shared<au::State<au::LocalizedString>>(count_label(1));
    auto last_id = std::make_shared<au::WindowId>(au::kInvalidWindowId);
    // 跨窗广播的回显值：所有窗口（含主窗与之后的辅窗）都绑定同一个 State。
    auto last_ping = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"(no broadcast yet)"});

    au::Button new_btn{au::ButtonProps{.label = "New auxiliary window"}};
    new_btn.on_click = [spawn]() -> void { (*spawn)(); };
    au::Button close_btn{au::ButtonProps{.label = "Close last auxiliary window"}};
    close_btn.on_click = [close_last]() -> void { (*close_last)(); };
    au::Button quit_btn{au::ButtonProps{.label = "Quit app (Application::quit)"}};
    quit_btn.on_click = [quit_app]() -> void { (*quit_app)(); };
    au::Button modal_btn{au::ButtonProps{.label = "Open modal dialog (blocks main window)"}};
    modal_btn.on_click = [open_modal]() -> void { (*open_modal)(); };
    au::Button display_btn{au::ButtonProps{.label = "Move main window to next display"}};
    display_btn.on_click = [move_display]() -> void { (*move_display)(); };
    au::Button bcast_btn{au::ButtonProps{.label = "Broadcast to all windows (bus)"}};
    bcast_btn.on_click = [broadcast]() -> void { (*broadcast)(); };

    au::Node root = au::Column{
        au::ColumnProps{
            .children =
                {
                    GradientTitle{"Multi-window"},
                    gap(12),
                    au::Text{au::TextProps{.content = au::Reactive{label}}},
                    gap(8),
                    std::move(new_btn),
                    gap(8),
                    std::move(close_btn),
                    gap(8),
                    std::move(quit_btn),
                    gap(8),
                    std::move(modal_btn),
                    gap(8),
                    std::move(display_btn),
                    gap(8),
                    std::move(bcast_btn),
                    gap(12),
                    au::Text{au::TextProps{.content = au::Reactive{last_ping}}},
                    gap(12),
                    au::Text{"Auxiliary windows are independent: closing one leaves the rest running."},
                    gap(4),
                    au::Text{"Closing the main window exits the app (ExitPolicy::LastWindowClosed)."},
                },
        },
    };

    au::WindowOptions opts;
    opts.size = au::Size{.width = 520.0F, .height = 380.0F};
    opts.title = "Aurora multi-window (main)";
    auto win_res = au::create_native_window(opts);
    if (!win_res) {
        AURORA_LOG_ERROR("demo", "[multi_window] main window creation failed: ", win_res.error().message);
        return -1;
    }
    au::Application app{au::Scene{std::move(root)}, std::move(win_res.value()), opts};

    auto created = std::make_shared<int>(0);
    *spawn = [&app, created, last_id, last_ping]() -> void {
        ++(*created);
        au::WindowOptions o;
        o.size = au::Size{.width = 440.0F, .height = 300.0F};
        o.title = "Auxiliary #" + std::to_string(*created);
        o.role = au::WindowRole::Auxiliary;  // 辅助窗口：关闭不影响其他窗口
        auto w = au::create_native_window(o);
        if (!w) {
            AURORA_LOG_ERROR("demo", "[multi_window] auxiliary window creation failed: ", w.error().message);
            return;
        }
        // 子节点（关闭按钮）需要自己的 id，而 id 由 open_window 返回：用共享槽位回填。
        auto self_id = std::make_shared<au::WindowId>(au::kInvalidWindowId);
        const au::WindowId id = app.open_window(
            std::move(w.value()),
            au::Scene{aux_ui(*created, [&app, self_id]() -> void { app.close_window(*self_id); }, last_ping)}, o);
        *self_id = id;
        *last_id = id;
        AURORA_LOG_INFO("demo", "[multi_window] opened window id=", id, " total=", app.window_count());
    };

    *close_last = [&app, last_id]() -> void {
        if (*last_id == au::kInvalidWindowId) {
            return;
        }
        app.close_window(*last_id);
        *last_id = au::kInvalidWindowId;
    };

    *quit_app = [&app]() -> void { app.quit(); };  // 任何退出策略下都能主动结束帧循环

    *open_modal = [&app]() -> void {
        au::WindowOptions o;
        o.size = au::Size{.width = 360.0F, .height = 200.0F};
        o.title = "Modal dialog";
        o.role = au::WindowRole::Transient;
        o.owner = app.main_window();  // 依附主窗口
        o.modal = true;               // 打开期间屏蔽 owner 输入，关闭后自动恢复
        auto w = au::create_native_window(o);
        if (!w) {
            AURORA_LOG_ERROR("demo", "[multi_window] modal window creation failed: ", w.error().message);
            return;
        }
        auto self_id = std::make_shared<au::WindowId>(au::kInvalidWindowId);
        au::Button close{au::ButtonProps{.label = "Close modal"}};
        close.on_click = [&app, self_id]() -> void { app.close_window(*self_id); };
        au::Node ui = au::Column{au::ColumnProps{.children =
                                                    {
                                                        GradientTitle{"Modal dialog"},
                                                        gap(12),
                                                        au::Text{"Main window input is blocked until this closes."},
                                                        gap(8),
                                                        std::move(close),
                                                    }}};
        *self_id = app.open_window(std::move(w.value()), au::Scene{std::move(ui)}, o);
    };

    auto display_idx = std::make_shared<std::size_t>(0);
    *move_display = [&app, display_idx]() -> void {
        const auto displays = au::app::list_displays();
        if (displays.size() < 2) {
            AURORA_LOG_INFO("demo", "[multi_window] single display; nothing to move to");
            return;
        }
        *display_idx = (*display_idx + 1U) % displays.size();
        if (auto *h = app.window_host(app.main_window())) {
            h->move_to_display(displays[*display_idx].id);
            AURORA_LOG_INFO("demo", "[multi_window] moved main window to display ", displays[*display_idx].name);
        }
    };

    // 跨窗通信：订阅广播。订阅句柄须存活到 run() 结束，故持有为局部变量（RAII：main 返回时自动取消）。
    // 广播只负责「通知」；各窗口的展示值来自共享的 `last_ping` State。
    const auto ping_sub = app.bus().on<PingMessage>([last_ping](const PingMessage &e, au::WindowId) -> void {
        last_ping->set(au::LocalizedString{e.text});
    });
    auto ping_seq = std::make_shared<int>(0);
    *broadcast = [&app, ping_seq]() -> void {
        ++(*ping_seq);
        // 携带发送者窗口 id：订阅侧可据此实现点对点过滤（这里全量广播）。
        app.bus().post(PingMessage{.text = "broadcast #" + std::to_string(*ping_seq)}, app.main_window());
    };

    // 每帧同步窗口计数：窗口被关闭且帧末回收后，主窗计数随之更新。
    app.set_on_frame([&app, count, label]() -> void {
        const int n = static_cast<int>(app.window_count());
        if (n != count->get()) {
            count->set(n);
            label->set(count_label(n));
        }
    });

    AURORA_LOG_INFO("demo", "[multi_window] main window shown (close it to exit)");
    app.run();
    return 0;
}
