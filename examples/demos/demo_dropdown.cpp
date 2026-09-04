// Dropdown 下拉选择器 demo：点击展开选项列表，选择后收起。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    auto label = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"Selected: Medium"});

    au::Dropdown dd{std::vector<std::string>{"Small", "Medium", "Large", "X-Large"}, 1};
    dd.set_on_change([label](int i) -> void {
        static constexpr std::array AURORA_NAMES = {"Small", "Medium", "Large", "X-Large"};
        if (i < 0 || static_cast<std::size_t>(i) >= AURORA_NAMES.size()) {
            return;
        }
        // NOLINTNEXTLINE(*-pro-bounds-constant-array-index)
        label->set(au::LocalizedString{std::string("Selected: ") + AURORA_NAMES.at(static_cast<std::size_t>(i))});
    });

    au::Node root = au::Column{
        GradientTitle{"Dropdown selector"},
        gap(12),
        std::move(dd),
        gap(12),
        au::Text{au::TextProps{.content = au::Reactive{label}}},
    };
    return run_demo(Card{std::move(root)}, "Dropdown · Aurora Demo", 440.0F, 360.0F);
}