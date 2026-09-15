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

/// @brief 主窗 UI：在 `Application` 已构造之后构建，故按钮回调可直接捕获 `app`。
/// 可变状态（`created`/`last_id`/`display_idx`/`ping_seq`）以引用捕获，生命周期覆盖 `run()`。
auto build_main_ui(au::Application &app, const std::shared_ptr<au::State<au::LocalizedString>> &label,
                   const std::shared_ptr<au::State<au::LocalizedString>> &last_ping, int &created,
                   au::WindowId &last_id, std::size_t &display_idx, int &ping_seq) -> au::Node {
    au::Button new_btn{au::ButtonProps{.label = "New auxiliary window"}};
    new_btn.on_click = [&app, &created, &last_id, &last_ping]() -> void {
        ++created;
        au::WindowOptions o;
        o.size = au::Size{.width = 440.0F, .height = 300.0F};
        o.title = "Auxiliary #" + std::to_string(created);
        o.role = au::WindowRole::Auxiliary;  // 辅助窗口：关闭不影响其他窗口
        auto w = au::create_native_window(o);
        if (!w) {
            AURORA_LOG_ERROR("demo", "[multi_window] auxiliary window creation failed: ", w.error().message);
            return;
        }
        // 子节点（关闭按钮）需要自己的 id，而 id 由 open_window 返回：用共享槽位回填。
        auto self_id = std::make_shared<au::WindowId>(au::AURORA_INVALID_WINDOW_ID);
        const au::WindowId id = app.open_window(
            std::move(w.value()),
            au::Scene{aux_ui(created, [&app, self_id]() -> void { app.close_window(*self_id); }, last_ping)}, o);
        *self_id = id;
        last_id = id;
        AURORA_LOG_INFO("demo", "[multi_window] opened window id=", id, " total=", app.window_count());
    };

    au::Button close_btn{au::ButtonProps{.label = "Close last auxiliary window"}};
    close_btn.on_click = [&app, &last_id]() -> void {
        if (last_id == au::AURORA_INVALID_WINDOW_ID) {
            return;
        }
        app.close_window(last_id);
        last_id = au::AURORA_INVALID_WINDOW_ID;
    };

    au::Button quit_btn{au::ButtonProps{.label = "Quit app (Application::quit)"}};
    quit_btn.on_click = [&app]() -> void { app.quit(); };  // 任何退出策略下都能主动结束帧循环

    au::Button modal_btn{au::ButtonProps{.label = "Open modal dialog (blocks main window)"}};
    modal_btn.on_click = [&app]() -> void {
        au::WindowOptions o;
        o.size = au::Size{.width = 360.0F, .height = 200.0F};
        o.title = "Modal dialog";
        o.role = au::WindowRole::Transient;
        o.owner = app.main_window();  // 依附主窗口
        o.modal = true;  // 打开期间屏蔽 owner 输入，关闭后自动恢复
        auto w = au::create_native_window(o);
        if (!w) {
            AURORA_LOG_ERROR("demo", "[multi_window] modal window creation failed: ", w.error().message);
            return;
        }
        auto self_id = std::make_shared<au::WindowId>(au::AURORA_INVALID_WINDOW_ID);
        au::Button close{au::ButtonProps{.label = "Close modal"}};
        close.on_click = [&app, self_id]() -> void { app.close_window(*self_id); };
        au::Node ui = au::Column{au::ColumnProps{.children = {
                                                     GradientTitle{"Modal dialog"},
                                                     gap(12),
                                                     au::Text{"Main window input is blocked until this closes."},
                                                     gap(8),
                                                     std::move(close),
                                                 }}};
        *self_id = app.open_window(std::move(w.value()), au::Scene{std::move(ui)}, o);
    };

    au::Button display_btn{au::ButtonProps{.label = "Move main window to next display"}};
    display_btn.on_click = [&app, &display_idx]() -> void {
        const auto displays = au::app::list_displays();
        if (displays.size() < 2) {
            AURORA_LOG_INFO("demo", "[multi_window] single display; nothing to move to");
            return;
        }
        display_idx = (display_idx + 1U) % displays.size();
        if (auto *h = app.window_host(app.main_window())) {
            h->move_to_display(displays[display_idx].id);
            AURORA_LOG_INFO("demo", "[multi_window] moved main window to display ", displays[display_idx].name);
        }
    };

    au::Button bcast_btn{au::ButtonProps{.label = "Broadcast to all windows (bus)"}};
    bcast_btn.on_click = [&app, &ping_seq]() -> void {
        ++ping_seq;
        // 携带发送者窗口 id：订阅侧可据此实现点对点过滤（这里全量广播）。
        app.bus().post(PingMessage{.text = "broadcast #" + std::to_string(ping_seq)}, app.main_window());
    };

    return au::Column{
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
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    au::enable_dpi_awareness();  // 必须在建窗前，否则 DPI 感知失败（scale=1.0，高分屏发虚）
    au::init_console();

    auto count = std::make_shared<au::State<int>>(1);
    auto label = std::make_shared<au::State<au::LocalizedString>>(count_label(1));
    // 跨窗广播的回显值：所有窗口（含主窗与之后的辅窗）都绑定同一个 State。
    auto last_ping = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"(no broadcast yet)"});

    // 命令型可变状态：被主树各按钮回调以引用捕获，生命周期覆盖 run()。
    int created = 0;
    au::WindowId last_id = au::AURORA_INVALID_WINDOW_ID;
    std::size_t display_idx = 0;
    int ping_seq = 0;

    au::WindowOptions opts;
    opts.size = au::Size{.width = 520.0F, .height = 380.0F};
    opts.title = "Aurora multi-window (main)";
    auto win_res = au::create_native_window(opts);
    if (!win_res) {
        AURORA_LOG_ERROR("demo", "[multi_window] main window creation failed: ", win_res.error().message);
        return -1;
    }
    // 主窗 UI 需要引用 app，故先以占位根构造 app，再在 app 存在后回填真实主树（并刷新焦点根）。
    au::Application app{au::Scene{au::Column{}}, std::move(win_res.value()), opts};
    app.scene().root_node() = build_main_ui(app, label, last_ping, created, last_id, display_idx, ping_seq);
    app.focus().set_root(&app.scene().root());

    // 跨窗通信：订阅广播。订阅句柄须存活到 run() 结束，故持有为局部变量（RAII：main 返回时自动取消）。
    // 广播只负责「通知」；各窗口的展示值来自共享的 `last_ping` State。
    const auto ping_sub = app.bus().on<PingMessage>(
        [last_ping](const PingMessage &e, au::WindowId) -> void { last_ping->set(au::LocalizedString{e.text}); });

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
