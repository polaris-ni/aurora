/// 测试类型: unit
/// 目标单元: include/aurora/app/notification.h
/// 测试说明: 经「仅记录」注入后端覆盖 notify() 的字段往返（title/body/tag/urgency/timeout 逐项）、
/// 空标题与空正文的边界、激活回调注册/注销与触发时回传正确 tag、以及不支持平台上
/// 返回结构化错误而非崩溃。全程不弹真实系统通知：装了记录后端后 notify() 不触达桌面通知服务。

#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "aurora/app/notification.h"
#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_notification {

namespace {

/// @brief 进入「仅记录」模式：清空历史并把 notification tag 的观测面复位，让用例互不污染。
auto enter_recording_mode() -> void {
    NotificationCenter::clear_last_notification();
    NotificationCenter::set_on_notification_activated(nullptr);
    if (!NotificationCenter::install_recording_backend()) {
        AURORA_TEST_SKIP("the record-only notification backend could not be installed");
    }
}

/// @brief 退出记录模式，避免在后续用例里留下「不投递」的假象。
auto leave_recording_mode() -> void { (void)NotificationCenter::remove_recording_backend(); }

/// @brief 取出最近一次记录；没有记录时本用例致命失败（比 opt-> 更利于静态分析）。
[[nodiscard]] auto require_recorded() -> Notification {
    return ::aurora::testing::require_value(NotificationCenter::last_notification());
}

/// @brief 构造一条各字段取值互不相同的通知（便于逐字段断言，避免错部分抵消）。
[[nodiscard]] auto sample_notification() -> Notification {
    Notification n;
    n.title = "Build finished";
    n.body = "All 42 targets are up to date";
    n.tag = "build-finished";
    n.urgency = NotificationUrgency::Critical;
    n.timeout_ms = 4500;
    return n;
}

}  // namespace

AURORA_TEST_CASE(notify_succeeds_and_records_every_field) {
    enter_recording_mode();

    const Notification sent = sample_notification();
    AURORA_TEST_REQUIRE(NotificationCenter::notify(sent).ok());

    const Notification recorded = require_recorded();
    AURORA_TEST_CHECK_EQ(recorded.title, std::string{"Build finished"});
    AURORA_TEST_CHECK_EQ(recorded.body, std::string{"All 42 targets are up to date"});
    AURORA_TEST_CHECK_EQ(recorded.tag, std::string{"build-finished"});
    AURORA_TEST_CHECK(recorded.urgency == NotificationUrgency::Critical);
    AURORA_TEST_CHECK_EQ(recorded.timeout_ms, 4500U);

    leave_recording_mode();
}

AURORA_TEST_CASE(last_notification_starts_empty_and_can_be_cleared) {
    enter_recording_mode();
    NotificationCenter::clear_last_notification();
    AURORA_TEST_CHECK_FALSE(NotificationCenter::last_notification().has_value());

    AURORA_TEST_REQUIRE(NotificationCenter::notify(sample_notification()).ok());
    AURORA_TEST_CHECK_TRUE(NotificationCenter::last_notification().has_value());

    NotificationCenter::clear_last_notification();
    AURORA_TEST_CHECK_FALSE(NotificationCenter::last_notification().has_value());

    leave_recording_mode();
}

AURORA_TEST_CASE(notify_overwrites_the_previous_record) {
    enter_recording_mode();

    Notification first;
    first.title = "first";
    first.tag = "first";
    AURORA_TEST_REQUIRE(NotificationCenter::notify(first).ok());

    Notification second;
    second.title = "second";
    second.tag = "second";
    second.urgency = NotificationUrgency::Low;
    AURORA_TEST_REQUIRE(NotificationCenter::notify(second).ok());

    const Notification recorded = require_recorded();
    AURORA_TEST_CHECK_EQ(recorded.title, std::string{"second"});
    AURORA_TEST_CHECK_EQ(recorded.tag, std::string{"second"});
    AURORA_TEST_CHECK(recorded.urgency == NotificationUrgency::Low);

    leave_recording_mode();
}

AURORA_TEST_CASE(empty_title_and_body_are_accepted_as_is) {
    enter_recording_mode();

    // 边界：空标题在本 API 语义下是合法的「只有正文甚至都没有正文」的通知 —— 既不报错，
    // 也不被悄悄改写（平台侧要不要用应用名兜底是平台的事，本层如实记录调用方的请求）。
    Notification blank;
    blank.tag = "silent";
    auto result = NotificationCenter::notify(blank);
    AURORA_TEST_REQUIRE(result.ok());

    const Notification recorded = require_recorded();
    AURORA_TEST_CHECK_EQ(recorded.title, std::string{});
    AURORA_TEST_CHECK_EQ(recorded.body, std::string{});
    AURORA_TEST_CHECK_EQ(recorded.tag, std::string{"silent"});
    AURORA_TEST_CHECK(recorded.urgency == NotificationUrgency::Normal);  // 默认紧急度
    AURORA_TEST_CHECK_EQ(recorded.timeout_ms, 0U);  // 默认超时：0 = 交给平台策略

    // 空 tag 同样是合法取值（实现层内部把它换成占位动作键，但记录面保持调用方原样）。
    Notification untagged;
    untagged.title = "no tag";
    result = NotificationCenter::notify(untagged);
    AURORA_TEST_REQUIRE(result.ok());
    AURORA_TEST_CHECK_EQ(require_recorded().tag, std::string{});

    leave_recording_mode();
}

AURORA_TEST_CASE(activated_callback_receives_the_notification_tag) {
    enter_recording_mode();

    std::string received;
    int calls = 0;
    NotificationCenter::set_on_notification_activated([&received, &calls](std::string tag) -> void {
        ++calls;
        received = std::move(tag);
    });

    // 平台后端 / 测试接缝：把一次真实的用户点击折算成回调投递。
    NotificationCenter::emit_activated("download-42");
    AURORA_TEST_CHECK_EQ(calls, 1);
    AURORA_TEST_CHECK_EQ(received, std::string{"download-42"});

    // 空 tag 也按调用方语义原样回传，而不是泄露实现层的占位动作键。
    NotificationCenter::emit_activated(std::string{});
    AURORA_TEST_CHECK_EQ(calls, 2);
    AURORA_TEST_CHECK_EQ(received, std::string{});

    leave_recording_mode();
}

AURORA_TEST_CASE(unregistering_the_callback_makes_activation_silent) {
    enter_recording_mode();

    int calls = 0;
    NotificationCenter::set_on_notification_activated([&calls](std::string tag) -> void {
        (void)tag;
        ++calls;
    });
    NotificationCenter::emit_activated("once");
    AURORA_TEST_CHECK_EQ(calls, 1);

    NotificationCenter::set_on_notification_activated(nullptr);
    NotificationCenter::emit_activated("twice");
    AURORA_TEST_CHECK_EQ(calls, 1);  // 注销后再有激活事件也不得多触发一次

    leave_recording_mode();
}

AURORA_TEST_CASE(recording_backend_install_and_remove_round_trip) {
    // 先确保退出记录模式，让「未安装时卸载」的负路径可判定。
    (void)NotificationCenter::remove_recording_backend();
    AURORA_TEST_CHECK_FALSE(NotificationCenter::remove_recording_backend());

    AURORA_TEST_CHECK_TRUE(NotificationCenter::install_recording_backend());
    AURORA_TEST_CHECK_TRUE(NotificationCenter::remove_recording_backend());
    AURORA_TEST_CHECK_FALSE(NotificationCenter::remove_recording_backend());
}

AURORA_TEST_CASE(pump_events_is_safe_without_a_backend) {
    // 排空入口在没有待处理事件、也没有已注册回调时必须是 no-op 而非崩溃：宿主帧循环、
    // demo、应用胶水都可能每帧调用它。
    enter_recording_mode();
    NotificationCenter::pump_events();
    NotificationCenter::pump_events();
    NotificationCenter::emit_activated("unhandled");  // 未注册回调时不抛、不崩
    NotificationCenter::clear_last_notification();
    AURORA_TEST_CHECK_FALSE(NotificationCenter::last_notification().has_value());
    leave_recording_mode();
}

// 支持与否都必须「有答案」：不支持时不许假装成功（静默 no-op），更不许崩溃。
// 有真实后端的平台已在上面的记录模式用例里覆盖语义面，这里不真发系统通知。
AURORA_TEST_CASE(unsupported_platform_reports_a_structured_error) {
#if defined(AURORA_PLATFORM_WINDOWS) || (defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID))
    AURORA_TEST_SKIP("this machine has a desktop notification backend; "
                     "posting a real notification belongs to the live probe, not to the unit suite");
#else
    NotificationCenter::clear_last_notification();
    const auto result = NotificationCenter::notify(sample_notification());
    AURORA_TEST_REQUIRE_FALSE(result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::NotificationPostFailed);
    AURORA_TEST_CHECK_EQ(result.error().code, std::string{"notification-post-failed"});
    AURORA_TEST_CHECK_FALSE(result.error().message.empty());
    // 失败也要留痕：调用方据此决定退避还是降级到自己的 UI 提示条。
    AURORA_TEST_CHECK_EQ(require_recorded().tag, std::string{"build-finished"});
#endif
}

}  // namespace aurora::test_cases::utest_notification
