// macos_menu_bar.cpp — 原生菜单栏的 macOS 桥（NSMenu 建树 + target-action 回投）。
//
// 折算层纯函数摊平出的扁平项表在此重建为 NSMenu 树并挂到 NSApp.mainMenu；菜单项点击经
// target-action 到达 AuroraMacMenuTarget，按 representedObject 里的模型 id 回投。与 Win32 桥
// 的差异（应用级对象、无宿主守卫、快捷键文本的显示口径）见 `macos_menu_bar.h` 文件头。
#include "aurora/window/detail/macos_menu_bar.h"

#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

#import <AppKit/AppKit.h>

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/window/detail/menu_bar_translate.h"

namespace aurora::detail {

/// @brief 桥的进程级状态：当前菜单树（本桥持有一份）与点击路由回调。
///
/// 单线程 UI 契约，无锁；`NSMenu` 是应用级对象，故状态随进程存活而不是随窗口。
struct MenuBridgeState {
    NSMenu *menu = nil;  ///< 当前菜单树（本桥持有一份；nil = 未安装）
    id target = nil;  ///< 共享的 target-action 接收者（本桥持有；进程内一份）
    std::function<void(std::string)> on_click;  ///< 点击路由回调（空 = 未安装）
    bool installed = false;  ///< 最近一次 install/uninstall 的净结果
};

/// @brief 进程唯一状态（函数内 static，规避静态初始化顺序问题）。
auto menu_state() -> MenuBridgeState & {
    static MenuBridgeState s;
    return s;
}

}  // namespace aurora::detail

// 点击接收者：每个终端菜单项的 target 都指向同一实例，实例从 representedObject 取模型 id 回投。
// 声明与实现放在 C++ 命名空间之外（ObjC 类不进命名空间），状态经 `owner` 以 void * 回指。
@interface AuroraMacMenuTarget : NSObject {
  @public
    void *owner;
}
// target-action 入口：sender 为被点击的 NSMenuItem。
- (void)activate:(id)sender;
@end

@implementation AuroraMacMenuTarget

- (void)activate:(id)sender {
    auto *state = static_cast<aurora::detail::MenuBridgeState *>(owner);
    if (state == nullptr) {
        return;
    }
    auto *item = static_cast<NSMenuItem *>(sender);
    // 可勾选项就地翻转显示态（与 Win32 菜单项的默认行为一致）；命令状态仍以应用侧为准，
    // 重装菜单时模型里的 checked 会覆盖这次翻转。
    if ([item tag] != 0) {
        [item setState:([item state] == NSControlStateValueOn) ? NSControlStateValueOff : NSControlStateValueOn];
    }
    NSString *model_id = [item representedObject];
    const char *utf8 = (model_id != nil) ? [model_id UTF8String] : nullptr;
    // 先落成 std::string 再判空：空串判据用 `empty()`，不去下标裸 C 指针。
    const std::string clicked_id = (utf8 != nullptr) ? std::string{utf8} : std::string{};
    if (clicked_id.empty()) {
        // 空 id 项照常建出、照常显示，但原生路径跨重建只能靠稳定 id 路由点击——空 id 路由不了。
        // 不静默丢弃、不伪造 id，只留诊断（与门面 route_click 的口径一致）。
        AURORA_LOG_WARN("native-menu-bar", "menu item clicked but its id is empty; click ignored");
        return;
    }
    if (state->on_click) {
        state->on_click(clicked_id);  // 按模型 id 回投（桥不知命令语义，只透传标识）
    }
}

@end

namespace aurora::detail {
namespace {

/// @brief 组装菜单项标题（label + 可选的 `\t` 快捷键提示）。
///
/// `shortcut_text` 的契约是「仅显示」，故这里不生成 `keyEquivalent`（见文件头注）。分隔符不消费本函数。
/// @param it 待建项。
/// @return UTF-8 转换后的标题；转换失败退回空串（标题缺失但不阻断建树）。
[[nodiscard]] auto title_of(const menu_bar_translate::FlatMenuItem &it) -> NSString * {
    std::string text = it.label;
    if (!it.shortcut_text.empty()) {
        text += "\t" + it.shortcut_text;
    }
    NSString *title = [NSString stringWithUTF8String:text.c_str()];
    return title != nil ? title : @"";
}

/// @brief 后端回调：把扁平项表重建为 NSMenu 树并挂到 `NSApp.mainMenu`（替换语义）。
auto install_backend(const std::vector<menu_bar_translate::FlatMenuItem> &items,
                     std::function<void(std::string)> on_click) -> Result<void> {
    @autoreleasepool {
        if ([NSThread isMainThread] == NO) {
            return make_error(ErrorCode::NativeMenuInstallFailed,
                              "native-menu-bar: NSMenu must be installed on the main thread");
        }
        NSApplication *app = [NSApplication sharedApplication];
        if (app == nil) {
            return make_error(ErrorCode::NativeMenuInstallFailed, "native-menu-bar: NSApplication is unavailable");
        }
        auto &self = menu_state();
        if (self.target == nil) {
            AuroraMacMenuTarget *target = [[AuroraMacMenuTarget alloc] init];
            target->owner = &self;
            self.target = target;  // 进程内一份，随状态长存（NSMenuItem.target 不持有 target）
        }
        // 菜单标题不参与渲染（主菜单栏取 `title` 只用于菜单自身标识），取应用名而非用户可见
        // 文案，故不接本地化目录（同一处置见 src/aurora/app/detail/macos_tray.cpp）。
        // NOLINTNEXTLINE(clang-analyzer-optin.osx.cocoa.localizability.NonLocalizedStringChecker)
        NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Aurora"];
        if (menu == nil) {
            return make_error(ErrorCode::NativeMenuInstallFailed, "native-menu-bar: NSMenu allocation failed");
        }
        // 默认的自动启用会按 target/action 是否可用来覆盖显式 enabled，必须先关掉。
        [menu setAutoenablesItems:NO];
        // submenu_of[i]：项 i 若是子菜单，则记它的 NSMenu；其 children 据此挂入。
        std::vector<NSMenu *> submenu_of(items.size(), nil);
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto &it = items[i];
            // 父项的菜单：顶层项挂到根菜单，否则挂到父项的子菜单。
            NSMenu *parent =
                (it.parent_index == menu_bar_translate::AURORA_MENU_BAR_NO_PARENT) ? menu : submenu_of[it.parent_index];
            if (parent == nil) {
                // 数据不变量被违反（父项未建出子菜单）：健壮兜底跳过本项，不污染已建部分。
                continue;
            }
            if (it.separator) {
                [parent addItem:[NSMenuItem separatorItem]];
                continue;
            }
            // 前序遍历下，子项紧跟父项、深度 +1；据此判断本项是否还有 children。
            const bool has_children = (i + 1U < items.size()) && items[i + 1U].depth == it.depth + 1U;
            NSString *title = title_of(it);
            NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
            if (item == nil) {
                [menu release];
                return make_error(ErrorCode::NativeMenuInstallFailed, "native-menu-bar: NSMenuItem allocation failed");
            }
            [item setEnabled:(it.enabled ? YES : NO)];
            if (it.checkable) {
                [item setState:(it.checked ? NSControlStateValueOn : NSControlStateValueOff)];
            }
            [item setTag:(it.checkable ? 1 : 0)];  // 供 activate: 判断是否就地翻转勾选态
            if (has_children) {
                NSMenu *submenu = [[NSMenu alloc] initWithTitle:title];
                [submenu setAutoenablesItems:NO];
                [item setSubmenu:submenu];
                submenu_of[i] = submenu;
                [submenu release];  // 菜单项已持有子菜单
            } else {
                // 终端项：target-action 回投；空 id 也挂上动作，点击时留 WARN 不路由（与 Win32 同口径）。
                NSString *model_id = [NSString stringWithUTF8String:it.id.c_str()];
                [item setTarget:self.target];
                [item setAction:@selector(activate:)];
                [item setRepresentedObject:(model_id != nil ? model_id : @"")];
            }
            [parent addItem:item];
            [item release];  // 父菜单已持有菜单项
        }
        [app setMainMenu:menu];  // 替换语义：先挂新树，再放本桥对旧树的持有
        if (self.menu != nil) {
            [self.menu release];
        }
        self.menu = menu;  // alloc 的 +1 转移给桥状态
        self.on_click = std::move(on_click);
        self.installed = true;
        AURORA_LOG_DEBUG("native-menu-bar", "installed ", items.size(), " flattened entries as NSMenu");
        return Result<void>{};
    }
}

/// @brief 后端回调：摘下并释放菜单树（幂等）。
auto uninstall_backend() -> void {
    @autoreleasepool {
        auto &self = menu_state();
        if (self.menu != nil) {
            NSApplication *app = [NSApplication sharedApplication];
            if (app != nil && [app mainMenu] == self.menu) {
                [app setMainMenu:nil];
            }
            [self.menu release];
            self.menu = nil;
        }
        self.on_click = nullptr;
        self.installed = false;
    }
}

/// @brief 后端回调：当前是否已安装。
auto is_installed_backend() -> bool { return menu_state().installed; }

}  // namespace

auto macos_menu_bar_ensure_registered() -> bool {
    // 进程级一次性登记：NSMenu 是应用级对象，通路不随窗口销毁而注销。
    // 登记位取「函数局部 bool + 访问前判」形态（同 `x11_drag` 的 `backend_registered` 口径），
    // 而非 lambda 初始化的 `static const bool`：后者会被 naming 检查归入静态常量（要求全大写），
    // 且登记抛出时 lambda 把失败态粘在 static 上，本形态下次调用可重试。
    static bool registered = false;
    if (!registered) {
        menu_bar_translate::register_menu_bar_backend(menu_bar_translate::MenuBarBackend{
            .install = &install_backend,
            .uninstall = &uninstall_backend,
            .is_installed = &is_installed_backend,
        });
        registered = true;
    }
    return registered;
}

auto macos_menu_bar_is_installed() -> bool { return menu_state().installed; }

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS
