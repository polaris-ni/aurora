// Checkbox 控件 demo：勾选态 bool，点击切换，配合 State 实时反映。
// 同时展示样式自定义：主题色跟随、active/check/border 颜色、尺寸、圆角、禁用态与 hover 反馈。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    auto checked = std::make_shared<au::State<bool>>(false);
    auto label = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"Unchecked"});

    au::Checkbox cb{au::Reactive{checked}, [checked, label](bool v) -> void {
                        checked->set(v);
                        label->set(au::LocalizedString{v ? "Checked" : "Unchecked"});
                    }};

    au::Checkbox cb2{au::Reactive{std::make_shared<au::State<bool>>(true)}};
    cb2.set_active_color(pal::AURORA_OK);

    // 自定义勾号色 + 大尺寸 + 大圆角
    au::Checkbox cb3{au::Reactive{std::make_shared<au::State<bool>>(true)}};
    cb3.set_active_color(au::Color{255, 122, 0, 255})
        .set_check_color(au::Color{40, 26, 0, 255})
        .set_size(28.0F)
        .set_corner_radius(9.0F);

    // 禁用态（勾选 / 未勾选各一）
    au::Checkbox cb4{au::Reactive{std::make_shared<au::State<bool>>(true)}};
    cb4.set_enabled(false);
    au::Checkbox cb5{au::Reactive{std::make_shared<au::State<bool>>(false)}};
    cb5.set_enabled(false);

    au::Node root = au::Column{
        GradientTitle{"Checkbox widget"},
        gap(12),
        au::Row{std::move(cb), au::Text{au::TextProps{.content = au::Reactive{label}}}},
        gap(12),
        au::Row{std::move(cb2), au::Text{au::LocalizedString{"Pre-checked + custom active color"}}},
        gap(12),
        au::Row{std::move(cb3), au::Text{au::LocalizedString{"Large size + large radius + custom check color"}}},
        gap(12),
        au::Row{std::move(cb4), std::move(cb5), au::Text{au::LocalizedString{"Disabled state (not clickable)"}}},
    };
    return run_demo(Card{std::move(root)}, "Checkbox · Aurora Demo", 480.0F, 400.0F);
}