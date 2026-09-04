// Stack 控件 demo：多层从 (0,0) 叠加，可用 Alignment 对齐。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    au::Node bottom =
        au::Stack{au::Text{au::LocalizedString{"Bottom text"}}, au::Text{au::LocalizedString{"Top overlay text"}}};
    au::Node centered =
        au::Stack{std::vector<au::Node>{au::Text{au::LocalizedString{"Center overlay"}}}, au::Alignment::Center};

    au::Node root = au::Column{
        GradientTitle{"Stack widget"},
        gap(12),
        au::Text{au::LocalizedString{"Default top-left overlay"}},
        std::move(bottom),
        gap(12),
        au::Text{au::LocalizedString{"Alignment::Center overlay"}},
        std::move(centered),
    };
    return run_demo(Card{std::move(root)}, "Stack · Aurora Demo", 520.0F, 440.0F);
}