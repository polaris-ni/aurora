// Lifecycle 控件 demo：声明式挂载/卸载副作用钩子（对齐 React useEffect / Flutter initState+dispose）。
// 演示：挂载后恰好跑一次副作用（计数 + 控制台日志）；卸载时清理（控制台日志）。
// 用 Show 显隐子树不会重新挂载（mount 计数保持 1，子树保留存活，对齐 Flutter Visibility）；
// 关闭窗口才触发 on_unmount（控件 Node 析构）。
#include <memory>
#include <string>

#include "demo_common.h"

auto main() -> int {
    auto mount_count = std::make_shared<au::State<int>>(0);
    auto status = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{ "未挂载" });
    auto count_label = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{ "on_mount 触发次数: 0" });
    auto visible = std::make_shared<au::State<bool>>(true);

    // 被包裹的子树：挂载时计数 + 打印，卸载时打印（RAII 析构触发）。
    au::Node subtree = au::Lifecycle(
        au::Text{ au::LocalizedString{ "这是被 Lifecycle 包裹的子树" } },
        [mount_count, status, count_label](const au::BuildContext &) -> void {
            mount_count->set(mount_count->get() + 1);
            const int n = mount_count->get();
            status->set(au::LocalizedString{ "已挂载（第 " + std::to_string(n) + " 次）" });
            count_label->set(au::LocalizedString{ "on_mount 触发次数: " + std::to_string(n) });
            AURORA_LOG_INFO("demo", "[Lifecycle] on_mount #", n);
        },
        [status]() -> void {
            status->set(au::LocalizedString{ "已卸载" });
            AURORA_LOG_INFO("demo", "[Lifecycle] on_unmount");
        });

    au::Button toggle{ au::ButtonProps{ .label = au::LocalizedString{ "显隐子树（Show）" } } };
    toggle.on_click = [visible]() -> void { visible->set(!visible->get()); };

    au::Node root = au::Column{
        GradientTitle{ "Lifecycle 控件" },
        gap(12),
        std::move(toggle),
        gap(8),
        au::Show{ visible, std::move(subtree) },
        gap(8),
        au::Text{ au::TextProps{ .content = au::Reactive{ status } } },
        au::Text{ au::TextProps{ .content = au::Reactive{ count_label } } },
        au::Text{
            au::LocalizedString{ "提示：用 Show 显隐子树不会重新挂载（计数保持 1）；关闭窗口才触发 on_unmount" } },
    };
    return run_demo(Card{ std::move(root) }, "Lifecycle · Aurora Demo", 520.0f, 440.0f);
}
