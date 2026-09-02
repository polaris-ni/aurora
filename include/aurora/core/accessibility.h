#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aurora/core/types.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 无障碍角色：对标 WAI-ARIA / UIAutomation ControlType 的精简子集。
enum class AccessibilityRole : std::uint8_t {
    Generic,   ///< 未知 / 通用容器
    Button,    ///< 可点击按钮
    Text,      ///< 静态文本
    TextInput, ///< 可编辑文本
    Checkbox,  ///< 复选框
    Switch,    ///< 开关
    Slider,    ///< 滑块
    Image,     ///< 图片
    List,      ///< 列表 / 网格容器
    ListItem,  ///< 列表项
    Header,    ///< 标题
    Progress,  ///< 进度指示
    Dialog,    ///< 对话框 / 弹层
};

/// @brief 无障碍动作位掩码（可组合）。
enum class AccessibilityAction : std::uint16_t {
    None = 0,
    Focus = 1u << 0u,
    Click = 1u << 1u,
    Value = 1u << 2u, ///< 可设值
    Select = 1u << 3u,
    Invoke = 1u << 4u, ///< 默认动作（按钮触发等）
    Toggle = 1u << 5u, ///< 切换状态（复选 / 开关）
};

[[nodiscard]] inline auto operator|(AccessibilityAction a, AccessibilityAction b) -> AccessibilityAction {
    return static_cast<AccessibilityAction>(static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b));
}

/// @brief 无障碍节点：运行时控件树的可访问视图。
/// @note Thread: main-thread only
/// @note Side-effects: pure
struct AccessibilityNode {
    AccessibilityRole role = AccessibilityRole::Generic;
    std::string name;  ///< 可读标签（控件自填，如按钮文字）
    std::string value; ///< 当前值（如文本内容、复选状态）
    Rect bounds;       ///< 屏幕坐标盒（由布局填充，默认空）
    AccessibilityAction actions = AccessibilityAction::None;
    std::vector<AccessibilityNode> children;

    [[nodiscard]] auto has_action(AccessibilityAction a) const -> bool {
        return (static_cast<std::uint16_t>(actions) & static_cast<std::uint16_t>(a)) != 0;
    }
};

/// @brief 由控件 `type_name()` 推断无障碍角色。
[[nodiscard]] inline auto infer_accessibility_role(const std::string_view type_name) -> AccessibilityRole {
    if (type_name == "Button") {
        return AccessibilityRole::Button;
    }
    if (type_name == "Text" || type_name == "RichText" || type_name == "Label") {
        return AccessibilityRole::Text;
    }
    if (type_name == "TextInput" || type_name == "RichTextEdit") {
        return AccessibilityRole::TextInput;
    }
    if (type_name == "Checkbox") {
        return AccessibilityRole::Checkbox;
    }
    if (type_name == "Switch") {
        return AccessibilityRole::Switch;
    }
    if (type_name == "Slider") {
        return AccessibilityRole::Slider;
    }
    if (type_name == "Image" || type_name == "ImageView" || type_name == "SvgImage") {
        return AccessibilityRole::Image;
    }
    if (type_name == "LazyList" || type_name == "GridView" || type_name == "ListView" || type_name == "DataTable" ||
        type_name == "TreeView") {
        return AccessibilityRole::List;
    }
    if (type_name == "ProgressIndicator") {
        return AccessibilityRole::Progress;
    }
    if (type_name == "Dialog" || type_name == "Popup" || type_name == "Drawer") {
        return AccessibilityRole::Dialog;
    }
    if (type_name == "Header" || type_name == "AppBar") {
        return AccessibilityRole::Header;
    }
    return AccessibilityRole::Generic;
}

/// @brief 由角色推断默认动作集。
[[nodiscard]] inline auto default_actions(const AccessibilityRole r) -> AccessibilityAction {
    switch (r) {
    case AccessibilityRole::Generic: return AccessibilityAction::Focus;
    case AccessibilityRole::Button:
        return AccessibilityAction::Click | AccessibilityAction::Invoke | AccessibilityAction::Focus;
    case AccessibilityRole::Text: return AccessibilityAction::Focus;
    case AccessibilityRole::TextInput: return AccessibilityAction::Value | AccessibilityAction::Focus;
    case AccessibilityRole::Checkbox:
    case AccessibilityRole::Switch: return AccessibilityAction::Toggle | AccessibilityAction::Focus;
    case AccessibilityRole::Slider: return AccessibilityAction::Value | AccessibilityAction::Focus;
    case AccessibilityRole::Image: return AccessibilityAction::None; // 非交互：图片无可执行动作
    case AccessibilityRole::List:
    case AccessibilityRole::ListItem: {
        return AccessibilityAction::Focus;
    }
    case AccessibilityRole::Header: // 非交互：标题
    case AccessibilityRole::Progress: {
        return AccessibilityAction::None;
    } // 非交互：进度指示
    case AccessibilityRole::Dialog: return AccessibilityAction::Focus;
    default: return AccessibilityAction::None; // 兜底：未来新增角色默认无动作，不再静默继承 Focus
    }
}

/// @brief 递归构建控件树的无障碍视图。
/// @note 不依赖 GUI 后端；bounds 默认为空（由布局阶段或宿主另行填充）。
[[nodiscard]] inline auto build_accessibility_tree(const Widget &root) -> AccessibilityNode {
    AccessibilityNode node;
    node.role = infer_accessibility_role(root.type_name());
    node.actions = default_actions(node.role);
    root.for_each_child([&](const Widget &child) -> void { node.children.push_back(build_accessibility_tree(child)); });
    return node;
}

/// @brief 统计无障碍树节点总数（含根）。
[[nodiscard]] inline auto accessibility_node_count(const AccessibilityNode &n) -> std::size_t {
    std::size_t c = 1;
    for (const auto &ch : n.children) {
        c += accessibility_node_count(ch);
    }
    return c;
}

} // namespace aurora
