/// 测试类型: unit
/// 目标单元: include/aurora/net/observability.h + include/aurora/net/http_observability.h 与其实现
/// 测试说明: 凭据分级与呈现、URL 折叠、活动存储的生命周期 / 容量 / 预算、事件总线的多路消费与自动退订、
///           时间线查询与阶段令牌。脱敏断言一律以「原值不出现 + 折叠形态出现」成对书写，避免只证明「变了」；
///           容量与预算断言同时钉住计数器（dropped / truncated）与「迟到 end 不复活记录」两条口径。
///           发起侧的阶段打点在真实交换用例里覆盖；本件只钉存储与总线的契约。
/// 覆盖说明: `AURORA_ENABLE_DEBUG` 关时存储与总线整体不存在，本件在该构建下只断言确定形态；两种构建同名
///           用例由 `activity_surface_follows_build_flavour` 分流。

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/url.h"
#include "aurora/net/detail/observability_trace.h"
#include "aurora/net/http_observability.h"
#include "aurora/net/observability.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_observability {
namespace {

using aurora::Url;
using aurora::net::ExportPolicy;
using aurora::net::ExportSurface;
using aurora::net::FieldClass;
using aurora::net::install_export_policy;
using aurora::net::present_field;
using aurora::net::present_url;
using aurora::net::http::activity_options;
using aurora::net::http::activity_snapshot;
using aurora::net::http::ActivityOptions;
using aurora::net::http::clear_network_activity;
using aurora::net::http::ExchangeTrace;
using aurora::net::http::http_stage_text;
using aurora::net::http::HttpActivityEntry;
using aurora::net::http::HttpErrorClass;
using aurora::net::http::HttpStage;
using aurora::net::http::NetEventSink;
using aurora::net::http::set_activity_options;
using aurora::net::http::StageMark;
using aurora::net::http::subscribe_events;
using aurora::net::http::timeline_of;
using aurora::net::http::timelines_of;
using aurora::testing::require_value;

/// @brief 每例前后把进程级存储与策略清回缺省，避免用例间串味。
struct ProcessReset {
    ProcessReset() { reset(); }
    ~ProcessReset() { reset(); }
    ProcessReset(const ProcessReset &) = delete;
    auto operator=(const ProcessReset &) -> ProcessReset & = delete;
    ProcessReset(ProcessReset &&) = delete;
    auto operator=(ProcessReset &&) -> ProcessReset & = delete;

  private:
    static auto reset() -> void {
        install_export_policy(nullptr);
        clear_network_activity();
        set_activity_options(ActivityOptions{});
    }
};

/// @brief 身份策略：对一切放行、原样返回（用来证明 Secret 不可被放大）。
class IdentityPolicy final : public ExportPolicy {
  public:
    [[nodiscard]] auto allows([[maybe_unused]] ExportSurface surface, [[maybe_unused]] std::string_view key,
                              [[maybe_unused]] FieldClass field_class) const -> bool override {
        return true;
    }
    [[nodiscard]] auto present([[maybe_unused]] ExportSurface surface, [[maybe_unused]] std::string_view key,
                               [[maybe_unused]] FieldClass field_class, std::string_view value) const
        -> std::string override {
        return std::string{value};
    }
};

/// @brief 收窄策略：Public 也改写，用以证明宿主可在 `present()` 里对 Public 再收窄。
class NarrowingPolicy final : public ExportPolicy {
  public:
    [[nodiscard]] auto allows([[maybe_unused]] ExportSurface surface, [[maybe_unused]] std::string_view key,
                              [[maybe_unused]] FieldClass field_class) const -> bool override {
        return false;
    }
    [[nodiscard]] auto present([[maybe_unused]] ExportSurface surface, [[maybe_unused]] std::string_view key,
                               [[maybe_unused]] FieldClass field_class, [[maybe_unused]] std::string_view value) const
        -> std::string override {
        return std::string{"<hidden>"};
    }
};

/// @brief 抛异常策略：呈现阶段抛出时库须回退到折叠形态且不把异常带出来。
class ThrowingPolicy final : public ExportPolicy {
  public:
    [[nodiscard]] auto allows([[maybe_unused]] ExportSurface surface, [[maybe_unused]] std::string_view key,
                              [[maybe_unused]] FieldClass field_class) const -> bool override {
        return true;
    }
    [[nodiscard]] auto present([[maybe_unused]] ExportSurface surface, [[maybe_unused]] std::string_view key,
                               [[maybe_unused]] FieldClass field_class, [[maybe_unused]] std::string_view value) const
        -> std::string override {
        throw std::runtime_error{"present must not escape"};
    }
};

/// @brief 撑长策略：把每个字段的呈现值撑到 64 字节，用来验证呈现侧截断。
class ExpandingPolicy final : public ExportPolicy {
  public:
    [[nodiscard]] auto allows([[maybe_unused]] ExportSurface surface, [[maybe_unused]] std::string_view key,
                              [[maybe_unused]] FieldClass field_class) const -> bool override {
        return false;
    }
    [[nodiscard]] auto present([[maybe_unused]] ExportSurface surface, [[maybe_unused]] std::string_view key,
                               [[maybe_unused]] FieldClass field_class, [[maybe_unused]] std::string_view value) const
        -> std::string override {
        const std::string expanded(64, 'x');
        return expanded;
    }
};

/// @brief 面相关策略：只在 InspectorJson 面放行 `query.q`。
class InspectorOnlyPolicy final : public ExportPolicy {
  public:
    [[nodiscard]] auto allows(ExportSurface surface, std::string_view key,
                              [[maybe_unused]] FieldClass field_class) const -> bool override {
        return surface == ExportSurface::InspectorJson && key == "query.q";
    }
    [[nodiscard]] auto present([[maybe_unused]] ExportSurface surface, [[maybe_unused]] std::string_view key,
                               [[maybe_unused]] FieldClass field_class, std::string_view value) const
        -> std::string override {
        return std::string{value};
    }
};

/// @brief 计数器消费者：只留存标量事实，`noexcept` 回调内不做任何可能抛出的复制。
class CountingSink final : public NetEventSink {
  public:
    auto on_exchange_begin(const HttpActivityEntry &entry) noexcept -> void override {
        ++begins;
        last_begin_id = entry.exchange_id;
    }
    auto on_stage(std::uint64_t exchange_id, const StageMark &mark) noexcept -> void override {
        ++stages;
        last_stage_id = exchange_id;
        last_stage = mark;
    }
    auto on_exchange_end(const HttpActivityEntry &entry) noexcept -> void override {
        ++ends;
        last_end_id = entry.exchange_id;
        last_end_abandoned = entry.abandoned;
    }

    std::uint64_t begins{0};
    std::uint64_t stages{0};
    std::uint64_t ends{0};
    std::uint64_t last_begin_id{0};
    std::uint64_t last_stage_id{0};
    StageMark last_stage{};
    std::uint64_t last_end_id{0};
    bool last_end_abandoned{false};
};

#ifdef AURORA_ENABLE_DEBUG

/// @brief 解析一枚测试 URL（失败即本用例致命失败）。
/// @param text URL 文本。
/// @return 解析后的 URL。
[[nodiscard]] auto make_url(std::string_view text) -> Url {
    auto parsed = Url::parse(text);
    AURORA_TEST_REQUIRE_TRUE(parsed.ok());
    return parsed.value();
}

#endif  // AURORA_ENABLE_DEBUG

AURORA_TEST_CASE(redaction_folds_by_class_and_never_amplifies_secret) {
    ProcessReset reset;

    // 无策略：Public 原样、Sensitive 折叠、Secret 折叠；键名能认出凭据的，声明为 Sensitive 也会被上调。
    AURORA_TEST_CHECK_EQ(present_field(ExportSurface::Timeline, "path", FieldClass::Public, "/a"), std::string{"/a"});
    AURORA_TEST_CHECK_EQ(present_field(ExportSurface::Timeline, "header.x-trace", FieldClass::Sensitive, "v"),
                         std::string{"***"});
    AURORA_TEST_CHECK_EQ(
        present_field(ExportSurface::Timeline, "header.authorization", FieldClass::Secret, "Bearer abcdef"),
        std::string{"Bearer len=6"});
    AURORA_TEST_CHECK_EQ(
        present_field(ExportSurface::Timeline, "header.proxy-authorization", FieldClass::Secret, "Basic dXNlcjpwYXNz"),
        std::string{"Basic len=12"});
    AURORA_TEST_CHECK_EQ(present_field(ExportSurface::Timeline, "header.cookie", FieldClass::Secret, "sid=1"),
                         std::string{"***"});
    AURORA_TEST_CHECK_EQ(present_field(ExportSurface::Timeline, "header.x-api-key", FieldClass::Sensitive, "k-123"),
                         std::string{"***"});
    AURORA_TEST_CHECK_EQ(present_field(ExportSurface::Timeline, "query.access_token", FieldClass::Sensitive, "abc"),
                         std::string{"access_token=***"});
    AURORA_TEST_CHECK_EQ(present_field(ExportSurface::Timeline, "userinfo", FieldClass::Sensitive, "u:p"),
                         std::string{"***"});

    // 身份策略（全部放行、原样返回）：Sensitive 放行后可给原值，Secret 拿到的仍是折叠形态。
    install_export_policy(std::make_shared<IdentityPolicy>());
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(aurora::net::export_policy()));
    AURORA_TEST_CHECK_EQ(present_field(ExportSurface::Timeline, "header.x-trace", FieldClass::Sensitive, "v"),
                         std::string{"v"});
    AURORA_TEST_CHECK_EQ(
        present_field(ExportSurface::Timeline, "header.authorization", FieldClass::Secret, "Bearer abcdef"),
        std::string{"Bearer len=6"});
    AURORA_TEST_CHECK_EQ(present_field(ExportSurface::Timeline, "query.api_key", FieldClass::Sensitive, "abc"),
                         std::string{"api_key=***"});

    // 收窄策略：Public 被替换成宿主给的文本。
    install_export_policy(std::make_shared<NarrowingPolicy>());
    AURORA_TEST_CHECK_EQ(present_field(ExportSurface::Timeline, "path", FieldClass::Public, "/a/b"),
                         std::string{"<hidden>"});

    // 抛出策略：Sensitive 回退折叠、Secret 回退折叠、Public 回退原值；异常不逸出。
    install_export_policy(std::make_shared<ThrowingPolicy>());
    AURORA_TEST_CHECK_EQ(present_field(ExportSurface::Timeline, "header.x-trace", FieldClass::Sensitive, "v"),
                         std::string{"***"});
    AURORA_TEST_CHECK_EQ(present_field(ExportSurface::Timeline, "header.cookie", FieldClass::Secret, "sid=1"),
                         std::string{"***"});
    AURORA_TEST_CHECK_EQ(present_field(ExportSurface::Timeline, "path", FieldClass::Public, "/a"), std::string{"/a"});

    install_export_policy(nullptr);
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(aurora::net::export_policy()));
}

AURORA_TEST_CASE(present_url_folds_userinfo_query_and_never_echoes_bad_input) {
    ProcessReset reset;

    const std::string absolute = present_url(
        ExportSurface::Timeline, "https://alice:s3cret@example.com:8443/pkg/run?access_token=abc&q=hello&flag");
    AURORA_TEST_CHECK_EQ(absolute, std::string{"https://example.com:8443/pkg/run?access_token=***&q=***&flag"});
    AURORA_TEST_CHECK_TRUE(absolute.find("alice") == std::string::npos);
    AURORA_TEST_CHECK_TRUE(absolute.find("s3cret") == std::string::npos);

    AURORA_TEST_CHECK_EQ(present_url(ExportSurface::Timeline, "/p?token=xyz&q=1"), std::string{"/p?token=***&q=***"});
    AURORA_TEST_CHECK_EQ(present_url(ExportSurface::Timeline, "/p?token=xyz&q=1#frag"),
                         std::string{"/p?token=***&q=***"});

    // 只有面与键都匹配时才放行：同一条 URL 在两个面上给出不同呈现。
    install_export_policy(std::make_shared<InspectorOnlyPolicy>());
    AURORA_TEST_CHECK_EQ(present_url(ExportSurface::Timeline, "/p?token=xyz&q=1"), std::string{"/p?token=***&q=***"});
    AURORA_TEST_CHECK_EQ(present_url(ExportSurface::InspectorJson, "/p?token=xyz&q=1"),
                         std::string{"/p?token=***&q=1"});

    // 解不开的 URL 只回占位符，绝不回显输入。
    const std::string unparsable = present_url(ExportSurface::Timeline, "http://[not-a-host");
    AURORA_TEST_CHECK_EQ(unparsable, std::string{"<unparsable-url>"});
    AURORA_TEST_CHECK_TRUE(unparsable.find("not-a-host") == std::string::npos);
}

AURORA_TEST_CASE(stage_tokens_are_stable_lower_case) {
    AURORA_TEST_CHECK_EQ(http_stage_text(HttpStage::Connect), std::string_view{"connect"});
    AURORA_TEST_CHECK_EQ(http_stage_text(HttpStage::TlsHandshake), std::string_view{"tls_handshake"});
    AURORA_TEST_CHECK_EQ(http_stage_text(HttpStage::ConnectionAcquired), std::string_view{"connection_acquired"});
    AURORA_TEST_CHECK_EQ(http_stage_text(HttpStage::RequestSent), std::string_view{"request_sent"});
    AURORA_TEST_CHECK_EQ(http_stage_text(HttpStage::FirstByte), std::string_view{"first_byte"});
    AURORA_TEST_CHECK_EQ(http_stage_text(HttpStage::BodyDone), std::string_view{"body_done"});
}

#ifdef AURORA_ENABLE_DEBUG

AURORA_TEST_CASE(lifecycle_pairs_begin_marks_and_end) {
    ProcessReset reset;
    auto sink = std::make_shared<CountingSink>();
    subscribe_events(sink);

    const Url target = make_url("https://example.com/pkg/run?access_token=abc&q=1");
    auto trace = std::make_unique<ExchangeTrace>("GET", "rid-1", target, 0, 0, false, false);
    const std::uint64_t id = trace->id();
    AURORA_TEST_CHECK_GT(id, std::uint64_t{0});
    trace->mark(HttpStage::Connect);
    trace->mark(HttpStage::ConnectionAcquired);
    trace->set_status(200);
    trace->note_sent(120);
    trace->note_received(64);
    trace->mark(HttpStage::RequestSent);
    trace->mark(HttpStage::FirstByte);
    trace->mark(HttpStage::BodyDone);
    trace->finish();

    const auto snapshot = activity_snapshot();
    AURORA_TEST_CHECK_TRUE(snapshot.available);
    AURORA_TEST_REQUIRE_EQ(snapshot.entries.size(), std::size_t{1});
    const HttpActivityEntry &entry = snapshot.entries.front();
    AURORA_TEST_CHECK_EQ(entry.exchange_id, id);
    AURORA_TEST_CHECK_EQ(entry.request_id, std::string{"rid-1"});
    AURORA_TEST_CHECK_EQ(entry.method, std::string{"GET"});
    AURORA_TEST_CHECK_EQ(entry.path, std::string{"/pkg/run?access_token=***&q=***"});
    AURORA_TEST_CHECK_EQ(entry.host, std::string{"example.com"});
    AURORA_TEST_CHECK_EQ(entry.status, static_cast<std::uint16_t>(200));
    AURORA_TEST_CHECK_FALSE(entry.in_flight);
    AURORA_TEST_CHECK_FALSE(entry.abandoned);
    AURORA_TEST_CHECK_FALSE(entry.truncated);
    AURORA_TEST_CHECK_EQ(entry.sent, std::uint64_t{120});
    AURORA_TEST_CHECK_EQ(entry.received, std::uint64_t{64});
    AURORA_TEST_CHECK_FALSE(entry.error_class.has_value());
    AURORA_TEST_CHECK_FALSE(entry.failed_stage.has_value());

    const auto timeline = require_value(timeline_of("rid-1"));
    AURORA_TEST_CHECK_EQ(timeline.exchange_id, id);
    AURORA_TEST_CHECK_FALSE(timeline.in_flight);
    AURORA_TEST_REQUIRE_EQ(timeline.marks.size(), std::size_t{5});
    AURORA_TEST_CHECK_TRUE(timeline.marks.front().stage == HttpStage::Connect);
    AURORA_TEST_CHECK_TRUE(timeline.marks.back().stage == HttpStage::BodyDone);
    for (std::size_t index = 0; index < timeline.marks.size(); ++index) {
        AURORA_TEST_CHECK_LE(timeline.marks[index].offset, timeline.total);
        if (index > 0) {
            AURORA_TEST_CHECK_LE(timeline.marks[index - 1].offset, timeline.marks[index].offset);
        }
    }
    AURORA_TEST_CHECK_FALSE(timeline_of("").has_value());
    AURORA_TEST_CHECK_FALSE(timeline_of("unknown").has_value());
    AURORA_TEST_CHECK_EQ(timelines_of("rid-1").size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(timelines_of("unknown").size(), std::size_t{0});

    // 总线：两人各收同一组事实，计数相等；收摊后的条目上原值仍不出现。
    AURORA_TEST_CHECK_EQ(sink->begins, std::uint64_t{1});
    AURORA_TEST_CHECK_EQ(sink->stages, std::uint64_t{5});
    AURORA_TEST_CHECK_EQ(sink->ends, std::uint64_t{1});
    AURORA_TEST_CHECK_EQ(sink->last_begin_id, id);
    AURORA_TEST_CHECK_EQ(sink->last_stage_id, id);
    AURORA_TEST_CHECK_EQ(sink->last_end_id, id);

    trace.reset();
    AURORA_TEST_CHECK_EQ(sink->ends, std::uint64_t{1});  // 已收摊的句柄析构不得补发
}

AURORA_TEST_CASE(timeline_of_returns_newest_hop_including_in_flight) {
    ProcessReset reset;
    const Url target = make_url("https://example.com/r");

    ExchangeTrace first{"GET", "rid-latest", target, 0, 0, false, false};
    first.mark(HttpStage::Connect);
    first.finish();
    ExchangeTrace second{"GET", "rid-latest", target, 1, 0, false, false};
    second.mark(HttpStage::Connect);

    // 同一链路：`timeline_of` 取 exchange_id 最大的一跳，在途也照取（不能因未收摊就跳过它）。
    auto latest = require_value(timeline_of("rid-latest"));
    AURORA_TEST_CHECK_EQ(latest.exchange_id, second.id());
    AURORA_TEST_CHECK_EQ(latest.hop, static_cast<std::uint16_t>(1));
    AURORA_TEST_CHECK_TRUE(latest.in_flight);
    AURORA_TEST_CHECK_GT(latest.total.count(), std::int64_t{0});

    // 全部跳按 exchange_id 升序，与快照的「最新在前」刻意分岔：时间线回答的是因果次序。
    const auto hops = timelines_of("rid-latest");
    AURORA_TEST_REQUIRE_EQ(hops.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(hops.front().exchange_id, first.id());
    AURORA_TEST_CHECK_EQ(hops.back().exchange_id, second.id());

    second.finish();
    latest = require_value(timeline_of("rid-latest"));
    AURORA_TEST_CHECK_FALSE(latest.in_flight);
}

AURORA_TEST_CASE(abandoned_trace_ends_once_without_fake_error) {
    ProcessReset reset;
    auto sink = std::make_shared<CountingSink>();
    subscribe_events(sink);

    const Url target = make_url("http://127.0.0.1:9/stop");
    {
        ExchangeTrace trace{"POST", "rid-abandon", target, 0, 0, false, false};
        trace.mark(HttpStage::Connect);
        trace.mark(HttpStage::ConnectionAcquired);
    }

    const auto snapshot = activity_snapshot();
    AURORA_TEST_REQUIRE_EQ(snapshot.entries.size(), std::size_t{1});
    const HttpActivityEntry &entry = snapshot.entries.front();
    AURORA_TEST_CHECK_TRUE(entry.abandoned);
    AURORA_TEST_CHECK_FALSE(entry.in_flight);
    AURORA_TEST_CHECK_TRUE(entry.error_code.empty());
    AURORA_TEST_CHECK_FALSE(entry.failed_stage.has_value());
    AURORA_TEST_CHECK_EQ(sink->ends, std::uint64_t{1});
    AURORA_TEST_CHECK_TRUE(sink->last_end_abandoned);
    const auto abandoned_timeline = require_value(timeline_of("rid-abandon"));
    AURORA_TEST_CHECK_FALSE(abandoned_timeline.in_flight);
}

AURORA_TEST_CASE(bus_notifies_every_subscriber_and_detaches_on_death) {
    ProcessReset reset;
    auto first = std::make_shared<CountingSink>();
    auto second = std::make_shared<CountingSink>();
    subscribe_events(first);
    subscribe_events(second);

    const Url target = make_url("https://example.com/x");
    {
        ExchangeTrace trace{"GET", "rid-bus", target, 0, 0, false, false};
        trace.mark(HttpStage::Connect);
        trace.finish();
    }
    AURORA_TEST_CHECK_EQ(first->begins, second->begins);
    AURORA_TEST_CHECK_EQ(first->stages, second->stages);
    AURORA_TEST_CHECK_EQ(first->ends, second->ends);
    AURORA_TEST_CHECK_EQ(first->ends, std::uint64_t{1});

    second.reset();
    {
        ExchangeTrace trace{"GET", "rid-bus-2", target, 0, 0, false, false};
        trace.mark(HttpStage::Connect);
        trace.finish();
    }
    AURORA_TEST_CHECK_EQ(first->ends, std::uint64_t{2});  // 存活者照常收
    subscribe_events(std::weak_ptr<NetEventSink>{});  // 空弱引用不崩、不登记
    {
        ExchangeTrace trace{"GET", "rid-bus-3", target, 0, 0, false, false};
        trace.finish();
    }
    AURORA_TEST_CHECK_EQ(first->ends, std::uint64_t{3});
}

AURORA_TEST_CASE(capacity_evicts_oldest_including_in_flight) {
    ProcessReset reset;
    auto sink = std::make_shared<CountingSink>();
    subscribe_events(sink);
    set_activity_options(ActivityOptions{.record = true, .capacity = 2, .max_value_chars = 256});

    const Url target = make_url("https://example.com/r");
    ExchangeTrace oldest{"GET", "rid-old", target, 0, 0, false, false};
    ExchangeTrace middle{"GET", "rid-mid", target, 1, 0, false, false};
    ExchangeTrace newest{"GET", "rid-new", target, 2, 0, false, false};
    const std::uint64_t oldest_id = oldest.id();
    oldest.mark(HttpStage::Connect);

    auto snapshot = activity_snapshot();
    AURORA_TEST_REQUIRE_EQ(snapshot.entries.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(snapshot.dropped, std::uint64_t{1});
    AURORA_TEST_CHECK_EQ(snapshot.entries.front().exchange_id, newest.id());
    AURORA_TEST_CHECK_EQ(snapshot.entries.back().exchange_id, middle.id());
    AURORA_TEST_CHECK_TRUE(snapshot.entries.front().in_flight);

    oldest.finish();  // 已被淘汰：存储不复活、不补造 end；总线照常收到真实收摊
    snapshot = activity_snapshot();
    AURORA_TEST_CHECK_EQ(snapshot.entries.size(), std::size_t{2});
    for (const HttpActivityEntry &entry : snapshot.entries) {
        AURORA_TEST_CHECK_NE(entry.exchange_id, oldest_id);
    }
    AURORA_TEST_CHECK_EQ(sink->ends, std::uint64_t{1});

    clear_network_activity();
    snapshot = activity_snapshot();
    AURORA_TEST_CHECK_TRUE(snapshot.entries.empty());
    AURORA_TEST_CHECK_EQ(snapshot.dropped, std::uint64_t{0});
    // 清空后迟到的 end 被存储忽略（总线仍收到）。
    middle.finish();
    AURORA_TEST_CHECK_TRUE(activity_snapshot().entries.empty());
    AURORA_TEST_CHECK_EQ(sink->ends, std::uint64_t{2});
}

AURORA_TEST_CASE(shrink_and_record_switch_follow_the_table) {
    ProcessReset reset;
    set_activity_options(ActivityOptions{.record = true, .capacity = 4, .max_value_chars = 256});
    const Url target = make_url("https://example.com/r");
    {
        ExchangeTrace first{"GET", "a", target, 0, 0, false, false};
        ExchangeTrace second{"GET", "b", target, 1, 0, false, false};
        ExchangeTrace third{"GET", "c", target, 2, 0, false, false};
        first.finish();
        second.finish();
        third.finish();
    }
    AURORA_TEST_CHECK_EQ(activity_snapshot().entries.size(), std::size_t{3});

    set_activity_options(ActivityOptions{.record = true, .capacity = 1, .max_value_chars = 256});
    auto snapshot = activity_snapshot();
    AURORA_TEST_CHECK_EQ(snapshot.entries.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(snapshot.dropped, std::uint64_t{2});

    clear_network_activity();
    set_activity_options(ActivityOptions{.record = false, .capacity = 4, .max_value_chars = 256});
    {
        ExchangeTrace ignored{"GET", "d", target, 0, 0, false, false};
        ignored.mark(HttpStage::Connect);
        ignored.finish();
    }
    AURORA_TEST_CHECK_TRUE(activity_snapshot().entries.empty());

    set_activity_options(ActivityOptions{.record = true, .capacity = 4, .max_value_chars = 256});
    {
        ExchangeTrace recorded{"GET", "e", target, 0, 0, false, false};
        recorded.finish();
    }
    AURORA_TEST_CHECK_EQ(activity_snapshot().entries.size(), std::size_t{1});
}

AURORA_TEST_CASE(bounded_budget_replaces_targets_and_caps_presentation) {
    ProcessReset reset;
    set_activity_options(ActivityOptions{.record = true, .capacity = 4, .max_value_chars = 32});

    const std::string long_query = "/a?token=abcdefghijklmnopqrstuvwxyz0123456789";
    const Url long_target = make_url("https://example.com" + long_query);
    {
        ExchangeTrace trace{"GET", "rid-long", long_target, 0, 0, false, false};
        trace.finish();
    }
    auto snapshot = activity_snapshot();
    AURORA_TEST_REQUIRE_EQ(snapshot.entries.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(snapshot.entries.front().path, std::string{"<oversized-url>"});
    AURORA_TEST_CHECK_TRUE(snapshot.entries.front().truncated);
    AURORA_TEST_CHECK_EQ(snapshot.truncated, std::uint64_t{1});
    AURORA_TEST_CHECK_TRUE(snapshot.entries.front().path.find("abcdefghij") == std::string::npos);

    // 呈现值超限：原始目标很短，宿主策略把它撑过上限 ⇒ 按上限截断并置 truncated。
    clear_network_activity();
    install_export_policy(std::make_shared<ExpandingPolicy>());
    const Url short_target = make_url("https://example.com/a");
    {
        ExchangeTrace trace{"GET", "rid-expand", short_target, 0, 0, false, false};
        trace.finish();
    }
    snapshot = activity_snapshot(ExportSurface::Timeline, 1);
    AURORA_TEST_REQUIRE_EQ(snapshot.entries.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(snapshot.entries.front().path.size(), std::size_t{32});
    AURORA_TEST_CHECK_TRUE(snapshot.entries.front().truncated);
    AURORA_TEST_CHECK_EQ(snapshot.truncated, std::uint64_t{1});
}

AURORA_TEST_CASE(snapshot_surface_changes_presentation) {
    ProcessReset reset;
    install_export_policy(std::make_shared<InspectorOnlyPolicy>());
    const Url target = make_url("https://user:pw@example.com/a?token=xyz&q=hello");
    {
        ExchangeTrace trace{"GET", "rid-surface", target, 0, 0, false, false};
        trace.finish();
    }

    const auto timeline_surface = activity_snapshot(ExportSurface::Timeline);
    const auto inspector_surface = activity_snapshot(ExportSurface::InspectorJson);
    AURORA_TEST_REQUIRE_EQ(timeline_surface.entries.size(), std::size_t{1});
    AURORA_TEST_REQUIRE_EQ(inspector_surface.entries.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(timeline_surface.entries.front().path, std::string{"/a?token=***&q=***"});
    AURORA_TEST_CHECK_EQ(inspector_surface.entries.front().path, std::string{"/a?token=***&q=hello"});
    AURORA_TEST_CHECK_TRUE(inspector_surface.entries.front().host == "example.com");
    AURORA_TEST_CHECK_TRUE(inspector_surface.entries.front().path.find("user") == std::string::npos);
    AURORA_TEST_CHECK_TRUE(inspector_surface.entries.front().path.find("xyz") == std::string::npos);
}

AURORA_TEST_CASE(error_classification_keeps_status_and_failed_stage) {
    ProcessReset reset;
    const Url target = make_url("https://example.com/e");
    {
        ExchangeTrace status_failure{"GET", "rid-404", target, 0, 0, false, false};
        status_failure.set_status(404);
        status_failure.finish();
    }
    {
        ExchangeTrace transport_failure{"GET", "rid-dns", target, 0, 1, false, false};
        transport_failure.mark(HttpStage::Connect);
        transport_failure.fail(aurora::ErrorCode::NetDnsFailed, HttpStage::Connect);
        transport_failure.finish();
    }
    const auto snapshot = activity_snapshot();
    AURORA_TEST_REQUIRE_EQ(snapshot.entries.size(), std::size_t{2});
    // 最新在前：DNS 失败在后建，故落在 front。
    AURORA_TEST_CHECK_TRUE(require_value(snapshot.entries.front().error_class) == HttpErrorClass::Dns);
    AURORA_TEST_CHECK_EQ(snapshot.entries.front().error_code, std::string{"net-dns-failed"});
    AURORA_TEST_CHECK_TRUE(require_value(snapshot.entries.front().failed_stage) == HttpStage::Connect);
    AURORA_TEST_CHECK_EQ(snapshot.entries.front().pass, static_cast<std::uint16_t>(1));
    AURORA_TEST_CHECK_TRUE(snapshot.entries.back().status == 404);
    AURORA_TEST_CHECK_TRUE(require_value(snapshot.entries.back().error_class) == HttpErrorClass::Status);
    AURORA_TEST_CHECK_TRUE(snapshot.entries.back().error_code.empty());
}

#endif  // AURORA_ENABLE_DEBUG

AURORA_TEST_CASE(activity_surface_follows_build_flavour) {
    ProcessReset reset;
#ifndef AURORA_ENABLE_DEBUG
    const auto snapshot = activity_snapshot();
    AURORA_TEST_CHECK_FALSE(snapshot.available);
    AURORA_TEST_CHECK_FALSE(snapshot.reason.empty());
    AURORA_TEST_CHECK_TRUE(snapshot.entries.empty());
    AURORA_TEST_CHECK_FALSE(timeline_of("anything").has_value());
    AURORA_TEST_CHECK_TRUE(timelines_of("anything").empty());
    subscribe_events(std::weak_ptr<NetEventSink>{});
    AURORA_TEST_CHECK_EQ(activity_options().record, true);
#else
    AURORA_TEST_CHECK_TRUE(activity_snapshot().available);
#endif
}

}  // namespace
}  // namespace aurora::test_cases::utest_net_observability
