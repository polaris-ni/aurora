/* macOS 原生菜单栏真机验收探针（人工触发，不进 CTest）
// ============================================================================
// 验收范围：
//   ✅ `PlatformCapabilities::native_menu_bar` 在 macOS 后端上翻位；
//   ✅ `NativeMenuBar::install()` 把 `MenuModel` 树投到 `NSApp.mainMenu`——逐项核对标题、
//      快捷键显示文本、分隔符、勾选态、禁用态、子菜单层级；
//   ✅ 点击经 target-action 回到 `CommandRegistry::invoke`（顶层项 / 嵌套项各一条）；
//   ✅ 可勾选项点击就地翻转 `NSMenuItem.state`；空 id 项点击不路由（不崩溃、不伪造 id）；
//   ✅ 重复 `install` 替换旧树、`uninstall` 后门面与桥的安装态都归零。
//
//   ❌ 不证明**真实鼠标**在屏幕顶栏点中菜单项——那需要人在真机上操作；本探针用
//      `performActionForItemAtIndex:` 驱动真实的 target-action 派发链，不冒充完整人工验收。
//
// 构建与运行：
//   cmake -S . -B build-macos -DAURORA_BACKEND_MACOS=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-macos --target aurora_verify_macos_menu_bar
//   ./build-macos/aurora_verify_macos_menu_bar
//
// 退出码：
//   0  全部判据通过
//   1  至少一条判据失败（逐行 FAIL 指出）
//   2  环境不可用（NSApplication 无法建立）
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_MACOS
#error "aurora_verify_macos_menu_bar 只能在 macOS 上构建（AURORA_PLATFORM_MACOS）"
#endif
#ifndef AURORA_BACKEND_MACOS
#error "须开启 AURORA_BACKEND_MACOS"
#endif

#include "aurora/app/menu.h"
#include "aurora/app/native_menu_bar.h"
#include "aurora/commands.h"
#include "aurora/window/detail/macos_menu_bar.h"
#include "aurora/window/platform.h"

#import <AppKit/AppKit.h>

#include <string>
#include <vector>

#include "verify_print.h"

namespace {

// 探针的通过 / 失败计数：匿名命名空间内的进程私有记账（无跨 TU 可见性、无初始化顺序
// 问题），就地豁免非 const 全局（同 x11_csd_live_probe 的探针记账口径）。
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
int g_checks = 0;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
int g_failures = 0;

auto check(bool ok, const std::string &name, const std::string &detail = {}) -> void {
    ++g_checks;
    if (!ok) {
        ++g_failures;
    }
    AURORA_LOG_RAW("verify", ok ? "PASS  " : "FAIL  ", aurora_verify::pad_right(name, 48), detail, "\n");
}

auto utf8(NSString *text) -> std::string {
    const char *raw = text != nil ? [text UTF8String] : nullptr;
    return raw != nullptr ? std::string{raw} : std::string{};
}

auto equal_text(NSString *text, const std::string &expected) -> bool { return utf8(text) == expected; }

}  // namespace

// 入口不吞异常：探针的失败以未捕获异常落到非零退出码，捕获反而把它压成 0（与既有探针同口径）。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        if (app == nil) {
            AURORA_LOG_RAW("verify", "FAIL  NSApplication unavailable (no GUI session)\n");
            return 2;
        }
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [app finishLaunching];

        aurora::Platform platform;
        platform.surface = aurora::SurfaceKind::MacOS;
        check(platform.capabilities().native_menu_bar, "capability native_menu_bar is true on macOS");

        aurora::CommandRegistry registry;
        std::vector<std::string> invoked;
        registry.add(aurora::Command{
            .id = "file.open", .title = "Open", .action = [&invoked]() -> void { invoked.emplace_back("file.open"); }});
        registry.add(aurora::Command{.id = "view.checked", .title = "Checked", .action = [&invoked]() -> void {
                                         invoked.emplace_back("view.checked");
                                     }});
        registry.add(aurora::Command{.id = "file.nested", .title = "Nested", .action = [&invoked]() -> void {
                                         invoked.emplace_back("file.nested");
                                     }});

        auto &bar = aurora::NativeMenuBar::instance();
        bar.uninstall();  // 起点清零
        bar.set_command_registry(&registry);
        check(aurora::detail::macos_menu_bar_ensure_registered(), "bridge registration is idempotent");

        aurora::MenuModel file;
        file.label = "File";
        aurora::MenuModel open;
        open.label = "Open";
        open.shortcut_text = "Ctrl+O";
        open.id = "file.open";
        aurora::MenuModel separator;
        separator.separator = true;
        aurora::MenuModel checked;
        checked.label = "Checked";
        checked.checkable = true;
        checked.checked = true;
        checked.id = "view.checked";
        aurora::MenuModel disabled;
        disabled.label = "Disabled";
        disabled.enabled = false;
        disabled.id = "file.disabled";
        aurora::MenuModel anonymous;
        anonymous.label = "Anonymous";  // 无 id
        aurora::MenuModel more;
        more.label = "More";
        aurora::MenuModel nested;
        nested.label = "Nested";
        nested.id = "file.nested";
        more.children.push_back(nested);
        file.children = {open, separator, checked, disabled, anonymous, more};

        const auto installed = bar.install({file});
        check(installed.ok(), "install returns Ok", installed.ok() ? "" : installed.error().message);
        check(bar.is_installed() && aurora::detail::macos_menu_bar_is_installed(), "install state is reported");

        NSMenu *main_menu = [NSApp mainMenu];
        check(main_menu != nil, "NSApp.mainMenu is present");
        check(main_menu != nil && [main_menu numberOfItems] == 1, "menu bar has one top-level menu");
        NSMenuItem *file_item = (main_menu != nil && [main_menu numberOfItems] > 0) ? [main_menu itemAtIndex:0] : nil;
        check(equal_text([file_item title], "File"), "top-level title follows the model label");
        NSMenu *file_menu = [file_item submenu];
        check(file_menu != nil && [file_menu numberOfItems] == 6, "File submenu has six items (incl. separator)");

        if (file_menu != nil && [file_menu numberOfItems] == 6) {
            NSMenuItem *open_item = [file_menu itemAtIndex:0];
            check(equal_text([open_item title], std::string { "Open\tCtrl+O" }),
                  "shortcut text is appended after a tab");
            check([[file_menu itemAtIndex:1] isSeparatorItem] != 0, "separator maps to NSMenuItem separator");
            check([[file_menu itemAtIndex:2] state] == NSControlStateValueOn, "checked state is mapped");
            check([[file_menu itemAtIndex:2] tag] != 0, "checkable item is flagged for in-place toggle");
            check([[file_menu itemAtIndex:3] isEnabled] == NO, "disabled item is greyed out");
            check([[file_menu itemAtIndex:4] representedObject] != nil, "anonymous item is still built");
            NSMenu *more_menu = [[file_menu itemAtIndex:5] submenu];
            check(more_menu != nil && [more_menu numberOfItems] == 1, "nested submenu is rebuilt");

            // 点击：走真实的 NSMenu target-action 派发链（不是直接调回调）。
            [file_menu performActionForItemAtIndex:0];
            check(invoked.size() == 1 && invoked.back() == "file.open", "top-level click routes by id");
            [file_menu performActionForItemAtIndex:2];
            check(invoked.size() == 2 && invoked.back() == "view.checked", "checkable click routes by id");
            check([[file_menu itemAtIndex:2] state] == NSControlStateValueOff, "checkable click toggles state");
            const std::size_t before_anonymous = invoked.size();
            [file_menu performActionForItemAtIndex:4];
            check(invoked.size() == before_anonymous, "empty-id click is not routed");
            if (more_menu != nil) {
                [more_menu performActionForItemAtIndex:0];
                check(invoked.size() == before_anonymous + 1 && invoked.back() == "file.nested",
                      "nested click routes by id");
            }
        }

        aurora::MenuModel edit;
        edit.label = "Edit";
        aurora::MenuModel undo;
        undo.label = "Undo";
        undo.id = "edit.undo";
        edit.children.push_back(undo);
        check(bar.install({edit}).ok(), "re-install replaces the menu tree");
        NSMenu *replaced = [NSApp mainMenu];
        check(replaced != nil && [replaced numberOfItems] == 1 && equal_text([[replaced itemAtIndex:0] title], "Edit"),
              "replacement tree is visible");

        bar.uninstall();
        check(!bar.is_installed() && !aurora::detail::macos_menu_bar_is_installed(), "uninstall clears install state");

        AURORA_LOG_RAW("verify", "\n", g_failures == 0 ? "ALL PASS" : "FAILURES", " (",
                       aurora_verify::format_int(g_checks), " checks, ", aurora_verify::format_int(g_failures),
                       " failed)\n");
    }
    return g_failures == 0 ? 0 : 1;
}
