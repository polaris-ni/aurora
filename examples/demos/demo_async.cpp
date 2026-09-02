// 异步 demo：au::async().then() 回调 + with_timeout + 协程 co_await 用法。
#include <chrono>

#include "demo_common.h"

// 协程示例：在后台线程池计算，续体回到主线程（无事件循环时由 worker 直接 resume）。
static auto demo_coro() -> au::CoroTask<void> {
    au::Result<int> r = co_await au::co_async([]() -> int {
        // 模拟后台计算
        return 21 * 2;
    });
    if (r) {
        AURORA_LOG_INFO("coro", "computed = ", r.value());
    } else {
        AURORA_LOG_ERROR("coro", "error: ", r.error().message);
    }
    co_return;
}

auto main() -> int {
    // 共享状态：Text 经 Reactive 订阅，后台回调写回时自动触发刷新（否则只拍静态快照）。
    auto status = std::make_shared<au::State<au::LocalizedString>>("running…");

    // 回调式：后台任务经 au::async().then() 回调，失败/超时均经 Result 传递。
    au::async([]() -> std::string {
        // 模拟后台计算
        std::this_thread::sleep_for(std::chrono::seconds(2));
        return "done";
    })
        .with_timeout(std::chrono::seconds(5))
        .then([status](const au::Result<std::string> &r) -> void {
            if (r) {
                status->set("result = " + r.value());
            } else {
                status->set("error: " + r.error().message);
            }
        });

    // 协程式：launch 后由事件循环/线程池驱动，续体回主线程。
    au::launch(demo_coro());

    au::Node root = au::Column{
        GradientTitle{ "异步 / Async" },
        gap(12),
        au::Text{ au::LocalizedString{ "后台任务经 au::async().with_timeout().then() 回调" } },
        au::Text{ au::LocalizedString{ "协程：co_await au::co_async(...) 续体回主线程" } },
        au::Text{ au::TextProps{ .content = au::Reactive{ status } } },
    };
    return run_demo(Card{ std::move(root) }, "Async · Aurora Demo", 520.0f, 360.0f);
}
