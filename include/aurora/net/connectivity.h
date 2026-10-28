#pragma once

/// @brief 网络链路态：同步采集的事实、不对称状态机与变更订阅。
/// @file net/connectivity.h
///
/// 本头回答一个问题：**「本机现在有没有一条能出去的缺省链路」**。它只报事实，不做编排，
/// 也**不是发起侧的闸**——`RetryPolicy` 与 `Scheduler` 的地盘一寸不变（见下「边界」段）。
///
/// 三条定案决定了本头的全部形状：
///
/// 1. **采集是同步读事实，失败只插队、不判决。** 三平台的原生连通性事件各有硬阻塞
///    （Win32 给的是 waitable `HANDLE`、进不了 `WSAPoll` 的句柄数组；Linux 的 `rtnetlink`
///    要求平台 socket 头出现在既有落点之外；Windows 上真正回答「有无外网」的
///    `INetworkListManager` 是 COM），合计代价是「至少一条自有线程或一套 COM」。同步采集
///    把它压成一枚无句柄、无回调、可在引擎线程上调用的查询，故**零新线程、跨平台同形、
///    可在库内注入替身做逐值单测**；感知延迟的下限由「失败插队」补回来——一次真失败的
///    往返就是一次最诚实的探测。
/// 2. **三档而不是四档。** 「有链路、无外网」这一档三平台语义各不相同，开档等于承诺一份
///    只有个别平台说得清的语义。`Online` 因此收窄为「存在可用的缺省链路」，**不等于互联网
///    可达**；captive portal、VPN 策略路由、DNS 可用性都不在三态语义内。
/// 3. **`Unknown` 是一条独立的态**，既不向 `Online` 也不向 `Offline` 折算。把它读成离线
///    会让界面显示一个假故障；读成在线会让宿主停止退避。两侧代价不等，故阈值也不等。
///
/// **边界（最容易被做错的一条）**：探测口径与真实路径不等价——服务可以只在链路本地可达、
///    VPN 的路由表在探测面里看不见、容器里读不到任何有效拓扑。拿它当闸等于把可交付的请求
///    变成**库侧自己发起的失败**，而那类失败对端不留痕、时间线上看不出根因。闸的位置已经
///    在 `NetPolicy` 与能力闸那一层，本件的产物只是读者可查询的事实 + 变更通知。
///
/// @code
/// const auto fact = aurora::net::ConnectivityMonitor::instance().snapshot();
/// if (fact.state == aurora::net::NetworkState::Offline) {
///     show_offline_badge();  // 只作提示，不拦截任何请求
/// }
///
/// const auto handle = aurora::net::ConnectivityMonitor::instance().subscribe(
///     [](const aurora::net::ConnectivityChange &change) {
///         apply(change.current.state);
///     });
/// @endcode
/// @note Thread: 见各函数；内部一把互斥量，订阅者回调在锁外调用。
/// @note Side-effects: 见各函数（登记定时采样、写进程级事实）。

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "aurora/core/result.h"

namespace aurora::net {

/// @brief 链路三态：无源 / 有缺省链路 / 明确读到无缺省路由。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
enum class NetworkState : std::uint8_t {
    Unknown,  ///< 无源 / 读不到 / 尚未采样；既不向 Online 也不向 Offline 折算
    Online,  ///< 存在可用缺省链路；**不承诺互联网可达**
    Offline,  ///< 仅由探测腿明确读到无可用缺省路由时成立
};

/// @brief 编译期采集腿令牌：这条构建用的是哪一路 OS 表 / 伪文件 / 动态存储。
///
/// 构造后**不随读数成败变化**——它表的是「本构建有没有这条腿」，不是「上次读成功没有」。
/// 需要细节的场合读 `ConnectivityFact::source`，不要拿读数去反推。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
enum class ConnectivitySource : std::uint8_t {
    None,  ///< 本构建没有采集腿（wasm、BSD / Android 一类）
    IpHelper,  ///< Windows 路由 / 接口表
    DynamicStore,  ///< macOS 动态存储全局 IPv4 / IPv6 键
    LinkFiles,  ///< Linux 路由伪文件 + sysfs `operstate`
};

/// @brief 一次变更的成因；一次发布只带一枚，多个触发合并时**首因保留**。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
enum class ConnectivityChangeCause : std::uint8_t {
    SubscriptionStart,  ///< 订阅触发的首采（未被合并时）
    PeriodicSample,  ///< 周期拍
    FailureEvidence,  ///< 失败触发的插队拍
    SuccessfulConnect,  ///< 成功建连的正证，或它触发的插队拍
    HostRefresh,  ///< `refresh()` / `set_options()` 的宿主动作
    StalenessMarked,  ///< `stale` 的 false→true 标记
};

/// @brief 链路态事实快照：值类型，逐值拷贝语义。
///
/// `generation` 只在**对外发布的变更**上单调 +1（状态转换或 `stale` 翻转）；`snapshot()` 重算出的
/// `stale` 是派生读数，**不推进代际**。`since` 只在 `state` 变化时更新，初值取 monitor 建立时刻。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct ConnectivityFact {
    NetworkState state = NetworkState::Unknown;  ///< 当前三态
    ConnectivitySource source = ConnectivitySource::None;  ///< 编译期采集腿令牌
    std::chrono::steady_clock::time_point since;  ///< 当前 `state` 的起始时刻（非 epoch 零点）
    std::uint64_t generation = 0;  ///< 已发布通知的序列计数
    bool stale = true;  ///< 距上次成功采样或合格正证超过 3× 采样周期
    std::optional<std::string> last_failure;  ///< 冻结错误码 slug，只做展示；转入 Online 时清空
};

/// @brief 一枚订阅者收到的变更：前值与现值之差。
///
/// `previous` 是**该订阅者**上次收到的 `current`（首条为订阅时刻的 `snapshot()`）——不是全局
/// 上一枚事实。合并待投递变更时以最后一次为准，但 `previous` 仍锚在订阅者已见值上。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct ConnectivityChange {
    ConnectivityFact previous{};  ///< 本订阅者上次收到的 `current`
    ConnectivityFact current{};  ///< 本次要投递的事实
    ConnectivityChangeCause cause = ConnectivityChangeCause::SubscriptionStart;  ///< 成因（首因保留）
};

/// @brief 采样旋钮；越界一律钳住而不是报错。
///
/// 缺省 `5000ms / 1 / 2`，经 `ConnectivityMonitor::defaults()` 在声明处可见
/// （`SPEC.API.SENSIBLE-DEFAULTS.001`）。`stale` 阈值 = 3× `sample_interval` 写定、**不设旋钮**，
/// 否则两枚旋钮能配出自相矛盾的组合。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct ConnectivityOptions {
    std::chrono::milliseconds sample_interval{5000};  ///< 周期采样间隔，钳到 `[100ms, 1h]`
    std::uint32_t offline_evidence = 1;  ///< 转入 `Offline` 所需合格探测读数，钳到 `[1, 16]`
    std::uint32_t online_evidence = 2;  ///< 转入 `Online` 所需一致正证，钳到 `[1, 16]`
};

/// @brief 订阅句柄：move-only RAII，析构即注销。
///
/// 逐字沿用 `EventSourceRegistration` 已定案的三条契约：析构即注销且幂等；移动后源句柄不再
/// 二次注销；已入队的一次尾批仍抵达，且 `cancel_pending()` 与析构互不隐含。
/// @note Thread: 见各函数；默认可在任意线程析构。
/// @note Side-effects: 析构会摘除本订阅。
class ConnectivityRegistration {
  public:
    /// @brief 构造一枚空句柄：不登记、不持有订阅。
    /// @note Thread: 任意线程
    /// @note Side-effects: none
    ConnectivityRegistration() = default;

    /// @brief 不可拷贝：注销权唯一，双重注销会破坏订阅表。
    ConnectivityRegistration(const ConnectivityRegistration &) = delete;
    /// @brief 不可拷贝赋值。
    auto operator=(const ConnectivityRegistration &) -> ConnectivityRegistration & = delete;

    /// @brief 转移订阅所有权；源句柄不再二次注销。
    /// @param other 源句柄。
    /// @note Thread: 任意线程
    /// @note Side-effects: 源句柄转为空态。
    ConnectivityRegistration(ConnectivityRegistration &&other) noexcept;

    /// @brief 转移订阅所有权；先注销本句柄现持订阅，再接管源的。
    /// @param other 源句柄。
    /// @return 本句柄引用。
    /// @note Thread: 任意线程
    /// @note Side-effects: 注销本句柄原订阅，源句柄转为空态。
    auto operator=(ConnectivityRegistration &&other) noexcept -> ConnectivityRegistration &;

    /// @brief 析构：注销本订阅（幂等；抛出型订阅者由此摘除）。
    /// @note Thread: 任意线程
    /// @note Side-effects: 从进程级订阅表摘除。
    ~ConnectivityRegistration();

    /// @brief 订阅是否仍生效。
    /// @return 已登记且未被摘除时 true；抛出型订阅者被摘除后转假。
    [[nodiscard]] auto is_registered() const -> bool;

    /// @brief 本订阅的序号：订阅表内的稳定标识，用于诊断与日志关联。
    /// @return 登记时分配的令牌；空句柄为 0。
    [[nodiscard]] auto token() const -> std::uint64_t;

    /// @brief 作废已排队的投递，不注销订阅。
    ///
    /// 与析构互不隐含：调它不注销，析构也不必先调它。抛出的订阅者回调由投递处兜住，
    /// 不穿破采样循环与任何在途交换。
    /// @note Thread: thread-safe
    /// @note Side-effects: 清掉本订阅待投递项。
    auto cancel_pending() -> void;

  private:
    friend class ConnectivityMonitor;

    /// @brief 订阅者共享状态（投递开关闭、待投递项、订阅者已见值）。
    struct State;

    /// @brief 由 `ConnectivityMonitor::subscribe()` 建立的句柄。
    /// @param state 订阅者共享状态。
    /// @param token 登记令牌。
    ConnectivityRegistration(std::shared_ptr<State> state, std::uint64_t token);

    std::shared_ptr<State> state_;  ///< 订阅者共享状态；空 = 空句柄
    std::uint64_t token_ = 0;  ///< 登记令牌
};

/// @brief 进程级链路态监视器：周期采样 + 失败插队，产出三态与变更通知。
///
/// 契约要点：
/// - **单例**：`instance()`，函数内静态构造并**刻意不析构**（与 `shared_engine()` 同口径），
///   避开静态拆除阶段与引擎 / 订阅表的次序问题。
/// - **`snapshot()` 恒可读**：只做一次加锁拷贝与 `stale` 重算，**绝不触发 I/O 或采样**，
///   也不发布任何通知。
/// - **定时复用引擎自己的定时器**：`post_after` 的到期时刻按 `steady_clock` 计算，与
///   `wait_ready` 共用同一条循环，**不额外起线程**。
/// - **无撤销口**：`post_after` 不支持取消，故用 epoch + 共享 live 标志让旧闭包到期时
///   no-op（与 `http_client.cpp` 的 `StageTimer` 同法）。
/// - **响应式一侧到此为止**：回调恒经 `aurora::detail::post_to_main` 回投主线程，
///   不提供 `Resource` / 流式信号形态（分层与线程不变量共同堵死）。
/// - **本类无成员状态**：事实、订阅表与定时登记都在库内运行时
///   （`src/aurora/net/detail/connectivity_state.h` 的 `ConnectivityRuntime`）里，故建连
///   旁证入口 `detail::note_connection_outcome` 无需特权即可触及状态机。
/// @note Thread: 全部成员可跨线程调用；采样体只在引擎线程执行。
/// @note Side-effects: 登记定时采样、写进程级事实、向主线程投递回调。
class ConnectivityMonitor {
  public:
    /// @brief 进程级单例。
    /// @return 全局唯一监视器引用；本调用本身不启动任何线程。
    [[nodiscard]] static auto instance() -> ConnectivityMonitor &;

    /// @brief 取当前事实快照。
    ///
    /// 恒可读、不阻塞、不做 I/O、不采样、不发布通知；`stale` 按「距上次成功采样或合格正证
    /// 超过 3× 采样周期」在读取时重算。从未采样时回 `Unknown` + 编译期腿令牌 + `stale=true` +
    /// `generation=0`，`since` 取 monitor 建立时刻。
    /// @return 当前事实的值拷贝。
    /// @note Thread: thread-safe
    /// @note Side-effects: none
    [[nodiscard]] auto snapshot() const -> ConnectivityFact;

    /// @brief 取当前采样旋钮（已是钳后值）。
    /// @return 生效中的选项。
    /// @note Thread: thread-safe
    /// @note Side-effects: none
    [[nodiscard]] auto options() const -> ConnectivityOptions;

    /// @brief 改采样旋钮：自下一轮采样起效。
    ///
    /// 不重置在途证据计数、不立即转换状态。有采集腿时安排一次 `HostRefresh` 采样（已有待
    /// 采样轮次则合并；按引擎惰性启动口径可能拉起线程），无腿时只存值。登记失败只清 pending、
    /// 不报错（故无返回值）。
    /// @param options 期望旋钮；越界项被钳到合法区间，`options()` 回钳后值。
    /// @note Thread: thread-safe
    /// @note Side-effects: 可能登记一次引擎定时器。
    auto set_options(ConnectivityOptions options) -> void;

    /// @brief 缺省旋钮：`5000ms / 1 / 2`。
    /// @return 缺省值。
    /// @note Thread: thread-safe
    /// @note Side-effects: none
    [[nodiscard]] static auto defaults() -> ConnectivityOptions;

    /// @brief 请求一次采样：在引擎线程安排**恰好一次**。
    ///
    /// 已有待采样轮次则合并、首因保留。本腿无采集源回 `net-platform-unavailable`（不得回
    /// OK 后永不通知）。返回值只表「请求已受理」，结果经 `snapshot()` 或订阅回调抵达。
    /// @return 已受理为 OK；本腿无源为 `net-platform-unavailable`。
    /// @note Thread: thread-safe
    /// @note Side-effects: 可能登记一次引擎定时器（含引擎惰性启动）。
    [[nodiscard]] auto refresh() -> Result<void>;

    /// @brief 订阅链路态变更。
    ///
    /// 回调按值收取；空回调回 `net-option-failed`。有采集腿时登记成功即安排一次
    /// `SubscriptionStart` 采样；无腿但有证据通路的平台只登记、等证据驱动。
    /// **返回前绝不回调**。
    /// @param callback 变更回调；在投递器线程（装有主线程投递器时为主线程）执行。
    /// @return 句柄为 OK；空回调为 `net-option-failed`；无源且无证据通路为
    ///         `net-platform-unavailable`。
    /// @note Thread: thread-safe
    /// @note Side-effects: 登记订阅，可能安排一次采样。
    [[nodiscard]] auto subscribe(std::function<void(ConnectivityChange)> callback) -> Result<ConnectivityRegistration>;

  private:
    ConnectivityMonitor() = default;
};

/// @brief 链路三态的稳定小写令牌。
/// @param state 三态之一。
/// @return `"unknown"` / `"online"` / `"offline"` 之一，恒非空。
/// @note Thread: thread-safe
/// @note Side-effects: none
[[nodiscard]] auto network_state_text(NetworkState state) -> std::string_view;

/// @brief 采集腿令牌的稳定小写文本。
/// @param source 采集腿令牌。
/// @return `"none"` / `"ip_helper"` / `"dynamic_store"` / `"link_files"` 之一，恒非空。
/// @note Thread: thread-safe
/// @note Side-effects: none
[[nodiscard]] auto connectivity_source_text(ConnectivitySource source) -> std::string_view;

/// @brief 变更成因的稳定小写令牌。
/// @param cause 成因。
/// @return `"subscription_start"` / `"periodic_sample"` / `"failure_evidence"` /
///         `"successful_connect"` / `"host_refresh"` / `"staleness_marked"` 之一，恒非空。
/// @note Thread: thread-safe
/// @note Side-effects: none
[[nodiscard]] auto connectivity_change_cause_text(ConnectivityChangeCause cause) -> std::string_view;

}  // namespace aurora::net
