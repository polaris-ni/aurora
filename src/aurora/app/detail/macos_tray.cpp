/// @file macos_tray.cpp
/// macOS 系统托盘（`NSStatusItem`）的 AppKit 实现。
///
/// 门控：仅在 `AURORA_PLATFORM_MACOS` 下编译（CMake 只把本文件登记进 macOS 后端）。
/// 语言：ObjC++、手动引用计数（与库其余平台层一致，不启用 ARC）。
///
/// 菜单回调路由：`NSMenuItem.tag` 存扁平索引（含子菜单的叶子项按构建序编号），
/// `AuroraTrayTarget` 收到 action 后按下标取 `std::function`，避免在 ObjC 对象里直接持有
/// C++ 回调容器（菜单重建时整体替换，索引同步重排）。

#include "aurora/app/detail/macos_tray.h"

#ifdef AURORA_PLATFORM_MACOS

#import <AppKit/AppKit.h>

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/app/notification.h"
#include "aurora/core/log.h"

namespace aurora::detail {
namespace {

/// @brief 状态项及其 AppKit / C++ 附属对象（由 `MacOSTrayBridge` 独占）。
struct TrayState {
    NSStatusItem *item = nil;  ///< 状态栏项（本类持有一个引用）
    NSImage *image = nil;  ///< 当前图像（本类持有一个引用）
    id target = nil;  ///< `AuroraTrayTarget`（菜单/按钮 action 的接收者）
    NSMenu *menu = nil;  ///< 当前菜单（本类持有一个引用）
    std::vector<std::function<void()>> actions;  ///< 叶子菜单项回调（按 tag 索引）
    std::function<void()> activate;  ///< 无菜单时点击状态按钮的回调
    std::size_t menu_items = 0;  ///< 当前菜单条目总数（含分隔符与子菜单项）
    bool visible = false;  ///< 库内可见性（与 `NSStatusItem.visible` 同步）
};

auto ns_string(const std::string &text) -> NSString * { return [NSString stringWithUTF8String:text.c_str()]; }

auto to_std(NSString *text) -> std::string {
    const char *utf8 = text != nil ? [text UTF8String] : nullptr;
    return utf8 != nullptr ? std::string{utf8} : std::string{};
}

/// @brief 点击状态按钮：转给桥的激活回调。
/// @param owner `TrayState *`（ObjC target 只持有不透明指针）。
void fire_activate(void *owner) {
    auto *state = static_cast<TrayState *>(owner);
    if ((state != nullptr) && state->activate) {
        state->activate();
    }
}

/// @brief 点击菜单项：按扁平索引取回调并执行。
/// @param owner `TrayState *`。
/// @param index 叶子项索引（`NSMenuItem.tag`）。
void fire_menu(void *owner, long index) {
    auto *state = static_cast<TrayState *>(owner);
    if (state == nullptr || index < 0) {
        return;
    }
    const auto slot = static_cast<std::size_t>(index);
    if (slot >= state->actions.size()) {
        return;
    }
    std::function<void()> action = state->actions[slot];  // 拷贝：回调可能重建菜单容器
    if (action) {
        action();
    }
}

/// @brief 存活桥登记表（验收探针取证用；主线程独占，无锁）。
/// @return 进程内桥列表的引用。
auto live_registry() -> std::vector<MacOSTrayBridge *> & {
    static std::vector<MacOSTrayBridge *> bridges;
    return bridges;
}

}  // namespace
}  // namespace aurora::detail

namespace aurora::detail {

auto live_tray_bridges() -> std::vector<MacOSTrayBridge *> { return live_registry(); }

}  // namespace aurora::detail

// 状态按钮 / 菜单项的 ObjC 接收者（target-action）。
@interface AuroraTrayTarget : NSObject
// 桥状态（`aurora::detail::TrayState *`，不持有）。
@property(nonatomic, assign) void *owner;
@end

@implementation AuroraTrayTarget
// 无菜单时点击状态按钮；sender（状态按钮）未使用。
- (void)activate:(id)sender {
    (void)sender;
    aurora::detail::fire_activate(self.owner);
}

// 点击菜单叶子项；sender 的 `tag` 是扁平索引。
- (void)menuItemClicked:(NSMenuItem *)sender {
    aurora::detail::fire_menu(self.owner, static_cast<long>(sender.tag));
}
@end

namespace aurora::detail {

MacOSTrayBridge::MacOSTrayBridge(std::string title, const std::string &icon_path) {
    @autoreleasepool {
        auto *state = new TrayState();
        state->item = [[[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength] retain];
        state->target = [[AuroraTrayTarget alloc] init];
        static_cast<AuroraTrayTarget *>(state->target).owner = state;
        if (state->item != nil) {
            state->item.button.target = state->target;
            state->item.button.action = @selector(activate:);
        }
        state_ = state;
        live_registry().push_back(this);
        set_title(title);
        set_icon(icon_path);
        show();
    }
}

MacOSTrayBridge::~MacOSTrayBridge() {
    auto *state = static_cast<TrayState *>(state_);
    if (state == nullptr) {
        return;
    }
    std::erase(live_registry(), this);
    @autoreleasepool {
        if (state->item != nil) {
            [state->item setMenu:nil];
            [[NSStatusBar systemStatusBar] removeStatusItem:state->item];
            [state->item release];
            state->item = nil;
        }
        [state->menu release];
        [state->target release];
        [state->image release];
    }
    delete state;
    state_ = nullptr;
}

void MacOSTrayBridge::set_title(const std::string &title) {
    auto *state = static_cast<TrayState *>(state_);
    if (state == nullptr) {
        return;
    }
    @autoreleasepool {
        if (state->item != nil) {
            state->item.button.toolTip = ns_string(title);
        }
    }
}

void MacOSTrayBridge::set_icon(const std::string &path) {
    auto *state = static_cast<TrayState *>(state_);
    if (state == nullptr) {
        return;
    }
    @autoreleasepool {
        NSImage *image = nil;
        if (!path.empty()) {
            image = [[NSImage alloc] initWithContentsOfFile:ns_string(path)];
        }
        if (image == nil) {
            image = [[NSApp applicationIconImage] retain];  // NSApp 可能为 nil：消息返回 nil
        }
        [image setSize:NSMakeSize(18.0, 18.0)];
        [state->image release];
        state->image = image;
        if (state->item != nil) {
            state->item.button.image = image;
        }
    }
}

void MacOSTrayBridge::show() {
    auto *state = static_cast<TrayState *>(state_);
    if ((state == nullptr) || (state->item == nil)) {
        return;
    }
    state->item.visible = YES;
    state->visible = true;
}

void MacOSTrayBridge::hide() {
    auto *state = static_cast<TrayState *>(state_);
    if ((state == nullptr) || (state->item == nil)) {
        return;
    }
    state->item.visible = NO;
    state->visible = false;
}

void MacOSTrayBridge::set_activate(std::function<void()> callback) {
    auto *state = static_cast<TrayState *>(state_);
    if (state != nullptr) {
        state->activate = std::move(callback);
    }
}

void MacOSTrayBridge::set_menu(const std::vector<MenuItem> &items) {
    auto *state = static_cast<TrayState *>(state_);
    if ((state == nullptr) || (state->item == nil)) {
        return;
    }
    @autoreleasepool {
        [state->item setMenu:nil];
        [state->menu release];
        state->menu = nil;
        state->actions.clear();
        state->menu_items = 0;
        if (items.empty()) {
            return;
        }

        NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Aurora"];
        // 关闭自动启用：NSMenu 默认按 responder 链重算 enabled，会覆盖声明式 `MenuItem::enabled`。
        menu.autoenablesItems = NO;
        std::function<void(const std::vector<MenuItem> &, NSMenu *)> build = [&](const std::vector<MenuItem> &list,
                                                                                 NSMenu *target_menu) {
            for (const auto &entry : list) {
                if (entry.separator) {
                    [target_menu addItem:[NSMenuItem separatorItem]];
                    ++state->menu_items;
                    continue;
                }
                NSMenuItem *menu_item =
                    [[NSMenuItem alloc] initWithTitle:ns_string(entry.label) action:nil keyEquivalent:@""];
                menu_item.enabled = entry.enabled ? YES : NO;
                if (entry.checkable) {
                    menu_item.state = entry.checked ? NSControlStateValueOn : NSControlStateValueOff;
                }
                if (entry.is_submenu()) {
                    NSMenu *submenu = [[NSMenu alloc] initWithTitle:ns_string(entry.label)];
                    submenu.autoenablesItems = NO;
                    build(entry.children, submenu);
                    menu_item.submenu = submenu;
                    [submenu release];
                } else {
                    menu_item.target = state->target;
                    menu_item.action = @selector(menuItemClicked:);
                    menu_item.tag = static_cast<NSInteger>(state->actions.size());
                    state->actions.push_back(entry.on_click);
                }
                [target_menu addItem:menu_item];
                [menu_item release];
                ++state->menu_items;
            }
        };
        build(items, menu);
        state->item.menu = menu;
        state->menu = menu;
    }
}

void MacOSTrayBridge::show_notification(const std::string &title, const std::string &body) {
    // macOS 没有「托盘气泡」：用本库通知中心等价表达（同 tag 原位替换，避免堆叠）。
    Notification notification;
    notification.title = title;
    notification.body = body;
    notification.tag = "aurora-tray-balloon";
    notification.urgency = NotificationUrgency::Normal;
    const auto result = NotificationCenter::notify(notification);
    if (!result.ok()) {
        AURORA_LOG_WARN("system_tray", "tray balloon notification failed: ", result.error().code);
    }
}

auto MacOSTrayBridge::attached() const -> bool {
    const auto *state = static_cast<const TrayState *>(state_);
    return (state != nullptr) && (state->item != nil);
}

auto MacOSTrayBridge::visible() const -> bool {
    const auto *state = static_cast<const TrayState *>(state_);
    return (state != nullptr) && state->visible;
}

auto MacOSTrayBridge::tooltip() const -> std::string {
    const auto *state = static_cast<const TrayState *>(state_);
    if ((state == nullptr) || (state->item == nil)) {
        return {};
    }
    return to_std(state->item.button.toolTip);
}

auto MacOSTrayBridge::menu_item_count() const -> std::size_t {
    const auto *state = static_cast<const TrayState *>(state_);
    return (state == nullptr) ? 0U : state->menu_items;
}

auto MacOSTrayBridge::native_status_item() const -> void * {
    const auto *state = static_cast<const TrayState *>(state_);
    return (state == nullptr) ? nullptr : state->item;
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_MACOS
