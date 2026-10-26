#include "aurora/window/detail/recent_documents_translate.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace aurora::recent_documents {

namespace {

/// @brief 进程级单槽的存取入口。
///
/// 用 `optional` 值持有而非裸指针，避免桥把栈上对象登记进来后析构造成悬垂；放在函数局部静态而非
/// 命名空间级变量，与 `win32_recent_documents.cpp` 的 `state()` 同纪律（避免全局可变量）。
/// 单线程 UI 契约下不加锁。
auto slot() -> std::optional<ProjectionBackend> & {
    static std::optional<ProjectionBackend> backend;  // 单线程 UI：无锁（与 RecentDocuments 单例同款纪律）
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

auto register_projection_backend(ProjectionBackend backend) -> void { slot().emplace(std::move(backend)); }

auto unregister_projection_backend() -> void { slot().reset(); }

auto projection_backend() -> const ProjectionBackend * { return slot().has_value() ? &*slot() : nullptr; }

}  // namespace aurora::recent_documents
