// Accessibility 无障碍树验证：由控件树构建角色/动作/结构。

#include <iostream>
#include <memory>

#include "aurora/aurora.h"
#include "test_harness.h"

AURORA_TEST() {
    // 构造一棵小树：Column{Button, Text, Column{Checkbox, Switch}}
    const auto btn = std::make_shared<aurora::Button>("OK");
    const auto txt = std::make_shared<aurora::Text>("hello");
    const auto cb = std::make_shared<aurora::Checkbox>();
    const auto sw = std::make_shared<aurora::Switch>();
    const auto inner = std::make_shared<aurora::Column>(aurora::Column{au::Node{cb}, au::Node{sw}});
    const auto root = std::make_shared<au::Column>(au::Column{au::Node{btn}, au::Node{txt}, au::Node{inner}});

    constexpr au::BuildContext ctx;
    root->mount(ctx);
    au::Constraints c;
    c.min = au::Size{.width = 0, .height = 0};
    c.max = au::Size{.width = 320, .height = 240};
    root->layout(c, ctx);

    const au::AccessibilityNode tree = au::build_accessibility_tree(*root);

    AURORA_TEST_CHECK_MSG(tree.role == au::AccessibilityRole::Generic, "root Column -> Generic");
    AURORA_TEST_CHECK_MSG(tree.children.size() == 3, "root has 3 children");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(tree.children[0].role == au::AccessibilityRole::Button, "child 0 Button");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(tree.children[1].role == au::AccessibilityRole::Text, "child 1 Text");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(tree.children[2].role == au::AccessibilityRole::Generic, "child 2 inner Column Generic");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(tree.children[2].children.size() == 2, "inner has 2 children");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(tree.children[2].children[0].role == au::AccessibilityRole::Checkbox,
                          "inner child 0 Checkbox");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(tree.children[2].children[1].role == au::AccessibilityRole::Switch, "inner child 1 Switch");

    // 动作集
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(tree.children[0].has_action(au::AccessibilityAction::Click), "Button has Click action");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(tree.children[0].has_action(au::AccessibilityAction::Invoke), "Button has Invoke action");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(tree.children[2].children[0].has_action(au::AccessibilityAction::Toggle),
                          "Checkbox has Toggle action");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(tree.children[2].children[1].has_action(au::AccessibilityAction::Toggle),
                          "Switch has Toggle action");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(tree.children[1].has_action(au::AccessibilityAction::Focus), "Text has Focus action");

    // 节点总数 = root + button + text + inner + checkbox + switch = 6
    AURORA_TEST_CHECK_MSG(accessibility_node_count(tree) == 6, "total node count 6");

    // 角色推断工具
    AURORA_TEST_CHECK_MSG(au::infer_accessibility_role("Button") == au::AccessibilityRole::Button, "infer Button");
    AURORA_TEST_CHECK_MSG(au::infer_accessibility_role("LazyList") == au::AccessibilityRole::List,
                          "infer LazyList -> List");
    AURORA_TEST_CHECK_MSG(au::infer_accessibility_role("GridView") == au::AccessibilityRole::List,
                          "infer GridView -> List");
    AURORA_TEST_CHECK_MSG(au::infer_accessibility_role("Unknown") == au::AccessibilityRole::Generic,
                          "infer unknown -> Generic");
}