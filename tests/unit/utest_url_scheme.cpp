/// 测试类型: unit
/// 目标单元: include/aurora/app/url_scheme.h
/// 测试说明: URL 方案登记的公共入口契约——文法（RFC 3986）与保留名单在**碰 OS 之前**拒收、
/// 缺失可执行路径的拒收、方案名规范化后交给平台桥、归属查询与 `is_url_scheme_registered` 的真值口径
/// （查询失败按「不能声称已登记」处理）、撤销时对「归属是别人」的原样透传，以及无桥时的
/// `url-scheme-unsupported` 退化。
///
/// 为何单测能跑产品代码路径：平台桥是进程级单槽（`desktop_shell::UrlSchemeBackend`），本文件注入
/// 伪后端，公共入口 `register_url_scheme` / `unregister_url_scheme` / `registered_url_scheme_owner`
/// 走的是同一条代码；真机上平台桥登记同名后端，两侧不改公共层一行。
///
/// 本文件**不改真实注册表 / 桌面条目**：伪后端一经登记即被采用，按需自举的真桥不会启动。
///
/// 变异自证（改坏实现应转红的五条）：
///  * 从保留名单删掉 `mailto` → `grammar_and_reserved_schemes_are_rejected_before_any_os_work` 转红；
///  * 把文法判定挪到平台桥之后（先写 OS 再校验）→ 同一用例的「桥未被调用」断言转红；
///  * 撤销时不透传桥的 `url-scheme-failed` 而改写为成功 → `unregister_propagates_ownership_conflicts` 转红；
///  * 让 `is_url_scheme_registered` 在查询失败时返回 true → `owner_lookup_and_is_registered` 转红；
///  * 无桥时返回成功而非 `url-scheme-unsupported` → `degradation_reports_unsupported_without_a_wired_bridge` 转红。

#include <cstddef>
#include <string>
#include <utility>

#include "aurora/app/detail/shell_integration_backend.h"
#include "aurora/app/url_scheme.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_url_scheme {

namespace {

/// @brief 伪造 URL 方案后端的观测状态与可编程结论。
struct FakeUrlScheme {
    int register_calls = 0;  ///< 登记次数
    int unregister_calls = 0;  ///< 撤销次数
    int owner_calls = 0;  ///< 归属查询次数
    std::string last_scheme;  ///< 最近一次收到的方案名（已规范化）
    std::string last_name;  ///< 最近一次收到的显示名
    std::string last_path;  ///< 最近一次收到的可执行路径
    bool register_fails = false;  ///< 登记是否报 `url-scheme-failed`
    bool unregister_fails = false;  ///< 撤销是否报 `url-scheme-failed`
    bool owner_fails = false;  ///< 归属查询是否报 `url-scheme-failed`
    std::string owner_value;  ///< 归属查询的成功值（空串 = 未登记）
    std::string failure_message{"url-scheme: backend refused"};  ///< 失败 detail
};

/// @brief 每个用例进出清空 URL 方案通路的伪造后端。
class UrlSchemeBackendGuard {
  public:
    UrlSchemeBackendGuard() { desktop_shell::unregister_url_scheme_backend(); }
    ~UrlSchemeBackendGuard() { desktop_shell::unregister_url_scheme_backend(); }
    UrlSchemeBackendGuard(const UrlSchemeBackendGuard &) = delete;
    auto operator=(const UrlSchemeBackendGuard &) -> UrlSchemeBackendGuard & = delete;
    UrlSchemeBackendGuard(UrlSchemeBackendGuard &&) = delete;
    auto operator=(UrlSchemeBackendGuard &&) -> UrlSchemeBackendGuard & = delete;
};

/// @brief 安装伪造 URL 方案桥。
void install_fake_backend(FakeUrlScheme &state) {
    desktop_shell::UrlSchemeBackend backend;
    backend.register_scheme = [&state](std::string_view scheme, std::string_view display_name,
                                       const AppIdentity &identity) -> Result<void> {
        ++state.register_calls;
        state.last_scheme = std::string(scheme);
        state.last_name = std::string(display_name);
        state.last_path = identity.executable_path;
        if (state.register_fails) {
            return make_error(ErrorCode::UrlSchemeFailed, state.failure_message);
        }
        return Result<void>{};
    };
    backend.unregister_scheme = [&state](std::string_view scheme, const AppIdentity &identity) -> Result<void> {
        ++state.unregister_calls;
        state.last_scheme = std::string(scheme);
        state.last_path = identity.executable_path;
        if (state.unregister_fails) {
            return make_error(ErrorCode::UrlSchemeFailed, state.failure_message);
        }
        return Result<void>{};
    };
    backend.owner = [&state](std::string_view scheme) -> Result<std::string> {
        ++state.owner_calls;
        state.last_scheme = std::string(scheme);
        if (state.owner_fails) {
            return make_error(ErrorCode::UrlSchemeFailed, state.failure_message);
        }
        return state.owner_value;
    };
    desktop_shell::register_url_scheme_backend(std::move(backend));
}

/// @brief 带可执行路径的身份（命令模板可拼装）。
[[nodiscard]] auto identity_with_path() -> AppIdentity {
    AppIdentity identity;
    identity.executable_path = "/opt/aurora/bin/app";
    return identity;
}

}  // namespace

AURORA_TEST_CASE(grammar_and_reserved_schemes_are_rejected_before_any_os_work) {
    UrlSchemeBackendGuard guard;
    FakeUrlScheme state;
    install_fake_backend(state);

    const auto identity = identity_with_path();
    const auto expect_invalid = [&identity](std::string_view scheme) {
        auto result = register_url_scheme(scheme, "App", identity);
        AURORA_TEST_REQUIRE(!result.ok());
        AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::GeneralInvalidArgument);
    };

    expect_invalid("");
    expect_invalid("1abc");
    expect_invalid("a_b");
    expect_invalid("a:b");
    expect_invalid("a b");
    expect_invalid("-abc");
    expect_invalid("abc/def");

    // 保留方案单独成类：报文点名「reserved」，不是平台不支持，也不是文法错误。
    auto reserved = register_url_scheme("mailto", "App", identity);
    AURORA_TEST_REQUIRE(!reserved.ok());
    AURORA_TEST_CHECK_EQ(reserved.error().code_enum, ErrorCode::GeneralInvalidArgument);
    AURORA_TEST_CHECK(reserved.error().message.find("reserved") != std::string::npos);
    // 大小写不敏感：`HTTPS` 同样属既有生态。
    auto upper_reserved = register_url_scheme("HTTPS", "App", identity);
    AURORA_TEST_REQUIRE(!upper_reserved.ok());
    AURORA_TEST_CHECK_EQ(upper_reserved.error().code_enum, ErrorCode::GeneralInvalidArgument);

    // 文法/保留名单的判定必须在平台桥之前：一次都不该落到 OS 侧。
    AURORA_TEST_CHECK_EQ(state.register_calls, 0);
}

AURORA_TEST_CASE(registration_requires_an_executable_path) {
    UrlSchemeBackendGuard guard;
    FakeUrlScheme state;
    install_fake_backend(state);

    AppIdentity no_path;
    auto result = register_url_scheme("myapp", "My App", no_path);
    AURORA_TEST_REQUIRE(!result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::GeneralInvalidArgument);
    AURORA_TEST_CHECK_EQ(state.register_calls, 0);

    auto unregister = unregister_url_scheme("myapp", no_path);
    AURORA_TEST_REQUIRE(!unregister.ok());
    AURORA_TEST_CHECK_EQ(unregister.error().code_enum, ErrorCode::GeneralInvalidArgument);
    AURORA_TEST_CHECK_EQ(state.unregister_calls, 0);
}

AURORA_TEST_CASE(registration_normalizes_and_forwards_to_the_backend) {
    UrlSchemeBackendGuard guard;
    FakeUrlScheme state;
    install_fake_backend(state);

    auto result = register_url_scheme("MyApp-Debug+1", "My App", identity_with_path());
    AURORA_TEST_REQUIRE(result.ok());
    AURORA_TEST_CHECK_EQ(state.register_calls, 1);
    AURORA_TEST_CHECK_EQ(state.last_scheme, std::string{"myapp-debug+1"});
    AURORA_TEST_CHECK_EQ(state.last_name, std::string{"My App"});
    AURORA_TEST_CHECK_EQ(state.last_path, std::string{"/opt/aurora/bin/app"});

    // 平台失败原样透传，不被公共层改写成成功。
    state.register_fails = true;
    auto failed = register_url_scheme("myapp", "My App", identity_with_path());
    AURORA_TEST_REQUIRE(!failed.ok());
    AURORA_TEST_CHECK_EQ(failed.error().code_enum, ErrorCode::UrlSchemeFailed);
    AURORA_TEST_CHECK_EQ(failed.error().message, state.failure_message);
    AURORA_TEST_CHECK_EQ(state.register_calls, 2);
}

AURORA_TEST_CASE(owner_lookup_and_is_registered) {
    UrlSchemeBackendGuard guard;
    FakeUrlScheme state;
    install_fake_backend(state);

    state.owner_value = "com.example.app";
    auto owner = registered_url_scheme_owner("MYAPP");
    AURORA_TEST_REQUIRE(owner.ok());
    AURORA_TEST_CHECK_EQ(owner.value(), std::string{"com.example.app"});
    AURORA_TEST_CHECK_EQ(state.last_scheme, std::string{"myapp"});
    AURORA_TEST_CHECK(is_url_scheme_registered("myapp"));

    state.owner_value.clear();
    owner = registered_url_scheme_owner("myapp");
    AURORA_TEST_REQUIRE(owner.ok());
    AURORA_TEST_CHECK(owner.value().empty());
    AURORA_TEST_CHECK_FALSE(is_url_scheme_registered("myapp"));

    // 查询失败：owner 报错原样透传，is_registered 按「不能声称已登记」收为 false。
    state.owner_fails = true;
    const auto broken = registered_url_scheme_owner("myapp");
    AURORA_TEST_REQUIRE(!broken.ok());
    AURORA_TEST_CHECK_EQ(broken.error().code_enum, ErrorCode::UrlSchemeFailed);
    AURORA_TEST_CHECK_FALSE(is_url_scheme_registered("myapp"));

    // 非法方案名在碰桥之前就被拒；查询类入口同样适用。
    const auto invalid = registered_url_scheme_owner("http");
    AURORA_TEST_REQUIRE(!invalid.ok());
    AURORA_TEST_CHECK_EQ(invalid.error().code_enum, ErrorCode::GeneralInvalidArgument);
    AURORA_TEST_CHECK_FALSE(is_url_scheme_registered(""));
}

AURORA_TEST_CASE(unregister_propagates_ownership_conflicts) {
    UrlSchemeBackendGuard guard;
    FakeUrlScheme state;
    install_fake_backend(state);

    // 归属是别人：桥拒改，公共层不得把它改写成成功（撤销越过别人的登记属越界动作）。
    state.unregister_fails = true;
    state.failure_message = "url-scheme: 'myapp' is owned by com.other.app";
    auto result = unregister_url_scheme("MyApp", identity_with_path());
    AURORA_TEST_REQUIRE(!result.ok());
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::UrlSchemeFailed);
    AURORA_TEST_CHECK(result.error().message.find("com.other.app") != std::string::npos);
    AURORA_TEST_CHECK_EQ(state.unregister_calls, 1);
    AURORA_TEST_CHECK_EQ(state.last_scheme, std::string{"myapp"});

    // 归属是自己：成功，且同样先做规范化。
    state.unregister_fails = false;
    AURORA_TEST_REQUIRE(unregister_url_scheme("MYAPP", identity_with_path()).ok());
    AURORA_TEST_CHECK_EQ(state.unregister_calls, 2);
    AURORA_TEST_CHECK_EQ(state.last_scheme, std::string{"myapp"});
}

AURORA_TEST_CASE(degradation_reports_unsupported_without_a_wired_bridge) {
    UrlSchemeBackendGuard guard;
#if defined(AURORA_PLATFORM_WINDOWS) || defined(AURORA_PLATFORM_LINUX) || \
    (defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS))
    // 这三条腿有可按需自举的真桥：清空槽位后公共入口会去碰真实注册表 / 桌面条目，不适合无头断言。
    AURORA_TEST_SKIP("native URL scheme bridge boots on demand on this platform");
#else
    const auto identity = identity_with_path();
    auto register_result = register_url_scheme("myapp", "My App", identity);
    AURORA_TEST_REQUIRE(!register_result.ok());
    AURORA_TEST_CHECK_EQ(register_result.error().code_enum, ErrorCode::UrlSchemeUnsupported);

    auto unregister_result = unregister_url_scheme("myapp", identity);
    AURORA_TEST_REQUIRE(!unregister_result.ok());
    AURORA_TEST_CHECK_EQ(unregister_result.error().code_enum, ErrorCode::UrlSchemeUnsupported);

    auto owner = registered_url_scheme_owner("myapp");
    AURORA_TEST_REQUIRE(!owner.ok());
    AURORA_TEST_CHECK_EQ(owner.error().code_enum, ErrorCode::UrlSchemeUnsupported);
    AURORA_TEST_CHECK_FALSE(is_url_scheme_registered("myapp"));
#endif
}

}  // namespace aurora::test_cases::utest_url_scheme
