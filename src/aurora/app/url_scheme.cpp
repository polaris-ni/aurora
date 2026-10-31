#include "aurora/app/url_scheme.h"

#include <string>

#include "aurora/app/detail/shell_integration_backend.h"
#include "aurora/app/detail/shell_integration_translate.h"
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

/// @brief 取当前可用的 URL 方案平台桥（运行期判据收敛在一处）。
///
/// 判据是**通路是否在**而非能力位：能力位是编译期后端申报，通路登记才是运行期事实；两者分开
/// 让折算层可被无头单测注入（与 `recent_documents` 的同款判据一致）。
[[nodiscard]] auto active_backend() -> const desktop_shell::UrlSchemeBackend * {
    const auto *backend = desktop_shell::url_scheme_backend();
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
    return desktop_shell::url_scheme_backend();
}

/// @brief 文法与保留名单校验，成功返回规范化后的方案名。
[[nodiscard]] auto validate_scheme(std::string_view raw) -> Result<std::string> {
    switch (desktop_shell::check_url_scheme(raw)) {
        case desktop_shell::SchemeCheck::Valid:
            break;
        case desktop_shell::SchemeCheck::Empty:
            return make_error(ErrorCode::GeneralInvalidArgument, "url-scheme: scheme is empty");
        case desktop_shell::SchemeCheck::InvalidSyntax:
            return make_error(ErrorCode::GeneralInvalidArgument,
                              "url-scheme: '" + std::string(raw) +
                                  "' must match RFC 3986 (ALPHA *( ALPHA / DIGIT / '+' / '-' / '.' ))");
        case desktop_shell::SchemeCheck::Reserved:
            return make_error(ErrorCode::GeneralInvalidArgument,
                              "url-scheme: '" + std::string(raw) +
                                  "' is a reserved scheme owned by the existing ecosystem and cannot be claimed");
    }
    return desktop_shell::normalize_url_scheme(raw);
}

}  // namespace

auto register_url_scheme(std::string_view scheme, std::string_view display_name, const AppIdentity &identity)
    -> Result<void> {
    auto normalized = validate_scheme(scheme);
    if (!normalized) {
        return normalized.error();
    }
    if (identity.executable_path.empty()) {
        return make_error(ErrorCode::GeneralInvalidArgument,
                          "url-scheme: AppIdentity.executable_path is empty, the OS command template cannot be built");
    }
    const auto *backend = active_backend();
    if (backend == nullptr) {
        return make_error(ErrorCode::UrlSchemeUnsupported);
    }
    return backend->register_scheme(normalized.value(), display_name, identity);
}

auto unregister_url_scheme(std::string_view scheme, const AppIdentity &identity) -> Result<void> {
    auto normalized = validate_scheme(scheme);
    if (!normalized) {
        return normalized.error();
    }
    if (identity.executable_path.empty()) {
        return make_error(ErrorCode::GeneralInvalidArgument,
                          "url-scheme: AppIdentity.executable_path is empty, ownership cannot be verified");
    }
    const auto *backend = active_backend();
    if (backend == nullptr) {
        return make_error(ErrorCode::UrlSchemeUnsupported);
    }
    return backend->unregister_scheme(normalized.value(), identity);
}

auto registered_url_scheme_owner(std::string_view scheme) -> Result<std::string> {
    auto normalized = validate_scheme(scheme);
    if (!normalized) {
        return normalized.error();
    }
    const auto *backend = active_backend();
    if (backend == nullptr) {
        return make_error(ErrorCode::UrlSchemeUnsupported);
    }
    return backend->owner(normalized.value());
}

auto is_url_scheme_registered(std::string_view scheme) -> bool {
    const auto owner = registered_url_scheme_owner(scheme);
    return owner && !owner.value().empty();
}

}  // namespace aurora
