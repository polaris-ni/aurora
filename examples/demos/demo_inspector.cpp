// InspectorPanel demo：左侧 Widget 树浏览器，右侧属性面板，中间分隔条可拖拽。
#include "demo_common.h"

auto main() -> int {
    // 构建一个示例 UI 树供 Inspector 检视
    auto target_tree = []() -> au::Node {
        au::TextInput name_input;
        name_input.set_value("Alice").set_placeholder("输入姓名");

        au::TextInput email_input;
        email_input.set_value("alice@example.com").set_placeholder("输入邮箱");

        return au::Node{ au::Column{ au::ColumnProps{ .children = {
            au::Node{ au::Text{ "用户信息表单" } },
            au::Node{ std::move(name_input) },
            au::Node{ std::move(email_input) },
            au::Node{ au::Button{ au::ButtonProps{ .label = "提交" } } },
            au::Node{ au::Checkbox{ au::Reactive<bool>{ true } } },
        }, .gap = 8.0f } } };
    };

    // InspectorPanel 包裹目标树
    au::InspectorPanel inspector{ target_tree, 0.4f };

    // 用 Splitter 分栏：左侧 Inspector，右侧被检查的 UI
    au::Node target = target_tree();
    au::Splitter splitter = au::HSplitter(
        au::Node{ std::move(inspector) },
        au::Node{ au::Column{ au::ColumnProps{ .children = {
            au::Node{ au::Text{ "被检查的 UI 树" } },
            target,
        }, .gap = 8.0f } } },
        0.4f);
    splitter.set_min_sizes(180.0f, 200.0f);

    au::Node root = au::Column{
        GradientTitle{ "Inspector 面板" },
        gap(8),
        std::move(splitter),
    };
    return run_demo(Card{ std::move(root) }, "Inspector · Aurora Demo", 720.0f, 480.0f);
}
