// Dropdown 下拉选择器 demo：点击展开选项列表，选择后收起。
#include "demo_common.h"

auto main() -> int {
    auto label = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{ "已选: Medium" });

    au::Dropdown dd{ std::vector<std::string>{ "Small", "Medium", "Large", "X-Large" }, 1 };
    dd.set_on_change([label](int i) -> void {
        static constexpr std::array aurora_names = { "Small", "Medium", "Large", "X-Large" };
        if (i < 0 || static_cast<std::size_t>(i) >= aurora_names.size()) {
            return;
        }
        // NOLINTNEXTLINE(*-pro-bounds-constant-array-index)
        label->set(au::LocalizedString{ std::string("已选: ") + aurora_names[static_cast<std::size_t>(i)] });
    });

    au::Node root = au::Column{
        GradientTitle{ "Dropdown 下拉选择" },
        gap(12),
        std::move(dd),
        gap(12),
        au::Text{ au::TextProps{ .content = au::Reactive{ label } } },
    };
    return run_demo(Card{ std::move(root) }, "Dropdown · Aurora Demo", 440.0f, 360.0f);
}
