#pragma once

/// @brief 网络活动记账：进程级活动存储、事件总线、交换时间线与有界预算。
/// @file net/http_observability.h
///
/// 本头回答一个问题：**「刚才那一批 HTTP 交换各自走到了哪一步、花了多久、失败在哪一阶段」**。
/// 它只记事实、不做判定：记账点在发起侧（`Client::send` / `open_stream` / `connect_websocket`），
/// 消费者按需从两条路取——一枚进程级环形存储（可按导出面呈现后查询）与一封事件总线（起手 /
/// 阶段 / 收摊三枚回调）。Inspector 的 `/api/network*` 与 MCP 的两枚工具都消费前者。
///
/// 口径要点：
/// - **一次物理往来一条记录**：重定向的每一跳、重试与凭据重放的每一次尝试各占一条，以
///   `exchange_id` 为唯一主键、`hop` / `pass` 为展开维度；`X-Request-Id` 把同一逻辑交换的若干
///   物理往来串起来。
/// - **在途也在表里**：阶段是陆续到达的，只记已完成者等于在最需要观察时看不见它。容量淘汰
///   不豁免在途记录，也不补造收摊事件——被淘汰者随后真实收摊时，存储不复活它，总线照常收到。
/// - **阶段缺席即「本跳没经历」**：复用连接没有 `Connect` / `TlsHandshake`；中间重定向跳、
///   主动停读与 WebSocket 握手不标 `BodyDone`。不伪造零值阶段。
/// - **有界预算**：单字段文本受 `max_value_chars` 约束；超限的原始目标存占位符而非前缀，
///   呈现值超限按上限截断，两种都置 `truncated`。记录条数受 `capacity` 约束。
/// - **Debug 构建限定**：`AURORA_ENABLE_DEBUG` 未开时存储、总线与发起侧埋点整体不存在，
///   `activity_snapshot()` 回 `available=false` 并给出原因；脱敏 helper 与策略不受裁切。
///
/// 呈现面由 `ExportSurface` 选择：缺省 `Timeline`。字段文本经 `present_field()` / `present_url()`
/// 折叠后交出，故快照与事件回调里不会出现凭据原值；`Secret` 类字段在任何策略下都不可放大。
///
/// @code
/// class Counter final : public aurora::net::http::NetEventSink {
///   public:
///     auto on_exchange_end(const aurora::net::http::HttpActivityEntry &entry) noexcept -> void override {
///         ++completed_;
///         last_status_ = entry.status;
///     }
///     std::uint64_t completed_{0};
///     std::uint16_t last_status_{0};
/// };
///
/// auto counter = std::make_shared<Counter>();
/// aurora::net::http::subscribe_events(counter);
/// // …发起交换…
/// const auto snapshot = aurora::net::http::activity_snapshot();
/// @endcode
/// @note Thread: 见各函数；存储与总线内部加锁，回调在锁外调用。
/// @note Side-effects: 见各函数（安装策略、写配置、清空存储）。
/// @note Rebuildable: no

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/net/http.h"
#include "aurora/net/observability.h"

namespace aurora::net::http {

/// @brief 一次交换的阶段。DNS 折在 Connect 内；复用连接没有前三枚。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
enum class HttpStage : std::uint8_t {
    Connect,  ///< DNS + TCP + 代理隧道；DNS 失败也记本阶段
    TlsHandshake,  ///< TLS 握手完成
    ConnectionAcquired,  ///< 传输就绪：新连接在 Connect/TLS 之后，复用连接的首枚 mark
    RequestSent,  ///< 头与体全部交给传输层
    FirstByte,  ///< 读到应答头首字节
    BodyDone,  ///< 最终一跳的线上体读完；重定向跳 / 主动停读 / WS 握手不标
};

/// @brief 阶段名的稳定小写令牌（日志与 JSON 用，跨进程只认这枚文本）。
/// @param stage 阶段。
/// @return `"connect"` / `"tls_handshake"` / `"connection_acquired"` / `"request_sent"` /
///         `"first_byte"` / `"body_done"` 之一，恒非空。
[[nodiscard]] auto http_stage_text(HttpStage stage) -> std::string_view;

/// @brief 一枚已到达的阶段及其相对时刻。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct StageMark {
    HttpStage stage{HttpStage::Connect};  ///< 阶段
    std::chrono::microseconds offset{0};  ///< 相对本跳起手的偏移（steady_clock）
};

/// @brief 一次物理往来的阶段时间线（`timeline_of()` / `timelines_of()` 的返回值）。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct HttpRequestTimeline {
    std::uint64_t exchange_id{0};  ///< 进程内唯一、递增；配对与排序的主键
    std::string request_id;  ///< X-Request-Id 的值；未带标识时为空
    std::uint16_t hop{0};  ///< 重定向跳序号，0 起
    std::uint16_t pass{0};  ///< 物理尝试序号，0 起（重试与凭据重放都递增）
    std::chrono::microseconds total{0};  ///< 本跳耗时；在途时为取快照时的已耗时
    bool in_flight{false};  ///< 是否仍在途
    bool reused_connection{false};  ///< 是否复用池内连接
    std::vector<StageMark> marks;  ///< 已到达阶段；缺席即本跳未经历
};

/// @brief 一条网络活动记录：谁、往哪儿、走到哪个阶段、结局是什么。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct HttpActivityEntry {
    std::uint64_t exchange_id{0};  ///< 进程内唯一、递增
    std::string request_id;  ///< X-Request-Id 的值；未带标识时为空
    std::string method;  ///< 请求方法
    std::string path;  ///< 呈现后的 origin-form（含查询串）
    std::string host;  ///< 目标主机（含端口；永不含用户信息段）
    std::uint16_t hop{0};  ///< 重定向跳序号，0 起
    std::uint16_t pass{0};  ///< 物理尝试序号，0 起
    std::uint16_t status{0};  ///< 收到应答头则为其状态码；未收到为 0（体失败仍保留）
    std::string error_code;  ///< 失败码；成功为空
    std::optional<HttpErrorClass> error_class;  ///< 失败归因；2xx 为空，≥400 为 Status
    std::optional<HttpStage> failed_stage;  ///< 失败时正在进行的阶段；本地预检拒绝为空
    std::chrono::microseconds elapsed{0};  ///< 本跳耗时；在途为取快照时已耗时
    std::uint64_t sent{0};  ///< 本跳交给传输层的字节
    std::uint64_t received{0};  ///< 本跳读回的线上字节；缓存命中为交付体字节
    bool in_flight{false};  ///< 是否仍在途
    bool abandoned{false};  ///< 协程帧未收摊即被销毁
    bool reused_connection{false};  ///< 本跳是否复用池内连接
    bool from_cache{false};  ///< 新鲜缓存命中；304 重验证为 false
    bool truncated{false};  ///< 某个文本字段因上限被替换或截断
};

/// @brief 进程级活动配置；读写都经 `activity_options()` / `set_activity_options()`。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct ActivityOptions {
    bool record = true;  ///< 缺省开：可观测性不配置也成立
    std::uint32_t capacity = 64;  ///< 环形容量（在途 + 已完成共用），钳到 [1, 1024]
    std::uint32_t max_value_chars = 256;  ///< 单字段文本上限，钳到 [32, 4096]
};

/// @brief 一次活动快照。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct ActivitySnapshot {
    bool available{true};  ///< Release 下为 false
    std::string reason;  ///< available == false 的原因
    std::vector<HttpActivityEntry> entries;  ///< 最新在前
    std::uint64_t dropped{0};  ///< 因容量 / 缩容被淘汰的记录数
    std::uint64_t truncated{0};  ///< 至少一个字段被截断的记录数
};

/// @brief 事件消费者；三枚回调缺省空实现，宿主只覆盖关心的那一枚。
/// @note Thread: 可能来自发起线程或主线程，且不同交换可并发；实现须线程安全。
/// @note 回调须快、不得抛、不得做 I/O、不得重入 net；entry 仅在本次调用期间有效，留存请复制。
/// @note Side-effects: 由实现决定（库只按订阅时的弱引用调用）。
class NetEventSink {
  public:
    NetEventSink() = default;
    NetEventSink(const NetEventSink &) = delete;
    auto operator=(const NetEventSink &) -> NetEventSink & = delete;
    NetEventSink(NetEventSink &&) = delete;
    auto operator=(NetEventSink &&) -> NetEventSink & = delete;
    virtual ~NetEventSink() = default;

    /// @brief 一次物理往来起手：记录已入表，此时尚无任何阶段与结局。
    /// @param entry 起手时的活动条目（呈现面为 Timeline）。
    virtual auto on_exchange_begin(const HttpActivityEntry &entry) noexcept -> void { (void)entry; }

    /// @brief 一枚阶段到达；与存储同步收到，即使记录已被容量淘汰或清空也会照常发出。
    /// @param exchange_id 本跳的主键。
    /// @param mark 阶段与相对偏移。
    virtual auto on_stage(std::uint64_t exchange_id, const StageMark &mark) noexcept -> void {
        (void)exchange_id;
        (void)mark;
    }

    /// @brief 一次物理往来收摊（成功、失败、取消与协程帧销毁都恰发一枚）。
    /// @param entry 收摊时的完整条目（呈现面为 Timeline）。
    virtual auto on_exchange_end(const HttpActivityEntry &entry) noexcept -> void { (void)entry; }
};

/// @brief 订阅进程级事件；宿主放手（weak 失效）即自动退订。
/// @param sink 消费者；只持弱引用，不延长其寿命。
/// @note Thread: thread-safe。
/// @note Side-effects: 追加一名订阅者并顺带清理已失效的订阅。
auto subscribe_events(std::weak_ptr<NetEventSink> sink) -> void;

/// @brief 读进程级活动配置（拷贝，线程安全）。
/// @return 当前配置；Release 构建下恒为缺省值。
[[nodiscard]] auto activity_options() -> ActivityOptions;

/// @brief 写进程级活动配置；缩容立即从最旧开始淘汰并累加 dropped，record=false 只停新记录。
/// @param options 新配置；两枚容量值越界即钳到声明区间。
/// @note Thread: thread-safe。
/// @note Side-effects: 替换进程级配置；缩容可能立即丢弃记录。
auto set_activity_options(ActivityOptions options) -> void;

/// @brief 清空全部记录（含在途）并归零 dropped / truncated；此后迟到的 end 被存储忽略。
/// @note Thread: thread-safe。
/// @note Side-effects: 丢弃全部记录并重置计数器；总线不补造收摊事件。
auto clear_network_activity() -> void;

/// @brief 活动快照；surface 选呈现面，limit=0 为全部，>0 为最新 N 条。
/// @param surface 呈现面；字段文本按该面的策略折叠。
/// @param limit 返回条数上限；`0` = 全部，其余为最新 N 条（不影响计数器）。
/// @return 最新在前的快照；Release 构建下 `available=false`。
[[nodiscard]] auto activity_snapshot(ExportSurface surface = ExportSurface::Timeline, std::uint32_t limit = 0)
    -> ActivitySnapshot;

/// @brief 同一 request_id 下 exchange_id 最大的一跳（含在途）；空/未知回 nullopt。
/// @param request_id 链路标识；空串或未知值回 `nullopt`。
/// @return 最新一跳的时间线；未知回 `nullopt`。
[[nodiscard]] auto timeline_of(std::string_view request_id) -> std::optional<HttpRequestTimeline>;

/// @brief 同一 request_id 下的全部跳，按 exchange_id 升序；空/未知回空数组。
/// @param request_id 链路标识；空串或未知值回空数组。
/// @return 按 `exchange_id` 升序的全部跳。
[[nodiscard]] auto timelines_of(std::string_view request_id) -> std::vector<HttpRequestTimeline>;

}  // namespace aurora::net::http
