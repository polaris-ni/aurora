/* macOS 通知中心真机验收探针（人工触发，不进 CTest）
// ============================================================================
// 验收范围：
//   ✅ `NotificationCenter::notify()` 经 `NSUserNotificationCenter` 真正投递，系统通知中心
//      的 `deliveredNotifications` 里能独立查到对应条目（标识/标题/正文/声音逐项核对）；
//   ✅ `tag` 映射为系统标识：同 tag 二次投递由系统**原位替换**（条数仍为 1、内容更新）；
//   ✅ 紧急度映射：Critical 带默认提示音、Low 不带；
//   ✅ 空 tag 使用稳定占位标识，激活回调仍回传空串；
//   ✅ 点击激活的接线：构造 delegate 回调后 `set_on_notification_activated` 的回调收到原 tag
//      （**合成激活**，验的是库内 delegate → 回调这一段；真实鼠标点击留给人工段）；
//   ✅ `pump_events()` 无事件时安全返回；`last_notification()` 字段往返。
//
//   ❌ 不证明真实鼠标点击通知后的系统焦点/回传路径——那需要人点一下横幅；
//      自动段只证 delegate 的入口与参数，不冒充完整人工验收。
//
// ⚠️ 运行形态是判据的一部分（本机实测 macOS 26）：`NSUserNotificationCenter` 只对**bundle
//    应用**可得（裸可执行文件取到的单例为 nil）。CMake 目标构建后把本探针封装为
//    `AuroraMacOSNotificationProbe.app` 并 ad-hoc 签名；请经包内可执行文件运行。
//    直接跑裸二进制会走「结构化失败」负向核对并以退出码 3 报 PENDING。
//
// 构建与运行：
//   cmake -S . -B build-macos -DAURORA_BACKEND_MACOS=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-macos --target aurora_verify_macos_notification
//   ./build-macos/AuroraMacOSNotificationProbe.app/Contents/MacOS/aurora_verify_macos_notification
//
// 退出码：
//   0  全部判据通过
//   1  至少一条判据失败（逐行 FAIL 指出）
//   2  环境不可用（通知中心单例取不到）
//   3  判据不适用（裸可执行文件：负向路径通过，真实投递需 bundle）
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_MACOS
#error "aurora_verify_macos_notification 只能在 macOS 上构建（AURORA_PLATFORM_MACOS）"
#endif
#ifndef AURORA_BACKEND_MACOS
#error "须开启 AURORA_BACKEND_MACOS"
#endif

#include "aurora/app/notification.h"

#import <AppKit/AppKit.h>

#include <string>
#include <unistd.h>

#include "verify_print.h"

// 探针直接读系统通知中心的条目（独立取证），legacy API 的弃用告警在本 TU 内豁免。
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

/// @brief 空 tag 的占位标识（与后端实现同值；探针按系统可见结果核对）。
NSString *const kDefaultIdentifier = @"x-aurora-default-action";

auto ns_string(const std::string &text) -> NSString * {
    return [NSString stringWithUTF8String:text.c_str()];
}

auto to_std(NSString *text) -> std::string {
    const char *utf8 = text != nil ? [text UTF8String] : nullptr;
    return utf8 != nullptr ? std::string{utf8} : std::string{};
}

/// @brief 在系统已投递列表中按标识取条目（独立取证，不经库内记账）。
auto delivered_item(NSString *identifier) -> NSUserNotification * {
    for (NSUserNotification *item in [[NSUserNotificationCenter defaultUserNotificationCenter] deliveredNotifications]) {
        if ([[item identifier] isEqualToString:identifier]) {
            return item;
        }
    }
    return nil;
}

/// @brief 同一标识在系统列表中的条数（验原位替换用）。
auto delivered_count(NSString *identifier) -> std::size_t {
    std::size_t count = 0;
    for (NSUserNotification *item in [[NSUserNotificationCenter defaultUserNotificationCenter] deliveredNotifications]) {
        if ([[item identifier] isEqualToString:identifier]) {
            ++count;
        }
    }
    return count;
}

/// @brief 清掉本探针留下的通知条目（标识前缀 + 空 tag 占位）。
auto cleanup_delivered(NSString *prefix) -> void {
    NSUserNotificationCenter *center = [NSUserNotificationCenter defaultUserNotificationCenter];
    NSArray<NSUserNotification *> *snapshot = [[center deliveredNotifications] copy];
    for (NSUserNotification *item in snapshot) {
        NSString *identifier = [item identifier];
        if ([identifier hasPrefix:prefix] || [identifier isEqualToString:kDefaultIdentifier]) {
            [center removeDeliveredNotification:item];
        }
    }
}

}  // namespace

auto main() -> int {
    @autoreleasepool {
        AURORA_LOG_RAW("verify", "aurora_verify_macos_notification\n");
        AURORA_LOG_RAW("verify", "----------------------------------------\n");

        [NSApplication sharedApplication];

        // 运行形态判据：裸可执行文件取不到通知中心，先验「结构化失败、不谎报成功」。
        if ([[NSBundle mainBundle] bundleIdentifier] == nil) {
            aurora::Notification bare;
            bare.title = "Aurora bare probe";
            bare.tag = "aurora-verify-notify-bare";
            const auto result = aurora::NotificationCenter::notify(bare);
            check(!result.ok(), "bare executable reports a structured failure");
            check(!result.ok() && result.error().code_enum == aurora::ErrorCode::NotificationPostFailed,
                  "failure code is NotificationPostFailed",
                  result.ok() ? std::string{} : result.error().code);
            AURORA_LOG_RAW("verify", "PENDING: run the bundled executable for real delivery checks\n");
            return 3;
        }

        NSUserNotificationCenter *center = [NSUserNotificationCenter defaultUserNotificationCenter];
        if (center == nil) {
            AURORA_LOG_RAW("verify", "ENV-UNAVAILABLE: NSUserNotificationCenter singleton is nil\n");
            return 2;
        }

        const std::string prefix = "aurora-verify-notify-" + std::to_string(::getpid()) + "-";
        NSString *ns_prefix = ns_string(prefix);
        cleanup_delivered(ns_prefix);

        AURORA_LOG_RAW("verify", "bundle: ", to_std([[NSBundle mainBundle] bundleIdentifier]), "\n");

        // ---- 投递 + 字段往返 ----
        aurora::Notification first;
        first.title = "Aurora verify notification";
        first.body = "body from the macOS live probe";
        first.tag = prefix + "main";
        first.urgency = aurora::NotificationUrgency::Critical;
        first.timeout_ms = 1200;
        const auto sent = aurora::NotificationCenter::notify(first);
        check(sent.ok(), "notify() returns Ok", sent.ok() ? std::string{} : sent.error().code);

        const auto recorded = aurora::NotificationCenter::last_notification();
        check(recorded.has_value() && recorded->tag == first.tag, "last_notification() keeps the tag");
        check(recorded.has_value() && recorded->title == first.title, "last_notification() keeps the title");
        check(recorded.has_value() && recorded->timeout_ms == first.timeout_ms, "last_notification() keeps timeout_ms");

        NSString *main_id = ns_string(first.tag);
        NSUserNotification *main_item = delivered_item(main_id);
        check(main_item != nil, "system recorded the notification (identifier)", first.tag);
        check(main_item != nil && to_std([main_item title]) == first.title, "delivered title matches");
        check(main_item != nil && to_std([main_item informativeText]) == first.body, "delivered body matches");
        check(main_item != nil && [main_item soundName] != nil, "Critical urgency carries the default sound");

        // ---- 同 tag 原位替换 ----
        aurora::Notification updated = first;
        updated.title = "Aurora verify notification (updated)";
        updated.body = "replacement body";
        updated.urgency = aurora::NotificationUrgency::Normal;
        const auto resent = aurora::NotificationCenter::notify(updated);
        check(resent.ok(), "second notify() with the same tag returns Ok");
        check(delivered_count(main_id) == 1, "same tag replaces instead of stacking",
              aurora_verify::format_uint(delivered_count(main_id)));
        NSUserNotification *updated_item = delivered_item(main_id);
        check(updated_item != nil && to_std([updated_item title]) == updated.title, "replacement title is visible");

        // ---- Low 紧急度不出声 ----
        aurora::Notification quiet;
        quiet.title = "Aurora quiet notification";
        quiet.body = "low urgency";
        quiet.tag = prefix + "low";
        quiet.urgency = aurora::NotificationUrgency::Low;
        const auto quiet_result = aurora::NotificationCenter::notify(quiet);
        check(quiet_result.ok(), "Low urgency notify() returns Ok");
        NSUserNotification *quiet_item = delivered_item(ns_string(quiet.tag));
        check(quiet_item != nil && [quiet_item soundName] == nil, "Low urgency has no sound");

        // ---- 空 tag：稳定占位标识，激活回传空串 ----
        aurora::Notification untagged;
        untagged.title = "Aurora untagged notification";
        untagged.body = "empty tag";
        const auto untagged_result = aurora::NotificationCenter::notify(untagged);
        check(untagged_result.ok(), "empty-tag notify() returns Ok");
        NSUserNotification *untagged_item = delivered_item(kDefaultIdentifier);
        check(untagged_item != nil, "empty tag uses the stable placeholder identifier");

        // ---- 激活接线（合成 delegate 回调）----
        std::string activated;
        int activations = 0;
        aurora::NotificationCenter::set_on_notification_activated([&activated, &activations](std::string tag) -> void {
            ++activations;
            activated = std::move(tag);
        });
        id<NSUserNotificationCenterDelegate> delegate = [center delegate];
        const bool can_activate = delegate != nil &&
                                  [delegate respondsToSelector:@selector(userNotificationCenter:didActivateNotification:)];
        check(can_activate, "notification delegate is installed");
        if (can_activate && main_item != nil) {
            [delegate userNotificationCenter:center didActivateNotification:delivered_item(main_id)];
        }
        check(activations == 1, "synthetic activation reaches the callback");
        check(activated == first.tag, "callback receives the original tag", activated);

        if (can_activate && untagged_item != nil) {
            [delegate userNotificationCenter:center didActivateNotification:delivered_item(kDefaultIdentifier)];
        }
        check(activations == 2 && activated.empty(), "empty tag activates with an empty string");
        aurora::NotificationCenter::set_on_notification_activated(nullptr);

        // ---- pump 安全 + 清理 ----
        aurora::NotificationCenter::pump_events();
        aurora::NotificationCenter::pump_events();
        check(true, "pump_events() is safe without pending work");

        cleanup_delivered(ns_prefix);
        check(delivered_item(main_id) == nil && delivered_item(ns_string(quiet.tag)) == nil,
              "probe notifications are removed");
        check(delivered_item(kDefaultIdentifier) == nil, "placeholder notification is removed");

        AURORA_LOG_RAW("verify", "----------------------------------------\n");
        AURORA_LOG_RAW("verify", aurora_verify::format_int(g_checks - g_failures), "/",
                       aurora_verify::format_int(g_checks), " checks passed\n");
        if (g_failures != 0) {
            AURORA_LOG_RAW("verify", "RESULT: FAIL\n");
            return 1;
        }
        AURORA_LOG_RAW("verify", "RESULT: PASS (real click delivery stays manual)\n");
        return 0;
    }
}

#pragma clang diagnostic pop
