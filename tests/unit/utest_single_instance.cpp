/// 测试类型: unit
/// 目标单元: include/aurora/app/single_instance.h
/// 测试说明: 应用身份与单实例守卫的契约——实例键的稳定性（同一可执行路径在参数 / 调用次数变化下
/// 不变，规范路径等价，Windows / macOS 大小写折叠）、显式实例键优先、命令行 URI 的提取与按到达序
/// 交付、回调登记前不丢队列、三种获取结果（`Primary` / `Forwarded` / `FallbackNewInstance`）互不顶替、
/// 通道故障不改状态、`release` / `clear_all` 幂等复位，以及无跨进程通道时如实报 `Primary`。
///
/// 为何单测能跑产品代码路径：单实例通道是进程级单槽（`desktop_shell::SingleInstanceBackend`），
/// 本文件注入伪后端；真机上平台桥（Win32 互斥量 + `WM_COPYDATA` / Linux 锁文件 + Unix 套接字 /
/// macOS AppleEvent）登记同名后端，公共层一行不改。
///
/// 变异自证（改坏实现应转红的五条）：
///  * 把命令行参数或 mtime 混进实例键 → `instance_key_ignores_arguments_and_call_history` 转红；
///  * 取消路径词法消解（`.` / `..` / 重复分隔符）→ 同一用例的等价路径断言转红；
///  * 回调登记前丢弃已排队 URI → `queued_uris_survive_until_a_callback_is_registered` 转红；
///  * 把 `Forwarded` / `FallbackNewInstance` 报成 `Primary` → `second_instance_outcomes_are_reported_honestly` 转红；
///  * 通道故障时仍置 `acquired_` → `backend_failure_leaves_the_guard_unacquired` 转红。

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/app/detail/shell_integration_backend.h"
#include "aurora/app/single_instance.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_single_instance {

namespace {

/// @brief 伪造单实例后端的观测状态与可编程结论。
struct FakeInstance {
    int acquire_calls = 0;  ///< 获取次数
    int poll_calls = 0;  ///< 排空次数
    int release_calls = 0;  ///< 释放次数
    std::vector<std::string> last_uris;  ///< 最近一次获取收到的命令行 URI
    std::function<void(std::string)> sink;  ///< 公共层交来的入队回调（模拟平台通道后续收到新 URI）
    SingleInstanceOutcome outcome = SingleInstanceOutcome::Primary;  ///< 获取结论
    bool acquire_fails = false;  ///< 获取是否报错
    std::string failure_message{"single-instance: channel unavailable"};  ///< 失败 detail
};

/// @brief 用例夹具：安装伪后端 → 复位单例 → 结束时先复位再摘除后端。
///
/// 顺序不可颠倒：`clear_all()` 会经 `active_backend()` 调用当前后端的 `release`；先摘后端会让
/// 复位落到按需自举的真桥上（Windows / Linux 会真的创建互斥量 / 锁文件）。
class Fixture {
  public:
    Fixture() : instance(SingleInstance::instance()) {
        desktop_shell::unregister_single_instance_backend();
        install();
        instance.clear_all();
    }
    ~Fixture() {
        instance.clear_all();
        desktop_shell::unregister_single_instance_backend();
    }
    Fixture(const Fixture &) = delete;
    auto operator=(const Fixture &) -> Fixture & = delete;
    Fixture(Fixture &&) = delete;
    auto operator=(Fixture &&) -> Fixture & = delete;

    FakeInstance fake;  ///< 观测状态
    SingleInstance &instance;  ///< 被测单例

  private:
    void install() {
        desktop_shell::SingleInstanceBackend backend;
        backend.acquire = [this](const AppIdentity &, const std::vector<std::string> &uris,
                                 const std::function<void(std::string)> &sink) -> Result<SingleInstanceOutcome> {
            ++fake.acquire_calls;
            fake.last_uris = uris;
            fake.sink = sink;
            if (fake.acquire_fails) {
                return make_error(ErrorCode::GeneralInvalidArgument, fake.failure_message);
            }
            return fake.outcome;
        };
        backend.poll = [this]() { ++fake.poll_calls; };
        backend.release = [this]() { ++fake.release_calls; };
        desktop_shell::register_single_instance_backend(std::move(backend));
    }
};

/// @brief 一份带可执行路径与若干参数的稳定身份。
[[nodiscard]] auto identity_primary() -> AppIdentity {
    AppIdentity identity;
    identity.executable_path = "/opt/aurora/bin/app";
    return identity;
}

/// @brief 收集所有经回调交付的 URI。
class UriRecorder {
  public:
    void attach(SingleInstance &instance) {
        instance.set_on_uri_received([this](std::string_view uri) { received.emplace_back(uri); });
    }
    std::vector<std::string> received;  ///< 按到达序
};

}  // namespace

AURORA_TEST_CASE(instance_key_ignores_arguments_and_call_history) {
    Fixture fixture;

    auto first = identity_primary();
    first.arguments = {"--profile", "a"};
    auto second = identity_primary();
    second.arguments = {"--profile", "b", "myapp://open/1"};

    // 参数快照不参与实例键：同一可执行文件的两次启动必须认得出彼此。
    AURORA_TEST_CHECK_EQ(resolve_instance_key(first), resolve_instance_key(second));
    // 同一路径重复调用稳定。
    AURORA_TEST_CHECK_EQ(derive_instance_key("/opt/aurora/bin/app"), derive_instance_key("/opt/aurora/bin/app"));
    // 不同可执行文件落到不同键。
    AURORA_TEST_CHECK_NE(derive_instance_key("/opt/aurora/bin/app"), derive_instance_key("/opt/aurora/bin/other"));
    // 空输入也给出非空键（平台桥不为空串特殊分叉）。
    AURORA_TEST_CHECK_FALSE(derive_instance_key("").empty());

    // 词法消解：`.` / `..` 与重复分隔符不影响键。
    AURORA_TEST_CHECK_EQ(derive_instance_key("/opt/aurora/./bin/../bin/app"),
                         derive_instance_key("/opt/aurora/bin/app"));
    AURORA_TEST_CHECK_EQ(derive_instance_key("/opt//aurora///bin/app"), derive_instance_key("/opt/aurora/bin/app"));

    // 显式实例键优先（同一可执行文件按配置区分实例）。
    AppIdentity explicit_key = identity_primary();
    explicit_key.instance_key = "tenant-a";
    AURORA_TEST_CHECK_EQ(resolve_instance_key(explicit_key), std::string{"tenant-a"});

    // 大小写折叠：Windows / macOS 的路径大小写不敏感，Linux / BSD 敏感。
#if defined(AURORA_PLATFORM_WINDOWS) || defined(AURORA_PLATFORM_MACOS)
    AURORA_TEST_CHECK_EQ(derive_instance_key("/opt/aurora/bin/APP"), derive_instance_key("/opt/aurora/bin/app"));
#else
    AURORA_TEST_CHECK_NE(derive_instance_key("/opt/aurora/bin/APP"), derive_instance_key("/opt/aurora/bin/app"));
#endif
}

AURORA_TEST_CASE(primary_forwards_argv_uris_to_the_callback_in_order) {
    Fixture fixture;
    UriRecorder recorder;

    auto identity = identity_primary();
    identity.arguments = {"--profile", "a", "myapp://open/42"};
    auto outcome = fixture.instance.acquire(identity);
    AURORA_TEST_REQUIRE(outcome.ok());
    AURORA_TEST_CHECK(outcome.value() == SingleInstanceOutcome::Primary);
    AURORA_TEST_CHECK(fixture.instance.is_primary());
    AURORA_TEST_CHECK_EQ(fixture.instance.instance_key(), resolve_instance_key(identity));
    AURORA_TEST_CHECK_EQ(fixture.fake.acquire_calls, 1);
    AURORA_TEST_REQUIRE_EQ(fixture.fake.last_uris.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(fixture.fake.last_uris[0], std::string{"myapp://open/42"});

    // 回调登记即交付排队项（按到达序）。
    recorder.attach(fixture.instance);
    AURORA_TEST_REQUIRE_EQ(recorder.received.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(recorder.received[0], std::string{"myapp://open/42"});

    // 重复 acquire 幂等：不再触达平台桥，返回上次结论。
    auto again = fixture.instance.acquire(identity);
    AURORA_TEST_REQUIRE(again.ok());
    AURORA_TEST_CHECK(again.value() == SingleInstanceOutcome::Primary);
    AURORA_TEST_CHECK_EQ(fixture.fake.acquire_calls, 1);
}

AURORA_TEST_CASE(non_uri_arguments_are_not_queued) {
    Fixture fixture;

    auto identity = identity_primary();
    identity.arguments = {"--verbose", "C:\\data\\file.txt", "-j4"};
    auto outcome = fixture.instance.acquire(identity);
    AURORA_TEST_REQUIRE(outcome.ok());
    AURORA_TEST_CHECK(outcome.value() == SingleInstanceOutcome::Primary);
    AURORA_TEST_CHECK(fixture.fake.last_uris.empty());

    UriRecorder recorder;
    recorder.attach(fixture.instance);
    AURORA_TEST_CHECK(recorder.received.empty());
}

AURORA_TEST_CASE(queued_uris_survive_until_a_callback_is_registered) {
    Fixture fixture;

    auto identity = identity_primary();
    identity.arguments = {"myapp://open/early"};
    auto outcome = fixture.instance.acquire(identity);
    AURORA_TEST_REQUIRE(outcome.ok());

    // 未登记回调时 poll 不丢项、也不崩。
    fixture.instance.poll();
    AURORA_TEST_CHECK_EQ(fixture.fake.poll_calls, 1);

    UriRecorder recorder;
    recorder.attach(fixture.instance);
    AURORA_TEST_REQUIRE_EQ(recorder.received.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(recorder.received[0], std::string{"myapp://open/early"});
}

AURORA_TEST_CASE(forwarded_uris_arrive_through_poll_in_order) {
    Fixture fixture;

    AURORA_TEST_REQUIRE(fixture.instance.acquire(identity_primary()).ok());
    UriRecorder recorder;
    recorder.attach(fixture.instance);
    AURORA_TEST_REQUIRE(fixture.fake.sink != nullptr);

    // 平台通道后续收到的新 URI 只入队；回调统一由 poll 在调用线程交付。
    fixture.fake.sink("myapp://open/second");
    fixture.fake.sink("myapp://open/third");
    AURORA_TEST_CHECK(recorder.received.empty());
    fixture.instance.poll();
    AURORA_TEST_CHECK_EQ(fixture.fake.poll_calls, 1);
    AURORA_TEST_REQUIRE_EQ(recorder.received.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(recorder.received[0], std::string{"myapp://open/second"});
    AURORA_TEST_CHECK_EQ(recorder.received[1], std::string{"myapp://open/third"});

    // 空回调 = 清除；此后到达的 URI 留在队列里，重新登记时补交付。
    fixture.instance.set_on_uri_received({});
    fixture.fake.sink("myapp://open/fourth");
    fixture.instance.poll();
    AURORA_TEST_CHECK_EQ(recorder.received.size(), std::size_t{2});
    recorder.attach(fixture.instance);
    AURORA_TEST_REQUIRE_EQ(recorder.received.size(), std::size_t{3});
    AURORA_TEST_CHECK_EQ(recorder.received[2], std::string{"myapp://open/fourth"});
}

AURORA_TEST_CASE(second_instance_outcomes_are_reported_honestly) {
    Fixture fixture;

    fixture.fake.outcome = SingleInstanceOutcome::Forwarded;
    auto forwarded = fixture.instance.acquire(identity_primary());
    AURORA_TEST_REQUIRE(forwarded.ok());
    AURORA_TEST_CHECK(forwarded.value() == SingleInstanceOutcome::Forwarded);
    // 已转发不是首实例：调用方据此外退，不得再建窗口。
    AURORA_TEST_CHECK_FALSE(fixture.instance.is_primary());
    AURORA_TEST_CHECK_EQ(fixture.instance.instance_key(), resolve_instance_key(identity_primary()));

    fixture.instance.clear_all();
    fixture.fake.outcome = SingleInstanceOutcome::FallbackNewInstance;
    auto fallback = fixture.instance.acquire(identity_primary());
    AURORA_TEST_REQUIRE(fallback.ok());
    AURORA_TEST_CHECK(fallback.value() == SingleInstanceOutcome::FallbackNewInstance);
    AURORA_TEST_CHECK_FALSE(fixture.instance.is_primary());
}

AURORA_TEST_CASE(backend_failure_leaves_the_guard_unacquired) {
    Fixture fixture;

    fixture.fake.acquire_fails = true;
    auto failed = fixture.instance.acquire(identity_primary());
    AURORA_TEST_REQUIRE(!failed.ok());
    AURORA_TEST_CHECK_EQ(failed.error().code_enum, ErrorCode::GeneralInvalidArgument);
    AURORA_TEST_CHECK_EQ(failed.error().message, fixture.fake.failure_message);
    AURORA_TEST_CHECK_FALSE(fixture.instance.is_primary());
    AURORA_TEST_CHECK(fixture.instance.instance_key().empty());

    // 故障不粘滞：调用方可重试，成功后才进入已获取状态。
    fixture.fake.acquire_fails = false;
    auto retry = fixture.instance.acquire(identity_primary());
    AURORA_TEST_REQUIRE(retry.ok());
    AURORA_TEST_CHECK(retry.value() == SingleInstanceOutcome::Primary);
    AURORA_TEST_CHECK(fixture.instance.is_primary());
    AURORA_TEST_CHECK_EQ(fixture.fake.acquire_calls, 2);
}

AURORA_TEST_CASE(acquire_requires_an_executable_path) {
    Fixture fixture;

    AppIdentity empty;
    auto result = fixture.instance.acquire(empty);
    AURORA_TEST_REQUIRE(!result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::GeneralInvalidArgument);
    AURORA_TEST_CHECK_EQ(fixture.fake.acquire_calls, 0);
    AURORA_TEST_CHECK_FALSE(fixture.instance.is_primary());
}

AURORA_TEST_CASE(release_and_clear_all_reset_the_session) {
    Fixture fixture;

    auto identity = identity_primary();
    identity.arguments = {"myapp://open/1"};
    AURORA_TEST_REQUIRE(fixture.instance.acquire(identity).ok());
    AURORA_TEST_CHECK(fixture.instance.is_primary());

    fixture.instance.release();
    AURORA_TEST_CHECK_EQ(fixture.fake.release_calls, 1);
    AURORA_TEST_CHECK_FALSE(fixture.instance.is_primary());
    AURORA_TEST_CHECK(fixture.instance.instance_key().empty());

    // 已释放时再 clear_all 是幂等空操作：不得重复触碰平台桥（更不得触发按需自举）。
    fixture.instance.clear_all();
    AURORA_TEST_CHECK_EQ(fixture.fake.release_calls, 1);

    // clear_all 之后 acquire 重新走平台桥；持有时 clear_all 会释放真实会话。
    AURORA_TEST_REQUIRE(fixture.instance.acquire(identity_primary()).ok());
    AURORA_TEST_CHECK_EQ(fixture.fake.acquire_calls, 2);
    fixture.instance.clear_all();
    AURORA_TEST_CHECK_EQ(fixture.fake.release_calls, 2);
    AURORA_TEST_CHECK_FALSE(fixture.instance.is_primary());
}

AURORA_TEST_CASE(no_channel_reports_primary_without_claiming_forwarding) {
    Fixture fixture;
#if defined(AURORA_PLATFORM_WINDOWS) || defined(AURORA_PLATFORM_LINUX) || \
    (defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS))
    // 这三条腿有可按需自举的真桥：摘掉伪后端后公共入口会真的创建互斥量 / 锁文件，不适合无头断言。
    AURORA_TEST_SKIP("native single-instance channel boots on demand on this platform");
#else
    desktop_shell::unregister_single_instance_backend();
    auto outcome = fixture.instance.acquire(identity_primary());
    AURORA_TEST_REQUIRE(outcome.ok());
    // 无跨进程通道：如实按首实例起步，不谎报「已转发」。
    AURORA_TEST_CHECK(outcome.value() == SingleInstanceOutcome::Primary);
    AURORA_TEST_CHECK(fixture.instance.is_primary());
#endif
}

}  // namespace aurora::test_cases::utest_single_instance
