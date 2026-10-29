/// @file macos_notification.cpp
/// macOS 通知中心后端：`NSUserNotificationCenter` 的投递与激活回调接线。
///
/// 门控：仅在 `AURORA_PLATFORM_MACOS` 下编译（CMake 只把本文件登记进 macOS 后端）。
/// 语言：ObjC++、手动引用计数（与库其余平台层一致，不启用 ARC）。
///
/// 弃用说明：`NSUserNotification*` 自 macOS 11 起被标记 deprecated，但仍是当前唯一
/// **无需用户授权、无签名身份也能在 bundle 中工作**的通知入口（`UNUserNotificationCenter`
/// 在本机 ad-hoc 签名包上明确拒绝授权，实测见 `detail/macos_notification.h` 的文件注记）。
/// 因此本文件整体关闭 `-Wdeprecated-declarations`；迁移到 UserNotifications 时只需替换本 TU。

#include "aurora/app/detail/macos_notification.h"

#ifdef AURORA_PLATFORM_MACOS

#import <Foundation/Foundation.h>

#include <string>

#include "aurora/core/log.h"

// 全文件豁免：见文件头的弃用说明。区间只覆盖本 TU 内部，不影响包含者。
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace {

// `Notification::tag` 在 `NSUserNotification.userInfo` 里的键。
NSString *const AURORA_NOTIFICATION_TAG_KEY = @"x-aurora-notification-tag";

// 空 tag 的稳定占位标识（与 Win32 后端的默认动作键同值，便于跨平台归因）。
NSString *const AURORA_NOTIFICATION_DEFAULT_IDENTIFIER = @"x-aurora-default-action";

/// @brief `std::string`（UTF-8）→ `NSString`；空串得到空串而不是 nil。
/// @param text UTF-8 文本。
/// @return 自动释放的 `NSString`。
auto ns_string(const std::string &text) -> NSString * { return [NSString stringWithUTF8String:text.c_str()]; }

}  // namespace

// ───────────────────────────── delegate 定义 ─────────────────────────────

// 通知中心的进程级 delegate：前台展示策略 + 点击激活回传。
@interface AuroraMacOSNotificationDelegate : NSObject <NSUserNotificationCenterDelegate>
@end

@implementation AuroraMacOSNotificationDelegate

// 应用在前台时也展示通知（与 Windows / Linux 后端「调用即展示」同口径）。
// center / notification 未使用；恒返回 YES。
- (BOOL)userNotificationCenter:(NSUserNotificationCenter *)center
     shouldPresentNotification:(NSUserNotification *)notification {
    (void)center;
    (void)notification;
    return YES;
}

// 用户点击 / 操作通知：把 tag 交给框架的统一激活入口。
//
// callback 只带 `Notification::tag`（空 tag 回传空串，不泄露占位标识）；未注册回调时
// `NotificationCenter::emit_activated` 自身静默返回。delegate 消息由主运行循环递送，
// 与框架的单线程 UI 模型一致。center 未使用，notification 为被激活的通知。
- (void)userNotificationCenter:(NSUserNotificationCenter *)center
       didActivateNotification:(NSUserNotification *)notification {
    (void)center;
    NSString *tag = nil;
    id recorded = notification.userInfo[AURORA_NOTIFICATION_TAG_KEY];
    if ([recorded isKindOfClass:[NSString class]] != 0) {
        tag = static_cast<NSString *>(recorded);
    }
    // 非本后端投递的通知（没有 userInfo 标签）退回 identifier；两者皆空时回传空串。
    if (tag == nil) {
        tag = notification.identifier;
    }
    const char *utf8 = tag != nil ? [tag UTF8String] : nullptr;
    aurora::NotificationCenter::emit_activated(utf8 != nullptr ? std::string{utf8} : std::string{});
}

@end

namespace aurora::detail {
namespace {

/// @brief 在通知中心的已投递列表里查标识是否被系统收录。
/// @param center 通知中心。
/// @param identifier 待查标识。
/// @return 收录时为 true。
auto is_delivered(NSUserNotificationCenter *center, NSString *identifier) -> bool {
    // Objective-C fast enumeration 的循环变量由运行期逐次写入，`init-variables` 只认 C++ 的
    // 声明式初始化，故此处恒报「未初始化」——它是语言形态差异，不是漏初始化。
    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    for (NSUserNotification *existing in [center deliveredNotifications]) {
        // 列表项按语言约定可为 nil（系统实现不保证），解引用前先判掉：既是空指针防护，
        // 也让 nullability 分析知道 receiver 非空。
        if (existing == nil) {
            continue;
        }
        // `NSUserNotification::identifier` 是 nullable 属性，`isEqualToString:` 要求非空实参：
        // 先取进局部并判掉 nil，未标识的项本就匹配不上任何标识。
        NSString *recorded = [existing identifier];
        if ((recorded != nil) && ([identifier isEqualToString:recorded] != 0)) {
            return true;
        }
    }
    return false;
}

/// @brief 取得进程级 delegate（首次调用时安装到通知中心）。
///
/// 通知中心的 `delegate` 是 assign 语义，必须由本文件持有强引用；delegate 是进程级单例，
/// 生命周期与进程相同（不为它安排注销：通知中心在退出前仍可能派发最后一次回调）。
/// @return 已安装的 delegate。
auto notification_delegate() -> id<NSUserNotificationCenterDelegate> {
    static id<NSUserNotificationCenterDelegate> delegate = nil;
    if (delegate == nil) {
        delegate = [[AuroraMacOSNotificationDelegate alloc] init];
        [NSUserNotificationCenter defaultUserNotificationCenter].delegate = delegate;
    }
    return delegate;
}

}  // namespace

auto macos_post_notification(const Notification &notification) -> Result<void> {
    @autoreleasepool {
        (void)notification_delegate();
        NSUserNotificationCenter *center = [NSUserNotificationCenter defaultUserNotificationCenter];
        if (center == nil) {
            const std::string detail =
                "NSUserNotificationCenter is unavailable (macOS notifications require a bundled application)";
            AURORA_LOG_WARN("notification", "post failed: ", detail);
            return make_error(ErrorCode::NotificationPostFailed, ErrorParams{{"detail", detail}});
        }

        NSString *identifier =
            notification.tag.empty() ? AURORA_NOTIFICATION_DEFAULT_IDENTIFIER : ns_string(notification.tag);
        NSUserNotification *item = [[NSUserNotification alloc] init];
        item.identifier = identifier;
        item.title = ns_string(notification.title);
        item.informativeText = ns_string(notification.body);
        // tag 原样放进 userInfo（含空串）：激活回调按调用方语义回传，占位标识只用于系统去重。
        // 字典字面量塞 nil 会抛 `attempt to insert nil object`（当场崩），而 `stringWithUTF8String:`
        // 对非法 UTF-8 返回 nil：兜成空 tag，语义与「调用方传了空串」一致。
        NSString *tag_info = ns_string(notification.tag);
        item.userInfo = @{AURORA_NOTIFICATION_TAG_KEY : (tag_info != nil) ? tag_info : @""};
        // Low 不出声；Normal / Critical 用系统默认提示音（对应 Windows 的「静音」两档与 XDG urgency）。
        if (notification.urgency != NotificationUrgency::Low) {
            item.soundName = NSUserNotificationDefaultSoundName;
        }
        [center deliverNotification:item];
        [item release];  // deliverNotification: 会拷贝通知，释放本地所有权

        // 读回校验：无 bundle / 通知服务不可达时 deliverNotification: 是静默 no-op，
        // 不校验就会把「没发出去」报成成功。系统对同标识通知做原位替换，故按标识查一次即可。
        bool recorded = is_delivered(center, identifier);
        if (!recorded) {
            // 少数环境下投递落到主运行循环的下一轮：给一次短排空再判定，仍查不到才算失败。
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.2]];
            recorded = is_delivered(center, identifier);
        }
        if (!recorded) {
            const std::string detail =
                "the system did not record the notification (check the bundle identifier and notification settings)";
            AURORA_LOG_WARN("notification", "post failed: ", detail);
            return make_error(ErrorCode::NotificationPostFailed, ErrorParams{{"detail", detail}});
        }
        return Result<void>{};
    }
}

auto macos_pump_notifications() -> void {
    @autoreleasepool {
        if ([NSUserNotificationCenter defaultUserNotificationCenter] == nil) {
            return;  // 无 bundle：没有可排空的来源，保持 no-op
        }
        (void)notification_delegate();
        // 非阻塞排空：只跑一轮默认模式。通知中心的 delegate 消息挂在这里。
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate distantPast]];
    }
}

}  // namespace aurora::detail

#pragma clang diagnostic pop

#endif  // AURORA_PLATFORM_MACOS
