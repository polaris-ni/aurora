/// @file http_observability.cpp
/// @brief 网络活动存储、事件总线与活动句柄的实现。
///
/// 存储与总线是同一份进程级状态的两面：存储回答「现在表里有什么」，总线回答「此刻发生了什么」。
/// 两者在同一把锁下更新，但**呈现与回调都在锁外**——策略实现与订阅者都是宿主代码，持锁调用会把
/// 一把内部锁交到外部手里。锁外呈现的代价是快照与事件可能看到略新的策略，这是「按调用时刻取
/// 策略」的既定口径，不是竞态缺陷。
///
/// Release（`AURORA_ENABLE_DEBUG` 未开）下本文件的全部机制被编译掉：公开函数回确定形态，发起侧
/// 句柄是头文件里的空壳，故不存在「关着也在记账」的隐藏成本。

#include "aurora/net/http_observability.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.gen.h"
#include "aurora/core/url.h"
#include "aurora/net/detail/observability_trace.h"
#include "aurora/net/observability.h"

namespace aurora::net::http {
namespace {

constexpr std::string_view AURORA_RELEASE_REASON =
    "network activity is unavailable: AURORA_ENABLE_DEBUG is off (store, bus and emit points are not compiled in)";
constexpr std::string_view AURORA_OVERSIZED_URL = "<oversized-url>";
constexpr std::uint32_t AURORA_MIN_CAPACITY = 1;
constexpr std::uint32_t AURORA_MAX_CAPACITY = 1024;
constexpr std::uint32_t AURORA_MIN_VALUE_CHARS = 32;
constexpr std::uint32_t AURORA_MAX_VALUE_CHARS = 4096;

#ifdef AURORA_ENABLE_DEBUG

/// @brief 存储里的一条原始记录：文本保持未呈现形态，快照按请求面现场折叠。
struct RawRecord {
    std::uint64_t exchange_id{0};
    std::string request_id;
    std::string method;
    std::string raw_path;
    std::string raw_host;
    std::uint16_t hop{0};
    std::uint16_t pass{0};
    std::uint16_t status{0};
    std::string error_code;
    std::optional<HttpErrorClass> error_class;
    std::optional<HttpStage> failed_stage;
    std::uint64_t sent{0};
    std::uint64_t received{0};
    bool in_flight{true};
    bool abandoned{false};
    bool reused_connection{false};
    bool from_cache{false};
    bool truncated{false};
    std::chrono::steady_clock::time_point started;
    std::chrono::microseconds elapsed{0};
    std::vector<StageMark> marks;
};

/// @brief 进程级状态：配置、环形记录与订阅名单共用一把锁。
struct StoreState {
    std::mutex mutex;
    ActivityOptions options;
    std::deque<RawRecord> records;  // 旧在前
    std::uint64_t dropped{0};
    std::vector<std::weak_ptr<NetEventSink>> sinks;
};

auto state() -> StoreState & {
    static StoreState instance;
    return instance;
}

auto next_exchange_id() -> std::uint64_t {
    static std::atomic<std::uint64_t> next{0};
    return next.fetch_add(1, std::memory_order_relaxed) + 1;
}

auto clamp_options(ActivityOptions options) -> ActivityOptions {
    options.capacity = std::clamp(options.capacity, AURORA_MIN_CAPACITY, AURORA_MAX_CAPACITY);
    options.max_value_chars = std::clamp(options.max_value_chars, AURORA_MIN_VALUE_CHARS, AURORA_MAX_VALUE_CHARS);
    return options;
}

auto origin_form_of(const Url &target) -> std::string {
    std::string path{target.path()};
    if (target.has_query()) {
        path.push_back('?');
        path += target.query();
    }
    return path;
}

auto authority_of(const Url &target) -> std::string {
    std::string host;
    if (target.host_kind() == UrlHostKind::Ipv6) {
        host.push_back('[');
        host += target.host();
        host.push_back(']');
    } else {
        host += target.host();
    }
    if (target.port()) {
        host.push_back(':');
        host += std::to_string(*target.port());
    }
    return host;
}

/// @brief 超限文本按上限截断；返回是否发生了截断。
auto cap_text(std::string &text, std::uint32_t limit, bool &truncated) -> void {
    if (text.size() > limit) {
        text.resize(limit);
        truncated = true;
    }
}

/// @brief 按导出面呈现一条记录；不修改记录本身。
auto entry_of(ExportSurface surface, const RawRecord &record, std::chrono::steady_clock::time_point now,
              std::uint32_t limit) -> HttpActivityEntry {
    HttpActivityEntry entry;
    entry.exchange_id = record.exchange_id;
    entry.request_id = present_field(surface, "request_id", FieldClass::Public, record.request_id);
    entry.method = present_field(surface, "method", FieldClass::Public, record.method);
    const std::string folded_target =
        record.raw_path == AURORA_OVERSIZED_URL ? record.raw_path : present_url(surface, record.raw_path);
    entry.path = present_field(surface, "path", FieldClass::Public, folded_target);
    entry.host = present_field(surface, "host", FieldClass::Public, record.raw_host);
    if (!record.error_code.empty()) {
        entry.error_code = present_field(surface, "detail.error_code", FieldClass::Public, record.error_code);
    }
    bool truncated = record.truncated;
    cap_text(entry.request_id, limit, truncated);
    cap_text(entry.method, limit, truncated);
    cap_text(entry.path, limit, truncated);
    cap_text(entry.host, limit, truncated);
    cap_text(entry.error_code, limit, truncated);
    entry.hop = record.hop;
    entry.pass = record.pass;
    entry.status = record.status;
    entry.error_class = record.error_class;
    if (!entry.error_class && entry.status >= 400) {
        entry.error_class = error_class_of_status(entry.status);
    }
    entry.failed_stage = record.failed_stage;
    entry.elapsed =
        record.in_flight ? std::chrono::duration_cast<std::chrono::microseconds>(now - record.started) : record.elapsed;
    entry.sent = record.sent;
    entry.received = record.received;
    entry.in_flight = record.in_flight;
    entry.abandoned = record.abandoned;
    entry.reused_connection = record.reused_connection;
    entry.from_cache = record.from_cache;
    entry.truncated = truncated;
    return entry;
}

auto timeline_of_record(const RawRecord &record, std::chrono::steady_clock::time_point now) -> HttpRequestTimeline {
    HttpRequestTimeline timeline;
    timeline.exchange_id = record.exchange_id;
    timeline.request_id = record.request_id;
    timeline.hop = record.hop;
    timeline.pass = record.pass;
    timeline.total =
        record.in_flight ? std::chrono::duration_cast<std::chrono::microseconds>(now - record.started) : record.elapsed;
    timeline.in_flight = record.in_flight;
    timeline.reused_connection = record.reused_connection;
    timeline.marks = record.marks;
    return timeline;
}

/// @brief 锁外调用订阅者：先在锁内取一份强引用名单，再逐枚回调。
template <typename Callback>
auto emit_events(const Callback &callback) -> void {
    std::vector<std::shared_ptr<NetEventSink>> alive;
    {
        const std::scoped_lock guard(state().mutex);
        auto &sinks = state().sinks;
        sinks.erase(std::remove_if(sinks.begin(), sinks.end(),
                                   [](const std::weak_ptr<NetEventSink> &sink) { return sink.expired(); }),
                    sinks.end());
        alive.reserve(sinks.size());
        for (const auto &sink : sinks) {
            if (auto strong = sink.lock()) {
                alive.push_back(std::move(strong));
            }
        }
    }
    for (const auto &sink : alive) {
        callback(*sink);
    }
}

/// @brief 在锁内定位一枚记录并就地更新；已被淘汰 / 清空的记录静默忽略。
template <typename Update>
auto update_record(std::uint64_t exchange_id, const Update &update) -> void {
    const std::scoped_lock guard(state().mutex);
    for (RawRecord &record : state().records) {
        if (record.exchange_id == exchange_id) {
            update(record);
            return;
        }
    }
}

#endif  // AURORA_ENABLE_DEBUG

}  // namespace

auto http_stage_text(HttpStage stage) -> std::string_view {
    switch (stage) {
        case HttpStage::Connect:
            return "connect";
        case HttpStage::TlsHandshake:
            return "tls_handshake";
        case HttpStage::ConnectionAcquired:
            return "connection_acquired";
        case HttpStage::RequestSent:
            return "request_sent";
        case HttpStage::FirstByte:
            return "first_byte";
        case HttpStage::BodyDone:
            return "body_done";
    }
    return "connect";  // 越界值折回首阶段，与 error_class_text 的兜底同一口径
}

#ifdef AURORA_ENABLE_DEBUG

// ---- 发起侧句柄 ----

ExchangeTrace::ExchangeTrace(std::string_view method, std::string_view request_id, const Url &target, std::uint16_t hop,
                             std::uint16_t pass, bool reused_connection, bool from_cache)
    : exchange_id_{next_exchange_id()}, request_id_{request_id}, method_{method}, path_{origin_form_of(target)},
      host_{authority_of(target)}, hop_{hop}, pass_{pass}, reused_connection_{reused_connection},
      from_cache_{from_cache}, started_{std::chrono::steady_clock::now()} {
    publish_begin();
}

ExchangeTrace::~ExchangeTrace() {
    if (finished_) {
        return;
    }
    finished_ = true;
    try {
        publish_end(true);
    }
    // 析构不外抛：发布的呈现与回调失败都不能把异常带出发起侧。
    // NOLINTNEXTLINE(bugprone-empty-catch) 见上行说明：静默收摊是本体的设计意图，不是漏处理
    catch (...) {
    }
}

auto ExchangeTrace::mark(HttpStage stage) -> void {
    // 复用连接没有建连与握手两段，首枚 `ConnectionAcquired` 就是这一跳的起点：offset 定 0。
    // 取用空闲连接的耗时在微秒口径下不可分辨，且它不属于任何协议阶段；`total` 仍含池排队时间。
    const bool reused_origin = reused_connection_ && !any_mark_ && stage == HttpStage::ConnectionAcquired;
    any_mark_ = true;
    const StageMark mark{.stage = stage,
                         .offset = reused_origin ? std::chrono::microseconds{0}
                                                 : std::chrono::duration_cast<std::chrono::microseconds>(
                                                       std::chrono::steady_clock::now() - started_)};
    update_record(exchange_id_, [&mark](RawRecord &record) { record.marks.push_back(mark); });
    emit_events([&mark, id = exchange_id_](NetEventSink &sink) { sink.on_stage(id, mark); });
}

auto ExchangeTrace::set_status(std::uint16_t status) -> void {
    status_ = status;
    update_record(exchange_id_, [status](RawRecord &record) { record.status = status; });
}

auto ExchangeTrace::note_sent(std::uint64_t bytes) -> void {
    sent_ += bytes;
    update_record(exchange_id_, [sent = sent_](RawRecord &record) { record.sent = sent; });
}

auto ExchangeTrace::note_received(std::uint64_t bytes) -> void {
    received_ += bytes;
    update_record(exchange_id_, [received = received_](RawRecord &record) { record.received = received; });
}

auto ExchangeTrace::set_reused_connection(bool reused) -> void {
    reused_connection_ = reused;
    update_record(exchange_id_, [reused](RawRecord &record) { record.reused_connection = reused; });
}

auto ExchangeTrace::fail(ErrorCode code, std::optional<HttpStage> failed_stage) -> void {
    error_code_ = std::string{aurora::slug(code)};
    error_class_ = error_class_of(code);
    failed_stage_ = failed_stage;
    update_record(exchange_id_, [this](RawRecord &record) {
        record.error_code = error_code_;
        record.error_class = error_class_;
        record.failed_stage = failed_stage_;
    });
}

auto ExchangeTrace::finish() -> void {
    if (finished_) {
        return;
    }
    finished_ = true;
    publish_end(false);
}

auto ExchangeTrace::publish_begin() -> void {
    std::uint32_t limit = 0;
    {
        const std::scoped_lock guard(state().mutex);
        auto &store = state();
        limit = store.options.max_value_chars;
        if (path_.size() > limit) {
            path_ = std::string{AURORA_OVERSIZED_URL};
            truncated_ = true;
        }
        if (store.options.record) {
            RawRecord record;
            record.exchange_id = exchange_id_;
            record.request_id = request_id_;
            record.method = method_;
            record.raw_path = path_;
            record.raw_host = host_;
            record.hop = hop_;
            record.pass = pass_;
            record.reused_connection = reused_connection_;
            record.from_cache = from_cache_;
            record.truncated = truncated_;
            record.started = started_;
            store.records.push_back(std::move(record));
            while (store.records.size() > static_cast<std::size_t>(store.options.capacity)) {
                store.records.pop_front();
                ++store.dropped;
            }
        }
    }
    RawRecord local;
    local.exchange_id = exchange_id_;
    local.request_id = request_id_;
    local.method = method_;
    local.raw_path = path_;
    local.raw_host = host_;
    local.hop = hop_;
    local.pass = pass_;
    local.reused_connection = reused_connection_;
    local.from_cache = from_cache_;
    local.truncated = truncated_;
    local.started = started_;
    const HttpActivityEntry entry = entry_of(ExportSurface::Timeline, local, std::chrono::steady_clock::now(), limit);
    emit_events([&entry](NetEventSink &sink) { sink.on_exchange_begin(entry); });
}

auto ExchangeTrace::publish_end(bool abandoned) -> void {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - started_);
    std::uint32_t limit = 0;
    {
        const std::scoped_lock guard(state().mutex);
        auto &store = state();
        limit = store.options.max_value_chars;
        if (path_.size() > limit && path_ != AURORA_OVERSIZED_URL) {
            path_ = std::string{AURORA_OVERSIZED_URL};
            truncated_ = true;
        }
        for (RawRecord &record : store.records) {
            if (record.exchange_id != exchange_id_) {
                continue;
            }
            record.status = status_;
            record.error_code = error_code_;
            record.error_class = error_class_;
            record.failed_stage = failed_stage_;
            record.sent = sent_;
            record.received = received_;
            record.reused_connection = reused_connection_;
            record.abandoned = abandoned;
            record.truncated = record.truncated || truncated_;
            record.elapsed = elapsed;
            record.in_flight = false;
            break;
        }
    }
    RawRecord local;
    local.exchange_id = exchange_id_;
    local.request_id = request_id_;
    local.method = method_;
    local.raw_path = path_;
    local.raw_host = host_;
    local.hop = hop_;
    local.pass = pass_;
    local.status = status_;
    local.error_code = error_code_;
    local.error_class = error_class_;
    local.failed_stage = failed_stage_;
    local.sent = sent_;
    local.received = received_;
    local.in_flight = false;
    local.abandoned = abandoned;
    local.reused_connection = reused_connection_;
    local.from_cache = from_cache_;
    local.truncated = truncated_;
    local.started = started_;
    local.elapsed = elapsed;
    const HttpActivityEntry entry = entry_of(ExportSurface::Timeline, local, now, limit);
    emit_events([&entry](NetEventSink &sink) { sink.on_exchange_end(entry); });
}

// ---- 进程级存储与总线 ----

auto subscribe_events(std::weak_ptr<NetEventSink> sink) -> void {
    if (sink.expired()) {
        return;
    }
    const std::scoped_lock guard(state().mutex);
    auto &sinks = state().sinks;
    sinks.erase(std::remove_if(sinks.begin(), sinks.end(),
                               [](const std::weak_ptr<NetEventSink> &entry) { return entry.expired(); }),
                sinks.end());
    sinks.push_back(std::move(sink));
}

auto activity_options() -> ActivityOptions {
    const std::scoped_lock guard(state().mutex);
    return state().options;
}

auto set_activity_options(ActivityOptions options) -> void {
    const ActivityOptions clamped = clamp_options(options);
    const std::scoped_lock guard(state().mutex);
    auto &store = state();
    store.options = clamped;
    while (store.records.size() > static_cast<std::size_t>(store.options.capacity)) {
        store.records.pop_front();
        ++store.dropped;
    }
}

auto clear_network_activity() -> void {
    const std::scoped_lock guard(state().mutex);
    auto &store = state();
    store.records.clear();
    store.dropped = 0;
}

auto activity_snapshot(ExportSurface surface, std::uint32_t limit) -> ActivitySnapshot {
    std::vector<RawRecord> copies;
    std::uint64_t dropped = 0;
    std::uint32_t max_chars = 0;
    {
        const std::scoped_lock guard(state().mutex);
        const auto &store = state();
        copies.assign(store.records.begin(), store.records.end());
        dropped = store.dropped;
        max_chars = store.options.max_value_chars;
    }
    ActivitySnapshot snapshot;
    snapshot.dropped = dropped;
    const auto now = std::chrono::steady_clock::now();
    snapshot.entries.reserve(copies.size());
    for (std::size_t index = copies.size(); index > 0; --index) {
        snapshot.entries.push_back(entry_of(surface, copies[index - 1], now, max_chars));
        if (snapshot.entries.back().truncated) {
            ++snapshot.truncated;
        }
    }
    if (limit > 0 && snapshot.entries.size() > limit) {
        snapshot.entries.resize(limit);
    }
    return snapshot;
}

auto timeline_of(std::string_view request_id) -> std::optional<HttpRequestTimeline> {
    if (request_id.empty()) {
        return std::nullopt;
    }
    std::optional<RawRecord> best;
    {
        const std::scoped_lock guard(state().mutex);
        for (const RawRecord &record : state().records) {
            if (record.request_id == request_id && (!best || record.exchange_id > best->exchange_id)) {
                best = record;
            }
        }
    }
    if (!best) {
        return std::nullopt;
    }
    return timeline_of_record(*best, std::chrono::steady_clock::now());
}

auto timelines_of(std::string_view request_id) -> std::vector<HttpRequestTimeline> {
    if (request_id.empty()) {
        return {};
    }
    std::vector<RawRecord> matches;
    {
        const std::scoped_lock guard(state().mutex);
        for (const RawRecord &record : state().records) {
            if (record.request_id == request_id) {
                matches.push_back(record);
            }
        }
    }
    std::sort(matches.begin(), matches.end(),
              [](const RawRecord &left, const RawRecord &right) { return left.exchange_id < right.exchange_id; });
    const auto now = std::chrono::steady_clock::now();
    std::vector<HttpRequestTimeline> timelines;
    timelines.reserve(matches.size());
    for (const RawRecord &record : matches) {
        timelines.push_back(timeline_of_record(record, now));
    }
    return timelines;
}

#else  // AURORA_ENABLE_DEBUG

// 公开签名按值收 weak_ptr（与头文件一致）；Release 下总线不存在，实参只为签名对齐。
// NOLINTNEXTLINE(performance-unnecessary-value-param)
auto subscribe_events(std::weak_ptr<NetEventSink> sink) -> void { (void)sink; }

auto activity_options() -> ActivityOptions { return ActivityOptions{}; }

auto set_activity_options(ActivityOptions options) -> void { (void)options; }

auto clear_network_activity() -> void {}

auto activity_snapshot(ExportSurface surface, std::uint32_t limit) -> ActivitySnapshot {
    (void)surface;
    (void)limit;
    ActivitySnapshot snapshot;
    snapshot.available = false;
    snapshot.reason = std::string{AURORA_RELEASE_REASON};
    return snapshot;
}

auto timeline_of(std::string_view request_id) -> std::optional<HttpRequestTimeline> {
    (void)request_id;
    return std::nullopt;
}

auto timelines_of(std::string_view request_id) -> std::vector<HttpRequestTimeline> {
    (void)request_id;
    return {};
}

#endif

}  // namespace aurora::net::http
