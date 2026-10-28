#pragma once

/// @brief 链路态判据层与进程级运行时：状态机裁决 + 订阅表 + 定时登记。
/// @file net/detail/connectivity_state.h
///
/// 与 `src/aurora/net/detail/connectivity_probe.h`（采集接缝）相邻而分工不同：接缝回答「本
/// 机构建有没有这条腿、这一拍读到什么」，本件回答「这些读数该落成什么态、要不要通知谁」。
///
/// 运行时（`ConnectivityRuntime`）刻意放在内部层而不是公共面：`note_connection_outcome()`
/// 由全库唯一建连出口调用，它必须够得着状态机与订阅表；若把状态锁在公共面里，就得为它开
/// 友元或额外的公共入口。
///
/// 两条不变量（本件不越界）：
/// - **平台 socket 头唯一落点**仍是 `src/aurora/net/socket.cpp`，本件**不含任何平台头**。
/// - **零出站 I/O**：采集只读 OS 表 / 伪文件 / 动态存储，不得 `connect` / `sendto` / `getaddrinfo`。
///
/// @note Thread: 运行时自带一把互斥量，全部入口可跨线程调用；采样体只在引擎线程执行。
/// @note Side-effects: 采样读 OS 状态并经投递器回调订阅者。
#include <mutex>
#include <vector>

#include "aurora/net/detail/connectivity_probe.h"

namespace aurora::net::detail {

/// @brief 注入时钟：返回当前 `steady_clock` 时刻。
using ConnectivityClockFn = std::function<std::chrono::steady_clock::time_point()>;

/// @brief 一步判定的产出：要不要发布、要不要安排采样。
struct ConnectivityTransition {
    std::optional<ConnectivityChange> change;  ///< 待发布变更；空 = 本次不发布
    bool want_sample = false;  ///< 是否需要安排一次（合并后的）采样
};

/// @brief 纯逻辑状态机：三态转换与证据计数。
///
/// 计数规则（四条，由用例固定，实现不得自由解释）：
/// - 反向证据清零对方计数：一枚在线读数清离线计数，一枚合格离线读数清在线计数；
/// - 状态转换清零两侧；
/// - `stale` 翻转清零两侧；
/// - 读失败（`nullopt`）**不计数也不清零**。
///
/// 阈值不对称（两侧代价不等）：转入 `Offline` 需 `offline_evidence` 枚合格的**探测**读数
/// （缺省 1）；`Unknown→Online` 一枚正证即可；`Offline→Online` 需 `online_evidence` 枚
/// 一致正证（缺省 2）。误判离线只是界面多一枚不准的提示；误判在线会让宿主**停止退避
/// 并立刻重试**，在真断链窗口里把一次故障放大成一片刷屏失败——故阈值也不等。
/// @note Thread: 非线程安全（由 `ConnectivityRuntime` 的互斥量保护）
/// @note Side-effects: none
class ConnectivityMachine {
  public:
    /// @brief 构造参数。
    struct Config {
        ConnectivitySource source = ConnectivitySource::None;  ///< 编译期采集腿令牌
        ConnectivityOptions options{};  ///< 采样旋钮（构造时被钳住）
        ConnectivityClockFn now = std::chrono::steady_clock::now;  ///< 注入时钟
    };

    /// @brief 构造一枚状态机；旋钮在此被钳到合法区间。
    /// @param config 构造参数。
    explicit ConnectivityMachine(Config config);

    /// @brief 析构：状态体（Pimpl `State`）在本件实现文件内才完整，故析构必须留在
    ///         `connectivity_state.cpp` 定义，避免在使用点（如纯逻辑单测 TU）因 `State`
    ///         不完整而实例化 `unique_ptr` 的删除器失败。
    ~ConnectivityMachine();

    ConnectivityMachine(const ConnectivityMachine &) = delete;
    auto operator=(const ConnectivityMachine &) -> ConnectivityMachine & = delete;
    ConnectivityMachine(ConnectivityMachine &&) = delete;
    auto operator=(ConnectivityMachine &&) -> ConnectivityMachine & = delete;

    /// @brief 编译期采集腿令牌（构造后恒定）。
    /// @return 本机构建所用的采集腿。
    [[nodiscard]] auto source() const -> ConnectivitySource;

    /// @brief 当前采样旋钮（钳后值）。
    /// @return 生效中的选项。
    [[nodiscard]] auto options() const -> ConnectivityOptions;

    /// @brief 改采样旋钮：钳后存值，自下一轮采样起效。
    /// @param options 期望旋钮；越界项被钳到合法区间。
    /// @note Thread: 由持有方串行调用
    /// @note Side-effects: 仅改本机的旋钮
    auto set_options(ConnectivityOptions options) -> void;

    /// @brief 取当前事实（含按同一公式重算的 `stale`）。
    /// @return 事实的值拷贝；不发布通知、不触发采样。
    /// @note Thread: 由持有方串行调用
    /// @note Side-effects: none
    [[nodiscard]] auto fact() const -> ConnectivityFact;

    /// @brief 落一次探测读数（周期拍与插队拍共用此处）。
    /// @param online 有可用缺省路由 / 没有 / 读不到（空值）。
    /// @param cause 本拍的成因；多个触发合并时首因保留，故由调用方带来。
    /// @return 转换与采样安排。
    /// @note Thread: 由持有方串行调用
    /// @note Side-effects: none
    auto apply_probe(std::optional<bool> online, ConnectivityChangeCause cause) -> ConnectivityTransition;

    /// @brief 落一枚建连正证（回环对端不计正证也不采样）。
    /// @param loopback_peer 对端是否为回环地址。
    /// @return 转换与采样安排。
    /// @note Thread: 由持有方串行调用
    /// @note Side-effects: none
    auto note_connect_success(bool loopback_peer) -> ConnectivityTransition;

    /// @brief 落一枚建连失败：只触发采样与展示字段，永不计负证。
    /// @param code 失败错误码；`Cancelled` / `Invalid` 一律无动作。
    /// @return 转换与采样安排。
    /// @note Thread: 由持有方串行调用
    /// @note Side-effects: none
    auto note_connect_failure(ErrorCode code) -> ConnectivityTransition;

    /// @brief 静止期到点则翻转 `stale` 并发布一次（未到点 / 已发布则无动作）。
    /// @return 转换与采样安排。
    /// @note Thread: 由持有方串行调用
    /// @note Side-effects: none
    auto mark_stale_if_due() -> ConnectivityTransition;

    /// @brief 复位到「从未采样」形态（仅测试用）。
    /// @note Thread: 仅用例侧串行调用
    /// @note Side-effects: 清事实、两侧计数与陈旧位
    auto reset_for_test() -> void;

  private:
    /// @brief 按「距上次成功采样或合格正证超过 3× 采样周期」重算陈旧位（纯派生读数）。
    /// @return 当前是否视为陈旧。
    /// @note Thread: 由持有方串行调用
    /// @note Side-effects: none
    [[nodiscard]] auto stale_now() const -> bool;

    /// @brief 落一次状态转换：更新 `state` / `since` / `generation`，清零两侧证据计数。
    /// @param next 目标态。
    /// @param at 转换时刻。
    /// @param cause 成因（首因保留）。
    /// @param previous 转换前订阅者可见的事实。
    /// @return 待发布变更。
    /// @note Thread: 由持有方串行调用
    /// @note Side-effects: none
    [[nodiscard]] auto apply_transition(NetworkState next, std::chrono::steady_clock::time_point at,
                                        ConnectivityChangeCause cause, const ConnectivityFact &previous)
        -> ConnectivityChange;

    struct State;
    std::unique_ptr<State> state_;
};

/// @brief 一枚订阅：回调、共享 live 标志、待投递项与「订阅者已见值」。
struct Subscription {
    std::uint64_t token = 0;
    std::function<void(ConnectivityChange)> callback;
    std::shared_ptr<bool> live = std::make_shared<bool>(true);  ///< 析构 / 注销即转假
    bool has_pending = false;  ///< 是否有未投递的变更
    ConnectivityChange pending{};  ///< 待投递项（合并后只留最后一枚 current）
    ConnectivityFact seen{};  ///< 订阅者已见值，即下一枚 `previous`
    bool has_seen = false;  ///< `seen` 是否已被写入
};

/// @brief 进程级运行时：状态机 + 订阅表 + 定时登记（全件唯一一把互斥量）。
struct ConnectivityRuntime {
    mutable std::mutex mutex;
    ConnectivityMachine machine{ConnectivityMachine::Config{.source = compiled_source()}};
    std::vector<std::shared_ptr<Subscription>> subscriptions;
    std::uint64_t next_token = 1U;
    std::uint64_t epoch = 0U;  ///< 每登记一拍 +1；旧闭包据此作废
    std::shared_ptr<bool> sample_live = std::make_shared<bool>(true);  ///< 当前一拍的 live 标志
    bool sample_pending = false;  ///< 是否已有待采样轮次（所有触发共用同一枚）
};

/// @brief 取进程级运行时（函数内静态、刻意不析构）。
/// @return 全局唯一运行时引用。
[[nodiscard]] auto connectivity_runtime() -> ConnectivityRuntime &;

/// @brief 落一次采样并把发布的变更并进各订阅者的待投递项；在引擎线程执行。
/// @param epoch 本拍的 epoch；与当前不符即整拍 no-op。
/// @param live 本拍的 live 标志；为假即 no-op。
/// @param cause 本拍成因。
auto connectivity_sample(std::uint64_t epoch, const std::shared_ptr<bool> &live, ConnectivityChangeCause cause) -> void;

/// @brief 安排一次采样：已有待采样轮次则合并，首因保留。
/// @param cause 本拍成因。
/// @return 已受理 / 已合并为 OK；引擎停机或无引擎语义为 `net-platform-unavailable`。
[[nodiscard]] auto connectivity_request_sample(ConnectivityChangeCause cause) -> Result<void>;

/// @brief 登记一枚订阅，并取回订阅时刻的事实快照。
/// @param callback 变更回调；空回调为 `net-option-failed`。
/// @return 成功为「订阅项 + 订阅时刻快照」；无源且无证据通路为 `net-platform-unavailable`。
[[nodiscard]] auto connectivity_subscribe(std::function<void(ConnectivityChange)> callback)
    -> Result<std::pair<std::shared_ptr<Subscription>, ConnectivityFact>>;

/// @brief 注销一枚订阅（幂等）。
/// @param entry 订阅项。
auto connectivity_unsubscribe(const std::shared_ptr<Subscription> &entry) -> void;

/// @brief 作废一枚订阅已排队的投递（不注销）。
/// @param entry 订阅项。
auto connectivity_cancel_pending(const std::shared_ptr<Subscription> &entry) -> void;

/// @brief 建连结局的旁证入口；由全库唯一建连出口的结局处调用一处。
///
/// 与 `set_lookup_hook()` / `set_system_proxy_hook()` / `set_engine_override()` 同思路——能改变
/// 运行期事实的开关一律留在库内、不进公共面、不占 API 预算。
/// @param connected 建连是否成功。
/// @param loopback_peer 对端是否为回环地址（是则不计正证、不采样）。
/// @param code 失败时的错误码；成功为空值。
/// @note Thread: 只在引擎线程调用
/// @note Side-effects: 可能推进状态机并向订阅者投递回调
auto note_connection_outcome(bool connected, bool loopback_peer, std::optional<ErrorCode> code) -> void;

/// @brief 采集钩子类型：无参、返回一次内部读数。
using ConnectivityProbeFn = std::function<ProbeReading()>;

/// @brief 装入 / 清除采集钩子（传空函数即清除，回落平台腿）。
/// @param hook 钩子本体。
auto set_connectivity_probe_hook(ConnectivityProbeFn hook) -> void;

/// @brief 取当前钩子（空函数表示未装入）。
/// @return 钩子的只读引用。
[[nodiscard]] auto connectivity_probe_hook() -> const ConnectivityProbeFn &;

/// @brief 复位进程级事实与订阅者可见值，回到「从未采样」形态（仅测试用）。
/// @note Thread: 仅用例侧串行调用
/// @note Side-effects: 清事实、两侧计数与陈旧位；不注销订阅
auto reset_connectivity_for_test() -> void;

}  // namespace aurora::net::detail
