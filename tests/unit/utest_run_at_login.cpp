/// 测试类型: unit
/// 目标单元: include/aurora/app/run_at_login.h
/// 测试说明: 登录项三态语义与开关入口的契约——`Enabled` / `Disabled` / `Unsupported` 互不顶替
/// （`Unsupported` 必带 detail；`Enabled` 为空；`Disabled` 可带平台成因，伪后端下保持空并原样透传）、
/// 查询与写入都原样走平台桥、身份至少要有可执行路径或 bundle identifier、平台失败原样透传，
/// 以及无桥时如实报 `Unsupported`。
///
/// 为何必须是三态而非布尔：布尔会把「从未设置 / 用户手动禁用 / 本机无此机制 / 查询失败」并成同一个
/// `false`，应用分不清「调一下就能开」和「这台机器不可能」。本文件把这条差异钉成断言。
///
/// 为何单测能跑产品代码路径：登录项桥是进程级单槽（`desktop_shell::RunAtLoginBackend`），
/// 本文件注入伪后端；真机上平台桥登记同名后端。
///
/// 变异自证（改坏实现应转红的四条）：
///  * 把 `RunAtLoginStatus` 压成布尔（`Unsupported` 与 `Disabled` 合流）→ `tri_state_is_not_collapsed_to_bool` 转红；
///  * 让 `Unsupported` 的 detail 变空 → 同一用例转红；
///  * 写入时不校验身份（空身份也下发）→ `set_requires_an_identity` 转红；
///  * 把桥的 `run-at-login-failed` 改写为成功 → `set_forwards_the_requested_state` 转红。

#include <cstddef>
#include <string>
#include <utility>

#include "aurora/app/detail/shell_integration_backend.h"
#include "aurora/app/run_at_login.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_run_at_login {

namespace {

/// @brief 伪造登录项后端的观测状态与可编程结论。
struct FakeLoginItem {
    int query_calls = 0;  ///< 查询次数
    int set_calls = 0;  ///< 写入次数
    RunAtLoginState query_result{};  ///< 查询成功值（默认 Unsupported）
    bool last_enabled = false;  ///< 最近一次写入的开关值
    std::string last_path;  ///< 最近一次收到的可执行路径
    std::string last_bundle;  ///< 最近一次收到的 bundle identifier
    bool set_fails = false;  ///< 写入是否报 `run-at-login-failed`
    std::string failure_message{"run-at-login: backend refused"};  ///< 失败 detail
};

/// @brief 每个用例进出清空登录项通路的伪造后端。
class RunAtLoginBackendGuard {
  public:
    RunAtLoginBackendGuard() { desktop_shell::unregister_run_at_login_backend(); }
    ~RunAtLoginBackendGuard() { desktop_shell::unregister_run_at_login_backend(); }
    RunAtLoginBackendGuard(const RunAtLoginBackendGuard &) = delete;
    auto operator=(const RunAtLoginBackendGuard &) -> RunAtLoginBackendGuard & = delete;
    RunAtLoginBackendGuard(RunAtLoginBackendGuard &&) = delete;
    auto operator=(RunAtLoginBackendGuard &&) -> RunAtLoginBackendGuard & = delete;
};

/// @brief 安装伪造登录项桥。
void install_fake_backend(FakeLoginItem &state) {
    desktop_shell::RunAtLoginBackend backend;
    backend.query = [&state](const AppIdentity &identity) -> RunAtLoginState {
        ++state.query_calls;
        state.last_path = identity.executable_path;
        state.last_bundle = identity.bundle_identifier;
        return state.query_result;
    };
    backend.set = [&state](bool enabled, const AppIdentity &identity) -> Result<void> {
        ++state.set_calls;
        state.last_enabled = enabled;
        state.last_path = identity.executable_path;
        state.last_bundle = identity.bundle_identifier;
        if (state.set_fails) {
            return make_error(ErrorCode::RunAtLoginFailed, state.failure_message);
        }
        return Result<void>{};
    };
    desktop_shell::register_run_at_login_backend(std::move(backend));
}

/// @brief 带可执行路径的身份。
[[nodiscard]] auto identity_with_path() -> AppIdentity {
    AppIdentity identity;
    identity.executable_path = "/opt/aurora/bin/app";
    return identity;
}

}  // namespace

AURORA_TEST_CASE(tri_state_is_not_collapsed_to_bool) {
    RunAtLoginBackendGuard guard;
    FakeLoginItem state;
    install_fake_backend(state);

    // 三态各自原样返回。
    state.query_result = RunAtLoginState{.status = RunAtLoginStatus::Enabled, .detail = ""};
    auto enabled = run_at_login(identity_with_path());
    AURORA_TEST_CHECK(enabled.status == RunAtLoginStatus::Enabled);
    AURORA_TEST_CHECK(enabled.detail.empty());

    state.query_result = RunAtLoginState{.status = RunAtLoginStatus::Disabled, .detail = ""};
    auto disabled = run_at_login(identity_with_path());
    AURORA_TEST_CHECK(disabled.status == RunAtLoginStatus::Disabled);
    AURORA_TEST_CHECK(disabled.detail.empty());
    // Enabled 与 Disabled 是两个可达状态，不是同一枚布尔的两面。
    AURORA_TEST_CHECK(enabled.status != disabled.status);

    // Unsupported 另带原因：这正是布尔状态值丢掉的那一层信息。
    state.query_result = RunAtLoginState{.status = RunAtLoginStatus::Unsupported,
                                         .detail = "run-at-login: sandboxed macOS app has no login item service"};
    auto unsupported = run_at_login(identity_with_path());
    AURORA_TEST_CHECK(unsupported.status == RunAtLoginStatus::Unsupported);
    AURORA_TEST_CHECK_FALSE(unsupported.detail.empty());
    AURORA_TEST_CHECK(unsupported.status != disabled.status);
    AURORA_TEST_CHECK_EQ(state.query_calls, 3);
}

AURORA_TEST_CASE(query_passes_the_identity_through) {
    RunAtLoginBackendGuard guard;
    FakeLoginItem state;
    install_fake_backend(state);

    AppIdentity identity;
    identity.bundle_identifier = "com.example.app";
    identity.executable_path = "/Applications/App.app/Contents/MacOS/App";
    auto result = run_at_login(identity);
    AURORA_TEST_CHECK(result.status == RunAtLoginStatus::Unsupported);
    AURORA_TEST_CHECK_EQ(state.last_bundle, std::string{"com.example.app"});
    AURORA_TEST_CHECK_EQ(state.last_path, std::string{"/Applications/App.app/Contents/MacOS/App"});
}

AURORA_TEST_CASE(set_requires_an_identity) {
    RunAtLoginBackendGuard guard;
    FakeLoginItem state;
    install_fake_backend(state);

    AppIdentity empty;
    auto result = set_run_at_login(true, empty);
    AURORA_TEST_REQUIRE(!result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::GeneralInvalidArgument);
    AURORA_TEST_CHECK_EQ(state.set_calls, 0);

    // bundle identifier 单独即可（macOS 的登录项按 bundle 定位）。
    AppIdentity bundle_only;
    bundle_only.bundle_identifier = "com.example.app";
    AURORA_TEST_REQUIRE(set_run_at_login(false, bundle_only).ok());
    AURORA_TEST_CHECK_EQ(state.set_calls, 1);
    AURORA_TEST_CHECK_FALSE(state.last_enabled);
    AURORA_TEST_CHECK_EQ(state.last_bundle, std::string{"com.example.app"});
}

AURORA_TEST_CASE(set_forwards_the_requested_state) {
    RunAtLoginBackendGuard guard;
    FakeLoginItem state;
    install_fake_backend(state);

    AURORA_TEST_REQUIRE(set_run_at_login(true, identity_with_path()).ok());
    AURORA_TEST_CHECK(state.last_enabled);
    AURORA_TEST_REQUIRE(set_run_at_login(false, identity_with_path()).ok());
    AURORA_TEST_CHECK_FALSE(state.last_enabled);
    AURORA_TEST_CHECK_EQ(state.set_calls, 2);

    // 平台失败原样透传：不静默当成「已关」。
    state.set_fails = true;
    auto failed = set_run_at_login(true, identity_with_path());
    AURORA_TEST_REQUIRE(!failed.ok());
    AURORA_TEST_CHECK_EQ(failed.error().code_enum, ErrorCode::RunAtLoginFailed);
    AURORA_TEST_CHECK_EQ(failed.error().message, state.failure_message);
}

AURORA_TEST_CASE(degradation_reports_unsupported_without_a_wired_bridge) {
    RunAtLoginBackendGuard guard;
#if defined(AURORA_PLATFORM_WINDOWS) || defined(AURORA_PLATFORM_LINUX) || \
    (defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS))
    // 这三条腿有可按需自举的真桥：清空槽位后公共入口会去碰真实注册表 / 自动启动目录，不适合无头断言。
    AURORA_TEST_SKIP("native login-item bridge boots on demand on this platform");
#else
    auto state = run_at_login(identity_with_path());
    AURORA_TEST_CHECK(state.status == RunAtLoginStatus::Unsupported);
    AURORA_TEST_CHECK_FALSE(state.detail.empty());

    auto result = set_run_at_login(true, identity_with_path());
    AURORA_TEST_REQUIRE(!result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::RunAtLoginUnsupported);
#endif
}

}  // namespace aurora::test_cases::utest_run_at_login
