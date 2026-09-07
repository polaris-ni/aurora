/// 测试类型: unit
/// 目标单元: include/aurora/state/signal_view.h
/// 测试说明: 信号视图基类契约（read() 经 get() 登记、纯视图 anchor()==nullptr、State 覆写 anchor()、锚点/边类型生命周期）单元测试

#include <memory>
#include <string>

#include "aurora/state/effect.h"
#include "aurora/state/signal_view.h"
#include "aurora/state/state.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_signal_view {

namespace {

/// 最小可观察视图：仅实现 get()/subscribe()，用于验证 SignalViewBase 提供的默认 read()/anchor() 契约。
class IntView final : public SignalView<int> {
  public:
    explicit IntView(int v) : m_value(v) {}

    [[nodiscard]] auto get() const -> const int & override {
        ++m_get_calls;
        return m_value;
    }

    auto subscribe(Effect &) -> void override { ++m_subscribe_calls; }

    [[nodiscard]] auto get_calls() const -> int { return m_get_calls; }
    [[nodiscard]] auto subscribe_calls() const -> int { return m_subscribe_calls; }

  private:
    int m_value;
    mutable int m_get_calls = 0;   // get() 为 const，读取计数用 mutable
    int m_subscribe_calls = 0;
};

}  // namespace

AURORA_TEST() {
    // ---- 1. 派生视图：get() 返回自身值，read() 经 get() 路由（默认实现 (void)get()) ----
    {
        IntView v{7};
        AURORA_TEST_CHECK_EQ(v.get(), 7);
        AURORA_TEST_CHECK_EQ(v.get_calls(), 1);

        const int before = v.get_calls();
        v.read();  // SignalView::read 非虚实现即 (void)get()
        AURORA_TEST_CHECK_EQ(v.get_calls(), before + 1);
    }

    // ---- 2. 纯视图 anchor() 默认返回 nullptr；subscribe() 可经接口调用 ----
    {
        IntView v{0};
        SignalViewBase &base = v;
        AURORA_TEST_CHECK(base.anchor() == nullptr);  // 默认契约

        Effect e([]() -> void {});
        v.subscribe(e);
        AURORA_TEST_CHECK_EQ(v.subscribe_calls(), 1);
    }

    // ---- 3. 经基类引用多态读取 ----
    {
        IntView v{42};
        SignalView<int> &sv = v;
        SignalViewBase &base = sv;
        AURORA_TEST_CHECK_EQ(sv.get(), 42);
        base.read();  // 虚派发回 SignalView<int>::read → get
        AURORA_TEST_CHECK(v.get_calls() >= 2);
    }

    // ---- 4. State 是一个 SignalView：可作视图引用读取，且覆写 anchor() 为真实锚点 ----
    {
        State<int> s{3};
        SignalView<int> &sv = s;
        AURORA_TEST_CHECK_EQ(sv.get(), 3);
        AURORA_TEST_CHECK(s.anchor() != nullptr);       // State 覆写为真实锚点
        AURORA_TEST_CHECK(sv.anchor() == s.anchor());   // 经视图引用虚派发取到同一锚点
    }

    // ---- 5. ReactiveAnchor / AnchorPtr：weak_ptr 探测生命周期 ----
    {
        AnchorPtr a = std::make_shared<ReactiveAnchor>();
        std::weak_ptr<ReactiveAnchor> w = a;
        AURORA_TEST_CHECK(!w.expired());
        AURORA_TEST_CHECK(w.lock() == a);
        a.reset();
        AURORA_TEST_CHECK(w.expired());
        AURORA_TEST_CHECK(w.lock() == nullptr);
    }

    // ---- 6. Connection：默认两端 weak 失效、effect_raw 为空 ----
    {
        Connection c;
        AURORA_TEST_CHECK(c.effect_raw == nullptr);
        AURORA_TEST_CHECK(c.effect.expired());
        AURORA_TEST_CHECK(c.state.expired());

        Effect e([]() -> void {});
        c.effect = e.anchor();
        c.effect_raw = &e;
        AURORA_TEST_CHECK(!c.effect.expired());
        AURORA_TEST_CHECK(c.effect_raw == &e);
    }
}

}  // namespace aurora::test_cases::utest_signal_view
