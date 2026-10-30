#pragma once

/// @brief 异步作业的三态响应式绑定：`Resource<T>`（一次性落定）与 `StreamResource<T>`（持续流）。
/// @file state/resource.h
///
/// 本头把「一个会异步落定的结果」变成 widget 可以直接读的信号：三态 `Loading / Ready(T) /
/// Failed(Error)`，读取即登记依赖、落定即定点刷新，对位业界的 `FutureBuilder`。
///
/// 三条设计口径值得在写代码前知道：
///
/// - **不认识网络**：本头只依赖「可取消 + 完成回投」这一既有抽象（`Task<T>`，`async.h`），
///   所以网络栈不进状态层的依赖图——异步计算、文件读取、进程间调用同样能产出 `Resource`。
/// - **一个资源 = 一个信号源**：阶段、值、错误打包成一份快照写进同一个 `State`，一次落定
///   只通知一次。三分支渲染必然同时读这三者，拆成三个信号只会让每次迁移多跑一轮重建；
///   粒度因此收在「每个资源一个信号」，不同资源之间依旧互不牵连。
/// - **结果不串台**：每次发送都推进一个代次（generation），完成回调只认自己那一代。
///   `Task::cancel()` 只能让「尚未投递」的结果闭嘴，已经排进主线程队列的那次投递照样抵达，
///   故 `reload()` 之后前序作业的迟到结果由代次守卫丢弃，不会覆盖新状态。
///
/// 取消与作用域联动：宿主卸载（资源随 widget 树析构）或被重新赋值时，最后一个持有者放手即
/// 触发内核析构，内核析构里取消前序未完成作业；完成回调持的是内核弱引用，内核已失效时直接
/// 丢弃——两条路径都不产生悬垂访问。限制照搬 `Task`：取消无法中断作业体本身，只丢弃结果。
///
/// **必须先安装主线程投递器**（`Task<T>::set_main_poster`，真实事件循环已装）：本头的状态写入
/// 发生在完成回调所在线程，未安装投递器时回调在承接作业的 worker 线程直调，写 `State` 就跨了
/// 「main-thread only」的约定。无事件循环的场景（headless / 单测）要么自装投递器，要么只在
/// 单线程作业形态下使用。
///
/// @code
/// auto user = aurora::Resource<Profile>::from(
///     aurora::async([] { return fetch_profile(id); }),
///     aurora::ResourceOptions{.timeout = std::chrono::seconds{5}});
///
/// // widget 侧：三分支读取，读取即订阅
/// if (user.phase() == aurora::ResourcePhase::Loading) { /* 骨架 */ }
/// else if (const Profile *p = user.value()) { /* 数据 */ }
/// else { /* 失败：user.error()->message */ }
///
/// user.reload();  // 前序请求自动取消；旧结果不会盖住新结果
/// @endcode

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/core/result.h"  // Error / Result（Error 连带 ErrorCode）
#include "aurora/core/thread_pool.h"  // 去抖延时通道走同一线程池（async.h 已含，此处显式列依赖意图）
#include "aurora/state/async.h"  // Task<T>、detail::post_to_main、到期看守登记
#include "aurora/state/state.h"  // State<S>：三态快照的信号源

namespace aurora {

/// @brief 三态资源的阶段；`Resource<T>` 与 `StreamResource<T>` 共用一枚枚举。
enum class ResourcePhase : std::uint8_t {
    Loading,  ///< 作业进行中：等待新结果；一次性资源重发期间可保留最近一次成功值，错误位为空
    Ready,  ///< 作业成功：值可读（流式：至少提交过一条，数据可读）
    Failed,  ///< 作业失败：错误可读；一次性资源此时值必为空
};

/// @brief 流式资源每批数据的落定策略（`StreamResource<T>` 的选项）。
enum class StreamPolicy : std::uint8_t {
    Append,  ///< 追加：本批数据接在既有列表之后（增量流，如逐条事件）
    Replace,  ///< 替换：只保留本批最后一条（整包快照流，如全量状态帧）
};

/// @brief `Resource<T>` 的行为选项；全部默认值在声明处可见。
/// @note Thread: thread-safe（纯值类型，构造后只读）
/// @note Side-effects: none
struct ResourceOptions {
    /// @brief 单次作业的超时窗口：到点仍未落定即以 `async-timeout` 错误进入 Failed。
    /// 0 或负值 = 不设超时。默认 **30 s**（需求 SPEC.API.SENSIBLE-DEFAULTS.001）。
    std::chrono::milliseconds timeout{std::chrono::seconds{30}};
    /// @brief `reload()` 的去抖窗口。默认 **0 = 不去抖**：每次调用都真发并取消前序。
    /// 非 0 时窗口内的多次 `reload()` 合并成一次发送（最后一次调用生效），窗口内不改动状态。
    std::chrono::milliseconds debounce{std::chrono::milliseconds{0}};
};

/// @brief `StreamResource<T>` 的行为选项。
/// @note Thread: thread-safe（纯值类型，构造后只读）
/// @note Side-effects: none
struct StreamOptions {
    /// @brief 提交策略，默认追加（`StreamPolicy::Append`）。
    StreamPolicy policy = StreamPolicy::Append;
};

/// @brief 一次性资源的三态快照：一个资源一次落定的全部可读信息。
///
/// 由 `Resource<T>` 经 `get()` / `subscribe(fn)` / 隐式转换到 `ReadOnlyState<ResourceSnapshot<T>>`
/// 暴露；字段一次提交，不会拆成多个独立来源。
/// @tparam T 作业值类型。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
template <typename T>
struct ResourceSnapshot {
    /// @brief 当前阶段。
    ResourcePhase phase = ResourcePhase::Loading;
    /// @brief 最近一次成功值；Ready 必有值，重发进入 Loading 时可保留旧值，Failed 时为空。
    std::optional<T> value;
    /// @brief 失败详情；仅 Failed 时有值。
    std::optional<Error> error;
};

/// @brief 流式资源的快照：已提交数据 + 最近批次字节累计 + 阶段 / 错误。
/// @tparam T 单条数据类型。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
template <typename T>
struct StreamSnapshot {
    /// @brief 当前阶段。
    ResourcePhase phase = ResourcePhase::Loading;
    /// @brief 已提交的数据（Append 策略下累计；Replace 策略下最多一条）。
    std::vector<T> items;
    /// @brief 失败详情；仅 Failed 时有值。
    std::optional<Error> error;
    /// @brief 最近一次提交批次的线上字节累计（同批多次交付的字节在此相加，不丢弃）。
    std::size_t batch_bytes = 0;
};

namespace detail {

/// @brief 延时 `d` 后执行 `action`（`reload()` 的去抖通道），与 `Task<T>::with_timeout` 共用
/// 同两条分流：worker 线程池下是一个睡 `d` 的池任务；deferred（无 pthreads）下无线程可睡，
/// 只登记到期看守，由宿主帧尾扫描触发。两条路径都在动作执行前回投主线程投递队列。
/// @param d 延时窗口；0 或负值 = 不等，直接回投。
/// @param action 到点执行的动作；须自身可跨线程安全调用（本函数只负责回投，不碰任何状态）。
inline auto schedule_after(std::chrono::milliseconds d, std::function<void()> action) -> void {
    if (d.count() <= 0) {
        post_to_main(std::move(action));  // 无需延时：与即时发送走同一条回投路径
        return;
    }
    auto fire = [action = std::move(action)]() -> void { post_to_main(action); };  // 到点后回主线程
    if (ThreadPool::default_pool().is_deferred()) {
        register_timeout_guard(std::chrono::steady_clock::now() + d, std::move(fire));
        return;
    }
    // 池任务体只有 sleep + 回投；回投闭包经 std::function 转发，命中本检查记录在案的
    // 「转入 std::function 一律判不应抛出」系统性假告警面。
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ThreadPool::default_pool().execute([fire = std::move(fire), d]() mutable -> void {
        std::this_thread::sleep_for(d);
        fire();
    });
}

/// @brief `Resource<T>` 的共享内核：三态快照信号 + 作业来源 + 前序作业句柄 + 代次守卫。
///
/// 由 `std::shared_ptr` 持有，句柄（`Resource<T>`）只是它的外壳：句柄可自由拷贝，全部拷贝共享
/// 同一内核。最后一个句柄放手时内核析构，析构里取消前序未完成作业——这就是「宿主卸载 /
/// 重新赋值 ⇒ 取消前序请求」的落点。完成回调持 `weak_ptr`，内核已亡即丢弃结果。
///
/// 锁纪律：`mutex_` 只保护簿记，**状态写入一律在锁外**——`State::set()` 会同步重跑观察者，
/// 而观察者里可能回头调用本内核的 `reload()`，锁内通知即自死锁。
/// @tparam T 作业值类型。
/// @note Thread: 状态写入限主线程；簿记字段由 `mutex_` 保护，可跨线程读写
/// @note Side-effects: none
/// @note Rebuildable: no
template <typename T>
class ResourceCore : public std::enable_shared_from_this<ResourceCore<T>> {
  public:
    /// @brief 由「作业来源」构造：每次发送调用 `producer` 产出新作业，故支持真正的重发。
    /// @param producer 作业来源；每次调用返回一个刚启动的 `Task<T>`。它在锁内被调用，
    ///                 因此不得回调回本资源（自反调用会撞上簿记锁）。
    /// @param opts 行为选项（超时与去抖窗口）。
    ResourceCore(std::function<Task<T>()> producer, ResourceOptions opts)
        : producer_(std::move(producer)), opts_(opts) {}

    /// @brief 由「已启动作业」构造：作业由调用方启动（典型为 `async(fn)`），内核只等待其结果
    /// 并落定；该形态没有可重发的来源，见 `send()` 的说明。
    /// @param job 已启动的作业句柄。
    /// @param opts 行为选项（超时与去抖窗口）。
    ResourceCore(Task<T> job, ResourceOptions opts) : job_(std::move(job)), opts_(opts) {}

    /// @brief 析构：取消仍未完成的作业，落实「最后一个持有者放手 ⇒ 前序请求被取消」。
    /// @note 此刻不可能有其它线程持着锁定的 `shared_ptr`（那会延后本析构），故读簿记无需加锁。
    /// @note 豁免 bugprone-exception-escape：`Task::cancel()` 取作业内部的 `std::mutex`，其 `lock()`
    /// 可抛 `system_error`；析构期无调用方可回报，本库不为此新增吞错路径。
    /// NOLINTNEXTLINE(bugprone-exception-escape)
    ~ResourceCore() {
        if (job_) {
            job_->cancel();
        }
    }

    /// @brief 禁拷贝：内核由 `shared_ptr` 共享，拷贝一份会造出两个代次计数器。
    ResourceCore(const ResourceCore &) = delete;
    /// @brief 禁拷贝赋值：理由同拷贝构造。
    auto operator=(const ResourceCore &) -> ResourceCore & = delete;
    /// @brief 禁移动：句柄的移动应由 `shared_ptr` 承担，内核本身原地不动。
    ResourceCore(ResourceCore &&) = delete;
    /// @brief 禁移动赋值：理由同移动构造。
    auto operator=(ResourceCore &&) -> ResourceCore & = delete;

    /// @brief 三态快照的信号源：读取即登记依赖，写入即定点刷新。
    /// @return 内部 `State` 的可变引用（写入只在锁外由本内核进行）。
    [[nodiscard]] auto signal() -> State<ResourceSnapshot<T>> & { return signal_; }

    /// @brief 发送一次作业：按选项决定即时发送还是去抖后再发。
    ///
    /// 两种构造形态的差别：有作业来源时先取消前序再产出新作业（真重发）；只有外部作业句柄时
    /// 没有可重发的来源，于是取消动作被跳过并重新等待**同一**作业——取消它就拿不回结果了。
    auto start() -> void {
        if (opts_.debounce.count() <= 0) {
            send();
            return;
        }
        std::uint64_t token = 0;
        {
            std::scoped_lock<std::mutex> lock(mutex_);
            token = ++debounce_token_;  // 后来的调用作废窗口内先前登记的发送
        }
        std::weak_ptr<ResourceCore<T>> weak = this->weak_from_this();
        detail::schedule_after(opts_.debounce, [weak, token]() -> void {
            const auto core = weak.lock();
            if (!core) {
                return;  // 宿主已卸载：不去复活
            }
            bool current = false;
            {
                std::scoped_lock<std::mutex> lock(core->mutex_);
                current = (core->debounce_token_ == token);
            }
            if (current) {
                core->send();  // 窗口内没有更新的调用：本代真发
            }
        });
    }

    /// @brief 即时发送一条作业并登记完成回调（首帧发起与去抖到点都走这里）。
    auto send() -> void {
        std::optional<Task<T>> armed;
        std::uint64_t gen = 0;
        ResourceSnapshot<T> next;
        {
            std::scoped_lock<std::mutex> lock(mutex_);
            if (producer_) {
                if (job_) {
                    job_->cancel();  // 重发：自动取消上一次未完成的作业
                }
                job_ = producer_();  // 作业来源产出一个刚启动的新作业
            }
            if (job_ && opts_.timeout.count() > 0) {
                job_->with_timeout(opts_.timeout);  // 默认 30 s；已自挂超时的作业由 delivered 单点裁决
            }
            gen = ++generation_;
            // 复位到 Loading：保留上一次的残留值（stale-while-revalidate），清掉旧错误。
            next =
                ResourceSnapshot<T>{.phase = ResourcePhase::Loading, .value = snapshot_.value, .error = std::nullopt};
            snapshot_ = next;
            armed = job_;  // 复制句柄（共享同一 AsyncState），出锁后挂接
        }
        // 先落 Loading 再挂回调：未装投递器时回调可能同步直投，顺序反过来会把结果压回 Loading。
        signal_.set(std::move(next));
        if (armed) {
            armed->then(completion(gen));
        }
    }

    /// @brief 落定一次结果：代次相符才写状态，否则判为前序作业的迟到结果直接丢弃。
    /// @param gen 该结果所属的发送代次。
    /// @param r 作业结果；成功进 Ready，失败进 Failed（值清空）。
    auto commit(std::uint64_t gen, const Result<T> &r) -> void {
        ResourceSnapshot<T> next;
        {
            std::scoped_lock<std::mutex> lock(mutex_);
            if (gen != generation_) {
                return;  // 已被新请求取代：旧结果不覆盖新状态
            }
            if (r.ok()) {
                next = ResourceSnapshot<T>{.phase = ResourcePhase::Ready, .value = r.value(), .error = std::nullopt};
            } else {
                next = ResourceSnapshot<T>{.phase = ResourcePhase::Failed, .value = std::nullopt, .error = r.error()};
            }
            snapshot_ = next;
        }
        signal_.set(std::move(next));  // 锁外通知：观察者可能重入本资源
    }

  private:
    /// @brief 构造完成回调：持内核弱引用 + 本次代次，两者任一不成立即丢弃结果。
    /// @param gen 本次发送的代次。
    /// @return 可直接交给 `Task<T>::then` 的回调。
    [[nodiscard]] auto completion(std::uint64_t gen) -> std::function<void(const Result<T> &)> {
        std::weak_ptr<ResourceCore<T>> weak = this->weak_from_this();
        return [weak, gen](const Result<T> &r) -> void {
            const auto core = weak.lock();
            if (!core) {
                return;  // 宿主已失效：丢弃回调，不产生悬垂访问
            }
            core->commit(gen, r);
        };
    }

    std::mutex mutex_;  ///< 保护下方全部簿记字段
    std::function<Task<T>()> producer_;  ///< 作业来源；外部作业句柄形态为空
    std::optional<Task<T>> job_;  ///< 当前（或唯一）作业句柄
    ResourceOptions opts_;  ///< 超时与去抖窗口
    std::uint64_t generation_ = 0;  ///< 发送代次：迟到结果的裁决依据
    std::uint64_t debounce_token_ = 0;  ///< 去抖代数：窗口内后来者作废先前的登记
    ResourceSnapshot<T> snapshot_;  ///< 当前可读信息（含跨请求的残留值）
    /// @brief 三态快照来源；显式关闭等值去重，保留每次有效落定的通知语义。
    State<ResourceSnapshot<T>> signal_{ResourceSnapshot<T>{}, always_publish};
};

/// @brief `StreamResource<T>` 的共享内核：待提交批次 + 已提交数据 + 帧内合并闸门。
/// @tparam T 单条数据类型。
/// @note Thread: 状态写入限主线程；批次字段由 `mutex_` 保护
/// @note Side-effects: none
/// @note Rebuildable: no
template <typename T>
class StreamCore : public std::enable_shared_from_this<StreamCore<T>> {
  public:
    /// @brief 以「等待写入」态构造：阶段为 Loading，直到第一批数据提交才进 Ready。
    /// @param opts 提交策略。
    explicit StreamCore(StreamOptions opts) : opts_(opts) {}

    /// @brief 交付一条数据：计入本批，并按需登记一次「提交」投递（帧内合并的入口）。
    /// @param item 本条数据（移入批次）。
    /// @param wire_bytes 该条数据的线上字节数，供 `batch_bytes()` 累计；非线上流传 0。
    auto push(T item, std::size_t wire_bytes) -> void {
        bool need_flush = false;
        {
            std::scoped_lock<std::mutex> lock(mutex_);
            if (terminal_) {
                return;  // 已报错的流是终态：后续交付丢弃，续用请另开一个流资源
            }
            pending_.push_back(std::move(item));
            pending_bytes_ += wire_bytes;
            need_flush = !flush_scheduled_;
            flush_scheduled_ = true;  // 已有投递在途：后续交付只累加，不再排投递
        }
        if (need_flush) {
            std::weak_ptr<StreamCore<T>> weak = this->weak_from_this();
            detail::post_to_main([weak]() -> void {
                const auto core = weak.lock();
                if (core) {
                    core->flush();  // 宿主已卸载则丢弃；回投在主线程，写入不越线程约定
                }
            });
        }
    }

    /// @brief 报错终止：未提交的批次丢弃，状态进 Failed，已提交数据保留。
    /// @param err 失败详情。
    auto fail(const Error &err) -> void {
        StreamSnapshot<T> next;
        {
            std::scoped_lock<std::mutex> lock(mutex_);
            terminal_ = true;
            pending_.clear();
            pending_bytes_ = 0;
            snapshot_.phase = ResourcePhase::Failed;
            snapshot_.error = err;
            next = snapshot_;
        }
        signal_.set(std::move(next));  // 锁外通知
    }

    /// @brief 流式快照的信号源：读取即登记依赖，提交即定点刷新。
    /// @return 内部 `State` 的可变引用。
    [[nodiscard]] auto signal() -> State<StreamSnapshot<T>> & { return signal_; }

  private:
    /// @brief 提交在途批次：一次 `set()` 落一批，这就是「每帧至多一次」的那一次。
    auto flush() -> void {
        std::vector<T> batch;
        std::size_t bytes = 0;
        StreamSnapshot<T> next;
        {
            std::scoped_lock<std::mutex> lock(mutex_);
            flush_scheduled_ = false;
            if (terminal_ || pending_.empty()) {
                return;  // 报错抢先，或批次已被提交（重复投递）
            }
            batch.swap(pending_);
            bytes = pending_bytes_;
            pending_bytes_ = 0;
            if (opts_.policy == StreamPolicy::Append) {
                snapshot_.items.insert(snapshot_.items.end(), std::make_move_iterator(batch.begin()),
                                       std::make_move_iterator(batch.end()));
            } else {
                snapshot_.items.clear();
                snapshot_.items.push_back(std::move(batch.back()));  // 整包快照流：只留最新一条
            }
            snapshot_.phase = ResourcePhase::Ready;
            snapshot_.error = std::nullopt;
            snapshot_.batch_bytes = bytes;
            next = snapshot_;
        }
        signal_.set(std::move(next));  // 锁外通知：渲染回调可能重入 push()
    }

    std::mutex mutex_;  ///< 保护下方全部字段
    StreamOptions opts_;  ///< 提交策略
    std::vector<T> pending_;  ///< 本批待提交数据（帧内合并的累加体）
    std::size_t pending_bytes_ = 0;  ///< 本批待提交的线上字节累计
    bool flush_scheduled_ = false;  ///< 是否已有提交投递在途（合并闸门的开关）
    bool terminal_ = false;  ///< 是否已 `fail()` 进入终态
    StreamSnapshot<T> snapshot_;  ///< 已提交的可读信息
    /// @brief 流式快照来源；显式关闭等值去重，保留每批提交的通知语义。
    State<StreamSnapshot<T>> signal_{StreamSnapshot<T>{}, always_publish};
};

}  // namespace detail

/// @brief 一次性异步作业的三态响应式绑定（需求 SPEC.FEAT.NET.DECLARATIVE-BINDING.001，
/// 语义详见 specification/02-state.md §5.4）。
///
/// 三态 `Loading / Ready(T) / Failed(Error)` 全在一个可订阅信号里：widget 读取任一 getter 即
/// 登记依赖，作业落定只重跑读它的组件。句柄可自由拷贝，全部拷贝共享同一内核；与宿主卸载 /
/// 重新赋值联动的取消由内核析构负责。
/// @tparam T 作业值类型。
/// @note Thread: 读取限主线程（`State` 的约定）；写入见本头文件头的投递器说明
/// @note Side-effects: none
/// @note Rebuildable: no
template <typename T>
class Resource {
  public:
    /// @brief 禁默认构造：没有作业的资源不是一个可落定的三态源。
    Resource() = delete;

    /// @brief 由「已启动作业」创建资源并立即开始等待其结果。
    ///
    /// 作业由调用方启动（典型为 `aurora::async(fn)`）。该形态没有可重发的来源，故 `reload()`
    /// 只是复位到 Loading 并重新等待**同一**作业的结果；要真重发请改用作业来源形态。
    /// @param job 已启动的作业句柄。
    /// @param opts 行为选项；默认 30 s 超时、不去抖。
    /// @return 处于 Loading 的资源句柄。
    [[nodiscard]] static auto from(Task<T> job, ResourceOptions opts = {}) -> Resource<T> {
        return Resource<T>{std::make_shared<detail::ResourceCore<T>>(std::move(job), opts)};
    }

    /// @brief 由「作业来源」创建资源并立即发送第一条作业；该形态支持真正的重发。
    ///
    /// `producer` 每次调用产出一个刚启动的新作业（形如 `[]{ return aurora::async(fn); }`），
    /// `reload()` 即再调它一次并取消前序。去抖窗口非 0 时窗口内的多次 `reload()` 合并为一次发送。
    /// @param producer 作业来源；在簿记锁内被调用，不得回调回本资源。
    /// @param opts 行为选项；默认 30 s 超时、不去抖。
    /// @return 处于 Loading 的资源句柄。
    [[nodiscard]] static auto from(std::function<Task<T>()> producer, ResourceOptions opts = {}) -> Resource<T> {
        return Resource<T>{std::make_shared<detail::ResourceCore<T>>(std::move(producer), opts)};
    }

    /// @brief 重发：取消上一次未完成的请求并重新发起（作业来源形态），或重新等待同一作业。
    /// 是否合并调用由选项里的去抖窗口决定。
    auto reload() -> void { core_->start(); }

    /// @brief 读取当前阶段；在活跃 `computed`/`effect` 作用域内读取即登记依赖。
    /// @return 三态之一。
    [[nodiscard]] auto phase() const -> ResourcePhase { return core_->signal().get().phase; }

    /// @brief 读取成功值；从未落定成功或最近一次失败时为 `nullptr`。
    ///
    /// 重发期间阶段为 Loading 但可保留最近一次成功值（stale-while-revalidate），
    /// 故「Loading 必空」不成立；失败后快照不保留 value。
    /// @return 指向内部存储的只读值指针；下一次状态写入后该指针失效。
    [[nodiscard]] auto value() const -> const T * {
        const auto &snap = core_->signal().get();
        return snap.value.has_value() ? &*snap.value : nullptr;
    }

    /// @brief 读取失败详情；非 Failed 为空。
    /// @return 错误副本（`Error` 含字符串字段，按值给出以免绑定内部存储）。
    [[nodiscard]] auto error() const -> std::optional<Error> { return core_->signal().get().error; }

    /// @brief 读取整份三态快照；在活跃 `computed`/`effect` 作用域内读取即登记依赖。
    /// @return 快照常引用；下一次落定写入后失效。
    [[nodiscard]] auto get() const -> const ResourceSnapshot<T> & { return core_->signal().get(); }

    /// @brief 订阅本资源的落定事件，返回可取消凭证（建立后立即交付当前快照一次）。
    ///
    /// 订阅回调保活控制核：作业不会因资源句柄被释放而静默停摆。
    /// @tparam F 可调用体，形参可接受 `const ResourceSnapshot<T> &`。
    /// @param fn 回调。
    /// @return 订阅凭证；析构或 `reset()` 即取消。
    template <typename F>
        requires std::invocable<std::decay_t<F>, const ResourceSnapshot<T> &>
    [[nodiscard]] auto subscribe(F &&fn) const -> Subscription {
        auto core = core_;  // 保活控制核：订阅存活期间作业继续运行并可落定
        return core_->signal().subscribe([core, callback = std::forward<F>(fn)](
                                             const ResourceSnapshot<T> &snap) mutable { std::invoke(callback, snap); });
    }

    /// @brief 降级为只读快照句柄：与控制核共享所有权并指向同一来源。
    ///
    /// 这是**别名**句柄而非快照格拷贝：作业随读句柄保活并继续落定。
    /// @return 持有同一快照来源的只读句柄。
    operator ReadOnlyState<ResourceSnapshot<T>>() const {  // NOLINT(google-explicit-constructor)
        auto source = detail::HandleAccess::source_of(core_->signal());
        return detail::HandleAccess::read_only<ResourceSnapshot<T>>(
            std::shared_ptr<detail::SourceOf<ResourceSnapshot<T>>>(core_, source.get()));
    }

  private:
    /// @brief 由工厂接管内核的私有构造；接管即发送第一条作业。
    /// @param core 已建好的共享内核。
    explicit Resource(std::shared_ptr<detail::ResourceCore<T>> core) : core_(std::move(core)) {
        core_->send();  // from() 即发送：首帧就处于 Loading，不需要调用方再显式 start()
    }

    std::shared_ptr<detail::ResourceCore<T>> core_;
};

/// @brief 持续数据流的三态响应式绑定（`Resource<T>` 的同族，语义详见
/// specification/02-state.md §5.4）。
///
/// 一次性资源装不下「一条接一条来」的语义，故本类型把写入端与读取端分开：写入端（逐条事件
/// 的生产者、分块产出的后台作业）`push()`，读取端像普通信号一样读 `items()`。
///
/// **帧内合并**：每次 `push()` 只累加批次，并把「提交」经主线程投递器排队；队列排空前到达的
/// 全部交付合并成一次状态写入，即一次定点刷新。故宿主卡顿期间的多次交付损失的是提交次数，
/// 不是数据——`batch_bytes()` 给出的是这一批的累计字节，不是最后一条的大小。未安装投递器时
/// 逐条直投（headless 场景每条约等于一次提交）。
/// @tparam T 单条数据类型。
/// @note Thread: 读取与提交限主线程；`push()` / `fail()` 从其它线程调用前须自行回投主线程
/// @note Side-effects: none
/// @note Rebuildable: no
template <typename T>
class StreamResource {
  public:
    /// @brief 禁默认构造：无内核的流不是一个可写入的槽位。
    StreamResource() = delete;

    /// @brief 打开一个等待写入的流槽位（阶段为 Loading）。
    /// @param opts 提交策略，默认追加。
    /// @return 处于 Loading 的流资源句柄。
    [[nodiscard]] static auto open(StreamOptions opts = {}) -> StreamResource<T> {
        return StreamResource<T>{std::make_shared<detail::StreamCore<T>>(opts)};
    }

    /// @brief 交付一条数据（写入端）。
    /// @param item 本条数据。
    /// @param wire_bytes 该条数据的线上字节数，供 `batch_bytes()` 累计；非线上流传 0。
    auto push(T item, std::size_t wire_bytes = 0) -> void { core_->push(std::move(item), wire_bytes); }

    /// @brief 报错终止（写入端）：丢弃未提交批次，状态进 Failed，已提交数据保留。
    /// @param err 失败详情。
    auto fail(const Error &err) -> void { core_->fail(err); }

    /// @brief 读取当前阶段；在活跃 `computed`/`effect` 作用域内读取即登记依赖。
    /// @return 三态之一。
    [[nodiscard]] auto phase() const -> ResourcePhase { return core_->signal().get().phase; }

    /// @brief 读取已提交的数据；在活跃 `computed`/`effect` 作用域内读取即登记依赖。
    /// @return 已提交列表的只读引用；Loading 时为空列表。引用在下一次提交后失效。
    [[nodiscard]] auto items() const -> const std::vector<T> & { return core_->signal().get().items; }

    /// @brief 读取失败详情；非 Failed 为空。
    /// @return 错误副本。
    [[nodiscard]] auto error() const -> std::optional<Error> { return core_->signal().get().error; }

    /// @brief 读取最近一次提交批次的累计字节数；在活跃 `computed`/`effect` 作用域内读取即登记依赖。
    /// @return 该批全部交付的线上字节之和（供进度展示取累计量，不是末条的大小）。
    [[nodiscard]] auto batch_bytes() const -> std::size_t { return core_->signal().get().batch_bytes; }

    /// @brief 读取整份流式快照；在活跃 `computed`/`effect` 作用域内读取即登记依赖。
    /// @return 快照常引用；下一次提交写入后失效。
    [[nodiscard]] auto get() const -> const StreamSnapshot<T> & { return core_->signal().get(); }

    /// @brief 订阅本流的批次提交事件，返回可取消凭证（建立后立即交付当前快照一次）。
    ///
    /// 订阅回调保活控制核：流不会因资源句柄被释放而静默停摆。
    /// @tparam F 可调用体，形参可接受 `const StreamSnapshot<T> &`。
    /// @param fn 回调。
    /// @return 订阅凭证；析构或 `reset()` 即取消。
    template <typename F>
        requires std::invocable<std::decay_t<F>, const StreamSnapshot<T> &>
    [[nodiscard]] auto subscribe(F &&fn) const -> Subscription {
        auto core = core_;  // 保活控制核：订阅存活期间提交仍会被交付
        return core_->signal().subscribe([core, callback = std::forward<F>(fn)](const StreamSnapshot<T> &snap) mutable {
            std::invoke(callback, snap);
        });
    }

    /// @brief 降级为只读快照句柄：与控制核共享所有权并指向同一来源。
    /// @return 持有同一快照来源的只读句柄。
    operator ReadOnlyState<StreamSnapshot<T>>() const {  // NOLINT(google-explicit-constructor)
        auto source = detail::HandleAccess::source_of(core_->signal());
        return detail::HandleAccess::read_only<StreamSnapshot<T>>(
            std::shared_ptr<detail::SourceOf<StreamSnapshot<T>>>(core_, source.get()));
    }

  private:
    /// @brief 由工厂接管内核的私有构造。
    /// @param core 已建好的共享内核。
    explicit StreamResource(std::shared_ptr<detail::StreamCore<T>> core) : core_(std::move(core)) {}

    std::shared_ptr<detail::StreamCore<T>> core_;
};

}  // namespace aurora
