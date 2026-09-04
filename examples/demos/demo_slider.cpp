// Slider 控件 demo：连续值 [0,1]，拖拽设置，配合 State 实时反映。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    auto val = std::make_shared<au::State<double>>(0.5);
    auto label = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"Value 0.50"});

    au::Slider sl{au::Reactive{val}, [val, label](double v) -> void {
                      val->set(v);
                      label->set(au::LocalizedString{"Value " + std::to_string(v)});
                  }};
    sl.set_active_color(pal::AURORA_PRIMARY);

    au::Node root = au::Column{
        GradientTitle{"Slider widget"},
        gap(12),
        std::move(sl),
        au::Text{au::TextProps{.content = au::Reactive{label}}},
    };
    return run_demo(Card{std::move(root)}, "Slider · Aurora Demo", 480.0F, 360.0F);
}