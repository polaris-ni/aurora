/// 测试类型: unit
/// 目标单元: include/aurora/state/state.h / include/aurora/state/read_only_state.h
/// 测试说明: 句柄构造与取值、set 去重 / always_publish / 自定义等价、update 与 notify、拷贝共享与赋值重绑、
/// 值赋值提交、订阅首值与固定来源、写队列按轮排空、StateSink 采集去重与 watch、只读句柄无写入面

#include <concepts>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "aurora/state/effect.h"
#include "aurora/state/read_only_state.h"
#include "aurora/state/state.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_state {

// ---- 类型面：只读句柄没有写入路径，单向转换成立 ----
namespace {

// requires 表达式放在概念模板里才有「成员缺失＝false」的替换语义（非模板上下文是硬错误）。
template <typename T>
concept HasSet = requires(T handle) { handle.set(1); };
template <typename T>
concept HasNotify = requires(T handle) { handle.notify(); };
template <typename T>
concept HasUpdate = requires(T handle) { handle.update([](const int &v) { return v + 1; }); };

}  // namespace

static_assert(std::is_convertible_v<State<int>, ReadOnlyState<int>>);
static_assert(!std::is_convertible_v<ReadOnlyState<int>, State<int>>);
static_assert(!HasSet<ReadOnlyState<int>>);
static_assert(!HasNotify<ReadOnlyState<int>>);
static_assert(!HasUpdate<ReadOnlyState<int>>);
static_assert(HasSet<State<int>>);
static_assert(HasNotify<State<int>>);
static_assert(HasUpdate<State<int>>);

AURORA_TEST_CASE(state_default_and_value_construction) {
    // 默认构造：值取 T{}；值构造：get() 返回初值。
    State<int> s{};
    State<int> t{3};
    AURORA_TEST_CHECK_EQ(s.get(), 0);
    AURORA_TEST_CHECK_EQ(t.get(), 3);

    // 单步隐式转换构造：字面量直接落到 T。
    State<std::string> word{"hi"};
    AURORA_TEST_CHECK_EQ(word.get(), std::string{"hi"});

    // get() 返回存储引用：set 后旧引用观察到新值。
    State<std::string> str{std::string{"a"}};
    const std::string &alias = str.get();
    str.set(std::string{"b"});
    AURORA_TEST_CHECK_EQ(alias, std::string{"b"});
}

AURORA_TEST_CASE(state_set_dedupes_equal_values) {
    // 默认等价策略：可观察等价的写入不替换、不通知。
    State<int> s{1};
    int runs = 0;
    auto sub = s.subscribe([&](const int &) { ++runs; });
    AURORA_TEST_CHECK_EQ(runs, 1);  // 订阅建立即交付首值
    s.set(1);
    AURORA_TEST_CHECK_EQ(runs, 1);
    s.set(2);
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(s.get(), 2);
}

AURORA_TEST_CASE(state_always_publish_and_custom_equivalence) {
    // always_publish：关闭去重，等值提交照样通知。
    State<int> s{1, always_publish};
    int runs = 0;
    auto sub = s.subscribe([&](const int &) { ++runs; });
    s.set(1);
    AURORA_TEST_CHECK_EQ(runs, 2);

    // 自定义等价：按十位取整比较，视为等价时不通知。
    State<int> coarse{10, [](int left, int right) -> bool { return left / 10 == right / 10; }};
    int coarse_runs = 0;
    auto coarse_sub = coarse.subscribe([&](const int &) { ++coarse_runs; });
    coarse.set(19);  // 与 10 同十位：等价
    AURORA_TEST_CHECK_EQ(coarse_runs, 1);
    coarse.set(20);  // 跨十位：真变化
    AURORA_TEST_CHECK_EQ(coarse_runs, 2);
    AURORA_TEST_CHECK_EQ(coarse.get(), 20);
}

AURORA_TEST_CASE(state_update_isolates_inner_reads_from_outer_effect) {
    // update 回调内的读取是纯计算：不扩充外层 effect 的依赖集合。
    State<int> trigger{0};
    State<int> counter{0};
    State<int> noise{0};
    int runs = 0;
    auto sub = effect([&]() -> void {
        ++runs;
        (void)trigger.get();
        counter.update([&](const int &v) -> int {
            (void)noise.get();  // 必须被隔离：不得成为本 effect 的依赖
            return v + 1;
        });
    });
    AURORA_TEST_CHECK_EQ(runs, 1);
    AURORA_TEST_CHECK_EQ(counter.get(), 1);
    noise.set(7);  // noise 未被登记 → effect 不重跑
    AURORA_TEST_CHECK_EQ(runs, 1);
    trigger.set(1);  // trigger 是依赖 → effect 重跑并再次 update
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(counter.get(), 2);
}

AURORA_TEST_CASE(state_notify_publishes_in_place_mutation) {
    // notify 不替换值：原地修改载荷后用它让修改对订阅者可见。
    State<std::vector<int>> list{std::vector<int>{1}};
    int runs = 0;
    std::size_t last_size = 0;
    auto sub = list.subscribe([&](const std::vector<int> &v) {
        ++runs;
        last_size = v.size();
    });
    AURORA_TEST_CHECK_EQ(runs, 1);
    list.update([](const std::vector<int> &v) {
        std::vector<int> next = v;
        next.push_back(2);
        return next;
    });
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(last_size, static_cast<std::size_t>(2));
    list.notify();  // 值未变，强制再发一轮
    AURORA_TEST_CHECK_EQ(runs, 3);
    AURORA_TEST_CHECK_EQ(last_size, static_cast<std::size_t>(2));
}

AURORA_TEST_CASE(state_copy_shares_source_and_assignment_rebinds) {
    // 拷贝即共享来源；拷贝赋值只重绑本句柄。
    State<int> a{1};
    // 本用例刻意按值复制以验证共享语义，复制本身就是被测行为。
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    State<int> b = a;
    b.set(2);
    AURORA_TEST_CHECK_EQ(a.get(), 2);

    State<int> c{9};
    c = a;  // 重绑到 a 的来源（不写值）
    AURORA_TEST_CHECK_EQ(c.get(), 2);
    a.set(3);
    AURORA_TEST_CHECK_EQ(c.get(), 3);
    AURORA_TEST_CHECK_EQ(b.get(), 3);

    // 值赋值：走 set，不换来源。
    c = 8;
    AURORA_TEST_CHECK_EQ(a.get(), 8);
    AURORA_TEST_CHECK_EQ(c.get(), 8);

    // 只读句柄共享同一来源：写经可写句柄，读句柄即时可见。
    State<int> src{5};
    ReadOnlyState<int> ro = src;
    src.set(6);
    AURORA_TEST_CHECK_EQ(ro.get(), 6);
}

AURORA_TEST_CASE(read_only_state_assign_rebinds_without_touching_shared_source) {
    // 只读句柄的赋值是重绑：其他副本仍指向原来源，原来源的值不受影响。
    ReadOnlyState<int> a{1};
    ReadOnlyState<int> b = a;  // 共享来源
    a = 2;  // 只是本句柄换到新的常量来源
    AURORA_TEST_CHECK_EQ(a.get(), 2);
    AURORA_TEST_CHECK_EQ(b.get(), 1);
}

AURORA_TEST_CASE(state_subscribe_delivers_first_value_and_supports_cancel) {
    State<int> s{0};
    std::vector<int> seen;
    auto sub = s.subscribe([&](const int &v) { seen.push_back(v); });
    AURORA_TEST_CHECK_EQ(seen.size(), static_cast<std::size_t>(1));
    AURORA_TEST_CHECK_EQ(seen.front(), 0);
    s.set(1);
    AURORA_TEST_CHECK_EQ(seen.size(), static_cast<std::size_t>(2));
    sub.reset();  // 取消：此后不再回调
    s.set(2);
    AURORA_TEST_CHECK_EQ(seen.size(), static_cast<std::size_t>(2));
    sub.reset();  // 幂等
}

AURORA_TEST_CASE(state_subscription_stays_fixed_to_original_source_after_rebind) {
    // 订阅固定到建立时的来源：句柄换源后，旧订阅仍跟随旧来源。
    State<int> s{0};
    State<int> old_writer = s;  // 仍指向旧来源的写句柄
    int runs = 0;
    int last = -1;
    auto sub = s.subscribe([&](const int &v) {
        ++runs;
        last = v;
    });
    AURORA_TEST_CHECK_EQ(runs, 1);

    State<int> other{100};
    s = other;  // 重绑：订阅不跟走
    other.set(101);  // 新来源的写入不触发旧订阅
    AURORA_TEST_CHECK_EQ(runs, 1);
    AURORA_TEST_CHECK_EQ(s.get(), 101);

    old_writer.set(7);  // 旧来源的写入照常触发
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(last, 7);
}

AURORA_TEST_CASE(state_writes_inside_subscriber_queue_next_round) {
    // 回调内的写入排到后续轮：当前回调读到旧值，外层入口返回后新值落定。
    State<int> s{0};
    State<int> t{0};
    std::string order;
    auto sub_s = s.subscribe([&](const int &v) {
        order += 's';
        t.set(v * 10);
        AURORA_TEST_CHECK_EQ(t.get(), 0);  // 写入尚在队列中
    });
    auto sub_t = t.subscribe([&](const int &) { order += 't'; });
    order.clear();
    s.set(1);
    AURORA_TEST_CHECK_EQ(t.get(), 10);
    AURORA_TEST_CHECK_EQ(order, std::string{"st"});
}

AURORA_TEST_CASE(state_sink_collects_sources_and_watch_fans_out) {
    // 采集按来源身份去重；watch 为每个来源建一条订阅并各首跑一次。
    State<int> a{1};
    State<int> b{2};
    StateSink sink;
    sink.push(a);
    sink.push(a);  // 重复登记同一来源
    sink.push(ReadOnlyState<int>{b});  // 只读句柄登记到同一来源
    AURORA_TEST_CHECK_EQ(sink.sources().size(), static_cast<std::size_t>(2));

    int runs = 0;
    auto subscriptions = sink.watch([&]() { ++runs; });
    AURORA_TEST_CHECK_EQ(runs, 2);  // 每条订阅各首跑一次
    a.set(3);
    AURORA_TEST_CHECK_EQ(runs, 3);  // 只有 a 的订阅触发
    b.set(4);
    AURORA_TEST_CHECK_EQ(runs, 4);
}

}  // namespace aurora::test_cases::utest_state
