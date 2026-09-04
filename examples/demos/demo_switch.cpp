// Switch 控件 demo：布尔开关，点击切换，配合 State 实时反映。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    auto on = std::make_shared<au::State<bool>>(false);
    auto label = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"Off"});

    au::Switch sw{au::Reactive{on}, [on, label](bool v) -> void {
                      on->set(v);
                      label->set(au::LocalizedString{v ? "On" : "Off"});
                  }};

    au::Node root = au::Column{
        GradientTitle{"Switch widget"},
        gap(12),
        au::Row{std::move(sw), au::Text{au::TextProps{.content = au::Reactive{label}}}},
    };
    return run_demo(Card{std::move(root)}, "Switch · Aurora Demo", 480.0F, 320.0F);
}