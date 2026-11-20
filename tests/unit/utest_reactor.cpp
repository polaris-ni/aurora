/// 测试类型: unit
/// 目标单元: include/aurora/state/detail/reactor.h
/// 测试说明: 响应式内核的提交/等价停止/菱形去重/动态依赖裁剪/回调内写排队/取消/反馈环截断

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include "aurora/state/detail/reactor.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_reactor {

namespace {

using aurora::detail::default_equivalence;
using aurora::detail::Derived;
using aurora::detail::RunRecord;
using aurora::detail::Source;

/// @brief 构造共享来源的便捷入口（内核节点须由 shared_ptr 持有）。
template <typename T>
auto make_source(T value) -> std::shared_ptr<Source<T>> {
    return std::make_shared<Source<T>>(std::move(value));
}

/// @brief 构造采用默认等价策略的派生节点。
template <typename T, typename F>
auto make_derived(F fn) -> std::shared_ptr<Derived<T>> {
    return std::make_shared<Derived<T>>(std::function<T()>(std::move(fn)), default_equivalence<T>());
}

}  // namespace

AURORA_TEST_CASE(reactor_source_assign_publishes_once) {
    // 提交新值：版本自增、终端重跑一次并读到新值；提交等值：版本不动、终端不重跑。
    auto src = make_source(1);
    int runs = 0;
    int seen = 0;
    auto rec = std::make_shared<RunRecord>([&] {
        ++runs;
        seen = src->read();
    });
    rec->run_first();
    AURORA_TEST_CHECK_EQ(runs, 1);
    AURORA_TEST_CHECK_EQ(seen, 1);
    AURORA_TEST_CHECK_EQ(src->version(), std::uint64_t{0});

    src->assign(2);
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(seen, 2);
    AURORA_TEST_CHECK_EQ(src->version(), std::uint64_t{1});

    src->assign(2);
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(src->version(), std::uint64_t{1});
}

AURORA_TEST_CASE(reactor_always_publish_bypasses_equality) {
    // always_publish：每次提交都通知，即使载荷相等。
    auto src = std::make_shared<Source<int>>(7, aurora::always_publish);
    int runs = 0;
    auto rec = std::make_shared<RunRecord>([&] {
        ++runs;
        (void)src->read();
    });
    rec->run_first();
    src->assign(7);
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(src->version(), std::uint64_t{1});
}

AURORA_TEST_CASE(reactor_force_publish_reruns_equal_value) {
    // 原地改载荷后强制发布：不经过等价比较，终端必须重跑。
    auto src = make_source(3);
    int runs = 0;
    auto rec = std::make_shared<RunRecord>([&] {
        ++runs;
        (void)src->read();
    });
    rec->run_first();
    src->force_publish();
    AURORA_TEST_CHECK_EQ(runs, 2);
}

AURORA_TEST_CASE(reactor_derived_is_lazy_on_first_read) {
    // 派生节点首次求值发生在第一次 read；未求值前 value() 属逻辑错误。
    auto src = make_source(2);
    int evaluations = 0;
    auto view = make_derived<int>([&] {
        ++evaluations;
        return src->read() * 2;
    });
    AURORA_TEST_CHECK_THROW(view->value(), std::logic_error);
    AURORA_TEST_CHECK_EQ(evaluations, 0);
    AURORA_TEST_CHECK_EQ(view->read(), 4);
    AURORA_TEST_CHECK_EQ(evaluations, 1);
    AURORA_TEST_CHECK_EQ(view->value(), 4);
}

AURORA_TEST_CASE(reactor_diamond_delivers_terminal_once) {
    // 菱形依赖：两个中间派生共享同一来源，终端只观察末端一次提交。
    auto src = make_source(1);
    auto doubled = make_derived<int>([&] { return src->read() * 2; });
    auto tripled = make_derived<int>([&] { return src->read() * 3; });
    auto total = make_derived<int>([&] { return doubled->read() + tripled->read(); });
    std::vector<int> seen;
    auto rec = std::make_shared<RunRecord>([&] { seen.push_back(total->read()); });
    rec->run_first();
    AURORA_TEST_CHECK_EQ(seen.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(seen.back(), 5);

    src->assign(2);
    AURORA_TEST_CHECK_EQ(seen.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(seen[1], 10);

    src->assign(2);
    AURORA_TEST_CHECK_EQ(seen.size(), std::size_t{2});
}

AURORA_TEST_CASE(reactor_terminals_deliver_in_registration_order) {
    // 同轮多个终端按登记顺序交付：后登记者的回调先读取来源也不改变交付顺序。
    auto src = make_source(0);
    std::vector<int> order;
    auto first = std::make_shared<RunRecord>([&] {
        (void)src->read();
        order.push_back(1);
    });
    auto second = std::make_shared<RunRecord>([&] {
        (void)src->read();
        order.push_back(2);
    });
    first->run_first();
    second->run_first();
    order.clear();

    src->assign(1);
    AURORA_TEST_CHECK_EQ(order.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(order[0], 1);
    AURORA_TEST_CHECK_EQ(order[1], 2);
}

AURORA_TEST_CASE(reactor_derived_equality_stops_downstream) {
    // 派生结果等值：版本不变，下游不重跑（等值停止在派生层生效）。
    auto src = make_source(0);
    auto even = make_derived<bool>([&] { return src->read() % 2 == 0; });
    int runs = 0;
    auto rec = std::make_shared<RunRecord>([&] {
        ++runs;
        (void)even->read();
    });
    rec->run_first();
    AURORA_TEST_CHECK_EQ(runs, 1);

    src->assign(2);
    AURORA_TEST_CHECK_EQ(runs, 1);

    src->assign(3);
    AURORA_TEST_CHECK_EQ(runs, 2);
}

AURORA_TEST_CASE(reactor_dynamic_branch_prunes_old_dependency) {
    // 分支切换后旧来源不再触发重算；切回后新来源生效。
    auto flag = make_source(true);
    auto left = make_source(1);
    auto right = make_source(2);
    int evaluations = 0;
    auto view = make_derived<int>([&] {
        ++evaluations;
        return flag->read() ? left->read() : right->read();
    });
    std::vector<int> seen;
    auto rec = std::make_shared<RunRecord>([&] { seen.push_back(view->read()); });
    rec->run_first();
    AURORA_TEST_CHECK_EQ(evaluations, 1);
    AURORA_TEST_CHECK_EQ(seen.back(), 1);

    right->assign(20);
    AURORA_TEST_CHECK_EQ(evaluations, 1);
    AURORA_TEST_CHECK_EQ(seen.size(), std::size_t{1});

    flag->assign(false);
    AURORA_TEST_CHECK_EQ(evaluations, 2);
    AURORA_TEST_CHECK_EQ(seen.back(), 20);

    left->assign(10);
    AURORA_TEST_CHECK_EQ(evaluations, 2);

    flag->assign(true);
    AURORA_TEST_CHECK_EQ(evaluations, 3);
    AURORA_TEST_CHECK_EQ(seen.back(), 10);
}

AURORA_TEST_CASE(reactor_write_inside_callback_defers_to_later_round) {
    // 回调内写入排到本轮之后：本轮仍读旧值，随后按提交顺序依次生效。
    auto trigger = make_source(1);
    auto sink = make_source(0);
    std::vector<int> observed_inside;
    std::vector<int> sink_seen;
    auto writer = std::make_shared<RunRecord>([&] {
        const int value = trigger->read();
        sink->assign(value * 10);
        observed_inside.push_back(sink->value());  // value() 不登记依赖，仅窥视当前提交值
    });
    auto observer = std::make_shared<RunRecord>([&] { sink_seen.push_back(sink->read()); });
    observer->run_first();
    writer->run_first();
    AURORA_TEST_CHECK_EQ(observed_inside.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(observed_inside[0], 0);
    AURORA_TEST_CHECK_EQ(sink->value(), 10);
    AURORA_TEST_CHECK_EQ(sink_seen.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(sink_seen[1], 10);

    trigger->assign(2);
    AURORA_TEST_CHECK_EQ(observed_inside.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(observed_inside[1], 10);
    AURORA_TEST_CHECK_EQ(sink->value(), 20);
    AURORA_TEST_CHECK_EQ(sink_seen.size(), std::size_t{3});
    AURORA_TEST_CHECK_EQ(sink_seen[2], 20);
}

AURORA_TEST_CASE(reactor_cancel_stops_delivery) {
    // 取消后不再交付，且可重复调用；其依赖边一并解除。
    auto src = make_source(0);
    int runs = 0;
    auto rec = std::make_shared<RunRecord>([&] {
        ++runs;
        (void)src->read();
    });
    rec->run_first();
    rec->cancel();
    AURORA_TEST_CHECK(rec->cancelled());
    rec->cancel();
    src->assign(1);
    AURORA_TEST_CHECK_EQ(runs, 1);
    rec->run_first();
    AURORA_TEST_CHECK_EQ(runs, 1);
}

AURORA_TEST_CASE(reactor_feedback_loop_is_cut) {
    // 读写同一来源的回调：首轮 + 一轮跟进后按因果链截断，不会无限自激。
    auto src = make_source(0);
    int runs = 0;
    auto rec = std::make_shared<RunRecord>([&] {
        ++runs;
        const int value = src->read();
        src->assign(value + 1);
    });
    rec->run_first();
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(src->value(), 2);

    // 后续外部写入仍可驱动该终端一次，其自写再次被截断。
    src->assign(100);
    AURORA_TEST_CHECK_EQ(runs, 3);
    AURORA_TEST_CHECK_EQ(src->value(), 101);
}

AURORA_TEST_CASE(reactor_destroyed_terminal_unlinks) {
    // 终端析构即退订：弱引用可确认对象已销毁，后续提交不触碰已亡观察者。
    auto src = make_source(0);
    int runs = 0;
    auto rec = std::make_shared<RunRecord>([&] {
        ++runs;
        (void)src->read();
    });
    rec->run_first();
    std::weak_ptr<RunRecord> const weak = rec;
    rec.reset();
    AURORA_TEST_CHECK(weak.expired());
    src->assign(1);
    AURORA_TEST_CHECK_EQ(src->value(), 1);
}

AURORA_TEST_CASE(epoch_arrays_shrink_to_live_high_water) {
#ifdef AURORA_ENABLE_DEBUG
    // 判据①③：批量创建来源与订阅者，提交被多数订阅者共享的最高 id 来源一次，同时把两枚去重戳
    // 数组抬过收缩阈值；销毁高 id / 高 seq 节点、只保留最低 id 来源与最低 seq 订阅者，两数组应
    // 收敛到各自存活上界而非累计创建峰值。末尾再提交一次覆盖轮末兜底路径。
    constexpr int batch_size = 100000;
    auto &engine = aurora::detail::Engine::instance();

    std::vector<std::shared_ptr<Source<int>>> sources;
    sources.reserve(static_cast<std::size_t>(batch_size));
    for (int i = 0; i < batch_size; ++i) {
        sources.push_back(make_source(i));
    }

    // rec[0] 订阅最低 id 来源（保留）；rec[1..] 订阅最高 id 来源 sink（撑数组用，随后销毁）。
    // 提交 sink 一次：mark 遍历其全部观察者，同时撑大来源戳数组（到 sink.id）与观察者戳数组（到最高 seq）。
    std::shared_ptr<Source<int>> sink = sources.back();
    auto *sink_ptr = sink.get();
    auto *keep_ptr = sources.front().get();
    std::vector<std::shared_ptr<RunRecord>> records;
    records.reserve(static_cast<std::size_t>(batch_size));
    auto keep_rec = std::make_shared<RunRecord>([keep_ptr] { (void)keep_ptr->read(); });
    keep_rec->run_first();
    records.push_back(std::move(keep_rec));
    for (int i = 1; i < batch_size; ++i) {
        auto rec = std::make_shared<RunRecord>([sink_ptr] { (void)sink_ptr->read(); });
        rec->run_first();
        records.push_back(std::move(rec));
    }
    sink_ptr->assign(sink_ptr->read() + 1);

    AURORA_TEST_CHECK_GT(engine.seen_source_size(), 4096U);
    AURORA_TEST_CHECK_GT(engine.seen_observer_size(), 4096U);

    const auto kept_src = static_cast<std::size_t>(sources.front()->id());
    const auto kept_obs = static_cast<std::size_t>(records.front()->seq());
    records.erase(records.begin() + 1, records.end());  // 高 seq 订阅者析构，释放对 sink 的依赖引用
    sources.erase(sources.begin() + 1, sources.end());  // 高 id 来源析构（含 sink 的 vector 引用）
    sink.reset();  // 释放本用例对 sink 的额外所有权，sink 真正析构

    // 再提交保留来源一次走轮末兜底（deliver 后收缩），保留节点仍各自存活，收缩目标不变。
    sources.front()->assign(sources.front()->read() + 1);

    const std::size_t src_bound = std::max<std::size_t>(4096U, kept_src + 1U);
    const std::size_t obs_bound = std::max<std::size_t>(4096U, kept_obs + 1U);
    AURORA_TEST_CHECK_EQ(engine.seen_source_size(), engine.seen_source_capacity());
    AURORA_TEST_CHECK_LE(engine.seen_source_size(), src_bound);
    AURORA_TEST_CHECK_EQ(engine.seen_observer_size(), engine.seen_observer_capacity());
    AURORA_TEST_CHECK_LE(engine.seen_observer_size(), obs_bound);
#else
    AURORA_TEST_SKIP("epoch-array observation ports are gated by AURORA_ENABLE_DEBUG");
#endif
}

AURORA_TEST_CASE(epoch_arrays_shrink_on_destruct_without_commit) {
#ifdef AURORA_ENABLE_DEBUG
    // 判据①「纯创建/销毁、零写提交」段落：撑大两数组后销毁高 id / 高 seq 节点期间不再提交，
    // 收缩只能由析构路径的注销检查完成（析构发生在交付之外，不受在途遍历守卫跳过）。
    constexpr int batch_size = 100000;
    auto &engine = aurora::detail::Engine::instance();

    std::vector<std::shared_ptr<Source<int>>> sources;
    sources.reserve(static_cast<std::size_t>(batch_size));
    for (int i = 0; i < batch_size; ++i) {
        sources.push_back(make_source(i));
    }
    std::shared_ptr<Source<int>> sink = sources.back();
    auto *sink_ptr = sink.get();
    auto *keep_ptr = sources.front().get();
    std::vector<std::shared_ptr<RunRecord>> records;
    records.reserve(static_cast<std::size_t>(batch_size));
    auto keep_rec = std::make_shared<RunRecord>([keep_ptr] { (void)keep_ptr->read(); });
    keep_rec->run_first();
    records.push_back(std::move(keep_rec));
    for (int i = 1; i < batch_size; ++i) {
        auto rec = std::make_shared<RunRecord>([sink_ptr] { (void)sink_ptr->read(); });
        rec->run_first();
        records.push_back(std::move(rec));
    }
    sink_ptr->assign(sink_ptr->read() + 1);

    AURORA_TEST_CHECK_GT(engine.seen_source_size(), 4096U);
    AURORA_TEST_CHECK_GT(engine.seen_observer_size(), 4096U);

    const auto kept_src = static_cast<std::size_t>(sources.front()->id());
    const auto kept_obs = static_cast<std::size_t>(records.front()->seq());
    // 销毁期间无任何提交：收缩只能由析构触发的注销检查完成。
    records.erase(records.begin() + 1, records.end());
    sources.erase(sources.begin() + 1, sources.end());
    sink.reset();

    const std::size_t src_bound = std::max<std::size_t>(4096U, kept_src + 1U);
    const std::size_t obs_bound = std::max<std::size_t>(4096U, kept_obs + 1U);
    AURORA_TEST_CHECK_EQ(engine.seen_source_size(), engine.seen_source_capacity());
    AURORA_TEST_CHECK_LE(engine.seen_source_size(), src_bound);
    AURORA_TEST_CHECK_EQ(engine.seen_observer_size(), engine.seen_observer_capacity());
    AURORA_TEST_CHECK_LE(engine.seen_observer_size(), obs_bound);
#else
    AURORA_TEST_SKIP("epoch-array observation ports are gated by AURORA_ENABLE_DEBUG");
#endif
}

AURORA_TEST_CASE(epoch_arrays_capacity_independent_of_cumulative) {
#ifdef AURORA_ENABLE_DEBUG
    // 判据③：两档累计创建量（10 万 / 20 万），各只保留最低 id 来源与最低 seq 订阅者，
    // 两档收缩后的来源戳数组容量都收敛到各自「存活最高身份 + 1」而非各自累计峰值，
    // 证明容量不随累计创建数增长。
    auto &engine = aurora::detail::Engine::instance();

    const auto run_one_tier = [&](int batch_size) -> std::tuple<std::size_t, std::size_t, std::size_t> {
        std::vector<std::shared_ptr<Source<int>>> sources;
        sources.reserve(static_cast<std::size_t>(batch_size));
        for (int i = 0; i < batch_size; ++i) {
            sources.push_back(make_source(i));
        }
        std::shared_ptr<Source<int>> sink = sources.back();
        auto *sink_ptr = sink.get();
        auto *keep_ptr = sources.front().get();
        std::vector<std::shared_ptr<RunRecord>> records;
        records.reserve(static_cast<std::size_t>(batch_size));
        auto keep_rec = std::make_shared<RunRecord>([keep_ptr] { (void)keep_ptr->read(); });
        keep_rec->run_first();
        records.push_back(std::move(keep_rec));
        for (int i = 1; i < batch_size; ++i) {
            auto rec = std::make_shared<RunRecord>([sink_ptr] { (void)sink_ptr->read(); });
            rec->run_first();
            records.push_back(std::move(rec));
        }
        sink_ptr->assign(sink_ptr->read() + 1);
        const std::size_t peak = engine.seen_source_size();
        records.erase(records.begin() + 1, records.end());
        sources.erase(sources.begin() + 1, sources.end());
        sink.reset();
        const std::size_t cap = engine.seen_source_size();
        const std::size_t bound = std::max<std::size_t>(4096U, static_cast<std::size_t>(sources.front()->id()) + 1U);
        return std::make_tuple(peak, cap, bound);
    };

    const auto [peak_small, cap_small, bound_small] = run_one_tier(100000);
    const auto [peak_large, cap_large, bound_large] = run_one_tier(200000);
    // 两档收缩前均为累计峰值，收缩后均落在各自存活上界内、且远小于各自累计峰值。
    AURORA_TEST_CHECK_GT(peak_small, bound_small);
    AURORA_TEST_CHECK_GT(peak_large, bound_large);
    AURORA_TEST_CHECK_LE(cap_small, bound_small);
    AURORA_TEST_CHECK_LE(cap_large, bound_large);
    AURORA_TEST_CHECK_LT(cap_small, peak_small);
    AURORA_TEST_CHECK_LT(cap_large, peak_large);
#else
    AURORA_TEST_SKIP("epoch-array observation ports are gated by AURORA_ENABLE_DEBUG");
#endif
}

AURORA_TEST_CASE(epoch_arrays_empty_when_no_live_sources) {
#ifdef AURORA_ENABLE_DEBUG
    // 判据②：存活集合清空后两枚去重戳数组的 size 与 capacity 均归零。
    constexpr int batch_size = 100000;
    auto &engine = aurora::detail::Engine::instance();

    std::vector<std::shared_ptr<Source<int>>> sources;
    sources.reserve(static_cast<std::size_t>(batch_size));
    for (int i = 0; i < batch_size; ++i) {
        sources.push_back(make_source(i));
    }
    std::shared_ptr<Source<int>> sink = sources.back();
    auto *sink_ptr = sink.get();
    auto *keep_ptr = sources.front().get();
    std::vector<std::shared_ptr<RunRecord>> records;
    records.reserve(static_cast<std::size_t>(batch_size));
    auto keep_rec = std::make_shared<RunRecord>([keep_ptr] { (void)keep_ptr->read(); });
    keep_rec->run_first();
    records.push_back(std::move(keep_rec));
    for (int i = 1; i < batch_size; ++i) {
        auto rec = std::make_shared<RunRecord>([sink_ptr] { (void)sink_ptr->read(); });
        rec->run_first();
        records.push_back(std::move(rec));
    }
    sink_ptr->assign(sink_ptr->read() + 1);

    records.clear();  // 全部订阅者析构：观察者存活集清空，并释放对 sink / keep 的依赖引用
    sources.clear();  // 全部来源 vector 引用释放
    sink.reset();  // 释放本用例对 sink 的最后所有权：来源存活集清空，最后一次析构检查收缩到 0
    AURORA_TEST_CHECK_EQ(engine.seen_source_size(), 0U);
    AURORA_TEST_CHECK_EQ(engine.seen_source_capacity(), 0U);
    AURORA_TEST_CHECK_EQ(engine.seen_observer_size(), 0U);
    AURORA_TEST_CHECK_EQ(engine.seen_observer_capacity(), 0U);
#else
    AURORA_TEST_SKIP("epoch-array observation ports are gated by AURORA_ENABLE_DEBUG");
#endif
}

AURORA_TEST_CASE(reactor_preregistered_source_registers_once) {
    // 固定来源订阅的形态：先手动挂接，再首跑读取同一来源。
    // 依赖替换必须跳过重复登记，否则来源的观察者表里会出现同一条记录两次。
    auto src = make_source(5);
    int runs = 0;
    auto rec = std::make_shared<RunRecord>([&] {
        ++runs;
        (void)src->read();
    });
    src->add_observer(*rec);
    rec->note_preregistered(*src);
    rec->run_first();
    AURORA_TEST_CHECK_EQ(runs, 1);
#ifdef AURORA_ENABLE_DEBUG
    AURORA_TEST_CHECK_EQ(src->observer_count(), std::size_t{1});
#endif

    src->assign(6);
    AURORA_TEST_CHECK_EQ(runs, 2);  // 重复登记会让一次提交触发两轮交付

    rec->cancel();
#ifdef AURORA_ENABLE_DEBUG
    AURORA_TEST_CHECK_EQ(src->observer_count(), std::size_t{0});
#endif
}

AURORA_TEST_CASE(reactor_preregistered_source_unread_is_unlinked) {
    // 回调没有读取其固定来源：依赖替换须摘掉预登记的观察者，后续提交不再触达该记录。
    auto src = make_source(1);
    int runs = 0;
    auto rec = std::make_shared<RunRecord>([&] { ++runs; });
    src->add_observer(*rec);
    rec->note_preregistered(*src);
    rec->run_first();
    AURORA_TEST_CHECK_EQ(runs, 1);
#ifdef AURORA_ENABLE_DEBUG
    AURORA_TEST_CHECK_EQ(src->observer_count(), std::size_t{0});
#endif

    src->assign(2);
    AURORA_TEST_CHECK_EQ(runs, 1);
}

AURORA_TEST_CASE(reactor_cancel_before_first_run_unlinks_preregistered) {
#ifdef AURORA_ENABLE_DEBUG
    // 首跑前取消：预登记的观察者尚未进入依赖边，取消须单独摘除，不留残留登记。
    auto src = make_source(1);
    auto rec = std::make_shared<RunRecord>([] {});
    src->add_observer(*rec);
    rec->note_preregistered(*src);
    AURORA_TEST_CHECK_EQ(src->observer_count(), std::size_t{1});

    rec->cancel();
    AURORA_TEST_CHECK_EQ(src->observer_count(), std::size_t{0});
#else
    AURORA_TEST_SKIP("observer-count port is gated by AURORA_ENABLE_DEBUG");
#endif
}

}  // namespace aurora::test_cases::utest_reactor
