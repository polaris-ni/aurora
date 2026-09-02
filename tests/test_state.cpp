// state_test.cpp — 覆盖响应式状态系统（原缺口模块）。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
// ── API 覆盖映射 ─────────────────────────────
// state/store.h(Store 读写/订阅)、state/signal_view.h(SignalView 只读视图)；
// state/state_graph.h、state/state_registry.h → 经状态系统与序列化链路间接行使（无独立直测函数，见
// test_serialization）。

#include <chrono>
#include <thread>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::Action;
using aurora::Binding;
using aurora::Computed;
using aurora::computed;
using aurora::Effect;
using aurora::ErrorCode;
using aurora::make_store;
using aurora::Reactive;
using aurora::Reducer;
using aurora::Result;
using aurora::SignalView;
using aurora::State;
using std::reduce;

// ---- State 读写 / 订阅(Effect 依赖追踪) ----
static void test_state() {
    State s{ 0 };
    AURORA_TEST_CHECK_MSG(s.get() == 0, "State: initial value");
    int observed = -1;
    Effect e([&]() -> void { observed = s.get(); });
    e.run(); // 首次运行登记 s 为依赖
    AURORA_TEST_CHECK_MSG(observed == 0, "State: Effect reads initial value");
    s.set(5); // 触发依赖 Effect 重跑
    AURORA_TEST_CHECK_MSG(observed == 5, "State: set notifies dependent Effect");
    AURORA_TEST_CHECK_MSG(!e.is_disposed(), "Effect: not disposed");
    e.dispose();
    AURORA_TEST_CHECK_MSG(e.is_disposed(), "Effect: disposed after dispose()");

    // Effect run 幂等：多次 run 每次都执行 fn
    int runs = 0;
    Effect e2([&]() -> void {
        ++runs;
        (void)s.get();
    });
    e2.run();
    e2.run();
    AURORA_TEST_CHECK_MSG(runs == 2, "Effect: explicit run() executes fn each time");
}

// ---- Computed 依赖追踪 / 重算 ----
static void test_computed() {
    State a{ 1 };
    State b{ 2 };
    const Computed<int> c([&]() -> int { return a.get() + b.get(); });
    AURORA_TEST_CHECK_MSG(c.get() == 3, "Computed: initial derived value");
    a.set(10);
    AURORA_TEST_CHECK_MSG(c.get() == 12, "Computed: recomputes when dependency changes");
    b.set(20);
    AURORA_TEST_CHECK_MSG(c.get() == 30, "Computed: recomputes on second dependency");

    // au::computed 工厂：T 由 lambda 返回类型推导，行为与显式 Computed<T> 一致
    const auto cf = computed([&] -> int { return a.get() * 2; });
    AURORA_TEST_CHECK_MSG(cf.get() == 20, "computed factory: initial derived value");
    a.set(5);
    AURORA_TEST_CHECK_MSG(cf.get() == 10, "computed factory: recomputes when dependency changes");
}

// ---- Binding ----
static void test_binding() {
    State s{ 5 };
    Binding bd{ s };
    AURORA_TEST_CHECK_MSG(bd.bound(), "Binding: bound() true after construction");
    AURORA_TEST_CHECK_MSG(bd.get() == 5, "Binding: get() forwards to State");
    bd.set(9);
    AURORA_TEST_CHECK_MSG(s.get() == 9, "Binding: set() writes through to State");
    AURORA_TEST_CHECK_MSG(bd.target() == &s, "Binding: target() returns upstream State");

    const Binding<int> unbound;
    AURORA_TEST_CHECK_MSG(!unbound.bound(), "Binding: default not bound");
    AURORA_TEST_CHECK_MSG(unbound.target() == nullptr, "Binding: default target null");
}

// ---- Reactive ----
static void test_reactive() {
    Reactive r{ 3 };
    AURORA_TEST_CHECK_MSG(r.get() == 3, "Reactive: value ctor");
    const Reactive r2 = 7; // 隐式转换
    AURORA_TEST_CHECK_MSG(r2.get() == 7, "Reactive: implicit value ctor");
    r.set(4);
    AURORA_TEST_CHECK_MSG(r.get() == 4, "Reactive: set()");
    AURORA_TEST_CHECK_MSG(r.state().get() == 4, "Reactive: state() exposes underlying State");

    const auto shared = std::make_shared<State<int>>(100);
    const Reactive r3{ shared };
    AURORA_TEST_CHECK_MSG(r3.get() == 100, "Reactive: from shared State");
    shared->set(200);
    AURORA_TEST_CHECK_MSG(r3.get() == 200, "Reactive: shares upstream State value");
}

// ---- SignalView 基类接口 ----
static void test_signal_view() {
    const State<std::string> s{ "hi" };
    const SignalView<std::string> &sv = s;
    AURORA_TEST_CHECK_MSG(sv.get() == "hi", "SignalView: get() via base ref");
}

// ---- Store / Action / Reducer ----
struct Counter {
    int count = 0;
};

static void test_store() {
    const Reducer<Counter> reduce = [](const Counter &s, const Action &a) -> Counter {
        Counter n = s;
        if (a.type == "inc") {
            ++n.count;
        } else if (a.type == "set") {
            if (const int *p = a.payload_as<int>()) {
                n.count = *p;
            }
        }
        return n;
    };

    const auto store = make_store(Counter{}, reduce);
    AURORA_TEST_CHECK_MSG(store->get_state().count == 0, "Store: initial state");

    store->dispatch(Action{ "inc" });
    AURORA_TEST_CHECK_MSG(store->get_state().count == 1, "Store: dispatch inc");

    const Action set_a{ "set", 42 };
    store->dispatch(set_a);
    AURORA_TEST_CHECK_MSG(store->get_state().count == 42, "Store: dispatch set with payload");
    const int *p = set_a.payload_as<int>();
    AURORA_TEST_CHECK_MSG(p != nullptr && *p == 42, "Action: payload_as<int> recovers value");
    // 注：payload_as<T>() 仅做空指针检查，不做类型校验（按值类型擦除存储）。
    // 调用方须保证类型匹配；不匹配时行为是未定义，这里只断言合法用法与空载荷。

    const Action no_payload{ "inc" };
    AURORA_TEST_CHECK_MSG(no_payload.payload_as<int>() == nullptr, "Action: no payload returns null");

    int calls = 0;
    const auto unsub = store->subscribe([&](const Counter &, const Counter &) -> void { ++calls; });
    store->dispatch(Action{ "inc" });
    AURORA_TEST_CHECK_MSG(calls == 1, "Store: listener called on dispatch");
    unsub();
    store->dispatch(Action{ "inc" });
    AURORA_TEST_CHECK_MSG(calls == 1, "Store: listener removed after unsubscribe");

    const auto sig = store->as_signal();
    AURORA_TEST_CHECK_MSG(sig->get().count == 44, "Store: as_signal reflects current state");
    store->dispatch(Action{ "inc" });
    AURORA_TEST_CHECK_MSG(sig->get().count == 45, "Store: as_signal updates on dispatch");
}

// ---- async ----
static void test_async() {
    bool done = false;
    int val = 0;
    au::async([]() -> int { return 21 * 2; }).then([&](const Result<int> &r) -> void {
        done = r.ok();
        if (r.ok()) {
            val = r.value();
        }
    });
    // 默认无事件循环：回调在后台线程直接执行，等待其完成。
    for (int i = 0; i < 100 && !done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    AURORA_TEST_CHECK_MSG(done, "async: then callback fired (success)");
    AURORA_TEST_CHECK_MSG(val == 42, "async: success value propagated");

    bool done2 = false;
    bool failed = false;
    au::async([]() -> Result<int> {
        return make_error(ErrorCode::GeneralUnknown, "nope");
    }).then([&](const Result<int> &r) -> void {
        done2 = true;
        failed = !r.ok();
    });
    for (int i = 0; i < 100 && !done2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    AURORA_TEST_CHECK_MSG(done2, "async: then callback fired (error path)");
    AURORA_TEST_CHECK_MSG(failed, "async: error propagated as !ok");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== state_test ===\n");
    test_state();
    test_computed();
    test_binding();
    test_reactive();
    test_signal_view();
    test_store();
    test_async();
}
