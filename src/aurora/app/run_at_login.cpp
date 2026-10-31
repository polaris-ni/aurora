#include "aurora/app/run_at_login.h"

#include <string>

#include "aurora/app/detail/shell_integration_backend.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/platform.h"
#include "aurora/core/result.h"

#ifdef AURORA_PLATFORM_WINDOWS
#include "aurora/app/detail/win32_shell_integration.h"
#elif defined(AURORA_PLATFORM_LINUX)
#include "aurora/app/detail/linux_shell_integration.h"
#elif defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)
#include "aurora/app/detail/macos_shell_integration.h"
#endif

namespace aurora {

namespace {

/// @brief 取当前可用的登录项平台桥（判据同 URL 方案：通路登记即运行期事实）。
[[nodiscard]] auto active_backend() -> const desktop_shell::RunAtLoginBackend * {
    const auto *backend = desktop_shell::run_at_login_backend();
    if (backend != nullptr) {
        return backend;
    }
#ifdef AURORA_PLATFORM_WINDOWS
    (void)detail::win32_shell_integration_ensure();
#elif defined(AURORA_PLATFORM_LINUX)
    (void)detail::linux_shell_integration_ensure();
#elif defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)
    (void)detail::macos_shell_integration_ensure();
#endif
    return desktop_shell::run_at_login_backend();
}

}  // namespace

auto run_at_login(const AppIdentity &identity) -> RunAtLoginState {
    const auto *backend = active_backend();
    if (backend == nullptr) {
        return RunAtLoginState{.status = RunAtLoginStatus::Unsupported,
                               .detail = "run-at-login: no login-item mechanism is wired for this platform"};
    }
    return backend->query(identity);
}

auto set_run_at_login(bool enabled, const AppIdentity &identity) -> Result<void> {
    if (identity.executable_path.empty() && identity.bundle_identifier.empty()) {
        return make_error(ErrorCode::GeneralInvalidArgument,
                          "run-at-login: AppIdentity needs an executable path or a bundle identifier");
    }
    const auto *backend = active_backend();
    if (backend == nullptr) {
        return make_error(ErrorCode::RunAtLoginUnsupported);
    }
    return backend->set(enabled, identity);
}

}  // namespace aurora
