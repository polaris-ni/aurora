#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/a11y_types.h"
#include "aurora/core/types.h"

namespace aurora {

/// @brief 控件基类（定义于 `widget/widget.h`）。
///
/// 本头只以**指针**持有 `Widget`（事件来源 `AccessibilityEvent::target`、销毁钩子），不触碰其
/// 任何成员，故仅需前置声明。需要 `Widget` 完整定义构建语义树的入口（`build_accessibility_tree`
/// 及各 Name 回退链求值）单列于 `widget/a11y_tree.h`——那是 `core/` 与 `widget/` 的依赖方向
/// 守卫：`core/`（基础层）不依赖 `widget/`（组件层），见 `ARCHITECTURE.md` §2。
class Widget;

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
    ScrollUp = 1U << 6U,  ///< 向上滚动（G32；对应 Flutter scrollUp）
    ScrollDown = 1U << 7U,  ///< 向下滚动（G32）
    ScrollLeft = 1U << 8U,  ///< 向左滚动（G32）
    ScrollRight = 1U << 9U,  ///< 向右滚动（G32）
    ScrollIntoView = 1U << 10U,  ///< 请求把本控件滚入视口（G32；UIA IScrollItemProvider）
};

/// @brief 读屏反向动作请求：`Widget::perform_accessibility_action` 的入参。
///
/// `text` 为 `std::string_view`：桥（如 UIA `IValueProvider::SetValue` 传入 UTF-16 BSTR）
/// 在调用前须把文本转 UTF-8 并以 `std::string` 持有，调用返回前不得析构（G15）。
/// @note Thread: main-thread only
struct AccessibilityActionRequest {
    AccessibilityAction action = AccessibilityAction::None;
    double number = 0.0;  ///< Value 动作的数值（Slider 设值）
    std::string_view text;  ///< Value 动作的文本（文本替换）
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
    std::string name;  ///< 可读标签（按 §4 Name 回退链求值；声明了 `labelled_by` 时为目标节点的名字，优先级最高）
    std::string value;  ///< 当前值（如文本内容、复选状态）
    std::string hint;  ///< 用途补充提示（控件可选覆写 `accessibility_hint()`）
    /// 几何盒：**窗口本地 DIP**（原点为窗口客户区左上；与 `Widget::paint_bounds()` 同语义）。
    /// 平台桥统一换算到屏幕物理像素（D12：`物理 = bounds × scale_factor + position`）。
    Rect bounds;
    AccessibilityAction actions = AccessibilityAction::None;
    std::vector<AccessibilityNode> children;

    // ---- 切片 1 新增：身份 / 状态 / 取值域 / 层级 / 树裁剪（追加在末尾，兼容既有聚合初始化）----
    std::uint64_t id = 0;  ///< 稳定身份（`Widget::runtime_id()`；0 = 无身份）
    AccessibilityState state;  ///< 状态位集（含 visible/focusable/offscreen 派生位）
    std::optional<AccessibilityRange> range;  ///< 取值域（Slider / ProgressIndicator）
    std::optional<int> level;  ///< 标题层级（OQ4；`accessibility_level()`）
    bool is_control = true;  ///< 是否进控制视图（UIA IsControlElement / macOS isAccessibilityElement）
    bool is_content = true;  ///< 是否进内容视图（UIA IsContentElement）
    // ---- 引用式标签关联（对标 `aria-labelledby`；追加在末尾，既有聚合初始化零变化）----
    std::string stable_key;  ///< 宿主经 `Widget::set_stable_key` 声明的跨重建稳定键（空 = 未设）
    std::string labelled_by;  ///< 本节点名字来源的**键**（`Widget::set_labelled_by`；空 = 未声明）
    std::uint64_t labelled_by_id = 0;  ///< 解析后的目标 `id`（0 = 未声明 / 未命中 / 目标无名 / 环上）；桥据此投影关系

    [[nodiscard]] auto has_action(AccessibilityAction a) const -> bool {
        return (static_cast<std::uint16_t>(actions) & static_cast<std::uint16_t>(a)) != 0;
    }

    /// @brief 是否有「交互类」动作（Click/Invoke/Toggle/Value/Select）：内容视图判定的输入之一。
    [[nodiscard]] auto has_interactive_action() const -> bool {
        return has_action(AccessibilityAction::Click) || has_action(AccessibilityAction::Invoke) ||
               has_action(AccessibilityAction::Toggle) || has_action(AccessibilityAction::Value) ||
               has_action(AccessibilityAction::Select);
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

/// @note 需要 `Widget` 完整定义的求值函数（语义几何盒、Name 回退链）见 `widget/a11y_tree.h`。

/// @brief 树裁剪 / 语义标记（G23，对标 Chromium `IsIgnored`、Flutter `excludeSemantics`）。
///
/// 判定落在**共享层**（三桥共用，避免各桥语义漂移，见设计 §8.4-1）：
/// - 控件显式声明「不参与语义树」（`accessibility_is_semantic() == false`）→ 非控制、非内容；
/// - 无 label 且非交互的 `Image`（装饰图）→ 非控制（UIA 完全忽略该元素）；
/// - 其余一律进控制视图；纯布局容器（`Generic` + 无名 + 无交互动作）不进内容视图，
///   只保留结构父职，避免读屏不停念「pane」。
/// @param n 已填 name/actions 的节点（尚未填 is_control/is_content）
/// @param semantic 控件的 `accessibility_is_semantic()` 取值
inline auto apply_semantic_pruning(AccessibilityNode &n, bool semantic) -> void {
    n.is_control = semantic;
    if (!semantic) {
        n.is_content = false;
        return;
    }
    if (n.role == AccessibilityRole::Image && n.name.empty() && !n.has_interactive_action()) {
        n.is_control = false;  // 装饰图：完全忽略
        n.is_content = false;
        return;
    }
    if (n.role == AccessibilityRole::Generic && n.name.empty() && !n.has_interactive_action()) {
        n.is_content = false;  // 纯布局容器：保留结构，不进内容视图
        return;
    }
    n.is_content = true;
}

}  // namespace detail

// ============================================================================
// 无障碍设置：注入与响应
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
    /// 读屏是否在线：**由平台桥在激活时回填**（heuristic——以「读屏主动取根对象」近似「有读屏
    /// 在线」；读屏退出无反向信号，故仅窗口销毁 / 桥去激活时复位。见设计 R9 / §5.1）。
    bool screen_reader_active = false;

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
inline auto set_accessibility_settings(AccessibilitySettings s) -> void { current_accessibility_settings() = s; }

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
// 无障碍事件：焦点 / 值 / 结构变化的上抛通道
// ============================================================================

/// @brief 无障碍事件种类：对标 UIA / NSAccessibility 的通知分区。
enum class AccessibilityEventKind : std::uint8_t {
    FocusChanged,  ///< 焦点转移（获焦 / 失焦均上报，一次聚焦变更一条）
    ValueChanged,  ///< 可取值的控件取值变化（Checkbox / Switch / Slider / TextInput …）
    NameChanged,  ///< 可访问名变化（`Widget::set_accessibility_label`）：读屏应重念该控件
    StructureChanged,  ///< 子树结构变化（子节点增删 / 替换）
    Announcement,  ///< 动态播报 / Live Region：请立即朗读 `announcement_text`（G4）
};

/// @brief 无障碍事件：携带种类与来源控件（非拥有指针，仅回调期间有效）。
/// @note Thread: main-thread only
struct AccessibilityEvent {
    AccessibilityEventKind kind = AccessibilityEventKind::FocusChanged;
    const Widget *target = nullptr;  ///< 来源控件；可为 nullptr（仅类型已知的场景）
    /// 播报文本（仅 `Announcement` 使用）：不经语义树 diff，由桥直译平台「立即朗读」信号。
    /// 不要求 `target` 在语义树内有对应节点（toast / 临时浮层的常见形态）。
    std::string announcement_text;
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

namespace detail {

/// @brief 无障碍事件**广播钩子**（平台桥注册表安装；与宿主单槽处理器并列，互不覆盖）。
///
/// 为何不复用单槽：宿主可能在桥激活**之后**才 `set_accessibility_event_handler`，若桥以
/// 「保存旧处理器 + 链式包裹」的方式挂载，先保存的处理器会失效、链式断裂（G12）。
/// 独立钩子使二者顺序无关——宿主处理器永远被调用，桥广播独立生效，公共契约不变。
using AccessibilityBroadcastHook = std::function<void(const AccessibilityEvent &)>;

[[nodiscard]] inline auto a11y_broadcast_hook() -> AccessibilityBroadcastHook & {
    static AccessibilityBroadcastHook hook;  // NOLINT
    return hook;
}

/// @brief 安装/卸载广播钩子（由 `a11y::ProviderRegistry` 调用；宿主不应直接使用）。
inline auto set_a11y_broadcast_hook(AccessibilityBroadcastHook h) -> void { a11y_broadcast_hook() = std::move(h); }

/// @brief 「控件实例即将销毁」的通知钩子（与广播钩子**并列**的第二条独立通道）。
///
/// 为何需要独立通道：桥按设计只持**裸根指针 + 节点 id**，快照可随时重投影，故子节点生死
/// 无需桥感知；但**根控件**一旦销毁，桥缓存的 `root_` 即悬垂——若宿主先拆 UI 树、后拆窗口
/// （常规顺序），窗口仍活着期间任何平台查询都会拿悬垂根去重建语义树（实机 SIGSEGV）。
/// 语义树事件（`AccessibilityEvent`）只能给出**宿主容器**（G3 要求上报父级、不得上报正在
/// 析构的控件），无法用于「是不是我的根没了」这一判定，故单列此通道传递正在销毁的控件指针。
using AccessibilityWidgetDestroyHook = std::function<void(const Widget *)>;

[[nodiscard]] inline auto a11y_widget_destroy_hook() -> AccessibilityWidgetDestroyHook & {
    static AccessibilityWidgetDestroyHook hook;  // NOLINT
    return hook;
}

/// @brief 安装/卸载控件销毁钩子（由 `a11y::ProviderRegistry` 调用；宿主不应直接使用）。
inline auto set_a11y_widget_destroy_hook(AccessibilityWidgetDestroyHook h) -> void {
    a11y_widget_destroy_hook() = std::move(h);
}

}  // namespace detail

/// @brief 上报一条无障碍事件（无处理器时为空操作，不改变控件状态）。
inline auto notify_accessibility_event(const AccessibilityEvent &e) -> void {
    if (const auto &handler = current_accessibility_event_handler()) {
        handler(e);
    }
    if (const auto &hook = detail::a11y_broadcast_hook()) {
        hook(e);
    }
}

/// @brief 上报「某个控件实例即将销毁」（在 `Widget` 释放**之前**调用，指针仍有效）。
///
/// 调用点唯一：`Node::~Node()` 中确认「真销毁控件实例」的分支（与结构事件同源，G3）。
/// 桥据此判定自己缓存的根是否已亡并立即切断投影，避免解引用悬垂指针。
/// @param w 正在销毁的控件（调用返回后即失效；接收方不得保存）
/// @note Thread: main-thread only
inline auto notify_accessibility_widget_destroying(const Widget *w) -> void {
    if (w == nullptr) {
        return;
    }
    if (const auto &hook = detail::a11y_widget_destroy_hook()) {
        hook(w);
    }
}

/// @brief 动态播报：「请现在朗读这段文本」（Live Region / Announcement，G4）。
///
/// 走独立事件通道（`AccessibilityEventKind::Announcement`），**不经**语义树 diff——
/// toast / 状态提示 / 异步结果这类临时文本没有焦点或取值变化，只有此通道能被读屏感知。
/// 各桥映射：UIA `UiaRaiseNotificationEvent`（回退 `UIA_LiveRegionChangedEventId`）、
/// macOS `NSAccessibilityAnnouncementRequestedNotification`、AT-SPI2 `object:announcement`、
/// Wasm ARIA `aria-live` 镜像。
/// @param text 待朗读文本（UTF-8；空串不上报）
/// @param target 关联控件（可为 nullptr）
/// @note Thread: main-thread only
/// @note Side-effects: invokes accessibility event handler
inline auto announce_accessibility(const std::string &text, const Widget *target = nullptr) -> void {
    if (text.empty()) {
        return;
    }
    notify_accessibility_event(
        AccessibilityEvent{.kind = AccessibilityEventKind::Announcement, .target = target, .announcement_text = text});
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
