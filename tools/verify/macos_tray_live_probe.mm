/* macOS 系统托盘真机验收探针（人工触发，不进 CTest）
// ============================================================================
// 验收范围：
//   ✅ `SystemTray` 构造后经 `NSStatusBar` 真正挂上状态项（`NSStatusItem` 非空、可见）；
//   ✅ `set_title` → 状态按钮 tooltip；`set_icon` → 18x18 状态栏图像（空路径回退应用图标）；
//   ✅ `show` / `hide` 与 `NSStatusItem.visible` 同步；
//   ✅ `on_activate`：无菜单时状态按钮 action 命中回调（合成 sendAction，真实点击留给人工段）；
//   ✅ `set_context_menu` → `NSMenu`：条目数 / 勾选 / 禁用 / 分隔符 / 子菜单逐项映射，
//      叶子项经 `NSMenuItem.tag` 路由到对应 `on_click`（合成 `performActionForItemAtIndex:`）；
//   ✅ 空菜单卸下 `NSMenu`，点击激活恢复；
//   ✅ 移动构造后状态项不重建、激活回调仍指向新对象（Impl 指针稳定 + owner 改指）；
//   ✅ `show_balloon` → 系统通知中心可独立查到同名条目（bundle 形态；裸跑核对降级）；
//   ✅ 析构移除状态项（存活桥登记表归零）。
//
//   ❌ 不证明真实鼠标点击菜单栏图标 / 菜单项的系统命中路径——那需要人点一下；
//      自动段只证 target-action 接线与模型映射，不冒充完整人工验收。
//
// ⚠️ 运行形态是判据的一部分：`show_balloon` 走 `NSUserNotificationCenter`，该单例只对
//    **bundle 应用**可得（裸可执行文件为 nil）。CMake 目标构建后把本探针封装为
//    `AuroraMacOSTrayProbe.app` 并 ad-hoc 签名；请经包内可执行文件运行。直接跑裸二进制
//    仍会完整核对状态项与菜单（这两项不依赖 bundle），气泡只核对结构化降级并以退出码 3 报 PENDING。
//
// 构建与运行：
//   cmake -S . -B build-macos -DAURORA_BACKEND_MACOS=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-macos --target aurora_verify_macos_tray
//   ./build-macos/AuroraMacOSTrayProbe.app/Contents/MacOS/aurora_verify_macos_tray
//
// 退出码：
//   0  全部判据通过
//   1  至少一条判据失败（逐行 FAIL 指出）
//   2  环境不可用（无 GUI 会话 / 通知中心单例取不到）
//   3  判据不适用（裸可执行文件：状态项与菜单已验，气泡需 bundle）
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_MACOS
#error "aurora_verify_macos_tray 只能在 macOS 上构建（AURORA_PLATFORM_MACOS）"
#endif
#ifndef AURORA_BACKEND_MACOS
#error "须开启 AURORA_BACKEND_MACOS"
#endif

#include "aurora/app/detail/macos_tray.h"
#include "aurora/app/system_tray.h"

#import <AppKit/AppKit.h>

#include <string>
#include <unistd.h>
#include <vector>

#include "verify_print.h"

// 气泡读回走 legacy NSUserNotificationCenter（弃用但可用），本 TU 内豁免。
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace {

int g_checks = 0;
int g_failures = 0;

auto check(bool ok, const std::string &name, const std::string &detail = {}) -> void {
    ++g_checks;
    if (!ok) {
        ++g_failures;
    }
    AURORA_LOG_RAW("verify", ok ? "PASS  " : "FAIL  ", aurora_verify::pad_right(name, 52), detail, "\n");
}

auto ns_string(const std::string &text) -> NSString * {
    return [NSString stringWithUTF8String:text.c_str()];
}

auto to_std(NSString *text) -> std::string {
    const char *utf8 = text != nil ? [text UTF8String] : nullptr;
    return utf8 != nullptr ? std::string{utf8} : std::string{};
}

/// @brief 托盘气泡使用的稳定标识（与 `MacOSTrayBridge` 实现同值；按系统可见结果核对）。
NSString *const kBalloonIdentifier = @"aurora-tray-balloon";

/// @brief 在系统已投递列表中按标识取条目（独立取证，不经库内记账）。
auto delivered_item(NSString *identifier) -> NSUserNotification * {
    for (NSUserNotification *item in [[NSUserNotificationCenter defaultUserNotificationCenter] deliveredNotifications]) {
        if ([[item identifier] isEqualToString:identifier]) {
            return item;
        }
    }
    return nil;
}

/// @brief 清掉本探针可能残留的气泡条目。
auto cleanup_balloon() -> void {
    NSUserNotificationCenter *center = [NSUserNotificationCenter defaultUserNotificationCenter];
    if (center == nil) {
        return;
    }
    NSUserNotification *item = delivered_item(kBalloonIdentifier);
    if (item != nil) {
        [center removeDeliveredNotification:item];
    }
}

/// @brief 写一张 16x16 PNG 到指定路径，供 `set_icon` 加载。
/// @param path 目标文件路径。
/// @return 写入成功为 true。
auto write_icon_file(NSString *path) -> bool {
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                    pixelsWide:16
                                                                    pixelsHigh:16
                                                                 bitsPerSample:8
                                                               samplesPerPixel:4
                                                                      hasAlpha:YES
                                                                      isPlanar:NO
                                                                colorSpaceName:NSDeviceRGBColorSpace
                                                                   bytesPerRow:0
                                                                  bitsPerPixel:0];
    if (rep == nil) {
        return false;
    }
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return png != nil && [png writeToFile:path atomically:YES];
}

/// @brief 合成一次状态按钮点击（直接向 target 派发 action）。
/// @param item 状态项。
/// @return action 被派发为 true。
auto send_button_action(NSStatusItem *item) -> bool {
    NSButton *button = item.button;
    if ((button == nil) || (button.action == nil) || (button.target == nil)) {
        return false;
    }
    return [NSApp sendAction:button.action to:button.target from:button] != NO;
}

/// @brief 按标题找顶层菜单项。
/// @param menu 菜单。
/// @param title 标题。
/// @return 命中项；未找到为 nil。
auto menu_item_with_title(NSMenu *menu, NSString *title) -> NSMenuItem * {
    for (NSMenuItem *item in menu.itemArray) {
        if ([[item title] isEqualToString:title]) {
            return item;
        }
    }
    return nil;
}

}  // namespace

auto main() -> int {
    @autoreleasepool {
        AURORA_LOG_RAW("verify", "aurora_verify_macos_tray\n");
        AURORA_LOG_RAW("verify", "----------------------------------------\n");

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp finishLaunching];

        if ([NSStatusBar systemStatusBar] == nil) {
            AURORA_LOG_RAW("verify", "ENV-UNAVAILABLE: NSStatusBar is nil (no GUI session)\n");
            return 2;
        }

        const bool bundled = [[NSBundle mainBundle] bundleIdentifier] != nil;
        AURORA_LOG_RAW("verify", "bundle: ", bundled ? to_std([[NSBundle mainBundle] bundleIdentifier]) : "nil",
                       "\n");

        const std::string temp_icon =
            to_std(NSTemporaryDirectory()) + "aurora_tray_probe_" + std::to_string(::getpid()) + ".png";
        NSString *ns_icon = ns_string(temp_icon);
        check(write_icon_file(ns_icon), "probe icon file is written", temp_icon);
        cleanup_balloon();

        {
            aurora::SystemTray tray("Aurora tray probe", "");
            check(aurora::detail::live_tray_bridges().size() == 1, "SystemTray creates exactly one bridge");
            aurora::detail::MacOSTrayBridge *bridge =
                aurora::detail::live_tray_bridges().empty() ? nullptr : aurora::detail::live_tray_bridges().front();
            check(bridge != nullptr && bridge->attached(), "NSStatusItem is attached");
            check(bridge != nullptr && bridge->native_status_item() != nullptr, "native NSStatusItem is reachable");

            NSStatusItem *item =
                (bridge != nullptr) ? (__bridge NSStatusItem *)bridge->native_status_item() : nil;
            check(item != nil, "status item pointer is non-nil");
            check(item != nil && item.button != nil, "status item exposes a button");
            check(bridge != nullptr && bridge->visible() && item.visible, "status item starts visible");

            check(bridge != nullptr && bridge->tooltip() == "Aurora tray probe", "initial tooltip comes from the title",
                  bridge != nullptr ? bridge->tooltip() : std::string{});

            tray.set_title("Aurora tray probe v2");
            check(bridge != nullptr && bridge->tooltip() == "Aurora tray probe v2",
                  "set_title forwards to the status button tooltip", bridge != nullptr ? bridge->tooltip() : std::string{});
            check(item != nil && to_std(item.button.toolTip) == "Aurora tray probe v2", "AppKit tooltip matches");

            tray.set_icon(temp_icon);
            check(item != nil && item.button.image != nil, "set_icon loads the image file");
            check(item != nil && item.button.image.size.width == 18.0 && item.button.image.size.height == 18.0,
                  "status image is normalized to 18x18");

            tray.set_icon("");
            const bool app_icon = [NSApp applicationIconImage] != nil;
            check(!app_icon || (item != nil && item.button.image != nil),
                  "empty path falls back to the application icon", app_icon ? "app icon available" : "no app icon");

            tray.hide();
            check(bridge != nullptr && !bridge->visible() && (item == nil || !item.visible),
                  "hide() hides the status item");
            tray.show();
            check(bridge != nullptr && bridge->visible() && (item == nil || item.visible), "show() reveals it again");

            // ---- 无菜单时的点击激活 ----
            int activations = 0;
            tray.on_activate([&activations] { ++activations; });
            check(send_button_action(item), "status button action is dispatched");
            check(activations == 1, "on_activate callback fires once", aurora_verify::format_int(activations));

            // ---- 菜单映射：普通 / 勾选 / 禁用 / 分隔符 / 子菜单 ----
            int alpha = 0;
            int beta = 0;
            std::vector<aurora::MenuItem> items;
            aurora::MenuItem first("Alpha", [&alpha] { ++alpha; });
            first.checkable = true;
            first.checked = true;
            items.push_back(first);
            aurora::MenuItem disabled("Disabled", [] {});
            disabled.enabled = false;
            items.push_back(disabled);
            items.push_back(aurora::MenuItem::separator_item());
            aurora::MenuItem more("More");
            more.children.push_back(aurora::MenuItem("Beta", [&beta] { ++beta; }));
            items.push_back(more);
            tray.set_context_menu(std::move(items));

            NSMenu *menu = (item != nil) ? item.menu : nil;
            check(menu != nil, "set_context_menu attaches an NSMenu");
            check(bridge != nullptr && bridge->menu_item_count() == 5, "menu item count includes nested entries",
                  bridge != nullptr ? aurora_verify::format_uint(bridge->menu_item_count()) : std::string{});
            check(menu != nil && menu.numberOfItems == 4, "top-level item count matches");
            check(menu != nil && !menu.autoenablesItems, "auto-enabling is off (declarative enabled wins)");

            NSMenuItem *alpha_item = (menu != nil) ? menu_item_with_title(menu, @"Alpha") : nil;
            NSMenuItem *disabled_item = (menu != nil) ? menu_item_with_title(menu, @"Disabled") : nil;
            NSMenuItem *more_item = (menu != nil) ? menu_item_with_title(menu, @"More") : nil;
            check(alpha_item != nil && alpha_item.state == NSControlStateValueOn, "checkable + checked maps to NSControlStateValueOn");
            check(disabled_item != nil && !disabled_item.enabled, "enabled=false maps to a disabled item");
            check(menu != nil && menu.numberOfItems == 4 && [[menu itemAtIndex:2] isSeparatorItem],
                  "separator maps to a separator item");
            check(more_item != nil && more_item.submenu != nil && more_item.submenu.numberOfItems == 1,
                  "nested children map to a submenu");
            check(more_item != nil && more_item.submenu != nil && !more_item.submenu.autoenablesItems,
                  "submenu auto-enabling is off");

            // ---- 叶子项点击路由（合成 performAction）----
            if (alpha_item != nil) {
                [menu performActionForItemAtIndex:[menu indexOfItem:alpha_item]];
            }
            check(alpha == 1, "top-level leaf routes to its on_click", aurora_verify::format_int(alpha));
            if (more_item != nil && more_item.submenu != nil) {
                [more_item.submenu performActionForItemAtIndex:0];
            }
            check(beta == 1, "submenu leaf routes to its on_click", aurora_verify::format_int(beta));

            // ---- 空菜单卸下，激活恢复 ----
            tray.set_context_menu({});
            check(item != nil && item.menu == nil, "empty menu detaches the NSMenu");
            check(bridge != nullptr && bridge->menu_item_count() == 0, "menu item count resets");
            check(send_button_action(item), "status button action survives menu removal");
            check(activations == 2, "activation still reaches the callback", aurora_verify::format_int(activations));

            // ---- 移动后仍指向新对象 ----
            int moved_activations = 0;
            tray.set_title("Aurora tray probe v3");
            check(bridge != nullptr && bridge->tooltip() == "Aurora tray probe v3", "title updates before the move");
            aurora::SystemTray moved(std::move(tray));
            check(!aurora::detail::live_tray_bridges().empty() &&
                      aurora::detail::live_tray_bridges().front() == bridge,
                  "move keeps the same bridge (status item is not recreated)");
            moved.on_activate([&moved_activations] { ++moved_activations; });
            check(send_button_action(item), "moved tray still dispatches the button action");
            check(moved_activations == 1, "activation reaches the moved object", aurora_verify::format_int(moved_activations));

            // ---- 气泡：系统通知中心独立取证（bundle 形态）----
            moved.show_balloon("Aurora tray balloon", "balloon body from the tray probe");
            check(moved.last_balloon_message() == "balloon body from the tray probe", "last_balloon_message is recorded");
            if (bundled) {
                NSUserNotificationCenter *center = [NSUserNotificationCenter defaultUserNotificationCenter];
                check(center != nil, "notification center is available in a bundle");
                if (center != nil) {
                    NSUserNotification *balloon = delivered_item(kBalloonIdentifier);
                    check(balloon != nil, "system recorded the balloon notification");
                    check(balloon != nil && to_std([balloon title]) == "Aurora tray balloon", "balloon title matches");
                    check(balloon != nil && to_std([balloon informativeText]) == "balloon body from the tray probe",
                          "balloon body matches");
                    cleanup_balloon();
                    check(delivered_item(kBalloonIdentifier) == nil, "balloon notification is cleaned up");
                }
            }
        }

        check(aurora::detail::live_tray_bridges().empty(), "destruction removes the status item from the registry");
        [[NSFileManager defaultManager] removeItemAtPath:ns_icon error:nil];

        AURORA_LOG_RAW("verify", "----------------------------------------\n");
        AURORA_LOG_RAW("verify", aurora_verify::format_int(g_checks - g_failures), "/",
                       aurora_verify::format_int(g_checks), " checks passed\n");
        if (g_failures != 0) {
            AURORA_LOG_RAW("verify", "RESULT: FAIL\n");
            return 1;
        }
        if (!bundled) {
            AURORA_LOG_RAW("verify", "PENDING: run the bundled executable for balloon readback\n");
            return 3;
        }
        AURORA_LOG_RAW("verify", "RESULT: PASS (real menu-bar clicks stay manual)\n");
        return 0;
    }
}

#pragma clang diagnostic pop
