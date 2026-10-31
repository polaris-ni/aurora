#include "aurora/app/file_association.h"

#include <string>

#include "aurora/app/detail/shell_integration_backend.h"
#include "aurora/app/detail/shell_integration_translate.h"
#include "aurora/app/mime_types.h"
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

/// @brief 取当前可用的文件关联平台桥（判据同 URL 方案：通路登记即运行期事实）。
[[nodiscard]] auto active_backend() -> const desktop_shell::FileAssociationBackend * {
    const auto *backend = desktop_shell::file_association_backend();
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
    return desktop_shell::file_association_backend();
}

/// @brief 扩展名规范化；非法输入落 `general-invalid-argument` 并点名原值。
[[nodiscard]] auto validate_extension(std::string_view raw) -> Result<std::string> {
    auto normalized = desktop_shell::normalize_extension(raw);
    if (!normalized.has_value()) {
        return make_error(ErrorCode::GeneralInvalidArgument,
                          "file-association: '" + std::string(raw) +
                              "' is not a valid extension (expect a non-empty name without dots or separators)");
    }
    return *normalized;
}

}  // namespace

auto associated_application(std::string_view extension) -> Result<AssociationInfo> {
    auto normalized = validate_extension(extension);
    if (!normalized) {
        return normalized.error();
    }
    const auto *backend = active_backend();
    if (backend == nullptr) {
        return make_error(ErrorCode::FileAssociationUnsupported);
    }
    return backend->query(normalized.value());
}

auto register_file_association(const AssociationDeclaration &declaration, const AppIdentity &identity) -> Result<void> {
    auto normalized = validate_extension(declaration.extension);
    if (!normalized) {
        return normalized.error();
    }
    // MIME 解析优先级：调用方显式值 → `mime_types.h` 的扩展名映射；两者都缺即无法声明。
    std::string mime = declaration.mime_type;
    if (mime.empty()) {
        const auto mapped = mime_type_for_extension(normalized.value());
        if (!mapped.has_value()) {
            return make_error(ErrorCode::GeneralInvalidArgument,
                              "file-association: no MIME type for extension '" + normalized.value() +
                                  "'; provide AssociationDeclaration.mime_type explicitly");
        }
        mime = std::string(*mapped);
    }
    if (identity.executable_path.empty()) {
        return make_error(
            ErrorCode::GeneralInvalidArgument,
            "file-association: AppIdentity.executable_path is empty, the OS command template cannot be built");
    }
    const auto *backend = active_backend();
    if (backend == nullptr) {
        return make_error(ErrorCode::FileAssociationUnsupported);
    }
    AssociationDeclaration resolved = declaration;
    resolved.extension = normalized.value();
    resolved.mime_type = mime;
    return backend->declare(resolved, mime, identity);
}

auto unregister_file_association(std::string_view extension, const AppIdentity &identity) -> Result<void> {
    auto normalized = validate_extension(extension);
    if (!normalized) {
        return normalized.error();
    }
    if (identity.executable_path.empty()) {
        return make_error(ErrorCode::GeneralInvalidArgument,
                          "file-association: AppIdentity.executable_path is empty, ownership cannot be verified");
    }
    const auto *backend = active_backend();
    if (backend == nullptr) {
        return make_error(ErrorCode::FileAssociationUnsupported);
    }
    return backend->undeclare(normalized.value(), identity);
}

}  // namespace aurora
