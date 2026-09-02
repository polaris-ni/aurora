// ToastHost 通知 demo：点击按钮弹出自动消失的通知（3 秒），可连发排队。
#include "demo_common.h"

auto main() -> int {
    au::Button notify_btn{ au::ButtonProps{ .label = "Show notification" } };
    au::Button clear_btn{ au::ButtonProps{ .label = "Clear" } };

    auto content = au::Column{
        GradientTitle{ "Toast / SnackBar notification" },
        gap(12),
        au::Row{ std::move(notify_btn), std::move(clear_btn) },
    };

    const auto host = std::make_shared<au::ToastHost>(au::Node{ Card{ std::move(content) } });

    // 重新接线按钮回调（host 构造后才能引用）
    auto counter = std::make_shared<int>(0);
    // 演示：直接经命令式 API 触发
    host->show("Welcome to Aurora Toast!", 3000.0f);

    au::Node root{ std::shared_ptr<au::Widget>(host) };
    return run_demo(std::move(root), "Toast · Aurora Demo", 480.0f, 360.0f);
}
