/// 测试类型: unit
/// 目标单元: include/aurora/state/signal_view.h
/// 测试说明: SignalViewBase/SignalView 的 get/read/subscribe 虚契约与默认空锚点、库类型（State）满足契约、Connection 弱引用锚点失效语义

#include <memory>

#include "aurora/state/signal_view.h"
#include "aurora/state/state.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_signal_view {

/// @brief 最小可观测 SignalView 桩：记录 get 次数与最近订阅者，不覆写 anchor()。
class CountingView final : public SignalView<int> {
  public:
    explicit CountingView(int v) : value_(v) {}

    auto get() const -> const int& override {
        ++reads_;
        return value_;
    }

    auto subscribe(Effect& e) -> void override { last_subscriber_ = &e; }

    [[nodiscard]] auto reads() const -> int { return reads_; }
    [[nodiscard]] auto last_subscriber() const -> Effect* { return last_subscriber_; }

  private:
    int value_;
    mutable int reads_ = 0;
    Effect* last_subscriber_ = nullptr;
};

AURORA_TEST_CASE(signal_view_read_dispatches_to_get) {
    // SignalView<T>::read() 即「读一次 get() 并丢弃」：经基类调用可观测到 get 被执行。
    CountingView v{5};
    AURORA_TEST_CHECK_EQ(v.get(), 5);
    AURORA_TEST_CHECK_EQ(v.reads(), 1);

    SignalViewBase& base = v;
    base.read();
    AURORA_TEST_CHECK_EQ(v.reads(), 2);
}

AURORA_TEST_CASE(signal_view_base_default_anchor_is_null) {
    // 未覆写 anchor() 的纯信号视图默认返回空锚点（如 Reactive 这类委托订阅的视图）。
    CountingView v{1};
    SignalViewBase& base = v;
    AURORA_TEST_CHECK(base.anchor() == nullptr);
    AURORA_TEST_CHECK(v.SignalView<int>::anchor() == nullptr);
}

AURORA_TEST_CASE(signal_view_subscribe_dispatches_through_base) {
    // subscribe 经 SignalViewBase 虚派发到具体实现。
    CountingView v{3};
    SignalViewBase& base = v;
    Effect e{[] {}};
    AURORA_TEST_CHECK(v.last_subscriber() == nullptr);
    base.subscribe(e);
    AURORA_TEST_CHECK(v.last_subscriber() == &e);
}

AURORA_TEST_CASE(signal_view_contract_held_by_state) {
    // 库内类型满足契约：State<T> 可经 SignalView<T> / SignalViewBase 使用，read() 在
    // Effect 作用域内读取即登记依赖。
    State<int> s{5};
    SignalView<int>& typed = s;
    AURORA_TEST_CHECK_EQ(typed.get(), 5);

    SignalViewBase& base = s;
    int runs = 0;
    Effect e{[&] {
        ++runs;
        base.read();
    }};
    e.run();
    AURORA_TEST_CHECK_EQ(runs, 1);
    s.set(6);
    AURORA_TEST_CHECK_EQ(runs, 2);  // read() 建立的依赖在 set 时触发重跑
    AURORA_TEST_CHECK_EQ(typed.get(), 6);
}

AURORA_TEST_CASE(connection_weak_anchors_expire_with_targets) {
    // Connection 以 weak_ptr 引用双方锚点：默认全空；目标锚点释放后 lock 失效。
    AnchorPtr state_anchor = std::make_shared<ReactiveAnchor>();
    AnchorPtr effect_anchor = std::make_shared<ReactiveAnchor>();

    Connection c;
    AURORA_TEST_CHECK(c.effect.expired());
    AURORA_TEST_CHECK(c.state.expired());
    AURORA_TEST_CHECK(c.effect_raw == nullptr);

    c.effect = effect_anchor;
    c.state = state_anchor;
    c.effect_raw = nullptr;
    AURORA_TEST_CHECK_FALSE(c.effect.expired());
    AURORA_TEST_CHECK_FALSE(c.state.expired());
    AURORA_TEST_CHECK(c.effect.lock() == effect_anchor);
    AURORA_TEST_CHECK(c.state.lock() == state_anchor);

    ConnectionPtr shared = std::make_shared<Connection>();
    shared->effect = effect_anchor;
    AURORA_TEST_CHECK_FALSE(shared->effect.expired());

    state_anchor.reset();
    AURORA_TEST_CHECK(c.state.expired());
    AURORA_TEST_CHECK(c.state.lock() == nullptr);
    effect_anchor.reset();
    AURORA_TEST_CHECK(c.effect.expired());
    AURORA_TEST_CHECK(shared->effect.expired());
}

}  // namespace aurora::test_cases::utest_signal_view
