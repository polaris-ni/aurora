#include "aurora/app/single_instance.h"

#include <string>
#include <utility>
#include <vector>

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

/// @brief 取当前可用的单实例平台桥；未接线返回 nullptr（公共入口如实按首实例起步）。
[[nodiscard]] auto active_backend() -> const desktop_shell::SingleInstanceBackend * {
    const auto *backend = desktop_shell::single_instance_backend();
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
    return desktop_shell::single_instance_backend();
}

}  // namespace

auto derive_instance_key(std::string_view executable_path) -> std::string {
    return desktop_shell::derive_instance_key(executable_path);
}

auto resolve_instance_key(const AppIdentity &identity) -> std::string {
    if (!identity.instance_key.empty()) {
        return identity.instance_key;
    }
    return derive_instance_key(identity.executable_path);
}

auto SingleInstance::instance() -> SingleInstance & {
    static SingleInstance guard;
    return guard;
}

SingleInstance::~SingleInstance() { release(); }

auto SingleInstance::acquire(const AppIdentity &identity) -> Result<SingleInstanceOutcome> {
    if (acquired_) {
        return outcome_;  // 幂等：重复调用返回上次结果
    }
    if (identity.executable_path.empty()) {
        return make_error(ErrorCode::GeneralInvalidArgument,
                          "single-instance: AppIdentity.executable_path is empty, the instance key cannot be derived");
    }
    const auto key = resolve_instance_key(identity);
    std::vector<std::string> uris;
    if (const auto uri = desktop_shell::extract_uri_argument(identity.arguments); uri.has_value()) {
        uris.push_back(*uri);
    }

    const auto *backend = active_backend();
    if (backend == nullptr) {
        // 无跨进程通道（Headless / Wasm / 未接线后端）：如实按首实例起步，不谎报「已转发」。
        acquired_ = true;
        primary_ = true;
        outcome_ = SingleInstanceOutcome::Primary;
        key_ = key;
        pending_uris_ = std::move(uris);
        return outcome_;
    }

    auto outcome =
        backend->acquire(identity, uris, [this](std::string uri) { pending_uris_.push_back(std::move(uri)); });
    if (!outcome) {
        return outcome.error();  // 通道故障：不改状态，调用方可重试或降级
    }
    acquired_ = true;
    key_ = key;
    outcome_ = outcome.value();
    primary_ = outcome_ == SingleInstanceOutcome::Primary;
    if (primary_) {
        // 首实例：命令行的 URI 入队（平台桥不把入参再经 sink 回推，避免重复）。
        for (auto &uri : uris) {
            pending_uris_.push_back(std::move(uri));
        }
    }
    return outcome_;
}

auto SingleInstance::set_on_uri_received(std::function<void(std::string_view)> callback) -> void {
    on_uri_ = std::move(callback);
    // 回调登记前已排队的 URI 不丢：登记即交付（按到达序）。
    while (!pending_uris_.empty() && on_uri_) {
        const std::string uri = std::move(pending_uris_.front());
        pending_uris_.erase(pending_uris_.begin());
        on_uri_(uri);
    }
}

auto SingleInstance::poll() -> void {
    if (const auto *backend = active_backend(); backend != nullptr && backend->poll) {
        backend->poll();
    }
    while (!pending_uris_.empty() && on_uri_) {
        const std::string uri = std::move(pending_uris_.front());
        pending_uris_.erase(pending_uris_.begin());
        on_uri_(uri);
    }
}

auto SingleInstance::release() -> void {
    // 未持有时是空操作：既不该触碰平台桥（可能触发按需自举），也不该让「释放」次数虚增。
    if (acquired_) {
        if (const auto *backend = active_backend(); backend != nullptr && backend->release) {
            backend->release();
        }
    }
    acquired_ = false;
    primary_ = false;
    outcome_ = SingleInstanceOutcome::FallbackNewInstance;
    key_.clear();
    pending_uris_.clear();
}

auto SingleInstance::is_primary() const -> bool { return acquired_ && primary_; }

auto SingleInstance::instance_key() const -> std::string { return key_; }

auto SingleInstance::clear_all() -> void {
    release();
    on_uri_ = nullptr;
}

}  // namespace aurora
