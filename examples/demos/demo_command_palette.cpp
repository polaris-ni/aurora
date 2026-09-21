// Command palette demo（对照 VSCode command palette）。
//
// 演示命令系统的三源统一：同一份 CommandRegistry 同时供给
//   ① 快捷键 —— 命令的 default_binding 经 bind_shortcuts 接入快捷键表；
//   ② 菜单数据 —— 经 to_menu_items() 生成 MenuItem（此处打印投影片段，真实应用交给 MenuBar）；
//   ③ 命令面板 —— Ctrl+K 唤出，模糊检索 + 键盘导航 + 回车执行。
//
// 用 Application（而非 run_demo）：命令注册表与快捷键表挂在 Application 上，须在 run() 前接线。
#include <memory>
#include <string>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/commands.h"
#include "aurora/widget/command_palette.h"
#include "aurora/widget/stack.h"
#include "aurora/widget/text.h"
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    auto palette = std::make_shared<au::CommandPalette>();
    auto status = std::make_shared<au::Text>("No command executed yet.");

    au::Node content = au::Column{
        au::ColumnProps{
            .children =
                {
                    GradientTitle{"Command Palette"},
                    gap(12),
                    au::Text{"Ctrl+K opens the palette; Ctrl+H / Ctrl+T run commands directly."},
                    au::Text{"Type to filter, Up/Down to move, Enter to run, Esc to close."},
                    gap(16),
                    Card{au::Node{status}},
                },
        },
    };

    // 面板铺满窗口（遮罩覆盖全屏）：栈 + Expand 让根按窗口约束撑开。
    auto stack = std::make_shared<au::Stack>(std::vector<au::Node>{std::move(content), au::Node{palette}});
    stack->set_fit(au::StackFit::Expand);
    au::Scene scene{au::Node{stack}};

    au::WindowOptions opts;
    opts.size = au::Size{.width = 720.0F, .height = 480.0F};
    opts.title = "Command Palette · Aurora Demo";
    auto win_res = create_native_window(opts);
    au::Application app{std::move(scene), win_res ? std::move(win_res.value()) : nullptr, opts};

    // ---- 命令注册（唯一真源）----
    au::Command greet;
    greet.id = "demo.greet";
    greet.title = "Say Hello";
    greet.icon = "chat";
    greet.category = "Demo";
    greet.default_binding = au::KeyCombo{au::ModifierKey::Control, au::KeyCode::H};
    greet.action = [status]() -> void { status->set_content("Executed: Say Hello"); };
    app.commands().add(std::move(greet));

    au::Command invert;
    invert.id = "demo.invert";
    invert.title = "Invert Theme";
    invert.icon = "palette";
    invert.category = "View";
    invert.default_binding = au::KeyCombo{au::ModifierKey::Control, au::KeyCode::T};
    invert.action = [status]() -> void { status->set_content("Executed: Invert Theme"); };
    app.commands().add(std::move(invert));

    au::Command hidden;
    hidden.id = "demo.hidden";
    hidden.title = "Unavailable Action";
    hidden.category = "Demo";
    hidden.when_label = "never";
    hidden.enabled = []() -> bool { return false; };  // 由启用条件隐藏：不出现在面板/菜单投影里
    app.commands().add(std::move(hidden));

    au::Command placeholder;
    placeholder.id = "demo.placeholder";
    placeholder.title = "Placeholder Command";
    placeholder.category = "Demo";  // 无 action：可列出但不可调用（面板内以弱色显示）
    app.commands().add(std::move(placeholder));

    // ---- 三源接线 ----
    app.commands().bind_shortcuts(app.shortcuts());  // ① 默认快捷键
    palette->set_commands(&app.commands());  // ③ 命令面板
    app.shortcuts().add(
        au::KeyCombo{au::ModifierKey::Control, au::KeyCode::K}, [palette]() -> void { palette->toggle(); },
        au::ShortcutScope::Global, "Open command palette");

    // ② 菜单投影：把命令转成菜单项数据（真实应用交给 MenuBar；此处打印以示意同一真源）。
    std::string menu_preview = "Menu projection:";
    for (const au::MenuItem &item : app.commands().to_menu_items()) {
        menu_preview += " [" + item.label + (item.enabled ? "" : " (disabled)") + "]";
    }
    status->set_content(menu_preview);

    app.run();
    return 0;
}
