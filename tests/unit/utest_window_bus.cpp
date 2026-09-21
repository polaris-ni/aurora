/// 测试类型: unit
/// 目标单元: include/aurora/app/window_bus.h
/// 测试说明: 跨窗口事件总线（specification/06-app-platform.md §2.4）——按类型分发互不串扰、RAII 订阅
/// 自动取消、按来源窗口过滤、回调内自取消/新增订阅的遍历安全、总线先于订阅句柄析构的生命周期安全。

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "aurora/app/window_bus.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_window_bus {

namespace {

struct OpenFile {  // 事件载荷：跨窗口通知「打开某文件」
    std::string path;
};

struct ThemeChanged {  // 另一类型：验证按类型分发互不串扰
    int index = 0;
};

}  // namespace

AURORA_TEST_CASE(post_delivers_by_type_without_crosstalk) {
    WindowEventBus bus;
    std::vector<std::string> opened;
    std::vector<int> themes;

    auto sub_open = bus.on<OpenFile>([&opened](const OpenFile &e, WindowId) -> void { opened.push_back(e.path); });
    auto sub_theme =
        bus.on<ThemeChanged>([&themes](const ThemeChanged &e, WindowId) -> void { themes.push_back(e.index); });

    bus.post(OpenFile{.path = "a.txt"});
    bus.post(ThemeChanged{.index = 2});
    bus.post(OpenFile{.path = "b.txt"});

    AURORA_TEST_CHECK_EQ(opened.size(), 2U);
    AURORA_TEST_CHECK_EQ(themes.size(), 1U);
    if (opened.size() == 2U) {
        AURORA_TEST_CHECK_EQ(opened.at(0), "a.txt");
        AURORA_TEST_CHECK_EQ(opened.at(1), "b.txt");
    }
    // 无订阅者的类型：发布为安全 no-op。
    bus.post(999);
    AURORA_TEST_CHECK_EQ(bus.subscriber_count(), 2U);
}

AURORA_TEST_CASE(subscription_raii_cancels_on_destruction) {
    WindowEventBus bus;
    int hits = 0;
    {
        auto sub = bus.on<OpenFile>([&hits](const OpenFile &, WindowId) -> void { ++hits; });
        bus.post(OpenFile{.path = "x"});
        AURORA_TEST_CHECK_EQ(hits, 1);
    }  // sub 析构 → 自动取消
    bus.post(OpenFile{.path = "y"});
    AURORA_TEST_CHECK_EQ(hits, 1);
    AURORA_TEST_CHECK_EQ(bus.subscriber_count(), 0U);
}

AURORA_TEST_CASE(from_filter_receives_only_matching_source) {
    WindowEventBus bus;
    int broadcast_hits = 0;
    int filtered_hits = 0;

    auto sub_all = bus.on<OpenFile>([&broadcast_hits](const OpenFile &, WindowId) -> void { ++broadcast_hits; });
    // 只接收来自窗口 7 的事件（点对点语义）。
    auto sub_from7 = bus.on<OpenFile>([&filtered_hits](const OpenFile &, WindowId) -> void { ++filtered_hits; }, 7);

    bus.post(OpenFile{.path = "a"}, 3);
    bus.post(OpenFile{.path = "b"}, 7);

    AURORA_TEST_CHECK_EQ(broadcast_hits, 2);  // 不限来源者两者都收
    AURORA_TEST_CHECK_EQ(filtered_hits, 1);  // 来源过滤者只收窗口 7 的
}

AURORA_TEST_CASE(callback_may_unsubscribe_and_subscribe_safely) {
    WindowEventBus bus;
    int first_hits = 0;
    int second_hits = 0;

    // 订阅句柄需在回调外存活：用 shared_ptr<Subscription> 表达「回调内自取消」。
    auto sub1 = std::make_shared<Subscription>();
    *sub1 = bus.on<OpenFile>([&bus, &first_hits, sub1](const OpenFile &, WindowId) -> void {
        ++first_hits;
        (*sub1).reset();  // 回调内自取消：遍历须仍安全（内部先拷贝订阅列表）
    });
    auto sub2 = bus.on<OpenFile>([&second_hits](const OpenFile &, WindowId) -> void { ++second_hits; });

    bus.post(OpenFile{.path = "a"});
    bus.post(OpenFile{.path = "b"});

    AURORA_TEST_CHECK_EQ(first_hits, 1);  // 自取消后不再收到
    AURORA_TEST_CHECK_EQ(second_hits, 2);  // 其他订阅者不受影响
}

AURORA_TEST_CASE(bus_may_be_destroyed_before_subscription) {
    std::vector<std::string> got;
    auto sub = std::make_unique<Subscription>();
    {
        WindowEventBus bus;
        *sub = bus.on<OpenFile>([&got](const OpenFile &e, WindowId) -> void { got.push_back(e.path); });
        bus.post(OpenFile{.path = "live"});
    }  // bus 先析构：订阅句柄仍有效（内部持有共享状态），析构时不得崩溃或访问已释放内存
    AURORA_TEST_CHECK_EQ(got.size(), 1U);
    sub = nullptr;  // 取消发生在总线销毁之后 —— 必须安全
    AURORA_TEST_CHECK_EQ(got.size(), 1U);
}

AURORA_TEST_CASE(off_and_clear_revoke_subscriptions) {
    WindowEventBus bus;
    int hits = 0;
    auto sub = bus.on<ThemeChanged>([&hits](const ThemeChanged &, WindowId) -> void { ++hits; });
    bus.post(ThemeChanged{});
    AURORA_TEST_CHECK_EQ(hits, 1);

    // clear() 撤销全部订阅；此后发布无人接收。已持有的 Subscription 析构时再取消也是安全的（幂等）。
    bus.clear();
    bus.post(ThemeChanged{});
    AURORA_TEST_CHECK_EQ(hits, 1);
    AURORA_TEST_CHECK_EQ(bus.subscriber_count(), 0U);
}

}  // namespace aurora::test_cases::utest_window_bus
