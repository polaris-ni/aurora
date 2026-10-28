/// 测试类型: unit
/// 目标单元: include/aurora/state/effect.h
/// 测试说明: 副作用首跑与依赖追踪、作用域外读取不登记、条件依赖替换、凭证取消与丢弃即停、
/// 回调内写入排到后续轮、自写反馈环有界停止

#include <string>
#include <utility>

#include "aurora/state/effect.h"
#include "aurora/state/state.h"
#include "aurora/state/subscription.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_effect {

AURORA_TEST_CASE(effect_runs_immediately_and_tracks_dependencies) {
    // 调用即首跑；依赖变化后重跑。
    State<int> s{0};
    int runs = 0;
    int last = -1;
    auto sub = effect([&]() -> void {
        ++runs;
        last = s.get();
    });
    AURORA_TEST_CHECK_EQ(runs, 1);
    AURORA_TEST_CHECK_EQ(last, 0);
    AURORA_TEST_CHECK(sub.active());

    s.set(5);
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(last, 5);

    s.set(5);  // 等值写入不通知
    AURORA_TEST_CHECK_EQ(runs, 2);
}

AURORA_TEST_CASE(effect_ignores_sources_read_outside_callback) {
    // 回调体不读取的来源不登记：写入不触发重跑。
    State<int> watched{0};
    State<int> bystander{0};
    int runs = 0;
    auto sub = effect([&]() -> void {
        ++runs;
        (void)watched.get();
    });
    AURORA_TEST_CHECK_EQ(runs, 1);
    bystander.set(1);
    AURORA_TEST_CHECK_EQ(runs, 1);
    watched.set(1);
    AURORA_TEST_CHECK_EQ(runs, 2);
}

AURORA_TEST_CASE(effect_conditional_dependencies_are_replaced_each_run) {
    // 每轮按实际读取替换依赖：旧分支写入不再触发。
    State<int> mode{0};
    State<int> x{1};
    State<int> y{2};
    int runs = 0;
    int seen = 0;
    auto sub = effect([&]() -> void {
        ++runs;
        seen = mode.get() != 0 ? x.get() : y.get();
    });
    AURORA_TEST_CHECK_EQ(runs, 1);
    AURORA_TEST_CHECK_EQ(seen, 2);

    x.set(10);  // 当前未读取 x
    AURORA_TEST_CHECK_EQ(runs, 1);

    mode.set(1);  // 切换分支
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(seen, 10);

    y.set(99);  // y 已退出依赖集
    AURORA_TEST_CHECK_EQ(runs, 2);

    x.set(20);
    AURORA_TEST_CHECK_EQ(runs, 3);
    AURORA_TEST_CHECK_EQ(seen, 20);
}

AURORA_TEST_CASE(effect_cancelled_by_reset_and_by_destruction) {
    // reset 与析构都取消：此后写入不再回调。
    State<int> s{0};
    int runs = 0;
    auto sub = effect([&]() -> void {
        ++runs;
        (void)s.get();
    });
    AURORA_TEST_CHECK_EQ(runs, 1);
    sub.reset();
    AURORA_TEST_CHECK(!sub.active());
    s.set(1);
    AURORA_TEST_CHECK_EQ(runs, 1);

    {
        auto transient = effect([&]() -> void {
            ++runs;
            (void)s.get();
        });
        AURORA_TEST_CHECK_EQ(runs, 2);
    }  // transient 析构 → 退订
    s.set(2);
    AURORA_TEST_CHECK_EQ(runs, 2);
}

AURORA_TEST_CASE(effect_result_discarded_runs_once_without_living_subscription) {
    // 丢弃返回值：首跑照常发生，之后不留后台订阅。
    State<int> s{0};
    int runs = 0;
    (void)effect([&]() -> void {  // 刻意丢弃：断言的就是「无凭证存活时退订」这条路径
        ++runs;
        (void)s.get();
    });
    AURORA_TEST_CHECK_EQ(runs, 1);
    s.set(1);
    AURORA_TEST_CHECK_EQ(runs, 1);
}

AURORA_TEST_CASE(effect_writes_within_callback_settle_after_current_round) {
    // 回调内写入排到后续轮：同轮读取仍是旧值，入口返回后新值落定。
    State<int> source{1};
    State<int> mirror{0};
    std::string order;
    auto sub = effect([&]() -> void {
        const int v = source.get();
        const int before = mirror.get();
        order += 'e';
        mirror.set(v * 10);
        AURORA_TEST_CHECK_EQ(mirror.get(), before);  // 尚在写队列：读数不变
    });
    auto mirror_sub = mirror.subscribe([&](const int &) { order += 'm'; });
    order.clear();
    source.set(2);
    AURORA_TEST_CHECK_EQ(mirror.get(), 20);
    AURORA_TEST_CHECK_EQ(order, std::string{"em"});
}

AURORA_TEST_CASE(effect_self_write_feedback_loop_stops_bounded) {
    // 自写反馈环：同一轮内因果链重复出现即停止，绝不无限循环。
    // 停止时引擎按设计记录一条 "reactive feedback loop cut" 错误日志，属预期输出。
    State<int> s{0};
    int runs = 0;
    auto sub = effect([&]() -> void {
        ++runs;
        s.set(s.get() + 1);  // 每轮自增：第二次重跑会被因果链截断
    });
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(s.get(), 2);

    // 外部写入重新开启链条，仍保持有界。
    s.set(10);
    AURORA_TEST_CHECK_EQ(runs, 3);
    AURORA_TEST_CHECK_EQ(s.get(), 11);
}

}  // namespace aurora::test_cases::utest_effect
