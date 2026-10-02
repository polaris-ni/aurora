/* 系统通知（XDG 桌面通知）—— Linux 真机验收探针（人工触发，不进 CTest）
// ============================================================================
// 为什么必须真机：`src/aurora/app/notification.cpp` 的 Linux 分支把三级后端
// （① libnotify.so.4 → ② libdbus-1.so.3 手写 org.freedesktop.Notifications.Notify
// → ③ popen("notify-send")）全部做成**运行时 dlopen / popen**，构建期不碰任何桌面依赖。
// 于是「本机装了哪几级」「会话总线通不通」「桌面通知服务肯不肯收」这些差别只有真机能分辨；
// 无桌面 / 无 libnotify 的 CI 里这条代码路径根本不会被走到，`notify()` 必定走 unhealthy 侧的
// fallback 或失败。本探针就是这条路径在真实桌面上的机器级物证 + 人工目视段。
//
// 关于「命中了哪一级」：公共 API **没有**暴露当前后端查询口（`Notification::` 与
// `NotificationCenter::` 都没有这类入口），实现层把它藏在 TU 内的进程状态里。故本探针自己
// `dlopen` 探一遍三级可用性，据此算出**预期级**并打印；「实际命中级」只作提示信息，
// 不作断言——降级选择是库的权利，只要有一级成功投递，`notify()` 就该返回 Ok。
// 已知语义落差：③ `notify-send` 是即发即忘，**不支持激活回调**（无需也无法注册动作），
// 因此「点击回调」相关判据在该级下落 degradez 为 WARN 而非 FAIL（见自动段 ⑧与人工段 b）。
//
// 自动段逐项验收（不需要人工看屏幕）：
//   ① 环境核对：`libnotify.so.4`（含 notify_init / notify_notification_new /
//      notify_notification_show 三符号）、`libdbus-1.so.3`（含 dbus_bus_get /
//      dbus_message_new_method_call / dbus_connection_send_with_reply_and_block）、
//      `notify-send`（PATH 查询）逐项打印；三级全缺 ⇒ 打印 SKIP 说明并以退出码 2 结束
//      （环境不可用，不算失败）。会话总线是否可得另外单列一行，只作诊断，不作判据
//      （拿不到会话总线时 ①仍可能成功，②必然降级到 ③）。
//   ② 全字段投递：title / body / tag / urgency=Normal / timeout_ms 均非默认值，
//      `notify()` 返回 Ok；失败时打印结构化错误的 code 与 message。
//   ③ `last_notification()` 往返：投递后读回，五个字段与请求逐字段相等。
//   ④ 紧急度三档：Low / Normal / Critical 各投递一次，各自返回 Ok。某档失败时按
//      「环境侧限制」输出 FAIL 并附上 manifest detail 交由人工判断（服务器有权按策略拒某种 hint）。
//   ⑤ `clear_last_notification()` 后 `last_notification() == std::nullopt`。
//   ⑥ tag 去重语义：同一 tag 连投两条不同 body，两次都返回 Ok（去重是**服务器端**替换行为，
//      本层只保证不报错、且记录面落在第二条上）。
//   ⑦ 记录后端注入面：`install_recording_backend()` ⇒ 期间 `notify()` 仍 Ok 且
//      `last_notification()` 有值 ⇒ `remove_recording_backend()` 返回 true ⇒
//      再 `remove_recording_backend()` 返回 false（幂等卸载）。
//   ⑧ 激活回调注入面：注册回调 ⇒ `emit_activated("...")` ⇒ 回调恰好收到该 tag；
//      传空 `std::function` 注销后 `emit_activated()` 不再触发。
//   ⑨ `pump_events()` 反复调用（无回调 / 无待处理事件时）不崩、不卡死。
// 人工段（`--interactive`，需人眼看屏幕上的通知气泡）：
//   a. 依次弹 Low / Normal / Critical 三档（各带不同 title / body / tag，超时交给桌面默认策略，
//      以便单独观察紧急度对驻留时长与打扰行为的影响）⇒ 请操作者确认三档表现确有差异。
//   b. 弹一条并请操作者**点击它**，程序在超时窗口内反复 `pump_events()`；收到激活回调即 PASS，
//      超时则输出 WARN（不是 FAIL：降级到 notify-send 级时该级本就不支持回调）。
//
// 退出码：
//   0  自动段全部通过（WARN 不影响退出码）
//   1  自动段存在 FAIL（含第 ④项的「环境侧限制」失败）
//   2  环境不可用：三级后端全缺失 —— 属合法降级下的「无从验收」
//
// 构建（目标由 cmake/AuroraVerify.cmake 注册；需 -DAURORA_BUILD_VERIFY_TOOLS=ON）：
//   cmake -S . -B build-verify -G Ninja -DCMAKE_BUILD_TYPE=Release \
//         -DAURORA_BUILD_VERIFY_TOOLS=ON -DAURORA_BACKEND_X11=ON
//   cmake --build build-verify --target aurora_verify_linux_notification
//   ./build-verify/aurora_verify_linux_notification                     # 自动段
//   ./build-verify/aurora_verify_linux_notification --interactive       # + 人工段
//
// 注：本文件在 Linux 桌面会话中运行会真实弹出系统通知（属被测行为本身），退出不残留图标。
//   构建命令中的行尾反斜杠为续行符，故本头注释整体使用块注释形态（避免 -Wcomment）。
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#if !defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_ANDROID)
#error "aurora_verify_linux_notification can only be built on desktop Linux"
#endif

#include <dlfcn.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "aurora/app/notification.h"
#include "verify_args.h"

namespace {

/// @brief 人工等待 `pump_events()` 的预算（毫秒）；超时视为「没收到点击」。
constexpr long k_click_budget_ms = 60000;
/// @brief 人工段每次 `pump_events()` 之间的间隔（毫秒）。
constexpr long k_click_poll_ms = 50;
/// @brief 三档紧急度之间留给桌面服务器的间隔（毫秒），避免相互覆盖。
constexpr long k_urgency_gap_ms = 1200;

int failures = 0;
int warnings = 0;

/// @brief 写一行探针输出到 stdout（走 raw 通道：无前缀、不过级别过滤）。
/// @param text 待输出的一行文本。
auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

/// @brief 记一项自动判据结论。
/// @param ok 判据是否成立。
/// @param label 结论文案（英文，ASCII）。
auto check(bool ok, const std::string &label) -> void {
    emit(std::string("[") + (ok ? "PASS" : "FAIL") + "] " + label);
    if (!ok) {
        ++failures;
    }
}

/// @brief 记一条不判失败的提示（ allergies 差异由真机环境决定，人工复核）。
/// @param label 提示文案（英文，ASCII）。
auto warn(const std::string &label) -> void {
    emit("[WARN] " + label);
    ++warnings;
}

/// @brief 记一条跳过说明。
/// @param label 说明文案。
auto skip(const std::string &label) -> void { emit("[SKIP] " + label); }

/// @brief 休眠指定毫秒（给桌面服务器与GLib 主循环留出处理时间）。
/// @param ms 休眠时长（毫秒）。
auto nap_ms(long ms) -> void { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

/// @brief 库能否被 `dlopen` 打开（只看加载，不解析符号）。
/// @param soname 目标库的 soname。
/// @return 打开成功为真。
[[nodiscard]] auto probe_library(const char *soname) -> bool {
    void *handle = dlopen(soname, RTLD_LAZY | RTLD_LOCAL);
    if (handle == nullptr) {
        return false;
    }
    dlclose(handle);
    return true;
}

/// @brief 一组符号是否齐备：少任何一个，整级后端在库侧就会降级。
/// @param soname 目标库的 soname。
/// @param symbols 关键符号名数组。
/// @param count 符号个数。
/// @return 全部解析到地址为真。
[[nodiscard]] auto probe_symbols(const char *soname, const char *const *symbols, std::size_t count) -> bool {
    void *handle = dlopen(soname, RTLD_LAZY | RTLD_LOCAL);
    if (handle == nullptr) {
        return false;
    }
    bool complete = true;
    for (std::size_t i = 0; i < count; ++i) {
        void *address = dlsym(handle, symbols[i]);
        if (address == nullptr) {
            emit(std::string("       ") + symbols[i] + " is missing from " + soname);
            complete = false;
        }
    }
    dlclose(handle);
    return complete;
}

/// @brief 外壳命令是否在 PATH 里（`command -v` 避开别名与 shell 内建干扰）。
/// @param name 命令名。
/// @return 命令可解析为真。
[[nodiscard]] auto probe_command(const char *name) -> bool {
    // 命令串是常量拼接，只做 PATH 查询，不含外部输入。
    const std::string command = std::string("command -v ") + name + " >/dev/null 2>&1";
    // NOLINTNEXTLINE(cert-env33, bugprone-command-processor)
    return std::system(command.c_str()) == 0;
}

/// @brief 会话总线是否可得：`dbus_bus_get(DBUS_BUS_SESSION)` 有没有返回连接。
/// @return 拿到连接为真；库或符号缺失时为 false。
/// @note 这是**诊断**用第二级探针：它失败不代表整体降级，因为第一级 libnotify 自带 GLib 会话总线接线。
[[nodiscard]] auto probe_session_bus() -> bool {
    void *handle = dlopen("libdbus-1.so.3", RTLD_LAZY | RTLD_LOCAL);
    if (handle == nullptr) {
        return false;
    }
    void *raw = dlsym(handle, "dbus_bus_get");
    if (raw == nullptr) {
        dlclose(handle);
        return false;
    }
    // dlsym 只能返回 void*，转为函数指针没有第二条路。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto bus_get = reinterpret_cast<void *(*)(int, void *)>(raw);
    // DBusError 是一片不透明栈空间：64 字节足以容纳任何版本（本探针不读其内容）。
    alignas(std::uint64_t) unsigned char error_storage[64] = {};
    void *connection = bus_get(0 /* DBUS_BUS_SESSION */, error_storage);
    dlclose(handle);
    return connection != nullptr;
}

/// @brief 紧急度的打印名（用于逐行的 tier / 三档判据文案）。
/// @param urgency 待命名的紧急度。
/// @return 稳定的 ASCII 名字。
[[nodiscard]] auto urgency_name(aurora::NotificationUrgency urgency) -> std::string {
    switch (urgency) {
        case aurora::NotificationUrgency::Low:
            return "Low";
        case aurora::NotificationUrgency::Normal:
            return "Normal";
        case aurora::NotificationUrgency::Critical:
            return "Critical";
    }
    return "unknown";
}

/// @brief 投递一条通知；失败时把结构化错误的 code 与 message 逐行打印出来。
/// @param notification 待投递的通知。
/// @return 失败原因（成功时为空串），便于调用方把 detail 附进判据文案。
[[nodiscard]] auto post_and_report(const aurora::Notification &notification) -> std::string {
    const aurora::Result<void> result = aurora::NotificationCenter::notify(notification);
    if (result.ok()) {
        return std::string{};
    }
    emit(std::string("       code    : ") + result.error().code);
    emit(std::string("       message : ") + result.error().message);
    return result.error().message;
}

/// @brief 逐字段比对记录面与请求的一致性，并在不一致时打印具体字段。
/// @param expected 交给 `notify()` 的请求。
/// @param actual `last_notification()` 读回的记录。
/// @return 五个字段全等为真。
[[nodiscard]] auto fields_match(const aurora::Notification &expected, const aurora::Notification &actual) -> bool {
    bool same = true;
    if (actual.title != expected.title) {
        emit("       title mismatch: expected \"" + expected.title + "\", got \"" + actual.title + "\"");
        same = false;
    }
    if (actual.body != expected.body) {
        emit("       body mismatch: expected \"" + expected.body + "\", got \"" + actual.body + "\"");
        same = false;
    }
    if (actual.tag != expected.tag) {
        emit("       tag mismatch: expected \"" + expected.tag + "\", got \"" + actual.tag + "\"");
        same = false;
    }
    if (actual.urgency != expected.urgency) {
        emit(std::string("       urgency mismatch: expected ") + urgency_name(expected.urgency) + ", got " +
             urgency_name(actual.urgency));
        same = false;
    }
    if (actual.timeout_ms != expected.timeout_ms) {
        emit(std::string("       timeout_ms mismatch: expected ") + std::to_string(expected.timeout_ms) + ", got " +
             std::to_string(actual.timeout_ms));
        same = false;
    }
    return same;
}

}  // namespace

auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("Linux XDG desktop notification live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");

    emit("== Aurora Linux desktop notification live probe ==");
    emit("auto segment: env tiers / full-field post / last_notification round trip / three urgencies / clear / "
         "tag dedup / recording backend / activation injection / pump_events");
    if (interactive) {
        emit("interactive segment: three urgencies on screen / click-activation");
    }

    // ---- ① 环境核对：三级后端逐项探测 ----
    emit("");
    emit("[ENV] tier availability (the library picks one of them at runtime):");

    bool libnotify_usable = false;
    if (probe_library("libnotify.so.4")) {
        static constexpr const char *k_notify_symbols[] = {"notify_init", "notify_notification_new",
                                                          "notify_notification_show"};
        libnotify_usable = probe_symbols("libnotify.so.4", k_notify_symbols, 3);
        emit(std::string("       tier 1 libnotify.so.4   : ") + (libnotify_usable ? "usable" : "incomplete symbols"));
    } else {
        emit("       tier 1 libnotify.so.4   : absent");
    }

    bool libdbus_usable = false;
    if (probe_library("libdbus-1.so.3")) {
        static constexpr const char *k_dbus_symbols[] = {"dbus_bus_get", "dbus_message_new_method_call",
                                                        "dbus_connection_send_with_reply_and_block"};
        libdbus_usable = probe_symbols("libdbus-1.so.3", k_dbus_symbols, 3);
        emit(std::string("       tier 2 libdbus-1.so.3   : ") + (libdbus_usable ? "usable" : "incomplete symbols"));
    } else {
        emit("       tier 2 libdbus-1.so.3   : absent");
    }

    const bool has_notify_send = probe_command("notify-send");
    emit(std::string("       tier 3 notify-send      : ") + (has_notify_send ? "on PATH" : "absent"));

    const char *bus_address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    emit(std::string("       DBUS_SESSION_BUS_ADDRESS: ") + ((bus_address != nullptr) ? bus_address : "(unset)"));
    emit(std::string("       dbus_bus_get(SESSION)   : ") + (probe_session_bus() ? "connected" : "no connection"));

    if (!libnotify_usable && !libdbus_usable && !has_notify_send) {
        emit("[SKIP] none of the three tiers is present on this machine "
             "(libnotify.so.4, libdbus-1.so.3 and notify-send are all unavailable)");
        emit("[SKIP] NotificationCenter::notify() must then return NotificationPostFailed: "
             "there is nothing here to accept a desktop notification");
        emit("result: ENV-UNAVAILABLE (exit 2)");
        return 2;
    }

    const std::string expected_tier =
        libnotify_usable ? "tier 1 (libnotify)" : (libdbus_usable ? "tier 2 (raw libdbus)" : "tier 3 (notify-send)");
    emit("[HINT] expected tier on this machine: " + expected_tier +
         " (informational only; the library decides at runtime, the probe does not assert it)");
    emit("[HINT] tier 3 is fire-and-forget and cannot register an action, so click-activation may legitimately "
         "never arrive (reported as WARN, not FAIL)");

    // ---- ② 全字段投递 ----
    emit("");
    aurora::Notification request;
    request.title = "Aurora notification live probe";
    request.body = "Full-field delivery from aurora_verify_linux_notification";
    request.tag = "aurora-verify-fullfield";
    request.urgency = aurora::NotificationUrgency::Normal;
    request.timeout_ms = 5000;
    emit("[POST] notify() with every field set (a notification should appear on screen)");
    const std::string full_field_failure = post_and_report(request);
    check(full_field_failure.empty(), full_field_failure.empty()
                                          ? "full-field notify() returns Ok"
                                          : std::string("full-field notify() returns Ok (detail: ") +
                                                full_field_failure + ")");

    // ---- ③ last_notification() 往返 ----
    const std::optional<aurora::Notification> recorded = aurora::NotificationCenter::last_notification();
    check(recorded.has_value() && fields_match(request, *recorded),
          "last_notification() round-trips every field of the request");

    // ---- ④ 紧急度三档 ----
    constexpr aurora::NotificationUrgency k_urgencies[] = {aurora::NotificationUrgency::Low,
                                                           aurora::NotificationUrgency::Normal,
                                                           aurora::NotificationUrgency::Critical};
    for (const aurora::NotificationUrgency urgency : k_urgencies) {
        aurora::Notification nudge = request;
        nudge.tag = std::string("aurora-verify-urgency-") + urgency_name(urgency);
        nudge.body = std::string("Urgency ") + urgency_name(urgency) + " by aurora_verify_linux_notification";
        nudge.urgency = urgency;
        const std::string failure = post_and_report(nudge);
        check(failure.empty(), failure.empty()
                                   ? (std::string("urgency ") + urgency_name(urgency) + " delivers Ok")
                                   : (std::string("urgency ") + urgency_name(urgency) +
                                      " delivers Ok (server-side limit, detail: " + failure + ")"));
    }

    // ---- ⑤ clear_last_notification() ----
    aurora::NotificationCenter::clear_last_notification();
    check(!aurora::NotificationCenter::last_notification().has_value(),
          "clear_last_notification() resets last_notification() to nullopt");

    // ---- ⑥ tag 去重语义（本层只保证不报错；替换与否由服务器决定） ----
    {
        aurora::Notification first = request;
        first.tag = "aurora-verify-dedup";
        first.body = "first body with the dedup tag";
        aurora::Notification second = first;
        second.body = "second body with the same dedup tag";
        const bool first_ok = post_and_report(first).empty();
        const bool second_ok = post_and_report(second).empty();
        check(first_ok && second_ok, "same tag posted twice with different bodies: both notify() calls return Ok");
        const std::optional<aurora::Notification> dedup = aurora::NotificationCenter::last_notification();
        check(dedup.has_value() && fields_match(second, *dedup),
              "the recording surface keeps the latest request (second body wins)");
    }

    // ---- ⑦ 记录后端注入面 ----
    {
        static_cast<void>(aurora::NotificationCenter::remove_recording_backend());  // 基线：先确保未安装
        check(aurora::NotificationCenter::install_recording_backend(), "install_recording_backend() installs");
        aurora::Notification muted = request;
        muted.tag = "aurora-verify-recording";
        const bool muted_ok = post_and_report(muted).empty();
        const bool recorded_anyway = aurora::NotificationCenter::last_notification().has_value();
        check(muted_ok, "notify() returns Ok while the record-only backend is installed");
        check(recorded_anyway, "last_notification() is still populated in record-only mode");
        check(aurora::NotificationCenter::remove_recording_backend(), "remove_recording_backend() uninstalls once");
        check(!aurora::NotificationCenter::remove_recording_backend(),
              "remove_recording_backend() returns false a second time (idempotent)");
    }

    // ---- ⑧ 激活回调注入面 ----
    {
        int calls = 0;
        std::string received;
        aurora::NotificationCenter::set_on_notification_activated([&calls, &received](std::string tag) -> void {
            ++calls;
            received = std::move(tag);
        });
        aurora::NotificationCenter::emit_activated("aurora-verify-activated");
        check(calls == 1 && received == "aurora-verify-activated",
              std::string("emit_activated() reaches the registered callback once with the exact tag (calls=") +
                  std::to_string(calls) + ", tag=\"" + received + "\")");

        aurora::NotificationCenter::set_on_notification_activated(nullptr);
        aurora::NotificationCenter::emit_activated("aurora-verify-activated");
        check(calls == 1, "after unregistering the callback, emit_activated() no longer triggers it");
    }

    // ---- ⑨ pump_events() 可反复调用 ----
    {
        aurora::NotificationCenter::clear_last_notification();
        for (int i = 0; i < 20; ++i) {
            aurora::NotificationCenter::pump_events();
        }
        check(!aurora::NotificationCenter::last_notification().has_value(),
              "pump_events() called 20 times with no callback and no pending event: no crash, no hang");
    }

    // ---- 人工段 ----
    if (interactive) {
        emit("");
        emit("[MANUAL] a. three urgencies are about to appear; please compare how long each stays on screen and "
             "whether it interrupts you (dwell time and interruption are server policy, driven by the urgency hint)");
        for (const aurora::NotificationUrgency urgency : k_urgencies) {
            aurora::Notification shown;
            shown.title = std::string("Aurora ") + urgency_name(urgency) + " notification";
            shown.body = std::string("Manual stage: urgency ") + urgency_name(urgency) +
                         " (posted by aurora_verify_linux_notification)";
            shown.tag = std::string("aurora-verify-manual-") + urgency_name(urgency);
            shown.urgency = urgency;
            shown.timeout_ms = 0;  // 交给桌面默认策略：单独观察紧急度本身的影响
            const std::string failure = post_and_report(shown);
            if (!failure.empty()) {
                warn(std::string("manual urgency ") + urgency_name(urgency) + " was rejected by the server: " + failure);
            }
            aurora::NotificationCenter::pump_events();
            nap_ms(k_urgency_gap_ms);
        }
        emit("[MANUAL] did the three urgencies differ on screen (dwell time / interruption)? If they all looked "
             "identical, this desktop server ignores the urgency hint — note it in the acceptance record.");

        emit("");
        emit("[MANUAL] b. one more notification is about to appear; please CLICK IT within " +
             std::to_string(k_click_budget_ms / 1000) + " s");
        int clicks = 0;
        std::string clicked_tag;
        aurora::NotificationCenter::set_on_notification_activated([&clicks, &clicked_tag](std::string tag) -> void {
            ++clicks;
            clicked_tag = std::move(tag);
        });
        aurora::Notification clickable;
        clickable.title = "Aurora click me";
        clickable.body = "Click this notification (manual stage b)";
        clickable.tag = "aurora-verify-click";
        clickable.urgency = aurora::NotificationUrgency::Normal;
        clickable.timeout_ms = 0;
        const std::string failure = post_and_report(clickable);
        if (!failure.empty()) {
            warn(std::string("manual click target could not be posted: ") + failure);
        }

        for (long elapsed = 0; (elapsed < k_click_budget_ms) && (clicks == 0); elapsed += k_click_poll_ms) {
            aurora::NotificationCenter::pump_events();
            nap_ms(k_click_poll_ms);
        }
        if (clicks == 0) {
            warn("no activation arrived within " + std::to_string(k_click_budget_ms / 1000) +
                 "s: nobody clicked, or the current degradation is the notify-send tier, which cannot register "
                 "an action at all - not a failure of the library");
        } else {
            check(clicked_tag == clickable.tag, std::string("clicking the notification fired the activation callback "
                                                           "with the exact tag (got \"") +
                                                    clicked_tag + "\")");
        }
        aurora::NotificationCenter::set_on_notification_activated(nullptr);
    }

    emit("");
    if (failures > 0) {
        emit("result: " + std::to_string(failures) + " FAILURE(S), " + std::to_string(warnings) + " warning(s) (exit 1)");
        return 1;
    }
    emit("result: ALL PASS, " + std::to_string(warnings) + " warning(s) (exit 0)");
    return 0;
}
