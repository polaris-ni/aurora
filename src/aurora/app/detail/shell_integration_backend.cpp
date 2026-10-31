#include "aurora/app/detail/shell_integration_backend.h"

#include <utility>

namespace aurora::desktop_shell {

namespace {

/// @brief 进程级单槽（值持有；单线程 UI 不加锁，与 `recent_documents::projection_backend` 同纪律）。
template <typename Backend>
auto slot() -> Backend & {
    static Backend backend;
    return backend;
}

}  // namespace

auto register_url_scheme_backend(UrlSchemeBackend backend) -> void { slot<UrlSchemeBackend>() = std::move(backend); }

auto unregister_url_scheme_backend() -> void { slot<UrlSchemeBackend>() = UrlSchemeBackend{}; }

auto url_scheme_backend() -> const UrlSchemeBackend * {
    const auto &backend = slot<UrlSchemeBackend>();
    return (backend.register_scheme && backend.unregister_scheme && backend.owner) ? &backend : nullptr;
}

auto register_file_association_backend(FileAssociationBackend backend) -> void {
    slot<FileAssociationBackend>() = std::move(backend);
}

auto unregister_file_association_backend() -> void { slot<FileAssociationBackend>() = FileAssociationBackend{}; }

auto file_association_backend() -> const FileAssociationBackend * {
    const auto &backend = slot<FileAssociationBackend>();
    return (backend.query && backend.declare && backend.undeclare) ? &backend : nullptr;
}

auto register_run_at_login_backend(RunAtLoginBackend backend) -> void {
    slot<RunAtLoginBackend>() = std::move(backend);
}

auto unregister_run_at_login_backend() -> void { slot<RunAtLoginBackend>() = RunAtLoginBackend{}; }

auto run_at_login_backend() -> const RunAtLoginBackend * {
    const auto &backend = slot<RunAtLoginBackend>();
    return (backend.query && backend.set) ? &backend : nullptr;
}

auto register_print_backend(PrintBackend backend) -> void { slot<PrintBackend>() = std::move(backend); }

auto unregister_print_backend() -> void { slot<PrintBackend>() = PrintBackend{}; }

auto print_backend() -> const PrintBackend * {
    const auto &backend = slot<PrintBackend>();
    return (backend.targets && backend.open) ? &backend : nullptr;
}

auto register_single_instance_backend(SingleInstanceBackend backend) -> void {
    slot<SingleInstanceBackend>() = std::move(backend);
}

auto unregister_single_instance_backend() -> void { slot<SingleInstanceBackend>() = SingleInstanceBackend{}; }

auto single_instance_backend() -> const SingleInstanceBackend * {
    const auto &backend = slot<SingleInstanceBackend>();
    return (backend.acquire != nullptr) ? &backend : nullptr;
}

}  // namespace aurora::desktop_shell
