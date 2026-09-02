// strict_mode_test.cpp — 覆盖 #2 strict_mode：au::App().strict_mode() 落地 +
// NDEBUG 安全的真正致命失败（不依赖被剥离的 AURORA_ASSERT）。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

#include <stdexcept>
#include <string>

#include "aurora/aurora.h"
#include "aurora/core/diagnostics.h"
#include "aurora/core/strict_mode.h"

#include "test_harness.h"

using aurora::App;
using aurora::Application;
using aurora::Diagnostics;
using aurora::HeadlessOptions;
using aurora::Node;
using aurora::Scene;
using aurora::set_strict_failure_handler;
using aurora::StrictMode;
using aurora::Text;

namespace {

// 验证 Application 上下文的 set/get 往返。
void test_application_strict_mode() {
    Scene scene{ Node{ Text{ "x" } } };
    Application app{ std::move(scene), 320, 240 };

    app.set_strict_mode(StrictMode::On);
    AURORA_TEST_CHECK(app.strict_mode() == StrictMode::On);

    app.set_strict_mode(StrictMode::Off);
    AURORA_TEST_CHECK(app.strict_mode() == StrictMode::Off);
}

// 验证 App().strict_mode(On) 构建链可编译、携带开关，且不泄漏线程级严格模式。
// 用 Headless Surface + frames(1) 限帧运行：避免自动检测创建真实 Win32 窗口
// 导致测试阻塞在等待手动关窗（CI/ctest 不可交互）。
void test_app_builder_strict_mode() {
    AURORA_TEST_CHECK(aurora::strict_mode() == StrictMode::Off);
    auto win_res = create_window(HeadlessOptions{});
    App()
        .title("t")
        .size(50, 50)
        .strict_mode(StrictMode::On)
        .window(win_res ? std::move(win_res.value()) : nullptr)
        .view(Node{ Text{ "x" } })
        .frames(1)
        .run();
    // run() 不论是否套用，都应还原线程级严格模式（不泄漏到同线程后续运行）。
    AURORA_TEST_CHECK(aurora::strict_mode() == StrictMode::Off);
}

// 核心：严格模式下 degraded 触发 NDEBUG 安全的硬失败（经可注入处理器）。
void test_strict_failure_is_fatal() {
    std::string captured;
    bool thrown = false;

    // 注入处理器：记录消息并抛异常，模拟 CI 捕获致命失败。
    set_strict_failure_handler([&](std::string_view msg) -> void {
        captured = std::string(msg);
        throw std::runtime_error("strict-failure");
    });

    set_strict_mode(StrictMode::On);
    try {
        Diagnostics::degraded("bad color", "paint", "render-degraded");
    } catch (const std::runtime_error &e) {
        thrown = true;
        AURORA_TEST_CHECK(std::string(e.what()) == "strict-failure");
    }
    AURORA_TEST_CHECK(thrown);            // 严格模式下降级确实致命
    AURORA_TEST_CHECK(!captured.empty()); // 处理器收到消息
    AURORA_TEST_CHECK(captured.find("bad color") != std::string::npos);

    // 非严格模式：仅记录，不触发硬失败。
    set_strict_mode(StrictMode::Off);
    captured.clear();
    Diagnostics::degraded("another", "paint", "render-degraded");
    AURORA_TEST_CHECK(captured.empty()); // 未触发处理器

    // 恢复默认（生产默认 std::terminate）。
    set_strict_failure_handler(nullptr);
    set_strict_mode(StrictMode::Off);
}

} // namespace

AURORA_TEST() {
    test_application_strict_mode();
    test_app_builder_strict_mode();
    test_strict_failure_is_fatal();
}
