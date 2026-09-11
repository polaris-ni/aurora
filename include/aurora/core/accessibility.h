#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/types.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 无障碍角色：对标 WAI-ARIA / UIAutomation ControlType 的精简子集。
enum class AccessibilityRole : std::uint8_t {
    Generic,  ///< 未知 / 通用容器
    Button,  ///< 可点击按钮
    Text,  ///< 静态文本
    TextInput,  ///< 可编辑文本
    Checkbox,  ///< 复选框
    Switch,  ///< 开关
    Slider,  ///< 滑块
    Image,  ///< 图片
    List,  ///< 列表 / 网格容器
    ListItem,  ///< 列表项
    Header,  ///< 标题
    Progress,  ///< 进度指示
    Dialog,  ///< 对话框 / 弹层
};

/// @brief 无障碍动作位掩码（可组合）。
enum class AccessibilityAction : std::uint16_t {  // NOLINT(*-enum-size)
    None = 0,
    Focus = 1U << 0U,
    Click = 1U << 1U,
    Value = 1U << 2U,  ///< 可设值
    Select = 1U << 3U,
    Invoke = 1U << 4U,  ///< 默认动作（按钮触发等）
    Toggle = 1U << 5U,  ///< 切换状态（复选 / 开关）
};

[[nodiscard]] inline auto operator|(AccessibilityAction a, AccessibilityAction b) -> AccessibilityAction {
    // 位标志组合结果天然可落在枚举器名单之外（如 Focus|Select = 5），掩码语义刻意构造，非越界错误。
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    return static_cast<AccessibilityAction>(static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b));
}

/// @brief 无障碍节点：运行时控件树的可访问视图。
/// @note Thread: main-thread only
/// @note Side-effects: pure
struct AccessibilityNode {
    AccessibilityRole role = AccessibilityRole::Generic;
    std::string name;  ///< 可读标签（控件自填，如按钮文字）
    std::string value;  ///< 当前值（如文本内容、复选状态）
    std::string hint;  ///< 用途补充提示（控件可选覆写 `accessibility_hint()`）
    Rect bounds;  ///< 屏幕坐标盒（语义树构建时按布局/绘制几何填充）
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
        case AccessibilityRole::Generic:
            return AccessibilityAction::Focus;
        case AccessibilityRole::Button:
            return AccessibilityAction::Click | AccessibilityAction::Invoke | AccessibilityAction::Focus;
        case AccessibilityRole::Text:
            return AccessibilityAction::Focus;
        case AccessibilityRole::TextInput:
            return AccessibilityAction::Value | AccessibilityAction::Focus;
        case AccessibilityRole::Checkbox:
        case AccessibilityRole::Switch:
            return AccessibilityAction::Toggle | AccessibilityAction::Focus;
        case AccessibilityRole::Slider:
            return AccessibilityAction::Value | AccessibilityAction::Focus;
        case AccessibilityRole::Image:
            return AccessibilityAction::None;  // 非交互：图片无可执行动作
        case AccessibilityRole::List:
        case AccessibilityRole::ListItem: {
            return AccessibilityAction::Focus;
        }
        case AccessibilityRole::Header:  // 非交互：标题
        case AccessibilityRole::Progress: {
            return AccessibilityAction::None;
        }  // 非交互：进度指示
        case AccessibilityRole::Dialog:
            return AccessibilityAction::Focus;
        default:
            return AccessibilityAction::None;  // 兜底：未来新增角色默认无动作，不再静默继承 Focus
    }
}

namespace detail {

/// @brief 语义几何盒取值：已绘制优先取绘制遍历写入的绝对盒，否则回退布局累加盒。
///
/// 绘制盒（`Widget::paint_bounds()`，与 `Widget::focus_bounds_` 同源同值）含 Modifier 的
/// padding / border 位移，是屏幕坐标下的真实盒；布局累加盒只累加各级 `Node` 的局部原点，
/// 不带父级 padding 偏移。**优先绘制盒**保证有 present 过的树几何精确；
/// **回退累加盒**保证未绘制（纯布局单测、首帧前查询）时节点仍有非空几何。
/// @note 离屏缓冲（如 `Scroll` 内容）内的后代盒为内容坐标系，与 `paint_bounds()` 同限制。
[[nodiscard]] inline auto accessibility_box(const Widget &w, const Rect &layout_box) -> Rect {
    const Rect painted = w.paint_bounds();
    if (painted.size.width > 0.0F || painted.size.height > 0.0F) {
        return painted;
    }
    return layout_box;
}

/// @brief 递归构建单个节点：几何由 `layout_box` 累加，子节点累加各自 `Node` 局部原点。
/// @param w 当前控件
/// @param layout_box 当前控件按布局累加得到的候选全局盒（未绘制时采用）
[[nodiscard]] inline auto build_accessibility_node(const Widget &w, const Rect &layout_box) -> AccessibilityNode {
    AccessibilityNode node;
    node.role = infer_accessibility_role(w.type_name());
    node.actions = default_actions(node.role);
    node.name = w.accessibility_label();
    node.value = w.accessibility_value();
    node.hint = w.accessibility_hint();
    node.bounds = accessibility_box(w, layout_box);

    // 子节点优先走 `child_nodes()`：其 `Node` 带父写入的局部盒，可累加出真实几何。
    const auto &nodes = w.child_nodes();
    if (!nodes.empty()) {
        for (const Node &child : nodes) {
            // 不可见子节点（`show == false`）与绘制/命中同口径：不入语义树，避免屏幕阅读器
            // 读到视觉上不存在的控件。
            if (!child.widget().show.get()) {
                continue;
            }
            // 累加仍走**布局**原点（而非绘制盒原点）：绘制偏移 Modifier（transform 等）会改变
            // 控件自身的绘制盒，据此累加会把偏移重复计入后代；后代各自再由 accessibility_box
            // 取其精确绘制盒，二者互不干扰。
            const Rect cb = child.bounds();
            const Rect child_box{
                .origin = Point{.x = layout_box.origin.x + cb.origin.x, .y = layout_box.origin.y + cb.origin.y},
                .size = cb.size,
            };
            node.children.push_back(build_accessibility_node(child.widget(), child_box));
        }
        return node;
    }

    // 兜底：虚拟化列表 / 导航栈等容器把子节点存在 `Node` 之外的私有表中，不覆写
    // `child_nodes()`（因而无 Node 局部盒），只经 `for_each_child` 暴露子树。此处与其行为对齐
    // 展开，几何缺失 ⇒ 后代继承本节点盒。（不可把 Node 拷出容器补几何：`Node` 析构会清子节点
    // 的 `layout_parent_`，脏标记传播会断链。）
    w.for_each_child([&node](const Widget &child) -> void {
        if (!child.show.get()) {
            return;
        }
        node.children.push_back(build_accessibility_node(child, node.bounds));
    });
    return node;
}

}  // namespace detail

// ============================================================================
// 无障碍设置（B2）：注入与响应
// ============================================================================

/// @brief 无障碍偏好设置：由宿主经 `Environment` 注入（`env.with<T>()/set<T>()`），
///        或作为无上下文子系统（动画 / 字体度量）的进程级兜底。
///
/// 取值意图对标 WAI / 各 OS 的无障碍开关：动效收敛、高对比、字号缩放、读屏是否在线。
/// 默认全关 / 不缩放——**默认行为与接入前完全一致**（golden 逐位不变）。
/// @note Thread: main-thread only
/// @note Side-effects: none
struct AccessibilitySettings {
    bool reduce_motion = false;  ///< 减弱动态效果：`AnimationController` 不再渐变而是直落端点
    bool high_contrast = false;  ///< 高对比样式请求（色 Responder 由各控件按此位取用）
    float font_scale = 1.0F;  ///< 字号缩放倍率（<= 0 或 NaN 视为 1.0，不缩放）
    bool screen_reader_active = false;  ///< 读屏在线（宿主据 OS 查询结果填写）

    /// @brief 归一化后的字号倍率：非法值（<= 0 / 非有限）回落到 1.0，避免污染度量。
    [[nodiscard]] auto resolved_font_scale() const -> float {
        return (font_scale > 0.0F && font_scale < 1.0e6F) ? font_scale : 1.0F;
    }
};

/// @brief 进程级无障碍设置（默认来源）：无 `Environment` 可用的子系统取此值。
/// @note 有意返回可变引用：宿主在帧首自然更新同一实例；约束同 `Environment::set`。
/// @note Thread: main-thread only
[[nodiscard]] inline auto current_accessibility_settings() -> AccessibilitySettings & {
    static AccessibilitySettings settings;  // NOLINT
    return settings;
}

/// @brief 设置进程级无障碍设置；单例被(destructor)静态对象持有，测试须自行复原。
inline auto set_accessibility_settings(AccessibilitySettings s) -> void {
    current_accessibility_settings() = std::move(s);
}

/// @brief 读取**生效**设置：上下文注入优先，缺失回落进程级默认值。
///
/// 模板化以避开 `core/accessibility.h → environment/*` 的表级依赖：任何提供成员模板
/// `environment<T>() const` 的上下文（现为 `BuildContext`）均可传入；实例化点补全类型。
/// @param ctx 提供 `environment<T>()` 的上下文（如 `BuildContext`）
/// @note Thread: main-thread only
template <typename Ctx>
[[nodiscard]] auto resolved_accessibility_settings(const Ctx &ctx) -> AccessibilitySettings {
    if (const auto *injected = ctx.template environment<AccessibilitySettings>()) {
        return *injected;
    }
    return current_accessibility_settings();
}

// ============================================================================
// 无障碍事件（B2）：焦点 / 值 / 结构变化的上抛通道
// ============================================================================

/// @brief 无障碍事件种类：对标 UIA / NSAccessibility 的通知分区。
enum class AccessibilityEventKind : std::uint8_t {
    FocusChanged,  ///< 焦点转移（获焦 / 失焦均上报，一次聚焦变更一条）
    ValueChanged,  ///< 可取值的控件取值变化（Checkbox / Switch / Slider / TextInput …）
    StructureChanged,  ///< 子树结构变化（子节点增删 / 替换）
};

/// @brief 无障碍事件：携带种类与来源控件（非拥有指针，仅回调期间有效）。
/// @note Thread: main-thread only
struct AccessibilityEvent {
    AccessibilityEventKind kind = AccessibilityEventKind::FocusChanged;
    const Widget *target = nullptr;  ///< 来源控件；可为 nullptr（仅类型已知的场景）
};

/// @brief 无障碍事件处理器签名。
using AccessibilityEventHandler = std::function<void(const AccessibilityEvent &)>;

/// @brief 进程级事件处理器（宿主 / 读屏桥 安装；未安装时上报为廉价空转）。
/// @note Thread: main-thread only
[[nodiscard]] inline auto current_accessibility_event_handler() -> AccessibilityEventHandler & {
    static AccessibilityEventHandler handler;  // NOLINT
    return handler;
}

/// @brief 安装无障碍事件处理器；传空值即卸载。
inline auto set_accessibility_event_handler(AccessibilityEventHandler h) -> void {
    current_accessibility_event_handler() = std::move(h);
}

/// @brief 上报一条无障碍事件（无处理器时为空操作，不改变控件状态）。
inline auto notify_accessibility_event(AccessibilityEvent e) -> void {
    if (const auto &handler = current_accessibility_event_handler()) {
        handler(e);
    }
}

/// @brief 递归构建控件树的无障碍视图（含几何）。
///
/// 几何来源见 `detail::accessibility_box`：已绘制取绘制盒、未绘制取布局累加盒。
/// 根节点未绘制时，其盒由 `root.size()`（布局结果）置于原点导出，故调用前应先完成一次布局
/// （`root.layout(constraints, ctx)`）以获得真实尺寸。
/// @note 不依赖 GUI 后端；name / value / hint 由 `Widget::accessibility_*()` 钩子自填。
[[nodiscard]] inline auto build_accessibility_tree(const Widget &root, const Rect &root_box) -> AccessibilityNode {
    return detail::build_accessibility_node(root, root_box);
}

/// @brief 递归构建控件树的无障碍视图（根节点几何置于原点）。
/// @note Side-effects: reads layout/state
[[nodiscard]] inline auto build_accessibility_tree(const Widget &root) -> AccessibilityNode {
    return detail::build_accessibility_node(root, Rect{.origin = Point{}, .size = root.size()});
}

/// @brief 统计无障碍树节点总数（含根）。
[[nodiscard]] inline auto accessibility_node_count(const AccessibilityNode &n) -> std::size_t {
    std::size_t c = 1;
    for (const auto &ch : n.children) {
        c += accessibility_node_count(ch);
    }
    return c;
}

}  // namespace aurora
