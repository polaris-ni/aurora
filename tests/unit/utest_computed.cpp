/// 测试类型: unit
/// 目标单元: include/aurora/state/computed.h
/// 测试说明: 派生来源的惰性求值与缓存、依赖变化重算、等值去重停止下游传播、always_publish 与自定义等价、
/// 链式派生、条件分支依赖替换、上游来源被派生保活、工厂返回类型与推导、
/// 派生直通展示属性（失效/重绘计数与离屏像素）

#include <cstring>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "aurora/environment/build_context.h"
#include "aurora/render/painter.h"
#include "aurora/state/computed.h"
#include "aurora/state/effect.h"
#include "aurora/state/read_only_state.h"
#include "aurora/state/state.h"
#include "aurora/widget/progress.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_computed {

namespace {

/// 绘制计数探针：重绘次数由真实 `on_paint` 入口统计，不靠像素相同反推。
class PaintProbe final : public ProgressIndicator {
  public:
    using ProgressIndicator::ProgressIndicator;
    int paints = 0;

  protected:
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        ++paints;
        ProgressIndicator::on_paint(p, bounds, ctx);
    }
};

}  // namespace

AURORA_TEST_CASE(computed_is_lazy_and_caches_until_dependency_changes) {
    // 构造不求值；首次 get() 求值一次，重复读取走缓存。
    State<int> n{4};
    int calls = 0;
    auto square = computed([&]() -> int {
        ++calls;
        return n.get() * n.get();
    });
    AURORA_TEST_CHECK_EQ(calls, 0);
    AURORA_TEST_CHECK_EQ(square.get(), 16);
    AURORA_TEST_CHECK_EQ(calls, 1);
    AURORA_TEST_CHECK_EQ(square.get(), 16);
    AURORA_TEST_CHECK_EQ(calls, 1);

    // 依赖变化后：下一次读取重算一次。
    n.set(5);
    AURORA_TEST_CHECK_EQ(square.get(), 25);
    AURORA_TEST_CHECK_EQ(calls, 2);
}

AURORA_TEST_CASE(computed_equal_result_stops_downstream_propagation) {
    // 结果可观察等价时不推进版本：下游 effect 不重跑。
    State<int> n{1};
    auto parity = computed([&]() -> int { return n.get() % 2; });
    int runs = 0;
    int seen = -1;
    auto sub = effect([&]() -> void {
        ++runs;
        seen = parity.get();
    });
    AURORA_TEST_CHECK_EQ(runs, 1);
    AURORA_TEST_CHECK_EQ(seen, 1);

    n.set(3);  // 奇偶不变：重算但结果等价
    AURORA_TEST_CHECK_EQ(runs, 1);
    AURORA_TEST_CHECK_EQ(parity.get(), 1);

    n.set(4);  // 结果改变
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(seen, 0);
}

AURORA_TEST_CASE(computed_always_publish_keeps_publishing_equal_results) {
    // always_publish：每次重算都视为变化，下游照常重跑。
    State<int> n{1};
    auto parity = computed([&]() -> int { return n.get() % 2; }, always_publish);
    int runs = 0;
    auto sub = effect([&]() -> void {
        ++runs;
        (void)parity.get();
    });
    AURORA_TEST_CHECK_EQ(runs, 1);
    n.set(3);  // 值仍为 1，但关闭了去重
    AURORA_TEST_CHECK_EQ(runs, 2);
}

AURORA_TEST_CASE(computed_custom_equivalence_controls_propagation) {
    // 自定义等价：差值小于阈值视为等价，不推进版本。
    State<int> n{0};
    auto bucket = computed([&]() -> int { return n.get(); },
                           [](int left, int right) -> bool { return (left - right) < 10 && (right - left) < 10; });
    int runs = 0;
    auto sub = effect([&]() -> void {
        ++runs;
        (void)bucket.get();
    });
    AURORA_TEST_CHECK_EQ(runs, 1);
    n.set(9);  // 与 0 的差 < 10：等价
    AURORA_TEST_CHECK_EQ(runs, 1);
    n.set(10);  // 差达到阈值：真变化
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(bucket.get(), 10);
}

AURORA_TEST_CASE(computed_chained_derivation_propagates) {
    // 链式派生：上游派生变化传播到下游派生。
    State<int> base{1};
    auto up = computed([&]() -> int { return base.get() + 1; });
    auto down = computed([&]() -> int { return up.get() * 10; });
    AURORA_TEST_CHECK_EQ(up.get(), 2);
    AURORA_TEST_CHECK_EQ(down.get(), 20);
    base.set(2);
    AURORA_TEST_CHECK_EQ(up.get(), 3);
    AURORA_TEST_CHECK_EQ(down.get(), 30);
}

AURORA_TEST_CASE(computed_diamond_observes_only_settled_values) {
    // 菱形：两个派生共享同一上游，终端每轮只看到一致结果（5 → 10），不得观察到 7 的混合态。
    State<int> x{1};
    auto twice = computed([&]() -> int { return x.get() * 2; });
    auto thrice = computed([&]() -> int { return x.get() * 3; });
    std::vector<int> seen;
    auto sub = effect([&]() -> void { seen.push_back(twice.get() + thrice.get()); });
    AURORA_TEST_REQUIRE_EQ(seen.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(seen.at(0), 5);

    x.set(2);
    AURORA_TEST_REQUIRE_EQ(seen.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(seen.at(1), 10);
}

AURORA_TEST_CASE(computed_conditional_reads_replace_dependencies) {
    // 条件分支：每轮按实际读取重建依赖，旧分支的写入不再触发下游。
    State<int> mode{0};
    State<int> x{1};
    State<int> y{2};
    auto pick = computed([&]() -> int { return mode.get() != 0 ? x.get() : y.get(); });
    int runs = 0;
    int seen = 0;
    auto sub = effect([&]() -> void {
        ++runs;
        seen = pick.get();
    });
    AURORA_TEST_CHECK_EQ(runs, 1);
    AURORA_TEST_CHECK_EQ(seen, 2);  // y 分支

    x.set(10);  // 当前未读取 x
    AURORA_TEST_CHECK_EQ(runs, 1);

    mode.set(1);  // 切到 x 分支
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(seen, 10);

    y.set(99);  // y 已退出依赖集
    AURORA_TEST_CHECK_EQ(runs, 2);

    x.set(20);
    AURORA_TEST_CHECK_EQ(runs, 3);
    AURORA_TEST_CHECK_EQ(seen, 20);
}

AURORA_TEST_CASE(computed_keeps_upstream_source_alive) {
    // 派生节点保活其上游来源：源句柄全部释放后仍可读取派生结果。
    auto derived = [&]() -> ReadOnlyState<int> {
        State<int> local{41};
        return computed([local]() -> int { return local.get() + 1; });
    }();
    AURORA_TEST_CHECK_EQ(derived.get(), 42);
}

AURORA_TEST_CASE(computed_factory_returns_read_only_handle) {
    // 工厂返回只读句柄：结果类型按返回值退化推导（const 引用也退化为值类型）。
    State<int> n{3};
    auto doubled = computed([&]() -> int { return n.get() * 2; });
    static_assert(std::is_same_v<decltype(doubled), ReadOnlyState<int>>);
    AURORA_TEST_CHECK_EQ(doubled.get(), 6);
    n.set(5);
    AURORA_TEST_CHECK_EQ(doubled.get(), 10);

    State<std::string> word{std::string{"a"}};
    auto shout = computed([&]() -> std::string { return word.get() + "!"; });
    static_assert(std::is_same_v<decltype(shout), ReadOnlyState<std::string>>);
    AURORA_TEST_CHECK_EQ(shout.get(), std::string{"a!"});

    auto aliased = computed([&]() -> const int & { return n.get(); });
    static_assert(std::is_same_v<decltype(aliased), ReadOnlyState<int>>);
    AURORA_TEST_CHECK_EQ(aliased.get(), 5);
}

AURORA_TEST_CASE(computed_feeds_display_property_with_selective_repaint) {
    // 派生直通：computed 直接填展示属性；上游变化只失效读取该来源的控件。
    // 计数（重绘 / 求值）与离屏像素双证：像素相同不能单独证明「未重绘」。
    State<double> left{0.25};
    State<double> right{0.5};
    int left_calls = 0;
    int right_calls = 0;
    auto left_ratio = computed([&]() -> double {
        ++left_calls;
        return left.get();
    });
    auto right_ratio = computed([&]() -> double {
        ++right_calls;
        return right.get();
    });

    PaintProbe a{left_ratio};  // 只读句柄直接填展示属性
    PaintProbe b{right_ratio};
    BuildContext ctx;
    a.mount(ctx);
    b.mount(ctx);
    // 订阅成功即交付首值一次：两个派生各求值一次（惰性只在无人订阅时成立）。
    const int left_baseline = left_calls;
    const int right_baseline = right_calls;
    AURORA_TEST_CHECK_EQ(left_baseline, 1);
    AURORA_TEST_CHECK_EQ(right_baseline, 1);
    int a_dirty = 0;
    int b_dirty = 0;
    a.on_dirty = [&a_dirty](bool /*layout*/) -> void { ++a_dirty; };
    b.on_dirty = [&b_dirty](bool /*layout*/) -> void { ++b_dirty; };

    const Rect box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 200.0F, .height = 12.0F}};
    Painter before;
    before.begin(200, 12);
    a.paint(before, box, ctx);
    AURORA_TEST_CHECK_EQ(a.paints, 1);
    AURORA_TEST_CHECK_EQ(left_calls, left_baseline);  // 绘制读缓存，不重复求值

    left.set(0.75);  // 只动 left 上游
    AURORA_TEST_CHECK_EQ(left_calls, left_baseline + 1);  // 读取该来源的派生重算恰好一次
    AURORA_TEST_CHECK_EQ(right_calls, right_baseline);  // 同级派生不重算
    AURORA_TEST_CHECK_TRUE(a_dirty > 0);  // 读取该来源的控件被标脏
    AURORA_TEST_CHECK_EQ(b_dirty, 0);  // 同级未读取该来源：不失效

    Painter after;
    after.begin(200, 12);
    a.paint(after, box, ctx);
    AURORA_TEST_CHECK_EQ(a.paints, 2);
    AURORA_TEST_CHECK_EQ(left_calls, left_baseline + 1);  // 第二帧同样读缓存
    AURORA_TEST_CHECK_TRUE(std::memcmp(before.data(), after.data(),
                                       static_cast<std::size_t>(before.width()) * before.height() * 4U) !=
                           0);  // 离屏像素确实变化
    AURORA_TEST_CHECK_EQ(b.paints, 0);  // 未标脏的控件没有被要求重绘
    a.unmount();
    b.unmount();
}

}  // namespace aurora::test_cases::utest_computed
