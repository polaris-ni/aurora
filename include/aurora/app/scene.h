#pragma once

#include <string>

#include "aurora/render/offscreen.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 场景：持有一棵 widget 树，提供无头渲染与结构快照。
 *
 * 对应 specification/06-app-platform.md §2.3 / ARCHITECTURE.md §4.6：`Scene` 只持 widget 树与根环境；`Window` 归 `Application`。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
class Scene {
  public:
    explicit Scene(Node root) : m_root(std::move(root)) {}

    [[nodiscard]] auto root() -> Widget & { return m_root.widget(); }

    /// @brief 返回根节点（供事件派发读取根命中矩形；几何权威在 Node）。
    [[nodiscard]] auto root_node() -> Node & { return m_root; }

    /// @brief 无头渲染为 PNG（specification/03-layout-render.md §8.4）。
    [[nodiscard]] auto render_to_png(const char *path, int width, int height) -> Result<bool> {
        return aurora::render_to_png(m_root, width, height, path);
    }

    /// @brief 结构快照（JSON）：用于 golden 比对（specification/06-app-platform.md §12.2，跨平台稳定）。
    [[nodiscard]] auto serialize() const -> std::string;

  private:
    static auto serialize_widget(const Widget &w, std::string &out) -> void;

    Node m_root;
};

} // namespace aurora
