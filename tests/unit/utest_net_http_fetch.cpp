/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/http_fetch.cpp
/// 目标单元: src/aurora/net/detail/http_fetch.h
/// 测试说明: wasm fetch 引擎桥的双腿名册与符号契约——native 腿钉「腿不激活 + 存根不可用」，
/// wasm 腿钉「腿激活 + NullEngine 能力位按实测改判」；交换行为判据（重定向 / 超时 / 错误映射 /
/// Jar 往返）由 node 真跑探针 `tools/verify/wasm_fetch_live_probe.cpp`（`wasm` 预设构建 + node ≥ 18
/// 运行）承担——本套件在事件循环形态下无法等待 promise 结算，故不在此谎报运行判据。

#include <chrono>

#include "aurora/core/error_codes.h"
#include "aurora/core/platform.h"
#include "aurora/core/result.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/detail/http_fetch.h"
#include "aurora/net/engine.h"
#include "aurora/state/coroutine.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_http_fetch {

using aurora::CoroTask;
using aurora::ErrorCode;
using aurora::Result;
using aurora::net::Cancellation;
using aurora::net::detail::fetch_exchange;
using aurora::net::detail::fetch_leg_active;
using aurora::net::detail::FetchOutcome;
using aurora::net::detail::FetchSpec;

/// @brief 驱动一次 `fetch_exchange`（`CallbackAwaiter` 须协程帧；native 存根同步结算，立即收尾）。
/// @param spec 交换规格（native 存根不消费）。
/// @return 交换结局。
auto drive_exchange(const FetchSpec &spec) -> CoroTask<Result<FetchOutcome>> {
    Cancellation token;
    auto outcome = co_await fetch_exchange(spec, token);
    co_return outcome;
}

AURORA_TEST_CASE(fetch_leg_matches_platform) {
#if defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_CHECK_TRUE(fetch_leg_active());
#else
    AURORA_TEST_CHECK_FALSE(fetch_leg_active());
#endif
}

AURORA_TEST_CASE(fetch_exchange_stub_reports_unavailable_on_native) {
#if defined(AURORA_PLATFORM_WASM)
    // wasm 腿的 fetch_exchange 是真桥：调它就发真网络请求，判据由 node 探针承担（见文件头注释）。
    AURORA_TEST_SKIP("the real fetch bridge is exercised by the node probe (tools/verify), not here");
#else
    const FetchSpec spec{.url = "https://stub.test/never", .method_text = "GET"};
    auto task = drive_exchange(spec);
    AURORA_TEST_REQUIRE_MSG(task.is_done(), "the native stub must settle synchronously");
    // `CoroTask<Result<T>>` 的 result() 是双包形态（T 本身已是 Result）：剥一层取交换结局。
    const auto wrapped = task.result();
    AURORA_TEST_REQUIRE_MSG(wrapped.ok(), wrapped.ok() ? std::string{} : wrapped.error().message);
    const auto &outcome = wrapped.value();
    AURORA_TEST_REQUIRE(!outcome.ok());
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, ErrorCode::NetPlatformUnavailable);
#endif
}

AURORA_TEST_CASE(wasm_engine_capability_bits_measured) {
#if defined(AURORA_PLATFORM_WASM)
    const auto caps = aurora::net::Engine::instance().capabilities();
    // 能力位按实测改判（§16 第 37 项）：自定义请求头可传（预检失败以错误形态抵达）、流式体本切片
    // 不消费 ReadableStream、传输形态本腿无 fd 语义。
    AURORA_TEST_CHECK_TRUE(caps.custom_headers);
    AURORA_TEST_CHECK_FALSE(caps.streaming);
    AURORA_TEST_CHECK_FALSE(caps.tcp);
#else
    AURORA_TEST_SKIP("NullEngine is only instantiated on the wasm leg");
#endif
}

}  // namespace aurora::test_cases::utest_net_http_fetch
