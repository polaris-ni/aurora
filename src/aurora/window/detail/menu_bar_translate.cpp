// menu_bar_translate.cpp — 原生菜单栏折算层（树 → 平台无关扁平项表 + 进程级渲染通路单槽）。
//
// 折算规则与字段保真见 `menu_bar_translate.h` 文件头；本文件只做落地，不另立口径。
#include "aurora/window/detail/menu_bar_translate.h"

#include <optional>
#include <utility>

namespace aurora::menu_bar_translate {
namespace {

/// @brief 进程级单槽的存取入口。
///
/// 用 `optional` 值持有而非裸指针，避免桥把栈上对象登记进来后析构造成悬垂；放在函数局部静态而非
/// 命名空间级变量，与 `recent_documents_translate.cpp` 的 `slot()` 同纪律（避免全局可变量）。
/// 单线程 UI 契约下不加锁。
auto slot() -> std::optional<MenuBarBackend> & {
    static std::optional<MenuBarBackend> backend;  // 单线程 UI：无锁（与 NativeMenuBar 单例同款纪律）
    return backend;
}

/// @brief 递归摊平（前序遍历）。
/// @param model 当前模型节点。
/// @param depth 当前深度（顶层 0）。
/// @param parent_index 父节点在输出表中的下标（`AURORA_MENU_BAR_NO_PARENT` 表示顶层）。
/// @param out 输出表（按值推入，父先于子）。
auto flatten_recursive(const MenuModel &model, std::size_t depth, std::size_t parent_index,
                       std::vector<FlatMenuItem> &out) -> void {
    const std::size_t index = out.size();
    out.push_back(FlatMenuItem{
        .depth = depth,
        .parent_index = parent_index,
        .separator = model.separator,
        .checkable = model.checkable,
        .checked = model.checked,
        .enabled = model.enabled,
        .label = model.label,
        .shortcut_text = model.shortcut_text,
        .id = model.id,
    });
    if (!model.children.empty()) {
        for (const auto &child : model.children) {
            flatten_recursive(child, depth + 1, index, out);
        }
    }
}

}  // namespace

auto flatten_menu_models(const std::vector<MenuModel> &models) -> std::vector<FlatMenuItem> {
    std::vector<FlatMenuItem> out;
    // 预留一点容量：树摊平后项数通常多于顶层项数。
    out.reserve((models.size() * 2U) + 1U);
    for (const auto &model : models) {
        flatten_recursive(model, 0U, AURORA_MENU_BAR_NO_PARENT, out);
    }
    return out;
}

auto register_menu_bar_backend(MenuBarBackend backend) -> void { slot().emplace(std::move(backend)); }

auto unregister_menu_bar_backend() -> void { slot().reset(); }

auto menu_bar_backend() -> const MenuBarBackend * { return slot().has_value() ? &*slot() : nullptr; }

}  // namespace aurora::menu_bar_translate
