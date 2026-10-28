/* macOS 全局热键真机验收探针（人工触发，不进 CTest）
// ============================================================================
// 验收范围：
//   ✅ `OsHotkeyRegistry::enabled()` 在 macOS 上为 true；
//   ✅ `add()` 经 Carbon `RegisterEventHotKey` 真正注册（返回有效句柄、count 增加）；
//   ✅ 同一组合重复注册被拒（`OsHotkeyRegisterFailed`），未指定主键同样被拒；
//   ✅ 合成 `kEventHotKeyPressed`（带库的签名 + 句柄 ID）经应用事件目标送达处理器，
//      `drain_pending()` 排空并执行动作——验证「按钮/系统事件 -> 队列 -> 帧循环回调」整条接线；
//   ✅ `remove()` 后系统注册释放（同 ID 事件不再命中、drain 为 0），`clear()` 批量释放；
//   ✅ 多个组合（字母 / 数字键）逐一注册，覆盖 Carbon 键码反向映射的代表性分支。
//
//   ❌ 不证明**真实键盘**按 Ctrl+Alt+K 时系统派发到本进程——那需要人在真机上按键；
//      自动段用合成事件证明事件处理器与库内路由，不冒充完整人工验收。
//
// 构建与运行：
//   cmake -S . -B build-macos -DAURORA_BACKEND_MACOS=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-macos --target aurora_verify_macos_hotkey
//   ./build-macos/aurora_verify_macos_hotkey
//
// 退出码：
//   0  全部判据通过
//   1  至少一条判据失败（逐行 FAIL 指出）
//   2  环境不可用（无 GUI 会话 / Carbon 事件目标不可达）
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_MACOS
#error "aurora_verify_macos_hotkey 只能在 macOS 上构建（AURORA_PLATFORM_MACOS）"
#endif
#ifndef AURORA_BACKEND_MACOS
#error "须开启 AURORA_BACKEND_MACOS"
#endif

#include "aurora/app/detail/macos_hotkey.h"
#include "aurora/app/os_hotkey.h"

#import <AppKit/AppKit.h>

#include <Carbon/Carbon.h>

#include <string>

#include "verify_print.h"

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
    AURORA_LOG_RAW("verify", ok ? "PASS  " : "FAIL  ", aurora_verify::pad_right(name, 46), detail, "\n");
}

/// @brief 向应用事件目标合成一次 `kEventHotKeyPressed`（带本库签名与热键 ID）。
/// @param id 热键 ID（`OsHotkeyHandle::id`）。
/// @return 事件成功交给事件目标时为 true（处理器是否命中另由 drain 计数观察）。
auto synthesize_press(std::uint32_t id) -> bool {
    EventRef event = nullptr;
    if (CreateEvent(nullptr, kEventClassKeyboard, kEventHotKeyPressed, GetCurrentEventTime(), kEventAttributeUserEvent,
                    &event) != noErr) {
        return false;
    }
    EventHotKeyID hotkey{};
    hotkey.signature = static_cast<OSType>(aurora::detail::AURORA_MACOS_HOTKEY_SIGNATURE);
    hotkey.id = id;
    const OSStatus set = SetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, sizeof(hotkey), &hotkey);
    const OSStatus sent =
        (set == noErr) ? SendEventToEventTarget(event, GetApplicationEventTarget()) : static_cast<OSStatus>(paramErr);
    ReleaseEvent(event);
    return sent == noErr;
}

}  // namespace

auto main() -> int {
    @autoreleasepool {
        AURORA_LOG_RAW("verify", "aurora_verify_macos_hotkey\n");
        AURORA_LOG_RAW("verify", "----------------------------------------\n");

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        if (GetApplicationEventTarget() == nullptr) {
            AURORA_LOG_RAW("verify", "ENV-UNAVAILABLE: Carbon application event target is unavailable\n");
            return 2;
        }

        aurora::OsHotkeyRegistry registry;
        check(aurora::OsHotkeyRegistry::enabled(), "OsHotkeyRegistry::enabled() is true on macOS");

        // ---- 一次真实注册 + 合成命中 ----
        bool fired = false;
        const aurora::KeyCombo first{aurora::ModifierKey::Control | aurora::ModifierKey::Alt, aurora::KeyCode::K};
        const auto handle = registry.add(first, [&fired]() -> void { fired = true; });
        check(handle.ok(), "add(Ctrl+Alt+K) returns Ok",
              handle.ok() ? std::string{} : handle.error().code);
        check(handle.ok() && handle.value().id != 0U, "handle carries a nonzero id");
        check(registry.count() == 1, "registered count is 1", aurora_verify::format_uint(registry.count()));

        const auto duplicate = registry.add(first, []() -> void {});
        check(!duplicate.ok() && duplicate.error().code_enum == aurora::ErrorCode::OsHotkeyRegisterFailed,
              "duplicate combination is rejected");

        const auto no_key = registry.add(aurora::KeyCombo{aurora::ModifierKey::Control, aurora::KeyCode::Unknown},
                                         []() -> void {});
        check(!no_key.ok() && no_key.error().code_enum == aurora::ErrorCode::OsHotkeyRegisterFailed,
              "combination without a primary key is rejected");

        check(registry.drain_pending() == 0, "drain before any press is 0");
        const bool sent = handle.ok() && synthesize_press(handle.value().id);
        check(sent, "synthetic kEventHotKeyPressed reaches the app event target");
        const std::size_t drained = registry.drain_pending();
        check(drained == 1, "drain executes the registered action", aurora_verify::format_uint(drained));
        check(fired, "action ran exactly through the queue");

        // ---- remove 后事件不再命中 ----
        check(handle.ok() && registry.remove(handle.value()), "remove(handle) succeeds once");
        check(registry.count() == 0, "count drops to 0 after remove");
        if (handle.ok()) {
            (void)synthesize_press(handle.value().id);
        }
        check(registry.drain_pending() == 0, "removed hotkey no longer queues hits");

        // ---- 多种键位逐一注册（覆盖反向键码映射的代表分支）----
        const aurora::KeyCombo others[] = {
            {aurora::ModifierKey::Control | aurora::ModifierKey::Shift, aurora::KeyCode::L},
            {aurora::ModifierKey::Meta | aurora::ModifierKey::Alt, aurora::KeyCode::M},
            {aurora::ModifierKey::Control | aurora::ModifierKey::Alt, aurora::KeyCode::D1},
            {aurora::ModifierKey::Control | aurora::ModifierKey::Alt, aurora::KeyCode::F5},
        };
        std::size_t added = 0;
        for (const auto &combo : others) {
            const auto result = registry.add(combo, []() -> void {});
            check(result.ok(), "add " + combo.to_string(), result.ok() ? std::string{} : result.error().code);
            if (result.ok()) {
                ++added;
            }
        }
        check(registry.count() == added, "count matches successful registrations",
              aurora_verify::format_uint(registry.count()));

        registry.clear();
        check(registry.count() == 0, "clear releases all registrations");
        check(registry.drain_pending() == 0, "drain after clear is 0");

        AURORA_LOG_RAW("verify", "----------------------------------------\n");
        AURORA_LOG_RAW("verify", aurora_verify::format_int(g_checks - g_failures), "/",
                       aurora_verify::format_int(g_checks), " checks passed\n");
        if (g_failures != 0) {
            AURORA_LOG_RAW("verify", "RESULT: FAIL\n");
            return 1;
        }
        AURORA_LOG_RAW("verify", "RESULT: PASS (a real key press stays manual)\n");
        return 0;
    }
}

#pragma clang diagnostic pop
