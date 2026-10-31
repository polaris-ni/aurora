// macOS NSAccessibility 桥：语义树快照 → `NSAccessibilityElement` 元素树 + 拉取式同步 +
// 通知广播。与 Win32 UIA / Linux AT-SPI 两座桥同一形态（惰性激活、按值快照、根销毁即切断），
// 差别只在平台查询入口：AppKit 经内容视图的 `accessibilityChildren` / `accessibilityHitTest:`
// 到达，而不是窗口消息 / D-Bus 方法调用。
//
// 线程：全部在 UI 主线程（AppKit 无障碍查询本就只在主线程回调）。本文件不建线程、不投递队列。

#include "aurora/window/detail/macos_ax_bridge.h"

#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "aurora/core/accessibility.h"
#include "aurora/widget/a11y_diff.h"
#include "aurora/widget/widget.h"

// =============================================================================
// 元素类（ObjC 声明须在全局作用域；方法实现见文件末尾）
// =============================================================================

// 单个语义节点的 AppKit 无障碍元素：字段按快照刷新，动作回桥执行。
//
// 所有 ivar 由桥在重建时直接写入（`@public`）；元素按节点 id 复用，读屏持有的旧引用不会
// 因重建而悬垂。`parent` / `bridge` 均为非拥有指针（父元素由本桥持有，桥由宿主持有）。
@interface AuroraAxElement : NSAccessibilityElement {
  @public
    aurora::detail::MacOSAxBridge *bridge;
    std::uint64_t node_id;
    NSString *role;
    NSString *subrole;
    NSString *label;
    NSString *value_string;
    NSString *hint;
    NSString *identifier;
    NSNumber *value_number;
    NSNumber *min_value;
    NSNumber *max_value;
    NSRect frame;
    NSArray *children;
    id parent;
    BOOL enabled;
    BOOL focused;
}
@end

namespace aurora::detail {

namespace {

/// @brief 语义角色 → AppKit 角色常量（`NSAccessibilityRole`）。
/// @param role 语义角色。
/// @param multiline 文本控件是否多行（决定 TextField / TextArea）。
/// @return AppKit 角色常量。
[[nodiscard]] auto ax_role(AccessibilityRole role, bool multiline) -> NSString * {
    switch (role) {
        case AccessibilityRole::Button:
            return NSAccessibilityButtonRole;
        case AccessibilityRole::Text:
            return NSAccessibilityStaticTextRole;
        case AccessibilityRole::TextInput:
            return multiline ? NSAccessibilityTextAreaRole : NSAccessibilityTextFieldRole;
        case AccessibilityRole::Checkbox:
        case AccessibilityRole::Switch:
            return NSAccessibilityCheckBoxRole;
        case AccessibilityRole::Slider:
            return NSAccessibilitySliderRole;
        case AccessibilityRole::Image:
            return NSAccessibilityImageRole;
        case AccessibilityRole::List:
            return NSAccessibilityListRole;
        case AccessibilityRole::ListItem:
            return NSAccessibilityRowRole;
        case AccessibilityRole::Header:
            return NSAccessibilityStaticTextRole;
        case AccessibilityRole::Progress:
            return NSAccessibilityProgressIndicatorRole;
        case AccessibilityRole::Dialog:
        case AccessibilityRole::Generic:
            break;
    }
    return NSAccessibilityGroupRole;
}

/// @brief 子角色：只在有专门语义时返回常量，其余 nil。
/// @param role 语义角色。
/// @return AppKit 子角色常量或 nil。
[[nodiscard]] auto ax_subrole(AccessibilityRole role, const AccessibilityState &state) -> NSString * {
    if (role == AccessibilityRole::Switch) {
        return NSAccessibilitySwitchSubrole;
    }
    if (role == AccessibilityRole::Dialog) {
        return NSAccessibilityDialogSubrole;
    }
    if (role == AccessibilityRole::TextInput && state.password) {
        return NSAccessibilitySecureTextFieldSubrole;
    }
    return nil;
}

/// @brief 由 UTF-8 窄串构造自动释放的 NSString（空串回 nil：缺失与空值在 AX 面同形）。
/// @param text UTF-8 文本。
/// @return NSString 或 nil。
[[nodiscard]] auto ax_string(const std::string &text) -> NSString * {
    if (text.empty()) {
        return nil;
    }
    return [NSString stringWithUTF8String:text.c_str()];
}

/// @brief 节点是否值得作为元素呈现：有名字、有交互动作或本身是控件。
/// @param node 语义节点。
/// @return 需呈现为元素时为 true。
[[nodiscard]] auto ax_presentable(const AccessibilityNode &node) -> bool {
    return !node.name.empty() || node.has_interactive_action() || node.actions != AccessibilityAction::None ||
           node.role != AccessibilityRole::Generic;
}

}  // namespace

// =============================================================================
// Impl：快照 / 元素表 / 坐标换算 / 通知
// =============================================================================

struct MacOSAxBridge::Impl {
    NSView *view = nil;  ///< 内容视图（非拥有；几何换算锚点）。
    MacOSAxBridge *self = nullptr;  ///< 回指（元素回调需要桥的公共方法）。
    Widget *root = nullptr;  ///< 语义树根（非拥有；由宿主每帧注入）。
    bool active = false;  ///< 是否已激活（已注册广播、可答平台查询）。
    bool dirty = true;  ///< 是否需要下一次查询时重建。
    a11y::TreeSnapshot snapshot;  ///< 最近一次投影的扁平快照。
    std::unordered_map<std::uint64_t, AuroraAxElement *> elements;  ///< 节点 id → 元素（拥有）。
    NSArray *children = nil;  ///< 内容视图的子元素数组（拥有；元素表同时拥有元素）。
    std::uint64_t focused_id = 0;  ///< 当前焦点节点 id（0 = 无）。

    /// @brief 内容视图坐标（逻辑 dp，y 向下）→ 屏幕坐标（点，y 向上）。
    /// @param box 窗口本地逻辑盒。
    /// @return 屏幕矩形；无窗口时为零矩形。
    [[nodiscard]] auto screen_rect(const Rect &box) const -> NSRect {
        const CGFloat w = std::max<CGFloat>(0.0, static_cast<CGFloat>(box.size.width));
        const CGFloat h = std::max<CGFloat>(0.0, static_cast<CGFloat>(box.size.height));
        const NSRect logical = NSMakeRect(static_cast<CGFloat>(box.origin.x), static_cast<CGFloat>(box.origin.y), w, h);
        const NSRect in_window = [view convertRect:logical toView:nil];
        NSWindow *const window = [view window];
        return (window != nil) ? [window convertRectToScreen:in_window] : NSZeroRect;
    }

    /// @brief 释放全部元素与顶层数组（去激活 / 根销毁 / 重建清空共用）。
    auto clear_elements() -> void {
        for (auto &entry : elements) {
            [entry.second release];
        }
        elements.clear();
        [children release];
        children = nil;
        focused_id = 0;
    }

    /// @brief 按节点 id 取活控件（快照内裸指针；调用期间有效）。
    /// @param id 节点 id。
    /// @return 控件指针；快照未命中为 nullptr。
    [[nodiscard]] auto widget_for(std::uint64_t id) -> Widget * {
        const a11y::NodeSnapshot *const node = snapshot.find(id);
        return (node != nullptr) ? const_cast<Widget *>(node->widget) : nullptr;  // NOLINT(*-const-cast)
    }

    /// @brief 递归建 / 复用元素，并返回该节点的子元素数组。
    /// @param node 当前节点。
    /// @param live 本次重建已访问的 id 集（未被访问者随后释放）。
    /// @return 元素（拥有权在 `elements`）。
    auto element_for(const a11y::NodeSnapshot &node, std::unordered_set<std::uint64_t> &live) -> AuroraAxElement * {
        live.insert(node.id);
        AuroraAxElement *element = nil;
        const auto found = elements.find(node.id);
        if (found != elements.end()) {
            element = found->second;
        } else {
            element = [[AuroraAxElement alloc] init];
            element->bridge = self;
            element->node_id = node.id;
            elements.emplace(node.id, element);
        }

        // 子元素先建（后序），再一次性替换本元素的 children 数组。
        NSMutableArray *kids = [[NSMutableArray alloc] init];
        const auto child_it = snapshot.children_of.find(node.id);
        if (child_it != snapshot.children_of.end()) {
            for (const std::uint64_t child_id : child_it->second) {
                const a11y::NodeSnapshot *const child = snapshot.find(child_id);
                if (child == nullptr) {
                    continue;
                }
                AuroraAxElement *child_element = element_for(*child, live);
                [kids addObject:child_element];
            }
        }
        NSArray *const kids_copy = [[NSArray alloc] initWithArray:kids];
        [kids release];

        const AccessibilityNode &n = node.node;
        [element->role release];
        element->role = [ax_role(n.role, n.state.multiline) retain];
        [element->subrole release];
        element->subrole = [ax_subrole(n.role, n.state) retain];
        [element->label release];
        element->label = [ax_string(n.name) retain];
        [element->hint release];
        element->hint = [ax_string(n.hint) retain];
        [element->identifier release];
        element->identifier = [ax_string(n.stable_key) retain];
        [element->value_string release];
        element->value_string = nil;
        [element->value_number release];
        element->value_number = nil;
        [element->min_value release];
        element->min_value = nil;
        [element->max_value release];
        element->max_value = nil;

        if (n.role == AccessibilityRole::Checkbox || n.role == AccessibilityRole::Switch) {
            element->value_number = [@(static_cast<BOOL>(n.state.checked)) retain];
        } else if (n.range.has_value()) {
            element->value_number = [@(n.range->value) retain];
            element->min_value = [@(n.range->min) retain];
            element->max_value = [@(n.range->max) retain];
        } else if (!n.state.password) {
            element->value_string = [ax_string(n.value) retain];
        }
        element->frame = screen_rect(n.bounds);
        element->enabled = n.state.disabled ? NO : YES;
        element->focused = n.state.focused ? YES : NO;
        [element->children release];
        element->children = kids_copy;
        return element;
    }

    /// @brief 重建快照与元素树，并按 diff 发平台通知。
    auto rebuild() -> void {
        dirty = false;
        if (root == nullptr || view == nil) {
            clear_elements();
            snapshot = a11y::TreeSnapshot{};
            return;
        }
        a11y::TreeSnapshot fresh = a11y::build_tree_snapshot(*root);
        const a11y::TreeDiff diff = a11y::diff_snapshots(snapshot, fresh);
        snapshot = std::move(fresh);

        std::unordered_set<std::uint64_t> live;
        NSMutableArray *tops = [[NSMutableArray alloc] init];
        const a11y::NodeSnapshot *const root_node = snapshot.flat.empty() ? nullptr : &snapshot.flat.front();
        if (root_node != nullptr) {
            const bool root_presentable =
                ax_presentable(root_node->node) || root_node->node.has_action(AccessibilityAction::Focus);
            if (root_presentable) {
                [tops addObject:element_for(*root_node, live)];
            } else {
                const auto child_it = snapshot.children_of.find(root_node->id);
                if (child_it != snapshot.children_of.end()) {
                    for (const std::uint64_t child_id : child_it->second) {
                        const a11y::NodeSnapshot *const child = snapshot.find(child_id);
                        if (child != nullptr) {
                            [tops addObject:element_for(*child, live)];
                        }
                    }
                }
            }
        }
        NSArray *const tops_copy = [[NSArray alloc] initWithArray:tops];
        [tops release];
        // ObjC 快速枚举的循环变量由 for 语句赋值，clang-tidy 的初始化检查看不到这一点。
        // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
        for (AuroraAxElement *const element : tops_copy) {
            element->parent = view;
        }
        [children release];
        children = tops_copy;

        // 未被访问的元素：节点已离开语义树（或整树换根），释放并从表里摘除。
        std::vector<std::uint64_t> stale;
        for (const auto &entry : elements) {
            if (!live.contains(entry.first)) {
                stale.push_back(entry.first);
            }
        }
        for (const std::uint64_t id : stale) {
            const auto it = elements.find(id);
            if (it != elements.end()) {
                [it->second release];
                elements.erase(it);
            }
        }
        // 子元素的 parent 在递归建树后统一回填（父子数组已定，父元素必在表中）。
        for (const auto &entry : elements) {
            AuroraAxElement *const element = entry.second;
            if (element->parent == view) {
                continue;  // 顶层元素已指向内容视图
            }
            const a11y::NodeSnapshot *const node = snapshot.find(entry.first);
            if (node == nullptr || node->parent_id == 0) {
                element->parent = view;
                continue;
            }
            const auto parent_it = elements.find(node->parent_id);
            element->parent = (parent_it != elements.end()) ? parent_it->second : view;
        }

        // 焦点按快照实值取，不沿用旧 id：焦点清空（无新焦点）时那一位也必须归零。
        focused_id = 0;
        for (const auto &entry : elements) {
            const a11y::NodeSnapshot *const node = snapshot.find(entry.first);
            if (node != nullptr && node->node.state.focused) {
                focused_id = entry.first;
                break;
            }
        }
        emit_notifications(diff);
    }

    /// @brief 按 diff 发 AppKit 通知（焦点优先，值 / 标题按元素，布局合并一条）。
    /// @param diff 本次重建的差异。
    auto emit_notifications(const a11y::TreeDiff &diff) const -> void {
        if (diff.focused_id.has_value()) {
            const auto it = elements.find(*diff.focused_id);
            if (it != elements.end()) {
                NSAccessibilityPostNotification(it->second, NSAccessibilityFocusedUIElementChangedNotification);
            }
        }
        bool layout_changed = !diff.added.empty() || !diff.removed.empty() || !diff.moved.empty();
        for (const auto &[id, field] : diff.updated) {
            const auto it = elements.find(id);
            if (it == elements.end()) {
                continue;
            }
            switch (field) {
                case a11y::FieldChange::Name:
                    NSAccessibilityPostNotification(it->second, NSAccessibilityTitleChangedNotification);
                    break;
                case a11y::FieldChange::Value:
                case a11y::FieldChange::Range:
                    NSAccessibilityPostNotification(it->second, NSAccessibilityValueChangedNotification);
                    break;
                case a11y::FieldChange::Hint:
                case a11y::FieldChange::Bounds:
                case a11y::FieldChange::State:
                case a11y::FieldChange::Actions:
                    layout_changed = true;
                    break;
            }
        }
        if (layout_changed && view != nil) {
            NSAccessibilityPostNotification(view, NSAccessibilityLayoutChangedNotification);
        }
    }
};

// =============================================================================
// MacOSAxBridge
// =============================================================================

MacOSAxBridge::MacOSAxBridge(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MacOSAxBridge::~MacOSAxBridge() {
    if (impl_ != nullptr) {
        deactivate();
    }
}

auto MacOSAxBridge::create(void *view) -> std::unique_ptr<MacOSAxBridge> {
    if (view == nullptr) {
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->view = (NSView *)view;  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast): 桥边界以 void* 承载 ObjC 指针
    auto bridge = std::unique_ptr<MacOSAxBridge>(new MacOSAxBridge(std::move(impl)));
    bridge->impl_->self = bridge.get();
    return bridge;
}

auto MacOSAxBridge::activate() -> void {
    if (impl_ == nullptr || impl_->active) {
        return;
    }
    impl_->active = true;
    impl_->dirty = true;
    a11y::register_provider(*this);
    // 激活即回填 `screen_reader_active`（heuristic 语义同 UIA 桥）：保留其余字段。
    AccessibilitySettings settings = current_accessibility_settings();
    settings.screen_reader_active = true;
    set_accessibility_settings(settings);
}

auto MacOSAxBridge::deactivate() -> void {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->active) {
        impl_->active = false;
        // 必须先注销：否则进程级广播会在已析构的桥上调用 `is_active()`（与 UIA 桥同一条不变量）。
        a11y::unregister_provider(*this);
        AccessibilitySettings settings = current_accessibility_settings();
        settings.screen_reader_active = false;
        set_accessibility_settings(settings);
    }
    impl_->clear_elements();
    impl_->snapshot = a11y::TreeSnapshot{};
    impl_->root = nullptr;
}

auto MacOSAxBridge::sync_if_dirty() -> void {
    if (impl_ != nullptr && impl_->active && impl_->dirty) {
        impl_->rebuild();
    }
}

auto MacOSAxBridge::mark_dirty() -> void {
    if (impl_ != nullptr) {
        impl_->dirty = true;
    }
}

auto MacOSAxBridge::is_active() const -> bool { return impl_ != nullptr && impl_->active; }

auto MacOSAxBridge::name() const -> std::string { return "macos-ax"; }

auto MacOSAxBridge::set_root(Widget *root) -> void {
    if (impl_ == nullptr) {
        return;
    }
    // 根身份不变也要置脏：控件树的布局每帧可动，帧坐标必须在下一次查询时刷新
    // （与 UIA 桥「仅换根置脏」不同——UIA 的 BoundingRectangle 由平台单独取，这里帧是元素属性）。
    impl_->root = root;
    impl_->dirty = true;
}

auto MacOSAxBridge::on_announcement(const std::string &text, const Widget *target) -> void {
    (void)target;
    if (text.empty()) {
        return;
    }
    NSString *const announcement = ax_string(text);
    if (announcement == nil) {
        return;
    }
    NSDictionary *const info = @{
        NSAccessibilityAnnouncementKey : announcement,
        NSAccessibilityPriorityKey : [NSNumber numberWithInteger:NSAccessibilityPriorityMedium],
    };
    NSAccessibilityPostNotificationWithUserInfo(NSApp, NSAccessibilityAnnouncementRequestedNotification, info);
}

auto MacOSAxBridge::on_widget_destroying(const Widget *w) -> void {
    if (impl_ == nullptr || w == nullptr || w != impl_->root) {
        return;
    }
    // 根销毁即切断：窗口存活期间的后续查询不得拿悬垂根重建语义树（与 UIA 桥同一处置）。
    impl_->root = nullptr;
    impl_->dirty = false;
    impl_->clear_elements();
    impl_->snapshot = a11y::TreeSnapshot{};
}

auto MacOSAxBridge::children() -> void * {
    if (impl_ == nullptr) {
        return @[];
    }
    sync_if_dirty();
    return (impl_->children != nil) ? impl_->children : @[];
}

auto MacOSAxBridge::hit_test(double x, double y) -> void * {
    if (impl_ == nullptr || impl_->view == nil) {
        return nullptr;
    }
    sync_if_dirty();
    NSWindow *const window = [impl_->view window];
    if (window == nil) {
        return impl_->view;
    }
    const NSRect in_window = [window convertRectFromScreen:NSMakeRect(x, y, 0.0, 0.0)];
    const NSPoint local = [impl_->view convertPoint:in_window.origin fromView:nil];
    // 先序表反向遍历：后出现的后代优先命中，得到最深元素。
    for (auto it = impl_->snapshot.flat.rbegin(); it != impl_->snapshot.flat.rend(); ++it) {
        if (!impl_->elements.contains(it->id)) {
            continue;
        }
        const Rect &b = it->node.bounds;
        if (static_cast<double>(local.x) >= b.origin.x && static_cast<double>(local.x) <= b.origin.x + b.size.width &&
            static_cast<double>(local.y) >= b.origin.y && static_cast<double>(local.y) <= b.origin.y + b.size.height) {
            return impl_->elements.at(it->id);
        }
    }
    return impl_->view;
}

auto MacOSAxBridge::focused_element() -> void * {
    if (impl_ == nullptr) {
        return nullptr;
    }
    sync_if_dirty();
    const auto it = impl_->elements.find(impl_->focused_id);
    return (it != impl_->elements.end()) ? it->second : nullptr;
}

auto MacOSAxBridge::perform_press(std::uint64_t id) -> bool {
    if (impl_ == nullptr) {
        return false;
    }
    sync_if_dirty();
    const a11y::NodeSnapshot *const node = impl_->snapshot.find(id);
    Widget *const widget = impl_->widget_for(id);
    if (node == nullptr || widget == nullptr) {
        return false;
    }
    AccessibilityAction action = AccessibilityAction::Invoke;
    if (!node->node.has_action(action)) {
        action = AccessibilityAction::Click;
        if (!node->node.has_action(action)) {
            action = AccessibilityAction::Toggle;
            if (!node->node.has_action(action)) {
                return false;
            }
        }
    }
    const AccessibilityActionRequest request{.action = action, .number = 0.0, .text = {}};
    return widget->perform_accessibility_action(request);
}

auto MacOSAxBridge::perform_step(std::uint64_t id, int step) -> bool {
    if (impl_ == nullptr) {
        return false;
    }
    sync_if_dirty();
    const a11y::NodeSnapshot *const node = impl_->snapshot.find(id);
    Widget *const widget = impl_->widget_for(id);
    if (node == nullptr || widget == nullptr || !node->node.range.has_value()) {
        return false;
    }
    const AccessibilityRange &range = *node->node.range;
    const double stride = range.step > 0.0 ? range.step : 1.0;
    double next = range.value + (static_cast<double>(step) * stride);
    next = std::clamp(next, range.min, range.max);
    const AccessibilityActionRequest request{.action = AccessibilityAction::Value, .number = next, .text = {}};
    return widget->perform_accessibility_action(request);
}

auto MacOSAxBridge::set_focused(std::uint64_t id, bool on) -> void {
    if (impl_ == nullptr || !on) {
        return;
    }
    sync_if_dirty();
    Widget *const widget = impl_->widget_for(id);
    if (widget == nullptr) {
        return;
    }
    const AccessibilityActionRequest request{.action = AccessibilityAction::Focus, .number = 0.0, .text = {}};
    (void)widget->perform_accessibility_action(request);
}

}  // namespace aurora::detail

// =============================================================================
// AuroraAxElement：字段读取 + 动作转发
// =============================================================================

@implementation AuroraAxElement

- (void)dealloc {
    [role release];
    [subrole release];
    [label release];
    [value_string release];
    [hint release];
    [identifier release];
    [value_number release];
    [min_value release];
    [max_value release];
    [children release];
    [super dealloc];
}

- (BOOL)isAccessibilityElement {
    return YES;
}

- (NSString *)accessibilityRole {
    return role;
}

- (NSString *)accessibilitySubrole {
    return subrole;
}

- (NSString *)accessibilityLabel {
    return label;
}

- (id)accessibilityValue {
    return (value_number != nil) ? value_number : value_string;
}

- (NSString *)accessibilityHelp {
    return hint;
}

- (NSString *)accessibilityIdentifier {
    return identifier;
}

- (id)accessibilityParent {
    return parent;
}

- (NSArray *)accessibilityChildren {
    return children;
}

- (NSRect)accessibilityFrame {
    return frame;
}

- (BOOL)isAccessibilityEnabled {
    return enabled;
}

- (BOOL)isAccessibilityFocused {
    return focused;
}

- (void)setAccessibilityFocused:(BOOL)focused {
    if (bridge != nullptr) {
        bridge->set_focused(node_id, focused != NO);
    }
}

- (NSNumber *)accessibilityMinValue {
    return min_value;
}

- (NSNumber *)accessibilityMaxValue {
    return max_value;
}

- (BOOL)accessibilityPerformPress {
    return (bridge != nullptr && bridge->perform_press(node_id)) ? YES : NO;
}

- (BOOL)accessibilityPerformIncrement {
    return (bridge != nullptr && bridge->perform_step(node_id, 1)) ? YES : NO;
}

- (BOOL)accessibilityPerformDecrement {
    return (bridge != nullptr && bridge->perform_step(node_id, -1)) ? YES : NO;
}

@end

#endif  // AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS
