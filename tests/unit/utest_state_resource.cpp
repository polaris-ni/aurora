/// 测试类型: unit
/// 目标单元: include/aurora/state/resource.h
/// 测试说明: 三态资源绑定的四组契约——`Resource::from` 的 Loading/Ready/Failed 迁移与「一次落定
/// 只通知一次」、投影只读句柄保活未落定作业、`reload` 取消前序请求且前序迟到结果不覆盖新状态、
/// 宿主卸载后回调被丢弃（弱引用不复活内核）、去抖窗口的多次调用合并、`StreamResource` 的追加/替换
/// 两策略与帧内合并批次字节，以及选项默认值在声明处可见
/// 覆盖说明: 用例统一给短超时——worker 池下的超时看守是一个睡满窗口的池任务，默认 30 s 会占住
/// worker 线程并把同进程后续用例拖进排队；全部状态写入经用例自装的主线程投递器回到用例线程

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/state/effect.h"
#include "aurora/state/resource.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_state_resource {

namespace {

/// 能力负向探针：资源类型不得暴露通用写入入口（写入只经 `from`/`reload`/`push` 的领域语义）。
template <typename R>
concept HasSet = requires(R r) { r.set(int{}); };
template <typename R>
concept HasNotify = requires(R r) { r.notify(); };

static_assert(!HasSet<Resource<int>>);
static_assert(!HasNotify<Resource<int>>);
static_assert(!HasSet<StreamResource<int>>);
static_assert(!HasNotify<StreamResource<int>>);
// 组合而非继承：资源不是只读句柄的派生，公共面不存在可写基类子对象。
static_assert(!std::is_base_of_v<ReadOnlyState<ResourceSnapshot<int>>, Resource<int>>);
static_assert(!std::is_base_of_v<ReadOnlyState<StreamSnapshot<int>>, StreamResource<int>>);
// 只读视图经隐式转换给出（别名句柄保活控制核）。
static_assert(std::is_convertible_v<Resource<int>, ReadOnlyState<ResourceSnapshot<int>>>);
static_assert(std::is_convertible_v<StreamResource<int>, ReadOnlyState<StreamSnapshot<int>>>);

}  // namespace

namespace {

// @brief 有界轮询：每 1ms 判一次 pred，超时返回最后一次判定（禁止无界阻塞）。
template <typename Pred>
// pred 在轮询循环内被反复调用，不能按「一次性转发」用 std::forward（对带状态可调用体转成右值
// 引用会误移动），刻意始终以左值形式调用，故抑制该告警。
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
auto wait_until(Pred &&pred, std::chrono::milliseconds budget = std::chrono::milliseconds{2000}) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return pred();
}

/// @brief 主线程投递器的替身：把回投闭包排队，由用例线程统一排空。
///
/// 装它有两个目的：① 全部 `State` 写入落在用例线程，守住「main-thread only」约定；② 队列就是
/// 「已排进主线程但尚未执行」的投递现场，可以据此造出「迟到结果」与「宿主已亡」两种时序。
class PosterQueue {
  public:
    PosterQueue() {
        Task<int>::set_main_poster([this](std::function<void()> fn) -> void {
            std::scoped_lock<std::mutex> lock(mutex_);
            queued_.push_back(std::move(fn));
        });
    }
    /// @brief 禁拷贝：投递器是进程级单例，两份替身会互相顶替。
    PosterQueue(const PosterQueue &) = delete;
    /// @brief 禁拷贝赋值：理由同拷贝构造。
    auto operator=(const PosterQueue &) -> PosterQueue & = delete;
    /// @brief 禁移动：投递器捕获的是 `this`，搬走即悬垂。
    PosterQueue(PosterQueue &&) = delete;
    /// @brief 禁移动赋值：理由同移动构造。
    auto operator=(PosterQueue &&) -> PosterQueue & = delete;
    ~PosterQueue() { Task<int>::set_main_poster(nullptr); }

    /// @brief 排队项数。
    /// @return 尚未执行的投递闭包数。
    [[nodiscard]] auto size() -> std::size_t {
        std::scoped_lock<std::mutex> lock(mutex_);
        return queued_.size();
    }

    /// @brief 排空当前队列（执行期间新入队的项在后续轮次继续执行，最多 8 轮防自旋）。
    /// @return 实际执行的闭包数。
    auto drain() -> std::size_t {
        std::size_t ran = 0;
        for (int round = 0; round < 8; ++round) {
            std::vector<std::function<void()>> batch;
            {
                std::scoped_lock<std::mutex> lock(mutex_);
                batch.swap(queued_);
            }
            if (batch.empty()) {
                break;
            }
            for (std::function<void()> &fn : batch) {
                fn();
                ++ran;
            }
        }
        return ran;
    }

    // @brief 边排空边轮询 `pred`：去抖发送与结果落定都只有排空才会发生。
    // @param pred 终止条件。
    // @param budget 有界等待预算。
    // @return 预算内命中为 true。
    template <typename Pred>
    // 同 wait_until：pred 反复以左值调用，不做一次性转发。
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
    auto pump_until(Pred &&pred, std::chrono::milliseconds budget = std::chrono::milliseconds{3000}) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            drain();
            if (pred()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        drain();
        return pred();
    }

  private:
    std::mutex mutex_;  ///< 保护 queued_
    std::vector<std::function<void()>> queued_;  ///< 待执行的主线程闭包
};

/// @brief 用例选项：短超时（看守线程占用有界）+ 默认不去抖。
/// @param debounce_window 去抖窗口，默认 0 = 不去抖。
/// @return 供用例使用的资源选项。
auto short_timeout_options(std::chrono::milliseconds debounce_window = std::chrono::milliseconds{0})
    -> ResourceOptions {
    return ResourceOptions{.timeout = std::chrono::milliseconds{500}, .debounce = debounce_window};
}

}  // namespace

AURORA_TEST_CASE(options_defaults_are_visible_at_declaration) {
    // 默认值口径（SPEC.API.SENSIBLE-DEFAULTS.001）：超时 30 s、reload 不去抖、流式默认追加。
    const ResourceOptions defaults{};
    AURORA_TEST_CHECK_EQ(defaults.timeout, std::chrono::milliseconds{std::chrono::seconds{30}});
    AURORA_TEST_CHECK_EQ(defaults.debounce, std::chrono::milliseconds{0});
    const StreamOptions stream_defaults{};
    AURORA_TEST_CHECK(stream_defaults.policy == StreamPolicy::Append);
}

AURORA_TEST_CASE(from_job_reaches_ready_with_single_notification) {
    AURORA_TEST_REQUIRE_THREADS();
    PosterQueue pq;
    std::atomic<bool> started{false};
    std::promise<void> gate;
    auto gate_future = gate.get_future().share();

    auto res = Resource<int>::from(async([&started, gate_future]() -> int {
                                       started.store(true, std::memory_order_release);
                                       gate_future.wait();
                                       return 42;
                                   }),
                                   short_timeout_options());

    int runs = 0;
    const auto watch = effect([&runs, &res]() -> void {
        ++runs;
        (void)res.phase();  // 读取即登记依赖
    });
    AURORA_TEST_CHECK_EQ(runs, 1);  // 首跑只算一次登记

    AURORA_TEST_CHECK(res.phase() == ResourcePhase::Loading);
    AURORA_TEST_CHECK_NULL(res.value());
    AURORA_TEST_CHECK_FALSE(res.error().has_value());

    AURORA_TEST_REQUIRE(wait_until([&started]() -> bool { return started.load(std::memory_order_acquire); }));
    gate.set_value();
    AURORA_TEST_REQUIRE(pq.pump_until([&res]() -> bool { return res.phase() == ResourcePhase::Ready; }));

    AURORA_TEST_REQUIRE_NOT_NULL(res.value());
    AURORA_TEST_CHECK_EQ(*res.value(), 42);
    AURORA_TEST_CHECK_FALSE(res.error().has_value());
    // 一次落定 = 一次通知：阶段/值/错误在同一份快照里，不拆成三个信号各刷一轮。
    AURORA_TEST_CHECK_EQ(runs, 2);
}

AURORA_TEST_CASE(read_only_projection_keeps_job_alive_until_landing) {
    AURORA_TEST_REQUIRE_THREADS();
    PosterQueue pq;
    std::atomic<bool> started{false};
    std::promise<void> gate;
    auto gate_future = gate.get_future().share();

    // 临时资源句柄在本行末即析构；只剩投影出的只读句柄，作业仍须存活并落定。
    ReadOnlyState<ResourceSnapshot<int>> handle =
        Resource<int>::from(async([&started, gate_future]() -> int {
                                started.store(true, std::memory_order_release);
                                gate_future.wait();
                                return 7;
                            }),
                            short_timeout_options());

    AURORA_TEST_CHECK(handle.get().phase == ResourcePhase::Loading);
    AURORA_TEST_CHECK_FALSE(handle.get().value.has_value());
    AURORA_TEST_REQUIRE(wait_until([&started]() -> bool { return started.load(std::memory_order_acquire); }));
    gate.set_value();
    AURORA_TEST_REQUIRE(pq.pump_until([&handle]() -> bool { return handle.get().phase == ResourcePhase::Ready; }));
    AURORA_TEST_REQUIRE(handle.get().value.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): 上一行 REQUIRE 已断言持有值，其宏展开对路径分析不透明
    AURORA_TEST_CHECK_EQ(*handle.get().value, 7);
}

AURORA_TEST_CASE(from_error_job_reaches_failed_and_clears_value) {
    AURORA_TEST_REQUIRE_THREADS();
    PosterQueue pq;
    auto res = Resource<int>::from(
        async([]() -> Result<int> {
            return Result<int>{make_error(ErrorCode::GeneralInvalidArgument, std::string{"bad input"})};
        }),
        short_timeout_options());

    AURORA_TEST_CHECK(res.phase() == ResourcePhase::Loading);
    AURORA_TEST_REQUIRE(pq.pump_until([&res]() -> bool { return res.phase() == ResourcePhase::Failed; }));

    AURORA_TEST_CHECK_NULL(res.value());  // 三态互斥：Failed 时值一律为空
    const std::optional<Error> err = res.error();
    AURORA_TEST_REQUIRE(err.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access): 上一行 REQUIRE 已断言持有值，其宏展开对路径分析不透明
    AURORA_TEST_CHECK(err->code_enum == ErrorCode::GeneralInvalidArgument);
    AURORA_TEST_CHECK_STREQ(err->message, "bad input");
    // NOLINTEND(bugprone-unchecked-optional-access)
}

AURORA_TEST_CASE(reload_cancels_previous_request) {
    AURORA_TEST_REQUIRE_THREADS();
    PosterQueue pq;
    std::array<std::promise<void>, 2> gates{};
    std::array<std::shared_future<void>, 2> releases{gates[0].get_future().share(), gates[1].get_future().share()};
    std::array<std::optional<Task<int>>, 2> jobs{};
    std::atomic<int> sends{0};
    std::atomic<int> bodies_finished{0};

    auto producer = [&sends, &bodies_finished, &releases, &jobs]() -> Task<int> {
        const auto idx = static_cast<std::size_t>(sends.fetch_add(1, std::memory_order_acq_rel));
        auto job = async([idx, &releases, &bodies_finished]() -> int {
            releases.at(idx).wait();
            bodies_finished.fetch_add(1, std::memory_order_acq_rel);
            return static_cast<int>(idx + 1U) * 10;
        });
        jobs.at(idx) = job;  // 留一份句柄给用例判取消
        return job;
    };

    auto res = Resource<int>::from(producer, short_timeout_options());
    AURORA_TEST_CHECK_EQ(sends.load(std::memory_order_acquire), 1);
    AURORA_TEST_REQUIRE(jobs[0].has_value());

    res.reload();  // 真重发：前序未完成作业被取消，第二条作业另起
    AURORA_TEST_CHECK_EQ(sends.load(std::memory_order_acquire), 2);
    AURORA_TEST_REQUIRE(jobs[1].has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access): 上两行 REQUIRE 已断言两个句柄皆持有值，宏展开对路径分析不透明
    AURORA_TEST_CHECK_TRUE(jobs[0]->is_cancelled());
    AURORA_TEST_CHECK_FALSE(jobs[1]->is_cancelled());
    // NOLINTEND(bugprone-unchecked-optional-access)

    gates[1].set_value();
    AURORA_TEST_REQUIRE(pq.pump_until([&res]() -> bool { return res.phase() == ResourcePhase::Ready; }));
    AURORA_TEST_REQUIRE_NOT_NULL(res.value());
    AURORA_TEST_CHECK_EQ(*res.value(), 20);

    gates[0].set_value();  // 前序作业体仍会跑完（取消不中断作业），等它收尾再退出用例
    AURORA_TEST_REQUIRE(
        wait_until([&bodies_finished]() -> bool { return bodies_finished.load(std::memory_order_acquire) == 2; }));
}

AURORA_TEST_CASE(previous_request_late_result_does_not_overwrite_new_state) {
    AURORA_TEST_REQUIRE_THREADS();
    PosterQueue pq;
    std::array<std::promise<void>, 2> gates{};
    std::array<std::shared_future<void>, 2> releases{gates[0].get_future().share(), gates[1].get_future().share()};
    std::atomic<int> sends{0};

    auto producer = [&sends, &releases]() -> Task<int> {
        const auto idx = static_cast<std::size_t>(sends.fetch_add(1, std::memory_order_acq_rel));
        return async([idx, &releases]() -> int {
            releases.at(idx).wait();
            return static_cast<int>(idx + 1U) * 10;
        });
    };

    auto res = Resource<int>::from(producer, short_timeout_options());
    AURORA_TEST_CHECK_EQ(sends.load(std::memory_order_acquire), 1);

    // 第一条作业跑完：投递已排进队列但**未执行**——这正是 cancel 挡不住的那次投递。
    gates[0].set_value();
    AURORA_TEST_REQUIRE(wait_until([&pq]() -> bool { return pq.size() >= 1; }));

    res.reload();  // 换代次、复位到 Loading；队列里仍躺着上一代的结果
    AURORA_TEST_CHECK_EQ(sends.load(std::memory_order_acquire), 2);
    AURORA_TEST_CHECK(res.phase() == ResourcePhase::Loading);

    pq.drain();  // 上一代的迟到结果抵达：代次守卫丢弃
    AURORA_TEST_CHECK(res.phase() == ResourcePhase::Loading);
    AURORA_TEST_CHECK_NULL(res.value());

    gates[1].set_value();
    AURORA_TEST_REQUIRE(pq.pump_until([&res]() -> bool { return res.phase() == ResourcePhase::Ready; }));
    AURORA_TEST_REQUIRE_NOT_NULL(res.value());
    AURORA_TEST_CHECK_EQ(*res.value(), 20);  // 新状态来自新一代，不是被旧结果改写
}

AURORA_TEST_CASE(host_scope_exit_cancels_pending_job) {
    AURORA_TEST_REQUIRE_THREADS();
    PosterQueue pq;
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    std::promise<void> gate;
    auto gate_future = gate.get_future().share();

    auto job = async([&started, &finished, gate_future]() -> int {
        started.store(true, std::memory_order_release);
        gate_future.wait();
        finished.store(true, std::memory_order_release);
        return 7;
    });
    {
        auto res = Resource<int>::from(job, short_timeout_options());  // 同一作业的另一个句柄
        AURORA_TEST_REQUIRE(wait_until([&started]() -> bool { return started.load(std::memory_order_acquire); }));
        AURORA_TEST_CHECK_FALSE(job.is_cancelled());
    }  // 最后一个持有者放手 ⇒ 内核析构 ⇒ 前序请求经取消
    AURORA_TEST_CHECK_TRUE(job.is_cancelled());

    gate.set_value();  // 作业体仍会跑完（取消只丢弃结果，无法中断它）
    AURORA_TEST_REQUIRE(wait_until([&finished]() -> bool { return finished.load(std::memory_order_acquire); }));
    pq.drain();
}

AURORA_TEST_CASE(callback_arriving_after_host_death_is_dropped) {
    AURORA_TEST_REQUIRE_THREADS();
    PosterQueue pq;
    std::atomic<bool> started{false};
    std::promise<void> gate;
    auto gate_future = gate.get_future().share();

    auto job = async([&started, gate_future]() -> int {
        started.store(true, std::memory_order_release);
        gate_future.wait();
        return 9;
    });
    {
        auto res = Resource<int>::from(job, short_timeout_options());
        AURORA_TEST_REQUIRE(wait_until([&started]() -> bool { return started.load(std::memory_order_acquire); }));
        gate.set_value();
        // 投递已排进队列：闭包里躺着指向内核的**弱**引用
        AURORA_TEST_REQUIRE(wait_until([&pq]() -> bool { return pq.size() >= 1; }));
    }  // 宿主先亡：内核此刻析构。闭包若持强引用就不会析构，下面的 is_cancelled() 也就为假
    AURORA_TEST_CHECK_TRUE(job.is_cancelled());
    AURORA_TEST_CHECK_EQ(pq.size(), std::size_t{1});

    // 回调抵达：弱引用失效 ⇒ 丢弃，不产生悬垂访问。被排空的闭包数就是被丢弃的投递数。
    AURORA_TEST_CHECK_GE(pq.drain(), std::size_t{1});
}

AURORA_TEST_CASE(task_form_reload_reawaits_the_same_job) {
    AURORA_TEST_REQUIRE_THREADS();
    PosterQueue pq;
    // 外部作业形态没有可重发的来源：reload() 复位到 Loading 并重新等待**同一**作业的结果，
    // 因此不得取消它（取消就永远拿不回结果），且 Loading 期间保留残留值。
    std::promise<void> gate;
    auto gate_future = gate.get_future().share();
    auto job = async([gate_future]() -> int {
        gate_future.wait();
        return 11;
    });
    auto res = Resource<int>::from(job, short_timeout_options());

    gate.set_value();
    AURORA_TEST_REQUIRE(pq.pump_until([&res]() -> bool { return res.phase() == ResourcePhase::Ready; }));
    AURORA_TEST_CHECK_FALSE(job.is_cancelled());

    res.reload();
    AURORA_TEST_CHECK(res.phase() == ResourcePhase::Loading);
    AURORA_TEST_REQUIRE_NOT_NULL(res.value());  // stale-while-revalidate：残留值仍在
    AURORA_TEST_CHECK_FALSE(job.is_cancelled());

    AURORA_TEST_REQUIRE(pq.pump_until([&res]() -> bool { return res.phase() == ResourcePhase::Ready; }));
    AURORA_TEST_REQUIRE_NOT_NULL(res.value());
    AURORA_TEST_CHECK_EQ(*res.value(), 11);  // 同一作业的同一结果重新落定
}

AURORA_TEST_CASE(debounce_window_merges_rapid_reloads_into_one_send) {
    AURORA_TEST_REQUIRE_THREADS();
    PosterQueue pq;
    std::atomic<int> sends{0};
    auto producer = [&sends]() -> Task<int> {
        sends.fetch_add(1, std::memory_order_acq_rel);
        return async([]() -> int { return 1; });
    };
    auto res = Resource<int>::from(producer, short_timeout_options(std::chrono::milliseconds{40}));
    AURORA_TEST_CHECK_EQ(sends.load(std::memory_order_acquire), 1);  // from() 的即时发送不受去抖影响

    res.reload();
    res.reload();
    res.reload();  // 窗口内的多次调用：先前的登记逐个作废
    AURORA_TEST_CHECK_EQ(sends.load(std::memory_order_acquire), 1);

    AURORA_TEST_REQUIRE(pq.pump_until([&sends]() -> bool { return sends.load(std::memory_order_acquire) >= 2; }));
    AURORA_TEST_CHECK_EQ(sends.load(std::memory_order_acquire), 2);  // 三次合并为一次真发
    // 结果落定同样要排空投递队列才发生：pump 而非一次性 drain（合并后那次作业的送达晚于 sends 计数）。
    AURORA_TEST_REQUIRE(pq.pump_until([&res]() -> bool { return res.phase() == ResourcePhase::Ready; }));
    AURORA_TEST_CHECK_EQ(*res.value(), 1);
}

AURORA_TEST_CASE(stream_append_policy_merges_pushes_into_one_commit) {
    PosterQueue pq;
    auto stream = StreamResource<std::string>::open();
    int runs = 0;
    const auto watch = effect([&runs, &stream]() -> void {
        ++runs;
        (void)stream.items();  // 读取即登记依赖
    });
    AURORA_TEST_CHECK_EQ(runs, 1);
    AURORA_TEST_CHECK(stream.phase() == ResourcePhase::Loading);
    AURORA_TEST_CHECK_EQ(stream.items().size(), std::size_t{0});

    stream.push("alpha", 5);
    stream.push("beta", 7);
    stream.push("gamma", 11);  // 三次交付只排一次提交投递：帧内合并
    AURORA_TEST_CHECK_EQ(pq.size(), std::size_t{1});

    pq.drain();
    AURORA_TEST_CHECK_EQ(stream.items().size(), std::size_t{3});
    AURORA_TEST_CHECK_STREQ(stream.items().front(), "alpha");
    AURORA_TEST_CHECK_STREQ(stream.items().back(), "gamma");
    AURORA_TEST_CHECK_EQ(stream.batch_bytes(), std::size_t{23});  // 批次累计，不是末条大小
    AURORA_TEST_CHECK(stream.phase() == ResourcePhase::Ready);
    AURORA_TEST_CHECK_EQ(runs, 2);  // 三次交付 = 一次定点刷新
}

AURORA_TEST_CASE(stream_replace_policy_keeps_only_latest_item) {
    PosterQueue pq;
    auto stream = StreamResource<std::string>::open(StreamOptions{.policy = StreamPolicy::Replace});
    stream.push("v1", 2);
    stream.push("v2", 3);
    stream.push("v3", 4);
    AURORA_TEST_CHECK_EQ(pq.size(), std::size_t{1});

    pq.drain();
    AURORA_TEST_CHECK_EQ(stream.items().size(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(stream.items().front(), "v3");
    AURORA_TEST_CHECK_EQ(stream.batch_bytes(), std::size_t{9});  // 丢弃数据不等于丢弃字节计数
}

AURORA_TEST_CASE(stream_fail_is_terminal_and_keeps_committed_items) {
    PosterQueue pq;
    auto stream = StreamResource<std::string>::open();
    stream.push("kept", 4);
    pq.drain();
    AURORA_TEST_REQUIRE_EQ(stream.items().size(), std::size_t{1});

    stream.fail(make_error(ErrorCode::GeneralInvalidArgument, std::string{"stream broke"}));
    AURORA_TEST_CHECK(stream.phase() == ResourcePhase::Failed);
    const std::optional<Error> err = stream.error();
    AURORA_TEST_REQUIRE(err.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): 上一行 REQUIRE 已断言持有值，其宏展开对路径分析不透明
    AURORA_TEST_CHECK_STREQ(err->message, "stream broke");
    AURORA_TEST_CHECK_EQ(stream.items().size(), std::size_t{1});  // 已提交数据不因失败丢弃

    stream.push("dropped", 8);  // 终态之后的交付被丢弃
    pq.drain();
    AURORA_TEST_CHECK(stream.phase() == ResourcePhase::Failed);
    AURORA_TEST_CHECK_EQ(stream.items().size(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(stream.items().front(), "kept");
}

}  // namespace aurora::test_cases::utest_state_resource
