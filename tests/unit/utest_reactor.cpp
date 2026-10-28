/// 测试类型: unit
/// 目标单元: include/aurora/state/detail/reactor.h
/// 测试说明: 响应式内核的提交/等价停止/菱形去重/动态依赖裁剪/回调内写排队/取消/反馈环截断

#include <cstdint>
#include <memory>
#include <stdexcept>
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
    std::weak_ptr<RunRecord> weak = rec;
    rec.reset();
    AURORA_TEST_CHECK(weak.expired());
    src->assign(1);
    AURORA_TEST_CHECK_EQ(src->value(), 1);
}

}  // namespace aurora::test_cases::utest_reactor
