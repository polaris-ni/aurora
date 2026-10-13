#pragma once

/// @brief 通用事件源注册口（CHG-010 底座切片：把「一个可等待句柄 + 就绪回调」并入宿主等待集）。
/// @file app/event_source_registry.h
///
/// 这是网络栈 L0 宿主集成层对外与对内共用的就绪通知底座：第三方非阻塞会话（libssh2、DBus
/// 一类）与库自带的 poll 线程引擎都以它为实现基座。**缺省驱动方是 `net::Engine` 的常驻线程**
/// （本库各平台腿的统一形态，不依赖各平台主循环的等待形态差异）；自带等待集的宿主改为自己等待
/// 时，调 `set_host_driven(true)` 让引擎让位，再每轮取「待等待的句柄集合」（`sources()`），
/// 等待返回后把就绪结果交回 `dispatch()`，回调即按 §18-1 的语义送达。两种形态对同一条事件源
/// **互斥**，同时启用会让一次就绪投递两次。
///
/// 本层刻意**平台无关且不依赖 `net/`**：它只搬运 `std::intptr_t` 句柄与回调，绝不出现任何
/// 平台 socket/poll 头（`winsock2.h` / `poll.h` 的仓内唯一落点是 `src/aurora/net/socket.cpp`，
/// CHG-006 不变量）。真正的「阻塞直到某些句柄就绪」经三枚可注入接缝表达：
/// - `Poller`：把一组 `WaitSource` 等待到就绪（由 net 引擎层或测试安装；未安装时
///   `wait_and_dispatch()` 返回 `net-platform-unavailable`，即 wasm 那种「不提供假降级」）。
/// - `Dispatcher`：把一次就绪回调投回主线程（缺省为就地同步执行，与 `post_to_main` 无 poster
///   时的口径一致；宿主可注入 `post_to_main` 形态）。
/// - `ChangeNotify`：条目集合变动（登记/注销）后通知常驻等待方重取快照（判据见成员声明处）。
///
/// 无 fd 等待语义的腿（wasm）在构造期即置 `event_source_supported()` 为假：`register_source()`
/// 一律返回 `net-platform-unavailable`，不排队等位、不静默收下永不被触发的条目。
///
/// 生命周期与竞态契约（提案 §18-1，规范性）：
/// - `register_source()` 返回 move-only RAII 句柄 `EventSourceRegistration`，析构即注销且**幂等**；
///   移动后源句柄转为空态，其析构不再触发二次注销。
/// - 注销语义 = **停止产生新回调**：已进入 `Dispatcher` 队列的一次尾批回调仍会抵达一次（载荷
///   是条目自身的弱所有权，宿主据此自行丢弃）；需连尾批一起作废另用 `cancel_pending()`，
///   该调用与析构互不隐含。
/// - `dispatch()` 在锁内只快照待投递闭包，**释放锁后**才逐个投递，故回调内可安全再注册/注销。
///
/// @code
/// auto reg = aurora::app::EventSourceRegistry::instance()
///                .register_source(fd, aurora::app::WaitInterest{.read = true},
///                                 [](const auto &r) { if (r.readable) { /* 处理可读 */ } });
/// if (reg) { /* 句柄存活期间回调持续送达；reg->value() 析构即注销 */ }
/// @endcode
/// @note Thread: thread-safe（表与条目状态加锁/原子；回调投递线程由 `Dispatcher` 决定）
/// @note Side-effects: `register_source` 写入进程级注册表；`dispatch` 触发已就绪条目的回调

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "aurora/core/result.h"

namespace aurora::app {

/// @brief 一个句柄想等待的就绪方向。
/// @note 具名 bool 而非位掩码：读取直观、加方向即加字段（与 `net::Capabilities` 同口径）。
struct WaitInterest {
    bool read = false;  ///< 是否等待可读（含对端 FIN / 错误）
    bool write = false;  ///< 是否等待可写（发送缓冲腾出空间，供背压）
};

/// @brief 一次就绪回报的方向结果。
/// @note Thread: thread-safe（值类型）
struct SourceReady {
    bool readable = false;  ///< 可读：有字节到达或对端已半关闭
    bool writable = false;  ///< 可写：可继续写入而不阻塞
    bool error = false;  ///< 出错：POLLERR 一类，后续读写都会失败
    bool hangup = false;  ///< 挂断：POLLHUP / 对端 FIN
};

/// @brief 交给 `Poller` 的一个等待项：句柄 + 关注方向 + 关联令牌。
/// @note `token` 由注册口分配，`Poller` 须原样在 `ReadySource` 里回填以完成关联（句柄可重复）。
struct WaitSource {
    std::intptr_t handle = -1;  ///< 平台承载的原生句柄/描述符（-1 恒无效）
    WaitInterest interest{};  ///< 关注方向
    std::uint64_t token = 0;  ///< 注册口分配的稳定令牌
};

/// @brief `Poller` 回报的一个就绪项：令牌 + 就绪结果。
struct ReadySource {
    std::uint64_t token = 0;  ///< 对应 `WaitSource::token`
    SourceReady ready{};  ///< 就绪方向
};

namespace detail {
/// @brief 注册表条目（共享状态）：被注册表、RAII 句柄、以及已投递的尾批闭包共同持有。
struct SourceEntry {
    std::uint64_t token = 0;  ///< 分配的稳定令牌
    std::intptr_t handle = -1;  ///< 承载句柄
    WaitInterest interest{};  ///< 关注方向
    std::function<void(SourceReady)> callback;  ///< 就绪回调
    std::atomic<bool> cancelled{false};  ///< `cancel_pending()` 置位：作废已投递尾批
};
}  // namespace detail

/// @brief 一次事件源注册的 move-only RAII 句柄。
/// @note Thread: 句柄本身非线程安全（单所有权）；它操作的注册表是线程安全的。
/// @note Side-effects: 析构（或移动赋值丢弃旧值）触发一次注销
class EventSourceRegistration {
  public:
    /// @brief 构造空句柄（占位；移动后的落点）。
    EventSourceRegistration() = default;
    /// @brief 析构：若仍持有登记则注销自有条目（幂等；空句柄无操作）。
    ~EventSourceRegistration();

    EventSourceRegistration(const EventSourceRegistration &) = delete;
    auto operator=(const EventSourceRegistration &) -> EventSourceRegistration & = delete;

    /// @brief 移动构造：所有权移交，源句柄转为空态（其析构因此不再触发注销）。
    /// @param other 被移交的源句柄。
    EventSourceRegistration(EventSourceRegistration &&other) noexcept;
    /// @brief 移动赋值：先注销自有句柄再接管，源转为空态。
    /// @param other 被移交的源句柄。
    /// @return 本对象引用。
    auto operator=(EventSourceRegistration &&other) noexcept -> EventSourceRegistration &;

    /// @brief 是否仍处于已注册态。
    /// @return 未析构、未移动走、未显式注销时为 true。
    [[nodiscard]] auto is_registered() const -> bool { return registry_ != nullptr; }

    /// @brief 注册口分配的稳定令牌。
    /// @return 已注册时为条目令牌；空句柄为 0。
    [[nodiscard]] auto token() const -> std::uint64_t { return entry_ ? entry_->token : 0U; }

    /// @brief 作废已投递但尚未执行的尾批回调（与析构注销互不隐含，见 §18-1）。
    ///
    /// 空句柄时无操作。与 `EventSourceRegistration` 的析构注销是两条独立语义：析构停止**新**回调
    /// 但放行已入队的一次尾批；本调用只作废尾批、不影响后续登记。
    auto cancel_pending() -> void {
        if (entry_) {
            entry_->cancelled.store(true);
        }
    }

  private:
    friend class EventSourceRegistry;
    /// @brief 由注册口在成功登记后构造。
    /// @param registry 所属注册表。
    /// @param entry 共享条目。
    EventSourceRegistration(class EventSourceRegistry *registry, std::shared_ptr<detail::SourceEntry> entry)
        : registry_(registry), entry_(std::move(entry)) {}

    /// @brief 注销并转空态（幂等）。
    auto unregister() -> void;

    class EventSourceRegistry *registry_ = nullptr;  ///< 所属注册表（空态为 nullptr）
    std::shared_ptr<detail::SourceEntry> entry_;  ///< 共享条目（空态为 null）
};

/// @brief 把可等待句柄 + 就绪回调并入宿主等待集的进程级注册口（CHG-010 L0 底座）。
/// @note Thread: thread-safe
/// @note Side-effects: none（除触发就绪回调外无进程级副作用）
class EventSourceRegistry {
  public:
    /// @brief 进程级单例（引擎与宿主共用同一张表）。
    /// @return 全局唯一实例引用。
    [[nodiscard]] static auto instance() -> EventSourceRegistry &;

    /// @brief 等待接缝：阻塞直到部分 `sources` 就绪或超时，回报就绪集合。
    /// @note 由 net 引擎层或测试安装；未安装时 `wait_and_dispatch()` 返回 `net-platform-unavailable`。
    using Poller =
        std::function<Result<std::vector<ReadySource>>(const std::vector<WaitSource> &, std::chrono::milliseconds)>;
    /// @brief 投递接缝：把一个就绪闭包投回主线程（缺省就地同步执行）。
    using Dispatcher = std::function<void(std::function<void()>)>;
    /// @brief 表变动通知接缝：条目集合发生变化（登记成功或注销生效）后调用。
    ///
    /// 常驻等待方（`net::Engine` 的 poll 线程）借此立刻重取 `sources()` 快照，而不是等一个等待
    /// 窗口才看到新增/移除的句柄；注销通知尤其要紧——宿主随后即关闭该句柄，滞留的快照会让下一次
    /// 系统调用撞上无效句柄。通知在**锁外**执行，故回调内可安全再登记/注销。
    using ChangeNotify = std::function<void()>;

    /// @brief 安装等待接缝。
    /// @param poller 就绪等待实现；空 `std::function` 表示卸载（此后 `wait_and_dispatch` 报不可用）。
    auto set_poller(Poller poller) -> void;

    /// @brief 安装投递接缝。
    /// @param dispatcher 主线程投递实现；空 `std::function` 恢复为缺省的就地同步执行。
    auto set_dispatcher(Dispatcher dispatcher) -> void;

    /// @brief 安装表变动通知接缝。
    /// @param notify 变动通知实现；空 `std::function` 表示卸载（宿主自带等待集时的常态）。
    auto set_change_notify(ChangeNotify notify) -> void;

    /// @brief 是否已装等待接缝（引擎启动时据此判断「不覆盖宿主已装的接缝」）。
    /// @return 已装为非空函数时为 true。
    [[nodiscard]] auto has_poller() -> bool;

    /// @brief 是否已装投递接缝（判据同 `has_poller()`）。
    /// @return 已装时为 true；缺省的就地同步执行视为未装。
    [[nodiscard]] auto has_dispatcher() -> bool;

    /// @brief 声明本腿是否具备事件源等待语义（§2 的 `capabilities().event_source` 运行期落点）。
    ///
    /// 自带等待集的宿主（Win32/X11/Wayland/Headless）应保持为真；无 fd 语义的腿（wasm）由
    /// 其初始化置假，此后 `register_source()` 一律返回 `net-platform-unavailable`（不假降级）。
    /// @param supported 是否支持事件源等待。
    auto set_event_source_supported(bool supported) -> void;

    /// @brief 本腿是否支持事件源等待。
    /// @return 缺省为 true；被 wasm 之类置假后为 false。
    [[nodiscard]] auto event_source_supported() const -> bool;

    /// @brief 声明「宿主自带等待集正在驱动本注册口」，据此让常驻引擎不再等待这些句柄。
    ///
    /// 一条事件源只由一个驱动方等待：缺省驱动方是 `net::Engine` 的常驻 poll 线程；置真后引擎只等
    /// 自己的唤醒通道，条目由宿主每轮取 `sources()` 快照并入自有等待集、就绪后调 `dispatch()` 投递
    /// （自带循环的宿主与第三方事件循环用这一条）。置回假则引擎重新接管。两种转变都经
    /// `ChangeNotify` 通知常驻方重取快照，不等一个等待窗口。
    /// @param driven true = 宿主驱动（引擎让位）；false = 交回引擎驱动（缺省）。
    auto set_host_driven(bool driven) -> void;

    /// @brief 当前驱动方是否为宿主自带等待集。
    /// @return 宿主驱动为 true；缺省（引擎驱动）为 false。
    [[nodiscard]] auto host_driven() const -> bool;

    /// @brief 登记一个事件源。
    ///
    /// 同一 `handle` 可被多次登记（各自独立令牌与回调）；登记即时生效，直到返回的句柄析构。
    /// @param handle 平台承载的原生句柄/描述符；-1 视为无效并拒绝。
    /// @param interest 关注方向（读/写）。
    /// @param callback 就绪回调；将按投递接缝在目标线程执行，须自行以弱令牌保护宿主。
    /// @return 成功为 RAII 句柄；本腿不支持或 `handle` 无效为 `net-platform-unavailable`。
    [[nodiscard]] auto register_source(std::intptr_t handle, WaitInterest interest,
                                       std::function<void(SourceReady)> callback) -> Result<EventSourceRegistration>;

    /// @brief 注销指定令牌（若仍存活）。由 RAII 句柄析构调用；幂等。
    /// @param token 待注销的条目令牌。
    /// @return 命中并移除为 true；令牌不存在为 false。
    auto unregister_source(std::uint64_t token) -> bool;

    /// @brief 快照当前所有存活等待项，供自带等待集的宿主（X11/Wayland/Win32）并入。
    /// @return 每项含句柄、关注方向与令牌；宿主须在 `dispatch()` 里回填同一令牌。
    [[nodiscard]] auto sources() -> std::vector<WaitSource>;

    /// @brief 投递宿主等待得到的就绪集合。
    ///
    /// 锁内仅快照待投递闭包并释放锁，再逐个经投递接缝执行；已注销令牌不再产生新回调，
    /// 但已入队的一次尾批仍会抵达（除非该句柄调用了 `cancel_pending()`）。
    /// @param ready 就绪集合（令牌 + 方向）。
    auto dispatch(const std::vector<ReadySource> &ready) -> void;

    /// @brief 无原生等待集的宿主（Headless）用的便捷循环：经等待接缝等待后投递。
    /// @param timeout 最长等待时长。
    /// @return 成功为 OK；未安装等待接缝为 `net-platform-unavailable`（不忙等、不假降级）。
    [[nodiscard]] auto wait_and_dispatch(std::chrono::milliseconds timeout) -> Result<void>;

    /// @brief 当前存活条目数（测试与自省用）。
    /// @return 已登记未注销的条目数量。
    [[nodiscard]] auto live_count() -> std::size_t;

  private:
    /// @brief 建进程级单例：无 fd 等待语义的腿（wasm）在构造期把事件源支持位置假。
    EventSourceRegistry();

    /// @brief 分配单调递增令牌（从 1 起；0 保留给空句柄）。
    [[nodiscard]] auto next_token() -> std::uint64_t;

    std::mutex mutex_;  ///< 保护 entries_/poller_/dispatcher_/change_notify_/host_driven_
    std::vector<std::shared_ptr<detail::SourceEntry>> entries_;  ///< 存活条目（线性表，规模小）
    Poller poller_;  ///< 等待接缝（可空）
    Dispatcher dispatcher_;  ///< 投递接缝（可空=就地执行）
    ChangeNotify change_notify_;  ///< 表变动通知接缝（可空=不通知）
    std::uint64_t token_counter_ = 0;  ///< 令牌计数器
    std::atomic<bool> event_source_supported_{true};  ///< 本腿是否有 fd 等待语义（wasm 构造期置假）
    std::atomic<bool> host_driven_{false};  ///< 是否由宿主自带等待集驱动（net 引擎据此让位）
};

}  // namespace aurora::app
