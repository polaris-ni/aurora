#pragma once

/// @brief macOS 通知中心后端（`NSUserNotificationCenter`）的 C++ 接线面。
///
/// 单独成头的原因：`notification.cpp` 是纯 C++ TU，不能直接触碰 AppKit / Foundation 对象；
/// 平台细节全部落在同目录的 `macos_notification.cpp`，本头只暴露三个 C++ 可调用的函数。
/// 门控与实现同源（`AURORA_PLATFORM_MACOS`），非 macOS 构建下整份声明被裁掉。
///
/// **为何走 legacy `NSUserNotificationCenter` 而非 `UNUserNotificationCenter`**：
/// 后者要求应用先取得用户授权，而授权在无签名身份的构建（本库 CI / 开发机的 ad-hoc 包）
/// 会被系统直接拒绝（`UNErrorDomain Code=1 "Notifications are not allowed for this application"`，
/// 实测 ad-hoc 签名包同样被拒）；legacy 路径在带 bundle 的应用里即刻可用、且仍受系统
/// 通知中心统一管理（勿扰模式、横幅样式、通知列表都照常生效）。代价是 API 自 macOS 11 起
/// 标记 deprecated——实现层就近关闭弃用告警，并保留切到 UserNotifications 的升级面：
/// 授权可用时再换后端不影响本头的三个入口。
/// @file macos_notification.h

#include "aurora/core/platform.h"

#ifdef AURORA_PLATFORM_MACOS

#include "aurora/app/notification.h"
#include "aurora/core/result.h"

namespace aurora::detail {

/// @brief 把一条通知投递到 macOS 通知中心。
///
/// 语义映射：`tag` → `NSUserNotification.identifier`（系统据此**原位替换**同标识的通知，
/// 与框架「同 tag 去重」口径一致）；`urgency` 为 `Low` 时不带声音，`Normal` / `Critical`
/// 使用系统默认提示音；`timeout_ms` 无对应机制（legacy API 只有 `deliveryDate` 定时投递，
/// 没有自动撤除），如实忽略。空 `tag` 使用稳定占位标识，激活回调仍回传调用方的空串。
///
/// 必须在有 bundle 的应用里调用：无 bundle 的可执行文件取不到通知中心（单例为 nil），
/// 此时返回 `NotificationPostFailed` 而非静默成功。
/// @param notification 通知内容。
/// @return 成功为 `Ok`；通知中心不可用或投递后系统未收录为 `NotificationPostFailed`。
[[nodiscard]] auto macos_post_notification(const Notification &notification) -> Result<void>;

/// @brief 排空主运行循环上待处理的通知中心事件（用户点击 / 关闭的回调由此派发）。
///
/// 非阻塞：只跑一轮 `NSDefaultRunLoopMode`（`distantPast` 截止），无待处理事件时立即返回。
/// 与 Windows 后端「宿主消息泵天然覆盖」不同，macOS 的 delegate 消息由主运行循环递送，
/// 宿主每帧调用 `NotificationCenter::pump_events()` 才能保证点击回调及时到达。
auto macos_pump_notifications() -> void;

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_MACOS
