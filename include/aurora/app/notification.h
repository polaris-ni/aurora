#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "aurora/core/result.h"

namespace aurora {

/// @brief 通知的紧急度（同时影响是否需要打扰用户）。
/// 平台映射：Windows 走「是否静音」/「是否遵守专注时段」两级；Linux 走 freedesktop
/// `urgency` 提示（Low=0 / Normal=1 / Critical=2）。
/// @note Thread: n/a（纯值类型）
/// @note Side-effects: none
/// @note Rebuildable: no
enum class NotificationUrgency : std::uint8_t {
    Low,  ///< 低： informational 级，Windows 下不出声，XDG 下 `urgency=0`
    Normal,  ///< 常规（默认）：普通提醒，XDG 下 `urgency=1`
    Critical,  ///< 紧急：不受专注时段抑制，XDG 下 `urgency=2`（服务器可能延长驻留时间）
};

/// @brief 一条系统通知的描述（纯值类型，可自由拷贝）。
///
/// 字段语义：`tag` 既是**去重/替换标识**（同一 tag 的后续通知在支持去重的服务器上替换前一条），
/// 也是**激活回调回传的键** —— 用户点击通知时 `NotificationCenter` 注册的回调收到的是这个 tag，
/// 便于调用方把「被点击」映射回自己的业务对象。
/// @note Thread: n/a（纯值类型）
/// @note Side-effects: none
/// @note Rebuildable: no
struct Notification {
    std::string title;  ///< 标题（可为空串：部分桌面会以应用名兜底，其余照原样展示空标题）
    std::string body;  ///< 正文（可为空串）
    std::string tag;  ///< 去重/替换标识，同时是激活回调回传的键；空串由实现层替换为稳定占位键
    /// @brief 紧急度；默认 `Normal`。
    NotificationUrgency urgency = NotificationUrgency::Normal;
    /// @brief 驻留/超时时长（毫秒）。`0` 表示交给平台默认策略，非零按该值尽力逼近。
    std::uint32_t timeout_ms = 0;
};

/// @brief 跨平台系统通知入口（win32 气球 / XDG 桌面通知）。
///
/// 各平台后端（全部为**运行时**探测，无构建期依赖，也不新增 `AURORA_ENABLE_*` 开关）：
///
/// | 平台 | 实现 | 激活回调 |
/// |:---|:---|:---|
/// | Windows | `Shell_NotifyIconW` 按需添加隐藏托盘图标 + `NIF_INFO` 气球；超时/关闭/点击后 `NIM_DELETE` 撤掉图标 | 支持（走 `NIN_BALLOONUSERCLICK`） |
/// | Linux | ① `dlopen("libnotify.so.4")` ② `dlopen("libdbus-1.so.3")` 手写 `org.freedesktop.Notifications.Notify` ③ `popen("notify-send")` 兜底 | ①② 支持（需宿主排空，见 `pump_events`）；③ 不支持 |
/// | Headless 回退 | 仅记录 `last_notification()`，`notify()` 返回成功 | 不支持 |
/// | macOS / 其它 | 本轮不支持，`notify()` 返回结构化错误 | 不支持 |
///
/// **失败口径**：一切失败统一为 `NotificationPostFailed`（`ErrorParams{{"detail", ...}}`，
/// 永久失败、不可重试语义），调用方可据此退化为自己 UI 内的提示条；不抛异常、不静默 no-op。
/// Linux 三层降级链**全部**缺失时才返回该错误。
///
/// @note Thread: main-thread only（`notify` / `set_on_notification_activated` / `emit_activated` /
/// `pump_events` 均在调用线程直行；内部记录区有互斥保护，仅供跨线程读取 `last_notification`）
/// @note Side-effects: posts a visual OS notification (interacts with the desktop shell)
/// @note Rebuildable: no
/// @note `Application::notify()` 与 `Application::set_on_notification_activated()` 是本类的转发入口：
/// 二者只转调这里的静态方法，不复制任何平台逻辑；本头自洽、不依赖 `application.h`，可单独包含。
class NotificationCenter {
  public:
    /// @brief 发送一条系统通知。先把本次内容记入 `last_notification()`，再投递到平台后端。
    /// @param notification 通知内容（标题/正文/tag/紧急度/超时）。
    /// @return 成功为 `Ok`；本平台无可用后端或投递失败为 `NotificationPostFailed`
    /// （detail 指出具体原因，如缺失哪个库、哪个系统调用失败）。
    [[nodiscard]] static auto notify(const Notification &notification) -> Result<void>;

    /// @brief 注册通知被用户激活（点击）时的回调。
    /// @param callback 回调；形参是 `Notification::tag`（空 tag 的通知回传实现层分配的稳定占位键）。
    ///        传空回调即注销既有回调。
    /// @note Linux 下回调依赖宿主定期调用 `pump_events()` 排空异步事件；Windows 下依赖宿主的消息泵
    ///       抽取隐藏窗口消息。**宿主不排空 ⇒ 通知照常显示、回调不到**（不会虚假成功）。
    static auto set_on_notification_activated(std::function<void(std::string tag)> callback) -> void;

    /// @brief 读取最近一次「试图投递」的通知内容（平台投递是否成功不影响本记录）。
    /// @return 最近一次通知；从未调用过 `notify()` 时为 `std::nullopt`。
    /// @note Headless 与自动化测试的主观测面：本入口在**所有**平台上维护（含投递失败的调用），
    ///       让无桌面环境也能断言「请求了什么」。
    [[nodiscard]] static auto last_notification() -> std::optional<Notification>;

    /// @brief 清空 `last_notification()` 记录（回到「从未通知过」的初始态）。
    static auto clear_last_notification() -> void;

    /// @brief 排空平台异步事件，把「通知被点击」转成已注册的激活回调（无回调/无待处理事件时几乎零开销）。
    /// @note Windows 下依附宿主消息泵，天然被 `PeekMessage` 覆盖，本入口在那里是 no-op；
    ///       Linux 下必须靠它驱动 GLib 主循环迭代 / libdbus 套接字读取 —— 建议宿主每帧调用一次
    ///       （demo 已按此接线；`Application` 若做帧循环集成，同样每帧一次即可）。
    static auto pump_events() -> void;

    /// @brief 平台后端 / 测试接缝：把一次激活事件（用户点击通知）投递给已注册的回调。
    /// @param tag 被点击通知的 `Notification::tag`。
    /// @note **应用代码不应调用**：它由平台后端在真正的用户点击到达时调用，暴露出来是为了让
    ///       平台后端代码（可能分布在不同编译单元）与单元测试有一个统一的进入点。
    ///       未注册回调时静默返回。
    static auto emit_activated(const std::string &tag) -> void;

    /// @brief 安装「仅记录」后端：此后 `notify()` 只更新 `last_notification()`，不触达任何系统通知服务。
    /// @return 安装是否生效（恒为 `true`；保留返回值以对齐 `Clipboard::install_test_backend` 的注入面形态）。
    /// @note 给 Headless 场景与单元测试用：装了它，单测可以在装有真实桌面的开发机上验证字段
    ///       往返而不真的弹出通知。进程级开关，卸载用 `remove_recording_backend()`。
    [[nodiscard]] static auto install_recording_backend() -> bool;

    /// @brief 卸载「仅记录」后端，恢复平台实现。
    /// @return 是否确有后端被卸载（未安装时为 `false`）。
    [[nodiscard]] static auto remove_recording_backend() -> bool;
};

}  // namespace aurora
