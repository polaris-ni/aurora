// Lifecycle 控件 demo：声明式挂载/卸载副作用钩子（对齐 React useEffect / Flutter initState+dispose）。
// 演示：挂载后恰好跑一次副作用（计数 + 控制台日志）；卸载时清理（控制台日志）。
// 用 Show 显隐子树不会重新挂载（mount 计数保持 1，子树保留存活，对齐 Flutter Visibility）；
// 关闭窗口才触发 on_unmount（控件 Node 析构）。
#include <memory>
#include <string>

#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    auto mount_count = std::make_shared<au::State<int>>(0);
    auto status = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"Not mounted"});
    auto count_label =
        std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"on_mount trigger count: 0"});
    auto visible = std::make_shared<au::State<bool>>(true);

    // 被包裹的子树：挂载时计数 + 打印，卸载时打印（RAII 析构触发）。
    au::Node subtree = au::Lifecycle(
        au::Text{au::LocalizedString{"This subtree is wrapped by Lifecycle"}},
        [mount_count, status, count_label](const au::BuildContext &) -> void {
            mount_count->set(mount_count->get() + 1);
            const int n = mount_count->get();
            status->set(au::LocalizedString{"Mounted (#" + std::to_string(n) + ")"});
            count_label->set(au::LocalizedString{"on_mount trigger count: " + std::to_string(n)});
            AURORA_LOG_INFO("demo", "[Lifecycle] on_mount #", n);
        },
        [status]() -> void {
            status->set(au::LocalizedString{"Unmounted"});
            AURORA_LOG_INFO("demo", "[Lifecycle] on_unmount");
        });

    au::Button toggle{au::ButtonProps{.label = au::LocalizedString{"Show/hide subtree (Show)"}}};
    toggle.on_click = [visible]() -> void { visible->set(!visible->get()); };

    au::Node root = au::Column{
        GradientTitle{"Lifecycle widget"},
        gap(12),
        std::move(toggle),
        gap(8),
        au::Show{visible, std::move(subtree)},
        gap(8),
        au::Text{au::TextProps{.content = au::Reactive{status}}},
        au::Text{au::TextProps{.content = au::Reactive{count_label}}},
        au::Text{au::LocalizedString{"Tip: showing/hiding subtree with Show does not remount (count stays 1); "
                                     "on_unmount triggers only when window closes"}},
    };
    return run_demo(Card{std::move(root)}, "Lifecycle · Aurora Demo", 520.0F, 440.0F);
}