// InspectorPanel demo：左侧 Widget 树浏览器，右侧属性面板，中间分隔条可拖拽。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    // 构建一个示例 UI 树供 Inspector 检视
    auto target_tree = []() -> au::Node {
        au::TextInput name_input;
        name_input.set_value("Alice").set_placeholder("Enter name");

        au::TextInput email_input;
        email_input.set_value("alice@example.com").set_placeholder("Enter email");

        return au::Node{au::Column{au::ColumnProps{.children =
                                                       {
                                                           au::Node{au::Text{"User info form"}},
                                                           au::Node{std::move(name_input)},
                                                           au::Node{std::move(email_input)},
                                                           au::Node{au::Button{au::ButtonProps{.label = "Submit"}}},
                                                           au::Node{au::Checkbox{au::Reactive{true}}},
                                                       },
                                                   .gap = 8.0F}}};
    };

    // InspectorPanel 包裹目标树
    au::InspectorPanel inspector{target_tree, 0.4F};

    // 用 Splitter 分栏：左侧 Inspector，右侧被检查的 UI
    au::Node target = target_tree();
    au::Splitter splitter =
        au::HSplitter(au::Node{std::move(inspector)},
                      au::Node{au::Column{au::ColumnProps{.children =
                                                              {
                                                                  au::Node{au::Text{"Inspected UI tree"}},
                                                                  target,
                                                              },
                                                          .gap = 8.0F}}},
                      0.4F);
    splitter.set_min_sizes(180.0F, 200.0F);

    au::Node root = au::Column{
        GradientTitle{"Inspector panel"},
        gap(8),
        std::move(splitter),
    };
    return run_demo(Card{std::move(root)}, "Inspector · Aurora Demo", 720.0F, 480.0F);
}