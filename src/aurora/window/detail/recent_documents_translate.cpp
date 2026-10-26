#include "aurora/window/detail/recent_documents_translate.h"

#include <algorithm>
#include <utility>

namespace aurora::recent_documents {

namespace {

/// @brief 进程级单槽的存取入口（**值持有**）。
///
/// 值持有而非裸指针：桥若把栈上对象登记进来，析构后槽里就是悬垂引用。放在函数局部静态而非命名空间级
/// 变量，与 `drag_payload.cpp` 的 `drag_out_backend_slot()`、`win32_recent_documents.cpp` 的 `state()`
/// 同纪律（免全局可变量、规避静态初始化顺序问题）。单线程 UI 契约下不加锁。
///
/// 「是否已登记」用 `project` 回调判空自证，不套 `optional`：槽每次经 `slot()` 取回的是引用表达式，
/// `slot().has_value() ? &*slot() : nullptr` 里那次解引用会被 `bugprone-unchecked-optional-access` 判成
/// unchecked access（分析器认不出它与同一行的 `has_value()` 是同一个对象），而值持有 + 哨兵字段既无
/// 解引用、也不引入「槽有位又有值」的第二真源。
auto slot() -> ProjectionBackend & {
    static ProjectionBackend backend;  // 单线程 UI：无锁（与 RecentDocuments 单例同款纪律）
    return backend;
}

}  // namespace

auto build_projection_plan(const std::vector<std::string> &documents, std::size_t max_entries)
    -> std::vector<std::string> {
    std::vector<std::string> plan;
    plan.reserve(std::min(documents.size(), max_entries));
    for (const auto &path : documents) {
        if (plan.size() >= max_entries) {
            break;  // 截断：入参是新 → 旧序，走到这里的是尾部最旧条目，先丢
        }
        if (path.empty()) {
            continue;  // 空串不是路径：丢弃（与「相对路径照进」是两条不同处置）
        }
        // 去重保持首次出现序：计划是新 → 旧投递序，后出现的同一路径不再重复投递。
        if (std::ranges::find(plan, path) != plan.end()) {
            continue;
        }
        plan.emplace_back(path);
    }
    return plan;
}

auto register_projection_backend(ProjectionBackend backend) -> void { slot() = std::move(backend); }

auto unregister_projection_backend() -> void { slot() = ProjectionBackend{}; }

auto projection_backend() -> const ProjectionBackend * {
    const auto &backend = slot();
    return backend.project != nullptr ? &backend : nullptr;
}

}  // namespace aurora::recent_documents
